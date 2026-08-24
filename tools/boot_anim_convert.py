#!/usr/bin/env python3
"""
开机动画帧转换工具
将视频帧(PNG/JPG)转换为 ESP32 可直接播放的 RGB565 原始帧。

竖屏(240x320)播放横版视频时使用 pillarbox:等宽缩放、上下留黑边,
内容完整、正立、不变形。

用法:
    python boot_anim_convert.py <frames_dir> <output_dir> [--frames 32] [--fps 13]
"""
import os
import sys
import struct
import argparse
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("需要安装: pip install Pillow numpy")
    sys.exit(1)

SCREEN_W = 240
SCREEN_H = 320


def convert_frame(img, mode):
    """将 PIL 图像转换为 240x320 RGB565(大端) 原始字节"""
    img = img.convert("RGB")
    src_w, src_h = img.size

    if mode == "fit":  # pillarbox:等宽缩放居中,黑边填充
        scale = min(SCREEN_W / src_w, SCREEN_H / src_h)
        new_w = max(1, round(src_w * scale))
        new_h = max(1, round(src_h * scale))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(img, ((SCREEN_W - new_w) // 2, (SCREEN_H - new_h) // 2))
        img = canvas
    else:  # fill:等比放大填满并居中裁剪
        scale = max(SCREEN_W / src_w, SCREEN_H / src_h)
        new_w = max(SCREEN_W, round(src_w * scale))
        new_h = max(SCREEN_H, round(src_h * scale))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        left = (new_w - SCREEN_W) // 2
        top = (new_h - SCREEN_H) // 2
        img = img.crop((left, top, left + SCREEN_W, top + SCREEN_H))

    # RGB565 大端,向量化打包
    arr = np.asarray(img, dtype=np.uint16)
    r = (arr[:, :, 0] & 0xF8) << 8
    g = (arr[:, :, 1] & 0xFC) << 3
    b = arr[:, :, 2] >> 3
    rgb565 = (r | g | b).astype(">u2")  # 大端 uint16
    return rgb565.tobytes()


def main():
    parser = argparse.ArgumentParser(description="开机动画帧转换")
    parser.add_argument("frames_dir", help="帧图片目录")
    parser.add_argument("output_dir", help="输出目录")
    parser.add_argument("--fps", type=int, default=13, help="目标帧率(默认 13)")
    parser.add_argument("--frames", type=int, default=32, help="最大帧数(默认 32)")
    parser.add_argument("--mode", choices=["fit", "fill"], default="fit",
                        help="fit= pillarbox 留黑边(默认); fill=填满裁剪")
    args = parser.parse_args()

    frames_dir = Path(args.frames_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 清掉旧帧,避免新旧混用
    for old in output_dir.glob("boot_*.bin"):
        old.unlink()
    for old in output_dir.glob("frame_index.bin"):
        old.unlink()

    files = sorted(frames_dir.glob("*.png")) or sorted(frames_dir.glob("*.jpg"))
    if not files:
        print(f"错误: 在 {frames_dir} 中没有找到帧图片")
        return

    print(f"找到 {len(files)} 帧, 目标 {args.frames} 帧 @ {args.fps}fps, 模式 {args.mode}")

    # 全程均匀抽样(含首尾)
    n = min(args.frames, len(files))
    if len(files) <= n:
        idx = list(range(len(files)))
    else:
        idx = [round(i * (len(files) - 1) / (n - 1)) for i in range(n)]
        # 去重保序
        seen, uniq = set(), []
        for i in idx:
            if i not in seen:
                seen.add(i)
                uniq.append(i)
        idx = uniq

    print(f"抽取 {len(idx)} 帧, 动画时长约 {len(idx)/args.fps:.2f} 秒")

    total = 0
    for i, fi in enumerate(idx):
        raw = convert_frame(Image.open(files[fi]), args.mode)
        (output_dir / f"boot_{i:03d}.bin").write_bytes(raw)
        total += len(raw)
        if (i + 1) % 8 == 0 or i == len(idx) - 1:
            print(f"  已转换 {i+1}/{len(idx)}")

    # 帧索引: 帧数(u16) + 帧率(u16) + 每帧大小(u32)
    (output_dir / "frame_index.bin").write_bytes(
        struct.pack("<HHI", len(idx), args.fps, SCREEN_W * SCREEN_H * 2))

    print(f"完成! 共 {len(idx)} 帧, 总大小 {total} 字节 ({total/1048576:.2f} MB)")


if __name__ == "__main__":
    main()
