#include "ai_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ai_assets.h"
#include "ai_audio.h"
#include "ai_audio_internal.h"
#include "ai_net.h"
#include "ai_settings.h"
#include "ai_system_info.h"
#include "ai_ui.h"
#include "mcp_server.h"
#include "esp_app_desc.h"
#include "audio_service.h"
#include "es8311_audio_codec.h"
#include "protocol.h"
#include "websocket_protocol.h"

extern "C" {
#include "hw.h"
#include "settings.h"
}

#define EVT_WAKE          (1 << 0)
#define EVT_SEND          (1 << 1)
#define EVT_UI_LISTEN     (1 << 2)
#define EVT_UI_STOP       (1 << 3)
#define EVT_UI_ABORT      (1 << 4)
#define EVT_UI_RETRY      (1 << 5)
#define EVT_NET_CHANGED   (1 << 6)
#define EVT_CHANNEL_CLOSED (1 << 7)
#define EVT_NET_ERROR     (1 << 8)
#define EVT_ALL (EVT_WAKE | EVT_SEND | EVT_UI_LISTEN | EVT_UI_STOP | \
                 EVT_UI_ABORT | EVT_UI_RETRY | EVT_NET_CHANGED | \
                 EVT_CHANNEL_CLOSED | EVT_NET_ERROR)

enum {
    JSON_STT = 1,
    JSON_TTS_START,
    JSON_TTS_SENTENCE,
    JSON_TTS_STOP,
    JSON_LLM,
    JSON_ALERT,
    JSON_GOODBYE,
    JSON_MCP,
};

enum {
    MCP_INITIALIZE = 1,
    MCP_TOOLS_LIST,
    MCP_TOOLS_CALL,
};

static const char *TAG = "ai.app";

AiApp &AiApp::GetInstance()
{
    static AiApp instance;
    return instance;
}

esp_err_t AiApp::Start()
{
    if (running_.exchange(true)) {
        return ESP_OK;
    }

    muted_ = ai_settings_muted();
    ai_audio_set_mute(muted_);

    events_ = xEventGroupCreate();
    json_queue_ = xQueueCreate(8, sizeof(AiJsonEvent));
    if (events_ == nullptr || json_queue_ == nullptr) {
        running_ = false;
        return ESP_ERR_NO_MEM;
    }

    Es8311AudioCodec *codec = ai_audio_codec();
    if (codec == nullptr) {
        running_ = false;
        SetError("audio codec not ready");
        return ESP_FAIL;
    }

    audio_service_ = std::make_unique<AudioService>();
    AudioServiceCallbacks callbacks = {};
    callbacks.on_send_queue_available = [this]() {
        if (events_) xEventGroupSetBits(events_, EVT_SEND);
    };
    callbacks.on_wake_word_detected = [this](const std::string &word) {
        ESP_LOGI(TAG, "wake word detected: %s", word.c_str());
        if (events_) xEventGroupSetBits(events_, EVT_WAKE);
    };
    callbacks.on_vad_change = [](bool speaking) { (void)speaking; };

    audio_service_->SetCallbacks(callbacks);
    audio_service_->Initialize(codec);
    audio_service_->SetModelsList(ai_assets_models());
    audio_service_->Start();
    codec->SetOutputVolume(ai_audio_get_volume());
    ai_audio_set_play_pcm_callback([this](std::vector<int16_t> &&pcm) {
        return audio_service_ != nullptr && audio_service_->PlayPcm(std::move(pcm));
    });

    McpServer::GetInstance().SetSender([this](const std::string &payload) {
        if (protocol_ != nullptr && protocol_open_) {
            protocol_->SendMcpMessage(payload);
        }
    });
    McpServer::GetInstance().AddCommonTools();

    ai_net_start();
    ai_net_set_callback([this](bool online) {
        (void)online;
        if (events_) xEventGroupSetBits(events_, EVT_NET_CHANGED);
    });

    SetState(AI_STATE_STARTING);
    BaseType_t ok = xTaskCreate([](void *arg) {
        static_cast<AiApp *>(arg)->Task();
        vTaskDelete(nullptr);
    }, "ai_app", 16384, this, 6, &task_);
    if (ok != pdPASS) {
        running_ = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void AiApp::Stop()
{
    running_ = false;
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }
    protocol_open_ = false;
    if (audio_service_) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->Stop();
    }
    state_ = AI_STATE_DISABLED;
}

