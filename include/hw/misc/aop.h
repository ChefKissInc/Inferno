/*
 * Apple Always-On Processor.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/dt.h"
#include "hw/misc/a7iop/base.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_AOP "apple-aop"
OBJECT_DECLARE_TYPE(AppleAOP, AppleAOPClass, APPLE_AOP)

typedef struct AppleAOPEndpoint AppleAOPEndpoint;

typedef enum
{
    AOP_EP_TYPE_HID,
    AOP_EP_TYPE_MUX,
    AOP_EP_TYPE_APP,
} AppleAOPEndpointType;

typedef enum
{
    AOP_RESULT_OK,
    AOP_RESULT_TIMEOUT,
    AOP_RESULT_ABORTED,
    AOP_RESULT_ERROR,
} AppleAOPResult;

typedef struct
{
    AppleAOPEndpointType type;
    const char*          service_name;
    uint32_t             service_id;
    uint32_t             interface_num;
    uint32_t             rx_len;
    uint32_t             tx_len;
    AppleAOPResult       (*get_property)(void* opaque, uint32_t property, void* data);
    AppleAOPResult       (*handle_command)(void* opaque, uint16_t seq, void* payload, uint32_t len, void* payload_out,
                                           uint32_t out_len);
} AppleAOPEndpointDescription;

SysBusDevice*     apple_aop_create(AppleDTNode* node, AppleA7IOPVersion version);
AppleAOPEndpoint* apple_aop_ep_create(AppleAOP* s, void* opaque, const AppleAOPEndpointDescription* descr);
/// NOTE: Must be used while state is locked.
MemTxResult apple_aop_ep_send_report_locked(AppleAOPEndpoint* s, uint16_t packet_type, const void* payload,
                                            uint32_t payload_len, uint32_t out_len);
MemTxResult apple_aop_ep_send_report(AppleAOPEndpoint* s, uint16_t packet_type, const void* payload,
                                     uint32_t payload_len, uint32_t out_len);
/// NOTE: Must be used while state is locked.
MemTxResult apple_aop_ep_send_reply_locked(AppleAOPEndpoint* s, uint16_t packet_type, uint16_t seq, const void* payload,
                                           uint32_t payload_len, uint32_t out_len);
MemTxResult apple_aop_ep_send_reply(AppleAOPEndpoint* s, uint16_t packet_type, uint16_t seq, const void* payload,
                                    uint32_t payload_len, uint32_t out_len);
