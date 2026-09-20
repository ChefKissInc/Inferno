/*
 * Apple OS Boot Logic.
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
#include "crypto/hash.h"
#include "hw/arm/boot.h"
#include "hw/arm/dt.h"
#include "hw/arm/mem.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "system/memory.h"
#include "img4.h"
#include "lzss.h"
#include <lzfse.h>

#if 0
    #define DINFO(fmt, ...) info_report(fmt, ##__VA_ARGS__)
#else
    #define DINFO(fmt, ...) \
        do { }              \
        while (0)
#endif

static const char* KEEP_COMP[] = {
    "adbe0,s8000\0$",
    // "aop-audio\0$",
    // "aop-audio-control\0$",
    // "aop-audio-speaker\0$",
    // "audio-aop-lp-mic-in\0$",
    // "audio-aop-mca2\0$",
    // "audio-aop-pcmaudiomgr\0$",
    // "alc,t8030\0$",
    // "audio-data,aop-audio-haptic\0$",
    // "audio-data,halogen\0$",
    // "audio-data,hawking\0$",
    // "audio-data,baseband-voice\0$",
    // "audio-data,dsp-debug1\0$",
    // "audio-data,mikeybus-secondary\0$",
    // "audio-data,aop-audio-hpdb\0$",
    // "halle-sensor,aop-ad5860-config\0$",
    // "audio-aop-hall\0$",
    // "audio-aop-hpdbg\0$",
    // "audio-aop-haptic\0$",
    // "audio-aop-haptic-leap\0$",
    // "audio-aop-hapticmgr\0$",
    // "aop-audio-hpdbg\0$",
    // "audio-data,aop-audio-hpdbg\0$",
    // "haptics-support,leap\0$",
    // "audio,embedded-resource-manager\0$",
    // "audio-control,cs35l27\0$",
    // "audio-data,audio-loopback\0$",
    // "audio-data,cs35l27\0$",
    // "audio-data,cs42l77\0$",
    // "audio-control,cs42l77\0$",
    "aes,s8000\0$",
    "aic,1\0$",
    "apcie-bridge\0$",
    "apple,lightning\0ARM,v8\0$",
    "apple,thunder\0ARM,v8\0$",
    "apple,twister\0ARM,v8\0$",
    "arm-io,s8000\0$",
    "arm-io,t8030\0$",
    "atc-phy,t8030\0atc-phy,t8027\0$",
    "backlight\0$",
    "backlight,lm3539\0$",
#ifdef ENABLE_BASEBAND
    "baseband,i19\0$",
#endif
    // "biosensor,pearl\0$", // not implemented
    "buttons\0$",
    // "bluetooth,n88\0$",
    "dart,s8000\0dart,s5l8960x\0$",
    "dart,t8020\0$",
    "disp0,t8030\0$",
    "display-pmu,chestnut\0$",
    "dock,9pin\0$",
    "dock,0pin\0$",
    "gen-mt-decider\0$",
    "gpio,s8000\0gpio,s5l8960x\0$",
    "gpio,t8015\0gpio,s5l8960x\0$",
    "gpio,t8030\0gpio,s5l8960x\0$",
    "i2c,s8000\0i2c,s5l8940x\0iic,soft\0$",
    "i2c,t8030\0i2c,s5l8940x\0iic,soft\0$",
    "iic,soft\0$",
    "iommu-mapper\0$",
    "iop,ascwrap-v2\0$",
    "iop,t8030\0iop,t8015\0$",
    "iop-nub,rtbuddy-v2\0$",
    "iop,s8000\0$",
    "iop-nub,sep\0$",
    "lcd,pinot\0$",
    // "mca-switch,t8030\0$",
    // "mcaCluster,t8030\0$",
    // "mca,t8030\0$",
    "mipi-dsim-1,synopsys\0$",
    "N104AP\0iPhone12,1\0AppleARM\0$",
    "N104DEV\0iPhone12,1\0AppleARM\0$",
    "N66AP\0iPhone8,2\0AppleARM\0$",
    "N66DEV\0iPhone8,2\0AppleARM\0$",
    "nvme-mmu,s8000\0$",
    "otgphyctrl,s8000\0otgphyctrl,s5l8960x\0$",
    "pmgr1,s8000\0$",
    "pmgr1,t8030\0$",
    // "pmu,d2255\0$", // buggy
    "pmu,spmi\0pmu,avus\0$",
    "roswell\0$",
    "sart,coastguard\0$",
    "sart,t8030\0$",
    "sacm,1\0$",
    "scaler,t8030\0scaler,s5l8960x\0$",
    "sio-dma-controller\0$",
    "smc-pmu\0$",
    "smc-tempsensor\0$",
    "soc-tuner,s8000\0$",
    "soc-tuner,t8030\0$",
    "spi-1,samsung\0$",
    "spmi,gen0\0$",
    "spmi,t8015\0$",
    "uart-1,samsung\0$",
    "usb-complex,s8000\0usb-complex,s5l8960x\0$",
    "usb-device,s5l8900x\0$",
    "usb-device,s8000\0usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-drd,t8030\0usb-drd,t8027\0$",
    "wdt,s8000\0wdt,s5l8960x\0$",
    "wdt,t8030\0wdt,s5l8960x\0$",
    "apcie,t8030\0$",
    "apcie,s8000\0$",
    "rsm,t8030\0$",
    "tempsensor,t8020\0$",
    "tempsensor,t8030\0$",
    "mtrtempsensor,t8020\0$",
    "dietempctl,t8030\0$",
    "vtempctl,t8030\0$",
    "aod,t8030\0aod,s5l8960x\0$",
    "mic-temp-sens,ica60\0$",
    "aapl,spmi\0$",
    "accbuck,fan53740\0$",
};

static const char* REM_NAMES[] = {
    "accel\0$",
    "gyro\0$",
    "compass\0$",
    "spherecontrol\0$",
    "prox\0$",
    "SPUApp\0$",
    "aop-mca\0$",
    "smc-control\0$",
    "aop-smart-cover\0$",
    "rose\0$",
    "smc-aop\0$",
    "gfx-asc\0$",
    "amfm\0$",
    "dart-ane\0$",
    "dart-avd\0$",
    "dart-ave\0$",
    "dart-isp\0$",
    "dart-jpeg0\0$",
    "dart-jpeg1\0$",
    "dart-pmp\0$",
    "dockchannel-uart\0$",
    "pmp\0$",
    "stockholm\0$",
    "stockholm-spmi\0$",
    "bluetooth-pcie\0$",
    "wlan\0$",
#ifndef ENABLE_BASEBAND
    "baseband\0$",
    "baseband-spmi\0$",
    "baseband-vol\0$",
    "baseband-pcie\0$",
    "audio-baseband-voice\0$",
#endif
    // doing this won't^H^H^Hwill fix sepfw 17
    // should also work with 14beta5
    "Lynx\0$",
};

static const char* REM_DEV_TYPES[] = {
    "wlan\0$",
    "pmp\0$",
};

static const char* REM_PROPS[] = {
#ifndef ENABLE_DATA_ENCRYPTION
    "content-protect",
    "encryptable",
#endif
    "function-dock_parent",
    "function-error_handler",
    "function-mcc_ctrl",
    "function-mcc_dataset",
    "function-pmp_control",
    "mcc-power-gating",
    "nand-debug",
    "nvme-coastguard",    // FIXME: panic: "pmap_iommu_map failed\n"
    "pmp",
    "soc-tuning",
#ifndef ENABLE_BASEBAND
    "baseband-chipset",
    "has-baseband",
    "event_name-gpio9",
    "gps-capable",
    "location-reminders",
    "personal-hotspot",
    "bitrate-3g",
    "bitrate-lte",
    "bitrate-2g",
    "navigation",
#endif
    "pearl-camera",
    "face-detection-support",
    "siri-gesture",
#if 0
    "pci-l1pm-control", // TODO?
    "pci-aspm-default",
    "manual-enable-s2r",
    "pci-l1pm-control-postrom",
    "pci-l1pm-control-a0",
#endif
};

static const char* srawmemchr(const char* str, int chr)
{
    while (*str != chr) { str++; }

    return str;
}

static uint64_t sstrlen(const char* str) { return srawmemchr(str, '$') - str; }

static void apple_boot_process_dt_node(AppleDTNode* node, AppleDTNode* parent)
{
    GList*       iter  = NULL;
    AppleDTNode* child = NULL;
    AppleDTProp* prop  = NULL;
    uint64_t     i;
    bool         found;

    if ((prop = apple_dt_get_prop(node, "compatible")) != NULL) {
        assert_nonnull(prop->data);
        found = false;
        for (i = 0; i < ARRAY_SIZE(KEEP_COMP); i++) {
            if (memcmp(prop->data, KEEP_COMP[i], MIN(prop->len, sstrlen(KEEP_COMP[i]))) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            assert_nonnull(parent);
            DINFO("Removing node `%s` (non-whitelisted compatible property `%s`)",
                  apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal), (char*)prop->data);
            apple_dt_del_node(parent, node);
            return;
        }
    }

    if ((prop = apple_dt_get_prop(node, "name")) != NULL) {
        assert_nonnull(prop->data);
        for (i = 0; i < ARRAY_SIZE(REM_NAMES); i++) {
            uint64_t size = MIN(prop->len, sstrlen(REM_NAMES[i]));
            if (memcmp(prop->data, REM_NAMES[i], size) == 0) {
                assert_nonnull(parent);
                DINFO("Removing node `%s` (blacklisted name)", (char*)prop->data);
                apple_dt_del_node(parent, node);
                return;
            }
        }
    }

    if ((prop = apple_dt_get_prop(node, "device_type")) != NULL) {
        assert_nonnull(prop->data);
        for (i = 0; i < ARRAY_SIZE(REM_DEV_TYPES); i++) {
            uint64_t size = MIN(prop->len, sstrlen(REM_DEV_TYPES[i]));
            if (memcmp(prop->data, REM_DEV_TYPES[i], size) == 0) {
                assert_nonnull(parent);
                DINFO("Removing node `%s` (blacklisted device type `%s`)",
                      apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal), (char*)prop->data);
                apple_dt_del_node(parent, node);
                return;
            }
        }
    }

    for (i = 0; i < ARRAY_SIZE(REM_PROPS); i++) {
        if (apple_dt_del_prop_named(node, REM_PROPS[i])) {
            DINFO("Removing prop `%s` from node `%s`", REM_PROPS[i],
                  apple_dt_get_prop_str_or(node, "name", "(null)", &error_fatal));
        }
    }

    for (iter = node->children; iter != NULL;) {
        child = (AppleDTNode*)iter->data;

        // iter might get invalidated
        iter = iter->next;
        apple_boot_process_dt_node(child, node);
    }
}

/*
 * @param payload_type must be at least 4 bytes long
 */
