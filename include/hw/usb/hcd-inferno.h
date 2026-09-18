/*
 * Inferno USB Uplink Host.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "qemu/timer.h"
#include "hw/usb/inferno-proto.h"
#include "io/channel.h"
#include "qapi/qapi-types-sockets.h"
#include "qemu/coroutine.h"
#include "qom/object.h"

#define TYPE_USB_INFERNO_HOST "usb-inferno-host"
OBJECT_DECLARE_SIMPLE_TYPE(USBInfernoHostState, USB_INFERNO_HOST)

typedef struct USBInfernoPacket
{
    USBPacket            p;
    void*                buffer;
    USBDevice*           dev;
    USBInfernoHostState* s;
    uint8_t              addr;
} USBInfernoPacket;

struct USBInfernoHostState
{
    SysBusDevice parent_obj;

    USBBus         bus;
    USBPort        ports[3];
    QEMUTimer*     retry_timer;
    QEMUBH*        reset_bh;
    QIOChannel*    ioc;
    CoMutex        write_mutex;
    bool           closed;
    bool           stopped;
    char*          connect_addr;
    SocketAddress* sockaddr;
};
