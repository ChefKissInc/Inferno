/*
 * ChefKiss Kernel Patches.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/kernel_patches.h"
#include "hw/arm/patcher.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"

#define NOP       (0xD503201F)
#define MOV_W0_0  (0x52800000)
#define NOP_BYTES 0x1F, 0x20, 0x03, 0xD5
#define RET       (0xD65F03C0)
#define RET_BYTES 0xC0, 0x03, 0x5F, 0xD6
#define RETAB     (0xD65F0FFF)
#define PACIBSP   (0xD503237F)

static CKPatcherRange* ck_kp_range_from_va(const char* name, vaddr base, vaddr size)
{
    CKPatcherRange* range = g_new0(CKPatcherRange, 1);
    range->addr           = base;
    range->length         = size;
    range->ptr            = apple_boot_va_to_ptr(base);
    range->name           = name;
    return range;
}

static CKPatcherRange* ck_kp_find_section_range(MachoHeader64* hdr, const char* segment, const char* section)
{
    MachoSection64*        sec;
    MachoSegmentCommand64* seg;

    seg = apple_boot_get_segment(hdr, segment);
    if (seg == NULL) { return NULL; }

    sec = apple_boot_get_section(seg, section);
    return sec == NULL ? NULL : ck_kp_range_from_va(segment, sec->addr, sec->size);
}

// TODO: Fix host endianness BE vs LE troubles in here.
static MachoHeader64* ck_kp_find_image_header(MachoHeader64* hdr, const char* bundle_id)
{
    g_autofree CKPatcherRange* kmod_info_range  = NULL;
    g_autofree CKPatcherRange* kext_info_range  = NULL;
    g_autofree CKPatcherRange* kmod_start_range = NULL;
    uint64_t*                  info;
    uint64_t*                  start;
    uint32_t                   count;
    uint32_t                   i;
    char                       kname[256] = {0};
    const char*                prelinkinfo;
    const char*                last_dict;

    if (hdr->file_type == MH_FILESET) { return apple_boot_get_fileset_header(hdr, bundle_id); }

    kmod_info_range = ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__kmod_info");
    if (kmod_info_range == NULL) {
        kext_info_range = ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__info");
        if (kext_info_range == NULL) {
            error_report("Unsupported XNU.");
            return NULL;
        }

        prelinkinfo = strstr((const char*)kext_info_range->ptr, "PrelinkInfoDictionary");
        last_dict   = strstr(prelinkinfo, "<array>") + 7;
        while (last_dict) {
            const char* nested_dict;
            const char* ident;
            const char* end_dict = strstr(last_dict, "</dict>");
            if (!end_dict) { break; }

            nested_dict = strstr(last_dict + 1, "<dict>");
            while (nested_dict) {
                if (nested_dict > end_dict) { break; }

                nested_dict = strstr(nested_dict + 1, "<dict>");
                end_dict    = strstr(end_dict + 1, "</dict>");
            }

            ident = g_strstr_len(last_dict, end_dict - last_dict, "CFBundleIdentifier");
            if (ident != NULL) {
                const char* value = strstr(ident, "<string>");
                if (value != NULL) {
                    const char* value_end;

                    value     += strlen("<string>");
                    value_end  = strstr(value, "</string>");
                    if (value_end != NULL) {
                        memcpy(kname, value, value_end - value);
                        kname[value_end - value] = 0;
                        if (strcmp(kname, bundle_id) == 0) {
                            const char* addr =
                                g_strstr_len(last_dict, end_dict - last_dict, "_PrelinkExecutableLoadAddr");
                            if (addr != NULL) {
                                const char* avalue = strstr(addr, "<integer");
                                if (avalue != NULL) {
                                    avalue = strstr(avalue, ">");
                                    if (avalue != NULL) { return apple_boot_va_to_ptr(strtoull(++avalue, 0, 0)); }
                                }
                            }
                        }
                    }
                }
            }

            last_dict = strstr(end_dict, "<dict>");
        }

        return NULL;
    }
    kmod_start_range = ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__kmod_start");
    if (kmod_start_range != NULL) {
        info  = (uint64_t*)kmod_info_range->ptr;
        start = (uint64_t*)kmod_start_range->ptr;
        count = kmod_info_range->length / 8;
        for (i = 0; i < count; i++) {
            const char* kext_name = (const char*)apple_boot_va_to_ptr(info[i]) + 0x10;
            if (strcmp(kext_name, bundle_id) == 0) { return apple_boot_va_to_ptr(start[i]); }
        }
    }

    return NULL;
}

static CKPatcherRange* ck_kp_find_image_text(MachoHeader64* hdr, const char* bundle_id)
{
    hdr = ck_kp_find_image_header(hdr, bundle_id);
    return hdr == NULL ? NULL : ck_kp_find_section_range(hdr, "__TEXT_EXEC", "__text");
}

static CKPatcherRange* ck_kp_get_kernel_section(MachoHeader64* hdr, const char* segment, const char* section)
{
    if (hdr->file_type == MH_FILESET) {
        MachoHeader64* kernel = ck_kp_find_image_header(hdr, "com.apple.kernel");
        return kernel == NULL ? NULL : ck_kp_find_section_range(kernel, segment, section);
    }

    return ck_kp_find_section_range(hdr, segment, section);
}

static bool ck_kp_root_auth_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* func_start = ck_patcher_find_prev_insn(buffer, 30, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        warn_report("%s: failed to find pacibsp, trying to find old logic", __func__);
        uint8_t* ret = ck_patcher_find_next_insn(buffer, 4, RET, 0xFFFFFFFF, 0);
        if (ret == NULL) {
            error_report("%s: neither variants matched", __func__);
            return false;
        }
        stl_le_p(ret - 4, MOV_W0_0);
        return true;
    }
    stl_le_p(func_start, MOV_W0_0);
    stl_le_p(func_start + 4, RET);

    return true;
}

static bool ck_kp_root_hash_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* func_start = ck_patcher_find_prev_insn(buffer, 0x40, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        error_report("%s: failed to find pacibsp", __func__);
        return false;
    }
    stl_le_p(func_start, MOV_W0_0);
    stl_le_p(func_start + 4, RET);

    return true;
}

static void ck_kp_apfs_patches(CKPatcherRange* range)
{
    static const uint8_t root_auth_pattern[] = {
        0x08, 0xE0, 0x40, 0x39,    // ldrb w8, [x?, #0x38]
        0x08, 0x00, 0x00, 0x37,    // tbnz w8, #0x5, #?
        0x00, 0x0A, 0x80, 0x52,    // mov w?, #0x50
    };
    static const uint8_t root_auth_mask[] = {0x1F, 0xFC, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(root_auth_pattern) != sizeof(root_auth_mask));
    ck_patcher_find_callback(range, "bypass root authentication", root_auth_pattern, root_auth_mask,
                             sizeof(root_auth_pattern), sizeof(uint32_t), ck_kp_root_auth_callback);

    static const uint8_t root_rw_pattern[] = {
        0x00, 0x00, 0x00, 0x94,    // bl ?
        0x00, 0x00, 0x70, 0x37,    // tbnz w0, 0xE, ?
        0xA0, 0x03, 0x40, 0xB9,    // ldr x?, [x29/sp, ?]
        0x00, 0x78, 0x1F, 0x12,    // and w?, w?, 0xFFFFFFFE
        0xA0, 0x03, 0x00, 0xB9,    // str x?, [x29/sp, ?]
    };
    static const uint8_t root_rw_mask[] = {
        0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF, 0xA0, 0x03,
        0xFE, 0xFF, 0x00, 0xFC, 0xFF, 0xFF, 0xA0, 0x03, 0xC0, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern) != sizeof(root_rw_mask));
    static const uint8_t root_rw_repl[] = {MOV_W0_0_BYTES};
    if (!ck_patcher_find_replace(range, "allow mounting root as R/W", root_rw_pattern, root_rw_mask,
                                 sizeof(root_rw_pattern), sizeof(uint32_t), root_rw_repl, NULL, 4,
                                 sizeof(root_rw_repl)))
    {
        static const uint8_t root_rw_pattern_new[] = {
            0x00, 0x00, 0x00, 0x94,    // bl ?
            0x00, 0x00, 0x70, 0x37,    // tbnz w0, 0xE, ?
            0x00, 0x00, 0x80, 0x52,    // mov w0, #0
            00,   0x00, 0x00, 0x14,    // bl ?
        };
        static const uint8_t root_rw_mask_new[] = {
            0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC,
        };
        QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern_new) != sizeof(root_rw_mask_new));
        ck_patcher_find_replace(range, "allow mounting root as R/W (new)", root_rw_pattern_new, root_rw_mask_new,
                                sizeof(root_rw_pattern_new), sizeof(uint32_t), root_rw_repl, NULL, 4,
                                sizeof(root_rw_repl));
    }

    static const uint8_t root_hash_pattern[] = {
        0x88, 0x62, 0x40, 0xF9,    // ldr x8, [x20, #0xC0]
        0x08, 0x89, 0x47, 0x79,    // ldrh w8, [x8, #0x3C4]
        0x1F, 0x11, 0x00, 0x71,    // cmp w8, #0x4
    };
    ck_patcher_find_callback(range, "bypass root hash authentication", root_hash_pattern, NULL,
                             sizeof(root_hash_pattern), sizeof(uint32_t), ck_kp_root_hash_callback);
}

static bool ck_kp_tc_callback(void* ctx, uint8_t* buffer)
{
    if (((ldl_le_p(buffer - 4) & 0xFF000000) != 0x91000000) && ((ldl_le_p(buffer - 8) & 0xFF000000) != 0x91000000)) {
        return false;
    }

    uint8_t* ldrb         = ck_patcher_find_next_insn(buffer, 256, 0x39402C00, 0xFFFFFC00, 0);
    uint32_t cdhash_param = extract32(ldl_le_p(ldrb), 5, 5);
    uint8_t* frame;
    uint8_t* start = buffer;
    bool     pac;

    frame = ck_patcher_find_prev_insn(buffer, 10, 0x910003FD, 0xFF8003FF, 0);
    if (frame == NULL) { info_report("%s: found AMFI (Leaf)", __func__); }
    else {
        info_report("%s: found AMFI (Routine)", __func__);
        start = ck_patcher_find_prev_insn(frame, 10, 0xA9A003E0, 0xFFE003E0, 0);
        if (start == NULL) {
            start = ck_patcher_find_prev_insn(frame, 10, 0xD10003FF, 0xFF8003FF, 0);
            if (start == NULL) {
                error_report("%s: failed to find AMFI start", __func__);
                return false;
            }
        }
    }

    pac = ck_patcher_find_prev_insn(start, 5, PACIBSP, 0xFFFFFFFF, 0) != NULL;
    switch (cdhash_param) {
        case 0: {
            // adrp x8, ?
            uint8_t* adrp = ck_patcher_find_prev_insn(start, 10, 0x90000008, 0x9F00001F, 0);
            if (adrp != NULL) { start = adrp; }
            stl_le_p(start, 0x52802020);    // mov w0, 0x101
            stl_le_p(start + 4, (pac ? RETAB : RET));
            return true;
        }
        case 1:
            stl_le_p(start, 0x52800040);         // mov w0, 2
            stl_le_p(start + 4, 0x39000040);     // strb w0, [x2]
            stl_le_p(start + 8, 0x52800020);     // mov w0, 1
            stl_le_p(start + 12, 0x39000060);    // strb w0, [x3]
            stl_le_p(start + 16, 0x52800020);    // mov w0, 1
            stl_le_p(start + 20, (pac ? RETAB : RET));
            return true;
        default: error_report("%s: found unexpected AMFI prototype: %u", __func__, cdhash_param); break;
    }
    return false;
}

static bool ck_kp_tc_ios16_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 100, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) { return false; }

    stl_le_p(start, 0x52802020);    // mov w0, 0x101
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_tc_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0x00, 0x02, 0x80, 0x52,    // mov w?, 0x16
        0x00, 0x00, 0x00, 0xD3,    // lsr ?
        0x00, 0x00, 0x00, 0x9B,    // madd ?
    };
    static const uint8_t mask[] = {0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "all binaries in trustcache", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                                  ck_kp_tc_callback))
    {
        static const uint8_t ios16_pattern[] = {0xC0, 0xCF, 0x9D, 0xD2};    // mov w?, 0xEE7E
        static const uint8_t io16_mask[]     = {0xC0, 0xFF, 0xFF, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(ios16_pattern) != sizeof(io16_mask));
        ck_patcher_find_callback(range, "all binaries in trustcache (iOS 16+)", ios16_pattern, io16_mask,
                                 sizeof(ios16_pattern), sizeof(uint32_t), ck_kp_tc_ios16_callback);
    }
}

static bool ck_kp_amfi_sha1_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* cmp = ck_patcher_find_next_insn(buffer, 0x10, 0x7100081F, 0xFFFFFFFF,
                                             0);    // cmp w0, 2

    if (cmp == NULL) {
        error_report("%s: failed to find cmp", __func__);
        return false;
    }

    stl_le_p(cmp, 0x6B00001F);    // cmp w0, w0
    return true;
}

static bool ck_kp_amfi_tc_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 0x20, PACIBSP, 0xFFFFFFFF, 0);
    if (start == NULL) {
        error_report("%s: failed to find start of function", __func__);
        return false;
    }

    stl_le_p(start, 0xD2800020);        // mov x0, #1
    stl_le_p(start + 4, 0xB4000042);    // cbz x2, #0x8
    stl_le_p(start + 8, 0xF9000040);    // str x0, [x2]
    stl_le_p(start + 12, RET);
    return true;
}

static void ck_kp_amfi_patches(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {0x02, 0x00, 0xD0, 0x36};    // tbz w2, 0x1A, ?
    static const uint8_t mask[]    = {0x1F, 0x00, 0xF8, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "allow SHA1 signatures in AMFI", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                             ck_kp_amfi_sha1_callback);

    static const uint8_t amfi_tc_cache_pattern[] = {
        0xE0, 0x03, 0x00, 0x91,    // mov x0, sp
        0xE1, 0x03, 0x13, 0xAA,    // mov x1, x19
        0x00, 0x00, 0x00, 0x94,    // bl trustCacheQueryGetFlags
        0x9F, 0x02, 0x00, 0x71,    // cmp w20, 0
        0xE0, 0x17, 0x9F, 0x1A,    // cset w0, eq
    };
    static const uint8_t amfi_tc_cache_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
        0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(amfi_tc_cache_pattern) != sizeof(amfi_tc_cache_mask));
    ck_patcher_find_callback(range, "all binaries in TrustCache (AMFI)", amfi_tc_cache_pattern, amfi_tc_cache_mask,
                             sizeof(amfi_tc_cache_pattern), sizeof(uint32_t), ck_kp_amfi_tc_callback);
}

static bool ck_kp_mac_mount_callback(void* ctx, uint8_t* buffer)
{
    // Search for tbnz w?, 5, ?
    uint8_t* inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
    if (inst == NULL) {
        inst = ck_patcher_find_next_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
        if (inst == NULL) {
            error_report("%s: failed to find nop point", __func__);
            return false;
        }
    }

    // Allow MNT_UNION mounts
    stl_le_p(inst, NOP);

    // Search for ldrb w8, [x?, 0x71]
    inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
    if (inst == NULL) {
        // Search for the same, but forwards.
        inst = ck_patcher_find_next_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
        if (inst == NULL) {
            // Search for add x8, x8/16, #0x70
            inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x9101C008, 0xFFFFFCFF, 0);
            // Search for ldr w8, [x8, #0x1]
            if (inst != NULL && ldl_le_p(inst + 4) == 0x39400508) { inst += 4; }
            else {
                error_report("%s: failed to find xzr point", __func__);
                return false;
            }
        }
    }

    // Replace with a mov x8, xzr
    // This will bypass the (vp->v_mount->mnt_flag & MNT_ROOTFS) check
    stl_le_p(inst, 0xAA1F03E8);

    return true;
}

static void ck_kp_mac_mount_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0xE9,
        0x2F,
        0x1F,
        0x32,    // orr w9, wzr, 0x1FFE
    };
    if (!ck_patcher_find_callback(range, "allow remounting rootfs, union mounts (old)", pattern, NULL, sizeof(pattern),
                                  sizeof(uint32_t), ck_kp_mac_mount_callback))
    {
        static const uint8_t new_pattern[] = {
            0xC9,
            0xFF,
            0x83,
            0x12,    // movz w/x9, 0x1FFE/-0x1FFF
        };
        static const uint8_t new_mask[] = {0xFF, 0xFF, 0xFF, 0x3F};
        QEMU_BUILD_BUG_ON(sizeof(new_pattern) != sizeof(new_mask));
        ck_patcher_find_callback(range, "allow remounting rootfs, union mounts (new)", new_pattern, new_mask,
                                 sizeof(new_pattern), sizeof(uint32_t), ck_kp_mac_mount_callback);
    }
}

static bool ck_kp_kprintf_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* comparison      = buffer + sizeof(uint32_t) * 3;
    uint32_t comparison_inst = ldl_le_p(comparison);
    // if cbnz
    if (comparison_inst & BIT32(24)) { stl_le_p(comparison, NOP); }
    else {
        // turn into unconditional branch
        stl_le_p(comparison, 0x14000000 | extract32(comparison_inst, 5, 19));
    }
    return true;
}

static void ck_kp_kprintf_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0xAA, 0x43, 0x00, 0x91,    // add x10, fp, #0x10
        0xEA, 0x07, 0x00, 0xF9,    // str x10, [sp, #0x8]
        0x08, 0x00, 0x00, 0x2A,    // orr w8, w?, w?
        0x08, 0x00, 0x00, 0x34,    // cbz w8, #?
    };
    static const uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0x1F, 0xFC, 0xE0, 0xFF, 0x1F, 0x00, 0x00, 0xFE};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "force enable kprintf", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                                  ck_kp_kprintf_callback))
    {
        static const uint8_t pattern_new[] = {
            0x08, 0x01, 0x40, 0x39,    // ldrb w8, [x8, #0x?]
            0x08, 0x00, 0x00, 0x36,    // tbz w8, #0, #?
            0xA0, 0x43, 0x00, 0x91,    // add x?, fp, #0x10
            0xE0, 0x17, 0x00, 0xF9,    // str x?, [sp, #0x28]
            0xE0, 0xA3, 0x00, 0x91,    // add x?, sp, #0x28
            0x00, 0x00, 0x00, 0x14,    // b #?
        };
        static const uint8_t mask_new[] = {0xFF, 0x03, 0xC0, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF,
                                           0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC};
        QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
        static const uint8_t repl_new[] = {NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES, NOP_BYTES};
        ck_patcher_find_replace(range, "force enable kprintf (new)", pattern_new, mask_new, sizeof(pattern_new),
                                sizeof(uint32_t), repl_new, NULL, 0, sizeof(repl_new));
    }
}

// gAMXVersion seemingly unused, but removing it just in case.
// New: Used in iOS 17+ to set the cpu_capabilities bit.
static bool ck_kp_amx_common(uint8_t* buffer, bool newer)
{
    uint8_t* amx_ver_str = ck_patcher_find_prev_insn(buffer, newer ? 6 : 10, 0xB8000000, 0xFEC00000, newer ? 0 : 1);
    if (amx_ver_str == NULL) {
        error_report("%s: Failed to find store to gAMXVersion.", __func__);
        return false;
    }
    stl_le_p(amx_ver_str, NOP);

    return true;
}

static bool ck_kp_amx_callback(void* ctx, uint8_t* buffer)
{
    stl_le_p(buffer, 0x52810009);    // mov w9, #0x800

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_new_callback(void* ctx, uint8_t* buffer)
{
    // remove AMX support bit from movk
    // movk w?, #0x100, lsl #0x10
    stl_le_p(buffer + 4, ldl_le_p(buffer + 4) & ~(0x800 << 5));

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_newer_callback(void* ctx, uint8_t* buffer) { return ck_kp_amx_common(buffer, true); }

// in _commpage_populate
static void ck_kp_amx_patch(CKPatcherRange* range)
{
    static const uint8_t pattern_new[] = {
        0x00, 0x90, 0x87, 0x52,    // mov w?, #0x3C80
        0x00, 0x20, 0xA1, 0x72,    // movk w?, #0x900, lsl #0x10
        0x00, 0x40, 0x00, 0x2A,    // orr w?, w?, w?, lsl #0x10
    };
    static const uint8_t mask_new[] = {0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0xFC, 0xE0, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
    if (!ck_patcher_find_callback(range, "disable AMX (new)", pattern_new, mask_new, sizeof(pattern_new),
                                  sizeof(uint32_t), ck_kp_amx_new_callback))
    {
        static const uint8_t pattern[] = {
            0xE9, 0x83, 0x05, 0x32,    // mov w9, #0x8000800
            0x09, 0x00, 0x00, 0xAA,    // orr x9, x?, x?
        };
        static const uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFC, 0xE0, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
        if (!ck_patcher_find_callback(range, "disable AMX", pattern, mask, sizeof(pattern), sizeof(uint32_t),
                                      ck_kp_amx_callback))
        {
            static const uint8_t pattern_newer[] = {
                0x0A, 0xF1, 0x1C, 0xD5,    // msr amx_config_el1, x?
                0xDF, 0x3F, 0x03, 0xD5,    // isb
            };
            static const uint8_t mask_newer[] = {0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            ck_patcher_find_callback(range, "disable AMX (newer)", pattern_newer, mask_newer, sizeof(pattern_newer),
                                     sizeof(uint32_t), ck_kp_amx_newer_callback);
        }
    }
}

static void ck_kp_apfs_snapshot_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = "com.apple.os.update-";
    static const uint8_t repl[]    = "shitcode.os.bullshit";
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(repl));
    ck_patcher_find_replace(range, "disable APFS snapshots", pattern, NULL, sizeof(pattern), 0, repl, NULL, 0,
                            sizeof(repl));
}

// this will tell launchd this is an internal build,
// and that way we can get hactivation without bypassing
// or patching the activation procedure.
// This is NOT an iCloud bypass. This is utilising code that ALREADY exists
// in the activation daemon. This is essentially telling iOS, it's a
// development kernel/device, NOT the real product sold on market. IF you
// decide to use this knowledge to BYPASS technological countermeasures
// or any other intellectual theft or crime, YOU are responsible in full,
// AND SHOULD BE PROSECUTED TO THE FULL EXTENT OF THE LAW.
// We do NOT endorse nor approve the theft of property.
static void ck_kp_hactivation_patch(CKPatcherRange* range)
{
    static const uint8_t pattern[] = "\0release";
    static const uint8_t repl[]    = "profile";
    ck_patcher_find_replace(range, "enable hactivation", pattern, NULL, sizeof(pattern), 0, repl, NULL, 1,
                            sizeof(repl));
}

static void ck_kp_sep_mgr_patches(CKPatcherRange* range)
{
    static const uint8_t pattern[] = {
        0x00, 0x04, 0x00, 0xF9,    // str x?, [x?, #0x8]
        0x08, 0x04, 0x80, 0x52,    // mov w8, #0x20
        0x08, 0x10, 0x00, 0xB9,    // str w8, [x?, #0x10]
    };
    static const uint8_t mask[] = {0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = {0x28, 0x00, 0xA0, 0x52};    // mov w8, #0x10000
    if (!ck_patcher_find_replace(range, "increase SCOT size to 0x10000 to use it as TRAC", pattern, mask,
                                 sizeof(pattern), sizeof(uint32_t), repl, NULL, 4, sizeof(repl)))
    {
        static const uint8_t pattern_new[] = {
            0x00, 0x00, 0x1E, 0xF8,    // stur x?, [x?, #-0x20]
            0x08, 0x04, 0x80, 0x52,    // mov w8, #0x20
            0x08, 0x80, 0x1E, 0xB8,    // stur w8, [x?, #-0x18]
        };
        static const uint8_t mask_new[] = {0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
        ck_patcher_find_replace(range, "increase SCOT size to 0x10000 to use it as TRAC (new)", pattern_new, mask_new,
                                sizeof(pattern_new), sizeof(uint32_t), repl, NULL, 4, sizeof(repl));
    }
}

static bool ck_kp_img4_callback(void* ctx, uint8_t* buffer)
{
    uint8_t* start = ck_patcher_find_prev_insn(buffer, 200, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) { return false; }

    stl_le_p(start, MOV_W0_0);
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_img4_patches(CKPatcherRange* range)
{
    // in Img4DecodePerformTrustEvaluationWithCallbacksInternal
    static const uint8_t pattern[] = {
        0x21,
        0x09,
        0x43,
        0xB2,    // orr x1, x9, #0xe000000000000000
    };
    ck_patcher_find_callback(range, "allow unsigned firmware in img4_firmware_evaluate", pattern, NULL, sizeof(pattern),
                             sizeof(uint32_t), ck_kp_img4_callback);
}

static void ck_kp_cs_patches(CKPatcherRange* range)
{
    // skip code signature checks in vm_fault_enter
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x18, 0x36,    // tbz w?, #3, #?
        0x00, 0x00, 0x80, 0x52,    // mov w?, #0
    };
    static const uint8_t mask[] = {0x00, 0x00, 0xF8, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = {NOP_BYTES};
    if (!ck_patcher_find_replace(range, "bypass code signature checks", pattern, mask, sizeof(pattern),
                                 sizeof(uint32_t), repl, NULL, 0, sizeof(repl)))
    {
        static const uint8_t alt[] = {
            0x00, 0x00, 0x18, 0x36,    // tbz w?, #3, #?
            0x10, 0x02, 0x17, 0xAA,    // mov x?, x?
            0x00, 0x00, 0x80, 0x52,    // mov w?, #0
        };
        static const uint8_t mask_alt[] = {0x00, 0x00, 0xF8, 0xFF, 0x10, 0xFE, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(alt) != sizeof(mask_alt));
        ck_patcher_find_replace(range, "bypass code signature checks (alt)", alt, mask_alt, sizeof(alt),
                                sizeof(uint32_t), repl, NULL, 0, sizeof(repl));
    }
}

static void ck_kp_pmap_cs_enforce_patch(CKPatcherRange* range)
{
    // in pmap_enter_options_internal
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x00, 0x94,    // bl #?
        0x00, 0x00, 0x00, 0x35,    // cbnz w0, #?
        0x88, 0x63, 0x80, 0x92,    // mov x8, #0xfffffffffffffce3
    };
    static const uint8_t mask[] = {
        0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = {MOV_W0_0_BYTES, NOP_BYTES};
    ck_patcher_find_replace(range, "bypass pmap_cs_enforce", pattern, mask, sizeof(pattern), sizeof(uint32_t), repl,
                            NULL, 0, sizeof(repl));
}

void ck_patch_kernel(MachoHeader64* hdr)
{
    MachoHeader64*             apfs_hdr;
    g_autofree CKPatcherRange* apfs_text;
    g_autofree CKPatcherRange* apfs_cstring;
    g_autofree CKPatcherRange* amfi_text;
    g_autofree CKPatcherRange* sep_mgr_text;
    g_autofree CKPatcherRange* img4_text;
    g_autofree CKPatcherRange* kernel_text;
    g_autofree CKPatcherRange* kernel_const;
    g_autofree CKPatcherRange* kernel_ppltext;

    apfs_hdr  = ck_kp_find_image_header(hdr, "com.apple.filesystems.apfs");
    apfs_text = ck_kp_find_section_range(apfs_hdr, "__TEXT_EXEC", "__text");
    ck_kp_apfs_patches(apfs_text);
    apfs_cstring = ck_kp_find_section_range(apfs_hdr, "__TEXT", "__cstring");
    if (apfs_cstring == NULL) { apfs_cstring = ck_kp_find_section_range(hdr, "__TEXT", "__cstring"); }
    ck_kp_apfs_snapshot_patch(apfs_cstring);

    amfi_text = ck_kp_find_image_text(hdr, "com.apple.driver.AppleMobileFileIntegrity");
    ck_kp_amfi_patches(amfi_text);

    sep_mgr_text = ck_kp_find_image_text(hdr, "com.apple.driver.AppleSEPManager");
    ck_kp_sep_mgr_patches(sep_mgr_text);

    img4_text = ck_kp_find_image_text(hdr, "com.apple.security.AppleImage4");
    ck_kp_img4_patches(img4_text);

    kernel_text = ck_kp_get_kernel_section(hdr, "__TEXT_EXEC", "__text");
    ck_kp_mac_mount_patch(kernel_text);
    ck_kp_kprintf_patch(kernel_text);
    ck_kp_amx_patch(kernel_text);
    ck_kp_cs_patches(kernel_text);
    kernel_const = ck_kp_get_kernel_section(hdr, "__TEXT", "__const");
    ck_kp_hactivation_patch(kernel_const);

    kernel_ppltext = ck_kp_find_section_range(hdr, "__PPLTEXT", "__text");
    if (kernel_ppltext == NULL) {
        warn_report("Failed to find `__PPLTEXT.__text`.");
        ck_kp_tc_patch(kernel_text);
        ck_kp_pmap_cs_enforce_patch(kernel_text);
    }
    else {
        ck_kp_tc_patch(kernel_ppltext);
        ck_kp_pmap_cs_enforce_patch(kernel_ppltext);
    }
}

#define MOVK_IMM16_SHIFT 5

static bool ck_kp_virt_boot_sctlr_callback(void* ctx, uint8_t* buffer)
{
    unsigned* matches = ctx;
    uint32_t  insns[5];
    uint32_t  reg;
    size_t    i;

    for (i = 0; i < ARRAY_SIZE(insns); ++i) { insns[i] = ldl_le_p(buffer + i * sizeof(uint32_t)); }

    reg = insns[0] & 0x1F;
    for (i = 1; i < ARRAY_SIZE(insns); ++i) {
        if ((insns[i] & 0x1F) != reg) { return false; }
    }
    if (extract32(insns[4], 5, 5) != reg) { return false; }

    insns[2] |= (uint32_t)0xC800 << MOVK_IMM16_SHIFT;    // SCTLR_EL1.EnIA (31), .EnIB (30) and .EnDA (27)
    insns[3] |= (uint32_t)0x2000 << MOVK_IMM16_SHIFT;    // SCTLR_EL1.EnDB (13)
    stl_le_p(buffer + 2 * sizeof(uint32_t), insns[2]);
    stl_le_p(buffer + 3 * sizeof(uint32_t), insns[3]);

    /*
     * `common_start` builds the value twice: once for `msr sctlr_el1` and once
     * to compare the result against, spinning forever on a mismatch.
     */
    return ++*matches == 2;
}

