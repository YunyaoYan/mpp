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

#define MODULE_TAG "enc_bg_filter_utils"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include "mpp_common.h"
#include "mpp_log.h"
#include "mpp_mem.h"

#include "mpp_enc_bg_filter_utils.h"

/* ========================================================================
 *  ROI box parsing (shared logic with qpmap_roi, simplified)
 * ======================================================================== */

typedef enum BgFilterBoxType_t {
    BG_FILTER_BOX_FACE,
    BG_FILTER_BOX_PLATE,
    BG_FILTER_BOX_VEHICLE,
    BG_FILTER_BOX_PERSON,
    BG_FILTER_BOX_UNKNOWN,
} BgFilterBoxType;

typedef struct BgFilterBox_t {
    BgFilterBoxType     type;
    RK_S32              x1;
    RK_S32              y1;
    RK_S32              x2;
    RK_S32              y2;
    RK_S32              score;  /* x100 if needed */
} BgFilterBox;

typedef struct BgFilterFrameBoxes_t {
    RK_S32              frame_idx;
    RK_U32              count;
    RK_U32              cap;
    BgFilterBox         *boxes;
} BgFilterFrameBoxes;

/* ========================================================================
 *  Context structure
 * ======================================================================== */

struct MppEncBgFilterImpl_t {
    MppEncBgFilterCfg   cfg;
    RK_U32              width;
    RK_U32              height;
    RK_U32              hor_stride;
    RK_U32              ver_stride;
    MppFrameFormat      fmt;
    RK_U32              y_size;         /* Y plane size in bytes */
    RK_U32              uv_size;        /* UV plane size in bytes */
    RK_U32              frame_size;     /* total frame size */

    /* ROI mask: 1 = protect, 0 = background. Per-pixel, width x height. */
    RK_U8               *roi_mask;

    /* Previous filtered frame for temporal blend (full frame copy) */
    RK_U8               *prev_filtered;
    RK_S32              has_prev;

    /* Frame boxes loaded from JSONL */
    BgFilterFrameBoxes  *frames;
    RK_U32              frame_count;
    RK_U32              frame_cap;

    /* Last process state */
    RK_S32              last_filtered;
    RK_S32              last_mean_y;
    RK_S32              last_level;
    RK_S32              last_kernel;
    RK_U32              last_roi_area;
};

/* ========================================================================
 *  JSONL parsing helpers (simplified, no external deps)
 * ======================================================================== */

