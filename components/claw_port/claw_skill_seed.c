/* Generated from the official ESP-Claw skill files. */
#include "claw_storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "claw_skills";

typedef struct {
    const char *path;
    const uint8_t *data;
    uint32_t len;
} claw_seed_file_t;

extern const uint8_t _binary_plan_mode_SKILL_md_start[] asm("_binary_plan_mode_SKILL_md_start");
extern const uint8_t _binary_plan_mode_SKILL_md_end[] asm("_binary_plan_mode_SKILL_md_end");
extern const uint8_t _binary_app_creator_SKILL_md_start[] asm("_binary_app_creator_SKILL_md_start");
extern const uint8_t _binary_app_creator_SKILL_md_end[] asm("_binary_app_creator_SKILL_md_end");
extern const uint8_t _binary_app_creator_app_package_spec_md_start[] asm("_binary_app_creator_app_package_spec_md_start");
extern const uint8_t _binary_app_creator_app_package_spec_md_end[] asm("_binary_app_creator_app_package_spec_md_end");
extern const uint8_t _binary_app_creator_resolve_apps_root_lua_start[] asm("_binary_app_creator_resolve_apps_root_lua_start");
extern const uint8_t _binary_app_creator_resolve_apps_root_lua_end[] asm("_binary_app_creator_resolve_apps_root_lua_end");
extern const uint8_t _binary_app_creator_scaffold_app_lua_start[] asm("_binary_app_creator_scaffold_app_lua_start");
extern const uint8_t _binary_app_creator_scaffold_app_lua_end[] asm("_binary_app_creator_scaffold_app_lua_end");
extern const uint8_t _binary_skills_lab_search_SKILL_md_start[] asm("_binary_skills_lab_search_SKILL_md_start");
extern const uint8_t _binary_skills_lab_search_SKILL_md_end[] asm("_binary_skills_lab_search_SKILL_md_end");
extern const uint8_t _binary_skills_lab_downloader_SKILL_md_start[] asm("_binary_skills_lab_downloader_SKILL_md_start");
extern const uint8_t _binary_skills_lab_downloader_SKILL_md_end[] asm("_binary_skills_lab_downloader_SKILL_md_end");
extern const uint8_t _binary_skills_lab_downloader_download_package_lua_start[] asm("_binary_skills_lab_downloader_download_package_lua_start");
extern const uint8_t _binary_skills_lab_downloader_download_package_lua_end[] asm("_binary_skills_lab_downloader_download_package_lua_end");
extern const uint8_t _binary_skill_creator_SKILL_md_start[] asm("_binary_skill_creator_SKILL_md_start");
extern const uint8_t _binary_skill_creator_SKILL_md_end[] asm("_binary_skill_creator_SKILL_md_end");
extern const uint8_t _binary_skill_creator_skill_package_spec_md_start[] asm("_binary_skill_creator_skill_package_spec_md_start");
extern const uint8_t _binary_skill_creator_skill_package_spec_md_end[] asm("_binary_skill_creator_skill_package_spec_md_end");
extern const uint8_t _binary_skill_creator_resolve_skills_root_lua_start[] asm("_binary_skill_creator_resolve_skills_root_lua_start");
extern const uint8_t _binary_skill_creator_resolve_skills_root_lua_end[] asm("_binary_skill_creator_resolve_skills_root_lua_end");
extern const uint8_t _binary_lua_package_authoring_SKILL_md_start[] asm("_binary_lua_package_authoring_SKILL_md_start");
extern const uint8_t _binary_lua_package_authoring_SKILL_md_end[] asm("_binary_lua_package_authoring_SKILL_md_end");
extern const uint8_t _binary_board_hardware_info_SKILL_md_start[] asm("_binary_board_hardware_info_SKILL_md_start");
extern const uint8_t _binary_board_hardware_info_SKILL_md_end[] asm("_binary_board_hardware_info_SKILL_md_end");
extern const uint8_t _binary_builtin_lua_modules_SKILL_md_start[] asm("_binary_builtin_lua_modules_SKILL_md_start");
extern const uint8_t _binary_builtin_lua_modules_SKILL_md_end[] asm("_binary_builtin_lua_modules_SKILL_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_adc_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_adc_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_adc_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_adc_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_gpio_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_gpio_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_gpio_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_gpio_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_i2c_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_i2c_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_i2c_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_i2c_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_ledc_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_ledc_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_ledc_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_ledc_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_pcnt_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_pcnt_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_pcnt_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_pcnt_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_rmt_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_rmt_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_rmt_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_rmt_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_uart_md_start[] asm("_binary_builtin_lua_modules_docs_lua_driver_uart_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_driver_uart_md_end[] asm("_binary_builtin_lua_modules_docs_lua_driver_uart_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_audio_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_audio_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_audio_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_audio_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_button_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_button_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_button_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_button_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_call_capability_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_call_capability_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_call_capability_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_call_capability_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_claw_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_claw_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_claw_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_claw_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_delay_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_delay_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_delay_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_delay_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_display_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_display_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_display_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_display_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_event_publisher_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_event_publisher_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_event_publisher_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_event_publisher_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_http_server_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_http_server_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_http_server_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_http_server_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_image_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_image_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_image_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_image_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_net_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_net_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_net_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_net_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_mqtt_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_mqtt_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_mqtt_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_mqtt_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_websocket_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_websocket_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_websocket_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_websocket_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_json_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_json_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_json_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_json_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_knob_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_knob_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_knob_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_knob_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_led_strip_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_led_strip_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_led_strip_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_led_strip_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_lvgl_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_lvgl_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_lvgl_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_lvgl_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_sci_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_sci_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_sci_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_sci_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_storage_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_storage_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_storage_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_storage_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_system_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_system_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_system_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_system_md_end");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_thread_md_start[] asm("_binary_builtin_lua_modules_docs_lua_module_thread_md_start");
extern const uint8_t _binary_builtin_lua_modules_docs_lua_module_thread_md_end[] asm("_binary_builtin_lua_modules_docs_lua_module_thread_md_end");

static void seed_dir(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno %d", path, errno);
    }
}

