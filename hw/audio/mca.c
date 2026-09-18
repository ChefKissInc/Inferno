/*
 * Apple Multi-Channel Audio Controller.
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
#include "audio/audio.h"
#include "hw/audio/mca.h"
#include "qemu/error-report.h"
#include "qemu/fifo32.h"
#include "qemu/timer.h"

#if 0
    #define MCA_DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#else
    #define MCA_DPRINTF(fmt, ...) \
        do { }                    \
        while (0)
#endif

// MMIO Index 0: SmartIO MCA
#define MCA_SIO_REG_STRIDE (0x4000)

// All MCA Units
#define REG_SIO_UNIT_CTL    (0x0)
#define SIO_UNIT_CTL_ENABLE BIT(0)
#define SIO_UNIT_CTL_RESET  BIT(1)

// Clock Generator
#define CLK_GEN_REG_BASE         (0x000)
#define REG_CLK_GEN_CFG          (0x4)
#define CLK_GEN_CFG_MCLK_DIVIDER GENMASK(11, 8)

// Sync Generator
#define SYNC_GEN_REG_BASE           (0x100)
#define REG_SYNC_GEN_CLK_CFG        (0x4)
#define SYNC_GEN_CLK_CFG_CLK_SEL(v) ((v) & 0xF)
#define REG_SYNC_GEN_HI_PERIOD      (0x8)
#define REG_SYNC_GEN_LO_PERIOD      (0xC)

// RX{A,B}/TX{A,B}
#define RXA_REG_BASE              (0x200)
#define TXA_REG_BASE              (0x300)
#define RXB_REG_BASE              (0x400)
#define TXB_REG_BASE              (0x500)
#define REG_TX_CFG                (0x4)
#define REG_RX_CFG                (0x8)
#define XFER_CFG_CHANNEL_COUNT(v) ((v) & 0xF)
#define XFER_CFG_WIDTH(v)         (((v) >> 6) & 0x7)
#define XFER_CFG_WIDTH_8          (0)
#define XFER_CFG_WIDTH_16         (1)
#define XFER_CFG_WIDTH_20         (2)
#define XFER_CFG_WIDTH_24         (3)
#define XFER_CFG_WIDTH_32         (4)
#define XFER_CFG_BCLK_POLARITY    BIT(10)
#define XFER_CFG_LSB_FIRST        BIT(11)
#define XFER_CFG_NO_FEEDBACK      BIT(15)
#define XFER_CFG_CLK_SEL(v)       (((v) >> 16) & 0x7)
#define XFER_CFG_CLK_SEL_DEFAULT  (0)
#define XFER_CFG_CLK_SEL_MASTER   (7)
#define REG_TX_CHANNEL_MASK       (0xC)
#define REG_RX_CHANNEL_MASK       (0x10)

// Master Clock Pin
#define MCLK_PIN_REG_BASE                (0x600)
#define REG_MCLK_PIN_CLK_SEL             (0x4)
#define MCLK_PIN_CLK_SEL_I2S_CLOCK(v)    ((v) & 0x7)
#define MCLK_PIN_CLK_SEL_I2S_CLOCK_24MHZ (0x1)
#define MCLK_PIN_CLK_SEL_I2S_CLOCK_12MHZ (0x6)
#define REG_MCLK_PIN_DATA_SEL            (0x8)

// Interrupts
#define INT_REG_BASE               (0x700)
#define REG_INT_STS                (0x0)
#define INT_STS_RXA_FRAME_ERR      BIT(0)
#define INT_STS_RXA_OVERFLOW       BIT(1)
#define INT_STS_RXB_FRAME_ERR      BIT(2)
#define INT_STS_RXB_OVERFLOW       BIT(3)
#define INT_STS_TXA_FRAME_ERR      BIT(4)
#define INT_STS_TXA_UNDERFLOW      BIT(5)
#define INT_STS_TXB_FRAME_ERR      BIT(6)
#define INT_STS_TXB_UNDERFLOW      BIT(7)
#define INT_STS_TXA_FIFO_OVERFLOW  BIT(9)
#define INT_STS_RXA_FIFO_UNDERFLOW BIT(11)
#define INT_STS_TXB_FIFO_OVERFLOW  BIT(13)
#define INT_STS_RXB_FIFO_UNDERFLOW BIT(15)
#define REG_INT_MASK               (0x4)

// MMIO Index 1: MCA DMA
#define MCA_DMA_REG_STRIDE (0x4000)

#define REG_MCA_DMA_CFG                 (0x0)
#define MCA_DMA_CFG_TX_PAD(v)           ((v) & 0x1F)
#define MCA_DMA_CFG_TX_CHANNEL_COUNT(v) (((v) >> 5) & 0x3)
#define MCA_DMA_CFG_CHANNEL_COUNT_1     (0)
#define MCA_DMA_CFG_CHANNEL_COUNT_2     (1)
#define MCA_DMA_CFG_CHANNEL_COUNT_4     (2)
#define MCA_DMA_CFG_CHANNEL_COUNT_8     (3)
#define MCA_DMA_CFG_RX_PAD(v)           (((v) >> 8) & 0x1F)
#define MCA_DMA_CFG_RX_CHANNEL_COUNT(v) (((v) >> 13) & 0x3)
#define REG_MCA_DMA_FIFO_STATUS         (0x8)
#define MCA_DMA_FIFO_STATUS_TX_COUNT(v) ((v) & 0x7F)
#define MCA_DMA_FIFO_STATUS_RX_COUNT(v) (((v) & 0x7F) << 16)
#define REG_MCA_DMA_FIFO_INOUT          (0xC)

// MMIO Index 2: Master Clock Config
#define MCLK_CFG_REG_STRIDE (0x4)

#define REG_MCLK_CFG        (0x0)
#define MCLK_CFG_FREQ(v)    (((v) >> 24) & 0x3)
#define MCLK_CFG_FREQ_24MHZ (0x0)
#define MCLK_CFG_FREQ_12MHZ (0x2)
#define MCLK_CFG_FREQ_6MHZ  (0x3)
#define MCLK_CFG_BUSY       BIT(30)
#define MCLK_CFG_ENABLED    BIT(31)

#define MCA_FRAME_RATE   (48000)
#define MCA_CHANNELS     (2)
#define MCA_SAMPLE_BYTES (sizeof(uint16_t))
#define MCA_FRAME_BYTES  (MCA_CHANNELS * MCA_SAMPLE_BYTES)
#define MCA_BYTE_RATE    (MCA_FRAME_RATE * MCA_FRAME_BYTES)

#define MCA_QUANTUM_BYTES (0x100)
#define MCA_QUANTUM_NS    (MCA_QUANTUM_BYTES * NANOSECONDS_PER_SECOND / MCA_BYTE_RATE)
#define MCA_MAX_DEBT_NS   (50 * SCALE_MS)
#define MCA_MAX_CATCHUP   (4)

#define MCA_RING_SIZE (65536)

typedef struct
{
    uint32_t txb_config;
    uint32_t rxb_config;
    uint32_t txb_control;
} AppleMCACluster;

typedef struct
{
    Fifo32   fifo;
    uint32_t config;
} AppleMCADMA;

struct AppleMCAState
{
    SysBusDevice parent_obj;

    MemoryRegion         mmio[3];
    uint32_t             sio_cluster_count;
    uint32_t             dma_cluster_count;
    uint32_t             mclk_cluster_count;
    AppleMCACluster*     sio_clusters;
    AppleMCADMA*         dma_clusters;
    uint32_t*            mclk_clusters;
    AppleSIODMAEndpoint* tx_ep;
    AppleSIODMAEndpoint* rx_ep;
    QEMUSoundCard        card;
    SWVoiceOut*          voice;
    QEMUTimer*           timer;
    int64_t              last_ns;
    bool                 running;
    uint8_t              ring[MCA_RING_SIZE];
    uint32_t             ring_head;
    uint32_t             ring_used;
};

static void apple_mca_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc;

    dc = DEVICE_CLASS(klass);

    dc->desc           = "Apple Multi-Channel Audio";
    dc->user_creatable = false;
    // dc->realize = apple_mca_realize;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleMCAState, apple_mca, APPLE_MCA, SYS_BUS_DEVICE)

static uint32_t apple_mca_dma_into_ring(AppleMCAState* s, uint32_t want)
{
    uint32_t done = 0;

    if (want > (MCA_RING_SIZE - s->ring_used)) {
        uint32_t drop  = want - (MCA_RING_SIZE - s->ring_used);
        drop           = ROUND_UP(drop, MCA_FRAME_BYTES);
        drop           = MIN(drop, s->ring_used);
        s->ring_head   = (s->ring_head + drop) % MCA_RING_SIZE;
        s->ring_used  -= drop;
    }

    while (done < want) {
        uint32_t freeb = MCA_RING_SIZE - s->ring_used;
        uint32_t tail  = (s->ring_head + s->ring_used) % MCA_RING_SIZE;
        uint32_t run   = MIN(want - done, MIN(freeb, MCA_RING_SIZE - tail));
        uint32_t got;

        if (run == 0) { break; }

        got = apple_sio_dma_read(s->tx_ep, s->ring + tail, run);
        if (got < run) { memset(s->ring + tail + got, 0, run - got); }

        s->ring_used += run;
        done         += run;
    }

    return done;
}

static void apple_mca_serialise(void* opaque)
{
    AppleMCAState* s = opaque;
    int64_t        now;
    uint64_t       due;
    uint64_t       frames;
    uint32_t       pulled = 0;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now <= s->last_ns) {
        timer_mod_ns(s->timer, now + MCA_QUANTUM_NS);
        return;
    }

    frames = muldiv64(now - s->last_ns, MCA_FRAME_RATE, NANOSECONDS_PER_SECOND);
    frames = MIN(frames, (uint64_t)MCA_QUANTUM_BYTES * MCA_MAX_CATCHUP / MCA_FRAME_BYTES);
    due    = frames * MCA_FRAME_BYTES;

    if (due != 0) {
        pulled = apple_mca_dma_into_ring(s, due);
        frames = pulled / MCA_FRAME_BYTES;

        // TX 2ch, RX 4ch. FIXME
        apple_sio_dma_blit(s->rx_ep, 0, frames * (MCA_FRAME_BYTES * 2));

        if (now - s->last_ns > MCA_MAX_DEBT_NS) { s->last_ns = now; }

        s->last_ns += muldiv64(frames, NANOSECONDS_PER_SECOND, MCA_FRAME_RATE);
    }

    timer_mod_ns(s->timer, now + MCA_QUANTUM_NS);
}

static void apple_mca_set_running(AppleMCAState* s, bool running)
{
    if (running == s->running) { return; }

    s->running = running;

    AUD_set_active_out(s->voice, running);
    AUD_set_volume_out(s->voice, !running, 255, 255);

    if (running) {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod_ns(s->timer, s->last_ns + MCA_QUANTUM_NS);
    }
    else {
        timer_del(s->timer);
    }
}

static void apple_mca_sio_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleMCAState* s = opaque;
    hwaddr         index;
    hwaddr         off;

    index = addr / MCA_SIO_REG_STRIDE;
    off   = addr % MCA_SIO_REG_STRIDE;

    MCA_DPRINTF("sio_cluster_%lld: 0x%llX <- 0x%llX\n", index, off, data);

    switch (off) {
        case RXB_REG_BASE + REG_RX_CFG: s->sio_clusters[index].rxb_config = data; break;
        case TXB_REG_BASE + REG_TX_CFG: s->sio_clusters[index].txb_config = data; break;
        case TXB_REG_BASE + REG_SIO_UNIT_CTL:
            if (index == 5) { apple_mca_set_running(s, (data & SIO_UNIT_CTL_ENABLE) != 0); }
            s->sio_clusters[index].txb_control = data;
            break;
    }
}

static uint64_t apple_mca_sio_reg_read(void* const opaque, const hwaddr addr, const unsigned size)
{
    AppleMCAState* s = opaque;

    hwaddr   index;
    hwaddr   off;
    uint64_t ret = 0;

    index = addr / MCA_SIO_REG_STRIDE;
    off   = addr % MCA_SIO_REG_STRIDE;

    switch (off) {
        case RXB_REG_BASE + REG_RX_CFG      : ret = s->sio_clusters[index].rxb_config; break;
        case TXB_REG_BASE + REG_TX_CFG      : ret = s->sio_clusters[index].txb_config; break;
        case TXB_REG_BASE + REG_SIO_UNIT_CTL: ret = s->sio_clusters[index].txb_control; break;
    }

    MCA_DPRINTF("sio_cluster_%lld: 0x%llX -> 0x%llX\n", index, off, ret);
    return ret;
}

static const MemoryRegionOps apple_mca_sio_ops = {
    .write                 = apple_mca_sio_reg_write,
    .read                  = apple_mca_sio_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_mca_dma_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleMCAState* s = opaque;
    hwaddr         index;
    hwaddr         off;

    index = addr / MCA_DMA_REG_STRIDE;
    off   = addr % MCA_DMA_REG_STRIDE;

    MCA_DPRINTF("dma_cluster_%lld: 0x%llX <- 0x%llX\n", index, off, data);

    switch (off) {
        case REG_MCA_DMA_CFG       : s->dma_clusters[index].config = data; break;
        case REG_MCA_DMA_FIFO_INOUT: fifo32_push(&s->dma_clusters[index].fifo, data); break;
        default                    : break;
    }
}

static uint64_t apple_mca_dma_reg_read(void* const opaque, const hwaddr addr, const unsigned size)
{
    AppleMCAState* s = opaque;
    hwaddr         index;
    hwaddr         off;
    uint64_t       ret;

    index = addr / MCA_DMA_REG_STRIDE;
    off   = addr % MCA_DMA_REG_STRIDE;

    switch (off) {
        case REG_MCA_DMA_CFG: ret = s->dma_clusters[index].config; break;
        case REG_MCA_DMA_FIFO_STATUS:
            ret = MCA_DMA_FIFO_STATUS_TX_COUNT(fifo32_num_used(&s->dma_clusters[index].fifo));
            if (fifo32_num_used(&s->dma_clusters[index].fifo) == 0x3C) {
                fifo32_push(&s->dma_clusters[index].fifo, 0);
                fifo32_push(&s->dma_clusters[index].fifo, 0);
                fifo32_push(&s->dma_clusters[index].fifo, 0);
                fifo32_push(&s->dma_clusters[index].fifo, 0);
            }
            break;
        default: ret = 0; break;
    }
    MCA_DPRINTF("dma_cluster_%lld: 0x%llX -> 0x%llX\n", index, off, ret);
    return ret;
}

static const MemoryRegionOps apple_mca_dma_ops = {
    .write                 = apple_mca_dma_reg_write,
    .read                  = apple_mca_dma_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_mca_mclk_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleMCAState* s = opaque;
    hwaddr         index;
    hwaddr         off;

    index = addr / MCLK_CFG_REG_STRIDE;
    off   = addr % MCLK_CFG_REG_STRIDE;

    MCA_DPRINTF("mclk_cluster_%lld: 0x%llX <- 0x%llX\n", index, off, data);

    switch (off) {
        case REG_MCLK_CFG: s->mclk_clusters[index] = (uint32_t)data; break;
        default          : break;
    }
}

static uint64_t apple_mca_mclk_reg_read(void* const opaque, const hwaddr addr, const unsigned size)
{
    AppleMCAState* s = opaque;
    hwaddr         index;
    hwaddr         off;
    uint64_t       ret;

    index = addr / MCLK_CFG_REG_STRIDE;
    off   = addr % MCLK_CFG_REG_STRIDE;

    switch (off) {
        case REG_MCLK_CFG: ret = s->mclk_clusters[index]; break;
        default          : ret = 0; break;
    }
    MCA_DPRINTF("mclk_cluster_%lld: 0x%llX -> 0x%llX\n", index, off, ret);
    return ret;
}

static const MemoryRegionOps apple_mca_mclk_ops = {
    .write                 = apple_mca_mclk_reg_write,
    .read                  = apple_mca_mclk_reg_read,
    .endianness            = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_mca_out_callback(void* opaque, int avail)
{
    AppleMCAState* s = opaque;
    uint32_t       to_write;
    uint32_t       written;

    while (avail > 0 && s->ring_used != 0) {
        to_write = MIN((uint32_t)avail, MIN(s->ring_used, MCA_RING_SIZE - s->ring_head));
        written  = AUD_write(s->voice, s->ring + s->ring_head, to_write);
        if (written == 0) { break; }
        s->ring_head  = (s->ring_head + written) % MCA_RING_SIZE;
        s->ring_used -= written;
        avail        -= written;
    }
}

SysBusDevice* apple_mca_create(AppleDTNode* node, AppleSIODMAEndpoint* tx_ep, AppleSIODMAEndpoint* rx_ep)
{
    DeviceState*   dev;
    SysBusDevice*  sbd;
    AppleMCAState* s;
    AppleDTProp*   prop;
    size_t         i;

    dev = qdev_new(TYPE_APPLE_MCA);
    sbd = SYS_BUS_DEVICE(dev);
    s   = APPLE_MCA(sbd);

    prop = apple_dt_get_prop(node, "reg");
    g_assert_nonnull(prop);

    s->sio_cluster_count = ldq_le_p(prop->data + sizeof(uint64_t)) / MCA_SIO_REG_STRIDE;
    s->sio_clusters      = g_new0(AppleMCACluster, s->sio_cluster_count);

    memory_region_init_io(&s->mmio[APPLE_MCA_MMIO_SIO], OBJECT(s), &apple_mca_sio_ops, s, "sio",
                          s->sio_cluster_count * MCA_SIO_REG_STRIDE);
    sysbus_init_mmio(sbd, &s->mmio[APPLE_MCA_MMIO_SIO]);

    s->dma_cluster_count = ldq_le_p(prop->data + sizeof(uint64_t) * 3) / MCA_DMA_REG_STRIDE;
    s->dma_clusters      = g_new0(AppleMCADMA, s->dma_cluster_count);

    for (i = 0; i < s->dma_cluster_count; ++i) { fifo32_create(&s->dma_clusters[i].fifo, 0x40); }

    memory_region_init_io(&s->mmio[APPLE_MCA_MMIO_DMA], OBJECT(s), &apple_mca_dma_ops, s, "dma",
                          s->dma_cluster_count * MCA_DMA_REG_STRIDE);
    sysbus_init_mmio(sbd, &s->mmio[APPLE_MCA_MMIO_DMA]);

    s->mclk_cluster_count = ldq_le_p(prop->data + sizeof(uint64_t) * 5) / MCLK_CFG_REG_STRIDE;
    s->mclk_clusters      = g_new0(uint32_t, s->mclk_cluster_count);

    memory_region_init_io(&s->mmio[APPLE_MCA_MMIO_MCLK_CFG], OBJECT(s), &apple_mca_mclk_ops, s, "mclk",
                          s->mclk_cluster_count * MCLK_CFG_REG_STRIDE);
    sysbus_init_mmio(sbd, &s->mmio[APPLE_MCA_MMIO_MCLK_CFG]);

    s->tx_ep = tx_ep;
    s->rx_ep = rx_ep;

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_mca_serialise, s);

    if (AUD_register_card("mca", &s->card, NULL)) {
        audsettings settings = {0};
        settings.fmt         = AUDIO_FORMAT_S16;
        settings.freq        = 48000;
        settings.nchannels   = 2;
        settings.endianness  = 0;    // LE
        s->voice             = AUD_open_out(&s->card, s->voice, "mca.out", s, apple_mca_out_callback, &settings);
        if (s->voice == NULL) { error_report("Failed to create voice for Multi-Channel Audio"); }
    }
    else {
        error_report("Failed to create QEMU sound card for Multi-Channel Audio");
    }

    return sbd;
}
