/*
 * Apple SEP Public Key Accelerator.
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
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "hw/arm/sep/private.h"

// T8030, but there's no overlap when the size is bigger
// #define PKA_BASE_REG_SIZE (0x4000)
#define PKA_BASE_REG_SIZE (0x10000)    // T8015/T8030
#define PKA_TMM_REG_SIZE  (0x4000)

#define SEP_PKA_STATUS_INTERRUPT_0xA 0x1
#define SEP_PKA_STATUS_INTERRUPT_0xB 0x2
#define SEP_PKA_STATUS_INTERRUPT_0xC 0x4

struct AppleSEPPKAState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    QEMUBH*      command_bh;    // currently unused
    QemuMutex    lock;
    MemoryRegion base_mr;
    MemoryRegion tmm_mr;
    uint32_t     command;                    // 0x0
    uint32_t     status0;                    // 0x4
    uint32_t     status_in0;                 // 0x8
    uint32_t     img4out_dgst_locked;        // 0x40
    uint8_t      img4out_dgst[32];           // 0x60
    uint8_t      output0[32];                // 0x60 ; read_cmd_0x2
    uint8_t      input0[0x80];               // 0x80 ; write_cmd_0x0 ; SMRK_pub ; 1024 bits ;
                                             // measurement==0x34_bytes
    uint8_t      public_key[32];             // 0x100 // for AESS ; read_cmd_0x0 ; read
                                             // public_key ; status_in0 needs to be 0x1
    uint8_t      attest_hash[32];            // 0x180 ; read_cmd_0x3 ; read attest_hash ;
                                             // status_in0 needs to be 0x1
    uint8_t      input1[0x20A];              // 0x200 .. 0x40A (not inclusive) ; write_cmd_0x1 ;
                                             // 4176 bits, maybe rsa input?
    uint32_t     chip_revision_locked;       // 0x800
    uint32_t     chip_revision;              // 0x820 ; mod_PKA_read buffer_id 0xd asks for that
    uint32_t     ecid_chipid_misc_locked;    // 0x840
    uint32_t     ecid_chipid_misc[5];        // 0x860
    uint8_t      pka_base_regs[PKA_BASE_REG_SIZE];
    uint8_t      pka_tmm_regs[PKA_TMM_REG_SIZE];
};

static void pka_handle_cmd(AppleSEPPKAState* s)
{
    AppleSEP* sep = s->sep;

    // values: 0x4/0x8/0x10/0x20/0x40/0x80/0x100
    if (s->command == 0x40) {    // migrate data with PKA
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x1000A);    // ack first interrupt/0xA
        // apple_a7iop_interrupt_status_push(sep->mailbox,
        // 0x1000B); // ack second interrupt/0xB
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x1000C);    // ack third interrupt/0xC
    }
    else if (s->command == 0x80) {    // MPKA_ECPUB_ATTEST
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x1000A);    // ack first interrupt/0xA
        // apple_a7iop_interrupt_status_push(sep->mailbox,
        // 0x1000B); // ack second interrupt/0xB
        apple_a7iop_interrupt_status_push(sep->mailbox,
                                          0x1000C);    // ack third interrupt/0xC
    }
}

static void pka_handle_cmd_bh(void* opaque)
{
    AppleSEPPKAState* s = opaque;
    pka_handle_cmd(s);
}

static void pka_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPPKAState* s   = opaque;
    AppleSEP*         sep = s->sep;

    QEMU_LOCK_GUARD(&s->lock);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x0:    // maybe command
            s->command = data;
            // PKA commands get executed directly, without additional trigger
            pka_handle_cmd(s);
            // qemu_bh_schedule(s->command_bh);
            goto jump_log;
        case 0x4:    // maybe status_out0
#if 1
            s->status0 = data;
            // maybe use & instead of ==
            if (s->status0 == 0x1) {
                // ack interrupt 0xA
                s->status_in0 = 1;
            }
            else if (s->status0 == 0x2) {
                // ack interrupt 0xB
                // unknown
            }
            else if (s->status0 == 0x4) {
                // ack interrupt 0xC
                // unknown
            }
#endif
            goto jump_log;
        case 0x40:    // img4out DGST locked
            s->img4out_dgst_locked |= (data & 1);
            goto jump_log;
        case 0x60 ... 0x7C:    // img4out DGST data
            if (!s->img4out_dgst_locked) { memcpy(&s->img4out_dgst[addr & 0x1F], &data, 4); }
            goto jump_log;
        case 0x80 ... 0x9C:    // some data
            goto jump_log;
        case 0x800:    // chip revision locked
            s->chip_revision_locked |= (data & 1);
            goto jump_log;
        case 0x820:    // chip revision data
            if (!s->chip_revision_locked) { s->chip_revision = data; }
            goto jump_log;
        case 0x840:    // ecid chipid misc locked
            s->ecid_chipid_misc_locked |= (data & 1);
            goto jump_log;
        case 0x860 ... 0x870:    // ecid chipid misc data ; 0x860/0x864 ecid, 0x870
                                 // chipid
            if (!s->ecid_chipid_misc_locked) { memcpy(&s->ecid_chipid_misc[(addr & 0x1F) >> 2], &data, 4); }
            goto jump_log;
        default:
        jump_default:
            memcpy(&s->pka_base_regs[addr], &data, size);
        jump_log:
            DPRINTF("SEP PKA_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t pka_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPPKAState* s   = opaque;
    AppleSEP*         sep = s->sep;
    uint64_t          ret = 0;

    QEMU_LOCK_GUARD(&s->lock);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x8:    // maybe status_in0/interrupt_status
#if 1
                     // if (s->status0 == 0x1)
            if (s->status_in0 == 0x1) {
                ret = 0x1;    // this means mod_PKA_read output ready
            }
#endif
#if 1
            ret = s->status_in0;
            if (s->status_in0 == 1) { s->status_in0 = 0; }
#endif
            goto jump_log;
        case 0x40:    // img4out DGST locked
            ret = s->img4out_dgst_locked;
            goto jump_log;
        case 0x60 ... 0x7C:    // img4out DGST data
            memcpy(&ret, &s->img4out_dgst[addr & 0x1F], 4);
            goto jump_log;
        case 0x800:    // chip revision locked
            ret = s->chip_revision_locked;
            goto jump_log;
        case 0x820:    // chip revision data
            ret = s->chip_revision;
            goto jump_log;
        case 0x840:    // ecid chipid misc locked
            ret = s->ecid_chipid_misc_locked;
            goto jump_log;
        case 0x860 ... 0x870:    // ecid chipid misc data
            memcpy(&ret, &s->ecid_chipid_misc[(addr & 0x1F) >> 2], 4);
            // memcpy(&ret, &s->ecid_chipid_misc + (addr & 0x1F), 4);
            goto jump_log;
        default:
        jump_default:
            memcpy(&ret, &s->pka_base_regs[addr], size);
        jump_log:
            DPRINTF("SEP PKA_BASE: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps pka_base_reg_ops = {
    .write                 = pka_base_reg_write,
    .read                  = pka_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void pka_tmm_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPPKAState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x818 ... 0x834:    // some data
            // correct?
            goto jump_log;
        default:
        jump_default:
            memcpy(&s->pka_tmm_regs[addr], &data, size);
        jump_log:
            DPRINTF("SEP PKA_TMM: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t pka_tmm_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPPKAState* s   = opaque;
    uint64_t          ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x818 ... 0x834:
            // TODO
            goto jump_log;
        default:
        jump_default:
            memcpy(&ret, &s->pka_tmm_regs[addr], size);
        jump_log:
            DPRINTF("SEP PKA_TMM: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps pka_tmm_reg_ops = {
    .write                 = pka_tmm_reg_write,
    .read                  = pka_tmm_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_pka_realize(DeviceState* dev, Error** errp)
{
    SysBusDevice*     sbd = SYS_BUS_DEVICE(dev);
    AppleSEPPKAState* s   = APPLE_SEP_PKA(dev);

    memory_region_init_io(&s->base_mr, OBJECT(dev), &pka_base_reg_ops, s, "base", PKA_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
    memory_region_init_io(&s->tmm_mr, OBJECT(dev), &pka_tmm_reg_ops, s, "tmm", PKA_TMM_REG_SIZE);
    sysbus_init_mmio(sbd, &s->tmm_mr);
}

static void apple_sep_pka_reset_enter(Object* obj, ResetType type)
{
    AppleSEPPKAState* s = APPLE_SEP_PKA(obj);

    s->command                 = 0;
    s->status0                 = 0;
    s->status_in0              = 0;
    s->img4out_dgst_locked     = 0;
    s->chip_revision_locked    = 0;
    s->ecid_chipid_misc_locked = 0;
    s->chip_revision           = 0;
    memset(s->img4out_dgst, 0, sizeof(s->img4out_dgst));
    memset(s->output0, 0, sizeof(s->output0));
    memset(s->input0, 0, sizeof(s->input0));
    memset(s->public_key, 0, sizeof(s->public_key));
    memset(s->attest_hash, 0, sizeof(s->attest_hash));
    memset(s->input1, 0, sizeof(s->input1));
    memset(s->ecid_chipid_misc, 0, sizeof(s->ecid_chipid_misc));
    memset(s->pka_base_regs, 0, sizeof(s->pka_base_regs));
    memset(s->pka_tmm_regs, 0, sizeof(s->pka_tmm_regs));
}

static void apple_sep_pka_init(Object* obj)
{
    AppleSEPPKAState* s = APPLE_SEP_PKA(obj);

    qemu_mutex_init(&s->lock);
    s->command_bh = aio_bh_new(qemu_get_aio_context(), pka_handle_cmd_bh, s);
}

static void apple_sep_pka_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_pka_reset_enter;
    dc->realize      = apple_sep_pka_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleSEPPKAState, apple_sep_pka, APPLE_SEP_PKA, SYS_BUS_DEVICE)

AppleSEPPKAState* apple_sep_pka_create(AppleSEP* sep)
{
    AppleSEPPKAState* s = APPLE_SEP_PKA(qdev_new(TYPE_APPLE_SEP_PKA));

    s->sep = sep;

    return s;
}
