/*
 * Apple Hardware AES.
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
#include "crypto/cipher.h"
#include "hw/arm/dt.h"
#include "hw/irq.h"
#include "hw/misc/aes.h"
#include "hw/misc/aes_reg.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "system/dma.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AppleAESState, APPLE_AES)

typedef struct AESCommand
{
    uint32_t  command;
    uint32_t* data;
    uint32_t  data_len;
    QTAILQ_ENTRY(AESCommand) next;
} AESCommand;

typedef struct
{
    QCryptoCipher*    cipher;
    key_select_t      select;
    QCryptoCipherAlgo algo;
    uint8_t           key[32];
    uint32_t          len;
    key_func_t        func;
    block_mode_t      mode;
    bool              wrapped;
    bool              encrypt;
    bool              disabled;
    uint8_t           id;
} AESKey;

struct AppleAESState
{
    SysBusDevice  parent_obj;
    MemoryRegion  iomems[2];
    MemoryRegion* dma_mr;
    AddressSpace  dma_as;
    qemu_irq      irq;
    int           last_level;
    aes_reg_t     reg;
    QemuMutex     mutex;
    QemuThread    thread;
    QemuCond      thread_cond;
    QemuMutex     queue_mutex;
    QTAILQ_HEAD(, AESCommand) queue;
    uint32_t  command;
    uint32_t* data;
    uint32_t  data_len;
    uint32_t  data_read;
    AESKey    keys[2];
    uint8_t   iv[4][16];
    bool      stopped;
    uint32_t  board_id;
};

static uint32_t key_size(uint8_t len)
{
    switch (len) {
        case KEY_LEN_128: return 128;
        case KEY_LEN_192: return 192;
        case KEY_LEN_256: return 256;
        default         : return 0;
    }
}

static QCryptoCipherAlgo key_algo(uint8_t mode)
{
    switch (mode) {
        case KEY_LEN_128: return QCRYPTO_CIPHER_ALGO_AES_128;
        case KEY_LEN_192: return QCRYPTO_CIPHER_ALGO_AES_192;
        case KEY_LEN_256: return QCRYPTO_CIPHER_ALGO_AES_256;
        default         : return QCRYPTO_CIPHER_ALGO__MAX;
    }
}

static QCryptoCipherMode key_mode(block_mode_t mode)
{
    switch (mode) {
        case BLOCK_MODE_ECB: return QCRYPTO_CIPHER_MODE_ECB;
        case BLOCK_MODE_CBC: return QCRYPTO_CIPHER_MODE_CBC;
        case BLOCK_MODE_CTR: return QCRYPTO_CIPHER_MODE_CTR;
        default            : return QCRYPTO_CIPHER_MODE__MAX;
    }
}

static void* aes_thread(void* opaque);

static void aes_update_irq(AppleAESState* s)
{
    if (s->reg.int_enable.raw & qatomic_read(&s->reg.int_status.raw)) {
        if (!s->last_level) {
            s->last_level = 1;
            qemu_irq_raise(s->irq);
            trace_apple_aes_update_irq(1);
        }
    }
    else if (s->last_level) {
        s->last_level = 0;
        qemu_irq_lower(s->irq);
        trace_apple_aes_update_irq(0);
    }
}

static void aes_update_command_fifo_status(AppleAESState* s)
{
    /* TODO: implement read/write_pointer */
    s->reg.command_fifo_status.empty    = s->reg.command_fifo_status.level == 0;
    s->reg.command_fifo_status.full     = s->reg.command_fifo_status.level >= COMMAND_FIFO_SIZE;
    s->reg.command_fifo_status.overflow = s->reg.command_fifo_status.level > COMMAND_FIFO_SIZE;
    s->reg.command_fifo_status.low      = s->reg.command_fifo_status.level < s->reg.watermarks.command_fifo_low;
    s->reg.int_status.command_fifo_low  = s->reg.command_fifo_status.low;
    aes_update_irq(s);
}

