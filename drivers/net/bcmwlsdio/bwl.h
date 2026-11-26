/*
 * bwl.h - Broadcom FullMAC SDIO core for the BCM43456 (AP6256, TaishanPi
 * 3M) and the BCM43752 (BW3752-50B1, ArmSoM CM5): the parts of the driver
 * that do not depend on Windows.
 *
 * Everything declared here is plain C with no Windows or libc dependency.
 * The same files build into bcmwlsdio.sys, where the bus operations below
 * are CMD52 and CMD53 through the SD bus driver's SDBUS interface, and into
 * tools/bwltest, where they are a simulated chip. CI runs the simulation on
 * Linux with gcc.
 *
 * The sequences follow OpenBSD's bwfm (sys/dev/ic/bwfm.c and
 * sys/dev/sdmmc/if_bwfm_sdio.c) and Linux brcmfmac; see bwl_regs.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#ifdef _MSC_VER
#define BWL_INLINE static __inline
#else
#define BWL_INLINE static inline
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* Return codes. 0 is success; everything else is negative. */
#define BWL_OK            0
#define BWL_E_IO         -1     /* a CMD52/CMD53 failed */
#define BWL_E_TIMEOUT    -2     /* a bit we waited for never came */
#define BWL_E_NOCHIP     -3     /* chip ID, interconnect or cores not as expected */
#define BWL_E_FORMAT     -4     /* a file or a frame is malformed */
#define BWL_E_SPACE      -5     /* buffer or RAM too small */
#define BWL_E_VERIFY     -6     /* read-back after download differs */
#define BWL_E_FIRMWARE   -7     /* the firmware answered a command with an error */
#define BWL_E_STATE      -8     /* called in the wrong state */
#define BWL_E_CREDIT     -9     /* the firmware gives no TX credit */

/* ------------------------------------------------------------ bus ops
 *
 * cmd52: one byte at Addr (17 bits) of function Fn. Write stores *Val,
 *        read returns the byte in *Val.
 * cmd53: one CMD53. Block = 0: byte mode, Len bytes (1..512, and no more
 *        than the function's block size). Block = 1: block mode, Len is a
 *        whole number of blocks of the function's block size. Incr = 1 for
 *        an incrementing address (backplane RAM), 0 for a fixed address
 *        (the F2 FIFO). Buf is the caller's bounce buffer, 4-byte aligned.
 * delay_us: sleep or spin at least Us microseconds.
 * log: optional, a short line for the debugger.
 */
typedef struct bwl_bus_ops {
    int  (*cmd52)(void *Ctx, unsigned Fn, u32 Addr, int Write, u8 *Val);
    int  (*cmd53)(void *Ctx, unsigned Fn, u32 Addr, int Incr, int Write, int Block,
                  u8 *Buf, u32 Len);
    void (*delay_us)(void *Ctx, u32 Us);
    void (*log)(void *Ctx, const char *Msg);
} bwl_bus_ops;

typedef struct bwl_core {
    u16 Id;
    u8  Rev;
    u32 Base;
    u32 Wrap;
} bwl_core;

#define BWL_MAX_CORES   32

/*
 * What differs between the chips this driver knows, from Linux brcmfmac
 * (chip.c, sdio.c, bcmsdh.c; ISC) where OpenBSD bwfm has no entry.
 */
enum bwl_sr_check {
    BWL_SR_GENERIC = 0,     /* PMU caps ext SR bit, then the retention control */
    BWL_SR_CHIPCTL3,        /* 4345: PMU chip control 3, bit 2 */
    BWL_SR_RETENTION,       /* 4359, 43751, 43752: the retention control only */
};

typedef struct bwl_chip_info {
    u16 Chip;
    u8  RevMin, RevMax;     /* inclusive; the first matching entry wins */
    const char *FwName;     /* brcmfmac<FwName>-sdio.bin/.txt/.clm_blob */
    u32 RamBase;            /* the ARM CR4's TCM base */
    u16 F2BlockSize;        /* 0: keep the caller's (512) */
    u8  F2Watermark;        /* F1_WATERMARK after F2 is up */
    u8  MesBusyCtrl;        /* 0: untouched; else F1_MESBUSYCTRL and DEVCTL_F2WM_ENAB */
    u8  AosNoDecode;        /* save/restore: CARDCAP_CMD_NODEC instead of CMD14 */
    u8  SrCheck;            /* enum bwl_sr_check */
} bwl_chip_info;

