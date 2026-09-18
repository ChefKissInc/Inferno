/*
 * Inferno USB Uplink.
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
#include "hw/usb.h"
#include "hw/usb/inferno-proto.h"
#include "io/channel.h"

typedef struct USBInfernoInflightPacket
{
    USBPacket* p;
    QTAILQ_ENTRY(USBInfernoInflightPacket) queue;
    uint8_t addr;
} USBInfernoInflightPacket;

typedef struct USBInfernoCompletedPacket
{
    USBPacket* p;
    QTAILQ_ENTRY(USBInfernoCompletedPacket) queue;
    uint8_t addr;
} USBInfernoCompletedPacket;

typedef struct USBInfernoRemoteMsg
{
    QTAILQ_ENTRY(USBInfernoRemoteMsg) queue;
    size_t  len;
    uint8_t data[];
} USBInfernoRemoteMsg;

struct USBInfernoRemoteState
{
    USBDevice parent_obj;

    QemuMutex queue_mutex;
    QTAILQ_HEAD(, USBInfernoInflightPacket) queue;

    QemuMutex completed_queue_mutex;
    QTAILQ_HEAD(, USBInfernoCompletedPacket) completed_queue;

    QemuMutex send_mutex;
    QTAILQ_HEAD(, USBInfernoRemoteMsg) send_queue;

    QEMUBH* completed_bh;
    QEMUBH* addr_bh;
    QEMUBH* cleanup_bh;
    QEMUBH* send_bh;

    char*       listen_addr;
    int         socket;
    QIOChannel* ioc;
    uint8_t     addr;
    bool        closed;
    bool        stopped;
    bool        sending;
};

#define TYPE_USB_INFERNO_REMOTE "usb-inferno-remote"
OBJECT_DECLARE_SIMPLE_TYPE(USBInfernoRemoteState, USB_INFERNO_REMOTE)
