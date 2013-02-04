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

#ifndef AVCODEC_HEVC_H
#define AVCODEC_HEVC_H

#include "avcodec.h"
#include "cabac.h"
#include "dsputil.h"
#include "get_bits.h"
#include "hevcpred.h"
#include "hevcdsp.h"

/**
 * Enable to diverge from the spec when the reference encoder
 * does so.
 */
#define REFERENCE_ENCODER_QUIRKS 1

/**
 * Value of the luma sample at position (x, y) in the 2D array tab.
 */
#define SAMPLE(tab, x, y) ((tab)[(y) * s->sps->pic_width_in_luma_samples + (x)])
#define SAMPLE_CBF(tab, x, y) ((tab)[((y) & ((1<<log2_trafo_size)-1)) * MAX_CU_SIZE + ((x) & ((1<<log2_trafo_size)-1))])

/**
 * Table 7-3: NAL unit type codes
 */
enum NALUnitType {
    NAL_TRAIL_R   = 0,
    NAL_TRAIL_N   = 1,
    NAL_IDR_W_DLP = 19,
    NAL_VPS = 32,
    NAL_SPS = 33,
    NAL_PPS = 34,
    NAL_AUD = 35,
    NAL_FILLER_DATA = 38,
    NAL_SEI = 39,
};

typedef struct ShortTermRPS {
    uint8_t inter_ref_pic_set_prediction_flag;
    int num_negative_pics;
    int num_positive_pics;
    int num_delta_pocs;
    uint8_t delta_rps_sign;
    int abs_delta_rps;
    int delta_poc;
} ShortTermRPS;

/**
 * 7.4.2.1
 */
#define MAX_SUB_LAYERS 7
#define MAX_VPS_COUNT 16
#define MAX_SPS_COUNT 32
#define MAX_PPS_COUNT 256
#define MAX_SHORT_TERM_RPS_COUNT 64
#define MAX_CU_SIZE 128

//TODO: check if this is really the maximum
#define MAX_TRANSFORM_DEPTH 3

#define MAX_TB_SIZE 32
#define MAX_PB_SIZE 64
#define MAX_CTB_SIZE 64

#define HEVC_CONTEXTS 183

typedef struct PTL {
    int general_profile_space;
    uint8_t general_tier_flag;
    int general_profile_idc;
    int general_profile_compatibility_flag[32];
    int general_level_idc;

    uint8_t sub_layer_profile_present_flag[MAX_SUB_LAYERS];
    uint8_t sub_layer_level_present_flag[MAX_SUB_LAYERS];

    int sub_layer_profile_space[MAX_SUB_LAYERS];
    uint8_t sub_layer_tier_flag[MAX_SUB_LAYERS];
    int sub_layer_profile_idc[MAX_SUB_LAYERS];
    uint8_t sub_layer_profile_compatibility_flags[MAX_SUB_LAYERS][32];
    int sub_layer_level_idc[MAX_SUB_LAYERS];
} PTL;

typedef struct VPS {
    uint8_t vps_temporal_id_nesting_flag;
    int vps_max_sub_layers; ///< vps_max_temporal_layers_minus1 + 1

    PTL ptl;
    int vps_sub_layer_ordering_info_present_flag;
    int vps_max_dec_pic_buffering[MAX_SUB_LAYERS];
    int vps_num_reorder_pics[MAX_SUB_LAYERS];
    int vps_max_latency_increase[MAX_SUB_LAYERS];
    int vps_max_nuh_reserved_zero_layer_id;
    int vps_max_op_sets; ///< vps_max_op_sets_minus1 + 1
    uint8_t vps_timing_info_present_flag;
    uint32_t vps_num_units_in_tick;
    uint32_t vps_time_scale;
    uint8_t vps_poc_proportional_to_timing_flag;
    int vps_num_ticks_poc_diff_one; ///< vps_num_ticks_poc_diff_one_minus1 + 1
    int vps_num_hrd_parameters;
} VPS;

