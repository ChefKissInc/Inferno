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

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/hcd-inferno.h"
#include "io/channel-util.h"
#include "io/channel.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qom/object.h"
#include "system/iothread.h"
#include "hw/usb/inferno-proto.h"

// #define DEBUG_HCD_INFERNO

#ifdef DEBUG_HCD_INFERNO
    #define DPRINTF(fmt, ...)                                    \
        do {                                                     \
            fprintf(stderr, "hcd-inferno: " fmt, ##__VA_ARGS__); \
        }                                                        \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

#define USB_INFERNO_HOST_RETRY_MS 1000

static void usb_inferno_host_arm_retry(USBInfernoHostState* s);

static bool usb_inferno_host_bus_populated(USBInfernoHostState* s);
static void usb_inferno_host_reset_bus(USBInfernoHostState* s);

static void usb_inferno_host_closed(USBInfernoHostState* s)
{
    QIOChannel* ioc = s->ioc;

    DPRINTF("%s\n", __func__);

    s->closed = true;

    if (ioc != NULL) {
        s->ioc = NULL;
        qio_channel_shutdown(ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        qio_channel_wake_read(ioc);
        qio_channel_wake_write(ioc);

        object_unref(OBJECT(ioc));
    }

    if (!s->stopped && usb_inferno_host_bus_populated(s)) { usb_inferno_host_arm_retry(s); }
}

static ssize_t coroutine_fn inferno_read(QIOChannel* ioc, void* buf, size_t len)
{
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    ssize_t      ret = -1;
    Error*       err = NULL;

    assert_true(qemu_in_coroutine());
    assert_true(ioc != NULL);

    ret = qio_channel_readv_full_all_eof(ioc, &iov, 1, NULL, 0, 0, &err);

    if (err) { error_report_err(err); }
    return (ret <= 0) ? ret : iov.iov_len;
}

static bool coroutine_fn inferno_write(QIOChannel* ioc, void* buf, ssize_t len)
{
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    bool         ret = false;
    Error*       err = NULL;

    assert_true(qemu_in_coroutine());
    assert_true(ioc != NULL);

    if (!qio_channel_writev_full_all(ioc, &iov, 1, NULL, 0, 0, &err)) { ret = true; }

    if (err) { error_report_err(err); }
    return ret;
}

static USBDevice* usb_inferno_host_find_device(USBInfernoHostState* s, uint8_t addr)
{
    for (int i = 0; i < G_N_ELEMENTS(s->ports) - 1; i++) {
        USBDevice* dev = s->ports[i].dev;

        if (dev == NULL || !dev->attached) { continue; }
        if (dev->state != USB_STATE_DEFAULT) { continue; }
        if (dev->addr == addr) { return dev; }

        USBDevice* sub = usb_device_find_device(dev, addr);
        if (sub != NULL) { return sub; }
    }
    return NULL;
}

static void coroutine_fn usb_inferno_host_respond_error(USBInfernoHostState* s, inferno_request_header* req,
                                                        uint32_t status)
{
    inferno_header_t        hdr  = {0};
    inferno_response_header resp = {0};
    g_autoptr(QIOChannel) ioc    = NULL;

    ioc = s->ioc;
    if (ioc == NULL) { return; }
    object_ref(OBJECT(ioc));

    hdr.type    = INFERNO_RESPONSE;
    resp.addr   = req->addr;
    resp.pid    = req->pid;
    resp.ep     = req->ep;
    resp.id     = req->id;
    resp.status = status;
    resp.length = 0;

    WITH_QEMU_LOCK_GUARD(&s->write_mutex)
    {
        if (!s->closed) {
            if (!inferno_write(ioc, &hdr, sizeof(hdr)) || !inferno_write(ioc, &resp, sizeof(resp))) {
                usb_inferno_host_closed(s);
            }
        }
    }
}

static void coroutine_fn usb_inferno_host_respond_packet_co(void* opaque)
{
    USBInfernoPacket*       pkt    = opaque;
    USBInfernoHostState*    s      = pkt->s;
    USBPacket*              p      = &pkt->p;
    inferno_header_t        hdr    = {0};
    inferno_response_header resp   = {0};
    g_autofree void*        buffer = NULL;
    g_autoptr(QIOChannel) ioc      = NULL;

    ioc = s->ioc;
    if (ioc != NULL) { object_ref(OBJECT(ioc)); }

    WITH_QEMU_LOCK_GUARD(&s->write_mutex)
    {
        if (!s->closed && ioc != NULL) {
            hdr.type    = INFERNO_RESPONSE;
            resp.addr   = pkt->addr;
            resp.pid    = p->pid;
            resp.ep     = p->ep->nr;
            resp.id     = p->id;
            resp.status = p->status;
            resp.length = p->iov.size;

            if (resp.length > p->actual_length) { resp.length = p->actual_length; }

            if (p->pid == USB_TOKEN_IN && p->status != USB_RET_ASYNC) {
                buffer = g_malloc(resp.length);
                iov_to_buf(p->iov.iov, p->iov.niov, 0, buffer, resp.length);
            }

            if (!inferno_write(ioc, &hdr, sizeof(hdr))) {
                usb_inferno_host_closed(s);
                break;
            }

            if (!inferno_write(ioc, &resp, sizeof(resp))) {
                usb_inferno_host_closed(s);
                break;
            }

            if (buffer) {
                if (!inferno_write(ioc, buffer, resp.length)) {
                    usb_inferno_host_closed(s);
                    break;
                }
            }
        }
    }

    if (!usb_packet_is_inflight(p)) {
        if (pkt->buffer) { g_free(pkt->buffer); }
        usb_packet_cleanup(p);
        g_free(pkt);
    }
}

static void usb_inferno_host_respond_packet(USBInfernoHostState* s, USBInfernoPacket* pkt)
{
    Coroutine* co = NULL;
    co            = qemu_coroutine_create(usb_inferno_host_respond_packet_co, pkt);
    qemu_coroutine_enter(co);
}

static void coroutine_fn usb_inferno_host_msg_loop_co(void* opaque)
{
    USBInfernoHostState* s;
    g_autoptr(QIOChannel) ioc = NULL;
    inferno_header_t hdr;

    s   = opaque;
    ioc = s->ioc;
    if (ioc == NULL) { return; }
    object_ref(OBJECT(ioc));

    for (;;) {
        if (unlikely((inferno_read(ioc, &hdr, sizeof(hdr)) != sizeof(hdr)))) {
            usb_inferno_host_closed(s);
            return;
        }

        switch (hdr.type) {
            case INFERNO_REQUEST: {
                inferno_request_header       pkt_hdr;
                g_autofree void*             buffer = NULL;
                g_autofree USBInfernoPacket* pkt    = g_new0(USBInfernoPacket, 1);
                USBEndpoint*                 ep     = NULL;
                USBDevice*                   dev    = NULL;

                if (unlikely(inferno_read(ioc, &pkt_hdr, sizeof(pkt_hdr)) != sizeof(pkt_hdr))) {
                    usb_inferno_host_closed(s);
                    return;
                }

                DPRINTF("%s: INFERNO_REQUEST addr: %d pid: 0x%x ep: %d id: 0x%" PRIx64 "\n", __func__, pkt_hdr.addr,
                        pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id);

                dev = usb_inferno_host_find_device(s, pkt_hdr.addr);
                ep  = (dev == NULL) ? NULL : usb_ep_get(dev, pkt_hdr.pid, pkt_hdr.ep);
                if (ep == NULL) {
                    if (pkt_hdr.length > 0 && pkt_hdr.pid != USB_TOKEN_IN) {
                        g_autofree void* discard = g_malloc0(pkt_hdr.length);
                        if (unlikely(inferno_read(ioc, discard, pkt_hdr.length) != pkt_hdr.length)) {
                            usb_inferno_host_closed(s);
                            return;
                        }
                    }
                    DPRINTF("%s: INFERNO_REQUEST no device at addr %d ep %d\n", __func__, pkt_hdr.addr, pkt_hdr.ep);
                    usb_inferno_host_respond_error(s, &pkt_hdr, USB_RET_NODEV);
                    break;
                }

                usb_packet_init(&pkt->p);
                usb_packet_setup(&pkt->p, pkt_hdr.pid, ep, pkt_hdr.stream, pkt_hdr.id, pkt_hdr.short_not_ok,
                                 pkt_hdr.int_req);

                if (pkt_hdr.length > 0) {
                    buffer = g_malloc0(pkt_hdr.length);

                    if (pkt_hdr.pid != USB_TOKEN_IN) {
                        if (unlikely(inferno_read(ioc, buffer, pkt_hdr.length) != pkt_hdr.length)) {
                            usb_inferno_host_closed(s);
                            usb_packet_cleanup(&pkt->p);
                            return;
                        }
                    }

                    usb_packet_addbuf(&pkt->p, buffer, pkt_hdr.length);
                    pkt->buffer = buffer;
                    g_steal_pointer(&buffer);
                }

                pkt->dev  = ep->dev;
                pkt->s    = s;
                pkt->addr = pkt_hdr.addr;
                assert_true(bql_locked());

                usb_handle_packet(pkt->dev, &pkt->p);
                usb_inferno_host_respond_packet(s, pkt);
                g_steal_pointer(&pkt);
                break;
            }
            case INFERNO_RESPONSE:
                fprintf(stderr, "%s: unexpected INFERNO_RESPONSE\n", __func__);
                usb_inferno_host_closed(s);
                return;
            case INFERNO_CANCEL: {
                inferno_cancel_header pkt_hdr = {0};
                USBInfernoPacket*     pkt     = NULL;
                USBPacket*            p       = NULL;

                if (unlikely(inferno_read(ioc, &pkt_hdr, sizeof(pkt_hdr)) != sizeof(pkt_hdr))) {
                    usb_inferno_host_closed(s);
                    return;
                }

                DPRINTF("%s: INFERNO_CANCEL pid: 0x%x ep: %d\n", __func__, pkt_hdr.pid, pkt_hdr.ep);

                assert_true(bql_locked());
                USBDevice* dev = usb_inferno_host_find_device(s, pkt_hdr.addr);
                p = (dev == NULL) ? NULL : usb_ep_find_packet_by_id(dev, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id);
                if (p) {
                    pkt = container_of(p, USBInfernoPacket, p);
                    usb_cancel_packet(&pkt->p);
                    DPRINTF("%s: INFERNO_CANCEL: packet"
                            " pid: 0x%x ep: %d id: 0x%" PRIx64 " len: 0x%x\n",
                            __func__, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id, p->actual_length);
                    usb_inferno_host_respond_packet(s, pkt);
                }
                else {
                    warn_report("%s: INFERNO_CANCEL: packet"
                                " pid: 0x%x ep: %d id: 0x%" PRIx64 " not found",
                                __func__, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id);
                }
                break;
            }
            case INFERNO_RESET:
                DPRINTF("%s: INFERNO_RESET\n", __func__);
                assert_true(bql_locked());
                usb_inferno_host_reset_bus(s);
                break;
            default: assert_not_reached(); break;
        }
    }

    return;
}

static bool usb_inferno_host_bus_populated(USBInfernoHostState* s)
{
    for (int i = 0; i < G_N_ELEMENTS(s->ports) - 1; i++) {
        USBDevice* dev = s->ports[i].dev;
        if (dev != NULL && dev->attached) { return true; }
    }

    return false;
}

static void usb_inferno_host_reset_bus(USBInfernoHostState* s)
{
    for (int i = 0; i < G_N_ELEMENTS(s->ports) - 1; i++) {
        USBDevice* dev = s->ports[i].dev;
        if (dev != NULL && dev->attached) {
            DPRINTF("%s: resetting port %d\n", __func__, i);
            usb_device_reset(dev);
        }
    }
}

static void usb_inferno_host_reset_bus_bh(void* opaque) { usb_inferno_host_reset_bus(opaque); }

static bool usb_inferno_host_try_connect(USBInfernoHostState* s)
{
    int         sock;
    Coroutine*  co;
    QIOChannel* ioc;
    Error*      err = NULL;

    sock = socket_connect(s->sockaddr, &err);
    if (sock == -1) {
        error_free(err);
        return false;
    }

    if (s->sockaddr->type == SOCKET_ADDRESS_TYPE_INET && socket_set_nodelay(sock) < 0) {
        warn_report("Failed to set nodelay for socket: %s", strerror(errno));
    }

    ioc = qio_channel_new_fd(sock, &err);
    if (ioc == NULL) {
        error_report_err(err);
        close(sock);
        return false;
    }

    qio_channel_set_blocking(ioc, false, NULL);
    s->closed = false;
    s->ioc    = ioc;

    qemu_bh_schedule(s->reset_bh);

    co = qemu_coroutine_create(usb_inferno_host_msg_loop_co, s);
    qemu_coroutine_enter(co);
    return true;
}

static void usb_inferno_host_retry_cb(void* opaque)
{
    USBInfernoHostState* s = opaque;

    if (!s->closed || s->stopped) { return; }
    if (!usb_inferno_host_bus_populated(s)) { return; }

    if (usb_inferno_host_try_connect(s)) {
        DPRINTF("%s: connected\n", __func__);
        return;
    }
    timer_mod(s->retry_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + USB_INFERNO_HOST_RETRY_MS);
}

static void usb_inferno_host_arm_retry(USBInfernoHostState* s)
{
    if (s->retry_timer == NULL || s->stopped) { return; }
    timer_mod(s->retry_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + USB_INFERNO_HOST_RETRY_MS);
}

static void usb_inferno_host_attach(USBPort* port)
{
    USBInfernoHostState* s = port->opaque;

    if (port->index >= G_N_ELEMENTS(s->ports) - 1) {
        error_report("%s: attached to unused port\n", __func__);
        return;
    }

    if (port->dev == NULL || !port->dev->attached) { return; }

    if (!s->closed) {
        DPRINTF("%s: port %d joining the existing connection\n", __func__, port->index);
        qemu_bh_schedule(s->reset_bh);
        return;
    }

    if (!usb_inferno_host_try_connect(s)) { usb_inferno_host_arm_retry(s); }
}

static void usb_inferno_host_detach(USBPort* port)
{
    USBInfernoHostState* s;

    s = port->opaque;

    for (int i = 0; i < G_N_ELEMENTS(s->ports) - 1; i++) {
        USBDevice* dev = s->ports[i].dev;
        if (dev != NULL && dev->attached && &s->ports[i] != port) {
            DPRINTF("%s: port %d detached, connection still in use\n", __func__, port->index);
            return;
        }
    }

    usb_inferno_host_closed(s);
}

static void usb_inferno_host_async_packet_complete(USBPort* port, USBPacket* p)
{
    USBInfernoHostState* s;

    s = port->opaque;

    usb_inferno_host_respond_packet(s, container_of(p, USBInfernoPacket, p));
}

static USBBusOps usb_inferno_bus_ops = {};

static USBPortOps usb_inferno_host_port_ops = {
    .attach       = usb_inferno_host_attach,
    .detach       = usb_inferno_host_detach,
    .child_detach = NULL,
    .wakeup       = NULL,
    .complete     = usb_inferno_host_async_packet_complete,
};

static void usb_inferno_host_realize(DeviceState* dev, Error** errp)
{
    USBInfernoHostState* s;
    int                  i;

    s = USB_INFERNO_HOST(dev);

    if (s->connect_addr == NULL) {
        s->connect_addr = g_strdup(USB_INFERNO_REMOTE_ADDR_DEFAULT);
        warn_report("No address specified, using default (`%s`).", USB_INFERNO_REMOTE_ADDR_DEFAULT);
    }

    s->sockaddr = socket_parse(s->connect_addr, errp);
    if (s->sockaddr == NULL) { return; }

    usb_bus_new(&s->bus, sizeof(s->bus), &usb_inferno_bus_ops, dev);
    for (i = 0; i < G_N_ELEMENTS(s->ports); i++) {
        usb_register_port(&s->bus, &s->ports[i], s, i, &usb_inferno_host_port_ops,
                          USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL | USB_SPEED_MASK_HIGH | USB_SPEED_MASK_SUPER);
    }

    s->closed      = true;
    s->retry_timer = timer_new_ms(QEMU_CLOCK_REALTIME, usb_inferno_host_retry_cb, s);
    s->reset_bh    = qemu_bh_new(usb_inferno_host_reset_bus_bh, s);
    qemu_co_mutex_init(&s->write_mutex);
}

static void usb_inferno_host_unrealize(DeviceState* dev)
{
    USBInfernoHostState* s = USB_INFERNO_HOST(dev);

    s->stopped = true;

    usb_inferno_host_closed(s);

    if (s->retry_timer != NULL) {
        timer_free(s->retry_timer);
        s->retry_timer = NULL;
    }

    if (s->reset_bh != NULL) {
        qemu_bh_delete(s->reset_bh);
        s->reset_bh = NULL;
    }

    qapi_free_SocketAddress(s->sockaddr);
    s->sockaddr = NULL;
}

static void usb_inferno_host_init(Object* obj)
{
    USBInfernoHostState* s = USB_INFERNO_HOST(obj);
    s->closed              = true;
}

static const Property usb_inferno_host_props[] = {
    DEFINE_PROP_STRING("addr", USBInfernoHostState, connect_addr),
};

static void usb_inferno_host_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize   = usb_inferno_host_realize;
    dc->unrealize = usb_inferno_host_unrealize;
    dc->desc      = "QEMU USB Passthrough Host Controller";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_props(dc, usb_inferno_host_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_INSTANCE_INIT(USBInfernoHostState, usb_inferno_host, USB_INFERNO_HOST, SYS_BUS_DEVICE)
