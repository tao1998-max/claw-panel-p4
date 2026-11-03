/*
 * Claw page: chat, automation, workspace and local-agent settings.
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

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "ai_ui.h"
#include "claw_agent_mgr.h"
#include "claw_api.h"
#include "claw_cap.h"
#include "claw_memory.h"
#include "claw_port.h"
#include "claw_session_mgr.h"
#include "claw_settings.h"
#include "claw_tts.h"
#include "claw_storage.h"
#include "i18n.h"
#include "ui_shell.h"
#include "ui_widgets.h"

static const char *TAG = "ui.claw";

#define CLAW_UI_SELFTEST 0

/* Four top-level pages. Chat first when the local agent is compiled in. */
#define CLAW_TAB_AUTOMATION 0
#define CLAW_TAB_WORKSPACE 1
#define CLAW_TAB_SETTINGS 2
#if CONFIG_TEMPLATE_CLAW_AGENT
#define CLAW_TAB_CHAT 3
#define CLAW_TAB_COUNT 4
#define CLAW_DEFAULT_TAB CLAW_TAB_CHAT
#else
#define CLAW_TAB_COUNT 3
#define CLAW_DEFAULT_TAB CLAW_TAB_AUTOMATION
#endif

#define SCHEDULES_JSON_MAX 16384
#define SCRIPT_VIEW_MAX (64 * 1024)
#define AGENT_STATUS_MAX 768

/* Settings-style rail + detail panel. */
#define RAIL_ITEM_X 12
#define RAIL_ITEM_W (UI_RAIL_W - RAIL_ITEM_X * 2)   /* 248 */
#define RAIL_ITEM_H 54
#define RAIL_ITEM_PITCH 60
#define RAIL_FIRST_Y 16
#define DETAIL_X (UI_RAIL_W + UI_SPACE_M)           /* 288 */
#define DETAIL_W UI_DETAIL_W                         /* 688 */
#define INNER_W (DETAIL_W - UI_PANEL_PAD * 2)       /* 640 */
#define BODY_Y 110
#define BODY_H (UI_PAGE_H - BODY_Y - UI_PANEL_PAD)  /* 286 */
#define CHAT_ROOT_Y 44
#define CHAT_ROOT_H (UI_PAGE_H - CHAT_ROOT_Y - UI_PANEL_PAD) /* 352 */

static lv_obj_t *s_root;
static lv_obj_t *s_rail;
static lv_obj_t *s_detail;
static lv_obj_t *s_body;
static lv_timer_t *s_timer;
static int s_tab;
static bool s_claw_ready_seen;

/* Rail footer: live agent / Lua / job status. */
static lv_obj_t *s_footer_agent_dot;
static lv_obj_t *s_footer_agent_label;
static lv_obj_t *s_footer_lua_dot;
static lv_obj_t *s_footer_lua_label;
static lv_obj_t *s_footer_jobs_dot;
static lv_obj_t *s_footer_jobs_label;

typedef struct {
    int tab;
    lv_obj_t *row;
    lv_obj_t *bar;
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
static bool s_agent_cfg_error;
#endif

static void build_detail(void);
static void set_tab(int tab);
static void label_set_if_changed(lv_obj_t *label, const char *text);
static const lv_font_t *claw_dyn_font(const lv_font_t *fallback);
static void chat_build(void);
#if CONFIG_TEMPLATE_CLAW_AGENT
static void chat_picker_close(void);
#endif
static void schedules_build(void);
static void workspace_build(void);
static void settings_build(void);
static void tts_picker_close(void);

/* ---------- helpers ---------- */

static const lv_font_t *claw_dyn_font(const lv_font_t *fallback)
{
    const lv_font_t *font = ai_ui_text_font();
    return font != NULL ? font : fallback;
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

/* ---------- rail footer status ---------- */

typedef struct {
    bool lua;
    int jobs;
#if CONFIG_TEMPLATE_CLAW_AGENT
    bool agent_enabled;
    bool agent_configured;
    bool agent_ready;
    bool agent_busy;
#endif
} claw_status_t;

static void status_read(claw_status_t *st)
{
    memset(st, 0, sizeof(*st));
    char text[512] = {0};
    if (claw_api_status_json(text, sizeof(text)) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        return;
    }
    cJSON *jobs = cJSON_GetObjectItem(root, "async_jobs");
    st->lua = cJSON_IsTrue(cJSON_GetObjectItem(root, "lua_ai"));
    st->jobs = cJSON_IsNumber(jobs) ? jobs->valueint : 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    st->agent_enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent"));
    st->agent_configured = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_configured"));
    st->agent_ready = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_ready"));
    st->agent_busy = cJSON_IsTrue(cJSON_GetObjectItem(root, "agent_busy"));
#endif
    cJSON_Delete(root);
}

static void status_refresh(void)
{
    if (s_root == NULL) {
        return;
    }
    claw_status_t st;
    status_read(&st);

    if (s_footer_lua_label != NULL) {
        label_set_if_changed(s_footer_lua_label,
                             i18n_str(st.lua ? STR_CLAW_CLOUD_ON : STR_CLAW_CLOUD_OFF));
    }
    if (s_footer_lua_dot != NULL) {
        ui_dot_set_color(s_footer_lua_dot, st.lua ? UI_COLOR_OK : UI_COLOR_DISABLED);
    }

#if CONFIG_TEMPLATE_CLAW_AGENT
    if (s_footer_agent_label != NULL) {
        label_set_if_changed(s_footer_agent_label,
                             claw_agent_state_text(st.agent_enabled, st.agent_configured,
                                                   st.agent_ready, st.agent_busy));
    }
    if (s_footer_agent_dot != NULL) {
        uint32_t color = UI_COLOR_DISABLED;
        if (st.agent_enabled && !st.agent_configured) color = UI_COLOR_WARN;
        else if (st.agent_enabled && st.agent_busy) color = UI_COLOR_ACCENT;
        else if (st.agent_enabled && st.agent_ready) color = UI_COLOR_OK;
        ui_dot_set_color(s_footer_agent_dot, color);
    }
#endif

    if (s_footer_jobs_label != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %d", i18n_str(STR_CLAW_JOBS), st.jobs);
        label_set_if_changed(s_footer_jobs_label, buf);
    }
    if (s_footer_jobs_dot != NULL) {
        ui_dot_set_color(s_footer_jobs_dot, st.jobs > 0 ? UI_COLOR_ACCENT : UI_COLOR_DISABLED);
    }
}

/* ---------- rail / detail shell ---------- */

typedef struct {
    int tab;
    i18n_id_t label;
    const char *symbol;
} claw_nav_def_t;

static const claw_nav_def_t s_nav_defs[] = {
#if CONFIG_TEMPLATE_CLAW_AGENT
    { CLAW_TAB_CHAT, STR_CLAW_NAV_CHAT, LV_SYMBOL_EDIT },
#endif
    { CLAW_TAB_AUTOMATION, STR_CLAW_TAB_AUTOMATION, LV_SYMBOL_REFRESH },
    { CLAW_TAB_WORKSPACE, STR_CLAW_WORKSPACE, LV_SYMBOL_DIRECTORY },
    { CLAW_TAB_SETTINGS, STR_CLAW_TAB_SETTINGS, LV_SYMBOL_SETTINGS },
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
        lv_obj_t *row = s_nav[i].row;
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ACCENT_DEEP), 0);
        lv_obj_set_style_bg_grad_color(row, lv_color_hex(UI_COLOR_VIOLET), 0);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(row, on ? UI_OPA_SOFT : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(row, on ? 1 : 0, 0);
        lv_obj_set_style_border_opa(row, on ? 50 : 0, 0);
        lv_obj_set_style_bg_opa(s_nav[i].bar, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        ui_badge_set_color(s_nav[i].icon, on ? UI_COLOR_ACCENT : UI_COLOR_MUTED);
        ui_label_set_text_color(s_nav[i].label,
                                on ? UI_COLOR_TEXT : UI_COLOR_MUTED);
    }
}

static void set_tab(int tab)
{
    if (tab < 0 || tab >= CLAW_TAB_COUNT || tab == s_tab) {
        return;
    }
    s_tab = tab;
    s_sched_msg[0] = '\0';
    s_sched_msg_error = false;
    nav_update();
    build_detail();
}

static void nav_build(void)
{
    s_rail = ui_panel_create(s_root, 0, 0, UI_RAIL_W, UI_PAGE_H);

    s_nav_count = (int)(sizeof(s_nav_defs) / sizeof(s_nav_defs[0]));
    for (int i = 0; i < s_nav_count; i++) {
        int y = RAIL_FIRST_Y + i * RAIL_ITEM_PITCH;
        lv_obj_t *row = lv_obj_create(s_rail);
        lv_obj_set_pos(row, RAIL_ITEM_X, y);
        lv_obj_set_size(row, RAIL_ITEM_W, RAIL_ITEM_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, nav_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_nav_defs[i].tab);

        lv_obj_t *bar = lv_obj_create(row);
        lv_obj_set_pos(bar, 8, (RAIL_ITEM_H - 24) / 2);
        lv_obj_set_size(bar, 3, 24);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

        s_nav[i].tab = s_nav_defs[i].tab;
        s_nav[i].row = row;
        s_nav[i].bar = bar;
        s_nav[i].icon = ui_badge_create(row, 20, (RAIL_ITEM_H - 32) / 2, 32,
                                        s_nav_defs[i].symbol, UI_COLOR_MUTED);
        s_nav[i].label = ui_label_create(row, i18n_str(s_nav_defs[i].label), 62,
                                         (RAIL_ITEM_H - 20) / 2,
                                         UI_COLOR_MUTED, UI_FONT_BODY);
    }
    nav_update();

    /* Fixed footer: live Agent / Lua / job state, replacing the old top chips. */
    ui_divider_create(s_rail, 20, 272, UI_RAIL_W - 40);
    ui_label_create(s_rail, i18n_str(STR_CLAW_SEC_STATUS_RUN), 20, 284,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    int y = 306;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_footer_agent_dot = ui_dot_create(s_rail, 20, y + 4, 8);
    s_footer_agent_label = ui_label_create(s_rail, "", 36, y,
                                           UI_COLOR_TEXT, UI_FONT_SMALL);
    y += 26;
#endif
    s_footer_lua_dot = ui_dot_create(s_rail, 20, y + 4, 8);
    s_footer_lua_label = ui_label_create(s_rail, "", 36, y,
                                         UI_COLOR_TEXT, UI_FONT_SMALL);
    y += 26;
    s_footer_jobs_dot = ui_dot_create(s_rail, 20, y + 4, 8);
    s_footer_jobs_label = ui_label_create(s_rail, "", 36, y,
                                          UI_COLOR_TEXT, UI_FONT_SMALL);
}

static void detail_header(i18n_id_t title, i18n_id_t desc,
                          const char *symbol, uint32_t color)
{
    ui_badge_create(s_detail, 24, 24, 48, symbol, color);
    lv_obj_t *t = ui_label_create(s_detail, i18n_str(title), 88, 24,
                                  UI_COLOR_TEXT, UI_FONT_TITLE);
    lv_obj_set_width(t, INNER_W - 64);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_t *d = ui_label_create(s_detail, i18n_str(desc), 88, 62,
                                  UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(d, INNER_W - 64);
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    ui_divider_create(s_detail, 24, 96, INNER_W);
}

static lv_obj_t *detail_body_create(bool scrollable)
{
    lv_obj_t *body = lv_obj_create(s_detail);
    lv_obj_set_pos(body, UI_PANEL_PAD, BODY_Y);
    lv_obj_set_size(body, INNER_W, BODY_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_style_pad_bottom(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    if (scrollable) {
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_ONE);
    } else {
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    }
    s_body = body;
    return body;
}

static void build_detail(void)
{
    if (s_detail == NULL) {
        return;
    }
    ui_text_view_close();
    tts_picker_close();
#if CONFIG_TEMPLATE_CLAW_AGENT
    chat_picker_close();
#endif
    lv_obj_clean(s_detail);
    s_body = NULL;

    if (!claw_port_is_ready()) {
        ui_label_create(s_detail, i18n_str(STR_CLAW_CLAW_UNAVAILABLE),
                        24, 24, UI_COLOR_MUTED, UI_FONT_BODY);
        return;
    }

    switch (s_tab) {
#if CONFIG_TEMPLATE_CLAW_AGENT
    case CLAW_TAB_CHAT:
        chat_build();
        break;
#endif
    case CLAW_TAB_AUTOMATION:
        schedules_build();
        break;
    case CLAW_TAB_WORKSPACE:
        workspace_build();
        break;
    case CLAW_TAB_SETTINGS:
        settings_build();
        break;
    default:
        break;
    }
}

/* Rebuild while keeping the outer body scroll position (automation actions). */
static void body_rebuild_keep_scroll(void)
{
    int32_t y = s_body != NULL ? lv_obj_get_scroll_y(s_body) : 0;
    build_detail();
    if (s_body != NULL) {
        lv_obj_update_layout(s_body);
        lv_obj_scroll_to_y(s_body, y, LV_ANIM_OFF);
    }
}

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
static void script_view_close_cb(void)
{
    heap_caps_free(s_script_view_buf);
    s_script_view_buf = NULL;
}

static void format_size(long size, char *out, size_t out_size)
{
    if (size < 1024) {
        snprintf(out, out_size, "%ld B", size);
    } else if (size < 1024L * 1024L) {
        snprintf(out, out_size, "%ld.%ld KB",
                 size / 1024, (size % 1024) * 10 / 1024);
    } else {
        snprintf(out, out_size, "%ld.%ld MB",
                 size / (1024L * 1024L),
                 (size % (1024L * 1024L)) * 10 / (1024L * 1024L));
    }
}

/* ---------- workspace (memory + files) ---------- */

#define WORKSPACE_SEG_MEMORY 0
#define WORKSPACE_SEG_FILES 1

static int s_workspace_seg;
static lv_obj_t *s_workspace_pane;
static char s_files_dir[192] = CLAW_STORAGE_DATA_DIR;
static int s_files_volume;   /* 0 = FAT 内部存储, 1 = SD 卡 */

static const char *files_volume_root(void)
{
    return s_files_volume == 1 ? BSP_SD_MOUNT_POINT
                               : CLAW_STORAGE_MOUNT_POINT "/claw";
}

static void workspace_pane_rebuild(void);
static void workspace_build(void);
static void memory_pane_build(void);
static void files_pane_build(void);

static void workspace_trim_utf8(const char *buf, size_t *len)
{
    size_t n = *len;
    while (n > 0 && (((unsigned char)buf[n - 1] & 0xC0) == 0x80)) {
        n--;
    }
    if (n > 0) {
        unsigned char lead = (unsigned char)buf[n - 1];
        size_t need = 1;
        if ((lead & 0xF8) == 0xF0) need = 4;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xE0) == 0xC0) need = 2;
        if (need > 1 && *len < n - 1 + need) {
            n--;
        }
    }
    *len = n;
}

static void workspace_view_file(const char *path, const char *title)
{
    ui_text_view_close();
    char *buf = NULL;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        long size = -1;
        if (fseek(f, 0, SEEK_END) == 0) {
            size = ftell(f);
        }
        if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            size_t cap = (size_t)size;
            bool truncated = cap > SCRIPT_VIEW_MAX;
            if (truncated) {
                cap = SCRIPT_VIEW_MAX;
            }
            const char *note = truncated
                               ? i18n_str(STR_CLAW_FILE_TOO_LARGE) : NULL;
            size_t note_len = note != NULL ? strlen(note) + 2 : 0;
            buf = heap_caps_malloc(cap + note_len + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf != NULL) {
                size_t got = fread(buf, 1, cap, f);
                if (truncated) {
                    workspace_trim_utf8(buf, &got);
                }
                if (note != NULL) {
                    buf[got++] = '\n';
                    buf[got++] = '\n';
                    memcpy(buf + got, note, strlen(note));
                    got += strlen(note);
                }
                buf[got] = '\0';
            }
        }
        fclose(f);
    }
    if (buf == NULL) {
        const char *msg = i18n_str(STR_CLAW_FILE_OPEN_FAILED);
        size_t n = strlen(msg);
        buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            return;
        }
        memcpy(buf, msg, n + 1);
    }
    if (s_script_view_buf != NULL) {
        heap_caps_free(s_script_view_buf);
    }
    s_script_view_buf = buf;
    ui_text_view_open(title, buf, claw_dyn_font(UI_FONT_SMALL),
                      script_view_close_cb);
}