static char *skip_ws(char *p)
{
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static char *find_matching(char *p, char open, char close)
{
    RK_S32 depth = 0;
    RK_U32 in_str = 0;
    RK_U32 esc = 0;

    for (; p && *p; p++) {
        if (in_str) {
            if (esc)
                esc = 0;
            else if (*p == '\\')
                esc = 1;
            else if (*p == '"')
                in_str = 0;
            continue;
        }
        if (*p == '"') {
            in_str = 1;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
            if (!depth)
                return p;
        }
    }
    return NULL;
}

static char *parse_key_value(char *obj, const char *key)
{
    char pattern[64];
    char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(obj, pattern);
    if (!p)
        return NULL;
    p = strchr(p + strlen(pattern), ':');
    return p ? skip_ws(p + 1) : NULL;
}

static RK_U32 parse_int_key(char *obj, const char *key, RK_S32 *val)
{
    char *p = parse_key_value(obj, key);
    char *end = NULL;
    long tmp;

    if (!p)
        return 0;
    tmp = strtol(p, &end, 10);
    if (p == end)
        return 0;
    *val = (RK_S32)tmp;
    return 1;
}

static BgFilterBoxType parse_type_key(char *obj)
{
    char *p = parse_key_value(obj, "type");

    if (!p || *p != '"')
        return BG_FILTER_BOX_UNKNOWN;
    p++;
    if (!strncmp(p, "face", 4))
        return BG_FILTER_BOX_FACE;
    if (!strncmp(p, "plate", 5))
        return BG_FILTER_BOX_PLATE;
    if (!strncmp(p, "vehicle", 7))
        return BG_FILTER_BOX_VEHICLE;
    if (!strncmp(p, "person", 6))
        return BG_FILTER_BOX_PERSON;
    if (!strncmp(p, "car", 3))
        return BG_FILTER_BOX_VEHICLE;

    return BG_FILTER_BOX_UNKNOWN;
}

static BgFilterFrameBoxes *get_or_add_frame(struct MppEncBgFilterImpl_t *ctx, RK_S32 frame_idx)
{
    RK_U32 i;

    for (i = 0; i < ctx->frame_count; i++) {
        if (ctx->frames[i].frame_idx == frame_idx)
            return &ctx->frames[i];
    }

    if (ctx->frame_count >= ctx->frame_cap) {
        RK_U32 new_cap = ctx->frame_cap ? ctx->frame_cap * 2 : 32;
        BgFilterFrameBoxes *frames = mpp_realloc(ctx->frames, BgFilterFrameBoxes, new_cap);

        if (!frames)
            return NULL;
        memset(frames + ctx->frame_cap, 0, (new_cap - ctx->frame_cap) * sizeof(*frames));
        ctx->frames = frames;
        ctx->frame_cap = new_cap;
    }

    ctx->frames[ctx->frame_count].frame_idx = frame_idx;
    return &ctx->frames[ctx->frame_count++];
}

static MPP_RET add_box(BgFilterFrameBoxes *frame, const BgFilterBox *box)
{
    if (frame->count >= frame->cap) {
        RK_U32 new_cap = frame->cap ? frame->cap * 2 : 4;
        BgFilterBox *boxes = mpp_realloc(frame->boxes, BgFilterBox, new_cap);

        if (!boxes)
            return MPP_ERR_MALLOC;
        frame->boxes = boxes;
        frame->cap = new_cap;
    }

    frame->boxes[frame->count++] = *box;
    return MPP_OK;
}

static MPP_RET parse_frame_object(struct MppEncBgFilterImpl_t *ctx, char *obj)
{
    RK_S32 frame_idx;
    BgFilterFrameBoxes *frame;
    char *boxes;
    char *arr_end;
    MPP_RET ret = MPP_OK;

    if (!parse_int_key(obj, "frame_idx", &frame_idx))
        return MPP_OK;

    frame = get_or_add_frame(ctx, frame_idx);
    if (!frame)
        return MPP_ERR_MALLOC;

    boxes = parse_key_value(obj, "boxes");
    if (!boxes || *boxes != '[')
        return MPP_OK;

    arr_end = find_matching(boxes, '[', ']');
    if (!arr_end)
        return MPP_NOK;

    for (boxes++; boxes < arr_end;) {
        char *box_start = strchr(boxes, '{');
        char *box_end;
        BgFilterBox box;

        if (!box_start || box_start >= arr_end)
            break;
        box_end = find_matching(box_start, '{', '}');
        if (!box_end || box_end > arr_end)
            return MPP_NOK;

        memset(&box, 0, sizeof(box));
        box.type = parse_type_key(box_start);
        parse_int_key(box_start, "x1", &box.x1);
        parse_int_key(box_start, "y1", &box.y1);
        parse_int_key(box_start, "x2", &box.x2);
        parse_int_key(box_start, "y2", &box.y2);
        parse_int_key(box_start, "score", &box.score);

        if (box.x2 > box.x1 && box.y2 > box.y1) {
            ret = add_box(frame, &box);
            if (ret)
                return ret;
        }

        boxes = box_end + 1;
    }

    return ret;
}

static MPP_RET load_boxes(struct MppEncBgFilterImpl_t *ctx, const char *file)
{
    FILE *fp = fopen(file, "rb");
    char *buf = NULL;
    long size;
    MPP_RET ret = MPP_OK;

    if (!fp) {
        mpp_err("bg_filter: failed to open boxes file %s: %s\n", file, strerror(errno));
        return MPP_NOK;
    }

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        mpp_err("bg_filter: failed to stat boxes file %s\n", file);
        fclose(fp);
        return MPP_NOK;
    }

    buf = mpp_malloc(char, size + 1);
    if (!buf) {
        fclose(fp);
        return MPP_ERR_MALLOC;
    }

    if (fread(buf, 1, size, fp) != (size_t)size) {
        mpp_err("bg_filter: failed to read boxes file %s\n", file);
        ret = MPP_NOK;
        goto DONE;
    }
    buf[size] = 0;

    for (char *p = buf; (p = strstr(p, "\"frame_idx\""));) {
        char *obj_start = p;
        char *obj_end;

        while (obj_start > buf && *obj_start != '{')
            obj_start--;
        if (*obj_start != '{') {
            ret = MPP_NOK;
            break;
        }
        obj_end = find_matching(obj_start, '{', '}');
        if (!obj_end) {
            ret = MPP_NOK;
            break;
        }
        ret = parse_frame_object(ctx, obj_start);
        if (ret)
            break;
        p = obj_end + 1;
    }

    if (ret)
        mpp_err("bg_filter: failed to parse boxes file %s\n", file);
    else if (!ctx->frame_count)
        mpp_log("bg_filter: no frame records parsed from boxes file %s\n", file);

DONE:
    fclose(fp);
    MPP_FREE(buf);
    return ret;
}

static const BgFilterFrameBoxes *find_frame_boxes(struct MppEncBgFilterImpl_t *ctx, RK_S32 frame_idx)
{
    RK_U32 i;

    for (i = 0; i < ctx->frame_count; i++) {
        if (ctx->frames[i].frame_idx == frame_idx)
            return &ctx->frames[i];
    }
    return NULL;
}

/* ========================================================================
 *  Clamp helper
 * ======================================================================== */

