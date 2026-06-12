# QPMAP ROI 与 ROI-Protected Background Filtering 功能文档

## 目录

- [1. 功能概述](#1-功能概述)
- [2. QPMAP ROI](#2-qpmap-roi)
  - [2.1 功能介绍](#21-功能介绍)
  - [2.2 接口说明](#22-接口说明)
  - [2.3 命令行参数](#23-命令行参数)
  - [2.4 算法原理](#24-算法原理)
  - [2.5 使用示例](#25-使用示例)
- [3. ROI-Protected Background Filtering](#3-roi-protected-background-filtering)
  - [3.1 功能介绍](#31-功能介绍)
  - [3.2 接口说明](#32-接口说明)
  - [3.3 命令行参数](#33-命令行参数)
  - [3.4 算法原理](#34-算法原理)
  - [3.5 使用示例](#35-使用示例)
- [4. 联合使用](#4-联合使用)
- [5. 输入框格式](#5-输入框格式)
- [6. 调试与验证](#6-调试与验证)
- [7. 已知限制](#7-已知限制)

---

## 1. 功能概述

本分支在 Rockchip MPP 编码器 demo 层新增了两个独立的功能模块：

| 功能 | 作用阶段 | 目的 |
|---|---|---|
| **QPMAP ROI** | 编码前，通过 `KEY_QPMAP0` 元数据传递 | 对 ROI 区域（人脸、车牌、人体、机动车、非机动车）分级降低 QP 以提升画质，可选对背景升高 QP 做码率补偿 |
| **ROI-Protected Background Filtering** | 编码前，对原始 YUV 帧做预处理 | 在低照度场景下对背景做高斯滤波降噪，同时保护 ROI 区域不被模糊 |

两个功能可以独立开启，也可以同时启用。默认均为关闭状态，不影响原有编码行为。

---

## 2. QPMAP ROI

### 2.1 功能介绍

QPMAP ROI 通过为每个 16×16 块设置不同的 delta QP，实现帧级 ROI 编码：

- **人脸区域**：分配较低的 delta QP（如 -3），提升面部细节
- **车牌区域**：分配更低的 delta QP（如 -6），保证 OCR 可读性
- **人体区域**：分配中等 delta QP（如 -4），保证行人轮廓清晰
- **机动车区域**：分配中等 delta QP（如 -4），保证车辆细节
- **非机动车区域**：分配中等 delta QP（如 -4），保证骑行者细节
- **背景区域**：可选升高 QP 做码率补偿，使整帧平均 delta QP 接近零
- **边界平滑**：新增 `smooth_radius` 参数，在 ROI 与背景交界处做渐变过渡，消除硬切导致的视觉边界

支持 5 种 ROI 类型分级控制：`face` / `plate` / `person` / `vehicle` / `nonmotor`。重叠区域取更小的 delta QP（更负 = 更高码率）。

### 2.2 接口说明

#### 配置结构体

```c
typedef struct MppEncQpmapRoiCfg_t {
    RK_U32              enable;               /* 0=关闭, 1=开启 */
    const char          *boxes_file;          /* ROI 框 JSON/JSONL 文件路径 */
    RK_S32              face_delta_qp;        /* 人脸区域 delta QP，默认 -3 */
    RK_S32              face_expand_blocks;   /* 人脸框外扩块数(16x16)，默认 1 */
    RK_S32              plate_delta_qp;       /* 车牌区域 delta QP，默认 -6 */
    RK_S32              person_delta_qp;      /* 人体区域 delta QP，默认 -4 */
    RK_S32              vehicle_delta_qp;     /* 机动车区域 delta QP，默认 -4 */
    RK_S32              nonmotor_delta_qp;    /* 非机动车区域 delta QP，默认 -4 */
    RK_S32              delta_qp_min;         /* delta QP 下限，默认 -8 */
    RK_S32              delta_qp_max;         /* delta QP 上限，默认 4 */
    RK_U32              enable_bg_compensation; /* 是否启用背景码率补偿，默认 1 */
    RK_S32              bg_delta_qp_max;      /* 背景补偿最大 delta QP，默认 4 */
    RK_S32              smooth_radius;          /* 平滑半径(块数)，默认 2，0=关闭 */
    RK_U32              dump_qpmap_debug;     /* 是否导出 QPMAP 调试文本 */
    const char          *debug_dir;             /* 调试文件输出目录 */
} MppEncQpmapRoiCfg;
```

#### 核心 API

```c
/* 初始化 QPMAP ROI 上下文 */
MPP_RET mpp_enc_qpmap_roi_init(MppEncQpmapRoiCtx *ctx, RK_U32 w, RK_U32 h,
                               MppCodingType type, const MppEncQpmapRoiCfg *cfg);

/* 释放上下文 */
MPP_RET mpp_enc_qpmap_roi_deinit(MppEncQpmapRoiCtx ctx);

/* 为当前帧设置 QPMAP 元数据 */
MPP_RET mpp_enc_qpmap_roi_setup_meta(MppEncQpmapRoiCtx ctx, MppMeta meta,
                                     RK_S32 frame_idx);
```

#### 使用方式

在提交编码帧前，将 QPMAP 附加到帧元数据：

```c
MppMeta meta = mpp_frame_get_meta(frame);
mpp_enc_qpmap_roi_setup_meta(qpmap_roi_ctx, meta, frame_idx);
```

### 2.3 命令行参数

| 参数 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `--enable_qpmap_roi` | int | 0 | 是否启用 QPMAP ROI |
| `--roi_boxes_json` | string | - | ROI 框 JSON/JSONL 文件路径 |
| `--face_delta_qp` | int | -3 | 人脸区域 delta QP |
| `--face_expand_blocks` | int | 1 | 人脸框外扩 16x16 块数 |
| `--plate_delta_qp` | int | -6 | 车牌区域 delta QP |
| `--person_delta_qp` | int | -4 | 人体/行人区域 delta QP |
| `--vehicle_delta_qp` | int | -4 | 机动车区域 delta QP |
| `--nonmotor_delta_qp` | int | -4 | 非机动车区域 delta QP |
| `--delta_qp_min` | int | -8 | delta QP 最小值 |
| `--delta_qp_max` | int | 4 | delta QP 最大值 |
| `--enable_bg_compensation` | int | 1 | 是否启用背景码率补偿 |
| `--bg_delta_qp_max` | int | 4 | 背景补偿最大 delta QP |
| `--qpmap_smooth_radius` | int | 2 | ROI 边界平滑半径（块数），0=关闭 |
| `--dump_qpmap_debug` | int | 0 | 是否导出 QPMAP 调试文本 |
| `--qpmap_debug_dir` | string | - | QPMAP 调试输出目录 |

### 2.4 算法原理

#### 2.4.1 Delta QP 分配

1. 将像素坐标框转换为 16×16 块坐标框
2. 人脸框按 `face_expand_blocks` 外扩（覆盖额头、下巴等检测遗漏区域）
3. 根据 ROI 类型分配对应的 delta QP：

| ROI 类型 | 默认 delta QP | 优先级 |
|---|---|---|
| `face` | -3 | 高 |
| `plate` | -6 | 最高 |
| `person` | -4 | 中 |
| `vehicle` | -4 | 中 |
| `nonmotor` | -4 | 中 |

4. **重叠区域取更小的 delta QP**（更负 = 更高码率）。例如 face (-3) 与 plate (-6) 重叠时，plate 的 -6 胜出
5. 所有值钳位到 `[delta_qp_min, delta_qp_max]`

#### 2.4.2 背景码率补偿

当 `enable_bg_compensation=1` 时：

```
neg_sum = sum(delta_qp over ROI blocks)   /* ROI 区域通常为负值 */
bg_count = number of non-ROI blocks
bg_delta = round(-neg_sum / bg_count)      /* 使整帧平均 delta QP 接近 0 */
bg_delta = clamp(bg_delta, 0, bg_delta_qp_max)
```

> 注意：这仅保证平均 delta QP 接近零，不保证码率与基线完全一致，因为 QP 与码率并非线性关系。

#### 2.4.3 边界平滑（Smooth Radius）

当 `smooth_radius > 0` 时，对 ROI 边界外的非 ROI 块做渐变过渡：

1. 对每个非 ROI 块，在 `smooth_radius` 范围内搜索最近的 ROI 块（Chebyshev 距离）
2. 计算距离比例 `t = dist / radius`（0~1）
3. 线性插值：
   ```
   smoothed = roi_delta * (1 - t) + bg_delta * t
   ```
4. 结果钳位到 `[delta_qp_min, delta_qp_max]`

ROI 块本身保持原值不变。平滑半径为 2 时，过渡带宽度约为 32 像素（2 个 16×16 块）。

#### 2.4.4 QPMAP 打包格式

每个 16×16 块打包为 16-bit VEPU541 兼容格式：

- `qp_area_en = 1`
- `qp_adj_mode = 0`（相对 QP 调整）
- `qp_adj = delta_qp`

### 2.5 使用示例

#### 仅启用 QPMAP ROI

```bash
./mpi_enc_test \
  -i input.yuv -o out_roi.h265 -w 1920 -h 1080 -f 0 -t 16777220 \
  --enable_qpmap_roi 1 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -3 \
  --face_expand_blocks 1 \
  --plate_delta_qp -6 \
  --enable_bg_compensation 1 \
  --qpmap_smooth_radius 2 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_debug
```

#### 关闭平滑（硬切边界）

```bash
./mpi_enc_test ... --qpmap_smooth_radius 0 ...
```

#### 更强的平滑过渡

```bash
./mpi_enc_test ... --qpmap_smooth_radius 4 ...
```

#### 分级 ROI（5 种类型同时启用）

```bash
./mpi_enc_test \
  -i input.yuv -o out_roi.h265 -w 1920 -h 1080 -f 0 -t 16777220 \
  --enable_qpmap_roi 1 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -3 \
  --plate_delta_qp -6 \
  --person_delta_qp -4 \
  --vehicle_delta_qp -4 \
  --nonmotor_delta_qp -4 \
  --enable_bg_compensation 1 \
  --qpmap_smooth_radius 2 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_debug
```

对应的 JSONL 框文件示例：

```json
{"frame_idx": 0, "boxes": [
  {"type": "face", "x1": 100, "y1": 100, "x2": 220, "y2": 240},
  {"type": "plate", "x1": 300, "y1": 400, "x2": 430, "y2": 450},
  {"type": "person", "x1": 50, "y1": 200, "x2": 150, "y2": 500},
  {"type": "vehicle", "x1": 400, "y1": 300, "x2": 800, "y2": 600},
  {"type": "nonmotor", "x1": 200, "y1": 350, "x2": 300, "y2": 500}
]}
```

> 重叠区域自动取更小的 delta QP。例如 face (-3) 与 plate (-6) 重叠时，使用 -6。

---

## 3. ROI-Protected Background Filtering

### 3.1 功能介绍

ROI-Protected Background Filtering 是一个轻量级的空域背景滤波器，用于夜间/低照度场景：

- **低照度检测**：通过帧平均亮度 Y 判断是否启用滤波
- **背景降噪**：对背景区域应用高斯模糊，抑制噪声、降低编码复杂度
- **ROI 保护**：人脸、车牌等 ROI 区域完全保留原始像素，不被模糊
- **时域融合**（可选）：对静态背景做帧间时域混合，进一步平滑

该功能在编码前对原始 YUV 帧做预处理，与编码器类型无关。

### 3.2 接口说明

#### 配置结构体

```c
typedef struct MppEncBgFilterCfg_t {
    RK_U32              enable;               /* 0=关闭, 1=开启 */
    RK_S32              filter_type;          /* 0=高斯, 1=双边(预留) */
    RK_S32              low_light_thr;       /* 弱滤波亮度阈值，默认 80 */
    RK_S32              strong_light_thr;   /* 强滤波亮度阈值，默认 50 */
    RK_S32              kernel_weak;        /* 弱滤波核大小，默认 3 */
    RK_S32              kernel_strong;      /* 强滤波核大小，默认 5 */
    RK_S32              roi_expand_face;    /* 人脸框外扩比例*100，默认 15=0.15 */
    RK_S32              roi_expand_plate;   /* 车牌框外扩比例*100，默认 20=0.20 */
    RK_S32              roi_expand_default; /* 其他框外扩比例*100，默认 10=0.10 */
    RK_S32              roi_mask_dilate_iter; /* ROI mask 膨胀迭代次数，默认 1 */
    RK_U32              enable_temporal;    /* 时域融合开关，默认 0 */
    RK_S32              temporal_alpha;     /* 当前帧权重*100，默认 80=0.8 */
    RK_S32              motion_diff_thr;    /* 运动检测阈值，默认 12 */
    RK_U32              dump_debug;         /* 是否导出调试图像 */
    const char          *debug_dir;          /* 调试输出目录 */
    const char          *boxes_file;        /* ROI 框文件路径 */
} MppEncBgFilterCfg;
```

#### 核心 API

```c
/* 初始化背景滤波上下文 */
MPP_RET mpp_enc_bg_filter_init(MppEncBgFilterCtx *ctx, RK_U32 w, RK_U32 h,
                               MppFrameFormat fmt, const MppEncBgFilterCfg *cfg);

/* 释放上下文 */
MPP_RET mpp_enc_bg_filter_deinit(MppEncBgFilterCtx ctx);

/* 处理一帧图像 */
MPP_RET mpp_enc_bg_filter_process(MppEncBgFilterCtx ctx, RK_U8 *buf, RK_U8 *out_buf,
                                  RK_S32 frame_idx, RK_S32 hor_stride, RK_S32 ver_stride);

/* 查询上一帧是否被滤波 */
RK_S32 mpp_enc_bg_filter_last_filtered(MppEncBgFilterCtx ctx);
```

#### 使用方式

在将原始帧包装为 `MppFrame` 之前，先对 buffer 做滤波处理：

```c
mpp_enc_bg_filter_process(bg_filter_ctx, raw_buf, filtered_buf, frame_idx, hor_stride, ver_stride);
/* 然后将 filtered_buf 包装为 MppFrame 提交给编码器 */
```

### 3.3 命令行参数

| 参数 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `--enable_bg_filter` | int | 0 | 是否启用背景滤波 |
| `--bg_filter_type` | int | 0 | 滤波类型：0=高斯，1=双边(预留) |
| `--bg_filter_low_light_thr` | int | 80 | 弱滤波亮度阈值（mean Y） |
| `--bg_filter_strong_light_thr` | int | 50 | 强滤波亮度阈值（mean Y） |
| `--bg_filter_kernel_weak` | int | 3 | 弱滤波核大小 |
| `--bg_filter_kernel_strong` | int | 5 | 强滤波核大小 |
| `--roi_expand_face` | int | 15 | 人脸框外扩比例 ×100 |
| `--roi_expand_plate` | int | 20 | 车牌框外扩比例 ×100 |
| `--roi_expand_default` | int | 10 | 其他框外扩比例 ×100 |
| `--roi_mask_dilate_iter` | int | 1 | ROI mask 膨胀迭代次数 |
| `--enable_temporal_bg_filter` | int | 0 | 是否启用时域融合 |
| `--temporal_alpha` | int | 80 | 时域融合当前帧权重 ×100 |
| `--motion_diff_thr` | int | 12 | 运动检测阈值 |
| `--dump_bg_filter_debug` | int | 0 | 是否导出调试图像 |
| `--bg_filter_debug_dir` | string | - | 调试输出目录 |
| `--bg_filter_boxes_json` | string | - | ROI 框文件（可选，默认使用 roi_boxes_json） |

### 3.4 算法原理

#### 3.4.1 低照度检测

计算帧 Y 平面平均亮度：

```
mean_y >= low_light_thr (80)      → 不滤波
strong_light_thr (50) <= mean_y < low_light_thr (80)  → 弱滤波（3×3 高斯）
mean_y < strong_light_thr (50)    → 强滤波（5×5 高斯）
```

#### 3.4.2 高斯滤波

可分离高斯核实现：

- **弱滤波（3×3）**：核 `[1, 2, 1] / 4`
- **强滤波（5×5）**：核 `[1, 4, 6, 4, 1] / 16`

对于 YUV420SP/NV12/YUV420P，仅滤波 Y 平面，UV 平面原样复制，避免色度失真。
对于 RGB/BGR 打包格式，所有通道均滤波。

#### 3.4.3 ROI Mask 生成

1. 按类型外扩检测框：
   - 人脸：`roi_expand_face`（默认 15%，即 0.15）
   - 车牌：`roi_expand_plate`（默认 20%，即 0.20）
   - 其他：`roi_expand_default`（默认 10%，即 0.10）

2. 外扩计算：
   ```
   dx = expand * box_width
   dy = expand * box_height
   x1 -= dx; y1 -= dy
   x2 += dx; y2 += dy
   ```

3. 图像边界钳位
4. 填充像素级 mask（1=保护，0=背景）
5. 对 mask 做 3×3 十字膨胀 `roi_mask_dilate_iter` 次（默认 1 次）

#### 3.4.4 ROI 保护

滤波后，将原始 ROI 像素复制回输出：

```
output[mask == 1] = original[mask == 1]
```

确保人脸、车牌等保护对象完全清晰。

#### 3.4.5 时域融合（可选）

当 `enable_temporal_bg_filter=1` 时：

1. 保存上一帧滤波结果
2. 计算当前帧与上一帧的逐像素绝对差
3. 分类处理：
   - `mask == 1`（ROI）：始终使用当前帧
   - `diff > motion_diff_thr`（运动区域）：使用当前帧
   - 其他（静态背景）：时域混合
     ```
     output = alpha * current + (1 - alpha) * previous
     ```
4. 再次恢复 ROI 原始像素

默认 `alpha = 0.8`，`motion_diff_thr = 12`。

> 注意：时域融合默认关闭。若运动阈值设置过松，运动物体可能出现拖影。

### 3.5 使用示例

#### 仅启用背景滤波

```bash
./mpi_enc_test \
  -i input.yuv -o out_bg.h265 -w 1920 -h 1080 -f 0 -t 16777220 \
  --enable_bg_filter 1 \
  --bg_filter_boxes_json boxes.jsonl \
  --bg_filter_low_light_thr 80 \
  --bg_filter_strong_light_thr 50 \
  --bg_filter_kernel_weak 3 \
  --bg_filter_kernel_strong 5 \
  --roi_expand_face 15 \
  --roi_expand_plate 20 \
  --roi_mask_dilate_iter 1 \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir bg_debug
```

#### 启用时域融合

```bash
./mpi_enc_test ... \
  --enable_temporal_bg_filter 1 \
  --temporal_alpha 80 \
  --motion_diff_thr 12 ...
```

---

## 4. 联合使用

两个功能可以同时启用，实现"背景降噪 + ROI 画质提升 + 码率补偿"的组合效果：

```bash
./mpi_enc_test \
  -i input.yuv -o out_combined.h265 -w 1920 -h 1080 -f 0 -t 16777220 \
  --enable_qpmap_roi 1 \
  --enable_bg_filter 1 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -3 \
  --plate_delta_qp -6 \
  --person_delta_qp -4 \
  --vehicle_delta_qp -4 \
  --nonmotor_delta_qp -4 \
  --enable_bg_compensation 1 \
  --qpmap_smooth_radius 2 \
  --bg_filter_low_light_thr 80 \
  --bg_filter_kernel_weak 3 \
  --dump_qpmap_debug 1 \
  --qpmap_debug_dir qpmap_debug \
  --dump_bg_filter_debug 1 \
  --bg_filter_debug_dir bg_debug
```

处理顺序：

1. 原始帧 → **Background Filtering**（降噪）→ 滤波后帧
2. 滤波后帧 → 包装为 `MppFrame`
3. `MppFrame` → 附加 **QPMAP ROI** 元数据 → 提交编码器

---

## 5. 输入框格式

两个功能共用相同的 JSON/JSONL 框格式：

```json
{"frame_idx": 0, "boxes": [{"type": "face", "x1": 100, "y1": 100, "x2": 220, "y2": 240, "score": 0.99}]}
{"frame_idx": 1, "boxes": [{"type": "plate", "x1": 300, "y1": 400, "x2": 430, "y2": 450, "score": 0.95}]}
{"frame_idx": 2, "boxes": [{"type": "face", "x1": 100, "y1": 100, "x2": 220, "y2": 240}, {"type": "plate", "x1": 300, "y1": 400, "x2": 430, "y2": 450}]}
```

支持字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `frame_idx` | int | 是 | 帧序号 |
| `type` | string | 是 | `face`, `plate`, `person`, `vehicle`, `nonmotor` 或其他 |
| `x1`, `y1`, `x2`, `y2` | int | 是 | 像素坐标框 |
| `score` | float | 否 | 置信度（当前版本忽略） |

- 坐标越界自动钳位到图像边界
- `x2 <= x1` 或 `y2 <= y1` 的框会被跳过
- 未在 JSONL 中列出的帧将使用全零 QPMAP / 全零 ROI mask

---

## 6. 调试与验证

### 6.1 QPMAP ROI 调试

启用调试导出后，每帧生成一个文本文件：

```
qpmap_debug/frame_000000_qpmap.txt
qpmap_debug/frame_000001_qpmap.txt
...
```

每行对应一行 16×16 块，值为 signed delta QP。例如：

```
0  0  1  1  1
0 -3 -3  1  1
0 -3 -6 -6  1
```

同时日志输出每帧统计（5 种 ROI 类型分别计数）：

```
frame N face X plate Y person Z vehicle W nonmotor V roi_blk A bg_blk B min C max D mean E bg_delta F
```

### 6.2 Background Filter 调试

启用调试导出后，每帧生成三幅 PGM 图像：

```
bg_debug/frame_000000_orig.pgm      /* 原始 Y 平面 */
bg_debug/frame_000000_mask.pgm    /* ROI mask，白色=保护 */
bg_debug/frame_000000_filtered.pgm /* 滤波后 Y 平面 */
```

同时日志输出每帧统计：

```
frame N boxes X mean_y Y level Z kernel K roi_ratio R temporal T
```

### 6.3 建议验证用例

| 用例 | QPMAP ROI | BG Filter | 目的 |
|---|---|---|---|
| 基线 | 关闭 | 关闭 | 对照组 |
| ROI 纯画质 | 开启，bg_compensation=0 | 关闭 | 观察纯 ROI 码率开销 |
| ROI + 补偿 | 开启，bg_compensation=1 | 关闭 | 观察补偿后码率 |
| 背景滤波 | 关闭 | 开启 | 观察降噪效果 |
| 组合 | 开启 | 开启 | 综合效果 |

---

## 7. 已知限制

### QPMAP ROI

- 暂无码率反馈闭环，delta QP 为固定值
- 暂无时域平滑（帧间 QPMAP 可能抖动）
- `face_expand_blocks` 为块级外扩，非像素级精细边缘
- 暂无基于背景复杂度的自适应分配
- JSON 解析器为轻量实现，不支持完整 JSON 规范

### ROI-Protected Background Filtering

- 暂无运动补偿（无块匹配、无 MV 对齐）
- 时域融合为简化实现，运动阈值过松可能产生拖影（默认关闭）
- 仅实现高斯滤波，双边滤波预留
- YUV422 等其他格式直接透传，不做滤波
- 滤波在 CPU 上执行，暂无硬件加速
- 暂无基于局部噪声水平的自适应核大小
- 背景滤波与 QPMAP 模块无联动（滤波强度不反馈到 QP 调整）

---

*文档版本：2026-06-09（含分级 ROI 与边界平滑）*
*分支：`codex/qpmap-roi-bg-filter`*
