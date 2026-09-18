/*
 * VirtualHere-protocol USB server.
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

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hcd-virtualhere.h"
#include "io/channel-util.h"
#include "io/channel.h"
#include "io/net-listener.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qom/object.h"
#include "system/iothread.h"
#include "system/system.h"
#include "qemu/uuid.h"

#include <lz4.h>

#if 0
    #define VIRTUALHERE_DPRINTF(fmt, ...)                            \
        do {                                                         \
            fprintf(stderr, "hcd-virtualhere: " fmt, ##__VA_ARGS__); \
        }                                                            \
        while (0)
#else
    #define VIRTUALHERE_DPRINTF(fmt, ...) (void)0
#endif

#if 0
    #define VIRTUALHERE_TRACE(fmt, ...) VIRTUALHERE_DPRINTF(fmt, ##__VA_ARGS__)
#else
    #define VIRTUALHERE_TRACE(fmt, ...) (void)0
#endif

#if 0
static void vh_debug_hexdump(const uint8_t* buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len && i < 64; i += 16) {
        uint32_t j;

        fprintf(stderr, "hcd-virtualhere:   %04x ", i);
        for (j = i; j < i + 16 && j < len && j < 64; j++) { fprintf(stderr, " %02x", buf[j]); }
        fprintf(stderr, "  |");
        for (j = i; j < i + 16 && j < len && j < 64; j++) {
            fprintf(stderr, "%c", (buf[j] >= 0x20 && buf[j] < 0x7F) ? buf[j] : '.');
        }
        fprintf(stderr, "|\n");
    }
}
#else
    #define vh_debug_hexdump(buf, len) (void)0
#endif

#define VIRTUALHERE_HEARTBEAT_PERIOD_MS 5000

#define VIRTUALHERE_SERVER_BANNER "Support Inferno at https://ko-fi.com/chefkiss"

#define VIRTUALHERE_MSG_MAX_SIZE  0x3200001
#define VIRTUALHERE_FRAME_HDR_LEN 8

#define VIRTUALHERE_PAY(off) ((off) - VIRTUALHERE_PAYLOAD_OFF)

static void vh_conn_close(USBVirtualHereConn* conn);
static void vh_conn_unref(USBVirtualHereConn* conn);
static void usb_virtualhere_exit_notifier(Notifier* n, void* data);

static ssize_t coroutine_fn vh_read(QIOChannel* ioc, void* buf, size_t len)
{
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    ssize_t      ret;
    Error*       err = NULL;

    ret = qio_channel_readv_full_all_eof(ioc, &iov, 1, NULL, 0, 0, &err);
    if (err) { error_report_err(err); }
    return (ret <= 0) ? ret : (ssize_t)iov.iov_len;
}

static bool coroutine_fn vh_write(QIOChannel* ioc, const void* buf, size_t len)
{
    struct iovec iov = {.iov_base = (void*)buf, .iov_len = len};
    Error*       err = NULL;
    int          ret = qio_channel_writev_full_all(ioc, &iov, 1, NULL, 0, 0, &err);

    if (err) { error_report_err(err); }
    return ret == 0;
}

static bool coroutine_fn vh_conn_send_raw(USBVirtualHereConn* conn, const void* buf, size_t len)
{
    bool ok = true;

    WITH_QEMU_LOCK_GUARD(&conn->write_mutex)
    {
        if (conn->closed) { ok = false; }
        else if (!vh_write(conn->ioc, buf, len)) {
            ok = false;
        }
    }

    if (!ok) { vh_conn_close(conn); }
    return ok;
}

static bool coroutine_fn vh_conn_send_frame(USBVirtualHereConn* conn, uint32_t compressed_len,
                                            uint32_t uncompressed_len, const void* body, uint32_t body_len)
{
    g_autofree uint8_t* frame = g_malloc(VIRTUALHERE_FRAME_HDR_LEN + body_len);

    stl_le_p(frame, compressed_len);
    stl_le_p(frame + 4, uncompressed_len);
    memcpy(frame + VIRTUALHERE_FRAME_HDR_LEN, body, body_len);

    return vh_conn_send_raw(conn, frame, VIRTUALHERE_FRAME_HDR_LEN + body_len);
}

static bool coroutine_fn vh_conn_send_msg(USBVirtualHereConn* conn, uint8_t type, const uint8_t* req_hdr,
                                          const void* payload, uint32_t payload_len)
{
    uint32_t            msg_len = VIRTUALHERE_PAYLOAD_OFF + payload_len;
    g_autofree uint8_t* msg     = g_malloc0(msg_len);

    if (req_hdr != NULL) { memcpy(msg, req_hdr, VIRTUALHERE_HDR_LEN); }
    msg[0] = type;
    stl_le_p(msg + VIRTUALHERE_LENGTH_OFF, payload_len);
    if (payload_len) { memcpy(msg + VIRTUALHERE_PAYLOAD_OFF, payload, payload_len); }

    return vh_conn_send_frame(conn, 0, msg_len, msg, msg_len);
}

static bool coroutine_fn vh_conn_send_lz4(USBVirtualHereConn* conn, const uint8_t* uncompressed,
                                          uint32_t uncompressed_len)
{
    g_autofree uint8_t* compressed = g_malloc(LZ4_compressBound(uncompressed_len));
    int                 compressed_len;

    compressed_len = LZ4_compress_default((const char*)uncompressed, (char*)compressed, uncompressed_len,
                                          LZ4_compressBound(uncompressed_len));
    if (compressed_len <= 0) {
        error_report("hcd-virtualhere: LZ4 compression failed");
        return false;
    }

    return vh_conn_send_frame(conn, compressed_len, uncompressed_len, compressed, compressed_len);
}

static bool coroutine_fn vh_read_body(QIOChannel* ioc, uint32_t compressed_len, uint8_t* msg, uint32_t length)
{
    g_autofree uint8_t* compressed = NULL;
    int                 decompressed;

    if (compressed_len == 0) { return vh_read(ioc, msg, length) == (ssize_t)length; }

    compressed = g_malloc(compressed_len);
    if (vh_read(ioc, compressed, compressed_len) != (ssize_t)compressed_len) { return false; }

    decompressed = LZ4_decompress_safe((const char*)compressed, (char*)msg, compressed_len, length);
    if (decompressed < 0 || (uint32_t)decompressed != length) {
        error_report("hcd-virtualhere: LZ4 decompression failed (%u -> %u, got %d)", compressed_len, length,
                     decompressed);
        return false;
    }

    VIRTUALHERE_TRACE("%s: decompressed message: %u -> %u bytes, type 0x%02x\n", __func__, compressed_len, length,
                      msg[0]);

    return true;
}

static bool coroutine_fn vh_read_message(QIOChannel* ioc, uint8_t** out_msg, uint32_t* out_len)
{
    uint8_t             header[VIRTUALHERE_FRAME_HDR_LEN];
    uint32_t            compressed_len, length;
    g_autofree uint8_t* msg = NULL;

    if (vh_read(ioc, header, sizeof(header)) != (ssize_t)sizeof(header)) { return false; }

    compressed_len = ldl_le_p(header);
    length         = ldl_le_p(header + 4);

    if (length == 0 || length > VIRTUALHERE_MSG_MAX_SIZE || compressed_len > VIRTUALHERE_MSG_MAX_SIZE) {
        error_report("hcd-virtualhere: bad frame header (compressed=%u uncompressed=%u)", compressed_len, length);
        return false;
    }

    msg = g_malloc(length);
    if (!vh_read_body(ioc, compressed_len, msg, length)) { return false; }

    *out_msg = g_steal_pointer(&msg);
    *out_len = length;

    return true;
}

static void vh_init_server_guid(USBVirtualHereState* s)
{
    QemuUUID uuid;

    if (!qemu_uuid_is_null(&qemu_uuid)) { uuid = qemu_uuid; }
    else {
        qemu_uuid_generate(&uuid);
    }

    memcpy(s->server_guid, uuid.data, VIRTUALHERE_GUID_LEN);
}

static const uint8_t* vh_find_first_interface(const uint8_t* cfg, uint32_t cfg_len)
{
    uint32_t off = 0;

    while (off + 2 <= cfg_len) {
        uint8_t blen = cfg[off];

        if (blen < 2 || off + blen > cfg_len) { break; }
        if (cfg[off + 1] == USB_DT_INTERFACE && blen >= 9) { return cfg + off; }
        off += blen;
    }
    return NULL;
}

static VHServerInfo* vh_build_server_announce(USBVirtualHereState* s)
{
    VHServerInfo* out = vh_msg_new(sizeof(*out), VIRTUALHERE_MSG_SERVER_INFO);

    out->protocol_version     = VIRTUALHERE_HUB_PROTOCOL_VERSION;
    out->server_version_major = 4;
    out->server_version_minor = 8;
    out->server_version_patch = 8;
    pstrcpy(out->name, sizeof(out->name), s->server_name);
    pstrcpy(out->unique_id, sizeof(out->unique_id), s->server_id);
    memcpy(out->guid, s->server_guid, sizeof(out->guid));
    pstrcpy(out->hostname, sizeof(out->hostname), s->server_host);

    return out;
}

static VHDeviceInfo* vh_build_short_info(USBVirtualHereState* s, USBVirtualHereConn* conn,
                                         const USBUplinkDescriptors* desc, uint16_t device_id)
{
    USBDevice*     dev   = usb_uplink_active(&s->usb);
    const uint8_t* iface = vh_find_first_interface(desc->configs, desc->configs_len);
    VHDeviceInfo*  out   = vh_msg_new(sizeof(*out), VIRTUALHERE_MSG_DEVICE_INFO);

    out->vendor_id  = cpu_to_le16(lduw_le_p(desc->device + USB_DEV_DESC_VENDOR_ID));
    out->product_id = cpu_to_le16(lduw_le_p(desc->device + USB_DEV_DESC_PRODUCT_ID));

    if (desc->manufacturer != NULL) { pstrcpy(out->manufacturer, sizeof(out->manufacturer), desc->manufacturer); }
    else {
        snprintf(out->manufacturer, sizeof(out->manufacturer), "0x%04x", le16_to_cpu(out->vendor_id));
    }

    pstrcpy(out->product, sizeof(out->product),
            desc->product ?: (dev && dev->product_desc[0] ? dev->product_desc : "USB Device"));

    out->device_id = cpu_to_le16(device_id);

    if (conn != NULL && (conn->using_device || conn->use_pending)) {
        out->state = VIRTUALHERE_DEVICE_STATE_IN_USE;
        memcpy(out->holder_guid, s->server_guid, sizeof(out->holder_guid));
        pstrcpy(out->holder_user, sizeof(out->holder_user), conn->client_name);
        pstrcpy(out->holder_host, sizeof(out->holder_host), conn->client_host);
    }
    else {
        out->state = VIRTUALHERE_DEVICE_STATE_AVAILABLE;
    }

    out->num_configs = desc->num_configs;
    if (desc->configs_len >= USB_CFG_DESC_NUM_IFACES + 1) { out->num_ifaces = desc->configs[USB_CFG_DESC_NUM_IFACES]; }
    if (iface != NULL) {
        out->iface_class    = iface[USB_IFACE_DESC_CLASS];
        out->iface_subclass = iface[USB_IFACE_DESC_SUBCLASS];
        out->iface_protocol = iface[USB_IFACE_DESC_PROTOCOL];
    }

    if (desc->serial != NULL) { pstrcpy(out->serial, sizeof(out->serial), desc->serial); }

    out->unknown_214[0] = 1;

    return out;
}

static VHDeviceDesc* vh_build_long_info(USBVirtualHereState* s, const USBUplinkDescriptors* desc, uint16_t device_id)
{
    USBDevice*    dev = usb_uplink_active(&s->usb);
    VHDeviceDesc* out = vh_msg_new(sizeof(*out), VIRTUALHERE_MSG_DEVICE_DESC);

    out->header.device_id = cpu_to_le16(device_id);
    out->bind_result      = cpu_to_le32(VIRTUALHERE_BIND_OK);
    out->speed            = desc->speed;

    memcpy(out->device_desc, desc->device, MIN(desc->device_len, sizeof(out->device_desc)));
    memcpy(out->config_desc, desc->configs, MIN(desc->configs_len, sizeof(out->config_desc)));

    pstrcpy(out->product, sizeof(out->product),
            desc->product ?: (dev && dev->product_desc[0] ? dev->product_desc : "USB Device"));
    pstrcpy(out->serial, sizeof(out->serial), s->server_id);

    return out;
}

static void vh_conn_spawn(USBVirtualHereConn* conn, CoroutineEntry* fn)
{
    conn->refcount++;
    qemu_coroutine_enter(qemu_coroutine_create(fn, conn));
}

static USBVirtualHerePacket* vh_packet_new(USBVirtualHereConn* conn)
{
    USBVirtualHerePacket* pkt = g_new0(USBVirtualHerePacket, 1);

    conn->refcount++;
    pkt->conn = conn;
    usb_packet_init(&pkt->base.p);
    QLIST_INSERT_HEAD(&conn->packets, pkt, link);
    return pkt;
}

static void vh_packet_free(USBVirtualHerePacket* pkt)
{
    USBVirtualHereConn* conn = pkt->conn;

    QLIST_REMOVE(pkt, link);
    usb_packet_cleanup(&pkt->base.p);
    g_free(pkt->buffer);
    g_free(pkt);
    vh_conn_unref(conn);
}

static void coroutine_fn vh_send_ack(USBVirtualHerePacket* pkt, const void* msg, uint32_t size)
{
    vh_conn_send_frame(pkt->conn, 0, size, msg, size);
    vh_packet_free(pkt);
}

static void coroutine_fn vh_send_ctrl_ack_co(void* opaque)
{
    USBVirtualHerePacket* pkt    = opaque;
    uint32_t              length = (pkt->setup[0] & USB_DIR_IN) ? pkt->base.p.actual_length : 0;
    uint32_t              size   = sizeof(VHCtrlAck) + length;
    g_autofree VHCtrlAck* ack    = vh_msg_new_reply(size, VIRTUALHERE_MSG_CTRL_ACK, pkt->hdr);

    memcpy(ack->setup, pkt->setup, sizeof(ack->setup));
    ack->status   = cpu_to_le32(pkt->out_status);
    ack->data_len = cpu_to_le32(length);
    if (length) { memcpy(ack->data, pkt->buffer, length); }

    vh_send_ack(pkt, ack, size);
}

static void coroutine_fn vh_send_bulk_ack_co(void* opaque)
{
    USBVirtualHerePacket* pkt    = opaque;
    bool                  is_in  = (pkt->ep_addr & USB_DIR_IN) != 0;
    uint32_t              length = is_in ? pkt->base.p.actual_length : 0;
    uint32_t              size   = sizeof(VHBulkAck) + length;
    g_autofree VHBulkAck* ack    = vh_msg_new_reply(size, VIRTUALHERE_MSG_BULK_ACK, pkt->hdr);

    ack->status = cpu_to_le32(pkt->out_status < 0 ? pkt->out_status : (int32_t)pkt->base.p.actual_length);
    if (length) { memcpy(ack->data, pkt->buffer, length); }

    vh_send_ack(pkt, ack, size);
}

static void virtualhere_port_complete(USBPort* port, USBPacket* p)
{
    USBVirtualHerePacket* pkt;

    if (usb_uplink_packet_wake(p)) { return; }

    pkt             = container_of(container_of(p, USBUplinkPacket, p), USBVirtualHerePacket, base);
    pkt->out_status = usb_uplink_status_to_errno(p->status);

    Coroutine* co = qemu_coroutine_create(vh_send_bulk_ack_co, pkt);
    qemu_coroutine_enter(co);
}

/* The USB core orphans queued packets on detach; the client still wants an ack. */
static void vh_conn_abort_packets(USBVirtualHereConn* conn)
{
    QLIST_HEAD(, USBVirtualHerePacket) aborted = QLIST_HEAD_INITIALIZER(aborted);
    USBVirtualHerePacket *pkt, *next;

    /* Cancel everything first: answering one packet can free another's entry. */
    QLIST_FOREACH_SAFE(pkt, &conn->packets, link, next)
    {
        if (!usb_packet_is_inflight(&pkt->base.p)) { continue; }

        usb_cancel_packet(&pkt->base.p);
        pkt->base.p.actual_length = 0;
        pkt->out_status           = -ENODEV;

        QLIST_REMOVE(pkt, link);
        QLIST_INSERT_HEAD(&aborted, pkt, link);
    }

    while ((pkt = QLIST_FIRST(&aborted)) != NULL) {
        QLIST_REMOVE(pkt, link);
        QLIST_INSERT_HEAD(&conn->packets, pkt, link);
        qemu_coroutine_enter(qemu_coroutine_create(vh_send_bulk_ack_co, pkt));
    }
}

