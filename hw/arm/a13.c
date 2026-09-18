/*
 * Apple A13 CPU.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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
#include "hw/arm/a13.h"
#include "hw/arm/a13_gxf.h"
#include "hw/arm/dt.h"
#include "hw/irq.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "target/arm/cpregs.h"
#include "arm-powerctl.h"

#define A13_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset)              \
    {.cp          = CP_REG_ARM64_SYSREG_CP,                                                      \
     .name        = #p_name,                                                                     \
     .opc0        = p_op0,                                                                       \
     .crn         = p_crn,                                                                       \
     .crm         = p_crm,                                                                       \
     .opc1        = p_op1,                                                                       \
     .opc2        = p_op2,                                                                       \
     .access      = p_access,                                                                    \
     .resetvalue  = p_reset,                                                                     \
     .state       = ARM_CP_STATE_AA64,                                                           \
     .type        = ARM_CP_OVERRIDE,                                                             \
     .fieldoffset = offsetof(AppleA13State, A13_CPREG_VAR_NAME(p_name)) - offsetof(ARMCPU, env)}

#define A13_CLUSTER_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access) \
    {.cp          = CP_REG_ARM64_SYSREG_CP,                                        \
     .name        = #p_name,                                                       \
     .opc0        = p_op0,                                                         \
     .crn         = p_crn,                                                         \
     .crm         = p_crm,                                                         \
     .opc1        = p_op1,                                                         \
     .opc2        = p_op2,                                                         \
     .access      = p_access,                                                      \
     .type        = ARM_CP_IO,                                                     \
     .state       = ARM_CP_STATE_AA64,                                             \
     .readfn      = apple_a13_cluster_cpreg_read,                                  \
     .writefn     = apple_a13_cluster_cpreg_write,                                 \
     .fieldoffset = offsetof(AppleA13Cluster, A13_CPREG_VAR_NAME(p_name))}

#define IPI_SR_SRC_CPU_SHIFT       8
#define IPI_SR_SRC_CPU_WIDTH       8
#define IPI_SR_SRC_CPU_MASK        (((1 << IPI_SR_SRC_CPU_WIDTH) - 1) << IPI_SR_SRC_CPU_SHIFT)
#define IPI_SR_SRC_CPU(ipi_sr_val) (((ipi_sr_val) & IPI_SR_SRC_CPU_MASK) >> IPI_SR_SRC_CPU_SHIFT)

#define IPI_RR_TARGET_CLUSTER_SHIFT 16

#define IPI_RR_TYPE_IMMEDIATE (0 << 28)
#define IPI_RR_TYPE_RETRACT   (1 << 28)
#define IPI_RR_TYPE_DEFERRED  (2 << 28)
#define IPI_RR_TYPE_NOWAKE    (3 << 28)
#define IPI_RR_TYPE_MASK      (3 << 28)
#define NSEC_PER_USEC         1000ull       /* nanoseconds per microsecond */
#define USEC_PER_SEC          1000000ull    /* microseconds per second */
#define NSEC_PER_SEC          1000000000ull /* nanoseconds per second */
#define NSEC_PER_MSEC         1000000ull    /* nanoseconds per millisecond */
#define RTCLOCK_SEC_DIVISOR   24000000ull

static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t* result)
{
    uint64_t t64;

    *result  = (t64 = abstime / RTCLOCK_SEC_DIVISOR) * NSEC_PER_SEC;
    abstime -= (t64 * RTCLOCK_SEC_DIVISOR);
    *result += (abstime * NSEC_PER_SEC) / RTCLOCK_SEC_DIVISOR;
}

static void nanoseconds_to_absolutetime(uint64_t nanosecs, uint64_t* result)
{
    uint64_t t64;

    *result   = (t64 = nanosecs / NSEC_PER_SEC) * RTCLOCK_SEC_DIVISOR;
    nanosecs -= (t64 * NSEC_PER_SEC);
    *result  += (nanosecs * RTCLOCK_SEC_DIVISOR) / NSEC_PER_SEC;
}

static QTAILQ_HEAD(, AppleA13Cluster) clusters = QTAILQ_HEAD_INITIALIZER(clusters);

