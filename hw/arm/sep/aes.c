/*
 * Apple SEP AES.
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
#include "qapi/error.h"
#include "hw/arm/sep/private.h"
#include "crypto/cipher.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"

#define AESC_BASE_REG_SIZE (0x4000)     // S8000
#define AESS_BASE_REG_SIZE (0x10000)    // T8015/T8030
#define AESH_BASE_REG_SIZE (0x10000)

#define SEP_AESS_CMD_FLAG_KEYSIZE_AES128 0x0U
#define SEP_AESS_CMD_FLAG_KEYSIZE_AES192 0x100U
#define SEP_AESS_CMD_FLAG_KEYSIZE_AES256 0x200U

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8010   0x00U    // ???
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8010   0x10U    // ???
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8010 0x20U    // ???
#define SEP_AESS_CMD_FLAG_UNKNOWN0_T8010         0x00U    // ???

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8020 0x00U    // also for T8015
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8020 0x40U    // also for T8015
// Also for T8015, this (custom) takes precedence over the other
// keyselect flags
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8020 0x80U
#define SEP_AESS_CMD_FLAG_UNKNOWN0_T8020         0x10U
#define SEP_AESS_CMD_FLAG_UNKNOWN1_T8020         0x20U

#define SEP_AESS_CMD_FLAG_UNKNOWN0 SEP_AESS_CMD_FLAG_UNKNOWN0_T8020
#define SEP_AESS_CMD_FLAG_UNKNOWN1 SEP_AESS_CMD_FLAG_UNKNOWN1_T8020

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0   SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8020
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1   SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8020
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8020

#define SEP_AESS_CMD_MASK 0x3FFU

#define SEP_AESS_CMD_WITHOUT_KEYSIZE(cmd) \
    ((cmd) & ~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 | SEP_AESS_CMD_FLAG_KEYSIZE_AES128))
#define SEP_AESS_CMD_WITHOUT_FLAGS(cmd)                                                                             \
    (SEP_AESS_CMD_WITHOUT_KEYSIZE(cmd)                                                                              \
     & ~(SEP_AESS_CMD_FLAG_KEYSELECT_GID0 | SEP_AESS_CMD_FLAG_KEYSELECT_GID1 | SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM))
// #define SEP_AESS_CMD_WITHOUT_FLAGS(cmd) (cmd &
// ~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 |
// SEP_AESS_CMD_FLAG_UNKNOWN0))
////#define SEP_AESS_CMD_WITHOUT_FLAGS(cmd) (cmd &
///~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 |
/// SEP_AESS_CMD_FLAG_UNKNOWN0 | SEP_AESS_CMD_FLAG_UNKNOWN1))

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_CUSTOM(cmd)                                  \
    ((cmd) & (SEP_AESS_CMD_FLAG_KEYSELECT_GID1 | SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM))

// static keys: 0x00/0x40/0x80/0xc0
// seeded keys:
// wrapping key: key index 0: 0x01..0x05 ; key index 1: 0x41..0x45
// authentication key: key index 0: 0x81..0x04 ; key index 1: 0xc1..0x44

#define SEP_AESS_COMMAND_SYNC_SEEDBITS 0x0    // sync with register_seed_bits?
// usually in combination with SYNC_SEEDBITS
#define SEP_AESS_COMMAND_CREATE_KEY_FROM_SEED 0x3
// forces and overwrites flags, aes256 && custom. do nothing if the custom flag
// was set.
#define SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256 0x6
// forces and overwrites flags, aes256 && custom.
#define SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256 0x8
#define SEP_AESS_COMMAND_ENCRYPT_CBC                     0x9
#define SEP_AESS_COMMAND_DECRYPT_CBC                     0xA
#define SEP_AESS_COMMAND_0xB                             0xB    // custom aes key?

#define SEP_AESS_REGISTER_STATUS                          0x4
#define SEP_AESS_REGISTER_COMMAND                         0x8
#define SEP_AESS_REGISTER_INTERRUPT_STATUS                0xC
#define SEP_AESS_REGISTER_INTERRUPT_ENABLED               0x10
#define SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER 0x14
#define SEP_AESS_REGISTER_0x18_KEYDISABLE                 0x18
#define SEP_AESS_REGISTER_SEED_BITS                       0x1C
#define SEP_AESS_REGISTER_SEED_BITS_LOCK                  0x20
#define SEP_AESS_REGISTER_IV                              0x40
#define SEP_AESS_REGISTER_IN                              0x50
#define SEP_AESS_REGISTER_TAG_OUT                         0x60
#define SEP_AESS_REGISTER_OUT                             0x70

#define SEP_AESS_REGISTER_STATUS_RUN_COMMAND BIT(0)
#define SEP_AESS_REGISTER_STATUS_ACTIVE      BIT(1)
#define SEP_AESS_REGISTER_STATUS_UNKN0       BIT(8)
#define SEP_AESS_REGISTER_STATUS_UNKN1_ERROR BIT(9)
#define SEP_AESS_REGISTER_STATUS_UNKN2_ERROR BIT(10)

#define SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE                          BIT(0)
#define SEP_AESS_REGISTER_INTERRUPT_STATUS_UNRECOVERABLE_ERROR_INTERRUPT BIT(1)

#define SEP_AESS_REGISTER_INTERRUPT_ENABLED_MASK                0x3
#define SEP_AESS_REGISTER_INTERRUPT_ENABLED_RAISE_ON_COMPLETION BIT(0)
#define SEP_AESS_REGISTER_INTERRUPT_ENABLED_INTERRUPT_ENABLED   BIT(1)

#define SEP_AESS_SEED_BITS_BIT0  BIT(0)
#define SEP_AESS_SEED_BITS_BIT27 BIT(27)    // cmds 0x50 and 0x90
#define SEP_AESS_SEED_BITS_BIT28 BIT(28)    // invalid or missing EKEY tag?
// DSEC tag present, demote SEP
#define SEP_AESS_SEED_BITS_SEP_DSEC_DEMOTED BIT(29)
// ap is prod-fused and demoted
#define SEP_AESS_SEED_BITS_AP_PROD_DEMOTED BIT(30)
#define SEP_AESS_SEED_BITS_IMG4_VERIFIED   (1 << 31)    // img4 verified?

// static uint32_t AESS_UID[0x20 / 4] = {0xDEADBEEF, 0x13371337, 0xA55A5AA5,
// 0xCAFECAFE, 0xC4F3C4F3, 0xD34DB33F, 0x73317331, 0x5AA5A55A};
static uint32_t AESS_UID0[0x20 / 4]                 = {0xDEADBEEF, 0x13370000, 0xA55A0000, 0xCAFECAFE,
                                                       0xC4F3C4F3, 0xD34DB33F, 0xFF317331, 0xFFA50000};
static uint32_t AESS_UID1[0x20 / 4]                 = {0xDEADBEEF, 0x13371111, 0xA55A1111, 0xCAFECAFE,
                                                       0xC4F3C4F3, 0xD34DB33F, 0xFF317331, 0xFFA50000};
static uint32_t AESS_GID0[0x20 / 4]                 = {0xDEADBE00, 0x13371337, 0xA55A5AA5, 0xCAFECA00,
                                                       0xC4F3C400, 0xD34DB33F, 0x73317331, 0x5AA5A500};
static uint32_t AESS_GID1[0x20 / 4]                 = {0xDEADBE11, 0x13371337, 0xA55A5AA5, 0xCAFECA11,
                                                       0xC4F3C411, 0xD34DB33F, 0x73317331, 0x5AA5A511};
static uint32_t AESS_KEY_FOR_DISABLED_KEY[0x20 / 4] = {0xF00FF00F, 0xF00FF00F, 0xF00FF00F, 0xCAFECA33,
                                                       0xC4F3C488, 0xD34DB33F, 0xF00FF00F, 0xF00FF00F};
static uint32_t AESS_UID_SEED_NOT_ENABLED[0x20 / 4] = {0x0FF00FF0, 0x0FF00FF0, 0x0FF00FF0, 0xCAFECA44,
                                                       0xC4F3C499, 0xD34DB33F, 0x0FF00FF0, 0x0FF00FF0};
static uint32_t AESS_UID_SEED_INVALID[0x20 / 4]     = {0x1FF11FF1, 0x1FF11FF1, 0x1FF11FF1, 0xCAFECA55,
                                                       0xC4F3C4AA, 0xD34DB33F, 0x1FF11FF1, 0x1FF11FF1};
struct AppleSEPAESSState
{
    SysBusDevice parent_obj;

    AppleSEP* sep;
    QEMUBH*   command_bh;
    QemuMutex lock;
    uint32_t  status;                                 // 0x4
    uint32_t  command;                                // 0x8
    uint32_t  interrupt_status;                       // 0xc
    uint32_t  interrupt_enabled;                      // 0x10
    uint32_t  reg_0x14_keywrap_iterations_counter;    // 0x14
    uint32_t  reg_0x18_keydisable;                    // 0x18
    uint32_t  seed_bits;                              // 0x1c
    uint32_t  seed_bits_lock;                         // 0x20
    union
    {
        struct
        {
            uint8_t iv[16];    // 0x40 // IV for enc, IN for dec?
            uint8_t in[16];    // 0x50 // IN for enc, IV for dec?
        };
        struct
        {
            uint8_t in_dec[16];    // 0x40 // IV for enc, IN for dec?
            uint8_t iv_dec[16];    // 0x50 // IN for enc, IV for dec?
        };
        uint8_t in_full[32];    // 0x40
    };
    // uint8_t in_t8015[16];      // 0x100
    // uint8_t iv_t8015[16];      // 0x110
    union
    {
        struct
        {
            uint8_t tag_out[16];    // 0x60
            uint8_t out[16];        // 0x70
        };
        uint8_t out_full[32];    // 0x60
    };
    uint8_t key_256_in[32];      // 0x40 ; for custom key
    uint8_t key_t8015_in[16];    // 0x100 ; for custom key
    uint8_t key_256_out[32];     // 0x60 ; for custom key
    uint8_t key_128_out[16];     // 0x60 ; for custom key
    //
    uint8_t keywrap_key_uid0[32];
    uint8_t keywrap_key_uid1[32];
    uint8_t custom_key_index[4][32];
    bool    custom_key_index_enabled[4];
    // put keywrap_uid[01]_enabled here, or else ASAN will complain about
    // misalignment.
    bool         keywrap_uid0_enabled;
    bool         keywrap_uid1_enabled;
    MemoryRegion base_mr;
    uint8_t      base_regs[AESS_BASE_REG_SIZE];
};

static QCryptoCipherAlgo get_aes_cipher_alg(uint32_t flags)
{
    switch (flags
            & (SEP_AESS_CMD_FLAG_KEYSIZE_AES128 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 | SEP_AESS_CMD_FLAG_KEYSIZE_AES256))
    {
        case SEP_AESS_CMD_FLAG_KEYSIZE_AES128: return QCRYPTO_CIPHER_ALGO_AES_128;
        case SEP_AESS_CMD_FLAG_KEYSIZE_AES192: return QCRYPTO_CIPHER_ALGO_AES_192;
        case SEP_AESS_CMD_FLAG_KEYSIZE_AES256: return QCRYPTO_CIPHER_ALGO_AES_256;
        default                              : assert_not_reached();
    }
}

/*
 * @param size Size in dwords
 */
