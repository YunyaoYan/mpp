/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define MODULE_TAG "enc_qpmap_roi_utils"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include "mpp_common.h"
#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_buffer.h"
#include "mpp_meta.h"

#include "mpp_enc_qpmap_roi_utils.h"

typedef enum RoiBoxType_t {
    ROI_BOX_FACE,
    ROI_BOX_PLATE,
    ROI_BOX_UNKNOWN,
} RoiBoxType;

typedef struct RoiBox_t {
    RoiBoxType          type;
    RK_S32              x1;
    RK_S32              y1;
    RK_S32              x2;
    RK_S32              y2;
} RoiBox;

typedef struct RoiFrameBoxes_t {
    RK_S32              frame_idx;
    RK_U32              count;
    RK_U32              cap;
    RoiBox              *boxes;
} RoiFrameBoxes;

typedef struct QpmapStats_t {
    RK_U32              face_box_count;
    RK_U32              plate_box_count;
    RK_U32              roi_block_count;
    RK_U32              bg_block_count;
    RK_S32              bg_delta_qp;
    RK_S32              min_delta_qp;
    RK_S32              max_delta_qp;
    double              mean_delta_qp;
} QpmapStats;

struct MppEncQpmapRoiImpl_t {
    MppEncQpmapRoiCfg   cfg;
    MppCodingType       type;
    RK_U32              width;
    RK_U32              height;
    RK_U32              mb_w;
    RK_U32              mb_h;
    RK_U32              stride_h;
    RK_U32              stride_v;
    RK_U32              block_count;
    RK_S16              *delta_map;
    RK_U8               *roi_mask;
    MppBuffer           qpmap_buf;
    RK_U32              qpmap_size;
    RoiFrameBoxes       *frames;
    RK_U32              frame_count;
    RK_U32              frame_cap;
};

static RK_S32 qpmap_clamp_s32(RK_S32 val, RK_S32 min, RK_S32 max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

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

static RoiBoxType parse_type_key(char *obj)
{
    char *p = parse_key_value(obj, "type");

    if (!p || *p != '"')
        return ROI_BOX_UNKNOWN;
    p++;
    if (!strncmp(p, "face", 4))
        return ROI_BOX_FACE;
    if (!strncmp(p, "plate", 5))
        return ROI_BOX_PLATE;

    return ROI_BOX_UNKNOWN;
}

static RoiFrameBoxes *get_or_add_frame(MppEncQpmapRoiCtx ctx, RK_S32 frame_idx)
{
    RK_U32 i;

    for (i = 0; i < ctx->frame_count; i++) {
        if (ctx->frames[i].frame_idx == frame_idx)
            return &ctx->frames[i];
    }

    if (ctx->frame_count >= ctx->frame_cap) {
        RK_U32 new_cap = ctx->frame_cap ? ctx->frame_cap * 2 : 32;
        RoiFrameBoxes *frames = mpp_realloc(ctx->frames, RoiFrameBoxes, new_cap);

        if (!frames)
            return NULL;
        memset(frames + ctx->frame_cap, 0, (new_cap - ctx->frame_cap) * sizeof(*frames));
        ctx->frames = frames;
        ctx->frame_cap = new_cap;
    }

    ctx->frames[ctx->frame_count].frame_idx = frame_idx;
    return &ctx->frames[ctx->frame_count++];
}

static MPP_RET add_box(RoiFrameBoxes *frame, const RoiBox *box)
{
    if (frame->count >= frame->cap) {
        RK_U32 new_cap = frame->cap ? frame->cap * 2 : 4;
        RoiBox *boxes = mpp_realloc(frame->boxes, RoiBox, new_cap);

        if (!boxes)
            return MPP_ERR_MALLOC;
        frame->boxes = boxes;
        frame->cap = new_cap;
    }

    frame->boxes[frame->count++] = *box;
    return MPP_OK;
}

static MPP_RET parse_frame_object(MppEncQpmapRoiCtx ctx, char *obj)
{
    RK_S32 frame_idx;
    RoiFrameBoxes *frame;
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
        RoiBox box;

        if (!box_start || box_start >= arr_end)
            break;
        box_end = find_matching(box_start, '{', '}');
        if (!box_end || box_end > arr_end)
            return MPP_NOK;

        memset(&box, 0, sizeof(box));
        box.type = parse_type_key(box_start);
        if (box.type != ROI_BOX_UNKNOWN &&
            parse_int_key(box_start, "x1", &box.x1) &&
            parse_int_key(box_start, "y1", &box.y1) &&
            parse_int_key(box_start, "x2", &box.x2) &&
            parse_int_key(box_start, "y2", &box.y2)) {
            ret = add_box(frame, &box);
            if (ret)
                return ret;
        }

        boxes = box_end + 1;
    }

    return ret;
}