static uint64_t   ipi_cr      = kDeferredIPITimerDefault;
static QEMUTimer* ipicr_timer = NULL;

bool apple_a13_is_asleep(const AppleA13State* acpu) { return CPU(acpu)->halted; }

bool apple_a13_is_off(const AppleA13State* acpu) { return acpu->parent_obj.power_state == PSCI_OFF; }

void apple_a13_set_on(AppleA13State* acpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (apple_a13_is_off(acpu)) { ret = arm_set_cpu_on_and_reset(acpu->parent_obj.mp_affinity); }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) { error_report("Failed to bring up CPU %d: err %d", acpu->cpu_id, ret); }
}

void apple_a13_reset(AppleA13State* acpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (!apple_a13_is_off(acpu)) { ret = arm_reset_cpu(acpu->parent_obj.mp_affinity); }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("%s: failed to reset CPU %d: err %d", __func__, acpu->cpu_id, ret);
    }
}

void apple_a13_set_off(AppleA13State* acpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (acpu->parent_obj.power_state != PSCI_OFF) { ret = arm_set_cpu_off(acpu->parent_obj.mp_affinity); }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("%s: failed to turn off CPU %d: err %d", __func__, acpu->cpu_id, ret);
    }
}

static AppleA13Cluster* apple_a13_find_cluster(uint32_t cluster_id)
{
    AppleA13Cluster* cluster;

    QTAILQ_FOREACH (cluster, &clusters, next) {
        if (cluster->parent_obj.cluster_id == cluster_id) { return cluster; }
    }

    return NULL;
}

static uint64_t apple_a13_cluster_cpreg_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    AppleA13State*   acpu = container_of(env_archcpu(env), AppleA13State, parent_obj);
    AppleA13Cluster* c    = apple_a13_find_cluster(acpu->cluster_id);

    if (unlikely(!c)) { return 0; }

    return *(uint64_t*)((char*)(c) + (ri)->fieldoffset);
}

static void apple_a13_cluster_cpreg_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    AppleA13State*   acpu = container_of(env_archcpu(env), AppleA13State, parent_obj);
    AppleA13Cluster* c    = apple_a13_find_cluster(acpu->cluster_id);

    if (unlikely(!c)) { return; }
    *(uint64_t*)((char*)(c) + (ri)->fieldoffset) = value;
}

/* Deliver IPI */
static void apple_a13_deliver_ipi(AppleA13State* cpu, uint64_t src_cpu, uint64_t flag)
{
    if (cpu->ipi_sr) { return; }

    cpu->ipi_sr = 1LL | (src_cpu << IPI_SR_SRC_CPU_SHIFT) | flag;
    qemu_irq_raise(cpu->fast_ipi);
}

static void apple_a13_cluster_deliver_ipi(AppleA13Cluster* c, uint64_t cpu_id, uint64_t src_cpu, uint64_t flag)
{ apple_a13_deliver_ipi(c->cpus[cpu_id], src_cpu, flag); }

static int add_cpu_to_cluster(Object* obj, void* opaque)
{
    AppleA13Cluster* cluster = opaque;
    AppleA13State*   acpu    = (AppleA13State*)object_dynamic_cast(obj, TYPE_APPLE_A13);

    if (!acpu) { return 0; }

    acpu->parent_obj.parent_obj.cluster_index = cluster->parent_obj.cluster_id;
    cluster->cpus[acpu->cpu_id]               = acpu;
    return 0;
}

static void apple_a13_cluster_realize(DeviceState* dev, Error** errp)
{
    AppleA13Cluster* cluster = APPLE_A13_CLUSTER(dev);
    object_child_foreach_recursive(OBJECT(cluster), add_cpu_to_cluster, dev);
}

