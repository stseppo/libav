/*
 * HEVC CABAC decoding
 *
 * Copyright (C) 2012 Guillaume Martres
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "cabac_functions.h"
#include "hevc.h"

/**
 * number of bin by SyntaxElement.
 */
static const int8_t num_bins_in_se[] = {
     1,  // sao_merge_flag
     1,  // sao_type_idx
     0,  // sao_eo_class
     0,  // sao_band_position
     0,  // sao_offset_abs
     0,  // sao_offset_sign
     0,  // end_of_slice_flag
     3,  // split_coding_unit_flag
     1,  // cu_transquant_bypass_flag
     3,  // skip_flag
     3,  // cu_qp_delta
     1,  // pred_mode
     3,  // part_mode
     0,  // pcm_flag
     1,  // prev_intra_luma_pred_mode
     0,  // mpm_idx
     0,  // rem_intra_luma_pred_mode
     2,  // intra_chroma_pred_mode
     1,  // merge_flag
     1,  // merge_idx
     5,  // inter_pred_idc
     2,  // ref_idx_l0
     2,  // ref_idx_l1
     2,  // abs_mvd_greater0_flag
     2,  // abs_mvd_greater1_flag
     0,  // abs_mvd_minus2
     0,  // mvd_sign_flag
     1,  // mvp_lx_flag
     1,  // no_residual_data_flag
     3,  // split_transform_flag
     2,  // cbf_luma
     3,  // cbf_cb, cbf_cr
     2,  // transform_skip_flag[][]
    18,  // last_significant_coeff_x_prefix
    18,  // last_significant_coeff_y_prefix
     0,  // last_significant_coeff_x_suffix
     0,  // last_significant_coeff_y_suffix
     4,  // significant_coeff_group_flag
    42,  // significant_coeff_flag
    24,  // coeff_abs_level_greater1_flag
     6,  // coeff_abs_level_greater2_flag
     0,  // coeff_abs_level_remaining
     0,  // coeff_sign_flag
};

/**
 * Offset to ctxIdx 0 in init_values and states, indexed by SyntaxElement.
 */
static int elem_offset[sizeof(num_bins_in_se)];

#define CNU 154
/**
 * Indexed by init_type
 */
