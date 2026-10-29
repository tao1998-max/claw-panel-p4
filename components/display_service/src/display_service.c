/*
 * Product-side display_service compatible layer.
 *
 * The upstream ESP-Claw display_service owns the LCD and LVGL through the
 * official Board Manager. This panel is already owned by the P4 BSP and the
 * XiaoZhi UI, so this implementation keeps that ownership and exposes the same
 * session API on top of the existing LVGL display:
 *
 *  - SHARED_LVGL    : no screen switch; calls run on the current UI.
 *  - EXCLUSIVE_LVGL : opens a new LVGL screen and restores the previous screen.
 *  - EXCLUSIVE_RAW  : pauses LVGL and writes RGB565 directly to the panel.
 *
 * The official Lua display/lvgl modules compile unchanged against this header.
 */
#include "display_service.h"

#include <stdlib.h>
#include <string.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "display_service";

#define DISPLAY_SERVICE_MAX_SESSIONS 4
#define DISPLAY_SERVICE_TOUCH_MAX 8

typedef struct display_service_session_t {
    bool used;
    bool active;
    char owner[DISPLAY_SERVICE_OWNER_NAME_LEN];
    display_service_mode_t mode;
    uint32_t flags;
    lv_obj_t *screen;
    lv_obj_t *canvas;
    uint8_t *raw_buf;
    bool raw_active;
    bool raw_lock_held;
    uint16_t width;
    uint16_t height;
    display_service_session_cleanup_cb_t cleanup_cb;
    display_service_session_exit_request_cb_t exit_request_cb;
    void *user_ctx;
} display_service_session_t;

typedef struct {
    display_service_touch_observer_cb_t cb;
    void *user_ctx;
} touch_observer_t;

static display_service_session_t s_sessions[DISPLAY_SERVICE_MAX_SESSIONS];
static lv_obj_t *s_exit_btn;
static bool s_started;
static lv_obj_t *s_default_screen;
static lv_obj_t *s_screen_stack[8];
static size_t s_screen_depth;

static void screen_stack_reset(void)
{
    s_screen_depth = 0;
}

static void screen_stack_push(lv_obj_t *screen)
{
    if (s_screen_depth < sizeof(s_screen_stack) / sizeof(s_screen_stack[0])) {
        s_screen_stack[s_screen_depth++] = screen;
    }
}

static lv_obj_t *screen_stack_pop(void)
{
    if (s_screen_depth == 0) {
        return s_default_screen;
    }
    return s_screen_stack[--s_screen_depth];
}
static display_service_state_observer_cb_t s_state_cb;
static void *s_state_ctx;
static touch_observer_t s_touch_observers[DISPLAY_SERVICE_TOUCH_MAX];
static size_t s_touch_observer_count;
static esp_timer_handle_t s_touch_timer;
static display_service_touch_snapshot_t s_last_touch;
static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;

static bool any_raw_active(void);

static void notify_state(display_service_state_event_t event)
{
    if (s_state_cb) {
        s_state_cb(event, s_state_ctx);
    }
}

static void touch_timer_cb(void *arg)
{
    (void)arg;
    if (s_touch_observer_count == 0) {
        return;
    }
    display_service_touch_snapshot_t snapshot = {0};
    if (any_raw_active()) {
        esp_lcd_touch_handle_t tp = bsp_display_get_touch_handle();
        if (tp) {
            (void)esp_lcd_touch_read_data(tp);
            uint16_t xs[1] = {0};
            uint16_t ys[1] = {0};
            uint16_t strength[1] = {0};
            uint8_t count = 0;
            if (esp_lcd_touch_get_coordinates(tp, xs, ys, strength, &count, 1) &&
                count > 0) {
                snapshot.count = 1;
                snapshot.points[0].id = 0;
                snapshot.points[0].x = xs[0];
                snapshot.points[0].y = ys[0];
            }
        }
    } else {
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev && bsp_display_lock(10)) {
            lv_indev_state_t state = lv_indev_get_state(indev);
            if (state == LV_INDEV_STATE_PRESSED) {
                lv_point_t point = {0};
                lv_indev_get_point(indev, &point);
                snapshot.count = 1;
                snapshot.points[0].id = 0;
                snapshot.points[0].x = point.x;
                snapshot.points[0].y = point.y;
            }
            bsp_display_unlock();
        }
    }
    s_last_touch = snapshot;
    touch_observer_t observers[DISPLAY_SERVICE_TOUCH_MAX];
    size_t count;
    taskENTER_CRITICAL(&s_touch_mux);
    count = s_touch_observer_count;
    memcpy(observers, s_touch_observers, count * sizeof(observers[0]));
    taskEXIT_CRITICAL(&s_touch_mux);
    for (size_t i = 0; i < count; i++) {
        if (observers[i].cb) {
            observers[i].cb(&snapshot, observers[i].user_ctx);
        }
    }
}

