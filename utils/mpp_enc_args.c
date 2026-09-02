/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#include "mpp_mem.h"
#include "mpp_cfg_io.h"

#include "iniparser.h"
#include "utils_singleton.h"
#include "mpi_enc_utils.h"
#include "mpp_enc_args.h"

static rk_s32 mpp_enc_args_impl_init(void *entry, KmppObj obj, const char *caller)
{
    MpiEncTestArgs *args = (MpiEncTestArgs *)entry;

    args->nthreads = 1;
    args->frm_step = 1;
    args->rc_mode = MPP_ENC_RC_MODE_BUTT;
    args->face_delta_qp = -3;
    args->face_expand_blocks = 1;
    args->face_abs_qp = -1;
    args->plate_delta_qp = -6;
    args->person_delta_qp = -4;
    args->vehicle_delta_qp = -4;
    args->nonmotor_delta_qp = -4;
    args->delta_qp_min = -8;
    args->delta_qp_max = 4;
    args->enable_bg_compensation = 1;
    args->bg_delta_qp_max = 4;
    args->qpmap_smooth_radius = 2;

    /* Static-structure ROI defaults: conservative, no spatial filtering. */
    args->enable_static_roi = 0;
    args->static_roi_sample_step = 2;
    args->static_roi_mad_thr = 4;
    args->static_roi_edge_thr = 24;
    args->static_roi_edge_density = 8;
    args->static_roi_stable_frames = 8;
    args->static_roi_hold_frames = 12;
    args->static_roi_structure_delta_qp = -4;
    args->static_roi_flat_delta_qp = 4;

    /* ROI-protected background filter defaults */
    args->enable_bg_filter = 0;
    args->bg_filter_type = 0;               /* gaussian */
    args->bg_filter_low_light_thr = 80;
    args->bg_filter_strong_light_thr = 50;
    args->bg_filter_kernel_weak = 3;
    args->bg_filter_kernel_strong = 5;
    args->roi_expand_face = 15;             /* 0.15 * 100 */
    args->roi_expand_plate = 20;            /* 0.20 * 100 */
    args->roi_expand_default = 10;          /* 0.10 * 100 */
    args->roi_mask_dilate_iter = 1;
    args->enable_temporal_bg_filter = 0;
    args->temporal_alpha = 80;              /* 0.8 * 100 */
    args->motion_diff_thr = 12;

    (void) obj;
    (void) caller;

    return rk_ok;
}

static rk_s32 mpp_enc_args_impl_deinit(void *entry, KmppObj obj, const char *caller)
{
    MpiEncTestArgs *args = (MpiEncTestArgs *)entry;

    if (args->cfg_ini) {
        iniparser_freedict(args->cfg_ini);
        args->cfg_ini = NULL;
    }

    if (args->fps) {
        fps_calc_deinit(args->fps);
        args->fps = NULL;
    }

    MPP_FREE(args->file_input);
    MPP_FREE(args->file_output);
    MPP_FREE(args->file_cfg);
    MPP_FREE(args->file_slt);
    MPP_FREE(args->roi_boxes_json);
    MPP_FREE(args->qpmap_debug_dir);
    MPP_FREE(args->static_roi_debug_dir);
    MPP_FREE(args->bg_filter_debug_dir);
    MPP_FREE(args->bg_filter_boxes_json);

    (void) obj;
    (void) caller;

    return rk_ok;
}

static rk_s32 mpp_enc_args_impl_dump(void *entry)
{
    MpiEncTestArgs *args = (MpiEncTestArgs *)entry;

    mpp_logi("cmd parse result:\n");
    mpp_logi("input  file name: %s\n", args->file_input);
    mpp_logi("output file name: %s\n", args->file_output);
    mpp_logi("width      : %d\n", args->width);
    mpp_logi("height     : %d\n", args->height);
    mpp_logi("format     : %d\n", args->format);
    mpp_logi("type       : %d\n", args->type);
    if (args->file_slt) {
        mpp_logi("verify     : %s\n", args->file_slt);
        mpp_logi("frame step : %d\n", args->frm_step);
    }

    return rk_ok;
}

