#include "ai.h"

#include <cstring>

#include "sdkconfig.h"

#if CONFIG_TEMPLATE_ENABLE_XIAOZHI

#include "ai_app.h"
#include "ai_assets.h"
#include "ai_audio.h"
#include "ai_settings.h"
#include "ai_system_info.h"

extern "C" esp_err_t ai_early_init(void)
{
    ai_settings_init();
    if (!ai_settings_enabled()) {
        return ESP_OK;
    }
    return ai_assets_init();
}

extern "C" esp_err_t ai_start(void)
{
    if (!ai_settings_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return AiApp::GetInstance().Start();
}

extern "C" void ai_stop(void)
{
    AiApp::GetInstance().Stop();
}

extern "C" bool ai_available(void)
{
    return true;
}

extern "C" bool ai_enabled(void)
{
    return ai_settings_enabled();
}

extern "C" void ai_set_enabled(bool enabled)
{
    ai_settings_set_enabled(enabled);
}

extern "C" ai_state_t ai_get_state(void)
{
    return AiApp::GetInstance().State();
}

extern "C" bool ai_is_busy(void)
{
    return AiApp::GetInstance().IsBusy();
}

extern "C" bool ai_is_activated(void)
{
    return AiApp::GetInstance().IsActivated();
}

extern "C" const char *ai_get_activation_code(void)
{
    static char code[32];
    std::string value = AiApp::GetInstance().ActivationCode();
    strlcpy(code, value.c_str(), sizeof(code));
    return code;
}

extern "C" const char *ai_get_error(void)
{
    static char error[192];
    std::string value = AiApp::GetInstance().LastError();
    strlcpy(error, value.c_str(), sizeof(error));
    return error;
}

extern "C" const char *ai_get_device_id(void)
{
    return ai_device_id();
}

extern "C" const char *ai_get_ota_url(void)
{
    return ai_settings_ota_url();
}

extern "C" bool ai_set_ota_url(const char *url)
{
    return ai_settings_set_ota_url(url) == ESP_OK;
}

extern "C" bool ai_set_direct_ws(const char *url, const char *token)
{
    return ai_settings_set_direct_ws(url, token) == ESP_OK;
}

extern "C" void ai_clear_activation(void)
{
    ai_settings_clear_activation();
}

extern "C" void ai_request_listen(void)
{
    AiApp::GetInstance().RequestListen();
}

extern "C" void ai_stop_session(void)
{
    AiApp::GetInstance().StopSession();
}

extern "C" void ai_abort_speaking(void)
{
    AiApp::GetInstance().AbortSpeaking();
}

extern "C" void ai_retry(void)
{
    AiApp::GetInstance().Retry();
}

extern "C" void ai_set_muted(bool muted)
{
    if (ai_get_state() == AI_STATE_DISABLED) {
        ai_settings_set_muted(muted);
        ai_audio_set_mute(muted);
        return;
    }
    AiApp::GetInstance().SetMuted(muted);
}

extern "C" bool ai_is_muted(void)
{
    return ai_settings_muted();
}

extern "C" bool ai_wake_word_enabled(void)
{
    return ai_settings_wake_word();
}

extern "C" void ai_set_wake_word_enabled(bool enabled)
{
    ai_settings_set_wake_word(enabled);
}

#endif
