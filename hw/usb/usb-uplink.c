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

#include "qemu/osdep.h"
#include "hw/usb.h"
#include "hw/usb/hcd-inferno.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
#include "desc.h"
#include "hcd-virtualhere.h"
#include "hw/usb/usb-uplink.h"

#define USB_CONFIG_DESC_MAX 0x1000

#define USB_UPLINK_POLL_NS       (1 * 1000 * 1000)
#define USB_UPLINK_IDLE_NS       (10 * 1000 * 1000)
#define USB_UPLINK_IDLE_AFTER_NS (1000ULL * 1000 * 1000)
#define USB_UPLINK_SLOW_NS       (10ULL * 1000 * 1000 * 1000)
#define USB_CONFIG_DESC_MIN      9

#define USB_UPLINK_MAX_RETRIES 2000
#define USB_UPLINK_RETRY_NS    (1 * 1000 * 1000)

bool usb_uplink_device_usable(USBDevice* dev)
{
    if (dev == NULL || !dev->attached) { return false; }

    if (dev->state != USB_STATE_DEFAULT) { usb_device_reset(dev); }

    return dev->state == USB_STATE_DEFAULT;
}

bool usb_uplink_packet_wake(USBPacket* p)
{
    USBUplinkPacket* pkt = container_of(p, USBUplinkPacket, p);
    Coroutine*       co  = pkt->waiter;

    if (co == NULL) { return false; }

    pkt->waiter = NULL;
    qemu_coroutine_enter(co);
    return true;
}

int32_t usb_uplink_status_to_errno(int32_t status)
{
    switch (status) {
        case USB_RET_SUCCESS: return 0;
        case USB_RET_NODEV  : return -ENODEV;
        case USB_RET_NAK    : return -EAGAIN;
        case USB_RET_STALL  : return -EPIPE;
        case USB_RET_BABBLE : return -EOVERFLOW;
        default             : return -EIO;
    }
}

int32_t usb_uplink_submit(USBDevice* dev, USBUplinkPacket* pkt, uint8_t ep_addr, void* buf, uint32_t len)
{
    uint8_t      pid = (ep_addr & USB_DIR_IN) ? USB_TOKEN_IN : USB_TOKEN_OUT;
    USBEndpoint* ep;

    if (!usb_uplink_device_usable(dev)) { return USB_RET_NODEV; }

    ep = usb_ep_get(dev, pid, ep_addr & 0x7F);
    if (ep == NULL) { return USB_RET_NODEV; }

    usb_packet_setup(&pkt->p, pid, ep, 0, 0, false, true);
    if (len > 0) { usb_packet_addbuf(&pkt->p, buf, len); }
    usb_handle_packet(dev, &pkt->p);

    return pkt->p.status;
}

void usb_uplink_packet_reset(USBUplinkPacket* pkt)
{
    usb_packet_cleanup(&pkt->p);
    usb_packet_init(&pkt->p);
}

static int32_t coroutine_fn usb_uplink_xfer_stage(USBDevice* dev, uint8_t pid, void* buf, int len, uint32_t* actual_len,
                                                  bool wait_for_device)
{
    int32_t status = USB_RET_NAK;
    int     attempt;

    if (!usb_uplink_device_usable(dev)) { return USB_RET_NODEV; }

    for (attempt = 0; attempt < USB_UPLINK_MAX_RETRIES; attempt++) {
        USBUplinkPacket pkt = {0};

        usb_packet_init(&pkt.p);
        usb_packet_setup(&pkt.p, pid, usb_ep_get(dev, pid, 0), 0, 0, false, false);
        if (len > 0) { usb_packet_addbuf(&pkt.p, buf, len); }
        usb_handle_packet(dev, &pkt.p);

        if (pkt.p.status == USB_RET_ASYNC) {
            pkt.waiter = qemu_coroutine_self();
            qemu_coroutine_yield();
        }

        status = pkt.p.status;
        if (status == USB_RET_SUCCESS && actual_len != NULL) { *actual_len = pkt.p.actual_length; }
        usb_packet_cleanup(&pkt.p);

        if (status != USB_RET_NAK && !(wait_for_device && status == USB_RET_IOERROR)) { return status; }
        qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, USB_UPLINK_RETRY_NS);
    }

    return status;
}

