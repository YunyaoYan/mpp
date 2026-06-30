# RK3588 框驱动 ROI 编码适配说明

本文档说明在 Rockchip MPP demo 层为 **RK3588 / vepu580** 增加的框驱动 ROI 编码适配：改动动机、硬件路径选择、代码变更与使用方法。

与 `QPMAP_ROI_README.md` 的关系：

| 文档 | 适用平台 | 元数据键 | 说明 |
|------|----------|----------|------|
| `QPMAP_ROI_README.md` | vepu541 系列（如 RK3568） | `KEY_QPMAP0` | 逐 16×16 块 delta QP |
| **本文档** | **RK3588 / vepu580** | **`KEY_ROI_DATA2`** | 区域式 ROI（`MppEncROICfg2`） |

两套能力可共用同一套 boxes JSON/JSONL 文件格式，但**运行时只能走其中一条硬件路径**，由 SoC 类型决定。

---

## 1. 为什么要改

### 1.1 业务目标

在 CBR / VBR 等限码率场景下，将码率预算向**人脸等保护区**倾斜，背景可使用更激进的 QP，在体积可控的前提下提升关键区域主观清晰度。典型 pipeline：

```
外部检测（如 vegapy FaceDetector）→ 每帧框 JSON → MPP 硬件编码时按帧注入 ROI
```

### 1.2 RK3588 上 QPMAP 路径不可用

仓库中已有的 QPMAP ROI 实现（`mpp_enc_qpmap_roi_utils.c`）通过帧 meta 挂载 `KEY_QPMAP0`，在 **vepu541** HAL 中消费。

RK3588 使用 **vepu580** 编码器。`mpp_enc_roi_init()` 在 RK3588 上自动选择 `ROI_TYPE_2`，走 `gen_vepu580_roi_h265()` / `KEY_ROI_DATA2`，**不会**处理 `KEY_QPMAP0`。

在 RK3588 上启用 `--enable_qpmap_roi 1` 时，QPMAP 缓冲区会被写入 meta，但硬件侧不生效，编码结果与 baseline 无差异。

### 1.3 需要框驱动的区域式 ROI

MPP 原生 ROI 演示（`mpi_enc_test` + `roi_enable`）原先只在编码循环内**硬编码**两块固定矩形区域，无法对接外部检测输出的逐帧变化框。

因此新增 **JSON/JSONL 框文件 → `mpp_enc_roi_add_region()` → `KEY_ROI_DATA2`** 的适配层，在 demo 层完成验证，不修改 MPP 核心编码器。

### 1.4 CBR 场景下的使用约束（实测结论）

- ROI 应在 **CBR / VBR** 下使用；`-rc 2`（FixQP）时部分区域 QP 调整可能不生效。
- **相对 QP**（`face_delta_qp`）更适合 CBR：总码率稳定，质量在人脸与背景间重新分配。
- **绝对 QP**（`face_abs_qp`）可强制人脸清晰度，但保护区过大时会导致 CBR 目标码率难以维持、输出体积明显增大。
- 不建议在 FixQP 模式下依赖 ROI 做质量分配。

---

## 2. 改了什么

### 2.1 新增文件

| 文件 | 作用 |
|------|------|
| `utils/mpp_enc_roi_boxes_utils.c` | 解析 boxes JSON/JSONL，按帧将检测框转为 `RoiRegionCfg` 并加入 `mpp_enc_roi` |
| `utils/mpp_enc_roi_boxes_utils.h` | 对外 API：`init` / `deinit` / `apply` |

### 2.2 修改文件

| 文件 | 变更摘要 |
|------|----------|
| `utils/CMakeLists.txt` | 编译链接 `mpp_enc_roi_boxes_utils.c` |
| `utils/mpi_enc_utils.h` | `MpiEncTestArgs` 增加 `roi_boxes_json`、`face_abs_qp`；`MpiEncMultiCtxInfo` 增加 `roi_boxes_ctx` |
| `utils/mpi_enc_utils.c` | 新增 CLI 选项；`roi_enable` 时可选初始化 `roi_boxes_ctx` |
| `utils/mpp_enc_args.c` | 参数默认值（`face_abs_qp = -1` 表示关闭） |
| `test/mpi_enc_test.c` | 每帧编码前：有 `roi_boxes_json` 则调用 `mpp_enc_roi_boxes_apply()`，再 `mpp_enc_roi_setup_meta()` |

