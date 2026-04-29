/*
 * Fault Injection Plugin
 *
 * Simulates radiation-induced bit flips in data cache, instruction cache,
 * L2 cache, and main memory. On each memory or instruction access, a random
 * bit flip may be injected at an address belonging to the configured cache
 * level or memory, with independently configurable probabilities.
 *
 * Requires the "cache" plugin to be loaded first. At init, this plugin
 * resolves cache_get_l1d_addr, cache_get_l1i_addr, cache_get_l2_addr, and
 * cache_get_mem_addr via dlsym.
 *
 * Parameters:
 *   l1d_flip_chance=N   - chance (1 in N) of flipping a bit in L1 data cache
 *   l1i_flip_chance=N   - chance (1 in N) of flipping a bit in L1 insn cache
 *   l2_flip_chance=N    - chance (1 in N) of flipping a bit in L2 cache
 *   mem_flip_chance=N   - chance (1 in N) of flipping a bit in main memory
 *
 * Copyright (C) 2026
 * License: GNU GPL, version 2 or later.
 */

#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

#define STRTOLL(x) g_ascii_strtoll(x, NULL, 10)

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t l1d_flip_chance;
static uint64_t l1i_flip_chance;
static uint64_t l2_flip_chance;
static uint64_t mem_flip_chance;

static uint64_t l1d_flips;
static uint64_t l1i_flips;
static uint64_t l2_flips;
static uint64_t mem_flips;
static uint64_t total_accesses;

static GMutex rng_lock;
static GRand *rng;

typedef uint64_t (*cache_addr_fn)(void);

static cache_addr_fn get_l1d_addr;
static cache_addr_fn get_l1i_addr;
static cache_addr_fn get_l2_addr;
static cache_addr_fn get_mem_addr;

/**
 * Read a byte at the given virtual address, flip a random bit, and write it
 * back. Returns true if the flip was successfully applied.
 */
static bool flip_bit_at(uint64_t addr)
{
    uint8_t byte;

    if (!qemu_plugin_read_memory_vaddr(addr, &byte, 1)) {
        return false;
    }

    g_mutex_lock(&rng_lock);
    int bit = g_rand_int_range(rng, 0, 8);
    g_mutex_unlock(&rng_lock);
    byte ^= (1u << bit);

    if (!qemu_plugin_write_memory_vaddr(addr, &byte, 1)) {
        return false;
    }

    return true;
}

/**
 * Try to inject a fault for a given cache level.
 * @chance: 1-in-N probability (0 means disabled)
 * @get_addr: function to get a target address from the cache plugin
 * @counter: pointer to the flip counter for this level
 */
static void maybe_flip(uint64_t chance, cache_addr_fn get_addr,
                        uint64_t *counter)
{
    if (chance == 0 || !get_addr) {
        return;
    }

    g_mutex_lock(&rng_lock);
    bool should_flip = (g_rand_int_range(rng, 0, chance) == 0);
    g_mutex_unlock(&rng_lock);

    if (!should_flip) {
        return;
    }

    uint64_t addr = get_addr();
    if (addr == UINT64_MAX) {
        return;
    }

    if (flip_bit_at(addr)) {
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
    }
}

static void vcpu_mem_access(unsigned int vcpu_index,
                            qemu_plugin_meminfo_t info,
                            uint64_t vaddr, void *userdata)
{
    __atomic_fetch_add(&total_accesses, 1, __ATOMIC_SEQ_CST);

    maybe_flip(l1d_flip_chance, get_l1d_addr, &l1d_flips);
    maybe_flip(l2_flip_chance, get_l2_addr, &l2_flips);
    maybe_flip(mem_flip_chance, get_mem_addr, &mem_flips);
}

static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata)
{
    maybe_flip(l1i_flip_chance, get_l1i_addr, &l1i_flips);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_access,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);

        if (l1i_flip_chance) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                                   QEMU_PLUGIN_CB_NO_REGS,
                                                   NULL);
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) rep = g_string_new("Fault Injection Summary:\n");

    g_string_append_printf(rep, "  Total memory accesses: %" PRIu64 "\n",
                           total_accesses);
    g_string_append_printf(rep, "  L1 data cache flips:   %" PRIu64 " (1 in %"
                           PRIu64 ")\n", l1d_flips, l1d_flip_chance);
    g_string_append_printf(rep, "  L1 insn cache flips:   %" PRIu64 " (1 in %"
                           PRIu64 ")\n", l1i_flips, l1i_flip_chance);
    g_string_append_printf(rep, "  L2 cache flips:        %" PRIu64 " (1 in %"
                           PRIu64 ")\n", l2_flips, l2_flip_chance);
    g_string_append_printf(rep, "  Memory flips:          %" PRIu64 " (1 in %"
                           PRIu64 ")\n", mem_flips, mem_flip_chance);

    qemu_plugin_outs(rep->str);
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_autofree char **tokens = g_strsplit(opt, "=", 2);

        if (g_strcmp0(tokens[0], "l1d_flip_chance") == 0) {
            l1d_flip_chance = STRTOLL(tokens[1]);
        } else if (g_strcmp0(tokens[0], "l1i_flip_chance") == 0) {
            l1i_flip_chance = STRTOLL(tokens[1]);
        } else if (g_strcmp0(tokens[0], "l2_flip_chance") == 0) {
            l2_flip_chance = STRTOLL(tokens[1]);
        } else if (g_strcmp0(tokens[0], "mem_flip_chance") == 0) {
            mem_flip_chance = STRTOLL(tokens[1]);
        } else {
            fprintf(stderr, "fault_injection: unknown option: %s\n", opt);
            return -1;
        }
    }

    if (!l1d_flip_chance && !l1i_flip_chance &&
        !l2_flip_chance && !mem_flip_chance) {
        fprintf(stderr, "fault_injection: at least one flip chance must be "
                "set\n");
        return -1;
    }

    /* Resolve cache plugin functions */
    get_l1d_addr = dlsym(RTLD_DEFAULT, "cache_get_l1d_addr");
    get_l1i_addr = dlsym(RTLD_DEFAULT, "cache_get_l1i_addr");
    get_l2_addr = dlsym(RTLD_DEFAULT, "cache_get_l2_addr");
    get_mem_addr = dlsym(RTLD_DEFAULT, "cache_get_mem_addr");

    if (!get_l1d_addr && !get_l1i_addr && !get_l2_addr && !get_mem_addr) {
        fprintf(stderr, "fault_injection: cache plugin not loaded — "
                "load it before fault_injection\n");
        return -1;
    }

    rng = g_rand_new();

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
