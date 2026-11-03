/*
 * ESP-Claw port layer.
 *
 * Owns startup order, the FAT workspace (see claw_storage.c) and the safety
 * policy. Vendored ESP-Claw components provide the capabilities; XiaoZhi keeps
 * owning audio, display and networking.
 */
#include "claw_port.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "claw_hw_registry.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CLAW_PORT_EXTRA_LUA_MODULES_MAX 4
#define CLAW_PORT_EXTRA_HW_RESOURCES_MAX 8

typedef struct {
    char key[CLAW_HW_KEY_SIZE];
    bool shared;
} claw_port_extra_hw_resource_t;

static claw_port_extra_hw_resource_t s_extra_hw_resources[CLAW_PORT_EXTRA_HW_RESOURCES_MAX];
static size_t s_extra_hw_resource_count;

esp_err_t claw_port_add_hw_resource(const char *key, bool shared)
{
    if (key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_extra_hw_resource_count >= CLAW_PORT_EXTRA_HW_RESOURCES_MAX) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_extra_hw_resources[s_extra_hw_resource_count].key,
             sizeof(s_extra_hw_resources[s_extra_hw_resource_count].key),
             "%s", key);
    s_extra_hw_resources[s_extra_hw_resource_count].shared = shared;
    s_extra_hw_resource_count++;
    return ESP_OK;
}

typedef struct {
    const char *name;
    lua_CFunction open_fn;
} claw_port_extra_lua_module_t;

static claw_port_extra_lua_module_t s_extra_lua_modules[CLAW_PORT_EXTRA_LUA_MODULES_MAX];
static size_t s_extra_lua_module_count;

esp_err_t claw_port_add_lua_module(const char *name, lua_CFunction open_fn)
{
    if (name == NULL || name[0] == '\0' || open_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_extra_lua_module_count >= CLAW_PORT_EXTRA_LUA_MODULES_MAX) {
        return ESP_ERR_NO_MEM;
    }
    s_extra_lua_modules[s_extra_lua_module_count].name = name;
    s_extra_lua_modules[s_extra_lua_module_count].open_fn = open_fn;
    s_extra_lua_module_count++;
    return ESP_OK;
}

#if CONFIG_TEMPLATE_ENABLE_CLAW

#include "cap_files.h"
#include "cap_lua.h"
#include "cap_scheduler.h"
#include "cap_router_mgr.h"
#include "cap_agent_mgr.h"
#include "cap_session_mgr.h"
#include "cap_llm_config.h"
#include "cap_llm_inspect.h"
#include "cap_system.h"
#include "cap_http_request.h"
#include "cap_web_search.h"
#include "cap_im_local.h"
#include "cap_app_mgr.h"
#include "cap_cli.h"
#include "cap_im_platform.h"
#include "app_registry.h"
#include "claw_agent.h"
#include "claw_api.h"
#include "claw_tts.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "claw_hw_registry.h"
#include "claw_lua_claw.h"
#include "claw_paths.h"
#include "lua_module_capability.h"
#include "lua_module_json.h"
#include "lua_module_system.h"
#include "lua_module_delay.h"
#include "lua_module_thread.h"
#include "lua_module_storage.h"
#include "lua_module_event_publisher.h"
#include "lua_module_http_server.h"
#include "lua_driver_adc.h"
#include "lua_driver_gpio.h"
#include "lua_driver_i2c.h"
#include "lua_driver_ledc.h"
#include "lua_driver_mcpwm.h"
#include "lua_driver_pcnt.h"
#include "lua_driver_rmt.h"
#include "lua_driver_uart.h"
#include "lua_module_button.h"
#include "lua_module_knob.h"
#include "lua_module_led_strip.h"
#include "lua_module_sci.h"
#include "lua_module_environmental_sensor.h"
#include "lua_image.h"
#include "lua_module_display.h"
#include "lua_module_lvgl.h"
#include "lua_module_net.h"
#include "lua_module_mqtt.h"
#include "lua_module_websocket.h"
#include "claw_settings.h"
#include "claw_storage.h"

static const char *TAG = "claw_port";

