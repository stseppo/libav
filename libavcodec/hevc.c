/*
 * HEVC video Decoder
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
#include "libavutil/pixdesc.h"
#include "golomb.h"
#include "hevcdata.h"
#include "hevc.h"

/**
 * NOTE: Each function hls_foo correspond to the function foo in the
 * specification (HLS stands for High Level Syntax).
 */

/**
 * Section 5.7
 */
#define INVERSE_RASTER_SCAN(a, b, c, d, e) ((e) ? ((a) / (ROUNDED_DIV(d, b)))*(c) : ((a)%(ROUNDED_DIV(d, b)))*(b))

static int pic_arrays_init(HEVCContext *s)
{
    int i;
    int pic_size = s->sps->pic_width_in_luma_samples * s->sps->pic_height_in_luma_samples;
    int ctb_count = s->sps->pic_width_in_ctbs * s->sps->pic_height_in_ctbs;
    int pic_width_in_min_pu  = s->sps->pic_width_in_min_cbs * 4;
    int pic_height_in_min_pu = s->sps->pic_height_in_min_cbs * 4;

    s->sao = av_mallocz(ctb_count * sizeof(*s->sao));

    s->split_coding_unit_flag = av_malloc(pic_size);
    s->cu.skip_flag = av_malloc(pic_size);

    s->cu.left_ct_depth = av_malloc(s->sps->pic_height_in_min_cbs);
    s->cu.top_ct_depth = av_malloc(s->sps->pic_width_in_min_cbs);

    s->pu.left_ipm = av_malloc(pic_height_in_min_pu);
    s->pu.top_ipm = av_malloc(pic_width_in_min_pu);

    if (!s->sao || !s->split_coding_unit_flag || !s->cu.skip_flag ||
        !s->pu.left_ipm || !s->pu.top_ipm)
        return -1;

    memset(s->pu.left_ipm, INTRA_DC, pic_height_in_min_pu);
    memset(s->pu.top_ipm, INTRA_DC, pic_width_in_min_pu);

    for (i = 0; i < MAX_TRANSFORM_DEPTH; i++) {
        s->tt.split_transform_flag[i] = av_malloc(pic_size);
        s->tt.cbf_cb[i] = av_malloc(pic_size);
        s->tt.cbf_cr[i] = av_malloc(pic_size);
        if (!s->tt.split_transform_flag[i] || !s->tt.cbf_cb[i] ||
            !s->tt.cbf_cr[i])
            return -1;
    }

    return 0;
}

static void pic_arrays_free(HEVCContext *s)
{
    int i;
    av_freep(&s->sao);

    av_freep(&s->split_coding_unit_flag);
    av_freep(&s->cu.skip_flag);

    av_freep(&s->pu.left_ipm);
    av_freep(&s->pu.top_ipm);

    for (i = 0; i < MAX_TRANSFORM_DEPTH; i++) {
        av_freep(&s->tt.split_transform_flag[i]);
        av_freep(&s->tt.cbf_cb[i]);
        av_freep(&s->tt.cbf_cr[i]);
    }
}

static int hls_slice_header(HEVCContext *s)
{
    int i;
    GetBitContext *gb = &s->gb;
    SliceHeader *sh = &s->sh;
    int slice_address_length = 0;

    av_log(s->avctx, AV_LOG_INFO, "Decoding slice\n");


    // Coded parameters

    sh->first_slice_in_pic_flag = get_bits1(gb);
    if (s->nal_unit_type >= 16 && s->nal_unit_type <= 23)
        sh->no_output_of_prior_pics_flag = get_bits1(gb);

    sh->pps_id = get_ue_golomb(gb);
    if (sh->pps_id >= MAX_PPS_COUNT || !s->pps_list[sh->pps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", sh->pps_id);
        return -1;
    }
    s->pps = s->pps_list[sh->pps_id];
    if (s->sps != s->sps_list[s->pps->sps_id]) {
        s->sps = s->sps_list[s->pps->sps_id];
        s->vps = s->vps_list[s->sps->vps_id];
        //TODO: Handle switching between different SPS better
        pic_arrays_free(s);
        if (pic_arrays_init(s) < 0) {
            pic_arrays_free(s);
            return -1;
        }

        s->avctx->width = s->sps->pic_width_in_luma_samples;
        s->avctx->height = s->sps->pic_height_in_luma_samples;
        if (s->sps->chroma_format_idc == 0 || s->sps->separate_colour_plane_flag) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "TODO: s->sps->chroma_format_idc == 0 || "
                   "s->sps->separate_colour_plane_flag\n");
            return -1;
        }

        if (s->sps->bit_depth[0] != s->sps->bit_depth[1]) {
            av_log_missing_feature(s->avctx,
                                   "different bit_depth for luma and chroma is", 0);
            return AVERROR_PATCHWELCOME;
        }

        if (s->sps->chroma_format_idc == 1) {
            switch (s->sps->bit_depth[0]) {
            case 8:
                s->avctx->pix_fmt = PIX_FMT_YUV420P;
                break;
            case 9:
                s->avctx->pix_fmt = PIX_FMT_YUV420P9;
                break;
            case 10:
                s->avctx->pix_fmt = PIX_FMT_YUV420P10;
                break;
            case 16:
                s->avctx->pix_fmt = PIX_FMT_YUV420P16;
                break;
            default:
                av_log(s->avctx, AV_LOG_ERROR, "unsupported bit depth: %d\n", s->sps->bit_depth[0]);
                return AVERROR_PATCHWELCOME;
            }
        } else {
            av_log(s->avctx, AV_LOG_ERROR, "non-4:2:0 support is currently unspecified.\n");
            return -1;
        }
        s->sps->hshift[0] = s->sps->vshift[0] = 0;
        s->sps->hshift[2] =
        s->sps->hshift[1] = av_pix_fmt_descriptors[s->avctx->pix_fmt].log2_chroma_w;
        s->sps->vshift[2] =
        s->sps->vshift[1] = av_pix_fmt_descriptors[s->avctx->pix_fmt].log2_chroma_h;

        s->sps->pixel_shift[0] = s->sps->bit_depth[0] > 8;
        s->sps->pixel_shift[2] =
        s->sps->pixel_shift[1] = s->sps->bit_depth[1] > 8;

        ff_hevc_pred_init(s->hpc[0], s->sps->bit_depth[0]);
        ff_hevc_pred_init(s->hpc[1], s->sps->bit_depth[1]);
        ff_hevc_dsp_init(s->hevcdsp[0], s->sps->bit_depth[0]);
        ff_hevc_dsp_init(s->hevcdsp[1], s->sps->bit_depth[1]);
    }

    if (!sh->first_slice_in_pic_flag) {
        if (s->pps->dependent_slice_segments_enabled_flag)
            sh->dependent_slice_segment_flag = get_bits1(gb);

        slice_address_length = av_ceil_log2_c(s->sps->pic_width_in_ctbs *
                                              s->sps->pic_height_in_ctbs);
        sh->slice_address = get_bits(gb, slice_address_length);
    }

    if (!s->pps->dependent_slice_segments_enabled_flag) {
        for(i = 0; i < s->pps->num_extra_slice_header_bits; i++)
            skip_bits(gb, 1); // slice_reserved_undetermined_flag[]
        sh->slice_type = get_ue_golomb(gb);
        if (s->pps->output_flag_present_flag)
            sh->pic_output_flag = get_bits1(gb);

        if (s->sps->separate_colour_plane_flag == 1)
            sh->colour_plane_id = get_bits(gb, 2);

        if (s->nal_unit_type != NAL_IDR_W_DLP) {
            int short_term_ref_pic_set_sps_flag;
            sh->pic_order_cnt_lsb = get_bits(gb, s->sps->log2_max_poc_lsb);
            short_term_ref_pic_set_sps_flag = get_bits1(gb);
            if (!short_term_ref_pic_set_sps_flag) {
                av_log(s->avctx, AV_LOG_ERROR, "TODO: !short_term_ref_pic_set_sps_flag\n");
                return -1;
            } else {
                int numbits = 0;
                int short_term_ref_pic_set_idx;
                while ((1 << numbits) < s->sps->num_short_term_ref_pic_sets)
                    numbits++;
                if (numbits > 0)
                    short_term_ref_pic_set_idx = get_bits(gb, numbits);
                else
                    short_term_ref_pic_set_idx = 0;
            }
            if (s->sps->long_term_ref_pics_present_flag) {
                av_log(s->avctx, AV_LOG_ERROR, "TODO: long_term_ref_pics_present_flag\n");
                return -1;
            }
        }
        if (!s->pps) {
            av_log(s->avctx, AV_LOG_ERROR, "No PPS active while decoding slice\n");
            return -1;
        }

        if (s->sps->sample_adaptive_offset_enabled_flag) {
            sh->slice_sample_adaptive_offset_flag[0] = get_bits1(gb);
            sh->slice_sample_adaptive_offset_flag[2] =
            sh->slice_sample_adaptive_offset_flag[1] = get_bits1(gb);
        }

        sh->num_ref_idx_l0_active = s->pps->num_ref_idx_l0_default_active;
        sh->num_ref_idx_l1_active = s->pps->num_ref_idx_l1_default_active;
        if (s->sps->sps_temporal_mvp_enabled_flag && s->nal_unit_type != NAL_IDR_W_DLP) {
            sh->slice_temporal_mvp_enable_flag = get_bits1(gb);
        }
        if (sh->slice_type == P_SLICE || sh->slice_type == B_SLICE) {
            sh->num_ref_idx_active_override_flag = get_bits1(gb);

            if (sh->num_ref_idx_active_override_flag) {
                sh->num_ref_idx_l0_active = get_ue_golomb(gb) + 1;
                if (sh->slice_type == B_SLICE)
                    sh->num_ref_idx_l1_active = get_ue_golomb(gb) + 1;
            }
            if (s->pps->lists_modification_present_flag) {
                av_log(s->avctx, AV_LOG_ERROR, "TODO: ref_pic_list_modification() \n");
                return -1;
            }

            if (sh->slice_type == B_SLICE)
                sh->mvd_l1_zero_flag = get_bits1(gb);

            if (s->pps->cabac_init_present_flag) {
                sh->cabac_init_flag = get_bits1(gb);
            }
            if (sh->slice_temporal_mvp_enable_flag) {
                if (sh->slice_type == B_SLICE) {
                    sh->collocated_from_l0_flag = get_bits1(gb);
                }
                if ((sh->collocated_from_l0_flag &&
                     sh->num_ref_idx_l0_active > 1) ||
                    (!sh->collocated_from_l0_flag &&
                     sh->num_ref_idx_l0_active > 1)) {
                    sh->collocated_ref_idx = get_ue_golomb(gb);
                }
            }


            sh->max_num_merge_cand = 5 - get_ue_golomb(gb);
        }

        sh->slice_qp_delta = get_se_golomb(gb);
        if (s->pps->pic_slice_level_chroma_qp_offsets_present_flag) {
            sh->slice_cb_qp_offset = get_se_golomb(gb);
            sh->slice_cr_qp_offset = get_se_golomb(gb);
        }
        if (s->pps->deblocking_filter_control_present_flag) {
            int deblocking_filter_override_flag = 0;
            if (s->pps->deblocking_filter_override_enabled_flag)
                deblocking_filter_override_flag = get_bits1(gb);
            if (deblocking_filter_override_flag) {
                sh->disable_deblocking_filter_flag = get_bits1(gb);
                if (!sh->disable_deblocking_filter_flag) {
                    sh->beta_offset = get_se_golomb(gb) * 2;
                    sh->tc_offset = get_se_golomb(gb) * 2;
                }
            } else {
                sh->disable_deblocking_filter_flag = s->pps->pps_disable_deblocking_filter_flag;
            }
        }

        if (s->pps->seq_loop_filter_across_slices_enabled_flag
            && (sh->slice_sample_adaptive_offset_flag[0] ||
                sh->slice_sample_adaptive_offset_flag[1] ||
                !sh->disable_deblocking_filter_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = get_bits1(gb);
        } else {
            sh->slice_loop_filter_across_slices_enabled_flag =
            s->pps->seq_loop_filter_across_slices_enabled_flag;
        }
    }

    if (s->pps->slice_header_extension_present_flag) {
        int length = get_ue_golomb(gb);
        for (i = 0; i < length; i++)
            skip_bits(gb, 8); // slice_header_extension_data_byte
    }

    // Inferred parameters
    sh->slice_qp = 26 + s->pps->pic_init_qp_minus26 + sh->slice_qp_delta;
    sh->slice_ctb_addr_rs = sh->slice_address;
    sh->slice_cb_addr_zs = sh->slice_address <<
                           (s->sps->log2_diff_max_min_coding_block_size << 1);

    return 0;
}