int32_t coroutine_fn usb_uplink_control(USBDevice* dev, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                        uint16_t wIndex, uint16_t wLength, void* buf, uint32_t* actual_len,
                                        bool wait_for_device)
{
    bool    in_dir = (bmRequestType & USB_DIR_IN) != 0;
    uint8_t setup[8];
    int32_t status;

    setup[0] = bmRequestType;
    setup[1] = bRequest;
    stw_le_p(setup + 2, wValue);
    stw_le_p(setup + 4, wIndex);
    stw_le_p(setup + 6, wLength);

    if (actual_len != NULL) { *actual_len = 0; }

    status = usb_uplink_xfer_stage(dev, USB_TOKEN_SETUP, setup, sizeof(setup), NULL, wait_for_device);
    if (status != USB_RET_SUCCESS) { return status; }

    if (wLength > 0) {
        status = usb_uplink_xfer_stage(dev, in_dir ? USB_TOKEN_IN : USB_TOKEN_OUT, buf, wLength, actual_len,
                                       wait_for_device);
        if (status != USB_RET_SUCCESS) { return status; }
    }

    return usb_uplink_xfer_stage(dev, in_dir ? USB_TOKEN_OUT : USB_TOKEN_IN, NULL, 0, NULL, wait_for_device);
}

void usb_uplink_descriptors_free(USBUplinkDescriptors* desc)
{
    if (desc == NULL) { return; }

    g_free(desc->device);
    g_free(desc->configs);
    g_free(desc);
}

static bool usb_uplink_descriptors_finish(USBUplinkDescriptors* desc, GByteArray* configs)
{
    if (desc->num_configs == 0 || configs->len < USB_CONFIG_DESC_MIN) {
        g_byte_array_free(configs, TRUE);
        return false;
    }

    desc->device[USB_DEV_DESC_NUM_CONFIGS] = desc->num_configs;
    desc->configs_len                      = configs->len;
    desc->configs                          = g_byte_array_free(configs, FALSE);

    return true;
}

static USBUplinkDescriptors* usb_uplink_descriptors_from_model(USBDevice* dev)
{
    const USBDesc*       model           = usb_device_get_usb_desc(dev);
    const USBDescDevice* model_dev       = dev->device;
    g_autoptr(USBUplinkDescriptors) desc = NULL;
    g_autofree uint8_t* buf              = NULL;
    GByteArray*         configs;
    int                 flags, len;
    uint8_t             i;

    if (model == NULL || model_dev == NULL) { return NULL; }

    buf   = g_malloc(USB_DESC_MAX_LEN);
    flags = model_dev->bcdUSB >= 0x0300 ? USB_DESC_FLAG_SUPER : 0;

    len = usb_desc_device(&model->id, model_dev, buf, USB_DESC_MAX_LEN);
    if (len < USB_DEV_DESC_SIZE) { return NULL; }

    desc             = g_new0(USBUplinkDescriptors, 1);
    desc->device     = g_memdup2(buf, len);
    desc->device_len = len;
    desc->speed      = dev->speed;

    desc->manufacturer = g_strdup(usb_desc_get_string(dev, desc->device[USB_DEV_DESC_MANUFACTURER]));
    desc->product      = g_strdup(usb_desc_get_string(dev, desc->device[USB_DEV_DESC_PRODUCT]));
    desc->serial       = g_strdup(usb_desc_get_string(dev, desc->device[USB_DEV_DESC_SERIAL]));

    configs = g_byte_array_new();
    for (i = 0; i < model_dev->bNumConfigurations; i++) {
        len = usb_desc_config(&model_dev->confs[i], flags, buf, USB_DESC_MAX_LEN);
        if (len < USB_CONFIG_DESC_MIN) { break; }

        g_byte_array_append(configs, buf, len);
        desc->num_configs++;
    }

    if (!usb_uplink_descriptors_finish(desc, configs)) { return NULL; }

    return g_steal_pointer(&desc);
}

