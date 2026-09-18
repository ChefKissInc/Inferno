/*
 * Apple Interrupt Controller.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2024-2026 Christian Inci (chris-pcguy).
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
#include "hw/arm/dt.h"
#include "hw/intc/apple_aic.h"
#include "hw/irq.h"
#include "hw/pci/msi.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

/*
 * AIC splits IRQs into domains (ipid)
 * In T8030 device tree, we have aic->ipid_length = 72
 * => IRQ(extInts) max nr = ((len(ipid_mask)>>2)<<5) = 0x240 (interrupts)
 * -> num domains = (0x240 + 31)>>5 = 18 (domains)
 * 0x240/18 = 32 (bits) of an uint32_t
 *
 * Commands such as REG_AIC_EIR_MASK_SET/CLR assign each domain to a 32bit
 * register. When masking/unmasking-ing IRQ n, write to (aic_base +
 * command_reg_base + (n / 32) * 4) a uint32_t which has (n % 32)-th bit set,
 * command_reg_base is 0x4100 for REG_AIC_EIR_MASK_SET, 0x4180 for
 * REG_AIC_EIR_MASK_CLR.
 *
 * T8030 uses both fast IPI, and AIC IPIs.
 * AIC IPIs' vectors are right after IRQs' vectors.
 * num IRQ + (X * 2) -> self_ipi (cpuX->cpuX)
 * num IRQ + (Y * 2) + 1 -> other_ipi (cpuX->cpuY)
 */

// TODO: this is hardcoded for T8030
#define AIC_INT_COUNT (576)
#define AIC_CPU_COUNT (6)
#define AIC_VERSION   (2)

#define REG_AIC_REV  (0x0000)
#define REG_AIC_CAP0 (0x0004)
#define REG_AIC_CAP1 (0x0008)
#define REG_AIC_RST  (0x000C)

#define REG_AIC_GLB_CFG       (0x0010)
#define AIC_GLBCFG_IEN        BIT32(0)
#define AIC_GLBCFG_AEWT_SHIFT (4)
#define AIC_GLBCFG_SEWT_SHIFT (8)
#define AIC_GLBCFG_AIWT_SHIFT (12)
#define AIC_GLBCFG_SIWT_SHIFT (16)
#define AIC_GLBCFG_DIWT_SHIFT (20)

#define AIC_GLBCFG_WT(_s, _t)     (((_t) & AIC_GLBCFG_WT_MASK) << (_s))
#define AIC_GLBCFG_WT_GET(_v, _s) (((_v) >> (_s)) & AIC_GLBCFG_WT_MASK)

#define AIC_GLBCFG_AEWT(_t)   AIC_GLBCFG_WT(AIC_GLBCFG_AEWT_SHIFT, _t)
#define AIC_GLBCFG_SEWT(_t)   AIC_GLBCFG_WT(AIC_GLBCFG_SEWT_SHIFT, _t)
#define AIC_GLBCFG_AIWT(_t)   AIC_GLBCFG_WT(AIC_GLBCFG_AIWT_SHIFT, _t)
#define AIC_GLBCFG_SIWT(_t)   AIC_GLBCFG_WT(AIC_GLBCFG_SIWT_SHIFT, _t)
#define AIC_GLBCFG_DIWT(_t)   AIC_GLBCFG_WT(AIC_GLBCFG_DIWT_SHIFT, _t)
#define AIC_GLBCFG_SYNC_ACG   BIT32(29)
#define AIC_GLBCFG_EIR_ACG    BIT32(30)
#define AIC_GLBCFG_REG_ACG    BIT32(31)
#define AIC_GLBCFG_WT_MASK    (15)
#define AIC_GLBCFG_WT_64MICRO (7)

#define REG_AIC_WHOAMI        (0x2000)
#define REG_AIC_IACK          (0x2004)
#define REG_AIC_IPI_SET       (0x2008)
#define REG_AIC_IPI_CLR       (0x200C)
#define AIC_IPI_NORMAL        BIT32(0)
#define AIC_IPI_SELF          BIT32(31)
#define REG_AIC_IPI_MASK_SET  (0x2024)
#define REG_AIC_IPI_MASK_CLR  (0x2028)
#define REG_AIC_IPI_DEFER_SET (0x202C)
#define REG_AIC_IPI_DEFER_CLR (0x2030)