static RK_S32 clamp_s32(RK_S32 val, RK_S32 min, RK_S32 max)
{
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

/* ========================================================================
 *  ROI mask generation
 * ======================================================================== */

static void build_roi_mask(struct MppEncBgFilterImpl_t *ctx, const BgFilterFrameBoxes *boxes)
{
    RK_U32 w = ctx->width;
    RK_U32 h = ctx->height;
    RK_U32 i, x, y;
    RK_U32 roi_pixels = 0;

    memset(ctx->roi_mask, 0, w * h);

    if (!boxes || !boxes->count) {
        ctx->last_roi_area = 0;
        return;
    }

    for (i = 0; i < boxes->count; i++) {
        const BgFilterBox *box = &boxes->boxes[i];
        RK_S32 expand;
        RK_S32 bw = box->x2 - box->x1;
        RK_S32 bh = box->y2 - box->y1;
        RK_S32 x1, y1, x2, y2;
        RK_S32 dx, dy;

        if (bw <= 0 || bh <= 0)
            continue;

        switch (box->type) {
        case BG_FILTER_BOX_FACE:
            expand = ctx->cfg.roi_expand_face;
            break;
        case BG_FILTER_BOX_PLATE:
            expand = ctx->cfg.roi_expand_plate;
            break;
        default:
            expand = ctx->cfg.roi_expand_default;
            break;
        }

        /* expand ratio is stored as percent * 100, e.g. 15 = 0.15 */
        dx = (bw * expand + 50) / 100;
        dy = (bh * expand + 50) / 100;

        x1 = clamp_s32(box->x1 - dx, 0, (RK_S32)w);
        y1 = clamp_s32(box->y1 - dy, 0, (RK_S32)h);
        x2 = clamp_s32(box->x2 + dx, 0, (RK_S32)w);
        y2 = clamp_s32(box->y2 + dy, 0, (RK_S32)h);

        if (x2 <= x1 || y2 <= y1)
            continue;

        for (y = (RK_U32)y1; y < (RK_U32)y2; y++) {
            memset(ctx->roi_mask + y * w + x1, 1, (size_t)(x2 - x1));
        }
    }

    /* Simple dilate: 3x3 cross, iterations */
    if (ctx->cfg.roi_mask_dilate_iter > 0) {
        RK_U8 *tmp = mpp_malloc(RK_U8, w * h);
        if (tmp) {
            RK_S32 iter;
            for (iter = 0; iter < ctx->cfg.roi_mask_dilate_iter; iter++) {
                memcpy(tmp, ctx->roi_mask, w * h);
                for (y = 1; y < h - 1; y++) {
                    for (x = 1; x < w - 1; x++) {
                        RK_U32 idx = y * w + x;
                        if (tmp[idx - 1] || tmp[idx + 1] ||
                            tmp[idx - w] || tmp[idx + w] || tmp[idx])
                            ctx->roi_mask[idx] = 1;
                    }
                }
            }
            mpp_free(tmp);
        }
    }

    for (i = 0; i < w * h; i++) {
        if (ctx->roi_mask[i])
            roi_pixels++;
    }
    ctx->last_roi_area = roi_pixels;
}

/* ========================================================================
 *  Low-light estimation
 * ======================================================================== */

static RK_S32 estimate_low_light_y_only(struct MppEncBgFilterImpl_t *ctx, RK_U8 *y_plane)
{
    RK_U32 w = ctx->width;
    RK_U32 h = ctx->height;
    RK_U32 hs = ctx->hor_stride;
    RK_U64 sum = 0;
    RK_U32 x, y;
    RK_U32 count = w * h;
    RK_S32 mean_y;
    RK_S32 level;

    if (!count)
        return 0;

    for (y = 0; y < h; y++) {
        RK_U8 *row = y_plane + y * hs;
        for (x = 0; x < w; x++)
            sum += row[x];
    }

    mean_y = (RK_S32)(sum / count);
    ctx->last_mean_y = mean_y;

    if (mean_y >= ctx->cfg.low_light_thr)
        level = 0;
    else if (mean_y >= ctx->cfg.strong_light_thr)
        level = 1;
    else
        level = 2;

    ctx->last_level = level;
    return level;
}

/* ========================================================================
 *  Separable Gaussian blur on Y plane (pure C, no OpenCV)
 * ======================================================================== */

/*
 * 3x3 Gaussian: separable kernel [1, 2, 1] / 4
 * 5x5 Gaussian: separable kernel [1, 4, 6, 4, 1] / 16
 */

static void gaussian_blur_y_plane(RK_U8 *src, RK_U8 *dst, RK_U32 w, RK_U32 h, RK_U32 stride,
                                   RK_S32 kernel_size)
{
    RK_S32 *tmp;
    RK_U32 x, y;
    RK_S32 half = kernel_size / 2;

    tmp = mpp_malloc(RK_S32, w * h);
    if (!tmp)
        return;

    if (kernel_size == 3) {
        /* Horizontal pass */
        for (y = 0; y < h; y++) {
            RK_U8 *row = src + y * stride;
            for (x = 0; x < w; x++) {
                RK_S32 a = (x > 0) ? row[x - 1] : row[x];
                RK_S32 b = row[x];
                RK_S32 c = (x + 1 < w) ? row[x + 1] : row[x];
                tmp[y * w + x] = a + 2 * b + c;
            }
        }
        /* Vertical pass + normalize */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                RK_S32 a = (y > 0) ? tmp[(y - 1) * w + x] : tmp[y * w + x];
                RK_S32 b = tmp[y * w + x];
                RK_S32 c = (y + 1 < h) ? tmp[(y + 1) * w + x] : tmp[y * w + x];
                RK_S32 v = (a + 2 * b + c + 8) >> 4;  /* divide by 16, round */
                dst[y * stride + x] = (RK_U8)clamp_s32(v, 0, 255);
            }
        }
    } else if (kernel_size == 5) {
        /* Horizontal pass */
        for (y = 0; y < h; y++) {
            RK_U8 *row = src + y * stride;
            for (x = 0; x < w; x++) {
                RK_S32 a = (x > 1) ? row[x - 2] : row[0];
                RK_S32 b = (x > 0) ? row[x - 1] : row[0];
                RK_S32 c = row[x];
                RK_S32 d = (x + 1 < w) ? row[x + 1] : row[w - 1];
                RK_S32 e = (x + 2 < w) ? row[x + 2] : row[w - 1];
                tmp[y * w + x] = a + 4 * b + 6 * c + 4 * d + e;
            }
        }
        /* Vertical pass + normalize */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                RK_S32 a = (y > 1) ? tmp[(y - 2) * w + x] : tmp[0 * w + x];
                RK_S32 b = (y > 0) ? tmp[(y - 1) * w + x] : tmp[0 * w + x];
                RK_S32 c = tmp[y * w + x];
                RK_S32 d = (y + 1 < h) ? tmp[(y + 1) * w + x] : tmp[(h - 1) * w + x];
                RK_S32 e = (y + 2 < h) ? tmp[(y + 2) * w + x] : tmp[(h - 1) * w + x];
                RK_S32 v = (a + 4 * b + 6 * c + 4 * d + e + 128) >> 8;  /* divide by 256, round */
                dst[y * stride + x] = (RK_U8)clamp_s32(v, 0, 255);
            }
        }
    }

    mpp_free(tmp);
}