static void apple_a13_cluster_tick(AppleA13Cluster* c)
{
    uint32_t on = 0, awake = 0;

    for (uint32_t i = 0; i < A13_MAX_CPU; ++i) {
        const AppleA13State* cpu = c->cpus[i];
        if (!cpu || apple_a13_is_off(cpu)) { continue; }
        on |= BIT32(i);
        if (!apple_a13_is_asleep(cpu)) { awake |= BIT32(i); }
    }

    if (!on) { return; }

    for (uint32_t src = 0; src < A13_MAX_CPU; ++src) {
        if (!c->cpus[src]) { continue; }

        const uint32_t noWakeCandidates = c->noWakeIPI[src] & awake;
        const uint32_t candidates       = noWakeCandidates | (c->deferredIPI[src] & on);
        if (!candidates) { continue; }

        const uint32_t dest = ctz32(candidates);
        const uint32_t type = (noWakeCandidates & BIT32(dest)) ? IPI_RR_TYPE_NOWAKE : IPI_RR_TYPE_DEFERRED;
        apple_a13_cluster_deliver_ipi(c, dest, src, type);
    }
}

static void apple_a13_cluster_ipicr_tick(void* opaque)
{
    AppleA13Cluster* cluster;

    QTAILQ_FOREACH (cluster, &clusters, next) { apple_a13_cluster_tick(cluster); }

    timer_mod_ns(ipicr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ipi_cr);
}

static void apple_a13_cluster_reset_handler(void* opaque)
{
    ipi_cr = kDeferredIPITimerDefault;
    if (ipicr_timer) { timer_del(ipicr_timer); }
    else {
        ipicr_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_a13_cluster_ipicr_tick, NULL);
    }
    timer_mod_ns(ipicr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ipi_cr);
}

static void apple_a13_cluster_instance_init(Object* obj)
{
    AppleA13Cluster* cluster = APPLE_A13_CLUSTER(obj);
    QTAILQ_INSERT_TAIL(&clusters, cluster, next);

    if (ipicr_timer == NULL) { qemu_register_reset(apple_a13_cluster_reset_handler, NULL); }
}

/* Deliver local IPI */
static void apple_a13_ipi_rr_local(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    AppleA13State* acpu = container_of(env_archcpu(env), AppleA13State, parent_obj);

    uint32_t         phys_id    = (value & 0xFF) | (acpu->cluster_id << 8);
    AppleA13Cluster* c          = apple_a13_find_cluster(acpu->cluster_id);
    uint32_t         dst_cpu_id = -1U;
    AppleA13State*   dst_acpu;

    for (uint32_t i = 0; i < A13_MAX_CPU; i++) {
        if (c->cpus[i] != NULL && c->cpus[i]->phys_id == phys_id) {
            dst_cpu_id = i;
            dst_acpu   = c->cpus[dst_cpu_id];
            break;
        }
    }

    if (dst_cpu_id == -1U) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CPU %x failed to send fast IPI to local CPU %x: value: "
                      "0x" HWADDR_FMT_plx "\n",
                      acpu->phys_id, phys_id, value);
        return;
    }

    switch (value & IPI_RR_TYPE_MASK) {
        case IPI_RR_TYPE_NOWAKE:
            if (apple_a13_is_asleep(dst_acpu)) { c->noWakeIPI[acpu->cpu_id] |= BIT32(dst_cpu_id); }
            else {
                apple_a13_deliver_ipi(dst_acpu, acpu->cpu_id, IPI_RR_TYPE_IMMEDIATE);
            }
            break;
        case IPI_RR_TYPE_DEFERRED: c->deferredIPI[acpu->cpu_id] |= BIT32(dst_cpu_id); break;
        case IPI_RR_TYPE_RETRACT:
            c->deferredIPI[acpu->cpu_id] &= ~BIT32(dst_cpu_id);
            c->noWakeIPI[acpu->cpu_id]   &= ~BIT32(dst_cpu_id);
            break;
        case IPI_RR_TYPE_IMMEDIATE: apple_a13_deliver_ipi(dst_acpu, acpu->cpu_id, IPI_RR_TYPE_IMMEDIATE); break;
        default                   : assert_not_reached();
    }
}

