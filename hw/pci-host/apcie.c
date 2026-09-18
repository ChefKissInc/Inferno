/*
 * Apple PCIe IP block emulation
 * Frankenstein's monster built from gutted designware/xiling/pnv_phb and others
 *
 * Copyright (c) 2025-2026 Christian Inci (chris-pcguy).
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
#include "hw/arm/dart.h"
#include "hw/intc/apple_aic.h"
#include "hw/irq.h"
#include "hw/misc/unimp.h"
#include "hw/pci-host/apcie.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci-internal.h"
// #include "hw/pci/msix.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_port.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"

// #define DEBUG_APCIE

#ifdef DEBUG_APCIE
    #define DPRINTF(fmt, ...)                             \
        do {                                              \
            qemu_log_mask(LOG_UNIMP, fmt, ##__VA_ARGS__); \
        }                                                 \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

// #define ENABLE_CPU_DUMP_STATE

#if 0
    #define APPLE_PCIE_PORT_LINK_CONTROL         0x710
    #define APPLE_PCIE_PHY_DEBUG_R1              0x72C
    #define APPLE_PCIE_PHY_DEBUG_R1_XMLH_LINK_UP BIT(4)
    #define APPLE_PCIE_LINK_WIDTH_SPEED_CONTROL  0x80C
    #define APPLE_PCIE_PORT_LOGIC_SPEED_CHANGE   BIT(17)
#endif
#if 1
    #define APPLE_PCIE_MSI_ADDR_LO 0x168
    // #define APPLE_PCIE_MSI_ADDR_HI 0x
    #define APPLE_PCIE_MSI_INTR0_ENABLE 0x124
// #define APPLE_PCIE_MSI_INTR0_MASK 0x82C
// #define APPLE_PCIE_MSI_INTR0_STATUS 0x830
#endif
// synopsis designware possible reused regs: 0x80c, but NOT 0x82c

static void pcie_set_power_device(PCIBus* bus, PCIDevice* dev, void* opaque)
{
    bool* power = opaque;

    pci_set_power(dev, *power);
}

void port_devices_set_power(ApplePCIEPort* port, bool power)
{
    if (port->manual_enable) {
        PCIDevice* pci_dev = PCI_DEVICE(port);
        PCIBus*    sec_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(pci_dev));
        pci_for_each_device(sec_bus, pci_bus_num(sec_bus), pcie_set_power_device, &power);
    }
}

static void apcie_port_gpio_set_clkreq(DeviceState* dev, int level)
{
    ApplePCIEPort* port = APPLE_PCIE_PORT(dev);
    DPRINTF("%s: device set_irq: old: %d ; new %d\n", __func__, port->gpio_clkreq_val, level);
    port->gpio_clkreq_val = level;
    qemu_set_irq(port->apcie_port_gpio_clkreq_irq, level);
}

static void apcie_port_gpio_perst(void* opaque, int n, int level)
{
    ApplePCIEPort* port = opaque;
    bool           val  = !!level;
    assert(n == 0);
    DPRINTF("%s: old: %d ; new %d\n", __func__, port->gpio_perst_val, val);
    if (port->gpio_perst_val != val) {
        // val != EP PERST
        // val: 0 during disable, 1 during enable
        // this breaks manual-enable ports
        ////port_devices_set_power(port, val);
    }
    port->gpio_perst_val = val;
}

static int apple_pcie_map_irq(PCIDevice* pci_dev, int irq_num)
{
    /* Check that out properly ... */
    // return 0;
    return irq_num & 3;
}

static void apple_pcie_set_irq(void* opaque, int irq_num, int level)
{
    ApplePCIEHost* host = opaque;

    qemu_set_irq(host->irqs[irq_num], level);
}

static void apple_pcie_set_own_irq(ApplePCIEPort* port, int level)
{
    ApplePCIEHost* host    = port->host;
    int            irq_num = port->bus_nr;

    // handling this, it might trigger interrupts on unmask otherwise
    port->port_last_interrupt &= ~port->port_interrupt_mask;

    if (level && !port->port_last_interrupt) { return; }
    qemu_set_irq(host->irqs[irq_num], level);
}

static void apple_pcie_root_bus_class_init(ObjectClass* klass, const void* data)
{
    BusClass* k = BUS_CLASS(klass);

    /*
     * Designware has only a single root complex. Enforce the limit on the
     * parent bus
     * And so does Apple, apparently.
     */
    k->max_dev = 1;
}

#if 1
static uint64_t apple_pcie_port_msi_read(void* opaque, hwaddr addr, unsigned size)
{
    /*
     * Attempts to read from the MSI address are undefined in
     * the PCI specifications. For this hardware, the datasheet
     * specifies that a read from the magic address is simply not
     * intercepted by the MSI controller, and will go out to the
     * AHB/AXI bus like any other PCI-device-initiated DMA read.
     * This is not trivial to implement in QEMU, so since
     * well-behaved guests won't ever ask a PCI device to DMA from
     * this address we just log the missing functionality.
     */
    DPRINTF("%s not implemented\n", __func__);
    return 0;
}

