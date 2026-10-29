#include "page_history.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"

/* 只需要日历按钮的 draw task 描述，用来给起点/终点日格上不同颜色 */
#include "src/draw/lv_draw_private.h"
#include "src/draw/lv_draw_rect.h"

#include "i18n.h"
#include "ui_widgets.h"

static const char *TAG = "ui.history";

/* 历史曲线页：单窗口 XY 曲线 + 日期时间轴 + 游标/缩放 + 刻度平移 + 区段查找。
 *
 * 数据是按时间实时生成的模拟曲线，不占用历史缓冲区：
 *   - 窗口 = 用户设定的秒数，可点胶囊中间数值按时/分/秒输入，也可 +/- 或双指缩放；
 *   - 显示区内单指只做游标，底部 X 刻度左右拖平移时间，左侧 Y 刻度上下拖平移数值；
 *   - 区段查找直接在日历上点起点/终点，确认后按区段长度和结束时间定位视图；
 *   - 曲线始终用 240 个采样点铺满当前窗口，窗口再大也不会加重点数。 */

#define CHART_POINTS   240
#define MIN_WINDOW_S   1
#define MAX_WINDOW_S   999999999   /* 仅防整数溢出，不是 UI 限制 */

#define WIN_W          UI_PAGE_W
#define WIN_H          UI_PAGE_H
#define PLOT_X         80
#define PLOT_Y         66
#define PLOT_W         (WIN_W - PLOT_X - 20)          /* 876 */
#define PLOT_H         322
#define DIVIDER_Y      54
#define X_LABEL_Y      (PLOT_Y + PLOT_H + 10)         /* 398 */
#define X_LABEL_W      140

typedef struct {
    uint32_t color;
    const char *symbol;
    const char *unit;
    int min;
    int max;                 /* 值都以 0.1 为单位 */
} series_t;

static const series_t s_series[2] = {
    { UI_COLOR_WARN, LV_SYMBOL_TINT, "℃", 150, 350 },
    { UI_COLOR_CYAN, LV_SYMBOL_BARS, "kPa", 950, 1050 },
};

static int s_selected;              /* 0 = 温度，1 = 压力 */
static int s_window_s = 40;         /* 当前显示窗口，秒 */
static int s_pinch_base_s = 40;
static bool s_pinching;

static float s_page_start_s;        /* 页面创建时刻，秒 */
static float s_chart_now;           /* 最近一次重绘用的时间轴 */
static int s_chart_window;          /* 最近一次重绘用的窗口 */

static lv_obj_t *s_root;
static lv_obj_t *s_dropdown;
static lv_obj_t *s_badge;
static lv_obj_t *s_value;
static lv_obj_t *s_readout;
static lv_obj_t *s_date_label;
static lv_obj_t *s_y_label[5];
static lv_obj_t *s_x_label[5];
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser;
static lv_obj_t *s_cursor;
static lv_obj_t *s_cursor_dot;
static lv_obj_t *s_touch_area;
static lv_obj_t *s_zoom_box;
static lv_obj_t *s_zoom_minus;
static lv_obj_t *s_zoom_plus;
static lv_obj_t *s_zoom_minus_icon;
static lv_obj_t *s_zoom_plus_icon;
static lv_obj_t *s_zoom_input;
static lv_obj_t *s_zoom_label;
static lv_obj_t *s_input_scrim;
static lv_obj_t *s_input_h;
static lv_obj_t *s_input_m;
static lv_obj_t *s_input_s;

static lv_obj_t *s_range_scrim;
static lv_obj_t *s_range_cal;
static lv_obj_t *s_range_start_hour;
static lv_obj_t *s_range_start_min;
static lv_obj_t *s_range_end_hour;
static lv_obj_t *s_range_end_min;
static lv_obj_t *s_range_start_label;
static lv_obj_t *s_range_end_label;
static lv_obj_t *s_range_month_label;
static int s_range_pick_stage;      /* 0 = 下一次点日历选起点，1 = 选终点 */
static int s_range_shown_year;
static int s_range_shown_month;
static struct tm s_range_tm[2];

static lv_timer_t *s_timer;
static int s_cursor_idx = -1;
static time_t s_chart_end_epoch;

/* 拖动视图：X 方向是相对“现在”的时间偏移，Y 方向是当前系列的数值偏移 */
static int s_view_shift_s;
static int s_y_shift[2];
static bool s_dragging;
static lv_point_t s_drag_start;
static int s_drag_shift_s;
static int s_drag_y_shift;
static int s_drag_window_s;
static int s_pan_axis;              /* 0=未定, 1=只拖 X, 2=只拖 Y, 3=同时 */
static bool s_drag_from_scale;      /* 从刻度区起手才平移，显示区只做游标 */
static int64_t s_last_pan_ms;

static float now_s(void)
{
    return (float)(esp_timer_get_time() / 1000000.0) - s_page_start_s;
}

static int current_window_s(void)
{
    return s_window_s;
}

static int sim_value(int series, float t)
{
    if (series == 0) {
        return (int)(250.0f + 35.0f * sinf(t / 23.0f)
                     + 12.0f * sinf(t / 7.0f + 0.3f));
    }
    return (int)(1013.0f + 12.0f * sinf(t / 31.0f + 0.7f)
                 + 5.0f * sinf(t / 9.0f));
}

static int y_view_min(void)
{
    return s_series[s_selected].min + s_y_shift[s_selected];
}

static int y_view_max(void)
{
    return s_series[s_selected].max + s_y_shift[s_selected];
}

static void apply_y_range(void)
{
    if (s_chart != NULL) {
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y,
                                y_view_min(), y_view_max());
    }
}

static void format_tenths(char *buf, size_t size, int v, const char *unit)
{
    int a = (v < 0) ? -v : v;
    const char *sign = (v < 0) ? "-" : "";
    if (unit != NULL) {
        snprintf(buf, size, "%s%d.%d %s", sign, a / 10, a % 10, unit);
    } else {
        snprintf(buf, size, "%s%d.%d", sign, a / 10, a % 10);
    }
}

static void format_window_label(char *buf, size_t size)
{
    int h = s_window_s / 3600;
    int m = (s_window_s % 3600) / 60;
    int s = s_window_s % 60;
    if (h > 0) {
        if (m > 0) {
            snprintf(buf, size, "%dh%02dm", h, m);
        } else {
            snprintf(buf, size, "%dh", h);
        }
    } else if (m > 0) {
        if (s > 0) {
            snprintf(buf, size, "%dm%02ds", m, s);
        } else {
            snprintf(buf, size, "%dm", m);
        }
    } else {
        snprintf(buf, size, "%ds", s);
    }
}