enum bwl_clk {
    BWL_CLK_NONE = 0,
    BWL_CLK_SDONLY,
    BWL_CLK_AVAIL,
};

/* The firmware's console ring, as found through the sdpcm_shared block. */
typedef struct bwl_console {
    u32 Addr;           /* address of struct rte_console in chip RAM, 0 if none */
    u32 LogBuf;
    u32 LogBufSize;
    u32 LogIdx;
} bwl_console;

/* What the last control exchange saw, for the breadcrumbs. */
typedef struct bwl_stats {
    u32 FramesTx;
    u32 FramesRx;
    u32 CtlRx;
    u32 EventsRx;
    u32 DataRx;
    u32 GlomRx;
    u32 RxErrors;
    u32 RxAborts;
    u32 LastEvent;      /* event type of the last firmware event */
    u32 LastEventStatus;
    u32 MailboxData;    /* last to-host mailbox data */
    u32 IntStatus;      /* last SDIO core intstatus read */
    u32 Polls;
} bwl_stats;

/* ------------------------------------------------------------ WLAN state (bwl_wlan.c) */

enum bwl_sec {
    BWL_SEC_OPEN = 0,       /* no privacy */
    BWL_SEC_WPA2_PSK,       /* RSN, PSK, CCMP pairwise and group */
    BWL_SEC_OTHER,          /* WEP, TKIP, 802.1X, SAE only: not joinable here */
};

enum bwl_link {
    BWL_LINK_DOWN = 0,
    BWL_LINK_CONNECTING,    /* C_SET_SSID sent, no result yet */
    BWL_LINK_ASSOCIATED,    /* associated; the port is closed until the keys are in */
    BWL_LINK_UP,            /* keys in (or an open network) */
};

/* One network as escan or a frame body describes it. */
typedef struct bwl_bss {
    u8  Bssid[6];
    u8  SsidLen;
    u8  Ssid[32];
    u8  Channel;            /* the control (primary) channel */
    u8  Band;               /* 0: 2.4 GHz, 1: 5 GHz */
    u8  Security;           /* enum bwl_sec */
    u8  Beacon;             /* the firmware heard it in a beacon (else a probe response) */
    int Rssi;               /* dBm */
    u16 Capability;
    u16 BeaconPeriod;
} bwl_bss;

/* What the front end gets from the core, on the thread that polls. */
typedef struct bwl_net_ops {
    /* A data frame from the firmware, as Ethernet (EAPOL included). */
    void (*rx_eth)(void *Ctx, const u8 *Eth, u32 Len);
    /* One escan result and its frame body (Timestamp, interval,
     * capability, elements), as Windows wants a beacon or probe response. */
    void (*scan_bss)(void *Ctx, const bwl_bss *Bss, const u8 *Body, u32 Len);
} bwl_net_ops;

/* Events for the front end, cleared by it (bwl_wlan.Events). */
#define BWL_EV_SCAN_DONE    0x01u   /* escan finished; ScanStatus */
#define BWL_EV_ASSOC        0x02u   /* the join finished; ConnStatus (0: associated) */
#define BWL_EV_DISCONNECT   0x04u   /* the link went down; DisconnectReason */
#define BWL_EV_KEYED        0x08u   /* firmware supplicant: the handshake finished; ConnStatus */

#define BWL_ASSOC_IE_MAX    512u

