/*
 * Apple SEP eISP.
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

#define EISP_BASE_REG_SIZE (0x240000)
#define EISP_HMAC_REG_SIZE (0x4000)

struct AppleSEPEISPState
{
    SysBusDevice parent_obj;

    MemoryRegion base_mr;
    MemoryRegion hmac_mr;
    uint8_t      base_regs[EISP_BASE_REG_SIZE];
    uint8_t      hmac_regs[EISP_HMAC_REG_SIZE];
};

static void eisp_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPEISPState* s = APPLE_SEP_EISP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif

    switch (addr) {
        default:
            memcpy(&s->base_regs[addr], &data, size);
            DPRINTF("SEP EISP_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t eisp_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPEISPState* s   = APPLE_SEP_EISP(opaque);
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif

    switch (addr) {
        default:
            memcpy(&ret, &s->base_regs[addr], size);
            DPRINTF("SEP EISP_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps eisp_base_reg_ops = {
    .write                 = eisp_base_reg_write,
    .read                  = eisp_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void eisp_hmac_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPEISPState* s = APPLE_SEP_EISP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif

    switch (addr) {
        default:
            memcpy(&s->hmac_regs[addr], &data, size);
            DPRINTF("SEP EISP_HMAC: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t eisp_hmac_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPEISPState* s   = APPLE_SEP_EISP(opaque);
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif

    switch (addr) {
        default:
            memcpy(&ret, &s->hmac_regs[addr], size);
            DPRINTF("SEP EISP_HMAC: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps eisp_hmac_reg_ops = {
    .write                 = eisp_hmac_reg_write,
    .read                  = eisp_hmac_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_eisp_reset_enter(Object* obj, ResetType type)
{
    AppleSEPEISPState* s = APPLE_SEP_EISP(obj);

    memset(s->base_regs, 0, sizeof(s->base_regs));
    memset(s->hmac_regs, 0, sizeof(s->hmac_regs));
}

static void apple_sep_eisp_realize(DeviceState* dev, Error** errp)
{
    AppleSEPEISPState* s   = APPLE_SEP_EISP(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(s), &eisp_base_reg_ops, s, "base", EISP_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
    memory_region_init_io(&s->hmac_mr, OBJECT(s), &eisp_hmac_reg_ops, s, "hmac", EISP_HMAC_REG_SIZE);
    sysbus_init_mmio(sbd, &s->hmac_mr);
}

static void apple_sep_eisp_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_eisp_reset_enter;
    dc->realize      = apple_sep_eisp_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPEISPState, apple_sep_eisp, APPLE_SEP_EISP, SYS_BUS_DEVICE)

AppleSEPEISPState* apple_sep_eisp_create(void)
{
    AppleSEPEISPState* s = APPLE_SEP_EISP(qdev_new(TYPE_APPLE_SEP_EISP));

    return s;
}
