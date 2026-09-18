/*
 * Apple Smart Input/Output DMA Controller.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/dma/apple_sio.h"
#include "hw/misc/a7iop/rtkit.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "system/dma.h"

#if 0
    #define SIO_LOG_MSG(ep, msg)                                                                       \
        do {                                                                                           \
            qemu_log_mask(LOG_GUEST_ERROR, "SIO: message: ep=%X msg=0x" HWADDR_FMT_plx "\n", ep, msg); \
        }                                                                                              \
        while (0)
#else
    #define SIO_LOG_MSG(ep, msg) \
        do { }                   \
        while (0)
#endif

#define SIO_NUM_EPS (0xDC)

typedef struct
{
    uint32_t xfer;
    uint32_t timeout;
    uint32_t fifo;
    uint32_t trigger;
    uint32_t limit;
    uint32_t field_14;
    uint32_t field_18;
} QEMU_PACKED SIODMAConfig;

typedef struct
{
    uint64_t addr;
    uint32_t len;
} QEMU_PACKED SIODMASegment;

typedef struct SIODMABuffer
{
    SIODMASegment* segments;
    QEMUSGList     sgl;
    QEMUIOVector   iov;
    uint8_t        tag;
    bool           mapped;
    uint32_t       segment_count;
    uint64_t       completed;
    uint64_t       start_timestamp;
    QTAILQ_ENTRY(SIODMABuffer) next;
} SIODMABuffer;

struct AppleSIODMAEndpoint
{
    SIODMAConfig config;
    DMADirection direction;
    QemuMutex    mutex;
    uint8_t      id;
    QTAILQ_HEAD(, SIODMABuffer) buffers;
};

struct AppleSIOClass
{
    /*< private >*/
    AppleRTKitClass base_class;

    /*< public >*/
    DeviceRealize    parent_realize;
    ResettablePhases parent_reset;
};

struct AppleSIO
{
    /*< private >*/
    AppleRTKit parent_obj;

    /*< public >*/
    MemoryRegion        ascv2_iomem;
    MemoryRegion*       dma_mr;
    AddressSpace        dma_as;
    AppleSIODMAEndpoint eps[SIO_NUM_EPS];
    uint32_t            protocol_version;
    uint32_t            segment_size;
    uint64_t            segment_base;
    uint64_t            resp_base;
    uint64_t            gtimer_freq;
};

typedef enum
{
    // 0 -> sio_aerror_2
    OP_PING      = 1,
    OP_GET_PARAM = 2,
    OP_SET_PARAM = 3,
    // 4 -> sio_aerror_2
    /// invalid values -> sio_aerror_3
    OP_CONFIGURE = 5,
    /// tag==0 -> sio_aerror_4,
    /// segment_count overflow/oob -> sio_aerror_8
    /// segment_region==0 -> sio_aerror_9
    OP_MAP            = 6,
    OP_QUERY          = 7,
    OP_STOP           = 8,
    OP_ACK            = 101,
    OP_GET_PARAM_RESP = 103,
    OP_COMPLETE       = 104,
    OP_QUERY_OK       = 105,
    // Errors
    OP_SYNC_ERROR      = 2,
    OP_SET_PARAM_ERROR = 3,
    OP_ASYNC_ERROR     = 102,
} SIOOp;

typedef enum
{
    EP_CONTROL = 0,
    EP_PERF    = 3,
} SIOEndpoint;

typedef enum
{
    PARAM_PROTOCOL_VERSION = 0,
    PARAM_SEGMENT_BASE,
    /// pre-divided by segment entry size
    PARAM_SEGMENT_SIZE,
    PARAM_RESPONSE_BASE = 11,
    PARAM_RESPONSE_SIZE,
    PARAM_PERF_BASE,
    PARAM_PERF_SIZE,
    PARAM_PANIC_BASE,
    PARAM_PANIC_SIZE,
    PARAM_PIO_BASE = 26,
    PARAM_PIO_SIZE,
    PARAM_DEVICES_BASE,
    PARAM_DEVICES_SIZE,
    PARAM_ASCWRAP_TUNABLES_BASE,
    PARAM_ASCWRAP_TUNABLES_SIZE,
    PARAM_MISC_TUNABLES_BASE,
    PARAM_MISC_TUNABLES_SIZE,
    PARAM_SHIMS_BASE,
    PARAM_SHIMS_SIZE,
    PARAM_PS_REGS_BASE,
    PARAM_PS_REGS_SIZE,
    PARAM_FORWARD_IRQS_BASE,
    PARAM_FORWARD_IRQS_SIZE,
} SmartIOParameter;

