/*
 * Apple A9 CPU.
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
#include "hw/arm/a9.h"
#include "hw/arm/dt.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "system/tcg.h"
#include "target/arm/cpregs.h"
#include "arm-powerctl.h"

#define A9_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access, p_reset)           \
    {                                                                                        \
        .cp          = CP_REG_ARM64_SYSREG_CP,                                               \
        .name        = #p_name,                                                              \
        .opc0        = p_op0,                                                                \
        .crn         = p_crn,                                                                \
        .crm         = p_crm,                                                                \
        .opc1        = p_op1,                                                                \
        .opc2        = p_op2,                                                                \
        .access      = p_access,                                                             \
        .resetvalue  = p_reset,                                                              \
        .state       = ARM_CP_STATE_AA64,                                                    \
        .type        = ARM_CP_OVERRIDE,                                                      \
        .fieldoffset = offsetof(AppleA9, A9_CPREG_VAR_NAME(p_name)) - offsetof(ARMCPU, env), \
    }

bool apple_a9_cpu_is_asleep(AppleA9* acpu) { return CPU(acpu)->halted; }

bool apple_a9_cpu_is_off(AppleA9* acpu) { return acpu->parent_obj.power_state == ARM_POWER_OFF; }

void apple_a9_cpu_set_on(AppleA9* acpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (apple_a9_cpu_is_off(acpu)) { ret = arm_set_cpu_on_and_reset(acpu->parent_obj.mp_affinity); }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) { error_report("Failed to bring up CPU %d: err %d", acpu->cpu_id, ret); }
}

static const ARMCPRegInfo a9_cp_reginfo_tcg[] = {
    A9_CPREG_DEF(HID11, 3, 0, 15, 13, 0, PL1_RW, 0),       A9_CPREG_DEF(HID3, 3, 0, 15, 3, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID4, 3, 0, 15, 4, 0, PL1_RW, 0),         A9_CPREG_DEF(HID5, 3, 0, 15, 5, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID7, 3, 0, 15, 7, 0, PL1_RW, 0),         A9_CPREG_DEF(HID8, 3, 0, 15, 8, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR0, 3, 1, 15, 0, 0, PL1_RW, 0),        A9_CPREG_DEF(PMCR1, 3, 1, 15, 1, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR2, 3, 1, 15, 2, 0, PL1_RW, 0),        A9_CPREG_DEF(PMCR4, 3, 1, 15, 4, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMESR0, 3, 1, 15, 5, 0, PL1_RW, 0),       A9_CPREG_DEF(PMESR1, 3, 1, 15, 6, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMAT0, 3, 1, 15, 7, 0, PL1_RW, 0),       A9_CPREG_DEF(OPMAT1, 3, 1, 15, 8, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMSK0, 3, 1, 15, 9, 0, PL1_RW, 0),       A9_CPREG_DEF(OPMSK1, 3, 1, 15, 10, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMSR, 3, 1, 15, 13, 0, PL1_RW, 0),        A9_CPREG_DEF(PMC0, 3, 2, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMC1, 3, 2, 15, 1, 0, PL1_RW, 0),         A9_CPREG_DEF(PMTRHLD6, 3, 2, 15, 12, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMTRHLD4, 3, 2, 15, 13, 0, PL1_RW, 0),    A9_CPREG_DEF(PMTRHLD2, 3, 2, 15, 14, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMMMAP, 3, 2, 15, 15, 0, PL1_RW, 0),      A9_CPREG_DEF(SYS_LSU_ERR_STS, 3, 3, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(LSU_ERR_STS, 3, 3, 15, 8, 0, PL1_RW, 0),  A9_CPREG_DEF(LSU_ERR_ADR, 3, 3, 15, 9, 0, PL1_RW, 0),
    A9_CPREG_DEF(L2C_ERR_INF, 3, 3, 15, 10, 0, PL1_RW, 0), A9_CPREG_DEF(FED_ERR_STS, 3, 4, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(CYC_CFG, 3, 5, 15, 4, 0, PL1_RW, 0),      A9_CPREG_DEF(RMR_EL3, 3, 6, 12, 0, 2, PL1_RW, 0),
    A9_CPREG_DEF(MMU_ERR_STS, 3, 6, 15, 0, 0, PL1_RW, 0),
};

static void a9_add_cpregs(AppleA9* acpu)
{
    ARMCPU* cpu = &acpu->parent_obj;
    define_arm_cp_regs(cpu, a9_cp_reginfo_tcg);
}

static void apple_a9_realize(DeviceState* dev, Error** errp)
{
    AppleA9*      acpu   = APPLE_A9(dev);
    AppleA9Class* tclass = APPLE_A9_GET_CLASS(dev);
    DeviceState*  fiq_or;
    Object*       obj = OBJECT(dev);

    object_property_set_link(OBJECT(acpu), "memory", OBJECT(&acpu->memory), errp);
    if (*errp) { return; }
    a9_add_cpregs(acpu);
    tclass->parent_realize(dev, errp);
    if (*errp) { return; }
    fiq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(obj, "fiq-or", OBJECT(fiq_or));
    qdev_prop_set_uint16(fiq_or, "num-lines", 16);
    qdev_realize_and_unref(fiq_or, NULL, errp);
    if (*errp) { return; }
    qdev_connect_gpio_out(fiq_or, 0, qdev_get_gpio_in(dev, ARM_CPU_FIQ));

    qdev_connect_gpio_out(dev, GTIMER_VIRT, qdev_get_gpio_in(fiq_or, 0));
}

// static void apple_a9_reset(DeviceState *dev)
// {
//     AppleA9Class *tclass = APPLE_A9_GET_CLASS(dev);
//     tclass->parent_reset(dev);
// }

static void apple_a9_instance_init(Object* obj) {
    object_property_set_uint(obj, "cntfrq", 24000000, &error_fatal);
    if (tcg_enabled()) { object_property_set_bool(obj, "pauth-noop", true, NULL); }
}

AppleA9* apple_a9_create(const char* name, uint32_t cpu_id, uint32_t phys_id)
{
    DeviceState* dev;
    AppleA9*     acpu;
    ARMCPU*      cpu;
    Object*      obj;
    uint64_t     mpidr;

    obj  = object_new(TYPE_APPLE_A9);
    dev  = DEVICE(obj);
    acpu = APPLE_A9(dev);
    cpu  = ARM_CPU(acpu);

    dev->id       = g_strdup(name);
    acpu->cpu_id  = cpu_id;
    acpu->phys_id = phys_id;

    mpidr = acpu->phys_id | (1LL << 31) | (1 << ARM_AFF2_SHIFT);

    cpu->midr = REG_FIELD_DP64(0, MIDR_EL1, IMPLEMENTER, 0x61);
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, ARCHITECTURE, 0xf);
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x4);    // Maui
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, VARIANT, 0x1);    // B1
    cpu->midr = REG_FIELD_DP64(cpu->midr, MIDR_EL1, REVISION, 0x1);

    SET_IDREG(&cpu->isar, ID_AA64MMFR1, REG_FIELD_DP64(GET_IDREG(&cpu->isar, ID_AA64MMFR1), ID_AA64MMFR1, PAN, 0));

    object_property_set_uint(obj, "mp-affinity", mpidr, &error_fatal);
    object_property_set_bool(obj, "start-powered-off", true, NULL);
    object_property_set_bool(obj, "has_el3", true, NULL);
    object_property_set_bool(obj, "has_el2", true, NULL);
    // KVM will throw up otherwise
    object_property_set_bool(obj, "pmu", false, NULL);

    memory_region_init(&acpu->memory, obj, "cpu-memory", UINT64_MAX);
    memory_region_init_alias(&acpu->sysmem, obj, "sysmem", get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&acpu->memory, 0, &acpu->sysmem, -2);

    return acpu;
}

AppleA9* apple_a9_from_node(AppleDTNode* node)
{
    AppleA9* acpu;

    acpu = apple_a9_create(apple_dt_get_prop_str(node, "name", &error_fatal),
                           apple_dt_get_prop_u32(node, "cpu-id", &error_fatal),
                           apple_dt_get_prop_u32(node, "reg", &error_fatal));

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

static void apple_a9_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*  dc = DEVICE_CLASS(klass);
    AppleA9Class* tc = APPLE_A9_CLASS(klass);

    device_class_set_parent_realize(dc, apple_a9_realize, &tc->parent_realize);
    // device_class_set_parent_reset(dc, apple_a9_reset, &tc->parent_reset);
    dc->desc = "Apple A9 CPU";
    set_bit(DEVICE_CATEGORY_CPU, dc->categories);
}

static const TypeInfo apple_a9_info = {
    .name   = TYPE_APPLE_A9,
    .parent = ARM_CPU_TYPE_NAME("max"),
    OBJECT_TYPE_INSTANCE(AppleA9),
    .instance_init = apple_a9_instance_init,
    .class_size    = sizeof(AppleA9Class),
    .class_init    = apple_a9_class_init,
};

DEFINE_TYPE(apple_a9_info)
