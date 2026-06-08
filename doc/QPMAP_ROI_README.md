# QPMAP0 ROI Delta QP Support

This document describes the local QPMAP0 ROI delta QP changes added on top of
Rockchip MPP.

## Goal

The change adds a first-stage per-frame ROI delta QP path for encoder demos:

- lower QP for face regions to improve visual quality;
- lower QP more strongly for plate regions to improve OCR/readability;
- optionally raise background QP to compensate ROI bitrate cost;
- keep the whole-frame average delta QP close to zero when possible;
- keep original encoder behavior unchanged when the feature is disabled.

This is intended as a minimal validation path. It does not implement bitrate
feedback yet.

## Implementation Scope

The feature is implemented in the demo/util layer rather than the core encoder
path.

New files:

- `utils/mpp_enc_qpmap_roi_utils.c`
- `utils/mpp_enc_qpmap_roi_utils.h`

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

The demo enables this path for:

- H.264 / AVC
- H.265 / HEVC

If QPMAP ROI is enabled for another codec, init fails with an explicit error.

## Runtime Options

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

`utils/mpp_opt.c` was also updated so command line options can use either one
dash or two dashes, for example:

```text
-enable_qpmap_roi 1
--enable_qpmap_roi 1
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

### 6. Run ROI without background compensation

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
       qpmap_roi_demo/out_qpmap_roi_no_bg.h265
```

Note: real encoding requires Rockchip hardware encoder support. On an x86
machine, the project can build and the input/debug files can be prepared, but
`mpi_enc_test` cannot verify hardware QPMAP behavior without the Rockchip
kernel device.

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

- `type`: `face` or `plate`
- `x1`, `y1`, `x2`, `y2`

The `score` field is ignored by this first version.

Invalid or out-of-range coordinates are clamped. Boxes with `x2 <= x1` or
`y2 <= y1` are skipped.

If QPMAP ROI is enabled and the box file cannot be opened, cannot be parsed, or
contains no frame records, encoder setup fails with an explicit error.

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

## Debug Dump

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

## Example Command

Example for `mpi_enc_test`:

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

Use the same input and encoder settings with `--enable_qpmap_roi 0` for a
baseline comparison.

## Suggested Validation

Run at least three cases:

1. Baseline

   ```text
   --enable_qpmap_roi 0
   ```

2. ROI only

   ```text
   --enable_qpmap_roi 1
   --enable_bg_compensation 0
   ```

3. ROI plus background compensation

   ```text
   --enable_qpmap_roi 1
   --enable_bg_compensation 1
   ```

Check:

- encoder does not fail;
- output bitstream is generated;
- QPMAP debug txt matches expected ROI positions;
- face blocks use negative delta QP;
- plate blocks use stronger negative delta QP;
- background blocks use non-negative compensation when enabled;
- mean delta QP is close to zero for the compensation case;
- bitrate and subjective quality differ from baseline.

## Known Limitations

- No bitrate feedback loop yet.
- No temporal smoothing yet.
- No ROI expansion yet.
- No complexity-aware background allocation yet.
- The parser is intentionally lightweight and supports the expected JSON/JSONL
  ROI box format rather than a full JSON feature set.
