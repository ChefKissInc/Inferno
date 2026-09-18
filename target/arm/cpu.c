/*
 * QEMU ARM CPU
 *
 * Copyright (c) 2012 SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "exec/page-vary.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "cpu.h"
#ifdef CONFIG_TCG
    #include "exec/translation-block.h"
    #include "accel/tcg/cpu-ops.h"
#endif /* CONFIG_TCG */
#include "internals.h"
#include "cpu-features.h"
#include "exec/target_page.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#ifdef CONFIG_TCG
#endif /* CONFIG_TCG */
#include "system/tcg.h"
#include "system/hw_accel.h"
#include "kvm_arm.h"
#include "fpu/softfloat.h"
#include "cpregs.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"

static void arm_cpu_set_pc(CPUState* cs, vaddr value)
{
    ARMCPU*      cpu = container_of(cs, ARMCPU, parent_obj);
    CPUARMState* env = &cpu->env;

    if (is_a64(env)) {
        env->pc    = value;
        env->thumb = false;
    }
    else {
        env->regs[15] = value & ~1;
        env->thumb    = value & 1;
    }
}

static vaddr arm_cpu_get_pc(CPUState* cs)
{
    ARMCPU*      cpu = container_of(cs, ARMCPU, parent_obj);
    CPUARMState* env = &cpu->env;

    if (is_a64(env)) { return env->pc; }
    else {
        return env->regs[15];
    }
}

#ifdef CONFIG_TCG
void arm_cpu_synchronize_from_tb(CPUState* cs, const TranslationBlock* tb)
{
    /* The program counter is always up to date with CF_PCREL. */
    if (!(tb_cflags(tb) & CF_PCREL)) {
        CPUARMState* env = cpu_env(cs);
        /*
         * It's OK to look at env for the current mode here, because it's
         * never possible for an AArch64 TB to chain to an AArch32 TB.
         */
        if (is_a64(env)) { env->pc = tb->pc; }
        else {
            env->regs[15] = tb->pc;
        }
    }
}

void arm_restore_state_to_opc(CPUState* cs, const TranslationBlock* tb, const uint64_t* data)
{
    CPUARMState* env = cpu_env(cs);

    if (is_a64(env)) {
        if (tb_cflags(tb) & CF_PCREL) { env->pc = (env->pc & TARGET_PAGE_MASK) | data[0]; }
        else {
            env->pc = data[0];
        }
        env->condexec_bits      = 0;
        env->exception.syndrome = data[2] << ARM_INSN_START_WORD2_SHIFT;
    }
    else {
        if (tb_cflags(tb) & CF_PCREL) { env->regs[15] = (env->regs[15] & TARGET_PAGE_MASK) | data[0]; }
        else {
            env->regs[15] = data[0];
        }
        env->condexec_bits      = data[1];
        env->exception.syndrome = data[2] << ARM_INSN_START_WORD2_SHIFT;
    }
}

int arm_cpu_mmu_index(CPUState* cs, bool ifetch) { return arm_env_mmu_index(cpu_env(cs)); }

#endif /* CONFIG_TCG */

/*
 * With SCTLR_ELx.NMI == 0, IRQ with Superpriority is masked identically with
 * IRQ without Superpriority. Moreover, if the interrupt controller is configured so that
 * FEAT_GICv3_NMI is only set if FEAT_NMI is set, then we won't ever see
 * CPU_INTERRUPT_*NMI anyway. So we might as well accept NMI here
 * unconditionally.
 */
static bool arm_cpu_has_work(CPUState* cs)
{
    ARMCPU* cpu = container_of(cs, ARMCPU, parent_obj);

    return (cpu->power_state != PSCI_OFF)
           && cpu_test_interrupt(cs, CPU_INTERRUPT_FIQ | CPU_INTERRUPT_HARD | CPU_INTERRUPT_NMI | CPU_INTERRUPT_VINMI
                                         | CPU_INTERRUPT_VFNMI | CPU_INTERRUPT_VFIQ | CPU_INTERRUPT_VIRQ
                                         | CPU_INTERRUPT_VSERR | CPU_INTERRUPT_EXITTB);
}

void arm_register_pre_el_change_hook(ARMCPU* cpu, ARMELChangeHookFn* hook, void* opaque)
{
    ARMELChangeHook* entry = g_new0(ARMELChangeHook, 1);

    entry->hook   = hook;
    entry->opaque = opaque;

    QLIST_INSERT_HEAD(&cpu->pre_el_change_hooks, entry, node);
}

void arm_register_el_change_hook(ARMCPU* cpu, ARMELChangeHookFn* hook, void* opaque)
{
    ARMELChangeHook* entry = g_new0(ARMELChangeHook, 1);

    entry->hook   = hook;
    entry->opaque = opaque;

    QLIST_INSERT_HEAD(&cpu->el_change_hooks, entry, node);
}

static void cp_reg_reset(ARMCPRegInfo* ri, ARMCPU* cpu)
{
    /* Reset a single ARMCPRegInfo register */
    if (ri->type & (ARM_CP_SPECIAL_MASK | ARM_CP_ALIAS)) { return; }

    if (ri->resetfn) {
        ri->resetfn(&cpu->env, ri);
        return;
    }

    /* A zero offset is never possible as it would be regs[0]
     * so we use it to indicate that reset is being handled elsewhere.
     * This is basically only used for fields in non-core coprocessors
     * (like the pxa2xx ones).
     */
    if (!ri->fieldoffset) { return; }

    if (cpreg_field_is_64bit(ri)) { CPREG_FIELD64(&cpu->env, ri) = ri->resetvalue; }
    else {
        CPREG_FIELD32(&cpu->env, ri) = ri->resetvalue;
    }
}

static void cp_reg_check_reset(ARMCPRegInfo* ri, ARMCPU* cpu)
{
    /* Purely an assertion check: we've already done reset once,
     * so now check that running the reset for the cpreg doesn't
     * change its value. This traps bugs where two different cpregs
     * both try to reset the same state field but to different values.
     */
    uint64_t oldvalue, newvalue;

    if (ri->type & (ARM_CP_SPECIAL_MASK | ARM_CP_ALIAS | ARM_CP_NO_RAW)) { return; }

    oldvalue = read_raw_cp_reg(&cpu->env, ri);
    cp_reg_reset(ri, cpu);
    newvalue = read_raw_cp_reg(&cpu->env, ri);
    assert(oldvalue == newvalue);
}

