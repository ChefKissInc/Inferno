/*
 * Apple Mic/ICA60 Temp Sensor.
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
#include "hw/sensor/mic_tempsensor.h"
#include "hw/registerfields.h"
#include "qapi/error.h"

// clang-format off
REG8(ID0, 0)
    REG_FIELD(ID0, PRODUCT_ID, 0, 5)
    REG_FIELD(ID0, VENDOR_ID, 5, 3)
REG8(ID1, 1)
    REG_FIELD(ID1, REVISION, 0, 5)
    REG_FIELD(ID1, FAB_ID, 5, 3)
REG8(STATUS, 2)
    REG_FIELD(STATUS, OK, 1, 1)
REG8(VALUE0, 4)
REG8(VALUE1, 5)
REG8(VALUE2, 6)
// clang-format on

struct AppleMicTempSensorState
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
    bool    receiving_addr;
    uint8_t addr;
    uint8_t cur_addr;
    uint8_t id0;
    uint8_t id1;
};

static uint8_t apple_mic_temp_sensor_rx(I2CSlave* s)
{
    AppleMicTempSensorState* sensor = container_of(s, AppleMicTempSensorState, i2c);
    uint8_t                  ret;

    switch (sensor->cur_addr) {
        case R_ID0   : ret = sensor->id0; break;
        case R_ID1   : ret = sensor->id1; break;
        case R_STATUS: ret = REG_FIELD_DP8(0, STATUS, OK, 1); break;
        case R_VALUE0: ret = 0x0A; break;
        case R_VALUE1:
        case R_VALUE2: ret = 0x00; break;
        default      : ret = 0x00; break;
    }

    ++sensor->cur_addr;
    return ret;
}

static int apple_mic_temp_sensor_tx(I2CSlave* s, uint8_t data)
{
    AppleMicTempSensorState* sensor = container_of(s, AppleMicTempSensorState, i2c);

    if (sensor->receiving_addr) {
        sensor->receiving_addr = false;
        sensor->cur_addr = sensor->addr = data;
    }
    else {
        ++sensor->cur_addr;
    }

    return 0;
}

static int apple_mic_temp_sensor_event(I2CSlave* s, enum i2c_event event)
{
    AppleMicTempSensorState* sensor = container_of(s, AppleMicTempSensorState, i2c);

    if (event == I2C_START_SEND) { sensor->receiving_addr = true; }
    else if (event == I2C_FINISH) {
        sensor->cur_addr = sensor->addr;
    }

    return 0;
}

static void apple_mic_temp_sensor_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*   dc = DEVICE_CLASS(klass);
    I2CSlaveClass* c  = I2C_SLAVE_CLASS(klass);

    dc->desc           = "Apple Mic/ICA60 Temp Sensor";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->recv  = apple_mic_temp_sensor_rx;
    c->send  = apple_mic_temp_sensor_tx;
    c->event = apple_mic_temp_sensor_event;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleMicTempSensorState, apple_mic_temp_sensor, APPLE_MIC_TEMP_SENSOR, I2C_SLAVE)

I2CSlave* apple_mic_temp_sensor_create(uint8_t addr, I2CBus* bus, uint8_t product_id, uint8_t vendor_id,
                                       uint8_t revision, uint8_t fab_id, Error** errp)
{
    ERRP_GUARD();

    I2CSlave*                dev    = i2c_slave_new(TYPE_APPLE_MIC_TEMP_SENSOR, addr);
    AppleMicTempSensorState* sensor = container_of(dev, AppleMicTempSensorState, i2c);

    sensor->id0 = REG_FIELD_DP8(REG_FIELD_DP8(0, ID0, PRODUCT_ID, product_id), ID0, VENDOR_ID, vendor_id);
    sensor->id1 = REG_FIELD_DP8(REG_FIELD_DP8(0, ID1, REVISION, revision), ID1, FAB_ID, fab_id);

    i2c_slave_realize_and_unref(dev, bus, errp);

    return dev;
}