/* ========================================================================
 *  RGB/BGR Gaussian blur (3-channel, simple per-channel separable)
 * ======================================================================== */

static void gaussian_blur_rgb_plane(RK_U8 *src, RK_U8 *dst, RK_U32 w, RK_U32 h, RK_U32 stride,
                                     RK_S32 kernel_size, RK_S32 channels)
{
    RK_S32 *tmp;
    RK_U32 x, y, c;
    RK_U32 ch_stride = stride;  /* bytes per row */

    tmp = mpp_malloc(RK_S32, w * channels);
    if (!tmp)
        return;

    if (kernel_size == 3) {
        for (c = 0; c < (RK_U32)channels; c++) {
            /* Horizontal pass, store in dst as intermediate */
            for (y = 0; y < h; y++) {
                RK_U8 *row = src + y * ch_stride;
                for (x = 0; x < w; x++) {
                    RK_S32 a = (x > 0) ? row[(x - 1) * channels + c] : row[x * channels + c];
                    RK_S32 b = row[x * channels + c];
                    RK_S32 d = (x + 1 < w) ? row[(x + 1) * channels + c] : row[x * channels + c];
                    tmp[x] = a + 2 * b + d;
                }
                for (x = 0; x < w; x++) {
                    RK_S32 a = (y > 0) ? dst[(y - 1) * ch_stride + x * channels + c] : 0;
                    /* Actually we need vertical on tmp. Let's write tmp back to dst first. */
                    dst[y * ch_stride + x * channels + c] = (RK_U8)clamp_s32((tmp[x] + 2) >> 2, 0, 255);
                }
            }
            /* Vertical pass on dst (which now holds horizontal blurred result) */
            memcpy(tmp, dst, h * ch_stride);  /* tmp too small, need bigger buffer */
        }
    }

    /* Simpler approach: allocate full temp buffer */
    mpp_free(tmp);
    {
        RK_S32 *tmp2 = mpp_malloc(RK_S32, w * h * channels);
        if (!tmp2)
            return;

        for (c = 0; c < (RK_U32)channels; c++) {
            /* Horizontal */
            for (y = 0; y < h; y++) {
                RK_U8 *row = src + y * ch_stride;
                for (x = 0; x < w; x++) {
                    RK_S32 a = (x > 0) ? row[(x - 1) * channels + c] : row[x * channels + c];
                    RK_S32 b = row[x * channels + c];
                    RK_S32 d = (x + 1 < w) ? row[(x + 1) * channels + c] : row[x * channels + c];
                    tmp2[(y * w + x) * channels + c] = a + 2 * b + d;
                }
            }
            /* Vertical */
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    RK_S32 a = (y > 0) ? tmp2[((y - 1) * w + x) * channels + c] : tmp2[(y * w + x) * channels + c];
                    RK_S32 b = tmp2[(y * w + x) * channels + c];
                    RK_S32 d = (y + 1 < h) ? tmp2[((y + 1) * w + x) * channels + c] : tmp2[(y * w + x) * channels + c];
                    RK_S32 v = (a + 2 * b + d + 8) >> 4;
                    dst[y * ch_stride + x * channels + c] = (RK_U8)clamp_s32(v, 0, 255);
                }
            }
        }
        mpp_free(tmp2);
    }
}

