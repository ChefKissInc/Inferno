/*
 * QEMU model of the USB DWC3 dual-role controller emulation.
 *
 * Copyright (C) 2025-2026 Christian Inci (chris-pcguy).
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/usb/dwc3-regs.h"
#include "hw/usb/hcd-dwc3.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "trace.h"

// #define DEBUG_DWC3

#ifdef DEBUG_DWC3
    #define HEXDUMP(a, b, c)               \
        do {                               \
            qemu_hexdump(stderr, a, b, c); \
        }                                  \
        while (0)
    #define DPRINTF(fmt, ...)                                   \
        do {                                                    \
            qemu_log_mask(LOG_GUEST_ERROR, fmt, ##__VA_ARGS__); \
        }                                                       \
        while (0)
#else
    #define HEXDUMP(a, b, c) \
        do { }               \
        while (0)
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

#ifdef DEBUG_DWC3
static const char* TRBControlType_names[] = {
    [TRBCTL_RESERVED]          = "TRBCTL_RESERVED",
    [TRBCTL_NORMAL]            = "TRBCTL_NORMAL",
    [TRBCTL_CONTROL_SETUP]     = "TRBCTL_CONTROL_SETUP",
    [TRBCTL_CONTROL_STATUS2]   = "TRBCTL_CONTROL_STATUS2",
    [TRBCTL_CONTROL_STATUS3]   = "TRBCTL_CONTROL_STATUS3",
    [TRBCTL_CONTROL_DATA]      = "TRBCTL_CONTROL_DATA",
    [TRBCTL_ISOCHRONOUS_FIRST] = "TRBCTL_ISOCHRONOUS_FIRST",
    [TRBCTL_ISOCHRONOUS]       = "TRBCTL_ISOCHRONOUS",
    [TRBCTL_LINK_TRB]          = "TRBCTL_LINK_TRB",
};
#endif

static void dwc3_device_event(DWC3State* s, struct dwc3_event_devt devt);
static void dwc3_ep_event(DWC3State* s, int epid, struct dwc3_event_depevt depevt);
static void dwc3_ep_trb_event(DWC3State* s, int epid, DWC3TRB* trb, struct dwc3_event_depevt depevt);
static void dwc3_event(DWC3State* s, union dwc3_event event, uint32_t v);
static void dwc3_ep_run(DWC3State* s, DWC3Endpoint* ep);
static void dwc3_ep_run_schedule_update(DWC3State* s, DWC3Endpoint* ep);

static inline dma_addr_t dwc3_addr64(uint32_t low, uint32_t high)
{
    if (sizeof(dma_addr_t) == 4) { return low; }
    else {
        return low | (((dma_addr_t)high << 16) << 16);
    }
}

static int dwc3_packet_find_epid(DWC3State* s, USBPacket* p)
{
    if (p->ep->nr == 0) {
        switch (p->pid) {
            case USB_TOKEN_SETUP:
            case USB_TOKEN_OUT  : return 0;
            case USB_TOKEN_IN   : return 1;
            default             : assert_not_reached(); break;
        }
    }

    for (int i = 0; i < DWC3_NUM_EPS; i++) {
        if (s->eps[i].uep == p->ep) { return i; }
    }
    // the signedness returned here has nothing to do with DWC3Endpoint->epid.
    return -1;
}

static int dwc3_device_intr_level(DWC3State* s, int n)
{
    if (s->gevntsiz(n) & GEVNTSIZ_EVNTINTRPTMASK) { return 0; }
    return qatomic_read(&s->intrs[n].count) > 0;
}

static void dwc3_update_irq(DWC3State* s)
{
    int ip = 0;
    for (uint32_t i = 0; i < s->numintrs; i++) {
        int level = dwc3_device_intr_level(s, i);
        qemu_set_irq(s->sysbus_xhci.irq[i], level || s->host_intr_state[i]);
        ip |= level;
    }
    if (ip) { s->gsts |= GSTS_DEVICE_IP; }
    else {
        s->gsts &= ~GSTS_DEVICE_IP;
    }
}

static bool dwc3_host_intr_raise(XHCIState* xhci, int n, bool level)
{
    XHCISysbusState* xhci_sysbus = container_of(xhci, XHCISysbusState, xhci);
    DWC3State*       s           = container_of(xhci_sysbus, DWC3State, sysbus_xhci);
    bool             host_ip     = false;

    s->host_intr_state[n] = level;
    for (int i = 0; i < DWC3_NUM_INTRS; i++) {
        if (s->host_intr_state[i]) {
            host_ip = true;
            break;
        }
    }
    if (host_ip) { s->gsts |= GSTS_HOST_IP; }
    else {
        s->gsts &= ~GSTS_HOST_IP;
    }
    qemu_set_irq(xhci_sysbus->irq[n], level || dwc3_device_intr_level(s, n));

    return false;
}

#ifdef DEBUG_DWC3
static void dwc3_td_dump(DWC3Transfer* xfer)
{
    DWC3BufferDesc* desc;
    int             k = 0;

    DPRINTF("Dumping td 0x%x (0x" HWADDR_FMT_plx "):\n", xfer->rsc_idx, xfer->tdaddr);
    if (QTAILQ_EMPTY(&xfer->buffers)) {
        DPRINTF("<empty>\n");
        return;
    }

    (void)TRBControlType_names;
    QTAILQ_FOREACH (desc, &xfer->buffers, queue) {
        DPRINTF("Buffer Desc %d:\n", ++k);
        for (int i = 0; i < desc->count; i++) {
            DPRINTF("\tTRB %d @ 0x" HWADDR_FMT_plx ":\n", i, desc->trbs[i].addr);
            DPRINTF("\t\tbp: 0x" HWADDR_FMT_plx "\n", desc->trbs[i].bp);
            DPRINTF("\t\tsize: 0x%x\n", desc->trbs[i].size);
            DPRINTF("\t\tcontrol: 0x%x (%s %s %s %s %s %s %s sid: %d)\n", desc->trbs[i].ctrl,
                    (desc->trbs[i].ctrl & TRB_CTRL_HWO) ? "HWO" : "", (desc->trbs[i].ctrl & TRB_CTRL_LST) ? "LST" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_CHN) ? "CHN" : "", (desc->trbs[i].ctrl & TRB_CTRL_CSP) ? "CSP" : "",
                    TRBControlType_names[TRB_CTRL_TRBCTL(desc->trbs[i].ctrl)],
                    (desc->trbs[i].ctrl & TRB_CTRL_ISP_IMI) ? "ISP_IMI" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_IOC) ? "IOC" : "", TRB_CTRL_SID_SOFN(desc->trbs[i].ctrl));
        }
    }
}
#endif

static int dwc3_bd_length(DWC3State* s, dma_addr_t tdaddr)
{
    struct dwc3_trb trb    = {0};
    int             length = 0;

    while (1) {
        if (dma_memory_read(&s->dma_as, tdaddr, &trb, sizeof(trb), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to read trb\n", __func__);
            return 0;
        }
        if (!(trb.ctrl & TRB_CTRL_HWO)) { return -length; }

        if (TRB_CTRL_TRBCTL(trb.ctrl) == TRBCTL_LINK_TRB) { return length; }
        length++;
        tdaddr += sizeof(trb);
        if (trb.ctrl & TRB_CTRL_LST) { return -length; }
    }
}

// looks/looked like a bad copy of libhw.c, just with in/out from/to being in
// reverse.
int dwc3_bd_map(DWC3State* s, DWC3BufferDesc* desc, USBPacket* p)
{
    DMADirection dir = (p->pid == USB_TOKEN_IN ? DMA_DIRECTION_TO_DEVICE : DMA_DIRECTION_FROM_DEVICE);
    void*        mem;
    int          i;

    if (desc->mapped) {
        // this will/would be printed several times
        // DPRINTF("%s: desc is already mapped: desc->mapped %d\n",
        //         __func__, desc->mapped);
        return 0;
    }
    assert(!desc->ended);
    desc->dir = dir;
    for (i = 0; i < desc->sgl.nsg; i++) {
        dma_addr_t base = desc->sgl.sg[i].base;
        dma_addr_t len  = desc->sgl.sg[i].len;

        while (len) {
            dma_addr_t xlen = len;
            mem             = dma_memory_map(desc->sgl.as, base, &xlen, dir, MEMTXATTRS_UNSPECIFIED);
            if (!mem) {
                // is it correct to set mapped to true even in this error case?
                qemu_log_mask(LOG_GUEST_ERROR, "%s: !mem: base: 0x%" HWADDR_PRIx " len: 0x%" HWADDR_PRIx "\n", __func__,
                              base, len);
                s->gbuserraddrlo = base;
                s->gbuserraddrhi = base >> 32;
                goto err;
            }
            if (xlen > len) { xlen = len; }
            qemu_iovec_add(&desc->iov, mem, xlen);
            len  -= xlen;
            base += xlen;
        }
    }
    desc->mapped        = true;
    desc->actual_length = 0;
    return 0;

err:
    // dwc3_bd_unmap will also get called via dwc3_bd_free in this case.
    dwc3_bd_unmap(s, desc);
    return -1;
}

void dwc3_bd_unmap(DWC3State* s, DWC3BufferDesc* desc)
{
    // usb_packet_unmap wouldn't do a early return here, but let's do it anyway.
    if (!desc->mapped) { return; }
    // don't do "ended" assert, because it can be triggered by DEPENDXFER
    desc->mapped = false;
    for (int i = 0; i < desc->iov.niov; i++) {
        if (desc->iov.iov[i].iov_base) {
            dma_memory_unmap(desc->sgl.as, desc->iov.iov[i].iov_base, desc->iov.iov[i].iov_len, desc->dir,
                             desc->iov.iov[i].iov_len);
            desc->iov.iov[i].iov_base = 0;
        }
    }
}

static bool dwc3_bd_writeback(DWC3State* s, DWC3BufferDesc* desc, USBPacket* p, bool buserr, int packet_left,
                              int xfer_size, void* buffer)
{
    uint32_t                 i            = 0;
    uint32_t                 j            = 0;
    uint32_t                 length       = desc->actual_length;
    uint32_t                 unmap_length = desc->actual_length;
    struct dwc3_event_depevt event        = {.endpoint_number = desc->epid};
    USBPacket*               next_p       = QTAILQ_NEXT(p, queue);
    bool                     setupPending = (next_p && next_p->pid == USB_TOKEN_SETUP);
    bool                     short_packet =
        p->pid != USB_TOKEN_IN && ((usb_packet_size(p) % p->ep->max_packet_size) != 0 || usb_packet_size(p) == 0);
    bool          ret      = false;
    DWC3Endpoint* setup_ep = &s->eps[desc->epid & ~1];

    bool skipTrbs = false;
    while (i < desc->count && event.endpoint_event != DEPEVT_XFERCOMPLETE) {
        DWC3TRB* trb         = &desc->trbs[i++];
        uint32_t controlType = TRB_CTRL_TRBCTL(trb->ctrl);
        // everything but reserved, isochronous* and link_trb
        // isochronous is only supported by one audio interface, "iPod USB
        // Interface"
        assert(controlType == TRBCTL_NORMAL || controlType == TRBCTL_CONTROL_SETUP
               || controlType == TRBCTL_CONTROL_STATUS2 || controlType == TRBCTL_CONTROL_STATUS3
               || controlType == TRBCTL_CONTROL_DATA);
        if (!(trb->ctrl & TRB_CTRL_HWO)) {
            // continue;
            // don't skip the TRB when HWO is unset, but error out and cancel
            // the transfer.
            event.endpoint_event  = DEPEVT_XFERNOTREADY;
            event.status         |= DEPEVT_STATUS_TRANSFER_ACTIVE;
            dwc3_ep_event(s, desc->epid, event);
            p->status = USB_RET_ASYNC;
            // return false;
            ret = false;
            goto end;
        }
        if (length > trb->size) {
            // not sure what to do in this case. this can only have an effect if
            // a huge transfer goes over at least two trb's inside the same
            // descriptor. assert_not_reached();
            DPRINTF("%s: (trb->ctrl & TRB_CTRL_HWO): length > trb->size\n", __func__);
            // trb->bp += trb->size;
            length    -= trb->size;
            trb->size  = 0;    // having this line above looked wrong
        }
        else {
            DPRINTF("%s: (trb->ctrl & TRB_CTRL_HWO): length <= trb->size\n", __func__);
            // trb->bp += length;
            trb->size -= length;
            length     = 0;
        }

        if (setupPending) { trb->trbsts = TRBSTS_SETUP_PENDING; }
        else {
            trb->trbsts = TRBSTS_OK;
        }

        trb->ctrl &= ~TRB_CTRL_HWO;

        // bp and addr have been switched positions now. not sure how compatible
        // this write would be with struct randomization. unsure if "trb->bp +=
        // " is necessary outside of very special circumstances maybe skip over
        // the updates for short packets
#if 0
        assert_cmphex(0x10, ==, sizeof(trb->bp) + sizeof(trb->status) + sizeof(trb->ctrl));
        if (dma_memory_write(desc->sgl.as, trb->addr + 0x0, &trb->bp, 0x10, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: dma_memory_write trb->bp/status/ctrl failed\n", __func__);
        }
#endif
#if 1
        assert_cmphex(0x8, ==, sizeof(trb->status) + sizeof(trb->ctrl));
        if (dma_memory_write(desc->sgl.as, trb->addr + 0x8, &trb->status, 0x8, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: dma_memory_write trb->status/ctrl failed\n", __func__);
        }
#endif
        if (skipTrbs) { continue; }
        if (length <= 0 && buserr) { event.status |= DEPEVT_STATUS_BUSERR; }
        // TRBCTL_CONTROL_* ctrl's should be alone in a descriptor, so return
        // directly. Don't override desc->epid with either 0x1 or 0x0.
        if (controlType == TRBCTL_CONTROL_SETUP) {
            DPRINTF("%s: TRBCTL_CONTROL_SETUP: p->pid: 0x%x desc->epid: 0x%x "
                    "desc->actual_length 0x%x length 0x%x trb->size 0x%x "
                    "last_control_command %s\n",
                    __func__, p->pid, desc->epid, desc->actual_length, length, trb->size,
                    TRBControlType_names[setup_ep->last_control_command]);
            setup_ep->last_control_command = TRBCTL_CONTROL_SETUP;
            if ((desc->epid & 1) != 0 || desc->actual_length != 0x8) {
                // maybe return true in this case, or let process_packet handle
                // this as well. unsure which status to return. this assert got
                // hit, because of further dwc3_process_packet xfer==NULL
                // handling when setting ASYNC. assert_not_reached();
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: TRBCTL_CONTROL_SETUP: bad setup stage: p->pid: 0x%x desc->epid: 0x%x "
                              "desc->actual_length 0x%x length 0x%x trb->size 0x%x\n",
                              __func__, p->pid, desc->epid, desc->actual_length, length, trb->size);
                event.endpoint_event  = DEPEVT_XFERNOTREADY;
                event.status         |= DEPEVT_STATUS_CONTROL_DATA;
                // must not use the dwc3_ep_trb_event variant here ???
                dwc3_ep_event(s, desc->epid, event);
                p->status = USB_RET_ASYNC;
                // return false;
                ret = false;
                goto end;
            }
            memcpy(&setup_ep->setup_packet, buffer, sizeof(setup_ep->setup_packet));
            HEXDUMP(__func__, buffer, sizeof(setup_ep->setup_packet));
            // event.endpoint_event = DEPEVT_XFERCOMPLETE;
            // dwc3_ep_trb_event(s, desc->epid, trb, event);
            // p->status = USB_RET_SUCCESS;
            // return true;
        }
        else if (controlType == TRBCTL_CONTROL_DATA) {
            DPRINTF("%s: TRBCTL_CONTROL_DATA: p->pid: 0x%x desc->epid: 0x%x "
                    "length 0x%x trb->size 0x%x last_control_command %s\n",
                    __func__, p->pid, desc->epid, length, trb->size,
                    TRBControlType_names[setup_ep->last_control_command]);
            DPRINTF("%s: TRBCTL_CONTROL_DATA: setup_ep->setup_packet.wLength "
                    "== 0x%x usb_packet_size(p) == 0x%zu desc->actual_length "
                    "== 0x%x\n",
                    __func__, setup_ep->setup_packet.wLength, usb_packet_size(p), desc->actual_length);
            setup_ep->last_control_command = TRBCTL_CONTROL_DATA;
            if ((p->pid != USB_TOKEN_IN && p->pid != USB_TOKEN_OUT) || setup_ep->setup_packet.wLength == 0x0) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: TRBCTL_CONTROL_DATA: pid 0x%x wLength 0x%x on ep %d\n", __func__,
                              p->pid, setup_ep->setup_packet.wLength, desc->epid);
            }
            // only do a xfercomplete here if returning here
            if (usb_packet_size(p) > setup_ep->setup_packet.wLength
                || usb_packet_size(p) == 0x0 /* || desc->actual_length == 0*/)
            {
                fprintf(stderr,
                        "%s: TRBCTL_CONTROL_DATA: edge_case_0: set "
                        "send_not_ready_control_data=true: "
                        "setup_ep->setup_packet.wLength == 0x%x usb_packet_size(p) "
                        "== 0x%zu desc->actual_length == 0x%x\n",
                        __func__, setup_ep->setup_packet.wLength, usb_packet_size(p), desc->actual_length);
                // don't use DEPEVT_STATUS_TRANSFER_ACTIVE
                setup_ep->send_not_ready_control_data = true;
                // must use USB_RET_ASYNC
                skipTrbs = true;
                // p->status = USB_RET_ASYNC;
                // return false;
            }
            else {
                setup_ep->send_not_ready_control_data = false;
                // event.endpoint_event = DEPEVT_XFERCOMPLETE;
                // dwc3_ep_trb_event(s, desc->epid, trb, event);
                // p->status = USB_RET_SUCCESS;
                // return true;
            }
            // assert_not_reached();
            // no return here, because control data can consist of multi trb's.
        }
        else if (controlType == TRBCTL_CONTROL_STATUS2) {
            DPRINTF("%s: TRBCTL_CONTROL_STATUS2: p->pid: 0x%x desc->epid: 0x%x "
                    "length 0x%x trb->size 0x%x last_control_command %s\n",
                    __func__, p->pid, desc->epid, length, trb->size,
                    TRBControlType_names[setup_ep->last_control_command]);
            setup_ep->last_control_command = TRBCTL_CONTROL_STATUS2;
            // event.endpoint_event = DEPEVT_XFERCOMPLETE;
            // dwc3_ep_trb_event(s, desc->epid, trb, event);
            // p->status = USB_RET_SUCCESS;
            // return true;
        }
        else if (controlType == TRBCTL_CONTROL_STATUS3) {
            DPRINTF("%s: TRBCTL_CONTROL_STATUS3: p->pid: 0x%x desc->epid: 0x%x "
                    "length 0x%x trb->size 0x%x last_control_command %s\n",
                    __func__, p->pid, desc->epid, length, trb->size,
                    TRBControlType_names[setup_ep->last_control_command]);
            setup_ep->last_control_command = TRBCTL_CONTROL_STATUS3;
            // event.endpoint_event = DEPEVT_XFERCOMPLETE;
            // dwc3_ep_trb_event(s, desc->epid, trb, event);
            // p->status = USB_RET_SUCCESS;
            // return true;
        }
        // no "else if" anymore because of TRBCTL_CONTROL_*.
        if (p->pid == USB_TOKEN_IN) {
            // only else if because all TRBCTL_CONTROL_* do return.
            /* IN token */
        trb_complete:
            // assert_cmphex(length, ==, trb->size);
            // length can mismatch trb->size. which one is correct?
            // actually <=, like below?
            // using "length" instead of "trb->size" can cause stalls
            // and "IOUSBDevicePipe::ioGated: unexpected pipe state"
            if (trb->ctrl & TRB_CTRL_LST) {
                event.endpoint_event = DEPEVT_XFERCOMPLETE;
                dwc3_ep_trb_event(s, desc->epid, trb, event);
            }
            else if (trb->ctrl & TRB_CTRL_IOC) {
                // maybe skip this during short IN control_data
                event.endpoint_event = DEPEVT_XFERINPROGRESS;
                dwc3_ep_trb_event(s, desc->epid, trb, event);
            }
            // nothing for else, especially no spurious event call
        }
        else {
            /* OUT or SETUP token */
            if (length <= 0 && short_packet) {
                event.status |= DEPEVT_STATUS_SHORT;
                if (trb->ctrl & TRB_CTRL_CSP) {
                    bool ioc = trb->ctrl & TRB_CTRL_IOC;
                    bool isp = trb->ctrl & TRB_CTRL_ISP_IMI;
                    switch (trb->ctrl & (TRB_CTRL_CHN | TRB_CTRL_LST)) {
                        case TRB_CTRL_CHN | TRB_CTRL_LST:
                        case TRB_CTRL_LST               : goto short_complete;
                        case TRB_CTRL_CHN               : {
                            for (j = 0; j < desc->count; j++) {
                                ioc |= (desc->trbs[j].ctrl & TRB_CTRL_IOC) != 0;
                                isp |= (desc->trbs[j].ctrl & TRB_CTRL_ISP_IMI) != 0;
                            }
                            QEMU_FALLTHROUGH;
                        }
                        case 0:
                            if (!ioc && !isp) { break; }
                            event.endpoint_event = DEPEVT_XFERINPROGRESS;
                            dwc3_ep_trb_event(s, desc->epid, trb, event);
                            break;
                        default: assert_not_reached(); break;
                    }
                }
                else {
                /* no CSP */
                // maybe check for LST bit in current or later TRBs.
                short_complete:
                    event.endpoint_event = DEPEVT_XFERCOMPLETE;
                    dwc3_ep_trb_event(s, desc->epid, trb, event);
                }
            }
            else if (!short_packet) {
                goto trb_complete;
            }
        }
        if (length <= 0) { break; }
    }
    // return p->actual_length == usb_packet_size(p) ||
    //        desc->actual_length % p->ep->max_packet_size != 0;
    // desc->trbs[i - 1].size < p->ep->max_packet_size;
    // with or without !xfer_size?
    // also seems to work to return true unconditionally.
    // !xfer_size or returning true altogether might be required for
    // idevicesyslog to stop hanging. however, cdc ncm is making some troubles.
    // this surely needs better conditions
    ret = p->actual_length == usb_packet_size(p) || desc->actual_length % p->ep->max_packet_size != 0 || !xfer_size
          || length <= 0;
    if (!ret) {
        // struct dwc3_event_depevt event = { .endpoint_number = desc->epid,
        //                                    .endpoint_event =
        //                                        DEPEVT_XFERNOTREADY };
        // event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
        // dwc3_ep_event(s, desc->epid, event);
        DPRINTF("%s: ret is FALSE\n", __func__);
        p->status = USB_RET_ASYNC;
        ret       = false;
    }
    else {
        DPRINTF("%s: ret is TRUE\n", __func__);
        p->status = USB_RET_SUCCESS;
        ret       = true;
    }
    uint32_t firstControlType = TRB_CTRL_TRBCTL(desc->trbs[0].ctrl);
    if (firstControlType == TRBCTL_CONTROL_DATA) {
        if (setup_ep->send_not_ready_control_data) {
            p->status = USB_RET_ASYNC;
            // return false;
            ret = false;
            goto end;
        }
        // sure to not let "ret" decide?
        p->status = USB_RET_SUCCESS;
        ret       = true;
    }
    else if (firstControlType == TRBCTL_CONTROL_SETUP) {
        p->status = USB_RET_SUCCESS;
        ret       = true;
    }
    else if (firstControlType == TRBCTL_CONTROL_STATUS2) {
        p->status = USB_RET_SUCCESS;
        ret       = true;
    }
    else if (firstControlType == TRBCTL_CONTROL_STATUS3) {
        p->status = USB_RET_SUCCESS;
        ret       = true;
    }