static bool coroutine_fn vh_conn_announce_device(USBVirtualHereConn* conn)
{
    USBVirtualHereState*        s          = conn->s;
    const USBUplinkDescriptors* desc       = usb_uplink_descriptors(&s->usb);
    g_autofree VHDeviceInfo*    short_info = NULL;

    if (desc == NULL) {
        VIRTUALHERE_DPRINTF("%s: no device with readable descriptors, nothing to announce\n", __func__);
        return true;
    }

    short_info = vh_build_short_info(s, conn, desc, VIRTUALHERE_SERVER_DEVICE_ID);

    VIRTUALHERE_DPRINTF("%s: announcing %04x:%04x \"%s\" class %u/%u/%u %u config(s) state=%s\n", __func__,
                        le16_to_cpu(short_info->vendor_id), le16_to_cpu(short_info->product_id), short_info->product,
                        short_info->iface_class, short_info->iface_subclass, short_info->iface_protocol,
                        desc->num_configs, conn->using_device ? "in-use" : "available");

    return vh_conn_send_lz4(conn, (const uint8_t*)short_info, sizeof(*short_info));
}

/* Answering instantly loses a race in the client and the claim is dropped. */
#define VIRTUALHERE_USE_DEVICE_REPLY_DELAY_NS (50 * 1000 * 1000)