typedef struct SPS {
    int vps_id;

    int sps_max_sub_layers; ///< sps_max_sub_layers_minus1 + 1

    PTL ptl;

    int chroma_format_idc;
    uint8_t separate_colour_plane_flag;

    int pic_width_in_luma_samples;
    int pic_height_in_luma_samples;

    uint8_t pic_cropping_flag;
    struct {
        int left_offset;
        int right_offset;
        int top_offset;
        int bottom_offset;
    } pic_crop;
    
    int bit_depth; ///< bit_depth_luma_minus8 + 8

    int pcm_enabled_flag;
    struct {
        uint8_t bit_depth_luma; ///< pcm_bit_depth_luma_minus1 + 1
        uint8_t bit_depth_chroma; ///< pcm_bit_depth_chroma_minus1 + 1

        int log2_min_pcm_coding_block_size; ///< log2_min_pcm_coding_block_size_minus3 + 3
        int log2_diff_max_min_pcm_coding_block_size;

        uint8_t loop_filter_disable_flag;
    } pcm;

    int log2_max_poc_lsb; ///< log2_max_pic_order_cnt_lsb_minus4 + 4
    uint8_t sps_sub_layer_ordering_info_present_flag;
    struct {
        int max_dec_pic_buffering;
        int num_reorder_pics;
        int max_latency_increase;
    } temporal_layer[MAX_SUB_LAYERS];

    uint8_t restricted_ref_pic_lists_flag;

    int log2_min_coding_block_size; ///< log2_min_coding_block_size_minus3 + 3
    int log2_diff_max_min_coding_block_size;
    int log2_min_transform_block_size; ///< log2_min_transform_block_size_minus2 + 2
    int log2_diff_max_min_transform_block_size;


    int max_transform_hierarchy_depth_inter;
    int max_transform_hierarchy_depth_intra;

    int scaling_list_enable_flag;

    uint8_t deblocking_filter_in_aps_enabled_flag;

    uint8_t amp_enabled_flag;
    uint8_t sample_adaptive_offset_enabled_flag;

    uint8_t temporal_id_nesting_flag;

    int num_short_term_ref_pic_sets;
    ShortTermRPS short_term_rps_list[MAX_SHORT_TERM_RPS_COUNT];

    uint8_t long_term_ref_pics_present_flag;
    uint8_t sps_temporal_mvp_enabled_flag;
    uint8_t sps_strong_intra_smoothing_enable_flag;

    uint8_t vui_parameters_present_flag;
    uint8_t sps_extension_flag;

    // Inferred parameters
    int log2_ctb_size; ///< Log2CtbSize
    int pic_width_in_ctbs; ///< PicWidthInCtbs
    int pic_height_in_ctbs; ///< PicHeightInCtbs
    int pic_width_in_min_cbs;
    int pic_height_in_min_cbs;
    int pic_width_in_min_tbs;
    int pic_height_in_min_tbs;

    int log2_min_pu_size;

    int hshift[3];
    int vshift[3];

    int pixel_shift;

    int qp_bd_offset; ///< QpBdOffsetY
} SPS;

