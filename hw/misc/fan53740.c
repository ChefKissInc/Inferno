/*
 * ACC Buck FAN53740.
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
#include "hw/misc/fan53740.h"

struct FAN53740State
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
};

static uint8_t fan53740_rx(I2CSlave* s) { return 0x00; }

static int fan53740_tx(I2CSlave* s, uint8_t data) { return 0; }

static int fan53740_event(I2CSlave* s, enum i2c_event event) { return 0; }

static void fan53740_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*   dc = DEVICE_CLASS(klass);
    I2CSlaveClass* c  = I2C_SLAVE_CLASS(klass);

    dc->desc           = "ACC Buck FAN53740";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->recv  = fan53740_rx;
    c->send  = fan53740_tx;
    c->event = fan53740_event;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(FAN53740State, fan53740, FAN53740, I2C_SLAVE)
