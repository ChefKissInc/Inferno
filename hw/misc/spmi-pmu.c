#include "qemu/osdep.h"
#include "hw/arm/dt.h"
#include "hw/irq.h"
#include "hw/misc/spmi-pmu.h"
#include "hw/registerfields.h"
#include "hw/spmi/spmi.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "system/system.h"

#define TYPE_APPLE_SPMI_PMU "apple-spmi-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSPMIPMUState, APPLE_SPMI_PMU)

#define RTC_TICK_FREQ (32768ULL)

REG8(LEG_SCRPAD_OFFSET_SECS, 4)
REG8(LEG_SCRPAD_OFFSET_TICKS, 21)
REG_FIELD(RTC_CONTROL, MONITOR, 0, 1)
REG_FIELD(RTC_CONTROL, ALARM_EN, 6, 1)
REG_FIELD(RTC_EVENT, ALARM, 0, 1)

struct AppleSPMIPMUState
{
    /*< private >*/
    SPMISlave parent_obj;

    /*< public >*/
    qemu_irq   irq;
    QEMUTimer* timer;
    uint32_t   reg_leg_scrpad;
    uint32_t   reg_rtc;
    uint32_t   reg_rtc_irq_mask;
    uint32_t   reg_alarm;
    uint32_t   reg_alarm_ctrl;
    uint32_t   reg_alarm_event;
    uint8_t    reg[0x10000];
    uint16_t   addr;
};

static uint64_t apple_spmi_pmu_get_tick_offset(AppleSPMIPMUState* pmu)
{
    uint32_t secs_reg;
    uint32_t ticks_reg;
    uint64_t tick_offset;

    secs_reg  = pmu->reg_leg_scrpad + R_LEG_SCRPAD_OFFSET_SECS;
    ticks_reg = pmu->reg_leg_scrpad + R_LEG_SCRPAD_OFFSET_TICKS;

    tick_offset  = (uint64_t)pmu->reg[secs_reg + 3] << 39;
    tick_offset |= (uint64_t)pmu->reg[secs_reg + 2] << 31;
    tick_offset |= (uint64_t)pmu->reg[secs_reg + 1] << 23;
    tick_offset |= (uint64_t)pmu->reg[secs_reg + 0] << 15;
    tick_offset |= ((uint64_t)pmu->reg[ticks_reg + 1] & 0x7F) << 8;
    tick_offset |= (uint64_t)pmu->reg[ticks_reg + 0];

    return tick_offset;
}

static void apple_spmi_pmu_set_tick_offset(AppleSPMIPMUState* pmu, uint64_t tick_offset)
{
    uint32_t secs_reg;
    uint32_t ticks_reg;

    secs_reg  = pmu->reg_leg_scrpad + R_LEG_SCRPAD_OFFSET_SECS;
    ticks_reg = pmu->reg_leg_scrpad + R_LEG_SCRPAD_OFFSET_TICKS;

    pmu->reg[secs_reg + 3]  = (tick_offset >> 39) & 0xFF;
    pmu->reg[secs_reg + 2]  = (tick_offset >> 31) & 0xFF;
    pmu->reg[secs_reg + 1]  = (tick_offset >> 23) & 0xFF;
    pmu->reg[secs_reg + 0]  = (tick_offset >> 15) & 0xFF;
    pmu->reg[ticks_reg + 1] = ((tick_offset >> 8) & 0x7F);
    pmu->reg[ticks_reg + 0] = tick_offset & 0xFF;
}

static uint64_t apple_rtc_ns_to_tick(AppleSPMIPMUState* pmu, uint64_t now)
{
    const uint64_t secs       = now / NANOSECONDS_PER_SECOND;
    const uint64_t frac_ns    = now % NANOSECONDS_PER_SECOND;
    const uint64_t frac_ticks = (frac_ns * RTC_TICK_FREQ + NANOSECONDS_PER_SECOND / 2) / NANOSECONDS_PER_SECOND;
    const uint64_t ticks      = ((secs << 15) | (frac_ticks & 0x7FFF));
    return ticks - apple_spmi_pmu_get_tick_offset(pmu);
}

static uint64_t apple_rtc_get_current_tick(AppleSPMIPMUState* pmu)
{ return apple_rtc_ns_to_tick(pmu, qemu_clock_get_ns(rtc_clock)); }

static void apple_spmi_pmu_update_irq(AppleSPMIPMUState* pmu)
{
    if (pmu->reg[pmu->reg_rtc_irq_mask] & pmu->reg[pmu->reg_alarm_event]) { qemu_irq_raise(pmu->irq); }
    else {
        qemu_irq_lower(pmu->irq);
    }
}

static void apple_spmi_pmu_alarm(void* opaque)
{
    AppleSPMIPMUState* pmu = opaque;

    pmu->reg[pmu->reg_alarm_event] |= R_RTC_EVENT_ALARM_MASK;
    apple_spmi_pmu_update_irq(pmu);
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_RTC, NULL);
}

