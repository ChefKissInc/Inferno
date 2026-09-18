/*
 * Apple SEP Secure Storage Component.
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
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/sep/private.h"
#include "hw/i2c/apple_i2c.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "hw/qdev-properties-system.h"
#include "system/runstate.h"
#include <nettle/ccm.h>
#include <nettle/cmac.h>
#include <nettle/ecc-curve.h>
#include <nettle/ecdsa.h>
#include <nettle/hkdf.h>
#include <nettle/hmac.h>
#include <nettle/knuth-lfib.h>

#define KBKDF_CMAC_OUTPUT_LEN   (0x48)
#define AES_CCM_NONCE_LENGTH    (12)
#define AES_CCM_AUTH_LENGTH     (8)
#define AES_CCM_TAG_LENGTH      (0x10)
#define AES_CCM_COUNTER_LENGTH  (4)
#define AES_CCM_MAX_DATA_LENGTH (0x54)
#define MSG_PREFIX_LENGTH       (4)

#define KBKDF_KEY_SEED_OFFSET         (0x00)
#define KBKDF_KEY_REQUEST_KEY_OFFSET  (0x08)
#define KBKDF_KEY_RESPONSE_KEY_OFFSET (0x28)
#define KBKDF_KEY_SEED_LENGTH         (8)
#define KBKDF_KEY_KEY_LENGTH          (0x20)
#define KBKDF_KEY_MAX_SLOTS           (0x49)
// 0x100 (0x00 .. 0xff) might be needed for >= iOS 17
// #define KBKDF_KEY_MAX_SLOTS (0x100)
#define KBKDF_KEY_KEY_FILE_OFFSET (0x100)    // 0x100*4*0x40 // store mac_keys after that

#define KBKDF_CMAC_LENGTH_SIZE  (2)
#define KBKDF_CMAC_LABEL_SIZE   (0x10)
#define KBKDF_CMAC_CONTEXT_SIZE MSG_PREFIX_LENGTH

#define CMD_METADATA_READ_REQUEST_ENCRYPTED_LENGTH (0x10)
#define CMD_METADATA_PAYLOAD_LENGTH                (0x20)
#define CMD_METADATA_DATA_PAYLOAD_LENGTH           (0x40)

#define SSC_MAX_REQUEST_SIZE  (0x84)
#define SSC_MAX_RESPONSE_SIZE (0xC4)

#define SECP384_PUBLIC_XY_SIZE (SHA384_DIGEST_SIZE * 2)

#define SSC_REQUEST_MAX_COPIES 4    // 0 .. 3

#define SSC_STORE_KEYS_OFFSET (KBKDF_KEY_KEY_FILE_OFFSET * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
#define SSC_STORE_SIZE        (SSC_STORE_KEYS_OFFSET + KBKDF_KEY_MAX_SLOTS * KBKDF_KEY_KEY_LENGTH)

#define SSC_RESPONSE_FLAG_COMMAND_SIZE_MISMATCH    0x02
#define SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID 0x04
#define SSC_RESPONSE_FLAG_KEYSLOT_INVALID          0x08
#define SSC_RESPONSE_FLAG_CMAC_INVALID             0x10
#define SSC_RESPONSE_FLAG_CURVE_INVALID            0x20
#define SSC_RESPONSE_FLAG_OK                       0x80

struct AppleSEPSSCState
{
    I2CSlave parent_obj;

    BlockBackend*       blk;
    uint8_t*            store;
    QEMUBH*             flush_bh;
    VMChangeStateEntry* vmstate_entry;
    QEMUIOVector        flush_qiov;
    uint64_t            store_blk_len;
    bool                store_dirty;
    bool                flush_in_flight;
    uint32_t            req_cur;
    uint32_t            resp_cur;
    uint8_t             req_cmd[0x100];
    uint8_t             resp_cmd[0x100];

    AppleSEP*             sep;
    struct ecc_scalar     ecc_key_main, ecc_keys[KBKDF_KEY_MAX_SLOTS];
    struct knuth_lfib_ctx rctx;
    uint8_t               random_hmac_key[SHA256_DIGEST_SIZE];
    uint8_t               slot_hmac_key[KBKDF_KEY_MAX_SLOTS][SHA256_DIGEST_SIZE];
    uint8_t               kbkdf_keys[KBKDF_KEY_MAX_SLOTS][KBKDF_CMAC_OUTPUT_LEN];
    uint32_t              kbkdf_counter[KBKDF_KEY_MAX_SLOTS];
    uint8_t               cpsn[0x07];
};

static bool ssc_store_range_valid(uint64_t offset, uint64_t len)
{
    if (offset > SSC_STORE_SIZE || len > SSC_STORE_SIZE - offset) {
        qemu_log_mask(LOG_GUEST_ERROR, "apple_sep_ssc: store access 0x%" PRIx64 "+0x%" PRIx64 " out of bounds\n",
                      offset, len);
        return false;
    }
    return true;
}

static void ssc_store_read(AppleSEPSSCState* ssc_state, uint64_t offset, uint64_t len, void* buf)
{
    if (!ssc_store_range_valid(offset, len)) {
        memset(buf, 0, len);
        return;
    }
    memcpy(buf, ssc_state->store + offset, len);
}

static void ssc_store_write(AppleSEPSSCState* ssc_state, uint64_t offset, uint64_t len, const void* buf)
{
    if (!ssc_store_range_valid(offset, len)) { return; }
    memcpy(ssc_state->store + offset, buf, len);

    ssc_state->store_dirty = true;
    if (ssc_state->blk) { qemu_bh_schedule(ssc_state->flush_bh); }
}

static void ssc_flush_done(void* opaque, int ret)
{
    AppleSEPSSCState* ssc_state = opaque;

    ssc_state->flush_in_flight = false;
    if (ret < 0) { qemu_log_mask(LOG_GUEST_ERROR, "apple_sep_ssc: store writeback failed: %s\n", strerror(-ret)); }

    /* Written again while the flush was in flight. */
    if (ssc_state->store_dirty) { qemu_bh_schedule(ssc_state->flush_bh); }
}

static void ssc_flush_sync(AppleSEPSSCState* ssc_state)
{
    int ret;

    if (ssc_state->blk == NULL || !ssc_state->store_dirty) { return; }

    ssc_state->store_dirty = false;
    ret                    = blk_pwrite(ssc_state->blk, 0, ssc_state->store_blk_len, ssc_state->store, 0);
    if (ret < 0) { qemu_log_mask(LOG_GUEST_ERROR, "apple_sep_ssc: store writeback failed: %s\n", strerror(-ret)); }
}

static void ssc_vm_state_change(void* opaque, bool running, RunState state)
{
    if (!running) { ssc_flush_sync(opaque); }
}

static void ssc_flush_bh(void* opaque)
{
    AppleSEPSSCState* ssc_state = opaque;

    if (ssc_state->flush_in_flight || !ssc_state->store_dirty) { return; }

    ssc_state->store_dirty     = false;
    ssc_state->flush_in_flight = true;
    qemu_iovec_init_buf(&ssc_state->flush_qiov, ssc_state->store, ssc_state->store_blk_len);
    blk_aio_pwritev(ssc_state->blk, 0, &ssc_state->flush_qiov, 0, ssc_flush_done, ssc_state);
}

