#include "mcp_server.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "settings.h"

extern "C" {
#include "hw.h"
}

#define TAG "MCP"

McpServer::~McpServer()
{
    for (auto *tool : tools_) delete tool;
    tools_.clear();
}

void McpServer::AddCommonTools()
{
    if (common_tools_added_) return;
    common_tools_added_ = true;

    AddTool("self.get_device_status",
            "Get the current speaker volume, screen brightness and network status.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                const sys_settings_t *cfg = settings_get();
                char status[256];
                snprintf(status, sizeof(status),
                         "{\"volume\":%u,\"brightness\":%u,\"demo_switch\":%s,\"wifi\":\"%s\",\"ip\":\"%s\"}",
                         (unsigned)cfg->volume, (unsigned)cfg->brightness,
                         cfg->demo ? "true" : "false",
                         hw_wifi_is_connected() ? "connected" : "disconnected",
                         hw_wifi_ip() != nullptr ? hw_wifi_ip() : "");
                return std::string(status);
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the speaker volume. If the current volume is unknown, call "
            "`self.get_device_status` first.",
            PropertyList({
                Property("volume", kPropertyTypeInteger, 0, 100),
            }),
            [](const PropertyList &properties) -> ReturnValue {
                int volume = properties["volume"].value<int>();
                settings_set_volume(volume);
                ESP_LOGI(TAG, "set volume=%d", volume);
                return true;
            });

    AddTool("self.screen.set_brightness",
            "Set the screen brightness.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100),
            }),
            [](const PropertyList &properties) -> ReturnValue {
                int brightness = properties["brightness"].value<int>();
                settings_set_brightness(brightness);
                ESP_LOGI(TAG, "set brightness=%d", brightness);
                return true;
            });
}

void McpServer::AddTool(McpTool *tool)
{
    if (tool == nullptr) return;
    auto it = std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool *t) {
        return t->name() == tool->name();
    });
    if (it != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        delete tool;
        return;
    }
    ESP_LOGI(TAG, "add tool: %s", tool->name().c_str());
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description,
                        const PropertyList &properties,
                        std::function<ReturnValue(const PropertyList &)> callback)
{
    AddTool(new McpTool(name, description, properties, std::move(callback)));
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    ESP_LOGI(TAG, "reply id=%d len=%u", id, (unsigned)result.size());
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"result\":" + result + "}";
    if (sender_) sender_(payload);
}

void McpServer::ReplyError(int id, const std::string &message)
{
    ESP_LOGW(TAG, "reply error id=%d: %s", id, message.c_str());
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                          ",\"error\":{\"code\":-32601,\"message\":\"" + message + "\"}}";
    if (sender_) sender_(payload);
}

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message");
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseMessage(const cJSON *json)
{
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version");
        return;
    }

    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    std::string method_str(method->valuestring);
    ESP_LOGI(TAG, "ParseMessage method=%s", method_str.c_str());
    if (method_str.rfind("notifications", 0) == 0) return;

    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    int id_value = id->valueint;

    if (method_str == "initialize") {
        const esp_app_desc_t *app = esp_app_get_description();
        std::string result = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"p4_template\",\"version\":\"";
        result += app != nullptr ? app->version : "1.1.0";
        result += "\"}}";
        ReplyResult(id_value, result);
    } else if (method_str == "tools/list") {
        GetToolsList(id_value);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ReplyError(id_value, "Missing params");
            return;
        }
        auto name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(name)) {
            ReplyError(id_value, "Missing name");
            return;
        }
        auto arguments = cJSON_GetObjectItem(params, "arguments");
        if (arguments != nullptr && !cJSON_IsObject(arguments)) {
            ReplyError(id_value, "Invalid arguments");
            return;
        }
        DoToolCall(id_value, std::string(name->valuestring), arguments);
    } else {
        ReplyError(id_value, "Method not implemented: " + method_str);
    }
}

void McpServer::GetToolsList(int id)
{
    std::string json = "{\"tools\":[";
    for (size_t i = 0; i < tools_.size(); i++) {
        if (i > 0) json += ",";
        json += tools_[i]->to_json();
    }
    json += "]}";
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name,
                           const cJSON *tool_arguments)
{
    ESP_LOGI(TAG, "tool call: %s (id=%d)", tool_name.c_str(), id);
    auto it = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const McpTool *tool) {
        return tool->name() == tool_name;
    });
    if (it == tools_.end()) {
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*it)->properties();
    try {
        for (auto &argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }
            if (!argument.has_default_value() && !found) {
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception &e) {
        ReplyError(id, e.what());
        return;
    }

    try {
        std::string result = (*it)->Call(arguments);
        ESP_LOGI(TAG, "tool result: %s -> %.160s", tool_name.c_str(), result.c_str());
        ReplyResult(id, result);
    } catch (const std::exception &e) {
        ReplyError(id, e.what());
    }
}
