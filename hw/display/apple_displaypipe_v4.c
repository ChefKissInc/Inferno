/*
 * Apple Display Pipe V4 Controller.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "block/aio.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "system/dma.h"
#include "ui/console.h"
#include "pixman.h"

#ifdef CONFIG_PNG
    #include <png.h>
#else
    #error "PNG support is required"
#endif

#if 0
    #define ADP_INFO(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
    #define ADP_INFO(fmt, ...) \
        do { }                 \
        while (0);
#endif

/**
 * Block Bases (DisplayTarget5)
 * 0x08000  |  M3 Control Mailbox
 * 0x0A000  |  M3 Video Mode Mailbox
 * 0x40000  |  Control
 * 0x48000  |  Vertical Frame Timing Generator
 * 0x50000  |  Generic Pixel Pipe 0
 * 0x58000  |  Generic Pixel Pipe 1
 * 0x60000  |  Blend Unit
 * 0x70000  |  White Point Correction
 * 0x7C000  |  Pixel Response Correction
 * 0x80000  |  Dither
 * 0x82000  |  Dither: Enchanced ST Dither 0
 * 0x83000  |  Dither: Enchanced ST Dither 1
 * 0x84000  |  Content-Dependent Frame Duration
 * 0x88000  |  SPLR (Sub-Pixel Layout R?)
 * 0x90000  |  Burn-In Compensation Sampler
 * 0x98000  |  Sub-Pixel Uniformity Correction
 * 0xA0000  |  PDC (Panel Dither Correction?)
 * 0xB0000  |  PCC (Pixel Color Correction?)
 * 0xD0000  |  PCC Mailbox
 * 0xF0000  |  DBM (Dynamic Backlight Modulation?)
 */

/**
 * Interrupt Indices
 * 0 | Maybe VBlank
 * 1 | APT
 * 2 | Maybe GP0
 * 3 | Maybe GP1
 * 4 | ?
 * 5 | ?
 * 6 | ?
 * 7 | ?
 * 8 | M3
 * 9 | ?
 */

#define ADP_V4_PANEL_HZ (60)

/* 2 GenPipes, 2 Layers per GenPipe */
#define ADP_V4_GP_COUNT    (2)
#define ADP_V4_LAYER_COUNT (2)

typedef struct
{
    uint32_t config_control;
    uint32_t pixel_format;
    uint16_t dest_width;
    uint16_t dest_height;
    uint32_t data_start;
    uint32_t data_end;
    uint32_t stride;
    uint16_t src_width;
    uint16_t src_height;
    void*    buf;
    uint32_t buf_len;
    uint32_t buf_capacity;
    /// Cached image, to not remake it every single run.
    pixman_image_t* image;
} ADPV4GenPipeState;

typedef struct
{
    QemuMutex         lock;
    uint8_t           index;
    ADPV4GenPipeState state;
} ADPV4GenPipe;

typedef struct
{
    uint32_t layer_config[ADP_V4_LAYER_COUNT];
} ADPV4BlendUnitState;

struct AppleDisplayPipeV4State
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion        up_regs;
    uint32_t            width;
    uint32_t            height;
    MemoryRegion*       vram_mr;
    uint64_t            vram_off;
    uint64_t            vram_size;
    uint64_t            fb_off;
    MemoryRegion*       dma_mr;
    AddressSpace        dma_as;
    qemu_irq            irqs[9];
    uint32_t            int_status;
    uint32_t            int_enable;
    uint32_t            commit_pending;
    QEMUTimer*          vsync_timer;
    uint64_t            next_vsync_ns;
    ADPV4GenPipe        genpipe[ADP_V4_GP_COUNT];
    ADPV4BlendUnitState blend_unit;
    QemuConsole*        console;
    QEMUBH*             update_disp_image_bh;
    QEMUTimer*          boot_splash_timer;
};

// clang-format off
// pipe control
REG32(CONTROL_SHADOW_FIFO_STATUS, 0x45004)
    REG_FIELD(CONTROL_SHADOW_FIFO_STATUS, EMPTY, 0, 1)
    REG_FIELD(CONTROL_SHADOW_FIFO_STATUS, PENDING, 4, 3)
REG32(CONTROL_INT_STATUS, 0x45818)
    REG_FIELD(CONTROL_INT, MODE_CHANGED, 1, 1)
    REG_FIELD(CONTROL_INT, UNDERRUN, 3, 1)
    REG_FIELD(CONTROL_INT, OUTPUT_READY, 10, 1)
    REG_FIELD(CONTROL_INT, SUB_FRAME_OVERFLOW, 11, 1)
    REG_FIELD(CONTROL_INT, M3, 13, 1)
    REG_FIELD(CONTROL_INT, PCC, 17, 1)
    REG_FIELD(CONTROL_INT, CDFD, 19, 1)
    REG_FIELD(CONTROL_INT, FRAME_PROCESSED, 20, 1)
    REG_FIELD(CONTROL_INT, AXI_READ_ERR, 30, 1)
    REG_FIELD(CONTROL_INT, AXI_WRITE_ERR, 31, 1)
