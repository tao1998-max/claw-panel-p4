#include "ui_widgets.h"
#include "i18n.h"

/* ==========================================================================
 *  通用控件。所有颜色/间距都来自 ui_theme.h，页面里不要再写裸值。
 * ========================================================================== */

/* 装饰性容器统一处理：不吃点击、不滚动、不带内边距 */
static void decor(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void gradient_bg(lv_obj_t *obj, uint32_t top, uint32_t bottom,
                        lv_grad_dir_t dir)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(obj, dir, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

lv_obj_t *ui_panel_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    /* 大面板用 84% 深色纯色，保留之前那版的面板层次，但不再用渐变。 */
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(panel, 215, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, UI_RADIUS_L, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t *ui_card_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    gradient_bg(card, UI_COLOR_CARD_TOP, UI_COLOR_CARD_BOT, LV_GRAD_DIR_VER);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_M, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_gradient_tile_create(lv_obj_t *parent, int x, int y, int w, int h,
                                  int radius, uint32_t top, uint32_t bottom)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    gradient_bg(tile, top, bottom, LV_GRAD_DIR_VER);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, radius, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    return tile;
}

lv_obj_t *ui_badge_create(lv_obj_t *parent, int x, int y, int size,
                          const char *symbol, uint32_t color)
{
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_set_pos(badge, x, y);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_bg_color(badge, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(badge, 40, 0);              /* ~16% tint */
    lv_obj_set_style_border_color(badge, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(badge, 60, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, size / 3, 0);
    decor(badge);

    lv_obj_t *icon = ui_label_create(badge, symbol, 0, 0, color, UI_FONT_ICON);
    lv_obj_center(icon);
    return badge;
}

void ui_badge_set_color(lv_obj_t *badge, uint32_t color)
{
    if (badge == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(badge, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(color), 0);
    lv_obj_t *icon = lv_obj_get_child(badge, 0);
    if (icon != NULL) {
        ui_label_set_text_color(icon, color);
    }
}

lv_obj_t *ui_divider_create(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    decor(line);
    return line;
}

lv_obj_t *ui_chip_create(lv_obj_t *parent, const char *text, uint32_t color,
                         lv_obj_t **out_label)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(chip, 36, 0);               /* ~14% tint */
    lv_obj_set_style_border_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(chip, 55, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, 12, 0);
    lv_obj_set_style_pad_ver(chip, 5, 0);
    decor(chip);

    lv_obj_t *label = ui_label_create(chip, text, 0, 0, color, UI_FONT_SMALL);
    lv_obj_center(label);
    if (out_label != NULL) {
        *out_label = label;
    }
    return chip;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const char *text, int x, int y,
                          uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

void ui_label_set_text_color(lv_obj_t *label, uint32_t color)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

lv_obj_t *ui_tile_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    if (text != NULL) {
        lv_obj_t *label = ui_label_create(tile, text, 0, 0, UI_COLOR_ACCENT, UI_FONT_SMALL);
        lv_obj_center(label);
    }
    return tile;
}

lv_obj_t *ui_dot_create(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COLOR_DISABLED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    decor(dot);
    return dot;
}

void ui_dot_set_color(lv_obj_t *dot, uint32_t color)
{
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
}

lv_obj_t *ui_slider_create(lv_obj_t *parent, int x, int y, int w,
                           int min, int max, int value)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, w, 10);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);

    /* 轨道 */
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(slider, 0, LV_PART_MAIN);

    /* 已选段：水平渐变，和主按钮呼应 */
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_ACCENT_DEEP),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider, lv_color_hex(UI_COLOR_VIOLET),
                                   LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    /* 旋钮：白芯 + 强调色细环 + 一圈柔光 */
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, 70, LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_x(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(slider, 0, LV_PART_KNOB);
    return slider;
}

lv_obj_t *ui_list_row_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, w, h);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_CARD_TOP), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(row, 80, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *ui_section_title(lv_obj_t *parent, const char *text, int x, int y)
{
    return ui_label_create(parent, text, x, y, UI_COLOR_MUTED, UI_FONT_SMALL);
}

lv_obj_t *ui_menu_row_create(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *symbol, const char *title, const char *value,
                             lv_obj_t **out_value, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = ui_list_row_create(parent, x, y, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (symbol != NULL) {
        lv_obj_t *icon = ui_label_create(row, symbol, 0, 0, UI_COLOR_ACCENT, UI_FONT_ICON);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);
    }

    lv_obj_t *label = ui_label_create(row, title, 0, 0, UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 60, 0);

    lv_obj_t *val = ui_label_create(row, value != NULL ? value : "", 0, 0,
                                    UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -48, 0);
    if (value == NULL) {
        lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *arrow = ui_label_create(row, LV_SYMBOL_RIGHT, 0, 0, UI_COLOR_DISABLED,
                                      UI_FONT_ICON_SMALL);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -20, 0);

    if (cb != NULL) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    if (out_value != NULL) {
        *out_value = val;
    }
    return row;
}

lv_obj_t *ui_button_create(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           int kind)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    uint32_t text_color = UI_COLOR_TEXT;
    if (kind == 1) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT_SOFT),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, LV_STATE_PRESSED);
        text_color = UI_COLOR_ON_ACCENT;
    } else if (kind == 2) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ERROR), 0);
        lv_obj_set_style_bg_opa(btn, 38, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ERROR), 0);
        lv_obj_set_style_border_opa(btn, 90, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_bg_opa(btn, UI_OPA_STRONG, LV_STATE_PRESSED);
        text_color = UI_COLOR_ERROR;
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_TILE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE_HI), LV_STATE_PRESSED);
        text_color = UI_COLOR_MUTED;
    }

    lv_obj_t *label = ui_label_create(btn, text, 0, 0, text_color, UI_FONT_SMALL);
    lv_obj_center(label);
    return btn;
}

