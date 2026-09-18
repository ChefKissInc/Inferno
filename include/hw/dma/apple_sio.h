#pragma once

#include "qemu/osdep.h"
#include "hw/arm/dt.h"
#include "hw/misc/a7iop/base.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_SIO "apple-sio"
OBJECT_DECLARE_TYPE(AppleSIO, AppleSIOClass, APPLE_SIO)

typedef struct AppleSIODMAEndpoint AppleSIODMAEndpoint;

uint64_t             apple_sio_dma_read(AppleSIODMAEndpoint* ep, void* buffer, uint64_t len);
uint64_t             apple_sio_dma_write(AppleSIODMAEndpoint* ep, void* buffer, uint64_t len);
uint64_t             apple_sio_dma_blit(AppleSIODMAEndpoint* ep, int fillc, uint64_t len);
uint64_t             apple_sio_dma_remaining(AppleSIODMAEndpoint* ep);
AppleSIODMAEndpoint* apple_sio_get_endpoint(AppleSIO* s, int ep);
AppleSIODMAEndpoint* apple_sio_get_endpoint_from_node(AppleSIO* s, AppleDTNode* node, int idx);
SysBusDevice*        apple_sio_from_node(AppleDTNode* node, AppleA7IOPVersion version, uint32_t protocol_version,
                                         uint64_t gtimer_freq);