// // // if (event.endpoint_event == DEPEVT_XFERINPROGRESS) {
// // //     p->status = USB_RET_ASYNC;
// // // }
// int packet_left = usb_packet_size(p) - p->actual_length;
// if (desc->length - desc->actual_length > 0 && packet_left > 0 &&
//     packet_left % p->ep->max_packet_size == 0) {
//     DPRINTF("%s: xfer_size 0x%x if_0: USB_RET_SUCCESS\n",
//             __func__, xfer_size);
//     p->status = USB_RET_SUCCESS;
//     // return true;
//     ret = true;
//     goto end;
// }
// if (p->actual_length < usb_packet_size(p) && p->actual_length % p->ep->max_packet_size == 0 && p->actual_length > 0)
// {
//     DPRINTF("%s: ret is FALSE\n", __func__);
//     p->status = USB_RET_ASYNC;
//     ret = false;
// } else {
//     DPRINTF("%s: ret is TRUE\n", __func__);
//     p->status = USB_RET_SUCCESS;
//     ret = true;
// }
// if (xfer_size < packet_left && xfer_size % p->ep->max_packet_size == 0 && xfer_size > 0) {
//     DPRINTF("%s: ret is FALSE\n", __func__);
//     p->status = USB_RET_ASYNC;
//     ret = false;
// } else {
//     DPRINTF("%s: ret is TRUE\n", __func__);
//     p->status = USB_RET_SUCCESS;
//     ret = true;
// }
end:

    j = 0;
    while (j < desc->iov.niov && unmap_length > 0) {
        size_t access_len = desc->iov.iov[j].iov_len;
        if (access_len > unmap_length) { access_len = unmap_length; }

        if (desc->iov.iov[j].iov_base) {
            dma_memory_unmap(desc->sgl.as, desc->iov.iov[j].iov_base, desc->iov.iov[j].iov_len, desc->dir, access_len);
            desc->iov.iov[j].iov_base = 0;
        }
        unmap_length -= access_len;
        j++;
    }

    return ret;
}