/* LVGL apps load fonts/assets relative to the writable DATA root; the official
 * app does the same when it registers lua_module_lvgl. */
static esp_err_t lua_module_lvgl_register_product(void)
{
    return lua_module_lvgl_register_with_data_root(CLAW_STORAGE_DATA_DIR);
}

static claw_hw_registry_handle_t s_hw_registry;
static bool s_ready;

/* One late-boot line so field logs show the internal/PSRAM headroom after the
 * AI pipeline and the UI have settled. */
static void heap_probe_cb(void *arg)
{
    (void)arg;
    multi_heap_info_t internal_info;
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG,
             "heap@30s internal free=%u KB largest=%u KB min=%u KB | psram free=%u KB min=%u KB",
             (unsigned)(internal_info.total_free_bytes / 1024),
             (unsigned)(internal_info.largest_free_block / 1024),
             (unsigned)(internal_info.minimum_free_bytes / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static esp_err_t space_provider(void *ctx, uint64_t *total_bytes, uint64_t *free_bytes)
{
    (void)ctx;
    return claw_storage_get_space(total_bytes, free_bytes);
}

/* Official cap_llm_config needs a product-side provider. Map it to the same
 * NVS settings used by the local Agent, so /llm commands and the panel stay in
 * sync. */
#if CONFIG_TEMPLATE_CLAW_AGENT
static esp_err_t llm_config_get(cap_llm_config_t *out_config, void *user_ctx)
{
    (void)user_ctx;
    if (!out_config) {
        return ESP_ERR_INVALID_ARG;
    }
    claw_agent_settings_t settings = {0};
    esp_err_t err = claw_settings_get_agent(&settings);
    if (err != ESP_OK) {
        return err;
    }
    memset(out_config, 0, sizeof(*out_config));
    strlcpy(out_config->api_key, settings.api_key, sizeof(out_config->api_key));
    strlcpy(out_config->backend_type, settings.backend, sizeof(out_config->backend_type));
    strlcpy(out_config->model, settings.model, sizeof(out_config->model));
    strlcpy(out_config->base_url, settings.base_url, sizeof(out_config->base_url));
    strlcpy(out_config->auth_type,
            settings.api_key[0] == '\0'
                ? "none"
                : (strcmp(settings.backend, "anthropic_compatible") == 0 ? "api-key" : "bearer"),
            sizeof(out_config->auth_type));
    snprintf(out_config->timeout_ms, sizeof(out_config->timeout_ms), "%u", 60000U);
    snprintf(out_config->max_tokens, sizeof(out_config->max_tokens), "%u", 3072U);
    strlcpy(out_config->supports_tools, "true", sizeof(out_config->supports_tools));
    strlcpy(out_config->supports_vision, "false", sizeof(out_config->supports_vision));
    strlcpy(out_config->image_remote_url_only, "true",
            sizeof(out_config->image_remote_url_only));
    return ESP_OK;
}

static esp_err_t llm_config_apply(const cap_llm_config_t *config, void *user_ctx)
{
    (void)user_ctx;
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->backend_type[0]) {
        esp_err_t err = claw_settings_set_agent_backend(config->backend_type);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->base_url[0]) {
        esp_err_t err = claw_settings_set_agent_base_url(config->base_url);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->model[0]) {
        esp_err_t err = claw_settings_set_agent_model(config->model);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->api_key[0]) {
        esp_err_t err = claw_settings_set_agent_api_key(config->api_key);
        if (err != ESP_OK) {
            return err;
        }
    }
    claw_agent_settings_changed();
    return ESP_OK;
}
#endif /* CONFIG_TEMPLATE_CLAW_AGENT */

/* Scheduler events are delivered here instead of through claw_event_router
 * (the router rules engine is intentionally not started).
 *
 * cap_scheduler wraps the stored item payload as:
 *   {schedule_id, planned_time_ms, fire_time_ms, kind, run_count,
 *    user_payload:{path, origin, args}}
 * The script path is therefore inside user_payload; fall back to the event key
 * for older entries that stored the path directly. */
/* Remove the compatibility rule used by earlier builds. Official schedules
 * are backed by their own run_script rules, created by the scheduled_task
 * skill or claw_api_add_schedule. */
static void claw_router_remove_legacy_default_rule(void)
{
    char existing[512];
    if (claw_event_router_get_rule_json("claw_script_default", existing,
                                        sizeof(existing)) != ESP_OK) {
        return;
    }
    esp_err_t err = claw_event_router_delete_rule("claw_script_default");
    ESP_LOGI(TAG, "legacy default rule remove -> %s", esp_err_to_name(err));
}

#if CONFIG_TEMPLATE_CLAW_SELFTEST
static const char *SELFTEST_SCRIPT =
    "print(\"selftest start\")\n"
    "print(\"env io=\" .. tostring(io) .. \" os=\" .. tostring(os) .. \" debug=\" .. tostring(debug)"
    " .. \" load=\" .. tostring(load) .. \" require=\" .. tostring(require))\n"
    "print(\"escape=\" .. tostring(claw.file.read(\"../../etc/passwd\")))\n"
    "print(\"escape2=\" .. tostring(claw.file.write(\"/tmp/x\", \"x\")))\n"
    "print(\"json=\" .. claw.json.encode({a=1, b={\"x\", \"y\"}, c=true, name=claw.get_args().name}))\n"
    "print(\"write=\" .. tostring(claw.file.write(\"scripts/_w.txt\", \"hello\")))\n"
    "print(\"read=\" .. tostring(claw.file.read(\"scripts/_w.txt\")))\n"
    "print(\"list=\" .. tostring(#claw.file.list(\"scripts\")))\n"
    "print(\"status=\" .. tostring(claw.panel.status()))\n"
    "local as_ok, as = pcall(require, \"arg_schema\")\n"
    "print(\"require arg_schema=\" .. tostring(as_ok) .. \" type=\" .. type(as))\n"
    "local cap_ok, cap = pcall(require, \"capability\")\n"
    "print(\"require capability=\" .. tostring(cap_ok) .. \" type=\" .. type(cap))\n"
    "local json_ok, json_mod = pcall(require, \"json\")\n"
    "print(\"require json=\" .. tostring(json_ok) .. \" type=\" .. type(json_mod))\n"
    "if as_ok and as.parse then\n"
    "  local parsed = as.parse({x=5}, {x=as.int({default=1, min=0})})\n"
    "  print(\"arg_schema parse x=\" .. tostring(parsed.x))\n"
    "end\n"
    "local okt, errt = pcall(function() while true do end end)\n"
    "print(\"timeout ok=\" .. tostring(okt) .. \" err=\" .. tostring(errt))\n"
    "print(\"selftest end\")\n";

static void claw_selftest_run(void)
{
    const char *path = CLAW_STORAGE_DATA_DIR "/scripts/_selftest.lua";
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "selftest: cannot write %s", path);
        return;
    }
    fputs(SELFTEST_SCRIPT, f);
    fclose(f);

    static char output[4096];
    esp_err_t err = cap_lua_run_script(path, "{\"name\":\"self\"}", 3000,
                                       output, sizeof(output));
    ESP_LOGI(TAG, "selftest err=%s\n%s", esp_err_to_name(err), output);

    /* Exercise the MCP/UI-facing API path (temp file + job submit + progress). */
    esp_err_t api_err = claw_api_run_lua(
        "print('api ok ' .. tostring(claw.file.exists('scripts/hello.lua')))",
        "{\"x\":1}", 2000, output, sizeof(output));
    ESP_LOGI(TAG, "api run_lua err=%s out=%s", esp_err_to_name(api_err), output);

    char task_out[512] = {0};
    api_err = claw_api_start_task(
        "selftest_task",
        "for i = 1, 3 do claw.progress(i * 30, 'step') claw.sleep(100) end print('async done')",
        NULL, NULL, 0, task_out, sizeof(task_out));
    ESP_LOGI(TAG, "api start_task err=%s out=%s", esp_err_to_name(api_err), task_out);
}

