/*
 * ARM virtual CPU header
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "kvm-consts.h"
#include "qemu/cpu-float.h"
#include "hw/registerfields.h"
#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-interrupt.h"
#include "exec/gdbstub.h"
#include "exec/page-protection.h"
#include "qapi/qapi-types-common.h"
#include "target/arm/multiprocessing.h"
#include "target/arm/gtimer.h"
#include "target/arm/cpu-sysregs.h"
#include "target/arm/cpregs.h"
#include "target/arm/mmuidx.h"
#include "exec/cputlb.h"

/* Maximum number of page invalidations deferred to the next DSB. */
#define ARM_TLBI_BATCH_MAX (64)

#define EXCP_UDEF           1 /* undefined instruction */
#define EXCP_SWI            2 /* software interrupt */
#define EXCP_PREFETCH_ABORT 3
#define EXCP_DATA_ABORT     4
#define EXCP_IRQ            5
#define EXCP_FIQ            6
#define EXCP_BKPT           7
#define EXCP_KERNEL_TRAP    8 /* Jumped to kernel code page.  */
#define EXCP_HVC            9 /* HyperVisor Call */
#define EXCP_HYP_TRAP       10
#define EXCP_SMC            11 /* Secure Monitor Call */
#define EXCP_VIRQ           12
#define EXCP_VFIQ           13
#define EXCP_VSERR          14
#define EXCP_GPC            15 /* v9 Granule Protection Check Fault */
#define EXCP_NMI            16
#define EXCP_VINMI          17
#define EXCP_VFNMI          18
#define EXCP_MON_TRAP       19 /* AArch32 trap to Monitor mode */
#define EXCP_GENTER         20
#define EXCP_GXF_ABORT      21
/* NB: add new EXCP_ defines to the array in arm_log_exception() too */

/* ARM-specific interrupt pending bits.  */
#define CPU_INTERRUPT_FIQ   CPU_INTERRUPT_TGT_EXT_1
#define CPU_INTERRUPT_VIRQ  CPU_INTERRUPT_TGT_EXT_2
#define CPU_INTERRUPT_VFIQ  CPU_INTERRUPT_TGT_EXT_3
#define CPU_INTERRUPT_VSERR CPU_INTERRUPT_TGT_INT_0
#define CPU_INTERRUPT_NMI   CPU_INTERRUPT_TGT_EXT_4
#define CPU_INTERRUPT_VINMI CPU_INTERRUPT_TGT_EXT_0
#define CPU_INTERRUPT_VFNMI CPU_INTERRUPT_TGT_INT_1

/* The usual mapping for an AArch64 system register to its AArch32
 * counterpart is for the 32 bit world to have access to the lower
 * half only (with writes leaving the upper half untouched). It's
 * therefore useful to be able to pass TCG the offset of the least
 * significant half of a uint64_t struct member.
 */
#if HOST_BIG_ENDIAN
    #define offsetoflow32(S, M)  (offsetof(S, M) + sizeof(uint32_t))
    #define offsetofhigh32(S, M) offsetof(S, M)
#else
    #define offsetoflow32(S, M)  offsetof(S, M)
    #define offsetofhigh32(S, M) (offsetof(S, M) + sizeof(uint32_t))
#endif

/* The 2nd extra word holding syndrome info for data aborts does not use
 * the upper 6 bits nor the lower 13 bits. We mask and shift it down to
 * help the sleb128 encoder do a better job.
 * When restoring the CPU state, we shift it back up.
 */
#define ARM_INSN_START_WORD2_MASK  ((1 << 26) - 1)
#define ARM_INSN_START_WORD2_SHIFT 13

/* We currently assume float and double are IEEE single and double
   precision respectively.
   Doing runtime conversions is tricky because VFP registers may contain
   integer values (eg. as the result of a FTOSI instruction).
   s<2n> maps to the least significant half of d<n>
   s<2n+1> maps to the most significant half of d<n>
 */

/**
 * DynamicGDBFeatureInfo:
 * @desc: Contains the feature descriptions.
 * @data: A union with data specific to the set of registers
 *    @cpregs_keys: Array that contains the corresponding Key of
 *                  a given cpreg with the same order of the cpreg
 *                  in the XML description.
 */
typedef struct DynamicGDBFeatureInfo
{
    GDBFeature desc;
    union
    {
        struct
        { uint32_t* keys; } cpregs;
    } data;
} DynamicGDBFeatureInfo;

/* CPU state for each instance of a generic timer (in cp15 c14) */
typedef struct ARMGenericTimer
{
    uint64_t cval; /* Timer CompareValue register */
    uint64_t ctl;  /* Timer Control register */
} ARMGenericTimer;

/* Define a maximum sized vector register.
 * For 32-bit, this is a 128-bit NEON/AdvSIMD register.
 * For 64-bit, this is a 2048-bit SVE register.
 *
 * Note that the mapping between S, D, and Q views of the register bank
 * differs between AArch64 and AArch32.
 * In AArch32:
 *  Qn = regs[n].d[1]:regs[n].d[0]
 *  Dn = regs[n / 2].d[n & 1]
 *  Sn = regs[n / 4].d[n % 4 / 2],
 *       bits 31..0 for even n, and bits 63..32 for odd n
 *       (and regs[16] to regs[31] are inaccessible)
 * In AArch64:
 *  Zn = regs[n].d[*]
 *  Qn = regs[n].d[1]:regs[n].d[0]
 *  Dn = regs[n].d[0]
 *  Sn = regs[n].d[0] bits 31..0
 *  Hn = regs[n].d[0] bits 15..0
 *
 * This corresponds to the architecturally defined mapping between
 * the two execution states, and means we do not need to explicitly
 * map these registers when changing states.
 *
 * Align the data for use with TCG host vector operations.
 */

#define ARM_MAX_VQ 16

typedef struct ARMVectorReg
{
    uint64_t d[2 * ARM_MAX_VQ] QEMU_ALIGNED(16);
} ARMVectorReg;

/* In AArch32 mode, predicate registers do not exist at all.  */
typedef struct ARMPredicateReg
{
    uint64_t p[DIV_ROUND_UP(2 * ARM_MAX_VQ, 8)] QEMU_ALIGNED(16);
} ARMPredicateReg;

/* In AArch32 mode, PAC keys do not exist at all.  */
typedef struct ARMPACKey
{
    uint64_t lo, hi;
} ARMPACKey;

/* See the commentary above the TBFLAG field definitions.  */
typedef struct CPUARMTBFlags
{
    uint32_t flags;
    uint64_t flags2;
} CPUARMTBFlags;

typedef struct ARMMMUFaultInfo ARMMMUFaultInfo;

typedef struct NVICState NVICState;

/*
 * Enum for indexing vfp.fp_status[].
 *
 * FPST_A32: is the "normal" fp status for AArch32 insns
 * FPST_A64: is the "normal" fp status for AArch64 insns
 * FPST_A32_F16: used for AArch32 half-precision calculations
 * FPST_A64_F16: used for AArch64 half-precision calculations
 * FPST_STD: the ARM "Standard FPSCR Value"
 * FPST_STD_F16: used for half-precision
 *       calculations with the ARM "Standard FPSCR Value"
 * FPST_AH: used for the A64 insns which change behaviour
 *       when FPCR.AH == 1 (bfloat16 conversions and multiplies,
 *       and the reciprocal and square root estimate/step insns)
 * FPST_AH_F16: used for the A64 insns which change behaviour
 *       when FPCR.AH == 1 (bfloat16 conversions and multiplies,
 *       and the reciprocal and square root estimate/step insns);
 *       for half-precision
 * ZA: the "streaming sve" fp status.
 * ZA_F16: likewise for half-precision.
 *
 * Half-precision operations are governed by a separate
 * flush-to-zero control bit in FPSCR:FZ16. We pass a separate
 * status structure to control this.
 *
 * The "Standard FPSCR", ie default-NaN, flush-to-zero,
 * round-to-nearest and is used by any operations (generally
 * Neon) which the architecture defines as controlled by the
 * standard FPSCR value rather than the FPSCR.
 *
 * The "standard FPSCR but for fp16 ops" is needed because
 * the "standard FPSCR" tracks the FPSCR.FZ16 bit rather than
 * using a fixed value for it.
 *
 * FPST_AH is needed because some insns have different
 * behaviour when FPCR.AH == 1: they don't update cumulative
 * exception flags, they act like FPCR.{FZ,FIZ} = {1,1} and
 * they ignore FPCR.RMode. But they don't ignore FPCR.FZ16,
 * which means we need an FPST_AH_F16 as well.
 *
 * The "ZA" float_status are for Streaming SVE operations which use
 * default-NaN and do not generate fp exceptions, which means that they
 * do not accumulate exception bits back into FPCR.
 * See e.g. FPAdd vs FPAdd_ZA pseudocode functions, and the setting
 * of fpcr.DN and fpexec parameters.
 *
 * To avoid having to transfer exception bits around, we simply
 * say that the FPSCR cumulative exception flags are the logical
 * OR of the flags in the four fp statuses. This relies on the
 * only thing which needs to read the exception flags being
 * an explicit FPSCR read.
 */
typedef enum ARMFPStatusFlavour
{
    FPST_A32,
    FPST_A64,
    FPST_A32_F16,
    FPST_A64_F16,
    FPST_AH,
    FPST_AH_F16,
    FPST_ZA,
    FPST_ZA_F16,
    FPST_STD,
    FPST_STD_F16,
} ARMFPStatusFlavour;
#define FPST_COUNT 10

#define APRR_ATTR_X (1ULL)
#define APRR_ATTR_W (2ULL)
#define APRR_ATTR_R (4ULL)

#define APRR_ATTR_WX  (APRR_ATTR_W | APRR_ATTR_X)
#define APRR_ATTR_RX  (APRR_ATTR_R | APRR_ATTR_X)
#define APRR_ATTR_RWX (APRR_ATTR_R | APRR_ATTR_W | APRR_ATTR_X)

#define APRR_ATTR_NONE (0ULL)
#define APRR_ATTR_MASK (APRR_ATTR_RWX)

#define SPRR_ATTR_MASK (15ULL)

#define APRR_SHIFT_FOR_IDX(x) ((x) << 2ULL)

#define APRR_EXTRACT_IDX_ATTR(_aprr_value, _idx) (((_aprr_value) >> APRR_SHIFT_FOR_IDX(_idx)) & APRR_ATTR_MASK)

#define SPRR_SHIFT_FOR_IDX(x) ((x) << 2ULL)

#define SPRR_EXTRACT_IDX_ATTR(_sprr_value, _idx) (((_sprr_value) >> SPRR_SHIFT_FOR_IDX(_idx)) & SPRR_ATTR_MASK)

#define SPRR_MASK_MASK (3ULL)

#define SPRR_MASK_SHIFT_FOR_IDX(x) ((x) << 1ULL)

#define SPRR_MASK_EXTRACT_IDX_ATTR(_sprr_mask_value, _idx)                   \
    (((_sprr_mask_value) >> SPRR_MASK_SHIFT_FOR_IDX(_idx)) & SPRR_MASK_MASK)