static void ck_kp_virt_boot_sctlr_patch(CKPatcherRange* range)
{
    unsigned matches = 0;

    static const uint8_t pattern[] = {
        0x00, 0x00, 0xE0, 0xF2,    // movk x?, #0, lsl #0x30
        0x00, 0x00, 0xC0, 0xF2,    // movk x?, #0, lsl #0x20
        0x00, 0x00, 0xA0, 0xF2,    // movk x?, #?, lsl #0x10
        0x00, 0x00, 0x80, 0xF2,    // movk x?, #?
        0x00, 0x00, 0x62, 0xB2,    // orr x?, x?, #0x40000000
    };
    static const uint8_t mask[] = {
        0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
        0xE0, 0xFF, 0x00, 0x00, 0xE0, 0xFF, 0x00, 0xFC, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback_ctx(range, "enable all PAC keys in the boot SCTLR_EL1", pattern, mask, sizeof(pattern),
                                 sizeof(uint32_t), &matches, ck_kp_virt_boot_sctlr_callback);
}

static bool ck_kp_pac_jop_toggle_callback(void* ctx, uint8_t* buffer)
{
    // msr sctlr_el1, x?
    uint8_t* msr = ck_patcher_find_next_insn(buffer, 24, 0xD5181000, 0xFFFFFFE0, 0);
    if (msr == NULL) {
        error_report("%s: failed to find the SCTLR_EL1 write", __func__);
        return false;
    }

    stl_le_p(msr, NOP);

    return true;
}

static void ck_kp_virt_jop_toggle_patch(CKPatcherRange* range)
{
    // in pmap_switch_user_ttb_internal
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x84, 0x52,    // mov w?, #0x2000
        0x00, 0x00, 0xB1, 0x72,    // movk w?, #0x8800, lsl #0x10
        0x00, 0x10, 0x38, 0xD5,    // mrs x?, sctlr_el1
    };
    static const uint8_t mask[] = {0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF};
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "keep the per-pmap JOP toggle from disabling PAC keys", pattern, mask,
                             sizeof(pattern), sizeof(uint32_t), ck_kp_pac_jop_toggle_callback);
}

