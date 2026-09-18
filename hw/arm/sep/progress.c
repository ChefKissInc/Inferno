/*
 * Apple SEP Boot Progress.
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
#include "hw/arm/sep/core.h"
#include "hw/irq.h"
#include "system/address-spaces.h"

#define PROGRESS_REG_SIZE (0x4000)    // ?

struct AppleSEPProgressState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    MemoryRegion progress_mr;
    uint8_t      progress_regs[PROGRESS_REG_SIZE];
};

#ifdef SEP_DISABLE_ASLR
    #include "hw/arm/a13.h"
static void disable_aslr(AppleSEPProgressState* s)
{
    DPRINTF("SEP_PROGRESS: Disable ASLR\n");
    AddressSpace* nsas = &address_space_memory;

    hwaddr phys_addr = 0x0;
    // easy way of retrieving the sepb random_0 address
    // T8020: b *0x340000000 ; p/x $x0+0x80 == e.g. 0x340736380
    // easy way of retrieving the sepb random_0 address
    // T8030: go to the first SYS_ACC_PWR_DN_SAVE read in the kernel,
    // and then do p/x $x0+0x80 == e.g. 0x3407CA380
    // TODO: do this automatically in the reset function instead.
    if (s->sep->chip_id == 0x8015) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        phys_addr = 0x34015FD40ULL;    // T8015
    #else
        assert_not_reached();
    #endif
    }
    else if (s->sep->chip_id == 0x8020) {
    #if SEP_USE_VERSION_OVERRIDE == 14
        phys_addr = 0x340736380ULL;    // T8020 iOS 14
    #elif SEP_USE_VERSION_OVERRIDE == 15
        phys_addr = 0x34086E380ULL;    // T8020 iOS 15
    #elif SEP_USE_VERSION_OVERRIDE == 16
        assert_not_reached();
    #elif SEP_USE_VERSION_OVERRIDE == 18
        assert_not_reached();
    #endif
    }
    else if (s->sep->chip_id == 0x8030) {
        // 0x8030 is now handled in disable_aslr_SYS_ACC_PWR_DN_SAVE, which is
        // handled/called in apple_sep_iop_start
        return;
    }
    else {
        assert_not_reached();
    }
    if (phys_addr) {
        // The first 16bytes of SEPB.random_0 are being used for SEPOS'
        // ASLR. GDB's awatch refuses to tell me where it ends up, so
        // here you go, I'm just zeroing that shit.
        // == This disables ASLR for SEPOS apps
        // Future iOS versions might use more than 16 bytes, so zero
        // the whole field here.
        // phys_SEPB + 0x80; pc==0x240005BAC
        address_space_set(nsas, phys_addr, 0, 0x40, MEMTXATTRS_UNSPECIFIED);
    }
}
#endif

static void progress_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPProgressState* s       = opaque;
    SEPMessage             sep_msg = {0};

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x4:
            if ((data == 0xFC4A2CAC || data == 0xEEE6BA79) && (s->sep->chip_id >= 0x8020))    // Enable Trace Buffer
            {
#ifdef SEP_ENABLE_TRACE_BUFFER
                // Only works for >= T8020 here, because the T8015 SEPOS is
                // compressed.
                apple_sep_debug_trace_enable(s->sep->debug_trace);
#endif
            }
            break;
        case 0x8:
#ifdef SEP_DISABLE_ASLR
            if (data == 0x23BFDFE7) { disable_aslr(s); }
#endif
            if (data == 0x41A7 && (s->sep->chip_id >= 0x8015)) {
                DPRINTF("%s: SEPFW_copy_test0: 0x" HWADDR_FMT_plx " 0x%" PRIX64 "\n", __func__, s->sep_fw_addr,
                        s->sep_fw_size);
#ifdef SEP_ENABLE_HARDCODED_FIRMWARE
                AddressSpace* nsas = &address_space_memory;
                address_space_write(nsas, s->sep->sep_fw_addr, MEMTXATTRS_UNSPECIFIED, s->sep->fw_data,
                                    s->sep->sep_fw_size);
#endif
                // g_free(sep_fw);
            }
#ifdef SEP_ENABLE_TRACE_BUFFER
            // if (data == 0x6A5D128D && (s->chip_id == 0x8015))
            if (data == 0x6A5D128D) {
                if (apple_a7iop_inbox_peek(s->sep->mailbox, &sep_msg.raw)) {
                    uint64_t shmbuf_base = (uint64_t)sep_msg.data << 12;
                    DPRINTF("%s: SHMBUF_TEST0: trace_data8:0x%" PRIX64 ": "
                            "shmbuf=0x" HWADDR_FMT_plx ": ep=0x%02x, tag=0x%02x, opcode=0x%02x(%u), "
                            "param=0x%02x, data=0x%08x\n",
                            s->mailbox->role, data, shmbuf_base, sep_msg.ep, sep_msg.tag, sep_msg.op, sep_msg.op,
                            sep_msg.param, sep_msg.data);
                    int debug_trace_mmio_index = -1;
                    if (s->sep->chip_id == 0x8015) { debug_trace_mmio_index = 11; }
                    else if (s->sep->chip_id >= 0x8020) {
                        debug_trace_mmio_index = 14;
                    }
                    if (debug_trace_mmio_index != -1) {
                        s->sep->shmbuf_base = shmbuf_base;
                        // uint64_t tracebuf_mmio_addr = shmbuf_base + s->trace_buffer_base_offset;
                        // DPRINTF("%s: SHMBUF_TEST1: tracbuf=0x" HWADDR_FMT_plx "\n", s->mailbox->role,
                        //         tracebuf_mmio_addr);
                        // _if SEP_ENABLE_DEBUG_TRACE_MAPPING
                        // TODO: T8020 isn't handled here anymore, but T8015
                        // probably still should.
                        // _endif
                    }
                }
            }
#endif
            if (data == 0x23BFDFE7 && (s->sep->chip_id == 0x8015)) {
#define LVL3_BASE_COPYFROM 0x24090C000ULL
                AddressSpace* nsas          = &address_space_memory;
                uint64_t      pagetable_val = 0;
                for (uint64_t page_addr = 0x340000000ULL; page_addr < 0x342000000ULL; page_addr += 0x4000) {
                    pagetable_val = page_addr | 0x603;
                    address_space_write(nsas, LVL3_BASE_COPYFROM + (((page_addr >> 14) & 0x7FF) * 8),
                                        MEMTXATTRS_UNSPECIFIED, &pagetable_val, sizeof(pagetable_val));
                }
            }
            break;
        case 0x0:
            memcpy(&s->progress_regs[addr], &data, size);
            DPRINTF("SEP Progress: Progress_0 write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            if (data == 0xDEADBEE0) { qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_IRQ)); }
            if (data == 0xDEADBEE1) { qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_FIQ)); }
            if (data == 0xDEADBEE2) { qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_VIRQ)); }
            if (data == 0xDEADBEE3) { qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_VFIQ)); }

            if (data == 0xDEADBEE4) { qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_IRQ)); }
            if (data == 0xDEADBEE5) { qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_FIQ)); }
            if (data == 0xDEADBEE6) { qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_VIRQ)); }
            if (data == 0xDEADBEE7) { qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->sep->cpu), ARM_CPU_VFIQ)); }
            if (data == 0xCAFE1334) {
                uint32_t i = 0;
                for (i = 0x10000; i < 0x10200; i++) {
                    if (i == 0x10008 || i == 0x1002C) { continue; }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
            }
            if (data == 0xCAFE1335) {
                uint32_t i = 0;
                for (i = 0x40000; i < 0x40100; i++) {
                    if (i == 0x40000) { continue; }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
            }
            if (data == 0xCAFE1336) {
                uint32_t i = 0;
                for (i = 0x70000; i < 0x70400; i++) {
                    // if (i == 0x70001) {
                    //     continue;
                    // }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
            }
            if (data == 0xCAFE1337) {
                uint32_t i = 0;
                for (i = 0x10000; i < 0x10200; i++) {
                    if (i == 0x10008 || i == 0x1002C) { continue; }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
                for (i = 0x40000; i < 0x40100; i++) {
                    if (i == 0x40000) { continue; }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
                for (i = 0x70000; i < 0x70400; i++) {
                    // if (i == 0x70001) {
                    //     continue;
                    // }
                    apple_a7iop_interrupt_status_push(s->sep->mailbox, i);
                }
            }
            break;
        case 0x3370:
            memcpy(&s->progress_regs[addr], &data, size);
            DPRINTF("SEP Progress: Progress_1 write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            // apple_mbox_set_custom0(s->mbox, data);
            apple_a7iop_interrupt_status_push(s->sep->mailbox, data);
            break;
        // case 0x4:
        // case 0x8:
        case 0x114:
        case 0x214:
        case 0x218:
        case 0x21C:
        case 0x220:
        case 0x2D8:
        case 0x2DC:
        case 0x2E0:    // ecid low
        case 0x2E4:    // ecid high
        case 0x2E8:    // board-id
        case 0x2EC:    // chip-id
        case 0x314:
        case 0x318:
        case 0x31C: memcpy(&s->progress_regs[addr], &data, size); break;
        default:
            // jump_default:
            memcpy(&s->progress_regs[addr], &data, size);
            DPRINTF("SEP Progress: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            break;
    }
}

static uint64_t progress_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPProgressState* s   = opaque;
    uint64_t               ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        default:
            memcpy(&ret, &s->progress_regs[addr], size);
            DPRINTF("SEP Progress: Unknown read at 0x" HWADDR_FMT_plx "\n", addr);
            break;
    }

    return ret;
}

static const MemoryRegionOps progress_reg_ops = {
    .write                 = progress_reg_write,
    .read                  = progress_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_progress_reset_enter(Object* obj, ResetType type)
{
    AppleSEPProgressState* s = APPLE_SEP_PROGRESS(obj);

    memset(s->progress_regs, 0, sizeof(s->progress_regs));
}

static void apple_sep_progress_realize(DeviceState* dev, Error** errp)
{
    AppleSEPProgressState* s   = APPLE_SEP_PROGRESS(dev);
    SysBusDevice*          sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->progress_mr, OBJECT(dev), &progress_reg_ops, s, "sep.progress", PROGRESS_REG_SIZE);
    sysbus_init_mmio(sbd, &s->progress_mr);
}

static void apple_sep_progress_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_progress_reset_enter;
    dc->realize      = apple_sep_progress_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPProgressState, apple_sep_progress, APPLE_SEP_PROGRESS, SYS_BUS_DEVICE)

AppleSEPProgressState* apple_sep_progress_create(AppleSEP* sep)
{
    AppleSEPProgressState* s = APPLE_SEP_PROGRESS(qdev_new(TYPE_APPLE_SEP_PROGRESS));

    s->sep = sep;

    return s;
}
