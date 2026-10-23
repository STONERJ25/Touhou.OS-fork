/* xHCI register, TRB and context definitions (xHCI specification 1.1/1.2, section numbers in comments). */
#ifndef K_XHCI_H
#define K_XHCI_H

#include "types.h"

/* ---------------------------------------------------------------- capability registers (5.3) */
#define XCAP_CAPLENGTH   0x00           /* u8 */
#define XCAP_HCIVERSION  0x02           /* u16 */
#define XCAP_HCSPARAMS1  0x04           /* MaxSlots 7:0, MaxIntrs 18:8, MaxPorts 31:24 */
#define XCAP_HCSPARAMS2  0x08           /* IST 3:0, ERSTMax 7:4, MaxScratchpadBufs Hi 25:21, SPR 26, Lo 31:27 */
#define XCAP_HCSPARAMS3  0x0C
#define XCAP_HCCPARAMS1  0x10           /* AC64 0, BNC 1, CSZ 2, PPC 3, xECP 31:16 */
#define XCAP_DBOFF       0x14
#define XCAP_RTSOFF      0x18
#define XCAP_HCCPARAMS2  0x1C

#define HCC1_AC64  0x01u
#define HCC1_CSZ   0x04u
#define HCC1_PPC   0x08u

/* ---------------------------------------------------------------- operational registers (5.4) */
#define XOP_USBCMD    0x00
#define XOP_USBSTS    0x04
#define XOP_PAGESIZE  0x08
#define XOP_CRCR      0x18              /* 64 bit */
#define XOP_DCBAAP    0x30              /* 64 bit */
#define XOP_CONFIG    0x38
#define XOP_PORTSC(n) (0x400 + 0x10 * ((n) - 1))

#define CMD_RUN    0x00000001u
#define CMD_RESET  0x00000002u
#define CMD_INTE   0x00000004u
#define CMD_HSEE   0x00000008u
#define CMD_EWE    0x00000400u

#define STS_HCH    0x00000001u          /* HC halted */
#define STS_HSE    0x00000004u          /* host system error */
#define STS_EINT   0x00000008u          /* event interrupt (RW1C) */
#define STS_PCD    0x00000010u          /* port change detect (RW1C) */
#define STS_SSS    0x00000100u
#define STS_RSS    0x00000200u
#define STS_SRE    0x00000400u          /* save/restore error */
#define STS_CNR    0x00000800u          /* controller not ready */
#define STS_HCE    0x00001000u          /* host controller error */
#define STS_RW1C   (STS_HSE | STS_EINT | STS_PCD | STS_SRE)

#define CRCR_RCS   0x1u
#define CRCR_CS    0x2u
#define CRCR_CA    0x4u
#define CRCR_CRR   0x8u

/* PORTSC (5.4.8). Bits written as 1 that are RW1C clear the status; the RO/RWS masks are what may be written back unchanged. */
#define PORTSC_CCS   0x00000001u
#define PORTSC_PED   0x00000002u        /* RW1C: writing 1 DISABLES the port */
#define PORTSC_OCA   0x00000008u
#define PORTSC_PR    0x00000010u        /* RW1S: port reset */
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK  0x000001E0u
#define PORTSC_PP    0x00000200u
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0x00003C00u
#define PORTSC_LWS   0x00010000u        /* link state write strobe */
#define PORTSC_CSC   0x00020000u        /* change bits, all RW1C */
#define PORTSC_PEC   0x00040000u
#define PORTSC_WRC   0x00080000u
#define PORTSC_OCC   0x00100000u
#define PORTSC_PRC   0x00200000u
#define PORTSC_PLC   0x00400000u
#define PORTSC_CEC   0x00800000u
#define PORTSC_CAS   0x01000000u
#define PORTSC_WCE   0x02000000u
#define PORTSC_WDE   0x04000000u
#define PORTSC_WOE   0x08000000u
#define PORTSC_DR    0x40000000u
#define PORTSC_WPR   0x80000000u        /* RW1S: warm port reset */
#define PORTSC_CHANGES (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* What may be written back without side effects: read-only bits and RWS state (PLS, PP, PIC, wake enables). */
#define PORTSC_PRESERVE (PORTSC_CCS | PORTSC_OCA | PORTSC_SPEED_MASK | PORTSC_DR | PORTSC_PLS_MASK | PORTSC_PP | 0x0000C000u | \
                         PORTSC_WCE | PORTSC_WDE | PORTSC_WOE)