static void arm_cpu_reset_hold(Object* obj, ResetType type)
{
    CPUState*              cs  = CPU(obj);
    ARMCPU*                cpu = container_of(cs, ARMCPU, parent_obj);
    ARMCPUClass*           acc = ARM_CPU_GET_CLASS(obj);
    CPUARMState*           env = &cpu->env;
    ARMCPRegTable_it_t     it;
    ARMCPRegTable_pair_ct* ref;

    if (acc->parent_phases.hold) { acc->parent_phases.hold(obj, type); }

    /*
     * Batched invalidations target the other cpus too, and they keep running
     * across our reset, so issue them before the batch is cleared below.
     */
    arm_tlbi_batch_drain(env);

    memset(env, 0, offsetof(CPUARMState, end_reset_fields));

    for (int i = 0; i < NUM_GTIMERS; i++) {
        cpu->gt_irqstate[i] = 0;
        qemu_set_irq(cpu->gt_timer_outputs[i], 0);
    }

    for (ARMCPRegTable_it(it, cpu->cp_regs); !ARMCPRegTable_end_p(it); ARMCPRegTable_next(it)) {
        ref = ARMCPRegTable_ref(it);
        cp_reg_reset(&ref->value, cpu);
    }

    for (ARMCPRegTable_it(it, cpu->cp_regs); !ARMCPRegTable_end_p(it); ARMCPRegTable_next(it)) {
        ref = ARMCPRegTable_ref(it);
        cp_reg_check_reset(&ref->value, cpu);
    }

    env->vfp.xregs[ARM_VFP_FPSID] = cpu->reset_fpsid;
    env->vfp.xregs[ARM_VFP_MVFR0] = cpu->isar.mvfr0;
    env->vfp.xregs[ARM_VFP_MVFR1] = cpu->isar.mvfr1;
    env->vfp.xregs[ARM_VFP_MVFR2] = cpu->isar.mvfr2;

    cpu->power_state = cs->start_powered_off ? PSCI_OFF : PSCI_ON;

    if (arm_feature(env, ARM_FEATURE_IWMMXT)) { env->iwmmxt.cregs[ARM_IWMMXT_wCID] = 0x69051000 | 'Q'; }

    if (arm_feature(env, ARM_FEATURE_AARCH64)) {
        /* 64 bit CPUs always start in 64 bit mode */
        env->aarch64 = true;

        /* Reset into the highest available EL */
        if (arm_feature(env, ARM_FEATURE_EL3)) { env->pstate = PSTATE_MODE_EL3h; }
        else if (arm_feature(env, ARM_FEATURE_EL2)) {
            env->pstate = PSTATE_MODE_EL2h;
        }
        else {
            env->pstate = PSTATE_MODE_EL1h;
        }

        /* Sample rvbar at reset.  */
        env->cp15.rvbar = cpu->rvbar_prop;
        env->pc         = env->cp15.rvbar;

        if (env->aarch64 && cpu_isar_feature(aa64_pauth, cpu)) {
            env->keys.m.lo = cpu->m_key_lo;
            env->keys.m.hi = cpu->m_key_hi;
        }
    }
    else if (arm_feature(env, ARM_FEATURE_V8)) {
        env->cp15.rvbar = cpu->rvbar_prop;
        env->regs[15]   = cpu->rvbar_prop;
    }

    /*
     * If the highest available EL is EL2, AArch32 will start in Hyp
     * mode; otherwise it starts in SVC. Note that if we start in
     * AArch64 then these values in the uncached_cpsr will be ignored.
     */
    if (arm_feature(env, ARM_FEATURE_EL2) && !arm_feature(env, ARM_FEATURE_EL3)) {
        env->uncached_cpsr = ARM_CPU_MODE_HYP;
    }
    else {
        env->uncached_cpsr = ARM_CPU_MODE_SVC;
    }
    env->daif = PSTATE_D | PSTATE_A | PSTATE_I | PSTATE_F;

    /* AArch32 has a hard highvec setting of 0xFFFF0000.  If we are currently
     * executing as AArch32 then check if highvecs are enabled and
     * adjust the PC accordingly.
     */
    if (A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_V) { env->regs[15] = 0xFFFF0000; }

    env->vfp.xregs[ARM_VFP_FPEXC] = 0;

    /* M profile requires that reset clears the exclusive monitor;
     * A profile does not, but clearing it makes more sense than having it
     * set with an exclusive access on address zero.
     */
    arm_clear_exclusive(env);

    set_flush_to_zero(1, &env->vfp.fp_status[FPST_STD]);
    set_flush_inputs_to_zero(1, &env->vfp.fp_status[FPST_STD]);
    set_default_nan_mode(1, &env->vfp.fp_status[FPST_STD]);
    set_default_nan_mode(1, &env->vfp.fp_status[FPST_STD_F16]);
    set_default_nan_mode(1, &env->vfp.fp_status[FPST_ZA]);
    set_default_nan_mode(1, &env->vfp.fp_status[FPST_ZA_F16]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_A32]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_A64]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_ZA]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_STD]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_A32_F16]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_A64_F16]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_ZA_F16]);
    arm_set_default_fp_behaviours(&env->vfp.fp_status[FPST_STD_F16]);
    arm_set_ah_fp_behaviours(&env->vfp.fp_status[FPST_AH]);
    set_flush_to_zero(1, &env->vfp.fp_status[FPST_AH]);
    set_flush_inputs_to_zero(1, &env->vfp.fp_status[FPST_AH]);
    arm_set_ah_fp_behaviours(&env->vfp.fp_status[FPST_AH_F16]);

    if (kvm_enabled()) { kvm_arm_reset_vcpu(cpu); }

    if (tcg_enabled()) {
        hw_breakpoint_update_all(cpu);
        hw_watchpoint_update_all(cpu);

        arm_rebuild_hflags(env);
    }
}

void arm_emulate_firmware_reset(CPUState* cpustate, int target_el)
{
    ARMCPU*      cpu      = container_of(cpustate, ARMCPU, parent_obj);
    CPUARMState* env      = &cpu->env;
    bool         have_el3 = arm_feature(env, ARM_FEATURE_EL3);
    bool         have_el2 = arm_feature(env, ARM_FEATURE_EL2);

    /*
     * Check we have the EL we're aiming for. If that is the
     * highest implemented EL, then cpu_reset has already done
     * all the work.
     */
    switch (target_el) {
        case 3: assert(have_el3); return;
        case 2:
            assert(have_el2);
            if (!have_el3) { return; }
            break;
        case 1:
            if (!have_el3 && !have_el2) { return; }
            break;
        default: assert_not_reached();
    }

    if (have_el3) {
        /*
         * Set the EL3 state so code can run at EL2. This should match
         * the requirements set by Linux in its booting spec.
         */
        if (env->aarch64) {
            env->cp15.scr_el3 |= SCR_RW;
            if (cpu_isar_feature(aa64_pauth, cpu)) { env->cp15.scr_el3 |= SCR_API | SCR_APK; }
            if (cpu_isar_feature(aa64_mte, cpu)) { env->cp15.scr_el3 |= SCR_ATA; }
            if (cpu_isar_feature(aa64_sve, cpu)) {
                env->cp15.cptr_el[3] |= R_CPTR_EL3_EZ_MASK;
                env->vfp.zcr_el[3]    = 0xf;
            }
            if (cpu_isar_feature(aa64_sme, cpu)) {
                env->cp15.cptr_el[3] |= R_CPTR_EL3_ESM_MASK;
                env->cp15.scr_el3    |= SCR_ENTP2;
                env->vfp.smcr_el[3]   = 0xf;
                if (cpu_isar_feature(aa64_sme2, cpu)) { env->vfp.smcr_el[3] |= R_SMCR_EZT0_MASK; }
            }
            if (cpu_isar_feature(aa64_hcx, cpu)) { env->cp15.scr_el3 |= SCR_HXEN; }
            if (cpu_isar_feature(aa64_fgt, cpu)) { env->cp15.scr_el3 |= SCR_FGTEN; }
        }

        if (target_el == 2) {
            /* If the guest is at EL2 then Linux expects the HVC insn to work */
            env->cp15.scr_el3 |= SCR_HCE;
        }

        /* Put CPU into non-secure state */
        env->cp15.scr_el3 |= SCR_NS;
        /* Set NSACR.{CP11,CP10} so NS can access the FPU */
        env->cp15.nsacr |= 3 << 10;
    }

    if (have_el2 && target_el < 2) {
        /* Set EL2 state so code can run at EL1. */
        if (env->aarch64) { env->cp15.hcr_el2 |= HCR_RW; }
    }

    /* Set the CPU to the desired state */
    if (env->aarch64) { env->pstate = aarch64_pstate_mode(target_el, true); }
    else {
        static const uint32_t mode_for_el[] = {
            0,
            ARM_CPU_MODE_SVC,
            ARM_CPU_MODE_HYP,
            ARM_CPU_MODE_SVC,
        };

        cpsr_write(env, mode_for_el[target_el], CPSR_M, CPSRWriteRaw);
    }
}

static void arm_cpu_set_irq(void* opaque, int irq, int level)
{
    ARMCPU*          cpu    = opaque;
    CPUARMState*     env    = &cpu->env;
    CPUState*        cs     = CPU(cpu);
    static const int mask[] = {
        [ARM_CPU_IRQ] = CPU_INTERRUPT_HARD,  [ARM_CPU_FIQ] = CPU_INTERRUPT_FIQ, [ARM_CPU_VIRQ] = CPU_INTERRUPT_VIRQ,
        [ARM_CPU_VFIQ] = CPU_INTERRUPT_VFIQ, [ARM_CPU_NMI] = CPU_INTERRUPT_NMI, [ARM_CPU_VINMI] = CPU_INTERRUPT_VINMI,
    };

    if (!arm_feature(env, ARM_FEATURE_EL2) && (irq == ARM_CPU_VIRQ || irq == ARM_CPU_VFIQ)) {
        /*
         * The interrupt controller might tell us about VIRQ and VFIQ state, but if we don't
         * have EL2 support we don't care. (Unless the guest is doing something
         * silly this will only be calls saying "level is still 0".)
         */
        return;
    }

    if (level) { qatomic_or(&env->irq_line_state, mask[irq]); }
    else {
        qatomic_and(&env->irq_line_state, ~mask[irq]);
    }

    switch (irq) {
        case ARM_CPU_VIRQ : arm_cpu_update_virq(cpu); break;
        case ARM_CPU_VFIQ : arm_cpu_update_vfiq(cpu); break;
        case ARM_CPU_VINMI: arm_cpu_update_vinmi(cpu); break;
        case ARM_CPU_IRQ  :
        case ARM_CPU_FIQ  :
        case ARM_CPU_NMI:
            if (level) { cpu_interrupt(cs, mask[irq]); }
            else {
                cpu_reset_interrupt(cs, mask[irq]);
            }
            break;
        default: assert_not_reached();
    }
}

