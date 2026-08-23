// main/ebook_reader.h —— 电子书阅读器核心逻辑:分页读取 + 进度保存。
// 设计原则:大文件不一次性加载,只维护当前页内容和文件偏移量。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBOOK_MAX_PATH     64     // 最大文件路径长度
#define EBOOK_PAGE_BUF     1024   // 单页缓冲大小(字节),足够装下 240x320 屏的文字

typedef struct {
    char     path[EBOOK_MAX_PATH];   // 当前书籍路径
    uint32_t file_size;              // 文件总大小
    uint32_t page_pos;               // 当前页在文件中的起始偏移
    uint32_t total_pages;            // 估算总页数(按平均字符数估算)
    uint32_t current_page;           // 当前页码(从 0 开始)
    char     page_buf[EBOOK_PAGE_BUF]; // 当前页文本缓冲
    int      page_len;               // 当前页实际字节数
    int      chars_per_page;         // 每页大约字符数(用于估算页数)
    bool     is_open;                // 是否已打开文件
} ebook_reader_t;

// 初始化阅读器
void ebook_reader_init(ebook_reader_t *r);

// 打开一本电子书(仅记录路径和大小,不读取全部内容)
// 返回 true 表示成功
bool ebook_reader_open(ebook_reader_t *r, const char *path);

// 关闭当前书籍
void ebook_reader_close(ebook_reader_t *r);

// 读取当前页内容到 page_buf,返回实际读取的字节数
// 从 page_pos 开始读取,遇到页边界(换行或字符数限制)停止
int ebook_reader_read_page(ebook_reader_t *r);

// 翻到下一页;返回 false 表示已到最后一页
bool ebook_reader_next_page(ebook_reader_t *r);

// 翻到上一页;返回 false 表示已到第一页
bool ebook_reader_prev_page(ebook_reader_t *r);

// 跳转到指定页(0-based)
bool ebook_reader_goto_page(ebook_reader_t *r, uint32_t page);

// ===== 阅读进度保存(NVS) =====

// 保存当前阅读进度到 NVS
bool ebook_reader_save_progress(ebook_reader_t *r);

// 从 NVS 加载阅读进度;返回 true 表示有保存的进度
bool ebook_reader_load_progress(ebook_reader_t *r);

// 清除保存的进度
void ebook_reader_clear_progress(void);
