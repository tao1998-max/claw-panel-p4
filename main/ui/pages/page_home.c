#include "page_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ai_ui.h"
#include "app_icon.h"
#include "app_registry.h"
#include "claw_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i18n.h"
#include "ui_widgets.h"

#define APPS_MAX_ITEMS 16

/* 手机式图标栅格：976 = 6×156 + 5×8，图标 96×96，下面名字 + 版本 */
#define GRID_COLS     6
#define TILE_W        156
#define TILE_H        146
#define TILE_GAP      8
#define ICON_SIZE     96
#define ICON_X        ((TILE_W - ICON_SIZE) / 2)
#define ICON_Y        6
#define NAME_Y        106
#define VERSION_Y     126

static const char *TAG = "ui.apps";

typedef struct {
    char id[APP_REGISTRY_ID_MAX_LEN + 1];
    char name[64];
    char entry[256];
    char args[512];
    char icon[192];
    char version[32];
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

/* Decoded icon descriptors owned by the current tile set. */
static lv_image_dsc_t *s_icons[APPS_MAX_ITEMS];
static size_t s_icon_count;

static void free_app_icons(void)
{
    for (size_t i = 0; i < s_icon_count; i++) {
        app_icon_free(s_icons[i]);
        s_icons[i] = NULL;
    }
    s_icon_count = 0;
}

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
    strlcpy(items[index].icon, entry->icon ? entry->icon : "", sizeof(items[index].icon));
    strlcpy(items[index].version, entry->version ? entry->version : "",
            sizeof(items[index].version));
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

static size_t utf8_first_char_len(const char *text)
{
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    return 4;
}

/* No icon file: draw a gradient tile with the App's first character. */
static void add_icon_fallback(lv_obj_t *tile, const app_item_t *item)
{
    lv_obj_t *art = ui_gradient_tile_create(tile, ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE,
                                            22, UI_COLOR_ACCENT_DEEP, UI_COLOR_VIOLET);
    if (!art) {
        return;
    }
    const lv_font_t *font = ai_ui_text_font();
    char initial[8] = {0};
    size_t len = item->name[0] ? utf8_first_char_len(item->name) : 0;
    if (len > 0 && len < sizeof(initial)) {
        memcpy(initial, item->name, len);
    }
    if (initial[0] && font) {
        lv_obj_t *label = ui_label_create(art, initial, 0, 0, UI_COLOR_TEXT, font);
        if (label) {
            lv_obj_center(label);
            return;
        }
    }
    lv_obj_t *label = ui_label_create(art, LV_SYMBOL_IMAGE, 0, 0,
                                      UI_COLOR_TEXT, UI_FONT_ICON);
    if (label) {
        lv_obj_center(label);
    }
}

static void add_app_tile(const app_item_t *item, int index)
{
    int col = index % GRID_COLS;
    int row = index / GRID_COLS;
    int x = col * (TILE_W + TILE_GAP);
    int y = row * (TILE_H + TILE_GAP);

    lv_obj_t *tile = lv_obj_create(s_list);
    if (!tile) {
        return;
    }
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    /* 按下反馈：一块很淡的高亮，和手机桌面一致 */
    lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_SURFACE_HI), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, 70, LV_STATE_PRESSED);

    lv_image_dsc_t *dsc = item->icon[0] ? app_icon_load(item->icon) : NULL;
    if (dsc && s_icon_count < APPS_MAX_ITEMS) {
        s_icons[s_icon_count++] = dsc;
        lv_obj_t *img = lv_image_create(tile);
        if (img) {
            lv_image_set_src(img, dsc);
            lv_obj_set_pos(img, ICON_X, ICON_Y);
        } else {
            app_icon_free(dsc);
            s_icons[--s_icon_count] = NULL;
            add_icon_fallback(tile, item);
        }
    } else {
        add_icon_fallback(tile, item);
    }

    /* App 名可能由 Agent 生成，用共享 puhui 字体而不是 i18n 子集 */
    const lv_font_t *name_font = ai_ui_text_font();
    lv_obj_t *name = ui_label_create(tile, item->name, 0, NAME_Y,
                                     UI_COLOR_TEXT,
                                     name_font != NULL ? name_font : UI_FONT_BODY);
    if (name) {
        lv_obj_set_width(name, TILE_W - 10);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    }

    if (item->version[0]) {
        lv_obj_t *version = ui_label_create(tile, item->version, 0, VERSION_Y,
                                            UI_COLOR_MUTED, UI_FONT_SMALL);
        if (version) {
            lv_obj_set_width(version, TILE_W - 10);
            lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(version, LV_LABEL_LONG_DOT);
        }
    }

    launch_ref_t *ref = calloc(1, sizeof(*ref));
    if (!ref) {
        return;
    }
    strlcpy(ref->name, item->name, sizeof(ref->name));
    strlcpy(ref->entry, item->entry, sizeof(ref->entry));
    strlcpy(ref->args, item->args, sizeof(ref->args));
    lv_obj_add_event_cb(tile, launch_clicked, LV_EVENT_CLICKED, ref);
    lv_obj_add_event_cb(tile, launch_ref_delete_cb, LV_EVENT_DELETE, ref);
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

    free_app_icons();
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
        add_app_tile(&items[i], (int)i);
    }
    ESP_LOGI(TAG, "home app grid rebuilt: %u item(s), %u icon(s)",
             (unsigned)count, (unsigned)s_icon_count);
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
    free_app_icons();
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
