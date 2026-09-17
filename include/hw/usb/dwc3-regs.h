/**
 * core.h - DesignWare USB3 DRD Core Header
 * linux commit 7bc5a6ba369217e0137833f5955cf0b0f08b0712 before
 * the switch to GPLv2 only
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *      Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

/* Global constants */
#define EP0_BOUNCE_SIZE    512
#define XHCI_RESOURCES_NUM 2

#define EVENT_SIZE         4  /* bytes */
#define EVENT_MAX_NUM      64 /* 2 events/endpoint */
#define EVENT_BUFFERS_SIZE (EVENT_SIZE * EVENT_MAX_NUM)
#define EVENT_TYPE_MASK    0xfe

#define EVENT_TYPE_DEV    0
#define EVENT_TYPE_CARKIT 3
#define EVENT_TYPE_I2C    4

#define DEVICE_EVENT_DISCONNECT         0
#define DEVICE_EVENT_RESET              1
#define DEVICE_EVENT_CONNECT_DONE       2
#define DEVICE_EVENT_LINK_STATUS_CHANGE 3
#define DEVICE_EVENT_WAKEUP             4
#define DEVICE_EVENT_HIBER_REQ          5
#define DEVICE_EVENT_EOPF               6
#define DEVICE_EVENT_SOF                7
#define DEVICE_EVENT_ERRATIC_ERROR      9
#define DEVICE_EVENT_CMD_CMPL           10
#define DEVICE_EVENT_OVERFLOW           11
#define DEVT_VNDRDEVTSTRCVED            0x0c

/* DWC3 registers memory space boundries */
#define XHCI_REGS_START    0x0
#define XHCI_REGS_END      0x7fff
#define GLOBALS_REGS_START 0xc100
#define GLOBALS_REGS_END   0xc6ff
#define DEVICE_REGS_START  0xc700
#define DEVICE_REGS_END    0xc7ff
#define DEPCMD_REGS_START  0xc800
#define DEPCMD_REGS_END    0xcbff
#define OTG_REGS_START     0xcc00
#define OTG_REGS_END       0xccff

/* Global Registers */
#define GSBUSCFG0     0xc100
#define GSBUSCFG1     0xc104
#define GTXTHRCFG     0xc108
#define GRXTHRCFG     0xc10c
#define GCTL          0xc110
#define GEVTEN        0xc114
#define GSTS          0xc118
#define GUCTL1        0xc11c
#define GSNPSID       0xc120
#define GGPIO         0xc124
#define GUID          0xc128
#define GUCTL         0xc12c
#define GBUSERRADDR0  0xc130
#define GBUSERRADDR1  0xc134
#define GPRTBIMAP0    0xc138
#define GPRTBIMAP1    0xc13c
#define GHWPARAMS0    0xc140
#define GHWPARAMS1    0xc144
#define GHWPARAMS2    0xc148
#define GHWPARAMS3    0xc14c
#define GHWPARAMS4    0xc150
#define GHWPARAMS5    0xc154
#define GHWPARAMS6    0xc158
#define GHWPARAMS7    0xc15c
#define GDBGFIFOSPACE 0xc160
#define GDBGLTSSM     0xc164
#define GPRTBIMAP_HS0 0xc180
#define GPRTBIMAP_HS1 0xc184
#define GPRTBIMAP_FS0 0xc188
#define GPRTBIMAP_FS1 0xc18c

#define GUSB2PHYCFG(n) (0xc200 + (n * 0x04))
#define GUSB2I2CCTL(n) (0xc240 + (n * 0x04))

#define GUSB2PHYACC(n) (0xc280 + (n * 0x04))

#define GUSB3PIPECTL(n) (0xc2c0 + (n * 0x04))

#define GTXFIFOSIZ(n) (0xc300 + (n * 0x04))
#define GRXFIFOSIZ(n) (0xc380 + (n * 0x04))

#define GEVNTADRLO(n) (0xc400 + (n * 0x10))
#define GEVNTADRHI(n) (0xc404 + (n * 0x10))
#define GEVNTSIZ(n)   (0xc408 + (n * 0x10))
#define GEVNTCOUNT(n) (0xc40c + (n * 0x10))

#define GHWPARAMS8 0xc600