const uint8_t init_values[3][HEVC_CONTEXTS] = {
    {
        // sao_merge_flag
        153,
        // sao_type_idx
        200,
        // split_coding_unit_flag
        139, 141, 157,
        // cu_transquant_bypass_flag
        154,
        // skip_flag
        CNU, CNU, CNU,
        // cu_qp_delta
        154, 154, 154,
        // pred_mode
        CNU,
        // part_mode
        184, CNU, CNU,
        // prev_intra_luma_pred_mode
        184,
        // intra_chroma_pred_mode
        63, 139,
        // merge_flag
        CNU,
        // merge_idx
        CNU,
        // inter_pred_idc
        CNU, CNU, CNU, CNU, CNU,
        // ref_idx_l0
        CNU, CNU,
        // ref_idx_l1
        CNU, CNU,
        // abs_mvd_greater1_flag
        CNU, CNU,
        // abs_mvd_greater1_flag
        CNU, CNU,
        // mvp_lx_flag
        CNU,
        // no_residual_data_flag
        CNU,
        // split_transform_flag
        153, 138, 138,
        // cbf_luma
        111, 141,
        // cbf_cb, cbf_cr
        94, 138, 182,
        // transform_skip_flag
        139, 139,
        // last_significant_coeff_x_prefix
        110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
         79, 108, 123,  63,
        // last_significant_coeff_y_prefix
        110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
         79, 108, 123,  63,
        // significant_coeff_group_flag
        91, 171, 134, 141,
        // significant_coeff_flag
        111, 111, 125, 110, 110,  94, 124, 108, 124, 107, 125, 141, 179, 153,
        125, 107, 125, 141, 179, 153, 125, 107, 125, 141, 179, 153, 125, 140,
        139, 182, 182, 152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111,
        // coeff_abs_level_greater1_flag
        140,  92, 137, 138, 140, 152, 138, 139, 153,  74, 149,  92, 139, 107,
        122, 152, 140, 179, 166, 182, 140, 227, 122, 197,
        // coeff_abs_level_greater2_flag
        138, 153, 136, 167, 152, 152,
    },
    {
        // sao_merge_flag
        153,
        // sao_type_idx
        185,
        // split_coding_unit_flag
        107, 139, 126,
        // cu_transquant_bypass_flag
        154,
        // skip_flag
        197, 185, 201,
        // cu_qp_delta
        154, 154, 154,
        // pred_mode
        149,
        // part_mode
        154, 139, 154,
        // prev_intra_luma_pred_mode
        154,
        // intra_chroma_pred_mode
        152, 139,
        // merge_flag
        110,
        // merge_idx
        122,
        // inter_pred_idc
        95, 79, 63, 31, 31,
        // ref_idx_l0
        153, 153,
        // ref_idx_l1
        153, 153,
        // abs_mvd_greater1_flag
        140, 198,
        // abs_mvd_greater1_flag
        140, 198,
        // mvp_lx_flag
        168,
        // no_residual_data_flag
        79,
        // split_transform_flag
        124, 138, 94,
        // cbf_luma
        153, 111,
        // cbf_cb, cbf_cr
        149, 107, 167,
        // transform_skip_flag
        139, 139,
        // last_significant_coeff_x_prefix
        125, 110,  94, 110,  95,  79, 125, 111, 110,  78, 110, 111, 111,  95,
         94, 108, 123, 108,
        // last_significant_coeff_y_prefix
        125, 110,  94, 110,  95,  79, 125, 111, 110,  78, 110, 111, 111,  95,
         94, 108, 123, 108,
        // significant_coeff_group_flag
        121, 140, 61, 154,
        // significant_coeff_flag
        155, 154, 139, 153, 139, 123, 123,  63, 153, 166, 183, 140, 136, 153,
        154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170,
        153, 123, 123, 107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140,
        // coeff_abs_level_greater1_flag
        154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121,
        136, 137, 169, 194, 166, 167, 154, 167, 137, 182,
        // coeff_abs_level_greater2_flag
        107, 167, 91, 122, 107, 167,
    },
    {
        // sao_merge_flag
        153,
        // sao_type_idx
        160,
        // split_coding_unit_flag
        107, 139, 126,
        // cu_transquant_bypass_flag
        154,
        // skip_flag
        197, 185, 201,
        // cu_qp_delta
        154, 154, 154,
        // pred_mode
        134,
        // part_mode
        154, 139, 154,
        // prev_intra_luma_pred_mode
        183,
        // intra_chroma_pred_mode
        152, 139,
        // merge_flag
        154,
        // merge_idx
        137,
        // inter_pred_idc
        95, 79, 63, 31, 31,
        // ref_idx_l0
        153, 153,
        // ref_idx_l1
        153, 153,
        // abs_mvd_greater1_flag
        169, 198,
        // abs_mvd_greater1_flag
        169, 198,
        // mvp_lx_flag
        168,
        // no_residual_data_flag
        79,
        // split_transform_flag
        224, 167, 122,
        // cbf_luma
        153, 111,
        // cbf_cb, cbf_cr
        149, 92, 167,
        // transform_skip_flag
        139, 139,
        // last_significant_coeff_x_prefix
        125, 110, 124, 110,  95,  94, 125, 111, 111,  79, 125, 126, 111, 111,
         79, 108, 123,  93,
        // last_significant_coeff_y_prefix
        125, 110, 124, 110,  95,  94, 125, 111, 111,  79, 125, 126, 111, 111,
         79, 108, 123,  93,
        // significant_coeff_group_flag
        121, 140, 61, 154,
        // significant_coeff_flag
        170, 154, 139, 153, 139, 123, 123,  63, 124, 166, 183, 140, 136, 153,
        154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170,
        153, 138, 138, 122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140,
        // coeff_abs_level_greater1_flag
        154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121,
        136, 122, 169, 208, 166, 167, 154, 152, 167, 182,
        // coeff_abs_level_greater2_flag
        107, 167, 91, 107, 107, 167,
    },
};

