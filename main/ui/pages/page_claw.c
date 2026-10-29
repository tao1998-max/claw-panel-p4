/*
 * Claw page: task list, workspace scripts and the AI-Lua safety switch.
 *
 * Everything runs in the LVGL task. claw_api_* is thread safe and never calls
 * back into LVGL, so the page polls it on a 1 s timer.
 */
#include "page_claw.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "cap_lua.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hw.h"
#include "sdkconfig.h"
#include "settings.h"

#include "ai_ui.h"
#include "claw_agent_mgr.h"
#include "claw_api.h"
#include "claw_cap.h"
#include "claw_memory.h"
#include "claw_port.h"
#include "claw_session_mgr.h"
#include "claw_settings.h"
#include "claw_storage.h"
#include "claw_tasks.h"
#include "i18n.h"
#include "ui_shell.h"
#include "ui_widgets.h"

static const char *TAG = "ui.claw";

#define CLAW_UI_SELFTEST 0

/* cap_lua exposes these only in its internal header. */
size_t cap_lua_get_module_count(void);
const cap_lua_module_t *cap_lua_get_module(size_t index);

/* The device page mirrors the ESP-Claw Web console's top-level navigation:
 * status, settings (basic/LLM/IM/network in one scroll), memory, chat,
 * capabilities, Lua modules and files. Records/schedules are managed by the
 * Agent and the Web UI, so the old task/schedule/script buttons are gone. */
#define CLAW_TAB_STATUS 0
#define CLAW_TAB_SETTINGS 1
#define CLAW_TAB_MEMORY 2
#if CONFIG_TEMPLATE_CLAW_AGENT
#define CLAW_TAB_CHAT 3
#define CLAW_TAB_SCHEDULES 4
#define CLAW_TAB_CAPS 5
#define CLAW_TAB_LUA 6
#define CLAW_TAB_FILES 7
#define CLAW_TAB_COUNT 8
#else
#define CLAW_TAB_SCHEDULES 3
#define CLAW_TAB_CAPS 4
#define CLAW_TAB_LUA 5
#define CLAW_TAB_FILES 6
#define CLAW_TAB_COUNT 7
#endif

#define TASKS_SNAPSHOT_MAX 4096
#define SCHEDULES_JSON_MAX 16384
#define SCRIPT_VIEW_MAX (64 * 1024)
#define AGENT_STATUS_MAX 768
#define AGENT_REPLY_MAX 2048

/* Left rail + web-UI-like main column: status bar, page header, content. */
#define RAIL_W 200
#define RAIL_GAP 16
#define MAIN_X (RAIL_W + RAIL_GAP)
#define CONTENT_W (UI_PAGE_W - MAIN_X)      /* 760 */
#define ACTIONS_W 320
#define BODY_Y 104
#define BODY_H (UI_PAGE_H - BODY_Y)

static lv_obj_t *s_root;
static lv_obj_t *s_rail;
static lv_obj_t *s_nav_list;
static lv_obj_t *s_main;
static lv_obj_t *s_page_title;
static lv_obj_t *s_page_desc;
static lv_obj_t *s_page_actions;
static lv_obj_t *s_body;
static lv_obj_t *s_chip_lua;
static lv_obj_t *s_chip_lua_label;
static lv_obj_t *s_chip_jobs;
static lv_obj_t *s_chip_jobs_label;
static lv_obj_t *s_chip_store;
static lv_obj_t *s_chip_store_label;
#if CONFIG_TEMPLATE_CLAW_AGENT
static lv_obj_t *s_chip_agent;
static lv_obj_t *s_chip_agent_label;
#endif
static lv_timer_t *s_timer;
static lv_obj_t *s_lua_switch;
static int s_tab;
static bool s_claw_ready_seen;

typedef struct {
    int tab;
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *label;
} claw_nav_t;
static claw_nav_t s_nav[CLAW_TAB_COUNT];
static int s_nav_count;
static char *s_script_view_buf;
static char s_pending_schedule[64];
static char s_sched_msg[96];
static bool s_sched_msg_error;

#if CONFIG_TEMPLATE_CLAW_AGENT
static uint32_t s_agent_seen_seq;
static bool s_agent_seen_busy;
static bool s_agent_seen_worker;
static bool s_agent_cfg_error;
static bool s_agent_ui_enabled;
static char s_agent_ui_error[160];
static lv_obj_t *s_agent_chip;
static lv_obj_t *s_agent_chip_label;
static lv_obj_t *s_agent_error_label;
static lv_obj_t *s_agent_reply_label;
static lv_obj_t *s_agent_stop_btn;
static char s_agent_current_task_id[16];
static int s_agent_chip_state = -1;
static bool s_agent_error_visible;
#endif

static void body_rebuild(void);
static void set_tab(int tab);
static void label_set_if_changed(lv_obj_t *label, const char *text);
static const lv_font_t *claw_dyn_font(const lv_font_t *fallback);

/* ---------- helpers ---------- */

static const lv_font_t *claw_dyn_font(const lv_font_t *fallback)
{
    const lv_font_t *font = ai_ui_text_font();
    return font != NULL ? font : fallback;
}

static uint32_t state_color(const char *state)
{
    if (state == NULL) return UI_COLOR_DISABLED;
    if (strcmp(state, "running") == 0 || strcmp(state, "queued") == 0) return UI_COLOR_ACCENT;
    if (strcmp(state, "done") == 0) return UI_COLOR_OK;
    if (strcmp(state, "failed") == 0) return UI_COLOR_ERROR;
    if (strcmp(state, "timeout") == 0) return UI_COLOR_WARN;
    return UI_COLOR_MUTED;
}

static uint32_t origin_color(const char *origin)
{
    if (origin == NULL) return UI_COLOR_MUTED;
    if (strcmp(origin, "ai") == 0) return UI_COLOR_VIOLET;
    if (strcmp(origin, "scheduled") == 0) return UI_COLOR_CYAN;
    if (strcmp(origin, "agent") == 0) return UI_COLOR_WARN;
    return UI_COLOR_ACCENT_SOFT;
}

static void free_user_data_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* Call an ESP-Claw capability as the system (not the LLM): the same
 * cap_files/cap_scheduler implementation the agent uses. */
static esp_err_t ui_cap_call(const char *name, const char *input,
                             char *out, size_t out_size)
{
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .source_cap = "ui.claw",
    };
    return claw_cap_call(name, input != NULL ? input : "{}", &ctx, out, out_size);
}

/* ---------- status chips ---------- */

static void status_refresh(void)
{
    if (s_root == NULL) return;

    char text[512] = {0};
    esp_err_t err = claw_api_status_json(text, sizeof(text));
    cJSON *root = (err == ESP_OK) ? cJSON_Parse(text) : NULL;

    bool lua = false;
    int jobs = 0;
    int free_kb = 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    bool agent_enabled = false;
    bool agent_configured = false;
    bool agent_ready = false;
    bool agent_busy = false;
#endif
    if (root != NULL) {
        cJSON *lua_item = cJSON_GetObjectItem(root, "lua_ai");
        cJSON *jobs_item = cJSON_GetObjectItem(root, "async_jobs");
        cJSON *store_item = cJSON_GetObjectItem(root, "storage_free_kb");
        lua = cJSON_IsTrue(lua_item);
        jobs = cJSON_IsNumber(jobs_item) ? jobs_item->valueint : 0;
        free_kb = cJSON_IsNumber(store_item) ? store_item->valueint : 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
        agent_enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent"));
        agent_configured = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_configured"));
        agent_ready = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_ready"));
        agent_busy = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_busy"));
#endif
        cJSON_Delete(root);
    }

    if (s_chip_lua_label != NULL) {
        label_set_if_changed(s_chip_lua_label,
                             i18n_str(lua ? STR_CLAW_CLOUD_ON : STR_CLAW_CLOUD_OFF));
    }

#if CONFIG_TEMPLATE_CLAW_AGENT
    if (s_chip_agent_label != NULL) {
        const i18n_id_t agent_text = !agent_enabled ? STR_CLAW_AGENT_OFF
                                     : !agent_configured ? STR_CLAW_AGENT_UNCONFIGURED
                                     : agent_busy ? STR_CLAW_AGENT_BUSY
                                     : agent_ready ? STR_CLAW_AGENT_READY
                                     : STR_CLAW_AGENT_ON;
        label_set_if_changed(s_chip_agent_label, i18n_str(agent_text));
    }
#endif

    char buf[48];
    snprintf(buf, sizeof(buf), "%s %d", i18n_str(STR_CLAW_JOBS), jobs);
    if (s_chip_jobs_label != NULL) {
        label_set_if_changed(s_chip_jobs_label, buf);
    }

    snprintf(buf, sizeof(buf), "%d KB", free_kb);
    if (s_chip_store_label != NULL) {
        label_set_if_changed(s_chip_store_label, buf);
    }
}

/* ---------- tasks ---------- */

static void stop_clicked(lv_event_t *e)
{
    const char *job_id = lv_event_get_user_data(e);
    if (job_id == NULL) return;
    char out[256] = {0};
    esp_err_t err = claw_api_stop_task(job_id, 1000, out, sizeof(out));
    ESP_LOGI(TAG, "stop %s -> %s %s", job_id, esp_err_to_name(err), out);
    status_refresh();
}

/* ---------- rail / page header ---------- */

typedef struct {
    int tab;
    i18n_id_t label;
    const char *symbol;
} claw_nav_def_t;

static const claw_nav_def_t s_nav_defs[] = {
    { CLAW_TAB_STATUS, STR_CLAW_NAV_STATUS, LV_SYMBOL_LIST },
    { CLAW_TAB_SETTINGS, STR_CLAW_TAB_SETTINGS, LV_SYMBOL_SETTINGS },
    { CLAW_TAB_MEMORY, STR_CLAW_NAV_MEMORY, LV_SYMBOL_SAVE },
#if CONFIG_TEMPLATE_CLAW_AGENT
    { CLAW_TAB_CHAT, STR_CLAW_NAV_CHAT, LV_SYMBOL_EDIT },
#endif
    { CLAW_TAB_SCHEDULES, STR_CLAW_TAB_SCHEDULES, LV_SYMBOL_REFRESH },
    { CLAW_TAB_CAPS, STR_CLAW_NAV_CAPS, LV_SYMBOL_DRIVE },
    { CLAW_TAB_LUA, STR_CLAW_NAV_LUA, LV_SYMBOL_FILE },
    { CLAW_TAB_FILES, STR_CLAW_NAV_FILES, LV_SYMBOL_DIRECTORY },
};

static void nav_clicked(lv_event_t *e)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    set_tab(tab);
}

static void nav_update(void)
{
    for (int i = 0; i < s_nav_count; i++) {
        bool on = s_nav[i].tab == s_tab;
        lv_obj_set_style_bg_color(s_nav[i].row, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(s_nav[i].row, on ? 36 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_nav[i].row, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(s_nav[i].row, on ? 1 : 0, 0);
        lv_obj_set_style_border_opa(s_nav[i].row, on ? 55 : 0, 0);
        ui_label_set_text_color(s_nav[i].icon,
                                on ? UI_COLOR_ACCENT : UI_COLOR_MUTED);
        ui_label_set_text_color(s_nav[i].label,
                                on ? UI_COLOR_TEXT : UI_COLOR_MUTED);
    }
}

static void header_update(void)
{
    i18n_id_t title = STR_CLAW_NAV_STATUS;
    i18n_id_t desc = STR_CLAW_DESC_STATUS;
    switch (s_tab) {
    case CLAW_TAB_STATUS:
        break;
    case CLAW_TAB_SETTINGS:
        title = STR_CLAW_TAB_SETTINGS;
        desc = STR_CLAW_DESC_SETTINGS;
        break;
    case CLAW_TAB_MEMORY:
        title = STR_CLAW_NAV_MEMORY;
        desc = STR_CLAW_DESC_MEMORY;
        break;
#if CONFIG_TEMPLATE_CLAW_AGENT
    case CLAW_TAB_CHAT:
        title = STR_CLAW_NAV_CHAT;
        desc = STR_CLAW_DESC_AGENT;
        break;
#endif
    case CLAW_TAB_SCHEDULES:
        title = STR_CLAW_TAB_SCHEDULES;
        desc = STR_CLAW_DESC_SCHEDULES;
        break;
    case CLAW_TAB_CAPS:
        title = STR_CLAW_NAV_CAPS;
        desc = STR_CLAW_DESC_CAPS;
        break;
    case CLAW_TAB_LUA:
        title = STR_CLAW_NAV_LUA;
        desc = STR_CLAW_DESC_LUA;
        break;
    case CLAW_TAB_FILES:
        title = STR_CLAW_NAV_FILES;
        desc = STR_CLAW_DESC_FILES;
        break;
    default:
        break;
    }
    label_set_if_changed(s_page_title, i18n_str(title));
    label_set_if_changed(s_page_desc, i18n_str(desc));
}

static void set_tab(int tab)
{
    if (tab < 0 || tab >= CLAW_TAB_COUNT) {
        return;
    }
    s_tab = tab;
    nav_update();
    header_update();
    body_rebuild();
}

static void nav_build(void)
{
    s_rail = ui_panel_create(s_root, 0, 0, RAIL_W, UI_PAGE_H);
    ui_label_create(s_rail, "Claw", 20, 16, UI_COLOR_TEXT, UI_FONT_TITLE);
    ui_label_create(s_rail, i18n_str(STR_CLAW_MANAGE), 20, 50,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    ui_divider_create(s_rail, 20, 78, RAIL_W - 40);

    /* The rail is shorter than a full page (420 px); keep the items in a
     * scrollable list so the last entry stays reachable. */
    s_nav_list = lv_obj_create(s_rail);
    lv_obj_set_pos(s_nav_list, 0, 94);
    lv_obj_set_size(s_nav_list, RAIL_W, UI_PAGE_H - 94);
    lv_obj_set_style_bg_opa(s_nav_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_nav_list, 0, 0);
    lv_obj_set_style_radius(s_nav_list, 0, 0);
    lv_obj_set_style_pad_all(s_nav_list, 0, 0);
    lv_obj_set_scroll_dir(s_nav_list, LV_DIR_VER);
    lv_obj_add_flag(s_nav_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_nav_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_nav_list, LV_OBJ_FLAG_SCROLL_ONE);

    s_nav_count = (int)(sizeof(s_nav_defs) / sizeof(s_nav_defs[0]));
    for (int i = 0; i < s_nav_count; i++) {
        /* 54px pitch keeps ~6 items in the 326px rail area; the rest scrolls. */
        int y = i * 54;
        lv_obj_t *row = lv_obj_create(s_nav_list);
        lv_obj_set_pos(row, 20, y);
        lv_obj_set_size(row, RAIL_W - 40, 48);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_S, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, nav_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_nav_defs[i].tab);

        s_nav[i].tab = s_nav_defs[i].tab;
        s_nav[i].row = row;
        s_nav[i].icon = ui_label_create(row, s_nav_defs[i].symbol, 0, 0,
                                        UI_COLOR_MUTED, UI_FONT_ICON);
        lv_obj_align(s_nav[i].icon, LV_ALIGN_LEFT_MID, 12, 0);
        s_nav[i].label = ui_label_create(row, i18n_str(s_nav_defs[i].label), 0, 0,
                                         UI_COLOR_MUTED, UI_FONT_BODY);
        lv_obj_align(s_nav[i].label, LV_ALIGN_LEFT_MID, 40, 0);
    }
    nav_update();
}

#define TASK_CARD_MAX 16

typedef struct {
    bool used;
    char id[16];
    lv_obj_t *card;
    lv_obj_t *name_label;
    lv_obj_t *origin_label;
    lv_obj_t *state_label;
    lv_obj_t *bar;
    lv_obj_t *stop_btn;
    lv_obj_t *delete_btn;
    char state_text[96];
    int progress;
    bool active;
} task_card_t;

static task_card_t s_task_cards[TASK_CARD_MAX];
static bool s_task_cards_valid;
static lv_obj_t *s_no_tasks_label;

static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static task_card_t *task_card_find(const char *id)
{
    if (id == NULL || id[0] == '\0' || !s_task_cards_valid) {
        return NULL;
    }
    for (int i = 0; i < TASK_CARD_MAX; i++) {
        if (s_task_cards[i].used && strcmp(s_task_cards[i].id, id) == 0) {
            return &s_task_cards[i];
        }
    }
    return NULL;
}

static void delete_task_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) return;
    char out[128] = {0};
    esp_err_t err = claw_api_remove_task(id, out, sizeof(out));
    ESP_LOGI(TAG, "delete task %s -> %s %s", id, esp_err_to_name(err), out);
    status_refresh();
}

