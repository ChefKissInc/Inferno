/*
 * Apple Temp Sensor.
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
#include "hw/sensor/tempsensor.h"
#include "system/memory.h"

struct AppleTempSensorState
{
    /*< private >*/
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq     irq;

    /*< public >*/
};

static void apple_temp_sensor_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleTempSensorState* s = opaque;

    switch (addr) {
        default: break;
    }
}

static uint64_t apple_temp_sensor_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleTempSensorState* s = opaque;

    switch (addr) {
        default: return 0xAFAFAFAF;
    }
}

static const MemoryRegionOps apple_temp_sensor_reg_ops = {
    .write                 = apple_temp_sensor_reg_write,
    .read                  = apple_temp_sensor_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

SysBusDevice* apple_temp_sensor_create(AppleDTNode* node)
{
    DeviceState*          dev;
    AppleTempSensorState* s;
    SysBusDevice*         sbd;
    AppleDTProp*          prop;
    uint64_t*             reg;

    dev = qdev_new(TYPE_APPLE_TEMP_SENSOR);
    s   = APPLE_TEMP_SENSOR(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    memory_region_init_io(&s->iomem, OBJECT(dev), &apple_temp_sensor_reg_ops, s, TYPE_APPLE_TEMP_SENSOR ".regs",
                          reg[1]);
    sysbus_init_mmio(sbd, &s->iomem);

    sysbus_init_irq(sbd, &s->irq);

    return sbd;
}

static void apple_temp_sensor_reset_hold(Object* obj, ResetType type)
{ AppleTempSensorState* s = APPLE_TEMP_SENSOR(obj); }

static void apple_temp_sensor_realize(DeviceState* dev, Error** errp) { }

static void apple_temp_sensor_class_init(ObjectClass* oc, const void* data)
{
    ResettableClass* rc;
    DeviceClass*     dc;

    rc = RESETTABLE_CLASS(oc);
    dc = DEVICE_CLASS(oc);

    rc->phases.hold = apple_temp_sensor_reset_hold;

    dc->desc           = "Apple Temp Sensor";
    dc->user_creatable = false;
    dc->realize        = apple_temp_sensor_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleTempSensorState, apple_temp_sensor, APPLE_TEMP_SENSOR, SYS_BUS_DEVICE)
