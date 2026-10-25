#include "ui_shell.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "hw.h"
#include "settings.h"
#include "ui_widgets.h"

extern const ui_page_t page_home;
extern const ui_page_t page_sysinfo;
extern const ui_page_t page_settings;
extern const ui_page_t page_history;

static const char *TAG = "ui.shell";

/* 整屏 Aurora 背景：竖向深色渐变 + 两团很淡的蓝/紫眩光。
 * 用一张 1024x600 的 RGB565 背景图实现，顶栏/内容/底栏都透明，
 * 这样不会出现三段纯色拼接。图放在 PSRAM，只生成一次。 */
#define AURORA_W  UI_SCREEN_W
#define AURORA_H  UI_SCREEN_H

static lv_image_dsc_t s_aurora_dsc;
static uint8_t *s_aurora_buf;
static bool s_aurora_ready;

static float aurora_glow(float x, float y, float cx, float cy,
                         float radius, float amp)
{
    float dx = (x - cx) / radius;
    float dy = (y - cy) / radius;
    float d2 = dx * dx + dy * dy;
    if (d2 >= 1.0f) {
        return 0.0f;
    }
    float f = 1.0f - sqrtf(d2);
    return amp * f * f;
}

static void aurora_build(void)
{
    if (s_aurora_ready) {
        return;
    }

    size_t bytes = (size_t)AURORA_W * AURORA_H * 2;
    s_aurora_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_aurora_buf == NULL) {
        ESP_LOGW(TAG, "aurora background: no PSRAM, fall back to gradients");
        return;
    }

    const float ar = 0x7B, ag = 0x88, ab = 0xFF;   /* accent */
    const float vr = 0xA0, vg = 0x7C, vb = 0xFF;   /* violet */
    const float amp_a = 50.0f / 255.0f;
    const float amp_v = 32.0f / 255.0f;

    /* Floyd–Steinberg 误差扩散：纯渐变在 RGB565 上会有大色阶，
     * 误差扩散只把量化误差分到相邻像素，肉眼连续，也不会出现规则网格颗粒。 */
    size_t err_count = (size_t)AURORA_W * 3;
    float *err_cur = heap_caps_calloc(err_count, sizeof(float), MALLOC_CAP_DEFAULT);
    float *err_next = heap_caps_calloc(err_count, sizeof(float), MALLOC_CAP_DEFAULT);
    if (err_cur == NULL || err_next == NULL) {
        heap_caps_free(err_cur);
        heap_caps_free(err_next);
        err_cur = err_next = NULL;
    }

    for (int y = 0; y < AURORA_H; y++) {
        float ty = (float)y / (AURORA_H - 1);
        float br = 0x06 + (0x09 - 0x06) * ty;
        float bg = 0x0A + (0x11 - 0x0A) * ty;
        float bb = 0x14 + (0x20 - 0x14) * ty;
        if (err_next != NULL) {
            memset(err_next, 0, err_count * sizeof(float));
        }

        for (int x = 0; x < AURORA_W; x++) {
            float a1 = aurora_glow((float)x, (float)y, 850.0f, 40.0f,
                                   260.0f, amp_a);
            float a2 = aurora_glow((float)x, (float)y, 120.0f, 500.0f,
                                   240.0f, amp_v);

            float r = br * (1.0f - a1) + ar * a1;
            float g = bg * (1.0f - a1) + ag * a1;
            float b = bb * (1.0f - a1) + ab * a1;
            r = r * (1.0f - a2) + vr * a2;
            g = g * (1.0f - a2) + vg * a2;
            b = b * (1.0f - a2) + vb * a2;

            int r5;
            int g6;
            int b5;
            if (err_cur != NULL) {
                int idx = x * 3;
                r += err_cur[idx];
                g += err_cur[idx + 1];
                b += err_cur[idx + 2];
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;

                r5 = (int)r >> 3;
                g6 = (int)g >> 2;
                b5 = (int)b >> 3;
                int rq = (r5 << 3) | (r5 >> 2);
                int gq = (g6 << 2) | (g6 >> 4);
                int bq = (b5 << 3) | (b5 >> 2);
                float er = r - rq;
                float eg = g - gq;
                float eb = b - bq;

                if (x + 1 < AURORA_W) {
                    err_cur[idx + 3] += er * 7.0f / 16.0f;
                    err_cur[idx + 4] += eg * 7.0f / 16.0f;
                    err_cur[idx + 5] += eb * 7.0f / 16.0f;
                }
                if (y + 1 < AURORA_H) {
                    if (x > 0) {
                        err_next[idx - 3] += er * 3.0f / 16.0f;
                        err_next[idx - 2] += eg * 3.0f / 16.0f;
                        err_next[idx - 1] += eb * 3.0f / 16.0f;
                    }
                    err_next[idx] += er * 5.0f / 16.0f;
                    err_next[idx + 1] += eg * 5.0f / 16.0f;
                    err_next[idx + 2] += eb * 5.0f / 16.0f;
                    if (x + 1 < AURORA_W) {
                        err_next[idx + 3] += er * 1.0f / 16.0f;
                        err_next[idx + 4] += eg * 1.0f / 16.0f;
                        err_next[idx + 5] += eb * 1.0f / 16.0f;
                    }
                }
            } else {
                r5 = (int)r >> 3;
                g6 = (int)g >> 2;
                b5 = (int)b >> 3;
            }

            uint16_t c = (uint16_t)(r5 << 11) | (uint16_t)(g6 << 5) | (uint16_t)b5;
            s_aurora_buf[(y * AURORA_W + x) * 2] = (uint8_t)(c & 0xFF);
            s_aurora_buf[(y * AURORA_W + x) * 2 + 1] = (uint8_t)(c >> 8);
        }

        if (err_cur != NULL) {
            float *tmp = err_cur;
            err_cur = err_next;
            err_next = tmp;
        }
    }

    heap_caps_free(err_cur);
    heap_caps_free(err_next);

    s_aurora_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_aurora_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_aurora_dsc.header.w = AURORA_W;
    s_aurora_dsc.header.h = AURORA_H;
    s_aurora_dsc.header.stride = AURORA_W * 2;
    s_aurora_dsc.data_size = (uint32_t)bytes;
    s_aurora_dsc.data = s_aurora_buf;
    s_aurora_ready = true;
    ESP_LOGI(TAG, "aurora background ready (%u bytes PSRAM)", (unsigned)bytes);
}

