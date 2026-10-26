#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ai.h"
#include "ai_ota.h"

class AudioService;
class WebsocketProtocol;

struct AiJsonEvent {
    uint8_t type;
    uint8_t role;
    uint8_t mcp_kind;
    uint8_t mcp_value_type; /* 0=none, 1=int, 2=bool */
    int32_t mcp_id;
    int32_t mcp_arg;
    char mcp_name[48];
    char text[1024];
};

class AiApp {
public:
    static AiApp &GetInstance();

    esp_err_t Start();
    void Stop();

    ai_state_t State() const { return state_.load(); }
    bool IsBusy() const;
    bool IsActivated() const { return activated_.load(); }
    bool IsMuted() const { return muted_.load(); }

    std::string ActivationCode();
    std::string LastError();

    void RequestListen();
    void StopSession();
    void AbortSpeaking();
    void Retry();
    void SetMuted(bool muted);

private:
    AiApp() = default;
    ~AiApp() = default;
    AiApp(const AiApp &) = delete;
    AiApp &operator=(const AiApp &) = delete;

    void Task();
    bool RunActivation();
    bool StartSession();
    void SessionLoop();
    void StartListening();
    void CloseSession();
    void InterruptSpeaking(bool wake_word);
    void SetState(ai_state_t state);
    void SetError(const char *message);
    void DrainJson();
    void HandleMcp(const AiJsonEvent &event);
    bool DrainSend();
    void SyncIdleAudio();
    void PlayPrompt(const char *name);
    EventBits_t WaitEvents(TickType_t ticks);

    void SetActivationCode(const std::string &code);
    void ClearActivationCode();

    TaskHandle_t task_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    QueueHandle_t json_queue_ = nullptr;

    std::unique_ptr<AudioService> audio_service_;
    std::unique_ptr<WebsocketProtocol> protocol_;
    AiOta ota_;

    std::atomic<ai_state_t> state_{AI_STATE_DISABLED};
    std::atomic<bool> running_{false};
    std::atomic<bool> activated_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> protocol_open_{false};
    std::atomic<bool> ignore_tts_stop_{false};

    std::mutex text_mutex_;
    std::string activation_code_;
    std::string error_;
    std::string last_wake_word_;
    uint32_t last_ping_ms_ = 0;
};
