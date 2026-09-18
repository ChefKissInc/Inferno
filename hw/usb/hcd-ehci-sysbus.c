/*
 * QEMU USB EHCI Emulation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb/hcd-ehci.h"

static const Property ehci_sysbus_properties[] = {
    DEFINE_PROP_UINT32("maxframes", EHCISysBusState, ehci.maxframes, 128),
    DEFINE_PROP_BOOL("companion-enable", EHCISysBusState, ehci.companion_enable, false),
};

static void usb_ehci_sysbus_reset_hold(Object* obj, ResetType type)
{
    EHCISysBusState* i = SYS_BUS_EHCI(obj);
    EHCIState*       s = &i->ehci;

    usb_ehci_reset(s);
}

static void usb_ehci_sysbus_realize(DeviceState* dev, Error** errp)
{
    SysBusDevice*    d = SYS_BUS_DEVICE(dev);
    EHCISysBusState* i = SYS_BUS_EHCI(dev);
    EHCIState*       s = &i->ehci;

    usb_ehci_realize(s, dev, errp);
    sysbus_init_irq(d, &s->irq);
}

static void ehci_sysbus_init(Object* obj)
{
    SysBusDevice*    d   = SYS_BUS_DEVICE(obj);
    EHCISysBusState* i   = SYS_BUS_EHCI(obj);
    SysBusEHCIClass* sec = SYS_BUS_EHCI_GET_CLASS(obj);
    EHCIState*       s   = &i->ehci;

    s->capsbase   = sec->capsbase;
    s->opregbase  = sec->opregbase;
    s->portscbase = sec->portscbase;
    s->portnr     = sec->portnr;
    s->as         = &address_space_memory;

    usb_ehci_init(s, DEVICE(obj));
    sysbus_init_mmio(d, &s->mem);
}

static void ehci_sysbus_finalize(Object* obj)
{
    EHCISysBusState* i = SYS_BUS_EHCI(obj);
    EHCIState*       s = &i->ehci;

    usb_ehci_finalize(s);
}

static void ehci_sysbus_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc  = RESETTABLE_CLASS(klass);
    DeviceClass*     dc  = DEVICE_CLASS(klass);
    SysBusEHCIClass* sec = SYS_BUS_EHCI_CLASS(klass);

    rc->phases.hold = usb_ehci_sysbus_reset_hold;

    dc->realize = usb_ehci_sysbus_realize;
    device_class_set_props(dc, ehci_sysbus_properties);
    set_bit(DEVICE_CATEGORY_USB, dc->categories);

    sec->portscbase = 0x44;
    sec->portnr     = EHCI_PORTS;
}

static const TypeInfo ehci_sysbus_types[] = {
    {
        .name   = TYPE_SYS_BUS_EHCI,
        .parent = TYPE_SYS_BUS_DEVICE,
        OBJECT_TYPE_INSTANCE(EHCISysBusState),
        .instance_init     = ehci_sysbus_init,
        .instance_finalize = ehci_sysbus_finalize,
        .abstract          = true,
        .class_init        = ehci_sysbus_class_init,
        .class_size        = sizeof(SysBusEHCIClass),
    },
};

DEFINE_TYPES(ehci_sysbus_types)