static void xor_32bit_value(uint8_t* dest, uint32_t val, int size)
{
    uint32_t tmp;
    for (int i = 0; i < size; i++) {
        memcpy(&tmp, dest + i * 4, sizeof(tmp));
        tmp ^= val;
        memcpy(dest + i * 4, &tmp, sizeof(tmp));
    }
}

static void aess_raise_interrupt(AppleSEPAESSState* s)
{
    AppleSEP* sep        = s->sep;
    s->interrupt_status |= SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
    if ((s->interrupt_enabled & SEP_AESS_REGISTER_INTERRUPT_ENABLED_MASK)
        == (SEP_AESS_REGISTER_INTERRUPT_ENABLED_INTERRUPT_ENABLED
            | SEP_AESS_REGISTER_INTERRUPT_ENABLED_RAISE_ON_COMPLETION))
    {
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x10005);    // AESS
    }
}

// TODO: This is 100% wrong, but it works anyhow/anyway.
// Somewhen, I'll have to handle keyunwrap (if that exists) and PKA.
// For the PKA ECDH command, reuse code from SSC.

static void aess_keywrap_uid(AppleSEPAESSState* s, uint8_t* in, uint8_t* out, QCryptoCipherAlgo cipher_alg,
                             uint32_t cmd, uint32_t reg_0x18_keydisable)
{    // for keywrap only
    // TODO: Second half of output might be CMAC!!!
    assert_cmpuint(cipher_alg, ==, QCRYPTO_CIPHER_ALGO_AES_256);
    QCryptoCipher* cipher;
    uint32_t       normalized_cmd = SEP_AESS_CMD_WITHOUT_FLAGS(cmd);
    size_t         key_len        = qcrypto_cipher_get_key_len(cipher_alg);
    size_t         data_len       = 0x20;
    assert_cmpuint(data_len, ==, 0x20);
    uint8_t used_key[0x20] = {0};
    if (normalized_cmd == 0x02 && s->keywrap_uid0_enabled) {
        memcpy(used_key, (uint8_t*)s->keywrap_key_uid0,
               sizeof(used_key));    // for UUID
    }
    else if (normalized_cmd == 0x12 && s->keywrap_uid1_enabled) {
        memcpy(used_key, (uint8_t*)s->keywrap_key_uid1,
               sizeof(used_key));    // for UUID
    }
    else if (normalized_cmd == 0x02 || normalized_cmd == 0x12) {
        memcpy(used_key, (uint8_t*)AESS_UID_SEED_NOT_ENABLED, sizeof(used_key));
    }
    else {
        assert_not_reached();
    }
    // TODO: Dirty hack, so iteration_register being set/unset shouldn't result
    // in the same output keys.
    xor_32bit_value(&used_key[0x10], s->reg_0x14_keywrap_iterations_counter,
                    0x8 / 4);    // seed_bits are only for keywrap
    DPRINTF("%s: cmd: 0x%02x normalized_cmd: 0x%02x cipher_alg: %u; "
            "key_len: %lu; iterations: %u, seed_bits: 0x%02x, "
            "reg_0x18_keydisable: 0x%02x\n",
            __func__, cmd, normalized_cmd, cipher_alg, key_len, s->reg_0x14_keywrap_iterations_counter, s->seed_bits,
            reg_0x18_keydisable);
    HEXDUMP("aess_keywrap_uid: used_key", used_key, sizeof(used_key));
    HEXDUMP("aess_keywrap_uid: in", in, data_len);
    cipher = qcrypto_cipher_new(cipher_alg, QCRYPTO_CIPHER_MODE_CBC, used_key, key_len, &error_abort);
    assert_nonnull(cipher);
    // uint8_t iv[0x10] = { 0 };
    // qcrypto_cipher_setiv(cipher, iv, sizeof(iv), &error_abort);
    uint8_t enc_temp[0x20] = {0};
    memcpy(enc_temp, in, sizeof(enc_temp));

    // TODO: iteration register is actually for the iterations inside the
    // algorithm, not how often the algorihm is being called.
    if (s->reg_0x14_keywrap_iterations_counter == 0) { s->reg_0x14_keywrap_iterations_counter = 1; }
    while (s->reg_0x14_keywrap_iterations_counter) {
        qcrypto_cipher_encrypt(cipher, enc_temp, enc_temp, sizeof(enc_temp), &error_abort);
        s->reg_0x14_keywrap_iterations_counter--;
    }

    memcpy(out, enc_temp, data_len);
    HEXDUMP("aess_keywrap_uid: out1", out, data_len);
    s->reg_0x14_keywrap_iterations_counter = 0;
    qcrypto_cipher_free(cipher);
    // interrupts are normally only raised by driver_ops 0x4/0x1D (keywrap) if
    // iterations_counter is over 10/0xA, but don't take that for granted.
    // aess_raise_interrupt(s); // run it always instead
}

