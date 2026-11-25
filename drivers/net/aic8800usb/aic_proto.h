/*
 * aic_proto.h - the AIC8800 USB protocol: frame types, LMAC message IDs,
 * parameter layouts (as byte offsets) and the addresses the firmware
 * download uses.
 *
 * Source: AICSemi's Linux driver as packaged in radxa-pkg/aic8800 (commit
 * 5562c254b0231be34588dc608ec28c4f1d613285): src/USB/driver_fw/drivers/
 * aic8800/aic_load_fw (aicwf_usb.h, aic_compat_8800d80.[ch],
 * aicbluetooth.c, aicbluetooth_cmds.[ch]) and aic8800_fdrv (lmac_msg.h,
 * lmac_mac.h, ipc_shared.h, rwnx_rx.h, rwnx_msg_tx.c, aicwf_usb.c,
 * rwnx_tx.c, rwnx_rx.c). Those files are "Copyright (C) RivieraWaves
 * 2011-2019" and "Copyright (C) AICSemi 2018-2020", MODULE_LICENSE("GPL"),
 * and the package's debian/copyright says that the files under src/ are "License: GPL-2".
 *
 * The layouts are written as offsets into a byte buffer, not C structs, so
 * that they do not depend on the compiler's packing. tools/aictest checks
 * them against the vendor headers (layoutcheck.c, CI only).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

/* ----------------------------------------------------- USB identities */

#define AIC_USB_VID             0xA69C
#define AIC_USB_PID_D80_LOADER  0x8D80  /* ROM, one vendor interface */
#define AIC_USB_PID_D80_RUNTIME 0x8D81  /* firmware: BT (MI_00, MI_01), WLAN (MI_02) */

/* -------------------------------------------------------- USB framing
 *
 * Every bulk transfer is one or more packets. A packet starts with
 *   u16 len (12 bits), u8 type, u8 reserved
 * Host to device:
 *   control message: len = 4 + 8 + param_len, type 0x11, then a dummy word,
 *                    then the LMAC header (id, dest, src, param_len) and
 *                    the parameters; 8 + 8 + param_len bytes in all
 *   data frame:      len = whole frame, type 0x01, then the host descriptor
 *                    and the payload, padded to 4 bytes
 * Device to host:
 *   type & 0x10 == 0: a data packet; the 60-byte RX header starts at the
 *                    packet (len is its first field), then len bytes of
 *                    802.11 frame; the next packet starts 4-byte aligned
 *   type & 0x7F 0x11: a message: E2A header at +4 (id, dest, src,
 *                    param_len, pattern), parameters at +16; next packet at
 *                    +4 + roundup(len, 4)
 *   0x12: a TX confirm, 0x13: firmware console text
 */
#define AIC_USB_TYPE_DATA       0x00
#define AIC_USB_TYPE_CFG        0x10
#define AIC_USB_TYPE_CMD_RSP    0x11
#define AIC_USB_TYPE_DATA_CFM   0x12
#define AIC_USB_TYPE_PRINT      0x13
#define AIC_USB_TX_TYPE_DATA    0x01

#define AIC_USB_HDR             4
#define AIC_A2E_HDR             8       /* after the 4-byte dummy word */
#define AIC_E2A_HDR             12      /* id, dest, src, param_len, pattern */
#define AIC_RX_HDR_LEN          60      /* struct hw_rxhdr (56) + 2 + 2 */
#define AIC_CMD_BUF_MAX         1536
#define AIC_E2A_PARAM_MAX       1024

/* ----------------------------------------------------------- tasks */

#define AIC_TASK_MM     0
#define AIC_TASK_DBG    1
#define AIC_TASK_SCANU  4
#define AIC_TASK_ME     5
#define AIC_TASK_SM     6
#define AIC_DRV_TASK_ID 100
#define AIC_MSG(task, i) ((u16)(((task) << 10) | (i)))
#define AIC_NO_CFM      0xFFFF

/* --------------------------------------------------------- messages */