/* Deliver global IPI */
static void apple_a13_ipi_rr_global(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    AppleA13State*   acpu       = container_of(env_archcpu(env), AppleA13State, parent_obj);
    uint32_t         cluster_id = (value >> IPI_RR_TARGET_CLUSTER_SHIFT) & 0xFF;
    AppleA13Cluster* cluster    = apple_a13_find_cluster(cluster_id);
    if (!cluster) { return; }

    uint32_t       phys_id    = (value & 0xFF) | (cluster_id << 8);
    uint32_t       dst_cpu_id = -1;
    AppleA13State* dst_acpu;

    for (uint32_t i = 0; i < A13_MAX_CPU; i++) {
        if (cluster->cpus[i] != NULL && cluster->cpus[i]->phys_id == phys_id) {
            dst_cpu_id = i;
            dst_acpu   = cluster->cpus[dst_cpu_id];
            break;
        }
    }

    if (dst_cpu_id == -1U) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CPU %x failed to send fast IPI to global CPU %x: value: "
                      "0x" HWADDR_FMT_plx "\n",
                      acpu->phys_id, phys_id, value);
        return;
    }

    switch (value & IPI_RR_TYPE_MASK) {
        case IPI_RR_TYPE_NOWAKE:
            if (apple_a13_is_asleep(dst_acpu)) { cluster->noWakeIPI[acpu->cpu_id] |= BIT32(dst_cpu_id); }
            else {
                apple_a13_deliver_ipi(dst_acpu, acpu->cpu_id, IPI_RR_TYPE_IMMEDIATE);
            }
            break;
        case IPI_RR_TYPE_DEFERRED: cluster->deferredIPI[acpu->cpu_id] |= BIT32(dst_cpu_id); break;
        case IPI_RR_TYPE_RETRACT:
            cluster->deferredIPI[acpu->cpu_id] &= ~BIT32(dst_cpu_id);
            cluster->noWakeIPI[acpu->cpu_id]   &= ~BIT32(dst_cpu_id);
            break;
        case IPI_RR_TYPE_IMMEDIATE: apple_a13_deliver_ipi(dst_acpu, acpu->cpu_id, IPI_RR_TYPE_IMMEDIATE); break;
        default                   : assert_not_reached();
    }
}

/* Receiving IPI */
static uint64_t apple_a13_ipi_read_sr(CPUARMState* env, const ARMCPRegInfo* ri)
{
    AppleA13State* acpu = container_of(env_archcpu(env), AppleA13State, parent_obj);

    return acpu->ipi_sr;
}

/* Acknowledge received IPI */
static void apple_a13_ipi_write_sr(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    AppleA13State*   acpu    = container_of(env_archcpu(env), AppleA13State, parent_obj);
    AppleA13Cluster* c       = apple_a13_find_cluster(acpu->cluster_id);
    uint64_t         src_cpu = IPI_SR_SRC_CPU(value);

    acpu->ipi_sr = 0;
    qemu_irq_lower(acpu->fast_ipi);

    switch (value & IPI_RR_TYPE_MASK) {
        case IPI_RR_TYPE_NOWAKE  : c->noWakeIPI[src_cpu] &= ~BIT32(acpu->cpu_id); break;
        case IPI_RR_TYPE_DEFERRED: c->deferredIPI[src_cpu] &= ~BIT32(acpu->cpu_id); break;
        default                  : break;
    }
}

/* Read deferred interrupt timeout (global) */
static uint64_t apple_a13_ipi_read_cr(CPUARMState* env, const ARMCPRegInfo* ri)
{
    uint64_t abstime = 0;

    nanoseconds_to_absolutetime(ipi_cr, &abstime);
    return abstime;
}

/* Set deferred interrupt timeout (global) */
static void apple_a13_ipi_write_cr(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    uint64_t nanosec = 0;
    uint64_t ct      = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (value != 0) { absolutetime_to_nanoseconds(value & 0xFFFF, &nanosec); }

    if (nanosec == 0) { timer_del(ipicr_timer); }
    else {
        timer_mod_ns(ipicr_timer, ct + nanosec);
    }
    ipi_cr = nanosec;
}