static MPP_RET load_boxes(MppEncQpmapRoiCtx ctx, const char *file)
{
    FILE *fp = fopen(file, "rb");
    char *buf = NULL;
    long size;
    MPP_RET ret = MPP_OK;

    if (!fp) {
        mpp_err("qpmap roi: failed to open boxes file %s: %s\n", file, strerror(errno));
        return MPP_NOK;
    }

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        mpp_err("qpmap roi: failed to stat boxes file %s\n", file);
        fclose(fp);
        return MPP_NOK;
    }

    buf = mpp_malloc(char, size + 1);
    if (!buf) {
        fclose(fp);
        return MPP_ERR_MALLOC;
    }

    if (fread(buf, 1, size, fp) != (size_t)size) {
        mpp_err("qpmap roi: failed to read boxes file %s\n", file);
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
        mpp_err("qpmap roi: failed to parse boxes file %s\n", file);
    else if (!ctx->frame_count) {
        mpp_err("qpmap roi: no frame records parsed from boxes file %s\n", file);
        ret = MPP_NOK;
    }

DONE:
    fclose(fp);
    MPP_FREE(buf);
    return ret;
}

static const RoiFrameBoxes *find_frame_boxes(MppEncQpmapRoiCtx ctx, RK_S32 frame_idx)
{
    RK_U32 i;

    for (i = 0; i < ctx->frame_count; i++) {
        if (ctx->frames[i].frame_idx == frame_idx)
            return &ctx->frames[i];
    }

    return NULL;
}

static RK_U16 pack_vepu541_roi_cfg(RK_S32 delta_qp)
{
    RK_U16 qp_adj = (RK_U16)delta_qp & 0x7f;

    /* qp_area_en=1, qp_adj_mode=0 means relative QP adjustment. */
    return (RK_U16)((1 << 7) | (qp_adj << 8));
}

static void encode_delta_to_qpmap(MppEncQpmapRoiCtx ctx)
{
    RK_U16 *dst = (RK_U16 *)mpp_buffer_get_ptr(ctx->qpmap_buf);
    RK_U32 x, y;

    memset(dst, 0, ctx->qpmap_size);
    for (y = 0; y < ctx->mb_h; y++) {
        for (x = 0; x < ctx->mb_w; x++) {
            RK_U32 map_idx = y * ctx->mb_w + x;
            RK_U32 dst_idx = y * ctx->stride_h + x;

            dst[dst_idx] = pack_vepu541_roi_cfg(ctx->delta_map[map_idx]);
        }
    }

    mpp_buffer_sync_end(ctx->qpmap_buf);
}

static MPP_RET dump_qpmap(MppEncQpmapRoiCtx ctx, RK_S32 frame_idx, const QpmapStats *stats)
{
    char path[1024];
    FILE *fp;
    RK_U32 x, y;

    if (!ctx->cfg.dump_qpmap_debug || !ctx->cfg.debug_dir || !ctx->cfg.debug_dir[0])
        return MPP_OK;

    if (mkdir(ctx->cfg.debug_dir) && errno != EEXIST) {
        mpp_err("qpmap roi: failed to create debug dir %s: %s\n",
                ctx->cfg.debug_dir, strerror(errno));
        return MPP_NOK;
    }

    snprintf(path, sizeof(path), "%s/frame_%06d_qpmap.txt", ctx->cfg.debug_dir, frame_idx);
    fp = fopen(path, "wb");
    if (!fp) {
        mpp_err("qpmap roi: failed to open debug dump %s: %s\n", path, strerror(errno));
        return MPP_NOK;
    }

    for (y = 0; y < ctx->mb_h; y++) {
        for (x = 0; x < ctx->mb_w; x++) {
            fprintf(fp, "%d%s", ctx->delta_map[y * ctx->mb_w + x],
                    (x + 1 == ctx->mb_w) ? "" : " ");
        }
        fprintf(fp, "\n");
    }
    fclose(fp);

    mpp_log("qpmap roi frame %d face %u plate %u roi_blk %u bg_blk %u "
            "min %d max %d mean %.3f bg_delta %d set KEY_QPMAP0\n",
            frame_idx, stats->face_box_count, stats->plate_box_count,
            stats->roi_block_count, stats->bg_block_count, stats->min_delta_qp,
            stats->max_delta_qp, stats->mean_delta_qp, stats->bg_delta_qp);

    return MPP_OK;
}