static void extract_im4p_payload(const char* filename, char* payload_type, uint8_t** data, uint32_t* length,
                                 uint8_t** secure_monitor)
{
    uint8_t*  file_data;
    gsize     fsize;
    char      error_desc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    asn1_node img4_definitions = NULL;
    asn1_node im4p;
    int       ret;
    char      magic[4];
    char      description[128];
    int       len;
    uint8_t*  payload_data;

    if (!g_file_get_contents(filename, (gchar**)&file_data, &fsize, NULL)) {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
        return;
    }

    if (asn1_array2tree(img4_definitions_array, &img4_definitions, error_desc) != ASN1_SUCCESS) {
        error_setg(&error_fatal, "ASN.1 parser initialisation failed: `%s`", error_desc);
        return;
    }

    ret = asn1_create_element(img4_definitions, "Image4.IM4P", &im4p);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P element creation failed: %d", ret);
        return;
    }

    ret = asn1_der_decoding(&im4p, file_data, (uint32_t)fsize, error_desc);
    if (ret != ASN1_SUCCESS) {
        *data   = file_data;
        *length = (uint32_t)fsize;
        strncpy(payload_type, "raw", 4);
        asn1_delete_structure(&im4p);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    len = 4;
    ret = asn1_read_value(im4p, "magic", magic, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P magic read for `%s` failed: %d", filename, ret);
        return;
    }

    if (memcmp(magic, "IM4P", 4) != 0) {
        error_setg(&error_fatal, "`%s` is not an IM4P", filename);
        return;
    }

    len = 4;
    ret = asn1_read_value(im4p, "type", payload_type, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P type read for `%s` failed: %d", filename, ret);
        return;
    }

    len = 128;
    ret = asn1_read_value(im4p, "description", description, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P description read for `%s` failed: %d", filename, ret);
        return;
    }

    len = 0;
    ret = asn1_read_value(im4p, "data", NULL, &len);
    if (ret != ASN1_MEM_ERROR) {
        error_setg(&error_fatal, "IM4P size read for `%s` failed: %d", filename, ret);
        return;
    }

    payload_data = g_malloc0(len);
    ret          = asn1_read_value(im4p, "data", payload_data, &len);
    g_free(file_data);

    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IM4P read for `%s` failed: %d", filename, ret);
        return;
    }

    asn1_delete_structure(&im4p);
    asn1_delete_structure(&img4_definitions);

    if (memcmp(payload_data, "bvx", 3) == 0) {
        size_t   decode_buffer_size = len * 8;
        uint8_t* decode_buffer      = g_malloc0(decode_buffer_size);
        int      decoded_length     = lzfse_decode_buffer(decode_buffer, decode_buffer_size, payload_data, len, NULL);
        g_free(payload_data);

        if (decoded_length == 0 || decoded_length == decode_buffer_size) {
            error_setg(&error_fatal, "LZFSE decompression for `%s` failed; insufficient buffer size", filename);
            g_free(decode_buffer);
            return;
        }

        *data   = decode_buffer;
        *length = decoded_length;
        return;
    }

    if (memcmp(payload_data, "complzss", 8) == 0) {
        LzssCompHeader* comp_header       = (LzssCompHeader*)payload_data;
        size_t          uncompressed_size = be32_to_cpu(comp_header->uncompressed_size);
        size_t          compressed_size   = be32_to_cpu(comp_header->compressed_size);
        uint8_t*        decode_buffer     = g_malloc0(uncompressed_size);
        int             decoded_length    = decompress_lzss(decode_buffer, comp_header->data, compressed_size);
        if (decoded_length == 0 || decoded_length != uncompressed_size) {
            error_setg(&error_fatal, "LZSS decompression for `%s` failed", filename);
            g_free(decode_buffer);
            return;
        }

        size_t monitor_off = compressed_size + sizeof(LzssCompHeader);
        if (secure_monitor && monitor_off < len) {
            size_t monitor_size = len - monitor_off;
            DINFO("Found AP Secure Monitor in payload with size 0x%zX!", monitor_size);
            *secure_monitor = g_malloc0(monitor_size);
            memcpy(*secure_monitor, payload_data + monitor_off, monitor_size);
        }

        g_free(payload_data);

        *data   = decode_buffer;
        *length = decoded_length;
        return;
    }

    *data   = payload_data;
    *length = len;
}