/* ---------------- 细进度条 ---------------- */

lv_obj_t *ui_progress_create(lv_obj_t *parent, int x, int y, int w, int h,
                             int permille, uint32_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, permille, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    decor(bar);
    return bar;
}

void ui_progress_set_value(lv_obj_t *bar, int permille)
{
    if (bar == NULL) {
        return;
    }
    if (permille < 0) {
        permille = 0;
    } else if (permille > 1000) {
        permille = 1000;
    }
    lv_bar_set_value(bar, permille, LV_ANIM_OFF);
}

/* ---------------- 轻量页面转场 ---------------- */

static void anim_translate_x_cb(void *obj, int32_t value)
{
    lv_obj_set_style_translate_x((lv_obj_t *)obj, value, 0);
}

static void anim_translate_y_cb(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

/* 用自定义贝塞尔而不是内置 ease_out：内置曲线收尾太陡，整页滑动会显得“顿”。
 * 300 ms + easeOutCubic，起步快、尾巴长，收得住。 */
void ui_animate_slide_in(lv_obj_t *obj, int from_x)
{
    if (obj == NULL) {
        return;
    }
    lv_anim_delete(obj, anim_translate_x_cb);
    lv_obj_set_style_translate_x(obj, from_x, 0);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from_x, 0);
    lv_anim_set_duration(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_custom_bezier3);
    LV_ANIM_SET_EASE_OUT_CUBIC(&anim);
    lv_anim_set_exec_cb(&anim, anim_translate_x_cb);
    lv_anim_start(&anim);
}

/* 设置页切分类：内容从下方轻轻托起，比整块横向平移柔和。 */
void ui_animate_rise_in(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_anim_delete(obj, anim_translate_y_cb);
    lv_obj_set_style_translate_y(obj, 16, 0);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, 16, 0);
    lv_anim_set_duration(&anim, 280);
    lv_anim_set_path_cb(&anim, lv_anim_path_custom_bezier3);
    LV_ANIM_SET_EASE_OUT_CUBIC(&anim);
    lv_anim_set_exec_cb(&anim, anim_translate_y_cb);
    lv_anim_start(&anim);
}

