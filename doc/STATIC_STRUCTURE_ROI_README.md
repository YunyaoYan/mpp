# 静态结构重要度与检测框 ROI 融合

## 目标

在不识别“建筑、树干、OSD”具体语义的情况下，使用 16×16 亮度块的
时域稳定性和持续边缘判断静态结构，并与已有 face / plate / person /
vehicle / nonmotor 检测框一起生成编码 QP 图。

本功能不做高斯滤波，也不强制 Skip。输入保持不变，由编码器根据稳定
输入和 QP 分配自然选择 Skip/Merge。

## 分区与优先级

每帧先生成静态结构底图，再应用原有检测框：

| 优先级 | 类型 | 默认 delta QP | 判定 |
|---|---|---:|---|
| 最高 | 原有检测框 | 沿用各类别参数 | JSON/JSONL boxes |
| 高 | 稳定高结构 | -4 | 低 MAD、持续高边缘密度 |
| 中 | 动态或不确定 | 0 | 未稳定或中等结构 |
| 低 | 稳定平坦 | +4 | 低 MAD、持续低边缘密度 |

检测框后写并覆盖静态底图。RK3588 的静态底图直接写入内部稠密 16×16
ROI 配置，不占用 64 个矩形 region 名额；检测框仍使用原来的矩形接口。

RK3568 开启背景补偿时，静态图启用后只对已经判为平坦的块做补偿，
不会把动态或未知块统一抬高 QP。

## 算法

每个 16×16 块以 `sample_step` 稀疏读取 Y 分量，同时计算：

- 当前源帧与上一源帧采样值的块级 MAD；
- 中心采样点与上下左右邻点的梯度；
- 边缘密度的指数滑动平均；
- 连续稳定帧计数和结构保护保持计数。

默认连续 8 帧稳定后分类。超过 70% 块同时运动时按场景切换、摄像机运动
或全局曝光变化处理，清空旧状态并输出全中性图。

当前版本有意不解析 RK3588 `KEY_MOTION_INFO`。vepu580 的输出尺寸和
布局不是公共 `MppEncMDBlkInfo` 的逐 16×16 SAD+MV 格式，必须先在板端
用已知平移视频确认字节布局。源帧 MAD 已经能提供无延迟的运动门控；
硬件 SAD 可在布局确认后作为下一帧反馈增加，不影响现有接口。

## RK3588 使用

```bash
./mpi_enc_test \
  ... \
  --roi_enable 1 \
  --roi_boxes_json boxes.jsonl \
  --enable_static_roi 1 \
  --enable_bg_filter 0
```

输出路径：静态底图和检测框都通过 `mpp_enc_roi` 生成
`KEY_ROI_DATA2`，检测框覆盖静态底图。

## RK3568 使用

```bash
./mpi_enc_test \
  ... \
  --enable_qpmap_roi 1 \
  --roi_boxes_json boxes.jsonl \
  --enable_static_roi 1 \
  --enable_bg_filter 0
```

输出路径：静态底图在 `mpp_enc_qpmap_roi_utils` 中与检测框合并后写入
`KEY_QPMAP0`。只做静态图测试时可以不提供 `roi_boxes_json`。

## 参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `enable_static_roi` | 0 | 开启静态结构图 |
| `static_roi_sample_step` | 2 | Y 分量采样步长，归一为 1/2/4/8 |
| `static_roi_mad_thr` | 4 | 稳定块平均亮度差阈值 |
| `static_roi_edge_thr` | 24 | 单采样点梯度阈值 |
| `static_roi_edge_density` | 8 | 高结构边缘点比例，百分数 |
| `static_roi_stable_frames` | 8 | 进入静态分类所需连续帧数 |
| `static_roi_hold_frames` | 12 | 结构发生短暂变化后的保护保持帧数 |
| `static_roi_structure_delta_qp` | -4 | 稳定高结构块相对 QP |
| `static_roi_flat_delta_qp` | +4 | 稳定平坦块相对 QP |
| `dump_static_roi_debug` | 0 | 输出块级 PGM 分类图 |
| `static_roi_debug_dir` | 空 | 分类图目录 |

调试 PGM 中：白色为稳定高结构，黑色为稳定平坦，灰色为动态或未知。

## 推荐验证顺序

1. 关闭背景滤波，只开启静态结构图和 debug dump。
2. 检查固定 OSD、楼体边缘、栏杆、树干是否主要为白色。
3. 检查天空和平坦地面是否主要为黑色。
4. 检查行人、车辆框是否在最终 QP 图中覆盖静态分类。
5. 先使用 `-4 / +4`，再按区域质量和实际码率调节，不建议一开始超过
   `-6 / +6`。
