/*
 * Apple RTKit.
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
#include "hw/misc/a7iop/private.h"
#include "hw/misc/a7iop/rtkit.h"
#include "qemu/lockable.h"
#include "trace.h"

#define EP_MANAGEMENT (0)
#define EP_CRASHLOG   (1)

#define EP0_IDLE          (0)
#define EP0_WAIT_HELLO    (1)
#define EP0_WAIT_ROLLCALL (2)
#define EP0_DONE          (3)

#define MSG_HELLO              (1)
#define MSG_HELLO_ACK          (2)
#define MSG_TYPE_PING          (3)
#define MSG_TYPE_PING_ACK      (4)
#define MSG_TYPE_SET_EP_STATUS (5)
#define MSG_TYPE_REQ_POWER     (6)
#define MSG_GET_PSTATE(_x)     ((_x) & 0xFFF)    // TODO: Investigate this
#define PSTATE_PWRGATE         (0x0)
#define PSTATE_SLEEP           (0x201)
#define PSTATE_OFF             (0x202)
#define PSTATE_ON              (0x220)
#define MSG_TYPE_POWER_ACK     (7)
#define MSG_TYPE_ROLLCALL      (8)
#define MSG_TYPE_POWER_NAP     (10)
#define MSG_TYPE_AP_POWER      (11)

typedef struct
{
    uint64_t msg;
    uint32_t endpoint;
    uint32_t flags;
} QEMU_PACKED AppleRTKitMessage;

typedef union
{
    union
    {
        struct
        {
            uint16_t min_version;
            uint16_t max_version;
        } hello;
        struct
        {
            uint16_t min_version;
            uint16_t max_version;
            union
            {
                struct
                {
                    uint32_t debug_en   : 1;
                    uint32_t tracing_en : 1;
                };
                uint32_t raw;
            } flags;
        } hello_ack;
        struct
        {
            uint32_t seg;
            uint16_t timestamp;
        } ping;
        struct
        {
            uint32_t state;
            uint32_t ep;
        } epstart;
        struct
        { uint32_t state; } power;
        struct
        {
            uint32_t mask;
            uint32_t block : 6;
            uint32_t _rsvd : 13;
            uint32_t end   : 1;
        } rollcall_v11;
        struct
        { uint64_t mask : 52; } rollcall_v10;
    };
    struct
    {
        uint64_t _rsvd : 52;
        uint64_t type  : 4;
    };
    uint64_t raw;
} AppleRTKitManagementMessage;
QEMU_BUILD_BUG_ON(sizeof(AppleRTKitManagementMessage) != sizeof(uint64_t));

enum
{
    RTKIT_MIN_VERSION           = 3,
    RTKIT_MAX_VERSION_PRE_IOS14 = 10,
    RTKIT_MAX_VERSION           = 12,
};

static void apple_rtkit_construct_msg(AppleA7IOPMessage* message, const uint8_t ep, const uint64_t data)
{
    AppleRTKitMessage* rtk_message;

    rtk_message           = (AppleRTKitMessage*)message->data;
    rtk_message->endpoint = ep;
    rtk_message->msg      = data;
}

static void apple_rtkit_send_msg(AppleRTKit* s, uint8_t ep, uint64_t data)
{
    AppleA7IOPMessage message;

    apple_rtkit_construct_msg(&message, ep, data);
    apple_a7iop_send_ap(&s->parent_obj, &message);
}

void apple_rtkit_send_control_msg(AppleRTKit* s, uint8_t ep, uint64_t data)
{
    assert_cmpuint(ep, <, RTKIT_EP_USER_START);
    apple_rtkit_send_msg(s, ep, data);
}

void apple_rtkit_send_user_msg(AppleRTKit* s, uint8_t ep, uint64_t data)
{
    assert_cmpuint(ep, <, 256 - RTKIT_EP_USER_START);
    apple_rtkit_send_msg(s, ep + RTKIT_EP_USER_START, data);
}

static void apple_rtkit_register_ep(AppleRTKit* s, uint8_t ep, void* opaque, AppleRTKitEPHandler* handler, bool user)
{
    assert_null(AppleRTKitEPTable_get(s->endpoints, ep));

    AppleRTKitEPTable_set_at(s->endpoints, ep,
                             (AppleRTKitEPData){
                                 .opaque  = opaque,
                                 .handler = handler,
                                 .user    = user,
                             });
}

void apple_rtkit_register_control_ep(AppleRTKit* s, uint8_t ep, void* opaque, AppleRTKitEPHandler* handler)
{
    assert_cmpuint(ep, <, RTKIT_EP_USER_START);
    apple_rtkit_register_ep(s, ep, opaque, handler, false);
}

void apple_rtkit_register_user_ep(AppleRTKit* s, uint8_t ep, void* opaque, AppleRTKitEPHandler* handler)
{
    assert_cmpuint(ep, <, 256 - RTKIT_EP_USER_START);
    apple_rtkit_register_ep(s, ep + RTKIT_EP_USER_START, opaque, handler, true);
}

static void apple_rtkit_unregister_ep(AppleRTKit* s, uint8_t ep) { AppleRTKitEPTable_erase(s->endpoints, ep); }

void apple_rtkit_unregister_control_ep(AppleRTKit* s, uint8_t ep)
{
    assert_cmpuint(ep, <, RTKIT_EP_USER_START);
    apple_rtkit_unregister_ep(s, ep);
}

void apple_rtkit_unregister_user_ep(AppleRTKit* s, uint8_t ep)
{
    assert_cmpuint(ep, <, 256 - RTKIT_EP_USER_START);
    apple_rtkit_unregister_ep(s, ep + RTKIT_EP_USER_START);
}

static void apple_rtkit_mgmt_send_hello_msg(AppleRTKit* s, uint16_t min_version, uint16_t max_version)
{
    AppleRTKitManagementMessage msg = {0};

    trace_apple_rtkit_mgmt_send_hello(s->parent_obj.role);

    msg.type              = MSG_HELLO;
    msg.hello.min_version = min_version;
    msg.hello.max_version = max_version;
    apple_rtkit_send_control_msg(s, EP_MANAGEMENT, msg.raw);
}

static void apple_rtkit_mgmt_send_hello(AppleRTKit* s)
{
    apple_rtkit_mgmt_send_hello_msg(s, RTKIT_MIN_VERSION, RTKIT_MAX_VERSION);
    s->ep0_status = EP0_WAIT_HELLO;
}

static void apple_rtkit_mgmt_rollcall_v10(AppleRTKit* s)
{
    AppleRTKitManagementMessage      msg = {0};
    AppleRTKitEPTable_it_t           it;
    const AppleRTKitEPTable_pair_ct* cref;

    msg.type = MSG_TYPE_ROLLCALL;
    for (AppleRTKitEPTable_it(it, s->endpoints); !AppleRTKitEPTable_end_p(it); AppleRTKitEPTable_next(it)) {
        cref                   = AppleRTKitEPTable_cref(it);
        msg.rollcall_v10.mask |= BIT_ULL(cref->key);
    }
    apple_rtkit_send_control_msg(s, EP_MANAGEMENT, msg.raw);
}

static void apple_rtkit_mgmt_reset_rollcall(AppleRTKit* s)
{
    s->rollcall_head  = 0;
    s->rollcall_count = 0;
}

static void apple_rtkit_mgmt_rollcall_v11(AppleRTKit* s)
{
    AppleA7IOP*                      a7iop    = &s->parent_obj;
    AppleRTKitManagementMessage      mgmt_msg = {0};
    AppleRTKitEPTable_it_t           it;
    const AppleRTKitEPTable_pair_ct* cref;
    uint32_t                         masks[RTKIT_ROLLCALL_BLOCKS] = {0};
    uint32_t                         ep, block, last_block = 0;

    assert_cmphex(s->rollcall_head, ==, 0);
    assert_cmphex(s->rollcall_count, ==, 0);

    for (AppleRTKitEPTable_it(it, s->endpoints); !AppleRTKitEPTable_end_p(it); AppleRTKitEPTable_next(it)) {
        cref = AppleRTKitEPTable_cref(it);
        ep   = cref->key;

        masks[ep / RTKIT_EP_USER_START] |= BIT(ep % RTKIT_EP_USER_START);
    }

    for (block = 0; block < RTKIT_ROLLCALL_BLOCKS; block++) {
        if (masks[block] != 0) { last_block = block; }
    }

    for (block = 0; block <= last_block; block++) {
        if (masks[block] == 0) { continue; }

        mgmt_msg.type               = MSG_TYPE_ROLLCALL;
        mgmt_msg.rollcall_v11.mask  = masks[block];
        mgmt_msg.rollcall_v11.block = block;
        mgmt_msg.rollcall_v11.end   = block == last_block;
        apple_rtkit_construct_msg(&s->rollcall[s->rollcall_count++], EP_MANAGEMENT, mgmt_msg.raw);
    }

    apple_a7iop_send_ap(a7iop, &s->rollcall[s->rollcall_head++]);
}

static void apple_rtkit_mgmt_handle_msg(void* opaque, uint8_t ep, uint64_t message)
{
    AppleRTKit*                        s     = opaque;
    AppleA7IOP*                        a7iop = opaque;
    const AppleRTKitManagementMessage* msg;
    AppleRTKitManagementMessage        m = {0};

    msg = (const AppleRTKitManagementMessage*)&message;

    trace_apple_rtkit_handle_mgmt_msg(a7iop->role, msg->raw, s->ep0_status, msg->type);

    switch (msg->type) {
        case MSG_HELLO_ACK:
            assert_cmphex(s->ep0_status, ==, EP0_WAIT_HELLO);

            // We must resend hello on iOS <=13 with a lower version range.
            // FIXME: This doesn't work consistently;
            // iOS sometimes panics about the mismatch due to a race condition.
            // Good job Apple! Now we'll have to read the version from the fw.
            if (msg->hello_ack.min_version == 0xFFFF && msg->hello_ack.max_version == 0xBAD) {
                apple_rtkit_mgmt_send_hello_msg(s, RTKIT_MIN_VERSION, RTKIT_MAX_VERSION_PRE_IOS14);
                break;
            }

            s->protocol_version = msg->hello_ack.max_version;

            if (s->protocol_version <= 10) { apple_rtkit_mgmt_rollcall_v10(s); }
            else {
                apple_rtkit_mgmt_rollcall_v11(s);
            }

            s->ep0_status = EP0_WAIT_ROLLCALL;
            break;
        case MSG_TYPE_PING:
            m.type           = MSG_TYPE_PING_ACK;
            m.ping.seg       = msg->ping.seg;
            m.ping.timestamp = msg->ping.timestamp;
            apple_rtkit_send_msg(s, ep, m.raw);
            break;
        case MSG_TYPE_AP_POWER:
            m.type        = MSG_TYPE_AP_POWER;
            m.power.state = msg->power.state;
            apple_rtkit_send_msg(s, ep, m.raw);
            break;
        case MSG_TYPE_REQ_POWER:
            switch (MSG_GET_PSTATE(msg->raw)) {
                case PSTATE_ON   : apple_a7iop_cpu_start(a7iop, true); break;
                case PSTATE_SLEEP:
                case PSTATE_OFF  :
                case PSTATE_PWRGATE:
                    apple_a7iop_set_cpu_status(a7iop, apple_a7iop_get_cpu_status(a7iop) | CPU_STATUS_IDLE);
                    m.type        = MSG_TYPE_POWER_ACK;
                    m.power.state = MSG_GET_PSTATE(msg->raw);
                    apple_rtkit_send_msg(s, ep, m.raw);
                    break;
                default: break;
            }
            break;
        case MSG_TYPE_ROLLCALL:
            assert_cmphex(s->ep0_status, ==, EP0_WAIT_ROLLCALL);

            if (s->rollcall_head >= s->rollcall_count) {
                apple_rtkit_mgmt_reset_rollcall(s);

                m.type        = MSG_TYPE_POWER_ACK;
                m.power.state = 32;
                s->ep0_status = EP0_IDLE;
                trace_apple_rtkit_rollcall_finished(a7iop->role);
                apple_rtkit_send_msg(s, ep, m.raw);

                if (s->ops != NULL && s->ops->boot_done != NULL) { s->ops->boot_done(s->opaque); }
            }
            else {
                apple_a7iop_send_ap(a7iop, &s->rollcall[s->rollcall_head++]);
            }
            break;
        default: break;
    }
}

static void apple_rtkit_iop_start(AppleA7IOP* s)
{
    AppleRTKit* rtk = container_of(s, AppleRTKit, parent_obj);

    trace_apple_rtkit_iop_start(s->role);

    if (rtk->ops && rtk->ops->start) { rtk->ops->start(rtk->opaque); }

    apple_rtkit_mgmt_send_hello(rtk);
}

static void apple_rtkit_iop_wakeup(AppleA7IOP* s)
{
    AppleRTKit* rtk = container_of(s, AppleRTKit, parent_obj);

    trace_apple_rtkit_iop_wakeup(s->role);

    if (rtk->ops && rtk->ops->wakeup) { rtk->ops->wakeup(rtk->opaque); }

    apple_rtkit_mgmt_send_hello(rtk);
}

static void apple_rtkit_handle_messages_bh(void* opaque)
{
    AppleRTKit*        s     = opaque;
    AppleA7IOP*        a7iop = opaque;
    AppleRTKitEPData*  data;
    AppleA7IOPMessage  message;
    AppleRTKitMessage* rtk_msg;

    QEMU_LOCK_GUARD(&s->lock);

    while (!apple_a7iop_mailbox_is_empty(a7iop->iop_mailbox)) {
        if (!apple_a7iop_recv_iop(a7iop, &message)) { break; }
        rtk_msg = (AppleRTKitMessage*)message.data;
        data    = AppleRTKitEPTable_get(s->endpoints, rtk_msg->endpoint);
        if (data != NULL && data->handler != NULL) {
            data->handler(data->opaque, data->user ? rtk_msg->endpoint - RTKIT_EP_USER_START : rtk_msg->endpoint,
                          rtk_msg->msg);
        }
    }
}

static const AppleA7IOPOps apple_rtkit_iop_ops = {
    .start  = apple_rtkit_iop_start,
    .wakeup = apple_rtkit_iop_wakeup,
};

void apple_rtkit_init(AppleRTKit* s, void* opaque, const char* role, uint64_t mmio_size, AppleA7IOPVersion version,
                      const AppleRTKitOps* ops)
{
    AppleA7IOP* a7iop = &s->parent_obj;

    apple_a7iop_init(a7iop, role, mmio_size, version, &apple_rtkit_iop_ops, apple_rtkit_handle_messages_bh);

    s->opaque = opaque ? opaque : s;
    AppleRTKitEPTable_init(s->endpoints);
    s->ops = ops;
    qemu_mutex_init(&s->lock);

    apple_rtkit_register_control_ep(s, EP_MANAGEMENT, s, apple_rtkit_mgmt_handle_msg);
    apple_rtkit_register_control_ep(s, EP_CRASHLOG, s, NULL);
}

AppleRTKit* apple_rtkit_new(void* opaque, const char* role, uint64_t mmio_size, AppleA7IOPVersion version,
                            const AppleRTKitOps* ops)
{
    AppleRTKit* s;

    s = APPLE_RTKIT(qdev_new(TYPE_APPLE_RTKIT));

    apple_rtkit_init(s, opaque, role, mmio_size, version, ops);

    return s;
}

static void apple_rtkit_reset_enter(Object* obj, ResetType type)
{
    AppleRTKit*      s;
    AppleRTKitClass* rtkc;

    s    = APPLE_RTKIT(obj);
    rtkc = APPLE_RTKIT_GET_CLASS(obj);

    if (rtkc->parent_phases.enter != NULL) { rtkc->parent_phases.enter(obj, type); }

    QEMU_LOCK_GUARD(&s->lock);

    s->ep0_status       = EP0_IDLE;
    s->protocol_version = 0;
    apple_rtkit_mgmt_reset_rollcall(s);
}

static void apple_rtkit_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc   = RESETTABLE_CLASS(klass);
    DeviceClass*     dc   = DEVICE_CLASS(klass);
    AppleRTKitClass* rtkc = APPLE_RTKIT_CLASS(klass);

    resettable_class_set_parent_phases(rc, apple_rtkit_reset_enter, NULL, NULL, &rtkc->parent_phases);

    dc->desc = "Apple RTKit IOP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

OBJECT_DEFINE_TYPE_CLASS_INIT(AppleRTKit, apple_rtkit, APPLE_RTKIT, APPLE_A7IOP)