#ifdef CONFIG_TCG
bool arm_cpu_exec_halt(CPUState* cs)
{
    bool leave_halt = cpu_has_work(cs);

    if (leave_halt) {
        /* We're about to come out of WFI/WFE: disable the WFxT timer */
        ARMCPU* cpu = container_of(cs, ARMCPU, parent_obj);
        if (cpu->wfxt_timer) { timer_del(cpu->wfxt_timer); }
    }
    return leave_halt;
}
#endif

static void arm_wfxt_timer_cb(void* opaque)
{
    ARMCPU*   cpu = opaque;
    CPUState* cs  = CPU(cpu);

    /*
     * We expect the CPU to be halted; this will cause arm_cpu_is_work()
     * to return true (so we will come out of halt even with no other
     * pending interrupt), and the TCG accelerator's cpu_exec_interrupt()
     * function auto-clears the CPU_INTERRUPT_EXITTB flag for us.
     */
    cpu_interrupt(cs, CPU_INTERRUPT_EXITTB);
}

static void aarch64_cpu_dump_state(CPUState* cs, FILE* f, int flags)
{
    ARMCPU*      cpu = container_of(cs, ARMCPU, parent_obj);
    CPUARMState* env = &cpu->env;
    uint32_t     psr = pstate_read(env);
    int          i, j;
    int          el  = arm_current_el(env);
    uint64_t     hcr = arm_hcr_el2_eff(env);
    const char*  ns_status;
    bool         sve;

    qemu_fprintf(f, " PC=%016" PRIx64 " ", env->pc);
    for (i = 0; i < 32; i++) {
        if (i == 31) { qemu_fprintf(f, " SP=%016" PRIx64 "\n", env->xregs[i]); }
        else {
            qemu_fprintf(f, "X%02d=%016" PRIx64 "%s", i, env->xregs[i], (i + 2) % 3 ? " " : "\n");
        }
    }

    if (arm_feature(env, ARM_FEATURE_EL3) && el != 3) { ns_status = env->cp15.scr_el3 & SCR_NS ? "NS " : "S "; }
    else {
        ns_status = "";
    }
    qemu_fprintf(f, "PSTATE=%08x %c%c%c%c %s%cL%d%c", psr, psr & PSTATE_N ? 'N' : '-', psr & PSTATE_Z ? 'Z' : '-',
                 psr & PSTATE_C ? 'C' : '-', psr & PSTATE_V ? 'V' : '-', ns_status, arm_is_guarded(env) ? 'G' : 'E', el,
                 psr & PSTATE_SP ? 'h' : 't');

    if (cpu_isar_feature(aa64_sme, cpu)) {
        qemu_fprintf(f, "  SVCR=%08" PRIx64 " %c%c", env->svcr, (REG_FIELD_EX64(env->svcr, SVCR, ZA) ? 'Z' : '-'),
                     (REG_FIELD_EX64(env->svcr, SVCR, SM) ? 'S' : '-'));
    }
    if (cpu_isar_feature(aa64_bti, cpu)) { qemu_fprintf(f, "  BTYPE=%d", (psr & PSTATE_BTYPE) >> 10); }
    qemu_fprintf(f, "%s%s%s", (hcr & HCR_NV) ? " NV" : "", (hcr & HCR_NV1) ? " NV1" : "",
                 (hcr & HCR_NV2) ? " NV2" : "");
    if (!(flags & CPU_DUMP_FPU)) {
        qemu_fprintf(f, "\n");
        return;
    }
    if (fp_exception_el(env, el) != 0) {
        qemu_fprintf(f, "    FPU disabled\n");
        return;
    }
    qemu_fprintf(f, "     FPCR=%08x FPSR=%08x\n", vfp_get_fpcr(env), vfp_get_fpsr(env));

    if (cpu_isar_feature(aa64_sme, cpu) && REG_FIELD_EX64(env->svcr, SVCR, SM)) {
        sve = sme_exception_el(env, el) == 0;
    }
    else if (cpu_isar_feature(aa64_sve, cpu)) {
        sve = sve_exception_el(env, el) == 0;
    }
    else {
        sve = false;
    }

    if (sve) {
        int zcr_len = sve_vqm1_for_el(env, el);

        for (i = 0; i <= FFR_PRED_NUM; i++) {
            bool eol;
            if (i == FFR_PRED_NUM) {
                qemu_fprintf(f, "FFR=");
                /* It's last, so end the line.  */
                eol = true;
            }
            else {
                qemu_fprintf(f, "P%02d=", i);
                switch (zcr_len) {
                    case 0: eol = i % 8 == 7; break;
                    case 1: eol = i % 6 == 5; break;
                    case 2:
                    case 3: eol = i % 3 == 2; break;
                    default:
                        /* More than one quadword per predicate.  */
                        eol = true;
                        break;
                }
            }
            for (j = zcr_len / 4; j >= 0; j--) {
                int digits;
                if (j * 4 + 4 <= zcr_len + 1) { digits = 16; }
                else {
                    digits = (zcr_len % 4 + 1) * 4;
                }
                qemu_fprintf(f, "%0*" PRIx64 "%s", digits, env->vfp.pregs[i].p[j], j ? ":" : eol ? "\n" : " ");
            }
        }

        if (zcr_len == 0) {
            /*
             * With vl=16, there are only 37 columns per register,
             * so output two registers per line.
             */
            for (i = 0; i < 32; i++) {
                qemu_fprintf(f, "Z%02d=%016" PRIx64 ":%016" PRIx64 "%s", i, env->vfp.zregs[i].d[1],
                             env->vfp.zregs[i].d[0], i & 1 ? "\n" : " ");
            }
        }
        else {
            for (i = 0; i < 32; i++) {
                qemu_fprintf(f, "Z%02d=", i);
                for (j = zcr_len; j >= 0; j--) {
                    qemu_fprintf(f, "%016" PRIx64 ":%016" PRIx64 "%s", env->vfp.zregs[i].d[j * 2 + 1],
                                 env->vfp.zregs[i].d[j * 2 + 0], j ? ":" : "\n");
                }
            }
        }
    }
    else {
        for (i = 0; i < 32; i++) {
            uint64_t* q = aa64_vfp_qreg(env, i);
            qemu_fprintf(f, "Q%02d=%016" PRIx64 ":%016" PRIx64 "%s", i, q[1], q[0], (i & 1 ? "\n" : " "));
        }
    }

    if (cpu_isar_feature(aa64_sme, cpu) && REG_FIELD_EX64(env->svcr, SVCR, ZA) && sme_exception_el(env, el) == 0) {
        int zcr_len  = sve_vqm1_for_el_sm(env, el, true);
        int svl      = (zcr_len + 1) * 16;
        int svl_lg10 = svl < 100 ? 2 : 3;

        for (i = 0; i < svl; i++) {
            qemu_fprintf(f, "ZA[%0*d]=", svl_lg10, i);
            for (j = zcr_len; j >= 0; --j) {
                qemu_fprintf(f, "%016" PRIx64 ":%016" PRIx64 "%c", env->za_state.za[i].d[2 * j + 1],
                             env->za_state.za[i].d[2 * j], j ? ':' : '\n');
            }
        }
    }
}