static uint8_t* coroutine_fn usb_uplink_descriptor_from_wire_lang(USBDevice* dev, uint8_t type, uint8_t index,
                                                                  uint16_t langid, uint16_t length, uint32_t* out_len)
{
    g_autofree uint8_t* buf        = g_malloc0(length ?: 1);
    uint32_t            actual_len = 0;
    int32_t             status;

    *out_len = 0;
    if (length == 0) { return NULL; }

    status = usb_uplink_control(dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (type << 8) | index, langid, length, buf,
                                &actual_len, true);
    if (status != USB_RET_SUCCESS || actual_len == 0) { return NULL; }

    *out_len = actual_len;
    return g_memdup2(buf, actual_len);
}

static uint8_t* coroutine_fn usb_uplink_descriptor_from_wire(USBDevice* dev, uint8_t type, uint8_t index,
                                                             uint16_t length, uint32_t* out_len)
{ return usb_uplink_descriptor_from_wire_lang(dev, type, index, 0, length, out_len); }

static char* usb_uplink_string_decode(const uint8_t* desc, uint32_t len)
{
    if (desc == NULL || len < 4 || desc[1] != USB_DT_STRING) { return NULL; }

    return g_utf16_to_utf8((const gunichar2*)(desc + 2), (len - 2) / 2, NULL, NULL, NULL);
}

static char* coroutine_fn usb_uplink_string_from_wire(USBDevice* dev, uint8_t index)
{
    g_autofree uint8_t* langs     = NULL;
    g_autofree uint8_t* str       = NULL;
    uint32_t            langs_len = 0;
    uint32_t            str_len   = 0;

    if (index == 0) { return NULL; }

    langs = usb_uplink_descriptor_from_wire(dev, USB_DT_STRING, 0, 255, &langs_len);
    if (langs_len < 4) { return NULL; }

    str = usb_uplink_descriptor_from_wire_lang(dev, USB_DT_STRING, index, lduw_le_p(langs + 2), 255, &str_len);
    return usb_uplink_string_decode(str, str_len);
}

static uint8_t* coroutine_fn usb_uplink_config_from_wire(USBDevice* dev, uint8_t index, uint32_t* out_len)
{
    g_autofree uint8_t* head     = NULL;
    uint32_t            head_len = 0;
    uint32_t            total_len;

    *out_len = 0;

    head = usb_uplink_descriptor_from_wire(dev, USB_DT_CONFIG, index, USB_CONFIG_DESC_MIN, &head_len);
    if (head_len < USB_CONFIG_DESC_MIN) { return NULL; }

    total_len = lduw_le_p(head + 2);
    if (total_len < USB_CONFIG_DESC_MIN || total_len > USB_CONFIG_DESC_MAX) { return NULL; }

    return usb_uplink_descriptor_from_wire(dev, USB_DT_CONFIG, index, total_len, out_len);
}

static USBUplinkDescriptors* coroutine_fn usb_uplink_descriptors_from_wire(USBDevice* dev)
{
    g_autoptr(USBUplinkDescriptors) desc = NULL;
    GByteArray* configs;
    uint8_t     i;

    desc         = g_new0(USBUplinkDescriptors, 1);
    desc->device = usb_uplink_descriptor_from_wire(dev, USB_DT_DEVICE, 0, USB_DEV_DESC_SIZE, &desc->device_len);
    desc->speed  = dev->speed;
    if (desc->device_len < USB_DEV_DESC_SIZE) { return NULL; }

    desc->manufacturer = usb_uplink_string_from_wire(dev, desc->device[USB_DEV_DESC_MANUFACTURER]);
    desc->product      = usb_uplink_string_from_wire(dev, desc->device[USB_DEV_DESC_PRODUCT]);
    desc->serial       = usb_uplink_string_from_wire(dev, desc->device[USB_DEV_DESC_SERIAL]);

    configs = g_byte_array_new();

    for (i = 0; i < desc->device[USB_DEV_DESC_NUM_CONFIGS]; i++) {
        g_autofree uint8_t* one     = NULL;
        uint32_t            one_len = 0;

        one = usb_uplink_config_from_wire(dev, i, &one_len);
        if (one_len < USB_CONFIG_DESC_MIN) { break; }

        g_byte_array_append(configs, one, one_len);
        desc->num_configs++;
    }

    if (!usb_uplink_descriptors_finish(desc, configs)) { return NULL; }

    return g_steal_pointer(&desc);
}

