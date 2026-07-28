/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "mpp_jpeg_roi_recompress.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

typedef struct MppJpegErrorMgr_t {
    struct jpeg_error_mgr   pub;
    jmp_buf                 jump_buffer;
    char                    message[JMSG_LENGTH_MAX];
} MppJpegErrorMgr;

typedef struct MppJpegPassResult_t {
    uint8_t                         *data;
    size_t                          size;
    MppJpegRoiRecompressResult      stats;
} MppJpegPassResult;

/* Zig-zag position to natural 8x8 coefficient index. */
static const uint8_t jpeg_zigzag_order[DCTSIZE2] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

static void mpp_jpeg_error_exit(j_common_ptr cinfo)
{
    MppJpegErrorMgr *err = (MppJpegErrorMgr *)cinfo->err;

    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump_buffer, 1);
}

static void set_error(char *dst, size_t dst_size, const char *message)
{
    if (!dst || !dst_size)
        return;

    snprintf(dst, dst_size, "%s", message ? message : "unknown JPEG error");
}

static int clamp_int(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

static int rect_gap(int a0, int a1, int b0, int b1)
{
    if (a1 <= b0)
        return b0 - a1;
    if (b1 <= a0)
        return a0 - b1;
    return 0;
}

static uint8_t block_importance(const MppJpegRoiRecompressCfg *cfg,
                                int block_x0, int block_y0,
                                int block_x1, int block_y1,
                                int image_width, int image_height)
{
    size_t i;
    int best = 0;

    for (i = 0; i < cfg->region_count; i++) {
        const MppJpegRoiRect *region = &cfg->regions[i];
        int64_t raw_x0, raw_y0, raw_x1, raw_y1;
        int x0, y0, x1, y1;
        int dx, dy, distance;
        int importance;
        int overlaps;

        if (region->w <= 0 || region->h <= 0 || !region->importance)
            continue;

        raw_x0 = (int64_t)region->x - cfg->roi_margin;
        raw_y0 = (int64_t)region->y - cfg->roi_margin;
        raw_x1 = (int64_t)region->x + region->w + cfg->roi_margin;
        raw_y1 = (int64_t)region->y + region->h + cfg->roi_margin;
        x0 = raw_x0 < 0 ? 0 :
             (raw_x0 > image_width ? image_width : (int)raw_x0);
        y0 = raw_y0 < 0 ? 0 :
             (raw_y0 > image_height ? image_height : (int)raw_y0);
        x1 = raw_x1 < 0 ? 0 :
             (raw_x1 > image_width ? image_width : (int)raw_x1);
        y1 = raw_y1 < 0 ? 0 :
             (raw_y1 > image_height ? image_height : (int)raw_y1);
        if (x1 <= x0 || y1 <= y0)
            continue;

        overlaps = block_x0 < x1 && block_x1 > x0 &&
                   block_y0 < y1 && block_y1 > y0;

        if (overlaps) {
            importance = region->importance;
        } else {
            dx = rect_gap(block_x0, block_x1, x0, x1);
            dy = rect_gap(block_y0, block_y1, y0, y1);
            distance = dx > dy ? dx : dy;
            if (cfg->feather_pixels && distance < cfg->feather_pixels)
                importance = region->importance *
                             (cfg->feather_pixels - distance) /
                             cfg->feather_pixels;
            else
                importance = 0;
        }

        if (importance > best)
            best = importance;
    }

    return (uint8_t)best;
}

static int count_nonzero_ac(const JCOEF *block)
{
    int pos;
    int count = 0;

    for (pos = 1; pos < DCTSIZE2; pos++)
        count += block[jpeg_zigzag_order[pos]] != 0;

    return count;
}

static int filter_block(JCOEF *block, int strength)
{
    int pos;
    int cutoff;
    int changed = 0;

    if (strength <= 0)
        return 0;

    /*
     * Keep at least the first eight zig-zag coefficients. At full strength,
     * all higher frequencies are erased; at lower strengths the cutoff moves
     * gradually toward the end of the block.
     */
    cutoff = 64 - (strength * 56 + 31) / 63;
    cutoff = clamp_int(cutoff, 8, 64);

    for (pos = 1; pos < DCTSIZE2; pos++) {
        int index = jpeg_zigzag_order[pos];
        int value = block[index];
        int magnitude = value < 0 ? -value : value;
        int threshold;

        if (!value)
            continue;

        if (pos >= cutoff) {
            block[index] = 0;
            changed = 1;
            continue;
        }

        /*
         * Increase the zero dead-zone with both ROI strength and spatial
         * frequency. This only modifies already-quantized coefficients and
         * does not change the JPEG DQT syntax.
         */
        threshold = strength * (pos + 8) / (63 * 12);
        if (threshold > 0 && magnitude <= threshold) {
            block[index] = 0;
            changed = 1;
        }
    }

    return changed;
}

static void copy_saved_markers(j_decompress_ptr src, j_compress_ptr dst)
{
    jpeg_saved_marker_ptr marker;

    for (marker = src->marker_list; marker; marker = marker->next)
        jpeg_write_marker(dst, marker->marker, marker->data,
                          marker->data_length);
}

static int run_recompress_pass(const uint8_t *input,
                               size_t input_size,
                               const MppJpegRoiRecompressCfg *cfg,
                               uint8_t strength,
                               MppJpegPassResult *pass,
                               char *error_message,
                               size_t error_message_size)
{
    struct jpeg_decompress_struct src;
    struct jpeg_compress_struct dst;
    MppJpegErrorMgr src_err;
    MppJpegErrorMgr dst_err;
    jvirt_barray_ptr *coef_arrays = NULL;
    unsigned char *jpeg_data = NULL;
    unsigned long jpeg_size = 0;
    int src_created = 0;
    int dst_created = 0;
    int ci;

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    memset(&src_err, 0, sizeof(src_err));
    memset(&dst_err, 0, sizeof(dst_err));
    memset(pass, 0, sizeof(*pass));

    src.err = jpeg_std_error(&src_err.pub);
    src_err.pub.error_exit = mpp_jpeg_error_exit;
    if (setjmp(src_err.jump_buffer)) {
        set_error(error_message, error_message_size, src_err.message);
        if (dst_created)
            jpeg_destroy_compress(&dst);
        if (src_created)
            jpeg_destroy_decompress(&src);
        free(jpeg_data);
        return -1;
    }

    jpeg_create_decompress(&src);
    src_created = 1;
    jpeg_mem_src(&src, input, (unsigned long)input_size);

    if (cfg->copy_markers) {
        int marker;

        for (marker = 0; marker < 16; marker++)
            jpeg_save_markers(&src, JPEG_APP0 + marker, 0xffff);
        jpeg_save_markers(&src, JPEG_COM, 0xffff);
    }

    if (jpeg_read_header(&src, TRUE) != JPEG_HEADER_OK) {
        set_error(error_message, error_message_size, "invalid JPEG header");
        jpeg_destroy_decompress(&src);
        return -1;
    }

    if (src.num_components <= 0 || src.max_h_samp_factor <= 0 ||
        src.max_v_samp_factor <= 0) {
        set_error(error_message, error_message_size,
                  "unsupported JPEG component layout");
        jpeg_destroy_decompress(&src);
        return -1;
    }

    coef_arrays = jpeg_read_coefficients(&src);
    if (!coef_arrays) {
        set_error(error_message, error_message_size,
                  "failed to read JPEG DCT coefficients");
        jpeg_destroy_decompress(&src);
        return -1;
    }

    pass->stats.width = src.image_width;
    pass->stats.height = src.image_height;
    pass->stats.input_bytes = input_size;
    pass->stats.used_background_strength = strength;

    for (ci = 0; ci < src.num_components; ci++) {
        jpeg_component_info *comp = &src.comp_info[ci];
        JDIMENSION block_y;

        for (block_y = 0; block_y < comp->height_in_blocks; block_y++) {
            JBLOCKARRAY row = (*src.mem->access_virt_barray)
                              ((j_common_ptr)&src, coef_arrays[ci],
                               block_y, 1, TRUE);
            JDIMENSION block_x;

            for (block_x = 0; block_x < comp->width_in_blocks; block_x++) {
                JCOEFPTR block = row[0][block_x];
                int x0 = (int)((uint64_t)block_x * DCTSIZE *
                               src.max_h_samp_factor /
                               comp->h_samp_factor);
                int y0 = (int)((uint64_t)block_y * DCTSIZE *
                               src.max_v_samp_factor /
                               comp->v_samp_factor);
                int x1 = (int)((uint64_t)(block_x + 1) * DCTSIZE *
                               src.max_h_samp_factor /
                               comp->h_samp_factor);
                int y1 = (int)((uint64_t)(block_y + 1) * DCTSIZE *
                               src.max_v_samp_factor /
                               comp->v_samp_factor);
                uint8_t importance;
                int effective_strength;
                int nonzero_before;
                int nonzero_after;

                importance = block_importance(cfg, x0, y0, x1, y1,
                                              src.image_width,
                                              src.image_height);
                effective_strength = strength * (255 - importance) / 255;

                nonzero_before = count_nonzero_ac(block);
                pass->stats.total_blocks++;
                pass->stats.nonzero_ac_before += nonzero_before;
                if (importance == 255)
                    pass->stats.protected_blocks++;

                if (filter_block(block, effective_strength))
                    pass->stats.modified_blocks++;

                nonzero_after = count_nonzero_ac(block);
                pass->stats.nonzero_ac_after += nonzero_after;
            }
        }
    }

    dst.err = jpeg_std_error(&dst_err.pub);
    dst_err.pub.error_exit = mpp_jpeg_error_exit;
    if (setjmp(dst_err.jump_buffer)) {
        set_error(error_message, error_message_size, dst_err.message);
        if (dst_created)
            jpeg_destroy_compress(&dst);
        jpeg_destroy_decompress(&src);
        free(jpeg_data);
        return -1;
    }

    jpeg_create_compress(&dst);
    dst_created = 1;
    jpeg_mem_dest(&dst, &jpeg_data, &jpeg_size);
    jpeg_copy_critical_parameters(&src, &dst);
    dst.optimize_coding = cfg->optimize_huffman ? TRUE : FALSE;

    if (cfg->copy_markers) {
        dst.write_JFIF_header = FALSE;
        dst.write_Adobe_marker = FALSE;
    }

    jpeg_write_coefficients(&dst, coef_arrays);
    if (cfg->copy_markers)
        copy_saved_markers(&src, &dst);

    jpeg_finish_compress(&dst);
    jpeg_finish_decompress(&src);
    jpeg_destroy_compress(&dst);
    jpeg_destroy_decompress(&src);

    pass->data = jpeg_data;
    pass->size = (size_t)jpeg_size;
    pass->stats.output_bytes = pass->size;
    return 0;
}

static void take_pass(MppJpegPassResult *dst, MppJpegPassResult *src)
{
    free(dst->data);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

int mpp_jpeg_roi_recompress(const uint8_t *input,
                            size_t input_size,
                            const MppJpegRoiRecompressCfg *cfg,
                            uint8_t **output,
                            size_t *output_size,
                            MppJpegRoiRecompressResult *result,
                            char *error_message,
                            size_t error_message_size)
{
    MppJpegRoiRecompressCfg local_cfg;
    MppJpegPassResult best;
    MppJpegPassResult current;
    int max_strength;

    if (error_message && error_message_size)
        error_message[0] = '\0';
    if (!input || !input_size || !cfg || !output || !output_size) {
        set_error(error_message, error_message_size, "invalid argument");
        return -1;
    }
    if (cfg->region_count && !cfg->regions) {
        set_error(error_message, error_message_size,
                  "region_count is non-zero but regions is NULL");
        return -1;
    }
    if (input_size > (size_t)~0UL) {
        set_error(error_message, error_message_size,
                  "input JPEG is too large for libjpeg");
        return -1;
    }

    *output = NULL;
    *output_size = 0;
    if (result)
        memset(result, 0, sizeof(*result));

    local_cfg = *cfg;
    if (local_cfg.background_strength > MPP_JPEG_ROI_STRENGTH_MAX)
        local_cfg.background_strength = MPP_JPEG_ROI_STRENGTH_MAX;
    memset(&best, 0, sizeof(best));
    memset(&current, 0, sizeof(current));

    if (!local_cfg.target_bytes) {
        if (run_recompress_pass(input, input_size, &local_cfg,
                                local_cfg.background_strength, &best,
                                error_message, error_message_size))
            return -1;
    } else {
        int low;
        int high;
        int candidate = -1;

        max_strength = local_cfg.background_strength ?
                       local_cfg.background_strength :
                       MPP_JPEG_ROI_STRENGTH_MAX;

        if (run_recompress_pass(input, input_size, &local_cfg, 0, &current,
                                error_message, error_message_size))
            return -1;
        if (current.size <= local_cfg.target_bytes) {
            take_pass(&best, &current);
            best.stats.target_met = 1;
        } else {
            free(current.data);
            memset(&current, 0, sizeof(current));

            if (run_recompress_pass(input, input_size, &local_cfg,
                                    (uint8_t)max_strength, &current,
                                    error_message, error_message_size))
                return -1;
            if (current.size > local_cfg.target_bytes) {
                take_pass(&best, &current);
                best.stats.target_met = 0;
            } else {
                free(current.data);
                memset(&current, 0, sizeof(current));
                low = 1;
                high = max_strength;

                while (low <= high) {
                    int mid = low + (high - low) / 2;

                    if (run_recompress_pass(input, input_size, &local_cfg,
                                            (uint8_t)mid, &current,
                                            error_message,
                                            error_message_size)) {
                        free(best.data);
                        return -1;
                    }

                    if (current.size <= local_cfg.target_bytes) {
                        candidate = mid;
                        take_pass(&best, &current);
                        high = mid - 1;
                    } else {
                        free(current.data);
                        memset(&current, 0, sizeof(current));
                        low = mid + 1;
                    }
                }

                /*
                 * Huffman optimization makes the size curve almost monotonic,
                 * but not mathematically guaranteed. Check the nearby levels
                 * and keep the lowest strength that meets the target.
                 */
                if (candidate >= 0) {
                    int start = candidate > 3 ? candidate - 3 : 0;
                    int end = candidate + 3 < max_strength ?
                              candidate + 3 : max_strength;
                    int strength;

                    for (strength = start; strength <= end; strength++) {
                        if (strength == best.stats.used_background_strength)
                            continue;
                        if (run_recompress_pass(input, input_size, &local_cfg,
                                                (uint8_t)strength, &current,
                                                error_message,
                                                error_message_size)) {
                            free(best.data);
                            return -1;
                        }
                        if (current.size <= local_cfg.target_bytes &&
                            strength <
                            best.stats.used_background_strength) {
                            take_pass(&best, &current);
                        } else {
                            free(current.data);
                            memset(&current, 0, sizeof(current));
                        }
                    }
                    best.stats.target_met = 1;
                }
            }
        }
    }

    if (!best.data) {
        set_error(error_message, error_message_size,
                  "JPEG recompression produced no output");
        return -1;
    }

    *output = best.data;
    *output_size = best.size;
    if (result)
        *result = best.stats;
    return 0;
}

void mpp_jpeg_roi_recompress_free(void *buffer)
{
    free(buffer);
}
