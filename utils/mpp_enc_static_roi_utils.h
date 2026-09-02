/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef MPP_ENC_STATIC_ROI_UTILS_H
#define MPP_ENC_STATIC_ROI_UTILS_H

#include "rk_mpi.h"

typedef struct MppEncStaticRoiImpl_t *MppEncStaticRoiCtx;

typedef struct MppEncStaticRoiCfg_t {
    RK_U32              enable;
    RK_S32              sample_step;          /* luma sampling step, normally 2 */
    RK_S32              motion_mad_thr;       /* mean absolute luma difference */
    RK_S32              edge_pixel_thr;       /* sampled gradient threshold */
    RK_S32              edge_density_thr;     /* structure edge density, percent */
    RK_S32              stable_frames;        /* frames required before classification */
    RK_S32              structure_hold_frames;/* retain structure protection over changes */
    RK_S32              structure_delta_qp;   /* stable high-structure relative QP */
    RK_S32              flat_delta_qp;        /* stable flat relative QP */
    RK_U32              dump_debug;
    const char          *debug_dir;
} MppEncStaticRoiCfg;

#ifdef __cplusplus
extern "C" {
#endif

MPP_RET mpp_enc_static_roi_init(MppEncStaticRoiCtx *ctx, RK_U32 width,
                                RK_U32 height, MppFrameFormat fmt,
                                const MppEncStaticRoiCfg *cfg);
MPP_RET mpp_enc_static_roi_deinit(MppEncStaticRoiCtx ctx);

/*
 * Analyse an 8-bit YUV frame before it is submitted to the encoder.
 * Only the luma plane is read. hor_stride is expressed in luma bytes/pixels.
 */
MPP_RET mpp_enc_static_roi_process(MppEncStaticRoiCtx ctx, const RK_U8 *luma,
                                   RK_U32 hor_stride, RK_S32 frame_idx);

/*
 * Return the current 16x16 relative-QP map. The pointers remain owned by ctx.
 * protect_map is one for stable high-structure blocks and zero otherwise.
 */
MPP_RET mpp_enc_static_roi_get_map(MppEncStaticRoiCtx ctx,
                                   const RK_S16 **delta_qp_map,
                                   const RK_U8 **protect_map,
                                   RK_U32 *mb_w, RK_U32 *mb_h,
                                   RK_U32 *map_stride);

#ifdef __cplusplus
}
#endif

#endif /* MPP_ENC_STATIC_ROI_UTILS_H */