void ff_hevc_cabac_init(HEVCContext *s)
{
    int i;
    int init_type;
    GetBitContext *gb = &s->gb;

    skip_bits(gb, 1);
    align_get_bits(gb);
    ff_init_cabac_states(&s->cc);
    ff_init_cabac_decoder(&s->cc,
                          gb->buffer + get_bits_count(gb) / 8,
                          (get_bits_left(&s->gb) + 7) / 8);

    init_type = 2 - s->sh.slice_type;
    if (s->sh.cabac_init_flag && s->sh.slice_type != I_SLICE)
        init_type ^= 3;

    elem_offset[0] = 0;
    for (i = 1; i < sizeof(num_bins_in_se); i++) {
        elem_offset[i] = elem_offset[i-1] + num_bins_in_se[i-1];
    }

    for (i = 0; i < HEVC_CONTEXTS; i++) {
        int init_value = init_values[init_type][i];
        int m = (init_value >> 4)*5 - 45;
        int n = ((init_value & 15) << 3) - 16;
        int pre = 2 * (((m * av_clip_c(s->sh.slice_qp, 0, 51)) >> 4) + n) - 127;
        pre ^= pre >> 31;
        if (pre > 124)
            pre = 124 + (pre & 1);

        s->cabac_state[i] =  pre;
    }
}

#define GET_CABAC(ctx) get_cabac(&s->cc, &s->cabac_state[ctx])

int ff_hevc_sao_merge_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[SAO_MERGE_FLAG]);
}

int ff_hevc_sao_type_idx_decode(HEVCContext *s)
{
    if (!GET_CABAC(elem_offset[SAO_TYPE_IDX]))
        return 0;

    if (!get_cabac_bypass(&s->cc))
        return SAO_BAND;
    return SAO_EDGE;
}

int ff_hevc_sao_band_position_decode(HEVCContext *s)
{
    int i;
    int value = get_cabac_bypass(&s->cc);

    for (i = 0; i < 4; i++)
        value = (value << 1) | get_cabac_bypass(&s->cc);
    return value;
}

int ff_hevc_sao_offset_abs_decode(HEVCContext *s)
{
    int i = 0;
    int length = (1 << (FFMIN(s->sps->bit_depth, 10) - 5)) - 1;

    while (i < length && get_cabac_bypass(&s->cc))
        i++;
    return i;
}

int ff_hevc_sao_offset_sign_decode(HEVCContext *s)
{
    return get_cabac_bypass(&s->cc);
}

int ff_hevc_sao_eo_class_decode(HEVCContext *s)
{
    return (get_cabac_bypass(&s->cc) << 1) |
        get_cabac_bypass(&s->cc);
}

int ff_hevc_end_of_slice_flag_decode(HEVCContext *s)
{
    return get_cabac_terminate(&s->cc);
}

int ff_hevc_cu_transquant_bypass_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[CU_TRANSQUANT_BYPASS_FLAG]);
}

int ff_hevc_skip_flag_decode(HEVCContext *s, int x_cb, int y_cb)
{
    int inc = 0;

    if (x_cb > 0)
        inc = SAMPLE(s->cu.skip_flag, x_cb-1, y_cb);
    if (y_cb > 0)
        inc += SAMPLE(s->cu.skip_flag, x_cb, y_cb-1);

    return GET_CABAC(elem_offset[SKIP_FLAG] + inc);
}
int ff_hevc_pred_mode_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[PRED_MODE_FLAG]);
}
int ff_hevc_split_coding_unit_flag_decode(HEVCContext *s, int ct_depth, int x0, int y0)
{
    int inc = 0, depth_left = 0, depth_top = 0;

    if (x0 > 0)
        depth_left = s->cu.left_ct_depth[y0 >> s->sps->log2_min_coding_block_size];
    if (y0 > 0)
        depth_top = s->cu.top_ct_depth[x0 >> s->sps->log2_min_coding_block_size];

    av_dlog(s->avctx, "depth cur: %d, left: %d, top: %d\n",
           ct_depth, depth_left, depth_top);

    inc += (depth_left > ct_depth);
    inc += (depth_top > ct_depth);

    return GET_CABAC(elem_offset[SPLIT_CODING_UNIT_FLAG] + inc);
}

