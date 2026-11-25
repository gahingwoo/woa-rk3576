/*
 * dxva_check.c - compile-time check that lib/v383_dxva.h has the layout of
 * the Windows SDK's dxva.h. Windows only; nothing in here runs.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifdef _WIN32
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <dxva.h>

#include "../../drivers/video/vdpu383/lib/v383_dxva.h"
#include "../../drivers/video/vdpu383/lib/v383_dxva_hevc.h"
#include "../../drivers/video/vdpu383/lib/v383_dxva_vp9.h"

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define SAME_SIZE(a, b) typedef char CAT(size_check_, __LINE__)[(sizeof(a) == sizeof(b)) ? 1 : -1]
#define SAME_OFF(f) typedef char CAT(off_check_, __LINE__)[ \
    (offsetof(DXVA_PicParams_H264, f) == offsetof(V383_DXVA_PicParams_H264, f)) ? 1 : -1]

SAME_SIZE(DXVA_PicParams_H264, V383_DXVA_PicParams_H264);
SAME_SIZE(DXVA_Qmatrix_H264, V383_DXVA_Qmatrix_H264);
SAME_SIZE(DXVA_PicEntry_H264, V383_DXVA_PicEntry_H264);
SAME_OFF(CurrPic);
SAME_OFF(num_ref_frames);
SAME_OFF(wBitFields);
SAME_OFF(bit_depth_luma_minus8);
SAME_OFF(StatusReportFeedbackNumber);
SAME_OFF(RefFrameList);
SAME_OFF(CurrFieldOrderCnt);
SAME_OFF(FieldOrderCntList);
SAME_OFF(pic_init_qs_minus26);
SAME_OFF(num_ref_idx_l1_active_minus1);
SAME_OFF(FrameNumList);
SAME_OFF(UsedForReferenceFlags);
SAME_OFF(NonExistingFrameFlags);
SAME_OFF(frame_num);
SAME_OFF(log2_max_frame_num_minus4);
SAME_OFF(redundant_pic_cnt_present_flag);
SAME_OFF(slice_group_change_rate_minus1);
SAME_OFF(SliceGroupMap);

#define SAME_OFF_HEVC(f) typedef char CAT(hoff_check_, __LINE__)[ \
    (offsetof(DXVA_PicParams_HEVC, f) == offsetof(V383_DXVA_PicParams_HEVC, f)) ? 1 : -1]

SAME_SIZE(DXVA_PicParams_HEVC, V383_DXVA_PicParams_HEVC);
SAME_SIZE(DXVA_Qmatrix_HEVC, V383_DXVA_Qmatrix_HEVC);
SAME_SIZE(DXVA_Slice_HEVC_Short, V383_DXVA_Slice_HEVC_Short);
typedef char hevc_pp_size_232[(sizeof(V383_DXVA_PicParams_HEVC) == 232) ? 1 : -1];
SAME_OFF_HEVC(wFormatAndSequenceInfoFlags);
SAME_OFF_HEVC(CurrPic);
SAME_OFF_HEVC(init_qp_minus26);
SAME_OFF_HEVC(wNumBitsForShortTermRPSInSlice);
SAME_OFF_HEVC(dwCodingParamToolFlags);
SAME_OFF_HEVC(dwCodingSettingPicturePropertyFlags);
SAME_OFF_HEVC(column_width_minus1);
SAME_OFF_HEVC(row_height_minus1);
SAME_OFF_HEVC(log2_parallel_merge_level_minus2);
SAME_OFF_HEVC(CurrPicOrderCntVal);
SAME_OFF_HEVC(RefPicList);
SAME_OFF_HEVC(PicOrderCntValList);
SAME_OFF_HEVC(RefPicSetStCurrBefore);
SAME_OFF_HEVC(RefPicSetLtCurr);
SAME_OFF_HEVC(StatusReportFeedbackNumber);

/* the bit order of the HEVC flag words */
static int v383_dxva_hevc_bit_order_check(void)
{
    DXVA_PicParams_HEVC pp;
    memset(&pp, 0, sizeof(pp));
    pp.log2_max_pic_order_cnt_lsb_minus4 = 5;
    pp.cabac_init_present_flag = 1;
    pp.num_extra_slice_header_bits = 3;
    pp.IntraPicFlag = 1;
    pp.uniform_spacing_flag = 1;
    return V383_HEVC_LOG2_MAX_POC_LSB_MINUS4((V383_DXVA_PicParams_HEVC *)&pp) == 5 &&
           V383_HEVC_CABAC_INIT_PRESENT((V383_DXVA_PicParams_HEVC *)&pp) &&
           V383_HEVC_NUM_EXTRA_SH_BITS((V383_DXVA_PicParams_HEVC *)&pp) == 3 &&
           V383_HEVC_INTRA((V383_DXVA_PicParams_HEVC *)&pp) &&
           V383_HEVC_UNIFORM_SPACING((V383_DXVA_PicParams_HEVC *)&pp);
}

