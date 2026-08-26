// main/ebook_reader.c —— 电子书阅读器核心实现。
// 大文件处理策略:只保存当前页的文件偏移量,翻页时 seek + 局部读取。
#include "ebook_reader.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ebook_reader";

#define NVS_NAMESPACE   "ebook"
#define NVS_KEY_PATH    "path"
#define NVS_KEY_POS     "page_pos"
#define NVS_KEY_PAGE    "cur_page"

// 每页字符数按面板实测: 正文区约 204x224px, 16 号中文字体
// 每行约 12 个汉字、共约 10 行 => 120 字左右; 取 120 防溢出
#define DEFAULT_CHARS_PER_PAGE  120

// ---------------------------------------------------------------------------
// 工具:从文件 pos 处读取一页内容,返回这页的字节数。
// 分页规则:读满 chars_per_page 个字符后,继续读到下一个换行或文件尾。
// 这样避免把一行拆在两页中间。
// ---------------------------------------------------------------------------
static int read_page_from_file(FILE *fp, uint32_t pos, char *buf, int buf_size,
                               int chars_per_page) {
    if (fseek(fp, pos, SEEK_SET) != 0) return 0;

    int total = 0;
    int char_count = 0;
    bool page_filled = false;

    while (total < buf_size - 1) {
        int c = fgetc(fp);
        if (c == EOF) break;

        buf[total++] = (char)c;

        // 统计字符数(ASCII 占 1 字节,UTF-8 多字节字符算 1 个字符)
        if ((c & 0xC0) != 0x80) {  // 不是 UTF-8 续字节
            char_count++;
        }

        // 已经够一页字符数后,再读到换行就停
        if (page_filled && c == '\n') break;

        if (char_count >= chars_per_page && !page_filled) {
            page_filled = true;
            // 如果当前正好是换行,直接停
            if (c == '\n') break;
        }
    }

    buf[total] = '\0';
    return total;
}

// 估算上一页的起始位置:从当前 pos 往回找 chars_per_page 个字符,
// 然后再往回找到最近的行首(换行符之后),避免把一行拆成两页。
static uint32_t find_prev_page_pos(FILE *fp, uint32_t current_pos,
                                    int chars_per_page) {
    if (current_pos == 0) return 0;

    // 先估算搜索范围:往回跳约 2 页的位置,留足够余量
    uint32_t search_start = (current_pos > (uint32_t)(chars_per_page * 2))
                              ? current_pos - chars_per_page * 2
                              : 0;

    int chunk_size = (int)(current_pos - search_start);
    if (chunk_size <= 0) return 0;

    char *chunk = malloc(chunk_size);
    if (!chunk) return 0;

    if (fseek(fp, search_start, SEEK_SET) != 0) { free(chunk); return 0; }
    int nread = (int)fread(chunk, 1, chunk_size, fp);
    if (nread <= 0) { free(chunk); return 0; }

    // 从 chunk 末尾(=current_pos - 1)往回数 chars_per_page 个字符
    int i = nread - 1;
    int chars_back = 0;
    int prev_page_end = nread;  // 上一页的结束位置(=当前页开始的前一个)

    // 第一步:往回数 chars_per_page 个字符
    while (i >= 0 && chars_back < chars_per_page) {
        if ((chunk[i] & 0xC0) != 0x80) {
            chars_back++;
        }
        i--;
    }

    // 第二步:继续往回找,直到找到换行符或文件开头
    // 此时 i 指向第 chars_per_page 个字符的前一个位置
    // 我们要找这之前最近的换行符,上一页从换行符之后开始
    while (i >= 0) {
        if (chunk[i] == '\n') {
            // 找到了换行符,上一页从换行符下一个字节开始
            free(chunk);
            return search_start + i + 1;
        }
        i--;
    }

    // 没找到换行符,说明上一页从文件开头开始
    free(chunk);
    return search_start > 0 ? 0 : search_start;
}

void ebook_reader_init(ebook_reader_t *r) {
    memset(r, 0, sizeof(*r));
    r->chars_per_page = DEFAULT_CHARS_PER_PAGE;
}