static int aess_get_custom_keywrap_index(uint32_t cmd)
{
    switch (cmd) {
        case 0x01:
        case 0x06: return 0;
        case 0x41:
        case 0x46: return 1;
        case 0x81:
        case 0x08:
        case 0x88: return 2;
        case 0xC1:
        case 0x48:
        case 0xC8: return 3;
        default  : assert_not_reached();
    }
}

static bool check_register_0x18_KEYDISABLE_BIT_INVALID(uint32_t cmd, uint32_t reg_0x18_keydisable)
{
    uint32_t cmd_without_keysize      = SEP_AESS_CMD_WITHOUT_KEYSIZE(cmd);
    bool     reg_0x18_keydisable_bit0 = (reg_0x18_keydisable & 0x1) != 0;
    bool     reg_0x18_keydisable_bit1 = (reg_0x18_keydisable & 0x2) != 0;
    bool     reg_0x18_keydisable_bit3 = (reg_0x18_keydisable & 0x8) != 0;
    bool     reg_0x18_keydisable_bit4 = (reg_0x18_keydisable & 0x10) != 0;
    switch (cmd_without_keysize) {
        // case 0x: // driver_op == 0x09 (cmd 0x00, invalid)
        case 0x0C:
        case 0x4C:
            // cmd 0x0C or 0x4C might be driver_op 0x09, if it would exist.
            return reg_0x18_keydisable_bit4;
        // driver_op == 0x0A/0x0D (cmds 0x00/0x00, both are invalid)
        case 0x09:    // driver_op 0x0A would be most likely cmd 0x09, if using it in
                      // the _operate function would be allowed
        case 0x0A:    // driver_op 0x0D would be most likely cmd 0x0A, if using it in
                      // the _operate function would be allowed
            return reg_0x18_keydisable_bit0;
        // driver_op == 0x0B/0x0E (cmds 0x49/0x00, 0x0E is invalid)
        case 0x49:
        case 0x4A:    // driver_op 0x0E would be most likely cmd 0x4A, if using it in
                      // the _operate function would be allowed
            return reg_0x18_keydisable_bit1;
        // driver_op == 0x13/0x14 (cmds 0x0D/0x00, 0x14 is invalid)
        case 0x0D:    // 0x0D and 0x4D, are those actually implemented in real
                      // hardware?
        case 0x4D:    // driver_op 0x14 would be most likely cmd 0x4D, if using it in
                      // the _operate function would be allowed
            return reg_0x18_keydisable_bit3;
        // driver_op == 0x23/0x24 (cmds 0x50/0x90)
        case 0x50:
        case 0x90:
            // driver_ops 0x23/0x24 are not available on iOS 12, but they're on iOS
            // 14
            return reg_0x18_keydisable_bit3;
        default: break;
    }
    return false;
}