static int apple_sep_ssc_event(I2CSlave* s, enum i2c_event event)
{
    AppleSEPSSCState* ssc = container_of(s, AppleSEPSSCState, parent_obj);

    switch (event) {
        case I2C_START_SEND: DPRINTF("apple_sep_ssc_event: I2C_START_SEND\n"); break;
        case I2C_FINISH    : DPRINTF("apple_sep_ssc_event: I2C_FINISH\n");
#if 1
            // hopefully this works against "sw timeout 1"
            apple_a7iop_interrupt_status_push(ssc->sep->mailbox,
                                              0x10002);    // I2C
#endif
            break;
        case I2C_START_RECV: DPRINTF("apple_sep_ssc_event: I2C_START_RECV\n"); break;
        case I2C_NACK      : DPRINTF("apple_sep_ssc_event: I2C_NACK\n"); break;
        default            : return -1;
    }
    return 0;
}

#define SSC_REQUEST_SIZE_CMD_0x0 (0x84)
#define SSC_REQUEST_SIZE_CMD_0x1 (0x74)
#define SSC_REQUEST_SIZE_CMD_0x2 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x3 (0x34)
#define SSC_REQUEST_SIZE_CMD_0x4 (0x14)
#define SSC_REQUEST_SIZE_CMD_0x5 (0x54)
#define SSC_REQUEST_SIZE_CMD_0x6 (0x14)
#define SSC_REQUEST_SIZE_CMD_0x7 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x8 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x9 (0x4)

#define SSC_RESPONSE_SIZE_CMD_0x0 (0xC4)
#define SSC_RESPONSE_SIZE_CMD_0x1 (0x74)
#define SSC_RESPONSE_SIZE_CMD_0x2 (0x4)
#define SSC_RESPONSE_SIZE_CMD_0x3 (0x14)
#define SSC_RESPONSE_SIZE_CMD_0x4 (0x54)
#define SSC_RESPONSE_SIZE_CMD_0x5 (0x14)
#define SSC_RESPONSE_SIZE_CMD_0x6 (0x34)
#define SSC_RESPONSE_SIZE_CMD_0x7 (0x78)
#define SSC_RESPONSE_SIZE_CMD_0x8 (0x4)
#define SSC_RESPONSE_SIZE_CMD_0x9 (0x2F)

static uint8_t ssc_request_sizes[] = {SSC_REQUEST_SIZE_CMD_0x0, SSC_REQUEST_SIZE_CMD_0x1, SSC_REQUEST_SIZE_CMD_0x2,
                                      SSC_REQUEST_SIZE_CMD_0x3, SSC_REQUEST_SIZE_CMD_0x4, SSC_REQUEST_SIZE_CMD_0x5,
                                      SSC_REQUEST_SIZE_CMD_0x6, SSC_REQUEST_SIZE_CMD_0x7, SSC_REQUEST_SIZE_CMD_0x8,
                                      SSC_REQUEST_SIZE_CMD_0x9};

static uint8_t INFOSTR_AKE_SESSIONSEED[]  = "AKE_SessionSeed\n";
static uint8_t INFOSTR_AKE_MACKEY[]       = "AKE_MACKey\n\n\n\n\n\n";
static uint8_t INFOSTR_AKE_EXTRACTORKEY[] = "AKE_ExtractorKey";

#if 0
    #define is_keyslot_valid(_ssc_state, _kbkdf_index) is_keyslot_valid_(__func__, _ssc_state, _kbkdf_index)

static bool is_keyslot_valid_(const char* func, struct AppleSEPSSCState *ssc_state,
                             uint16_t kbkdf_index)
#else
static bool is_keyslot_valid(struct AppleSEPSSCState* ssc_state, uint16_t kbkdf_index)
#endif
{
    bool ret;

    if (kbkdf_index >= KBKDF_KEY_MAX_SLOTS) {
        DPRINTF("%s: kbkdf_index over limit: %u\n", func, kbkdf_index);
        ret = false;
    }
    else {
        ret  = !buffer_is_zero(&ssc_state->ecc_keys[kbkdf_index], sizeof(struct ecc_scalar));
        ret &= !buffer_is_zero(&ssc_state->kbkdf_keys[kbkdf_index], sizeof(ssc_state->kbkdf_keys[kbkdf_index]));
    }

    DPRINTF("%s: kbkdf_index: %d ; ecc_keys_item_size: 0x%lX ; "
            "kbkdf_keys_item_size: 0x%lX\n",
            func, kbkdf_index, sizeof(struct ecc_scalar), sizeof(ssc_state->kbkdf_keys[kbkdf_index]));
    return ret;
}

static int aes_ccm_crypt(struct AppleSEPSSCState* ssc_state, uint16_t kbkdf_index, uint8_t* prefix, int payload_len,
                         uint8_t* data, uint8_t* out, int encrypt, int response_key)
{
    assert_cmpuint(payload_len, >=, 0);
    assert_cmpuint(payload_len, <=, AES_CCM_MAX_DATA_LENGTH - AES_CCM_TAG_LENGTH);

    struct ccm_aes256_ctx aes;
    uint32_t              counter_be                       = cpu_to_be32(ssc_state->kbkdf_counter[kbkdf_index]);
    uint8_t               nonce[AES_CCM_NONCE_LENGTH]      = {0};
    uint8_t               auth[AES_CCM_AUTH_LENGTH]        = {0};
    uint8_t               tmp_in[AES_CCM_MAX_DATA_LENGTH]  = {0};
    uint8_t               tmp_out[AES_CCM_MAX_DATA_LENGTH] = {0};
    uint8_t*              key                              = NULL;
    int                   status                           = 0;
#if 0
    // SEPFW role
    if (encrypt) {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_REQUEST_KEY_OFFSET];
        ssc_state->kbkdf_counter[kbkdf_index]++;
    } else {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_RESPONSE_KEY_OFFSET];
    }
#endif
#if 1
    // SSC role
    // if (encrypt)
    if (response_key) { key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_RESPONSE_KEY_OFFSET]; }
    else {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_REQUEST_KEY_OFFSET];
        ssc_state->kbkdf_counter[kbkdf_index]++;
    }