/* A claim the client never hears back about hangs its hub entry forever. */
static bool coroutine_fn vh_conn_fail_claim(USBVirtualHereConn* conn)
{
    g_autofree VHDeviceDesc* fail = vh_msg_new(sizeof(*fail), VIRTUALHERE_MSG_DEVICE_DESC);

    fail->header.device_id = cpu_to_le16(VIRTUALHERE_SERVER_DEVICE_ID);
    fail->bind_result      = cpu_to_le32(VIRTUALHERE_BIND_ERROR);

    return vh_conn_send_lz4(conn, (const uint8_t*)fail, sizeof(*fail));
}

static bool coroutine_fn vh_handle_use_device(USBVirtualHereConn* conn)
{
    USBVirtualHereState*        s         = conn->s;
    const USBUplinkDescriptors* desc      = usb_uplink_descriptors(&s->usb);
    g_autofree VHDeviceDesc*    long_info = NULL;

    if (desc == NULL) {
        VIRTUALHERE_DPRINTF("%s: no readable descriptors, failing the claim\n", __func__);
        return vh_conn_fail_claim(conn);
    }

    conn->using_device = true;

    long_info = vh_build_long_info(s, desc, VIRTUALHERE_SERVER_DEVICE_ID);

    VIRTUALHERE_DPRINTF("%s: sending long-info (%zu bytes)\n", __func__, sizeof(*long_info));

    if (!vh_conn_send_lz4(conn, (const uint8_t*)long_info, sizeof(*long_info))) {
        VIRTUALHERE_DPRINTF("%s: failed to send long-info\n", __func__);
        return false;
    }
    VIRTUALHERE_DPRINTF("%s: long-info sent ok\n", __func__);
    return vh_conn_announce_device(conn);
}

