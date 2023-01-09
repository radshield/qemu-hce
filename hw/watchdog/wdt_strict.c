/*
 * Strict watchdog device -- not an emulation of any real watchdog timer.
 *
 * This is a custom-designed watchdog based on following best practices for
 * high-assurance systems:
 *  1. It initializes itself at boot, so that even if control software fails
 *     to boot, it can still cause a reset.
 *  2. It must be fed within a predefined range of times. If it is fed early
 *     or late, the watchdog will decide that the software is malfunctioning
 *     and issue a reset.
 *  3. Feeding the watchdog requires reading from one register, performing a
 *     simple computation, and feeding the result back into a second register.
 *     This minimizes the chance that malfunctioning code can inadvertently
 *     feed the watchdog.
 */

#include "qemu/osdep.h"
#include "sysemu/runstate.h"
#include "sysemu/watchdog.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "hw/qdev-properties.h"
#include "hw/mem/memory-device.h"

enum {
    WDT_STRICT_REG_GREET        = 0x00,
    WDT_STRICT_REG_FEED         = 0x04,
    WDT_STRICT_REG_DEADLINE     = 0x08,
    WDT_STRICT_REG_EARLY_OFFSET = 0x0C,
    WDT_STRICT_MMIO_SIZE        = 0x10,
};

#define TYPE_WDT_STRICT "watchdog-strict"
OBJECT_DECLARE_SIMPLE_TYPE(WatchdogStrictState, WDT_STRICT)

typedef struct WatchdogStrictState {
    SysBusDevice parent_obj;

    QEMUTimer *timer;
    MemoryRegion mmio;

    bool       disable_auto;
    uint64_t   feeding_period_ns;
    uint64_t   early_feed_limit_ns;
    bool       was_greeted;
    uint32_t   next_food_expected;
    uint64_t   next_expiration_time;
} WatchdogAutoState;

static const VMStateDescription vmstate_strict = {
    .name = "vmstate_watchdog_strict",
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, WatchdogStrictState),
        VMSTATE_UINT64(feeding_period_ns, WatchdogStrictState),
        VMSTATE_UINT64(early_feed_limit_ns, WatchdogStrictState),
        VMSTATE_BOOL(was_greeted, WatchdogStrictState),
        VMSTATE_UINT32(next_food_expected, WatchdogStrictState),
        VMSTATE_UINT64(next_expiration_time, WatchdogStrictState),
        VMSTATE_END_OF_LIST()
    }
};

static void wdt_strict_timer_expired(void *dev)
{
    WatchdogStrictState *wdt_strict = WDT_STRICT(dev);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    qemu_log_mask(CPU_LOG_RESET, "Strict watchdog expired at %ld.\n", now);
    assert(now >= wdt_strict->next_expiration_time);
    wdt_strict->next_expiration_time += wdt_strict->feeding_period_ns;
    assert(now < wdt_strict->next_expiration_time);

    timer_mod(wdt_strict->timer, wdt_strict->next_expiration_time);

    wdt_strict->was_greeted = false;
    if (!wdt_strict->disable_auto) {
        watchdog_perform_action();
    }
}

static void wdt_strict_defer_next_reset(WatchdogStrictState *wdt_strict)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    assert(now <= wdt_strict->next_expiration_time);
    if (wdt_strict->next_expiration_time <= now + wdt_strict->feeding_period_ns) {
        wdt_strict->next_expiration_time += wdt_strict->feeding_period_ns;
        timer_mod(wdt_strict->timer, wdt_strict->next_expiration_time);
    }
}

static void wdt_strict_immediate_reset(WatchdogStrictState *wdt_strict)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    qemu_log_mask(CPU_LOG_RESET, "Strict watchdog experienced secondary error at %ld.\n", now);

    wdt_strict_defer_next_reset(wdt_strict);

    wdt_strict->was_greeted = false;
    watchdog_perform_action();
}

static uint32_t integer_power_truncated(uint32_t base, uint16_t power)
{
    uint32_t out = 1;

    for (int i = 15; i >= 0; i--) {
        out *= out;
        if (power & (1 << i)) {
            out *= base;
        }
    }

    return out;
}

static uint32_t wdt_strict_food_from_recipe(uint32_t recipe)
{
    // pick out a base and exponent from the recipe and raise the base to that power
    // (but make sure the base is odd, because if it's even, it will quickly become 0)
    uint32_t result = integer_power_truncated((recipe >> 8) | 1, recipe & 0xFFFF);
    // XOR by reversed bits
    for (int i = 0; i < 32; i++) {
        result ^= ((recipe >> i) & 1) << (31 - i);
    }
    return result;
}