static void arm_cpu_dump_state(CPUState* cs, FILE* f, int flags)
{
    ARMCPU*      cpu = container_of(cs, ARMCPU, parent_obj);
    CPUARMState* env = &cpu->env;
    int          i;

    if (is_a64(env)) {
        aarch64_cpu_dump_state(cs, f, flags);
        return;
    }

    for (i = 0; i < 16; i++) {
        qemu_fprintf(f, "R%02d=%08x", i, env->regs[i]);
        if ((i % 4) == 3) { qemu_fprintf(f, "\n"); }
        else {
            qemu_fprintf(f, " ");
        }
    }

    uint32_t    psr       = cpsr_read(env);
    const char* ns_status = "";

    if (arm_feature(env, ARM_FEATURE_EL3) && (psr & CPSR_M) != ARM_CPU_MODE_MON) {
        ns_status = env->cp15.scr_el3 & SCR_NS ? "NS " : "S ";
    }

    qemu_fprintf(f, "PSR=%08x %c%c%c%c %c %s%s%d\n", psr, psr & CPSR_N ? 'N' : '-', psr & CPSR_Z ? 'Z' : '-',
                 psr & CPSR_C ? 'C' : '-', psr & CPSR_V ? 'V' : '-', psr & CPSR_T ? 'T' : 'A', ns_status,
                 aarch32_mode_name(psr), (psr & 0x10) ? 32 : 26);

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 0;
        if (cpu_isar_feature(aa32_simd_r32, cpu)) { numvfpregs = 32; }
        else if (cpu_isar_feature(aa32_vfp_simd, cpu)) {
            numvfpregs = 16;
        }
        for (i = 0; i < numvfpregs; i++) {
            uint64_t v = *aa32_vfp_dreg(env, i);
            qemu_fprintf(f, "s%02d=%08x s%02d=%08x d%02d=%016" PRIx64 "\n", i * 2, (uint32_t)v, i * 2 + 1,
                         (uint32_t)(v >> 32), i, v);
        }
        qemu_fprintf(f, "FPSCR: %08x\n", vfp_get_fpscr(env));
    }
}

uint64_t arm_build_mp_affinity(int idx, uint8_t clustersz)
{
    uint32_t Aff1 = idx / clustersz;
    uint32_t Aff0 = idx % clustersz;
    return (Aff1 << ARM_AFF1_SHIFT) | Aff0;
}

uint64_t arm_cpu_mp_affinity(ARMCPU* cpu) { return cpu->mp_affinity; }

static void arm_cpu_initfn(Object* obj)
{
    ARMCPU* cpu = ARM_CPU(obj);

    ARMCPRegTable_init(cpu->cp_regs);

    QLIST_INIT(&cpu->pre_el_change_hooks);
    QLIST_INIT(&cpu->el_change_hooks);

    /* Our inbound IRQ and FIQ lines */
    if (kvm_enabled()) {
        /*
         * VIRQ, VFIQ, NMI, VINMI are unused with KVM but we add
         * them to maintain the same interface as non-KVM CPUs.
         */
        qdev_init_gpio_in(DEVICE(cpu), arm_cpu_kvm_set_irq, 6);
    }
    else {
        qdev_init_gpio_in(DEVICE(cpu), arm_cpu_set_irq, 6);
    }

    qdev_init_gpio_out(DEVICE(cpu), cpu->gt_timer_outputs, ARRAY_SIZE(cpu->gt_timer_outputs));

    qdev_init_gpio_out_named(DEVICE(cpu), &cpu->pmu_interrupt, "pmu-interrupt", 1);

    cpu->psci_version = QEMU_PSCI_VERSION_0_1; /* By default assume PSCI v0.1 */
    cpu->kvm_target   = QEMU_KVM_ARM_TARGET_NONE;

    if (tcg_enabled() || hvf_enabled()) {
        /* TCG and HVF implement PSCI 1.1 */
        cpu->psci_version = QEMU_PSCI_VERSION_1_1;
    }
}

/*
 * 0 means "unset, use the default value". That default might vary depending
 * on the CPU type, and is set in the realize fn.
 */
static const Property arm_cpu_gt_cntfrq_property = DEFINE_PROP_UINT64("cntfrq", ARMCPU, gt_cntfrq_hz, 0);

static const Property arm_cpu_reset_cbar_property = DEFINE_PROP_UINT64("reset-cbar", ARMCPU, reset_cbar, 0);

static const Property arm_cpu_reset_hivecs_property = DEFINE_PROP_BOOL("reset-hivecs", ARMCPU, reset_hivecs, false);

static const Property arm_cpu_has_el2_property = DEFINE_PROP_BOOL("has_el2", ARMCPU, has_el2, true);

static const Property arm_cpu_has_el3_property = DEFINE_PROP_BOOL("has_el3", ARMCPU, has_el3, true);

static const Property arm_cpu_cfgend_property = DEFINE_PROP_BOOL("cfgend", ARMCPU, cfgend, false);

static const Property arm_cpu_has_vfp_property = DEFINE_PROP_BOOL("vfp", ARMCPU, has_vfp, true);

static const Property arm_cpu_has_vfp_d32_property = DEFINE_PROP_BOOL("vfp-d32", ARMCPU, has_vfp_d32, true);

static const Property arm_cpu_has_neon_property = DEFINE_PROP_BOOL("neon", ARMCPU, has_neon, true);

static bool arm_get_pmu(Object* obj, Error** errp)
{
    ARMCPU* cpu = ARM_CPU(obj);

    return cpu->has_pmu;
}

static void arm_set_pmu(Object* obj, bool value, Error** errp)
{
    ARMCPU* cpu = ARM_CPU(obj);

    if (value) {
        if (kvm_enabled() && !kvm_arm_pmu_supported()) {
            error_setg(errp, "'pmu' feature not supported by KVM on this host");
            return;
        }
        set_feature(&cpu->env, ARM_FEATURE_PMU);
    }
    else {
        unset_feature(&cpu->env, ARM_FEATURE_PMU);
    }
    cpu->has_pmu = value;
}

static bool aarch64_cpu_get_aarch64(Object* obj, Error** errp)
{
    ARMCPU* cpu = ARM_CPU(obj);

    return arm_feature(&cpu->env, ARM_FEATURE_AARCH64);
}

static void aarch64_cpu_set_aarch64(Object* obj, bool value, Error** errp)
{
    ARMCPU* cpu = ARM_CPU(obj);

    /*
     * At this time, this property is only allowed if KVM is enabled.  This
     * restriction allows us to avoid fixing up functionality that assumes a
     * uniform execution state like do_interrupt.
     */
    if (value == false) {
        if (!kvm_enabled() || !kvm_arm_aarch32_supported()) {
            error_setg(errp, "'aarch64' feature cannot be disabled "
                             "unless KVM is enabled and 32-bit EL1 "
                             "is supported");
            return;
        }
        unset_feature(&cpu->env, ARM_FEATURE_AARCH64);
    }
    else {
        set_feature(&cpu->env, ARM_FEATURE_AARCH64);
    }
}