static void coroutine_fn vh_ctrl_submit_co(void* opaque)
{
    USBVirtualHerePacket* pkt        = opaque;
    USBVirtualHereState*  s          = pkt->conn->s;
    const uint8_t*        setup      = pkt->setup;
    uint32_t              actual_len = 0;
    int32_t               status;

    WITH_QEMU_LOCK_GUARD(&s->usb.ctrl_lock)
    {
        status = usb_uplink_control(usb_uplink_active(&s->usb), setup[0], setup[1], lduw_le_p(setup + 2),
                                    lduw_le_p(setup + 4), lduw_le_p(setup + 6), pkt->buffer, &actual_len, false);
    }

    VIRTUALHERE_TRACE("%s: %02x %02x %04x %04x %04x -> status=%d len=%u\n", __func__, setup[0], setup[1],
                      lduw_le_p(setup + 2), lduw_le_p(setup + 4), lduw_le_p(setup + 6), status, actual_len);

    pkt->base.p.actual_length = actual_len;
    pkt->out_status           = usb_uplink_status_to_errno(status);

    vh_send_ctrl_ack_co(pkt);
}

static USBVirtualHerePacket* vh_submit_packet_new(USBVirtualHereConn* conn, const uint8_t* msg, uint32_t len,
                                                  uint32_t data_off, uint32_t xfer_len, bool is_out)
{
    USBVirtualHerePacket* pkt = vh_packet_new(conn);

    memcpy(pkt->hdr, msg, VIRTUALHERE_HDR_LEN);
    pkt->buffer   = g_malloc0(xfer_len ?: 1);
    pkt->xfer_len = xfer_len;

    if (is_out && len > data_off) { memcpy(pkt->buffer, msg + data_off, MIN(len - data_off, xfer_len)); }

    return pkt;
}

