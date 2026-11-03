/*
 * Doubao TTS status/config/test API.
 *
 * GET  /api/tts/status   -> configured/auth/speaker/resource/auto_speak
 * POST /api/tts/config   -> partial update of the NVS-backed credentials
 * POST /api/tts/speak    -> synthesize and play text synchronously
 *
 * This is a LAN-only management API, like the rest of the Web endpoints.
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_tts.h"

static cJSON *tts_status_json(void)
{
    claw_tts_config_t cfg = {0};
    (void)claw_tts_get_config(&cfg);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    const char *auth = cfg.api_key[0] ? "api_key"
                       : (cfg.app_id[0] && cfg.access_token[0]) ? "app_key"
                       : "none";
    cJSON_AddBoolToObject(root, "configured", claw_tts_is_configured());
    http_server_json_add_string(root, "auth", auth);
    http_server_json_add_string(root, "speaker", cfg.speaker);
    http_server_json_add_string(root, "resource_id", cfg.resource_id);
    cJSON_AddBoolToObject(root, "auto_speak", cfg.auto_speak);
    return root;
}

static esp_err_t tts_status_handler(httpd_req_t *req)
{
    cJSON *root = tts_status_json();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t tts_config_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    claw_tts_config_t cfg = {0};
    (void)claw_tts_get_config(&cfg);

    const cJSON *item = NULL;
    item = cJSON_GetObjectItem(body, "app_id");
    if (cJSON_IsString(item)) {
        strlcpy(cfg.app_id, item->valuestring, sizeof(cfg.app_id));
    }
    item = cJSON_GetObjectItem(body, "access_token");
    if (cJSON_IsString(item)) {
        strlcpy(cfg.access_token, item->valuestring, sizeof(cfg.access_token));
    }
    item = cJSON_GetObjectItem(body, "api_key");
    if (cJSON_IsString(item)) {
        strlcpy(cfg.api_key, item->valuestring, sizeof(cfg.api_key));
    }
    item = cJSON_GetObjectItem(body, "resource_id");
    if (cJSON_IsString(item)) {
        strlcpy(cfg.resource_id, item->valuestring, sizeof(cfg.resource_id));
    }
    item = cJSON_GetObjectItem(body, "speaker");
    if (cJSON_IsString(item)) {
        strlcpy(cfg.speaker, item->valuestring, sizeof(cfg.speaker));
    }
    item = cJSON_GetObjectItem(body, "auto_speak");
    if (cJSON_IsBool(item)) {
        cfg.auto_speak = cJSON_IsTrue(item);
    }
    cJSON_Delete(body);

    err = claw_tts_set_config(&cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to save TTS config");
    }
    cJSON *root = tts_status_json();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    return http_server_send_json_response(req, root);
}

static esp_err_t tts_speak_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }
    const cJSON *text = cJSON_GetObjectItem(body, "text");
    const cJSON *timeout = cJSON_GetObjectItem(body, "timeout_ms");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing text");
    }
    if (strlen(text->valuestring) > 2000) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                                   "Text too long (max 2000 bytes)");
    }

    uint32_t timeout_ms = cJSON_IsNumber(timeout)
                          ? (uint32_t)timeout->valueint : 30000;
    if (timeout_ms < 2000) timeout_ms = 2000;
    if (timeout_ms > 180000) timeout_ms = 180000;

    if (!claw_tts_is_configured()) {
        cJSON_Delete(body);
        httpd_resp_set_status(req, "409 Conflict");
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            return httpd_resp_send_500(req);
        }
        cJSON_AddBoolToObject(root, "ok", false);
        http_server_json_add_string(root, "error", "tts_not_configured");
        return http_server_send_json_response(req, root);
    }

    err = claw_tts_speak(text->valuestring, timeout_ms);
    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        http_server_json_add_string(root, "error", esp_err_to_name(err));
        httpd_resp_set_status(req, "502 Bad Gateway");
    }
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_tts_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/tts/status", .method = HTTP_GET,
          .handler = tts_status_handler },
        { .uri = "/api/tts/config", .method = HTTP_POST,
          .handler = tts_config_handler },
        { .uri = "/api/tts/speak", .method = HTTP_POST,
          .handler = tts_speak_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
