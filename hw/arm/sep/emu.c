/*
 * Apple SEP Emulation.
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
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/arm/a13.h"
#include "hw/arm/a9.h"
#include "hw/arm/sep/private.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/nvram/eeprom_at24c.h"
#include "system/address-spaces.h"
#include "system/block-backend-global-state.h"
#include "system/blockdev.h"
#include "hw/arm/dart.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "trace.h"
#include "hw/arm/sep/core.h"
#include "system/hw_accel.h"
#include "system/tcg.h"

/*
 * Interrupts 0x100...:
 * 0x10000: KEY
 * 0x10001: MISC2
 * 0x10002: I2C
 * 0x10003: TRNG
 * 0x10004: what is this thing?
 * 0x10005: AES_SEP
 * 0x10006: ?
 * 0x10007: GPIO
 * 0x10008: manual timer
 * 0x10009: AES_HDCP
 * 0xA/0xB/0xC: PKA
 * 0x20/0x21/0x22/0x23/0x24/0x25/0x28/0x29/0x2A/0x2B: EISP. maybe 0x20 .. 0x2B
 * 0x1002C: DART/IOMMU fault
 *
 * PMGRs:
 * 0x00:
 * 0x08:
 * 0x10: AES_HDCP
 * 0x18:
 * 0x20: PKA0
 * 0x28: TRNG
 * 0x30: PKA1?
 * 0x38:
 * 0x40:
 * 0x48: I2C
 * 0x50:
 * 0x58: KEY
 * 0x60: EISP
 * 0x68: SEPD
 */

#define SEP_SHMBUF_BASE (SEPFW_MAPPING_SIZE + 0xC000)

struct AppleSEPClass
{
    /*< private >*/
    SysBusDeviceClass base_class;

    /*< public >*/
    DeviceRealize    parent_realize;
    ResettablePhases parent_phases;
};

static void apple_sep_iop_start(AppleA7IOP* s)
{
    AppleSEP* sep = container_of(s, AppleSEP, parent_obj);

    trace_apple_sep_iop_start(s->iop_mailbox->role);

    apple_sep_boot_monitor_jump(sep->boot_monitor);
}

static void apple_sep_iop_wakeup(AppleA7IOP* s)
{
    AppleSEP* sep = container_of(s, AppleSEP, parent_obj);

    trace_apple_sep_iop_wakeup(s->iop_mailbox->role);

    // TODO
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented", __func__);
}

static const AppleA7IOPOps apple_sep_iop_ops = {
    .start  = apple_sep_iop_start,
    .wakeup = apple_sep_iop_wakeup,
};

void ck_sep_seprom_patches(CKPatcherRange* range)
{
    // cbz/0x34 for A11/A12/A13, tbz/0x36 for A14/M1
    static const uint8_t memcmp_0x30[] = {
        0xa8, 0x00, 0x00, 0x34,    // cbz/tbz w8, 0x...
        0xe2, 0x07, 0x1c, 0x32,    // orr w2, wzr, #0x30
        0x00, 0x00, 0x00, 0x97,    // bl memcmp
    };
    static const uint8_t memcmp_0x14[] = {
        0xa8, 0x00, 0x00, 0x34,    // cbz/tbz w8, 0x...
        0x82, 0x02, 0x80, 0x52,    // mov w2, #0x14
        0x00, 0x00, 0x00, 0x97,    // bl memcmp
    };
    static const uint8_t memcmp_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(memcmp_0x30) != sizeof(memcmp_mask));
    QEMU_BUILD_BUG_ON(sizeof(memcmp_0x14) != sizeof(memcmp_mask));
    static const uint8_t repl[] = {MOV_W0_0_BYTES};
    ck_patcher_find_replace(range, "memcmp_validstrs30", memcmp_0x30, memcmp_mask, sizeof(memcmp_0x30),
                            sizeof(uint32_t), repl, NULL, 8, sizeof(repl));
    ck_patcher_find_replace(range, "memcmp_validstrs14", memcmp_0x14, memcmp_mask, sizeof(memcmp_0x14),
                            sizeof(uint32_t), repl, NULL, 8, sizeof(repl));

    // Doesn't match for A11
    static const uint8_t verify_rsa_signature[] = {
        0x00, 0x01, 0x00, 0x13,    // sbfx w0, w8, #0, #0x1
        0x7f, 0x03, 0x00, 0x91,    // mov sp, x27
    };
    ck_patcher_find_replace(range, "verify_rsa_signature", verify_rsa_signature, NULL, sizeof(verify_rsa_signature),
                            sizeof(uint32_t), repl, NULL, 0, sizeof(repl));
}