#endif

    memcpy(auth, prefix, MSG_PREFIX_LENGTH);
    memcpy(&auth[MSG_PREFIX_LENGTH], &counter_be, AES_CCM_COUNTER_LENGTH);
    memcpy(nonce, &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_SEED_OFFSET], KBKDF_KEY_SEED_LENGTH);
    memcpy(&nonce[KBKDF_KEY_SEED_LENGTH], &counter_be, AES_CCM_COUNTER_LENGTH);
    ccm_aes256_set_key(&aes, key);
    if (encrypt) {
#if NETTLE_VERSION_MAJOR >= 4
        ccm_aes256_encrypt_message(&aes.cipher, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth,
                                   AES_CCM_TAG_LENGTH, AES_CCM_TAG_LENGTH + payload_len, tmp_out, data);
#else
        ccm_aes256_encrypt_message(&aes, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth, AES_CCM_TAG_LENGTH,
                                   AES_CCM_TAG_LENGTH + payload_len, tmp_out, data);
#endif
        // data[0x20]-tag[0x10] => tag[0x10]-data[0x20]
        memcpy(out, &tmp_out[payload_len], AES_CCM_TAG_LENGTH);
        memcpy(&out[AES_CCM_TAG_LENGTH], tmp_out, payload_len);
    }
    else {
        DPRINTF("counter_be: 0x%08x\n", counter_be);
        // tag[0x10]-data[0x20] => data[0x20]-tag[0x10]
        memcpy(tmp_in, &data[AES_CCM_TAG_LENGTH], payload_len);
        memcpy(&tmp_in[payload_len], data, AES_CCM_TAG_LENGTH);
        HEXDUMP("tmp_in__tag_plus_encdata", data, AES_CCM_TAG_LENGTH + payload_len);
        HEXDUMP("tmp_in__encdata_plus_tag", tmp_in, AES_CCM_TAG_LENGTH + payload_len);
#if NETTLE_VERSION_MAJOR >= 4
        status = ccm_aes256_decrypt_message(&aes.cipher, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth,
                                            AES_CCM_TAG_LENGTH, payload_len, tmp_out, tmp_in);
#else
        status = ccm_aes256_decrypt_message(&aes, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth,
                                            AES_CCM_TAG_LENGTH, payload_len, tmp_out, tmp_in);
#endif
        if (!status) { DPRINTF("%s: ccm_aes256_decrypt_message: DIGEST INVALID\n", __func__); }
        memcpy(out, tmp_out, payload_len);
    }
    ////memcpy(out, tmp_out, AES_CCM_MAX_DATA_LENGTH);
    return status;
}

static int aes_cmac_prefix_public(uint8_t* key, uint8_t* prefix, uint8_t* public0, uint8_t* digest)
{
    struct cmac_aes256_ctx ctx;
    cmac_aes256_set_key(&ctx, key);
    cmac_aes256_update(&ctx, MSG_PREFIX_LENGTH, prefix);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public0);
#if NETTLE_VERSION_MAJOR >= 4
    cmac_aes256_digest(&ctx, digest);
#else
    cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
#endif
    return 0;
}

static int aes_cmac_prefix_public_public(uint8_t* key, uint8_t* prefix, uint8_t* public0, uint8_t* public1,
                                         uint8_t* digest)
{
    struct cmac_aes256_ctx ctx;
    cmac_aes256_set_key(&ctx, key);
    cmac_aes256_update(&ctx, MSG_PREFIX_LENGTH, prefix);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public0);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public1);
#if NETTLE_VERSION_MAJOR >= 4
    cmac_aes256_digest(&ctx, digest);
#else
    cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
#endif
    return 0;
}

static int kbkdf_generate_key(uint8_t* cmac_key, uint8_t* label, uint8_t* context, uint8_t* derived, int length)
{
    struct cmac_aes256_ctx ctx;

    uint8_t digest[CMAC128_DIGEST_SIZE] = {0};

    int      counter = 1;
    uint16_t be_len  = cpu_to_be16(length * 8);
    uint8_t  zero    = 0;

    for (size_t i = 0; i < length; i += CMAC128_DIGEST_SIZE) {
        cmac_aes256_set_key(&ctx, cmac_key);
        uint16_t be_cnt = cpu_to_be16(counter);
        cmac_aes256_update(&ctx, KBKDF_CMAC_LENGTH_SIZE, (uint8_t*)&be_cnt);
        cmac_aes256_update(&ctx, KBKDF_CMAC_LABEL_SIZE, label);    // 0x10 bytes
        cmac_aes256_update(&ctx, 1, (uint8_t*)&zero);
        cmac_aes256_update(&ctx, KBKDF_CMAC_CONTEXT_SIZE, context);    // 4 bytes
        cmac_aes256_update(&ctx, KBKDF_CMAC_LENGTH_SIZE, (uint8_t*)&be_len);
#if NETTLE_VERSION_MAJOR >= 4
        cmac_aes256_digest(&ctx, digest);
#else
        cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
#endif
        memcpy(&derived[i], digest, MIN(CMAC128_DIGEST_SIZE, length - i));
        counter++;
    }

    return 0;
}

static void clear_ecc_scalar(struct ecc_scalar* ecc_key)
{
    if (!buffer_is_zero(ecc_key, sizeof(struct ecc_scalar))) {
        ecc_scalar_clear(ecc_key);
        memset(ecc_key, 0, sizeof(*ecc_key));
    }
}

static int generate_ec_priv(struct AppleSEPSSCState* ssc_state, const char* priv, struct ecc_scalar* ecc_key,
                            struct ecc_point* ecc_pub)
{
    const struct ecc_curve* ecc = nettle_get_secp_384r1();
    mpz_t                   temp1;

    ecc_point_init(ecc_pub, ecc);
    clear_ecc_scalar(ecc_key);
    ecc_scalar_init(ecc_key, ecc);

    if (priv == NULL) {
        ecdsa_generate_keypair(ecc_pub, ecc_key, &ssc_state->rctx, (nettle_random_func*)knuth_lfib_random);
    }
    else {
        if (mpz_init_set_str(temp1, priv, 16) != 0) {
            mpz_clear(temp1);
            ecc_point_clear(ecc_pub);
            clear_ecc_scalar(ecc_key);
            return -1;
        }
        mpz_add_ui(temp1, temp1, 1);
        if (ecc_scalar_set(ecc_key, temp1) == 0) {
            mpz_clear(temp1);
            ecc_point_clear(ecc_pub);
            clear_ecc_scalar(ecc_key);
            return -1;
        }
        mpz_clear(temp1);
        ecc_point_mul_g(ecc_pub, ecc_key);
    }

    return 0;
}

static int output_ec_pub(struct ecc_point* ecc_pub, uint8_t* pub_xy)
{
    // const struct ecc_curve *ecc = nettle_get_secp_384r1();
    mpz_t temp1, temp2;

    mpz_inits(temp1, temp2, NULL);
    ecc_point_get(ecc_pub, temp1, temp2);
    mpz_export(&pub_xy[0x00], NULL, 1, 1, 1, 0, temp1);
    mpz_export(&pub_xy[0x00 + SHA384_DIGEST_SIZE], NULL, 1, 1, 1, 0, temp2);
    HEXDUMP("output_ec_pub: pub_x", &pub_xy[0x00], SHA384_DIGEST_SIZE);
    HEXDUMP("output_ec_pub: pub_y", &pub_xy[0x00 + SHA384_DIGEST_SIZE], SHA384_DIGEST_SIZE);

    mpz_clears(temp1, temp2, NULL);

    return 0;
}

