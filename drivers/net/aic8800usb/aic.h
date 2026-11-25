/*
 * aic.h - AICSemi AIC8800D80 USB Wi-Fi core: the parts of the driver that do
 * not depend on Windows.
 *
 * Everything declared here is plain C with no Windows or libc dependency.
 * The same files build into aic8800usb.sys, where the bus operations are
 * bulk transfers through KMDF's USB target, and into tools/aictest, where
 * they are a simulated chip and access point. CI runs the simulation on
 * Linux with gcc.
 *
 * The protocol (message framing, message IDs and parameter layouts, the
 * firmware download sequence, the data descriptors) follows AICSemi's
 * GPL-2.0 Linux driver (aic_load_fw and aic8800_fdrv, as packaged by Radxa
 * in radxa-pkg/aic8800); see LICENSING.md and aic_proto.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#ifdef _MSC_VER
#define AIC_INLINE static __inline
/* Byte arithmetic (u8 = u8 ^ u8 and the like) goes through int by C's
 * promotions, which MSVC reports as C4244 at /W4. The core is built with
 * gcc -Wconversion -Werror in CI (tools/aictest), which checks the real
 * narrowing cases, so MSVC's version of the warning is off for it. */
#pragma warning(disable : 4244)
#else
#define AIC_INLINE static inline
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed char        s8;
typedef unsigned long long u64;

/* Return codes. 0 is success; everything else is negative. */
#define AIC_OK            0
#define AIC_E_IO         -1     /* a bulk transfer failed */
#define AIC_E_TIMEOUT    -2     /* no confirm or indication in time */
#define AIC_E_NOCHIP     -3     /* chip ID not one this driver knows */
#define AIC_E_FORMAT     -4     /* a file or a frame is malformed */
#define AIC_E_SPACE      -5     /* buffer too small */
#define AIC_E_FIRMWARE   -6     /* the firmware answered with an error status */
#define AIC_E_STATE      -7     /* called in the wrong state */
#define AIC_E_NOTFOUND   -8     /* network not in the scan list */
#define AIC_E_AUTH       -9     /* association or 4-way handshake failed */
#define AIC_E_MIC        -10    /* EAPOL-Key MIC did not verify (wrong passphrase?) */

/* ------------------------------------------------------------ bus ops
 *
 * send:     one bulk OUT transfer. Msg = 1 for a control message (the
 *           message endpoint if the device has one, else the data endpoint),
 *           0 for a data frame. Buf/Len is the whole USB frame.
 * wait:     block for at most Ms milliseconds or until the bus side has
 *           received something; then hand every received bulk IN transfer
 *           to aic_rx() before returning. Never called from inside aic_rx().
 * delay_ms: sleep.
 * log:      optional, one short line for the debugger.
 * rx_eth:   optional, an Ethernet frame for the operating system (not
 *           EAPOL, which the core keeps).
 */
typedef struct aic_bus_ops {
    int  (*send)(void *Ctx, int Msg, const u8 *Buf, u32 Len);
    void (*wait)(void *Ctx, u32 Ms);
    void (*delay_ms)(void *Ctx, u32 Ms);
    void (*log)(void *Ctx, const char *Msg);
    void (*rx_eth)(void *Ctx, const u8 *Frame, u32 Len);
} aic_bus_ops;

/* A firmware file, loaded by the caller (the kernel reads it from disk). */
typedef struct aic_file {
    const u8 *Data;
    u32 Len;
} aic_file;

/* The files aic_fw_download() needs, by role. Ext[i] is
 * fw_patch_8800d80_u02_ext<i>.bin, only as many as the patch table asks for. */
#define AIC_MAX_EXT_PATCH 4
typedef struct aic_fw_files {
    aic_file PatchTable;    /* fw_patch_table_8800d80_u02.bin */
    aic_file Adid;          /* fw_adid_8800d80_u02.bin */
    aic_file Patch;         /* fw_patch_8800d80_u02.bin */
    aic_file Ext[AIC_MAX_EXT_PATCH];
    aic_file Fmac;          /* fmacfw_8800d80_u02.bin, or _h_u02 on an H chip */
} aic_fw_files;

/* What aic_fw_parse_table() finds in the patch table (AICBT_PINF_T). */
typedef struct aic_patch_info {
    u32 AddrAdid;
    u32 AddrPatch;
    u32 ExtCount;
    u32 ExtId[AIC_MAX_EXT_PATCH];
    u32 ExtAddr[AIC_MAX_EXT_PATCH];
    u32 Sections;           /* entries in the table */
    u32 Writes;             /* (address, value) pairs it will write */
    char Version[64];       /* AICBT_VER_INFO text, if any */
} aic_patch_info;

