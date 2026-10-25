#include "page_settings.h"

#include <stdio.h>
#include <string.h>

#include "hw.h"
#include "i18n.h"
#include "settings.h"
#include "ui_shell.h"
#include "ui_widgets.h"

/* 设置页：左侧分类栏 + 右侧详情卡片。
 *
 * 视觉约定（与 ui_theme.h 配套）：
 *   - 左栏是一张独立面板：品牌块 → 四个分类 → 底部设备状态；
 *   - 右栏顶部是「图标 + 标题 + 副标题 + 装饰序号」的页头，下面是可切换的内容；
 *   - 每一项设置放进比面板亮一级的内层卡片；数值用强调色大号字或胶囊；
 *   - 切分类时内容从右侧轻轻滑入，不做透明度混合，整页移动也很便宜。 */

#define RAIL_W        UI_RAIL_W
#define RAIL_ITEM_X   12
#define RAIL_ITEM_W   (RAIL_W - RAIL_ITEM_X * 2)      /* 248 */
#define RAIL_ITEM_H   UI_RAIL_ITEM_H
#define GAP           (UI_PAGE_W - RAIL_W - UI_DETAIL_W) /* 16 */
#define DETAIL_W      UI_DETAIL_W
#define INNER_W       (DETAIL_W - UI_PANEL_PAD * 2)   /* 640 */

#define CARD_PAD      20
#define CARD_INNER_W  (INNER_W - CARD_PAD * 2)        /* 600 */
#define BODY_Y        110                             /* 页头下方 */
#define BODY_H        (UI_PAGE_H - BODY_Y - UI_PANEL_PAD)  /* 286 */

typedef enum {
    CAT_DISPLAY = 0,
    CAT_SOUND,
    CAT_NETWORK,
    CAT_SYSTEM,
    CAT_COUNT
} category_t;

static const i18n_id_t s_cat_name[CAT_COUNT] = {
    STR_GROUP_DISPLAY, STR_GROUP_SOUND, STR_GROUP_NETWORK, STR_GROUP_SYSTEM,
};
static const i18n_id_t s_cat_sub[CAT_COUNT] = {
    STR_SET_SUB_DISPLAY, STR_SET_SUB_SOUND, STR_SET_SUB_NETWORK, STR_SET_SUB_SYSTEM,
};
static const char *const s_cat_symbol[CAT_COUNT] = {
    LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS,
};
static const uint32_t s_cat_color[CAT_COUNT] = {
    UI_COLOR_ACCENT, UI_COLOR_OK, UI_COLOR_CYAN, UI_COLOR_VIOLET,
};

static lv_obj_t *s_content;
static lv_obj_t *s_detail;
static lv_obj_t *s_body;
static lv_obj_t *s_rail_item[CAT_COUNT];
static lv_obj_t *s_rail_bar[CAT_COUNT];
static lv_obj_t *s_rail_label[CAT_COUNT];
static lv_obj_t *s_rail_chevron[CAT_COUNT];
static category_t s_cat;
static lv_timer_t *s_timer;

/* Wi-Fi 页可变控件 */
static lv_obj_t *s_wifi_name;
static lv_obj_t *s_wifi_state;
static lv_obj_t *s_wifi_ip;
static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_wifi_badge;
static lv_obj_t *s_wifi_disc;
static lv_obj_t *s_wifi_list;
static int s_wifi_last_count = -1;
static bool s_wifi_last_running;

/* 滑条卡片 / 息屏胶囊的共享控件 */
static lv_obj_t *s_slider_value;
static lv_obj_t *s_off_chip_label;

void factory_click_event(lv_event_t *e);
void wifi_connect_dialog(const char *ssid);
static void ap_click(lv_event_t *e);

/* ---------------- 小工具 ---------------- */

/* 卡片标题：左上角 20 px 内边距，和内层控件对齐 */
static lv_obj_t *card_title(lv_obj_t *card, i18n_id_t id)
{
    return ui_label_create(card, i18n_str(id), CARD_PAD, 14, UI_COLOR_TEXT, UI_FONT_BODY);
}