#define REG_AIC_TMR_CFG    (0x2010)
#define AIC_TMRCFG_EN      BIT32(0)
#define AIC_TMRCFG_IMD     BIT32(1)
#define AIC_TMRCFG_EMD     BIT32(2)
#define AIC_TMRCFG_SMD     BIT32(3)
#define AIC_TMRCFG_FSL(_s) ((_s) << 4)
#define REG_AIC_TMR_CNT    (0x2014)
#define AIC_TMR_MAX_COUNT  (0xFFFFFFFFU)
#define REG_AIC_TMR_ISR    (0x2018)
#define AIC_TMRISR_PCT     BIT32(0)
#define AIC_TMRISR_ETS     BIT32(1)
#define AIC_TMRISR_STS     BIT32(2)
#define REG_AIC_TMR_ST_SET (0x201C)
#define REG_AIC_TMR_ST_CLR (0x2020)
#define AIC_TMRST_SGT      BIT32(0)
#define AIC_TMRST_TIM      BIT32(1)

#define REG_AIC_PVT_STAMP_CFG (0x2040)
#define AIC_PVT_STAMP_CFG_EN  BIT32(31)
#define REG_AIC_PVT_STAMP_LO  (0x2048)
#define REG_AIC_PVT_STAMP_HI  (0x204C)

#define AIC_SHARED_STAMP_COUNT    (16)
#define REG_AIC_SHARED_STAMP(_n)  (0x6000 + ((_n) * 0x10))
#define AIC_SHARED_STAMP_SLOT(_a) (((_a) - 0x6000) / 0x10)
#define AIC_SHARED_STAMP_OFF(_a)  (((_a) - 0x6000) % 0x10)

#define REG_AIC_EIR_DEST(_n)     (0x3000 + ((_n) * 4))
#define REG_AIC_EIR_SW_SET(_n)   (0x4000 + ((_n) * 4))
#define REG_AIC_EIR_SW_CLR(_n)   (0x4080 + ((_n) * 4))
#define REG_AIC_EIR_MASK_SET(_n) (0x4100 + ((_n) * 4))
#define REG_AIC_EIR_MASK_CLR(_n) (0x4180 + ((_n) * 4))
#define REG_AIC_EIR_INT_RO(_n)   (0x4200 + ((_n) * 4))

#define REG_AIC_WHOAMI_Pn(_n)        (0x5000 + ((_n) * 0x80))
#define REG_AIC_IACK_Pn(_n)          (0x5004 + ((_n) * 0x80))
#define REG_AIC_IPI_SET_Pn(_n)       (0x5008 + ((_n) * 0x80))
#define REG_AIC_IPI_CLR_Pn(_n)       (0x500C + ((_n) * 0x80))
#define REG_AIC_IPI_MASK_SET_Pn(_n)  (0x5024 + ((_n) * 0x80))
#define REG_AIC_IPI_MASK_CLR_Pn(_n)  (0x5028 + ((_n) * 0x80))
#define REG_AIC_IPI_DEFER_SET_Pn(_n) (0x502C + ((_n) * 0x80))
#define REG_AIC_IPI_DEFER_CLR_Pn(_n) (0x5030 + ((_n) * 0x80))

#define kAIC_INT_SPURIOUS (0x00000)
#define kAIC_INT_EXT      (0x10000)
#define kAIC_INT_IPI      (0x40000)
#define kAIC_INT_IPI_NORM (0x40001)
#define kAIC_INT_IPI_SELF (0x40002)
#define kAIC_INT_TMR      (0x70000)
#define kAIC_INT_PVT_TMR  (0x70001)
#define kAIC_INT_EXT_TMR  (0x70002)
#define kAIC_INT_SW_TMR   (0x70004)

#define AIC_INT_EXT(_v) (((_v) & 0x70000) == kAIC_INT_EXT)
#define AIC_INT_IPI(_v) (((_v) & 0x70000) == kAIC_INT_IPI)