/* One network from a scan. */
#define AIC_MAX_BSS      48
#define AIC_MAX_RSN_IE   64
typedef struct aic_bss {
    u8  Bssid[6];
    u8  SsidLen;
    u8  Ssid[32];
    u16 Freq;               /* MHz */
    u8  Band;               /* 0 = 2.4 GHz, 1 = 5 GHz */
    s8  Rssi;
    u16 Capability;
    u8  Security;           /* AIC_SEC_* */
    u8  GroupCipher;        /* RSN suite type, 4 = CCMP, 2 = TKIP */
    u8  RsnIeLen;
    u8  RsnIe[AIC_MAX_RSN_IE];
    u32 Seen;               /* how many result frames named it */
} aic_bss;

#define AIC_SEC_OPEN        0
#define AIC_SEC_WEP         1
#define AIC_SEC_WPA         2   /* WPA1 only: not supported for connect */
#define AIC_SEC_WPA2_PSK    3   /* RSN with PSK and CCMP pairwise */
#define AIC_SEC_RSN_OTHER   4   /* RSN, but SAE/802.1X only or no CCMP */

/* Link and handshake state. */
enum aic_link {
    AIC_LINK_DOWN = 0,
    AIC_LINK_ASSOCIATING,
    AIC_LINK_ASSOCIATED,        /* SM_CONNECT_IND ok, handshake running */
    AIC_LINK_UP,                /* keys installed, port open */
};

typedef struct aic_stats {
    u32 TxMsg;
    u32 RxMsg;
    u32 RxInd;
    u32 RxData;
    u32 RxDataDropped;
    u32 RxEapol;
    u32 TxData;
    u32 RxPrint;
    u32 RxDataCfm;
    u32 RxErrors;
    u32 RxUnknown;
    u32 LastUnknownId;
    u32 FwPanic;
    u32 ScanResults;
    u32 MicFailures;
} aic_stats;

/* The 4-way handshake (aic_wpa.c). */
typedef struct aic_wpa {
    u8  Pmk[32];
    u8  HavePmk;
    u8  Anonce[32];
    u8  Snonce[32];
    u8  Ptk[48];            /* KCK 16 | KEK 16 | TK 16 */
    u8  HavePtk;
    u8  Gtk[32];
    u8  GtkLen;
    u8  GtkIdx;
    u8  ReplayValid;
    u8  Replay[8];
    u8  Done;               /* pairwise and group keys installed */
    u8  Msg;                /* last message number handled (1..4) */
    u32 NonceCounter;
} aic_wpa;

#define AIC_EAPOL_QUEUE   4
#define AIC_EAPOL_MAX     512

typedef struct aic_dev {
    const aic_bus_ops *Ops;
    void *Ctx;

    /* Set by the driver when the device is stopping: every wait gives up. */
    volatile int Abort;

    /* Scratch for one outgoing USB frame. Size >= AIC_TXBUF_MIN. */
    u8  *TxBuf;
    u32  TxBufSize;

    /* The command waiting for its confirm. */
    volatile u16 CfmWaitId;     /* 0xFFFF when none */
    volatile u8  CfmDone;
    u8  *CfmBuf;
    u32  CfmLen;                /* capacity of CfmBuf; on completion, bytes copied */
    u32  CfmTimeoutMs;

    /* Chip, from DBG_MEM_READ 0x40500000 and 0x00000004 */
    u32 ChipWord;
    u8  ChipRev;                /* bits 23:16: 3 = U02, 7 = U03 */
    u8  ChipSubRev;
    u8  ChipIsH;                /* (ChipRev & 0xC0) == 0xC0: load fmacfw_..._h_u02 */
    u8  ChipMcu;
    u32 FwVersionWord;          /* RAM 0x12001C after fmacfw is in place */

    /* Runtime firmware */
    char FwVersion[64];
    u8  Mac[6];
    u8  Is5gSupported;
    u8  VendorInfo;
    u32 LmacVersion;
    u32 Features;
    u16 MaxSta;
    u8  MaxVif;
    u8  VifIdx;
    u8  VifUp;

    /* Scan */
    aic_bss Bss[AIC_MAX_BSS];
    u32 BssCount;
    volatile u8 ScanDone;
    u8  ScanStatus;
    u8  ScanResultCnt;

    /* Link */
    volatile enum aic_link Link;
    u8  ApIdx;                  /* firmware station index of the AP */
    u8  ApQos;
    u16 ConnectStatus;          /* 802.11 status code from SM_CONNECT_IND */
    u16 DisconnectReason;
    u8  Bssid[6];
    u16 LinkFreq;
    u16 Aid;
    u8  AssocRsnIe[AIC_MAX_RSN_IE];
    u8  AssocRsnIeLen;
    u8  CurBss;                 /* index into Bss of the network joined */

    aic_wpa Wpa;

    /* EAPOL frames from aic_rx(), handled by aic_poll(). Each one is an
     * Ethernet frame (destination, source, 0x888E, EAPOL). */
    u8  EapolBuf[AIC_EAPOL_QUEUE][AIC_EAPOL_MAX];
    u16 EapolLen[AIC_EAPOL_QUEUE];
    u8  EapolHead, EapolTail;

    aic_stats Stats;
    char LastError[96];

    /* Parameter scratch for the larger requests (DBG_MEM_BLOCK_WRITE is
     * 1032 bytes, SM_CONNECT 320, SCANU_START 376). */
    u8  Scratch[1040];

    /* One received frame converted to Ethernet (an MSDU is at most 2304). */
    u8  RxEth[2400];
} aic_dev;