/* Device Registers */
#define DCFG     0xc700
#define DCTL     0xc704
#define DEVTEN   0xc708
#define DSTS     0xc70c
#define DGCMDPAR 0xc710
#define DGCMD    0xc714
#define DALEPENA 0xc720

/* Device Endpoint Command Registers */
#define DEPCMDPAR2(n) (0xc800 + (n * 0x10))
#define DEPCMDPAR1(n) (0xc804 + (n * 0x10))
#define DEPCMDPAR0(n) (0xc808 + (n * 0x10))
#define DEPCMD(n)     (0xc80c + (n * 0x10))

/* OTG Registers */
#define OCFG   0xcc00
#define OCTL   0xcc04
#define OEVT   0xcc08
#define OEVTEN 0xcc0C
#define OSTS   0xcc10

/* Bit fields */

/* Global Configuration Register */
#define GCTL_PWRDNSCALE(n) ((n) << 19)
#define GCTL_U2RSTECN      BIT(16)
#define GCTL_RAMCLKSEL(x)  (((x) & GCTL_CLK_MASK) << 6)
#define GCTL_CLK_BUS       (0)
#define GCTL_CLK_PIPE      (1)
#define GCTL_CLK_PIPEHALF  (2)
#define GCTL_CLK_MASK      (3)

#define GCTL_PRTCAP(n)     (((n) & (3 << 12)) >> 12)
#define GCTL_PRTCAPDIR(n)  ((n) << 12)
#define GCTL_PRTCAP_HOST   1
#define GCTL_PRTCAP_DEVICE 2
#define GCTL_PRTCAP_OTG    3

#define GCTL_CORESOFTRESET    BIT(11)
#define GCTL_SCALEDOWN(n)     ((n) << 4)
#define GCTL_SCALEDOWN_MASK   GCTL_SCALEDOWN(3)
#define GCTL_DISSCRAMBLE      BIT(3)
#define GCTL_U2EXIT_LFPS      BIT(2)
#define GCTL_GBLHIBERNATIONEN BIT(1)
#define GCTL_DSBLCLKGTNG      BIT(0)

/* Global Status Register */
#define GSTS_OTG_IP           BIT(10)
#define GSTS_BC_IP            BIT(9)
#define GSTS_ADP_IP           BIT(8)
#define GSTS_HOST_IP          BIT(7)
#define GSTS_DEVICE_IP        BIT(6)
#define GSTS_CSR_TIMEOUT      BIT(5)
#define GSTS_BUS_ERR_ADDR_VLD BIT(4)
#define GSTS_CURMOD_MASK      (0x3)
#define GSTS_CURMOD(_v)       ((_v) & GSTS_CURMOD_MASK)
#define GSTS_CURMOD_DEVICE    0
#define GSTS_CURMOD_HOST      1
#define GSTS_CURMOD_DRD       2

/* Global Core ID */
#define GSNPSID_MASK  0xffff0000
#define GSNPSREV_MASK 0xffff

#define GSNPSID_REVISION_173A 0x5533173a
#define GSNPSID_REVISION_175A 0x5533175a
#define GSNPSID_REVISION_180A 0x5533180a
#define GSNPSID_REVISION_183A 0x5533183a
#define GSNPSID_REVISION_185A 0x5533185a
#define GSNPSID_REVISION_187A 0x5533187a
#define GSNPSID_REVISION_188A 0x5533188a
#define GSNPSID_REVISION_190A 0x5533190a
#define GSNPSID_REVISION_194A 0x5533194a
#define GSNPSID_REVISION_200A 0x5533200a
#define GSNPSID_REVISION_202A 0x5533202a
#define GSNPSID_REVISION_210A 0x5533210a
#define GSNPSID_REVISION_220A 0x5533220a
#define GSNPSID_REVISION_230A 0x5533230a
#define GSNPSID_REVISION_240A 0x5533240a
#define GSNPSID_REVISION_250A 0x5533250a

