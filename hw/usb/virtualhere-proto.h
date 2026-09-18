/*
 * VirtualHere wire protocol: message layout and construction.
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
#include "qemu/bswap.h"

#define VIRTUALHERE_SERVER_DEFAULT_ADDR "127.0.0.1:8030"
#define VIRTUALHERE_SERVICE_TYPE        "_vhusb._tcp"
#define VIRTUALHERE_SERVER_DEVICE_ID    1

enum
{
    /* Client -> server. */
    VIRTUALHERE_MSG_CLIENT_HELLO      = 0x00,
    VIRTUALHERE_MSG_READY             = 0x09,
    VIRTUALHERE_MSG_HEARTBEAT         = 0x0A,
    VIRTUALHERE_MSG_CTRL_SUBMIT       = 0x0D,
    VIRTUALHERE_MSG_TIME_PONG         = 0x0F,
    VIRTUALHERE_MSG_BULK_SUBMIT       = 0x12,
    VIRTUALHERE_MSG_DEVICE_INIT       = 0x16,
    VIRTUALHERE_MSG_USE_DEVICE        = 0x19,
    VIRTUALHERE_MSG_CLEAR_HALT        = 0x1A, /* CLEAR_FEATURE fast path */
    VIRTUALHERE_MSG_STOP_USING_DEVICE = 0x23,
    VIRTUALHERE_MSG_ISO_SUBMIT        = 0x24,
    VIRTUALHERE_MSG_SET_NICKNAME      = 0x2A, /* SetDeviceNicknamePayload */
    VIRTUALHERE_MSG_LICENSE           = 0x2B, /* LicenseServerPayload */
    VIRTUALHERE_MSG_SET_DEBUG_LEVEL   = 0x2E, /* SetDebugLevelPayload */
    VIRTUALHERE_MSG_ADDRESS_CONN_ID   = 0x2F, /* AddressConnIdStringPayload */
    VIRTUALHERE_MSG_PORT_POWER        = 0x30, /* PortPowerControlPayload */
    VIRTUALHERE_MSG_SERVER_ADMIN      = 0x36,
    VIRTUALHERE_MSG_REMOTE_EXEC       = 0x38, /* EncryptedRequest */
    VIRTUALHERE_MSG_IGNORE_DEVICE     = 0x3D, /* IgnoreDevicePayload */
    VIRTUALHERE_MSG_LIST_REVERSE      = 0x40,
    VIRTUALHERE_MSG_ENCRYPTED_REQUEST = 0x45, /* EncryptedRequest */
    VIRTUALHERE_MSG_BULK_SUBMIT_ALT   = 0x4A, /* shares the 0x12 handler */
    VIRTUALHERE_MSG_TOGGLE_EASYFIND   = 0x4B, /* ToggleEasyFindPayload */
    VIRTUALHERE_MSG_GADGET_LIST       = 0x59, /* AvailableGadgetsPayload */
    VIRTUALHERE_MSG_SEND_TO_GADGET    = 0x5B, /* SendToGadgetPayload */

    /* Server -> client. */
    VIRTUALHERE_MSG_SERVER_INFO         = 0x01,
    VIRTUALHERE_MSG_UNBIND_ACK          = 0x07,
    VIRTUALHERE_MSG_HEARTBEAT_ACK       = 0x0B,
    VIRTUALHERE_MSG_TIME_PING           = 0x0C,
    VIRTUALHERE_MSG_CTRL_ACK            = 0x0E,
    VIRTUALHERE_MSG_DEVICE_INFO         = 0x10,
    VIRTUALHERE_MSG_BULK_ACK            = 0x13,
    VIRTUALHERE_MSG_DEVICE_INIT_ACK     = 0x17,
    VIRTUALHERE_MSG_SERVER_NOTICE       = 0x18,
    VIRTUALHERE_MSG_DEVICE_DESC         = 0x27,
    VIRTUALHERE_MSG_BIND_RESULT         = 0x2C,
    VIRTUALHERE_MSG_SERVER_TEXT         = 0x2D, /* C string at +0x41 */
    VIRTUALHERE_MSG_PORT_POWER_ACK      = 0x31,
    VIRTUALHERE_MSG_SERVER_ADMIN_ACK    = 0x37, /* StartServerAdminPayload */
    VIRTUALHERE_MSG_REMOTE_EXEC_ACK     = 0x39, /* RemoteExecResponse */
    VIRTUALHERE_MSG_LIST_REVERSE_ACK    = 0x41, /* ListReverseClientsAckPayload */
    VIRTUALHERE_MSG_TOGGLE_EASYFIND_ACK = 0x4C, /* array of 24-byte records */
    VIRTUALHERE_MSG_NOTICE_TEXT         = 0x4F, /* C string at +0x21 */
    VIRTUALHERE_MSG_GADGET_LIST_ACK     = 0x5A,

    /*
     * Registered like a wire type but its handler reads a function
     * pointer out of the message at +0x11 and calls it.
     */
    VIRTUALHERE_MSG_LOCAL_CALLBACK = 0x3C,
};