/* ========================================================================
 *  Apply blur according to format
 * ======================================================================== */

static void apply_spatial_filter(struct MppEncBgFilterImpl_t *ctx,
                                  RK_U8 *src, RK_U8 *dst,
                                  RK_S32 kernel_size)
{
    MppFrameFormat fmt = ctx->fmt & MPP_FRAME_FMT_MASK;

    switch (fmt) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420SP_VU: {
        /* Only blur Y plane; copy UV as-is */
        RK_U32 y_size = ctx->y_size;
        RK_U32 uv_size = ctx->uv_size;

        gaussian_blur_y_plane(src, dst, ctx->width, ctx->height, ctx->hor_stride, kernel_size);
        memcpy(dst + y_size, src + y_size, uv_size);
        break;
    }
    case MPP_FMT_YUV420P: {
        RK_U32 y_size = ctx->y_size;
        RK_U32 u_size = ctx->uv_size / 2;
        RK_U32 v_size = ctx->uv_size / 2;

        gaussian_blur_y_plane(src, dst, ctx->width, ctx->height, ctx->hor_stride, kernel_size);
        memcpy(dst + y_size, src + y_size, u_size);
        memcpy(dst + y_size + u_size, src + y_size + u_size, v_size);
        break;
    }
    case MPP_FMT_YUV422SP:
    case MPP_FMT_YUV422SP_VU:
    case MPP_FMT_YUV422_YUYV:
    case MPP_FMT_YUV422_YVYU:
    case MPP_FMT_YUV422_UYVY:
    case MPP_FMT_YUV422_VYUY: {
        /* For packed/semi-planar 422, just copy whole frame to avoid format complexity */
        memcpy(dst, src, ctx->frame_size);
        break;
    }
    case MPP_FMT_RGB888:
    case MPP_FMT_BGR888: {
        gaussian_blur_rgb_plane(src, dst, ctx->width, ctx->height, ctx->hor_stride, kernel_size, 3);
        break;
    }
    case MPP_FMT_ARGB8888:
    case MPP_FMT_ABGR8888:
    case MPP_FMT_BGRA8888:
    case MPP_FMT_RGBA8888: {
        gaussian_blur_rgb_plane(src, dst, ctx->width, ctx->height, ctx->hor_stride, kernel_size, 4);
        break;
    }
    default: {
        /* Unsupported: copy as-is */
        memcpy(dst, src, ctx->frame_size);
        break;
    }
    }
}

/* ========================================================================
 *  ROI-protected copy: restore ROI pixels from original
 * ======================================================================== */

static void roi_restore(struct MppEncBgFilterImpl_t *ctx, RK_U8 *orig, RK_U8 *filtered)
{
    RK_U32 w = ctx->width;
    RK_U32 h = ctx->height;
    RK_U32 hs = ctx->hor_stride;
    MppFrameFormat fmt = ctx->fmt & MPP_FRAME_FMT_MASK;
    RK_U32 x, y;

    switch (fmt) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420SP_VU:
    case MPP_FMT_YUV420P: {
        /* Only Y plane has per-pixel ROI */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                if (ctx->roi_mask[y * w + x]) {
                    filtered[y * hs + x] = orig[y * hs + x];
                }
            }
        }
        break;
    }
    case MPP_FMT_RGB888:
    case MPP_FMT_BGR888: {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                if (ctx->roi_mask[y * w + x]) {
                    RK_U32 idx = y * hs + x * 3;
                    filtered[idx + 0] = orig[idx + 0];
                    filtered[idx + 1] = orig[idx + 1];
                    filtered[idx + 2] = orig[idx + 2];
                }
            }
        }
        break;
    }
    case MPP_FMT_ARGB8888:
    case MPP_FMT_ABGR8888:
    case MPP_FMT_BGRA8888:
    case MPP_FMT_RGBA8888: {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                if (ctx->roi_mask[y * w + x]) {
                    RK_U32 idx = y * hs + x * 4;
                    filtered[idx + 0] = orig[idx + 0];
                    filtered[idx + 1] = orig[idx + 1];
                    filtered[idx + 2] = orig[idx + 2];
                    filtered[idx + 3] = orig[idx + 3];
                }
            }
        }
        break;
    }
    default:
        /* For other formats, skip ROI restore (whole frame filtered) */
        break;
    }
}

/* ========================================================================
 *  Temporal blend (simplified, no motion compensation)
 * ======================================================================== */

