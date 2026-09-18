/*
 * Apple SPI Controller.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/dt.h"
#include "hw/dma/apple_sio.h"
#include "hw/irq.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/fifo32.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* XXX: Based on linux/drivers/spi/spi-apple.c */

#define REG_CTRL          0x000
#define REG_CTRL_RUN      BIT(0)
#define REG_CTRL_TX_RESET BIT(2)
#define REG_CTRL_RX_RESET BIT(3)

#define REG_CFG               0x004
#define REG_CFG_AGD           BIT(0)
#define REG_CFG_CPHA          BIT(1)
#define REG_CFG_CPOL          BIT(2)
#define REG_CFG_MODE(_x)      (((_x) >> 5) & 0x3)
#define REG_CFG_MODE_INVALID  0
#define REG_CFG_MODE_IRQ      1
#define REG_CFG_MODE_DMA      2
#define REG_CFG_IE_RXREADY    BIT(7)
#define REG_CFG_IE_TXEMPTY    BIT(8)
#define REG_CFG_LSB_FIRST     BIT(13)
#define REG_CFG_WORD_SIZE(_x) (((_x) >> 15) & 0x3)
#define REG_CFG_WORD_SIZE_8B  0
#define REG_CFG_WORD_SIZE_16B 1
#define REG_CFG_WORD_SIZE_32B 2
#define REG_CFG_IE_COMPLETE   BIT(21)

#define REG_STATUS              0x008
#define REG_STATUS_RXREADY      BIT(0)
#define REG_STATUS_TXEMPTY      BIT(1)
#define REG_STATUS_RXOVERFLOW   BIT(3)
#define REG_STATUS_COMPLETE     BIT(22)
#define REG_STATUS_TXFIFO_SHIFT (6)
#define REG_STATUS_TXFIFO_MASK  (31 << REG_STATUS_TXFIFO_SHIFT)
#define REG_STATUS_RXFIFO_SHIFT (11)
#define REG_STATUS_RXFIFO_MASK  (31 << REG_STATUS_RXFIFO_SHIFT)

#define REG_PIN    0x00c
#define REG_PIN_CS BIT(1)

#define REG_TXDATA     0x010
#define REG_RXDATA     0x020
#define REG_CLKDIV     0x030
#define REG_CLKDIV_MAX 0x7ff
#define REG_RXCNT      0x034
#define REG_WORD_DELAY 0x038
#define REG_TXCNT      0x04c
#define REG_MAX        (0x50)

#define REG_FIFO_DEPTH 16

#define REG(_s, _v) ((_s)->regs[(_v) >> 2])

struct AppleSPIState
{
    SysBusDevice parent_obj;

    MemoryRegion         iomem;
    SSIBus*              ssi_bus;
    AppleSIODMAEndpoint* tx_chan;
    AppleSIODMAEndpoint* rx_chan;

    QemuMutex lock;
    qemu_irq  cs_line_pin;

    qemu_irq irq;
    qemu_irq cs_line;

    Fifo32   rx_fifo;
    Fifo32   tx_fifo;
    uint32_t regs[APPLE_SPI_MMIO_SIZE >> 2];

    int  tx_chan_id;
    int  rx_chan_id;
    bool dma_capable;
};

static uint32_t apple_spi_word_size(AppleSPIState* spi)
{
    switch (REG_CFG_WORD_SIZE(REG(spi, REG_CFG))) {
        case REG_CFG_WORD_SIZE_8B : return 1;
        case REG_CFG_WORD_SIZE_16B: return 2;
        case REG_CFG_WORD_SIZE_32B: return 4;
        default                   : break;
    }
    assert_not_reached();
}