static int dwc3_bd_copy(DWC3State* s, DWC3BufferDesc* desc, USBPacket* p)
{
    g_autofree void* buffer      = NULL;
    uint32_t         packet_left = usb_packet_size(p) - p->actual_length;
    uint32_t         desc_left   = desc->length - desc->actual_length;
    uint32_t         actual_xfer = 0;
    uint32_t         xfer_size;
    DPRINTF("%s: entered function\n", __func__);

    xfer_size = MIN(packet_left, desc_left);
    // Don't override xfer_size for USB_TOKEN_SETUP here!

    // packet_left (dwc3) == pktsize (dwc2)
    // desc_left (dwc3) == amtDone before (dwc2)
    // xfer_size (dwc3) == amtDone after (dwc2)

    // maybe do map/unmap even when xfer_size is zero. is this only about speed,
    // or also about correctness?
    if (dwc3_bd_map(s, desc, p)) {
        // do dwc3_bd_free at the caller instead
        return -1;
    }

    // depend on a non-null pointer, even if xfer_size is zero.
    buffer = g_malloc0(MAX(xfer_size, 1));
    assert_nonnull(buffer);
    if (p->pid == USB_TOKEN_IN) {
#if 1
        DPRINTF("%s IN Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__, xfer_size, desc->epid,
                desc->trbs[0].bp);
        DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length, usb_packet_size(p));
#endif
        if (xfer_size) {
            actual_xfer = qemu_iovec_to_buf(&desc->iov, desc->actual_length, buffer, xfer_size);
            usb_packet_copy(p, buffer, xfer_size);
#if 0
            HEXDUMP(__func__, buffer, xfer_size);
#endif
        }
    }
    else {
#if 1
        if (p->pid == USB_TOKEN_OUT) {
            DPRINTF("%s OUT Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__, xfer_size, desc->epid,
                    desc->trbs[0].bp);
        }
        else {
            DPRINTF("%s Setup Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__, xfer_size, desc->epid,
                    desc->trbs[0].bp);
        }
        DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length, usb_packet_size(p));
#endif
        if (xfer_size) {
            usb_packet_copy(p, buffer, xfer_size);
            actual_xfer = qemu_iovec_from_buf(&desc->iov, desc->actual_length, buffer, xfer_size);
#if 0
            HEXDUMP(__func__, buffer, xfer_size);
#endif
        }
    }

    assert_cmphex(actual_xfer, >=, 0);
    desc->actual_length += actual_xfer;
    // commenting this out leads e.g. to "Unable to send NORData"
    // for out-direction mass-transfer
    // also known as if_0
    // if both if_0/if_1 are there, if_0 is out, if_1 is in.
    // can't use desc_left here, must recognize the new desc->actual_length
    // unsure if that should only be done for OUT, or for both.
    // "v0 - v1 > 0" equals "v0 > v1" according to a quick python testcase
    if (desc->length > desc->actual_length && packet_left > 0 && packet_left % p->ep->max_packet_size == 0) {
        DPRINTF("%s: xfer_size 0x%x packet_left 0x%x if_0: USB_RET_SUCCESS\n", __func__, xfer_size, packet_left);
        DPRINTF("%s: desc->length 0x%x desc->actual_length 0x%x "
                "p->ep->max_packet_size 0x%x\n",
                __func__, desc->length, desc->actual_length, p->ep->max_packet_size);
        p->status = USB_RET_SUCCESS;
        return xfer_size;
    }

    desc->ended = true;
    bool buserr = false;
    if (xfer_size) {
        // even with xfer_size being zero, this can only be true if actual_xfer
        // would become negative for whatever reason
        buserr = actual_xfer < xfer_size;
    }
    dwc3_bd_writeback(s, desc, p, buserr, packet_left, xfer_size, buffer);
    // Don't do dwc3_bd_unmap for the if_0 case.
    dwc3_bd_unmap(s, desc);
    return xfer_size;
}

void dwc3_bd_free(DWC3State* s, DWC3BufferDesc* desc)
{
    dwc3_bd_unmap(s, desc);
    g_free(desc->trbs);
    qemu_iovec_destroy(&desc->iov);
    qemu_sglist_destroy(&desc->sgl);
    desc->trbs = NULL;
    g_free(desc);
}

static void dwc3_td_free_buffers(DWC3State* s, DWC3Transfer* xfer)
{
    DWC3BufferDesc* desc;
    DWC3BufferDesc* desc_next;
    QTAILQ_FOREACH_SAFE (desc, &xfer->buffers, queue, desc_next) {
        QTAILQ_REMOVE(&xfer->buffers, desc, queue);
        xfer->count--;
        dwc3_bd_free(s, desc);
    }
}

static void dwc3_td_free(DWC3State* s, DWC3Transfer* xfer)
{
    // assert(xfer->can_free);
    dwc3_td_free_buffers(s, xfer);
    g_free(xfer);
}

static void dwc3_td_fetch(DWC3State* s, DWC3Transfer* xfer, dma_addr_t tdaddr)
{
    struct dwc3_trb trb = {0};
    int             count;
    bool            ended = false;

    do {
        DWC3BufferDesc* desc;

        count = dwc3_bd_length(s, tdaddr);
        if (count < 0) {
            ended = true;
            count = -count;
        }
        if (count == 0) {
            ended = true;
            break;
        }

        desc         = g_new0(DWC3BufferDesc, 1);
        desc->epid   = xfer->epid;
        desc->count  = 0;
        desc->trbs   = g_new0(DWC3TRB, count);
        desc->length = 0;
        qemu_iovec_init(&desc->iov, 1);
        qemu_sglist_init(&desc->sgl, &s->parent_obj.parent_obj, 1, &s->dma_as);
        // having the insert before the loop doesn't seem to trigger the
        // multi-buffer-assert, but it'll cause entering the multi-buffer-loop
        // even for single-buffer tranfers.

        do {
            if (dma_memory_read(desc->sgl.as, tdaddr, &trb, sizeof(trb), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to read trb\n", __func__);
                ended = true;
                break;
            }
            uint32_t controlType = TRB_CTRL_TRBCTL(trb.ctrl);
            // everything but reserved and isochronous*
            if (!(controlType == TRBCTL_NORMAL || controlType == TRBCTL_CONTROL_SETUP
                  || controlType == TRBCTL_CONTROL_STATUS2 || controlType == TRBCTL_CONTROL_STATUS3
                  || controlType == TRBCTL_CONTROL_DATA || controlType == TRBCTL_LINK_TRB))
            {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: unsupported TRBCTL %d at 0x%" HWADDR_PRIx "\n", __func__,
                              controlType, tdaddr);
                ended = true;
                break;
            }
            DPRINTF("%s: tdaddr 0x%" HWADDR_PRIx " controlType: %d trb.ctrl: 0x%x\n", __func__, tdaddr, controlType,
                    trb.ctrl);

            if (!(trb.ctrl & TRB_CTRL_HWO)) {
                ended = true;
                DPRINTF("%s: ended;break: (!(trb.ctrl & TRB_CTRL_HWO))\n", __func__);
                break;
            }

            if (controlType == TRBCTL_LINK_TRB) {
                DWC3BufferDesc* d;

                tdaddr = dwc3_addr64(trb.bpl, trb.bph);
                DPRINTF("%s: TRBCTL_LINK_TRB: link_tdaddr 0x%" HWADDR_PRIx "\n", __func__, tdaddr);

                if (desc->trbs[0].addr <= tdaddr && tdaddr <= desc->trbs[0].addr + sizeof(trb) * (count + 1)) {
                    /* self loop */
                    ended = true;
                    DPRINTF("%s: ended;break: self-loop\n", __func__);
                    break;
                }

                /* Multi Buffer Loops */
                QTAILQ_FOREACH (d, &xfer->buffers, queue) {
                    assert(d->count > 0 && d->trbs);
                    if (d->trbs[0].addr <= tdaddr && d->trbs[d->count - 1].addr <= tdaddr) {
                        ended = true;
                        DPRINTF("%s: ended;break: multi-buffer-loops\n", __func__);
                        break;
                    }
                }
                break;
            }
            if (desc->count >= count) {
                /* We don't include the link TRB in the desc count */
                ended = true;
                DPRINTF("%s: ended;break: We don't include the link TRB in the "
                        "desc count\n",
                        __func__);
                break;
            }
            desc->trbs[desc->count].bp     = dwc3_addr64(trb.bpl, trb.bph);
            desc->trbs[desc->count].addr   = tdaddr;
            desc->trbs[desc->count].status = trb.status;
            desc->trbs[desc->count].ctrl   = trb.ctrl;
            qemu_sglist_add(&desc->sgl, desc->trbs[desc->count].bp, desc->trbs[desc->count].size);
            desc->length += desc->trbs[desc->count].size;
            desc->count++;

            tdaddr += sizeof(trb);

            if (trb.ctrl & TRB_CTRL_LST) {
                xfer->can_free = true;
                ended          = true;
                DPRINTF("%s: ended;break: (trb.ctrl & TRB_CTRL_LST)\n", __func__);
                break;
            }
        }
        while (!ended);
        if (desc->count == 0) {
            dwc3_bd_free(s, desc);
            ended = true;
            break;
        }
        QTAILQ_INSERT_TAIL(&xfer->buffers, desc, queue);
        xfer->count++;
    }
    while (!ended && xfer->count < 256);
    xfer->tdaddr = tdaddr;
#ifdef DEBUG_DWC3
    #if 1
    dwc3_td_dump(xfer);
    #endif
#endif
}

