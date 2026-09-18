/*
 * Apple SEP Debug Trace.
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

#include "hw/arm/a13.h"
#include "hw/arm/a9.h"
#include "hw/arm/sep/private.h"

#ifdef SEP_ENABLE_TRACE_BUFFER
    #include "system/address-spaces.h"
    #include "qemu/osdep.h"
    #include "qemu/log.h"
    #include "hw/arm/sep/core.h"

    #define DEBUG_TRACE_SIZE (0x10000)

struct AppleSEPDebugTraceState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    MemoryRegion mr;
    hwaddr       size;
    hwaddr       offset;
    uint8_t      regs[DEBUG_TRACE_SIZE];
};

void apple_sep_debug_trace_enable(AppleSEPDebugTraceState* s)
{
    DPRINTF("SEP_PROGRESS: Enable Trace Buffer: s->shmbuf_base: "
            "0x" HWADDR_FMT_plx "\n",
            s->shmbuf_base);

    if (!s->sep->shmbuf_base) { return; }

    AddressSpace* nsas = &address_space_memory;
    typedef struct
    {
        uint32_t name;
        uint32_t size;
        uint64_t offset;
    } QEMU_PACKED shm_region_t;
    #ifdef SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS
    shm_region_t shm_region_TRAC = {0};
    assert_cmpuint(sizeof(shm_region_TRAC), ==, 0x10);
    shm_region_TRAC.name         = 'TRAC';
    shm_region_TRAC.size         = s->size;
    shm_region_TRAC.offset       = s->offset;
    shm_region_t shm_region_null = {0};
    assert_cmpuint(sizeof(shm_region_null), ==, 0x10);
    shm_region_null.name      = 'null';
    uint32_t region_SCOT_size = 0x4000;
    address_space_write(nsas, s->sep->shmbuf_base + 0x14, MEMTXATTRS_UNSPECIFIED, &region_SCOT_size,
                        sizeof(region_SCOT_size));
    address_space_write(nsas, s->sep->shmbuf_base + 0x20, MEMTXATTRS_UNSPECIFIED, &shm_region_TRAC,
                        sizeof(shm_region_TRAC));
    address_space_write(nsas, s->sep->shmbuf_base + 0x30, MEMTXATTRS_UNSPECIFIED, &shm_region_null,
                        sizeof(shm_region_null));
    address_space_set(nsas, s->sep->shmbuf_base + 0xC000 + 0x20, 0, region_SCOT_size - 0x20,
                      MEMTXATTRS_UNSPECIFIED);    // clean up SCOT a bit
    #endif
    typedef struct
    {
        uint64_t name;
        uint64_t size;                  // aligned
        uint8_t  access_permissions;    // 0x04/0x06/0x16 // (arg5 & 1) != 0
                                        // create_object panic? ;; maybe permissions
        uint8_t  arg6;                  // 0x00/0x02/0x06 // >= 0x03 create_object panic?
        uint8_t  arg7;                  // 0x01/0x02/0x03/0x04/0x05/0x0D/0x0E/0x0F/0x10 // if
                                        // (arg7 != 0) create_object data_346d0 checking block ;;
                                        // maybe module_index
        uint8_t  pad0;
        uint32_t some_id;    // maybe segment name like _dat, _asc, STAK, TEXT,
                             // PMGR or _hep.
        uint64_t phys;
        uint32_t phys_module_name;         // phys module name like EISP
        uint32_t phys_region_name;         // phys region name like BASE
        uint64_t virt_mapping_next;        // sepos_virt_mapping_t
        uint64_t virt_mapping_previous;    // sepos_virt_mapping_t.next or
                                           // object_mappings_ios14_t.virt_mapping_next
        uint64_t acl_next;                 // sepos_acl_t
        uint64_t acl_previous;             // sepos_acl_t.next or
                                           // object_mappings_ios14_t.acl_next
    } QEMU_PACKED object_mappings_ios14_t;
    typedef struct
    {
        uint64_t name;
        uint64_t size;                  // aligned
        uint8_t  access_permissions;    // 0x04/0x06/0x16 // (arg5 & 1) != 0
                                        // create_object panic? ;; maybe permissions
        uint8_t  arg6;                  // 0x00/0x02/0x06 // >= 0x03 create_object panic?
        uint8_t  arg7;                  // 0x01/0x02/0x03/0x04/0x05/0x0D/0x0E/0x0F/0x10 // if
                                        // (arg7 != 0) create_object data_346d0 checking block ;;
                                        // maybe module_index
        uint8_t  pad0;
        uint32_t some_id;    // maybe segment name like _dat, _asc, STAK, TEXT, PMGR
                             // or _hep.
        uint64_t phys;
        uint32_t phys_module_name;         // phys module name like EISP
        uint32_t phys_region_name;         // phys region name like BASE
        uint64_t virt_mapping_next;        // sepos_virt_mapping_t
        uint64_t virt_mapping_previous;    // sepos_virt_mapping_t.next or
                                           // object_mappings_ios16_t.virt_mapping_next
        uint64_t acl_next;                 // sepos_acl_t
        uint64_t acl_previous;             // sepos_acl_t.next or object_mappings_ios16_t.acl_next
        uint64_t base_cap;                 // some offset, can be positive or negative. 0xf<<32 only set if negative?
        uint64_t some_addr0;               // some offset, can be positive or negative. 0xf<<32 only set if negative?
        uint64_t some_addr1;    // some aligned offset, could be related to size. can be 0x0, can be the phys in case of
                                // shm buffers
        uint64_t virt_mapping_next_is_nonzero;    // actually a boolean, can be 0x0/0x1, mostly 0x1.
    } QEMU_PACKED object_mappings_ios16_t;
    typedef struct
    {
        uint32_t maybe_module_id;    // 0x2/0x3/0x4/10001
        uint32_t acl;                // 0x4/0x6/0x14/0x16
        uint64_t next;               // sepos_acl_t
        uint64_t previous;           // sepos_acl_t.next
    } QEMU_PACKED sepos_acl_t;
    #if 0
    typedef struct {
        uint64_t object_mapping; // object_mappings_t
        uint64_t maybe_virt_base;
        uint8_t sending_pid;
        uint8_t maybe_permissions; // maybe permissions ;; data0
        uint8_t maybe_subregion; // 0x00/0x01/0x02 ;; data1
        uint8_t pad0;
        uint32_t pad1;
        uint64_t module_next; // sepos_virt_mapping_t
        uint64_t module_previous; // sepos_virt_mapping_t.next
        uint64_t all_next; // sepos_virt_mapping_t
        uint64_t all_previous; // sepos_virt_mapping_t.all_next
    } QEMU_PACKED sepos_virt_mapping_t;
    #endif
    // object_mappings_ios14_t object_mapping_THDR_IOS15 = { 0 };
    // assert_cmpuint(sizeof(object_mapping_THDR_IOS15), ==, 0x48);
    object_mappings_ios14_t object_mapping_TRAC_IOS14 = {0};
    assert_cmpuint(sizeof(object_mapping_TRAC_IOS14), ==, 0x48);

    // object_mappings_ios16_t object_mapping_THDR_IOS16 = { 0 };
    // assert_cmpuint(sizeof(object_mapping_THDR_IOS16), ==, 0x68);
    // object_mappings_ios16_t object_mapping_TRAC_IOS16 = { 0 };
    // assert_cmpuint(sizeof(object_mapping_TRAC_IOS16), ==, 0x68);
    sepos_acl_t acl_for_TRAC = {0};
    assert_cmpuint(sizeof(acl_for_TRAC), ==, 0x18);
    // sepos_virt_mapping_t virt_mapping_for_TRAC = { 0 };
    // assert_cmpuint(sizeof(virt_mapping_for_TRAC), ==, 0x38);

    // SEPOS_PHYS_BASEs: not in runtime, but while in SEPROM. Same on T8020
    // (0x340611BA8-0x11BA8)
    // get this with gdb, prerequisite is disabling aslr(?):
    // b *0x<sepos_module_start_function> ; gva2gpa 0x<sepos_module_start_function>
    // the result minus <sepos_module_start_function from binja without rebasing>
    // &~0x100000000 (only if the upper bits in the sepos address are set?)
    // xp/1xw phys_base + 0x8000 should be 0xfeedfacf
    // maybe it's not that easy to disable the SEPOS module ASLR under iOS 15:
    // so instead make breakpoints for the second (or both) eret and do "si".
    // disabling the SEPOS module's ASLR was easy under iOS 16.
    #define SEPOS_PHYS_BASE_T8015       (0x3404A4000ULL)
    #define SEPOS_PHYS_BASE_T8020_IOS14 (0x340600000ULL)
    #define SEPOS_PHYS_BASE_T8020_IOS15 (0x340710000ULL)
    // #define SEPOS_PHYS_BASE_T8030_IOS14 (0x340634000ULL) // for 14.7.1
    #define SEPOS_PHYS_BASE_T8030_IOS14 (0x340628000ULL)    // for 14beta5
    #define SEPOS_PHYS_BASE_T8030_IOS15 (0x34075C000ULL)
    #define SEPOS_PHYS_BASE_T8030_IOS16 (0x340440000ULL)
    #define SEPOS_PHYS_BASE_T8030_IOS18 (0x3403A0000ULL)
    // for T8020/T8030 SEPFW of early 14 and 14.7.1
    #define SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS14 (0x198D0)
    #define SEPOS_OBJECT_MAPPING_INDEX              (7)
    #define SEPOS_OBJECT_MAPPING_INDEX_THDR         (SEPOS_OBJECT_MAPPING_INDEX - 1)
    // for T8020/T8030 SEPFW of early 14 and 14.7.1
    #define SEPOS_ACL_BASE_VERSION_IOS14 (0x140D0)
    #define SEPOS_ACL_INDEX              (19)

    uint64_t sepos_phys_base           = 0x0;
    uint64_t sepos_object_mapping_base = 0x0;
    uint64_t sepos_acl_base            = 0x0;
    if (s->sep->chip_id == 0x8015) { sepos_phys_base = SEPOS_PHYS_BASE_T8015; }
    else if (s->sep->chip_id == 0x8020) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        sepos_phys_base = SEPOS_PHYS_BASE_T8020_IOS14;
    #elif SEP_USE_VERSION_OVERRIDE == 15
        sepos_phys_base = SEPOS_PHYS_BASE_T8020_IOS15;
    #elif SEP_USE_VERSION_OVERRIDE == 16
        assert_not_reached();
    #elif SEP_USE_VERSION_OVERRIDE == 18
        assert_not_reached();
    #endif
    }
    else if (s->sep->chip_id == 0x8030) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        sepos_phys_base = SEPOS_PHYS_BASE_T8030_IOS14;
    #elif SEP_USE_VERSION_OVERRIDE == 15
        sepos_phys_base = SEPOS_PHYS_BASE_T8030_IOS15;
    #elif SEP_USE_VERSION_OVERRIDE == 16
        sepos_phys_base = SEPOS_PHYS_BASE_T8030_IOS16;
    #elif SEP_USE_VERSION_OVERRIDE == 18
        sepos_phys_base = SEPOS_PHYS_BASE_T8030_IOS18;
    #endif
    }
    else {
        assert_not_reached();
    }

    // alternative bypass as if_module_AAES_Debu_or_SEPD is also used by other
    // functions, more restrictive.
    uint32_t value32_nop   = 0xD503201F;    // nop
    uint64_t bypass_offset = 0;
    if (s->sep->chip_id == 0x8020) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        bypass_offset = 0x11BB0;    // T8020 iOS14
    #elif SEP_USE_VERSION_OVERRIDE == 15
        bypass_offset = 0x12FB4;    // T8020 iOS15
    #endif
    }
    else if (s->sep->chip_id == 0x8030) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        // bypass_offset = 0x11B34; // T8030 iOS14.7.1
        bypass_offset = 0x11C38;    // T8030 iOS14beta5
    #elif SEP_USE_VERSION_OVERRIDE == 15
        bypass_offset = 0x12E9C;    // T8030 iOS15
    #elif SEP_USE_VERSION_OVERRIDE == 16
        bypass_offset = 0x1A074;    // T8030 iOS16
    #elif SEP_USE_VERSION_OVERRIDE == 17
        bypass_offset = 0x14c44;    // T8030 iOS17
    #elif SEP_USE_VERSION_OVERRIDE == 18
        bypass_offset = 0x14db4;    // T8030 iOS18
    #endif
    }
    else if (s->sep->chip_id == 0x8015) {
        // T8015's SEPFW SEPOS is not reachable from SEPROM, it's LZVN
        // compressed.
        bypass_offset = 0x11C2C;    // T8015
    }
    address_space_write(nsas, sepos_phys_base + bypass_offset, MEMTXATTRS_UNSPECIFIED, &value32_nop,
                        sizeof(value32_nop));

    #if SEP_USE_VERSION_OVERRIDE == 14
    sepos_object_mapping_base = SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS14;
    sepos_acl_base            = SEPOS_ACL_BASE_VERSION_IOS14;
    #else
    return;
    #endif

    #if SEP_USE_VERSION_OVERRIDE == 14
    // THDR is sepfw >= 15
    // if 14: TRAC: 0x06/0x00
    // if 15: THDR: 0x06/0x01 TRAC: 0x06/0x02

    object_mapping_TRAC_IOS14.name               = 'TRAC';
    object_mapping_TRAC_IOS14.size               = s->size;
    object_mapping_TRAC_IOS14.access_permissions = 0x06;
        #if SEP_USE_VERSION_OVERRIDE == 14
    object_mapping_TRAC_IOS14.arg6 = 0x00;
        #else    // == 15
    object_mapping_TRAC_IOS14.arg6 = 0x02;
        #endif
    object_mapping_TRAC_IOS14.arg7    = 0x01;
    object_mapping_TRAC_IOS14.some_id = '_dat';
    object_mapping_TRAC_IOS14.phys    = s->sep->shmbuf_base + s->offset;
    // object_mapping_TRAC_IOS14.virt_mapping_next = SEPOS_VIRT_MAPPING_BASE_IOS14 +
    // (sizeof(sepos_virt_mapping_t) * SEPOS_VIRT_MAPPING_INDEX);
    // object_mapping_TRAC_IOS14.virt_mapping_previous = SEPOS_VIRT_MAPPING_BASE_IOS14 +
    // (sizeof(sepos_virt_mapping_t) * SEPOS_VIRT_MAPPING_INDEX) +
    // offsetof(sepos_virt_mapping_t, module_next);
    // virt_mapping_previous really needs to be set!
    object_mapping_TRAC_IOS14.virt_mapping_previous = sepos_object_mapping_base
                                                      + (sizeof(object_mapping_TRAC_IOS14) * SEPOS_OBJECT_MAPPING_INDEX)
                                                      + offsetof(object_mappings_ios14_t, virt_mapping_next);
    object_mapping_TRAC_IOS14.acl_next              = sepos_acl_base + (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX);
    object_mapping_TRAC_IOS14.acl_previous =
        sepos_acl_base + (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX) + offsetof(sepos_acl_t, next);
    address_space_write(nsas,
                        sepos_phys_base + sepos_object_mapping_base
                            + (sizeof(object_mapping_TRAC_IOS14) * SEPOS_OBJECT_MAPPING_INDEX),
                        MEMTXATTRS_UNSPECIFIED, &object_mapping_TRAC_IOS14, sizeof(object_mapping_TRAC_IOS14));
    acl_for_TRAC.maybe_module_id = 10001;
    ////acl_for_TRAC.maybe_module_id = 55; // non-existent
    acl_for_TRAC.acl      = 0x6;
    acl_for_TRAC.previous = sepos_object_mapping_base + (sizeof(object_mapping_TRAC_IOS14) * SEPOS_OBJECT_MAPPING_INDEX)
                            + offsetof(object_mappings_ios14_t, acl_next);
    address_space_write(nsas, sepos_phys_base + sepos_acl_base + (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX),
                        MEMTXATTRS_UNSPECIFIED, &acl_for_TRAC, sizeof(acl_for_TRAC));
    #endif
}

void apple_sep_debug_trace_set_region(AppleSEPDebugTraceState* s, hwaddr offset, hwaddr size)
{
    s->offset = offset;
    s->size   = size;
}

static const char* sepos_return_module_thread_string_t8015(uint64_t module_thread_id)
{
    // base == sepdump02_SEPOS?
    // T8015 thread name/info base 0xFFFFFFE00001A988

    switch (module_thread_id) {
        case 0x0    : return "SEPOS";    // SEPOS/BOOT, actually BOOT
        case 0x10000: return "SEPD";
        case 0x10001: return "intr";
        case 0x10002: return "XPRT";
        case 0x10003: return "PMGR";
        case 0x10004: return "AKF";
        case 0x10005: return "EP0D";
        case 0x10006: return "TRNG";
        case 0x10007: return "KEY";
        case 0x10008: return "shnd";
        case 0x10009: return "ep0";
        case 0x20000: return "DAES";
        case 0x20001: return "AESS";
        case 0x20002: return "AEST";
        case 0x20003: return "PKA";
        case 0x30000: return "dxio";
        case 0x30001: return "GPIO";
        case 0x30002: return "I2C";
        case 0x40000: return "enti";
        case 0x50000: return "sskg";
        case 0x50001: return "skgs";
        case 0x50002: return "crow";
        case 0x50003: return "cro2";
        case 0x60000: return "sars";
        case 0x70000: return "ARTM";
        case 0x80000: return "xART";
        case 0x90000: return "scrd";
        case 0xA0000: return "pass";
        case 0xB0000: return "sks";    // 13
        case 0xB0001: return "sksa";
        case 0xC0000: return "sbio";           // 14
        case 0xC0001: return "SBIO_THREAD";    // thread name missing from array
        case 0xD0000: return "sse";            // 15
        default     : return "Unknown";
    }
}

static const char* sepos_return_module_thread_string_t8030(uint64_t module_thread_id)
{
    // base == sepdump02_SEPOS?
    // T8020/T8030 thread name/info base 0xFFFFFFE00001B1C8

    switch (module_thread_id) {
        case 0x0    : return "BOOT";    // SEPOS
        case 0x10000: return "SEPD";
        case 0x10001: return "intr";
        case 0x10002: return "XPRT";
        case 0x10003: return "PMGR";
        case 0x10004: return "AKF";
        case 0x10005: return "EP0D";
        case 0x10006: return "TRNG";
        case 0x10007: return "KEY";
        case 0x10008: return "MONI";
        case 0x10009: return "AESH";
        case 0x1000A: return "EISP";
        case 0x1000B: return "shnd";
        case 0x1000C: return "ep0";
        case 0x20000: return "DAES";
        case 0x20001: return "AESS";
        case 0x20002: return "AEST";
        case 0x20003: return "PKA";
        case 0x30000: return "dxio";
        case 0x30001: return "GPIO";
        case 0x30002: return "I2C";
        case 0x40000: return "enti";
        case 0x50000: return "sskg";
        case 0x50001: return "skgs";
        case 0x50002: return "crow";
        case 0x50003: return "cro2";
        case 0x60000: return "sars";
        case 0x70000: return "ARTM";
        case 0x80000: return "xART";
        case 0x90000: return "eiAp";
        case 0x90001: return "EISP";
        case 0x90002: return "HWRS";
        case 0x90003: return "FDCN";
        case 0x90004: return "SDCN";
        case 0x90005: return "FIPP";
        case 0x90006: return "FPCE";
        case 0x90007: return "FPPD";
        case 0x90008: return "FDMA";
        case 0x90009: return "SHAV";
        case 0x9000A: return "PROX";
        case 0xA0000: return "scrd";
        case 0xB0000: return "pass";
        case 0xC0000: return "sks";
        case 0xC0001: return "sksa";
        case 0xD0000: return "hdcp";
        case 0xE0000: return "sprl";
        case 0xF0000: return "sse";
        default     : return "Unknown";
    }
}

static const char* sepos_return_module_thread_string_t8030_15(uint64_t module_thread_id)
{
    // base == sepdump02_SEPOS?
    // T8030 sepfw 15 thread name/info base 0xFFFFFFE0000235C8

    switch (module_thread_id) {
        case 0x0       : return "BOOT";    // SEPOS
        case 0x10000   : return "SEPD";
        case 0x10001   : return "intr";
        case 0x10002   : return "Cons";
        case 0x10003   : return "XPRT";
        case 0x10004   : return "PMGR";
        case 0x10005   : return "AKF ";
        case 0x10006   : return "EP0D";
        case 0x10007   : return "EPCD";
        case 0x10008   : return "TRNG";
        case 0x10009   : return "KEY ";
        case 0x1000A   : return "MONI";
        case 0x1000B   : return "AESH";
        case 0x1000C   : return "EISP";
        case 0x1000D   : return "cnin";
        case 0x1000E   : return "shnd";
        case 0x1000F   : return "ep0 ";
        case 0x10010   : return "ep1 ";
        case 0x20000   : return "DAES";
        case 0x20001   : return "AESS";
        case 0x20002   : return "AEST";
        case 0x20003   : return "PKA ";
        case 0x30000   : return "dxio";
        case 0x30001   : return "GPIO";
        case 0x30002   : return "I2C ";
        case 0x40000   : return "enti";
        case 0x50000   : return "sskg";
        case 0x50001   : return "skgs";
        case 0x50002   : return "crow";
        case 0x50003   : return "cro2";
        case 0x60000   : return "sars";
        case 0x70000   : return "ARTM";
        case 0x80000   : return "xART";
        case 0x90000   : return "eiAp";
        case 0x90001   : return "EISP";
        case 0x90002   : return "HWRS";
        case 0x90003   : return "FDCN";
        case 0x90004   : return "SDCN";
        case 0x90005   : return "FIPP";
        case 0x90006   : return "FPCE";
        case 0x90007   : return "FPPD";
        case 0x90008   : return "FDMA";
        case 0x90009   : return "SHAV";
        case 0x9000A   : return "PROX";
        case 0xA0000   : return "scrd";
        case 0xB0000   : return "pass";
        case 0xC0000   : return "sks ";
        case 0xC0001   : return "sksa";
        case 0xD0000   : return "hdcp";
        case 0xE0000   : return "pair";
        case 0xF0000   : return "sprl";
        case 0x100000  : return "sse";
        case 0x110000  : return "sidv";
        case 0x120000  : return "unit";
        case 0x1D1E1D1E: return "IDLE";
        default        : return "Unknown";
    }
}

static const char* sepos_return_module_thread_string(uint32_t chip_id, uint64_t module_thread_id)
{
    if (chip_id == 0x8015) { return sepos_return_module_thread_string_t8015(module_thread_id); }
    else if (chip_id == 0x8030) {
    #if SEP_USE_VERSION_OVERRIDE == 15
        return sepos_return_module_thread_string_t8030_15(module_thread_id);
    #else
        return sepos_return_module_thread_string_t8030(module_thread_id);
    #endif
    }
    assert_not_reached();
}

static void debug_trace_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPDebugTraceState* s      = opaque;
    uint32_t                 offset = 0;
    if (size == 1) {
        // iOS 15 SEPFW workaround against a brief logspam
        return;
    }

    #ifdef ENABLE_CPU_DUMP_STATE
        // cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
    #endif

    if (s->sep->shmbuf_base == 0) {
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: SHMBUF_BASE==NULL: Unknown write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64
                      " size=%u\n",
                      addr, data, size);
        return;
    }

    if (addr + size > ARRAY_SIZE(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "DEBUG_TRACE: Unknown write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64 " size=%u\n",
                      addr, data, size);
        return;
    }

    offset = ((uint32_t*)s->regs)[0x4 / 4];
    if (offset != 0) {
        offset  -= 1;
        offset <<= 6;
    }

    memcpy(&s->regs[addr], &data, size);

    uint32_t addr_mod = addr % 0x40;
    if (addr != 0x40 &&    // offset register
        addr != 0x04 &&    // some index
        addr_mod != 0x20 && addr_mod != 0x28 && addr_mod != 0x00 && addr_mod != 0x08 && addr_mod != 0x10
        && addr_mod != 0x18 && addr_mod != 0x30)
    {
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Unknown write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64
                      " size=%u offset==0x%08x\n",
                      addr, data, size, offset);
    }

    // Might not include SEPOS output, as it's not initialized like e.g.
    // SEPD.
    if (addr_mod != 0x30) { return; }

    SEPMessage m        = {0};
    uint64_t   trace_id = *(uint64_t*)&s->regs[addr - 0x30];
    uint64_t   arg2     = *(uint64_t*)&s->regs[addr - 0x28];
    uint64_t   arg3     = *(uint64_t*)&s->regs[addr - 0x20];
    uint64_t   arg4     = *(uint64_t*)&s->regs[addr - 0x18];
    uint64_t   arg5     = *(uint64_t*)&s->regs[addr - 0x10];
    uint64_t   tid      = *(uint64_t*)&s->regs[addr - 0x08];
    uint64_t   time     = *(uint64_t*)&s->regs[addr - 0x00];
    DPRINTF("\nDEBUG_TRACE: Debug:"
            " 0x%" PRIX64 " 0x%" PRIX64 " 0x%" PRIX64 " 0x%" PRIX64 " 0x%" PRIX64 " 0x%" PRIX64 " %" PRIu64 "\n",
            trace_id, arg2, arg3, arg4, arg5, tid, time);
    const char* tid_str = sepos_return_module_thread_string(s->sep->chip_id, tid);
    switch (trace_id) {
        case 0x82000004: {    // SEP L4 task switch
            // %s instead of %c%c%c%c because the names will be nullbytes sometimes.
            uint64_t old_taskname = bswap32(arg2);
            uint64_t new_taskname = bswap32(arg4);
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "L4 task switch: old task thread name: 0x%02" PRIX64 "(%s) old task id: 0x%05" PRIX64
                    " new task thread name: 0x%02" PRIX64 "(%s) "
                    "arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, (char*)&old_taskname, arg3, arg4, (char*)&new_taskname, arg5);
            break;
        }
        case 0x82010004:    // panic
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "module panicked\n",
                    tid, tid_str);
            break;
        case 0x82030004:    // initialize_ool_page
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "initialize_ool_page:"
                    " obj_id: 0x%02" PRIX64 " address: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3);
            break;
        case 0x82040005:    // before SEP_IO__Control
        case 0x82040006:    // after SEP_IO__Control
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: %s "
                    "SEP_IO__Control Sending message to other module:"
                    " fromto: 0x%02" PRIX64 " method: 0x%02" PRIX64 " data0: 0x%02" PRIX64 " "
                    "data1: 0x%02" PRIX64 "\n",
                    tid, tid_str, (trace_id == 0x82040005) ? "Before" : "After", arg2, arg3, arg4, arg5);
            break;
        case 0x82050005:    // SEP_SERVICE__Call: request
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_SERVICE__Call: request:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " "
                    "method: 0x%02" PRIX64 " data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82050006:    // SEP_SERVICE__Call: response
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_SERVICE__Call: response:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " "
                    "method: 0x%02" PRIX64 " status/data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82060004:    // entered workloop function
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "module entered workloop function:"
                    " handlers0: 0x%02" PRIX64 " handlers1: 0x%02" PRIX64 " arg5: "
                    "0x%02" PRIX64 " arg6: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82060010:    // workloop function: interface_msgid==0xFFFE after
                            // receiving
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "module workloop function:"
                    " interface_msgid==0xFFFE after receiving: "
                    "data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x82060014:    // workloop function: before handlers0 handler
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP module "
                    "workloop function: before handlers0 handler:"
                    " handler_index: 0x%02" PRIX64 " data0: 0x%02" PRIX64 " data1: 0x%02" PRIX64 " "
                    "data2: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82060018:    // workloop function: handlers0: handler not found,
                            // panic
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP module "
                    "workloop function: handlers0: handler not found, panic:"
                    " interface_msgid: 0x%02" PRIX64 " method: 0x%02" PRIX64 " data0: "
                    "0x%02" PRIX64 " "
                    "data1: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x8206001C:    // workloop function: interface_msgid==0xFFFE
                            // before handler
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "module workloop function:"
                    " interface_msgid==0xFFFE before handler: data0: "
                    "0x%02" PRIX64 " handler: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3);
            break;
        case 0x82080005:    // 0x82080005==before Rpc_Call
        case 0x82080006:    // 0x82080006==after Rpc_Call
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: %s "
                    "Rpc_Call Sending message to other module:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " ool: 0x%02" PRIX64
                    " method: 0x%02" PRIX64 "\n",
                    tid, tid_str, (trace_id == 0x82080005) ? "Before" : "After", arg2, arg3, arg4, arg5);
            break;
        case 0x8208000D:    // before Rpc_Wait
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: Before "
                    "Rpc_Wait Receiving message from other module\n",
                    tid, tid_str);
            break;
        case 0x8208000E:    // after Rpc_Wait
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: After "
                    "Rpc_Wait "
                    "Receiving message from other module:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " ool: 0x%02" PRIX64
                    " method: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82080019:    // before Rpc_WaitFrom
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: Before "
                    "Rpc_WaitFrom Receiving message from other module:"
                    " arg2: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x8208001A:    // after Rpc_WaitFrom
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: After "
                    "Rpc_WaitFrom Receiving message from other module:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " ool: 0x%02" PRIX64
                    " method: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82080011:    // before Rpc_ReturnWait
        case 0x82080012:    // after Rpc_ReturnWait
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: %s "
                    "Rpc_ReturnWait Receiving message from other module:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " ool: 0x%02" PRIX64
                    " method: 0x%02" PRIX64 "\n",
                    tid, tid_str, (trace_id == 0x82080011) ? "Before" : "After", arg2, arg3, arg4, arg5);
            break;
        case 0x82080014:    // before Rpc_Return return response
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "Before Rpc_Return return response:"
                    " fromto: 0x%02" PRIX64 " interface_msgid: 0x%02" PRIX64 " ool: 0x%02" PRIX64
                    " method: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x8208001D:    // before Rpc_WaitNotify
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: Before "
                    "Rpc_WaitNotify:"
                    " Rpc_WaitNotify_arg2 != 0: Rpc_WaitNotify_arg1: "
                    "0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x8208001E:    // after Rpc_WaitNotify
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "After Rpc_WaitNotify:"
                    " svc_0x5_0_func_arg2 != 0: svc_0x5_0_func_arg1: "
                    "0x%02" PRIX64 " L4_MR0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3);
            break;
        case 0x82140004:    // _dispatch_thread_main__intr/SEPD interrupt
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "_dispatch_thread_main__intr/SEPD interrupt "
                    "trace_id 0x%02" PRIX64 ":"
                    " arg2: 0x%02" PRIX64 " arg3: 0x%02" PRIX64 " arg4: 0x%02" PRIX64 " arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, trace_id, arg2, arg3, arg4, arg5);
            break;
        case 0x82140014:    // SEP_Driver__Close
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Close:"
                    " module_name_int: 0x%02" PRIX64 " fromto: 0x%02" PRIX64 " "
                    "response_data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg5);
            break;
        case 0x82140024:    // *_enable_powersave_arg2/SEP_Driver__SetPowerState
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__SetPowerState:"
                    " function called: enable_powersave?: 0x%02" PRIX64 " "
                    "is_powersave_enabled: 0x%02" PRIX64 " field_cc3: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4);
            break;
        case 0x82140031:    // SEPD_thread_handler:
                            // SEP_Driver__before_InterruptAsync
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEPD_thread_handler: before_InterruptAsync:"
                    " arg2: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x82140032:    // SEPD_thread_handler:
                            // SEP_Driver__after_InterruptAsync
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEPD_thread_handler: after_InterruptAsync\n",
                    tid, tid_str);
            break;
        case 0x82140195:    // AESS_message_received: before
                            // AESS_keywrap_cmd_0x02
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "AESS_message_received: before AESS_keywrap_cmd_0x02:"
                    " data0_low: 0x%02" PRIX64 " data0_high: 0x%02" PRIX64 " data1_low: "
                    "0x%02" PRIX64 " data1_high: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82140196:    // AESS_message_received: after
                            // AESS_keywrap_cmd_0x02
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "AESS_message_received: after AESS_keywrap_cmd_0x02:"
                    " status: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x82140324:    // SEP_Driver__Mailbox_Rx
            if (offset + 0x90 + sizeof(uint32_t) > ARRAY_SIZE(s->regs)) {
                DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                        "SEP_Driver__Mailbox_Rx:"
                        " INVALID OFFSET!\n",
                        tid, tid_str);
                break;
            }
            memcpy((void*)&m + 0x00, &s->regs[offset + 0x88], sizeof(uint32_t));
            memcpy((void*)&m + 0x04, &s->regs[offset + 0x90], sizeof(uint32_t));
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_Rx:"
                    " endpoint: 0x%02x tag: 0x%02x opcode: "
                    "0x%02x(%u) param: 0x%02x data: 0x%02x\n",
                    tid, tid_str, m.ep, m.tag, m.op, m.op, m.param, m.data);
            break;
        case 0x82140328:    // SEP_Driver__Mailbox_RxMessageQueue
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_RxMessageQueue:"
                    " endpoint: 0x%02" PRIX64 " opcode: 0x%02" PRIX64 " arg4: "
                    "0x%02" PRIX64 " arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82140334:    // SEP_Driver__Mailbox_ReadMsgFetch
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_ReadMsgFetch:"
                    " endpoint: 0x%02" PRIX64 " data: 0x%02" PRIX64 " data2: 0x%02" PRIX64 " "
                    "read_msg.data[0]: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82140338:    // SEP_Driver__Mailbox_ReadBlocked
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_ReadBlocked:"
                    " for_TRNG_ASC0_ASC1_read_0 returned False: "
                    "data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x8214033C:    // SEP_Driver__Mailbox_ReadComplete
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_ReadComplete:"
                    " for_TRNG_ASC0_ASC1_read_0 returned True: "
                    "data0: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x82140340:    // SEP_Driver__Mailbox_Tx
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_Tx:"
                    " function_13 returned True:  arg2: 0x%02" PRIX64 " "
                    "arg3: 0x%02" PRIX64 " arg4: 0x%02" PRIX64 " arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82140344:    // SEP_Driver__Mailbox_TxStall
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_TxStall:"
                    " function_13 returned False: arg2: 0x%02" PRIX64 " "
                    "arg3: 0x%02" PRIX64 " arg4: 0x%02" PRIX64 " arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4, arg5);
            break;
        case 0x82140348:    // mod_ASC0_ASC1_function_message_received:
                            // method_0x4131/Mailbox_OOL_In
        case 0x8214034C:    // mod_ASC0_ASC1_function_message_received:
                            // Mailbox_OOL_Out
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: SEP "
                    "mod_ASC0_ASC1_function_message_received "
                    "SEP_Driver: Mailbox_OOL_%s:"
                    " arg2: 0x%02" PRIX64 " arg3: 0x%02" PRIX64 " arg4: 0x%02" PRIX64 "\n",
                    tid, tid_str, (trace_id == 0x82140348) ? "In" : "Out", arg2, arg3, arg4);
            break;
        case 0x82140360:    // SEP_Driver__Mailbox_Wake
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_Wake:"
                    " current value: registers[0x4108]: 0x%08" PRIX64 " "
                    "SEP_message_incoming: %" PRIu64 "\n",
                    tid, tid_str, arg2, arg3);
            break;
        case 0x82140364:    // SEP_Driver__Mailbox_NoData
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "SEP_Driver__Mailbox_NoData:"
                    " current value: registers[0x4108]: 0x%08" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        case 0x82140964:    // PMGR_message_received
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "PMGR_message_received:"
                    " fromto: 0x%02" PRIX64 " data0: 0x%02" PRIX64 " data1: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2, arg3, arg4);
            break;
        case 0x82140968:    // PMGR_enable_clock
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "PMGR_enable_clock:"
                    " enable_clock: 0x%02" PRIX64 "\n",
                    tid, tid_str, arg2);
            break;
        default:    // Unknown trace value
            DPRINTF("DEBUG_TRACE: Description: tid: 0x%05" PRIX64 "/%s: "
                    "Unknown trace_id 0x%02" PRIX64 ":"
                    " arg2: 0x%02" PRIX64 " arg3: 0x%02" PRIX64 " arg4: 0x%02" PRIX64 " "
                    "arg5: 0x%02" PRIX64 "\n",
                    tid, tid_str, trace_id, arg2, arg3, arg4, arg5);
            break;
    }
}

static uint64_t debug_trace_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPDebugTraceState* s   = opaque;
    uint64_t                 ret = 0;

    #ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
    #endif

    if (!s->sep->shmbuf_base) {
        qemu_log_mask(LOG_UNIMP, "DEBUG_TRACE: SHMBUF_BASE==NULL: Unknown read at 0x" HWADDR_FMT_plx " size=%u\n", addr,
                      size);
        return 0;
    }
    switch (addr) {
        case 0x0 : return 0xFFFFFFFF;    // negated trace exclusion mask for wrapper
        case 0x4 :                       // some index
        case 0x18:                       // unknown0
        case 0x40:                       // unknown1
            goto jump_default;
        case 0x1C: return 0x0;           // disable trace mask for inner function
        case 0x20: return 0xFFFFFFFF;    // trace mask for inner function
        default:
            qemu_log_mask(LOG_UNIMP, "DEBUG_TRACE: Unknown read at 0x" HWADDR_FMT_plx " size=%u ret==0x%" PRIX64 "\n",
                          addr, size, ret);
        jump_default:
            memcpy(&ret, &s->regs[addr], size);
            break;
    }
    return ret;
}

static const MemoryRegionOps debug_trace_reg_ops = {
    .write                 = debug_trace_reg_write,
    .read                  = debug_trace_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 1,
    .impl.max_access_size  = 8,
    .valid.unaligned       = false,
};

static void apple_sep_debug_trace_reset_enter(Object* obj, ResetType type)
{
    AppleSEPDebugTraceState* s = APPLE_SEP_DEBUG_TRACE(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void apple_sep_debug_trace_realize(DeviceState* dev, Error** errp)
{
    AppleSEPDebugTraceState* s   = APPLE_SEP_DEBUG_TRACE(dev);
    SysBusDevice*            sbd = SYS_BUS_DEVICE(dev);

    // TODO: Let's think about something for T8015
    memory_region_init_io(&s->mr, OBJECT(s), &debug_trace_reg_ops, s, "mr", s->size);
    sysbus_init_mmio(sbd, &s->mr);

    #ifdef SEP_ENABLE_DEBUG_TRACE_MAPPING
    if (s->sep->chip_id >= 0x8020) {
        if (s->sep->modern) {
            memory_region_add_subregion(&APPLE_A13(s->sep->cpu)->memory, s->sep->shmbuf_base + s->offset, &s->mr);
        }
        else {
            memory_region_add_subregion(&APPLE_A9(s->sep->cpu)->memory, s->sep->shmbuf_base + s->offset, &s->mr);
        }
    }
    #endif
}

static void apple_sep_debug_trace_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_debug_trace_reset_enter;
    dc->realize      = apple_sep_debug_trace_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPDebugTraceState, apple_sep_debug_trace, APPLE_SEP_DEBUG_TRACE, I2C_SLAVE)

AppleSEPDebugTraceState* apple_sep_debug_trace_create(AppleSEP* sep)
{
    AppleSEPDebugTraceState* s = APPLE_SEP_DEBUG_TRACE(qdev_new(TYPE_APPLE_SEP_DEBUG_TRACE));

    s->sep = sep;

    return s;
}
#endif

#ifdef ENABLE_CPU_DUMP_STATE
    #include "hw/boards.h"
    #include "qapi/error.h"

void apple_sep_dump_cpu_handler(void)
{
    MachineState* machine = MACHINE(qdev_get_machine());
    AppleSEP*     sep     = APPLE_SEP(object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    assert_nonnull(sep);
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
}
#endif