#define VIRTUALHERE_BIND_OK              1
#define VIRTUALHERE_SETUP_LEN            8
#define VIRTUALHERE_GUID_LEN             16
#define VIRTUALHERE_HUB_PROTOCOL_VERSION 8

typedef struct QEMU_PACKED VHHeader
{
    uint8_t  type;
    uint8_t  opaque[0x1A];
    uint16_t device_id;
    uint32_t length;
} VHHeader;

#define VIRTUALHERE_HDR_LEN      offsetof(VHHeader, length)
#define VIRTUALHERE_LENGTH_OFF   offsetof(VHHeader, length)
#define VIRTUALHERE_PAYLOAD_OFF  sizeof(VHHeader)
#define VIRTUALHERE_MSG_MIN_SIZE sizeof(VHHeader)

typedef struct QEMU_PACKED VHCtrlSubmit
{
    VHHeader header;
    uint8_t  reserved[4];
    uint8_t  flags;
    uint8_t  pad[3];
    uint8_t  setup[VIRTUALHERE_SETUP_LEN];
    uint8_t  data[];
} VHCtrlSubmit;

typedef struct QEMU_PACKED VHCtrlAck
{
    VHHeader header;
    uint8_t  setup[VIRTUALHERE_SETUP_LEN];
    int32_t  status;
    uint32_t data_len;
    uint8_t  data[];
} VHCtrlAck;

typedef struct QEMU_PACKED VHBulkSubmit
{
    VHHeader header;
    uint32_t endpoint;
    uint8_t  flags;
    uint32_t xfer_type;
    uint32_t length;
    uint8_t  data[];
} VHBulkSubmit;

typedef struct QEMU_PACKED VHBulkAck
{
    VHHeader header;
    int32_t  status; /* <0 -> errno */
    uint8_t  data[];
} VHBulkAck;

typedef struct QEMU_PACKED VHClientHello
{
    VHHeader header;
    char     user[0x40];
    char     host[0x40];
} VHClientHello;

typedef struct QEMU_PACKED VHServerInfo
{
    VHHeader header;
    uint8_t  protocol_version;
    char     name[0x80];
    char     unique_id[0x25];
    uint16_t device_limit; /* 0 is unlimited */
    uint8_t  guid[VIRTUALHERE_GUID_LEN];
    uint8_t  server_version_major;
    uint8_t  server_version_minor;
    uint8_t  server_version_patch;
    uint8_t  unknown_dc;
    char     hostname[0x100];
    char     net_interface[0x100]; /* INTERFACE row in the dialog */
    uint8_t  supports_easyfind;
    uint8_t  easyfind_enabled;
    uint8_t  easyfind_id[16];
    uint32_t easyfind_suffix;
    uint8_t  unknown_2f3;
    /*
     * Zero makes the client rewrite a Mass Storage UAS interface protocol
     * 0x62 to 0x50 (BOT) as the configuration descriptor passes through.
     */
    uint8_t supports_uasp;
} VHServerInfo;

typedef struct QEMU_PACKED VHDeviceInfo
{
    VHHeader header;
    uint16_t vendor_id;
    uint16_t product_id;
    char     manufacturer[0x40];
    char     product[0x40];
    uint8_t  pad0[2];
    uint16_t device_id;
    uint8_t  state; /* VIRTUALHERE_DEVICE_STATE_* */
    uint8_t  holder_guid[VIRTUALHERE_GUID_LEN];
    uint32_t unknown_ba;
    char     unknown_be[0x40];
    uint8_t  num_configs;
    uint8_t  num_ifaces;
    uint8_t  iface_class;
    uint8_t  iface_subclass;
    uint8_t  iface_protocol;
    char     holder_user[0x40];
    uint8_t  unknown_143[16];
    char     serial[0x80];
    char     holder_host[0x40];
    uint8_t  unknown_213;
    /* Only the first byte was ever non-zero in a capture. */
    uint64_t unknown_214[5];
    char     timestamp[0x20];
} VHDeviceInfo;

