/*
 * Apple Chestnut Display PMU.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/i2c/i2c.h"
#include "hw/misc/chestnut.h"

#define REG_DEVICE_ID_0  (0x00)
#define REG_DEVICE_ID_1  (0x01)
#define REG_DEVICE_ID_2  (0x02)
#define REG_REVISION     (0x03)
#define REG_STATUS       (0x04)
#define REG_ENABLE       (0x05)
#define REG_VNEG_CONTROL (0x0F)

#define TI_DEVICE_ID_0     (0x7365)
#define TI_DEVICE_ID_1     (0xF365)
#define INTERSIL_DEVICE_ID (0x0BA46E)

struct AppleChestnutState
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
};

static uint8_t apple_chestnut_rx(I2CSlave* i2c) { return 0x00; }

static int apple_chestnut_tx(I2CSlave* i2c, uint8_t data) { return 0x00; }

static void apple_chestnut_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*   dc = DEVICE_CLASS(klass);
    I2CSlaveClass* c  = I2C_SLAVE_CLASS(klass);

    dc->desc           = "Apple Chestnut Display PMU";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->recv = apple_chestnut_rx;
    c->send = apple_chestnut_tx;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleChestnutState, apple_chestnut, APPLE_CHESTNUT, I2C_SLAVE)