#define MPP_ENC_ARGS_ENTRY_TABLE(prefix, ENTRY, STRCT, EHOOK, SHOOK, ALIAS) \
    CFG_DEF_START() \
    ENTRY(prefix, s32,  rk_s32,     type_src,         FLAG_NONE,       type_src) \
    ENTRY(prefix, s32,  rk_s32,     frame_num,        FLAG_NONE,       frame_num) \
    ENTRY(prefix, s32,  rk_s32,     loop_cnt,         FLAG_NONE,       loop_cnt) \
    ENTRY(prefix, s32,  rk_s32,     nthreads,         FLAG_NONE,       nthreads) \
    ENTRY(prefix, s32,  rk_s32,     frm_step,         FLAG_NONE,       frm_step) \
    ENTRY(prefix, s32,  rk_s32,     gop_mode,         FLAG_NONE,       gop_mode) \
    ENTRY(prefix, s32,  rk_s32,     vi_len,           FLAG_NONE,       vi_len) \
    ENTRY(prefix, s32,  rk_s32,     quiet,            FLAG_NONE,       quiet) \
    ENTRY(prefix, s32,  rk_s32,     trace_fps,        FLAG_NONE,       trace_fps) \
    ENTRY(prefix, u32,  rk_u32,     kmpp_en,          FLAG_NONE,       kmpp_en) \
    ENTRY(prefix, u32,  rk_u32,     osd_enable,       FLAG_NONE,       osd_enable) \
    ENTRY(prefix, u32,  rk_u32,     osd_mode,         FLAG_NONE,       osd_mode) \
    ENTRY(prefix, u32,  rk_u32,     user_data_enable, FLAG_NONE,       user_data_enable) \
    ENTRY(prefix, u32,  rk_u32,     roi_enable,       FLAG_NONE,       roi_enable) \
    ENTRY(prefix, u32,  rk_u32,     roi_jpeg_enable,  FLAG_NONE,       roi_jpeg_enable) \
    ENTRY(prefix, u32,  rk_u32,     enable_qpmap_roi, FLAG_NONE,       enable_qpmap_roi) \
    ENTRY(prefix, s32,  rk_s32,     face_delta_qp,    FLAG_NONE,       face_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     face_expand_blocks, FLAG_NONE,     face_expand_blocks) \
    ENTRY(prefix, s32,  rk_s32,     face_abs_qp,      FLAG_NONE,       face_abs_qp) \
    ENTRY(prefix, s32,  rk_s32,     plate_delta_qp,   FLAG_NONE,       plate_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     person_delta_qp,  FLAG_NONE,       person_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     vehicle_delta_qp, FLAG_NONE,       vehicle_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     nonmotor_delta_qp, FLAG_NONE,      nonmotor_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     delta_qp_min,     FLAG_NONE,       delta_qp_min) \
    ENTRY(prefix, s32,  rk_s32,     delta_qp_max,     FLAG_NONE,       delta_qp_max) \
    ENTRY(prefix, u32,  rk_u32,     enable_bg_compensation, FLAG_NONE, enable_bg_compensation) \
    ENTRY(prefix, s32,  rk_s32,     bg_delta_qp_max,  FLAG_NONE,       bg_delta_qp_max) \
    ENTRY(prefix, s32,  rk_s32,     qpmap_smooth_radius, FLAG_NONE,    qpmap_smooth_radius) \
    ENTRY(prefix, u32,  rk_u32,     dump_qpmap_debug, FLAG_NONE,       dump_qpmap_debug) \
    ENTRY(prefix, u32,  rk_u32,     enable_static_roi, FLAG_NONE,      enable_static_roi) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_sample_step, FLAG_NONE, static_roi_sample_step) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_mad_thr, FLAG_NONE,     static_roi_mad_thr) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_edge_thr, FLAG_NONE,    static_roi_edge_thr) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_edge_density, FLAG_NONE, static_roi_edge_density) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_stable_frames, FLAG_NONE, static_roi_stable_frames) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_hold_frames, FLAG_NONE, static_roi_hold_frames) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_structure_delta_qp, FLAG_NONE, static_roi_structure_delta_qp) \
    ENTRY(prefix, s32,  rk_s32,     static_roi_flat_delta_qp, FLAG_NONE, static_roi_flat_delta_qp) \
    ENTRY(prefix, u32,  rk_u32,     dump_static_roi_debug, FLAG_NONE,  dump_static_roi_debug) \
    ENTRY(prefix, u32,  rk_u32,     jpeg_osd_case,    FLAG_NONE,       jpeg_osd_case) \
    ENTRY(prefix, u32,  rk_u32,     constraint_set,   FLAG_NONE,       constraint_set) \
    ENTRY(prefix, u32,  rk_u32,     sei_mode,         FLAG_NONE,       sei_mode) \
    ENTRY(prefix, u32,  rk_u32,     enable_bg_filter, FLAG_NONE,       enable_bg_filter) \
    ENTRY(prefix, s32,  rk_s32,     bg_filter_type,   FLAG_NONE,       bg_filter_type) \
    ENTRY(prefix, s32,  rk_s32,     bg_filter_low_light_thr, FLAG_NONE,  bg_filter_low_light_thr) \
    ENTRY(prefix, s32,  rk_s32,     bg_filter_strong_light_thr, FLAG_NONE, bg_filter_strong_light_thr) \
    ENTRY(prefix, s32,  rk_s32,     bg_filter_kernel_weak, FLAG_NONE,  bg_filter_kernel_weak) \
    ENTRY(prefix, s32,  rk_s32,     bg_filter_kernel_strong, FLAG_NONE, bg_filter_kernel_strong) \
    ENTRY(prefix, s32,  rk_s32,     roi_expand_face,  FLAG_NONE,       roi_expand_face) \
    ENTRY(prefix, s32,  rk_s32,     roi_expand_plate, FLAG_NONE,       roi_expand_plate) \
    ENTRY(prefix, s32,  rk_s32,     roi_expand_default, FLAG_NONE,     roi_expand_default) \
    ENTRY(prefix, s32,  rk_s32,     roi_mask_dilate_iter, FLAG_NONE,   roi_mask_dilate_iter) \
    ENTRY(prefix, u32,  rk_u32,     enable_temporal_bg_filter, FLAG_NONE, enable_temporal_bg_filter) \
    ENTRY(prefix, s32,  rk_s32,     temporal_alpha,   FLAG_NONE,       temporal_alpha) \
    ENTRY(prefix, s32,  rk_s32,     motion_diff_thr,  FLAG_NONE,       motion_diff_thr) \
    ENTRY(prefix, u32,  rk_u32,     dump_bg_filter_debug, FLAG_NONE,   dump_bg_filter_debug) \
    CFG_DEF_END()