typedef struct QEMU_PACKED VHDeviceDesc
{
    VHHeader header;
    int32_t  bind_result;
    uint8_t  speed;
    uint8_t  device_desc[0x12];
    uint8_t  config_desc[0xFFFF];
    char     product[0x100];
    char     serial[0x40];
} VHDeviceDesc;

enum
{
    VIRTUALHERE_DEVICE_STATE_GONE      = 0,
    VIRTUALHERE_DEVICE_STATE_AVAILABLE = 1,
    VIRTUALHERE_DEVICE_STATE_IN_USE    = 3,
};

QEMU_BUILD_BUG_ON(offsetof(VHHeader, device_id) != 0x1B);
QEMU_BUILD_BUG_ON(offsetof(VHHeader, length) != 0x1D);
QEMU_BUILD_BUG_ON(sizeof(VHHeader) != 0x21);

QEMU_BUILD_BUG_ON(offsetof(VHCtrlSubmit, flags) != 0x25);
QEMU_BUILD_BUG_ON(offsetof(VHCtrlSubmit, setup) != 0x29);
QEMU_BUILD_BUG_ON(offsetof(VHCtrlSubmit, data) != 0x31);

QEMU_BUILD_BUG_ON(offsetof(VHCtrlAck, setup) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHCtrlAck, status) != 0x29);
QEMU_BUILD_BUG_ON(offsetof(VHCtrlAck, data_len) != 0x2D);
QEMU_BUILD_BUG_ON(offsetof(VHCtrlAck, data) != 0x31);

QEMU_BUILD_BUG_ON(offsetof(VHBulkSubmit, endpoint) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHBulkSubmit, flags) != 0x25);
QEMU_BUILD_BUG_ON(offsetof(VHBulkSubmit, xfer_type) != 0x26);
QEMU_BUILD_BUG_ON(offsetof(VHBulkSubmit, length) != 0x2A);
QEMU_BUILD_BUG_ON(offsetof(VHBulkSubmit, data) != 0x2E);

QEMU_BUILD_BUG_ON(offsetof(VHBulkAck, status) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHBulkAck, data) != 0x25);

QEMU_BUILD_BUG_ON(offsetof(VHClientHello, user) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHClientHello, host) != 0x61);

QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, protocol_version) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, name) != 0x22);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, unique_id) != 0xA2);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, device_limit) != 0xC7);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, guid) != 0xC9);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, server_version_major) != 0xD9);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, server_version_minor) != 0xDA);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, server_version_patch) != 0xDB);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, hostname) != 0xDD);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, net_interface) != 0x1DD);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, supports_easyfind) != 0x2DD);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, easyfind_id) != 0x2DF);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, easyfind_suffix) != 0x2EF);
QEMU_BUILD_BUG_ON(offsetof(VHServerInfo, supports_uasp) != 0x2F4);
QEMU_BUILD_BUG_ON(sizeof(VHServerInfo) != 757);

QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, vendor_id) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, manufacturer) != 0x25);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, product) != 0x65);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, device_id) != 0xA7);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, state) != 0xA9);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, holder_guid) != 0xAA);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, num_configs) != 0xFE);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, iface_class) != 0x100);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, unknown_ba) != 0xBA);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, unknown_be) != 0xBE);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, holder_user) != 0x103);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, unknown_143) != 0x143);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, unknown_213) != 0x213);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, unknown_214) != 0x214);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, timestamp) != 0x23C);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, serial) != 0x153);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceInfo, holder_host) != 0x1D3);
QEMU_BUILD_BUG_ON(sizeof(VHDeviceInfo) != 604);

QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, bind_result) != 0x21);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, speed) != 0x25);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, device_desc) != 0x26);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, config_desc) != 0x38);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, product) != 0x10037);
QEMU_BUILD_BUG_ON(offsetof(VHDeviceDesc, serial) != 0x10137);
QEMU_BUILD_BUG_ON(sizeof(VHDeviceDesc) != 65911);

static inline void* vh_msg_new_reply(size_t size, uint8_t type, const void* req_hdr);

static inline void* vh_msg_new(size_t size, uint8_t type)
{
    VHHeader* hdr = g_malloc0(size);

    hdr->type   = type;
    hdr->length = cpu_to_le32(size - sizeof(VHHeader));

    return hdr;
}

static inline void* vh_msg_new_reply(size_t size, uint8_t type, const void* req_hdr)
{
    VHHeader* hdr = vh_msg_new(size, type);

    if (req_hdr != NULL) {
        memcpy(hdr, req_hdr, VIRTUALHERE_HDR_LEN);
        hdr->type = type;
    }

    return hdr;
}
