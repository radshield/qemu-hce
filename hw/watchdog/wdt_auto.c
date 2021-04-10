/*
 * Autonomous watchdog device -- for gdb hacking
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
#include "qapi/qapi-commands-run-state.h"
#include "qapi/qapi-events-run-state.h"

#define TYPE_WDT_AUTO "watchdog_auto"
OBJECT_DECLARE_SIMPLE_TYPE(WatchdogAutoState, WDT_AUTO)

typedef struct WatchdogAutoState {
    /*< private >*/
    DeviceState parent_obj;
    QEMUTimer *timer;

    /*< public >*/
} WatchdogAutoState;

static const VMStateDescription vmstate_auto = {
    .name = "vmstate_watchdog_auto",
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, WatchdogAutoState),
        VMSTATE_END_OF_LIST()
    }
};

static void wdt_auto_timer_expired(void *dev)
{
    WatchdogAutoState *wdt_auto = WDT_AUTO(dev);
    qemu_log_mask(CPU_LOG_RESET, "Autonomous watchdog expired at %ld.\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    timer_del(wdt_auto->timer);
    /* timer_mod(wdt_auto->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1 * NANOSECONDS_PER_SECOND); */

    qemu_system_vmstop_request_prepare();
    // qapi_event_send_watchdog(WATCHDOG_ACTION_PAUSE);
    qemu_system_vmstop_request(RUN_STATE_PAUSED);
}

static void wdt_auto_realize(DeviceState *dev, Error **errp)
{
    WatchdogAutoState *wdt_auto = WDT_AUTO(dev);

    wdt_auto->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_auto_timer_expired, dev);
    qemu_log_mask(CPU_LOG_RESET, "Autonomous watchdog INITIALIZED at %ld.\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    timer_mod(wdt_auto->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1 * NANOSECONDS_PER_SECOND);
}

static void wdt_auto_unrealize(DeviceState *dev)
{
    WatchdogAutoState *wdt_auto = WDT_AUTO(dev);

    timer_del(wdt_auto->timer);
    timer_free(wdt_auto->timer);
}

static void wdt_auto_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = wdt_auto_realize;
    dc->unrealize = wdt_auto_unrealize;
    dc->vmsd = &vmstate_auto;
    dc->desc = "autonomous watchdog for gdb hacking";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo wdt_auto_info = {
    .name          = TYPE_WDT_AUTO,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(WatchdogAutoState),
    .class_init    = wdt_auto_class_init,
};

static void wdt_auto_register_types(void)
{
    type_register_static(&wdt_auto_info);
}

type_init(wdt_auto_register_types)
