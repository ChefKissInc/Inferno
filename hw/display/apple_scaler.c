/*
 * Apple M2 Scaler and Color Space Converter.
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
#include "hw/display/apple_scaler.h"
#include "hw/irq.h"
#include "hw/registerfields.h"
#include "hw/resettable.h"
#include "qemu/lockable.h"
#include "qemu/thread.h"
#include "system/dma.h"
#include <libyuv.h>

#if 0
    #define SCALER_INFO(fmt, ...) fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__)
#else
    #define SCALER_INFO(fmt, ...) \
        do { }                    \
        while (0);
#endif

// clang-format off
REG32(GLBL_VER, 0x0)
REG32(GLBL_CTRL, 0x4)
    REG_FIELD(GLBL_CTRL, RESET, 0, 1)
REG32(GLBL_STS, 0xC)
    REG_FIELD(GLBL_STS, RUNNING, 0, 1)
REG32(GLBL_IRQ_MASK, 0x1C)
REG32(GLBL_IRQSTS, 0x20)
REG32(GLBL_FRAMECNT, 0x24)
// End: 0x60

REG32(CTRL_COMMAND, 0x80)
    REG_FIELD(CTRL_COMMAND, RUN, 0, 1)
    REG_FIELD(CTRL_COMMAND, READ_ONLY, 1, 1)
REG32(CTRL_DBG, 0x9C)
REG32(CTRL_PIXEL_AVERAGING, 0xE4)
// End: 0x10C

// REG32(SRC_LUMA_BASE, 0x104)
// REG32(SRC_CHROMA_BASE, 0x108)
REG32(SRC_LUMA_PITCH, 0x10C)
REG32(SRC_CHROMA_PITCH, 0x110)
REG32(SRC_LUMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x114)
REG32(SRC_CHROMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x118)
REG32(SRC_RDMA_SIZE, 0x128)
REG32(SRC_FORMAT, 0x180)
REG32(SRC_LUMA_TEXTURE_SIZES, 0x184)
REG32(SRC_CHROMA_TEXTURE_SIZES, 0x188)
// REG32(SRC_LUMA_COMP_BASE, 0x194)
// REG32(SRC_CHROMA_COMP_BASE, 0x198)
REG32(SRC_LUMA_BASE, 0x194)
REG32(SRC_CHROMA_BASE, 0x198)
REG32(SRC_LUMA_COMP_INFO, 0x19C)
REG32(SRC_CHROMA_COMP_INFO, 0x1A0)
REG32(SRC_LUMA_COMP_HEADER_BASE, 0x1A4)
REG32(SRC_CHROMA_COMP_HEADER_BASE, 0x1A8)
REG32(SRC_LUMA_COMP_DATA_BASE, 0x1AC)
REG32(SRC_CHROMA_COMP_DATA_BASE, 0x1B0)
REG32(SRC_LUMA_STRIDE, 0x1B4)
REG32(SRC_CHROMA_STRIDE, 0x1B8)
REG32(SRC_LUMA_OFFSETS, 0x1BC)
REG32(SRC_CHROMA_OFFSETS, 0x1C0)
REG32(SRC_SWIZZLE, 0x1C4)
REG32(SRC_SIZE, 0x1C8)
// End: 0x1F0

// REG32(DST_LUMA_BASE, 0x204)
// REG32(DST_CHROMA_BASE, 0x208)
REG32(DST_LUMA_PITCH, 0x20C)
REG32(DST_CHROMA_PITCH, 0x210)
REG32(DST_LUMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x214)
REG32(DST_CHROMA_PIXEL_OFFSET_IN_TILE_ARRAY, 0x218)
REG32(DST_WDMA_SIZE, 0x228)
REG32(DST_FORMAT, 0x280)
REG32(DST_LUMA_TEXTURE_SIZES, 0x284)
REG32(DST_CHROMA_TEXTURE_SIZES, 0x288)
// REG32(DST_LUMA_COMP_BASE, 0x294)
// REG32(DST_CHROMA_COMP_BASE, 0x298)
REG32(DST_LUMA_BASE, 0x294)
REG32(DST_CHROMA_BASE, 0x298)
REG32(DST_LUMA_COMP_INFO, 0x29C)
REG32(DST_CHROMA_COMP_INFO, 0x2A0)
REG32(DST_LUMA_COMP_HEADER_BASE, 0x2A4)
REG32(DST_CHROMA_COMP_HEADER_BASE, 0x2A8)
REG32(DST_LUMA_COMP_DATA_BASE, 0x2AC)
REG32(DST_CHROMA_COMP_DATA_BASE, 0x2B0)
REG32(DST_LUMA_STRIDE, 0x2B4)
REG32(DST_CHROMA_STRIDE, 0x2B8)
REG32(DST_LUMA_OFFSETS, 0x2BC)
REG32(DST_CHROMA_OFFSETS, 0x2C0)
REG32(DST_SWIZZLE, 0x2C4)
REG32(DST_SIZE, 0x2C8)
// End: 0x2FC

/* packs two 14-bit pixel-space offsets, per M2ScalerSrcDestCfgControlMSR8::configureOffsets_gatedContext */
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, VERTICAL_LO, 0, 16)
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, HORIZONTAL, 16, 13)
REG_FIELD(SRCDST_CL_PIXEL_OFFSET_IN_TILE_ARRAY, VERTICAL_HI, 30, 2)
REG_FIELD(SRCDST_CL_OFFSET, HORIZONTAL, 0, 14)
REG_FIELD(SRCDST_CL_OFFSET, VERTICAL, 16, 14)
REG_FIELD(SRCDST_FORMAT, PACKED_RGB, 0, 1)
REG_FIELD(SRCDST_FORMAT, PREMULTIPLIED, 1, 1) /* PBGR/PBGH */
REG_FIELD(SRCDST_FORMAT, SUBSAMPLING, 2, 2) /* YUV */
REG_FIELD(SRCDST_FORMAT, GAMUT_MODE, 4, 3)
REG_FIELD(SRCDST_FORMAT, PLANE_LAYOUT, 8, 6) /* 0x06 8-bit 2-plane, 0x12 10-bit 2-plane, 0x13 wide 2-plane, ... */
REG_FIELD(SRCDST_FORMAT, CHROMA_COMPRESSED, 14, 1)
REG_FIELD(SRCDST_FORMAT, CHROMA_ADDRESSING_FORMAT, 16, 4)
REG_FIELD(SRCDST_FORMAT, SAMPLE_FORMAT, 20, 6) /* 1 = 8-bit, 2 = 10-bit, 3 = 12-bit, 5 = 16-bit */
REG_FIELD(SRCDST_FORMAT, LUMA_COMPRESSED, 26, 1)
REG_FIELD(SRCDST_FORMAT, LUMA_ADDRESSING_FORMAT, 28, 4)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_0, 0, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_1, 8, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_2, 16, 2)
REG_FIELD(SRCDST_SWIZZLE, COMPONENT_3, 24, 2)
REG_FIELD(SRCDST_SIZE, WIDTH, 0, 15)
REG_FIELD(SRCDST_SIZE, HEIGHT, 16, 15)

REG32(FLIP_ROTATE_CFG, 0x380)
    REG_FIELD(FLIP_ROTATE_CFG, ROTATE_90, 0, 1)
    REG_FIELD(FLIP_ROTATE_CFG, ROTATE_180, 1, 1)
    REG_FIELD(FLIP_ROTATE_CFG, FLIP_Y, 2, 1)
    REG_FIELD(FLIP_ROTATE_CFG, FLIP_X, 3, 1)