/* 设备没有 RTC 时，以固件编译时间作为起点，保证曲线横轴一开始就是
 * 可读的日期时间；Wi-Fi 连上后由 SNTP 校正系统时间。 */
static time_t fallback_base_epoch(void)
{
    static time_t base;
    if (base != 0) {
        return base;
    }

    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1;
    int year = 2026;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) {
        const char *p = strstr(months, mon);
        int mon_idx = (p != NULL) ? (int)(p - months) / 3 : 0;
        sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

        struct tm t = {0};
        t.tm_year = year - 1900;
        t.tm_mon = mon_idx;
        t.tm_mday = day;
        t.tm_hour = hh;
        t.tm_min = mm;
        t.tm_sec = ss;
        t.tm_isdst = -1;
        base = mktime(&t);
    }
    if (base <= 0) {
        base = 1767225600;   /* 2026-01-01 00:00:00 UTC */
    }
    return base;
}

static time_t epoch_now(void)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        return now;
    }
    return fallback_base_epoch() + (time_t)(esp_timer_get_time() / 1000000);
}

static void format_axis_time(char *buf, size_t size, time_t t)
{
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) {
        snprintf(buf, size, "--");
        return;
    }

    if (s_chart_window >= 7 * 24 * 3600) {
        strftime(buf, size, "%Y-%m-%d", &tmv);
    } else if (s_chart_window >= 60) {
        strftime(buf, size, "%m-%d %H:%M", &tmv);
    } else {
        strftime(buf, size, "%H:%M:%S", &tmv);
    }
}

static void format_clock(char *buf, size_t size, time_t t)
{
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) {
        snprintf(buf, size, "--");
        return;
    }
    strftime(buf, size, "%m-%d %H:%M:%S", &tmv);
}

static void format_date(char *buf, size_t size, time_t t)
{
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) {
        snprintf(buf, size, "--");
        return;
    }
    strftime(buf, size, "%Y-%m-%d", &tmv);
}

static void hide_cursor(void)
{
    s_cursor_idx = -1;
    if (s_cursor != NULL) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_cursor_dot != NULL) {
        lv_obj_add_flag(s_cursor_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_readout != NULL) {
        lv_obj_add_flag(s_readout, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_cursor(int idx)
{
    if (idx < 0 || idx >= CHART_POINTS || s_cursor == NULL || s_cursor_dot == NULL) {
        return;
    }

    const series_t *s = &s_series[s_selected];
    float off = -(float)s_chart_window
                + (float)idx * (float)s_chart_window / (CHART_POINTS - 1);
    int v = sim_value(s_selected, s_chart_now + off);
    int cx = PLOT_X + (idx * PLOT_W) / (CHART_POINTS - 1);
    int ymin = y_view_min();
    int ymax = y_view_max();
    int cy = PLOT_Y + PLOT_H - ((v - ymin) * PLOT_H) / (ymax - ymin);
    if (cy < PLOT_Y + 5) {
        cy = PLOT_Y + 5;
    } else if (cy > PLOT_Y + PLOT_H - 5) {
        cy = PLOT_Y + PLOT_H - 5;
    }

    s_cursor_idx = idx;
    lv_obj_set_pos(s_cursor, cx, PLOT_Y);
    lv_obj_set_pos(s_cursor_dot, cx - 5, cy - 5);
    lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cursor_dot, LV_OBJ_FLAG_HIDDEN);

    if (s_readout != NULL) {
        char tbuf[32];
        char vbuf[24];
        format_clock(tbuf, sizeof(tbuf),
                     s_chart_end_epoch + (time_t)off);
        format_tenths(vbuf, sizeof(vbuf), v, s->unit);
        lv_label_set_text_fmt(s_readout, "%s   %s", tbuf, vbuf);
        lv_obj_remove_flag(s_readout, LV_OBJ_FLAG_HIDDEN);
    }
}

#if LV_USE_GESTURE_RECOGNITION
static void pinch_event(lv_event_t *e);
#endif

static void chart_reload(void);
static void update_y_labels(void);
static void update_x_labels(void);
static void update_value(void);
static void update_zoom_ui(void);

static void cursor_from_point(lv_point_t p)
{
    if (s_chart == NULL) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(s_chart, &area);

    int x = p.x - area.x1;
    if (x < 0) {
        x = 0;
    } else if (x > PLOT_W) {
        x = PLOT_W;
    }
    show_cursor((x * (CHART_POINTS - 1)) / PLOT_W);
}

static void chart_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_GESTURE) {
#if LV_USE_GESTURE_RECOGNITION
        if (lv_event_get_gesture_type(e) == LV_INDEV_GESTURE_PINCH) {
            pinch_event(e);
        }
#endif
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        if (s_pinching) {
            return;
        }

        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL || s_chart == NULL) {
            return;
        }

        lv_indev_get_point(indev, &s_drag_start);

        lv_area_t area;
        lv_obj_get_coords(s_chart, &area);
        bool in_plot = s_drag_start.x >= area.x1 && s_drag_start.x <= area.x2
                       && s_drag_start.y >= area.y1 && s_drag_start.y <= area.y2;

        s_drag_shift_s = s_view_shift_s;
        s_drag_y_shift = s_y_shift[s_selected];
        s_drag_window_s = s_chart_window;
        s_dragging = false;
        s_pan_axis = 0;
        s_last_pan_ms = 0;
        s_drag_from_scale = !in_plot;

        if (in_plot) {
            /* 显示区：和以前一样只做游标，不拖坐标轴 */
            cursor_from_point(s_drag_start);
        } else {
            hide_cursor();
            /* 底部 X 刻度只左右拖；左侧 Y 刻度只上下拖 */
            if (s_drag_start.y > area.y2) {
                s_pan_axis = 1;
            } else if (s_drag_start.x < area.x1) {
                s_pan_axis = 2;
            }
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (s_pinching) {
            return;
        }

        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL || s_chart == NULL) {
            return;
        }

        lv_point_t p;
        lv_indev_get_point(indev, &p);

        if (!s_drag_from_scale) {
            cursor_from_point(p);
            return;
        }

        int dx = p.x - s_drag_start.x;
        int dy = p.y - s_drag_start.y;

        if (!s_dragging && (abs(dx) > 6 || abs(dy) > 6)) {
            s_dragging = true;
            hide_cursor();

            if (s_pan_axis == 0) {
                if (abs(dx) > abs(dy) * 3 / 2) {
                    s_pan_axis = 1;
                } else if (abs(dy) > abs(dx) * 3 / 2) {
                    s_pan_axis = 2;
                } else {
                    s_pan_axis = 3;
                }
            }

        }

        if (s_dragging) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - s_last_pan_ms >= 50) {
                s_last_pan_ms = now_ms;

                if (s_pan_axis == 1 || s_pan_axis == 3) {
                    /* keep dx */
                } else {
                    dx = 0;
                }
                if (s_pan_axis == 2 || s_pan_axis == 3) {
                    /* keep dy */
                } else {
                    dy = 0;
                }

                long long shift = (long long)s_drag_shift_s
                                  + (long long)dx * s_drag_window_s / PLOT_W;
                if (shift > MAX_WINDOW_S) {
                    shift = MAX_WINDOW_S;
                } else if (shift < -MAX_WINDOW_S) {
                    shift = -MAX_WINDOW_S;
                }
                s_view_shift_s = (int)shift;

                int range = s_series[s_selected].max - s_series[s_selected].min;
                long long yshift = (long long)s_drag_y_shift
                                   + (long long)dy * range / PLOT_H;
                long long ylimit = (long long)range * 10;
                if (yshift > ylimit) {
                    yshift = ylimit;
                } else if (yshift < -ylimit) {
                    yshift = -ylimit;
                }
                s_y_shift[s_selected] = (int)yshift;

                apply_y_range();
                chart_reload();
                update_y_labels();
                update_x_labels();
                update_value();
            }
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_dragging = false;
        s_drag_from_scale = false;
        s_pinching = false;
        hide_cursor();
    }
}

