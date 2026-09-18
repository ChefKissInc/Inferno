/*
 * Apple SEP Private.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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
#include "qom/object.h"
#include "hw/arm/sep/emu.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/a7iop/core.h"
#include "cpu-qom.h"

#if 0
    #define HEXDUMP(a, b, c) qemu_hexdump(stderr, a, b, c)
    #define DPRINTF(v, ...)  fprintf(stderr, v, ##__VA_ARGS__)
#else
    #define HEXDUMP(a, b, c) \
        do { }               \
        while (0)
    #define DPRINTF(v, ...) \
        do { }              \
        while (0)
#endif

// #define ENABLE_CPU_DUMP_STATE

#define SEP_ENABLE_HARDCODED_FIRMWARE
// #define SEP_ENABLE_DEBUG_TRACE_MAPPING
// #define SEP_ENABLE_TRACE_BUFFER
// can cause conflicts with kernel and userspace, not anymore?
// #define SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS
// #define SEP_DISABLE_ASLR

#if defined(SEP_ENABLE_TRACE_BUFFER) || defined(SEP_DISABLE_ASLR)
    #define SEP_USE_VERSION_OVERRIDE 14
// #define SEP_USE_VERSION_OVERRIDE 15
// #define SEP_USE_VERSION_OVERRIDE 16
// #define SEP_USE_VERSION_OVERRIDE 17
// #define SEP_USE_VERSION_OVERRIDE 18
// #define SEP_USE_VERSION_OVERRIDE 26
#endif

/* aes.c */
#define TYPE_APPLE_SEP_AESS "apple-sep.aess"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPAESSState, APPLE_SEP_AESS)

AppleSEPAESSState* apple_sep_aess_create(AppleSEP* sep);

#define TYPE_APPLE_SEP_AESH "apple-sep.aesh"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPAESHState, APPLE_SEP_AESH)

AppleSEPAESHState* apple_sep_aesh_create(AppleSEP* sep);

#define TYPE_APPLE_SEP_AESC "apple-sep.aesc"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPAESCState, APPLE_SEP_AESC)

AppleSEPAESCState* apple_sep_aesc_create(void);

/* boot-monitor.c */
#define TYPE_APPLE_SEP_BOOT_MONITOR "apple-sep.boot-monitor"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPBootMonitorState, APPLE_SEP_BOOT_MONITOR)

void apple_sep_boot_monitor_jump(AppleSEPBootMonitorState* s);

AppleSEPBootMonitorState* apple_sep_boot_monitor_create(AppleSEP* sep);

/* debug-trace.c */
#ifdef ENABLE_CPU_DUMP_STATE
void apple_sep_dump_cpu_handler(void);
#else
    #define apple_sep_dump_cpu_handler() (void)0
#endif
#ifdef SEP_ENABLE_TRACE_BUFFER
    #define TYPE_APPLE_SEP_DEBUG_TRACE "apple-sep.debug-trace"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPDebugTraceState, APPLE_SEP_DEBUG_TRACE)

void                     apple_sep_debug_trace_enable(AppleSEPDebugTraceState* s);
void                     apple_sep_debug_trace_set_region(AppleSEPDebugTraceState* s, hwaddr offset, hwaddr size);
AppleSEPDebugTraceState* apple_sep_debug_trace_create(AppleSEP* sep);
#endif

/* eisp.c */
#define TYPE_APPLE_SEP_EISP "apple-sep.eisp"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPEISPState, APPLE_SEP_EISP)

AppleSEPEISPState* apple_sep_eisp_create(void);

/* key.c */
#define TYPE_APPLE_SEP_KEY "apple-sep.key"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPKeyState, APPLE_SEP_KEY)

AppleSEPKeyState* apple_sep_key_create(AppleSEP* sep);

/* misc.c */
#define TYPE_APPLE_SEP_MISC "apple-sep.misc"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPMiscState, APPLE_SEP_MISC)

AppleSEPMiscState* apple_sep_misc_create(void);

/* monitor.c */
#define TYPE_APPLE_SEP_MONITOR "apple-sep.monitor"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPMonitorState, APPLE_SEP_MONITOR)

AppleSEPMonitorState* apple_sep_monitor_create(void);

/* ssc.c */
#define TYPE_APPLE_SEP_SSC "apple-sep.ssc"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPSSCState, APPLE_SEP_SSC)

AppleSEPSSCState* apple_sep_ssc_create(AppleI2CState* i2c, uint8_t addr, AppleSEP* sep);

/* pka.c */
#define TYPE_APPLE_SEP_PKA "apple-sep.pka"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPPKAState, APPLE_SEP_PKA)

AppleSEPPKAState* apple_sep_pka_create(AppleSEP* sep);

/* pmgr.c */
#define TYPE_APPLE_SEP_PMGR "apple-sep.pmgr"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPPMGRState, APPLE_SEP_PMGR)

AppleSEPPMGRState* apple_sep_pmgr_create(AppleSEP* sep);
bool               apple_sep_pmgr_get_fuse_changer_bit(AppleSEPPMGRState* s, uint8_t bit);

/* progress.c */
#define TYPE_APPLE_SEP_PROGRESS "apple-sep.progress"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPProgressState, APPLE_SEP_PROGRESS)

AppleSEPProgressState* apple_sep_progress_create(AppleSEP* sep);

/* trng.c */
#define TYPE_APPLE_SEP_TRNG "apple-sep.trng"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSEPTRNGState, APPLE_SEP_TRNG)

AppleSEPTRNGState* apple_sep_trng_create(AppleSEP* sep);

struct AppleSEP
{
    /*< private >*/
    AppleA7IOP parent_obj;

    /*< public >*/
    vaddr                     base;
    ARMCPU*                   cpu;
    bool                      modern;
    MemoryRegion*             ool_mr;
    AddressSpace*             ool_as;
    MemoryRegion              tz0_win_r2;
    MemoryRegion              tz0_win_r3;
    MemoryRegion              tz0_mirror_r2;
    MemoryRegion              tz0_mirror_r3;
    bool                      tz0_wins_inited;
    IOMMUNotifier             dma_notifier;
    GArray*                   dma_pending;
    GPtrArray*                dma_windows;
    GHashTable*               dma_shadow_pages;
    QEMUBH*                   dma_rebuild_bh;
    bool                      dma_refreshing;
    QEMUTimer*                timer;
    I2CSlave*                 nvram;
    hwaddr                    sep_fw_addr;
    gsize                     sep_fw_size;
    uint32_t                  chip_id;
    hwaddr                    shmbuf_base;
    gchar*                    fw_data;
    AppleA7IOPMailbox*        mailbox;
    AppleSEPAESSState*        aess;
    AppleSEPAESHState*        aesh;
    AppleSEPAESCState*        aesc;
    AppleSEPBootMonitorState* boot_monitor;
#ifdef SEP_ENABLE_TRACE_BUFFER
    AppleSEPDebugTraceState* debug_trace;
#endif
    AppleSEPEISPState*     eisp;
    AppleSEPKeyState*      key;
    AppleSEPMiscState*     misc;
    AppleSEPMonitorState*  monitor;
    AppleSEPPKAState*      pka;
    AppleSEPPMGRState*     pmgr;
    AppleSEPProgressState* progress;
    AppleSEPTRNGState*     trng;
};
