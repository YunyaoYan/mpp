/*
 * Copyright 2026 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpp_jpeg_roi_recompress.h"

#define MAX_ROI_REGIONS 1024

static void show_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s -i input.jpg -o output.jpg [options]\n"
            "Options:\n"
            "  -r, --roi x,y,w,h[,importance]  Add ROI rectangle (repeatable)\n"
            "  -s, --strength 0..63            Background strength (default 32)\n"
            "  -m, --margin pixels             ROI expansion (default 16)\n"
            "  -f, --feather pixels            ROI feather width (default 16)\n"
            "  -t, --target-bytes bytes        Search strength for target size\n"
            "      --no-optimize               Disable Huffman optimization\n"
            "      --drop-markers              Do not copy APP/COM markers\n"
            "  -h, --help                      Show this help\n",
            program);
}

static int parse_long(const char *text, long min_value, long max_value,
                      long *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < min_value || parsed > max_value)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_roi(const char *text, MppJpegRoiRect *roi)
{
    int x, y, w, h, importance = 255;
    char tail;
    int count;

    count = sscanf(text, "%d,%d,%d,%d,%d%c",
                   &x, &y, &w, &h, &importance, &tail);
    if (count != 4 && count != 5)
        return -1;
    if (w <= 0 || h <= 0 || importance < 0 || importance > 255)
        return -1;

    roi->x = x;
    roi->y = y;
    roi->w = w;
    roi->h = h;
    roi->importance = (uint8_t)importance;
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file;
    long length;
    uint8_t *buffer;

    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "failed to open input %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "failed to determine input size: %s\n",
                strerror(errno));
        fclose(file);
        return -1;
    }
    if (!length) {
        fprintf(stderr, "input file is empty\n");
        fclose(file);
        return -1;
    }

    buffer = (uint8_t *)malloc((size_t)length);
    if (!buffer) {
        fprintf(stderr, "failed to allocate %ld input bytes\n", length);
        fclose(file);
        return -1;
    }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "failed to read input file\n");
        free(buffer);
        fclose(file);
        return -1;
    }

    fclose(file);
    *data = buffer;
    *size = (size_t)length;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    int failed;

    if (!file) {
        fprintf(stderr, "failed to open output %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    failed = fwrite(data, 1, size, file) != size;
    if (fclose(file))
        failed = 1;
    if (failed) {
        fprintf(stderr, "failed to write output %s\n", path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        { "input",         required_argument, NULL, 'i' },
        { "output",        required_argument, NULL, 'o' },
        { "roi",           required_argument, NULL, 'r' },
        { "strength",      required_argument, NULL, 's' },
        { "margin",        required_argument, NULL, 'm' },
        { "feather",       required_argument, NULL, 'f' },
        { "target-bytes",  required_argument, NULL, 't' },
        { "no-optimize",   no_argument,       NULL, 1000 },
        { "drop-markers",  no_argument,       NULL, 1001 },
        { "help",          no_argument,       NULL, 'h' },
        { NULL,            0,                 NULL, 0 },
    };
    MppJpegRoiRect regions[MAX_ROI_REGIONS];
    MppJpegRoiRecompressCfg cfg;
    MppJpegRoiRecompressResult result;
    const char *input_path = NULL;
    const char *output_path = NULL;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    size_t input_size = 0;
    size_t output_size = 0;
    size_t region_count = 0;
    char error_message[256];
    int option;
    int ret = EXIT_FAILURE;
    int strength_set = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.background_strength = 32;
    cfg.roi_margin = 16;
    cfg.feather_pixels = 16;
    cfg.optimize_huffman = 1;
    cfg.copy_markers = 1;

    while ((option = getopt_long(argc, argv, "i:o:r:s:m:f:t:h",
                                 long_options, NULL)) != -1) {
        long value;

        switch (option) {
        case 'i':
            input_path = optarg;
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'r':
            if (region_count >= MAX_ROI_REGIONS ||
                parse_roi(optarg, &regions[region_count])) {
                fprintf(stderr, "invalid ROI: %s\n", optarg);
                goto done;
            }
            region_count++;
            break;
        case 's':
            if (parse_long(optarg, 0, MPP_JPEG_ROI_STRENGTH_MAX, &value)) {
                fprintf(stderr, "invalid strength: %s\n", optarg);
                goto done;
            }
            cfg.background_strength = (uint8_t)value;
            strength_set = 1;
            break;
        case 'm':
            if (parse_long(optarg, 0, 65535, &value)) {
                fprintf(stderr, "invalid margin: %s\n", optarg);
                goto done;
            }
            cfg.roi_margin = (uint16_t)value;
            break;
        case 'f':
            if (parse_long(optarg, 0, 65535, &value)) {
                fprintf(stderr, "invalid feather: %s\n", optarg);
                goto done;
            }
            cfg.feather_pixels = (uint16_t)value;
            break;
        case 't':
            if (parse_long(optarg, 1, 0x7fffffffL, &value)) {
                fprintf(stderr, "invalid target size: %s\n", optarg);
                goto done;
            }
            cfg.target_bytes = (size_t)value;
            break;
        case 1000:
            cfg.optimize_huffman = 0;
            break;
        case 1001:
            cfg.copy_markers = 0;
            break;
        case 'h':
            show_usage(argv[0]);
            ret = EXIT_SUCCESS;
            goto done;
        default:
            show_usage(argv[0]);
            goto done;
        }
    }

    if (!input_path || !output_path) {
        show_usage(argv[0]);
        goto done;
    }

    cfg.regions = regions;
    cfg.region_count = region_count;
    if (cfg.target_bytes && !strength_set)
        cfg.background_strength = MPP_JPEG_ROI_STRENGTH_MAX;

    if (read_file(input_path, &input, &input_size))
        goto done;

    if (mpp_jpeg_roi_recompress(input, input_size, &cfg,
                                &output, &output_size, &result,
                                error_message, sizeof(error_message))) {
        fprintf(stderr, "JPEG ROI recompression failed: %s\n",
                error_message);
        goto done;
    }

    if (write_file(output_path, output, output_size))
        goto done;

    printf("JPEG ROI recompression complete\n");
    printf("  image:              %ux%u\n", result.width, result.height);
    printf("  regions:            %zu\n", region_count);
    printf("  strength:           %u\n", result.used_background_strength);
    printf("  target:             %zu (%s)\n", cfg.target_bytes,
           cfg.target_bytes ? (result.target_met ? "met" : "not met") :
           "disabled");
    printf("  bytes:              %zu -> %zu\n",
           result.input_bytes, result.output_bytes);
    printf("  recompression:      %.3fx\n",
           result.output_bytes ?
           (double)result.input_bytes / result.output_bytes : 0.0);
    printf("  blocks:             %zu total, %zu fully protected, "
           "%zu modified\n",
           result.total_blocks, result.protected_blocks,
           result.modified_blocks);
    printf("  nonzero AC:         %zu -> %zu\n",
           result.nonzero_ac_before, result.nonzero_ac_after);
    ret = EXIT_SUCCESS;

done:
    free(input);
    mpp_jpeg_roi_recompress_free(output);
    return ret;
}