static const ARMCPRegInfo apple_a13_cp_reginfo_tcg[] = {
    A13_CPREG_DEF(ARM64_REG_EHID3, 3, 0, 15, 3, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_EHID4, 3, 0, 15, 4, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_EHID10, 3, 0, 15, 10, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID0, 3, 0, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID1, 3, 0, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID3, 3, 0, 15, 3, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID4, 3, 0, 15, 4, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID5, 3, 0, 15, 5, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID7, 3, 0, 15, 7, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID8, 3, 0, 15, 8, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID9, 3, 0, 15, 9, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID11, 3, 0, 15, 11, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID13, 3, 0, 15, 14, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID14, 3, 0, 15, 15, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID16, 3, 0, 15, 15, 2, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_MMU_ERR_STS, 3, 6, 15, 2, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LSU_ERR_STS, 3, 3, 15, 2, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_FED_ERR_STS, 3, 4, 15, 0, 2, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_STS, 3, 3, 15, 8, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_INF, 3, 3, 15, 10, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_ADR, 3, 3, 15, 9, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_RAM_CONFIG, 3, 3, 15, 7, 0, PL1_RW, 0x8000000000332211ULL),
    A13_CPREG_DEF(IMP_BARRIER_LBSY_BST_SYNC_W0_EL0, 3, 3, 15, 15, 0, PL1_RW, 0),
    A13_CPREG_DEF(IMP_BARRIER_LBSY_BST_SYNC_W1_EL0, 3, 3, 15, 15, 1, PL1_RW, 0),
    A13_CPREG_DEF(PMC0, 3, 2, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMC1, 3, 2, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMCR0, 3, 1, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMCR1, 3, 1, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMSR, 3, 1, 15, 13, 0, PL1_RW, 0),
    A13_CPREG_DEF(S3_4_c15_c0_5, 3, 4, 15, 0, 5, PL1_RW, 0),
    A13_CPREG_DEF(AMX_STATUS_EL1, 3, 4, 15, 1, 3, PL1_R, 0),
    A13_CPREG_DEF(AMX_CTL_EL1, 3, 4, 15, 1, 4, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_CPU_OVRD, 3, 5, 15, 5, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_ACC_OVRD, 3, 5, 15, 6, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_ACC_CFG, 3, 5, 15, 4, 0, PL1_RW, 0),
    A13_CPREG_DEF(S3_5_c15_c10_1, 3, 5, 15, 10, 1, PL0_RW, 0),
    A13_CPREG_DEF(SYS_ACC_PWR_DN_SAVE, 3, 7, 15, 2, 0, PL1_RW, 0),
    A13_CPREG_DEF(UPMPCM, 3, 7, 15, 5, 4, PL1_RW, 0),
    A13_CPREG_DEF(UPMCR0, 3, 7, 15, 0, 4, PL1_RW, 0),
    A13_CPREG_DEF(UPMSR, 3, 7, 15, 6, 4, PL1_RW, 0),
    A13_CLUSTER_CPREG_DEF(CTRR_A_LWR_EL1, 3, 4, 15, 2, 3, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_A_UPR_EL1, 3, 4, 15, 2, 4, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_B_LWR_EL1, 3, 4, 15, 1, 7, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_B_UPR_EL1, 3, 4, 15, 1, 6, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_CTL_EL1, 3, 4, 15, 2, 5, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_LOCK_EL1, 3, 4, 15, 2, 2, PL1_RW),
    {
        .cp      = CP_REG_ARM64_SYSREG_CP,
        .name    = "ARM64_REG_IPI_RR_LOCAL",
        .opc0    = 3,
        .opc1    = 5,
        .crn     = 15,
        .crm     = 0,
        .opc2    = 0,
        .access  = PL1_W,
        .type    = ARM_CP_IO | ARM_CP_NO_RAW,
        .state   = ARM_CP_STATE_AA64,
        .readfn  = arm_cp_read_zero,
        .writefn = apple_a13_ipi_rr_local,
    },
    {
        .cp      = CP_REG_ARM64_SYSREG_CP,
        .name    = "ARM64_REG_IPI_RR_GLOBAL",
        .opc0    = 3,
        .opc1    = 5,
        .crn     = 15,
        .crm     = 0,
        .opc2    = 1,
        .access  = PL1_W,
        .type    = ARM_CP_IO | ARM_CP_NO_RAW,
        .state   = ARM_CP_STATE_AA64,
        .readfn  = arm_cp_read_zero,
        .writefn = apple_a13_ipi_rr_global,
    },
    {
        .cp      = CP_REG_ARM64_SYSREG_CP,
        .name    = "ARM64_REG_IPI_SR",
        .opc0    = 3,
        .opc1    = 5,
        .crn     = 15,
        .crm     = 1,
        .opc2    = 1,
        .access  = PL1_RW,
        .type    = ARM_CP_IO | ARM_CP_NO_RAW,
        .state   = ARM_CP_STATE_AA64,
        .readfn  = apple_a13_ipi_read_sr,
        .writefn = apple_a13_ipi_write_sr,
    },
    {
        .cp      = CP_REG_ARM64_SYSREG_CP,
        .name    = "ARM64_REG_IPI_CR",
        .opc0    = 3,
        .opc1    = 5,
        .crn     = 15,
        .crm     = 3,
        .opc2    = 1,
        .access  = PL1_RW,
        .type    = ARM_CP_IO,
        .state   = ARM_CP_STATE_AA64,
        .readfn  = apple_a13_ipi_read_cr,
        .writefn = apple_a13_ipi_write_cr,
    },
};

