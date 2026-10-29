#include "ai_ui.h"

#include <cstring>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ai_audio.h"
#include "ai_assets.h"
#include "cbin_font.h"
#include "i18n.h"
#include "page_home.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ai.ui";

#define ROBOT_X 936
#define ROBOT_Y 452
#define ROBOT_SIZE 64
#define CHAT_W 460
#define CHAT_H 370
#define CHAT_X (UI_SCREEN_W - 24 - CHAT_W)
#define CHAT_Y (UI_HEADER_H + 8)
#define MINI_W 320
#define MINI_H 54
#define MINI_X (ROBOT_X + ROBOT_SIZE - MINI_W)
#define MINI_Y (ROBOT_Y - MINI_H - 12)
#define MINI_HIDE_MS 8000
#define LOG_MAX 30
#define LOG_TEXT_MAX 2048

#define ICON_ROBOT       "\xF3\xB0\x9A\xA9"
#define ICON_ROBOT_HAPPY "\xF3\xB1\x9C\x99"
#define ICON_ROBOT_EXCITED "\xF3\xB1\x9A\xA3"

enum UiEventType {
    UI_EVT_STATE = 1,
    UI_EVT_MESSAGE,
    UI_EVT_ACTIVATION,
    UI_EVT_ERROR,
    UI_EVT_EMOTION,
    UI_EVT_DEMO,
    UI_EVT_REPLY_DONE,
};

struct UiEvent {
    uint8_t type;
    uint8_t role;
    char text[1024];
    char extra[64];
};

struct LogMsg {
    uint8_t role;
    char text[LOG_TEXT_MAX];
};

static QueueHandle_t s_queue;
static lv_timer_t *s_timer;
static LogMsg *s_log;
static int s_log_head;
static int s_log_count;

static lv_font_t *s_chat_font;
static lv_obj_t *s_root;
static lv_obj_t *s_robot;
static lv_obj_t *s_robot_ring;
static lv_obj_t *s_visor;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_mouth;
static lv_obj_t *s_robot_dot;
static lv_obj_t *s_unread;

static lv_obj_t *s_outside;
static lv_obj_t *s_popover;
static lv_obj_t *s_card;
static lv_obj_t *s_msg_list;
static lv_obj_t *s_status_chip;
static lv_obj_t *s_status_chip_label;
static lv_obj_t *s_footer_status;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_mute_btn;
static lv_obj_t *s_mute_icon;
static lv_obj_t *s_activation_box;
static lv_obj_t *s_activation_code;
static lv_obj_t *s_activation_hint;

static lv_obj_t *s_mini;
static lv_obj_t *s_mini_label;
static lv_obj_t *s_mini_dot;
static lv_timer_t *s_mini_timer;

static bool s_chat_open;
static bool s_ring_running;
static bool s_auto_close;
static bool s_user_dismissed;
static lv_timer_t *s_auto_close_timer;
static char s_error[192];
static char s_activation[16];
static char s_activation_message[256];

static uint32_t state_color(ai_state_t state)
{
    switch (state) {
    case AI_STATE_ACTIVATING: return UI_COLOR_WARN;
    case AI_STATE_LISTENING:  return UI_COLOR_CYAN;
    case AI_STATE_SPEAKING:   return UI_COLOR_VIOLET;
    case AI_STATE_CONNECTING: return UI_COLOR_ACCENT_SOFT;
    case AI_STATE_ERROR:      return UI_COLOR_ERROR;
    case AI_STATE_DISABLED:   return UI_COLOR_DISABLED;
    default:                  return UI_COLOR_ACCENT;
    }
}

static const char *state_text(ai_state_t state)
{
    switch (state) {
    case AI_STATE_STARTING:   return i18n_str(STR_AI_ST_STARTING);
    case AI_STATE_ACTIVATING: return i18n_str(STR_AI_ST_ACTIVATING);
    case AI_STATE_IDLE:       return i18n_str(STR_AI_ST_IDLE);
    case AI_STATE_CONNECTING: return i18n_str(STR_AI_ST_CONNECTING);
    case AI_STATE_LISTENING:  return i18n_str(STR_AI_ST_LISTENING);
    case AI_STATE_SPEAKING:   return i18n_str(STR_AI_ST_SPEAKING);
    case AI_STATE_ERROR:      return i18n_str(STR_AI_ST_ERROR);
    default:                  return i18n_str(STR_AI_ST_DISABLED);
    }
}



