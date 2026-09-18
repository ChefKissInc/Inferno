/*
 * Apple SMC.
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
#include "hw/misc/a7iop/base.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_SMC_IOP "apple-smc"
OBJECT_DECLARE_TYPE(AppleSMC, AppleSMCClass, APPLE_SMC_IOP)

#define SMC_KEY_FORMAT(v) (((v) >> 24) & 0xFF), (((v) >> 16) & 0xFF), (((v) >> 8) & 0xFF), ((v) & 0xFF)

typedef enum
{
    SMC_KEY_TYPE_FLAG   = 'flag',
    SMC_KEY_TYPE_HEX    = 'hex_',
    SMC_KEY_TYPE_SINT8  = 'si8 ',
    SMC_KEY_TYPE_SINT16 = 'si16',
    SMC_KEY_TYPE_SINT32 = 'si32',
    SMC_KEY_TYPE_SINT64 = 'si64',
    SMC_KEY_TYPE_UINT8  = 'ui8 ',
    SMC_KEY_TYPE_UINT16 = 'ui16',
    SMC_KEY_TYPE_UINT32 = 'ui32',
    SMC_KEY_TYPE_UINT64 = 'ui64',
    SMC_KEY_TYPE_SP78   = 'Sp78',
    SMC_KEY_TYPE_CLH    = '{clh',
    SMC_KEY_TYPE_IOFLT  = 'ioft',
    SMC_KEY_TYPE_FLT    = 'flt ',
} SMCKeyType;

typedef enum
{
    SMC_NO_COMMAND = 0x0,
    SMC_READ_KEY   = 0x10,
    SMC_WRITE_KEY,
    SMC_GET_KEY_BY_INDEX,
    SMC_GET_KEY_INFO,
    SMC_GET_SRAM_ADDR = 0x17,
    SMC_NOTIFICATION,
    SMC_READ_KEY_PAYLOAD = 0x20,
} SMCCommand;

typedef enum
{
    SMC_RESULT_SUCCESS                = 0,
    SMC_RESULT_ERROR                  = 1,
    SMC_RESULT_COMM_COLLISION         = 0x80,
    SMC_RESULT_SPURIOUS_DATA          = 0x81,
    SMC_RESULT_BAD_COMMAND            = 0x82,
    SMC_RESULT_BAD_PARAMETER          = 0x83,
    SMC_RESULT_KEY_NOT_FOUND          = 0x84,
    SMC_RESULT_KEY_NOT_READABLE       = 0x85,
    SMC_RESULT_KEY_NOT_WRITABLE       = 0x86,
    SMC_RESULT_KEY_SIZE_MISMATCH      = 0x87,
    SMC_RESULT_FRAMING_ERROR          = 0x88,
    SMC_RESULT_BAD_ARGUMENT_ERROR     = 0x89,
    SMC_RESULT_TIMEOUT_ERROR          = 0xB7,
    SMC_RESULT_KEY_INDEX_RANGE_ERROR  = 0xB8,
    SMC_RESULT_BAD_FUNC_PARAMETER     = 0xC0,
    SMC_RESULT_EVENT_BUFF_WRONG_ORDER = 0xC4,
    SMC_RESULT_EVENT_BUFF_READ_ERROR  = 0xC5,
    SMC_RESULT_DEVICE_ACCESS_ERROR    = 0xC7,
    SMC_RESULT_UNSUPPORTED_FEATURE    = 0xCB,
    SMC_RESULT_SMB_ACCESS_ERROR       = 0xCC,
} SMCResult;

typedef enum
{
    SMC_EVENT_SYSTEM_STATE_NOTIFY = 0x70,
    SMC_EVENT_POWER_STATE_NOTIFY,
    SMC_EVENT_HID_EVENT_NOTIFY,
    SMC_EVENT_BATTERY_AUTH_NOTIFY,
    SMC_EVENT_GG_FW_UPDATE_NOTIFY,
    SMC_EVENT_PLIMIT_CHANGE = 0x80,
    SMC_EVENT_PCIE_READY    = 0x83,
} SMCEvent;

typedef enum
{
    SMC_SYSTEM_STATE_NOTIFY_PANIC_DETECTED  = 0x4,
    SMC_SYSTEM_STATE_NOTIFY_PREPARE_FOR_S0  = 0x6,
    SMC_SYSTEM_STATE_NOTIFY_SMC_PANIC_DONE  = 0xA,
    SMC_SYSTEM_STATE_NOTIFY_SYNC_RTC_OFFSET = 0xC,
    SMC_SYSTEM_STATE_NOTIFY_RESTART         = 0xF,
    SMC_SYSTEM_STATE_NOTIFY_MAC_EFI_FIRMWARE_UPDATED,
    SMC_SYSTEM_STATE_NOTIFY_QUIESCE_DEVICES,
    SMC_SYSTEM_STATE_NOTIFY_RESUME_DEVICES,
    SMC_SYSTEM_STATE_NOTIFY_GPU_PANEL_POWER_ON,
    SMC_SYSTEM_STATE_NOTIFY_SMC_PANIC_PROGRESS = 0x22,
} SMCSystemStateNotify;

typedef enum
{
    SMC_PANIC_CAUSE_UNKNOWN = 0,
    SMC_PANIC_CAUSE_MACOS_PANIC_DETECTED,
    SMC_PANIC_CAUSE_WATCHDOG_DETECTED,
    SMC_PANIC_CAUSE_X86_STRAIGHT_S5_SHUTDOWN_DETECTED,
    SMC_PANIC_CAUSE_X86_GLOBAL_RESET_DETECTED,
    SMC_PANIC_CAUSE_X86_CPU_CATERR_DETECTED,
    SMC_PANIC_CAUSE_X86_ACPI_PANIC_DETECTED,
    SMC_PANIC_CAUSE_X86_MACEFI_PANIC_DETECTED,
    SMC_PANIC_CAUSE_COUNT,
} SMCPanicCause;

typedef enum
{
    SMC_HID_EVENT_NOTIFY_BUTTON = 1,
    SMC_HID_EVENT_NOTIFY_INTERRUPT_VECTOR,
    SMC_HID_EVENT_NOTIFY_LID_STATE,
} SMCHIDEventNotify;

#define kSMCKeyEndpoint (0)

typedef enum
{
    SMC_ATTR_LE       = BIT(2),
    SMC_ATTR_FUNC     = BIT(4),
    SMC_ATTR_UNK_0x20 = BIT(5),
    SMC_ATTR_W        = BIT(6),
    SMC_ATTR_R        = BIT(7),
    SMC_ATTR_RW       = SMC_ATTR_R | SMC_ATTR_W,
    SMC_ATTR_RW_LE    = SMC_ATTR_RW | SMC_ATTR_LE,
    SMC_ATTR_R_LE     = SMC_ATTR_R | SMC_ATTR_LE,
    SMC_ATTR_W_LE     = SMC_ATTR_W | SMC_ATTR_LE,
} SMCKeyAttribute;

typedef enum
{
    SMC_HID_BUTTON_FORCE_SHUTDOWN = 0,
    SMC_HID_BUTTON_HOLD,
    SMC_HID_BUTTON_VOL_UP,
    SMC_HID_BUTTON_VOL_DOWN,
    SMC_HID_BUTTON_RINGER,
    SMC_HID_BUTTON_HELP,
    SMC_HID_BUTTON_MENU,
    SMC_HID_BUTTON_HELP_DOUBLE,
    SMC_HID_BUTTON_HALL_EFFECT_1,
    SMC_HID_BUTTON_HALL_EFFECT,
    SMC_HID_BUTTON_COUNT,
} AppleSMCHIDButton;

typedef struct SMCKey     SMCKey;
typedef struct SMCKeyData SMCKeyData;

/// `in` and `in_length` refer to the function payload on reads,
/// on writes it's the data being written to the key.
/// additionally, the structure located in `data` contains
/// the old data, not the new data.
///
/// `in` will be NULL when `in_length` is 0.
typedef SMCResult SMCKeyFunc(SMCKey* key, SMCKeyData* data, const void* in, uint8_t in_length);

typedef struct
{
    union
    {
        struct
        {
            uint8_t status;
            uint8_t tag_and_id;
            uint8_t length;
            uint8_t unk3;
            uint8_t response[4];
        };
        uint64_t raw;
    };
} QEMU_PACKED KeyResponse;

typedef struct
{
    uint8_t  size;
    uint32_t type;
    uint8_t  attr;
} QEMU_PACKED SMCKeyInfo;

struct SMCKey
{
    SMCKeyInfo  info;
    uint32_t    key;
    bool        is_sensor;
    void*       opaque;
    SMCKeyFunc* read;
    SMCKeyFunc* write;
    QTAILQ_ENTRY(SMCKey) next;
};

struct SMCKeyData
{
    uint32_t key;
    uint32_t size;
    void*    data;
    QTAILQ_ENTRY(SMCKeyData) next;
};

SysBusDevice* apple_smc_create(AppleDTNode* node, AppleA7IOPVersion version, uint64_t sram_size);

SMCKey*     apple_smc_get_key(AppleSMC* s, uint32_t key);
SMCKeyData* apple_smc_get_key_data(AppleSMC* s, uint32_t key);
void        apple_smc_add_key(AppleSMC* s, uint32_t key, uint8_t size, SMCKeyType type, SMCKeyAttribute attr,
                              const void* data);
void        apple_smc_add_sensor(AppleSMC* s, uint32_t key, uint8_t size, SMCKeyType type, SMCKeyAttribute attr,
                                 const void* data);
void        apple_smc_add_key_func(AppleSMC* s, uint32_t key, uint8_t size, SMCKeyType type, SMCKeyAttribute attr,
                                   void* opaque, SMCKeyFunc* reader, SMCKeyFunc* writer);
void        apple_smc_send_hid_button(AppleSMC* s, AppleSMCHIDButton button, bool state);
