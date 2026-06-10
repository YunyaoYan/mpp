#!/usr/bin/env python3
"""
Generate a synthetic night-scene YUV420SP (NV12) video for bg_filter demo.

Usage:
    python3 gen_night_yuv.py --output /tmp/night_640x360.yuv --width 640 --height 360 --frames 10
"""

import argparse
import numpy as np
import os


def yuv420sp_to_yuv_planes(yuv_bytes, width, height):
    """Split NV12 bytes into Y and UV planes."""
    y_size = width * height
    uv_size = width * height // 2
    y = np.frombuffer(yuv_bytes[:y_size], dtype=np.uint8).reshape((height, width))
    uv = np.frombuffer(yuv_bytes[y_size:y_size + uv_size], dtype=np.uint8).reshape((height // 2, width))
    return y, uv


def yuv_planes_to_yuv420sp(y, uv):
    """Merge Y and UV planes into NV12 bytes."""
    return y.tobytes() + uv.tobytes()


def generate_night_frame(width, height, frame_idx, noise_sigma=8.0):
    """
    Generate one synthetic night frame:
      - Dark background (mean ~40)
      - Some brighter regions (street lights, walls)
      - Add Gaussian noise
      - Optionally place a bright "face" blob and a "plate" blob
    """
    # Dark base
    y = np.full((height, width), 35, dtype=np.float32)

    # Add some gradient / structure (simulating street scene)
    xx, yy = np.meshgrid(np.arange(width), np.arange(height))
    # A "wall" on the left
    y += 20 * np.exp(-((xx - width * 0.2) ** 2) / (2 * (width * 0.15) ** 2))
    # A "street light" glow at top-right
    y += 40 * np.exp(-((xx - width * 0.8) ** 2 + (yy - height * 0.15) ** 2) / (2 * (width * 0.1) ** 2))
    # Ground reflection
    y += 15 * np.exp(-((yy - height * 0.9) ** 2) / (2 * (height * 0.1) ** 2))

    # Add Gaussian noise
    noise = np.random.normal(0, noise_sigma, (height, width))
    y += noise

    # Add a "face" blob (brighter, smoother)
    face_cx, face_cy = int(width * 0.35), int(height * 0.35)
    face_w, face_h = int(width * 0.12), int(height * 0.18)
    y += 60 * np.exp(-((xx - face_cx) ** 2 / (2 * (face_w) ** 2) +
                       (yy - face_cy) ** 2 / (2 * (face_h) ** 2)))

    # Add a "plate" blob (small, bright)
    plate_cx, plate_cy = int(width * 0.65), int(height * 0.55)
    plate_w, plate_h = int(width * 0.10), int(height * 0.05)
    y += 50 * np.exp(-((xx - plate_cx) ** 2 / (2 * (plate_w) ** 2) +
                       (yy - plate_cy) ** 2 / (2 * (plate_h) ** 2)))

    # Clip to valid Y range
    y = np.clip(y, 0, 255).astype(np.uint8)

    # UV plane: neutral gray (128,128) interleaved
    uv = np.full((height // 2, width), 128, dtype=np.uint8)

    return y, uv


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic night YUV video")
    parser.add_argument("--output", default="/tmp/night_640x360.yuv", help="Output YUV file path")
    parser.add_argument("--width", type=int, default=640, help="Frame width")
    parser.add_argument("--height", type=int, default=360, help="Frame height")
    parser.add_argument("--frames", type=int, default=10, help="Number of frames")
    parser.add_argument("--noise", type=float, default=8.0, help="Noise sigma")
    args = parser.parse_args()

    np.random.seed(42)

    with open(args.output, "wb") as fp:
        for i in range(args.frames):
            y, uv = generate_night_frame(args.width, args.height, i, args.noise)
            fp.write(y.tobytes())
            fp.write(uv.tobytes())

    file_size = os.path.getsize(args.output)
    frame_size = args.width * args.height * 3 // 2
    print(f"Generated {args.output}")
    print(f"  width={args.width} height={args.height} frames={args.frames}")
    print(f"  frame_size={frame_size} total_size={file_size}")
    print(f"  mean Y of frame 0: {np.mean(y):.1f} (low-light threshold=80)")


if __name__ == "__main__":
    main()
