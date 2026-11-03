/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_openai_compatible.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "llm/claw_llm_http_transport.h"
#include "llm/media/claw_media_pipeline.h"

typedef struct {
    claw_endpoint_handle_t endpoint;
    char *model;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
} openai_compatible_backend_ctx_t;

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static esp_err_t dup_tool_call_string(cJSON *json, char **out_value)
{
    if (!out_value || !json || !cJSON_IsString(json) || !json->valuestring) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = strdup(json->valuestring);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t parse_chat_response(const char *body,
                                     claw_llm_response_t *out_response,
                                     char **out_error_message)
{
    cJSON *root = NULL;
    cJSON *choices;
    cJSON *choice0;
    cJSON *message;
    cJSON *content;
    cJSON *reasoning_content;
    cJSON *tool_calls;
    cJSON *tool_call;
    size_t tool_count = 0;
    size_t tool_index = 0;

    if (!body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_response, 0, sizeof(*out_response));
    root = cJSON_Parse(body);
    if (!root) {
        *out_error_message = dup_printf("Failed to parse LLM JSON response");
        return ESP_FAIL;
    }

    choices = cJSON_GetObjectItem(root, "choices");
    choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    if (!message || !cJSON_IsObject(message)) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("LLM response missing message");
        return ESP_FAIL;
    }
    {
        cJSON *role = cJSON_GetObjectItem(message, "role");

        if (!cJSON_IsString(role) || !role->valuestring ||
                strcmp(role->valuestring, "assistant") != 0) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("LLM response message is not assistant");
            return ESP_FAIL;
        }
    }

    out_response->raw_message_json = cJSON_PrintUnformatted(message);
    if (!out_response->raw_message_json) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("Out of memory copying LLM raw message");
        return ESP_ERR_NO_MEM;
    }

    content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        out_response->text = strdup(content->valuestring);
        if (!out_response->text) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying LLM response");
            return ESP_ERR_NO_MEM;
        }
    }

    reasoning_content = cJSON_GetObjectItem(message, "reasoning_content");
    if (reasoning_content && cJSON_IsString(reasoning_content)) {
        out_response->reasoning_content = strdup(reasoning_content->valuestring ?
                                                 reasoning_content->valuestring : "");
        if (!out_response->reasoning_content) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying LLM reasoning");
            return ESP_ERR_NO_MEM;
        }
    }

    tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        tool_count = (size_t)cJSON_GetArraySize(tool_calls);
        if (tool_count > 0) {
            out_response->tool_calls = calloc(tool_count, sizeof(claw_llm_tool_call_t));
            if (!out_response->tool_calls) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Out of memory copying tool calls");
                return ESP_ERR_NO_MEM;
            }
            out_response->tool_call_count = tool_count;
        }

        cJSON_ArrayForEach(tool_call, tool_calls) {
            claw_llm_tool_call_t *dst = &out_response->tool_calls[tool_index];
            cJSON *id_json = cJSON_GetObjectItem(tool_call, "id");
            cJSON *function_json = cJSON_GetObjectItem(tool_call, "function");
            cJSON *name_json = function_json ? cJSON_GetObjectItem(function_json, "name") : NULL;
            cJSON *args_json = function_json ? cJSON_GetObjectItem(function_json, "arguments") : NULL;
            esp_err_t err;

            if (!id_json || !function_json || !name_json || !args_json) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Malformed tool call in LLM response");
                return ESP_FAIL;
            }

            err = dup_tool_call_string(id_json, &dst->id);
            if (err == ESP_OK) {
                err = dup_tool_call_string(name_json, &dst->name);
            }
            if (err == ESP_OK) {
                err = dup_tool_call_string(args_json, &dst->arguments_json);
            }
            if (err != ESP_OK) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Out of memory copying tool call");
                return err;
            }
            tool_index++;
        }
    }

    if (!out_response->text && out_response->tool_call_count == 0) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("LLM returned empty text response");
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t build_chat_body(const openai_compatible_backend_ctx_t *ctx,
                                 const claw_llm_model_profile_t *profile,
                                 const claw_llm_chat_request_t *request,
                                 char **out_post_data,
                                 char **out_error_message)
{
    cJSON *body = NULL;
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *item;
    char *post_data = NULL;

    body = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    if (!body || !messages || !system_msg) {
        *out_error_message = dup_printf("Out of memory building request");
        cJSON_Delete(body);
        cJSON_Delete(messages);
        cJSON_Delete(system_msg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, profile->max_tokens_field, ctx->max_tokens);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", request->system_prompt);
    cJSON_AddItemToArray(messages, system_msg);
    system_msg = NULL;

    cJSON_ArrayForEach(item, request->messages) {
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) {
            *out_error_message = dup_printf("Out of memory copying messages");
            cJSON_Delete(body);
            cJSON_Delete(messages);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    cJSON_AddItemToObject(body, "messages", messages);
    messages = NULL;

    if (request->tools_json && request->tools_json[0]) {
        if (!profile->supports_tools) {
            cJSON_Delete(body);
            *out_error_message = dup_printf("Selected backend does not support tool calls");
            return ESP_ERR_NOT_SUPPORTED;
        }
        cJSON *tools = cJSON_Parse(request->tools_json);

        if (!tools || !cJSON_IsArray(tools)) {
            cJSON_Delete(tools);
            cJSON_Delete(body);
            *out_error_message = dup_printf("Invalid tools JSON");
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(body, "tools", tools);
    }

    if (request->on_text_delta != NULL) {
        cJSON_AddBoolToObject(body, "stream", true);
    }

    post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        *out_error_message = dup_printf("Out of memory serializing request");
        return ESP_ERR_NO_MEM;
    }

    *out_post_data = post_data;
    return ESP_OK;
}

static esp_err_t openai_compatible_init(const claw_llm_backend_config_t *config,
                                        const claw_llm_model_profile_t *profile,
                                        claw_endpoint_handle_t endpoint,
                                        void **out_backend_ctx,
                                        char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx;

    if (!config || !profile || !endpoint || !out_backend_ctx || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->model || !config->model[0]) {
        *out_error_message = dup_printf("LLM model is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        *out_error_message = dup_printf("Out of memory allocating backend context");
        return ESP_ERR_NO_MEM;
    }

    ctx->endpoint = endpoint;
    ctx->model = strdup(config->model);
    ctx->timeout_ms = config->timeout_ms;
    ctx->max_tokens = config->max_tokens;
    ctx->image_max_bytes = config->image_max_bytes;
    if (!ctx->model) {
        *out_error_message = dup_printf("Out of memory copying backend config");
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *out_backend_ctx = ctx;
    return ESP_OK;
}

/* ---------- OpenAI-compatible SSE streaming ---------- */

typedef struct {
    const claw_llm_chat_request_t *request;
    char *line;
    size_t line_len;
    size_t line_cap;
    char *content;
    size_t content_len;
    size_t content_cap;
    char *reasoning;
    size_t reasoning_len;
    size_t reasoning_cap;
    cJSON *tool_map;
    int max_tool_index;
    bool saw_data;
    bool done;
    bool failed;
    char error[160];
} openai_stream_t;

static void stream_set_error(openai_stream_t *stream, const char *message)
{
    if (!stream->failed) {
        stream->failed = true;
        snprintf(stream->error, sizeof(stream->error), "%s", message);
    }
}

static esp_err_t stream_append(char **buffer, size_t *length, size_t *capacity,
                               const char *data, size_t size)
{
    if (*length + size + 1 > *capacity) {
        size_t cap = *capacity ? *capacity : 1024;
        while (cap < *length + size + 1) {
            cap *= 2;
        }
        if (cap > 128 * 1024) {
            return ESP_ERR_INVALID_SIZE;
        }
        char *grown = realloc(*buffer, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        *buffer = grown;
        *capacity = cap;
    }
    memcpy(*buffer + *length, data, size);
    *length += size;
    (*buffer)[*length] = '\0';
    return ESP_OK;
}

static cJSON *stream_tool_entry(openai_stream_t *stream, int index)
{
    char key[16];
    snprintf(key, sizeof(key), "%d", index);
    if (!stream->tool_map) {
        stream->tool_map = cJSON_CreateObject();
        if (!stream->tool_map) {
            return NULL;
        }
    }
    cJSON *entry = cJSON_GetObjectItem(stream->tool_map, key);
    if (!entry) {
        entry = cJSON_CreateObject();
        if (!entry) {
            return NULL;
        }
        cJSON_AddStringToObject(entry, "id", "");
        cJSON_AddStringToObject(entry, "name", "");
        cJSON_AddStringToObject(entry, "arguments", "");
        cJSON_AddItemToObject(stream->tool_map, key, entry);
    }
    return entry;
}

static void stream_append_field(cJSON *entry, const char *field,
                                const char *value)
{
    if (!entry || !value || !value[0]) {
        return;
    }
    cJSON *item = cJSON_GetObjectItem(entry, field);
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_AddStringToObject(entry, field, value);
        return;
    }
    size_t old_len = strlen(item->valuestring);
    size_t add_len = strlen(value);
    char *joined = malloc(old_len + add_len + 1);
    if (!joined) {
        return;
    }
    memcpy(joined, item->valuestring, old_len);
    memcpy(joined + old_len, value, add_len + 1);
    cJSON *replacement = cJSON_CreateString(joined);
    free(joined);
    if (replacement) {
        cJSON_ReplaceItemInObject(entry, field, replacement);
    }
}

static esp_err_t stream_process_payload(openai_stream_t *stream,
                                        const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        stream_set_error(stream, "invalid SSE JSON");
        return ESP_FAIL;
    }
    stream->saw_data = true;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;
    if (cJSON_IsObject(delta)) {
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if (cJSON_IsString(content) && content->valuestring &&
            content->valuestring[0]) {
            esp_err_t err = stream_append(&stream->content, &stream->content_len,
                                          &stream->content_cap,
                                          content->valuestring,
                                          strlen(content->valuestring));
            if (err != ESP_OK) {
                cJSON_Delete(root);
                stream_set_error(stream, "stream content too large");
                return err;
            }
            if (stream->request->on_text_delta) {
                stream->request->on_text_delta(
                    content->valuestring, strlen(content->valuestring),
                    stream->request->text_delta_ctx);
            }
        }

        cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
        if (cJSON_IsString(reasoning) && reasoning->valuestring &&
            reasoning->valuestring[0]) {
            (void)stream_append(&stream->reasoning, &stream->reasoning_len,
                                &stream->reasoning_cap,
                                reasoning->valuestring,
                                strlen(reasoning->valuestring));
        }

        cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
        if (cJSON_IsArray(tool_calls)) {
            int position = 0;
            cJSON *call = NULL;
            cJSON_ArrayForEach(call, tool_calls) {
                cJSON *index_json = cJSON_GetObjectItem(call, "index");
                int index = cJSON_IsNumber(index_json) ? index_json->valueint
                                                       : position;
                cJSON *entry = stream_tool_entry(stream, index);
                if (!entry) {
                    cJSON_Delete(root);
                    stream_set_error(stream, "out of memory in tool stream");
                    return ESP_ERR_NO_MEM;
                }
                stream_append_field(entry, "id",
                                    cJSON_GetStringValue(
                                        cJSON_GetObjectItem(call, "id")));
                cJSON *function = cJSON_GetObjectItem(call, "function");
                if (cJSON_IsObject(function)) {
                    stream_append_field(entry, "name",
                                        cJSON_GetStringValue(
                                            cJSON_GetObjectItem(function,
                                                               "name")));
                    stream_append_field(entry, "arguments",
                                        cJSON_GetStringValue(
                                            cJSON_GetObjectItem(function,
                                                               "arguments")));
                }
                if (index > stream->max_tool_index) {
                    stream->max_tool_index = index;
                }
                position++;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t stream_process_line(openai_stream_t *stream, char *line,
                                     size_t length)
{
    while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
    if (length == 0 || line[0] == ':') {
        return ESP_OK;
    }
    if (strncmp(line, "data:", 5) != 0) {
        return ESP_OK;
    }
    const char *payload = line + 5;
    while (*payload == ' ') {
        payload++;
    }
    if (strcmp(payload, "[DONE]") == 0) {
        stream->done = true;
        return ESP_OK;
    }
    if (stream->failed || stream->done) {
        return ESP_OK;
    }
    return stream_process_payload(stream, payload);
}

static esp_err_t stream_on_chunk(const char *data, size_t size, void *ctx)
{
    openai_stream_t *stream = (openai_stream_t *)ctx;
    if (stream->failed) {
        return ESP_FAIL;
    }
    esp_err_t err = stream_append(&stream->line, &stream->line_len,
                                  &stream->line_cap, data, size);
    if (err != ESP_OK) {
        stream_set_error(stream, "SSE line too large");
        return err;
    }

    size_t line_start = 0;
    for (size_t i = 0; i < stream->line_len; i++) {
        if (stream->line[i] != '\n') {
            continue;
        }
        stream->line[i] = '\0';
        err = stream_process_line(stream, stream->line + line_start,
                                  i - line_start);
        if (err != ESP_OK) {
            return err;
        }
        line_start = i + 1;
        if (stream->done) {
            break;
        }
    }
    if (line_start > 0) {
        memmove(stream->line, stream->line + line_start,
                stream->line_len - line_start);
        stream->line_len -= line_start;
        stream->line[stream->line_len] = '\0';
    }
    return ESP_OK;
}

static void stream_free(openai_stream_t *stream)
{
    free(stream->line);
    free(stream->content);
    free(stream->reasoning);
    cJSON_Delete(stream->tool_map);
    memset(stream, 0, sizeof(*stream));
}

static esp_err_t stream_finish(openai_stream_t *stream,
                               claw_llm_response_t *out_response,
                               char **out_error_message)
{
    if (stream->line_len > 0) {
        (void)stream_process_line(stream, stream->line, stream->line_len);
        stream->line_len = 0;
    }
    if (stream->failed) {
        *out_error_message = dup_printf("%s", stream->error);
        return ESP_FAIL;
    }
    if (!stream->saw_data) {
        *out_error_message = dup_printf("LLM stream produced no data");
        return ESP_FAIL;
    }
    if (stream->content_len == 0 && stream->max_tool_index < 0) {
        *out_error_message = dup_printf("LLM returned empty text response");
        return ESP_FAIL;
    }

    cJSON *message = cJSON_CreateObject();
    cJSON *tool_array = NULL;
    if (!message) {
        *out_error_message = dup_printf("Out of memory building response");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content",
                            stream->content ? stream->content : "");
    if (stream->reasoning_len > 0) {
        cJSON_AddStringToObject(message, "reasoning_content", stream->reasoning);
    }

    if (stream->tool_map && stream->max_tool_index >= 0) {
        tool_array = cJSON_CreateArray();
        if (!tool_array) {
            cJSON_Delete(message);
            *out_error_message = dup_printf("Out of memory building tool calls");
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i <= stream->max_tool_index; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%d", i);
            cJSON *entry = cJSON_GetObjectItem(stream->tool_map, key);
            if (!entry) {
                continue;
            }
            const char *name = cJSON_GetStringValue(
                cJSON_GetObjectItem(entry, "name"));
            const char *id = cJSON_GetStringValue(
                cJSON_GetObjectItem(entry, "id"));
            const char *arguments = cJSON_GetStringValue(
                cJSON_GetObjectItem(entry, "arguments"));
            if (!name || !name[0]) {
                continue;
            }
            cJSON *call = cJSON_CreateObject();
            cJSON *function = cJSON_CreateObject();
            if (!call || !function) {
                cJSON_Delete(call);
                cJSON_Delete(function);
                cJSON_Delete(tool_array);
                cJSON_Delete(message);
                *out_error_message = dup_printf("Out of memory building tool call");
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(call, "id",
                                    (id && id[0]) ? id : "call_stream");
            cJSON_AddStringToObject(call, "type", "function");
            cJSON_AddStringToObject(function, "name", name);
            cJSON_AddStringToObject(function, "arguments",
                                    (arguments && arguments[0]) ? arguments : "{}");
            cJSON_AddItemToObject(call, "function", function);
            cJSON_AddItemToArray(tool_array, call);
        }
        if (cJSON_GetArraySize(tool_array) > 0) {
            cJSON_AddItemToObject(message, "tool_calls", tool_array);
            tool_array = NULL;
        } else {
            cJSON_Delete(tool_array);
            tool_array = NULL;
        }
    }

    out_response->raw_message_json = cJSON_PrintUnformatted(message);
    if (!out_response->raw_message_json) {
        cJSON_Delete(message);
        *out_error_message = dup_printf("Out of memory serializing response");
        return ESP_ERR_NO_MEM;
    }
    if (stream->content_len > 0) {
        out_response->text = strdup(stream->content);
        if (!out_response->text) {
            cJSON_Delete(message);
            *out_error_message = dup_printf("Out of memory copying response");
            return ESP_ERR_NO_MEM;
        }
    }
    if (stream->reasoning_len > 0) {
        out_response->reasoning_content = strdup(stream->reasoning);
    }

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tool_calls)) {
        int count = cJSON_GetArraySize(tool_calls);
        if (count > 0) {
            out_response->tool_calls = calloc((size_t)count,
                                              sizeof(claw_llm_tool_call_t));
            if (!out_response->tool_calls) {
                cJSON_Delete(message);
                *out_error_message = dup_printf("Out of memory copying tools");
                return ESP_ERR_NO_MEM;
            }
            out_response->tool_call_count = (size_t)count;
            for (int i = 0; i < count; i++) {
                cJSON *call = cJSON_GetArrayItem(tool_calls, i);
                cJSON *function = cJSON_GetObjectItem(call, "function");
                const char *id = cJSON_GetStringValue(
                    cJSON_GetObjectItem(call, "id"));
                const char *name = cJSON_GetStringValue(
                    cJSON_GetObjectItem(function, "name"));
                const char *arguments = cJSON_GetStringValue(
                    cJSON_GetObjectItem(function, "arguments"));
                out_response->tool_calls[i].id = strdup(id ? id : "");
                out_response->tool_calls[i].name = strdup(name ? name : "");
                out_response->tool_calls[i].arguments_json =
                    strdup(arguments ? arguments : "{}");
                if (!out_response->tool_calls[i].id ||
                    !out_response->tool_calls[i].name ||
                    !out_response->tool_calls[i].arguments_json) {
                    cJSON_Delete(message);
                    *out_error_message = dup_printf("Out of memory copying tool call");
                    return ESP_ERR_NO_MEM;
                }
            }
        }
    }

    cJSON_Delete(message);
    return ESP_OK;
}

static esp_err_t openai_compatible_chat(void *backend_ctx,
                                        const claw_llm_model_profile_t *profile,
                                        const claw_llm_chat_request_t *request,
                                        claw_llm_response_t *out_response,
                                        char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    char *post_data = NULL;
    esp_err_t err;

    if (!ctx || !profile || !request || !request->system_prompt || !request->messages ||
            !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_chat_body(ctx, profile, request, &post_data, out_error_message);
    if (err != ESP_OK) {
        return err;
    }

    http_request.body = post_data;
    http_request.timeout_ms = ctx->timeout_ms;
    http_request.abort_flag = request->abort_flag;

    openai_stream_t stream = {0};
    if (request->on_text_delta != NULL) {
        stream.request = request;
        stream.max_tool_index = -1;
        http_request.on_chunk = stream_on_chunk;
        http_request.chunk_ctx = &stream;
    }

    err = claw_llm_http_post_endpoint(ctx->endpoint, profile->chat_path, &http_request,
                                      &http_response, out_error_message);
    free(post_data);
    if (err != ESP_OK) {
        stream_free(&stream);
        return err;
    }

    if (request->on_text_delta != NULL) {
        if (stream.failed) {
            err = stream_finish(&stream, out_response, out_error_message);
        } else if (!stream.saw_data && http_response.body &&
                   http_response.body[0] == '{') {
            /* Provider ignored stream=true and returned a normal JSON body. */
            err = parse_chat_response(http_response.body, out_response,
                                      out_error_message);
        } else {
            err = stream_finish(&stream, out_response, out_error_message);
        }
        stream_free(&stream);
    } else {
        err = parse_chat_response(http_response.body, out_response,
                                  out_error_message);
    }
    claw_llm_http_response_free(&http_response);
    return err;
}

static esp_err_t openai_compatible_infer_media(void *backend_ctx,
                                               const claw_llm_model_profile_t *profile,
                                               const claw_llm_media_request_t *request,
                                               char **out_text,
                                               char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;
    claw_media_prepared_t prepared = {0};
    claw_llm_response_t response = {0};
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *user_msg = NULL;
    cJSON *content = NULL;
    cJSON *text_block = NULL;
    cJSON *image_block = NULL;
    cJSON *image_value = NULL;
    char *post_data = NULL;
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    cJSON *body = NULL;
    esp_err_t err;

    if (out_text) {
        *out_text = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!ctx || !profile || !request || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!profile->supports_vision) {
        *out_error_message = dup_printf("Selected profile does not support media inference");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!request->user_prompt || !request->user_prompt[0] || !request->media || request->media_count == 0) {
        *out_error_message = dup_printf("media request is incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_media_prepare_asset(&request->media[0],
                                   profile,
                                   ctx->image_max_bytes,
                                   &prepared,
                                   out_error_message);
    if (err != ESP_OK) {
        return err;
    }

    body = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    user_msg = cJSON_CreateObject();
    content = cJSON_CreateArray();
    text_block = cJSON_CreateObject();
    image_block = cJSON_CreateObject();
    image_value = cJSON_CreateObject();
    if (!body || !messages || !system_msg || !user_msg || !content ||
            !text_block || !image_block || !image_value) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory building media request");
        goto cleanup;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, profile->max_tokens_field, ctx->max_tokens);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", request->system_prompt ? request->system_prompt : "");
    cJSON_AddItemToArray(messages, system_msg);
    system_msg = NULL;

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", request->user_prompt);
    cJSON_AddItemToArray(content, text_block);
    text_block = NULL;

    cJSON_AddStringToObject(image_block, "type", "image_url");
    cJSON_AddStringToObject(image_value, "url", prepared.payload);
    cJSON_AddItemToObject(image_block, "image_url", image_value);
    image_value = NULL;
    cJSON_AddItemToArray(content, image_block);
    image_block = NULL;

    cJSON_AddItemToObject(user_msg, "content", content);
    content = NULL;
    cJSON_AddItemToArray(messages, user_msg);
    user_msg = NULL;
    cJSON_AddItemToObject(body, "messages", messages);
    messages = NULL;

    post_data = cJSON_PrintUnformatted(body);
    if (!post_data) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory serializing media request");
        goto cleanup;
    }

    http_request.body = post_data;
    http_request.timeout_ms = ctx->timeout_ms;

    err = claw_llm_http_post_endpoint(ctx->endpoint, profile->chat_path, &http_request,
                                      &http_response, out_error_message);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = parse_chat_response(http_response.body, &response, out_error_message);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!response.text || !response.text[0]) {
        err = ESP_FAIL;
        *out_error_message = dup_printf("LLM returned empty media response");
        goto cleanup;
    }

    *out_text = response.text;
    response.text = NULL;
    err = ESP_OK;

cleanup:
    free(post_data);
    claw_llm_http_response_free(&http_response);
    claw_llm_response_free(&response);
    claw_media_prepared_free(&prepared);
    cJSON_Delete(body);
    cJSON_Delete(messages);
    cJSON_Delete(system_msg);
    cJSON_Delete(user_msg);
    cJSON_Delete(content);
    cJSON_Delete(text_block);
    cJSON_Delete(image_block);
    cJSON_Delete(image_value);
    return err;
}

static void openai_compatible_deinit(void *backend_ctx)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;

    if (!ctx) {
        return;
    }

    free(ctx->model);
    free(ctx);
}

static const claw_llm_backend_vtable_t s_openai_compatible_vtable = {
    .id = CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_ID,
    .init = openai_compatible_init,
    .chat = openai_compatible_chat,
    .infer_media = openai_compatible_infer_media,
    .deinit = openai_compatible_deinit,
};

static const claw_llm_backend_registration_t s_openai_compatible_registration = {
    .id = CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_ID,
    .vtable = &s_openai_compatible_vtable,
    .defaults = {
        .chat_path = CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_CHAT_PATH,
        .max_tokens_field = CLAW_LLM_BACKEND_OPENAI_COMPATIBLE_DEFAULT_MAX_TOKENS_FIELD,
    },
};

const claw_llm_backend_vtable_t *claw_llm_backend_openai_compatible_vtable(void)
{
    return &s_openai_compatible_vtable;
}

const claw_llm_backend_registration_t *claw_llm_backend_openai_compatible_registration(void)
{
    return &s_openai_compatible_registration;
}