int ff_hevc_part_mode_decode(HEVCContext *s, int log2_cb_size)
{
    if (GET_CABAC(elem_offset[PART_MODE])) // 1
        return PART_2Nx2N;
    if (log2_cb_size == s->sps->log2_min_coding_block_size) {
        if (s->cu.pred_mode == MODE_INTRA) // 0
            return PART_NxN;
        if (GET_CABAC(elem_offset[PART_MODE] + 1)) // 01
            return PART_2NxN;
        if (log2_cb_size == 3) // 00
            return PART_Nx2N;
        if (GET_CABAC(elem_offset[PART_MODE] + 2)) // 001
            return PART_Nx2N;
        return PART_NxN; // 000
    }

    if (!s->sps->amp_enabled_flag) {
        if (GET_CABAC(elem_offset[PART_MODE] + 1)) // 01
            return PART_2NxN;
        return PART_Nx2N;
    }

    if (GET_CABAC(elem_offset[PART_MODE] + 1)) { // 01X, 01XX
        if (GET_CABAC(elem_offset[PART_MODE] + 3)) // 011
            return PART_2NxN;
        if (get_cabac_bypass(&s->cc)) // 0101
            return PART_2NxnD;
        return PART_2NxnU; // 0100
    }

    if (GET_CABAC(elem_offset[PART_MODE] + 3)) // 001
        return PART_Nx2N;
    if (get_cabac_bypass(&s->cc)) // 0001
        return PART_nRx2N;
    return  PART_nLx2N; // 0000
}

int ff_hevc_pcm_flag_decode(HEVCContext *s)
{
    return get_cabac_terminate(&s->cc);
}

int ff_hevc_prev_intra_luma_pred_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[PREV_INTRA_LUMA_PRED_FLAG]);
}

int ff_hevc_mpm_idx_decode(HEVCContext *s)
{
    int i = 0;
    while (i < 2 && get_cabac_bypass(&s->cc))
        i++;
    return i;
}

int ff_hevc_rem_intra_luma_pred_mode_decode(HEVCContext *s)
{
    int i;
    int value = get_cabac_bypass(&s->cc);

    for (i = 0; i < 4; i++)
        value = (value << 1) | get_cabac_bypass(&s->cc);
    return value;
}

int ff_hevc_intra_chroma_pred_mode_decode(HEVCContext *s)
{
    if (!GET_CABAC(elem_offset[INTRA_CHROMA_PRED_MODE]))
        return 4;

    return (get_cabac_bypass(&s->cc) << 1) |
        get_cabac_bypass(&s->cc);
}

int ff_hevc_merge_idx_decode(HEVCContext *s)
{
    int i = 1;
    if(!GET_CABAC(elem_offset[MERGE_IDX]))
        return 0;

    while (i < s->sh.max_num_merge_cand-1 && get_cabac_bypass(&s->cc))
        i++;
    return i;
}

int ff_hevc_merge_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[MERGE_FLAG]);
}

int ff_hevc_inter_pred_idc_decode(HEVCContext *s, int nPbW, int nPbH)
{
    if (nPbW + nPbH == 12)
        return GET_CABAC(elem_offset[INTER_PRED_IDC] + 4);
    if (GET_CABAC(elem_offset[INTER_PRED_IDC] + s->ct.depth))
        return PRED_BI;

    return GET_CABAC(elem_offset[INTER_PRED_IDC] + 4);
}

int ff_hevc_ref_idx_lx_decode(HEVCContext *s, int num_ref_idx_lx)
{
    int i = 0;
    int max = num_ref_idx_lx - 1;
    int max_ctx = FFMIN(max, 2);

    while (i < max_ctx && GET_CABAC(elem_offset[REF_IDX_L0] + i))
        i++;
    while (i < max && get_cabac_bypass(&s->cc))
        i++;
    return i;
}

int ff_hevc_mvp_lx_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[MVP_LX_FLAG]);
}

int ff_hevc_no_residual_syntax_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[NO_RESIDUAL_DATA_FLAG]);
}

int ff_hevc_abs_mvd_greater0_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[ABS_MVD_GREATER0_FLAG]);
}

int ff_hevc_abs_mvd_greater1_flag_decode(HEVCContext *s)
{
    return GET_CABAC(elem_offset[ABS_MVD_GREATER0_FLAG] + 1);
}

int ff_hevc_abs_mvd_minus2_decode(HEVCContext *s)
{
    int ret = 0;
    int k = 1;

    while (get_cabac_bypass(&s->cc)) {
        ret += 1 << k;
        k++;
    }
    while (k--)
        ret += get_cabac_bypass(&s->cc) << k;

    return ret;
}

int ff_hevc_mvd_sign_flag_decode(HEVCContext *s)
{
    return get_cabac_bypass(&s->cc);
}

int ff_hevc_split_transform_flag_decode(HEVCContext *s, int log2_trafo_size)
{
    return GET_CABAC(elem_offset[SPLIT_TRANSFORM_FLAG] + 5 - log2_trafo_size);
}

