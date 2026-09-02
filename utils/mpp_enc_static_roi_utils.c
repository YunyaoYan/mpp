/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define MODULE_TAG "enc_static_roi_utils"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "mpp_common.h"
#include "mpp_log.h"
#include "mpp_mem.h"

#include "mpp_enc_static_roi_utils.h"

struct MppEncStaticRoiImpl_t {
    MppEncStaticRoiCfg  cfg;
    RK_U32              width;
    RK_U32              height;
    RK_U32              mb_w;
    RK_U32              mb_h;
    RK_U32              block_count;
    RK_U32              sample_w;
    RK_U32              sample_h;
    RK_U8               *prev_samples;
    RK_U8               *stable_count;
    RK_U8               *structure_hold;
    RK_U8               *edge_ema;
    RK_S16              *delta_qp_map;
    RK_U8               *protect_map;
    RK_U32              has_previous;
};

static RK_S32 static_roi_clamp_s32(RK_S32 value, RK_S32 min, RK_S32 max)
{
    return value < min ? min : (value > max ? max : value);
}

static RK_U32 static_roi_min_u32(RK_U32 a, RK_U32 b)
{
    return a < b ? a : b;
}

static RK_U32 static_roi_abs_diff_u8(RK_U8 a, RK_U8 b)
{
    return a > b ? a - b : b - a;
}

static void static_roi_save_initial_samples(MppEncStaticRoiCtx ctx,
                                            const RK_U8 *luma,
                                            RK_U32 hor_stride)
{
    RK_U32 step = (RK_U32)ctx->cfg.sample_step;
    RK_U32 sy, sx;

    for (sy = 0; sy < ctx->sample_h; sy++) {
        RK_U32 y = static_roi_min_u32(sy * step, ctx->height - 1);
        const RK_U8 *row = luma + y * hor_stride;

        for (sx = 0; sx < ctx->sample_w; sx++) {
            RK_U32 x = static_roi_min_u32(sx * step, ctx->width - 1);
            ctx->prev_samples[sy * ctx->sample_w + sx] = row[x];
        }
    }
}

static MPP_RET static_roi_dump_map(MppEncStaticRoiCtx ctx, RK_S32 frame_idx,
                                   RK_U32 structure_count, RK_U32 flat_count,
                                   RK_U32 neutral_count)
{
    char path[1024];
    FILE *fp;
    RK_U32 i;

    if (!ctx->cfg.dump_debug || !ctx->cfg.debug_dir || !ctx->cfg.debug_dir[0])
        return MPP_OK;

    if (mkdir(ctx->cfg.debug_dir) && errno != EEXIST) {
        mpp_err("static roi: failed to create debug dir %s: %s\n",
                ctx->cfg.debug_dir, strerror(errno));
        return MPP_NOK;
    }

    snprintf(path, sizeof(path), "%s/frame_%06d_static_roi.pgm",
             ctx->cfg.debug_dir, frame_idx);
    fp = fopen(path, "wb");
    if (!fp) {
        mpp_err("static roi: failed to open %s: %s\n", path, strerror(errno));
        return MPP_NOK;
    }

    fprintf(fp, "P5\n%u %u\n255\n", ctx->mb_w, ctx->mb_h);
    for (i = 0; i < ctx->block_count; i++) {
        RK_U8 value = ctx->protect_map[i] ? 240 :
                      (ctx->delta_qp_map[i] > 0 ? 32 : 128);
        fwrite(&value, 1, 1, fp);
    }
    fclose(fp);

    mpp_log("static roi frame %d structure %u flat %u neutral %u map %ux%u\n",
            frame_idx, structure_count, flat_count, neutral_count,
            ctx->mb_w, ctx->mb_h);
    return MPP_OK;
}

