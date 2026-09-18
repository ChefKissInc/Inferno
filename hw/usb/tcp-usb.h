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

#define USB_TCP_REMOTE_ADDR_DEFAULT "unix:/tmp/InfernoUSBRemote"

enum
{
    TCP_USB_REQUEST = 1,
    TCP_USB_RESPONSE,
    TCP_USB_RESET,
    TCP_USB_CANCEL
};

typedef struct QEMU_PACKED tcp_usb_header
{
    uint8_t type;
} tcp_usb_header_t;

typedef struct QEMU_PACKED tcp_usb_request_header
{
    uint8_t      addr;
    int          pid;
    uint8_t      ep;
    uint64_t     id;
    unsigned int stream;
    uint8_t      short_not_ok;
    uint8_t      int_req;
    uint16_t     length;
} tcp_usb_request_header;

typedef struct QEMU_PACKED tcp_usb_response_header
{
    uint8_t  addr;
    int      pid;
    uint8_t  ep;
    uint64_t id;
    uint32_t status;
    uint16_t length;
} tcp_usb_response_header;

typedef struct QEMU_PACKED tcp_usb_cancel_header
{
    uint8_t  addr;
    int      pid;
    uint8_t  ep;
    uint64_t id;
} tcp_usb_cancel_header;