int ff_hevc_cbf_cb_cr_decode(HEVCContext *s, int trafo_depth)
{
    return GET_CABAC(elem_offset[CBF_CB_CR] + trafo_depth);
}

int ff_hevc_cbf_luma_decode(HEVCContext *s, int trafo_depth)
{
    return GET_CABAC(elem_offset[CBF_LUMA] + !trafo_depth);
}

int ff_hevc_transform_skip_flag_decode(HEVCContext *s, int c_idx)
{
    return GET_CABAC(elem_offset[TRANSFORM_SKIP_FLAG] + !!c_idx);
}

#define LAST_SIG_COEFF(elem)                                                    \
    int i = 0;                                                                  \
    int max = (log2_size << 1) - 1;                                             \
    int ctx_offset, ctx_shift;                                                  \
                                                                                \
    if (c_idx == 0) {                                                           \
        ctx_offset = 3 * (log2_size - 2)  + ((log2_size - 1) >> 2);             \
        ctx_shift = (log2_size + 1) >> 2;                                       \
    } else {                                                                    \
        ctx_offset = 15;                                                        \
        ctx_shift = log2_size - 2;                                              \
    }                                                                           \
    while (i < max &&                                                           \
           GET_CABAC(elem_offset[elem] + (i >> ctx_shift) + ctx_offset))        \
        i++;                                                                    \
    return i;

int ff_hevc_last_significant_coeff_x_prefix_decode(HEVCContext *s, int c_idx,
                                                   int log2_size)
{
    LAST_SIG_COEFF(LAST_SIGNIFICANT_COEFF_X_PREFIX)
}

int ff_hevc_last_significant_coeff_y_prefix_decode(HEVCContext *s, int c_idx,
                                                   int log2_size)
{
    LAST_SIG_COEFF(LAST_SIGNIFICANT_COEFF_Y_PREFIX)
}

int ff_hevc_last_significant_coeff_suffix_decode(HEVCContext *s,
                                                 int last_significant_coeff_prefix)
{
    int i;
    int length = (last_significant_coeff_prefix >> 1) - 1;
    int value = get_cabac_bypass(&s->cc);

    for (i = 1; i < length; i++)
        value = (value << 1) | get_cabac_bypass(&s->cc);
    return value;
}

int ff_hevc_significant_coeff_group_flag_decode(HEVCContext *s, int c_idx, int x_cg,
                                                int y_cg, int log2_trafo_size)
{
    int ctx_cg = 0;
    int inc;

    if (x_cg < (1 << (log2_trafo_size - 2)) - 1)
        ctx_cg += s->rc.significant_coeff_group_flag[x_cg + 1][y_cg];
    if (y_cg < (1 << (log2_trafo_size - 2)) - 1)
        ctx_cg += s->rc.significant_coeff_group_flag[x_cg][y_cg + 1];

    inc = FFMIN(ctx_cg, 1) + (c_idx>0 ? 2 : 0);

    return GET_CABAC(elem_offset[SIGNIFICANT_COEFF_GROUP_FLAG] + inc);
}

int ff_hevc_significant_coeff_flag_decode(HEVCContext *s, int c_idx, int x_c, int y_c,
                                          int log2_trafo_size, int scan_idx)
{
    static const uint8_t ctx_idx_map[] = {
        0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8
    };
    int x_cg = x_c >> 2;
    int y_cg = y_c >> 2;
    int sig_ctx;
    int inc;

    if (x_c + y_c == 0) {
        sig_ctx = 0;
    } else if (log2_trafo_size == 2) {
        sig_ctx = ctx_idx_map[(y_c << 2) + x_c];
    } else {
        int prev_sig = 0;
        int x_off = x_c - (x_cg << 2);
        int y_off = y_c - (y_cg << 2);

        if (x_cg < ((1 << log2_trafo_size) - 1) >> 2)
            prev_sig += s->rc.significant_coeff_group_flag[x_cg + 1][y_cg];
        if (y_cg < ((1 << log2_trafo_size) - 1) >> 2)
            prev_sig += (s->rc.significant_coeff_group_flag[x_cg][y_cg + 1] << 1);
        av_dlog(s->avctx, "prev_sig: %d\n", prev_sig);

        switch (prev_sig) {
        case 0:
            sig_ctx = ((x_off + y_off) == 0) ? 2 : ((x_off + y_off) <= 2) ? 1 : 0;
            break;
        case 1:
            sig_ctx = 2 - FFMIN(y_off, 2);
            break;
        case 2:
            sig_ctx = 2 - FFMIN(x_off, 2);
            break;
        default:
            sig_ctx = 2;
        }

        if (c_idx == 0 && (x_cg > 0 || y_cg > 0))
            sig_ctx += 3;

        if (log2_trafo_size == 3) {
            sig_ctx += (scan_idx == SCAN_DIAG) ? 9 : 15;
        } else {
            sig_ctx += c_idx ? 12 : 21;
        }
    }

    if (c_idx == 0) {
        inc = sig_ctx;
    } else {
        inc = sig_ctx + 27;
    }

    return GET_CABAC(elem_offset[SIGNIFICANT_COEFF_FLAG] + inc);
}