#define CTB(tab, x, y) ((tab)[(y) * s->sps->pic_width_in_ctbs + (x)])

#define set_sao(elem, value)                            \
    if (!sao_merge_up_flag && !sao_merge_left_flag) {   \
        sao->elem = value;                              \
    } else if (sao_merge_left_flag) {                   \
        sao->elem = CTB(s->sao, rx-1, ry).elem;         \
    } else if (sao_merge_up_flag) {                     \
        sao->elem = CTB(s->sao, rx, ry-1).elem;         \
    } else {                                            \
        sao->elem = 0;                                  \
    }

static int hls_sao_param(HEVCContext *s, int rx, int ry)
{
    int c_idx, i;
    int sao_merge_left_flag = 0;
    int sao_merge_up_flag = 0;

    SAOParams *sao = &CTB(s->sao, rx, ry);

    if (rx > 0) {
        int left_ctb_in_slice = s->ctb_addr_in_slice > 0;
        int left_ctb_in_tile = s->pps->tile_id[s->ctb_addr_ts] ==
                               s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[s->ctb_addr_rs - 1]];
        if (left_ctb_in_slice && left_ctb_in_tile)
            sao_merge_left_flag = ff_hevc_sao_merge_flag_decode(s);
    }
    if (ry > 0 && !sao_merge_left_flag) {
        int up_ctb_in_slice = (s->ctb_addr_ts - s->pps->ctb_addr_rs_to_ts[s->ctb_addr_rs - s->sps->pic_width_in_ctbs]) <=
                              s->ctb_addr_in_slice;
        int up_ctb_in_tile = (s->pps->tile_id[s->ctb_addr_ts] ==
                              s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[s->ctb_addr_rs - s->sps->pic_width_in_ctbs]]);
        if (up_ctb_in_slice && up_ctb_in_tile)
            sao_merge_up_flag = ff_hevc_sao_merge_flag_decode(s);
    }
    for (c_idx = 0; c_idx < 3; c_idx++) {
        int bit_depth = s->sps->bit_depth[c_idx];
        int shift = bit_depth - FFMIN(bit_depth, 10);

        if (!s->sh.slice_sample_adaptive_offset_flag[c_idx])
            continue;

        if (c_idx == 2) {
            sao->type_idx[2] = sao->type_idx[1];
            sao->eo_class[2] = sao->eo_class[1];
        } else {
            set_sao(type_idx[c_idx], ff_hevc_sao_type_idx_decode(s));
        }
        av_log(s->avctx, AV_LOG_DEBUG, "sao_type_idx: %d\n",
               sao->type_idx[c_idx]);

        if (sao->type_idx[c_idx] == SAO_NOT_APPLIED)
            continue;

        for (i = 0; i < 4; i++)
            set_sao(offset_abs[c_idx][i], ff_hevc_sao_offset_abs_decode(s, bit_depth));

        if (sao->type_idx[c_idx] == SAO_BAND) {
            for (i = 0; i < 4; i++) {
                if (sao->offset_abs[c_idx][i]) {
                    set_sao(offset_sign[c_idx][i], ff_hevc_sao_offset_sign_decode(s));
                } else {
                    sao->offset_sign[c_idx][i] = 0;
                }
            }
            set_sao(band_position[c_idx], ff_hevc_sao_band_position_decode(s));
        } else if (c_idx != 2) {
            set_sao(eo_class[c_idx], ff_hevc_sao_eo_class_decode(s));
        }

        // Inferred parameters
        for (i = 0; i < 4; i++) {
            sao->offset_val[c_idx][i+1] = sao->offset_abs[c_idx][i] << shift;
            if (sao->type_idx[c_idx] == SAO_EDGE) {
                if (i > 1)
                    sao->offset_val[c_idx][i+1] = -sao->offset_val[c_idx][i+1];
            } else if (sao->offset_sign[c_idx][i]) {
                sao->offset_val[c_idx][i+1] = -sao->offset_val[c_idx][i+1];
            }
        }
    }
    return 0;
}

#undef set_sao

static const uint8_t tctable[] =
{
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, // QP  0...18
     1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, // QP 19...37
     5, 5, 6, 6, 7, 8, 9,10,11,13,14,16,18,20,22,24           // QP 38...53
};

static const uint8_t betatable[] =
{
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 8, // QP 0...18
     9,10,11,12,13,14,15,16,17,18,20,22,24,26,28,30,32,34,36, // QP 19...37
    38,40,42,44,46,48,50,52,54,56,58,60,62,64                 // QP 38...51
};

// line zero
#define P3 pix[-4*xstride]
#define P2 pix[-3*xstride]
#define P1 pix[-2*xstride]
#define P0 pix[-xstride]
#define Q0 pix[0]
#define Q1 pix[xstride]
#define Q2 pix[2*xstride]
#define Q3 pix[3*xstride]

// line three. used only for deblocking decision
#define TP3 pix[-4*xstride+3*ystride]
#define TP2 pix[-3*xstride+3*ystride]
#define TP1 pix[-2*xstride+3*ystride]
#define TP0 pix[-xstride+3*ystride]
#define TQ0 pix[3*ystride]
#define TQ1 pix[xstride+3*ystride]
#define TQ2 pix[2*xstride+3*ystride]
#define TQ3 pix[3*xstride+3*ystride]

#define av_clip_pixel(a) av_clip_uint8(a)