static void aes_empty_fifo(AppleAESState* s)
{
    AESCommand* cmd;
    AESCommand* cmd_next;

    QEMU_LOCK_GUARD(&s->queue_mutex);

    QTAILQ_FOREACH_SAFE (cmd, &s->queue, next, cmd_next) {
        QTAILQ_REMOVE(&s->queue, cmd, next);
        g_free(cmd);
    }

    s->reg.command_fifo_status.level = 0;
    aes_update_command_fifo_status(s);
}

static void aes_start(AppleAESState* s)
{
    if (s->stopped) {
        s->stopped = false;
        qemu_thread_create(&s->thread, TYPE_APPLE_AES, aes_thread, s, QEMU_THREAD_JOINABLE);
    }
}

static void aes_stop(AppleAESState* s)
{
    if (!s->stopped) {
        s->stopped = true;
        qemu_cond_signal(&s->thread_cond);
        qemu_thread_join(&s->thread);
    }
}

static bool aes_process_command(AppleAESState* s, AESCommand* cmd)
{
    trace_apple_aes_process_command(COMMAND_OPCODE(cmd->command));
    bool locked = false;
#define lock_reg()     \
    do {               \
        bql_lock();    \
        locked = true; \
    }                  \
    while (0)
    switch (COMMAND_OPCODE(cmd->command)) {
        case OPCODE_KEY: {
            uint32_t ctx         = COMMAND_KEY_COMMAND_KEY_CONTEXT(cmd->command);
            s->keys[ctx].select  = COMMAND_KEY_COMMAND_KEY_SELECT(cmd->command);
            s->keys[ctx].algo    = key_algo(COMMAND_KEY_COMMAND_KEY_LENGTH(cmd->command));
            s->keys[ctx].len     = key_size(COMMAND_KEY_COMMAND_KEY_LENGTH(cmd->command)) / 8;
            s->keys[ctx].wrapped = (cmd->command & COMMAND_KEY_COMMAND_WRAPPED) != 0;
            s->keys[ctx].encrypt = (cmd->command & COMMAND_KEY_COMMAND_ENCRYPT) != 0;
            s->keys[ctx].func    = COMMAND_KEY_COMMAND_KEY_FUNC(cmd->command);
            s->keys[ctx].mode    = COMMAND_KEY_COMMAND_BLOCK_MODE(cmd->command);
            s->keys[ctx].id      = COMMAND_KEY_COMMAND_COMMAND_ID(cmd->command);
            memcpy(s->keys[ctx].key, &cmd->data[1], s->keys[ctx].len);
            if (ctx) { s->reg.key_id.context_1 = s->keys[ctx].id; }
            else {
                s->reg.key_id.context_0 = s->keys[ctx].id;
            }
            if (s->keys[ctx].cipher) {
                qcrypto_cipher_free(s->keys[ctx].cipher);
                s->keys[ctx].cipher = NULL;
            }
            lock_reg();
            if (s->keys[ctx].select != KEY_SELECT_SOFTWARE) {
                s->keys[ctx].disabled = true;

                if (ctx) { s->reg.int_status.key_1_disabled = true; }
                else {
                    s->reg.int_status.key_0_disabled = true;
                }
                qemu_log_mask(LOG_GUEST_ERROR, "%s: Attempting to select unsupported hardware key: 0x%x\n", __func__,
                              s->keys[ctx].select);
            }
            else {
                if (s->keys[ctx].wrapped) { qemu_log_mask(LOG_GUEST_ERROR, "%s: What is wrapped key?\n", __func__); }
                s->keys[ctx].disabled = false;
                if (ctx) { s->reg.int_status.key_1_disabled = false; }
                else {
                    s->reg.int_status.key_0_disabled = false;
                }
                s->keys[ctx].cipher = qcrypto_cipher_new(s->keys[ctx].algo, key_mode(s->keys[ctx].mode),
                                                         s->keys[ctx].key, s->keys[ctx].len, &error_abort);
            }
            break;
        }
        case OPCODE_IV: {
            uint32_t ctx = COMMAND_IV_COMMAND_IV_CONTEXT(cmd->command);
            memcpy(s->iv[ctx], &cmd->data[1], 16);
            break;
        }
        case OPCODE_DSB: {
            // the first 16 byte input to dsb must be xor'ed with the key of the software encryption for reasons
            // when using aes256 with dsb, ignore the second half of the key.
            // memcpy(, &cmd->data[1], 16);
            // memcpy(, &cmd->data[5], 16);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DSB: data0", &cmd->data[0], 4);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DSB: arr0", &cmd->data[1], 16);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DSB: arr1", &cmd->data[5], 16);
            break;
        }
        case OPCODE_DATA: {
            command_data_t*     c           = (command_data_t*)cmd->data;
            uint32_t            key_ctx     = COMMAND_DATA_COMMAND_KEY_CONTEXT(c->command);
            uint32_t            iv_ctx      = COMMAND_DATA_COMMAND_IV_CONTEXT(c->command);
            uint32_t            len         = COMMAND_DATA_COMMAND_LENGTH(c->command);
            dma_addr_t          source_addr = c->source_addr;
            dma_addr_t          dest_addr   = c->dest_addr;
            g_autofree uint8_t* buffer      = NULL;
            g_autofree Error*   errp        = NULL;

            source_addr |= ((dma_addr_t)COMMAND_DATA_UPPER_ADDR_SOURCE(c->upper_addr)) << 32;
            dest_addr   |= ((dma_addr_t)COMMAND_DATA_UPPER_ADDR_DEST(c->upper_addr)) << 32;
            if (len & 0xf) {
                lock_reg();
                s->reg.int_status.invalid_data_length = true;
                break;
            }
            if (s->keys[key_ctx].disabled || !s->keys[key_ctx].cipher) {
                lock_reg();
                if (key_ctx) { s->reg.int_status.key_1_disabled = true; }
                else {
                    s->reg.int_status.key_0_disabled = true;
                }
                break;
            }

            buffer = g_malloc0(len);

            WITH_RCU_READ_LOCK_GUARD()
            { dma_memory_read(&s->dma_as, source_addr, buffer, len, MEMTXATTRS_UNSPECIFIED); }
            qcrypto_cipher_setiv(s->keys[key_ctx].cipher, s->iv[iv_ctx], 16, &errp);

            // qemu_hexdump(stderr, "AP AES: OPCODE_DATA: key", s->keys[key_ctx].key, s->keys[key_ctx].len);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DATA: iv_in", s->iv[iv_ctx], 16);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DATA: data_in", buffer, len);
            int res;
            if (s->keys[key_ctx].encrypt) {
                res = qcrypto_cipher_encrypt(s->keys[key_ctx].cipher, buffer, buffer, len, &errp);
            }
            else {
                res = qcrypto_cipher_decrypt(s->keys[key_ctx].cipher, buffer, buffer, len, &errp);
            }
            if (res != 0) {
                fprintf(stderr, "AES %scryption failed, res = %s\n", s->keys[key_ctx].encrypt ? "en" : "de",
                        strerror(-res));
                // } else {
                //     fprintf(stderr, "AES %scryption success, res = %s\n",
                //             s->keys[key_ctx].encrypt ? "en" : "de", strerror(-res));
            }
            qcrypto_cipher_getiv(s->keys[key_ctx].cipher, s->iv[iv_ctx], 16, &errp);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DATA: iv_out", s->iv[iv_ctx], 16);
            // qemu_hexdump(stderr, "AP AES: OPCODE_DATA: data_out", buffer, len);
            dma_memory_write(&s->dma_as, dest_addr, buffer, len, MEMTXATTRS_UNSPECIFIED);
            break;
        }
        case OPCODE_STORE_IV: {
            command_store_iv_t* c          = (command_store_iv_t*)cmd->data;
            dma_addr_t          dest_addr  = 0;
            uint32_t            ctx        = COMMAND_STORE_IV_COMMAND_CONTEXT(cmd->command);
            dest_addr                      = c->dest_addr;
            dest_addr                     |= ((dma_addr_t)COMMAND_STORE_IV_COMMAND_UPPER_ADDR_DEST(c->command)) << 32;
            dma_memory_write(&s->dma_as, dest_addr, s->iv[ctx], 16, MEMTXATTRS_UNSPECIFIED);
            break;
        }
        case OPCODE_FLAG:
            lock_reg();
            qatomic_set(&s->reg.flag_command.code, COMMAND_FLAG_ID_CODE(cmd->command));
            if (cmd->command & COMMAND_FLAG_STOP_COMMANDS) { s->stopped = true; }
            if (cmd->command & COMMAND_FLAG_SEND_INTERRUPT) { s->reg.int_status.flag_command = true; }
            break;
        default:
            lock_reg();
            s->reg.int_status.invalid_command = true;
            break;
    }

    return locked;
#undef lock_reg
}

