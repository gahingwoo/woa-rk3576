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
    return v383_dxva_bit_order_check();
}
#else
typedef int v383_dxva_check_not_on_this_platform;
#endif