static MPP_RET generate_qpmap(MppEncQpmapRoiCtx ctx, const RoiFrameBoxes *boxes,
                              QpmapStats *stats)
{
    RK_S64 sum = 0;
    RK_S64 neg_sum = 0;
    RK_U32 bg_count = 0;
    RK_U32 roi_count = 0;
    RK_U32 i;

    memset(ctx->delta_map, 0, ctx->block_count * sizeof(ctx->delta_map[0]));
    memset(ctx->roi_mask, 0, ctx->block_count * sizeof(ctx->roi_mask[0]));
    memset(stats, 0, sizeof(*stats));
    stats->min_delta_qp = 0;
    stats->max_delta_qp = 0;

    if (boxes) {
        for (i = 0; i < boxes->count; i++) {
            const RoiBox *box = &boxes->boxes[i];
            RK_S32 x1 = qpmap_clamp_s32(box->x1, 0, (RK_S32)ctx->width);
            RK_S32 y1 = qpmap_clamp_s32(box->y1, 0, (RK_S32)ctx->height);
            RK_S32 x2 = qpmap_clamp_s32(box->x2, 0, (RK_S32)ctx->width);
            RK_S32 y2 = qpmap_clamp_s32(box->y2, 0, (RK_S32)ctx->height);
            RK_S32 delta = (box->type == ROI_BOX_PLATE) ?
                           ctx->cfg.plate_delta_qp : ctx->cfg.face_delta_qp;
            RK_S32 bx1, by1, bx2, by2, x, y;

            if (box->type == ROI_BOX_FACE)
                stats->face_box_count++;
            else if (box->type == ROI_BOX_PLATE)
                stats->plate_box_count++;
            else
                continue;

            if (x2 <= x1 || y2 <= y1)
                continue;

            bx1 = x1 / 16;
            by1 = y1 / 16;
            bx2 = (x2 + 15) / 16 - 1;
            by2 = (y2 + 15) / 16 - 1;
            bx1 = qpmap_clamp_s32(bx1, 0, (RK_S32)ctx->mb_w - 1);
            bx2 = qpmap_clamp_s32(bx2, 0, (RK_S32)ctx->mb_w - 1);
            by1 = qpmap_clamp_s32(by1, 0, (RK_S32)ctx->mb_h - 1);
            by2 = qpmap_clamp_s32(by2, 0, (RK_S32)ctx->mb_h - 1);
            delta = qpmap_clamp_s32(delta, ctx->cfg.delta_qp_min, ctx->cfg.delta_qp_max);

            for (y = by1; y <= by2; y++) {
                for (x = bx1; x <= bx2; x++) {
                    RK_U32 idx = y * ctx->mb_w + x;

                    if (!ctx->roi_mask[idx]) {
                        ctx->roi_mask[idx] = 1;
                        roi_count++;
                    }
                    if (ctx->delta_map[idx] > delta)
                        ctx->delta_map[idx] = (RK_S16)delta;
                }
            }
        }
    }

    for (i = 0; i < ctx->block_count; i++) {
        if (ctx->roi_mask[i])
            neg_sum += ctx->delta_map[i];
        else
            bg_count++;
    }

    if (roi_count && bg_count && ctx->cfg.enable_bg_compensation) {
        RK_S32 bg_delta = (RK_S32)((-neg_sum + bg_count / 2) / bg_count);

        bg_delta = qpmap_clamp_s32(bg_delta, 0, ctx->cfg.bg_delta_qp_max);
        bg_delta = qpmap_clamp_s32(bg_delta, ctx->cfg.delta_qp_min, ctx->cfg.delta_qp_max);
        stats->bg_delta_qp = bg_delta;

        for (i = 0; i < ctx->block_count; i++) {
            if (!ctx->roi_mask[i])
                ctx->delta_map[i] = (RK_S16)bg_delta;
        }
    }

    stats->roi_block_count = roi_count;
    stats->bg_block_count = bg_count;

    for (i = 0; i < ctx->block_count; i++) {
        RK_S32 val = ctx->delta_map[i];

        val = qpmap_clamp_s32(val, ctx->cfg.delta_qp_min, ctx->cfg.delta_qp_max);
        ctx->delta_map[i] = (RK_S16)val;
        if (!i || val < stats->min_delta_qp)
            stats->min_delta_qp = val;
        if (!i || val > stats->max_delta_qp)
            stats->max_delta_qp = val;
        sum += val;
    }
    stats->mean_delta_qp = ctx->block_count ? (double)sum / ctx->block_count : 0.0;

    return MPP_OK;
}