#define KMPP_OBJ_NAME               mpp_enc_args
#define KMPP_OBJ_INTF_TYPE          MppEncArgs
#define KMPP_OBJ_IMPL_TYPE          MpiEncTestArgs
#define KMPP_OBJ_SGLN               UTILS_SINGLETON
#define KMPP_OBJ_SGLN_ID            UTILS_SGLN_ENC_ARGS
#define KMPP_OBJ_FUNC_INIT          mpp_enc_args_impl_init
#define KMPP_OBJ_FUNC_DEINIT        mpp_enc_args_impl_deinit
#define KMPP_OBJ_FUNC_DUMP          mpp_enc_args_impl_dump
#define KMPP_OBJ_ENTRY_TABLE        MPP_ENC_ARGS_ENTRY_TABLE
#define KMPP_OBJ_ACCESS_DISABLE
#define KMPP_OBJ_HIERARCHY_ENABLE
#include "kmpp_obj_helper.h"

rk_s32 mpp_enc_args_extract(MppEncArgs cmd_obj, MppCfgStrFmt fmt, char **buf)
{
    MpiEncTestArgs *cmd = kmpp_obj_to_entry(cmd_obj);
    MppCfgObj obj = NULL;
    MppCfgObj root = NULL;

    root = kmpp_objdef_get_cfg_root(mpp_enc_args_def);

    mpp_cfg_from_struct(&obj, root, cmd);
    if (obj) {
        mpp_cfg_to_string(obj, fmt, buf);
        mpp_cfg_put_all(obj);
    }

    return rk_ok;
}

rk_s32 mpp_enc_args_apply(MppEncArgs cmd_obj, MppCfgStrFmt fmt, char *buf)
{
    MpiEncTestArgs *cmd = kmpp_obj_to_entry(cmd_obj);
    MppCfgObj obj = NULL;
    MppCfgObj root = NULL;

    root = kmpp_objdef_get_cfg_root(mpp_enc_args_def);

    mpp_cfg_from_string(&obj, fmt, buf);
    if (obj) {
        mpp_cfg_to_struct(obj, root, cmd);
        mpp_cfg_put_all(obj);
    }

    return rk_ok;
}
