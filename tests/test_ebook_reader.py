"""
电子书阅读器分页逻辑测试
验证翻页、内容连续性等核心逻辑
"""
import os
import tempfile

CHARS_PER_PAGE = 400
PAGE_BUF_SIZE = 1024

def read_page_from_file(fp, pos, chars_per_page):
    """从文件 pos 处读取一页内容,返回 (内容(bytes), 读取字节数)"""
    fp.seek(pos)
    buf = bytearray()
    char_count = 0
    page_filled = False
    
    while len(buf) < PAGE_BUF_SIZE - 1:
        c = fp.read(1)
        if not c:
            break
        buf.extend(c)
        
        # UTF-8 字符计数:非续字节算一个字符
        if (c[0] & 0xC0) != 0x80:
            char_count += 1
        
        # 够一页后,再读到换行就停
        if page_filled and c == b'\n':
            break
        
        if char_count >= chars_per_page and not page_filled:
            page_filled = True
            if c == b'\n':
                break
    
    return bytes(buf), len(buf)


def find_prev_page_pos(fp, current_pos, chars_per_page):
    """找上一页的起始位置"""
    if current_pos == 0:
        return 0
    
    # 往回跳约 2 页的位置开始搜索
    search_start = max(0, current_pos - chars_per_page * 2)
    fp.seek(search_start)
    chunk = fp.read(current_pos - search_start)
    nread = len(chunk)
    
    if nread <= 0:
        return 0
    
    # 第一步:从末尾往回数 chars_per_page 个字符
    i = nread - 1
    chars_back = 0
    
    while i >= 0 and chars_back < chars_per_page:
        if (chunk[i] & 0xC0) != 0x80:
            chars_back += 1
        i -= 1
    
    # 第二步:继续往回找,直到找到换行符或文件开头
    while i >= 0:
        if chunk[i] == ord('\n'):
            return search_start + i + 1
        i -= 1
    
    # 没找到换行符,说明从文件开头开始
    return 0 if search_start > 0 else search_start


def test_basic():
    print("=== 测试1: 基本打开和读取 ===")
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.txt', delete=False) as f:
        for i in range(1, 51):
            f.write(f"Line {i}: Hello world, this is a test line for ebook reader.\n".encode('utf-8'))
        fname = f.name
    
    try:
        with open(fname, 'rb') as fp:
            content, length = read_page_from_file(fp, 0, CHARS_PER_PAGE)
            print(f"  第一页: {length} 字节")
            print(f"  开头: {content[:60]}...")
            assert length > 0
        print("  ✓ 通过\n")
    finally:
        os.unlink(fname)


def test_next_prev():
    print("=== 测试2: 翻页和回翻 ===")
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.txt', delete=False) as f:
        for i in range(1, 101):
            f.write(f"Line {i}: Hello world, this is a test line for ebook reader.\n".encode('utf-8'))
        fname = f.name
    
    try:
        with open(fname, 'rb') as fp:
            # 第一页
            pos = 0
            content1, len1 = read_page_from_file(fp, pos, CHARS_PER_PAGE)
            print(f"  第1页: pos={pos}, len={len1}")
            
            # 第二页
            pos2 = pos + len1
            content2, len2 = read_page_from_file(fp, pos2, CHARS_PER_PAGE)
            print(f"  第2页: pos={pos2}, len={len2}")
            assert pos2 > pos
            
            # 第三页
            pos3 = pos2 + len2
            content3, len3 = read_page_from_file(fp, pos3, CHARS_PER_PAGE)
            print(f"  第3页: pos={pos3}, len={len3}")
            
            # 回翻到第二页
            prev_pos = find_prev_page_pos(fp, pos3, CHARS_PER_PAGE)
            print(f"  回翻到: pos={prev_pos} (预期等于 {pos2})")
            
            # 验证回翻的内容和第二页一样
            prev_content, prev_len = read_page_from_file(fp, prev_pos, CHARS_PER_PAGE)
            assert prev_content == content2, f"回翻内容不一致: {prev_content[:30]} != {content2[:30]}"
            print("  ✓ 回翻内容正确")
            
            # 再回翻到第一页
            prev_pos2 = find_prev_page_pos(fp, prev_pos, CHARS_PER_PAGE)
            print(f"  再回翻到: pos={prev_pos2} (预期等于 {pos})")
            prev_content2, _ = read_page_from_file(fp, prev_pos2, CHARS_PER_PAGE)
            assert prev_content2 == content1, "回翻到第一页内容不一致"
            print("  ✓ 回翻到第一页正确")
            
            # 第一页再往前翻应该返回 0
            prev_pos3 = find_prev_page_pos(fp, 0, CHARS_PER_PAGE)
            assert prev_pos3 == 0
            print("  ✓ 第一页往前翻返回 0")
        
        print("  ✓ 通过\n")
    finally:
        os.unlink(fname)