static void apple_sep_aess_handle_cmd(AppleSEPAESSState* s)
{
    uint32_t cmd                 = s->command;
    uint32_t reg_0x18_keydisable = s->reg_0x18_keydisable;

    bool              keyselect_non_gid0            = SEP_AESS_CMD_FLAG_KEYSELECT_GID1_CUSTOM(cmd) != 0;
    bool              keyselect_gid1                = (cmd & SEP_AESS_CMD_FLAG_KEYSELECT_GID1) != 0;
    bool              keyselect_custom              = (cmd & SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM) != 0;
    uint32_t          normalized_cmd                = SEP_AESS_CMD_WITHOUT_FLAGS(cmd);
    QCryptoCipherAlgo cipher_alg                    = get_aes_cipher_alg(cmd);
    size_t            key_len                       = qcrypto_cipher_get_key_len(cipher_alg);
    bool              zero_iv_two_blocks_encryption = false;
    bool register_0x18_KEYDISABLE_BIT_INVALID = check_register_0x18_KEYDISABLE_BIT_INVALID(cmd, reg_0x18_keydisable);
    bool valid_command                        = true;
    bool invalid_parameters                   = register_0x18_KEYDISABLE_BIT_INVALID;
#if 1
    // not correct behavior, but SEPFW likes to complain if it doesn't expect
    // the output to be zero, so keep it.
    memset(s->out_full, 0, sizeof(s->out_full));
#endif
#if 1
    HEXDUMP("s->in_full", s->in_full, sizeof(s->in_full));
#endif
#if 1
    // not GID1 && not Custom ; ignore the keysize flags here
    if (!keyselect_non_gid0 && normalized_cmd == SEP_AESS_COMMAND_0xB) {
        {
            memset(s->key_256_in, 0, sizeof(s->key_256_in));
            memcpy(s->key_256_in, s->in_full, sizeof(s->in_full));
        }
    }
#endif
    // Not GID1 && not Custom ; Always AES256!!
    else if (!keyselect_non_gid0 && (normalized_cmd == 0x2 || normalized_cmd == 0x12)) {
#if 1
        cipher_alg = QCRYPTO_CIPHER_ALGO_AES_256;
        // VERY important, otherwise key_len would be too short in case that
        // flag 0x200 is missing.
        key_len = qcrypto_cipher_get_key_len(cipher_alg);
        // keyselect_gid1 (= true) variable has no use here
        // key wrapping/deriving data
        uint8_t key_wrap_data_in[0x20]  = {0};
        uint8_t key_wrap_data_out[0x20] = {0};
        assert_cmpuint(sizeof(key_wrap_data_in), >=, key_len);
        memcpy(key_wrap_data_in, s->in_full, key_len);
        // aess_encrypt_decrypt_uid(s, key_wrap_data_in, key_wrap_data_out,
        // cipher_alg, true);
        ////aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg,
        /// false);
        // aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg,
        // true);
        aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg, cmd, reg_0x18_keydisable);
        // qemu_guest_getrandom_nofail(key_wrap_data_out, sizeof(
        // key_wrap_data_out)); // For testing if random output breaks stuff.
        memcpy(s->out_full, key_wrap_data_out, key_len);
#endif
    }
#if 1
    else if (normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC || normalized_cmd == SEP_AESS_COMMAND_DECRYPT_CBC
             || normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256
             || normalized_cmd
                    == SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256) /* GID0 || GID1 || Custom */
    {
        bool custom_encryption = false;
        DPRINTF("%s: cmd 0x%03x ; ", __func__, cmd);
        HEXDUMP("s->in_full", s->in_full, sizeof(s->in_full));
        if (normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256) {
            if (keyselect_custom) {    // 0x80
                goto jump_return;      // valid: 0x206, 0x246; invalid: 0x286, 0x2C6
            }
            normalized_cmd = SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256;
        }
        if (normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256) {
            if (!keyselect_custom) { zero_iv_two_blocks_encryption = true; }
            custom_encryption = true;
            // use_aes256 = true; // variable only used for gid decryption
            keyselect_non_gid0 = true;
            keyselect_gid1     = false;
            keyselect_custom   = true;
            normalized_cmd     = SEP_AESS_COMMAND_ENCRYPT_CBC;
            cipher_alg         = QCRYPTO_CIPHER_ALGO_AES_256;
            key_len            = qcrypto_cipher_get_key_len(cipher_alg);
        }
        bool    do_encryption  = (normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC);
        uint8_t used_key[0x20] = {0};
        if (custom_encryption) {
            int custom_keywrap_index = aess_get_custom_keywrap_index(cmd & 0xFF);
            if (s->custom_key_index_enabled[custom_keywrap_index]) {
                memcpy(used_key, s->custom_key_index[custom_keywrap_index], sizeof(used_key));
            }
            // Custom takes precedence over GID0 or GID1
        }
        else if (keyselect_custom) {
            memcpy(used_key, s->key_256_in, sizeof(used_key));    // for custom
        }
        else {
            if (register_0x18_KEYDISABLE_BIT_INVALID) {
                memcpy(used_key, (uint8_t*)AESS_KEY_FOR_DISABLED_KEY, sizeof(used_key));
            }
            else if (keyselect_gid1) {
                memcpy(used_key, (uint8_t*)AESS_GID1,
                       sizeof(used_key));    // for GID1
            }
            else {
                memcpy(used_key, (uint8_t*)AESS_GID0,
                       sizeof(used_key));    // for GID0
            }
        }
        QCryptoCipher* cipher;
        cipher = qcrypto_cipher_new(cipher_alg, QCRYPTO_CIPHER_MODE_CBC, used_key, key_len, &error_abort);
        assert_nonnull(cipher);
        uint8_t iv[0x10] = {0};
        uint8_t in[0x10] = {0};
        if (do_encryption) {
            memcpy(iv, s->iv, sizeof(iv));
            memcpy(in, s->in, sizeof(in));
            //} else if (normalized_cmd == SEP_AESS_COMMAND_DECRYPT_CBC) {
        }
        else {
            memcpy(iv, s->iv_dec, sizeof(iv));
            memcpy(in, s->in_dec, sizeof(in));
        }
        if (zero_iv_two_blocks_encryption) {
            qcrypto_cipher_encrypt(cipher, s->in_full, s->out_full, sizeof(s->in_full), &error_abort);
            // if ((cmd & 0xF) == 0x9)
        }
        else if (do_encryption) {
            qcrypto_cipher_setiv(cipher, iv, sizeof(iv),
                                 &error_abort);    // sizeof(iv) == 0x10 on 256 and 128
            qcrypto_cipher_encrypt(cipher, s->in, s->out, sizeof(s->in), &error_abort);
            memcpy(s->tag_out, iv, sizeof(iv));
        }
        else {
            qcrypto_cipher_decrypt(cipher, in, s->tag_out, sizeof(in), &error_abort);
            qcrypto_cipher_setiv(cipher, iv, sizeof(iv),
                                 &error_abort);    // sizeof(iv) == 0x10 on 256 and 128
            qcrypto_cipher_decrypt(cipher, in, s->out, sizeof(in), &error_abort);
        }
        qcrypto_cipher_free(cipher);
    }