### 2.3 数据流（RK3588）

```
boxes.jsonl
    ↓ mpp_enc_roi_boxes_init()      # 加载全部帧记录
每帧编码循环:
    ↓ mpp_enc_roi_boxes_apply()     # 当前 frame_idx 的框 → mpp_enc_roi_add_region()
    ↓ mpp_enc_roi_setup_meta()      # gen_vepu580_roi_h265() → KEY_ROI_DATA2
    ↓ mpp_encode_put_frame()
vepu580 硬件编码
```

`mpp_enc_roi_setup_meta()` 在提交 meta 后会将 `impl->count` 置零，因此**每帧**可注入不同的框列表（最多 64 个 region，由 `mpp_enc_roi_init(..., 64)` 限制）。开启平滑时会先为所有原始 ROI 预留核心 region，再用剩余名额添加由外到内的平滑矩形，最后写入核心 ROI；平滑不会挤占原始人脸框。

### 2.4 框坐标与块对齐

检测框为像素坐标 `(x1,y1,x2,y2)`。写入硬件前在工具层转换为 16×16 宏块网格，并做边界裁剪，避免越界导致 `gen_vepu54x_roi` 断言失败：

- 块起点：`bx1 = x1/16`，`by1 = y1/16`
- 块终点（开区间）：`bx2 = (x2+15)/16`，`by2 = (y2+15)/16`
- 裁剪到 `[0, mb_w]` / `[0, mb_h]`

人脸框可通过 `face_expand_blocks` 在块坐标系下向四边扩展，扩大保护区（对远景小脸有效）。

### 2.5 QP 模式

每个框对应一个 `RoiRegionCfg`：

| 参数 | qp_mode | 含义 |
|------|---------|------|
| `face_delta_qp` / `plate_delta_qp` | 0（相对） | 在 RC 当前帧 QP 基础上增减；负值降低 QP、提升质量 |
| `face_abs_qp >= 0` | 1（绝对） | 人脸区固定为该 QP；**优先级高于** `face_delta_qp` |

默认：`face_delta_qp = -3`，`face_abs_qp = -1`（不使用绝对 QP）。

---

## 3. 使用方法

### 3.1 构建

```bash
cmake -S . -B build/roi_validate -DCMAKE_BUILD_TYPE=Release
cmake --build build/roi_validate --target mpi_enc_test -j4
```

### 3.2 启用 ROI

RK3588 上通过**环境变量**启用原生 ROI 路径（非 CLI `--roi_enable`）：

```bash
export LD_LIBRARY_PATH=build/roi_validate/mpp:$LD_LIBRARY_PATH
export roi_enable=1
```

### 3.3 boxes 文件格式

与 `QPMAP_ROI_README.md` 相同，JSONL 每行一帧：

```json
{"frame_idx": 0, "boxes": [{"type": "face", "x1": 100, "y1": 200, "x2": 180, "y2": 280}]}
```

- `frame_idx`：从 0 起，与编码帧序号一致（`mpi_enc_test` 中 `frm_cnt_out`）
- `type`：`face` 或 `plate`（plate 使用 `plate_delta_qp`）
- 若检测输出为 `xywh` 格式，需转换：`x2 = x + w`，`y2 = y + h`，`frame_idx = fid - 1`

### 3.4 编码示例

**CBR + 人脸相对 QP（推荐）：**

```bash
roi_enable=1 ./build/roi_validate/test/mpi_enc_test \
  -i input.nv12 -w 1920 -h 1080 -f 0 -t 16777220 -n <frames> -fps 25/1 \
  -rc 1 -bps 3000000 \
  -qc 42:30:51:30:51 \
  -o out.h265 \
  --roi_boxes_json boxes.jsonl \
  --face_delta_qp -20 \
  --face_expand_blocks 3
```

**仅当相对 QP 仍不足、且可接受体积上涨时，使用绝对 QP：**

```bash
  --face_abs_qp 22 --face_expand_blocks 4
```

