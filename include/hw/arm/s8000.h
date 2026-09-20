/*
 * Apple S8000 SoC (iPhone 6s Plus).
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

#pragma once

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/a9.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/cpu/cluster.h"
#include "hw/sysbus.h"
#include "qapi/qapi-types-usb.h"
#include "system/kvm.h"

#define TYPE_APPLE_S8000 MACHINE_TYPE_NAME("s8000")

#define APPLE_S8000(obj) OBJECT_CHECK(AppleS8000MachineState, (obj), TYPE_APPLE_S8000)

typedef struct
{
    MachineClass parent;
} AppleS8000MachineClass;

typedef struct
{
    MachineState parent;
    hwaddr       armio_base;
    hwaddr       armio_size;

    unsigned long   dram_size;
    AppleA9*        cpus[A9_MAX_CPU];
    CPUClusterState cluster;
    SysBusDevice*   aic;
    SysBusDevice*   sep;
    MemoryRegion*   sys_mem;
    MachoHeader64*  kernel;
    MachoHeader64*  secure_monitor;
    uint8_t*        trustcache;
    char*           securerom;
    gsize           securerom_size;
    AppleDTNode*    device_tree;
    AppleBootInfo   boot_info;
    AppleVideoArgs  video_args;
    char*           trustcache_filename;
    char*           sep_rom_filename;
    char*           sep_fw_filename;
    char*           securerom_filename;
    uint32_t        build_version;
    uint64_t        ecid;
    Notifier        init_done_notifier;
    hwaddr          panic_base;
    hwaddr          panic_size;
    char            pmgr_reg[0x100000];
    bool            kaslr_off;
    bool            force_dfu;
    uint32_t        board_id;
    char*           usb_uplink_addr;
    USBUplinkType   usb_uplink_type;
} AppleS8000MachineState;
