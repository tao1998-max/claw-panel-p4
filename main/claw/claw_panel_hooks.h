#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect claw.panel.* to the template's settings/UI.
 * Called from app_start() after settings_apply() and ui_init().
 */
void claw_panel_hooks_init(void);

#ifdef __cplusplus
}
#endif