static void apple_spi_update_xfer_tx(AppleSPIState* spi)
{
    if (!fifo32_is_empty(&spi->tx_fifo)) { return; }

    if ((REG_CFG_MODE(REG(spi, REG_CFG))) != REG_CFG_MODE_DMA) {
        REG(spi, REG_STATUS) |= REG_STATUS_TXEMPTY;
        return;
    }

    uint64_t dma_remaining = apple_sio_dma_remaining(spi->tx_chan);
    if (dma_remaining == 0) {
        REG(spi, REG_STATUS) |= REG_STATUS_TXEMPTY;
        return;
    }

    uint32_t word_size = apple_spi_word_size(spi);
    uint32_t dma_len   = REG(spi, REG_TXCNT) * word_size;
    uint64_t fifo_len  = (uint64_t)fifo32_num_free(&spi->tx_fifo) * word_size;

    dma_len = MIN(dma_len, dma_remaining);
    if (dma_len == 0) {
        REG(spi, REG_STATUS) |= REG_STATUS_TXEMPTY;
        return;
    }

    dma_len = MIN(dma_len, fifo_len);
    if (dma_len == 0) {    // TODO: TX Overflow
        return;
    }

    uint8_t* buffer = g_new(uint8_t, dma_len);
    apple_sio_dma_read(spi->tx_chan, buffer, dma_len);

    switch (word_size) {
        case sizeof(uint8_t):
            for (uint64_t i = 0; i < dma_len; ++i) { fifo32_push(&spi->tx_fifo, buffer[i]); }
            break;
        case sizeof(uint16_t):
            for (uint64_t i = 0; i < dma_len; i += sizeof(uint16_t)) {
                fifo32_push(&spi->tx_fifo, lduw_le_p(&buffer[i]));
            }
            break;
        case sizeof(uint32_t):
            for (uint64_t i = 0; i < dma_len; i += sizeof(uint32_t)) {
                fifo32_push(&spi->tx_fifo, ldl_le_p(&buffer[i]));
            }
            break;
        default: assert_not_reached();
    }

    g_free(buffer);
}

static void apple_spi_flush_rx(AppleSPIState* spi)
{
    if ((REG_CFG_MODE(REG(spi, REG_CFG))) != REG_CFG_MODE_DMA) { return; }

    uint64_t dma_remaining = apple_sio_dma_remaining(spi->rx_chan);
    if (dma_remaining == 0) { return; }

    uint32_t word_size = apple_spi_word_size(spi);
    uint64_t dma_len   = (uint64_t)fifo32_num_used(&spi->rx_fifo) * word_size;
    dma_len            = MIN(dma_len, dma_remaining);
    if (dma_len == 0) { return; }

    uint8_t* buffer = g_new(uint8_t, dma_len);

    switch (word_size) {
        case sizeof(uint8_t):
            for (uint64_t i = 0; i < dma_len; ++i) { buffer[i] = fifo32_pop(&spi->rx_fifo); }
            break;
        case sizeof(uint16_t):
            for (uint64_t i = 0; i < dma_len; i += sizeof(uint16_t)) {
                stw_le_p(buffer + i, fifo32_pop(&spi->rx_fifo));
            }
            break;
        case sizeof(uint32_t):
            for (uint64_t i = 0; i < dma_len; i += sizeof(uint32_t)) {
                stl_le_p(buffer + i, fifo32_pop(&spi->rx_fifo));
            }
            break;
        default: assert_not_reached();
    }

    apple_sio_dma_write(spi->rx_chan, buffer, dma_len);
    g_free(buffer);
}

static void apple_spi_update_xfer_rx(AppleSPIState* spi)
{
    if (fifo32_is_empty(&spi->rx_fifo)) { REG(spi, REG_STATUS) &= ~REG_STATUS_RXREADY; }
    else {
        REG(spi, REG_STATUS) |= REG_STATUS_RXREADY;
    }
}

static void apple_spi_update_irq(AppleSPIState* spi)
{
    uint32_t mask = 0;

    if (REG(spi, REG_CFG) & REG_CFG_IE_RXREADY) { mask |= REG_STATUS_RXREADY; }
    if (REG(spi, REG_CFG) & REG_CFG_IE_TXEMPTY) { mask |= REG_STATUS_TXEMPTY; }
    if (REG(spi, REG_CFG) & REG_CFG_IE_COMPLETE) { mask |= REG_STATUS_COMPLETE; }

    qemu_set_irq(spi->irq, (REG(spi, REG_STATUS) & mask) != 0);
}

