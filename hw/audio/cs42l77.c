/*
 * Apple CS42L77 Amp.
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
#include "hw/audio/cs42l77.h"
#include "hw/qdev-core.h"
#include "hw/ssi/ssi.h"
#include "qemu/bswap.h"
#include "qemu/lockable.h"

#if 0
    #define DPRINTF(v, ...) fprintf(stderr, v, ##__VA_ARGS__)
#else
    #define DPRINTF(v, ...) \
        do { }              \
        while (0)
#endif

#define CS42L77_CHIP_ID (0x042A77)

#define CMD_SET_START_ADDRESS (0x80)
#define CMD_SET_LENGTH        (0x81)
#define CMD_WRITE_WORDS       (0xB2)
#define CMD_WRITE_BYTES       (0xB5)
#define CMD_READ_WORDS        (0xC4)
#define CMD_READ_BYTES        (0xC6)

#define STS_OK (0x50)

#define CS42L77_REG_SIZE (0x1000000)

struct AppleCS42L77State
{
    /*< private >*/
    SSIPeripheral parent_obj;

    /*< public >*/
    QemuMutex mutex;
    uint64_t  start_addr;
    uint64_t  address;
    uint32_t  pos;
    uint32_t  end_pos;
    uint16_t  length;
    uint8_t   cur_cmd;
    bool      data_ready;
    uint8_t   regs[CS42L77_REG_SIZE];
};

static void apple_cs42l77_realize(SSIPeripheral* dev, Error** errp) { qemu_mutex_init(&APPLE_CS42L77(dev)->mutex); }