#define AIC_MM_RESET_REQ            0x0000
#define AIC_MM_RESET_CFM            0x0001
#define AIC_MM_START_REQ            0x0002
#define AIC_MM_START_CFM            0x0003
#define AIC_MM_VERSION_REQ          0x0004
#define AIC_MM_VERSION_CFM          0x0005
#define AIC_MM_ADD_IF_REQ           0x0006
#define AIC_MM_ADD_IF_CFM           0x0007
#define AIC_MM_KEY_ADD_REQ          0x0024
#define AIC_MM_KEY_ADD_CFM          0x0025
#define AIC_MM_KEY_DEL_REQ          0x0026
#define AIC_MM_KEY_DEL_CFM          0x0027
#define AIC_MM_CHANNEL_SWITCH_IND   0x0044
#define AIC_MM_CHANNEL_PRE_SWITCH_IND 0x0045
#define AIC_MM_PS_CHANGE_IND        0x0049
#define AIC_MM_TRAFFIC_REQ_IND      0x004A
#define AIC_MM_P2P_VIF_PS_CHANGE_IND 0x004D
#define AIC_MM_SET_RF_CALIB_REQ     0x0069
#define AIC_MM_SET_RF_CALIB_CFM     0x006A
#define AIC_MM_GET_MAC_ADDR_REQ     0x0073
#define AIC_MM_GET_MAC_ADDR_CFM     0x0074
#define AIC_MM_SET_STACK_START_REQ  0x007B
#define AIC_MM_SET_STACK_START_CFM  0x007C
#define AIC_MM_GET_FW_VERSION_REQ   0x0080
#define AIC_MM_GET_FW_VERSION_CFM   0x0081
#define AIC_MM_FW_PANIC_IND         0x0095
#define AIC_MM_FW_ASSERT_IND        0x0096

#define AIC_DBG_MEM_READ_REQ        0x0400
#define AIC_DBG_MEM_READ_CFM        0x0401
#define AIC_DBG_MEM_WRITE_REQ       0x0402
#define AIC_DBG_MEM_WRITE_CFM       0x0403
#define AIC_DBG_MEM_BLOCK_WRITE_REQ 0x040B
#define AIC_DBG_MEM_BLOCK_WRITE_CFM 0x040C
#define AIC_DBG_START_APP_REQ       0x040D
#define AIC_DBG_START_APP_CFM       0x040E
#define AIC_DBG_MEM_MASK_WRITE_REQ  0x0411
#define AIC_DBG_MEM_MASK_WRITE_CFM  0x0412

#define AIC_SCANU_START_REQ         0x1000
#define AIC_SCANU_START_CFM         0x1001
#define AIC_SCANU_RESULT_IND        0x1004
#define AIC_SCANU_START_CFM_ADDTIONAL 0x1009   /* sic, the vendor's spelling */

#define AIC_ME_CONFIG_REQ           0x1400
#define AIC_ME_CONFIG_CFM           0x1401
#define AIC_ME_CHAN_CONFIG_REQ      0x1402
#define AIC_ME_CHAN_CONFIG_CFM      0x1403
#define AIC_ME_SET_CONTROL_PORT_REQ 0x1404
#define AIC_ME_SET_CONTROL_PORT_CFM 0x1405
#define AIC_ME_TX_CREDITS_UPDATE_IND 0x140B

#define AIC_SM_CONNECT_REQ          0x1800
#define AIC_SM_CONNECT_CFM          0x1801
#define AIC_SM_CONNECT_IND          0x1802
#define AIC_SM_DISCONNECT_REQ       0x1803
#define AIC_SM_DISCONNECT_CFM       0x1804
#define AIC_SM_DISCONNECT_IND       0x1805

/* ---------------------------------------------- parameter layouts */