bool AiApp::IsBusy() const
{
    switch (state_.load()) {
    case AI_STATE_ACTIVATING:
    case AI_STATE_CONNECTING:
    case AI_STATE_LISTENING:
    case AI_STATE_SPEAKING:
        return true;
    default:
        return false;
    }
}

std::string AiApp::ActivationCode()
{
    std::lock_guard<std::mutex> lock(text_mutex_);
    return activation_code_;
}

std::string AiApp::LastError()
{
    std::lock_guard<std::mutex> lock(text_mutex_);
    return error_;
}

void AiApp::SetActivationCode(const std::string &code)
{
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        activation_code_ = code;
    }
}

void AiApp::ClearActivationCode()
{
    std::lock_guard<std::mutex> lock(text_mutex_);
    activation_code_.clear();
}

void AiApp::SetState(ai_state_t state)
{
    ai_state_t old = state_.exchange(state);
    if (old != state) {
        ai_ui_on_state(state);
    }
}

void AiApp::SetError(const char *message)
{
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_ = message != nullptr ? message : "unknown error";
    }
    PlayPrompt("exclamation.ogg");
    ai_ui_on_error(message);
    SetState(AI_STATE_ERROR);
}

void AiApp::PlayPrompt(const char *name)
{
#if CONFIG_TEMPLATE_XIAOZHI_CHIME
    if (audio_service_ == nullptr || muted_ || name == nullptr) return;
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "prompt missing: %s", path);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    rewind(file);
    if (size <= 0) {
        fclose(file);
        return;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (buf == nullptr) {
        fclose(file);
        return;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (got == (size_t)size) {
        ESP_LOGI(TAG, "play prompt %s (%ld bytes)", name, size);
        audio_service_->PlaySound(std::string_view((const char *)buf, (size_t)size));
    }
    free(buf);
#endif
}

EventBits_t AiApp::WaitEvents(TickType_t ticks)
{
    if (events_ == nullptr) return 0;
    return xEventGroupWaitBits(events_, EVT_ALL, pdTRUE, pdFALSE, ticks);
}

void AiApp::SyncIdleAudio()
{
    if (!audio_service_) return;
    if (muted_) {
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->EnableVoiceProcessing(false);
        return;
    }
    audio_service_->EnableVoiceProcessing(false);
#if CONFIG_TEMPLATE_XIAOZHI_WAKE_WORD
    audio_service_->EnableWakeWordDetection(true);
#else
    audio_service_->EnableWakeWordDetection(false);
#endif
}

void AiApp::Task()
{
    SetState(AI_STATE_STARTING);

    while (running_) {
        if (!ai_net_is_online()) {
            if (protocol_open_) {
                CloseSession();
            }
            SetState(AI_STATE_STARTING);
            ai_net_wait_online(1000);
            continue;
        }

        if (state_.load() == AI_STATE_ERROR) {
            EventBits_t bits = WaitEvents(pdMS_TO_TICKS(200));
            if (!running_) break;
            if (bits & (EVT_UI_RETRY | EVT_UI_LISTEN | EVT_WAKE | EVT_NET_CHANGED)) {
                {
                    std::lock_guard<std::mutex> lock(text_mutex_);
                    error_.clear();
                }
                SetState(AI_STATE_IDLE);
            }
            continue;
        }

        if (!activated_) {
            if (!RunActivation()) {
                if (running_ && state_.load() != AI_STATE_ERROR) {
                    SetError("activation failed");
                }
                continue;
            }
            continue;
        }

        SetState(AI_STATE_IDLE);
        SyncIdleAudio();

        EventBits_t bits = WaitEvents(pdMS_TO_TICKS(200));
        if (!running_) break;
        if (bits & EVT_NET_CHANGED) {
            if (!ai_net_is_online()) continue;
        }
        if (bits & (EVT_WAKE | EVT_UI_LISTEN | EVT_UI_RETRY)) {
            if (StartSession()) {
                SessionLoop();
            }
        }
    }

    CloseSession();
    SetState(AI_STATE_DISABLED);
}

bool AiApp::RunActivation()
{
    SetState(AI_STATE_ACTIVATING);
    ClearActivationCode();

    if (AiOta::ApplyDirectWs()) {
        activated_ = true;
        return true;
    }

    const int64_t start_ms = esp_timer_get_time() / 1000;
    bool have_cached = ota_.HasWebsocketConfig();
    bool announced = false;
    bool saw_activation = false;

    while (running_) {
        if (!ai_net_is_online()) {
            return false;
        }

        esp_err_t err = ota_.CheckVersion();
        if (err == ESP_OK) {
            if (ota_.HasActivationCode()) {
                saw_activation = true;
                SetActivationCode(ota_.ActivationCode());
                ai_ui_on_activation(ota_.ActivationCode().c_str(),
                                    ota_.ActivationMessage().c_str());
                if (!announced) PlayPrompt("activation.ogg");
                announced = true;
            } else if (ota_.ActivationMessage().size() > 0 && !announced) {
                ai_ui_on_activation("", ota_.ActivationMessage().c_str());
                announced = true;
            }
            if (ota_.HasChallenge()) {
                saw_activation = true;
                ota_.Activate();
            }
            if (!ota_.HasActivationCode() && !ota_.HasChallenge() &&
                ota_.HasWebsocketConfig()) {
                activated_ = true;
                ClearActivationCode();
                PlayPrompt("success.ogg");
                ESP_LOGI(TAG, "activation complete");
#if CONFIG_TEMPLATE_XIAOZHI_AUTO_LISTEN
                RequestListen();
#endif
                return true;
            }
        } else if (have_cached && !saw_activation) {
            ESP_LOGW(TAG, "OTA failed, using cached websocket config");
            activated_ = true;
            return true;
        } else {
            ESP_LOGW(TAG, "OTA check failed, retrying");
        }

        int64_t elapsed = esp_timer_get_time() / 1000 - start_ms;
        if (elapsed > 300000) {
            SetError("activation timeout");
            return false;
        }

        EventBits_t bits = WaitEvents(pdMS_TO_TICKS(5000));
        if (bits & EVT_NET_CHANGED && !ai_net_is_online()) {
            return false;
        }
        if (!running_) return false;
    }
    return false;
}

bool AiApp::StartSession()
{
    if (!running_ || muted_ || !ai_settings_enabled()) {
        return false;
    }
    if (!ota_.HasWebsocketConfig()) {
        SetError("activation config missing");
        return false;
    }

    SetState(AI_STATE_CONNECTING);
    /* 唤醒/点击后先给反馈，不等 WebSocket 握手完成。 */
    PlayPrompt("popup.ogg");
    if (!protocol_) {
        protocol_ = std::make_unique<WebsocketProtocol>();
        protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
            if (audio_service_) {
                audio_service_->PushPacketToDecodeQueue(std::move(packet));
            }
        });
        protocol_->OnIncomingJson([this](const cJSON *root) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (!cJSON_IsString(type)) return;

            AiJsonEvent event = {};
            if (strcmp(type->valuestring, "stt") == 0) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (!cJSON_IsString(text)) return;
                event.type = JSON_STT;
                strlcpy(event.text, text->valuestring, sizeof(event.text));
            } else if (strcmp(type->valuestring, "tts") == 0) {
                cJSON *state = cJSON_GetObjectItem(root, "state");
                if (!cJSON_IsString(state)) return;
                if (strcmp(state->valuestring, "start") == 0) {
                    event.type = JSON_TTS_START;
                } else if (strcmp(state->valuestring, "stop") == 0) {
                    event.type = JSON_TTS_STOP;
                } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                    cJSON *text = cJSON_GetObjectItem(root, "text");
                    if (!cJSON_IsString(text)) return;
                    event.type = JSON_TTS_SENTENCE;
                    strlcpy(event.text, text->valuestring, sizeof(event.text));
                } else {
                    return;
                }
            } else if (strcmp(type->valuestring, "llm") == 0) {
                cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
                if (!cJSON_IsString(emotion)) return;
                event.type = JSON_LLM;
                strlcpy(event.text, emotion->valuestring, sizeof(event.text));
            } else if (strcmp(type->valuestring, "alert") == 0) {
                cJSON *message = cJSON_GetObjectItem(root, "message");
                if (!cJSON_IsString(message)) return;
                event.type = JSON_ALERT;
                strlcpy(event.text, message->valuestring, sizeof(event.text));
            } else if (strcmp(type->valuestring, "goodbye") == 0) {
                event.type = JSON_GOODBYE;
            } else if (strcmp(type->valuestring, "mcp") == 0) {
                cJSON *payload = cJSON_GetObjectItem(root, "payload");
                if (!cJSON_IsObject(payload)) return;
                cJSON *method = cJSON_GetObjectItem(payload, "method");
                cJSON *id = cJSON_GetObjectItem(payload, "id");
                if (!cJSON_IsString(method) || !cJSON_IsNumber(id)) {
                    return;
                }
                event.type = JSON_MCP;
                event.mcp_id = id->valueint;
                ESP_LOGI(TAG, "MCP request method=%s id=%d", method->valuestring, event.mcp_id);
                if (strcmp(method->valuestring, "initialize") == 0) {
                    event.mcp_kind = MCP_INITIALIZE;
                } else if (strcmp(method->valuestring, "tools/list") == 0) {
                    event.mcp_kind = MCP_TOOLS_LIST;
                } else if (strcmp(method->valuestring, "tools/call") == 0) {
                    cJSON *params = cJSON_GetObjectItem(payload, "params");
                    cJSON *name = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "name") : nullptr;
                    if (!cJSON_IsString(name)) return;
                    event.mcp_kind = MCP_TOOLS_CALL;
                    strlcpy(event.mcp_name, name->valuestring, sizeof(event.mcp_name));
                    event.mcp_arg = -1;
                    event.mcp_value_type = 0;
                    cJSON *args = cJSON_GetObjectItem(params, "arguments");
                    if (cJSON_IsObject(args)) {
                        if (strcmp(name->valuestring, "self.home.set_switch") == 0) {
                            cJSON *on = cJSON_GetObjectItem(args, "on");
                            if (cJSON_IsBool(on) || cJSON_IsNumber(on)) {
                                event.mcp_value_type = 2;
                                event.mcp_arg = (cJSON_IsTrue(on) ||
                                                 (cJSON_IsNumber(on) && on->valueint != 0)) ? 1 : 0;
                            }
                        } else {
                            cJSON *v = cJSON_GetObjectItem(args, "volume");
                            if (!cJSON_IsNumber(v)) v = cJSON_GetObjectItem(args, "brightness");
                            if (cJSON_IsNumber(v)) {
                                event.mcp_value_type = 1;
                                event.mcp_arg = v->valueint;
                            }
                        }
                    }
                } else {
                    return;
                }
            } else {
                return;
            }
            if (json_queue_) xQueueSend(json_queue_, &event, 0);
        });
        protocol_->OnAudioChannelOpened([this]() {
            protocol_open_ = true;
        });
        protocol_->OnAudioChannelClosed([this]() {
            protocol_open_ = false;
            if (events_) xEventGroupSetBits(events_, EVT_CHANNEL_CLOSED);
        });
        protocol_->OnNetworkError([this](const std::string &message) {
            {
                std::lock_guard<std::mutex> lock(text_mutex_);
                error_ = message;
            }
            if (events_) xEventGroupSetBits(events_, EVT_NET_ERROR);
        });
        protocol_->Start();
    }

    if (!protocol_->OpenAudioChannel()) {
        protocol_open_ = false;
        SetError("cannot connect to server");
        return false;
    }

    protocol_open_ = true;
    StartListening();
    return true;
}