#define PLS_U0         0
#define PLS_U3         3
#define PLS_DISABLED   4
#define PLS_RXDETECT   5
#define PLS_INACTIVE   6
#define PLS_POLLING    7
#define PLS_RECOVERY   8
#define PLS_HOT_RESET  9
#define PLS_COMPLIANCE 10
#define PLS_LOOPBACK   11
#define PLS_RESUME     15

/* ---------------------------------------------------------------- runtime registers (5.5) */
#define XRT_IR(n)     (0x20 + 0x20 * (n))
#define XIR_IMAN      0x00              /* IP 0 (RW1C), IE 1 */
#define XIR_IMOD      0x04
#define XIR_ERSTSZ    0x08
#define XIR_ERSTBA    0x10              /* 64 bit */
#define XIR_ERDP      0x18              /* 64 bit: DESI 2:0, EHB 3 (RW1C), pointer 63:4 */
#define ERDP_EHB      0x8ull

/* ---------------------------------------------------------------- extended capabilities (7) */
#define XECP_LEGACY    1
#define XECP_PROTOCOL  2
#define LEGACY_BIOS_OWNED 0x00010000u
#define LEGACY_OS_OWNED   0x01000000u
#define LEGACY_CTLSTS     4
/* SMI enables to clear (keep the reserved/preserved bits of Linux's XHCI_LEGACY_DISABLE_SMI) and RW1C status bits 31:29 */
#define LEGACY_KEEP_MASK  ((0x3u << 1) | (0xffu << 5) | (0x7u << 17))
#define LEGACY_SMI_EVENTS (0x7u << 29)

/* ---------------------------------------------------------------- TRBs (6.4) */
typedef struct Trb {
    u32 param_lo, param_hi;
    u32 status;
    u32 control;
} Trb;

#define TRB_CYCLE      0x1u
#define TRB_TC         0x2u             /* link: toggle cycle */
#define TRB_ENT        0x2u
#define TRB_ISP        0x4u
#define TRB_CHAIN      0x10u
#define TRB_IOC        0x20u
#define TRB_IDT        0x40u
#define TRB_BSR        0x200u           /* address device: block set address */
#define TRB_DIR_IN     0x10000u         /* data/status stage: direction IN */
#define TRB_TYPE(t)    ((u32)(t) << 10)
#define TRB_TYPE_OF(c) (((c) >> 10) & 0x3F)
#define TRB_SLOT(s)    ((u32)(s) << 24)
#define TRB_EP(e)      ((u32)(e) << 16)
#define TRB_TRT_OUT    (2u << 16)
#define TRB_TRT_IN     (3u << 16)

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_ISOCH = 5, TRB_LINK = 6, TRB_EVENT_DATA = 7, TRB_NOOP = 8,
    TRB_CMD_ENABLE_SLOT = 9, TRB_CMD_DISABLE_SLOT = 10, TRB_CMD_ADDRESS = 11, TRB_CMD_CONFIGURE = 12, TRB_CMD_EVALUATE = 13,
    TRB_CMD_RESET_EP = 14, TRB_CMD_STOP_EP = 15, TRB_CMD_SET_DEQ = 16, TRB_CMD_RESET_DEV = 17, TRB_CMD_NOOP = 23,
    TRB_EV_TRANSFER = 32, TRB_EV_CMD_COMPLETE = 33, TRB_EV_PORT_STATUS = 34, TRB_EV_BANDWIDTH = 35, TRB_EV_DOORBELL = 36,
    TRB_EV_HOST_CTRL = 37, TRB_EV_DEV_NOTIFY = 38, TRB_EV_MFINDEX_WRAP = 39,
};