typedef union
{
    uint64_t raw;
    struct
    {
        uint8_t  ep;
        uint8_t  tag;
        uint8_t  op;
        uint8_t  param;
        uint32_t data;
    };
} SIOMessage;

static void apple_sio_dma_destroy_buffer(AppleSIODMAEndpoint* ep, SIODMABuffer* buf)
{
    int      i;
    uint64_t completed;
    uint64_t access_len;

    if (buf->mapped) {
        completed = buf->completed;
        for (i = 0; i < buf->iov.niov; ++i) {
            access_len = MIN(completed, buf->iov.iov[i].iov_len);
            dma_memory_unmap(buf->sgl.as, buf->iov.iov[i].iov_base, buf->iov.iov[i].iov_len, ep->direction, access_len);
            completed -= access_len;
        }
    }

    qemu_iovec_destroy(&buf->iov);
    qemu_sglist_destroy(&buf->sgl);
    g_free(buf->segments);
    QTAILQ_REMOVE(&ep->buffers, buf, next);
    g_free(buf);
}

// -- internal references --
// Firestorm$Inferno/18A5351d/sio.bndb@00009f14{armv8_timebase_get_current}
// -- end internal references --
static uint64_t apple_sio_get_cur_ts(const AppleSIO* s)
{ return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->gtimer_freq, NANOSECONDS_PER_SECOND); }

static void apple_sio_dma_del_buffers(AppleSIODMAEndpoint* ep)
{
    SIODMABuffer* buf;
    SIODMABuffer* buf_next;

    QTAILQ_FOREACH_SAFE (buf, &ep->buffers, next, buf_next) { apple_sio_dma_destroy_buffer(ep, buf); }
}

// -- internal references --
// Firestorm$Inferno/18A5351d/sio.bndb@000030e4{sio_endpoint::handle_message}+0x64
// -- end internal references --
static void apple_sio_dma_stop(AppleSIO* s, AppleSIODMAEndpoint* ep)
{
    SIODMABuffer* buf;
    SIODMABuffer* buf_next;
    SIOMessage    m   = {0};
    AppleRTKit*   rtk = &s->parent_obj;

    QTAILQ_FOREACH_SAFE (buf, &ep->buffers, next, buf_next) { apple_sio_dma_destroy_buffer(ep, buf); }

    m.op = OP_COMPLETE;
    m.ep = ep->id;
    apple_rtkit_send_user_msg(rtk, EP_CONTROL, m.raw);
}

static void apple_sio_dma_writeback(AppleSIO* s, AppleSIODMAEndpoint* ep, SIODMABuffer* buf)
{
    AppleRTKit* rtk = &s->parent_obj;
    SIOMessage  m   = {0};
    uint64_t    end_timestamp;
    dma_addr_t  resp_off;

    // -- internal references --
    // Firestorm$Inferno/18A5351d/sio.bndb@00002b0c
    // Firestorm$Inferno/18A5351d/sio.bndb@00001c08
    // -- end internal references --
    end_timestamp = apple_sio_get_cur_ts(s);
    resp_off      = ((buf->tag - 1) + (ep->id * 0x20)) * 0x10;
    stq_le_dma(&s->dma_as, s->resp_base + resp_off, buf->start_timestamp, MEMTXATTRS_UNSPECIFIED);
    stq_le_dma(&s->dma_as, s->resp_base + resp_off + 8, end_timestamp, MEMTXATTRS_UNSPECIFIED);

    m.op    = OP_COMPLETE;
    m.ep    = ep->id;
    m.param = BIT(7);
    m.tag   = buf->tag;
    m.data  = buf->completed;

    apple_sio_dma_destroy_buffer(ep, buf);

    apple_rtkit_send_user_msg(rtk, EP_CONTROL, m.raw);
}

