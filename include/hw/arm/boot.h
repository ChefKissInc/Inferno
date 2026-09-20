/*
 * Apple OS Boot Logic.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#pragma once

// #define ENABLE_BASEBAND
#define ENABLE_DATA_ENCRYPTION

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"
#include "hw/arm/dt.h"

#define LC_SYMTAB              (0x2)
#define LC_UNIXTHREAD          (0x5)
#define LC_DYSYMTAB            (0xB)
#define LC_SEGMENT_64          (0x19)
#define LC_SOURCE_VERSION      (0x2A)
#define LC_BUILD_VERSION       (0x32)
#define LC_REQ_DYLD            (0x80000000)
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)
#define LC_FILESET_ENTRY       (0x35 | LC_REQ_DYLD)

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t sym_off;
    uint32_t nsyms;
    uint32_t str_off;
    uint32_t str_size;
} MachoSymtabCommand;

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t local_sym_i;
    uint32_t local_sym_n;
    uint32_t ext_def_sym_i;
    uint32_t ext_def_sym_n;
    uint32_t undef_sym_i;
    uint32_t undef_sym_n;
    uint32_t toc_off;
    uint32_t toc_n;
    uint32_t mod_tab_off;
    uint32_t mod_tab_n;
    uint32_t ext_ref_sym_off;
    uint32_t ext_ref_syms_n;
    uint32_t indirect_sym_off;
    uint32_t indirect_syms_n;
    uint32_t ext_rel_off;
    uint32_t ext_rel_n;
    uint32_t loc_rel_off;
    uint32_t loc_rel_n;
} MachoDysymtabCommand;

#define VM_PROT_READ    (0x01)
#define VM_PROT_WRITE   (0x02)
#define VM_PROT_EXECUTE (0x04)

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} MachoSegmentCommand64;

typedef struct
{
    char     sect_name[16];
    char     seg_name[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t rel_off;
    uint32_t n_reloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} MachoSection64;

#define SECTION_TYPE               (0x000000FF)
#define S_NON_LAZY_SYMBOL_POINTERS (0x6)

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    uint64_t vm_addr;
    uint64_t file_off;
    uint32_t entry_id;
    uint32_t reserved;
} MachoFilesetEntryCommand;

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    uint64_t version;
} MachoSourceVersionCommand;

#define PLATFORM_UNKNOWN              (0)
#define PLATFORM_MACOS                (1)
#define PLATFORM_IOS                  (2)
#define PLATFORM_TVOS                 (3)
#define PLATFORM_WATCHOS              (4)
#define PLATFORM_BRIDGEOS             (5)
#define PLATFORM_MAC_CATALYST         (6)
#define PLATFORM_IOS_SIMULATOR        (7)
#define PLATFORM_TVOS_SIMULATOR       (8)
#define PLATFORM_WATCHOS_SIMULATOR    (9)
#define PLATFORM_DRIVERKIT            (10)
#define PLATFORM_VISIONOS             (11)
#define PLATFORM_VISIONOS_SIMULATOR   (12)
#define PLATFORM_FIRMWARE             (13)
#define PLATFORM_SEPOS                (14)
#define PLATFORM_MACOS_EXCLAVECORE    (15)
#define PLATFORM_MACOS_EXCLAVEKIT     (16)
#define PLATFORM_IOS_EXCLAVECORE      (17)
#define PLATFORM_IOS_EXCLAVEKIT       (18)
#define PLATFORM_TVOS_EXCLAVECORE     (19)
#define PLATFORM_TVOS_EXCLAVEKIT      (20)
#define PLATFORM_WATCHOS_EXCLAVECORE  (21)
#define PLATFORM_WATCHOS_EXCLAVEKIT   (22)
#define PLATFORM_VISIONOS_EXCLAVECORE (23)
#define PLATFORM_VISIONOS_EXCLAVEKIT  (24)

#define BUILD_VERSION_MAJOR(_v) (((_v) & 0xFFFF0000) >> 16)
#define BUILD_VERSION_MINOR(_v) (((_v) & 0xFF00) >> 8)
#define BUILD_VERSION_PATCH(_v) ((_v) & 0xFF)

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t platform;
    uint32_t min_os;
    uint32_t sdk;
    uint32_t n_tools;
} MachoBuildVersionCommand;

#define MACH_MAGIC_64 (0xFEEDFACFu)
#define MH_EXECUTE    (0x2)
#define MH_FILESET    (0xC)

typedef struct
{
    uint32_t                   magic;
    uint32_t /*cpu_type_t*/    cpu_type;
    uint32_t /*cpu_subtype_t*/ cpu_subtype;
    uint32_t                   file_type;
    uint32_t                   n_cmds;
    uint32_t                   size_of_cmds;
    uint32_t                   flags;
    uint32_t                   reserved;
} MachoHeader64;

