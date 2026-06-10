#!/usr/bin/env python3
"""
Visualize bg_filter results: side-by-side original vs filtered + ROI mask overlay.

Usage:
    python3 viz_bg_filter.py --debug_dir /tmp/bg_filter_debug --frame 0 --output /tmp/compare.png
"""

import argparse
import os
import numpy as np
from PIL import Image


def read_pgm(path):
    """Read a binary PGM file."""
    with open(path, 'rb') as f:
        header = f.readline().decode().strip()
        if header != 'P5':
            raise ValueError(f"Not a binary PGM: {path}")
        # Skip comments
        while True:
            line = f.readline().decode().strip()
            if not line.startswith('#'):
                break
        width, height = map(int, line.split())
        maxval = int(f.readline().decode().strip())
        data = np.frombuffer(f.read(), dtype=np.uint8)
        return data.reshape((height, width))


def y_to_rgb(y):
    """Grayscale Y to RGB."""
    return np.stack([y, y, y], axis=-1).astype(np.uint8)


def overlay_mask(rgb, mask, color=(255, 0, 0), alpha=0.3):
    """Overlay red mask on RGB image."""
    out = rgb.copy().astype(np.float32)
    mask_idx = mask > 0
    for c in range(3):
        out[mask_idx, c] = out[mask_idx, c] * (1 - alpha) + color[c] * alpha
    return out.astype(np.uint8)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug_dir", default="/tmp/bg_filter_debug")
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--output", default="/tmp/bg_filter_compare.png")
    args = parser.parse_args()

    prefix = f"frame_{args.frame:06d}"
    orig_path = os.path.join(args.debug_dir, f"{prefix}_orig.pgm")
    filtered_path = os.path.join(args.debug_dir, f"{prefix}_filtered.pgm")
    mask_path = os.path.join(args.debug_dir, f"{prefix}_mask.pgm")

    if not os.path.exists(orig_path):
        print(f"Missing: {orig_path}")
        return

    orig = read_pgm(orig_path)
    filtered = read_pgm(filtered_path)
    mask = read_pgm(mask_path)

    h, w = orig.shape

    # Build comparison image: [orig | orig+mask | filtered | filtered+mask]
    orig_rgb = y_to_rgb(orig)
    filtered_rgb = y_to_rgb(filtered)

    orig_masked = overlay_mask(orig_rgb, mask, color=(255, 0, 0), alpha=0.3)
    filtered_masked = overlay_mask(filtered_rgb, mask, color=(0, 255, 0), alpha=0.3)

    # Create canvas
    canvas = np.zeros((h, w * 4, 3), dtype=np.uint8)
    canvas[:, 0*w:1*w] = orig_rgb
    canvas[:, 1*w:2*w] = orig_masked
    canvas[:, 2*w:3*w] = filtered_rgb
    canvas[:, 3*w:4*w] = filtered_masked

    # Add labels
    img = Image.fromarray(canvas)
    from PIL import ImageDraw, ImageFont
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16)
    except:
        font = ImageFont.load_default()

    labels = ["Original", "Original + ROI", "Filtered", "Filtered + ROI"]
    for i, label in enumerate(labels):
        draw.text((i * w + 10, 10), label, fill=(255, 255, 0), font=font)

    img.save(args.output)
    print(f"Saved comparison to {args.output}  ({w*4}x{h})")

    # Also print stats
    diff = np.abs(orig.astype(np.int16) - filtered.astype(np.int16))
    bg_mask = (mask == 0)
    roi_mask = (mask > 0)
    print(f"  Background mean diff: {diff[bg_mask].mean():.2f}  (should be > 0, noise reduced)")
    print(f"  ROI mean diff:        {diff[roi_mask].mean():.2f}  (should be ~0, protected)")
    print(f"  Background variance:  orig={orig[bg_mask].var():.2f} filtered={filtered[bg_mask].var():.2f}")
    print(f"  ROI variance:         orig={orig[roi_mask].var():.2f} filtered={filtered[roi_mask].var():.2f}")


if __name__ == "__main__":
    main()
