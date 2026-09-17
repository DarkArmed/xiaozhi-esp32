# peppa-v1 角色包源目录

CoreS3 佩奇角色显示 v1（设计文档 `docs/peppa-character-display-design.md`）第一阶段产物。
本目录自包含：`raw/` 内是选定的原始素材副本（构建不依赖 `tmp/`）。

## 目录结构

```
pack.json        运行时清单（§12 契约：version/pack_id/background/canvas/neutral/actions）
report.md        尺寸/容量报告（人读）
report.json      完整制作记录（变换、帧数、延时、校验结果）
neutral.png      256x168 RGBA 静态 neutral（固件打包阶段转 LVGL Alpha CBin）
actions/*.gif    256x168 全画布 GIF（全局色表 256 色、透明索引 255、disposal=1、loop=0）
preview/*.png    浅蓝底 #DCEFF8 逐帧 contact sheet（人工检查用，不入固件）
raw/             原始素材副本 + SHA256SUMS.txt
```

## 复现

```
scripts/character_pack_tools/.venv/Scripts/python.exe \
    scripts/character_pack_tools/peppa_pack.py build
```

venv 依赖：pillow、numpy（`py -m venv` 后自行安装；pypi 直连超时时可用清华镜像）。

## 制作规则（与设计 §3 对应）

- 画布 256x168，脚底基准 y=152，单人躯干中线 x=128。
- 单人动作（dance/walk/sing）：按角色净高缩放到 128 px；源画布中心映射到画布中线。
- 整体动作（muddy_jump/play_with_george）：整场景等比缩放到舞台内（含 2px 边距），
  各自记录变换——泥点飞溅跨满源画布、双人+床为互动整体，若强行套 128 px
  站立高必然切头/切泥坑，违反"不能切掉头、脚或必要道具"硬约束。
- 时序：按累计时长以 70 ms（约 14.3 fps）重采样，合并连续重复帧；延时量化到
  GIF 的 10 ms 精度，末帧吸收总时长误差，循环时长与源一致（见表）。
- 调色板：全部帧联合 MEDIANCUT 255 色 + 1 透明索引，NEAREST 映射，无抖动；
  单一全局色表（gifdec 要求），无局部色表。
- GIF 由工具内置编码器直接生成（Pillow 会写局部色表并裁剪帧矩形，不满足要求）；
  LZW 位宽时序与固件 `gifdec` 解码逻辑逐位对齐。

## 特殊处理记录

- walk（travel.gif）：贴纸白边已去除——从紧邻透明像素的近白色（RGB 最小通道
  >=200）向外泛洪置透明；眼白等被黑轮廓包围的白色不受影响，逐帧检查无残留。
- neutral：按 Alpha bbox 裁剪后缩放 0.07048，缩放用预乘 alpha 的 Lanczos（避免
  暗边）；浅蓝底合成检查无灰晕残留。

## 尺寸摘要

| 项 | 值 |
|---|---|
| 动作 GIF 合计 | 829,757 B（预算 <=2 MiB） |
| neutral PNG | 14,212 B；A8 全画布 43,008 B / 按 bbox 12,416 B |
| 解码 RAM（5*W*H/帧缓冲） | 215,040 B + LZW 表/结构体 |
| 帧数合计 | 131 帧（源 197 帧） |

来源与排除项：`tmp/peppa-media/round2/sources.json`、`tmp/peppa-media/neutral/selected.json`；
neutral 原图 SHA-256 与设计文档 §2 一致。