static void update_y_labels(void)
{
    int ymin = y_view_min();
    int ymax = y_view_max();
    for (int i = 0; i < 5; i++) {
        int v = ymax - i * (ymax - ymin) / 4;
        char buf[16];
        format_tenths(buf, sizeof(buf), v, NULL);
        lv_label_set_text(s_y_label[i], buf);
    }
}

static void update_x_labels(void)
{
    int offs[5] = {
        -s_chart_window,
        -(int)((long long)s_chart_window * 3 / 4),
        -(s_chart_window / 2),
        -(s_chart_window / 4),
        0
    };

    for (int i = 0; i < 5; i++) {
        char buf[32];
        format_axis_time(buf, sizeof(buf),
                         s_chart_end_epoch + (time_t)offs[i]);
        if (strcmp(lv_label_get_text(s_x_label[i]), buf) != 0) {
            lv_label_set_text(s_x_label[i], buf);
        }
    }

    if (s_date_label != NULL) {
        char date[16];
        format_date(date, sizeof(date), s_chart_end_epoch);
        if (strcmp(lv_label_get_text(s_date_label), date) != 0) {
            lv_label_set_text(s_date_label, date);
        }
    }
}

static void update_value(void)
{
    const series_t *s = &s_series[s_selected];
    char buf[24];
    format_tenths(buf, sizeof(buf), sim_value(s_selected, s_chart_now), s->unit);
    lv_label_set_text(s_value, buf);
    ui_label_set_text_color(s_value, s->color);
}

static void chart_reload(void)
{
    s_chart_now = now_s() - (float)s_view_shift_s;
    s_chart_window = s_window_s;
    s_chart_end_epoch = epoch_now() - (time_t)s_view_shift_s;

    for (int i = 0; i < CHART_POINTS; i++) {
        float off = -(float)s_chart_window
                    + (float)i * (float)s_chart_window / (CHART_POINTS - 1);
        lv_chart_set_next_value(s_chart, s_ser,
                                sim_value(s_selected, s_chart_now + off));
    }
    lv_chart_refresh(s_chart);
}

static void update_zoom_ui(void)
{
    if (s_zoom_label == NULL) {
        return;
    }

    char buf[24];
    format_window_label(buf, sizeof(buf));
    lv_label_set_text(s_zoom_label, buf);
    ui_label_set_text_color(s_zoom_label, UI_COLOR_MUTED);
    ui_label_set_text_color(s_zoom_minus_icon, UI_COLOR_TEXT);
    ui_label_set_text_color(s_zoom_plus_icon, UI_COLOR_TEXT);
    lv_obj_add_flag(s_zoom_minus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_zoom_plus, LV_OBJ_FLAG_CLICKABLE);
}

static void apply_fixed_window(int window_s)
{
    if (window_s < MIN_WINDOW_S) {
        window_s = MIN_WINDOW_S;
    } else if (window_s > MAX_WINDOW_S) {
        window_s = MAX_WINDOW_S;
    }

    if (window_s == s_window_s) {
        return;
    }

    s_window_s = window_s;

    hide_cursor();
    chart_reload();
    update_x_labels();
    update_value();
    update_zoom_ui();
}

static void zoom_step(int zoom_in)
{
    int window_s = s_window_s;
    if (zoom_in) {
        window_s /= 2;
        if (window_s < MIN_WINDOW_S) {
            window_s = MIN_WINDOW_S;
        }
    } else {
        if (window_s >= MAX_WINDOW_S / 2) {
            window_s = MAX_WINDOW_S;
        } else {
            window_s *= 2;
        }
    }
    apply_fixed_window(window_s);
    ESP_LOGI(TAG, "zoom -> %ds", s_window_s);
}

static void zoom_minus_cb(lv_event_t *e)
{
    (void)e;
    zoom_step(0);
}

static void zoom_plus_cb(lv_event_t *e)
{
    (void)e;
    zoom_step(1);
}

#if LV_USE_GESTURE_RECOGNITION
static void pinch_event(lv_event_t *e)
{
    lv_indev_gesture_state_t state =
        lv_event_get_gesture_state(e, LV_INDEV_GESTURE_PINCH);
    float scale = lv_event_get_pinch_scale(e);

    if (state == LV_INDEV_GESTURE_STATE_RECOGNIZED) {
        if (scale <= 0.05f) {
            return;
        }

        if (!s_pinching) {
            s_pinching = true;
            s_dragging = false;
            hide_cursor();
            s_pinch_base_s = current_window_s();
        }

        int target = (int)((float)s_pinch_base_s / scale);
        if (target < MIN_WINDOW_S) {
            target = MIN_WINDOW_S;
        } else if (target > MAX_WINDOW_S) {
            target = MAX_WINDOW_S;
        }
        apply_fixed_window(target);
    } else if (state == LV_INDEV_GESTURE_STATE_ENDED ||
               state == LV_INDEV_GESTURE_STATE_CANCELED ||
               state == LV_INDEV_GESTURE_STATE_NONE) {
        s_pinching = false;
    }
}
#endif

static void apply_series(void)
{
    hide_cursor();
    const series_t *s = &s_series[s_selected];

    ui_badge_set_color(s_badge, s->color);
    lv_obj_t *icon = lv_obj_get_child(s_badge, 0);
    if (icon != NULL) {
        lv_label_set_text(icon, s->symbol);
    }

    apply_y_range();
    lv_chart_set_series_color(s_chart, s_ser, lv_color_hex(s->color));

    chart_reload();
    update_y_labels();
    update_x_labels();
    update_value();

    if (s_readout != NULL) {
        ui_label_set_text_color(s_readout, s->color);
    }
    if (s_cursor_dot != NULL) {
        lv_obj_set_style_bg_color(s_cursor_dot, lv_color_hex(s->color), 0);
    }
}