// End: 0x380

REG32(CSC_CFG_CHROMA_DOWNSAMPLING, 0x900)
// End: 0x900

REG32(BORDER_FILL_CFG, 0x3034)
REG32(BORDER_FILL_CFG_RED_Y, 0x3038)
REG32(BORDER_FILL_CFG_BLUE_CR, 0x303C)
REG32(BORDER_FILL_CFG_GREEN_CB, 0x3040)
REG32(BORDER_FILL_CFG_LUMA_OFFSETS, 0x3044)
    REG_FIELD(BORDER_FILL_CFG_LUMA_OFFSETS, X, 0, 15)
    REG_FIELD(BORDER_FILL_CFG_LUMA_OFFSETS, Y, 16, 15)
REG32(BORDER_FILL_CFG_CHROMA_OFFSETS, 0x3048)
    REG_FIELD(BORDER_FILL_CFG_CHROMA_OFFSETS, X, 0, 15)
    REG_FIELD(BORDER_FILL_CFG_CHROMA_OFFSETS, Y, 16, 15)
// End: 0x3048
// clang-format on

typedef enum SwizzleComponent
{
    SWIZZLE_COMPONENT_BLUE  = 0,
    SWIZZLE_COMPONENT_GREEN = 1,
    SWIZZLE_COMPONENT_RED   = 2,
    SWIZZLE_COMPONENT_ALPHA = 3,
} SwizzleComponent;

typedef enum AddressingFormat
{
    ADDRESSING_FORMAT_LINEAR            = 0,
    ADDRESSING_FORMAT_INDIRECT          = 1,
    ADDRESSING_FORMAT_TWIDDLED          = 2,
    ADDRESSING_FORMAT_TILED             = 3,
    ADDRESSING_FORMAT_REFERENCE         = 4,
    ADDRESSING_FORMAT_INTERCHANGE_TILED = 5,
} AddressingFormat;

typedef enum Subsampling
{
    SUBSAMPLING_444 = 0,
    SUBSAMPLING_422 = 1,
    SUBSAMPLING_420 = 3,
} Subsampling;

typedef enum SourceDest
{
    SOURCE,
    DEST,
    SOURCE_DEST_COUNT,
} SourceDest;

typedef enum LumaChroma
{
    CHROMA,
    LUMA,
    LUMA_CHROMA_COUNT,
} LumaChroma;

typedef struct AppleScalerSrcDstConfig
{
    uint32_t format;
    uint32_t base[LUMA_CHROMA_COUNT];
    uint32_t stride[LUMA_CHROMA_COUNT];
    uint32_t offsets[LUMA_CHROMA_COUNT];
    uint32_t swizzle;
    uint32_t size;
} AppleScalerSrcDstConfig;

struct AppleScalerState
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuMutex     lock;
    MemoryRegion  regs[2];
    MemoryRegion* dma_mr;
    AddressSpace  dma_as;
    qemu_irq      irqs[2];
    QEMUBH*       bh;

    AppleScalerSrcDstConfig srcdst[SOURCE_DEST_COUNT];
    uint32_t                flip_rotate_cfg;
    uint32_t                frame_count;
    uint32_t                irq_sts;
    bool                    running;
};

typedef enum AppleScalerFormat
{
    APPLE_SCALER_FORMAT_BGRA                 = 0x00,
    APPLE_SCALER_FORMAT_RGBA                 = 0x01,
    APPLE_SCALER_FORMAT_L565                 = 0x02,
    APPLE_SCALER_FORMAT_1555_5551            = 0x03,
    APPLE_SCALER_FORMAT_YUV_420              = 0x04, /* 420{f,v} */
    APPLE_SCALER_FORMAT_YUV_422              = 0x05, /* 422{f,v} */
    APPLE_SCALER_FORMAT_YUV_444              = 0x06, /* 444{f,v} */
    APPLE_SCALER_FORMAT_YUVS_YUVF            = 0x07,
    APPLE_SCALER_FORMAT_L10R                 = 0x08,
    APPLE_SCALER_FORMAT_PBGR                 = 0x09,
    APPLE_SCALER_FORMAT_W40A                 = 0x0A,
    APPLE_SCALER_FORMAT_PBGH                 = 0x0B,
    APPLE_SCALER_FORMAT_XF4P_X44P            = 0x0C,
    APPLE_SCALER_FORMAT_X420_XF20            = 0x0D,
    APPLE_SCALER_FORMAT_X422_XF22            = 0x0E,
    APPLE_SCALER_FORMAT_XF2P                 = 0x0F,
    APPLE_SCALER_FORMAT_X444_XF44_XW44       = 0x10,
    APPLE_SCALER_FORMAT_L008                 = 0x11,
    APPLE_SCALER_FORMAT_L010                 = 0x12,
    APPLE_SCALER_FORMAT_L012                 = 0x13,
    APPLE_SCALER_FORMAT_L016                 = 0x14,
    APPLE_SCALER_FORMAT_B3A8                 = 0x15,
    APPLE_SCALER_FORMAT_PW20_P420            = 0x16,
    APPLE_SCALER_FORMAT_PF22_P422            = 0x17,
    APPLE_SCALER_FORMAT_PF44_P444            = 0x18, /* name inferred from its 420/422 siblings */
    APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE = 0x19,
    APPLE_SCALER_FORMAT_T422_Y422_FAMILY     = 0x1A,
    APPLE_SCALER_FORMAT_Y444_FAMILY          = 0x1B,
    APPLE_SCALER_FORMAT_S4F4_S4V4            = 0x1E,
    APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V       = 0x1F,
    APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V       = 0x20,
    APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY     = 0x21,
    APPLE_SCALER_FORMAT_RGHA_FP16            = 0x28,
    APPLE_SCALER_FORMAT_COUNT,
    APPLE_SCALER_FORMAT_UNKNOWN = -1,
} AppleScalerFormat;

typedef enum
{
    SCALER_LAYOUT_NONE = 0,
    SCALER_LAYOUT_BIPLANAR_420, /* NV12: chroma at (w+1)/2 x (h+1)/2 */
    SCALER_LAYOUT_BIPLANAR_422, /* NV16: chroma at (w+1)/2 x h */
    SCALER_LAYOUT_BIPLANAR_444, /* NV24: chroma at w x h */
    SCALER_LAYOUT_PACKED32,
    SCALER_LAYOUT_PACKED_YUY2, /* 2 bytes/pixel average, 4:2:2 */
    SCALER_LAYOUT_LUMA_ONLY,
} AppleScalerLayout;

