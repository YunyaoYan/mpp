/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MPP_ENC_BG_FILTER_UTILS_H
#define MPP_ENC_BG_FILTER_UTILS_H

#include "rk_mpi.h"

typedef struct MppEncBgFilterImpl_t *MppEncBgFilterCtx;

typedef struct MppEncBgFilterCfg_t {
    RK_U32              enable;
    RK_S32              filter_type;          /* 0: gaussian, 1: bilateral (reserved) */
    RK_S32              low_light_thr;       /* mean Y < thr: enable weak filter */
    RK_S32              strong_light_thr;   /* mean Y < thr: enable strong filter */
    RK_S32              kernel_weak;        /* weak filter kernel size, e.g. 3 */
    RK_S32              kernel_strong;      /* strong filter kernel size, e.g. 5 */
    RK_S32              roi_expand_face;    /* face box expand ratio * 100, e.g. 15 = 0.15 */
    RK_S32              roi_expand_plate;   /* plate box expand ratio * 100 */
    RK_S32              roi_expand_default; /* other box expand ratio * 100 */
    RK_S32              roi_mask_dilate_iter;
    RK_U32              enable_temporal;    /* temporal blend enable */
    RK_S32              temporal_alpha;     /* current frame weight * 100, e.g. 80 = 0.8 */
    RK_S32              motion_diff_thr;    /* motion gate threshold */
    RK_U32              dump_debug;         /* dump debug images */
    const char          *debug_dir;          /* debug output directory */
    const char          *boxes_file;        /* ROI boxes JSON/JSONL file */
} MppEncBgFilterCfg;

MPP_RET mpp_enc_bg_filter_init(MppEncBgFilterCtx *ctx, RK_U32 w, RK_U32 h,
                               MppFrameFormat fmt, const MppEncBgFilterCfg *cfg);
MPP_RET mpp_enc_bg_filter_deinit(MppEncBgFilterCtx ctx);

/*
 * Process one frame. Input and output are both raw buffer pointers.
 * The caller must ensure buf and out_buf have enough size.
 * If filtering is skipped (not low-light), out_buf is filled with original data.
 * Returns MPP_OK if output is ready.
 */
MPP_RET mpp_enc_bg_filter_process(MppEncBgFilterCtx ctx, RK_U8 *buf, RK_U8 *out_buf,
                                  RK_S32 frame_idx, RK_S32 hor_stride, RK_S32 ver_stride);

/*
 * Query whether the last processed frame was filtered.
 * Returns 1 if filtered, 0 if skipped, -1 if error.
 */
RK_S32 mpp_enc_bg_filter_last_filtered(MppEncBgFilterCtx ctx);

#endif /* MPP_ENC_BG_FILTER_UTILS_H */