#define AIC_INT_EXTID(_v) ((_v) & 0x3FF)

#define AIC_SRC_TO_EIR(_s)     ((_s) >> 5)
#define AIC_SRC_TO_MASK(_s)    (1 << ((_s) & 0x1F))
#define AIC_EIR_TO_SRC(_s, _v) (((_s) << 5) + ((_v) & 0x1F))

#define kAIC_MAX_EXTID       (AIC_INT_COUNT)
#define kAIC_VEC_IPI(_c, _k) (kAIC_MAX_EXTID + ((_c) * 2) + (_k))
#define kAIC_VEC_IPI_SELF    (0)
#define kAIC_VEC_IPI_NORMAL  (1)
#define kAIC_NUM_IPIS(_n)    ((_n) * 2)
#define kAIC_NUM_INTS(_n)    (kAIC_MAX_EXTID + kAIC_NUM_IPIS(_n))

#define kAIC_NUM_EIRS AIC_SRC_TO_EIR(kAIC_MAX_EXTID)

#define AIC_GLBCFG_WT_64MICRO_US (64)

#define kCNTFRQ (24000000)

static inline uint64_t apple_aic_emulate_timer(void)
{ return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), kCNTFRQ, NANOSECONDS_PER_SECOND); }

static inline uint64_t apple_aic_ticks_to_ns(uint64_t ticks)
{ return muldiv64(ticks, NANOSECONDS_PER_SECOND, kCNTFRQ); }

static inline bool aic_test_bit(const uint32_t* addr, long nr)
{ return (qatomic_read(&addr[BIT32_WORD(nr)]) >> (nr & 31)) & 1U; }

static uint64_t apple_aic_wt_ns(uint32_t enc)
{
    uint64_t us = ((uint64_t)AIC_GLBCFG_WT_64MICRO_US << enc) >> AIC_GLBCFG_WT_64MICRO;

    return us * (NANOSECONDS_PER_SECOND / 1000000);
}

static uint64_t apple_aic_diwt_ns(AppleAICState* s)
{
    uint32_t enc = AIC_GLBCFG_WT_GET(qatomic_read(&s->global_cfg), AIC_GLBCFG_DIWT_SHIFT);

    if (enc == 0) { enc = AIC_GLBCFG_WT_64MICRO; }

    return apple_aic_wt_ns(enc);
}

static void apple_aic_deliver(AppleAICState* s)
{
    uint32_t intr      = 0;
    uint32_t potential = 0;
    int      i;

    for (i = 0; i < s->numCPU; i++) {
        uint32_t pending  = qatomic_read(&s->cpus[i].pendingIPI);
        uint32_t ipi_mask = qatomic_read(&s->cpus[i].ipi_mask);

        if ((pending & AIC_IPI_SELF) & ~ipi_mask) { intr |= (1 << i); }
        if ((~ipi_mask & AIC_IPI_NORMAL) && (pending & ((1 << s->numCPU) - 1))) { intr |= (1 << i); }
    }

    i = -1;
    while ((i = find_next_bit32(s->eir_state, s->numIRQ, i + 1)) < s->numIRQ) {
        uint32_t dest;
        if (!aic_test_bit(s->eir_mask, i) && (dest = qatomic_read(&s->eir_dest[i]))) {
            if (((intr & dest) == 0)) {
                /* The interrupt doesn't have a cpu that can process it yet */
                uint32_t cpu = ctz32(dest);

                if (unlikely(cpu >= s->numCPU)) {
                    qemu_log_mask(LOG_GUEST_ERROR, "AIC: vector %d has no reachable destination (0x%x)\n", i, dest);
                    continue;
                }

                intr      |= (1 << cpu);
                potential |= dest;
            }
            else {
                int k;

                potential |= dest;

                for (k = 0; k < s->numCPU; k++) {
                    if (((intr & (1 << k)) == 0) && (potential & (1 << k))) {
                        /*
                         * cpu K isn't in the interrupt list
                         * and can handle some of the previous interrupts
                         */
                        intr |= (1 << k);
                        break;
                    }
                }
            }
        }
    }
    for (i = 0; i < s->numCPU; i++) {
        if (intr & (1 << i)) { qemu_irq_raise(s->cpus[i].irq); }
    }
}