typedef struct bwl_wlan {
    const bwl_net_ops *Net;     /* NULL: data frames dropped, scan results counted only */
    u8   Mac[6];
    u32  IoType;                /* C_GET_VERSION: chanspec encoding, IOTYPE_* */
    u8   Up;                    /* C_UP done */
    u8   Has5g;
    u8   FwSupCapable;          /* "cap" lists idsup */
    u8   FwSup;                 /* this connection: the firmware runs the handshake */
    u32  Events;                /* BWL_EV_* */

    /* scan */
    u16  ScanSyncId;
    u8   Scanning;
    u32  ScanStatus;
    u32  ScanResults;

    /* connection */
    int  Link;                  /* enum bwl_link */
    bwl_bss Target;
    u8   Bssid[6];
    u32  ConnStatus;            /* event status of the failure, 0 if none */
    u32  ConnEvent;             /* the event that ended the join */
    u32  DisconnectReason;
    u32  DisconnectEvent;
    u8   DisconnectByPeer;      /* the AP deauthenticated or disassociated us (else the link was lost) */
    u8   AssocPending;          /* join succeeded: read the IEs on the next service */
    u8   RsnIe[24];             /* what goes into our association request */
    u32  RsnIeLen;
    u8   AssocReqIe[BWL_ASSOC_IE_MAX];
    u32  AssocReqIeLen;
    u8   AssocRspIe[BWL_ASSOC_IE_MAX];
    u32  AssocRspIeLen;
    u16  AssocCapability;
    u16  AssocListen;
    u16  AssocAid;
    u32  RateKbps;              /* BwlLinkInfo */
    int  Rssi;

    /* data */
    u32  TxData, TxDataDrops, RxData, RxEapol, TxEapol, Keys, FlowHold;
    u8   FlowCtl;               /* the firmware's per-priority flow-off bits */
    u32  LastFwStatus;          /* the firmware's status for the last refused command */
    u32  JoinFlags;             /* bwl_wlan.c: the join's progress */
    u8   ScanBody[2048];        /* bwl_wlan.c: an escan record as a frame body */
} bwl_wlan;

typedef struct bwl_dev {
    const bwl_bus_ops *Ops;
    void *Ctx;

    /* Bounce buffer: every CMD53 goes through it. Size >= BWL_BOUNCE_MIN. */
    u8  *Bounce;
    u32  BounceSize;

    /* Limits for one CMD53, set by the caller before BwlAttach. */
    u16  F1BlockSize;           /* 64 */
    u16  F2BlockSize;           /* 512 */
    u16  MaxBlocks;             /* blocks per block-mode CMD53, 1..511 */
    u8   F2ByteModeOnly;        /* 1: never use block mode on F2 */

    /* Backplane window (SBADDR), ~0 when unknown. */
    u32  Window;

    /* Chip */
    u32  ChipIdReg;
    u16  Chip;
    u8   ChipRev;
    const bwl_chip_info *Info;  /* NULL until the chip ID matched an entry */
    bwl_core Cores[BWL_MAX_CORES];
    unsigned NumCores;
    u32  RamBase;
    u32  RamSize;
    u32  SrSize;
    u32  CcCaps;
    u32  CcCapsExt;
    u32  PmuCaps;
    u8   PmuRev;
    u8   SrCapable;
    u8   SrEnabled;
    u8   AlpOnly;
    int  ClkState;

    /* Firmware */
    u8   FwRunning;
    u32  SharedAddr;
    bwl_console Console;

    /* SDPCM / BCDC */
    u8   TxSeq;
    u8   TxMaxSeq;
    u16  ReqId;
    bwl_stats Stats;

    /* Where it stopped, for the breadcrumbs. */
    const char *Step;
    u32  Detail;

    bwl_wlan Wlan;
} bwl_dev;

/* Big enough for a 32 KB window chunk plus a full control frame. */
#define BWL_BOUNCE_MIN  (64u * 1024u)

/* ------------------------------------------------------------ bwl_sdio.c */

void BwlInit(bwl_dev *Dev, const bwl_bus_ops *Ops, void *Ctx, u8 *Bounce, u32 BounceSize);

int  BwlRead8(bwl_dev *Dev, u32 Addr, u8 *Val);
int  BwlWrite8(bwl_dev *Dev, u32 Addr, u8 Val);
int  BwlRead32(bwl_dev *Dev, u32 Addr, u32 *Val);
int  BwlWrite32(bwl_dev *Dev, u32 Addr, u32 Val);

/* One logical transfer, split into byte- and block-mode CMD53s. */
int  BwlXfer(bwl_dev *Dev, unsigned Fn, u32 Addr, int Incr, int Write, u8 *Buf, u32 Len);

/* Chip RAM through the backplane window, copying through the bounce buffer. */
int  BwlRamRw(bwl_dev *Dev, u32 Addr, u8 *Data, u32 Len, int Write);