/* DBG */
#define AIC_DBG_MEM_READ_REQ_LEN        4       /* memaddr */
#define AIC_DBG_MEM_READ_CFM_LEN        8       /* memaddr, memdata */
#define AIC_DBG_MEM_WRITE_REQ_LEN       8       /* memaddr, memdata */
#define AIC_DBG_MEM_MASK_WRITE_REQ_LEN  12      /* memaddr, memmask, memdata */
#define AIC_DBG_BLOCK_MAX               1024
#define AIC_DBG_MEM_BLOCK_WRITE_REQ_LEN (8 + AIC_DBG_BLOCK_MAX) /* memaddr, memsize, memdata[256] */
#define AIC_DBG_START_APP_REQ_LEN       8       /* bootaddr, boottype */
#define AIC_HOST_START_APP_AUTO         1
#define AIC_HOST_START_APP_REBOOT       3

/* MM_START_REQ: phy_cfg (16 words), uapsd_timeout, lp_clk_accuracy */
#define AIC_MM_START_REQ_LEN            72
#define AIC_MM_START_UAPSD_TIMEOUT      64
#define AIC_MM_START_LP_CLK             68

/* MM_ADD_IF_REQ: type, addr[6] (at 2, u16 aligned), p2p */
#define AIC_MM_ADD_IF_REQ_LEN           10
#define AIC_MM_ADD_IF_TYPE              0
#define AIC_MM_ADD_IF_ADDR              2
#define AIC_MM_ADD_IF_P2P               8
#define AIC_MM_STA                      0
/* MM_ADD_IF_CFM: status, inst_nbr */

/* MM_VERSION_CFM: version_lmac, machw_1, machw_2, phy_1, phy_2, features,
 * max_sta_nb (u16), max_vif_nb */
#define AIC_MM_VERSION_CFM_LEN          28

/* MM_KEY_ADD_REQ: key_idx, sta_idx, key {length u8, array u32[8]} at 4,
 * cipher_suite 40, inst_nbr 41, spp 42, pairwise 43 */
#define AIC_MM_KEY_ADD_REQ_LEN          44
#define AIC_KEY_ADD_KEY_IDX             0
#define AIC_KEY_ADD_STA_IDX             1
#define AIC_KEY_ADD_KEY_LEN             4
#define AIC_KEY_ADD_KEY                 8
#define AIC_KEY_ADD_CIPHER              40
#define AIC_KEY_ADD_INST                41
#define AIC_KEY_ADD_SPP                 42
#define AIC_KEY_ADD_PAIRWISE            43
#define AIC_MAC_CIPHER_TKIP             1
#define AIC_MAC_CIPHER_CCMP             2
#define AIC_INVALID_STA                 0xFF

/* MM_SET_STACK_START_REQ: is_stack_start, efuse_valid, set_vendor_info,
 * fwtrace_redir. CFM: is_5g_support, vendor_info */
#define AIC_MM_SET_STACK_START_REQ_LEN  4

/* MM_GET_FW_VERSION_CFM: fw_version_len, fw_version[63] */
#define AIC_MM_GET_FW_VERSION_CFM_LEN   64

/* MM_SET_RF_CALIB_REQ: cal_cfg_24g, cal_cfg_5g, param_alpha, bt_calib_en,
 * bt_calib_param, xtal_cap, xtal_cap_fine; CFM is four addresses */
#define AIC_MM_SET_RF_CALIB_REQ_LEN     24
#define AIC_MM_SET_RF_CALIB_CFM_LEN     16

/* ME_CONFIG_REQ: ht_cap (32) at 0, vht_cap (12) at 32, he_cap (56) at 44,
 * tx_lft (u16) 100, phy_bw_max 102, ht_supp 103, vht_supp 104, he_supp 105,
 * he_ul_on 106, ps_on 107, ant_div_on 108, dpsm 109 */