#define AIC_TXBUF_MIN 2048

/* ---------------------------------------------------------------- core */

/* Fill Dev's fixed fields; Ops/Ctx/TxBuf must be set first. */
void aic_init(aic_dev *Dev);

/* Every bulk IN transfer goes here, whole (it may hold several packets). */
void aic_rx(aic_dev *Dev, const u8 *Buf, u32 Len);

/* Send one LMAC message and wait for the confirm CfmId (if CfmId is not
 * 0xFFFF), copying up to CfmLen bytes of its parameters to Cfm. */
int aic_cmd(aic_dev *Dev, u16 Id, u16 DestTask, const void *Param, u16 ParamLen,
            u16 CfmId, void *Cfm, u32 CfmLen);

/* Debug-task memory access, valid in both the loader and runtime images. */
int aic_mem_read(aic_dev *Dev, u32 Addr, u32 *Val);
int aic_mem_write(aic_dev *Dev, u32 Addr, u32 Val);
int aic_mem_block_write(aic_dev *Dev, u32 Addr, const u8 *Data, u32 Len);

/* ------------------------------------------------- firmware (aic_fw.c) */

/* Read the chip ID words; Dev->ChipRev etc. */
int aic_identify(aic_dev *Dev);

/* Parse fw_patch_table_*.bin without touching the chip. */
int aic_fw_parse_table(const aic_file *Table, aic_patch_info *Info);

/* The whole download, as aic_load_fw does it for an AIC8800D80 U02/U03,
 * ending with DBG_START_APP. On success the device leaves the bus and comes
 * back as VID_A69C&PID_8D81. Info may be NULL. */
int aic_fw_download(aic_dev *Dev, const aic_fw_files *Files, aic_patch_info *Info);

/* Ask the runtime image to reboot into the ROM loader (PID_8D80). */
int aic_fw_reboot(aic_dev *Dev);

/* ---------------------------------------------- runtime (aic_wlan.c) */

/* After the re-enumeration: stack start, version, calibration, MAC,
 * reset, version, ME config, channel config, start, add a station VIF. */
int aic_wlan_init(aic_dev *Dev);

/* Active scan of every 2.4 GHz channel (and 5 GHz if the chip has it),
 * results in Dev->Bss. */
int aic_scan(aic_dev *Dev, u32 TimeoutMs);

/* Index of the strongest network named Ssid, or -1. */
int aic_find_bss(const aic_dev *Dev, const u8 *Ssid, u32 SsidLen);

/* Join Bss[Index] with the PMK in Dev->Wpa (WPA2-PSK) or open; returns when
 * the link is up (keys installed, port open) or on failure. */
int aic_connect(aic_dev *Dev, int Index, u32 TimeoutMs);

int aic_disconnect(aic_dev *Dev);

/* MM_KEY_ADD: the pairwise key for the AP, or a group key (Pairwise 0). */
int aic_key_add(aic_dev *Dev, int Pairwise, u8 KeyIdx, const u8 *Key, u32 KeyLen, u8 Cipher);

/* ME_SET_CONTROL_PORT for the AP's station entry. */
int aic_set_port(aic_dev *Dev, int Open);

/* Handle queued EAPOL frames (the 4-way and group key handshakes). Call
 * from the thread that also calls aic_cmd; returns 1 if it did something. */
int aic_poll(aic_dev *Dev);

/* ------------------------------------------------- data (aic_data.c) */

