/*
 * Standalone test for ROI-protected background filtering.
 * No MPP dependency. Pure C + stdlib.
 *
 * Build:
 *   gcc -O2 -o test_bg_filter_standalone test_bg_filter_standalone.c -lm
 *
 * Run:
 *   ./test_bg_filter_standalone \
 *       -i /tmp/night_640x360.yuv \
 *       -o /tmp/filtered_640x360.yuv \
 *       -w 640 -h 360 -n 10 \
 *       -low_thr 80 -strong_thr 50 -weak_k 3 -strong_k 5 \
 *       -dump_mask 1 -dump_dir /tmp/bg_filter_debug
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

/* ========================================================================
 *  Config
 * ======================================================================== */

typedef struct {
    int enable;
    int low_light_thr;
    int strong_light_thr;
    int kernel_weak;
    int kernel_strong;
    int roi_expand_face;      /* percent * 100, e.g. 15 = 0.15 */
    int roi_expand_plate;
    int roi_expand_default;
    int roi_mask_dilate_iter;
    int enable_temporal;
    int temporal_alpha;       /* 0~100 */
    int motion_diff_thr;
    int dump_debug;
    char *debug_dir;
} BgFilterCfg;

/* ========================================================================
 *  Box / ROI
 * ======================================================================== */

typedef enum {
    BOX_FACE,
    BOX_PLATE,
    BOX_VEHICLE,
    BOX_PERSON,
    BOX_UNKNOWN,
} BoxType;

typedef struct {
    BoxType type;
    int x1, y1, x2, y2;
} Box;

typedef struct {
    int frame_idx;
    int count;
    Box *boxes;
} FrameBoxes;

/* ========================================================================
 *  Helpers
 * ======================================================================== */

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void *safe_malloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "malloc failed size=%zu\n", size);
        exit(1);
    }
    return p;
}

/* ========================================================================
 *  Fake detection boxes (hard-coded for demo)
 * ======================================================================== */

static FrameBoxes *get_fake_boxes(int frame_idx, int w, int h)
{
    static FrameBoxes fb[3];
    static Box b0[] = {
        {BOX_FACE,    0, 0, 0, 0},
        {BOX_PLATE,   0, 0, 0, 0},
    };
    static Box b1[] = {
        {BOX_FACE,    0, 0, 0, 0},
        {BOX_PLATE,   0, 0, 0, 0},
    };
    static Box b2[] = {
        {BOX_FACE,    0, 0, 0, 0},
        {BOX_VEHICLE, 0, 0, 0, 0},
    };
    static int inited = 0;

    if (!inited) {
        /* Frame 0 */
        b0[0].x1 = (int)(w * 0.28); b0[0].y1 = (int)(h * 0.25);
        b0[0].x2 = (int)(w * 0.42); b0[0].y2 = (int)(h * 0.50);
        b0[1].x1 = (int)(w * 0.55); b0[1].y1 = (int)(h * 0.48);
        b0[1].x2 = (int)(w * 0.70); b0[1].y2 = (int)(h * 0.56);
        fb[0].frame_idx = 0; fb[0].count = 2; fb[0].boxes = b0;

        /* Frame 1 (slight shift) */
        b1[0].x1 = (int)(w * 0.29); b1[0].y1 = (int)(h * 0.26);
        b1[0].x2 = (int)(w * 0.43); b1[0].y2 = (int)(h * 0.51);
        b1[1].x1 = (int)(w * 0.56); b1[1].y1 = (int)(h * 0.49);
        b1[1].x2 = (int)(w * 0.71); b1[1].y2 = (int)(h * 0.57);
        fb[1].frame_idx = 1; fb[1].count = 2; fb[1].boxes = b1;

        /* Frame 2 */
        b2[0].x1 = (int)(w * 0.30); b2[0].y1 = (int)(h * 0.27);
        b2[0].x2 = (int)(w * 0.44); b2[0].y2 = (int)(h * 0.52);
        b2[1].x1 = (int)(w * 0.40); b2[1].y1 = (int)(h * 0.50);
        b2[1].x2 = (int)(w * 0.75); b2[1].y2 = (int)(h * 0.85);
        fb[2].frame_idx = 2; fb[2].count = 2; fb[2].boxes = b2;

        inited = 1;
    }

    if (frame_idx >= 0 && frame_idx < 3)
        return &fb[frame_idx];
    return &fb[frame_idx % 3];
}

/* ========================================================================
 *  ROI mask
 * ======================================================================== */