static DWC3Transfer* dwc3_xfer_alloc(DWC3State* s, int epid, dma_addr_t tdaddr)
{
    DWC3Endpoint* ep   = &s->eps[epid];
    DWC3Transfer* xfer = g_new0(DWC3Transfer, 1);

    xfer->epid   = epid;
    xfer->tdaddr = tdaddr;
    QTAILQ_INIT(&xfer->buffers);
    xfer->count = 0;
    // // // xfer->rsc_idx = tdaddr & 0x7f; // please no, trung. I even thought
    // about doing a crc8, but it seems to require having 0x0 as a starting
    // value. (global_)rsc_idx_counter will not work if it's inside DWC3State
    // (shared between all endpoints) "rsc_idx"-handling is still incomplete,
    // but better than having "tdaddr & 0x7f" randomly result in e.g. 0x10
    // instead of 0x0. doing that is surely not correct, since the value will
    // always either be 0x0 or 0x2. dunno why it keeps working, though.
    // ep->rsc_idx_counter = s->global_rsc_idx_counter;
    xfer->rsc_idx        = ep->rsc_idx_counter++;
    ep->rsc_idx_counter &= 0x7f;
    if (ep->rsc_idx_counter == 0) {
        DPRINTF("%s: wraparound reached. xfer->rsc_idx %d\n", __func__, xfer->rsc_idx);
        // fprintf(stderr, "%s: wraparound reached. xfer->rsc_idx %d\n",
        // __func__, xfer->rsc_idx); no idea what to do in case of a wraparound,
        // but let's try to make it start at the global value.
        ep->rsc_idx_counter = s->global_rsc_idx_counter;
    }
    xfer->can_free = false;

    dwc3_td_fetch(s, xfer, tdaddr);
    return xfer;
}

static void dwc3_write_event(DWC3State* s, union dwc3_event event, uint32_t v)
{
    DWC3EventRing* intr = &s->intrs[v];
    dma_addr_t     ring_base;
    dma_addr_t     ev_addr;
    uint32_t       head;

    ring_base = dwc3_addr64(s->gevntadr_lo(v), s->gevntadr_hi(v));

    head    = qatomic_read(&intr->head);
    ev_addr = ring_base + head;
    qatomic_set(&intr->head, (head + EVENT_SIZE) % intr->size);

    dma_memory_write(&s->dma_as, ev_addr, &event.raw, EVENT_SIZE, MEMTXATTRS_UNSPECIFIED);
    smp_wmb();
    qatomic_add(&intr->count, EVENT_SIZE);
    smp_wmb();
}

static void dwc3_event(DWC3State* s, union dwc3_event event, uint32_t v)
{
    DWC3EventRing* intr;
    uint32_t       count;

    if (v >= s->numintrs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ring nr out of range (%u >= %u)\n", __func__, v, s->numintrs);
        return;
    }
    intr = &s->intrs[v];

    if (intr->size < 2 * EVENT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ring nr %u is not configured (size 0x%x). Dropping event.\n", __func__, v,
                      intr->size);
        return;
    }

    count = qatomic_read(&intr->count);

    if (count + EVENT_SIZE <= intr->size - EVENT_SIZE) { dwc3_write_event(s, event, v); }
    else {
        union dwc3_event overflow = {.devt = {1, 0, DEVICE_EVENT_OVERFLOW}};

        if (count + EVENT_SIZE <= intr->size && event.raw != overflow.raw) {
            dwc3_write_event(s, overflow, v);
            qemu_log_mask(LOG_GUEST_ERROR, "%s: ring nr %u is full. Sending event overflow.\n", __func__, v);
        }
        else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: ring nr %u is full. Dropping event.\n", __func__, v);
        }
    }
    dwc3_update_irq(s);
}

static void dwc3_device_event_ungated(DWC3State* s, struct dwc3_event_devt devt)
{
    union dwc3_event event = {.devt = devt};

    dwc3_event(s, event, DCFG_INTRNUM_GET(s->dcfg));
}

static void dwc3_device_event(DWC3State* s, struct dwc3_event_devt devt)
{
    if (!(s->devten & (1 << (devt.type)))) { return; }
    dwc3_device_event_ungated(s, devt);
}

static void dwc3_ep_event(DWC3State* s, int epid, struct dwc3_event_depevt depevt)
{
    union dwc3_event event = {.depevt = depevt};
    DWC3Endpoint*    ep    = &s->eps[epid];
    int              v     = ep->intrnum;
    DPRINTF("%s: epid: %d ev: %d raw: 0x%x\n", __func__, epid, depevt.endpoint_event, event.raw);

    if (depevt.endpoint_event == DEPEVT_XFERNOTREADY) {
        if (ep->not_ready) { return; }
        ep->not_ready = true;
    }
    // handling DEPEVT_EPCMDCMPLT here, because it shouldn't be working with
    // the underneath mask, it'll otherwise collide with DEPCFG_PAR1_BIT14
    if (depevt.endpoint_event == DEPEVT_EPCMDCMPLT) {
        dwc3_event(s, event, v);
        return;
    }
    // this odd half-matching bitmask only works for these events
    assert(depevt.endpoint_event == DEPEVT_XFERCOMPLETE || depevt.endpoint_event == DEPEVT_XFERINPROGRESS
           || depevt.endpoint_event == DEPEVT_XFERNOTREADY || depevt.endpoint_event == DEPEVT_STREAMEVT);
    if (ep->event_en & (1 << (depevt.endpoint_event))) { dwc3_event(s, event, v); }
}

static void dwc3_ep_trb_event(DWC3State* s, int epid, DWC3TRB* trb, struct dwc3_event_depevt depevt)
{
    if (depevt.endpoint_event == DEPEVT_XFERCOMPLETE || depevt.endpoint_event == DEPEVT_XFERINPROGRESS) {
        if (trb->ctrl & TRB_CTRL_LST) { depevt.status |= DEPEVT_STATUS_LST; }
        // no else if here because those flags can be set individually
        if (trb->ctrl & TRB_CTRL_IOC) { depevt.status |= DEPEVT_STATUS_IOC; }
    }
    dwc3_ep_event(s, epid, depevt);
}

static void dwc3_dcore_reset(DWC3State* s, bool soft)
{
    USBDevice* udev = &s->device.parent_obj;

    /* Clear Interrupts */
    for (int i = 0; i < s->numintrs; i++) {
        s->intrs[i].size  = 0;
        s->intrs[i].head  = 0;
        s->intrs[i].count = 0;
    }

    /* Clearing MMR */
    s->gsbuscfg0 = (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
    s->gsbuscfg1 = (0xf << 8);
    s->gtxthrcfg = 0;
    s->grxthrcfg = 0;
    if (!soft) {
        s->gctl   = GCTL_PWRDNSCALE(0x4b0) | GCTL_PRTCAPDIR(GCTL_PRTCAP_DEVICE) | GCTL_U2RSTECN | GCTL_U2EXIT_LFPS;
        s->guctl  = (1 << 15) | (0x10 << 0);
        s->gsts  &= ~GSTS_BUS_ERR_ADDR_VLD;
    }
    // usb_dwc3_glbreg_write: default: addr: 0xc11c val: 0x80400000
    s->guctl1          = 0;
    s->gevten          = 0;
    s->gbuserraddrlo   = 0;
    s->gbuserraddrhi   = 0;
    s->gprtbimaplo     = 0;
    s->gprtbimaphi     = 0;
    s->gprtbimap_hs_lo = 0;
    s->gprtbimap_hs_hi = 0;
    s->gprtbimap_fs_lo = 0;
    s->gprtbimap_fs_hi = 0;
    s->ghwparams0      = 0x40204048 | (GHWPARAMS0_MODE_DRD);
    s->ghwparams1      = 0x222493b;
    s->ghwparams2      = 0x12345678;
    s->ghwparams3      = (0x20 << 23) | GHWPARAMS3_NUM_IN_EPS(DWC3_NUM_EPS >> 1) | GHWPARAMS3_NUM_EPS(DWC3_NUM_EPS)
                         | (0x2 << 6) | (0x3 << 2) | (0x1 << 0);
    s->ghwparams4      = 0x47822004;
    s->ghwparams5      = 0x4202088;
    s->ghwparams6      = 0x7850c20;
    s->ghwparams7      = 0x0;
    s->ghwparams8      = 0x478;
    memset(s->gtxfifosiz, 0, sizeof(s->gtxfifosiz));
    memset(s->grxfifosiz, 0, sizeof(s->grxfifosiz));
    memset(s->gevntregs, 0, sizeof(s->gevntregs));
    s->dgcmdpar = 0;
    s->dgcmd    = 0;
    s->dalepena = 0;
    memset(s->depcmdreg, 0, sizeof(s->depcmdreg));
    s->global_rsc_idx_counter = 0;

    /* Terminate all USB transaction */
    for (int i = 0; i < DWC3_NUM_EPS; i++) {
        DWC3Endpoint* ep = &s->eps[i];
        USBPacket*    p  = NULL;

        if (ep->xfer) {
            dwc3_td_free(s, ep->xfer);
            ep->xfer = NULL;
        }

        if (ep->uep) {
            QTAILQ_FOREACH (p, &ep->uep->queue, queue) {
                p->status = USB_RET_IOERROR;
                usb_packet_complete(udev, p);
            }
        }
        memset(ep, 0, sizeof(*ep));
        ep->epid = i;
    }
    usb_ep_reset(udev);
}

static void dwc3_usb_device_connected_speed(DWC3State* s)
{
    USBDevice* udev = &s->device.parent_obj;
    switch (udev->speed) {
        case USB_SPEED_LOW  : s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_LOWSPEED; break;
        case USB_SPEED_FULL : s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_FULLSPEED2; break;
        case USB_SPEED_HIGH : s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_HIGHSPEED; break;
        case USB_SPEED_SUPER: s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_SUPERSPEED; break;
        default             : assert_not_reached();
    }
}

static void dwc3_reset_enter(Object* obj, ResetType type)
{
    DWC3State* s = DWC3_USB(obj);

    dwc3_dcore_reset(s, false);
    s->gsts    = GSTS_CURMOD_DRD;
    s->gsnpsid = GSNPSID_REVISION_180A;
    s->ggpio   = 0;
    s->guid    = 0;
    // maybe set it to 0 instead of SUSPHY?
    // s->gusb2phycfg = GUSB2PHYCFG_SUSPHY;
    s->gusb2phycfg  = 0;
    s->gusb2phyacc  = 0;
    s->gusb3pipectl = GUSB3PIPECTL_REQP1P2P3 | GUSB3PIPECTL_DEP1P2P3_EN | GUSB3PIPECTL_DEPOCHANGE;
    // restore mode and regular boot can cope with
    // DCFG_HIGHSPEED/DCFG_SUPERSPEED. anything seen otherwise (e.g. when
    // restore mode can, but regular boot can't) could be a red herring because
    // of rsc_idx (especially when it's globally defined).
    // s->dcfg = DCFG_IGNSTRMPP | (2 << 10) | DCFG_SUPERSPEED;
    s->dcfg = DCFG_IGNSTRMPP | (2 << 10) | DCFG_HIGHSPEED;
    s->dsts = DSTS_COREIDLE | DSTS_USBLNKST(LINK_STATE_SS_DIS) | DSTS_RXFIFOEMPTY;
    dwc3_usb_device_connected_speed(s);
}

static void dwc3_reset_hold(Object* obj, ResetType type)
{
    DWC3State* s = DWC3_USB(obj);

    dwc3_update_irq(s);
}

static void dwc3_reset_exit(Object* obj, ResetType type)
{
    DWC3State* s = DWC3_USB(obj);

    s->device.parent_obj.addr = 0;
}

static uint64_t usb_dwc3_gevntreg_read(void* opaque, hwaddr addr, int index)
{
    DWC3State*     s = opaque;
    uint32_t       val;
    uint32_t*      mmio;
    uint32_t       v    = index >> 2;
    DWC3EventRing* intr = &s->intrs[v];

    if (addr >= GHWPARAMS8) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }

    mmio = &s->gevntregs[index];
    val  = *mmio;

    switch (GEVNTADRLO(0) + (addr & 0xc)) {
        case GEVNTCOUNT(0): val = qatomic_read(&intr->count) & 0xffff; break;
        default           : break;
    }

    return val;
}