static uint32_t apple_aic_tmr_count(AppleAICCPU* o)
{
    uint64_t now      = apple_aic_emulate_timer();
    uint64_t deadline = qatomic_read__nocheck(&o->tmr_deadline);

    return deadline > now ? MIN(deadline - now, AIC_TMR_MAX_COUNT) : 0;
}

static void apple_aic_tmr_rearm(AppleAICCPU* o)
{
    uint64_t now      = apple_aic_emulate_timer();
    uint64_t deadline = qatomic_read__nocheck(&o->tmr_deadline);

    if (!(qatomic_read(&o->tmr_cfg) & AIC_TMRCFG_EN) || (qatomic_read(&o->tmr_isr) & AIC_TMRISR_PCT)) {
        timer_del(o->tmr);
        return;
    }

    if (deadline <= now) {
        timer_del(o->tmr);
        qatomic_or(&o->tmr_isr, AIC_TMRISR_PCT);
        qemu_irq_raise(o->irq);
        return;
    }

    timer_mod_ns(o->tmr, apple_aic_ticks_to_ns(deadline));
}

static void apple_aic_tmr_tick(void* opaque)
{
    AppleAICCPU* o = opaque;

    if (!(qatomic_read(&o->tmr_cfg) & AIC_TMRCFG_EN)) { return; }

    qatomic_or(&o->tmr_isr, AIC_TMRISR_PCT);
    qemu_irq_raise(o->irq);
}

static void apple_aic_update(AppleAICState* s)
{
    int i;

    for (i = 0; i < s->numCPU; i++) { qatomic_or(&s->cpus[i].pendingIPI, qatomic_xchg(&s->cpus[i].deferredIPI, 0)); }

    apple_aic_deliver(s);
}

static void apple_aic_set_irq(void* opaque, int irq, int level)
{
    AppleAICState* s = opaque;

    trace_aic_set_irq(irq, level);
    if (level) {
        set_bit32_atomic(irq, s->eir_state);
        apple_aic_deliver(s);
    }
    else {
        clear_bit32_atomic(irq, s->eir_state);
    }
}

static void apple_aic_tick(void* opaque)
{
    AppleAICState* s = opaque;

    apple_aic_update(s);

    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + apple_aic_diwt_ns(s));
}

static void apple_aic_reset_state(AppleAICState* s)
{
    int i;

    /* iBoot leaves ACG enabled with all wait timeouts at 64us */
    qatomic_set(&s->global_cfg, AIC_GLBCFG_IEN | AIC_GLBCFG_SYNC_ACG | AIC_GLBCFG_EIR_ACG | AIC_GLBCFG_REG_ACG
                                    | AIC_GLBCFG_AEWT(AIC_GLBCFG_WT_64MICRO) | AIC_GLBCFG_SEWT(AIC_GLBCFG_WT_64MICRO)
                                    | AIC_GLBCFG_AIWT(AIC_GLBCFG_WT_64MICRO) | AIC_GLBCFG_SIWT(AIC_GLBCFG_WT_64MICRO)
                                    | AIC_GLBCFG_DIWT(AIC_GLBCFG_WT_64MICRO));

    /* mask all IRQs */
    for (i = 0; i < s->numEIR; i++) { qatomic_set(&s->eir_mask[i], 0xFFFFFFFF); }

    /* dest default to 0 */
    for (i = 0; i < s->numIRQ; i++) { qatomic_set(&s->eir_dest[i], 0); }

    for (i = 0; i < s->numCPU; i++) {
        AppleAICCPU* o = &s->cpus[i];

        if (o->tmr != NULL) { timer_del(o->tmr); }

        /* mask all IPI */
        qatomic_set(&o->ipi_mask, AIC_IPI_NORMAL | AIC_IPI_SELF);
        qatomic_set(&o->pendingIPI, 0);
        qatomic_set(&o->deferredIPI, 0);
        qatomic_set(&o->tmr_cfg, 0);
        qatomic_set(&o->tmr_isr, 0);
        qatomic_set(&o->tmr_state, 0);
        qatomic_set__nocheck(&o->tmr_deadline, 0);

        qemu_irq_lower(o->irq);
    }

    qatomic_set(&s->pvt_stamp_cfg, 0);

    for (i = 0; i < AIC_SHARED_STAMP_COUNT; i++) { qatomic_set(&s->shared_stamp[i], 0); }
}