MPP_RET mpp_enc_qpmap_roi_init(MppEncQpmapRoiCtx *ctx_out, RK_U32 w, RK_U32 h,
                               MppCodingType type, const MppEncQpmapRoiCfg *cfg)
{
    MppEncQpmapRoiCtx ctx;
    MPP_RET ret;

    if (!ctx_out || !cfg || !cfg->enable)
        return MPP_NOK;

    if (type != MPP_VIDEO_CodingAVC && type != MPP_VIDEO_CodingHEVC) {
        mpp_err("qpmap roi: KEY_QPMAP0 is only enabled for H.264/H.265 in this demo\n");
        return MPP_NOK;
    }

    if (!cfg->boxes_file || !cfg->boxes_file[0]) {
        mpp_err("qpmap roi: roi_boxes_json is required when enable_qpmap_roi=1\n");
        return MPP_NOK;
    }

    ctx = mpp_calloc(struct MppEncQpmapRoiImpl_t, 1);
    if (!ctx)
        return MPP_ERR_MALLOC;

    ctx->cfg = *cfg;
    ctx->type = type;
    ctx->width = w;
    ctx->height = h;
    ctx->mb_w = MPP_ALIGN(w, 16) / 16;
    ctx->mb_h = MPP_ALIGN(h, 16) / 16;
    ctx->stride_h = MPP_ALIGN(ctx->mb_w, 4);
    ctx->stride_v = MPP_ALIGN(ctx->mb_h, 4);
    ctx->block_count = ctx->mb_w * ctx->mb_h;
    ctx->qpmap_size = ctx->stride_h * ctx->stride_v * sizeof(RK_U16) + 32;
    ctx->delta_map = mpp_calloc(RK_S16, ctx->block_count);
    ctx->roi_mask = mpp_calloc(RK_U8, ctx->block_count);

    if (!ctx->delta_map || !ctx->roi_mask) {
        ret = MPP_ERR_MALLOC;
        goto ERR;
    }

    ret = mpp_buffer_get(NULL, &ctx->qpmap_buf, ctx->qpmap_size);
    if (ret || !ctx->qpmap_buf) {
        mpp_err("qpmap roi: failed to allocate qpmap buffer size %u\n", ctx->qpmap_size);
        ret = MPP_ERR_MALLOC;
        goto ERR;
    }

    ret = load_boxes(ctx, cfg->boxes_file);
    if (ret)
        goto ERR;

    mpp_log("qpmap roi: loaded %u frame records from %s, grid %ux%u stride %ux%u\n",
            ctx->frame_count, cfg->boxes_file, ctx->mb_w, ctx->mb_h,
            ctx->stride_h, ctx->stride_v);

    *ctx_out = ctx;
    return MPP_OK;

ERR:
    mpp_enc_qpmap_roi_deinit(ctx);
    return ret;
}

MPP_RET mpp_enc_qpmap_roi_deinit(MppEncQpmapRoiCtx ctx)
{
    RK_U32 i;

    if (!ctx)
        return MPP_OK;

    if (ctx->qpmap_buf)
        mpp_buffer_put(ctx->qpmap_buf);

    for (i = 0; i < ctx->frame_count; i++)
        MPP_FREE(ctx->frames[i].boxes);

    MPP_FREE(ctx->frames);
    MPP_FREE(ctx->delta_map);
    MPP_FREE(ctx->roi_mask);
    MPP_FREE(ctx);

    return MPP_OK;
}

MPP_RET mpp_enc_qpmap_roi_setup_meta(MppEncQpmapRoiCtx ctx, MppMeta meta,
                                     RK_S32 frame_idx)
{
    const RoiFrameBoxes *boxes;
    QpmapStats stats;
    MPP_RET ret;

    if (!ctx || !meta)
        return MPP_NOK;

    boxes = find_frame_boxes(ctx, frame_idx);
    ret = generate_qpmap(ctx, boxes, &stats);
    if (ret)
        return ret;

    encode_delta_to_qpmap(ctx);
    mpp_meta_set_buffer(meta, KEY_QPMAP0, ctx->qpmap_buf);

    return dump_qpmap(ctx, frame_idx, &stats);
}