static int input_ec_pub(struct ecc_point* ecc_pub, uint8_t* pub_xy)
{
    const struct ecc_curve* ecc = nettle_get_secp_384r1();
    mpz_t                   temp1, temp2;
    int                     ret = 0;

    HEXDUMP("input_ec_pub: pub_x", &pub_xy[0x00], SHA384_DIGEST_SIZE);
    HEXDUMP("input_ec_pub: pub_y", &pub_xy[0x00 + SHA384_DIGEST_SIZE], SHA384_DIGEST_SIZE);
    mpz_inits(temp1, temp2, NULL);
    mpz_import(temp1, SHA384_DIGEST_SIZE, 1, 1, 1, 0, &pub_xy[0x00]);
    mpz_import(temp2, SHA384_DIGEST_SIZE, 1, 1, 1, 0, &pub_xy[0x00 + SHA384_DIGEST_SIZE]);
    ecc_point_init(ecc_pub, ecc);
    ret = ecc_point_set(ecc_pub, temp1, temp2);

    mpz_clears(temp1, temp2, NULL);

    return ret;
}

static int generate_kbkdf_keys(struct AppleSEPSSCState* ssc_state, struct ecc_scalar* ecc_key,
                               struct ecc_point* ecc_pub_peer, uint8_t* hmac_key, uint8_t* label, uint8_t* context,
                               uint16_t kbkdf_index)
{
    const struct ecc_curve* ecc = nettle_get_secp_384r1();
    struct ecc_point        T;
    // shared_key == pub_x (first half)
    uint8_t shared_key_xy[SECP384_PUBLIC_XY_SIZE] = {0};
    uint8_t derived_key[SHA256_DIGEST_SIZE]       = {0};
    DPRINTF("generate_kbkdf_keys: label: %s\n", label);    // 0x10 bytes
    DPRINTF("generate_kbkdf_keys: context: %02x%02x%02x%02x\n", context[0x00], context[0x01], context[0x02],
            context[0x03]);    // 4 bytes

    ecc_point_init(&T, ecc);
    ecc_point_mul(&T, ecc_key, ecc_pub_peer);
    DPRINTF("generate_kbkdf_keys: shared_key==pub_x:\n");
    output_ec_pub(&T, shared_key_xy);
    ecc_point_clear(&T);

    struct hmac_sha256_ctx ctx;
    hmac_sha256_set_key(&ctx, SHA256_DIGEST_SIZE, hmac_key);
    // only the first half is the shared_key
    hmac_sha256_update(&ctx, SHA384_DIGEST_SIZE, shared_key_xy);
#if NETTLE_VERSION_MAJOR >= 4
    hmac_sha256_digest(&ctx, derived_key);
#else
    hmac_sha256_digest(&ctx, SHA256_DIGEST_SIZE, derived_key);
#endif
    HEXDUMP("generate_kbkdf_keys: derived_key", derived_key, SHA256_DIGEST_SIZE);

    int err =
        kbkdf_generate_key(derived_key, label, context, ssc_state->kbkdf_keys[kbkdf_index], KBKDF_CMAC_OUTPUT_LEN);
    if (err != 0) {
        DPRINTF("error: kbkdf_generate_key returned non-zero\n");
        return err;
    }
    ssc_state->kbkdf_counter[kbkdf_index] = 0;
    HEXDUMP("generate_kbkdf_keys: ssc_state->kbkdf_keys[kbkdf_index]", ssc_state->kbkdf_keys[kbkdf_index],
            KBKDF_CMAC_OUTPUT_LEN);

    return 0;
}

// this function should be kept, it might be the key to properly accessing SSC.
static void hkdf_sha256(int salt_len, uint8_t* salt, int info_len, uint8_t* info, int key_len, uint8_t* key,
                        uint8_t* out)
{
    struct hmac_sha256_ctx ctx;
    uint8_t                prk[SHA256_DIGEST_SIZE];

    hmac_sha256_set_key(&ctx, salt_len, salt);
#if NETTLE_VERSION_MAJOR >= 4
    hkdf_extract(&ctx, (nettle_hash_update_func*)hmac_sha256_update, (nettle_hash_digest_func*)hmac_sha256_digest,
                 key_len, key, prk);
#else
    hkdf_extract(&ctx, (nettle_hash_update_func*)hmac_sha256_update, (nettle_hash_digest_func*)hmac_sha256_digest,
                 SHA256_DIGEST_SIZE, key_len, key, prk);
#endif

    hmac_sha256_set_key(&ctx, SHA256_DIGEST_SIZE, prk);
    hkdf_expand(&ctx, (nettle_hash_update_func*)hmac_sha256_update, (nettle_hash_digest_func*)hmac_sha256_digest,
                SHA256_DIGEST_SIZE, info_len, info, SHA256_DIGEST_SIZE, out);
}

static void aes_keys_from_sp_key(struct AppleSEPSSCState* ssc_state, uint16_t kbkdf_index, uint8_t* prefix,
                                 uint8_t* aes_key_mackey, uint8_t* aes_key_extractorkey)
{
    // wrapping with "SP key"/"Spes"/"Lynx version 1 crypto" could be wrong.
    uint8_t hmac_key[0x20] = {0};
    memcpy(hmac_key, ssc_state->slot_hmac_key[kbkdf_index], 0x20);
    HEXDUMP("aes_keys_from_sp_key: hmac_key", hmac_key, 0x20);
    kbkdf_generate_key(hmac_key, INFOSTR_AKE_MACKEY, prefix, aes_key_mackey, 0x20);
    HEXDUMP("aes_keys_from_sp_key: aes_key_mackey", aes_key_mackey, 0x20);
    kbkdf_generate_key(hmac_key, INFOSTR_AKE_EXTRACTORKEY, prefix, aes_key_extractorkey, 0x20);
    HEXDUMP("aes_keys_from_sp_key: aes_key_extractorkey", aes_key_extractorkey, 0x20);
}

static void do_response_prefix(uint8_t* request, uint8_t* response, uint8_t flags)
{
    memset(response, 0, SSC_MAX_RESPONSE_SIZE);
    uint8_t cmd = request[0];
    response[0] = cmd;
    if (cmd <= 0x6) { response[1] = request[1]; }
    response[2] = 0;
    response[3] = flags;
}

// TODO: Properly handle various error cases with cmd 0x0/0x1/..., like wrong
// hashes/signatures/parameters or public keys not being on the curve.

