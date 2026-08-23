// tests/test_ebook_reader.c —— 电子书阅读器分页逻辑的主机端测试。
// 用标准 C 文件 I/O 模拟,验证翻页、分页边界等纯逻辑部分。
//
// 编译运行:
//   cc -std=c11 -Wall -Wextra -Imain tests/test_ebook_reader.c \
//      main/ebook_reader.c -o /tmp/test_ebook_reader
//   /tmp/test_ebook_reader

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// 屏蔽 ESP-IDF 相关的头文件,用标准库替代 NVS 部分
#define ESP_LOGE(tag, fmt, ...)  fprintf(stderr, "E: " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)  fprintf(stdout, "I: " fmt "\n", ##__VA_ARGS__)

// 用简单的内存结构模拟 NVS
static struct {
    char path[64];
    unsigned int page_pos;
    unsigned int cur_page;
    int valid;
} s_fake_nvs = {0};

// 重新定义 ebook_reader.h 中用到的 ESP 类型
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define esp_err_to_name(x) ("unknown")

// 把 nvs 相关函数替换为假实现
#define nvs_handle_t int
#define NVS_READWRITE 1
#define NVS_READONLY  0

static int s_nvs_opened = 0;

static int nvs_open(const char *ns, int mode, int *handle) {
    (void)ns; (void)mode;
    *handle = 1;
    s_nvs_opened = 1;
    return 0;
}

static void nvs_close(int handle) {
    (void)handle;
    s_nvs_opened = 0;
}

static int nvs_set_str(int handle, const char *key, const char *str) {
    (void)handle;
    if (strcmp(key, "path") == 0) {
        strncpy(s_fake_nvs.path, str, sizeof(s_fake_nvs.path) - 1);
        s_fake_nvs.path[sizeof(s_fake_nvs.path) - 1] = '\0';
    }
    return 0;
}

static int nvs_set_u32(int handle, const char *key, unsigned int val) {
    (void)handle;
    if (strcmp(key, "page_pos") == 0) s_fake_nvs.page_pos = val;
    if (strcmp(key, "cur_page") == 0) s_fake_nvs.cur_page = val;
    return 0;
}

static int nvs_commit(int handle) {
    (void)handle;
    s_fake_nvs.valid = 1;
    return 0;
}

static int nvs_get_str(int handle, const char *key, char *buf, size_t *len) {
    (void)handle;
    if (!s_fake_nvs.valid) return 1;
    if (strcmp(key, "path") == 0) {
        size_t needed = strlen(s_fake_nvs.path) + 1;
        if (*len < needed) return 1;
        strcpy(buf, s_fake_nvs.path);
        *len = needed;
        return 0;
    }
    return 1;
}

static int nvs_get_u32(int handle, const char *key, unsigned int *val) {
    (void)handle;
    if (!s_fake_nvs.valid) return 1;
    if (strcmp(key, "page_pos") == 0) { *val = s_fake_nvs.page_pos; return 0; }
    if (strcmp(key, "cur_page") == 0) { *val = s_fake_nvs.cur_page; return 0; }
    return 1;
}

static void nvs_erase_all(int handle) {
    (void)handle;
    s_fake_nvs.valid = 0;
    memset(&s_fake_nvs, 0, sizeof(s_fake_nvs));
}

// 包含实现文件(直接包含 .c 以便替换 NVS 函数)
#include "ebook_reader.c"

// ---- 测试辅助函数 ----

static void create_test_file(const char *path, int lines) {
    FILE *f = fopen(path, "w");
    assert(f);
    for (int i = 1; i <= lines; i++) {
        fprintf(f, "Line %d: Hello world, this is a test line for ebook reader.\n", i);
    }
    fclose(f);
}

static void test_basic_open_read(void) {
    printf("=== test_basic_open_read ===\n");
    const char *path = "/tmp/test_ebook_1.txt";
    create_test_file(path, 50);

    ebook_reader_t r;
    ebook_reader_init(&r);

    bool ok = ebook_reader_open(&r, path);
    assert(ok);
    assert(r.is_open);
    assert(r.file_size > 0);
    printf("  文件大小: %u 字节, 估算页数: %u\n", r.file_size, r.total_pages);

    int len = ebook_reader_read_page(&r);
    assert(len > 0);
    printf("  第一页 %d 字节, 开头: %.40s\n", len, r.page_buf);

    ebook_reader_close(&r);
    remove(path);
    printf("  PASS\n\n");
}