static void apple_a13_add_cpregs(AppleA13State* acpu)
{
    ARMCPU* cpu = &acpu->parent_obj;
    define_arm_cp_regs(cpu, apple_a13_cp_reginfo_tcg);
    apple_a13_init_gxf(acpu);
}

static void apple_a13_realize(DeviceState* dev, Error** errp)
{
    AppleA13State* acpu   = APPLE_A13(dev);
    AppleA13Class* tclass = APPLE_A13_GET_CLASS(dev);
    DeviceState*   fiq_or;
    Object*        obj = OBJECT(dev);

    object_property_set_link(OBJECT(acpu), "memory", OBJECT(&acpu->memory), errp);
    if (*errp) { return; }
    apple_a13_add_cpregs(acpu);
    tclass->parent_realize(dev, errp);
    if (*errp) { return; }
    apple_a13_init_gxf_override(acpu);
    fiq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(obj, "fiq-or", OBJECT(fiq_or));
    qdev_prop_set_uint16(fiq_or, "num-lines", 16);
    qdev_realize_and_unref(fiq_or, NULL, errp);
    if (*errp) { return; }
    qdev_connect_gpio_out(fiq_or, 0, qdev_get_gpio_in(dev, ARM_CPU_FIQ));

    qdev_connect_gpio_out(dev, GTIMER_VIRT, qdev_get_gpio_in(fiq_or, 0));
    acpu->fast_ipi = qdev_get_gpio_in(fiq_or, 1);
}

static void apple_a13_instance_init(Object* obj) { object_property_set_uint(obj, "cntfrq", 24000000, &error_fatal); }

AppleA13State* apple_a13_create(const char* name, uint32_t cpu_id, uint32_t phys_id, uint32_t cluster_id,
                                uint16_t cluster_type)
{
    DeviceState*   dev;
    AppleA13State* acpu;
    ARMCPU*        cpu;
    Object*        obj;
    uint64_t       mpidr;

    obj  = object_new(TYPE_APPLE_A13);
    dev  = DEVICE(obj);
    acpu = APPLE_A13(dev);
    cpu  = &acpu->parent_obj;

    dev->id          = g_strdup(name);
    acpu->cpu_id     = cpu_id;
    acpu->phys_id    = phys_id;
    acpu->cluster_id = cluster_id;

    mpidr = acpu->phys_id | BIT_ULL(31);

    cpu->midr = REG_FIELD_DP64(0, MIDR_EL1, IMPLEMENTER, 0x61);    // Apple
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, ARCHITECTURE, 0x0f);
    /* chip-revision = (variant << 4) | (revision) */
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, VARIANT, 0x1);
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, REVISION, 0x1);

    switch (cluster_type) {
        case 'P':    // Lightning
            mpidr     |= (1 << ARM_AFF2_SHIFT);
            cpu->midr  = REG_FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x12);
            break;
        case 'E':    // Thunder
            cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x13);
            break;
        case 'S':    // SEP
            mpidr     |= (0xb << ARM_AFF1_SHIFT);
            cpu->midr  = REG_FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x15);
            break;
        default: break;
    }

    object_property_set_uint(obj, "mp-affinity", mpidr, &error_fatal);
    object_property_set_bool(obj, "start-powered-off", true, NULL);
    object_property_set_bool(obj, "has_el3", false, NULL);
    object_property_set_bool(obj, "has_el2", false, NULL);
    // KVM will throw up otherwise
    object_property_set_bool(obj, "pmu", false, NULL);

    memory_region_init(&acpu->memory, obj, "cpu-memory", UINT64_MAX);
    memory_region_init_alias(&acpu->sysmem, obj, "sysmem", get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&acpu->memory, 0, &acpu->sysmem, -2);

    return acpu;
}

