# QPMAP0 ROI Delta QP + ROI-Protected Background Filtering

This document describes the local QPMAP0 ROI delta QP and ROI-protected
background filtering changes added on top of Rockchip MPP.

## Goal

### QPMAP0 ROI Delta QP

The change adds a first-stage per-frame ROI delta QP path for encoder demos:

- lower QP for face regions to improve visual quality;
- lower QP more strongly for plate regions to improve OCR/readability;
- optionally raise background QP to compensate ROI bitrate cost;
- keep the whole-frame average delta QP close to zero when possible;
- keep original encoder behavior unchanged when the feature is disabled.

### ROI-Protected Background Filtering

A lightweight spatial background filter for night / low-light scenes:

- detect low-light condition by mean Y luminance;
- apply Gaussian blur to background regions to suppress noise;
- protect ROI regions (face, plate, vehicle, person) from blur;
- optionally apply temporal blend on static background (default off);
- reduce background complexity to save encoding bits;
- keep original encoder behavior unchanged when the feature is disabled.

Both features are intended as minimal validation paths. They do not implement
bitrate feedback yet.

## Implementation Scope

Both features are implemented in the demo/util layer rather than the core encoder
path.

### QPMAP ROI

New files:

- `utils/mpp_enc_qpmap_roi_utils.c`
- `utils/mpp_enc_qpmap_roi_utils.h`

### Background Filter

New files:

- `utils/mpp_enc_bg_filter_utils.c`
- `utils/mpp_enc_bg_filter_utils.h`

### Shared modified files

Modified files:

- `utils/CMakeLists.txt`
- `utils/mpi_enc_utils.h`
- `utils/mpi_enc_utils.c`
- `utils/mpp_enc_args.c`
- `utils/mpp_opt.c`
- `test/mpi_enc_test.c`
- `test/mpi_enc_mt_test.c`

`KEY_QPMAP0` is attached to each input `MppFrame` through frame metadata before
the frame is submitted to the encoder:

```c
MppMeta meta = mpp_frame_get_meta(frame);
mpp_meta_set_buffer(meta, KEY_QPMAP0, qpmap_buf);
```

Background filtering is applied to the raw frame buffer **before** the frame is
wrapped into `MppFrame` and submitted to the encoder. The detection boxes used
for ROI mask generation come from the same JSONL file as QPMAP ROI (or a
separate file if configured).

## QPMAP0 Layout

This implementation follows the current MPP code and `inc/mpp_meta.h`.

`KEY_QPMAP0` is not written as raw signed `int16_t` delta QP values. Each 16x16
block is packed as a 16-bit `Vepu541RoiCfg`-compatible value:

- `qp_area_en = 1`
- `qp_adj_mode = 0`
- `qp_adj = delta_qp`

`qp_adj_mode = 0` means relative QP adjustment.

The debug dump still writes plain signed delta QP values so the map can be
checked by humans.

## Supported Codecs

### QPMAP ROI

The demo enables this path for:

- H.264 / AVC
- H.265 / HEVC

If QPMAP ROI is enabled for another codec, init fails with an explicit error.

### Background Filter

The background filter is codec-agnostic. It operates on raw YUV/RGB frames
before encoding. Supported input formats:

- YUV420SP / NV12 (Y plane filtered, UV copied as-is)
- YUV420P (Y plane filtered, U/V copied as-is)
- RGB888 / BGR888
- RGBA8888 / BGRA8888 / ARGB8888 / ABGR8888

Other formats are passed through without filtering.

## Runtime Options

### QPMAP ROI

Default behavior is disabled:

```text
--enable_qpmap_roi 0
```

Added options:

```text
--enable_qpmap_roi 0|1
--roi_boxes_json path/to/boxes.jsonl
--face_delta_qp -3
--face_expand_blocks 1
--plate_delta_qp -6
--delta_qp_min -8
--delta_qp_max 4
--enable_bg_compensation 1
--bg_delta_qp_max 4
--dump_qpmap_debug 0|1
--qpmap_debug_dir path/to/debug_dir
```