/* F2 frame FIFO. Data must be the bounce buffer or a 4-aligned buffer. */
int  BwlFrameRw(bwl_dev *Dev, u8 *Data, u32 Len, int Write);

int  BwlFuncEnable(bwl_dev *Dev, unsigned Fn, int Enable, u32 TimeoutMs);
int  BwlSetBlockSize(bwl_dev *Dev, unsigned Fn, u16 Size);

int  BwlSdioCoreRead(bwl_dev *Dev, u32 Reg, u32 *Val);
int  BwlSdioCoreWrite(bwl_dev *Dev, u32 Reg, u32 Val);

void BwlClkCtl(bwl_dev *Dev, int NewState);

/* ------------------------------------------------------------ bwl_chip.c */

/* Clock up, chip ID, EROM scan, CPU halted, RAM size. */
int  BwlChipAttach(bwl_dev *Dev);
const bwl_core *BwlChipCore(const bwl_dev *Dev, u16 Id, unsigned Index);
const bwl_core *BwlChipPmu(const bwl_dev *Dev);
int  BwlChipSetPassive(bwl_dev *Dev);
int  BwlChipSetActive(bwl_dev *Dev, u32 ResetVector);
int  BwlChipSrCapable(bwl_dev *Dev);

/* The table entry for a chip, NULL if this driver does not know it. */
const bwl_chip_info *BwlChipInfo(u16 Chip, u8 Rev);

/* "43456" for chip 0x4345 rev 9, "43752" for 0xaae8, NULL if this driver has no name for it. */
const char *BwlChipFwName(u16 Chip, u8 Rev);

/* ------------------------------------------------------------ bwl_bringup.c */

/* bwfm_sdio_attach: F1 on, chip attach, KSO, WLAN reset, PMU reload, F2 off. */
int  BwlAttach(bwl_dev *Dev);

/* bwfm_sdio_load_microcode: firmware at RAM base, NVRAM at the top, verify, start the CPU. */
int  BwlDownload(bwl_dev *Dev, const u8 *Fw, u32 FwLen, const u8 *Nvram, u32 NvramLen, int Verify);

/* bwfm_sdio_preinit after the download: HT clock, mailbox, F2 on, interrupt mask, watermark, SR. */
int  BwlStart(bwl_dev *Dev, u32 F2TimeoutMs);

/* Read sdpcm_shared and the console pointers. */
int  BwlReadShared(bwl_dev *Dev);

/* Copy up to Cap-1 bytes of the console ring, oldest first; returns the length. */
u32  BwlConsoleRead(bwl_dev *Dev, char *Out, u32 Cap);

/* ------------------------------------------------------------ bwl_fw.c */

/*
 * NVRAM text to the form the firmware expects: one NUL-terminated
 * key=value per line, comments and blank lines dropped, padded to 4 bytes,
 * then a 32-bit token (length in words, and its complement in the upper
 * half). Dst must hold SrcLen + BWL_NVRAM_SLACK bytes: at worst every
 * byte is kept and the last line has no newline (SrcLen + 1), the padding
 * adds up to 4 (at least one NUL) and the token 4, so SrcLen + 9; 12 keeps
 * the size a multiple of 4. Returns the length, or a negative error.
 */
int  BwlNvramConvert(const u8 *Src, u32 SrcLen, u8 *Dst, u32 DstCap);
#define BWL_NVRAM_SLACK 12u

/*
 * The version in a firmware image's trailer, as text. 0 if found. Older
 * images end with "... Version: 7.45.96.0 CRC: ..."; newer ones with a
 * "<FW-TAG>... Version=18.35.387.23.7 (...)" line. The last one wins.
 */
int  BwlFwImageVersion(const u8 *Img, u32 Len, char *Out, u32 Cap);

/* Basic checks on a firmware image before it goes to RAM. */
int  BwlFwImageCheck(const u8 *Img, u32 Len, u32 RamSize);

/* The version out of the "ver" iovar's reply ("... version 7.45.96 ..."). */
int  BwlVerString(const char *Ver, u32 Len, char *Out, u32 Cap);

/*
 * One "clmload" chunk: the payload of the iovar (after its name) for the
 * bytes Blob[Off .. Off+n), n = min(Len - Off, BWL_DLOAD_MAX). Writes it to
 * Out and returns its length; *Consumed = n.
 */