static void apple_aic_reset_enter(Object* obj, ResetType type) { apple_aic_reset_state(APPLE_AIC(obj)); }

static void apple_aic_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleAICCPU*   o   = opaque;
    AppleAICState* s   = o->aic;
    uint32_t       val = (uint32_t)data;

    switch (addr) {
        case REG_AIC_RST          : apple_aic_reset_state(s); break;
        case REG_AIC_GLB_CFG      : qatomic_set(&s->global_cfg, val); break;
        case REG_AIC_PVT_STAMP_CFG: qatomic_set(&s->pvt_stamp_cfg, val); break;
        case REG_AIC_TMR_CFG:
            qatomic_set(&o->tmr_cfg, val);
            apple_aic_tmr_rearm(o);
            break;
        case REG_AIC_TMR_CNT:
            qatomic_set__nocheck(&o->tmr_deadline, apple_aic_emulate_timer() + val);
            apple_aic_tmr_rearm(o);
            break;
        case REG_AIC_TMR_ISR:
            qatomic_and(&o->tmr_isr, ~val);
            apple_aic_tmr_rearm(o);
            break;
        case REG_AIC_TMR_ST_SET: qatomic_or(&o->tmr_state, val); break;
        case REG_AIC_TMR_ST_CLR: qatomic_and(&o->tmr_state, ~val); break;
        case REG_AIC_SHARED_STAMP(0)... REG_AIC_SHARED_STAMP(AIC_SHARED_STAMP_COUNT) - 4: {
            uint32_t slot = AIC_SHARED_STAMP_SLOT(addr);

            if (AIC_SHARED_STAMP_OFF(addr) != 0) {
                qemu_log_mask(LOG_UNIMP, "AIC: Write to unsupported shared timestamp reg 0x" HWADDR_FMT_plx "\n", addr);
                break;
            }

            qatomic_set(&s->shared_stamp[slot], val);
            break;
        }
        case REG_AIC_IPI_SET: {
            int i;

            for (i = 0; i < s->numCPU; i++) {
                if (val & (1 << i)) {
                    set_bit32_atomic(o->cpu_id, &s->cpus[i].pendingIPI);
                    if (~qatomic_read(&s->cpus[i].ipi_mask) & AIC_IPI_NORMAL) { qemu_irq_raise(s->cpus[i].irq); }
                }
            }

            if (val & AIC_IPI_SELF) {
                qatomic_or(&o->pendingIPI, AIC_IPI_SELF);
                if (~qatomic_read(&o->ipi_mask) & AIC_IPI_SELF) { qemu_irq_raise(o->irq); }
            }
            break;
        }
        case REG_AIC_IPI_CLR: {
            int i;

            for (i = 0; i < s->numCPU; i++) {
                if (val & (1 << i)) { clear_bit32_atomic(o->cpu_id, &s->cpus[i].pendingIPI); }
            }

            if (val & AIC_IPI_SELF) { qatomic_and(&o->pendingIPI, ~AIC_IPI_SELF); }
            break;
        }
        case REG_AIC_IPI_MASK_SET: qatomic_or(&o->ipi_mask, val & (AIC_IPI_NORMAL | AIC_IPI_SELF)); break;
        case REG_AIC_IPI_MASK_CLR:
            qatomic_and(&o->ipi_mask, ~(val & (AIC_IPI_NORMAL | AIC_IPI_SELF)));
            apple_aic_deliver(s);
            break;
        case REG_AIC_IPI_DEFER_SET: {
            int i;

            for (i = 0; i < s->numCPU; i++) {
                if (val & (1 << i)) { set_bit32_atomic(o->cpu_id, &s->cpus[i].deferredIPI); }
            }

            if (val & AIC_IPI_SELF) { qatomic_or(&o->deferredIPI, AIC_IPI_SELF); }
            break;
        }
        case REG_AIC_IPI_DEFER_CLR: {
            int i;

            for (i = 0; i < s->numCPU; i++) {
                if (val & (1 << i)) { clear_bit32_atomic(o->cpu_id, &s->cpus[i].deferredIPI); }
            }

            if (val & AIC_IPI_SELF) { qatomic_and(&o->deferredIPI, ~AIC_IPI_SELF); }
            break;
        }
        case REG_AIC_EIR_DEST(0)... REG_AIC_EIR_DEST(AIC_INT_COUNT): {
            uint32_t vector = (addr - REG_AIC_EIR_DEST(0)) / 4;
            if (unlikely(vector >= s->numIRQ)) { break; }
            qatomic_set(&s->eir_dest[vector], val);
            break;
        }
        case REG_AIC_EIR_SW_SET(0)... REG_AIC_EIR_SW_SET(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_SW_SET(0)) / 4;
            if (unlikely(eir >= s->numEIR)) { break; }
            qatomic_or(&s->eir_state[eir], val);
            apple_aic_deliver(s);
            break;
        }
        case REG_AIC_EIR_SW_CLR(0)... REG_AIC_EIR_SW_CLR(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_SW_CLR(0)) / 4;
            if (unlikely(eir >= s->numEIR)) { break; }
            qatomic_and(&s->eir_state[eir], ~val);
            break;
        }
        case REG_AIC_EIR_MASK_SET(0)... REG_AIC_EIR_MASK_SET(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_MASK_SET(0)) / 4;
            if (unlikely(eir >= s->numEIR)) { break; }
            qatomic_or(&s->eir_mask[eir], val);
            break;
        }
        case REG_AIC_EIR_MASK_CLR(0)... REG_AIC_EIR_MASK_CLR(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_MASK_CLR(0)) / 4;

            if (unlikely(eir >= s->numEIR)) { break; }

            qatomic_and(&s->eir_mask[eir], ~val);
            apple_aic_deliver(s);
            break;
        }
        case REG_AIC_WHOAMI_Pn(0)... REG_AIC_WHOAMI_Pn(AIC_CPU_COUNT) - 4: {
            uint32_t cpu = ((addr - 0x5000) / 0x80);
            if (unlikely(cpu >= s->numCPU)) { break; }
            addr = addr - 0x5000 + 0x2000 - 0x80 * cpu;
            apple_aic_write(&s->cpus[cpu], addr, data, size);
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP, "AIC: Write to unsupported reg 0x" HWADDR_FMT_plx " cpu %u: 0x%x\n", addr,
                          o->cpu_id, val);
            break;
    }
}