/* Global USB2 PHY Configuration Register */
#define GUSB2PHYCFG_PHYSOFTRST        BIT(31)
#define GUSB2PHYCFG_U2_FREECLK_EXISTS BIT(30)
#define GUSB2PHYCFG_ULPIEXTVBUSDRV    BIT(17)
#define GUSB2PHYCFG_SUSPHY            BIT(6)
#define GUSB2PHYCFG_ULPI_UTMI         BIT(4)
#define GUSB2PHYCFG_ENBLSLPM          BIT(8)
#define GUSB2PHYCFG_PHYIF(n)          (n << 3)
#define GUSB2PHYCFG_PHYIF_MASK        GUSB2PHYCFG_PHYIF(1)
#define GUSB2PHYCFG_USBTRDTIM(n)      (n << 10)
#define GUSB2PHYCFG_USBTRDTIM_MASK    GUSB2PHYCFG_USBTRDTIM(0xf)
#define USBTRDTIM_UTMI_8_BIT          9
#define USBTRDTIM_UTMI_16_BIT         5
#define UTMI_PHYIF_16_BIT             1
#define UTMI_PHYIF_8_BIT              0

/* Global USB2 PHY Vendor Control Register */
#define GUSB2PHYACC_NEWREGREQ      BIT(25)
#define GUSB2PHYACC_DONE           BIT(24)
#define GUSB2PHYACC_BUSY           BIT(23)
#define GUSB2PHYACC_WRITE          BIT(22)
#define GUSB2PHYACC_ADDR(n)        (n << 16)
#define GUSB2PHYACC_EXTEND_ADDR(n) (n << 8)
#define GUSB2PHYACC_DATA(n)        (n & 0xff)

/* Global USB3 PIPE Control Register */
#define GUSB3PIPECTL_PHYSOFTRST    BIT(31)
#define GUSB3PIPECTL_U2SSINP3OK    BIT(29)
#define GUSB3PIPECTL_DISRXDETINP3  BIT(28)
#define GUSB3PIPECTL_UX_EXIT_PX    BIT(27)
#define GUSB3PIPECTL_REQP1P2P3     BIT(24)
#define GUSB3PIPECTL_DEP1P2P3(n)   ((n) << 19)
#define GUSB3PIPECTL_DEP1P2P3_MASK GUSB3PIPECTL_DEP1P2P3(7)
#define GUSB3PIPECTL_DEP1P2P3_EN   GUSB3PIPECTL_DEP1P2P3(1)
#define GUSB3PIPECTL_DEPOCHANGE    BIT(18)
#define GUSB3PIPECTL_SUSPHY        BIT(17)
#define GUSB3PIPECTL_LFPSFILT      BIT(9)
#define GUSB3PIPECTL_RX_DETOPOLL   BIT(8)
#define GUSB3PIPECTL_TX_DEEPH_MASK GUSB3PIPECTL_TX_DEEPH(3)
#define GUSB3PIPECTL_TX_DEEPH(n)   ((n) << 1)

/* Global TX Fifo Size Register */
#define GTXFIFOSIZ_TXFDEF(n)    ((n) & 0xffff)
#define GTXFIFOSIZ_TXFSTADDR(n) ((n) & 0xffff0000)

/* Global HWPARAMS1 Register */
#define GHWPARAMS1_EN_PWROPT(n)  (((n) & (3 << 24)) >> 24)
#define GHWPARAMS1_EN_PWROPT_NO  0
#define GHWPARAMS1_EN_PWROPT_CLK 1
#define GHWPARAMS1_EN_PWROPT_HIB 2
#define GHWPARAMS1_PWROPT(n)     ((n) << 24)
#define GHWPARAMS1_PWROPT_MASK   GHWPARAMS1_PWROPT(3)

/* Global HWPARAMS4 Register */
#define GHWPARAMS4_HIBER_SCRATCHBUFS(n) (((n) & (0x0f << 13)) >> 13)
#define MAX_HIBER_SCRATCHBUFS           15

/* Global Event Count/Size Register */
#define GEVNTCOUNT_EVENTSIZ_MASK (0xfffc)
#define GEVNTSIZ_EVNTINTRPTMASK  BIT(31)

/* Device Configuration Register */
#define DCFG_BIT11     BIT(11)
#define DCFG_LPM_CAP   BIT(22)
#define DCFG_IGNSTRMPP BIT(23)