/* An Ethernet frame (destination, source, type, payload) to a USB data
 * frame: header, host descriptor, payload. Returns the USB frame length or
 * a negative error. Pure function, safe on any thread. */
int aic_build_tx(const aic_dev *Dev, const u8 *Eth, u32 EthLen, u8 *Out, u32 OutMax);

/* Build and send. */
int aic_tx_eth(aic_dev *Dev, const u8 *Eth, u32 EthLen);

/* One received data packet (starting at the 60-byte RX header) to an
 * Ethernet frame. Returns the frame length, 0 for a frame to drop, or a
 * negative error. Pure function. */
int aic_rx_to_eth(const u8 *Pkt, u32 PktLen, u8 *Out, u32 OutMax);

/* -------------------------------------------------- WPA2 (aic_wpa.c) */

/* PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32). */
void aic_wpa_pmk(const char *Passphrase, u32 PassLen, const u8 *Ssid, u32 SsidLen, u8 Pmk[32]);

/* Called by aic_poll for each EAPOL frame. */
int aic_wpa_rx(aic_dev *Dev, const u8 *Eth, u32 Len);

/* Our RSN IE for the association request, from the AP's. */
u32 aic_wpa_build_rsn_ie(const aic_bss *Bss, u8 *Out, u32 OutMax);

/* ---------------------------------------------- crypto (aic_crypto.c) */

typedef struct aic_sha1 {
    u32 H[5];
    u64 Len;
    u8  Buf[64];
    u32 BufLen;
} aic_sha1;

void aic_sha1_init(aic_sha1 *S);
void aic_sha1_update(aic_sha1 *S, const u8 *Data, u32 Len);
void aic_sha1_final(aic_sha1 *S, u8 Out[20]);
void aic_hmac_sha1(const u8 *Key, u32 KeyLen, const u8 *Data, u32 Len, u8 Out[20]);
/* HMAC over the concatenation of Count buffers. */
void aic_hmac_sha1_vec(const u8 *Key, u32 KeyLen, u32 Count, const u8 *const *Data,
                       const u32 *Len, u8 Out[20]);
void aic_pbkdf2_sha1(const u8 *Pass, u32 PassLen, const u8 *Salt, u32 SaltLen,
                     u32 Iter, u8 *Out, u32 OutLen);
/* IEEE 802.11 PRF-n with HMAC-SHA1: Label, then a zero byte, then Data. */
void aic_prf_sha1(const u8 *Key, u32 KeyLen, const char *Label, const u8 *Data,
                  u32 DataLen, u8 *Out, u32 OutLen);
void aic_aes128_encrypt(const u8 Key[16], const u8 In[16], u8 Out[16]);
void aic_aes128_decrypt(const u8 Key[16], const u8 In[16], u8 Out[16]);
/* RFC 3394 unwrap; InLen is a multiple of 8, >= 24. Out gets InLen - 8
 * bytes. Returns 0, or AIC_E_MIC if the integrity check fails. */
int  aic_aes_unwrap(const u8 Kek[16], const u8 *In, u32 InLen, u8 *Out);
/* RFC 3394 wrap (the simulated access point uses it). */
void aic_aes_wrap(const u8 Kek[16], const u8 *In, u32 InLen, u8 *Out);

/* ----------------------------------------------------------- helpers */

void aic_memcpy(void *Dst, const void *Src, u32 Len);
void aic_memset(void *Dst, int Val, u32 Len);
int  aic_memcmp(const void *A, const void *B, u32 Len);
void aic_log(aic_dev *Dev, const char *Fmt, u32 A, u32 B);
void aic_set_error(aic_dev *Dev, const char *Msg, u32 Val);

AIC_INLINE u16 aic_get16(const u8 *P) { return (u16)(P[0] | (P[1] << 8)); }
AIC_INLINE u32 aic_get32(const u8 *P)
{
    return (u32)P[0] | ((u32)P[1] << 8) | ((u32)P[2] << 16) | ((u32)P[3] << 24);
}
AIC_INLINE void aic_put16(u8 *P, u32 V) { P[0] = (u8)V; P[1] = (u8)(V >> 8); }
AIC_INLINE void aic_put32(u8 *P, u32 V)
{
    P[0] = (u8)V; P[1] = (u8)(V >> 8); P[2] = (u8)(V >> 16); P[3] = (u8)(V >> 24);
}
AIC_INLINE u16 aic_be16(const u8 *P) { return (u16)((P[0] << 8) | P[1]); }
AIC_INLINE void aic_put_be16(u8 *P, u32 V) { P[0] = (u8)(V >> 8); P[1] = (u8)V; }