REG32(CONTROL_INT_ENABLE, 0x4581C)

// pipe config
REG32(CONTROL_VERSION, 0x46020)
#define CONTROL_VERSION_A0 (0x70044)
#define CONTROL_VERSION_A1 (0x70045)
REG32(CONTROL_UPDATE, 0x4602C)
    REG_FIELD(CONTROL_UPDATE, TAG, 8, 4)
    REG_FIELD(CONTROL_UPDATE, COMMIT, 12, 1)
REG32(CONTROL_FRAME_SIZE, 0x4603C)

#define GP_BLOCK_BASE (0x50000)
#define GP_BLOCK_SIZE (0x8000)
REG32(GP_CONFIG_CONTROL, 0x4)
    REG_FIELD(GP_CONFIG_CONTROL, NO_SCALE, 0, 1)
    REG_FIELD(GP_CONFIG_CONTROL, SCALE_CROPPED, 4, 1)
    REG_FIELD(GP_CONFIG_CONTROL, TWO_PLANE, 8, 1)
    REG_FIELD(GP_CONFIG_CONTROL, MULTI_PLANE, 12, 1)
    REG_FIELD(GP_CONFIG_CONTROL, GAMMA_SPACE, 16, 2)
    REG_FIELD(GP_CONFIG_CONTROL, HDR, 24, 1)
    REG_FIELD(GP_CONFIG_CONTROL, WIDE_GAMUT, 30, 1)
    REG_FIELD(GP_CONFIG_CONTROL, ENABLED, 31, 1)
REG32(GP_PIXEL_FORMAT, 0x1C)
#define GP_PIXEL_FORMAT_BGRA ((BIT32(4) << 22) | BIT32(24) | (3 << 13))
#define GP_PIXEL_FORMAT_ARGB ((BIT32(4) << 22) | BIT32(24))
#define GP_PIXEL_FORMAT_COMPRESSED BIT32(30)
REG32(GP_LAYER_0_HTPC_CONFIG, 0x28)
REG32(GP_LAYER_1_HTPC_CONFIG, 0x2C)
REG32(GP_LAYER_0_DATA_START, 0x30)
REG32(GP_LAYER_1_DATA_START, 0x34)
REG32(GP_LAYER_0_DATA_END, 0x40)
REG32(GP_LAYER_1_DATA_END, 0x44)
REG32(GP_LAYER_0_HEADER_BASE, 0x48)
REG32(GP_LAYER_1_HEADER_BASE, 0x4C)
REG32(GP_LAYER_0_HEADER_END, 0x58)
REG32(GP_LAYER_1_HEADER_END, 0x5C)
REG32(GP_LAYER_0_STRIDE, 0x60)
REG32(GP_LAYER_1_STRIDE, 0x64)
REG32(GP_LAYER_0_POSITION, 0x68)
REG32(GP_LAYER_1_POSITION, 0x6C)
REG32(GP_LAYER_0_DIMENSIONS, 0x70)
REG32(GP_LAYER_1_DIMENSIONS, 0x74)
REG32(GP_SRC_RECT, 0x78)
REG32(GP_DEST_POSITION, 0x7C)
REG32(GP_DEST_DIMENSIONS, 0x80)
REG32(GP_SRC_ACTIVE_REGION_0_POSITION, 0x98)
REG32(GP_SRC_ACTIVE_REGION_1_POSITION, 0x9C)
REG32(GP_SRC_ACTIVE_REGION_2_POSITION, 0xA0)
REG32(GP_SRC_ACTIVE_REGION_3_POSITION, 0xA4)
REG32(GP_SRC_ACTIVE_REGION_0_DIMENSIONS, 0xA8)
REG32(GP_SRC_ACTIVE_REGION_1_DIMENSIONS, 0xAC)
REG32(GP_SRC_ACTIVE_REGION_2_DIMENSIONS, 0xB0)
REG32(GP_SRC_ACTIVE_REGION_3_DIMENSIONS, 0xB4)
REG32(GP_CRC_DATA, 0x160)
REG32(GP_DMA_BANDWIDTH_RATE, 0x170)
REG32(GP_STATUS, 0x184)
#define GP_STATUS_DECOMPRESSION_FAIL BIT32(0)

#define GP_BLOCK_BASE_FOR(i) (GP_BLOCK_BASE + ((i) * GP_BLOCK_SIZE))
#define GP_BLOCK_END_FOR(i) (GP_BLOCK_BASE_FOR(i) + (GP_BLOCK_SIZE - 1))