MPP_RET mpp_enc_static_roi_init(MppEncStaticRoiCtx *ctx_out, RK_U32 width,
                                RK_U32 height, MppFrameFormat fmt,
                                const MppEncStaticRoiCfg *cfg)
{
    MppEncStaticRoiCtx ctx = NULL;
    MppFrameFormat base_fmt = fmt & MPP_FRAME_FMT_MASK;
    MPP_RET ret = MPP_NOK;

    if (!ctx_out || !cfg || !cfg->enable || !width || !height)
        return MPP_NOK;

    if (!MPP_FRAME_FMT_IS_YUV(base_fmt) || MPP_FRAME_FMT_IS_YUV_10BIT(base_fmt)) {
        mpp_err("static roi: only 8-bit YUV input is supported, fmt 0x%x\n", fmt);
        return MPP_ERR_VALUE;
    }

    ctx = mpp_calloc(struct MppEncStaticRoiImpl_t, 1);
    if (!ctx)
        return MPP_ERR_MALLOC;

    ctx->cfg = *cfg;
    ctx->cfg.sample_step = static_roi_clamp_s32(ctx->cfg.sample_step, 1, 8);
    if (ctx->cfg.sample_step <= 1)
        ctx->cfg.sample_step = 1;
    else if (ctx->cfg.sample_step <= 2)
        ctx->cfg.sample_step = 2;
    else if (ctx->cfg.sample_step <= 4)
        ctx->cfg.sample_step = 4;
    else
        ctx->cfg.sample_step = 8;
    ctx->cfg.motion_mad_thr = static_roi_clamp_s32(ctx->cfg.motion_mad_thr, 0, 255);
    ctx->cfg.edge_pixel_thr = static_roi_clamp_s32(ctx->cfg.edge_pixel_thr, 1, 1020);
    ctx->cfg.edge_density_thr = static_roi_clamp_s32(ctx->cfg.edge_density_thr, 1, 100);
    ctx->cfg.stable_frames = static_roi_clamp_s32(ctx->cfg.stable_frames, 1, 255);
    ctx->cfg.structure_hold_frames = static_roi_clamp_s32(ctx->cfg.structure_hold_frames, 0, 255);
    ctx->cfg.structure_delta_qp = static_roi_clamp_s32(ctx->cfg.structure_delta_qp, -51, 0);
    ctx->cfg.flat_delta_qp = static_roi_clamp_s32(ctx->cfg.flat_delta_qp, 0, 51);
    ctx->width = width;
    ctx->height = height;
    ctx->mb_w = MPP_ALIGN(width, 16) / 16;
    ctx->mb_h = MPP_ALIGN(height, 16) / 16;
    ctx->block_count = ctx->mb_w * ctx->mb_h;
    ctx->sample_w = (width + ctx->cfg.sample_step - 1) / ctx->cfg.sample_step;
    ctx->sample_h = (height + ctx->cfg.sample_step - 1) / ctx->cfg.sample_step;

    ctx->prev_samples = mpp_malloc(RK_U8, ctx->sample_w * ctx->sample_h);
    ctx->stable_count = mpp_calloc(RK_U8, ctx->block_count);
    ctx->structure_hold = mpp_calloc(RK_U8, ctx->block_count);
    ctx->edge_ema = mpp_calloc(RK_U8, ctx->block_count);
    ctx->delta_qp_map = mpp_calloc(RK_S16, ctx->block_count);
    ctx->protect_map = mpp_calloc(RK_U8, ctx->block_count);
    if (!ctx->prev_samples || !ctx->stable_count || !ctx->structure_hold ||
        !ctx->edge_ema || !ctx->delta_qp_map || !ctx->protect_map) {
        ret = MPP_ERR_MALLOC;
        goto FAIL;
    }

    mpp_log("static roi: grid %ux%u sample_step %d mad_thr %d edge %d/%d%% "
            "stable %d hold %d delta structure %d flat %d\n",
            ctx->mb_w, ctx->mb_h, ctx->cfg.sample_step,
            ctx->cfg.motion_mad_thr, ctx->cfg.edge_pixel_thr,
            ctx->cfg.edge_density_thr, ctx->cfg.stable_frames,
            ctx->cfg.structure_hold_frames, ctx->cfg.structure_delta_qp,
            ctx->cfg.flat_delta_qp);

    *ctx_out = ctx;
    return MPP_OK;

FAIL:
    mpp_enc_static_roi_deinit(ctx);
    return ret;
}

MPP_RET mpp_enc_static_roi_deinit(MppEncStaticRoiCtx ctx)
{
    if (!ctx)
        return MPP_OK;

    MPP_FREE(ctx->prev_samples);
    MPP_FREE(ctx->stable_count);
    MPP_FREE(ctx->structure_hold);
    MPP_FREE(ctx->edge_ema);
    MPP_FREE(ctx->delta_qp_map);
    MPP_FREE(ctx->protect_map);
    MPP_FREE(ctx);
    return MPP_OK;
}