typedef struct CPUArchState
{
    /* Regs for current mode.  */
    uint32_t regs[16];

    /* 32/64 switch only happens when taking and returning from
     * exceptions so the overlap semantics are taken care of then
     * instead of having a complicated union.
     */
    /* Regs for A64 mode.  */
    uint64_t xregs[32];
    uint64_t pc;
    /* PSTATE isn't an architectural register for ARMv8. However, it is
     * convenient for us to assemble the underlying state into a 32 bit format
     * identical to the architectural format used for the SPSR. (This is also
     * what the Linux kernel's 'pstate' field in signal handlers and KVM's
     * 'pstate' register are.) Of the PSTATE bits:
     *  NZCV are kept in the split out env->CF/VF/NF/ZF, (which have the same
     *    semantics as for AArch32, as described in the comments on each field)
     *  nRW (also known as M[4]) is kept, inverted, in env->aarch64
     *  DAIF (exception masks) are kept in env->daif
     *  BTYPE is kept in env->btype
     *  SM and ZA are kept in env->svcr
     *  all other bits are stored in their correct places in env->pstate
     */
    uint32_t pstate;
    bool     aarch64; /* True if CPU is in aarch64 state; inverse of PSTATE.nRW */
    bool     thumb;   /* True if CPU is in thumb mode; cpsr[5] */

    /* Cached TBFLAGS state.  See below for which bits are included.  */
    CPUARMTBFlags hflags;

    /* Frequently accessed CPSR bits are stored separately for efficiency.
       This contains all the other bits.  Use cpsr_{read,write} to access
       the whole CPSR.  */
    uint32_t uncached_cpsr;
    uint32_t spsr;

    /* Banked registers.  */
    uint64_t banked_spsr[8];
    uint32_t banked_r13[8];
    uint32_t banked_r14[8];

    /* These hold r8-r12.  */
    uint32_t usr_regs[5];
    uint32_t fiq_regs[5];

    /* cpsr flag cache for faster execution */
    uint32_t CF;            /* 0 or 1 */
    uint32_t VF;            /* V is the bit 31. All other bits are undefined */
    uint32_t NF;            /* N is bit 31. All other bits are undefined.  */
    uint32_t ZF;            /* Z set if zero.  */
    uint32_t QF;            /* 0 or 1 */
    uint32_t GE;            /* cpsr[19:16] */
    uint32_t condexec_bits; /* IT bits.  cpsr[15:10,26:25].  */
    uint32_t btype;         /* BTI branch type.  spsr[11:10].  */
    uint64_t daif;          /* exception masks, in the bits they are in PSTATE */
    uint64_t svcr;          /* PSTATE.{SM,ZA} in the bits they are in SVCR */

    uint64_t elr_el[4]; /* AArch64 exception link regs  */
    uint64_t sp_el[4];  /* AArch64 banked stack pointers */

    /* System control coprocessor (cp15) */
    struct
    {
        uint32_t c0_cpuid;
        union
        { /* Cache size selection */
            struct
            {
                uint64_t _unused_csselr0;
                uint64_t csselr_ns;
                uint64_t _unused_csselr1;
                uint64_t csselr_s;
            };
            uint64_t csselr_el[4];
        };
        union
        { /* System control register. */
            struct
            {
                uint64_t _unused_sctlr;
                uint64_t sctlr_ns;
                uint64_t hsctlr;
                uint64_t sctlr_s;
            };
            uint64_t sctlr_el[4];
        };
        uint64_t vsctlr;     /* Virtualization System control register. */
        uint64_t cpacr_el1;  /* Architectural feature access control register */
        uint64_t cptr_el[4]; /* ARMv8 feature trap registers */
        uint64_t sder;       /* Secure debug enable register. */
        uint32_t nsacr;      /* Non-secure access control register. */
        union
        { /* MMU translation table base 0. */
            struct
            {
                uint64_t _unused_ttbr0_0;
                uint64_t ttbr0_ns;
                uint64_t _unused_ttbr0_1;
                uint64_t ttbr0_s;
            };
            uint64_t ttbr0_el[4];
        };
        union
        { /* MMU translation table base 1. */
            struct
            {
                uint64_t _unused_ttbr1_0;
                uint64_t ttbr1_ns;
                uint64_t _unused_ttbr1_1;
                uint64_t ttbr1_s;
            };
            uint64_t ttbr1_el[4];
        };
        uint64_t vttbr_el2;  /* Virtualization Translation Table Base.  */
        uint64_t vsttbr_el2; /* Secure Virtualization Translation Table. */
        /* MMU translation table base control. */
        uint64_t tcr_el[4];
        uint64_t vtcr_el2;  /* Virtualization Translation Control.  */
        uint64_t vstcr_el2; /* Secure Virtualization Translation Control. */
        uint32_t c2_data;   /* MPU data cacheable bits.  */
        uint32_t c2_insn;   /* MPU instruction cacheable bits.  */
        union
        { /* MMU domain access control register
           * MPU write buffer control.
           */
            struct
            {
                uint64_t dacr_ns;
                uint64_t dacr_s;
            };
            struct
            { uint64_t dacr32_el2; };
        };
        uint32_t pmsav5_data_ap; /* PMSAv5 MPU data access permissions */
        uint32_t pmsav5_insn_ap; /* PMSAv5 MPU insn access permissions */
        uint64_t hcr_el2;        /* Hypervisor configuration register */
        uint64_t hcrx_el2;       /* Extended Hypervisor configuration register */
        uint64_t scr_el3;        /* Secure configuration register.  */
        union
        { /* Fault status registers.  */
            struct
            {
                uint64_t ifsr_ns;
                uint64_t ifsr_s;
            };
            struct
            { uint64_t ifsr32_el2; };
        };
        union
        {
            struct
            {
                uint64_t _unused_dfsr;
                uint64_t dfsr_ns;
                uint64_t hsr;
                uint64_t dfsr_s;
            };
            uint64_t esr_el[4];
        };
        uint32_t c6_region[8]; /* MPU base/size registers.  */
        union
        { /* Fault address registers. */
            struct
            {
                uint64_t _unused_far0;
#if HOST_BIG_ENDIAN
                uint32_t ifar_ns;
                uint32_t dfar_ns;
                uint32_t ifar_s;
                uint32_t dfar_s;
#else
                uint32_t dfar_ns;
                uint32_t ifar_ns;
                uint32_t dfar_s;
                uint32_t ifar_s;
#endif
                uint64_t _unused_far3;
            };
            uint64_t far_el[4];
        };
        uint64_t hpfar_el2;
        uint64_t hstr_el2;
        union
        { /* Translation result. */
            struct
            {
                uint64_t _unused_par_0;
                uint64_t par_ns;
                uint64_t _unused_par_1;
                uint64_t par_s;
            };
            uint64_t par_el[4];
        };

        uint32_t c9_insn; /* Cache lockdown registers.  */
        uint32_t c9_data;
        uint64_t c9_pmcr;      /* performance monitor control register */
        uint64_t c9_pmcnten;   /* perf monitor counter enables */
        uint64_t c9_pmovsr;    /* perf monitor overflow status */
        uint64_t c9_pmuserenr; /* perf monitor user enable */
        uint64_t c9_pmselr;    /* perf monitor counter selection register */
        uint64_t c9_pminten;   /* perf monitor interrupt enables */
        union
        { /* Memory attribute redirection */
            struct
            {
#if HOST_BIG_ENDIAN
                uint64_t _unused_mair_0;
                uint32_t mair1_ns;
                uint32_t mair0_ns;
                uint64_t _unused_mair_1;
                uint32_t mair1_s;
                uint32_t mair0_s;
#else
                uint64_t _unused_mair_0;
                uint32_t mair0_ns;
                uint32_t mair1_ns;
                uint64_t _unused_mair_1;
                uint32_t mair0_s;
                uint32_t mair1_s;
#endif
            };
            uint64_t mair_el[4];
        };
        union
        { /* vector base address register */
            struct
            {
                uint64_t _unused_vbar;
                uint64_t vbar_ns;
                uint64_t hvbar;
                uint64_t vbar_s;
            };
            uint64_t vbar_el[4];
        };
        uint32_t mvbar; /* (monitor) vector base address register */
        uint64_t rvbar; /* rvbar sampled from rvbar property at reset */
        struct
        { /* FCSE PID. */
            uint32_t fcseidr_ns;
            uint32_t fcseidr_s;
        };
        union
        { /* Context ID. */
            struct
            {
                uint64_t _unused_contextidr_0;
                uint64_t contextidr_ns;
                uint64_t _unused_contextidr_1;
                uint64_t contextidr_s;
            };
            uint64_t contextidr_el[4];
        };
        union
        { /* User RW Thread register. */
            struct
            {
                uint64_t tpidrurw_ns;
                uint64_t tpidrprw_ns;
                uint64_t htpidr;
                uint64_t _tpidr_el3;
            };
            uint64_t tpidr_el[4];
        };
        uint64_t tpidr2_el0;
        /* The secure banks of these registers don't map anywhere */
        uint64_t tpidrurw_s;
        uint64_t tpidrprw_s;
        uint64_t tpidruro_s;

        union
        { /* User RO Thread register. */
            uint64_t tpidruro_ns;
            uint64_t tpidrro_el[1];
        };
        uint64_t        c14_cntfrq;  /* Counter Frequency register */
        uint64_t        c14_cntkctl; /* Timer Control register */
        uint64_t        cnthctl_el2; /* Counter/Timer Hyp Control register */
        uint64_t        cntvoff_el2; /* Counter Virtual Offset register */
        uint64_t        cntpoff_el2; /* Counter Physical Offset register */
        ARMGenericTimer c14_timer[NUM_GTIMERS];
        uint32_t        c15_ticonfig;            /* TI925T configuration byte.  */
        uint32_t        c15_i_max;               /* Maximum D-cache dirty line index.  */
        uint32_t        c15_i_min;               /* Minimum D-cache dirty line index.  */
        uint32_t        c15_threadid;            /* TI debugger thread-ID.  */
        uint32_t        c15_config_base_address; /* SCU base address.  */
        uint32_t        c15_diagnostic;          /* diagnostic register */
        uint32_t        c15_power_diagnostic;
        uint32_t        c15_power_control; /* power control */
        uint64_t        dbgbvr[16];        /* breakpoint value registers */
        uint64_t        dbgbcr[16];        /* breakpoint control registers */
        uint64_t        dbgwvr[16];        /* watchpoint value registers */
        uint64_t        dbgwcr[16];        /* watchpoint control registers */
        uint64_t        dbgclaim;          /* DBGCLAIM bits */
        uint64_t        mdscr_el1;
        uint64_t        oslsr_el1; /* OS Lock Status */
        uint64_t        osdlr_el1; /* OS DoubleLock status */
        uint64_t        mdcr_el2;
        uint64_t        mdcr_el3;
        /* Stores the architectural value of the counter *the last time it was
         * updated* by pmccntr_op_start. Accesses should always be surrounded
         * by pmccntr_op_start/pmccntr_op_finish to guarantee the latest
         * architecturally-correct value is being read/set.
         */
        uint64_t c15_ccnt;
        /* Stores the delta between the architectural value and the underlying
         * cycle count during normal operation. It is used to update c15_ccnt
         * to be the correct architectural value before accesses. During
         * accesses, c15_ccnt_delta contains the underlying count being used
         * for the access, after which it reverts to the delta value in
         * pmccntr_op_finish.
         */
        uint64_t c15_ccnt_delta;
        uint64_t c14_pmevcntr[31];
        uint64_t c14_pmevcntr_delta[31];
        uint64_t c14_pmevtyper[31];
        uint64_t pmccfiltr_el0; /* Performance Monitor Filter Register */
        uint64_t vpidr_el2;     /* Virtualization Processor ID Register */
        uint64_t vmpidr_el2;    /* Virtualization Multiprocessor ID Register */
        uint64_t tfsr_el[4];    /* tfsre0_el1 is index 0.  */
        uint64_t gcr_el1;
        uint64_t rgsr_el1;

        /* Minimal RAS registers */
        uint64_t disr_el1;
        uint64_t vdisr_el2;
        uint64_t vsesr_el2;

        /*
         * Fine-Grained Trap registers. We store these as arrays so the
         * access checking code doesn't have to manually select
         * HFGRTR_EL2 vs HFDFGRTR_EL2 etc when looking up the bit to test.
         * FEAT_FGT2 will add more elements to these arrays.
         */
        uint64_t fgt_read[2];  /* HFGRTR, HDFGRTR */
        uint64_t fgt_write[2]; /* HFGWTR, HDFGWTR */
        uint64_t fgt_exec[1];  /* HFGITR */

        /* RME registers */
        uint64_t gpccr_el3;
        uint64_t gptbr_el3;
        uint64_t mfar_el3;

        /* NV2 register */
        uint64_t vncr_el2;

        /* Apple-specific registers */
        uint64_t vmsa_lock_el1;
        uint64_t apctl_el1;
        uint64_t apcfg_el1;
    } cp15;

    struct
    {
        uint64_t gxf_config_el[4];
        uint64_t gxf_enter_el[4];
        uint64_t gxf_status_el[4];
        uint64_t gxf_abort_el[4];
        uint64_t sp_gl[4];
        uint64_t tpidr_gl[4];
        uint64_t vbar_gl[4];
        uint64_t spsr_gl[4];
        uint64_t aspsr_gl[4];
        uint64_t esr_gl[4];
        uint64_t elr_gl[4];
        uint64_t far_gl[4];
    } gxf;

    struct
    {
        uint64_t sprr_el_br_el1[4][2];
        uint64_t sprr_config_el[4];
        uint64_t mprr_el_br_el1[4][2];
    } sprr;

    /* Information associated with an exception about to be taken:
     * code which raises an exception must set cs->exception_index and
     * the relevant parts of this structure; the cpu_do_interrupt function
     * will then set the guest-visible registers as part of the exception
     * entry process.
     */
    struct
    {
        uint32_t syndrome;  /* AArch64 format syndrome register */
        uint32_t fsr;       /* AArch32 format fault status register info */
        uint64_t vaddress;  /* virtual addr associated with exception, if any */
        uint32_t target_el; /* EL the exception should be targeted for */
        /* If we implement EL2 we will also need to store information
         * about the intermediate physical address for stage 2 faults.
         */
    } exception;

    /* Information associated with an SError */
    struct
    {
        uint8_t  pending;
        uint8_t  has_esr;
        uint64_t esr;
    } serror;

    uint8_t ext_dabt_raised; /* Tracking/verifying injection of ext DABT */

    /* State of our input IRQ/FIQ/VIRQ/VFIQ lines */
    uint32_t irq_line_state;

    /* Thumb-2 EE state.  */
    uint32_t teecr;
    uint32_t teehbr;

    /* VFP coprocessor state.  */
    struct
    {
        ARMVectorReg zregs[32];

        /* Store FFR as pregs[16] to make it easier to treat as any other.  */
#define FFR_PRED_NUM 16
        ARMPredicateReg pregs[17];
        /* Scratch space for aa64 sve predicate temporary.  */
        ARMPredicateReg preg_tmp;

        /* We store these fpcsr fields separately for convenience.  */
        uint32_t qc[4] QEMU_ALIGNED(16);
        int      vec_len;
        int      vec_stride;

        /*
         * Floating point status and control registers. Some bits are
         * stored separately in other fields or in the float_status below.
         */
        uint64_t fpsr;
        uint64_t fpcr;

        uint32_t xregs[16];

        /* There are a number of distinct float control structures. */
        float_status fp_status[FPST_COUNT];

        uint64_t zcr_el[4];  /* ZCR_EL[1-3] */
        uint64_t smcr_el[4]; /* SMCR_EL[1-3] */
    } vfp;

    uint64_t exclusive_addr;
    uint64_t exclusive_val;
    /*
     * Contains the 'val' for the second 64-bit register of LDXP, which comes
     * from the higher address, not the high part of a complete 128-bit value.
     * In some ways it might be more convenient to record the exclusive value
     * as the low and high halves of a 128 bit data value, but the current
     * semantics of these fields are baked into the migration format.
     */
    uint64_t exclusive_high;

    /* iwMMXt coprocessor state.  */
    struct
    {
        uint64_t regs[16];
        uint64_t val;

        uint32_t cregs[16];
    } iwmmxt;

    struct
    {
        ARMPACKey apia;
        ARMPACKey apib;
        ARMPACKey apda;
        ARMPACKey apdb;
        ARMPACKey apga;
        ARMPACKey kernel;
        ARMPACKey m;
    } keys;

    uint64_t scxtnum_el[4];

    struct
    {
        /* SME2 ZT0 -- 512 bit array, with data ordered like ARMVectorReg. */
        uint64_t zt0[512 / 64] QEMU_ALIGNED(16);

        /*
         * SME ZA storage -- 256 x 256 byte array, with bytes in host
         * word order, as we do with vfp.zregs[].  This corresponds to
         * the architectural ZA array, where ZA[N] is in the least
         * significant bytes of env->za_state.za[N].
         *
         * When SVL is less than the architectural maximum, the accessible
         * storage is restricted, such that if the SVL is X bytes the guest
         * can see only the bottom X elements of zarray[], and only the least
         * significant X bytes of each element of the array. (In other words,
         * the observable part is always square.)
         *
         * The ZA storage can also be considered as a set of square tiles of
         * elements of different sizes. The mapping from tiles to the ZA array
         * is architecturally defined, such that for tiles of elements of esz
         * bytes, the Nth row (or "horizontal slice") of tile T is in
         * ZA[T + N * esz]. Note that this means that each tile is not
         * contiguous in the ZA storage, because its rows are striped through
         * the ZA array.
         *
         * Because this is so large, keep this toward the end of the
         * reset area, to keep the offsets into the rest of the structure
         * smaller.
         */
        ARMVectorReg za[ARM_MAX_VQ * 16];
    } za_state;

    struct CPUBreakpoint* cpu_breakpoint[16];
    struct CPUWatchpoint* cpu_watchpoint[16];

    /* Optional fault info across tlb lookup. */
    ARMMMUFaultInfo* tlb_fi;

    /*
     * Broadcast TLB invalidations issued since the last DSB. The architecture
     * only requires a TLBI to have completed once a DSB has completed, so a run
     * of them is accumulated here and applied in one go, turning an N
     * instruction sequence into a single round of cross-CPU work. @pending is
     * nonzero whenever there is anything to drain, and is the only field the
     * translator looks at.
     */
    struct
    {
        uint32_t     pending;
        MMUIdxMap    idxmap;
        TLBFlushPage pages[ARM_TLBI_BATCH_MAX];
    } tlbi_batch;

    /* Fields up to this point are cleared by a CPU reset */
    struct
    {
    } end_reset_fields;

    /* Fields after this point are preserved across CPU reset. */

    /* Internal CPU feature flags.  */
    uint64_t features;
} CPUARMState;

static inline void set_feature(CPUARMState* env, int feature) { env->features |= 1ULL << feature; }

static inline void unset_feature(CPUARMState* env, int feature) { env->features &= ~(1ULL << feature); }

/**
 * ARMELChangeHookFn:
 * type of a function which can be registered via arm_register_el_change_hook()
 * to get callbacks when the CPU changes its exception level or mode.
 */
typedef void                   ARMELChangeHookFn(ARMCPU* cpu, void* opaque);
typedef struct ARMELChangeHook ARMELChangeHook;
struct ARMELChangeHook
{
    ARMELChangeHookFn* hook;
    void*              opaque;
    QLIST_ENTRY(ARMELChangeHook) node;
};

/* These values map onto the return values for
 * QEMU_PSCI_0_2_FN_AFFINITY_INFO */
typedef enum ARMPSCIState
{
    PSCI_ON         = 0,
    PSCI_OFF        = 1,
    PSCI_ON_PENDING = 2
} ARMPSCIState;

typedef struct ARMISARegisters ARMISARegisters;

/*
 * In map, each set bit is a supported vector length of (bit-number + 1) * 16
 * bytes, i.e. each bit number + 1 is the vector length in quadwords.
 *
 * While processing properties during initialization, corresponding init bits
 * are set for bits in sve_vq_map that have been set by properties.
 *
 * Bits set in supported represent valid vector lengths for the CPU type.
 */
