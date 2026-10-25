#pragma once

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 外壳：顶栏 + 内容区 + 底部导航。
 * 页面只往内容区里画，切页/选中态/息屏都由这里统一处理。 */

void ui_shell_init(void);

/* 切到指定页面：首次进入时 create，之后只做 hide/show；语言切换时整体重建 */
void ui_shell_show(const ui_page_t *page);

/* 语言/字号变化后整屏重建（会重建 chrome 与当前页） */
void ui_shell_rebuild(void);

/* 只重绘顶栏与导航文案，不重建页面（一般由 rebuild 内部调用） */
void ui_shell_refresh_chrome(void);

const ui_page_t *ui_shell_current(void);

#ifdef __cplusplus
}
#endif