static void hevc_loop_filter_luma(uint8_t *pix, int xstride, int ystride, int bs, int qp, int bit_depth)
{
    int d;
    const int inner_iters = 4;
    const int dp0 = abs(P2 - 2 * P1 +  P0);
    const int dq0 = abs(Q2 - 2 * Q1 +  Q0);
    const int dp3 = abs(TP2 - 2 * TP1 + TP0);
    const int dq3 = abs(TQ2 - 2 * TQ1 + TQ0);
    const int d0 = dp0 + dq0;
    const int d3 = dp3 + dq3;

    const int idxb = av_clip_c(qp, 0, MAX_QP);
    const int beta = betatable[idxb] * (1 << (bit_depth - 8));
    const int idxt = av_clip_c(qp + DEFAULT_INTRA_TC_OFFSET * (bs - 1), 0, MAX_QP + DEFAULT_INTRA_TC_OFFSET);
    const int tc = tctable[idxt] * (1 << (bit_depth - 8));

    if (d0 + d3 < beta) {
        const int beta_3 = beta >> 3;
        const int beta_2 = beta >> 2;
        const int tc25 = ((tc * 5 + 1) >> 1);

        if(abs( P3 -  P0) + abs( Q3 -  Q0) < beta_3 && abs( P0 -  Q0) < tc25 &&
           abs(TP3 - TP0) + abs(TQ3 - TQ0) < beta_3 && abs(TP0 - TQ0) < tc25 &&
                                 (d0 << 1) < beta_2 &&      (d3 << 1) < beta_2) {
            // strong filtering
            const int tc2 = tc << 1;
            for(d = 0; d < inner_iters; d++) {
                const int p3 = P3;
                const int p2 = P2;
                const int p1 = P1;
                const int p0 = P0;
                const int q0 = Q0;
                const int q1 = Q1;
                const int q2 = Q2;
                const int q3 = Q3;
                P0 = av_clip_c(( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3, p0-tc2, p0+tc2);
                P1 = av_clip_c(( p2 + p1 + p0 + q0 + 2 ) >> 2, p1-tc2, p1+tc2);
                P2 = av_clip_c(( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3, p2-tc2, p2+tc2);
                Q0 = av_clip_c(( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3, q0-tc2, q0+tc2);
                Q1 = av_clip_c(( p0 + q0 + q1 + q2 + 2 ) >> 2, q1-tc2, q1+tc2);
                Q2 = av_clip_c(( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3, q2-tc2, q2+tc2);
                pix += ystride;
            }
        } else { // normal filtering
            int nd_p = 1;
            int nd_q = 1;
            const int tc_2 = tc >> 1;
            if (dp0 + dp3 < ((beta+(beta>>1))>>3))
                nd_p = 2;
            if (dq0 + dq3 < ((beta+(beta>>1))>>3))
                nd_q = 2;

            for(d = 0; d < inner_iters; d++) {
                const int p2 = P2;
                const int p1 = P1;
                const int p0 = P0;
                const int q0 = Q0;
                const int q1 = Q1;
                const int q2 = Q2;
                int delta0 = (9*(q0 - p0) - 3*(q1 - p1) + 8) >> 4;
                if (abs(delta0) < 10 * tc) {
                    delta0 = av_clip_c(delta0, -tc, tc);
                    P0 = av_clip_pixel(p0 + delta0);
                    Q0 = av_clip_pixel(q0 - delta0);
                    if(nd_p > 1) {
                        const int deltap1 = av_clip_c((((p2 + p0 + 1) >> 1) - p1 + delta0) >> 1, -tc_2, tc_2);
                        P1 = av_clip_pixel(p1 + deltap1);
                    }
                    if(nd_q > 1) {
                        const int deltaq1 = av_clip_c((((q2 + q0 + 1) >> 1) - q1 - delta0) >> 1, -tc_2, tc_2);
                        Q1 = av_clip_pixel(q1 + deltaq1);
                    }
                }
                pix += ystride;
            }
        }
    }
}
#undef P3
#undef P2
#undef P1
#undef P0
#undef Q0
#undef Q1
#undef Q2
#undef Q3

#undef TP3
#undef TP2
#undef TP1
#undef TP0
#undef TQ0
#undef TQ1
#undef TQ2
#undef TQ3

#undef av_clip_pixel

static void deblocking_filter(HEVCContext *s)
{
    int bs = 2; // intra, TODO other modes
    int c_idx = 0;
    int stride = s->frame.linesize[c_idx];
    int ctb_size = (1 << (s->sps->log2_ctb_size)) >> s->sps->hshift[c_idx];
    int qp_y_pred = s->sh.slice_qp;
    int qp_y = ((qp_y_pred + s->tu.cu_qp_delta + 52 + 2 * s->sps->qp_bd_offset_luma) %
        (52 + s->sps->qp_bd_offset_luma)) - s->sps->qp_bd_offset_luma;
    int qp = qp_y + s->sps->qp_bd_offset_luma; // TODO adaptive QP
    int i;

    for (int y_ctb = 0; y_ctb < s->sps->pic_height_in_ctbs; y_ctb++) {
        for (int x_ctb = 1; x_ctb < s->sps->pic_width_in_ctbs; x_ctb++) {
            int x = x_ctb * ctb_size;
            int y = y_ctb * ctb_size;
            uint8_t *src = &s->frame.data[c_idx][y * stride + x];
            int height = FFMIN(ctb_size,
                       (s->sps->pic_height_in_luma_samples >> s->sps->vshift[c_idx]) - y);
            for(i=0; i < height; i+=4)
                hevc_loop_filter_luma(src+i*stride, sizeof(uint8_t), stride, bs, qp, s->sps->bit_depth[c_idx]);
         }
    }

    for (int y_ctb = 1; y_ctb < s->sps->pic_height_in_ctbs; y_ctb++) {
        for (int x_ctb = 0; x_ctb < s->sps->pic_width_in_ctbs; x_ctb++) {
            int x = x_ctb * ctb_size;
            int y = y_ctb * ctb_size;
            uint8_t *src = &s->frame.data[c_idx][y * stride + x];
            int width = FFMIN(ctb_size,
                      (s->sps->pic_width_in_luma_samples >> s->sps->hshift[c_idx]) - x);
            for(i=0; i < width; i+=4)
                hevc_loop_filter_luma(src+i, stride, sizeof(uint8_t), bs, qp, s->sps->bit_depth[c_idx]);
         }
    }
}

static void sao_filter(HEVCContext *s)
{
    //TODO: This should be easily parallelizable
    //TODO: skip CBs when (cu_transquant_bypass_flag || (pcm_loop_filter_disable_flag && pcm_flag))
    for (int c_idx = 0; c_idx < 3; c_idx++) {
        int stride = s->frame.linesize[c_idx];
        int ctb_size = (1 << (s->sps->log2_ctb_size)) >> s->sps->hshift[c_idx];
        for (int y_ctb = 0; y_ctb < s->sps->pic_height_in_ctbs; y_ctb++) {
            for (int x_ctb = 0; x_ctb < s->sps->pic_width_in_ctbs; x_ctb++) {
                struct SAOParams *sao = &CTB(s->sao, x_ctb, y_ctb);
                int x = x_ctb * ctb_size;
                int y = y_ctb * ctb_size;
                int width = FFMIN(ctb_size,
                                  (s->sps->pic_width_in_luma_samples >> s->sps->hshift[c_idx]) - x);
                int height = FFMIN(ctb_size,
                                   (s->sps->pic_height_in_luma_samples >> s->sps->vshift[c_idx]) - y);
                uint8_t *src = &s->frame.data[c_idx][y * stride + x];
                uint8_t *dst = &s->sao_frame.data[c_idx][y * stride + x];
                switch (sao->type_idx[c_idx]) {
                case SAO_BAND:
                    s->hevcdsp[c_idx]
                        ->sao_band_filter(dst, src, stride, sao->offset_val[c_idx],
                                          sao->band_position[c_idx], width, height,
                                          s->sps->bit_depth[c_idx]);
                    break;
                case SAO_EDGE: {
                    int top    = y_ctb == 0;
                    int bottom = y_ctb == (s->sps->pic_height_in_ctbs - 1);
                    int left   = x_ctb == 0;
                    int right  = x_ctb == (s->sps->pic_width_in_ctbs - 1);
                    s->hevcdsp[c_idx]
                        ->sao_edge_filter(dst, src, stride, sao->offset_val[c_idx],
                                          sao->eo_class[c_idx],
                                          top, bottom, left, right,
                                          width, height, s->sps->bit_depth[c_idx]);
                    break;
                }
                }
            }
        }
    }
}

#undef CTB


static av_always_inline int min_cb_addr_zs(HEVCContext *s, int x, int y)
{
    return s->pps->min_cb_addr_zs[y * s->sps->pic_width_in_min_cbs + x];
}

static void hls_residual_coding(HEVCContext *s, int x0, int y0, int log2_trafo_size, enum ScanType scan_idx, int c_idx)
{
#define GET_COORD(offset, n)                                    \
    do {                                                        \
        x_c = (scan_x_cg[offset >> 4] << 2) + scan_x_off[n];    \
        y_c = (scan_y_cg[offset >> 4] << 2) + scan_y_off[n];    \
    } while (0)

    int i;

    HEVCDSPContext *hevcdsp = s->hevcdsp[c_idx];

    int transform_skip_flag = 0;

    int last_significant_coeff_x, last_significant_coeff_y;
    int num_coeff = 0;
    int num_last_subset;
    int x_cg_last_sig, y_cg_last_sig;

    const uint8_t *scan_x_cg, *scan_y_cg, *scan_x_off, *scan_y_off;

    ptrdiff_t stride = s->frame.linesize[c_idx];
    int hshift = s->sps->hshift[c_idx];
    int vshift = s->sps->vshift[c_idx];
    int bit_depth = s->sps->bit_depth[c_idx];
    int pixel_shift = s->sps->pixel_shift[c_idx];
    uint8_t *dst = &s->frame.data[c_idx][(y0 >> vshift) * stride + ((x0 >> hshift) << pixel_shift)];

    int16_t coeffs[MAX_TB_SIZE * MAX_TB_SIZE] = { 0 };
    int trafo_size = 1 << log2_trafo_size;

    av_log(s->avctx, AV_LOG_DEBUG, "scan_idx: %d, c_idx: %d\n",
           scan_idx, c_idx);
    memset(s->rc.significant_coeff_group_flag, 0, 8*8);

    if (log2_trafo_size == 1) {
        log2_trafo_size = 2;
    }


    if (s->pps->transform_skip_enabled_flag && !s->cu.cu_transquant_bypass_flag &&
        log2_trafo_size == 2) {
        transform_skip_flag = ff_hevc_transform_skip_flag_decode(s, c_idx);
    }

    last_significant_coeff_x =
    ff_hevc_last_significant_coeff_prefix_decode(s, c_idx, log2_trafo_size, 1);
    last_significant_coeff_y =
    ff_hevc_last_significant_coeff_prefix_decode(s, c_idx, log2_trafo_size, 0);


    if (last_significant_coeff_x > 3) {
        int suffix = ff_hevc_last_significant_coeff_suffix_decode(s, last_significant_coeff_x, 1);
        last_significant_coeff_x = (1 << ((last_significant_coeff_x >> 1) - 1)) *
                                   (2 + (last_significant_coeff_x & 1)) +
                                   suffix;
    }
    if (last_significant_coeff_y > 3) {
        int suffix = ff_hevc_last_significant_coeff_suffix_decode(s, last_significant_coeff_y, 0);
        last_significant_coeff_y = (1 << ((last_significant_coeff_y >> 1) - 1)) *
                                   (2 + (last_significant_coeff_y & 1)) +
                                   suffix;
    }

    if (scan_idx == SCAN_VERT)
        FFSWAP(int, last_significant_coeff_x, last_significant_coeff_y);

    av_log(s->avctx, AV_LOG_DEBUG, "last_significant_coeff_x: %d\n",
           last_significant_coeff_x);
    av_log(s->avctx, AV_LOG_DEBUG, "last_significant_coeff_y: %d\n",
           last_significant_coeff_y);

    x_cg_last_sig = last_significant_coeff_x >> 2;
    y_cg_last_sig = last_significant_coeff_y >> 2;

    switch (scan_idx) {
    case SCAN_DIAG: {
        int last_x_c = last_significant_coeff_x % 4;
        int last_y_c = last_significant_coeff_y % 4;

        scan_x_off = diag_scan4x4_x;
        scan_y_off = diag_scan4x4_y;
        num_coeff = diag_scan4x4_inv[last_y_c][last_x_c];
        if (trafo_size == 4) {
            scan_x_cg = scan_1x1;
            scan_y_cg = scan_1x1;
        } else if (trafo_size == 8) {
            num_coeff += diag_scan2x2_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan2x2_x;
            scan_y_cg = diag_scan2x2_y;
        } else if (trafo_size == 16) {
            num_coeff += diag_scan4x4_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan4x4_x;
            scan_y_cg = diag_scan4x4_y;
        } else { // trafo_size == 32
            num_coeff += diag_scan8x8_inv[y_cg_last_sig][x_cg_last_sig] << 4;
            scan_x_cg = diag_scan8x8_x;
            scan_y_cg = diag_scan8x8_y;
        }
        break;
    }
    case SCAN_HORIZ:
        scan_x_cg = horiz_scan2x2_x;
        scan_y_cg = horiz_scan2x2_y;
        scan_x_off = horiz_scan4x4_x;
        scan_y_off = horiz_scan4x4_y;
        num_coeff = horiz_scan8x8_inv[last_significant_coeff_y][last_significant_coeff_x];
        break;
    default: //SCAN_VERT
        scan_x_cg = horiz_scan2x2_y;
        scan_y_cg = horiz_scan2x2_x;
        scan_x_off = horiz_scan4x4_y;
        scan_y_off = horiz_scan4x4_x;
        num_coeff = horiz_scan8x8_inv[last_significant_coeff_x][last_significant_coeff_y];
        break;
    }
    num_coeff++;
    av_log(s->avctx, AV_LOG_DEBUG, "num_coeff: %d\n",
           num_coeff);

    num_last_subset = (num_coeff - 1) >> 4;

    for (i = num_last_subset; i >= 0; i--) {
        int n;
        int first_nz_pos_in_cg, last_nz_pos_in_cg, num_sig_coeff, first_greater1_coeff_idx;
        int sign_hidden;
        int sum_abs;
        int x_cg, y_cg, x_c, y_c;
        int implicit_non_zero_coeff = 0;
        int trans_coeff_level;

        int offset = i << 4;

        uint8_t significant_coeff_flag[16] = {0};
        uint8_t coeff_abs_level_greater1_flag[16] = {0};
        uint8_t coeff_abs_level_greater2_flag[16] = {0};
        uint8_t coeff_sign_flag[16] = {0};

        int first_elem;

        x_cg = scan_x_cg[i];
        y_cg = scan_y_cg[i];

        if ((i < num_last_subset) && (i > 0)) {
            s->rc.significant_coeff_group_flag[x_cg][y_cg] =
            ff_hevc_significant_coeff_group_flag_decode(s, c_idx, x_cg, y_cg,
                                                        log2_trafo_size,
                                                        scan_idx);
            implicit_non_zero_coeff = 1;
        } else {
            s->rc.significant_coeff_group_flag[x_cg][y_cg] =
            ((x_cg == x_cg_last_sig && y_cg == y_cg_last_sig) ||
             (x_cg == 0 && y_cg == 0));
        }
        av_log(s->avctx, AV_LOG_DEBUG, "significant_coeff_group_flag[%d][%d]: %d\n",
               x_cg, y_cg, s->rc.significant_coeff_group_flag[x_cg][y_cg]);

        for (n = 15; n >= 0; n--) {
            GET_COORD(offset, n);

            if ((n + offset) < (num_coeff - 1) &&
                s->rc.significant_coeff_group_flag[x_cg][y_cg] &&
                (n > 0 || implicit_non_zero_coeff == 0)) {
                significant_coeff_flag[n] =
                ff_hevc_significant_coeff_flag_decode(s, c_idx, x_c, y_c, log2_trafo_size, scan_idx);
                if (significant_coeff_flag[n] == 1)
                    implicit_non_zero_coeff = 0;
            } else {
                int last_cg = (x_c == (x_cg << 2) && y_c == (y_cg << 2));
                significant_coeff_flag[n] =
                ((n + offset) == (num_coeff - 1) ||
                 (last_cg && implicit_non_zero_coeff &&
                  s->rc.significant_coeff_group_flag[x_cg][y_cg])); // not in spec
            }
            av_log(s->avctx, AV_LOG_DEBUG, "significant_coeff_flag(%d, %d): %d\n",
                   x_c, y_c, significant_coeff_flag[n]);

        }

        first_nz_pos_in_cg = 16;
        last_nz_pos_in_cg = -1;
        num_sig_coeff = 0;
        first_greater1_coeff_idx = -1;
        for (n = 15; n >= 0; n--) {
            if (significant_coeff_flag[n]) {
                if (num_sig_coeff < 8) {
                    coeff_abs_level_greater1_flag[n] =
                    ff_hevc_coeff_abs_level_greater1_flag_decode(s, c_idx, i, n,
                                                                 (num_sig_coeff == 0),
                                                                 (i == num_last_subset));
                    num_sig_coeff++;
                    if (coeff_abs_level_greater1_flag[n] &&
                        first_greater1_coeff_idx == -1)
                        first_greater1_coeff_idx = n;
                }
                if (last_nz_pos_in_cg == -1)
                    last_nz_pos_in_cg = n;
                first_nz_pos_in_cg = n;
                av_log(s->avctx, AV_LOG_DEBUG, "coeff_abs_level_greater1_flag[%d]: %d\n",
                       n, coeff_abs_level_greater1_flag[n]);
            }
        }

        sign_hidden = (last_nz_pos_in_cg - first_nz_pos_in_cg >= 4 &&
                       !s->cu.cu_transquant_bypass_flag);
        if (first_greater1_coeff_idx != -1) {
            coeff_abs_level_greater2_flag[first_greater1_coeff_idx] =
            ff_hevc_coeff_abs_level_greater2_flag_decode(s, c_idx, i, first_greater1_coeff_idx);
            av_log(s->avctx, AV_LOG_DEBUG, "coeff_abs_level_greater2_flag[%d]: %d\n",
                   first_greater1_coeff_idx,
                   coeff_abs_level_greater2_flag[first_greater1_coeff_idx]);
        }

        for (n = 15; n >= 0; n--) {
            if (significant_coeff_flag[n] &&
                (!s->pps->sign_data_hiding_flag || !sign_hidden ||
                 n != first_nz_pos_in_cg)) {
                coeff_sign_flag[n] = ff_hevc_coeff_sign_flag(s);
                av_log(s->avctx, AV_LOG_DEBUG, "coeff_sign_flag[%d]: %d\n",
                       n, coeff_sign_flag[n]);
            }
        }

        num_sig_coeff = 0;
        sum_abs = 0;
        first_elem = 1;
        for (n = 15; n >= 0; n--) {

            if (significant_coeff_flag[n]) {
                GET_COORD(offset, n);
                trans_coeff_level = 1 + coeff_abs_level_greater1_flag[n] +
                                    coeff_abs_level_greater2_flag[n];
                if (trans_coeff_level == ((num_sig_coeff < 8) ?
                                          ((n == first_greater1_coeff_idx) ? 3 : 2) : 1)) {
                    trans_coeff_level += ff_hevc_coeff_abs_level_remaining(s, first_elem, trans_coeff_level);
                    first_elem = 0;
                }
                if (s->pps->sign_data_hiding_flag && sign_hidden) {
                    sum_abs += trans_coeff_level;
                    if (n == first_nz_pos_in_cg && (sum_abs%2 == 1))
                        trans_coeff_level = -trans_coeff_level;
                }
                if (coeff_sign_flag[n])
                    trans_coeff_level = -trans_coeff_level;
                num_sig_coeff++;
                av_log(s->avctx, AV_LOG_DEBUG, "trans_coeff_level: %d\n",
                       trans_coeff_level);
                coeffs[y_c * trafo_size + x_c] = trans_coeff_level;

            }
        }
    }


    if (s->cu.cu_transquant_bypass_flag) {
        hevcdsp->transquant_bypass(dst, coeffs, stride, log2_trafo_size, bit_depth);
    } else {
        int qp;
        //TODO: handle non-constant QP
        int qp_y_pred = s->sh.slice_qp;
        int qp_y = ((qp_y_pred + s->tu.cu_qp_delta + 52 + 2 * s->sps->qp_bd_offset_luma) %
                    (52 + s->sps->qp_bd_offset_luma)) - s->sps->qp_bd_offset_luma;
        static int qp_c[] = { 29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37 };
        if (c_idx == 0) {
            qp = qp_y + s->sps->qp_bd_offset_luma;
        } else {
            int qp_i, offset;

            if (c_idx == 1) {
                offset = s->pps->cb_qp_offset + s->sh.slice_cb_qp_offset;
            } else {
                offset = s->pps->cr_qp_offset + s->sh.slice_cr_qp_offset;
            }
            qp_i = av_clip_c(qp_y + offset, - s->sps->qp_bd_offset_luma, 57);
            if (qp_i < 30) {
                qp = qp_i;
            } else if (qp_i > 43) {
                qp = qp_i - 6;
            } else {
                qp = qp_c[qp_i - 30];
            }

            qp += s->sps->qp_bd_offset_chroma;

        }

        hevcdsp->dequant(coeffs, log2_trafo_size, qp, bit_depth);
        if (transform_skip_flag) {
            hevcdsp->transform_skip(dst, coeffs, stride, log2_trafo_size, bit_depth);
        } else if (s->cu.pred_mode == MODE_INTRA && c_idx == 0 && log2_trafo_size == 2) {
            hevcdsp->transform_4x4_luma_add(dst, coeffs, stride, bit_depth);
        } else {
            hevcdsp->transform_add[log2_trafo_size-2](dst, coeffs, stride, bit_depth);
        }
    }
}

static void hls_transform_unit(HEVCContext *s, int x0, int  y0, int xBase, int yBase,
                               int log2_trafo_size, int trafo_depth, int blk_idx) {
    int scan_idx = SCAN_DIAG;
    int scan_idx_c = SCAN_DIAG;
    if (s->cu.pred_mode == MODE_INTRA) {
        s->hpc[0]->intra_pred(s, x0, y0, log2_trafo_size, 0);
        if (log2_trafo_size > 2) {
            s->hpc[1]->intra_pred(s, x0, y0, log2_trafo_size - 1, 1);
            s->hpc[2]->intra_pred(s, x0, y0, log2_trafo_size - 1, 2);
        } else if (blk_idx == 3) {
            s->hpc[1]->intra_pred(s, xBase, yBase, log2_trafo_size, 1);
            s->hpc[2]->intra_pred(s, xBase, yBase, log2_trafo_size, 2);
        }
    }

    if (s->tt.cbf_luma ||
        SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0) ||
        SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0)) {
        if (s->pps->cu_qp_delta_enabled_flag && !s->tu.is_cu_qp_delta_coded) {
            av_log(s->avctx, AV_LOG_ERROR, "TODO: cu_qp_delta_enabled_flag\n");
            s->tu.is_cu_qp_delta_coded = 1;
        }

        if (s->cu.pred_mode == MODE_INTRA && log2_trafo_size < 4) {
            if (s->tu.cur_intra_pred_mode >= 6 &&
                s->tu.cur_intra_pred_mode <= 14) {
                scan_idx = SCAN_VERT;
            } else if (s->tu.cur_intra_pred_mode >= 22 &&
                       s->tu.cur_intra_pred_mode <= 30) {
                scan_idx = SCAN_HORIZ;
            }

            if (s->pu.intra_pred_mode_c >= 6 &&
                s->pu.intra_pred_mode_c <= 14) {
                scan_idx_c = SCAN_VERT;
            } else if (s->pu.intra_pred_mode_c >= 22 &&
                       s->pu.intra_pred_mode_c <= 30) {
                scan_idx_c = SCAN_HORIZ;
            }
        }

        if (s->tt.cbf_luma)
            hls_residual_coding(s, x0, y0, log2_trafo_size, scan_idx, 0);
        if (log2_trafo_size > 2) {
            if (SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0))
                hls_residual_coding(s, x0, y0, log2_trafo_size - 1, scan_idx_c, 1);
            if (SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0))
                hls_residual_coding(s, x0, y0, log2_trafo_size - 1, scan_idx_c, 2);
        } else if (blk_idx == 3) {
            if (SAMPLE(s->tt.cbf_cb[trafo_depth], xBase, yBase))
                hls_residual_coding(s, xBase, yBase, log2_trafo_size, scan_idx_c, 1);
            if (SAMPLE(s->tt.cbf_cr[trafo_depth], xBase, yBase))
                hls_residual_coding(s, xBase, yBase, log2_trafo_size, scan_idx_c, 2);
        }
    }
}