typedef struct
{
    uint32_t cmd;
    uint32_t cmd_size;
} MachoLoadCommand;

typedef struct
{
    union
    { uint32_t n_strx; } n_un;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} MachoNList64;

#define N_STAB (0xE0)
#define N_PEXT (0x10)
#define N_TYPE (0x0E)
#define N_EXT  (0x01)

typedef struct
{
    uint64_t base_addr;
    uint64_t display;
    uint64_t row_bytes;
    uint64_t width;
    uint64_t height;
    union
    {
        struct
        {
            uint8_t depth       : 8;
            uint8_t rotate      : 8;
            uint8_t scale       : 8;
            uint8_t boot_rotate : 8;
        };
        uint64_t raw;
    } depth;
} AppleVideoArgs;

typedef struct
{
    uint64_t version;
    uint64_t virt_base;
    uint64_t phys_base;
    uint64_t mem_size;
    uint64_t kern_args;
    uint64_t kern_entry;
    uint64_t kern_phys_base;
    uint64_t kern_phys_slide;
    uint64_t kern_virt_slide;
    uint64_t kern_text_section_off;
    uint8_t  random_bytes[0x10];
} AppleMonitorBootArgs;

#define BOOT_FLAGS_DARK_BOOT BIT(0)

typedef struct
{
    uint16_t       revision;
    uint16_t       version;
    uint64_t       virt_base;
    uint64_t       phys_base;
    uint64_t       mem_size;
    uint64_t       kernel_top;
    AppleVideoArgs video_args;
    uint32_t       machine_type;
    uint64_t       device_tree_ptr;
    uint32_t       device_tree_length;
    char           cmdline[0x260];
    uint64_t       boot_flags;
    uint64_t       mem_size_actual;
} AppleKernelBootArgsRev2;

typedef struct
{
    uint16_t       revision;
    uint16_t       version;
    uint64_t       virt_base;
    uint64_t       phys_base;
    uint64_t       mem_size;
    uint64_t       kernel_top;
    AppleVideoArgs video_args;
    uint32_t       machine_type;
    uint64_t       device_tree_ptr;
    uint32_t       device_tree_length;
    char           cmdline[0x400];
    uint64_t       boot_flags;
    uint64_t       mem_size_actual;
} AppleKernelBootArgsRev3;

#define EMBEDDED_PANIC_HEADER_FLAG_COREDUMP_COMPLETE             (0x01)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_SUCCEEDED           (0x02)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_DEBUGGERSYNC (0x04)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_ERROR        (0x08)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_INCOMPLETE   (0x10)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_NESTED       (0x20)
#define EMBEDDED_PANIC_HEADER_FLAG_NESTED_PANIC                  (0x40)
#define EMBEDDED_PANIC_HEADER_FLAG_BUTTON_RESET_PANIC            (0x80)
#define EMBEDDED_PANIC_HEADER_FLAG_COPROC_INITIATED_PANIC        (0x100)
#define EMBEDDED_PANIC_HEADER_FLAG_COREDUMP_FAILED               (0x200)
#define EMBEDDED_PANIC_HEADER_FLAG_COMPRESS_FAILED               (0x400)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_DATA_COMPRESSED     (0x800)

#define EMBEDDED_PANIC_HEADER_CURRENT_VERSION (2)
#define EMBEDDED_PANIC_MAGIC                  (0x46554E4B)
#define EMBEDDED_PANIC_HEADER_OSVERSION_LEN   (32)

typedef struct
{
    uint32_t magic;
    uint32_t crc;
    uint32_t version;
    uint64_t panic_flags;
    uint32_t panic_log_offset;
    uint32_t panic_log_len;
    uint32_t stackshot_offset;
    uint32_t stackshot_len;
    uint32_t other_log_offset;
    uint32_t other_log_len;
    union
    {
        struct
        {
            uint64_t x86_power_state    : 8;
            uint64_t x86_efi_boot_state : 8;
            uint64_t x86_system_state   : 8;
            uint64_t x86_unused_bits    : 40;
        };
        uint64_t x86_do_not_use;
    };
    char os_version[EMBEDDED_PANIC_HEADER_OSVERSION_LEN];
    char macos_version[EMBEDDED_PANIC_HEADER_OSVERSION_LEN];
} QEMU_PACKED AppleEmbeddedPanicHeader;