static bool apple_sio_dma_map_buf(const AppleSIO* s, AppleSIODMAEndpoint* ep, SIODMABuffer* buf)
{
    if (buf->mapped) { return true; }

    buf->start_timestamp = apple_sio_get_cur_ts(s);

    qemu_iovec_init(&buf->iov, buf->segment_count);

    for (int i = 0; i < buf->segment_count; ++i) {
        dma_addr_t base = buf->sgl.sg[i].base;
        dma_addr_t len  = buf->sgl.sg[i].len;

        while (len != 0) {
            dma_addr_t xlen = len;
            void*      mem  = dma_memory_map(buf->sgl.as, base, &xlen, ep->direction, MEMTXATTRS_UNSPECIFIED);
            if (mem == NULL || xlen == 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: dma_memory_map failed; buf->tag=%d, "
                              "base=0x%" HWADDR_PRIX ", len=0x%" HWADDR_PRIX ", ep->direction=%d\n",
                              __func__, buf->tag, base, len, ep->direction);
                qemu_iovec_destroy(&buf->iov);
                return false;
            }

            qemu_iovec_add(&buf->iov, mem, xlen);
            len  -= xlen;
            base += xlen;
        }
    }

    buf->mapped = true;
    return true;
}

uint64_t apple_sio_dma_read(AppleSIODMAEndpoint* ep, void* buffer, uint64_t len)
{
    AppleSIO*     s;
    SIODMABuffer* buf;
    uint64_t      iovec_len;
    uint64_t      actual_len = 0;

    assert_cmpuint(ep->direction, ==, DMA_DIRECTION_TO_DEVICE);

    if (len == 0) { return 0; }

    QEMU_LOCK_GUARD(&ep->mutex);

    s = container_of(ep, AppleSIO, eps[ep->id]);

    while (len > actual_len) {
        buf = QTAILQ_FIRST(&ep->buffers);
        if (buf == NULL || !apple_sio_dma_map_buf(s, ep, buf)) { break; }
        iovec_len       = qemu_iovec_to_buf(&buf->iov, buf->completed, buffer + actual_len, len - actual_len);
        actual_len     += iovec_len;
        buf->completed += iovec_len;
        if (buf->completed >= buf->iov.size) { apple_sio_dma_writeback(s, ep, buf); }
    }

    return actual_len;
}

uint64_t apple_sio_dma_write(AppleSIODMAEndpoint* ep, void* buffer, uint64_t len)
{
    AppleSIO*     s;
    SIODMABuffer* buf;
    uint64_t      iovec_len;
    uint64_t      actual_len = 0;

    assert_cmpuint(ep->direction, ==, DMA_DIRECTION_FROM_DEVICE);

    if (len == 0) { return 0; }

    QEMU_LOCK_GUARD(&ep->mutex);

    s = container_of(ep, AppleSIO, eps[ep->id]);

    while (len > actual_len) {
        buf = QTAILQ_FIRST(&ep->buffers);
        if (buf == NULL || !apple_sio_dma_map_buf(s, ep, buf)) { break; }
        iovec_len       = qemu_iovec_from_buf(&buf->iov, buf->completed, buffer + actual_len, len - actual_len);
        actual_len     += iovec_len;
        buf->completed += iovec_len;
        if (buf->completed >= buf->iov.size) { apple_sio_dma_writeback(s, ep, buf); }
    }

    return actual_len;
}