typedef struct PPS {
    int sps_id; ///< seq_parameter_set_id

    uint8_t sign_data_hiding_flag;

    uint8_t cabac_init_present_flag;

    int num_ref_idx_l0_default_active; ///< num_ref_idx_l0_default_active_minus1 + 1
    int num_ref_idx_l1_default_active; ///< num_ref_idx_l1_default_active_minus1 + 1
    int pic_init_qp_minus26;

    uint8_t constrained_intra_pred_flag;
    uint8_t transform_skip_enabled_flag;

    uint8_t cu_qp_delta_enabled_flag;
    int diff_cu_qp_delta_depth;

    int cb_qp_offset;
    int cr_qp_offset;
    uint8_t pic_slice_level_chroma_qp_offsets_present_flag;
    uint8_t weighted_pred_flag;
    uint8_t weighted_bipred_flag;
    uint8_t output_flag_present_flag;
    uint8_t transquant_bypass_enable_flag;

    uint8_t dependent_slice_segments_enabled_flag;
    uint8_t tiles_enabled_flag;
    uint8_t entropy_coding_sync_enabled_flag;

    int num_tile_columns; ///< num_tile_columns_minus1 + 1
    int num_tile_rows; ///< num_tile_rows_minus1 + 1
    uint8_t uniform_spacing_flag;
    uint8_t loop_filter_across_tiles_enabled_flag;

    uint8_t seq_loop_filter_across_slices_enabled_flag;

    uint8_t deblocking_filter_control_present_flag;
    uint8_t deblocking_filter_override_enabled_flag;
    uint8_t pps_disable_deblocking_filter_flag;
    int beta_offset; ///< beta_offset_div2 * 2
    int tc_offset; ///< tc_offset_div2 * 2

    int pps_scaling_list_data_present_flag;

    uint8_t lists_modification_present_flag;
    int log2_parallel_merge_level; ///< log2_parallel_merge_level_minus2 + 2
    int num_extra_slice_header_bits;
    uint8_t slice_header_extension_present_flag;

    uint8_t pps_extension_flag;
    uint8_t pps_extension_data_flag;

    // Inferred parameters
    int *column_width; ///< ColumnWidth
    int *row_height; ///< RowHeight
    int *col_bd; ///< ColBd
    int *row_bd; ///< RowBd

    int *ctb_addr_rs_to_ts; ///< CtbAddrRSToTS
    int *ctb_addr_ts_to_rs; ///< CtbAddrTSToRS
    int *tile_id; ///< TileId
    int *min_cb_addr_zs; ///< MinCbAddrZS
    int *min_tb_addr_zs; ///< MinTbAddrZS
} PPS;

enum SliceType {
    B_SLICE = 0,
    P_SLICE = 1,
    I_SLICE = 2
};

typedef struct SliceHeader {
    uint8_t first_slice_in_pic_flag;
    int slice_address;

    enum SliceType slice_type;

    uint8_t dependent_slice_segment_flag;
    int pps_id; ///< pic_parameter_set_id
    uint8_t pic_output_flag;
    uint8_t colour_plane_id;

    int pic_order_cnt_lsb;

    uint8_t no_output_of_prior_pics_flag;

    uint8_t slice_sample_adaptive_offset_flag[3];

    uint8_t slice_temporal_mvp_enable_flag;
    uint8_t num_ref_idx_active_override_flag;
    int num_ref_idx_l0_active;
    int num_ref_idx_l1_active;

    uint8_t mvd_l1_zero_flag;
    uint8_t cabac_init_flag;
    uint8_t collocated_from_l0_flag;
    int collocated_ref_idx;
    int slice_qp_delta;
    int slice_cb_qp_offset;
    int slice_cr_qp_offset;

    uint8_t disable_deblocking_filter_flag; ///< slice_header_disable_deblocking_filter_flag
    int beta_offset; ///< beta_offset_div2 * 2
    int tc_offset; ///< tc_offset_div2 * 2

    int max_num_merge_cand; ///< 5 - 5_minus_max_num_merge_cand

    uint8_t slice_loop_filter_across_slices_enabled_flag;

#if REFERENCE_ENCODER_QUIRKS
    uint8_t tile_marker_flag;
#endif

    // Inferred parameters
    uint8_t slice_qp; ///< SliceQP
    int slice_ctb_addr_rs; ///< SliceCtbAddrRS
    int slice_cb_addr_zs; ///< SliceCbAddrZS
} SliceHeader;