#endif
#if 1
    // cmd 0x40 == sync seed_bits for keywrap cmd 0x2
    // effect for wrap/UID, no effect for GID/custom?
    else if (normalized_cmd == 0x00) {
        if (keyselect_gid1) {
            memcpy(s->keywrap_key_uid0, (uint8_t*)AESS_UID0,
                   sizeof(s->keywrap_key_uid0));    // for UUID
            xor_32bit_value(&s->keywrap_key_uid0[0x8], s->seed_bits,
                            0x8 / 4);    // seed_bits are only for keywrap
            ////xor_32bit_value(&s->keywrap_key_uid0[0x18],
            /// s->reg_0x18_KEYDISABLE, 0x8/4);
            // NOT AFFECTED by REG_0x18???
            s->keywrap_uid0_enabled = true;
            DPRINTF("SEP AESS_BASE: %s: Copied seed_bits for uid0 0x%X\n", __func__, s->seed_bits);
        }
    }
#endif
#if 1
    // cmd 0x50 == sync seed_bits for keywrap cmd 0x12
    else if (normalized_cmd == 0x10) {
        if (keyselect_gid1) {
            // this is conditional memcpy is actually needed, because the result
            // will change if reg_0x18_BIT3 is set
            if (invalid_parameters) {
                memcpy(s->keywrap_key_uid1, (uint8_t*)AESS_UID_SEED_INVALID, sizeof(s->keywrap_key_uid1));
            }
            else {
                memcpy(s->keywrap_key_uid1, (uint8_t*)AESS_UID1,
                       sizeof(s->keywrap_key_uid1));    // for UUID
            }
            // this xor should happen, even if invalid_parameters is activated
            xor_32bit_value(&s->keywrap_key_uid1[0x8], s->seed_bits,
                            0x8 / 4);    // seed_bits are only for keywrap
            ////// NOT AFFECTED by REG_0x18???
            /// xor_32bit_value(&s->keywrap_key_uid1[0x18],
            /// s->reg_0x18_KEYDISABLE, 0x8/4);
            // actually affected by reg_0x18?
            s->keywrap_uid1_enabled = true;
            DPRINTF("SEP AESS_BASE: %s: Copied seed_bits for uid1 0x%X\n", __func__, s->seed_bits);
        }
    }
#endif
#if 0
    // cmd 0xc0 AESSEP_OPERATION_CREATE_D
    // cmd 0x43 AESSEP_OPERATION_CREATE_FROM_GEN2D
#endif
#if 1
    // sync/set key for command 0x206(0x201), 0x246(0x241), 0x208/0x288(0x281),
    // 0x248/0x2C8(0x2C1)
    else if (normalized_cmd == 0x1) {
        int custom_keywrap_index = aess_get_custom_keywrap_index(cmd & 0xFF);
        memcpy(s->custom_key_index[custom_keywrap_index], s->in_full,
               sizeof(s->custom_key_index[custom_keywrap_index]));
        // unset (real zero-key) != zero-key set (not real zero-key)
        xor_32bit_value(s->custom_key_index[custom_keywrap_index], 0xDEADBEEF, 0x20 / 4);
        s->custom_key_index_enabled[custom_keywrap_index] = true;
        DPRINTF("SEP AESS_BASE: %s: sync/set key command 0x%02x "
                "cmd 0x%02x\n",
                __func__, normalized_cmd, cmd);
    }
#endif
// TODO: other sync commands: 0x205(0x201), 0x204(0x281), 0x245(0x241),
// 0x244(0x2C1)
#if 0
    else if (normalized_cmd == 0x...)
    {
    }
#endif
    else {
        DPRINTF("SEP AESS_BASE: %s: Unknown command 0x%02x\n", __func__, cmd);
        // valid_command = false;
    }

jump_return:
    // comment this out when not using async
    // if using QEMU_LOCK_GUARD (non-WITH_) in write
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        invalid_parameters |= !valid_command;
        if (invalid_parameters) {
            // always keep this flag
            s->interrupt_status |= SEP_AESS_REGISTER_INTERRUPT_STATUS_UNRECOVERABLE_ERROR_INTERRUPT;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unrecoverable_error just got raised, SEP will "
                          "panic soon.: cmd 0x%03x\n",
                          __func__, cmd);
        }
        s->status &= ~SEP_AESS_REGISTER_STATUS_ACTIVE;
        // call raise_interrupt always instead of only on keywrap, because it's
        // checking conditions
        aess_raise_interrupt(s);
    }
}

static void apple_sep_aess_handle_cmd_bh(void* opaque)
{
    AppleSEPAESSState* s = opaque;
    apple_sep_aess_handle_cmd(s);
}

static void apple_sep_aess_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPAESSState* s         = opaque;
    AppleSEP*          sep       = s->sep;
    uint64_t           orig_data = data;

    // QEMU_LOCK_GUARD(&s->lock);

