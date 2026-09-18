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

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/dev-inferno-remote.h"
#include "hw/usb/inferno-proto.h"
#include "io/channel-util.h"
#include "io/channel.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/coroutine.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qom/object.h"
#include "trace.h"

#if 0
    #define DPRINTF(fmt, ...)                                       \
        do {                                                        \
            fprintf(stderr, "dev-inferno-remote: " fmt, ##__VA_ARGS__); \
        }                                                           \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

static USBInfernoInflightPacket* usb_inferno_remote_take_inflight_packet(USBInfernoRemoteState* s, int pid, uint8_t ep, uint64_t id)
{
    USBInfernoInflightPacket* p;

    QEMU_LOCK_GUARD(&s->queue_mutex);

    QTAILQ_FOREACH (p, &s->queue, queue) {
        if (p->p->pid == pid && p->p->ep->nr == ep && p->p->id == id) {
            QTAILQ_REMOVE(&s->queue, p, queue);
            return p;
        }
    }

    return NULL;
}

static void usb_inferno_remote_drop_inflight_packet(USBInfernoRemoteState* s, USBPacket* packet)
{
    USBInfernoInflightPacket* p;

    QEMU_LOCK_GUARD(&s->queue_mutex);

    QTAILQ_FOREACH (p, &s->queue, queue) {
        if (p->p == packet) {
            QTAILQ_REMOVE(&s->queue, p, queue);
            g_free(p);
            return;
        }
    }
}

static void usb_inferno_remote_clean_inflight_queue(USBInfernoRemoteState* s)
{
    USBInfernoInflightPacket* p;
    USBDevice*            dev = USB_DEVICE(s);

    QEMU_LOCK_GUARD(&s->queue_mutex);

    while (!QTAILQ_EMPTY(&s->queue)) {
        p = QTAILQ_FIRST(&s->queue);
        QTAILQ_REMOVE(&s->queue, p, queue);

        p->p->status = USB_RET_STALL;
        if (usb_packet_is_inflight(p->p)) { usb_packet_complete(dev, p->p); }
        g_free(p);
    }
}

static void usb_inferno_remote_clean_send_queue(USBInfernoRemoteState* s)
{
    USBInfernoRemoteMsg* m;

    QEMU_LOCK_GUARD(&s->send_mutex);

    while (!QTAILQ_EMPTY(&s->send_queue)) {
        m = QTAILQ_FIRST(&s->send_queue);
        QTAILQ_REMOVE(&s->send_queue, m, queue);
        g_free(m);
    }
}

static void usb_inferno_remote_clean_completed_queue(USBInfernoRemoteState* s)
{
    USBInfernoCompletedPacket* p;
    USBDevice*             dev = USB_DEVICE(s);

    QEMU_LOCK_GUARD(&s->completed_queue_mutex);

    while (!QTAILQ_EMPTY(&s->completed_queue)) {
        p = QTAILQ_FIRST(&s->completed_queue);
        QTAILQ_REMOVE(&s->completed_queue, p, queue);
        if (p->p->status == USB_RET_REMOVE_FROM_QUEUE) { dev->port->ops->complete(dev->port, p->p); }
        else {
            usb_packet_complete(USB_DEVICE(s), p->p);
        }
        g_free(p);
    }
}

static void usb_inferno_remote_cleanup(void* opaque)
{
    USBInfernoRemoteState* s   = opaque;
    QIOChannel*        ioc = s->ioc;

    if (ioc == NULL) { return; }

    s->ioc  = NULL;
    s->addr = 0;

    qio_channel_shutdown(ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
    object_unref(OBJECT(ioc));

    usb_inferno_remote_clean_send_queue(s);
    usb_inferno_remote_clean_completed_queue(s);

    if (USB_DEVICE(s)->attached) { usb_device_detach(USB_DEVICE(s)); }
}

static void usb_inferno_remote_update_addr_bh(void* opaque)
{
    USBInfernoRemoteState* s   = opaque;
    USBDevice*         dev = USB_DEVICE(s);
    dev->addr              = s->addr;
    trace_usb_set_addr(dev->addr);
}

static void usb_inferno_remote_completed_bh(void* opaque)
{
    USBInfernoRemoteState* s   = opaque;
    USBDevice*         dev = USB_DEVICE(s);

    USBInfernoCompletedPacket* p;

    QEMU_LOCK_GUARD(&s->completed_queue_mutex);

    while (!QTAILQ_EMPTY(&s->completed_queue)) {
        p = QTAILQ_FIRST(&s->completed_queue);
        QTAILQ_REMOVE(&s->completed_queue, p, queue);

        qemu_mutex_unlock(&s->completed_queue_mutex);
        if (s->addr != dev->addr && p->p->ep->nr == 0 && p->p->pid == USB_TOKEN_IN && p->p->status == USB_RET_SUCCESS) {
            /*
             * EHCI will append the completed packet to a queue
             * and then schedule a BH
             * BH scheduling is FIFO
             * we want addr to be update after the IN status completed
             */
            qemu_bh_schedule(s->addr_bh);
        }
        if (usb_packet_is_inflight(p->p)) {
            if (p->p->status == USB_RET_REMOVE_FROM_QUEUE) { dev->port->ops->complete(dev->port, p->p); }
            else {
                usb_packet_complete(USB_DEVICE(s), p->p);
            }
        }
        g_free(p);
        qemu_mutex_lock(&s->completed_queue_mutex);
    }
}

static void usb_inferno_remote_closed(USBInfernoRemoteState* s)
{
    if (s->closed) { return; }

    s->closed = true;
    smp_wmb();

    DPRINTF("%s\n", __func__);
    /* Cleanup inflights, otherwise mainloop is stuck */
    usb_inferno_remote_clean_inflight_queue(s);
    qemu_bh_schedule(s->cleanup_bh);
}

static ssize_t coroutine_fn usb_inferno_remote_read(USBInfernoRemoteState* s, QIOChannel* ioc, void* buffer,
                                                unsigned int length)
{
    struct iovec iov = {.iov_base = buffer, .iov_len = length};
    Error*       err = NULL;
    ssize_t      ret;

    ret = qio_channel_readv_full_all_eof(ioc, &iov, 1, NULL, 0, 0, &err);

    if (err) { error_report_err(err); }

    if (ret <= 0) {
        usb_inferno_remote_closed(s);
        return -1;
    }

    return length;
}

static void coroutine_fn usb_inferno_remote_send_co(void* opaque)
{
    USBInfernoRemoteState* s      = opaque;
    g_autoptr(QIOChannel) ioc = NULL;

    if (s->ioc == NULL) { return; }

    ioc        = QIO_CHANNEL(object_ref(s->ioc));
    s->sending = true;

    for (;;) {
        USBInfernoRemoteMsg* m = NULL;
        struct iovec     iov;
        Error*           err = NULL;

        WITH_QEMU_LOCK_GUARD(&s->send_mutex)
        {
            m = QTAILQ_FIRST(&s->send_queue);
            if (m != NULL) { QTAILQ_REMOVE(&s->send_queue, m, queue); }
        }

        if (m == NULL) { break; }

        iov.iov_base = m->data;
        iov.iov_len  = m->len;

        if (qio_channel_writev_full_all(ioc, &iov, 1, NULL, 0, 0, &err) < 0) {
            if (err) { error_report_err(err); }
            g_free(m);
            usb_inferno_remote_closed(s);
            break;
        }

        g_free(m);
    }

    s->sending = false;
}

static void usb_inferno_remote_send_bh(void* opaque)
{
    USBInfernoRemoteState* s = opaque;

    if (s->sending || s->closed) { return; }

    qemu_coroutine_enter(qemu_coroutine_create(usb_inferno_remote_send_co, s));
}

static void usb_inferno_remote_send(USBInfernoRemoteState* s, const struct iovec* iov, int niov)
{
    size_t           len = iov_size(iov, niov);
    USBInfernoRemoteMsg* m   = g_malloc(sizeof(USBInfernoRemoteMsg) + len);

    m->len = len;
    iov_to_buf(iov, niov, 0, m->data, len);

    WITH_QEMU_LOCK_GUARD(&s->send_mutex) { QTAILQ_INSERT_TAIL(&s->send_queue, m, queue); }

    qemu_bh_schedule(s->send_bh);
}

static bool coroutine_fn usb_inferno_remote_read_one(USBInfernoRemoteState* s, QIOChannel* ioc)
{
    inferno_header_t hdr = {0};

    if (usb_inferno_remote_read(s, ioc, &hdr, sizeof(hdr)) != sizeof(hdr)) { return false; }

    switch (hdr.type) {
        case INFERNO_RESPONSE: {
            inferno_response_header rhdr      = {0};
            USBPacket*              p         = NULL;
            USBInfernoInflightPacket*   pkt       = NULL;
            bool                    cancelled = false;

            if (usb_inferno_remote_read(s, ioc, &rhdr, sizeof(rhdr)) != sizeof(rhdr)) { return false; }

            pkt = usb_inferno_remote_take_inflight_packet(s, rhdr.pid, rhdr.ep, rhdr.id);
            if (pkt == NULL) { p = usb_ep_find_packet_by_id(USB_DEVICE(s), rhdr.pid, rhdr.ep, rhdr.id); }
            else {
                p = pkt->p;
            }
            DPRINTF("%s: INFERNO_RESPONSE "
                    "Received packet pid: 0x%x ep: %d id: 0x%" PRIx64 " status: %d\n",
                    __func__, rhdr.pid, rhdr.ep, rhdr.id, rhdr.status);

            if (p == NULL) {
                warn_report("%s: INFERNO_RESPONSE "
                            "Invalid packet pid: 0x%x ep: %d id: 0x%" PRIx64 "\n",
                            __func__, rhdr.pid, rhdr.ep, rhdr.id);
                //__builtin_dump_struct(&rhdr, &printf);
                /* likely canceled */
                /* When an EP is aborted, all of its queued packets are removed */
            }

            if (rhdr.length > 0 && rhdr.status != USB_RET_ASYNC) {
                g_autofree void* buffer = g_malloc(rhdr.length);
                if (rhdr.pid == USB_TOKEN_IN) {
                    if (usb_inferno_remote_read(s, ioc, buffer, rhdr.length) < rhdr.length) { return false; }
                    if (p) { usb_packet_copy(p, buffer, rhdr.length); }
                }
                else if (p) {
                    p->actual_length += rhdr.length;
                }
            }

            if (!p) {
                g_free(pkt);
                return true;
            }

            p->status = rhdr.status;
            if (p->state == USB_PACKET_ASYNC) {
                if (p->status == USB_RET_NAK || p->status == USB_RET_ASYNC) {
                    fprintf(stderr,
                            "%s: INFERNO_RESPONSE "
                            "USB_RET_NAK|ASYNC an ASYNC packet",
                            __func__);
                    usb_inferno_remote_closed(s);
                    g_free(pkt);
                    return false;
                }
            }
            if (p->state == USB_PACKET_QUEUED) {
                if (p->status == USB_RET_NAK) { p->status = USB_RET_IOERROR; }
            }
            if (p->state == USB_PACKET_CANCELED) { cancelled = true; }
            if (((p->status != USB_RET_SUCCESS && p->status != USB_RET_ASYNC && p->status != USB_RET_NAK) || cancelled)
                && p->ep->nr == 0 && p->pid == USB_TOKEN_IN)
            {
                s->addr = USB_DEVICE(s)->addr;
            }
            g_free(pkt);

            if (p->status != USB_RET_ASYNC && !cancelled) {
                USBInfernoCompletedPacket* c = g_malloc0(sizeof(USBInfernoCompletedPacket));
                c->p                     = p;
                c->addr                  = rhdr.addr;

                WITH_QEMU_LOCK_GUARD(&s->completed_queue_mutex) { QTAILQ_INSERT_TAIL(&s->completed_queue, c, queue); }

                qemu_bh_schedule(s->completed_bh);
            }
            return true;
        }

        case INFERNO_REQUEST:
        case INFERNO_RESET  :
        default:
            // "Invalid header type: 0x0" can happen upon closing the connection
            DPRINTF("%s: Invalid header type: 0x%x\n", __func__, hdr.type);
            usb_inferno_remote_closed(s);
            return false;
    }
}

static void coroutine_fn usb_inferno_remote_msg_loop_co(void* opaque)
{
    USBInfernoRemoteState* s      = opaque;
    g_autoptr(QIOChannel) ioc = NULL;

    if (s->ioc == NULL) { return; }

    ioc = QIO_CHANNEL(object_ref(s->ioc));

    while (!s->closed && usb_inferno_remote_read_one(s, ioc)) { continue; }
}

static void usb_inferno_remote_accept(void* opaque)
{
    USBInfernoRemoteState* s   = opaque;
    Error*             err = NULL;
    QIOChannel*        ioc;
    int                fd;

    fd = qemu_accept(s->socket, NULL, NULL);
    if (fd < 0) { return; }

    if (!s->closed || s->stopped) {
        close(fd);
        return;
    }

    ioc = qio_channel_new_fd(fd, &err);
    if (ioc == NULL) {
        error_report_err(err);
        close(fd);
        return;
    }

    qio_channel_set_blocking(ioc, false, NULL);

    s->ioc    = ioc;
    s->addr   = 0;
    s->closed = false;

    DPRINTF("%s: USB device accepted!\n", __func__);

    usb_device_attach(USB_DEVICE(s), &error_abort);

    qemu_coroutine_enter(qemu_coroutine_create(usb_inferno_remote_msg_loop_co, s));
}

static void usb_inferno_remote_realize(USBDevice* dev, Error** errp)
{
    USBInfernoRemoteState* s          = USB_INFERNO_REMOTE(dev);
    g_autoptr(SocketAddress) addr = NULL;

    dev->speed        = USB_SPEED_HIGH;
    dev->speedmask    = USB_SPEED_MASK_HIGH;
    dev->flags       |= (1 << USB_DEV_FLAG_IS_HOST);
    dev->auto_attach  = 0;

    qemu_mutex_init(&s->queue_mutex);
    QTAILQ_INIT(&s->queue);

    qemu_mutex_init(&s->completed_queue_mutex);
    QTAILQ_INIT(&s->completed_queue);

    qemu_mutex_init(&s->send_mutex);
    QTAILQ_INIT(&s->send_queue);

    s->completed_bh = qemu_bh_new(usb_inferno_remote_completed_bh, s);
    s->addr_bh      = qemu_bh_new(usb_inferno_remote_update_addr_bh, s);
    s->cleanup_bh   = qemu_bh_new(usb_inferno_remote_cleanup, s);
    s->send_bh      = qemu_bh_new(usb_inferno_remote_send_bh, s);

    s->socket = -1;
    s->closed = true;

    if (s->listen_addr == NULL) {
        s->listen_addr = g_strdup(USB_INFERNO_REMOTE_ADDR_DEFAULT);
        warn_report("No address specified, using default (`%s`).", USB_INFERNO_REMOTE_ADDR_DEFAULT);
    }

    addr = socket_parse(s->listen_addr, errp);
    if (addr == NULL) { return; }

    s->socket = socket_listen(addr, 1, errp);
    if (s->socket < 0) { return; }

    /* The peer is not necessarily the user QEMU runs as. */
    if (addr->type == SOCKET_ADDRESS_TYPE_UNIX && chmod(addr->u.q_unix.path, 0666) < 0) {
        warn_report("chmod('%s') failed: %s", addr->u.q_unix.path, strerror(errno));
    }

    qemu_set_blocking(s->socket, false, &error_abort);
    qemu_set_fd_handler(s->socket, usb_inferno_remote_accept, NULL, s);
}

static void usb_inferno_remote_unrealize(USBDevice* dev)
{
    USBInfernoRemoteState* s = USB_INFERNO_REMOTE(dev);

    s->stopped = true;

    if (s->socket >= 0) {
        qemu_set_fd_handler(s->socket, NULL, NULL, NULL);
        socket_listen_cleanup(s->socket, NULL);
        s->socket = -1;
    }

    s->closed = true;

    usb_inferno_remote_clean_inflight_queue(s);
    usb_inferno_remote_clean_send_queue(s);
    usb_inferno_remote_clean_completed_queue(s);

    if (s->ioc != NULL) {
        qio_channel_shutdown(s->ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
    }
}

static void usb_inferno_remote_handle_reset(USBDevice* dev)
{
    inferno_header_t   hdr = {0};
    USBInfernoRemoteState* s   = USB_INFERNO_REMOTE(dev);
    struct iovec       iov;

    if (s->closed) { return; }

    DPRINTF("%s\n", __func__);
    usb_inferno_remote_clean_inflight_queue(s);
    usb_inferno_remote_clean_completed_queue(s);
    s->addr  = 0;
    hdr.type = INFERNO_RESET;

    iov.iov_base = &hdr;
    iov.iov_len  = sizeof(hdr);
    usb_inferno_remote_send(s, &iov, 1);
}

static void usb_inferno_remote_cancel_packet(USBDevice* dev, USBPacket* p)
{
    USBInfernoRemoteState*    s   = USB_INFERNO_REMOTE(dev);
    inferno_header_t      hdr = {0};
    inferno_cancel_header pkt = {0};
    struct iovec          iov[2];

    if (p->combined) {
        usb_combined_packet_cancel(dev, p);
        return;
    }

    usb_inferno_remote_drop_inflight_packet(s, p);

    if (s->closed) { return; }

    hdr.type = INFERNO_CANCEL;
    pkt.addr = s->addr;
    pkt.pid  = p->pid;
    pkt.ep   = p->ep->nr;
    pkt.id   = p->id;

    DPRINTF("%s: pid: 0x%x ep %d id 0x%" PRIx64 "\n", __func__, pkt.pid, pkt.ep, pkt.id);

    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = &pkt;
    iov[1].iov_len  = sizeof(pkt);
    usb_inferno_remote_send(s, iov, 2);
}

static void usb_inferno_remote_handle_packet(USBDevice* dev, USBPacket* p)
{
    USBInfernoRemoteState*     s              = USB_INFERNO_REMOTE(dev);
    inferno_header_t       hdr            = {0};
    inferno_request_header pkt            = {0};
    USBInfernoInflightPacket*  inflightPacket = NULL;
    g_autofree void*       buffer         = NULL;
    struct iovec           iov[3];
    int                    niov = 2;

    if (s->closed) {
        p->status = USB_RET_STALL;
        return;
    }

    hdr.type         = INFERNO_REQUEST;
    pkt.addr         = s->addr;
    pkt.pid          = p->pid;
    pkt.ep           = p->ep->nr;
    pkt.id           = p->id;
    pkt.stream       = p->stream;
    pkt.short_not_ok = p->short_not_ok;
    pkt.int_req      = p->int_req;
    pkt.length       = p->iov.size - p->actual_length;

    DPRINTF("%s: pid: 0x%x ep %d id 0x%" PRIx64 " len 0x%x\n", __func__, pkt.pid, pkt.ep, pkt.id, pkt.length);

    if (p->pid != USB_TOKEN_IN && pkt.length) {
        buffer = g_malloc0(pkt.length);
        usb_packet_copy(p, buffer, pkt.length);
        p->actual_length -= pkt.length;
        if (p->pid == USB_TOKEN_SETUP && p->ep->nr == 0 && buffer) {
            struct usb_control_packet* setup = (struct usb_control_packet*)buffer;
            if (setup->bmRequestType == 0 && setup->bRequest == USB_REQ_SET_ADDRESS) { s->addr = setup->wValue; }
        }
    }

    inflightPacket       = g_malloc0(sizeof(USBInfernoInflightPacket));
    inflightPacket->p    = p;
    inflightPacket->addr = dev->addr;

    WITH_QEMU_LOCK_GUARD(&s->queue_mutex) { QTAILQ_INSERT_TAIL(&s->queue, inflightPacket, queue); }

    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    iov[1].iov_base = &pkt;
    iov[1].iov_len  = sizeof(pkt);

    if (buffer) {
        iov[2].iov_base = buffer;
        iov[2].iov_len  = pkt.length;
        niov            = 3;
    }

    usb_inferno_remote_send(s, iov, niov);

    p->status = USB_RET_ASYNC;
}

static const Property usb_inferno_remote_dev_props[] = {
    DEFINE_PROP_STRING("addr", USBInfernoRemoteState, listen_addr),
};

static void usb_inferno_remote_dev_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*    dc = DEVICE_CLASS(klass);
    USBDeviceClass* uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_inferno_remote_realize;
    uc->unrealize      = usb_inferno_remote_unrealize;
    uc->handle_attach  = NULL;
    uc->handle_detach  = NULL;
    uc->cancel_packet  = usb_inferno_remote_cancel_packet;
    uc->handle_reset   = usb_inferno_remote_handle_reset;
    uc->handle_control = NULL;
    uc->handle_data    = NULL;
    uc->handle_packet  = usb_inferno_remote_handle_packet;
    uc->product_desc   = "QEMU USB Passthrough Device";

    dc->desc = "QEMU USB Passthrough Device";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_props(dc, usb_inferno_remote_dev_props);
}

static const TypeInfo usb_inferno_remote_dev_type_info = {
    .name          = TYPE_USB_INFERNO_REMOTE,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBInfernoRemoteState),
    .class_init    = usb_inferno_remote_dev_class_init,
};

static void usb_inferno_register_types(void) { type_register_static(&usb_inferno_remote_dev_type_info); }

type_init(usb_inferno_register_types)
