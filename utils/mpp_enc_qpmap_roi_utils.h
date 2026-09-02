/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef MPP_ENC_QPMAP_ROI_UTILS_H
#define MPP_ENC_QPMAP_ROI_UTILS_H

#include "rk_mpi.h"

typedef struct MppEncQpmapRoiImpl_t *MppEncQpmapRoiCtx;

typedef struct MppEncQpmapRoiCfg_t {
    RK_U32              enable;
    const char          *boxes_file;
    RK_S32              face_delta_qp;
    RK_S32              face_expand_blocks;
    RK_S32              plate_delta_qp;
    RK_S32              person_delta_qp;
    RK_S32              vehicle_delta_qp;
    RK_S32              nonmotor_delta_qp;
    RK_S32              delta_qp_min;
    RK_S32              delta_qp_max;
    RK_U32              enable_bg_compensation;
    RK_S32              bg_delta_qp_max;
    RK_S32              smooth_radius;
    RK_U32              dump_qpmap_debug;
    const char          *debug_dir;
    RK_U32              enable_external_map;
} MppEncQpmapRoiCfg;

MPP_RET mpp_enc_qpmap_roi_init(MppEncQpmapRoiCtx *ctx, RK_U32 w, RK_U32 h,
                               MppCodingType type, const MppEncQpmapRoiCfg *cfg);
MPP_RET mpp_enc_qpmap_roi_deinit(MppEncQpmapRoiCtx ctx);
/*
 * Set a dense 16x16 relative-QP base map for the next setup_meta call.
 * Detection boxes are applied afterwards and take priority. protect_map marks
 * stable high-structure blocks for ROI accounting and boundary smoothing.
 */
MPP_RET mpp_enc_qpmap_roi_set_external_map(MppEncQpmapRoiCtx ctx,
                                           const RK_S16 *delta_qp_map,
                                           const RK_U8 *protect_map,
                                           RK_U32 map_w, RK_U32 map_h,
                                           RK_U32 map_stride);
MPP_RET mpp_enc_qpmap_roi_setup_meta(MppEncQpmapRoiCtx ctx, MppMeta meta,
                                     RK_S32 frame_idx);

#endif /* MPP_ENC_QPMAP_ROI_UTILS_H */
