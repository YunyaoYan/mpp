# JPEG ROI coefficient-domain recompression

## Scope

This prototype accepts an existing DCT-based JPEG, reads its quantized DCT
coefficients with libjpeg-turbo, preserves coefficients covered by ROI
rectangles, and sparsifies AC coefficients outside the ROI. It then writes a
normal JPEG with the original quantization tables.

The path runs on the CPU. It does not use the RK3588 VEPU2 JPEG encoder and
does not feed DCT coefficients into MPP hardware.

## Bitstream compatibility

- JPEG quantization tables are copied from the input.
- Protected ROI blocks are not modified.
- Non-ROI DC coefficients are not modified.
- Only non-ROI AC coefficient values are changed.
- APP/COM markers are copied by default.
- The output uses standard JPEG syntax and does not contain an ROI map.

Any normal JPEG decoder should decode the result. Decoder compatibility still
needs to be verified on each product decoder, including MPP JPEG decode.

## Dependency and build

The optional test target needs libjpeg-turbo development headers and library.
The core MPP libraries do not acquire a libjpeg dependency.

```sh
cmake -S . -B build-roi \
  -DBUILD_TEST=OFF \
  -DMPP_JPEG_ROI_RECOMPRESS_TEST=ON
cmake --build build-roi --target mpp_jpeg_roi_recompress_test
```

For an RK3588 cross build, libjpeg-turbo must be available in the target
sysroot. Use the existing MPP AArch64 toolchain file and ensure `find_package`
resolves the target library rather than a host library.

## CLI

```sh
mpp_jpeg_roi_recompress_test \
  -i input.jpg \
  -o output.jpg \
  -r 160,120,320,240,255 \
  -s 48 \
  -m 16 \
  -f 16
```

Options:

- `--roi x,y,w,h[,importance]`: repeatable rectangle; default importance 255.
- `--strength 0..63`: exact background coefficient sparsification strength.
- `--margin pixels`: expand each ROI before block protection.
- `--feather pixels`: gradually reduce protection outside the expanded ROI.
- `--target-bytes bytes`: search the lowest strength that meets the target.
- `--no-optimize`: disable output Huffman optimization.
- `--drop-markers`: do not copy APP/COM markers.

When `--target-bytes` is used without `--strength`, the search ceiling is 63.
When both are used, `--strength` is the search ceiling.

### ROI rectangle semantics

The `--roi` argument uses image pixel coordinates:

```text
x,y,width,height[,importance]
```

- `x,y` is the top-left corner, with `(0,0)` at the image top-left.
- `width,height` must be positive.
- The geometric rectangle is `[x, x + width) x [y, y + height)`.
- `importance` is in `[0,255]` and defaults to 255 when omitted.
- `--roi` may be repeated. If several regions affect one DCT block, the
  highest importance is used.

For example, `--roi 100,80,400,300,255` fully protects the rectangle from
`x=100` through `499` and `y=80` through `379`.

The library accepts the same data through an array of `MppJpegRoiRect`.
Parsing detector-specific JSON or tensor output is intentionally left to the
caller.

### Margin and feather

`--margin N` expands every ROI by `N` image pixels on all four sides before
block protection. Expansion is clipped to the image boundary. For example,
`--roi 100,80,400,300,255 --margin 16` produces an expanded rectangle of
approximately `[84,516) x [64,396)`.

`--feather N` adds a transition band outside the expanded rectangle. Inside
the expanded rectangle the requested importance is used. Across the next `N`
pixels, importance decreases approximately linearly toward zero, so
background filtering increases gradually rather than changing abruptly.
`--feather 0` disables this transition and creates a hard boundary.

Protection is evaluated per JPEG DCT block, not per pixel. A block that
overlaps an expanded ROI receives that ROI's importance. Chroma subsampling
also means that a Cb/Cr block can cover a larger image area than an 8x8 luma
block. The effective protected area can therefore extend beyond the exact
pixel rectangle.

## Library API

The reusable API is declared in:

```text
utils/mpp_jpeg_roi_recompress.h
```

The output buffer returned by `mpp_jpeg_roi_recompress()` must be released
with `mpp_jpeg_roi_recompress_free()`.

## Strength behavior

Strength is an encoder-private control, not a JPEG Quality or video delta-QP:

- strength 0: coefficients are unchanged; only marker/Huffman serialization
  may change the file bytes;
- higher strength: progressively larger AC dead-zone and lower high-frequency
  cutoff outside the ROI;
- strength 63: strongest current background filtering;
- protected blocks keep all input quantized coefficients at every strength.

The global strength is reduced by ROI importance for every component block:

```text
effective_strength = background_strength * (255 - importance) / 255
```

Consequently:

- importance 255 gives effective strength 0 and preserves the block;
- importance 128 applies approximately half the background strength;
- importance 0 applies the full background strength.

For an effective strength greater than zero, the DC coefficient is always
kept. AC coefficients are processed in JPEG zig-zag order using two controls:

1. A high-frequency cutoff moves from zig-zag position 64 toward position 8
   as strength increases. Coefficients at or above the cutoff are set to zero.
2. Below the cutoff, a strength- and frequency-dependent dead-zone sets small
   coefficient magnitudes to zero.

At background strength 48 with importance 0, the cutoff is zig-zag position
21: positions 21 through 63 are cleared, and small coefficients below position
21 may also be cleared. At strength 63, positions 8 through 63 are cleared.
These are coefficient-sparsification levels; they must not be interpreted as
JPEG Quality 48/63 or video QP values.

When `--target-bytes` is set, the implementation searches for the lowest
strength, up to the configured ceiling, whose output meets the requested byte
size. The result reports both the selected strength and whether the target was
met.

## Relationship to video codec ROI

The rate-allocation policy is similar to video codec ROI: preserve important
regions and spend fewer bits on background detail. The mechanism is different:

- a video codec normally consumes a macroblock/CTU QP map during encoding and
  performs transform and quantization with different QPs;
- this prototype accepts an already encoded JPEG, reads its quantized DCT
  coefficients, leaves protected coefficients unchanged, sparsifies
  background AC coefficients, and entropy-encodes a new standard JPEG;
- it does not send a QP map to MPP hardware and does not decode the JPEG to
  pixels for a full pixel-domain re-encode.

For the example below, the ROI is expanded by 16 pixels, followed by a
16-pixel transition band, while the remaining background uses strength 48:

```sh
mpp_jpeg_roi_recompress_test \
  -i input.jpg \
  -o output.jpg \
  -r 100,80,400,300,255 \
  -s 48 \
  -m 16 \
  -f 16
```

## Known first-version limitations

- Only existing DCT JPEG input is supported.
- Progressive input may be serialized with a different scan organization.
- Target-size search reparses the input for each trial and is not optimized.
- ROI boxes are provided through the API/CLI; detector JSON integration is not
  included yet.
- Coefficient protection is block-based. Chroma subsampling can make the
  protected area extend beyond the exact rectangle.
- The current heuristic is a dead-zone plus high-frequency cutoff, not
  MozJPEG weighted trellis RDO.