static void hls_transform_tree(HEVCContext *s, int x0, int y0,
                               int xBase, int yBase, int log2_cb_size, int log2_trafo_size,
                               int trafo_depth, int blk_idx)
{

    if (trafo_depth > 0 && log2_trafo_size == 2) {
        SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0) =
        SAMPLE(s->tt.cbf_cb[trafo_depth - 1], xBase, yBase);
        SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0) =
        SAMPLE(s->tt.cbf_cr[trafo_depth - 1], xBase, yBase);
    } else {
        SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0) =
        SAMPLE(s->tt.cbf_cb[trafo_depth - 1], xBase, yBase);
        SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0) =
        SAMPLE(s->tt.cbf_cr[trafo_depth - 1], xBase, yBase);
    }

    if (s->cu.intra_split_flag) {
        if (trafo_depth == 1)
            s->tu.cur_intra_pred_mode = s->pu.intra_pred_mode[blk_idx];
    } else {
        s->tu.cur_intra_pred_mode = s->pu.intra_pred_mode[0];
    }

    s->tt.cbf_luma = 1;

    s->tt.inter_split_flag = (s->sps->max_transform_hierarchy_depth_inter == 0 &&
                              s->cu.pred_mode == MODE_INTER &&
                              s->cu.part_mode != PART_2Nx2N && trafo_depth == 0);

    if (log2_trafo_size <= s->sps->log2_min_transform_block_size +
        s->sps->log2_diff_max_min_coding_block_size &&
        log2_trafo_size > s->sps->log2_min_transform_block_size &&
        trafo_depth < s->cu.max_trafo_depth &&
        !(s->cu.intra_split_flag && trafo_depth == 0)) {
        SAMPLE(s->tt.split_transform_flag[trafo_depth], x0, y0) =
        ff_hevc_split_transform_flag_decode(s, log2_trafo_size);
    } else {
        SAMPLE(s->tt.split_transform_flag[trafo_depth], x0, y0) =
        (log2_trafo_size >
         s->sps->log2_min_transform_block_size +
         s->sps->log2_diff_max_min_coding_block_size ||
         (s->cu.intra_split_flag && trafo_depth == 0) ||
         s->tt.inter_split_flag);
    }

    if (trafo_depth == 0 || log2_trafo_size > 2) {
        if (trafo_depth == 0 || SAMPLE(s->tt.cbf_cb[trafo_depth - 1], xBase, yBase)) {
            SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0) =
            ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            av_log(s->avctx, AV_LOG_DEBUG,
                   "cbf_cb: %d\n", SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0));
        }
        if (trafo_depth == 0 || SAMPLE(s->tt.cbf_cr[trafo_depth - 1], xBase, yBase)) {
            SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0) =
            ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            av_log(s->avctx, AV_LOG_DEBUG,
                   "cbf_cr: %d\n", SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0));
        }
    }

    if (SAMPLE(s->tt.split_transform_flag[trafo_depth], x0, y0)) {
        int x1 = x0 + ((1 << log2_trafo_size) >> 1);
        int y1 = y0 + ((1 << log2_trafo_size) >> 1);

        hls_transform_tree(s, x0, y0, x0, y0, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 0);
        hls_transform_tree(s, x1, y0, x0, y0, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 1);
        hls_transform_tree(s, x0, y1, x0, y0, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 2);
        hls_transform_tree(s, x1, y1, x0, y0, log2_cb_size,
                           log2_trafo_size - 1, trafo_depth + 1, 3);
    } else {
        if (s->cu.pred_mode == MODE_INTRA || trafo_depth != 0 ||
            SAMPLE(s->tt.cbf_cb[trafo_depth], x0, y0) ||
            SAMPLE(s->tt.cbf_cr[trafo_depth], x0, y0))
            s->tt.cbf_luma = ff_hevc_cbf_luma_decode(s, trafo_depth);

        hls_transform_unit(s, x0, y0, xBase,
                           yBase, log2_trafo_size, trafo_depth, blk_idx);
    }

}