static void* aes_thread(void* opaque)
{
    AppleAESState* s = opaque;
    rcu_register_thread();
    while (!s->stopped) {
        AESCommand* cmd = NULL;
        WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
        {
            cmd = QTAILQ_FIRST(&s->queue);
            if (cmd) { QTAILQ_REMOVE(&s->queue, cmd, next); }
        }
        if (cmd) {
            if (!aes_process_command(s, cmd)) { bql_lock(); }
            s->reg.command_fifo_status.level -= cmd->data_len;
            aes_update_command_fifo_status(s);
            bql_unlock();

            if (cmd->data) { g_free(cmd->data); }
            g_free(cmd);
        }
        WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
        {
            while (QTAILQ_EMPTY(&s->queue) && !s->stopped) { qemu_cond_wait(&s->thread_cond, &s->queue_mutex); }
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static void aes_security_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size) { }

static uint64_t aes_security_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAESState* s = opaque;

#if 0
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Read 0x" HWADDR_FMT_plx "\n", __func__,
                  addr);
#endif

    switch (addr) {
        case REG_AES_V3_SECURITY_AES_DISABLE: return AES_V3_SECURITY_AES_DISABLE_GID1 | AES_V3_SECURITY_AES_DISABLE_UID;
        case REG_AES_V3_SECURITY_GPIO_STRAPS:
            return AES_V3_SECURITY_GPIO_STRAPS_BOARD_ID(s->board_id) | AES_V3_SECURITY_GPIO_STRAPS_VALID;
        case REG_AES_V3_SECURITY_SET_ONLY: return 0x00;
        case REG_AES_V3_SECURITY_SEP     : return AES_V3_SECURITY_SEP_FIRST_BOOT | AES_V3_SECURITY_SEP_FIRST_AWAKE_BOOT;
        case REG_AES_V3_SECURITY_MCC_BOOTROM_DIS:
            // normally 0x0 for SecureROM, but it only checks after writing to it
            return AES_V3_SECURITY_MCC_BOOTROM_DIS;    // for iBoot (and SecureROM)
        default: return 0xFF;
    }
}

