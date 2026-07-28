/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef MPP_JPEG_ROI_RECOMPRESS_H
#define MPP_JPEG_ROI_RECOMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPP_JPEG_ROI_STRENGTH_MAX 63

typedef struct MppJpegRoiRect_t {
    int32_t     x;
    int32_t     y;
    int32_t     w;
    int32_t     h;
    uint8_t     importance;
} MppJpegRoiRect;

typedef struct MppJpegRoiRecompressCfg_t {
    const MppJpegRoiRect    *regions;
    size_t                  region_count;

    /*
     * Exact strength when target_bytes is zero. When target_bytes is non-zero,
     * this is the search ceiling. A zero search ceiling means the maximum 63.
     */
    uint8_t                 background_strength;
    uint16_t                roi_margin;
    uint16_t                feather_pixels;
    size_t                  target_bytes;

    uint8_t                 optimize_huffman;
    uint8_t                 copy_markers;
} MppJpegRoiRecompressCfg;

typedef struct MppJpegRoiRecompressResult_t {
    uint32_t    width;
    uint32_t    height;
    size_t      input_bytes;
    size_t      output_bytes;
    uint8_t     used_background_strength;
    uint8_t     target_met;

    size_t      total_blocks;
    size_t      protected_blocks;
    size_t      modified_blocks;
    size_t      nonzero_ac_before;
    size_t      nonzero_ac_after;
} MppJpegRoiRecompressResult;

/*
 * Recompress an in-memory DCT JPEG without decoding it to pixels.
 *
 * ROI blocks keep their existing quantized DCT coefficients. Non-ROI blocks
 * receive a strength-dependent AC dead-zone and high-frequency cutoff. The
 * JPEG quantization tables and standard bitstream syntax remain unchanged.
 *
 * The returned output buffer is allocated by libjpeg and must be released with
 * mpp_jpeg_roi_recompress_free().
 */
int mpp_jpeg_roi_recompress(const uint8_t *input,
                            size_t input_size,
                            const MppJpegRoiRecompressCfg *cfg,
                            uint8_t **output,
                            size_t *output_size,
                            MppJpegRoiRecompressResult *result,
                            char *error_message,
                            size_t error_message_size);

void mpp_jpeg_roi_recompress_free(void *buffer);

#ifdef __cplusplus
}
#endif

#endif /* MPP_JPEG_ROI_RECOMPRESS_H */