static bool coroutine_fn vh_handle_ctrl_submit(USBVirtualHereConn* conn, const uint8_t* msg, uint32_t len)
{
    const VHCtrlSubmit*   sub = (const VHCtrlSubmit*)msg;
    USBVirtualHerePacket* pkt;

    if (len < sizeof(*sub)) {
        VIRTUALHERE_DPRINTF("%s: short ctrl submit (%u bytes)\n", __func__, len);
        return true;
    }

    if (usb_uplink_active(&conn->s->usb) == NULL) { return true; }

    pkt = vh_submit_packet_new(conn, msg, len, offsetof(VHCtrlSubmit, data), lduw_le_p(sub->setup + 6),
                               !(sub->setup[0] & USB_DIR_IN));
    memcpy(pkt->setup, sub->setup, sizeof(pkt->setup));

    qemu_coroutine_enter(qemu_coroutine_create(vh_ctrl_submit_co, pkt));

    return true;
}

static void coroutine_fn vh_bulk_submit_co(void* opaque)
{
    USBVirtualHerePacket* pkt = opaque;
    int32_t status = usb_uplink_transfer(&pkt->conn->s->usb, &pkt->base, pkt->ep_addr, pkt->buffer, pkt->xfer_len,
                                         &pkt->conn->closed);

    if (status == USB_RET_ASYNC) { return; }

    /* NODEV just means the gadget went away under us, which is normal teardown. */
    if (status == USB_RET_NODEV) { VIRTUALHERE_TRACE("%s: ep 0x%02x gone\n", __func__, pkt->ep_addr); }
    else if (status != USB_RET_SUCCESS) {
        VIRTUALHERE_DPRINTF("%s: ep 0x%02x failed, status=%d\n", __func__, pkt->ep_addr, status);
    }

    pkt->out_status = usb_uplink_status_to_errno(status);
    vh_send_bulk_ack_co(pkt);
}

static bool coroutine_fn vh_handle_bulk_submit(USBVirtualHereConn* conn, const uint8_t* msg, uint32_t len)
{
    const VHBulkSubmit*   sub = (const VHBulkSubmit*)msg;
    USBVirtualHerePacket* pkt;
    uint8_t               endpoint;
    uint32_t              length;

    if (len < sizeof(*sub)) {
        VIRTUALHERE_DPRINTF("%s: short bulk submit (%u bytes)\n", __func__, len);
        return true;
    }

    if (usb_uplink_active(&conn->s->usb) == NULL) { return true; }

    endpoint = le32_to_cpu(sub->endpoint);
    length   = le32_to_cpu(sub->length);

    VIRTUALHERE_TRACE("%s: ep 0x%02x len %u\n", __func__, endpoint, length);

    pkt          = vh_submit_packet_new(conn, msg, len, offsetof(VHBulkSubmit, data), length, !(endpoint & USB_DIR_IN));
    pkt->ep_addr = endpoint;

    qemu_coroutine_enter(qemu_coroutine_create(vh_bulk_submit_co, pkt));

    return true;
}

typedef struct VHRemovalNotice
{
    USBVirtualHereConn* conn;
    VHDeviceInfo*       msg;
} VHRemovalNotice;

static void coroutine_fn vh_notify_device_removed_co(void* opaque)
{
    g_autofree VHRemovalNotice* notice = opaque;
    g_autofree VHDeviceInfo*    gone   = notice->msg;

    vh_conn_send_lz4(notice->conn, (const uint8_t*)gone, sizeof(*gone));
    vh_conn_unref(notice->conn);
}

/* The reference server removes a device by re-sending its announce with state 0. */
static void vh_notify_device_removed(USBVirtualHereConn* conn)
{
    USBVirtualHereState*        s      = conn->s;
    const USBUplinkDescriptors* desc   = usb_uplink_descriptors_cached(&s->usb);
    VHRemovalNotice*            notice = g_new0(VHRemovalNotice, 1);

    notice->conn = conn;
    notice->msg  = desc != NULL ? vh_build_short_info(s, conn, desc, VIRTUALHERE_SERVER_DEVICE_ID)
                                : vh_msg_new(sizeof(VHDeviceInfo), VIRTUALHERE_MSG_DEVICE_INFO);

    notice->msg->device_id = cpu_to_le16(VIRTUALHERE_SERVER_DEVICE_ID);
    notice->msg->state     = VIRTUALHERE_DEVICE_STATE_GONE;

    conn->refcount++;
    qemu_coroutine_enter(qemu_coroutine_create(vh_notify_device_removed_co, notice));
}

/* Attach runs before the core finishes wiring the device, so announce off a BH. */
static void coroutine_fn vh_announce_attach_co(void* opaque)
{
    USBVirtualHereConn* conn = opaque;

    if (!conn->closed) { vh_conn_announce_device(conn); }
    vh_conn_unref(conn);
}