#define DCFG_NUMP(_n)     (((_n) & 0x1f) << 17)
#define DCFG_NUMP_MASK    DCFG_NUMP(0x1f)
#define DCFG_NUMP_GET(_v) (((_v) >> 17) & 0x1f)

#define DCFG_INTRNUM(_intr)  (((_intr) & 0x1f) << 12)
#define DCFG_INTRNUM_MASK    DCFG_INTRNUM(0x1f)
#define DCFG_INTRNUM_GET(_v) (((_v) & DCFG_INTRNUM_MASK) >> 12)

#define DCFG_DEVADDR(addr)   ((addr) << 3)
#define DCFG_DEVADDR_MASK    DCFG_DEVADDR(0x7f)
#define DCFG_DEVADDR_GET(_v) (((_v) & DCFG_DEVADDR_MASK) >> 3)

#define DCFG_SPEED_MASK      (7 << 0)
#define DCFG_SUPERSPEED_PLUS (5 << 0)
#define DCFG_SUPERSPEED      (4 << 0)
#define DCFG_HIGHSPEED       (0 << 0)
#define DCFG_FULLSPEED2      (1 << 0)
#define DCFG_LOWSPEED        (2 << 0)
#define DCFG_FULLSPEED1      (3 << 0)

/* Device Control Register */
#define DCTL_RUN_STOP BIT(31)
#define DCTL_CSFTRST  BIT(30)
#define DCTL_LSFTRST  BIT(29)

#define DCTL_HIRD_THRES_MASK (0x1f << 24)
#define DCTL_HIRD_THRES(n)   ((n) << 24)

#define DCTL_APPL1RES BIT(23)

/* These apply for core versions 1.87a and earlier */
#define DCTL_TRGTULST_MASK     (0x0f << 17)
#define DCTL_TRGTULST(n)       ((n) << 17)
#define DCTL_TRGTULST_U2       (DCTL_TRGTULST(2))
#define DCTL_TRGTULST_U3       (DCTL_TRGTULST(3))
#define DCTL_TRGTULST_SS_DIS   (DCTL_TRGTULST(4))
#define DCTL_TRGTULST_RX_DET   (DCTL_TRGTULST(5))
#define DCTL_TRGTULST_SS_INACT (DCTL_TRGTULST(6))

/* These apply for core versions 1.94a and later */
#define DCTL_KEEP_CONNECT BIT(19)
#define DCTL_L1_HIBER_EN  BIT(18)
#define DCTL_CRS          BIT(17)
#define DCTL_CSS          BIT(16)

#define DCTL_INITU2ENA    BIT(12)
#define DCTL_ACCEPTU2ENA  BIT(11)
#define DCTL_INITU1ENA    BIT(10)
#define DCTL_ACCEPTU1ENA  BIT(9)
#define DCTL_TSTCTRL_MASK (0xf << 1)

#define DCTL_ULSTCHNGREQ_MASK (0x0f << 5)
#define DCTL_ULSTCHNGREQ(n)   (((n) << 5) & DCTL_ULSTCHNGREQ_MASK)

#define DCTL_ULSTCHNG_NO_ACTION   (DCTL_ULSTCHNGREQ(0))
#define DCTL_ULSTCHNG_SS_DISABLED (DCTL_ULSTCHNGREQ(4))
#define DCTL_ULSTCHNG_RX_DETECT   (DCTL_ULSTCHNGREQ(5))
#define DCTL_ULSTCHNG_SS_INACTIVE (DCTL_ULSTCHNGREQ(6))
#define DCTL_ULSTCHNG_RECOVERY    (DCTL_ULSTCHNGREQ(8))
#define DCTL_ULSTCHNG_COMPLIANCE  (DCTL_ULSTCHNGREQ(10))
#define DCTL_ULSTCHNG_LOOPBACK    (DCTL_ULSTCHNGREQ(11))

/* Device Event Enable Register */
#define DEVTEN_VNDRDEVTSTRCVEDEN   BIT(12)
#define DEVTEN_EVNTOVERFLOWEN      BIT(11)
#define DEVTEN_CMDCMPLTEN          BIT(10)
#define DEVTEN_ERRTICERREN         BIT(9)
#define DEVTEN_SOFEN               BIT(7)
#define DEVTEN_EOPFEN              BIT(6)
#define DEVTEN_HIBERNATIONREQEVTEN BIT(5)
#define DEVTEN_WKUPEVTEN           BIT(4)
#define DEVTEN_ULSTCNGEN           BIT(3)
#define DEVTEN_CONNECTDONEEN       BIT(2)
#define DEVTEN_USBRSTEN            BIT(1)
#define DEVTEN_DISCONNEVTEN        BIT(0)