uint64_t apple_sio_dma_blit(AppleSIODMAEndpoint* ep, int fillc, uint64_t len)
{
    AppleSIO*     s;
    SIODMABuffer* buf;
    uint64_t      iovec_len;
    uint64_t      actual_len = 0;

    assert_cmpuint(ep->direction, ==, DMA_DIRECTION_FROM_DEVICE);

    if (len == 0) { return 0; }

    QEMU_LOCK_GUARD(&ep->mutex);

    s = container_of(ep, AppleSIO, eps[ep->id]);

    while (len > actual_len) {
        buf = QTAILQ_FIRST(&ep->buffers);
        if (buf == NULL || !apple_sio_dma_map_buf(s, ep, buf)) { break; }
        iovec_len       = qemu_iovec_memset(&buf->iov, buf->completed, fillc, len - actual_len);
        actual_len     += iovec_len;
        buf->completed += iovec_len;
        if (buf->completed >= buf->iov.size) { apple_sio_dma_writeback(s, ep, buf); }
    }

    return actual_len;
}

static uint64_t apple_sio_dma_remaining_locked(AppleSIODMAEndpoint* ep)
{
    uint64_t      len;
    SIODMABuffer* buf;

    len = 0;

    QTAILQ_FOREACH (buf, &ep->buffers, next) {
        // using iov requires the buffer to be mapped.
        len += buf->sgl.size - buf->completed;
    }

    return len;
}

uint64_t apple_sio_dma_remaining(AppleSIODMAEndpoint* ep)
{
    QEMU_LOCK_GUARD(&ep->mutex);

    return apple_sio_dma_remaining_locked(ep);
}

static void apple_sio_control_get_param(AppleSIO* s, SIOMessage* reply, uint32_t param)
{
    if (param == PARAM_PROTOCOL_VERSION) {
        reply->data = s->protocol_version;
        reply->op   = OP_GET_PARAM_RESP;
    }
    else {
        reply->op = OP_SYNC_ERROR;
    }
}

static void apple_sio_control_set_param(AppleSIO* s, SIOMessage* reply, uint32_t param, uint32_t value)
{
    switch (param) {
        case PARAM_SEGMENT_BASE : s->segment_base = value << 12; break;
        case PARAM_SEGMENT_SIZE : s->segment_size = value; break;
        case PARAM_RESPONSE_BASE: s->resp_base = value << 12; break;
    }
    reply->op = OP_ACK;
}

static void apple_sio_control(AppleSIO* s, AppleSIODMAEndpoint* ep, SIOMessage* m)
{
    AppleRTKit* rtk   = &s->parent_obj;
    SIOMessage  reply = {0};

    QEMU_LOCK_GUARD(&ep->mutex);

    reply.ep  = m->ep;
    reply.tag = m->tag;

    switch (m->op) {
        case OP_GET_PARAM: apple_sio_control_get_param(s, &reply, m->param); break;
        case OP_SET_PARAM: apple_sio_control_set_param(s, &reply, m->param, m->data); break;
    }

    apple_rtkit_send_user_msg(rtk, EP_CONTROL, reply.raw);
};