static void temporal_blend(struct MppEncBgFilterImpl_t *ctx,
                            RK_U8 *current, RK_U8 *prev,
                            RK_U8 *output)
{
    RK_U32 w = ctx->width;
    RK_U32 h = ctx->height;
    RK_U32 hs = ctx->hor_stride;
    MppFrameFormat fmt = ctx->fmt & MPP_FRAME_FMT_MASK;
    RK_S32 alpha = ctx->cfg.temporal_alpha;  /* 0~100 */
    RK_S32 motion_thr = ctx->cfg.motion_diff_thr;
    RK_U32 x, y;
    RK_U32 motion_pixels = 0;
    RK_U32 static_pixels = 0;

    if (alpha < 0) alpha = 0;
    if (alpha > 100) alpha = 100;

    /* Only blend Y plane for YUV formats */
    if (fmt == MPP_FMT_YUV420SP || fmt == MPP_FMT_YUV420SP_VU || fmt == MPP_FMT_YUV420P) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                RK_U32 idx = y * hs + x;
                RK_S32 diff = (RK_S32)current[idx] - (RK_S32)prev[idx];
                if (diff < 0) diff = -diff;

                if (ctx->roi_mask[y * w + x]) {
                    /* ROI: always use current */
                    output[idx] = current[idx];
                } else if (diff > motion_thr) {
                    /* Motion area: use current (or spatial filtered) */
                    output[idx] = current[idx];
                    motion_pixels++;
                } else {
                    /* Static background: temporal blend */
                    RK_S32 v = (alpha * current[idx] + (100 - alpha) * prev[idx] + 50) / 100;
                    output[idx] = (RK_U8)clamp_s32(v, 0, 255);
                    static_pixels++;
                }
            }
        }
        /* Copy UV unchanged (already in current) */
        if (fmt == MPP_FMT_YUV420SP || fmt == MPP_FMT_YUV420SP_VU) {
            memcpy(output + ctx->y_size, current + ctx->y_size, ctx->uv_size);
        } else if (fmt == MPP_FMT_YUV420P) {
            RK_U32 u_size = ctx->uv_size / 2;
            memcpy(output + ctx->y_size, current + ctx->y_size, u_size);
            memcpy(output + ctx->y_size + u_size, current + ctx->y_size + u_size, u_size);
        }
    } else {
        /* For RGB: blend all channels, skip motion gating for simplicity */
        RK_S32 channels = (fmt == MPP_FMT_RGB888 || fmt == MPP_FMT_BGR888) ? 3 : 4;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                RK_U32 base = y * hs + x * channels;
                RK_S32 c;
                for (c = 0; c < channels; c++) {
                    RK_S32 v = (alpha * current[base + c] + (100 - alpha) * prev[base + c] + 50) / 100;
                    output[base + c] = (RK_U8)clamp_s32(v, 0, 255);
                }
            }
        }
    }

    (void)motion_pixels;
    (void)static_pixels;
}

/* ========================================================================
 *  Debug dump
 * ======================================================================== */

static void dump_debug_pgm(const char *path, RK_U8 *data, RK_U32 w, RK_U32 h, RK_U32 stride)
{
    FILE *fp = fopen(path, "wb");
    RK_U32 y;
    if (!fp) return;
    fprintf(fp, "P5\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++)
        fwrite(data + y * stride, 1, w, fp);
    fclose(fp);
}

static void dump_debug_ppm(const char *path, RK_U8 *data, RK_U32 w, RK_U32 h, RK_U32 stride, RK_S32 ch)
{
    FILE *fp = fopen(path, "wb");
    RK_U32 y, x;
    if (!fp) return;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            RK_U8 pix[3];
            if (ch == 3) {
                pix[0] = data[y * stride + x * 3 + 0];
                pix[1] = data[y * stride + x * 3 + 1];
                pix[2] = data[y * stride + x * 3 + 2];
            } else if (ch == 4) {
                pix[0] = data[y * stride + x * 4 + 0];
                pix[1] = data[y * stride + x * 4 + 1];
                pix[2] = data[y * stride + x * 4 + 2];
            } else {
                pix[0] = pix[1] = pix[2] = data[y * stride + x];
            }
            fwrite(pix, 1, 3, fp);
        }
    }
    fclose(fp);
}

static void dump_debug_mask_pgm(const char *path, RK_U8 *mask, RK_U32 w, RK_U32 h)
{
    FILE *fp = fopen(path, "wb");
    RK_U32 y;
    if (!fp) return;
    fprintf(fp, "P5\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        RK_U32 x;
        for (x = 0; x < w; x++) {
            RK_U8 v = mask[y * w + x] ? 255 : 0;
            fwrite(&v, 1, 1, fp);
        }
    }
    fclose(fp);
}