static G_GNUC_UNUSED const char* apple_scaler_stringify_format(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA                : return "BGRA";
        case APPLE_SCALER_FORMAT_RGBA                : return "RGBA";
        case APPLE_SCALER_FORMAT_L565                : return "L565";
        case APPLE_SCALER_FORMAT_1555_5551           : return "1555_5551";
        case APPLE_SCALER_FORMAT_YUV_420             : return "YUV_420";
        case APPLE_SCALER_FORMAT_YUV_422             : return "YUV_422";
        case APPLE_SCALER_FORMAT_YUV_444             : return "YUV_444";
        case APPLE_SCALER_FORMAT_YUVS_YUVF           : return "YUVS_YUVF";
        case APPLE_SCALER_FORMAT_L10R                : return "L10R";
        case APPLE_SCALER_FORMAT_PBGR                : return "PBGR";
        case APPLE_SCALER_FORMAT_W40A                : return "W40A";
        case APPLE_SCALER_FORMAT_PBGH                : return "PBGH";
        case APPLE_SCALER_FORMAT_XF4P_X44P           : return "XF4P_X44P";
        case APPLE_SCALER_FORMAT_X420_XF20           : return "X420_XF20";
        case APPLE_SCALER_FORMAT_X422_XF22           : return "X422_XF22";
        case APPLE_SCALER_FORMAT_XF2P                : return "XF2P";
        case APPLE_SCALER_FORMAT_X444_XF44_XW44      : return "X444_XF44_XW44";
        case APPLE_SCALER_FORMAT_L008                : return "L008";
        case APPLE_SCALER_FORMAT_L010                : return "L010";
        case APPLE_SCALER_FORMAT_L012                : return "L012";
        case APPLE_SCALER_FORMAT_L016                : return "L016";
        case APPLE_SCALER_FORMAT_B3A8                : return "B3A8";
        case APPLE_SCALER_FORMAT_PW20_P420           : return "PW20_P420";
        case APPLE_SCALER_FORMAT_PF22_P422           : return "PF22_P422";
        case APPLE_SCALER_FORMAT_PF44_P444           : return "PF44_P444";
        case APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE: return "PTW0_420_2PLANE_WIDE";
        case APPLE_SCALER_FORMAT_T422_Y422_FAMILY    : return "T422_Y422_FAMILY";
        case APPLE_SCALER_FORMAT_Y444_FAMILY         : return "Y444_FAMILY";
        case APPLE_SCALER_FORMAT_S4F4_S4V4           : return "S4F4_S4V4";
        case APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V      : return "PTW0_YP0F_YP0V";
        case APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V      : return "PTW2_YP2F_YP2V";
        case APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY    : return "Y4F4_TW44_FAMILY";
        case APPLE_SCALER_FORMAT_RGHA_FP16           : return "RGHA_FP16";
        default                                      : return "UNKNOWN";
    }
}

static AppleScalerLayout apple_scaler_format_layout(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA     :
        case APPLE_SCALER_FORMAT_RGBA     :
        case APPLE_SCALER_FORMAT_L10R     : return SCALER_LAYOUT_PACKED32;
        case APPLE_SCALER_FORMAT_YUV_420  : return SCALER_LAYOUT_BIPLANAR_420;
        case APPLE_SCALER_FORMAT_YUV_422  : return SCALER_LAYOUT_BIPLANAR_422;
        case APPLE_SCALER_FORMAT_YUV_444  : return SCALER_LAYOUT_BIPLANAR_444;
        case APPLE_SCALER_FORMAT_YUVS_YUVF: return SCALER_LAYOUT_PACKED_YUY2;
        case APPLE_SCALER_FORMAT_L008     :
        case APPLE_SCALER_FORMAT_L010     :
        case APPLE_SCALER_FORMAT_L012     :
        case APPLE_SCALER_FORMAT_L016     : return SCALER_LAYOUT_LUMA_ONLY;
        default                           : return SCALER_LAYOUT_NONE;
    }
}

static uint32_t apple_scaler_format_depth(AppleScalerFormat format)
{
    switch (format) {
        case APPLE_SCALER_FORMAT_L008: return 8;
        case APPLE_SCALER_FORMAT_L010: return 10;
        case APPLE_SCALER_FORMAT_L012: return 12;
        case APPLE_SCALER_FORMAT_L016: return 16;
        default                      : return 0;
    }
}

static AppleScalerFormat apple_scaler_subsampled_format(uint32_t hw_subsampling, AppleScalerFormat first)
{
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_YUV_422 != APPLE_SCALER_FORMAT_YUV_420 + 1);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_YUV_444 != APPLE_SCALER_FORMAT_YUV_420 + 2);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_X422_XF22 != APPLE_SCALER_FORMAT_X420_XF20 + 1);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_T422_Y422_FAMILY != APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE + 1);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_Y444_FAMILY != APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE + 2);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_PF22_P422 != APPLE_SCALER_FORMAT_PW20_P420 + 1);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_PF44_P444 != APPLE_SCALER_FORMAT_PW20_P420 + 2);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_PTW2_YP2F_YP2V != APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V + 1);
    QEMU_BUILD_BUG_ON(APPLE_SCALER_FORMAT_Y4F4_TW44_FAMILY != APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V + 2);

    switch (hw_subsampling) {
        case SUBSAMPLING_420: return first;
        case SUBSAMPLING_422: return first + 1;
        case SUBSAMPLING_444: return first + 2;
        default             : return APPLE_SCALER_FORMAT_UNKNOWN;
    }
}

/* NOTE: MSR8-specific */
static AppleScalerFormat apple_scaler_convert_hw_format(uint32_t hw_format, uint32_t hw_swizzle)
{
    uint32_t hw_subsampling;
    uint32_t hw_sample;
    bool     hw_planar;
    bool     hw_premult;

    if (REG_FIELD_EX32(hw_format, SRCDST_FORMAT, CHROMA_COMPRESSED)
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, LUMA_COMPRESSED)
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, CHROMA_ADDRESSING_FORMAT) != ADDRESSING_FORMAT_LINEAR
        || REG_FIELD_EX32(hw_format, SRCDST_FORMAT, LUMA_ADDRESSING_FORMAT) != ADDRESSING_FORMAT_LINEAR)
    {
        return APPLE_SCALER_FORMAT_UNKNOWN;
    }

    hw_subsampling = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, SUBSAMPLING);
    hw_premult     = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, PREMULTIPLIED);
    hw_planar      = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, PLANE_LAYOUT) != 0;
    hw_sample      = REG_FIELD_EX32(hw_format, SRCDST_FORMAT, SAMPLE_FORMAT);

    switch (hw_sample) {
        case 0x01: /* 8-bit */
            return hw_planar ? apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_YUV_420) :
                               APPLE_SCALER_FORMAT_L008;
        case 0x02: /* 10-bit */
            if (!hw_planar) { return APPLE_SCALER_FORMAT_L010; }
            return hw_subsampling == SUBSAMPLING_444 ?
                       APPLE_SCALER_FORMAT_X444_XF44_XW44 :
                       apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_X420_XF20);
        case 0x03: /* 12-bit */
            return hw_planar ?
                       apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PTW0_420_2PLANE_WIDE) :
                       APPLE_SCALER_FORMAT_L012;
        case 0x05: return hw_planar ? APPLE_SCALER_FORMAT_UNKNOWN : APPLE_SCALER_FORMAT_L016; /* 16-bit */
        case 0x08: return APPLE_SCALER_FORMAT_L565;
        case 0x0B: return APPLE_SCALER_FORMAT_1555_5551;
        case 0x10: return apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PW20_P420);
        case 0x18: return hw_planar ? APPLE_SCALER_FORMAT_B3A8 : APPLE_SCALER_FORMAT_XF4P_X44P;
        case 0x19: return APPLE_SCALER_FORMAT_L10R;
        case 0x1E:
            if (hw_subsampling == SUBSAMPLING_422) { return APPLE_SCALER_FORMAT_YUVS_YUVF; }
            if (hw_premult) { return APPLE_SCALER_FORMAT_PBGR; }
            /*
             * M2ScalerCSCColorConversionControlMSR8::getSwizzleRegisterFields_gatedContext
             * There is also GRBA, but it has no libyuv equivalent.
             */
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_1) != SWIZZLE_COMPONENT_GREEN
                || REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_3) != SWIZZLE_COMPONENT_ALPHA)
            {
                return APPLE_SCALER_FORMAT_UNKNOWN;
            }
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_0) == SWIZZLE_COMPONENT_BLUE
                && REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_2) == SWIZZLE_COMPONENT_RED)
            {
                return APPLE_SCALER_FORMAT_BGRA;
            }
            if (REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_0) == SWIZZLE_COMPONENT_RED
                && REG_FIELD_EX32(hw_swizzle, SRCDST_SWIZZLE, COMPONENT_2) == SWIZZLE_COMPONENT_BLUE)
            {
                return APPLE_SCALER_FORMAT_RGBA;
            }
            return APPLE_SCALER_FORMAT_UNKNOWN;
        case 0x20: return apple_scaler_subsampled_format(hw_subsampling, APPLE_SCALER_FORMAT_PTW0_YP0F_YP0V);
        case 0x23:
            if (hw_subsampling == SUBSAMPLING_422) { return APPLE_SCALER_FORMAT_XF2P; }
            return hw_premult ? APPLE_SCALER_FORMAT_PBGH : APPLE_SCALER_FORMAT_W40A;
        case 0x24: return APPLE_SCALER_FORMAT_RGHA_FP16;
        default  : return APPLE_SCALER_FORMAT_UNKNOWN;
    }
}