static void answer_cmd_0x0_init1(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    struct ecc_point     cmd0_ecpub, ecc_pub;
    struct dsa_signature signature;
    uint8_t              digest[SHA384_DIGEST_SIZE] = {0};
    uint16_t             kbkdf_index                = 0;    // hardcoded
    struct sha384_ctx    ctx;

    if (is_keyslot_valid(ssc_state, kbkdf_index)) {    // shouldn't already exist
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    if (input_ec_pub(&cmd0_ecpub,
                     &request[MSG_PREFIX_LENGTH + SHA256_DIGEST_SIZE]) == 0) {    // curve is invalid
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid curve\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret1;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    const char* priv_str = "222222222222222222222222222222222222222222222222"
                           "222222222222222222222222222222222222222222222222";
    if (generate_ec_priv(ssc_state, priv_str, &ssc_state->ecc_keys[kbkdf_index], &ecc_pub) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: generate_ec_priv failed\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret0;
    }
    output_ec_pub(&ecc_pub, &response[MSG_PREFIX_LENGTH + SECP384_PUBLIC_XY_SIZE]);
    memcpy(ssc_state->random_hmac_key, &request[MSG_PREFIX_LENGTH], SHA256_DIGEST_SIZE);
    DPRINTF("INFOSTR_AKE_SESSIONSEED: %s\n", INFOSTR_AKE_SESSIONSEED);
    generate_kbkdf_keys(ssc_state, &ssc_state->ecc_keys[kbkdf_index], &cmd0_ecpub, ssc_state->random_hmac_key,
                        INFOSTR_AKE_SESSIONSEED, request, kbkdf_index);

    sha384_init(&ctx);
    sha384_update(&ctx, MSG_PREFIX_LENGTH, &response[0x00]);    // prefix
    sha384_update(&ctx, SECP384_PUBLIC_XY_SIZE,
                  &request[MSG_PREFIX_LENGTH + SHA256_DIGEST_SIZE]);    // sw_public_xy0
    sha384_update(&ctx, SECP384_PUBLIC_XY_SIZE,
                  &response[MSG_PREFIX_LENGTH + SECP384_PUBLIC_XY_SIZE]);    // public_xy1
    sha384_update(&ctx, SHA256_DIGEST_SIZE,
                  ssc_state->random_hmac_key);    // hmac_key
#if NETTLE_VERSION_MAJOR >= 4
    sha384_digest(&ctx, digest);
#else
    sha384_digest(&ctx, SHA384_DIGEST_SIZE, digest);
#endif
    HEXDUMP("answer_cmd_0x0_init1 digest", digest, SHA384_DIGEST_SIZE);
    // Using non-deterministic signing here like it's probably supposed to be.
    // Don't want to implement/port deterministic signing.
    dsa_signature_init(&signature);
    ecdsa_sign(&ssc_state->ecc_key_main, &ssc_state->rctx, (nettle_random_func*)knuth_lfib_random, SHA384_DIGEST_SIZE,
               digest, &signature);
    mpz_export(&response[MSG_PREFIX_LENGTH + 0x00 + 0x00], NULL, 1, 1, 1, 0, signature.r);
    mpz_export(&response[MSG_PREFIX_LENGTH + 0x00 + SHA384_DIGEST_SIZE], NULL, 1, 1, 1, 0, signature.s);
    dsa_signature_clear(&signature);
jump_ret0:
    ecc_point_clear(&ecc_pub);
jump_ret1:
    ecc_point_clear(&cmd0_ecpub);
}

static void answer_cmd_0x1_connect_sp(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x01_req", request, SSC_REQUEST_SIZE_CMD_0x1);
    struct ecc_point cmd1_ecpub, ecc_pub;
    uint16_t         kbkdf_index = request[1];

    uint8_t* cmac_req_should = &request[MSG_PREFIX_LENGTH];
    uint8_t* sw_public_xy2   = &request[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE];
    DPRINTF("answer_cmd_0x1_connect_sp: kbkdf_index: %u\n", kbkdf_index);
    HEXDUMP("answer_cmd_0x1_connect_sp: req_prefix", request, MSG_PREFIX_LENGTH);
    HEXDUMP("answer_cmd_0x1_connect_sp: sw_public_xy2", sw_public_xy2, SECP384_PUBLIC_XY_SIZE);
    HEXDUMP("answer_cmd_0x1_connect_sp: cmac_req_should", cmac_req_should, AES_BLOCK_SIZE);
    if (is_keyslot_valid(ssc_state, kbkdf_index)) {    // shouldn't already exist
        DPRINTF("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    if (input_ec_pub(&cmd1_ecpub, sw_public_xy2) == 0) {    // curve is invalid
        DPRINTF("%s: invalid curve\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret1;
    }
    const char* priv_str = "333333333333333333333333333333333333333333333333"
                           "333333333333333333333333333333333333333333333333";
    if (generate_ec_priv(ssc_state, priv_str, &ssc_state->ecc_keys[kbkdf_index], &ecc_pub) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: generate_ec_priv failed\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret0;
    }
    uint8_t aes_key_mackey_req[0x20]       = {0};
    uint8_t aes_key_extractorkey_req[0x20] = {0};
    aes_keys_from_sp_key(ssc_state, kbkdf_index, request, aes_key_mackey_req, aes_key_extractorkey_req);
    uint8_t cmac_req_is[AES_BLOCK_SIZE] = {0};
    aes_cmac_prefix_public(aes_key_mackey_req, request, sw_public_xy2, cmac_req_is);
    HEXDUMP("answer_cmd_0x1_connect_sp: aes_key_mackey_req", aes_key_mackey_req, sizeof(aes_key_mackey_req));
    HEXDUMP("answer_cmd_0x1_connect_sp: aes_key_extractorkey_req ", aes_key_extractorkey_req,
            sizeof(aes_key_extractorkey_req));
    HEXDUMP("answer_cmd_0x1_connect_sp: cmac_req_is", cmac_req_is, sizeof(cmac_req_is));
    if (memcmp(cmac_req_should, cmac_req_is, sizeof(cmac_req_is)) != 0) {
        DPRINTF("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        goto jump_ret0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    output_ec_pub(&ecc_pub, &response[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE]);
    generate_kbkdf_keys(ssc_state, &ssc_state->ecc_keys[kbkdf_index], &cmd1_ecpub, aes_key_extractorkey_req,
                        INFOSTR_AKE_SESSIONSEED, request, kbkdf_index);

    uint8_t* cmac_resp  = &response[MSG_PREFIX_LENGTH];
    uint8_t* public_xy2 = &response[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE];
    aes_cmac_prefix_public_public(aes_key_mackey_req, response, sw_public_xy2, public_xy2, cmac_resp);

    HEXDUMP("cmd_0x01_resp", response, SSC_RESPONSE_SIZE_CMD_0x1);
jump_ret0:
    ecc_point_clear(&ecc_pub);
jump_ret1:
    ecc_point_clear(&cmd1_ecpub);
}

static void answer_cmd_0x2_disconnect_sp(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x02_req", request, SSC_REQUEST_SIZE_CMD_0x2);
    uint16_t kbkdf_index = request[1];
    if (!is_keyslot_valid(ssc_state, kbkdf_index)) {    // should already exist
        DPRINTF("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    clear_ecc_scalar(&ssc_state->ecc_keys[kbkdf_index]);
    memset(&ssc_state->kbkdf_keys[kbkdf_index], 0, sizeof(ssc_state->kbkdf_keys[kbkdf_index]));
    ssc_state->kbkdf_counter[kbkdf_index] = 0;
    DPRINTF("answer_cmd_0x2_disconnect_sp: kbkdf_index: %u\n", kbkdf_index);
}

static void answer_cmd_0x3_metadata_write(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x03_req", request, SSC_REQUEST_SIZE_CMD_0x3);
    uint16_t kbkdf_index_key      = request[1];
    uint16_t kbkdf_index_dataslot = request[2];
    uint8_t  copy                 = request[3];
    DPRINTF("cmd_0x03_req: kbkdf_index_key: %u\n", kbkdf_index_key);
    DPRINTF("cmd_0x03_req: kbkdf_index_dataslot: %u\n", kbkdf_index_dataslot);
    DPRINTF("cmd_0x03_req: copy: %u\n", copy);
    ////if (copy >= SSC_REQUEST_MAX_COPIES)
    if (copy > 0) {
        DPRINTF("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return;
    }
    if (!is_keyslot_valid(ssc_state, kbkdf_index_key)) {
        DPRINTF("%s: invalid kbkdf_index_key: %u\n", __func__, kbkdf_index_key);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    if (kbkdf_index_dataslot == 0 || kbkdf_index_dataslot >= KBKDF_KEY_MAX_SLOTS) {
        DPRINTF("%s: invalid kbkdf_index_dataslot: %u\n", __func__, kbkdf_index_dataslot);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    int blk_offset = (kbkdf_index_dataslot * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int key_offset = (KBKDF_KEY_KEY_FILE_OFFSET * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (kbkdf_index_dataslot * KBKDF_KEY_KEY_LENGTH);
    DPRINTF("cmd_0x03_req: blk_offset: 0x%X\n", blk_offset);
    HEXDUMP("cmd_0x03_req: ssc_state->kbkdf_keys[kbkdf_index_key]", ssc_state->kbkdf_keys[kbkdf_index_key],
            KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_dec_out[CMD_METADATA_PAYLOAD_LENGTH] = {0};
    int     err0 = aes_ccm_crypt(ssc_state, kbkdf_index_key, &request[0x00], CMD_METADATA_PAYLOAD_LENGTH,
                                 &request[MSG_PREFIX_LENGTH], req_dec_out, false, false);
    if (err0 == 0) {
        DPRINTF("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x03_req: req_dec_out", req_dec_out, CMD_METADATA_PAYLOAD_LENGTH);

    memcpy(ssc_state->slot_hmac_key[kbkdf_index_dataslot], req_dec_out,
           sizeof(req_dec_out));    // 0x20 bytes ; necessary here because there
                                    // are no metadata reads (cmd 0x6) after that.

    // blk_pwrite(ssc_state->blk, blk_offset, CMD_METADATA_PAYLOAD_LENGTH,
    // req_dec_out, 0); // Is it really necessary to write the mac_key or any
    // metadata to blk_offset?
    uint8_t zeroes_0x40[CMD_METADATA_DATA_PAYLOAD_LENGTH] = {0};
    // clear it on metadata write, all 0x40 bytes at blk_offset. is this correct?
    ssc_store_write(ssc_state, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH, zeroes_0x40);
    ssc_store_write(ssc_state, key_offset, CMD_METADATA_PAYLOAD_LENGTH, req_dec_out);

    uint8_t resp_nop_out[1] = {0x00};
    HEXDUMP("cmd_0x03_resp: resp_nop_out", resp_nop_out, 1);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index_key, &response[0x00], 0x0, resp_nop_out,
                             &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x03_resp", response, SSC_RESPONSE_SIZE_CMD_0x3);
}

static void answer_cmd_0x4_metadata_data_read(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x04_req", request, SSC_REQUEST_SIZE_CMD_0x4);
    uint16_t kbkdf_index = request[1];
    uint8_t  copy        = request[3];
    DPRINTF("cmd_0x04_req: kbkdf_index: %u\n", kbkdf_index);
    DPRINTF("cmd_0x04_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DPRINTF("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return;
    }
    if (kbkdf_index == 0 || !is_keyslot_valid(ssc_state, kbkdf_index)) {
        DPRINTF("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    int blk_offset = (kbkdf_index * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    DPRINTF("cmd_0x04_req: blk_offset: 0x%X\n", blk_offset);
    HEXDUMP("cmd_0x04_req: ssc_state->kbkdf_keys[kbkdf_index]", ssc_state->kbkdf_keys[kbkdf_index],
            KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_nop_out[1] = {0};
    int     err0 = aes_ccm_crypt(ssc_state, kbkdf_index, &request[0x00], 0x0, &request[MSG_PREFIX_LENGTH], req_nop_out,
                                 false, false);
    if (err0 == 0) {
        DPRINTF("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x04_req: req_nop_out", req_nop_out, 1);

    uint8_t resp_dec_out[CMD_METADATA_DATA_PAYLOAD_LENGTH] = {0};
    ssc_store_read(ssc_state, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH, resp_dec_out);

    HEXDUMP("cmd_0x04_resp: resp_dec_out", resp_dec_out, CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index, &response[0x00], CMD_METADATA_DATA_PAYLOAD_LENGTH, resp_dec_out,
                             &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x04_resp", response, SSC_RESPONSE_SIZE_CMD_0x4);
}

static void answer_cmd_0x5_metadata_data_write(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x05_req", request, SSC_REQUEST_SIZE_CMD_0x5);
    uint16_t kbkdf_index = request[1];
    uint8_t  copy        = request[3];
    DPRINTF("cmd_0x05_req: kbkdf_index: %u\n", kbkdf_index);
    DPRINTF("cmd_0x05_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DPRINTF("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return;
    }
    if (kbkdf_index == 0 || !is_keyslot_valid(ssc_state, kbkdf_index)) {
        DPRINTF("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    int blk_offset = (kbkdf_index * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    DPRINTF("cmd_0x05_req: blk_offset: 0x%X\n", blk_offset);
    HEXDUMP("cmd_0x05_req: ssc_state->kbkdf_keys[kbkdf_index]", ssc_state->kbkdf_keys[kbkdf_index],
            KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_dec_out[CMD_METADATA_DATA_PAYLOAD_LENGTH] = {0};
    int     err0 = aes_ccm_crypt(ssc_state, kbkdf_index, &request[0x00], CMD_METADATA_DATA_PAYLOAD_LENGTH,
                                 &request[MSG_PREFIX_LENGTH], req_dec_out, false, false);
    if (err0 == 0) {
        DPRINTF("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x05_req: req_dec_out", req_dec_out, CMD_METADATA_DATA_PAYLOAD_LENGTH);

    ssc_store_write(ssc_state, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH, req_dec_out);

    uint8_t resp_nop_out[1] = {0x00};
    HEXDUMP("cmd_0x05_resp: resp_nop_out", resp_nop_out, 1);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index, &response[0x00], 0x0, resp_nop_out, &response[MSG_PREFIX_LENGTH],
                             true, true);
    HEXDUMP("cmd_0x05_resp", response, SSC_RESPONSE_SIZE_CMD_0x5);
}

static void answer_cmd_0x6_metadata_read(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x06_req", request, SSC_REQUEST_SIZE_CMD_0x6);

    uint16_t kbkdf_index_key      = request[1];
    uint16_t kbkdf_index_dataslot = request[2];
    uint8_t  copy                 = request[3];
    DPRINTF("cmd_0x06_req: kbkdf_index_key: %u\n", kbkdf_index_key);
    DPRINTF("cmd_0x06_req: kbkdf_index_dataslot: %u\n", kbkdf_index_dataslot);
    DPRINTF("cmd_0x06_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DPRINTF("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return;
    }
    if (!is_keyslot_valid(ssc_state, kbkdf_index_key)) {
        DPRINTF("%s: invalid kbkdf_index_key: %u\n", __func__, kbkdf_index_key);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    if (kbkdf_index_dataslot == 0 || kbkdf_index_dataslot >= KBKDF_KEY_MAX_SLOTS) {
        DPRINTF("%s: invalid kbkdf_index_dataslot: %u\n", __func__, kbkdf_index_dataslot);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return;
    }
    int blk_offset = (kbkdf_index_dataslot * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int key_offset = (KBKDF_KEY_KEY_FILE_OFFSET * CMD_METADATA_DATA_PAYLOAD_LENGTH * SSC_REQUEST_MAX_COPIES)
                     + (kbkdf_index_dataslot * KBKDF_KEY_KEY_LENGTH);
    DPRINTF("cmd_0x06_req: blk_offset: 0x%X\n", blk_offset);
    HEXDUMP("cmd_0x06_req: ssc_state->kbkdf_keys[kbkdf_index_key]", ssc_state->kbkdf_keys[kbkdf_index_key],
            KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_nop_out[1] = {0};
    int err0 = aes_ccm_crypt(ssc_state, kbkdf_index_key, &request[0x00], 0x0, &request[MSG_PREFIX_LENGTH], req_nop_out,
                             false, false);
    if (err0 == 0) {
        DPRINTF("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x06_req: req_nop_out", req_nop_out, 1);

    uint8_t resp_dec_out[CMD_METADATA_PAYLOAD_LENGTH] = {0};
    ssc_store_read(ssc_state, blk_offset, CMD_METADATA_PAYLOAD_LENGTH, resp_dec_out);
    ssc_store_read(ssc_state, key_offset, CMD_METADATA_PAYLOAD_LENGTH, ssc_state->slot_hmac_key[kbkdf_index_dataslot]);

    HEXDUMP("cmd_0x06_resp: resp_dec_out", resp_dec_out, CMD_METADATA_PAYLOAD_LENGTH);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index_key, &response[0x00], CMD_METADATA_PAYLOAD_LENGTH, resp_dec_out,
                             &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x06_resp", response, SSC_RESPONSE_SIZE_CMD_0x6);
}

static void answer_cmd_0x7_init0(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    struct ecc_point ecc_pub;
    DPRINTF("%s: entered function\n", __func__);

    const char* priv_str = "111111111111111111111111111111111111111111111111"
                           "111111111111111111111111111111111111111111111111";
    // no NULL here, this should stay static
    if (generate_ec_priv(ssc_state, priv_str, &ssc_state->ecc_key_main, &ecc_pub) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: generate_ec_priv failed\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    uint8_t unknown0[0x06] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xAB};
    uint8_t cpsn[0x07]     = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xCC};
    uint8_t unknown1[0x07] = {0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05};
    memcpy(ssc_state->cpsn, cpsn, sizeof(ssc_state->cpsn));
    memcpy(&response[MSG_PREFIX_LENGTH], unknown0, sizeof(unknown0));
    memcpy(&response[MSG_PREFIX_LENGTH + sizeof(unknown0)], ssc_state->cpsn, sizeof(ssc_state->cpsn));
    memcpy(&response[MSG_PREFIX_LENGTH + sizeof(unknown0) + sizeof(ssc_state->cpsn)], unknown1, sizeof(unknown1));
    output_ec_pub(&ecc_pub,
                  &response[MSG_PREFIX_LENGTH + sizeof(unknown0) + sizeof(ssc_state->cpsn) + sizeof(unknown1)]);

    HEXDUMP("cmd_0x07_resp", response, SSC_RESPONSE_SIZE_CMD_0x7);

jump_ret:
    ecc_point_clear(&ecc_pub);
}

static void answer_cmd_0x8_sleep(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x08_resp", response, SSC_RESPONSE_SIZE_CMD_0x8);
}

static void answer_cmd_0x9_panic(struct AppleSEPSSCState* ssc_state, uint8_t* request, uint8_t* response)
{
    DPRINTF("%s: entered function\n", __func__);
    ////apple_sep_ssc_reset(DEVICE(ssc_state));
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    // uint8_t panic_data[0x24] = {...};
    // memcpy(&response[MSG_PREFIX_LENGTH], panic_data, 0x24);
    memset(&response[MSG_PREFIX_LENGTH], 0xCC, 0x24);
    memcpy(&response[MSG_PREFIX_LENGTH + 0x24], ssc_state->cpsn, sizeof(ssc_state->cpsn));
    HEXDUMP("cmd_0x09_resp", response, SSC_RESPONSE_SIZE_CMD_0x9);
}

static uint8_t apple_sep_ssc_rx(I2CSlave* i2c)
{
    AppleSEPSSCState* ssc = container_of(i2c, AppleSEPSSCState, parent_obj);
    uint8_t           ret = 0;

    // ssc->req_cur = 0;

    if (ssc->resp_cur >= sizeof(ssc->resp_cmd)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ssc->resp_cur too high 0x%02x\n", __func__, ssc->resp_cur);
        return 0;
    }

    if (ssc->resp_cur == 0) {
        // ssc->req_cur = 0;
        memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));
        ssc->resp_cmd[0] = ssc->req_cmd[0];
    }
    // This tries to prevent a spurious call during a dummy read.
    if (ssc->resp_cur == 1) {
        uint8_t cmd = ssc->req_cmd[0];
        if (cmd > 0x09) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: cmd %u: invalid command > 0x09", __func__, cmd);
            do_response_prefix(ssc->req_cmd, ssc->resp_cmd, SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        }
        else if (ssc->req_cur != ssc_request_sizes[cmd]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cmd %u: invalid cmdsize mismatch req_cur "
                          "is 0x%02x != should 0x%02x\n",
                          __func__, cmd, ssc->req_cur, ssc_request_sizes[cmd]);
            do_response_prefix(ssc->req_cmd, ssc->resp_cmd, SSC_RESPONSE_FLAG_COMMAND_SIZE_MISMATCH);
        }
        else if (cmd == 0x00) {    // req 0x84 bytes, resp 0xC4 bytes
            answer_cmd_0x0_init1(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x01) {    // req 0x74 bytes, resp 0x74 bytes
            answer_cmd_0x1_connect_sp(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x02) {    // req 0x04 bytes, resp 0x04 bytes
            answer_cmd_0x2_disconnect_sp(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x03) {    // req 0x34 bytes, resp 0x14 bytes
            answer_cmd_0x3_metadata_write(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x04) {    // req 0x14 bytes, resp 0x54 bytes
            answer_cmd_0x4_metadata_data_read(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x05) {    // req 0x54 bytes, resp 0x14 bytes
            answer_cmd_0x5_metadata_data_write(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x06) {    // req 0x14 bytes, resp 0x34 bytes
            answer_cmd_0x6_metadata_read(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x07) {    // req 0x04 bytes, resp 0x78 bytes
            answer_cmd_0x7_init0(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x08) {    // req 0x04 bytes, resp 0x04 bytes
            answer_cmd_0x8_sleep(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        else if (cmd == 0x09) {    // req 0x04 bytes, resp 0x2F bytes
            answer_cmd_0x9_panic(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        ssc->req_cur = 0;
        memset(ssc->req_cmd, 0, sizeof(ssc->req_cmd));
        HEXDUMP("apple_sep_ssc_rx: before resp_cmd invalid check", ssc->resp_cmd, sizeof(ssc->resp_cmd));
        if (ssc->resp_cmd[3] != SSC_RESPONSE_FLAG_OK) {
            memset(&ssc->resp_cmd[MSG_PREFIX_LENGTH], 0xFF, sizeof(ssc->resp_cmd) - MSG_PREFIX_LENGTH);
        }
    }

    ret = ssc->resp_cmd[ssc->resp_cur++];
    DPRINTF("apple_sep_ssc_rx: resp_cur=0x%02x ret=0x%02x\n", ssc->resp_cur - 1, ret);
#if 0
    // could raising the interrupt here cause hangs?
    apple_a7iop_interrupt_status_push(ssc->sep->mailbox,
                                      0x10002); // I2C
#endif
    return ret;
}

static int apple_sep_ssc_tx(I2CSlave* i2c, uint8_t data)
{
    AppleSEPSSCState* ssc = container_of(i2c, AppleSEPSSCState, parent_obj);

    if (ssc->req_cur == 0) {
        ssc->resp_cur = 0;
        memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));
    }

    if (ssc->req_cur >= sizeof(ssc->req_cmd)) {
        qemu_log_mask(LOG_GUEST_ERROR, "apple_sep_ssc_tx: ssc->req_cur too high 0x%02x\n", ssc->req_cur);
        return 0;
    }

    DPRINTF("apple_sep_ssc_tx: req_cur=0x%02x data=0x%02x\n", ssc->req_cur, data);
    ssc->req_cmd[ssc->req_cur++] = data;
    return 0;
}

static void apple_sep_ssc_reset_enter(Object* obj, ResetType type)
{
    AppleSEPSSCState* ssc = APPLE_SEP_SSC(obj);
    DPRINTF("%s: called\n", __func__);

    ssc->req_cur  = 0;
    ssc->resp_cur = 0;
    memset(ssc->req_cmd, 0, sizeof(ssc->req_cmd));
    memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));

    const struct ecc_curve* ecc = nettle_get_secp_384r1();
    clear_ecc_scalar(&ssc->ecc_key_main);
    ecc_scalar_init(&ssc->ecc_key_main, ecc);
    for (int i = 0; i < KBKDF_KEY_MAX_SLOTS; i++) {
        clear_ecc_scalar(&ssc->ecc_keys[i]);
        ecc_scalar_init(&ssc->ecc_keys[i], ecc);
    }
    knuth_lfib_init(&ssc->rctx, 4711);
    memset(ssc->random_hmac_key, 0, sizeof(ssc->random_hmac_key));
    memset(ssc->slot_hmac_key, 0, sizeof(ssc->slot_hmac_key));
    memset(ssc->kbkdf_keys, 0, sizeof(ssc->kbkdf_keys));
    memset(ssc->kbkdf_counter, 0, sizeof(ssc->kbkdf_counter));
    uint8_t cpsn[0x07] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xFE};
    memcpy(ssc->cpsn, cpsn, sizeof(cpsn));
}

static void apple_sep_ssc_realize(DeviceState* dev, Error** errp)
{
    AppleSEPSSCState* ssc = APPLE_SEP_SSC(dev);
    int64_t           len;
    int               ret;

    ssc->store = g_malloc0(SSC_STORE_SIZE);

    if (ssc->blk == NULL) { return; }

    len = blk_getlength(ssc->blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "apple_sep_ssc: cannot determine store size");
        return;
    }

    /* A short image is not an error: the tail is neither read nor written. */
    len                = MIN(len, SSC_STORE_SIZE);
    ssc->store_blk_len = len;
    if (len < SSC_STORE_SIZE) {
        warn_report("apple_sep_ssc: store is 0x%" PRIx64 " bytes, want 0x%x; the tail will not persist", (uint64_t)len,
                    SSC_STORE_SIZE);
    }
    if (len > 0) {
        ret = blk_pread(ssc->blk, 0, len, ssc->store, 0);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "apple_sep_ssc: cannot read store");
            return;
        }
    }

    ssc->flush_bh      = qemu_bh_new(ssc_flush_bh, ssc);
    ssc->vmstate_entry = qemu_add_vm_change_state_handler(ssc_vm_state_change, ssc);
}

static void apple_sep_ssc_unrealize(DeviceState* dev)
{
    AppleSEPSSCState* ssc = APPLE_SEP_SSC(dev);

    if (ssc->vmstate_entry != NULL) {
        qemu_del_vm_change_state_handler(ssc->vmstate_entry);
        ssc->vmstate_entry = NULL;
    }
    if (ssc->flush_bh != NULL) {
        ssc_flush_sync(ssc);
        qemu_bh_delete(ssc->flush_bh);
        ssc->flush_bh = NULL;
    }
    g_free(ssc->store);
    ssc->store = NULL;
}

AppleSEPSSCState* apple_sep_ssc_create(AppleI2CState* i2c, uint8_t addr, AppleSEP* sep, BlockBackend* blk)
{
    AppleSEPSSCState* ssc;

    ssc      = APPLE_SEP_SSC(i2c_slave_new(TYPE_APPLE_SEP_SSC, addr));
    ssc->sep = sep;

    qdev_prop_set_drive_err(DEVICE(ssc), "drive", blk, &error_fatal);
    i2c_slave_realize_and_unref(I2C_SLAVE(ssc), i2c->bus, &error_fatal);

    return ssc;
}

static const Property apple_sep_ssc_props[] = {
    DEFINE_PROP_DRIVE("drive", AppleSEPSSCState, blk),
};

static void apple_sep_ssc_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    I2CSlaveClass*   c  = I2C_SLAVE_CLASS(klass);

    rc->phases.enter = apple_sep_ssc_reset_enter;

    dc->desc      = "Apple SSC";
    dc->realize   = apple_sep_ssc_realize;
    dc->unrealize = apple_sep_ssc_unrealize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->event = apple_sep_ssc_event;
    c->recv  = apple_sep_ssc_rx;
    c->send  = apple_sep_ssc_tx;

    device_class_set_props(dc, apple_sep_ssc_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPSSCState, apple_sep_ssc, APPLE_SEP_SSC, I2C_SLAVE)