#ifdef ENABLE_CPU_DUMP_STATE
    DPRINTF("\n");
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_AESS_REGISTER_STATUS:    // Status
            if ((data & SEP_AESS_REGISTER_STATUS_RUN_COMMAND) != 0) {
                data &= ~SEP_AESS_REGISTER_STATUS_RUN_COMMAND;
                data |= SEP_AESS_REGISTER_STATUS_ACTIVE;
                WITH_QEMU_LOCK_GUARD(&s->lock)
                {
                    s->status            = data;    // surely no bitwise OR?
                    s->interrupt_status &= ~SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
                }
                apple_sep_aess_handle_cmd(s);
                // qemu_bh_schedule(s->command_bh);
            }
            goto jump_log;
        case SEP_AESS_REGISTER_COMMAND:         // Command
            data       &= SEP_AESS_CMD_MASK;    // for T8020
            s->command  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_STATUS:    // Interrupt Status
            WITH_QEMU_LOCK_GUARD(&s->lock)
            {
                if ((data & SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE) != 0) {
                    s->interrupt_status &= ~SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
                }
            }
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_ENABLED:    // Interrupt Enabled
            // bit1 == maybe enable interrupt(s)
            // bit0 == maybe activate interrupt when command is done ;
            // ... used for keywrap with > 10/0xA iterations
            data                 &= SEP_AESS_REGISTER_INTERRUPT_ENABLED_MASK;
            s->interrupt_enabled  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER:    // has affect on
                                                                   // keywrap
            s->reg_0x14_keywrap_iterations_counter = data;
            goto jump_log;
        case SEP_AESS_REGISTER_0x18_KEYDISABLE:    // has affect on keywrap
            data                   |= s->reg_0x18_keydisable;
            data                   &= 0x1B;
            s->reg_0x18_keydisable  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_SEED_BITS:    // seed_bits ;; has affect on keywrap ;;
                                             // offset 0x1C == flags offset: stores
                                             // flags, like if the device has been
                                             // demoted (bit 30). On T8010, the bits
                                             // are between 28 and 31, on T8020, the
                                             // bits are between 27 and 31.
            data         &= ~s->seed_bits_lock;
            data         |= s->seed_bits & s->seed_bits_lock;
            s->seed_bits  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_SEED_BITS_LOCK:         // seed_bits_lock ;; has no affect on
                                                       // keywrap?
            data              |= s->seed_bits_lock;    // don't allow unsetting
            s->seed_bits_lock  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_IV ... SEP_AESS_REGISTER_IV + 0xC:    // IV
        case 0x100 ... 0x10C                                    :    // IV T8015
            memcpy(&s->iv[addr & 0xF], &data, 4);
            goto jump_log;
        case SEP_AESS_REGISTER_IN ... SEP_AESS_REGISTER_IN + 0xC:    // IN
        case 0x110 ... 0x11C                                    :    // IN T8015
            memcpy(&s->in[addr & 0xF], &data, 4);
            goto jump_log;
        // AES engine?: case 0xA4: 0x40 bytes from TRNG
        default:
        jump_default:
            memcpy(&s->base_regs[addr], &data, size);
        jump_log:
            DPRINTF("SEP AESS_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, orig_data);
            break;
    }
}

static uint64_t apple_sep_aess_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPAESSState* s   = opaque;
    AppleSEP*          sep = s->sep;
    uint64_t           ret = 0;

    // QEMU_LOCK_GUARD(&s->lock);

#ifdef ENABLE_CPU_DUMP_STATE
    DPRINTF("\n");
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_AESS_REGISTER_STATUS:    // Status
            WITH_QEMU_LOCK_GUARD(&s->lock) { ret = s->status; }
            goto jump_log;
        case SEP_AESS_REGISTER_COMMAND:    // Command
            ret = s->command;
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_STATUS:    // Interrupt Status
            WITH_QEMU_LOCK_GUARD(&s->lock) { ret = s->interrupt_status; }
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_ENABLED:    // Interrupt Enabled
            ret = s->interrupt_enabled;
            goto jump_log;
        case SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER:
            ret = s->reg_0x14_keywrap_iterations_counter;
            goto jump_log;
        case SEP_AESS_REGISTER_0x18_KEYDISABLE: ret = s->reg_0x18_keydisable; goto jump_log;
        case SEP_AESS_REGISTER_SEED_BITS      :    // seed_bits
            ret = s->seed_bits;
            goto jump_log;
        case SEP_AESS_REGISTER_SEED_BITS_LOCK:    // seed_bits_lock
            ret = s->seed_bits_lock;
            goto jump_log;
        case SEP_AESS_REGISTER_IV ... SEP_AESS_REGISTER_IV + 0xC:    // IV
            ////case 0x100 ... 0x10C: // IV T8015 ; is this also being read?
            memcpy(&ret, &s->iv[addr & 0xF], 4);
            goto jump_log;
        case SEP_AESS_REGISTER_IN ... SEP_AESS_REGISTER_IN + 0xC:    // IN
            ////case 0x110 ... 0x11C: // IN T8015 ; is this also being read?
            memcpy(&ret, &s->in[addr & 0xF], 4);
            goto jump_log;
        case SEP_AESS_REGISTER_TAG_OUT ... SEP_AESS_REGISTER_TAG_OUT + 0xC:    // TAG OUT
            memcpy(&ret, &s->tag_out[addr & 0xF], 4);
            goto jump_log;
        case SEP_AESS_REGISTER_OUT ... SEP_AESS_REGISTER_OUT + 0xC:    // OUT
            memcpy(&ret, &s->out[addr & 0xF], 4);
            goto jump_log;
        case 0xE4:    // ????
            ret = 0x0;
            goto jump_log;
        case 0x280:    // ????
            ret = 0x1;
            goto jump_log;
        default:
        jump_default:
            memcpy(&ret, &s->base_regs[addr], size);
        jump_log:
            DPRINTF("SEP AESS_BASE: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps apple_sep_aess_base_reg_ops = {
    .write                 = apple_sep_aess_base_reg_write,
    .read                  = apple_sep_aess_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_aess_reset_enter(Object* obj, ResetType type)
{
    AppleSEPAESSState* s = APPLE_SEP_AESS(obj);

    s->status                              = 0;
    s->command                             = 0;
    s->interrupt_status                    = 0;
    s->interrupt_enabled                   = 0;
    s->reg_0x14_keywrap_iterations_counter = 0;
    s->reg_0x18_keydisable                 = 0;
    s->seed_bits                           = 0;
    s->seed_bits_lock                      = 0;

    s->keywrap_uid0_enabled = false;
    s->keywrap_uid1_enabled = false;
    memset(s->keywrap_key_uid0, 0, sizeof(s->keywrap_key_uid0));
    memset(s->keywrap_key_uid1, 0, sizeof(s->keywrap_key_uid1));
    memset(s->custom_key_index, 0, sizeof(s->custom_key_index));
    memset(s->custom_key_index_enabled, 0, sizeof(s->custom_key_index_enabled));
    memset(s->base_regs, 0, sizeof(s->base_regs));
}

static void apple_sep_aess_init(Object* obj)
{
    AppleSEPAESSState* s = APPLE_SEP_AESS(obj);

    qemu_mutex_init(&s->lock);
    s->command_bh = aio_bh_new(qemu_get_aio_context(), apple_sep_aess_handle_cmd_bh, s);
}

static void apple_sep_aess_realize(DeviceState* dev, Error** errp)
{
    AppleSEPAESSState* s   = APPLE_SEP_AESS(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(dev), &apple_sep_aess_base_reg_ops, s, "base", AESS_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
}

static void apple_sep_aess_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_aess_reset_enter;
    dc->realize      = apple_sep_aess_realize;
}

struct AppleSEPAESHState
{
    SysBusDevice parent_obj;

    AppleSEP* sep;
    QEMUBH*   command_bh;
    QemuMutex lock;
    uint32_t  status;               // 0x4
    uint32_t  command;              // 0x8
    uint32_t  interrupt_status;     // 0xc
    uint32_t  interrupt_enabled;    // 0x10
    // uint32_t reg_0x14_keywrap_iterations_counter; // 0x14
    // uint32_t reg_0x18_keydisable; // 0x18
    // uint32_t seed_bits; // 0x1c
    // uint32_t seed_bits_lock; // 0x20
    // union {
    //     struct {
    //         uint8_t iv[16]; // 0x40 // IV for enc, IN for dec?
    //         uint8_t in[16]; // 0x50 // IN for enc, IV for dec?
    //     };
    //     struct {
    //         uint8_t in_dec[16]; // 0x40 // IV for enc, IN for dec?
    //         uint8_t iv_dec[16]; // 0x50 // IN for enc, IV for dec?
    //     };
    //     uint8_t in_full[32]; // 0x40
    // };
    // // uint8_t in_t8015[16];      // 0x100
    // // uint8_t iv_t8015[16];      // 0x110
    // union {
    //     struct {
    //         uint8_t tag_out[16]; // 0x60
    //         uint8_t out[16]; // 0x70
    //     };
    //     uint8_t out_full[32]; // 0x60
    // };
    // uint8_t key_256_in[32]; // 0x40 ; for custom key
    // uint8_t key_t8015_in[16]; // 0x100 ; for custom key
    // uint8_t key_256_out[32]; // 0x60 ; for custom key
    // uint8_t key_128_out[16]; // 0x60 ; for custom key
    // //
    // uint8_t keywrap_key_uid0[32];
    // uint8_t keywrap_key_uid1[32];
    // uint8_t custom_key_index[4][32];
    // bool custom_key_index_enabled[4];
    // // put keywrap_uid[01]_enabled here, or else ASAN will complain about
    // // misalignment.
    // bool keywrap_uid0_enabled;
    // bool keywrap_uid1_enabled;
    MemoryRegion base_mr;
    uint8_t      base_regs[AESH_BASE_REG_SIZE];
};

static void apple_sep_aesh_raise_interrupt(AppleSEPAESHState* s)
{
    AppleSEP* sep        = s->sep;
    s->interrupt_status |= SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
    if ((s->interrupt_enabled & SEP_AESS_REGISTER_INTERRUPT_ENABLED_MASK)
        == (SEP_AESS_REGISTER_INTERRUPT_ENABLED_INTERRUPT_ENABLED
            | SEP_AESS_REGISTER_INTERRUPT_ENABLED_RAISE_ON_COMPLETION))
    {
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x10009);    // AESH
    }
}