static void apple_spi_update_cs(AppleSPIState* spi)
{
    if (spi->cs_line_pin != NULL) { qemu_set_irq(spi->cs_line_pin, (REG(spi, REG_PIN) & REG_PIN_CS) != 0); }
}

static void apple_spi_cs_set(void* opaque, int pin, int level)
{
    AppleSPIState* spi = opaque;

    QEMU_LOCK_GUARD(&spi->lock);

    if (level) { REG(spi, REG_PIN) |= REG_PIN_CS; }
    else {
        REG(spi, REG_PIN) &= ~REG_PIN_CS;
    }

    apple_spi_update_cs(spi);
}

static void apple_spi_run(AppleSPIState* spi)
{
    uint32_t tx;
    uint32_t rx;
    int      word_size = apple_spi_word_size(spi);

    if ((REG_CFG_MODE(REG(spi, REG_CFG))) == REG_CFG_MODE_DMA && !spi->dma_capable) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA mode is not supported on this device\n", __func__);
        return;
    }

    if (REG_CFG_MODE(REG(spi, REG_CFG)) == REG_CFG_MODE_INVALID || (REG(spi, REG_CTRL) & REG_CTRL_RUN) == 0
        || ((REG(spi, REG_RXCNT) | REG(spi, REG_TXCNT)) == 0))
    {
        return;
    }

    apple_spi_update_xfer_tx(spi);

    while (REG(spi, REG_TXCNT) && !fifo32_is_empty(&spi->tx_fifo)) {
        tx = fifo32_pop(&spi->tx_fifo);
        rx = 0;
        for (int i = 0; i < word_size; i++) {
            rx <<= 8;
            rx  |= ssi_transfer(spi->ssi_bus, tx & 0xff);
            tx >>= 8;
        }
        REG(spi, REG_TXCNT)--;
        apple_spi_update_xfer_tx(spi);
        if (REG(spi, REG_RXCNT) > 0) {
            if (fifo32_is_full(&spi->rx_fifo)) { apple_spi_flush_rx(spi); }
            if (fifo32_is_full(&spi->rx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: RX overflow\n", __func__);
                REG(spi, REG_STATUS) |= REG_STATUS_RXOVERFLOW;
                break;
            }
            else {
                fifo32_push(&spi->rx_fifo, rx);
                REG(spi, REG_RXCNT)--;
                apple_spi_update_xfer_rx(spi);
            }
        }
    }

    if (fifo32_is_full(&spi->rx_fifo)) { apple_spi_flush_rx(spi); }

    while (!fifo32_is_full(&spi->rx_fifo) && (REG(spi, REG_RXCNT) > 0) && (REG(spi, REG_CFG) & REG_CFG_AGD)) {
        rx = 0;
        for (int i = 0; i < word_size; i++) {
            rx <<= 8;
            rx  |= ssi_transfer(spi->ssi_bus, 0xff);
        }
        if (fifo32_is_full(&spi->rx_fifo)) { apple_spi_flush_rx(spi); }
        if (fifo32_is_full(&spi->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
            REG(spi, REG_STATUS) |= REG_STATUS_RXOVERFLOW;
            break;
        }
        else {
            fifo32_push(&spi->rx_fifo, rx);
            REG(spi, REG_RXCNT)--;
            apple_spi_update_xfer_rx(spi);
        }
    }

    apple_spi_flush_rx(spi);
    if (REG(spi, REG_RXCNT) == 0 && REG(spi, REG_TXCNT) == 0) { REG(spi, REG_STATUS) |= REG_STATUS_COMPLETE; }
}

static void apple_spi_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleSPIState* spi = opaque;
    QEMU_LOCK_GUARD(&spi->lock);
    uint32_t  value  = data;
    uint32_t* mmio   = &REG(spi, addr);
    uint32_t  old    = *mmio;
    bool      cs_flg = false;
    bool      run    = false;

    if (addr >= REG_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: reg WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr,
                      data);
        return;
    }

    switch (addr) {
        case REG_CTRL:
            if (value & REG_CTRL_TX_RESET) {
                fifo32_reset(&spi->tx_fifo);
                value &= ~REG_CTRL_TX_RESET;
            }
            if (value & REG_CTRL_RX_RESET) {
                fifo32_reset(&spi->rx_fifo);
                value &= ~REG_CTRL_RX_RESET;
            }
            if (value & REG_CTRL_RUN) { run = true; }
            break;
        case REG_STATUS: value = old & (~value); break;
        case REG_PIN   : cs_flg = true; break;
        case REG_TXDATA: {
            if (fifo32_is_full(&spi->tx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: tx overflow\n", __func__);
                value = 0;
                break;
            }

            switch (apple_spi_word_size(spi)) {
                case sizeof(uint8_t) : fifo32_push(&spi->tx_fifo, value & 0xFF); break;
                case sizeof(uint16_t): fifo32_push(&spi->tx_fifo, value & 0xFFFF); break;
                case sizeof(uint32_t): fifo32_push(&spi->tx_fifo, value); break;
                default              : assert_not_reached();
            }

            run = true;
            break;
            case REG_TXCNT:
            case REG_RXCNT:
            case REG_CFG  : run = true; break;
        }
        default: break;
    }

    *mmio = value;
    if (cs_flg) { apple_spi_update_cs(spi); }
    if (run) { apple_spi_run(spi); }
    apple_spi_update_irq(spi);
}

