/*
 * Panel hook registry. See claw_panel.h for the threading contract.
 */
#include "claw_panel.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_panel";

static claw_panel_hooks_t s_hooks;
static bool s_ready;

esp_err_t claw_panel_set_hooks(const claw_panel_hooks_t *hooks)
{
    if (!hooks) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hooks = *hooks;
    s_ready = true;
    return ESP_OK;
}

bool claw_panel_available(void)
{
    return s_ready;
}

esp_err_t claw_panel_set_volume(int percent)
{
    if (!s_ready || !s_hooks.set_volume) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_hooks.set_volume(percent);
}

esp_err_t claw_panel_set_brightness(int percent)
{
    if (!s_ready || !s_hooks.set_brightness) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_hooks.set_brightness(percent);
}

esp_err_t claw_panel_status(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_hooks.status) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_hooks.status(out, out_size);
}

esp_err_t claw_panel_show_message(const char *text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_hooks.show_message) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_hooks.show_message(text);
}

esp_err_t claw_panel_demo(bool on)
{
    if (!s_ready || !s_hooks.demo) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_hooks.demo(on);
}