static bool apple_scaler_layout_is_biplanar(AppleScalerLayout layout)
{ return layout >= SCALER_LAYOUT_BIPLANAR_420 && layout <= SCALER_LAYOUT_BIPLANAR_444; }

static void apple_scaler_chroma_dims(AppleScalerLayout layout, uint32_t width, uint32_t height, uint32_t* chroma_width,
                                     uint32_t* chroma_height)
{
    *chroma_width  = layout == SCALER_LAYOUT_BIPLANAR_444 ? width : (width + 1) / 2;
    *chroma_height = layout == SCALER_LAYOUT_BIPLANAR_420 ? (height + 1) / 2 : height;
}

static uint32_t apple_scaler_luma_bpp(AppleScalerLayout layout, uint32_t depth)
{
    switch (layout) {
        case SCALER_LAYOUT_PACKED32   : return 4;
        case SCALER_LAYOUT_PACKED_YUY2: return 2;
        case SCALER_LAYOUT_LUMA_ONLY  : return depth > 8 ? 2 : 1;
        default                       : return 1;
    }
}

static bool apple_scaler_layout_is_luma(AppleScalerLayout layout) { return layout == SCALER_LAYOUT_LUMA_ONLY; }

static int apple_scaler_depth_scale(uint32_t depth) { return 65536 >> (depth - 8); }

static dma_addr_t apple_scaler_offset_bytes(uint32_t offsets_reg, uint32_t stride, uint32_t bytes_per_sample)
{
    dma_addr_t h = REG_FIELD_EX32(offsets_reg, SRCDST_CL_OFFSET, HORIZONTAL);
    dma_addr_t v = REG_FIELD_EX32(offsets_reg, SRCDST_CL_OFFSET, VERTICAL);
    return v * stride + h * bytes_per_sample;
}

static void apple_scaler_dma_rows(AddressSpace* as, dma_addr_t base, uint32_t stride, uint32_t row_bytes,
                                  uint32_t height, void* buf, bool write)
{
    uint8_t* rows = buf;

    if (row_bytes == 0 || height == 0) { return; }

    if (stride == row_bytes) {
        size_t total = (size_t)row_bytes * height;
        if (write) { dma_memory_write(as, base, rows, total, MEMTXATTRS_UNSPECIFIED); }
        else {
            dma_memory_read(as, base, rows, total, MEMTXATTRS_UNSPECIFIED);
        }
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        dma_addr_t addr = base + (dma_addr_t)y * stride;
        uint8_t*   row  = rows + (size_t)y * row_bytes;
        if (write) { dma_memory_write(as, addr, row, row_bytes, MEMTXATTRS_UNSPECIFIED); }
        else {
            dma_memory_read(as, addr, row, row_bytes, MEMTXATTRS_UNSPECIFIED);
        }
    }
}

typedef enum
{
    IMAGE_ARGB,
    IMAGE_I444,
    IMAGE_I422,
    IMAGE_I420,
    IMAGE_I400,
} AppleScalerImageKind;

typedef struct
{
    uint8_t*             plane[3];
    int                  stride[3];
    uint32_t             width;
    uint32_t             height;
    AppleScalerImageKind kind;
} AppleScalerImage;

static AppleScalerImageKind apple_scaler_layout_kind(AppleScalerLayout layout)
{
    switch (layout) {
        case SCALER_LAYOUT_BIPLANAR_444: return IMAGE_I444;
        case SCALER_LAYOUT_BIPLANAR_422: return IMAGE_I422;
        case SCALER_LAYOUT_PACKED_YUY2 : return IMAGE_I422; /* YUY2 is packed 4:2:2 */
        case SCALER_LAYOUT_PACKED32    : return IMAGE_ARGB;
        case SCALER_LAYOUT_LUMA_ONLY   : return IMAGE_I400;
        default                        : return IMAGE_I420;
    }
}

static bool apple_scaler_kind_has_chroma(AppleScalerImageKind kind)
{ return kind == IMAGE_I444 || kind == IMAGE_I422 || kind == IMAGE_I420; }

static void apple_scaler_kind_chroma_dims(AppleScalerImageKind kind, uint32_t width, uint32_t height, uint32_t* cw,
                                          uint32_t* ch)
{
    *cw = kind == IMAGE_I444 ? width : (width + 1) / 2;
    *ch = kind == IMAGE_I420 ? (height + 1) / 2 : height;
}

static void apple_scaler_image_init(AppleScalerImage* image, AppleScalerImageKind kind, uint32_t width, uint32_t height)
{
    uint32_t cw, ch;
    size_t   luma_size;
    size_t   chroma_size;

    *image        = (AppleScalerImage){0};
    image->kind   = kind;
    image->width  = width;
    image->height = height;

    if (kind == IMAGE_ARGB) {
        image->stride[0] = (int)width * 4;
        image->plane[0]  = g_malloc((size_t)image->stride[0] * height);
        return;
    }

    image->stride[0] = (int)width;
    luma_size        = (size_t)width * height;

    if (!apple_scaler_kind_has_chroma(kind)) {
        image->plane[0] = g_malloc(luma_size);
        return;
    }

    apple_scaler_kind_chroma_dims(kind, width, height, &cw, &ch);
    image->stride[1] = image->stride[2] = (int)cw;
    chroma_size                         = (size_t)cw * ch;
    image->plane[0]                     = g_malloc(luma_size + 2 * chroma_size);
    image->plane[1]                     = image->plane[0] + luma_size;
    image->plane[2]                     = image->plane[1] + chroma_size;
}

static void apple_scaler_image_destroy(AppleScalerImage* image) { g_free(image->plane[0]); }

typedef int (*AppleScalerPlanarRotate)(const uint8_t*, int, const uint8_t*, int, const uint8_t*, int, uint8_t*, int,
                                       uint8_t*, int, uint8_t*, int, int, int, enum RotationMode);
typedef int (*AppleScalerPlanarScale)(const uint8_t*, int, const uint8_t*, int, const uint8_t*, int, int, int, uint8_t*,
                                      int, uint8_t*, int, uint8_t*, int, int, int, enum FilterMode);

static const AppleScalerPlanarRotate apple_scaler_planar_rotate[] = {
    [IMAGE_I444] = I444Rotate,
    [IMAGE_I422] = I422Rotate,
    [IMAGE_I420] = I420Rotate,
};

