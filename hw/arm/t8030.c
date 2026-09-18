/*
 * Apple T8030 SoC (iPhone 11).
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
#include "hw/arm/boot.h"
#include "hw/arm/dart.h"
#include "hw/arm/dt.h"
#include "hw/arm/kernel_patches.h"
#include "hw/arm/lm-backlight.h"
#include "hw/arm/mem.h"
#include "hw/input/mt-spi.h"
#include "hw/arm/sart.h"
#include "hw/arm/sep/sim.h"
#include "hw/arm/sep/emu.h"
#include "hw/arm/t8030-config.c.inc"
#include "hw/arm/t8030.h"
#include "hw/audio/aop-audio.h"
#include "hw/audio/cs35l27.h"
#include "hw/audio/cs42l77.h"
#include "hw/audio/mca.h"
#include "hw/block/ans.h"
#include "hw/char/apple_uart.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/display/apple_scaler.h"
#include "hw/display/synopsys_mipi_dsim.h"
#include "hw/dma/apple_sio.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/misc/aes.h"
#include "hw/misc/aop.h"
#include "hw/misc/baseband.h"
#include "hw/input/buttons.h"
#include "hw/misc/chestnut.h"
#include "hw/misc/fan53740.h"
#include "hw/sensor/mic_tempsensor.h"
#include "hw/misc/roswell.h"
#include "hw/misc/smc.h"
#include "hw/misc/spmi-baseband.h"
#include "hw/misc/spmi-pmu.h"
#include "hw/sensor/tempsensor.h"
#include "hw/misc/unimp.h"
#include "hw/nvram/apple_nvram.h"
#include "hw/pci-host/apcie.h"
#include "hw/spmi/apple_spmi.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "hw/usb/apple_typec.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/hw_accel.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "arm-powerctl.h"

#define PROP_VISIT_GETTER_SETTER(_type, _name)                                                           \
    static void t8030_get_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    {                                                                                                    \
        _type##_t value;                                                                                 \
                                                                                                         \
        value = APPLE_T8030(obj)->_name;                                                                 \
        visit_type_##_type(v, name, &value, errp);                                                       \
    }                                                                                                    \
                                                                                                         \
    static void t8030_set_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    { visit_type_##_type(v, name, &APPLE_T8030(obj)->_name, errp); }

#define PROP_STR_GETTER_SETTER(_name)                                                                       \
    static char* t8030_get_##_name(Object* obj, Error** errp) { return g_strdup(APPLE_T8030(obj)->_name); } \
                                                                                                            \
    static void t8030_set_##_name(Object* obj, const char* value, Error** errp)                             \
    {                                                                                                       \
        AppleT8030MachineState* t8030;                                                                      \
                                                                                                            \
        t8030 = APPLE_T8030(obj);                                                                           \
        g_free(t8030->_name);                                                                               \
        t8030->_name = g_strdup(value);                                                                     \
    }

#define PROP_GETTER_SETTER(_type, _name)                                                                       \
    static void t8030_set_##_name(Object* obj, _type value, Error** errp) { APPLE_T8030(obj)->_name = value; } \
                                                                                                               \
    static _type t8030_get_##_name(Object* obj, Error** errp) { return APPLE_T8030(obj)->_name; }

#define SROM_BASE (0x100000000)
#define SROM_SIZE (512 * KiB)

#define SRAM_BASE (0x19C000000)
#define SRAM_SIZE (0x400000)

#define DRAM_BASE (0x800000000)

#define SEPROM_BASE (0x240000000)
#define SEPROM_SIZE (8 * MiB)

#define SPI0_IRQ     (444)
#define GPIO_SPI0_CS (204)
#define SPI0_BASE    (0x35100000)

#define GPIO_FORCE_DFU (161)

#define DISPLAY_SIZE (68 * MiB)

#define DWC2_IRQ (495)

#define NUM_UARTS (9)

#define ANS_SIZE   (0x3D24000)
#define SIO_SIZE   (0x114000)
#define PANIC_SIZE (0x100000)

#define AMCC_BASE                               (0x200000000)
#define AMCC_SIZE                               (0x100000)
#define AMCC_PLANE_COUNT                        (4)
#define AMCC_PLANE_STRIDE                       (0x40000)
#define AMCC_PLANE_BLK_MCC_CHANNEL_DEC(_p)      (0x4 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH0_LO(_p)             (0x8 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH0_HI(_p)             (0xC + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH1_LO(_p)             (0x10 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BLK_HASH1_HI(_p)             (0x14 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_LOWER_LIMIT(_p)              (0x680 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_UPPER_LIMIT(_p)              (0x684 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_ENABLED(_p)                  (0x688 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_LOCK(_p)                     (0x68C + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_BASE(_p)                 (0x6A0 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_END(_p)                  (0x6A4 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_TZ0_LOCK(_p)                 (0x6A8 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH0(_p)  (0x1004 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH1(_p)  (0x1008 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_MCS_ADDR_BANK_HASH2(_p)  (0x100C + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_ADDR_MAP_MODE(_p)    (0x1010 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_ADDR_CFG(_p)         (0x1014 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_MUI_BLK_NUM_MCU_CHANNEL(_p)  (0x1020 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_CACHE_STATUS(_p)             (0x1C00 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_PLANE_BROADCAST(_p)                (0x1000C + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_RREG32(_machine, _off)             ldl_le_p(&_machine->amcc_reg[_off])
#define AMCC_WREG32(_machine, _off, _val)       stl_le_p(&_machine->amcc_reg[_off], _val)
#define AMCC_NON_PLANE_GFX                      (AMCC_BASE + AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE)
#define AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH0   (0x80)
#define AMCC_NON_PLANE_GFX_TAG_RAM_BANK_HASH1   (0x84)
#define AMCC_NON_PLANE_GFX_ADDR_HASH0_LO        (0x100)
#define AMCC_NON_PLANE_GFX_ADDR_HASH0_HI        (0x104)
#define AMCC_NON_PLANE_GFX_ADDR_HASH1_LO        (0x108)
#define AMCC_NON_PLANE_GFX_ADDR_HASH1_HI        (0x10C)
#define AMCC_NON_PLANE_GFX_ADDR_NUM_MCU_CHANNEL (0x118)
#define AMCC_NON_PLANE                          (AMCC_NON_PLANE_GFX + AMCC_PLANE_STRIDE)
#define AMCC_NON_PLANE_TAG_RAM_BANK_HASH0       (0x80)
#define AMCC_NON_PLANE_TAG_RAM_BANK_HASH1       (0x84)
#define AMCC_NON_PLANE_ADDR_HASH0_LO            (0x180)
#define AMCC_NON_PLANE_ADDR_HASH0_HI            (0x184)
#define AMCC_NON_PLANE_ADDR_HASH1_LO            (0x188)
#define AMCC_NON_PLANE_ADDR_HASH1_HI            (0x18C)
#define AMCC_NON_PLANE_ADDR_NUM_MCU_CHANNEL     (0x198)

#define FUSE_ENABLED  (0xA55AC33C)
#define FUSE_DISABLED (0xA050C030)

static uint32_t t8030_real_cpu_count(AppleT8030MachineState* t8030)
{ return MACHINE(t8030)->smp.cpus - (t8030->sep_fw_filename != NULL); }

static void t8030_start_cpus(AppleT8030MachineState* t8030, uint64_t cpu_mask)
{
    uint32_t i;

    for (i = 0; i < t8030_real_cpu_count(t8030); ++i) {
        if ((cpu_mask & BIT_ULL(i)) != 0) { apple_a13_set_on(t8030->cpus[i]); }
    }
}

static void t8030_create_s3c_uart(const AppleT8030MachineState* t8030, uint32_t port, Chardev* chr)
{
    DeviceState* dev;
    hwaddr       base;
    AppleDTProp* prop;
    hwaddr*      uart_offset;
    AppleDTNode* child = apple_dt_get_node(t8030->device_tree, "arm-io/uart0");
    char         name[32];

    assert_cmpuint(port, <, NUM_UARTS);

    assert_nonnull(child);
    snprintf(name, sizeof(name), "uart%u", port);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    uart_offset = (hwaddr*)prop->data;
    base        = t8030->armio_base + uart_offset[0] + uart_offset[1] * port;

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    dev = apple_uart_create(base, 15, 0, chr, qdev_get_gpio_in(DEVICE(t8030->aic), lduw_le_p(prop->data) + port));
    assert_nonnull(dev);
    dev->id = g_strdup(name);
}

static void t8030_patch_kernel(MachoHeader64* header, uint32_t build_version, const bool enable_pac)
{
    ck_patch_kernel(header);

    if (hwaccel_enabled()) { ck_patch_virt(header, enable_pac); }
    else if (enable_pac) {
        error_report("You have specified the PAC enablement property on TCG.");
    }
}

static bool t8030_check_panic(AppleT8030MachineState* t8030)
{
    AppleEmbeddedPanicHeader* panic_info;
    bool                      ret;

    if (t8030->panic_size == 0) { return false; }

    panic_info = g_malloc0(t8030->panic_size);

    address_space_rw(&address_space_memory, t8030->panic_base, MEMTXATTRS_UNSPECIFIED, panic_info, t8030->panic_size,
                     false);
    address_space_set(&address_space_memory, t8030->panic_base, 0, t8030->panic_size, MEMTXATTRS_UNSPECIFIED);

    ret = panic_info->magic == EMBEDDED_PANIC_MAGIC;
    g_free(panic_info);
    return ret;
}

static uint64_t get_kaslr_random(void)
{
    uint64_t value = 0;
    qemu_guest_getrandom(&value, sizeof(value), NULL);
    return value;
}

#define L2_GRANULE      ((0x4000) * (0x4000 / 8))
#define L2_GRANULE_MASK (L2_GRANULE - 1)

static void get_kaslr_slides(AppleT8030MachineState* t8030, hwaddr* phys_slide_out, vaddr* virt_slide_out)
{
    static const size_t slide_granular      = (1 << 21);
    static const size_t slide_granular_mask = slide_granular - 1;
    static const size_t slide_virt_max      = 0x100 * (2 * 1024 * 1024);

    hwaddr slide_phys;
    vaddr  slide_virt;
    size_t random_value = get_kaslr_random();

    if (t8030->kaslr_off) {
        *phys_slide_out = 0;
        *virt_slide_out = 0;
        return;
    }

    slide_virt = (random_value & ~slide_granular_mask) % slide_virt_max;
    if (slide_virt == 0) { slide_virt = slide_virt_max; }
    slide_phys = slide_virt & L2_GRANULE_MASK;

    *phys_slide_out = slide_phys;
    *virt_slide_out = slide_virt;
}

static void t8030_load_kernelcache(AppleT8030MachineState* t8030, const char* cmdline, CarveoutAllocator* ca)
{
    MachineState*  machine = MACHINE(t8030);
    vaddr          kc_base;
    vaddr          kc_end;
    vaddr          ro_lower;
    vaddr          ro_upper;
    vaddr          apple_dt_va;
    hwaddr         mem_size;
    hwaddr         phys_ptr;
    AppleBootInfo* info = &t8030->boot_info;

    apple_boot_get_kc_bounds(t8030->kernel, NULL, &kc_base, &kc_end, &ro_lower, &ro_upper);

    get_kaslr_slides(t8030, &g_phys_slide, &g_virt_slide);

    g_phys_base = DRAM_BASE;
    g_virt_base = kc_base + (g_virt_slide - g_phys_slide);

    info->trustcache_addr = vtop_slid(ro_lower) - info->trustcache_size;
    info_report("TrustCache Physical Base: 0x" HWADDR_FMT_plx, info->trustcache_addr);

    address_space_rw(&address_space_memory, info->trustcache_addr, MEMTXATTRS_UNSPECIFIED, t8030->trustcache,
                     info->trustcache_size, true);

    info->kern_entry = apple_boot_load_macho(t8030->kernel, &address_space_memory,
                                             apple_dt_get_node(t8030->device_tree, "/chosen/memory-map"),
                                             g_phys_base + g_phys_slide, g_virt_slide);

    info_report("Kernel Virtual Base: 0x%016" VADDR_PRIx, g_virt_base);
    info_report("Kernel Physical Base: 0x" HWADDR_FMT_plx, g_phys_base);
    info_report("Kernel Virtual Slide: 0x%016" VADDR_PRIx, g_virt_slide);
    info_report("Kernel Physical Slide: 0x" HWADDR_FMT_plx, g_phys_slide);
    info_report("Kernel Entry Point: 0x%016" VADDR_PRIx, info->kern_entry);

    phys_ptr = vtop_static(ROUND_UP_16K(kc_end + g_virt_slide));

    // RAM Disk
    if (machine->initrd_filename != NULL) {
        info->ramdisk_addr = phys_ptr;
        apple_boot_load_ramdisk(machine->initrd_filename, &address_space_memory, info->ramdisk_addr,
                                &info->ramdisk_size);
        info->ramdisk_size  = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr           += info->ramdisk_size;
    }

    // TZ0
    // 256 MiB doesn't seem to work properly with sepos 18.5
    // SEP's MPIDR influences stack/sp address
    // info->tz0_size = 300 * MiB;
    info->tz0_size = 240 * MiB;    // FIXME: workaround for sepos >= 16

    // for booting sepfw 26 without opcode17:
    // tz0_base must be, at least, aligned to 32 MiB
    phys_ptr = ROUND_UP(phys_ptr, 32 * MiB);

    info->tz0_addr  = phys_ptr;
    phys_ptr       += info->tz0_size;
    info_report("TZ0 Physical Base: 0x" HWADDR_FMT_plx, info->tz0_addr);

    // SEP Firmware
    info->sep_fw_addr = phys_ptr;
    // should be equal to make the mapping work
    info->sep_fw_size = SEPFW_MAPPING_SIZE;
    info_report("SEPFW Physical Base: 0x" HWADDR_FMT_plx, info->sep_fw_addr);
    info_report("SEPFW Size: 0x" HWADDR_FMT_plx, info->sep_fw_size);
    phys_ptr += info->sep_fw_size;

    if (t8030->sep_fw_filename != NULL) {
        AppleSEPState* sep     = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", &error_fatal));
        gchar*         fw_data = NULL;
        gsize          sep_fw_size;
        if (!g_file_get_contents(t8030->sep_fw_filename, &fw_data, &sep_fw_size, NULL)) {
            error_setg(&error_fatal, "Failed to read SEP Firmware from `%s`", t8030->sep_fw_filename);
            return;
        }
        apple_sep_set_fw(sep, info->sep_fw_addr, fw_data, sep_fw_size);
    }

    // Kernel boot args
    info->kern_boot_args_addr  = phys_ptr;
    info->kern_boot_args_size  = 0x4000;
    phys_ptr                  += info->kern_boot_args_size;

    // Device tree
    info->device_tree_addr  = phys_ptr;
    apple_dt_va             = ptov_static(info->device_tree_addr);
    phys_ptr               += info->device_tree_size;
    info_report("Device Tree Physical Base: 0x" HWADDR_FMT_plx, info->device_tree_addr);
    info_report("Device Tree Virtual Base: 0x%016" VADDR_PRIx, apple_dt_va);
    info_report("Device Tree Size: 0x" HWADDR_FMT_plx, info->device_tree_size);

    info->top_of_kernel_data_pa = ROUND_UP_16K(phys_ptr);
    info_report("Top Of Kernel Data: 0x" HWADDR_FMT_plx, info->top_of_kernel_data_pa);

    apple_boot_finalise_dt(t8030->device_tree, &address_space_memory, info);

    mem_size = carveout_alloc_finalise(ca);
    info_report("Memory Size: 0x" HWADDR_FMT_plx, mem_size);

    apple_boot_setup_bootargs(t8030->build_version, &address_space_memory, info->kern_boot_args_addr, g_virt_base,
                              g_phys_base, mem_size, info->top_of_kernel_data_pa, apple_dt_va, info->device_tree_size,
                              &t8030->video_args, cmdline, machine->ram_size);
    g_virt_base = kc_base;

    for (int i = 0; i < AMCC_PLANE_COUNT; ++i) {
        AMCC_WREG32(t8030, AMCC_PLANE_LOWER_LIMIT(i), (info->trustcache_addr - info->dram_base) >> 14);
        AMCC_WREG32(t8030, AMCC_PLANE_UPPER_LIMIT(i), (vtop_slid(ro_upper) - info->dram_base - 1) >> 14);
        AMCC_WREG32(t8030, AMCC_PLANE_LOCK(i), 1);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_BASE(i), (info->tz0_addr - info->dram_base) >> 12);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_END(i), ((info->tz0_addr + info->tz0_size - 1) - info->dram_base) >> 12);
        AMCC_WREG32(t8030, AMCC_PLANE_TZ0_LOCK(i), 1);
        AMCC_WREG32(t8030, AMCC_PLANE_BLK_MCC_CHANNEL_DEC(i), 0x2F);
    }

    if (t8030->sep_rom_filename != NULL || t8030->sep_fw_filename != NULL) {
        apple_sep_setup_tz0(APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", &error_fatal)), machine->ram,
                            info->tz0_addr - info->dram_base, info->tz0_size);
    }
}

static void t8030_rtkit_seg_prop_setup(AppleDTNode* iop_nub, hwaddr base, uint32_t size)
{
    apple_dt_set_prop_u64(iop_nub, "region-base", base);
    apple_dt_set_prop_u64(iop_nub, "region-size", size);
}

static void t8030_rtkit_mem_setup(AppleT8030MachineState* t8030, CarveoutAllocator* ca, const char* name,
                                  const char* nub_name, uint32_t size)
{
    AppleDTNode* child;
    AppleDTNode* iop_nub;

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, nub_name);
    assert_nonnull(iop_nub);

    t8030_rtkit_seg_prop_setup(iop_nub, carveout_alloc_mem(ca, size), size);
}

static void t8030_memory_setup(AppleT8030MachineState* t8030)
{
    AppleDTNode*       carveout_memory_map;
    MachineState*      machine;
    MachoHeader64*     hdr;
    AppleNvramState*   nvram;
    AppleBootInfo*     info;
    AppleDTNode*       memory_map;
    bool               have_autoboot;
    bool               auto_boot;
    char*              cmdline;
    char*              seprom;
    gsize              fsize;
    CarveoutAllocator* ca;
    CKPatcherRange*    seprom_range;

    apple_dt_unfinalise(t8030->device_tree);

    carveout_memory_map = apple_dt_get_node(t8030->device_tree, "/chosen/carveout-memory-map");

    machine    = MACHINE(t8030);
    info       = &t8030->boot_info;
    memory_map = apple_dt_get_node(t8030->device_tree, "/chosen/memory-map");

    if (t8030_check_panic(t8030)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = DRAM_BASE;
    info->dram_size = machine->maxram_size;

    ca = carveout_alloc_new(carveout_memory_map, info->dram_base, info->dram_size, 16 * KiB);

    t8030_rtkit_mem_setup(t8030, ca, "sio", "iop-sio-nub", SIO_SIZE);
    t8030_rtkit_mem_setup(t8030, ca, "ans", "iop-ans-nub", ANS_SIZE);

    if (t8030->sep_rom_filename) {
        if (!g_file_get_contents(t8030->sep_rom_filename, &seprom, &fsize, NULL)) {
            error_setg(&error_fatal, "Could not load data from file '%s'", t8030->sep_rom_filename);
            return;
        }

        seprom_range = ck_patcher_range_from_ptr("seprom", seprom, fsize);
        ck_sep_seprom_patches(seprom_range);

        address_space_rw(&address_space_memory, SEPROM_BASE, MEMTXATTRS_UNSPECIFIED, (uint8_t*)seprom, fsize, true);

        g_free(seprom);
    }

    nvram = APPLE_NVRAM(object_resolve_path_at(NULL, "/machine/peripheral/nvram"));
    if (nvram == NULL) {
        error_setg(&error_fatal, "Failed to find NVRAM device");
        return;
    }
    apple_nvram_load(nvram);

    info_report("Boot Mode: %u", t8030->boot_info.boot_mode);
    switch (t8030->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery:
            auto_boot = false;
            env_unset(nvram, "auto-boot");
            t8030->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        case kAppleBootModeExitRecovery:
            auto_boot = true;
            env_set_bool(nvram, "auto-boot", true, 0);
            t8030->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        default: break;
    }

    have_autoboot = env_find(nvram, "auto-boot") != NULL;
    auto_boot     = env_get_bool(nvram, "auto-boot", false);

    if (t8030->boot_info.non_cold_boot && (!t8030->boot_info.had_autoboot && have_autoboot && auto_boot)) {
        fprintf(stdout, "\n--------\nDetected potentially-completed restore process. "
                        "Turning "
                        "off so the filesystem patches can be performed.\n--------\n");
        exit(0);
        return;
    }

    t8030->boot_info.had_autoboot  = have_autoboot;
    t8030->boot_info.non_cold_boot = true;

    info_report("Auto Boot: %s", auto_boot ? "true" : "false");

    if (machine->initrd_filename == NULL && !auto_boot) {
        error_setg(&error_fatal, "RAM Disk required for recovery, please specify it via `-initrd`.");
        return;
    }

    if (auto_boot) { cmdline = g_strdup(machine->kernel_cmdline); }
    else {
        cmdline = g_strconcat("-restore rd=md0 nand-enable-reformat=1 ", machine->kernel_cmdline, NULL);
    }
    info_report("Boot Args: [%s]", cmdline);

    AppleDTNode* chosen = apple_dt_get_node(t8030->device_tree, "chosen");

    if (env_find(nvram, "security-mode-change-enable") != NULL) {
        uint32_t val = env_get_uint(nvram, "security-mode-change-enable", 0);
        env_unset(nvram, "security-mode-change-enable");
        apple_dt_set_prop_u32(chosen, "security-mode-change-enable", val);
    }

    apple_nvram_save(nvram);

    info->nvram_size = nvram->len;

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) { info->nvram_size = XNU_MAX_NVRAM_SIZE; }
    if (apple_nvram_serialize(nvram, info->nvram_data, sizeof(info->nvram_data)) < 0) {
        error_report("Failed to read NVRAM");
    }

    t8030->video_args.base_addr = carveout_alloc_mem(ca, DISPLAY_SIZE);
    adp_v4_update_vram_mapping(APPLE_DISPLAY_PIPE_V4(object_property_get_link(OBJECT(t8030), "disp0", &error_abort)),
                               machine->ram, t8030->video_args.base_addr - DRAM_BASE, DISPLAY_SIZE);

    if (t8030->securerom_filename != NULL) {
        address_space_rw(&address_space_memory, SROM_BASE, MEMTXATTRS_UNSPECIFIED, t8030->securerom,
                         t8030->securerom_size, 1);
        return;
    }

    // HACK: Use DEV model to restore without FDR errors
    // NOTE: Previously restored back to AP, however,
    // iOS 16+ kernel panics because of the AP ticket,
    // as it names it "n104dev" and then tries to read
    // "n104ap" after restore.
    apple_dt_set_prop(t8030->device_tree, "compatible", 28, "N104DEV\0iPhone12,1\0AppleARM");

    if (!apple_boot_contains_boot_arg(cmdline, "rd=", true)) {
        apple_dt_set_prop_strn(chosen, "root-matching", 256,
                               "<dict><key>IOProviderClass</key><string>IOMedia</"
                               "string><key>IOPropertyMatch</key><dict><key>Partition "
                               "ID</key><integer>1</integer></dict></dict>");
    }

    AppleDTNode* pram = apple_dt_get_node(t8030->device_tree, "pram");
    if (pram != NULL) {
        uint64_t panic_reg[2];
        panic_reg[0] = carveout_alloc_mem(ca, PANIC_SIZE);
        panic_reg[1] = PANIC_SIZE;
        apple_dt_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        apple_dt_set_prop_u64(chosen, "embedded-panic-log-size", panic_reg[1]);
        t8030->panic_base = panic_reg[0];
        t8030->panic_size = panic_reg[1];
    }

    AppleDTNode* vram = apple_dt_get_node(t8030->device_tree, "vram");
    if (vram != NULL) {
        uint64_t vram_reg[2];
        vram_reg[0] = t8030->video_args.base_addr;
        vram_reg[1] = DISPLAY_SIZE;
        apple_dt_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
    }
    // adjust framebuffer for verbose display.
    t8030->video_args.base_addr +=
        adp_v4_get_fb_off(APPLE_DISPLAY_PIPE_V4(object_property_get_link(OBJECT(t8030), "disp0", &error_abort)));

    hdr = t8030->kernel;
    assert_nonnull(hdr);

    apple_boot_allocate_segment_records(memory_map, hdr);

    apple_boot_populate_dt(t8030->device_tree, info, auto_boot);

    switch (hdr->file_type) {
        case MH_EXECUTE:
        case MH_FILESET:
            t8030_load_kernelcache(t8030, cmdline, ca);
            g_free(cmdline);
            break;
        default: error_setg(&error_fatal, "Unsupported kernelcache type: 0x%X\n", hdr->file_type); return;
    }
}

static uint64_t pmgr_unk_e4800            = 0;
static uint32_t pmgr_unk_e4000[0x180 / 4] = {0};

static void pmgr_unk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    hwaddr base = (hwaddr)opaque;
    hwaddr i;

    switch (base + addr) {
        case 0x3D2E4800:    // ???? 0x240002c00 and 0x2400037a4
            pmgr_unk_e4800 = deposit64(pmgr_unk_e4800, 0, size * 8,
                                       data);    // 0x240002c00 and 0x2400037a4
            break;
        case 0x3D2E4804               : pmgr_unk_e4800 = deposit64(pmgr_unk_e4800, 32, 32, data); break;
        case 0x3D2E4000 ... 0x3D2E417f:    // ???? 0x24000377c
            i                 = ((base + addr) - 0x3D2E4000) / 4;
            pmgr_unk_e4000[i] = extract64(data, 0, 32);    // 0x24000377c
            if (size == 8) { pmgr_unk_e4000[i + 1] = extract64(data, 32, 32); }
            break;
        default: break;
    }

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" HWADDR_FMT_plx
                  " base: 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
                  base + addr, base, data);
#endif
}

static uint64_t pmgr_unk_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030 = APPLE_T8030(qdev_get_machine());
    AppleSEPState*          sep;
    hwaddr                  base = (hwaddr)opaque;

#if 0
    if ((((base + addr) & 0xfffffffb) != 0x10E20020) &&
        (((base + addr) & 0xfffffffb) != 0x11E20020)) {
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" HWADDR_FMT_plx
                      " base: 0x" HWADDR_FMT_plx "\n",
                      base + addr, base);
    }
#endif

    uint32_t security_epoch          = 1;    // On IMG4: Security Epoch ; On IMG3: Minimum
                                             // Epoch, verified on SecureROM s5l8955xsi
    bool     current_prod            = true;
    bool     current_secure_mode     = true;    // T8015 SEPOS Kernel also requires this.
    uint32_t security_domain         = 1;
    bool     raw_prod                = true;
    bool     raw_secure_mode         = true;
    uint32_t sep_bit30_current_value = 0;
    bool     fuses_locked            = true;

    switch (base + addr) {
        case 0x3D280088:    // PMGR_AON
            return 0xFF;
        case 0x3D2BC000:    // Current Production Mode
            return current_prod ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x3D2BC400:    // Raw Production Mode
            return raw_prod ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x3D2BC004:    // Current Secure Mode
            sep = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", NULL));

            if (sep != NULL && apple_sep_get_fuse_changer_bit(sep, 1)) {
                current_secure_mode = 0;    // SEP DSEC img4 tag demotion active
            }
            return current_secure_mode ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x3D2BC404:    // Raw Secure Mode
            return raw_secure_mode ? FUSE_ENABLED : FUSE_DISABLED;
        case 0x3D2BC008:    // Current Security Domain Bit 0
        case 0x3D2BC408:    // Raw Security Domain Bit 0
            return (security_domain & BIT32(0)) == 0 ? FUSE_DISABLED : FUSE_ENABLED;
        case 0x3D2BC00C:    // Current Security Domain Bit 1
        case 0x3D2BC40C:    // Raw Security Domain Bit 1
            return (security_domain & BIT32(1)) == 0 ? FUSE_DISABLED : FUSE_ENABLED;
        case 0x3D2BC010:    // Current Board ID and Epoch
            sep = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", NULL));

            if (sep != NULL) { sep_bit30_current_value = (uint32_t)apple_sep_get_fuse_changer_bit(sep, 0) << 30; }
            QEMU_FALLTHROUGH;
        case 0x3D2BC410:    // Raw Board ID and Epoch, bit 30 should remain unset
            return ((t8030->board_id >> 5) & 0x7) | ((security_epoch & 0x7f) << 5) | sep_bit30_current_value
                   | ((uint32_t)fuses_locked << 31);
        case 0x3D2BC020:    // Fuses Sealed?
            return current_prod ? FUSE_ENABLED : FUSE_DISABLED;
        // case 0x3D2BC024: // ?
        //     return ...;
        // case 0x3D2BC028: // ?
        //     return ...;
        case 0x3D2BC02c:                     // Unknown
            return (0 << 31) | BIT32(30);    // bit31 causes a panic
        case 0x3D2BC030:                     // Chip Revision
            return ((t8030->chip_revision & 0x7) << 6) | (((t8030->chip_revision & 0x70) >> 4) << 5) | (0 << 1);
        case 0x3D2BC300:    // ECID LOW T8030
            return extract64(t8030->ecid, 0, 32);
        case 0x3D2BC304:    // ECID HIGH T8030
            return extract64(t8030->ecid, 32, 32);
        case 0x3D2BC30C: {
            // T8030 SEP Chip Revision
            //  1 vs. not 1: TRNG/Monitor
            //  0 vs. not 0: Monitor
            //  2 vs. not 2: ARTM
            //  Production SARS doesn't like value (0 << 28) in combination with
            //  kbkdf_index being 0
            uint32_t environment;
            // environment = 0x0; // 0 ; might be the production value
            // environment = 0x2; // 1
            // environment = 0x3; // 1
            environment = 0x8;    // 2 ; might be a development value, skips a few
                                  // checks (lynx and others)
            return (environment << 28);
        }
        case 0x3D2E8000:    // ????
            // return 0x32B3; // memory encryption AMK (Authentication Master Key)
            // disabled
            return 0xC2E9;    // memory encryption AMK (Authentication Master Key)
                              // enabled
        case 0x3D2E4800:      // sep: likely lock bits: 1 on <= A13, 3 on A14
            return pmgr_unk_e4800;
        case 0x3D2E4000 ... 0x3D2E417F:    // sep: something memory encryption and sepb
            return pmgr_unk_e4000[((base + addr) - 0x3D2E4000) / 4];
        case 0x3C100C4C:    // Could that also have been for T8015? No idea anymore.
            return 0x1;
        default:
            // SOC MTR Temp Sensor0
            if ((base + addr) >= 0x3B281000 && (base + addr) < 0x3B281000 + 0x8000) { return 0xAFAFAFAF; }
            // SOC MTR Temp Sensor3
            if ((base + addr) >= 0x3B289000 && (base + addr) < 0x3B289000 + 0x8000) { return 0xAFAFAFAF; }
            // ANE MTR Temp Sensor0
            if ((base + addr) >= 0x3B299000 && (base + addr) < 0x3B299000 + 0x8000) { return 0xAFAFAFAF; }
            if (((base + addr) & 0x10E70000) == 0x10E70000) {
                return (108 << 4) | 0x200000;    // ?
            }
            return 0;
    }
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write                 = pmgr_unk_reg_write,
    .read                  = pmgr_unk_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

static void pmgr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;
    AppleSEPState*          sep;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
                  "\n",
                  addr, data);
#endif

    if (addr >= 0x80000 && addr <= 0x8C000) {
        data = deposit64(data, 4, 4, extract64(data, 0, 4));
        if (size == 8) { data = deposit64(data, 32 + 4, 4, extract64(data, 32, 4)); }
    }

    switch (addr) {
        case 0xD4004: t8030_start_cpus(t8030, data); return;
        case 0x80C00:    // SEP_PS
            sep = APPLE_SEP(object_property_get_link(OBJECT(t8030), "sep", NULL));

            if (sep != NULL) {
                if (data & BIT32(31)) { apple_a13_reset(APPLE_A13(apple_sep_get_cpu(sep))); }
                else if (data & BIT32(10)) {
                    apple_a13_set_off(APPLE_A13(apple_sep_get_cpu(sep)));
                }
                else {
                    apple_a13_set_on(APPLE_A13(apple_sep_get_cpu(sep)));
                }
            }
            break;
        default: break;
    }

    if (size == 8) { stq_le_p(t8030->pmgr_reg + addr, data); }
    else {
        stl_le_p(t8030->pmgr_reg + addr, data);
    }
}

static uint64_t pmgr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030  = opaque;
    uint64_t                result = 0;

    switch (addr) {
        case 0xF0010:    // AppleT8030PMGR::commonSramCheck
            result = 0x5000;
            break;
        default: result = size == 8 ? ldq_le_p(t8030->pmgr_reg + addr) : ldl_le_p(t8030->pmgr_reg + addr); break;
    }

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg READ @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
                  "\n",
                  addr, result);
#endif

    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write                 = pmgr_reg_write,
    .read                  = pmgr_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

static void amcc_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;

    if ((AMCC_RREG32(t8030, AMCC_PLANE_LOCK(addr / AMCC_PLANE_STRIDE)) != 0
         && (addr % AMCC_PLANE_STRIDE == AMCC_PLANE_LOWER_LIMIT(0)
             || addr % AMCC_PLANE_STRIDE == AMCC_PLANE_UPPER_LIMIT(0)
             || addr % AMCC_PLANE_STRIDE == AMCC_PLANE_LOCK(0)))
        || (AMCC_RREG32(t8030, AMCC_PLANE_TZ0_LOCK(addr / AMCC_PLANE_STRIDE)) != 0
            && (addr % AMCC_PLANE_STRIDE == AMCC_PLANE_TZ0_BASE(0) || addr % AMCC_PLANE_STRIDE == AMCC_PLANE_TZ0_END(0)
                || addr % AMCC_PLANE_STRIDE == AMCC_PLANE_TZ0_LOCK(0))))
    {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx ", this register is LOCKED, merdOS.\n", __func__,
                      addr, data);
        return;
    }

    stl_le_p(t8030->amcc_reg + addr, data);
}

static uint64_t amcc_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleT8030MachineState* t8030 = opaque;
    uint64_t                result;

    result = ldl_le_p(t8030->amcc_reg + addr);

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "AMCC reg READ @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
                  "\n",
                  orig_addr, result);
#endif

    return result;
}

static const MemoryRegionOps amcc_reg_ops = {
    .write                 = amcc_reg_write,
    .read                  = amcc_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void t8030_cluster_setup(AppleT8030MachineState* t8030)
{
    for (uint32_t i = 0; i < A13_MAX_CLUSTER; ++i) {
        char* name = NULL;

        name = g_strdup_printf("cluster%u", i);
        object_initialize_child(OBJECT(t8030), name, &t8030->clusters[i], TYPE_APPLE_A13_CLUSTER);
        g_free(name);
        qdev_prop_set_uint32(DEVICE(&t8030->clusters[i]), "cluster-id", i);
    }
}

static void t8030_cluster_realize(AppleT8030MachineState* t8030)
{
    for (int i = 0; i < A13_MAX_CLUSTER; ++i) { qdev_realize(DEVICE(&t8030->clusters[i]), NULL, &error_fatal); }
}

static void t8030_cpu_setup(AppleT8030MachineState* t8030)
{
    unsigned int i;
    AppleDTNode* root;
    GList*       iter;
    GList*       next = NULL;

    t8030_cluster_setup(t8030);

    root = apple_dt_get_node(t8030->device_tree, "cpus");
    assert_nonnull(root);

    for (iter = root->children, i = 0; iter; iter = next, ++i) {
        uint32_t     cluster_id;
        AppleDTNode* node;

        next = iter->next;
        node = (AppleDTNode*)iter->data;
        if (i >= t8030_real_cpu_count(t8030)) {
            apple_dt_del_node(root, node);
            continue;
        }

        t8030->cpus[i] = apple_a13_from_node(node);
        cluster_id     = t8030->cpus[i]->cluster_id;

        object_property_add_child(OBJECT(&t8030->clusters[cluster_id]), DEVICE(t8030->cpus[i])->id,
                                  OBJECT(t8030->cpus[i]));
        qdev_realize(DEVICE(t8030->cpus[i]), NULL, &error_fatal);
    }
    t8030_cluster_realize(t8030);
}

static void t8030_create_aic(AppleT8030MachineState* t8030)
{
    uint32_t     i;
    hwaddr*      reg;
    AppleDTProp* prop;
    AppleDTNode* soc = apple_dt_get_node(t8030->device_tree, "arm-io");
    AppleDTNode* child;
    AppleDTNode* timebase;

    assert_nonnull(soc);
    child = apple_dt_get_node(soc, "aic");
    assert_nonnull(child);
    timebase = apple_dt_get_node(soc, "aic-timebase");
    assert_nonnull(timebase);

    t8030->aic = apple_aic_create(t8030_real_cpu_count(t8030), child, timebase);
    object_property_add_child(OBJECT(t8030), "aic", OBJECT(t8030->aic));
    assert_nonnull(t8030->aic);
    sysbus_realize(t8030->aic, &error_fatal);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (hwaddr*)prop->data;

    for (i = 0; i < t8030_real_cpu_count(t8030); ++i) {
        memory_region_add_subregion_overlap(&t8030->cpus[i]->memory, t8030->armio_base + reg[0],
                                            sysbus_mmio_get_region(t8030->aic, i), 0);
        sysbus_connect_irq(t8030->aic, i, qdev_get_gpio_in(DEVICE(t8030->cpus[i]), ARM_CPU_IRQ));
    }
}

static void t8030_pmgr_setup(AppleT8030MachineState* t8030)
{
    uint64_t*    reg;
    uint32_t     i;
    char         name[32];
    AppleDTProp* prop;
    AppleDTNode* child = apple_dt_get_node(t8030->device_tree, "arm-io");

    assert_nonnull(child);
    child = apple_dt_get_node(child, "pmgr");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    for (i = 0; i < prop->len / 8; i += 2) {
        MemoryRegion* mem = g_new(MemoryRegion, 1);
        if (i > 0) {
            snprintf(name, 32, "pmgr-unk-reg-%u", i);
            memory_region_init_io(mem, OBJECT(t8030), &pmgr_unk_reg_ops, (void*)reg[i], name, reg[i + 1]);
        }
        else {
            memory_region_init_io(mem, OBJECT(t8030), &pmgr_reg_ops, t8030, "pmgr-reg", reg[i + 1]);
        }
        memory_region_add_subregion(get_system_memory(),
                                    reg[i] + reg[i + 1] < t8030->armio_size ? t8030->armio_base + reg[i] : reg[i], mem);
    }

    MemoryRegion* mem = g_new(MemoryRegion, 1);

    snprintf(name, 32, "pmp-reg");
    memory_region_init_io(mem, OBJECT(t8030), &pmgr_unk_reg_ops, (void*)0x3BC00000, name, 0x60000);
    memory_region_add_subregion(get_system_memory(), t8030->armio_base + 0x3BC00000, mem);

    apple_dt_set_prop(child, "voltage-states0", sizeof(t8030_voltage_states0), t8030_voltage_states0);
    apple_dt_set_prop(child, "voltage-states1", sizeof(t8030_voltage_states1), t8030_voltage_states1);
    apple_dt_set_prop(child, "voltage-states2", sizeof(t8030_voltage_states2), t8030_voltage_states2);
    apple_dt_set_prop(child, "voltage-states5", sizeof(t8030_voltage_states5), t8030_voltage_states5);
    apple_dt_set_prop(child, "voltage-states8", sizeof(t8030_voltage_states8), t8030_voltage_states8);
    apple_dt_set_prop(child, "voltage-states9", sizeof(t8030_voltage_states9), t8030_voltage_states9);
    apple_dt_set_prop(child, "voltage-states10", sizeof(t8030_voltage_states10), t8030_voltage_states10);
    apple_dt_set_prop(child, "voltage-states11", sizeof(t8030_voltage_states11), t8030_voltage_states11);

    apple_dt_set_prop(child, "voltage-states1-sram", sizeof(t8030_voltage_states1_sram), t8030_voltage_states1_sram);
    apple_dt_set_prop(child, "voltage-states2-sram", sizeof(t8030_voltage_states2_sram), t8030_voltage_states2_sram);
    apple_dt_set_prop(child, "voltage-states5-sram", sizeof(t8030_voltage_states5_sram), t8030_voltage_states5_sram);
    apple_dt_set_prop(child, "voltage-states8-sram", sizeof(t8030_voltage_states8_sram), t8030_voltage_states8_sram);
    apple_dt_set_prop(child, "voltage-states9-sram", sizeof(t8030_voltage_states9_sram), t8030_voltage_states9_sram);

    apple_dt_set_prop(child, "bridge-settings-0", sizeof(t8030_bridge_settings0), t8030_bridge_settings0);
    apple_dt_set_prop(child, "bridge-settings-1", sizeof(t8030_bridge_settings1), t8030_bridge_settings1);
    apple_dt_set_prop(child, "bridge-settings-2", sizeof(t8030_bridge_settings2), t8030_bridge_settings2);
    apple_dt_set_prop(child, "bridge-settings-3", sizeof(t8030_bridge_settings3), t8030_bridge_settings3);
    apple_dt_set_prop(child, "bridge-settings-4", sizeof(t8030_bridge_settings4), t8030_bridge_settings4);
    apple_dt_set_prop(child, "bridge-settings-5", sizeof(t8030_bridge_settings5), t8030_bridge_settings5);
    apple_dt_set_prop(child, "bridge-settings-6", sizeof(t8030_bridge_settings6), t8030_bridge_settings6);
    apple_dt_set_prop(child, "bridge-settings-7", sizeof(t8030_bridge_settings7), t8030_bridge_settings7);
    apple_dt_set_prop(child, "bridge-settings-8", sizeof(t8030_bridge_settings8), t8030_bridge_settings8);
    apple_dt_set_prop(child, "bridge-settings-12", sizeof(t8030_bridge_settings12), t8030_bridge_settings12);
    apple_dt_set_prop(child, "bridge-settings-13", sizeof(t8030_bridge_settings13), t8030_bridge_settings13);
    apple_dt_set_prop(child, "bridge-settings-14", sizeof(t8030_bridge_settings14), t8030_bridge_settings14);
    apple_dt_set_prop(child, "bridge-settings-15", sizeof(t8030_bridge_settings15), t8030_bridge_settings15);
    apple_dt_set_prop(child, "bridge-settings-16", sizeof(t8030_bridge_settings16), t8030_bridge_settings16);
    apple_dt_set_prop(child, "bridge-settings-17", sizeof(t8030_bridge_settings17), t8030_bridge_settings17);
}

static void t8030_amcc_setup(AppleT8030MachineState* t8030)
{
    AppleDTNode* child;

    child = apple_dt_get_node(t8030->device_tree, "chosen/lock-regs/amcc");
    if (child != NULL) {
        apple_dt_set_prop_u32(child, "aperture-count", 1);
        apple_dt_set_prop_u32(child, "aperture-size", AMCC_PLANE_COUNT * AMCC_PLANE_STRIDE + 0x4000);
        apple_dt_set_prop_u32(child, "plane-count", AMCC_PLANE_COUNT);
        apple_dt_set_prop_u32(child, "plane-stride", AMCC_PLANE_STRIDE);
        apple_dt_set_prop_u64(child, "aperture-phys-addr", AMCC_BASE);
        apple_dt_set_prop_u32(child, "cache-status-reg-offset", AMCC_PLANE_CACHE_STATUS(0));
        apple_dt_set_prop_u32(child, "cache-status-reg-mask", 0x1F);
        apple_dt_set_prop_u32(child, "cache-status-reg-value", 0);
        apple_dt_set_prop_u32(child, "broadcast-reg-offset", 0x100c0);
        apple_dt_set_prop_u32(child, "broadcast-reg-value", 1);

        child = apple_dt_get_node(child, "amcc-ctrr-a");
        assert_nonnull(child);

        apple_dt_set_prop_u32(child, "page-size-shift", 14);
        apple_dt_set_prop_u32(child, "lower-limit-reg-offset", AMCC_PLANE_LOWER_LIMIT(0));
        apple_dt_set_prop_u32(child, "lower-limit-reg-mask", 0xFFFFFFFF);
        apple_dt_set_prop_u32(child, "upper-limit-reg-offset", AMCC_PLANE_UPPER_LIMIT(0));
        apple_dt_set_prop_u32(child, "upper-limit-reg-mask", 0xFFFFFFFF);
        apple_dt_set_prop_u32(child, "lock-reg-offset", AMCC_PLANE_LOCK(0));
        apple_dt_set_prop_u32(child, "lock-reg-mask", 1);
        apple_dt_set_prop_u32(child, "lock-reg-value", 1);
        apple_dt_set_prop_u32(child, "enable-reg-offset", AMCC_PLANE_ENABLED(0));
        apple_dt_set_prop_u32(child, "enable-reg-value", 1);
        apple_dt_set_prop_u32(child, "enable-reg-mask", 1);
    }

    memory_region_init_io(&t8030->amcc, OBJECT(t8030), &amcc_reg_ops, t8030, "amcc", AMCC_SIZE);
    memory_region_add_subregion(get_system_memory(), AMCC_BASE, &t8030->amcc);
}

static void t8030_create_dart(AppleT8030MachineState* t8030, const char* name, bool absolute_mmio)
{
    AppleDARTState* dart = NULL;
    AppleDTProp*    prop;
    uint64_t*       reg;
    uint32_t*       ints;
    uint32_t        i;
    AppleDTNode*    child;

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "vm-base");
    if (prop != NULL && ldl_le_p(prop->data) == 0) { stl_le_p(prop->data, 0x4000); }

    dart = apple_dart_from_node(child);
    assert_nonnull(dart);
    object_property_add_child(OBJECT(t8030), name, OBJECT(dart));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    for (i = 0; i < prop->len / 16; ++i) {
        sysbus_mmio_map(SYS_BUS_DEVICE(dart), i, (absolute_mmio ? 0 : t8030->armio_base) + reg[i * 2]);
    }

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dart), i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
}

static void t8030_create_sart(AppleT8030MachineState* t8030)
{
    uint64_t*     reg;
    AppleDTNode*  child;
    AppleDTProp*  prop;
    SysBusDevice* sart;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/sart-ans");
    assert_nonnull(child);

    sart = apple_sart_from_node(child);
    assert_nonnull(sart);
    object_property_add_child(OBJECT(t8030), "sart-ans", OBJECT(sart));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sart, 0, t8030->armio_base + reg[0]);
    sysbus_realize_and_unref(sart, &error_fatal);
}

static void t8030_create_ans(AppleT8030MachineState* t8030)
{
    uint32_t       i;
    uint32_t*      ints;
    AppleDTProp*   prop;
    uint64_t*      reg;
    SysBusDevice*  sart;
    SysBusDevice*  ans;
    AppleDTNode*   child = apple_dt_get_node(t8030->device_tree, "arm-io");
    AppleDTNode*   iop_nub;
    ApplePCIEHost* apcie_host;

    assert_nonnull(child);
    child = apple_dt_get_node(child, "ans");
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, "iop-ans-nub");
    assert_nonnull(iop_nub);

    t8030_create_sart(t8030);
    sart = SYS_BUS_DEVICE(object_property_get_link(OBJECT(t8030), "sart-ans", &error_fatal));

    // bridge0 and bridge1 don't exist in the t8030 device tree
    // at least one bridge must exist
    // NVMe can be e.g. attached to bridge2 in order to test the apcie subsystem
    // for that, change the bridge? string and the bridge_index variable below.
    // the number must match

    PCIDevice* pci_dev    = PCI_DEVICE(object_property_get_link(OBJECT(t8030), "pcie.bridge0", &error_fatal));
    PCIBridge* pci_bridge = PCI_BRIDGE(pci_dev);
    PCIBus*    sec_bus    = pci_bridge_get_sec_bus(pci_bridge);
    apcie_host            = APPLE_PCIE_HOST(object_property_get_link(OBJECT(t8030), "pcie.host", &error_fatal));
    ans                   = apple_ans_from_node(child, APPLE_A7IOP_V4, sec_bus);
    assert_nonnull(ans);
    object_property_add_const_link(OBJECT(ans), "dma-mr", OBJECT(sysbus_mmio_get_region(sart, 1)));

    object_property_add_child(OBJECT(t8030), "ans", OBJECT(ans));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    for (i = 0; i < 4; ++i) { sysbus_mmio_map(ans, i, t8030->armio_base + reg[i << 1]); }

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 20);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(ans, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

#if 1
    // needed for ANS
    uint32_t bridge_index = 0;
    qdev_connect_gpio_out_named(DEVICE(apcie_host), "interrupt_pci", bridge_index,
                                qdev_get_gpio_in_named(DEVICE(ans), "interrupt_pci", 0));
#endif

    sysbus_realize_and_unref(ans, &error_fatal);
}

static void t8030_create_baseband(AppleT8030MachineState* t8030)
{
    int            i;
    uint32_t*      ints;
    AppleDTProp*   prop;
    uint64_t*      reg;
    SysBusDevice*  baseband;
    AppleDTNode*   child = apple_dt_get_node(t8030->device_tree, "baseband");
    ApplePCIEHost* apcie_host;
    DeviceState*   gpio = NULL;
    int            coredump_pin, reset_det_pin;

    assert_nonnull(child);

    ApplePCIEPort* port       = APPLE_PCIE_PORT(object_property_get_link(OBJECT(t8030), "pcie.bridge3", &error_fatal));
    PCIDevice*     pci_dev    = PCI_DEVICE(port);
    PCIBridge*     pci_bridge = PCI_BRIDGE(pci_dev);
    PCIBus*        sec_bus    = pci_bridge_get_sec_bus(pci_bridge);
    apcie_host                = port->host;
    baseband                  = apple_baseband_create(child, sec_bus, port);
    assert_nonnull(baseband);
    object_property_add_child(OBJECT(t8030), "baseband", OBJECT(baseband));
    sysbus_realize_and_unref(baseband, &error_fatal);
#if 1
    apple_dt_connect_function_prop_out_in_gpio(DEVICE(baseband), apple_dt_get_prop(child, "function-coredump"),
                                               BASEBAND_GPIO_COREDUMP);
    apple_dt_connect_function_prop_in_out_gpio(DEVICE(baseband), apple_dt_get_prop(child, "function-reset_det"),
                                               BASEBAND_GPIO_RESET_DET_OUT);
#endif
    // the interrupts prop in this node seem to be actually for baseband-spmi.

#if 0
    uint32_t bridge_index = 3;
    qdev_connect_gpio_out_named(
        DEVICE(apcie_host), "interrupt_pci", bridge_index,
        qdev_get_gpio_in_named(DEVICE(baseband), "interrupt_pci", 0));
#endif
}

static void t8030_create_gpio(AppleT8030MachineState* t8030, const char* name)
{
    DeviceState* gpio = NULL;
    AppleDTProp* prop;
    uint64_t*    reg;
    uint32_t*    ints;
    uint32_t     i;
    AppleDTNode* child = apple_dt_get_node(t8030->device_tree, "arm-io");

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);
    gpio = apple_gpio_from_node(child);
    assert_nonnull(gpio);
    object_property_add_child(OBJECT(t8030), name, OBJECT(gpio));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, t8030->armio_base + reg[0]);
    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(gpio), i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
}

static void t8030_create_i2c(AppleT8030MachineState* t8030, const char* name)
{
    SysBusDevice* i2c = NULL;
    AppleDTProp*  prop;
    uint64_t*     reg;
    AppleDTNode*  child = apple_dt_get_node(t8030->device_tree, "arm-io");

    child = apple_dt_get_node(child, name);
    if (child == NULL) {
        warn_report("Failed to create %s", name);
        return;
    }

    i2c = apple_i2c_create(name);
    assert_nonnull(i2c);
    object_property_add_child(OBJECT(t8030), name, OBJECT(i2c));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    sysbus_mmio_map(i2c, 0, t8030->armio_base + reg[0]);
    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    sysbus_connect_irq(i2c, 0, qdev_get_gpio_in(DEVICE(t8030->aic), *(uint32_t*)prop->data));

    sysbus_realize_and_unref(i2c, &error_fatal);
}

static void t8030_create_spi0(AppleT8030MachineState* t8030)
{
    DeviceState* spi  = NULL;
    DeviceState* gpio = NULL;
    Object*      sio;

    spi = qdev_new(TYPE_APPLE_SPI);
    assert_nonnull(spi);
    spi->id = g_strdup("spi0");

    object_property_add_child(OBJECT(t8030), "spi0", OBJECT(spi));

    sio = object_property_get_link(OBJECT(t8030), "sio", &error_fatal);
    object_property_add_const_link(OBJECT(spi), "sio", sio);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, t8030->armio_base + SPI0_BASE);

    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, qdev_get_gpio_in(DEVICE(t8030->aic), SPI0_IRQ));
    // The second sysbus IRQ is the cs line
    gpio = DEVICE(object_property_get_link(OBJECT(t8030), "gpio", &error_fatal));
    assert_nonnull(gpio);
    qdev_connect_gpio_out(gpio, GPIO_SPI0_CS, qdev_get_gpio_in_named(spi, SSI_GPIO_CS, 0));
}

static void t8030_create_spi(AppleT8030MachineState* t8030, uint32_t port)
{
    SysBusDevice* spi  = NULL;
    DeviceState*  gpio = NULL;
    AppleDTProp*  prop;
    uint64_t*     reg;
    uint32_t*     ints;
    AppleDTNode*  child = apple_dt_get_node(t8030->device_tree, "arm-io");
    Object*       sio;
    char          name[32];
    hwaddr        base;
    uint32_t      irq;
    uint32_t      cs_pin;

    snprintf(name, sizeof(name), "spi%u", port);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    spi = apple_spi_from_node(child);
    assert_nonnull(spi);
    object_property_add_child(OBJECT(t8030), name, OBJECT(spi));

    sio = object_property_get_link(OBJECT(t8030), "sio", &error_fatal);
    object_property_add_const_link(OBJECT(spi), "sio", sio);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg  = (uint64_t*)prop->data;
    base = t8030->armio_base + reg[0];
    sysbus_mmio_map(spi, 0, base);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;
    irq  = ints[0];

    // The second sysbus IRQ is the cs line
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, qdev_get_gpio_in(DEVICE(t8030->aic), irq));

    prop = apple_dt_get_prop(child, "function-spi_cs0");
    assert_nonnull(prop);
    ints   = (uint32_t*)prop->data;
    cs_pin = ints[2];
    gpio   = DEVICE(object_property_get_link(OBJECT(t8030), "gpio", &error_fatal));
    assert_nonnull(gpio);
    qdev_connect_gpio_out(gpio, cs_pin, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
}

static void t8030_create_usb(AppleT8030MachineState* t8030)
{
    AppleDTNode*       child            = apple_dt_get_node(t8030->device_tree, "arm-io");
    AppleDTNode*       drd              = apple_dt_get_node(child, "usb-drd");
    AppleDTNode*       dart_usb         = apple_dt_get_node(child, "dart-usb");
    AppleDTNode*       dart_mapper      = apple_dt_get_node(dart_usb, "mapper-usb-drd");
    AppleDTNode*       dart_dwc2_mapper = apple_dt_get_node(dart_usb, "mapper-usb-device");
    AppleDTNode*       phy              = apple_dt_get_node(child, "atc-phy");
    AppleDTProp*       prop;
    DeviceState*       atc;
    AppleDARTState*    dart;
    IOMMUMemoryRegion* iommu = NULL;
    uint32_t*          ints;

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-usb", &error_fatal));

    atc = qdev_new(TYPE_APPLE_TYPEC);
    object_property_add_child(OBJECT(t8030), "atc", OBJECT(atc));

    if (t8030->usb_conn_addr != NULL) {
        object_property_set_str(OBJECT(atc), "addr", t8030->usb_conn_addr, &error_fatal);
    }

    prop = apple_dt_get_prop(dart_mapper, "reg");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 4);
    iommu = apple_dart_instance_iommu_mr(dart, 1, *(uint32_t*)prop->data);
    assert_nonnull(iommu);

    object_property_add_const_link(OBJECT(atc), "dma-xhci", OBJECT(iommu));
    object_property_add_const_link(OBJECT(atc), "dma-drd", OBJECT(iommu));

    prop = apple_dt_get_prop(dart_dwc2_mapper, "reg");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 4);
    iommu = apple_dart_instance_iommu_mr(dart, 1, *(uint32_t*)prop->data);
    assert_nonnull(iommu);

    object_property_add_const_link(OBJECT(atc), "dma-otg", OBJECT(iommu));

    prop = apple_dt_get_prop(phy, "reg");
    assert_nonnull(prop);
    sysbus_mmio_map(SYS_BUS_DEVICE(atc), 0, t8030->armio_base + ((uint64_t*)prop->data)[0]);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(atc), &error_fatal);

    prop = apple_dt_get_prop(drd, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;
    for (int i = 0; i < 4; ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(atc), i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(atc), 4, qdev_get_gpio_in(DEVICE(t8030->aic), DWC2_IRQ));
}

static void t8030_create_wdt(AppleT8030MachineState* t8030)
{
    uint32_t      i;
    uint32_t*     ints;
    AppleDTProp*  prop;
    uint64_t*     reg;
    SysBusDevice* wdt;
    AppleDTNode*  child = apple_dt_get_node(t8030->device_tree, "arm-io");

    assert_nonnull(child);
    child = apple_dt_get_node(child, "wdt");
    assert_nonnull(child);

    wdt = apple_wdt_from_node(child);
    assert_nonnull(wdt);

    object_property_add_child(OBJECT(t8030), "wdt", OBJECT(wdt));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(wdt, 0, t8030->armio_base + reg[0]);
    sysbus_mmio_map(wdt, 1, t8030->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(wdt, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    // TODO: MCC
    apple_dt_del_prop_named(child, "function-panic_flush_helper");
    apple_dt_del_prop_named(child, "function-panic_halt_helper");

    apple_dt_set_prop_u32(child, "no-pmu", 1);

    sysbus_realize_and_unref(wdt, &error_fatal);
}

static void t8030_create_aes(AppleT8030MachineState* t8030)
{
    uint32_t*          ints;
    AppleDTProp*       prop;
    uint64_t*          reg;
    SysBusDevice*      aes;
    AppleDARTState*    dart;
    AppleDTNode*       child           = apple_dt_get_node(t8030->device_tree, "arm-io");
    IOMMUMemoryRegion* dma_mr          = NULL;
    AppleDTNode*       dart_sio        = apple_dt_get_node(child, "dart-sio");
    AppleDTNode*       dart_aes_mapper = apple_dt_get_node(dart_sio, "mapper-aes");

    assert_nonnull(child);
    child = apple_dt_get_node(child, "aes");
    assert_nonnull(child);
    assert_nonnull(dart_sio);
    assert_nonnull(dart_aes_mapper);

    aes = apple_aes_create(child, t8030->board_id);
    assert_nonnull(aes);

    object_property_add_child(OBJECT(t8030), "aes", OBJECT(aes));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(aes, 0, t8030->armio_base + reg[0]);
    sysbus_mmio_map(aes, 1, t8030->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 4);
    ints = (uint32_t*)prop->data;

    sysbus_connect_irq(aes, 0, qdev_get_gpio_in(DEVICE(t8030->aic), *ints));

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-sio", &error_fatal));
    assert_nonnull(dart);

    prop = apple_dt_get_prop(dart_aes_mapper, "reg");

    dma_mr = apple_dart_iommu_mr(dart, *(uint32_t*)prop->data);
    assert_nonnull(dma_mr);
    object_property_add_const_link(OBJECT(aes), "dma-mr", OBJECT(dma_mr));

    sysbus_realize_and_unref(aes, &error_fatal);
}

static void t8030_create_spmi(AppleT8030MachineState* t8030, const char* name)
{
    SysBusDevice* spmi = NULL;
    AppleDTProp*  prop;
    uint64_t*     reg;
    uint32_t*     ints;
    AppleDTNode*  child = apple_dt_get_node(t8030->device_tree, "arm-io");

    assert_nonnull(child);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    spmi = apple_spmi_from_node(child);
    assert_nonnull(spmi);
    object_property_add_child(OBJECT(t8030), name, OBJECT(spmi));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(SYS_BUS_DEVICE(spmi), 0, (t8030->armio_base + reg[2]) & ~(APPLE_SPMI_MMIO_SIZE - 1));

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;
    // XXX: Only the second interrupt's parent is AIC
    sysbus_connect_irq(SYS_BUS_DEVICE(spmi), 0, qdev_get_gpio_in(DEVICE(t8030->aic), ints[1]));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(spmi), &error_fatal);
}

static void t8030_create_pmu(AppleT8030MachineState* t8030, const char* parent, const char* name)
{
    DeviceState*    pmu  = NULL;
    AppleSPMIState* spmi = NULL;
    AppleDTProp*    prop;
    AppleDTNode*    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    uint32_t*       ints;

    assert_nonnull(child);
    child = apple_dt_get_node(child, parent);
    assert_nonnull(child);

    spmi = APPLE_SPMI(object_property_get_link(OBJECT(t8030), parent, &error_fatal));
    assert_nonnull(spmi);

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    pmu = apple_spmi_pmu_from_node(child);
    assert_nonnull(pmu);
    object_property_add_child(OBJECT(t8030), name, OBJECT(pmu));

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    qdev_connect_gpio_out(pmu, 0, qdev_get_gpio_in(DEVICE(spmi), ints[0]));
    spmi_slave_realize_and_unref(SPMI_SLAVE(pmu), spmi->bus, &error_fatal);

    qemu_register_wakeup_support();
}

static void t8030_create_baseband_spmi(AppleT8030MachineState* t8030, const char* parent, const char* name)
{
    DeviceState*    baseband = NULL;
    AppleSPMIState* spmi     = NULL;
    AppleDTProp*    prop;
    AppleDTNode*    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    uint32_t*       ints;

    assert_nonnull(child);
    child = apple_dt_get_node(child, parent);
    assert_nonnull(child);

    spmi = APPLE_SPMI(object_property_get_link(OBJECT(t8030), parent, &error_fatal));
    assert_nonnull(spmi);

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    baseband = apple_spmi_baseband_create(child);
    assert_nonnull(baseband);
    object_property_add_child(OBJECT(t8030), name, OBJECT(baseband));

#if 1
    child = apple_dt_get_node(t8030->device_tree, "baseband");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    qdev_connect_gpio_out(baseband, 0, qdev_get_gpio_in(DEVICE(spmi), ints[0]));
#endif
    spmi_slave_realize_and_unref(SPMI_SLAVE(baseband), spmi->bus, &error_fatal);

    qemu_register_wakeup_support();
}

static void t8030_create_smc(AppleT8030MachineState* t8030)
{
    uint32_t      i;
    uint32_t*     ints;
    AppleDTProp*  prop;
    uint64_t*     reg;
    SysBusDevice* smc;
    AppleDTNode*  child = apple_dt_get_node(t8030->device_tree, "arm-io");
    AppleDTNode*  iop_nub;

    assert_nonnull(child);
    child = apple_dt_get_node(child, "smc");
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, "iop-smc-nub");
    assert_nonnull(iop_nub);

    smc = apple_smc_create(child, APPLE_A7IOP_V4, apple_dt_get_prop_u64(iop_nub, "region-size", &error_fatal));

    object_property_add_child(OBJECT(t8030), "smc", OBJECT(smc));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    for (i = 0; i < prop->len / 16; ++i) { sysbus_mmio_map(smc, i, t8030->armio_base + reg[i * 2]); }

    sysbus_mmio_map(smc, i, apple_dt_get_prop_u64(iop_nub, "region-base", &error_fatal));

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(smc, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    sysbus_realize_and_unref(smc, &error_fatal);
}

static void t8030_create_sio(AppleT8030MachineState* t8030)
{
    uint32_t           i;
    uint32_t*          ints;
    AppleDTProp*       prop;
    uint64_t*          reg;
    SysBusDevice*      sio;
    AppleDARTState*    dart;
    AppleDTNode*       child = apple_dt_get_node(t8030->device_tree, "arm-io");
    AppleDTNode*       iop_nub;
    IOMMUMemoryRegion* dma_mr          = NULL;
    AppleDTNode*       dart_sio        = apple_dt_get_node(child, "dart-sio");
    AppleDTNode*       dart_sio_mapper = apple_dt_get_node(dart_sio, "mapper-sio");

    assert_nonnull(child);
    child = apple_dt_get_node(child, "sio");
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, "iop-sio-nub");
    assert_nonnull(iop_nub);

    // a13.c
    sio = apple_sio_from_node(child, APPLE_A7IOP_V4, t8030->sio_protocol, 24000000);
    object_property_add_child(OBJECT(t8030), "sio", OBJECT(sio));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    for (i = 0; i < 2; ++i) { sysbus_mmio_map(sio, i, t8030->armio_base + reg[i * 2]); }

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(sio, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-sio", &error_fatal));
    assert_nonnull(dart);

    prop = apple_dt_get_prop(dart_sio_mapper, "reg");

    dma_mr = apple_dart_iommu_mr(dart, *(uint32_t*)prop->data);
    assert_nonnull(dma_mr);
    object_property_add_const_link(OBJECT(sio), "dma-mr", OBJECT(dma_mr));

    sysbus_realize_and_unref(sio, &error_fatal);
}

static void t8030_create_roswell(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c3/roswell");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c3", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_ROSWELL, *(uint8_t*)prop->data);
}

static void t8030_create_display_pmu(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c2/display-pmu");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c2", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_CHESTNUT, *(uint8_t*)prop->data);
}

static void t8030_create_pcie(AppleT8030MachineState* t8030)
{
    uint32_t     i;
    uint32_t*    ints;
    AppleDTProp* prop;
    // uint64_t *reg;
    SysBusDevice* pcie;
    // char temp_name[32];
    // uint32_t port_index, port_entries;

    prop = apple_dt_get_prop(apple_dt_get_node(t8030->device_tree, "chosen"), "chip-id");
    assert_nonnull(prop);
    uint32_t chip_id = *(uint32_t*)prop->data;

    AppleDTNode* child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, "apcie");
    assert_nonnull(child);

    // TODO: S8000 needs it, and T8030 probably does need it as well.
    apple_dt_set_prop_null(child, "apcie-phy-tunables");
    // do not use no-phy-power-gating for T8030
    //// apple_dt_set_prop_null(child, "no-phy-power-gating");

    pcie = apple_pcie_from_node(child, chip_id);
    assert_nonnull(pcie);
    object_property_add_child(OBJECT(t8030), "apcie", OBJECT(pcie));

#if 0
    prop = apple_dt_find_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
#endif

#if 0
    #define PORT_COUNT    4
    #define ROOT_MAPPINGS 3
    #define PORT_MAPPINGS 3
    #define PORT_ENTRIES  4

    // TODO: Hook up all ports
    for (i = 0; i < ROOT_MAPPINGS; ++i) {
        ////sysbus_mmio_map(pcie, (PORT_COUNT * PORT_MAPPINGS) + i, reg[i * 2]);
        sysbus_mmio_map(pcie, i, reg[i * 2]);
    }
    #if 0
    for (i = 0; i < PORT_COUNT; ++i) {
        snprintf(temp_name, sizeof(temp_name), "port%u_config", i);
        create_unimplemented_device(temp_name, reg[(6 + (i * PORT_ENTRIES) + 0) * 2 + 0], reg[(6 + (i * PORT_ENTRIES) + 0) * 2 + 1]);
        snprintf(temp_name, sizeof(temp_name), "port%u_config_ltssm_debug", i);
        create_unimplemented_device(temp_name, reg[(6 + (i * PORT_ENTRIES) + 1) * 2 + 0], reg[(6 + (i * PORT_ENTRIES) + 1) * 2 + 1]);
        snprintf(temp_name, sizeof(temp_name), "port%u_phy_glue", i);
        create_unimplemented_device(temp_name, reg[(6 + (i * PORT_ENTRIES) + 2) * 2 + 0], reg[(6 + (i * PORT_ENTRIES) + 2) * 2 + 1]);
        snprintf(temp_name, sizeof(temp_name), "port%u_phy_ip", i);
        create_unimplemented_device(temp_name, reg[(6 + (i * PORT_ENTRIES) + 3) * 2 + 0], reg[(6 + (i * PORT_ENTRIES) + 3) * 2 + 1]);
    }
    #endif
    #if 1
    port_index = 6;
    port_entries = 4;
    // this has to come later, as root and port phy's will overlap otherwise
    for (i = 0; i < PORT_COUNT; ++i) {
        sysbus_mmio_map(pcie, ROOT_MAPPINGS + (i * PORT_MAPPINGS) + 0, reg[(port_index + (i * port_entries) + 0) * 2 + 0]);
        sysbus_mmio_map(pcie, ROOT_MAPPINGS + (i * PORT_MAPPINGS) + 1, reg[(port_index + (i * port_entries) + 2) * 2 + 0]);
        sysbus_mmio_map(pcie, ROOT_MAPPINGS + (i * PORT_MAPPINGS) + 2, reg[(port_index + (i * port_entries) + 3) * 2 + 0]);
    }
    #endif
#endif

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints                 = (uint32_t*)prop->data;
    int interrupts_count = prop->len / sizeof(uint32_t);

    for (i = 0; i < interrupts_count; ++i) {
        sysbus_connect_irq(pcie, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }
    uint32_t msi_vector_offset = apple_dt_get_prop_u32(child, "msi-vector-offset", &error_fatal);
    uint32_t msi_vectors       = apple_dt_get_prop_u32(child, "#msi-vectors", &error_fatal);
    for (i = 0; i < msi_vectors; ++i) {
        sysbus_connect_irq(pcie, interrupts_count + i, qdev_get_gpio_in(DEVICE(t8030->aic), msi_vector_offset + i));
    }

    sysbus_realize_and_unref(pcie, &error_fatal);
}

static void t8030_create_lm_backlight(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c0/lm3539");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c0", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, *(uint8_t*)prop->data);

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c2/lm3539-1");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c2", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, *(uint8_t*)prop->data);
}

static void t8030_create_accbuck(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/smc-i2c1/accbuck");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "smc-i2c1", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_FAN53740, *(uint8_t*)prop->data);
}

static void t8030_create_misc(AppleT8030MachineState* t8030)
{
    AppleDTNode* armio;
    AppleDTNode* chosen;
    AppleDTNode* child;

    armio = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(armio);
    chosen = apple_dt_get_node(t8030->device_tree, "chosen");
    assert_nonnull(chosen);

    child = apple_dt_get_node(armio, "bluetooth");
    assert_nonnull(child);

    apple_dt_set_prop(child, "local-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x42});

    apple_dt_set_prop(chosen, "mac-address-bluetooth0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x42});

    child = apple_dt_get_node(armio, "usb-drd");
    assert_nonnull(child);

    apple_dt_set_prop(child, "device-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x22});
    apple_dt_set_prop(child, "host-mac-address", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x22, 0x12});

    apple_dt_set_prop(chosen, "mac-address-ethernet0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x22});
    apple_dt_set_prop(chosen, "mac-address-wifi0", 6, (const uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF, 0x22, 0x12});

    // child = apple_dt_get_node(armio, "wlan");
    // assert_nonnull(child);
}

static void t8030_create_display(AppleT8030MachineState* t8030)
{
    MachineState* machine;
    SysBusDevice* sbd;
    AppleDTNode*  child;
    uint64_t*     reg;
    AppleDTProp*  prop;

    machine = MACHINE(t8030);

    AppleDARTState* dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-disp0", &error_fatal));
    assert_nonnull(dart);
    child = apple_dt_get_node(t8030->device_tree, "arm-io/dart-disp0/mapper-disp0");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    child = apple_dt_get_node(t8030->device_tree, "arm-io/disp0");

    sbd = adp_v4_from_node(child, MEMORY_REGION(apple_dart_iommu_mr(dart, ldl_le_p(prop->data))));

    qdev_prop_set_uint32(DEVICE(sbd), "width", t8030->disp_width);
    qdev_prop_set_uint32(DEVICE(sbd), "height", t8030->disp_height);

    t8030->video_args.row_bytes    = t8030->disp_width * sizeof(uint32_t);
    t8030->video_args.width        = t8030->disp_width;
    t8030->video_args.height       = t8030->disp_height;
    t8030->video_args.depth.depth  = sizeof(uint32_t) * 8;
    t8030->video_args.depth.rotate = 1;

    t8030->video_args.display = !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-s", false)
                                && !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-v", false);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sbd, 0, t8030->armio_base + reg[0]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    uint32_t* ints = (uint32_t*)prop->data;

    for (uint32_t i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    object_property_add_child(OBJECT(t8030), "disp0", OBJECT(sbd));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void t8030_create_scaler(AppleT8030MachineState* t8030)
{
    SysBusDevice* sbd;
    AppleDTNode*  child;
    uint64_t*     reg;
    AppleDTProp*  prop;

    AppleDARTState* dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-scaler", &error_fatal));
    g_assert_nonnull(dart);
    child = apple_dt_get_node(t8030->device_tree, "arm-io/dart-scaler/mapper-scaler");
    g_assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    g_assert_nonnull(prop);

    child = apple_dt_get_node(t8030->device_tree, "arm-io/scaler0");

    sbd = apple_scaler_create(child, MEMORY_REGION(apple_dart_iommu_mr(dart, ldl_le_p(prop->data))));

    prop = apple_dt_get_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sbd, 0, t8030->armio_base + reg[0]);
    sysbus_mmio_map(sbd, 1, t8030->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    g_assert_nonnull(prop);
    uint32_t* ints = (uint32_t*)prop->data;

    for (size_t i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    object_property_add_child(OBJECT(t8030), "scaler0", OBJECT(sbd));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void t8030_create_sep(AppleT8030MachineState* t8030)
{
    AppleDTNode*    armio;
    AppleDTNode*    child;
    AppleSEPState*  sep;
    AppleDTProp*    prop;
    uint32_t*       ints;
    AppleDARTState* dart;

    prop = apple_dt_get_prop(apple_dt_get_node(t8030->device_tree, "chosen"), "chip-id");
    assert_nonnull(prop);
    uint32_t chip_id = *(uint32_t*)prop->data;

    armio = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(armio);

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-sep", &error_fatal));

    child = apple_dt_get_node(armio, "dart-sep/mapper-sep");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    child = apple_dt_get_node(armio, "sep");
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "low-power-enable", 1);

    sep = apple_sep_from_node(child, MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t*)prop->data)), SEPROM_BASE,
                              A13_MAX_CPU, true, chip_id);
    assert_nonnull(sep);

    object_property_add_child(OBJECT(t8030), "sep", OBJECT(sep));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 0, t8030->armio_base + ldq_le_p(prop->data));
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PMGR, t8030->armio_base + 0x41000000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_TRNG_REGS, t8030->armio_base + 0x41180000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_KEY, t8030->armio_base + 0x411C0000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_KEY_FCFG, t8030->armio_base + 0x41440000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MONI, t8030->armio_base + 0x413C0000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MONI_THRM, t8030->armio_base + 0x41400000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_EISP, t8030->armio_base + 0x40800000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_EISP_HMAC, t8030->armio_base + 0x40AA0000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_AESS, t8030->armio_base + 0x41040000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_AESH, t8030->armio_base + 0x41080000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PKA, t8030->armio_base + 0x41100000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PKA_TMM, t8030->armio_base + 0x41504000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC0, t8030->armio_base + 0x42140000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC1, t8030->armio_base + 0x41240000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_MISC2, t8030->armio_base + 0x410C4000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_PROGRESS, t8030->armio_base + 0x41280000);
    apple_sep_map_mmio(sep, SEP_MMIO_INDEX_BOOT_MONITOR, t8030->armio_base + 0x41500000);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (uint32_t i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(sep), i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(sep), &error_fatal);
}

static void t8030_create_sep_sim(AppleT8030MachineState* t8030)
{
    AppleDTNode*      armio;
    AppleDTNode*      child;
    AppleSEPSimState* sep;
    AppleDTProp*      prop;
    uint64_t*         reg;
    uint32_t*         ints;
    AppleDARTState*   dart;

    armio = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(armio);
    child = apple_dt_get_node(armio, "sep");
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "low-power-enable", 1);

    sep = apple_sep_sim_from_node(child, true);
    assert_nonnull(sep);

    object_property_add_child(OBJECT(t8030), "sep-sim", OBJECT(sep));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 0, t8030->armio_base + reg[0]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (uint32_t i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(SYS_BUS_DEVICE(sep), i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-sep", &error_fatal));
    assert_nonnull(dart);
    child = apple_dt_get_node(armio, "dart-sep");
    assert_nonnull(child);
    child = apple_dt_get_node(child, "mapper-sep");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    sep->dma_mr = MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t*)prop->data));
    assert_nonnull(sep->dma_mr);
    object_property_add_const_link(OBJECT(sep), "dma-mr", OBJECT(sep->dma_mr));
    sep->dma_as = g_new0(AddressSpace, 1);
    address_space_init(sep->dma_as, sep->dma_mr, "sep.dma");

    sysbus_realize_and_unref(SYS_BUS_DEVICE(sep), &error_fatal);
}

static void t8030_create_mt_spi(AppleT8030MachineState* t8030)
{
    AppleSPIState* spi;
    DeviceState*   device;
    DeviceState*   gpio;
    AppleDTNode*   child;
    AppleDTProp*   prop;
    uint32_t*      ints;

    spi = APPLE_SPI(object_property_get_link(OBJECT(t8030), "spi1", &error_fatal));
    // device = ssi_create_peripheral(apple_spi_get_bus(spi),
    // TYPE_APPLE_MT_SPI);
    device = qdev_new(TYPE_APPLE_MT_SPI);
    qdev_prop_set_uint32(device, "display_width", t8030->disp_width);
    qdev_prop_set_uint32(device, "display_height", t8030->disp_height);
    ssi_realize_and_unref(device, apple_spi_get_bus(spi), &error_fatal);

    gpio = DEVICE(object_property_get_link(OBJECT(t8030), "aop-gpio", &error_fatal));

    child = apple_dt_get_node(t8030->device_tree, "arm-io/spi1/multi-touch");
    assert_nonnull(child);

    apple_dt_set_prop_null(child, "function-power_ana");
    // is deleting auth-required really necessary for iOS 18.5?
    apple_dt_del_prop_named(child, "auth-required");
    apple_dt_set_prop_null(child, "force-supported");
    apple_dt_set_prop_null(child, "maintain-power");

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    qdev_connect_gpio_out_named(device, APPLE_MT_SPI_IRQ, 0, qdev_get_gpio_in(gpio, ints[0]));
}

static void t8030_create_aop(AppleT8030MachineState* t8030)
{
    uint32_t           i;
    uint32_t*          ints;
    AppleDTProp*       prop;
    uint64_t*          reg;
    SysBusDevice*      aop;
    AppleDARTState*    dart;
    AppleDTNode*       child;
    AppleDTNode*       iop_nub;
    IOMMUMemoryRegion* dma_mr;
    AppleDTNode*       dart_aop;
    AppleDTNode*       dart_aop_mapper;
    SysBusDevice*      sbd;

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    dart_aop        = apple_dt_get_node(child, "dart-aop");
    dart_aop_mapper = apple_dt_get_node(dart_aop, "mapper-aop");
    child           = apple_dt_get_node(child, "aop");
    assert_nonnull(child);
    iop_nub = apple_dt_get_node(child, "iop-aop-nub");
    assert_nonnull(iop_nub);

    aop = apple_aop_create(child, APPLE_A7IOP_V4);
    assert_nonnull(aop);

    object_property_add_child(OBJECT(t8030), "aop", OBJECT(aop));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    for (i = 0; i < 2; ++i) { sysbus_mmio_map(aop, i, t8030->armio_base + reg[i * 2]); }

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); ++i) {
        sysbus_connect_irq(aop, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030), "dart-aop", &error_fatal));
    assert_nonnull(dart);

    prop = apple_dt_get_prop(dart_aop_mapper, "reg");

    dma_mr = apple_dart_iommu_mr(dart, *(uint32_t*)prop->data);
    assert_nonnull(dma_mr);
    object_property_add_const_link(OBJECT(aop), "dma-mr", OBJECT(dma_mr));

    // sbd = apple_aop_audio_create(APPLE_AOP(aop));
    // assert_nonnull(sbd);
    // object_property_add_child(OBJECT(aop), "aop-audio", OBJECT(sbd));
    // sysbus_realize_and_unref(sbd, &error_fatal);

    sysbus_realize_and_unref(aop, &error_fatal);
}

static void t8030_create_mca(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleSIOState* sio;
    // DTBNode *mca5;
    SysBusDevice* sbd;
    AppleDTProp*  prop;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/mca-switch");
    assert_nonnull(child);

    sio = APPLE_SIO(object_property_get_link(OBJECT(t8030), "sio", &error_fatal));
    sbd = apple_mca_create(child, apple_sio_get_endpoint(sio, 0x7A), apple_sio_get_endpoint(sio, 0x7B));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_SIO, t8030->armio_base + ldq_le_p(prop->data));
    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_DMA, t8030->armio_base + ldq_le_p(prop->data + sizeof(uint64_t) * 2));
    sysbus_mmio_map(sbd, APPLE_MCA_MMIO_MCLK_CFG, t8030->armio_base + ldq_le_p(prop->data + sizeof(uint64_t) * 4));

    object_property_add_child(OBJECT(t8030), "mca", OBJECT(sbd));
    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void t8030_create_speaker_top(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c2/audio-speaker-top");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c2", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_CS35L27, *(uint8_t*)prop->data);
}

static void t8030_create_speaker_bottom(AppleT8030MachineState* t8030)
{
    AppleSPIState* spi;

    spi = APPLE_SPI(object_property_get_link(OBJECT(t8030), "spi3", &error_fatal));
    ssi_create_peripheral(apple_spi_get_bus(spi), TYPE_APPLE_CS42L77);
}

static void t8030_create_buttons(AppleT8030MachineState* t8030)
{
    SysBusDevice* buttons;
    AppleDTNode*  child;

    child = apple_dt_get_node(t8030->device_tree, "buttons");
    assert_nonnull(child);

    buttons = apple_buttons_create(child);
    object_property_add_child(OBJECT(t8030), "buttons", OBJECT(buttons));
    sysbus_realize_and_unref(buttons, &error_fatal);
}

static void t8030_create_mipi_dsim(AppleT8030MachineState* t8030)
{
    int           i;
    uint32_t*     ints;
    AppleDTProp*  prop;
    uint64_t*     reg;
    SysBusDevice* sbd;
    AppleDTNode*  child;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/mipi-dsim");
    assert_nonnull(child);

    sbd = synopsys_mipi_dsim_create(child);
    assert_nonnull(sbd);

    object_property_add_child(OBJECT(t8030), "mipi-dsim", OBJECT(sbd));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sbd, 0, t8030->armio_base + reg[0]);
    sysbus_mmio_map(sbd, 1, t8030->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(DEVICE(t8030->aic), ints[i]));
    }

    sysbus_realize_and_unref(sbd, &error_fatal);

    child = apple_dt_get_node(child, "lcd");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "lcd-panel-id", 0xA1C432D1);
}

static void t8030_create_tempsensor(AppleT8030MachineState* t8030, const char* name, bool conflicts_pmgr)
{
    AppleDTNode*  child;
    AppleDTProp*  prop;
    SysBusDevice* sbd;
    uint64_t*     reg;

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "invokes-sochot", 0);

    if (conflicts_pmgr) { return; }

    sbd = apple_temp_sensor_create(child);
    object_property_add_child(OBJECT(t8030), name, OBJECT(sbd));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sbd, 0, t8030->armio_base + reg[0]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(t8030->aic), ldl_le_p(prop->data)));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void t8030_create_mtr_tempsensor(AppleT8030MachineState* t8030, const char* name)
{
    AppleDTNode* child;
    AppleDTProp* prop;
    uint64_t*    reg;

    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "invokes-sochot", 0);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    create_unimplemented_device(name, t8030->armio_base + reg[0], reg[1]);
}

static void t8030_create_mic_temp_sensor2(AppleT8030MachineState* t8030)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(t8030->device_tree, "arm-io/i2c1/mic-temp-sens2");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    apple_dt_del_prop_named(child, "function-micbias-enable");

    i2c = APPLE_I2C(object_property_get_link(OBJECT(t8030), "i2c1", &error_fatal));
    // TODO: Revert product id to 3 and vendor id to 2 once audio support is
    // implemented.
    apple_mic_temp_sensor_create(*(uint8_t*)prop->data, i2c->bus, 1, 0, 0, 5, &error_fatal);
}

static void t8030_cpu_reset(AppleT8030MachineState* t8030)
{
    CPUState*      cpu;
    ARMCPU*        arm_cpu;
    AppleA13State* acpu;
    uint64_t       m_lo;
    uint64_t       m_hi;

    qemu_guest_getrandom(&m_lo, sizeof(m_lo), NULL);
    qemu_guest_getrandom(&m_hi, sizeof(m_hi), NULL);

    CPU_FOREACH (cpu) {
        arm_cpu = container_of(cpu, ARMCPU, parent_obj);
        acpu    = container_of(arm_cpu, AppleA13State, parent_obj);

        arm_cpu->m_key_lo = m_lo;
        arm_cpu->m_key_hi = m_hi;

        if (t8030->securerom_filename == NULL) {
            if (acpu->cpu_id != A13_MAX_CPU) {
                object_property_set_uint(OBJECT(cpu), "rvbar", t8030->boot_info.kern_entry & ~0xFFFULL, &error_abort);
                cpu_reset(cpu);
            }
            if (acpu->cpu_id == 0) {
                arm_set_cpu_on(arm_cpu->mp_affinity, t8030->boot_info.kern_entry, t8030->boot_info.kern_boot_args_addr,
                               1, true);
            }
        }
        else {
            if (acpu->cpu_id != A13_MAX_CPU) {
                object_property_set_uint(OBJECT(cpu), "rvbar", SROM_BASE, &error_abort);
                cpu_reset(cpu);
            }
            if (acpu->cpu_id == 0) { arm_set_cpu_on(arm_cpu->mp_affinity, SROM_BASE, 0, 1, true); }
        }
    }
}

static void t8030_reset(MachineState* machine, ResetType type)
{
    AppleT8030MachineState* t8030 = APPLE_T8030(machine);
    DeviceState*            gpio;

    memset(t8030->pmgr_reg, 0, sizeof(t8030->pmgr_reg));

    pmgr_unk_e4800 = 0;
    // maybe also reset pmgr_unk_e4000 array
    // Ah, what the heck. Let's do it.
    memset(pmgr_unk_e4000, 0, sizeof(pmgr_unk_e4000));

    qemu_devices_reset(type);

    if (!runstate_check(RUN_STATE_PRELAUNCH)) { t8030_memory_setup(t8030); }

    t8030_cpu_reset(t8030);

    gpio = DEVICE(object_property_get_link(OBJECT(t8030), "gpio", &error_fatal));
    qemu_set_irq(qdev_get_gpio_in(gpio, GPIO_FORCE_DFU), t8030->force_dfu);
}

static void t8030_init_done(Notifier* notifier, void* data)
{
    AppleT8030MachineState* t8030 = container_of(notifier, AppleT8030MachineState, init_done_notifier);
    t8030_memory_setup(t8030);
    t8030_cpu_reset(t8030);
}

static void t8030_init(MachineState* machine)
{
    AppleT8030MachineState* t8030;
    vaddr                   kc_end;
    uint32_t                build_version;
    AppleDTNode*            child;
    AppleDTProp*            prop;
    hwaddr*                 ranges;

    t8030 = APPLE_T8030(machine);

    if ((t8030->sep_fw_filename == NULL) != (t8030->sep_rom_filename == NULL)) {
        error_setg(&error_fatal, "You need to specify both the SEPROM and the decrypted "
                                 "SEPFW in order to use SEP emulation!");
        return;
    }

    if (t8030->sep_fw_filename == NULL) {
        if (machine->smp.cpus > A13_MAX_CPU) {
            error_setg(&error_fatal, "Too many CPU cores specified for simulated SEP!");
            return;
        }
    }
    else if (machine->smp.cpus < 2) {
        error_setg(&error_fatal, "Too few CPU cores specified for emulated SEP!");
        return;
    }

    allocate_ram(get_system_memory(), "SROM", SROM_BASE, SROM_SIZE, 0);
    allocate_ram(get_system_memory(), "SRAM", SRAM_BASE, SRAM_SIZE, 0);
    memory_region_add_subregion(get_system_memory(), DRAM_BASE, machine->ram);

    if (t8030->sep_rom_filename != NULL) {
        allocate_ram(get_system_memory(), "SEPROM", SEPROM_BASE, SEPROM_SIZE, 0);
        // SEP_UNKN0 is now MISC0
        allocate_ram(get_system_memory(), "SEP_UNKN1", 0x242200000ULL, 0x24000, 0);
        // for last_jump
        allocate_ram(get_system_memory(), "SEP_UNKN10", 0x242150000ULL, 0x4000, 0);
        allocate_ram(get_system_memory(), "SEP_UNKN11", 0x241010000ULL, 0x4000, 0);
        // SEP_UNKN12 is now MISC1
        // 0x242400000 is apple-a7iop.SEP.regs
        // sepfw 26.2beta2/26.3.1 says it's (a7iop) size would be 0x64000
        // stack for 0x340005BF4/SEPFW
        allocate_ram(get_system_memory(), "SEP_UNKN13", 0x24020C000ULL, 0x4000, 0);
        // for SEP Panic: [elfour panic] [/,&&&&+&] exception.c:'&.
        allocate_ram(get_system_memory(), "SEP_UNKN14", 0x240A80000ULL, 0x4000, 0);
        allocate_ram(get_system_memory(), "SEP_UNKN15", 0x240A90000ULL, 0x4000, 0);
        // for sepfw 26.2beta2/26.3.1
        allocate_ram(get_system_memory(), "SEP_UNKN16", 0x242000000ULL, 0x400000, 0);
        allocate_ram(get_system_memory(), "SEP_dram_nubs", 0x23DF00000ULL, 0x20000, 0);
        allocate_ram(get_system_memory(), "SEP_dram_nubg", 0x23D000000ULL, 0x4000, 0);
        // dram_scr address 0x23D2C0000 size 0x8000 inside pmgr-unk-reg-2
        // dram_pln0 address 0x200000000 size 0x8000, inside amcc
    }

    t8030->device_tree = apple_boot_load_dt_file(machine->dtb);
    if (t8030->device_tree == NULL) {
        error_setg(&error_fatal, "Failed to load device tree");
        return;
    }

    t8030->kernel = apple_boot_load_kernel(machine->kernel_filename, NULL);
    if (t8030->kernel == NULL) {
        error_setg(&error_fatal, "Failed to load kernel");
        return;
    }

    build_version = apple_boot_build_version(t8030->kernel);
    info_report("%s %u.%u.%u", apple_boot_platform_string(t8030->kernel), BUILD_VERSION_MAJOR(build_version),
                BUILD_VERSION_MINOR(build_version), BUILD_VERSION_PATCH(build_version));
    t8030->build_version = build_version;

    switch (BUILD_VERSION_MAJOR(build_version)) {
        case 13:
        case 14:
        case 15:
        case 16: t8030->sio_protocol = 9; break;
        case 17:
        case 18:
        case 26: t8030->sio_protocol = 10; break;
        default: assert_not_reached();
    }

    if (t8030->securerom_filename == NULL) {
        apple_boot_get_kc_bounds(t8030->kernel, NULL, &g_virt_base, &kc_end, NULL, NULL);
        info_report("Kernel virtual low: 0x%016" VADDR_PRIx, g_virt_base);
        info_report("Kernel virtual high: 0x%016" VADDR_PRIx, kc_end);

        g_phys_base = (hwaddr)apple_boot_get_macho_buffer(t8030->kernel);

        t8030_patch_kernel(t8030->kernel, build_version, t8030->enable_pac);

        t8030->trustcache =
            apple_boot_load_trustcache_file(t8030->trustcache_filename, &t8030->boot_info.trustcache_size);

        if (t8030->ticket_filename != NULL) {
            if (!g_file_get_contents(t8030->ticket_filename, &t8030->boot_info.ticket_data,
                                     &t8030->boot_info.ticket_length, NULL))
            {
                error_setg(&error_fatal, "Failed to read ticket from `%s`", t8030->ticket_filename);
                return;
            }
        }
    }
    else {
        if (!g_file_get_contents(t8030->securerom_filename, &t8030->securerom, &t8030->securerom_size, NULL)) {
            error_setg(&error_fatal, "Failed to load SecureROM from `%s`", t8030->securerom_filename);
            return;
        }
    }

    apple_dt_set_prop_u32(t8030->device_tree, "clock-frequency", 24000000);
    child = apple_dt_get_node(t8030->device_tree, "arm-io");
    assert_nonnull(child);

    t8030->chip_revision = 0x20;
    apple_dt_set_prop_u32(child, "chip-revision", t8030->chip_revision);

    apple_dt_set_prop(child, "clock-frequencies", sizeof(t8030_clock_frequencies), t8030_clock_frequencies);
    apple_dt_set_prop(child, "clock-frequencies-nclk", sizeof(t8030_clock_frequencies_nclk),
                      t8030_clock_frequencies_nclk);

    prop = apple_dt_get_prop(child, "ranges");
    assert_nonnull(prop);

    ranges            = (hwaddr*)prop->data;
    t8030->armio_base = ranges[1];
    t8030->armio_size = ranges[2];

    apple_dt_set_prop_strn(t8030->device_tree, "platform-name", 32, "t8030");
    apple_dt_set_prop_strn(t8030->device_tree, "model-number", 32, t8030->model_number);
    apple_dt_set_prop_strn(t8030->device_tree, "region-info", 32, t8030->region_info);
    apple_dt_set_prop_strn(t8030->device_tree, "config-number", 64, t8030->config_number);
    apple_dt_set_prop_strn(t8030->device_tree, "serial-number", 32, t8030->serial_number);
    apple_dt_set_prop_strn(t8030->device_tree, "mlb-serial-number", 32, t8030->mlb_serial_number);
    apple_dt_set_prop_strn(t8030->device_tree, "regulatory-model-number", 32, t8030->regulatory_model);

    child = apple_dt_get_node(t8030->device_tree, "chosen");
    apple_dt_set_prop_u32(child, "chip-id", 0x8030);
    t8030->board_id = 0x4;
    apple_dt_set_prop_u32(child, "board-id", t8030->board_id);
    apple_dt_set_prop_u32(child, "certificate-production-status", 1);
    apple_dt_set_prop_u32(child, "certificate-security-mode", 1);
    apple_dt_set_prop_u32(child, "mix-n-match-prevention-status", 1);
    apple_dt_set_prop_u64(child, "unique-chip-id", t8030->ecid);

    // Update the display parameters
    apple_dt_set_prop_u32(child, "display-scale", 2);

    child = apple_dt_get_node(t8030->device_tree, "product");
    apple_dt_set_prop_u64(child, "display-corner-radius", 0x100000027);
    apple_dt_set_prop_u32(child, "oled-display", 1);
    apple_dt_set_prop_str(child, "graphics-featureset-class", "GLES2,0");
    apple_dt_set_prop_str(child, "graphics-featureset-fallbacks", "");
    apple_dt_set_prop_str(child, "artwork-display-gamut", "sRGB");
    // TODO: PMP
    apple_dt_set_prop_str(t8030->device_tree, "target-type", "n104sim");

    t8030_cpu_setup(t8030);
    t8030_create_aic(t8030);

    for (uint32_t i = 0; i < NUM_UARTS; ++i) { t8030_create_s3c_uart(t8030, i, serial_hd(i)); }

    t8030_pmgr_setup(t8030);
    t8030_amcc_setup(t8030);
    t8030_create_gpio(t8030, "gpio");
    t8030_create_gpio(t8030, "smc-gpio");
    t8030_create_gpio(t8030, "nub-gpio");
    t8030_create_gpio(t8030, "aop-gpio");
    t8030_create_i2c(t8030, "i2c0");
    t8030_create_i2c(t8030, "i2c1");
    t8030_create_i2c(t8030, "i2c2");
    t8030_create_i2c(t8030, "i2c3");
    t8030_create_i2c(t8030, "smc-i2c0");
    t8030_create_i2c(t8030, "smc-i2c1");
    t8030_create_dart(t8030, "dart-usb", false);
    t8030_create_dart(t8030, "dart-sio", false);
    t8030_create_dart(t8030, "dart-disp0", false);
    t8030_create_dart(t8030, "dart-sep", false);
    t8030_create_dart(t8030, "dart-apcie2", true);
    t8030_create_dart(t8030, "dart-apcie3", true);
    t8030_create_dart(t8030, "dart-aop", false);
    t8030_create_dart(t8030, "dart-rsm", false);
    t8030_create_dart(t8030, "dart-scaler", false);
    t8030_create_pcie(t8030);
    t8030_create_ans(t8030);
    t8030_create_usb(t8030);
    t8030_create_wdt(t8030);
    t8030_create_aes(t8030);
    t8030_create_spmi(t8030, "spmi0");
    t8030_create_spmi(t8030, "spmi1");
    t8030_create_spmi(t8030, "spmi2");
    t8030_create_pmu(t8030, "spmi0", "spmi-pmu");
    t8030_create_smc(t8030);
#ifdef ENABLE_BASEBAND
    t8030_create_baseband_spmi(t8030, "spmi1", "baseband-spmi");
    t8030_create_baseband(t8030);
#endif
    t8030_create_sio(t8030);
    t8030_create_spi0(t8030);
    t8030_create_spi(t8030, 1);
    t8030_create_spi(t8030, 3);

    if (t8030->sep_rom_filename && t8030->sep_fw_filename) { t8030_create_sep(t8030); }
    else {
#ifdef ENABLE_DATA_ENCRYPTION
        error_setg(&error_fatal, "Simulated SEP cannot be used with data "
                                 "encryption at the moment.");
        return;
#else
        t8030_create_sep_sim(t8030);
#endif
    }

    t8030_create_roswell(t8030);
    t8030_create_lm_backlight(t8030);
    t8030_create_display_pmu(t8030);
    t8030_create_display(t8030);
    t8030_create_mt_spi(t8030);
    t8030_create_aop(t8030);
    t8030_create_mca(t8030);
    t8030_create_speaker_top(t8030);
    t8030_create_speaker_bottom(t8030);
    t8030_create_buttons(t8030);
    t8030_create_mipi_dsim(t8030);
    t8030_create_scaler(t8030);
    t8030_create_accbuck(t8030);
    t8030_create_misc(t8030);

    t8030_create_tempsensor(t8030, "tempsensor0", false);
    t8030_create_tempsensor(t8030, "tempsensor1", false);
    t8030_create_tempsensor(t8030, "tempsensor2", false);
    t8030_create_tempsensor(t8030, "tempsensor3", false);
    t8030_create_tempsensor(t8030, "tempsensor4", false);
    t8030_create_tempsensor(t8030, "tempsensor5", false);
    t8030_create_tempsensor(t8030, "mtrtempsensor6", true);
    t8030_create_tempsensor(t8030, "mtrtempsensor7", true);
    t8030_create_tempsensor(t8030, "mtrtempsensor8", true);
    t8030_create_tempsensor(t8030, "mtrtempsensor11", false);
    t8030_create_tempsensor(t8030, "mtrtempsensor13", false);
    t8030_create_mtr_tempsensor(t8030, "mtrtempsensor14");
    t8030_create_mtr_tempsensor(t8030, "mtrtempsensor15");
    t8030_create_mic_temp_sensor2(t8030);

    t8030->init_done_notifier.notify = t8030_init_done;
    qemu_add_machine_init_done_notifier(&t8030->init_done_notifier);
}

static ram_addr_t t8030_fixup_ram_size(ram_addr_t size)
{
    ram_addr_t ret = ROUND_UP_16K(size);
    if (ret > 4 * GiB) {
        error_setg(&error_fatal, "Specified RAM size exceeds supported SoC maximum (4 GiB)");
        assert_not_reached();
    }
    return ret;
}

static void t8030_set_boot_mode(Object* obj, const char* value, Error** errp)
{
    AppleBootMode boot_mode;

    if (g_str_equal(value, "auto")) { boot_mode = kAppleBootModeAuto; }
    else if (g_str_equal(value, "enter_recovery")) {
        boot_mode = kAppleBootModeEnterRecovery;
    }
    else if (g_str_equal(value, "exit_recovery")) {
        boot_mode = kAppleBootModeExitRecovery;
    }
    else {
        error_setg(errp, "Invalid boot mode: %s", value);
        return;
    }

    APPLE_T8030(obj)->boot_info.boot_mode = boot_mode;
}

static char* t8030_get_boot_mode(Object* obj, Error** errp)
{
    switch (APPLE_T8030(obj)->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery: return g_strdup("enter_recovery");
        case kAppleBootModeExitRecovery : return g_strdup("exit_recovery");
        default                         : return g_strdup("auto");
    }
}

PROP_VISIT_GETTER_SETTER(uint64, ecid);
PROP_GETTER_SETTER(bool, kaslr_off);
PROP_GETTER_SETTER(bool, force_dfu);
PROP_STR_GETTER_SETTER(trustcache_filename);
PROP_STR_GETTER_SETTER(ticket_filename);
PROP_STR_GETTER_SETTER(sep_rom_filename);
PROP_STR_GETTER_SETTER(sep_fw_filename);
PROP_STR_GETTER_SETTER(securerom_filename);
PROP_STR_GETTER_SETTER(usb_conn_addr);
PROP_STR_GETTER_SETTER(model_number);
PROP_STR_GETTER_SETTER(region_info);
PROP_STR_GETTER_SETTER(config_number);
PROP_STR_GETTER_SETTER(serial_number);
PROP_STR_GETTER_SETTER(mlb_serial_number);
PROP_STR_GETTER_SETTER(regulatory_model);
PROP_VISIT_GETTER_SETTER(uint32, disp_width);
PROP_VISIT_GETTER_SETTER(uint32, disp_height);
PROP_GETTER_SETTER(bool, enable_pac);

static void t8030_class_init(ObjectClass* klass, const void* data)
{
    MachineClass*   mc = MACHINE_CLASS(klass);
    ObjectProperty* oprop;

    mc->desc              = "Apple T8030 SoC (iPhone 11)";
    mc->init              = t8030_init;
    mc->reset             = t8030_reset;
    mc->max_cpus          = A13_MAX_CPU + 1;
    mc->default_cpu_type  = TYPE_APPLE_A13;
    mc->minimum_page_bits = 14;
    mc->default_ram_size  = 4 * GiB;
    mc->fixup_ram_size    = t8030_fixup_ram_size;
    mc->default_ram_id    = "t8030.ram";

    object_class_property_add_str(klass, "trustcache", t8030_get_trustcache_filename, t8030_set_trustcache_filename);
    object_class_property_set_description(klass, "trustcache", "TrustCache");
    object_class_property_add_str(klass, "ticket", t8030_get_ticket_filename, t8030_set_ticket_filename);
    object_class_property_set_description(klass, "ticket", "AP Ticket");
    object_class_property_add_str(klass, "sep-rom", t8030_get_sep_rom_filename, t8030_set_sep_rom_filename);
    object_class_property_set_description(klass, "sep-rom", "SEP ROM");
    object_class_property_add_str(klass, "sep-fw", t8030_get_sep_fw_filename, t8030_set_sep_fw_filename);
    object_class_property_set_description(klass, "sep-fw", "SEP Firmware");
    object_class_property_add_str(klass, "securerom", t8030_get_securerom_filename, t8030_set_securerom_filename);
    object_class_property_set_description(klass, "securerom", "SecureROM");
    oprop = object_class_property_add_str(klass, "boot-mode", t8030_get_boot_mode, t8030_set_boot_mode);
    object_property_set_default_str(oprop, "auto");
    object_class_property_set_description(klass, "boot-mode", "Boot Mode");
    oprop = object_class_property_add(klass, "ecid", "uint64", t8030_get_ecid, t8030_set_ecid, NULL, NULL);
    object_property_set_default_uint(oprop, 0x1122334455667788);
    object_class_property_set_description(klass, "ecid", "Device ECID");
    object_class_property_add_bool(klass, "kaslr-off", t8030_get_kaslr_off, t8030_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    object_class_property_add_bool(klass, "force-dfu", t8030_get_force_dfu, t8030_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
    object_class_property_add_str(klass, "usb-conn-addr", t8030_get_usb_conn_addr, t8030_set_usb_conn_addr);
    object_class_property_set_description(klass, "usb-conn-addr", "USB Connection Address");
    oprop = object_class_property_add_str(klass, "model", t8030_get_model_number, t8030_set_model_number);
    object_property_set_default_str(oprop, "CKI12");
    object_class_property_set_description(klass, "model", "Model Number");
    oprop = object_class_property_add_str(klass, "region-info", t8030_get_region_info, t8030_set_region_info);
    object_property_set_default_str(oprop, "LL/A");
    object_class_property_set_description(klass, "region-info", "Region Info");
    oprop = object_class_property_add_str(klass, "config-number", t8030_get_config_number, t8030_set_config_number);
    object_property_set_default_str(oprop, "");
    object_class_property_set_description(klass, "config-number", "Config Number");
    oprop = object_class_property_add_str(klass, "serial-number", t8030_get_serial_number, t8030_set_serial_number);
    object_property_set_default_str(oprop, "INFERNO_1122");
    object_class_property_set_description(klass, "serial-number", "Serial Number");
    oprop = object_class_property_add_str(klass, "mlb", t8030_get_mlb_serial_number, t8030_set_mlb_serial_number);
    object_property_set_default_str(oprop, "INFERNO_MLB1122");
    object_class_property_set_description(klass, "mlb", "MLB Serial Number");
    oprop = object_class_property_add_str(klass, "regulatory-model", t8030_get_regulatory_model,
                                          t8030_set_regulatory_model);
    object_property_set_default_str(oprop, "INFERN8030");
    object_class_property_set_description(klass, "regulatory-model", "Regulatory Model Number");
    oprop = object_class_property_add(klass, "disp-width", "uint32", t8030_get_disp_width, t8030_set_disp_width, NULL,
                                      NULL);
    object_property_set_default_uint(oprop, 828);
    oprop = object_class_property_add(klass, "disp-height", "uint32", t8030_get_disp_height, t8030_set_disp_height,
                                      NULL, NULL);
    object_property_set_default_uint(oprop, 1792);
    object_class_property_add_bool(klass, "enable-pac", t8030_get_enable_pac, t8030_set_enable_pac);
    object_class_property_set_description(klass, "enable-pac",
                                          "Enable PAC with virtualisation (supports arm64e binaries only).");
}

static const TypeInfo t8030_info = {
    .name          = TYPE_APPLE_T8030,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(AppleT8030MachineState),
    .class_size    = sizeof(AppleT8030MachineClass),
    .class_init    = t8030_class_init,
};

static void t8030_types(void) { type_register_static(&t8030_info); }

type_init(t8030_types)
