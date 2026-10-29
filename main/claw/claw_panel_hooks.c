/*
 * Bridge from claw.panel.* (Lua task) to the template's settings and UI.
 *
 * ai_ui_on_* only enqueues events for the LVGL task, so these hooks are safe
 * on any task; settings_* writes NVS synchronously (the Lua task runs on an
 * internal-RAM stack by design, so flash writes are allowed there).
 */
#include "claw_panel_hooks.h"

#include <stdio.h>

#include "esp_log.h"
#include "ai_ui.h"
#include "claw_panel.h"
#include "hw.h"
#include "settings.h"

static int clamp_percent(int value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static esp_err_t panel_set_volume(int percent)
{
    settings_set_volume(clamp_percent(percent));
    return ESP_OK;
}

static esp_err_t panel_set_brightness(int percent)
{
    settings_set_brightness(clamp_percent(percent));
    return ESP_OK;
}

static esp_err_t panel_status(char *out, size_t out_size)
{
    const sys_settings_t *cfg = settings_get();
    const char *ip = hw_wifi_ip();
    snprintf(out, out_size,
             "{\"volume\":%u,\"brightness\":%u,\"demo\":%s,\"wifi\":\"%s\",\"ip\":\"%s\"}",
             (unsigned)cfg->volume,
             (unsigned)cfg->brightness,
             cfg->demo ? "true" : "false",
             hw_wifi_is_connected() ? "connected" : "disconnected",
             ip != NULL ? ip : "");
    return ESP_OK;
}

static esp_err_t panel_show_message(const char *text)
{
    ai_ui_on_message(AI_ROLE_SYSTEM, text);
    return ESP_OK;
}

static esp_err_t panel_demo(bool on)
{
    ESP_LOGI("claw.panel", "demo switch -> %d", on ? 1 : 0);
    settings_set_demo(on);
    ai_ui_on_demo_switch(on);
    return ESP_OK;
}

void claw_panel_hooks_init(void)
{
    static const claw_panel_hooks_t hooks = {
        .set_volume = panel_set_volume,
        .set_brightness = panel_set_brightness,
        .status = panel_status,
        .show_message = panel_show_message,
        .demo = panel_demo,
    };
    (void)claw_panel_set_hooks(&hooks);
}
