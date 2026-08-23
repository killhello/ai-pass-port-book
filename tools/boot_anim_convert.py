#!/usr/bin/env python3
"""
开机动画帧转换工具
将 JPG 视频帧转换为 ESP32 可直接播放的 RGB565 格式

用法:
    python boot_anim_convert.py <frames_dir> <output_dir> [--fps 10] [--frames 30]
"""
import os
import sys
import struct
import argparse
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("需要安装 Pillow: pip install Pillow")
    sys.exit(1)

SCREEN_W = 240
SCREEN_H = 320


def rgb_to_rgb565(r, g, b):
    """RGB888 转 RGB565 (16位, 大端序)"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_frame(jpg_path):
    """将单帧 JPG 转换为 RGB565 原始数据"""
    img = Image.open(jpg_path)
    # 确保尺寸正确，缩放并居中裁剪
    img = img.convert('RGB')
    
    # 按比例缩放到填满屏幕
    src_w, src_h = img.size
    scale = max(SCREEN_W / src_w, SCREEN_H / src_h)
    new_w = int(src_w * scale)
    new_h = int(src_h * scale)
    img = img.resize((new_w, new_h), Image.LANCZOS)
    
    # 居中裁剪到 240x320
    left = (new_w - SCREEN_W) // 2
    top = (new_h - SCREEN_H) // 2
    img = img.crop((left, top, left + SCREEN_W, top + SCREEN_H))
    
    # 转换为 RGB565 (大端序，与 ST7789 匹配)
    pixels = img.load()
    raw = bytearray()
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            r, g, b = pixels[x, y]
            rgb565 = rgb_to_rgb565(r, g, b)
            raw.extend(struct.pack('>H', rgb565))  # 大端
    
    return bytes(raw)


def main():
    parser = argparse.ArgumentParser(description='开机动画帧转换')
    parser.add_argument('frames_dir', help='JPG 帧目录')
    parser.add_argument('output_dir', help='输出目录')
    parser.add_argument('--fps', type=int, default=10, help='目标帧率 (默认 10fps)')
    parser.add_argument('--max-frames', type=int, default=30, help='最大帧数 (默认 30)')
    args = parser.parse_args()

    frames_dir = Path(args.frames_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 收集所有帧文件
    jpg_files = sorted(frames_dir.glob('*.jpg'))
    if not jpg_files:
        jpg_files = sorted(frames_dir.glob('*.png'))
    
    if not jpg_files:
        print(f"错误: 在 {frames_dir} 中没有找到帧图片")
        return

    print(f"找到 {len(jpg_files)} 帧")
    print(f"目标帧率: {args.fps} fps")
    print(f"最大帧数: {args.max_frames}")

    # 计算抽帧步长
    total_frames = len(jpg_files)
    if total_frames <= args.max_frames:
        step = 1
    else:
        step = total_frames // args.max_frames
    
    selected = jpg_files[::step]
    # 确保不超过 max_frames
    if len(selected) > args.max_frames:
        selected = selected[:args.max_frames]
    
    print(f"将抽取 {len(selected)} 帧")
    duration = len(selected) / args.fps
    print(f"动画时长: 约 {duration:.1f} 秒")

    # 转换每一帧
    total_size = 0
    frame_info = []
    
    for i, jpg_path in enumerate(selected):
        raw_data = convert_frame(jpg_path)
        out_name = f"boot_{i:03d}.bin"
        out_path = output_dir / out_name
        out_path.write_bytes(raw_data)
        
        total_size += len(raw_data)
        frame_info.append((out_name, len(raw_data)))
        
        if (i + 1) % 10 == 0 or i == len(selected) - 1:
            print(f"  已转换 {i+1}/{len(selected)} 帧")

    # 生成帧索引文件 (frame_index.bin)
    # 格式: uint16_t frame_count; uint32_t frame_size; 
    index_data = struct.pack('<H', len(selected))  # 帧数(小端)
    index_data += struct.pack('<H', args.fps)     # 帧率
    index_data += struct.pack('<I', len(raw_data)) # 每帧大小(固定)
    
    index_path = output_dir / "frame_index.bin"
    index_path.write_bytes(index_data)
    
    print(f"\n完成!")
    print(f"  输出目录: {output_dir}")
    print(f"  帧数: {len(selected)}")
    print(f"  每帧大小: {len(raw_data)} 字节 ({len(raw_data)/1024:.1f} KB)")
    print(f"  总大小: {total_size} 字节 ({total_size/1024:.1f} KB / {total_size/1024/1024:.2f} MB)")
    print(f"  帧率: {args.fps} fps")
    print(f"  时长: {duration:.1f} 秒")
    print(f"\n  将 {output_dir} 目录下的文件复制到 spiffs_data/boot_anim/")
    print(f"  然后重新生成 SPIFFS 镜像: idf.py spiffsgen")


if __name__ == '__main__':
    main()