static uint64_t gt_gcd(uint64_t a, uint64_t b)
{
    while (b != 0) {
        uint64_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

void gt_derive_tick_ratio(ARMCPU* cpu)
{
    uint64_t g = gt_gcd(cpu->gt_cntfrq_hz, NANOSECONDS_PER_SECOND);

    cpu->gt_tick_num = cpu->gt_cntfrq_hz / g;
    cpu->gt_tick_den = NANOSECONDS_PER_SECOND / g;
}

uint64_t gt_ns_to_ticks(ARMCPU* cpu, uint64_t ns)
{
    if (ns <= UINT64_MAX / cpu->gt_tick_num) { return ns * cpu->gt_tick_num / cpu->gt_tick_den; }
    return muldiv64(ns, cpu->gt_cntfrq_hz, NANOSECONDS_PER_SECOND);
}

uint64_t gt_ticks_to_ns(ARMCPU* cpu, uint64_t ticks)
{
    if (ticks <= UINT64_MAX / cpu->gt_tick_den) { return ticks * cpu->gt_tick_den / cpu->gt_tick_num; }
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, cpu->gt_cntfrq_hz);
}

int64_t gt_ticks_to_ns_ceil(ARMCPU* cpu, uint64_t ticks)
{
    uint64_t ns = gt_ticks_to_ns(cpu, ticks);

    if (gt_ns_to_ticks(cpu, ns) < ticks) { ns++; }
    return ns > INT64_MAX ? INT64_MAX : ns;
}

static void arm_cpu_propagate_feature_implications(ARMCPU* cpu)
{
    CPUARMState* env     = &cpu->env;
    bool         no_aa32 = false;

    /*
     * Some features automatically imply others: set the feature
     * bits explicitly for these cases.
     */

    if (arm_feature(env, ARM_FEATURE_V8)) { set_feature(env, ARM_FEATURE_V7VE); }

    /*
     * There exist AArch64 cpus without AArch32 support.  When KVM
     * queries ID_ISAR0_EL1 on such a host, the value is UNKNOWN.
     * Similarly, we cannot check ID_AA64PFR0 without AArch64 support.
     * As a general principle, we also do not make ID register
     * consistency checks anywhere unless using TCG, because only
     * for TCG would a consistency-check failure be a QEMU bug.
     */
    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)) { no_aa32 = !cpu_isar_feature(aa64_aa32, cpu); }

    if (arm_feature(env, ARM_FEATURE_V7VE)) {
        /*
         * v7 Virtualization Extensions. In real hardware this implies
         * EL2 and also the presence of the Security Extensions.
         * For QEMU, for backwards-compatibility we implement some
         * CPUs or CPU configs which have no actual EL2 or EL3 but do
         * include the various other features that V7VE implies.
         * Presence of EL2 itself is ARM_FEATURE_EL2, and of the
         * Security Extensions is ARM_FEATURE_EL3.
         */
        assert(!tcg_enabled() || no_aa32 || cpu_isar_feature(aa32_arm_div, cpu));
        set_feature(env, ARM_FEATURE_LPAE);
        set_feature(env, ARM_FEATURE_V7);
    }
    if (arm_feature(env, ARM_FEATURE_V7)) {
        set_feature(env, ARM_FEATURE_VAPA);
        set_feature(env, ARM_FEATURE_THUMB2);
        set_feature(env, ARM_FEATURE_MPIDR);
        set_feature(env, ARM_FEATURE_V6K);

        /*
         * Always define VBAR for V7 CPUs even if it doesn't exist in
         * non-EL3 configs. This is needed by some legacy boards.
         */
        set_feature(env, ARM_FEATURE_VBAR);
    }
    if (arm_feature(env, ARM_FEATURE_V6K)) {
        set_feature(env, ARM_FEATURE_V6);
        set_feature(env, ARM_FEATURE_MVFR);
    }
    if (arm_feature(env, ARM_FEATURE_V6)) {
        set_feature(env, ARM_FEATURE_V5);
        assert(!tcg_enabled() || no_aa32 || cpu_isar_feature(aa32_jazelle, cpu));
        set_feature(env, ARM_FEATURE_AUXCR);
    }
    if (arm_feature(env, ARM_FEATURE_V5)) { set_feature(env, ARM_FEATURE_V4T); }
    if (arm_feature(env, ARM_FEATURE_LPAE)) { set_feature(env, ARM_FEATURE_V7MP); }
    if (arm_feature(env, ARM_FEATURE_CBAR_RO)) { set_feature(env, ARM_FEATURE_CBAR); }
    if (arm_feature(env, ARM_FEATURE_THUMB2)) { set_feature(env, ARM_FEATURE_THUMB_DSP); }
}

static void arm_cpu_post_init(Object* obj)
{
    ARMCPU* cpu = ARM_CPU(obj);

    /*
     * Some features imply others. Figure this out now, because we
     * are going to look at the feature bits in deciding which
     * properties to add.
     */
    arm_cpu_propagate_feature_implications(cpu);

    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)) {
        object_property_add_bool(obj, "aarch64", aarch64_cpu_get_aarch64, aarch64_cpu_set_aarch64);
        object_property_set_description(obj, "aarch64",
                                        "Set on/off to enable/disable aarch64 "
                                        "execution state ");
    }

    if (arm_feature(&cpu->env, ARM_FEATURE_CBAR) || arm_feature(&cpu->env, ARM_FEATURE_CBAR_RO)) {
        qdev_property_add_static(DEVICE(obj), &arm_cpu_reset_cbar_property);
    }

    qdev_property_add_static(DEVICE(obj), &arm_cpu_reset_hivecs_property);

    if (arm_feature(&cpu->env, ARM_FEATURE_V8)) {
        object_property_add_uint64_ptr(obj, "rvbar", &cpu->rvbar_prop, OBJ_PROP_FLAG_READWRITE);
    }

    if (arm_feature(&cpu->env, ARM_FEATURE_EL3)) {
        /* Add the has_el3 state CPU property only if EL3 is allowed.  This will
         * prevent "has_el3" from existing on CPUs which cannot support EL3.
         */
        qdev_property_add_static(DEVICE(obj), &arm_cpu_has_el3_property);

        object_property_add_link(obj, "secure-memory", TYPE_MEMORY_REGION, (Object**)&cpu->secure_memory,
                                 qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);
    }

    if (arm_feature(&cpu->env, ARM_FEATURE_EL2)) { qdev_property_add_static(DEVICE(obj), &arm_cpu_has_el2_property); }

    if (arm_feature(&cpu->env, ARM_FEATURE_PMU)) {
        cpu->has_pmu = true;
        object_property_add_bool(obj, "pmu", arm_get_pmu, arm_set_pmu);
    }

    /*
     * Allow user to turn off VFP and Neon support, but only for TCG --
     * KVM does not currently allow us to lie to the guest about its
     * ID/feature registers, so the guest always sees what the host has.
     */
    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)) {
        if (cpu_isar_feature(aa64_fp_simd, cpu)) {
            cpu->has_vfp     = true;
            cpu->has_vfp_d32 = true;
            if (tcg_enabled()) { qdev_property_add_static(DEVICE(obj), &arm_cpu_has_vfp_property); }
        }
    }
    else if (cpu_isar_feature(aa32_vfp, cpu)) {
        cpu->has_vfp = true;
        if (tcg_enabled()) { qdev_property_add_static(DEVICE(obj), &arm_cpu_has_vfp_property); }
        if (cpu_isar_feature(aa32_simd_r32, cpu)) {
            cpu->has_vfp_d32 = true;
            /*
             * The permitted values of the SIMDReg bits [3:0] on
             * Armv8-A are either 0b0000 and 0b0010. On such CPUs,
             * make sure that has_vfp_d32 can not be set to false.
             */
            if ((tcg_enabled()) && !(arm_feature(&cpu->env, ARM_FEATURE_V8))) {
                qdev_property_add_static(DEVICE(obj), &arm_cpu_has_vfp_d32_property);
            }
        }
    }

    if (arm_feature(&cpu->env, ARM_FEATURE_NEON)) {
        cpu->has_neon = true;
        if (tcg_enabled()) { qdev_property_add_static(DEVICE(obj), &arm_cpu_has_neon_property); }
    }

    /* Not DEFINE_PROP_UINT32: we want this to be settable after realize */
    object_property_add_uint32_ptr(obj, "psci-conduit", &cpu->psci_conduit, OBJ_PROP_FLAG_READWRITE);

    if (arm_feature(&cpu->env, ARM_FEATURE_GENERIC_TIMER)) {
        qdev_property_add_static(DEVICE(cpu), &arm_cpu_gt_cntfrq_property);
    }

    if (kvm_enabled()) { kvm_arm_add_vcpu_properties(cpu); }

    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64) && cpu_isar_feature(aa64_mte, cpu)) {
        object_property_add_link(obj, "tag-memory", TYPE_MEMORY_REGION, (Object**)&cpu->tag_memory,
                                 qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);

        if (arm_feature(&cpu->env, ARM_FEATURE_EL3)) {
            object_property_add_link(obj, "secure-tag-memory", TYPE_MEMORY_REGION, (Object**)&cpu->secure_tag_memory,
                                     qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);
        }
    }

    qdev_property_add_static(DEVICE(obj), &arm_cpu_cfgend_property);
}

static void arm_cpu_finalizefn(Object* obj)
{
    ARMCPU*          cpu = ARM_CPU(obj);
    ARMELChangeHook *hook, *next;

    ARMCPRegTable_clear(cpu->cp_regs);

    QLIST_FOREACH_SAFE (hook, &cpu->pre_el_change_hooks, node, next) {
        QLIST_REMOVE(hook, node);
        g_free(hook);
    }
    QLIST_FOREACH_SAFE (hook, &cpu->el_change_hooks, node, next) {
        QLIST_REMOVE(hook, node);
        g_free(hook);
    }

    if (cpu->pmu_timer) { timer_free(cpu->pmu_timer); }
    if (cpu->wfxt_timer) { timer_free(cpu->wfxt_timer); }
}