static void build_roi_mask(uint8_t *mask, int w, int h,
                           const FrameBoxes *fb, const BgFilterCfg *cfg)
{
    int i, x, y;
    memset(mask, 0, w * h);

    if (!fb || !fb->count)
        return;

    for (i = 0; i < fb->count; i++) {
        const Box *box = &fb->boxes[i];
        int expand;
        int bw = box->x2 - box->x1;
        int bh = box->y2 - box->y1;
        int x1, y1, x2, y2;
        int dx, dy;

        if (bw <= 0 || bh <= 0)
            continue;

        switch (box->type) {
        case BOX_FACE:    expand = cfg->roi_expand_face;    break;
        case BOX_PLATE:   expand = cfg->roi_expand_plate;   break;
        default:          expand = cfg->roi_expand_default; break;
        }

        dx = (bw * expand + 50) / 100;
        dy = (bh * expand + 50) / 100;

        x1 = clamp_int(box->x1 - dx, 0, w);
        y1 = clamp_int(box->y1 - dy, 0, h);
        x2 = clamp_int(box->x2 + dx, 0, w);
        y2 = clamp_int(box->y2 + dy, 0, h);

        if (x2 <= x1 || y2 <= y1)
            continue;

        for (y = y1; y < y2; y++)
            memset(mask + y * w + x1, 1, (size_t)(x2 - x1));
    }

    /* Dilate */
    if (cfg->roi_mask_dilate_iter > 0) {
        uint8_t *tmp = safe_malloc(w * h);
        int iter;
        for (iter = 0; iter < cfg->roi_mask_dilate_iter; iter++) {
            memcpy(tmp, mask, w * h);
            for (y = 1; y < h - 1; y++) {
                for (x = 1; x < w - 1; x++) {
                    int idx = y * w + x;
                    if (tmp[idx - 1] || tmp[idx + 1] ||
                        tmp[idx - w] || tmp[idx + w] || tmp[idx])
                        mask[idx] = 1;
                }
            }
        }
        free(tmp);
    }
}

/* ========================================================================
 *  Low-light estimation
 * ======================================================================== */

static int estimate_low_light(const uint8_t *y_plane, int w, int h, const BgFilterCfg *cfg)
{
    uint64_t sum = 0;
    int x, y;
    int count = w * h;
    int mean_y;

    for (y = 0; y < h; y++) {
        const uint8_t *row = y_plane + y * w;
        for (x = 0; x < w; x++)
            sum += row[x];
    }

    mean_y = (int)(sum / count);

    if (mean_y >= cfg->low_light_thr)
        return 0;
    if (mean_y >= cfg->strong_light_thr)
        return 1;
    return 2;
}

/* ========================================================================
 *  Gaussian blur on Y plane (separable, pure C)
 * ======================================================================== */

static void gaussian_blur_y(const uint8_t *src, uint8_t *dst,
                            int w, int h, int kernel_size)
{
    int *tmp = safe_malloc(w * h * sizeof(int));
    int x, y;

    if (kernel_size == 3) {
        /* Horizontal [1,2,1] */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int a = (x > 0) ? src[y * w + x - 1] : src[y * w + x];
                int b = src[y * w + x];
                int c = (x + 1 < w) ? src[y * w + x + 1] : src[y * w + x];
                tmp[y * w + x] = a + 2 * b + c;
            }
        }
        /* Vertical [1,2,1] / 16 */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int a = (y > 0) ? tmp[(y - 1) * w + x] : tmp[y * w + x];
                int b = tmp[y * w + x];
                int c = (y + 1 < h) ? tmp[(y + 1) * w + x] : tmp[y * w + x];
                int v = (a + 2 * b + c + 8) >> 4;
                dst[y * w + x] = (uint8_t)clamp_int(v, 0, 255);
            }
        }
    } else if (kernel_size == 5) {
        /* Horizontal [1,4,6,4,1] */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int a = (x > 1) ? src[y * w + x - 2] : src[y * w + 0];
                int b = (x > 0) ? src[y * w + x - 1] : src[y * w + 0];
                int c = src[y * w + x];
                int d = (x + 1 < w) ? src[y * w + x + 1] : src[y * w + w - 1];
                int e = (x + 2 < w) ? src[y * w + x + 2] : src[y * w + w - 1];
                tmp[y * w + x] = a + 4 * b + 6 * c + 4 * d + e;
            }
        }
        /* Vertical [1,4,6,4,1] / 256 */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int a = (y > 1) ? tmp[(y - 2) * w + x] : tmp[0 * w + x];
                int b = (y > 0) ? tmp[(y - 1) * w + x] : tmp[0 * w + x];
                int c = tmp[y * w + x];
                int d = (y + 1 < h) ? tmp[(y + 1) * w + x] : tmp[(h - 1) * w + x];
                int e = (y + 2 < h) ? tmp[(y + 2) * w + x] : tmp[(h - 1) * w + x];
                int v = (a + 4 * b + 6 * c + 4 * d + e + 128) >> 8;
                dst[y * w + x] = (uint8_t)clamp_int(v, 0, 255);
            }
        }
    } else {
        memcpy(dst, src, w * h);
    }

    free(tmp);
}

