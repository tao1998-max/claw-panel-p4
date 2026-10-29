#include "page_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ai_ui.h"
#include "app_registry.h"
#include "claw_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i18n.h"
#include "ui_widgets.h"

#define APPS_MAX_ITEMS   16

/* 卡片栅格：976 页宽 = 16 边距 + 464 卡宽 + 16 列间距 + 464 卡宽 + 16 边距 */
#define CARD_W        464
#define CARD_H        96
#define CARD_ROW_GAP  12
#define CARD_COL_GAP  16
#define CARD_SIDE     16

/* 图标色块按索引循环的四色强调色 */
static const uint32_t s_tile_palette[] = {
    UI_COLOR_ACCENT,
    UI_COLOR_VIOLET,
    UI_COLOR_CYAN,
    UI_COLOR_OK,
};
#define TILE_PALETTE_COUNT (sizeof(s_tile_palette) / sizeof(s_tile_palette[0]))

static const char *TAG = "ui.apps";

typedef struct {
    char id[APP_REGISTRY_ID_MAX_LEN + 1];
    char name[64];
    char entry[256];
    char args[512];
} app_item_t;

typedef struct {
    char name[64];
    char entry[256];
    char args[512];
} launch_ref_t;

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_empty;
static lv_obj_t *s_status;
static lv_timer_t *s_timer;
static volatile bool s_dirty;
static bool s_listener_registered;

static esp_err_t collect_item(const app_registry_entry_t *entry, void *user_ctx)
{
    app_item_t *items = user_ctx;
    if (!entry || !entry->app_id || !entry->app_id[0]) {
        return ESP_OK;
    }
    size_t index = 0;
    while (index < APPS_MAX_ITEMS && items[index].id[0]) {
        index++;
    }
    if (index >= APPS_MAX_ITEMS) {
        return ESP_OK;
    }
    strlcpy(items[index].id, entry->app_id, sizeof(items[index].id));
    strlcpy(items[index].name,
            entry->display_name && entry->display_name[0] ? entry->display_name : entry->app_id,
            sizeof(items[index].name));
    strlcpy(items[index].entry, entry->entry ? entry->entry : "", sizeof(items[index].entry));
    strlcpy(items[index].args, entry->args_json ? entry->args_json : "{}",
            sizeof(items[index].args));
    return ESP_OK;
}

static size_t collect_apps(app_item_t *items)
{
    memset(items, 0, sizeof(app_item_t) * APPS_MAX_ITEMS);
    (void)app_registry_foreach(collect_item, items);
    size_t count = 0;
    for (size_t i = 0; i < APPS_MAX_ITEMS; i++) {
        if (items[i].id[0]) {
            count++;
        }
    }
    return count;
}

static void apps_changed_cb(void *user_ctx)
{
    (void)user_ctx;
    s_dirty = true;
}

/* The home page is created before claw_port_init(), so the registry does not
 * exist yet on the first call. Retry from the 1 s timer instead of marking the
 * listener as registered on failure. */
static bool register_listener_if_needed(void)
{
    if (s_listener_registered) {
        return true;
    }
    if (app_registry_register_changed_cb(apps_changed_cb, NULL) != ESP_OK) {
        return false;
    }
    s_listener_registered = true;
    s_dirty = true;
    ESP_LOGI(TAG, "app registry listener registered");
    return true;
}

static void launch_ref_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static void launch_clicked(lv_event_t *e)
{
    launch_ref_t *ref = lv_event_get_user_data(e);
    if (!ref || !ref->entry[0]) {
        return;
    }
    char out[512] = {0};
    esp_err_t err = claw_api_start_task(ref->name, NULL, ref->entry, ref->args,
                                        0, out, sizeof(out));
    if (s_status) {
        if (err == ESP_OK) {
            lv_label_set_text(s_status, i18n_str(STR_APPS_LAUNCH));
            lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_OK), 0);
        } else {
            lv_label_set_text_fmt(s_status, "%s: %.120s",
                                  i18n_str(STR_APPS_FAILED),
                                  out[0] ? out : esp_err_to_name(err));
            lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_ERROR), 0);
        }
    }
    ESP_LOGI(TAG, "launch %s path=%s -> %s (%s)", ref->name, ref->entry,
             esp_err_to_name(err), out);
}