static void touch_timer_start(void)
{
    if (s_touch_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = touch_timer_cb,
        .name = "disp_touch",
    };
    if (esp_timer_create(&args, &s_touch_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_touch_timer, 30 * 1000);
    } else {
        s_touch_timer = NULL;
    }
}

static display_service_session_t *session_from_handle(display_service_session_handle_t handle)
{
    display_service_session_t *session = (display_service_session_t *)handle;
    if (!session || session < s_sessions ||
        session >= s_sessions + DISPLAY_SERVICE_MAX_SESSIONS) {
        return NULL;
    }
    return session->used ? session : NULL;
}

static bool any_exclusive(void)
{
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && s_sessions[i].active &&
            s_sessions[i].mode != DISPLAY_SERVICE_MODE_SHARED_LVGL) {
            return true;
        }
    }
    return false;
}


esp_err_t display_service_request_exit(void);

static void exit_btn_cb(lv_event_t *e)
{
    (void)e;
    (void)display_service_request_exit();
}

static void exit_overlay_create(void)
{
    if (s_exit_btn) {
        return;
    }
    s_exit_btn = lv_button_create(lv_layer_top());
    if (!s_exit_btn) {
        return;
    }
    lv_obj_set_size(s_exit_btn, 64, 40);
    lv_obj_align(s_exit_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(s_exit_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_exit_btn, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_exit_btn, 0, 0);
    lv_obj_set_style_radius(s_exit_btn, 8, 0);
    lv_obj_add_event_cb(s_exit_btn, exit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(s_exit_btn);
    lv_label_set_text(label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

static void exit_overlay_remove(void)
{
    if (s_exit_btn) {
        lv_obj_delete(s_exit_btn);
        s_exit_btn = NULL;
    }
}

static bool any_raw_active(void)
{
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && s_sessions[i].active && s_sessions[i].raw_active) {
            return true;
        }
    }
    return false;
}

esp_err_t display_service_start(const display_service_config_t *config)
{
    (void)config;
    if (s_started) {
        return ESP_OK;
    }
    if (bsp_display_lock(0)) {
        s_default_screen = lv_screen_active();
        screen_stack_reset();
        bsp_display_unlock();
    }
    s_started = true;
    return ESP_OK;
}

void display_service_stop(void)
{
    s_started = false;
    s_default_screen = NULL;
    screen_stack_reset();
}

bool display_service_is_started(void)
{
    return s_started;
}

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session)
{
    if (!config || !ret_session || !config->owner_name || !config->owner_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        esp_err_t err = display_service_start(&config->display_config);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->mode != DISPLAY_SERVICE_MODE_SHARED_LVGL && any_exclusive()) {
        return ESP_ERR_INVALID_STATE;
    }

    display_service_session_t *session = NULL;
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        if (!s_sessions[i].used) {
            session = &s_sessions[i];
            break;
        }
    }
    if (!session) {
        return ESP_ERR_NO_MEM;
    }
    memset(session, 0, sizeof(*session));
    session->used = true;
    session->active = true;
    session->mode = config->mode;
    session->flags = config->flags;
    session->cleanup_cb = config->cleanup_cb;
    session->exit_request_cb = config->exit_request_cb;
    session->user_ctx = config->user_ctx;
    strlcpy(session->owner, config->owner_name, sizeof(session->owner));

    if (!bsp_display_lock(0)) {
        session->used = false;
        return ESP_ERR_TIMEOUT;
    }
    lv_display_t *disp = lv_display_get_default();
    session->width = (uint16_t)lv_display_get_horizontal_resolution(disp);
    session->height = (uint16_t)lv_display_get_vertical_resolution(disp);

    if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        screen_stack_push(lv_screen_active());
        session->screen = lv_obj_create(NULL);
        if (!session->screen) {
            (void)screen_stack_pop();
            bsp_display_unlock();
            session->used = false;
            return ESP_ERR_NO_MEM;
        }
        lv_screen_load(session->screen);
        exit_overlay_create();
        notify_state(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED);
    } else if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW) {
        screen_stack_push(lv_screen_active());
        /* Keep the LVGL lock for the whole raw session. The LVGL task blocks
         * while the Lua app owns the panel, so it cannot flush over our
         * direct esp_lcd_panel_draw_bitmap() writes. */
        (void)lvgl_port_stop();
        session->raw_active = true;
        session->raw_lock_held = true;
        notify_state(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED);
        ESP_LOGI(TAG, "session open owner=%s mode=raw", session->owner);
        *ret_session = session;
        return ESP_OK;
    } else {
        session->screen = s_default_screen;
    }
    bsp_display_unlock();
    ESP_LOGI(TAG, "session open owner=%s mode=%d", session->owner, (int)session->mode);
    *ret_session = session;
    return ESP_OK;
}