static const AppleScalerPlanarScale apple_scaler_planar_scale[] = {
    [IMAGE_I444] = I444Scale,
    [IMAGE_I422] = I422Scale,
    [IMAGE_I420] = I420Scale,
};

static void apple_scaler_image_rotate(AppleScalerImage* dst, const AppleScalerImage* src, enum RotationMode rotation)
{
    bool swap = rotation == kRotate90 || rotation == kRotate270;

    apple_scaler_image_init(dst, src->kind, swap ? src->height : src->width, swap ? src->width : src->height);

    switch (src->kind) {
        case IMAGE_ARGB:
            ARGBRotate(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                       rotation);
            return;
        case IMAGE_I400:
            RotatePlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height,
                        rotation);
            return;
        default:
            apple_scaler_planar_rotate[src->kind](src->plane[0], src->stride[0], src->plane[1], src->stride[1],
                                                  src->plane[2], src->stride[2], dst->plane[0], dst->stride[0],
                                                  dst->plane[1], dst->stride[1], dst->plane[2], dst->stride[2],
                                                  (int)src->width, (int)src->height, rotation);
            return;
    }
}

static void apple_scaler_image_scale(AppleScalerImage* dst, const AppleScalerImage* src, uint32_t width,
                                     uint32_t height)
{
    apple_scaler_image_init(dst, src->kind, width, height);

    switch (src->kind) {
        case IMAGE_ARGB:
            ARGBScale(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                      (int)width, (int)height, kFilterBox);
            return;
        case IMAGE_I400:
            ScalePlane(src->plane[0], src->stride[0], (int)src->width, (int)src->height, dst->plane[0], dst->stride[0],
                       (int)width, (int)height, kFilterBox);
            return;
        default:
            apple_scaler_planar_scale[src->kind](src->plane[0], src->stride[0], src->plane[1], src->stride[1],
                                                 src->plane[2], src->stride[2], (int)src->width, (int)src->height,
                                                 dst->plane[0], dst->stride[0], dst->plane[1], dst->stride[1],
                                                 dst->plane[2], dst->stride[2], (int)width, (int)height, kFilterBox);
            return;
    }
}

static void apple_scaler_image_mirror(AppleScalerImage* dst, const AppleScalerImage* src, bool flip_x)
{
    uint32_t cw, ch;
    int      i;

    apple_scaler_image_init(dst, src->kind, src->width, src->height);

    if (src->kind == IMAGE_ARGB) {
        if (flip_x) {
            ARGBMirror(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);
        }
        else {
            ARGBCopy(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, -(int)src->height);
        }
        return;
    }

    apple_scaler_kind_chroma_dims(src->kind, src->width, src->height, &cw, &ch);

    for (i = 0; i < (apple_scaler_kind_has_chroma(src->kind) ? 3 : 1); i++) {
        int w = i == 0 ? (int)src->width : (int)cw;
        int h = i == 0 ? (int)src->height : (int)ch;

        if (flip_x) { MirrorPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, h); }
        else {
            CopyPlane(src->plane[i], src->stride[i], dst->plane[i], dst->stride[i], w, -h);
        }
    }
}

static void apple_scaler_image_from_i400(AppleScalerImage* dst, const AppleScalerImage* src)
{
    uint32_t cw, ch;

    CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], (int)src->width, (int)src->height);

    if (!apple_scaler_kind_has_chroma(dst->kind)) { return; }

    apple_scaler_kind_chroma_dims(dst->kind, dst->width, dst->height, &cw, &ch);
    SetPlane(dst->plane[1], dst->stride[1], (int)cw, (int)ch, 0x80);
    SetPlane(dst->plane[2], dst->stride[2], (int)cw, (int)ch, 0x80);
}

#define PLANAR3(i) (i)->plane[0], (i)->stride[0], (i)->plane[1], (i)->stride[1], (i)->plane[2], (i)->stride[2]