/* ========================================================================
 *  Temporal blend (simplified)
 * ======================================================================== */

static void temporal_blend(const uint8_t *curr, const uint8_t *prev,
                           uint8_t *out, const uint8_t *mask,
                           int w, int h, const BgFilterCfg *cfg)
{
    int alpha = cfg->temporal_alpha;
    int motion_thr = cfg->motion_diff_thr;
    int x, y;

    if (alpha < 0) alpha = 0;
    if (alpha > 100) alpha = 100;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int idx = y * w + x;
            int diff = (int)curr[idx] - (int)prev[idx];
            if (diff < 0) diff = -diff;

            if (mask[idx]) {
                out[idx] = curr[idx];  /* ROI: current */
            } else if (diff > motion_thr) {
                out[idx] = curr[idx];  /* Motion: current */
            } else {
                int v = (alpha * curr[idx] + (100 - alpha) * prev[idx] + 50) / 100;
                out[idx] = (uint8_t)clamp_int(v, 0, 255);
            }
        }
    }
}

/* ========================================================================
 *  ROI restore
 * ======================================================================== */

static void roi_restore(const uint8_t *orig, uint8_t *filtered,
                        const uint8_t *mask, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int idx = y * w + x;
            if (mask[idx])
                filtered[idx] = orig[idx];
        }
    }
}

/* ========================================================================
 *  Debug dump
 * ======================================================================== */

static void dump_pgm(const char *path, const uint8_t *data, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    int y;
    if (!fp) return;
    fprintf(fp, "P5\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++)
        fwrite(data + y * w, 1, w, fp);
    fclose(fp);
}

static void dump_mask_pgm(const char *path, const uint8_t *mask, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    int y, x;
    if (!fp) return;
    fprintf(fp, "P5\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t v = mask[y * w + x] ? 255 : 0;
            fwrite(&v, 1, 1, fp);
        }
    }
    fclose(fp);
}

static void ensure_dir(const char *dir)
{
    if (mkdir(dir, 0755) && errno != EEXIST)
        fprintf(stderr, "warn: mkdir %s failed: %s\n", dir, strerror(errno));
}

/* ========================================================================
 *  Main
 * ======================================================================== */