static void aes_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleAESState* s     = opaque;
    uint32_t       orig  = data;
    uint32_t       index = addr >> 2;
    uint32_t*      mmio;
    uint32_t       old;
    uint32_t       val     = data;
    int            iflg    = 0;
    bool           nowrite = false;

    if (addr >= AES_BLK_REG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x" HWADDR_FMT_plx "\n", __func__, addr);
        return;
    }

    mmio = &s->reg.raw[index];
    old  = *mmio;

    switch (addr) {
        case REG_AES_VERSION            :
        case REG_AES_STATUS             :
        case REG_AES_KEY_ID             :
        case REG_AES_AXI_STATUS         :
        case REG_AES_COMMAND_FIFO_STATUS:
        case REG_AES_COMMAND_FIFO_COUNT :
        case REG_AES_FLAG_COMMAND       :
        case REG_AES_SKG_KEY:
            nowrite = true;
            val     = old;
            break;
        case REG_AES_INT_STATUS:
            nowrite = true;
            val     = qatomic_and_fetch(&s->reg.int_status.raw, ~val);
            QEMU_FALLTHROUGH;
        case REG_AES_INT_ENABLE: iflg = 1; break;
        case REG_AES_WATERMARKS: aes_update_command_fifo_status(s); break;
        case REG_AES_CONTROL:
            switch (val) {
                case AES_BLK_CONTROL_START    : aes_start(s); break;
                case AES_BLK_CONTROL_STOP     : aes_stop(s); break;
                case AES_BLK_CONTROL_RESET    : aes_empty_fifo(s); break;
                case AES_BLK_CONTROL_RESET_AES: device_cold_reset(DEVICE(s)); break;
                default: qemu_log_mask(LOG_GUEST_ERROR, "REG_AES_CONTROL: Invalid write: 0x%x\n", val); break;
            }
            nowrite = true;
            val     = old;
            break;
        case REG_AES_COMMAND_FIFO_S8000:
        case REG_AES_COMMAND_FIFO:
            if (s->data_len > s->data_read) {
                s->data[s->data_read] = val;
                s->data_read++;
            }
            else {
                s->command = val;
                switch (COMMAND_OPCODE(val)) {
                    case OPCODE_KEY:
                        if (COMMAND_KEY_COMMAND_KEY_SELECT(val) == KEY_SELECT_SOFTWARE) {
                            uint32_t key_len = key_size(COMMAND_KEY_COMMAND_KEY_LENGTH(val)) / 8;

                            s->data_len  = key_len / 4 + 1;
                            s->data      = g_new0(uint32_t, s->data_len);
                            s->data[0]   = val;
                            s->data_read = 1;
                        }
                        else {
                            s->data_len  = 1;
                            s->data      = g_new0(uint32_t, s->data_len);
                            s->data[0]   = val;
                            s->data_read = 1;
                        }
                        break;
                    case OPCODE_IV:
                        s->data_len  = sizeof(command_iv_t) / 4;
                        s->data      = g_new0(uint32_t, s->data_len);
                        s->data[0]   = val;
                        s->data_read = 1;
                        break;
                    case OPCODE_DSB:
                        s->data_len  = sizeof(command_dsb_t) / 4;
                        s->data      = g_new0(uint32_t, s->data_len);
                        s->data[0]   = val;
                        s->data_read = 1;
                        break;
                    case OPCODE_DATA:
                        s->data_len  = sizeof(command_data_t) / 4;
                        s->data      = g_new0(uint32_t, s->data_len);
                        s->data[0]   = val;
                        s->data_read = 1;
                        break;
                    case OPCODE_STORE_IV:
                        s->data_len  = sizeof(command_store_iv_t) / 4;
                        s->data      = g_new0(uint32_t, s->data_len);
                        s->data[0]   = val;
                        s->data_read = 1;
                        break;
                    case OPCODE_FLAG:
                        s->data_len  = 1;
                        s->data      = g_new0(uint32_t, s->data_len);
                        s->data[0]   = val;
                        s->data_read = 1;
                        break;
                    default:
                        s->reg.int_status.invalid_command = true;
                        iflg                              = 1;
                        qemu_log_mask(LOG_GUEST_ERROR, "REG_AES_COMMAND_FIFO: Unknown opcode: 0x%x\n",
                                      COMMAND_OPCODE(val));
                        break;
                }
            }

            if (s->data && s->data_len <= s->data_read) {
                AESCommand* cmd = g_new0(AESCommand, 1);
                cmd->command    = s->command;
                cmd->data       = s->data;
                cmd->data_len   = s->data_len;

                s->command  = 0;
                s->data     = NULL;
                s->data_len = s->data_read = 0;

                WITH_QEMU_LOCK_GUARD(&s->queue_mutex) { QTAILQ_INSERT_TAIL(&s->queue, cmd, next); }
                qemu_cond_signal(&s->thread_cond);
            }

            nowrite = true;
            val     = 0;
            s->reg.command_fifo_status.level++;
            aes_update_command_fifo_status(s);
            break;
        case REG_AES_CONFIG: break;
        case REG_AES_CLEAR_FIFO:
            if (val == REG_AES_CLEAR_FIFO_RESET) { aes_empty_fifo(s); }
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write to unknown reg: 0x" HWADDR_FMT_plx "\n", __func__, addr);
            break;
    }

    if (!nowrite) { *mmio = val; }

    if (iflg) { aes_update_irq(s); }

    trace_apple_aes_reg_write(addr, orig, old, val);
}

