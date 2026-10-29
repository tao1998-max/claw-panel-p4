/*
 * XiaoZhi MCP tools for ESP-Claw.
 *
 * Every tool returns quickly: run_lua blocks at most a few seconds, and long
 * work is always started as an async job (the id is returned immediately).
 * There are no delayed JSON-RPC replies.
 */
#include "claw_mcp_bridge.h"

#include <cstring>
#include <string>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#include "claw_api.h"
#include "claw_settings.h"
#include "mcp_server.h"

static const char *TAG = "claw.mcp";

#define CLAW_TOOL_OUT_MAX 2048

static const char *LUA_API_HINT =
    "Lua API summary (only `claw` is available):\n"
    "  claw.log(...), claw.sleep(ms), claw.time(), claw.uptime_ms(),\n"
    "  claw.progress(pct, msg), claw.abort_requested(), claw.get_args(),\n"
    "  claw.file.read/write/list/exists/remove(path)  (relative paths under the workspace),\n"
    "  claw.file.write only below data/, max 16 KB per call; read max 64 KB,\n"
    "  claw.json.encode(value), claw.json.decode(text),\n"
    "  claw.panel.set_volume(pct), claw.panel.set_brightness(pct),\n"
    "  claw.panel.status(), claw.panel.show_message(text), claw.panel.demo(on).\n"
    "A JSON object is available as both `args` and claw.get_args().\n"
    "No io/os/debug/package/require. Memory is capped, execution is time limited.\n";

static bool lua_ai_enabled_or_reply(std::string &reply)
{
    if (!claw_settings_lua_ai_enabled()) {
        reply = "Error: AI Lua 已关闭。请先在面板 Claw 页面 → 设置 中打开"
                "“允许小智运行 Lua 脚本”，然后再试。";
        return false;
    }
    return true;
}

static std::string gated_result(esp_err_t err, const char *text)
{
    if (text == nullptr) {
        text = "";
    }
    if (err == ESP_OK) {
        return std::string(text);
    }
    if (strncmp(text, "Error", 5) == 0) {
        return std::string(text);
    }
    return "Error: " + std::string(text) + " (" + esp_err_to_name(err) + ")";
}