int  BwlClmChunk(const u8 *Blob, u32 Len, u32 Off, u8 *Out, u32 Cap, u32 *Consumed);
int  BwlClmCheck(const u8 *Blob, u32 Len);

/* ------------------------------------------------------------ bwl_proto.c */

typedef struct bwl_frame {
    u16 FrameLen;       /* hardware header length field */
    u8  Seq;
    u8  Channel;
    u8  NextLen;        /* in 16-byte units */
    u8  DataOff;
    u8  FlowCtl;
    u8  MaxSeq;
    const u8 *Payload;  /* at DataOff */
    u32 PayloadLen;
} bwl_frame;

#define BWL_SDPCM_HDR   12u     /* 4-byte hardware + 8-byte software header */

/* Build an SDPCM frame around Payload. Returns the padded length to send. */
int  BwlSdpcmBuild(u8 *Buf, u32 Cap, u8 Channel, u8 Seq, const u8 *Payload, u32 Len,
                   u16 F2BlockSize);

/* Header checks only (frame length, checksum, data offset). */
int  BwlSdpcmParseHeader(const u8 *Buf, u32 Len, bwl_frame *F);

/* Full frame: header checks plus payload bounds. */
int  BwlSdpcmParse(const u8 *Buf, u32 Len, bwl_frame *F);

/* Can another frame go out? (bwfm_sdio_tx_ok) */
int  BwlTxCredit(u8 Seq, u8 MaxSeq);

#define BWL_BCDC_DCMD_HDR  16u

int  BwlBcdcBuildDcmd(u8 *Buf, u32 Cap, u32 Cmd, int Set, unsigned IfIdx, u16 ReqId,
                      const u8 *Data, u32 Len);

typedef struct bwl_dcmd_reply {
    u32 Cmd;
    u32 Len;
    u32 Flags;
    int Status;
    u16 ReqId;
    int Error;          /* the error flag was set */
    const u8 *Data;
    u32 DataLen;
} bwl_dcmd_reply;

int  BwlBcdcParseDcmd(const u8 *Buf, u32 Len, bwl_dcmd_reply *R);

/* Strip the 4-byte BCDC data header and its data offset. */
int  BwlBcdcData(const u8 *Buf, u32 Len, const u8 **Payload, u32 *PayloadLen, unsigned *IfIdx);

typedef struct bwl_event {
    u16 Flags;
    u32 Type;
    u32 Status;
    u32 Reason;
    u32 AuthType;
    u32 DataLen;
    u8  Addr[6];
    u8  IfIdx;
    u8  BssCfgIdx;
    const u8 *Data;
} bwl_event;

/* A Broadcom event (ethertype 0x886c, OUI 00:10:18, subtype 1) out of an 802.3 frame. */
int  BwlEventParse(const u8 *Eth, u32 Len, bwl_event *E);

/* ------------------------------------------------------------ bwl_ctl.c */

/*
 * One firmware command over the control channel, polled: build the BCDC
 * command from In (InLen bytes), send it on F2, then read frames until
 * the reply with the same request ID comes back or TimeoutMs runs out.
 * Events seen meanwhile are counted in Dev->Stats and dropped. The
 * reply's data is copied to Out (at most OutCap bytes; Out may be NULL),
 * *OutLen is its length. A reply with the error flag returns
 * BWL_E_FIRMWARE and the firmware's status in *FwStatus. In and Out must
 * not point into the lower half of the bounce buffer; the upper half
 * (BwlScratch) is free for building In.
 */
int  BwlDcmd(bwl_dev *Dev, u32 Cmd, int Set, const u8 *In, u32 InLen, u8 *Out, u32 OutCap,
             u32 *OutLen, int *FwStatus, u32 TimeoutMs);

/* The upper half of the bounce buffer: scratch for iovar payloads. */
#define BwlScratch(Dev)     ((Dev)->Bounce + (Dev)->BounceSize / 2)

int  BwlGetVar(bwl_dev *Dev, const char *Name, u8 *Out, u32 Cap, u32 *OutLen, int *FwStatus);
int  BwlSetVar(bwl_dev *Dev, const char *Name, const u8 *Val, u32 Len, int *FwStatus);
int  BwlGetInt(bwl_dev *Dev, u32 Cmd, u32 *Val, int *FwStatus);

