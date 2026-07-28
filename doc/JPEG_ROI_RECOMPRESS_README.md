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
