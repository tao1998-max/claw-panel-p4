#pragma once

#include <string>

#include "esp_err.h"

/* 只做小智 OTA/激活检查，绝不下载或写入固件。 */
class AiOta {
public:
    AiOta();
    esp_err_t CheckVersion();
    esp_err_t Activate();

    bool HasWebsocketConfig() const;
    bool HasActivationCode() const { return has_code_; }
    bool HasChallenge() const { return has_challenge_; }

    const std::string &ActivationCode() const { return code_; }
    const std::string &ActivationMessage() const { return message_; }

    static std::string GetOtaUrl();
    static bool ApplyDirectWs();

private:
    bool has_ws_ = false;
    bool has_code_ = false;
    bool has_challenge_ = false;
    std::string code_;
    std::string message_;
    std::string challenge_;

    void ParseResponse(const char *json, size_t len);
    void StoreWebsocket(const char *url, const char *token, int version);
};