typedef struct {
    char path[192];
    char title[64];
} workspace_ref_t;

static void workspace_memory_clicked(lv_event_t *e)
{
    workspace_ref_t *ref = lv_event_get_user_data(e);
    if (ref == NULL) return;
    workspace_view_file(ref->path, ref->title);
}

static void memory_pane_build(void)
{
    static const struct {
        const char *path;
        i18n_id_t title;
    } files[] = {
        { CLAW_STORAGE_DATA_DIR "/agent/memory/MEMORY.md", STR_CLAW_MEMORY_LONG },
        { CLAW_STORAGE_DATA_DIR "/agent/memory/soul.md", STR_CLAW_MEMORY_SOUL },
        { CLAW_STORAGE_DATA_DIR "/agent/memory/identity.md", STR_CLAW_MEMORY_IDENTITY },
        { CLAW_STORAGE_DATA_DIR "/agent/memory/user.md", STR_CLAW_MEMORY_USER },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        struct stat st;
        bool exists = stat(files[i].path, &st) == 0 && S_ISREG(st.st_mode);
        lv_obj_t *row = ui_list_row_create(s_workspace_pane, 0, 0, INNER_W, 52);
        if (row == NULL) continue;
        ui_label_create(row, i18n_str(files[i].title), 16, 6,
                        UI_COLOR_TEXT, claw_dyn_font(UI_FONT_BODY));
        lv_obj_t *path = ui_label_create(row, files[i].path, 16, 28,
                                         UI_COLOR_MUTED,
                                         claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(path, 390);
        lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
        char size_text[32] = "--";
        if (exists) {
            format_size((long)st.st_size, size_text, sizeof(size_text));
        }
        lv_obj_t *size = ui_label_create(row, size_text, 430, 16,
                                         exists ? UI_COLOR_OK : UI_COLOR_DISABLED,
                                         UI_FONT_SMALL);
        lv_obj_set_width(size, 190);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
        if (exists) {
            workspace_ref_t *ref = calloc(1, sizeof(*ref));
            if (ref == NULL) continue;
            strlcpy(ref->path, files[i].path, sizeof(ref->path));
            strlcpy(ref->title, i18n_str(files[i].title), sizeof(ref->title));
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, workspace_memory_clicked,
                                LV_EVENT_CLICKED, ref);
            lv_obj_add_event_cb(row, free_user_data_cb, LV_EVENT_DELETE, ref);
        }
    }
    ui_label_create(s_workspace_pane, i18n_str(STR_CLAW_WEB_HINT), 0, 0,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
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

static void files_volume_seg_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target(e);
    int volume = ui_segmented_get_selected(seg) == 1 ? 1 : 0;
    if (volume == s_files_volume) {
        return;
    }
    s_files_volume = volume;
    strlcpy(s_files_dir, files_volume_root(), sizeof(s_files_dir));
    workspace_pane_rebuild();
}

static void workspace_file_clicked(lv_event_t *e)
{
    claw_file_ref_t *ref = lv_event_get_user_data(e);
    if (ref == NULL) return;
    if (ref->is_dir) {
        strlcpy(s_files_dir, ref->path, sizeof(s_files_dir));
        workspace_pane_rebuild();
    } else {
        workspace_view_file(ref->path, ref->name);
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
    workspace_pane_rebuild();
}

static void files_refresh_clicked(lv_event_t *e)
{
    (void)e;
    workspace_pane_rebuild();
}

static void files_pane_build(void)
{
    lv_obj_t *toolbar = lv_obj_create(s_workspace_pane);
    lv_obj_set_size(toolbar, INNER_W, 44);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_radius(toolbar, 0, 0);
    lv_obj_set_style_pad_all(toolbar, 0, 0);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

    const char *vol_labels[2] = {
        i18n_str(STR_CLAW_FILES_FAT), i18n_str(STR_CLAW_FILES_SD)
    };
    lv_obj_t *vol = ui_segmented_create(toolbar, 0, 6, 210, 32, vol_labels, 2,
                                        s_files_volume);
    if (vol != NULL) {
        lv_obj_add_event_cb(vol, files_volume_seg_changed,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_t *path = ui_label_create(toolbar, s_files_dir, 222, 10,
                                     UI_COLOR_MUTED,
                                     claw_dyn_font(UI_FONT_SMALL));
    lv_obj_set_width(path, 244);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_t *up = ui_button_create(toolbar, i18n_str(STR_CLAW_UP), 474, 6,
                                    76, 32, 0);
    if (up != NULL) {
        lv_obj_add_event_cb(up, files_up_clicked, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *refresh = ui_button_create(toolbar, i18n_str(STR_CLAW_REFRESH),
                                         556, 6, 76, 32, 0);
    if (refresh != NULL) {
        lv_obj_add_event_cb(refresh, files_refresh_clicked, LV_EVENT_CLICKED, NULL);
    }

    if (s_files_volume == 1) {
        struct stat st;
        if (stat(BSP_SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ui_label_create(s_workspace_pane, i18n_str(STR_CLAW_SD_NOT_MOUNTED),
                            0, 0, UI_COLOR_MUTED, UI_FONT_BODY);
            return;
        }
    }

    DIR *dir = opendir(s_files_dir);
    if (dir == NULL) {
        ui_label_create(s_workspace_pane, i18n_str(STR_CLAW_FILES_EMPTY), 0, 0,
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
            char path_full[320];
            int n = snprintf(path_full, sizeof(path_full), "%s/%s",
                             s_files_dir, item->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path_full)) {
                continue;
            }
            struct stat st;
            if (stat(path_full, &st) != 0) {
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
        ui_label_create(s_workspace_pane, i18n_str(STR_CLAW_FILES_EMPTY), 0, 0,
                        UI_COLOR_MUTED, UI_FONT_BODY);
        heap_caps_free(entries);
        return;
    }
    qsort(entries, count, sizeof(*entries), claw_file_cmp);

    for (size_t i = 0; i < count; i++) {
        lv_obj_t *row = ui_list_row_create(s_workspace_pane, 0, 0, INNER_W, 48);
        if (row == NULL) continue;
        ui_label_create(row,
                        entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                        16, 14, entries[i].is_dir ? UI_COLOR_ACCENT : UI_COLOR_MUTED,
                        UI_FONT_ICON_SMALL);
        lv_obj_t *name = ui_label_create(row, entries[i].name, 48, 6,
                                         UI_COLOR_TEXT,
                                         claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(name, 380);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char size_text[32] = "";
        if (!entries[i].is_dir) {
            format_size(entries[i].size, size_text, sizeof(size_text));
        }
        lv_obj_t *size = ui_label_create(row, size_text, 440, 14,
                                         UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_width(size, 180);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);

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
        lv_obj_add_event_cb(row, workspace_file_clicked, LV_EVENT_CLICKED, ref);
        lv_obj_add_event_cb(row, free_user_data_cb, LV_EVENT_DELETE, ref);
    }
    heap_caps_free(entries);
}

static void workspace_pane_rebuild(void)
{
    if (s_workspace_pane == NULL) {
        return;
    }
    lv_obj_clean(s_workspace_pane);
    if (s_workspace_seg == WORKSPACE_SEG_MEMORY) {
        memory_pane_build();
    } else {
        files_pane_build();
    }
}

static void workspace_seg_changed(lv_event_t *e)
{
    lv_obj_t *seg = lv_event_get_target(e);
    s_workspace_seg = ui_segmented_get_selected(seg) == 1
                      ? WORKSPACE_SEG_FILES : WORKSPACE_SEG_MEMORY;
    workspace_pane_rebuild();
}

static void workspace_build(void)
{
    detail_header(STR_CLAW_WORKSPACE, STR_CLAW_DESC_WORKSPACE,
                  LV_SYMBOL_DIRECTORY, UI_COLOR_CYAN);
    lv_obj_t *body = detail_body_create(false);

    const char *labels[2] = {
        i18n_str(STR_CLAW_NAV_MEMORY), i18n_str(STR_CLAW_NAV_FILES)
    };
    lv_obj_t *seg = ui_segmented_create(body, 0, 0, 320, 36, labels, 2,
                                        s_workspace_seg);
    if (seg != NULL) {
        lv_obj_add_event_cb(seg, workspace_seg_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    s_workspace_pane = lv_obj_create(body);
    lv_obj_set_width(s_workspace_pane, lv_pct(100));
    lv_obj_set_height(s_workspace_pane, 0);
    lv_obj_set_flex_grow(s_workspace_pane, 1);
    lv_obj_set_style_bg_opa(s_workspace_pane, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_workspace_pane, 0, 0);
    lv_obj_set_style_radius(s_workspace_pane, 0, 0);
    lv_obj_set_style_pad_all(s_workspace_pane, 0, 0);
    lv_obj_set_style_pad_row(s_workspace_pane, 8, 0);
    lv_obj_set_flex_flow(s_workspace_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_workspace_pane, LV_DIR_VER);
    lv_obj_add_flag(s_workspace_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_workspace_pane, LV_SCROLLBAR_MODE_AUTO);
    workspace_pane_rebuild();
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
    strlcpy(s_sched_msg,
            i18n_str(err == ESP_OK
                     ? (enabled ? STR_CLAW_ACTION_ENABLED : STR_CLAW_ACTION_DISABLED)
                     : STR_CLAW_ACTION_FAILED),
            sizeof(s_sched_msg));
    s_sched_msg_error = err != ESP_OK;
    body_rebuild_keep_scroll();
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
    strlcpy(s_sched_msg,
            i18n_str(err == ESP_OK ? STR_CLAW_ACTION_TRIGGERED
                                   : STR_CLAW_ACTION_FAILED),
            sizeof(s_sched_msg));
    s_sched_msg_error = err != ESP_OK;
    body_rebuild_keep_scroll();
}

static void schedule_delete_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_pending_schedule[0] == '\0') return;
    char out[512] = {0};
    esp_err_t err = claw_api_remove_schedule(s_pending_schedule, out, sizeof(out));
    ESP_LOGI(TAG, "remove schedule %s -> %s %s", s_pending_schedule,
             esp_err_to_name(err), out);
    strlcpy(s_sched_msg,
            i18n_str(err == ESP_OK ? STR_CLAW_ACTION_DELETED
                                   : STR_CLAW_ACTION_FAILED),
            sizeof(s_sched_msg));
    s_sched_msg_error = err != ESP_OK;
    s_pending_schedule[0] = '\0';
    body_rebuild_keep_scroll();
}

static void schedule_delete_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) return;
    strlcpy(s_pending_schedule, id, sizeof(s_pending_schedule));
    ui_confirm_create(lv_screen_active(), i18n_str(STR_CLAW_DELETE_CONFIRM),
                      schedule_delete_confirm_cb);
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
    detail_header(STR_CLAW_TAB_AUTOMATION, STR_CLAW_DESC_SCHEDULES,
                  LV_SYMBOL_REFRESH, UI_COLOR_ACCENT);
    lv_obj_t *body = detail_body_create(true);

    lv_obj_t *hint = ui_label_create(body,
                                     i18n_str(STR_CLAW_SCHEDULE_CREATE_HINT),
                                     0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(hint, INNER_W);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    if (s_sched_msg[0] != '\0') {
        ui_label_create(body, s_sched_msg, 0, 0,
                        s_sched_msg_error ? UI_COLOR_ERROR : UI_COLOR_OK,
                        UI_FONT_SMALL);
    }

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

    int total = cJSON_GetArraySize(array);
    int shown = total > SCHEDULE_UI_ROWS_MAX ? SCHEDULE_UI_ROWS_MAX : total;
    if (total == 0) {
        ui_label_create(body, i18n_str(STR_CLAW_NO_SCHEDULES), 0, 0,
                        UI_COLOR_MUTED, UI_FONT_BODY);
    } else if (total > shown) {
        char limit[64];
        snprintf(limit, sizeof(limit), "%s %d / %d",
                 i18n_str(STR_CLAW_SCHEDULE_LIMIT), shown, total);
        ui_label_create(body, limit, 0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
    }

    s_schedule_row_count = 0;
    s_schedule_refresh_tick = 0;
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int index = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (index >= shown) {
            break;
        }
        index++;
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

        lv_obj_t *row = ui_list_row_create(body, 0, 0, INNER_W, 68);
        lv_obj_t *title = ui_label_create(row, id, 16, 8, UI_COLOR_TEXT,
                                          claw_dyn_font(UI_FONT_BODY));
        lv_obj_set_width(title, 280);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

        char detail[224];
        char countdown[48];
        uint32_t countdown_color = UI_COLOR_MUTED;
        schedule_detail_text(&current, now_ms, detail, sizeof(detail),
                             countdown, sizeof(countdown), &countdown_color);

        lv_obj_t *countdown_label = ui_label_create(row, countdown, 452, 8,
                                                    countdown_color,
                                                    claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(countdown_label, 172);
        lv_label_set_long_mode(countdown_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(countdown_label, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t *detail_label = ui_label_create(row, detail, 16, 36,
                                                 UI_COLOR_MUTED,
                                                 claw_dyn_font(UI_FONT_SMALL));
        lv_obj_set_width(detail_label, 276);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
        if (s_schedule_row_count < SCHEDULE_UI_ROWS_MAX) {
            current.detail_label = detail_label;
            current.countdown_label = countdown_label;
            s_schedule_rows[s_schedule_row_count++] = current;
        }

        lv_obj_t *trigger = ui_button_create(row,
                                             i18n_str(STR_CLAW_SCHEDULE_TRIGGER),
                                             300, 36, 96, 26, 1);
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
                                            404, 36, 96, 26, 0);
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
                                         508, 36, 96, 26, 2);
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

    int total = cJSON_GetArraySize(array);
    int shown = total > SCHEDULE_UI_ROWS_MAX ? SCHEDULE_UI_ROWS_MAX : total;
    if (shown != s_schedule_row_count) {
        cJSON_Delete(array);
        body_rebuild_keep_scroll();
        return;
    }

    bool rebuild = false;
    const cJSON *item = NULL;
    int index = 0;
    cJSON_ArrayForEach(item, array) {
        if (index >= shown) {
            break;
        }
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        if (id == NULL || strcmp(id, s_schedule_rows[index].id) != 0) {
            rebuild = true;
            break;
        }
        index++;
    }
    if (rebuild) {
        cJSON_Delete(array);
        body_rebuild_keep_scroll();
        return;
    }

    index = 0;
    cJSON_ArrayForEach(item, array) {
        if (index >= shown) {
            break;
        }
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
/* ---------- shared session chat ---------- */

static char s_chat_public_id[CLAW_SESSION_PUBLIC_ID_SIZE];
static char s_chat_session_id[CLAW_SESSION_MGR_ID_SIZE];
static char s_chat_error[128];
static uint32_t s_chat_sig;
static bool s_chat_sig_valid;
static char s_chat_state_last[16];
static lv_obj_t *s_chat_history;
static lv_obj_t *s_chat_selector_label;
static lv_obj_t *s_chat_state_chip;
static lv_obj_t *s_chat_state_label;
static lv_obj_t *s_chat_error_label;
static lv_obj_t *s_chat_picker;
static char s_chat_delete_id[CLAW_SESSION_PUBLIC_ID_SIZE];
static char s_chat_stream_text[4096];
static uint32_t s_chat_stream_rev;
static uint32_t s_chat_stream_rev_seen;
static bool s_chat_stream_active;
static lv_obj_t *s_chat_stream_label;
static uint32_t s_chat_prefix_sig;
static size_t s_chat_rendered_count;

static void chat_reload(void);
static void chat_update_state_chip(void);
static void chat_picker_close(void);
static void chat_picker_open(void);

static void agent_go_settings(lv_event_t *e)
{
    (void)e;
    set_tab(CLAW_TAB_SETTINGS);
}

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
    s_chat_sig_valid = false;
    s_chat_rendered_count = 0;
    s_chat_prefix_sig = 0;
    s_chat_stream_active = false;
    s_chat_stream_text[0] = '\0';
    s_chat_stream_label = NULL;
    s_chat_state_last[0] = '\0';
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

static uint32_t chat_hash_messages(cJSON *messages, size_t limit)
{
    uint32_t hash = 2166136261u;
    if (!cJSON_IsArray(messages)) {
        return hash;
    }
    size_t index = 0;
    cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        if (index++ >= limit) {
            break;
        }
        hash = chat_hash_text(hash,
                              cJSON_GetStringValue(cJSON_GetObjectItem(message, "role")));
        hash = chat_hash_text(hash,
                              cJSON_GetStringValue(cJSON_GetObjectItem(message, "text")));
        cJSON *seq = cJSON_GetObjectItem(message, "seq");
        if (cJSON_IsNumber(seq)) {
            hash = (hash ^ (uint32_t)seq->valuedouble) * 16777619u;
        }
    }
    return hash;
}

static uint32_t chat_history_sig(const cJSON *history)
{
    cJSON *messages = history != NULL ? cJSON_GetObjectItem(history, "messages") : NULL;
    return chat_hash_messages(messages, (size_t)-1);
}

static void chat_error_set(const char *text)
{
    strlcpy(s_chat_error, text != NULL ? text : "", sizeof(s_chat_error));
    if (s_chat_error_label == NULL) {
        return;
    }
    if (s_chat_error[0] == '\0') {
        lv_obj_add_flag(s_chat_error_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_chat_error_label, s_chat_error);
        lv_obj_remove_flag(s_chat_error_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool chat_send_done(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (!s_chat_public_id[0] && !chat_pick_or_create()) {
        chat_error_set(i18n_str(STR_CLAW_CHAT_SESSION_UNAVAILABLE));
        return true;
    }

    char message_id[33];
    snprintf(message_id, sizeof(message_id), "%08x%08x%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random(),
             (unsigned)esp_random(), (unsigned)esp_random());

    claw_core_message_receipt_t receipt = {0};
    esp_err_t err = claw_agent_mgr_post_session_message(
        s_chat_public_id, text, message_id, false, &receipt);
    chat_error_set(err == ESP_OK ? "" : esp_err_to_name(err));
    chat_reload();
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
    if (claw_session_mgr_catalog_create(&created) == ESP_OK &&
        chat_select_public_id(created.id)) {
        chat_error_set("");
    } else {
        chat_error_set(i18n_str(STR_CLAW_CHAT_SESSION_FAILED));
    }
    chat_picker_close();
    chat_reload();
}

static void chat_avatar_create(lv_obj_t *row, const char *glyph, uint32_t bg_color)
{
    lv_obj_t *avatar = lv_obj_create(row);
    lv_obj_set_size(avatar, 36, 36);
    lv_obj_set_style_radius(avatar, 8, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_set_style_pad_all(avatar, 0, 0);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(avatar);
    lv_label_set_text(label, glyph);
    lv_obj_set_style_text_font(label, claw_dyn_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
}

static lv_obj_t *chat_bubble_create(lv_obj_t *list, const char *text, bool user)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (!user) {
        chat_avatar_create(row, "爪", UI_COLOR_ACCENT_DEEP);
    }

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(user ? 0x2F9E63 : UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_left(bubble, 12, 0);
    lv_obj_set_style_pad_right(bubble, 12, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, claw_dyn_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_max_width(label, 480, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    if (user) {
        chat_avatar_create(row, "我", UI_COLOR_TILE);
    }
    return label;
}

static void chat_system_note(lv_obj_t *list, const char *text)
{
    lv_obj_t *label = lv_label_create(list);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, claw_dyn_font(UI_FONT_SMALL), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_pad_top(label, 4, 0);
    lv_obj_set_style_pad_bottom(label, 4, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void chat_stream_refresh(void)
{
    char text[sizeof(s_chat_stream_text)] = {0};
    uint32_t rev = 0;
    bool active = false;
    if (claw_api_agent_stream_snapshot(s_chat_session_id, text, sizeof(text),
                                       &rev, &active) == ESP_OK) {
        strlcpy(s_chat_stream_text, text, sizeof(s_chat_stream_text));
        s_chat_stream_rev = rev;
        s_chat_stream_active = active;
    } else {
        s_chat_stream_text[0] = '\0';
        s_chat_stream_rev = 0;
        s_chat_stream_active = false;
    }
}

static void chat_history_fill(cJSON *history)
{
    if (s_chat_history == NULL) {
        return;
    }
    bool near_bottom = lv_obj_get_scroll_bottom(s_chat_history) < 24;
    s_chat_stream_label = NULL;
    lv_obj_clean(s_chat_history);

    cJSON *messages = history != NULL ? cJSON_GetObjectItem(history, "messages") : NULL;
    if (!cJSON_IsArray(messages)) {
        messages = NULL;
    }
    cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(message, "text"));
        if (content == NULL || content[0] == '\0') {
            continue;
        }
        if (role != NULL && strcmp(role, "user") == 0) {
            chat_bubble_create(s_chat_history, content, true);
        } else if (role != NULL && strcmp(role, "assistant") == 0) {
            chat_bubble_create(s_chat_history, content, false);
        } else {
            chat_system_note(s_chat_history, content);
        }
    }
    if (s_chat_stream_active) {
        s_chat_stream_label = chat_bubble_create(
            s_chat_history,
            s_chat_stream_text[0] != '\0' ? s_chat_stream_text : "...",
            false);
    }
    s_chat_rendered_count = messages != NULL
                                ? (size_t)cJSON_GetArraySize(messages) : 0;
    s_chat_prefix_sig = chat_hash_messages(messages, s_chat_rendered_count);
    s_chat_sig = chat_history_sig(history);
    s_chat_sig_valid = true;

    if (lv_obj_get_child_count(s_chat_history) == 0) {
        lv_obj_t *empty = ui_label_create(s_chat_history,
                                          i18n_str(STR_CLAW_NO_MESSAGES),
                                          0, 0, UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_style_text_font(empty, claw_dyn_font(UI_FONT_SMALL), 0);
    } else if (near_bottom) {
        lv_obj_update_layout(s_chat_history);
        lv_obj_scroll_to_view(
            lv_obj_get_child(s_chat_history,
                             lv_obj_get_child_count(s_chat_history) - 1),
            LV_ANIM_OFF);
    }
}

/* Append only new messages. A full clean/rebuild on every send or final
 * message makes the panel flash for one frame. */
static void chat_history_sync(cJSON *history)
{
    if (s_chat_history == NULL) {
        return;
    }
    cJSON *messages = history != NULL
                          ? cJSON_GetObjectItem(history, "messages") : NULL;
    size_t count = cJSON_IsArray(messages)
                       ? (size_t)cJSON_GetArraySize(messages) : 0;
    bool near_bottom = lv_obj_get_scroll_bottom(s_chat_history) < 24;

    /* The temporary streaming bubble is replaced by the final committed
     * message once it appears. */
    if (!s_chat_stream_active && s_chat_stream_label != NULL &&
        count > s_chat_rendered_count) {
        lv_obj_t *row = lv_obj_get_parent(
            lv_obj_get_parent(s_chat_stream_label));
        if (row != NULL) {
            lv_obj_delete(row);
        }
        s_chat_stream_label = NULL;
        s_chat_stream_text[0] = '\0';
    }

    if (!s_chat_sig_valid || s_chat_rendered_count == 0 ||
        count < s_chat_rendered_count ||
        chat_hash_messages(messages, s_chat_rendered_count) !=
            s_chat_prefix_sig) {
        chat_history_fill(history);
        return;
    }
    if (count == s_chat_rendered_count && !s_chat_stream_active) {
        return;
    }

    for (size_t i = s_chat_rendered_count; i < count; i++) {
        cJSON *message = cJSON_GetArrayItem(messages, (int)i);
        const char *role = cJSON_GetStringValue(
            cJSON_GetObjectItem(message, "role"));
        const char *content = cJSON_GetStringValue(
            cJSON_GetObjectItem(message, "text"));
        if (content == NULL || content[0] == '\0') {
            continue;
        }
        if (role != NULL && strcmp(role, "user") == 0) {
            chat_bubble_create(s_chat_history, content, true);
        } else if (role != NULL && strcmp(role, "assistant") == 0) {
            chat_bubble_create(s_chat_history, content, false);
        } else {
            chat_system_note(s_chat_history, content);
        }
    }
    if (count > s_chat_rendered_count) {
        s_chat_rendered_count = count;
        s_chat_prefix_sig = chat_hash_messages(messages, count);
        s_chat_sig = chat_history_sig(history);
        s_chat_sig_valid = true;
    }

    if (s_chat_stream_active) {
        if (s_chat_stream_label == NULL) {
            s_chat_stream_label = chat_bubble_create(
                s_chat_history,
                s_chat_stream_text[0] != '\0' ? s_chat_stream_text : "...",
                false);
        } else {
            lv_obj_move_foreground(
                lv_obj_get_parent(lv_obj_get_parent(s_chat_stream_label)));
        }
    }

    if (near_bottom && lv_obj_get_child_count(s_chat_history) > 0) {
        lv_obj_update_layout(s_chat_history);
        lv_obj_scroll_to_view(
            lv_obj_get_child(s_chat_history,
                             lv_obj_get_child_count(s_chat_history) - 1),
            LV_ANIM_OFF);
    }
}

static void chat_update_selector(void)
{
    if (s_chat_selector_label == NULL) {
        return;
    }
    const char *text = s_chat_public_id;
    claw_session_info_t info = {0};
    if (s_chat_public_id[0] &&
        claw_session_mgr_catalog_get(s_chat_public_id, &info) == ESP_OK &&
        info.title[0]) {
        text = info.title;
    }
    lv_label_set_text(s_chat_selector_label, text);
}

static void chat_update_state_chip(void)
{
    if (s_chat_state_label == NULL || s_chat_state_chip == NULL) {
        return;
    }
    const char *state = claw_agent_mgr_session_state(s_chat_session_id);
    if (state == NULL) {
        state = "";
    }
    if (strcmp(s_chat_state_last, state) == 0) {
        return;
    }
    strlcpy(s_chat_state_last, state, sizeof(s_chat_state_last));

    bool running = strcmp(state, "running") == 0 || strcmp(state, "queued") == 0;
    const char *text = running ? "运行中" : "空闲";
    uint32_t color = running ? UI_COLOR_ACCENT : UI_COLOR_MUTED;
    lv_label_set_text(s_chat_state_label, text);
    lv_obj_set_style_text_color(s_chat_state_label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_chat_state_chip, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_chat_state_chip, lv_color_hex(color), 0);
}

static void chat_reload(void)
{
    if (s_chat_history == NULL || s_chat_session_id[0] == '\0') {
        return;
    }
    chat_update_selector();
    chat_update_state_chip();

    cJSON *history = NULL;
    if (claw_memory_read_session_messages(s_chat_session_id, 0, 30, &history) == ESP_OK) {
        chat_stream_refresh();
        s_chat_stream_rev_seen = s_chat_stream_rev;
        chat_history_sync(history);
        cJSON_Delete(history);
    }
}

static void chat_tick(void)
{
    if (s_chat_history == NULL || s_chat_session_id[0] == '\0') {
        return;
    }
    cJSON *history = NULL;
    if (claw_memory_read_session_messages(s_chat_session_id, 0, 30, &history) != ESP_OK) {
        return;
    }
    chat_stream_refresh();
    uint32_t sig = chat_history_sig(history);
    bool stream_changed = s_chat_stream_rev != s_chat_stream_rev_seen;
    if (!s_chat_sig_valid || sig != s_chat_sig) {
        s_chat_stream_rev_seen = s_chat_stream_rev;
        chat_history_sync(history);
    } else if (stream_changed && s_chat_stream_active &&
               s_chat_stream_label != NULL) {
        /* Update only the streaming bubble; rebuilding the whole list every
         * second makes the panel flash during long answers. */
        bool near_bottom = lv_obj_get_scroll_bottom(s_chat_history) < 24;
        lv_label_set_text(s_chat_stream_label,
                          s_chat_stream_text[0] != '\0'
                              ? s_chat_stream_text : "...");
        if (near_bottom) {
            lv_obj_update_layout(s_chat_history);
            lv_obj_scroll_to_view(s_chat_stream_label, LV_ANIM_OFF);
        }
        s_chat_stream_rev_seen = s_chat_stream_rev;
    } else if (stream_changed) {
        s_chat_stream_rev_seen = s_chat_stream_rev;
        chat_history_sync(history);
    }
    cJSON_Delete(history);
    chat_update_state_chip();
}

static void chat_picker_close(void)
{
    if (s_chat_picker != NULL) {
        lv_obj_delete(s_chat_picker);
        s_chat_picker = NULL;
    }
}

static void chat_picker_dismiss(lv_event_t *e)
{
    (void)e;
    chat_picker_close();
}

static void chat_selector_clicked(lv_event_t *e)
{
    (void)e;
    chat_picker_open();
}

static void chat_picker_select(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id != NULL && chat_select_public_id(id)) {
        chat_error_set("");
        chat_picker_close();
        chat_reload();
    }
}

static void chat_delete_confirmed(lv_event_t *e)
{
    (void)e;
    if (s_chat_delete_id[0] == '\0') {
        return;
    }
    bool deleting_current = strcmp(s_chat_delete_id, s_chat_public_id) == 0;
    esp_err_t err = claw_agent_mgr_delete_session(s_chat_delete_id);
    s_chat_delete_id[0] = '\0';
    chat_picker_close();
    if (err != ESP_OK) {
        chat_error_set(i18n_str(STR_CLAW_ACTION_FAILED));
        return;
    }

    chat_error_set("");
    if (deleting_current) {
        s_chat_public_id[0] = '\0';
        s_chat_session_id[0] = '\0';
        s_chat_sig_valid = false;
        s_chat_state_last[0] = '\0';
        if (!chat_pick_or_create()) {
            chat_error_set(i18n_str(STR_CLAW_CHAT_SESSION_UNAVAILABLE));
            return;
        }
    }
    chat_reload();
    chat_picker_open();
}

static void chat_picker_delete_clicked(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) {
        return;
    }
    strlcpy(s_chat_delete_id, id, sizeof(s_chat_delete_id));
    ui_confirm_create(lv_screen_active(), i18n_str(STR_CLAW_DELETE_CONFIRM),
                      chat_delete_confirmed);
}

static void chat_picker_open(void)
{
    chat_picker_close();

    lv_obj_t *scrim = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_size(scrim, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, chat_picker_dismiss, LV_EVENT_CLICKED, NULL);
    s_chat_picker = scrim;

    lv_obj_t *card = ui_card_create(scrim, 0, 0, 560, 400);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(card, 70, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    ui_badge_create(card, 20, 18, 36, LV_SYMBOL_EDIT, UI_COLOR_ACCENT);
    ui_label_create(card, i18n_str(STR_CLAW_SESSION_PICKER), 68, 22,
                    UI_COLOR_TEXT, UI_FONT_BODY);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_pos(list, 20, 72);
    lv_obj_set_size(list, 520, 248);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    cJSON *sessions = NULL;
    int shown = 0;
    if (claw_session_mgr_catalog_list("web", 0, 16, &sessions) == ESP_OK) {
        cJSON *items = cJSON_GetObjectItem(sessions, "items");
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            if (shown >= 16) break;
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "session"));
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
            if (id == NULL || id[0] == '\0') continue;
            lv_obj_t *row = ui_list_row_create(list, 0, 0, 520, 56);
            if (row == NULL) continue;
            bool selected = strcmp(id, s_chat_public_id) == 0;
            if (selected) {
                lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ACCENT), 0);
                lv_obj_set_style_bg_opa(row, UI_OPA_SOFT, 0);
                lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ACCENT), 0);
                lv_obj_set_style_border_opa(row, 60, 0);
            }
            const lv_font_t *name_font = claw_dyn_font(UI_FONT_SMALL);
            lv_obj_t *name = ui_label_create(row,
                                             (title != NULL && title[0]) ? title : id,
                                             16, 4, UI_COLOR_TEXT, name_font);
            lv_obj_set_size(name, 430, lv_font_get_line_height(name_font));
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_anim_duration(name, lv_anim_speed(40), 0);
            lv_obj_t *id_label = ui_label_create(row, id, 16, 32,
                                                 UI_COLOR_MUTED, UI_FONT_SMALL);
            lv_obj_set_size(id_label, 430, lv_font_get_line_height(UI_FONT_SMALL));
            lv_label_set_long_mode(id_label, LV_LABEL_LONG_DOT);
            char *id_copy = strdup(id);
            if (id_copy != NULL) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, chat_picker_select,
                                    LV_EVENT_CLICKED, id_copy);
                lv_obj_add_event_cb(row, free_user_data_cb,
                                    LV_EVENT_DELETE, id_copy);
            }

            lv_obj_t *del = ui_button_create(row, LV_SYMBOL_TRASH,
                                             462, 10, 40, 36, 2);
            if (del != NULL) {
                lv_obj_t *icon = lv_obj_get_child(del, 0);
                if (icon != NULL) {
                    lv_obj_set_style_text_font(icon, UI_FONT_ICON, 0);
                }
                char *del_id = strdup(id);
                if (del_id != NULL) {
                    lv_obj_add_event_cb(del, chat_picker_delete_clicked,
                                        LV_EVENT_CLICKED, del_id);
                    lv_obj_add_event_cb(del, free_user_data_cb,
                                        LV_EVENT_DELETE, del_id);
                }
            }
            shown++;
        }
    }
    cJSON_Delete(sessions);
    if (shown == 0) {
        ui_label_create(list, i18n_str(STR_CLAW_NO_SESSIONS), 0, 0,
                        UI_COLOR_MUTED, UI_FONT_BODY);
    }

    lv_obj_t *fresh = ui_button_create(card, i18n_str(STR_CLAW_NEW_SESSION),
                                       20, 336, 200, 44, 1);
    lv_obj_add_event_cb(fresh, chat_new_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close = ui_button_create(card, i18n_str(STR_AI_CLOSE),
                                       380, 336, 160, 44, 0);
    lv_obj_add_event_cb(close, chat_picker_dismiss, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(scrim);
}

static void chat_build(void)
{
    char text[AGENT_STATUS_MAX] = {0};
    if (claw_api_agent_status_json(text, sizeof(text)) != ESP_OK) {
        detail_header(STR_CLAW_NAV_CHAT, STR_CLAW_DESC_AGENT,
                      LV_SYMBOL_EDIT, UI_COLOR_ACCENT);
        lv_obj_t *body = detail_body_create(true);
        ui_label_create(body, i18n_str(STR_CLAW_AGENT_STATUS_UNAVAILABLE),
                        0, 0, UI_COLOR_MUTED, UI_FONT_BODY);
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
        detail_header(STR_CLAW_NAV_CHAT, STR_CLAW_DESC_AGENT,
                      LV_SYMBOL_EDIT, UI_COLOR_ACCENT);
        lv_obj_t *body = detail_body_create(true);
        lv_obj_t *card = ui_card_create(body, 0, 0, INNER_W, 168);
        ui_label_create(card,
                        i18n_str(enabled ? STR_CLAW_AGENT_UNCONFIGURED
                                         : STR_CLAW_AGENT_DISABLED),
                        20, 18, UI_COLOR_TEXT, UI_FONT_BODY);
        lv_obj_t *hint = ui_label_create(card,
                                         i18n_str(STR_CLAW_AGENT_DISABLED_HINT),
                                         20, 54, UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_width(hint, INNER_W - 40);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_t *btn = ui_button_create(card, i18n_str(STR_CLAW_AGENT_GO_SETTINGS),
                                         20, 112, 180, 40, 1);
        lv_obj_add_event_cb(btn, agent_go_settings, LV_EVENT_CLICKED, NULL);
        return;
    }

    if (!chat_pick_or_create()) {
        detail_header(STR_CLAW_NAV_CHAT, STR_CLAW_DESC_AGENT,
                      LV_SYMBOL_EDIT, UI_COLOR_ACCENT);
        lv_obj_t *body = detail_body_create(true);
        ui_label_create(body, i18n_str(STR_CLAW_CHAT_SESSION_UNAVAILABLE),
                        0, 0, UI_COLOR_ERROR, UI_FONT_BODY);
        return;
    }

    static bool s_font_logged;
    if (!s_font_logged) {
        ESP_LOGI(TAG, "chat history font: %s",
                 ai_ui_text_font() != NULL ? "puhui" : "subset");
        s_font_logged = true;
    }

    lv_obj_t *root = lv_obj_create(s_detail);
    lv_obj_set_pos(root, UI_PANEL_PAD, CHAT_ROOT_Y);
    lv_obj_set_size(root, INNER_W, CHAT_ROOT_H);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(root);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 40);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *selector = lv_button_create(top);
    lv_obj_set_height(selector, 40);
    lv_obj_set_flex_grow(selector, 1);
    lv_obj_set_style_radius(selector, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(selector, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(selector, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(selector, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(selector, 1, 0);
    const lv_font_t *selector_font = claw_dyn_font(UI_FONT_SMALL);
    s_chat_selector_label = ui_label_create(selector, "", 0, 0, UI_COLOR_TEXT,
                                            selector_font);
    lv_obj_set_size(s_chat_selector_label, 400,
                    lv_font_get_line_height(selector_font));
    lv_obj_set_style_text_align(s_chat_selector_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_chat_selector_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_chat_selector_label, lv_anim_speed(40), 0);
    lv_obj_align(s_chat_selector_label, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *chev = ui_label_create(selector, LV_SYMBOL_DOWN, 0, 0,
                                     UI_COLOR_MUTED, UI_FONT_ICON_SMALL);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(selector, chat_selector_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *fresh = ui_button_create(top, "+", 0, 0, 44, 40, 0);
    lv_obj_set_style_radius(fresh, 20, 0);
    lv_obj_add_event_cb(fresh, chat_new_clicked, LV_EVENT_CLICKED, NULL);

    s_chat_state_chip = ui_chip_create(top, "空闲", UI_COLOR_MUTED,
                                       &s_chat_state_label);
    lv_obj_set_style_text_font(s_chat_state_label, UI_FONT_SMALL, 0);

    s_chat_history = ui_card_create(root, 0, 0, INNER_W, 0);
    lv_obj_set_width(s_chat_history, lv_pct(100));
    lv_obj_set_height(s_chat_history, 0);
    lv_obj_set_flex_grow(s_chat_history, 1);
    lv_obj_set_flex_flow(s_chat_history, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_chat_history, 12, 0);
    lv_obj_set_style_pad_row(s_chat_history, 10, 0);
    lv_obj_set_scroll_dir(s_chat_history, LV_DIR_VER);
    lv_obj_add_flag(s_chat_history, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_chat_history, LV_SCROLLBAR_MODE_AUTO);

    s_chat_error_label = ui_label_create(root, s_chat_error, 0, 0,
                                         UI_COLOR_ERROR, UI_FONT_SMALL);
    lv_obj_set_width(s_chat_error_label, lv_pct(100));
    if (s_chat_error[0] == '\0') {
        lv_obj_add_flag(s_chat_error_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *composer = lv_obj_create(root);
    lv_obj_set_width(composer, lv_pct(100));
    lv_obj_set_height(composer, 48);
    lv_obj_set_style_bg_opa(composer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(composer, 0, 0);
    lv_obj_set_style_pad_all(composer, 0, 0);
    lv_obj_set_style_pad_column(composer, 8, 0);
    lv_obj_set_flex_flow(composer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(composer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *input = lv_obj_create(composer);
    lv_obj_set_height(input, 44);
    lv_obj_set_flex_grow(input, 1);
    lv_obj_set_style_radius(input, 22, 0);
    lv_obj_set_style_bg_color(input, lv_color_hex(UI_COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(UI_COLOR_BORDER_SOFT), 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_pad_left(input, 16, 0);
    lv_obj_set_style_pad_right(input, 16, 0);
    lv_obj_set_style_pad_top(input, 0, 0);
    lv_obj_set_style_pad_bottom(input, 0, 0);
    lv_obj_remove_flag(input, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *placeholder = lv_label_create(input);
    lv_label_set_text(placeholder, i18n_str(STR_CLAW_AGENT_ASK));
    lv_obj_set_style_text_font(placeholder, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(placeholder, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_align(placeholder, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(input, chat_send_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send = ui_button_create(composer, i18n_str(STR_CLAW_AGENT_SEND),
                                      0, 0, 80, 44, 1);
    lv_obj_set_style_radius(send, 22, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(0x2F9E63), 0);
    lv_obj_set_style_bg_grad_color(send, lv_color_hex(0x2F9E63), 0);
    lv_obj_set_style_bg_grad_dir(send, LV_GRAD_DIR_NONE, 0);
    lv_obj_t *send_label = lv_obj_get_child(send, 0);
    if (send_label != NULL) {
        lv_obj_set_style_text_color(send_label, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(send, chat_send_clicked, LV_EVENT_CLICKED, NULL);

    chat_reload();
}

static void agent_tick(void)
{
    chat_tick();
}

#endif /* CONFIG_TEMPLATE_CLAW_AGENT */


/* ---------- settings ---------- */

static lv_obj_t *s_lua_switch;
static lv_obj_t *s_set_agent_switch;
static lv_obj_t *s_set_agent_state;
static lv_obj_t *s_set_agent_error;
static lv_obj_t *s_set_url_value;
static lv_obj_t *s_set_model_value;
static lv_obj_t *s_set_key_value;

static lv_obj_t *s_set_tts_api_value;
static lv_obj_t *s_set_tts_app_value;
static lv_obj_t *s_set_tts_token_value;
static lv_obj_t *s_set_tts_resource_value;
static lv_obj_t *s_set_tts_speaker_value;
static lv_obj_t *s_set_tts_auto_switch;
static lv_obj_t *s_tts_picker;
static lv_obj_t *s_tts_picker_list;
static int s_tts_picker_cat;
static int s_tts_edit_field;

enum {
    TTS_EDIT_API_KEY = 1,
    TTS_EDIT_APP_ID,
    TTS_EDIT_TOKEN,
    TTS_EDIT_RESOURCE,
    TTS_EDIT_SPEAKER,
};

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
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        s_lua_switch = sw;
        ui_confirm_create(lv_screen_active(), i18n_str(STR_CLAW_ENABLE_CONFIRM),
                          lua_ai_confirm);
        return;
    }
    claw_settings_set_lua_ai(on);
    status_refresh();
}

#if CONFIG_TEMPLATE_CLAW_AGENT
static void settings_agent_refresh(void)
{
    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK) {
        return;
    }
    if (s_set_agent_state != NULL) {
        char text[192];
        if (!cfg.enabled) {
            snprintf(text, sizeof(text), "%s", i18n_str(STR_CLAW_AGENT_OFF));
        } else if (!claw_settings_agent_configured()) {
            snprintf(text, sizeof(text), "%s",
                     i18n_str(STR_CLAW_AGENT_UNCONFIGURED));
        } else {
            snprintf(text, sizeof(text), "%s · %s · %s",
                     i18n_str(STR_CLAW_AGENT_ON),
                     cfg.backend[0] ? cfg.backend : "-",
                     cfg.model[0] ? cfg.model : "-");
        }
        label_set_if_changed(s_set_agent_state, text);
    }
    if (s_set_agent_switch != NULL) {
        if (cfg.enabled) {
            lv_obj_add_state(s_set_agent_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_set_agent_switch, LV_STATE_CHECKED);
        }
    }
    if (s_set_agent_error != NULL) {
        if (s_agent_cfg_error) {
            lv_label_set_text(s_set_agent_error,
                              i18n_str(STR_CLAW_AGENT_NEED_CONFIG));
            lv_obj_remove_flag(s_set_agent_error, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_set_agent_error, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_set_url_value != NULL) {
        label_set_if_changed(s_set_url_value,
                             cfg.base_url[0] ? cfg.base_url
                                             : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_model_value != NULL) {
        label_set_if_changed(s_set_model_value,
                             cfg.model[0] ? cfg.model
                                          : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_key_value != NULL) {
        label_set_if_changed(s_set_key_value,
                             cfg.api_key[0] ? i18n_str(STR_CLAW_AGENT_KEY_SET)
                                            : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
}

static void agent_enable_confirm(lv_event_t *e)
{
    (void)e;
    char out[128] = {0};
    (void)claw_api_agent_set_enabled(true, out, sizeof(out));
    s_agent_cfg_error = false;
    settings_agent_refresh();
    status_refresh();
}

static void agent_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (on && !claw_settings_agent_enabled()) {
        if (!claw_settings_agent_configured()) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
            s_agent_cfg_error = true;
            settings_agent_refresh();
            return;
        }
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ui_confirm_create(lv_screen_active(),
                          i18n_str(STR_CLAW_AGENT_ENABLE_CONFIRM),
                          agent_enable_confirm);
        return;
    }

    char out[128] = {0};
    (void)claw_api_agent_set_enabled(on, out, sizeof(out));
    s_agent_cfg_error = false;
    settings_agent_refresh();
    status_refresh();
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
    settings_agent_refresh();
}

static bool agent_url_done(const char *text)
{
    if (claw_settings_set_agent_base_url(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    settings_agent_refresh();
    return true;
}

static bool agent_model_done(const char *text)
{
    if (claw_settings_set_agent_model(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    settings_agent_refresh();
    return true;
}

static bool agent_key_done(const char *text)
{
    if (claw_settings_set_agent_api_key(text) != ESP_OK) {
        return false;
    }
    claw_api_agent_settings_changed();
    settings_agent_refresh();
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

static void agent_menu_row(lv_obj_t *parent, int y, const char *title,
                           const char *value, lv_event_cb_t cb,
                           lv_obj_t **out_value)
{
    lv_obj_t *val_label = NULL;
    ui_menu_row_create(parent, 20, y, INNER_W - 40, 42,
                       LV_SYMBOL_EDIT, title, value, &val_label, cb, NULL);
    if (val_label != NULL) {
        lv_obj_set_width(val_label, 300);
        lv_label_set_long_mode(val_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(val_label, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (out_value != NULL) {
        *out_value = val_label;
    }
}
#endif /* CONFIG_TEMPLATE_CLAW_AGENT */

typedef struct {
    const char *id;
    const char *name;
} tts_voice_entry_t;

typedef struct {
    const char *label;
    const char *resource_id;
    const tts_voice_entry_t *items;
    size_t count;
} tts_voice_group_t;

static const tts_voice_entry_t s_tts_voices_10[] = {
    { "zh_female_shuangkuaisisi_moon_bigtts", "爽快思思 · 女声" },
    { "zh_female_wanwanxiaohe_moon_bigtts", "湾湾小何 · 女声" },
    { "zh_female_linjianvhai_moon_bigtts", "邻家女孩 · 女声" },
    { "zh_male_yangguangqingnian_moon_bigtts", "阳光青年 · 男声" },
    { "zh_male_jingqiangkanye_moon_bigtts", "京腔侃爷 · 男声" },
};

static const tts_voice_entry_t s_tts_voices_20_general[] = {
    { "zh_female_vv_uranus_bigtts", "Vivi · 女声" },
    { "zh_female_xiaohe_uranus_bigtts", "小何 · 女声" },
    { "zh_male_m191_uranus_bigtts", "云舟 · 男声" },
    { "zh_male_taocheng_uranus_bigtts", "小天 · 男声" },
    { "zh_male_liufei_uranus_bigtts", "刘飞 · 男声" },
    { "zh_female_sophie_uranus_bigtts", "魅力苏菲 · 女声" },
    { "zh_female_qingxinnvsheng_uranus_bigtts", "清新女声" },
    { "zh_female_tianmeixiaoyuan_uranus_bigtts", "甜美小源 · 女声" },
    { "zh_female_tianmeitaozi_uranus_bigtts", "甜美桃子 · 女声" },
    { "zh_female_shuangkuaisisi_uranus_bigtts", "爽快思思 · 女声" },
    { "zh_female_linjianvhai_uranus_bigtts", "邻家女孩 · 女声" },
    { "zh_male_shaonianzixin_uranus_bigtts", "少年梓辛/Brayan · 男声" },
    { "zh_male_linjiananhai_uranus_bigtts", "邻家男孩 · 男声" },
    { "zh_male_ruyaqingnian_uranus_bigtts", "儒雅青年 · 男声" },
    { "zh_male_wennuanahu_uranus_bigtts", "温暖阿虎/Alvin · 男声" },
    { "zh_male_naiqimengwa_uranus_bigtts", "奶气萌娃 · 男声" },
    { "zh_female_popo_uranus_bigtts", "婆婆 · 女声" },
    { "zh_female_gaolengyujie_uranus_bigtts", "高冷御姐 · 女声" },
    { "zh_male_aojiaobazong_uranus_bigtts", "傲娇霸总 · 男声" },
    { "zh_male_fanjuanqingnian_uranus_bigtts", "反卷青年 · 男声" },
    { "zh_female_wenroushunv_uranus_bigtts", "温柔淑女 · 女声" },
    { "zh_male_huolixiaoge_uranus_bigtts", "活力小哥 · 男声" },
    { "zh_female_mengyatou_uranus_bigtts", "萌丫头/Cutey · 女声" },
    { "zh_female_tiexinnvsheng_uranus_bigtts", "贴心女声/Candy · 女声" },
    { "zh_female_jitangmei_uranus_bigtts", "鸡汤妹妹/Hope · 女声" },
    { "zh_male_cixingjieshuonan_uranus_bigtts", "磁性解说男声/Morgan · 男声" },
    { "zh_male_liangsangmengzai_uranus_bigtts", "亮嗓萌仔 · 男声" },
    { "zh_female_kailangjiejie_uranus_bigtts", "开朗姐姐 · 女声" },
    { "zh_male_gaolengchenwen_uranus_bigtts", "高冷沉稳 · 男声" },
    { "zh_male_shenyeboke_uranus_bigtts", "深夜播客 · 男声" },
    { "zh_male_kailangdidi_uranus_bigtts", "开朗弟弟 · 男声" },
    { "zh_female_qinqienv_uranus_bigtts", "亲切女声" },
    { "zh_male_kuailexiaodong_uranus_bigtts", "快乐小东 · 男声" },
    { "zh_male_kailangxuezhang_uranus_bigtts", "开朗学长 · 男声" },
    { "zh_male_youyoujunzi_uranus_bigtts", "悠悠君子 · 男声" },
    { "zh_female_wenjingmaomao_uranus_bigtts", "文静毛毛 · 女声" },
    { "zh_female_zhixingnv_uranus_bigtts", "知性女声" },
    { "zh_male_qingshuangnanda_uranus_bigtts", "清爽男大 · 男声" },
    { "zh_male_yuanboxiaoshu_uranus_bigtts", "渊博小叔 · 男声" },
    { "zh_male_yangguangqingnian_uranus_bigtts", "阳光青年 · 男声" },
    { "zh_female_qingchezizi_uranus_bigtts", "清澈梓梓 · 女声" },
    { "zh_female_tianmeiyueyue_uranus_bigtts", "甜美悦悦 · 女声" },
    { "zh_female_xinlingjitang_uranus_bigtts", "心灵鸡汤 · 女声" },
    { "zh_male_wenrouxiaoge_uranus_bigtts", "温柔小哥 · 男声" },
    { "zh_female_roumeinvyou_uranus_bigtts", "柔美女友 · 女声" },
    { "zh_male_dongfanghaoran_uranus_bigtts", "东方浩然 · 男声" },
    { "zh_female_wenrouxiaoya_uranus_bigtts", "温柔小雅 · 女声" },
    { "zh_male_tiancaitongsheng_uranus_bigtts", "天才童声 · 男声" },
    { "zh_male_guanggaojieshuo_uranus_bigtts", "广告解说 · 男声" },
    { "zh_male_jieshuoxiaoming_uranus_bigtts", "解说小明 · 男声" },
    { "zh_female_tvbnv_uranus_bigtts", "TVB女声" },
    { "zh_male_yizhipiannan_uranus_bigtts", "译制片男 · 男声" },
    { "zh_female_qiaopinv_uranus_bigtts", "俏皮女声" },
    { "zh_female_jiaochuannv_uranus_bigtts", "娇喘女声" },
    { "zh_female_chanmeinv_uranus_bigtts", "谄媚女声" },
    { "zh_female_meilinvyou_uranus_bigtts", "魅力女友 · 女声" },
    { "zh_female_wenroumama_uranus_bigtts", "温柔妈妈 · 女声" },
    { "zh_female_peiqi_uranus_bigtts", "佩奇猪 · 女声" },
    { "zh_male_sunwukong_uranus_bigtts", "猴哥 · 男声" },
    { "zh_male_dayi_uranus_bigtts", "大壹 · 男声" },
    { "zh_female_mizai_uranus_bigtts", "黑猫侦探社咪仔 · 女声" },
    { "zh_female_jitangnv_uranus_bigtts", "鸡汤女 · 女声" },
    { "zh_female_liuchangnv_uranus_bigtts", "流畅女声" },
    { "zh_male_ruyayichen_uranus_bigtts", "儒雅逸辰 · 男声" },
    { "zh_female_xiaoxue_uranus_bigtts", "儿童绘本 · 女声" },
    { "zh_male_baqiqingshu_uranus_bigtts", "霸气青叔 · 男声" },
    { "zh_male_xuanyijieshuo_uranus_bigtts", "悬疑解说 · 男声" },
    { "zh_female_shaoergushi_uranus_bigtts", "少儿故事 · 女声" },
    { "ICL_uranus_zh_male_ruyagongzi_tob", "儒雅公子 · 男声" },
    { "ICL_uranus_zh_male_neiliancaijun_tob", "内敛才俊 · 男声" },
    { "ICL_uranus_zh_male_wennuanshaonian_tob", "温暖少年 · 男声" },
    { "zh_female_yingyujiaoxue_uranus_bigtts", "Tina老师 · 女声" },
};

static const tts_voice_entry_t s_tts_voices_20_role[] = {
    { "zh_female_cancan_uranus_bigtts", "知性灿灿 · 女声" },
    { "zh_female_sajiaoxuemei_uranus_bigtts", "撒娇学妹 · 女声" },
    { "zh_female_zhishuaiyingzi_uranus_bigtts", "直率英子 · 女声" },
    { "zh_male_silang_uranus_bigtts", "四郎 · 男声" },
    { "zh_male_qingcang_uranus_bigtts", "擎苍 · 男声" },
    { "zh_male_xionger_uranus_bigtts", "熊二 · 男声" },
    { "zh_female_yingtaowanzi_uranus_bigtts", "樱桃丸子 · 女声" },
    { "zh_male_lanyinmianbao_uranus_bigtts", "懒音绵宝 · 男声" },
    { "zh_female_gufengshaoyu_uranus_bigtts", "古风少御 · 女声" },
    { "zh_male_lubanqihao_uranus_bigtts", "鲁班七号 · 男声" },
    { "zh_female_linxiao_uranus_bigtts", "林潇 · 女声" },
    { "zh_female_lingling_uranus_bigtts", "玲玲姐姐 · 女声" },
    { "zh_female_chunribu_uranus_bigtts", "春日部姐姐 · 女声" },
    { "zh_male_tangseng_uranus_bigtts", "唐僧 · 男声" },
    { "zh_male_zhuangzhou_uranus_bigtts", "庄周 · 男声" },
    { "zh_male_zhubajie_uranus_bigtts", "猪八戒 · 男声" },
    { "zh_female_ganmaodianyin_uranus_bigtts", "感冒电音姐姐 · 女声" },
    { "zh_female_nvleishen_uranus_bigtts", "女雷神 · 女声" },
    { "zh_female_wuzetian_uranus_bigtts", "武则天 · 女声" },
    { "zh_female_gujie_uranus_bigtts", "顾姐 · 女声" },
};

static const tts_voice_entry_t s_tts_voices_20_service[] = {
    { "zh_female_kefunvsheng_uranus_bigtts", "暖阳女声" },
    { "ICL_uranus_zh_female_guaiqiaokeer_tob", "乖巧可儿 · 女声" },
    { "ICL_uranus_zh_female_kailangtingting_tob", "开朗婷婷 · 女声" },
    { "ICL_uranus_zh_female_kaixinxiaohong_tob", "开心小鸿 · 女声" },
    { "ICL_uranus_zh_female_lingdongxinxin_tob", "灵动欣欣 · 女声" },
    { "ICL_uranus_zh_female_lixingyuanzi_tob", "理性圆子 · 女声" },
    { "ICL_uranus_zh_female_nuanxinqianqian_tob", "暖心茜茜 · 女声" },
    { "ICL_uranus_zh_female_qingtianmeimei_tob", "清甜莓莓 · 女声" },
    { "ICL_uranus_zh_female_qingtiantaotao_tob", "清甜桃桃 · 女声" },
    { "ICL_uranus_zh_female_qingxixiaoxue_tob", "清晰小雪 · 女声" },
    { "ICL_uranus_zh_female_ruanmengtangtang_tob", "软萌糖糖 · 女声" },
    { "ICL_uranus_zh_female_ruanmengtuanzi_tob", "软萌团子 · 女声" },
    { "ICL_uranus_zh_female_tianmeixiaoju_tob", "甜美小橘 · 女声" },
    { "ICL_uranus_zh_female_tianmeixiaoyu_tob", "甜美小雨 · 女声" },
    { "ICL_uranus_zh_female_xiuliqianqian_tob", "秀丽倩倩 · 女声" },
    { "ICL_uranus_zh_male_chenwenmingzai_tob", "沉稳明仔 · 男声" },
    { "ICL_uranus_zh_male_qingxinbobo_tob", "清新波波 · 男声" },
    { "ICL_uranus_zh_male_qinqiexiaozhuo_tob", "亲切小卓 · 男声" },
    { "ICL_uranus_zh_male_shuanglangxiaoyang_tob", "爽朗小阳 · 男声" },
    { "ICL_uranus_zh_male_yangguangyangyang_tob", "阳光洋洋 · 男声" },
    { "ICL_uranus_zh_male_qingxinmumu_tob", "清新沐沐 · 男声" },
    { "ICL_uranus_zh_female_wenwanshanshan_tob", "温婉珊珊 · 女声" },
    { "ICL_uranus_zh_female_reqingaina_tob", "热情艾娜 · 女声" },
    { "ICL_uranus_zh_female_qingyingduoduo_tob", "轻盈朵朵 · 女声" },
};

static const tts_voice_entry_t s_tts_voices_20_multi[] = {
    { "ICL_uranus_en_female_charlie_tob", "Charlie · 女声" },
    { "ICL_uranus_en_male_ethan_tob", "Ethan · 男声" },
    { "ICL_uranus_en_male_alastor_tob", "Alastor · 男声" },
    { "ICL_uranus_en_male_chucky_tob", "Chucky · 男声" },
    { "ICL_uranus_en_male_noah_tob", "Noah · 男声" },
    { "ICL_uranus_en_male_jigsaw_tob", "Jigsaw · 男声" },
    { "ICL_uranus_en_male_clown_man_tob", "Clown Man · 男声" },
    { "ICL_uranus_en_male_cartoon_chef_tob", "Cartoon Chef · 男声" },
    { "ICL_uranus_en_male_frosty_man_tob", "Frosty Man · 男声" },
    { "ICL_uranus_en_male_the_grinch_tob", "The Grinch · 男声" },
    { "ICL_uranus_en_male_kevin_mccallister_tob", "Kevin McCallister · 男声" },
    { "ICL_uranus_en_male_michael_tob", "Michael · 男声" },
    { "ICL_uranus_en_male_big_boogie_tob", "Big Boogie · 男声" },
    { "ICL_uranus_en_male_xavier_tob", "Xavier · 男声" },
    { "ICL_uranus_en_male_zayne_tob", "Zayne · 男声" },
};

static const tts_voice_entry_t s_tts_voices_20_icl[] = {
    { "ICL_uranus_zh_female_aojiaonvyou_tob", "傲娇女友 · 女声" },
    { "ICL_uranus_zh_female_xiemeinvwang_tob", "邪魅女王 · 女声" },
    { "ICL_uranus_zh_female_bingjiaojiejie_tob", "病娇姐姐 · 女声" },
    { "ICL_uranus_zh_female_bingjiaomengmei_tob", "病娇萌妹 · 女声" },
    { "ICL_uranus_zh_female_chengshuwenrou_tob", "成熟温柔 · 女声" },
    { "ICL_uranus_zh_female_chengshujiejie_tob", "成熟姐姐 · 女声" },
    { "ICL_uranus_zh_female_chunzhenshaonv_tob", "纯真少女 · 女声" },
    { "ICL_uranus_zh_female_chunchenvsheng_tob", "纯澈女生 · 女声" },
    { "ICL_uranus_zh_female_wumeikeren_tob", "妩媚可人 · 女声" },
    { "ICL_uranus_zh_female_heainainai_tob", "和蔼奶奶 · 女声" },
    { "ICL_uranus_zh_female_huopodiaoman_tob", "活泼刁蛮 · 女声" },
    { "ICL_uranus_zh_female_jiaohannvwang_tob", "娇憨女王 · 女声" },
    { "ICL_uranus_zh_female_jiaoruoluoli_tob", "娇弱萝莉 · 女声" },
    { "ICL_uranus_zh_female_jiaxiaozi_tob", "假小子 · 女声" },
    { "ICL_uranus_zh_female_jinglingxiangdao_tob", "精灵向导 · 女声" },
    { "ICL_uranus_zh_female_keainvsheng_tob", "可爱女生 · 女声" },
    { "ICL_uranus_zh_female_linjuayi_tob", "邻居阿姨 · 女声" },
    { "ICL_uranus_zh_female_tianmeijiaoqiao_tob", "甜美娇俏 · 女声" },
    { "ICL_uranus_zh_female_qinglenggaoya_tob", "清冷高雅 · 女声" },
    { "ICL_uranus_zh_female_xingganmeihuo_tob", "性感魅惑 · 女声" },
    { "ICL_uranus_zh_female_nuanxinxuejie_tob", "暖心学姐 · 女声" },
    { "ICL_uranus_zh_female_qingxinshaonv_tob", "倾心少女 · 女声" },
    { "ICL_uranus_zh_female_rouguhunshi_tob", "柔骨魂师 · 女声" },
    { "ICL_uranus_zh_female_tianmeihuopo_tob", "甜美活泼 · 女声" },
    { "ICL_uranus_zh_female_tiaopigongzhu_tob", "调皮公主 · 女声" },
    { "ICL_uranus_zh_female_tiexinnvyou_tob", "贴心女友 · 女声" },
    { "ICL_uranus_zh_female_wenrounvshen_tob", "温柔女神 · 女声" },
    { "ICL_uranus_zh_female_wenrouwenya_tob", "温柔文雅 · 女声" },
    { "ICL_uranus_zh_female_zhixinjiejie_tob", "知心姐姐 · 女声" },
    { "ICL_uranus_zh_female_wumeiyujie_tob", "妩媚御姐 · 女声" },
    { "ICL_uranus_zh_female_yuanqitianmei_tob", "元气甜妹 · 女声" },
    { "ICL_uranus_zh_female_xiemeiyujie_tob", "邪魅御姐 · 女声" },
    { "ICL_uranus_zh_female_xingganyujie_tob", "性感御姐 · 女声" },
    { "ICL_uranus_zh_female_tiexinguimi_tob", "贴心闺蜜 · 女声" },
    { "ICL_uranus_zh_female_tiexinmeimei_tob", "贴心妹妹 · 女声" },
    { "ICL_uranus_zh_female_wenroubaiyueguang_tob", "温柔白月光 · 女声" },
    { "ICL_uranus_zh_female_chuliannvyou_tob", "初恋女友 · 女声" },
    { "ICL_uranus_zh_female_zhixingwenwan_tob", "知性温婉 · 女声" },
    { "ICL_uranus_zh_male_aoqilingren_tob", "傲气凌人 · 男声" },
    { "ICL_uranus_zh_male_anrenqinzhu_tob", "黯刃秦主 · 男声" },
    { "ICL_uranus_zh_male_aojiaogongzi_tob", "傲娇公子 · 男声" },
    { "ICL_uranus_zh_male_aojiaojingying_tob", "傲娇精英 · 男声" },
    { "ICL_uranus_zh_male_aomanqingnian_tob", "傲慢青年 · 男声" },
    { "ICL_uranus_zh_male_aomanshaoye_tob", "傲慢少爷 · 男声" },
    { "ICL_uranus_zh_male_zhenbiandiyu_tob", "枕边低语 · 男声" },
    { "ICL_uranus_zh_male_badaoshaoye_tob", "霸道少爷 · 男声" },
    { "ICL_uranus_zh_male_badaozongcai_tob", "霸道总裁 · 男声" },
    { "ICL_uranus_zh_male_bingjiaobailian_tob", "病娇白莲 · 男声" },
    { "ICL_uranus_zh_male_bingjiaodidi_tob", "病娇弟弟 · 男声" },
    { "ICL_uranus_zh_male_bingjiaogege_tob", "病娇哥哥 · 男声" },
    { "ICL_uranus_zh_male_bingjiaonanyou_tob", "病娇男友 · 男声" },
    { "ICL_uranus_zh_male_bingjiaoshaonian_tob", "病娇少年 · 男声" },
    { "ICL_uranus_zh_male_bingruogongzi_tob", "病弱公子 · 男声" },
    { "ICL_uranus_zh_male_bingruoshaonian_tob", "病弱少年 · 男声" },
    { "ICL_uranus_zh_male_bujiqingnian_tob", "不羁青年 · 男声" },
    { "ICL_uranus_zh_male_chunhoudiyin_tob", "醇厚低音 · 男声" },
    { "ICL_uranus_zh_male_paoxiaoxiaoge_tob", "咆哮小哥 · 男声" },
    { "ICL_uranus_zh_male_yangyang_tob", "炀炀 · 男声" },
    { "ICL_uranus_zh_male_chanruoshaoye_tob", "孱弱少爷 · 男声" },
    { "ICL_uranus_zh_male_chengshuzongcai_tob", "成熟总裁 · 男声" },
    { "ICL_uranus_zh_male_qingyisugan_tob", "清逸苏感 · 男声" },
    { "ICL_uranus_zh_male_chunzhenxuedi_tob", "纯真学弟 · 男声" },
    { "ICL_uranus_zh_male_cixingnansang_tob", "磁性男嗓 · 男声" },
    { "ICL_uranus_zh_male_cujingnansheng_tob", "醋精男生 · 男声" },
    { "ICL_uranus_zh_male_cujingnanyou_tob", "醋精男友 · 男声" },
    { "ICL_uranus_zh_male_diyinchenyu_tob", "低音沉郁 · 男声" },
    { "ICL_uranus_zh_male_fengfashaonian_tob", "风发少年 · 男声" },
    { "ICL_uranus_zh_male_fuheigongzi_tob", "腹黑公子 · 男声" },
    { "ICL_uranus_zh_male_ganjingshaonian_tob", "干净少年 · 男声" },
    { "ICL_uranus_zh_male_gaolengzongcai_tob", "高冷总裁 · 男声" },
    { "ICL_uranus_zh_male_guaogongzi_tob", "孤傲公子 · 男声" },
    { "ICL_uranus_zh_male_gugaogongzi_tob", "孤高公子 · 男声" },
    { "ICL_uranus_zh_male_guiyishenmi_tob", "诡异神秘 · 男声" },
    { "ICL_uranus_zh_male_guzhibingjiao_tob", "固执病娇 · 男声" },
    { "ICL_uranus_zh_male_hanhoudunshi_tob", "憨厚敦实 · 男声" },
    { "ICL_uranus_zh_male_huoliqingnian_tob", "活力青年 · 男声" },
    { "ICL_uranus_zh_male_huoponanyou_tob", "活泼男友 · 男声" },
    { "ICL_uranus_zh_male_huoposhuanglang_tob", "活泼爽朗 · 男声" },
    { "ICL_uranus_zh_male_huzishushu_tob", "胡子叔叔 · 男声" },
    { "ICL_uranus_zh_male_jijiazhineng_tob", "机甲智能 · 男声" },
    { "ICL_uranus_zh_male_jingyingqingnian_tob", "精英青年 · 男声" },
    { "ICL_uranus_zh_male_junyigongzi_tob", "俊逸公子 · 男声" },
    { "ICL_uranus_zh_male_kailangqingkuai_tob", "开朗轻快 · 男声" },
    { "ICL_uranus_zh_male_kailangqingnian_tob", "开朗青年 · 男声" },
    { "ICL_uranus_zh_male_lanyincaohunshi_tob", "蓝银草魂师 · 男声" },
    { "ICL_uranus_zh_male_lengaozongcai_tob", "冷傲总裁 · 男声" },
    { "ICL_uranus_zh_male_lengdanshuli_tob", "冷淡疏离 · 男声" },
    { "ICL_uranus_zh_male_lengjungaozhi_tob", "冷峻高智 · 男声" },
    { "ICL_uranus_zh_male_lengjunshangsi_tob", "冷峻上司 · 男声" },
    { "ICL_uranus_zh_male_lengkugege_tob", "冷酷哥哥 · 男声" },
    { "ICL_uranus_zh_male_lenglianxiongzhang_tob", "冷脸兄长 · 男声" },
    { "ICL_uranus_zh_male_lenglianxueba_tob", "冷脸学霸 · 男声" },
    { "ICL_uranus_zh_male_lengmonanyou_tob", "冷漠男友 · 男声" },
    { "ICL_uranus_zh_male_lengmoxiongzhang_tob", "冷漠兄长 · 男声" },
    { "ICL_uranus_zh_male_lingyunqingnian_tob", "凌云青年 · 男声" },
    { "ICL_uranus_zh_male_qinglengjingui_tob", "清冷矜贵 · 男声" },
    { "ICL_uranus_zh_male_lvchaxiaoge_tob", "绿茶小哥 · 男声" },
    { "ICL_uranus_zh_male_mengdongqingnian_tob", "懵懂青年 · 男声" },
    { "ICL_uranus_zh_male_menyoupingxiaoge_tob", "闷油瓶小哥 · 男声" },
    { "ICL_uranus_zh_male_xiaozhangxiaoge_tob", "嚣张小哥 · 男声" },
    { "ICL_uranus_zh_male_nianrennanyou_tob", "粘人男友 · 男声" },
    { "ICL_uranus_zh_male_nuanxintitie_tob", "暖心体贴 · 男声" },
    { "ICL_uranus_zh_male_pianpiangongzi_tob", "翩翩公子 · 男声" },
    { "ICL_uranus_zh_male_chenwenyouya_tob", "沉稳优雅 · 男声" },
    { "ICL_uranus_zh_male_qingsexiaosheng_tob", "青涩小生 · 男声" },
    { "ICL_uranus_zh_male_qingseqingnian_tob", "青涩青年 · 男声" },
    { "ICL_uranus_zh_male_qingshuangshaonian_tob", "清爽少年 · 男声" },
    { "ICL_uranus_zh_male_qinqieqingnian_tob", "亲切青年 · 男声" },
    { "ICL_uranus_zh_male_qinglangwenrun_tob", "清朗温润 · 男声" },
    { "ICL_uranus_zh_male_rexueshaonian_tob", "热血少年 · 男声" },
    { "ICL_uranus_zh_male_ruyacaijun_tob", "儒雅才俊 · 男声" },
    { "ICL_uranus_zh_male_ruyajunzi_tob", "儒雅君子 · 男声" },
    { "ICL_uranus_zh_male_ruyazongcai_tob", "儒雅总裁 · 男声" },
    { "ICL_uranus_zh_male_sajiaonansheng_tob", "撒娇男生 · 男声" },
    { "ICL_uranus_zh_male_sajiaonanyou_tob", "撒娇男友 · 男声" },
    { "ICL_uranus_zh_male_sajiaonianren_tob", "撒娇粘人 · 男声" },
    { "ICL_uranus_zh_male_satuoqingnian_tob", "洒脱青年 · 男声" },
    { "ICL_uranus_zh_male_shaonianjiangjun_tob", "少年将军 · 男声" },
    { "ICL_uranus_zh_male_shenchenzongcai_tob", "深沉总裁 · 男声" },
    { "ICL_uranus_zh_male_jilingxiaohuo_tob", "机灵小伙 · 男声" },
    { "ICL_uranus_zh_male_shenmifashi_tob", "神秘法师 · 男声" },
    { "ICL_uranus_zh_male_shuaizhenxiaohuo_tob", "率真小伙 · 男声" },
    { "ICL_uranus_zh_male_dichenqianquan_tob", "低沉缱绻 · 男声" },
    { "ICL_uranus_zh_male_siwenqingnian_tob", "斯文青年 · 男声" },
    { "ICL_uranus_zh_male_tianxinanyou_tob", "甜系男友 · 男声" },
    { "ICL_uranus_zh_male_tiexinnanyou_tob", "贴心男友 · 男声" },
    { "ICL_uranus_zh_male_wenrounantongzhuo_tob", "温柔男同桌 · 男声" },
    { "ICL_uranus_zh_male_wenrounanyou_tob", "温柔男友 · 男声" },
    { "ICL_uranus_zh_male_wenrouxuezhang_tob", "温柔学长 · 男声" },
    { "ICL_uranus_zh_male_wenrunxuezhe_tob", "温润学者 · 男声" },
    { "ICL_uranus_zh_male_wenshunshaonian_tob", "温顺少年 · 男声" },
    { "ICL_uranus_zh_male_guayanxiaoge_tob", "寡言小哥 · 男声" },
    { "ICL_uranus_zh_male_xiaohouye_tob", "小侯爷 · 男声" },
    { "ICL_uranus_zh_male_naiqixiaosheng_tob", "奶气小生 · 男声" },
    { "ICL_uranus_zh_male_xiaosasuixing_tob", "潇洒随性 · 男声" },
    { "ICL_uranus_zh_male_wenrouneilian_tob", "温柔内敛 · 男声" },
    { "ICL_uranus_zh_male_xuebanantongzhuo_tob", "学霸男同桌 · 男声" },
    { "ICL_uranus_zh_male_xuebatongzhuo_tob", "学霸同桌 · 男声" },
    { "ICL_uranus_zh_male_yiqishaonian_tob", "意气少年 · 男声" },
    { "ICL_uranus_zh_male_younidashu_tob", "油腻大叔 · 男声" },
    { "ICL_uranus_zh_male_youmodaye_tob", "幽默大爷 · 男声" },
    { "ICL_uranus_zh_male_youmoshushu_tob", "幽默叔叔 · 男声" },
    { "ICL_uranus_zh_male_youroubangzhu_tob", "优柔帮主 · 男声" },
    { "ICL_uranus_zh_male_yourougongzi_tob", "优柔公子 · 男声" },
    { "ICL_uranus_zh_male_yuanqishaonian_tob", "元气少年 · 男声" },
    { "ICL_uranus_zh_male_zhangjianjunzi_tob", "仗剑君子 · 男声" },
    { "ICL_uranus_zh_male_zhangjianxiake_tob", "仗剑侠客 · 男声" },
    { "ICL_uranus_zh_male_zhengzhiqingnian_tob", "正直青年 · 男声" },
    { "ICL_uranus_zh_male_zhishuaiqingnian_tob", "直率青年 · 男声" },
    { "ICL_uranus_zh_male_zhongerqingnian_tob", "中二青年 · 男声" },
    { "ICL_uranus_zh_male_zifuqingnian_tob", "自负青年 · 男声" },
    { "ICL_uranus_zh_male_zixinqingnian_tob", "自信青年 · 男声" },
    { "ICL_uranus_zh_male_tiancaitongzhuo_tob", "天才同桌 · 男声" },
    { "ICL_uranus_zh_male_shuanglangshaonian_tob", "爽朗少年 · 男声" },
};

#define TTS_VOICE_GROUP_COUNT \
    (sizeof(s_tts_groups) / sizeof(s_tts_groups[0]))

static const tts_voice_group_t s_tts_groups[] = {
    { "1.0", "volc.service_type.10029", s_tts_voices_10,
      sizeof(s_tts_voices_10) / sizeof(s_tts_voices_10[0]) },
    { "2.0", "seed-tts-2.0", s_tts_voices_20_general,
      sizeof(s_tts_voices_20_general) / sizeof(s_tts_voices_20_general[0]) },
    { "角色", "seed-tts-2.0", s_tts_voices_20_role,
      sizeof(s_tts_voices_20_role) / sizeof(s_tts_voices_20_role[0]) },
    { "客服", "seed-tts-2.0", s_tts_voices_20_service,
      sizeof(s_tts_voices_20_service) / sizeof(s_tts_voices_20_service[0]) },
    { "多语种", "seed-tts-2.0", s_tts_voices_20_multi,
      sizeof(s_tts_voices_20_multi) / sizeof(s_tts_voices_20_multi[0]) },
    { "ICL", "seed-tts-2.0", s_tts_voices_20_icl,
      sizeof(s_tts_voices_20_icl) / sizeof(s_tts_voices_20_icl[0]) },
};

static const tts_voice_entry_t *tts_voice_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t g = 0; g < TTS_VOICE_GROUP_COUNT; g++) {
        for (size_t i = 0; i < s_tts_groups[g].count; i++) {
            if (strcmp(id, s_tts_groups[g].items[i].id) == 0) {
                return &s_tts_groups[g].items[i];
            }
        }
    }
    return NULL;
}

static int tts_voice_category(const char *id)
{
    if (id == NULL) {
        return 0;
    }
    for (size_t g = 0; g < TTS_VOICE_GROUP_COUNT; g++) {
        for (size_t i = 0; i < s_tts_groups[g].count; i++) {
            if (strcmp(id, s_tts_groups[g].items[i].id) == 0) {
                return (int)g;
            }
        }
    }
    return 0;
}

static const char *tts_voice_display_name(const char *id)
{
    const tts_voice_entry_t *voice = tts_voice_find(id);
    return voice != NULL ? voice->name : id;
}

static void settings_tts_refresh(void)
{
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return;
    }
    if (s_set_tts_api_value != NULL) {
        label_set_if_changed(s_set_tts_api_value,
                             cfg.api_key[0] ? i18n_str(STR_CLAW_AGENT_KEY_SET)
                                            : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_tts_app_value != NULL) {
        label_set_if_changed(s_set_tts_app_value,
                             cfg.app_id[0] ? cfg.app_id
                                           : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_tts_token_value != NULL) {
        label_set_if_changed(s_set_tts_token_value,
                             cfg.access_token[0] ? i18n_str(STR_CLAW_AGENT_KEY_SET)
                                                 : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_tts_resource_value != NULL) {
        label_set_if_changed(s_set_tts_resource_value,
                             cfg.resource_id[0] ? cfg.resource_id
                                                : "seed-tts-2.0");
    }
    if (s_set_tts_speaker_value != NULL) {
        label_set_if_changed(s_set_tts_speaker_value,
                             cfg.speaker[0] ? tts_voice_display_name(cfg.speaker)
                                            : i18n_str(STR_CLAW_AGENT_KEY_NONE));
    }
    if (s_set_tts_auto_switch != NULL) {
        if (cfg.auto_speak) {
            lv_obj_add_state(s_set_tts_auto_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_set_tts_auto_switch, LV_STATE_CHECKED);
        }
    }
}

static bool settings_tts_done(const char *text)
{
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return false;
    }
    const char *value = text != NULL ? text : "";
    switch (s_tts_edit_field) {
    case TTS_EDIT_API_KEY:
        strlcpy(cfg.api_key, value, sizeof(cfg.api_key));
        break;
    case TTS_EDIT_APP_ID:
        strlcpy(cfg.app_id, value, sizeof(cfg.app_id));
        break;
    case TTS_EDIT_TOKEN:
        strlcpy(cfg.access_token, value, sizeof(cfg.access_token));
        break;
    case TTS_EDIT_RESOURCE:
        strlcpy(cfg.resource_id, value, sizeof(cfg.resource_id));
        break;
    case TTS_EDIT_SPEAKER:
        strlcpy(cfg.speaker, value, sizeof(cfg.speaker));
        break;
    default:
        return false;
    }
    if (claw_tts_set_config(&cfg) != ESP_OK) {
        return false;
    }
    settings_tts_refresh();
    return true;
}

static void tts_picker_close(void)
{
    if (s_tts_picker != NULL) {
        lv_obj_delete(s_tts_picker);
        s_tts_picker = NULL;
        s_tts_picker_list = NULL;
    }
}

static void tts_picker_dismiss(lv_event_t *e)
{
    (void)e;
    tts_picker_close();
}

static void tts_picker_select(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (id == NULL) {
        return;
    }
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) == ESP_OK) {
        strlcpy(cfg.speaker, id, sizeof(cfg.speaker));
        /* Catalog entries are grouped by TTS generation; match the resource
         * so a 2.0 voice never runs against the 1.0 product (error 55000000). */
        if (s_tts_picker_cat >= 0 &&
            s_tts_picker_cat < (int)TTS_VOICE_GROUP_COUNT) {
            strlcpy(cfg.resource_id, s_tts_groups[s_tts_picker_cat].resource_id,
                    sizeof(cfg.resource_id));
        }
        if (claw_tts_set_config(&cfg) == ESP_OK) {
            settings_tts_refresh();
        }
    }
    tts_picker_close();
}

static void tts_picker_custom(lv_event_t *e)
{
    (void)e;
    tts_picker_close();
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return;
    }
    s_tts_edit_field = TTS_EDIT_SPEAKER;
    ui_text_dialog_open(i18n_str(STR_CLAW_TTS_SPEAKER),
                        i18n_str(STR_CLAW_TTS_HINT),
                        cfg.speaker, false, settings_tts_done);
}

static void tts_picker_fill_list(void)
{
    if (s_tts_picker_list == NULL) {
        return;
    }
    lv_obj_clean(s_tts_picker_list);
    if (s_tts_picker_cat < 0 ||
        s_tts_picker_cat >= (int)TTS_VOICE_GROUP_COUNT) {
        s_tts_picker_cat = 0;
    }

    const tts_voice_group_t *group = &s_tts_groups[s_tts_picker_cat];
    claw_tts_config_t cfg = {0};
    (void)claw_tts_get_config(&cfg);
    lv_obj_t *selected_row = NULL;

    for (size_t i = 0; i < group->count; i++) {
        const tts_voice_entry_t *voice = &group->items[i];
        lv_obj_t *row = ui_list_row_create(s_tts_picker_list, 0, 0, 520, 56);
        if (row == NULL) {
            continue;
        }
        bool selected = cfg.speaker[0] != '\0' &&
                        strcmp(cfg.speaker, voice->id) == 0;
        if (selected) {
            selected_row = row;
            lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ACCENT), 0);
            lv_obj_set_style_bg_opa(row, UI_OPA_SOFT, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ACCENT), 0);
            lv_obj_set_style_border_opa(row, 60, 0);
        }
        const lv_font_t *name_font = claw_dyn_font(UI_FONT_SMALL);
        lv_obj_t *name = ui_label_create(row, voice->name, 16, 4,
                                         UI_COLOR_TEXT, name_font);
        lv_obj_set_size(name, 480, lv_font_get_line_height(name_font));
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(name, lv_anim_speed(40), 0);
        lv_obj_t *id = ui_label_create(row, voice->id, 16, 32,
                                       UI_COLOR_MUTED, UI_FONT_SMALL);
        lv_obj_set_size(id, 480, lv_font_get_line_height(UI_FONT_SMALL));
        lv_label_set_long_mode(id, LV_LABEL_LONG_DOT);
        char *id_copy = strdup(voice->id);
        if (id_copy != NULL) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, tts_picker_select, LV_EVENT_CLICKED, id_copy);
            lv_obj_add_event_cb(row, free_user_data_cb, LV_EVENT_DELETE, id_copy);
        }
    }

    lv_obj_update_layout(s_tts_picker_list);
    if (selected_row != NULL) {
        lv_obj_scroll_to_view(selected_row, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_tts_picker_list, 0, LV_ANIM_OFF);
    }
}

static void tts_picker_category_event(lv_event_t *e)
{
    s_tts_picker_cat = ui_segmented_get_selected(lv_event_get_target(e));
    tts_picker_fill_list();
}

static void tts_picker_open(void)
{
    tts_picker_close();
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);

    lv_obj_t *scrim = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_size(scrim, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, tts_picker_dismiss, LV_EVENT_CLICKED, NULL);
    s_tts_picker = scrim;

    lv_obj_t *card = ui_card_create(scrim, 0, 0, 560, 448);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(card, 70, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    ui_badge_create(card, 20, 14, 36, LV_SYMBOL_AUDIO, UI_COLOR_CYAN);
    ui_label_create(card, i18n_str(STR_CLAW_TTS_SPEAKER), 68, 18,
                    UI_COLOR_TEXT, UI_FONT_BODY);

    s_tts_picker_cat = tts_voice_category(cfg.speaker);

    const char *labels[TTS_VOICE_GROUP_COUNT];
    for (size_t i = 0; i < TTS_VOICE_GROUP_COUNT; i++) {
        labels[i] = s_tts_groups[i].label;
    }
    lv_obj_t *seg = ui_segmented_create(card, 20, 56, 520, 40, labels,
                                        (int)TTS_VOICE_GROUP_COUNT,
                                        s_tts_picker_cat);
    for (uint32_t i = 0; i < lv_obj_get_child_count(seg); i++) {
        lv_obj_t *btn = lv_obj_get_child(seg, i);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label != NULL) {
            lv_obj_set_style_text_font(label, claw_dyn_font(UI_FONT_SMALL), 0);
        }
    }
    lv_obj_add_event_cb(seg, tts_picker_category_event, LV_EVENT_VALUE_CHANGED,
                        NULL);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_pos(list, 20, 104);
    lv_obj_set_size(list, 520, 244);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    s_tts_picker_list = list;
    tts_picker_fill_list();

    lv_obj_t *custom = ui_button_create(card, "自定义", 20, 360, 200, 44, 1);
    lv_obj_t *custom_label = lv_obj_get_child(custom, 0);
    if (custom_label != NULL) {
        lv_obj_set_style_text_font(custom_label, claw_dyn_font(UI_FONT_SMALL), 0);
    }
    lv_obj_add_event_cb(custom, tts_picker_custom, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close = ui_button_create(card, i18n_str(STR_AI_CLOSE),
                                       400, 360, 140, 44, 0);
    lv_obj_add_event_cb(close, tts_picker_dismiss, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(scrim);
}

static void settings_tts_field_clicked(lv_event_t *e)
{
    int field = (int)(intptr_t)lv_event_get_user_data(e);
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return;
    }
    const char *title = "";
    const char *value = "";
    bool password = false;
    switch (field) {
    case TTS_EDIT_API_KEY:
        title = "API Key";
        value = cfg.api_key;
        password = true;
        break;
    case TTS_EDIT_APP_ID:
        title = "App ID";
        value = cfg.app_id;
        break;
    case TTS_EDIT_TOKEN:
        title = "Access Token";
        value = cfg.access_token;
        password = true;
        break;
    case TTS_EDIT_RESOURCE:
        title = i18n_str(STR_CLAW_TTS_RESOURCE);
        value = cfg.resource_id;
        break;
    case TTS_EDIT_SPEAKER:
        tts_picker_open();
        return;
    default:
        return;
    }
    s_tts_edit_field = field;
    ui_text_dialog_open(title, i18n_str(STR_CLAW_TTS_HINT), value, password,
                        settings_tts_done);
}

static void settings_tts_auto_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return;
    }
    cfg.auto_speak = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (claw_tts_set_config(&cfg) == ESP_OK) {
        settings_tts_refresh();
    }
}

static void settings_tts_menu_row(lv_obj_t *parent, int y, const char *title,
                                  const char *value, int field,
                                  lv_obj_t **out_value)
{
    lv_obj_t *val = NULL;
    ui_menu_row_create(parent, 20, y, INNER_W - 40, 42, LV_SYMBOL_EDIT,
                       title, value, &val, settings_tts_field_clicked,
                       (void *)(intptr_t)field);
    if (val != NULL) {
        lv_obj_set_width(val, 300);
        lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (out_value != NULL) {
        *out_value = val;
    }
}

static void settings_tts_build(lv_obj_t *body)
{
    tts_picker_close();
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);

    lv_obj_t *card = ui_card_create(body, 0, 0, INNER_W, 356);
    ui_label_create(card, i18n_str(STR_CLAW_TTS_TITLE), 20, 12,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_t *hint = ui_label_create(card, i18n_str(STR_CLAW_TTS_HINT),
                                     20, 40, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(hint, 520);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);

    const char *set = i18n_str(STR_CLAW_AGENT_KEY_SET);
    const char *unset = i18n_str(STR_CLAW_AGENT_KEY_NONE);
    settings_tts_menu_row(card, 68, "API Key",
                          cfg.api_key[0] ? set : unset,
                          TTS_EDIT_API_KEY, &s_set_tts_api_value);
    settings_tts_menu_row(card, 114, "App ID",
                          cfg.app_id[0] ? cfg.app_id : unset,
                          TTS_EDIT_APP_ID, &s_set_tts_app_value);
    settings_tts_menu_row(card, 160, "Access Token",
                          cfg.access_token[0] ? set : unset,
                          TTS_EDIT_TOKEN, &s_set_tts_token_value);
    settings_tts_menu_row(card, 206, i18n_str(STR_CLAW_TTS_RESOURCE),
                          cfg.resource_id[0] ? cfg.resource_id : "seed-tts-2.0",
                          TTS_EDIT_RESOURCE, &s_set_tts_resource_value);
    settings_tts_menu_row(card, 252, i18n_str(STR_CLAW_TTS_SPEAKER),
                          cfg.speaker[0] ? tts_voice_display_name(cfg.speaker) : unset,
                          TTS_EDIT_SPEAKER, &s_set_tts_speaker_value);
    if (s_set_tts_speaker_value != NULL) {
        lv_obj_set_style_text_font(s_set_tts_speaker_value,
                                   claw_dyn_font(UI_FONT_SMALL), 0);
    }

    lv_obj_t *auto_row = ui_list_row_create(card, 20, 300, INNER_W - 40, 42);
    if (auto_row != NULL) {
        ui_label_create(auto_row, i18n_str(STR_CLAW_TTS_AUTO_SPEAK), 16, 11,
                        UI_COLOR_TEXT, UI_FONT_BODY);
        s_set_tts_auto_switch = claw_switch_create(auto_row, 528, 6,
                                                   cfg.auto_speak,
                                                   settings_tts_auto_event);
    }
    settings_tts_refresh();
}

static void settings_build(void)
{
    detail_header(STR_CLAW_TAB_SETTINGS, STR_CLAW_DESC_SETTINGS,
                  LV_SYMBOL_SETTINGS, UI_COLOR_ACCENT_SOFT);
    lv_obj_t *body = detail_body_create(true);

    s_lua_switch = NULL;
    s_set_agent_switch = NULL;
    s_set_agent_state = NULL;
    s_set_agent_error = NULL;
    s_set_url_value = NULL;
    s_set_model_value = NULL;
    s_set_key_value = NULL;
    s_set_tts_api_value = NULL;
    s_set_tts_app_value = NULL;
    s_set_tts_token_value = NULL;
    s_set_tts_resource_value = NULL;
    s_set_tts_speaker_value = NULL;
    s_set_tts_auto_switch = NULL;

#if CONFIG_TEMPLATE_CLAW_AGENT
    claw_agent_settings_t cfg;
    (void)claw_settings_get_agent(&cfg);

    lv_obj_t *agent_card = ui_card_create(body, 0, 0, INNER_W, 96);
    ui_label_create(agent_card, i18n_str(STR_CLAW_AGENT_TITLE), 20, 12,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    s_set_agent_state = ui_label_create(agent_card, "", 20, 42,
                                        UI_COLOR_MUTED,
                                        claw_dyn_font(UI_FONT_SMALL));
    lv_obj_set_width(s_set_agent_state, 520);
    lv_label_set_long_mode(s_set_agent_state, LV_LABEL_LONG_DOT);
    s_set_agent_error = ui_label_create(agent_card, "", 20, 66,
                                        UI_COLOR_ERROR, UI_FONT_SMALL);
    lv_obj_set_width(s_set_agent_error, 520);
    lv_obj_add_flag(s_set_agent_error, LV_OBJ_FLAG_HIDDEN);
    s_set_agent_switch = claw_switch_create(agent_card, 568, 18, cfg.enabled,
                                            agent_switch_event);

    lv_obj_t *cfg_card = ui_card_create(body, 0, 0, INNER_W, 240);
    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_SETTINGS), 20, 10,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_BACKEND), 20, 48,
                    UI_COLOR_MUTED, UI_FONT_SMALL);
    static const char *const backends[3] = { "OpenAI", "DeepSeek", "Claude" };
    lv_obj_t *seg = ui_segmented_create(cfg_card, 130, 38, 320, 34,
                                        backends, 3, agent_backend_index(&cfg));
    lv_obj_add_event_cb(seg, agent_backend_event, LV_EVENT_VALUE_CHANGED, NULL);

    const char *not_set = i18n_str(STR_CLAW_AGENT_KEY_NONE);
    agent_menu_row(cfg_card, 82, i18n_str(STR_CLAW_AGENT_URL),
                   cfg.base_url[0] ? cfg.base_url : not_set,
                   agent_url_clicked, &s_set_url_value);
    agent_menu_row(cfg_card, 128, i18n_str(STR_CLAW_AGENT_MODEL),
                   cfg.model[0] ? cfg.model : not_set,
                   agent_model_clicked, &s_set_model_value);
    agent_menu_row(cfg_card, 174, i18n_str(STR_CLAW_AGENT_KEY),
                   cfg.api_key[0] ? i18n_str(STR_CLAW_AGENT_KEY_SET) : not_set,
                   agent_key_clicked, &s_set_key_value);
    ui_label_create(cfg_card, i18n_str(STR_CLAW_AGENT_KEY_HINT), 20, 218,
                    UI_COLOR_MUTED, UI_FONT_SMALL);

    settings_agent_refresh();
#endif

    lv_obj_t *lua_card = ui_card_create(body, 0, 0, INNER_W, 84);
    ui_label_create(lua_card, i18n_str(STR_CLAW_LUA_AI), 20, 12,
                    UI_COLOR_TEXT, UI_FONT_BODY);
    lv_obj_t *hint = ui_label_create(lua_card, i18n_str(STR_CLAW_LUA_AI_HINT),
                                     20, 40, UI_COLOR_MUTED, UI_FONT_SMALL);
    lv_obj_set_width(hint, 520);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    s_lua_switch = claw_switch_create(lua_card, 568, 18,
                                      claw_settings_lua_ai_enabled(),
                                      lua_ai_switch_event);

    settings_tts_build(body);

    ui_label_create(body, i18n_str(STR_CLAW_WEB_HINT), 0, 0,
                    UI_COLOR_DISABLED, UI_FONT_SMALL);
}

/* ---------- page ---------- */

static void claws_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_root == NULL || !lv_obj_is_visible(s_root)) {
        return;
    }

    bool ready = claw_port_is_ready();
    if (ready != s_claw_ready_seen) {
        s_claw_ready_seen = ready;
        build_detail();
    }

    status_refresh();
#if CONFIG_TEMPLATE_CLAW_AGENT
    if (s_tab == CLAW_TAB_CHAT) {
        agent_tick();
    }
#endif
    if (s_tab == CLAW_TAB_AUTOMATION) {
        schedules_tick();
    }
}

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
    s_tab = CLAW_DEFAULT_TAB;
    s_claw_ready_seen = claw_port_is_ready();
    s_pending_schedule[0] = '\0';
    s_sched_msg[0] = '\0';
    s_sched_msg_error = false;
    s_workspace_seg = WORKSPACE_SEG_MEMORY;
    s_workspace_pane = NULL;
    s_schedule_row_count = 0;
    s_schedule_refresh_tick = 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_chat_history = NULL;
    s_chat_selector_label = NULL;
    s_chat_state_chip = NULL;
    s_chat_state_label = NULL;
    s_chat_error_label = NULL;
    s_chat_picker = NULL;
    s_chat_sig_valid = false;
    s_chat_state_last[0] = '\0';
#endif
    s_lua_switch = NULL;
    s_set_agent_switch = NULL;
    s_set_agent_state = NULL;
    s_set_agent_error = NULL;
    s_set_url_value = NULL;
    s_set_model_value = NULL;
    s_set_key_value = NULL;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_agent_cfg_error = false;
#endif

    nav_build();

    s_detail = ui_panel_create(content, DETAIL_X, 0, DETAIL_W, UI_PAGE_H);

    status_refresh();
    build_detail();

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
    tts_picker_close();
#if CONFIG_TEMPLATE_CLAW_AGENT
    chat_picker_close();
#endif
    if (s_script_view_buf != NULL) {
        heap_caps_free(s_script_view_buf);
        s_script_view_buf = NULL;
    }
    s_root = NULL;
    s_rail = NULL;
    s_detail = NULL;
    s_body = NULL;
    s_claw_ready_seen = false;
    s_nav_count = 0;
    memset(s_nav, 0, sizeof(s_nav));
    s_footer_agent_dot = NULL;
    s_footer_agent_label = NULL;
    s_footer_lua_dot = NULL;
    s_footer_lua_label = NULL;
    s_footer_jobs_dot = NULL;
    s_footer_jobs_label = NULL;
    s_lua_switch = NULL;
    s_set_agent_switch = NULL;
    s_set_agent_state = NULL;
    s_set_agent_error = NULL;
    s_set_url_value = NULL;
    s_set_model_value = NULL;
    s_set_key_value = NULL;
    s_set_tts_api_value = NULL;
    s_set_tts_app_value = NULL;
    s_set_tts_token_value = NULL;
    s_set_tts_resource_value = NULL;
    s_set_tts_speaker_value = NULL;
    s_set_tts_auto_switch = NULL;
    s_tts_picker = NULL;
    s_workspace_pane = NULL;
    s_schedule_row_count = 0;
    s_schedule_refresh_tick = 0;
#if CONFIG_TEMPLATE_CLAW_AGENT
    s_chat_history = NULL;
    s_chat_selector_label = NULL;
    s_chat_state_chip = NULL;
    s_chat_state_label = NULL;
    s_chat_error_label = NULL;
    s_chat_picker = NULL;
    s_chat_sig_valid = false;
    s_chat_state_last[0] = '\0';
    s_chat_stream_text[0] = '\0';
    s_chat_stream_rev = 0;
    s_chat_stream_rev_seen = 0;
    s_chat_stream_active = false;
    s_chat_stream_label = NULL;
    s_chat_prefix_sig = 0;
    s_chat_rendered_count = 0;
#endif
}

const ui_page_t page_claw = {
    .id = "claw",
    .name = STR_NAV_CLAW,
    .symbol = LV_SYMBOL_EDIT,
    .create = claw_create,
    .destroy = claw_destroy,
};