/* clmload in BWL_DLOAD_MAX chunks, then clmload_status. Returns 0 or an error. */
int  BwlClmLoad(bwl_dev *Dev, const u8 *Blob, u32 Len, u32 *Chunks, u32 *ClmStatus, int *FwStatus);

/* Drain whatever frames the firmware has queued (events at start-up). */
int  BwlPollOnce(bwl_dev *Dev);

/*
 * The poller's step: one look at the SDIO core and every frame it has,
 * events handled (bwl_wlan.c) and data frames handed to Net->rx_eth; then
 * what an event left to do (the association's elements). Not from inside
 * a Net callback. Returns the number of frames read, or a negative error.
 */
int  BwlService(bwl_dev *Dev);

/*
 * One Ethernet frame to the firmware on the data channel (BCDC header,
 * SDPCM channel 2). BWL_E_CREDIT: no window or flow control is on, try
 * again after the next BwlService. Prio is the 802.1D priority, 0..7.
 */
int  BwlTxEth(bwl_dev *Dev, const u8 *Eth, u32 Len, unsigned Prio);
int  BwlTxReady(const bwl_dev *Dev, unsigned Prio);

/* ------------------------------------------------------------ bwl_wlan.c */

/*
 * After the firmware runs (BwlStart, the CLM): the event mask, power save
 * and roaming as this driver wants them, infrastructure mode, C_UP, and
 * the chip's address, encoding and bands into Dev->Wlan. Country: two
 * letters, or NULL for the CLM's default.
 */
int  BwlWlanInit(bwl_dev *Dev, const char *Country);

/* escan: every channel, active, the wildcard SSID. Results come through Net->scan_bss. */
int  BwlScanStart(bwl_dev *Dev);

/*
 * Join Bss (BSSID, SSID, control channel). Open: no security. WPA2-PSK:
 * CCMP, our RSN element; Passphrase NULL leaves the handshake to the host
 * (sup_wpa 0: EAPOL comes up as data, keys with BwlSetKey), else the
 * firmware runs it (sup_wpa 1, C_SET_WSEC_PMK with the passphrase, 8 to 63
 * characters, or 64 hex digits). The result comes as BWL_EV_ASSOC (and
 * BWL_EV_KEYED with the firmware supplicant).
 */
int  BwlConnect(bwl_dev *Dev, const bwl_bss *Bss, const char *Passphrase);

/* A CCMP key from the host supplicant: pairwise (index 0, the AP's address) or group. */
int  BwlSetKey(bwl_dev *Dev, int Pairwise, unsigned KeyIdx, const u8 *Key, u32 Len);

int  BwlDisconnect(bwl_dev *Dev, u16 Reason);

/* Rate and RSSI of the current link into Dev->Wlan. */
int  BwlLinkInfo(bwl_dev *Dev);

/* The event handler (bwl_ctl.c calls it for every firmware event). */
void BwlWlanEvent(bwl_dev *Dev, const bwl_event *E);

/* What happens after the event handler, outside the receive path. */
int  BwlWlanService(bwl_dev *Dev);

/* Association request and response bodies, as Windows wants them in an
 * association result: fixed fields, then the elements the firmware sent and got. */
u32  BwlAssocReqBody(const bwl_dev *Dev, u8 *Out, u32 Max);
u32  BwlAssocRspBody(const bwl_dev *Dev, u8 *Out, u32 Max);

/* ------------------------------------------------------------ bwl_data.c (pure functions) */

/* A chanspec for a 20 MHz channel; the control channel and band of any chanspec. */
u16  BwlChanspec(u32 IoType, unsigned Channel, unsigned Band);
void BwlChanspecDecode(u32 IoType, u16 Cs, u8 *Channel, u8 *Band);

/*
 * A wl_bss_info record (Len bytes, as escan delivers it) to a bwl_bss and
 * the frame body Windows wants (8 zero bytes of Timestamp, interval,
 * capability, elements; the SSID and rates built from the record when it
 * has no elements). 0, or a negative error.
 */
int  BwlBssInfoParse(const u8 *Bi, u32 Len, u32 IoType, bwl_bss *Out, u8 *Body, u32 BodyMax, u32 *BodyLen);