static void vh_announce_attach_bh(void* opaque)
{ qemu_coroutine_enter(qemu_coroutine_create(vh_announce_attach_co, opaque)); }

#define VIRTUALHERE_USE_DEVICE_WAIT_NS (10ULL * 1000 * 1000 * 1000)
#define VIRTUALHERE_USE_DEVICE_POLL_NS (50 * 1000 * 1000)

static void coroutine_fn vh_use_device_co(void* opaque)
{
    USBVirtualHereConn* conn   = opaque;
    int64_t             giveup = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + VIRTUALHERE_USE_DEVICE_WAIT_NS;

    qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, VIRTUALHERE_USE_DEVICE_REPLY_DELAY_NS);

    while (!conn->closed && usb_uplink_descriptors(&conn->s->usb) == NULL
           && qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < giveup)
    {
        qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, VIRTUALHERE_USE_DEVICE_POLL_NS);
    }

    if (!conn->closed) { vh_handle_use_device(conn); }
    conn->use_pending = false;
    vh_conn_unref(conn);
}

static void coroutine_fn vh_heartbeat_co(void* opaque)
{
    USBVirtualHereConn* conn  = opaque;
    uint32_t            stamp = cpu_to_le32(qemu_clock_get_ms(QEMU_CLOCK_REALTIME) / 1000);

    if (vh_conn_send_msg(conn, VIRTUALHERE_MSG_TIME_PING, NULL, &stamp, sizeof(stamp))
        && (!conn->using_device || usb_uplink_descriptors(&conn->s->usb) != NULL))
    {
        vh_conn_announce_device(conn);
    }

    vh_conn_unref(conn);
}

static void vh_heartbeat_cb(void* opaque)
{
    USBVirtualHereConn* conn = opaque;

    if (conn->closed) { return; }

    /* Re-arm first: the beat itself can fail the socket and free this timer. */
    timer_mod(conn->heartbeat_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + VIRTUALHERE_HEARTBEAT_PERIOD_MS);

    vh_conn_spawn(conn, vh_heartbeat_co);
}

