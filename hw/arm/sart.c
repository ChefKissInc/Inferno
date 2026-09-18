/*
 * Apple SART.
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
#include "hw/arm/sart.h"
#include "qapi/error.h"
#include "system/address-spaces.h"

#if 0
    #define DPRINTF(fmt, ...)                    \
        do {                                     \
            printf("sart: " fmt, ##__VA_ARGS__); \
        }                                        \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

#define SART_MAX_VA_BITS (42)
#define SART_NUM_REGIONS (16)

struct AppleSARTIOMMUMemoryRegion
{
    IOMMUMemoryRegion parent_obj;
    AppleSARTState*   s;
};

typedef struct
{
    uint64_t addr;
    uint64_t size;
    uint32_t flags;
} AppleSARTRegion;

struct AppleSARTState
{
    SysBusDevice               parent_obj;
    MemoryRegion               iomem;
    AppleSARTIOMMUMemoryRegion iommu;
    AppleSARTRegion            regions[SART_NUM_REGIONS];
    uint32_t                   version;
    uint32_t                   reg[0x8000 / sizeof(uint32_t)];
};

static inline uint32_t sart_get_reg(AppleSARTState* s, uint32_t offset) { return s->reg[offset / sizeof(uint32_t)]; }

static inline hwaddr sart_get_region_addr(AppleSARTState* s, int region)
{
    assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
        case 1 :
        case 2 : return (sart_get_reg(s, 0x40 + region * sizeof(uint32_t)) >> 0) & 0xFFFFFF;
        case 3 : return (sart_get_reg(s, 0x40 + region * sizeof(uint32_t)) >> 0) & 0x3FFFFFFF;
        default: assert_not_reached(); break;
    }
}

static inline uint64_t sart_get_region_size(AppleSARTState* s, int region)
{
    assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
        case 1 : return (sart_get_reg(s, 0x0 + region * 4) >> 0) & 0x7FFFF;
        case 2 : return (sart_get_reg(s, 0x0 + region * 4) >> 0) & 0xFFFFFF;
        case 3 : return (sart_get_reg(s, 0x80 + region * 4) >> 0) & 0x3FFFFFFF;
        default: assert_not_reached(); break;
    }
}

static inline uint32_t sart_get_region_flags(AppleSARTState* s, int region)
{
    assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
        case 1 : return sart_get_reg(s, 0x0 + region * 4) & ~(0x7FFFFULL << 12);
        case 2 : return sart_get_reg(s, 0x0 + region * 4) & ~(0xFFFFFFULL << 12);
        case 3 : return sart_get_reg(s, 0x0 + region * 4);
        default: assert_not_reached(); break;
    }
}

static void base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSARTState* s     = opaque;
    IOMMUTLBEvent   event = {0};

    DPRINTF("%s: %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", DEVICE(s)->id, __func__, addr, data);

    s->reg[addr / sizeof(uint32_t)] = (uint32_t)data;

    for (int i = 0; i < SART_NUM_REGIONS; i++) {
        if ((sart_get_region_addr(s, i) != s->regions[i].addr) || (sart_get_region_size(s, i) != s->regions[i].size)
            || (sart_get_region_flags(s, i) != s->regions[i].flags))
        {
            hwaddr curr = s->regions[i].addr;
            for (curr = s->regions[i].addr; curr < s->regions[i].addr + s->regions[i].size; curr++) {
                event.type            = IOMMU_NOTIFIER_UNMAP;
                event.entry.target_as = &address_space_memory;
                event.entry.iova      = curr << 12;
                event.entry.perm      = IOMMU_NONE;
                event.entry.addr_mask = 0xFFF;
                memory_region_notify_iommu(IOMMU_MEMORY_REGION(&s->iommu), 0, event);
            }
            s->regions[i].addr  = sart_get_region_addr(s, i);
            s->regions[i].size  = sart_get_region_size(s, i);
            s->regions[i].flags = sart_get_region_flags(s, i);
        }
    }
}

static uint64_t base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSARTState* s = opaque;

    DPRINTF("%s: %s @ 0x" HWADDR_FMT_plx "\n", DEVICE(s)->id, __func__, addr);

    return s->reg[addr >> 2];
}

static const MemoryRegionOps base_reg_ops = {
    .write                 = base_reg_write,
    .read                  = base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static IOMMUTLBEntry apple_sart_translate(IOMMUMemoryRegion* mr, hwaddr addr, IOMMUAccessFlags flag, int iommu_idx)
{
    AppleSARTIOMMUMemoryRegion* iommu;
    AppleSARTState*             s;

    iommu = APPLE_SART_IOMMU_MEMORY_REGION(mr);
    s     = container_of(iommu, AppleSARTState, iommu);

    IOMMUTLBEntry entry = {
        .target_as       = &address_space_memory,
        .iova            = addr & ~0xFFF,
        .translated_addr = addr & ~0xFFF,
        .addr_mask       = 0xFFF,
        .perm            = IOMMU_RW,
    };
    addr >>= 12;

    for (int i = 0; i < SART_NUM_REGIONS; i++) {
        if ((s->regions[i].addr <= addr) && (addr < s->regions[i].addr + s->regions[i].size) && s->regions[i].flags) {
            entry.perm = IOMMU_RW;
            break;
        }
    }
    return entry;
}

static void apple_sart_reset_enter(Object* obj, ResetType type)
{
    AppleSARTState* s = APPLE_SART(obj);

    memset(s->reg, 0, sizeof(s->reg));
    memset(s->regions, 0, sizeof(s->regions));
}

SysBusDevice* apple_sart_from_node(AppleDTNode* node)
{
    DeviceState*    dev;
    AppleSARTState* s;
    SysBusDevice*   sbd;
    AppleDTProp*    prop;
    uint64_t*       reg;

    dev = qdev_new(TYPE_APPLE_SART);
    s   = APPLE_SART(dev);
    sbd = SYS_BUS_DEVICE(dev);

    dev->id = apple_dt_get_prop_strdup(node, "name", &error_fatal);

    // iOS 13 => v1?
    s->version = apple_dt_get_prop_u32_or(node, "sart-version", 1, &error_fatal);
    assert_cmpuint(s->version, >=, 1);
    assert_cmpuint(s->version, <=, 3);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;
    memory_region_init_io(&s->iomem, OBJECT(dev), &base_reg_ops, s, TYPE_APPLE_SART ".reg", reg[1]);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init_iommu(&s->iommu, sizeof(AppleSARTIOMMUMemoryRegion), TYPE_APPLE_SART_IOMMU_MEMORY_REGION,
                             OBJECT(s), dev->id, 1ULL << SART_MAX_VA_BITS);

    sysbus_init_mmio(sbd, MEMORY_REGION(&s->iommu));

    return sbd;
}

static void apple_sart_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sart_reset_enter;

    dc->desc = "Apple SART IOMMU";
}

static void apple_sart_iommu_memory_region_class_init(ObjectClass* klass, const void* data)
{
    IOMMUMemoryRegionClass* imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = apple_sart_translate;
}

static const TypeInfo apple_sart_types[] = {
    {
        .name   = TYPE_APPLE_SART,
        .parent = TYPE_SYS_BUS_DEVICE,
        OBJECT_TYPE_INSTANCE(AppleSARTState),
        .class_init = apple_sart_class_init,
    },
    {
        .parent     = TYPE_IOMMU_MEMORY_REGION,
        .name       = TYPE_APPLE_SART_IOMMU_MEMORY_REGION,
        .class_init = apple_sart_iommu_memory_region_class_init,
    },
};

DEFINE_TYPES(apple_sart_types)
