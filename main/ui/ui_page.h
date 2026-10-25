#pragma once

#include "lvgl.h"
#include "i18n.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个页面就是一组「往内容容器里画控件」的函数。
 * 页面不持有顶栏/底栏，也不负责切页——那是 ui_shell 的事。 */
typedef struct {
    const char *id;
    i18n_id_t   name;      /* 底栏标签 + 顶栏标题 */
    const char *symbol;    /* LV_SYMBOL_xxx，图标用 */
    void (*create)(lv_obj_t *content);
    void (*destroy)(void); /* 可空：页面自己申请的资源在这里释放 */
} ui_page_t;

#ifdef __cplusplus
}
#endif
