/*
 * HEVC Parameter Set Decoding
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

#include "golomb.h"
#include "hevc.h"

/**
 * Section 7.3.3.1
 */
int ff_hevc_decode_short_term_rps(HEVCContext *s, int idx, SPS *sps)
{
    int delta_idx = 1;
    int delta_rps;
    uint8_t used_by_curr_pic_flag;
    uint8_t use_delta_flag;
    int delta_poc;
    int k0 = 0;
    int k1 = 0;
    int k  = 0;
    int delta_poc_s0_minus1, delta_poc_s1_minus1;
    int used_by_curr_pic_s0_flag, used_by_curr_pic_s1_flag;
    int i;
    GetBitContext *gb = &s->gb;

    ShortTermRPS *rps = &sps->short_term_rps_list[idx];
    ShortTermRPS *rps_ridx;

    if (idx != 0) {
        rps->inter_ref_pic_set_prediction_flag = get_bits1(gb);
    } else {
        rps->inter_ref_pic_set_prediction_flag = 0;
    }
    if (rps->inter_ref_pic_set_prediction_flag) {
        if (idx == sps->num_short_term_ref_pic_sets) {
            delta_idx = get_ue_golomb(gb) + 1;
        }
        rps_ridx = &sps->short_term_rps_list[idx - delta_idx];
        rps->delta_rps_sign = get_bits1(gb);
        rps->abs_delta_rps = get_ue_golomb(gb) + 1;
        delta_rps = (1 - (rps_ridx->delta_rps_sign<<1)) * rps->abs_delta_rps;
        for (i = 0; i <= rps_ridx->num_delta_pocs; i++) {
            used_by_curr_pic_flag = get_bits1(gb);
            if (!used_by_curr_pic_flag) {
                use_delta_flag = get_bits1(gb);
            }
            if (used_by_curr_pic_flag || use_delta_flag) {
                if (i < rps_ridx->num_delta_pocs) {
                    delta_poc = delta_rps + rps_ridx->delta_poc;
                } else {
                    delta_poc = delta_rps;
                }
                if (delta_poc < 0) {
                    k0++;
                } else {
                    k1++;
                }
                k++;
            }
        }
        rps->num_delta_pocs    = k;
        rps->num_negative_pics = k0;
        rps->num_positive_pics = k1;
    } else {
        rps->num_negative_pics = get_ue_golomb(gb);
        rps->num_positive_pics = get_ue_golomb(gb);
        rps->num_delta_pocs = rps->num_negative_pics + rps->num_positive_pics;
        if (rps->num_negative_pics || rps->num_positive_pics) {
            for (i = 0; i < rps->num_negative_pics; i++) {
                delta_poc_s0_minus1      = get_ue_golomb(gb);
                used_by_curr_pic_s0_flag = get_bits1(gb);
            }
            for (i = 0; i < rps->num_positive_pics; i++) {
                delta_poc_s1_minus1      = get_ue_golomb(gb);
                used_by_curr_pic_s1_flag = get_bits1(gb);
            }
        }
    }

    return 0;
}

static int decode_profile_tier_level(HEVCContext *s, PTL *ptl,
                                      int profile_present_flag, int max_num_sub_layers)
{
    int i, j;
    GetBitContext *gb = &s->gb;

    if (profile_present_flag) {
        ptl->general_profile_space = get_bits(gb, 2);
        ptl->general_tier_flag = get_bits1(gb);
        ptl->general_profile_idc = get_bits(gb, 5);
        for (i = 0; i < 32; i++)
            ptl->general_profile_compatibility_flag[i] = get_bits1(gb);
        if (get_bits(gb, 16) != 0) // XXX_reserved_zero_48bits[0..15]
            return -1;
        if (get_bits(gb, 16) != 0) // XXX_reserved_zero_48bits[16..31]
            return -1;
        if (get_bits(gb, 16) != 0) // XXX_reserved_zero_48bits[32..47]
            return -1;
    }

    ptl->general_level_idc = get_bits(gb, 8);
    for (i = 0; i < max_num_sub_layers - 1; i++) {
        ptl->sub_layer_profile_present_flag[i] = get_bits1(gb);
        ptl->sub_layer_level_present_flag[i] = get_bits1(gb);
        if (profile_present_flag && ptl->sub_layer_profile_present_flag[i]) {
            ptl->sub_layer_profile_space[i] = get_bits(gb, 2);
            ptl->sub_layer_tier_flag[i] = get_bits(gb, 1);
            ptl->sub_layer_profile_idc[i] = get_bits(gb, 5);
            for (j = 0; j < 32; j++)
                ptl->sub_layer_profile_compatibility_flags[i][j] = get_bits1(gb);
            skip_bits(gb, 16); // sub_layer_reserved_zero_16bits[i]
        }
        if (ptl->sub_layer_level_present_flag[i])
            ptl->sub_layer_level_idc[i] = get_bits(gb, 8);
    }
    return 0;
}