void claw_mcp_bridge_register(void)
{
    auto &server = McpServer::GetInstance();

    server.AddTool(
        "self.claw.status",
        "Get ESP-Claw status: whether AI-generated Lua is enabled, async job "
        "count, workspace free space, wall-clock validity, and whether the "
        "optional local agent is enabled/ready/busy. Trigger words: "
        "小爪/Claw/脚本/自动化.",
        PropertyList(),
        [](const PropertyList &) -> ReturnValue {
            char out[512] = {0};
            esp_err_t err = claw_api_status_json(out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.run_lua",
        std::string("Run a short Lua script synchronously (a few seconds max) "
                    "and return its print output. Use start_task for anything that "
                    "may run longer. Trigger words: 小爪/Claw/脚本/运行一下.\n") +
            LUA_API_HINT,
        PropertyList({
            Property("code", kPropertyTypeString),
            Property("args", kPropertyTypeString, std::string("")),
            Property("timeout_ms", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string reply;
            if (!lua_ai_enabled_or_reply(reply)) {
                return reply;
            }
            std::string code = properties["code"].value<std::string>();
            std::string args = properties["args"].value<std::string>();
            int timeout_ms = properties["timeout_ms"].value<int>();

            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_run_lua(code.c_str(), args.c_str(),
                                             (uint32_t)timeout_ms, out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.start_task",
        std::string("Start a background Lua task and return its job id "
                    "immediately. Provide either `code` or `path` (a .lua file "
                    "under the workspace, e.g. scripts/hello.lua). Long tasks do "
                    "not speak; check them with get_task. Trigger words: "
                    "小爪/Claw/后台任务/定时/自动化.\n") +
            LUA_API_HINT,
        PropertyList({
            Property("name", kPropertyTypeString, std::string("")),
            Property("code", kPropertyTypeString, std::string("")),
            Property("path", kPropertyTypeString, std::string("")),
            Property("args", kPropertyTypeString, std::string("")),
            Property("timeout_ms", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string reply;
            if (!lua_ai_enabled_or_reply(reply)) {
                return reply;
            }
            std::string name = properties["name"].value<std::string>();
            std::string code = properties["code"].value<std::string>();
            std::string path = properties["path"].value<std::string>();
            std::string args = properties["args"].value<std::string>();
            int timeout_ms = properties["timeout_ms"].value<int>();

            if (code.empty() && path.empty()) {
                return std::string("Error: provide code or path");
            }
            if (timeout_ms < 0) {
                timeout_ms = 0;
            }

            char out[512] = {0};
            esp_err_t err = claw_api_start_task_ex(name.c_str(), code.c_str(),
                                                   path.c_str(), args.c_str(),
                                                   (uint32_t)timeout_ms,
                                                   CLAW_TASK_ORIGIN_AI,
                                                   out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.list_tasks",
        "List ESP-Claw background tasks (Lua jobs). Optional state filter: "
        "running, done, failed, timeout, stopped. Trigger words: 小爪/任务列表.",
        PropertyList({
            Property("state", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string state = properties["state"].value<std::string>();
            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_list_tasks(state.c_str(), out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.get_task",
        "Get one ESP-Claw task by job id or name: status, output tail, progress.",
        PropertyList({
            Property("id", kPropertyTypeString),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string id = properties["id"].value<std::string>();
            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_get_task(id.c_str(), out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.stop_task",
        "Stop a running ESP-Claw task cooperatively.",
        PropertyList({
            Property("id", kPropertyTypeString),
            Property("wait_ms", kPropertyTypeInteger, 2000),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string id = properties["id"].value<std::string>();
            int wait_ms = properties["wait_ms"].value<int>();
            if (wait_ms < 0) wait_ms = 2000;
            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_stop_task(id.c_str(), (uint32_t)wait_ms,
                                               out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.add_schedule",
        "Schedule a Lua script. kind=once uses `when` as epoch seconds (or "
        "seconds from now), kind=interval uses seconds, kind=cron uses a cron "
        "expression. `script` is a workspace .lua path. Trigger words: "
        "小爪/定时/每天/每隔.",
        PropertyList({
            Property("id", kPropertyTypeString),
            Property("kind", kPropertyTypeString),
            Property("when", kPropertyTypeString),
            Property("script", kPropertyTypeString),
            Property("args", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string reply;
            if (!lua_ai_enabled_or_reply(reply)) {
                return reply;
            }
            std::string id = properties["id"].value<std::string>();
            std::string kind = properties["kind"].value<std::string>();
            std::string when = properties["when"].value<std::string>();
            std::string script = properties["script"].value<std::string>();
            std::string args = properties["args"].value<std::string>();

            char out[512] = {0};
            esp_err_t err = claw_api_add_schedule(id.c_str(), kind.c_str(),
                                                  when.c_str(), script.c_str(),
                                                  args.c_str(), out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.list_schedules",
        "List scheduled ESP-Claw automation with next/last fire times.",
        PropertyList(),
        [](const PropertyList &) -> ReturnValue {
            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_list_schedules(out, sizeof(out));
            return gated_result(err, out);
        });

#if CONFIG_TEMPLATE_CLAW_AGENT
    server.AddTool(
        "self.claw.ask",
        "Ask the LOCAL agent (小爪) a question or give it a short task. It has "
        "its own LLM and workspace tools; the final answer is returned as text "
        "for you to speak. Blocks at most 8 seconds. Use agent_task for work "
        "that may take longer. When the local agent is disabled, tell the user "
        "to enable it on the Claw page. Trigger words: 小爪/本机 Agent/自己想想.",
        PropertyList({
            Property("prompt", kPropertyTypeString),
            Property("timeout_ms", kPropertyTypeInteger, 8000),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string prompt = properties["prompt"].value<std::string>();
            int timeout_ms = properties["timeout_ms"].value<int>();
            if (timeout_ms < 500) timeout_ms = 500;
            if (timeout_ms > 8000) timeout_ms = 8000;

            char out[CLAW_TOOL_OUT_MAX] = {0};
            esp_err_t err = claw_api_agent_ask(prompt.c_str(), "xiaozhi",
                                               (uint32_t)timeout_ms,
                                               out, sizeof(out));
            return gated_result(err, out);
        });

    server.AddTool(
        "self.claw.agent_task",
        "Start a background LOCAL agent task and return its task_id immediately. "
        "Use get_task with that id to read the result later; long tasks do not "
        "speak. Trigger words: 小爪/后台/慢慢想/本机 Agent.",
        PropertyList({
            Property("name", kPropertyTypeString, std::string("")),
            Property("prompt", kPropertyTypeString),
            Property("timeout_ms", kPropertyTypeInteger, 120000),
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            std::string prompt = properties["prompt"].value<std::string>();
            int timeout_ms = properties["timeout_ms"].value<int>();
            if (timeout_ms < 1000) timeout_ms = 120000;

            char out[512] = {0};
            esp_err_t err = claw_api_agent_start_task(name.c_str(), prompt.c_str(),
                                                      (uint32_t)timeout_ms,
                                                      out, sizeof(out));
            return gated_result(err, out);
        });
#endif /* CONFIG_TEMPLATE_CLAW_AGENT */

    ESP_LOGI(TAG, "self.claw.* tools registered");
}