/* Device Status Register */
#define DSTS_DCNRD BIT(29)

/* This applies for core versions 1.87a and earlier */
#define DSTS_PWRUPREQ BIT(24)

/* These apply for core versions 1.94a and later */
#define DSTS_RSS BIT(25)
#define DSTS_SSS BIT(24)

#define DSTS_COREIDLE   BIT(23)
#define DSTS_DEVCTRLHLT BIT(22)

#define DSTS_USBLNKST_MASK (0x0f << 18)
#define DSTS_USBLNKST(n)   (((n) & 0x0f) << 18)

#define DSTS_RXFIFOEMPTY BIT(17)

#define DSTS_SOFFN_MASK (0x3fff << 3)
#define DSTS_SOFFN(n)   (((n) & DSTS_SOFFN_MASK) >> 3)

#define DSTS_CONNECTSPD (7 << 0)

#define DSTS_SUPERSPEED_PLUS (5 << 0)
#define DSTS_SUPERSPEED      (4 << 0)
#define DSTS_HIGHSPEED       (0 << 0)
#define DSTS_FULLSPEED2      (1 << 0)
#define DSTS_LOWSPEED        (2 << 0)
#define DSTS_FULLSPEED1      (3 << 0)

/* Device Generic Command Register */
#define DGCMD_SET_LMP          0x01
#define DGCMD_SET_PERIODIC_PAR 0x02
#define DGCMD_XMIT_FUNCTION    0x03

/* These apply for core versions 1.94a and later */
#define DGCMD_SET_SCRATCHPAD_ADDR_LO 0x04
#define DGCMD_SET_SCRATCHPAD_ADDR_HI 0x05

#define DGCMD_SELECTED_FIFO_FLUSH  0x09
#define DGCMD_ALL_FIFO_FLUSH       0x0a
#define DGCMD_SET_ENDPOINT_NRDY    0x0c
#define DGCMD_RUN_SOC_BUS_LOOPBACK 0x10

#define DGCMD_CMDSTATUS       BIT(15)
#define DGCMD_CMDACT          BIT(10)
#define DGCMD_CMDIOC          BIT(8)
#define DGCMD_CMDTYPE_MASK    (0xff << 0)
#define DGCMD_CMDTYPE_GET(_v) ((_v) & 0xff)

/* Device Generic Command Parameter Register */
#define DGCMDPAR_FORCE_LINKPM_ACCEPT BIT(0)
#define DGCMDPAR_FIFO_NUM(n)         ((n) << 0)
#define DGCMDPAR_RX_FIFO             (0 << 5)
#define DGCMDPAR_TX_FIFO             BIT(5)
#define DGCMDPAR_LOOPBACK_DIS        (0 << 0)
#define DGCMDPAR_LOOPBACK_ENA        BIT(0)

/* Device Endpoint Command Register */
#define DEPCMD_PARAM_SHIFT   16
#define DEPCMD_PARAM(x)      ((x) << DEPCMD_PARAM_SHIFT)
#define DEPCMD_PARAM_MASK    DEPCMD_PARAM(0xffff)
#define DEPCMD_STATUS        BIT(15)
#define DEPCMD_HIPRI_FORCERM BIT(11)
#define DEPCMD_CMDACT        BIT(10)
#define DEPCMD_CMDIOC        BIT(8)

#define DEPCMD_STARTCFG   (0x09 << 0)
#define DEPCMD_ENDXFER    (0x08 << 0)
#define DEPCMD_UPDATEXFER (0x07 << 0)
#define DEPCMD_STARTXFER  (0x06 << 0)
#define DEPCMD_CLEARSTALL (0x05 << 0)
#define DEPCMD_SETSTALL   (0x04 << 0)

