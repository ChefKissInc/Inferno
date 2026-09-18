/*
 * Apple CS35L27 Amp.
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
#include "hw/audio/cs35l27.h"
#include "hw/i2c/i2c.h"

#if 0
    #define DPRINTF(v, ...) fprintf(stderr, v, ##__VA_ARGS__)
#else
    #define DPRINTF(v, ...) \
        do { }              \
        while (0)
#endif

#define CS35L27_REG_SIZE (0x1000000)

struct AppleCS35L27State
{
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
    uint32_t addr;
    uint32_t tx_bytes;
    uint8_t  regs[CS35L27_REG_SIZE];
};

#define DEVID_SFT_RESET_DEVICE_ID_REG   (0x0)
#define VPBR_PAC_INT_INT_REGISTER_1     (0x2810)
#define VPBR_PAC_INT_INT_REGISTER_2     (0x2814)
#define VPBR_PAC_INT_INT_REGISTER_3     (0x2818)
#define VPBR_PAC_INT_INT_REGISTER_4     (0x281C)
#define VPBR_PAC_INT_INT_REGISTER_5     (0x2820)
#define VPBR_PAC_INT_INT_REGISTER_6     (0x2824)
#define CLOCKING_GLOBAL_SAMPLE_RATE     (0x3004)
#define BST_CLG_SPWR_VBST_RATIO_CTL     (0x3800)
#define BST_CLG_SPWR_CLASSG_CONFIG      (0x3820)
#define BST_CLG_SPWR_CLASSG_HDRM_CONFIG (0x3824)
#define AMP_PCM_AMP_PCM_CONTROL         (0x5000)

static uint8_t apple_cs35l27_rx(I2CSlave* i2c)
{
    AppleCS35L27State* s;
    uint8_t            ret = 0x00;

    s = APPLE_CS35L27(i2c);

    switch (s->addr) {
        case DEVID_SFT_RESET_DEVICE_ID_REG    :
        case DEVID_SFT_RESET_DEVICE_ID_REG + 1:
        case DEVID_SFT_RESET_DEVICE_ID_REG + 2:
        case DEVID_SFT_RESET_DEVICE_ID_REG + 3:
            ret = (cpu_to_be32(0x0035A270) >> ((s->addr - DEVID_SFT_RESET_DEVICE_ID_REG) * 8)) & 0xFF;
            break;
        case VPBR_PAC_INT_INT_REGISTER_2    :
        case VPBR_PAC_INT_INT_REGISTER_2 + 1:
        case VPBR_PAC_INT_INT_REGISTER_2 + 2:
        case VPBR_PAC_INT_INT_REGISTER_2 + 3:
            ret = (cpu_to_be32(BIT(12)) >> ((s->addr - VPBR_PAC_INT_INT_REGISTER_2) * 8)) & 0xFF;
            break;
        case VPBR_PAC_INT_INT_REGISTER_6    :
        case VPBR_PAC_INT_INT_REGISTER_6 + 1:
        case VPBR_PAC_INT_INT_REGISTER_6 + 2:
        case VPBR_PAC_INT_INT_REGISTER_6 + 3:
            ret = (cpu_to_be32(BIT(27)) >> ((s->addr - VPBR_PAC_INT_INT_REGISTER_6) * 8)) & 0xFF;
            break;
        default:
            if (s->addr < 0x1000000) { ret = s->regs[s->addr]; }
            break;
    }

    DPRINTF("%s: addr=0x%X, ret=0x%X\n", __func__, s->addr, ret);

    s->addr += 1;
    if (s->addr == 4) {
        s->addr     = 0;
        s->tx_bytes = 0;
    }

    return ret;
}

static int apple_cs35l27_tx(I2CSlave* i2c, uint8_t data)
{
    AppleCS35L27State* s;

    s = APPLE_CS35L27(i2c);

    DPRINTF("%s: 0x%02X\n", __func__, data);

    if (s->tx_bytes < 4) {
        s->addr |= ((uint32_t)data) << (s->tx_bytes * 8);
        if (s->tx_bytes == 3) {
            s->addr = be32_to_cpu(s->addr);
            DPRINTF("%s: set addr=0x%X\n", __func__, s->addr);
        }
        s->tx_bytes += 1;
    }
    else if (s->tx_bytes < 0x40) {
        DPRINTF("%s: addr=0x%X,data=0x%02X\n", __func__, s->addr, data);
        if (s->addr < 0x1000000) { s->regs[s->addr] = data; }
        s->addr     += 1;
        s->tx_bytes += 1;
    }
    else {
        DPRINTF("%s: chunk end\n", __func__);
        s->addr     = 0;
        s->tx_bytes = 0;
    }

    return 0;
}

static int apple_cs35l27_event(I2CSlave* i2c, enum i2c_event event)
{
    AppleCS35L27State* s;

    s = APPLE_CS35L27(i2c);

    switch (event) {
        case I2C_START_RECV      : DPRINTF("%s: I2C_START_RECV\n", __func__); break;
        case I2C_START_SEND      : DPRINTF("%s: I2C_START_SEND\n", __func__); break;
        case I2C_START_SEND_ASYNC: DPRINTF("%s: I2C_START_SEND_ASYNC\n", __func__); break;
        case I2C_FINISH:
            DPRINTF("%s: I2C_FINISH\n", __func__);
            s->addr     = 0;
            s->tx_bytes = 0;
            break;
        case I2C_NACK: DPRINTF("%s: I2C_NACK\n", __func__); break;
    }
    return 0;
}

static void apple_cs35l27_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*   dc = DEVICE_CLASS(klass);
    I2CSlaveClass* c  = I2C_SLAVE_CLASS(klass);

    dc->desc           = "Apple CS35L27 Amp";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->recv  = apple_cs35l27_rx;
    c->send  = apple_cs35l27_tx;
    c->event = apple_cs35l27_event;
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleCS35L27State, apple_cs35l27, APPLE_CS35L27, I2C_SLAVE)