static uint64_t apple_spi_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleSPIState* spi = opaque;
    uint32_t       r;
    bool           run = false;

    QEMU_LOCK_GUARD(&spi->lock);

    if (addr >= REG_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: reg READ @ 0x" HWADDR_FMT_plx "\n", __func__, addr);
        return 0;
    }

    r = spi->regs[addr >> 2];
    switch (addr) {
        case REG_RXDATA: {
            if (fifo32_is_empty(&spi->rx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx underflow\n", __func__);
                r = 0;
                break;
            }

            r = fifo32_pop(&spi->rx_fifo);
            if (fifo32_is_empty(&spi->rx_fifo)) { run = true; }
            break;
        }
        case REG_STATUS: {
            uint32_t val  = 0;
            val          |= fifo32_num_used(&spi->tx_fifo) << REG_STATUS_TXFIFO_SHIFT;
            val          |= fifo32_num_used(&spi->rx_fifo) << REG_STATUS_RXFIFO_SHIFT;
            val          &= (REG_STATUS_TXFIFO_MASK | REG_STATUS_RXFIFO_MASK);
            r            &= ~(REG_STATUS_TXFIFO_MASK | REG_STATUS_RXFIFO_MASK);
            r            |= val;
            break;
        }
        default: break;
    }

    if (run) { apple_spi_run(spi); }
    apple_spi_update_irq(spi);
    return r;
}

static const MemoryRegionOps apple_spi_reg_ops = {
    .write                 = apple_spi_reg_write,
    .read                  = apple_spi_reg_read,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness            = DEVICE_LITTLE_ENDIAN,
};

static void apple_spi_reset_enter(Object* obj, ResetType type)
{
    AppleSPIState* spi   = APPLE_SPI(obj);
    BusState*      b     = BUS(spi->ssi_bus);
    BusChild*      child = QTAILQ_FIRST(&b->children);

    QEMU_LOCK_GUARD(&spi->lock);

    memset(spi->regs, 0, sizeof(spi->regs));
    fifo32_reset(&spi->tx_fifo);
    fifo32_reset(&spi->rx_fifo);

    spi->cs_line_pin = NULL;

    if (!child) { return; }

    SSIPeripheralClass* spc = SSI_PERIPHERAL_GET_CLASS(child->child);

    if (spc->cs_polarity == SSI_CS_NONE) { return; }

    spi->cs_line_pin = qdev_get_gpio_in_named(child->child, SSI_GPIO_CS, 0);
}