### Background Filter

Default behavior is disabled:

```text
--enable_bg_filter 0
```

Added options:

```text
--enable_bg_filter 0|1
--bg_filter_type 0|1              (0=gaussian, 1=bilateral reserved)
--bg_filter_low_light_thr 80      (mean Y threshold for weak filter)
--bg_filter_strong_light_thr 50   (mean Y threshold for strong filter)
--bg_filter_kernel_weak 3         (weak filter kernel size)
--bg_filter_kernel_strong 5       (strong filter kernel size)
--roi_expand_face 15              (face box expand ratio * 100, 15=0.15)
--roi_expand_plate 20             (plate box expand ratio * 100, 20=0.20)
--roi_expand_default 10           (other box expand ratio * 100, 10=0.10)
--roi_mask_dilate_iter 1          (ROI mask dilate iterations)
--enable_temporal_bg_filter 0|1   (temporal blend, default off)
--temporal_alpha 80               (current frame weight * 100, 80=0.8)
--motion_diff_thr 12              (motion gate threshold)
--dump_bg_filter_debug 0|1        (dump debug images)
--bg_filter_debug_dir path/to/dir
--bg_filter_boxes_json path/to/boxes.jsonl  (optional, falls back to roi_boxes_json)
```

`utils/mpp_opt.c` was also updated so command line options can use either one
dash or two dashes, for example:

```text
-enable_qpmap_roi 1
--enable_qpmap_roi 1
-enable_bg_filter 1
--enable_bg_filter 1
```

## Quick Start

This quick start prepares a short raw NV12 input, uses the included fake JSONL
ROI boxes, and runs both baseline and QPMAP ROI encoding.

### 1. Build

Build MPP with CMake:

```bash
cmake -S . -B build/qpmap_roi -DCMAKE_BUILD_TYPE=Debug
cmake --build build/qpmap_roi -j4
```

The encoder demo will be generated at:

```text
build/qpmap_roi/test/mpi_enc_test
```

### 2. Prepare a 10-second NV12 YUV input

Use any input video. This example converts the first 10 seconds to
`640x360`, `10 fps`, NV12 raw video:

```bash
mkdir -p qpmap_roi_demo

ffmpeg -y \
  -ss 0 \
  -t 10 \
  -i input.mp4 \
  -vf "scale=640:360,fps=10" \
  -pix_fmt nv12 \
  -f rawvideo \
  qpmap_roi_demo/input_10s_640x360_nv12.yuv
```

The expected size for 100 frames of `640x360` NV12 is:

```text
640 * 360 * 3 / 2 * 100 = 34560000 bytes
```

Check it:

```bash
ls -l qpmap_roi_demo/input_10s_640x360_nv12.yuv
```

### 3. Use the included fake ROI JSONL

This repository includes a ready-to-run overlap test file:

```text
test/qpmap_roi_overlap_boxes.jsonl
```

It contains two frame records:

- frame 0: two face boxes and two plate boxes, with face/plate overlap;
- frame 1: two overlapping plate boxes and two face boxes.

The boxes are designed for `640x360` input. Frames that are not listed in the
JSONL file will get an all-zero QPMAP.

Face boxes are expanded by one 16x16 block on each side by default. This helps
when the detector box only covers the eyebrow-to-mouth area instead of the
whole face/head region. Set `--face_expand_blocks 0` to disable this expansion.

### 4. Run baseline

```bash
./build/qpmap_roi/test/mpi_enc_test \
  -i qpmap_roi_demo/input_10s_640x360_nv12.yuv \
  -o qpmap_roi_demo/out_base.h265 \
  -w 640 \
  -h 360 \
  -f 0 \
  -n 100 \
  --enable_qpmap_roi 0
```

### 5. Run QPMAP ROI