static void usb_dwc3_gevntreg_write(void* opaque, hwaddr addr, int index, uint64_t val)
{
    DWC3State*     s = opaque;
    uint32_t*      mmio;
    uint32_t       old;
    uint32_t       v    = index >> 2;
    DWC3EventRing* intr = &s->intrs[v];
    int            iflg = 0;

    if (addr >= GHWPARAMS8) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return;
    }

    mmio = &s->gevntregs[index];
    old  = *mmio;

    switch (GEVNTADRLO(0) + (addr & 0xc)) {
        case GEVNTSIZ(0):
            val &= (GEVNTCOUNT_EVENTSIZ_MASK | GEVNTSIZ_EVNTINTRPTMASK);
            if ((old & GEVNTCOUNT_EVENTSIZ_MASK) != 0) {
                val &= ~GEVNTCOUNT_EVENTSIZ_MASK;
                val |= (old & GEVNTCOUNT_EVENTSIZ_MASK);
            }
            else {
                intr->size = val & GEVNTCOUNT_EVENTSIZ_MASK;
            }
            iflg = true;
            break;
        case GEVNTCOUNT(0): {
            uint32_t dec = (val & 0xffff);
            if (dec > intr->count) { qatomic_set(&intr->count, 0); }
            else {
                qatomic_sub(&intr->count, dec);
            }
            iflg = true;
            break;
        }
        default: break;
    }

    *mmio = val;

    if (iflg) { dwc3_update_irq(s); }
}

static uint64_t usb_dwc3_glbreg_read(void* opaque, hwaddr addr, int index)
{
    DWC3State* s = opaque;
    uint32_t   val;

    if (addr > GHWPARAMS8) {
#if 0
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
#endif
        return 0;
    }

    val = s->glbreg[index];

    switch (addr) {
        // keep the extra parentheses!!!!
        case GEVNTADRLO(0)... GEVNTCOUNT((DWC3_NUM_INTRS - 1)):
            val = usb_dwc3_gevntreg_read(s, addr, (addr - GEVNTADRLO(0)) >> 2);
            break;
        default:
#if 0
        qemu_log_mask(LOG_UNIMP, "%s: default: addr: 0x%" HWADDR_PRIx "\n", __func__,
                      addr);
#endif
            break;
    }
    return val;
}

static void usb_dwc3_glbreg_write(void* opaque, hwaddr addr, int index, uint64_t val)
{
    DWC3State* s = opaque;
    uint32_t*  mmio;
    uint32_t   old;
    int        iflg = 0;

    if (addr > GHWPARAMS8) {
#if 0
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
#endif
        return;
    }

    mmio = &s->glbreg[index];
    old  = *mmio;

    switch (addr) {
        case GCTL:
            if (!(old & GCTL_CORESOFTRESET) && (val & GCTL_CORESOFTRESET)) {
                device_cold_reset(&s->parent_obj.parent_obj);
            }
            break;
        case GSTS:
            val &= (GSTS_CSR_TIMEOUT | GSTS_BUS_ERR_ADDR_VLD);
            /* clearing Write to Clear bits */
            val = old & ~val;
            break;
        case GSNPSID      :
        case GGPIO        :
        case GBUSERRADDR0 :
        case GBUSERRADDR1 :
        case GPRTBIMAP1   :
        case GHWPARAMS0   :
        case GHWPARAMS1   :
        case GHWPARAMS2   :
        case GHWPARAMS3   :
        case GHWPARAMS4   :
        case GHWPARAMS5   :
        case GHWPARAMS6   :
        case GHWPARAMS7   :
        case GHWPARAMS8   :
        case GPRTBIMAP_HS1:
        case GPRTBIMAP_FS1: val = old; break;
        case GPRTBIMAP0   :
        case GPRTBIMAP_HS0:
        case GPRTBIMAP_FS0: val &= (0xf << 0); break;
        case GUSB2PHYCFG(0):
            val &= ~((1 << 7) | (1 << 5) | GUSB2PHYCFG_PHYIF(1));
            if (!(old & GUSB2PHYCFG_PHYSOFTRST) && (val & GUSB2PHYCFG_PHYSOFTRST)) {
                /* TODO: Implement Phy Soft Reset */
                qemu_log_mask(LOG_UNIMP, "%s: Phy Soft Reset not implemented\n", __func__);
                break;
            }
            if ((old & GUSB2PHYCFG_SUSPHY) != (val & GUSB2PHYCFG_SUSPHY)) {
/* TODO: Implement Phy Suspend */
#if 0
            qemu_log_mask(LOG_UNIMP, "%s: Phy (un)Suspend not implemented\n",
                          __func__);
#endif
                break;
            }
            break;
        case GUSB2PHYACC(0): val &= ~((1 << 26) | GUSB2PHYACC_DONE | GUSB2PHYACC_BUSY); break;
        case GUSB3PIPECTL(0):
            val &= ~((3 << 15));
            if (!(old & GUSB3PIPECTL_PHYSOFTRST) && (val & GUSB3PIPECTL_PHYSOFTRST)) {
                /* TODO: Implement Phy Soft Reset */
                qemu_log_mask(LOG_UNIMP, "%s: Phy Soft Reset not implemented\n", __func__);
                break;
            }
            if ((old & GUSB3PIPECTL_SUSPHY) != (val & GUSB3PIPECTL_SUSPHY)) {
                /* TODO: Implement Phy Suspend */
                qemu_log_mask(LOG_UNIMP, "%s: Phy (un)Suspend not implemented\n", __func__);
                break;
            }
            break;
        // keep the extra parentheses!!!!
        case GEVNTADRLO(0)... GEVNTCOUNT((DWC3_NUM_INTRS - 1)):
            usb_dwc3_gevntreg_write(s, addr, (addr - GEVNTADRLO(0)) >> 2, val);
            break;
        default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "%s: default: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n",
                      __func__, addr, val);
#endif
            break;
    }

    *mmio = val;

    if (iflg) { dwc3_update_irq(s); }
}

static uint64_t usb_dwc3_dreg_read(void* opaque, hwaddr addr, int index)
{
    DWC3State* s = opaque;
    uint32_t   val;
    uint32_t*  mmio;

    if (addr > DALEPENA) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }

    mmio = &s->dreg[index];
    val  = *mmio;

    return val;
}

static void usb_dwc3_dreg_write(void* opaque, hwaddr addr, int index, uint64_t val)
{
    DWC3State* s    = opaque;
    USBDevice* udev = &s->device.parent_obj;
    uint32_t*  mmio;
    uint32_t   old;
    int        iflg = 0;

    if (addr > DALEPENA) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return;
    }

    mmio = &s->dreg[index];
    old  = *mmio;

    switch (addr) {
        case DCFG: {
            int devaddr = DCFG_DEVADDR_GET(val);
            if (devaddr != udev->addr) {
                s->pending_addr = (uint8_t)devaddr;
                s->addr_pending = true;
            }
            break;
        }
        case DCTL:
            if (val & DCTL_CSFTRST) {
                dwc3_dcore_reset(s, true);
                iflg = true;
            }

            if (!(old & DCTL_RUN_STOP) && (val & DCTL_RUN_STOP)) {
                /* go on bus */
                usb_device_attach(udev, NULL);
                s->dsts &= ~DSTS_DEVCTRLHLT;
            }
            if ((old & DCTL_RUN_STOP) && !(val & DCTL_RUN_STOP)) {
                /* go off bus */
                if (udev->attached) { usb_device_detach(udev); }
                s->dsts |= DSTS_DEVCTRLHLT;
            }
            /* Self clearing bits */
            val &= ~DCTL_CSFTRST;
            break;
        case DSTS: val = old; break;
        case DGCMD:
            val &= ~DGCMD_CMDSTATUS;
            if (!(val & DGCMD_CMDACT)) { break; }
            /* TODO DGCMD */
            switch (DGCMD_CMDTYPE_GET(val)) {
                case DGCMD_SET_LMP:
                    qemu_log_mask(LOG_UNIMP,
                                  "%s: Set Link Function LPM is "
                                  "not implemented\n",
                                  __func__);
                    break;
                case DGCMD_SET_PERIODIC_PAR:
                    qemu_log_mask(LOG_UNIMP,
                                  "%s: Set Periodic Parameters is "
                                  "not implemented\n",
                                  __func__);
                    break;
                case DGCMD_XMIT_FUNCTION:
                    qemu_log_mask(LOG_UNIMP,
                                  "%s: Transmit Function Notification "
                                  "is not implemented\n",
                                  __func__);
                    break;
                default:
                    qemu_log_mask(LOG_UNIMP, "%s: Unsupported DGCMD\n", __func__);
                    val |= (DGCMD_CMDSTATUS);
                    break;
            }
            val &= ~DGCMD_CMDACT;
            if (val & DGCMD_CMDIOC) {
                struct dwc3_event_devt ioc = {1, 0, DEVICE_EVENT_CMD_CMPL};
                dwc3_device_event_ungated(s, ioc);
            }
            break;
        case DALEPENA:
            // already handled via *mmio
            break;
        default: break;
    }

    *mmio = val;

    if (iflg) { dwc3_update_irq(s); }
}

