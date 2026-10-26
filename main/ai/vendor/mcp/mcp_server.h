#ifndef MCP_SERVER_H
#define MCP_SERVER_H

/* Trimmed port of XiaoZhi v2.2.6 mcp_server.{h,cc}.
 * Keeps the official Property / PropertyList / McpTool / McpServer structure
 * and JSON-RPC message shapes; only the board-specific tools are replaced. */

#include <cJSON.h>

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using ReturnValue = std::variant<bool, int, std::string, cJSON *>;

enum PropertyType {
    kPropertyTypeBoolean,
    kPropertyTypeInteger,
    kPropertyTypeString
};

class Property {
private:
    std::string name_;
    PropertyType type_;
    std::variant<bool, int, std::string> value_;
    bool has_default_value_;
    std::optional<int> min_value_;
    std::optional<int> max_value_;

public:
    Property(const std::string &name, PropertyType type)
        : name_(name), type_(type), has_default_value_(false) {}

    template <typename T>
    Property(const std::string &name, PropertyType type, const T &default_value)
        : name_(name), type_(type), has_default_value_(true) {
        value_ = default_value;
    }

    Property(const std::string &name, PropertyType type, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(false),
          min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
    }

    inline const std::string &name() const { return name_; }
    inline PropertyType type() const { return type_; }
    inline bool has_default_value() const { return has_default_value_; }

    template <typename T>
    inline T value() const { return std::get<T>(value_); }

    template <typename T>
    inline void set_value(const T &value) {
        if constexpr (std::is_same_v<T, int>) {
            if (min_value_.has_value() && value < min_value_.value()) {
                throw std::invalid_argument("Value is below minimum allowed");
            }
            if (max_value_.has_value() && value > max_value_.value()) {
                throw std::invalid_argument("Value exceeds maximum allowed");
            }
        }
        value_ = value;
    }

    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();
        if (type_ == kPropertyTypeBoolean) {
            cJSON_AddStringToObject(json, "type", "boolean");
            if (has_default_value_) cJSON_AddBoolToObject(json, "default", value<bool>());
        } else if (type_ == kPropertyTypeInteger) {
            cJSON_AddStringToObject(json, "type", "integer");
            if (has_default_value_) cJSON_AddNumberToObject(json, "default", value<int>());
            if (min_value_.has_value()) cJSON_AddNumberToObject(json, "minimum", min_value_.value());
            if (max_value_.has_value()) cJSON_AddNumberToObject(json, "maximum", max_value_.value());
        } else {
            cJSON_AddStringToObject(json, "type", "string");
            if (has_default_value_) {
                cJSON_AddStringToObject(json, "default", value<std::string>().c_str());
            }
        }
        char *str = cJSON_PrintUnformatted(json);
        std::string result(str != nullptr ? str : "{}");
        cJSON_free(str);
        cJSON_Delete(json);
        return result;
    }
};

class PropertyList {
private:
    std::vector<Property> properties_;

public:
    PropertyList() = default;
    explicit PropertyList(const std::vector<Property> &properties) : properties_(properties) {}

    const Property &operator[](const std::string &name) const {
        for (const auto &property : properties_) {
            if (property.name() == name) return property;
        }
        throw std::runtime_error("Property not found: " + name);
    }

    std::vector<std::string> GetRequired() const {
        std::vector<std::string> required;
        for (const auto &property : properties_) {
            if (!property.has_default_value()) required.push_back(property.name());
        }
        return required;
    }

    auto begin() { return properties_.begin(); }
    auto end() { return properties_.end(); }
    auto begin() const { return properties_.begin(); }
    auto end() const { return properties_.end(); }

    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();
        for (const auto &property : properties_) {
            cJSON *prop = cJSON_Parse(property.to_json().c_str());
            if (prop != nullptr) cJSON_AddItemToObject(json, property.name().c_str(), prop);
        }
        char *str = cJSON_PrintUnformatted(json);
        std::string result(str != nullptr ? str : "{}");
        cJSON_free(str);
        cJSON_Delete(json);
        return result;
    }
};

class McpTool {
private:
    std::string name_;
    std::string description_;
    PropertyList properties_;
    std::function<ReturnValue(const PropertyList &)> callback_;

public:
    McpTool(const std::string &name, const std::string &description,
            const PropertyList &properties,
            std::function<ReturnValue(const PropertyList &)> callback)
        : name_(name), description_(description), properties_(properties),
          callback_(std::move(callback)) {}

    inline const std::string &name() const { return name_; }
    inline const std::string &description() const { return description_; }
    inline const PropertyList &properties() const { return properties_; }

    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "name", name_.c_str());
        cJSON_AddStringToObject(json, "description", description_.c_str());
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_Parse(properties_.to_json().c_str());
        cJSON_AddItemToObject(schema, "properties", props != nullptr ? props : cJSON_CreateObject());
        auto required = properties_.GetRequired();
        if (!required.empty()) {
            cJSON *arr = cJSON_CreateArray();
            for (const auto &name : required) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
            }
            cJSON_AddItemToObject(schema, "required", arr);
        }
        cJSON_AddItemToObject(json, "inputSchema", schema);
        char *str = cJSON_PrintUnformatted(json);
        std::string result(str != nullptr ? str : "{}");
        cJSON_free(str);
        cJSON_Delete(json);
        return result;
    }

    std::string Call(const PropertyList &properties) {
        ReturnValue value = callback_(properties);
        cJSON *result = cJSON_CreateObject();
        cJSON *content = cJSON_CreateArray();
        cJSON *text = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");
        if (std::holds_alternative<std::string>(value)) {
            cJSON_AddStringToObject(text, "text", std::get<std::string>(value).c_str());
        } else if (std::holds_alternative<bool>(value)) {
            cJSON_AddStringToObject(text, "text", std::get<bool>(value) ? "true" : "false");
        } else if (std::holds_alternative<int>(value)) {
            cJSON_AddStringToObject(text, "text", std::to_string(std::get<int>(value)).c_str());
        } else if (std::holds_alternative<cJSON *>(value)) {
            cJSON *json = std::get<cJSON *>(value);
            char *str = cJSON_PrintUnformatted(json);
            cJSON_AddStringToObject(text, "text", str != nullptr ? str : "{}");
            cJSON_free(str);
            cJSON_Delete(json);
        }
        cJSON_AddItemToArray(content, text);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddBoolToObject(result, "isError", false);
        char *str = cJSON_PrintUnformatted(result);
        std::string output(str != nullptr ? str : "{}");
        cJSON_free(str);
        cJSON_Delete(result);
        return output;
    }
};

class McpServer {
public:
    static McpServer &GetInstance() {
        static McpServer instance;
        return instance;
    }

    void SetSender(std::function<void(const std::string &)> sender) {
        sender_ = std::move(sender);
    }

    void AddCommonTools();
    void AddTool(McpTool *tool);
    void AddTool(const std::string &name, const std::string &description,
                 const PropertyList &properties,
                 std::function<ReturnValue(const PropertyList &)> callback);

    void ParseMessage(const cJSON *json);
    void ParseMessage(const std::string &message);

private:
    McpServer() = default;
    ~McpServer();

    void ReplyResult(int id, const std::string &result);
    void ReplyError(int id, const std::string &message);
    void GetToolsList(int id);
    void DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments);

    std::vector<McpTool *> tools_;
    std::function<void(const std::string &)> sender_;
    bool common_tools_added_ = false;
};

#endif  // MCP_SERVER_H