#define AIC_ME_CONFIG_REQ_LEN           112
#define AIC_ME_CONFIG_HT_CAPA_INFO      0       /* u16 */
#define AIC_ME_CONFIG_HT_AMPDU_PARAM    2
#define AIC_ME_CONFIG_HT_MCS            3       /* 16 bytes */
#define AIC_ME_CONFIG_TX_LFT            100
#define AIC_ME_CONFIG_PHY_BW_MAX        102
#define AIC_ME_CONFIG_HT_SUPP           103
#define AIC_ME_CONFIG_VHT_SUPP          104
#define AIC_ME_CONFIG_HE_SUPP           105
#define AIC_ME_CONFIG_HE_UL_ON          106
#define AIC_ME_CONFIG_PS_ON             107
#define AIC_ME_CONFIG_ANT_DIV           108
#define AIC_ME_CONFIG_DPSM              109

/* struct mac_chan_def: freq (u16), band, flags, tx_power (s8); 6 bytes */
#define AIC_CHAN_DEF_LEN                6
#define AIC_MAX_CHAN_24G                14
#define AIC_MAX_CHAN_5G                 28

/* ME_CHAN_CONFIG_REQ: chan2G4[14] at 0, chan5G[28] at 84, chan2G4_cnt 252,
 * chan5G_cnt 253 */
#define AIC_ME_CHAN_CONFIG_REQ_LEN      254
#define AIC_ME_CHAN_CONFIG_5G           84
#define AIC_ME_CHAN_CONFIG_CNT24        252
#define AIC_ME_CHAN_CONFIG_CNT5         253

/* ME_SET_CONTROL_PORT_REQ: sta_idx, control_port_open */
#define AIC_ME_SET_CONTROL_PORT_REQ_LEN 2

/* SCANU_START_REQ: chan[42] at 0, ssid[3] (33 bytes each) at 252, bssid
 * 352, add_ies (u32) 360, add_ie_len 364, vif_idx 366, chan_cnt 367,
 * ssid_cnt 368, no_cck 369, duration (u32) 372 */
#define AIC_SCANU_START_REQ_LEN         376
#define AIC_SCANU_SSID                  252
#define AIC_SCANU_SSID_LEN              33
#define AIC_SCANU_BSSID                 352
#define AIC_SCANU_VIF_IDX               366
#define AIC_SCANU_CHAN_CNT              367
#define AIC_SCANU_SSID_CNT              368
#define AIC_SCANU_NO_CCK                369
#define AIC_SCANU_DURATION              372
#define AIC_SCAN_CHANNEL_MAX            42
/* SCANU_START_CFM: vif_idx, status, result_cnt */
/* SCANU_RESULT_IND: length (u16), framectrl (u16), center_freq (u16), band,
 * sta_idx, inst_nbr, rssi (s8), then the beacon or probe response from its
 * 802.11 header on (payload at 12) */
#define AIC_SCANU_RESULT_PAYLOAD        12

/* SM_CONNECT_REQ: ssid (33) at 0, bssid 34, chan (6) 40, flags (u32) 48,
 * ctrl_port_ethertype (u16, network order) 52, ie_len 54, listen_interval
 * 56, dont_wait_bcmc 58, auth_type 59, uapsd_queues 60, vif_idx 61,
 * ie_buf[256] 64 */
#define AIC_SM_CONNECT_REQ_LEN          320
#define AIC_SM_CONNECT_BSSID            34
#define AIC_SM_CONNECT_CHAN             40
#define AIC_SM_CONNECT_FLAGS            48
#define AIC_SM_CONNECT_ETHERTYPE        52
#define AIC_SM_CONNECT_IE_LEN           54
#define AIC_SM_CONNECT_LISTEN           56
#define AIC_SM_CONNECT_DONT_WAIT_BCMC   58
#define AIC_SM_CONNECT_AUTH_TYPE        59
#define AIC_SM_CONNECT_UAPSD            60
#define AIC_SM_CONNECT_VIF_IDX          61
#define AIC_SM_CONNECT_IE_BUF           64
#define AIC_SM_CONNECT_IE_MAX           256
#define AIC_CONTROL_PORT_HOST           0x01
#define AIC_CONTROL_PORT_NO_ENC         0x02
#define AIC_DISABLE_HT                  0x04
#define AIC_WPA_WPA2_IN_USE             0x08
#define AIC_MFP_IN_USE                  0x10
#define AIC_REASSOCIATION               0x20
#define AIC_AUTH_OPEN                   0

