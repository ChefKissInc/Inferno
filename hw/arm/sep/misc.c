/*
 * Apple SEP Misc.
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

#define MISC0_REG_SIZE (0x4000)     // ?
#define MISC1_REG_SIZE (0x40000)    // ?
#define MISC2_REG_SIZE (0x4000)     // ?

struct AppleSEPMiscState
{
    SysBusDevice parent_obj;

    MemoryRegion misc0_mr;
    MemoryRegion misc1_mr;
    MemoryRegion misc2_mr;
    uint8_t      misc0_regs[MISC0_REG_SIZE];
    uint8_t      misc1_regs[MISC1_REG_SIZE];
    uint8_t      misc2_regs[MISC2_REG_SIZE];
};

static void misc0_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPMiscState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        case 0x108:
            // initial busy-loop on T8020/T8030 SEPROM
            // same addresses and offsets on both
            if ((data & BIT(2)) != 0) { data |= BIT_ULL(63); }
            goto jump_default;
        default:
        jump_default:
            memcpy(&s->misc0_regs[addr], &data, size);
            DPRINTF("SEP MISC0: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t misc0_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPMiscState* s   = opaque;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
        jump_default:
            memcpy(&ret, &s->misc0_regs[addr], size);
            DPRINTF("SEP MISC0: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps misc0_reg_ops = {
    .write                 = misc0_reg_write,
    .read                  = misc0_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 8,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

static void misc1_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPMiscState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        case 0x180:
            // workaround for slpw
            if ((data & BIT(0)) != 0) { data &= ~BIT(0); }
            goto jump_default;
        default:
        jump_default:
            memcpy(&s->misc1_regs[addr], &data, size);
            DPRINTF("SEP MISC1: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t misc1_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPMiscState* s   = opaque;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
        jump_default:
            memcpy(&ret, &s->misc1_regs[addr], size);
            DPRINTF("SEP MISC1: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps misc1_reg_ops = {
    .write                 = misc1_reg_write,
    .read                  = misc1_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void misc2_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPMiscState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        // Some engine?: case 0x28: 0x8 bytes from TRNG
        default:
            memcpy(&s->misc2_regs[addr], &data, size);
            DPRINTF("SEP MISC2: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t misc2_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPMiscState* s   = opaque;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        case 0x24:    // ????
            return 0x0;
        default:
            memcpy(&ret, &s->misc2_regs[addr], size);
            DPRINTF("SEP MISC2: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps misc2_reg_ops = {
    .write                 = misc2_reg_write,
    .read                  = misc2_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_misc_reset_enter(Object* obj, ResetType type)
{
    AppleSEPMiscState* s = APPLE_SEP_MISC(obj);

    memset(s->misc0_regs, 0, sizeof(s->misc0_regs));
    memset(s->misc1_regs, 0, sizeof(s->misc1_regs));
    memset(s->misc2_regs, 0, sizeof(s->misc2_regs));
}

static void apple_sep_misc_realize(DeviceState* dev, Error** errp)
{
    AppleSEPMiscState* s   = APPLE_SEP_MISC(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->misc0_mr, OBJECT(dev), &misc0_reg_ops, s, "misc0", MISC0_REG_SIZE);
    sysbus_init_mmio(sbd, &s->misc0_mr);
    memory_region_init_io(&s->misc1_mr, OBJECT(dev), &misc1_reg_ops, s, "misc1", MISC1_REG_SIZE);
    sysbus_init_mmio(sbd, &s->misc1_mr);
    memory_region_init_io(&s->misc2_mr, OBJECT(dev), &misc2_reg_ops, s, "misc2", MISC2_REG_SIZE);
    sysbus_init_mmio(sbd, &s->misc2_mr);
}

static void apple_sep_misc_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_misc_reset_enter;
    dc->realize      = apple_sep_misc_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPMiscState, apple_sep_misc, APPLE_SEP_MISC, SYS_BUS_DEVICE)

AppleSEPMiscState* apple_sep_misc_create(void)
{
    AppleSEPMiscState* s = APPLE_SEP_MISC(qdev_new(TYPE_APPLE_SEP_MISC));

    return s;
}