enum SyntaxElement {
    SAO_MERGE_FLAG = 0,
    SAO_TYPE_IDX,
    SAO_EO_CLASS,
    SAO_BAND_POSITION,
    SAO_OFFSET_ABS,
    SAO_OFFSET_SIGN,
    END_OF_SLICE_FLAG,
    SPLIT_CODING_UNIT_FLAG,
    CU_TRANSQUANT_BYPASS_FLAG,
    SKIP_FLAG,
    CU_QP_DELTA,
    PRED_MODE_FLAG,
    PART_MODE,
    PCM_FLAG,
    PREV_INTRA_LUMA_PRED_FLAG,
    MPM_IDX,
    REM_INTRA_LUMA_PRED_MODE,
    INTRA_CHROMA_PRED_MODE,
    MERGE_FLAG,
    MERGE_IDX,
    INTER_PRED_IDC,
    REF_IDX_L0,
    REF_IDX_L1,
    ABS_MVD_GREATER0_FLAG,
    ABS_MVD_GREATER1_FLAG,
    ABS_MVD_MINUS2,
    MVD_SIGN_FLAG,
    MVP_LX_FLAG,
    NO_RESIDUAL_DATA_FLAG,
    SPLIT_TRANSFORM_FLAG,
    CBF_LUMA,
    CBF_CB_CR,
    TRANSFORM_SKIP_FLAG,
    LAST_SIGNIFICANT_COEFF_X_PREFIX,
    LAST_SIGNIFICANT_COEFF_Y_PREFIX,
    LAST_SIGNIFICANT_COEFF_X_SUFFIX,
    LAST_SIGNIFICANT_COEFF_Y_SUFFIX,
    SIGNIFICANT_COEFF_GROUP_FLAG,
    SIGNIFICANT_COEFF_FLAG,
    COEFF_ABS_LEVEL_GREATER1_FLAG,
    COEFF_ABS_LEVEL_GREATER2_FLAG,
    COEFF_ABS_LEVEL_REMAINING,
    COEFF_SIGN_FLAG
};

enum PartMode {
    PART_2Nx2N = 0,
    PART_2NxN = 1,
    PART_Nx2N = 2,
    PART_NxN = 3,
    PART_2NxnU = 4,
    PART_2NxnD = 5,
    PART_nLx2N = 6,
    PART_nRx2N = 7
};

enum PredMode {
    MODE_INTER = 0,
    MODE_INTRA,
    MODE_SKIP
};

enum InterPredIdc {
    PRED_L0 = 0,
    PRED_L1,
    PRED_BI
};

typedef struct CodingTree {
    int depth; ///< ctDepth
} CodingTree;

typedef struct CodingUnit {
    uint8_t cu_transquant_bypass_flag;
    uint8_t *skip_flag;
    enum PredMode pred_mode; ///< PredMode
    enum PartMode part_mode; ///< PartMode
    uint8_t no_residual_data_flag;

    uint8_t pcm_flag;

    // Inferred parameters
    uint8_t intra_split_flag; ///< IntraSplitFlag
    uint8_t max_trafo_depth; ///< MaxTrafoDepth

    int x;
    int y;
    uint8_t *top_ct_depth;
    uint8_t *left_ct_depth;
} CodingUnit;

enum IntraPredMode {
    INTRA_PLANAR = 0,
    INTRA_DC,
    INTRA_ANGULAR_2,
    INTRA_ANGULAR_3,
    INTRA_ANGULAR_4,
    INTRA_ANGULAR_5,
    INTRA_ANGULAR_6,
    INTRA_ANGULAR_7,
    INTRA_ANGULAR_8,
    INTRA_ANGULAR_9,
    INTRA_ANGULAR_10,
    INTRA_ANGULAR_11,
    INTRA_ANGULAR_12,
    INTRA_ANGULAR_13,
    INTRA_ANGULAR_14,
    INTRA_ANGULAR_15,
    INTRA_ANGULAR_16,
    INTRA_ANGULAR_17,
    INTRA_ANGULAR_18,
    INTRA_ANGULAR_19,
    INTRA_ANGULAR_20,
    INTRA_ANGULAR_21,
    INTRA_ANGULAR_22,
    INTRA_ANGULAR_23,
    INTRA_ANGULAR_24,
    INTRA_ANGULAR_25,
    INTRA_ANGULAR_26,
    INTRA_ANGULAR_27,
    INTRA_ANGULAR_28,
    INTRA_ANGULAR_29,
    INTRA_ANGULAR_30,
    INTRA_ANGULAR_31,
    INTRA_ANGULAR_32,
    INTRA_ANGULAR_33,
    INTRA_ANGULAR_34
};