void claw_skill_seed_write(void)
{
    const claw_seed_file_t files[] = {
        { "skills/board_hardware_info/SKILL.md", _binary_board_hardware_info_SKILL_md_start, (uint32_t)(_binary_board_hardware_info_SKILL_md_end - _binary_board_hardware_info_SKILL_md_start) },
        { "skills/plan_mode/SKILL.md", _binary_plan_mode_SKILL_md_start, (uint32_t)(_binary_plan_mode_SKILL_md_end - _binary_plan_mode_SKILL_md_start) },
        { "skills/app_creator/SKILL.md", _binary_app_creator_SKILL_md_start, (uint32_t)(_binary_app_creator_SKILL_md_end - _binary_app_creator_SKILL_md_start) },
        { "skills/app_creator/references/app-package-spec.md", _binary_app_creator_app_package_spec_md_start, (uint32_t)(_binary_app_creator_app_package_spec_md_end - _binary_app_creator_app_package_spec_md_start) },
        { "skills/app_creator/scripts/resolve_apps_root.lua", _binary_app_creator_resolve_apps_root_lua_start, (uint32_t)(_binary_app_creator_resolve_apps_root_lua_end - _binary_app_creator_resolve_apps_root_lua_start) },
        { "skills/app_creator/scripts/scaffold_app.lua", _binary_app_creator_scaffold_app_lua_start, (uint32_t)(_binary_app_creator_scaffold_app_lua_end - _binary_app_creator_scaffold_app_lua_start) },
        { "skills/skills_lab_search/SKILL.md", _binary_skills_lab_search_SKILL_md_start, (uint32_t)(_binary_skills_lab_search_SKILL_md_end - _binary_skills_lab_search_SKILL_md_start) },
        { "skills/skills_lab_downloader/SKILL.md", _binary_skills_lab_downloader_SKILL_md_start, (uint32_t)(_binary_skills_lab_downloader_SKILL_md_end - _binary_skills_lab_downloader_SKILL_md_start) },
        { "skills/skills_lab_downloader/scripts/download_package.lua", _binary_skills_lab_downloader_download_package_lua_start, (uint32_t)(_binary_skills_lab_downloader_download_package_lua_end - _binary_skills_lab_downloader_download_package_lua_start) },
        { "skills/skill_creator/SKILL.md", _binary_skill_creator_SKILL_md_start, (uint32_t)(_binary_skill_creator_SKILL_md_end - _binary_skill_creator_SKILL_md_start) },
        { "skills/skill_creator/references/skill-package-spec.md", _binary_skill_creator_skill_package_spec_md_start, (uint32_t)(_binary_skill_creator_skill_package_spec_md_end - _binary_skill_creator_skill_package_spec_md_start) },
        { "skills/skill_creator/scripts/resolve_skills_root.lua", _binary_skill_creator_resolve_skills_root_lua_start, (uint32_t)(_binary_skill_creator_resolve_skills_root_lua_end - _binary_skill_creator_resolve_skills_root_lua_start) },
        { "skills/lua_package_authoring/SKILL.md", _binary_lua_package_authoring_SKILL_md_start, (uint32_t)(_binary_lua_package_authoring_SKILL_md_end - _binary_lua_package_authoring_SKILL_md_start) },
        { "skills/builtin_lua_modules/SKILL.md", _binary_builtin_lua_modules_SKILL_md_start, (uint32_t)(_binary_builtin_lua_modules_SKILL_md_end - _binary_builtin_lua_modules_SKILL_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_adc.md", _binary_builtin_lua_modules_docs_lua_driver_adc_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_adc_md_end - _binary_builtin_lua_modules_docs_lua_driver_adc_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_gpio.md", _binary_builtin_lua_modules_docs_lua_driver_gpio_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_gpio_md_end - _binary_builtin_lua_modules_docs_lua_driver_gpio_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_i2c.md", _binary_builtin_lua_modules_docs_lua_driver_i2c_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_i2c_md_end - _binary_builtin_lua_modules_docs_lua_driver_i2c_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_ledc.md", _binary_builtin_lua_modules_docs_lua_driver_ledc_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_ledc_md_end - _binary_builtin_lua_modules_docs_lua_driver_ledc_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_mcpwm.md", _binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_end - _binary_builtin_lua_modules_docs_lua_driver_mcpwm_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_pcnt.md", _binary_builtin_lua_modules_docs_lua_driver_pcnt_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_pcnt_md_end - _binary_builtin_lua_modules_docs_lua_driver_pcnt_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_rmt.md", _binary_builtin_lua_modules_docs_lua_driver_rmt_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_rmt_md_end - _binary_builtin_lua_modules_docs_lua_driver_rmt_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_driver_uart.md", _binary_builtin_lua_modules_docs_lua_driver_uart_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_driver_uart_md_end - _binary_builtin_lua_modules_docs_lua_driver_uart_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_audio.md", _binary_builtin_lua_modules_docs_lua_module_audio_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_audio_md_end - _binary_builtin_lua_modules_docs_lua_module_audio_md_start) },
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
        { "skills/builtin_lua_modules/scripts/docs/lua_module_button.md", _binary_builtin_lua_modules_docs_lua_module_button_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_button_md_end - _binary_builtin_lua_modules_docs_lua_module_button_md_start) },
#endif
        { "skills/builtin_lua_modules/scripts/docs/lua_module_call_capability.md", _binary_builtin_lua_modules_docs_lua_module_call_capability_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_call_capability_md_end - _binary_builtin_lua_modules_docs_lua_module_call_capability_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_claw.md", _binary_builtin_lua_modules_docs_lua_module_claw_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_claw_md_end - _binary_builtin_lua_modules_docs_lua_module_claw_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_delay.md", _binary_builtin_lua_modules_docs_lua_module_delay_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_delay_md_end - _binary_builtin_lua_modules_docs_lua_module_delay_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_display.md", _binary_builtin_lua_modules_docs_lua_module_display_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_display_md_end - _binary_builtin_lua_modules_docs_lua_module_display_md_start) },
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
        { "skills/builtin_lua_modules/scripts/docs/lua_module_environmental_sensor.md", _binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_end - _binary_builtin_lua_modules_docs_lua_module_environmental_sensor_md_start) },
#endif
        { "skills/builtin_lua_modules/scripts/docs/lua_module_event_publisher.md", _binary_builtin_lua_modules_docs_lua_module_event_publisher_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_event_publisher_md_end - _binary_builtin_lua_modules_docs_lua_module_event_publisher_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_http_server.md", _binary_builtin_lua_modules_docs_lua_module_http_server_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_http_server_md_end - _binary_builtin_lua_modules_docs_lua_module_http_server_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_image.md", _binary_builtin_lua_modules_docs_lua_module_image_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_image_md_end - _binary_builtin_lua_modules_docs_lua_module_image_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_net.md", _binary_builtin_lua_modules_docs_lua_module_net_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_net_md_end - _binary_builtin_lua_modules_docs_lua_module_net_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_mqtt.md", _binary_builtin_lua_modules_docs_lua_module_mqtt_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_mqtt_md_end - _binary_builtin_lua_modules_docs_lua_module_mqtt_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_websocket.md", _binary_builtin_lua_modules_docs_lua_module_websocket_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_websocket_md_end - _binary_builtin_lua_modules_docs_lua_module_websocket_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_json.md", _binary_builtin_lua_modules_docs_lua_module_json_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_json_md_end - _binary_builtin_lua_modules_docs_lua_module_json_md_start) },
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
        { "skills/builtin_lua_modules/scripts/docs/lua_module_knob.md", _binary_builtin_lua_modules_docs_lua_module_knob_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_knob_md_end - _binary_builtin_lua_modules_docs_lua_module_knob_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_led_strip.md", _binary_builtin_lua_modules_docs_lua_module_led_strip_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_led_strip_md_end - _binary_builtin_lua_modules_docs_lua_module_led_strip_md_start) },
#endif
        { "skills/builtin_lua_modules/scripts/docs/lua_module_lvgl.md", _binary_builtin_lua_modules_docs_lua_module_lvgl_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_lvgl_md_end - _binary_builtin_lua_modules_docs_lua_module_lvgl_md_start) },
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
        { "skills/builtin_lua_modules/scripts/docs/lua_module_sci.md", _binary_builtin_lua_modules_docs_lua_module_sci_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_sci_md_end - _binary_builtin_lua_modules_docs_lua_module_sci_md_start) },
#endif
        { "skills/builtin_lua_modules/scripts/docs/lua_module_storage.md", _binary_builtin_lua_modules_docs_lua_module_storage_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_storage_md_end - _binary_builtin_lua_modules_docs_lua_module_storage_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_system.md", _binary_builtin_lua_modules_docs_lua_module_system_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_system_md_end - _binary_builtin_lua_modules_docs_lua_module_system_md_start) },
        { "skills/builtin_lua_modules/scripts/docs/lua_module_thread.md", _binary_builtin_lua_modules_docs_lua_module_thread_md_start, (uint32_t)(_binary_builtin_lua_modules_docs_lua_module_thread_md_end - _binary_builtin_lua_modules_docs_lua_module_thread_md_start) },
    };
    static const char *const dirs[] = {
        CLAW_STORAGE_SYSTEM_DIR "/skills/board_hardware_info",
        CLAW_STORAGE_SYSTEM_DIR "/skills/plan_mode",
        CLAW_STORAGE_SYSTEM_DIR "/skills/app_creator",
        CLAW_STORAGE_SYSTEM_DIR "/skills/app_creator/references",
        CLAW_STORAGE_SYSTEM_DIR "/skills/app_creator/scripts",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skills_lab_search",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skills_lab_downloader",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skills_lab_downloader/scripts",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skill_creator",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skill_creator/references",
        CLAW_STORAGE_SYSTEM_DIR "/skills/skill_creator/scripts",
        CLAW_STORAGE_SYSTEM_DIR "/skills/lua_package_authoring",
        CLAW_STORAGE_SYSTEM_DIR "/skills/builtin_lua_modules",
        CLAW_STORAGE_SYSTEM_DIR "/skills/builtin_lua_modules/scripts",
        CLAW_STORAGE_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        seed_dir(dirs[i]);
    }

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const claw_seed_file_t *file = &files[i];
        char path[256];
        int n = snprintf(path, sizeof(path), "%s/%s", CLAW_STORAGE_SYSTEM_DIR, file->path);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            ESP_LOGW(TAG, "seed path too long: %s", file->path);
            continue;
        }
        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGW(TAG, "open %s failed: errno %d", path, errno);
            continue;
        }
        /* EMBED_TXTFILES appends a NUL terminator and _end points past it.
         * Keep that byte out of the FAT file: Lua treats it as source text
         * and fails with "unexpected symbol" (the official skill scripts are
         * embedded this way). */
        uint32_t len = file->len;
        while (len > 0 && file->data[len - 1] == '\0') {
            len--;
        }
        if (fwrite(file->data, 1, len, f) != len) {
            ESP_LOGW(TAG, "short write to %s", path);
        }
        fclose(f);
    }
}