def test_content_continuity():
    print("=== 测试3: 翻页内容连续性 ===")
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.txt', delete=False) as f:
        for i in range(200):
            f.write(f"[{i:03d}]".encode('utf-8'))
        f.write(b"\n")
        fname = f.name
    
    try:
        with open(fname, 'rb') as fp:
            fp.seek(0, 2)
            fsize = fp.tell()
            
            # 逐页读取并拼接
            all_content = []
            pos = 0
            page_count = 0
            
            while True:
                content, length = read_page_from_file(fp, pos, CHARS_PER_PAGE)
                all_content.append(content)
                page_count += 1
                if pos + length >= fsize:
                    break
                pos += length
            
            full_text = b''.join(all_content)
            print(f"  文件大小: {fsize} 字节, 共 {page_count} 页")
            print(f"  拼接后大小: {len(full_text)} 字节")
            
            # 读原文件对比
            fp.seek(0)
            original = fp.read()
            
            if full_text == original:
                print("  ✓ 翻页拼接内容与原文件完全一致")
            else:
                print(f"  ✗ 内容不匹配!")
                print(f"    original len={len(original)}, full_text len={len(full_text)}")
                # 找第一个不同的位置
                min_len = min(len(full_text), len(original))
                diff_pos = -1
                for i in range(min_len):
                    if full_text[i] != original[i]:
                        diff_pos = i
                        break
                if diff_pos >= 0:
                    print(f"    第一个不同在字节 {diff_pos}")
                    print(f"    original[{diff_pos-10}:{diff_pos+10}] = {repr(original[max(0,diff_pos-10):diff_pos+10])}")
                    print(f"    full_text[{diff_pos-10}:{diff_pos+10}] = {repr(full_text[max(0,diff_pos-10):diff_pos+10])}")
                elif len(full_text) != len(original):
                    print(f"    前面 {min_len} 字节相同,长度不同")
                assert False, "内容不匹配"
        
        print("  ✓ 通过\n")
    finally:
        os.unlink(fname)


def test_small_file():
    print("=== 测试4: 小文件处理 ===")
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.txt', delete=False) as f:
        f.write(b"Short text\n")
        fname = f.name
    
    try:
        with open(fname, 'rb') as fp:
            content, length = read_page_from_file(fp, 0, CHARS_PER_PAGE)
            print(f"  小文件: {length} 字节, 内容: {repr(content)}")
            assert length == len(b"Short text\n")
            assert content == b"Short text\n"
            
            # 下一页应该返回空
            fp.seek(0, 2)
            fsize = fp.tell()
            assert length >= fsize
            print("  ✓ 小文件只有一页")
        
        print("  ✓ 通过\n")
    finally:
        os.unlink(fname)


def test_utf8():
    print("=== 测试5: UTF-8 中文支持 ===")
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.txt', delete=False) as f:
        for i in range(1, 51):
            f.write(f"第{i}行: 这是中文测试内容,用于验证UTF-8分页是否正确。\n".encode('utf-8'))
        fname = f.name
    
    try:
        with open(fname, 'rb') as fp:
            content, length = read_page_from_file(fp, 0, CHARS_PER_PAGE)
            print(f"  第一页: {length} 字节")
            print(f"  开头: {content[:60].decode('utf-8', errors='replace')}...")
            
            # 逐页读取并验证内容连续性
            fp.seek(0, 2)
            fsize = fp.tell()
            
            all_content = []
            pos = 0
            page_count = 0
            while True:
                content, length = read_page_from_file(fp, pos, CHARS_PER_PAGE)
                all_content.append(content)
                page_count += 1
                if pos + length >= fsize:
                    break
                pos += length
            
            full_text = b''.join(all_content)
            fp.seek(0)
            original = fp.read()
            assert full_text == original, "中文内容分页不连续"
            print(f"  ✓ 中文分页连续,共 {page_count} 页")
        
        print("  ✓ 通过\n")
    finally:
        os.unlink(fname)


if __name__ == '__main__':
    print("电子书阅读器分页逻辑测试")
    print("=" * 40 + "\n")
    
    test_basic()
    test_next_prev()
    test_content_continuity()
    test_small_file()
    test_utf8()
    
    print("=" * 40)
    print("所有测试通过 ✓")