static void hls_pcm_sample(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int i, j;
    GetBitContext *gb = &s->gb;
    int cb_size = 1 << log2_cb_size;

    // Directly fill the current frame (section 8.4)
    for (j = 0; j < cb_size; j++)
        for (i = 0; i < cb_size; i++)
            s->frame.data[0][(y0 + j) * s->frame.linesize[0] + (x0 + i)]
                = get_bits(gb, s->sps->pcm.bit_depth_luma) <<
                (s->sps->bit_depth[0] - s->sps->pcm.bit_depth_luma);

    //TODO: put the samples at the correct place in the frame
    for (i = 0; i < (1 << (log2_cb_size << 1)) >> 1; i++)
        get_bits(gb, s->sps->pcm.bit_depth_chroma);

    s->num_pcm_block--;
}

static void hls_mvd_coding(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int abs_mvd_greater0_flag[2];
    int abs_mvd_greater1_flag[2] = { 0, 0 };
    int abs_mvd_minus2[2];
    int mvd_sign_flag[2];
    int mvd_x;
    int mvd_y;
    abs_mvd_greater0_flag[0] = ff_hevc_abs_mvd_greater0_flag_decode(s);
    abs_mvd_greater0_flag[1] = ff_hevc_abs_mvd_greater0_flag_decode(s);
    if (abs_mvd_greater0_flag[0])
        abs_mvd_greater1_flag[0] = ff_hevc_abs_mvd_greater1_flag_decode(s);

    if (abs_mvd_greater0_flag[1])
        abs_mvd_greater1_flag[1] = ff_hevc_abs_mvd_greater1_flag_decode(s);

    if (abs_mvd_greater0_flag[0]) {
        if (abs_mvd_greater1_flag[0])
            abs_mvd_minus2[0] = ff_hevc_abs_mvd_minus2_decode(s);
        mvd_sign_flag[0] = ff_hevc_mvd_sign_flag_decode(s);
    }
    if (abs_mvd_greater0_flag[1]) {
        if (abs_mvd_greater1_flag[1])
            abs_mvd_minus2[1] = ff_hevc_abs_mvd_minus2_decode(s);
        mvd_sign_flag[1] = ff_hevc_mvd_sign_flag_decode(s);
    }
    mvd_x = abs_mvd_greater0_flag[0] * (abs_mvd_minus2[0] + 2) * (1 - (mvd_sign_flag[0] << 1));
    mvd_y = abs_mvd_greater0_flag[1] * (abs_mvd_minus2[1] + 2) * (1 - (mvd_sign_flag[1] << 1));
    return;
}

