#include "ai_ota.h"

#include <cstring>
#include <sys/time.h>

#include "ai_net.h"
#include "ai_settings.h"
#include "ai_system_info.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs_settings.h"

static const char *TAG = "ai.ota";

static std::string activation_endpoint(const std::string &base)
{
    if (base.empty()) return base;
    if (base.back() == '/') return base + "activate";
    return base + "/activate";
}

AiOta::AiOta()
{
    Settings settings("websocket", false);
    has_ws_ = !settings.GetString("url").empty();
}

bool AiOta::HasWebsocketConfig() const
{
    Settings settings("websocket", false);
    return !settings.GetString("url").empty();
}

std::string AiOta::GetOtaUrl()
{
    const char *url = ai_settings_ota_url();
    return url != nullptr ? url : "";
}

bool AiOta::ApplyDirectWs()
{
    const char *url = ai_settings_ws_url();
    if (url == nullptr || url[0] == '\0') {
        return false;
    }
    Settings settings("websocket", true);
    settings.SetString("url", url);
    settings.SetString("token", ai_settings_token());
    settings.SetInt("version", 1);
    ESP_LOGI(TAG, "using direct websocket URL");
    return true;
}

void AiOta::StoreWebsocket(const char *url, const char *token, int version)
{
    if (url == nullptr || url[0] == '\0') return;
    Settings settings("websocket", true);
    settings.SetString("url", url);
    settings.SetString("token", token != nullptr ? token : "");
    settings.SetInt("version", version > 0 ? version : 1);
    has_ws_ = true;
    ESP_LOGI(TAG, "websocket config stored, version=%d", version > 0 ? version : 1);
}

esp_err_t AiOta::CheckVersion()
{
    const std::string url = GetOtaUrl();
    if (url.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    auto http = ai_net_get()->CreateHttp(0);
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->SetTimeout(10000);
    http->SetKeepAlive(false);
    http->SetHeader("Activation-Version", "1");
    http->SetHeader("Device-Id", ai_device_id());
    http->SetHeader("Client-Id", ai_device_uuid());
    http->SetHeader("User-Agent", ai_user_agent());
    http->SetHeader("Accept-Language", "zh-CN");
    http->SetHeader("Content-Type", "application/json");

    char *body = ai_system_info_json();
    if (body == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    http->SetContent(std::string(body));
    ai_system_info_free(body);

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "OTA open failed, err=%d", http->GetLastError());
        http->Close();
        return ESP_FAIL;
    }
    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "OTA status=%d bytes=%u", status, (unsigned)response.size());
    if (status != 200) {
        return ESP_FAIL;
    }
    ParseResponse(response.c_str(), response.size());
    return ESP_OK;
}

void AiOta::ParseResponse(const char *json, size_t len)
{
    (void)len;
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) {
        ESP_LOGE(TAG, "OTA response is not JSON");
        return;
    }

    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *ver = cJSON_GetObjectItem(firmware, "version");
        ESP_LOGW(TAG, "server offered firmware %s, ignored (app OTA disabled)",
                 cJSON_IsString(ver) ? ver->valuestring : "?");
    }

    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    has_code_ = false;
    has_challenge_ = false;
    code_.clear();
    message_.clear();
    challenge_.clear();
    if (cJSON_IsObject(activation)) {
        cJSON *code = cJSON_GetObjectItem(activation, "code");
        cJSON *msg = cJSON_GetObjectItem(activation, "message");
        cJSON *challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(code)) {
            code_ = code->valuestring;
            has_code_ = true;
        }
        if (cJSON_IsString(msg)) {
            message_ = msg->valuestring;
        }
        if (cJSON_IsString(challenge)) {
            challenge_ = challenge->valuestring;
            has_challenge_ = true;
        }
    }

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        cJSON *v = cJSON_GetObjectItem(ws, "version");
        int version = cJSON_IsNumber(v) ? v->valueint : 1;
        if (cJSON_IsString(u) && u->valuestring[0] != '\0') {
            StoreWebsocket(u->valuestring,
                           cJSON_IsString(t) ? t->valuestring : "",
                           version);
        }
    }

    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *ts = cJSON_GetObjectItem(server_time, "timestamp");
        if (cJSON_IsNumber(ts) && ts->valuedouble > 0) {
            struct timeval tv = {
                .tv_sec = (time_t)(ts->valuedouble / 1000.0),
                .tv_usec = (suseconds_t)((int64_t)ts->valuedouble % 1000) * 1000,
            };
            settimeofday(&tv, nullptr);
        }
    }

    cJSON_Delete(root);

    if (has_code_) {
        ESP_LOGI(TAG, "activation code=%s message=%s", code_.c_str(), message_.c_str());
    }
}

esp_err_t AiOta::Activate()
{
    if (!has_challenge_) {
        return ESP_OK;
    }

    const std::string url = activation_endpoint(GetOtaUrl());
    auto http = ai_net_get()->CreateHttp(0);
    if (!http) return ESP_ERR_NO_MEM;
    http->SetTimeout(10000);
    http->SetKeepAlive(false);
    http->SetHeader("Activation-Version", "1");
    http->SetHeader("Device-Id", ai_device_id());
    http->SetHeader("Client-Id", ai_device_uuid());
    http->SetHeader("User-Agent", ai_user_agent());
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string("{}"));

    if (!http->Open("POST", url)) {
        http->Close();
        return ESP_FAIL;
    }
    int status = http->GetStatusCode();
    http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "activate status=%d", status);
    return (status == 200 || status == 202) ? ESP_OK : ESP_FAIL;
}