static void vh_copy_field(char* dst, size_t dst_size, const uint8_t* msg, uint32_t len, uint32_t off)
{
    if (len < off + dst_size) { return; }

    memcpy(dst, msg + off, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool coroutine_fn vh_conn_handshake(USBVirtualHereConn* conn, QIOChannel* ioc)
{
    USBVirtualHereState*     s        = conn->s;
    g_autofree uint8_t*      msg      = NULL;
    g_autofree VHServerInfo* hub_info = NULL;
    uint32_t                 len;

    if (!vh_read_message(ioc, &msg, &len)) {
        VIRTUALHERE_DPRINTF("%s: failed to read ClientHello (connection dropped?)\n", __func__);
        return false;
    }

    if (msg[0] != VIRTUALHERE_MSG_CLIENT_HELLO) {
        VIRTUALHERE_DPRINTF("%s: expected ClientHello (0x00), got 0x%02x\n", __func__, msg[0]);
        return false;
    }

    vh_copy_field(conn->client_name, sizeof(conn->client_name), msg, len, offsetof(VHClientHello, user));
    vh_copy_field(conn->client_host, sizeof(conn->client_host), msg, len, offsetof(VHClientHello, host));

    VIRTUALHERE_DPRINTF("%s: got ClientHello from \"%s\" on \"%s\", sending server announce\n", __func__,
                        conn->client_name, conn->client_host);

    hub_info = vh_build_server_announce(s);
    if (!vh_conn_send_lz4(conn, (const uint8_t*)hub_info, sizeof(*hub_info))) { return false; }

    vh_conn_send_msg(conn, VIRTUALHERE_MSG_SERVER_NOTICE, NULL, VIRTUALHERE_SERVER_BANNER,
                     sizeof(VIRTUALHERE_SERVER_BANNER));

    usb_uplink_descriptors(&s->usb);

    return true;
}

static void coroutine_fn vh_conn_dispatch(USBVirtualHereConn* conn, const uint8_t* msg, uint32_t len)
{
    switch (msg[0]) {
        case VIRTUALHERE_MSG_READY      : vh_conn_announce_device(conn); break;
        case VIRTUALHERE_MSG_HEARTBEAT  : vh_conn_send_msg(conn, VIRTUALHERE_MSG_HEARTBEAT_ACK, msg, NULL, 0); break;
        case VIRTUALHERE_MSG_TIME_PONG  : break;
        case VIRTUALHERE_MSG_USE_DEVICE:
            /* Retries arrive while the first claim is still waiting on descriptors. */
            if (!conn->using_device && !conn->use_pending) {
                conn->use_pending = true;
                vh_conn_spawn(conn, vh_use_device_co);
            }
            break;
        case VIRTUALHERE_MSG_GADGET_LIST: {
            uint32_t count = 0;

            vh_conn_send_msg(conn, VIRTUALHERE_MSG_GADGET_LIST_ACK, msg, &count, sizeof(count));
            break;
        }
        case VIRTUALHERE_MSG_DEVICE_INIT: {
            uint8_t status[4] = {0};

            vh_conn_send_msg(conn, VIRTUALHERE_MSG_DEVICE_INIT_ACK, msg, status, sizeof(status));
            break;
        }
        case VIRTUALHERE_MSG_STOP_USING_DEVICE: {
            uint8_t released[4] = {1, 0, 0, 0};

            conn->using_device = false;
            conn->use_pending  = false;
            vh_conn_abort_packets(conn);
            usb_uplink_release(&conn->s->usb);
            vh_conn_send_msg(conn, VIRTUALHERE_MSG_UNBIND_ACK, msg, released, sizeof(released));
            vh_conn_announce_device(conn);
            break;
        }
        case VIRTUALHERE_MSG_CTRL_SUBMIT: vh_handle_ctrl_submit(conn, msg, len); break;
        case VIRTUALHERE_MSG_BULK_SUBMIT: vh_handle_bulk_submit(conn, msg, len); break;
        default:
            VIRTUALHERE_DPRINTF("%s: unhandled message type 0x%02x len %u\n", __func__, msg[0], len);
            vh_debug_hexdump(msg, len);
            break;
    }
}

static void coroutine_fn vh_conn_msg_loop_co(void* opaque)
{
    USBVirtualHereConn* conn  = opaque;
    g_autoptr(QIOChannel) ioc = conn->ioc;

    object_ref(OBJECT(ioc));

    if (!vh_conn_handshake(conn, ioc)) { goto out; }

    timer_mod(conn->heartbeat_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + VIRTUALHERE_HEARTBEAT_PERIOD_MS);

    while (!conn->closed) {
        g_autofree uint8_t* msg = NULL;
        uint32_t            len;

        if (!vh_read_message(ioc, &msg, &len)) { break; }

        VIRTUALHERE_TRACE("%s: recv type 0x%02x len %u\n", __func__, msg[0], len);

        if (len < VIRTUALHERE_MSG_MIN_SIZE) {
            error_report("hcd-virtualhere: runt message (type 0x%02x, %u bytes), dropping connection", msg[0], len);
            break;
        }

        vh_conn_dispatch(conn, msg, len);
    }

out:
    vh_conn_close(conn);
    vh_conn_unref(conn);
}

static void vh_conn_close(USBVirtualHereConn* conn)
{
    USBVirtualHereState* s = conn->s;

    if (conn->closed) { return; }
    conn->closed = true;

    vh_conn_abort_packets(conn);

    /* The claim dies with the connection, so the device must come back clean. */
    if (conn->using_device || conn->use_pending) {
        conn->using_device = false;
        conn->use_pending  = false;
        usb_uplink_release(&s->usb);
    }

    qio_channel_shutdown(conn->ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);

    if (s->active_conn == conn) { s->active_conn = NULL; }

    if (conn->heartbeat_timer) {
        timer_free(conn->heartbeat_timer);
        conn->heartbeat_timer = NULL;
    }
}

static void vh_conn_unref(USBVirtualHereConn* conn)
{
    if (--conn->refcount > 0) { return; }

    object_unref(OBJECT(conn->ioc));
    g_free(conn);
}

static void virtualhere_accept(QIONetListener* listener, QIOChannelSocket* cioc, void* opaque)
{
    USBVirtualHereState* s = opaque;
    USBVirtualHereConn*  conn;
    Coroutine*           co;

    VIRTUALHERE_DPRINTF("%s: incoming connection, active_conn=%p\n", __func__, s->active_conn);

    if (s->active_conn != NULL) {
        qio_channel_close(QIO_CHANNEL(cioc), NULL);
        return;
    }

    conn           = g_new0(USBVirtualHereConn, 1);
    conn->refcount = 1;
    conn->s        = s;
    conn->ioc      = QIO_CHANNEL(cioc);
    object_ref(OBJECT(conn->ioc));
    qio_channel_set_blocking(conn->ioc, false, NULL);
    qemu_co_mutex_init(&conn->write_mutex);
    QLIST_INIT(&conn->packets);
    conn->heartbeat_timer = timer_new_ms(QEMU_CLOCK_REALTIME, vh_heartbeat_cb, conn);

    s->active_conn = conn;

    co = qemu_coroutine_create(vh_conn_msg_loop_co, conn);
    qemu_coroutine_enter(co);
}

/*
 * A dwc3 run/stop toggle detaches and re-attaches the same device, so a device
 * that comes straight back has re-enumerated rather than been unplugged.
 */
#define VIRTUALHERE_UNPLUG_SETTLE_MS 1000

static void vh_unplug_timeout(void* opaque)
{
    USBVirtualHereState* s    = opaque;
    USBVirtualHereConn*  conn = s->active_conn;

    if (conn != NULL && !conn->closed) {
        conn->using_device = false;
        conn->use_pending  = false;
        vh_notify_device_removed(conn);
    }

    usb_uplink_invalidate(&s->usb);
}

static void usb_virtualhere_attach(USBPort* port)
{
    USBVirtualHereState* s = port->opaque;

    VIRTUALHERE_DPRINTF("%s: port[%d]->dev=%p attached=%d active=%p\n", __func__, port->index, port->dev,
                        port->dev ? port->dev->attached : -1, usb_uplink_active(&s->usb));

    if (port->dev == NULL || !port->dev->attached) { return; }

    timer_del(s->unplug_timer);
    usb_uplink_invalidate(&s->usb);

    if (s->active_conn != NULL && !s->active_conn->closed) {
        s->active_conn->refcount++;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), vh_announce_attach_bh, s->active_conn);
    }
}