static void log_push(ai_role_t role, const char *text)
{
    if (s_log == NULL || text == NULL) return;
    int index;
    if (s_log_count < LOG_MAX) {
        index = s_log_head;
        s_log_head = (s_log_head + 1) % LOG_MAX;
        s_log_count++;
    } else {
        index = s_log_head;
        s_log_head = (s_log_head + 1) % LOG_MAX;
    }
    s_log[index].role = (uint8_t)role;
    strlcpy(s_log[index].text, text, LOG_TEXT_MAX);
}

static void unread_update(void)
{
    if (s_unread == NULL) return;
    if (s_chat_open || s_log_count == 0) {
        lv_obj_add_flag(s_unread, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_unread, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ring_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void ring_set_busy(bool busy)
{
    if (s_robot_ring == NULL) return;
    if (busy == s_ring_running) return;
    s_ring_running = busy;
    if (busy) {
        lv_obj_remove_flag(s_robot_ring, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_robot_ring);
        lv_anim_set_exec_cb(&anim, ring_opa_cb);
        lv_anim_set_values(&anim, 24, 110);
        lv_anim_set_duration(&anim, 900);
        lv_anim_set_playback_duration(&anim, 900);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim);
    } else {
        lv_anim_delete(s_robot_ring, ring_opa_cb);
        lv_obj_add_flag(s_robot_ring, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_state(ai_state_t state)
{
    uint32_t color = state_color(state);
    if (s_robot_dot) ui_dot_set_color(s_robot_dot, color);

    uint32_t eye_color = UI_COLOR_ACCENT_SOFT;
    if (state == AI_STATE_LISTENING) eye_color = UI_COLOR_CYAN;
    else if (state == AI_STATE_SPEAKING) eye_color = UI_COLOR_VIOLET;
    else if (state == AI_STATE_ERROR) eye_color = UI_COLOR_ERROR;
    else if (state == AI_STATE_DISABLED) eye_color = UI_COLOR_DISABLED;
    if (s_eye_l) lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(eye_color), 0);
    if (s_eye_r) lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(eye_color), 0);
    if (s_mouth) {
        lv_obj_set_style_bg_color(s_mouth, lv_color_hex(eye_color), 0);
        if (state == AI_STATE_SPEAKING) {
            lv_obj_set_size(s_mouth, 8, 6);
            lv_obj_set_style_radius(s_mouth, 3, 0);
        } else {
            lv_obj_set_size(s_mouth, 10, 2);
            lv_obj_set_style_radius(s_mouth, 1, 0);
        }
    }
    ring_set_busy(ai_is_busy());

    if (s_status_chip_label) {
        lv_label_set_text(s_status_chip_label, state_text(state));
        lv_obj_set_style_text_color(s_status_chip_label, lv_color_hex(color), 0);
    }
    if (s_status_chip) {
        lv_obj_set_style_bg_color(s_status_chip, lv_color_hex(color), 0);
        lv_obj_set_style_border_color(s_status_chip, lv_color_hex(color), 0);
    }

    if (s_footer_status) {
        const char *text;
        if (state == AI_STATE_ERROR && s_error[0]) {
            text = s_error;
        } else if (state == AI_STATE_IDLE) {
            text = i18n_str(STR_AI_WAKE_HINT);
        } else if (state == AI_STATE_LISTENING) {
            text = i18n_str(STR_AI_ST_LISTENING);
        } else if (state == AI_STATE_SPEAKING) {
            text = i18n_str(STR_AI_CHAT_HINT);
        } else {
            text = state_text(state);
        }
        lv_label_set_text(s_footer_status, text);
        lv_obj_set_style_text_color(s_footer_status, lv_color_hex(color), 0);
    }

    if (s_start_btn) {
        lv_obj_t *label = lv_obj_get_child(s_start_btn, 0);
        if (label != NULL) {
            const char *text = i18n_str(STR_AI_START);
            if (state == AI_STATE_LISTENING) {
                text = i18n_str(STR_AI_STOP);
            } else if (state == AI_STATE_SPEAKING) {
                text = i18n_str(STR_AI_STOP);
            } else if (state == AI_STATE_CONNECTING || state == AI_STATE_ACTIVATING) {
                text = state_text(state);
            } else if (state == AI_STATE_ERROR) {
                text = i18n_str(STR_AI_RETRY);
            }
            lv_label_set_text(label, text);
        }
    }
    if (s_mute_icon) {
        lv_label_set_text(s_mute_icon, ai_is_muted() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    }

    if (state == AI_STATE_IDLE && ai_get_activation_code()[0] == '\0') {
        s_activation[0] = '\0';
        s_activation_message[0] = '\0';
    }
    if (s_activation_box && s_activation[0] == '\0') {
        lv_obj_add_flag(s_activation_box, LV_OBJ_FLAG_HIDDEN);
        if (s_msg_list) lv_obj_remove_flag(s_msg_list, LV_OBJ_FLAG_HIDDEN);
    }
}

static void message_bubble(lv_obj_t *list, uint8_t role, const char *text, bool scroll)
{
    if (list == NULL || text == NULL) return;

    bool near_bottom = lv_obj_get_scroll_bottom(list) < 40;

    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_flex_align_t align = role == AI_ROLE_USER ? LV_FLEX_ALIGN_END :
                            (role == AI_ROLE_SYSTEM ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START);
    lv_obj_set_flex_align(row, align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_left(bubble, 12, 0);
    lv_obj_set_style_pad_right(bubble, 12, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (role == AI_ROLE_USER) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(bubble, 48, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(bubble, 70, 0);
    } else if (role == AI_ROLE_SYSTEM) {
        lv_obj_set_style_bg_opa(bubble, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_COLOR_SURFACE_HI), 0);
        lv_obj_set_style_bg_opa(bubble, 220, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
        lv_obj_set_style_border_opa(bubble, 90, 0);
    }

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, role == AI_ROLE_SYSTEM ? 380 : 320);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(role == AI_ROLE_SYSTEM ? UI_COLOR_MUTED : UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(label, s_chat_font != NULL ? s_chat_font : UI_FONT_BODY, 0);
    lv_obj_set_style_text_align(label, role == AI_ROLE_SYSTEM ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);

    while (lv_obj_get_child_count(list) > LOG_MAX) {
        lv_obj_delete(lv_obj_get_child(list, 0));
    }

    if (scroll && near_bottom) {
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
}

static void rebuild_messages(void)
{
    if (s_msg_list == NULL) return;
    lv_obj_clean(s_msg_list);
    int start = (s_log_head - s_log_count + LOG_MAX) % LOG_MAX;
    for (int i = 0; i < s_log_count; i++) {
        int index = (start + i) % LOG_MAX;
        message_bubble(s_msg_list, s_log[index].role, s_log[index].text, false);
    }
    int count = lv_obj_get_child_count(s_msg_list);
    if (count > 0) {
        lv_obj_scroll_to_view(lv_obj_get_child(s_msg_list, count - 1), LV_ANIM_OFF);
    }
}

static void close_chat(bool manual);
static void open_chat(lv_obj_t *root);
static void mini_hide(void);
static void mini_show(ai_role_t role, const char *text);
static void auto_close_cancel(void);
static void auto_close_schedule(void);

static void bubble_opa_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void bubble_y_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

static void open_chat(lv_obj_t *root)
{
    (void)root;
    if (s_chat_open) return;
    mini_hide();
    s_chat_open = true;
    unread_update();

    /* 点击气泡外任意位置关闭：透明点击层在气泡下面、机器人上面。 */
    s_outside = lv_obj_create(s_root);
    lv_obj_set_pos(s_outside, 0, 0);
    lv_obj_set_size(s_outside, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_opa(s_outside, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_outside, 0, 0);
    lv_obj_set_style_pad_all(s_outside, 0, 0);
    lv_obj_remove_flag(s_outside, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_outside, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_outside, [](lv_event_t *e) { (void)e; close_chat(true); },
                        LV_EVENT_CLICKED, NULL);

    /* 悬浮气泡：贴着机器人上方，不铺全屏遮罩。 */
    s_popover = lv_obj_create(s_root);
    lv_obj_set_pos(s_popover, CHAT_X, CHAT_Y);
    lv_obj_set_size(s_popover, CHAT_W, CHAT_H);
    lv_obj_set_style_bg_opa(s_popover, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_popover, 0, 0);
    lv_obj_set_style_pad_all(s_popover, 0, 0);
    lv_obj_remove_flag(s_popover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_popover, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* 指向机器人的小尾巴（旋转 45° 的方块，被卡片盖住上半部分）。 */
    lv_obj_t *tail = lv_obj_create(s_popover);
    lv_obj_set_size(tail, 18, 18);
    lv_obj_set_pos(tail, 419, 361);
    lv_obj_set_style_bg_color(tail, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tail, 0, 0);
    lv_obj_set_style_radius(tail, 3, 0);
    lv_obj_set_style_transform_rotation(tail, 450, 0);
    lv_obj_remove_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tail, LV_OBJ_FLAG_CLICKABLE);

    s_card = ui_card_create(s_popover, 0, 0, CHAT_W, CHAT_H);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(s_card, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 24, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(s_card, 70, 0);
    lv_obj_set_style_shadow_width(s_card, 0, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = ui_label_create(s_card, i18n_str(STR_AI_TITLE), 20, 10,
                                      UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_set_width(title, 200);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    s_status_chip = ui_chip_create(s_card, state_text(ai_get_state()),
                                   state_color(ai_get_state()), &s_status_chip_label);
    lv_obj_align(s_status_chip, LV_ALIGN_TOP_LEFT, 230, 10);
    lv_obj_set_style_text_font(s_status_chip_label, UI_FONT_SMALL, 0);

    lv_obj_t *close = lv_button_create(s_card);
    lv_obj_set_pos(close, 402, 8);
    lv_obj_set_size(close, 40, 40);
    lv_obj_set_style_radius(close, UI_RADIUS_S, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_t *close_icon = ui_label_create(close, LV_SYMBOL_CLOSE, 0, 0,
                                           UI_COLOR_MUTED, UI_FONT_ICON);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *e) { (void)e; close_chat(true); }, LV_EVENT_CLICKED, NULL);

    s_msg_list = lv_obj_create(s_card);
    lv_obj_set_pos(s_msg_list, 16, 56);
    lv_obj_set_size(s_msg_list, 428, 224);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msg_list, 0, 0);
    lv_obj_set_style_pad_all(s_msg_list, 4, 0);
    lv_obj_set_style_pad_row(s_msg_list, 4, 0);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);

    s_activation_box = lv_obj_create(s_card);
    lv_obj_set_pos(s_activation_box, 16, 56);
    lv_obj_set_size(s_activation_box, 428, 224);
    lv_obj_set_style_bg_opa(s_activation_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_activation_box, 0, 0);
    lv_obj_set_style_pad_all(s_activation_box, 0, 0);
    lv_obj_remove_flag(s_activation_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_activation_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *act_title = ui_label_create(s_activation_box, i18n_str(STR_AI_ACTIVATION),
                                          0, 16, UI_COLOR_ACCENT, UI_FONT_BODY);
    lv_obj_align(act_title, LV_ALIGN_TOP_MID, 0, 16);
    s_activation_code = ui_label_create(s_activation_box, s_activation[0] ? s_activation : "------",
                                        0, 0, UI_COLOR_TEXT, UI_FONT_TITLE);
    lv_obj_align(s_activation_code, LV_ALIGN_TOP_MID, 0, 62);
    s_activation_hint = ui_label_create(s_activation_box,
                                        s_activation_message[0] ? s_activation_message : i18n_str(STR_AI_ACTIVATION_HINT),
                                        0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(s_activation_hint, 380);
    lv_label_set_long_mode(s_activation_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_activation_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_activation_hint, LV_ALIGN_TOP_MID, 0, 110);

    s_footer_status = ui_label_create(s_card, i18n_str(STR_AI_WAKE_HINT), 18, 312,
                                      UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(s_footer_status, 250);
    lv_label_set_long_mode(s_footer_status, LV_LABEL_LONG_DOT);

    s_mute_btn = lv_button_create(s_card);
    lv_obj_set_pos(s_mute_btn, 286, 300);
    lv_obj_set_size(s_mute_btn, 44, 44);
    lv_obj_set_style_radius(s_mute_btn, UI_RADIUS_S, 0);
    lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_border_width(s_mute_btn, 0, 0);
    s_mute_icon = ui_label_create(s_mute_btn, ai_is_muted() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX,
                                  0, 0, ai_is_muted() ? UI_COLOR_WARN : UI_COLOR_MUTED, UI_FONT_ICON);
    lv_obj_center(s_mute_icon);
    lv_obj_add_event_cb(s_mute_btn, [](lv_event_t *e) {
        (void)e;
        ai_set_muted(!ai_is_muted());
        apply_state(ai_get_state());
    }, LV_EVENT_CLICKED, NULL);

    s_start_btn = ui_button_create(s_card, i18n_str(STR_AI_START), 338, 300, 104, 44, 1);
    lv_obj_add_event_cb(s_start_btn, [](lv_event_t *e) {
        (void)e;
        ai_state_t state = ai_get_state();
        if (state == AI_STATE_LISTENING || state == AI_STATE_SPEAKING) {
            ai_stop_session();
        } else if (state == AI_STATE_ERROR) {
            ai_retry();
        } else {
            ai_request_listen();
        }
    }, LV_EVENT_CLICKED, NULL);

    rebuild_messages();
    if (s_activation[0] != '\0') {
        lv_obj_add_flag(s_msg_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_activation_box, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_opa(s_popover, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_popover, 12, 0);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_popover);
    lv_anim_set_exec_cb(&anim, bubble_opa_anim_cb);
    lv_anim_set_values(&anim, 0, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 200);
    lv_anim_start(&anim);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_popover);
    lv_anim_set_exec_cb(&anim, bubble_y_anim_cb);
    lv_anim_set_values(&anim, 12, 0);
    lv_anim_set_duration(&anim, 220);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);

    apply_state(ai_get_state());
}

static void close_chat(bool manual)
{
    auto_close_cancel();
    s_auto_close = false;
    if (manual) {
        s_user_dismissed = true;
        ESP_LOGI(TAG, "chat closed by user; new info will use mini bubble");
    }
    mini_hide();
    if (s_popover != NULL) {
        lv_anim_delete(s_popover, bubble_opa_anim_cb);
        lv_anim_delete(s_popover, bubble_y_anim_cb);
        lv_obj_delete(s_popover);
    }
    if (s_outside != NULL) {
        lv_obj_delete(s_outside);
    }
    s_outside = NULL;
    s_popover = NULL;
    s_card = NULL;
    s_msg_list = NULL;
    s_status_chip = NULL;
    s_status_chip_label = NULL;
    s_footer_status = NULL;
    s_start_btn = NULL;
    s_mute_btn = NULL;
    s_mute_icon = NULL;
    s_activation_box = NULL;
    s_activation_code = NULL;
    s_activation_hint = NULL;
    s_chat_open = false;
    unread_update();
}

static uint32_t mini_role_color(ai_role_t role)
{
    if (role == AI_ROLE_USER) return UI_COLOR_ACCENT_SOFT;
    if (role == AI_ROLE_SYSTEM) return UI_COLOR_MUTED;
    return UI_COLOR_VIOLET;
}

static lv_obj_t *mini_create(lv_obj_t *root)
{
    /* 外层只负责定位和点击；小尾巴在下、卡片在上，卡片盖住尾巴的上半段。 */
    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_pos(box, MINI_X, MINI_Y);
    lv_obj_set_size(box, MINI_W, MINI_H);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *tail = lv_obj_create(box);
    lv_obj_set_size(tail, 16, 16);
    lv_obj_set_pos(tail, MINI_W - 46, MINI_H - 9);
    lv_obj_set_style_bg_color(tail, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tail, 0, 0);
    lv_obj_set_style_radius(tail, 3, 0);
    lv_obj_set_style_transform_rotation(tail, 450, 0);
    lv_obj_remove_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tail, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(box);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_size(card, MINI_W, MINI_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, 80, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

    s_mini_dot = ui_dot_create(card, 14, (MINI_H - 8) / 2, 8);
    ui_dot_set_color(s_mini_dot, UI_COLOR_VIOLET);

    const lv_font_t *mini_font = s_chat_font != NULL ? s_chat_font : UI_FONT_BODY;
    s_mini_label = lv_label_create(card);
    lv_obj_set_width(s_mini_label, MINI_W - 44);
    lv_obj_set_height(s_mini_label, lv_font_get_line_height(mini_font));
    lv_obj_align(s_mini_label, LV_ALIGN_LEFT_MID, 30, 0);
    lv_label_set_long_mode(s_mini_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_mini_label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_mini_label, mini_font, 0);
    /* 文字比气泡宽时自动循环滚动；60 px/s 比 LVGL 默认稍快。 */
    lv_obj_set_style_anim_duration(s_mini_label, lv_anim_speed(60), 0);

    lv_obj_add_event_cb(box, [](lv_event_t *e) {
        (void)e;
        s_user_dismissed = false;
        mini_hide();
        ui_shell_wake();
        if (!s_chat_open && s_root != NULL) open_chat(s_root);
    }, LV_EVENT_CLICKED, NULL);

    return box;
}

static void mini_hide(void)
{
    if (s_mini_timer != NULL) {
        lv_timer_delete(s_mini_timer);
        s_mini_timer = NULL;
    }
    if (s_mini_label != NULL) {
        lv_label_set_text(s_mini_label, "");
    }
    if (s_mini != NULL) {
        lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mini_timer_cb(lv_timer_t *timer)
{
    s_mini_timer = NULL;
    lv_timer_delete(timer);
    mini_hide();
}

static void mini_show(ai_role_t role, const char *text)
{
    if (s_chat_open || s_mini == NULL || s_mini_label == NULL) return;
    if (text == NULL || text[0] == '\0') return;

    ui_dot_set_color(s_mini_dot, mini_role_color(role));
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mini);
    lv_label_set_text(s_mini_label, text);

    if (s_mini_timer != NULL) {
        lv_timer_delete(s_mini_timer);
        s_mini_timer = NULL;
    }
    s_mini_timer = lv_timer_create(mini_timer_cb, MINI_HIDE_MS, NULL);
}

static void auto_close_timer_cb(lv_timer_t *timer)
{
    s_auto_close_timer = NULL;
    lv_timer_delete(timer);
    if (s_auto_close && s_chat_open) {
        ESP_LOGI(TAG, "voice chat auto close UI after reply (session kept)");
        close_chat(false);
    }
}

static void auto_close_schedule(void)
{
    if (!s_auto_close || !s_chat_open || s_auto_close_timer != NULL) return;
    s_auto_close_timer = lv_timer_create(auto_close_timer_cb, 5000, NULL);
}

static void auto_close_cancel(void)
{
    if (s_auto_close_timer != NULL) {
        lv_timer_delete(s_auto_close_timer);
        s_auto_close_timer = NULL;
    }
}

const lv_font_t *ai_ui_text_font(void)
{
    return s_chat_font;
}

void ai_ui_init(void)
{
    if (!ai_available()) return;
    if (s_queue != NULL) return;

    s_queue = xQueueCreate(8, sizeof(UiEvent));
    s_log = (LogMsg *)heap_caps_calloc(LOG_MAX, sizeof(LogMsg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_log == NULL) {
        ESP_LOGE(TAG, "no PSRAM for chat log");
    }

    size_t font_size = 0;
    uint8_t *font_buf = ai_assets_font_buffer(&font_size);
    if (font_buf != NULL && font_size > 0) {
        s_chat_font = cbin_font_create(font_buf);
        if (s_chat_font == NULL) {
            ESP_LOGE(TAG, "cbin font create failed");
        }
    }

    s_timer = lv_timer_create([](lv_timer_t *timer) {
        (void)timer;
        if (s_queue == NULL) return;
        UiEvent event;
        while (xQueueReceive(s_queue, &event, 0) == pdTRUE) {
            if (event.type == UI_EVT_STATE) {
                ai_state_t state = (ai_state_t)event.role;
                apply_state(state);
                if (state == AI_STATE_ACTIVATING || state == AI_STATE_CONNECTING ||
                    state == AI_STATE_LISTENING || state == AI_STATE_SPEAKING) {
                    ui_shell_wake();
                }
                if (state == AI_STATE_CONNECTING) {
                    auto_close_cancel();
                    if (!s_chat_open && !s_user_dismissed && s_root != NULL) {
                        s_auto_close = true;
                        open_chat(s_root);
                        ui_shell_wake();
                    }
                }
                if (state == AI_STATE_SPEAKING) {
                    auto_close_cancel();
                    if (!s_chat_open && !s_user_dismissed && s_root != NULL) {
                        s_auto_close = true;
                        open_chat(s_root);
                        ui_shell_wake();
                    }
                }
                if (state == AI_STATE_IDLE || state == AI_STATE_DISABLED) {
                    s_user_dismissed = false;
                }
                if (state == AI_STATE_IDLE) {
                    auto_close_schedule();
                }
            } else if (event.type == UI_EVT_MESSAGE) {
                ai_role_t role = (ai_role_t)event.role;
                auto_close_cancel();
                /* 回到最初的逻辑：用户/小智的新信息都自动弹出完整对话，
                 * 除非用户手动关闭过；手动关闭后只显示小气泡。 */
                if (!s_chat_open && !s_user_dismissed && s_root != NULL) {
                    s_auto_close = true;
                    open_chat(s_root);
                    ui_shell_wake();
                }
                log_push(role, event.text);
                if (s_chat_open) {
                    message_bubble(s_msg_list, event.role, event.text, true);
                } else if (s_user_dismissed) {
                    mini_show(role, event.text);
                } else {
                    unread_update();
                }
                ui_shell_note_activity();
            } else if (event.type == UI_EVT_ACTIVATION) {
                bool changed = strcmp(s_activation, event.extra) != 0;
                strlcpy(s_activation, event.extra, sizeof(s_activation));
                strlcpy(s_activation_message, event.text, sizeof(s_activation_message));
                if (changed) {
                    s_user_dismissed = false;
                    s_auto_close = false;
                    auto_close_cancel();
                    if (!s_chat_open) open_chat(s_root);
                    ui_shell_wake();
                }
                if (s_activation_box && s_chat_open) {
                    lv_label_set_text(s_activation_code, s_activation[0] ? s_activation : "------");
                    lv_label_set_text(s_activation_hint,
                                      s_activation_message[0] ? s_activation_message : i18n_str(STR_AI_ACTIVATION_HINT));
                    lv_obj_remove_flag(s_activation_box, LV_OBJ_FLAG_HIDDEN);
                    if (s_msg_list) lv_obj_add_flag(s_msg_list, LV_OBJ_FLAG_HIDDEN);
                }
            } else if (event.type == UI_EVT_ERROR) {
                strlcpy(s_error, event.text, sizeof(s_error));
                apply_state(AI_STATE_ERROR);
                if (!s_chat_open) {
                    mini_show(AI_ROLE_SYSTEM, event.text);
                }
            } else if (event.type == UI_EVT_EMOTION) {
                /* Emotion is reflected through the robot state; keep the hook. */
            } else if (event.type == UI_EVT_DEMO) {
                page_home_apply_demo(event.role != 0);
            } else if (event.type == UI_EVT_REPLY_DONE) {
                auto_close_schedule();
            }
        }
    }, 80, NULL);
}

static void post_event(uint8_t type, uint8_t role, const char *text, const char *extra)
{
    if (s_queue == NULL) return;
    UiEvent event = {};
    event.type = type;
    event.role = role;
    if (text) strlcpy(event.text, text, sizeof(event.text));
    if (extra) strlcpy(event.extra, extra, sizeof(event.extra));
    xQueueSend(s_queue, &event, 0);
}

void ai_ui_on_state(ai_state_t state)
{
    post_event(UI_EVT_STATE, (uint8_t)state, NULL, NULL);
}

void ai_ui_on_message(ai_role_t role, const char *text)
{
    post_event(UI_EVT_MESSAGE, (uint8_t)role, text, NULL);
}

void ai_ui_on_activation(const char *code, const char *message)
{
    post_event(UI_EVT_ACTIVATION, 0, message, code);
}

void ai_ui_on_error(const char *message)
{
    post_event(UI_EVT_ERROR, 0, message, NULL);
}

void ai_ui_on_emotion(const char *emotion)
{
    post_event(UI_EVT_EMOTION, 0, emotion, NULL);
}

void ai_ui_on_demo_switch(bool on)
{
    post_event(UI_EVT_DEMO, on ? 1 : 0, NULL, NULL);
}

void ai_ui_on_reply_done(void)
{
    post_event(UI_EVT_REPLY_DONE, 0, NULL, NULL);
}

void ai_ui_attach(lv_obj_t *root)
{
    if (!ai_available() || !ai_enabled() || root == NULL) return;
    s_root = root;

    s_robot_ring = lv_obj_create(root);
    lv_obj_set_pos(s_robot_ring, ROBOT_X - 6, ROBOT_Y - 6);
    lv_obj_set_size(s_robot_ring, ROBOT_SIZE + 12, ROBOT_SIZE + 12);
    lv_obj_set_style_bg_opa(s_robot_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_robot_ring, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_robot_ring, 2, 0);
    lv_obj_set_style_border_opa(s_robot_ring, 40, 0);
    lv_obj_set_style_radius(s_robot_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(s_robot_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_robot_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_robot_ring, LV_OBJ_FLAG_HIDDEN);

    s_robot = ui_gradient_tile_create(root, ROBOT_X, ROBOT_Y, ROBOT_SIZE, ROBOT_SIZE,
                                      22, UI_COLOR_ACCENT_DEEP, UI_COLOR_VIOLET);
    lv_obj_add_flag(s_robot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_robot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_border_color(s_robot, lv_color_hex(UI_COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_border_width(s_robot, 1, 0);
    lv_obj_set_style_border_opa(s_robot, 90, 0);
    lv_obj_set_style_shadow_width(s_robot, 18, 0);
    lv_obj_set_style_shadow_color(s_robot, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_opa(s_robot, 55, 0);

    /* 自定义机器人脸：深色面罩 + 两只发光眼睛 + 嘴。比 MDI 机器人字形精致。 */
    s_visor = lv_obj_create(s_robot);
    lv_obj_set_pos(s_visor, 12, 15);
    lv_obj_set_size(s_visor, 40, 25);
    lv_obj_set_style_bg_color(s_visor, lv_color_hex(0x050912), 0);
    lv_obj_set_style_bg_opa(s_visor, 230, 0);
    lv_obj_set_style_border_color(s_visor, lv_color_hex(UI_COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_border_width(s_visor, 1, 0);
    lv_obj_set_style_border_opa(s_visor, 55, 0);
    lv_obj_set_style_radius(s_visor, 12, 0);
    lv_obj_set_style_pad_all(s_visor, 0, 0);
    lv_obj_remove_flag(s_visor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_visor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_visor, LV_OBJ_FLAG_CLICKABLE);

    s_eye_l = lv_obj_create(s_visor);
    lv_obj_set_pos(s_eye_l, 9, 9);
    lv_obj_set_size(s_eye_l, 5, 7);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(UI_COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(s_eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_l, 0, 0);
    lv_obj_set_style_radius(s_eye_l, 2, 0);
    lv_obj_remove_flag(s_eye_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_eye_l, LV_OBJ_FLAG_CLICKABLE);

    s_eye_r = lv_obj_create(s_visor);
    lv_obj_set_pos(s_eye_r, 26, 9);
    lv_obj_set_size(s_eye_r, 5, 7);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(UI_COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(s_eye_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_r, 0, 0);
    lv_obj_set_style_radius(s_eye_r, 2, 0);
    lv_obj_remove_flag(s_eye_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_eye_r, LV_OBJ_FLAG_CLICKABLE);

    s_mouth = lv_obj_create(s_visor);
    lv_obj_set_pos(s_mouth, 15, 19);
    lv_obj_set_size(s_mouth, 10, 2);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(UI_COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mouth, 0, 0);
    lv_obj_set_style_radius(s_mouth, 1, 0);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(s_robot, [](lv_event_t *e) {
        (void)e;
        if (s_chat_open) {
            close_chat(true);
            return;
        }
        s_user_dismissed = false;
        s_auto_close = false;
        auto_close_cancel();
        mini_hide();
        open_chat(s_root);
        ai_state_t state = ai_get_state();
        if (state == AI_STATE_IDLE) {
            ai_request_listen();
        } else if (state == AI_STATE_SPEAKING) {
            ai_abort_speaking();
        }
    }, LV_EVENT_CLICKED, NULL);

    s_robot_dot = ui_dot_create(root, ROBOT_X + ROBOT_SIZE - 10, ROBOT_Y + 4, 10);
    lv_obj_add_flag(s_robot_dot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_unread = ui_dot_create(root, ROBOT_X + 2, ROBOT_Y + 4, 10);
    ui_dot_set_color(s_unread, UI_COLOR_ERROR);
    lv_obj_add_flag(s_unread, LV_OBJ_FLAG_HIDDEN);

    /* 手动关闭完整对话后，新信息只用这个小气泡提示。
     * 位置贴着机器人上方，文字过长时横向循环滚动。 */
    s_mini = mini_create(root);

    apply_state(ai_get_state());
    unread_update();
#if CONFIG_TEMPLATE_XIAOZHI_AUTO_OPEN_CHAT
    open_chat(root);
    ai_ui_on_message(AI_ROLE_ASSISTANT, "你好，我是小智。");
#endif
}

void ai_ui_detach(void)
{
    close_chat(false);
    if (s_robot_ring != NULL) {
        ring_set_busy(false);
    }
    if (s_robot != NULL) lv_obj_delete(s_robot);
    if (s_robot_ring != NULL) lv_obj_delete(s_robot_ring);
    if (s_robot_dot != NULL) lv_obj_delete(s_robot_dot);
    if (s_unread != NULL) lv_obj_delete(s_unread);
    if (s_mini != NULL) lv_obj_delete(s_mini);
    s_robot = NULL;
    s_robot_ring = NULL;
    s_visor = NULL;
    s_eye_l = NULL;
    s_eye_r = NULL;
    s_mouth = NULL;
    s_robot_dot = NULL;
    s_unread = NULL;
    s_mini = NULL;
    s_mini_label = NULL;
    s_mini_dot = NULL;
    s_root = NULL;
}

bool ai_ui_is_busy(void)
{
    /* Only the visible bubble keeps the screen awake. Once it auto-collapses,
     * the session may keep listening, but the screen is allowed to sleep; a new
     * STT/TTS reopens the bubble and wakes the screen. */
    return s_chat_open;
}