static void apple_sio_dma(AppleSIO* s, AppleSIODMAEndpoint* ep, SIOMessage m)
{
    AppleRTKit*   rtk   = &s->parent_obj;
    SIOMessage    reply = {0};
    dma_addr_t    config_addr;
    dma_addr_t    segment_addr;
    uint32_t      segment_count;
    size_t        i;
    SIODMABuffer* buf;

    QEMU_LOCK_GUARD(&ep->mutex);

    reply.ep  = m.ep;
    reply.tag = m.tag;

    switch (m.op) {
        case OP_PING     : reply.op = OP_ACK; break;
        case OP_CONFIGURE: {
            config_addr = s->segment_base + (m.data * 0xC);
            if (dma_memory_read(&s->dma_as, config_addr, &ep->config, sizeof(ep->config), MEMTXATTRS_UNSPECIFIED)
                == MEMTX_OK)
            {
                reply.op = OP_ACK;
            }
            else {
                reply.op = OP_SYNC_ERROR;
            }
            break;
        }
        case OP_MAP: {
            // visual: if assertion hit, iOS behaviour changed.
            // -- internal references --
            // Firestorm$Inferno/18A5351d/sio.bndb@00003bc4
            // Firestorm$Inferno/18A5351d/kernelcache.research.iphone12b.bndb@fffffff0085d2b80
            // -- end internal references --
            assert_cmphex(m.param, ==, 0);

            segment_addr = s->segment_base + (m.data * 0xC);
            if (dma_memory_read(&s->dma_as, segment_addr + 0x3C, &segment_count, sizeof(segment_count),
                                MEMTXATTRS_UNSPECIFIED)
                != MEMTX_OK)
            {
                reply.op = OP_SYNC_ERROR;
                break;
            }

            buf           = g_new0(SIODMABuffer, 1);
            buf->segments = g_new(SIODMASegment, segment_count);
            if (dma_memory_read(&s->dma_as, segment_addr + 0x48, buf->segments, segment_count * sizeof(SIODMASegment),
                                MEMTXATTRS_UNSPECIFIED)
                != MEMTX_OK)
            {
                g_free(buf->segments);
                g_free(buf);
                reply.op = OP_SYNC_ERROR;
                break;
            }

            qemu_sglist_init(&buf->sgl, DEVICE(s), segment_count, &s->dma_as);
            buf->tag           = m.tag;
            buf->segment_count = segment_count;
            for (i = 0; i < segment_count; ++i) {
                qemu_sglist_add(&buf->sgl, buf->segments[i].addr, buf->segments[i].len);
            }
            QTAILQ_INSERT_TAIL(&ep->buffers, buf, next);

            reply.op = OP_ACK;
            break;
        }
        case OP_QUERY:
            buf = QTAILQ_FIRST(&ep->buffers);
            if (buf == NULL) {
                reply.op = OP_SYNC_ERROR;
                break;
            }

            reply.op    = OP_QUERY_OK;
            reply.param = buf->tag;
            reply.data  = buf->completed;
            break;
        case OP_STOP:
            reply.op = OP_ACK;
            apple_rtkit_send_user_msg(rtk, EP_CONTROL, reply.raw);
            apple_sio_dma_stop(s, ep);
            return;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: Unknown SIO op: %d\n", __func__, m.op);
            reply.op = OP_SYNC_ERROR;
            break;
    }
    apple_rtkit_send_user_msg(rtk, EP_CONTROL, reply.raw);
};

static void apple_sio_handle_endpoint(void* opaque, uint8_t ep, uint64_t msg)
{
    AppleSIO*  sio = opaque;
    SIOMessage m   = {0};

    m.raw = msg;

    SIO_LOG_MSG(ep, msg);

    switch (m.ep) {
        case EP_CONTROL:
        case EP_PERF   : apple_sio_control(sio, &sio->eps[EP_CONTROL], &m); break;
        default:
            if (m.ep >= SIO_NUM_EPS) { qemu_log_mask(LOG_UNIMP, "%s: Unknown ep %X\n", __func__, m.ep); }
            else {
                apple_sio_dma(sio, &sio->eps[m.ep], m);
            }
            break;
    }
}

AppleSIODMAEndpoint* apple_sio_get_endpoint(AppleSIO* s, int ep)
{
    if (ep <= EP_PERF || ep >= SIO_NUM_EPS) { return NULL; }

    return &s->eps[ep];
}

AppleSIODMAEndpoint* apple_sio_get_endpoint_from_node(AppleSIO* s, AppleDTNode* node, int idx)
{
    AppleDTProp* prop;
    uint32_t*    data;
    int          count;

    prop = apple_dt_get_prop(node, "dma-channels");
    if (prop == NULL) { return NULL; }

    count = prop->len / 32;
    if (idx >= count) { return NULL; }

    data = (uint32_t*)prop->data;

    return apple_sio_get_endpoint(s, data[8 * idx]);
}

static void ascv2_core_reg_write(void* opaque, hwaddr addr, uint64_t data, unsigned size) { }

static uint64_t ascv2_core_reg_read(void* opaque, hwaddr addr, unsigned size) { return 0; }