void AiApp::StartListening()
{
    if (!protocol_open_ || !protocol_ || muted_) return;
    audio_service_->EnableWakeWordDetection(false);
    audio_service_->EnableVoiceProcessing(true);
    protocol_->SendStartListening(kListeningModeAutoStop);
    SetState(AI_STATE_LISTENING);
}

void AiApp::CloseSession()
{
    if (protocol_ && protocol_open_) {
        protocol_->CloseAudioChannel();
    }
    protocol_open_ = false;
    if (audio_service_) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->EnableWakeWordDetection(false);
    }
}

void AiApp::InterruptSpeaking(bool wake_word)
{
    if (state_.load() != AI_STATE_SPEAKING) {
        return;
    }
    if (protocol_open_ && protocol_) {
        protocol_->SendAbortSpeaking(wake_word ? kAbortReasonWakeWordDetected
                                               : kAbortReasonNone);
    }
    ignore_tts_stop_ = true;
    if (audio_service_) {
        audio_service_->ResetDecoder();
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->WaitForPlaybackQueueEmpty();
    }
    StartListening();
}

void AiApp::SessionLoop()
{
    while (running_ && protocol_open_) {
        DrainJson();
        if (!protocol_open_) break;

        if (!DrainSend()) {
            SetError("audio send failed");
            CloseSession();
            return;
        }

        EventBits_t bits = WaitEvents(pdMS_TO_TICKS(100));
        if (!running_) break;

        if (bits & EVT_NET_CHANGED) {
            if (!ai_net_is_online()) {
                CloseSession();
                SetState(AI_STATE_STARTING);
                return;
            }
        }
        if (bits & EVT_NET_ERROR) {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(text_mutex_);
                message = error_;
            }
            CloseSession();
            SetError(message.c_str());
            return;
        }
        if (bits & EVT_CHANNEL_CLOSED) {
            protocol_open_ = false;
            SetState(AI_STATE_IDLE);
            SyncIdleAudio();
            return;
        }
        if (bits & EVT_UI_STOP) {
            CloseSession();
            SetState(AI_STATE_IDLE);
            SyncIdleAudio();
            return;
        }
        if (bits & EVT_UI_ABORT) {
            InterruptSpeaking(false);
        }
        if (bits & EVT_UI_LISTEN) {
            if (state_.load() == AI_STATE_SPEAKING) {
                InterruptSpeaking(false);
            } else if (state_.load() == AI_STATE_LISTENING) {
                CloseSession();
                SetState(AI_STATE_IDLE);
                SyncIdleAudio();
                return;
            }
        }
        if (bits & EVT_WAKE) {
            if (state_.load() == AI_STATE_SPEAKING) {
                InterruptSpeaking(true);
            }
        }
        if (bits & EVT_SEND) {
            DrainSend();
        }

        if (protocol_ && !protocol_->IsAudioChannelOpened()) {
            protocol_open_ = false;
            SetState(AI_STATE_IDLE);
            SyncIdleAudio();
            return;
        }
    }
}