static void usb_virtualhere_detach(USBPort* port)
{
    USBVirtualHereState* s    = port->opaque;
    USBVirtualHereConn*  conn = s->active_conn;

    VIRTUALHERE_DPRINTF("%s: port[%d] detached, remaining=%p\n", __func__, port->index,
                        usb_uplink_active_except(&s->usb, port));

    if (conn != NULL && !conn->closed) { vh_conn_abort_packets(conn); }

    if (usb_uplink_active_except(&s->usb, port) != NULL) {
        usb_uplink_invalidate(&s->usb);
        return;
    }

    /* The descriptors are kept until it settles, so a real unplug can name it. */
    timer_mod(s->unplug_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + VIRTUALHERE_UNPLUG_SETTLE_MS);
}

static USBBusOps  usb_virtualhere_bus_ops  = {};
static USBPortOps usb_virtualhere_port_ops = {
    .attach   = usb_virtualhere_attach,
    .detach   = usb_virtualhere_detach,
    .complete = virtualhere_port_complete,
};

static bool virtualhere_listen(USBVirtualHereState* s, Error** errp)
{
    g_autofree char* path         = object_get_canonical_path(OBJECT(s));
    g_autoptr(SocketAddress) addr = NULL;
    Error* local_err              = NULL;

    addr = socket_parse(s->addr, errp);
    if (addr == NULL) { return false; }
    if (addr->type != SOCKET_ADDRESS_TYPE_INET) {
        error_setg(errp, "VirtualHere only supports TCP/IP sockets");
        return false;
    }

    s->listener = qio_net_listener_new();
    if (qio_net_listener_open_sync(s->listener, addr, 1, &local_err) < 0) {
        error_propagate_prepend(errp, local_err, "hcd-virtualhere (%s): failed to listen on %s: ", path, s->addr);
        return false;
    }

    info_report("hcd-virtualhere (%s): listening on %s", path, s->addr);
    qio_net_listener_set_client_func(s->listener, virtualhere_accept, s, NULL);

    /* Discovery is a convenience; a client can still be pointed at us by hand. */
    s->mdns = mdns_service_register(s->server_name, VIRTUALHERE_SERVICE_TYPE, atoi(addr->u.inet.port), s->server_id,
                                    &local_err);
    if (s->mdns == NULL) { warn_reportf_err(local_err, "hcd-virtualhere (%s): not advertising over mDNS: ", path); }

    return true;
}

static void usb_virtualhere_realize(DeviceState* dev, Error** errp)
{
    USBVirtualHereState* s = USB_VIRTUALHERE(dev);

    if (s->server_name == NULL) { s->server_name = g_strdup("Inferno"); }
    if (s->server_id == NULL) { s->server_id = g_strdup("INFERNO001"); }
    if (s->server_host == NULL) { s->server_host = g_strdup(g_get_host_name()); }
    if (s->addr == NULL) { s->addr = g_strdup(VIRTUALHERE_SERVER_DEFAULT_ADDR); }
    vh_init_server_guid(s);

    usb_uplink_init(&s->usb, dev, &usb_virtualhere_bus_ops, &usb_virtualhere_port_ops, s);
    s->unplug_timer = timer_new_ms(QEMU_CLOCK_REALTIME, vh_unplug_timeout, s);

    if (!virtualhere_listen(s, errp)) { return; }

    s->exit_notifier.notify = usb_virtualhere_exit_notifier;
    qemu_add_exit_notifier(&s->exit_notifier);
}

static void usb_virtualhere_teardown(USBVirtualHereState* s)
{
    if (s->unplug_timer != NULL) {
        timer_free(s->unplug_timer);
        s->unplug_timer = NULL;
    }

    if (s->active_conn != NULL) { vh_conn_close(s->active_conn); }

    mdns_service_unregister(s->mdns);
    s->mdns = NULL;

    if (s->listener) {
        qio_net_listener_disconnect(s->listener);
        object_unref(OBJECT(s->listener));
        s->listener = NULL;
    }
}

static void usb_virtualhere_exit_notifier(Notifier* n, void* data)
{
    USBVirtualHereState* s = container_of(n, USBVirtualHereState, exit_notifier);

    usb_virtualhere_teardown(s);
}

static void usb_virtualhere_unrealize(DeviceState* dev)
{
    USBVirtualHereState* s = USB_VIRTUALHERE(dev);

    qemu_remove_exit_notifier(&s->exit_notifier);

    usb_virtualhere_teardown(s);
    usb_uplink_invalidate(&s->usb);
}

static const Property usb_virtualhere_props[] = {
    DEFINE_PROP_STRING("addr", USBVirtualHereState, addr),
    DEFINE_PROP_STRING("server-name", USBVirtualHereState, server_name),
    DEFINE_PROP_STRING("server-id", USBVirtualHereState, server_id),
    DEFINE_PROP_STRING("server-host", USBVirtualHereState, server_host),
};

static void usb_virtualhere_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize   = usb_virtualhere_realize;
    dc->unrealize = usb_virtualhere_unrealize;
    dc->desc      = "VirtualHere-protocol USB server";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_props(dc, usb_virtualhere_props);
}

static const TypeInfo usb_virtualhere_type_info = {
    .name          = TYPE_USB_VIRTUALHERE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(USBVirtualHereState),
    .class_init    = usb_virtualhere_class_init,
};

static void usb_virtualhere_register_types(void) { type_register_static(&usb_virtualhere_type_info); }

type_init(usb_virtualhere_register_types)