static uint64_t aes_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAESState* s    = opaque;
    uint32_t       val  = 0;
    uint32_t*      mmio = NULL;

    if (addr >= AES_BLK_REG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x" HWADDR_FMT_plx "\n", __func__, addr);
        return 0;
    }

    mmio = &s->reg.raw[addr >> 2];

    switch (addr) {
        case REG_AES_INT_STATUS         :
        case REG_AES_COMMAND_FIFO_STATUS:
        case REG_AES_FLAG_COMMAND       : val = qatomic_read(mmio); break;
        default                         : val = s->reg.raw[addr >> 2]; break;
    }

    trace_apple_aes_reg_read(addr, val);
    return val;
}

static const MemoryRegionOps aes_reg_ops = {
    .write                 = aes_reg_write,
    .read                  = aes_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static const MemoryRegionOps aes_security_reg_ops = {
    .write                 = aes_security_reg_write,
    .read                  = aes_security_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_aes_reset_enter(Object* obj, ResetType type)
{
    AppleAESState* s = APPLE_AES(obj);

    s->reg = (aes_reg_t){0};

    s->reg.status.v5.text0_dpa_random_seeded               = true;
    s->reg.status.v5.text1_dpa_random_seeded               = true;
    s->reg.status.v5.text2_dpa_random_seeded               = true;
    s->reg.status.v5.text3_dpa_random_seeded               = true;
    s->reg.status.v5.text4_dpa_random_seeded               = true;
    s->reg.status.v5.text5_dpa_random_seeded               = true;
    s->reg.status.v5.key_unwrap_dpa_random_seeded          = true;
    s->reg.status.v5.gid_self_test_passed                  = true;
    s->reg.status.v5.fairplay_descrambler_self_test_passed = true;

    s->command = 0;
    if (s->data) {
        g_free(s->data);
        s->data = NULL;
    }
    s->data_read = 0;
    s->data_len  = 0;
    s->stopped   = true;
}

static void apple_aes_reset_hold(Object* obj, ResetType type)
{
    AppleAESState* s = APPLE_AES(obj);

    aes_stop(s);
    aes_empty_fifo(s);
}

static void apple_aes_realize(DeviceState* dev, Error** errp)
{
    AppleAESState* s = APPLE_AES(dev);
    Object*        obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_APPLE_AES);

    qemu_cond_init(&s->thread_cond);
    qemu_mutex_init(&s->queue_mutex);
}

static void apple_aes_unrealize(DeviceState* dev)
{
    AppleAESState* s = APPLE_AES(dev);

    qemu_cond_destroy(&s->thread_cond);
    qemu_mutex_destroy(&s->queue_mutex);
}

SysBusDevice* apple_aes_create(AppleDTNode* node, uint32_t board_id)
{
    DeviceState*   dev;
    AppleAESState* s;
    SysBusDevice*  sbd;
    AppleDTProp*   prop;
    uint64_t*      reg;

    dev = qdev_new(TYPE_APPLE_AES);
    s   = APPLE_AES(dev);
    sbd = SYS_BUS_DEVICE(dev);

    s->board_id = board_id;

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    memory_region_init_io(&s->iomems[0], OBJECT(dev), &aes_reg_ops, s, TYPE_APPLE_AES ".mmio", reg[1]);

    sysbus_init_mmio(sbd, &s->iomems[0]);

    // also called PKA_SECU, but with size 0x8000 instead of 0x4000
    memory_region_init_io(&s->iomems[1], OBJECT(dev), &aes_security_reg_ops, s, TYPE_APPLE_AES ".security.mmio",
                          reg[3]);
    sysbus_init_mmio(sbd, &s->iomems[1]);

    s->last_level = 0;
    sysbus_init_irq(sbd, &s->irq);

    QTAILQ_INIT(&s->queue);

    return sbd;
}

static void apple_aes_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_aes_reset_enter;
    rc->phases.hold  = apple_aes_reset_hold;

    dc->realize   = apple_aes_realize;
    dc->unrealize = apple_aes_unrealize;
    dc->desc      = "Apple AES Accelerator";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleAESState, apple_aes, APPLE_AES, SYS_BUS_DEVICE)