static void hls_prediction_unit(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int merge_idx;
    enum InterPredIdc inter_pred_idc = PRED_L0;
    int ref_idx_l0;
    int ref_idx_l1;
    int mvp_l0_flag;
    int mvp_l1_flag;

    if (SAMPLE(s->cu.skip_flag, x0, y0)) {
        if (s->sh.max_num_merge_cand > 1) {
            merge_idx = ff_hevc_merge_idx_decode(s);
        }
    } else {/* MODE_INTER */
        s->pu.merge_flag = ff_hevc_merge_flag_decode(s);
        if (s->pu.merge_flag) {
            if (s->sh.max_num_merge_cand > 1)
                merge_idx = ff_hevc_merge_idx_decode(s);
        } else {
            if (s->sh.slice_type == B_SLICE)
                inter_pred_idc = ff_hevc_inter_pred_idc_decode(s, 1<<log2_cb_size);
            if (inter_pred_idc != PRED_L1) {
                if (s->sh.num_ref_idx_l0_active > 1)
                    ref_idx_l0 = ff_hevc_ref_idx_lx_decode(s, s->sh.num_ref_idx_l0_active);
                hls_mvd_coding(s, x0, y0, 0 );
                mvp_l0_flag = ff_hevc_mvp_lx_flag_decode(s);
            }
            if (inter_pred_idc != PRED_L0) {
                if (s->sh.num_ref_idx_l1_active > 1)
                    ref_idx_l1 = ff_hevc_ref_idx_lx_decode(s, s->sh.num_ref_idx_l1_active);
                if (s->sh.mvd_l1_zero_flag == 1 && inter_pred_idc == PRED_BI) {
                    //mvd_l1[ x0 ][ y0 ][ 0 ] = 0
                    //mvd_l1[ x0 ][ y0 ][ 1 ] = 0
                } else {
                    hls_mvd_coding(s, x0, y0, 1 );
                }
                mvp_l1_flag = ff_hevc_mvp_lx_flag_decode(s);
            }
        }
    }
    return;
}

/**
 * 8.4.1
 */
static int luma_intra_pred_mode(HEVCContext *s, int x0, int y0, int pu_size,
                                int prev_intra_luma_pred_flag)
{
    int i;
    int candidate[3];
    int intra_pred_mode;

    int x_pu = x0 >> s->sps->log2_min_pu_size;
    int y_pu = y0 >> s->sps->log2_min_pu_size;
    int size_in_pus = pu_size >> s->sps->log2_min_pu_size;

    int cand_up = s->pu.top_ipm[x_pu];
    int cand_left = s->pu.left_ipm[y_pu];

    int y_ctb = (y0 >> (s->sps->log2_ctb_size)) << (s->sps->log2_ctb_size);

    // intra_pred_mode prediction does not cross vertical CTB boundaries
    if ((y0 - 1) < y_ctb)
        cand_up = INTRA_DC;

    av_log(s->avctx, AV_LOG_DEBUG, "cand_left: %d, cand_up: %d\n",
           cand_left, cand_up);

    if (cand_left == cand_up) {
        if (cand_left < 2) {
            candidate[0] = INTRA_PLANAR;
            candidate[1] = INTRA_DC;
            candidate[2] = INTRA_ANGULAR_26;
        } else {
            candidate[0] = cand_left;
            candidate[1] = 2 + ((cand_left - 2 - 1 + 32) % 32);
            candidate[2] = 2 + ((cand_left - 2 + 1) % 32);
        }
    } else {
        candidate[0] = cand_left;
        candidate[1] = cand_up;
        if (candidate[0] != INTRA_PLANAR && candidate[1] != INTRA_PLANAR) {
            candidate[2] = INTRA_PLANAR;
        } else if (candidate[0] != INTRA_DC && candidate[1] != INTRA_DC) {
            candidate[2] = INTRA_DC;
        } else {
            candidate[2] = INTRA_ANGULAR_26;
        }
    }

    if (prev_intra_luma_pred_flag) {
        intra_pred_mode = candidate[s->pu.mpm_idx];
    } else {
        if (candidate[0] > candidate[1])
            FFSWAP(uint8_t, candidate[0], candidate[1]);
        if (candidate[0] > candidate[2])
            FFSWAP(uint8_t, candidate[0], candidate[2]);
        if (candidate[1] > candidate[2])
            FFSWAP(uint8_t, candidate[1], candidate[2]);

        intra_pred_mode = s->pu.rem_intra_luma_pred_mode;
        for (i = 0; i < 3; i++) {
            av_log(s->avctx, AV_LOG_DEBUG, "candidate[%d] = %d\n",
                   i, candidate[i]);
            if (intra_pred_mode >= candidate[i])
                intra_pred_mode++;
        }
    }

    memset(&s->pu.top_ipm[x_pu], intra_pred_mode, size_in_pus);
    memset(&s->pu.left_ipm[y_pu], intra_pred_mode, size_in_pus);

    av_log(s->avctx, AV_LOG_DEBUG, "intra_pred_mode: %d\n",
           intra_pred_mode);
    return intra_pred_mode;
}

static av_always_inline void set_ct_depth(HEVCContext *s, int x0, int y0,
                                          int log2_cb_size, int ct_depth)
{
    int length = (1 << log2_cb_size) >> s->sps->log2_min_coding_block_size;
    int x_cb = x0 >> s->sps->log2_min_coding_block_size;
    int y_cb = y0 >> s->sps->log2_min_coding_block_size;

    memset(&s->cu.top_ct_depth[x_cb], ct_depth, length);
    memset(&s->cu.left_ct_depth[y_cb], ct_depth, length);
}