static void decode_bit_rate_pic_rate(HEVCContext *s, int tempLevelLow, int tempLevelHigh)
{
    int i;
    int bit_rate_info_present_flag, pic_rate_info_present_flag;
    GetBitContext *gb = &s->gb;

    for (i = tempLevelLow; i <= tempLevelHigh; i++) {
        bit_rate_info_present_flag = get_bits1(gb);
        pic_rate_info_present_flag = get_bits1(gb);
        if (bit_rate_info_present_flag) {
            skip_bits(gb, 16); // avg_bit_rate[i]
            skip_bits(gb, 16); // max_bit_rate[i]
        }
        if (pic_rate_info_present_flag) {
            skip_bits(gb, 2);  // constant_pic_rate_idc[i]
            skip_bits(gb, 16); // avg_pic_rate[i]
        }
    }
}

int ff_hevc_decode_nal_vps(HEVCContext *s)
{
    int i,j;
    uint8_t vps_temporal_id_nesting_flag, vps_extension_flag;
    GetBitContext *gb = &s->gb;
    int vps_id = 0;
    VPS *vps = av_mallocz(sizeof(*vps));

    av_log(s->avctx, AV_LOG_DEBUG, "Decoding VPS\n");

    if (!vps)
        return -1;
    vps_id = get_bits(gb, 4) + 1;

    if (vps_id >= MAX_VPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "VPS id out of range: %d\n", vps_id);
        goto err;
    }

    if (get_bits(gb, 2) != 3) { // vps_reserved_three_2bits
        av_log(s->avctx, AV_LOG_ERROR, "vps_reserved_three_2bits is not three\n");
        goto err;
    }

    if (get_bits(gb, 6) != 0) { // vps_reserved_zero_6bits
        av_log(s->avctx, AV_LOG_ERROR, "vps_reserved_zero_6bits is not zero\n");
        goto err;
    }

    vps->vps_max_sub_layers = get_bits(gb, 3) + 1;
    vps->vps_temporal_id_nesting_flag = get_bits1(gb);
    if (get_bits(gb, 16) != 0xffff) { // vps_reserved_ffff_16bits
        av_log(s->avctx, AV_LOG_ERROR, "vps_reserved_ffff_16bits is not 0xffff\n");
        goto err;
    }

    if (vps->vps_max_sub_layers > MAX_SUB_LAYERS) {
        av_log(s->avctx, AV_LOG_ERROR, "vps_max_sub_layers out of range: %d\n",
               vps->vps_max_sub_layers);
        goto err;
    }

    if (decode_profile_tier_level(s, &vps->ptl, 1, vps->vps_max_sub_layers) != 0) {
        av_log(s->avctx, AV_LOG_ERROR, "error decoding profile tier level");
        goto err;
    }
    decode_bit_rate_pic_rate(s, 0, vps->vps_max_sub_layers - 1);

    vps->vps_sub_layer_ordering_info_present_flag = get_bits1(gb);
    j = vps->vps_sub_layer_ordering_info_present_flag ? 0 : (vps->vps_max_sub_layers - 1);
    for (i = j; i < vps->vps_max_sub_layers; i++) {
        vps->vps_max_dec_pic_buffering[i] = get_ue_golomb(gb);
        vps->vps_num_reorder_pics[i] = get_ue_golomb(gb);
        vps->vps_max_latency_increase[i] = get_ue_golomb(gb);
    }

    vps->vps_max_nuh_reserved_zero_layer_id = get_bits(gb, 6);
    vps->vps_max_op_sets = get_ue_golomb(gb) + 1;
    for (j = 1; j < vps->vps_max_op_sets; j++)
    {
        for (i = 0; i <= vps->vps_max_nuh_reserved_zero_layer_id; i++)
            skip_bits(gb, 1); // layer_id_included_flag[opsIdx][i]
    }
    vps->vps_timing_info_present_flag = get_bits1(gb);
    if(vps->vps_timing_info_present_flag) {
        vps->vps_num_units_in_tick = get_bits_long(gb, 32);
        vps->vps_time_scale = get_bits_long(gb, 32);
        vps->vps_poc_proportional_to_timing_flag = get_bits1(gb);
        if(vps->vps_poc_proportional_to_timing_flag)
            vps->vps_num_ticks_poc_diff_one = get_ue_golomb(gb) + 1;
        vps->vps_num_hrd_parameters = get_ue_golomb(gb);
        if (vps->vps_num_hrd_parameters != 0) {
            av_log_missing_feature(s->avctx, "support for vps_num_hrd_parameters != 0", 0);
            av_free(vps);
            return AVERROR_PATCHWELCOME;
        }
    }
    vps_extension_flag = get_bits1(gb);
    av_free(s->vps_list[vps_id]);
    s->vps_list[vps_id] = vps;
    return 0;