static int ctx_set = 0;

int ff_hevc_coeff_abs_level_greater1_flag_decode(HEVCContext *s, int c_idx,
                                                 int i, int n,
                                                 int first_elem,
                                                 int first_subset)
{
    static int greater1_ctx = 0;
    static int last_coeff_abs_level_greater1_flag = 0;

    int inc;

    if (first_elem) {
        ctx_set = (i > 0 && c_idx == 0) ? 2 : 0;

        if (!first_subset && greater1_ctx == 0)
            ctx_set++;
        greater1_ctx = 1;
    }

    inc = (ctx_set * 4) + greater1_ctx;
    if (c_idx > 0)
        inc += 16;

    last_coeff_abs_level_greater1_flag =
        GET_CABAC(elem_offset[COEFF_ABS_LEVEL_GREATER1_FLAG] + inc);

    if (last_coeff_abs_level_greater1_flag) {
        greater1_ctx = 0;
    } else if (greater1_ctx > 0 && greater1_ctx < 3) {
        greater1_ctx++;
    }

    return last_coeff_abs_level_greater1_flag;
}

int ff_hevc_coeff_abs_level_greater2_flag_decode(HEVCContext *s, int c_idx,
                                                 int i, int n)
{
    int inc;

    inc = ctx_set;
    if (c_idx > 0)
        inc += 4;

    return GET_CABAC(elem_offset[COEFF_ABS_LEVEL_GREATER2_FLAG] + inc);
}

int ff_hevc_coeff_abs_level_remaining(HEVCContext *s, int first_elem, int base_level)
{
    int i;
    static int c_rice_param, last_coeff_abs_level_remaining;
    int prefix = 0;
    int suffix = 0;

    if (first_elem) {
        c_rice_param = 0;
        last_coeff_abs_level_remaining = 0;
        av_dlog(s->avctx,
               "c_rice_param reset to 0\n");
    }

    while (get_cabac_bypass(&s->cc))
        prefix++;

    if (prefix < 3) {
        for (i = 0; i < c_rice_param; i++)
            suffix = (suffix << 1) | get_cabac_bypass(&s->cc);
        last_coeff_abs_level_remaining = (prefix << c_rice_param) + suffix;
    } else {
        for (i = 0; i < prefix - 3 + c_rice_param; i++)
            suffix = (suffix << 1) | get_cabac_bypass(&s->cc);
        last_coeff_abs_level_remaining = (((1 << (prefix - 3)) + 3 - 1)
                                          << c_rice_param) + suffix;
    }

    av_dlog(s->avctx,
           "coeff_abs_level_remaining c_rice_param: %d\n", c_rice_param);
    av_dlog(s->avctx,
           "coeff_abs_level_remaining base_level: %d, prefix: %d, suffix: %d\n",
           base_level, prefix, suffix);
    av_dlog(s->avctx,
           "coeff_abs_level_remaining: %d\n",
           last_coeff_abs_level_remaining);

    av_dlog(s->avctx, "last_coeff_(%d) > %d\n", base_level + last_coeff_abs_level_remaining, 3*(1<<(c_rice_param)));

    c_rice_param = FFMIN(c_rice_param +
                         ((base_level + last_coeff_abs_level_remaining) >
                          (3 * (1 << c_rice_param))), 4);
    av_dlog(s->avctx,
           "new c_rice_param: %d\n", c_rice_param);

    return last_coeff_abs_level_remaining;
}

int ff_hevc_coeff_sign_flag(HEVCContext *s, uint8_t nb)
{
    int i;
    int ret = 0;

    for (i = 0; i < nb; i++)
        ret = (ret << 1) | get_cabac_bypass(&s->cc);
    return ret;
}