/* This applies for core versions 1.90a and earlier */
#define DEPCMD_GETSEQNUMBER (0x03 << 0)
/* This applies for core versions 1.94a and later */
#define DEPCMD_GETEPSTATE (0x03 << 0)

#define DEPCMD_XFERCFG     (0x02 << 0)
#define DEPCMD_CFG         (0x01 << 0)
#define DEPCMD_CMDMASK     (0xf << 0)
#define DEPCMD_CMD_GET(_v) ((_v) & 0xf)

/* The EP number goes 0..31 so ep0 is always out and ep1 is always in */
#define DALEPENA_EP(n) BIT(n)

#define DEPCMD_TYPE_CONTROL 0
#define DEPCMD_TYPE_ISOC    1
#define DEPCMD_TYPE_BULK    2
#define DEPCMD_TYPE_INTR    3

#define EVENT_PENDING BIT(0)

#define EP_FLAG_STALLED BIT(0)
#define EP_FLAG_WEDGED  BIT(1)

#define EP_DIRECTION_TX true
#define EP_DIRECTION_RX false

#define TRB_NUM  32
#define TRB_MASK (TRB_NUM - 1)

#define EP_ENABLED         BIT(0)
#define EP_STALL           BIT(1)
#define EP_WEDGE           BIT(2)
#define EP_BUSY            BIT(4)
#define EP_PENDING_REQUEST BIT(5)
#define EP_MISSED_ISOC     BIT(6)

/* This last one is specific to EP0 */
#define EP0_DIR_IN BIT(31)

enum dwc3_link_state
{
    /* In SuperSpeed */
    LINK_STATE_U0       = 0x00, /* in HS, means ON */
    LINK_STATE_U1       = 0x01,
    LINK_STATE_U2       = 0x02, /* in HS, means SLEEP */
    LINK_STATE_U3       = 0x03, /* in HS, means SUSPEND */
    LINK_STATE_SS_DIS   = 0x04,
    LINK_STATE_RX_DET   = 0x05, /* in HS, means Early Suspend */
    LINK_STATE_SS_INACT = 0x06,
    LINK_STATE_POLL     = 0x07,
    LINK_STATE_RECOV    = 0x08,
    LINK_STATE_HRESET   = 0x09,
    LINK_STATE_CMPLY    = 0x0a,
    LINK_STATE_LPBK     = 0x0b,
    LINK_STATE_RESET    = 0x0e,
    LINK_STATE_RESUME   = 0x0f,
    LINK_STATE_MASK     = 0x0f,
};

/* TRB Length, PCM and Status */
#define TRB_SIZE_MASK      (0x00ffffff)
#define TRB_SIZE_LENGTH(n) ((n) & TRB_SIZE_MASK)
#define TRB_SIZE_PCM1(n)   (((n) & 0x03) << 24)
#define TRB_SIZE_TRBSTS(n) (((n) & (0x0f << 28)) >> 28)

#define TRBSTS_OK            0
#define TRBSTS_MISSED_ISOC   1
#define TRBSTS_SETUP_PENDING 2
#define TRB_STS_XFER_IN_PROG 4

/* TRB Control */
#define TRB_CTRL_HWO         BIT(0)
#define TRB_CTRL_LST         BIT(1)
#define TRB_CTRL_CHN         BIT(2)
#define TRB_CTRL_CSP         BIT(3)
#define TRB_CTRL_TRBCTL(n)   (((n) >> 4) & 0x3f)
#define TRB_CTRL_ISP_IMI     BIT(10)
#define TRB_CTRL_IOC         BIT(11)
#define TRB_CTRL_SID_SOFN(n) (((n) >> 14) & 0xffff)

typedef enum TRBControlType
{
    TRBCTL_RESERVED = 0,
    TRBCTL_NORMAL,
    TRBCTL_CONTROL_SETUP,
    TRBCTL_CONTROL_STATUS2,
    TRBCTL_CONTROL_STATUS3,
    TRBCTL_CONTROL_DATA,
    TRBCTL_ISOCHRONOUS_FIRST,
    TRBCTL_ISOCHRONOUS,
    TRBCTL_LINK_TRB,
} TRBControlType;

/**
 * struct dwc3_trb - transfer request block (hw format)
 * @bpl: DW0-3
 * @bph: DW4-7
 * @status: DW8-B
 * @trl: DWC-F
 */