void arm_cpu_finalize_features(ARMCPU* cpu, Error** errp)
{
    Error* local_err = NULL;

    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)) {
        arm_cpu_sve_finalize(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }

        /*
         * FEAT_SME is not architecturally dependent on FEAT_SVE (unless
         * FEAT_SME_FA64 is present). However our implementation currently
         * assumes it, so if the user asked for sve=off then turn off SME also.
         * (KVM doesn't currently support SME at all.)
         */
        if (cpu_isar_feature(aa64_sme, cpu) && !cpu_isar_feature(aa64_sve, cpu)) {
            object_property_set_bool(OBJECT(cpu), "sme", false, &error_abort);
        }

        arm_cpu_sme_finalize(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }

        arm_cpu_pauth_finalize(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }

        arm_cpu_lpa2_finalize(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }
    }

    if (kvm_enabled()) {
        kvm_arm_steal_time_finalize(cpu, &local_err);
        if (local_err != NULL) {
            error_propagate(errp, local_err);
            return;
        }
    }
}

static void arm_cpu_realizefn(DeviceState* dev, Error** errp)
{
    CPUState*        cs        = CPU(dev);
    ARMCPU*          cpu       = ARM_CPU(dev);
    ARMISARegisters* isar      = &cpu->isar;
    ARMCPUClass*     acc       = ARM_CPU_GET_CLASS(dev);
    CPUARMState*     env       = &cpu->env;
    Error*           local_err = NULL;

#ifdef CONFIG_TCG
    /* Use pc-relative instructions in system-mode */
    tcg_cflags_set(cs, CF_PCREL);
#endif

    /* If we needed to query the host kernel for the CPU features
     * then it's possible that might have failed in the initfn, but
     * this is the first point where we can report it.
     */
    if (cpu->host_cpu_probe_failed) {
        if (!hwaccel_enabled()) {
            error_setg(errp, "The 'host' CPU type can only be used with hwaccel");
        }
        else {
            error_setg(errp, "Failed to retrieve host CPU features");
        }
        return;
    }

    if (!tcg_enabled()) {
        /*
         * We assume that no accelerator except TCG can handle these features,
         * because Arm hardware virtualization can't virtualize them.
         *
         * Catch all the cases which might cause us to create more than one
         * address space for the CPU (otherwise we will assert() later in
         * cpu_address_space_init()).
         */
        if (cpu->has_el3) {
            error_setg(errp, "Cannot enable %s when guest CPU has EL3 enabled", current_accel_name());
            return;
        }
        if (cpu->tag_memory) {
            error_setg(errp, "Cannot enable %s when guest CPUs has MTE enabled", current_accel_name());
            return;
        }
    }

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    arm_cpu_finalize_features(cpu, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    if (!cpu->gt_cntfrq_hz) {
        error_setg(errp, "The CPU has no GTimer frequency set.");
        return;
    }

    gt_derive_tick_ratio(cpu);

    {
        cpu->gt_timer[GTIMER_PHYS]       = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_ptimer_cb, cpu);
        cpu->gt_timer[GTIMER_VIRT]       = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_vtimer_cb, cpu);
        cpu->gt_timer[GTIMER_HYP]        = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_htimer_cb, cpu);
        cpu->gt_timer[GTIMER_SEC]        = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_stimer_cb, cpu);
        cpu->gt_timer[GTIMER_HYPVIRT]    = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_hvtimer_cb, cpu);
        cpu->gt_timer[GTIMER_S_EL2_PHYS] = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_sel2timer_cb, cpu);
        cpu->gt_timer[GTIMER_S_EL2_VIRT] = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_gt_sel2vtimer_cb, cpu);
    }

    if (arm_feature(env, ARM_FEATURE_AARCH64) && cpu->has_vfp != cpu->has_neon) {
        /*
         * This is an architectural requirement for AArch64; AArch32 is
         * more flexible and permits VFP-no-Neon and Neon-no-VFP.
         */
        error_setg(errp, "AArch64 CPUs must have both VFP and Neon or neither");
        return;
    }

    if (cpu->has_vfp_d32 != cpu->has_neon) {
        error_setg(errp, "ARM CPUs must have both VFP-D32 and Neon or neither");
        return;
    }

    if (!cpu->has_vfp_d32) {
        uint32_t u;

        u               = cpu->isar.mvfr0;
        u               = REG_FIELD_DP32(u, MVFR0, SIMDREG, 1); /* 16 registers */
        cpu->isar.mvfr0 = u;
    }

    if (!cpu->has_vfp) {
        uint32_t u;

        FIELD_DP64_IDREG(isar, ID_AA64ISAR1, JSCVT, 0);

        FIELD_DP64_IDREG(isar, ID_AA64PFR0, FP, 0xf);

        u = GET_IDREG(isar, ID_ISAR6);
        u = REG_FIELD_DP32(u, ID_ISAR6, JSCVT, 0);
        u = REG_FIELD_DP32(u, ID_ISAR6, BF16, 0);
        SET_IDREG(isar, ID_ISAR6, u);

        u               = cpu->isar.mvfr0;
        u               = REG_FIELD_DP32(u, MVFR0, FPSP, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPDP, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPDIVIDE, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPSQRT, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPROUND, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPTRAP, 0);
        u               = REG_FIELD_DP32(u, MVFR0, FPSHVEC, 0);
        cpu->isar.mvfr0 = u;

        u               = cpu->isar.mvfr1;
        u               = REG_FIELD_DP32(u, MVFR1, FPFTZ, 0);
        u               = REG_FIELD_DP32(u, MVFR1, FPDNAN, 0);
        u               = REG_FIELD_DP32(u, MVFR1, FPHP, 0);
        cpu->isar.mvfr1 = u;

        u               = cpu->isar.mvfr2;
        u               = REG_FIELD_DP32(u, MVFR2, FPMISC, 0);
        cpu->isar.mvfr2 = u;
    }

    if (!cpu->has_neon) {
        uint64_t t;
        uint32_t u;

        unset_feature(env, ARM_FEATURE_NEON);

        t = GET_IDREG(isar, ID_AA64ISAR0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, AES, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, SHA1, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, SHA2, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, SHA3, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, SM3, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, SM4, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR0, DP, 0);
        SET_IDREG(isar, ID_AA64ISAR0, t);

        t = GET_IDREG(isar, ID_AA64ISAR1);
        t = REG_FIELD_DP64(t, ID_AA64ISAR1, FCMA, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR1, BF16, 0);
        t = REG_FIELD_DP64(t, ID_AA64ISAR1, I8MM, 0);
        SET_IDREG(isar, ID_AA64ISAR1, t);

        FIELD_DP64_IDREG(isar, ID_AA64PFR0, ADVSIMD, 0xf);

        u = GET_IDREG(isar, ID_ISAR5);
        u = REG_FIELD_DP32(u, ID_ISAR5, AES, 0);
        u = REG_FIELD_DP32(u, ID_ISAR5, SHA1, 0);
        u = REG_FIELD_DP32(u, ID_ISAR5, SHA2, 0);
        u = REG_FIELD_DP32(u, ID_ISAR5, RDM, 0);
        u = REG_FIELD_DP32(u, ID_ISAR5, VCMA, 0);
        SET_IDREG(isar, ID_ISAR5, u);

        u = GET_IDREG(isar, ID_ISAR6);
        u = REG_FIELD_DP32(u, ID_ISAR6, DP, 0);
        u = REG_FIELD_DP32(u, ID_ISAR6, FHM, 0);
        u = REG_FIELD_DP32(u, ID_ISAR6, BF16, 0);
        u = REG_FIELD_DP32(u, ID_ISAR6, I8MM, 0);
        SET_IDREG(isar, ID_ISAR6, u);

        u               = cpu->isar.mvfr1;
        u               = REG_FIELD_DP32(u, MVFR1, SIMDLS, 0);
        u               = REG_FIELD_DP32(u, MVFR1, SIMDINT, 0);
        u               = REG_FIELD_DP32(u, MVFR1, SIMDSP, 0);
        u               = REG_FIELD_DP32(u, MVFR1, SIMDHP, 0);
        cpu->isar.mvfr1 = u;

        u               = cpu->isar.mvfr2;
        u               = REG_FIELD_DP32(u, MVFR2, SIMDMISC, 0);
        cpu->isar.mvfr2 = u;
    }

    if (!cpu->has_neon && !cpu->has_vfp) {
        uint32_t u;

        FIELD_DP64_IDREG(isar, ID_AA64ISAR0, FHM, 0);

        FIELD_DP64_IDREG(isar, ID_AA64ISAR1, FRINTTS, 0);

        u               = cpu->isar.mvfr0;
        u               = REG_FIELD_DP32(u, MVFR0, SIMDREG, 0);
        cpu->isar.mvfr0 = u;

        /* Despite the name, this field covers both VFP and Neon */
        u               = cpu->isar.mvfr1;
        u               = REG_FIELD_DP32(u, MVFR1, SIMDFMAC, 0);
        cpu->isar.mvfr1 = u;
    }

    if (!TARGET_PAGE_BITS) {
        int pagebits;
        if (arm_feature(env, ARM_FEATURE_V7)) { pagebits = 12; }
        else {
            /*
             * For CPUs which might have tiny 1K pages, or which have an
             * MPU and might have small region sizes, stick with 1K pages.
             */
            pagebits = 10;
        }
        if (!set_preferred_target_page_bits(pagebits)) {
            /*
             * This can only ever happen for hotplugging a CPU, or if
             * the board code incorrectly creates a CPU which it has
             * promised via minimum_page_size that it will not.
             */
            error_setg(errp, "This CPU requires a smaller page size "
                             "than the system is using");
            return;
        }
    }

    /* This cpu-id-to-MPIDR affinity is used only for TCG; KVM will override it.
     * We don't support setting cluster ID ([16..23]) (known as Aff2
     * in later ARM ARM versions), or any of the higher affinity level fields,
     * so these bits always RAZ.
     */
    if (cpu->mp_affinity == ARM64_AFFINITY_INVALID) {
        cpu->mp_affinity = arm_build_mp_affinity(cs->cpu_index, ARM_DEFAULT_CPUS_PER_CLUSTER);
    }

    if (cpu->reset_hivecs) { cpu->reset_sctlr |= (1 << 13); }

    if (cpu->cfgend) {
        if (arm_feature(env, ARM_FEATURE_V7)) { cpu->reset_sctlr |= SCTLR_EE; }
        else {
            cpu->reset_sctlr |= SCTLR_B;
        }
    }

    if (!cpu->has_el3) {
        /* If the has_el3 CPU property is disabled then we need to disable the
         * feature.
         */
        unset_feature(env, ARM_FEATURE_EL3);

        /*
         * Disable the security extension feature bits in the processor
         * feature registers as well.
         */
        FIELD_DP32_IDREG(isar, ID_PFR1, SECURITY, 0);
        FIELD_DP32_IDREG(isar, ID_DFR0, COPSDBG, 0);
        FIELD_DP64_IDREG(isar, ID_AA64PFR0, EL3, 0);

        /* Disable the realm management extension, which requires EL3. */
        FIELD_DP64_IDREG(isar, ID_AA64PFR0, RME, 0);
    }

    if (!cpu->has_el2) { unset_feature(env, ARM_FEATURE_EL2); }

    if (!cpu->has_pmu) { unset_feature(env, ARM_FEATURE_PMU); }
    if (arm_feature(env, ARM_FEATURE_PMU)) {
        pmu_init(cpu);

        if (!kvm_enabled()) {
            arm_register_pre_el_change_hook(cpu, &pmu_pre_el_change, 0);
            arm_register_el_change_hook(cpu, &pmu_post_el_change, 0);
        }

        cpu->pmu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_pmu_timer_cb, cpu);
    }
    else {
        FIELD_DP64_IDREG(isar, ID_AA64DFR0, PMUVER, 0);
        FIELD_DP32_IDREG(isar, ID_DFR0, PERFMON, 0);
        cpu->pmceid0 = 0;
        cpu->pmceid1 = 0;
    }

    if (!arm_feature(env, ARM_FEATURE_EL2)) {
        /*
         * Disable the hypervisor feature bits in the processor feature
         * registers if we don't have EL2.
         */
        FIELD_DP64_IDREG(isar, ID_AA64PFR0, EL2, 0);
        FIELD_DP32_IDREG(isar, ID_PFR1, VIRTUALIZATION, 0);
    }

    if (cpu_isar_feature(aa64_mte, cpu)) {
        /*
         * The architectural range of GM blocksize is 2-6, however qemu
         * doesn't support blocksize of 2 (see HELPER(ldgm)).
         */
        if (tcg_enabled()) { assert(cpu->gm_blocksize >= 3 && cpu->gm_blocksize <= 6); }

        /*
         * If we run with TCG and do not have tag-memory provided by
         * the machine, then reduce MTE support to instructions enabled at EL0.
         * This matches Cortex-A710 BROADCASTMTE input being LOW.
         */
        if (tcg_enabled() && cpu->tag_memory == NULL) { FIELD_DP64_IDREG(isar, ID_AA64PFR1, MTE, 1); }

        /*
         * If MTE is supported by the host, however it should not be
         * enabled on the guest (i.e mte=off), clear guest's MTE bits."
         */
        if (kvm_enabled() && !cpu->kvm_mte) { FIELD_DP64_IDREG(isar, ID_AA64PFR1, MTE, 0); }
    }

    if (tcg_enabled() && cpu_isar_feature(aa64_wfxt, cpu)) {
        cpu->wfxt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, arm_wfxt_timer_cb, cpu);
    }

    if (tcg_enabled()) {
        /*
         * Don't report some architectural features in the ID registers
         * where TCG does not yet implement it (not even a minimal
         * stub version). This avoids guests falling over when they
         * try to access the non-existent system registers for them.
         */
        /* FEAT_SPE (Statistical Profiling Extension) */
        FIELD_DP64_IDREG(isar, ID_AA64DFR0, PMSVER, 0);
        /* FEAT_TRBE (Trace Buffer Extension) */
        FIELD_DP64_IDREG(isar, ID_AA64DFR0, TRACEBUFFER, 0);
        /* FEAT_TRF (Self-hosted Trace Extension) */
        FIELD_DP64_IDREG(isar, ID_AA64DFR0, TRACEFILT, 0);
        FIELD_DP32_IDREG(isar, ID_DFR0, TRACEFILT, 0);
        /* Trace Macrocell system register access */
        FIELD_DP64_IDREG(isar, ID_AA64DFR0, TRACEVER, 0);
        FIELD_DP32_IDREG(isar, ID_DFR0, COPTRC, 0);
        /* Memory mapped trace */
        FIELD_DP32_IDREG(isar, ID_DFR0, MMAPTRC, 0);
        /* FEAT_AMU (Activity Monitors Extension) */
        FIELD_DP64_IDREG(isar, ID_AA64PFR0, AMU, 0);
        FIELD_DP32_IDREG(isar, ID_PFR0, AMU, 0);
        /* FEAT_MPAM (Memory Partitioning and Monitoring Extension) */
        FIELD_DP64_IDREG(isar, ID_AA64PFR0, MPAM, 0);
    }

    if (arm_feature(env, ARM_FEATURE_EL3)) { set_feature(env, ARM_FEATURE_VBAR); }

    if (tcg_enabled() && cpu_isar_feature(aa64_rme, cpu)) {
        arm_register_el_change_hook(cpu, &gt_rme_post_el_change, 0);
    }

    register_cp_regs_for_features(cpu);
    arm_cpu_register_gdb_regs_for_features(cpu);
    arm_cpu_register_gdb_commands(cpu);

    init_cpreg_list(cpu);

    MachineState* ms         = MACHINE(qdev_get_machine());
    unsigned int  smp_cpus   = ms->smp.cpus;
    bool          has_secure = cpu->has_el3;

    /*
     * We must set cs->num_ases to the final value before
     * the first call to cpu_address_space_init.
     */
    if (cpu->tag_memory != NULL) { cs->num_ases = 3 + has_secure; }
    else {
        cs->num_ases = 1 + has_secure;
    }

    if (has_secure) {
        if (!cpu->secure_memory) { cpu->secure_memory = cs->memory; }
        cpu_address_space_init(cs, ARMASIdx_S, "cpu-secure-memory", cpu->secure_memory);
    }

    if (cpu->tag_memory != NULL) {
        cpu_address_space_init(cs, ARMASIdx_TagNS, "cpu-tag-memory", cpu->tag_memory);
        if (has_secure) { cpu_address_space_init(cs, ARMASIdx_TagS, "cpu-tag-memory", cpu->secure_tag_memory); }
    }

    cpu_address_space_init(cs, ARMASIdx_NS, "cpu-memory", cs->memory);

    /* No core_count specified, default to smp_cpus. */
    if (cpu->core_count == -1) { cpu->core_count = smp_cpus; }

    if (tcg_enabled()) {
        int dcz_blocklen = 4 << cpu->dcz_blocksize;

        /*
         * We only support DCZ blocklen that fits on one page.
         *
         * Architectually this is always true.  However TARGET_PAGE_SIZE
         * is variable and, for compatibility with -machine virt-2.7,
         * is only 1KiB, as an artifact of legacy ARMv5 subpage support.
         * But even then, while the largest architectural DCZ blocklen
         * is 2KiB, no cpu actually uses such a large blocklen.
         */
        assert(dcz_blocklen <= TARGET_PAGE_SIZE);

        /*
         * We only support DCZ blocksize >= 2*TAG_GRANULE, which is to say
         * both nibbles of each byte storing tag data may be written at once.
         * Since TAG_GRANULE is 16, this means that blocklen must be >= 32.
         */
        if (cpu_isar_feature(aa64_mte, cpu)) { assert(dcz_blocklen >= 2 * TAG_GRANULE); }
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    acc->parent_realize(dev, errp);
}