MPP_RET mpp_enc_static_roi_process(MppEncStaticRoiCtx ctx, const RK_U8 *luma,
                                   RK_U32 hor_stride, RK_S32 frame_idx)
{
    RK_U32 step, bx, by;
    RK_U32 structure_count = 0;
    RK_U32 flat_count = 0;
    RK_U32 neutral_count = 0;
    RK_U32 motion_count = 0;

    if (!ctx || !luma || hor_stride < ctx->width)
        return MPP_ERR_VALUE;

    memset(ctx->delta_qp_map, 0, ctx->block_count * sizeof(ctx->delta_qp_map[0]));
    memset(ctx->protect_map, 0, ctx->block_count * sizeof(ctx->protect_map[0]));

    if (!ctx->has_previous) {
        static_roi_save_initial_samples(ctx, luma, hor_stride);
        ctx->has_previous = 1;
        return static_roi_dump_map(ctx, frame_idx, 0, 0, ctx->block_count);
    }

    step = (RK_U32)ctx->cfg.sample_step;
    for (by = 0; by < ctx->mb_h; by++) {
        RK_U32 y0 = by * 16;
        RK_U32 y1 = static_roi_min_u32(y0 + 16, ctx->height);

        for (bx = 0; bx < ctx->mb_w; bx++) {
            RK_U32 x0 = bx * 16;
            RK_U32 x1 = static_roi_min_u32(x0 + 16, ctx->width);
            RK_U32 idx = by * ctx->mb_w + bx;
            RK_U32 diff_sum = 0;
            RK_U32 edge_count = 0;
            RK_U32 sample_count = 0;
            RK_U32 x, y;
            RK_U32 mad, edge_density;
            RK_U32 stable, structure;

            for (y = y0; y < y1; y += step) {
                RK_U32 sy = y / step;
                RK_U32 yu = y >= step ? y - step : y;
                RK_U32 yd = static_roi_min_u32(y + step, ctx->height - 1);
                const RK_U8 *row = luma + y * hor_stride;
                const RK_U8 *row_up = luma + yu * hor_stride;
                const RK_U8 *row_down = luma + yd * hor_stride;

                for (x = x0; x < x1; x += step) {
                    RK_U32 sx = x / step;
                    RK_U32 xl = x >= step ? x - step : x;
                    RK_U32 xr = static_roi_min_u32(x + step, ctx->width - 1);
                    RK_U32 sample_idx = sy * ctx->sample_w + sx;
                    RK_U8 cur = row[x];
                    RK_U32 gradient;

                    diff_sum += static_roi_abs_diff_u8(cur, ctx->prev_samples[sample_idx]);
                    ctx->prev_samples[sample_idx] = cur;
                    /* Compare against the centre instead of subtracting the
                     * two sides. This avoids cancelling symmetric thin text
                     * strokes or textures at the sampling Nyquist period. */
                    gradient = static_roi_abs_diff_u8(cur, row[xl]) +
                               static_roi_abs_diff_u8(row[xr], cur) +
                               static_roi_abs_diff_u8(cur, row_up[x]) +
                               static_roi_abs_diff_u8(row_down[x], cur);
                    if (gradient >= (RK_U32)ctx->cfg.edge_pixel_thr)
                        edge_count++;
                    sample_count++;
                }
            }

            if (!sample_count) {
                neutral_count++;
                continue;
            }

            mad = (diff_sum + sample_count / 2) / sample_count;
            edge_density = (edge_count * 100 + sample_count / 2) / sample_count;
            ctx->edge_ema[idx] = (RK_U8)((3 * ctx->edge_ema[idx] +
                                          edge_density + 2) / 4);
            stable = mad <= (RK_U32)ctx->cfg.motion_mad_thr;
            structure = ctx->edge_ema[idx] >= (RK_U32)ctx->cfg.edge_density_thr;
            if (!stable)
                motion_count++;

            if (stable) {
                if (ctx->stable_count[idx] < 255)
                    ctx->stable_count[idx]++;
            } else {
                ctx->stable_count[idx] = 0;
            }

            if (stable && structure &&
                ctx->stable_count[idx] >= (RK_U32)ctx->cfg.stable_frames) {
                ctx->structure_hold[idx] = (RK_U8)ctx->cfg.structure_hold_frames;
            } else if (ctx->structure_hold[idx]) {
                ctx->structure_hold[idx]--;
            }

            if ((stable && structure &&
                 ctx->stable_count[idx] >= (RK_U32)ctx->cfg.stable_frames) ||
                ctx->structure_hold[idx]) {
                ctx->delta_qp_map[idx] = (RK_S16)ctx->cfg.structure_delta_qp;
                ctx->protect_map[idx] = 1;
                structure_count++;
            } else if (stable &&
                       ctx->stable_count[idx] >= (RK_U32)ctx->cfg.stable_frames &&
                       ctx->edge_ema[idx] * 2 < (RK_U32)ctx->cfg.edge_density_thr) {
                ctx->delta_qp_map[idx] = (RK_S16)ctx->cfg.flat_delta_qp;
                flat_count++;
            } else {
                neutral_count++;
            }
        }
    }

    /* Fail conservative on scene cuts, camera motion, or global exposure
     * changes. Rebuild the temporal state instead of reusing stale classes. */
    if (motion_count * 100 > ctx->block_count * 70) {
        memset(ctx->stable_count, 0, ctx->block_count * sizeof(ctx->stable_count[0]));
        memset(ctx->structure_hold, 0, ctx->block_count * sizeof(ctx->structure_hold[0]));
        memset(ctx->delta_qp_map, 0, ctx->block_count * sizeof(ctx->delta_qp_map[0]));
        memset(ctx->protect_map, 0, ctx->block_count * sizeof(ctx->protect_map[0]));
        structure_count = 0;
        flat_count = 0;
        neutral_count = ctx->block_count;
    }

    return static_roi_dump_map(ctx, frame_idx, structure_count,
                               flat_count, neutral_count);
}

MPP_RET mpp_enc_static_roi_get_map(MppEncStaticRoiCtx ctx,
                                   const RK_S16 **delta_qp_map,
                                   const RK_U8 **protect_map,
                                   RK_U32 *mb_w, RK_U32 *mb_h,
                                   RK_U32 *map_stride)
{
    if (!ctx || !delta_qp_map || !protect_map || !mb_w || !mb_h || !map_stride)
        return MPP_ERR_VALUE;

    *delta_qp_map = ctx->delta_qp_map;
    *protect_map = ctx->protect_map;
    *mb_w = ctx->mb_w;
    *mb_h = ctx->mb_h;
    *map_stride = ctx->mb_w;
    return MPP_OK;
}
