#include "page_home.h"

#include "i18n.h"
#include "ui_widgets.h"

/* 主页：暂时留空。
 * 只放一个很淡的文档图标 + 一行提示，告诉二次开发者业务页应该加在哪里；
 * 等有实际内容后再往 content 里加卡片即可。 */

static void home_create(lv_obj_t *content)
{
    lv_obj_t *icon = ui_label_create(content, LV_SYMBOL_FILE, 0, 0,
                                     UI_COLOR_DISABLED, UI_FONT_ICON);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *hint = ui_label_create(content, i18n_str(STR_HOME_HINT), 0, 0,
                                     UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
}

static void home_destroy(void)
{
}

const ui_page_t page_home = {
    .id = "home",
    .name = STR_NAV_HOME,
    .symbol = LV_SYMBOL_HOME,
    .create = home_create,
    .destroy = home_destroy,
};