static ObjectClass* arm_cpu_class_by_name(const char* cpu_model)
{
    ObjectClass* oc;
    char*        typename;
    char**       cpuname;
    const char*  cpunamestr;

    cpuname    = g_strsplit(cpu_model, ",", 1);
    cpunamestr = cpuname[0];
    typename   = g_strdup_printf(ARM_CPU_TYPE_NAME("%s"), cpunamestr);
    oc         = object_class_by_name(typename);
    g_strfreev(cpuname);
    g_free(typename);

    return oc;
}

static const Property arm_cpu_properties[] = {
    DEFINE_PROP_UINT64("midr", ARMCPU, midr, 0),
    DEFINE_PROP_UINT64("mp-affinity", ARMCPU, mp_affinity, ARM64_AFFINITY_INVALID),
    DEFINE_PROP_INT32("core-count", ARMCPU, core_count, -1),
};

static const gchar* arm_gdb_arch_name(CPUState* cs)
{
    ARMCPU*      cpu = container_of(cs, ARMCPU, parent_obj);
    CPUARMState* env = &cpu->env;

    if (arm_gdbstub_is_aarch64(cpu)) { return "aarch64"; }
    if (arm_feature(env, ARM_FEATURE_IWMMXT)) { return "iwmmxt"; }
    return "arm";
}

