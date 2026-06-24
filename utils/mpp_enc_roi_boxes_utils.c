/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define MODULE_TAG "enc_roi_boxes_utils"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpp_common.h"
#include "mpp_log.h"
#include "mpp_mem.h"

#include "mpp_enc_roi_boxes_utils.h"

typedef enum RoiBoxType_e {
    ROI_BOX_FACE,
    ROI_BOX_PLATE,
    ROI_BOX_PERSON,
    ROI_BOX_VEHICLE,
    ROI_BOX_NONMOTOR,
    ROI_BOX_UNKNOWN,
} RoiBoxType;

typedef struct RoiBox_s {
    RoiBoxType  type;
    RK_S32      x1, y1, x2, y2;
} RoiBox;

typedef struct RoiFrameBoxes_s {
    RK_S32      frame_idx;
    RK_U32      count;
    RK_U32      cap;
    RoiBox      *boxes;
} RoiFrameBoxes;

struct MppEncRoiBoxesImpl_t {
    RoiFrameBoxes   *frames;
    RK_U32          frame_count;
    RK_U32          frame_cap;
};

static char *skip_ws(char *p)
{
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static char *find_matching(char *p, char open, char close)
{
    RK_S32 depth = 0;
    RK_U32 in_str = 0, esc = 0;

    for (; p && *p; p++) {
        if (in_str) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') in_str = 1;
        else if (*p == open) depth++;
        else if (*p == close) { depth--; if (!depth) return p; }
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
    if (!strncmp(p, "person", 6))
        return ROI_BOX_PERSON;
    if (!strncmp(p, "vehicle", 7))
        return ROI_BOX_VEHICLE;
    if (!strncmp(p, "nonmotor", 8))
        return ROI_BOX_NONMOTOR;
    return ROI_BOX_UNKNOWN;
}

static RoiFrameBoxes *get_or_add_frame(MppEncRoiBoxesCtx ctx, RK_S32 frame_idx)
{
    RK_U32 i;

    for (i = 0; i < ctx->frame_count; i++)
        if (ctx->frames[i].frame_idx == frame_idx)
            return &ctx->frames[i];

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

static RK_S32 clamp_s32(RK_S32 v, RK_S32 lo, RK_S32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static RK_S32 apply_one_box(MppEncRoiCtx roi_ctx, const RoiBox *box,
                            RK_U32 width, RK_U32 height,
                            RK_S32 face_delta, RK_S32 plate_delta,
                            RK_S32 person_delta, RK_S32 vehicle_delta,
                            RK_S32 nonmotor_delta,
                            RK_S32 face_expand_blocks, RK_S32 face_abs_qp)
{
    RoiRegionCfg region;
    RK_S32 delta;
    RK_S32 mb_w = (RK_S32)(MPP_ALIGN(width, 16) / 16);
    RK_S32 mb_h = (RK_S32)(MPP_ALIGN(height, 16) / 16);
    RK_S32 bx1, by1, bx2, by2, bw, bh;

    if (!roi_ctx || !box || mb_w <= 0 || mb_h <= 0)
        return 0;

    switch (box->type) {
    case ROI_BOX_FACE:     delta = face_delta;     break;
    case ROI_BOX_PLATE:    delta = plate_delta;    break;
    case ROI_BOX_PERSON:   delta = person_delta;   break;
    case ROI_BOX_VEHICLE:  delta = vehicle_delta;  break;
    case ROI_BOX_NONMOTOR: delta = nonmotor_delta; break;
    default:               return 0;
    }

    bx1 = box->x1 / 16;
    by1 = box->y1 / 16;
    bx2 = (box->x2 + 15) / 16;
    by2 = (box->y2 + 15) / 16;

    if (box->x2 <= box->x1 || box->y2 <= box->y1)
        return 0;

    if (box->type == ROI_BOX_FACE && face_expand_blocks > 0) {
        bx1 -= face_expand_blocks;
        by1 -= face_expand_blocks;
        bx2 += face_expand_blocks;
        by2 += face_expand_blocks;
    }

    bx1 = clamp_s32(bx1, 0, mb_w - 1);
    by1 = clamp_s32(by1, 0, mb_h - 1);
    bx2 = clamp_s32(bx2, bx1 + 1, mb_w);
    by2 = clamp_s32(by2, by1 + 1, mb_h);
    bw  = bx2 - bx1;
    bh  = by2 - by1;

    memset(&region, 0, sizeof(region));
    region.x = (RK_U16)(bx1 * 16);
    region.y = (RK_U16)(by1 * 16);
    region.w = (RK_U16)(bw * 16);
    region.h = (RK_U16)(bh * 16);

    if (region.x >= width || region.y >= height)
        return 0;
    if (region.x + region.w > width)
        region.w = (RK_U16)(width - region.x);
    if (region.y + region.h > height)
        region.h = (RK_U16)(height - region.y);
    if (region.w < 16 || region.h < 16)
        return 0;

    region.force_intra = 0;
    if (box->type == ROI_BOX_FACE && face_abs_qp >= 0) {
        region.qp_mode = 1;
        region.qp_val = clamp_s32(face_abs_qp, 0, 51);
    } else {
        region.qp_mode = 0;
        region.qp_val = clamp_s32(delta, -51, 51);
    }

    return mpp_enc_roi_add_region(roi_ctx, &region) == MPP_OK ? 1 : 0;
}

static MPP_RET parse_frame_object(MppEncRoiBoxesCtx ctx, char *obj)
{
    RK_S32 frame_idx;
    RoiFrameBoxes *frame;
    char *boxes, *arr_end;
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

MPP_RET mpp_enc_roi_boxes_init(MppEncRoiBoxesCtx *ctx_out, const char *file)
{
    MppEncRoiBoxesCtx ctx;
    FILE *fp;
    char *buf = NULL;
    long size;
    MPP_RET ret = MPP_OK;

    if (!ctx_out || !file || !file[0])
        return MPP_NOK;

    ctx = mpp_calloc(struct MppEncRoiBoxesImpl_t, 1);
    if (!ctx)
        return MPP_ERR_MALLOC;

    fp = fopen(file, "rb");
    if (!fp) {
        mpp_err("roi boxes: failed to open %s: %s\n", file, strerror(errno));
        MPP_FREE(ctx);
        return MPP_NOK;
    }

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        mpp_err("roi boxes: failed to stat %s\n", file);
        fclose(fp);
        MPP_FREE(ctx);
        return MPP_NOK;
    }

    buf = mpp_malloc(char, size + 1);
    if (!buf) {
        fclose(fp);
        MPP_FREE(ctx);
        return MPP_ERR_MALLOC;
    }

    if (fread(buf, 1, size, fp) != (size_t)size) {
        mpp_err("roi boxes: failed to read %s\n", file);
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

    if (!ret && !ctx->frame_count) {
        mpp_err("roi boxes: no frame records parsed from %s\n", file);
        ret = MPP_NOK;
    }

DONE:
    fclose(fp);
    MPP_FREE(buf);
    if (ret) {
        mpp_enc_roi_boxes_deinit(ctx);
        return ret;
    }

    mpp_log("roi boxes: loaded %u frame records from %s\n", ctx->frame_count, file);
    *ctx_out = ctx;
    return MPP_OK;
}

MPP_RET mpp_enc_roi_boxes_deinit(MppEncRoiBoxesCtx ctx)
{
    RK_U32 i;

    if (!ctx)
        return MPP_OK;
    for (i = 0; i < ctx->frame_count; i++)
        MPP_FREE(ctx->frames[i].boxes);
    MPP_FREE(ctx->frames);
    MPP_FREE(ctx);
    return MPP_OK;
}

RK_S32 mpp_enc_roi_boxes_apply(MppEncRoiBoxesCtx ctx, MppEncRoiCtx roi_ctx,
                               RK_S32 frame_idx, RK_U32 width, RK_U32 height,
                               RK_S32 face_delta, RK_S32 plate_delta,
                               RK_S32 person_delta, RK_S32 vehicle_delta,
                               RK_S32 nonmotor_delta,
                               RK_S32 face_expand_blocks, RK_S32 face_abs_qp)
{
    const RoiFrameBoxes *frame = NULL;
    RK_S32 mb_w = (RK_S32)(MPP_ALIGN(width, 16) / 16);
    RK_S32 mb_h = (RK_S32)(MPP_ALIGN(height, 16) / 16);
    RK_S32 added = 0;
    RK_U32 i;

    if (!ctx || !roi_ctx || mb_w <= 0 || mb_h <= 0)
        return -1;

    for (i = 0; i < ctx->frame_count; i++) {
        if (ctx->frames[i].frame_idx == frame_idx) {
            frame = &ctx->frames[i];
            break;
        }
    }
    if (!frame)
        return 0;

    for (i = 0; i < frame->count; i++)
        added += apply_one_box(roi_ctx, &frame->boxes[i], width, height,
                               face_delta, plate_delta, person_delta, vehicle_delta,
                               nonmotor_delta, face_expand_blocks, face_abs_qp);
    return added;
}

RK_S32 mpp_enc_roi_boxes_apply_frame(MppEncRoiCtx roi_ctx, const char *boxes_json,
                                     RK_U32 width, RK_U32 height,
                                     RK_S32 face_delta, RK_S32 plate_delta,
                                     RK_S32 person_delta, RK_S32 vehicle_delta,
                                     RK_S32 nonmotor_delta,
                                     RK_S32 face_expand_blocks, RK_S32 face_abs_qp)
{
    char *json;
    char *arr_end;
    RK_S32 added = 0;
    RK_S32 mb_w;
    RK_S32 mb_h;

    if (!roi_ctx || !boxes_json || !boxes_json[0])
        return 0;

    mb_w = (RK_S32)(MPP_ALIGN(width, 16) / 16);
    mb_h = (RK_S32)(MPP_ALIGN(height, 16) / 16);
    if (mb_w <= 0 || mb_h <= 0)
        return -1;

    json = skip_ws((char *)boxes_json);
    if (!json || *json != '[')
        return -1;

    arr_end = find_matching(json, '[', ']');
    if (!arr_end)
        return -1;

    for (json++; json < arr_end;) {
        char *box_start = strchr(json, '{');
        char *box_end;
        RoiBox box;

        if (!box_start || box_start >= arr_end)
            break;
        box_end = find_matching(box_start, '{', '}');
        if (!box_end || box_end > arr_end)
            return -1;

        memset(&box, 0, sizeof(box));
        box.type = parse_type_key(box_start);
        if (box.type != ROI_BOX_UNKNOWN &&
            parse_int_key(box_start, "x1", &box.x1) &&
            parse_int_key(box_start, "y1", &box.y1) &&
            parse_int_key(box_start, "x2", &box.x2) &&
            parse_int_key(box_start, "y2", &box.y2)) {
            added += apply_one_box(roi_ctx, &box, width, height,
                                   face_delta, plate_delta, person_delta, vehicle_delta,
                                   nonmotor_delta, face_expand_blocks, face_abs_qp);
        }
        json = box_end + 1;
    }
    return added;
}