static void intra_prediction_unit(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int i, j;
    uint8_t prev_intra_luma_pred_flag[4];
    int chroma_mode;
    static const uint8_t intra_chroma_table[4] = {0, 26, 10, 1};

    int split = s->cu.part_mode == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side = split + 1;

    for (i = 0; i < side; i++)
        for (j = 0; j < side; j++)
            prev_intra_luma_pred_flag[2*i+j] = ff_hevc_prev_intra_luma_pred_flag_decode(s);

    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            if (prev_intra_luma_pred_flag[2*i+j]) {
                s->pu.mpm_idx = ff_hevc_mpm_idx_decode(s);
                av_log(s->avctx, AV_LOG_DEBUG, "mpm_idx: %d\n", s->pu.mpm_idx);
            } else {
                s->pu.rem_intra_luma_pred_mode = ff_hevc_rem_intra_luma_pred_mode_decode(s);
                av_log(s->avctx, AV_LOG_DEBUG, "rem_intra_luma_pred_mode: %d\n", s->pu.rem_intra_luma_pred_mode);
            }
            s->pu.intra_pred_mode[2*i+j] =
            luma_intra_pred_mode(s, x0 + pb_size * j, y0 + pb_size * i, pb_size,
                                 prev_intra_luma_pred_flag[2*i+j]);
        }
    }

    chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
    if (chroma_mode != 4) {
        if (s->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode]) {
            s->pu.intra_pred_mode_c = 34;
        } else {
            s->pu.intra_pred_mode_c = intra_chroma_table[chroma_mode];
        }
    } else {
        s->pu.intra_pred_mode_c = s->pu.intra_pred_mode[0];
    }

    av_log(s->avctx, AV_LOG_DEBUG, "intra_pred_mode_c: %d\n",
           s->pu.intra_pred_mode_c);
}
static void intra_prediction_unit_default_value(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int i, j;
    int split = s->cu.part_mode == PART_NxN;
    int side = split + 1;
    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            int x_pu = x0 >> s->sps->log2_min_pu_size;
            int y_pu = y0 >> s->sps->log2_min_pu_size;
            int size_in_pus = log2_cb_size >> s->sps->log2_min_pu_size;
            memset(&s->pu.top_ipm[x_pu], INTRA_DC, size_in_pus);
            memset(&s->pu.left_ipm[y_pu], INTRA_DC, size_in_pus);
        }
    }
}

static void hls_coding_unit(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size = 1 << log2_cb_size;
    int x1, y1, x2, y2, x3, y3;

    int log2_min_cb_size = s->sps->log2_min_coding_block_size;
    int length = cb_size >> log2_min_cb_size;
    int x_cb = x0 >> log2_min_cb_size;
    int y_cb = y0 >> log2_min_cb_size;
    int x, y;

    s->cu.x = x0;
    s->cu.y = y0;
    s->cu.no_residual_data_flag = 1;
    s->cu.pcm_flag = 0;

    s->cu.pred_mode = MODE_INTRA;
    s->cu.part_mode = PART_2Nx2N;
    s->cu.intra_split_flag = 0;
    SAMPLE(s->cu.skip_flag, x0, y0) = 0;
    for (x = 0; x < 4; x++) {
        s->pu.intra_pred_mode[x] = 1;
    }
    if (s->pps->transquant_bypass_enable_flag)
        s->cu.cu_transquant_bypass_flag = ff_hevc_cu_transquant_bypass_flag_decode(s);

    if (s->sh.slice_type != I_SLICE) {
        s->cu.pred_mode = MODE_SKIP;
        SAMPLE(s->cu.skip_flag, x0, y0) = ff_hevc_skip_flag_decode(s, x_cb, y_cb);
        for (x = 0; x < length; x++) {
            for (y = 0; y < length; y++) {
                SAMPLE(s->cu.skip_flag, x_cb+x, y_cb+y) = SAMPLE(s->cu.skip_flag, x0, y0);
            }
        }
        s->cu.pred_mode = s->cu.skip_flag ? MODE_SKIP : MODE_INTER;
    }

    if (SAMPLE(s->cu.skip_flag, x0, y0)) {
        hls_prediction_unit(s, x0, y0, log2_cb_size);
    } else {
        if (s->sh.slice_type != I_SLICE) {
            s->cu.pred_mode = ff_hevc_pred_mode_decode(s);
        }
        if (s->cu.pred_mode != MODE_INTRA ||
            log2_cb_size == s->sps->log2_min_coding_block_size) {
            s->cu.part_mode = ff_hevc_part_mode_decode(s, log2_cb_size);
            av_log(s->avctx, AV_LOG_DEBUG, "part_mode: %d\n", s->cu.part_mode);
            s->cu.intra_split_flag = s->cu.part_mode == PART_NxN &&
                                     s->cu.pred_mode == MODE_INTRA;
        }

        if (s->cu.pred_mode == MODE_INTRA) {
            if (s->cu.part_mode == PART_2Nx2N && s->sps->pcm_enabled_flag &&
                log2_cb_size >= s->sps->pcm.log2_min_pcm_coding_block_size &&
                log2_cb_size <= s->sps->pcm.log2_min_pcm_coding_block_size + s->sps->pcm.log2_diff_max_min_pcm_coding_block_size) {
                s->cu.pcm_flag = ff_hevc_pcm_flag_decode(s);
                av_log(s->avctx, AV_LOG_ERROR, "pcm_flag: %d\n", s->cu.pcm_flag);
            }
            if (s->cu.pcm_flag) {
                s->num_pcm_block = 1;
                while (s->num_pcm_block < 4 && get_bits1(&s->gb))
                    s->num_pcm_block++;

                align_get_bits(&s->gb);
                hls_pcm_sample(s, x0, y0, log2_cb_size);
                intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
            } else {
                intra_prediction_unit(s, x0, y0, log2_cb_size);
            }
        } else {
            intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
            x1 = x0 + (cb_size >> 1);
            y1 = y0 + (cb_size >> 1);
            x2 = x1 - (cb_size >> 2);
            y2 = y1 - (cb_size >> 2);
            x3 = x1 + (cb_size >> 2);
            y3 = y1 + (cb_size >> 2);

            switch (s->cu.part_mode) {
            case PART_2Nx2N:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                break;
            case PART_2NxN:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x0, y1, log2_cb_size);
                break;
            case PART_Nx2N:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x1, y0, log2_cb_size);
                break;
            case PART_2NxnU:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x0, y2, log2_cb_size);
                break;
            case PART_2NxnD:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x0, y3, log2_cb_size);
                break;
            case PART_nLx2N:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x2, y0, log2_cb_size);
                break;
            case PART_nRx2N:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x3, y0, log2_cb_size);
                break;
            case PART_NxN:
                hls_prediction_unit(s, x0, y0, log2_cb_size);
                hls_prediction_unit(s, x1, y0, log2_cb_size);
                hls_prediction_unit(s, x0, y1, log2_cb_size);
                hls_prediction_unit(s, x1, y1, log2_cb_size);
                break;
            }
        }
        if (!s->cu.pcm_flag) {
            if (s->cu.pred_mode != MODE_INTRA &&
                !(s->cu.part_mode == PART_2Nx2N && s->pu.merge_flag)) {
                s->cu.no_residual_data_flag = ff_hevc_no_residual_syntax_flag_decode(s);
            }
            if (s->cu.no_residual_data_flag) {
                s->cu.max_trafo_depth = s->cu.pred_mode == MODE_INTRA ?
                                        s->sps->max_transform_hierarchy_depth_intra + s->cu.intra_split_flag :
                                        s->sps->max_transform_hierarchy_depth_inter;
                hls_transform_tree(s, x0, y0, x0, y0, log2_cb_size,
                                   log2_cb_size, 0, 0);
            }
        }
    }

    set_ct_depth(s, x0, y0, log2_cb_size, s->ct.depth);
}

static int hls_coding_tree(HEVCContext *s, int x0, int y0, int log2_cb_size, int cb_depth)
{
    s->ct.depth = cb_depth;
    if ((x0 + (1 << log2_cb_size) <= s->sps->pic_width_in_luma_samples) &&
        (y0 + (1 << log2_cb_size) <= s->sps->pic_height_in_luma_samples) &&
        min_cb_addr_zs(s, x0 >> s->sps->log2_min_coding_block_size,
                       y0 >> s->sps->log2_min_coding_block_size) >= s->sh.slice_cb_addr_zs &&
        log2_cb_size > s->sps->log2_min_coding_block_size && s->num_pcm_block == 0) {
        SAMPLE(s->split_coding_unit_flag, x0, y0) =
        ff_hevc_split_coding_unit_flag_decode(s, cb_depth, x0, y0);
    } else {
        SAMPLE(s->split_coding_unit_flag, x0, y0) =
        (log2_cb_size > s->sps->log2_min_coding_block_size);
    }
    av_log(s->avctx, AV_LOG_DEBUG, "split_coding_unit_flag: %d\n",
           SAMPLE(s->split_coding_unit_flag, x0, y0));

    if (SAMPLE(s->split_coding_unit_flag, x0, y0)) {
        int more_data = 0;
        int cb_size = (1 << (log2_cb_size)) >> 1;
        int x1 = x0 + cb_size;
        int y1 = y0 + cb_size;

        more_data = hls_coding_tree(s, x0, y0, log2_cb_size - 1, cb_depth + 1);

        if (more_data && x1 < s->sps->pic_width_in_luma_samples)
            more_data = hls_coding_tree(s, x1, y0, log2_cb_size - 1, cb_depth + 1);
        if (more_data && y1 < s->sps->pic_height_in_luma_samples)
            more_data = hls_coding_tree(s, x0, y1, log2_cb_size - 1, cb_depth + 1);
        if (more_data && x1 < s->sps->pic_width_in_luma_samples &&
           y1 < s->sps->pic_height_in_luma_samples) {
            return hls_coding_tree(s, x1, y1, log2_cb_size - 1, cb_depth + 1);
        }
        return ((x1 + cb_size) < s->sps->pic_width_in_luma_samples ||
                (y1 + cb_size) < s->sps->pic_height_in_luma_samples);
    } else {
        if (s->num_pcm_block == 0) {
            hls_coding_unit(s, x0, y0, log2_cb_size);
        } else {
            hls_pcm_sample(s, x0, y0, log2_cb_size);
        }

        av_log(s->avctx, AV_LOG_DEBUG, "x0: %d, y0: %d, cb: %d, %d\n",
               x0, y0, (1 << log2_cb_size), (1 << (s->sps->log2_ctb_size)));
        if ((!((x0 + (1 << log2_cb_size)) %
               (1 << (s->sps->log2_ctb_size))) ||
             (x0 + (1 << log2_cb_size) >= s->sps->pic_width_in_luma_samples)) &&
            (!((y0 + (1 << log2_cb_size)) %
               (1 << (s->sps->log2_ctb_size))) ||
             (y0 + (1 << log2_cb_size) >= s->sps->pic_height_in_luma_samples)) &&
            s->num_pcm_block == 0) {
            int end_of_slice_flag = ff_hevc_end_of_slice_flag_decode(s);
            return !end_of_slice_flag;
        } else {
            return 1;
        }
    }

    return 0;
}

