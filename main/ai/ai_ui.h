#pragma once

#include "lvgl.h"
#include "ai.h"

#ifdef __cplusplus
extern "C" {
#endif

void ai_ui_init(void);
void ai_ui_attach(lv_obj_t *root);
void ai_ui_detach(void);
bool ai_ui_is_busy(void);

/* Dynamic CJK font used for chat text; NULL when the AI assets are absent.
 * Other pages may use it to render model output without adding glyphs to the
 * subset fonts. */
const lv_font_t *ai_ui_text_font(void);

/* 由 AI 任务调用；内部只入队，真正改 LVGL 在定时器里。 */
void ai_ui_on_state(ai_state_t state);
void ai_ui_on_message(ai_role_t role, const char *text);
void ai_ui_on_activation(const char *code, const char *message);
void ai_ui_on_error(const char *message);
void ai_ui_on_emotion(const char *emotion);
void ai_ui_on_demo_switch(bool on);
void ai_ui_on_reply_done(void);

#ifdef __cplusplus
}
#endif
