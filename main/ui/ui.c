#include "ui.h"

#include "ai_ui.h"
#include "ui_shell.h"
#include "ui_theme.h"

/* ui 层的唯一入口：把壳（顶栏 + 内容区 + 底栏）建起来，
 * 具体页面由 ui_shell 内部按页面表加载。 */

void ui_init(void)
{
    /* 默认主题是浅色的，键盘/输入框这类内置控件会漏出浅色底。
     * 这里统一切成深色主题，字体沿用中文字库，保证和中文字号一致。 */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        lv_theme_t *theme = lv_theme_default_init(disp,
                                                  lv_palette_main(LV_PALETTE_INDIGO),
                                                  lv_palette_main(LV_PALETTE_CYAN),
                                                  true, UI_FONT_BODY);
        lv_display_set_theme(disp, theme);
    }
    ai_ui_init();
    ui_shell_init();
}

void ui_rebuild(void)
{
    ui_shell_rebuild();
}
