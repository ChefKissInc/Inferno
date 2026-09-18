/*
 * USB xHCI controller for system-bus interface
 * Based on hcd-echi-sysbus.c

 * SPDX-FileCopyrightText: 2020 Xilinx
 * SPDX-FileContributor: Author: Sai Pavan Boddu <sai.pavan.boddu@xilinx.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "trace.h"
#include "qapi/error.h"
#include "hcd-xhci-sysbus.h"
#include "hw/irq.h"

static bool xhci_sysbus_intr_raise(XHCIState* xhci, int n, bool level)
{
    XHCISysbusState* s = container_of(xhci, XHCISysbusState, xhci);

    qemu_set_irq(s->irq[n], level);

    return false;
}

static void xhci_sysbus_realize(DeviceState* dev, Error** errp)
{
    XHCISysbusState* s = XHCI_SYSBUS(dev);

    object_property_set_link(OBJECT(&s->xhci), "host", OBJECT(s), NULL);
    if (!qdev_realize(DEVICE(&s->xhci), NULL, errp)) { return; }
    s->irq = g_new0(qemu_irq, s->xhci.numintrs);
    qdev_init_gpio_out_named(dev, s->irq, SYSBUS_DEVICE_GPIO_IRQ, s->xhci.numintrs);
    if (s->xhci.dma_mr) {
        s->xhci.as = g_malloc0(sizeof(AddressSpace));
        address_space_init(s->xhci.as, s->xhci.dma_mr, NULL);
    }
    else {
        s->xhci.as = &address_space_memory;
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->xhci.mem);
}

static void xhci_sysbus_init(Object* obj)
{
    XHCISysbusState* s = XHCI_SYSBUS(obj);

    object_initialize_child(obj, "xhci-core", &s->xhci, TYPE_XHCI);
    qdev_alias_all_properties(DEVICE(&s->xhci), obj);

    object_property_add_link(obj, "dma", TYPE_MEMORY_REGION, (Object**)&s->xhci.dma_mr,
                             qdev_prop_allow_set_link_before_realize, OBJ_PROP_LINK_STRONG);
    s->xhci.intr_update = NULL;
    s->xhci.intr_raise  = xhci_sysbus_intr_raise;
}

static const Property xhci_sysbus_props[] = {
    DEFINE_PROP_UINT32("intrs", XHCISysbusState, xhci.numintrs, XHCI_MAXINTRS),
    DEFINE_PROP_UINT32("slots", XHCISysbusState, xhci.numslots, XHCI_MAXSLOTS),
};

static void xhci_sysbus_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize = xhci_sysbus_realize;
    device_class_set_props(dc, xhci_sysbus_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(XHCISysbusState, xhci_sysbus, XHCI_SYSBUS, SYS_BUS_DEVICE)
