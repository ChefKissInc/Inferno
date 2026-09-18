/*
 * Apple SEP PMGR.
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

#define PMGR_BASE_REG_SIZE (0x10000)    // T8015/T8030

#define SEP_PMGR_REGISTER_POWER_CONTROL                        0x4000
#define SEP_PMGR_REGISTER_POWER_CONTROL_POWER_GATE_DELAY_SHIFT 16
#define SEP_PMGR_REGISTER_ACG_CONTROL                          0x4004    // some tunables shit

struct AppleSEPPMGRState
{
    SysBusDevice parent_obj;

    AppleSEP*    sep;
    MemoryRegion base_mr;
    uint8_t      base_regs[PMGR_BASE_REG_SIZE];
    bool         fuse_changer_bit0_was_set;
    bool         fuse_changer_bit1_was_set;
};

static const char* sepos_powerstate_name(uint64_t powerstate_offset)
{
    switch (powerstate_offset) {
        case 0x10: return "AES_HDCP";
        case 0x20: return "PKA0";    // sometimes_arg8/scheduling_priority is 0xC8/200
        case 0x28: return "TRNG";
        case 0x30: return "PKA1";
        case 0x48: return "I2C";
        case 0x58: return "KEY";
        case 0x60: return "EISP";
        case 0x68: return "SEPD";
        default  : break;
    }
    return "Unknown";
}

static void pmgr_base_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSEPPMGRState* s = opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
        case 0x10:    // mod_AES_HDCP
        case 0x20:    // mod_PKA ; PKA0
        case 0x28:    // mod_TRNG
        case 0x30:    // PKA1
        case 0x48:    // mod_I2C
        case 0x58:    // mod_KEY
        case 0x60:    // mod_EISP
        case 0x68:    // mod_SEPD
            DPRINTF("SEP PMGR_BASE: PowerState %s write before at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n",
                    sepos_powerstate_name(addr), addr, data);
            /*
                LIKE AP PMGR
                data | 0x80000000 == RESET
                data | 0x.F == ENABLE
                data | 0x.4 == POWER_SAVE
                data | 0xF. == ENABLED
                data | 0x4. == POWER_SAVE_ACTIVATED?
            */
            data = ((data & 0xF) << 4) | (data & 0xF);
            // Don't push any interrupt_status here, it was a nice workaround for
            // stuff, but now it's causing issues.

            DPRINTF("SEP PMGR_BASE: PowerState %s write after at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n",
                    sepos_powerstate_name(addr), addr, data);
            // workaround for 18.5 exception issues
            s->sep->mailbox->sepd_enabled = (data == 0xff);
            goto jump_default;
        case 0x8000:
            // the resulting values should only reset on SoC reset
            if ((data & 1) != 0) { s->fuse_changer_bit0_was_set = true; }
            if ((data & 2) != 0) { s->fuse_changer_bit1_was_set = true; }
            DPRINTF("SEP PMGR_BASE: fuse change write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
            goto jump_default;
        default:
            DPRINTF("SEP PMGR_BASE: Unknown write at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, data);
        jump_default:
            memcpy(&s->base_regs[addr], &data, size);
            break;
    }
}

static uint64_t pmgr_base_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSEPPMGRState* s   = opaque;
    uint64_t           ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    memcpy(&ret, &s->base_regs[addr], size);
    switch (addr) {
        case 0x10:    // mod_AES_HDCP
        case 0x20:    // mod_PKA ; PKA0
        case 0x28:    // mod_TRNG
        case 0x30:    // PKA1
        case 0x48:    // mod_I2C
        case 0x58:    // mod_KEY
        case 0x60:    // mod_EISP
        case 0x68:    // mod_SEPD
            DPRINTF("SEP PMGR_BASE: PowerState %s read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n",
                    sepos_powerstate_name(addr), addr, ret);
            break;
        case 0x8200:
#ifdef SEP_ENABLE_TRACE_BUFFER
            if (s->sep->chip_id == 0x8015) {
                apple_sep_debug_trace_enable(s->sep->debug_trace);    // for T8015
            }
#endif
            goto jump_default;
        default:
        jump_default:
            DPRINTF("SEP PMGR_BASE: Unknown read at 0x" HWADDR_FMT_plx " with value 0x%" PRIX64 "\n", addr, ret);
            break;
    }

    return ret;
}

static const MemoryRegionOps pmgr_base_reg_ops = {
    .write                 = pmgr_base_reg_write,
    .read                  = pmgr_base_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.unaligned       = false,
};

static void apple_sep_pmgr_reset_enter(Object* obj, ResetType type)
{
    AppleSEPPMGRState* s = APPLE_SEP_PMGR(obj);

    s->fuse_changer_bit0_was_set = false;
    s->fuse_changer_bit1_was_set = false;
    memset(s->base_regs, 0, sizeof(s->base_regs));
}

static void apple_sep_pmgr_realize(DeviceState* dev, Error** errp)
{
    AppleSEPPMGRState* s   = APPLE_SEP_PMGR(dev);
    SysBusDevice*      sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->base_mr, OBJECT(s), &pmgr_base_reg_ops, s, "sep.pmgr_base", PMGR_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->base_mr);
}

static void apple_sep_pmgr_class_init(ObjectClass* klass, const void* class_data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_sep_pmgr_reset_enter;
    dc->realize      = apple_sep_pmgr_realize;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleSEPPMGRState, apple_sep_pmgr, APPLE_SEP_PMGR, SYS_BUS_DEVICE)

AppleSEPPMGRState* apple_sep_pmgr_create(AppleSEP* sep)
{
    AppleSEPPMGRState* s = APPLE_SEP_PMGR(qdev_new(TYPE_APPLE_SEP_PMGR));
    s->sep               = sep;
    return s;
}

bool apple_sep_pmgr_get_fuse_changer_bit(AppleSEPPMGRState* s, uint8_t bit)
{
    switch (bit) {
        case 0 : return s->fuse_changer_bit0_was_set;
        case 1 : return s->fuse_changer_bit1_was_set;
        default: assert_not_reached();
    }
}