#define BLEND_BLOCK_BASE (0x60000)
#define BLEND_BLOCK_SIZE (0x8000)
REG32(BLEND_CONFIG, 0x4)
REG32(BLEND_BG, 0x8)
REG32(BLEND_LAYER_0_BG, 0xC)
REG32(BLEND_LAYER_1_BG, 0x10)
REG32(BLEND_LAYER_0_CONFIG, 0x14)
REG32(BLEND_LAYER_1_CONFIG, 0x18)
#define BLEND_LAYER_CONFIG_PIPE(v) ((v) & 0xF)
#define BLEND_LAYER_CONFIG_MODE(v) (((v) >> 4) & 0xF)
#define BLEND_MODE_NONE (0)
#define BLEND_MODE_ALPHA (1)
#define BLEND_MODE_PREMULT (2)
#define BLEND_MODE_BYPASS (3)
REG32(BLEND_DEGAMMA_TABLE_R, 0x1C)
REG32(BLEND_DEGAMMA_TABLE_G, 0x1024)
REG32(BLEND_DEGAMMA_TABLE_B, 0x202C)
// REG32(BLEND_??, 0x3034)
REG32(BLEND_PIXCAP_CONFIG, 0x303C)
// clang-format on

static void adp_v4_update_irqs(AppleDisplayPipeV4State* genpipe)
{ qemu_set_irq(genpipe->irqs[0], (qatomic_read(&genpipe->int_enable) & qatomic_read(&genpipe->int_status)) != 0); }

static pixman_format_code_t adp_v4_gp_fmt_to_pixman(ADPV4GenPipe* genpipe)
{
    if ((genpipe->state.pixel_format & GP_PIXEL_FORMAT_BGRA) == GP_PIXEL_FORMAT_BGRA) {
        ADP_INFO("gp%d: pixel format is BGRA (0x%X).", genpipe->index, genpipe->state.pixel_format);
        return PIXMAN_b8g8r8a8;
    }
    if ((genpipe->state.pixel_format & GP_PIXEL_FORMAT_ARGB) == GP_PIXEL_FORMAT_ARGB) {
        ADP_INFO("gp%d: pixel format is ARGB (0x%X).", genpipe->index, genpipe->state.pixel_format);
        return PIXMAN_a8r8g8b8;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "gp%d: pixel format is unknown (0x%X).\n", genpipe->index,
                  genpipe->state.pixel_format);
    return 0;
}

static void adp_v4_gp_read(ADPV4GenPipe* genpipe, AddressSpace* dma_as)
{
    uint32_t len;

    genpipe->state.buf_len = 0;

    // TODO: Decompress the data and display it properly.
    if (genpipe->state.pixel_format & GP_PIXEL_FORMAT_COMPRESSED) {
        qemu_log_mask(LOG_GUEST_ERROR, "gp%d: dropping frame as it's compressed.\n", genpipe->index);
        return;
    }

    ADP_INFO("gp%d: width and height is %dx%d.", genpipe->index, genpipe->state.src_width, genpipe->state.src_height);
    ADP_INFO("gp%d: stride is %d.", genpipe->index, genpipe->state.stride);

    len = genpipe->state.src_height * genpipe->state.stride;
    if (genpipe->state.buf_capacity < len) {
        g_free(genpipe->state.buf);
        genpipe->state.buf          = g_malloc(len);
        genpipe->state.buf_capacity = len;
    }

    if (dma_memory_read(dma_as, genpipe->state.data_start, genpipe->state.buf, len, MEMTXATTRS_UNSPECIFIED) == MEMTX_OK)
    {
        genpipe->state.buf_len = len;
    }
    else {
        qemu_log_mask(LOG_GUEST_ERROR, "gp%d: failed to read from DMA.\n", genpipe->index);
    }
}

static void adp_v4_gp_image_changed_check(ADPV4GenPipe* genpipe, uint32_t old, uint32_t new)
{
    if (old == new) { return; }

    qemu_pixman_image_unref(genpipe->state.image);
    genpipe->state.image = NULL;
}

#define ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(_field, _val)                \
    do {                                                                    \
        typeof(_val) val = _val;                                            \
        adp_v4_gp_image_changed_check(genpipe, genpipe->state._field, val); \
        genpipe->state._field = val;                                        \
    }                                                                       \
    while (0)
#define ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(_field) ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(_field, (uint32_t)data);