typedef struct {
    lv_obj_t *box;      /* 整块点击区 */
    lv_obj_t *pill;     /* 选中时浮起的胶囊底 */
    lv_obj_t *icon;
    lv_obj_t *name;
} nav_item_t;

static const ui_page_t *s_pages[] = {
    &page_home,
    &page_sysinfo,
    &page_history,
    &page_settings,
};

#define NAV_COUNT (int)(sizeof(s_pages) / sizeof(s_pages[0]))

static lv_obj_t *s_root;
static lv_obj_t *s_header;
static lv_obj_t *s_content;
static lv_obj_t *s_nav;
static lv_obj_t *s_title;
static lv_obj_t *s_uptime;
/* Material Design Icons：wifi-strength-1..4 与 ethernet（PUA 编码） */
#define ICON_WIFI_1 "\xF3\xB0\xA4\x9F"
#define ICON_WIFI_2 "\xF3\xB0\xA4\xA2"
#define ICON_WIFI_3 "\xF3\xB0\xA4\xA5"
#define ICON_WIFI_4 "\xF3\xB0\xA4\xA8"
#define ICON_ETH    "\xF3\xB0\x88\x80"

static lv_obj_t *s_net_wifi;
static lv_obj_t *s_net_eth;
static int s_wifi_level = -1;
static lv_obj_t *s_net_chip;
static lv_obj_t *s_off_overlay;
static lv_obj_t *s_nav_marker;      /* 底栏顶部那根会滑动的强调条 */
static bool s_nav_marker_ready;     /* 第一次定位直接跳，之后才做动画 */
static int s_net_kind = -1;         /* bit0=有线, bit1=Wi-Fi，-1=还没算过 */

