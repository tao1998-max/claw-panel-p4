#include "page_sysinfo.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "claw_storage.h"
#include "hw.h"
#include "i18n.h"
#include "ui_widgets.h"

/* 设备状态页：四张 KPI 卡 + 模块状态网格。
 * 只读 g_hw[]，不做任何硬件初始化；数值靠页面自己的定时器刷新，
 * 页面销毁时把定时器一起删掉。 */

#define KPI_COUNT      4
#define KPI_W          232
#define KPI_H          112
#define KPI_GAP        16

#define PANEL_Y        128
#define PANEL_H        (UI_PAGE_H - PANEL_Y)          /* 292 */
#define GRID_Y         54
#define CELL_W         468
#define CELL_H         42
#define CELL_GAP_X     8
#define CELL_GAP_Y     4

static lv_obj_t *s_root;
static lv_obj_t *s_ready;
static lv_obj_t *s_ready_bar;
static lv_obj_t *s_heap;
static lv_obj_t *s_heap_bar;
static lv_obj_t *s_psram;
static lv_obj_t *s_psram_bar;
static lv_obj_t *s_uptime;
static lv_obj_t *s_fat;
static lv_obj_t *s_mod_badge[HW_MODULE_COUNT];
static lv_obj_t *s_mod_dot[HW_MODULE_COUNT];
static lv_obj_t *s_mod_value[HW_MODULE_COUNT];
static lv_timer_t *s_timer;

/* 与 hw_module_t 顺序一致的图标（图标必须来自 Montserrat 符号字体） */
static const char *const s_mod_symbol[HW_MODULE_COUNT] = {
    LV_SYMBOL_EYE_OPEN,     /* 显示 */
    LV_SYMBOL_EDIT,         /* 触摸 */
    LV_SYMBOL_VOLUME_MAX,   /* 音频 */
    LV_SYMBOL_SD_CARD,      /* SD 卡 */
    LV_SYMBOL_LIST,         /* RS485 */
    LV_SYMBOL_DRIVE,        /* 以太网 */
    LV_SYMBOL_IMAGE,        /* 摄像头 */
    LV_SYMBOL_WIFI,         /* Wi-Fi */
};

static uint32_t module_state_color(const hw_status_t *st, const char **text)
{
    if (!st->enabled) {
        *text = i18n_str(STR_ST_OFF);
        return UI_COLOR_DISABLED;
    }
    if (st->ok) {
        *text = i18n_str(STR_ST_OK);
        return UI_COLOR_OK;
    }
    *text = i18n_str(STR_ST_FAIL);
    return UI_COLOR_ERROR;
}

static void format_storage(char *out, size_t out_size,
                           uint64_t total, uint64_t used)
{
    if (total == 0) {
        strlcpy(out, "--", out_size);
    } else if (total >= 1024 * 1024) {
        snprintf(out, out_size, "%.2f / %.2f MB",
                 (double)used / (1024.0 * 1024.0),
                 (double)total / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_size, "%llu / %llu KB",
                 (unsigned long long)(used / 1024),
                 (unsigned long long)(total / 1024));
    }
}

