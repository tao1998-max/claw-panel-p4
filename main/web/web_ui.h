#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the ESP-Claw Web management UI on port 80. Safe to call once after
 * Claw and network init; failure is logged and does not affect the device. */
void web_ui_start(void);

#ifdef __cplusplus
}
#endif
