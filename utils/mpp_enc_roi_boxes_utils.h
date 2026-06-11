/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/*
 * Box-JSON driven ROI region adapter for vepu580 (RK3588).
 *
 * Unlike mpp_enc_qpmap_roi_utils.c which builds a vepu541-only KEY_QPMAP0,
 * this adapter feeds detected boxes as relative-QP regions into the standard
 * mpp_enc_roi (MppEncROICfg2 / KEY_ROI_DATA2) path, which IS supported on
 * RK3588 / vepu580.
 *
 * JSONL box format (same as QPMAP_ROI_README):
 *   {"frame_idx": 0, "boxes": [{"type":"face","x1":..,"y1":..,"x2":..,"y2":..}]}
 */

#ifndef MPP_ENC_ROI_BOXES_UTILS_H
#define MPP_ENC_ROI_BOXES_UTILS_H

#include "rk_mpi.h"
#include "mpp_enc_roi_utils.h"

typedef struct MppEncRoiBoxesImpl_t *MppEncRoiBoxesCtx;

#ifdef __cplusplus
extern "C" {
#endif

MPP_RET mpp_enc_roi_boxes_init(MppEncRoiBoxesCtx *ctx, const char *json_file);
MPP_RET mpp_enc_roi_boxes_deinit(MppEncRoiBoxesCtx ctx);

/*
 * Add this frame's boxes into roi_ctx as ROI regions.
 * face_abs_qp >= 0: absolute QP for face boxes (qp_mode=1).
 * otherwise face_delta / plate_delta are relative adjustments (qp_mode=0).
 * face_expand_blocks expands each face box by N 16x16 blocks per side.
 * Returns number of regions added (>=0), or negative on error.
 */
RK_S32 mpp_enc_roi_boxes_apply(MppEncRoiBoxesCtx ctx, MppEncRoiCtx roi_ctx,
                               RK_S32 frame_idx, RK_U32 width, RK_U32 height,
                               RK_S32 face_delta, RK_S32 plate_delta,
                               RK_S32 face_expand_blocks, RK_S32 face_abs_qp);

#ifdef __cplusplus
}
#endif

#endif /* MPP_ENC_ROI_BOXES_UTILS_H */
