#pragma once

#include "lvgl.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  通用控件。所有颜色/间距都来自 ui_theme.h，页面里不要再写裸值。
 *
 *  表面分四级：BG → SURFACE（面板）→ SURFACE_HI（面板内卡片）→ TILE（控件）。
 *  刻意不用投影：1024×600 全屏重绘时阴影是纯开销，层次靠明度和描边表达。
 * ========================================================================== */

/* 大面板：设置页左右两栏、设备状态页的大卡片 */
lv_obj_t *ui_panel_create(lv_obj_t *parent, int x, int y, int w, int h);

/* 面板内卡片：比面板亮一级，带 1 px 极淡描边 */
lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h);

/* 渐变方块：品牌标/主按钮这类需要 靛蓝→紫罗兰 的元素 */
lv_obj_t *ui_gradient_tile_create(lv_obj_t *parent, int x, int y, int w, int h,
                                  int radius, uint32_t top, uint32_t bottom);

/* 图标色块：size×size 圆角方块，背景 tint 15%，图标用指定颜色 */
lv_obj_t *ui_badge_create(lv_obj_t *parent, int x, int y, int size,
                          const char *symbol, uint32_t color);
void ui_badge_set_color(lv_obj_t *badge, uint32_t color);

/* 1 px 分隔线 */
lv_obj_t *ui_divider_create(lv_obj_t *parent, int x, int y, int w);

/* 数值胶囊：右侧的「80%」「已连接」小药丸，out_label 可空 */
lv_obj_t *ui_chip_create(lv_obj_t *parent, const char *text, uint32_t color,
                         lv_obj_t **out_label);

/* ---------- 基础容器 ---------- */
lv_obj_t *ui_tile_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h);
lv_obj_t *ui_dot_create(lv_obj_t *parent, int x, int y, int size);
void ui_dot_set_color(lv_obj_t *dot, uint32_t color);

/* 细进度条：permille 0–1000 */
lv_obj_t *ui_progress_create(lv_obj_t *parent, int x, int y, int w, int h,
                             int permille, uint32_t color);
void ui_progress_set_value(lv_obj_t *bar, int permille);

/* ---------- 文本 ---------- */
lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, int x, int y,
                          uint32_t color, const lv_font_t *font);
void ui_label_set_text_color(lv_obj_t *label, uint32_t color);

/* ---------- 控件 ---------- */
lv_obj_t *ui_slider_create(lv_obj_t *parent, int x, int y, int w,
                           int min, int max, int value);

/* 列表行：整行底色 + 圆角，返回容器，调用方往里放标题和控件 */
lv_obj_t *ui_list_row_create(lv_obj_t *parent, int x, int y, int w, int h);

/* 分组小标题（灰色小节标题） */
lv_obj_t *ui_section_title(lv_obj_t *parent, const char *text, int x, int y);

/* 手机式菜单行：图标 + 标题 + 右侧值 + 箭头，可点击。
 * out_value 可空；非空时返回右侧值标签，方便后续改文字。 */
lv_obj_t *ui_menu_row_create(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *symbol, const char *title, const char *value,
                             lv_obj_t **out_value, lv_event_cb_t cb, void *user_data);

/* 按钮：kind = 0 次要（TILE 底）/ 1 主按钮（实心强调色）/ 2 危险（红色浅底） */
lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           int kind);

/* 分段选择器：点击子按钮后向自身发 LV_EVENT_VALUE_CHANGED，
 * 调用方在那个事件里用 ui_segmented_get_selected() 取值 */
lv_obj_t *ui_segmented_create(lv_obj_t *parent, int x, int y, int w, int h,
                              const char *const *labels, int count, int selected);
int ui_segmented_get_selected(lv_obj_t *seg);
void ui_segmented_set_selected(lv_obj_t *seg, int index);

/* 二次确认弹层：铺满 parent，点确认先回调 on_confirm 再关闭；点取消直接关闭 */
void ui_confirm_create(lv_obj_t *parent, const char *text, lv_event_cb_t on_confirm);

/* 进场动画：只改 translate，不做透明度混合，整页移动也很便宜。
 * - slide_in：从右侧 from_x 像素滑入，300 ms easeOutCubic（切页用）
 * - rise_in：从下方 16 px 托起，280 ms easeOutCubic（设置页切分类用） */
void ui_animate_slide_in(lv_obj_t *obj, int from_x);
void ui_animate_rise_in(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
