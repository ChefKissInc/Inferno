/*
 * Apple SEP True Random Number Generator.
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
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "crypto/cipher.h"
#include "hw/arm/sep/private.h"
#include <nettle/drbg-ctr.h>
#include <nettle/version.h>
#include <nettle/macros.h>
#include <nettle/memxor.h>

#define TRNG_REGS_REG_SIZE (0x10000)    // T8015/T8030

#define REG_TRNG_INOUT_START      (0x00)
#define REG_TRNG_INOUT_END        (0x0C)
#define REG_TRNG_STATUS           (0x10)
#define TRNG_STATUS_READY         BIT(0)
#define TRNG_STATUS_SHUTDOWN_OVFL BIT(1)
#define TRNG_STATUS_STUCK         BIT(2)
#define TRNG_STATUS_NOISE_FAIL    BIT(3)
#define TRNG_STATUS_RUN_FAIL      BIT(4)
#define TRNG_STATUS_LONG_RUN_FAIL BIT(5)
#define TRNG_STATUS_POKER_FAIL    BIT(6)
#define TRNG_STATUS_MONOBIT_FAIL  BIT(7)
#define TRNG_STATUS_TEST_READY    BIT(8)
#define TRNG_STATUS_STUCK_NRBG    BIT(9)
#define TRNG_STATUS_RESEED_AI     BIT(10)
#define TRNG_STATUS_REPCNT_FAIL   BIT(13)
#define TRNG_STATUS_APROP_FAIL    BIT(14)
#define TRNG_STATUS_TEST_STUCK    BIT(15)
// blocks_available 16..23
// blocks_threshold 24..30
#define TRNG_STATUS_NEED_CLOCK         BIT(31)
#define REG_TRNG_CONTROL               (0x14)
#define TRNG_CONTROL_READY             BIT(0)
#define TRNG_CONTROL_SHUTDOWN_OVFLO    BIT(1)
#define TRNG_CONTROL_STUCK             BIT(2)
#define TRNG_CONTROL_NOISE_FAIL        BIT(3)
#define TRNG_CONTROL_RUN_FAIL          BIT(4)
#define TRNG_CONTROL_LONG_RUN_FAIL     BIT(5)
#define TRNG_CONTROL_POKER_FAIL        BIT(6)
#define TRNG_CONTROL_MONOBIT_FAIL      BIT(7)
#define TRNG_CONTROL_TEST_MODE         BIT(8)
#define TRNG_CONTROL_STUCK_NRBG        BIT(9)
#define TRNG_CONTROL_ENABLED           BIT(10)
#define TRNG_CONTROL_DRBG_ENABLED      BIT(12)
#define TRNG_CONTROL_REP_CNT_FAIL_MASK BIT(13)
#define TRNG_CONTROL_APROP_FAIL_MASK   BIT(14)
#define TRNG_CONTROL_RESEED            BIT(15)
#define TRNG_CONTROL_REQUEST_DATA      BIT(16)
#define TRNG_CONTROL_REQUEST_HOLD      BIT(17)
#define TRNG_CONTROL_DATA_BLOCKS(v)    (((v) >> 20) & 0xFFF)
#define REG_TRNG_CONFIG                (0x18)
#define TRNG_CONFIG_NOISE_BLOCKS(v)    ((v) & 0x1F)
#define TRNG_CONFIG_USE_STARTUP_BITS   BIT(5)
#define TRNG_CONFIG_SCALE(v)           (((v) >> 6) & 0x3)
#define TRNG_CONFIG_SAMPLE_DIV(v)      (((v) >> 8) & 0xF)
#define TRNG_CONFIG_READ_TIMEOUT(v)    (((v) >> 12) & 0xF)
#define TRNG_CONFIG_SAMPLE_CYCLES(v)   (((v) >> 16) & 0xFFFF)
#define REG_TRNG_UNKN0                 (0x1C)
#define REG_TRNG_UNKN1                 (0x20)
#define REG_TRNG_UNKN2                 (0x24)
#define REG_TRNG_UNKN3                 (0x28)
#define REG_TRNG_UNKN4                 (0x2C)
#define REG_TRNG_AES_KEY_BASE          (0x40)
#define REG_TRNG_AES_KEY_END           (0x5C)
#define REG_TRNG_ECID_LOW              (0x60)
#define REG_TRNG_ECID_HI               (0x64)
#define REG_TRNG_COUNTER_LOW           (0x68)
#define REG_TRNG_COUNTER_HI            (0x6C)
#define REG_TRNG_UNKN5                 (0x70)
#define TRNG_UNKN5_ENCRYPT_FIFO        BIT(6)
#define TRNG_UNKN5_INIT_DRBG           BIT(7)
#define REG_TRNG_UNKN6                 (0x78)
#define TRNG_UNKN6_UNKN0               BIT(19)
#define TRNG_UNKN6_UNKN1               BIT(20)
#define TRNG_UNKN6_UNKN2               BIT(23)
#define REG_TRNG_UNKN7                 (0x7C)

struct AppleSEPTRNGState
{
    SysBusDevice parent_obj;

    AppleSEP*                  sep;
    MemoryRegion               regs_mr;
    uint8_t                    key[32];
    uint8_t                    fifo[16];
    uint32_t                   offset_0x70;
    uint64_t                   ecid;
    uint64_t                   counter;
    uint32_t                   config;
    bool                       ctr_drbg_init;
    struct drbg_ctr_aes256_ctx ctr_drbg_rng;
};

// many thanks to the nettle guy(s) for finally exporting the drbg_ctr_aes256_update function!
// LIBNETTLE_MAJOR is not exported, and not sure what to make about the "PACKAGE_VERSION=snapshot" in nettle's gitlab-ci
// file.
// #if LIBNETTLE_MAJOR <= 8
#if NETTLE_VERSION_MAJOR <= 3
static inline void block16_set(union nettle_block16* r, const union nettle_block16* x)
{
    r->u64[0] = x->u64[0];
    r->u64[1] = x->u64[1];
}
static void drbg_ctr_aes256_output(const struct aes256_ctx* key, union nettle_block16* V, size_t n, uint8_t* dst)
{
    for (; n >= AES_BLOCK_SIZE; n -= AES_BLOCK_SIZE, dst += AES_BLOCK_SIZE) {
        INCREMENT(AES_BLOCK_SIZE, V->b);
        aes256_encrypt(key, AES_BLOCK_SIZE, dst, V->b);
    }
    if (n > 0) {
        union nettle_block16 block;

        INCREMENT(AES_BLOCK_SIZE, V->b);
        aes256_encrypt(key, AES_BLOCK_SIZE, block.b, V->b);
        memcpy(dst, block.b, n);
    }
}
static void drbg_ctr_aes256_update(struct aes256_ctx* key, union nettle_block16* V, const uint8_t* provided_data)
{
    union nettle_block16 tmp[3];
    drbg_ctr_aes256_output(key, V, DRBG_CTR_AES256_SEED_SIZE, tmp[0].b);

    if (provided_data) { memxor(tmp[0].b, provided_data, DRBG_CTR_AES256_SEED_SIZE); }

    aes256_set_encrypt_key(key, tmp[0].b);
    block16_set(V, &tmp[2]);
}
#endif

static void trng_regs_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPTRNGState* s   = opaque;
    AppleSEP*          sep = s->sep;
    uint32_t           enabled;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    DPRINTF("TRNG_REGS: Write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64 "\n", addr, data);

    switch (addr) {
        case REG_TRNG_INOUT_START ... REG_TRNG_INOUT_END:
            if ((s->offset_0x70 & TRNG_UNKN5_ENCRYPT_FIFO) != 0) { data = bswap32(data); }
            memcpy(s->fifo + (addr - REG_TRNG_INOUT_START), &data, size);
            if (addr == REG_TRNG_INOUT_END && ((s->offset_0x70 & TRNG_UNKN5_ENCRYPT_FIFO) != 0)) {
                QCryptoCipher* cipher;

                cipher = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_256, QCRYPTO_CIPHER_MODE_ECB, s->key,
                                            sizeof(s->key), &error_abort);
                assert_nonnull(cipher);
                qcrypto_cipher_encrypt(cipher, s->fifo, s->fifo, sizeof(s->fifo), &error_abort);
                qcrypto_cipher_free(cipher);
            }
            break;
        case REG_TRNG_STATUS:
            // enabled = (s->config & TRNG_CONTROL_ENABLED) != 0;
            if ((data & TRNG_STATUS_READY) != 0
                && (s->offset_0x70 & (TRNG_UNKN5_ENCRYPT_FIFO | TRNG_UNKN5_INIT_DRBG)) == 0)
            {
                qemu_guest_getrandom_nofail(s->fifo, sizeof(s->fifo));
                if ((s->config & TRNG_CONTROL_SHUTDOWN_OVFLO) != 0) {
                    apple_a7iop_interrupt_status_push(sep->mailbox,
                                                      0x10003);    // TRNG
                }
            }
            break;
        case REG_TRNG_CONTROL: {
            uint32_t old_enabled = (s->config & TRNG_CONTROL_ENABLED) != 0;
            s->config            = (uint32_t)data;
            DPRINTF("TRNG_REGS: REG_TRNG_CONTROL write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64 "\n", addr, data);
            // enabled = (data & TRNG_CONTROL_ENABLED) != 0;

            // if (!old_enabled && enabled) {
            //     apple_a7iop_interrupt_status_push(sep->mailbox,
            //                                       0x10003); // TRNG
            // }
            break;
        }
        case REG_TRNG_AES_KEY_BASE ... REG_TRNG_AES_KEY_END:
            if ((s->offset_0x70 & (TRNG_UNKN5_ENCRYPT_FIFO | TRNG_UNKN5_INIT_DRBG)) != 0) { data = bswap32(data); }
            memcpy(s->key + (addr - REG_TRNG_AES_KEY_BASE), &data, size);
            break;
        case REG_TRNG_ECID_LOW:
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) { data = bswap32(data); }
            s->ecid = deposit64(s->ecid, 0, 32, data);
            break;
        case REG_TRNG_ECID_HI:
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) { data = bswap32(data); }
            s->ecid = deposit64(s->ecid, 32, 32, data);
            break;
        case REG_TRNG_COUNTER_LOW:
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) { data = bswap32(data); }
            s->counter = deposit64(s->counter, 0, 32, data);
            break;
        case REG_TRNG_COUNTER_HI:
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) { data = bswap32(data); }
            s->counter = deposit64(s->counter, 32, 32, data);
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) {
                uint8_t seed_material[DRBG_CTR_AES256_SEED_SIZE] = {0};
                memcpy(seed_material + 0x0, s->key, sizeof(s->key));
                memcpy(seed_material + 0x20, &s->ecid, sizeof(s->ecid));
                memcpy(seed_material + 0x28, &s->counter, sizeof(s->counter));
                if (s->ctr_drbg_init) {
                    s->ctr_drbg_init = false;
                    drbg_ctr_aes256_init(&s->ctr_drbg_rng, seed_material);
                    memset(s->fifo, 0, sizeof(s->fifo));
                }
                else {
#if NETTLE_VERSION_MAJOR >= 4
                    drbg_ctr_aes256_update(&s->ctr_drbg_rng, seed_material);
#else
                    drbg_ctr_aes256_update(&s->ctr_drbg_rng.key, &s->ctr_drbg_rng.V, seed_material);
#endif
                    drbg_ctr_aes256_random(&s->ctr_drbg_rng, sizeof(s->fifo), s->fifo);
                }
            }
            break;
        case REG_TRNG_UNKN5:
            s->offset_0x70 = data;
            if ((s->offset_0x70 & TRNG_UNKN5_INIT_DRBG) != 0) { s->ctr_drbg_init = true; }
            else if ((s->offset_0x70 & TRNG_UNKN5_ENCRYPT_FIFO) == 0) {
                memset(s->key, 0, sizeof(s->key));
            }
            // don't do the encryption here
            break;
        default:
            DPRINTF("TRNG_REGS: Unknown write at 0x" HWADDR_FMT_plx " of value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t trng_regs_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPTRNGState* s   = opaque;
    AppleSEP*          sep = s->sep;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    // uint32_t enabled = (s->config & TRNG_CONTROL_ENABLED) != 0;
    switch (addr) {
        case REG_TRNG_INOUT_START ... REG_TRNG_INOUT_END:
            ret = ldl_le_p(s->fifo + (addr - REG_TRNG_INOUT_START));
            if ((s->offset_0x70 & (TRNG_UNKN5_ENCRYPT_FIFO | TRNG_UNKN5_INIT_DRBG)) != 0) { ret = bswap32(ret); }
            break;
        case REG_TRNG_STATUS: ret = TRNG_STATUS_READY | TRNG_STATUS_TEST_READY; break;
        case REG_TRNG_CONTROL:
            s->config &= ~TRNG_CONTROL_RESEED;
            ret        = s->config;
            // if (enabled) {
            //     apple_a7iop_interrupt_status_push(sep->mailbox,
            //                                       0x10003); // TRNG
            // }
            break;
        case REG_TRNG_AES_KEY_BASE ... REG_TRNG_AES_KEY_END:
            ret = ldl_le_p(s->key + (addr - REG_TRNG_AES_KEY_BASE));
            break;
        case REG_TRNG_ECID_LOW   : ret = extract64(s->ecid, 0, 32); break;
        case REG_TRNG_ECID_HI    : ret = extract64(s->ecid, 32, 32); break;
        case REG_TRNG_COUNTER_LOW: ret = extract64(s->counter, 0, 32); break;
        case REG_TRNG_COUNTER_HI : ret = extract64(s->counter, 32, 32); break;
        case REG_TRNG_UNKN5      : ret = s->offset_0x70; break;
        case REG_TRNG_UNKN6      :    // (value & 0x180000) == 0 == panic
            ret = 0x180000;
            break;
        case REG_TRNG_UNKN7:
            // either 0x2 or 0x4, depending on certain factors
            // mask 0xf << 20
            // mask 0xf << 24
            ret |= 0x2 << 24;
            // ret |= 0x4 << 24;
            break;
        default: DPRINTF("TRNG_REGS: Unknown read at 0x" HWADDR_FMT_plx "\n", addr); break;
    }
    DPRINTF("TRNG_REGS: Read at 0x" HWADDR_FMT_plx " ret: 0x%" PRIX64 "\n", addr, ret);
    return ret;
}

static const MemoryRegionOps trng_regs_reg_ops = {
    .write                 = trng_regs_reg_write,
    .read                  = trng_regs_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_trng_reset_enter(Object* obj, ResetType type)
{
    AppleSEPTRNGState* s = APPLE_SEP_TRNG(obj);

    memset(s->key, 0, sizeof(s->key));
    memset(s->fifo, 0, sizeof(s->fifo));
    s->offset_0x70   = 0;
    s->ecid          = 0;
    s->counter       = 0;
    s->config        = 0;
    s->ctr_drbg_init = false;
    memset(&s->ctr_drbg_rng, 0, sizeof(s->ctr_drbg_rng));
}

static void apple_sep_trng_realize(DeviceState* dev, Error** errp)
{
    AppleSEPTRNGState* s   = APPLE_SEP_TRNG(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->regs_mr, OBJECT(s), &trng_regs_reg_ops, s, "sep.trng_regs", TRNG_REGS_REG_SIZE);
    sysbus_init_mmio(sbd, &s->regs_mr);
}

static void apple_sep_trng_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_trng_reset_enter;

    dc->realize = apple_sep_trng_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPTRNGState, apple_sep_trng, APPLE_SEP_TRNG, SYS_BUS_DEVICE)

AppleSEPTRNGState* apple_sep_trng_create(AppleSEP* sep)
{
    AppleSEPTRNGState* s = APPLE_SEP_TRNG(qdev_new(TYPE_APPLE_SEP_TRNG));

    s->sep = sep;

    return s;
}