static void refresh_values(void)
{
    /* 页面被隐藏时不做无谓的文本重排 */
    if (s_root != NULL && !lv_obj_is_visible(s_root)) {
        return;
    }

    int ready = 0;
    for (int i = 0; i < HW_MODULE_COUNT; i++) {
        const char *text;
        uint32_t color = module_state_color(&g_hw[i], &text);
        if (g_hw[i].enabled && g_hw[i].ok) {
            ready++;
        }

        ui_badge_set_color(s_mod_badge[i], color);
        ui_dot_set_color(s_mod_dot[i], color);
        if (s_mod_value[i] != NULL) {
            lv_label_set_text(s_mod_value[i], text);
            ui_label_set_text_color(s_mod_value[i], color);
        }
    }

    bool all_ok = (ready == HW_MODULE_COUNT);
    if (s_ready != NULL) {
        lv_label_set_text_fmt(s_ready, "%d / %d", ready, HW_MODULE_COUNT);
        ui_label_set_text_color(s_ready, all_ok ? UI_COLOR_OK : UI_COLOR_WARN);
    }
    ui_progress_set_value(s_ready_bar, ready * 1000 / HW_MODULE_COUNT);

    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_heap != NULL) {
        lv_label_set_text_fmt(s_heap, "%lu KB", (unsigned long)(heap_free / 1024));
    }
    ui_progress_set_value(s_heap_bar,
                          heap_total > 0 ? (int)((uint64_t)heap_free * 1000 / heap_total) : 0);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_psram != NULL) {
        uint32_t tenths = (uint32_t)((uint64_t)psram_free * 10 / (1024 * 1024));
        lv_label_set_text_fmt(s_psram, "%lu.%lu MB",
                              (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    }
    ui_progress_set_value(s_psram_bar,
                          psram_total > 0 ? (int)((uint64_t)psram_free * 1000 / psram_total) : 0);

    if (s_uptime != NULL) {
        uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
        lv_label_set_text_fmt(s_uptime, "%02lu:%02lu:%02lu",
                              (unsigned long)(sec / 3600),
                              (unsigned long)((sec / 60) % 60),
                              (unsigned long)(sec % 60));
    }

    char usage[64];
    uint64_t fat_total = 0, fat_free = 0;
    if (esp_vfs_fat_info(CLAW_STORAGE_MOUNT_POINT, &fat_total, &fat_free) == ESP_OK) {
        format_storage(usage, sizeof(usage), fat_total,
                       fat_total > fat_free ? fat_total - fat_free : 0);
    } else {
        strlcpy(usage, "--", sizeof(usage));
    }
    if (s_fat != NULL) {
        lv_label_set_text(s_fat, usage);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_values();
}

/* KPI 卡：右上角图标 + 小标题 + 大数值 + 底部细进度条（或一行小状态） */
static lv_obj_t *metric_card(lv_obj_t *parent, int x, const char *symbol,
                             uint32_t accent, i18n_id_t caption,
                             lv_obj_t **out_bar, uint32_t bar_color, bool uptime_card)
{
    lv_obj_t *card = ui_card_create(parent, x, 0, KPI_W, KPI_H);
    ui_badge_create(card, KPI_W - 52, 16, 34, symbol, accent);

    ui_label_create(card, i18n_str(caption), 20, 16, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_t *value = ui_label_create(card, "-", 20, 40, UI_COLOR_TEXT, UI_FONT_TITLE);

    if (uptime_card) {
        lv_obj_t *dot = ui_dot_create(card, 20, 98, 6);
        ui_dot_set_color(dot, accent);
        ui_label_create(card, "P4 + C6", 32, 90, UI_COLOR_DISABLED, UI_FONT_SMALL);
    } else if (out_bar != NULL) {
        *out_bar = ui_progress_create(card, 20, 96, KPI_W - 40, 4, 0, bar_color);
    }
    return value;
}

/* 图例：小圆点 + 状态词 */
static void legend_item(lv_obj_t *parent, uint32_t color, i18n_id_t id)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 5, 0);

    lv_obj_t *dot = ui_dot_create(item, 0, 0, 6);
    ui_dot_set_color(dot, color);
    ui_label_create(item, i18n_str(id), 0, 0, UI_COLOR_DISABLED, UI_FONT_SMALL);
}

/* 模块格子：图标 + 名称 + 状态点 + 状态词 */
static void module_cells(lv_obj_t *parent)
{
    for (int i = 0; i < HW_MODULE_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t *cell = ui_card_create(parent,
                                        col * (CELL_W + CELL_GAP_X),
                                        row * (CELL_H + CELL_GAP_Y),
                                        CELL_W, CELL_H);
        lv_obj_set_style_radius(cell, 12, 0);

        const char *text;
        uint32_t color = module_state_color(&g_hw[i], &text);
        s_mod_badge[i] = ui_badge_create(cell, 12, 9, 28, s_mod_symbol[i], color);

        ui_label_create(cell, i18n_str((i18n_id_t)(STR_MOD_DISPLAY + i)),
                        52, (CELL_H - 20) / 2, UI_COLOR_TEXT, UI_FONT_BODY);

        s_mod_dot[i] = ui_dot_create(cell, CELL_W - 98, (CELL_H - 8) / 2, 8);
        ui_dot_set_color(s_mod_dot[i], color);

        s_mod_value[i] = ui_label_create(cell, text, CELL_W - 84,
                                         (CELL_H - 16) / 2, color, UI_FONT_SMALL);
    }
}

static void sysinfo_create(lv_obj_t *content)
{
    s_root = content;

    /* ---- 四张 KPI 卡 ---- */
    s_ready = metric_card(content, 0, LV_SYMBOL_OK, UI_COLOR_ACCENT,
                          STR_HOME_MODULES, &s_ready_bar, UI_COLOR_ACCENT, false);
    s_heap = metric_card(content, KPI_W + KPI_GAP, LV_SYMBOL_DRIVE, UI_COLOR_CYAN,
                         STR_SYS_HEAP, &s_heap_bar, UI_COLOR_CYAN, false);
    s_psram = metric_card(content, (KPI_W + KPI_GAP) * 2, LV_SYMBOL_SD_CARD,
                          UI_COLOR_OK, STR_SYS_PSRAM, &s_psram_bar, UI_COLOR_OK, false);
    s_uptime = metric_card(content, (KPI_W + KPI_GAP) * 3, LV_SYMBOL_REFRESH,
                           UI_COLOR_VIOLET, STR_SYS_UPTIME, NULL, 0, true);

    /* ---- 模块状态面板 ---- */
    lv_obj_t *panel = ui_panel_create(content, 0, PANEL_Y, UI_PAGE_W, PANEL_H);
    ui_label_create(panel, i18n_str(STR_SYS_MODULES), 20, 16, UI_COLOR_TEXT,
                    UI_FONT_BODY);

    lv_obj_t *legend = lv_obj_create(panel);
    lv_obj_set_size(legend, LV_SIZE_CONTENT, 16);
    lv_obj_align(legend, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend, 0, 0);
    lv_obj_set_style_pad_all(legend, 0, 0);
    lv_obj_remove_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 14, 0);

    legend_item(legend, UI_COLOR_OK, STR_ST_OK);
    legend_item(legend, UI_COLOR_ERROR, STR_ST_FAIL);
    legend_item(legend, UI_COLOR_DISABLED, STR_ST_OFF);

    lv_obj_t *grid = lv_obj_create(panel);
    lv_obj_set_pos(grid, 16, GRID_Y);
    lv_obj_set_size(grid, UI_PAGE_W - 32, CELL_H * 4 + CELL_GAP_Y * 3);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    module_cells(grid);

    lv_obj_t *storage = lv_obj_create(panel);
    lv_obj_set_pos(storage, 20, 240);
    lv_obj_set_size(storage, UI_PAGE_W - 40, 22);
    lv_obj_set_style_bg_opa(storage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(storage, 0, 0);
    lv_obj_set_style_pad_all(storage, 0, 0);
    lv_obj_remove_flag(storage, LV_OBJ_FLAG_SCROLLABLE);
    ui_label_create(storage, i18n_str(STR_SYS_STORAGE), 0, 3,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    ui_label_create(storage, "FAT", 100, 3, UI_COLOR_DISABLED, UI_FONT_SMALL);
    s_fat = ui_label_create(storage, "-", 140, 3, UI_COLOR_TEXT, UI_FONT_SMALL);

    char info[128];
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(info, sizeof(info),
             "%s v%s   |   JC1060P470C_I_W_Y   |   %s %02X:%02X:%02X:%02X:%02X:%02X",
             i18n_str(STR_SYS_FIRMWARE), esp_app_get_description()->version,
             i18n_str(STR_SYS_MAC),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ui_label_create(panel, info, 20, PANEL_H - 26, UI_COLOR_DISABLED, UI_FONT_SMALL);

    refresh_values();
    s_timer = lv_timer_create(refresh_timer_cb, 500, NULL);
}

static void sysinfo_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_root = NULL;
    s_ready = s_heap = s_psram = s_uptime = NULL;
    s_fat = NULL;
    s_ready_bar = s_heap_bar = s_psram_bar = NULL;
    for (int i = 0; i < HW_MODULE_COUNT; i++) {
        s_mod_badge[i] = NULL;
        s_mod_dot[i] = NULL;
        s_mod_value[i] = NULL;
    }
}

const ui_page_t page_sysinfo = {
    .id = "sysinfo",
    .name = STR_NAV_SYSINFO,
    .symbol = LV_SYMBOL_LIST,
    .create = sysinfo_create,
    .destroy = sysinfo_destroy,
};