static void test_next_prev_page(void) {
    printf("=== test_next_prev_page ===\n");
    const char *path = "/tmp/test_ebook_2.txt";
    create_test_file(path, 100);  // 100 行,应该有好几页

    ebook_reader_t r;
    ebook_reader_init(&r);
    ebook_reader_open(&r, path);
    ebook_reader_read_page(&r);

    uint32_t first_pos = r.page_pos;
    printf("  第 1 页起始偏移: %u\n", first_pos);

    // 翻到下一页
    bool ok = ebook_reader_next_page(&r);
    assert(ok);
    assert(r.page_pos > first_pos);
    printf("  第 2 页起始偏移: %u\n", r.page_pos);

    // 再翻一页
    ok = ebook_reader_next_page(&r);
    assert(ok);
    printf("  第 3 页起始偏移: %u\n", r.page_pos);

    // 翻回上一页
    ok = ebook_reader_prev_page(&r);
    assert(ok);
    printf("  回到第 2 页偏移: %u (应等于上面第 2 页)\n", r.page_pos);

    // 再翻回第一页
    ok = ebook_reader_prev_page(&r);
    assert(ok);
    printf("  回到第 1 页偏移: %u (应等于 %u)\n", r.page_pos, first_pos);
    assert(r.page_pos == first_pos);

    // 第一页再往前翻应该失败
    ok = ebook_reader_prev_page(&r);
    assert(!ok);
    printf("  第一页往前翻返回 false ✓\n");

    ebook_reader_close(&r);
    remove(path);
    printf("  PASS\n\n");
}

static void test_page_content_continuous(void) {
    printf("=== test_page_content_continuous ===\n");
    const char *path = "/tmp/test_ebook_3.txt";

    // 创建一个内容连续的文件,每页的开头应该接上上一页的结尾
    FILE *f = fopen(path, "w");
    for (int i = 0; i < 200; i++) {
        fprintf(f, "[%03d]", i);
    }
    fprintf(f, "\n");
    fclose(f);

    ebook_reader_t r;
    ebook_reader_init(&r);
    ebook_reader_open(&r, path);

    // 收集所有页的内容
    char all_content[4096] = {0};
    int total_len = 0;

    ebook_reader_read_page(&r);
    memcpy(all_content + total_len, r.page_buf, r.page_len);
    total_len += r.page_len;

    while (ebook_reader_next_page(&r)) {
        memcpy(all_content + total_len, r.page_buf, r.page_len);
        total_len += r.page_len;
    }

    // 读原文件
    f = fopen(path, "r");
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *original = malloc(fsize + 1);
    fread(original, 1, fsize, f);
    original[fsize] = '\0';
    fclose(f);

    printf("  原文件: %ld 字节, 拼接后: %d 字节\n", fsize, total_len);
    printf("  内容匹配: %s\n",
           strncmp(all_content, original, fsize) == 0 ? "YES ✓" : "NO ✗");
    assert(strncmp(all_content, original, fsize) == 0);

    free(original);
    ebook_reader_close(&r);
    remove(path);
    printf("  PASS\n\n");
}

static void test_small_file(void) {
    printf("=== test_small_file ===\n");
    const char *path = "/tmp/test_ebook_small.txt";

    FILE *f = fopen(path, "w");
    fprintf(f, "Short text\n");
    fclose(f);

    ebook_reader_t r;
    ebook_reader_init(&r);
    bool ok = ebook_reader_open(&r, path);
    assert(ok);

    int len = ebook_reader_read_page(&r);
    assert(len > 0);
    printf("  小文件第一页 %d 字节: %s", len, r.page_buf);

    // 小文件下一页应该失败
    ok = ebook_reader_next_page(&r);
    printf("  下一页: %s\n", ok ? "true" : "false");
    assert(!ok);

    ebook_reader_close(&r);
    remove(path);
    printf("  PASS\n\n");
}

static void test_nvs_save_load(void) {
    printf("=== test_nvs_save_load ===\n");
    const char *path = "/tmp/test_ebook_nvs.txt";
    create_test_file(path, 80);

    ebook_reader_t r1;
    ebook_reader_init(&r1);
    ebook_reader_open(&r1, path);
    ebook_reader_read_page(&r1);

    // 翻几页
    ebook_reader_next_page(&r1);
    ebook_reader_next_page(&r1);
    printf("  保存时: page=%u, pos=%u\n", r1.current_page, r1.page_pos);

    // 保存进度
    bool ok = ebook_reader_save_progress(&r1);
    assert(ok);
    ebook_reader_close(&r1);

    // 新阅读器加载进度
    ebook_reader_t r2;
    ebook_reader_init(&r2);
    ok = ebook_reader_load_progress(&r2);
    assert(ok);
    printf("  加载后: page=%u, pos=%u\n", r2.current_page, r2.page_pos);
    assert(r2.current_page == r1.current_page);
    assert(r2.page_pos == r1.page_pos);
    assert(strcmp(r2.path, path) == 0);

    ebook_reader_close(&r2);
    remove(path);

    // 清除 NVS
    ebook_reader_clear_progress();
    printf("  清除后再加载: ");
    ebook_reader_t r3;
    ebook_reader_init(&r3);
    ok = ebook_reader_load_progress(&r3);
    printf("%s\n", ok ? "成功(不正常)" : "失败(正常)");
    assert(!ok);

    printf("  PASS\n\n");
}

int main(void) {
    printf("电子书阅读器单元测试\n");
    printf("====================\n\n");

    test_basic_open_read();
    test_next_prev_page();
    test_page_content_continuous();
    test_small_file();
    test_nvs_save_load();

    printf("所有测试通过 ✓\n");
    return 0;
}
