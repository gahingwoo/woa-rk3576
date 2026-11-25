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
 * half). Dst must hold SrcLen + 8 bytes. Returns the length, or a negative
 * error.
 */
int  BwlNvramConvert(const u8 *Src, u32 SrcLen, u8 *Dst, u32 DstCap);
#define BWL_NVRAM_SLACK 8u

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