static uint32_t wdt_strict_greet(WatchdogStrictState *wdt_strict)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    assert(wdt_strict->next_expiration_time >= now);

    // make sure we aren't greeted early, and that we weren't already greeted!
    if (now + wdt_strict->early_feed_limit_ns < wdt_strict->next_expiration_time
            || wdt_strict->was_greeted) {
        // if anything is wrong, the system is probably out of order, and will
        // need to be reset.
        wdt_strict_immediate_reset(wdt_strict);
        return 0;
    } else {
        // reuse food_from_recipe function to also generate the recipe
        // based on the current time
        uint32_t recipe = wdt_strict_food_from_recipe(~(uint32_t)now);

        // generate expected "food" based on recipe
        wdt_strict->was_greeted = true;
        wdt_strict->next_food_expected = wdt_strict_food_from_recipe(recipe);

        // and return the recipe for the watchdog caretaker to reproduce our work
        return recipe;
    }
}

static void wdt_strict_feed(WatchdogStrictState *wdt_strict, uint32_t value)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    assert(wdt_strict->next_expiration_time >= now);

    // make sure we aren't fed early, that we aren't fed before we're greeted,
    // and that we're fed the right food!
    if (now + wdt_strict->early_feed_limit_ns < wdt_strict->next_expiration_time
            || !wdt_strict->was_greeted || value != wdt_strict->next_food_expected) {
        // if anything is wrong, the system is probably out of order, and will
        // need to be reset.
        wdt_strict_immediate_reset(wdt_strict);
    } else {
        wdt_strict_defer_next_reset(wdt_strict);
        wdt_strict->was_greeted = false;
    }
}

static uint64_t wdt_strict_read(void *opaque, hwaddr addr, unsigned int size)
{
    WatchdogStrictState *wdt_strict = WDT_STRICT(opaque);

    assert(size == 4);

    switch (addr) {
    case WDT_STRICT_REG_GREET:
        // handle reads from GREET register
        return wdt_strict_greet(wdt_strict);
    case WDT_STRICT_REG_FEED:
        // reads from FEED register should be rejected
        wdt_strict_immediate_reset(wdt_strict);
        return 0;
    case WDT_STRICT_REG_DEADLINE:
        // return (truncated) deadline for feed
        return (uint32_t) wdt_strict->next_expiration_time;
    case WDT_STRICT_REG_EARLY_OFFSET:
        // return fixed offset of how early feeding is permitted
        return (uint32_t) wdt_strict->early_feed_limit_ns;
    default:
        assert(false);
    }
}

static void wdt_strict_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned int size)
{
    WatchdogStrictState *wdt_strict = WDT_STRICT(opaque);

    assert(size == 4);

    switch (addr) {
    case WDT_STRICT_REG_GREET:
    case WDT_STRICT_REG_DEADLINE:
    case WDT_STRICT_REG_EARLY_OFFSET:
        // writes to read-only registers should be rejected
        wdt_strict_immediate_reset(wdt_strict);
        break;
    case WDT_STRICT_REG_FEED:
        // handle writes to FEED register
        wdt_strict_feed(wdt_strict, value);
        break;
    default:
        assert(false);
    }
}

static const MemoryRegionOps wdt_strict_ops = {
    .read  = wdt_strict_read,
    .write = wdt_strict_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void wdt_strict_realize(DeviceState *dev, Error **errp)
{
    WatchdogStrictState *wdt_strict = WDT_STRICT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&wdt_strict->mmio, OBJECT(dev),
                          &wdt_strict_ops, wdt_strict,
                          TYPE_WDT_STRICT,
                          WDT_STRICT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &wdt_strict->mmio);

    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    wdt_strict->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_strict_timer_expired, dev);
    qemu_log_mask(CPU_LOG_RESET, "Strict initialized at %ld.\n", now);
    wdt_strict->next_expiration_time = now + wdt_strict->feeding_period_ns;
    wdt_strict->was_greeted = false;
    timer_mod(wdt_strict->timer, wdt_strict->next_expiration_time);
}

static void wdt_strict_unrealize(DeviceState *dev)
{
    WatchdogStrictState *wdt_strict = WDT_STRICT(dev);

    timer_del(wdt_strict->timer);
    timer_free(wdt_strict->timer);
}

static Property wdt_strict_properties[] = {
    DEFINE_PROP_BOOL("disable-auto", WatchdogStrictState, disable_auto, false),
    DEFINE_PROP_UINT64("period-ns", WatchdogStrictState, feeding_period_ns, NANOSECONDS_PER_SECOND),
    DEFINE_PROP_UINT64("early-feed-ns", WatchdogStrictState, early_feed_limit_ns, NANOSECONDS_PER_SECOND),
    DEFINE_PROP_END_OF_LIST(),
};

static void wdt_strict_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, wdt_strict_properties);
    dc->realize = wdt_strict_realize;
    dc->unrealize = wdt_strict_unrealize;
    dc->vmsd = &vmstate_strict;
    dc->desc = "strict watchdog for high-assurance systems";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo wdt_strict_info = {
    .name          = TYPE_WDT_STRICT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WatchdogStrictState),
    .class_init    = wdt_strict_class_init,
};

static void wdt_strict_register_types(void)
{
    type_register_static(&wdt_strict_info);
}

type_init(wdt_strict_register_types)