AppleA13State* apple_a13_from_node(AppleDTNode* node)
{
    AppleA13State* acpu;

    acpu = apple_a13_create(
        apple_dt_get_prop_str(node, "name", &error_fatal), apple_dt_get_prop_u32(node, "cpu-id", &error_fatal),
        apple_dt_get_prop_u32(node, "reg", &error_fatal), apple_dt_get_prop_u32(node, "cluster-id", &error_fatal),
        apple_dt_get_prop_u16(node, "cluster-type", &error_fatal));

    apple_dt_del_prop_named(node, "reg-private");
    apple_dt_del_prop_named(node, "cpu-uttdbg-reg");
    if (acpu->cpu_id == 0) { apple_dt_set_prop_str(node, "state", "running"); }
    apple_dt_set_prop_u64(node, "timebase-frequency", 24000000);
    apple_dt_set_prop_u64(node, "fixed-frequency", 24000000);
    apple_dt_set_prop_u64(node, "peripheral-frequency", 24000000);
    apple_dt_set_prop_u64(node, "memory-frequency", 24000000);
    apple_dt_set_prop_u64(node, "bus-frequency", 24000000);
    apple_dt_set_prop_u64(node, "clock-frequency", 24000000);
    apple_dt_del_prop_named(node, "coresight-reg");

    return acpu;
}

static const Property apple_a13_cluster_properties[] = {
    DEFINE_PROP_UINT32("cluster-type", AppleA13Cluster, cluster_type, 0),
};

static void apple_a13_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*   dc = DEVICE_CLASS(klass);
    AppleA13Class* tc = APPLE_A13_CLASS(klass);

    device_class_set_parent_realize(dc, apple_a13_realize, &tc->parent_realize);
    dc->desc = "Apple A13 CPU";
    set_bit(DEVICE_CATEGORY_CPU, dc->categories);
}

static void apple_a13_cluster_reset_enter(Object* obj, ResetType type)
{
    AppleA13Cluster* cluster = APPLE_A13_CLUSTER(obj);
    memset(cluster->deferredIPI, 0, sizeof(cluster->deferredIPI));
    memset(cluster->noWakeIPI, 0, sizeof(cluster->noWakeIPI));
}

static void apple_a13_cluster_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    dc->realize        = apple_a13_cluster_realize;
    rc->phases.enter   = apple_a13_cluster_reset_enter;
    dc->desc           = "Apple A13 CPU Cluster";
    dc->user_creatable = false;
    device_class_set_props(dc, apple_a13_cluster_properties);
}

static const TypeInfo apple_a13_types[] = {
    {
        .name   = TYPE_APPLE_A13,
        .parent = ARM_CPU_TYPE_NAME("apple-gxf"),
        OBJECT_TYPE_INSTANCE(AppleA13State),
        .instance_init = apple_a13_instance_init,
        .class_size    = sizeof(AppleA13Class),
        .class_init    = apple_a13_class_init,
    },
    {
        .name   = TYPE_APPLE_A13_CLUSTER,
        .parent = TYPE_CPU_CLUSTER,
        OBJECT_TYPE_INSTANCE(AppleA13Cluster),
        .instance_init = apple_a13_cluster_instance_init,
        .class_init    = apple_a13_cluster_class_init,
    },
};

DEFINE_TYPES(apple_a13_types)