```bash
./build/qpmap_roi/test/mpi_enc_test \
  -i qpmap_roi_demo/input_10s_640x360_nv12.yuv \
  -o qpmap_roi_demo/out_qpmap_roi.h265 \
  -w 640 \
  -h 360 \
  -f 0 \
  -n 100 \
  --enable_qpmap_roi 1 \
  --roi_boxes_json test/qpmap_roi_overlap_boxes.jsonl \
  --face_delta_qp -3 \
  --face_expand_blocks 1 \
  --plate_delta_qp -6 \
  --enable_bg_compensation 1 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_roi_demo/qpmap_debug
```

After the run, inspect:

```bash
ls qpmap_roi_demo/qpmap_debug
sed -n '8,13p' qpmap_roi_demo/qpmap_debug/frame_000000_qpmap.txt
sed -n '6,13p' qpmap_roi_demo/qpmap_debug/frame_000001_qpmap.txt
```

Expected behavior:

- face-only blocks are `-3`;
- plate-only blocks are `-6`;
- face/plate overlap blocks are `-6`;
- background blocks are non-negative when background compensation is enabled.

### 6. Run ROI-protected background filter

```bash
./build/qpmap_roi/test/mpi_enc_test \
  -i qpmap_roi_demo/input_10s_640x360_nv12.yuv \
  -o qpmap_roi_demo/out_bg_filter.h265 \
  -w 640 \
  -h 360 \
  -f 0 \
  -n 100 \
  --enable_bg_filter 1 \
  --bg_filter_boxes_json test/qpmap_roi_overlap_boxes.jsonl \
  --bg_filter_low_light_thr 80 \
  --bg_filter_strong_light_thr 50 \
  --bg_filter_kernel_weak 3 \
  --bg_filter_kernel_strong 5 \
  --roi_expand_face 15 \
  --roi_expand_plate 20 \
  --roi_expand_default 10 \
  --roi_mask_dilate_iter 1 \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir qpmap_roi_demo/bg_filter_debug
```

After the run, inspect:

```bash
ls qpmap_roi_demo/bg_filter_debug
```

Expected files per frame:

- `frame_000000_orig.pgm` — original Y plane
- `frame_000000_mask.pgm` — ROI mask (white = protected)
- `frame_000000_filtered.pgm` — filtered Y plane

Expected behavior:

- background noise is visibly reduced;
- ROI regions (face, plate) remain sharp and unchanged;
- mean Y of the frame determines whether filtering is applied.

### 7. Run QPMAP ROI + background filter together

```bash
./build/qpmap_roi/test/mpi_enc_test \
  -i qpmap_roi_demo/input_10s_640x360_nv12.yuv \
  -o qpmap_roi_demo/out_combined.h265 \
  -w 640 \
  -h 360 \
  -f 0 \
  -n 100 \
  --enable_qpmap_roi 1 \
  --enable_bg_filter 1 \
  --roi_boxes_json test/qpmap_roi_overlap_boxes.jsonl \
  --face_delta_qp -3 \
  --plate_delta_qp -6 \
  --enable_bg_compensation 1 \
  --bg_filter_low_light_thr 80 \
  --bg_filter_kernel_weak 3 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_roi_demo/qpmap_debug_combined \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir qpmap_roi_demo/bg_filter_debug_combined
```

### 8. Run ROI without background compensation

This case is useful to see the pure ROI bitrate cost:

```bash
./build/qpmap_roi/test/mpi_enc_test \
  -i qpmap_roi_demo/input_10s_640x360_nv12.yuv \
  -o qpmap_roi_demo/out_qpmap_roi_no_bg.h265 \
  -w 640 \
  -h 360 \
  -f 0 \
  -n 100 \
  --enable_qpmap_roi 1 \
  --roi_boxes_json test/qpmap_roi_overlap_boxes.jsonl \
  --enable_bg_compensation 0 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_roi_demo/qpmap_debug_no_bg
```

Compare output sizes:

```bash
ls -lh qpmap_roi_demo/out_base.h265 \
       qpmap_roi_demo/out_qpmap_roi.h265 \
       qpmap_roi_demo/out_qpmap_roi_no_bg.h265 \
       qpmap_roi_demo/out_bg_filter.h265 \
       qpmap_roi_demo/out_combined.h265
```

Note: real encoding requires Rockchip hardware encoder support. On an x86
machine, the project can build and the input/debug files can be prepared, but
`mpi_enc_test` cannot verify hardware QPMAP behavior without the Rockchip
kernel device.

A standalone C test program is provided for off-device validation:

```text
test/test_bg_filter_standalone.c
test/test_bg_filter_standalone.py   (YUV generator)
test/viz_bg_filter.py               (visualization)
```

Build and run the standalone test:

```bash
cd test
gcc -O2 -o test_bg_filter_standalone test_bg_filter_standalone.c -lm
python3 test_bg_filter_standalone.py --output /tmp/night_640x360.yuv --width 640 --height 360 --frames 10
./test_bg_filter_standalone -i /tmp/night_640x360.yuv -o /tmp/filtered.yuv -w 640 -h 360 -n 10 \
    -low_thr 80 -strong_thr 50 -weak_k 3 -strong_k 5 -dump_mask 1 -dump_dir /tmp/debug
python3 viz_bg_filter.py --debug_dir /tmp/debug --frame 0 --output /tmp/compare.png
```

## Input Box Format

JSONL example:

```json
{"frame_idx": 0, "boxes": [{"type": "face", "x1": 100, "y1": 100, "x2": 220, "y2": 240, "score": 0.99}]}
{"frame_idx": 1, "boxes": [{"type": "plate", "x1": 300, "y1": 400, "x2": 430, "y2": 450, "score": 0.95}]}
{"frame_idx": 2, "boxes": [{"type": "face", "x1": 100, "y1": 100, "x2": 220, "y2": 240}, {"type": "plate", "x1": 300, "y1": 400, "x2": 430, "y2": 450}]}
```

JSON object with a `frames` array is also accepted as long as each frame object
contains:

- `frame_idx`
- `boxes`

Each box supports:

- `type`: `face`, `plate`, `vehicle`, `person`, or any other string
- `x1`, `y1`, `x2`, `y2`

The `score` field is ignored by this first version.

Invalid or out-of-range coordinates are clamped. Boxes with `x2 <= x1` or
`y2 <= y1` are skipped.

If QPMAP ROI is enabled and the box file cannot be opened, cannot be parsed, or
contains no frame records, encoder setup fails with an explicit error.

If background filter is enabled and the box file is missing, the filter still
works but generates an all-zero ROI mask (the whole frame may be filtered in
low-light scenes).

## Delta QP Policy

Default ROI deltas:

```text
face_delta_qp  = -3
plate_delta_qp = -6
```

Default face expansion:

```text
face_expand_blocks = 1
```

The expansion is applied after converting the face box to the 16x16 QPMAP grid:

```text
block_x1 -= face_expand_blocks
block_y1 -= face_expand_blocks
block_x2 += face_expand_blocks
block_y2 += face_expand_blocks
```

Then the expanded block range is clamped to the frame boundary. Plate boxes are
not expanded by this option.

Plate regions have stronger protection than face regions. When regions overlap,
the smaller delta QP is used.

All generated delta QP values are clamped to:

```text
[delta_qp_min, delta_qp_max]
```

Default range:

```text
[-8, 4]
```

## Background Compensation

When background compensation is enabled, the generator computes:

```text
neg_sum = sum(delta_qp over ROI blocks)
bg_count = number of non-ROI blocks
bg_delta = round(-neg_sum / bg_count)
```

Then `bg_delta` is clamped to:

```text
[0, bg_delta_qp_max]
```

and assigned to all non-ROI blocks.

If there is no ROI or no background area, no compensation is applied.

This only makes the mean delta QP close to zero. It does not guarantee bitrate
will match baseline because QP and bits are not linear.

## Background Filter Algorithm

### Low-Light Detection