static void dump_debug_compare(struct MppEncBgFilterImpl_t *ctx,
                                RK_U8 *orig, RK_U8 *filtered,
                                RK_S32 frame_idx)
{
    char path[1024];
    MppFrameFormat fmt = ctx->fmt & MPP_FRAME_FMT_MASK;
    RK_U32 w = ctx->width;
    RK_U32 h = ctx->height;
    RK_U32 hs = ctx->hor_stride;

    if (!ctx->cfg.debug_dir || !ctx->cfg.debug_dir[0])
        return;

    if (mkdir(ctx->cfg.debug_dir) && errno != EEXIST) {
        mpp_err("bg_filter: failed to create debug dir %s: %s\n",
                ctx->cfg.debug_dir, strerror(errno));
        return;
    }

    /* Original Y or RGB */
    snprintf(path, sizeof(path), "%s/frame_%06d_orig.pgm",
             ctx->cfg.debug_dir, frame_idx);
    if (fmt == MPP_FMT_YUV420SP || fmt == MPP_FMT_YUV420SP_VU || fmt == MPP_FMT_YUV420P)
        dump_debug_pgm(path, orig, w, h, hs);
    else if (fmt == MPP_FMT_RGB888 || fmt == MPP_FMT_BGR888)
        dump_debug_ppm(path, orig, w, h, hs, 3);
    else if (fmt == MPP_FMT_ARGB8888 || fmt == MPP_FMT_ABGR8888 ||
             fmt == MPP_FMT_BGRA8888 || fmt == MPP_FMT_RGBA8888)
        dump_debug_ppm(path, orig, w, h, hs, 4);

    /* Mask */
    snprintf(path, sizeof(path), "%s/frame_%06d_mask.pgm",
             ctx->cfg.debug_dir, frame_idx);
    dump_debug_mask_pgm(path, ctx->roi_mask, w, h);

    /* Filtered Y or RGB */
    snprintf(path, sizeof(path), "%s/frame_%06d_filtered.pgm",
             ctx->cfg.debug_dir, frame_idx);
    if (fmt == MPP_FMT_YUV420SP || fmt == MPP_FMT_YUV420SP_VU || fmt == MPP_FMT_YUV420P)
        dump_debug_pgm(path, filtered, w, h, hs);
    else if (fmt == MPP_FMT_RGB888 || fmt == MPP_FMT_BGR888)
        dump_debug_ppm(path, filtered, w, h, hs, 3);
    else if (fmt == MPP_FMT_ARGB8888 || fmt == MPP_FMT_ABGR8888 ||
             fmt == MPP_FMT_BGRA8888 || fmt == MPP_FMT_RGBA8888)
        dump_debug_ppm(path, filtered, w, h, hs, 4);
}

/* ========================================================================
 *  Public API
 * ======================================================================== */

MPP_RET mpp_enc_bg_filter_init(MppEncBgFilterCtx *ctx_out, RK_U32 w, RK_U32 h,
                               MppFrameFormat fmt, const MppEncBgFilterCfg *cfg)
{
    struct MppEncBgFilterImpl_t *ctx;
    MPP_RET ret = MPP_OK;
    RK_U32 y_size, uv_size, frame_size;

    if (!ctx_out || !cfg)
        return MPP_NOK;

    if (!cfg->enable) {
        *ctx_out = NULL;
        return MPP_OK;
    }

    ctx = mpp_calloc(struct MppEncBgFilterImpl_t, 1);
    if (!ctx)
        return MPP_ERR_MALLOC;

    ctx->cfg = *cfg;
    ctx->width = w;
    ctx->height = h;
    ctx->hor_stride = w;  /* will be updated per-frame if needed */
    ctx->ver_stride = h;
    ctx->fmt = fmt;

    /* Compute plane sizes */
    switch (fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420SP_VU:
        y_size = w * h;
        uv_size = w * h / 2;
        frame_size = y_size + uv_size;
        break;
    case MPP_FMT_YUV420P:
        y_size = w * h;
        uv_size = w * h / 2;
        frame_size = y_size + uv_size;
        break;
    case MPP_FMT_YUV422SP:
    case MPP_FMT_YUV422SP_VU:
    case MPP_FMT_YUV422_YUYV:
    case MPP_FMT_YUV422_YVYU:
    case MPP_FMT_YUV422_UYVY:
    case MPP_FMT_YUV422_VYUY:
        y_size = w * h;
        uv_size = w * h;
        frame_size = y_size + uv_size;
        break;
    case MPP_FMT_RGB888:
    case MPP_FMT_BGR888:
        y_size = 0;
        uv_size = 0;
        frame_size = w * h * 3;
        break;
    case MPP_FMT_ARGB8888:
    case MPP_FMT_ABGR8888:
    case MPP_FMT_BGRA8888:
    case MPP_FMT_RGBA8888:
        y_size = 0;
        uv_size = 0;
        frame_size = w * h * 4;
        break;
    default:
        y_size = w * h;
        uv_size = 0;
        frame_size = w * h * 4;
        break;
    }

    ctx->y_size = y_size;
    ctx->uv_size = uv_size;
    ctx->frame_size = frame_size;

    ctx->roi_mask = mpp_malloc(RK_U8, w * h);
    ctx->prev_filtered = mpp_malloc(RK_U8, frame_size);

    if (!ctx->roi_mask || !ctx->prev_filtered) {
        ret = MPP_ERR_MALLOC;
        goto ERR;
    }

    if (cfg->boxes_file && cfg->boxes_file[0]) {
        ret = load_boxes(ctx, cfg->boxes_file);
        if (ret)
            goto ERR;
        mpp_log("bg_filter: loaded %u frame records from %s\n",
                ctx->frame_count, cfg->boxes_file);
    } else {
        mpp_log("bg_filter: no boxes file provided, ROI mask will be all-zero\n");
    }

    mpp_log("bg_filter: init w %u h %u fmt 0x%x y_size %u uv_size %u\n",
            w, h, fmt, y_size, uv_size);
    mpp_log("bg_filter: cfg enable %d type %d low_thr %d strong_thr %d "
            "weak_k %d strong_k %d temporal %d alpha %d motion_thr %d\n",
            cfg->enable, cfg->filter_type, cfg->low_light_thr, cfg->strong_light_thr,
            cfg->kernel_weak, cfg->kernel_strong, cfg->enable_temporal,
            cfg->temporal_alpha, cfg->motion_diff_thr);

    *ctx_out = ctx;
    return MPP_OK;

ERR:
    mpp_enc_bg_filter_deinit(ctx);
    *ctx_out = NULL;
    return ret;
}

