#!/usr/bin/env python3
"""
开机动画帧转换工具(RLE 压缩版)
将视频帧(PNG/JPG)转换为 ESP32 可播放的 RGB565 帧,支持:
  - rotate 模式:横版视频旋转90°铺满竖屏(设备横握观看)
  - fit 模式:等宽缩放居中,上下黑边
  - fill 模式:等比放大填满并居中裁剪
帧文件格式: [u16 LE flag] + payload
  flag=0: payload 为原始 RGB565(大端) 240x320
  flag=1: payload 为 RLE 流,每项 4 字节 = [u16 BE 像素数][u16 BE RGB565 颜色]

用法:
    python boot_anim_convert.py <frames_dir> <output_dir> --frames 70 --mode rotate
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
FLAG_RAW = 0
FLAG_RLE = 1


def preprocess(img, mode):
    """返回 240x320 的 RGB 图像。保留原始画质,不做模糊或量化。"""
    img = img.convert("RGB")
    src_w, src_h = img.size

    if mode == "rotate":  # 旋转90°铺满(横版视频 -> 竖屏全屏,设备横握)
        img = img.transpose(Image.ROTATE_90)
        src_w, src_h = img.size
        scale = max(SCREEN_W / src_w, SCREEN_H / src_h)
        new_w = max(SCREEN_W, round(src_w * scale))
        new_h = max(SCREEN_H, round(src_h * scale))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        left = (new_w - SCREEN_W) // 2
        top = (new_h - SCREEN_H) // 2
        img = img.crop((left, top, left + SCREEN_W, top + SCREEN_H))
    elif mode == "fit":  # pillarbox
        scale = min(SCREEN_W / src_w, SCREEN_H / src_h)
        new_w = max(1, round(src_w * scale))
        new_h = max(1, round(src_h * scale))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        canvas = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
        canvas.paste(img, ((SCREEN_W - new_w) // 2, (SCREEN_H - new_h) // 2))
        img = canvas
    else:  # fill
        scale = max(SCREEN_W / src_w, SCREEN_H / src_h)
        new_w = max(SCREEN_W, round(src_w * scale))
        new_h = max(SCREEN_H, round(src_h * scale))
        img = img.resize((new_w, new_h), Image.LANCZOS)
        left = (new_w - SCREEN_W) // 2
        top = (new_h - SCREEN_H) // 2
        img = img.crop((left, top, left + SCREEN_W, top + SCREEN_H))

    return img


def to_rgb565(img):
    arr = np.asarray(img, dtype=np.uint16)
    r = (arr[:, :, 0] & 0xF8) << 8
    g = (arr[:, :, 1] & 0xFC) << 3
    b = arr[:, :, 2] >> 3
    return (r | g | b).astype(">u2")  # 大端


def rle_encode(pix):
    """pix: 大端 uint16 一维数组。返回 (flag, payload bytes)"""
    flat = pix.reshape(-1).astype(np.uint16)   # 转本机字节序,否则 np.diff 结果错误
    if len(flat) == 0:
        return FLAG_RAW, pix.tobytes()
    idx = np.flatnonzero(np.diff(flat)) + 1
    starts = np.concatenate(([0], idx))
    ends = np.concatenate((idx, [len(flat)]))
    counts = (ends - starts).astype(np.uint64)
    colors = flat[starts]

    # 拆分超过 65535 的游程
    while (counts > 65535).any():
        i = int(np.argmax(counts))
        c = int(counts[i])
        half = c // 2
        counts = np.insert(np.delete(counts, i), i, [half, c - half])
        colors = np.insert(np.delete(colors, i), i, [colors[i], colors[i]])

    # np.stack 会丢弃字节序,必须预分配 '>u2' 数组赋值
    pairs = np.empty((len(counts), 2), dtype=">u2")
    pairs[:, 0] = counts
    pairs[:, 1] = colors
    payload = pairs.tobytes()
    if len(payload) + 2 < flat.nbytes:
        return FLAG_RLE, payload
    return FLAG_RAW, flat.tobytes()


def main():
    parser = argparse.ArgumentParser(description="开机动画帧转换(RLE)")
    parser.add_argument("frames_dir", help="帧图片目录")
    parser.add_argument("output_dir", help="输出目录")
    parser.add_argument("--fps", type=int, default=13, help="目标帧率(默认 13)")
    parser.add_argument("--frames", type=int, default=70, help="最大帧数(默认 70)")
    parser.add_argument("--mode", choices=["rotate", "fit", "fill"], default="rotate")
    args = parser.parse_args()

    frames_dir = Path(args.frames_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    for old in output_dir.glob("boot_*.bin"):
        old.unlink()
    for old in output_dir.glob("frame_index.bin"):
        old.unlink()

    files = sorted(frames_dir.glob("*.png")) or sorted(frames_dir.glob("*.jpg"))
    if not files:
        print(f"错误: 在 {frames_dir} 中没有找到帧图片")
        return

    print(f"找到 {len(files)} 帧, 目标 {args.frames} 帧 @ {args.fps}fps, 模式 {args.mode}")

    n = min(args.frames, len(files))
    if len(files) <= n:
        idx = list(range(len(files)))
    else:
        idx = [round(i * (len(files) - 1) / (n - 1)) for i in range(n)]
        seen, uniq = set(), []
        for i in idx:
            if i not in seen:
                seen.add(i)
                uniq.append(i)
        idx = uniq

    print(f"抽取 {len(idx)} 帧, 播放时长约 {len(idx)/args.fps:.2f} 秒")

    total = 0
    for i, fi in enumerate(idx):
        img = preprocess(Image.open(files[fi]), args.mode)
        pix = to_rgb565(img)
        flag, payload = rle_encode(pix)
        data = struct.pack("<H", flag) + payload
        (output_dir / f"boot_{i:03d}.bin").write_bytes(data)
        total += len(data)
        if (i + 1) % 10 == 0 or i == len(idx) - 1:
            print(f"  已转换 {i+1}/{len(idx)} (累计 {total/1048576:.2f} MB)")

    (output_dir / "frame_index.bin").write_bytes(
        struct.pack("<HHI", len(idx), args.fps, SCREEN_W * SCREEN_H * 2))

    print(f"完成! 共 {len(idx)} 帧, 总大小 {total} 字节 ({total/1048576:.2f} MB)")


if __name__ == "__main__":
    main()