static nav_item_t s_nav_items[NAV_COUNT];
static const ui_page_t *s_current;
static lv_timer_t *s_chrome_timer;
static lv_timer_t *s_idle_timer;

/* ---------------- 顶栏 ---------------- */

static void wifi_icon_set(int level, uint32_t color)
{
    if (s_net_wifi == NULL) {
        return;
    }
    if (level < 1) {
        level = 1;
    } else if (level > 4) {
        level = 4;
    }

    static const char *const glyphs[4] = {
        ICON_WIFI_1, ICON_WIFI_2, ICON_WIFI_3, ICON_WIFI_4,
    };
    lv_label_set_text(s_net_wifi, glyphs[level - 1]);
    ui_label_set_text_color(s_net_wifi, color);
}

/* 有线和 Wi-Fi 可同时显示；Wi-Fi 用 1~4 级图标 */
static void net_icons_apply(int state, int level)
{
    bool eth = (state & 1) != 0;
    bool wifi = (state & 2) != 0;

    if (s_net_wifi != NULL) {
        if (wifi || !eth) {
            lv_obj_remove_flag(s_net_wifi, LV_OBJ_FLAG_HIDDEN);
            wifi_icon_set(wifi ? (level > 0 ? level : 1) : 4,
                          wifi ? UI_COLOR_CYAN : UI_COLOR_DISABLED);
        } else {
            lv_obj_add_flag(s_net_wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_net_eth != NULL) {
        if (eth) {
            lv_obj_remove_flag(s_net_eth, LV_OBJ_FLAG_HIDDEN);
            ui_label_set_text_color(s_net_eth, UI_COLOR_OK);
        } else {
            lv_obj_add_flag(s_net_eth, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_net_chip != NULL) {
        lv_obj_update_layout(s_net_chip);
    }
}

static void chrome_update(void)
{
    if (s_uptime == NULL) {
        return;
    }

    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_uptime, "%s %02lu:%02lu:%02lu",
                          i18n_str(STR_CHROME_UPTIME),
                          (unsigned long)(sec / 3600),
                          (unsigned long)((sec / 60) % 60),
                          (unsigned long)(sec % 60));

    int state = (g_hw[HW_ETH].ok ? 1 : 0) | (g_hw[HW_WIFI].ok ? 2 : 0);
    int rssi = -127;
    int level = 0;
    if (g_hw[HW_WIFI].ok) {
        rssi = hw_wifi_get_rssi();
        if (rssi >= -55) {
            level = 4;
        } else if (rssi >= -65) {
            level = 3;
        } else if (rssi >= -75) {
            level = 2;
        } else {
            level = 1;
        }
    }

    if (state != s_net_kind || level != s_wifi_level) {
        s_net_kind = state;
        s_wifi_level = level;
        ESP_LOGI(TAG, "net: eth=%d wifi=%d rssi=%d level=%d",
                 (state & 1) != 0, (state & 2) != 0, rssi, level);
        net_icons_apply(state, level);
    }
}

static void chrome_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    chrome_update();
}

/* ---------------- 底栏 ---------------- */

static int nav_marker_x(int index)
{
    int start_x = (UI_SCREEN_W - UI_NAV_ITEM_W * NAV_COUNT) / 2;
    return start_x + index * UI_NAV_ITEM_W + (UI_NAV_ITEM_W - 48) / 2;
}

static void nav_anim_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
}

static void nav_anim_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

/* 顶部强调条在三个 tab 之间滑动；第一次定位直接跳，避免重建时横穿整条底栏 */
static void nav_marker_move(int index)
{
    if (s_nav_marker == NULL) {
        return;
    }

    int target = nav_marker_x(index);
    lv_anim_delete(s_nav_marker, nav_anim_x_cb);

    if (!s_nav_marker_ready) {
        lv_obj_set_x(s_nav_marker, target);
        s_nav_marker_ready = true;
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_nav_marker);
    lv_anim_set_values(&anim, lv_obj_get_x(s_nav_marker), target);
    lv_anim_set_duration(&anim, 280);
    lv_anim_set_path_cb(&anim, lv_anim_path_custom_bezier3);
    LV_ANIM_SET_EASE_OUT_CUBIC(&anim);
    lv_anim_set_exec_cb(&anim, nav_anim_x_cb);
    lv_anim_start(&anim);
}

/* 胶囊底只淡入淡出背景，不动文字；比整块硬切柔和 */
static void nav_pill_fade(lv_obj_t *pill, lv_opa_t target)
{
    lv_anim_delete(pill, nav_anim_opa_cb);

    lv_opa_t from = lv_obj_get_style_bg_opa(pill, 0);
    if (from == target) {
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, pill);
    lv_anim_set_values(&anim, from, target);
    lv_anim_set_duration(&anim, 240);
    lv_anim_set_path_cb(&anim, lv_anim_path_custom_bezier3);
    LV_ANIM_SET_EASE_OUT_CUBIC(&anim);
    lv_anim_set_exec_cb(&anim, nav_anim_opa_cb);
    lv_anim_start(&anim);
}

static void nav_apply_state(int selected)
{
    for (int i = 0; i < NAV_COUNT; i++) {
        bool on = (i == selected);
        nav_item_t *item = &s_nav_items[i];

        ui_label_set_text_color(item->icon, on ? UI_COLOR_ACCENT : UI_COLOR_MUTED);
        ui_label_set_text_color(item->name, on ? UI_COLOR_TEXT : UI_COLOR_MUTED);

        /* 选中态 = 半透明渐变胶囊 + 顶部 3 px 强调条（Material 3 导航条做法） */
        lv_obj_set_style_bg_color(item->pill, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(item->pill, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(item->pill, LV_GRAD_DIR_HOR, 0);
        nav_pill_fade(item->pill, on ? UI_OPA_TINT : LV_OPA_TRANSP);
        lv_obj_set_style_border_width(item->pill, 1, 0);
        lv_obj_set_style_border_color(item->pill, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(item->pill, on ? 55 : 0, 0);
    }

    nav_marker_move(selected);
}

static void nav_clicked(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= NAV_COUNT) {
        return;
    }
    ESP_LOGI(TAG, "nav click -> %s", s_pages[index]->id);
    ui_shell_show(s_pages[index]);
}

/* ---------------- 息屏 ---------------- */

static void wake_up(lv_event_t *e)
{
    (void)e;
    hw_display_set_backlight(true);
    if (s_off_overlay != NULL) {
        lv_obj_delete(s_off_overlay);
        s_off_overlay = NULL;
    }
    ESP_LOGI(TAG, "wake up from screen off");
}

static void idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    const sys_settings_t *cfg = settings_get();
    if (cfg->screen_off_s == 0 || s_off_overlay != NULL || s_root == NULL) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) < (uint32_t)cfg->screen_off_s * 1000U) {
        return;
    }

    /* 盖一层全屏遮罩：第一次触摸只负责亮屏，不会误点到下面的控件 */
    s_off_overlay = lv_obj_create(s_root);
    lv_obj_set_size(s_off_overlay, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_off_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_off_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_off_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_off_overlay, 0, 0);
    lv_obj_set_style_radius(s_off_overlay, 0, 0);
    lv_obj_remove_flag(s_off_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_off_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_off_overlay, wake_up, LV_EVENT_PRESSED, NULL);

    hw_display_set_backlight(false);
    ESP_LOGI(TAG, "screen off after %us idle", cfg->screen_off_s);
}