#define IOP_SEGMENT_RANGE_NEEDS_BACKUP BIT(2)

typedef struct
{
    uint64_t phys;
    uint64_t virt;
    uint64_t remap;
    uint32_t size;
    uint32_t flags;
} AppleIOPSegmentRange;

#define XNU_MAX_NVRAM_SIZE (0xFFFF * 0x10)
#define XNU_BNCH_SIZE      (32)

typedef enum
{
    kAppleBootModeAuto = 0,
    kAppleBootModeEnterRecovery,
    kAppleBootModeExitRecovery,
} AppleBootMode;

typedef struct
{
    vaddr         kern_entry;
    hwaddr        kern_text_off;
    vaddr         tz1_entry;
    hwaddr        device_tree_addr;
    uint64_t      device_tree_size;
    hwaddr        ramdisk_addr;
    uint64_t      ramdisk_size;
    hwaddr        trustcache_addr;
    uint64_t      trustcache_size;
    hwaddr        sep_fw_addr;
    uint64_t      sep_fw_size;
    hwaddr        tz0_addr;
    uint64_t      tz0_size;
    hwaddr        kern_boot_args_addr;
    uint64_t      kern_boot_args_size;
    hwaddr        top_of_kernel_data_pa;
    hwaddr        tz1_boot_args_pa;
    hwaddr        dram_base;
    uint64_t      dram_size;
    uint8_t       nvram_data[XNU_MAX_NVRAM_SIZE];
    uint32_t      nvram_size;
    gchar*        ticket_data;
    gsize         ticket_length;
    bool          non_cold_boot;
    bool          had_autoboot;
    AppleBootMode boot_mode;
} AppleBootInfo;

MachoHeader64* apple_boot_load_kernel(const char* filename, MachoHeader64** secure_monitor);

MachoHeader64* apple_boot_parse_macho(uint8_t* data, uint32_t len);

uint8_t* apple_boot_get_macho_buffer(MachoHeader64* header);

uint32_t apple_boot_build_version(MachoHeader64* header);

uint32_t apple_boot_platform(MachoHeader64* header);

const char* apple_boot_platform_string(MachoHeader64* header);

void apple_boot_get_kc_bounds(MachoHeader64* header, vaddr* text_base, vaddr* kc_base, vaddr* kc_end, vaddr* ro_lower,
                              vaddr* ro_upper);

MachoFilesetEntryCommand* apple_boot_get_fileset(MachoHeader64* header, const char* entry);

MachoHeader64* apple_boot_get_fileset_header(MachoHeader64* header, const char* entry);

MachoSegmentCommand64* apple_boot_get_segment(MachoHeader64* header, const char* name);

MachoSection64* apple_boot_get_section(MachoSegmentCommand64* segment, const char* name);

/// Modify a XNU virtual address to be fixed up and slide-adjusted.
vaddr apple_boot_fixup_slide_va(vaddr va);

/// Convert a XNU virtual address to a host pointer.
void* apple_boot_va_to_ptr(vaddr va);

bool apple_boot_contains_boot_arg(const char* boot_args, const char* arg, bool match_prefix);

void apple_boot_setup_monitor_boot_args(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                        hwaddr mem_size, hwaddr kern_args, vaddr kern_entry, hwaddr kern_phys_base,
                                        hwaddr kern_phys_slide, vaddr kern_virt_slide, vaddr kern_text_section_off);
void apple_boot_setup_bootargs(uint32_t build_version, AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                               hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                               AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual);

void apple_boot_allocate_segment_records(AppleDTNode* memory_map, MachoHeader64* header);

vaddr apple_boot_load_macho(MachoHeader64* header, AddressSpace* as, AppleDTNode* memory_map, hwaddr phys_base,
                            vaddr virt_slide);

void apple_boot_load_raw_file(const char* filename, AddressSpace* as, hwaddr file_pa, uint64_t* size);

AppleDTNode* apple_boot_load_dt_file(const char* filename);

void apple_boot_populate_dt(AppleDTNode* root, AppleBootInfo* info, bool auto_boot);

void apple_boot_finalise_dt(AppleDTNode* root, AddressSpace* as, AppleBootInfo* info);

uint8_t* apple_boot_load_trustcache_file(const char* filename, uint64_t* size);

void apple_boot_load_ramdisk(const char* filename, AddressSpace* as, hwaddr pa, uint64_t* size);

void apple_boot_extract_manifest(const uint8_t* img4_data, uint64_t img4_data_len_max, gchar** out_data,
                                 gsize* out_size);