typedef struct
{
    uint32_t map, init, supported;
} ARMVQMap;

/* REG is ID_XXX */
#define FIELD_DP64_IDREG(ISAR, REG, FIELD, VALUE)                              \
    ({                                                                         \
        ARMISARegisters* i_       = (ISAR);                                    \
        uint64_t         regval   = i_->idregs[REG##_EL1_IDX];                 \
        regval                    = REG_FIELD_DP64(regval, REG, FIELD, VALUE); \
        i_->idregs[REG##_EL1_IDX] = regval;                                    \
    })

#define FIELD_DP32_IDREG(ISAR, REG, FIELD, VALUE)                              \
    ({                                                                         \
        ARMISARegisters* i_       = (ISAR);                                    \
        uint64_t         regval   = i_->idregs[REG##_EL1_IDX];                 \
        regval                    = REG_FIELD_DP32(regval, REG, FIELD, VALUE); \
        i_->idregs[REG##_EL1_IDX] = regval;                                    \
    })

#define FIELD_EX64_IDREG(ISAR, REG, FIELD)                     \
    ({                                                         \
        const ARMISARegisters* i_ = (ISAR);                    \
        REG_FIELD_EX64(i_->idregs[REG##_EL1_IDX], REG, FIELD); \
    })

#define FIELD_EX32_IDREG(ISAR, REG, FIELD)                     \
    ({                                                         \
        const ARMISARegisters* i_ = (ISAR);                    \
        REG_FIELD_EX32(i_->idregs[REG##_EL1_IDX], REG, FIELD); \
    })

#define FIELD_SEX64_IDREG(ISAR, REG, FIELD)                     \
    ({                                                          \
        const ARMISARegisters* i_ = (ISAR);                     \
        REG_FIELD_SEX64(i_->idregs[REG##_EL1_IDX], REG, FIELD); \
    })

#define SET_IDREG(ISAR, REG, VALUE)         \
    ({                                      \
        ARMISARegisters* i_       = (ISAR); \
        i_->idregs[REG##_EL1_IDX] = VALUE;  \
    })

#define GET_IDREG(ISAR, REG)                \
    ({                                      \
        const ARMISARegisters* i_ = (ISAR); \
        i_->idregs[REG##_EL1_IDX];          \
    })

/**
 * ARMCPU:
 * @env: #CPUARMState
 *
 * An ARM CPU core.
 */
struct ArchCPU
{
    CPUState parent_obj;

    CPUARMState env;

    /* Coprocessor information */
    ARMCPRegTable_t cp_regs;
    /* For marshalling (mostly coprocessor) register state between the
     * kernel and QEMU (for KVM) and between two QEMUs (for migration),
     * we use these arrays.
     */
    /* List of register indexes managed via these arrays; (full KVM style
     * 64 bit indexes, not CPRegInfo 32 bit indexes)
     */
    uint64_t* cpreg_indexes;
    /* Values of the registers (cpreg_indexes[i]'s value is cpreg_values[i]) */
    uint64_t* cpreg_values;
    /* Length of the indexes, values, reset_values arrays */
    int32_t cpreg_array_len;
    /* These are used only for migration: incoming data arrives in
     * these fields and is sanity checked in post_load before copying
     * to the working data structures above.
     */
    uint64_t* cpreg_vmstate_indexes;
    uint64_t* cpreg_vmstate_values;
    int32_t   cpreg_vmstate_array_len;

    DynamicGDBFeatureInfo dyn_sysreg_feature;
    DynamicGDBFeatureInfo dyn_svereg_feature;
    DynamicGDBFeatureInfo dyn_smereg_feature;
    DynamicGDBFeatureInfo dyn_m_systemreg_feature;
    DynamicGDBFeatureInfo dyn_m_secextreg_feature;

    /* Timers used by the generic (architected) timer */
    QEMUTimer* gt_timer[NUM_GTIMERS];
    /*
     * Timer used by the PMU. Its state is restored after migration by
     * pmu_op_finish() - it does not need other handling during migration
     */
    QEMUTimer* pmu_timer;
    /* Timer used for WFxT timeouts */
    QEMUTimer* wfxt_timer;

    /* GPIO outputs for generic timer */
    qemu_irq gt_timer_outputs[NUM_GTIMERS];
    int      gt_irqstate[NUM_GTIMERS];
    /* GPIO output for the PMU interrupt */
    qemu_irq pmu_interrupt;

    /* MemoryRegion to use for secure physical accesses */
    MemoryRegion* secure_memory;

    /* MemoryRegion to use for allocation tag accesses */
    MemoryRegion* tag_memory;
    MemoryRegion* secure_tag_memory;

    /* Current power state, access guarded by BQL */
    ARMPSCIState power_state;

    /* CPU has virtualization extension */
    bool has_el2;
    /* CPU has security extension */
    bool has_el3;
    /* CPU has PMU (Performance Monitor Unit) */
    bool has_pmu;
    /* CPU has VFP */
    bool has_vfp;
    /* CPU has 32 VFP registers */
    bool has_vfp_d32;
    /* CPU has Neon */
    bool has_neon;

    /* CPU has MTE enabled in KVM mode */
    bool kvm_mte;

    /* [QEMU_]KVM_ARM_TARGET_* constant for this CPU, or
     * QEMU_KVM_ARM_TARGET_NONE if the kernel doesn't support this CPU type.
     */
    uint32_t kvm_target;

    /* KVM init features for this CPU */
    uint32_t kvm_init_features[7];

    /* KVM CPU state */

    /* KVM virtual time adjustment */
    bool     kvm_adjvtime;
    bool     kvm_vtime_dirty;
    uint64_t kvm_vtime;

    /* KVM steal time */
    OnOffAuto kvm_steal_time;

    /* Uniprocessor system with MP extensions */
    bool mp_is_up;

    /* True if we tried kvm_arm_host_cpu_features() during CPU instance_init
     * and the probe failed (so we need to report the error in realize)
     */
    bool host_cpu_probe_failed;

    /* Specify the number of cores in this CPU cluster. Used for the L2CTLR
     * register.
     */
    int32_t core_count;

    /* The instance init functions for implementation-specific subclasses
     * set these fields to specify the implementation-dependent values of
     * various constant registers and reset values of non-constant
     * registers.
     * Some of these might become QOM properties eventually.
     * Field names match the official register names as defined in the
     * ARMv7AR ARM Architecture Reference Manual. A reset_ prefix
     * is used for reset values of non-constant registers; no reset_
     * prefix means a constant register.
     * Some of these registers are split out into a substructure that
     * is shared with the translators to control the ISA.
     *
     * Note that if you add an ID register to the ARMISARegisters struct
     * you need to also update the 32-bit and 64-bit versions of the
     * kvm_arm_get_host_cpu_features() function to correctly populate the
     * field by reading the value from the KVM vCPU.
     */
    struct ARMISARegisters
    {
        uint32_t mvfr0;
        uint32_t mvfr1;
        uint32_t mvfr2;
        uint32_t dbgdidr;
        uint32_t dbgdevid;
        uint32_t dbgdevid1;
        uint64_t reset_pmcr_el0;
        uint64_t idregs[NUM_ID_IDX];
    } isar;
    uint64_t midr;
    uint32_t revidr;
    uint32_t reset_fpsid;
    uint64_t ctr;
    uint32_t reset_sctlr;
    uint64_t pmceid0;
    uint64_t pmceid1;
    uint64_t mp_affinity; /* MP ID without feature bits */
    /* The elements of this array are the CCSIDR values for each cache,
     * in the order L1DCache, L1ICache, L2DCache, L2ICache, etc.
     */
    uint64_t ccsidr[16];
    uint64_t reset_cbar;
    uint32_t reset_auxcr;
    bool     reset_hivecs;
    uint8_t  reset_l0gptsz;

    /*
     * Intermediate values used during property parsing.
     * Once finalized, the values should be read from ID_AA64*.
     */
    bool prop_pauth;
    bool prop_pauth_impdef;
    bool prop_pauth_qarma3;
    bool prop_pauth_qarma5;
    bool prop_pauth_noop;
    bool prop_lpa2;

    /* DCZ blocksize, in log_2(words), ie low 4 bits of DCZID_EL0 */
    uint8_t dcz_blocksize;
    /* GM blocksize, in log_2(words), ie low 4 bits of GMID_EL0 */
    uint8_t gm_blocksize;

    uint64_t rvbar_prop; /* Property/input signals.  */

    /* Whether the cfgend input is high (i.e. this CPU should reset into
     * big-endian mode).  This setting isn't used directly: instead it modifies
     * the reset_sctlr value to have SCTLR_B or SCTLR_EE set, depending on the
     * architecture version.
     */
    bool cfgend;

    QLIST_HEAD(, ARMELChangeHook) pre_el_change_hooks;
    QLIST_HEAD(, ARMELChangeHook) el_change_hooks;

    /* Used to synchronize KVM and QEMU in-kernel device levels */
    uint8_t device_irq_level;

    /* Used to set the maximum vector length the cpu will support.  */
    uint32_t sve_max_vq;
    uint32_t sme_max_vq;

    ARMVQMap sve_vq;
    ARMVQMap sme_vq;

    /* Generic timer counter frequency, in Hz */
    uint64_t gt_cntfrq_hz;
    uint64_t gt_tick_num;
    uint64_t gt_tick_den;

    /* Apple PAC boot diversifier */
    uint64_t m_key_lo;
    uint64_t m_key_hi;
};

typedef struct ARMCPUInfo
{
    const char* name;
    const char* deprecation_note;
    void        (*initfn)(Object* obj);
    void        (*class_init)(ObjectClass* oc, const void* data);
} ARMCPUInfo;

/**
 * ARMCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_phases: The parent class' reset phase handlers.
 *
 * An ARM CPU model.
 */
struct ARMCPUClass
{
    CPUClass parent_class;

    const ARMCPUInfo* info;
    DeviceRealize     parent_realize;
    ResettablePhases  parent_phases;
};

/* Callback functions for the generic timer's timers. */
void arm_gt_ptimer_cb(void* opaque);
void arm_gt_vtimer_cb(void* opaque);
void arm_gt_htimer_cb(void* opaque);
void arm_gt_stimer_cb(void* opaque);
void arm_gt_hvtimer_cb(void* opaque);
void arm_gt_sel2timer_cb(void* opaque);
void arm_gt_sel2vtimer_cb(void* opaque);

int64_t  gt_ticks_to_ns_ceil(ARMCPU* cpu, uint64_t ticks);
uint64_t gt_ns_to_ticks(ARMCPU* cpu, uint64_t ns);
uint64_t gt_ticks_to_ns(ARMCPU* cpu, uint64_t ticks);
void     gt_derive_tick_ratio(ARMCPU* cpu);
void     gt_rme_post_el_change(ARMCPU* cpu, void* opaque);

#define ARM_AFF0_SHIFT               0
#define ARM_AFF0_MASK                (0xFFULL << ARM_AFF0_SHIFT)
#define ARM_AFF1_SHIFT               8
#define ARM_AFF1_MASK                (0xFFULL << ARM_AFF1_SHIFT)
#define ARM_AFF2_SHIFT               16
#define ARM_AFF2_MASK                (0xFFULL << ARM_AFF2_SHIFT)
#define ARM_AFF3_SHIFT               32
#define ARM_AFF3_MASK                (0xFFULL << ARM_AFF3_SHIFT)
#define ARM_DEFAULT_CPUS_PER_CLUSTER 8

#define ARM32_AFFINITY_MASK    (ARM_AFF0_MASK | ARM_AFF1_MASK | ARM_AFF2_MASK)
#define ARM64_AFFINITY_MASK    (ARM_AFF0_MASK | ARM_AFF1_MASK | ARM_AFF2_MASK | ARM_AFF3_MASK)
#define ARM64_AFFINITY_INVALID (~ARM64_AFFINITY_MASK)

uint64_t arm_build_mp_affinity(int idx, uint8_t clustersz);

void arm_cpu_do_interrupt(CPUState* cpu);

hwaddr arm_cpu_get_phys_page_attrs_debug(CPUState* cpu, vaddr addr, MemTxAttrs* attrs);

int arm_cpu_gdb_read_register(CPUState* cpu, GByteArray* buf, int reg);
int arm_cpu_gdb_write_register(CPUState* cpu, uint8_t* buf, int reg);

/**
 * arm_emulate_firmware_reset: Emulate firmware CPU reset handling
 * @cpu: CPU (which must have been freshly reset)
 * @target_el: exception level to put the CPU into
 * @secure: whether to put the CPU in secure state
 *
 * When QEMU is directly running a guest kernel at a lower level than
 * EL3 it implicitly emulates some aspects of the guest firmware.
 * This includes that on reset we need to configure the parts of the
 * CPU corresponding to EL3 so that the real guest code can run at its
 * lower exception level. This function does that post-reset CPU setup,
 * for when we do direct boot of a guest kernel, and for when firmware
 * interfaces start a CPU at a lower exception level.
 *
 * @target_el must be an EL implemented by the CPU between 1 and 3.
 * We do not support dropping into a Secure EL other than 3.
 *
 * It is the responsibility of the caller to call arm_rebuild_hflags().
 */
void arm_emulate_firmware_reset(CPUState* cpustate, int target_el);

int  aarch64_cpu_gdb_read_register(CPUState* cpu, GByteArray* buf, int reg);
int  aarch64_cpu_gdb_write_register(CPUState* cpu, uint8_t* buf, int reg);
void aarch64_sve_narrow_vq(CPUARMState* env, unsigned vq);
void aarch64_sve_change_el(CPUARMState* env, int old_el, int new_el, bool el0_a64);
void aarch64_set_svcr(CPUARMState* env, uint64_t new, uint64_t mask);

/*
 * SVE registers are encoded in KVM's memory in an endianness-invariant format.
 * The byte at offset i from the start of the in-memory representation contains
 * the bits [(7 + 8 * i) : (8 * i)] of the register value. As this means the
 * lowest offsets are stored in the lowest memory addresses, then that nearly
 * matches QEMU's representation, which is to use an array of host-endian
 * uint64_t's, where the lower offsets are at the lower indices. To complete
 * the translation we just need to byte swap the uint64_t's on big-endian hosts.
 */
static inline uint64_t* sve_bswap64(uint64_t* dst, uint64_t* src, int nr)
{
#if HOST_BIG_ENDIAN
    int i;

    for (i = 0; i < nr; ++i) { dst[i] = bswap64(src[i]); }

    return dst;
#else
    return src;
#endif
}

void aarch64_sync_32_to_64(CPUARMState* env);
void aarch64_sync_64_to_32(CPUARMState* env);

int fp_exception_el(CPUARMState* env, int cur_el);
int sve_exception_el(CPUARMState* env, int cur_el);
int sme_exception_el(CPUARMState* env, int cur_el);

/**
 * sve_vqm1_for_el_sm:
 * @env: CPUARMState
 * @el: exception level
 * @sm: streaming mode
 *
 * Compute the current vector length for @el & @sm, in units of
 * Quadwords Minus 1 -- the same scale used for ZCR_ELx.LEN.
 * If @sm, compute for SVL, otherwise NVL.
 */
uint32_t sve_vqm1_for_el_sm(CPUARMState* env, int el, bool sm);

/* Likewise, but using @sm = PSTATE.SM. */
uint32_t sve_vqm1_for_el(CPUARMState* env, int el);

static inline bool is_a64(CPUARMState* env) { return env->aarch64; }

/**
 * pmu_op_start/finish
 * @env: CPUARMState
 *
 * Convert all PMU counters between their delta form (the typical mode when
 * they are enabled) and the guest-visible values. These two calls must
 * surround any action which might affect the counters.
 */
void pmu_op_start(CPUARMState* env);
void pmu_op_finish(CPUARMState* env);

/*
 * Called when a PMU counter is due to overflow
 */
void arm_pmu_timer_cb(void* opaque);

/**
 * Functions to register as EL change hooks for PMU mode filtering
 */
void pmu_pre_el_change(ARMCPU* cpu, void* ignored);
void pmu_post_el_change(ARMCPU* cpu, void* ignored);

/*
 * pmu_init
 * @cpu: ARMCPU
 *
 * Initialize the CPU's PMCEID[01]_EL0 registers and associated internal state
 * for the current configuration
 */
void pmu_init(ARMCPU* cpu);

/* SCTLR bit meanings. Several bits have been reused in newer
 * versions of the architecture; in that case we define constants
 * for both old and new bit meanings. Code which tests against those
 * bits should probably check or otherwise arrange that the CPU
 * is the architectural version it expects.
 */
#define SCTLR_M         (1U << 0)
#define SCTLR_A         (1U << 1)
#define SCTLR_C         (1U << 2)
#define SCTLR_W         (1U << 3)  /* up to v6; RAO in v7 */
#define SCTLR_nTLSMD_32 (1U << 3)  /* v8.2-LSMAOC, AArch32 only */
#define SCTLR_SA        (1U << 3)  /* AArch64 only */
#define SCTLR_P         (1U << 4)  /* up to v5; RAO in v6 and v7 */
#define SCTLR_LSMAOE_32 (1U << 4)  /* v8.2-LSMAOC, AArch32 only */
#define SCTLR_SA0       (1U << 4)  /* v8 onward, AArch64 only */
#define SCTLR_D         (1U << 5)  /* up to v5; RAO in v6 */
#define SCTLR_CP15BEN   (1U << 5)  /* v7 onward */
#define SCTLR_L         (1U << 6)  /* up to v5; RAO in v6 and v7; RAZ in v8 */
#define SCTLR_nAA       (1U << 6)  /* when FEAT_LSE2 is implemented */
#define SCTLR_B         (1U << 7)  /* up to v6; RAZ in v7 */
#define SCTLR_ITD       (1U << 7)  /* v8 onward */
#define SCTLR_S         (1U << 8)  /* up to v6; RAZ in v7 */
#define SCTLR_SED       (1U << 8)  /* v8 onward */
#define SCTLR_R         (1U << 9)  /* up to v6; RAZ in v7 */
#define SCTLR_UMA       (1U << 9)  /* v8 onward, AArch64 only */
#define SCTLR_F         (1U << 10) /* up to v6 */
#define SCTLR_SW        (1U << 10) /* v7 */
#define SCTLR_EnRCTX    (1U << 10) /* in v8.0-PredInv */
#define SCTLR_Z         (1U << 11) /* in v7, RES1 in v8 */
#define SCTLR_EOS       (1U << 11) /* v8.5-ExS */
#define SCTLR_I         (1U << 12)
#define SCTLR_V         (1U << 13) /* AArch32 only */
#define SCTLR_EnDB      (1U << 13) /* v8.3, AArch64 only */
#define SCTLR_RR        (1U << 14) /* up to v7 */
#define SCTLR_DZE       (1U << 14) /* v8 onward, AArch64 only */
#define SCTLR_L4        (1U << 15) /* up to v6; RAZ in v7 */
#define SCTLR_UCT       (1U << 15) /* v8 onward, AArch64 only */
#define SCTLR_DT        (1U << 16) /* up to ??, RAO in v6 and v7 */
#define SCTLR_nTWI      (1U << 16) /* v8 onward */
#define SCTLR_HA        (1U << 17) /* up to v7, RES0 in v8 */
#define SCTLR_BR        (1U << 17) /* PMSA only */
#define SCTLR_IT        (1U << 18) /* up to ??, RAO in v6 and v7 */
#define SCTLR_nTWE      (1U << 18) /* v8 onward */
#define SCTLR_WXN       (1U << 19)
#define SCTLR_ST        (1U << 20) /* up to ??, RAZ in v6 */
#define SCTLR_UWXN      (1U << 20) /* v7 onward, AArch32 only */
#define SCTLR_TSCXT     (1U << 20) /* FEAT_CSV2_1p2, AArch64 only */
#define SCTLR_FI        (1U << 21) /* up to v7, v8 RES0 */
#define SCTLR_IESB      (1U << 21) /* v8.2-IESB, AArch64 only */
#define SCTLR_U         (1U << 22) /* up to v6, RAO in v7 */
#define SCTLR_EIS       (1U << 22) /* v8.5-ExS */
#define SCTLR_XP        (1U << 23) /* up to v6; v7 onward RAO */
#define SCTLR_SPAN      (1U << 23) /* v8.1-PAN */
#define SCTLR_VE        (1U << 24) /* up to v7 */
#define SCTLR_E0E       (1U << 24) /* v8 onward, AArch64 only */
#define SCTLR_EE        (1U << 25)
#define SCTLR_L2        (1U << 26)          /* up to v6, RAZ in v7 */
#define SCTLR_UCI       (1U << 26)          /* v8 onward, AArch64 only */
#define SCTLR_NMFI      (1U << 27)          /* up to v7, RAZ in v7VE and v8 */
#define SCTLR_EnDA      (1U << 27)          /* v8.3, AArch64 only */
#define SCTLR_TRE       (1U << 28)          /* AArch32 only */
#define SCTLR_nTLSMD_64 (1U << 28)          /* v8.2-LSMAOC, AArch64 only */
#define SCTLR_AFE       (1U << 29)          /* AArch32 only */
#define SCTLR_LSMAOE_64 (1U << 29)          /* v8.2-LSMAOC, AArch64 only */
#define SCTLR_TE        (1U << 30)          /* AArch32 only */
#define SCTLR_EnIB      (1U << 30)          /* v8.3, AArch64 only */
#define SCTLR_EnIA      (1U << 31)          /* v8.3, AArch64 only */
#define SCTLR_DSSBS_32  (1U << 31)          /* v8.5, AArch32 only */
#define SCTLR_CMOW      (1ULL << 32)        /* FEAT_CMOW */
#define SCTLR_MSCEN     (1ULL << 33)        /* FEAT_MOPS */
#define SCTLR_BT0       (1ULL << 35)        /* v8.5-BTI */
#define SCTLR_BT1       (1ULL << 36)        /* v8.5-BTI */
#define SCTLR_ITFSB     (1ULL << 37)        /* v8.5-MemTag */
#define SCTLR_TCF0      (3ULL << 38)        /* v8.5-MemTag */
#define SCTLR_TCF       (3ULL << 40)        /* v8.5-MemTag */
#define SCTLR_ATA0      (1ULL << 42)        /* v8.5-MemTag */
#define SCTLR_ATA       (1ULL << 43)        /* v8.5-MemTag */
#define SCTLR_DSSBS_64  (1ULL << 44)        /* v8.5, AArch64 only */
#define SCTLR_TWEDEn    (1ULL << 45)        /* FEAT_TWED */
#define SCTLR_TWEDEL    MAKE_64_MASK(46, 4) /* FEAT_TWED */
#define SCTLR_TMT0      (1ULL << 50)        /* FEAT_TME */
#define SCTLR_TMT       (1ULL << 51)        /* FEAT_TME */
#define SCTLR_TME0      (1ULL << 52)        /* FEAT_TME */
#define SCTLR_TME       (1ULL << 53)        /* FEAT_TME */
#define SCTLR_EnASR     (1ULL << 54)        /* FEAT_LS64_V */
#define SCTLR_EnAS0     (1ULL << 55)        /* FEAT_LS64_ACCDATA */
#define SCTLR_EnALS     (1ULL << 56)        /* FEAT_LS64 */
#define SCTLR_EPAN      (1ULL << 57)        /* FEAT_PAN3 */
#define SCTLR_EnTP2     (1ULL << 60)        /* FEAT_SME */
#define SCTLR_NMI       (1ULL << 61)        /* FEAT_NMI */
#define SCTLR_SPINTMASK (1ULL << 62)        /* FEAT_NMI */
#define SCTLR_TIDCP     (1ULL << 63)        /* FEAT_TIDCP1 */

#define CPSR_M      (0x1fU)
#define CPSR_T      (1U << 5)
#define CPSR_F      (1U << 6)
#define CPSR_I      (1U << 7)
#define CPSR_A      (1U << 8)
#define CPSR_E      (1U << 9)
#define CPSR_IT_2_7 (0xfc00U)
#define CPSR_GE     (0xfU << 16)
#define CPSR_IL     (1U << 20)
#define CPSR_DIT    (1U << 21)
#define CPSR_PAN    (1U << 22)
#define CPSR_SSBS   (1U << 23)
#define CPSR_J      (1U << 24)
#define CPSR_IT_0_1 (3U << 25)
#define CPSR_Q      (1U << 27)
#define CPSR_V      (1U << 28)
#define CPSR_C      (1U << 29)
#define CPSR_Z      (1U << 30)
#define CPSR_N      (1U << 31)
#define CPSR_NZCV   (CPSR_N | CPSR_Z | CPSR_C | CPSR_V)
#define CPSR_AIF    (CPSR_A | CPSR_I | CPSR_F)
#define ISR_FS      (1U << 9)
#define ISR_IS      (1U << 10)

#define CPSR_IT          (CPSR_IT_0_1 | CPSR_IT_2_7)
#define CACHED_CPSR_BITS (CPSR_T | CPSR_AIF | CPSR_GE | CPSR_IT | CPSR_Q | CPSR_NZCV)
/* Bits writable in user mode.  */
#define CPSR_USER (CPSR_NZCV | CPSR_Q | CPSR_GE | CPSR_E)
/* Execution state bits.  MRS read as zero, MSR writes ignored.  */
#define CPSR_EXEC (CPSR_T | CPSR_IT | CPSR_J | CPSR_IL)

/* Bit definitions for M profile XPSR. Most are the same as CPSR. */
#define XPSR_EXCP      0x1ffU
#define XPSR_SPREALIGN (1U << 9) /* Only set in exception stack frames */
#define XPSR_IT_2_7    CPSR_IT_2_7
#define XPSR_GE        CPSR_GE
#define XPSR_SFPA      (1U << 20) /* Only set in exception stack frames */
#define XPSR_T         (1U << 24) /* Not the same as CPSR_T ! */
#define XPSR_IT_0_1    CPSR_IT_0_1
#define XPSR_Q         CPSR_Q
#define XPSR_V         CPSR_V
#define XPSR_C         CPSR_C
#define XPSR_Z         CPSR_Z
#define XPSR_N         CPSR_N
#define XPSR_NZCV      CPSR_NZCV
#define XPSR_IT        CPSR_IT

/* Bit definitions for ARMv8 SPSR (PSTATE) format.
 * Only these are valid when in AArch64 mode; in
 * AArch32 mode SPSRs are basically CPSR-format.
 */
#define PSTATE_SP          (1U)
#define PSTATE_M           (0xFU)
#define PSTATE_nRW         (1U << 4)
#define PSTATE_F           (1U << 6)
#define PSTATE_I           (1U << 7)
#define PSTATE_A           (1U << 8)
#define PSTATE_D           (1U << 9)
#define PSTATE_BTYPE       (3U << 10)
#define PSTATE_SSBS        (1U << 12)
#define PSTATE_ALLINT      (1U << 13)
#define PSTATE_IL          (1U << 20)
#define PSTATE_SS          (1U << 21)
#define PSTATE_PAN         (1U << 22)
#define PSTATE_UAO         (1U << 23)
#define PSTATE_DIT         (1U << 24)
#define PSTATE_TCO         (1U << 25)
#define PSTATE_V           (1U << 28)
#define PSTATE_C           (1U << 29)
#define PSTATE_Z           (1U << 30)
#define PSTATE_N           (1U << 31)
#define PSTATE_NZCV        (PSTATE_N | PSTATE_Z | PSTATE_C | PSTATE_V)
#define PSTATE_DAIF        (PSTATE_D | PSTATE_A | PSTATE_I | PSTATE_F)
#define CACHED_PSTATE_BITS (PSTATE_NZCV | PSTATE_DAIF | PSTATE_BTYPE)
/* Mode values for AArch64 */
#define PSTATE_MODE_EL3h 13
#define PSTATE_MODE_EL3t 12
#define PSTATE_MODE_EL2h 9
#define PSTATE_MODE_EL2t 8
#define PSTATE_MODE_EL1h 5
#define PSTATE_MODE_EL1t 4
#define PSTATE_MODE_EL0t 0
/* VMSA_LOCK_EL1 */
#define VMSA_LOCK_VBAR_EL1    (1ULL << 0)
#define VMSA_LOCK_SCTLR_EL1   (1ULL << 1)
#define VMSA_LOCK_TCR_EL1     (1ULL << 2)
#define VMSA_LOCK_TTBR0_EL1   (1ULL << 3)
#define VMSA_LOCK_TTBR1_EL1   (1ULL << 4)
#define VMSA_LOCK_SCTLR_M_BIT (1ULL << 63)
/* APCTL_EL1 */
#define APCTL_AppleMode (1ULL << 0)
#define APCTL_MKEYVld   (1ULL << 1)
#define APCTL_KernKeyEn (1ULL << 2)
/* APCFG_EL1 */
#define APCFG_EL1_ELXENKEY (1ULL << 1)

/* PSTATE bits that are accessed via SVCR and not stored in SPSR_ELx. */
REG_FIELD(SVCR, SM, 0, 1)
REG_FIELD(SVCR, ZA, 1, 1)

/* Fields for SMCR_ELx. */
REG_FIELD(SMCR, LEN, 0, 4)
REG_FIELD(SMCR, EZT0, 30, 1)
REG_FIELD(SMCR, FA64, 31, 1)

/* Map EL and handler into a PSTATE_MODE.  */
static inline unsigned int aarch64_pstate_mode(unsigned int el, bool handler) { return (el << 2) | handler; }

/* Return the current PSTATE value. For the moment we don't support 32<->64 bit
 * interprocessing, so we don't attempt to sync with the cpsr state used by
 * the 32 bit decoder.
 */
static inline uint32_t pstate_read(CPUARMState* env)
{
    int ZF;

    ZF = (env->ZF == 0);
    return (env->NF & 0x80000000) | (ZF << 30) | (env->CF << 29) | ((env->VF & 0x80000000) >> 3) | env->pstate
           | env->daif | (env->btype << 10);
}

static inline void pstate_write(CPUARMState* env, uint32_t val)
{
    env->ZF     = (~val) & PSTATE_Z;
    env->NF     = val;
    env->CF     = (val >> 29) & 1;
    env->VF     = (val << 3) & 0x80000000;
    env->daif   = val & PSTATE_DAIF;
    env->btype  = (val >> 10) & 3;
    env->pstate = val & ~CACHED_PSTATE_BITS;
}

/* Return the current CPSR value.  */
uint32_t cpsr_read(CPUARMState* env);

typedef enum CPSRWriteType
{
    CPSRWriteByInstr         = 0, /* from guest MSR or CPS */
    CPSRWriteExceptionReturn = 1, /* from guest exception return insn */
    CPSRWriteRaw             = 2,
    /* trust values, no reg bank switch, no hflags rebuild */
    CPSRWriteByGDBStub = 3, /* from the GDB stub */
} CPSRWriteType;

/*
 * Set the CPSR.  Note that some bits of mask must be all-set or all-clear.
 * This will do an arm_rebuild_hflags() if any of the bits in @mask
 * correspond to TB flags bits cached in the hflags, unless @write_type
 * is CPSRWriteRaw.
 */
void cpsr_write(CPUARMState* env, uint32_t val, uint32_t mask, CPSRWriteType write_type);

/* Return the current xPSR value.  */
static inline uint32_t xpsr_read(CPUARMState* env)
{
    int ZF;
    ZF = (env->ZF == 0);
    return (env->NF & 0x80000000) | (ZF << 30) | (env->CF << 29) | ((env->VF & 0x80000000) >> 3) | (env->QF << 27)
           | (env->thumb << 24) | ((env->condexec_bits & 3) << 25) | ((env->condexec_bits & 0xfc) << 8)
           | (env->GE << 16);
}

/* Set the xPSR.  Note that some bits of mask must be all-set or all-clear.  */
static inline void xpsr_write(CPUARMState* env, uint32_t val, uint32_t mask)
{
    if (mask & XPSR_NZCV) {
        env->ZF = (~val) & XPSR_Z;
        env->NF = val;
        env->CF = (val >> 29) & 1;
        env->VF = (val << 3) & 0x80000000;
    }
    if (mask & XPSR_Q) { env->QF = ((val & XPSR_Q) != 0); }
    if (mask & XPSR_GE) { env->GE = (val & XPSR_GE) >> 16; }
    if (mask & XPSR_T) { env->thumb = ((val & XPSR_T) != 0); }
    if (mask & XPSR_IT_0_1) {
        env->condexec_bits &= ~3;
        env->condexec_bits |= (val >> 25) & 3;
    }
    if (mask & XPSR_IT_2_7) {
        env->condexec_bits &= 3;
        env->condexec_bits |= (val >> 8) & 0xfc;
    }
}

#define HCR_VM       (1ULL << 0)
#define HCR_SWIO     (1ULL << 1)
#define HCR_PTW      (1ULL << 2)
#define HCR_FMO      (1ULL << 3)
#define HCR_IMO      (1ULL << 4)
#define HCR_AMO      (1ULL << 5)
#define HCR_VF       (1ULL << 6)
#define HCR_VI       (1ULL << 7)
#define HCR_VSE      (1ULL << 8)
#define HCR_FB       (1ULL << 9)
#define HCR_BSU_MASK (3ULL << 10)
#define HCR_DC       (1ULL << 12)
#define HCR_TWI      (1ULL << 13)
#define HCR_TWE      (1ULL << 14)
#define HCR_TID0     (1ULL << 15)
#define HCR_TID1     (1ULL << 16)
#define HCR_TID2     (1ULL << 17)
#define HCR_TID3     (1ULL << 18)
#define HCR_TSC      (1ULL << 19)
#define HCR_TIDCP    (1ULL << 20)
#define HCR_TACR     (1ULL << 21)
#define HCR_TSW      (1ULL << 22)
#define HCR_TPCP     (1ULL << 23)
#define HCR_TPU      (1ULL << 24)
#define HCR_TTLB     (1ULL << 25)
#define HCR_TVM      (1ULL << 26)
#define HCR_TGE      (1ULL << 27)
#define HCR_TDZ      (1ULL << 28)
#define HCR_HCD      (1ULL << 29)
#define HCR_TRVM     (1ULL << 30)
#define HCR_RW       (1ULL << 31)
#define HCR_CD       (1ULL << 32)
#define HCR_ID       (1ULL << 33)
#define HCR_E2H      (1ULL << 34)
#define HCR_TLOR     (1ULL << 35)
#define HCR_TERR     (1ULL << 36)
#define HCR_TEA      (1ULL << 37)
#define HCR_MIOCNCE  (1ULL << 38)
#define HCR_TME      (1ULL << 39)
#define HCR_APK      (1ULL << 40)
#define HCR_API      (1ULL << 41)
#define HCR_NV       (1ULL << 42)
#define HCR_NV1      (1ULL << 43)
#define HCR_AT       (1ULL << 44)
#define HCR_NV2      (1ULL << 45)
#define HCR_FWB      (1ULL << 46)
#define HCR_FIEN     (1ULL << 47)
#define HCR_GPF      (1ULL << 48)
#define HCR_TID4     (1ULL << 49)
#define HCR_TICAB    (1ULL << 50)
#define HCR_AMVOFFEN (1ULL << 51)
#define HCR_TOCU     (1ULL << 52)
#define HCR_ENSCXT   (1ULL << 53)
#define HCR_TTLBIS   (1ULL << 54)
#define HCR_TTLBOS   (1ULL << 55)
#define HCR_ATA      (1ULL << 56)
#define HCR_DCT      (1ULL << 57)
#define HCR_TID5     (1ULL << 58)
#define HCR_TWEDEN   (1ULL << 59)
#define HCR_TWEDEL   MAKE_64BIT_MASK(60, 4)

#define SCR_NS       (1ULL << 0)
#define SCR_IRQ      (1ULL << 1)
#define SCR_FIQ      (1ULL << 2)
#define SCR_EA       (1ULL << 3)
#define SCR_FW       (1ULL << 4)
#define SCR_AW       (1ULL << 5)
#define SCR_NET      (1ULL << 6)
#define SCR_SMD      (1ULL << 7)
#define SCR_HCE      (1ULL << 8)
#define SCR_SIF      (1ULL << 9)
#define SCR_RW       (1ULL << 10)
#define SCR_ST       (1ULL << 11)
#define SCR_TWI      (1ULL << 12)
#define SCR_TWE      (1ULL << 13)
#define SCR_TLOR     (1ULL << 14)
#define SCR_TERR     (1ULL << 15)
#define SCR_APK      (1ULL << 16)
#define SCR_API      (1ULL << 17)
#define SCR_EEL2     (1ULL << 18)
#define SCR_EASE     (1ULL << 19)
#define SCR_NMEA     (1ULL << 20)
#define SCR_FIEN     (1ULL << 21)
#define SCR_ENSCXT   (1ULL << 25)
#define SCR_ATA      (1ULL << 26)
#define SCR_FGTEN    (1ULL << 27)
#define SCR_ECVEN    (1ULL << 28)
#define SCR_TWEDEN   (1ULL << 29)
#define SCR_TWEDEL   MAKE_64BIT_MASK(30, 4)
#define SCR_TME      (1ULL << 34)
#define SCR_AMVOFFEN (1ULL << 35)
#define SCR_ENAS0    (1ULL << 36)
#define SCR_ADEN     (1ULL << 37)
#define SCR_HXEN     (1ULL << 38)
#define SCR_TRNDR    (1ULL << 40)
#define SCR_ENTP2    (1ULL << 41)
#define SCR_GPF      (1ULL << 48)
#define SCR_NSE      (1ULL << 62)

/* Return the current FPSCR value.  */
uint32_t vfp_get_fpscr(CPUARMState* env);
void     vfp_set_fpscr(CPUARMState* env, uint32_t val);

/*
 * FPCR, Floating Point Control Register
 * FPSR, Floating Point Status Register
 *
 * For A64 floating point control and status bits are stored in
 * two logically distinct registers, FPCR and FPSR. We store these
 * in QEMU in vfp.fpcr and vfp.fpsr.
 * For A32 there was only one register, FPSCR. The bits are arranged
 * such that FPSCR bits map to FPCR or FPSR bits in the same bit positions,
 * so we can use appropriate masking to handle FPSCR reads and writes.
 * Note that the FPCR has some bits which are not visible in the
 * AArch32 view (for FEAT_AFP). Writing the FPSCR leaves these unchanged.
 */

/* FPCR bits */
#define FPCR_FIZ         (1 << 0)  /* Flush Inputs to Zero (FEAT_AFP) */
#define FPCR_AH          (1 << 1)  /* Alternate Handling (FEAT_AFP) */
#define FPCR_NEP         (1 << 2)  /* SIMD scalar ops preserve elts (FEAT_AFP) */
#define FPCR_IOE         (1 << 8)  /* Invalid Operation exception trap enable */
#define FPCR_DZE         (1 << 9)  /* Divide by Zero exception trap enable */
#define FPCR_OFE         (1 << 10) /* Overflow exception trap enable */
#define FPCR_UFE         (1 << 11) /* Underflow exception trap enable */
#define FPCR_IXE         (1 << 12) /* Inexact exception trap enable */
#define FPCR_EBF         (1 << 13) /* Extended BFloat16 behaviors */
#define FPCR_IDE         (1 << 15) /* Input Denormal exception trap enable */
#define FPCR_LEN_MASK    (7 << 16) /* LEN, A-profile only */
#define FPCR_FZ16        (1 << 19) /* ARMv8.2+, FP16 flush-to-zero */
#define FPCR_STRIDE_MASK (3 << 20) /* Stride */
#define FPCR_RMODE_MASK  (3 << 22) /* Rounding mode */
#define FPCR_FZ          (1 << 24) /* Flush-to-zero enable bit */
#define FPCR_DN          (1 << 25) /* Default NaN enable bit */
#define FPCR_AHP         (1 << 26) /* Alternative half-precision */

#define FPCR_LTPSIZE_SHIFT  16 /* LTPSIZE, M-profile only */
#define FPCR_LTPSIZE_MASK   (7 << FPCR_LTPSIZE_SHIFT)
#define FPCR_LTPSIZE_LENGTH 3

/* Cumulative exception trap enable bits */
#define FPCR_EEXC_MASK (FPCR_IOE | FPCR_DZE | FPCR_OFE | FPCR_UFE | FPCR_IXE | FPCR_IDE)

/* FPSR bits */
#define FPSR_IOC (1 << 0)  /* Invalid Operation cumulative exception */
#define FPSR_DZC (1 << 1)  /* Divide by Zero cumulative exception */
#define FPSR_OFC (1 << 2)  /* Overflow cumulative exception */
#define FPSR_UFC (1 << 3)  /* Underflow cumulative exception */
#define FPSR_IXC (1 << 4)  /* Inexact cumulative exception */
#define FPSR_IDC (1 << 7)  /* Input Denormal cumulative exception */
#define FPSR_QC  (1 << 27) /* Cumulative saturation bit */
#define FPSR_V   (1 << 28) /* FP overflow flag */
#define FPSR_C   (1 << 29) /* FP carry flag */
#define FPSR_Z   (1 << 30) /* FP zero flag */
#define FPSR_N   (1 << 31) /* FP negative flag */

/* Cumulative exception status bits */
#define FPSR_CEXC_MASK (FPSR_IOC | FPSR_DZC | FPSR_OFC | FPSR_UFC | FPSR_IXC | FPSR_IDC)

#define FPSR_NZCV_MASK   (FPSR_N | FPSR_Z | FPSR_C | FPSR_V)
#define FPSR_NZCVQC_MASK (FPSR_NZCV_MASK | FPSR_QC)

/* A32 FPSCR bits which architecturally map to FPSR bits */
#define FPSCR_FPSR_MASK (FPSR_NZCVQC_MASK | FPSR_CEXC_MASK)
/* A32 FPSCR bits which architecturally map to FPCR bits */
#define FPSCR_FPCR_MASK                                                                                              \
    (FPCR_EEXC_MASK | FPCR_LEN_MASK | FPCR_FZ16 | FPCR_STRIDE_MASK | FPCR_RMODE_MASK | FPCR_FZ | FPCR_DN | FPCR_AHP)
/* These masks don't overlap: each bit lives in only one place */
QEMU_BUILD_BUG_ON(FPSCR_FPSR_MASK& FPSCR_FPCR_MASK);

/**
 * vfp_get_fpsr: read the AArch64 FPSR
 * @env: CPU context
 *
 * Return the current AArch64 FPSR value
 */
uint32_t vfp_get_fpsr(CPUARMState* env);

/**
 * vfp_get_fpcr: read the AArch64 FPCR
 * @env: CPU context
 *
 * Return the current AArch64 FPCR value
 */
uint32_t vfp_get_fpcr(CPUARMState* env);

/**
 * vfp_set_fpsr: write the AArch64 FPSR
 * @env: CPU context
 * @value: new value
 */
void vfp_set_fpsr(CPUARMState* env, uint32_t value);

/**
 * vfp_set_fpcr: write the AArch64 FPCR
 * @env: CPU context
 * @value: new value
 */
void vfp_set_fpcr(CPUARMState* env, uint32_t value);

enum arm_cpu_mode
{
    ARM_CPU_MODE_USR = 0x10,
    ARM_CPU_MODE_FIQ = 0x11,
    ARM_CPU_MODE_IRQ = 0x12,
    ARM_CPU_MODE_SVC = 0x13,
    ARM_CPU_MODE_MON = 0x16,
    ARM_CPU_MODE_ABT = 0x17,
    ARM_CPU_MODE_HYP = 0x1a,
    ARM_CPU_MODE_UND = 0x1b,
    ARM_CPU_MODE_SYS = 0x1f
};

/* VFP system registers.  */
#define ARM_VFP_FPSID   0
#define ARM_VFP_FPSCR   1
#define ARM_VFP_MVFR2   5
#define ARM_VFP_MVFR1   6
#define ARM_VFP_MVFR0   7
#define ARM_VFP_FPEXC   8
#define ARM_VFP_FPINST  9
#define ARM_VFP_FPINST2 10

/* QEMU-internal value meaning "FPSCR, but we care only about NZCV" */
#define QEMU_VFP_FPSCR_NZCV 0xffff

/* iwMMXt coprocessor control registers.  */
#define ARM_IWMMXT_wCID  0
#define ARM_IWMMXT_wCon  1
#define ARM_IWMMXT_wCSSF 2
#define ARM_IWMMXT_wCASF 3
#define ARM_IWMMXT_wCGR0 8
#define ARM_IWMMXT_wCGR1 9
#define ARM_IWMMXT_wCGR2 10
#define ARM_IWMMXT_wCGR3 11

/*
 * System register ID fields.
 */
REG_FIELD(CLIDR_EL1, CTYPE1, 0, 3)
REG_FIELD(CLIDR_EL1, CTYPE2, 3, 3)
REG_FIELD(CLIDR_EL1, CTYPE3, 6, 3)
REG_FIELD(CLIDR_EL1, CTYPE4, 9, 3)
REG_FIELD(CLIDR_EL1, CTYPE5, 12, 3)
REG_FIELD(CLIDR_EL1, CTYPE6, 15, 3)
REG_FIELD(CLIDR_EL1, CTYPE7, 18, 3)
REG_FIELD(CLIDR_EL1, LOUIS, 21, 3)
REG_FIELD(CLIDR_EL1, LOC, 24, 3)
REG_FIELD(CLIDR_EL1, LOUU, 27, 3)
REG_FIELD(CLIDR_EL1, ICB, 30, 3)

/* When FEAT_CCIDX is implemented */
REG_FIELD(CCSIDR_EL1, CCIDX_LINESIZE, 0, 3)
REG_FIELD(CCSIDR_EL1, CCIDX_ASSOCIATIVITY, 3, 21)
REG_FIELD(CCSIDR_EL1, CCIDX_NUMSETS, 32, 24)

/* When FEAT_CCIDX is not implemented */
REG_FIELD(CCSIDR_EL1, LINESIZE, 0, 3)
REG_FIELD(CCSIDR_EL1, ASSOCIATIVITY, 3, 10)
REG_FIELD(CCSIDR_EL1, NUMSETS, 13, 15)

REG_FIELD(CTR_EL0, IMINLINE, 0, 4)
REG_FIELD(CTR_EL0, L1IP, 14, 2)
REG_FIELD(CTR_EL0, DMINLINE, 16, 4)
REG_FIELD(CTR_EL0, ERG, 20, 4)
REG_FIELD(CTR_EL0, CWG, 24, 4)
REG_FIELD(CTR_EL0, IDC, 28, 1)
REG_FIELD(CTR_EL0, DIC, 29, 1)
REG_FIELD(CTR_EL0, TMINLINE, 32, 6)

REG_FIELD(MIDR_EL1, REVISION, 0, 4)
REG_FIELD(MIDR_EL1, PARTNUM, 4, 12)
REG_FIELD(MIDR_EL1, ARCHITECTURE, 16, 4)
REG_FIELD(MIDR_EL1, VARIANT, 20, 4)
REG_FIELD(MIDR_EL1, IMPLEMENTER, 24, 8)

REG_FIELD(ID_ISAR0, SWAP, 0, 4)
REG_FIELD(ID_ISAR0, BITCOUNT, 4, 4)
REG_FIELD(ID_ISAR0, BITFIELD, 8, 4)
REG_FIELD(ID_ISAR0, CMPBRANCH, 12, 4)
REG_FIELD(ID_ISAR0, COPROC, 16, 4)
REG_FIELD(ID_ISAR0, DEBUG, 20, 4)
REG_FIELD(ID_ISAR0, DIVIDE, 24, 4)

REG_FIELD(ID_ISAR1, ENDIAN, 0, 4)
REG_FIELD(ID_ISAR1, EXCEPT, 4, 4)
REG_FIELD(ID_ISAR1, EXCEPT_AR, 8, 4)
REG_FIELD(ID_ISAR1, EXTEND, 12, 4)
REG_FIELD(ID_ISAR1, IFTHEN, 16, 4)
REG_FIELD(ID_ISAR1, IMMEDIATE, 20, 4)
REG_FIELD(ID_ISAR1, INTERWORK, 24, 4)
REG_FIELD(ID_ISAR1, JAZELLE, 28, 4)

REG_FIELD(ID_ISAR2, LOADSTORE, 0, 4)
REG_FIELD(ID_ISAR2, MEMHINT, 4, 4)
REG_FIELD(ID_ISAR2, MULTIACCESSINT, 8, 4)
REG_FIELD(ID_ISAR2, MULT, 12, 4)
REG_FIELD(ID_ISAR2, MULTS, 16, 4)
REG_FIELD(ID_ISAR2, MULTU, 20, 4)
REG_FIELD(ID_ISAR2, PSR_AR, 24, 4)
REG_FIELD(ID_ISAR2, REVERSAL, 28, 4)

REG_FIELD(ID_ISAR3, SATURATE, 0, 4)
REG_FIELD(ID_ISAR3, SIMD, 4, 4)
REG_FIELD(ID_ISAR3, SVC, 8, 4)
REG_FIELD(ID_ISAR3, SYNCHPRIM, 12, 4)
REG_FIELD(ID_ISAR3, TABBRANCH, 16, 4)
REG_FIELD(ID_ISAR3, T32COPY, 20, 4)
REG_FIELD(ID_ISAR3, TRUENOP, 24, 4)
REG_FIELD(ID_ISAR3, T32EE, 28, 4)

REG_FIELD(ID_ISAR4, UNPRIV, 0, 4)
REG_FIELD(ID_ISAR4, WITHSHIFTS, 4, 4)
REG_FIELD(ID_ISAR4, WRITEBACK, 8, 4)
REG_FIELD(ID_ISAR4, SMC, 12, 4)
REG_FIELD(ID_ISAR4, BARRIER, 16, 4)
REG_FIELD(ID_ISAR4, SYNCHPRIM_FRAC, 20, 4)
REG_FIELD(ID_ISAR4, PSR_M, 24, 4)
REG_FIELD(ID_ISAR4, SWP_FRAC, 28, 4)

REG_FIELD(ID_ISAR5, SEVL, 0, 4)
REG_FIELD(ID_ISAR5, AES, 4, 4)
REG_FIELD(ID_ISAR5, SHA1, 8, 4)
REG_FIELD(ID_ISAR5, SHA2, 12, 4)
REG_FIELD(ID_ISAR5, CRC32, 16, 4)
REG_FIELD(ID_ISAR5, RDM, 24, 4)
REG_FIELD(ID_ISAR5, VCMA, 28, 4)

REG_FIELD(ID_ISAR6, JSCVT, 0, 4)
REG_FIELD(ID_ISAR6, DP, 4, 4)
REG_FIELD(ID_ISAR6, FHM, 8, 4)
REG_FIELD(ID_ISAR6, SB, 12, 4)
REG_FIELD(ID_ISAR6, SPECRES, 16, 4)
REG_FIELD(ID_ISAR6, BF16, 20, 4)
REG_FIELD(ID_ISAR6, I8MM, 24, 4)

REG_FIELD(ID_MMFR0, VMSA, 0, 4)
REG_FIELD(ID_MMFR0, PMSA, 4, 4)
REG_FIELD(ID_MMFR0, OUTERSHR, 8, 4)
REG_FIELD(ID_MMFR0, SHARELVL, 12, 4)
REG_FIELD(ID_MMFR0, TCM, 16, 4)
REG_FIELD(ID_MMFR0, AUXREG, 20, 4)
REG_FIELD(ID_MMFR0, FCSE, 24, 4)
REG_FIELD(ID_MMFR0, INNERSHR, 28, 4)

REG_FIELD(ID_MMFR1, L1HVDVA, 0, 4)
REG_FIELD(ID_MMFR1, L1UNIVA, 4, 4)
REG_FIELD(ID_MMFR1, L1HVDSW, 8, 4)
REG_FIELD(ID_MMFR1, L1UNISW, 12, 4)
REG_FIELD(ID_MMFR1, L1HVD, 16, 4)
REG_FIELD(ID_MMFR1, L1UNI, 20, 4)
REG_FIELD(ID_MMFR1, L1TSTCLN, 24, 4)
REG_FIELD(ID_MMFR1, BPRED, 28, 4)

REG_FIELD(ID_MMFR2, L1HVDFG, 0, 4)
REG_FIELD(ID_MMFR2, L1HVDBG, 4, 4)
REG_FIELD(ID_MMFR2, L1HVDRNG, 8, 4)
REG_FIELD(ID_MMFR2, HVDTLB, 12, 4)
REG_FIELD(ID_MMFR2, UNITLB, 16, 4)
REG_FIELD(ID_MMFR2, MEMBARR, 20, 4)
REG_FIELD(ID_MMFR2, WFISTALL, 24, 4)
REG_FIELD(ID_MMFR2, HWACCFLG, 28, 4)

REG_FIELD(ID_MMFR3, CMAINTVA, 0, 4)
REG_FIELD(ID_MMFR3, CMAINTSW, 4, 4)
REG_FIELD(ID_MMFR3, BPMAINT, 8, 4)
REG_FIELD(ID_MMFR3, MAINTBCST, 12, 4)
REG_FIELD(ID_MMFR3, PAN, 16, 4)
REG_FIELD(ID_MMFR3, COHWALK, 20, 4)
REG_FIELD(ID_MMFR3, CMEMSZ, 24, 4)
REG_FIELD(ID_MMFR3, SUPERSEC, 28, 4)

REG_FIELD(ID_MMFR4, SPECSEI, 0, 4)
REG_FIELD(ID_MMFR4, AC2, 4, 4)
REG_FIELD(ID_MMFR4, XNX, 8, 4)
REG_FIELD(ID_MMFR4, CNP, 12, 4)
REG_FIELD(ID_MMFR4, HPDS, 16, 4)
REG_FIELD(ID_MMFR4, LSM, 20, 4)
REG_FIELD(ID_MMFR4, CCIDX, 24, 4)
REG_FIELD(ID_MMFR4, EVT, 28, 4)

REG_FIELD(ID_MMFR5, ETS, 0, 4)
REG_FIELD(ID_MMFR5, NTLBPA, 4, 4)

REG_FIELD(ID_PFR0, STATE0, 0, 4)
REG_FIELD(ID_PFR0, STATE1, 4, 4)
REG_FIELD(ID_PFR0, STATE2, 8, 4)
REG_FIELD(ID_PFR0, STATE3, 12, 4)
REG_FIELD(ID_PFR0, CSV2, 16, 4)
REG_FIELD(ID_PFR0, AMU, 20, 4)
REG_FIELD(ID_PFR0, DIT, 24, 4)
REG_FIELD(ID_PFR0, RAS, 28, 4)

REG_FIELD(ID_PFR1, PROGMOD, 0, 4)
REG_FIELD(ID_PFR1, SECURITY, 4, 4)
REG_FIELD(ID_PFR1, MPROGMOD, 8, 4)
REG_FIELD(ID_PFR1, VIRTUALIZATION, 12, 4)
REG_FIELD(ID_PFR1, GENTIMER, 16, 4)
REG_FIELD(ID_PFR1, SEC_FRAC, 20, 4)
REG_FIELD(ID_PFR1, VIRT_FRAC, 24, 4)

REG_FIELD(ID_PFR2, CSV3, 0, 4)
REG_FIELD(ID_PFR2, SSBS, 4, 4)
REG_FIELD(ID_PFR2, RAS_FRAC, 8, 4)

REG_FIELD(ID_AA64ISAR0, AES, 4, 4)
REG_FIELD(ID_AA64ISAR0, SHA1, 8, 4)
REG_FIELD(ID_AA64ISAR0, SHA2, 12, 4)
REG_FIELD(ID_AA64ISAR0, CRC32, 16, 4)
REG_FIELD(ID_AA64ISAR0, ATOMIC, 20, 4)
REG_FIELD(ID_AA64ISAR0, TME, 24, 4)
REG_FIELD(ID_AA64ISAR0, RDM, 28, 4)
REG_FIELD(ID_AA64ISAR0, SHA3, 32, 4)
REG_FIELD(ID_AA64ISAR0, SM3, 36, 4)
REG_FIELD(ID_AA64ISAR0, SM4, 40, 4)
REG_FIELD(ID_AA64ISAR0, DP, 44, 4)
REG_FIELD(ID_AA64ISAR0, FHM, 48, 4)
REG_FIELD(ID_AA64ISAR0, TS, 52, 4)
REG_FIELD(ID_AA64ISAR0, TLB, 56, 4)
REG_FIELD(ID_AA64ISAR0, RNDR, 60, 4)

REG_FIELD(ID_AA64ISAR1, DPB, 0, 4)
REG_FIELD(ID_AA64ISAR1, APA, 4, 4)
REG_FIELD(ID_AA64ISAR1, API, 8, 4)
REG_FIELD(ID_AA64ISAR1, JSCVT, 12, 4)
REG_FIELD(ID_AA64ISAR1, FCMA, 16, 4)
REG_FIELD(ID_AA64ISAR1, LRCPC, 20, 4)
REG_FIELD(ID_AA64ISAR1, GPA, 24, 4)
REG_FIELD(ID_AA64ISAR1, GPI, 28, 4)
REG_FIELD(ID_AA64ISAR1, FRINTTS, 32, 4)
REG_FIELD(ID_AA64ISAR1, SB, 36, 4)
REG_FIELD(ID_AA64ISAR1, SPECRES, 40, 4)
REG_FIELD(ID_AA64ISAR1, BF16, 44, 4)
REG_FIELD(ID_AA64ISAR1, DGH, 48, 4)
REG_FIELD(ID_AA64ISAR1, I8MM, 52, 4)
REG_FIELD(ID_AA64ISAR1, XS, 56, 4)
REG_FIELD(ID_AA64ISAR1, LS64, 60, 4)

REG_FIELD(ID_AA64ISAR2, WFXT, 0, 4)
REG_FIELD(ID_AA64ISAR2, RPRES, 4, 4)
REG_FIELD(ID_AA64ISAR2, GPA3, 8, 4)
REG_FIELD(ID_AA64ISAR2, APA3, 12, 4)
REG_FIELD(ID_AA64ISAR2, MOPS, 16, 4)
REG_FIELD(ID_AA64ISAR2, BC, 20, 4)
REG_FIELD(ID_AA64ISAR2, PAC_FRAC, 24, 4)
REG_FIELD(ID_AA64ISAR2, CLRBHB, 28, 4)
REG_FIELD(ID_AA64ISAR2, SYSREG_128, 32, 4)
REG_FIELD(ID_AA64ISAR2, SYSINSTR_128, 36, 4)
REG_FIELD(ID_AA64ISAR2, PRFMSLC, 40, 4)
REG_FIELD(ID_AA64ISAR2, RPRFM, 48, 4)
REG_FIELD(ID_AA64ISAR2, CSSC, 52, 4)
REG_FIELD(ID_AA64ISAR2, LUT, 56, 4)
REG_FIELD(ID_AA64ISAR2, ATS1A, 60, 4)

REG_FIELD(ID_AA64PFR0, EL0, 0, 4)
REG_FIELD(ID_AA64PFR0, EL1, 4, 4)
REG_FIELD(ID_AA64PFR0, EL2, 8, 4)
REG_FIELD(ID_AA64PFR0, EL3, 12, 4)
REG_FIELD(ID_AA64PFR0, FP, 16, 4)
REG_FIELD(ID_AA64PFR0, ADVSIMD, 20, 4)
REG_FIELD(ID_AA64PFR0, RAS, 28, 4)
REG_FIELD(ID_AA64PFR0, SVE, 32, 4)
REG_FIELD(ID_AA64PFR0, SEL2, 36, 4)
REG_FIELD(ID_AA64PFR0, MPAM, 40, 4)
REG_FIELD(ID_AA64PFR0, AMU, 44, 4)
REG_FIELD(ID_AA64PFR0, DIT, 48, 4)
REG_FIELD(ID_AA64PFR0, RME, 52, 4)
REG_FIELD(ID_AA64PFR0, CSV2, 56, 4)
REG_FIELD(ID_AA64PFR0, CSV3, 60, 4)

REG_FIELD(ID_AA64PFR1, BT, 0, 4)
REG_FIELD(ID_AA64PFR1, SSBS, 4, 4)
REG_FIELD(ID_AA64PFR1, MTE, 8, 4)
REG_FIELD(ID_AA64PFR1, RAS_FRAC, 12, 4)
REG_FIELD(ID_AA64PFR1, MPAM_FRAC, 16, 4)
REG_FIELD(ID_AA64PFR1, SME, 24, 4)
REG_FIELD(ID_AA64PFR1, RNDR_TRAP, 28, 4)
REG_FIELD(ID_AA64PFR1, CSV2_FRAC, 32, 4)
REG_FIELD(ID_AA64PFR1, NMI, 36, 4)
REG_FIELD(ID_AA64PFR1, MTE_FRAC, 40, 4)
REG_FIELD(ID_AA64PFR1, GCS, 44, 4)
REG_FIELD(ID_AA64PFR1, THE, 48, 4)
REG_FIELD(ID_AA64PFR1, MTEX, 52, 4)
REG_FIELD(ID_AA64PFR1, DF2, 56, 4)
REG_FIELD(ID_AA64PFR1, PFAR, 60, 4)

REG_FIELD(ID_AA64MMFR0, PARANGE, 0, 4)
REG_FIELD(ID_AA64MMFR0, ASIDBITS, 4, 4)
REG_FIELD(ID_AA64MMFR0, BIGEND, 8, 4)
REG_FIELD(ID_AA64MMFR0, SNSMEM, 12, 4)
REG_FIELD(ID_AA64MMFR0, BIGENDEL0, 16, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN16, 20, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN64, 24, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN4, 28, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN16_2, 32, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN64_2, 36, 4)
REG_FIELD(ID_AA64MMFR0, TGRAN4_2, 40, 4)
REG_FIELD(ID_AA64MMFR0, EXS, 44, 4)
REG_FIELD(ID_AA64MMFR0, FGT, 56, 4)
REG_FIELD(ID_AA64MMFR0, ECV, 60, 4)

REG_FIELD(ID_AA64MMFR1, HAFDBS, 0, 4)
REG_FIELD(ID_AA64MMFR1, VMIDBITS, 4, 4)
REG_FIELD(ID_AA64MMFR1, VH, 8, 4)
REG_FIELD(ID_AA64MMFR1, HPDS, 12, 4)
REG_FIELD(ID_AA64MMFR1, LO, 16, 4)
REG_FIELD(ID_AA64MMFR1, PAN, 20, 4)
REG_FIELD(ID_AA64MMFR1, SPECSEI, 24, 4)
REG_FIELD(ID_AA64MMFR1, XNX, 28, 4)
REG_FIELD(ID_AA64MMFR1, TWED, 32, 4)
REG_FIELD(ID_AA64MMFR1, ETS, 36, 4)
REG_FIELD(ID_AA64MMFR1, HCX, 40, 4)
REG_FIELD(ID_AA64MMFR1, AFP, 44, 4)
REG_FIELD(ID_AA64MMFR1, NTLBPA, 48, 4)
REG_FIELD(ID_AA64MMFR1, TIDCP1, 52, 4)
REG_FIELD(ID_AA64MMFR1, CMOW, 56, 4)
REG_FIELD(ID_AA64MMFR1, ECBHB, 60, 4)

REG_FIELD(ID_AA64MMFR2, CNP, 0, 4)
REG_FIELD(ID_AA64MMFR2, UAO, 4, 4)
REG_FIELD(ID_AA64MMFR2, LSM, 8, 4)
REG_FIELD(ID_AA64MMFR2, IESB, 12, 4)
REG_FIELD(ID_AA64MMFR2, VARANGE, 16, 4)
REG_FIELD(ID_AA64MMFR2, CCIDX, 20, 4)
REG_FIELD(ID_AA64MMFR2, NV, 24, 4)
REG_FIELD(ID_AA64MMFR2, ST, 28, 4)
REG_FIELD(ID_AA64MMFR2, AT, 32, 4)
REG_FIELD(ID_AA64MMFR2, IDS, 36, 4)
REG_FIELD(ID_AA64MMFR2, FWB, 40, 4)
REG_FIELD(ID_AA64MMFR2, TTL, 48, 4)
REG_FIELD(ID_AA64MMFR2, BBM, 52, 4)
REG_FIELD(ID_AA64MMFR2, EVT, 56, 4)
REG_FIELD(ID_AA64MMFR2, E0PD, 60, 4)

REG_FIELD(ID_AA64MMFR3, TCRX, 0, 4)
REG_FIELD(ID_AA64MMFR3, SCTLRX, 4, 4)
REG_FIELD(ID_AA64MMFR3, S1PIE, 8, 4)
REG_FIELD(ID_AA64MMFR3, S2PIE, 12, 4)
REG_FIELD(ID_AA64MMFR3, S1POE, 16, 4)
REG_FIELD(ID_AA64MMFR3, S2POE, 20, 4)
REG_FIELD(ID_AA64MMFR3, AIE, 24, 4)
REG_FIELD(ID_AA64MMFR3, MEC, 28, 4)
REG_FIELD(ID_AA64MMFR3, D128, 32, 4)
REG_FIELD(ID_AA64MMFR3, D128_2, 36, 4)
REG_FIELD(ID_AA64MMFR3, SNERR, 40, 4)
REG_FIELD(ID_AA64MMFR3, ANERR, 44, 4)
REG_FIELD(ID_AA64MMFR3, SDERR, 52, 4)
REG_FIELD(ID_AA64MMFR3, ADERR, 56, 4)
REG_FIELD(ID_AA64MMFR3, SPEC_FPACC, 60, 4)

REG_FIELD(ID_AA64DFR0, DEBUGVER, 0, 4)
REG_FIELD(ID_AA64DFR0, TRACEVER, 4, 4)
REG_FIELD(ID_AA64DFR0, PMUVER, 8, 4)
REG_FIELD(ID_AA64DFR0, BRPS, 12, 4)
REG_FIELD(ID_AA64DFR0, PMSS, 16, 4)
REG_FIELD(ID_AA64DFR0, WRPS, 20, 4)
REG_FIELD(ID_AA64DFR0, SEBEP, 24, 4)
REG_FIELD(ID_AA64DFR0, CTX_CMPS, 28, 4)
REG_FIELD(ID_AA64DFR0, PMSVER, 32, 4)
REG_FIELD(ID_AA64DFR0, DOUBLELOCK, 36, 4)
REG_FIELD(ID_AA64DFR0, TRACEFILT, 40, 4)
REG_FIELD(ID_AA64DFR0, TRACEBUFFER, 44, 4)
REG_FIELD(ID_AA64DFR0, MTPMU, 48, 4)
REG_FIELD(ID_AA64DFR0, BRBE, 52, 4)
REG_FIELD(ID_AA64DFR0, EXTTRCBUFF, 56, 4)
REG_FIELD(ID_AA64DFR0, HPMN0, 60, 4)

REG_FIELD(ID_AA64ZFR0, SVEVER, 0, 4)
REG_FIELD(ID_AA64ZFR0, AES, 4, 4)
REG_FIELD(ID_AA64ZFR0, BITPERM, 16, 4)
REG_FIELD(ID_AA64ZFR0, BFLOAT16, 20, 4)
REG_FIELD(ID_AA64ZFR0, B16B16, 24, 4)
REG_FIELD(ID_AA64ZFR0, SHA3, 32, 4)
REG_FIELD(ID_AA64ZFR0, SM4, 40, 4)
REG_FIELD(ID_AA64ZFR0, I8MM, 44, 4)
REG_FIELD(ID_AA64ZFR0, F32MM, 52, 4)
REG_FIELD(ID_AA64ZFR0, F64MM, 56, 4)

REG_FIELD(ID_AA64SMFR0, F32F32, 32, 1)
REG_FIELD(ID_AA64SMFR0, BI32I32, 33, 1)
REG_FIELD(ID_AA64SMFR0, B16F32, 34, 1)
REG_FIELD(ID_AA64SMFR0, F16F32, 35, 1)
REG_FIELD(ID_AA64SMFR0, I8I32, 36, 4)
REG_FIELD(ID_AA64SMFR0, F16F16, 42, 1)
REG_FIELD(ID_AA64SMFR0, B16B16, 43, 1)
REG_FIELD(ID_AA64SMFR0, I16I32, 44, 4)
REG_FIELD(ID_AA64SMFR0, F64F64, 48, 1)
REG_FIELD(ID_AA64SMFR0, I16I64, 52, 4)
REG_FIELD(ID_AA64SMFR0, SMEVER, 56, 4)
REG_FIELD(ID_AA64SMFR0, FA64, 63, 1)

REG_FIELD(ID_DFR0, COPDBG, 0, 4)
REG_FIELD(ID_DFR0, COPSDBG, 4, 4)
REG_FIELD(ID_DFR0, MMAPDBG, 8, 4)
REG_FIELD(ID_DFR0, COPTRC, 12, 4)
REG_FIELD(ID_DFR0, MMAPTRC, 16, 4)
REG_FIELD(ID_DFR0, MPROFDBG, 20, 4)
REG_FIELD(ID_DFR0, PERFMON, 24, 4)
REG_FIELD(ID_DFR0, TRACEFILT, 28, 4)

REG_FIELD(ID_DFR1, MTPMU, 0, 4)
REG_FIELD(ID_DFR1, HPMN0, 4, 4)

REG_FIELD(DBGDIDR, SE_IMP, 12, 1)
REG_FIELD(DBGDIDR, NSUHD_IMP, 14, 1)
REG_FIELD(DBGDIDR, VERSION, 16, 4)
REG_FIELD(DBGDIDR, CTX_CMPS, 20, 4)
REG_FIELD(DBGDIDR, BRPS, 24, 4)
REG_FIELD(DBGDIDR, WRPS, 28, 4)

REG_FIELD(DBGDEVID, PCSAMPLE, 0, 4)
REG_FIELD(DBGDEVID, WPADDRMASK, 4, 4)
REG_FIELD(DBGDEVID, BPADDRMASK, 8, 4)
REG_FIELD(DBGDEVID, VECTORCATCH, 12, 4)
REG_FIELD(DBGDEVID, VIRTEXTNS, 16, 4)
REG_FIELD(DBGDEVID, DOUBLELOCK, 20, 4)
REG_FIELD(DBGDEVID, AUXREGS, 24, 4)
REG_FIELD(DBGDEVID, CIDMASK, 28, 4)

REG_FIELD(DBGDEVID1, PCSROFFSET, 0, 4)

REG_FIELD(MVFR0, SIMDREG, 0, 4)
REG_FIELD(MVFR0, FPSP, 4, 4)
REG_FIELD(MVFR0, FPDP, 8, 4)
REG_FIELD(MVFR0, FPTRAP, 12, 4)
REG_FIELD(MVFR0, FPDIVIDE, 16, 4)
REG_FIELD(MVFR0, FPSQRT, 20, 4)
REG_FIELD(MVFR0, FPSHVEC, 24, 4)
REG_FIELD(MVFR0, FPROUND, 28, 4)

REG_FIELD(MVFR1, FPFTZ, 0, 4)
REG_FIELD(MVFR1, FPDNAN, 4, 4)
REG_FIELD(MVFR1, SIMDLS, 8, 4)   /* A-profile only */
REG_FIELD(MVFR1, SIMDINT, 12, 4) /* A-profile only */
REG_FIELD(MVFR1, SIMDSP, 16, 4)  /* A-profile only */
REG_FIELD(MVFR1, SIMDHP, 20, 4)  /* A-profile only */
REG_FIELD(MVFR1, FPHP, 24, 4)
REG_FIELD(MVFR1, SIMDFMAC, 28, 4)

REG_FIELD(MVFR2, SIMDMISC, 0, 4)
REG_FIELD(MVFR2, FPMISC, 4, 4)

REG_FIELD(GPCCR, PPS, 0, 3)
REG_FIELD(GPCCR, IRGN, 8, 2)
REG_FIELD(GPCCR, ORGN, 10, 2)
REG_FIELD(GPCCR, SH, 12, 2)
REG_FIELD(GPCCR, PGS, 14, 2)
REG_FIELD(GPCCR, GPC, 16, 1)
REG_FIELD(GPCCR, GPCP, 17, 1)
REG_FIELD(GPCCR, L0GPTSZ, 20, 4)

REG_FIELD(MFAR, FPA, 12, 40)
REG_FIELD(MFAR, NSE, 62, 1)
REG_FIELD(MFAR, NS, 63, 1)

enum arm_features
{
    ARM_FEATURE_AUXCR,  /* ARM1026 Auxiliary control register.  */
    ARM_FEATURE_IWMMXT, /* Intel iwMMXt extension.  */
    ARM_FEATURE_V6,
    ARM_FEATURE_V6K,
    ARM_FEATURE_V7,
    ARM_FEATURE_THUMB2,
    ARM_FEATURE_NEON,
    ARM_FEATURE_OMAPCP, /* OMAP specific CP15 ops handling.  */
    ARM_FEATURE_THUMB2EE,
    ARM_FEATURE_V7MP, /* v7 Multiprocessing Extensions */
    ARM_FEATURE_V7VE, /* v7 Virtualization Extensions (non-EL2 parts) */
    ARM_FEATURE_V4T,
    ARM_FEATURE_V5,
    ARM_FEATURE_STRONGARM,
    ARM_FEATURE_VAPA, /* cp15 VA to PA lookups */
    ARM_FEATURE_GENERIC_TIMER,
    ARM_FEATURE_MVFR,             /* Media and VFP Feature Registers 0 and 1 */
    ARM_FEATURE_DUMMY_C15_REGS,   /* RAZ/WI all of cp15 crn=15 */
    ARM_FEATURE_CACHE_TEST_CLEAN, /* 926/1026 style test-and-clean ops */
    ARM_FEATURE_CACHE_DIRTY_REG,  /* 1136/1176 cache dirty status register */
    ARM_FEATURE_CACHE_BLOCK_OPS,  /* v6 optional cache block operations */
    ARM_FEATURE_MPIDR,            /* has cp15 MPIDR */
    ARM_FEATURE_LPAE,             /* has Large Physical Address Extension */
    ARM_FEATURE_V8,
    ARM_FEATURE_AARCH64,   /* supports 64 bit mode */
    ARM_FEATURE_CBAR,      /* has cp15 CBAR */
    ARM_FEATURE_CBAR_RO,   /* has cp15 CBAR and it is read-only */
    ARM_FEATURE_EL2,       /* has EL2 Virtualization support */
    ARM_FEATURE_EL3,       /* has EL3 Secure monitor support */
    ARM_FEATURE_THUMB_DSP, /* DSP insns supported in the Thumb encodings */
    ARM_FEATURE_PMU,       /* has PMU support */
    ARM_FEATURE_VBAR,      /* has cp15 VBAR */
    ARM_FEATURE_GXF,       /* has Apple's GXF support */
};

static inline int arm_feature(CPUARMState* env, int feature) { return (env->features & BIT_ULL(feature)) != 0; }

void arm_cpu_finalize_features(ARMCPU* cpu, Error** errp);

/*
 * ARM v9 security states.
 * The ordering of the enumeration corresponds to the low 2 bits
 * of the GPI value, and (except for Root) the concat of NSE:NS.
 */

typedef enum ARMSecuritySpace
{
    ARMSS_Secure    = 0,
    ARMSS_NonSecure = 1,
    ARMSS_Root      = 2,
    ARMSS_Realm     = 3,
} ARMSecuritySpace;

/* Return true if @space is secure, in the pre-v9 sense. */
static inline bool arm_space_is_secure(ARMSecuritySpace space) { return space == ARMSS_Secure || space == ARMSS_Root; }

/* Return the ARMSecuritySpace for @secure, assuming !RME or EL[0-2]. */
static inline ARMSecuritySpace arm_secure_to_space(bool secure) { return secure ? ARMSS_Secure : ARMSS_NonSecure; }

/**
 * arm_security_space_below_el3:
 * @env: cpu context
 *
 * Return the security space of exception levels below EL3, following
 * an exception return to those levels.  Unlike arm_security_space,
 * this doesn't care about the current EL.
 */
ARMSecuritySpace arm_security_space_below_el3(CPUARMState* env);

/**
 * arm_is_secure_below_el3:
 * @env: cpu context
 *
 * Return true if exception levels below EL3 are in secure state,
 * or would be following an exception return to those levels.
 */
static inline bool arm_is_secure_below_el3(CPUARMState* env)
{
    ARMSecuritySpace ss = arm_security_space_below_el3(env);
    return ss == ARMSS_Secure;
}

/* Return true if the CPU is AArch64 EL3 or AArch32 Mon */
static inline bool arm_is_el3_or_mon(CPUARMState* env)
{
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        if (is_a64(env) && extract32(env->pstate, 2, 2) == 3) {
            /* CPU currently in AArch64 state and EL3 */
            return true;
        }
        else if (!is_a64(env) && (env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON) {
            /* CPU currently in AArch32 state and monitor mode */
            return true;
        }
    }
    return false;
}

/**
 * arm_security_space:
 * @env: cpu context
 *
 * Return the current security space of the cpu.
 */
ARMSecuritySpace arm_security_space(CPUARMState* env);

/**
 * arm_is_secure:
 * @env: cpu context
 *
 * Return true if the processor is in secure state.
 */
static inline bool arm_is_secure(CPUARMState* env) { return arm_space_is_secure(arm_security_space(env)); }

/*
 * Return true if the current security state has AArch64 EL2 or AArch32 Hyp.
 * This corresponds to the pseudocode EL2Enabled().
 */
static inline bool arm_is_el2_enabled_secstate(CPUARMState* env, ARMSecuritySpace space)
{
    assert(space != ARMSS_Root);
    return arm_feature(env, ARM_FEATURE_EL2) && (space != ARMSS_Secure || (env->cp15.scr_el3 & SCR_EEL2));
}

static inline bool arm_is_el2_enabled(CPUARMState* env)
{ return arm_feature(env, ARM_FEATURE_EL2) && arm_is_el2_enabled_secstate(env, arm_security_space_below_el3(env)); }

/**
 * arm_hcr_el2_eff(): Return the effective value of HCR_EL2.
 * E.g. when in secure state, fields in HCR_EL2 are suppressed,
 * "for all purposes other than a direct read or write access of HCR_EL2."
 * Not included here is HCR_RW.
 */
uint64_t arm_hcr_el2_eff_secstate(CPUARMState* env, ARMSecuritySpace space);
uint64_t arm_hcr_el2_eff(CPUARMState* env);
uint64_t arm_hcrx_el2_eff(CPUARMState* env);

/*
 * Function for determining whether guest cp register reads and writes should
 * access the secure or non-secure bank of a cp register.  When EL3 is
 * operating in AArch32 state, the NS-bit determines whether the secure
 * instance of a cp register should be used. When EL3 is AArch64 (or if
 * it doesn't exist at all) then there is no register banking, and all
 * accesses are to the non-secure version.
 */
bool access_secure_reg(CPUARMState* env);

uint32_t arm_phys_excp_target_el(CPUState* cs, uint32_t excp_idx, uint32_t cur_el, bool secure);

/* Return the highest implemented Exception Level */
static inline int arm_highest_el(CPUARMState* env)
{
    if (arm_feature(env, ARM_FEATURE_EL3)) { return 3; }
    if (arm_feature(env, ARM_FEATURE_EL2)) { return 2; }
    return 1;
}

/**
 * write_list_to_cpustate
 * @cpu: ARMCPU
 *
 * For each register listed in the ARMCPU cpreg_indexes list, write
 * its value from the cpreg_values list into the ARMCPUState structure.
 * This updates TCG's working data structures from KVM data or
 * from incoming migration state.
 *
 * Returns: true if all register values were updated correctly,
 * false if some register was unknown or could not be written.
 * Note that we do not stop early on failure -- we will attempt
 * writing all registers in the list.
 */
bool write_list_to_cpustate(ARMCPU* cpu);

/**
 * write_cpustate_to_list:
 * @cpu: ARMCPU
 * @kvm_sync: true if this is for syncing back to KVM
 *
 * For each register listed in the ARMCPU cpreg_indexes list, write
 * its value from the ARMCPUState structure into the cpreg_values list.
 * This is used to copy info from TCG's working data structures into
 * KVM or for outbound migration.
 *
 * @kvm_sync is true if we are doing this in order to sync the
 * register state back to KVM. In this case we will only update
 * values in the list if the previous list->cpustate sync actually
 * successfully wrote the CPU state. Otherwise we will keep the value
 * that is in the list.
 *
 * Returns: true if all register values were read correctly,
 * false if some register was unknown or could not be read.
 * Note that we do not stop early on failure -- we will attempt
 * reading all registers in the list.
 */
bool write_cpustate_to_list(ARMCPU* cpu, bool kvm_sync);

#define ARM_CPUID_TI915T 0x54029152
#define ARM_CPUID_TI925T 0x54029252

#define CPU_RESOLVING_TYPE TYPE_ARM_CPU

#define TYPE_ARM_HOST_CPU "host-" TYPE_ARM_CPU

/* Indexes used when registering address spaces with cpu_address_space_init */
typedef enum ARMASIdx
{
    ARMASIdx_NS    = 0,
    ARMASIdx_S     = 1,
    ARMASIdx_TagNS = 2,
    ARMASIdx_TagS  = 3,
} ARMASIdx;

static inline ARMMMUIdx arm_space_to_phys(ARMSecuritySpace space)
{
    /* Assert the relative order of the physical mmu indexes. */
    QEMU_BUILD_BUG_ON(ARMSS_Secure != 0);
    QEMU_BUILD_BUG_ON(ARMMMUIdx_Phys_NS != ARMMMUIdx_Phys_S + ARMSS_NonSecure);
    QEMU_BUILD_BUG_ON(ARMMMUIdx_Phys_Root != ARMMMUIdx_Phys_S + ARMSS_Root);
    QEMU_BUILD_BUG_ON(ARMMMUIdx_Phys_Realm != ARMMMUIdx_Phys_S + ARMSS_Realm);

    return ARMMMUIdx_Phys_S + space;
}

static inline ARMSecuritySpace arm_phys_to_space(ARMMMUIdx idx)
{
    assert(idx >= ARMMMUIdx_Phys_S && idx <= ARMMMUIdx_Phys_Realm);
    return idx - ARMMMUIdx_Phys_S;
}

static inline bool arm_sctlr_b(CPUARMState* env)
{ return !arm_feature(env, ARM_FEATURE_V7) && (env->cp15.sctlr_el[1] & SCTLR_B) != 0; }

uint64_t arm_sctlr(CPUARMState* env, int el);

/*
 * We have more than 32-bits worth of state per TB, so we split the data
 * between tb->flags and tb->cs_base, which is otherwise unused for ARM.
 * We collect these two parts in CPUARMTBFlags where they are named
 * flags and flags2 respectively.
 *
 * The flags that are shared between all execution modes, TBFLAG_ANY, are stored
 * in flags. The flags that are specific to a given mode are stored in flags2.
 * flags2 always has 64-bits, even though only 32-bits are used for A32.
 *
 * Unless otherwise noted, these bits are cached in env->hflags.
 */
REG_FIELD(TBFLAG_ANY, AARCH64_STATE, 0, 1)
REG_FIELD(TBFLAG_ANY, SS_ACTIVE, 1, 1)
REG_FIELD(TBFLAG_ANY, PSTATE__SS, 2, 1) /* Not cached. */
REG_FIELD(TBFLAG_ANY, BE_DATA, 3, 1)
REG_FIELD(TBFLAG_ANY, MMUIDX, 4, 5)
/* Target EL if we take a floating-point-disabled exception */
REG_FIELD(TBFLAG_ANY, FPEXC_EL, 9, 2)
/* Memory operations require alignment: SCTLR_ELx.A or CCR.UNALIGN_TRP */
REG_FIELD(TBFLAG_ANY, ALIGN_MEM, 11, 1)
REG_FIELD(TBFLAG_ANY, PSTATE__IL, 12, 1)
REG_FIELD(TBFLAG_ANY, FGT_ACTIVE, 13, 1)
REG_FIELD(TBFLAG_ANY, FGT_SVC, 14, 1)

/*
 * Bit usage when in AArch32 state, both A- and M-profile.
 */
REG_FIELD(TBFLAG_A32, CONDEXEC, 24, 8) /* Not cached. */
REG_FIELD(TBFLAG_A32, THUMB, 23, 1)    /* Not cached. */

/*
 * Bit usage when in AArch32 state, for A-profile only.
 */
REG_FIELD(TBFLAG_A32, VECLEN, 0, 3)    /* Not cached. */
REG_FIELD(TBFLAG_A32, VECSTRIDE, 3, 2) /* Not cached. */
REG_FIELD(TBFLAG_A32, VFPEN, 7, 1)     /* Partially cached, minus FPEXC. */
REG_FIELD(TBFLAG_A32, SCTLR__B, 8, 1)  /* Cannot overlap with SCTLR_B */
REG_FIELD(TBFLAG_A32, HSTR_ACTIVE, 9, 1)
/*
 * Indicates whether cp register reads and writes by guest code should access
 * the secure or nonsecure bank of banked registers; note that this is not
 * the same thing as the current security state of the processor!
 */
REG_FIELD(TBFLAG_A32, NS, 10, 1)
/*
 * Indicates that SME Streaming mode is active, and SMCR_ELx.FA64 is not.
 * This requires an SME trap from AArch32 mode when using NEON.
 */
REG_FIELD(TBFLAG_A32, SME_TRAP_NONSTREAMING, 11, 1)

/*
 * Bit usage when in AArch64 state
 */
REG_FIELD(TBFLAG_A64, TBII, 0, 2)
REG_FIELD(TBFLAG_A64, SVEEXC_EL, 2, 2)
/* The current vector length, either NVL or SVL. */
REG_FIELD(TBFLAG_A64, VL, 4, 4)
REG_FIELD(TBFLAG_A64, PAUTH_ACTIVE, 8, 1)
REG_FIELD(TBFLAG_A64, BT, 9, 1)
REG_FIELD(TBFLAG_A64, BTYPE, 10, 2) /* Not cached. */
REG_FIELD(TBFLAG_A64, TBID, 12, 2)
REG_FIELD(TBFLAG_A64, UNPRIV, 14, 1)
REG_FIELD(TBFLAG_A64, ATA, 15, 1)
REG_FIELD(TBFLAG_A64, TCMA, 16, 2)
REG_FIELD(TBFLAG_A64, MTE_ACTIVE, 18, 1)
REG_FIELD(TBFLAG_A64, MTE0_ACTIVE, 19, 1)
REG_FIELD(TBFLAG_A64, SMEEXC_EL, 20, 2)
REG_FIELD(TBFLAG_A64, PSTATE_SM, 22, 1)
REG_FIELD(TBFLAG_A64, PSTATE_ZA, 23, 1)
REG_FIELD(TBFLAG_A64, SVL, 24, 4)
/* Indicates that SME Streaming mode is active, and SMCR_ELx.FA64 is not. */
REG_FIELD(TBFLAG_A64, SME_TRAP_NONSTREAMING, 28, 1)
REG_FIELD(TBFLAG_A64, TRAP_ERET, 29, 1)
REG_FIELD(TBFLAG_A64, NAA, 30, 1)
REG_FIELD(TBFLAG_A64, ATA0, 31, 1)
REG_FIELD(TBFLAG_A64, NV, 32, 1)
REG_FIELD(TBFLAG_A64, NV1, 33, 1)
REG_FIELD(TBFLAG_A64, NV2, 34, 1)
/* Set if FEAT_NV2 RAM accesses use the EL2&0 translation regime */
REG_FIELD(TBFLAG_A64, NV2_MEM_E20, 35, 1)
/* Set if FEAT_NV2 RAM accesses are big-endian */
REG_FIELD(TBFLAG_A64, NV2_MEM_BE, 36, 1)
REG_FIELD(TBFLAG_A64, AH, 37, 1)  /* FPCR.AH */
REG_FIELD(TBFLAG_A64, NEP, 38, 1) /* FPCR.NEP */
REG_FIELD(TBFLAG_A64, ZT0EXC_EL, 39, 2)

/*
 * Helpers for using the above. Note that only the A64 accessors use
 * FIELD_DP64() and FIELD_EX64(), because in the other cases the flags
 * word either is or might be 32 bits only.
 */
#define DP_TBFLAG_ANY(DST, WHICH, VAL) (DST.flags = REG_FIELD_DP32(DST.flags, TBFLAG_ANY, WHICH, VAL))
#define DP_TBFLAG_A64(DST, WHICH, VAL) (DST.flags2 = REG_FIELD_DP64(DST.flags2, TBFLAG_A64, WHICH, VAL))
#define DP_TBFLAG_A32(DST, WHICH, VAL) (DST.flags2 = REG_FIELD_DP32(DST.flags2, TBFLAG_A32, WHICH, VAL))

#define EX_TBFLAG_ANY(IN, WHICH) REG_FIELD_EX32(IN.flags, TBFLAG_ANY, WHICH)
#define EX_TBFLAG_A64(IN, WHICH) REG_FIELD_EX64(IN.flags2, TBFLAG_A64, WHICH)
#define EX_TBFLAG_A32(IN, WHICH) REG_FIELD_EX32(IN.flags2, TBFLAG_A32, WHICH)

/**
 * sve_vq
 * @env: the cpu context
 *
 * Return the VL cached within env->hflags, in units of quadwords.
 */
static inline int sve_vq(CPUARMState* env) { return EX_TBFLAG_A64(env->hflags, VL) + 1; }

/**
 * sme_vq
 * @env: the cpu context
 *
 * Return the SVL cached within env->hflags, in units of quadwords.
 */
static inline int sme_vq(CPUARMState* env) { return EX_TBFLAG_A64(env->hflags, SVL) + 1; }

/* Return the address space index to use for a memory access */
static inline int arm_asidx_from_attrs(CPUState* cs, MemTxAttrs attrs)
{ return attrs.secure ? ARMASIdx_S : ARMASIdx_NS; }

/* Return the AddressSpace to use for a memory access
 * (which depends on whether the access is S or NS, and whether
 * the board gave us a separate AddressSpace for S accesses).
 */
static inline AddressSpace* arm_addressspace(CPUState* cs, MemTxAttrs attrs)
{ return cpu_get_address_space(cs, arm_asidx_from_attrs(cs, attrs)); }

/**
 * arm_register_pre_el_change_hook:
 * Register a hook function which will be called immediately before this
 * CPU changes exception level or mode. The hook function will be
 * passed a pointer to the ARMCPU and the opaque data pointer passed
 * to this function when the hook was registered.
 *
 * Note that if a pre-change hook is called, any registered post-change hooks
 * are guaranteed to subsequently be called.
 */
void arm_register_pre_el_change_hook(ARMCPU* cpu, ARMELChangeHookFn* hook, void* opaque);
/**
 * arm_register_el_change_hook:
 * Register a hook function which will be called immediately after this
 * CPU changes exception level or mode. The hook function will be
 * passed a pointer to the ARMCPU and the opaque data pointer passed
 * to this function when the hook was registered.
 *
 * Note that any registered hooks registered here are guaranteed to be called
 * if pre-change hooks have been.
 */
void arm_register_el_change_hook(ARMCPU* cpu, ARMELChangeHookFn* hook, void* opaque);

/**
 * arm_rebuild_hflags:
 * Rebuild the cached TBFLAGS for arbitrary changed processor state.
 */
void arm_rebuild_hflags(CPUARMState* env);

/**
 * aa32_vfp_dreg:
 * Return a pointer to the Dn register within env in 32-bit mode.
 */
static inline uint64_t* aa32_vfp_dreg(CPUARMState* env, unsigned regno)
{ return &env->vfp.zregs[regno >> 1].d[regno & 1]; }

/**
 * aa32_vfp_qreg:
 * Return a pointer to the Qn register within env in 32-bit mode.
 */
static inline uint64_t* aa32_vfp_qreg(CPUARMState* env, unsigned regno) { return &env->vfp.zregs[regno].d[0]; }

/**
 * aa64_vfp_qreg:
 * Return a pointer to the Qn register within env in 64-bit mode.
 */
static inline uint64_t* aa64_vfp_qreg(CPUARMState* env, unsigned regno) { return &env->vfp.zregs[regno].d[0]; }

/* Shared between translate-sve.c and sve_helper.c.  */
extern const uint64_t pred_esz_masks[5];

/*
 * AArch64 usage of the PAGE_TARGET_* bits for linux-user.
 * Note that with the Linux kernel, PROT_MTE may not be cleared by mprotect
 * mprotect but PROT_BTI may be cleared.  C.f. the kernel's VM_ARCH_CLEAR.
 */
#define PAGE_BTI           PAGE_TARGET_1
#define PAGE_MTE           PAGE_TARGET_2
#define PAGE_TARGET_STICKY PAGE_MTE

/* We associate one allocation tag per 16 bytes, the minimum.  */
#define LOG2_TAG_GRANULE 4
#define TAG_GRANULE      (1 << LOG2_TAG_GRANULE)