typedef struct Mv {
    int16_t m_iHor;     ///< horizontal component of motion vector
    int16_t m_iVer;     ///< vertical component of motion vector
} Mv;

typedef struct MvField {
      Mv   acMv;
      int  RefIdx;
      int predFlag;
      int isIntra;
} MvField;

// MERGE
#define MRG_MAX_NUM_CANDS     5

typedef struct PredictionUnit {
    uint8_t merge_flag;

    int mpm_idx;
    int rem_intra_luma_pred_mode;

    uint8_t intra_pred_mode[4];
    uint8_t intra_pred_mode_c;

    uint8_t *top_ipm;
    uint8_t *left_ipm;

    MvField *tab_mvf;
} PredictionUnit;

typedef struct TransformTree {
    uint8_t *cbf_cb[MAX_TRANSFORM_DEPTH];
    uint8_t *cbf_cr[MAX_TRANSFORM_DEPTH];
    uint8_t cbf_luma;

    // Inferred parameters
    uint8_t inter_split_flag;
} TransformTree;

typedef struct TransformUnit {
    int cu_qp_delta;

    // Inferred parameters;
    uint8_t is_cu_qp_delta_coded;
    int cur_intra_pred_mode;
} TransformUnit;

typedef struct ResidualCoding {
    uint8_t significant_coeff_group_flag[8][8];
} ResidualCoding;

enum SAOType {
    SAO_NOT_APPLIED = 0,
    SAO_BAND,
    SAO_EDGE
};

enum SAOEOClass {
    SAO_EO_HORIZ = 0,
    SAO_EO_VERT,
    SAO_EO_135D,
    SAO_EO_45D
};

typedef struct SAOParams {
    uint8_t type_idx[3]; ///< sao_type_idx

    int offset_abs[3][4]; ///< sao_offset_abs
    int offset_sign[3][4]; ///< sao_offset_sign

    int band_position[3]; ///< sao_band_position

    int eo_class[3]; ///< sao_eo_class

    // Inferred parameters
    int offset_val[3][5]; ///<SaoOffsetVal
} SAOParams;

typedef struct HEVCContext {
    AVCodecContext *avctx;
    AVFrame frame;
    AVFrame sao_frame;

    HEVCPredContext hpc;
    HEVCDSPContext hevcdsp;
    DSPContext dsp;

    GetBitContext gb;
    CABACContext cc;

    uint8_t cabac_state[HEVC_CONTEXTS];

    int nal_ref_flag;
    enum NALUnitType nal_unit_type;
    int temporal_id;  ///< temporal_id_plus1 - 1

    VPS *vps_list[MAX_VPS_COUNT];
    SPS *sps_list[MAX_SPS_COUNT];
    PPS *pps_list[MAX_PPS_COUNT];

    VPS *vps;
    SPS *sps;
    PPS *pps;

    SliceHeader sh;
    SAOParams *sao;

    int ctb_addr_in_slice; ///< CtbAddrInSlice
    int num_pcm_block; ///< NumPCMBlock

    int ctb_addr_rs; ///< CtbAddrRS
    int ctb_addr_ts; ///< CtbAddrTS

    uint8_t *split_coding_unit_flag;

    uint8_t *edge_emu_buffer;

    CodingTree ct;
    CodingUnit cu;
    PredictionUnit pu;
    TransformTree tt;
    TransformUnit tu;
    ResidualCoding rc;
    int poc;
} HEVCContext;

enum ScanType {
    SCAN_DIAG = 0,
    SCAN_HORIZ,
    SCAN_VERT
};

int ff_hevc_decode_short_term_rps(HEVCContext *s, int idx, SPS *sps);
int ff_hevc_decode_nal_vps(HEVCContext *s);
int ff_hevc_decode_nal_sps(HEVCContext *s);
int ff_hevc_decode_nal_pps(HEVCContext *s);
int ff_hevc_decode_nal_sei(HEVCContext *s);