void AiApp::HandleMcp(const AiJsonEvent &event)
{
    /* The WebSocket receive task only parsed the MCP request into a compact
     * event. Reconstruct the JSON-RPC message here (AI task, 16 KB stack) and
     * hand it to the official McpServer implementation. */
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", event.mcp_id);

    const char *method = nullptr;
    if (event.mcp_kind == MCP_INITIALIZE) {
        method = "initialize";
    } else if (event.mcp_kind == MCP_TOOLS_LIST) {
        method = "tools/list";
    } else if (event.mcp_kind == MCP_TOOLS_CALL) {
        method = "tools/call";
    }
    if (method == nullptr) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(root, "method", method);

    if (event.mcp_kind == MCP_TOOLS_CALL) {
        cJSON *params = cJSON_AddObjectToObject(root, "params");
        cJSON_AddStringToObject(params, "name", event.mcp_name);
        if (event.mcp_value_type == 1) {
            cJSON *args = cJSON_AddObjectToObject(params, "arguments");
            const char *key = nullptr;
            if (strcmp(event.mcp_name, "self.audio_speaker.set_volume") == 0) {
                key = "volume";
            } else if (strcmp(event.mcp_name, "self.screen.set_brightness") == 0) {
                key = "brightness";
            }
            if (key != nullptr) {
                cJSON_AddNumberToObject(args, key, event.mcp_arg);
            }
        } else if (event.mcp_value_type == 2) {
            cJSON *args = cJSON_AddObjectToObject(params, "arguments");
            cJSON_AddBoolToObject(args, "on", event.mcp_arg != 0);
        }
    }

    McpServer::GetInstance().ParseMessage(root);
    if (event.mcp_kind == MCP_TOOLS_CALL &&
        strncmp(event.mcp_name, "self.home.", 10) == 0) {
        bool on = settings_get()->demo != 0;
        ESP_LOGI(TAG, "home mcp -> demo=%d", on ? 1 : 0);
        ai_ui_on_demo_switch(on);
    }
    cJSON_Delete(root);
}

