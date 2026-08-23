#!/usr/bin/env python3
"""
电子书管理工具 - 用于准备 SPIFFS 数据目录

用法:
    python ebook_tool.py list                    # 列出 spiffs_data 中的文件
    python ebook_tool.py import <file.txt>       # 导入电子书到 spiffs_data/book.txt
    python ebook_tool.py info                    # 显示电子书信息
    python ebook_tool.py generate-pages          # 生成分页预览(仅测试用)
"""
import os
import sys
import shutil

SPIFFS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spiffs_data")
BOOK_FILE = os.path.join(SPIFFS_DIR, "book.txt")
CHARS_PER_PAGE = 400


def ensure_dir():
    os.makedirs(SPIFFS_DIR, exist_ok=True)


def cmd_list():
    """列出 spiffs_data 目录中的文件"""
    ensure_dir()
    files = os.listdir(SPIFFS_DIR)
    if not files:
        print("spiffs_data/ 目录为空")
        return
    print(f"spiffs_data/ 目录中的文件 ({len(files)} 个):")
    for f in sorted(files):
        path = os.path.join(SPIFFS_DIR, f)
        size = os.path.getsize(path)
        print(f"  {f:30s}  {size:>8d} 字节")


def cmd_import(filepath):
    """导入电子书到 spiffs_data/book.txt"""
    if not os.path.isfile(filepath):
        print(f"错误: 文件不存在: {filepath}")
        return False

    ensure_dir()

    # 检查文件编码(尝试 UTF-8)
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            content = f.read()
    except UnicodeDecodeError:
        # 尝试 GBK
        try:
            with open(filepath, "r", encoding="gbk") as f:
                content = f.read()
            print("检测到 GBK 编码,已转换为 UTF-8")
        except UnicodeDecodeError:
            print("错误: 无法识别文件编码,请转换为 UTF-8 后重试")
            return False

    # 保存为 UTF-8
    with open(BOOK_FILE, "w", encoding="utf-8") as f:
        f.write(content)

    size = os.path.getsize(BOOK_FILE)
    pages = (len(content) + CHARS_PER_PAGE - 1) // CHARS_PER_PAGE
    print(f"已导入: {filepath}")
    print(f"  保存位置: spiffs_data/book.txt")
    print(f"  文件大小: {size} 字节")
    print(f"  字符数量: {len(content)}")
    print(f"  估算页数: 约 {pages} 页")
    return True


def cmd_info():
    """显示当前电子书信息"""
    if not os.path.isfile(BOOK_FILE):
        print("尚未导入电子书 (spiffs_data/book.txt 不存在)")
        return

    size = os.path.getsize(BOOK_FILE)
    with open(BOOK_FILE, "r", encoding="utf-8") as f:
        content = f.read()

    lines = content.count("\n") + 1
    chars = len(content)
    pages = (chars + CHARS_PER_PAGE - 1) // CHARS_PER_PAGE

    print("当前电子书信息:")
    print(f"  文件: spiffs_data/book.txt")
    print(f"  大小: {size} 字节 ({size/1024:.1f} KB)")
    print(f"  行数: {lines}")
    print(f"  字符: {chars}")
    print(f"  估算页数: 约 {pages} 页")
    print(f"  前 80 字: {content[:80].replace(chr(10), ' ')}...")


def count_utf8_chars(data):
    """计算字节数据中的 UTF-8 字符数"""
    count = 0
    for b in data:
        if (b & 0xC0) != 0x80:
            count += 1
    return count


def cmd_generate_pages():
    """生成分页预览(用于测试分页逻辑)"""
    if not os.path.isfile(BOOK_FILE):
        print("错误: 请先导入电子书")
        return

    with open(BOOK_FILE, "rb") as f:
        data = f.read()

    pages = []
    pos = 0
    file_size = len(data)

    while pos < file_size:
        # 读取一页
        buf = bytearray()
        char_count = 0
        page_filled = False

        while len(buf) < 1024 and pos + len(buf) < file_size:
            c = data[pos + len(buf)]
            buf.append(c)

            if (c & 0xC0) != 0x80:
                char_count += 1

            if page_filled and c == ord('\n'):
                break

            if char_count >= CHARS_PER_PAGE and not page_filled:
                page_filled = True
                if c == ord('\n'):
                    break

        pages.append(bytes(buf))
        pos += len(buf)

    print(f"共 {len(pages)} 页")
    print(f"文件总大小: {file_size} 字节")
    total_read = sum(len(p) for p in pages)
    print(f"分页总大小: {total_read} 字节")
    print(f"连续性验证: {'✓ 通过' if total_read == file_size else '✗ 失败'}")

    # 打印前 3 页预览
    for i, page in enumerate(pages[:3]):
        text = page.decode('utf-8', errors='replace')
        preview = text[:60].replace('\n', ' ↵ ')
        print(f"\n第 {i+1} 页 ({len(page)} 字节): {preview}...")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    cmd = sys.argv[1]

    if cmd == "list":
        cmd_list()
    elif cmd == "import":
        if len(sys.argv) < 3:
            print("用法: python ebook_tool.py import <file.txt>")
            return
        cmd_import(sys.argv[2])
    elif cmd == "info":
        cmd_info()
    elif cmd == "generate-pages":
        cmd_generate_pages()
    else:
        print(f"未知命令: {cmd}")
        print(__doc__)


if __name__ == "__main__":
    main()
