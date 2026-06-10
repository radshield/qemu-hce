/*
 * Fault Injection Plugin
 *
 * Simulates radiation-induced bit flips on memory and instruction accesses.
 * Flip probability depends on cache level (L1d, L1i, L2, or main memory).
 * Requires the "cache" plugin to be loaded first.
 *
 * Data flips occur after the current access, affecting subsequent loads.
 * Instruction flips flush the TB cache to force re-translation.
 *
 * Parameters (1 in N chance per access):
 *   l1d_flip_chance, l1i_flip_chance, l2_flip_chance, mem_flip_chance
 *
 * Copyright (C) 2026
 * License: GNU GPL, version 2 or later.
 */

#define _GNU_SOURCE
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

typedef bool (*cache_check_fn)(uint64_t addr, int core_idx);

static cache_check_fn is_in_l1d;
static cache_check_fn is_in_l1i;
static cache_check_fn is_in_l2;

/* Flip a random bit in the byte at vaddr. Returns true on success. */
static bool flip_bit_at(uint64_t vaddr)
{
    uint8_t byte;

    if (!qemu_plugin_read_memory_vaddr(vaddr, &byte, 1)) {
        return false;
    }

    g_mutex_lock(&rng_lock);
    int bit = g_rand_int_range(rng, 0, 8);
    g_mutex_unlock(&rng_lock);
    byte ^= (1u << bit);

    if (!qemu_plugin_write_memory_vaddr(vaddr, &byte, 1)) {
        return false;
    }

    return true;
}

/* Returns true with probability 1/chance. */
static bool should_flip(uint64_t chance)
{
    if (chance == 0) {
        return false;
    }
    g_mutex_lock(&rng_lock);
    bool result = (g_rand_int_range(rng, 0, (gint32)chance) == 0);
    g_mutex_unlock(&rng_lock);
    return result;
}

static void vcpu_mem_access(unsigned int vcpu_index,
                            qemu_plugin_meminfo_t info,
                            uint64_t vaddr, void *userdata)
{
    __atomic_fetch_add(&total_accesses, 1, __ATOMIC_SEQ_CST);

    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(info, vaddr);
    if (hwaddr && qemu_plugin_hwaddr_is_io(hwaddr)) {
        return;
    }
    uint64_t paddr = hwaddr ? qemu_plugin_hwaddr_phys_addr(hwaddr) : vaddr;

    uint64_t chance;
    uint64_t *counter;

    if (is_in_l1d && is_in_l1d(paddr, vcpu_index)) {
        chance = l1d_flip_chance;
        counter = &l1d_flips;
    } else if (is_in_l2 && is_in_l2(paddr, vcpu_index)) {
        chance = l2_flip_chance;
        counter = &l2_flips;
    } else {
        chance = mem_flip_chance;
        counter = &mem_flips;
    }

    if (should_flip(chance) && flip_bit_at(vaddr)) {
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
    }
}

/* Instruction fault: check L1i vs main memory, flip a bit, flush TBs. */
static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata)
{
    uint64_t vaddr = (uint64_t)(uintptr_t)userdata;
    uint64_t chance;
    uint64_t *counter;

    if (is_in_l1i && is_in_l1i(vaddr, vcpu_index)) {
        chance = l1i_flip_chance;
        counter = &l1i_flips;
    } else {
        chance = mem_flip_chance;
        counter = &mem_flips;
    }

    if (should_flip(chance) && flip_bit_at(vaddr)) {
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
        qemu_plugin_tb_flush();
    }
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_access,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);

        if (l1i_flip_chance || mem_flip_chance) {
            uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS,
                (void *)(uintptr_t)vaddr);
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

    /*
     * Find libcache.so in the same directory as our own .so.
     * Use dladdr on one of our own symbols to find our path, then replace
     * the filename with libcache.so.
     */
    Dl_info self_info;
    if (!dladdr((void *)qemu_plugin_install, &self_info)) {
        fprintf(stderr, "fault_injection: dladdr failed: %s\n", dlerror());
        return -1;
    }

    const char *self_path = self_info.dli_fname;
    const char *last_slash = strrchr(self_path, '/');
    g_autofree char *cache_path = NULL;
    if (last_slash) {
        cache_path = g_strdup_printf("%.*s/libcache.so",
                                     (int)(last_slash - self_path), self_path);
    } else {
        cache_path = g_strdup("libcache.so");
    }

    void *cache_handle = dlopen(cache_path, RTLD_LAZY | RTLD_NOLOAD);
    if (!cache_handle) {
        fprintf(stderr, "fault_injection: cache plugin not loaded — "
                "load libcache.so before libfault_injection.so\n");
        return -1;
    }

    is_in_l1d = dlsym(cache_handle, "cache_is_in_l1d");
    is_in_l1i = dlsym(cache_handle, "cache_is_in_l1i");
    is_in_l2 = dlsym(cache_handle, "cache_is_in_l2");

    if (!is_in_l1d && !is_in_l1i && !is_in_l2) {
        fprintf(stderr, "fault_injection: cache plugin has no "
                "cache_is_in_* symbols\n");
        dlclose(cache_handle);
        return -1;
    }

    rng = g_rand_new();

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}