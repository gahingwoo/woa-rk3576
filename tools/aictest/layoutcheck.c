/*
 * layoutcheck.c - compile-time check of drivers/net/aic8800usb/aic_proto.h
 * against AICSemi's own headers (lmac_msg.h, ipc_shared.h from
 * aic8800_fdrv). CI fetches the vendor tree at the pinned commit and
 * builds this; nothing of it is stored in this repository.
 *
 * Not covered here (the vendor header pulls in most of the kernel):
 * struct hw_rxhdr from rwnx_rx.h, 56 bytes, flags word at 48 and status
 * word at 36, checked by hand on 2026-11-25 with the same stubs.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "lmac_msg.h"
#include "ipc_shared.h"
#include "../../drivers/net/aic8800usb/aic_proto.h"
#include <stdio.h>

#define ID(x)        _Static_assert(AIC_##x == x, #x)
#define SZ(s, v)     _Static_assert(sizeof(struct s) == (v), "sizeof " #s)
#define OF(s, f, v)  _Static_assert(offsetof(struct s, f) == (v), #s "." #f)

ID(MM_RESET_REQ); ID(MM_RESET_CFM); ID(MM_START_REQ); ID(MM_START_CFM); ID(MM_VERSION_REQ);
ID(MM_VERSION_CFM); ID(MM_ADD_IF_REQ); ID(MM_ADD_IF_CFM); ID(MM_KEY_ADD_REQ); ID(MM_KEY_ADD_CFM);
ID(MM_KEY_DEL_REQ); ID(MM_KEY_DEL_CFM); ID(MM_CHANNEL_SWITCH_IND); ID(MM_CHANNEL_PRE_SWITCH_IND);
ID(MM_PS_CHANGE_IND); ID(MM_TRAFFIC_REQ_IND); ID(MM_P2P_VIF_PS_CHANGE_IND); ID(MM_SET_RF_CALIB_REQ);
ID(MM_SET_RF_CALIB_CFM); ID(MM_GET_MAC_ADDR_REQ); ID(MM_GET_MAC_ADDR_CFM); ID(MM_SET_STACK_START_REQ);
ID(MM_SET_STACK_START_CFM); ID(MM_GET_FW_VERSION_REQ); ID(MM_GET_FW_VERSION_CFM); ID(MM_FW_PANIC_IND);
ID(MM_FW_ASSERT_IND);
ID(DBG_MEM_READ_REQ); ID(DBG_MEM_READ_CFM); ID(DBG_MEM_WRITE_REQ); ID(DBG_MEM_WRITE_CFM);
ID(DBG_MEM_BLOCK_WRITE_REQ); ID(DBG_MEM_BLOCK_WRITE_CFM); ID(DBG_START_APP_REQ); ID(DBG_START_APP_CFM);
ID(DBG_MEM_MASK_WRITE_REQ); ID(DBG_MEM_MASK_WRITE_CFM);
ID(SCANU_START_REQ); ID(SCANU_START_CFM); ID(SCANU_RESULT_IND); ID(SCANU_START_CFM_ADDTIONAL);
ID(ME_CONFIG_REQ); ID(ME_CONFIG_CFM); ID(ME_CHAN_CONFIG_REQ); ID(ME_CHAN_CONFIG_CFM);
ID(ME_SET_CONTROL_PORT_REQ); ID(ME_SET_CONTROL_PORT_CFM); ID(ME_TX_CREDITS_UPDATE_IND);
ID(SM_CONNECT_REQ); ID(SM_CONNECT_CFM); ID(SM_CONNECT_IND); ID(SM_DISCONNECT_REQ); ID(SM_DISCONNECT_CFM);
ID(SM_DISCONNECT_IND);
_Static_assert(AIC_TASK_MM == TASK_MM && AIC_TASK_DBG == TASK_DBG && AIC_TASK_SCANU == TASK_SCANU &&
               AIC_TASK_ME == TASK_ME && AIC_TASK_SM == TASK_SM && AIC_DRV_TASK_ID == DRV_TASK_ID, "tasks");
_Static_assert(AIC_MM_STA == MM_STA, "MM_STA");
_Static_assert(AIC_MAC_CIPHER_CCMP == MAC_CIPHER_CCMP && AIC_MAC_CIPHER_TKIP == MAC_CIPHER_TKIP, "ciphers");
_Static_assert(AIC_CONTROL_PORT_HOST == CONTROL_PORT_HOST && AIC_CONTROL_PORT_NO_ENC == CONTROL_PORT_NO_ENC &&
               AIC_DISABLE_HT == DISABLE_HT && AIC_WPA_WPA2_IN_USE == WPA_WPA2_IN_USE &&
               AIC_MFP_IN_USE == MFP_IN_USE && AIC_REASSOCIATION == REASSOCIATION, "connect flags");
_Static_assert(AIC_SCAN_CHANNEL_MAX == SCAN_CHANNEL_MAX && AIC_MAX_CHAN_24G == MAC_DOMAINCHANNEL_24G_MAX &&
               AIC_MAX_CHAN_5G == MAC_DOMAINCHANNEL_5G_MAX, "channel counts");

SZ(mm_start_req, AIC_MM_START_REQ_LEN); OF(mm_start_req, uapsd_timeout, AIC_MM_START_UAPSD_TIMEOUT);
OF(mm_start_req, lp_clk_accuracy, AIC_MM_START_LP_CLK);
SZ(mm_add_if_req, AIC_MM_ADD_IF_REQ_LEN); OF(mm_add_if_req, addr, AIC_MM_ADD_IF_ADDR);
OF(mm_add_if_req, p2p, AIC_MM_ADD_IF_P2P);
SZ(mm_version_cfm, AIC_MM_VERSION_CFM_LEN); OF(mm_version_cfm, features, 20); OF(mm_version_cfm, max_sta_nb, 24);
OF(mm_version_cfm, max_vif_nb, 26);
SZ(mm_key_add_req, AIC_MM_KEY_ADD_REQ_LEN); OF(mm_key_add_req, key_idx, AIC_KEY_ADD_KEY_IDX);
OF(mm_key_add_req, sta_idx, AIC_KEY_ADD_STA_IDX); OF(mm_key_add_req, key, AIC_KEY_ADD_KEY_LEN);
OF(mm_key_add_req, key.array, AIC_KEY_ADD_KEY); OF(mm_key_add_req, cipher_suite, AIC_KEY_ADD_CIPHER);
OF(mm_key_add_req, inst_nbr, AIC_KEY_ADD_INST); OF(mm_key_add_req, spp, AIC_KEY_ADD_SPP);
OF(mm_key_add_req, pairwise, AIC_KEY_ADD_PAIRWISE);
SZ(mm_set_stack_start_req, AIC_MM_SET_STACK_START_REQ_LEN);
SZ(mm_get_fw_version_cfm, AIC_MM_GET_FW_VERSION_CFM_LEN);
SZ(mm_set_rf_calib_req, AIC_MM_SET_RF_CALIB_REQ_LEN); SZ(mm_set_rf_calib_cfm, AIC_MM_SET_RF_CALIB_CFM_LEN);
OF(mm_set_rf_calib_req, bt_calib_param, 16); OF(mm_set_rf_calib_req, xtal_cap, 20);
SZ(me_config_req, AIC_ME_CONFIG_REQ_LEN); OF(me_config_req, ht_cap.a_mpdu_param, AIC_ME_CONFIG_HT_AMPDU_PARAM);
OF(me_config_req, ht_cap.mcs_rate, AIC_ME_CONFIG_HT_MCS); OF(me_config_req, tx_lft, AIC_ME_CONFIG_TX_LFT);
OF(me_config_req, phy_bw_max, AIC_ME_CONFIG_PHY_BW_MAX); OF(me_config_req, ht_supp, AIC_ME_CONFIG_HT_SUPP);
OF(me_config_req, vht_supp, AIC_ME_CONFIG_VHT_SUPP); OF(me_config_req, he_supp, AIC_ME_CONFIG_HE_SUPP);
OF(me_config_req, he_ul_on, AIC_ME_CONFIG_HE_UL_ON); OF(me_config_req, ps_on, AIC_ME_CONFIG_PS_ON);
OF(me_config_req, ant_div_on, AIC_ME_CONFIG_ANT_DIV); OF(me_config_req, dpsm, AIC_ME_CONFIG_DPSM);
SZ(mac_chan_def, AIC_CHAN_DEF_LEN); OF(mac_chan_def, band, 2); OF(mac_chan_def, flags, 3); OF(mac_chan_def, tx_power, 4);
SZ(me_chan_config_req, AIC_ME_CHAN_CONFIG_REQ_LEN); OF(me_chan_config_req, chan5G, AIC_ME_CHAN_CONFIG_5G);
OF(me_chan_config_req, chan2G4_cnt, AIC_ME_CHAN_CONFIG_CNT24); OF(me_chan_config_req, chan5G_cnt, AIC_ME_CHAN_CONFIG_CNT5);
SZ(me_set_control_port_req, AIC_ME_SET_CONTROL_PORT_REQ_LEN);
SZ(scanu_start_req, AIC_SCANU_START_REQ_LEN); OF(scanu_start_req, ssid, AIC_SCANU_SSID);
SZ(mac_ssid, AIC_SCANU_SSID_LEN); OF(scanu_start_req, bssid, AIC_SCANU_BSSID);
OF(scanu_start_req, vif_idx, AIC_SCANU_VIF_IDX); OF(scanu_start_req, chan_cnt, AIC_SCANU_CHAN_CNT);
OF(scanu_start_req, ssid_cnt, AIC_SCANU_SSID_CNT); OF(scanu_start_req, no_cck, AIC_SCANU_NO_CCK);
OF(scanu_start_req, duration, AIC_SCANU_DURATION);
OF(scanu_result_ind, center_freq, 4); OF(scanu_result_ind, band, 6); OF(scanu_result_ind, rssi, 9);
OF(scanu_result_ind, payload, AIC_SCANU_RESULT_PAYLOAD);
OF(scanu_start_cfm, status, 1); OF(scanu_start_cfm, result_cnt, 2);
SZ(sm_connect_req, AIC_SM_CONNECT_REQ_LEN); OF(sm_connect_req, bssid, AIC_SM_CONNECT_BSSID);
OF(sm_connect_req, chan, AIC_SM_CONNECT_CHAN); OF(sm_connect_req, flags, AIC_SM_CONNECT_FLAGS);
OF(sm_connect_req, ctrl_port_ethertype, AIC_SM_CONNECT_ETHERTYPE); OF(sm_connect_req, ie_len, AIC_SM_CONNECT_IE_LEN);
OF(sm_connect_req, listen_interval, AIC_SM_CONNECT_LISTEN);
OF(sm_connect_req, dont_wait_bcmc, AIC_SM_CONNECT_DONT_WAIT_BCMC);
OF(sm_connect_req, auth_type, AIC_SM_CONNECT_AUTH_TYPE); OF(sm_connect_req, uapsd_queues, AIC_SM_CONNECT_UAPSD);
OF(sm_connect_req, vif_idx, AIC_SM_CONNECT_VIF_IDX); OF(sm_connect_req, ie_buf, AIC_SM_CONNECT_IE_BUF);
SZ(sm_connect_ind, AIC_SM_CONNECT_IND_LEN); OF(sm_connect_ind, bssid, AIC_SM_CONNECT_IND_BSSID);
OF(sm_connect_ind, ap_idx, AIC_SM_CONNECT_IND_AP_IDX); OF(sm_connect_ind, qos, AIC_SM_CONNECT_IND_QOS);
OF(sm_connect_ind, assoc_req_ie_len, AIC_SM_CONNECT_IND_REQ_IE_LEN);
OF(sm_connect_ind, assoc_rsp_ie_len, AIC_SM_CONNECT_IND_RSP_IE_LEN);
OF(sm_connect_ind, assoc_ie_buf, AIC_SM_CONNECT_IND_IE_BUF);
OF(sm_connect_ind, aid, AIC_SM_CONNECT_IND_AID); OF(sm_connect_ind, center_freq, AIC_SM_CONNECT_IND_FREQ);
SZ(sm_disconnect_req, AIC_SM_DISCONNECT_REQ_LEN); OF(sm_disconnect_ind, vif_idx, 2);
SZ(hostdesc, AIC_HOSTDESC_LEN); OF(hostdesc, packet_len, AIC_HD_PACKET_LEN);
OF(hostdesc, status_desc_addr, AIC_HD_STATUS_ADDR); OF(hostdesc, eth_dest_addr, AIC_HD_DEST);
OF(hostdesc, eth_src_addr, AIC_HD_SRC); OF(hostdesc, ethertype, AIC_HD_ETHERTYPE); OF(hostdesc, ac, AIC_HD_AC);
OF(hostdesc, tid, AIC_HD_TID); OF(hostdesc, vif_idx, AIC_HD_VIF); OF(hostdesc, staid, AIC_HD_STA);
OF(hostdesc, flags, AIC_HD_FLAGS);
/* dbg_mem_block_write_req: the loader (aic_load_fw/aicbluetooth_cmds.h)
 * sends 1024-byte blocks; aic8800_fdrv declares 512 for its own use. */

int
main(void)
{
    printf("aic_proto.h matches the vendor headers\n");
    return 0;
}
