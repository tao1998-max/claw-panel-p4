#pragma once

#include <stdbool.h>

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ui_page_t page_home;

/* 供 AI/MCP 侧通过 ai_ui 事件刷新开关显示（LVGL 任务内调用）。 */
void page_home_apply_demo(bool on);

#ifdef __cplusplus
}
#endif
