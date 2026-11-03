#pragma once

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ui_page_t page_settings;

/* Open the settings page on the 小智 AI category. The rail entry itself is
 * reached from the AI Dynamic Island in the title bar. */
void page_settings_open_ai(void);

#ifdef __cplusplus
}
#endif