static bool ck_kp_disable_ppl_locked_down(void* ctx, uint8_t* buffer)
{
    // mov w?, #1
    uint8_t* mov = ck_patcher_find_next_insn(buffer + 8 + sizeof(uint32_t), 2, 0x52800020, 0xFFFFFFE0, 0);
    if (mov == NULL) { return false; }

    // str w?, pmap_ppl_locked_down
    uint8_t* str = ck_patcher_find_next_insn(mov + sizeof(uint32_t), 2, 0xB9000000, 0xFF000000, 0);
    if (str == NULL) { return false; }

    // bl #?
    if ((ldl_le_p(str + sizeof(uint32_t)) & 0xFC000000) != 0x94000000) { return false; }

    stl_le_p(str, NOP);
    return true;
}

static void ck_kp_disable_ppl(CKPatcherRange* range)
{
    static const uint8_t gxf_pattern[] = {
        0x20, 0x00, 0x80, 0xD2,    // mov x0, #1
        0x40, 0xF1, 0x1E, 0xD5,    // msr gxf_config_el1, x0
    };
    static const uint8_t gxf_replace[] = {
        RET_BYTES,
    };
    if (ck_patcher_find_replace(range, "neutralise gxf_enable", gxf_pattern, NULL, sizeof(gxf_pattern),
                                sizeof(uint32_t), gxf_replace, NULL, 0, sizeof(gxf_replace)))
    {
        static const uint8_t ppl_pattern[] = {
            0x00, 0x00, 0x38, 0x37,    // tbnz w?, 0x7, ?
            0xDF, 0x47, 0x03, 0xD5,    // msr daifset, #7
        };
        static const uint8_t ppl_pattern_mask[] = {0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        QEMU_BUILD_BUG_ON(sizeof(ppl_pattern) != sizeof(ppl_pattern_mask));
        ck_patcher_find_callback(range, "neutralise pmap_ppl_locked_down", ppl_pattern, ppl_pattern_mask,
                                 sizeof(ppl_pattern), sizeof(uint32_t), ck_kp_disable_ppl_locked_down);
        ck_patcher_find_callback(range, "neutralise pmap_ppl_locked_down (inlined)", ppl_pattern, ppl_pattern_mask,
                                 sizeof(ppl_pattern), sizeof(uint32_t), ck_kp_disable_ppl_locked_down);
    }
}

void ck_patch_virt(MachoHeader64* hdr, const bool enable_pac)
{
    g_autofree CKPatcherRange* kernel_text    = NULL;
    g_autofree CKPatcherRange* kernel_ppltext = NULL;

    kernel_text = ck_kp_get_kernel_section(hdr, "__TEXT_EXEC", "__text");
    if (kernel_text == NULL) {
        error_report("Failed to find `__TEXT_EXEC.__text`.");
        return;
    }

    if (enable_pac) { ck_kp_virt_boot_sctlr_patch(kernel_text); }

    kernel_ppltext = ck_kp_find_section_range(hdr, "__PPLTEXT", "__text");
    ck_kp_virt_jop_toggle_patch(kernel_ppltext == NULL ? kernel_text : kernel_ppltext);

    ck_kp_disable_ppl(kernel_text);
}
