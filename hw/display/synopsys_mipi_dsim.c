/*
 * Synopsys MIPI DSIM.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "exec/hwaddr.h"
#include "hw/display/synopsys_mipi_dsim.h"
#include "hw/registerfields.h"
#include "system/memory.h"

// clang-format off
REG32(CORE_VERSION, 0x0)
REG32(CORE_PWR_UP, 0x4)
    REG_FIELD(CORE_PWR_UP, SHUTDOWNZ, 0, 1)
REG32(CORE_CMD_PKT_STATUS, 0x74)
    REG_FIELD(CORE_CMD_PKT_STATUS, GEN_CMD_EMPTY, 0, 1)
REG32(GENERAL_CTRL, 0x80004)
    REG_FIELD(GENERAL_CTRL, PHYLOCK_HW_LOCK, 4, 1)
REG32(TOP_PLL_CTRL, 0x80034)
// clang-format on

struct SynopsysMIPIDSIMState
{
    /*< private >*/
    SysBusDevice parent_obj;
    MemoryRegion iomems[2];
    qemu_irq     irqs[2];

    /*< public >*/
    uint32_t power_up;
};

static void synopsys_mipi_dsim_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    SynopsysMIPIDSIMState* s = opaque;

    switch (addr >> 2) {
        case R_CORE_PWR_UP: s->power_up = data; break;
        default           : break;
    }
}

static uint64_t synopsys_mipi_dsim_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    SynopsysMIPIDSIMState* s = opaque;

    switch (addr >> 2) {
        case R_CORE_VERSION       : return 0x3133302A;
        case R_CORE_PWR_UP        : return s->power_up;
        case R_CORE_CMD_PKT_STATUS: return REG_FIELD_DP32(0, CORE_CMD_PKT_STATUS, GEN_CMD_EMPTY, 1);
        case R_GENERAL_CTRL       : return REG_FIELD_DP32(0, GENERAL_CTRL, PHYLOCK_HW_LOCK, 1);
        case R_TOP_PLL_CTRL       : return 0;
        default                   : return UINT32_MAX;
    }
}

static const MemoryRegionOps synopsys_mipi_dsim_reg_ops = {
    .write                 = synopsys_mipi_dsim_reg_write,
    .read                  = synopsys_mipi_dsim_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void synopsys_mipi_dsim_swmpr_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    switch (addr >> 2) {
        default: break;
    }
}

static uint64_t synopsys_mipi_dsim_swmpr_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    switch (addr >> 2) {
        default: return UINT32_MAX;
    }
}

static const MemoryRegionOps synopsys_mipi_dsim_swmpr_reg_ops = {
    .write                 = synopsys_mipi_dsim_swmpr_reg_write,
    .read                  = synopsys_mipi_dsim_swmpr_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

SysBusDevice* synopsys_mipi_dsim_create(AppleDTNode* node)
{
    DeviceState*           dev;
    SynopsysMIPIDSIMState* s;
    SysBusDevice*          sbd;
    AppleDTProp*           prop;
    uint64_t*              reg;

    dev = qdev_new(TYPE_SYNOPSYS_MIPI_DSIM);
    s   = SYNOPSYS_MIPI_DSIM(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    memory_region_init_io(&s->iomems[0], OBJECT(dev), &synopsys_mipi_dsim_reg_ops, s, TYPE_SYNOPSYS_MIPI_DSIM ".regs",
                          reg[1]);
    sysbus_init_mmio(sbd, &s->iomems[0]);
    memory_region_init_io(&s->iomems[1], OBJECT(dev), &synopsys_mipi_dsim_swmpr_reg_ops, s,
                          TYPE_SYNOPSYS_MIPI_DSIM ".swmpr_regs", reg[3]);
    sysbus_init_mmio(sbd, &s->iomems[1]);

    sysbus_init_irq(sbd, &s->irqs[0]);
    sysbus_init_irq(sbd, &s->irqs[1]);

    return sbd;
}

static void synopsys_mipi_dsim_reset_hold(Object* obj, ResetType type)
{
    SynopsysMIPIDSIMState* s = SYNOPSYS_MIPI_DSIM(obj);

    // Default to display powered on
    s->power_up = REG_FIELD_DP32(0, CORE_PWR_UP, SHUTDOWNZ, 1);
}

static void synopsys_mipi_dsim_class_init(ObjectClass* oc, const void* data)
{
    ResettableClass* rc;
    DeviceClass*     dc;

    rc = RESETTABLE_CLASS(oc);
    dc = DEVICE_CLASS(oc);

    rc->phases.hold = synopsys_mipi_dsim_reset_hold;

    dc->desc           = "Synopsys MIPI DSIM";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(SynopsysMIPIDSIMState, synopsys_mipi_dsim, SYNOPSYS_MIPI_DSIM, SYS_BUS_DEVICE)
