/*
 * Apple SEP Monitor.
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

#include "hw/arm/sep/private.h"
#include "hw/resettable.h"

#define MONI_BASE_REG_SIZE (0x40000)
#define MONI_THRM_REG_SIZE (0x10000)

struct AppleSEPMonitorState
{
    SysBusDevice parent_obj;

    MemoryRegion base_mr;
    MemoryRegion thrm_mr;
    uint8_t      base_regs[MONI_BASE_REG_SIZE];
    uint8_t      thrm_regs[MONI_THRM_REG_SIZE];
};

static void moni_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPMonitorState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
            memcpy(&s->base_regs[addr], &data, size);
            DPRINTF("SEP MONI_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t moni_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPMonitorState* s   = opaque;
    uint64_t              ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        case 0xa000:
            // bit0 needed to select non-default region?
            // bit0 set while opcode17 empty/0x0: uses 0x8... (TZ0). it requires an
            // aligned address, otherwise seprom will panic.
            // bit0 unset while opcode17 empty/0x0: uses 0x30... .
            // only one of the two bit0's set with opcode17 being non-empty is
            // untested
            // both bit0 unset with opcode17 non-empty: as before
            // both bit0 set with opcode17 non-empty: tries to jump to 0x810000000
            // this bit0 unset, other bit0 set with opcode17 non-empty: fails to
            // boot to 0x300000000/0x340000000
            // this bit0 set, other bit0 unset with opcode17 non-empty: boots via
            // 0x340000000
            // ret |= BIT(0);
            break;
        case 0xa004:
            // bit0 needed to allow opcode17 missing or 0x0
            // bit0 set: currently doesn't boot with/without opcode17.
            // bit0 set probably means "disable integrity tree"
            // ret |= BIT(0);
            break;
        default:
            memcpy(&ret, &s->base_regs[addr], size);
            DPRINTF("SEP MONI_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps moni_base_reg_ops = {
    .write                 = moni_base_reg_write,
    .read                  = moni_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void moni_thrm_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPMonitorState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
            memcpy(&s->thrm_regs[addr], &data, size);
            DPRINTF("SEP MONI_THRM: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t moni_thrm_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPMonitorState* s   = opaque;
    uint64_t              ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
            memcpy(&ret, &s->thrm_regs[addr], size);
            DPRINTF("SEP MONI_THRM: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps moni_thrm_reg_ops = {
    .write                 = moni_thrm_reg_write,
    .read                  = moni_thrm_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_monitor_reset_enter(Object* obj, ResetType type)
{
    AppleSEPMonitorState* s = APPLE_SEP_MONITOR(obj);

    memset(s->base_regs, 0, sizeof(s->base_regs));
    memset(s->thrm_regs, 0, sizeof(s->thrm_regs));
}

static void apple_sep_monitor_realize(DeviceState* dev, Error** errp)
{
    AppleSEPMonitorState* s   = APPLE_SEP_MONITOR(dev);
    SysBusDevice*         sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(s), &moni_base_reg_ops, s, "base", MONI_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
    memory_region_init_io(&s->thrm_mr, OBJECT(s), &moni_thrm_reg_ops, s, "thrm", MONI_THRM_REG_SIZE);
    sysbus_init_mmio(sbd, &s->thrm_mr);
}

static void apple_sep_monitor_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_monitor_reset_enter;
    dc->realize      = apple_sep_monitor_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPMonitorState, apple_sep_monitor, APPLE_SEP_MONITOR, SYS_BUS_DEVICE)

AppleSEPMonitorState* apple_sep_monitor_create(void) { return APPLE_SEP_MONITOR(qdev_new(TYPE_APPLE_SEP_MONITOR)); }