/* SM_CONNECT_IND: status_code (u16) 0, bssid 2, roamed 8, vif_idx 9,
 * ap_idx 10, ch_idx 11, qos 12, acm 13, assoc_req_ie_len 14,
 * assoc_rsp_ie_len 16, assoc_ie_buf 20, aid 820, band 822,
 * center_freq 824, width 826, center_freq1 828, center_freq2 832,
 * ac_param[4] 836 */
#define AIC_SM_CONNECT_IND_LEN          852
#define AIC_SM_CONNECT_IND_BSSID        2
#define AIC_SM_CONNECT_IND_AP_IDX       10
#define AIC_SM_CONNECT_IND_QOS          12
#define AIC_SM_CONNECT_IND_AID          820
#define AIC_SM_CONNECT_IND_FREQ         824

/* SM_DISCONNECT_REQ: reason_code (u16), vif_idx; IND: reason_code, vif_idx,
 * ft_over_ds, reassoc */
#define AIC_SM_DISCONNECT_REQ_LEN       4

/* ---------------------------------------------------- data descriptors
 *
 * TX: struct hostdesc (CONFIG_RWNX_FULLMAC, no MU-MIMO, no split buffers),
 * 28 bytes after the USB header: packet_len (u16) 0, flags_ext (u16) 2,
 * status_desc_addr (u32) 4, eth_dest_addr 8, eth_src_addr 14, ethertype
 * (network order) 20, ac 22, tid 23, vif_idx 24, staid 25, flags (u16) 26.
 * The payload is the Ethernet payload (the 14-byte header is not sent).
 */
#define AIC_HOSTDESC_LEN                28
#define AIC_HD_PACKET_LEN               0
#define AIC_HD_STATUS_ADDR              4
#define AIC_HD_DEST                     8
#define AIC_HD_SRC                      14
#define AIC_HD_ETHERTYPE                20
#define AIC_HD_AC                       22
#define AIC_HD_TID                      23
#define AIC_HD_VIF                      24
#define AIC_HD_STA                      25
#define AIC_HD_FLAGS                    26
#define AIC_HWQ_BE                      1
#define AIC_TID_NONE                    0xFF

/*
 * RX: struct hw_rxhdr, 56 bytes, then 4 bytes of padding:
 *   +0  u16 len of the 802.11 frame, u8 type, u8 mpdu/ampdu counts
 *   +36 status word: bit 0 rx_vect2_valid, bit 1 resp_frame, bits 2-4
 *       decr_status (0 none, 1 WEP, 2 TKIP, 3 CCMP128, 4 CCMP256,
 *       5 GCMP128, 6 GCMP256, 7 WAPI), ...
 *   +40 phy info (band, channel type, prim20 freq, center freqs)
 *   +48 flags: bit 0 amsdu, 1 is_80211_mpdu, 2 4addr, 3 new_peer,
 *       4 user_prio, 5 need_reord, 6 upload, 7 monitor, 8-15 vif_idx,
 *       16-23 sta_idx, 24-31 dst_idx
 *   +52 pattern
 *   +60 the 802.11 frame (decrypted; the security header is still there)
 */
#define AIC_RX_STATUS                   36
#define AIC_RX_PHY_INFO                 40
#define AIC_RX_FLAGS                    48
#define AIC_RX_FLAG_AMSDU               0x01
#define AIC_RX_FLAG_80211               0x02
#define AIC_RX_FLAG_4ADDR               0x04
#define AIC_RX_FLAG_UPLOAD              0x40
#define AIC_RX_FLAG_MONITOR             0x80
#define AIC_DECR_NONE                   0
#define AIC_DECR_WEP                    1
#define AIC_DECR_TKIP                   2
#define AIC_DECR_CCMP128                3
#define AIC_DECR_CCMP256                4
#define AIC_DECR_GCMP128                5
#define AIC_DECR_GCMP256                6
#define AIC_DECR_WAPI                   7

