/*
 * Export a locally attached USB device to a network protocol.
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
#include "qapi/qapi-types-usb.h"
#include "qemu/coroutine.h"

/* Offsets within the standard descriptors */
#define USB_DEV_DESC_SIZE         0x12
#define USB_DEV_DESC_VENDOR_ID    8
#define USB_DEV_DESC_PRODUCT_ID   10
#define USB_DEV_DESC_MANUFACTURER 14
#define USB_DEV_DESC_PRODUCT      15
#define USB_DEV_DESC_SERIAL       16
#define USB_DEV_DESC_NUM_CONFIGS  17
#define USB_CFG_DESC_NUM_IFACES   4
#define USB_IFACE_DESC_CLASS      5
#define USB_IFACE_DESC_SUBCLASS   6
#define USB_IFACE_DESC_PROTOCOL   7

typedef struct USBUplinkPacket
{
    USBPacket  p;
    Coroutine* waiter;
} USBUplinkPacket;

typedef struct USBUplinkDescriptors
{
    uint8_t* device;
    uint32_t device_len;
    uint8_t* configs;
    uint32_t configs_len;
    uint8_t  num_configs;
    uint8_t  speed; /* USB_SPEED_* */
    char*    manufacturer;
    char*    product;
    char*    serial;
} USBUplinkDescriptors;

void usb_uplink_descriptors_free(USBUplinkDescriptors* desc);

#define USB_UPLINK_PORTS 4

/* A bus that exports whichever device is plugged into it. */
typedef struct USBUplinkDevice
{
    USBBus  bus;
    USBPort ports[USB_UPLINK_PORTS];
    CoMutex ctrl_lock;

    USBUplinkDescriptors* desc;
} USBUplinkDevice;

void usb_uplink_init(USBUplinkDevice* x, DeviceState* owner, USBBusOps* bus_ops, USBPortOps* port_ops, void* opaque);

/*
 * The device being exported, skipping hubs.
 */
USBDevice* usb_uplink_active_except(USBUplinkDevice* x, USBPort* except);
USBDevice* usb_uplink_active(USBUplinkDevice* x);

/* Cached; read on first use and dropped by usb_uplink_invalidate(). */
const USBUplinkDescriptors* coroutine_fn usb_uplink_descriptors(USBUplinkDevice* x);
void                                     usb_uplink_invalidate(USBUplinkDevice* x);

/* The cache alone, for callers that cannot block on a wire read. */
const USBUplinkDescriptors* usb_uplink_descriptors_cached(USBUplinkDevice* x);

/* Drops the cache and resets the device; use when a claim ends. */
void usb_uplink_release(USBUplinkDevice* x);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(USBUplinkDescriptors, usb_uplink_descriptors_free)

USBUplinkDescriptors* coroutine_fn usb_uplink_read_descriptors(USBDevice* dev);

bool usb_uplink_device_usable(USBDevice* dev);

/*
 * Resumes a coroutine parked on this packet.
 * @ret: true if it consumed the completion.
 */
bool usb_uplink_packet_wake(USBPacket* p);

/* Map a USB_RET_* code to the negative errno a USB/IP peer expects. */
int32_t usb_uplink_status_to_errno(int32_t status);

int32_t usb_uplink_submit(USBDevice* dev, USBUplinkPacket* pkt, uint8_t ep_addr, void* buf, uint32_t len);

/*
 * NOTE: Will keep asking on NACK.
 */
int32_t coroutine_fn usb_uplink_transfer(USBUplinkDevice* x, USBUplinkPacket* pkt, uint8_t ep_addr, void* buf,
                                         uint32_t len, const bool* abort);

/* Return a completed packet to a state where it can be submitted again. */
void usb_uplink_packet_reset(USBUplinkPacket* pkt);

int32_t coroutine_fn usb_uplink_control(USBDevice* dev, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                        uint16_t wIndex, uint16_t wLength, void* buf, uint32_t* actual_len,
                                        bool wait_for_device);

/*
 * @ret: Caller-owned reference to created USB uplink.
 */
DeviceState* usb_uplink_new(USBUplinkType type, const char* addr, Error** errp);