static int apple_scaler_image_convert(AppleScalerImage* dst, const AppleScalerImage* src, AppleScalerImageKind kind)
{
    int w = (int)src->width;
    int h = (int)src->height;

    apple_scaler_image_init(dst, kind, src->width, src->height);

    if (src->kind == IMAGE_I400) {
        if (kind == IMAGE_ARGB) {
            return I400ToARGB(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        apple_scaler_image_from_i400(dst, src);
        return 0;
    }

    if (kind == IMAGE_I400) {
        if (src->kind == IMAGE_ARGB) {
            return ARGBToI400(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        }
        CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
        return 0;
    }

    if (src->kind == IMAGE_ARGB) {
        switch (kind) {
            case IMAGE_I444: return ARGBToI444(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            case IMAGE_I422: return ARGBToI422(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
            default        : return ARGBToI420(src->plane[0], src->stride[0], PLANAR3(dst), w, h);
        }
    }

    if (kind == IMAGE_ARGB) {
        switch (src->kind) {
            case IMAGE_I444: return I444ToARGB(PLANAR3(src), dst->plane[0], dst->stride[0], w, h);
            case IMAGE_I422: return I422ToARGB(PLANAR3(src), dst->plane[0], dst->stride[0], w, h);
            default        : return I420ToARGB(PLANAR3(src), dst->plane[0], dst->stride[0], w, h);
        }
    }

    switch (src->kind) {
        case IMAGE_I444:
            if (kind == IMAGE_I420) { return I444ToI420(PLANAR3(src), PLANAR3(dst), w, h); }
            /* No I444ToI422 in libyuv: halve the chroma planes horizontally, full height. */
            CopyPlane(src->plane[0], src->stride[0], dst->plane[0], dst->stride[0], w, h);
            ScalePlane(src->plane[1], src->stride[1], w, h, dst->plane[1], dst->stride[1], dst->stride[1], h,
                       kFilterBilinear);
            return ScalePlane(src->plane[2], src->stride[2], w, h, dst->plane[2], dst->stride[2], dst->stride[2], h,
                              kFilterBilinear);
        case IMAGE_I422:
            return kind == IMAGE_I444 ? I422ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                        I422ToI420(PLANAR3(src), PLANAR3(dst), w, h);
        default:
            return kind == IMAGE_I444 ? I420ToI444(PLANAR3(src), PLANAR3(dst), w, h) :
                                        I420ToI422(PLANAR3(src), PLANAR3(dst), w, h);
    }
}

static void apple_scaler_chroma_split(const AppleScalerImage* image, const uint8_t* plane1, int stride1)
{
    uint32_t cw, ch;

    apple_scaler_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
    SplitUVPlane(plane1, stride1, image->plane[1], image->stride[1], image->plane[2], image->stride[2], (int)cw,
                 (int)ch);
}

static void apple_scaler_chroma_merge(const AppleScalerImage* image, uint8_t* plane1, int stride1)
{
    uint32_t cw, ch;

    apple_scaler_kind_chroma_dims(image->kind, image->width, image->height, &cw, &ch);
    MergeUVPlane(image->plane[1], image->stride[1], image->plane[2], image->stride[2], plane1, stride1, (int)cw,
                 (int)ch);
}

static int apple_scaler_packed_to_image(AppleScalerFormat format, const uint8_t* src, int stride,
                                        const AppleScalerImage* dst)
{
    int w = (int)dst->width;
    int h = (int)dst->height;

    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA     : return ARGBCopy(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SCALER_FORMAT_RGBA     : return ABGRToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SCALER_FORMAT_L10R     : return AR30ToARGB(src, stride, dst->plane[0], dst->stride[0], w, h);
        case APPLE_SCALER_FORMAT_YUVS_YUVF: return YUY2ToI422(src, stride, PLANAR3(dst), w, h);
        default                           : return -1; /* extend alongside the format table */
    }
}

static int apple_scaler_packed_from_image(AppleScalerFormat format, const AppleScalerImage* src, uint8_t* dst,
                                          int stride)
{
    int w = (int)src->width;
    int h = (int)src->height;

    switch (format) {
        case APPLE_SCALER_FORMAT_BGRA     : return ARGBCopy(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SCALER_FORMAT_RGBA     : return ARGBToABGR(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SCALER_FORMAT_L10R     : return ARGBToAR30(src->plane[0], src->stride[0], dst, stride, w, h);
        case APPLE_SCALER_FORMAT_YUVS_YUVF: return I422ToYUY2(PLANAR3(src), dst, stride, w, h);
        default                           : return -1; /* extend alongside the format table */
    }
}

typedef struct
{
    AppleScalerFormat format;
    AppleScalerLayout layout;
    uint32_t          depth;
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride[LUMA_CHROMA_COUNT];
    dma_addr_t        base[LUMA_CHROMA_COUNT];
} AppleScalerSurface;

static bool apple_scaler_surface_init(AppleScalerSurface* surface, AppleScalerState* scaler, SourceDest srcdst)
{
    const AppleScalerSrcDstConfig* cfg = &scaler->srcdst[srcdst];

    surface->format         = apple_scaler_convert_hw_format(cfg->format, cfg->swizzle);
    surface->layout         = apple_scaler_format_layout(surface->format);
    surface->depth          = apple_scaler_format_depth(surface->format);
    surface->width          = REG_FIELD_EX32(cfg->size, SRCDST_SIZE, WIDTH);
    surface->height         = REG_FIELD_EX32(cfg->size, SRCDST_SIZE, HEIGHT);
    surface->stride[LUMA]   = cfg->stride[LUMA];
    surface->stride[CHROMA] = cfg->stride[CHROMA];

    SCALER_INFO("%s w/h %ux%u stride 0x%X 0x%X format %s @ cl 0x%X 0x%X offsets %d %d",
                srcdst == SOURCE ? "src" : "dst", surface->width, surface->height, cfg->stride[CHROMA],
                cfg->stride[LUMA], apple_scaler_stringify_format(surface->format), cfg->base[CHROMA], cfg->base[LUMA],
                cfg->offsets[CHROMA], cfg->offsets[LUMA]);

    if (surface->layout == SCALER_LAYOUT_NONE || surface->width == 0 || surface->height == 0) { return false; }

    surface->base[LUMA]   = cfg->base[LUMA]
                            + apple_scaler_offset_bytes(cfg->offsets[LUMA], cfg->stride[LUMA],
                                                        apple_scaler_luma_bpp(surface->layout, surface->depth));
    surface->base[CHROMA] = cfg->base[CHROMA] + apple_scaler_offset_bytes(cfg->offsets[CHROMA], cfg->stride[CHROMA], 2);
    return true;
}

static bool apple_scaler_image_load(AppleScalerImage* image, AddressSpace* as, const AppleScalerSurface* surface)
{
    AppleScalerLayout layout    = surface->layout;
    uint32_t          row_bytes = surface->width * apple_scaler_luma_bpp(surface->layout, surface->depth);
    uint8_t*          staging;
    int               ret;

    apple_scaler_image_init(image, apple_scaler_layout_kind(layout), surface->width, surface->height);

    if (apple_scaler_layout_is_biplanar(layout)) {
        uint32_t cw, ch;
        uint8_t* plane1;

        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, surface->height,
                              image->plane[0], false);

        apple_scaler_chroma_dims(layout, surface->width, surface->height, &cw, &ch);
        plane1 = g_malloc((size_t)cw * 2 * ch);
        apple_scaler_dma_rows(as, surface->base[CHROMA], surface->stride[CHROMA], cw * 2, ch, plane1, false);
        apple_scaler_chroma_split(image, plane1, (int)(cw * 2));
        g_free(plane1);
        return true;
    }

    if (apple_scaler_layout_is_luma(layout)) {
        if (surface->depth == 8) {
            apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, surface->height,
                                  image->plane[0], false);
            return true;
        }

        staging = g_malloc((size_t)row_bytes * surface->height);
        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, surface->height, staging,
                              false);
        Convert16To8Plane((const uint16_t*)staging, (int)(row_bytes / 2), image->plane[0], image->stride[0],
                          apple_scaler_depth_scale(surface->depth), (int)image->width, (int)image->height);
        g_free(staging);
        return true;
    }

    if (surface->format == APPLE_SCALER_FORMAT_BGRA) {
        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, surface->height,
                              image->plane[0], false);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * surface->height);
    apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, surface->height, staging, false);
    ret = apple_scaler_packed_to_image(surface->format, staging, (int)row_bytes, image);
    g_free(staging);

    if (ret) {
        SCALER_INFO("conversion from %s failed: %d", apple_scaler_stringify_format(surface->format), ret);
        apple_scaler_image_destroy(image);
        return false;
    }
    return true;
}

static bool apple_scaler_image_store(const AppleScalerImage* image, AddressSpace* as, const AppleScalerSurface* surface)
{
    AppleScalerLayout layout    = surface->layout;
    uint32_t          row_bytes = image->width * apple_scaler_luma_bpp(surface->layout, surface->depth);
    uint8_t*          staging;
    int               ret;

    if (apple_scaler_layout_is_biplanar(layout)) {
        uint32_t cw, ch;
        uint8_t* plane1;

        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, image->height, image->plane[0],
                              true);

        apple_scaler_chroma_dims(layout, image->width, image->height, &cw, &ch);
        plane1 = g_malloc((size_t)cw * 2 * ch);
        apple_scaler_chroma_merge(image, plane1, (int)(cw * 2));
        apple_scaler_dma_rows(as, surface->base[CHROMA], surface->stride[CHROMA], cw * 2, ch, plane1, true);
        g_free(plane1);
        return true;
    }

    if (apple_scaler_layout_is_luma(layout)) {
        if (surface->depth == 8) {
            apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, image->height,
                                  image->plane[0], true);
            return true;
        }

        staging = g_malloc((size_t)row_bytes * image->height);
        Convert8To16Plane(image->plane[0], image->stride[0], (uint16_t*)staging, (int)(row_bytes / 2),
                          (int)surface->depth, (int)image->width, (int)image->height);
        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, image->height, staging, true);
        g_free(staging);
        return true;
    }

    if (surface->format == APPLE_SCALER_FORMAT_BGRA) {
        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, image->height, image->plane[0],
                              true);
        return true;
    }

    staging = g_malloc((size_t)row_bytes * image->height);
    ret     = apple_scaler_packed_from_image(surface->format, image, staging, (int)row_bytes);
    if (ret == 0) {
        apple_scaler_dma_rows(as, surface->base[LUMA], surface->stride[LUMA], row_bytes, image->height, staging, true);
    }
    else {
        SCALER_INFO("conversion to %s failed: %d", apple_scaler_stringify_format(surface->format), ret);
    }
    g_free(staging);
    return ret == 0;
}