For each frame, the filter computes the mean luminance (Y plane):

```text
mean_y = mean(Y plane)
```

Then decides the filter level:

```text
mean_y >= low_light_thr (80)      -> level 0, no filter
strong_light_thr (50) <= mean_y < low_light_thr (80) -> level 1, weak filter
mean_y < strong_light_thr (50)    -> level 2, strong filter
```

### Spatial Filter

The spatial filter is a separable Gaussian blur implemented in pure C:

- **Level 1 (weak)**: 3x3 kernel, separable `[1, 2, 1] / 4`
- **Level 2 (strong)**: 5x5 kernel, separable `[1, 4, 6, 4, 1] / 16`

For YUV420SP / NV12 / YUV420P, only the Y plane is filtered. UV planes are
copied unchanged to avoid color distortion.

For RGB/BGR packed formats, all channels are filtered.

### ROI Mask Generation

For each detection box:

1. Determine expand ratio by type:
   - `face`:    `roi_expand_face`    (default 0.15)
   - `plate`:   `roi_expand_plate`   (default 0.20)
   - `others`:  `roi_expand_default` (default 0.10)

2. Expand the box:
   ```text
   dx = expand * box_width
   dy = expand * box_height
   x1 -= dx;  y1 -= dy
   x2 += dx;  y2 += dy
   ```

3. Clamp to image boundaries.

4. Fill the expanded region into a per-pixel mask (1 = protected, 0 = background).

5. Apply 3x3 cross dilate for `roi_mask_dilate_iter` iterations (default 1).

### ROI Protection

After Gaussian blur, the filter copies original pixels back into ROI regions:

```text
output[mask == 1] = original[mask == 1]
```

This guarantees that face, plate, and other protected objects remain completely
unblurred.

### Temporal Blend (Optional)

When `enable_temporal_bg_filter` is set:

1. Save the previous filtered frame.
2. Compute per-pixel absolute difference between current and previous frame.
3. Classify each pixel:
   - `mask == 1` (ROI): always use current frame
   - `diff > motion_diff_thr` (motion): use current frame
   - otherwise (static background): temporal blend
     ```text
     output = alpha * current + (1 - alpha) * previous
     ```
4. Restore ROI from original again after blend.

Default `alpha = 0.8`, `motion_diff_thr = 12`.

Temporal blend is disabled by default because it can cause ghosting on moving
objects if the motion gate is not strict enough.

## Debug Dump

### QPMAP ROI

Enable debug dump:

```text
--dump_qpmap_debug 1
--qpmap_debug_dir qpmap_debug
```

For each frame, a text file is written:

```text
qpmap_debug/frame_000000_qpmap.txt
qpmap_debug/frame_000001_qpmap.txt
```

Each text file contains one signed delta QP value per 16x16 block.

Example:

```text
0 0 1 1 1
0 -3 -3 1 1
0 -3 -6 -6 1
```

The log also prints per-frame stats:

```text
frame_idx
face box count
plate box count
ROI block count
background block count
min / max / mean delta QP
background delta QP
whether KEY_QPMAP0 is set
```

### Background Filter

Enable debug dump:

```text
--dump_bg_filter_debug 1
--bg_filter_debug_dir bg_filter_debug
```

For each frame, PGM images are written:

```text
bg_filter_debug/frame_000000_orig.pgm      (original Y plane)
bg_filter_debug/frame_000000_mask.pgm      (ROI mask, white=protected)
bg_filter_debug/frame_000000_filtered.pgm  (filtered Y plane)
```

The log also prints per-frame stats:

```text
frame_idx
box count
mean_y
low_light_level
filter_kernel
roi_area_ratio
whether_temporal_enabled
```

## Example Command

### QPMAP ROI only

```bash
./mpi_enc_test \
  -i input.yuv \
  -o out_roi.h265 \
  -w 1920 \
  -h 1080 \
  -f 0 \
  -t 16777220 \
  --enable_qpmap_roi 1 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -3 \
  --face_expand_blocks 1 \
  --plate_delta_qp -6 \
  --enable_bg_compensation 1 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_debug
```