static void apple_sep_aesh_handle_cmd(AppleSEPAESHState* s)
{
    uint32_t cmd = s->command;

    uint32_t          normalized_cmd                = SEP_AESS_CMD_WITHOUT_FLAGS(cmd);
    QCryptoCipherAlgo cipher_alg                    = get_aes_cipher_alg(cmd);
    size_t            key_len                       = qcrypto_cipher_get_key_len(cipher_alg);
    bool              zero_iv_two_blocks_encryption = false;
    bool              valid_command                 = true;
    bool              invalid_parameters            = false;
#if 1
    // not correct behavior, but SEPFW likes to complain if it doesn't expect
    // the output to be zero, so keep it.
    // memset(s->out_full, 0, sizeof(s->out_full));
#endif
    if (normalized_cmd == 0xff && 0) { }
    else {
        DPRINTF("SEP AESH_BASE: %s: Unknown command 0x%02x\n", __func__, cmd);
        // valid_command = false;
    }

jump_return:
    // comment this out when not using async
    // if using QEMU_LOCK_GUARD (non-WITH_) in write
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        invalid_parameters |= !valid_command;
        if (invalid_parameters) {
            // always keep this flag
            s->interrupt_status |= SEP_AESS_REGISTER_INTERRUPT_STATUS_UNRECOVERABLE_ERROR_INTERRUPT;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unrecoverable_error just got raised, SEP will "
                          "panic soon.: cmd 0x%03x\n",
                          __func__, cmd);
        }
        s->status &= ~SEP_AESS_REGISTER_STATUS_ACTIVE;
        // call raise_interrupt always instead of only on keywrap, because it's
        // checking conditions
        apple_sep_aesh_raise_interrupt(s);
    }
}

static void apple_sep_aesh_handle_cmd_bh(void* opaque)
{
    AppleSEPAESHState* s = opaque;
    apple_sep_aesh_handle_cmd(s);
}