typedef struct
{
    hwaddr iova;
    hwaddr pa;
    hwaddr len;
} AppleSEPDMARange;

static MemoryRegion* apple_sep_cpu_memory(AppleSEP* s)
{
    if (s->modern) { return &APPLE_A13(s->cpu)->memory; }

    return &APPLE_A9(s->cpu)->memory;
}

typedef struct
{
    AppleSEP*    sep;
    MemoryRegion mr;
    hwaddr       pa;
    uint8_t*     data;
} AppleSEPShadowPage;

static void apple_sep_dma_refresh(AppleSEP* s);

static uint64_t apple_sep_shadow_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPShadowPage* page = opaque;
    uint64_t            val  = 0;

    memcpy(&val, page->data + addr, size);
    return val;
}

static void apple_sep_shadow_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPShadowPage* page = opaque;

    memcpy(page->data + addr, &data, size);
    qemu_bh_schedule(page->sep->dma_rebuild_bh);
}

static const MemoryRegionOps apple_sep_shadow_ops = {
    .read                  = apple_sep_shadow_read,
    .write                 = apple_sep_shadow_write,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void apple_sep_shadow_free(gpointer data)
{
    AppleSEPShadowPage* page = data;

    memory_region_del_subregion(get_system_memory(), &page->mr);
    object_unparent(OBJECT(&page->mr));
    g_free(page);
}

static void apple_sep_shadow_add(AppleSEP* s, hwaddr pa)
{
    AppleSEPShadowPage* page;
    g_autofree char*    name = NULL;
    uint64_t            size = SEP_DART_PAGE_SIZE;

    pa &= ~(size - 1);

    if (g_hash_table_contains(s->dma_shadow_pages, GUINT_TO_POINTER(pa))) { return; }

    page      = g_new0(AppleSEPShadowPage, 1);
    page->sep = s;
    page->pa  = pa;
    name      = g_strdup_printf("sep_dart_table_0x%" HWADDR_PRIx, pa);

    memory_region_init_rom_device(&page->mr, OBJECT(s), &apple_sep_shadow_ops, page, name, size, &error_fatal);
    page->data = memory_region_get_ram_ptr(&page->mr);
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, page->data, size);
    memory_region_add_subregion_overlap(get_system_memory(), pa, &page->mr, 2);

    g_hash_table_insert(s->dma_shadow_pages, GUINT_TO_POINTER(pa), page);
    // info_report("sep dma: shadowing dart table page 0x%" HWADDR_PRIx, pa);
}

static void apple_sep_dma_shadow_tables(AppleSEP* s)
{
    g_autoptr(GArray) pages    = g_array_new(false, false, sizeof(hwaddr));
    g_autoptr(GHashTable) live = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter iter;
    gpointer       key;

    apple_dart_iommu_table_pages(IOMMU_MEMORY_REGION(s->ool_mr), pages);

    for (guint i = 0; i < pages->len; i++) {
        hwaddr pa = g_array_index(pages, hwaddr, i) & ~(SEP_DART_PAGE_SIZE - 1);

        apple_sep_shadow_add(s, pa);
        g_hash_table_add(live, GUINT_TO_POINTER(pa));
    }

    g_hash_table_iter_init(&iter, s->dma_shadow_pages);

    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(live, key)) { g_hash_table_iter_remove(&iter); }
    }
}