esp_err_t display_service_close(display_service_session_handle_t handle)
{
    display_service_session_t *session = session_from_handle(handle);
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "session close owner=%s mode=%d", session->owner, (int)session->mode);
    if (session->raw_lock_held) {
        if (session->cleanup_cb) {
            session->cleanup_cb(handle, session->user_ctx);
        }
        lv_obj_t *restore_screen = screen_stack_pop();
        if (restore_screen) {
            lv_screen_load(restore_screen);
            lv_obj_invalidate(restore_screen);
        }
        exit_overlay_remove();
        session->raw_active = false;
        session->raw_lock_held = false;
        session->active = false;
        session->used = false;
        session->screen = NULL;
        session->canvas = NULL;
        bsp_display_unlock();
        (void)lvgl_port_resume();
        notify_state(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED);
        return ESP_OK;
    }
    if (bsp_display_lock(0)) {
        if (session->cleanup_cb) {
            session->cleanup_cb(handle, session->user_ctx);
        }
        lv_obj_t *restore_screen = NULL;
        if (session->mode != DISPLAY_SERVICE_MODE_SHARED_LVGL) {
            restore_screen = screen_stack_pop();
        }
        /* The Lua LVGL cleanup callback deletes its own root screen and
         * objects. Only delete the placeholder screen when no cleanup owner
         * was supplied. */
        if (session->screen && session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL &&
            session->cleanup_cb == NULL) {
            lv_obj_delete(session->screen);
        }
        session->screen = NULL;
        session->canvas = NULL;
        if (restore_screen) {
            lv_screen_load(restore_screen);
        } else if (s_default_screen &&
                   (session->flags & DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE)) {
            lv_screen_load(s_default_screen);
        }
        if (session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
            exit_overlay_remove();
            notify_state(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED);
        } else if (session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW) {
            notify_state(DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED);
        }
        bsp_display_unlock();
    }
    heap_caps_free(session->raw_buf);
    session->raw_buf = NULL;
    session->used = false;
    session->active = false;
    return ESP_OK;
}

bool display_service_session_is_valid(display_service_session_handle_t handle)
{
    return session_from_handle(handle) != NULL;
}

bool display_service_session_is_active(display_service_session_handle_t handle)
{
    display_service_session_t *session = session_from_handle(handle);
    return session && session->active;
}

display_service_mode_t display_service_session_mode(display_service_session_handle_t handle)
{
    display_service_session_t *session = session_from_handle(handle);
    return session ? session->mode : DISPLAY_SERVICE_MODE_SHARED_LVGL;
}

const char *display_service_session_owner_name(display_service_session_handle_t handle)
{
    display_service_session_t *session = session_from_handle(handle);
    return session ? session->owner : "";
}

