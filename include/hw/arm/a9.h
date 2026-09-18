/*
 * Apple A9 CPU.
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
#include "hw/arm/dt.h"
#include "system/memory.h"
#include "cpu.h"

#define A9_MAX_CPU 2

#define TYPE_APPLE_A9 "apple-a9-cpu"
OBJECT_DECLARE_TYPE(AppleA9, AppleA9Class, APPLE_A9)

#define A9_CPREG_VAR_NAME(name) cpreg_##name
#define A9_CPREG_VAR_DEF(name)  uint64_t A9_CPREG_VAR_NAME(name)

typedef struct AppleA9Class
{
    /*< private >*/
    ARMCPUClass base_class;
    /*< public >*/

    DeviceRealize   parent_realize;
    DeviceUnrealize parent_unrealize;
    DeviceReset     parent_reset;
} AppleA9Class;

typedef struct AppleA9
{
    ARMCPU       parent_obj;
    MemoryRegion memory;
    MemoryRegion sysmem;
    uint32_t     cpu_id;
    uint32_t     phys_id;
    A9_CPREG_VAR_DEF(HID11);
    A9_CPREG_VAR_DEF(HID3);
    A9_CPREG_VAR_DEF(HID4);
    A9_CPREG_VAR_DEF(HID5);
    A9_CPREG_VAR_DEF(HID7);
    A9_CPREG_VAR_DEF(HID8);
    A9_CPREG_VAR_DEF(PMCR0);
    A9_CPREG_VAR_DEF(PMCR1);
    A9_CPREG_VAR_DEF(PMCR2);
    A9_CPREG_VAR_DEF(PMCR4);
    A9_CPREG_VAR_DEF(PMESR0);
    A9_CPREG_VAR_DEF(PMESR1);
    A9_CPREG_VAR_DEF(OPMAT0);
    A9_CPREG_VAR_DEF(OPMAT1);
    A9_CPREG_VAR_DEF(OPMSK0);
    A9_CPREG_VAR_DEF(OPMSK1);
    A9_CPREG_VAR_DEF(PMSR);
    A9_CPREG_VAR_DEF(PMC0);
    A9_CPREG_VAR_DEF(PMC1);
    A9_CPREG_VAR_DEF(PMTRHLD6);
    A9_CPREG_VAR_DEF(PMTRHLD4);
    A9_CPREG_VAR_DEF(PMTRHLD2);
    A9_CPREG_VAR_DEF(PMMMAP);
    A9_CPREG_VAR_DEF(SYS_LSU_ERR_STS);
    A9_CPREG_VAR_DEF(LSU_ERR_STS);
    A9_CPREG_VAR_DEF(LSU_ERR_ADR);
    A9_CPREG_VAR_DEF(L2C_ERR_INF);
    A9_CPREG_VAR_DEF(FED_ERR_STS);
    A9_CPREG_VAR_DEF(CYC_CFG);
    A9_CPREG_VAR_DEF(RMR_EL3);
    A9_CPREG_VAR_DEF(MMU_ERR_STS);
} AppleA9;

AppleA9* apple_a9_create(const char* name, uint32_t cpu_id, uint32_t phys_id);
AppleA9* apple_a9_from_node(AppleDTNode* node);
bool     apple_a9_cpu_is_asleep(AppleA9* acpu);
bool     apple_a9_cpu_is_off(AppleA9* acpu);
void     apple_a9_cpu_set_on(AppleA9* acpu);