/* 说明文字：控件下面一行浅灰小字 */
static void card_hint(lv_obj_t *card, int y, i18n_id_t id)
{
    lv_obj_t *hint = ui_label_create(card, i18n_str(id), CARD_PAD, y,
                                     UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_set_width(hint, CARD_INNER_W);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* ---------------- 左侧分类栏 ---------------- */

static void rail_apply_state(void)
{
    for (int i = 0; i < CAT_COUNT; i++) {
        bool on = (i == (int)s_cat);
        lv_obj_t *item = s_rail_item[i];

        lv_obj_set_style_bg_color(item, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(item, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(item, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(item, on ? UI_OPA_SOFT : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, on ? 1 : 0, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(item, 50, 0);

        lv_obj_set_style_bg_opa(s_rail_bar[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        ui_label_set_text_color(s_rail_label[i], on ? UI_COLOR_TEXT : UI_COLOR_MUTED);

        if (on) {
            lv_obj_add_flag(s_rail_chevron[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_rail_chevron[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void build_detail(void);

static void rail_clicked(lv_event_t *e)
{
    category_t cat = (category_t)(intptr_t)lv_event_get_user_data(e);
    if (cat == s_cat) {
        return;
    }
    s_cat = cat;
    rail_apply_state();
    build_detail();
}

static void rail_build(lv_obj_t *parent)
{
    lv_obj_t *rail = ui_panel_create(parent, 0, 0, RAIL_W, UI_PAGE_H);

    /* 顶部品牌块：渐变标 + 设置中心 + 产品版本 */
    lv_obj_t *mark = ui_gradient_tile_create(rail, 20, 20, 44, 44, 14,
                                             UI_COLOR_ACCENT_DEEP, UI_COLOR_VIOLET);
    lv_obj_t *mark_icon = ui_label_create(mark, LV_SYMBOL_SETTINGS, 0, 0,
                                          UI_COLOR_TEXT, UI_FONT_ICON);
    lv_obj_center(mark_icon);

    ui_label_create(rail, i18n_str(STR_SET_CENTER), 76, 22, UI_COLOR_TEXT, UI_FONT_BODY);

    lv_obj_t *sub_label = ui_label_create(rail, "v1.1.0", 76, 50,
                                          UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_set_width(sub_label, RAIL_W - 76 - 20);
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_DOT);

    ui_divider_create(rail, 20, 84, RAIL_W - 40);

    for (int i = 0; i < CAT_COUNT; i++) {
        int y = 98 + i * (RAIL_ITEM_H + 6);

        lv_obj_t *item = lv_obj_create(rail);
        lv_obj_set_pos(item, RAIL_ITEM_X, y);
        lv_obj_set_size(item, RAIL_ITEM_W, RAIL_ITEM_H);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(item, UI_RADIUS, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item, UI_OPA_TINT, LV_STATE_PRESSED);
        lv_obj_add_event_cb(item, rail_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* 选中时左侧亮起的竖向短条 */
        lv_obj_t *bar = lv_obj_create(item);
        lv_obj_set_pos(bar, 8, (RAIL_ITEM_H - 24) / 2);
        lv_obj_set_size(bar, 3, 24);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        ui_badge_create(item, 20, (RAIL_ITEM_H - 32) / 2, 32, s_cat_symbol[i],
                        s_cat_color[i]);

        lv_obj_t *label = ui_label_create(item, i18n_str(s_cat_name[i]), 62,
                                          (RAIL_ITEM_H - 20) / 2,
                                          UI_COLOR_MUTED, UI_FONT_BODY);

        lv_obj_t *chevron = ui_label_create(item, LV_SYMBOL_RIGHT, 0, 0,
                                            UI_COLOR_DISABLED, UI_FONT_ICON_SMALL);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -16, 0);

        s_rail_item[i] = item;
        s_rail_bar[i] = bar;
        s_rail_label[i] = label;
        s_rail_chevron[i] = chevron;
    }
}

/* ---------------- 右侧页头 ---------------- */

static void detail_header_build(void)
{
    ui_badge_create(s_detail, UI_PANEL_PAD, 24, 48, s_cat_symbol[s_cat],
                    s_cat_color[s_cat]);

    ui_label_create(s_detail, i18n_str(s_cat_name[s_cat]), 88, 24,
                    UI_COLOR_TEXT, UI_FONT_TITLE);
    ui_label_create(s_detail, i18n_str(s_cat_sub[s_cat]), 88, 62,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);

    char index[16];
    snprintf(index, sizeof(index), "0%d", (int)s_cat + 1);
    lv_obj_t *idx = ui_label_create(s_detail, index, 0, 0, UI_COLOR_DECOR, UI_FONT_TITLE);
    lv_obj_align(idx, LV_ALIGN_TOP_RIGHT, -UI_PANEL_PAD, 26);

    ui_divider_create(s_detail, UI_PANEL_PAD, 96, INNER_W);
}

/* ---------------- 显示 / 声音 ---------------- */

static void brightness_event(lv_event_t *e)
{
    int value = lv_slider_get_value(lv_event_get_target(e));
    settings_preview_brightness(value);
    if (s_slider_value != NULL) {
        lv_label_set_text_fmt(s_slider_value, "%d%%", value);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        settings_set_brightness(value);
    }
}

static void volume_event(lv_event_t *e)
{
    int value = lv_slider_get_value(lv_event_get_target(e));
    settings_preview_volume(value);
    if (s_slider_value != NULL) {
        lv_label_set_text_fmt(s_slider_value, "%d%%", value);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        settings_set_volume(value);
    }
}

static const uint16_t s_screen_off_values[] = { 0, 30, 60, 300 };
static const i18n_id_t s_screen_off_labels[] = {
    STR_OFF_NEVER, STR_OFF_30S, STR_OFF_1M, STR_OFF_5M,
};

static int screen_off_to_index(uint16_t seconds)
{
    for (int i = 0; i < 4; i++) {
        if (s_screen_off_values[i] == seconds) {
            return i;
        }
    }
    return 3;
}

static void screen_off_event(lv_event_t *e)
{
    int index = ui_segmented_get_selected(lv_event_get_target(e));
    if (index >= 0 && index < 4) {
        settings_set_screen_off(s_screen_off_values[index]);
        if (s_off_chip_label != NULL) {
            lv_label_set_text(s_off_chip_label, i18n_str(s_screen_off_labels[index]));
        }
    }
}

static void rebuild_async(void *arg)
{
    (void)arg;
    ui_shell_rebuild();
}

static void language_event(lv_event_t *e)
{
    int index = ui_segmented_get_selected(lv_event_get_target(e));
    if (index == (int)settings_get()->lang) {
        return;
    }
    settings_set_lang(index);
    lv_async_call(rebuild_async, NULL);
}

/* 音量 / 亮度共用的滑条卡片：小标题 + 大号数值 + 滑条 */
static void slider_card(lv_obj_t *body, int y, i18n_id_t title,
                        int min, int max, int value, uint32_t value_color,
                        lv_event_cb_t cb)
{
    lv_obj_t *card = ui_card_create(body, 0, y, INNER_W, 92);
    card_title(card, title);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", value);
    s_slider_value = ui_label_create(card, buf, 0, 0, value_color, UI_FONT_TITLE);
    lv_obj_align(s_slider_value, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 8);

    lv_obj_t *slider = ui_slider_create(card, CARD_PAD, 56, CARD_INNER_W,
                                        min, max, value);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, NULL);
}

static void build_display(lv_obj_t *body)
{
    const sys_settings_t *cfg = settings_get();

    slider_card(body, 0, STR_SET_BRIGHTNESS,
                10, 100, cfg->brightness, UI_COLOR_ACCENT, brightness_event);

    lv_obj_t *card = ui_card_create(body, 0, 108, INNER_W, 148);
    card_title(card, STR_SET_SCREEN_OFF);

    int selected = screen_off_to_index(cfg->screen_off_s);
    lv_obj_t *chip = ui_chip_create(card, i18n_str(s_screen_off_labels[selected]),
                                    UI_COLOR_ACCENT, &s_off_chip_label);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 11);

    const char *labels[4] = {
        i18n_str(STR_OFF_NEVER), i18n_str(STR_OFF_30S),
        i18n_str(STR_OFF_1M), i18n_str(STR_OFF_5M),
    };
    lv_obj_t *seg = ui_segmented_create(card, CARD_PAD, 54, CARD_INNER_W, 48,
                                        labels, 4, selected);
    lv_obj_add_event_cb(seg, screen_off_event, LV_EVENT_VALUE_CHANGED, NULL);

    card_hint(card, 114, STR_HINT_SCREEN_OFF);
}

static void build_sound(lv_obj_t *body)
{
    const sys_settings_t *cfg = settings_get();

    slider_card(body, 0, STR_SET_VOLUME,
                0, 100, cfg->volume, UI_COLOR_OK, volume_event);
}

/* ---------- 网络 ---------- */

static const char *wifi_summary(void)
{
    if (!g_hw[HW_WIFI].enabled) {
        return i18n_str(STR_ST_OFF);
    }
    if (hw_wifi_is_connected()) {
        return hw_wifi_sta_ssid();
    }
    return i18n_str(STR_WIFI_DISCONNECTED);
}

static void wifi_refresh_list(void)
{
    if (s_wifi_list == NULL) {
        return;
    }
    lv_obj_clean(s_wifi_list);

    int count = hw_wifi_scan_count();
    if (count <= 0) {
        ui_label_create(s_wifi_list, i18n_str(STR_WIFI_NO_AP), 12, 16,
                        UI_COLOR_MUTED, UI_FONT_SMALL);
        return;
    }

    bool connected = hw_wifi_is_connected();

    for (int i = 0; i < count; i++) {
        const hw_wifi_ap_t *ap = hw_wifi_scan_ap(i);
        if (ap == NULL) {
            continue;
        }
        int y = i * (44 + 7);
        lv_obj_t *row = ui_list_row_create(s_wifi_list, 0, y, INNER_W - 8, 44);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ap_click, LV_EVENT_CLICKED, NULL);

        bool is_cur = connected && strcmp(ap->ssid, hw_wifi_sta_ssid()) == 0;
        ui_badge_create(row, 7, 7, 30, LV_SYMBOL_WIFI,
                        is_cur ? UI_COLOR_OK : UI_COLOR_CYAN);

        lv_obj_t *ssid = ui_label_create(row, ap->ssid, 0, 0,
                                         is_cur ? UI_COLOR_TEXT : UI_COLOR_MUTED,
                                         UI_FONT_BODY);
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 50, 0);
        lv_obj_set_width(ssid, 400);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

        char rssi[24];
        snprintf(rssi, sizeof(rssi), "%d dBm", ap->rssi);
        lv_obj_t *r = ui_label_create(row, rssi, 0, 0,
                                      is_cur ? UI_COLOR_OK : UI_COLOR_DISABLED,
                                      UI_FONT_SMALL);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -14, 0);

        /* 记录 ssid 以便点击时连这个 AP */
        lv_obj_set_user_data(row, (void *)ap->ssid);
    }
}

static void ap_click(lv_event_t *e)
{
    const char *ssid = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
    wifi_connect_dialog(ssid);
}

static void wifi_scan_click(lv_event_t *e)
{
    (void)e;
    hw_wifi_scan_start();
    if (s_wifi_name != NULL) {
        lv_label_set_text(s_wifi_name, i18n_str(STR_WIFI_SCANNING));
    }
}

static void wifi_disconnect_click(lv_event_t *e)
{
    (void)e;
    hw_wifi_disconnect();
    if (s_wifi_name != NULL) {
        lv_label_set_text(s_wifi_name, i18n_str(STR_WIFI_DISCONNECTED));
    }
}

static void wifi_apply_status(void)
{
    bool enabled = g_hw[HW_WIFI].enabled;
    bool connected = hw_wifi_is_connected();
    bool scanning = hw_wifi_scan_running();
    uint32_t state_color = !enabled ? UI_COLOR_DISABLED
                                    : (connected ? UI_COLOR_OK : UI_COLOR_MUTED);

    if (s_wifi_name != NULL) {
        lv_label_set_text(s_wifi_name,
                          scanning ? i18n_str(STR_WIFI_SCANNING) : wifi_summary());
        ui_label_set_text_color(s_wifi_name, connected ? UI_COLOR_TEXT : UI_COLOR_MUTED);
    }
    if (s_wifi_state != NULL) {
        lv_label_set_text(s_wifi_state,
                          i18n_str(connected ? STR_WIFI_CONNECTED : STR_WIFI_DISCONNECTED));
        ui_label_set_text_color(s_wifi_state, state_color);
    }
    if (s_wifi_ip != NULL) {
        const char *ip = connected ? hw_wifi_ip() : NULL;
        lv_label_set_text(s_wifi_ip, (ip != NULL && ip[0] != '\0') ? ip : "—");
    }
    if (s_wifi_dot != NULL) {
        ui_dot_set_color(s_wifi_dot, state_color);
    }
    if (s_wifi_badge != NULL) {
        ui_badge_set_color(s_wifi_badge, connected ? UI_COLOR_CYAN : UI_COLOR_DISABLED);
    }
    if (s_wifi_disc != NULL) {
        if (connected) {
            lv_obj_remove_flag(s_wifi_disc, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_disc, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void wifi_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_cat != CAT_NETWORK || s_body == NULL || !lv_obj_is_visible(s_body)) {
        return;
    }

    bool running = hw_wifi_scan_running();
    int count = hw_wifi_scan_count();

    wifi_apply_status();

    if (running != s_wifi_last_running || count != s_wifi_last_count) {
        s_wifi_last_running = running;
        s_wifi_last_count = count;
        if (!running) {
            wifi_refresh_list();
        }
    }
}

static void build_network(lv_obj_t *body)
{
    bool connected = hw_wifi_is_connected();

    /* 状态卡：图标 + 网络名 + 状态/地址 + 右侧操作按钮 */
    lv_obj_t *card = ui_card_create(body, 0, 0, INNER_W, 108);
    s_wifi_badge = ui_badge_create(card, CARD_PAD, 18, 40, LV_SYMBOL_WIFI,
                                   connected ? UI_COLOR_CYAN : UI_COLOR_DISABLED);

    ui_label_create(card, "Wi-Fi", 72, 14, UI_COLOR_TEXT, UI_FONT_BODY);

    s_wifi_name = ui_label_create(card, wifi_summary(), 72, 42,
                                  connected ? UI_COLOR_TEXT : UI_COLOR_MUTED,
                                  UI_FONT_BODY);
    lv_obj_set_width(s_wifi_name, 260);
    lv_label_set_long_mode(s_wifi_name, LV_LABEL_LONG_DOT);

    s_wifi_dot = ui_dot_create(card, 74, 80, 8);
    s_wifi_state = ui_label_create(card, "", 88, 72, UI_COLOR_MUTED, UI_FONT_SMALL);
    s_wifi_ip = ui_label_create(card, "", 190, 72, UI_COLOR_DISABLED, UI_FONT_SMALL);

    /* 两个按钮常驻，连接状态变化时只切换「断开」的可见性，避免整页重建 */
    s_wifi_disc = ui_button_create(card, i18n_str(STR_WIFI_DISCONNECT),
                                   348, 18, 128, 36, 0);
    lv_obj_add_event_cb(s_wifi_disc, wifi_disconnect_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *scan = ui_button_create(card, i18n_str(STR_WIFI_SCAN),
                                      484, 18, 136, 36, 1);
    lv_obj_add_event_cb(scan, wifi_scan_click, LV_EVENT_CLICKED, NULL);

    /* 扫描结果列表：固定高度，超出部分滚动 */
    ui_label_create(body, i18n_str(STR_WIFI_AVAILABLE), 0, 118, UI_COLOR_MUTED,
                    UI_FONT_SMALL);

    s_wifi_list = lv_obj_create(body);
    lv_obj_set_pos(s_wifi_list, 0, 140);
    lv_obj_set_size(s_wifi_list, INNER_W, BODY_H - 140);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 0, 0);
    lv_obj_set_style_pad_right(s_wifi_list, 8, 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_wifi_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_wifi_list, UI_OPA_TINT, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_wifi_list, 2, LV_PART_SCROLLBAR);

    wifi_apply_status();
    wifi_refresh_list();
    s_wifi_last_count = -1;
    s_wifi_last_running = hw_wifi_scan_running();

    if (s_timer == NULL) {
        s_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
    }
}

/* ---------- 系统 ---------- */

static void build_system(lv_obj_t *body)
{
    lv_obj_t *card = ui_card_create(body, 0, 0, INNER_W, 112);
    card_title(card, STR_SET_LANGUAGE);

    const char *langs[2] = { i18n_str(STR_LANG_ZH), i18n_str(STR_LANG_EN) };
    lv_obj_t *seg = ui_segmented_create(card, CARD_PAD, 50, CARD_INNER_W, 46,
                                        langs, 2, settings_get()->lang);
    lv_obj_add_event_cb(seg, language_event, LV_EVENT_VALUE_CHANGED, NULL);

    /* 版本信息：两行浅色信息，不抢主设置的视觉 */
    lv_obj_t *about = ui_card_create(body, 0, 122, INNER_W, 66);
    ui_badge_create(about, 18, 17, 32, LV_SYMBOL_LIST, UI_COLOR_VIOLET);

    char fw[80];
    snprintf(fw, sizeof(fw), "%s   v1.1.0", i18n_str(STR_SYS_FIRMWARE));
    ui_label_create(about, fw, 62, 10, UI_COLOR_TEXT, UI_FONT_SMALL);

    char board[96];
    snprintf(board, sizeof(board), "%s   JC1060P470C_I_W_Y", i18n_str(STR_SYS_BOARD));
    ui_label_create(about, board, 62, 36, UI_COLOR_DISABLED, UI_FONT_SMALL);

    /* 危险操作：红色浅底 + 明确的确认弹层 */
    lv_obj_t *danger = ui_card_create(body, 0, 198, INNER_W, 84);
    lv_obj_set_style_bg_color(danger, lv_color_hex(UI_COLOR_ERROR), 0);
    lv_obj_set_style_bg_grad_dir(danger, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(danger, 18, 0);
    lv_obj_set_style_border_color(danger, lv_color_hex(UI_COLOR_ERROR), 0);
    lv_obj_set_style_border_opa(danger, 60, 0);

    ui_badge_create(danger, 18, 22, 40, LV_SYMBOL_WARNING, UI_COLOR_ERROR);
    ui_label_create(danger, i18n_str(STR_SET_FACTORY), 70, 14, UI_COLOR_ERROR,
                    UI_FONT_BODY);
    lv_obj_t *hint = ui_label_create(danger, i18n_str(STR_HINT_FACTORY), 70, 46,
                                     UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_set_width(hint, 400);

    lv_obj_t *factory = ui_button_create(danger, i18n_str(STR_SET_FACTORY),
                                         INNER_W - 180, 21, 160, 42, 2);
    lv_obj_add_event_cb(factory, factory_click_event, LV_EVENT_CLICKED, NULL);
}

/* ---------------- 详情容器 ---------------- */

static void build_detail(void)
{
    if (s_detail == NULL) {
        return;
    }
    lv_obj_clean(s_detail);
    s_body = NULL;
    s_wifi_name = NULL;
    s_wifi_state = NULL;
    s_wifi_ip = NULL;
    s_wifi_dot = NULL;
    s_wifi_badge = NULL;
    s_wifi_disc = NULL;
    s_wifi_list = NULL;
    s_slider_value = NULL;
    s_off_chip_label = NULL;

    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    detail_header_build();

    s_body = lv_obj_create(s_detail);
    lv_obj_set_pos(s_body, UI_PANEL_PAD, BODY_Y);
    lv_obj_set_size(s_body, INNER_W, BODY_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    switch (s_cat) {
    case CAT_DISPLAY: build_display(s_body); break;
    case CAT_SOUND:   build_sound(s_body); break;
    case CAT_NETWORK: build_network(s_body); break;
    case CAT_SYSTEM:  build_system(s_body); break;
    default: break;
    }

    ui_animate_rise_in(s_body);
}

/* ---------------- 恢复出厂 / 密码框 ---------------- */

static void factory_confirm_cb(lv_event_t *e)
{
    (void)e;
    settings_factory_reset();
}

void factory_click_event(lv_event_t *e)
{
    (void)e;
    ui_confirm_create(lv_screen_active(), i18n_str(STR_FACTORY_CONFIRM), factory_confirm_cb);
}

static lv_obj_t *s_pass_scrim;
static lv_obj_t *s_pass_textarea;
static char s_pass_ssid[33];

static void pass_close(void)
{
    if (s_pass_scrim != NULL) {
        lv_obj_delete(s_pass_scrim);
        s_pass_scrim = NULL;
        s_pass_textarea = NULL;
    }
}

static void pass_cancel_cb(lv_event_t *e)
{
    (void)e;
    pass_close();
}

static void pass_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *pass = (s_pass_textarea != NULL) ? lv_textarea_get_text(s_pass_textarea) : "";
    hw_wifi_connect(s_pass_ssid, pass);
    pass_close();
}

static void pass_keyboard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        pass_connect_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        pass_close();
    }
}

void wifi_connect_dialog(const char *ssid)
{
    if (ssid == NULL) {
        return;
    }
    snprintf(s_pass_ssid, sizeof(s_pass_ssid), "%s", ssid);

    lv_obj_t *scrim = lv_obj_create(lv_screen_active());
    lv_obj_set_size(scrim, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    s_pass_scrim = scrim;

    lv_obj_t *box = ui_card_create(scrim, 0, 0, 760, 420);
    lv_obj_center(box);

    ui_badge_create(box, 20, 22, 36, LV_SYMBOL_WIFI, UI_COLOR_CYAN);

    char title[96];
    snprintf(title, sizeof(title), "%s: %s", i18n_str(STR_WIFI_PASSWORD), ssid);
    lv_obj_t *label = ui_label_create(box, title, 64, 30, UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_set_width(label, 660);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_obj_set_size(ta, 712, 56);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 72);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, i18n_str(STR_WIFI_PASSWORD));
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_pad_ver(ta, 8, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(ta, UI_FONT_BODY, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    s_pass_textarea = ta;

    lv_obj_t *cancel = ui_button_create(box, i18n_str(STR_BTN_CANCEL), 24, 144, 160, 44, 0);
    lv_obj_add_event_cb(cancel, pass_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok = ui_button_create(box, i18n_str(STR_WIFI_CONNECT), 194, 144, 160, 44, 1);
    lv_obj_add_event_cb(ok, pass_connect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(box);
    lv_obj_set_size(kb, 712, 200);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 8, 0);
    /* 键盘上有退格/回车等 LV_SYMBOL_* 键，必须用 Montserrat；
     * 中文字库是子集，没有这些符号，否则会显示成方框。 */
    lv_obj_set_style_text_font(kb, UI_FONT_ICON, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_TILE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, UI_RADIUS_S, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_ON_ACCENT),
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(kb, pass_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, pass_keyboard_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------------- 页面入口 ---------------- */

static void settings_create(lv_obj_t *content)
{
    s_content = content;
    s_cat = CAT_DISPLAY;
    s_detail = NULL;
    s_body = NULL;
    s_wifi_list = NULL;
    s_wifi_name = NULL;
    s_wifi_state = NULL;
    s_wifi_ip = NULL;
    s_wifi_dot = NULL;
    s_wifi_badge = NULL;
    s_wifi_disc = NULL;

    rail_build(content);

    s_detail = ui_panel_create(content, RAIL_W + GAP, 0, DETAIL_W, UI_PAGE_H);
    rail_apply_state();
    build_detail();
}

static void settings_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    pass_close();
    s_content = NULL;
    s_detail = NULL;
    s_body = NULL;
    s_wifi_list = NULL;
    s_wifi_name = NULL;
    s_wifi_state = NULL;
    s_wifi_ip = NULL;
    s_wifi_dot = NULL;
    s_wifi_badge = NULL;
    s_wifi_disc = NULL;
}

const ui_page_t page_settings = {
    .id = "settings",
    .name = STR_NAV_SETTINGS,
    .symbol = LV_SYMBOL_SETTINGS,
    .create = settings_create,
    .destroy = settings_destroy,
};