void ff_hevc_cabac_init(HEVCContext *s);
int ff_hevc_sao_merge_flag_decode(HEVCContext *s);
int ff_hevc_sao_type_idx_decode(HEVCContext *s);
int ff_hevc_sao_band_position_decode(HEVCContext *s);
int ff_hevc_sao_offset_abs_decode(HEVCContext *s);
int ff_hevc_sao_offset_sign_decode(HEVCContext *s);
int ff_hevc_sao_eo_class_decode(HEVCContext *s);
int ff_hevc_end_of_slice_flag_decode(HEVCContext *s);
int ff_hevc_cu_transquant_bypass_flag_decode(HEVCContext *s);
int ff_hevc_skip_flag_decode(HEVCContext *s, int x_cb, int y_cb);
int ff_hevc_pred_mode_decode(HEVCContext *s);
int ff_hevc_split_coding_unit_flag_decode(HEVCContext *s, int ct_depth, int x0, int y0);
int ff_hevc_part_mode_decode(HEVCContext *s, int log2_cb_size);
int ff_hevc_pcm_flag_decode(HEVCContext *s);
int ff_hevc_prev_intra_luma_pred_flag_decode(HEVCContext *s);
int ff_hevc_mpm_idx_decode(HEVCContext *s);
int ff_hevc_rem_intra_luma_pred_mode_decode(HEVCContext *s);
int ff_hevc_intra_chroma_pred_mode_decode(HEVCContext *s);
int ff_hevc_merge_idx_decode(HEVCContext *s);
int ff_hevc_merge_flag_decode(HEVCContext *s);
int ff_hevc_inter_pred_idc_decode(HEVCContext *s, int nPbW, int nPbH);
int ff_hevc_ref_idx_lx_decode(HEVCContext *s, int num_ref_idx_lx);
int ff_hevc_mvp_lx_flag_decode(HEVCContext *s);
int ff_hevc_no_residual_syntax_flag_decode(HEVCContext *s);
int ff_hevc_abs_mvd_greater0_flag_decode(HEVCContext *s);
int ff_hevc_abs_mvd_greater1_flag_decode(HEVCContext *s);
int ff_hevc_abs_mvd_minus2_decode(HEVCContext *s);
int ff_hevc_mvd_sign_flag_decode(HEVCContext *s);
int ff_hevc_split_transform_flag_decode(HEVCContext *s, int log2_trafo_size);
int ff_hevc_cbf_cb_cr_decode(HEVCContext *s, int trafo_depth);
int ff_hevc_cbf_luma_decode(HEVCContext *s, int trafo_depth);
int ff_hevc_transform_skip_flag_decode(HEVCContext *s, int c_idx);
int ff_hevc_last_significant_coeff_x_prefix_decode(HEVCContext *s, int c_idx,
                                                   int log2_size);
int ff_hevc_last_significant_coeff_y_prefix_decode(HEVCContext *s, int c_idx,
                                                   int log2_size);
int ff_hevc_last_significant_coeff_suffix_decode(HEVCContext *s,
                                                 int last_significant_coeff_prefix);
int ff_hevc_significant_coeff_group_flag_decode(HEVCContext *s, int c_idx, int x_cg,
                                                int y_cg, int log2_trafo_size);
int ff_hevc_significant_coeff_flag_decode(HEVCContext *s, int c_idx, int x_c, int y_c,
                                          int log2_trafo_size, int scan_idx);
int ff_hevc_coeff_abs_level_greater1_flag_decode(HEVCContext *s, int c_idx,
                                                 int i, int n,
                                                 int first_greater1_coeff_idx,
                                                 int first_subset);
int ff_hevc_coeff_abs_level_greater2_flag_decode(HEVCContext *s, int c_idx,
                                                 int i, int n);
int ff_hevc_coeff_abs_level_remaining(HEVCContext *s, int n, int base_level);
int ff_hevc_coeff_sign_flag(HEVCContext *s, uint8_t nb);

#endif // AVCODEC_HEVC_H