static uint64_t apple_aic_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAICCPU*   o = opaque;
    AppleAICState* s = o->aic;

    switch (addr) {
        case REG_AIC_REV          : return AIC_VERSION;
        case REG_AIC_CAP0         : return (((uint64_t)s->numCPU - 1) << 16) | (s->numIRQ);
        case REG_AIC_GLB_CFG      : return qatomic_read(&s->global_cfg);
        case REG_AIC_PVT_STAMP_CFG: return qatomic_read(&s->pvt_stamp_cfg);
        case REG_AIC_PVT_STAMP_LO:
            if (!(qatomic_read(&s->pvt_stamp_cfg) & AIC_PVT_STAMP_CFG_EN)) { return 0; }
            return apple_aic_emulate_timer() & 0xFFFFFFFF;
        case REG_AIC_PVT_STAMP_HI:
            if (!(qatomic_read(&s->pvt_stamp_cfg) & AIC_PVT_STAMP_CFG_EN)) { return 0; }
            return (apple_aic_emulate_timer() >> 32) & 0xFFFFFFFF;
        case REG_AIC_TMR_CFG   : return qatomic_read(&o->tmr_cfg);
        case REG_AIC_TMR_CNT   : return apple_aic_tmr_count(o);
        case REG_AIC_TMR_ISR   : return qatomic_read(&o->tmr_isr);
        case REG_AIC_TMR_ST_SET:
        case REG_AIC_TMR_ST_CLR: return qatomic_read(&o->tmr_state);
        case REG_AIC_SHARED_STAMP(0)... REG_AIC_SHARED_STAMP(AIC_SHARED_STAMP_COUNT) - 4: {
            uint32_t slot = AIC_SHARED_STAMP_SLOT(addr);

            if (AIC_SHARED_STAMP_OFF(addr) != 0) {
                qemu_log_mask(LOG_UNIMP, "AIC: Read from unsupported shared timestamp reg 0x" HWADDR_FMT_plx "\n",
                              addr);
                break;
            }

            return qatomic_read(&s->shared_stamp[slot]);
        }
        case REG_AIC_WHOAMI: return o->cpu_id;
        case REG_AIC_IACK  : {
            int i;

            qemu_irq_lower(o->irq);

            if (qatomic_read(&o->tmr_isr) & AIC_TMRISR_PCT) {
                qatomic_and(&o->tmr_isr, ~AIC_TMRISR_PCT);
                return kAIC_INT_TMR | kAIC_INT_PVT_TMR;
            }

            if (qatomic_read(&o->pendingIPI) & AIC_IPI_SELF & ~qatomic_read(&o->ipi_mask)) {
                qatomic_and(&o->pendingIPI, ~AIC_IPI_SELF);
                qatomic_or(&o->ipi_mask, AIC_IPI_SELF);
                return kAIC_INT_IPI | kAIC_INT_IPI_SELF;
            }

            if (~qatomic_read(&o->ipi_mask) & AIC_IPI_NORMAL) {
                if (qatomic_read(&o->pendingIPI) & ((1 << s->numCPU) - 1)) {
                    qatomic_and(&o->pendingIPI, ~((1 << s->numCPU) - 1));
                    qatomic_or(&o->ipi_mask, AIC_IPI_NORMAL);
                    return kAIC_INT_IPI | kAIC_INT_IPI_NORM;
                }
            }

            i = -1;
            while ((i = find_next_bit32(s->eir_state, s->numIRQ, i + 1)) < s->numIRQ) {
                if (!(qatomic_read(&s->eir_dest[i]) & (1 << o->cpu_id))) { continue; }
                if (test_and_set_bit32_acquire(i, s->eir_mask)) { continue; }
                return kAIC_INT_EXT | AIC_INT_EXTID(i);
            }
            return kAIC_INT_SPURIOUS;
        }
        case REG_AIC_IPI_MASK_SET                                  :
        case REG_AIC_IPI_MASK_CLR                                  : return qatomic_read(&o->ipi_mask);
        case REG_AIC_EIR_DEST(0)... REG_AIC_EIR_DEST(AIC_INT_COUNT): {
            uint32_t vector = (addr - REG_AIC_EIR_DEST(0)) / 4;

            if (unlikely(vector >= s->numIRQ)) { break; }

            return qatomic_read(&s->eir_dest[vector]);
        }
        case REG_AIC_EIR_MASK_SET(0)... REG_AIC_EIR_MASK_SET(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_MASK_SET(0)) / 4;

            if (unlikely(eir >= s->numEIR)) { break; }

            return qatomic_read(&s->eir_mask[eir]);
        }
        case REG_AIC_EIR_MASK_CLR(0)... REG_AIC_EIR_MASK_CLR(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_MASK_CLR(0)) / 4;

            if (unlikely(eir >= s->numEIR)) { break; }

            return qatomic_read(&s->eir_mask[eir]);
        }
        case REG_AIC_EIR_INT_RO(0)... REG_AIC_EIR_INT_RO(kAIC_NUM_EIRS): {
            uint32_t eir = (addr - REG_AIC_EIR_INT_RO(0)) / 4;

            if (unlikely(eir >= s->numEIR)) { break; }
            return qatomic_read(&s->eir_state[eir]);
        }
        case REG_AIC_WHOAMI_Pn(0)... REG_AIC_WHOAMI_Pn(AIC_CPU_COUNT) - 4: {
            uint32_t cpu = ((addr - 0x5000) / 0x80);

            if (unlikely(cpu >= s->numCPU)) { break; }

            addr = addr - 0x5000 + 0x2000 - 0x80 * cpu;
            return apple_aic_read(&s->cpus[cpu], addr, size);
        }
        default:
            if (addr == s->time_base + 0x20) { return apple_aic_emulate_timer() & 0xFFFFFFFF; }
            else if (addr == s->time_base + 0x28) {
                return (apple_aic_emulate_timer() >> 32) & 0xFFFFFFFF;
            }
            else {
                qemu_log_mask(LOG_UNIMP, "AIC: Read from unsupported reg 0x" HWADDR_FMT_plx " cpu: %u\n", addr,
                              o->cpu_id);
            }
            break;
    }
    return -1;
}