static void apple_spi_reset_hold(Object* obj, ResetType type)
{
    AppleSPIState* spi = APPLE_SPI(obj);

    QEMU_LOCK_GUARD(&spi->lock);

    apple_spi_update_cs(spi);
}

SSIBus* apple_spi_get_bus(AppleSPIState* spi) { return spi->ssi_bus; }

static void apple_spi_realize(DeviceState* dev, struct Error** errp)
{
    AppleSPIState* spi = APPLE_SPI(dev);
    char           name[32];
    AppleSIOState* sio;

    snprintf(name, sizeof(name), "%s.bus", dev->id);
    spi->ssi_bus = ssi_create_bus(dev, name);

    if (spi->iomem.size == 0) {
        snprintf(name, sizeof(name), "%s.mmio", dev->id);
        memory_region_init_io(&spi->iomem, OBJECT(dev), &apple_spi_reg_ops, spi, name, APPLE_SPI_MMIO_SIZE);
    }

    sio = APPLE_SIO(object_property_get_link(OBJECT(dev), "sio", NULL));

    if (sio == NULL) {
        if (spi->dma_capable) {
            warn_report("%s: SPI bus is DMA capable, but no SIO is attached. "
                        "This is a bug.",
                        __func__);
            spi->dma_capable = false;
        }
    }
    else if (spi->dma_capable) {
        spi->tx_chan = apple_sio_get_endpoint(sio, spi->tx_chan_id);
        spi->rx_chan = apple_sio_get_endpoint(sio, spi->rx_chan_id);
    }
}

SysBusDevice* apple_spi_from_node(AppleDTNode* node)
{
    DeviceState*   dev = qdev_new(TYPE_APPLE_SPI);
    SysBusDevice*  sbd = SYS_BUS_DEVICE(dev);
    AppleSPIState* spi = APPLE_SPI(dev);
    char           name[32];
    AppleDTProp*   prop = apple_dt_get_prop(node, "reg");

    dev->id = apple_dt_get_prop_strdup(node, "name", &error_fatal);

    snprintf(name, sizeof(name), "%s.mmio", dev->id);
    memory_region_init_io(&spi->iomem, OBJECT(dev), &apple_spi_reg_ops, spi, name, ldq_le_p(prop->data + 8));

    if ((prop = apple_dt_get_prop(node, "dma-channels")) != NULL) {
        spi->dma_capable = true;
        spi->tx_chan_id  = ldl_le_p(prop->data);
        spi->rx_chan_id  = ldl_le_p(prop->data + 0x20);
    }

    return sbd;
}

static void apple_spi_instance_init(Object* obj)
{
    AppleSPIState* spi = APPLE_SPI(obj);
    DeviceState*   dev = DEVICE(spi);
    SysBusDevice*  sbd = SYS_BUS_DEVICE(spi);

    sysbus_init_mmio(sbd, &spi->iomem);
    sysbus_init_irq(sbd, &spi->irq);
    sysbus_init_irq(sbd, &spi->cs_line);
    qdev_init_gpio_in_named(dev, apple_spi_cs_set, SSI_GPIO_CS, 1);

    fifo32_create(&spi->tx_fifo, REG_FIFO_DEPTH);
    fifo32_create(&spi->rx_fifo, REG_FIFO_DEPTH);

    qemu_mutex_init(&spi->lock);

    memory_region_enable_lockless_io(&spi->iomem);
}

static void apple_spi_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_spi_reset_enter;
    rc->phases.hold  = apple_spi_reset_hold;

    dc->desc    = "Apple Samsung SPI Controller";
    dc->realize = apple_spi_realize;
}

static const TypeInfo apple_spi_type_info = {
    .name          = TYPE_APPLE_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleSPIState),
    .instance_init = apple_spi_instance_init,
    .class_init    = apple_spi_class_init,
};

static void apple_spi_register_types(void) { type_register_static(&apple_spi_type_info); }

type_init(apple_spi_register_types)