bool AiApp::DrainSend()
{
    if (!protocol_ || !protocol_open_ || !audio_service_) {
        return true;
    }
    while (auto packet = audio_service_->PopPacketFromSendQueue()) {
        if (!protocol_->SendAudio(std::move(packet))) {
            return false;
        }
    }
    return true;
}

void AiApp::DrainJson()
{
    if (!json_queue_) return;

    AiJsonEvent event;
    while (xQueueReceive(json_queue_, &event, 0) == pdTRUE) {
        switch (event.type) {
        case JSON_STT:
            ai_ui_on_message(AI_ROLE_USER, event.text);
            break;
        case JSON_TTS_START:
            ignore_tts_stop_ = false;
            SetState(AI_STATE_SPEAKING);
            audio_service_->EnableVoiceProcessing(false);
            audio_service_->ResetDecoder();
#if CONFIG_TEMPLATE_XIAOZHI_WAKE_WORD && CONFIG_TEMPLATE_XIAOZHI_WAKE_WORD_IN_SPEAKING
            if (!muted_) audio_service_->EnableWakeWordDetection(true);
#else
            audio_service_->EnableWakeWordDetection(false);
#endif
            break;
        case JSON_TTS_SENTENCE:
            ai_ui_on_message(AI_ROLE_ASSISTANT, event.text);
            break;
        case JSON_TTS_STOP:
            if (ignore_tts_stop_.exchange(false)) {
                break;
            }
            if (state_.load() == AI_STATE_SPEAKING) {
                audio_service_->WaitForPlaybackQueueEmpty();
                if (muted_) {
                    CloseSession();
                    SetState(AI_STATE_IDLE);
                    SyncIdleAudio();
                } else {
                    StartListening();
                }
                ai_ui_on_reply_done();
            }
            break;
        case JSON_LLM:
            ai_ui_on_emotion(event.text);
            break;
        case JSON_ALERT:
            ai_ui_on_message(AI_ROLE_SYSTEM, event.text);
            break;
        case JSON_GOODBYE:
            CloseSession();
            SetState(AI_STATE_IDLE);
            SyncIdleAudio();
            break;
        case JSON_MCP:
            HandleMcp(event);
            break;
        default:
            break;
        }
    }
}

void AiApp::RequestListen()
{
    if (events_) xEventGroupSetBits(events_, EVT_UI_LISTEN);
}

void AiApp::StopSession()
{
    if (events_) xEventGroupSetBits(events_, EVT_UI_STOP);
}

void AiApp::AbortSpeaking()
{
    if (events_) xEventGroupSetBits(events_, EVT_UI_ABORT);
}

void AiApp::Retry()
{
    if (events_) xEventGroupSetBits(events_, EVT_UI_RETRY);
}

void AiApp::SetMuted(bool muted)
{
    muted_ = muted;
    ai_settings_set_muted(muted);
    ai_audio_set_mute(muted);

    if (!audio_service_) return;
    if (muted) {
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->EnableVoiceProcessing(false);
        if (state_.load() == AI_STATE_LISTENING) {
            if (protocol_open_ && protocol_) protocol_->SendStopListening();
            CloseSession();
            SetState(AI_STATE_IDLE);
        }
    } else if (state_.load() == AI_STATE_IDLE) {
        SyncIdleAudio();
    }
}