static void apple_spmi_pmu_set_alarm(AppleSPMIPMUState* pmu)
{
    int64_t now     = qemu_clock_get_ns(rtc_clock);
    int64_t seconds = (int64_t)ldl_le_p(&pmu->reg[pmu->reg_alarm]) - (int64_t)(apple_rtc_ns_to_tick(pmu, now) >> 15);

    if (pmu->reg[pmu->reg_alarm_ctrl] & R_RTC_CONTROL_ALARM_EN_MASK) {
        if (seconds <= 0) {
            timer_del(pmu->timer);
            apple_spmi_pmu_alarm(pmu);
        }
        else {
            timer_mod_ns(pmu->timer, now + seconds * NANOSECONDS_PER_SECOND);
        }
    }
    else {
        timer_del(pmu->timer);
    }
}

static int apple_spmi_pmu_send(SPMISlave* slave, uint8_t* data, uint8_t len)
{
    AppleSPMIPMUState* pmu = container_of(slave, AppleSPMIPMUState, parent_obj);
    uint16_t           addr, end_addr;

    for (addr = pmu->addr, end_addr = addr + len; addr < end_addr; addr++) {
        pmu->reg[addr] = data[addr - pmu->addr];
        if (addr == pmu->reg_alarm_ctrl || (addr >= pmu->reg_alarm && addr < pmu->reg_alarm + 4)) {
            apple_spmi_pmu_set_alarm(pmu);
        }
    }
    pmu->addr = addr;

    return len;
}

static int apple_spmi_pmu_recv(SPMISlave* slave, uint8_t* data, uint8_t len)
{
    AppleSPMIPMUState* pmu      = container_of(slave, AppleSPMIPMUState, parent_obj);
    uint16_t           addr     = pmu->addr;
    uint16_t           end_addr = pmu->addr + len;

    if (end_addr > pmu->reg_rtc && addr < pmu->reg_rtc + 6) {
        uint64_t now               = apple_rtc_get_current_tick(pmu);
        pmu->reg[pmu->reg_rtc]     = (now << 1) & 0xFF;
        pmu->reg[pmu->reg_rtc + 1] = (now >> 7) & 0xFF;
        pmu->reg[pmu->reg_rtc + 2] = (now >> 15) & 0xFF;
        pmu->reg[pmu->reg_rtc + 3] = (now >> 23) & 0xFF;
        pmu->reg[pmu->reg_rtc + 4] = (now >> 31) & 0xFF;
        pmu->reg[pmu->reg_rtc + 5] = (now >> 39) & 0xFF;
    }

    for (; addr < end_addr; ++addr) { data[addr - pmu->addr] = pmu->reg[addr]; }

    pmu->addr = addr;
    return len;
}

static int apple_spmi_pmu_command(SPMISlave* slave, uint8_t opcode, uint16_t addr)
{
    AppleSPMIPMUState* pmu = container_of(slave, AppleSPMIPMUState, parent_obj);
    pmu->addr              = addr;

    switch (opcode) {
        case SPMI_CMD_EXT_READ  :
        case SPMI_CMD_EXT_READL :
        case SPMI_CMD_EXT_WRITE :
        case SPMI_CMD_EXT_WRITEL: return 0;
        default                 : return 1;
    }
}

DeviceState* apple_spmi_pmu_from_node(AppleDTNode* node)
{
    DeviceState*       dev;
    AppleSPMIPMUState* pmu;
    AppleDTProp*       prop;

    dev = qdev_new(TYPE_APPLE_SPMI_PMU);
    pmu = APPLE_SPMI_PMU(dev);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    spmi_set_slave_sid(SPMI_SLAVE(dev), *(uint32_t*)prop->data);

    pmu->reg_rtc          = apple_dt_get_prop_u32(node, "info-rtc", &error_fatal);
    pmu->reg_alarm        = apple_dt_get_prop_u32(node, "info-rtc_alarm_offset", &error_fatal);
    pmu->reg_alarm_ctrl   = apple_dt_get_prop_u32(node, "info-rtc_alarm_ctrl", &error_fatal);
    pmu->reg_alarm_event  = apple_dt_get_prop_u32(node, "info-rtc_alarm_event", &error_fatal);
    pmu->reg_rtc_irq_mask = apple_dt_get_prop_u32(node, "info-rtc_irq_mask_offset", &error_fatal);
    pmu->reg_leg_scrpad   = apple_dt_get_prop_u32(node, "info-leg_scrpad", &error_fatal);
    // TODO: Handle `info-clock_offset` if it exists.

    apple_spmi_pmu_set_tick_offset(pmu, apple_rtc_get_current_tick(pmu));

    pmu->timer = timer_new_ns(rtc_clock, apple_spmi_pmu_alarm, pmu);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_RTC, true);

    qdev_init_gpio_out(dev, &pmu->irq, 1);

    return dev;
}

static void apple_spmi_pmu_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*    dc = DEVICE_CLASS(klass);
    SPMISlaveClass* sc = SPMI_SLAVE_CLASS(klass);

    dc->desc = "Apple Dialog SPMI PMU";

    sc->send    = apple_spmi_pmu_send;
    sc->recv    = apple_spmi_pmu_recv;
    sc->command = apple_spmi_pmu_command;
}

static const TypeInfo apple_spmi_pmu_type_info = {
    .name          = TYPE_APPLE_SPMI_PMU,
    .parent        = TYPE_SPMI_SLAVE,
    .instance_size = sizeof(AppleSPMIPMUState),
    .class_init    = apple_spmi_pmu_class_init,
};

static void apple_spmi_pmu_register_types(void) { type_register_static(&apple_spmi_pmu_type_info); }

type_init(apple_spmi_pmu_register_types)
