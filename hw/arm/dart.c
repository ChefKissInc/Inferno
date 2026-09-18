/*
 * Apple Device Address Resolution Table.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/dart.h"
#include "hw/arm/dt.h"
#include "hw/irq.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "monitor/hmp-target.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "system/dma.h"
#include "qobject/qdict.h"

#if 0
    #define DPRINTF(fmt, ...)                             \
        do {                                              \
            fprintf(stderr, "dart: " fmt, ##__VA_ARGS__); \
        }                                                 \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

#define DART_TLB_ENTRIES     (256)
#define DART_TLB_VALID       BIT_ULL(63)
#define DART_TLB_GEN_SHIFT   (48)
#define DART_TLB_GEN_MASK    (0x7FFFULL)
#define DART_TLB_IOVA_MASK   (BIT_ULL(DART_TLB_GEN_SHIFT) - 1)
#define DART_MAX_STREAMS     (16)
#define DART_MAX_TTBR        (4)
#define DART_MAX_VA_BITS     (38)
#define DART_MAX_TLB_OP_SETS (1)

enum
{
    DART_TLB_OP_INVALIDATE = 0,
};

// clang-format off
REG32(DAPF_CONFIG, 0x0)
REG32(DAPF_SID, 0x4)
REG32(DAPF_START_LSB, 0x8)
REG32(DAPF_START_MSB, 0xC)
REG32(DAPF_END_LSB, 0x10)
REG32(DAPF_END_MSB, 0x14)
// DAPF REG STRIDE 0x40

REG32(DART_PARAMS1, 0x0)
    REG_FIELD(DART_PARAMS1, PAGE_SHIFT, 24, 4)
    REG_FIELD(DART_PARAMS1, ACCESS_REGION_PROTECTION, 31, 1)
REG32(DART_PARAMS2, 0x4)
    REG_FIELD(DART_PARAMS2, BYPASS_SUPPORT, 0, 1)
    REG_FIELD(DART_PARAMS2, LOCK_SUPPORT, 1, 1)
// 0x8..0x1C = ??
REG32(DART_TLB_OP, 0x20)
    REG_FIELD(DART_TLB_OP, OP, 0, 2)
    REG_FIELD(DART_TLB_OP, BUSY, 2, 1)
    REG_FIELD(DART_TLB_OP, HARDWARE_FLUSH, 3, 1)
    REG_FIELD(DART_TLB_OP, WAY_INDEX, 4, 4)
    REG_FIELD(DART_TLB_OP, TE_INDEX, 8, 3)
    REG_FIELD(DART_TLB_OP, SET_INDEX, 20, 12)
// 0x24..0x30 = ??
REG32(DART_TLB_OP_SET_0_LOW, 0x34)
REG32(DART_TLB_OP_SET_0_HIGH, 0x38)
// 0x3C = ??
REG32(DART_ERROR_STATUS, 0x40)
    REG_FIELD(DART_ERROR_STATUS, TTBR_INVLD, 0, 1)
    REG_FIELD(DART_ERROR_STATUS, L2E_INVLD, 1, 1)
    REG_FIELD(DART_ERROR_STATUS, PTE_INVLD, 2, 1)
    REG_FIELD(DART_ERROR_STATUS, WRITE_PROT, 3, 1)
    REG_FIELD(DART_ERROR_STATUS, READ_PROT, 4, 1)
    REG_FIELD(DART_ERROR_STATUS, AXI_SLV_DECODE, 5, 1)
    REG_FIELD(DART_ERROR_STATUS, AXI_SLV_ERR, 6, 1)
    REG_FIELD(DART_ERROR_STATUS, REGION_PROT, 7, 1)
    REG_FIELD(DART_ERROR_STATUS, CTRR_WRITE_PROT, 8, 1)
    REG_FIELD(DART_ERROR_STATUS, UNKNOWN, 9, 1)
    REG_FIELD(DART_ERROR_STATUS, APF_REJECT, 11, 1)
    REG_FIELD(DART_ERROR_STATUS, SID, 24, 4)
    REG_FIELD(DART_ERROR_STATUS, FLAG, 31, 1)
// 0x44..0x4C = ??
REG32(DART_ERROR_ADDRESS_LOW, 0x50)
REG32(DART_ERROR_ADDRESS_HIGH, 0x54)
// 0x58..0x5C = ??
REG32(DART_CONFIG, 0x60)
    REG_FIELD(DART_CONFIG, LOCK, 15, 1)
#define A_DART_SID_REMAP(sid) (0x80 + ((sid) << 2))
#define R_DART_SID_REMAP(sid) (A_DART_SID_REMAP(sid) >> 2)
REG32(DART_SID_VALID, 0xFC)
#define A_DART_SID_CONFIG(sid) (0x100 + ((sid) << 2))
#define R_DART_SID_CONFIG(sid) (A_DART_SID_CONFIG(sid) >> 2)
    REG_FIELD(DART_SID_CONFIG, DISABLE_TTBR_INVALID_ERR, 0, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_STE_INVALID_ERR, 1, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_PTE_INVALID_ERR, 2, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_WRITE_PROTECT_EXCEPTION, 3, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_READ_PROTECT_EXCEPTION, 4, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_AXI_RRESP_EXCEPTION, 6, 1)
    REG_FIELD(DART_SID_CONFIG, TRANSLATION_ENABLE, 7, 1)
    REG_FIELD(DART_SID_CONFIG, FULL_BYPASS, 8, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_DROP_PROTECT_EXCEPTION, 9, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_APF_REJECT_EXCEPTION, 11, 1)
    REG_FIELD(DART_SID_CONFIG, APF_BYPASS, 12, 1)
    REG_FIELD(DART_SID_CONFIG, BYPASS_ADDR_39_32, 16, 8)
#define A_DART_TLB_CONFIG(sid) (0x180 + ((sid) << 2))
#define R_DART_TLB_CONFIG(sid) (A_DART_TLB_CONFIG(sid) >> 2)
#define A_DART_TTBR(sid, idx) \
    (0x200 + (((DART_MAX_STREAMS * (sid)) + (DART_MAX_TTBR * (idx))) << 2))
#define R_DART_TTBR(sid, idx) (A_DART_TTBR(sid, idx) >> 2)
REG_FIELD(DART_TTBR, VALID, 31, 1)
#define DART_TTBR_SHIFT (12)
#define DART_TTBR_MASK (0xFFFFFFF)
REG_FIELD(DART_PTE, NO_WRITE, 7, 1)
REG_FIELD(DART_PTE, NO_READ, 8, 1)
#define DART_PTE_AP_MASK (3 << 7)
#define DART_PTE_VALID (1 << 0) // wut?
#define DART_PTE_TYPE_TABLE (1 << 0)
#define DART_PTE_TYPE_BLOCK (3 << 0)
#define DART_PTE_TYPE_MASK (0x3)
#define DART_PTE_ADDR_MASK (0xFFFFFFFFFFull)
// 0x1000 = ??, default val 0x3B6D
REG32(DART_PERF_SID_ENABLE_LOW, 0x1004)
REG32(DART_PERF_SID_ENABLE_HIGH, 0x1008)
REG32(DART_PERF_STATUS, 0x100C)
REG32(DART_TLB_MISS_CTR, 0x1020)
REG32(DART_TLB_HIT_CTR, 0x1028)
REG32(DART_ST_MISS_CTR, 0x102C)
REG32(DART_ST_HIT_CTR, 0x1034)
// clang-format on

typedef enum
{
    DART_UNKNOWN = 0,
    DART_DART,
    DART_SMMU,
    DART_DAPF,
} dart_instance_t;

static const char* dart_instance_name[] = {
    [DART_UNKNOWN] = "Unknown",
    [DART_DART]    = "DART",
    [DART_SMMU]    = "SMMU",
    [DART_DAPF]    = "DAPF",
};

typedef struct AppleDARTMapperInstance AppleDARTMapperInstance;

typedef struct AppleDARTIOMMUMemoryRegion
{
    IOMMUMemoryRegion        iommu;
    AppleDARTMapperInstance* mapper;
    uint32_t                 sid;
} AppleDARTIOMMUMemoryRegion;

typedef struct
{
    uint32_t params1;
    uint32_t params2;
    uint32_t tlb_op;
    uint64_t tlb_op_set[DART_MAX_TLB_OP_SETS];
    uint32_t error_status;
    uint64_t error_address;
    uint32_t config;
    uint32_t sid_remap[DART_MAX_STREAMS / 4];
    uint32_t sid_config[DART_MAX_STREAMS];
    uint32_t ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
} AppleDARTDARTRegs;

typedef struct
{
    MemoryRegion    iomem;
    AppleDARTState* dart;
    uint32_t        id;
    dart_instance_t type;
} AppleDARTInstance;

typedef struct
{
    uint64_t val;
    uint64_t tag;
} AppleDARTTLBEntry;

struct AppleDARTMapperInstance
{
    AppleDARTInstance           common;
    AppleDARTIOMMUMemoryRegion* iommus[DART_MAX_STREAMS];
    AppleDARTDARTRegs           regs;
    uint32_t                    tlb_gen[DART_MAX_STREAMS];
    AppleDARTTLBEntry           tlb[DART_MAX_STREAMS][DART_TLB_ENTRIES];
};

struct AppleDARTState
{
    SysBusDevice        parent_obj;
    qemu_irq            irq;
    AppleDARTInstance** instances;
    uint32_t            num_instances;
    uint32_t            page_size;
    uint32_t            page_shift;
    uint64_t            page_mask;
    uint64_t            page_bits;
    uint32_t            l_mask[3];
    uint32_t            l_shift[3];
    uint64_t            sid_mask;
    uint32_t            dart_options;
};

static int apple_dart_device_list(Object* obj, void* opaque)
{
    GSList** list = opaque;

    if (object_dynamic_cast(obj, TYPE_APPLE_DART)) { *list = g_slist_append(*list, obj); }

    object_child_foreach(obj, apple_dart_device_list, opaque);
    return 0;
}

static GSList* apple_dart_get_device_list(void)
{
    GSList* list = NULL;

    object_child_foreach(qdev_get_machine(), apple_dart_device_list, &list);

    return list;
}

static void apple_dart_raise_irq(AppleDARTState* dart) { qemu_irq_raise(dart->irq); }

static void apple_dart_update_irq(AppleDARTState* dart)
{
    AppleDARTInstance*       instance;
    AppleDARTMapperInstance* mapper;

    for (uint32_t i = 0; i < dart->num_instances; i++) {
        instance = dart->instances[i];
        if (instance->type != DART_DART) { continue; }

        mapper = container_of(instance, AppleDARTMapperInstance, common);

        if (mapper->regs.error_status == 0) { continue; }

        apple_dart_raise_irq(dart);
        return;
    }

    qemu_irq_lower(dart->irq);
}

static uint8_t apple_dart_sid_remap(AppleDARTMapperInstance* mapper, uint32_t sid)
{ return extract32(qatomic_read(&mapper->regs.sid_remap[sid / 4]), (sid % 4) * 8, 8); }

static void apple_dart_tlb_flush_sid(AppleDARTMapperInstance* mapper, uint32_t sid)
{
    uint32_t gen;

    if (sid >= DART_MAX_STREAMS) { return; }

    gen = qatomic_fetch_inc(&mapper->tlb_gen[sid]) + 1;

    if ((gen & DART_TLB_GEN_MASK) == 0) {
        uint32_t i;

        for (i = 0; i < DART_TLB_ENTRIES; i++) { qatomic_set__nocheck(&mapper->tlb[sid][i].tag, 0); }
    }
}

static void apple_dart_tlb_flush_all(AppleDARTMapperInstance* mapper)
{
    uint32_t sid;

    for (sid = 0; sid < DART_MAX_STREAMS; sid++) { apple_dart_tlb_flush_sid(mapper, sid); }
}

static uint64_t apple_dart_tlb_tag(uint32_t gen, hwaddr iova)
{ return DART_TLB_VALID | (((uint64_t)gen & DART_TLB_GEN_MASK) << DART_TLB_GEN_SHIFT) | (iova & DART_TLB_IOVA_MASK); }

static bool apple_dart_tlb_lookup(AppleDARTMapperInstance* mapper, uint32_t sid, hwaddr iova, uint32_t gen,
                                  IOMMUTLBEntry* tlb_entry)
{
    AppleDARTTLBEntry* e   = &mapper->tlb[sid][iova & (DART_TLB_ENTRIES - 1)];
    uint64_t           tag = apple_dart_tlb_tag(gen, iova);
    uint64_t           val;

    if (qatomic_load_acquire(&e->tag) != tag) { return false; }

    val = qatomic_read__nocheck(&e->val);

    smp_rmb();

    if (qatomic_read__nocheck(&e->tag) != tag) { return false; }

    tlb_entry->translated_addr = val & ~mapper->common.dart->page_bits;
    tlb_entry->perm            = val & IOMMU_RW;

    return true;
}

static void apple_dart_tlb_insert(AppleDARTMapperInstance* mapper, uint32_t sid, hwaddr iova, uint32_t gen,
                                  const IOMMUTLBEntry* tlb_entry)
{
    AppleDARTTLBEntry* e = &mapper->tlb[sid][iova & (DART_TLB_ENTRIES - 1)];
    uint64_t           val;
    uint64_t           tag;

    val = (tlb_entry->translated_addr & ~mapper->common.dart->page_bits) | (tlb_entry->perm & IOMMU_RW);

    qatomic_set__nocheck(&e->tag, 0);

    smp_wmb();

    qatomic_set__nocheck(&e->val, val);

    tag = apple_dart_tlb_tag(gen, iova);

    qatomic_store_release(&e->tag, tag);

    smp_mb();

    if (qatomic_read(&mapper->tlb_gen[sid]) != gen) { qatomic_cmpxchg__nocheck(&e->tag, tag, 0); }
}

static void apple_dart_notify_remap(AppleDARTMapperInstance* mapper, uint32_t index)
{
    IOMMUMemoryRegion* iommu;
    IOMMUNotifier*     notifier;
    IOMMUTLBEvent      event = {0};

    if (index >= DART_MAX_STREAMS || mapper->iommus[index] == NULL) { return; }

    iommu = &mapper->iommus[index]->iommu;

    if (QLIST_EMPTY(&iommu->iommu_notify)) { return; }

    event.type            = IOMMU_NOTIFIER_UNMAP;
    event.entry.target_as = &address_space_memory;
    event.entry.iova      = 0;
    event.entry.perm      = IOMMU_NONE;
    event.entry.addr_mask = HWADDR_MAX;
    memory_region_notify_iommu(iommu, 0, event);

    IOMMU_NOTIFIER_FOREACH (notifier, iommu) { memory_region_iommu_replay(iommu, notifier); }
}

static void apple_dart_mapper_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleDARTMapperInstance* mapper = opaque;
    uint32_t                 val    = data;
    uint32_t                 i;
    uint32_t                 set_index;
    uint64_t                 sid_mask = 0;

    DPRINTF("%s[%d]: (DART) 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx "\n",
            mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id, addr, data);

    switch (addr >> 2) {
        case R_DART_TLB_OP:
            for (i = 0; i < DART_MAX_STREAMS; ++i) {
                if ((mapper->common.dart->sid_mask & BIT_ULL(i)) != 0) { apple_dart_notify_remap(mapper, i); }
            }

            if (REG_FIELD_EX32(val, DART_TLB_OP, OP) != DART_TLB_OP_INVALIDATE
                || REG_FIELD_EX32(val, DART_TLB_OP, SET_INDEX) == 0
                || REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP, BUSY))
            {
                break;
            }

            qatomic_set(&mapper->regs.tlb_op, REG_FIELD_DP32(val, DART_TLB_OP, BUSY, 1));

            set_index = REG_FIELD_EX32(val, DART_TLB_OP, SET_INDEX);

            for (i = 0; i < DART_MAX_TLB_OP_SETS; ++i) {
                if ((set_index & BIT_ULL(i)) == 0) { continue; }

                sid_mask |= qatomic_read__nocheck(&mapper->regs.tlb_op_set[i]) & mapper->common.dart->sid_mask;
            }

            if (sid_mask != 0) {
                for (i = 0; i < DART_MAX_STREAMS; ++i) {
                    if ((sid_mask & BIT_ULL(i)) == 0) { continue; }

                    apple_dart_tlb_flush_sid(mapper, apple_dart_sid_remap(mapper, i));
                    apple_dart_notify_remap(mapper, i);
                }
            }

            qatomic_and(&mapper->regs.tlb_op, ~R_DART_TLB_OP_BUSY_MASK);
            break;
        case R_DART_TLB_OP_SET_0_LOW:
            if (!REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP, BUSY)) {
                qatomic_set__nocheck(&mapper->regs.tlb_op_set[0],
                                     deposit64(qatomic_read__nocheck(&mapper->regs.tlb_op_set[0]), 0, 32, val));
            }
            break;
        case R_DART_TLB_OP_SET_0_HIGH:
            if (!REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP, BUSY)) {
                qatomic_set__nocheck(&mapper->regs.tlb_op_set[0],
                                     deposit64(qatomic_read__nocheck(&mapper->regs.tlb_op_set[0]), 32, 32, val));
            }
            break;
        case R_DART_ERROR_STATUS:
            qatomic_and(&mapper->regs.error_status, ~val);
            apple_dart_update_irq(mapper->common.dart);
            break;
        case R_DART_CONFIG: qatomic_set(&mapper->regs.config, val); break;
        case R_DART_SID_REMAP(0)...(R_DART_SID_REMAP(DART_MAX_STREAMS) - 1):
            i = (addr >> 2) - R_DART_SID_REMAP(0);
            qatomic_set(&mapper->regs.sid_remap[i], val);
            apple_dart_tlb_flush_all(mapper);
            break;
        case R_DART_SID_CONFIG(0)...(R_DART_SID_CONFIG(DART_MAX_STREAMS) - 1):
            i = (addr >> 2) - R_DART_SID_CONFIG(0);
            qatomic_set(&mapper->regs.sid_config[i], val);
            apple_dart_tlb_flush_sid(mapper, i);
            break;
        case R_DART_TTBR(0, 0)...(R_DART_TTBR(DART_MAX_STREAMS, DART_MAX_TTBR) - 1):
            i = (addr >> 2) - R_DART_TTBR(0, 0);
            qatomic_set(&((uint32_t*)mapper->regs.ttbr)[i], val);
            apple_dart_tlb_flush_sid(mapper, i / DART_MAX_TTBR);
            apple_dart_notify_remap(mapper, i / DART_MAX_TTBR);
            break;
        default: break;
    }
}

static uint64_t apple_dart_mapper_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleDARTMapperInstance* mapper = opaque;
    uint32_t                 i;

    DPRINTF("%s[%d]: (DART) 0x" HWADDR_FMT_plx "\n", mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id,
            addr);

    switch (addr >> 2) {
        case R_DART_PARAMS1           : return mapper->regs.params1;
        case R_DART_PARAMS2           : return mapper->regs.params2;
        case R_DART_TLB_OP            : return qatomic_read(&mapper->regs.tlb_op);
        case R_DART_TLB_OP_SET_0_LOW  : return extract64(qatomic_read__nocheck(&mapper->regs.tlb_op_set[0]), 0, 32);
        case R_DART_TLB_OP_SET_0_HIGH : return extract64(qatomic_read__nocheck(&mapper->regs.tlb_op_set[0]), 32, 32);
        case R_DART_ERROR_STATUS      : return qatomic_read(&mapper->regs.error_status);
        case R_DART_ERROR_ADDRESS_LOW : return extract64(qatomic_read__nocheck(&mapper->regs.error_address), 0, 32);
        case R_DART_ERROR_ADDRESS_HIGH: return extract64(qatomic_read__nocheck(&mapper->regs.error_address), 32, 32);
        case R_DART_CONFIG            : return qatomic_read(&mapper->regs.config);
        case R_DART_SID_REMAP(0)...(R_DART_SID_REMAP(DART_MAX_STREAMS) - 1):
            i = (addr >> 2) - R_DART_SID_REMAP(0);
            return qatomic_read(&mapper->regs.sid_remap[i]);
        case R_DART_SID_CONFIG(0)...(R_DART_SID_CONFIG(DART_MAX_STREAMS) - 1):
            i = (addr >> 2) - R_DART_SID_CONFIG(0);
            return qatomic_read(&mapper->regs.sid_config[i]);
        case R_DART_TTBR(0, 0)...(R_DART_TTBR(DART_MAX_STREAMS, DART_MAX_TTBR) - 1):
            i = (addr >> 2) - R_DART_TTBR(0, 0);
            return qatomic_read(&((uint32_t*)mapper->regs.ttbr)[i]);
        default: return 0;
    }
}

static const MemoryRegionOps apple_dart_mapper_reg_ops = {
    .write                 = apple_dart_mapper_reg_write,
    .read                  = apple_dart_mapper_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_dart_dummy_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    G_GNUC_UNUSED AppleDARTInstance* instance = opaque;

    DPRINTF("%s[%d]: (%s) 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx "\n", instance->dart->parent_obj.parent_obj.id,
            instance->id, dart_instance_name[instance->type], addr, data);
}

static uint64_t apple_dart_dummy_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    G_GNUC_UNUSED AppleDARTInstance* instance = opaque;

    DPRINTF("%s[%d]: (%s) 0x" HWADDR_FMT_plx "\n", instance->dart->parent_obj.parent_obj.id, instance->id,
            dart_instance_name[instance->type], addr);

    return 0;
}

static const MemoryRegionOps apple_dart_dummy_reg_ops = {
    .write                 = apple_dart_dummy_reg_write,
    .read                  = apple_dart_dummy_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_dart_mapper_set_error(AppleDARTMapperInstance* mapper, hwaddr addr, uint32_t status, uint32_t sid)
{
    uint32_t old;

    qatomic_set__nocheck(&mapper->regs.error_address, addr);

    do {
        old = qatomic_read(&mapper->regs.error_status);
    }
    while (qatomic_cmpxchg(&mapper->regs.error_status, old, REG_FIELD_DP32(old | status, DART_ERROR_STATUS, SID, sid))
           != old);

    apple_dart_raise_irq(mapper->common.dart);
}

static inline uint32_t apple_dart_mapper_ptw(AppleDARTMapperInstance* mapper, uint32_t sid, hwaddr iova,
                                             IOMMUTLBEntry* tlb_entry)
{
    AppleDARTState* dart = mapper->common.dart;

    uint64_t    idx = (iova & (dart->l_mask[0])) >> dart->l_shift[0];
    uint64_t    pte;
    uint64_t    pa;
    int         level;
    MemTxResult res;

    if (sid >= DART_MAX_STREAMS || (dart->sid_mask & BIT_ULL(sid)) == 0 || idx >= DART_MAX_TTBR) {
        return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, TTBR_INVLD, 1);
    }

    pte = qatomic_read(&mapper->regs.ttbr[sid][idx]);

    if (!REG_FIELD_EX32(pte, DART_TTBR, VALID)) {
        return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, TTBR_INVLD, 1);
    }

    pa = (pte & DART_TTBR_MASK) << DART_TTBR_SHIFT;

    for (level = 1; level < 3; level++) {
        idx  = (iova & (dart->l_mask[level])) >> dart->l_shift[level];
        pa  += 8 * idx;

        pte = address_space_ldq(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &res);
        if (res != MEMTX_OK) {
            return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, L2E_INVLD, 1);
        }
        DPRINTF("%s: level: %d, pa: 0x" HWADDR_FMT_plx " pte: 0x%llx(0x%llx)\n", __func__, level, pa, pte, idx);

        if ((pte & DART_PTE_VALID) == 0) {
            return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, PTE_INVLD, 1);
        }

        pa = pte & dart->page_mask & DART_PTE_ADDR_MASK;
    }

    tlb_entry->translated_addr = (pte & dart->page_mask & DART_PTE_ADDR_MASK);
    tlb_entry->perm =
        IOMMU_ACCESS_FLAG(!REG_FIELD_EX32(pte, DART_PTE, NO_READ), !REG_FIELD_EX32(pte, DART_PTE, NO_WRITE));

    return 0;
}

static void apple_dart_mapper_replay(IOMMUMemoryRegion* mr, IOMMUNotifier* n)
{
    AppleDARTIOMMUMemoryRegion* iommu  = container_of(mr, AppleDARTIOMMUMemoryRegion, iommu);
    AppleDARTMapperInstance*    mapper = iommu->mapper;
    AppleDARTState*             dart   = mapper->common.dart;
    uint32_t                    sid    = apple_dart_sid_remap(mapper, iommu->sid);
    uint64_t                    l1_count;
    uint64_t                    l2_count;
    IOMMUTLBEvent               event = {0};

    if (sid >= DART_MAX_STREAMS || (dart->sid_mask & BIT_ULL(sid)) == 0) { return; }

    l1_count = (dart->l_mask[1] >> dart->l_shift[1]) + 1;
    l2_count = (dart->l_mask[2] >> dart->l_shift[2]) + 1;

    event.entry.target_as = &address_space_memory;
    event.entry.addr_mask = dart->page_bits;
    event.type            = IOMMU_NOTIFIER_MAP;

    for (uint64_t t = 0; t < DART_MAX_TTBR; t++) {
        uint64_t    ttbr = qatomic_read(&mapper->regs.ttbr[sid][t]);
        hwaddr      l1_pa;
        MemTxResult res;

        if (!REG_FIELD_EX32(ttbr, DART_TTBR, VALID)) { continue; }

        l1_pa = (ttbr & DART_TTBR_MASK) << DART_TTBR_SHIFT;

        for (uint64_t i = 0; i < l1_count; i++) {
            uint64_t l1 = address_space_ldq(&address_space_memory, l1_pa + 8 * i, MEMTXATTRS_UNSPECIFIED, &res);
            hwaddr   l2_pa;

            if (res != MEMTX_OK || (l1 & DART_PTE_VALID) == 0) { continue; }

            l2_pa = l1 & dart->page_mask & DART_PTE_ADDR_MASK;

            for (uint64_t j = 0; j < l2_count; j++) {
                uint64_t l2 = address_space_ldq(&address_space_memory, l2_pa + 8 * j, MEMTXATTRS_UNSPECIFIED, &res);

                if (res != MEMTX_OK || (l2 & DART_PTE_VALID) == 0) { continue; }

                event.entry.iova = ((t << dart->l_shift[0]) | (i << dart->l_shift[1]) | (j << dart->l_shift[2]))
                                   << dart->page_shift;
                event.entry.translated_addr = l2 & dart->page_mask & DART_PTE_ADDR_MASK;
                event.entry.perm =
                    IOMMU_ACCESS_FLAG(!REG_FIELD_EX32(l2, DART_PTE, NO_READ), !REG_FIELD_EX32(l2, DART_PTE, NO_WRITE));
                memory_region_notify_iommu_one(n, &event);
            }
        }
    }
}

static int apple_dart_mapper_notify_flag_changed(IOMMUMemoryRegion* mr, IOMMUNotifierFlag old, IOMMUNotifierFlag new,
                                                 Error** errp)
{ return 0; }

static IOMMUTLBEntry apple_dart_mapper_translate(IOMMUMemoryRegion* mr, hwaddr addr, IOMMUAccessFlags flag,
                                                 int iommu_idx)
{
    AppleDARTIOMMUMemoryRegion* iommu  = container_of(mr, AppleDARTIOMMUMemoryRegion, iommu);
    AppleDARTMapperInstance*    mapper = iommu->mapper;
    AppleDARTState*             dart   = mapper->common.dart;
    uint32_t                    sid    = iommu->sid;
    uint32_t                    sid_config;
    uint32_t                    gen;
    uint64_t                    iova;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova      = addr,
        .addr_mask = dart->page_bits,
        .perm      = IOMMU_NONE,
    };

    if (REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP, BUSY)) { return entry; }

    sid = apple_dart_sid_remap(mapper, sid);

    if (sid >= DART_MAX_STREAMS) {
        apple_dart_mapper_set_error(
            mapper, addr,
            REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, TTBR_INVLD, 1),
            iommu->sid);
        goto end;
    }

    sid_config = qatomic_read(&mapper->regs.sid_config[sid]);

    if (REG_FIELD_EX32(sid_config, DART_SID_CONFIG, FULL_BYPASS) != 0) {
        entry.translated_addr = deposit64(addr & ~(hwaddr)dart->page_bits, 32, 8,
                                          REG_FIELD_EX32(sid_config, DART_SID_CONFIG, BYPASS_ADDR_39_32));
        entry.perm            = IOMMU_RW;
        goto end;
    }

    if (REG_FIELD_EX32(sid_config, DART_SID_CONFIG, TRANSLATION_ENABLE) == 0) {
        apple_dart_mapper_set_error(
            mapper, addr,
            REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, TTBR_INVLD, 1),
            iommu->sid);
        goto end;
    }

    iova = addr >> dart->page_shift;
    gen  = qatomic_read(&mapper->tlb_gen[sid]);

    if (!apple_dart_tlb_lookup(mapper, sid, iova, gen, &entry)) {
        uint32_t status = apple_dart_mapper_ptw(mapper, sid, iova, &entry);

        if (status != 0) {
            apple_dart_mapper_set_error(mapper, addr, status, iommu->sid);
            goto end;
        }

        apple_dart_tlb_insert(mapper, sid, iova, gen, &entry);
    }

    entry.translated_addr |= addr & entry.addr_mask;

    if (((flag & IOMMU_WO) != 0 && (entry.perm & IOMMU_WO) == 0)
        || ((flag & IOMMU_RO) != 0 && (entry.perm & IOMMU_RO) == 0))
    {
        apple_dart_mapper_set_error(
            mapper, addr,
            REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1), DART_ERROR_STATUS, WRITE_PROT, 1),
            iommu->sid);
    }

end:
    DPRINTF("%s[%d]: (%s) SID %u: 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx " (%c%c)\n",
            mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id, dart_instance_name[mapper->common.type],
            iommu->sid, entry.iova, entry.translated_addr, (entry.perm & IOMMU_RO) ? 'r' : '-',
            (entry.perm & IOMMU_WO) ? 'w' : '-');
    return entry;
}

static void apple_dart_reset_enter(Object* obj, ResetType type)
{
    AppleDARTState*          dart = APPLE_DART(obj);
    AppleDARTMapperInstance* mapper;
    uint32_t                 i;
    uint32_t                 j;

    for (i = 0; i < dart->num_instances; i++) {
        switch (dart->instances[i]->type) {
            case DART_DART: {
                mapper = container_of(dart->instances[i], AppleDARTMapperInstance, common);

                uint32_t params1 = REG_FIELD_DP32(0, DART_PARAMS1, PAGE_SHIFT, dart->page_shift);

                // TODO: added hack against panic
                params1 =
                    REG_FIELD_DP32(params1, DART_PARAMS1, ACCESS_REGION_PROTECTION, (dart->dart_options & BIT(1)) != 0);

                qatomic_set(&mapper->regs.tlb_op, 0);
                qatomic_set(&mapper->regs.error_status, 0);
                qatomic_set__nocheck(&mapper->regs.error_address, 0);
                qatomic_set(&mapper->regs.config, 0);

                for (j = 0; j < DART_MAX_TLB_OP_SETS; j++) { qatomic_set__nocheck(&mapper->regs.tlb_op_set[j], 0); }

                for (j = 0; j < DART_MAX_STREAMS; j++) {
                    uint32_t k;

                    qatomic_set(&mapper->regs.sid_config[j], 0);
                    if ((j % 4) == 0) {
                        qatomic_set(&mapper->regs.sid_remap[j / 4],
                                    j | ((j + 1) << 8) | ((j + 2) << 16) | ((j + 3) << 24));
                    }

                    for (k = 0; k < DART_MAX_TTBR; k++) { qatomic_set(&mapper->regs.ttbr[j][k], 0); }

                    qatomic_set(&mapper->tlb_gen[j], 1);

                    for (k = 0; k < DART_TLB_ENTRIES; k++) { qatomic_set__nocheck(&mapper->tlb[j][k].tag, 0); }
                }

                mapper->regs.params1 = params1;
                mapper->regs.params2 = REG_FIELD_DP32(0, DART_PARAMS2, BYPASS_SUPPORT, 1);
                break;
            }
            default: break;
        }
    }
}

static void apple_dart_realize(DeviceState* dev, Error** errp) { }

void apple_dart_iommu_table_pages(IOMMUMemoryRegion* mr, GArray* out)
{
    AppleDARTIOMMUMemoryRegion* iommu  = container_of(mr, AppleDARTIOMMUMemoryRegion, iommu);
    AppleDARTMapperInstance*    mapper = iommu->mapper;
    AppleDARTState*             dart   = mapper->common.dart;
    uint32_t                    sid    = apple_dart_sid_remap(mapper, iommu->sid);
    uint64_t                    l1_count;

    if (sid >= DART_MAX_STREAMS || (dart->sid_mask & BIT_ULL(sid)) == 0) { return; }

    l1_count = (dart->l_mask[1] >> dart->l_shift[1]) + 1;

    for (uint64_t t = 0; t < DART_MAX_TTBR; t++) {
        uint64_t    ttbr = qatomic_read(&mapper->regs.ttbr[sid][t]);
        hwaddr      l1_pa;
        MemTxResult res;

        if (!REG_FIELD_EX32(ttbr, DART_TTBR, VALID)) { continue; }

        l1_pa = (ttbr & DART_TTBR_MASK) << DART_TTBR_SHIFT;
        g_array_append_val(out, l1_pa);

        for (uint64_t i = 0; i < l1_count; i++) {
            uint64_t l1 = address_space_ldq(&address_space_memory, l1_pa + 8 * i, MEMTXATTRS_UNSPECIFIED, &res);
            hwaddr   l2_pa;

            if (res != MEMTX_OK || (l1 & DART_PTE_VALID) == 0) { continue; }

            l2_pa = l1 & dart->page_mask & DART_PTE_ADDR_MASK;
            g_array_append_val(out, l2_pa);
        }
    }
}

IOMMUMemoryRegion* apple_dart_iommu_mr(AppleDARTState* dart, uint32_t sid)
{
    AppleDARTInstance*       instance;
    AppleDARTMapperInstance* mapper;
    uint32_t                 i;

    if (dart->sid_mask & BIT_ULL(sid)) {
        for (i = 0; i < dart->num_instances; i++) {
            instance = dart->instances[i];
            if (instance->type != DART_DART) { continue; }

            mapper = container_of(instance, AppleDARTMapperInstance, common);
            return &mapper->iommus[sid]->iommu;
        }
    }
    return NULL;
}

IOMMUMemoryRegion* apple_dart_instance_iommu_mr(AppleDARTState* dart, uint32_t instance, uint32_t sid)
{
    AppleDARTInstance*       o;
    AppleDARTMapperInstance* mapper;

    if (instance >= dart->num_instances || (dart->sid_mask & BIT_ULL(sid)) == 0) { return NULL; }

    o = dart->instances[instance];
    if (o->type != DART_DART) { return NULL; }

    mapper = container_of(o, AppleDARTMapperInstance, common);
    return &mapper->iommus[sid]->iommu;
}

AppleDARTState* apple_dart_from_node(AppleDTNode* node)
{
    DeviceState*    dev;
    AppleDARTState* dart;
    SysBusDevice*   sbd;
    AppleDTProp*    prop;
    uint64_t*       reg;
    uint32_t*       instance_data;
    int             i;

    dev  = qdev_new(TYPE_APPLE_DART);
    dart = APPLE_DART(dev);
    sbd  = SYS_BUS_DEVICE(dev);

    dev->id = apple_dt_get_prop_strdup(node, "name", &error_fatal);

    dart->page_size  = apple_dt_get_prop_u32_or(node, "page-size", 0x1000, &error_fatal);
    dart->page_shift = 31 - clz32(dart->page_size);
    dart->page_bits  = dart->page_size - 1;
    dart->page_mask  = ~dart->page_bits;

    switch (dart->page_shift) {
        case 12:
            dart->l_mask[0]  = 0xC0000;
            dart->l_mask[1]  = 0x3FE00;
            dart->l_mask[2]  = 0x1FF;
            dart->l_shift[0] = 0x12;
            dart->l_shift[1] = 9;
            dart->l_shift[2] = 0;
            break;
        case 14:
            dart->l_mask[0]  = 0xC00000;
            dart->l_mask[1]  = 0x3FF800;
            dart->l_mask[2]  = 0x7FF;
            dart->l_shift[0] = 0x16;
            dart->l_shift[1] = 11;
            dart->l_shift[2] = 0;
            break;
        default: assert_not_reached();
    }

    // NOTE: there can be up to 64 SIDs. Not on the currently-emulated hardware,
    // but other ones.
    dart->sid_mask     = apple_dt_get_prop_u32_or(node, "sids", 0xFFFF, &error_fatal);
    dart->dart_options = apple_dt_get_prop_u32_or(node, "dart-options", 0, &error_fatal);

    prop = apple_dt_get_prop(node, "instance");
    if (prop == NULL) {
        if (apple_dt_get_prop_u32_or(node, "smmu-present", 0, &error_fatal) == 1) {
            instance_data = (uint32_t*)"TRADDART\0\0\0\0UMMSSMMU\0\0\0";
        }
        else {
            instance_data = (uint32_t*)"TRADDART\0\0\0";
        }
    }
    else {
        assert_cmpuint(prop->len % 12, ==, 0);
        instance_data = (uint32_t*)prop->data;
    }

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    dart->num_instances = prop->len / 16;
    dart->instances     = g_new(AppleDARTInstance*, dart->num_instances);
    for (i = 0; i < dart->num_instances; i++) {
        AppleDARTInstance* instance;

        switch (ldl_le_p(instance_data)) {
            case 'DART': {
                AppleDARTMapperInstance* mapper = g_new0(AppleDARTMapperInstance, 1);
                instance                        = &mapper->common;
                instance->type                  = DART_DART;
                memory_region_init_io(&instance->iomem, OBJECT(dev), &apple_dart_mapper_reg_ops, instance,
                                      TYPE_APPLE_DART ".reg", reg[(i * 2) + 1]);
                for (uint32_t sid = 0; sid < DART_MAX_STREAMS; sid++) {
                    if (!(dart->sid_mask & BIT_ULL(sid))) { continue; }

                    char* name = g_strdup_printf("dart-%s-%u-%u", dart->parent_obj.parent_obj.id, instance->id, sid);
                    mapper->iommus[sid]         = g_new0(AppleDARTIOMMUMemoryRegion, 1);
                    mapper->iommus[sid]->sid    = sid;
                    mapper->iommus[sid]->mapper = mapper;
                    memory_region_init_iommu(mapper->iommus[sid], sizeof(AppleDARTIOMMUMemoryRegion),
                                             TYPE_APPLE_DART_IOMMU_MEMORY_REGION, OBJECT(dart), name,
                                             1ULL << DART_MAX_VA_BITS);
                    g_free(name);
                }
                break;
            }
            case 'SMMU':
                instance       = g_new0(AppleDARTInstance, 1);
                instance->type = DART_SMMU;
                goto common;
            case 'DAPF':
                instance       = g_new0(AppleDARTInstance, 1);
                instance->type = DART_DAPF;
                goto common;
            default:
                instance       = g_new0(AppleDARTInstance, 1);
                instance->type = DART_UNKNOWN;
            common:
                memory_region_init_io(&instance->iomem, OBJECT(dev), &apple_dart_dummy_reg_ops, instance,
                                      TYPE_APPLE_DART ".reg", reg[(i * 2) + 1]);
                break;
        }
        instance->id       = i;
        instance->dart     = dart;
        dart->instances[i] = instance;
        sysbus_init_mmio(sbd, &instance->iomem);
        DPRINTF("%s: DART %s instance %d: %s\n", __func__, instance->dart->parent_obj.parent_obj.id, i,
                dart_instance_name[instance->type]);
        instance_data += 3;
    }

    sysbus_init_irq(sbd, &dart->irq);

    return dart;
}

static void apple_dart_dump_pt(Monitor* mon, AppleDARTInstance* instance, hwaddr iova, const uint64_t* entries,
                               int level, uint64_t pte)
{
    AppleDARTState* dart = instance->dart;
    if (level == 3) {
        monitor_printf(mon, "\t\t\t0x" HWADDR_FMT_plx " ... 0x" HWADDR_FMT_plx " -> 0x%llx %c%c\n",
                       iova << dart->page_shift, (iova + 1) << dart->page_shift,
                       pte & dart->page_mask & DART_PTE_ADDR_MASK, REG_FIELD_EX32(pte, DART_PTE, NO_READ) ? '-' : 'r',
                       REG_FIELD_EX32(pte, DART_PTE, NO_WRITE) ? '-' : 'w');
        return;
    }

    for (uint64_t i = 0; i <= (dart->l_mask[level] >> dart->l_shift[level]); i++) {
        uint64_t pte2 = entries[i];

        if ((pte2 & DART_PTE_VALID) || ((level == 0) && REG_FIELD_EX32(pte2, DART_TTBR, VALID))) {
            uint64_t pa = pte2 & dart->page_mask & DART_PTE_ADDR_MASK;
            if (level == 0) { pa = (pte2 & DART_TTBR_MASK) << DART_TTBR_SHIFT; }
            uint64_t next_n_entries = 0;
            if (level < 2) { next_n_entries = (dart->l_mask[level + 1] >> dart->l_shift[level + 1]) + 1; }
            g_autofree uint64_t* next = g_malloc0(8 * next_n_entries);
            if (dma_memory_read(&address_space_memory, pa, next, 8 * next_n_entries, MEMTXATTRS_UNSPECIFIED)
                != MEMTX_OK)
            {
                continue;
            }

            apple_dart_dump_pt(mon, instance, iova | (i << dart->l_shift[level]), next, level + 1, pte2);
        }
    }
}

void hmp_info_dart(Monitor* mon, const QDict* qdict)
{
    const char* name              = qdict_get_try_str(qdict, "name");
    g_autoptr(GSList) device_list = apple_dart_get_device_list();
    AppleDARTState* dart          = NULL;

    if (name == NULL) {
        for (GSList* ele = device_list; ele; ele = ele->next) {
            DeviceState* dev = ele->data;
            dart             = APPLE_DART(dev);
            monitor_printf(mon, "%s\tPage size: %d\t%d Instances\n", dev->id, dart->page_size, dart->num_instances);
        }
        return;
    }

    for (GSList* ele = device_list; ele; ele = ele->next) {
        DeviceState* dev = ele->data;
        if (!strcmp(dev->id, name)) {
            dart = APPLE_DART(dev);
            break;
        }
    }

    if (dart == NULL) {
        monitor_printf(mon, "Cannot find dart %s\n", name);
        return;
    }

    for (uint32_t i = 0; i < dart->num_instances; i++) {
        AppleDARTInstance* instance = dart->instances[i];
        monitor_printf(mon, "\tInstance %d: type: %s\n", i, dart_instance_name[instance->type]);
        if (instance->type != DART_DART) { continue; }
        AppleDARTMapperInstance* mapper = container_of(instance, AppleDARTMapperInstance, common);

        for (int sid = 0; sid < DART_MAX_STREAMS; sid++) {
            if (dart->sid_mask & BIT_ULL(sid)) {
                uint8_t remap = apple_dart_sid_remap(mapper, sid);
                if (sid != remap) {
                    monitor_printf(mon, "\t\tSID %d: Remapped to %d\n", sid, remap);
                    continue;
                }
                if (REG_FIELD_EX32(mapper->regs.sid_config[sid], DART_SID_CONFIG, TRANSLATION_ENABLE) == 0) {
                    monitor_printf(mon, "\t\tSID %d: Translation disabled\n", sid);
                    continue;
                }

                if (REG_FIELD_EX32(mapper->regs.sid_config[sid], DART_SID_CONFIG, FULL_BYPASS) != 0) {
                    monitor_printf(mon, "\t\tSID %d: Translation bypassed\n", sid);
                    continue;
                }
                monitor_printf(mon, "\t\tSID %d:\n", sid);
                const uint64_t l0_entries[] = {mapper->regs.ttbr[sid][0], mapper->regs.ttbr[sid][1],
                                               mapper->regs.ttbr[sid][2], mapper->regs.ttbr[sid][3]};
                apple_dart_dump_pt(mon, instance, 0, l0_entries, 0, 0);
            }
        }
    }
}

static void apple_dart_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_dart_reset_enter;

    dc->realize = apple_dart_realize;
    dc->desc    = "Apple DART IOMMU";
}

static void apple_dart_iommu_memory_region_class_init(ObjectClass* klass, const void* data)
{
    IOMMUMemoryRegionClass* imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate           = apple_dart_mapper_translate;
    imrc->replay              = apple_dart_mapper_replay;
    imrc->notify_flag_changed = apple_dart_mapper_notify_flag_changed;
}

static const TypeInfo apple_dart_info = {
    .name          = TYPE_APPLE_DART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDARTState),
    .class_init    = apple_dart_class_init,
};

static const TypeInfo apple_dart_iommu_memory_region_info = {
    .parent     = TYPE_IOMMU_MEMORY_REGION,
    .name       = TYPE_APPLE_DART_IOMMU_MEMORY_REGION,
    .class_init = apple_dart_iommu_memory_region_class_init,
};

static void apple_dart_register_types(void)
{
    type_register_static(&apple_dart_info);
    type_register_static(&apple_dart_iommu_memory_region_info);
}

type_init(apple_dart_register_types);