USBUplinkDescriptors* coroutine_fn usb_uplink_read_descriptors(USBDevice* dev)
{
    USBUplinkDescriptors* desc;

    if (!usb_uplink_device_usable(dev)) { return NULL; }

    desc = usb_uplink_descriptors_from_model(dev);
    if (desc != NULL) { return desc; }

    return usb_uplink_descriptors_from_wire(dev);
}

void usb_uplink_init(USBUplinkDevice* x, DeviceState* owner, USBBusOps* bus_ops, USBPortOps* port_ops, void* opaque)
{
    unsigned int i;

    qemu_co_mutex_init(&x->ctrl_lock);
    usb_bus_new(&x->bus, sizeof(x->bus), bus_ops, owner);

    for (i = 0; i < ARRAY_SIZE(x->ports); i++) {
        unsigned int speeds = USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL | USB_SPEED_MASK_HIGH;

        if (i != 0) { speeds |= USB_SPEED_MASK_SUPER; }

        usb_register_port(&x->bus, &x->ports[i], opaque, i, port_ops, speeds);
    }
}

USBDevice* usb_uplink_active_except(USBUplinkDevice* x, USBPort* except)
{
    USBDevice*   best = NULL;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(x->ports); i++) {
        USBDevice* dev = x->ports[i].dev;

        if (&x->ports[i] == except) { continue; }
        if (dev == NULL || !dev->attached) { continue; }
        if (strcmp(object_get_typename(OBJECT(dev)), "usb-hub") == 0) { continue; }

        best = dev;
    }

    return best;
}

USBDevice* usb_uplink_active(USBUplinkDevice* x) { return usb_uplink_active_except(x, NULL); }

const USBUplinkDescriptors* coroutine_fn usb_uplink_descriptors(USBUplinkDevice* x)
{
    USBDevice* dev = usb_uplink_active(x);

    if (dev == NULL) { return NULL; }

    if (x->desc == NULL) {
        WITH_QEMU_LOCK_GUARD(&x->ctrl_lock) { x->desc = usb_uplink_read_descriptors(dev); }
    }

    return x->desc;
}

void usb_uplink_invalidate(USBUplinkDevice* x) { g_clear_pointer(&x->desc, usb_uplink_descriptors_free); }

void usb_uplink_release(USBUplinkDevice* x)
{
    USBDevice* dev = usb_uplink_active(x);

    usb_uplink_invalidate(x);

    if (dev != NULL && dev->attached) { usb_device_reset(dev); }
}

int32_t coroutine_fn usb_uplink_transfer(USBUplinkDevice* x, USBUplinkPacket* pkt, uint8_t ep_addr, void* buf,
                                         uint32_t len, const bool* abort)
{
    int64_t  started = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    uint64_t waited  = 0;
    bool     warned  = false;

    for (;;) {
        int32_t status;

        if (abort != NULL && *abort) { return USB_RET_NODEV; }

        status = usb_uplink_submit(usb_uplink_active(x), pkt, ep_addr, buf, len);

        if (status == USB_RET_ASYNC) { return status; }
        if (status != USB_RET_NAK && status != USB_RET_IOERROR) { return status; }

        waited = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - started;

        if (!warned && waited > USB_UPLINK_SLOW_NS) {
            warn_report("usb-uplink: endpoint 0x%02x still waiting on the device", ep_addr);
            warned = true;
        }

        usb_uplink_packet_reset(pkt);
        qemu_co_sleep_ns(QEMU_CLOCK_REALTIME,
                         waited > USB_UPLINK_IDLE_AFTER_NS ? USB_UPLINK_IDLE_NS : USB_UPLINK_POLL_NS);
    }
}

DeviceState* usb_uplink_new(USBUplinkType type, const char* addr, Error** errp)
{
    DeviceState* dev;

    switch (type) {
        case USB_UPLINK_TYPE_INFERNO    : dev = qdev_new(TYPE_USB_INFERNO_HOST); break;
        case USB_UPLINK_TYPE_VIRTUALHERE: dev = qdev_new(TYPE_USB_VIRTUALHERE); break;
        default                         : g_assert_not_reached();
    }

    if (addr != NULL) { object_property_set_str(OBJECT(dev), "addr", addr, errp); }
    return dev;
}