/* completion codes (6.4.5) */
enum {
    CC_INVALID = 0, CC_SUCCESS = 1, CC_DATA_BUFFER = 2, CC_BABBLE = 3, CC_USB_TRANSACTION = 4, CC_TRB_ERROR = 5, CC_STALL = 6,
    CC_RESOURCE = 7, CC_BANDWIDTH = 8, CC_NO_SLOTS = 9, CC_INVALID_STREAM = 10, CC_SLOT_NOT_ENABLED = 11, CC_EP_NOT_ENABLED = 12,
    CC_SHORT_PACKET = 13, CC_RING_UNDERRUN = 14, CC_RING_OVERRUN = 15, CC_VF_EVENT_RING_FULL = 16, CC_PARAMETER = 17,
    CC_BANDWIDTH_OVERRUN = 18, CC_CONTEXT_STATE = 19, CC_NO_PING_RESPONSE = 20, CC_EVENT_RING_FULL = 21, CC_INCOMPATIBLE = 22,
    CC_MISSED_SERVICE = 23, CC_CMD_RING_STOPPED = 24, CC_CMD_ABORTED = 25, CC_STOPPED = 26, CC_STOPPED_LENGTH = 27,
    CC_STOPPED_SHORT = 28, CC_MAX_EXIT_LATENCY = 29, CC_ISOCH_OVERRUN = 31, CC_EVENT_LOST = 32, CC_UNDEFINED = 33,
    CC_INVALID_STREAM_ID = 34, CC_SECONDARY_BW = 35, CC_SPLIT_TRANSACTION = 36,
};

/* ---------------------------------------------------------------- contexts (6.2) */
/* Dword indexes. Slot context: */
#define SLOT_DW0_ROUTE(r)    ((u32)(r) & 0xFFFFFu)
#define SLOT_DW0_SPEED(s)    ((u32)(s) << 20)
#define SLOT_DW0_MTT         (1u << 25)
#define SLOT_DW0_HUB         (1u << 26)
#define SLOT_DW0_ENTRIES(n)  ((u32)(n) << 27)
#define SLOT_DW0_ENTRIES_MASK (0x1Fu << 27)
#define SLOT_DW1_ROOTPORT(p) ((u32)(p) << 16)
#define SLOT_DW1_NPORTS(n)   ((u32)(n) << 24)
#define SLOT_DW2_TT(hub, port) ((u32)(hub) | ((u32)(port) << 8))
#define SLOT_DW2_TTT(t)      ((u32)(t) << 16)
#define SLOT_STATE(dw3)      (((dw3) >> 27) & 0x1F)
#define SLOT_ADDR(dw3)       ((dw3) & 0xFF)

/* Endpoint context: */
#define EP_TYPE_ISOC_OUT 1
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_INT_OUT  3
#define EP_TYPE_CONTROL  4
#define EP_TYPE_ISOC_IN  5
#define EP_TYPE_BULK_IN  6
#define EP_TYPE_INT_IN   7
#define EP_DW0_STATE(dw0)    ((dw0) & 7)
#define EP_DW0_MULT(m)       ((u32)(m) << 8)
#define EP_DW0_INTERVAL(i)   ((u32)(i) << 16)
#define EP_DW0_ESIT_HI(v)    ((u32)((v) >> 16) << 24)
#define EP_DW1_CERR(c)       ((u32)(c) << 1)
#define EP_DW1_TYPE(t)       ((u32)(t) << 3)
#define EP_DW1_BURST(b)      ((u32)(b) << 8)
#define EP_DW1_MPS(m)        ((u32)(m) << 16)
#define EP_DW4_AVG(a)        ((u32)(a) & 0xFFFFu)
#define EP_DW4_ESIT_LO(v)    ((u32)((v) & 0xFFFFu) << 16)

enum { EPS_DISABLED = 0, EPS_RUNNING, EPS_HALTED, EPS_STOPPED, EPS_ERROR };

#endif