static uint64_t usb_dwc3_depcmdreg_read(void* opaque, hwaddr addr, int index)
{
    DWC3State* s = opaque;
    uint32_t   val;
    uint32_t*  mmio;

    // keep the extra parentheses!!!!
    if (addr > DEPCMD((DWC3_NUM_EPS - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
    mmio = &s->depcmdreg[index];
    val  = *mmio;

    return val;
}

#ifdef DEBUG_DWC3
static const char* DEPCMD_names[] = {
    [DEPCMD_CFG]        = "DEPCFG",
    [DEPCMD_XFERCFG]    = "DEPXFERCFG",
    [DEPCMD_GETEPSTATE] = "DEPGETEPSTATE",
    [DEPCMD_SETSTALL]   = "DEPSETSTALL",
    [DEPCMD_CLEARSTALL] = "DEPCSTALL",
    [DEPCMD_STARTXFER]  = "DEPSTRTXFER",
    [DEPCMD_UPDATEXFER] = "DEPUPDXFER",
    [DEPCMD_ENDXFER]    = "DEPENDXFER",
    [DEPCMD_STARTCFG]   = "DEPSTARTCFG",
};

static const char* dwc3_depcmd_name(uint32_t cmd)
{
    if (cmd < ARRAY_SIZE(DEPCMD_names) && DEPCMD_names[cmd] != NULL) { return DEPCMD_names[cmd]; }
    return "<reserved>";
}
#endif

static void usb_dwc3_depcmdreg_write(void* opaque, hwaddr addr, int index, uint64_t val)
{
    DWC3State*    s    = opaque;
    USBDevice*    udev = &s->device.parent_obj;
    uint32_t*     mmio;
    int           iflg = 0;
    uint32_t      epid = index >> 2;
    DWC3Endpoint* ep   = &s->eps[epid];

    // keep the extra parentheses!!!!
    if (addr > DEPCMD((DWC3_NUM_EPS - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return;
    }

    mmio = &s->depcmdreg[index];

    switch (DEPCMDPAR2(0) + (addr & 0xc)) {
        case DEPCMD(0): {
            uint32_t                 par0  = s->depcmdpar0(epid);
            uint32_t                 par1  = s->depcmdpar1(epid);
            uint32_t G_GNUC_UNUSED   par2  = s->depcmdpar2(epid);
            struct dwc3_event_depevt ioc   = {0, epid, DEPEVT_EPCMDCMPLT, 0, 0, DEPCMD_CMD_GET(val) << 8};
            val                           &= ~DEPCMD_STATUS_MASK;
            if (!(val & DEPCMD_CMDACT)) {
                if (!(val & DEPCMD_CMDIOC) && DEPCMD_CMD_GET(val) == DEPCMD_UPDATEXFER) {
#ifdef DEBUG_DWC3
                    qemu_log_mask(LOG_UNIMP,
                                  "Special no response update?: DEPCMD: %s epid: %d"
                                  " par2: 0x%x par1: 0x%x par0: 0x%x\n",
                                  dwc3_depcmd_name(DEPCMD_CMD_GET(val)), epid, par2, par1, par0);
#endif
                    if (ep->xfer == NULL) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "%s: no-response UPDATEXFER on ep %d without an active transfer\n", __func__,
                                      epid);
                    }
                    else {
                        DPRINTF("%s: Special no response update?: tdaddr: 0x%" HWADDR_PRIx "\n", __func__,
                                ep->xfer->tdaddr);
                        dwc3_td_fetch(s, ep->xfer, ep->xfer->tdaddr);
                        ep->not_ready = false;
                        dwc3_ep_run_schedule_update(s, ep);
                    }
                }
                break;
            }
#ifdef DEBUG_DWC3
            qemu_log_mask(LOG_UNIMP,
                          "DEPCMD: %s epid: %d "
                          "par2: 0x%x par1: 0x%x par0: 0x%x\n",
                          dwc3_depcmd_name(DEPCMD_CMD_GET(val)), epid, par2, par1, par0);
#endif
            ep->not_ready = false;

            switch (DEPCMD_CMD_GET(val)) {
                case DEPCMD_CFG: {
                    int epnum = DEPCFG_EP_NUMBER(par1);
                    assert_cmpuint(epnum, ==, epid);
                    if (epid == 0 || epid == 1 || (epnum >> 1) == 0) {
                        if (epnum != epid) {
                            val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                            // this will be set below anyway
                            // ioc.status = 1;
                            break;
                        }
                    }
                    ep->epnum                = epnum;
                    ep->intrnum              = DEPCFG_INT_NUM(par1);
                    ep->event_en             = DEPCFG_EVENT_EN(par1);
                    ep->uep                  = usb_ep_get(udev, epnum & 1 ? USB_TOKEN_IN : USB_TOKEN_OUT, epnum >> 1);
                    ep->uep->max_packet_size = DEPCFG_MAX_PACKET_SIZE(par0);
                    ep->uep->type            = DEPCFG_EP_TYPE(par0);
                    if (DEPCFG_ACTION(par0) == DEPCFG_ACTION_INIT) { ep->dseqnum = 0; }
                    DPRINTF("%s: DEPCMD_XFERCFG: par1: epid: %d fifo: %d bulk: %d "
                            "epnum: %d stream: %d\n",
                            __func__, epid, (par1 & DEPCFG_FIFO_BASED) != 0, (par1 & DEPCFG_BULK_BASED) != 0, epnum,
                            (par1 & DEPCFG_STREAM_CAPABLE) != 0);
                    DPRINTF("%s: DEPCMD_XFERCFG: par1: bInterval_m1: %d bit15: %d "
                            "bit14: %d stream_event: %d bit12: %d\n",
                            __func__, DEPCFG_BINTERVAL_M1(par1), (par1 & DEPCFG_PAR1_BIT15) != 0,
                            (par1 & DEPCFG_PAR1_BIT14) != 0, (par1 & DEPCFG_STREAM_EVENT_EN) != 0,
                            (par1 & DEPCFG_PAR1_BIT12) != 0);
                    DPRINTF("%s: DEPCMD_XFERCFG: par1: fifo_error: %d xfer_not_ready: "
                            "%d xfer_in_progress: %d xfer_complete: %d bit7: %d\n",
                            __func__, (par1 & DEPCFG_FIFO_ERROR_EN) != 0, (par1 & DEPCFG_XFER_NOT_READY_EN) != 0,
                            (par1 & DEPCFG_XFER_IN_PROGRESS_EN) != 0, (par1 & DEPCFG_XFER_COMPLETE_EN) != 0,
                            (par1 & DEPCFG_PAR1_BIT7) != 0);
                    DPRINTF("%s: DEPCMD_XFERCFG: par1: bit6: %d bit5: %d int_num: %d\n", __func__,
                            (par1 & DEPCFG_PAR1_BIT6) != 0, (par1 & DEPCFG_PAR1_BIT5) != 0, DEPCFG_INT_NUM(par1));
                    DPRINTF("%s: DEPCMD_XFERCFG: par0: ep_type: %d mps: %d "
                            "fifo_number: %d burst_size: %d action: %d\n",
                            __func__, DEPCFG_EP_TYPE(par0), DEPCFG_MAX_PACKET_SIZE(par0), DEPCFG_FIFO_NUMBER(par0),
                            DEPCFG_BURST_SIZE(par0), DEPCFG_ACTION(par0));
                    break;
                }
                case DEPCMD_XFERCFG:
                    DPRINTF("%s: DEPCMD_XFERCFG: ep->epid: %d\n", __func__, ep->epid);
                    if (DEPXFERCFG_NUMXFERRES(par0) != 1) { val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE); }
                    break;
                case DEPCMD_GETSEQNUMBER:
                    // case DEPCMD_GETEPSTATE:
                    DPRINTF("%s: DEPCMD_GETSEQNUMBER/DEPCMD_GETEPSTATE: ep->epid: %d\n", __func__, ep->epid);
                    ioc.parameters |= ep->dseqnum & 0xf;
                    val            &= ~DEPCMD_PARAM_MASK;
                    val            |= DEPCMD_PARAM(ep->dseqnum & 0xf);
                    break;
                case DEPCMD_SETSTALL:
                    ep->stalled = true;
                    if (epid == 0 || epid == 1) {
                        DWC3Endpoint* pair = &s->eps[epid ^ 1];

                        pair->stalled = true;
                        dwc3_ep_run_schedule_update(s, pair);
                    }
                    dwc3_ep_run_schedule_update(s, ep);
                    break;
                case DEPCMD_CLEARSTALL:
                    if (epid == 0 || epid == 1) {
                        /* Automatically cleared upon SETUP */
                        break;
                    }
                    ep->stalled = false;
                    ep->dseqnum = 0;
                    dwc3_ep_run_schedule_update(s, ep);
                    break;
                case DEPCMD_STARTXFER: {
                    dma_addr_t tdaddr = dwc3_addr64(par1, par0);
                    if ((tdaddr & 0xf) != 0) {
                        qemu_log_mask(LOG_GUEST_ERROR, "DEPCMD_STARTXFER: misaligned TD address 0x%" HWADDR_PRIx "\n",
                                      tdaddr);
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        break;
                    }
                    if (ep->xfer) {
                        qemu_log_mask(LOG_GUEST_ERROR, "DEPCMD_STARTXFER: xfer existed\n");
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        break;
                    }
                    DPRINTF("%s: DEPCMD_STARTXFER: ep->epid: %d tdaddr: 0x%" HWADDR_PRIx "\n", __func__, ep->epid,
                            tdaddr);
                    ep->xfer = dwc3_xfer_alloc(s, epid, tdaddr);
                    if (!ep->xfer) {
                        qemu_log_mask(LOG_GUEST_ERROR, "DEPCMD_STARTXFER: Cannot alloc xfer\n");
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        break;
                    }
                    val            &= ~DEPCMD_PARAM_MASK;
                    val            |= DEPCFG_RSC_IDX(ep->xfer->rsc_idx);
                    ioc.parameters |= ep->xfer->rsc_idx & 0x7f;
                    dwc3_ep_run_schedule_update(s, ep);
                    break;
                }
                case DEPCMD_UPDATEXFER: {
                    if (!ep->xfer || (ep->xfer->rsc_idx) != DEPCFG_RSC_IDX_GET(val)) {
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        if (!ep->xfer) {
                            DPRINTF("%s: UPDATEXFER: Unknown rsc_idx: ep->epid: %d "
                                    "!ep->xfer %d ep->xfer->rsc_idx N/A "
                                    "DEPCFG_RSC_IDX_GET(val) 0x%" PRIx64 "\n",
                                    __func__, ep->epid, !ep->xfer, DEPCFG_RSC_IDX_GET(val));
                        }
                        else {
                            DPRINTF("%s: UPDATEXFER: Unknown rsc_idx: ep->epid: %d "
                                    "!ep->xfer %d ep->xfer->rsc_idx 0x%x "
                                    "DEPCFG_RSC_IDX_GET(val) 0x%" PRIx64 "\n",
                                    __func__, ep->epid, !ep->xfer, ep->xfer->rsc_idx, DEPCFG_RSC_IDX_GET(val));
                        }
                        qemu_log_mask(LOG_GUEST_ERROR, "UPDATEXFER: Unknown rsc_idx\n");
                        break;
                    }
                    DPRINTF("%s: DEPCMD_UPDATEXFER: ep->epid: %d tdaddr: 0x%" HWADDR_PRIx "\n", __func__, ep->epid,
                            ep->xfer->tdaddr);
                    dwc3_td_fetch(s, ep->xfer, ep->xfer->tdaddr);
                    if (ep->xfer->count == 0) {
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        qemu_log_mask(LOG_GUEST_ERROR, "UPDATEXFER: empty xfer\n");
                        break;
                    }
                    dwc3_ep_run_schedule_update(s, ep);
                    break;
                }
                case DEPCMD_ENDXFER:
                    if (ep->xfer) {
                        dwc3_td_free(s, ep->xfer);
                        ep->xfer = NULL;
                        if (ep->uep) {
                            USBPacket* p = NULL;
                            QTAILQ_FOREACH (p, &ep->uep->queue, queue) {
                                p->status = USB_RET_IOERROR;
                                usb_packet_complete(udev, p);
                            }
                        }
                    }
                    else {
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                    }
                    break;
                case DEPCMD_STARTCFG: {
                    int rsc_idx = DEPCFG_RSC_IDX_GET(val);
                    if (!ep->xfer) {
                        DPRINTF("%s: DEPCMD_STARTCFG: rsc_idx: ep->epid: %d !ep->xfer %d "
                                "ep->xfer->rsc_idx N/A DEPCFG_RSC_IDX_GET(val) 0x%x\n",
                                __func__, ep->epid, !ep->xfer, rsc_idx);
                    }
                    else {
                        DPRINTF("%s: DEPCMD_STARTCFG: rsc_idx: ep->epid: %d !ep->xfer %d "
                                "ep->xfer->rsc_idx 0x%x DEPCFG_RSC_IDX_GET(val) 0x%x\n",
                                __func__, ep->epid, !ep->xfer, ep->xfer->rsc_idx, rsc_idx);
                    }
                    if (rsc_idx != 0 && rsc_idx != 2) {
                        val |= DEPCMD_STATUS_SET(DEPEVT_TRANSFER_NO_RESOURCE);
                        qemu_log_mask(LOG_GUEST_ERROR, "DEPCMD_STARTCFG: invalid rsc_idx %d\n", rsc_idx);
                        break;
                    }
                    s->global_rsc_idx_counter = rsc_idx;
                    for (int i = 0; i < DWC3_NUM_EPS; i++) { s->eps[i].rsc_idx_counter = s->global_rsc_idx_counter; }
                    break;
                }
                default: break;
            }

            val &= ~DEPCMD_CMDACT;

            if (val & DEPCMD_CMDIOC) {
                ioc.status = DEPCMD_STATUS_GET(val);
                dwc3_ep_event(s, epid, ioc);
            }
            break;
        }
        default: break;
    }

    *mmio = val;

    if (iflg) { dwc3_update_irq(s); }
}

static uint64_t usb_dwc3_read(void* opaque, hwaddr addr, unsigned size)
{
    uint64_t val = 0;
    switch (addr) {
        case GLOBALS_REGS_START ... GLOBALS_REGS_END:
            val = usb_dwc3_glbreg_read(opaque, addr, (addr - GLOBALS_REGS_START) >> 2);
            break;
        case DEVICE_REGS_START ... DEVICE_REGS_END:
            val = usb_dwc3_dreg_read(opaque, addr, (addr - DEVICE_REGS_START) >> 2);
            break;
        case DEPCMD_REGS_START ... DEPCMD_REGS_END:
            val = usb_dwc3_depcmdreg_read(opaque, addr, (addr - DEPCMD_REGS_START) >> 2);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: addr: 0x%" HWADDR_PRIx "\n", __func__, addr);
            // assert_not_reached();
            break;
    };
    DPRINTF("%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__, addr, val);
    return val;
}

static void usb_dwc3_write(void* opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DPRINTF("%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__, addr, val);
    switch (addr) {
        case GLOBALS_REGS_START ... GLOBALS_REGS_END:
            usb_dwc3_glbreg_write(opaque, addr, (addr - GLOBALS_REGS_START) >> 2, val);
            break;
        case DEVICE_REGS_START ... DEVICE_REGS_END:
            usb_dwc3_dreg_write(opaque, addr, (addr - DEVICE_REGS_START) >> 2, val);
            break;
        case DEPCMD_REGS_START ... DEPCMD_REGS_END:
            usb_dwc3_depcmdreg_write(opaque, addr, (addr - DEPCMD_REGS_START) >> 2, val);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__, addr, val);
            // assert_not_reached();
            break;
    };
}

static const MemoryRegionOps usb_dwc3_ops = {
    .read       = usb_dwc3_read,
    .write      = usb_dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 4,
            .max_access_size = 4,
        },
};

static void usb_dwc3_realize(DeviceState* dev, Error** errp)
{
    DWC3State*    s   = DWC3_USB(dev);
    SysBusDevice* sbd = SYS_BUS_DEVICE(dev);
    Error*        err = NULL;
    Object*       obj;
    MemoryRegion* dma_mr;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, dma_mr, "dwc3");

    obj                        = object_property_get_link(OBJECT(dev), "dma-xhci", &error_abort);
    s->sysbus_xhci.xhci.dma_mr = MEMORY_REGION(obj);

    sysbus_realize(SYS_BUS_DEVICE(&s->sysbus_xhci), &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    QEMU_BUILD_BUG_ON(DWC3_NUM_INTRS < XHCI_MAXINTRS);
    s->numintrs = s->sysbus_xhci.xhci.numintrs;

    memory_region_add_subregion(&s->iomem, 0, sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sysbus_xhci), 0));
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->sysbus_xhci));
    s->sysbus_xhci.xhci.intr_raise = dwc3_host_intr_raise;
}