AppleDTNode* apple_boot_load_dt_file(const char* filename)
{
    AppleDTNode* root      = NULL;
    uint8_t*     file_data = NULL;
    uint32_t     fsize;
    char         payload_type[4];

    if (filename == NULL) { return NULL; }

    extract_im4p_payload(filename, payload_type, &file_data, &fsize, NULL);

    if (memcmp(payload_type, "dtre", 4) != 0 && memcmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `dtre`)", filename, payload_type);
        return NULL;
    }

    root = apple_dt_deserialise(file_data);
    g_free(file_data);
    return root;
}

static void apple_boot_init_mem_ranges(AppleDTNode* root)
{
    AppleDTNode* child;

    child = apple_dt_get_node(root, "chosen/memory-map");
    assert_nonnull(child);

    apple_dt_set_prop(child, "RAMDisk", 16, NULL);
    apple_dt_set_prop(child, "TrustCache", 16, NULL);
    apple_dt_set_prop(child, "SEPFW", 16, NULL);
    apple_dt_set_prop(child, "BootArgs", 16, NULL);
    apple_dt_set_prop(child, "DeviceTree", 16, NULL);
}

void apple_boot_populate_dt(AppleDTNode* root, AppleBootInfo* info, bool auto_boot)
{
    AppleDTNode* child;
    AppleDTProp* prop;
    char*        boot_nonce_data;
    size_t       boot_nonce_len;
    uint8_t*     hash     = NULL;
    size_t       hash_len = 0;

    assert_cmphex(info->nvram_size, <=, XNU_MAX_NVRAM_SIZE);

    apple_dt_set_prop_null(root, "no-suspend");

    child = apple_dt_get_node(root, "chosen");
    assert_nonnull(child);

    prop = apple_dt_get_prop(child, "random-seed");
    assert_nonnull(prop);
    qemu_guest_getrandom_nofail(prop->data, prop->len);

    prop = apple_dt_get_prop(child, "boot-nonce");
    assert_nonnull(prop);
    boot_nonce_data = prop->data;
    boot_nonce_len  = prop->len;
    qemu_guest_getrandom_nofail(boot_nonce_data, boot_nonce_len);

    apple_dt_set_prop_u64(child, "dram-base", info->dram_base);
    apple_dt_set_prop_u64(child, "dram-size", info->dram_size);
    apple_dt_set_prop_str(child, "firmware-version", "ChefKiss Inferno");
    apple_dt_set_prop_str(child, "system-firmware-version", QEMU_VERSION);

    apple_dt_set_prop_u32(child, "nvram-total-size", info->nvram_size);
    apple_dt_set_prop_u32(child, "nvram-bank-size", info->nvram_size);
    apple_dt_set_prop(child, "nvram-proxy-data", info->nvram_size, info->nvram_data);

    // apple_dt_set_prop_u32(child, "research-enabled", 1);
    apple_dt_set_prop_u32(child, "effective-production-status-ap", 1);
    apple_dt_set_prop_u32(child, "effective-security-mode-ap", 1);
    apple_dt_set_prop_u32(child, "security-domain", 1);
    apple_dt_set_prop_u32(child, "chip-epoch", 1);
    // apple_dt_set_prop_u32(child, "debug-enabled", 1);

    // fstab os_env_type:
    // 0x01: fstab
    // 0x02: fstab-ephemeral-recovery-data
    // 0x03: fstab-ephemeral-diag-data
    // darwinos-security-environment:
    // 0x04: macos-darwinos-environment
    // 0x05: embedded-darwinos-environment
    // 0x06: trusted-darwinos-environment/"PrivateCloudOS detected"
    // TODO: Don't set this one manually, process osenvironments node.
    apple_dt_set_prop_u32(child, "darwinos-security-environment", auto_boot ? 1 : 2);
    // 7: Erase/darwinOS RAMDisk?
    // 3: Upgrade RAMDisk?
    // 1: Main OS?
    apple_dt_set_prop_u32(child, "boot-command", auto_boot ? 1 : 7);

    child = apple_dt_get_node(root, "defaults");
    assert_nonnull(child);

    // Enable FastSim for iOS 26
    apple_dt_set_prop_u32(child, "insecure_hpr", 1);

    child = apple_dt_get_node(root, "chosen/manifest-properties");
    assert_nonnull(child);
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, boot_nonce_data, boot_nonce_len, &hash, &hash_len, &error_fatal)
        >= 0)
    {
        apple_dt_set_prop(child, "BNCH", hash_len, hash);
        g_free(hash);
        hash     = NULL;
        hash_len = 0;
    }
    else {
        return;
    }

    child = apple_dt_get_node(root, "product");
    assert_nonnull(child);
    apple_dt_set_prop_u32(child, "allow-hactivation", 1);

    apple_boot_init_mem_ranges(root);

    apple_boot_process_dt_node(root, NULL);

    // Prevent further additions.
    info->device_tree_size = ROUND_UP_16K(apple_dt_finalise(root));
}

