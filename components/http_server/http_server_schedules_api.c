/*
 * Scheduler management API used by the Web UI. The panel Claw page and the
 * Web page operate on the same cap_scheduler entries, so both stay in sync.
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "cap_scheduler.h"
#include "esp_heap_caps.h"

#define SCHEDULE_JSON_MAX 16384
#define SCHEDULE_URI_PREFIX "/api/schedules/"

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t schedules_list_handler(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(SCHEDULE_JSON_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = cap_scheduler_list_json(buf, SCHEDULE_JSON_MAX);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to list schedules");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t send_err = httpd_resp_sendstr(req, buf);
    heap_caps_free(buf);
    return send_err;
}

static bool parse_schedule_uri(const char *uri, char *id, size_t id_size,
                               char *action, size_t action_size)
{
    const char *p = uri + strlen(SCHEDULE_URI_PREFIX);
    size_t id_len = strcspn(p, "/");
    if (id_len == 0 || id_len >= id_size) {
        return false;
    }
    memcpy(id, p, id_len);
    id[id_len] = '\0';
    p += id_len;
    if (*p == '/') {
        p++;
    }
    strlcpy(action, p, action_size);
    return true;
}

static esp_err_t schedule_action_handler(httpd_req_t *req)
{
    char id[CAP_SCHEDULER_ID_LEN];
    char action[24];
    if (!parse_schedule_uri(req->uri, id, sizeof(id), action, sizeof(action))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid schedule id");
    }

    esp_err_t err = ESP_OK;
    if (req->method == HTTP_DELETE) {
        err = cap_scheduler_remove(id);
    } else if (strcmp(action, "trigger") == 0) {
        err = cap_scheduler_trigger_now(id);
    } else if (strcmp(action, "enabled") == 0) {
        cJSON *body = NULL;
        err = http_server_parse_json_body(req, &body);
        cJSON *enabled = err == ESP_OK ? cJSON_GetObjectItem(body, "enabled") : NULL;
        if (err == ESP_OK && !cJSON_IsBool(enabled)) {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            err = cap_scheduler_enable(id, cJSON_IsTrue(enabled));
        }
        cJSON_Delete(body);
    } else {
        err = ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        cJSON *body = cJSON_CreateObject();
        if (body != NULL) {
            cJSON_AddStringToObject(body, "error", esp_err_to_name(err));
            char *text = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            if (text != NULL) {
                httpd_resp_sendstr(req, text);
                cJSON_free(text);
                return ESP_OK;
            }
        }
        return httpd_resp_sendstr(req, "{\"error\":\"schedule_action_failed\"}");
    }
    return send_ok(req);
}

esp_err_t http_server_register_schedule_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/schedules", .method = HTTP_GET,
          .handler = schedules_list_handler },
        { .uri = "/api/schedules/*", .method = HTTP_POST,
          .handler = schedule_action_handler },
        { .uri = "/api/schedules/*", .method = HTTP_DELETE,
          .handler = schedule_action_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
