#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 界面入口：必须在 LVGL 初始化之后、持有 LVGL 锁时调用 */
void ui_init(void);

/* 语言等全局样式变化后整屏重建（必须从 LVGL 任务上下文调用） */
void ui_rebuild(void);

#ifdef __cplusplus
}
#endif
