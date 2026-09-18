/*
 * Apple Roswell (Display Authentication CP Relay IC).
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/misc/roswell.h"
#include "qemu/log.h"

#if 0
    #define ROSWELL_DPRINTF(v, ...) fprintf(stderr, "%s:%s@%d: " v "\n", __func__, __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define ROSWELL_DPRINTF(v, ...) \
        do { }                      \
        while (0)
#endif

enum
{
    ROSWELL_COMMAND_GET_DEVICE_INFO = 0,
    ROSWELL_COMMAND_GEN_SIGNATURE,
    ROSWELL_COMMAND_GET_SIGNATURE_LENGTH,
    ROSWELL_COMMAND_GET_SIGNATURE,
    ROSWELL_COMMAND_GET_CERT_LENGTH,
    ROSWELL_COMMAND_GET_CERT,
    ROSWELL_COMMAND_GET_CERT_SN,
    ROSWELL_COMMAND_EXTENDED,
    ROSWELL_COMMAND_INVALID,
};

typedef struct
{
    uint8_t  buf[0x261];
    uint16_t len;
    uint16_t pos;
} AppleRoswellResponse;

struct AppleRoswellState
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
    uint8_t              command;
    AppleRoswellResponse resp;
};

static void apple_roswell_reset_command(AppleRoswellState* roswell)
{
    ROSWELL_DPRINTF("command end/reset");
    roswell->command = ROSWELL_COMMAND_INVALID;
    roswell->resp    = (const AppleRoswellResponse){0};
}

static uint8_t apple_roswell_pop_resp(AppleRoswellState* roswell)
{
    if (roswell->command == ROSWELL_COMMAND_INVALID) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no command set!!\n", __func__);
        return 0x00;
    }

    if (roswell->resp.len == 0) { return 0x00; }

    uint8_t ret = roswell->resp.buf[roswell->resp.pos++];
    ROSWELL_DPRINTF("-> 0x%02X", ret);

    if (roswell->resp.len == roswell->resp.pos) { apple_roswell_reset_command(roswell); }

    return ret;
}

static uint8_t apple_roswell_rx(I2CSlave* s)
{
    AppleRoswellState* roswell = container_of(s, AppleRoswellState, i2c);

    return apple_roswell_pop_resp(roswell);
}

static int apple_roswell_tx(I2CSlave* s, uint8_t data)
{
    AppleRoswellState* roswell = container_of(s, AppleRoswellState, i2c);

    ROSWELL_DPRINTF("0x%X", data);

    if (roswell->command != ROSWELL_COMMAND_INVALID) { return 0; }

    switch (data) {    // CP Auth Command ID
        case 0x00: roswell->command = ROSWELL_COMMAND_GET_DEVICE_INFO; break;
        case 0x10: roswell->command = ROSWELL_COMMAND_GEN_SIGNATURE; break;
        case 0x11: roswell->command = ROSWELL_COMMAND_GET_SIGNATURE_LENGTH; break;
        case 0x12: roswell->command = ROSWELL_COMMAND_GET_SIGNATURE; break;
        case 0x30: roswell->command = ROSWELL_COMMAND_GET_CERT_LENGTH; break;
        case 0x31: roswell->command = ROSWELL_COMMAND_GET_CERT; break;
        case 0x4E: roswell->command = ROSWELL_COMMAND_GET_CERT_SN; break;
        case 0x60: roswell->command = ROSWELL_COMMAND_EXTENDED; break;
        default  : qemu_log_mask(LOG_GUEST_ERROR, "%s: unknown command 0x%X\n", __func__, data); return -1;
    }

    ROSWELL_DPRINTF("run command 0x%X (-> %d)", data, roswell->command);

    switch (roswell->command) {
        case ROSWELL_COMMAND_GET_DEVICE_INFO:
            roswell->resp.buf[0] = 0x0;                     // Device Version
            roswell->resp.buf[1] = 0x0;                     // Firmware Version
            roswell->resp.buf[2] = 0x2;                     // Authentication Major Version
            roswell->resp.buf[3] = 0x0;                     // Authentication Minor Version
            stl_be_p(&roswell->resp.buf[4], 0xDEADBEEF);    // Device ID
            roswell->resp.len = 8;
            break;
        case ROSWELL_COMMAND_EXTENDED:
            // Not sure about possible arguments, so just
            // always doing "GET_IDSN".
            memset(&roswell->resp.buf[0], 0, 9);
            roswell->resp.buf[9] = 0xE3;    // check in `AppleAuthCPAID::_getIDSN`
            memcpy(&roswell->resp.buf[10], "CKRosw", 6);
            roswell->resp.len = 9 + 7;    // header + ack byte + idsn
            break;
        default: break;
    }

    return 0;
}

static void apple_roswell_reset_enter(Object* obj, ResetType type)
{
    AppleRoswellState* roswell = APPLE_ROSWELL(obj);
    apple_roswell_reset_command(roswell);
}

static void apple_roswell_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    I2CSlaveClass*   c  = I2C_SLAVE_CLASS(klass);

    rc->phases.enter = apple_roswell_reset_enter;

    dc->desc           = "Apple Roswell";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->recv = apple_roswell_rx;
    c->send = apple_roswell_tx;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleRoswellState, apple_roswell, APPLE_ROSWELL, I2C_SLAVE)