bool ebook_reader_open(ebook_reader_t *r, const char *path) {
    if (!path || !path[0]) return false;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "无法打开文件: %s", path);
        return false;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);

    if (size < 0) return false;

    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';
    r->file_size = (uint32_t)size;
    r->page_pos = 0;
    r->current_page = 0;
    r->page_len = 0;
    r->is_open = true;

    // 估算总页数
    r->total_pages = (r->file_size + r->chars_per_page - 1) / r->chars_per_page;
    if (r->total_pages == 0) r->total_pages = 1;

    ESP_LOGI(TAG, "打开书籍: %s, 大小: %lu 字节, 约 %lu 页",
             path, (unsigned long)r->file_size, (unsigned long)r->total_pages);
    return true;
}

void ebook_reader_close(ebook_reader_t *r) {
    r->is_open = false;
    r->page_len = 0;
}

int ebook_reader_read_page(ebook_reader_t *r) {
    if (!r->is_open) return 0;

    FILE *fp = fopen(r->path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "读取页失败:无法打开 %s", r->path);
        return 0;
    }

    r->page_len = read_page_from_file(fp, r->page_pos, r->page_buf,
                                       EBOOK_PAGE_BUF, r->chars_per_page);
    fclose(fp);
    return r->page_len;
}

bool ebook_reader_next_page(ebook_reader_t *r) {
    if (!r->is_open) return false;
    if (r->page_pos + r->page_len >= r->file_size) return false;  // 已到末尾

    uint32_t next_pos = r->page_pos + r->page_len;
    if (next_pos >= r->file_size) return false;

    r->page_pos = next_pos;
    r->current_page++;
    ebook_reader_read_page(r);
    return true;
}

bool ebook_reader_prev_page(ebook_reader_t *r) {
    if (!r->is_open) return false;
    if (r->page_pos == 0) return false;  // 已到开头

    FILE *fp = fopen(r->path, "r");
    if (!fp) return false;

    uint32_t prev_pos = find_prev_page_pos(fp, r->page_pos, r->chars_per_page);
    fclose(fp);

    if (prev_pos >= r->page_pos) return false;  // 没找到上一页

    r->page_pos = prev_pos;
    if (r->current_page > 0) r->current_page--;
    ebook_reader_read_page(r);
    return true;
}

bool ebook_reader_goto_page(ebook_reader_t *r, uint32_t page) {
    if (!r->is_open) return false;

    // 估算目标页的起始位置(粗略估算,实际位置会在翻页时调整)
    uint32_t target_pos = page * r->chars_per_page;
    if (target_pos >= r->file_size) target_pos = r->file_size - 1;

    // 如果目标页在当前页之前,从开头翻过去更准确
    r->page_pos = 0;
    r->current_page = 0;
    ebook_reader_read_page(r);

    for (uint32_t i = 0; i < page; i++) {
        if (!ebook_reader_next_page(r)) break;
    }
    return true;
}

// ===== NVS 进度保存 =====

bool ebook_reader_save_progress(ebook_reader_t *r) {
    if (!r->is_open) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_PATH, r->path);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, NVS_KEY_POS, r->page_pos);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, NVS_KEY_PAGE, r->current_page);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存进度失败: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "进度已保存: %s, 页 %lu, 偏移 %lu",
             r->path, (unsigned long)r->current_page, (unsigned long)r->page_pos);
    return true;
}

bool ebook_reader_load_progress(ebook_reader_t *r) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    char path[EBOOK_MAX_PATH];
    size_t path_len = sizeof(path);
    uint32_t page_pos = 0;
    uint32_t cur_page = 0;

    err = nvs_get_str(handle, NVS_KEY_PATH, path, &path_len);
    if (err == ESP_OK) {
        err = nvs_get_u32(handle, NVS_KEY_POS, &page_pos);
    }
    if (err == ESP_OK) {
        err = nvs_get_u32(handle, NVS_KEY_PAGE, &cur_page);
    }

    nvs_close(handle);

    if (err != ESP_OK) return false;

    // 尝试打开保存的书籍
    if (!ebook_reader_open(r, path)) return false;

    // 跳转到保存的位置
    if (page_pos < r->file_size) {
        r->page_pos = page_pos;
        r->current_page = cur_page;
        ebook_reader_read_page(r);
    }

    ESP_LOGI(TAG, "进度已加载: %s, 页 %lu", path, (unsigned long)cur_page);
    return true;
}

void ebook_reader_clear_progress(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}