static void set_memory_range(AppleDTNode* root, const char* name, uint64_t addr, uint64_t size)
{
    AppleDTNode* child;
    AppleDTProp* prop;

    child = apple_dt_get_node(root, "chosen/memory-map");
    assert_nonnull(child);

    if (addr == 0) {
        assert_cmphex(size, ==, 0);
        apple_dt_del_prop_named(child, name);
        return;
    }

    prop = apple_dt_get_prop(child, name);
    assert_nonnull(prop);

    stq_le_p(prop->data, addr);
    stq_le_p(prop->data + sizeof(uint64_t), size);
}

void apple_boot_finalise_dt(AppleDTNode* root, AddressSpace* as, AppleBootInfo* info)
{
    uint8_t*        buf;
    QCryptoHashAlgo alg;
    uint8_t*        hash     = NULL;
    size_t          hash_len = 0;
    AppleDTNode*    child;
    const char*     crypto_hash_method;
    AppleDTProp*    prop;

    set_memory_range(root, "DeviceTree", info->device_tree_addr, info->device_tree_size);
    set_memory_range(root, "RAMDisk", info->ramdisk_addr, info->ramdisk_size);
    set_memory_range(root, "TrustCache", info->trustcache_addr, info->trustcache_size);
    set_memory_range(root, "SEPFW", info->sep_fw_addr, info->sep_fw_size);
    set_memory_range(root, "BootArgs", info->kern_boot_args_addr, info->kern_boot_args_size);

    if (info->ticket_data != NULL && info->ticket_length != 0) {
        child = apple_dt_get_node(root, "chosen");
        assert_nonnull(child);

        crypto_hash_method = apple_dt_get_prop_str_or(child, "crypto-hash-method", "sha1", &error_fatal);
        if (strcmp(crypto_hash_method, "sha2-384") == 0) { alg = QCRYPTO_HASH_ALGO_SHA384; }
        else if (strcmp(crypto_hash_method, "sha1") == 0) {
            alg = QCRYPTO_HASH_ALGO_SHA1;
        }
        else {
            assert_not_reached();
        }

        prop = apple_dt_get_prop(child, "boot-manifest-hash");
        assert_nonnull(prop);

        if (qcrypto_hash_bytes(alg, info->ticket_data, info->ticket_length, &hash, &hash_len, &error_fatal) >= 0) {
            assert_cmpuint(hash_len, ==, prop->len);
            memcpy(prop->data, hash, hash_len);
            g_free(hash);
            hash     = NULL;
            hash_len = 0;
        }
        else {
            return;
        }
    }

    buf = g_malloc0(info->device_tree_size);
    apple_dt_serialise(root, buf);

    address_space_rw(as, info->device_tree_addr, MEMTXATTRS_UNSPECIFIED, buf, info->device_tree_size, true);
    g_free(buf);
}

uint8_t* apple_boot_load_trustcache_file(const char* filename, uint64_t* size)
{
    uint32_t*           trustcache_data;
    uint64_t            trustcache_size;
    g_autofree uint8_t* file_data;
    unsigned long       file_size;
    uint32_t            length;
    char                payload_type[4];
    uint32_t            trustcache_version;
    uint32_t            trustcache_entry_count;
    uint32_t            expected_file_size;
    uint32_t            trustcache_entry_size;

    extract_im4p_payload(filename, payload_type, &file_data, &length, NULL);

    if (memcmp(payload_type, "trst", 4) != 0 && memcmp(payload_type, "rtsc", 4) != 0
        && memcmp(payload_type, "raw", 4) != 0)
    {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `trst`/`rtsc`)", filename, payload_type);
        return NULL;
    }

    file_size = (unsigned long)length;

    trustcache_size    = ROUND_UP_16K(file_size + 8);
    trustcache_data    = (uint32_t*)g_malloc0(trustcache_size);
    trustcache_data[0] = 1;    // #trustcaches
    trustcache_data[1] = 8;    // offset
    memcpy(&trustcache_data[2], file_data, file_size);

    // Validate the trustcache v1 header. The layout is:
    // uint32_t version
    // uuid (16 bytes)
    // uint32_t entry_count
    //
    // The cache is then followed by entry_count entries, each of which
    // contains a 20 byte hash and 2 additional bytes (hence is 22 bytes long)
    // for v1 and contains a 20 byte hash and 4 additional bytes (hence is 24
    // bytes long) for v2
    trustcache_version     = trustcache_data[2];
    trustcache_entry_count = trustcache_data[7];

    switch (trustcache_version) {
        case 1: trustcache_entry_size = 22; break;
        case 2: trustcache_entry_size = 24; break;
        default:
            error_setg(&error_fatal, "invalid trustcache header in `%s` (expected v1 or v2, got %d)", filename,
                       trustcache_version);
            return NULL;
    }

    // 24 is header size
    expected_file_size = 24 + trustcache_entry_count * trustcache_entry_size;

    assert_cmpuint(file_size, ==, expected_file_size);

    *size = trustcache_size;
    return (uint8_t*)trustcache_data;
}

void apple_boot_load_ramdisk(const char* filename, AddressSpace* as, hwaddr pa, uint64_t* size)
{
    uint8_t*      file_data = NULL;
    unsigned long file_size = 0;
    uint32_t      length    = 0;
    char          payload_type[4];

    extract_im4p_payload(filename, payload_type, &file_data, &length, NULL);
    if (memcmp(payload_type, "rdsk", 4) != 0 && memcmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `rdsk`)", filename, payload_type);
        return;
    }

    file_size = length;
    file_data = g_realloc(file_data, file_size);

    address_space_rw(as, pa, MEMTXATTRS_UNSPECIFIED, file_data, file_size, true);
    *size = file_size;
    g_free(file_data);
}

void apple_boot_load_raw_file(const char* filename, AddressSpace* as, hwaddr file_pa, uint64_t* size)
{
    uint8_t* file_data;
    gsize    sizef;

    if (g_file_get_contents(filename, (gchar**)&file_data, &sizef, NULL)) {
        *size = sizef;
        address_space_rw(as, file_pa, MEMTXATTRS_UNSPECIFIED, file_data, sizef, true);
        g_free(file_data);
    }
    else {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
    }
}

