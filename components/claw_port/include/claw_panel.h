/*
 * Panel actions available to Lua/Agent tasks.
 *
 * The runtime never touches LVGL directly: the application registers hooks
 * here (main/claw/claw_panel_hooks.c) and posts UI work through the LVGL
 * thread. When no hooks are registered every call returns an error.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*set_volume)(int percent);
    esp_err_t (*set_brightness)(int percent);
    /** Fill out with a short human readable status (heap, wifi, volume ...). */
    esp_err_t (*status)(char *out, size_t out_size);
    /** Show a message on the panel (chat mini bubble / toast). */
    esp_err_t (*show_message)(const char *text);
    /** Toggle the home page demo switch. */
    esp_err_t (*demo)(bool on);
} claw_panel_hooks_t;

esp_err_t claw_panel_set_hooks(const claw_panel_hooks_t *hooks);
bool claw_panel_available(void);

esp_err_t claw_panel_set_volume(int percent);
esp_err_t claw_panel_set_brightness(int percent);
esp_err_t claw_panel_status(char *out, size_t out_size);
esp_err_t claw_panel_show_message(const char *text);
esp_err_t claw_panel_demo(bool on);

#ifdef __cplusplus
}
#endif