/**
 * 7.3.4
 */
static int hls_slice_data(HEVCContext *s)
{
    int ctb_size = 1 << s->sps->log2_ctb_size;
    int pic_size = s->sps->pic_width_in_luma_samples * s->sps->pic_height_in_luma_samples;
    int more_data = 1;
    int x_ctb, y_ctb;

    memset(s->cu.skip_flag, 0, pic_size);

    s->ctb_addr_rs = s->sh.slice_ctb_addr_rs;
    s->ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[s->ctb_addr_rs];

    while (s->ctb_addr_ts < s->sps->pic_width_in_ctbs*s->sps->pic_height_in_ctbs) {
        x_ctb = INVERSE_RASTER_SCAN(s->ctb_addr_rs, ctb_size, ctb_size, s->sps->pic_width_in_luma_samples, 0);
        y_ctb = INVERSE_RASTER_SCAN(s->ctb_addr_rs, ctb_size, ctb_size, s->sps->pic_width_in_luma_samples, 1);
        s->num_pcm_block = 0;
        s->ctb_addr_in_slice = s->ctb_addr_rs - s->sh.slice_address;
        if (s->sh.slice_sample_adaptive_offset_flag[0] ||
            s->sh.slice_sample_adaptive_offset_flag[1])
            hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        more_data = hls_coding_tree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0);
        if (!more_data)
            return 0;

        s->ctb_addr_ts++;
        s->ctb_addr_rs = s->pps->ctb_addr_ts_to_rs[s->ctb_addr_ts];

        if (more_data && (s->pps->tiles_enabled_flag &&
                          s->pps->tile_id[s->ctb_addr_ts] !=
                          s->pps->tile_id[s->ctb_addr_ts - 1]) ||
            (s->pps->tiles_enabled_flag &&
             ((s->ctb_addr_ts % s->sps->pic_width_in_ctbs) == 0)))
            align_get_bits(&s->gb);
    }

    return 0;
}

/**
 * @return AVERROR_INVALIDDATA if the packet is not a valid NAL unit,
 * 0 if the unit should be skipped, 1 otherwise
 */
static int hls_nal_unit(HEVCContext *s)
{
    GetBitContext *gb = &s->gb;
    int ret;

    if (get_bits1(gb) != 0)
        return AVERROR_INVALIDDATA;

    s->nal_unit_type = get_bits(gb, 6);

    s->temporal_id = get_bits(gb, 3) - 1;
    ret = (get_bits(gb, 6) != 0);

    av_log(s->avctx, AV_LOG_DEBUG,
           "nal_ref_flag: %d, nal_unit_type: %d, temporal_id: %d\n",
           s->nal_ref_flag, s->nal_unit_type, s->temporal_id);

    return ret;
}

/**
 * Note: avpkt->data must contain exactly one NAL unit
 */
static int hevc_decode_frame(AVCodecContext *avctx, void *data, int *data_size,
                             AVPacket *avpkt)
{
    HEVCContext *s = avctx->priv_data;
    GetBitContext *gb = &s->gb;


    *data_size = 0;

    init_get_bits(gb, avpkt->data, avpkt->size*8);

    av_log(s->avctx, AV_LOG_DEBUG, "=================\n");

    if (hls_nal_unit(s) <= 0) {
        av_log(s->avctx, AV_LOG_INFO, "Skipping NAL unit\n");
        return avpkt->size;
    }

    switch (s->nal_unit_type) {
    case NAL_VPS:
        ff_hevc_decode_nal_vps(s);
        break;
    case NAL_SPS:
        ff_hevc_decode_nal_sps(s);
        break;
    case NAL_PPS:
        ff_hevc_decode_nal_pps(s);
        break;
    case NAL_SEI:
        ff_hevc_decode_nal_sei(s);
        break;
    case NAL_TRAIL_R:
        // fall-through
    case NAL_TRAIL_N: {
        int pic_height_in_min_pu = s->sps->pic_height_in_min_cbs * 4;
        int pic_width_in_min_pu = s->sps->pic_width_in_min_cbs * 4;

        memset(s->pu.left_ipm, INTRA_DC, pic_height_in_min_pu);
        memset(s->pu.top_ipm, INTRA_DC, pic_width_in_min_pu);
        // fall-through
    }
    case NAL_IDR_W_DLP:
        if (hls_slice_header(s) < 0)
            return -1;

        if (s->frame.data[0])
            s->avctx->release_buffer(s->avctx, &s->frame);
        if (s->avctx->get_buffer(s->avctx, &s->frame) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return -1;
        }

        ff_hevc_cabac_init(s);
        if (hls_slice_data(s) < 0)
            return -1;

        if (s->sao_frame.data[0])
            s->avctx->release_buffer(s->avctx, &s->sao_frame);
        if (s->avctx->get_buffer(s->avctx, &s->sao_frame) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return -1;
        }

        if(!s->sh.disable_deblocking_filter_flag)
            deblocking_filter(s);

        av_picture_copy((AVPicture*)&s->sao_frame, (AVPicture*)&s->frame,
                        s->avctx->pix_fmt, s->avctx->width, s->avctx->height);

        if (s->sps->sample_adaptive_offset_enabled_flag)
            sao_filter(s);

        s->frame.pict_type = AV_PICTURE_TYPE_I;
        s->frame.key_frame = 1;
        *(AVFrame*)data = s->sao_frame;
        *data_size = sizeof(AVFrame);
        break;
    default:
        av_log(s->avctx, AV_LOG_INFO, "Skipping NAL unit\n");
        return avpkt->size;
    }

    av_log(s->avctx, AV_LOG_DEBUG, "%d bits left in unit\n", get_bits_left(gb));
    return avpkt->size;
}

static av_cold int hevc_decode_init(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;

    s->avctx = avctx;
    memset(s->sps_list, 0, sizeof(s->sps_list));
    memset(s->pps_list, 0, sizeof(s->pps_list));

    s->hevcdsp[0] = av_malloc(sizeof(*s->hevcdsp[0]));
    s->hevcdsp[2] =
    s->hevcdsp[1] = av_malloc(sizeof(*s->hevcdsp[0]));
    s->hpc[0] = av_malloc(sizeof(*s->hpc[0]));
    s->hpc[2] =
    s->hpc[1] = av_malloc(sizeof(*s->hpc[1]));

    if (!s->hevcdsp[0] || !s->hevcdsp[1] || !s->hpc[0] || !s->hpc[1])
        return -1;
    return 0;
}

static av_cold int hevc_decode_free(AVCodecContext *avctx)
{
    int i;
    HEVCContext *s = avctx->priv_data;

    if (s->frame.data[0])
        s->avctx->release_buffer(s->avctx, &s->frame);
    if (s->sao_frame.data[0])
        s->avctx->release_buffer(s->avctx, &s->sao_frame);

    for (i = 0; i < MAX_SPS_COUNT; i++) {
        av_freep(&s->sps_list[i]);
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (s->pps_list[i]) {
            av_freep(&s->pps_list[i]->column_width);
            av_freep(&s->pps_list[i]->row_height);
            av_freep(&s->pps_list[i]->col_bd);
            av_freep(&s->pps_list[i]->row_bd);
            av_freep(&s->pps_list[i]->ctb_addr_rs_to_ts);
            av_freep(&s->pps_list[i]->ctb_addr_ts_to_rs);
            av_freep(&s->pps_list[i]->tile_id);
            av_freep(&s->pps_list[i]->min_cb_addr_zs);
            av_freep(&s->pps_list[i]->min_tb_addr_zs);
        }
        av_freep(&s->pps_list[i]);
    }

    pic_arrays_free(s);
    return 0;
}

static void hevc_decode_flush(AVCodecContext *avctx)
{
}

AVCodec ff_hevc_decoder = {
    .name           = "hevc",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .priv_data_size = sizeof(HEVCContext),
    .init           = hevc_decode_init,
    .close          = hevc_decode_free,
    .decode         = hevc_decode_frame,
    .capabilities   = 0,
    .flush          = hevc_decode_flush,
    .long_name      = NULL_IF_CONFIG_SMALL("HEVC (High Efficiency Video Coding)"),
};