static void apple_sep_dma_clear_windows(AppleSEP* s)
{
    for (guint i = 0; i < s->dma_windows->len; i++) {
        MemoryRegion* win = g_ptr_array_index(s->dma_windows, i);

        memory_region_del_subregion(win->container, win);
        object_unparent(OBJECT(win));
    }

    g_ptr_array_set_size(s->dma_windows, 0);
}

static void apple_sep_dma_add_window(AppleSEP* s, MemoryRegion* parent, hwaddr iova, hwaddr len, MemoryRegion* target,
                                     hwaddr offset)
{
    MemoryRegion*    win  = g_new0(MemoryRegion, 1);
    g_autofree char* name = g_strdup_printf("sep_dma_0x%" HWADDR_PRIx, iova);

    memory_region_init_alias(win, OBJECT(s), name, target, offset, len);
    memory_region_add_subregion_overlap(parent, iova, win, 1);
    g_ptr_array_add(s->dma_windows, win);
}

static gint apple_sep_dma_range_cmp(gconstpointer a, gconstpointer b)
{
    const AppleSEPDMARange* x = a;
    const AppleSEPDMARange* y = b;

    return x->iova < y->iova ? -1 : x->iova > y->iova ? 1 : 0;
}

static void apple_sep_dma_rebuild(void* opaque) { apple_sep_dma_refresh(opaque); }

static void apple_sep_dma_build_windows(AppleSEP* s)
{
    memory_region_transaction_begin();
    apple_sep_dma_clear_windows(s);

    g_array_sort(s->dma_pending, apple_sep_dma_range_cmp);

    for (guint i = 0; i < s->dma_pending->len;) {
        AppleSEPDMARange    run = g_array_index(s->dma_pending, AppleSEPDMARange, i);
        MemoryRegionSection section;
        guint               j;

        for (j = i + 1; j < s->dma_pending->len; j++) {
            AppleSEPDMARange* next = &g_array_index(s->dma_pending, AppleSEPDMARange, j);

            if (next->iova != run.iova + run.len || next->pa != run.pa + run.len) { break; }

            run.len += next->len;
        }

        i = j;

        section = memory_region_find(get_system_memory(), run.pa, run.len);

        if (section.mr == NULL) {
            // warn_report("sep dma: unbacked 0x%" HWADDR_PRIx "+0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx, run.iova, run.len,
            //             run.pa);
            continue;
        }

        if (memory_region_is_ram(section.mr) && int128_get64(section.size) == run.len) {
            apple_sep_dma_add_window(s, get_system_memory(), run.iova, run.len, section.mr,
                                     section.offset_within_region);
            // info_report("sep dma: 0x%" HWADDR_PRIx "+0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx, run.iova, run.len, run.pa);
        }
        else {
            // warn_report("sep dma: skipped 0x%" HWADDR_PRIx "+0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx " (%s)", run.iova,
            //             run.len, run.pa, section.mr->name);
        }

        memory_region_unref(section.mr);
    }

    memory_region_transaction_commit();
}

static void apple_sep_dma_refresh(AppleSEP* s)
{
    s->dma_refreshing = true;

    apple_sep_dma_shadow_tables(s);

    g_array_set_size(s->dma_pending, 0);
    memory_region_iommu_replay(IOMMU_MEMORY_REGION(s->ool_mr), &s->dma_notifier);
    apple_sep_dma_build_windows(s);

    s->dma_refreshing = false;
}

static void apple_sep_dma_notify(IOMMUNotifier* n, IOMMUTLBEntry* entry)
{
    AppleSEP*        s = container_of(n, AppleSEP, dma_notifier);
    AppleSEPDMARange range;

    if ((entry->perm & IOMMU_RW) == 0) {
        if (!s->dma_refreshing) { qemu_bh_schedule(s->dma_rebuild_bh); }
        return;
    }

    if (entry->iova >= SEP_DMA_IOVA_SIZE) { return; }

    range.iova = entry->iova;
    range.pa   = entry->translated_addr;
    range.len  = entry->addr_mask + 1;

    g_array_append_val(s->dma_pending, range);
}