static void apple_pcie_port_msi_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEPort* port   = opaque;
    ApplePCIEHost* host   = port->host;
    int            bus_nr = port->bus_nr;

    DPRINTF("%s: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr, data);

    // int msi_intr_index = 0;
    ////int msi_intr_index = 1;
    // int msi_intr_index = data;
    int msi_intr_index = data % 8;
    assert_cmpuint(msi_intr_index, <, APPLE_PCIE_NUM_MSI_BANKS);

    #if 0
    port->msi.intr[msi_intr_index].status |=
        BIT(data) & port->msi.intr[msi_intr_index].enable;

    if (port->msi.intr[msi_intr_index].status &
        ~port->msi.intr[msi_intr_index].mask) {
    #endif
    // uint32_t status = BIT(data) & port->msi.intr[msi_intr_index].enable;
    uint32_t status        = BIT(msi_intr_index) & port->msi.intr[msi_intr_index].enable;
    uint32_t msi_interrupt = host->pcie->msi_vector_offset + data;
    DPRINTF("%s: status: 0x%x ; msi_enable: 0x%x ; BIT(msi_intr_index): 0x%" PRIX64
            " ; msi_intr_index: 0x%x ; msi_interrupt: %d/0x%x\n",
            __func__, status, port->msi.intr[msi_intr_index].enable, BIT(msi_intr_index), msi_intr_index, msi_interrupt,
            msi_interrupt);

    status = true;
    // status might be msi_intr_index
    if (status) {
        // iOS will only acknowledge the interrupt when it expects it, and will
        // cause an interrupt storm otherwise
        // need to find a place to quisce it properly
        // apple_aic_unmask_interrupt(msi_interrupt);
        qemu_set_irq(host->msi_irqs[bus_nr * 8 + msi_intr_index], 1);
        // apple_aic_mask_interrupt(msi_interrupt);
        //  pulsing doesn't work.
        //  qemu_irq_pulse(host->msi_irqs[bus_nr * 8 + msi_intr_index]);
    }
}

void apple_pcie_port_temp_lower_msi_irq(ApplePCIEPort* port, int msi_intr_index)
{
    ApplePCIEHost* host   = port->host;
    int            bus_nr = port->bus_nr;

    DPRINTF("%s: temporary function: bus_nr: %d ; msi_intr_index: %d\n", __func__, bus_nr, msi_intr_index);

    assert_cmpuint(msi_intr_index, <, APPLE_PCIE_NUM_MSI_BANKS);

    qemu_set_irq(host->msi_irqs[bus_nr * 8 + msi_intr_index], 0);
}

static const MemoryRegionOps apple_pcie_port_msi_ops = {
    .read       = apple_pcie_port_msi_read,
    .write      = apple_pcie_port_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static void apple_pcie_port_update_msi_mapping(ApplePCIEPort* port)

{
    int            msi_intr_index = 0;
    MemoryRegion*  mem            = &port->msi.iomem;
    const uint64_t base           = port->msi.base;
    const bool     enable         = port->msi.intr[msi_intr_index].enable;

    // return;
    memory_region_set_address(mem, base);
    memory_region_set_enabled(mem, enable);
    // dma_mr
}
#endif

static void machine_set_gpio(int interrupt_num, int level)
{
    DeviceState* gpio;
    gpio = DEVICE(object_property_get_link(OBJECT(qdev_get_machine()), "gpio", &error_fatal));

    DPRINTF("%s: called with interrupt_num 0x%x/%u level %u\n", __func__, interrupt_num, interrupt_num, level);
    qemu_set_irq(qdev_get_gpio_in(gpio, interrupt_num), level);
}

static uint32_t apple_pcie_port_bridge_config_read(PCIDevice* d, uint32_t address, int len)
{
    ApplePCIEPort* port = APPLE_PCIE_PORT(d);
    ApplePCIEHost* host = port->host;

    uint32_t val;

    switch (address) {
        case 0x3c:
            // hotResetLinkPartner
            goto jump_default;
        case 0x80:
            // readAndClearLinkControlSts
            goto jump_default;
        case 0x130:
            // isPortErrorInterrupt
            goto jump_default;
        case 0x17c:
            // _captureRASCounters
            goto jump_default;
        case 0x710:
            // _initializeRootComplex
            goto jump_default;
        case 0x728:
            // logLinkState ; ltssm state & 0x1f
            goto jump_default;
        case 0x80c:
            // _initializeRootComplex
            goto jump_default;
        case 0x890:
            // _initializeRootComplex/maximum_link_speed
            goto jump_default;
        default:
        jump_default:
            val = pci_default_read_config(d, address, len);
            DPRINTF("%s: bridge_config: READ DEFAULT @ 0x%x value:"
                    " 0x%x\n",
                    __func__, address, val);
            break;
    }

    DPRINTF("%s: READ @ 0x%x value: 0x%x\n", __func__, address, val);
    return val;
}

static void apple_pcie_port_bridge_config_write(PCIDevice* d, uint32_t address, uint32_t val, int len)
{
    ApplePCIEPort* port = APPLE_PCIE_PORT(d);
    ApplePCIEHost* host = port->host;

    DPRINTF("%s: WRITE @ 0x%x value: 0x%x\n", __func__, address, val);

    switch (address) {
        case 0x04:
            // HACK to activate the sub-devices (again).
#if 0
        if ((val & 1) != 0) {
            port_devices_set_power(port, true);
        }
#endif
            goto jump_default;
        case 0x3c:
            // hotResetLinkPartner
            goto jump_default;
        case 0x80:
            // readAndClearLinkControlSts
            goto jump_default;
        case 0x178:
            // _enableRASCounters/_captureRASCounters
            goto jump_default;
        case 0x710:
            // _initializeRootComplex
            goto jump_default;
        case 0x80c:
            // _initializeRootComplex
            goto jump_default;
        case 0x890:
            // _initializeRootComplex/maximum_link_speed
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: bridge_config: WRITE DEFAULT @ 0x%x value:"
                    " 0x%x\n",
                    __func__, address, val);
            pci_bridge_write_config(d, address, val, len);
            break;
    }
}

static uint64_t apple_pcie_root_conf_access(void* opaque, hwaddr addr, uint64_t* data, unsigned size)
{
    ApplePCIEHost* host      = opaque;
    hwaddr         orig_addr = addr;

    uint8_t busnum, device, function, devfn;
    function = (addr % 0x8000) / 0x1000;
    device   = addr / 0x8000;
    // function = (addr & 0x7FFF) >> 12;
    // device = addr >> 15;
    busnum                = device / PCI_SLOT_MAX;
    device               %= PCI_SLOT_MAX;
    devfn                 = PCI_DEVFN(device, function);
    PCIHostState* pci     = PCI_HOST_BRIDGE(host);
    PCIDevice*    pcidev  = pci_find_device(pci->bus, busnum, devfn);

    if (pcidev) {
        addr &= pci_config_size(pcidev) - 1;

#if 0
        if (addr == 0x890) {
            cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
        }
#endif
        if (data) {
#if 0
            if (addr == 0x80c) {
                // offset 0x80c: setLinkSpeed: bit17: left-over from synopsys designware: ignored by iOS.
                ApplePCIEPort *port = APPLE_PCIE_PORT(pcidev);
                PCIESlot *slot = PCIE_SLOT(pcidev);

                DPRINTF("%s: port->bus_nr == %u ; slot->width == %u ; slot->speed == %u\n", __func__, port->bus_nr, slot->width, slot->speed);
                pcie_cap_fill_link_ep_usp(pcidev, slot->width, slot->speed);
            }
#endif
            pci_host_config_write_common(pcidev, addr, pci_config_size(pcidev), *data, size);
            DPRINTF("%s: test0: %02u:%02u.%01u: orig_addr == 0x" HWADDR_FMT_plx " ; size %u ; write 0x" HWADDR_FMT_plx
                    "\n",
                    __func__, busnum, device, function, orig_addr, size, *data);
        }
        else {
            uint64_t ret = pci_host_config_read_common(pcidev, addr, pci_config_size(pcidev), size);
            DPRINTF("%s: test0: %02u:%02u.%01u: orig_addr == 0x" HWADDR_FMT_plx " ; size %u ; read 0x" HWADDR_FMT_plx
                    "\n",
                    __func__, busnum, device, function, orig_addr, size, ret);
            return ret;
        }
    }
    else {
        if (data) {
            DPRINTF("%s: test0: %02u:%02u.%01u: orig_addr == 0x" HWADDR_FMT_plx " ; size %u ; write UNKNOWN DEVICE\n",
                    __func__, busnum, device, function, orig_addr, size);
        }
        else {
            DPRINTF("%s: test0: %02u:%02u.%01u: orig_addr == 0x" HWADDR_FMT_plx " ; size %u ; read UNKNOWN DEVICE\n",
                    __func__, busnum, device, function, orig_addr, size);
        }
    }

    return UINT64_MAX;
}

static uint64_t apple_pcie_root_conf_read(void* opaque, hwaddr addr, unsigned size)
{
    // offset 0x80c: setLinkSpeed: bit17: left-over from synopsys designware:
    // ignored by iOS.
    return apple_pcie_root_conf_access(opaque, addr, NULL, size);
}

static void apple_pcie_root_conf_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    // offset 0x80c: setLinkSpeed: bit17: left-over from synopsys designware:
    // ignored by iOS.
    apple_pcie_root_conf_access(opaque, addr, &data, size);
}