#if 0
static void apple_scaler_export_file(SourceDest srcdst, uint32_t width, uint32_t height, AppleScalerFormat format,
                                     LumaChroma lc, const void* contents, dma_addr_t size)
{
    char fn[128];
    snprintf(fn, sizeof(fn), "/Users/visual/Downloads/Scaler/%s_%lld_%c_%ux%u_%s", srcdst == SOURCE ? "src" : "dst",
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), lc == CHROMA ? 'c' : 'l', width, height,
             apple_scaler_stringify_format(format));
    g_file_set_contents(fn, contents, size, NULL);
}

static void apple_scaler_export_source(AppleScalerState* scaler, const AppleScalerSurface* surface)
{
    const AppleScalerSrcDstConfig* cfg = &scaler->srcdst[SOURCE];
    LumaChroma                     lc;

    for (lc = CHROMA; lc < LUMA_CHROMA_COUNT; lc++) {
        dma_addr_t size = (dma_addr_t)cfg->stride[lc] * (lc == LUMA ? surface->height : surface->height / 2);
        void*      buf;

        if (size == 0) { continue; }

        buf = g_malloc(size);
        if (dma_memory_read(&scaler->dma_as, cfg->base[lc], buf, size, MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            apple_scaler_export_file(SOURCE, surface->width, surface->height, surface->format, lc, buf, size);
        }
        g_free(buf);
    }
}
#else
    #define apple_scaler_export_source(scaler, surface) (void)0
#endif

static void apple_scaler_update_irqs(AppleScalerState* scaler)
{ qemu_set_irq(scaler->irqs[0], qatomic_read(&scaler->irq_sts) != 0); }

static void apple_scaler_signal_frame_done(AppleScalerState* scaler)
{
    qatomic_fetch_inc(&scaler->frame_count);
    /*
     * AppleM2ScalerCSCHalMSR8::scalerErrorInterruptStatus(): &0x3e0 compression/decompression, &0x1e018 read/write,
     * &0x1000 AXI, and &0x2 MSR_CTRL_DBGSTS (0x9C).
     */
    qatomic_set(&scaler->irq_sts, 1);
    qatomic_set(&scaler->running, false);
    apple_scaler_update_irqs(scaler);
}

typedef struct
{
    enum RotationMode rotation;
    bool              flip_x;
    bool              flip_y;
} AppleScalerTransform;

static AppleScalerTransform apple_scaler_transform(const AppleScalerState* scaler)
{
    uint32_t             cfg        = scaler->flip_rotate_cfg;
    bool                 rotate_90  = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, ROTATE_90);
    bool                 rotate_180 = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, ROTATE_180);
    AppleScalerTransform transform;

    transform.flip_x = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, FLIP_X);
    transform.flip_y = REG_FIELD_EX32(cfg, FLIP_ROTATE_CFG, FLIP_Y);

    if (transform.flip_x && transform.flip_y) {
        transform.flip_x = transform.flip_y = false;
        rotate_180                          = !rotate_180;
    }

    transform.rotation = rotate_90 ? (rotate_180 ? kRotate270 : kRotate90) : (rotate_180 ? kRotate180 : kRotate0);
    return transform;
}

static bool apple_scaler_transform_is_identity(const AppleScalerTransform* transform)
{ return transform->rotation == kRotate0 && !transform->flip_x && !transform->flip_y; }

static void apple_scaler_blit(AddressSpace* as, const AppleScalerSurface* src, const AppleScalerSurface* dst)
{
    uint32_t row_bytes = src->width * apple_scaler_luma_bpp(src->layout, src->depth);
    uint8_t* buf;

    buf = g_malloc((size_t)row_bytes * src->height);
    apple_scaler_dma_rows(as, src->base[LUMA], src->stride[LUMA], row_bytes, src->height, buf, false);
    apple_scaler_dma_rows(as, dst->base[LUMA], dst->stride[LUMA], row_bytes, dst->height, buf, true);
    g_free(buf);

    if (apple_scaler_layout_is_biplanar(src->layout)) {
        uint32_t cw, ch;

        apple_scaler_chroma_dims(src->layout, src->width, src->height, &cw, &ch);
        buf = g_malloc((size_t)cw * 2 * ch);
        apple_scaler_dma_rows(as, src->base[CHROMA], src->stride[CHROMA], cw * 2, ch, buf, false);
        apple_scaler_dma_rows(as, dst->base[CHROMA], dst->stride[CHROMA], cw * 2, ch, buf, true);
        g_free(buf);
    }
}

static void apple_scaler_process(AppleScalerState* scaler, const AppleScalerSurface* src, const AppleScalerSurface* dst)
{
    AppleScalerTransform transform = apple_scaler_transform(scaler);
    AppleScalerImageKind dst_kind  = apple_scaler_layout_kind(dst->layout);
    AppleScalerImage     image, tmp;
    int                  ret;

    if (apple_scaler_transform_is_identity(&transform) && src->format == dst->format && src->width == dst->width
        && src->height == dst->height)
    {
        apple_scaler_blit(&scaler->dma_as, src, dst);
        return;
    }

    if (!apple_scaler_image_load(&image, &scaler->dma_as, src)) { return; }

    if (transform.rotation != kRotate0) {
        apple_scaler_image_rotate(&tmp, &image, transform.rotation);
        apple_scaler_image_destroy(&image);
        image = tmp;
    }

    if (transform.flip_x || transform.flip_y) {
        apple_scaler_image_mirror(&tmp, &image, transform.flip_x);
        apple_scaler_image_destroy(&image);
        image = tmp;
    }

    if (image.width != dst->width || image.height != dst->height) {
        apple_scaler_image_scale(&tmp, &image, dst->width, dst->height);
        apple_scaler_image_destroy(&image);
        image = tmp;
    }

    if (image.kind != dst_kind) {
        ret = apple_scaler_image_convert(&tmp, &image, dst_kind);
        apple_scaler_image_destroy(&image);
        if (ret) {
            SCALER_INFO("conversion to the destination layout failed: %d", ret);
            apple_scaler_image_destroy(&tmp);
            return;
        }
        image = tmp;
    }

    apple_scaler_image_store(&image, &scaler->dma_as, dst);
    apple_scaler_image_destroy(&image);
}

static void apple_scaler_bh(void* opaque)
{
    AppleScalerState*  scaler = opaque;
    AppleScalerSurface src, dst;
    bool               src_ok, dst_ok;

    QEMU_LOCK_GUARD(&scaler->lock);

    src_ok = apple_scaler_surface_init(&src, scaler, SOURCE);
    dst_ok = apple_scaler_surface_init(&dst, scaler, DEST);

    if (src_ok && dst_ok) { apple_scaler_process(scaler, &src, &dst); }
    else {
        SCALER_INFO("unsupported format pair %s -> %s", apple_scaler_stringify_format(src.format),
                    apple_scaler_stringify_format(dst.format));
        apple_scaler_export_source(scaler, &src);
    }

    apple_scaler_signal_frame_done(scaler);
}