struct dwc3_trb
{
    uint32_t bpl;
    uint32_t bph;
    uint32_t status;
    uint32_t ctrl;
} QEMU_PACKED;

/* GHWPARAMS0 */
#define GHWPARAMS0_MODE(n) ((n) & 0x7)

#define GHWPARAMS0_MODE_DEVICE 0
#define GHWPARAMS0_MODE_HOST   1
#define GHWPARAMS0_MODE_DRD    2
#define GHWPARAMS0_MODE_HUB    3

#define GHWPARAMS0_MDWIDTH(n) (((n) & 0xff00) >> 8)

/* GHWPARAMS1 */
#define GHWPARAMS1_NUM_INT(n) (((n) & (0x3f << 15)) >> 15)

/* HWPARAMS3 */
#define GHWPARAMS3_NUM_IN_EPS_MASK (0x1f << 18)
#define GHWPARAMS3_NUM_EPS_MASK    (0x3f << 12)
#define GHWPARAMS3_NUM_IN_EPS(_n)  (((_n) & 0x1f) << 18)
#define GHWPARAMS3_NUM_EPS(_n)     (((_n) & 0x3f) << 12)

/* HWPARAMS7 */
#define GHWPARAMS7_RAM1_DEPTH(n) ((n) & 0xffff)

/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */

struct dwc3_event_type
{
    uint32_t is_devspec   : 1;
    uint32_t type         : 7;
    uint32_t reserved8_31 : 24;
} QEMU_PACKED;

#define DEPEVT_XFERCOMPLETE   0x01
#define DEPEVT_XFERINPROGRESS 0x02
#define DEPEVT_XFERNOTREADY   0x03
#define DEPEVT_RXTXFIFOEVT    0x04
#define DEPEVT_STREAMEVT      0x06
#define DEPEVT_EPCMDCMPLT     0x07

/**
 * struct dwc3_event_depevt - Device Endpoint Events
 * @one_bit: indicates this is an endpoint event (not used)
 * @endpoint_number: number of the endpoint
 * @endpoint_event: The event we have:
 *  0x00    - Reserved
 *  0x01    - XferComplete
 *  0x02    - XferInProgress
 *  0x03    - XferNotReady
 *  0x04    - RxTxFifoEvt (IN->Underrun, OUT->Overrun)
 *  0x05    - Reserved
 *  0x06    - StreamEvt
 *  0x07    - EPCmdCmplt
 * @reserved11_10: Reserved, don't use.
 * @status: Indicates the status of the event. Refer to databook for
 *  more information.
 * @parameters: Parameters of the current event. Refer to databook for
 *  more information.
 */
struct dwc3_event_depevt
{
    uint32_t one_bit         : 1;
    uint32_t endpoint_number : 5;
    uint32_t endpoint_event  : 4;
    uint32_t reserved11_10   : 2;
    uint32_t status          : 4;

/* Within XferNotReady */
#define DEPEVT_STATUS_TRANSFER_ACTIVE BIT(3)

/* Within XferComplete or XferInProgress */
#define DEPEVT_STATUS_BUSERR      BIT(0)
#define DEPEVT_STATUS_SHORT       BIT(1)
#define DEPEVT_STATUS_IOC         BIT(2)
#define DEPEVT_STATUS_LST         BIT(3) /* XferComplete */
#define DEPEVT_STATUS_MISSED_ISOC BIT(3) /* XferInProgress */

/* Stream event only */
#define DEPEVT_STREAMEVT_FOUND    1
#define DEPEVT_STREAMEVT_NOTFOUND 2

/* Control-only Status */
#define DEPEVT_STATUS_CONTROL_DATA     1
#define DEPEVT_STATUS_CONTROL_STATUS   2
#define DEPEVT_STATUS_CONTROL_PHASE(n) ((n) & 3)

/* In response to Start Transfer */
#define DEPEVT_TRANSFER_NO_RESOURCE 1
#define DEPEVT_TRANSFER_BUS_EXPIRY  2

    uint32_t parameters : 16;
} QEMU_PACKED;