bool apple_boot_contains_boot_arg(const char* boot_args, const char* arg, bool match_prefix)
{
    g_autofree char* args = g_strdup(boot_args);
    char*            pos  = args;
    char*            token;
    size_t           arglen = strlen(arg);

    if (args == NULL) { return false; }

    while ((token = qemu_strsep(&pos, " ")) != NULL) {
        if ((match_prefix && strncmp(token, arg, arglen) == 0) || strcmp(token, arg) == 0) { return true; }
    }

    return false;
}

void apple_boot_setup_monitor_boot_args(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                        hwaddr mem_size, hwaddr kern_args, vaddr kern_entry, hwaddr kern_phys_base,
                                        hwaddr kern_phys_slide, vaddr kern_virt_slide, vaddr kern_text_section_off)
{
    AppleMonitorBootArgs args = {0};

    args.version               = 4;
    args.virt_base             = virt_base;
    args.phys_base             = phys_base;
    args.mem_size              = mem_size;
    args.kern_args             = kern_args;
    args.kern_entry            = kern_entry;
    args.kern_phys_base        = kern_phys_base;
    args.kern_phys_slide       = kern_phys_slide;
    args.kern_virt_slide       = kern_virt_slide;
    args.kern_text_section_off = kern_text_section_off;
    qemu_guest_getrandom_nofail(args.random_bytes, sizeof(args.random_bytes));

    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

static void apple_boot_setup_bootargs_rev2(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                           hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                                           AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual)
{
    AppleKernelBootArgsRev2 args = {0};

    args.revision           = 2;
    args.version            = 2;
    args.virt_base          = virt_base;
    args.phys_base          = phys_base;
    args.mem_size           = mem_size;
    args.kernel_top         = kernel_top;
    args.video_args         = *video_args;
    args.device_tree_ptr    = dtb_va;
    args.device_tree_length = dtb_size;
    if (cmdline != NULL) { g_strlcpy(args.cmdline, cmdline, sizeof(args.cmdline)); }
    args.boot_flags      = 0;
    args.mem_size_actual = mem_size_actual;

    // iOS 13: mem_size_actual is not a thing
    // however, the boot args region should always
    // be large enough (one page).
    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

static void apple_boot_setup_bootargs_rev3(AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                                           hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                                           AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual)
{
    AppleKernelBootArgsRev3 args = {0};

    args.revision           = 3;
    args.version            = 2;
    args.virt_base          = virt_base;
    args.phys_base          = phys_base;
    args.mem_size           = mem_size;
    args.kernel_top         = kernel_top;
    args.video_args         = *video_args;
    args.device_tree_ptr    = dtb_va;
    args.device_tree_length = dtb_size;
    if (cmdline != NULL) { g_strlcpy(args.cmdline, cmdline, sizeof(args.cmdline)); }
    args.boot_flags      = 0;
    args.mem_size_actual = mem_size_actual;

    address_space_rw(as, addr, MEMTXATTRS_UNSPECIFIED, &args, sizeof(args), true);
}

void apple_boot_setup_bootargs(uint32_t build_version, AddressSpace* as, hwaddr addr, vaddr virt_base, hwaddr phys_base,
                               hwaddr mem_size, hwaddr kernel_top, vaddr dtb_va, vaddr dtb_size,
                               AppleVideoArgs* video_args, const char* cmdline, hwaddr mem_size_actual)
{
    if (BUILD_VERSION_MAJOR(build_version) >= 17) {
        apple_boot_setup_bootargs_rev3(as, addr, virt_base, phys_base, mem_size, kernel_top, dtb_va, dtb_size,
                                       video_args, cmdline, mem_size_actual);
    }
    else {
        apple_boot_setup_bootargs_rev2(as, addr, virt_base, phys_base, mem_size, kernel_top, dtb_va, dtb_size,
                                       video_args, cmdline, mem_size_actual);
    }
}

void apple_boot_get_kc_bounds(MachoHeader64* header, vaddr* text_base, vaddr* kc_base, vaddr* kc_end, vaddr* ro_lower,
                              vaddr* ro_upper)
{
    MachoLoadCommand* cmd      = (MachoLoadCommand*)(header + 1);
    vaddr        text_base_cur = -1ULL, kc_base_cur = -1ULL, kc_end_cur = 0, ro_lower_cur = -1ULL, ro_upper_cur = 0;
    unsigned int i;

    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }

        MachoSegmentCommand64* seg_cmd = (MachoSegmentCommand64*)cmd;

        if (strncmp(seg_cmd->segname, "__PAGEZERO", 10) == 0 || seg_cmd->vmsize == 0) { continue; }

        if (seg_cmd->filesize != 0 && seg_cmd->fileoff == 0) { text_base_cur = seg_cmd->vmaddr; }

        if (seg_cmd->vmaddr < kc_base_cur) { kc_base_cur = seg_cmd->vmaddr; }

        if (seg_cmd->vmaddr + seg_cmd->vmsize > kc_end_cur) { kc_end_cur = seg_cmd->vmaddr + seg_cmd->vmsize; }

        if ((seg_cmd->maxprot & VM_PROT_WRITE) != 0 || strncmp(seg_cmd->segname, "__LINKEDIT", 10) == 0) { continue; }

        if (ro_lower_cur >= seg_cmd->vmaddr) { ro_lower_cur = seg_cmd->vmaddr; }

        hwaddr vmend = seg_cmd->vmaddr + seg_cmd->vmsize;
        if (ro_upper_cur <= vmend) { ro_upper_cur = vmend; }
    }

    assert_cmphex(text_base_cur, !=, 0);
    assert_cmphex(text_base_cur, !=, -1ULL);
    assert_cmphex(kc_base_cur, !=, 0);
    assert_cmphex(kc_base_cur, !=, -1ULL);
    assert_cmphex(kc_end_cur, !=, 0);
    assert_cmphex(ro_lower_cur, !=, 0);
    assert_cmphex(ro_lower_cur, !=, -1ULL);
    assert_cmphex(ro_upper_cur, !=, 0);

    if (text_base != NULL) { *text_base = text_base_cur; }

    if (kc_base != NULL) { *kc_base = kc_base_cur & -0x2000000ull; }

    if (kc_end != NULL) { *kc_end = kc_end_cur; }

    if (ro_lower != NULL) { *ro_lower = ro_lower_cur; }

    if (ro_upper != NULL) { *ro_upper = ro_upper_cur; }
}

MachoHeader64* apple_boot_load_kernel(const char* filename, MachoHeader64** secure_monitor)
{
    uint32_t       len;
    uint8_t*       data = NULL;
    char           payload_type[4];
    MachoHeader64* header;

    extract_im4p_payload(filename, payload_type, &data, &len, (uint8_t**)secure_monitor);

    if (memcmp(payload_type, "krnl", 4) != 0 && memcmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `krnl`)", filename, payload_type);
        return NULL;
    }

    header = apple_boot_parse_macho(data, len);
    g_free(data);
    return header;
}