err:
    av_free(vps);
    return -1;
}

int ff_hevc_decode_nal_sps(HEVCContext *s)
{
    int i;
    int bit_depth_chroma, start;
    GetBitContext *gb = &s->gb;

    int sps_id = 0;
    SPS *sps = av_mallocz(sizeof(*sps));
    if (!sps)
        goto err;

    av_log(s->avctx, AV_LOG_DEBUG, "Decoding SPS\n");

    memset(sps->short_term_rps_list, 0, sizeof(sps->short_term_rps_list));

    // Coded parameters

    sps->vps_id = get_bits(gb, 4);
    if (sps->vps_id >= MAX_VPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "VPS id out of range: %d\n", sps->vps_id);
        goto err;
    }

    sps->sps_max_sub_layers = get_bits(gb, 3) + 1;
    if (sps->sps_max_sub_layers > MAX_SUB_LAYERS) {
        av_log(s->avctx, AV_LOG_ERROR, "vps_max_sub_layers out of range: %d\n",
               sps->sps_max_sub_layers);
        goto err;
    }

    sps->temporal_id_nesting_flag = get_bits1(gb);
    if(decode_profile_tier_level(s, &sps->ptl, 1, sps->sps_max_sub_layers) != 0) {
        av_log(s->avctx, AV_LOG_ERROR, "error decoding profile tier level");
        goto err;
    }
    sps_id = get_ue_golomb(gb);
    if (sps_id >= MAX_SPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "SPS id out of range: %d\n", sps_id);
        goto err;
    }

    sps->chroma_format_idc = get_ue_golomb(gb);
    if (sps->chroma_format_idc != 1)
    	av_log(s->avctx, AV_LOG_ERROR, " chroma_format_idc != 1 : error SEI\n");

    if (sps->chroma_format_idc == 3)
        sps->separate_colour_plane_flag = get_bits1(gb);

    sps->pic_width_in_luma_samples  = get_ue_golomb(gb);
    sps->pic_height_in_luma_samples = get_ue_golomb(gb);

    sps->pic_cropping_flag = get_bits1(gb);
    if (sps->pic_cropping_flag) {
        sps->pic_crop.left_offset   = get_ue_golomb(gb);
        sps->pic_crop.right_offset  = get_ue_golomb(gb);
        sps->pic_crop.top_offset    = get_ue_golomb(gb);
        sps->pic_crop.bottom_offset = get_ue_golomb(gb);
    }

    sps->bit_depth = get_ue_golomb(gb) + 8;
    bit_depth_chroma = get_ue_golomb(gb) + 8;
    if (bit_depth_chroma != sps->bit_depth) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Luma bit depth (%d) is different from chroma bit depth (%d), this is unsupported.\n",
               sps->bit_depth, bit_depth_chroma);
        goto err;
    }
    if (sps->bit_depth > 10) {
        av_log(s->avctx, AV_LOG_ERROR, "Unsupported bit depth: %d\n",
               sps->bit_depth);
        goto err;
    }

    sps->log2_max_poc_lsb = get_ue_golomb(gb) + 4;
    sps->sps_sub_layer_ordering_info_present_flag = get_bits1(gb);

    start = (sps->sps_sub_layer_ordering_info_present_flag ? 0 : (sps->sps_max_sub_layers-1));
    for (i = start; i < sps->sps_max_sub_layers; i++) {
        sps->temporal_layer[i].max_dec_pic_buffering = get_ue_golomb(gb);
        sps->temporal_layer[i].num_reorder_pics      = get_ue_golomb(gb);
        sps->temporal_layer[i].max_latency_increase  = get_ue_golomb(gb);
    }

    sps->log2_min_coding_block_size             = get_ue_golomb(gb) + 3;
    sps->log2_diff_max_min_coding_block_size    = get_ue_golomb(gb);
    sps->log2_min_transform_block_size          = get_ue_golomb(gb) + 2;
    sps->log2_diff_max_min_transform_block_size = get_ue_golomb(gb);

    sps->max_transform_hierarchy_depth_inter = get_ue_golomb(gb);
    sps->max_transform_hierarchy_depth_intra = get_ue_golomb(gb);

    sps->scaling_list_enable_flag = get_bits1(gb);
    if (sps->scaling_list_enable_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "TODO: scaling_list_enable_flag\n");
        goto err;
    }

    sps->amp_enabled_flag = get_bits1(gb);
    sps->sample_adaptive_offset_enabled_flag = get_bits1(gb);

    sps->pcm_enabled_flag = get_bits1(gb);
    if (sps->pcm_enabled_flag) {
        sps->pcm.bit_depth_luma = get_bits(gb, 4) + 1;
        sps->pcm.bit_depth_chroma = get_bits(gb, 4) + 1;
        sps->pcm.log2_min_pcm_coding_block_size = get_ue_golomb(gb) + 3;
        sps->pcm.log2_diff_max_min_pcm_coding_block_size = get_ue_golomb(gb);
        sps->pcm.loop_filter_disable_flag = get_bits1(gb);
    }

    sps->num_short_term_ref_pic_sets = get_ue_golomb(gb);
    for (i = 0; i < sps->num_short_term_ref_pic_sets; i++) {
        if (ff_hevc_decode_short_term_rps(s, i, sps) < 0)
            goto err;
    }

    sps->long_term_ref_pics_present_flag = get_bits1(gb);
    sps->sps_temporal_mvp_enabled_flag   = get_bits1(gb);
    sps->sps_strong_intra_smoothing_enable_flag = get_bits1(gb);
    sps->vui_parameters_present_flag = get_bits1(gb);
    sps->sps_extension_flag = get_bits1(gb);

    // Inferred parameters

    sps->log2_ctb_size = sps->log2_min_coding_block_size
                         + sps->log2_diff_max_min_coding_block_size;
    sps->pic_width_in_ctbs  = ( sps->pic_width_in_luma_samples  + (1 << sps->log2_ctb_size)-1 ) >> sps->log2_ctb_size;
    sps->pic_height_in_ctbs = ( sps->pic_height_in_luma_samples + (1 << sps->log2_ctb_size)-1 ) >> sps->log2_ctb_size;
    sps->pic_width_in_min_cbs = sps->pic_width_in_luma_samples >>
                                sps->log2_min_coding_block_size;
    sps->pic_height_in_min_cbs = sps->pic_height_in_luma_samples >>
                                 sps->log2_min_coding_block_size;
    sps->pic_width_in_min_tbs = sps->pic_width_in_luma_samples >>
                                sps->log2_min_transform_block_size;
    sps->pic_height_in_min_tbs = sps->pic_height_in_luma_samples >>
                                 sps->log2_min_transform_block_size;
    sps->log2_min_pu_size = sps->log2_min_coding_block_size - 1;

    sps->qp_bd_offset = 6 * (sps->bit_depth - 8);

    av_free(s->sps_list[sps_id]);
    s->sps_list[sps_id] = sps;
    return 0;