int main(int argc, char **argv)
{
    BgFilterCfg cfg = {
        .enable = 1,
        .low_light_thr = 80,
        .strong_light_thr = 50,
        .kernel_weak = 3,
        .kernel_strong = 5,
        .roi_expand_face = 15,
        .roi_expand_plate = 20,
        .roi_expand_default = 10,
        .roi_mask_dilate_iter = 1,
        .enable_temporal = 0,
        .temporal_alpha = 80,
        .motion_diff_thr = 12,
        .dump_debug = 0,
        .debug_dir = "/tmp/bg_filter_debug",
    };

    char *input_file = NULL;
    char *output_file = NULL;
    int width = 640, height = 360, frame_num = 10;
    int i;

    /* Parse args */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) input_file = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output_file = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) frame_num = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-low_thr") && i + 1 < argc) cfg.low_light_thr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-strong_thr") && i + 1 < argc) cfg.strong_light_thr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-weak_k") && i + 1 < argc) cfg.kernel_weak = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-strong_k") && i + 1 < argc) cfg.kernel_strong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dump_mask") && i + 1 < argc) cfg.dump_debug = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dump_dir") && i + 1 < argc) cfg.debug_dir = argv[++i];
        else if (!strcmp(argv[i], "-temporal") && i + 1 < argc) cfg.enable_temporal = atoi(argv[++i]);
    }

    if (!input_file || !output_file) {
        fprintf(stderr,
            "Usage: %s -i input.yuv -o output.yuv -w width -h height -n frames\n"
            "  [-low_thr 80] [-strong_thr 50] [-weak_k 3] [-strong_k 5]\n"
            "  [-dump_mask 0] [-dump_dir /tmp/debug] [-temporal 0]\n",
            argv[0]);
        return 1;
    }

    int y_size = width * height;
    int uv_size = width * height / 2;
    int frame_size = y_size + uv_size;

    FILE *fp_in = fopen(input_file, "rb");
    FILE *fp_out = fopen(output_file, "wb");
    if (!fp_in || !fp_out) {
        fprintf(stderr, "Failed to open input/output file\n");
        return 1;
    }

    uint8_t *frame_orig = safe_malloc(frame_size);
    uint8_t *frame_filtered = safe_malloc(frame_size);
    uint8_t *frame_spatial = safe_malloc(frame_size);
    uint8_t *prev_filtered = safe_malloc(frame_size);
    uint8_t *roi_mask = safe_malloc(width * height);
    int has_prev = 0;

    if (cfg.dump_debug)
        ensure_dir(cfg.debug_dir);

    printf("bg_filter_standalone: w=%d h=%d frames=%d\n", width, height, frame_num);
    printf("  low_thr=%d strong_thr=%d weak_k=%d strong_k=%d temporal=%d\n",
           cfg.low_light_thr, cfg.strong_light_thr,
           cfg.kernel_weak, cfg.kernel_strong, cfg.enable_temporal);

    for (i = 0; i < frame_num; i++) {
        if (fread(frame_orig, 1, frame_size, fp_in) != (size_t)frame_size) {
            fprintf(stderr, "EOF at frame %d\n", i);
            break;
        }

        /* 1. Get fake boxes */
        FrameBoxes *fb = get_fake_boxes(i, width, height);

        /* 2. Build ROI mask */
        build_roi_mask(roi_mask, width, height, fb, &cfg);

        /* 3. Low-light estimate */
        int level = estimate_low_light(frame_orig, width, height, &cfg);
        int mean_y = 0;
        {
            uint64_t sum = 0;
            int x, y;
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                    sum += frame_orig[y * width + x];
            mean_y = (int)(sum / (width * height));
        }

        if (level == 0) {
            /* Not low-light: copy as-is */
            memcpy(frame_filtered, frame_orig, frame_size);
            memcpy(prev_filtered, frame_orig, frame_size);
            has_prev = 1;
            printf("frame %d: mean_y=%d level=%d -> skip (not low-light)\n", i, mean_y, level);
        } else {
            int kernel = (level == 1) ? cfg.kernel_weak : cfg.kernel_strong;
            if (kernel < 3) kernel = 3;
            if (kernel > 5) kernel = 5;
            if (kernel % 2 == 0) kernel++;

            /* 4. Spatial filter on Y only */
            gaussian_blur_y(frame_orig, frame_spatial, width, height, kernel);
            /* Copy UV as-is */
            memcpy(frame_spatial + y_size, frame_orig + y_size, uv_size);

            /* 5. ROI restore */
            roi_restore(frame_orig, frame_spatial, roi_mask, width, height);

            /* 6. Temporal blend (optional) */
            if (cfg.enable_temporal && has_prev) {
                temporal_blend(frame_spatial, prev_filtered,
                               frame_filtered, roi_mask,
                               width, height, &cfg);
                /* ROI restore again after temporal */
                roi_restore(frame_orig, frame_filtered, roi_mask, width, height);
            } else {
                memcpy(frame_filtered, frame_spatial, frame_size);
            }

            memcpy(prev_filtered, frame_filtered, frame_size);
            has_prev = 1;

            printf("frame %d: boxes=%d mean_y=%d level=%d kernel=%d -> filtered\n",
                   i, fb->count, mean_y, level, kernel);
        }

        /* Write output */
        fwrite(frame_filtered, 1, frame_size, fp_out);

        /* Debug dump */
        if (cfg.dump_debug) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/frame_%06d_orig.pgm", cfg.debug_dir, i);
            dump_pgm(path, frame_orig, width, height);
            snprintf(path, sizeof(path), "%s/frame_%06d_mask.pgm", cfg.debug_dir, i);
            dump_mask_pgm(path, roi_mask, width, height);
            snprintf(path, sizeof(path), "%s/frame_%06d_filtered.pgm", cfg.debug_dir, i);
            dump_pgm(path, frame_filtered, width, height);
        }
    }

    printf("Done. Output: %s\n", output_file);

    free(frame_orig);
    free(frame_filtered);
    free(frame_spatial);
    free(prev_filtered);
    free(roi_mask);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
