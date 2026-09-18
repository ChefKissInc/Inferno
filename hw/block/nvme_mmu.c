/*
 * Apple NVMe MMU Controller.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/block/nvme_mmu.h"
#include "hw/pci-host/apcie.h"
#include "hw/pci/msi.h"

#if 0
    #include "qemu/log.h"
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

static void apple_nvme_mmu_common_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size)
{
    AppleNVMeMMUState* s    = opaque;
    uint32_t*          mmio = &s->common_reg[addr >> 2];
    DPRINTF("apple_nvme_mmu: common reg WRITE @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n", addr, data);
    switch (addr) {
#if 0
    case NVME_APPLE_MAX_PEND_CMDS:
        val = NVME_APPLE_MAX_PEND_CMDS_VAL;
        break;
    case NVME_APPLE_BOOT_STATUS:
        val = NVME_APPLE_BOOT_STATUS_OK;
        break;
    case NVME_APPLE_BASE_CMD_ID:
        val = 0x6000;
        break;
#endif
#if 1
        case 0x4:    // e.g. reset
            if ((data & (1 << 16)) != 0) { data &= ~(1 << 16); }
            break;
#endif
        case 0x8:    // tcb physical lower
            break;
        case 0xc:    // tcb physical upper
            break;
        case 0x10:    // something physical lower ; maybe scratch buffer?
            break;
        case 0x14:    // something physical upper ; maybe scratch buffer?
            break;
        case 0x20:    // arg2_set_addrs: 0x1 if sync/set addresses, 0x0 if not
            break;
        case 0x24:    // arg3_sart_virtual_base_val_aligned
            break;
        case 0x28:    // arg4: sart_region_val1_plus_virtual_base_minus_0x100000
            break;
        case 0x2c:    // arg5: sart_region_val0 >> 20
            break;
        case 0x30:    // prp
            // (tag & 0x7f) << 23
            // (sector & 0x7ff) << 12
            break;
        default: break;
    }
    *mmio = data;
}

static uint64_t apple_nvme_mmu_common_reg_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleNVMeMMUState* s    = opaque;
    uint32_t*          mmio = &s->common_reg[addr >> 2];
    uint32_t           val  = *mmio;

    switch (addr) {
#if 0
    case NVME_APPLE_MAX_PEND_CMDS:
        val = NVME_APPLE_MAX_PEND_CMDS_VAL;
        break;
    case NVME_APPLE_BOOT_STATUS:
        val = NVME_APPLE_BOOT_STATUS_OK;
        break;
    case NVME_APPLE_BASE_CMD_ID:
        val = 0x6000;
        break;
#endif
        case 0x4:    // e.g. reset
            break;
        case 0x8:    // tcb physical lower
            break;
        case 0xc:    // tcb physical upper
            break;
        case 0x30:    // prp
            // (tag & 0x7f) << 23
            // (sector & 0x7ff) << 12
            break;
        case 0x34:    // error
            break;
        case 0x38:    // ignored?
            break;
        case 0x40:    // tag
            // (incrypto_tag & 0x7f) << 0
            // (outcrypto_tag & 0x7f) << 8
            break;
        default: break;
    }
    DPRINTF("apple_nvme_mmu: common reg READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n", addr, val);
    return val;
}

static const MemoryRegionOps apple_nvme_mmu_common_reg_ops = {
    .write                 = apple_nvme_mmu_common_reg_write,
    .read                  = apple_nvme_mmu_common_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned       = false,
};

static void apple_nvme_mmu_set_irq(void* opaque, int irq_num, int level)
{
    AppleNVMeMMUState* s = opaque;
    // qemu_set_irq(s->irq, level);
    // return;

    PCIDevice* pci_dev = PCI_DEVICE(s->nvme);
#if 0
    if (msi_enabled(pci_dev)) {
        if (level) {
            msi_notify(pci_dev, 0);
        }
    } else
#endif
    {
        pci_set_irq(pci_dev, level);
    }
}

static void apple_nvme_mmu_start(AppleNVMeMMUState* s)
{
    PCIDevice* pci_dev = PCI_DEVICE(s->nvme);
    uint32_t   config;

    config  = pci_default_read_config(pci_dev, PCI_COMMAND, 4);
    config |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_default_write_config(pci_dev, PCI_COMMAND, config, 4);

    assert_true(pci_dev->bus_master_enable_region.enabled);
}

SysBusDevice* apple_nvme_mmu_from_node(AppleDTNode* node, PCIBus* pci_bus)
{
    DeviceState*       dev;
    AppleNVMeMMUState* s;
    SysBusDevice*      sbd;
    AppleDTProp*       prop;
    uint64_t*          reg;
    PCIDevice*         pci_dev;

    dev = qdev_new(TYPE_APPLE_NVME_MMU);
    s   = APPLE_NVME_MMU(dev);
    sbd = SYS_BUS_DEVICE(dev);

    s->pci_bus = pci_bus;
    pci_dev    = pci_new(-1, TYPE_NVME);
    s->nvme    = NVME(pci_dev);

    object_property_set_str(OBJECT(s->nvme), "serial", "ChefKiss-NVMeMMU", &error_fatal);
    object_property_set_uint(OBJECT(s->nvme), "max_ioqpairs", 7, &error_fatal);
    object_property_set_uint(OBJECT(s->nvme), "mdts", 8, &error_fatal);
    object_property_set_uint(OBJECT(s->nvme), "logical_block_size", 4096, &error_fatal);
    object_property_set_uint(OBJECT(s->nvme), "physical_block_size", 4096, &error_fatal);
    object_property_add_child(OBJECT(dev), "nvme", OBJECT(s->nvme));

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(dev, apple_nvme_mmu_set_irq, "interrupt_pci", 1);
    memory_region_init_io(&s->common, OBJECT(dev), &apple_nvme_mmu_common_reg_ops, s, TYPE_APPLE_NVME_MMU ".common-reg",
                          reg[1]);
    sysbus_init_mmio(sbd, &s->common);

    return sbd;
}

static void apple_nvme_mmu_realize(DeviceState* dev, Error** errp)
{
    AppleNVMeMMUState* s = APPLE_NVME_MMU(dev);
    ApplePCIEPort*     port =
        APPLE_PCIE_PORT(object_property_get_link(OBJECT(qdev_get_machine()), "pcie.bridge0", &error_fatal));

    PCIDevice* pci_dev = PCI_DEVICE(s->nvme);
    qdev_realize(DEVICE(s->nvme), BUS(s->pci_bus), &error_fatal);
    assert_true(pci_is_express(pci_dev));
    pcie_endpoint_cap_init(pci_dev, 0);
    pcie_cap_deverr_init(pci_dev);

    // for root bridge: offset 0x50, only 1 vector for the first bridge, 64-bit
    // enabled, per-vector-mask disabled
    msi_init(pci_dev, 0, 1, true, false, &error_fatal);
    // msi_init(pci_dev, 0, 8, true, false, &error_fatal);

    pci_pm_init(pci_dev, 0, &error_fatal);
    // maximum-link-speed is 2 for s8000 and 3 for t8010
#if 1
    // warning: this will override the settings of the ports as well.
    // for S8000
    if (port->maximum_link_speed == 2) {
        // S8000's baseband actually seems to have 1, not 2. s3e has 2.
        pcie_cap_fill_link_ep_usp(pci_dev, QEMU_PCI_EXP_LNK_X2, QEMU_PCI_EXP_LNK_5GT);
    }
    #if 0
    // TODO?: for T8010
    if (port->maximum_link_speed == 3) {
        pcie_cap_fill_link_ep_usp(pci_dev, QEMU_PCI_EXP_LNK_X2,
                                  QEMU_PCI_EXP_LNK_8GT);
    }
    #endif
#endif
    pcie_aer_init(pci_dev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, &error_fatal);
    pci_config_set_class(pci_dev->config, PCI_CLASS_STORAGE_OTHER);
    apple_nvme_mmu_start(s);
}

static void apple_nvme_mmu_reset_enter(Object* obj, ResetType type)
{
    AppleNVMeMMUState* s = APPLE_NVME_MMU(obj);
    PCIDevice*         d = PCI_DEVICE(s->nvme);

    pcie_cap_deverr_reset(d);
}

static void apple_nvme_mmu_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = apple_nvme_mmu_reset_enter;

    dc->realize = apple_nvme_mmu_realize;
    dc->desc    = "Apple NVMe MMU";
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleNVMeMMUState, apple_nvme_mmu, APPLE_NVME_MMU, SYS_BUS_DEVICE)
