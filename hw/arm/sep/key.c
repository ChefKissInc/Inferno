/*
 * Apple SEP Key (??).
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

#include "hw/arm/sep/private.h"
#include "hw/resettable.h"
#include "qemu/lockable.h"

#define KEY_BASE_REG_SIZE       (0x10000)    // T8015/T8030
#define KEY_FKEY_REG_SIZE_S8000 (0x1000)     // S8000
#define KEY_FKEY_REG_SIZE_T8015 (0x4000)     // T8015
#define KEY_FCFG_REG_SIZE_S8000 (0x4000)     // S8000
#define KEY_FCFG_REG_SIZE_T8015 (0x10000)    // T8015
#define KEY_FCFG_REG_SIZE_T8020 (0x18000)    // T8020
// #define KEY_FCFG_REG_SIZE_T8030 (0x14000) // T8030 ; sepfw module
#define KEY_FCFG_REG_SIZE_T8030 (0x40000)    // T8030 e.g. 26.2beta2 ; sepfw kernel

// for KEY_BASE register 0x0: key status
#define SEP_KEY_BASE_KEY_STATUS_OFFSET          0x0
#define SEP_KEY_BASE_KEY_STATUS_HDCP_FUSE_VALID BIT(0)
#define SEP_KEY_BASE_KEY_STATUS_LC128_VALID     BIT(1)
#define SEP_KEY_BASE_KEY_STATUS_DPK_TX_VALID    BIT(2)

#define SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_SHIFT 16
#define SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_MASK  0x7f
#define SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_GET(n)                                                         \
    (((n) >> SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_SHIFT) & SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_MASK)
#define SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_SET(n)                                                         \
    (((n) & SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_MASK) << SEP_KEY_BASE_KEY_STATUS_KM_VALID_INTERFACE_SHIFT)

#define SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_SHIFT 24
#define SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_MASK  0x7f
#define SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_GET(n)                                                         \
    (((n) >> SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_SHIFT) & SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_MASK)
#define SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_SET(n)                                                         \
    (((n) & SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_MASK) << SEP_KEY_BASE_KEY_STATUS_KS_VALID_INTERFACE_SHIFT)

// for KEY_BASE register 0x4: load key
// interface value is a bitmask
#define SEP_KEY_BASE_LOAD_KEY_OFFSET          0x4
#define SEP_KEY_BASE_LOAD_KEY_INTERFACE_SHIFT 8
#define SEP_KEY_BASE_LOAD_KEY_INTERFACE_MASK  0xff
#define SEP_KEY_BASE_LOAD_KEY_INTERFACE_GET(n)                                              \
    (((n) >> SEP_KEY_BASE_LOAD_KEY_INTERFACE_SHIFT) & SEP_KEY_BASE_LOAD_KEY_INTERFACE_MASK)
#define SEP_KEY_BASE_LOAD_KEY_INTERFACE_SET(n)                                              \
    (((n) & SEP_KEY_BASE_LOAD_KEY_INTERFACE_MASK) << SEP_KEY_BASE_LOAD_KEY_INTERFACE_SHIFT)

// unkn0 0x0==Lc128/DpkTx, 0x1/0x3==Km/Ks, 0x2==Km
#define SEP_KEY_BASE_LOAD_KEY_UNKN0_SHIFT 4
#define SEP_KEY_BASE_LOAD_KEY_UNKN0_MASK  0x3
#define SEP_KEY_BASE_LOAD_KEY_UNKN0_GET(n)                                          \
    (((n) >> SEP_KEY_BASE_LOAD_KEY_UNKN0_SHIFT) & SEP_KEY_BASE_LOAD_KEY_UNKN0_MASK)
#define SEP_KEY_BASE_LOAD_KEY_UNKN0_SET(n)                                          \
    (((n) & SEP_KEY_BASE_LOAD_KEY_UNKN0_MASK) << SEP_KEY_BASE_LOAD_KEY_UNKN0_SHIFT)

#define SEP_KEY_BASE_LOAD_KEY_ACTIVE BIT(0)

// for KEY_BASE registers 0xc (PKA) && 0x10 (AESH) && 0x14 (AES2): send key
// die offset/bitshift is unknown
// interface value is a bitmask
// interface is for Km for PKA && AESH, but Ks for AES2
#define SEP_KEY_BASE_SEND_KEY_PKA_OFFSET      0xc
#define SEP_KEY_BASE_SEND_KEY_AESH_OFFSET     0x10
#define SEP_KEY_BASE_SEND_KEY_AES2_OFFSET     0x14
#define SEP_KEY_BASE_SEND_KEY_INTERFACE_SHIFT 8
#define SEP_KEY_BASE_SEND_KEY_INTERFACE_MASK  0x7
#define SEP_KEY_BASE_SEND_KEY_INTERFACE_GET(n)                                              \
    (((n) >> SEP_KEY_BASE_SEND_KEY_INTERFACE_SHIFT) & SEP_KEY_BASE_SEND_KEY_INTERFACE_MASK)
#define SEP_KEY_BASE_SEND_KEY_INTERFACE_SET(n)                                              \
    (((n) & SEP_KEY_BASE_SEND_KEY_INTERFACE_MASK) << SEP_KEY_BASE_SEND_KEY_INTERFACE_SHIFT)
#define SEP_KEY_BASE_SEND_KEY_ACTIVE BIT(0)

struct AppleSEPKeyState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    MemoryRegion base_mr;
    MemoryRegion fkey_mr;
    MemoryRegion fcfg_mr;
    uint8_t      fcfg_offset_0x14_index;
    uint16_t     fcfg_offset_0x14_values[5];
    QEMUTimer*   manual_timer;
    QemuMutex    manual_timer_lock;
    uint32_t     manual_timer_hertz;
    bool         manual_timer_enabled;
    uint8_t      base_regs[KEY_BASE_REG_SIZE];
    uint8_t      fkey_regs[KEY_FKEY_REG_SIZE_T8015];
    uint8_t      fcfg_regs[KEY_FCFG_REG_SIZE_T8020];
};

// handler key selectors: Lc128==0x0; DpkTx==0x1; Km==0x2; Ks==0x3

static void key_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPKeyState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_KEY_BASE_LOAD_KEY_OFFSET:    // load_key
            DPRINTF("SEP KEY_BASE: Offset 0x" HWADDR_FMT_plx ": Input: load_key 0x%" PRIX64 "\n", addr, data);
            data &= ~SEP_KEY_BASE_LOAD_KEY_ACTIVE;
            goto jump_default;
        case 0x8:    // command or storage index: 0x20-0x26, 0x30-0x31, 0x04 (without
                     // input)
            /*
            cmds:
            0x0/0x1: wrapping key primary/secondary cmd7_0x4
            0x2/0x3: auth key primary/secondary cmd7_0x5
            0x6/0x7: cmd7_0x8
            0x8/0x9: cmd7_0x9
            0xA/0xB: sub key primary/secondary cmd7_0x6
            0xC: cmd7_0xB
            0xD: cmd7_0xC
            0xE/0xF: cmd7_0xA
            0x10..0x16: something about Ks and interfaces cmd7_0x3
            0x18..0x1E: send data2==data_size_qwords of data cmd7_0x2(cmd7_0x7)
            0x3F: first 0x40 bytes of random data cmd7_0x7
            0x40: second 0x40 bytes of random data cmd7_0x7
            */
            DPRINTF("SEP KEY_BASE: Offset 0x" HWADDR_FMT_plx ": Execute Command/Storage Index: cmd 0x%" PRIX64 "\n",
                    addr, data);
            // apple_a7iop_interrupt_status_push(s->mailbox,
            //                                   0x10000); // KEY
            goto jump_default;
        case 0x308 ... 0x344:    // 0x40 bytes of output from TRNG
            DPRINTF("SEP KEY_BASE: Offset 0x" HWADDR_FMT_plx ": Input: cmd 0x%" PRIX64 "\n", addr, data);
            goto jump_default;
        default:
        jump_default:
            memcpy(&s->base_regs[addr], &data, size);
            DPRINTF("SEP KEY_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t key_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPKeyState* s   = opaque;
    uint64_t          ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case SEP_KEY_BASE_LOAD_KEY_OFFSET:
            DPRINTF("SEP KEY_BASE: LOAD_KEY read-back. read at 0x" HWADDR_FMT_plx "\n", addr);
            goto jump_default;
        case 0x40 ... 0x248:
            // actual size 0x20a
            DPRINTF("SEP KEY_BASE: data0 read. read at 0x" HWADDR_FMT_plx "\n", addr);
            goto jump_default;
        default:
        jump_default:
            memcpy(&ret, &s->base_regs[addr], size);
            DPRINTF("SEP KEY_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps key_base_reg_ops = {
    .write                 = key_base_reg_write,
    .read                  = key_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void key_fkey_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPKeyState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        default:
        jump_default:
            memcpy(&s->fkey_regs[addr], &data, size);
            DPRINTF("SEP KEY_FKEY: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t key_fkey_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPKeyState* s                                = opaque;
    uint64_t          ret                              = 0;
    uint8_t           key_fkey_offset_0x14_index       = 0;
    uint8_t           key_fkey_offset_0x14_index_limit = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        default:
            memcpy(&ret, &s->fkey_regs[addr], size);
            DPRINTF("SEP KEY_FKEY: Unknown read at 0x" HWADDR_FMT_plx " ret: 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps key_fkey_reg_ops = {
    .write                 = key_fkey_reg_write,
    .read                  = key_fkey_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void sep_manual_timer_mod(AppleSEPKeyState* s)
{
    if (s->manual_timer_enabled && s->manual_timer_hertz != 0) {
        // 0x10008 actually stays active until being properly disabled here
        // timer msr's next to the write are physical ones
        // sync 24 MHz with platform (e.g. t8030.c) if it changes there.
        timer_mod(s->manual_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                       + ((NANOSECONDS_PER_SECOND * s->manual_timer_hertz) / 24000000));
    }
}

static void sep_manual_timer(void* opaque)
{
    AppleSEPKeyState* s = opaque;
    WITH_QEMU_LOCK_GUARD(&s->manual_timer_lock)
    {
        // DPRINTF("%s: interrupt_status_push\n", __func__);
        QEMU_LOCK_GUARD(&s->sep->mailbox->lock);
        apple_a7iop_interrupt_status_push(s->sep->mailbox, INTERRUPT_SEP_MANUAL_TIMER);
        sep_manual_timer_mod(s);
    }
}

static void key_fcfg_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPKeyState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x0:
            // DPRINTF("SEP KEY_FCFG: TEST0 0x" HWADDR_FMT_plx " with value 0x%" PRIX64
            //         "\n",
            //         addr, data);
            // 0x101 (bit0) enables interrupt 0x8 timer
            WITH_QEMU_LOCK_GUARD(&s->manual_timer_lock)
            {
                // if ((data & BIT(0)) != 0) {
                //     s->manual_timer_enabled = true;
                // }
                s->manual_timer_enabled = ((data & BIT(0)) != 0);
                sep_manual_timer_mod(s);
            }
            goto jump_default;
        case 0x4:
            // DPRINTF("SEP KEY_FCFG: TEST1 0x" HWADDR_FMT_plx " with value 0x%" PRIX64
            //         "\n",
            //         addr, data);
            // 0x3 (bit0) disables interrupt 0x8 timer
            WITH_QEMU_LOCK_GUARD(&s->manual_timer_lock)
            {
                if ((data & BIT(0)) != 0) { s->manual_timer_enabled = false; }
            }
            goto jump_default;
        case 0xc:
            // DPRINTF("SEP KEY_FCFG: TEST2 0x" HWADDR_FMT_plx " with value 0x%" PRIX64
            //         "\n",
            //         addr, data);
            // 0x249F00/2400000 is 0.1 seconds in hertz
            WITH_QEMU_LOCK_GUARD(&s->manual_timer_lock) { s->manual_timer_hertz = data; }
            goto jump_default;
        case 0x10:
            if (data == 0x1) { ((uint32_t*)s->base_regs)[0x00 / 4] = BIT(31) | BIT(0); }
            goto jump_log_and_write;
        case 0x14:
            DPRINTF("SEP KEY_FCFG: vals 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            if (data == 0xFFFF) {
                s->fcfg_offset_0x14_index = 0x0;
                memset(s->fcfg_offset_0x14_values, 0, sizeof(s->fcfg_offset_0x14_values));
            }
            uint8_t index       = s->fcfg_offset_0x14_index;
            uint8_t index_limit = sizeof(s->fcfg_offset_0x14_values) / sizeof(s->fcfg_offset_0x14_values[0]);
            index               = (index < index_limit) ? index : 0;
            s->fcfg_offset_0x14_values[index] = data & 0xFFFF;
            s->fcfg_offset_0x14_index++;
            goto jump_log_and_write;
        default:
        jump_log_and_write:
            DPRINTF("SEP KEY_FCFG: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
        jump_default:
            memcpy(&s->fcfg_regs[addr], &data, size);
            break;
    }
}

static uint64_t key_fcfg_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPKeyState* s   = opaque;
    uint64_t          ret = 0;
    uint8_t           key_fcfg_offset_0x14_index;
    uint8_t           key_fcfg_offset_0x14_index_limit;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x14: {
            key_fcfg_offset_0x14_index = s->fcfg_offset_0x14_index;
            key_fcfg_offset_0x14_index_limit =
                sizeof(s->fcfg_offset_0x14_values) / sizeof(s->fcfg_offset_0x14_values[0]);
            key_fcfg_offset_0x14_index =
                (key_fcfg_offset_0x14_index < key_fcfg_offset_0x14_index_limit) ? key_fcfg_offset_0x14_index : 0;
            ret = ((uint32_t)key_fcfg_offset_0x14_index << 16) | s->fcfg_offset_0x14_values[key_fcfg_offset_0x14_index];
            DPRINTF("SEP KEY_FCFG: vals read at 0x" HWADDR_FMT_plx " ret: 0x%" PRIX64 "\n", addr, ret);
            break;
        }
        case 0x18:
            // for SKG (0x44c4) ; 0x4 | (value & 0x3)
            // another function (unknown: 0x44cd) returns: value & 0xff07
            // ret = 0x4 | 0x0; // when AMK is disabled
            ret = 0x4 | 0x1;    // when AMK is enabled
            DPRINTF("SEP KEY_FCFG: AMK read at 0x" HWADDR_FMT_plx " ret: 0x%" PRIX64 "\n", addr, ret);
            break;
        case 0x20:
            // HCDP: 0x44c6 ; 0x4 | (value & 0x3)
            ret = 0x4;
            break;
        case 0x24:
            // HDCP: 0x44c7 ; 0x4 | (value & 0x3)
            // another function (unknown: 0x44ce) returns: value & 0x7
            ret = 0x4;
            break;
        case 0x8100: ret = 0x0; break;
        // case 0x10000:
        //     ret = 0x0;
        //     // interface enabled: (1 << interface) & 0x7f
        //     break;
        default:
            memcpy(&ret, &s->fcfg_regs[addr], size);
            DPRINTF("SEP KEY_FCFG: Unknown read at 0x" HWADDR_FMT_plx " ret: 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps key_fcfg_reg_ops = {
    .write                 = key_fcfg_reg_write,
    .read                  = key_fcfg_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_key_reset_enter(Object* obj, ResetType type)
{
    AppleSEPKeyState* s = APPLE_SEP_KEY(obj);

    s->fcfg_offset_0x14_index = 0;
    memset(s->fcfg_offset_0x14_values, 0, sizeof(s->fcfg_offset_0x14_values));
    s->manual_timer_hertz   = 0;
    s->manual_timer_enabled = false;
    memset(s->base_regs, 0, sizeof(s->base_regs));
    memset(s->fkey_regs, 0, sizeof(s->fkey_regs));
    memset(s->fcfg_regs, 0, sizeof(s->fcfg_regs));
}

static void apple_sep_key_realize(DeviceState* dev, Error** errp)
{
    AppleSEPKeyState* s   = APPLE_SEP_KEY(dev);
    SysBusDevice*     sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(dev), &key_base_reg_ops, s, "base", KEY_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
    memory_region_init_io(&s->fkey_mr, OBJECT(dev), &key_fkey_reg_ops, s, "fkey", KEY_FKEY_REG_SIZE_T8015);
    sysbus_init_mmio(sbd, &s->fkey_mr);
    memory_region_init_io(&s->fcfg_mr, OBJECT(dev), &key_fcfg_reg_ops, s, "fcfg", KEY_FCFG_REG_SIZE_T8020);
    sysbus_init_mmio(sbd, &s->fcfg_mr);

    s->manual_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sep_manual_timer, s);
}

static void apple_sep_key_init(Object* obj)
{
    AppleSEPKeyState* s = APPLE_SEP_KEY(obj);

    qemu_mutex_init(&s->manual_timer_lock);
}

static void apple_sep_key_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_key_reset_enter;
    dc->realize      = apple_sep_key_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleSEPKeyState, apple_sep_key, APPLE_SEP_KEY, SYS_BUS_DEVICE)

AppleSEPKeyState* apple_sep_key_create(AppleSEP* sep)
{
    AppleSEPKeyState* s = APPLE_SEP_KEY(qdev_new(TYPE_APPLE_SEP_KEY));

    s->sep = sep;

    return s;
}