MachoHeader64* apple_boot_parse_macho(uint8_t* data, uint32_t len)
{
    uint8_t*          phys_base;
    MachoHeader64*    header;
    MachoLoadCommand* cmd;
    vaddr             text_base;
    vaddr             kc_base;
    vaddr             kc_end;
    int               i;

    header = (MachoHeader64*)data;
    assert_cmphex(header->magic, ==, MACH_MAGIC_64);

    apple_boot_get_kc_bounds(header, &text_base, &kc_base, &kc_end, NULL, NULL);
    assert_cmphex(kc_base, <, kc_end);

    phys_base = g_malloc0(kc_end - kc_base);

    cmd = (MachoLoadCommand*)(header + 1);
    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }

        MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;
        if (strncmp(segCmd->segname, "__PAGEZERO", 10) == 0 || segCmd->vmsize == 0) { continue; }
        assert_cmphex(segCmd->fileoff, <, len);
        memcpy(phys_base + (segCmd->vmaddr - kc_base), data + segCmd->fileoff, segCmd->filesize);
    }

    return (MachoHeader64*)(phys_base + text_base - kc_base);
}

uint32_t apple_boot_build_version(MachoHeader64* header)
{
    MachoLoadCommand* cmd;
    int               index;

    if (header->file_type == MH_FILESET) { header = apple_boot_get_fileset_header(header, "com.apple.kernel"); }
    cmd = (MachoLoadCommand*)(header + 1);

    for (index = 0; index < header->n_cmds; index++) {
        switch (cmd->cmd) {
            case LC_BUILD_VERSION: {
                return ((MachoBuildVersionCommand*)cmd)->sdk;
            }

            default: break;
        }

        cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size);
    }
    return 0;
}

uint32_t apple_boot_platform(MachoHeader64* header)
{
    MachoLoadCommand* cmd;
    uint32_t          i;

    if (header->file_type == MH_FILESET) { header = apple_boot_get_fileset_header(header, "com.apple.kernel"); }

    cmd = (MachoLoadCommand*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd == LC_BUILD_VERSION) {
            MachoBuildVersionCommand* buildVerCmd = (MachoBuildVersionCommand*)cmd;
            return buildVerCmd->platform;
        }
    }

    return 0;
}

const char* apple_boot_platform_string(MachoHeader64* header)
{
    switch (apple_boot_platform(header)) {
        case PLATFORM_MACOS               : return "macOS";
        case PLATFORM_IOS                 : return "iOS";
        case PLATFORM_TVOS                : return "tvOS";
        case PLATFORM_WATCHOS             : return "watchOS";
        case PLATFORM_BRIDGEOS            : return "bridgeOS";
        case PLATFORM_MAC_CATALYST        : return "Mac Catalyst";
        case PLATFORM_IOS_SIMULATOR       : return "iOS Simulator";
        case PLATFORM_TVOS_SIMULATOR      : return "tvOS Simulator";
        case PLATFORM_WATCHOS_SIMULATOR   : return "watchOS Simulator";
        case PLATFORM_DRIVERKIT           : return "DriverKit";
        case PLATFORM_VISIONOS            : return "visionOS";
        case PLATFORM_VISIONOS_SIMULATOR  : return "visionOS Simulator";
        case PLATFORM_FIRMWARE            : return "Firmware";
        case PLATFORM_SEPOS               : return "sepOS";
        case PLATFORM_MACOS_EXCLAVECORE   : return "macOS ExclaveCore";
        case PLATFORM_MACOS_EXCLAVEKIT    : return "macOS ExclaveKit";
        case PLATFORM_IOS_EXCLAVECORE     : return "iOS ExclaveCore";
        case PLATFORM_IOS_EXCLAVEKIT      : return "iOS ExclaveKit";
        case PLATFORM_TVOS_EXCLAVECORE    : return "tvOS ExclaveCore";
        case PLATFORM_TVOS_EXCLAVEKIT     : return "tvOS ExclaveKit";
        case PLATFORM_WATCHOS_EXCLAVECORE : return "watchOS ExclaveCore";
        case PLATFORM_WATCHOS_EXCLAVEKIT  : return "watchOS ExclaveKit";
        case PLATFORM_VISIONOS_EXCLAVECORE: return "visionOS ExclaveCore";
        case PLATFORM_VISIONOS_EXCLAVEKIT : return "visionOS ExclaveKit";
        default                           : return "Unknown";
    }
}

static MachoSegmentCommand64* apple_boot_get_first_seg(MachoHeader64* header)
{
    MachoSegmentCommand64* sgp;
    uint32_t               i;

    sgp = (MachoSegmentCommand64*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, sgp = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size)) {
        if (sgp->cmd == LC_SEGMENT_64) { return sgp; }
    }

    // not found
    return NULL;
}

static MachoSegmentCommand64* apple_boot_get_next_seg(MachoHeader64* header, MachoSegmentCommand64* seg)
{
    MachoSegmentCommand64* sgp;
    uint32_t               i;
    bool                   found = false;

    sgp = (MachoSegmentCommand64*)(header + 1);

    for (i = 0; i < header->n_cmds; i++, sgp = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size)) {
        if (found && sgp->cmd == LC_SEGMENT_64) { return sgp; }
        if (seg == sgp) { found = true; }
    }

    // not found
    return NULL;
}

static MachoSection64* apple_boot_first_sect(MachoSegmentCommand64* seg) { return (MachoSection64*)(seg + 1); }

static MachoSection64* apple_boot_next_sect(MachoSection64* sp) { return sp + 1; }

static MachoSection64* apple_boot_end_sect(MachoSegmentCommand64* seg)
{
    MachoSection64* sp;

    sp = (MachoSection64*)(seg + 1);
    return &sp[seg->nsects];
}