static void apple_sep_aesh_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPAESHState* s   = opaque;
    AppleSEP*          sep = s->sep;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_AESS_REGISTER_STATUS:    // Status
            if ((data & SEP_AESS_REGISTER_STATUS_RUN_COMMAND) != 0) {
                data &= ~SEP_AESS_REGISTER_STATUS_RUN_COMMAND;
                data |= SEP_AESS_REGISTER_STATUS_ACTIVE;
                WITH_QEMU_LOCK_GUARD(&s->lock)
                {
                    s->status            = data;    // surely no bitwise OR?
                    s->interrupt_status &= ~SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
                }
                apple_sep_aesh_handle_cmd(s);
                // qemu_bh_schedule(s->command_bh);
            }
            goto jump_log;
        case SEP_AESS_REGISTER_COMMAND:    // Command
            // should be the same as AESS, just with different commands.
            // 0x2/0x18/0x1/0x8/0x10/0xc/0x1c/0x0/0x4/0x9
            data       &= SEP_AESS_CMD_MASK;    // for T8020??????
            s->command  = data;
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_STATUS:    // Interrupt Status
            WITH_QEMU_LOCK_GUARD(&s->lock)
            {
                if ((data & SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE) != 0) {
                    s->interrupt_status &= ~SEP_AESS_REGISTER_INTERRUPT_STATUS_DONE;
                }
            }
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_ENABLED:    // Interrupt Enabled
            // bit1 == maybe enable interrupt(s)
            // bit0 == maybe activate interrupt when command is done ;
            // ... used for keywrap with > 10/0xA iterations
            data                 &= SEP_AESS_REGISTER_INTERRUPT_ENABLED_MASK;
            s->interrupt_enabled  = data;
            goto jump_log;
        // case 0x8: // cmd
        //     goto jump_default;
        // case 0xB4: 0x40 bytes from TRNG
        default:
        jump_default:
            memcpy(&s->base_regs[addr], &data, size);
        jump_log:
            DPRINTF("SEP AESH_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t apple_sep_aesh_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPAESHState* s   = opaque;
    AppleSEP*          sep = s->sep;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_AESS_REGISTER_STATUS:    // Status
            WITH_QEMU_LOCK_GUARD(&s->lock) { ret = s->status; }
            goto jump_log;
        case SEP_AESS_REGISTER_COMMAND:    // Command
            ret = s->command;
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_STATUS:    // Interrupt Status
            WITH_QEMU_LOCK_GUARD(&s->lock) { ret = s->interrupt_status; }
            goto jump_log;
        case SEP_AESS_REGISTER_INTERRUPT_ENABLED:    // Interrupt Enabled
            ret = s->interrupt_enabled;
            goto jump_log;
        // // from misc0: 0xC, 0xF4
        // case 0xC: // ???? bit1 clear, bit0 set ; REGISTER_INTERRUPT_STATUS
        //     return (0 << 1) | (1 << 0);
        case 0xF4:    // ????
            return 0x0;
        default:
        jump_default:
            memcpy(&ret, &s->base_regs[addr], size);
        jump_log:
            DPRINTF("SEP AESH_BASE: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps aesh_base_reg_ops = {
    .write                 = apple_sep_aesh_base_reg_write,
    .read                  = apple_sep_aesh_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_aesh_reset_enter(Object* obj, ResetType type)
{
    AppleSEPAESHState* s = APPLE_SEP_AESH(obj);

    s->status            = 0;
    s->command           = 0;
    s->interrupt_status  = 0;
    s->interrupt_enabled = 0;
    // s->reg_0x14_keywrap_iterations_counter = 0;
    // s->reg_0x18_keydisable = 0;
    // s->seed_bits = 0;
    // s->seed_bits_lock = 0;
    //
    // s->keywrap_uid0_enabled = false;
    // s->keywrap_uid1_enabled = false;
    // memset(s->keywrap_key_uid0, 0, sizeof(s->keywrap_key_uid0));
    // memset(s->keywrap_key_uid1, 0, sizeof(s->keywrap_key_uid1));
    // memset(s->custom_key_index, 0, sizeof(s->custom_key_index));
    // memset(s->custom_key_index_enabled, 0, sizeof(s->custom_key_index_enabled));
    memset(s->base_regs, 0, sizeof(s->base_regs));
}

static void apple_sep_aesh_init(Object* obj)
{
    AppleSEPAESHState* s = APPLE_SEP_AESH(obj);

    qemu_mutex_init(&s->lock);
    s->command_bh = aio_bh_new(qemu_get_aio_context(), apple_sep_aesh_handle_cmd_bh, s);
}

static void apple_sep_aesh_realize(DeviceState* dev, Error** errp)
{
    AppleSEPAESHState* s   = APPLE_SEP_AESH(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    // at least >= t8015 have this (aesh), according to their seprom's, but I'm
    // not sure about s8000
    memory_region_init_io(&s->base_mr, OBJECT(s), &aesh_base_reg_ops, s, "base", AESH_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
}

static void apple_sep_aesh_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_aesh_reset_enter;
    dc->realize      = apple_sep_aesh_realize;
}

AppleSEPAESHState* apple_sep_aesh_create(AppleSEP* sep)
{
    AppleSEPAESHState* s = APPLE_SEP_AESH(qdev_new(TYPE_APPLE_SEP_AESH));

    s->sep = sep;

    return s;
}

struct AppleSEPAESCState
{
    SysBusDevice parent_obj;

    MemoryRegion base_mr;
    uint8_t      base_regs[AESC_BASE_REG_SIZE];
};

static void aesc_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPAESCState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
            memcpy(&s->base_regs[addr], &data, size);
            DPRINTF("SEP AESC_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t aesc_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPAESCState* s   = opaque;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    apple_sep_dump_cpu_handler();
#endif
    switch (addr) {
        default:
            memcpy(&ret, &s->base_regs[addr], size);
            DPRINTF("SEP AESC_BASE: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps aesc_base_reg_ops = {
    .write                 = aesc_base_reg_write,
    .read                  = aesc_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_aesc_reset_enter(Object* obj, ResetType type)
{
    AppleSEPAESCState* s = APPLE_SEP_AESC(obj);

    memset(s->base_regs, 0, sizeof(s->base_regs));
}

static void apple_sep_aesc_realize(DeviceState* dev, Error** errp)
{
    AppleSEPAESCState* s   = APPLE_SEP_AESC(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(dev), &aesc_base_reg_ops, s, "base", AESC_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
}

static void apple_sep_aesc_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_aesc_reset_enter;
    dc->realize      = apple_sep_aesc_realize;
}

static const TypeInfo apple_sep_aes_types[] = {
    {
        .name       = TYPE_APPLE_SEP_AESS,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .class_init = apple_sep_aess_class_init,
        OBJECT_TYPE_INSTANCE(AppleSEPAESSState),
        .instance_init = apple_sep_aess_init,
    },
    {
        .name       = TYPE_APPLE_SEP_AESH,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .class_init = apple_sep_aesh_class_init,
        OBJECT_TYPE_INSTANCE(AppleSEPAESHState),
        .instance_init = apple_sep_aesh_init,
    },
    {
        .name       = TYPE_APPLE_SEP_AESC,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .class_init = apple_sep_aesc_class_init,
        OBJECT_TYPE_INSTANCE(AppleSEPAESCState),
    },
};

DEFINE_TYPES(apple_sep_aes_types)

AppleSEPAESCState* apple_sep_aesc_create(void) { return APPLE_SEP_AESC(qdev_new(TYPE_APPLE_SEP_AESC)); }

AppleSEPAESSState* apple_sep_aess_create(AppleSEP* sep)
{
    AppleSEPAESSState* s = APPLE_SEP_AESS(qdev_new(TYPE_APPLE_SEP_AESS));

    s->sep = sep;

    return s;
}