/* ---------------- 分段选择器 ---------------- */

static void seg_apply_style(lv_obj_t *btn, bool selected)
{
    if (selected) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(btn, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(btn, 140, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    }

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        ui_label_set_text_color(label, selected ? UI_COLOR_ON_ACCENT : UI_COLOR_MUTED);
    }
}

static void seg_child_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *seg = lv_obj_get_parent(btn);
    int index = (int)(intptr_t)lv_obj_get_user_data(btn);

    ui_segmented_set_selected(seg, index);
    lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *ui_segmented_create(lv_obj_t *parent, int x, int y, int w, int h,
                              const char *const *labels, int count, int selected)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_set_pos(seg, x, y);
    lv_obj_set_size(seg, w, h);
    lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COLOR_TILE_DARK), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(seg, 0, 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);

    if (count <= 0) {
        count = 1;
    }
    const int pad = 4;
    const int gap = 3;
    int item_w = (w - pad * 2 - (count - 1) * gap) / count;

    for (int i = 0; i < count; i++) {
        int bx = pad + i * (item_w + gap);
        int bw = (i == count - 1) ? (w - pad - bx) : item_w;

        lv_obj_t *btn = lv_obj_create(seg);
        lv_obj_set_pos(btn, bx, pad);
        lv_obj_set_size(btn, bw, h - pad * 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, UI_OPA_SOFT, LV_STATE_PRESSED);

        lv_obj_t *label = ui_label_create(btn, labels[i], 0, 0, UI_COLOR_MUTED,
                                          UI_FONT_SMALL);
        lv_obj_center(label);

        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, seg_child_clicked, LV_EVENT_CLICKED, NULL);
        seg_apply_style(btn, false);
    }

    ui_segmented_set_selected(seg, selected);
    return seg;
}

int ui_segmented_get_selected(lv_obj_t *seg)
{
    return (int)(intptr_t)lv_obj_get_user_data(seg);
}

void ui_segmented_set_selected(lv_obj_t *seg, int index)
{
    uint32_t count = lv_obj_get_child_count(seg);
    if (index < 0 || (uint32_t)index >= count) {
        index = 0;
    }
    lv_obj_set_user_data(seg, (void *)(intptr_t)index);
    for (uint32_t i = 0; i < count; i++) {
        seg_apply_style(lv_obj_get_child(seg, i), (int)i == index);
    }
}

/* ---------------- 二次确认弹层 ---------------- */

static void confirm_cancel_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *scrim = lv_obj_get_parent(lv_obj_get_parent(btn));
    lv_obj_delete(scrim);
}

static void confirm_ok_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_event_cb_t on_confirm = (lv_event_cb_t)lv_obj_get_user_data(btn);
    lv_obj_t *scrim = lv_obj_get_parent(lv_obj_get_parent(btn));

    /* 先拆弹层再回调：回调里可能重建整个界面（切语言/恢复出厂），
     * 如果之后再删 scrim，指针已经失效了。 */
    lv_obj_delete(scrim);
    if (on_confirm != NULL) {
        on_confirm(e);
    }
}

void ui_confirm_create(lv_obj_t *parent, const char *text, lv_event_cb_t on_confirm)
{
    lv_obj_t *scrim = lv_obj_create(parent);
    lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);   /* 吃掉背面的点击 */

    lv_obj_t *box = ui_card_create(scrim, 0, 0, 520, 256);
    lv_obj_center(box);

    lv_obj_t *icon = ui_badge_create(box, 0, 0, 48, LV_SYMBOL_WARNING, UI_COLOR_ERROR);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *label = ui_label_create(box, text, 0, 0, UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_set_width(label, 440);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t *cancel = ui_button_create(box, i18n_str(STR_BTN_CANCEL), 64, 176, 168, 48, 0);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok = ui_button_create(box, i18n_str(STR_BTN_CONFIRM), 288, 176, 168, 48, 2);
    lv_obj_set_user_data(ok, (void *)on_confirm);
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, NULL);
}