static uint32_t apple_cs42l77_transfer(SSIPeripheral* dev, uint32_t val)
{
    AppleCS42L77State* s;
    uint8_t            ret;

    s   = APPLE_CS42L77(dev);
    ret = 0;

    QEMU_LOCK_GUARD(&s->mutex);

    if (s->pos == 0) { s->cur_cmd = (uint8_t)val; }

    switch (s->cur_cmd) {
        case CMD_SET_START_ADDRESS:
            if (s->pos == 0) {
                s->start_addr = 0;
                s->address    = 0;
                s->end_pos    = 3;
            }
            else {
                s->start_addr = deposit64(s->start_addr, (s->pos - 1) * 8, 8, val);
            }
            DPRINTF("%s: val=0x%X -> 0x%X\n", __func__, val, ret);
            if (s->pos == s->end_pos) {
                s->start_addr = le64_to_cpu(s->start_addr);
                s->address    = s->start_addr;
                DPRINTF("%s: set start_addr=0x%llX\n", __func__, s->start_addr);
            }
            break;
        case CMD_SET_LENGTH:
            if (s->pos == 0) {
                s->length  = 0;
                s->end_pos = 2;
            }
            else {
                s->length |= (((uint16_t)val) << ((s->pos - 1) * 8));
            }
            DPRINTF("%s: val=0x%X -> 0x%X\n", __func__, val, ret);
            if (s->pos == s->end_pos) {
                s->length = le16_to_cpu(s->length);
                DPRINTF("%s: set length=0x%X\n", __func__, s->length);
            }
            break;
        case CMD_READ_WORDS:
            if (s->pos == 0) {
                if (s->data_ready) {
                    s->end_pos =
                        sizeof(uint8_t) + MIN(s->start_addr + (s->length * sizeof(uint32_t)) - s->address, 0x80);
                }
                else {
                    s->end_pos    = sizeof(uint8_t);
                    s->data_ready = true;
                }
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
            }
            else if (s->pos == sizeof(uint8_t)) {
                ret = STS_OK;
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
            }
            else {
                ret = s->regs[s->address];
                DPRINTF("%s: address=0x%llX -> 0x%X\n", __func__, s->address, ret);
                s->address += 1;
            }
            if (s->pos == s->end_pos) {
                if (s->address == s->start_addr + s->length * sizeof(uint32_t)) {
                    s->address    = s->start_addr;
                    s->data_ready = false;
                    DPRINTF("%s: resetting address=0x%llX\n", __func__, s->address);
                }
            }
            break;
        case CMD_READ_BYTES:
            if (s->pos == 0) {
                s->end_pos = sizeof(uint8_t) + MIN((s->start_addr + s->length) - s->address, 0x20);
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
                ret = STS_OK;
            }
            else if (s->pos == 1) {
                ret = STS_OK;
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
            }
            else {
                ret = s->regs[s->address];
                DPRINTF("%s: address=0x%llX -> 0x%X\n", __func__, s->address, ret);
                s->address += 1;
            }
            if (s->pos == s->end_pos && s->address == s->start_addr + s->length) {
                s->address = s->start_addr;
                DPRINTF("%s: resetting address=0x%llX\n", __func__, s->address);
            }
            break;
        case CMD_WRITE_WORDS:
            if (s->pos == 0) {
                s->end_pos = MIN(s->start_addr + (s->length * sizeof(uint32_t)) - s->address, 0x80);
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
            }
            else {
                DPRINTF("%s: address=0x%llX <- 0x%X\n", __func__, s->address, val);
                s->regs[s->address]  = (uint8_t)val;
                s->address          += 1;
            }
            ret = STS_OK;
            if (s->pos == s->end_pos && s->address == s->start_addr + s->length * sizeof(uint32_t)) {
                s->address = s->start_addr;
                DPRINTF("%s: resetting address=0x%llX\n", __func__, s->address);
            }
            break;
        case CMD_WRITE_BYTES:
            if (s->pos == 0) {
                s->end_pos = MIN(s->start_addr + s->length - s->address, 0x20);
                DPRINTF("%s: val=0x%X -> 0x%X, address=0x%llX\n", __func__, val, ret, s->address);
            }
            else {
                DPRINTF("%s: address=0x%llX <- 0x%X\n", __func__, s->address, val);
                s->regs[s->address]  = (uint8_t)val;
                s->address          += 1;
            }
            ret = STS_OK;
            if (s->pos == s->end_pos && s->address == s->start_addr + s->length) {
                s->address = s->start_addr;
                DPRINTF("%s: resetting address=0x%llX\n", __func__, s->address);
            }
            break;
        case 0x90:
            if (s->pos == 0) {
                s->end_pos = 1;
                ret        = STS_OK;
            }
            else {
                // Bit 7: SPI Ready
                // Bit 4: Bus Write Ready
                // Bit 3: PLL
                ret = BIT(3) | BIT(4) | BIT(7);
            }
            DPRINTF("%s: val=0x%X -> 0x%X\n", __func__, val, ret);
            break;
        case 0x91:
            if (s->pos == 0) {
                s->end_pos = 2;
                ret        = STS_OK;
            }
            else if (s->pos == s->end_pos) {
                ret = 0;
            }
            DPRINTF("%s: val=0x%X -> 0x%X\n", __func__, val, ret);
            break;
        case 0xA1:
            if (s->pos == 0) {
                s->end_pos = 1;
                ret        = STS_OK;
            }
            else {
                if ((val & BIT(6)) != 0) {
                    s->address = s->start_addr;
                    DPRINTF("%s: Reset address back to 0x%llX\n", __func__, s->address);
                }
            }
            break;
        default: DPRINTF("%s: val=0x%X -> 0x%X\n", __func__, val, ret); break;
    }

    if (s->pos == s->end_pos) {
        DPRINTF("%s: cmd 0x%X end at 0x%X, start_addr=0x%llX, address=0x%llX, "
                "len=0x%X\n",
                __func__, s->cur_cmd, s->pos, s->start_addr, s->address, s->length);

        s->cur_cmd = 0;
        s->pos     = 0;
        s->end_pos = 0;
    }
    else {
        s->pos += 1;
    }

    return ret;
}

static void apple_cs42l77_reset_enter(Object* obj, ResetType type)
{
    AppleCS42L77State* s = APPLE_CS42L77(obj);

    s->start_addr = 0;
    s->address    = 0;
    s->pos        = 0;
    s->end_pos    = 0;
    s->length     = 0;
    s->cur_cmd    = 0;
    s->data_ready = false;
    memset(s->regs, 0, sizeof(s->regs));
    stl_le_p(s->regs, CS42L77_CHIP_ID);
    stl_le_p(s->regs + 0x20034, 3);    // CLP State
    stl_le_p(s->regs + 0x40048, BIT(4));
    stl_le_p(s->regs + 0x40054, BIT(3));     // ANC_BLOCK_POWER_UP
    stl_le_p(s->regs + 0xBC09C8, 0xA500);    // DSP_TO_HOST
}

static void apple_cs42l77_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass*    rc = RESETTABLE_CLASS(klass);
    DeviceClass*        dc = DEVICE_CLASS(klass);
    SSIPeripheralClass* c  = SSI_PERIPHERAL_CLASS(klass);

    rc->phases.enter = apple_cs42l77_reset_enter;

    dc->desc           = "Apple CS42L77 Amp";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->realize  = apple_cs42l77_realize;
    c->transfer = apple_cs42l77_transfer;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleCS42L77State, apple_cs42l77, APPLE_CS42L77, SSI_PERIPHERAL)
