/*
 * TCP Remote USB.
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
#include "hw/usb/tcp-usb.h"
#include "io/channel.h"

typedef struct USBTCPInflightPacket
{
    USBPacket* p;
    QTAILQ_ENTRY(USBTCPInflightPacket) queue;
    uint8_t addr;
} USBTCPInflightPacket;

typedef struct USBTCPCompletedPacket
{
    USBPacket* p;
    QTAILQ_ENTRY(USBTCPCompletedPacket) queue;
    uint8_t addr;
} USBTCPCompletedPacket;

typedef struct USBTCPRemoteMsg
{
    QTAILQ_ENTRY(USBTCPRemoteMsg) queue;
    size_t  len;
    uint8_t data[];
} USBTCPRemoteMsg;

struct USBTCPRemoteState
{
    USBDevice parent_obj;

    QemuMutex queue_mutex;
    QTAILQ_HEAD(, USBTCPInflightPacket) queue;

    QemuMutex completed_queue_mutex;
    QTAILQ_HEAD(, USBTCPCompletedPacket) completed_queue;

    QemuMutex send_mutex;
    QTAILQ_HEAD(, USBTCPRemoteMsg) send_queue;

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

#define TYPE_USB_TCP_REMOTE "usb-tcp-remote"
OBJECT_DECLARE_SIMPLE_TYPE(USBTCPRemoteState, USB_TCP_REMOTE)