esp_err_t display_service_session_load_screen(display_service_session_handle_t handle,
                                              lv_obj_t *screen)
{
    if (!display_service_session_is_valid(handle) || !screen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    display_service_session_t *session = session_from_handle(handle);
    session->screen = screen;
    lv_screen_load(screen);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t handle,
                                                     lv_obj_t *screen)
{
    if (!display_service_session_is_valid(handle) || !screen) {
        return ESP_ERR_INVALID_ARG;
    }
    display_service_session_t *session = session_from_handle(handle);
    session->screen = screen;
    lv_screen_load(screen);
    return ESP_OK;
}

lv_display_t *display_service_session_display(display_service_session_handle_t handle)
{
    (void)handle;
    return lv_display_get_default();
}

esp_err_t display_service_session_get_info(display_service_session_handle_t handle,
                                           display_service_info_t *info)
{
    display_service_session_t *session = session_from_handle(handle);
    if (!session || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    info->width = session->width;
    info->height = session->height;
    info->bits_per_pixel = 16;
    info->panel_interface = DISPLAY_SERVICE_PANEL_INTERFACE_DSI;
    info->touch_available = bsp_display_get_input_dev() != NULL;
    return ESP_OK;
}

esp_err_t display_service_session_raw_blit(display_service_session_handle_t handle,
                                           const display_service_raw_blit_t *blit)
{
    display_service_session_t *session = session_from_handle(handle);
    if (!session || !session->raw_active || !blit || !blit->frame_buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_lcd_panel_handle_t panel = bsp_display_get_panel_handle();
    if (!panel) {
        return ESP_ERR_INVALID_STATE;
    }

    int x0 = blit->x_start < 0 ? 0 : blit->x_start;
    int y0 = blit->y_start < 0 ? 0 : blit->y_start;
    int x1 = blit->x_end > session->width ? session->width : blit->x_end;
    int y1 = blit->y_end > session->height ? session->height : blit->y_end;
    if (x1 <= x0 || y1 <= y0) {
        return ESP_OK;
    }

    size_t width = (size_t)(x1 - x0);
    size_t src_stride = blit->stride_bytes ? blit->stride_bytes : width * 2;
    const uint8_t *src = blit->frame_buffer;

    if (src_stride == width * 2) {
        return esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, src);
    }

    /* Strided source: copy one row at a time; the full-screen buffer is never
     * duplicated. */
    uint8_t *row = heap_caps_malloc(width * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!row) {
        return ESP_ERR_NO_MEM;
    }
    for (int y = y0; y < y1; y++) {
        memcpy(row, src + (size_t)(y - y0) * src_stride, width * 2);
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x0, y, x1, y + 1, row);
        if (err != ESP_OK) {
            heap_caps_free(row);
            return err;
        }
    }
    heap_caps_free(row);
    return ESP_OK;
}

esp_err_t display_service_session_get_touch_snapshot(display_service_session_handle_t handle,
                                                     display_service_touch_snapshot_t *snapshot)
{
    if (!display_service_session_is_valid(handle) || !snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_last_touch;
    return ESP_OK;
}

bool display_service_has_exclusive_session(void)
{
    return any_exclusive();
}

bool display_service_exclusive_allows_system_overlay(void)
{
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        if (s_sessions[i].used && s_sessions[i].active &&
            s_sessions[i].mode != DISPLAY_SERVICE_MODE_SHARED_LVGL &&
            (s_sessions[i].flags & DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY)) {
            return true;
        }
    }
    return false;
}

esp_err_t display_service_lock(void)
{
    return bsp_display_lock(0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_service_unlock(void)
{
    bsp_display_unlock();
}

esp_err_t display_service_add_touch_observer(display_service_touch_observer_cb_t cb,
                                             void *user_ctx,
                                             display_service_touch_observer_handle_t *ret_handle)
{
    if (!cb || !ret_handle || s_touch_observer_count >= DISPLAY_SERVICE_TOUCH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_touch_mux);
    size_t index = s_touch_observer_count;
    s_touch_observers[index].cb = cb;
    s_touch_observers[index].user_ctx = user_ctx;
    s_touch_observer_count++;
    taskEXIT_CRITICAL(&s_touch_mux);
    touch_timer_start();
    *ret_handle = (display_service_touch_observer_handle_t)(index + 1);
    return ESP_OK;
}

esp_err_t display_service_remove_touch_observer(display_service_touch_observer_handle_t handle)
{
    if (handle == 0 || handle > s_touch_observer_count) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_touch_mux);
    s_touch_observers[handle - 1].cb = NULL;
    s_touch_observers[handle - 1].user_ctx = NULL;
    taskEXIT_CRITICAL(&s_touch_mux);
    return ESP_OK;
}

esp_err_t display_service_set_state_observer(display_service_state_observer_cb_t cb,
                                             void *user_ctx)
{
    s_state_cb = cb;
    s_state_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t display_service_set_default_screen(lv_obj_t *screen)
{
    if (!screen) {
        return ESP_ERR_INVALID_ARG;
    }
    s_default_screen = screen;
    return ESP_OK;
}

void display_service_set_default_screen_locked(lv_obj_t *screen)
{
    s_default_screen = screen;
}

lv_obj_t *display_service_default_screen(void)
{
    return s_default_screen;
}

void display_service_exclusive_raw_suspend_locked(void)
{
}

void display_service_exclusive_raw_resume_locked(void)
{
}

esp_err_t display_service_request_exit(void)
{
    for (size_t i = 0; i < DISPLAY_SERVICE_MAX_SESSIONS; i++) {
        display_service_session_t *session = &s_sessions[i];
        if (session->used && session->active &&
            session->mode != DISPLAY_SERVICE_MODE_SHARED_LVGL &&
            session->exit_request_cb) {
            session->exit_request_cb(session, session->user_ctx);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