/**
 * struct dwc3_event_devt - Device Events
 * @one_bit: indicates this is a non-endpoint event (not used)
 * @device_event: indicates it's a device event. Should read as 0x00
 * @type: indicates the type of device event.
 *  0   - DisconnEvt
 *  1   - USBRst
 *  2   - ConnectDone
 *  3   - ULStChng
 *  4   - WkUpEvt
 *  5   - Reserved
 *  6   - EOPF
 *  7   - SOF
 *  8   - Reserved
 *  9   - ErrticErr
 *  10  - CmdCmplt
 *  11  - EvntOverflow
 *  12  - VndrDevTstRcved
 * @reserved15_12: Reserved, not used
 * @event_info: Information about this event
 * @reserved31_24: Reserved, not used
 */
struct dwc3_event_devt
{
    uint32_t one_bit       : 1;
    uint32_t device_event  : 7;
    uint32_t type          : 4;
    uint32_t reserved15_12 : 4;
    uint32_t event_info    : 8;
    uint32_t reserved31_24 : 8;
} QEMU_PACKED;

/**
 * struct dwc3_event_gevt - Other Core Events
 * @one_bit: indicates this is a non-endpoint event (not used)
 * @device_event: indicates it's (0x03) Carkit or (0x04) I2C event.
 * @phy_port_number: self-explanatory
 * @reserved31_12: Reserved, not used.
 */
struct dwc3_event_gevt
{
    uint32_t one_bit         : 1;
    uint32_t device_event    : 7;
    uint32_t phy_port_number : 4;
    uint32_t reserved31_12   : 20;
} QEMU_PACKED;

union dwc3_event
{
    uint32_t                 raw;
    struct dwc3_event_type   type;
    struct dwc3_event_depevt depevt;
    struct dwc3_event_devt   devt;
    struct dwc3_event_gevt   gevt;
};

#define DEPCFG_RSC_IDX(x)     (((x) & 0x7f) << DEPCMD_PARAM_SHIFT)
#define DEPCFG_RSC_IDX_MASK   DEPCFG_RSC_IDX(0x7f)
#define DEPCFG_RSC_IDX_GET(x) (((x) >> DEPCMD_PARAM_SHIFT) & 0x7f)

#define DEPCFG_EP_TYPE(n)         (((n) >> 1) & 0x3)
#define DEPCFG_FIFO_NUMBER(n)     (((n) >> 17) & 0x1f)
#define DEPCFG_MAX_PACKET_SIZE(n) (((n) >> 3) & 0x7ff)
#define DEPCFG_ACTION(n)          (((n) >> 30) & 0x3)
#define DEPCFG_BURST_SIZE(n)      (((n) >> 22) & 0xf)
#define DEPCFG_ACTION_INIT        (0)
#define DEPCFG_ACTION_RESTORE     (1)
#define DEPCFG_ACTION_MODIFY      (2)

#define DEPCFG_INT_NUM(n)          (((n) >> 0) & 0x1f)
#define DEPCFG_PAR1_BIT5           BIT(5)
#define DEPCFG_PAR1_BIT6           BIT(6)
#define DEPCFG_PAR1_BIT7           BIT(7)
#define DEPCFG_EVENT_EN(_v)        (((_v) >> 7) & 0x7f)
#define DEPCFG_XFER_COMPLETE_EN    BIT(8)
#define DEPCFG_XFER_IN_PROGRESS_EN BIT(9)
#define DEPCFG_XFER_NOT_READY_EN   BIT(10)
#define DEPCFG_FIFO_ERROR_EN       BIT(11)
#define DEPCFG_PAR1_BIT12          BIT(12)
#define DEPCFG_STREAM_EVENT_EN     BIT(13)
#define DEPCFG_PAR1_BIT14          BIT(14)
#define DEPCFG_PAR1_BIT15          BIT(15)
#define DEPCFG_BINTERVAL_M1(n)     (((n) >> 16) & 0xff)
#define DEPCFG_STREAM_CAPABLE      BIT(24)
#define DEPCFG_EP_NUMBER(n)        (((n) >> 25) & 0x1f)
#define DEPCFG_BULK_BASED          BIT(30)
#define DEPCFG_FIFO_BASED          BIT(31)

#define DEPXFERCFG_NUMXFERRES(_v) ((_v) & 0xffff)