static const char* arm_gdb_get_core_xml_file(CPUState* cs)
{
    ARMCPU* cpu = container_of(cs, ARMCPU, parent_obj);

    if (arm_gdbstub_is_aarch64(cpu)) { return "aarch64-core.xml"; }
    return "arm-core.xml";
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps arm_sysemu_ops = {
    .has_work                  = arm_cpu_has_work,
    .get_phys_page_attrs_debug = arm_cpu_get_phys_page_attrs_debug,
    .asidx_from_attrs          = arm_asidx_from_attrs,
};

#ifdef CONFIG_TCG
static vaddr aprofile_pointer_wrap(CPUState* cs, int mmu_idx, vaddr result, vaddr base)
{
    /*
     * The Stage2 and Phys indexes are only used for ptw on arm32,
     * and all pte's are aligned, so we never produce a wrap for these.
     * Double check that we're not truncating a 40-bit physical address.
     */
    assert(((unsigned)mmu_idx & ~ARM_MMU_IDX_A_GXF) < (ARMMMUIdx_Stage2_S & ARM_MMU_IDX_COREIDX_MASK));

    if (!is_a64(cpu_env(cs))) { return (uint32_t)result; }

    /*
     * TODO: For FEAT_CPA2, decide how to we want to resolve
     * Unpredictable_CPACHECK in AddressIncrement.
     */
    return result;
}

static const TCGCPUOps arm_tcg_ops = {
    .mttcg_supported = true,
    /* ARM processors have a weak memory model */
    .guest_default_memory_order = 0,

    .initialize                = arm_translate_init,
    .translate_code            = arm_translate_code,
    .get_tb_cpu_state          = arm_get_tb_cpu_state,
    .synchronize_from_tb       = arm_cpu_synchronize_from_tb,
    .debug_excp_handler        = arm_debug_excp_handler,
    .restore_state_to_opc      = arm_restore_state_to_opc,
    .mmu_index                 = arm_cpu_mmu_index,
    .tlb_fill_align            = arm_cpu_tlb_fill_align,
    .pointer_wrap              = aprofile_pointer_wrap,
    .cpu_exec_interrupt        = arm_cpu_exec_interrupt,
    .cpu_exec_halt             = arm_cpu_exec_halt,
    .cpu_exec_reset            = cpu_reset,
    .do_interrupt              = arm_cpu_do_interrupt,
    .do_transaction_failed     = arm_cpu_do_transaction_failed,
    .do_unaligned_access       = arm_cpu_do_unaligned_access,
    .adjust_watchpoint_address = arm_adjust_watchpoint_address,
    .debug_check_watchpoint    = arm_debug_check_watchpoint,
    .debug_check_breakpoint    = arm_debug_check_breakpoint,
};
#endif /* CONFIG_TCG */

static void arm_cpu_class_init(ObjectClass* oc, const void* data)
{
    ARMCPUClass*     acc = ARM_CPU_CLASS(oc);
    CPUClass*        cc  = CPU_CLASS(acc);
    DeviceClass*     dc  = DEVICE_CLASS(oc);
    ResettableClass* rc  = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, arm_cpu_realizefn, &acc->parent_realize);

    device_class_set_props(dc, arm_cpu_properties);

    resettable_class_set_parent_phases(rc, NULL, arm_cpu_reset_hold, NULL, &acc->parent_phases);

    cc->class_by_name              = arm_cpu_class_by_name;
    cc->dump_state                 = arm_cpu_dump_state;
    cc->set_pc                     = arm_cpu_set_pc;
    cc->get_pc                     = arm_cpu_get_pc;
    cc->gdb_read_register          = arm_cpu_gdb_read_register;
    cc->gdb_write_register         = arm_cpu_gdb_write_register;
    cc->sysemu_ops                 = &arm_sysemu_ops;
    cc->gdb_arch_name              = arm_gdb_arch_name;
    cc->gdb_get_core_xml_file      = arm_gdb_get_core_xml_file;
    cc->gdb_stop_before_watchpoint = true;

#ifdef CONFIG_TCG
    cc->tcg_ops = &arm_tcg_ops;
#endif /* CONFIG_TCG */
}

static void arm_cpu_instance_init(Object* obj)
{
    ARMCPUClass* acc = ARM_CPU_GET_CLASS(obj);

    acc->info->initfn(obj);
    arm_cpu_post_init(obj);
}

static void cpu_register_class_init(ObjectClass* oc, const void* data)
{
    ARMCPUClass* acc = ARM_CPU_CLASS(oc);
    CPUClass*    cc  = CPU_CLASS(acc);

    acc->info = data;
    if (acc->info->deprecation_note) { cc->deprecation_note = acc->info->deprecation_note; }
}

void arm_cpu_register(const ARMCPUInfo* info)
{
    TypeInfo type_info = {
        .parent        = TYPE_ARM_CPU,
        .instance_init = arm_cpu_instance_init,
        .class_init    = info->class_init ?: cpu_register_class_init,
        .class_data    = info,
    };

    type_info.name = g_strdup_printf("%s-" TYPE_ARM_CPU, info->name);
    type_register_static(&type_info);
    g_free((void*)type_info.name);
}

static const TypeInfo arm_cpu_type_info = {
    .name   = TYPE_ARM_CPU,
    .parent = TYPE_CPU,
    OBJECT_TYPE_INSTANCE(ARMCPU),
    .instance_init     = arm_cpu_initfn,
    .instance_finalize = arm_cpu_finalizefn,
    .abstract          = true,
    .class_size        = sizeof(ARMCPUClass),
    .class_init        = arm_cpu_class_init,
};

DEFINE_TYPE(arm_cpu_type_info)