static void apple_sep_dma_init(AppleSEP* s)
{
    if (!hwaccel_enabled()) { return; }

    s->dma_pending      = g_array_new(false, false, sizeof(AppleSEPDMARange));
    s->dma_windows      = g_ptr_array_new();
    s->dma_shadow_pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, apple_sep_shadow_free);
    s->dma_rebuild_bh   = qemu_bh_new(apple_sep_dma_rebuild, s);

    iommu_notifier_init(&s->dma_notifier, apple_sep_dma_notify, IOMMU_NOTIFIER_ALL, 0, HWADDR_MAX, 0);
    memory_region_register_iommu_notifier(s->ool_mr, &s->dma_notifier, &error_fatal);
}

static void apple_sep_tz0_window(AppleSEP* s, MemoryRegion* win, MemoryRegion* mirror, const char* name,
                                 MemoryRegion* dram, hwaddr base, hwaddr offset, uint64_t size)
{
    if (s->tz0_wins_inited) {
        memory_region_set_alias_offset(win, offset);
        memory_region_set_size(win, size);

        if (hwaccel_enabled()) {
            memory_region_set_alias_offset(mirror, offset);
            memory_region_set_size(mirror, size);
        }
    }
    else {
        g_autofree char* mirror_name = g_strdup_printf("%s_mirror", name);

        memory_region_init_alias(win, OBJECT(s), name, dram, offset, size);
        memory_region_add_subregion(apple_sep_cpu_memory(s), base, win);

        if (hwaccel_enabled()) {
            memory_region_init_alias(mirror, OBJECT(s), mirror_name, dram, offset, size);
            memory_region_add_subregion(get_system_memory(), base, mirror);
        }
    }
}

void apple_sep_setup_tz0(AppleSEP* s, MemoryRegion* dram, hwaddr tz0_off, hwaddr tz0_size)
{
    uint64_t msg = SEP_OPCODE17_INTEGRITY_TREE_SIZE;
    hwaddr   tz0_end;
    hwaddr   tree;
    hwaddr   r3_off;
    hwaddr   r3_size;

    tz0_end = tz0_off + tz0_size;
    tree    = ((msg & 0x1FFFFF) << 8) + (msg << 11) + ((msg & 0x1FFFFF) << 4);

    if (tree > tz0_size) {
        error_setg(&error_fatal, "TZ0 (0x%" HWADDR_PRIx ") smaller than the SEP integrity tree (0x%" HWADDR_PRIx ")",
                   tz0_size, tree);
        return;
    }

    r3_off  = (tz0_off + tree + 0x3FFULL) & ~0x3FFULL;
    r3_size = tz0_end <= r3_off ? 0 : (tz0_end - r3_off) & ~0x3FFFULL;

    memory_region_transaction_begin();
    apple_sep_tz0_window(s, &s->tz0_win_r2, &s->tz0_mirror_r2, "sep_tz0_r2", dram, SEP_APERTURE_REGION2, tz0_off,
                         SEP_REGION2_SIZE);
    apple_sep_tz0_window(s, &s->tz0_win_r3, &s->tz0_mirror_r3, "sep_tz0_r3", dram, SEP_APERTURE_REGION3, r3_off,
                         r3_size);
    s->tz0_wins_inited = true;
    memory_region_transaction_commit();
}

