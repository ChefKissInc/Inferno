/*
 * Apple S8000 SoC (iPhone 6s Plus).
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
#include "hw/arm/boot.h"
#include "hw/arm/dart.h"
#include "hw/arm/kernel_patches.h"
#include "hw/arm/lm-backlight.h"
#include "hw/arm/mem.h"
#include "hw/arm/s8000-config.c.inc"
#include "hw/arm/s8000.h"
#include "hw/arm/sep/sim.h"
#include "hw/char/exynos4210_uart.h"
#include "hw/block/nvme_mmu.h"
#include "hw/display/apple_displaypipe_v2.h"
// #include "hw/dma/apple_sio.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/misc/aes.h"
#include "hw/misc/chestnut.h"
#include "hw/misc/pmu-d2255.h"
#include "hw/nvram/apple_nvram.h"
#include "hw/pci-host/apcie.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/apple_otg.h"
#include "hw/usb/usb-uplink.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/arm-powerctl.h"

#define PROP_VISIT_GETTER_SETTER(_type, _name)                                                           \
    static void s8000_get_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    {                                                                                                    \
        _type##_t value;                                                                                 \
                                                                                                         \
        value = APPLE_S8000(obj)->_name;                                                                 \
        visit_type_##_type(v, name, &value, errp);                                                       \
    }                                                                                                    \
                                                                                                         \
    static void s8000_set_##_name(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) \
    { visit_type_##_type(v, name, &APPLE_S8000(obj)->_name, errp); }

#define PROP_STR_GETTER_SETTER(_name)                                                                       \
    static char* s8000_get_##_name(Object* obj, Error** errp) { return g_strdup(APPLE_S8000(obj)->_name); } \
                                                                                                            \
    static void s8000_set_##_name(Object* obj, const char* value, Error** errp)                             \
    {                                                                                                       \
        AppleS8000MachineState* s8000;                                                                      \
                                                                                                            \
        s8000 = APPLE_S8000(obj);                                                                           \
        g_free(s8000->_name);                                                                               \
        s8000->_name = g_strdup(value);                                                                     \
    }

#define PROP_GETTER_SETTER(_type, _name)                                                                       \
    static void s8000_set_##_name(Object* obj, _type value, Error** errp) { APPLE_S8000(obj)->_name = value; } \
                                                                                                               \
    static _type s8000_get_##_name(Object* obj, Error** errp) { return APPLE_S8000(obj)->_name; }

#define SPI0_IRQ       188
#define GPIO_SPI0_CS   106
#define GPIO_FORCE_DFU 123

#define SPI0_BASE (0xA080000ULL)

#define SROM_BASE (0x100000000)
#define SROM_SIZE (512 * KiB)

#define DRAM_BASE (0x800000000ULL)
#define DRAM_SIZE (2 * GiB)

#define SRAM_BASE (0x180000000ULL)
#define SRAM_SIZE (0x400000ULL)

#define SEPROM_BASE (0x20D000000ULL)
#define SEPROM_SIZE (0x1000000ULL)

// Carveout region 0x2 ; this is the first region
#define NVME_SART_BASE (DRAM_BASE + 0x7F400000ULL)
#define NVME_SART_SIZE (0xC00000ULL)

// regions 0x1/0x7/0xa are in-between, each with a size of 0x4000 bytes.

// Carveout region 0xC
#define PANIC_SIZE (0x80000ULL)
#define PANIC_BASE (NVME_SART_BASE - PANIC_SIZE - 0xC000ULL)

// Carveout region 0x50
#define REGION_50_SIZE (0x18000ULL)
#define REGION_50_BASE (PANIC_BASE - REGION_50_SIZE)

// Carveout region 0xE
#define DISPLAY_SIZE (0x854000ULL)
#define DISPLAY_BASE (REGION_50_BASE - DISPLAY_SIZE)

// Carveout region 0x4
#define TZ0_SIZE (0x1E00000ULL)
#define TZ0_BASE (DISPLAY_BASE - TZ0_SIZE)

// Carveout region 0x6
#define TZ1_SIZE (0x80000ULL)
#define TZ1_BASE (TZ0_BASE - TZ1_SIZE)

// Carveout region 0x18
#define KERNEL_REGION_BASE DRAM_BASE
#define KERNEL_REGION_SIZE ((TZ1_BASE + ~KERNEL_REGION_BASE + 0x4000ULL) & -0x4000ULL)

static void s8000_start_cpus(MachineState* machine, uint64_t cpu_mask)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    uint32_t                i;

    for (i = 0; i < machine->smp.cpus; i++) {
        if ((cpu_mask & BIT_ULL(i)) != 0 && apple_a9_cpu_is_off(s8000->cpus[i])) {
            apple_a9_cpu_set_on(s8000->cpus[i]);
        }
    }
}

static void s8000_create_s3c_uart(const AppleS8000MachineState* s8000, Chardev* chr)
{
    DeviceState* dev;
    hwaddr       base;
    uint32_t     vector;
    AppleDTProp* prop;
    hwaddr*      uart_offset;
    AppleDTNode* child;

    child = apple_dt_get_node(s8000->device_tree, "arm-io/uart0");
    assert_nonnull(child);

    assert_nonnull(apple_dt_get_prop(child, "boot-console"));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    uart_offset = (hwaddr*)prop->data;
    base        = s8000->armio_base + uart_offset[0];

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    vector = *(uint32_t*)prop->data;
    dev    = exynos4210_uart_create(base, 256, 0, chr, qdev_get_gpio_in(DEVICE(s8000->aic), vector));
    assert_nonnull(dev);
}

static void s8000_patch_kernel(MachoHeader64* header) { ck_patch_kernel(header); }

static bool s8000_check_panic(AppleS8000MachineState* s8000)
{
    AppleEmbeddedPanicHeader* panic_info;
    bool                      ret;

    if (s8000->panic_size == 0) { return false; }

    panic_info = g_malloc0(s8000->panic_size);

    address_space_rw(&address_space_memory, s8000->panic_base, MEMTXATTRS_UNSPECIFIED, panic_info, s8000->panic_size,
                     false);
    address_space_set(&address_space_memory, s8000->panic_base, 0, s8000->panic_size, MEMTXATTRS_UNSPECIFIED);

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

static void get_kaslr_slides(AppleS8000MachineState* s8000, hwaddr* phys_slide_out, vaddr* virt_slide_out)
{
    static const size_t slide_granular      = (1 << 21);
    static const size_t slide_granular_mask = slide_granular - 1;
    static const size_t slide_virt_max      = 0x100 * (2 * 1024 * 1024);

    hwaddr slide_phys;
    vaddr  slide_virt;
    size_t random_value = get_kaslr_random();

    if (s8000->kaslr_off) {
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

static void s8000_load_kernelcache(AppleS8000MachineState* s8000, const char* cmdline)
{
    MachineState*  machine = MACHINE(s8000);
    vaddr          text_base;
    vaddr          kc_base;
    vaddr          kc_end;
    vaddr          apple_dt_va;
    hwaddr         top_of_kernel_data_pa;
    hwaddr         phys_ptr;
    AppleBootInfo* info = &s8000->boot_info;
    hwaddr         prelink_text_base;
    AppleDTNode*   memory_map = apple_dt_get_node(s8000->device_tree, "/chosen/memory-map");
    vaddr          tz1_virt_low;
    vaddr          tz1_virt_high;

    apple_boot_get_kc_bounds(s8000->kernel, &text_base, &kc_base, &kc_end, NULL, NULL);

    info->kern_text_off = text_base - kc_base;

    prelink_text_base = apple_boot_get_segment(s8000->kernel, "__PRELINK_TEXT")->vmaddr;

    get_kaslr_slides(s8000, &g_phys_slide, &g_virt_slide);

    g_phys_base = KERNEL_REGION_BASE;
    g_virt_base = kc_base + (g_virt_slide - g_phys_slide);

    info->trustcache_addr = vtop_static(prelink_text_base + g_virt_slide) - info->trustcache_size;

    address_space_rw(&address_space_memory, info->trustcache_addr, MEMTXATTRS_UNSPECIFIED, s8000->trustcache,
                     info->trustcache_size, true);

    info->kern_entry = apple_boot_load_macho(s8000->kernel, &address_space_memory, memory_map,
                                             g_phys_base + g_phys_slide, g_virt_slide);

    info_report("Kernel virtual base: 0x%016" VADDR_PRIx, g_virt_base);
    info_report("Kernel physical base: 0x" HWADDR_FMT_plx, g_phys_base);
    info_report("Kernel text off: 0x" HWADDR_FMT_plx, info->kern_text_off);
    info_report("Kernel virtual slide: 0x%016" VADDR_PRIx, g_virt_slide);
    info_report("Kernel physical slide: 0x" HWADDR_FMT_plx, g_phys_slide);
    info_report("Kernel entry point: 0x%016" VADDR_PRIx, info->kern_entry);

    phys_ptr = vtop_static(ROUND_UP_16K(kc_end + g_virt_slide));

    // Device tree
    info->device_tree_addr  = phys_ptr;
    apple_dt_va             = ptov_static(info->device_tree_addr);
    phys_ptr               += ROUND_UP_16K(info->device_tree_size);

    // RAM disk
    if (machine->initrd_filename) {
        info->ramdisk_addr = phys_ptr;
        apple_boot_load_ramdisk(machine->initrd_filename, &address_space_memory, info->ramdisk_addr,
                                &info->ramdisk_size);
        info->ramdisk_size  = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr           += info->ramdisk_size;
    }

    phys_ptr          = ROUND_UP_16K(phys_ptr);
    info->sep_fw_addr = phys_ptr;
    if (s8000->sep_fw_filename) {
        // TODO
    }
    info->sep_fw_size  = 8 * MiB;
    phys_ptr          += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr  = phys_ptr;
    info->kern_boot_args_size  = 0x4000;
    phys_ptr                  += info->kern_boot_args_size;

    apple_boot_finalise_dt(s8000->device_tree, &address_space_memory, info);

    top_of_kernel_data_pa = (ROUND_UP_16K(phys_ptr) + 0x3000ull) & ~0x3FFFull;

    info_report("Boot args: [%s]", cmdline);
    apple_boot_setup_bootargs(s8000->build_version, &address_space_memory, info->kern_boot_args_addr, g_virt_base,
                              g_phys_base, KERNEL_REGION_SIZE, top_of_kernel_data_pa, apple_dt_va,
                              info->device_tree_size, &s8000->video_args, cmdline, machine->ram_size);
    g_virt_base = kc_base;

    apple_boot_get_kc_bounds(s8000->secure_monitor, NULL, &tz1_virt_low, &tz1_virt_high, NULL, NULL);
    info_report("TrustZone 1 virtual address low: 0x%016" VADDR_PRIx, tz1_virt_low);
    info_report("TrustZone 1 virtual address high: 0x%016" VADDR_PRIx, tz1_virt_high);
    vaddr tz1_entry = apple_boot_load_macho(s8000->secure_monitor, &address_space_memory, NULL, TZ1_BASE, 0);
    info_report("TrustZone 1 entry: 0x%016" VADDR_PRIx, tz1_entry);
    hwaddr tz1_boot_args_pa = TZ1_BASE + (TZ1_SIZE - sizeof(AppleMonitorBootArgs));
    info_report("TrustZone 1 boot args address: 0x" HWADDR_FMT_plx, tz1_boot_args_pa);
    apple_boot_setup_monitor_boot_args(&address_space_memory, tz1_boot_args_pa, tz1_virt_low, TZ1_BASE, TZ1_SIZE,
                                       s8000->boot_info.kern_boot_args_addr, s8000->boot_info.kern_entry, g_phys_base,
                                       g_phys_slide, g_virt_slide, info->kern_text_off);
    s8000->boot_info.tz1_entry        = tz1_entry;
    s8000->boot_info.tz1_boot_args_pa = tz1_boot_args_pa;
}

static void s8000_memory_setup(MachineState* machine)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    AppleBootInfo*          info  = &s8000->boot_info;
    AppleNvram*             nvram;
    bool                    auto_boot;
    char*                   cmdline;
    MachoHeader64*          header;
    AppleDTNode*            memory_map;

    apple_dt_unfinalise(s8000->device_tree);

    memory_map = apple_dt_get_node(s8000->device_tree, "/chosen/memory-map");

    if (s8000_check_panic(s8000)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = DRAM_BASE;
    info->dram_size = DRAM_SIZE;

    nvram = APPLE_NVRAM(object_resolve_path_at(NULL, "/machine/peripheral/nvram"));
    if (!nvram) {
        error_setg(&error_fatal, "Failed to find NVRAM device");
        return;
    }
    apple_nvram_load(nvram);

    auto_boot = env_get_bool(nvram, "auto-boot", false);

    info_report("Boot Mode: %u", s8000->boot_info.boot_mode);
    switch (s8000->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery: auto_boot = false; goto set_boot_mode;
        case kAppleBootModeExitRecovery:
            auto_boot = true;
        set_boot_mode:
            env_set_bool(nvram, "auto-boot", auto_boot, 0);
            s8000->boot_info.boot_mode = kAppleBootModeAuto;
            break;
        default: break;
    }

    info_report("auto-boot=%s", auto_boot ? "true" : "false");

    if (machine->initrd_filename == NULL && !auto_boot) {
        error_setg(&error_fatal, "RAM Disk required for recovery, please specify it via `-initrd`.");
        return;
    }

    if (auto_boot) { cmdline = g_strdup(machine->kernel_cmdline); }
    else {
        cmdline = g_strconcat("-restore rd=md0 nand-enable-reformat=1 ", machine->kernel_cmdline, NULL);
    }

    apple_nvram_save(nvram);

    info->nvram_size = nvram->len;

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) { info->nvram_size = XNU_MAX_NVRAM_SIZE; }

    if (apple_nvram_serialize(nvram, info->nvram_data, sizeof(info->nvram_data)) < 0) {
        error_report("Failed to read NVRAM");
    }

    if (s8000->securerom_filename != NULL) {
        address_space_rw(&address_space_memory, SROM_BASE, MEMTXATTRS_UNSPECIFIED, s8000->securerom,
                         s8000->securerom_size, 1);
        return;
    }

    // HACK: Use DEV Hardware model to restore without FDR errors
    apple_dt_set_prop(s8000->device_tree, "compatible", 26, "N66DEV\0iPhone8,2\0AppleARM");

    AppleDTNode* chosen = apple_dt_get_node(s8000->device_tree, "chosen");
    if (!apple_boot_contains_boot_arg(cmdline, "rd=", true)) {
        apple_dt_set_prop_strn(chosen, "root-matching", 256,
                               "<dict><key>IOProviderClass</key><string>IOMedia</"
                               "string><key>IOPropertyMatch</key><dict><key>Partition "
                               "ID</key><integer>1</integer></dict></dict>");
    }

    AppleDTNode* pram = apple_dt_get_node(s8000->device_tree, "pram");
    if (pram) {
        uint64_t panic_reg[2] = {0};
        uint64_t panic_base   = PANIC_BASE;
        uint64_t panic_size   = PANIC_SIZE;

        panic_reg[0] = panic_base;
        panic_reg[1] = panic_size;

        apple_dt_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        apple_dt_set_prop_u64(chosen, "embedded-panic-log-size", panic_size);
        s8000->panic_base = panic_base;
        s8000->panic_size = panic_size;
    }

    AppleDTNode* vram = apple_dt_get_node(s8000->device_tree, "vram");
    if (vram) {
        uint64_t vram_reg[2] = {0};
        uint64_t vram_base   = DISPLAY_BASE;
        uint64_t vram_size   = DISPLAY_SIZE;
        vram_reg[0]          = vram_base;
        vram_reg[1]          = vram_size;
        apple_dt_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
    }

    header = s8000->kernel;
    assert_nonnull(header);

    apple_boot_allocate_segment_records(memory_map, header);

    apple_boot_populate_dt(s8000->device_tree, info, auto_boot);

    switch (header->file_type) {
        case MH_EXECUTE:
        case MH_FILESET:
            s8000_load_kernelcache(s8000, cmdline);
            g_free(cmdline);
            break;
        default:
            error_setg(&error_fatal, "Unsupported kernelcache type: 0x%x\n", header->file_type);
            assert_not_reached();
    }
}

static void pmgr_unk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
#if 0
    hwaddr base = (hwaddr)opaque;
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" TARGET_FMT_lx
                  " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                  base + addr, base, data);
#endif
}

static uint64_t pmgr_unk_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(qdev_get_machine());
    hwaddr                  base  = (hwaddr)opaque;

    uint32_t security_epoch          = 1;    // On IMG4: Security Epoch ; On IMG3: Minimum
                                             // Epoch, verified on SecureROM s5l8955xsi
    bool     current_prod            = true;
    bool     current_secure_mode     = true;    // T8015 SEPOS Kernel also requires this.
    uint32_t security_domain         = 1;
    bool     raw_prod                = true;
    bool     raw_secure_mode         = true;
    uint32_t sep_bit30_current_value = 0;
    bool     fuses_locked            = true;
    uint32_t ret                     = 0x0;

    switch (base + addr) {
        case 0x102BC000:    // CFG_FUSE0
            //     // handle SEP DSEC demotion
            //     if (sep != NULL && sep->pmgr_fuse_changer_bit1_was_set)
            //         current_secure_mode = 0; // SEP DSEC img4 tag demotion active
            ret |= ((uint32_t)current_prod << 0);
            ret |= ((uint32_t)current_secure_mode << 1);
            ret |= ((security_domain & 3) << 2);
            ret |= ((s8000->board_id & 7) << 4);
            ret |= ((security_epoch & 0x7f) << 9);
            // ret |= (( & ) << );
            return ret;
        case 0x102BC200:    // CFG_FUSE0_RAW
            ret |= ((uint32_t)raw_prod << 0);
            ret |= ((uint32_t)raw_secure_mode << 1);
            return ret;
        case 0x102BC080:                              // ECID_LO
            return extract64(s8000->ecid, 0, 32);     // ECID lower
        case 0x102BC084:                              // ECID_HI
            return extract64(s8000->ecid, 32, 32);    // ECID upper
        case 0x102E8000:                              // ????
            return 0x4;
        case 0x102BC104:    // ???? bit 24 => is fresh boot?
            return BIT32(24) | BIT32(25);
        default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" TARGET_FMT_lx
                      " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                      base + addr, base, 0);
#endif
            break;
    }
    return 0;
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write = pmgr_unk_reg_write,
    .read  = pmgr_unk_reg_read,
};

static void pmgr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    MachineState*           machine = opaque;
    AppleS8000MachineState* s8000   = opaque;
    uint32_t                value   = (uint32_t)data;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, data);
#endif

    if (addr >= 0x80000 && addr <= 0x88010) { value = (value & 0xf) << 4 | (value & 0xf); }

    switch (addr) {
        case 0x80400:    // SEP Power State, Manual & Actual: Run Max
            value = 0xFF;
            break;
        case 0xD4004: s8000_start_cpus(machine, data);
        default     : break;
    }
    memcpy(s8000->pmgr_reg + addr, &value, size);
}

static uint64_t pmgr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleS8000MachineState* s8000  = opaque;
    uint64_t                result = 0;

    memcpy(&result, s8000->pmgr_reg + addr, size);
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg READ @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, result);
#endif
    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write = pmgr_reg_write,
    .read  = pmgr_reg_read,
};

static void s8000_cpu_setup(AppleS8000MachineState* s8000)
{
    uint32_t      i;
    AppleDTNode*  root;
    MachineState* machine = MACHINE(s8000);
    GList*        iter;
    GList*        next = NULL;

    root = apple_dt_get_node(s8000->device_tree, "cpus");
    assert_nonnull(root);
    object_initialize_child(OBJECT(s8000), "cluster", &s8000->cluster, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s8000->cluster), "cluster-id", 0);

    for (iter = root->children, i = 0; iter; iter = next, i++) {
        AppleDTNode* node;

        next = iter->next;
        node = (AppleDTNode*)iter->data;
        if (i >= machine->smp.cpus) {
            apple_dt_del_node(root, node);
            continue;
        }

        s8000->cpus[i] = apple_a9_from_node(node);

        object_property_add_child(OBJECT(&s8000->cluster), DEVICE(s8000->cpus[i])->id, OBJECT(s8000->cpus[i]));

        qdev_realize(DEVICE(s8000->cpus[i]), NULL, &error_fatal);
    }
    qdev_realize(DEVICE(&s8000->cluster), NULL, &error_fatal);
}

static void s8000_create_aic(AppleS8000MachineState* s8000)
{
    uint32_t      i;
    hwaddr*       reg;
    AppleDTProp*  prop;
    MachineState* machine = MACHINE(s8000);
    AppleDTNode*  soc     = apple_dt_get_node(s8000->device_tree, "arm-io");
    AppleDTNode*  child;
    AppleDTNode*  timebase;

    assert_nonnull(soc);
    child = apple_dt_get_node(soc, "aic");
    assert_nonnull(child);
    timebase = apple_dt_get_node(soc, "aic-timebase");
    assert_nonnull(timebase);

    s8000->aic = apple_aic_create(machine->smp.cpus, child, timebase);
    object_property_add_child(OBJECT(s8000), "aic", OBJECT(s8000->aic));
    assert_nonnull(s8000->aic);
    sysbus_realize(s8000->aic, &error_fatal);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (hwaddr*)prop->data;

    for (i = 0; i < machine->smp.cpus; i++) {
        memory_region_add_subregion_overlap(&s8000->cpus[i]->memory, s8000->armio_base + reg[0],
                                            sysbus_mmio_get_region(s8000->aic, i), 0);
        sysbus_connect_irq(s8000->aic, i, qdev_get_gpio_in(DEVICE(s8000->cpus[i]), ARM_CPU_IRQ));
    }
}

static void s8000_pmgr_setup(AppleS8000MachineState* s8000)
{
    uint64_t*    reg;
    uint32_t     i;
    char         name[32];
    AppleDTProp* prop;
    AppleDTNode* child;

    child = apple_dt_get_node(s8000->device_tree, "arm-io/pmgr");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    for (i = 0; i < prop->len / 8; i += 2) {
        MemoryRegion* mem = g_new(MemoryRegion, 1);
        if (i == 0) { memory_region_init_io(mem, OBJECT(s8000), &pmgr_reg_ops, s8000, "pmgr-reg", reg[i + 1]); }
        else {
            snprintf(name, sizeof(name), "pmgr-unk-reg-%d", i);
            memory_region_init_io(mem, OBJECT(s8000), &pmgr_unk_reg_ops, (void*)reg[i], name, reg[i + 1]);
        }
        memory_region_add_subregion_overlap(
            s8000->sys_mem, reg[i] + reg[i + 1] < s8000->armio_size ? s8000->armio_base + reg[i] : reg[i], mem, -1);
    }

    apple_dt_set_prop(child, "voltage-states1", sizeof(s8000_voltage_states1), s8000_voltage_states1);
}

static void s8000_create_dart(AppleS8000MachineState* s8000, const char* name, bool absolute_mmio)
{
    AppleDARTState* dart = NULL;
    AppleDTProp*    prop;
    uint64_t*       reg;
    uint32_t        i;
    AppleDTNode*    child;

    child = apple_dt_get_node(s8000->device_tree, "arm-io");
    assert_nonnull(child);

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    dart = apple_dart_from_node(child);
    assert_nonnull(dart);
    object_property_add_child(OBJECT(s8000), name, OBJECT(dart));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    for (i = 0; i < prop->len / 16; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(dart), i, (absolute_mmio ? 0 : s8000->armio_base) + reg[i * 2]);
    }

    // if there's SMMU there are two indices, 2nd being the SMMU,
    // the code below should be brought back if SMMU is ever implemented
    //
    // for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
    //     sysbus_connect_irq(
    //         SYS_BUS_DEVICE(dart), i,
    //         qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    // }
    sysbus_connect_irq(SYS_BUS_DEVICE(dart), 0,
                       qdev_get_gpio_in(DEVICE(s8000->aic), apple_dt_get_prop_u32(child, "interrupts", &error_warn)));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
}

static void s8000_create_chestnut(AppleS8000MachineState* s8000)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(s8000->device_tree, "arm-io/i2c0/display-pmu");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(s8000), "i2c0", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_CHESTNUT, *(uint8_t*)prop->data);
}

static void s8000_create_pcie(AppleS8000MachineState* s8000)
{
    uint32_t      i;
    uint32_t*     ints;
    AppleDTProp*  prop;
    uint64_t*     reg;
    SysBusDevice* pcie;

    uint32_t chip_id = apple_dt_get_prop_u32(apple_dt_get_node(s8000->device_tree, "chosen"), "chip-id", &error_fatal);

    AppleDTNode* child = apple_dt_get_node(s8000->device_tree, "arm-io/apcie");
    assert_nonnull(child);

    // TODO: S8000 needs it, and probably T8030 does need it as well.
    apple_dt_set_prop_null(child, "apcie-phy-tunables");

    pcie = apple_pcie_from_node(child, chip_id);
    assert_nonnull(pcie);
    object_property_add_child(OBJECT(s8000), "pcie", OBJECT(pcie));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    // TODO: Hook up all ports
    // sysbus_mmio_map(pcie, 0, reg[0 * 2]);
    // sysbus_mmio_map(pcie, 1, reg[9 * 2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints                      = (uint32_t*)prop->data;
    uint32_t interrupts_count = prop->len / sizeof(uint32_t);

    for (i = 0; i < interrupts_count; i++) {
        sysbus_connect_irq(pcie, i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }
    prop = apple_dt_get_prop(child, "msi-vector-offset");
    assert_nonnull(prop);
    uint32_t msi_vector_offset = *(uint32_t*)prop->data;
    prop                       = apple_dt_get_prop(child, "#msi-vectors");
    assert_nonnull(prop);
    uint32_t msi_vectors = *(uint32_t*)prop->data;
    for (i = 0; i < msi_vectors; i++) {
        sysbus_connect_irq(pcie, interrupts_count + i, qdev_get_gpio_in(DEVICE(s8000->aic), msi_vector_offset + i));
    }

    sysbus_realize_and_unref(pcie, &error_fatal);
}

static void s8000_create_nvme(AppleS8000MachineState* s8000)
{
    uint32_t           i;
    uint32_t*          ints;
    AppleDTProp*       prop;
    uint64_t*          reg;
    SysBusDevice*      nvme;
    AppleNVMeMMUState* s;
    AppleDTNode *      child, *child_s3e;
    ApplePCIEHost*     apcie_host;

    child = apple_dt_get_node(s8000->device_tree, "arm-io/nvme-mmu0");
    assert_nonnull(child);

    child_s3e = apple_dt_get_node(s8000->device_tree, "arm-io/apcie/pci-bridge0/s3e");
    assert_nonnull(child_s3e);

    // might also work without the sart regions?

    uint64_t sart_region[2];
    sart_region[0] = NVME_SART_BASE;
    sart_region[1] = NVME_SART_SIZE;
    apple_dt_set_prop(child, "sart-region", sizeof(sart_region), &sart_region);

    uint32_t sart_virtual_base;
    prop = apple_dt_get_prop(child, "sart-virtual-base");
    assert_nonnull(prop);
    sart_virtual_base = *(uint32_t*)prop->data;

    uint64_t nvme_scratch_virt_region[2];
    nvme_scratch_virt_region[0] = sart_virtual_base;
    nvme_scratch_virt_region[1] = NVME_SART_SIZE;
    apple_dt_set_prop(child_s3e, "nvme-scratch-virt-region", sizeof(nvme_scratch_virt_region),
                      &nvme_scratch_virt_region);

    PCIBridge* pci     = PCI_BRIDGE(object_property_get_link(OBJECT(s8000), "pcie.bridge0", &error_fatal));
    PCIBus*    sec_bus = pci_bridge_get_sec_bus(pci);
    apcie_host         = APPLE_PCIE_HOST(object_property_get_link(OBJECT(s8000), "pcie.host", &error_fatal));
    nvme               = apple_nvme_mmu_from_node(child, sec_bus);
    assert_nonnull(nvme);
    object_property_add_child(OBJECT(s8000), "nvme", OBJECT(nvme));

    s = APPLE_NVME_MMU(nvme);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(nvme, 0, reg[0]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 4);
    ints = (uint32_t*)prop->data;

    sysbus_connect_irq(nvme, 0, qdev_get_gpio_in(DEVICE(s8000->aic), ints[0]));

#if 0
    uint32_t bridge_index = 0;
    qdev_connect_gpio_out_named(
        DEVICE(apcie_host), "interrupt_pci", bridge_index,
        qdev_get_gpio_in_named(DEVICE(nvme), "interrupt_pci", 0));
#endif

    AppleDARTState* dart = APPLE_DART(object_property_get_link(OBJECT(s8000), "dart-apcie0", &error_fatal));
    assert_nonnull(dart);
    child = apple_dt_get_node(s8000->device_tree, "arm-io/dart-apcie0/mapper-apcie0");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    s->dma_mr = MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t*)prop->data));
    assert_nonnull(s->dma_mr);
    object_property_add_const_link(OBJECT(nvme), "dma_mr", OBJECT(s->dma_mr));
    address_space_init(&s->dma_as, s->dma_mr, "apcie0.dma");

    sysbus_realize_and_unref(nvme, &error_fatal);
}

static void s8000_create_gpio(AppleS8000MachineState* s8000, const char* name)
{
    DeviceState* gpio = NULL;
    AppleDTProp* prop;
    uint64_t*    reg;
    uint32_t*    ints;
    uint32_t     i;
    AppleDTNode* child = apple_dt_get_node(s8000->device_tree, "arm-io");

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);
    gpio = apple_gpio_from_node(child);
    assert_nonnull(gpio);
    object_property_add_child(OBJECT(s8000), name, OBJECT(gpio));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, s8000->armio_base + reg[0]);
    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(gpio), i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
}

static void s8000_create_i2c(AppleS8000MachineState* s8000, const char* name)
{
    SysBusDevice* i2c;
    AppleDTProp*  prop;
    uint64_t*     reg;
    uint32_t*     ints;
    uint32_t      i;
    AppleDTNode*  child = apple_dt_get_node(s8000->device_tree, "arm-io");

    child = apple_dt_get_node(child, name);
    assert_nonnull(child);
    i2c = apple_i2c_create(name);
    assert_nonnull(i2c);
    object_property_add_child(OBJECT(s8000), name, OBJECT(i2c));

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    sysbus_mmio_map(i2c, 0, s8000->armio_base + reg[0]);
    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);

    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(i2c, i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }

    sysbus_realize_and_unref(i2c, &error_fatal);
}

static void s8000_create_spi0(AppleS8000MachineState* s8000)
{
    DeviceState* spi  = NULL;
    DeviceState* gpio = NULL;
    // Object *sio;
    const char* name = "spi0";

    spi = qdev_new(TYPE_APPLE_SPI);
    assert_nonnull(spi);
    DEVICE(spi)->id = g_strdup(name);
    object_property_add_child(OBJECT(s8000), name, OBJECT(spi));

    // sio = object_property_get_link(OBJECT(s8000), "sio",
    // &error_fatal);
    // assert_nonnull(object_property_add_const_link(OBJECT(spi), "sio",
    // sio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, s8000->armio_base + SPI0_BASE);

    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, qdev_get_gpio_in(DEVICE(s8000->aic), SPI0_IRQ));
    // The second sysbus IRQ is the cs line
    gpio = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));
    assert_nonnull(gpio);
    qdev_connect_gpio_out(gpio, GPIO_SPI0_CS, qdev_get_gpio_in_named(spi, SSI_GPIO_CS, 0));
}

static void s8000_create_spi(AppleS8000MachineState* s8000, uint32_t port)
{
    SysBusDevice* spi  = NULL;
    DeviceState*  gpio = NULL;
    AppleDTProp*  prop;
    uint64_t*     reg;
    uint32_t*     ints;
    AppleDTNode*  child = apple_dt_get_node(s8000->device_tree, "arm-io");
    // Object *sio;
    char     name[32] = {0};
    hwaddr   base;
    uint32_t irq;
    uint32_t cs_pin;

    snprintf(name, sizeof(name), "spi%u", port);
    child = apple_dt_get_node(child, name);
    assert_nonnull(child);

    spi = apple_spi_from_node(child);
    assert_nonnull(spi);
    object_property_add_child(OBJECT(s8000), name, OBJECT(spi));

    // sio = object_property_get_link(OBJECT(s8000), "sio",
    // &error_fatal);
    // assert_nonnull(object_property_add_const_link(OBJECT(spi), "sio",
    // sio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg  = (uint64_t*)prop->data;
    base = s8000->armio_base + reg[0];
    sysbus_mmio_map(spi, 0, base);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;
    irq  = ints[0];

    // The second sysbus IRQ is the cs line
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0, qdev_get_gpio_in(DEVICE(s8000->aic), irq));

    prop = apple_dt_get_prop(child, "function-spi_cs0");
    assert_nonnull(prop);
    if (prop->len >= sizeof(uint32_t) * 3) {
        ints   = (uint32_t*)prop->data;
        cs_pin = ints[2];
        gpio   = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));
        assert_nonnull(gpio);
        qdev_connect_gpio_out(gpio, cs_pin, qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
    }
}

static void s8000_create_usb(AppleS8000MachineState* s8000)
{
    AppleDTNode* child = apple_dt_get_node(s8000->device_tree, "arm-io");
    AppleDTNode *phy, *complex, *device;
    AppleDTProp* prop;
    DeviceState* otg;
    DeviceState* host;

    phy = apple_dt_get_node(child, "otgphyctrl");
    assert_nonnull(phy);

    complex = apple_dt_get_node(child, "usb-complex");
    assert_nonnull(complex);

    device = apple_dt_get_node(complex, "usb-device");
    assert_nonnull(device);

    otg = apple_otg_from_node(complex);
    object_property_add_child(OBJECT(s8000), "otg", OBJECT(otg));

    host = usb_uplink_new(s8000->usb_uplink_type, s8000->usb_uplink_addr, &error_fatal);
    object_property_add_child(OBJECT(s8000), "usb-host", OBJECT(host));
    object_property_set_link(OBJECT(otg), "host", OBJECT(host), &error_fatal);
    object_unref(OBJECT(host));

    prop = apple_dt_get_prop(phy, "reg");
    assert_nonnull(prop);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 0, s8000->armio_base + ((uint64_t*)prop->data)[0]);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 1, s8000->armio_base + ((uint64_t*)prop->data)[2]);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 2,
                    s8000->armio_base + ((uint64_t*)apple_dt_get_prop(complex, "ranges")->data)[1]
                        + ((uint64_t*)apple_dt_get_prop(device, "reg")->data)[0]);

    prop = apple_dt_get_prop(complex, "reg");
    if (prop) { sysbus_mmio_map(SYS_BUS_DEVICE(otg), 3, s8000->armio_base + ((uint64_t*)prop->data)[0]); }
    // no-pmu is needed for T8015, and is also necessary for S8000.
    apple_dt_set_prop_u32(complex, "no-pmu", 1);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(otg), &error_fatal);

    prop = apple_dt_get_prop(device, "interrupts");
    assert_nonnull(prop);
    sysbus_connect_irq(SYS_BUS_DEVICE(otg), 0, qdev_get_gpio_in(DEVICE(s8000->aic), ((uint32_t*)prop->data)[0]));
}

static void s8000_create_wdt(AppleS8000MachineState* s8000)
{
    uint32_t      i;
    uint32_t*     ints;
    AppleDTProp*  prop;
    uint64_t*     reg;
    SysBusDevice* wdt;
    AppleDTNode*  child = apple_dt_get_node(s8000->device_tree, "arm-io");

    assert_nonnull(child);
    child = apple_dt_get_node(child, "wdt");
    assert_nonnull(child);

    wdt = apple_wdt_from_node(child);
    assert_nonnull(wdt);

    object_property_add_child(OBJECT(s8000), "wdt", OBJECT(wdt));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(wdt, 0, s8000->armio_base + reg[0]);
    sysbus_mmio_map(wdt, 1, s8000->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(wdt, i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }

    // TODO: MCC
    apple_dt_del_prop_named(child, "function-panic_flush_helper");
    apple_dt_del_prop_named(child, "function-panic_halt_helper");

    apple_dt_set_prop_u32(child, "no-pmu", 1);

    sysbus_realize_and_unref(wdt, &error_fatal);
}

static void s8000_create_aes(AppleS8000MachineState* s8000)
{
    AppleDTNode*  child;
    SysBusDevice* aes;
    AppleDTProp*  prop;
    uint64_t*     reg;
    uint32_t*     ints;

    child = apple_dt_get_node(s8000->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, "aes");
    assert_nonnull(child);

    aes = apple_aes_create(child, s8000->board_id);
    assert_nonnull(aes);

    object_property_add_child(OBJECT(s8000), "aes", OBJECT(aes));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(aes, 0, s8000->armio_base + reg[0]);
    sysbus_mmio_map(aes, 1, s8000->armio_base + reg[2]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, ==, 4);
    ints = (uint32_t*)prop->data;

    sysbus_connect_irq(aes, 0, qdev_get_gpio_in(DEVICE(s8000->aic), *ints));

    object_property_add_const_link(OBJECT(aes), "dma-mr", OBJECT(s8000->sys_mem));

    sysbus_realize_and_unref(aes, &error_fatal);
}

static void s8000_create_sep(AppleS8000MachineState* s8000)
{
    AppleDTNode* child;
    AppleDTProp* prop;
    uint64_t*    reg;
    uint32_t*    ints;
    uint32_t     i;

    child = apple_dt_get_node(s8000->device_tree, "arm-io");
    assert_nonnull(child);
    child = apple_dt_get_node(child, "sep");
    assert_nonnull(child);

    s8000->sep = SYS_BUS_DEVICE(apple_sep_sim_from_node(child, false));
    assert_nonnull(s8000->sep);

    object_property_add_child(OBJECT(s8000), "sep", OBJECT(s8000->sep));
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s8000->sep), 0, s8000->armio_base + reg[0], 2);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    for (i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s8000->sep), i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }

    object_property_add_const_link(OBJECT(s8000->sep), "dma-mr", OBJECT(s8000->sys_mem));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s8000->sep), &error_fatal);
}

static void s8000_create_pmu(AppleS8000MachineState* s8000)
{
    AppleI2CState* i2c;
    AppleDTNode*   child;
    AppleDTProp*   prop;
    DeviceState*   dev;
    DeviceState*   gpio;
    uint32_t*      ints;

    i2c = APPLE_I2C(object_property_get_link(OBJECT(s8000), "i2c0", &error_fatal));

    child = apple_dt_get_node(s8000->device_tree, "arm-io/i2c0/pmu");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    dev = DEVICE(i2c_slave_create_simple(i2c->bus, TYPE_PMU_D2255, *(uint8_t*)prop->data));

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    ints = (uint32_t*)prop->data;

    gpio = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(gpio, ints[0]));
}

static void s8000_display_create(AppleS8000MachineState* s8000)
{
    MachineState* machine;
    SysBusDevice* sbd;
    AppleDTNode*  child;
    uint64_t*     reg;
    AppleDTProp*  prop;

    machine = MACHINE(s8000);

    AppleDARTState* dart = APPLE_DART(object_property_get_link(OBJECT(s8000), "dart-disp0", &error_fatal));
    assert_nonnull(dart);
    child = apple_dt_get_node(s8000->device_tree, "arm-io/dart-disp0/mapper-disp0");
    assert_nonnull(child);
    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/disp0");
    assert_nonnull(child);

    sbd = adp_v2_from_node(child, MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t*)prop->data)), &s8000->video_args,
                           DISPLAY_SIZE);
    s8000->video_args.base_addr = DISPLAY_BASE;
    s8000->video_args.display   = !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-s", false)
                                  && !apple_boot_contains_boot_arg(machine->kernel_cmdline, "-v", false);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    sysbus_mmio_map(sbd, 0, s8000->armio_base + reg[0]);
    sysbus_mmio_map(sbd, 1, s8000->armio_base + reg[2]);
    sysbus_mmio_map(sbd, 2, s8000->armio_base + reg[4]);
    sysbus_mmio_map(sbd, 3, s8000->armio_base + reg[6]);
    sysbus_mmio_map(sbd, 4, s8000->armio_base + reg[8]);
    sysbus_mmio_map(sbd, 5, s8000->armio_base + reg[10]);

    prop = apple_dt_get_prop(child, "interrupts");
    assert_nonnull(prop);
    uint32_t* ints = (uint32_t*)prop->data;

    for (uint32_t i = 0; i < prop->len / sizeof(uint32_t); i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(DEVICE(s8000->aic), ints[i]));
    }

    adp_v2_update_vram_mapping(APPLE_DISPLAY_PIPE_V2(sbd), s8000->sys_mem, s8000->video_args.base_addr);
    object_property_add_child(OBJECT(s8000), "disp0", OBJECT(sbd));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void s8000_create_backlight(AppleS8000MachineState* s8000)
{
    AppleDTNode*   child;
    AppleDTProp*   prop;
    AppleI2CState* i2c;

    child = apple_dt_get_node(s8000->device_tree, "arm-io/i2c0/lm3539");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(s8000), "i2c0", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, *(uint8_t*)prop->data);

    child = apple_dt_get_node(s8000->device_tree, "arm-io/i2c2/lm3539-1");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "reg");
    assert_nonnull(prop);
    i2c = APPLE_I2C(object_property_get_link(OBJECT(s8000), "i2c2", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT, *(uint8_t*)prop->data);
}

static void s8000_cpu_reset(AppleS8000MachineState* s8000)
{
    CPUState* cpu;
    AppleA9*  acpu;

    CPU_FOREACH (cpu) {
        acpu = APPLE_A9(cpu);
        if (s8000->securerom_filename == NULL) {
            object_property_set_int(OBJECT(cpu), "rvbar", TZ1_BASE, &error_abort);
            cpu_reset(cpu);
            if (acpu->cpu_id == 0) {
                arm_set_cpu_on(acpu->parent_obj.mp_affinity, s8000->boot_info.tz1_entry,
                               s8000->boot_info.tz1_boot_args_pa, 3, true);
            }
        }
        else {
            object_property_set_int(OBJECT(cpu), "rvbar", SROM_BASE, &error_abort);
            cpu_reset(cpu);
            if (acpu->cpu_id == 0) { arm_set_cpu_on(acpu->parent_obj.mp_affinity, SROM_BASE, 0, 3, true); }
        }
    }
}

static void s8000_reset(MachineState* machine, ResetType type)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    DeviceState*            gpio  = NULL;

    qemu_devices_reset(type);

    if (!runstate_check(RUN_STATE_PRELAUNCH)) { s8000_memory_setup(MACHINE(s8000)); }

    s8000_cpu_reset(s8000);

    gpio = DEVICE(object_property_get_link(OBJECT(s8000), "gpio", &error_fatal));

    qemu_set_irq(qdev_get_gpio_in(gpio, GPIO_FORCE_DFU), s8000->force_dfu);
}

static void s8000_init_done(Notifier* notifier, void* data)
{
    AppleS8000MachineState* s8000 = container_of(notifier, AppleS8000MachineState, init_done_notifier);
    s8000_memory_setup(MACHINE(s8000));
}

static void s8000_init(MachineState* machine)
{
    AppleS8000MachineState* s8000 = APPLE_S8000(machine);
    AppleDTNode*            child;
    AppleDTProp*            prop;
    hwaddr*                 ranges;
    uint32_t                build_version;
    vaddr                   kc_base;
    vaddr                   kc_end;

    s8000->sys_mem = get_system_memory();
    allocate_ram(s8000->sys_mem, "SROM", SROM_BASE, SROM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "SRAM", SRAM_BASE, SRAM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "DRAM", DRAM_BASE, DRAM_SIZE, 0);
    allocate_ram(s8000->sys_mem, "SEPROM", SEPROM_BASE, SEPROM_SIZE, 0);
    MemoryRegion* mr = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(s8000), "s8000.seprom.alias", s8000->sys_mem, SEPROM_BASE, SEPROM_SIZE);
    memory_region_add_subregion_overlap(s8000->sys_mem, 0, mr, 1);

    s8000->device_tree = apple_boot_load_dt_file(machine->dtb);
    if (s8000->device_tree == NULL) {
        error_setg(&error_fatal, "Failed to load device tree");
        return;
    }

    if (s8000->securerom_filename == NULL) {
        s8000->kernel = apple_boot_load_kernel(machine->kernel_filename, &s8000->secure_monitor);
        assert_nonnull(s8000->kernel);
        assert_nonnull(s8000->secure_monitor);
        build_version = apple_boot_build_version(s8000->kernel);
        info_report("%s %u.%u.%u...", apple_boot_platform_string(s8000->kernel), BUILD_VERSION_MAJOR(build_version),
                    BUILD_VERSION_MINOR(build_version), BUILD_VERSION_PATCH(build_version));
        s8000->build_version = build_version;

        apple_boot_get_kc_bounds(s8000->kernel, NULL, &kc_base, &kc_end, NULL, NULL);
        info_report("Kernel virtual low: 0x%016" VADDR_PRIx, kc_base);
        info_report("Kernel virtual high: 0x%016" VADDR_PRIx, kc_end);

        g_virt_base = kc_base;
        g_phys_base = (hwaddr)apple_boot_get_macho_buffer(s8000->kernel);

        s8000_patch_kernel(s8000->kernel);

        s8000->trustcache =
            apple_boot_load_trustcache_file(s8000->trustcache_filename, &s8000->boot_info.trustcache_size);
        if (s8000->ticket_filename != NULL) {
            if (!g_file_get_contents(s8000->ticket_filename, &s8000->boot_info.ticket_data,
                                     &s8000->boot_info.ticket_length, NULL))
            {
                error_setg(&error_fatal, "Failed to read ticket from `%s`", s8000->ticket_filename);
                return;
            }
        }
    }
    else {
        if (!g_file_get_contents(s8000->securerom_filename, &s8000->securerom, &s8000->securerom_size, NULL)) {
            error_setg(&error_fatal, "Failed to load SecureROM from `%s`", s8000->securerom_filename);
            return;
        }
    }

    apple_dt_set_prop_u32(s8000->device_tree, "clock-frequency", 24000000);
    child = apple_dt_get_node(s8000->device_tree, "arm-io");
    assert_nonnull(child);

    apple_dt_set_prop_u32(child, "chip-revision", 0);

    apple_dt_set_prop(child, "clock-frequencies", sizeof(s8000_clock_frequencies), s8000_clock_frequencies);

    prop = apple_dt_get_prop(child, "ranges");
    assert_nonnull(prop);

    ranges            = (hwaddr*)prop->data;
    s8000->armio_base = ranges[1];
    s8000->armio_size = ranges[2];

    apple_dt_set_prop_strn(s8000->device_tree, "platform-name", 32, "s8000");
    apple_dt_set_prop_strn(s8000->device_tree, "model-number", 32, "MWL72");
    apple_dt_set_prop_strn(s8000->device_tree, "region-info", 32, "LL/A");
    apple_dt_set_prop_strn(s8000->device_tree, "config-number", 64, "");
    apple_dt_set_prop_strn(s8000->device_tree, "serial-number", 32, "C39ZRMDEN72J");
    apple_dt_set_prop_strn(s8000->device_tree, "mlb-serial-number", 32, "C39948108J9N72J1F");
    apple_dt_set_prop_strn(s8000->device_tree, "regulatory-model-number", 32, "A2111");

    child = apple_dt_get_node(s8000->device_tree, "chosen");
    apple_dt_set_prop_u32(child, "chip-id", 0x8000);
    s8000->board_id = 1;    // Match with apple_aes.c
    apple_dt_set_prop_u32(child, "board-id", s8000->board_id);

    apple_dt_set_prop_u64(child, "unique-chip-id", s8000->ecid);

    // Update the display parameters
    apple_dt_set_prop_u32(child, "display-rotation", 0);
    apple_dt_set_prop_u32(child, "display-scale", 2);

    child = apple_dt_get_node(s8000->device_tree, "product");

    apple_dt_set_prop_u32(child, "oled-display", 1);
    apple_dt_set_prop_str(child, "graphics-featureset-class", "");
    apple_dt_set_prop_str(child, "graphics-featureset-fallbacks", "");
    apple_dt_set_prop_u32(child, "device-color-policy", 0);

    s8000_cpu_setup(s8000);
    s8000_create_aic(s8000);
    s8000_create_s3c_uart(s8000, serial_hd(0));
    s8000_pmgr_setup(s8000);
    s8000_create_dart(s8000, "dart-disp0", false);
    s8000_create_dart(s8000, "dart-apcie0", true);
    s8000_create_dart(s8000, "dart-apcie1", true);
    s8000_create_dart(s8000, "dart-apcie2", true);
    s8000_create_gpio(s8000, "gpio");
    s8000_create_gpio(s8000, "aop-gpio");
    s8000_create_i2c(s8000, "i2c0");
    s8000_create_i2c(s8000, "i2c1");
    s8000_create_i2c(s8000, "i2c2");
    s8000_create_usb(s8000);
    s8000_create_wdt(s8000);
    s8000_create_aes(s8000);
    // s8000_create_sio(s8000);
    s8000_create_spi0(s8000);
    s8000_create_spi(s8000, 1);
    s8000_create_spi(s8000, 2);
    s8000_create_spi(s8000, 3);
    s8000_create_sep(s8000);
    s8000_create_pmu(s8000);
    s8000_create_pcie(s8000);
    s8000_create_nvme(s8000);
    s8000_create_chestnut(s8000);
    s8000_display_create(s8000);
    s8000_create_backlight(s8000);

    s8000->init_done_notifier.notify = s8000_init_done;
    qemu_add_machine_init_done_notifier(&s8000->init_done_notifier);
}

static ram_addr_t s8000_fixup_ram_size(ram_addr_t size)
{
    ram_addr_t ret = ROUND_UP_16K(size);
    if (ret != DRAM_SIZE) { error_setg(&error_fatal, "Specified RAM size must be 2 GiB"); }
    return ret;
}

static void s8000_set_boot_mode(Object* obj, const char* value, Error** errp)
{
    AppleS8000MachineState* s8000;

    s8000 = APPLE_S8000(obj);
    if (g_str_equal(value, "auto")) { s8000->boot_info.boot_mode = kAppleBootModeAuto; }
    else if (g_str_equal(value, "enter_recovery")) {
        s8000->boot_info.boot_mode = kAppleBootModeEnterRecovery;
    }
    else if (g_str_equal(value, "exit_recovery")) {
        s8000->boot_info.boot_mode = kAppleBootModeExitRecovery;
    }
    else {
        s8000->boot_info.boot_mode = kAppleBootModeAuto;
        error_setg(errp, "Invalid boot mode: %s", value);
    }
}

static char* s8000_get_boot_mode(Object* obj, Error** errp)
{
    AppleS8000MachineState* s8000;

    s8000 = APPLE_S8000(obj);
    switch (s8000->boot_info.boot_mode) {
        case kAppleBootModeEnterRecovery: return g_strdup("enter_recovery");
        case kAppleBootModeExitRecovery : return g_strdup("exit_recovery");
        case kAppleBootModeAuto         : return g_strdup("auto");
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
PROP_STR_GETTER_SETTER(usb_uplink_addr);
PROP_GETTER_SETTER(int, usb_uplink_type);

static void s8000_class_init(ObjectClass* klass, const void* data)
{
    MachineClass*   mc;
    ObjectProperty* oprop;

    mc                    = MACHINE_CLASS(klass);
    mc->desc              = "Apple S8000 SoC (iPhone 6s Plus)";
    mc->init              = s8000_init;
    mc->reset             = s8000_reset;
    mc->max_cpus          = A9_MAX_CPU;
    mc->default_cpu_type  = TYPE_APPLE_A9;
    mc->minimum_page_bits = 14;
    mc->default_ram_size  = DRAM_SIZE;
    mc->fixup_ram_size    = s8000_fixup_ram_size;

    object_class_property_add_str(klass, "trustcache", s8000_get_trustcache_filename, s8000_set_trustcache_filename);
    object_class_property_set_description(klass, "trustcache", "TrustCache");
    object_class_property_add_str(klass, "ticket", s8000_get_ticket_filename, s8000_set_ticket_filename);
    object_class_property_set_description(klass, "ticket", "AP Ticket");
    object_class_property_add_str(klass, "sep-rom", s8000_get_sep_rom_filename, s8000_set_sep_rom_filename);
    object_class_property_set_description(klass, "sep-rom", "SEP ROM");
    object_class_property_add_str(klass, "sep-fw", s8000_get_sep_fw_filename, s8000_set_sep_fw_filename);
    object_class_property_set_description(klass, "sep-fw", "SEP Firmware");
    object_class_property_add_str(klass, "securerom", s8000_get_securerom_filename, s8000_set_securerom_filename);
    object_class_property_set_description(klass, "securerom", "SecureROM");
    object_class_property_add_str(klass, "boot-mode", s8000_get_boot_mode, s8000_set_boot_mode);
    object_class_property_set_description(klass, "boot-mode", "Boot Mode");
    object_class_property_add_bool(klass, "kaslr-off", s8000_get_kaslr_off, s8000_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    oprop = object_class_property_add(klass, "ecid", "uint64", s8000_get_ecid, s8000_set_ecid, NULL, NULL);
    object_property_set_default_uint(oprop, 0x1122334455667788);
    object_class_property_set_description(klass, "ecid", "Device ECID");
    object_class_property_add_bool(klass, "force-dfu", s8000_get_force_dfu, s8000_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
    object_class_property_add_str(klass, "usb-uplink-addr", s8000_get_usb_uplink_addr, s8000_set_usb_uplink_addr);
    object_class_property_set_description(klass, "usb-uplink-addr", "USB Uplink Address");
    oprop = object_class_property_add_enum(klass, "usb-uplink-type", "USBUplinkType", &USBUplinkType_lookup,
                                           s8000_get_usb_uplink_type, s8000_set_usb_uplink_type);
    object_property_set_default_str(oprop, qapi_enum_lookup(&USBUplinkType_lookup, USB_UPLINK_TYPE_VIRTUALHERE));
    object_class_property_set_description(klass, "usb-uplink-type", "USB Uplink Type");
}

static const TypeInfo s8000_info = {
    .name   = TYPE_APPLE_S8000,
    .parent = TYPE_MACHINE,
    OBJECT_TYPE_INSTANCE(AppleS8000MachineState),
    .class_size = sizeof(AppleS8000MachineClass),
    .class_init = s8000_class_init,
};

static void s8000_types(void) { type_register_static(&s8000_info); }

type_init(s8000_types)