static const MemoryRegionOps apple_aic_ops = {
    .read                  = apple_aic_read,
    .write                 = apple_aic_write,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_aic_realize(DeviceState* dev, struct Error** errp)
{
    AppleAICState* s   = APPLE_AIC(dev);
    SysBusDevice*  sbd = SYS_BUS_DEVICE(dev);
    int            i;

    s->cpus = g_new0(AppleAICCPU, s->numCPU);

    for (i = 0; i < s->numCPU; i++) {
        AppleAICCPU* cpu = &s->cpus[i];

        cpu->aic    = s;
        cpu->cpu_id = i;
        cpu->tmr    = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_aic_tmr_tick, cpu);
        memory_region_init_io(&cpu->iomem, OBJECT(dev), &apple_aic_ops, cpu, TYPE_APPLE_AIC, s->base_size);
        memory_region_enable_lockless_io(&cpu->iomem);
        sysbus_init_mmio(sbd, &cpu->iomem);
        sysbus_init_irq(sbd, &cpu->irq);
    }

    qdev_init_gpio_in(dev, apple_aic_set_irq, s->numIRQ);

    assert_cmpuint(s->numCPU, !=, 0);

    s->eir_mask  = g_new0(uint32_t, s->numEIR);
    s->eir_dest  = g_new0(uint32_t, s->numIRQ);
    s->eir_state = g_new0(uint32_t, s->numEIR);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_aic_tick, dev);
    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + apple_aic_diwt_ns(s));

    msi_nonbroken = true;
}