static void apple_boot_process_symbols(MachoHeader64* header, uint64_t slide)
{
    MachoLoadCommand*      cmd;
    uint8_t*               data;
    vaddr                  text_base;
    vaddr                  kernel_low;
    vaddr                  kernel_high;
    uint32_t               index;
    uint8_t*               base;
    MachoSegmentCommand64* linkedit_seg;
    MachoNList64*          sym;
    uint32_t               off;

    if (slide == 0) { return; }

    apple_boot_get_kc_bounds(header, &text_base, &kernel_low, &kernel_high, NULL, NULL);

    data         = apple_boot_get_macho_buffer(header);
    linkedit_seg = apple_boot_get_segment(header, "__LINKEDIT");

    cmd = (MachoLoadCommand*)(header + 1);
    for (index = 0; index < header->n_cmds; index++) {
        switch (cmd->cmd) {
            case LC_SYMTAB: {
                MachoSymtabCommand* symtab = (MachoSymtabCommand*)cmd;
                if (linkedit_seg == NULL) {
                    error_report("Did not find __LINKEDIT segment");
                    return;
                }
                base = data + (linkedit_seg->vmaddr - kernel_low);
                off  = linkedit_seg->fileoff;
                sym  = (MachoNList64*)(base + (symtab->sym_off - off));
                for (int i = 0; i < symtab->nsyms; i++) {
                    if (sym[i].n_type & N_STAB) { continue; }
                    sym[i].n_value += slide;
                }
                break;
            }
            case LC_DYSYMTAB: {
                MachoDysymtabCommand* dysymtab = (MachoDysymtabCommand*)cmd;
                if (!dysymtab->loc_rel_n) { break; }

                if (linkedit_seg == NULL) {
                    error_report("Did not find __LINKEDIT segment");
                    return;
                }

                base = data + (linkedit_seg->vmaddr - kernel_low);
                off  = linkedit_seg->fileoff;
                for (size_t i = 0; i < dysymtab->loc_rel_n; i++) {
                    int32_t r_address = *(int32_t*)(base + (dysymtab->loc_rel_off - off) + (i * 8));
                    *(uint64_t*)(data + ((text_base - kernel_low) + r_address)) += slide;
                }
                break;
            }
            default: break;
        }
        cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size);
    }
}

void apple_boot_allocate_segment_records(AppleDTNode* memory_map, MachoHeader64* header)
{
    unsigned int      index;
    MachoLoadCommand* cmd;

    cmd = (MachoLoadCommand*)((char*)header + sizeof(MachoHeader64));
    for (index = 0; index < header->n_cmds; index++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd != LC_SEGMENT_64) { continue; }
        MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;
        char                   region_name[32];

        snprintf(region_name, sizeof(region_name), "Kernel-%s", segCmd->segname);
        struct MemoryMapFileInfo
        {
            uint64_t paddr;
            uint64_t length;
        } file_info = {0};
        apple_dt_set_prop(memory_map, region_name, sizeof(file_info), &file_info);
    }
}