static void add_app_card(const app_item_t *item, int index)
{
    int col = index % 2;
    int row = index / 2;
    int x = CARD_SIDE + col * (CARD_W + CARD_COL_GAP);
    int y = row * (CARD_H + CARD_ROW_GAP);

    lv_obj_t *card = ui_card_create(s_list, x, y, CARD_W, CARD_H);
    if (!card) {
        return;
    }
    /* 整卡可点：按下时提亮一档 + 强调色描边，视觉反馈比单独按钮强 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_SURFACE_HI), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);

    /* 左侧图标色块，颜色按索引在强调色板里轮换 */
    uint32_t tile_color = s_tile_palette[index % TILE_PALETTE_COUNT];
    (void)ui_badge_create(card, 16, (CARD_H - 56) / 2, 56, LV_SYMBOL_IMAGE, tile_color);

    /* App 名可能由 Agent 生成，用共享 puhui 字体而不是 i18n 子集 */
    const lv_font_t *name_font = ai_ui_text_font();
    lv_obj_t *name = ui_label_create(card, item->name, 88, 24,
                                     UI_COLOR_TEXT,
                                     name_font != NULL ? name_font : UI_FONT_BODY);
    if (name) {
        lv_obj_set_width(name, 248);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    }
    lv_obj_t *path = ui_label_create(card, item->entry, 88, 52,
                                     UI_COLOR_MUTED, UI_FONT_SMALL);
    if (path) {
        lv_obj_set_width(path, 248);
        lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    }

    /* 右侧「启动」胶囊是静态提示，点击由整卡统一响应 */
    lv_obj_t *chip = ui_chip_create(card, i18n_str(STR_APPS_LAUNCH),
                                    UI_COLOR_ACCENT, NULL);
    if (chip) {
        lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    launch_ref_t *ref = calloc(1, sizeof(*ref));
    if (!ref) {
        return;
    }
    strlcpy(ref->name, item->name, sizeof(ref->name));
    strlcpy(ref->entry, item->entry, sizeof(ref->entry));
    strlcpy(ref->args, item->args, sizeof(ref->args));
    lv_obj_add_event_cb(card, launch_clicked, LV_EVENT_CLICKED, ref);
    lv_obj_add_event_cb(card, launch_ref_delete_cb, LV_EVENT_DELETE, ref);
}

static void rebuild(void)
{
    if (!s_list) {
        return;
    }
    app_item_t *items = heap_caps_calloc(APPS_MAX_ITEMS, sizeof(*items),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) {
        return;
    }
    size_t count = collect_apps(items);

    lv_obj_clean(s_list);
    if (count == 0) {
        if (s_empty) {
            lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        }
        heap_caps_free(items);
        return;
    }
    if (s_empty) {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < count && i < APPS_MAX_ITEMS; i++) {
        add_app_card(&items[i], (int)i);
    }
    ESP_LOGI(TAG, "home app list rebuilt: %u item(s)", (unsigned)count);
    heap_caps_free(items);
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_root || !lv_obj_is_visible(s_root)) {
        return;
    }
    if (!register_listener_if_needed()) {
        return;
    }
    if (!s_dirty) {
        return;
    }
    s_dirty = false;
    rebuild();
}

static void page_home_create(lv_obj_t *content)
{
    s_root = lv_obj_create(content);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, UI_PAGE_W, UI_PAGE_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);

    s_status = ui_label_create(s_root, "", 0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
    if (s_status) {
        lv_obj_set_width(s_status, UI_PAGE_W);
        lv_obj_set_pos(s_status, 0, 0);
    }

    s_list = lv_obj_create(s_root);
    lv_obj_set_pos(s_list, 0, 30);
    lv_obj_set_size(s_list, UI_PAGE_W, UI_PAGE_H - 30);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_empty = ui_label_create(s_root, i18n_str(STR_APPS_EMPTY), 20, 60,
                              UI_COLOR_MUTED, UI_FONT_BODY);
    if (s_empty) {
        lv_obj_set_width(s_empty, UI_PAGE_W - 40);
        lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    }

    s_dirty = true;
    (void)register_listener_if_needed();
    rebuild();
    s_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
}

static void page_home_destroy(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_root = NULL;
    s_list = NULL;
    s_empty = NULL;
    s_status = NULL;
    s_dirty = false;
}

const ui_page_t page_home = {
    .id = "home",
    .name = STR_NAV_HOME,
    .symbol = LV_SYMBOL_DIRECTORY,
    .create = page_home_create,
    .destroy = page_home_destroy,
};

/* The legacy home dashboard owned the demo switch. Apps are now the home
 * page; keep the symbol so the AI demo event remains harmless. */
void page_home_apply_demo(bool on)
{
    (void)on;
}