static uint32_t* apple_scaler_reg_ptr(AppleScalerState* scaler, hwaddr index)
{
    switch (index) {
        case R_SRC_FORMAT        : return &scaler->srcdst[SOURCE].format;
        case R_SRC_LUMA_BASE     : return &scaler->srcdst[SOURCE].base[LUMA];
        case R_SRC_CHROMA_BASE   : return &scaler->srcdst[SOURCE].base[CHROMA];
        case R_SRC_LUMA_STRIDE   : return &scaler->srcdst[SOURCE].stride[LUMA];
        case R_SRC_CHROMA_STRIDE : return &scaler->srcdst[SOURCE].stride[CHROMA];
        case R_SRC_SWIZZLE       : return &scaler->srcdst[SOURCE].swizzle;
        case R_SRC_SIZE          : return &scaler->srcdst[SOURCE].size;
        case R_SRC_CHROMA_OFFSETS: return &scaler->srcdst[SOURCE].offsets[CHROMA];
        case R_SRC_LUMA_OFFSETS  : return &scaler->srcdst[SOURCE].offsets[LUMA];
        case R_DST_FORMAT        : return &scaler->srcdst[DEST].format;
        case R_DST_LUMA_BASE     : return &scaler->srcdst[DEST].base[LUMA];
        case R_DST_CHROMA_BASE   : return &scaler->srcdst[DEST].base[CHROMA];
        case R_DST_LUMA_STRIDE   : return &scaler->srcdst[DEST].stride[LUMA];
        case R_DST_CHROMA_STRIDE : return &scaler->srcdst[DEST].stride[CHROMA];
        case R_DST_CHROMA_OFFSETS: return &scaler->srcdst[DEST].offsets[CHROMA];
        case R_DST_LUMA_OFFSETS  : return &scaler->srcdst[DEST].offsets[LUMA];
        case R_DST_SWIZZLE       : return &scaler->srcdst[DEST].swizzle;
        case R_DST_SIZE          : return &scaler->srcdst[DEST].size;
        case R_FLIP_ROTATE_CFG   : return &scaler->flip_rotate_cfg;
        default                  : return NULL;
    }
}

static void apple_scaler_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleScalerState* scaler = opaque;
    uint32_t*         reg;

    // SCALER_INFO("0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);

    addr >>= 2;

    reg = apple_scaler_reg_ptr(scaler, addr);
    if (reg != NULL) {
        if (!qatomic_read(&scaler->running)) { *reg = (uint32_t)data; }
        return;
    }

    switch (addr) {
        case R_GLBL_IRQSTS:
            qatomic_and(&scaler->irq_sts, ~(uint32_t)data);
            qemu_mutex_lock(&scaler->lock);
            apple_scaler_update_irqs(scaler);
            qemu_mutex_unlock(&scaler->lock);
            break;
        case R_GLBL_CTRL:
            if (REG_FIELD_EX32(data, GLBL_CTRL, RESET)) {
                BQL_LOCK_GUARD();
                resettable_reset(OBJECT(scaler), RESET_TYPE_COLD);
            }
            break;
        case R_CTRL_COMMAND:
            if (REG_FIELD_EX32(data, CTRL_COMMAND, RUN) && !qatomic_cmpxchg(&scaler->running, false, true)) {
                qemu_bh_schedule(scaler->bh);
            }
            break;
        default: break;
    }
}

static uint64_t apple_scaler_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleScalerState* scaler = opaque;
    const uint32_t*   reg;
    uint32_t          ret;

    addr >>= 2;

    reg = apple_scaler_reg_ptr(scaler, addr);
    if (reg != NULL) { return *reg; }

    switch (addr) {
        case R_GLBL_VER:
            // SOC 0x8 -> 0x90082/0x9009B/0x900A7
            ret = 0x9009B;
            break;
        case R_GLBL_STS     : ret = qatomic_read(&scaler->running) ? R_GLBL_STS_RUNNING_MASK : 0; break;
        case R_GLBL_IRQSTS  : ret = qatomic_read(&scaler->irq_sts); break;
        case R_GLBL_FRAMECNT: ret = qatomic_read(&scaler->frame_count); break;
        default             : ret = 0; break;
    }

    // SCALER_INFO("0x" HWADDR_FMT_plx " -> 0x%X", addr << 2, ret);

    return ret;
}

static const MemoryRegionOps apple_scaler_reg_ops = {
    .write                 = apple_scaler_reg_write,
    .read                  = apple_scaler_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_scaler_unk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    // AppleScalerState *scaler = opaque;

    // SCALER_INFO("0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx, addr, data);

    switch (addr) {
        default: {
            break;
        }
    }
}

static uint64_t apple_scaler_unk_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    // AppleScalerState *scaler = opaque;
    uint64_t ret;

    switch (addr >> 2) {
        default: ret = 0; break;
    }

    // SCALER_INFO("0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx, addr, ret);

    return ret;
}

static const MemoryRegionOps apple_scaler_unk_reg_ops = {
    .write                 = apple_scaler_unk_reg_write,
    .read                  = apple_scaler_unk_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_scaler_reset_enter(Object* obj, ResetType type)
{
    AppleScalerState* scaler = APPLE_SCALER(obj);

    QEMU_LOCK_GUARD(&scaler->lock);

    qemu_bh_cancel(scaler->bh);
    memset(&scaler->srcdst, 0, sizeof(scaler->srcdst));
    scaler->flip_rotate_cfg = 0;
    qatomic_set(&scaler->frame_count, 0);
    qatomic_set(&scaler->irq_sts, 0);
    qatomic_set(&scaler->running, false);
}

static void apple_scaler_reset_hold(Object* obj, ResetType type)
{
    AppleScalerState* scaler = APPLE_SCALER(obj);

    QEMU_LOCK_GUARD(&scaler->lock);

    apple_scaler_update_irqs(scaler);
}

static void apple_scaler_realize(DeviceState* dev, Error** errp)
{
    // AppleScalerState *scaler = APPLE_SCALER(dev);
    //
    // QEMU_LOCK_GUARD(&scaler->lock);
}

static void apple_scaler_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_scaler_reset_enter;
    rc->phases.hold  = apple_scaler_reset_hold;

    dc->realize = apple_scaler_realize;
    dc->desc    = "Apple M2 Scaler and Color Space Converter";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo apple_scaler_type_info = {
    .name          = TYPE_APPLE_SCALER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleScalerState),
    .class_init    = apple_scaler_class_init,
};

static void apple_scaler_register_types(void) { type_register_static(&apple_scaler_type_info); }

type_init(apple_scaler_register_types);

SysBusDevice* apple_scaler_create(AppleDTNode* node, MemoryRegion* dma_mr)
{
    DeviceState*      dev;
    SysBusDevice*     sbd;
    AppleScalerState* scaler;
    AppleDTProp*      prop;
    uint64_t*         reg;
    int               i;

    assert_nonnull(node);
    assert_nonnull(dma_mr);

    dev    = qdev_new(TYPE_APPLE_SCALER);
    sbd    = SYS_BUS_DEVICE(dev);
    scaler = APPLE_SCALER(sbd);

    qemu_mutex_init(&scaler->lock);

    scaler->dma_mr = dma_mr;
    object_property_add_const_link(OBJECT(scaler), "dma_mr", OBJECT(scaler->dma_mr));
    address_space_init(&scaler->dma_as, scaler->dma_mr, "scaler0.dma");

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;
    memory_region_init_io(&scaler->regs[0], OBJECT(scaler), &apple_scaler_reg_ops, scaler, "scaler0.regs0", reg[1]);
    memory_region_enable_lockless_io(&scaler->regs[0]);
    memory_region_init_io(&scaler->regs[1], OBJECT(scaler), &apple_scaler_unk_reg_ops, scaler, "scaler0.regs1", reg[3]);
    memory_region_enable_lockless_io(&scaler->regs[1]);
    sysbus_init_mmio(sbd, &scaler->regs[0]);
    sysbus_init_mmio(sbd, &scaler->regs[1]);

    for (i = 0; i < 2; i++) { sysbus_init_irq(sbd, &scaler->irqs[i]); }

    scaler->bh = aio_bh_new(qemu_get_aio_context(), apple_scaler_bh, scaler);

    return sbd;
}
