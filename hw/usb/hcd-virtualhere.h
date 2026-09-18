#pragma once

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "io/channel.h"
#include "io/net-listener.h"
#include "qemu/coroutine.h"
#include "qemu/lockable.h"
#include "qom/object.h"
#include "hw/usb/usb-uplink.h"

#include "mdns.h"
#include "virtualhere-proto.h"

#define TYPE_USB_VIRTUALHERE "usb-virtualhere"
OBJECT_DECLARE_SIMPLE_TYPE(USBVirtualHereState, USB_VIRTUALHERE)

typedef struct USBVirtualHereConn USBVirtualHereConn;

typedef struct USBVirtualHerePacket
{
    USBUplinkPacket     base;
    USBVirtualHereConn* conn;
    QLIST_ENTRY(USBVirtualHerePacket) link;
    int32_t             out_status;
    uint8_t             hdr[VIRTUALHERE_HDR_LEN];
    uint8_t             setup[VIRTUALHERE_SETUP_LEN];
    uint8_t             ep_addr;
    uint32_t            xfer_len;
    void*               buffer;
} USBVirtualHerePacket;

struct USBVirtualHereConn
{
    USBVirtualHereState* s;
    int                  refcount;
    QIOChannel*          ioc;
    CoMutex              write_mutex;
    QEMUTimer*           heartbeat_timer;
    bool                 closed;
    bool                 using_device;
    bool                 use_pending;
    QLIST_HEAD(, USBVirtualHerePacket) packets;
    char                 client_name[64];
    char                 client_host[64];
};

struct USBVirtualHereState
{
    SysBusDevice parent_obj;

    USBUplinkDevice usb;

    QIONetListener* listener;
    char*           addr;
    MDNSService*    mdns;

    Notifier exit_notifier;

    QEMUTimer* unplug_timer;

    USBVirtualHereConn* active_conn;

    char*   server_name;
    char*   server_id;
    char*   server_host;
    uint8_t server_guid[VIRTUALHERE_GUID_LEN];
};