static void claw_selftest_enable_lua_ai(void)
{
    if (!claw_settings_lua_ai_enabled()) {
        ESP_LOGW(TAG, "selftest enabling AI Lua persistently (factory reset restores OFF)");
        (void)claw_settings_set_lua_ai(true);
    }
}

#endif /* CONFIG_TEMPLATE_CLAW_SELFTEST */

esp_err_t claw_port_init(const char *sd_mount_point)
{
    esp_err_t err;
    char sd_apps_root[96] = {0};
    bool has_sd = sd_mount_point != NULL && sd_mount_point[0] != '\0';

    if (has_sd) {
        snprintf(sd_apps_root, sizeof(sd_apps_root), "%s/apps", sd_mount_point);
    }

    err = claw_storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "workspace mount failed: %s", esp_err_to_name(err));
        return err;
    }

    err = claw_paths_set(CLAW_PATH_DATA, CLAW_STORAGE_DATA_DIR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_paths_set(DATA) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = claw_paths_set(CLAW_PATH_SYSTEM, CLAW_STORAGE_SYSTEM_DIR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_paths_set(SYSTEM) failed: %s", esp_err_to_name(err));
        return err;
    }
    claw_tts_init();
    (void)claw_paths_set_space_provider(CLAW_PATH_DATA, space_provider, NULL);

    err = claw_hw_registry_create(&s_hw_registry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hw registry create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Board resources queued by main/hal before claw_port_init(). Official Lua
     * drivers can only claim registered resources. */
    for (size_t i = 0; i < s_extra_hw_resource_count; i++) {
        const claw_hw_resource_config_t resource = {
            .resource = s_extra_hw_resources[i].key,
            .policy = s_extra_hw_resources[i].shared ? CLAW_HW_SHARED : CLAW_HW_EXCLUSIVE,
        };
        esp_err_t res_err = claw_hw_registry_register(s_hw_registry, &resource, 1);

        if (res_err != ESP_OK) {
            ESP_LOGW(TAG, "hw resource '%s' register failed: %s",
                     s_extra_hw_resources[i].key, esp_err_to_name(res_err));
        }
    }

    err = claw_cap_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_cap_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = cap_lua_set_hw_registry(s_hw_registry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_lua_set_hw_registry failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Official Lua modules are registered as globals through luaL_requiref.
     * Built-in Lua libraries (arg_schema.lua) are loaded from the read-only
     * system tree, matching the upstream package-path layout. */
    char builtin_dir[160];
    snprintf(builtin_dir, sizeof(builtin_dir),
             CLAW_STORAGE_SYSTEM_DIR "/scripts/builtin");
    err = cap_lua_add_package_path_dir(builtin_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lua package path %s failed: %s", builtin_dir,
                 esp_err_to_name(err));
        return err;
    }
    snprintf(builtin_dir, sizeof(builtin_dir),
             CLAW_STORAGE_SYSTEM_DIR "/scripts/builtin/lib");
    err = cap_lua_add_package_path_dir(builtin_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lua package path %s failed: %s", builtin_dir,
                 esp_err_to_name(err));
        return err;
    }

    err = cap_lua_register_module("claw", luaopen_claw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Official skills use these modules through require(). */
    err = lua_module_capability_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capability module registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = lua_module_json_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "json module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_system_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "system module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_delay_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "delay module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_thread_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "thread module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_storage_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_event_publisher_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event publisher registration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_http_server_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server module registration failed: %s", esp_err_to_name(err));
        return err;
    }
    {
        static const struct {
            const char *name;
            esp_err_t (*reg)(void);
        } extra_modules[] = {
            { "adc", lua_driver_adc_register },
            { "gpio", lua_driver_gpio_register },
            { "i2c", lua_driver_i2c_register },
            { "ledc", lua_driver_ledc_register },
            { "mcpwm", lua_driver_mcpwm_register },
            { "pcnt", lua_driver_pcnt_register },
            { "rmt", lua_driver_rmt_register },
            { "uart", lua_driver_uart_register },
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
            /* This board has none of these devices. Keep them opt-in so the
             * Agent does not advertise hardware that will fail at new(). */
            { "button", lua_module_button_register },
            { "knob", lua_module_knob_register },
            { "led_strip", lua_module_led_strip_register },
            { "sci", lua_module_sci_register },
            { "environmental_sensor", lua_module_environmental_sensor_register },
#endif
            { "net", lua_module_net_register },
            { "mqtt", lua_module_mqtt_register },
            { "websocket", lua_module_websocket_register },
            { "image", lua_module_image_register },
            { "display", lua_module_display_register },
            { "lvgl", lua_module_lvgl_register_product },
        };
        for (size_t i = 0; i < sizeof(extra_modules) / sizeof(extra_modules[0]); i++) {
            err = extra_modules[i].reg();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s module registration failed: %s",
                         extra_modules[i].name, esp_err_to_name(err));
                return err;
            }
        }
    }

    for (size_t i = 0; i < s_extra_lua_module_count; i++) {
        err = cap_lua_register_module(s_extra_lua_modules[i].name,
                                      s_extra_lua_modules[i].open_fn);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s Lua module registration failed: %s",
                     s_extra_lua_modules[i].name, esp_err_to_name(err));
            return err;
        }
    }

    err = cap_lua_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_lua_register_group failed: %s", esp_err_to_name(err));
        return err;
    }

    err = claw_api_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_api_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register the optional agent tools/state. This does not start the agent
     * or create any TLS session; the first request does that lazily. */
    err = claw_agent_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_agent_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = cap_files_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_files_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_scheduler_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_scheduler_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_router_mgr_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_router_mgr_register_group failed: %s",
                 esp_err_to_name(err));
        return err;
    }