vaddr apple_boot_load_macho(MachoHeader64* header, AddressSpace* as, AppleDTNode* memory_map, hwaddr phys_base,
                            vaddr virt_slide)
{
    uint8_t*          data = NULL;
    unsigned int      i;
    MachoLoadCommand* cmd;
    hwaddr            pc = 0;
    data                 = apple_boot_get_macho_buffer(header);
    vaddr          kc_base;
    vaddr          kc_end;
    bool           is_fileset = header->file_type == MH_FILESET;
    MachoHeader64* header2    = NULL;
    uint8_t*       load_from2 = NULL;

    apple_boot_get_kc_bounds(header, NULL, &kc_base, &kc_end, NULL, NULL);

    cmd = (MachoLoadCommand*)(header + 1);
    if (!is_fileset) { apple_boot_process_symbols(header, virt_slide); }
    for (i = 0; i < header->n_cmds; i++, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        switch (cmd->cmd) {
            case LC_SEGMENT_64: {
                MachoSegmentCommand64* segCmd = (MachoSegmentCommand64*)cmd;

                if (strncmp(segCmd->segname, "__PAGEZERO", 10) == 0) { continue; }

                char   region_name[64];
                void*  load_from = (void*)(data + segCmd->vmaddr - kc_base);
                hwaddr load_to   = (phys_base + segCmd->vmaddr - kc_base);

                if (memory_map) {
                    snprintf(region_name, sizeof(region_name), "Kernel-%s", segCmd->segname);
                    struct MemoryMapFileInfo
                    {
                        uint64_t paddr;
                        uint64_t length;
                    } file_info = {load_to, segCmd->vmsize};
                    apple_dt_set_prop(memory_map, region_name, sizeof(file_info), &file_info);
                }
                else {
                    snprintf(region_name, sizeof(region_name), "TZ1-%s", segCmd->segname);
                }

                if (segCmd->vmsize == 0) { break; }

                if (!is_fileset) {
                    MachoSection64* sp;
                    for (sp = apple_boot_first_sect(segCmd); sp != apple_boot_end_sect(segCmd);
                         sp = apple_boot_next_sect(sp))
                    {
                        if ((sp->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                            load_from2 = (uint8_t*)(data + sp->addr - kc_base);
                            void** nl_symbol_ptr;
                            for (nl_symbol_ptr = (void**)load_from2; nl_symbol_ptr < (void**)(load_from2 + sp->size);
                                 nl_symbol_ptr++)
                            {
                                *nl_symbol_ptr += virt_slide;
                            }
                        }
                    }
                }

                if (!is_fileset) {
                    if (strcmp(segCmd->segname, "__TEXT") == 0) {
                        header2 = load_from;
                        MachoSegmentCommand64* seg;
                        assert_cmphex(header2->magic, ==, MACH_MAGIC_64);
                        for (seg = apple_boot_get_first_seg(header2); seg != NULL;
                             seg = apple_boot_get_next_seg(header2, seg))
                        {
                            MachoSection64* sp;
                            seg->vmaddr += virt_slide;
                            for (sp = apple_boot_first_sect(seg); sp != apple_boot_end_sect(seg);
                                 sp = apple_boot_next_sect(sp))
                            {
                                sp->addr += virt_slide;
                            }
                        }
                    }
                }

                DINFO("Loading %s to 0x%" PRIx64 " (filesize: 0x%" PRIx64 " vmsize: 0x%" PRIx64 ")", region_name,
                      load_to, segCmd->filesize, segCmd->vmsize);
                uint8_t* buf = g_malloc0(segCmd->vmsize);
                memcpy(buf, load_from, segCmd->filesize);
                address_space_rw(as, load_to, MEMTXATTRS_UNSPECIFIED, buf, segCmd->vmsize, true);
                g_free(buf);

                if (!is_fileset) {
                    if (strcmp(segCmd->segname, "__TEXT") == 0) {
                        header2 = load_from;
                        MachoSegmentCommand64* seg;
                        assert_cmphex(header2->magic, ==, MACH_MAGIC_64);
                        for (seg = apple_boot_get_first_seg(header2); seg != NULL;
                             seg = apple_boot_get_next_seg(header2, seg))
                        {
                            MachoSection64* sp;
                            for (sp = apple_boot_first_sect(seg); sp != apple_boot_end_sect(seg);
                                 sp = apple_boot_next_sect(sp))
                            {
                                sp->addr -= virt_slide;
                            }
                            // probably won't change anything, it's just for the
                            // symmetry
                            seg->vmaddr -= virt_slide;
                        }
                    }
                }

                if (!is_fileset) {
                    MachoSection64* sp;
                    for (sp = apple_boot_first_sect(segCmd); sp != apple_boot_end_sect(segCmd);
                         sp = apple_boot_next_sect(sp))
                    {
                        if ((sp->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                            load_from2 = (uint8_t*)(data + sp->addr - kc_base);
                            void** nl_symbol_ptr;
                            for (nl_symbol_ptr = (void**)load_from2; nl_symbol_ptr < (void**)(load_from2 + sp->size);
                                 nl_symbol_ptr++)
                            {
                                *nl_symbol_ptr -= virt_slide;
                            }
                        }
                    }
                }
                break;
            }
            case LC_UNIXTHREAD: {
                // grab just the entry point PC
                uint64_t* ptrPc = (uint64_t*)((char*)cmd + 0x110);

                // 0x110 for arm64 only.
                pc = vtop_bases(*ptrPc, phys_base, kc_base);

                break;
            }
            default: {
                break;
            }
        }
    }

    if (!is_fileset) { apple_boot_process_symbols(header, -virt_slide); }

    return pc;
}

uint8_t* apple_boot_get_macho_buffer(MachoHeader64* header)
{
    vaddr text_base, kc_base;

    apple_boot_get_kc_bounds(header, &text_base, &kc_base, NULL, NULL, NULL);

    return (uint8_t*)header - text_base + kc_base;
}

MachoFilesetEntryCommand* apple_boot_get_fileset(MachoHeader64* header, const char* entry)
{
    if (header->file_type != MH_FILESET) { return NULL; }

    MachoFilesetEntryCommand* fileset;
    fileset = (MachoFilesetEntryCommand*)((char*)header + sizeof(MachoHeader64));

    for (uint32_t i = 0; i < header->n_cmds;
         i++, fileset = (MachoFilesetEntryCommand*)((char*)fileset + fileset->cmd_size))
    {
        if (fileset->cmd == LC_FILESET_ENTRY) {
            const char* entry_id = (char*)fileset + fileset->entry_id;
            if (strcmp(entry_id, entry) == 0) { return fileset; }
        }
    }

    return NULL;
}

MachoHeader64* apple_boot_get_fileset_header(MachoHeader64* header, const char* entry)
{
    MachoFilesetEntryCommand* fileset = apple_boot_get_fileset(header, entry);
    if (fileset == NULL) { return NULL; }
    return (MachoHeader64*)((char*)header + fileset->file_off);
}

MachoSegmentCommand64* apple_boot_get_segment(MachoHeader64* header, const char* name)
{
    uint32_t               i;
    MachoSegmentCommand64* sgp;

    if (header->file_type == MH_FILESET) {
        return apple_boot_get_segment(apple_boot_get_fileset_header(header, "com.apple.kernel"), name);
    }

    for (sgp = (MachoSegmentCommand64*)(header + 1), i = 0; i < header->n_cmds;
         i++, sgp                                      = (MachoSegmentCommand64*)((char*)sgp + sgp->cmd_size))
    {
        if (sgp->cmd == LC_SEGMENT_64 && strncmp(sgp->segname, name, sizeof(sgp->segname) - 1) == 0) { return sgp; }
    }

    return NULL;
}

MachoSection64* apple_boot_get_section(MachoSegmentCommand64* segment, const char* name)
{
    MachoSection64* sp;
    uint32_t        i;

    for (sp = (MachoSection64*)(segment + 1), i = 0; i < segment->nsects; i++, sp++) {
        if (strncmp(sp->sect_name, name, sizeof(sp->sect_name) - 1) == 0) { return sp; }
    }

    return NULL;
}

vaddr apple_boot_fixup_slide_va(vaddr va) { return (0xFFFF000000000000 | va) + g_virt_slide; }

void* apple_boot_va_to_ptr(vaddr va) { return (void*)(apple_boot_fixup_slide_va(va) - g_virt_base + g_phys_base); }

void apple_boot_extract_manifest(const uint8_t* img4_data, uint64_t img4_data_len_max, gchar** out_data,
                                 gsize* out_size)
{
    asn1_node img4_definitions = NULL;
    asn1_node img4;
    char      error_desc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    int       ret;
    char      magic[4];
    int       len;

    *out_data = NULL;
    *out_size = 0;

    if (asn1_array2tree(img4_definitions_array, &img4_definitions, error_desc) != ASN1_SUCCESS) {
        error_setg(&error_fatal, "ASN.1 parser initialisation failed: `%s`", error_desc);
        return;
    }

    ret = asn1_create_element(img4_definitions, "Image4.IMG4", &img4);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "IMG4 element creation failed: %d", ret);
        return;
    }

    unsigned char cls;
    int           tag_len;
    unsigned long tag_num;
    ret = asn1_get_tag_der(img4_data, (int)img4_data_len_max, &cls, &tag_len, &tag_num);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int  len_len;
    long content_len = asn1_get_length_der(img4_data + tag_len, (int)(img4_data_len_max - tag_len), &len_len);
    if (content_len < 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    uint64_t total_len = (uint64_t)tag_len + (uint64_t)len_len + (uint64_t)content_len;
    if (total_len > img4_data_len_max) { return; }

    ret = asn1_der_decoding(&img4, img4_data, (uint32_t)total_len, error_desc);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    len = 4;
    ret = asn1_read_value(img4, "magic", magic, &len);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    if (memcmp(magic, "IMG4", 4) != 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int start, end;
    ret = asn1_der_decoding_startEnd(img4, img4_data, total_len, "manifest", &start, &end);
    if (ret != ASN1_SUCCESS || start < 0 || end < 0 || start == end) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int           wrapper_tag_len;
    unsigned long wrapper_tag_num;
    ret = asn1_get_tag_der(img4_data + start, (int)(total_len - start), &cls, &wrapper_tag_len, &wrapper_tag_num);
    if (ret != ASN1_SUCCESS) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int  wrapper_len_len;
    long inner_len = asn1_get_length_der(img4_data + start + wrapper_tag_len,
                                         (int)(total_len - start - wrapper_tag_len), &wrapper_len_len);
    if (inner_len < 0) {
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    int inner_start = start + wrapper_tag_len + wrapper_len_len;

    *out_size = inner_len;
    *out_data = g_malloc(*out_size);
    memcpy(*out_data, img4_data + inner_start, *out_size);

    asn1_delete_structure(&img4);
    asn1_delete_structure(&img4_definitions);
}