static task_card_t *task_card_alloc(const char *id, const char *origin)
{
    for (int i = 0; i < TASK_CARD_MAX; i++) {
        task_card_t *slot = &s_task_cards[i];
        if (slot->used) {
            continue;
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        strlcpy(slot->id, id != NULL ? id : "", sizeof(slot->id));
        slot->progress = -1;

        slot->card = ui_card_create(s_body, 0, 0, CONTENT_W, 76);
        slot->name_label = ui_label_create(slot->card, "(unnamed)", 16, 10,
                                           UI_COLOR_TEXT, claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(slot->name_label, 480);
        lv_label_set_long_mode(slot->name_label, LV_LABEL_LONG_DOT);

        lv_obj_t *chip = ui_chip_create(slot->card,
                                        origin != NULL ? origin : "?",
                                        origin_color(origin), &slot->origin_label);
        lv_obj_set_pos(chip, CONTENT_W - 220, 10);

        slot->state_label = ui_label_create(slot->card, "", 16, 42,
                                            UI_COLOR_MUTED, claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(slot->state_label, 500);
        lv_label_set_long_mode(slot->state_label, LV_LABEL_LONG_DOT);

        slot->bar = ui_progress_create(slot->card, 300, 50, 300, 5, 0,
                                       UI_COLOR_MUTED);
        lv_obj_add_flag(slot->bar, LV_OBJ_FLAG_HIDDEN);

        slot->stop_btn = ui_button_create(slot->card, i18n_str(STR_CLAW_STOP),
                                          CONTENT_W - 112, 38, 96, 28, 2);
        lv_obj_add_flag(slot->stop_btn, LV_OBJ_FLAG_HIDDEN);
        char *id_copy = strdup(slot->id);
        if (id_copy != NULL) {
            lv_obj_add_event_cb(slot->stop_btn, stop_clicked, LV_EVENT_CLICKED,
                                id_copy);
            lv_obj_add_event_cb(slot->stop_btn, free_user_data_cb, LV_EVENT_DELETE,
                                id_copy);
        }

        slot->delete_btn = ui_button_create(slot->card, i18n_str(STR_CLAW_DELETE),
                                            CONTENT_W - 112, 38, 96, 28, 2);
        lv_obj_add_flag(slot->delete_btn, LV_OBJ_FLAG_HIDDEN);
        char *del_id = strdup(slot->id);
        if (del_id != NULL) {
            lv_obj_add_event_cb(slot->delete_btn, delete_task_clicked,
                                LV_EVENT_CLICKED, del_id);
            lv_obj_add_event_cb(slot->delete_btn, free_user_data_cb,
                                LV_EVENT_DELETE, del_id);
        }
        return slot;
    }
    return NULL;
}

static void task_card_update(task_card_t *slot, const cJSON *item)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
    const char *origin = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origin"));
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(item, "state"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(item, "message"));
    cJSON *progress_item = cJSON_GetObjectItem(item, "progress");
    int progress = cJSON_IsNumber(progress_item) ? progress_item->valueint : -1;

    label_set_if_changed(slot->name_label,
                         name != NULL && name[0] ? name : "(unnamed)");
    label_set_if_changed(slot->origin_label, origin != NULL ? origin : "?");

    char state_text[96];
    snprintf(state_text, sizeof(state_text), "%s%s%s",
             state != NULL ? state : "?", progress >= 0 ? " · " : "",
             (progress >= 0 && message != NULL) ? message : "");
    if (strcmp(slot->state_text, state_text) != 0) {
        strlcpy(slot->state_text, state_text, sizeof(slot->state_text));
        lv_label_set_text(slot->state_label, slot->state_text);
        uint32_t color = state_color(state);
        lv_obj_set_style_text_color(slot->state_label, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(slot->bar, lv_color_hex(color),
                                  LV_PART_INDICATOR);
    }

    if (progress != slot->progress) {
        slot->progress = progress;
        if (progress >= 0) {
            lv_obj_remove_flag(slot->bar, LV_OBJ_FLAG_HIDDEN);
            ui_progress_set_value(slot->bar, progress * 10);
        } else {
            lv_obj_add_flag(slot->bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool active = state != NULL &&
                  (strcmp(state, "running") == 0 || strcmp(state, "queued") == 0);
    slot->active = active;
    if (active) {
        lv_obj_remove_flag(slot->stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(slot->delete_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(slot->stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(slot->delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Incremental update: the task table changes on every progress report, and a
 * full delete/recreate of the list several times per second is visible as a
 * flash on the panel. Existing cards are updated in place instead. */
static void tasks_rebuild(void)
{
    if (!s_task_cards_valid) {
        memset(s_task_cards, 0, sizeof(s_task_cards));
        s_task_cards_valid = true;
        s_no_tasks_label = NULL;
    }

    char snapshot[TASKS_SNAPSHOT_MAX] = {0};
    if (claw_task_registry_snapshot_json(snapshot, sizeof(snapshot)) != ESP_OK) {
        return;
    }
    cJSON *array = cJSON_Parse(snapshot);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return;
    }

    bool seen[TASK_CARD_MAX] = {false};
    int index = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *origin = cJSON_GetStringValue(cJSON_GetObjectItem(item, "origin"));
        if (id == NULL || id[0] == '\0') {
            continue;
        }
        task_card_t *slot = task_card_find(id);
        if (slot == NULL) {
            slot = task_card_alloc(id, origin);
        }
        if (slot == NULL) {
            continue;
        }
        task_card_update(slot, item);
        lv_obj_move_to_index(slot->card, index);
        seen[slot - s_task_cards] = true;
        index++;
    }

    for (int i = 0; i < TASK_CARD_MAX; i++) {
        if (s_task_cards[i].used && !seen[i]) {
            lv_obj_delete(s_task_cards[i].card);
            memset(&s_task_cards[i], 0, sizeof(s_task_cards[i]));
        }
    }

    if (index == 0) {
        if (s_no_tasks_label == NULL) {
            s_no_tasks_label = ui_label_create(s_body, i18n_str(STR_CLAW_NO_TASKS),
                                               8, 12, UI_COLOR_MUTED, UI_FONT_BODY);
        }
        lv_obj_remove_flag(s_no_tasks_label, LV_OBJ_FLAG_HIDDEN);
    } else if (s_no_tasks_label != NULL) {
        lv_obj_add_flag(s_no_tasks_label, LV_OBJ_FLAG_HIDDEN);
    }

    cJSON_Delete(array);
}

/* ---------- scripts ---------- */

typedef struct {
    char path[192];
} run_ref_t;

static void run_clicked(lv_event_t *e)
{
    run_ref_t *ref = lv_event_get_user_data(e);
    if (ref == NULL) return;
    if (claw_api_is_script_active(ref->path)) {
        ESP_LOGI(TAG, "run %s ignored: already running", ref->path);
        return;
    }
    char out[256] = {0};
    esp_err_t err = claw_api_start_task(NULL, NULL, ref->path, NULL, 0,
                                        out, sizeof(out));
    ESP_LOGI(TAG, "run %s -> %s %s", ref->path, esp_err_to_name(err), out);
    status_refresh();
}

static run_ref_t *run_ref_new(const char *path)
{
    run_ref_t *ref = calloc(1, sizeof(*ref));
    if (ref == NULL) return NULL;
    strlcpy(ref->path, path != NULL ? path : "", sizeof(ref->path));
    return ref;
}

static char s_pending_delete[128];
static lv_obj_t *s_pending_delete_row;
static int s_script_rows;

static void delete_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_pending_delete[0] == '\0') {
        return;
    }
    char out[256] = {0};
    esp_err_t err = claw_api_delete_script(s_pending_delete, out, sizeof(out));
    ESP_LOGI(TAG, "delete %s -> %s %s", s_pending_delete,
             esp_err_to_name(err), out);
    if (err == ESP_OK) {
        if (s_pending_delete_row != NULL &&
            lv_obj_is_valid(s_pending_delete_row)) {
            lv_obj_delete(s_pending_delete_row);
            if (s_script_rows > 0) {
                s_script_rows--;
            }
        }
        s_pending_delete_row = NULL;
        if (s_script_rows == 0) {
            body_rebuild();
        }
    } else {
        s_pending_delete_row = NULL;
    }
    s_pending_delete[0] = '\0';
}

static void script_delete_clicked(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    if (name == NULL) return;
    strlcpy(s_pending_delete, name, sizeof(s_pending_delete));
    s_pending_delete_row = lv_obj_get_parent(lv_event_get_target(e));
    ui_confirm_create(lv_screen_active(), i18n_str(STR_CLAW_DELETE_CONFIRM),
                      delete_confirm_cb);
}

static bool new_script_done(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char out[256] = {0};
    esp_err_t err = claw_api_create_script(text, out, sizeof(out));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "create script failed: %s", out);
        return false;
    }
    body_rebuild();
    return true;
}

static void new_script_clicked(lv_event_t *e)
{
    (void)e;
    ui_text_dialog_open(i18n_str(STR_CLAW_NEW_SCRIPT),
                        i18n_str(STR_CLAW_SCRIPT_NAME_HINT),
                        "", false, new_script_done);
}

static void script_view_close_cb(void)
{
    heap_caps_free(s_script_view_buf);
    s_script_view_buf = NULL;
}

static void script_view_clicked(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    if (name == NULL || name[0] == '\0') {
        return;
    }
    ui_text_view_close();

    char *buf = heap_caps_malloc(SCRIPT_VIEW_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "no PSRAM for script view");
        return;
    }

    char absolute[256];
    snprintf(absolute, sizeof(absolute), CLAW_STORAGE_DATA_DIR "/scripts/%s", name);
    cJSON *request = cJSON_CreateObject();
    if (request == NULL) {
        heap_caps_free(buf);
        return;
    }
    cJSON_AddStringToObject(request, "path", absolute);
    char *input = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (input == NULL) {
        heap_caps_free(buf);
        return;
    }
    esp_err_t err = ui_cap_call("read_file", input, buf, SCRIPT_VIEW_MAX);
    cJSON_free(input);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read_file %s failed: %s", absolute, buf);
        heap_caps_free(buf);
        return;
    }

    s_script_view_buf = buf;
    ui_text_view_open(name, buf, claw_dyn_font(UI_FONT_SMALL),
                      script_view_close_cb);
}

static void format_size(long size, char *out, size_t out_size)
{
    if (size < 1024) {
        snprintf(out, out_size, "%ld B", size);
    } else {
        snprintf(out, out_size, "%ld.%ld KB",
                 size / 1024, (size % 1024) * 10 / 1024);
    }
}

static void files_refresh_clicked(lv_event_t *e)
{
    (void)e;
    body_rebuild();
}

static void table_header_label(lv_obj_t *parent, const char *text, int x, int w)
{
    lv_obj_t *label = ui_label_create(parent, text, x, 4,
                                      UI_COLOR_DISABLED, UI_FONT_SMALL);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static char s_files_dir[192] = CLAW_STORAGE_DATA_DIR;
static int s_files_volume;   /* 0 = FAT 内部存储, 1 = SD 卡 */

static const char *files_volume_root(void)
{
    return s_files_volume == 1 ? BSP_SD_MOUNT_POINT
                               : CLAW_STORAGE_MOUNT_POINT "/claw";
}

static void files_volume_clicked(lv_event_t *e)
{
    int volume = (int)(intptr_t)lv_event_get_user_data(e);
    if (volume == s_files_volume) {
        return;
    }
    s_files_volume = volume;
    strlcpy(s_files_dir, files_volume_root(), sizeof(s_files_dir));
    body_rebuild();
}

typedef struct {
    char name[64];
    bool is_dir;
    long size;
} claw_file_entry_t;

typedef struct {
    char path[192];
    char name[64];
    bool is_dir;
} claw_file_ref_t;

static int claw_file_cmp(const void *a, const void *b)
{
    const claw_file_entry_t *left = a;
    const claw_file_entry_t *right = b;
    if (left->is_dir != right->is_dir) {
        return left->is_dir ? -1 : 1;
    }
    return strcasecmp(left->name, right->name);
}

static void claw_file_view(const char *path, const char *title)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    if (f != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size >= 0 && size <= 64 * 1024) {
                buf = heap_caps_malloc((size_t)size + 1,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (buf != NULL) {
                    rewind(f);
                    size_t got = fread(buf, 1, (size_t)size, f);
                    buf[got] = '\0';
                }
            }
        }
        fclose(f);
    }
    if (buf == NULL) {
        buf = heap_caps_malloc(32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) return;
        strlcpy(buf, i18n_str(STR_CLAW_MEMORY_EMPTY), 32);
    }
    if (s_script_view_buf != NULL) {
        heap_caps_free(s_script_view_buf);
    }
    s_script_view_buf = buf;
    ui_text_view_open(title, buf, claw_dyn_font(UI_FONT_SMALL),
                      script_view_close_cb);
}

static void claw_file_clicked(lv_event_t *e)
{
    claw_file_ref_t *ref = lv_event_get_user_data(e);
    if (ref == NULL) return;
    if (ref->is_dir) {
        strlcpy(s_files_dir, ref->path, sizeof(s_files_dir));
        body_rebuild();
    } else {
        claw_file_view(ref->path, ref->name);
    }
}

static void files_up_clicked(lv_event_t *e)
{
    (void)e;
    const char *root = files_volume_root();
    if (strcmp(s_files_dir, root) == 0) {
        return;
    }
    if (strcmp(s_files_dir, CLAW_STORAGE_DATA_DIR) == 0) {
        strlcpy(s_files_dir, root, sizeof(s_files_dir));
    } else {
        char *slash = strrchr(s_files_dir, '/');
        if (slash != NULL && slash > s_files_dir + strlen(root)) {
            *slash = '\0';
        } else {
            strlcpy(s_files_dir, root, sizeof(s_files_dir));
        }
    }
    body_rebuild();
}

static void files_build(void)
{
    /* 存储卷切换：内部存储（FAT）/ SD 卡 */
    lv_obj_t *vol_row = lv_obj_create(s_body);
    lv_obj_set_size(vol_row, CONTENT_W, 44);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_radius(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fat_btn = ui_button_create(vol_row, i18n_str(STR_CLAW_FILES_FAT),
                                         0, 2, 130, 38, s_files_volume == 0 ? 1 : 0);
    lv_obj_add_event_cb(fat_btn, files_volume_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)0);
    lv_obj_t *sd_btn = ui_button_create(vol_row, i18n_str(STR_CLAW_FILES_SD),
                                        142, 2, 130, 38, s_files_volume == 1 ? 1 : 0);
    lv_obj_add_event_cb(sd_btn, files_volume_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);

    lv_obj_t *bar = ui_list_row_create(s_body, 0, 0, CONTENT_W, 48);
    if (bar != NULL) {
        lv_obj_t *path = ui_label_create(bar, s_files_dir, 16, 14,
                                         UI_COLOR_TEXT, claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(path, CONTENT_W - 220);
        lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
        lv_obj_t *up = ui_button_create(bar, i18n_str(STR_CLAW_UP),
                                        CONTENT_W - 180, 6, 76, 36, 0);
        if (up != NULL) {
            lv_obj_add_event_cb(up, files_up_clicked, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_t *refresh = ui_button_create(bar, i18n_str(STR_CLAW_REFRESH),
                                             CONTENT_W - 96, 6, 80, 36, 0);
        if (refresh != NULL) {
            lv_obj_add_event_cb(refresh, files_refresh_clicked, LV_EVENT_CLICKED, NULL);
        }
    }

    if (s_files_volume == 1) {
        struct stat st;
        if (stat(BSP_SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ui_label_create(s_body, i18n_str(STR_CLAW_SD_NOT_MOUNTED), 8, 8,
                            UI_COLOR_MUTED, UI_FONT_BODY);
            return;
        }
    }

    DIR *dir = opendir(s_files_dir);
    if (dir == NULL) {
        ui_label_create(s_body, i18n_str(STR_CLAW_MEMORY_EMPTY), 8, 0,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }

    claw_file_entry_t *entries = heap_caps_calloc(64, sizeof(*entries),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t count = 0;
    if (entries != NULL) {
        struct dirent *item;
        while (count < 64 && (item = readdir(dir)) != NULL) {
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
                continue;
            }
            char path[320];
            int n = snprintf(path, sizeof(path), "%s/%s", s_files_dir, item->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path)) {
                continue;
            }
            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }
            strlcpy(entries[count].name, item->d_name, sizeof(entries[count].name));
            entries[count].is_dir = S_ISDIR(st.st_mode);
            entries[count].size = (long)st.st_size;
            count++;
        }
    }
    closedir(dir);

    if (entries == NULL) {
        return;
    }
    if (count == 0) {
        ui_label_create(s_body, "Empty", 8, 0, UI_COLOR_MUTED, UI_FONT_BODY);
        heap_caps_free(entries);
        return;
    }
    qsort(entries, count, sizeof(*entries), claw_file_cmp);

    for (size_t i = 0; i < count; i++) {
        lv_obj_t *row = ui_list_row_create(s_body, 0, 0, CONTENT_W, 52);
        if (row == NULL) continue;
        ui_label_create(row,
                        entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                        16, 14, entries[i].is_dir ? UI_COLOR_ACCENT : UI_COLOR_MUTED,
                        UI_FONT_ICON_SMALL);
        lv_obj_t *name = ui_label_create(row, entries[i].name, 48, 8,
                                         UI_COLOR_TEXT, claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(name, CONTENT_W - 220);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char sub[48];
        if (entries[i].is_dir) {
            snprintf(sub, sizeof(sub), "DIR");
        } else {
            format_size(entries[i].size, sub, sizeof(sub));
        }
        ui_label_create(row, sub, 48, 30, UI_COLOR_MUTED, UI_FONT_SMALL);

        claw_file_ref_t *ref = calloc(1, sizeof(*ref));
        if (ref == NULL) continue;
        int n = snprintf(ref->path, sizeof(ref->path), "%s/%s",
                         s_files_dir, entries[i].name);
        if (n <= 0 || (size_t)n >= sizeof(ref->path)) {
            free(ref);
            continue;
        }
        strlcpy(ref->name, entries[i].name, sizeof(ref->name));
        ref->is_dir = entries[i].is_dir;
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, claw_file_clicked, LV_EVENT_CLICKED, ref);
        lv_obj_add_event_cb(row, free_user_data_cb, LV_EVENT_DELETE, ref);
    }
    heap_caps_free(entries);
}

/* ---------- capabilities ---------- */

static void caps_build(void)
{
    lv_obj_t *header = lv_obj_create(s_body);
    lv_obj_set_size(header, CONTENT_W, 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    ui_label_create(header, i18n_str(STR_CLAW_CAP_GROUP), 16, 4,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
    ui_label_create(header, i18n_str(STR_CLAW_CAP_STATE), 400, 4,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
    ui_label_create(header, i18n_str(STR_CLAW_CAP_TOOLS), 560, 4,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);

    claw_cap_group_list_t groups = claw_cap_list_groups();
    if (groups.count == 0) {
        ui_label_create(s_body, i18n_str(STR_CLAW_CAP_NONE), 8, 12,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }

    for (size_t i = 0; i < groups.count; i++) {
        const claw_cap_group_info_t *info = &groups.items[i];
        lv_obj_t *row = ui_list_row_create(s_body, 0, 0, CONTENT_W, 56);
        lv_obj_t *name = ui_label_create(row, info->group_id, 16, 8,
                                         UI_COLOR_TEXT,
                                         claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(name, 360);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

        char sub[96];
        snprintf(sub, sizeof(sub), "%s %s",
                 info->plugin_name != NULL ? info->plugin_name : "",
                 info->version != NULL ? info->version : "");
        lv_obj_t *sub_label = ui_label_create(row, sub, 16, 32,
                                              UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_width(sub_label, 360);
        lv_label_set_long_mode(sub_label, LV_LABEL_LONG_DOT);

        i18n_id_t state_id = STR_ST_WAIT;
        uint32_t color = UI_COLOR_WARN;
        if (info->state == CLAW_CAP_STATE_STARTED) {
            state_id = STR_ST_OK;
            color = UI_COLOR_OK;
        } else if (info->state == CLAW_CAP_STATE_DISABLED) {
            state_id = STR_ST_OFF;
            color = UI_COLOR_DISABLED;
        }
        ui_label_create(row, i18n_str(state_id), 400, 18, color, UI_FONT_SMALL);

        char count[24];
        snprintf(count, sizeof(count), "%u", (unsigned)info->descriptor_count);
        ui_label_create(row, count, 560, 18, UI_COLOR_MUTED, UI_FONT_SMALL);
    }
}

/* ---------- schedules ---------- */

#define SCHEDULE_UI_ROWS_MAX 16

typedef struct {
    char id[64];
    char kind[16];
    char event_key[96];
    bool enabled;
    int64_t next_ms;
    int run_count;
    lv_obj_t *detail_label;
    lv_obj_t *countdown_label;
} schedule_ui_row_t;

static schedule_ui_row_t s_schedule_rows[SCHEDULE_UI_ROWS_MAX];
static int s_schedule_row_count;
static int s_schedule_refresh_tick;

static const char *schedule_kind_text(const char *kind)
{
    if (kind != NULL && strcmp(kind, "once") == 0) {
        return i18n_str(STR_CLAW_SCHEDULE_ONCE);
    }
    if (kind != NULL && strcmp(kind, "interval") == 0) {
        return i18n_str(STR_CLAW_SCHEDULE_INTERVAL);
    }
    if (kind != NULL && strcmp(kind, "cron") == 0) {
        return i18n_str(STR_CLAW_SCHEDULE_CRON);
    }
    return kind != NULL && kind[0] ? kind : "?";
}

static const char *schedule_script_name(const char *path)
{
    if (path == NULL) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void schedule_id_request(const char *id, char *out, size_t out_size)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        out[0] = '\0';
        return;
    }
    cJSON_AddStringToObject(root, "id", id);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        out[0] = '\0';
        return;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
}

static void schedule_set_enabled(lv_event_t *e, bool enabled)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) return;
    char input[160];
    schedule_id_request(id, input, sizeof(input));
    if (input[0] == '\0') return;
    char out[512] = {0};
    esp_err_t err = ui_cap_call(enabled ? "scheduler_enable" : "scheduler_disable",
                               input, out, sizeof(out));
    ESP_LOGI(TAG, "schedule %s enable=%d -> %s %s", id, enabled,
             esp_err_to_name(err), out);
    body_rebuild();
}

static void schedule_enable_clicked(lv_event_t *e)
{
    schedule_set_enabled(e, true);
}

static void schedule_disable_clicked(lv_event_t *e)
{
    schedule_set_enabled(e, false);
}

static void schedule_trigger_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) return;
    char input[160];
    schedule_id_request(id, input, sizeof(input));
    if (input[0] == '\0') return;
    char out[512] = {0};
    esp_err_t err = ui_cap_call("scheduler_trigger_now", input, out, sizeof(out));
    ESP_LOGI(TAG, "schedule %s trigger -> %s %s", id,
             esp_err_to_name(err), out);
    if (err == ESP_OK) {
        body_rebuild();
    }
}

static void schedule_delete_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_pending_schedule[0] == '\0') return;
    char out[512] = {0};
    esp_err_t err = claw_api_remove_schedule(s_pending_schedule, out, sizeof(out));
    ESP_LOGI(TAG, "remove schedule %s -> %s %s", s_pending_schedule,
             esp_err_to_name(err), out);
    s_pending_schedule[0] = '\0';
    body_rebuild();
}

static void schedule_delete_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) return;
    strlcpy(s_pending_schedule, id, sizeof(s_pending_schedule));
    ui_confirm_create(lv_screen_active(), i18n_str(STR_CLAW_DELETE_CONFIRM),
                      schedule_delete_confirm_cb);
}

static void schedule_refresh_clicked(lv_event_t *e)
{
    (void)e;
    body_rebuild();
}

static void schedule_test_clicked(lv_event_t *e)
{
    (void)e;
    const char *script = CLAW_STORAGE_DATA_DIR "/scripts/_test.lua";
    char out[1024] = {0};
    esp_err_t err = ESP_ERR_NOT_FOUND;

    char id[32];
    snprintf(id, sizeof(id), "test%lu",
             (unsigned long)(esp_random() % 1000000UL));
    char rule_id[48];
    snprintf(rule_id, sizeof(rule_id), "%s_run", id);

    /* Official scheduled_task shape: add_router_rule first, then scheduler_add;
     * roll the rule back if the scheduler entry cannot be created. */
    cJSON *rule = cJSON_CreateObject();
    if (rule != NULL) {
        cJSON_AddStringToObject(rule, "id", rule_id);
        cJSON_AddStringToObject(rule, "description", "panel test schedule");
        cJSON_AddBoolToObject(rule, "enabled", true);
        cJSON_AddBoolToObject(rule, "consume_on_match", false);
        cJSON *match = cJSON_AddObjectToObject(rule, "match");
        cJSON_AddStringToObject(match, "event_type", "schedule");
        cJSON_AddStringToObject(match, "event_key", id);
        cJSON *actions = cJSON_AddArrayToObject(rule, "actions");
        cJSON *action = cJSON_CreateObject();
        cJSON_AddStringToObject(action, "type", "run_script");
        cJSON *input = cJSON_AddObjectToObject(action, "input");
        cJSON_AddStringToObject(input, "path", script);
        cJSON_AddObjectToObject(input, "args");
        cJSON_AddItemToArray(actions, action);

        char *rule_text = cJSON_PrintUnformatted(rule);
        cJSON_Delete(rule);
        if (rule_text != NULL) {
            cJSON *request = cJSON_CreateObject();
            cJSON_AddStringToObject(request, "rule_json", rule_text);
            char *request_text = cJSON_PrintUnformatted(request);
            cJSON_Delete(request);
            cJSON_free(rule_text);
            if (request_text != NULL) {
                err = ui_cap_call("add_router_rule", request_text,
                                  out, sizeof(out));
                cJSON_free(request_text);
            }
        }
    }

    if (err == ESP_OK) {
        cJSON *schedule = cJSON_CreateObject();
        if (schedule != NULL) {
            cJSON_AddStringToObject(schedule, "id", id);
            cJSON_AddBoolToObject(schedule, "enabled", true);
            cJSON_AddStringToObject(schedule, "kind", "interval");
            cJSON_AddNumberToObject(schedule, "start_at_ms", 0);
            cJSON_AddNumberToObject(schedule, "end_at_ms", 0);
            cJSON_AddNumberToObject(schedule, "interval_ms", 5000);
            cJSON_AddStringToObject(schedule, "event_type", "schedule");
            cJSON_AddStringToObject(schedule, "event_key", id);
            cJSON_AddStringToObject(schedule, "source_channel", "time");
            cJSON_AddStringToObject(schedule, "chat_id", "");
            cJSON_AddStringToObject(schedule, "content_type", "trigger");
            cJSON_AddStringToObject(schedule, "session_policy", "trigger");
            cJSON_AddStringToObject(schedule, "text", "panel test");
            cJSON_AddObjectToObject(schedule, "payload_json");
            cJSON_AddNumberToObject(schedule, "max_runs", 1);
            char *schedule_text = cJSON_PrintUnformatted(schedule);
            cJSON_Delete(schedule);
            if (schedule_text != NULL) {
                cJSON *request = cJSON_CreateObject();
                cJSON_AddStringToObject(request, "schedule_json", schedule_text);
                char *request_text = cJSON_PrintUnformatted(request);
                cJSON_Delete(request);
                cJSON_free(schedule_text);
                if (request_text != NULL) {
                    err = ui_cap_call("scheduler_add", request_text,
                                      out, sizeof(out));
                    cJSON_free(request_text);
                }
            }
        }
        if (err != ESP_OK) {
            char rollback[160];
            snprintf(rollback, sizeof(rollback), "{\"id\":\"%s\"}", rule_id);
            char ignored[256] = {0};
            (void)ui_cap_call("delete_router_rule", rollback,
                              ignored, sizeof(ignored));
        }
    }

    s_sched_msg_error = err != ESP_OK;
    strlcpy(s_sched_msg, i18n_str(err == ESP_OK ? STR_CLAW_SCHEDULE_CREATED
                                                : STR_CLAW_SCHEDULE_FAILED),
            sizeof(s_sched_msg));
    ESP_LOGI(TAG, "schedule test %s via rule %s -> %s %s", id, rule_id,
             esp_err_to_name(err), out);
    body_rebuild();
}

static void format_remaining(int64_t secs, char *out, size_t out_size)
{
    if (secs < 60) {
        snprintf(out, out_size, "%llds", (long long)secs);
    } else if (secs < 3600) {
        snprintf(out, out_size, "%lldm%02llds",
                 (long long)(secs / 60), (long long)(secs % 60));
    } else if (secs < 86400) {
        snprintf(out, out_size, "%lldh%02lldm",
                 (long long)(secs / 3600), (long long)((secs % 3600) / 60));
    } else {
        snprintf(out, out_size, "%lldd%02lldh",
                 (long long)(secs / 86400), (long long)((secs % 86400) / 3600));
    }
}

/* The countdown lives in its own right-aligned label so it is never
 * dot-truncated by a long kind/script/run text on the left. */
static void schedule_detail_text(const schedule_ui_row_t *row, int64_t now_ms,
                                 char *detail, size_t detail_size,
                                 char *countdown, size_t countdown_size,
                                 uint32_t *countdown_color)
{
    int64_t next_s = -1;
    if (row->next_ms > now_ms) {
        next_s = (row->next_ms - now_ms + 999) / 1000;
    }

    const char *kind = schedule_kind_text(row->kind);
    const char *script = schedule_script_name(row->event_key);
    const char *run = i18n_str(STR_CLAW_RUN);

    if (!row->enabled) {
        snprintf(detail, detail_size, "%s | %s | %s %d",
                 kind, script, run, row->run_count);
        snprintf(countdown, countdown_size, "%s", i18n_str(STR_ST_OFF));
        *countdown_color = UI_COLOR_MUTED;
    } else if (next_s >= 0) {
        char remaining[32];
        format_remaining(next_s, remaining, sizeof(remaining));
        snprintf(detail, detail_size, "%s | %s | %s %d",
                 kind, script, run, row->run_count);
        snprintf(countdown, countdown_size, "%s %s",
                 i18n_str(STR_CLAW_SCHEDULE_NEXT), remaining);
        *countdown_color = UI_COLOR_ACCENT;
    } else if (row->next_ms > 0) {
        snprintf(detail, detail_size, "%s | %s | %s %d",
                 kind, script, run, row->run_count);
        snprintf(countdown, countdown_size, "%s",
                 i18n_str(STR_CLAW_SCHEDULE_DUE));
        *countdown_color = UI_COLOR_WARN;
    } else if (row->run_count > 0) {
        snprintf(detail, detail_size, "%s | %s | %s %d",
                 kind, script, run, row->run_count);
        snprintf(countdown, countdown_size, "%s",
                 i18n_str(STR_CLAW_SCHEDULE_DONE));
        *countdown_color = UI_COLOR_OK;
    } else {
        snprintf(detail, detail_size, "%s | %s | %s %d",
                 kind, script, run, row->run_count);
        snprintf(countdown, countdown_size, "--");
        *countdown_color = UI_COLOR_MUTED;
    }
}

static void schedules_build(void)
{
    if (s_page_actions != NULL) {
        lv_obj_t *refresh = ui_button_create(s_page_actions,
                                             i18n_str(STR_CLAW_REFRESH),
                                             0, 2, 120, 40, 0);
        lv_obj_add_event_cb(refresh, schedule_refresh_clicked,
                            LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *msg_row = lv_obj_create(s_body);
    lv_obj_set_size(msg_row, CONTENT_W, 24);
    lv_obj_set_style_bg_opa(msg_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msg_row, 0, 0);
    lv_obj_set_style_radius(msg_row, 0, 0);
    lv_obj_set_style_pad_all(msg_row, 0, 0);
    lv_obj_remove_flag(msg_row, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t msg_color = s_sched_msg[0] == '\0' ? UI_COLOR_MUTED
                         : s_sched_msg_error ? UI_COLOR_ERROR : UI_COLOR_OK;
    lv_obj_t *msg = ui_label_create(msg_row, s_sched_msg, 8, 2,
                                    msg_color, UI_FONT_SMALL);
    lv_obj_set_width(msg, CONTENT_W - 16);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_DOT);

    char *text = heap_caps_malloc(SCHEDULES_JSON_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) {
        return;
    }
    if (ui_cap_call("scheduler_list", "{}", text, SCHEDULES_JSON_MAX) != ESP_OK) {
        heap_caps_free(text);
        return;
    }
    cJSON *array = cJSON_Parse(text);
    heap_caps_free(text);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return;
    }

    int count = cJSON_GetArraySize(array);
    if (count == 0) {
        ui_label_create(s_body, i18n_str(STR_CLAW_NO_SCHEDULES), 8, 12,
                        UI_COLOR_MUTED, UI_FONT_BODY);
    }

    s_schedule_row_count = 0;
    s_schedule_refresh_tick = 0;
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
        const char *event_key = cJSON_GetStringValue(cJSON_GetObjectItem(item, "event_key"));
        bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));
        cJSON *next_item = cJSON_GetObjectItem(item, "next_fire_ms");
        cJSON *run_item = cJSON_GetObjectItem(item, "run_count");
        int64_t next_ms = cJSON_IsNumber(next_item)
                          ? (int64_t)next_item->valuedouble : -1;
        int run_count = cJSON_IsNumber(run_item) ? run_item->valueint : 0;
        if (id == NULL || id[0] == '\0') {
            continue;
        }

        schedule_ui_row_t current = {0};
        strlcpy(current.id, id, sizeof(current.id));
        strlcpy(current.kind, kind != NULL ? kind : "", sizeof(current.kind));
        strlcpy(current.event_key, event_key != NULL ? event_key : "",
                sizeof(current.event_key));
        current.enabled = enabled;
        current.next_ms = next_ms;
        current.run_count = run_count;

        lv_obj_t *row = ui_list_row_create(s_body, 0, 0, CONTENT_W, 64);
        lv_obj_t *title = ui_label_create(row, id, 16, 8, UI_COLOR_TEXT,
                                          claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(title, CONTENT_W - 360);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

        char detail[224];
        char countdown[48];
        uint32_t countdown_color = UI_COLOR_MUTED;
        schedule_detail_text(&current, now_ms, detail, sizeof(detail),
                             countdown, sizeof(countdown), &countdown_color);

        /* Countdown gets a fixed slot on the right so it never truncates;
         * the 20px AI font needs ~120px for "下次 128d22h". */
        lv_obj_t *countdown_label = ui_label_create(row, countdown,
                                                    CONTENT_W - 360 - 144, 36,
                                                    countdown_color,
                                                    claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(countdown_label, 144);
        lv_label_set_long_mode(countdown_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(countdown_label, LV_TEXT_ALIGN_RIGHT, 0);

        /* Left part (kind | script | run count) marquees when it does not
         * fit, so nothing after the script name stays hidden. */
        lv_obj_t *detail_label = ui_label_create(row, detail, 16, 36,
                                                 UI_COLOR_MUTED,
                                                 claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(detail_label, CONTENT_W - 360 - 144 - 8);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_time(detail_label, 3000, 0);
        if (s_schedule_row_count < SCHEDULE_UI_ROWS_MAX) {
            current.detail_label = detail_label;
            current.countdown_label = countdown_label;
            s_schedule_rows[s_schedule_row_count++] = current;
        }

        lv_obj_t *trigger = ui_button_create(row,
                                             i18n_str(STR_CLAW_SCHEDULE_TRIGGER),
                                             CONTENT_W - 336, 18, 96, 28, 1);
        char *trigger_id = strdup(id);
        if (trigger_id != NULL) {
            lv_obj_add_event_cb(trigger, schedule_trigger_clicked,
                                LV_EVENT_CLICKED, trigger_id);
            lv_obj_add_event_cb(trigger, free_user_data_cb, LV_EVENT_DELETE,
                                trigger_id);
        }

        lv_obj_t *toggle = ui_button_create(row,
                                            i18n_str(enabled
                                                     ? STR_CLAW_SCHEDULE_DISABLE
                                                     : STR_CLAW_SCHEDULE_ENABLE),
                                            CONTENT_W - 224, 18, 96, 28, 0);
        char *toggle_id = strdup(id);
        if (toggle_id != NULL) {
            lv_obj_add_event_cb(toggle,
                                enabled ? schedule_disable_clicked
                                        : schedule_enable_clicked,
                                LV_EVENT_CLICKED, toggle_id);
            lv_obj_add_event_cb(toggle, free_user_data_cb, LV_EVENT_DELETE,
                                toggle_id);
        }

        lv_obj_t *del = ui_button_create(row, i18n_str(STR_CLAW_DELETE),
                                         CONTENT_W - 112, 18, 96, 28, 2);
        char *del_id = strdup(id);
        if (del_id != NULL) {
            lv_obj_add_event_cb(del, schedule_delete_clicked, LV_EVENT_CLICKED,
                                del_id);
            lv_obj_add_event_cb(del, free_user_data_cb, LV_EVENT_DELETE, del_id);
        }
    }
    cJSON_Delete(array);
}

static void schedules_tick(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    for (int i = 0; i < s_schedule_row_count; i++) {
        schedule_ui_row_t *row = &s_schedule_rows[i];
        if (row->detail_label == NULL || !lv_obj_is_valid(row->detail_label)) {
            continue;
        }
        char detail[224];
        char countdown[48];
        uint32_t countdown_color = UI_COLOR_MUTED;
        schedule_detail_text(row, now_ms, detail, sizeof(detail),
                             countdown, sizeof(countdown), &countdown_color);
        if (strcmp(lv_label_get_text(row->detail_label), detail) != 0) {
            lv_label_set_text(row->detail_label, detail);
        }
        if (row->countdown_label != NULL &&
            lv_obj_is_valid(row->countdown_label) &&
            strcmp(lv_label_get_text(row->countdown_label), countdown) != 0) {
            lv_label_set_text(row->countdown_label, countdown);
            lv_obj_set_style_text_color(row->countdown_label,
                                        lv_color_hex(countdown_color), 0);
        }
    }

    /* Refresh next_fire/run_count from cap_scheduler every 5 seconds; only
     * rebuild when the set/order changes, otherwise update labels in place. */
    if (++s_schedule_refresh_tick < 5) {
        return;
    }
    s_schedule_refresh_tick = 0;

    char *text = heap_caps_malloc(SCHEDULES_JSON_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) {
        return;
    }
    if (ui_cap_call("scheduler_list", "{}", text, SCHEDULES_JSON_MAX) != ESP_OK) {
        heap_caps_free(text);
        return;
    }
    cJSON *array = cJSON_Parse(text);
    heap_caps_free(text);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return;
    }

    int count = cJSON_GetArraySize(array);
    if (count != s_schedule_row_count) {
        cJSON_Delete(array);
        body_rebuild();
        return;
    }

    bool rebuild = false;
    const cJSON *item = NULL;
    int index = 0;
    cJSON_ArrayForEach(item, array) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        if (id == NULL || strcmp(id, s_schedule_rows[index].id) != 0) {
            rebuild = true;
            break;
        }
        index++;
    }
    if (rebuild) {
        cJSON_Delete(array);
        body_rebuild();
        return;
    }

    index = 0;
    cJSON_ArrayForEach(item, array) {
        schedule_ui_row_t *row = &s_schedule_rows[index++];
        row->enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));
        cJSON *next_item = cJSON_GetObjectItem(item, "next_fire_ms");
        row->next_ms = cJSON_IsNumber(next_item)
                       ? (int64_t)next_item->valuedouble : -1;
        cJSON *run_item = cJSON_GetObjectItem(item, "run_count");
        row->run_count = cJSON_IsNumber(run_item) ? run_item->valueint : 0;

        char detail[224];
        char countdown[48];
        uint32_t countdown_color = UI_COLOR_MUTED;
        schedule_detail_text(row, now_ms, detail, sizeof(detail),
                             countdown, sizeof(countdown), &countdown_color);
        if (row->detail_label != NULL && lv_obj_is_valid(row->detail_label) &&
            strcmp(lv_label_get_text(row->detail_label), detail) != 0) {
            lv_label_set_text(row->detail_label, detail);
        }
        if (row->countdown_label != NULL &&
            lv_obj_is_valid(row->countdown_label) &&
            strcmp(lv_label_get_text(row->countdown_label), countdown) != 0) {
            lv_label_set_text(row->countdown_label, countdown);
            lv_obj_set_style_text_color(row->countdown_label,
                                        lv_color_hex(countdown_color), 0);
        }
    }
    cJSON_Delete(array);
}

#if CONFIG_TEMPLATE_CLAW_AGENT

/* ---------- agent tab ---------- */

static void agent_refresh_widgets(void);

static void agent_go_settings(lv_event_t *e)
{
    (void)e;
    set_tab(CLAW_TAB_SETTINGS);
}

static bool agent_prompt_done(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char out[512] = {0};
    esp_err_t err = claw_api_agent_start_task(NULL, text, 120000,
                                              out, sizeof(out));
    if (err != ESP_OK) {
        strlcpy(s_agent_ui_error, out, sizeof(s_agent_ui_error));
    } else {
        s_agent_ui_error[0] = '\0';
    }
    agent_refresh_widgets();
    return true;
}

static void agent_ask_clicked(lv_event_t *e)
{
    (void)e;
    ui_text_dialog_open(i18n_str(STR_CLAW_AGENT_ASK),
                     i18n_str(STR_CLAW_AGENT_KBD_HINT),
                     "", false, agent_prompt_done);
}

static void agent_refresh_widgets(void);

static void agent_stop_clicked(lv_event_t *e)
{
    (void)e;
    if (s_agent_current_task_id[0] == '\0') {
        return;
    }
    char out[256] = {0};
    esp_err_t err = claw_api_stop_task(s_agent_current_task_id, 2000,
                                       out, sizeof(out));
    ESP_LOGI(TAG, "stop agent %s -> %s %s", s_agent_current_task_id,
             esp_err_to_name(err), out);
    agent_refresh_widgets();
}

static void agent_update_state_chip(bool configured, bool busy, bool ready)
{
    if (s_agent_chip == NULL || s_agent_chip_label == NULL) {
        return;
    }
    const int state = !configured ? 0 : busy ? 2 : ready ? 3 : 1;
    if (state == s_agent_chip_state) {
        return;  /* never re-style the chip every second */
    }
    s_agent_chip_state = state;

    const i18n_id_t state_id = !configured ? STR_CLAW_AGENT_UNCONFIGURED
                              : busy ? STR_CLAW_AGENT_BUSY
                              : ready ? STR_CLAW_AGENT_READY
                              : STR_CLAW_AGENT_ON;
    uint32_t color = !configured ? UI_COLOR_WARN
                     : busy ? UI_COLOR_WARN
                     : ready ? UI_COLOR_OK : UI_COLOR_OK;
    label_set_if_changed(s_agent_chip_label, i18n_str(state_id));
    lv_obj_set_style_bg_color(s_agent_chip, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_agent_chip, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_agent_chip_label, lv_color_hex(color), 0);
}

static void agent_build(void)
{
    char text[AGENT_STATUS_MAX] = {0};
    if (claw_api_agent_status_json(text, sizeof(text)) != ESP_OK) {
        ui_label_create(s_body, "Agent status unavailable", 8, 12,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        return;
    }

    bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "enabled"));
    bool configured = cJSON_IsTrue(cJSON_GetObjectItem(root, "configured"));
    bool ready = cJSON_IsTrue(cJSON_GetObjectItem(root, "ready"));
    bool busy = cJSON_IsTrue(cJSON_GetObjectItem(root, "busy"));
    bool worker = cJSON_IsTrue(cJSON_GetObjectItem(root, "worker"));
    const char *provider = cJSON_GetStringValue(cJSON_GetObjectItem(root, "provider"));
    const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(root, "model"));
    const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "task_id"));
    cJSON *seq_item = cJSON_GetObjectItem(root, "reply_seq");
    s_agent_seen_seq = cJSON_IsNumber(seq_item) ? (uint32_t)seq_item->valueint : 0;
    s_agent_seen_busy = busy;
    s_agent_seen_worker = worker;
    s_agent_ui_enabled = enabled;

    s_agent_chip = NULL;
    s_agent_chip_label = NULL;
    s_agent_error_label = NULL;
    s_agent_reply_label = NULL;
    s_agent_stop_btn = NULL;
    s_agent_current_task_id[0] = '\0';
    if (task_id != NULL && task_id[0] != '\0') {
        strlcpy(s_agent_current_task_id, task_id, sizeof(s_agent_current_task_id));
    }

    if (!enabled) {
        lv_obj_t *card = ui_card_create(s_body, 0, 0, CONTENT_W, 168);
        ui_label_create(card, i18n_str(STR_CLAW_AGENT_DISABLED), 20, 18,
                        UI_COLOR_TEXT, UI_FONT_BODY);
        lv_obj_t *hint = ui_label_create(card, i18n_str(STR_CLAW_AGENT_DISABLED_HINT),
                                         20, 54, UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_width(hint, CONTENT_W - 40);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_t *btn = ui_button_create(card, i18n_str(STR_CLAW_AGENT_GO_SETTINGS),
                                         20, 112, 180, 40, 1);
        lv_obj_add_event_cb(btn, agent_go_settings, LV_EVENT_CLICKED, NULL);
        cJSON_Delete(root);
        return;
    }

    /* status card */
    lv_obj_t *status_card = ui_card_create(s_body, 0, 0, CONTENT_W, 104);
    ui_label_create(status_card, i18n_str(STR_CLAW_AGENT_TITLE), 20, 14,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    s_agent_chip = ui_chip_create(status_card, "", UI_COLOR_MUTED,
                                  &s_agent_chip_label);
    lv_obj_align(s_agent_chip, LV_ALIGN_TOP_RIGHT, -20, 12);
    s_agent_chip_state = -1;
    s_agent_error_visible = false;
    agent_update_state_chip(configured, busy, ready);

    char line[160];
    snprintf(line, sizeof(line), "%s · %s",
             provider != NULL ? provider : "?", model != NULL ? model : "?");
    ui_label_create(status_card, line, 20, 50, UI_COLOR_MUTED, UI_FONT_SMALL);

    s_agent_error_label = ui_label_create(status_card, "", 20, 74,
                                          UI_COLOR_ERROR, UI_FONT_SMALL);
    lv_obj_set_width(s_agent_error_label, CONTENT_W - 40);
    lv_label_set_long_mode(s_agent_error_label, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_agent_error_label, LV_OBJ_FLAG_HIDDEN);

    /* last reply: fixed height + internal scroll keeps the layout stable, so
     * new answers never force a page rebuild (and never flash the panel). */
    const lv_font_t *reply_font = ai_ui_text_font();
    if (reply_font == NULL) {
        reply_font = UI_FONT_BODY;
    }
    lv_obj_t *reply_card = ui_card_create(s_body, 0, 0, CONTENT_W, 300);
    ui_section_title(reply_card, i18n_str(STR_CLAW_AGENT_LAST_REPLY), 20, 10);
    s_agent_reply_label = ui_label_create(reply_card, "", 20, 38,
                                          UI_COLOR_TEXT, reply_font);
    lv_obj_set_width(s_agent_reply_label, CONTENT_W - 40);
    lv_label_set_long_mode(s_agent_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(reply_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(reply_card, LV_DIR_VER);

    char reply[AGENT_REPLY_MAX] = {0};
    (void)claw_api_agent_last_reply(reply, sizeof(reply));
    if (reply[0] == '\0') {
        strlcpy(reply, i18n_str(STR_CLAW_AGENT_NO_REPLY), sizeof(reply));
    }
    lv_label_set_text(s_agent_reply_label, reply);

    const char *last_error = cJSON_GetStringValue(cJSON_GetObjectItem(root, "last_error"));
    const char *shown_error = s_agent_ui_error[0] != '\0'
                              ? s_agent_ui_error
                              : (last_error != NULL ? last_error : "");
    if (shown_error[0] != '\0') {
        lv_label_set_text(s_agent_error_label, shown_error);
        lv_obj_remove_flag(s_agent_error_label, LV_OBJ_FLAG_HIDDEN);
        s_agent_error_visible = true;
    }

    /* actions */
    lv_obj_t *row = lv_obj_create(s_body);
    lv_obj_set_size(row, CONTENT_W, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ask = ui_button_create(row, i18n_str(STR_CLAW_AGENT_ASK),
                                     0, 0, 210, 44, 1);
    lv_obj_add_event_cb(ask, agent_ask_clicked, LV_EVENT_CLICKED, NULL);

    s_agent_stop_btn = ui_button_create(row, i18n_str(STR_CLAW_STOP),
                                        0, 0, 140, 44, 0);
    lv_obj_add_event_cb(s_agent_stop_btn, agent_stop_clicked, LV_EVENT_CLICKED, NULL);
    if (!worker) {
        lv_obj_add_flag(s_agent_stop_btn, LV_OBJ_FLAG_HIDDEN);
    }

    ui_label_create(s_body, i18n_str(STR_CLAW_AGENT_VOICE_HINT), 20, 0,
                    UI_COLOR_MUTED, UI_FONT_SMALL);

    cJSON_Delete(root);
}

static void agent_refresh_widgets(void)
{
    char text[AGENT_STATUS_MAX] = {0};
    if (claw_api_agent_status_json(text, sizeof(text)) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        return;
    }

    bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "enabled"));
    bool configured = cJSON_IsTrue(cJSON_GetObjectItem(root, "configured"));
    bool ready = cJSON_IsTrue(cJSON_GetObjectItem(root, "ready"));
    bool busy = cJSON_IsTrue(cJSON_GetObjectItem(root, "busy"));
    bool worker = cJSON_IsTrue(cJSON_GetObjectItem(root, "worker"));
    cJSON *seq_item = cJSON_GetObjectItem(root, "reply_seq");
    uint32_t seq = cJSON_IsNumber(seq_item) ? (uint32_t)seq_item->valueint : 0;
    const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "task_id"));
    const char *last_error = cJSON_GetStringValue(cJSON_GetObjectItem(root, "last_error"));

    if (enabled != s_agent_ui_enabled) {
        cJSON_Delete(root);
        body_rebuild();
        return;
    }
    if (!enabled) {
        cJSON_Delete(root);
        return;
    }

    agent_update_state_chip(configured, busy, ready);

    if (seq != s_agent_seen_seq) {
        s_agent_seen_seq = seq;
        s_agent_ui_error[0] = '\0';
        char reply[AGENT_REPLY_MAX] = {0};
        (void)claw_api_agent_last_reply(reply, sizeof(reply));
        if (reply[0] == '\0') {
            strlcpy(reply, i18n_str(STR_CLAW_AGENT_NO_REPLY), sizeof(reply));
        }
        label_set_if_changed(s_agent_reply_label, reply);
    }

    if (s_agent_error_label != NULL) {
        const char *shown_error = s_agent_ui_error[0] != '\0'
                                  ? s_agent_ui_error
                                  : (last_error != NULL ? last_error : "");
        label_set_if_changed(s_agent_error_label, shown_error);
        bool visible = shown_error[0] != '\0';
        if (visible != s_agent_error_visible) {
            s_agent_error_visible = visible;
            if (visible) {
                lv_obj_remove_flag(s_agent_error_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_agent_error_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (busy != s_agent_seen_busy) {
        s_agent_seen_busy = busy;
    }
    if (worker != s_agent_seen_worker) {
        s_agent_seen_worker = worker;
        if (s_agent_stop_btn != NULL) {
            if (worker) {
                lv_obj_remove_flag(s_agent_stop_btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_agent_stop_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (task_id != NULL && task_id[0] != '\0') {
        strlcpy(s_agent_current_task_id, task_id, sizeof(s_agent_current_task_id));
    } else {
        s_agent_current_task_id[0] = '\0';
    }

    cJSON_Delete(root);
}

/* ---------- shared session chat ---------- */

static char s_chat_public_id[CLAW_SESSION_PUBLIC_ID_SIZE];
static char s_chat_session_id[CLAW_SESSION_MGR_ID_SIZE];
static char s_chat_title[CLAW_SESSION_TITLE_SIZE];
static char s_chat_error[128];
static uint32_t s_chat_sig;
static bool s_chat_sig_valid;

static bool chat_select_public_id(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    claw_session_info_t info = {0};
    if (claw_session_mgr_catalog_get(id, &info) != ESP_OK) {
        return false;
    }
    strlcpy(s_chat_public_id, id, sizeof(s_chat_public_id));
    strlcpy(s_chat_session_id, info.session_id, sizeof(s_chat_session_id));
    strlcpy(s_chat_title, info.title[0] ? info.title : id, sizeof(s_chat_title));
    s_chat_sig_valid = false;
    return true;
}

static bool chat_pick_or_create(void)
{
    if (s_chat_public_id[0] && chat_select_public_id(s_chat_public_id)) {
        return true;
    }

    cJSON *list = NULL;
    if (claw_session_mgr_catalog_list("web", 0, 10, &list) == ESP_OK) {
        cJSON *items = cJSON_GetObjectItem(list, "items");
        cJSON *first = items != NULL ? items->child : NULL;
        const char *id = cJSON_IsObject(first)
                         ? cJSON_GetStringValue(cJSON_GetObjectItem(first, "session"))
                         : NULL;
        if (id != NULL && chat_select_public_id(id)) {
            cJSON_Delete(list);
            return true;
        }
    }
    cJSON_Delete(list);

    claw_session_info_t created = {0};
    if (claw_session_mgr_catalog_create(&created) != ESP_OK) {
        return false;
    }
    return chat_select_public_id(created.id);
}

static uint32_t chat_hash_text(uint32_t hash, const char *text)
{
    if (text == NULL) {
        return hash;
    }
    while (*text) {
        hash = (hash ^ (uint8_t)*text++) * 16777619u;
    }
    return (hash ^ '\n') * 16777619u;
}

static uint32_t chat_history_sig(const cJSON *history, const char *state)
{
    uint32_t hash = 2166136261u;
    cJSON *messages = history != NULL ? cJSON_GetObjectItem(history, "messages") : NULL;
    if (!cJSON_IsArray(messages)) {
        return chat_hash_text(hash, state);
    }
    cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        hash = chat_hash_text(hash,
                              cJSON_GetStringValue(cJSON_GetObjectItem(message, "role")));
        hash = chat_hash_text(hash,
                              cJSON_GetStringValue(cJSON_GetObjectItem(message, "text")));
    }
    hash = chat_hash_text(hash, state);
    return hash;
}

static bool chat_send_done(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (!s_chat_public_id[0] && !chat_pick_or_create()) {
        strlcpy(s_chat_error, "no chat session", sizeof(s_chat_error));
        body_rebuild();
        return true;
    }

    char message_id[33];
    snprintf(message_id, sizeof(message_id), "%08x%08x%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random(),
             (unsigned)esp_random(), (unsigned)esp_random());

    claw_core_message_receipt_t receipt = {0};
    esp_err_t err = claw_agent_mgr_post_session_message(
        s_chat_public_id, text, message_id, false, &receipt);
    if (err != ESP_OK) {
        strlcpy(s_chat_error, esp_err_to_name(err), sizeof(s_chat_error));
    } else {
        s_chat_error[0] = '\0';
    }
    body_rebuild();
    return true;
}

static void chat_send_clicked(lv_event_t *e)
{
    (void)e;
    ui_text_dialog_open(i18n_str(STR_CLAW_AGENT_ASK),
                        i18n_str(STR_CLAW_AGENT_KBD_HINT),
                        "", false, chat_send_done);
}

static void chat_new_clicked(lv_event_t *e)
{
    (void)e;
    claw_session_info_t created = {0};
    if (claw_session_mgr_catalog_create(&created) == ESP_OK) {
        (void)chat_select_public_id(created.id);
        s_chat_error[0] = '\0';
    } else {
        strlcpy(s_chat_error, "create session failed", sizeof(s_chat_error));
    }
    body_rebuild();
}

static void chat_refresh_clicked(lv_event_t *e)
{
    (void)e;
    s_chat_public_id[0] = '\0';
    body_rebuild();
}

static void chat_session_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id != NULL && chat_select_public_id(id)) {
        s_chat_error[0] = '\0';
        body_rebuild();
    }
}

static void chat_build(void)
{
    char text[AGENT_STATUS_MAX] = {0};
    if (claw_api_agent_status_json(text, sizeof(text)) != ESP_OK) {
        ui_label_create(s_body, "Agent status unavailable", 8, 12,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }
    cJSON *status = cJSON_Parse(text);
    if (status == NULL) {
        return;
    }
    bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(status, "enabled"));
    bool configured = cJSON_IsTrue(cJSON_GetObjectItem(status, "configured"));
    cJSON_Delete(status);

    if (!enabled || !configured) {
        lv_obj_t *card = ui_card_create(s_body, 0, 0, CONTENT_W, 168);
        ui_label_create(card,
                        i18n_str(enabled ? STR_CLAW_AGENT_UNCONFIGURED
                                         : STR_CLAW_AGENT_DISABLED),
                        20, 18, UI_COLOR_TEXT, UI_FONT_BODY);
        lv_obj_t *hint = ui_label_create(card,
                                         i18n_str(STR_CLAW_AGENT_DISABLED_HINT),
                                         20, 54, UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_width(hint, CONTENT_W - 40);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_t *btn = ui_button_create(card, i18n_str(STR_CLAW_AGENT_GO_SETTINGS),
                                         20, 112, 180, 40, 1);
        lv_obj_add_event_cb(btn, agent_go_settings, LV_EVENT_CLICKED, NULL);
        return;
    }

    if (!chat_pick_or_create()) {
        ui_label_create(s_body, "chat session unavailable", 8, 12,
                        UI_COLOR_ERROR, UI_FONT_BODY);
        return;
    }

    static bool s_font_logged;
    if (!s_font_logged) {
        ESP_LOGI(TAG, "chat history font: %s",
                 ai_ui_text_font() != NULL ? "puhui" : "subset");
        s_font_logged = true;
    }

    const char *state = claw_agent_mgr_session_state(s_chat_session_id);
    lv_obj_t *bar = ui_card_create(s_body, 0, 0, CONTENT_W, 74);
    lv_obj_t *title = ui_label_create(bar, s_chat_title, 18, 10,
                                      UI_COLOR_TEXT, claw_dyn_font(UI_FONT_BODY));
    lv_obj_set_width(title, CONTENT_W - 300);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    char state_text[64];
    snprintf(state_text, sizeof(state_text), "%s  %s",
             i18n_str(STR_CLAW_AGENT_STATE),
             state != NULL && strcmp(state, "idle") != 0 ? "运行中" : "空闲");
    lv_obj_t *state_label = ui_label_create(bar, state_text, 18, 40,
                                            UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(state_label, CONTENT_W - 40);

    /* Recent Web sessions form a compact horizontal selector. */
    cJSON *list = NULL;
    if (claw_session_mgr_catalog_list("web", 0, 8, &list) == ESP_OK) {
        cJSON *items = cJSON_GetObjectItem(list, "items");
        int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
        if (count > 0) {
            lv_obj_t *chips = lv_obj_create(s_body);
            lv_obj_set_size(chips, CONTENT_W, 46);
            lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(chips, 0, 0);
            lv_obj_set_style_pad_all(chips, 0, 0);
            lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(chips, 8, 0);
            lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
            lv_obj_add_flag(chips, LV_OBJ_FLAG_SCROLLABLE);

            int shown = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, items) {
                if (shown >= 8) break;
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "session"));
                const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
                if (id == NULL || id[0] == '\0') continue;
                char chip_text[40];
                snprintf(chip_text, sizeof(chip_text), "%s", label && label[0] ? label : id);
                lv_obj_t *btn = ui_button_create(chips, chip_text, 0, 0, 170, 36,
                                                 strcmp(id, s_chat_public_id) == 0 ? 1 : 0);
                lv_obj_set_width(btn, 170);
                /* Session titles come from the first user message and can be
                 * arbitrary Chinese; the UI subset font would tofu. */
                lv_obj_t *chip_label = lv_obj_get_child(btn, 0);
                if (chip_label != NULL) {
                    lv_obj_set_style_text_font(chip_label,
                                               claw_dyn_font(UI_FONT_SMALL), 0);
                    lv_obj_set_width(chip_label, 150);
                    lv_label_set_long_mode(chip_label, LV_LABEL_LONG_DOT);
                }
                char *id_copy = strdup(id);
                if (id_copy != NULL) {
                    lv_obj_add_event_cb(btn, chat_session_clicked, LV_EVENT_CLICKED,
                                        id_copy);
                    lv_obj_add_event_cb(btn, free_user_data_cb, LV_EVENT_DELETE,
                                        id_copy);
                }
                shown++;
            }
        }
    }
    cJSON_Delete(list);

    lv_obj_t *history_card = ui_card_create(s_body, 0, 0, CONTENT_W, 292);
    lv_obj_set_flex_flow(history_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(history_card, 12, 0);
    lv_obj_set_style_pad_row(history_card, 8, 0);
    lv_obj_set_scroll_dir(history_card, LV_DIR_VER);
    lv_obj_add_flag(history_card, LV_OBJ_FLAG_SCROLLABLE);

    cJSON *history = NULL;
    esp_err_t history_err = claw_memory_read_session_messages(
        s_chat_session_id, 0, 30, &history);
    lv_obj_t *last_label = NULL;
    if (history_err == ESP_OK) {
        cJSON *messages = cJSON_GetObjectItem(history, "messages");
        cJSON *message = NULL;
        if (!cJSON_IsArray(messages)) {
            messages = NULL;
        }
        cJSON_ArrayForEach(message, messages) {
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
            const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(message, "text"));
            if (content == NULL || content[0] == '\0') continue;
            bool user = role != NULL && strcmp(role, "user") == 0;
            size_t line_len = strlen(content) + 8;
            char *line = malloc(line_len);
            if (line == NULL) continue;
            snprintf(line, line_len, "%s: %s", user ? "你" : "小爪", content);
            lv_obj_t *label = lv_label_create(history_card);
            /* Model replies contain arbitrary Chinese; the UI subset font only
             * carries the chars used by i18n, so use the shared puhui font. */
            lv_obj_set_style_text_font(label, claw_dyn_font(UI_FONT_BODY), 0);
            lv_label_set_text(label, line);
            free(line);
            lv_obj_set_width(label, CONTENT_W - 48);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(label,
                                        lv_color_hex(user ? UI_COLOR_TEXT : UI_COLOR_MUTED),
                                        0);
            last_label = label;
        }
        s_chat_sig = chat_history_sig(history, state);
        s_chat_sig_valid = true;
        cJSON_Delete(history);
    }
    if (last_label == NULL) {
        ui_label_create(history_card, "暂无消息", 8, 8, UI_COLOR_MUTED, UI_FONT_SMALL);
    } else {
        lv_obj_update_layout(history_card);
        lv_obj_scroll_to_view(last_label, LV_ANIM_OFF);
    }

    if (s_chat_error[0] != '\0') {
        ui_label_create(s_body, s_chat_error, 8, 2, UI_COLOR_ERROR, UI_FONT_SMALL);
    }

    lv_obj_t *row = lv_obj_create(s_body);
    lv_obj_set_size(row, CONTENT_W, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *send = ui_button_create(row, i18n_str(STR_CLAW_AGENT_SEND),
                                      0, 0, 180, 44, 1);
    lv_obj_add_event_cb(send, chat_send_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fresh = ui_button_create(row, "新会话",
                                       0, 0, 180, 44, 0);
    lv_obj_add_event_cb(fresh, chat_new_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh = ui_button_create(row, i18n_str(STR_CLAW_REFRESH),
                                         0, 0, 120, 44, 0);
    lv_obj_add_event_cb(refresh, chat_refresh_clicked, LV_EVENT_CLICKED, NULL);

    ui_label_create(s_body, i18n_str(STR_CLAW_AGENT_VOICE_HINT), 20, 0,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
}

static void chat_tick(void)
{
    if (s_chat_session_id[0] == '\0') {
        return;
    }
    cJSON *history = NULL;
    if (claw_memory_read_session_messages(s_chat_session_id, 0, 30, &history) != ESP_OK) {
        return;
    }
    uint32_t sig = chat_history_sig(history, claw_agent_mgr_session_state(s_chat_session_id));
    cJSON_Delete(history);
    if (!s_chat_sig_valid || sig != s_chat_sig) {
        s_chat_sig = sig;
        s_chat_sig_valid = true;
        body_rebuild();
    }
}

static void agent_tick(void)
{
    chat_tick();
}

#endif /* CONFIG_TEMPLATE_CLAW_AGENT */

/* ---------- settings ---------- */

static void lua_ai_confirm(lv_event_t *e)
{
    (void)e;
    claw_settings_set_lua_ai(true);
    if (s_lua_switch != NULL) {
        lv_obj_add_state(s_lua_switch, LV_STATE_CHECKED);
    }
    status_refresh();
}

static void lua_ai_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (on && !claw_settings_lua_ai_enabled()) {
        /* Never enable a risky feature on a single accidental tap: revert the
         * visual state and ask first. */
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        s_lua_switch = sw;
        ui_confirm_create(s_root, i18n_str(STR_CLAW_ENABLE_CONFIRM), lua_ai_confirm);
        return;
    }
    claw_settings_set_lua_ai(on);
    status_refresh();
}

static lv_obj_t *claw_switch_create(lv_obj_t *parent, int x, int y, bool on,
                                    lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_size(sw, 52, 30);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

#if CONFIG_TEMPLATE_CLAW_AGENT
static void agent_enable_confirm(lv_event_t *e)
{
    (void)e;
    char out[128] = {0};
    (void)claw_api_agent_set_enabled(true, out, sizeof(out));
    s_agent_cfg_error = false;
    body_rebuild();
}

static void agent_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (on && !claw_settings_agent_enabled()) {
        if (!claw_settings_agent_configured()) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
            s_agent_cfg_error = true;
            body_rebuild();
            return;
        }
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ui_confirm_create(s_root, i18n_str(STR_CLAW_AGENT_ENABLE_CONFIRM),
                          agent_enable_confirm);
        return;
    }

    char out[128] = {0};
    (void)claw_api_agent_set_enabled(on, out, sizeof(out));
    s_agent_cfg_error = false;
    body_rebuild();
}

static int agent_backend_index(const claw_agent_settings_t *cfg)
{
    if (strcmp(cfg->backend, "anthropic_compatible") == 0) {
        return 2;
    }
    if (strstr(cfg->base_url, "deepseek") != NULL) {
        return 1;
    }
    return 0;
}

static bool url_is_preset(const char *url)
{
    return url[0] == '\0' ||
           strstr(url, "deepseek.com") != NULL ||
           strstr(url, "openai.com") != NULL ||
           strstr(url, "anthropic.com") != NULL;
}

static void agent_backend_event(lv_event_t *e)
{
    int selected = ui_segmented_get_selected(lv_event_get_target(e));
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);

    if (selected == 2) {
        (void)claw_settings_set_agent_backend("anthropic_compatible");
        if (url_is_preset(cfg.base_url) && strstr(cfg.base_url, "anthropic.com") == NULL) {
            (void)claw_settings_set_agent_base_url("https://api.anthropic.com/v1");
        }
    } else {
        (void)claw_settings_set_agent_backend("openai_compatible");
        if (selected == 1) {
            if (url_is_preset(cfg.base_url) && strstr(cfg.base_url, "deepseek.com") == NULL) {
                (void)claw_settings_set_agent_base_url("https://api.deepseek.com/v1");
            }
            if (cfg.model[0] == '\0') {
                (void)claw_settings_set_agent_model("deepseek-chat");
            }
        } else if (cfg.base_url[0] == '\0' ||
                   strstr(cfg.base_url, "deepseek.com") != NULL) {
            (void)claw_settings_set_agent_base_url("https://api.openai.com/v1");
        }
    }
    claw_api_agent_settings_changed();
    body_rebuild();
}

static bool agent_url_done(const char *text)
{
    if (claw_settings_set_agent_base_url(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    body_rebuild();
    return true;
}

static bool agent_model_done(const char *text)
{
    if (claw_settings_set_agent_model(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    body_rebuild();
    return true;
}

static bool agent_key_done(const char *text)
{
    if (claw_settings_set_agent_api_key(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    body_rebuild();
    return true;
}

static void agent_url_clicked(lv_event_t *e)
{
    (void)e;
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);
    ui_text_dialog_open(i18n_str(STR_CLAW_AGENT_URL),
                     "https://api.openai.com/v1",
                     cfg.base_url, false, agent_url_done);
}

static void agent_model_clicked(lv_event_t *e)
{
    (void)e;
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);
    ui_text_dialog_open(i18n_str(STR_CLAW_AGENT_MODEL),
                     i18n_str(STR_CLAW_AGENT_MODEL),
                     cfg.model, false, agent_model_done);
}

static void agent_key_clicked(lv_event_t *e)
{
    (void)e;
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);
    ui_text_dialog_open(i18n_str(STR_CLAW_AGENT_KEY),
                     i18n_str(STR_CLAW_AGENT_KEY_HINT),
                     cfg.api_key, true, agent_key_done);
}

static lv_obj_t *agent_menu_row(lv_obj_t *parent, int y, const char *title,
                                const char *value, lv_event_cb_t cb)
{
    lv_obj_t *val_label = NULL;
    ui_menu_row_create(parent, 20, y, CONTENT_W - 40, 52,
                       LV_SYMBOL_EDIT, title, value, &val_label, cb, NULL);
    if (val_label != NULL) {
        lv_obj_set_width(val_label, 380);
        lv_label_set_long_mode(val_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(val_label, LV_TEXT_ALIGN_RIGHT, 0);
    }
    return parent;
}

static void agent_settings_build(void)
{
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);

    lv_obj_t *card = ui_card_create(s_body, 0, 0, CONTENT_W, 104);
    ui_label_create(card, i18n_str(STR_CLAW_AGENT_TITLE), 20, 18,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    ui_label_create(card, i18n_str(STR_CLAW_AGENT_DISABLED_HINT), 20, 50,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    claw_switch_create(card, CONTENT_W - 92, 22, cfg.enabled, agent_switch_event);
    if (s_agent_cfg_error) {
        ui_label_create(card, i18n_str(STR_CLAW_AGENT_NEED_CONFIG), 20, 74,
                        UI_COLOR_ERROR, UI_FONT_SMALL);
    }

    lv_obj_t *cfg_card = ui_card_create(s_body, 0, 0, CONTENT_W, 348);
    ui_section_title(cfg_card, i18n_str(STR_CLAW_AGENT_SETTINGS), 20, 12);

    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_BACKEND), 20, 56,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    static const char *const backends[3] = { "OpenAI", "DeepSeek", "Claude" };
    lv_obj_t *seg = ui_segmented_create(cfg_card, 150, 42, 430, 36,
                                        backends, 3, agent_backend_index(&cfg));
    lv_obj_add_event_cb(seg, agent_backend_event, LV_EVENT_VALUE_CHANGED, NULL);

    const char *not_set = i18n_str(STR_CLAW_AGENT_KEY_NONE);
    agent_menu_row(cfg_card, 96, i18n_str(STR_CLAW_AGENT_URL),
                   cfg.base_url[0] ? cfg.base_url : not_set, agent_url_clicked);
    agent_menu_row(cfg_card, 156, i18n_str(STR_CLAW_AGENT_MODEL),
                   cfg.model[0] ? cfg.model : not_set, agent_model_clicked);
    agent_menu_row(cfg_card, 216, i18n_str(STR_CLAW_AGENT_KEY),
                   cfg.api_key[0] ? i18n_str(STR_CLAW_AGENT_KEY_SET) : not_set,
                   agent_key_clicked);
    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_KEY_HINT), 20, 282,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_MEM_OFF), 20, 308,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
}
#endif /* CONFIG_TEMPLATE_CLAW_AGENT */

static app_config_t *claw_app_config_alloc(void)
{
    app_config_t *cfg = heap_caps_calloc(1, sizeof(*cfg),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cfg == NULL) {
        return NULL;
    }
    if (app_config_load(cfg) != ESP_OK) {
        heap_caps_free(cfg);
        return NULL;
    }
    return cfg;
}

static void claw_section_title(const char *text)
{
    if (text == NULL) return;
    lv_obj_t *label = ui_label_create(s_body, text, 8, 0,
                                      UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(label, CONTENT_W - 16);
}

static void claw_format_storage(char *out, size_t out_size,
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

static const char *claw_agent_state_text(bool enabled, bool configured,
                                         bool ready, bool busy)
{
    if (!enabled) return i18n_str(STR_CLAW_AGENT_OFF);
    if (!configured) return i18n_str(STR_CLAW_AGENT_UNCONFIGURED);
    if (busy) return i18n_str(STR_CLAW_AGENT_BUSY);
    if (ready) return i18n_str(STR_CLAW_AGENT_READY);
    return i18n_str(STR_CLAW_AGENT_ON);
}

static void claw_info_row(lv_obj_t *parent, const char *label, const char *value)
{
    lv_obj_t *row = ui_list_row_create(parent, 0, 0, CONTENT_W, 48);
    if (row == NULL) return;
    ui_label_create(row, label, 16, 14, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_t *val = ui_label_create(row, value != NULL ? value : "--",
                                    200, 14, UI_COLOR_TEXT, UI_FONT_SMALL);
    lv_obj_set_width(val, CONTENT_W - 220);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
}

static void claw_status_row(lv_obj_t *parent, const char *label,
                            const char *value, bool ok)
{
    lv_obj_t *row = ui_list_row_create(parent, 0, 0, CONTENT_W, 48);
    if (row == NULL) return;
    ui_label_create(row, label, 16, 14, UI_COLOR_TEXT, claw_dyn_font(UI_FONT_BODY));
    lv_obj_t *val = ui_label_create(row, value != NULL ? value : "--",
                                    200, 14, ok ? UI_COLOR_OK : UI_COLOR_DISABLED,
                                    UI_FONT_SMALL);
    lv_obj_set_width(val, CONTENT_W - 220);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
}

static void status_build(void)
{
    app_config_t *cfg = claw_app_config_alloc();
    bool connected = hw_wifi_is_connected();
    const char *ip = hw_wifi_ip();
    int jobs = 0;
    bool lua = false, agent = false, configured = false, ready = false, busy = false;

    char text[1024] = {0};
    if (claw_api_status_json(text, sizeof(text)) == ESP_OK) {
        cJSON *root = cJSON_Parse(text);
        if (root != NULL) {
            jobs = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "async_jobs"));
            lua = cJSON_IsTrue(cJSON_GetObjectItem(root, "lua_ai"));
            agent = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent"));
            configured = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_configured"));
            ready = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_ready"));
            busy = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_busy"));
            cJSON_Delete(root);
        }
    }

    claw_section_title(i18n_str(STR_CLAW_SEC_STATUS_NET));
    claw_info_row(s_body, i18n_str(STR_CLAW_WIFI_STATUS),
                  i18n_str(connected ? STR_WIFI_CONNECTED : STR_WIFI_DISCONNECTED));
    claw_info_row(s_body, i18n_str(STR_CLAW_IP),
                  (ip != NULL && ip[0]) ? ip : "--");
    claw_info_row(s_body, i18n_str(STR_CLAW_SSID),
                  (cfg != NULL && cfg->wifi_ssid[0]) ? cfg->wifi_ssid : "--");
    claw_info_row(s_body, i18n_str(STR_CLAW_MODE), "STA");

    claw_section_title(i18n_str(STR_CLAW_SEC_STATUS_STORE));
    char usage[64];
    uint64_t fat_total = 0, fat_free = 0;
    if (claw_storage_get_space(&fat_total, &fat_free) == ESP_OK) {
        claw_format_storage(usage, sizeof(usage), fat_total,
                            fat_total > fat_free ? fat_total - fat_free : 0);
    } else {
        strlcpy(usage, "--", sizeof(usage));
    }
    claw_info_row(s_body, "FAT", usage);
    claw_info_row(s_body, "AI", CLAW_STORAGE_MOUNT_POINT "/ai");
    claw_info_row(s_body, i18n_str(STR_CLAW_WORKSPACE),
                  CLAW_STORAGE_MOUNT_POINT "/claw");

    claw_section_title(i18n_str(STR_CLAW_SEC_STATUS_RUN));
    claw_info_row(s_body, i18n_str(STR_CLAW_AGENT_STATE),
                  claw_agent_state_text(agent, configured, ready, busy));
    claw_info_row(s_body, i18n_str(STR_CLAW_LUA_AI),
                  i18n_str(lua ? STR_CLAW_CLOUD_ON : STR_CLAW_CLOUD_OFF));
    char jobs_text[32];
    snprintf(jobs_text, sizeof(jobs_text), "%d", jobs);
    claw_info_row(s_body, i18n_str(STR_CLAW_JOBS), jobs_text);
    char counts[48];
    snprintf(counts, sizeof(counts), "%u / %u",
             (unsigned)cap_lua_get_module_count(),
             (unsigned)claw_cap_list_groups().count);
    claw_info_row(s_body, i18n_str(STR_CLAW_MODULES), counts);

    heap_caps_free(cfg);
}

typedef struct {
    char path[192];
    char title[64];
} memory_ref_t;

static void memory_view_clicked(lv_event_t *e)
{
    memory_ref_t *ref = lv_event_get_user_data(e);
    if (ref == NULL) return;

    FILE *f = fopen(ref->path, "rb");
    char *buf = NULL;
    if (f != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size >= 0 && size <= 64 * 1024) {
                buf = heap_caps_malloc((size_t)size + 1,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (buf != NULL) {
                    rewind(f);
                    size_t got = fread(buf, 1, (size_t)size, f);
                    buf[got] = '\0';
                }
            }
        }
        fclose(f);
    }
    if (buf == NULL) {
        buf = heap_caps_malloc(1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) return;
        buf[0] = '\0';
    }
    if (s_script_view_buf != NULL) {
        heap_caps_free(s_script_view_buf);
    }
    s_script_view_buf = buf;
    ui_text_view_open(ref->title, buf, claw_dyn_font(UI_FONT_SMALL),
                      script_view_close_cb);
}

static void memory_row(const char *path, i18n_id_t title_id)
{
    struct stat st;
    bool exists = stat(path, &st) == 0;
    lv_obj_t *row = ui_list_row_create(s_body, 0, 0, CONTENT_W, 64);
    if (row == NULL) return;
    ui_label_create(row, i18n_str(title_id), 16, 10, UI_COLOR_TEXT,
                    claw_dyn_font(UI_FONT_BODY));
    lv_obj_t *path_label = ui_label_create(row, path, 16, 36,
                                           UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(path_label, CONTENT_W - 200);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);

    char right[32];
    if (!exists) {
        snprintf(right, sizeof(right), "%s", i18n_str(STR_CLAW_MEMORY_EMPTY));
    } else {
        format_size((long)st.st_size, right, sizeof(right));
    }
    lv_obj_t *size = ui_label_create(row, right, CONTENT_W - 180, 22,
                                     exists ? UI_COLOR_OK : UI_COLOR_DISABLED,
                                     UI_FONT_SMALL);
    lv_obj_set_width(size, 160);
    lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
    if (!exists) {
        return;
    }

    memory_ref_t *ref = calloc(1, sizeof(*ref));
    if (ref == NULL) return;
    strlcpy(ref->path, path, sizeof(ref->path));
    strlcpy(ref->title, i18n_str(title_id), sizeof(ref->title));
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, memory_view_clicked, LV_EVENT_CLICKED, ref);
    lv_obj_add_event_cb(row, free_user_data_cb, LV_EVENT_DELETE, ref);
}

static void memory_build(void)
{
    memory_row(CLAW_STORAGE_DATA_DIR "/agent/memory/MEMORY.md", STR_CLAW_MEMORY_LONG);
    memory_row(CLAW_STORAGE_DATA_DIR "/agent/memory/soul.md", STR_CLAW_MEMORY_SOUL);
    memory_row(CLAW_STORAGE_DATA_DIR "/agent/memory/identity.md", STR_CLAW_MEMORY_IDENTITY);
    memory_row(CLAW_STORAGE_DATA_DIR "/agent/memory/user.md", STR_CLAW_MEMORY_USER);
    ui_label_create(s_body, i18n_str(STR_CLAW_WEB_HINT), 8, 0,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
}

static void lua_build(void)
{
    size_t count = cap_lua_get_module_count();
    if (count == 0) {
        ui_label_create(s_body, i18n_str(STR_CLAW_CAP_NONE), 8, 0,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const cap_lua_module_t *module = cap_lua_get_module(i);
        if (module == NULL || module->name == NULL) continue;
        lv_obj_t *row = ui_list_row_create(s_body, 0, 0, CONTENT_W, 48);
        ui_label_create(row, LV_SYMBOL_FILE, 16, 14, UI_COLOR_MUTED,
                        UI_FONT_ICON_SMALL);
        ui_label_create(row, module->name, 48, 14, UI_COLOR_TEXT,
                        claw_dyn_font(UI_FONT_BODY));
    }
}

static void settings_build(void)
{
    app_config_t *cfg = claw_app_config_alloc();

    claw_section_title(i18n_str(STR_CLAW_SEC_LLM));
#if CONFIG_TEMPLATE_CLAW_AGENT
    agent_settings_build();
#endif
    lv_obj_t *lua_card = ui_card_create(s_body, 0, 0, CONTENT_W, 104);
    ui_label_create(lua_card, i18n_str(STR_CLAW_LUA_AI), 20, 18,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    ui_label_create(lua_card, i18n_str(STR_CLAW_LUA_AI_HINT), 20, 50,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    s_lua_switch = claw_switch_create(lua_card, CONTENT_W - 92, 22,
                                      claw_settings_lua_ai_enabled(),
                                      lua_ai_switch_event);

    claw_section_title(i18n_str(STR_CLAW_SEC_IM));
    const char *yes = i18n_str(STR_CLAW_CONFIGURED);
    const char *no = i18n_str(STR_CLAW_NOT_CONFIGURED);
    struct {
        const char *name;
        const char *value;
        bool ok;
    } im_rows[] = {
        { "WeChat", (cfg != NULL && (cfg->wechat_token[0] || cfg->wechat_account_id[0])) ? yes : no,
          cfg != NULL && (cfg->wechat_token[0] || cfg->wechat_account_id[0]) },
        { "Feishu", (cfg != NULL && cfg->feishu_app_id[0]) ? yes : no,
          cfg != NULL && cfg->feishu_app_id[0] },
        { "QQ", (cfg != NULL && cfg->qq_app_id[0]) ? yes : no,
          cfg != NULL && cfg->qq_app_id[0] },
        { "Telegram", (cfg != NULL && cfg->tg_bot_token[0]) ? yes : no,
          cfg != NULL && cfg->tg_bot_token[0] },
    };
    for (size_t i = 0; i < sizeof(im_rows) / sizeof(im_rows[0]); i++) {
        claw_status_row(s_body, im_rows[i].name, im_rows[i].value, im_rows[i].ok);
    }

    ui_label_create(s_body, i18n_str(STR_CLAW_WEB_HINT), 8, 0,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
    heap_caps_free(cfg);
}

/* ---------- tabs ---------- */

static void body_rebuild(void)
{
    if (s_body == NULL) return;
    lv_obj_clean(s_body);
    if (s_page_actions != NULL) {
        lv_obj_clean(s_page_actions);
    }
    s_lua_switch = NULL;
    s_task_cards_valid = false;
    s_no_tasks_label = NULL;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_agent_chip = NULL;
    s_agent_chip_label = NULL;
    s_agent_error_label = NULL;
    s_agent_reply_label = NULL;
    s_agent_stop_btn = NULL;
    s_agent_ui_enabled = false;
    s_agent_seen_worker = false;
    s_agent_chip_state = -1;
    s_agent_error_visible = false;
    s_agent_current_task_id[0] = '\0';
#endif

    if (!claw_port_is_ready()) {
        ui_label_create(s_body, "Claw unavailable", 8, 12,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }

    if (s_tab == CLAW_TAB_STATUS) {
        status_build();
    } else if (s_tab == CLAW_TAB_SETTINGS) {
        settings_build();
    } else if (s_tab == CLAW_TAB_MEMORY) {
        memory_build();
#if CONFIG_TEMPLATE_CLAW_AGENT
    } else if (s_tab == CLAW_TAB_CHAT) {
        chat_build();
#endif
    } else if (s_tab == CLAW_TAB_SCHEDULES) {
        schedules_build();
    } else if (s_tab == CLAW_TAB_CAPS) {
        caps_build();
    } else if (s_tab == CLAW_TAB_LUA) {
        lua_build();
    } else if (s_tab == CLAW_TAB_FILES) {
        files_build();
    }
}

static void claws_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_root == NULL || !lv_obj_is_visible(s_root)) return;

    bool ready = claw_port_is_ready();
    if (ready != s_claw_ready_seen) {
        s_claw_ready_seen = ready;
        if (ready) {
            body_rebuild();
        }
    }

    status_refresh();
#if CONFIG_TEMPLATE_CLAW_AGENT
    if (s_tab == CLAW_TAB_CHAT) {
        agent_tick();
    }
#endif
    if (s_tab == CLAW_TAB_SCHEDULES) {
        schedules_tick();
    }
}

/* ---------- page ---------- */

#if CLAW_UI_SELFTEST
static void selftest_tab_cb(lv_timer_t *timer)
{
    static int index = 0;
    index++;
    if (index >= CLAW_TAB_COUNT) {
        lv_timer_delete(timer);
        ESP_LOGI(TAG, "ui selftest done");
        return;
    }
    ESP_LOGI(TAG, "ui selftest -> tab %d", index);
    set_tab(index);
}
#endif

static void claw_create(lv_obj_t *content)
{
    s_root = content;
    s_tab = CLAW_TAB_STATUS;
    s_claw_ready_seen = claw_port_is_ready();
    s_lua_switch = NULL;
    s_pending_schedule[0] = '\0';
    s_sched_msg[0] = '\0';
    s_sched_msg_error = false;
    s_pending_delete[0] = '\0';
    s_pending_delete_row = NULL;
    s_script_rows = 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_agent_seen_seq = 0;
    s_agent_seen_busy = false;
    s_agent_cfg_error = false;
    s_agent_ui_error[0] = '\0';
#endif

    nav_build();

    s_main = lv_obj_create(content);
    lv_obj_set_pos(s_main, MAIN_X, 0);
    lv_obj_set_size(s_main, CONTENT_W, UI_PAGE_H);
    lv_obj_set_style_bg_opa(s_main, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_main, 0, 0);
    lv_obj_set_style_radius(s_main, 0, 0);
    lv_obj_set_style_pad_all(s_main, 0, 0);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    s_chip_lua = ui_chip_create(s_main, i18n_str(STR_CLAW_CLOUD_OFF),
                                UI_COLOR_MUTED, &s_chip_lua_label);
    lv_obj_set_pos(s_chip_lua, 0, 0);
    s_chip_jobs = ui_chip_create(s_main, i18n_str(STR_CLAW_JOBS),
                                 UI_COLOR_ACCENT, &s_chip_jobs_label);
    lv_obj_set_pos(s_chip_jobs, 180, 0);
    s_chip_store = ui_chip_create(s_main, "0 KB", UI_COLOR_CYAN,
                                  &s_chip_store_label);
    lv_obj_set_pos(s_chip_store, 340, 0);
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_chip_agent = ui_chip_create(s_main, i18n_str(STR_CLAW_AGENT_OFF),
                                  UI_COLOR_WARN, &s_chip_agent_label);
    lv_obj_set_pos(s_chip_agent, 500, 0);
#endif

    s_page_title = ui_label_create(s_main, "", 0, 42, UI_COLOR_TEXT,
                                   UI_FONT_BODY);
    lv_obj_set_width(s_page_title, CONTENT_W - ACTIONS_W - 16);
    lv_label_set_long_mode(s_page_title, LV_LABEL_LONG_DOT);
    s_page_desc = ui_label_create(s_main, "", 0, 72, UI_COLOR_MUTED,
                                  UI_FONT_SMALL);
    lv_obj_set_width(s_page_desc, CONTENT_W - ACTIONS_W - 16);
    lv_label_set_long_mode(s_page_desc, LV_LABEL_LONG_DOT);

    s_page_actions = lv_obj_create(s_main);
    lv_obj_set_pos(s_page_actions, CONTENT_W - ACTIONS_W, 30);
    lv_obj_set_size(s_page_actions, ACTIONS_W, 44);
    lv_obj_set_style_bg_opa(s_page_actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_actions, 0, 0);
    lv_obj_set_style_radius(s_page_actions, 0, 0);
    lv_obj_set_style_pad_all(s_page_actions, 0, 0);
    lv_obj_remove_flag(s_page_actions, LV_OBJ_FLAG_SCROLLABLE);

    ui_divider_create(s_main, 0, 96, CONTENT_W);

    s_body = lv_obj_create(s_main);
    lv_obj_set_pos(s_body, 0, BODY_Y);
    lv_obj_set_size(s_body, CONTENT_W, BODY_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_style_pad_row(s_body, 8, 0);
    lv_obj_set_style_pad_bottom(s_body, 24, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLL_ONE);

    header_update();
    body_rebuild();
    status_refresh();

    s_timer = lv_timer_create(claws_timer_cb, 1000, NULL);
#if CLAW_UI_SELFTEST
    lv_timer_create(selftest_tab_cb, 1500, NULL);
#endif
}

static void claw_destroy(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    ui_text_dialog_close();
    ui_text_view_close();
    if (s_script_view_buf != NULL) {
        heap_caps_free(s_script_view_buf);
        s_script_view_buf = NULL;
    }
    s_root = NULL;
    s_claw_ready_seen = false;
    s_rail = NULL;
    s_nav_list = NULL;
    s_main = NULL;
    s_page_title = NULL;
    s_page_desc = NULL;
    s_page_actions = NULL;
    s_body = NULL;
    s_nav_count = 0;
    memset(s_nav, 0, sizeof(s_nav));
    s_chip_lua = NULL;
    s_chip_lua_label = NULL;
    s_chip_jobs = NULL;
    s_chip_jobs_label = NULL;
    s_chip_store = NULL;
    s_chip_store_label = NULL;
    s_lua_switch = NULL;
    s_task_cards_valid = false;
    s_no_tasks_label = NULL;
    s_pending_delete_row = NULL;
    s_script_rows = 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_chip_agent = NULL;
    s_chip_agent_label = NULL;
    s_agent_chip = NULL;
    s_agent_chip_label = NULL;
    s_agent_error_label = NULL;
    s_agent_reply_label = NULL;
    s_agent_stop_btn = NULL;
    s_agent_ui_enabled = false;
    s_agent_seen_worker = false;
    s_agent_chip_state = -1;
    s_agent_error_visible = false;
    s_agent_current_task_id[0] = '\0';
#endif
}

void page_claw_open_settings(void)
{
    ui_shell_show(&page_claw);
    set_tab(CLAW_TAB_SETTINGS);
}

const ui_page_t page_claw = {
    .id = "claw",
    .name = STR_NAV_CLAW,
    .symbol = LV_SYMBOL_LIST,
    .create = claw_create,
    .destroy = claw_destroy,
};