err:

    av_free(sps);
    return -1;
}

int ff_hevc_decode_nal_pps(HEVCContext *s)
{
    int i, j, x, y, ctb_addr_rs, tile_id;
    GetBitContext *gb = &s->gb;

    SPS *sps = 0;
    int pps_id = 0;
    int log2_diff_ctb_min_tb_size;

    PPS *pps = av_mallocz(sizeof(*pps));
    if (!pps)
        goto err;

    av_log(s->avctx, AV_LOG_DEBUG, "Decoding PPS\n");

    // Default values
    pps->loop_filter_across_tiles_enabled_flag = 1;
    pps->num_tile_columns     = 1;
    pps->num_tile_rows        = 1;
    pps->uniform_spacing_flag = 1;


    // Coded parameters
    pps_id = get_ue_golomb(gb);
    if (pps_id >= MAX_PPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", pps_id);
        goto err;
    }
    pps->sps_id = get_ue_golomb(gb);
    if (pps->sps_id >= MAX_SPS_COUNT) {
        av_log(s->avctx, AV_LOG_ERROR, "SPS id out of range: %d\n", pps->sps_id);
        goto err;
    }
    sps = s->sps_list[pps->sps_id];

    pps->dependent_slice_segments_enabled_flag = get_bits1(gb);
    pps->output_flag_present_flag = get_bits1(gb);
    pps->num_extra_slice_header_bits = get_bits(gb, 3);

    pps->sign_data_hiding_flag = get_bits1(gb);

    pps->cabac_init_present_flag = get_bits1(gb);

    pps->num_ref_idx_l0_default_active = get_ue_golomb(gb) + 1;
    pps->num_ref_idx_l1_default_active = get_ue_golomb(gb) + 1;

    pps->pic_init_qp_minus26 = get_se_golomb(gb);

    pps->constrained_intra_pred_flag = get_bits1(gb);
    pps->transform_skip_enabled_flag = get_bits1(gb);

    pps->cu_qp_delta_enabled_flag = get_bits1(gb);
    if (pps->cu_qp_delta_enabled_flag)
        pps->diff_cu_qp_delta_depth = get_ue_golomb(gb);

    pps->cb_qp_offset = get_se_golomb(gb);
    pps->cr_qp_offset = get_se_golomb(gb);
    pps->pic_slice_level_chroma_qp_offsets_present_flag = get_bits1(gb);

    pps->weighted_pred_flag            = get_bits1(gb);
    pps->weighted_bipred_flag          = get_bits1(gb);
    pps->transquant_bypass_enable_flag = get_bits1(gb);
    pps->tiles_enabled_flag               = get_bits1(gb);
    pps->entropy_coding_sync_enabled_flag = get_bits1(gb);

    if (pps->tiles_enabled_flag) {
        pps->num_tile_columns     = get_ue_golomb(gb) + 1;
        pps->num_tile_rows        = get_ue_golomb(gb) + 1;

        pps->column_width = av_malloc(pps->num_tile_columns * sizeof(*pps->column_width));
        pps->row_height   = av_malloc(pps->num_tile_rows * sizeof(*pps->row_height));
        if (!pps->column_width || !pps->row_height)
            goto err;

        pps->uniform_spacing_flag = get_bits1(gb);
        if (!pps->uniform_spacing_flag) {
            for (i = 0; i < pps->num_tile_columns - 1; i++) {
                pps->column_width[i] = get_ue_golomb(gb);
            }
            for (i = 0; i < pps->num_tile_rows - 1; i++) {
                pps->row_height[i] = get_ue_golomb(gb);
            }
        }
        pps->loop_filter_across_tiles_enabled_flag = get_bits1(gb);
    }

    pps->seq_loop_filter_across_slices_enabled_flag = get_bits1(gb);

    pps->deblocking_filter_control_present_flag = get_bits1(gb);
    if (pps->deblocking_filter_control_present_flag) {
        pps->deblocking_filter_override_enabled_flag = get_bits1(gb);
        pps->pps_disable_deblocking_filter_flag = get_bits1(gb);
        if (!pps->pps_disable_deblocking_filter_flag) {
            pps->beta_offset = get_se_golomb(gb) * 2;
            pps->tc_offset = get_se_golomb(gb) * 2;
        }
    }

    pps->pps_scaling_list_data_present_flag = get_bits1(gb);
    if (pps->pps_scaling_list_data_present_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "TODO: scaling_list_data_present_flag\n");
        goto err;
    }
    pps->lists_modification_present_flag = get_bits1(gb);
    pps->log2_parallel_merge_level = get_ue_golomb(gb) + 2;

    pps->slice_header_extension_present_flag = get_bits1(gb);
    pps->pps_extension_flag = get_bits1(gb);
    // Inferred parameters
    pps->col_bd = av_malloc((pps->num_tile_columns + 1) * sizeof(*pps->col_bd));
    pps->row_bd = av_malloc((pps->num_tile_rows + 1) * sizeof(*pps->row_bd));
    if (!pps->col_bd || !pps->row_bd)
        goto err;

    if (pps->uniform_spacing_flag) {
        pps->column_width = av_malloc(pps->num_tile_columns * sizeof(*pps->column_width));
        pps->row_height   = av_malloc(pps->num_tile_rows * sizeof(*pps->row_height));
        if (!pps->column_width || !pps->row_height)
            goto err;

        for (i = 0; i < pps->num_tile_columns; i++) {
            pps->column_width[i] = ((i + 1) * sps->pic_width_in_ctbs) / (pps->num_tile_columns) -
                                   (i * sps->pic_width_in_ctbs) / (pps->num_tile_columns);
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            pps->row_height[i] = ((i + 1) * sps->pic_height_in_ctbs) / (pps->num_tile_rows) -
                                 (i * sps->pic_height_in_ctbs) / (pps->num_tile_rows);
        }
    }

    pps->col_bd[0] = 0;
    for (i = 0; i < pps->num_tile_columns; i++)
        pps->col_bd[i+1] = pps->col_bd[i] + pps->column_width[i];

    pps->row_bd[0] = 0;
    for (i = 0; i < pps->num_tile_rows; i++)
        pps->row_bd[i+1] = pps->row_bd[i] + pps->row_height[i];

    /**
     * 6.5
     */
    pps->ctb_addr_rs_to_ts = av_malloc(sps->pic_width_in_ctbs *
                                       sps->pic_height_in_ctbs * sizeof(*pps->ctb_addr_rs_to_ts));
    pps->ctb_addr_ts_to_rs = av_malloc(sps->pic_width_in_ctbs *
                                       sps->pic_height_in_ctbs * sizeof(*pps->ctb_addr_ts_to_rs));
    pps->tile_id = av_malloc(sps->pic_width_in_ctbs *
                             sps->pic_height_in_ctbs * sizeof(*pps->tile_id));
    pps->min_cb_addr_zs = av_malloc(sps->pic_width_in_min_cbs *
                                    sps->pic_height_in_min_cbs * sizeof(*pps->min_cb_addr_zs));

    pps->min_tb_addr_zs = av_malloc(sps->pic_width_in_min_tbs *
                                    sps->pic_height_in_min_tbs * sizeof(*pps->min_tb_addr_zs));
    if (!pps->ctb_addr_rs_to_ts || !pps->ctb_addr_ts_to_rs ||
        !pps->tile_id || !pps->min_cb_addr_zs ||
        !pps->min_tb_addr_zs)
        goto err;

    for (ctb_addr_rs = 0;
         ctb_addr_rs < sps->pic_width_in_ctbs * sps->pic_height_in_ctbs;
         ctb_addr_rs++) {
        int tb_x = ctb_addr_rs % sps->pic_width_in_ctbs;
        int tb_y = ctb_addr_rs / sps->pic_width_in_ctbs;
        int tile_x = 0;
        int tile_y = 0;
        int val = 0;

        for (i = 0; i < pps->num_tile_columns; i++) {
            if (tb_x < pps->col_bd[i + 1]) {
                tile_x = i;
                break;
            }
        }

        for (i = 0; i < pps->num_tile_rows; i++) {
            if (tb_y < pps->row_bd[i + 1]) {
                tile_y = i;
                break;
            }
        }

        for (i = 0; i < tile_x; i++ )
            val += pps->row_height[tile_y] * pps->column_width[i];
        for (i = 0; i < tile_y; i++ )
            val += sps->pic_width_in_ctbs * pps->row_height[i];

        val += (tb_y - pps->row_bd[tile_y]) * pps->column_width[tile_x] +
               tb_x - pps->col_bd[tile_x];

        pps->ctb_addr_rs_to_ts[ctb_addr_rs] = val;
        pps->ctb_addr_ts_to_rs[val] = ctb_addr_rs;
    }

    for (j = 0, tile_id = 0; j < pps->num_tile_rows; j++)
        for (i = 0; i < pps->num_tile_columns; i++, tile_id++)
            for (y = pps->row_bd[j]; y < pps->row_bd[j+1]; y++)
                for (x = pps->col_bd[j]; x < pps->col_bd[j+1]; x++)
                    pps->tile_id[pps->ctb_addr_rs_to_ts[y * sps->pic_width_in_ctbs + x]] = tile_id;

    for (y = 0; y < sps->pic_height_in_min_cbs; y++) {
        for (x = 0; x < sps->pic_width_in_min_cbs; x++) {
            int tb_x = x >> sps->log2_diff_max_min_coding_block_size;
            int tb_y = y >> sps->log2_diff_max_min_coding_block_size;
            int ctb_addr_rs = sps->pic_width_in_ctbs * tb_y + tb_x;
            int val = pps->ctb_addr_rs_to_ts[ctb_addr_rs] <<
                      (sps->log2_diff_max_min_coding_block_size * 2);
            for (i = 0; i < sps->log2_diff_max_min_coding_block_size; i++) {
                int m = 1 << i;
                val += (m & x ? m*m : 0) + (m & y ? 2*m*m : 0);
            }
            pps->min_cb_addr_zs[y * sps->pic_width_in_min_cbs + x] = val;
        }
    }

    log2_diff_ctb_min_tb_size = sps->log2_ctb_size - sps->log2_min_transform_block_size;
    for (y = 0; y < sps->pic_height_in_min_tbs; y++) {
        for (x = 0; x < sps->pic_width_in_min_tbs; x++) {
            int tb_x = x >> log2_diff_ctb_min_tb_size;
            int tb_y = y >> log2_diff_ctb_min_tb_size;
            int ctb_addr_rs = sps->pic_width_in_ctbs * tb_y + tb_x;
            int val = pps->ctb_addr_rs_to_ts[ctb_addr_rs] <<
                      (log2_diff_ctb_min_tb_size * 2);
            for (i = 0; i < log2_diff_ctb_min_tb_size; i++) {
                int m = 1 << i;
                val += (m & x ? m*m : 0) + (m & y ? 2*m*m : 0);
            }
            pps->min_tb_addr_zs[y * sps->pic_width_in_min_tbs + x] = val;
        }
    }

    av_free(s->pps_list[pps_id]);
    s->pps_list[pps_id] = pps;
    return 0;

