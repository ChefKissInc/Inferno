/*
 * Apple iPhone 11 Buttons
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
#include "hw/input/buttons.h"
#include "hw/misc/smc.h"
#include "qapi/error.h"
#include "qemu/lockable.h"
#include "system/runstate.h"
#include "ui/input.h"

#define TYPE_APPLE_BUTTONS "apple-buttons"
OBJECT_DECLARE_SIMPLE_TYPE(AppleButtonsState, APPLE_BUTTONS)

struct AppleButtonsState
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuMutex mutex;
    uint16_t  states;
};

static void apple_buttons_handle_event(DeviceState* dev, QemuConsole* src, InputEvent* evt)
{
    AppleButtonsState* s   = APPLE_BUTTONS(dev);
    InputKeyEvent*     key = evt->u.key.data;
    int                qcode;
    AppleSMCHIDButton  button;

    QEMU_LOCK_GUARD(&s->mutex);

    AppleSMC* smc = APPLE_SMC_IOP(object_property_get_link(OBJECT(qdev_get_machine()), "smc", &error_fatal));

    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);

    qcode = qemu_input_key_value_to_qcode(key->key);

    switch (qcode) {
        case Q_KEY_CODE_F1: button = SMC_HID_BUTTON_FORCE_SHUTDOWN; break;
        case Q_KEY_CODE_F2:
            if (key->down) {
                button = SMC_HID_BUTTON_RINGER;
                if (s->states & BIT32(button)) { s->states &= ~BIT32(button); }
                else {
                    s->states |= BIT32(button);
                }
                apple_smc_send_hid_button(smc, button, (s->states & BIT32(button)) != 0);
            }
            return;
        case Q_KEY_CODE_F3 : button = SMC_HID_BUTTON_VOL_DOWN; break;
        case Q_KEY_CODE_F4 : button = SMC_HID_BUTTON_VOL_UP; break;
        case Q_KEY_CODE_F5 : button = SMC_HID_BUTTON_HOLD; break;
        case Q_KEY_CODE_F6 : button = SMC_HID_BUTTON_MENU; break;
        case Q_KEY_CODE_F7 : button = SMC_HID_BUTTON_HELP; break;
        case Q_KEY_CODE_F8 : button = SMC_HID_BUTTON_HELP_DOUBLE; break;
        case Q_KEY_CODE_F9 : button = SMC_HID_BUTTON_HALL_EFFECT_1; break;
        case Q_KEY_CODE_F10: button = SMC_HID_BUTTON_HALL_EFFECT; break;
        default            : return;
    }

    if (((s->states & BIT32(button)) != 0) != key->down) {
        if (key->down) { s->states |= BIT32(button); }
        else {
            s->states &= ~BIT32(button);
        }
        apple_smc_send_hid_button(smc, button, key->down);
    }
}

#define BUTTON_READER(_btn, _enum)                                                                                   \
    static SMCResult apple_buttons_smc_read_##_btn(SMCKey* key, SMCKeyData* data, const void* in, uint8_t in_length) \
    {                                                                                                                \
        AppleButtonsState* s = key->opaque;                                                                          \
                                                                                                                     \
        stl_le_p(data->data, (s->states & BIT32(SMC_HID_BUTTON_##_enum)) != 0);                                      \
                                                                                                                     \
        return SMC_RESULT_SUCCESS;                                                                                   \
    }

BUTTON_READER(vol_up, VOL_UP);
BUTTON_READER(vol_down, VOL_DOWN);
BUTTON_READER(hold, HOLD);
BUTTON_READER(ringer, RINGER);

SysBusDevice* apple_buttons_create(AppleDTNode* node)
{
    DeviceState*       dev;
    AppleButtonsState* s;
    SysBusDevice*      sbd;

    dev = qdev_new(TYPE_APPLE_BUTTONS);
    s   = APPLE_BUTTONS(dev);
    sbd = SYS_BUS_DEVICE(dev);

    AppleSMC* smc = APPLE_SMC_IOP(object_property_get_link(OBJECT(qdev_get_machine()), "smc", &error_fatal));
    apple_smc_add_key_func(smc, 'bVUP', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, s, apple_buttons_smc_read_vol_up, NULL);
    apple_smc_add_key_func(smc, 'bVDN', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, s, apple_buttons_smc_read_vol_down, NULL);
    apple_smc_add_key_func(smc, 'bHLD', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, s, apple_buttons_smc_read_hold, NULL);
    apple_smc_add_key_func(smc, 'bRIN', 4, SMC_KEY_TYPE_UINT32, SMC_ATTR_LE, s, apple_buttons_smc_read_ringer, NULL);
    bool powered_by_hold_btn = true;
    apple_smc_add_key(smc, 'bPHD', 1, SMC_KEY_TYPE_FLAG, SMC_ATTR_R, &powered_by_hold_btn);

    qemu_mutex_init(&s->mutex);

    return sbd;
}

static void apple_buttons_qdev_reset_hold(Object* obj, ResetType type)
{
    AppleButtonsState* s = APPLE_BUTTONS(obj);
    QEMU_LOCK_GUARD(&s->mutex);
    s->states = 0;
}

static const QemuInputHandler apple_buttons_handler = {
    .name  = "Apple Buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = apple_buttons_handle_event,
};

static void apple_buttons_realize(DeviceState* dev, Error** errp)
{
    QemuInputHandlerState* s = qemu_input_handler_register(dev, &apple_buttons_handler);
    qemu_input_handler_activate(s);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_OTHER, true);
}

static void apple_buttons_unrealize(DeviceState* dev) { }

static void apple_buttons_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*     dc = DEVICE_CLASS(klass);
    ResettableClass* rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = apple_buttons_qdev_reset_hold;

    dc->realize   = apple_buttons_realize;
    dc->unrealize = apple_buttons_unrealize;
    dc->desc      = "Apple Buttons";
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo apple_buttons_types = {
    .name   = TYPE_APPLE_BUTTONS,
    .parent = TYPE_SYS_BUS_DEVICE,
    OBJECT_TYPE_INSTANCE(AppleButtonsState),
    .class_init = apple_buttons_class_init,
};

static void apple_buttons_init(void) { type_register_static(&apple_buttons_types); }

type_init(apple_buttons_init);