AppleSEP* apple_sep_from_node(AppleDTNode* node, MemoryRegion* ool_mr, vaddr base, uint32_t cpu_id, bool modern,
                              uint32_t chip_id)
{
    DeviceState* dev;
    AppleA7IOP*  a7iop;
    AppleSEP*    s;
    AppleDTProp* prop;
    uint64_t*    reg;
    uint32_t     i;

    dev   = qdev_new(TYPE_APPLE_SEP);
    a7iop = APPLE_A7IOP(dev);
    s     = APPLE_SEP(dev);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    apple_a7iop_init(a7iop, "SEP", reg[1], modern ? APPLE_A7IOP_V4 : APPLE_A7IOP_V2, &apple_sep_iop_ops, NULL);
    s->base    = base;
    s->modern  = modern;
    s->chip_id = chip_id;

#ifdef SEP_ENABLE_TRACE_BUFFER
    if (s->chip_id >= 0x8020) {
        if (s->chip_id == 0x8020) { assert_not_reached(); }
        s->shmbuf_base = SEP_SHMBUF_BASE;
        apple_sep_debug_trace_set_region(s->debug_trace, 0x10000, 0x10000);
    }
    else if (s->chip_id == 0x8015) {
        s->shmbuf_base = 0;    // is dynamic
        apple_sep_debug_trace_set_region(s->debug_trace, 0x10000, 0x10000);
    }
    else if (s->chip_id == 0x8000) {
        s->shmbuf_base = 0;    // is dynamic ???
        apple_sep_debug_trace_set_region(s->debug_trace, 0x10000, 0x10000);
    }
    else {
        assert_not_reached();
    }
#endif

    if (modern) { s->cpu = &apple_a13_create("sep-cpu", cpu_id, BIT_ULL(30), -1, 'S')->parent_obj; }
    else {
        s->cpu = &apple_a9_create("sep-cpu", cpu_id, BIT_ULL(30))->parent_obj;
        object_property_set_bool(OBJECT(s->cpu), "aarch64", false, NULL);
    }

    if (tcg_enabled()) {
        MemoryRegion* mr0 = g_new0(MemoryRegion, 1);
        memory_region_init_alias(mr0, OBJECT(s), "sep_dma", ool_mr, 0, SEP_DMA_MAPPING_SIZE);
        memory_region_add_subregion(apple_sep_cpu_memory(s), 0, mr0);
    }

#ifdef SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS
    // hack
    if (s->chip_id >= 0x8020) {
        MemoryRegion* mr1 = g_new0(MemoryRegion, 1);
        memory_region_init_alias(mr1, OBJECT(s), "sep_shmbuf_hdr", ool_mr, s->shmbuf_base, 0x4000);
        memory_region_add_subregion(get_system_memory(), s->shmbuf_base, mr1);
    }
#endif
    object_property_set_uint(OBJECT(s->cpu), "rvbar", s->base & ~0xFFF, NULL);
    object_property_add_child(OBJECT(dev), DEVICE(s->cpu)->id, OBJECT(s->cpu));

    // AKF_MBOX reg is handled using the device tree
    // XPRT_{PMSC,FUSE,MISC} regs are handled in t8030.c

    AppleDTNode* child = apple_dt_get_node(node, "iop-sep-nub");
    assert_nonnull(child);

    SysBusDevice* gpio                = NULL;
    uint32_t      sep_gpio_pins       = 0x4;
    uint32_t      sep_gpio_int_groups = 0x1;
    gpio = SYS_BUS_DEVICE(apple_gpio_new("sep_gpio", 0x10000, sep_gpio_pins, sep_gpio_int_groups));
    assert_nonnull(gpio);
    if (s->chip_id == 0x8030) {
        sysbus_mmio_map(gpio, 0, 0x2414C0000ULL);    // T8030
    }
    else if (s->chip_id == 0x8020) {
        sysbus_mmio_map(gpio, 0, 0x241480000ULL);    // T8020
    }
    else if (s->chip_id == 0x8015) {
        sysbus_mmio_map(gpio, 0, 0x240F00000ULL);    // T8015
    }
    else if (s->chip_id == 0x8000) {
        sysbus_mmio_map(gpio, 0, 0x20DF00000ULL);    // S8000
    }

    for (i = 0; i < sep_gpio_int_groups; i++) {
        // sysbus_connect_irq(gpio, i,
        // qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    }
    for (i = 0; i < sep_gpio_pins; i++) {
        // qdev_connect_gpio_out(DEVICE(gpio), i,
        // qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    }
    object_property_add_child(OBJECT(s), "gpio", OBJECT(gpio));
    sysbus_realize_and_unref(gpio, &error_fatal);
    SysBusDevice* i2c = NULL;
    i2c               = apple_i2c_create("sep_i2c");
    assert_nonnull(i2c);
    object_property_add_child(OBJECT(s), "i2c", OBJECT(i2c));
    if (s->chip_id == 0x8030) {
        sysbus_mmio_map(i2c, 0, 0x241480000ULL);    // T8030
    }
    else if (s->chip_id == 0x8020) {
        sysbus_mmio_map(i2c, 0, 0x241440000ULL);    // T8020
    }
    else if (s->chip_id == 0x8015) {
        sysbus_mmio_map(i2c, 0, 0x240700000ULL);    // T8015
    }
    else if (s->chip_id == 0x8000) {
        sysbus_mmio_map(i2c, 0, 0x20D700000ULL);    // S8000
    }
    sysbus_realize_and_unref(i2c, &error_fatal);
    uint64_t nvram_size = 64 * KiB;

    DriveInfo* dinfo_eeprom = drive_get_by_index(IF_PFLASH, 0);
    assert_nonnull(dinfo_eeprom);
    BlockBackend* blk_eeprom = blk_by_legacy_dinfo(dinfo_eeprom);
    assert_nonnull(blk_eeprom);
    I2CSlave* nvram = at24c_eeprom_init_rom_blk(APPLE_I2C(i2c)->bus, 0x51, nvram_size, NULL, 0, 2, blk_eeprom);
    assert_nonnull(nvram);
    s->nvram = nvram;
    if (s->chip_id >= 0x8020) {
        DriveInfo* dinfo_ssc = drive_get_by_index(IF_PFLASH, 1);
        assert_nonnull(dinfo_ssc);
        BlockBackend* blk_ssc = blk_by_legacy_dinfo(dinfo_ssc);
        assert_nonnull(blk_ssc);
        blk_set_perm(blk_ssc, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, &error_fatal);
        AppleSEPSSCState* ssc = apple_sep_ssc_create(APPLE_I2C(i2c), 0x71, s, blk_ssc);
        assert_nonnull(ssc);
    }

    object_property_add_child(OBJECT(s), "aess", OBJECT(s->aess = apple_sep_aess_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->aess), &error_fatal);
    object_property_add_child(OBJECT(s), "aesh", OBJECT(s->aesh = apple_sep_aesh_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->aesh), &error_fatal);
    object_property_add_child(OBJECT(s), "aesc", OBJECT(s->aesc = apple_sep_aesc_create()));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->aesc), &error_fatal);
    object_property_add_child(OBJECT(s), "boot_monitor", OBJECT(s->boot_monitor = apple_sep_boot_monitor_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->boot_monitor), &error_fatal);
#ifdef SEP_ENABLE_TRACE_BUFFER
    object_property_add_child(OBJECT(s), "debug_trace", OBJECT(s->debug_trace = apple_sep_debug_trace_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->debug_trace), &error_fatal);
#endif
    object_property_add_child(OBJECT(s), "eisp", OBJECT(s->eisp = apple_sep_eisp_create()));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->eisp), &error_fatal);
    object_property_add_child(OBJECT(s), "key", OBJECT(s->key = apple_sep_key_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->key), &error_fatal);
    object_property_add_child(OBJECT(s), "misc", OBJECT(s->misc = apple_sep_misc_create()));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->misc), &error_fatal);
    object_property_add_child(OBJECT(s), "monitor", OBJECT(s->monitor = apple_sep_monitor_create()));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->monitor), &error_fatal);
    object_property_add_child(OBJECT(s), "pka", OBJECT(s->pka = apple_sep_pka_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pka), &error_fatal);
    object_property_add_child(OBJECT(s), "pmgr", OBJECT(s->pmgr = apple_sep_pmgr_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pmgr), &error_fatal);
    object_property_add_child(OBJECT(s), "progress", OBJECT(s->progress = apple_sep_progress_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->progress), &error_fatal);
    object_property_add_child(OBJECT(s), "trng", OBJECT(s->trng = apple_sep_trng_create(s)));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->trng), &error_fatal);