/* ---------------- 构建 ---------------- */

static void nav_build(void)
{
    s_nav = lv_obj_create(s_root);
    lv_obj_set_pos(s_nav, 0, UI_HEADER_H + UI_CONTENT_H);
    lv_obj_set_size(s_nav, UI_SCREEN_W, UI_NAV_H);
    if (s_aurora_ready) {
        lv_obj_set_style_bg_opa(s_nav, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(s_nav, lv_color_hex(UI_COLOR_BG_BOT), 0);
        lv_obj_set_style_bg_grad_color(s_nav, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_grad_dir(s_nav, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_nav, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_border_side(s_nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_nav, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(s_nav, 1, 0);
    lv_obj_set_style_radius(s_nav, 0, 0);
    lv_obj_set_style_pad_all(s_nav, 0, 0);
    lv_obj_remove_flag(s_nav, LV_OBJ_FLAG_SCROLLABLE);

    int total_w = UI_NAV_ITEM_W * NAV_COUNT;
    int start_x = (UI_SCREEN_W - total_w) / 2;

    for (int i = 0; i < NAV_COUNT; i++) {
        int box_x = start_x + i * UI_NAV_ITEM_W;

        /* 外层整块可点，保证点图标旁边的空白也能切页 */
        lv_obj_t *box = lv_obj_create(s_nav);
        lv_obj_set_pos(box, box_x, 0);
        lv_obj_set_size(box, UI_NAV_ITEM_W, UI_NAV_H);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(box, UI_OPA_SOFT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(box, UI_RADIUS, LV_STATE_PRESSED);
        lv_obj_add_event_cb(box, nav_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* 胶囊：宽度随内容，图标与文字同一排 */
        lv_obj_t *pill = lv_obj_create(box);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, 44);
        lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(pill, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(pill, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(pill, 0, 0);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(pill, 18, 0);
        lv_obj_set_style_pad_ver(pill, 0, 0);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(pill, UI_SPACE_S, 0);

        lv_obj_t *icon = ui_label_create(pill, s_pages[i]->symbol, 0, 0,
                                         UI_COLOR_MUTED, UI_FONT_ICON);
        lv_obj_t *name = ui_label_create(pill, i18n_str(s_pages[i]->name), 0, 0,
                                         UI_COLOR_MUTED, UI_FONT_BODY);

        s_nav_items[i] = (nav_item_t){
            .box = box, .pill = pill, .icon = icon, .name = name,
        };
    }

    /* 顶部 3 px 强调条只建一根，选中页变化时用动画滑过去 */
    s_nav_marker = lv_obj_create(s_nav);
    lv_obj_set_pos(s_nav_marker, nav_marker_x(0), 0);
    lv_obj_set_size(s_nav_marker, 48, 3);
    lv_obj_set_style_bg_color(s_nav_marker, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_nav_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_nav_marker, 0, 0);
    lv_obj_set_style_radius(s_nav_marker, 2, 0);
    lv_obj_set_style_pad_all(s_nav_marker, 0, 0);
    lv_obj_remove_flag(s_nav_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_nav_marker, LV_OBJ_FLAG_CLICKABLE);
    s_nav_marker_ready = false;
}

static void header_build(void)
{
    s_header = lv_obj_create(s_root);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_size(s_header, UI_SCREEN_W, UI_HEADER_H);
    if (s_aurora_ready) {
        lv_obj_set_style_bg_opa(s_header, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(s_header, lv_color_hex(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_grad_color(s_header, lv_color_hex(UI_COLOR_BG), 0);
        lv_obj_set_style_bg_grad_dir(s_header, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_border_side(s_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_header, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(s_header, 1, 0);
    lv_obj_set_style_radius(s_header, 0, 0);
    lv_obj_set_style_pad_all(s_header, 0, 0);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧品牌标 + 页面标题 + 产品名。用 flex 排，标题变长也不会压字。 */
    lv_obj_t *left = lv_obj_create(s_header);
    lv_obj_set_pos(left, UI_SPACE_L, 0);
    lv_obj_set_size(left, 560, UI_HEADER_H);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 12, 0);

    lv_obj_t *brand = ui_gradient_tile_create(left, 0, 0, 32, 32, UI_RADIUS_S,
                                              UI_COLOR_ACCENT_DEEP, UI_COLOR_VIOLET);
    lv_obj_t *brand_text = ui_label_create(brand, "P4", 0, 0, UI_COLOR_ON_ACCENT,
                                           UI_FONT_SMALL);
    lv_obj_center(brand_text);

    s_title = ui_label_create(left, "", 0, 0, UI_COLOR_TEXT, UI_FONT_TITLE);

    lv_obj_t *right = lv_obj_create(s_header);
    lv_obj_set_size(right, 460, UI_HEADER_H);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, UI_SPACE_M, 0);
    lv_obj_set_style_pad_right(right, UI_SPACE_L, 0);

    s_uptime = ui_label_create(right, "", 0, 0, UI_COLOR_MUTED, UI_FONT_BODY);

    /* 网络状态：同一套 Material Design Icons，24px 视觉大小 */
    s_net_chip = lv_obj_create(right);
    lv_obj_set_size(s_net_chip, 64, 26);
    lv_obj_set_style_bg_opa(s_net_chip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_net_chip, 0, 0);
    lv_obj_set_style_pad_all(s_net_chip, 0, 0);
    lv_obj_set_style_pad_column(s_net_chip, 2, 0);
    lv_obj_remove_flag(s_net_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_net_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_net_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_net_chip, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_net_wifi = ui_label_create(s_net_chip, ICON_WIFI_4, 0, 0,
                                 UI_COLOR_CYAN, UI_FONT_NET);
    s_net_eth = ui_label_create(s_net_chip, ICON_ETH, 0, 0,
                                UI_COLOR_OK, UI_FONT_NET);
}

/* 页面容器常驻：第一次进入时创建，之后切换只做 hide/show。
 * 每次切页都销毁重建的话，设置页 60+ 个对象要花几十毫秒，触摸就会觉得卡。
 * 语言切换会走 ui_shell_rebuild()，把所有页面真正销毁重建一次。 */
typedef struct {
    lv_obj_t *container;
} page_slot_t;

static page_slot_t s_slots[NAV_COUNT];

static int page_index(const ui_page_t *page)
{
    for (int i = 0; i < NAV_COUNT; i++) {
        if (s_pages[i] == page) {
            return i;
        }
    }
    return -1;
}

static lv_obj_t *page_ensure(const ui_page_t *page)
{
    int idx = page_index(page);
    if (idx < 0) {
        return NULL;
    }
    if (s_slots[idx].container != NULL) {
        return s_slots[idx].container;
    }

    lv_obj_t *c = lv_obj_create(s_content);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_size(c, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_slots[idx].container = c;

    if (page->create != NULL) {
        page->create(c);
    }
    return c;
}

static void page_destroy_all(void)
{
    for (int i = 0; i < NAV_COUNT; i++) {
        if (s_slots[i].container == NULL) {
            continue;
        }
        if (s_pages[i]->destroy != NULL) {
            s_pages[i]->destroy();
        }
        lv_obj_delete(s_slots[i].container);
        s_slots[i].container = NULL;
    }
}

static void pages_hide_all(void)
{
    for (int i = 0; i < NAV_COUNT; i++) {
        if (s_slots[i].container != NULL) {
            lv_obj_add_flag(s_slots[i].container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void content_build(void)
{
    s_content = lv_obj_create(s_root);
    lv_obj_set_pos(s_content, 0, UI_HEADER_H);
    lv_obj_set_size(s_content, UI_SCREEN_W, UI_CONTENT_H);
    if (s_aurora_ready) {
        lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_style_bg_color(s_content, lv_color_hex(UI_COLOR_BG), 0);
        lv_obj_set_style_bg_grad_color(s_content, lv_color_hex(UI_COLOR_BG_BOT), 0);
        lv_obj_set_style_bg_grad_dir(s_content, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    /* 内容区内缩一圈：页面拿到的坐标系就是 976×420（UI_PAGE_W × UI_PAGE_H） */
    lv_obj_set_style_pad_all(s_content, UI_PAGE_PAD, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
}

static void shell_build(void)
{
    /* 重建后第一次 chrome_update 必须重新落一遍网络图标 */
    s_net_kind = -1;
    s_wifi_level = -1;

    aurora_build();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    if (s_aurora_ready) {
        lv_obj_set_style_bg_image_src(screen, &s_aurora_dsc, 0);
        lv_obj_set_style_bg_image_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_image_tiled(screen, false, 0);
    }
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_root = lv_obj_create(screen);
    lv_obj_set_size(s_root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    header_build();
    content_build();
    nav_build();

    if (s_idle_timer == NULL) {
        s_idle_timer = lv_timer_create(idle_timer_cb, 1000, NULL);
    }
}

/* ---------------- 自检：反复切页看有没有泄漏 ---------------- */

#if CONFIG_TEMPLATE_UI_SELFTEST
static void selftest_cb(lv_timer_t *timer)
{
    (void)timer;
    static int index;
    index = (index + 1) % NAV_COUNT;

    int64_t t0 = esp_timer_get_time();
    ui_shell_show(s_pages[index]);
    int64_t cost = esp_timer_get_time() - t0;

    ESP_LOGI(TAG, "selftest -> %s, switch=%lld us, free heap=%lu, internal=%lu",
             s_pages[index]->id, cost,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
#endif

/* ---------------- 对外接口 ---------------- */

void ui_shell_init(void)
{
    shell_build();

    s_current = s_pages[0];
    lv_label_set_text(s_title, i18n_str(s_current->name));
    lv_obj_update_layout(s_header);
    lv_obj_t *first = page_ensure(s_current);
    ui_animate_slide_in(first, 24);
    nav_apply_state(0);
    chrome_update();

    /* 顶栏 1 s 刷新一次运行时长与网络状态（内容没变就不动控件，避免无谓重排） */
    if (s_chrome_timer == NULL) {
        s_chrome_timer = lv_timer_create(chrome_timer_cb, 1000, NULL);
    }

#if CONFIG_TEMPLATE_UI_SELFTEST
    lv_timer_create(selftest_cb, 2000, NULL);
    ESP_LOGW(TAG, "UI selftest enabled: switching pages every 2s");
#endif

    ESP_LOGI(TAG, "shell ready, %d pages", NAV_COUNT);
}

void ui_shell_show(const ui_page_t *page)
{
    if (page == NULL || page == s_current) {
        return;
    }
    int idx = page_index(page);
    if (idx < 0) {
        return;
    }

    int64_t t0 = esp_timer_get_time();

    pages_hide_all();
    lv_obj_t *container = page_ensure(page);
    if (container == NULL) {
        return;
    }
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
    ui_animate_slide_in(container, 24);

    s_current = page;
    lv_label_set_text(s_title, i18n_str(page->name));
    lv_obj_update_layout(s_header);
    nav_apply_state(idx);

    ESP_LOGD(TAG, "show %s: %lld us", page->id, esp_timer_get_time() - t0);
}

const ui_page_t *ui_shell_current(void)
{
    return s_current;
}

void ui_shell_refresh_chrome(void)
{
    if (s_title != NULL && s_current != NULL) {
        lv_label_set_text(s_title, i18n_str(s_current->name));
        lv_obj_update_layout(s_header);
    }
    if (s_nav != NULL) {
        for (int i = 0; i < NAV_COUNT; i++) {
            lv_label_set_text(s_nav_items[i].name, i18n_str(s_pages[i]->name));
        }
    }
    chrome_update();
}

void ui_shell_rebuild(void)
{
    const ui_page_t *keep = s_current;

    if (s_off_overlay != NULL) {
        lv_obj_delete(s_off_overlay);
        s_off_overlay = NULL;
    }
    page_destroy_all();
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_header = s_content = s_nav = s_title = s_uptime = s_net_wifi = NULL;
    s_net_eth = NULL;
    s_net_chip = NULL;
    s_net_kind = -1;
    s_wifi_level = -1;
    s_nav_marker = NULL;
    s_nav_marker_ready = false;

    shell_build();

    s_current = keep;
    lv_label_set_text(s_title, i18n_str(s_current->name));
    lv_obj_update_layout(s_header);
    lv_obj_t *container = page_ensure(s_current);
    ui_animate_slide_in(container, 24);
    for (int i = 0; i < NAV_COUNT; i++) {
        if (s_pages[i] == s_current) {
            nav_apply_state(i);
            break;
        }
    }
    chrome_update();
    ESP_LOGI(TAG, "shell rebuilt (lang switch)");
}