### Background filter only

```bash
./mpi_enc_test \
  -i input.yuv \
  -o out_bg_filter.h265 \
  -w 1920 \
  -h 1080 \
  -f 0 \
  -t 16777220 \
  --enable_bg_filter 1 \
  --bg_filter_boxes_json boxes.jsonl \
  --bg_filter_low_light_thr 80 \
  --bg_filter_strong_light_thr 50 \
  --bg_filter_kernel_weak 3 \
  --bg_filter_kernel_strong 5 \
  --roi_expand_face 15 \
  --roi_expand_plate 20 \
  --roi_expand_default 10 \
  --roi_mask_dilate_iter 1 \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir bg_filter_debug
```

### Combined (QPMAP ROI + background filter)

```bash
./mpi_enc_test \
  -i input.yuv \
  -o out_combined.h265 \
  -w 1920 \
  -h 1080 \
  -f 0 \
  -t 16777220 \
  --enable_qpmap_roi 1 \
  --enable_bg_filter 1 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -3 \
  --plate_delta_qp -6 \
  --enable_bg_compensation 1 \
  --bg_filter_low_light_thr 80 \
  --bg_filter_kernel_weak 3 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_debug \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir bg_filter_debug
```

Use the same input and encoder settings with `--enable_qpmap_roi 0` and
`--enable_bg_filter 0` for a baseline comparison.

## Suggested Validation

Run at least five cases:

1. Baseline

   ```text
   --enable_qpmap_roi 0
   --enable_bg_filter 0
   ```

2. ROI only

   ```text
   --enable_qpmap_roi 1
   --enable_bg_compensation 0
   --enable_bg_filter 0
   ```

3. ROI plus background compensation

   ```text
   --enable_qpmap_roi 1
   --enable_bg_compensation 1
   --enable_bg_filter 0
   ```

4. Background filter only

   ```text
   --enable_qpmap_roi 0
   --enable_bg_filter 1
   ```

5. Combined (ROI + background filter)

   ```text
   --enable_qpmap_roi 1
   --enable_bg_compensation 1
   --enable_bg_filter 1
   ```

Check QPMAP ROI:

- encoder does not fail;
- output bitstream is generated;
- QPMAP debug txt matches expected ROI positions;
- face blocks use negative delta QP;
- plate blocks use stronger negative delta QP;
- background blocks use non-negative compensation when enabled;
- mean delta QP is close to zero for the compensation case;
- bitrate and subjective quality differ from baseline.

Check background filter:

- encoder does not fail;
- output bitstream is generated;
- debug PGM shows background noise reduced in filtered frames;
- ROI regions in debug mask match detection boxes (expanded and dilated);
- ROI pixels in filtered frame are identical to original (mean diff = 0);
- when mean Y >= low_light_thr, frames pass through unmodified;
- when temporal blend is enabled, static background is smoother but moving
  objects show no ghosting.

Compare bitrates across all five cases to understand the interaction between
ROI QP reduction and background filtering.

## Known Limitations

### QPMAP ROI

- No bitrate feedback loop yet.
- No temporal smoothing yet.
- No ROI expansion yet (face_expand_blocks is a grid expansion, not pixel-level).
- No complexity-aware background allocation yet.
- The parser is intentionally lightweight and supports the expected JSON/JSONL
  ROI box format rather than a full JSON feature set.

### Background Filter

- No motion compensation (no block matching, no MV alignment).
- Temporal blend is simplified and may cause ghosting if motion gate is too
  lenient; it is disabled by default.
- Only Gaussian blur is implemented; bilateral filter is reserved for future.
- For YUV422 and other packed formats, the filter passes through unchanged
  to avoid format complexity.
- The filter operates on CPU; no hardware acceleration yet.
- No adaptive kernel size based on local noise level yet.
- No interaction with QPMAP module (background filter does not adjust QP
  based on filtered strength).