`-qc qp_init:min:max:min_i:max_i` 用于背景激进压缩（提高 QP 上限），与 ROI 配合：人脸降 QP，背景允许更高 QP。

### 3.5 新增 CLI 选项一览

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--roi_boxes_json` | 空 | boxes JSON/JSONL 路径；设置后走框驱动 ROI |
| `--face_delta_qp` | -3 | 人脸相对 QP 调整 |
| `--face_expand_blocks` | 1 | 人脸框扩展块数（16×16 像素/块） |
| `--qpmap_smooth_radius` | 2 | RK3588 ROI box 路径下复用为人脸边界过渡层数（16×16 像素/层，每层每脸 1 个 region） |
| `--face_abs_qp` | -1 | ≥0 时启用人脸绝对 QP，覆盖 delta |
| `--plate_delta_qp` | -6 | 车牌相对 QP 调整 |

说明：`--enable_qpmap_roi`、`--enable_bg_compensation` 等 QPMAP 专用选项仍保留，但在 **RK3588 上不生效**；RK3588 应使用本文档的 `roi_enable` + `roi_boxes_json` 路径。`--face_expand_blocks` 和 `--qpmap_smooth_radius` 已接入 RK3588 ROI box 路径。

---

## 4. RK3588 平台验证摘要

在 RK3588 Toybrick 上已验证：

| 场景 | 结论 |
|------|------|
| `roi_enable=1` 硬编码区域 vs baseline | 码率/体积变化明显，ROI 硬件生效 |
| CBR + `face_delta_qp` + boxes JSON | 同码率下保护区 PSNR 提升，背景承担压缩 |
| `KEY_QPMAP0` + `--enable_qpmap_roi 1` | 相对 baseline 无变化（硬件不消费） |
| 4K / 1080p 交通场景 CBR 极限压缩 | 存在编码器码率下限；相对 QP + 背景激进 `-qc` 效果优于绝对 QP |

---

## 5. Vega 生产路径接入（阶段 B）

`vega/rknn2/srv/video_device_encoder.cpp` 的 `RknnVencProc` 已通过环境变量启用 ROI，由 `demo/main_video.py` 阶段2 子进程透传。

| 环境变量 | 说明 |
|----------|------|
| `VEGA_ROI_ENABLE` | `1` 启用 |
| `VEGA_ROI_BOXES_JSON` | `face_boxes.jsonl` 路径 |
| `VEGA_FACE_DELTA_QP` | 人脸相对 QP（CBR 推荐） |
| `VEGA_FACE_EXPAND_BLOCKS` | 框扩展（16×16 块） |
| `VEGA_FACE_ABS_QP` | 绝对 QP（≥0 生效，默认关闭） |
| `VEGA_ENC_RC_MODE` | 默认 `cbr` |
| `VEGA_ENC_BPS` | 目标码率 bps |
| `VEGA_ENC_QC` | 背景 QP `init:min:max:min_i:max_i` |
| `VEGA_ROI_FRAME_OFFSET` | 帧序校准偏移 |

```bash
python3 demo/main_video.py -v demo/video/trim_dh_1955_1959.mp4 --codec h265 \
  --roi-enable --cbr-bps 1250000 --face-delta-qp -18 --face-expand-blocks 2
```

`trim_dh_1955_1959.mp4`（CBR 1.25Mbps）：vega baseline ~2.43MB，ROI ~2.52MB，与 `mpi_enc_test` 阶段 A 结果一致。

---

## 6. 相关源码索引

- vepu580 ROI 生成：`utils/mpp_enc_roi_utils.c` → `gen_vepu580_roi_h265()`，`ROI_TYPE_2`
- SoC 选择：`mpp_enc_roi_init()` 中 `ROCKCHIP_SOC_RK3588 → ROI_TYPE_2`
- 框适配器：`utils/mpp_enc_roi_boxes_utils.c`
- 编码循环注入：`test/mpi_enc_test.c`（`cmd->roi_enable` 分支）
- QPMAP 参考实现（非 RK3588）：`utils/mpp_enc_qpmap_roi_utils.c`，文档 `doc/QPMAP_ROI_README.md`