static void dropdown_event_cb(lv_event_t *e)
{
    uint32_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel > 1) {
        sel = 0;
    }
    if ((int)sel == s_selected) {
        return;
    }
    s_selected = (int)sel;
    apply_series();
    update_zoom_ui();
    ESP_LOGI(TAG, "series -> %s", s_selected ? "pressure" : "temperature");
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    chart_reload();
    update_x_labels();
    update_value();

    if (s_cursor_idx >= 0) {
        show_cursor(s_cursor_idx);
    }
    if (s_root != NULL && !lv_obj_is_visible(s_root)) {
        hide_cursor();
    }
}

static void input_close(void)
{
    if (s_input_scrim != NULL) {
        lv_obj_delete(s_input_scrim);
        s_input_scrim = NULL;
        s_input_h = NULL;
        s_input_m = NULL;
        s_input_s = NULL;
    }
}

static long input_field_value(lv_obj_t *ta)
{
    if (ta == NULL) {
        return 0;
    }
    return strtol(lv_textarea_get_text(ta), NULL, 10);
}

static void input_confirm_cb(lv_event_t *e)
{
    (void)e;
    long long total = (long long)input_field_value(s_input_h) * 3600
                    + (long long)input_field_value(s_input_m) * 60
                    + (long long)input_field_value(s_input_s);
    if (total < MIN_WINDOW_S) {
        total = MIN_WINDOW_S;
    } else if (total > MAX_WINDOW_S) {
        total = MAX_WINDOW_S;
    }

    input_close();
    apply_fixed_window((int)total);
    ESP_LOGI(TAG, "input window -> %llds", total);
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    input_close();
}

static void input_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        input_confirm_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        input_close();
    }
}

static lv_obj_t *input_field_create(lv_obj_t *box, int x, const char *unit,
                                    const char *prefill, int max_len)
{
    ui_label_create(box, unit, x, 58, UI_COLOR_MUTED, UI_FONT_SMALL);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_obj_set_pos(ta, x, 78);
    lv_obj_set_size(ta, 148, 52);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, prefill);
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(ta, UI_FONT_BODY, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    return ta;
}

static void input_field_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_user_data(e);
    if (kb != NULL) {
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
    }
}