static void apple_aic_unrealize(DeviceState* dev)
{
    AppleAICState* s = APPLE_AIC(dev);
    int            i;

    for (i = 0; i < s->numCPU; i++) {
        if (s->cpus[i].tmr != NULL) { timer_free(s->cpus[i].tmr); }
    }

    timer_free(s->timer);
}

SysBusDevice* apple_aic_create(uint32_t numCPU, AppleDTNode* node, AppleDTNode* timebase_node)
{
    DeviceState*   dev;
    AppleAICState* s;
    AppleDTProp*   prop;
    hwaddr*        reg;

    dev = qdev_new(TYPE_APPLE_AIC);
    s   = APPLE_AIC(dev);

    s->phandle = apple_dt_get_prop_u32(node, "AAPL,phandle", &error_fatal);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg          = (hwaddr*)prop->data;
    s->base_size = reg[1];

    prop = apple_dt_get_prop(node, "ipid-mask");
    assert_nonnull(prop);
    s->numEIR = prop->len / 4;
    s->numIRQ = s->numEIR * 32;
    s->numCPU = numCPU;

    s->time_base =
        apple_dt_get_prop_u64(timebase_node, "reg", &error_warn) - apple_dt_get_prop_u64(node, "reg", &error_warn);

    apple_dt_set_prop_u32(node, "#main-cpus", numCPU);
    apple_dt_set_prop_u32(node, "#shared-timestamps", 0);

    return SYS_BUS_DEVICE(dev);
}

static void apple_aic_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_aic_reset_enter;

    dc->realize   = apple_aic_realize;
    dc->unrealize = apple_aic_unrealize;
    dc->desc      = "Apple Interrupt Controller";
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleAICState, apple_aic, APPLE_AIC, SYS_BUS_DEVICE)