/* ------------------------------------------------- firmware download
 *
 * AIC8800D80 U02/U03 (aic_compat_8800d80.[ch]).
 */
#define AIC_CHIP_ID_ADDR                0x40500000  /* bits 23:16 revision, bit 25 0 = MCU part */
#define AIC_CHIP_SUBID_ADDR             0x00000004
#define AIC_CHIP_REV_U01                0x01
#define AIC_CHIP_REV_U02                0x03
#define AIC_CHIP_REV_U03                0x07
#define AIC_CHIP_ID_H_MASK              0xC0
#define AIC_D80_FMAC_ADDR               0x120000    /* RAM_FMAC_FW_ADDR_8800D80_U02 */
#define AIC_D80_ADID_ADDR               0x00201940  /* defaults if the table has no PINF */
#define AIC_D80_PATCH_ADDR              0x0020B43C
#define AIC_D80_CONFIG_BASE_PTR         (AIC_D80_FMAC_ADDR + 0x0198)
#define AIC_D80_PATCH_STR_PTR           (AIC_D80_FMAC_ADDR + 0x01A0)
#define AIC_D80_PATCH_BUF_PTR           (AIC_D80_FMAC_ADDR + 0x01A4)
#define AIC_D80_FW_VERSION              (AIC_D80_FMAC_ADDR + 0x001C)
#define AIC_D80_PATCH_START             0x001D7000
#define AIC_PATCH_MAGIC                 0x48435450  /* "PTCH" */
#define AIC_PATCH_MAGIC_2               0x50544348  /* "HCTP" */
/* aic_patch_t: magic_num, pair_start, magic_num_2, pair_count,
 * block_dst[4], block_src[4], block_size[4] */
#define AIC_PATCH_OFS_MAGIC             0
#define AIC_PATCH_OFS_PAIR_START        4
#define AIC_PATCH_OFS_MAGIC_2           8
#define AIC_PATCH_OFS_PAIR_COUNT        12
#define AIC_PATCH_OFS_BLOCK_SIZE        44

/* fw_patch_table_*.bin: "AICBT_PT_TAG" in 16 bytes, then sections of
 * name[16], type (u32), count (u32), count (address, value) pairs. */
#define AIC_PT_TAG                      "AICBT_PT_TAG"
#define AIC_PT_INF                      0   /* addresses of the adid/patch files, ext patches */
#define AIC_PT_TRAP                     1
#define AIC_PT_B4                       2
#define AIC_PT_BTMODE                   3   /* values patched from the BT settings below */
#define AIC_PT_PWRON                    4   /* wait 100 ms after writing it */
#define AIC_PT_AF                       5
#define AIC_PT_VER                      6   /* text, not written */

/* The Bluetooth settings aic_load_fw writes into the AICBT_PT_BTMODE section
 * for an AIC8800D80 (aicbsp_info and aicbt_info defaults): hwinfo < 0,
 * hwinfo = -1, cpmode 0 (work), btmode 5 (BT only, no external switch),
 * btport 1 (USB "MB"), UART baud 1500000, flow control 1, LPM 0, TX power
 * levels 0x6F2F. */
#define AIC_BT_MODE_VALUES { 1, 0xFFFFFFFFu, 0, 5, 1, 1500000, 1, 0, 0x00006F2F }
#define AIC_BT_MODE_COUNT  9

/* patch_tbl_d80[] in aic_compat_8800d80.c, offsets from config_base, with
 * USE_5G off, no power limit, no PRBREQ report, no radar, no WoWLAN:
 *   0x00B4 = 0xF3010000, 0x0170 = 0x0001000A (RX aggregation counter),
 *   0x0188 = 0x00000001 (user_ext_flags: power offset covers calibration) */
#define AIC_D80_PATCH_TBL { { 0x00B4, 0xF3010000 }, { 0x0170, 0x0001000A }, { 0x0188, 0x00000001 } }
#define AIC_D80_PATCH_TBL_COUNT 3
#define AIC_D80_NEW_PATCH_MAP_VER 0x06090100