/* A beacon or probe response body (from the Timestamp on) to a bwl_bss. */
int  BwlBssFromBody(const u8 *Bssid, unsigned Channel, unsigned Band, int Rssi, const u8 *Body, u32 Len,
                    bwl_bss *Out);

/*
 * The SDIO function an sdbus device ID names: "...&FN_<d>" followed by the
 * end, '\\', '&' or a NUL (a multi-string is searched whole). S is UTF-16
 * (Chars units, NUL or not). Returns 0..7, or -1 when no ID names one.
 */
int  BwlFindFn(const u16 *S, u32 Chars);

/* The element with this ID in Ies, or NULL. */
const u8 *BwlFindIe(const u8 *Ies, u32 Len, u8 Id);

/* What an RSN element (or its absence, with the privacy bit) allows: enum bwl_sec. */
int  BwlRsnSecurity(const u8 *Rsn, u16 Capability);

/* Our RSN element: CCMP group and pairwise, PSK. Returns its length. */
u32  BwlBuildRsnIe(u8 *Out, u32 Max);

/*
 * Native 802.11 data frames as WiFiCx hands them over and wants them back
 * (the same conversions as aic8800usb's aic_data.c).
 * BwlEthTo80211: an Ethernet frame from the firmware to a 24-byte header
 *   with From DS (A1 DA, A2 Bssid, A3 SA) plus RFC 1042 / 802.1H SNAP.
 * Bwl80211ToEth: a To DS data frame from Windows (QoS, +HTC allowed, not
 *   protected) to Ethernet.
 * Both return the length, 0 for a frame not to pass, or a negative error.
 */
#define BWL_80211_HDR   24u
int  BwlEthTo80211(const u8 *Bssid, const u8 *Eth, u32 EthLen, u8 *Out, u32 OutMax);
int  Bwl80211ToEth(const u8 *Frame, u32 Len, u8 *Out, u32 OutMax);

/* A FIFO of frames between the poller and the OS's queues; the driver locks around each call. */
#define BWL_FRAMEQ_SLOTS 64
#define BWL_FRAMEQ_MAX   1600
typedef struct bwl_frameq {
    u8  Buf[BWL_FRAMEQ_SLOTS][BWL_FRAMEQ_MAX];
    u16 Len[BWL_FRAMEQ_SLOTS];
    u8  Prio[BWL_FRAMEQ_SLOTS];
    u32 Head, Tail;
    u32 Drops;
} bwl_frameq;

int  BwlFrameqPush(bwl_frameq *Q, const u8 *Frame, u32 Len, unsigned Prio);
const u8 *BwlFrameqPeek(const bwl_frameq *Q, u32 *Len, unsigned *Prio);
void BwlFrameqPop(bwl_frameq *Q);
u32  BwlFrameqCount(const bwl_frameq *Q);

/* ------------------------------------------------------------ small helpers */

void BwlMemCpy(void *Dst, const void *Src, u32 Len);
void BwlMemSet(void *Dst, u8 Val, u32 Len);
int  BwlMemCmp(const void *A, const void *B, u32 Len);
u32  BwlStrLen(const char *S);

BWL_INLINE u16 BwlGetLe16(const u8 *P) { return (u16)(P[0] | (P[1] << 8)); }
BWL_INLINE u32 BwlGetLe32(const u8 *P) { return (u32)P[0] | ((u32)P[1] << 8) | ((u32)P[2] << 16) | ((u32)P[3] << 24); }
BWL_INLINE u16 BwlGetBe16(const u8 *P) { return (u16)((P[0] << 8) | P[1]); }
BWL_INLINE u32 BwlGetBe32(const u8 *P) { return ((u32)P[0] << 24) | ((u32)P[1] << 16) | ((u32)P[2] << 8) | (u32)P[3]; }
BWL_INLINE void BwlPutLe16(u8 *P, u16 V) { P[0] = (u8)V; P[1] = (u8)(V >> 8); }
BWL_INLINE void BwlPutLe32(u8 *P, u32 V) { P[0] = (u8)V; P[1] = (u8)(V >> 8); P[2] = (u8)(V >> 16); P[3] = (u8)(V >> 24); }