err:
    av_free(pps->column_width);
    av_free(pps->row_height);
    av_free(pps->col_bd);
    av_free(pps->row_bd);
    av_free(pps->ctb_addr_rs_to_ts);
    av_free(pps->ctb_addr_ts_to_rs);
    av_free(pps->tile_id);
    av_free(pps->min_cb_addr_zs);
    av_free(pps->min_tb_addr_zs);

    av_free(pps);
    return -1;
}

static void decode_nal_sei_decoded_picture_hash(HEVCContext *s, int payload_size)
{
	int cIdx, i;
	int hash_type;
	int picture_md5;
	int picture_crc;
	int picture_checksum;
	GetBitContext *gb = &s->gb;
	hash_type = get_bits(gb, 8);
	for( cIdx = 0; cIdx < 3/*((s->sps->chroma_format_idc == 0) ? 1 : 3)*/; cIdx++ ) {
		if ( hash_type == 0 ) {
			for( i = 0; i < 16; i++) {
				picture_md5 = get_bits(gb, 8);
			}
		} else if( hash_type == 1 ) {
			picture_crc = get_bits(gb, 16);
		} else if( hash_type == 2 ) {
			picture_checksum = get_bits(gb, 32);
		}
	}
}
static int decode_nal_sei_message(HEVCContext *s)
{
    GetBitContext *gb = &s->gb;

    int payload_type = 0;
    int payload_size = 0;
    int byte = 0xFF;
    av_log(s->avctx, AV_LOG_DEBUG, "Decoding SEI\n");

    while (byte == 0xFF) {
    	byte = get_bits(gb, 8);
        payload_type += byte;
    }
    byte = 0xFF;
    while (byte == 0xFF) {
       	byte = get_bits(gb, 8);
        payload_size += byte;
    }
    if (payload_type == 256)
    	decode_nal_sei_decoded_picture_hash(s, payload_size);
    else
    skip_bits(gb, 8*payload_size);
    return 0;
}

static int more_rbsp_data(GetBitContext *gb)
{
    return get_bits_left(gb) > 0 && show_bits(gb, 8) != 0x80;
}

int ff_hevc_decode_nal_sei(HEVCContext *s)
{
    do {
        decode_nal_sei_message(s);
    } while (more_rbsp_data(&s->gb));
    return 0;
}
