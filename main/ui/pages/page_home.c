#include "page_home.h"

#include <stdio.h>

#include "esp_log.h"
#include "i18n.h"
#include "settings.h"
#include "ui_widgets.h"

/* 主页示例：一个居中的演示开关。
 * 开关状态存 NVS，UI 和 MCP 工具（self.home.set_switch）共用；
 * MCP 从 AI 任务改状态时，UI 用 500 ms 定时器同步。 */

static lv_obj_t *s_demo_switch;
static lv_obj_t *s_demo_state;
static lv_timer_t *s_timer;

void page_home_apply_demo(bool on)
{
    ESP_LOGI("page.home", "apply demo=%d switch=%p", on ? 1 : 0, s_demo_switch);
    if (s_demo_switch == NULL) return;
    if (on) {
        lv_obj_add_state(s_demo_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_demo_switch, LV_STATE_CHECKED);
    }
    if (s_demo_state != NULL) {
        lv_label_set_text(s_demo_state, i18n_str(on ? STR_HOME_DEMO_ON : STR_HOME_DEMO_OFF));
        ui_label_set_text_color(s_demo_state, on ? UI_COLOR_OK : UI_COLOR_MUTED);
    }
}

static void demo_switch_event(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ESP_LOGI("page.home", "demo switch -> %d", on ? 1 : 0);
    settings_set_demo(on);
}

static void demo_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_demo_switch == NULL || !lv_obj_is_visible(s_demo_switch)) {
        return;
    }
    bool on = settings_get()->demo != 0;
    bool shown = lv_obj_has_state(s_demo_switch, LV_STATE_CHECKED);
    if (on != shown) {
        ESP_LOGI("page.home", "timer sync demo=%d", on ? 1 : 0);
        page_home_apply_demo(on);
    }
}

static lv_obj_t *demo_switch_create(lv_obj_t *parent, int x, int y, bool on)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sw, 10);
    lv_obj_add_event_cb(sw, demo_switch_event, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void home_create(lv_obj_t *content)
{
    lv_obj_t *card = ui_card_create(content, 0, 0, 560, 230);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *title = ui_label_create(card, i18n_str(STR_HOME_DEMO_TITLE),
                                      0, 0, UI_COLOR_TEXT, UI_FONT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *chip = ui_chip_create(card, "MCP", UI_COLOR_CYAN, NULL);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -24, 24);

    lv_obj_t *sub = ui_label_create(card, i18n_str(STR_HOME_DEMO_SUB),
                                    0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(sub, 500);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 70);

    /* 开关本身水平居中；名称放下面，避免把开关挤到一侧。 */
    s_demo_switch = demo_switch_create(card, (560 - 64) / 2, 118,
                                       settings_get()->demo != 0);

    bool demo_on = settings_get()->demo != 0;
    char demo_text[64];
    snprintf(demo_text, sizeof(demo_text), "%s · %s", i18n_str(STR_HOME_DEMO_SWITCH),
             i18n_str(demo_on ? STR_HOME_DEMO_ON : STR_HOME_DEMO_OFF));
    s_demo_state = ui_label_create(card, demo_text, 0, 0,
                                   demo_on ? UI_COLOR_OK : UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_align(s_demo_state, LV_ALIGN_TOP_MID, 0, 168);

    s_timer = lv_timer_create(demo_timer_cb, 500, NULL);
}

static void home_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_demo_switch = NULL;
    s_demo_state = NULL;
}

const ui_page_t page_home = {
    .id = "home",
    .name = STR_NAV_HOME,
    .symbol = LV_SYMBOL_HOME,
    .create = home_create,
    .destroy = home_destroy,
};