static const MemoryRegionOps apple_pcie_root_conf_ops = {
    .read       = apple_pcie_root_conf_read,
    .write      = apple_pcie_root_conf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_root_common_read(void* opaque, hwaddr addr, unsigned int size)
{
    ApplePCIEHost* host = opaque;
    uint32_t       val  = 0;

    switch (addr) {
        case 0x0:
            // break;
            goto jump_default;
        case 0x24:    // TODO: for T8015
            val = 0xffffffff;
            break;
            // goto jump_default;
        case 0x34:
            // break;
            goto jump_default;
        case 0x28:
            val = 0x10;    // refclk good ; for T8030
            break;
        case 0x1ac:    // for S8000
            val = 0x1;
            break;
            // goto jump_default;
        case 0x22c:    // for S8000
            val = 0x1;
            break;
            // goto jump_default;
        default:
        // val = 0;
        jump_default:
            val = host->root_common_regs[addr >> 2];
            DPRINTF("%s: root_common: READ DEFAULT @ 0x" HWADDR_FMT_plx " value:"
                    " 0x%x\n",
                    __func__, addr, val);
            break;
    }

    DPRINTF("%s: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
    return val;
}

static void apple_pcie_root_common_write(void* opaque, hwaddr addr, uint64_t data, unsigned int size)
{
    ApplePCIEHost* host = opaque;
    ApplePCIEPort* port;
    int            i;

    DPRINTF("%s: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr, data);
    switch (addr) {
        case 0x0:
            // break;
            goto jump_default;
        case 0x24:
            // break;
            goto jump_default;
        case 0x34:
            // break;
            goto jump_default;
        case 0x4:
            if (data == 0x11) {
                host->root_common_regs[0x114 >> 2] = 0x100;
                // for S8000/N66AP
                // host->root_common_regs[0x2c >> 2] = 0x11;
                host->root_common_regs[0x2c >> 2] = 0x1;
                // for S8000/N66AP, mayber lower for S8003, dunno.
                host->root_common_regs[0x12c >> 2] = 0x1;
#if 0
            if (host->clkreq_gpio_id != 0) {
                machine_set_gpio(host->clkreq_gpio_id, host->clkreq_gpio_value);
            }
#endif
            }
            break;
        case 0x38:
            if (data == 0x1) {
                // for S8000/N66AP
                host->root_common_regs[0x2c >> 2] |= 0x10;
#if 1
                for (i = 0; i < APCIE_MAX_PORTS; i++) {
                    port = host->pcie->ports[i];
                    //////apcie_port_gpio_set_clkreq(DEVICE(port), 0);
                    apcie_port_gpio_set_clkreq(DEVICE(port), 1);
                }
#endif
            }
            break;
        case 0x114:
            if (data == 0x101) {
                ////host->root_common_regs[0x2c >> 2] = 0x11; // for S8003/N66mAP
            }
            break;
        default:
        jump_default:
            DPRINTF("%s: root_common: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value:"
                    " 0x" HWADDR_FMT_plx "\n",
                    __func__, addr, data);
            break;
    }
    host->root_common_regs[addr >> 2] = data;
}

static const MemoryRegionOps apple_pcie_root_common_ops = {
    .read       = apple_pcie_root_common_read,
    .write      = apple_pcie_root_common_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl =
        {
            .min_access_size = 4,
            .max_access_size = 4,
            .unaligned       = false,
        },
};

static uint64_t apple_pcie_host_root_phy_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEHost* host = opaque;
    uint32_t       val  = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    // #if 1
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x0    : val = host->root_phy_enabled; break;
        case 0x10000: val = host->root_refclk_buffer_enabled; break;
        default:
        jump_default:
            DPRINTF("%s: root_phy: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x"
                    "\n",
                    __func__, addr, val);
            break;
    }

    DPRINTF("%s: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
    return val;
}

static void apple_pcie_host_root_phy_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEHost* host = opaque;

    DPRINTF("%s: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr, data);
    switch (addr) {
        case 0x0:
            DPRINTF("root_phy: phy_enabled before == 0x%x\n", host->root_phy_enabled);
            if ((data & (1 << 0)) != 0) { data |= (1 << 2); }
            if ((data & (1 << 1)) != 0) { data |= (1 << 3); }
            host->root_phy_enabled = data;
            DPRINTF("phy_enabled after == 0x%x\n", host->root_phy_enabled);
            break;
        case 0x10000:    // for refclk buffer
            DPRINTF("root_phy: refclk_buffer_enabled before == 0x%x\n", host->root_refclk_buffer_enabled);
            if ((data & (1 << 0)) != 0) { data |= (1 << 2); }
            if ((data & (1 << 1)) != 0) {
                // TODO: maybe bit3 here as well.
                data |= (1 << 1);    // yes, REALLY bit1
            }
            host->root_refclk_buffer_enabled = data;
            DPRINTF("refclk_buffer_enabled after == 0x%x\n", host->root_refclk_buffer_enabled);
            break;
        default:
        jump_default:
            DPRINTF("%s: root_phy: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr,
                    data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_host_root_phy_ops = {
    .read       = apple_pcie_host_root_phy_read,
    .write      = apple_pcie_host_root_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_host_root_phy_ip_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEHost* host = opaque;
    uint32_t       val  = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    // #if 1
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        default:
        jump_default:
            DPRINTF("%s: root_phy_ip: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
            break;
    }

    DPRINTF("%s: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
    return val;
}

static void apple_pcie_host_root_phy_ip_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEHost* host = opaque;

    DPRINTF("%s: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr, data);
    switch (addr) {
        default:
        jump_default:
            DPRINTF("%s: root_phy_ip: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_host_root_phy_ip_ops = {
    .read       = apple_pcie_host_root_phy_ip_read,
    .write      = apple_pcie_host_root_phy_ip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_host_root_axi2af_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEHost* host = opaque;
    uint32_t       val  = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    // #if 1
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x410:
            // break;
            goto jump_default;
        case 0x420:
            // break;
            goto jump_default;
        case 0x430:
            // break;
            goto jump_default;
        case 0x72c:
            // break;
            goto jump_default;
        case 0x768:
            // break;
            goto jump_default;
        case 0x810:
            // break;
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: root_axi2af: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
            break;
    }

    DPRINTF("%s: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, addr, val);
    return val;
}

static void apple_pcie_host_root_axi2af_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEHost* host = opaque;

    DPRINTF("%s: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, addr, data);
    switch (addr) {
        case 0x0:
            // break;
            goto jump_default;
        case 0x400:
            // break;
            goto jump_default;
        case 0x410:
            // break;
            goto jump_default;
        case 0x420:
            // break;
            goto jump_default;
        case 0x430:
            // break;
            goto jump_default;
        case 0x600:
            // break;
            goto jump_default;
        case 0x708:
            // break;
            goto jump_default;
        case 0x70c:
            // break;
            goto jump_default;
        case 0x710:
            // break;
            goto jump_default;
        case 0x714:
            // break;
            goto jump_default;
        case 0x718:
            // break;
            goto jump_default;
        case 0x71c:
            // break;
            goto jump_default;
        case 0x72c:
            // break;
            goto jump_default;
        case 0x744:
            // break;
            goto jump_default;
        case 0x748:
            // break;
            goto jump_default;
        case 0x74c:
            // break;
            goto jump_default;
        case 0x750:
            // break;
            goto jump_default;
        case 0x754:
            // break;
            goto jump_default;
        case 0x758:
            // break;
            goto jump_default;
        case 0x768:
            // break;
            goto jump_default;
        case 0x800:
            // break;
            goto jump_default;
        case 0x810:
            // break;
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: root_axi2af: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_host_root_axi2af_ops = {
    .read       = apple_pcie_host_root_axi2af_read,
    .write      = apple_pcie_host_root_axi2af_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_port_config_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEPort* port = opaque;
    uint32_t       is_port_enabled;
    uint32_t       val            = 0;
    int            msi_intr_index = 0;

// #ifdef ENABLE_CPU_DUMP_STATE
#if 0
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif
    // T8030 doesn't support legacy interrupts

    is_port_enabled = (port->port_cfg_port_config & 1) != 0;

    switch (addr) {
        case 0x80 : val = port->port_ltssm_enable; break;
        case 0x8c : val = port->port_pme_to_ack; break;
        case 0x100:    // pcielint/getPortInterrupts
            // val = 0xdeadbeef;
            // val |= 0xf; // legacy interrupts. not on t8030
            // val |= 0x10; // isPortErrorInterrupt
            // val |= 0x300; // isPMEHandshakeInterrupt on t8103
            // val |= 0x1000; // link-up interrupt
            // val |= 0x4000; // link-down interrupt // T8103's one is: 0x80004000
            // val |= 0x8000; // AF timeout interrupt
            // val |= 0x20000; // bad-request-interrupt: malformed mmu request
            // val |= 0x40000; // bad-request-interrupt: msi error
            // val |= 0x80000; // bad-request-interrupt: msi data miscompare
            // val |= 0x200000; // bad-request-interrupt: read response error
            // val |= 0x800000; // completion-timeout interrupt
            // val |= 0x2000000; // completer-abort interrupt
            // val |= 0x4000000; // bad-request-interrupt: requester-to-sid mapping
            // error
            //  hex(0x1000|0x4000|0x8000|0x20000|0x40000|0x80000|0x200000|0x800000|0x2000000|0x4000000)
            //  == 0x6aed000 enableInterrupts doesn't clear completer-abort
            //  interrupt
            val = port->port_last_interrupt;
            //  Don't reset/clear the value here, iOS will do that!
            ////port->port_last_interrupt = 0;
            // val = port->msi.intr[msi_intr_index].status;
            // // HACK ORing
            // val |= port->port_last_interrupt;
            break;
        case 0x104:    // read in AppleT803xPCIePort::disableAERInterrupts, written
                       // back via enableInterrupts
            val = port->port_interrupt_mask;
            break;
        case 0x108:    // AppleT803xPCIePort::disableAERInterrupts
            // TODO
            break;
        case APPLE_PCIE_MSI_ADDR_LO:    // msi addr low
            val = port->msi.base;
            break;
        // case APPLE_PCIE_MSI_ADDR_HI: // msi addr high
        //     val = port->msi.base >> 32;
        //     break;
        case APPLE_PCIE_MSI_INTR0_ENABLE:
            ////val = port->msi.intr[msi_intr_index].enable;
            val = port->port_msiVectors;
            break;
        // TODO: what/where is msi.status, does it even exist?
        case 0x128:    // msi unknown
            val = port->port_msiUnknown0;
            // val0 = ((val >> 0) & 0xffff) / 0x8
            // val1 = ((val >> 16) & 0xffff) / 0x8
            break;
        case 0x13c: val = port->port_hotreset; break;
        case 0x88 :    // S800x linksts
        case 0x208:    // T8030 linksts ; for getLinkUp/isLinkInL2.
            if (addr == 0x88 && (port->host->pcie->chip_id == 0x8000 || port->host->pcie->chip_id == 0x8003)) {
                port->is_link_up = (port->port_ltssm_enable & 1) != 0;
                DPRINTF("%s: Port %u: S800x linksts: is_link_up: %d\n", __func__, port->bus_nr, port->is_link_up);
            }
            else {
                port->is_link_up = is_port_enabled;
                DPRINTF("%s: Port %u: T8030/else linksts: is_link_up: %d\n", __func__, port->bus_nr, port->is_link_up);
            }
            val = (port->is_link_up << 0);    // getLinkUp
            if (port->is_link_up) {
                // TODO: I doubt that this is correct, but it should be good enough.
                // commented out, because is_link_up stays on during requesting l2
                // port->is_link_in_l2 = false;
            }
            // not sure why real s8000 and t8015 continuously fail to enter L2
            val                 |= (port->is_link_in_l2 << 6);    // isLinkInL2
            port->is_link_in_l2  = false;
            break;
        case 0x210:    // linkcdmsts
            val = port->port_linkcdmsts;
            break;
        case 0x800:    // for setPortEnable/initializeRootComplex/expressCapOffset?
                       // bit0 seems to be "enable port"
            val = port->port_cfg_port_config;
            break;
        case 0x804:    // for enable port hardware ; port status ; bit0: port status
                       // ready
            val = is_port_enabled;
            // val = (port->gpio_perst_val << 0);
            break;
        case 0x80c:    // disablePortHardware
            // not to be confused with the config register used in setLinkSpeed
            val = 0x0;
            // break;
            goto jump_default;
        case 0x810          : val = port->port_cfg_refclk_config; break;
        case 0x814          : val = port->port_cfg_rootport_perst; break;
        case 0x828 ... 0x924: {
            int sid_0 = (addr - 0x828) >> 2;
            val       = port->port_rid_sid_map[sid_0];
            break;
        }
        case 0x4000 ... 0x400c:
            // readTimeCounter
            // break;
            goto jump_default;
        case 0x4010 ... 0x410c:
            // readEntriesCounter
            // break;
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: Port %u: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x"
                    "\n",
                    __func__, port->bus_nr, addr, val);
            break;
    }

    DPRINTF("%s: Port %u: READ @ 0x" HWADDR_FMT_plx " value: 0x%x"
            "\n",
            __func__, port->bus_nr, addr, val);
    return val;
}

static void apple_pcie_port_config_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEPort* port = opaque;
    uint32_t       is_port_enabled;
    int            msi_intr_index = 0;

    DPRINTF("%s: Port %u: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, port->bus_nr, addr,
            data);
    switch (addr) {
        case 0x80:
            // bit0 is ltssm enable
            port->port_ltssm_enable = data;
            DPRINTF("%s: reg==0x80: Port %u: port_ltssm_enable: 0x%x\n", __func__, port->bus_nr,
                    port->port_ltssm_enable);
            if ((data & 1) != 0) {
                DPRINTF("%s: reg==0x80: Port %u: enable_power_and_irq\n", __func__, port->bus_nr);
                if (port->manual_enable) { port_devices_set_power(port, true); }
                // TODO: handle link-down and other interrupts as well.
#if 0
            // link-up interrupt
            port->msi.intr[msi_intr_index].status |= 0x1000;
#endif
                port->port_last_interrupt |= 0x1000;
                apple_pcie_set_own_irq(port, 1);
            }
            break;
        case 0x8c:
            // t8030: write requestPMEToBroadcast value 0x11
            // s8000: write requestPMEToBroadcast value 0x31
            // read receivedPMEToAck value/pmeto full value and bit0
            // possible expected return value 0x10
            // bit0 might be the acknowledgement, and bit4 the actual result
            // bit5 some request bit?
            port->port_pme_to_ack = data;
            // if ((port->port_pme_to_ack & BIT(0)) != 0)
            if (((port->port_pme_to_ack & BIT(4)) != 0) && ((port->port_pme_to_ack & BIT(0)) != 0)) {
                // TODO: I doubt that this is correct, but it should be good enough.
                port->is_link_in_l2 = true;
            }
            // port->port_pme_to_ack &= ~(0x20 | 0x1);
            port->port_pme_to_ack &= ~(BIT(5) | BIT(0));
            break;
        case 0x100:    // pcielint? ; and enableInterrupts?
                       // clearLinkUpInterrupt/clearPortInterrupts
#if 1
            DPRINTF("%s: reg==0x100: Port %u: previous_port_last_interrupt: 0x%x\n", __func__, port->bus_nr,
                    port->port_last_interrupt);
            port->port_last_interrupt &= ~data;    // not xor
            DPRINTF("%s: reg==0x100: Port %u: current_port_last_interrupt: 0x%x\n", __func__, port->bus_nr,
                    port->port_last_interrupt);
            // port->port_last_interrupt == 0x0 might not be nothing, but INTx 0x0,
            // so always lower irq
            // if (!port->port_last_interrupt)
            {
                apple_pcie_set_own_irq(port, 0);
                // qemu_set_irq(port->msi_irqs[msi_intr_index], 0);
            }
#endif
            break;
        case 0x104:    // disableVectorHard/enableInterrupts/enableVector
            // (0xf << 4) == AER interrupts;
            port->port_interrupt_mask = data;
            break;
        case 0x108:    // disableAERInterrupts
            // TODO
            break;
        case 0x128:    // msi unknown
            // 0x0000000000180018
            // (data >> 0) & 0x1f
            // (data >> 16) & 0x1f
            port->port_msiUnknown0 = data;
            // TODO: maybe min-max vectors
#if 0
        // TODO: which is which? assuming that it's even remotely correct.
        int msiUnknown0_data0 = (data >> 0) & 0x1f;
        int msiUnknown0_data1 = (data >> 16) & 0x1f;
        port->msi.intr[msi_intr_index].mask = 1 << msiUnknown0_data0;
        port->msi.intr[msi_intr_index].status ^= 1 << msiUnknown0_data1;
        if (!port->msi.intr[msi_intr_index].status) {
            qemu_set_irq(port->msi_irqs[msi_intr_index], 0);
        }
#endif
            break;
        case 0x13c:
            // bit8 is hot reset
            port->port_hotreset = data;
#if 1
            if ((data & 0x100) != 0) {
                // return 0x4000 at offset 0x100
                port->port_last_interrupt |= 0x4000;    // link-down interrupt
                apple_pcie_set_own_irq(port, 1);
            }
#endif
            break;
        case 0x210:    // linkcdmsts
            port->port_linkcdmsts &= ~data;
            // port->port_linkcdmsts &= ~(uint32_t)data;
            break;
        case 0x800:    // for setPortEnable/initializeRootComplex/expressCapOffset?
                       // bit0 seems to be "enable port"
            port->port_cfg_port_config = data;
            is_port_enabled            = (port->port_cfg_port_config & 1) != 0;
            DPRINTF("%s: reg==0x800: Port %u: port_cfg_port_config: 0x%x ;"
                    " is_port_enabled: %u\n",
                    __func__, port->bus_nr, port->port_cfg_port_config, is_port_enabled);
            break;
        // case 0x80c:
        //     // not to be confused with the config register used in setLinkSpeed
        //     break;
        case 0x810: port->port_cfg_refclk_config = data; break;
        case 0x814:
            port->port_cfg_rootport_perst = data;
            bool perst_bool               = ((port->port_cfg_rootport_perst & 1) != 0);
            break;
        case APPLE_PCIE_MSI_ADDR_LO:    // msi address & 0xfffffff0
            // 0x00000000fffff000
            port->msi.base &= 0xFFFFFFFF00000000ULL;
            port->msi.base |= data;
            apple_pcie_port_update_msi_mapping(port);
            if (data != 0) {
                // apcie_port_gpio_set_clkreq(DEVICE(port), 0);
                // apcie_port_gpio_set_clkreq(DEVICE(port), 1);
                // qemu_irq_raise(port->apcie_port_gpio_clkreq_irq);
                // qemu_irq_lower(port->apcie_port_gpio_clkreq_irq);
            }
            break;
        // case APPLE_PCIE_MSI_ADDR_HI:
        //     port->msi.base &= 0x00000000FFFFFFFFULL;
        //     port->msi.base |= (uint64_t)val << 32;
        //     apple_pcie_port_update_msi_mapping(port);
        //     break;
        case APPLE_PCIE_MSI_INTR0_ENABLE:    // msiVectors
            // 0x0000000000000031
            // 32 == 0x51 ; 16 == 0x41 ; 8 == 0x31 ; 4 == 0x21 ; 2 == 0x11 ; 1 ==
            // 0x1 ; 0 == 0x0
            port->port_msiVectors = data;
#if 1
            uint32_t enable      = (data & 1) != 0;
            uint32_t vectors     = 1 << ((data & 0xf0) >> 4);
            uint32_t vector_mask = (1 << vectors) - 1;
    #if 0
        if (enable) {
            port->msi.intr[msi_intr_index].enable = vector_mask;
        } else {
            port->msi.intr[msi_intr_index].enable = 0;
        }
    #endif
    #if 1
            if (enable) { port->msi.intr[msi_intr_index].enable |= vector_mask; }
            else {
                port->msi.intr[msi_intr_index].enable &= ~vector_mask;
            }
    #endif
            apple_pcie_port_update_msi_mapping(port);
#endif
            break;
        // case DESIGNWARE_PCIE_MSI_INTR0_MASK:
        //     port->msi.intr[msi_intr_index].mask = val;
        //     break;
        // case DESIGNWARE_PCIE_MSI_INTR0_STATUS:
        //     port->msi.intr[msi_intr_index].status ^= val;
        //     if (!port->msi.intr[msi_intr_index].status) {
        //         qemu_set_irq(host->pci.msi, 0);
        //     }
        //     break;
        case 0x828 ... 0x924: {
            // offset 0x82c value 0x80010100
            int sid_0                     = (addr - 0x828) >> 2;
            int sid_and_rid_nonzero       = (data >> 31) & 1;
            int sid_1                     = (data >> 16) & 0xf;
            int rid                       = (data >> 0) & UINT16_MAX;
            port->port_rid_sid_map[sid_0] = data;
            DPRINTF("%s: Port %u: sid_rid_map: sid_and_rid_nonzero: %u sid_0: %u"
                    " sid_1: %u rid: 0x%x\n",
                    __func__, port->bus_nr, sid_and_rid_nonzero, sid_0, sid_1, rid);
            break;
        }
        case 0x4020:
            // enableCounters 0x3
            // captureCounters 0x7
            // break;
            goto jump_default;
#if 1
        case 0x84:    // unknown_0
            // break;
            goto jump_default;
        case 0x130:    // unknown_1
            // break;
            goto jump_default;
        case 0x140:    // unknown_2
            // break;
            goto jump_default;
        case 0x144:    // unknown_3
            // break;
            goto jump_default;
        case 0x148:    // unknown_4
            // break;
            goto jump_default;
        case 0x21c:    // unknown_5
            // break;
            goto jump_default;
        case 0x808:    // unknown_6
            // break;
            goto jump_default;
        case 0x81c:    // unknown_7
            // break;
            goto jump_default;
        case 0x824:    // unknown_8
            // break;
            goto jump_default;
#endif
        default:
        jump_default:
            DPRINTF("%s: Port %u: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    port->bus_nr, addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_port_config_ops = {
    .read       = apple_pcie_port_config_read,
    .write      = apple_pcie_port_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_port_config_ltssm_debug_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEPort* port = opaque;
    uint32_t       val  = 0;

// #ifdef ENABLE_CPU_DUMP_STATE
#if 0
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x20:
            // break;
            goto jump_default;
        case 0x30: val = port->port_ltssm_status; break;
        default:
        jump_default:
            DPRINTF("%s: Port %u: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x"
                    "\n",
                    __func__, port->bus_nr, addr, val);
            break;
    }

    DPRINTF("%s: Port %u: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, port->bus_nr, addr, val);
    return val;
}

static void apple_pcie_port_config_ltssm_debug_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEPort* port = opaque;

    DPRINTF("%s: Port %u: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, port->bus_nr, addr,
            data);
    switch (addr) {
        case 0x10:
            // break;
            goto jump_default;
        case 0x14:
            // break;
            goto jump_default;
        case 0x1c:
            // break;
            goto jump_default;
        case 0x20:
            // break;
            goto jump_default;
        case 0x38:
            if ((data & 1) != 0) { port->port_ltssm_status = 0x1000; }
            break;
        default:
        jump_default:
            DPRINTF("%s: Port %u: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    port->bus_nr, addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_port_config_ltssm_debug_ops = {
    .read       = apple_pcie_port_config_ltssm_debug_read,
    .write      = apple_pcie_port_config_ltssm_debug_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_port_phy_glue_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEPort* port = opaque;
    uint32_t       val  = 0;

// #ifdef ENABLE_CPU_DUMP_STATE
#if 0
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x0:    // for port refclk buffer ; copied from
                     // apple_pcie_host_root_phy_read
            val = port->port_refclk_buffer_enabled;
            break;
        default:
        jump_default:
            DPRINTF("%s: Port %u: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x"
                    "\n",
                    __func__, port->bus_nr, addr, val);
            break;
    }

    DPRINTF("%s: Port %u: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, port->bus_nr, addr, val);
    return val;
}

static void apple_pcie_port_phy_glue_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEPort* port = opaque;

    DPRINTF("%s: Port %u: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, port->bus_nr, addr,
            data);
    switch (addr) {
        case 0x0:    // for port refclk buffer ; copied from
                     // apple_pcie_host_root_phy_write
            DPRINTF("port_phy: refclk_buffer_enabled before == 0x%x\n", port->port_refclk_buffer_enabled);
            if ((data & (1 << 0)) != 0) { data |= (1 << 2); }
            if ((data & (1 << 1)) != 0) {
                // was: "yes, REALLY bit1"
                // Somebody at Apple apparently fucked up in iOS 14 and decided
                // to use the request bit for the response as well.
                // the correct choice after all was to use bit3 (like in iOS 16),
                // just like in apple_pcie_host_root_phy_write
                data |= (1 << 3);    // wrong: iOS 14 bit1, correct: iOS 16 bit3
            }
            port->port_refclk_buffer_enabled = data;
            DPRINTF("port_phy: refclk_buffer_enabled after == 0x%x\n", port->port_refclk_buffer_enabled);
            break;
        default:
        jump_default:
            DPRINTF("%s: Port %u: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    port->bus_nr, addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_port_phy_glue_ops = {
    .read       = apple_pcie_port_phy_glue_read,
    .write      = apple_pcie_port_phy_glue_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static uint64_t apple_pcie_port_phy_ip_read(void* opaque, hwaddr addr, unsigned size)
{
    ApplePCIEPort* port = opaque;
    uint32_t       val  = 0;

// #ifdef ENABLE_CPU_DUMP_STATE
#if 0
    cpu_dump_state(CPU(first_cpu), stderr, CPU_DUMP_CODE);
#endif

    switch (addr) {
        case 0x400:
            // break;
            goto jump_default;
        case 0xa3c:
            // break;
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: Port %u: READ DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x%x"
                    "\n",
                    __func__, port->bus_nr, addr, val);
            break;
    }

    DPRINTF("%s: Port %u: READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", __func__, port->bus_nr, addr, val);
    return val;
}

static void apple_pcie_port_phy_ip_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    ApplePCIEPort* port = opaque;

    DPRINTF("%s: Port %u: WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__, port->bus_nr, addr,
            data);
    switch (addr) {
        case 0x400:
            // break;
            goto jump_default;
        case 0xa3c:
            // break;
            goto jump_default;
        default:
        jump_default:
            DPRINTF("%s: Port %u: WRITE DEFAULT @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", __func__,
                    port->bus_nr, addr, data);
            break;
    }
}

static const MemoryRegionOps apple_pcie_port_phy_ip_ops = {
    .read       = apple_pcie_port_phy_ip_read,
    .write      = apple_pcie_port_phy_ip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static const char* apple_pcie_host_root_bus_path(PCIHostState* host_bridge, PCIBus* rootbus) { return "0000:00"; }

static void apple_pcie_host_reset_enter(Object* obj, ResetType type)
{
    ApplePCIEHost* host = APPLE_PCIE_HOST(obj);

    host->root_phy_enabled           = 0x0;
    host->root_refclk_buffer_enabled = 0x0;
    memset(host->root_common_regs, 0, sizeof(host->root_common_regs));
}

static ApplePCIEPort* apple_pcie_create_port(AppleDTNode* node, uint32_t bus_nr, qemu_irq irq, PCIBus* bus,
                                             ApplePCIEHost* host)
{
    // DeviceState *dev;
    PCIDevice*   pci_dev;
    AppleDTNode* child;
    AppleDTProp* prop;
    // ApplePCIEHost *s;
    char               link_name[16];
    char               bridge_node_name[16];
    char               dart_name[16];
    AppleDARTState*    dart;
    IOMMUMemoryRegion* dma_mr = NULL;
    // uint32_t *armfunc;
    // int clkreq_gpio_id = 0, clkreq_gpio_value = 0;
    int device_id = 0;    //, maximum_link_speed = 0;
    snprintf(link_name, sizeof(link_name), "pcie.bridge%u", bus_nr);
    snprintf(dart_name, sizeof(dart_name), "dart-apcie%u", bus_nr);
    // char link_secbus_name[32];
    // snprintf(link_secbus_name, sizeof(link_secbus_name),
    // "pcie.bridge%u.secbus", bus_nr);
    snprintf(bridge_node_name, sizeof(bridge_node_name), "pci-bridge%u", bus_nr);
    child = apple_dt_get_node(node, bridge_node_name);
#if 0
    if (child != NULL) {
        // only on S8000
        assert_nonnull(child);

        prop = apple_dt_find_prop(child, "function-clkreq");
        assert_nonnull(prop);
        if (prop->length == 16) {
            armfunc = (uint32_t *)prop->data;
            clkreq_gpio_id = armfunc[2];
            clkreq_gpio_value = armfunc[3] != 2; // == 0
            ////clkreq_gpio_value = armfunc[3] == 2; // != 0
        }
    }
#endif
    pci_dev = pci_new(PCI_DEVFN(bus_nr, 0), TYPE_APPLE_PCIE_PORT);
    object_property_add_child(qdev_get_machine(), link_name, OBJECT(pci_dev));
    ApplePCIEPort* port = APPLE_PCIE_PORT(pci_dev);
    port->host          = host;

    if (child == NULL) { port->manual_enable = false; }
    else {
        prop                = apple_dt_get_prop(child, "manual-enable");
        port->manual_enable = (prop != NULL);
    }

    if (host->pcie->chip_id == 0x8020 || host->pcie->chip_id == 0x8030) {
        // device_id = 0x1002;
        // device_id = 0x1003;
        device_id = 0x100c;    // for t8030, according to srd.cx's iotree
    }
    else if (child != NULL) {
        device_id = (prop == NULL) ? 0x1003 : 0x1004;
    }

#if 1
    if (child != NULL) {
        // set those maximum-link-speed values inside the respective device:
        // S8000: nvme/s3e: 5GT, wlan: 2_5GT, baseband: 2_5GT
        // T8030: nvme/ans: N/A, wlan/bluetooth: 5GT, baseband: 5GT
        // widths:
        // T8030: baseband: X2 (not shown in the device tree, but maybe
        // implied by the speed value)
        port->maximum_link_speed = apple_dt_get_prop_u32(child, "maximum-link-speed", &error_fatal);

        // TODO: manual-enable/function-pcie_port_control
        // apple_dt_remove_prop_named(child, "manual-enable");
        // apple_dt_remove_prop_named(child, "manual-enable-s2r");

        // apple_dt_set_prop_u32(child, "ignore-link-speed-mismatch", 1);
        ////apple_dt_set_prop_u32(child, "ignore-link-width-mismatch", 1);
        //////apple_dt_remove_prop_named(child, "maximum-link-speed");
        // apple_dt_set_prop_u32(child, "no-refclk-gating", 1);
        // apple_dt_set_prop_u32(child, "allow-endpoint-reset", 0);
        ////apple_dt_set_prop_u32(child, "clkreq-wait-time", 100);
        // apple_dt_set_prop_u32(child, "ltssm-timeout", 0);

        // TODO: for S800x, until the GPIO shitshow gets fixed
        // apple_dt_remove_prop_named(child, "clkreq-wait-time");

        dart = APPLE_DART(object_property_get_link(OBJECT(qdev_get_machine()), dart_name, &error_fatal));
        assert_nonnull(dart);

        if (host->pcie->chip_id == 0x8015) { dma_mr = apple_dart_iommu_mr(dart, 0); }
        else {
            dma_mr = apple_dart_iommu_mr(dart, 1);
        }
        assert_nonnull(dma_mr);
        object_property_add_const_link(OBJECT(port), "dma-mr", OBJECT(dma_mr));
        port->dma_mr = MEMORY_REGION(dma_mr);

    #if 1
        qdev_init_gpio_out_named(DEVICE(port), &port->apcie_port_gpio_clkreq_irq, APCIE_PORT_GPIO_CLKREQ_OUT, 1);
        qdev_init_gpio_in_named(DEVICE(port), apcie_port_gpio_perst, APCIE_PORT_GPIO_PERST, 1);

        apple_dt_connect_function_prop_in_out_gpio(DEVICE(port), apple_dt_get_prop(child, "function-clkreq"),
                                                   APCIE_PORT_GPIO_CLKREQ_OUT);
        apple_dt_connect_function_prop_out_in_gpio(DEVICE(port), apple_dt_get_prop(child, "function-perst"),
                                                   APCIE_PORT_GPIO_PERST);
        #if 0
        apple_dt_connect_function_prop_out_in(DEVICE(dart), DEVICE(port), apple_dt_find_prop(child,
                                "function-dart_force_active"),
                                DART_DART_FORCE_ACTIVE);
        apple_dt_connect_function_prop_out_in(DEVICE(dart), DEVICE(port), apple_dt_find_prop(child,
                                "function-dart_request_sid"),
                                DART_DART_REQUEST_SID);
        apple_dt_connect_function_prop_out_in(DEVICE(dart), DEVICE(port), apple_dt_find_prop(child,
                                "function-dart_release_sid"),
                                DART_DART_RELEASE_SID);
        apple_dt_connect_function_prop_out_in(DEVICE(dart), DEVICE(port), apple_dt_find_prop(child,
                                "function-dart_self"),
                                DART_DART_SELF);
        #endif
        #if 1
                // apple_dt_remove_prop_named(child, "function-dart_force_active");
                // apple_dt_remove_prop_named(child, "function-dart_request_sid");
                // apple_dt_remove_prop_named(child, "function-dart_release_sid");
                // apple_dt_remove_prop_named(child, "function-dart_self");
            #if 0
        apple_dt_remove_prop_named(child, "pci-l1pm-control");
        apple_dt_remove_prop_named(child, "manual-enable-s2r");
        apple_dt_remove_prop_named(child, "pci-aspm-default");
        apple_dt_remove_prop_named(child, "pci-l1pm-control-postrom");
        apple_dt_remove_prop_named(child, "pci-l1pm-control-a0");
        //apple_dt_remove_prop_named(child, "");
            #endif
            #if 0
        apple_dt_set_prop_null(child, "pci-wake-l1pm-disable");
        child = apple_dt_get_node(child, "baseband-pcie");
        if (child != NULL) {
            apple_dt_set_prop_null(child, "pci-wake-l1pm-disable");
        }
            #endif
        #endif
    #endif
    }
    else {
        port->dma_mr             = NULL;
        port->maximum_link_speed = 0;
    }
#endif

    qdev_prop_set_uint32(DEVICE(port), "bus_nr", bus_nr);
    // qdev_prop_set_uint32(dev, "clkreq_gpio_id", clkreq_gpio_id);
    // qdev_prop_set_uint32(dev, "clkreq_gpio_value", clkreq_gpio_value);
    qdev_prop_set_uint32(DEVICE(port), "device_id", device_id);
    DPRINTF("%s: port->bus_nr == %u ; maximum_link_speed == %u\n", __func__, port->bus_nr, port->maximum_link_speed);
#if 1
    // for S8000
    if (port->maximum_link_speed == 1) {
        // qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_5);
        //  set speed to 8GT here, so qemu will start writing to PCI_EXP_LNKCAP2
        //  the actual device can/will/should set it to the proper value
        qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_8);
        qdev_prop_set_enum(DEVICE(port), "x-width", PCIE_LINK_WIDTH_1);
    }
    // for T8030 (and S800x)
    else if (port->maximum_link_speed == 2) {
        // qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_5);
        //  set speed to 8GT here, so qemu will start writing to PCI_EXP_LNKCAP2
        //  the actual device can/will/should set it to the proper value
        qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_8);
        qdev_prop_set_enum(DEVICE(port), "x-width", PCIE_LINK_WIDTH_2);
    }
    #if 0
    // TODO: for T8010
    else if (port->maximum_link_speed == 3) {
        // qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_5);
        //  set speed to 8GT here, so qemu will start writing to PCI_EXP_LNKCAP2
        //  the actual device can/will/should set it to the proper value
        qdev_prop_set_enum(DEVICE(port), "x-speed", PCIE_LINK_SPEED_8);
        qdev_prop_set_enum(DEVICE(port), "x-width", PCIE_LINK_WIDTH_2);
    }
    #endif
#endif

    // qdev_realize(DEVICE(dev), NULL, &error_abort);
    // sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    pci_realize_and_unref(pci_dev, bus, &error_fatal);

    return port;
}

SysBusDevice* apple_pcie_from_node(AppleDTNode* node, uint32_t chip_id)
{
    DeviceState*    dev;
    ApplePCIEState* s;
    SysBusDevice*   sbd;
    DeviceState*    host_dev;
    ApplePCIEHost*  host;
    ApplePCIEPort*  port;
    PCIHostState*   pci;
    // size_t i;
    int          i, j;
    int          mmio_index = 0;
    AppleDTProp* prop;
    uint64_t*    reg;
    char         temp_name[32];

    dev = qdev_new(TYPE_APPLE_PCIE);
    s   = APPLE_PCIE(dev);
    sbd = SYS_BUS_DEVICE(dev);

    host_dev = qdev_new(TYPE_APPLE_PCIE_HOST);
    object_property_add_child(qdev_get_machine(), "pcie.host", OBJECT(host_dev));
    host       = APPLE_PCIE_HOST(host_dev);
    host->pcie = s;
    s->host    = host;
    s->chip_id = chip_id;

    s->node = node;
    prop    = apple_dt_get_prop(s->node, "reg");
    assert_nonnull(prop);
    reg = (uint64_t*)prop->data;

    s->msi_vector_offset = apple_dt_get_prop_u32(s->node, "msi-vector-offset", &error_fatal);

    for (i = 0; i < ARRAY_SIZE(host->irqs); i++) { sysbus_init_irq(sbd, &host->irqs[i]); }
    for (i = 0; i < ARRAY_SIZE(host->msi_irqs); i++) { sysbus_init_irq(sbd, &host->msi_irqs[i]); }

    uint64_t common_index, port_index, port_count, port_entries, root_mappings, port_mappings;

    // t8015 is similar to both, s800x and t8020/t8030
    if (chip_id == 0x8000 || chip_id == 0x8003) {
        DPRINTF("%s: compatible check: use S8000(/S8003) mode\n", __func__);

        common_index  = 9;
        port_index    = 1;
        port_count    = 4;
        port_entries  = 2;
        root_mappings = 3;
        port_mappings = 1;
    }
    else if (chip_id == 0x8015) {
        DPRINTF("%s: compatible check: use T8015 mode\n", __func__);

        common_index  = 9;
        port_index    = 1;
        port_count    = 4;
        port_entries  = 2;
        root_mappings = 4;
        port_mappings = 1;
    }
    else if (chip_id == 0x8020 || chip_id == 0x8030) {
        DPRINTF("%s: compatible check: use T8030(/T8020) mode\n", __func__);

        common_index  = 1;
        port_index    = 6;
        port_count    = 4;
        port_entries  = 4;
        root_mappings = 5;
        port_mappings = 4;
    }
    else {
        assert_not_reached();
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(host_dev), &error_fatal);
    pci = PCI_HOST_BRIDGE(host_dev);
    for (i = 0; i < port_count; i++) { s->ports[i] = apple_pcie_create_port(node, i, host->irqs[i], pci->bus, host); }
    assert_cmpuint(reg[common_index * 2 + 1], <=, APCIE_COMMON_REGS_LENGTH);

    memory_region_init_io(&host->root_cfg, OBJECT(host), &apple_pcie_root_conf_ops, host, "root_cfg", reg[0 * 2 + 1]);
    sysbus_init_mmio(sbd, &host->root_cfg);
    sysbus_mmio_map(sbd, mmio_index++, reg[0 * 2]);

    memory_region_init_io(&host->root_common, OBJECT(host), &apple_pcie_root_common_ops, host, "root_common",
                          reg[(common_index + 0) * 2 + 1]);
    sysbus_init_mmio(sbd, &host->root_common);
    sysbus_mmio_map(sbd, mmio_index++, reg[(common_index + 0) * 2]);

    memory_region_init_io(&host->root_phy, OBJECT(host), &apple_pcie_host_root_phy_ops, host, "root_phy",
                          reg[(common_index + 1) * 2 + 1]);
    sysbus_init_mmio(sbd, &host->root_phy);
    sysbus_mmio_map(sbd, mmio_index++, reg[(common_index + 1) * 2]);

    if (root_mappings >= 4) {
        memory_region_init_io(&host->root_phy_ip, OBJECT(host), &apple_pcie_host_root_phy_ip_ops, host, "root_phy_ip",
                              reg[(common_index + 2) * 2 + 1]);
        sysbus_init_mmio(sbd, &host->root_phy_ip);
        sysbus_mmio_map(sbd, mmio_index++, reg[(common_index + 2) * 2]);
    }
    if (root_mappings >= 5) {
        memory_region_init_io(&host->root_axi2af, OBJECT(host), &apple_pcie_host_root_axi2af_ops, host, "root_axi2af",
                              reg[(common_index + 3) * 2 + 1]);
        sysbus_init_mmio(sbd, &host->root_axi2af);
        sysbus_mmio_map(sbd, mmio_index++, reg[(common_index + 3) * 2]);
    }

    // the ports have to come later, as root and port phy's will overlap
    // otherwise (ports need to take preference)
    for (i = 0; i < port_count; i++) {
        port = s->ports[i];
        if (port == NULL) { continue; }
        // for (j = 0; j < 8; j++) {
        //     sysbus_init_irq(sbd, &port->msi_irqs[j]);
        // }
        snprintf(temp_name, sizeof(temp_name), "port%u_config", i);
        memory_region_init_io(&port->port_cfg, OBJECT(port), &apple_pcie_port_config_ops, port, temp_name,
                              reg[(port_index + (i * port_entries) + 0) * 2 + 1]);
        sysbus_init_mmio(sbd, &port->port_cfg);
        sysbus_mmio_map(sbd, mmio_index++, reg[(port_index + (i * port_entries) + 0) * 2 + 0]);
        if (chip_id == 0x8020 || chip_id == 0x8030) {
            snprintf(temp_name, sizeof(temp_name), "port%u_config_ltssm_debug", i);
            memory_region_init_io(&port->port_config_ltssm_debug, OBJECT(port), &apple_pcie_port_config_ltssm_debug_ops,
                                  port, temp_name, reg[(port_index + (i * port_entries) + 1) * 2 + 1]);
            sysbus_init_mmio(sbd, &port->port_config_ltssm_debug);
            sysbus_mmio_map(sbd, mmio_index++, reg[(port_index + (i * port_entries) + 1) * 2 + 0]);

            snprintf(temp_name, sizeof(temp_name), "port%u_phy_glue", i);
            memory_region_init_io(&port->port_phy_glue, OBJECT(port), &apple_pcie_port_phy_glue_ops, port, temp_name,
                                  reg[(port_index + (i * port_entries) + 2) * 2 + 1]);
            sysbus_init_mmio(sbd, &port->port_phy_glue);
            sysbus_mmio_map(sbd, mmio_index++, reg[(port_index + (i * port_entries) + 2) * 2 + 0]);

            snprintf(temp_name, sizeof(temp_name), "port%u_phy_ip", i);
            memory_region_init_io(&port->port_phy_ip, OBJECT(port), &apple_pcie_port_phy_ip_ops, port, temp_name,
                                  reg[(port_index + (i * port_entries) + 3) * 2 + 1]);
            sysbus_init_mmio(sbd, &port->port_phy_ip);
            sysbus_mmio_map(sbd, mmio_index++, reg[(port_index + (i * port_entries) + 3) * 2 + 0]);
        }
        else {
            snprintf(temp_name, sizeof(temp_name), "port%u_phy", i);
            create_unimplemented_device(temp_name, reg[(port_index + (i * port_entries) + 1) * 2 + 0],
                                        reg[(port_index + (i * port_entries) + 1) * 2 + 1]);
        }
    }

    if (chip_id == 0x8015 || chip_id == 0x8020 || chip_id == 0x8030) {
        pci_set_power(PCI_DEVICE(s->ports[0]), false);
        pci_set_power(PCI_DEVICE(s->ports[1]), false);
        // pci_set_power(PCI_DEVICE(s->ports[2]), false);
        // pci_set_power(PCI_DEVICE(s->ports[3]), false);
    }
    else {
        pci_set_power(PCI_DEVICE(s->ports[3]), false);
    }

    DPRINTF("%s: reg[1] == 0x" HWADDR_FMT_plx "\n", __func__, reg[1]);

    return sbd;
}

static void apple_pcie_port_reset_hold(Object* obj, ResetType type)
{
    PCIERootPortClass* rpc      = PCIE_ROOT_PORT_GET_CLASS(obj);
    ApplePCIEPort*     port     = APPLE_PCIE_PORT(obj);
    PCIDevice*         pci_dev  = PCI_DEVICE(obj);
    PCIESlot*          slot     = PCIE_SLOT(pci_dev);
    uint8_t*           pci_conf = pci_dev->config;
    uint32_t           config;
    uint8_t*           exp_cap;
    uint32_t           val;

    DPRINTF("%s: port->bus_nr == %u ; resetType == %u ; pci_dev->enabled == %u\n", __func__, port->bus_nr, type,
            pci_dev->enabled);
    if (!port->skip_reset_clear) {
        if (rpc->parent_phases.hold) { rpc->parent_phases.hold(obj, type); }
        bool is_port_enabled             = (port->port_cfg_port_config & 1) != 0;
        port->port_ltssm_enable          = 0x0;
        port->port_pme_to_ack            = 0x0;
        port->port_last_interrupt        = 0x0;
        port->port_interrupt_mask        = 0x0;
        port->port_hotreset              = 0x0;
        port->port_cfg_port_config       = 0x0;
        port->port_cfg_refclk_config     = 0x0;
        port->port_cfg_rootport_perst    = 0x0;
        port->port_refclk_buffer_enabled = 0x0;
        port->port_msiVectors            = 0x0;
        port->port_msiUnknown0           = 0x0;
        port->port_linkcdmsts            = 0x0;
        memset(port->port_rid_sid_map, 0, sizeof(port->port_rid_sid_map));
        port->port_ltssm_status = 0x0;
        port->is_link_up        = false;
        port->is_link_in_l2     = false;

        memory_region_set_enabled(&port->msi.iomem, false);
        port->gpio_perst_val  = 0;
        port->gpio_clkreq_val = 0;
        apcie_port_gpio_set_clkreq(DEVICE(port), 0);
        // apcie_port_gpio_set_clkreq(DEVICE(port), 1);
        if (port->manual_enable) { port_devices_set_power(port, false); }
    }
    port->skip_reset_clear = false;
}

static AddressSpace* apple_pcie_host_set_iommu(PCIBus* bus, void* opaque, int devfn)
{
    ApplePCIEPort* port = opaque;

    return &port->dma_as;
}

static const PCIIOMMUOps apple_pcie_iommu_ops = {
    .get_address_space = apple_pcie_host_set_iommu,
};

static void apple_pcie_port_realize(DeviceState* dev, Error** errp)
{
    const hwaddr   dummy_offset = 0;
    const uint64_t dummy_size   = 4;
    Object*        obj;
#if 0
    BusState *bus = qdev_get_parent_bus(DEVICE(pci_dev));
    ApplePCIEHost *s = APPLE_PCIE_HOST(bus->parent);
    pci_bridge_initfn(pci_dev, TYPE_PCI_BUS);
    pcie_cap_init(pci_dev, 0x70, PCI_EXP_TYPE_ROOT_PORT, 0, &error_fatal);
#endif

#if 0
    if (pcie_endpoint_cap_v1_init(pci_dev, 0x80) < 0) {
        error_setg(errp, "Failed to initialize PCIe capability");
    }
#endif
    // return;

    PCIERootPortClass* rpc  = PCIE_ROOT_PORT_GET_CLASS(dev);
    ApplePCIEPort*     port = APPLE_PCIE_PORT(dev);
    PCIBus*            bus  = PCI_BUS(qdev_get_parent_bus(dev));
    PCIDevice*         pci  = PCI_DEVICE(dev);
    PCIESlot*          slot = PCIE_SLOT(pci);

    // PCIHostState *pci = PCI_HOST_BRIDGE(dev);
    // ApplePCIEHost *s = APPLE_PCIE_HOST(dev);
    // PCIExpressHost *pex = PCIE_HOST_BRIDGE(dev);
    // pcie_host_mmcfg_init(pex, 32 * 1024 * 1024);
    // pcie_host_mmcfg_init(pex, PCIE_MMCFG_SIZE_MAX);

    // MemoryRegion *host_mem = get_system_memory();
    //  MemoryRegion *address_space = &host->pci.memory;
    PCIBridge* br = PCI_BRIDGE(pci);
    br->bus_name  = "apple-pcie";

    /* Set unique chassis/slot values for the root port */
    qdev_prop_set_uint8(dev, "chassis", 0);
    qdev_prop_set_uint16(dev, "slot", port->bus_nr);

    rpc->parent_realize(dev, &error_fatal);

    pci_config_set_device_id(pci->config, port->device_id);
    // pci_config_set_interrupt_pin(pci->config, 0);

    // pci_set_word(pci->config + PCI_COMMAND,
    //              PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
#if 1
    // this leads to baseband triggering pci-bridge3, like it should
    pci_config_set_interrupt_pin(pci->config, 1);
    // this leads to baseband triggering pci-bridge2 instead of pci-bridge3
    // pci_config_set_interrupt_pin(pci->config, port->bus_nr + 1);
#endif

#if 0
    pci_config_set_interrupt_pin(pci->config, 1);
    pci_set_byte(pci->config + PCI_INTERRUPT_LINE, 0xff);
    // pci->config[PCI_INTERRUPT_LINE] = 0xff;
    pci->wmask[PCI_INTERRUPT_LINE] = 0x0;
    // pci_default_write_config(pci, PCI_INTERRUPT_LINE, 0xff, 1);
#endif
    // pci_bridge_initfn(pci, TYPE_PCIE_BUS);
    // pci_bridge_initfn(pci, TYPE_PCI_BUS); // this avoids a qemu windows hack

    // pcie_port_init_reg(pci);

    // v2, offset 0x70, root_port
    // pcie_cap_init(pci, 0x70, PCI_EXP_TYPE_ROOT_PORT, 0, &error_fatal);
    // pcie_cap_init(pci, 0, PCI_EXP_TYPE_ROOT_PORT, 0, &error_fatal);
#if 0
    pci_bridge_initfn(pci_dev, TYPE_PCI_BUS);

    if (pcie_endpoint_cap_v1_init(pci_dev, 0x80) < 0) {
        error_setg(errp, "Failed to initialize PCIe capability");
    }
#endif

    // deverr enabled
    pcie_cap_deverr_init(pci);

    // pci_pm_init(pci, 0x40, &error_fatal);
    pci_pm_init(pci, 0, &error_fatal);

    bool is_bridge = IS_PCI_BRIDGE(pci);
    DPRINTF("%s: is_bridge == %u\n", __func__, is_bridge);

#if 0
    // sizes: 0x50 for the bridges and qualcomm baseband,
    // 0x3c for broadcom wifi, 0x48 for nvme
    // versions: 1 for broadcom wifi, 2 for the rest
    ////pcie_aer_init(pci_dev, 1, 0x100, PCI_ERR_SIZEOF, &error_fatal);
    // pcie_aer_init(pci_dev, PCI_ERR_VER, 0x100, 0x50, &error_fatal);
#endif

#if 0
    msix_init_exclusive_bar(pci, 1, 0, &error_fatal);
    msix_vector_use(pci, 0);
#endif
#if 1
    // Warning: pcie_endpoint_cap_init inside endpoint devices can and will
    // override this!
    DPRINTF("%s: slot->width == %u ; slot->speed == %u\n", __func__, slot->width, slot->speed);
    pcie_cap_fill_link_ep_usp(pci, slot->width, slot->speed);
#endif

#if 1
    if (port->dma_mr) {
        address_space_init(&port->dma_as, port->dma_mr, "pcieport.dma-as");
        PCIBus* sec_bus = pci_bridge_get_sec_bus(br);
        pci_setup_iommu(sec_bus, &apple_pcie_iommu_ops, port);
    #if 0
        if (pci->config[PCI_SECONDARY_BUS] != 0) {
            PCIBus *child_bus = pci_find_bus_nr(bus,
                                                pci->config[PCI_SECONDARY_BUS]);
            if (child_bus) {
                pci_setup_iommu(child_bus, &apple_pcie_iommu_ops, port);
            }
        }
    #endif
        // pci_setup_iommu(pci_get_bus(pci), &apple_pcie_iommu_ops, port);
        // pci_setup_iommu(bus, &apple_pcie_iommu_ops, port);
        // // PCIHostState *pci_host = PCI_HOST_BRIDGE(port->host);
        // // pci_setup_iommu(pci_host->bus, &apple_pcie_iommu_ops, port);
    }
#endif
#if 1
    memory_region_init_io(&port->msi.iomem, OBJECT(port), &apple_pcie_port_msi_ops, port, "pcie-msi", 0x4);
    /*
     * We initially place MSI interrupt I/O region at address 0 and
     * disable it. It'll be later moved to correct offset and enabled
     * in apple_pcie_port_update_msi_mapping() as a part of
     * initialization done by guest OS
     */
    MemoryRegion* address_space;
    if (port->dma_mr) { address_space = port->dma_mr; }
    else {
        address_space = get_system_memory();
    }
    memory_region_add_subregion(address_space, dummy_offset, &port->msi.iomem);
    memory_region_set_enabled(&port->msi.iomem, false);
    // pci_setup_iommu(pci_get_bus(pci), &apple_pcie_iommu_ops, port);
    // pci_setup_iommu(bus, &apple_pcie_iommu_ops, port);
    // // PCIHostState *pci_host = PCI_HOST_BRIDGE(port->host);
    // // pci_setup_iommu(pci_host->bus, &apple_pcie_iommu_ops, port);
#endif
    port->skip_reset_clear = false;
}

static int apple_pcie_port_interrupts_init(PCIDevice* d, Error** errp)
{
    int rc;
    DPRINTF("%s: entered function\n", __func__);

    // offset 0x50, only 1 vector for the first bridge, 64-bit enabled,
    // per-vector-mask disabled
    // rc = msi_init(d, 0x50, 1, true, false, errp);
    rc = msi_init(d, 0x50, 8, true, false, errp);
    // rc = msi_init(d, 0x50, 8, true, true, errp);
    if (rc < 0) {
        DPRINTF("%s: msi_init(d, ...) call failed == %d\n", __func__, rc);
        assert(rc == -ENOTSUP);
    }

    return rc;
}

static void apple_pcie_port_interrupts_uninit(PCIDevice* d) { msi_uninit(d); }

/*
 * If two MSI vector are allocated, Advanced Error Interrupt Message Number
 * is 1. otherwise 0.
 * 17.12.5.10 RPERRSTS,  32:27 bit Advanced Error Interrupt Message Number.
 */
static uint8_t apple_pcie_aer_vector(const PCIDevice* d)
{
    DPRINTF("%s: msi_nr_vectors_allocated(d) == %u\n", __func__, msi_nr_vectors_allocated(d));
    switch (msi_nr_vectors_allocated(d)) {
        case 1 : return 0;
        case 2 : return 1;
        case 4 :
        case 8 :
        case 16:
        case 32:
        default: break;
    }
    abort();
    return 0;
}

static const Property apple_pcie_port_props[] = {
    DEFINE_PROP_UINT32("bus_nr", ApplePCIEPort, bus_nr, 0),
    DEFINE_PROP_UINT32("device_id", ApplePCIEPort, device_id, 0),
    DEFINE_PROP_PCIE_LINK_SPEED("x-speed", PCIESlot, speed, PCIE_LINK_SPEED_8),
    DEFINE_PROP_PCIE_LINK_WIDTH("x-width", PCIESlot, width, PCIE_LINK_WIDTH_2),
};

static void apple_pcie_port_class_init(ObjectClass* klass, const void* data)
{
    PCIDeviceClass*    k   = PCI_DEVICE_CLASS(klass);
    DeviceClass*       dc  = DEVICE_CLASS(klass);
    ResettableClass*   rc  = RESETTABLE_CLASS(klass);
    PCIERootPortClass* rpc = PCIE_ROOT_PORT_CLASS(klass);
    // HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(klass);

    dc->desc     = "Apple PCIE Root Port";
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    // s8000: 0x1003 for the bridge ; + 1 if manual-enable property exists?
    // t8030: 0x1002? 0x100c?
    k->device_id = 0;
    k->revision  = 0x1;

    device_class_set_props(dc, apple_pcie_port_props);
    device_class_set_parent_realize(dc, apple_pcie_port_realize, &rpc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, apple_pcie_port_reset_hold, NULL, &rpc->parent_phases);
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;

    rpc->exp_offset = 0x70;
    rpc->aer_offset = 0x100;
    rpc->aer_vector = apple_pcie_aer_vector;
    ////rpc->acs_offset = ;

    rpc->interrupts_init   = apple_pcie_port_interrupts_init;
    rpc->interrupts_uninit = apple_pcie_port_interrupts_uninit;
    k->config_read         = apple_pcie_port_bridge_config_read;
    k->config_write        = apple_pcie_port_bridge_config_write;

    dc->hotpluggable = false;
}

static void apple_pcie_host_realize(DeviceState* dev, Error** errp)
{
    PCIHostState*  pci = PCI_HOST_BRIDGE(dev);
    ApplePCIEHost* s   = APPLE_PCIE_HOST(dev);
    // PCIExpressHost *pex = PCIE_HOST_BRIDGE(dev);
    // pcie_host_mmcfg_init(pex, PCIE_MMCFG_SIZE_MAX);

    /* MMIO region */
    memory_region_init(&s->mmio, OBJECT(s), "mmio", UINT64_MAX);
    /* dummy PCI I/O region (not visible to the CPU) */
    memory_region_init(&s->io, OBJECT(s), "io", 16);

    /* interrupt out */
    qdev_init_gpio_out_named(dev, s->irqs, "interrupt_pci", 4);

#if 1
    pci->bus = pci_register_root_bus(dev, "apcie", apple_pcie_set_irq, pci_swizzle_map_irq_fn, s, &s->mmio, &s->io, 0,
                                     4, TYPE_APPLE_PCIE_ROOT_BUS);
#endif
#if 0
    pci->bus = pci_register_root_bus(dev, "apcie", apple_pcie_set_irq,
                                     apple_pcie_map_irq, s, &s->mmio,
                                     &s->io, 0, 4, TYPE_APPLE_PCIE_ROOT_BUS);
#endif
    // pci->bus->flags |= PCI_BUS_EXTENDED_CONFIG_SPACE;
}

static void apple_pcie_host_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass*    rc = RESETTABLE_CLASS(klass);
    PCIHostBridgeClass* hc = PCI_HOST_BRIDGE_CLASS(klass);
    DeviceClass*        dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_pcie_host_reset_enter;

    hc->root_bus_path = apple_pcie_host_root_bus_path;
    dc->realize       = apple_pcie_host_realize;
    // dc->fw_name = "pci";

    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static void apple_pcie_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->desc           = "Apple PCI Express (APCIE)";
    dc->user_creatable = false;
    dc->hotpluggable   = false;
}

static const TypeInfo apple_pcie_types[] = {
    {
        .name   = TYPE_APPLE_PCIE_ROOT_BUS,
        .parent = TYPE_PCIE_BUS,
        OBJECT_TYPE_INSTANCE(ApplePCIERootBus),
        .class_init = apple_pcie_root_bus_class_init,
    },
    {
        .name   = TYPE_APPLE_PCIE_HOST,
        .parent = TYPE_PCIE_HOST_BRIDGE,
        OBJECT_TYPE_INSTANCE(ApplePCIEHost),
        .class_init = apple_pcie_host_class_init,
    },
    {
        .name   = TYPE_APPLE_PCIE_PORT,
        .parent = TYPE_PCIE_ROOT_PORT,
        OBJECT_TYPE_INSTANCE(ApplePCIEPort),
        .class_init = apple_pcie_port_class_init,
    },
    {
        .name   = TYPE_APPLE_PCIE,
        .parent = TYPE_SYS_BUS_DEVICE,
        OBJECT_TYPE_INSTANCE(ApplePCIEState),
        .class_init = apple_pcie_class_init,
    },
};

DEFINE_TYPES(apple_pcie_types)