static void adp_v4_gp_reg_write(ADPV4GenPipe* genpipe, hwaddr addr, uint64_t data)
{
    switch (addr >> 2) {
        case R_GP_CONFIG_CONTROL: {
            ADP_INFO("gp%d: control <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.config_control = (uint32_t)data;
            break;
        }
        case R_GP_PIXEL_FORMAT: {
            ADP_INFO("gp%d: pixel format <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(pixel_format);
            break;
        }
        case R_GP_LAYER_0_DATA_START: {
            ADP_INFO("gp%d: layer 0 data start <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.data_start = (uint32_t)data;
            break;
        }
        case R_GP_LAYER_0_DATA_END: {
            ADP_INFO("gp%d: layer 0 data end <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            genpipe->state.data_end = (uint32_t)data;
            break;
        }
        case R_GP_LAYER_0_STRIDE: {
            ADP_INFO("gp%d: layer 0 stride <- 0x" HWADDR_FMT_plx, genpipe->index, data);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK(stride);
            break;
        }
        case R_GP_LAYER_0_DIMENSIONS: {
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(src_height, data & 0xFFFF);
            ADP_V4_GP_REG_WRITE_SET_WITH_CHECK_VAL(src_width, (data >> 16) & 0xFFFF);
            ADP_INFO("gp%d: layer 0 dimensions <- 0x" HWADDR_FMT_plx " (%dx%d)", genpipe->index, data,
                     genpipe->state.src_width, genpipe->state.src_height);
            break;
        }
        case R_GP_DEST_DIMENSIONS: {
            genpipe->state.dest_height = data & 0xFFFF;
            genpipe->state.dest_width  = (data >> 16) & 0xFFFF;
            ADP_INFO("gp%d: dest dimensions <- 0x" HWADDR_FMT_plx " (%dx%d)", genpipe->index, data,
                     genpipe->state.dest_width, genpipe->state.dest_height);
            break;
        }
        default: {
            ADP_INFO("gp%d: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, genpipe->index, addr, data);
            break;
        }
    }
}

static uint32_t adp_v4_gp_reg_read(ADPV4GenPipe* genpipe, hwaddr addr)
{
    switch (addr >> 2) {
        case R_GP_CONFIG_CONTROL: {
            ADP_INFO("gp%d: control -> 0x%X", genpipe->index, genpipe->state.config_control);
            return genpipe->state.config_control;
        }
        case R_GP_PIXEL_FORMAT: {
            ADP_INFO("gp%d: pixel format -> 0x%X", genpipe->index, genpipe->state.pixel_format);
            return genpipe->state.pixel_format;
        }
        case R_GP_LAYER_0_DATA_START: {
            ADP_INFO("gp%d: layer 0 data start -> 0x%X", genpipe->index, genpipe->state.data_start);
            return genpipe->state.data_start;
        }
        case R_GP_LAYER_0_DATA_END: {
            ADP_INFO("gp%d: layer 0 data end -> 0x%X", genpipe->index, genpipe->state.data_end);
            return genpipe->state.data_end;
        }
        case R_GP_LAYER_0_STRIDE: {
            ADP_INFO("gp%d: layer 0 stride -> 0x%X", genpipe->index, genpipe->state.stride);
            return genpipe->state.stride;
        }
        case R_GP_LAYER_0_DIMENSIONS: {
            ADP_INFO("gp%d: layer 0 dimensions -> 0x%X (%dx%d)", genpipe->index,
                     (genpipe->state.src_width << 16) | genpipe->state.src_height, genpipe->state.src_width,
                     genpipe->state.src_height);
            return ((uint32_t)genpipe->state.src_width << 16) | genpipe->state.src_height;
        }
        case R_GP_DEST_DIMENSIONS: {
            ADP_INFO("gp%d: dest dimensions -> 0x%X (%dx%d)", genpipe->index,
                     (genpipe->state.dest_width << 16) | genpipe->state.dest_height, genpipe->state.dest_width,
                     genpipe->state.dest_height);
            return ((uint32_t)genpipe->state.dest_width << 16) | genpipe->state.dest_height;
        }
        default: {
            ADP_INFO("gp%d: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, genpipe->index, addr, (hwaddr)0);
            return 0;
        }
    }
}

static void adp_v4_gp_reset(ADPV4GenPipe* genpipe)
{
    qemu_pixman_image_unref(genpipe->state.image);
    g_free(genpipe->state.buf);
    genpipe->state = (ADPV4GenPipeState){0};
}

static void adp_v4_blend_reg_write(ADPV4BlendUnitState* blend, uint64_t addr, uint64_t data)
{
    switch (addr >> 2) {
        case R_BLEND_LAYER_0_CONFIG: {
            ADP_INFO("blend: layer 0 config <- 0x" HWADDR_FMT_plx, data);
            blend->layer_config[0] = (uint32_t)data;
            break;
        }
        case R_BLEND_LAYER_1_CONFIG: {
            blend->layer_config[1] = (uint32_t)data;
            ADP_INFO("blend: layer 1 config <- 0x" HWADDR_FMT_plx, data);
            break;
        }
        default: {
            ADP_INFO("blend: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);
            break;
        }
    }
}

static uint64_t adp_v4_blend_reg_read(ADPV4BlendUnitState* blend, uint64_t addr)
{
    switch (addr >> 2) {
        case R_BLEND_LAYER_0_CONFIG: {
            ADP_INFO("blend: layer 0 config -> 0x%X", blend->layer_config[0]);
            return blend->layer_config[0];
        }
        case R_BLEND_LAYER_1_CONFIG: {
            ADP_INFO("blend: layer 1 config -> 0x%X", blend->layer_config[1]);
            return blend->layer_config[1];
        }
        default: {
            ADP_INFO("blend: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, (hwaddr)0);
            return 0;
        }
    }
}

static void adp_v4_blend_reset(ADPV4BlendUnitState* blend) { *blend = (ADPV4BlendUnitState){0}; }

static void adp_v4_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleDisplayPipeV4State* adp = opaque;

    if (addr >= 0x200000) {    // some weird shadow shit
        addr -= 0x200000;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)
        && qatomic_read(&adp->commit_pending))
    {
        qemu_log_mask(LOG_GUEST_ERROR, "disp: write @ 0x" HWADDR_FMT_plx " while a commit is pending.\n", addr);
        return;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < GP_BLOCK_END_FOR(0)) {
        return adp_v4_gp_reg_write(&adp->genpipe[0], addr - GP_BLOCK_BASE_FOR(0), data);
    }

    if (addr >= GP_BLOCK_BASE_FOR(1) && addr < GP_BLOCK_END_FOR(1)) {
        return adp_v4_gp_reg_write(&adp->genpipe[1], addr - GP_BLOCK_BASE_FOR(1), data);
    }

    if (addr >= BLEND_BLOCK_BASE && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)) {
        return adp_v4_blend_reg_write(&adp->blend_unit, addr - BLEND_BLOCK_BASE, data);
    }

    switch (addr >> 2) {
        case R_CONTROL_INT_STATUS: {
            ADP_INFO("disp: int status <- 0x%X", (uint32_t)data);
            qatomic_and(&adp->int_status, ~(uint32_t)data);
            adp_v4_update_irqs(adp);
            break;
        }
        case R_CONTROL_INT_ENABLE: {
            ADP_INFO("disp: int enable <- 0x%X", (uint32_t)data);
            qatomic_set(&adp->int_enable, (uint32_t)data);
            adp_v4_update_irqs(adp);
            break;
        }
        case R_CONTROL_UPDATE: {
            ADP_INFO("disp: update <- 0x%X", (uint32_t)data);
            if (REG_FIELD_EX32((uint32_t)data, CONTROL_UPDATE, COMMIT)) {
                if (qatomic_xchg(&adp->commit_pending, 1) != 0) {
                    qemu_log_mask(LOG_GUEST_ERROR, "disp: commit while one is still pending.\n");
                }
                qemu_bh_schedule(adp->update_disp_image_bh);
            }
            break;
        }
        default: {
            ADP_INFO("disp: unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);
            break;
        }
    }
}

static uint64_t adp_v4_reg_read(void* const opaque, hwaddr addr, unsigned size)
{
    AppleDisplayPipeV4State* adp = opaque;

    if (addr >= 0x200000) {    // ditto
        addr -= 0x200000;
    }

    if (addr >= GP_BLOCK_BASE_FOR(0) && addr < GP_BLOCK_END_FOR(0)) {
        return adp_v4_gp_reg_read(&adp->genpipe[0], addr - GP_BLOCK_BASE_FOR(0));
    }

    if (addr >= GP_BLOCK_BASE_FOR(1) && addr < GP_BLOCK_END_FOR(1)) {
        return adp_v4_gp_reg_read(&adp->genpipe[1], addr - GP_BLOCK_BASE_FOR(1));
    }

    if (addr >= BLEND_BLOCK_BASE && addr < (BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE)) {
        return adp_v4_blend_reg_read(&adp->blend_unit, addr - BLEND_BLOCK_BASE);
    }

    switch (addr >> 2) {
        case R_CONTROL_VERSION: {
            ADP_INFO("disp: version -> 0x%X", CONTROL_VERSION_A1);
            return CONTROL_VERSION_A1;
        }
        case R_CONTROL_FRAME_SIZE: {
            ADP_INFO("disp: frame size -> 0x%X", (adp->width << 16) | adp->height);
            return (adp->width << 16) | adp->height;
        }
        case R_CONTROL_SHADOW_FIFO_STATUS: {
            if (qatomic_read(&adp->commit_pending)) {
                return REG_FIELD_DP32(0, CONTROL_SHADOW_FIFO_STATUS, PENDING, 1);
            }
            return REG_FIELD_DP32(0, CONTROL_SHADOW_FIFO_STATUS, EMPTY, 1);
        }
        case R_CONTROL_INT_STATUS: {
            ADP_INFO("disp: int status -> 0x%X", qatomic_read(&adp->int_status));
            return qatomic_read(&adp->int_status);
        }
        case R_CONTROL_INT_ENABLE: {
            ADP_INFO("disp: int enable -> 0x%X", qatomic_read(&adp->int_enable));
            return qatomic_read(&adp->int_enable);
        }
        default: {
            ADP_INFO("disp: unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, (hwaddr)0);
            return 0;
        }
    }
}

static const MemoryRegionOps adp_v4_reg_ops = {
    .write                 = adp_v4_reg_write,
    .read                  = adp_v4_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void adp_v4_invalidate(void* opaque) { }

static void adp_v4_vsync_tick(void* opaque)
{
    AppleDisplayPipeV4State* adp    = opaque;
    uint64_t                 now    = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t                 period = NANOSECONDS_PER_SECOND / ADP_V4_PANEL_HZ;

    qatomic_or(&adp->int_status, R_CONTROL_INT_OUTPUT_READY_MASK);
    adp_v4_update_irqs(adp);

    adp->next_vsync_ns += period;
    if (adp->next_vsync_ns <= now) { adp->next_vsync_ns = now + period; }

    timer_mod_ns(adp->vsync_timer, adp->next_vsync_ns);
}

static void adp_v4_vsync_start(AppleDisplayPipeV4State* adp)
{
    uint64_t period = NANOSECONDS_PER_SECOND / ADP_V4_PANEL_HZ;

    adp->next_vsync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period;
    timer_mod_ns(adp->vsync_timer, adp->next_vsync_ns);
}

static void adp_v4_gfx_update(void* opaque)
{
    AppleDisplayPipeV4State* adp = opaque;
    DirtyBitmapSnapshot*     snap;
    bool                     dirty;
    uint32_t                 y, ys;

    snap =
        memory_region_snapshot_and_clear_dirty(adp->vram_mr, adp->vram_off + adp->fb_off,
                                               (hwaddr)adp->height * adp->width * sizeof(uint32_t), DIRTY_MEMORY_VGA);
    ys = -1U;
    for (y = 0; y < adp->height; ++y) {
        dirty = memory_region_snapshot_get_dirty(adp->vram_mr, snap,
                                                 adp->vram_off + adp->fb_off + adp->width * sizeof(uint32_t) * y,
                                                 (hwaddr)adp->width * sizeof(uint32_t));
        if (dirty && ys == -1U) { ys = y; }
        if (!dirty && ys != -1U) {
            dpy_gfx_update(adp->console, 0, ys, adp->width, y - ys);
            ys = -1U;
        }
    }
    if (ys != -1U) { dpy_gfx_update(adp->console, 0, ys, adp->width, y - ys); }
    g_free(snap);
}

static const GraphicHwOps adp_v4_ops = {
    .invalidate = adp_v4_invalidate,
    .gfx_update = adp_v4_gfx_update,
};

static void* adp_v4_get_fb_ptr(AppleDisplayPipeV4State* adp)
{ return memory_region_get_ram_ptr(adp->vram_mr) + adp->vram_off + adp->fb_off; }

static void adp_v4_update_disp_image_ptr(AppleDisplayPipeV4State* adp)
{
    pixman_image_t* image;

    image = pixman_image_create_bits(PIXMAN_a8r8g8b8, adp->width, adp->height, adp_v4_get_fb_ptr(adp),
                                     adp->width * sizeof(uint32_t));

    dpy_gfx_replace_surface(adp->console, qemu_create_displaysurface_pixman(image));
    qemu_pixman_image_unref(image);
}

typedef struct
{
    AppleDisplayPipeV4State* adp;
    uint32_t                 width;
    uint32_t                 height;
    pixman_transform_t       transform;
    double                   dest_width;
    int16_t                  dest_x;
    int16_t                  dest_y;
    pixman_image_t*          image;
    pixman_image_t*          disp_image;
} ADPV4DrawBootSplashContext;

static void adp_v4_draw_boot_splash(void* opaque)
{
    ADPV4DrawBootSplashContext* ctx = opaque;

    pixman_image_composite(PIXMAN_OP_SRC, ctx->image, NULL, ctx->disp_image, 0, 0, 0, 0, ctx->dest_x, ctx->dest_y,
                           ctx->dest_width, ctx->dest_width);

    dpy_gfx_update_full(ctx->adp->console);
}

static void adp_v4_draw_boot_splash_timer(void* opaque)
{
    ADPV4DrawBootSplashContext* ctx = opaque;

    adp_v4_draw_boot_splash(ctx);

    pixman_image_unref(ctx->image);
    timer_free(ctx->adp->boot_splash_timer);
    ctx->adp->boot_splash_timer = NULL;
    g_free(ctx);
}

// Please see `ui/icons/CKBrandingNotice.md`
static void adp_v4_read_and_draw_boot_splash(AppleDisplayPipeV4State* adp)
{
    char*                       path;
    FILE*                       fp;
    uint8_t                     sig[8] = {0};
    png_structp                 png_ptr;
    png_infop                   info_ptr;
    ADPV4DrawBootSplashContext* ctx;
    uint32_t*                   data;
    png_bytep*                  row_ptrs;
    uint32_t                    disp_width;
    uint32_t                    disp_height;

    path = get_relocated_path(CONFIG_QEMU_ICONDIR "/hicolor/512x512/apps/CKQEMUBootSplash@2x.png");
    assert_nonnull(path);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        error_setg(&error_abort, "Missing emulator branding: %s.", path);
        return;
    }
    fread(sig, sizeof(sig), 1, fp);
    if (png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
        error_setg(&error_abort, "Invalid emulator branding: %s.", path);
        return;
    }
    g_free(path);
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    assert_nonnull(png_ptr);
    info_ptr = png_create_info_struct(png_ptr);
    assert_nonnull(info_ptr);
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sizeof(sig));

    png_read_info(png_ptr, info_ptr);

    ctx         = g_new(ADPV4DrawBootSplashContext, 1);
    ctx->width  = png_get_image_width(png_ptr, info_ptr);
    ctx->height = png_get_image_height(png_ptr, info_ptr);
    ctx->image =
        pixman_image_create_bits(PIXMAN_a8b8g8r8, ctx->width, ctx->height, NULL, ctx->width * sizeof(uint32_t));
    data = pixman_image_get_data(ctx->image);

    png_read_update_info(png_ptr, info_ptr);

    row_ptrs = g_new(png_bytep, ctx->height);
    for (size_t y = 0; y < ctx->height; y++) { row_ptrs[y] = (png_bytep)(data + (y * ctx->width)); }

    png_read_image(png_ptr, row_ptrs);

    g_free(row_ptrs);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    disp_width  = qemu_console_get_width(adp->console, 0);
    disp_height = qemu_console_get_height(adp->console, 0);

    ctx->adp        = adp;
    ctx->dest_width = (double)disp_width / 1.5;
    ctx->dest_x     = (disp_width / 2) - (ctx->dest_width / 2);
    ctx->dest_y     = (disp_height / 2) - (ctx->dest_width / 2);
    ctx->disp_image = qemu_console_surface(adp->console)->image;

    pixman_image_set_filter(ctx->image, PIXMAN_FILTER_BEST, NULL, 0);
    pixman_transform_init_identity(&ctx->transform);
    pixman_transform_scale(&ctx->transform, NULL, pixman_double_to_fixed((double)ctx->width / ctx->dest_width),
                           pixman_double_to_fixed((double)ctx->height / ctx->dest_width));
    pixman_image_set_transform(ctx->image, &ctx->transform);

    pixman_rectangle16_t rect = {
        .x      = 0,
        .y      = 0,
        .width  = disp_width,
        .height = disp_height,
    };
    pixman_color_t color = QEMU_PIXMAN_COLOR_BLACK;

    pixman_image_fill_rectangles(PIXMAN_OP_SRC, ctx->disp_image, &color, 1, &rect);

    adp_v4_draw_boot_splash(ctx);

    adp->boot_splash_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adp_v4_draw_boot_splash_timer, ctx);

    // Workaround for `-v` removing the boot splash.
    timer_mod(adp->boot_splash_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (NANOSECONDS_PER_SECOND / 2));
}

static void adp_v4_reset_hold(Object* obj, ResetType type)
{
    AppleDisplayPipeV4State* adp = APPLE_DISPLAY_PIPE_V4(obj);

    qatomic_set(&adp->int_status, 0);
    qatomic_set(&adp->int_enable, 0);
    qatomic_set(&adp->commit_pending, 0);

    adp_v4_update_irqs(adp);

    adp_v4_update_disp_image_ptr(adp);

    adp_v4_gp_reset(&adp->genpipe[0]);
    adp_v4_gp_reset(&adp->genpipe[1]);
    adp_v4_blend_reset(&adp->blend_unit);

    adp_v4_read_and_draw_boot_splash(adp);
}

static void adp_v4_realize(DeviceState* dev, Error** errp)
{
    AppleDisplayPipeV4State* adp = APPLE_DISPLAY_PIPE_V4(dev);

    adp->console     = graphic_console_init(dev, 0, &adp_v4_ops, adp);
    adp->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adp_v4_vsync_tick, adp);

    adp_v4_vsync_start(adp);
}

static const Property adp_v4_props[] = {
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV4State, width, 0),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV4State, height, 0),
};

static void adp_v4_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.hold = adp_v4_reset_hold;

    dc->desc = "Apple Display Pipe V4";
    device_class_set_props(dc, adp_v4_props);
    dc->realize = adp_v4_realize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo adp_v4_type_info = {
    .name          = TYPE_APPLE_DISPLAY_PIPE_V4,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDisplayPipeV4State),
    .class_init    = adp_v4_class_init,
};

static void adp_v4_register_types(void) { type_register_static(&adp_v4_type_info); }

type_init(adp_v4_register_types);

// TODO: handle source/dest position, etc.
static void adp_v4_gp_draw(ADPV4GenPipe* genpipe, AddressSpace* dma_as, pixman_image_t* disp_image,
                           QemuConsole* console)
{
    pixman_format_code_t fmt;
    pixman_image_t*      image;
    pixman_transform_t   transform;

    if (REG_FIELD_EX32(genpipe->state.config_control, GP_CONFIG_CONTROL, ENABLED) == 0) { return; }

    qemu_mutex_lock(&genpipe->lock);
    adp_v4_gp_read(genpipe, dma_as);
    qemu_mutex_unlock(&genpipe->lock);

    if (genpipe->state.buf_len != 0) {
        image = genpipe->state.image;
        if (image == NULL) {
            fmt                  = adp_v4_gp_fmt_to_pixman(genpipe);
            genpipe->state.image = image =
                pixman_image_create_bits(fmt, genpipe->state.src_width, genpipe->state.src_height,
                                         (uint32_t*)genpipe->state.buf, genpipe->state.stride);
        }

        if (genpipe->state.src_width != genpipe->state.dest_width
            || genpipe->state.src_height != genpipe->state.dest_height)
        {
            pixman_transform_init_scale(
                &transform, pixman_double_to_fixed((double)genpipe->state.src_width / genpipe->state.dest_width),
                pixman_double_to_fixed((double)genpipe->state.src_height / genpipe->state.dest_height));
            pixman_image_set_filter(image, PIXMAN_FILTER_BILINEAR, NULL, 0);
            pixman_image_set_transform(image, &transform);
        }
        else {
            pixman_image_set_transform(image, NULL);
        }

        pixman_image_composite(PIXMAN_OP_SRC, image, NULL, disp_image, 0, 0, 0, 0, 0, 0, genpipe->state.dest_width,
                               genpipe->state.dest_height);

        dpy_gfx_update(console, 0, 0, genpipe->state.dest_width, genpipe->state.dest_height);
    }
}

static void adp_v4_update_disp_bh(void* opaque)
{
    AppleDisplayPipeV4State* adp = opaque;
    pixman_image_t*          disp_image;
    int                      i;

    disp_image = qemu_console_surface(adp->console)->image;

    for (i = 0; i < ADP_V4_GP_COUNT; ++i) { adp_v4_gp_draw(&adp->genpipe[i], &adp->dma_as, disp_image, adp->console); }

    qatomic_set(&adp->commit_pending, 0);

    qatomic_or(&adp->int_status, R_CONTROL_INT_FRAME_PROCESSED_MASK);
    adp_v4_update_irqs(adp);
}

// `display-timing-info`
// w_active, v_back_porch, v_front_porch, v_sync_pulse, h_active, h_back_porch,
// h_front_porch, h_sync_pulse
// FIXME: Unhardcode.
static const uint32_t adp_v4_timing_info[] = {828, 144, 1, 1, 1792, 1, 1, 1};

SysBusDevice* adp_v4_from_node(AppleDTNode* node, MemoryRegion* dma_mr)
{
    DeviceState*             dev;
    SysBusDevice*            sbd;
    AppleDisplayPipeV4State* adp;
    AppleDTProp*             prop;
    uint64_t*                reg;
    int                      i;

    assert_nonnull(node);
    assert_nonnull(dma_mr);

    dev = qdev_new(TYPE_APPLE_DISPLAY_PIPE_V4);
    sbd = SYS_BUS_DEVICE(dev);
    adp = APPLE_DISPLAY_PIPE_V4(sbd);

    adp->update_disp_image_bh = aio_bh_new(qemu_get_aio_context(), adp_v4_update_disp_bh, adp);

    apple_dt_set_prop_str(node, "display-target", "DisplayTarget5");
    apple_dt_set_prop(node, "display-timing-info", sizeof(adp_v4_timing_info), adp_v4_timing_info);
    apple_dt_set_prop_u32(node, "bics-param-set", 0xD);
    apple_dt_set_prop_u32(node, "dot-pitch", 326);
    apple_dt_set_prop_null(node, "function-brightness_update");

    adp->dma_mr = dma_mr;
    object_property_add_const_link(OBJECT(sbd), "dma_mr", OBJECT(adp->dma_mr));
    address_space_init(&adp->dma_as, adp->dma_mr, "disp0.dma");

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    memory_region_init_io(&adp->up_regs, OBJECT(sbd), &adp_v4_reg_ops, sbd, "up.regs", reg[1]);
    memory_region_enable_lockless_io(&adp->up_regs);
    sysbus_init_mmio(sbd, &adp->up_regs);
    object_property_add_const_link(OBJECT(sbd), "up.regs", OBJECT(&adp->up_regs));

    qemu_mutex_init(&adp->genpipe[0].lock);
    qemu_mutex_init(&adp->genpipe[1].lock);

    for (i = 0; i < ARRAY_SIZE(adp->irqs); i++) { sysbus_init_irq(sbd, &adp->irqs[i]); }

    return sbd;
}

void adp_v4_update_vram_mapping(AppleDisplayPipeV4State* adp, MemoryRegion* mr, hwaddr base, uint64_t size)
{
    adp->vram_mr   = mr;
    adp->vram_off  = base;
    adp->vram_size = size;
    // Put framebuffer at the end of VRAM (the start is used for GP stuff).
    adp->fb_off = adp->vram_size - ((hwaddr)adp->height * adp->width * sizeof(uint32_t));
}

uint64_t adp_v4_get_fb_off(AppleDisplayPipeV4State* adp) { return adp->fb_off; }