static void input_open(void)
{
    if (s_input_scrim != NULL) {
        return;
    }

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
    s_input_scrim = scrim;

    lv_obj_t *box = ui_card_create(scrim, 0, 0, 520, 400);
    lv_obj_center(box);

    ui_label_create(box, i18n_str(STR_HISTORY_WINDOW), 24, 20, UI_COLOR_TEXT,
                    UI_FONT_BODY);

    int window = current_window_s();
    char hbuf[12];
    char mbuf[8];
    char sbuf[8];
    snprintf(hbuf, sizeof(hbuf), "%d", window / 3600);
    snprintf(mbuf, sizeof(mbuf), "%d", (window % 3600) / 60);
    snprintf(sbuf, sizeof(sbuf), "%d", window % 60);

    s_input_h = input_field_create(box, 24, i18n_str(STR_UNIT_HOUR), hbuf, 7);
    s_input_m = input_field_create(box, 186, i18n_str(STR_UNIT_MIN), mbuf, 2);
    s_input_s = input_field_create(box, 348, i18n_str(STR_UNIT_SEC), sbuf, 2);

    lv_obj_t *cancel = ui_button_create(box, i18n_str(STR_BTN_CANCEL), 24, 148,
                                        150, 44, 0);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok = ui_button_create(box, i18n_str(STR_BTN_CONFIRM), 346, 148,
                                    150, 44, 1);
    lv_obj_add_event_cb(ok, input_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(box);
    lv_obj_set_size(kb, 472, 180);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_keyboard_set_textarea(kb, s_input_h);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 8, 0);
    lv_obj_set_style_text_font(kb, UI_FONT_ICON, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_TILE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, UI_RADIUS_S, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_ON_ACCENT),
                                LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(kb, input_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, input_kb_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_event_cb(s_input_h, input_field_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_input_m, input_field_cb, LV_EVENT_CLICKED, kb);
    lv_obj_add_event_cb(s_input_s, input_field_cb, LV_EVENT_CLICKED, kb);
}

static const char s_range_hour_opts[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";

static const char s_range_min_opts[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n"
    "10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n"
    "20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n"
    "30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n"
    "40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n"
    "50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

static void range_close(void)
{
    if (s_range_scrim != NULL) {
        lv_obj_delete(s_range_scrim);
    }
    s_range_scrim = NULL;
    s_range_cal = NULL;
    s_range_start_hour = NULL;
    s_range_start_min = NULL;
    s_range_end_hour = NULL;
    s_range_end_min = NULL;
    s_range_start_label = NULL;
    s_range_end_label = NULL;
    s_range_month_label = NULL;
}

static void range_tm_from_time(int idx, time_t t)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (localtime_r(&t, &tmv) == NULL) {
        tmv.tm_year = 126;
        tmv.tm_mon = 0;
        tmv.tm_mday = 1;
    }
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    s_range_tm[idx] = tmv;
}

static time_t range_time_from_tm(int idx)
{
    struct tm t = s_range_tm[idx];
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

static void range_update_summary(void)
{
    if (s_range_start_label == NULL || s_range_end_label == NULL) {
        return;
    }

    time_t a = range_time_from_tm(0);
    time_t b = range_time_from_tm(1);
    struct tm ta;
    struct tm tb;
    char sa[32] = "--";
    char sb[32] = "--";
    if (localtime_r(&a, &ta) != NULL) {
        strftime(sa, sizeof(sa), "%Y-%m-%d %H:%M", &ta);
    }
    if (localtime_r(&b, &tb) != NULL) {
        strftime(sb, sizeof(sb), "%Y-%m-%d %H:%M", &tb);
    }
    lv_label_set_text_fmt(s_range_start_label, "%s  %s",
                          i18n_str(STR_HISTORY_RANGE_START), sa);
    lv_label_set_text_fmt(s_range_end_label, "%s  %s",
                          i18n_str(STR_HISTORY_RANGE_END), sb);
}

static void range_set_month(int year, int month)
{
    while (month < 1) {
        month += 12;
        year--;
    }
    while (month > 12) {
        month -= 12;
        year++;
    }
    s_range_shown_year = year;
    s_range_shown_month = month;
    if (s_range_cal != NULL) {
        lv_calendar_set_month_shown(s_range_cal, year, month);
    }
    if (s_range_month_label != NULL) {
        lv_label_set_text_fmt(s_range_month_label, "%04d-%02d", year, month);
    }
}

static void range_month_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    range_set_month(s_range_shown_year, s_range_shown_month + delta);
}

static void range_hide_calendar_header(lv_obj_t *cal)
{
    lv_obj_t *btnm = lv_calendar_get_btnmatrix(cal);
    uint32_t n = lv_obj_get_child_count(cal);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(cal, i);
        if (child != btnm) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static int range_day_button_id(const struct tm *t, int year, int month)
{
    if (t->tm_year + 1900 != year || t->tm_mon + 1 != month) {
        return -1;
    }

    struct tm first;
    memset(&first, 0, sizeof(first));
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    first.tm_isdst = -1;
    if (mktime(&first) == (time_t)-1) {
        return -1;
    }

#if LV_CALENDAR_WEEK_STARTS_MONDAY
    int day_first = (first.tm_wday + 6) % 7;
#else
    int day_first = first.tm_wday;
#endif
    return t->tm_mday - 1 + day_first + 7;
}

/* 日历自带的高亮只有一种颜色，这里直接改按钮的 draw task，
 * 起点用靛蓝，终点用紫罗兰。 */
static void range_cal_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *task = lv_event_get_param(e);
    if (task == NULL) {
        return;
    }

    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)task->draw_dsc;
    if (base == NULL || base->part != LV_PART_ITEMS) {
        return;
    }

    int start_id = range_day_button_id(&s_range_tm[0],
                                       s_range_shown_year, s_range_shown_month);
    int end_id = range_day_button_id(&s_range_tm[1],
                                     s_range_shown_year, s_range_shown_month);
    bool is_start = (base->id1 == start_id);
    bool is_end = (base->id1 == end_id);
    if (!is_start && !is_end) {
        return;
    }

    uint32_t color = is_start ? UI_COLOR_ACCENT : UI_COLOR_VIOLET;
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    lv_draw_border_dsc_t *border = lv_draw_task_get_border_dsc(task);
    if (fill != NULL) {
        fill->color = lv_color_hex(color);
        fill->opa = LV_OPA_60;
    }
    if (border != NULL) {
        border->color = lv_color_hex(color);
        border->opa = LV_OPA_COVER;
        border->width = 2;
    }
    if (is_start && is_end && border != NULL) {
        border->color = lv_color_hex(UI_COLOR_VIOLET);
    }
}

static void range_date_cb(lv_event_t *e)
{
    (void)e;
    lv_calendar_date_t date;
    if (lv_calendar_get_pressed_date(s_range_cal, &date) != LV_RESULT_OK) {
        return;
    }

    int idx = s_range_pick_stage ? 1 : 0;
    struct tm *t = &s_range_tm[idx];
    t->tm_year = (int)date.year - 1900;
    t->tm_mon = (int)date.month - 1;
    t->tm_mday = (int)date.day;

    if (idx == 0) {
        if (range_time_from_tm(0) > range_time_from_tm(1)) {
            s_range_tm[1] = s_range_tm[0];
        }
        s_range_pick_stage = 1;
    } else {
        if (range_time_from_tm(1) < range_time_from_tm(0)) {
            struct tm tmp = s_range_tm[0];
            s_range_tm[0] = s_range_tm[1];
            s_range_tm[1] = tmp;
        }
        s_range_pick_stage = 0;
    }

    range_update_summary();
    lv_obj_invalidate(s_range_cal);
}

static void range_start_time_cb(lv_event_t *e)
{
    (void)e;
    if (s_range_start_hour != NULL && s_range_start_min != NULL) {
        s_range_tm[0].tm_hour = (int)lv_roller_get_selected(s_range_start_hour);
        s_range_tm[0].tm_min = (int)lv_roller_get_selected(s_range_start_min);
        range_update_summary();
    }
}

static void range_end_time_cb(lv_event_t *e)
{
    (void)e;
    if (s_range_end_hour != NULL && s_range_end_min != NULL) {
        s_range_tm[1].tm_hour = (int)lv_roller_get_selected(s_range_end_hour);
        s_range_tm[1].tm_min = (int)lv_roller_get_selected(s_range_end_min);
        range_update_summary();
    }
}

static void range_cancel_cb(lv_event_t *e)
{
    (void)e;
    range_close();
}

static void range_confirm_cb(lv_event_t *e)
{
    (void)e;

    s_range_tm[0].tm_hour = (int)lv_roller_get_selected(s_range_start_hour);
    s_range_tm[0].tm_min = (int)lv_roller_get_selected(s_range_start_min);
    s_range_tm[1].tm_hour = (int)lv_roller_get_selected(s_range_end_hour);
    s_range_tm[1].tm_min = (int)lv_roller_get_selected(s_range_end_min);

    time_t start = range_time_from_tm(0);
    time_t end = range_time_from_tm(1);
    if (start == (time_t)-1 || end == (time_t)-1) {
        range_close();
        return;
    }
    if (end < start) {
        time_t tmp = start;
        start = end;
        end = tmp;
    }
    if (end == start) {
        end += 60;
    }

    long long span = (long long)end - (long long)start;
    if (span < MIN_WINDOW_S) {
        span = MIN_WINDOW_S;
    } else if (span > MAX_WINDOW_S) {
        span = MAX_WINDOW_S;
    }

    long long now = (long long)epoch_now();
    long long shift = now - (long long)end;
    if (shift > MAX_WINDOW_S) {
        shift = MAX_WINDOW_S;
    } else if (shift < -MAX_WINDOW_S) {
        shift = -MAX_WINDOW_S;
    }

    s_window_s = (int)span;
    s_view_shift_s = (int)shift;
    range_close();

    hide_cursor();
    chart_reload();
    update_x_labels();
    update_value();
    update_zoom_ui();
    ESP_LOGI(TAG, "range -> start=%lld end=%lld window=%ds",
             (long long)start, (long long)end, s_window_s);
}

static void range_style_roller(lv_obj_t *roller)
{
    lv_obj_set_style_bg_color(roller, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(roller, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(roller, 1, 0);
    lv_obj_set_style_radius(roller, UI_RADIUS, 0);
    lv_obj_set_style_text_color(roller, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(roller, UI_FONT_BODY, 0);
    lv_obj_set_style_bg_color(roller, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(UI_COLOR_ON_ACCENT),
                                LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, UI_RADIUS_S, LV_PART_SELECTED);
}

static void range_style_calendar(lv_obj_t *cal)
{
    lv_obj_set_style_bg_color(cal, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(cal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cal, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(cal, 1, 0);
    lv_obj_set_style_radius(cal, UI_RADIUS_M, 0);
    lv_obj_set_style_text_color(cal, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(cal, UI_FONT_SMALL, 0);
    lv_obj_set_style_bg_opa(cal, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(cal, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(cal, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(cal, lv_color_hex(UI_COLOR_ON_ACCENT),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
}

static void range_open(void)
{
    if (s_range_scrim != NULL) {
        return;
    }

    time_t now = epoch_now();
    time_t end = (s_chart_end_epoch != 0) ? s_chart_end_epoch : now;
    time_t start = end - (time_t)s_chart_window;
    range_tm_from_time(0, start);
    range_tm_from_time(1, end);
    s_range_pick_stage = 0;

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
    s_range_scrim = scrim;

    lv_obj_t *box = ui_card_create(scrim, 0, 0, 760, 500);
    lv_obj_set_pos(box, 132, 40);

    ui_label_create(box, i18n_str(STR_HISTORY_RANGE), 24, 18, UI_COLOR_TEXT,
                    UI_FONT_BODY);

    /* 自带的日历头部在不同主题下不一定好点，这里加一组明确的月份切换 */
    lv_obj_t *prev = ui_button_create(box, "<", 24, 58, 36, 32, 0);
    lv_obj_t *next = ui_button_create(box, ">", 300, 58, 36, 32, 0);
    s_range_month_label = ui_label_create(box, "", 68, 62, UI_COLOR_TEXT,
                                          UI_FONT_BODY);
    lv_obj_set_width(s_range_month_label, 224);
    lv_obj_set_style_text_align(s_range_month_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(prev, range_month_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-1);
    lv_obj_add_event_cb(next, range_month_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);

    s_range_cal = lv_calendar_create(box);
    lv_obj_set_pos(s_range_cal, 24, 100);
    lv_obj_set_size(s_range_cal, 350, 300);
    range_style_calendar(s_range_cal);
    range_hide_calendar_header(s_range_cal);
    lv_obj_add_event_cb(lv_calendar_get_btnmatrix(s_range_cal),
                        range_cal_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    struct tm ntm;
    if (localtime_r(&now, &ntm) != NULL) {
        lv_calendar_set_today_date(s_range_cal, ntm.tm_year + 1900,
                                   ntm.tm_mon + 1, ntm.tm_mday);
    }
    range_set_month(s_range_tm[0].tm_year + 1900, s_range_tm[0].tm_mon + 1);

    ui_label_create(box, i18n_str(STR_HISTORY_RANGE_START), 400, 100,
                    UI_COLOR_ACCENT, UI_FONT_SMALL);
    ui_label_create(box, i18n_str(STR_UNIT_HOUR), 400, 124, UI_COLOR_MUTED,
                    UI_FONT_SMALL);
    s_range_start_hour = lv_roller_create(box);
    lv_obj_set_pos(s_range_start_hour, 400, 144);
    lv_obj_set_size(s_range_start_hour, 70, 90);
    lv_roller_set_options(s_range_start_hour, s_range_hour_opts,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_range_start_hour, 3);
    range_style_roller(s_range_start_hour);
    lv_obj_set_size(s_range_start_hour, 70, 90);

    ui_label_create(box, i18n_str(STR_UNIT_MIN), 478, 124, UI_COLOR_MUTED,
                    UI_FONT_SMALL);
    s_range_start_min = lv_roller_create(box);
    lv_obj_set_pos(s_range_start_min, 478, 144);
    lv_obj_set_size(s_range_start_min, 70, 90);
    lv_roller_set_options(s_range_start_min, s_range_min_opts,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_range_start_min, 3);
    range_style_roller(s_range_start_min);
    lv_obj_set_size(s_range_start_min, 70, 90);

    ui_label_create(box, i18n_str(STR_HISTORY_RANGE_END), 580, 100,
                    UI_COLOR_VIOLET, UI_FONT_SMALL);
    ui_label_create(box, i18n_str(STR_UNIT_HOUR), 580, 124, UI_COLOR_MUTED,
                    UI_FONT_SMALL);
    s_range_end_hour = lv_roller_create(box);
    lv_obj_set_pos(s_range_end_hour, 580, 144);
    lv_obj_set_size(s_range_end_hour, 70, 90);
    lv_roller_set_options(s_range_end_hour, s_range_hour_opts,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_range_end_hour, 3);
    range_style_roller(s_range_end_hour);
    lv_obj_set_size(s_range_end_hour, 70, 90);

    ui_label_create(box, i18n_str(STR_UNIT_MIN), 658, 124, UI_COLOR_MUTED,
                    UI_FONT_SMALL);
    s_range_end_min = lv_roller_create(box);
    lv_obj_set_pos(s_range_end_min, 658, 144);
    lv_obj_set_size(s_range_end_min, 70, 90);
    lv_roller_set_options(s_range_end_min, s_range_min_opts,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_range_end_min, 3);
    range_style_roller(s_range_end_min);
    lv_obj_set_size(s_range_end_min, 70, 90);

    lv_roller_set_selected(s_range_start_hour, s_range_tm[0].tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_range_start_min, s_range_tm[0].tm_min, LV_ANIM_OFF);
    lv_roller_set_selected(s_range_end_hour, s_range_tm[1].tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_range_end_min, s_range_tm[1].tm_min, LV_ANIM_OFF);

    s_range_start_label = ui_label_create(box, "", 400, 300, UI_COLOR_ACCENT,
                                          UI_FONT_SMALL);
    lv_obj_set_width(s_range_start_label, 336);
    s_range_end_label = ui_label_create(box, "", 400, 334, UI_COLOR_VIOLET,
                                        UI_FONT_SMALL);
    lv_obj_set_width(s_range_end_label, 336);

    lv_obj_add_event_cb(s_range_cal, range_date_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_range_start_hour, range_start_time_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_range_start_min, range_start_time_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_range_end_hour, range_end_time_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_range_end_min, range_end_time_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel = ui_button_create(box, i18n_str(STR_BTN_CANCEL), 24, 436,
                                        150, 44, 0);
    lv_obj_add_event_cb(cancel, range_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok = ui_button_create(box, i18n_str(STR_BTN_CONFIRM), 586, 436,
                                    150, 44, 1);
    lv_obj_add_event_cb(ok, range_confirm_cb, LV_EVENT_CLICKED, NULL);

    range_update_summary();
}

static void range_open_cb(lv_event_t *e)
{
    (void)e;
    range_open();
}

static void zoom_input_cb(lv_event_t *e)
{
    (void)e;
    input_open();
}

static lv_obj_t *grid_line(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    return line;
}

static void y_axis_build(lv_obj_t *win)
{
    for (int i = 0; i < 5; i++) {
        int y = PLOT_Y + i * PLOT_H / 4 - 10;
        lv_obj_t *label = ui_label_create(win, "", 20, y, UI_COLOR_MUTED,
                                          UI_FONT_SMALL);
        lv_obj_set_width(label, 52);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        s_y_label[i] = label;
    }
}

static void x_axis_build(lv_obj_t *win)
{
    for (int i = 0; i < 5; i++) {
        lv_obj_t *label = ui_label_create(win, "", 0, X_LABEL_Y,
                                          UI_COLOR_DISABLED, UI_FONT_SMALL);
        lv_obj_set_width(label, X_LABEL_W);
        lv_obj_set_style_text_align(label, i == 0 ? LV_TEXT_ALIGN_LEFT
                                    : (i == 4 ? LV_TEXT_ALIGN_RIGHT
                                       : LV_TEXT_ALIGN_CENTER), 0);
        int x = (i == 0) ? PLOT_X : (i == 4 ? PLOT_X + PLOT_W - X_LABEL_W
                                : PLOT_X + i * PLOT_W / 4 - X_LABEL_W / 2);
        lv_obj_set_pos(label, x, X_LABEL_Y);
        s_x_label[i] = label;
    }
}

static void grid_build(lv_obj_t *win)
{
    for (int i = 0; i < 5; i++) {
        int y = PLOT_Y + i * PLOT_H / 4;
        int x = PLOT_X + i * PLOT_W / 4;
        grid_line(win, PLOT_X, y, PLOT_W, 1,
                  i == 4 ? UI_COLOR_BORDER : UI_COLOR_BORDER_SOFT);
        grid_line(win, x, PLOT_Y, 1, PLOT_H,
                  i == 0 ? UI_COLOR_BORDER : UI_COLOR_BORDER_SOFT);
    }
}

static void chart_build(lv_obj_t *win)
{
    s_chart = lv_chart_create(win);
    lv_obj_set_pos(s_chart, PLOT_X, PLOT_Y);
    lv_obj_set_size(s_chart, PLOT_W, PLOT_H);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 0, 0);
    lv_obj_set_style_shadow_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_chart, true, LV_PART_ITEMS);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);

    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    s_ser = lv_chart_add_series(s_chart, lv_color_hex(s_series[s_selected].color),
                                LV_CHART_AXIS_PRIMARY_Y);
}

static void crosshair_build(lv_obj_t *win)
{
    s_cursor = lv_obj_create(win);
    lv_obj_set_pos(s_cursor, PLOT_X, PLOT_Y);
    lv_obj_set_size(s_cursor, 1, PLOT_H);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(s_cursor, 90, 0);
    lv_obj_set_style_border_width(s_cursor, 0, 0);
    lv_obj_set_style_radius(s_cursor, 0, 0);
    lv_obj_set_style_pad_all(s_cursor, 0, 0);
    lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

    s_cursor_dot = lv_obj_create(win);
    lv_obj_set_size(s_cursor_dot, 10, 10);
    lv_obj_set_style_radius(s_cursor_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_cursor_dot, lv_color_hex(s_series[s_selected].color), 0);
    lv_obj_set_style_bg_opa(s_cursor_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cursor_dot, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(s_cursor_dot, 2, 0);
    lv_obj_set_style_pad_all(s_cursor_dot, 0, 0);
    lv_obj_remove_flag(s_cursor_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_cursor_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cursor_dot, LV_OBJ_FLAG_HIDDEN);
}

static void dropdown_build(lv_obj_t *win, int x)
{
    s_dropdown = lv_dropdown_create(win);
    lv_obj_set_pos(s_dropdown, x, 8);
    lv_obj_set_size(s_dropdown, 180, 38);

    char options[64];
    snprintf(options, sizeof(options), "%s\n%s",
             i18n_str(STR_HISTORY_TEMP), i18n_str(STR_HISTORY_PRESSURE));
    lv_dropdown_set_options(s_dropdown, options);
    lv_dropdown_set_selected(s_dropdown, s_selected);

    lv_obj_set_style_bg_color(s_dropdown, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(s_dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_dropdown, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_dropdown, 1, 0);
    lv_obj_set_style_radius(s_dropdown, UI_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_dropdown, 14, 0);
    lv_obj_set_style_text_color(s_dropdown, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_dropdown, UI_FONT_SMALL, 0);
    lv_obj_set_style_shadow_width(s_dropdown, 0, 0);
    lv_obj_set_style_text_color(s_dropdown, lv_color_hex(UI_COLOR_MUTED),
                                LV_PART_INDICATOR);
    lv_obj_set_style_text_font(s_dropdown, UI_FONT_ICON_SMALL, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_dropdown, dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *list = lv_dropdown_get_list(s_dropdown);
    if (list != NULL) {
        lv_obj_set_style_bg_color(list, lv_color_hex(UI_COLOR_SURFACE_HI), 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(list, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(list, 1, 0);
        lv_obj_set_style_radius(list, UI_RADIUS_S, 0);
        lv_obj_set_style_text_font(list, UI_FONT_SMALL, LV_PART_SELECTED);
    }
}

static void zoom_build(lv_obj_t *win, int x)
{
    s_zoom_box = lv_obj_create(win);
    lv_obj_set_pos(s_zoom_box, x, 13);
    lv_obj_set_size(s_zoom_box, 190, 28);
    lv_obj_set_style_bg_color(s_zoom_box, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(s_zoom_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_zoom_box, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_zoom_box, 1, 0);
    lv_obj_set_style_radius(s_zoom_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_zoom_box, 0, 0);
    lv_obj_remove_flag(s_zoom_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_zoom_box, LV_OBJ_FLAG_CLICKABLE);

    s_zoom_minus = lv_obj_create(s_zoom_box);
    lv_obj_set_pos(s_zoom_minus, 0, 0);
    lv_obj_set_size(s_zoom_minus, 40, 28);
    lv_obj_set_style_bg_opa(s_zoom_minus, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zoom_minus, 0, 0);
    lv_obj_set_style_radius(s_zoom_minus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_zoom_minus, 0, 0);
    lv_obj_remove_flag(s_zoom_minus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_zoom_minus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_zoom_minus, lv_color_hex(UI_COLOR_ACCENT),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_zoom_minus, UI_OPA_SOFT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_zoom_minus, zoom_minus_cb, LV_EVENT_CLICKED, NULL);

    s_zoom_minus_icon = ui_label_create(s_zoom_minus, LV_SYMBOL_MINUS, 0, 0,
                                        UI_COLOR_TEXT, UI_FONT_ICON);
    lv_obj_center(s_zoom_minus_icon);

    s_zoom_input = lv_obj_create(s_zoom_box);
    lv_obj_set_pos(s_zoom_input, 40, 0);
    lv_obj_set_size(s_zoom_input, 110, 28);
    lv_obj_set_style_bg_opa(s_zoom_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zoom_input, 0, 0);
    lv_obj_set_style_radius(s_zoom_input, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_zoom_input, 0, 0);
    lv_obj_remove_flag(s_zoom_input, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_zoom_input, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_zoom_input, lv_color_hex(UI_COLOR_ACCENT),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_zoom_input, UI_OPA_SOFT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_zoom_input, zoom_input_cb, LV_EVENT_CLICKED, NULL);

    s_zoom_label = ui_label_create(s_zoom_input, "", 0, 0, UI_COLOR_MUTED,
                                   UI_FONT_SMALL);
    lv_obj_set_width(s_zoom_label, 110);
    lv_obj_set_style_text_align(s_zoom_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_zoom_label, LV_ALIGN_CENTER, 0, 0);

    s_zoom_plus = lv_obj_create(s_zoom_box);
    lv_obj_set_pos(s_zoom_plus, 150, 0);
    lv_obj_set_size(s_zoom_plus, 40, 28);
    lv_obj_set_style_bg_opa(s_zoom_plus, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_zoom_plus, 0, 0);
    lv_obj_set_style_radius(s_zoom_plus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_zoom_plus, 0, 0);
    lv_obj_remove_flag(s_zoom_plus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_zoom_plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_zoom_plus, lv_color_hex(UI_COLOR_ACCENT),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_zoom_plus, UI_OPA_SOFT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_zoom_plus, zoom_plus_cb, LV_EVENT_CLICKED, NULL);

    s_zoom_plus_icon = ui_label_create(s_zoom_plus, LV_SYMBOL_PLUS, 0, 0,
                                       UI_COLOR_TEXT, UI_FONT_ICON);
    lv_obj_center(s_zoom_plus_icon);
}

static void history_create(lv_obj_t *content)
{
    s_root = content;
    s_page_start_s = (float)(esp_timer_get_time() / 1000000.0);
    s_view_shift_s = 0;
    s_y_shift[0] = 0;
    s_y_shift[1] = 0;
    s_dragging = false;

    lv_obj_t *win = ui_panel_create(content, 0, 0, WIN_W, WIN_H);

    s_badge = ui_badge_create(win, 20, 11, 32, s_series[s_selected].symbol,
                              s_series[s_selected].color);
    ui_label_create(win, i18n_str(STR_NAV_HISTORY), 62, 13, UI_COLOR_TEXT,
                    UI_FONT_BODY);

    s_value = ui_label_create(win, "", 160, 7, s_series[s_selected].color,
                              UI_FONT_TITLE);
    lv_obj_set_width(s_value, 200);

    int right_x = WIN_W - 20 - 180;
    dropdown_build(win, right_x - 10 - 180);
    zoom_build(win, right_x - 10 - 180 - 20 - 190);

    lv_obj_t *range_btn = ui_button_create(win, i18n_str(STR_HISTORY_RANGE),
                                           right_x, 8, 180, 38, 0);
    lv_obj_add_event_cb(range_btn, range_open_cb, LV_EVENT_CLICKED, NULL);

    ui_divider_create(win, 20, DIVIDER_Y, WIN_W - 40);

    y_axis_build(win);
    x_axis_build(win);
    grid_build(win);
    chart_build(win);
    crosshair_build(win);

    s_readout = ui_label_create(win, "", PLOT_X + 10, PLOT_Y + 8,
                                s_series[s_selected].color, UI_FONT_SMALL);
    lv_obj_set_width(s_readout, 340);
    lv_obj_add_flag(s_readout, LV_OBJ_FLAG_HIDDEN);

    s_date_label = ui_label_create(win, "", PLOT_X + PLOT_W - 110, PLOT_Y + 8,
                                   UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_set_width(s_date_label, 100);
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_RIGHT, 0);

    /* 透明触摸层把“刻度区 + 绘图区”都包进来：从 Y 刻度、X 刻度或曲线上
     * 起手都可以拖动。它盖在最上面，所以图表本身不需要再接收事件。 */
    s_touch_area = lv_obj_create(win);
    lv_obj_set_pos(s_touch_area, 0, PLOT_Y - 10);
    lv_obj_set_size(s_touch_area, PLOT_X + PLOT_W,
                    X_LABEL_Y + 20 - (PLOT_Y - 10));
    lv_obj_set_style_bg_opa(s_touch_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_touch_area, 0, 0);
    lv_obj_set_style_pad_all(s_touch_area, 0, 0);
    lv_obj_remove_flag(s_touch_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touch_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_touch_area, chart_event_cb, LV_EVENT_ALL, NULL);

#if LV_USE_GESTURE_RECOGNITION
    /* 默认阈值 1.5 / 0.75 太钝，调成 1.1 / 0.9 更容易捏动 */
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_set_pinch_up_threshold(indev, 1.06f);
            lv_indev_set_pinch_down_threshold(indev, 0.94f);
            /* 两指捏合时手指连线也会旋转，旋转识别器会先抢到手势并
             * 把 pinch 重置掉；把旋转阈值调到实际不可能触发的高度。
             * 双指平移同理，调高距离阈值避免它抢在 pinch 前面。 */
            lv_indev_set_rotation_rad_threshold(indev, 100.0f);
            lv_indev_set_gesture_min_distance(indev, 250);
        }
        indev = lv_indev_get_next(indev);
    }
#endif

    apply_series();
    update_zoom_ui();
    s_timer = lv_timer_create(tick_cb, 1000, NULL);
    ESP_LOGI(TAG, "created, window=%ds series=%s",
             s_window_s, s_selected ? "pressure" : "temperature");
}

static void history_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    input_close();
    range_close();
    s_cursor_idx = -1;
    s_pinching = false;
    s_dragging = false;
    s_root = NULL;
    s_dropdown = NULL;
    s_badge = NULL;
    s_value = NULL;
    s_readout = NULL;
    s_date_label = NULL;
    s_zoom_box = NULL;
    s_zoom_minus = NULL;
    s_zoom_plus = NULL;
    s_zoom_minus_icon = NULL;
    s_zoom_plus_icon = NULL;
    s_zoom_input = NULL;
    s_zoom_label = NULL;
    for (int i = 0; i < 5; i++) {
        s_y_label[i] = NULL;
        s_x_label[i] = NULL;
    }
    s_chart = NULL;
    s_ser = NULL;
    s_cursor = NULL;
    s_cursor_dot = NULL;
    s_touch_area = NULL;
}

const ui_page_t page_history = {
    .id = "history",
    .name = STR_NAV_HISTORY,
    .symbol = LV_SYMBOL_BARS,
    .create = history_create,
    .destroy = history_destroy,
};