static const MemoryRegionOps ascv2_core_reg_ops = {
    .write                 = ascv2_core_reg_write,
    .read                  = ascv2_core_reg_read,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .valid.unaligned       = false,
};

static void apple_sio_realize(DeviceState* dev, Error** errp)
{
    AppleSIO*      s;
    AppleSIOClass* sioc;
    Object*        obj;

    s    = APPLE_SIO(dev);
    sioc = APPLE_SIO_GET_CLASS(dev);

    if (sioc->parent_realize != NULL) { sioc->parent_realize(dev, errp); }

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    s->dma_mr = MEMORY_REGION(obj);
    assert_nonnull(s->dma_mr);
    address_space_init(&s->dma_as, s->dma_mr, "sio.dma-as");

    for (int i = 0; i < SIO_NUM_EPS; ++i) {
        s->eps[i].id        = i;
        s->eps[i].direction = (i & 1) ? DMA_DIRECTION_FROM_DEVICE : DMA_DIRECTION_TO_DEVICE;
        qemu_mutex_init(&s->eps[i].mutex);
        QTAILQ_INIT(&s->eps[i].buffers);
    }
}

static void apple_sio_reset_hold(Object* obj, ResetType type)
{
    AppleSIO*      s;
    AppleSIOClass* sioc;

    s    = APPLE_SIO(obj);
    sioc = APPLE_SIO_GET_CLASS(obj);

    if (sioc->parent_reset.hold != NULL) { sioc->parent_reset.hold(obj, type); }

    s->segment_base = 0;
    s->segment_size = 0;

    for (size_t i = 0; i < SIO_NUM_EPS; ++i) {
        apple_sio_dma_del_buffers(&s->eps[i]);

        s->eps[i].config = (SIODMAConfig){0};
    }
}

static void apple_sio_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc;
    DeviceClass*     dc;
    AppleSIOClass*   sioc;

    rc   = RESETTABLE_CLASS(klass);
    dc   = DEVICE_CLASS(klass);
    sioc = APPLE_SIO_CLASS(klass);

    device_class_set_parent_realize(dc, apple_sio_realize, &sioc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, apple_sio_reset_hold, NULL, &sioc->parent_reset);
    dc->desc           = "Apple Smart IO DMA Controller";
    dc->user_creatable = false;
}

OBJECT_DEFINE_TYPE_CLASS_INIT(AppleSIO, apple_sio, APPLE_SIO, APPLE_RTKIT)

SysBusDevice* apple_sio_from_node(AppleDTNode* node, AppleA7IOPVersion version, uint32_t protocol_version,
                                  uint64_t gtimer_freq)
{
    DeviceState*  dev;
    AppleSIO*     s;
    SysBusDevice* sbd;
    AppleRTKit*   rtk;
    AppleDTNode*  child;
    AppleDTProp*  prop;
    uint64_t*     reg;

    assert_false(gtimer_freq == 0);

    dev = qdev_new(TYPE_APPLE_SIO);
    s   = APPLE_SIO(dev);
    sbd = SYS_BUS_DEVICE(dev);
    rtk = APPLE_RTKIT(dev);

    dev->id = g_strdup("sio");

    s->protocol_version = protocol_version;
    s->gtimer_freq      = gtimer_freq;

    child = apple_dt_get_node(node, "iop-sio-nub");
    assert_nonnull(child);

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t*)prop->data;

    apple_rtkit_init(rtk, NULL, "SIO", reg[1], version, NULL);
    apple_rtkit_register_user_ep(rtk, 0, s, apple_sio_handle_endpoint);

    memory_region_init_io(&s->ascv2_iomem, OBJECT(dev), &ascv2_core_reg_ops, s, TYPE_APPLE_SIO ".ascv2-core-reg",
                          reg[3]);
    sysbus_init_mmio(sbd, &s->ascv2_iomem);

    apple_dt_set_prop_u32(child, "pre-loaded", 1);
    apple_dt_set_prop_u32(child, "running", 1);

    return sbd;
}
