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
    RK_S32              delta_qp_min;
    RK_S32              delta_qp_max;
    RK_U32              enable_bg_compensation;
    RK_S32              bg_delta_qp_max;
    RK_U32              dump_qpmap_debug;
    const char          *debug_dir;
} MppEncQpmapRoiCfg;

MPP_RET mpp_enc_qpmap_roi_init(MppEncQpmapRoiCtx *ctx, RK_U32 w, RK_U32 h,
                               MppCodingType type, const MppEncQpmapRoiCfg *cfg);
MPP_RET mpp_enc_qpmap_roi_deinit(MppEncQpmapRoiCtx ctx);
MPP_RET mpp_enc_qpmap_roi_setup_meta(MppEncQpmapRoiCtx ctx, MppMeta meta,
                                     RK_S32 frame_idx);

#endif /* MPP_ENC_QPMAP_ROI_UTILS_H */