static void usb_dwc3_init(Object* obj)
{
    DWC3State*    s   = DWC3_USB(obj);
    SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "dwc3-xhci", &s->sysbus_xhci, TYPE_XHCI_SYSBUS);
    object_initialize_child(obj, "dwc3-usb-device", &s->device, TYPE_DWC3_USB_DEVICE);
    qdev_alias_all_properties(DEVICE(&s->sysbus_xhci), obj);

    memory_region_init_io(&s->iomem, obj, &usb_dwc3_ops, s, "dwc3-io", DWC3_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qemu_mutex_init(&s->lock);

    for (int i = 0; i < DWC3_NUM_EPS; i++) { s->eps[i].epid = i; }
}

static void dwc3_device_apply_pending_addr(DWC3State* s, USBPacket* p);

static void dwc3_process_packet(DWC3State* s, DWC3Endpoint* ep, USBPacket* p)
{
    USBDevice*      udev     = &s->device.parent_obj;
    DWC3BufferDesc* desc     = NULL;
    DWC3Transfer*   xfer     = NULL;
    DWC3Endpoint*   setup_ep = &s->eps[ep->epid & ~1];

    DPRINTF("%s: pid: 0x%x epid: %d id: 0x%" PRIx64 " (%d/%" PRIx64 ") "
            "stalled: %d\n",
            __func__, p->pid, ep->epid, p->id, p->actual_length, usb_packet_size(p), ep->stalled);

    // apparently for the dwc3_ep_run callpath for whatever reason
    if (ep->stalled && p->actual_length == 0) {
        p->status = USB_RET_STALL;
        // maybe do usb_packet_complete, maybe not. no idea.
        goto complete;
        // return;
    }

    xfer = ep->xfer;
    if (xfer == NULL) {
        // xfer == NULL == URB_COMPLETE
        assert(p->status != USB_RET_ASYNC);
        if (setup_ep->last_control_command == TRBCTL_CONTROL_SETUP) {
            DPRINTF("%s: TRBCTL_CONTROL_SETUP: setup_packet.wLength == 0x%x\n", __func__,
                    setup_ep->setup_packet.wLength);
            // wLength==0 means two-stage setup
            if (!setup_ep->setup_packet.wLength) {
                struct dwc3_event_depevt event  = {0, ep->epid, DEPEVT_XFERNOTREADY, 0, 0};
                event.status                   |= DEPEVT_STATUS_CONTROL_STATUS;
                dwc3_ep_event(s, ep->epid, event);
            }
            // async, because we're waiting for data here.
            // Don't return "success" or "nak" here, must return "async"!!
            // p->status = USB_RET_ASYNC;
            p->status = USB_RET_NAK;
            goto complete;
        }
        else if (setup_ep->last_control_command == TRBCTL_CONTROL_DATA) {
            DPRINTF("%s: ep->xfer == NULL: TRBCTL_CONTROL_DATA second-stage\n", __func__);
            struct dwc3_event_depevt event = {0, ep->epid, DEPEVT_XFERNOTREADY, 0, 0};
            if (setup_ep->send_not_ready_control_data) { event.status |= DEPEVT_STATUS_CONTROL_DATA; }
            else {
                event.status |= DEPEVT_STATUS_CONTROL_STATUS;
            }
            // needs DEPEVT_STATUS_TRANSFER_ACTIVE? does it really need that,
            // though?
            dwc3_ep_event(s, ep->epid, event);
            // maybe don't use NAK here, or maybe do???
            // p->status = USB_RET_ASYNC;
            p->status = USB_RET_NAK;
            goto complete;
        }
        else if (setup_ep->last_control_command == TRBCTL_CONTROL_STATUS2) {
            DPRINTF("%s: ep->xfer == NULL: TRBCTL_CONTROL_STATUS2\n", __func__);
            // must set USB_RET_ASYNC ???^H^H^H must use NAK, since ASYNC causes
            // DART faults.
            p->status = USB_RET_NAK;
            goto complete;
        }
        else if (setup_ep->last_control_command == TRBCTL_CONTROL_STATUS3) {
            DPRINTF("%s: ep->xfer == NULL: TRBCTL_CONTROL_STATUS3\n", __func__);
            // must set USB_RET_ASYNC ???^H^H^H must use NAK, since ASYNC causes
            // DART faults.
            p->status = USB_RET_NAK;
            goto complete;
        }
        else if (setup_ep->last_control_command == 0) {
            DPRINTF("%s: ep->xfer == NULL: ELSE: last_control_command_0x0: %d, "
                    "return.\n",
                    __func__, setup_ep->last_control_command);
            // p->status = USB_RET_ASYNC;
            p->status = USB_RET_NAK;
            // p->status = USB_RET_SUCCESS;
            goto complete;
            // return;
        }
        DPRINTF("%s: ep->xfer == NULL: ELSE: last_control_command: %d\n", __func__, setup_ep->last_control_command);
        // using DEPEVT_XFERNOTREADY will cause this chain:
        // AppleUSBXDCI::ep0OutEventOccurred ->
        // IOUSBDeviceControlRequest::stallSetupRequest
        // Probably don't set |= DEPEVT_STATUS_CONTROL_* here!!
        // don't use DEPEVT_STATUS_TRANSFER_ACTIVE here?
        struct dwc3_event_depevt event = {0, ep->epid, DEPEVT_XFERNOTREADY, 0, 0};
        // // // event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
        // // // // // event.endpoint_event = DEPEVT_XFERCOMPLETE;
        dwc3_ep_event(s, ep->epid, event);
        // using NAK or SUCCESS here might hurt performance, but might avoid
        // timeouts
        // p->status = USB_RET_ASYNC;
        p->status = USB_RET_NAK;
        // p->status = USB_RET_SUCCESS;
        goto complete;
        // return;
    }
    else {
        // else: xfer != NULL == URB_SUBMIT
        DPRINTF("%s: xfer != NULL: xfer->tdaddr: 0x%" HWADDR_PRIx "\n", __func__, xfer->tdaddr);
#ifdef DEBUG_DWC3
    #if 1
        dwc3_td_dump(xfer);
    #endif
#endif
        desc = QTAILQ_FIRST(&xfer->buffers);
        if (desc == NULL) {
            // one specific packet keeps repeating if returning NAK (or
            // SUCCESS^H^H^H) here, despite everything seem to work as usual.
            // let's try to abuse that to keep the host from choking/starving us
            // trying to abuse that to keep the host from choking/starving us
            // does seem to work still recent/needed with that rsc_idx fix?
            DPRINTF("%s: xfer != NULL: desc == NULL\n", __func__);
            struct dwc3_event_depevt event = {0, ep->epid, DEPEVT_XFERNOTREADY, 0, 0};
            // Probably don't set |= DEPEVT_STATUS_CONTROL_* here!!
            event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
            dwc3_ep_event(s, ep->epid, event);
            // using NAK or SUCCESS here might hurt performance, but might avoid
            // timeouts
            // p->status = USB_RET_ASYNC;
            p->status = USB_RET_NAK;
            // using "SUCCESS" will cause the "AppleUSBXDCI" to be used and its
            // debug messages to show, but using "nak" or "async" will make the
            // recovery process continue. must return "success" here?
            // p->status = USB_RET_SUCCESS;
            // using "complete" instead of "return" here
            // might cause a loop, but using "return" causes the loop to be even
            // earlier. using xfercomplete/success/complete will result in dart
            // errors using xferinprogress/success/complete will result in dart
            // errors
            goto complete;
        }
        else {
            DPRINTF("%s: xfer != NULL: desc != NULL\n", __func__);
        }

        // can either return xfer_size, which also can be zero, or return -1 in
        // case of an error
        int xfer_size = dwc3_bd_copy(s, desc, p);
        if (desc->ended || xfer_size == -1) {
            QTAILQ_REMOVE(&xfer->buffers, desc, queue);
            xfer->count--;
            dwc3_bd_free(s, desc);
            if (xfer->count == 0 && xfer->can_free) {
                ep->xfer = NULL;
                smp_wmb();
                dwc3_td_free(s, xfer);
            }
        }
    }
complete:
    // usb_packet_complete asserts on ASYNC (not always?) and NAK. maybe only
    // while not is_inflight? keep that if, and don't call "complete" if the
    // status is "async". it looks like ASYNC and NAK can be interchanged most
    // of the times without change in behavior.
    if (p->status != USB_RET_ASYNC) {
        if (usb_packet_is_inflight(p)) {
            if (p->status == USB_RET_NAK) { p->status = USB_RET_IOERROR; }
            // usb_packet_complete must be inside the usb_packet_is_inflight
            // "if"
            usb_packet_complete(udev, p);
        }
    }

    dwc3_device_apply_pending_addr(s, p);
}