#if CONFIG_TEMPLATE_CLAW_AGENT
    /* The local Agent now runs on the official claw_agent_mgr, so these
     * capabilities have a root agent and session manager behind them. */
    err = cap_agent_mgr_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_agent_mgr_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_session_mgr_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_session_mgr_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    const cap_llm_config_provider_t llm_provider = {
        .get_config = llm_config_get,
        .apply_config = llm_config_apply,
        .user_ctx = NULL,
    };
    err = cap_llm_config_set_provider(&llm_provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_llm_config_set_provider failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_llm_config_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_llm_config_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
#endif
    err = cap_llm_inspect_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_llm_inspect_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_system_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_system_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_http_request_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_http_request_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_web_search_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_web_search_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_im_local_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_im_local_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = app_registry_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_registry_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = app_registry_add_directory(CLAW_STORAGE_DATA_DIR "/apps");
    if (err == ESP_OK && has_sd) {
        /* SD apps: read-only root, launched in place from the card. publish()
         * and remove() still target the FAT root, and a missing card just
         * yields an empty scan. */
        err = app_registry_add_directory(sd_apps_root);
        if (err == ESP_OK) {
            err = claw_api_add_script_root(sd_apps_root);
        }
        if (err == ESP_OK) {
            /* Expose the card to LVGL as drive S:, so lvgl.image/animimg/gif
             * src paths can read SD assets. */
            err = lua_module_lvgl_set_sd_root(sd_mount_point);
        }
        if (err == ESP_OK) {
            /* Fonts may live on the card too; lvgl.font_load() reads absolute
             * paths under this root directly. */
            err = lua_lvgl_add_extra_font_root(sd_mount_point);
        }
    }
    if (err == ESP_OK) {
        err = app_registry_reload();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app registry setup failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_app_mgr_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_app_mgr_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_cli_register_group();
    if (err != ESP_OK) {
        /* This product does not start the esp_console REPL; the capability is
         * optional and must not take the whole Claw runtime down. */
        ESP_LOGW(TAG, "cap_cli disabled: %s", esp_err_to_name(err));
    }
    err = cap_im_platform_register_groups();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_im_platform_register_groups failed: %s", esp_err_to_name(err));
        return err;
    }

    static char s_router_rules_path[192];
    static char s_router_recovery_path[192];
    snprintf(s_router_rules_path, sizeof(s_router_rules_path),
             CLAW_STORAGE_DATA_DIR "/router_rules/router_rules.json");
    snprintf(s_router_recovery_path, sizeof(s_router_recovery_path),
             CLAW_STORAGE_SYSTEM_DIR "/router_rules/router_rules.json");
    const claw_event_router_config_t router_cfg = {
        .rules_path = s_router_rules_path,
        .recovery_rules_path = s_router_recovery_path,
        .max_rules = 32,
        .max_actions_per_rule = 8,
        .cap_output_size = 8192,
        .event_queue_len = 6,
        .task_stack_size = 8192,
        .task_priority = 4,
        .task_core = tskNO_AFFINITY,
        .agent_submit_timeout_ms = 1000,
        /* QQ/Feishu/Telegram/WeChat inbound messages are chat messages for
         * the Agent; without this fallback they are logged and dropped when
         * no router rule matches. */
        .default_route_messages_to_agent = true,
    };
    err = claw_event_router_init(&router_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_event_router_init failed: %s", esp_err_to_name(err));
        return err;
    }
#if CONFIG_APP_CLAW_CAP_IM_QQ
    (void)claw_event_router_register_outbound_binding("qq", "qq_send_message");
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    (void)claw_event_router_register_outbound_binding("feishu", "feishu_send_message");
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    (void)claw_event_router_register_outbound_binding("telegram", "tg_send_message");
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    (void)claw_event_router_register_outbound_binding("wechat", "wechat_send_message");
#endif
    (void)claw_event_router_register_outbound_binding("web", "local_send_message");
    claw_router_remove_legacy_default_rule();

    const cap_scheduler_config_t sched_cfg = {
        .schedules_path = CLAW_STORAGE_DATA_DIR "/schedules/schedules.json",
        .recovery_schedules_path = CLAW_STORAGE_SYSTEM_DIR "/skills/schedules.json",
        .tick_ms = 1000,
        .max_items = 16,
        .task_stack_size = 8192,
        .task_priority = 3,
        .task_core = tskNO_AFFINITY,
        .publish_event = claw_event_router_publish,
        .persist_after_fire = true,
    };
    err = cap_scheduler_init(&sched_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_scheduler_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = claw_cap_start_all();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_cap_start_all failed: %s", esp_err_to_name(err));
        return err;
    }

    err = claw_event_router_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claw_event_router_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = cap_scheduler_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cap_scheduler_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;

#if CONFIG_TEMPLATE_CLAW_AGENT
    /* Upstream app_claw_start() brings claw_agent_mgr up at boot. Do the same
     * when the product Agent switch is on and the endpoint is configured, so
     * the session catalog/history and the Web UI work before the first
     * request. A failure is non-fatal. */
    esp_err_t agent_err = claw_agent_prepare();
    if (agent_err != ESP_OK) {
        ESP_LOGI(TAG, "local agent not started at boot: %s",
                 esp_err_to_name(agent_err));
    }
#endif

    {
        const esp_timer_create_args_t probe_args = {
            .callback = heap_probe_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "claw_heap_probe",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t probe = NULL;
        if (esp_timer_create(&probe_args, &probe) == ESP_OK) {
            esp_timer_start_once(probe, 30ULL * 1000 * 1000);
        }
    }

#if CONFIG_TEMPLATE_CLAW_SELFTEST
    claw_selftest_enable_lua_ai();
    claw_selftest_run();
#endif

    ESP_LOGI(TAG, "ESP-Claw port ready");
    return ESP_OK;
}

esp_err_t claw_port_factory_reset(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    char out[128] = {0};
    (void)claw_agent_shutdown(3000);
    (void)cap_lua_stop_all_jobs(NULL, 2000, out, sizeof(out));
    (void)cap_scheduler_stop();
    return claw_storage_reset_user_data();
}

bool claw_port_is_ready(void)
{
    return s_ready;
}

#else /* CONFIG_TEMPLATE_ENABLE_CLAW */

esp_err_t claw_port_init(const char *sd_mount_point)
{
    (void)sd_mount_point;
    return ESP_OK;
}

esp_err_t claw_port_factory_reset(void)
{
    return ESP_OK;
}

bool claw_port_is_ready(void)
{
    return false;
}

#endif /* CONFIG_TEMPLATE_ENABLE_CLAW */
