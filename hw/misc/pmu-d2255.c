/*
 * Apple PMU D2255.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/misc/pmu-d2255.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "system/system.h"

// #define DEBUG_PMU_D2255

typedef enum
{
    PMU_OP_STATE_NONE,
    PMU_OP_STATE_RECV,
    PMU_OP_STATE_SEND,
} PMUOpState;

typedef enum
{
    PMU_ADDR_UPPER,
    PMU_ADDR_LOWER,
    PMU_ADDR_RECEIVED,
} PMUAddrState;

#define REG_SIZE (0x8800)

struct PMUD2255State
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
    uint8_t      reg[REG_SIZE];
    QEMUTimer*   timer;
    qemu_irq     irq;
    uint32_t     tick_period;
    uint64_t     rtc_offset;
    PMUOpState   op_state;
    PMUAddrState address_state;
    uint16_t     address;
};

#define RTC_TICK_FREQ (32768)

#define REG_EVENT_A              (0x140)
#define REG_EVENT_B              (0x141)
#define REG_EVENT_C              (0x142)
#define REG_EVENT_D              (0x143)
#define REG_EVENT_E              (0x144)
#define REG_EVENT_F              (0x145)
#define REG_EVENT_G              (0x146)
#define REG_EVENT_H              (0x147)
#define REG_EVENT_I              (0x148)
#define REG_EVENT_J              (0x149)
#define REG_EVENT_K              (0x14A)
#define REG_EVENT_L              (0x14B)
#define REG_EVENT_M              (0x14C)
#define REG_EVENT_N              (0x14D)
#define REG_EVENT_O              (0x14E)
#define REG_EVENT_P              (0x14F)
#define REG_EVENT_Q              (0x150)
#define REG_EVENT_R              (0x151)
#define REG_EVENT_S              (0x152)
#define REG_EVENT_T              (0x153)
#define REG_EVENT_U              (0x154)
#define REG_EVENT_V              (0x155)
#define REG_EVENT_W              (0x156)
#define REG_EVENT_X              (0x157)
#define REG_EVENT_Y              (0x158)
#define REG_STATUS_A             (0x180)
#define REG_STATUS_B             (0x181)
#define REG_STATUS_C             (0x182)
#define REG_STATUS_D             (0x183)
#define REG_STATUS_E             (0x184)
#define REG_STATUS_F             (0x185)
#define REG_STATUS_G             (0x186)
#define REG_STATUS_H             (0x187)
#define REG_STATUS_I             (0x188)
#define REG_STATUS_J             (0x189)
#define REG_STATUS_K             (0x18A)
#define REG_STATUS_L             (0x18B)
#define REG_STATUS_M             (0x18C)
#define REG_STATUS_N             (0x18D)
#define REG_STATUS_O             (0x18E)
#define REG_STATUS_P             (0x18F)
#define REG_STATUS_Q             (0x190)
#define REG_STATUS_R             (0x191)
#define REG_STATUS_S             (0x192)
#define REG_STATUS_T             (0x193)
#define REG_STATUS_U             (0x194)
#define REG_STATUS_V             (0x195)
#define REG_STATUS_W             (0x196)
#define REG_STATUS_X             (0x197)
#define REG_STATUS_Y             (0x198)
#define REG_IRQ_MASK_A           (0x1C0)
#define REG_IRQ_MASK_B           (0x1C1)
#define REG_IRQ_MASK_C           (0x1C2)
#define REG_IRQ_MASK_D           (0x1C3)
#define REG_IRQ_MASK_E           (0x1C4)
#define REG_IRQ_MASK_F           (0x1C5)
#define REG_IRQ_MASK_G           (0x1C6)
#define REG_IRQ_MASK_H           (0x1C7)
#define REG_IRQ_MASK_I           (0x1C8)
#define REG_IRQ_MASK_J           (0x1C9)
#define REG_IRQ_MASK_K           (0x1CA)
#define REG_IRQ_MASK_L           (0x1CB)
#define REG_IRQ_MASK_M           (0x1CC)
#define REG_IRQ_MASK_N           (0x1CD)
#define REG_IRQ_MASK_O           (0x1CE)
#define REG_IRQ_MASK_P           (0x1CF)
#define REG_IRQ_MASK_Q           (0x1D0)
#define REG_IRQ_MASK_R           (0x1D1)
#define REG_IRQ_MASK_S           (0x1D2)
#define REG_IRQ_MASK_T           (0x1D3)
#define REG_IRQ_MASK_U           (0x1D4)
#define REG_IRQ_MASK_V           (0x1D5)
#define REG_IRQ_MASK_W           (0x1D6)
#define REG_IRQ_MASK_X           (0x1D7)
#define REG_IRQ_MASK_Y           (0x1D8)
#define REG_MASK_REV_CODE        (0x200)
#define REG_TRIM_REL_CODE        (0x201)
#define REG_PLATFORM_ID          (0x202)
#define REG_DEVICE_ID0           (0x203)
#define REG_DEVICE_ID1           (0x204)
#define REG_DEVICE_ID2           (0x205)
#define REG_DEVICE_ID3           (0x206)
#define REG_DEVICE_ID4           (0x207)
#define REG_DEVICE_ID5           (0x208)
#define REG_DEVICE_ID6           (0x209)
#define REG_DEVICE_ID7           (0x20A)
#define REG_APP_TMUX             (0x212)
#define REG_SYSCTL_PRE_UVLO_CTRL (0x268)
#define REG_FAULT_LOG1           (0x2C0)
#define REG_FAULT_LOG2           (0x2C1)
#define REG_RTC_CONTROL          (0x500)
#define REG_RTC_TIMEZONE         (0x0501)
#define REG_RTC_SUB_SECOND_A     (0x0502)
#define REG_RTC_SUB_SECOND_B     (0x0503)
#define REG_RTC_SECOND_A         (0x504)
#define REG_RTC_SECOND_B         (0x505)
#define REG_RTC_SECOND_C         (0x506)
#define REG_RTC_SECOND_D         (0x507)
#define REG_RTC_ALARM_A          (0x508)
#define REG_RTC_ALARM_B          (0x509)
#define REG_RTC_ALARM_C          (0x50A)
#define REG_RTC_ALARM_D          (0x50B)
#define REG_SCRATCH              (0x5000)

#define RTC_EVENT_ALARM          (1 << 0)
#define RTC_CONTROL_MONITOR      (1 << 0)
#define RTC_CONTROL_ALARM_EN     (1 << 6)
#define SCRATCH_LEN              (0x27)
#define OFF_SCRATCH_SECS_OFFSET  (4)
#define OFF_SCRATCH_TICKS_OFFSET (21)

#define RREG32(off)         ldl_le_p(&s->reg[off])
#define WREG32(off, val)    stl_le_p(&s->reg[off], val)
#define WREG32_OR(off, val) WREG32(off, RREG32(off) | (val))

static unsigned int frq_to_period_ns(unsigned int freq_hz)
{ return NANOSECONDS_PER_SECOND > freq_hz ? NANOSECONDS_PER_SECOND / freq_hz : 1; }

static uint64_t G_GNUC_UNUSED tick_to_ns(PMUD2255State* s, uint64_t tick)
{ return (tick >> 15) * NANOSECONDS_PER_SECOND + (tick & 0x7FFF) * s->tick_period; }

static uint64_t rtc_get_tick(PMUD2255State* s, uint64_t* out_ns)
{
    uint64_t now    = qemu_clock_get_ns(rtc_clock);
    uint64_t offset = s->rtc_offset;
    if (out_ns) { *out_ns = now; }
    now -= offset;
    return ((now / NANOSECONDS_PER_SECOND) << 15) | ((now / s->tick_period) & 0x7FFF);
}

static void pmu_d2255_set_tick_offset(PMUD2255State* s, uint64_t tick_offset)
{
    WREG32(REG_SCRATCH + OFF_SCRATCH_SECS_OFFSET, tick_offset >> 15);
    WREG32(REG_SCRATCH + OFF_SCRATCH_TICKS_OFFSET + 0, tick_offset & 0xFF);
    WREG32(REG_SCRATCH + OFF_SCRATCH_TICKS_OFFSET + 1, (tick_offset >> 8) & 0x7F);
}

static void pmu_d2255_update_irq(PMUD2255State* s)
{
    if (RREG32(REG_IRQ_MASK_A) & RREG32(REG_EVENT_C)) {
        qemu_irq_raise(s->irq);
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: raised IRQ");
#endif
    }
    else {
        qemu_irq_lower(s->irq);
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: lowered IRQ");
#endif
    }
}

static void pmu_d2255_alarm(void* opaque)
{
    PMUD2255State* s = opaque;

    WREG32_OR(REG_EVENT_C, RTC_EVENT_ALARM);
    pmu_d2255_update_irq(s);
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_RTC, NULL);
}

static void pmu_d2255_set_alarm(PMUD2255State* s)
{
    uint32_t seconds = RREG32(REG_RTC_ALARM_A) - (rtc_get_tick(s, NULL) >> 15);
    if (RREG32(REG_RTC_CONTROL) & RTC_CONTROL_ALARM_EN) {
        if (seconds == 0) {
            timer_del(s->timer);
            pmu_d2255_alarm(s);
        }
        else {
            int64_t now = qemu_clock_get_ns(rtc_clock);
            timer_mod_ns(s->timer, now + ((int64_t)seconds * NANOSECONDS_PER_SECOND));
        }
    }
    else {
        timer_del(s->timer);
    }
}

static int pmu_d2255_event(I2CSlave* i2c, enum i2c_event event)
{
    PMUD2255State* s;

    s = PMU_D2255(i2c);

    switch (event) {
        case I2C_START_RECV:
            if (s->op_state != PMU_OP_STATE_NONE) {
                qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: attempted to start transaction while a "
                                               "transaction is already ongoing.\n");
                return -1;
            }

            s->op_state = PMU_OP_STATE_RECV;
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: recv started.");
#endif
            return 0;
        case I2C_START_SEND:
            if (s->op_state != PMU_OP_STATE_NONE) {
                qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: attempted to start transaction while a "
                                               "transaction is already ongoing.\n");
                return -1;
            }

            s->op_state      = PMU_OP_STATE_SEND;
            s->address       = 0;
            s->address_state = PMU_ADDR_UPPER;
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: send started.");
#endif
            return 0;
        case I2C_START_SEND_ASYNC:
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: async is not supported.");
#endif
            return -1;
        case I2C_FINISH: s->op_state = PMU_OP_STATE_NONE;
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: transaction end.");
#endif
            return 0;
        case I2C_NACK:
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: transaction nack.");
#endif
            return -1;
        default: info_report("PMU D2255: TODO: DEFAULT CASE!!! what to return?"); return -1;
    }
    return 0;
}

static uint8_t pmu_d2255_rx(I2CSlave* i2c)
{
    PMUD2255State* s;

    s = PMU_D2255(i2c);

    if (s->op_state != PMU_OP_STATE_RECV) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: RX attempted but transaction is not recv.\n");
        return 0x00;
    }

    if (s->address_state != PMU_ADDR_RECEIVED) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: no address was sent.\n");
        return 0x00;
    }

    if (s->address >= sizeof(s->reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: offset 0x%X is INVALID.\n", s->address);
        return 0x00;
    }

    if (s->address == REG_RTC_SUB_SECOND_A) {
        uint64_t now                 = rtc_get_tick(s, NULL);
        s->reg[REG_RTC_SUB_SECOND_A] = (now << 1) & 0xFF;
        s->reg[REG_RTC_SUB_SECOND_B] = (now >> 7) & 0xFF;
        s->reg[REG_RTC_SECOND_A]     = (now >> 15) & 0xFF;
        s->reg[REG_RTC_SECOND_B]     = (now >> 23) & 0xFF;
        s->reg[REG_RTC_SECOND_C]     = (now >> 31) & 0xFF;
        s->reg[REG_RTC_SECOND_D]     = (now >> 39) & 0xFF;
    }

#ifdef DEBUG_PMU_D2255
    info_report("PMU D2255: 0x%X -> 0x%X.", s->address, s->reg[s->address]);
#endif

    return s->reg[s->address++];
}

static int pmu_d2255_tx(I2CSlave* i2c, uint8_t data)
{
    PMUD2255State* s;

    s = PMU_D2255(i2c);

    if (s->op_state != PMU_OP_STATE_SEND) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: TX attempted but transaction is not send.\n");
        return 0x00;
    }

    switch (s->address_state) {
        case PMU_ADDR_UPPER:
            s->address       |= data << 8;
            s->address_state  = PMU_ADDR_LOWER;
            break;
        case PMU_ADDR_LOWER:
            s->address       |= data;
            s->address        = le16_to_cpu(s->address);
            s->address_state  = PMU_ADDR_RECEIVED;
#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: address set to 0x%X.", s->address);
#endif
            break;
        case PMU_ADDR_RECEIVED:
            if (s->op_state == PMU_OP_STATE_RECV) {
                qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: send transaction attempted but transaction "
                                               "is recv.\n");
                return -1;
            }

            if (s->address >= sizeof(s->reg)) {
                qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: 0x%X <- 0x%X is INVALID.\n", s->address, data);
                return -1;
            }

#ifdef DEBUG_PMU_D2255
            info_report("PMU D2255: 0x%X <- 0x%X.", s->address, data);
#endif
            s->reg[s->address] = data;

            switch (s->address) {
                case REG_RTC_CONTROL                    :
                case REG_RTC_ALARM_A ... REG_RTC_ALARM_D: pmu_d2255_set_alarm(s); break;
                default                                 : break;
            }

            s->address += 1;
            break;
    }
    return 0;
}

static void pmu_d2255_reset_enter(Object* obj, ResetType type)
{
    PMUD2255State* s = PMU_D2255(obj);

    s->op_state      = PMU_OP_STATE_NONE;
    s->address       = 0;
    s->address_state = PMU_ADDR_UPPER;
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->reg + REG_MASK_REV_CODE, 0xFF, REG_DEVICE_ID7 - REG_MASK_REV_CODE);
}

static void pmu_d2255_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    I2CSlaveClass*   c  = I2C_SLAVE_CLASS(klass);

    rc->phases.enter = pmu_d2255_reset_enter;

    dc->desc = "Apple PMU D2255";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->event = pmu_d2255_event;
    c->recv  = pmu_d2255_rx;
    c->send  = pmu_d2255_tx;
}

static void pmu_d2255_init(Object* obj)
{
    PMUD2255State* s;

    s = PMU_D2255(obj);

    s->tick_period = frq_to_period_ns(RTC_TICK_FREQ);
    pmu_d2255_set_tick_offset(s, rtc_get_tick(s, &s->rtc_offset));

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_d2255_alarm, s);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_RTC, true);

    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(PMUD2255State, pmu_d2255, PMU_D2255, I2C_SLAVE)