#define SAME_OFF_VP9(f) typedef char CAT(voff_check_, __LINE__)[ \
    (offsetof(DXVA_PicParams_VP9, f) == offsetof(V383_DXVA_PicParams_VP9, f)) ? 1 : -1]

SAME_SIZE(DXVA_PicParams_VP9, V383_DXVA_PicParams_VP9);
SAME_SIZE(DXVA_segmentation_VP9, V383_DXVA_segmentation_VP9);
SAME_SIZE(DXVA_Slice_VPx_Short, V383_DXVA_Slice_VPx_Short);
SAME_SIZE(DXVA_PicEntry_VPx, V383_DXVA_PicEntry_VPx);
SAME_OFF_VP9(profile);
SAME_OFF_VP9(wFormatAndPictureInfoFlags);
SAME_OFF_VP9(width);
SAME_OFF_VP9(BitDepthMinus8Luma);
SAME_OFF_VP9(interp_filter);
SAME_OFF_VP9(ref_frame_map);
SAME_OFF_VP9(ref_frame_coded_width);
SAME_OFF_VP9(ref_frame_coded_height);
SAME_OFF_VP9(frame_refs);
SAME_OFF_VP9(ref_frame_sign_bias);
SAME_OFF_VP9(filter_level);
SAME_OFF_VP9(wControlInfoFlags);
SAME_OFF_VP9(ref_deltas);
SAME_OFF_VP9(mode_deltas);
SAME_OFF_VP9(base_qindex);
SAME_OFF_VP9(uv_ac_delta_q);
SAME_OFF_VP9(stVP9Segments);
SAME_OFF_VP9(log2_tile_cols);
SAME_OFF_VP9(uncompressed_header_size_byte_aligned);
SAME_OFF_VP9(first_partition_size);
SAME_OFF_VP9(StatusReportFeedbackNumber);

/* the bit order of the VP9 flag bytes and words */
static int v383_dxva_vp9_bit_order_check(void)
{
    DXVA_PicParams_VP9 pp;
    memset(&pp, 0, sizeof(pp));
    pp.frame_parallel_decoding_mode = 1;
    pp.frame_context_idx = 2;
    pp.reset_frame_context = 3;
    pp.allow_high_precision_mv = 1;
    pp.use_prev_in_find_mv_refs = 1;
    pp.stVP9Segments.abs_delta = 1;
    return V383_VP9_PARALLEL_MODE((V383_DXVA_PicParams_VP9 *)&pp) &&
           V383_VP9_FRAME_CTX_IDX((V383_DXVA_PicParams_VP9 *)&pp) == 2 &&
           V383_VP9_RESET_CTX((V383_DXVA_PicParams_VP9 *)&pp) == 3 &&
           V383_VP9_ALLOW_HP_MV((V383_DXVA_PicParams_VP9 *)&pp) &&
           !V383_VP9_REFRESH_CTX((V383_DXVA_PicParams_VP9 *)&pp) &&
           V383_VP9_USE_PREV_MVS((V383_DXVA_PicParams_VP9 *)&pp) &&
           V383_VP9_SEG_ABS_DELTA((V383_DXVA_PicParams_VP9 *)&pp);
}

/* the bit order of wBitFields */
static int v383_dxva_bit_order_check(void)
{
    DXVA_PicParams_H264 pp;
    memset(&pp, 0, sizeof(pp));
    pp.transform_8x8_mode_flag = 1;
    pp.chroma_format_idc = 2;
    return pp.wBitFields == ((1u << V383_PP_TRANSFORM_8X8) | (2u << V383_PP_CHROMA_FORMAT_IDC));
}

int v383_dxva_check(void)
{
    return v383_dxva_bit_order_check() && v383_dxva_hevc_bit_order_check() && v383_dxva_vp9_bit_order_check();
}
#else
typedef int v383_dxva_check_not_on_this_platform;
#endif
