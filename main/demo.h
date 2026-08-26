// main/demo.h —— 每个演示页实现的统一接口。
// 新增一个演示页 = 实现这三个函数 + 在 main.c 的 DEMOS[] 里加一行。
#pragma once

#include "bsp_button.h"

typedef struct {
    const char *name;
    void (*enter)(void);                          // 建自己的屏并载入
    void (*exit)(void);                           // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键(长按确定已被 main 拦截)
} demo_entry_t;

// 各演示页(定义在各自的 .c 里)
void demo_wifi_enter(void);  void demo_wifi_exit(void);
void demo_wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_ebook_enter(void); void demo_ebook_exit(void);
void demo_ebook_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_ai_enter(void); void demo_ai_exit(void);
void demo_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev);

void demo_about_enter(void); void demo_about_exit(void);
void demo_about_key(bsp_btn_t btn, bsp_btn_ev_t ev);
