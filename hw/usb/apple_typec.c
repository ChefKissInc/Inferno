/*
 * Apple Type-C.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/qdev-properties.h"
#include "hw/usb/apple_typec.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

static void apple_typec_realize(DeviceState* dev, Error** errp)
{
    AppleTypeCState* s = APPLE_TYPEC(dev);
    Object*          obj;
    BusState*        bus = NULL;

    memory_region_init(&s->dma_container_mr, OBJECT(dev), TYPE_APPLE_TYPEC ".dma-container-mr", UINT32_MAX);
    obj       = object_property_get_link(OBJECT(dev), "dma-drd", errp);
    s->dma_mr = MEMORY_REGION(obj);
    memory_region_add_subregion(&s->dma_container_mr, 0, s->dma_mr);

    object_property_add_const_link(OBJECT(&s->dwc3), "dma-mr", OBJECT(&s->dma_container_mr));

    obj = object_property_get_link(OBJECT(dev), "dma-xhci", errp);
    assert_nonnull(obj);
    object_property_add_const_link(OBJECT(&s->dwc3), "dma-xhci", obj);

    obj = object_property_get_link(OBJECT(dev), "dma-otg", errp);
    assert_nonnull(obj);
    object_property_add_const_link(OBJECT(&s->dwc2), "dma-mr", obj);

    sysbus_realize(SYS_BUS_DEVICE(&s->dwc2), errp);
    sysbus_realize(SYS_BUS_DEVICE(&s->dwc3), errp);
    sysbus_pass_irq(SYS_BUS_DEVICE(s), SYS_BUS_DEVICE(&s->dwc3));
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->dwc2.irq);

    if (s->host == NULL) {
        error_setg(errp, "`host' link is not set");
        return;
    }
    sysbus_realize(s->host, errp);

    bus = QLIST_FIRST(&DEVICE(s->host)->child_bus);
    qdev_realize(DEVICE(s->dwc2.device), bus, errp);
    qdev_realize(DEVICE(&s->dwc3.device), bus, errp);
}

static void phy_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: phy reg WRITE @ 0x" HWADDR_FMT_plx "
    // value: 0x" HWADDR_FMT_plx "\n", addr, data);

    AppleTypeCState* s = opaque;
    memcpy(s->phy_reg + addr, &data, size);
}

static uint64_t phy_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: phy reg READ @ 0x" HWADDR_FMT_plx "\n",
    // addr);
    AppleTypeCState* s   = opaque;
    uint64_t         val = 0;

    memcpy(&val, s->phy_reg + addr, size);
    return val;
}

static const MemoryRegionOps phy_reg_ops = {
    .write = phy_reg_write,
    .read  = phy_reg_read,
};

static void config_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: config reg WRITE @ 0x" HWADDR_FMT_plx "
    // value: 0x" HWADDR_FMT_plx "\n", addr, data); arm_cpu_backtrace();

    AppleTypeCState* s = opaque;
    memcpy(s->config_reg + addr, &data, size);
}

static uint64_t config_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: config reg READ @ 0x" HWADDR_FMT_plx "\n",
    // addr); arm_cpu_backtrace();
    AppleTypeCState* s   = opaque;
    uint64_t         val = 0;

    memcpy(&val, s->config_reg + addr, size);
    return val;
}

static const MemoryRegionOps config_reg_ops = {
    .write = config_reg_write,
    .read  = config_reg_read,
};

static void apple_typec_init(Object* obj)
{
    DeviceState*     dev;
    SysBusDevice*    sbd;
    AppleTypeCState* s;

    dev = DEVICE(obj);
    sbd = SYS_BUS_DEVICE(dev);
    s   = APPLE_TYPEC(dev);

    memory_region_init(&s->container, OBJECT(dev), TYPE_APPLE_TYPEC ".container", ATC_USB_MMIO_SIZE);
    memory_region_init_io(&s->phy, OBJECT(dev), &phy_reg_ops, s, TYPE_APPLE_TYPEC ".phy", sizeof(s->phy_reg));
    memory_region_add_subregion(&s->container, 0x0000, &s->phy);

    memory_region_init_io(&s->config, OBJECT(dev), &config_reg_ops, s, TYPE_APPLE_TYPEC ".config",
                          sizeof(s->config_reg));
    memory_region_add_subregion(&s->container, 0x20000, &s->config);
    *(uint32_t*)(s->config_reg + 0x20) |= 0x40000000;    // pipe ready
    *(uint32_t*)(s->phy_reg + 0x64)    |= (1 << 16);     // OTG cable connected

    object_property_add_link(obj, "host", TYPE_SYS_BUS_DEVICE, (Object**)&s->host,
                             qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);

    object_initialize_child(OBJECT(dev), "dwc2", &s->dwc2, TYPE_DWC2_USB);
    object_initialize_child(OBJECT(dev), "dwc3", &s->dwc3, TYPE_DWC3_USB);
    object_property_set_uint(OBJECT(&s->dwc3), "intrs", 4, &error_fatal);
    object_property_set_uint(OBJECT(&s->dwc3), "slots", 1, &error_fatal);
    memory_region_add_subregion(&s->container, 0x10000, sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dwc3), 0));
    memory_region_add_subregion(&s->container, 0x100000, sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dwc2), 0));
    sysbus_init_mmio(sbd, &s->container);
}

static void apple_typec_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);
    dc->realize     = apple_typec_realize;
    dc->desc        = "Apple Type C USB PHY";
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(AppleTypeCState, apple_typec, APPLE_TYPEC, SYS_BUS_DEVICE)