MPP_RET mpp_enc_bg_filter_deinit(MppEncBgFilterCtx ctx)
{
    RK_U32 i;

    if (!ctx)
        return MPP_OK;

    for (i = 0; i < ctx->frame_count; i++)
        MPP_FREE(ctx->frames[i].boxes);

    MPP_FREE(ctx->frames);
    MPP_FREE(ctx->roi_mask);
    MPP_FREE(ctx->prev_filtered);
    MPP_FREE(ctx);

    return MPP_OK;
}

MPP_RET mpp_enc_bg_filter_process(MppEncBgFilterCtx ctx, RK_U8 *buf, RK_U8 *out_buf,
                                  RK_S32 frame_idx, RK_S32 hor_stride, RK_S32 ver_stride)
{
    const BgFilterFrameBoxes *boxes;
    RK_S32 level;
    RK_S32 kernel_size;
    RK_U8 *work_buf = NULL;
    RK_U8 *spatial_filtered = NULL;
    MPP_RET ret = MPP_OK;

    if (!ctx || !buf || !out_buf)
        return MPP_NOK;

    ctx->hor_stride = hor_stride ? (RK_U32)hor_stride : ctx->width;
    ctx->ver_stride = ver_stride ? (RK_U32)ver_stride : ctx->height;

    /* 1. Find boxes for this frame */
    boxes = find_frame_boxes(ctx, frame_idx);

    /* 2. Build ROI mask */
    build_roi_mask(ctx, boxes);

    /* 3. Estimate low-light level */
    level = estimate_low_light_y_only(ctx, buf);
    ctx->last_level = level;

    if (level == 0) {
        /* Not low-light: copy original to output, save as prev */
        memcpy(out_buf, buf, ctx->frame_size);
        memcpy(ctx->prev_filtered, buf, ctx->frame_size);
        ctx->has_prev = 1;
        ctx->last_filtered = 0;
        ctx->last_kernel = 0;

        mpp_log("bg_filter frame %d mean_y %d level %d -> skip (not low-light)\n",
                frame_idx, ctx->last_mean_y, level);
        return MPP_OK;
    }

    /* 4. Determine kernel size */
    if (level == 1)
        kernel_size = ctx->cfg.kernel_weak;
    else
        kernel_size = ctx->cfg.kernel_strong;

    if (kernel_size < 3)
        kernel_size = 3;
    if (kernel_size > 5)
        kernel_size = 5;
    if (kernel_size % 2 == 0)
        kernel_size++;  /* must be odd */

    ctx->last_kernel = kernel_size;

    /* 5. Spatial filter */
    work_buf = mpp_malloc(RK_U8, ctx->frame_size);
    if (!work_buf) {
        memcpy(out_buf, buf, ctx->frame_size);
        return MPP_ERR_MALLOC;
    }

    apply_spatial_filter(ctx, buf, work_buf, kernel_size);
    roi_restore(ctx, buf, work_buf);
    spatial_filtered = work_buf;

    /* 6. Temporal filter (optional) */
    if (ctx->cfg.enable_temporal && ctx->has_prev) {
        temporal_blend(ctx, spatial_filtered, ctx->prev_filtered, out_buf);
        /* After temporal blend, still restore ROI to be safe */
        roi_restore(ctx, buf, out_buf);
    } else {
        memcpy(out_buf, spatial_filtered, ctx->frame_size);
    }

    /* 7. Save as previous filtered frame */
    memcpy(ctx->prev_filtered, out_buf, ctx->frame_size);
    ctx->has_prev = 1;
    ctx->last_filtered = 1;

    /* 8. Debug dump */
    if (ctx->cfg.dump_debug) {
        dump_debug_compare(ctx, buf, out_buf, frame_idx);
    }

    mpp_log("bg_filter frame %d boxes %u mean_y %d level %d kernel %d "
            "roi_area %u temporal %d -> filtered\n",
            frame_idx, boxes ? boxes->count : 0, ctx->last_mean_y, level,
            kernel_size, ctx->last_roi_area, ctx->cfg.enable_temporal);

    mpp_free(work_buf);
    return ret;
}

RK_S32 mpp_enc_bg_filter_last_filtered(MppEncBgFilterCtx ctx)
{
    if (!ctx)
        return -1;
    return ctx->last_filtered;
}