#if 1
    s->ool_mr = ool_mr;
    assert_nonnull(s->ool_mr);
    assert_nonnull(object_property_add_const_link(OBJECT(s), "ool-mr", OBJECT(s->ool_mr)));
    s->ool_as = g_new0(AddressSpace, 1);
    assert_nonnull(s->ool_as);
    address_space_init(s->ool_as, s->ool_mr, "sep.ool");
#endif

    apple_sep_dma_init(s);

    s->mailbox = s->parent_obj.iop_mailbox;

    return s;
}

static void apple_sep_cpu_reset_work(CPUState* cpu, run_on_cpu_data data)
{
    AppleSEP* s = data.host_ptr;
    object_property_set_uint(OBJECT(s->cpu), "rvbar", s->base & ~0xFFF, NULL);
    cpu_reset(cpu);
    DPRINTF("apple_sep_cpu_reset_work: before cpu_set_pc: base=0x%" VADDR_PRIX "\n", s->base);
    cpu_set_pc(cpu, s->base);
    cpu_synchronize_post_reset(cpu);
}

static void apple_sep_realize(DeviceState* dev, Error** errp)
{
    AppleSEP*      s;
    AppleSEPClass* sc;

    s  = APPLE_SEP(dev);
    sc = APPLE_SEP_GET_CLASS(dev);

    if (sc->parent_realize) { sc->parent_realize(dev, errp); }

    qdev_realize(DEVICE(s->cpu), NULL, errp);
    qdev_connect_gpio_out_named(DEVICE(s->mailbox), APPLE_A7IOP_SEP_CPU_IRQ, 0,
                                qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    // mailbox irq's aren't being handled that way (anymore)
    // timer0 == phys
    qdev_connect_gpio_out(DEVICE(s->cpu), GTIMER_PHYS,
                          qdev_get_gpio_in_named(DEVICE(s->mailbox), APPLE_A7IOP_SEP_GPIO_TIMER0, 0));
    // timer1 == virt (sepos >= 16)
    qdev_connect_gpio_out(DEVICE(s->cpu), GTIMER_VIRT,
                          qdev_get_gpio_in_named(DEVICE(s->mailbox), APPLE_A7IOP_SEP_GPIO_TIMER1, 0));
}

static void apple_sep_send_message(AppleSEP* s, uint8_t ep, uint8_t tag, uint8_t op, uint8_t param, uint32_t data)
{
    AppleA7IOP*        a7iop = &s->parent_obj;
    AppleA7IOPMessage* sent_msg;
    SEPMessage*        sent_sep_msg;

    sent_msg            = g_new0(AppleA7IOPMessage, 1);
    sent_sep_msg        = (SEPMessage*)sent_msg->data;
    sent_sep_msg->ep    = ep;
    sent_sep_msg->tag   = tag;
    sent_sep_msg->op    = op;
    sent_sep_msg->param = param;
    sent_sep_msg->data  = data;
    ////apple_a7iop_send_ap(a7iop, sent_msg);
    apple_a7iop_send_iop(a7iop, sent_msg);
}

static void apple_sep_reset_hold(Object* obj, ResetType type)
{
    AppleSEP*      s;
    AppleSEPClass* sc;

    s  = APPLE_SEP(obj);
    sc = APPLE_SEP_GET_CLASS(obj);

    if (sc->parent_phases.hold != NULL) { sc->parent_phases.hold(obj, type); }

    // apple_ssc_reset is being called, but not here.
    run_on_cpu(CPU(s->cpu), apple_sep_cpu_reset_work, RUN_ON_CPU_HOST_PTR(s));

    // iBoot would send those requests. iOS warns about the
    // responses, because it doesn't expect them.
    // SEP's mailbox inbox clearing is really happening before, I checked.
    apple_sep_send_message(s, 0xFF, 0x67, 3, 0x00, 0x00);
    DPRINTF("SEP Progress: Sent fake GenerateNonce\n");
    // we have no damn idea what this opcode is, but if tz0
    // isn't large enough compared to the value derived from this data,
    // it whines. this value is for t8030, straight from the decompiler.
    // INTEGRITY_TREE_SIZE/arms
    apple_sep_send_message(s, 0xFF, 0x0, 17, 0x00, 0x8000);
    DPRINTF("SEP Progress: Sent fake Opcode17/INTEGRITY_TREE_SIZE\n");
}

static void apple_sep_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    AppleSEPClass*   sc = APPLE_SEP_CLASS(klass);
    device_class_set_parent_realize(dc, apple_sep_realize, &sc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, apple_sep_reset_hold, NULL, &sc->parent_phases);
    dc->desc = "Apple SEP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_TYPE_CLASS_INIT(AppleSEP, apple_sep, APPLE_SEP, APPLE_A7IOP)

bool apple_sep_get_fuse_changer_bit(AppleSEP* s, uint8_t bit)
{ return apple_sep_pmgr_get_fuse_changer_bit(s->pmgr, bit); }

void apple_sep_set_fw(AppleSEP* s, hwaddr sep_fw_addr, gchar* fw_data, gsize sep_fw_size)
{
    s->sep_fw_addr = sep_fw_addr;
    s->fw_data     = fw_data;
    s->sep_fw_size = sep_fw_size;
}

void apple_sep_map_mmio(AppleSEP* s, AppleSEPMMIOIndex mmio_index, hwaddr addr)
{
    SysBusDevice* sbd;
    int           i = 0;

    switch (mmio_index) {
        case SEP_MMIO_INDEX_PMGR     : sbd = SYS_BUS_DEVICE(s->pmgr); break;
        case SEP_MMIO_INDEX_TRNG_REGS: sbd = SYS_BUS_DEVICE(s->trng); break;
        case SEP_MMIO_INDEX_KEY      : sbd = SYS_BUS_DEVICE(s->key); break;
        case SEP_MMIO_INDEX_KEY_FKEY:
            sbd = SYS_BUS_DEVICE(s->key);
            i   = 1;
            break;
        case SEP_MMIO_INDEX_KEY_FCFG:
            sbd = SYS_BUS_DEVICE(s->key);
            i   = 2;
            break;
        case SEP_MMIO_INDEX_MONI: sbd = SYS_BUS_DEVICE(s->monitor); break;
        case SEP_MMIO_INDEX_MONI_THRM:
            sbd = SYS_BUS_DEVICE(s->monitor);
            i   = 1;
            break;
        case SEP_MMIO_INDEX_EISP: sbd = SYS_BUS_DEVICE(s->eisp); break;
        case SEP_MMIO_INDEX_EISP_HMAC:
            sbd = SYS_BUS_DEVICE(s->eisp);
            i   = 1;
            break;
        case SEP_MMIO_INDEX_AESS: sbd = SYS_BUS_DEVICE(s->aess); break;
        case SEP_MMIO_INDEX_AESH: sbd = SYS_BUS_DEVICE(s->aesh); break;
        case SEP_MMIO_INDEX_AESC: sbd = SYS_BUS_DEVICE(s->aesc); break;
        case SEP_MMIO_INDEX_PKA : sbd = SYS_BUS_DEVICE(s->pka); break;
        case SEP_MMIO_INDEX_PKA_TMM:
            sbd = SYS_BUS_DEVICE(s->pka);
            i   = 1;
            break;
        case SEP_MMIO_INDEX_MISC0: sbd = SYS_BUS_DEVICE(s->misc); break;
        case SEP_MMIO_INDEX_MISC1:
            sbd = SYS_BUS_DEVICE(s->misc);
            i   = 1;
            break;
        case SEP_MMIO_INDEX_MISC2:
            sbd = SYS_BUS_DEVICE(s->misc);
            i   = 2;
            break;
        case SEP_MMIO_INDEX_PROGRESS    : sbd = SYS_BUS_DEVICE(s->progress); break;
        case SEP_MMIO_INDEX_BOOT_MONITOR: sbd = SYS_BUS_DEVICE(s->boot_monitor); break;
        default                         : assert_not_reached();
    }

    sysbus_mmio_map(sbd, i, addr);
}

ARMCPU* apple_sep_get_cpu(AppleSEP* s) { return s->cpu; }