static void dwc3_usb_device_realize(USBDevice* dev, Error** errp)
{
    // if you use _SUPER here, you'll run into this "Invalid ep0 maxpacket: 9"
    // not sure if I can or even should use a workaround like the one found in
    // host-libusb having _HIGH here while having _SUPER in dev-tcp-remote
    // causes "Warning: speed mismatch ...", followed by an abort in the
    // companion
    dev->speed        = USB_SPEED_HIGH;
    dev->speedmask    = USB_SPEED_MASK_HIGH;
    dev->flags       |= (1 << USB_DEV_FLAG_IS_HOST);
    dev->auto_attach  = false;
}

static void dwc3_usb_device_handle_attach(USBDevice* dev)
{
    DWC3DeviceState* udev = container_of(dev, DWC3DeviceState, parent_obj);
    DWC3State*       s    = container_of(udev, DWC3State, device);

    dwc3_usb_device_connected_speed(s);
    s->dsts = (s->dsts & ~DSTS_USBLNKST_MASK) | DSTS_USBLNKST(LINK_STATE_U0);

    struct dwc3_event_devt ulschng = {1, 0, DEVICE_EVENT_LINK_STATUS_CHANGE, 0, LINK_STATE_U0};
    struct dwc3_event_devt connect = {1, 0, DEVICE_EVENT_CONNECT_DONE};
    dwc3_device_event(s, ulschng);
    dwc3_device_event(s, connect);
}

static void dwc3_usb_device_handle_detach(USBDevice* dev)
{
    DWC3DeviceState* udev = container_of(dev, DWC3DeviceState, parent_obj);
    DWC3State*       s    = container_of(udev, DWC3State, device);

    dwc3_usb_device_connected_speed(s);
    s->dsts = (s->dsts & ~DSTS_USBLNKST_MASK) | DSTS_USBLNKST(LINK_STATE_SS_DIS);

    struct dwc3_event_devt ulschng = {1, 0, DEVICE_EVENT_LINK_STATUS_CHANGE, 0, LINK_STATE_SS_DIS};
    dwc3_device_event(s, ulschng);
    struct dwc3_event_devt disconn = {1, 0, DEVICE_EVENT_DISCONNECT};
    dwc3_device_event(s, disconn);
}

static void dwc3_usb_device_handle_reset(USBDevice* dev)
{
    DWC3DeviceState* udev = container_of(dev, DWC3DeviceState, parent_obj);
    DWC3State*       s    = container_of(udev, DWC3State, device);

    s->dcfg &= ~DCFG_DEVADDR_MASK;
    dwc3_usb_device_connected_speed(s);

    struct dwc3_event_devt usbrst = {1, 0, DEVICE_EVENT_RESET};
    dwc3_device_event(s, usbrst);
    struct dwc3_event_devt connect = {1, 0, DEVICE_EVENT_CONNECT_DONE};
    dwc3_device_event(s, connect);
}

static void dwc3_usb_device_cancel_packet(USBDevice* dev, USBPacket* p)
{
    /* TODO: complete td if packet partially complete */
    DPRINTF("%s: pid: 0x%x ep: %d id: 0x%" PRIx64 "\n", __func__, p->pid, p->ep->nr, p->id);
    // assert(p->actual_length == 0);
}

static void dwc3_device_apply_pending_addr(DWC3State* s, USBPacket* p)
{
    if (!s->addr_pending) { return; }
    if (p->ep->nr != 0 || p->pid != USB_TOKEN_IN) { return; }
    if (p->status != USB_RET_SUCCESS) { return; }
    if (p->iov.size != 0) { return; }

    s->addr_pending              = false;
    USB_DEVICE(&s->device)->addr = s->pending_addr;
    trace_usb_set_addr(s->pending_addr);
}

static void dwc3_usb_device_handle_packet(USBDevice* dev, USBPacket* p)
{
    DWC3DeviceState* udev = container_of(dev, DWC3DeviceState, parent_obj);
    DWC3State*       s    = container_of(udev, DWC3State, device);

    DPRINTF("%s: entered function\n", __func__);

    assert(bql_locked());
    QEMU_LOCK_GUARD(&s->lock);

    int           epid = dwc3_packet_find_epid(s, p);
    DWC3Endpoint* ep;

    if (epid == -1) {
        // qemu_log_mask(LOG_GUEST_ERROR,
        //               "%s: Unable to find ep for nr: %d pid: 0x%x\n",
        //               __func__, p->ep->nr, p->pid);
        DPRINTF("%s: Unable to find ep for nr: %d pid: 0x%x\n", __func__, p->ep->nr, p->pid);
        p->status = USB_RET_NAK;
        goto status_update;
    }

    ep = &s->eps[epid];
    if (!ep->uep) {
        DPRINTF("%s: !ep->uep\n", __func__);
        // p->status = USB_RET_NAK;
        // goto status_update;
        return;
    }

    if (p->pid == USB_TOKEN_SETUP && ep->uep->nr == 0) {
        s->eps[0].stalled   = false;
        s->eps[0].not_ready = false;
        s->eps[1].stalled   = false;
        s->eps[1].not_ready = false;
    }

    if (ep->stalled) {
        DPRINTF("%s: ep->stalled\n", __func__);
        p->status = USB_RET_STALL;
        return;
    }

    if (!(s->dalepena & (1 << epid))) {
        DPRINTF("%s: ! s->dalepena 0x%x epid %d\n", __func__, s->dalepena, epid);
        // dwc2 would do ASYNC instead of NAK in this case.
        p->status = USB_RET_NAK;
        goto status_update;
    }

    dwc3_process_packet(s, ep, p);

status_update:
    if (usb_packet_is_inflight(p)) {
        if (p->status == USB_RET_NAK) {
            // This rewrite must happen instead of calling
            // "dwc3_process_packet"!
            p->status = USB_RET_IOERROR;
        }
    }
}

// maybe add an async handler for doing all ep's, just like in dwc2.

static void dwc3_ep_run(DWC3State* s, DWC3Endpoint* ep)
{
    USBPacket* p = NULL;

    assert(bql_locked());
    QEMU_LOCK_GUARD(&s->lock);

    assert_cmpuint(ep->epid, ==, ep->epnum);

    if (!ep->uep) {
        DPRINTF("%s: !ep->uep\n", __func__);
        return;
    }

    // still have to test whether the _first or _foreach variant is better
    // this means both, correctness and speed. everywhere it's used
    QTAILQ_FOREACH (p, &ep->uep->queue, queue) {
        DPRINTF("%s: pid: 0x%x ep: %d epid: %d id: 0x%" PRIx64 "\n", __func__, p->pid, p->ep->nr, ep->epid, p->id);
        dwc3_process_packet(s, ep, p);
    }
}

typedef struct
{
    DWC3State*    s;
    DWC3Endpoint* ep;
} DWC3EPRunUpdate;

static DWC3EPRunUpdate* dwc3_new_ep_run_update(DWC3State* s, DWC3Endpoint* ep)
{
    DWC3EPRunUpdate* update = g_new(DWC3EPRunUpdate, 1);
    update->s               = s;
    update->ep              = ep;
    return update;
}

static void dwc3_ep_run_update_bh(void* opaque)
{
    DWC3EPRunUpdate* update = opaque;

    // is already locked inside dwc3_ep_run
    dwc3_ep_run(update->s, update->ep);

    g_free(opaque);
}

// Borrowed from mt-spi. Thank you, Visual!
static void dwc3_ep_run_schedule_update(DWC3State* s, DWC3Endpoint* ep)
{
    // easier to make it temporarily sync again here, instead of all the
    // callers. return dwc3_ep_run(s, ep);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), dwc3_ep_run_update_bh, dwc3_new_ep_run_update(s, ep));
}

static void dwc3_usb_device_class_initfn(ObjectClass* klass, const void* data)
{
    DeviceClass*    dc = DEVICE_CLASS(klass);
    USBDeviceClass* uc = USB_DEVICE_CLASS(klass);

    uc->realize        = dwc3_usb_device_realize;
    uc->product_desc   = "DWC3 USB Device";
    uc->unrealize      = NULL;
    uc->cancel_packet  = dwc3_usb_device_cancel_packet;
    uc->handle_attach  = dwc3_usb_device_handle_attach;
    uc->handle_detach  = dwc3_usb_device_handle_detach;
    uc->handle_reset   = dwc3_usb_device_handle_reset;
    uc->handle_data    = NULL;
    uc->handle_control = NULL;
    uc->handle_packet  = dwc3_usb_device_handle_packet;
    uc->flush_ep_queue = NULL;
    uc->ep_stopped     = NULL;
    uc->alloc_streams  = NULL;
    uc->free_streams   = NULL;
    uc->usb_desc       = NULL;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void usb_dwc3_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);

    rc->phases.enter = dwc3_reset_enter;
    rc->phases.hold  = dwc3_reset_hold;
    rc->phases.exit  = dwc3_reset_exit;

    dc->realize = usb_dwc3_realize;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static const TypeInfo dwc3_usb_device_type_info = {
    .name          = TYPE_DWC3_USB_DEVICE,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(DWC3DeviceState),
    .class_init    = dwc3_usb_device_class_initfn,
};

static const TypeInfo usb_dwc3_info = {
    .name          = TYPE_DWC3_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWC3State),
    .instance_init = usb_dwc3_init,
    .class_init    = usb_dwc3_class_init,
};

static void usb_dwc3_register_types(void)
{
    type_register_static(&dwc3_usb_device_type_info);
    type_register_static(&usb_dwc3_info);
}

type_init(usb_dwc3_register_types)
