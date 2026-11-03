/*
 * ESP-Claw workspace on the dedicated "claw" FAT partition.
 *
 * The filesystem is created on first boot and never silently reformatted on
 * later boots: if it stops mounting, the workspace is left untouched for
 * recovery instead of losing user scripts.
 */
#include "claw_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "claw_storage";

#define CLAW_PARTITION_LABEL "claw"
/* The suffix is part of the marker so toggling the optional hardware-module
 * Kconfig re-runs the seed pass and restores/removes their docs. */
#if CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
#define CLAW_SEED_VERSION    "45-hw"
#else
#define CLAW_SEED_VERSION    "45"
#endif

#define CLAW_DATA_DIR   CLAW_STORAGE_DATA_DIR
#define CLAW_SYSTEM_DIR CLAW_STORAGE_SYSTEM_DIR
#define CLAW_SEED_MARKER CLAW_SYSTEM_DIR "/.seeded"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;

static const char *const s_data_dirs[] = {
    CLAW_DATA_DIR "/scripts",
    CLAW_DATA_DIR "/schedules",
    CLAW_DATA_DIR "/router_rules",
    CLAW_DATA_DIR "/logs",
    CLAW_DATA_DIR "/skills",
    CLAW_DATA_DIR "/skills_state",
    CLAW_DATA_DIR "/apps",
    CLAW_DATA_DIR "/agent",
    CLAW_DATA_DIR "/agent/sessions",
    CLAW_DATA_DIR "/agent/memory",
};

static const char *const s_system_dirs[] = {
    CLAW_SYSTEM_DIR "/skills",
    CLAW_SYSTEM_DIR "/scripts",
    CLAW_SYSTEM_DIR "/scripts/builtin",
    CLAW_SYSTEM_DIR "/scripts/builtin/lib",
    CLAW_SYSTEM_DIR "/router_rules",
};

static void remove_tree(const char *path);

static const char *const s_seed_hello =
    "-- hello.lua: ESP-Claw 最小示例\n"
    "-- print 的输出会显示在 Claw 页面的任务详情里。\n"
    "print(\"Hello from ESP-Claw\")\n"
    "local name = (args and args.name) or \"world\"\n"
    "print(\"hello, \" .. tostring(name))\n";

static const char *const s_seed_test =
    "-- _test.lua: Claw -> 定时 -> 测试定时 的固定测试脚本\n"
    "print(\"scheduled hello from the panel\")\n";

/* Official ESP-Claw skills, embedded at build time. */
extern const uint8_t scheduled_task_SKILL_md_start[] asm("_binary_scheduled_task_SKILL_md_start");
extern const uint32_t scheduled_task_SKILL_md_length;
extern const uint8_t scheduled_task_lua_start[] asm("_binary_scheduled_task_lua_start");
extern const uint32_t scheduled_task_lua_length;
extern const uint8_t cap_router_mgr_SKILL_md_start[] asm("_binary_cap_router_mgr_SKILL_md_start");
extern const uint32_t cap_router_mgr_SKILL_md_length;
extern const uint8_t cap_scheduler_SKILL_md_start[] asm("_binary_cap_scheduler_SKILL_md_start");
extern const uint32_t cap_scheduler_SKILL_md_length;
extern const uint8_t demo_on_lua_start[] asm("_binary_demo_on_lua_start");
extern const uint32_t demo_on_lua_length;
extern const uint8_t arg_schema_lua_start[] asm("_binary_arg_schema_lua_start");
extern const uint32_t arg_schema_lua_length;
extern const uint8_t memory_ops_SKILL_md_start[] asm("_binary_memory_ops_SKILL_md_start");
extern const uint32_t memory_ops_SKILL_md_length;
extern const uint8_t profile_memory_ops_SKILL_md_start[]
    asm("_binary_profile_memory_ops_SKILL_md_start");
extern const uint32_t profile_memory_ops_SKILL_md_length;
extern const uint8_t cap_http_request_SKILL_md_start[]
    asm("_binary_cap_http_request_SKILL_md_start");
extern const uint32_t cap_http_request_SKILL_md_length;
extern const uint8_t cap_web_search_SKILL_md_start[]
    asm("_binary_cap_web_search_SKILL_md_start");
extern const uint32_t cap_web_search_SKILL_md_length;
extern const uint8_t cap_llm_inspect_image_SKILL_md_start[]
    asm("_binary_cap_llm_inspect_image_SKILL_md_start");
extern const uint32_t cap_llm_inspect_image_SKILL_md_length;
extern const uint8_t lib_fuel_gauge_lua_start[] asm("_binary_lib_fuel_gauge_lua_start");
extern const uint32_t lib_fuel_gauge_lua_length;
extern const uint8_t lib_si12t_touch_lua_start[] asm("_binary_lib_si12t_touch_lua_start");
extern const uint32_t lib_si12t_touch_lua_length;
extern const uint8_t ssd1306_lua_start[] asm("_binary_ssd1306_lua_start");
extern const uint32_t ssd1306_lua_length;
extern const uint8_t ir_driver_lua_start[] asm("_binary_ir_driver_lua_start");
extern const uint32_t ir_driver_lua_length;
extern const uint8_t router_rules_json_start[] asm("_binary_router_rules_json_start");
extern const uint32_t router_rules_json_length;

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno %d", path, errno);
    }
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Treat a missing file or a JSON array containing only brackets/whitespace as
 * "no rules". The event router otherwise creates an empty [] file and never
 * falls back to the seeded recovery rules. */
static bool file_is_empty_rules(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return true;
    }
    char buf[32];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (!isspace(c) && c != '[' && c != ']') {
            return false;
        }
    }
    return true;
}

static void write_file(const char *path, const char *content, bool overwrite)
{
    if (!overwrite && file_exists(path)) {
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "open %s for write failed: errno %d", path, errno);
        return;
    }
    size_t len = strlen(content);
    if (fwrite(content, 1, len, f) != len) {
        ESP_LOGW(TAG, "short write to %s", path);
    }
    fclose(f);
}

static void write_file_bin(const char *path, const uint8_t *start,
                           size_t len, bool overwrite)
{
    if (!overwrite && file_exists(path)) {
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s for write failed: errno %d", path, errno);
        return;
    }
    if (fwrite(start, 1, len, f) != len) {
        ESP_LOGW(TAG, "short write to %s", path);
    }
    fclose(f);
}

extern void claw_skill_seed_write(void);

static void seed_workspace(void)
{
    char marker[32];
    FILE *f = fopen(CLAW_SEED_MARKER, "r");
    bool up_to_date = false;
    if (f) {
        if (fgets(marker, sizeof(marker), f) && strncmp(marker, CLAW_SEED_VERSION, sizeof(CLAW_SEED_VERSION)) == 0) {
            up_to_date = true;
        }
        fclose(f);
    }
    if (up_to_date) {
        return;
    }

    ensure_dir(CLAW_STORAGE_MOUNT_POINT "/claw");
    for (size_t i = 0; i < sizeof(s_system_dirs) / sizeof(s_system_dirs[0]); i++) {
        ensure_dir(s_system_dirs[i]);
    }
    for (size_t i = 0; i < sizeof(s_data_dirs) / sizeof(s_data_dirs[0]); i++) {
        ensure_dir(s_data_dirs[i]);
    }

    /* Official recovery rules: IM messages -> Agent, Agent out_message ->
     * the originating IM channel. Seed the writable copy only when it is
     * missing/empty so user-authored rules are never overwritten. */
    write_file_bin(CLAW_SYSTEM_DIR "/router_rules/router_rules.json",
                   router_rules_json_start, router_rules_json_length, true);
    char data_rules_path[192];
    snprintf(data_rules_path, sizeof(data_rules_path),
             "%s/router_rules/router_rules.json", CLAW_DATA_DIR);
    if (!file_exists(data_rules_path) || file_is_empty_rules(data_rules_path)) {
        write_file_bin(data_rules_path, router_rules_json_start,
                       router_rules_json_length, true);
    }

    /* Drop the retired demo/skill seeds and the removed app runtime from
     * devices that still carry them. */
    remove_tree(CLAW_SYSTEM_DIR "/skills/panel_demo");
    remove_tree(CLAW_SYSTEM_DIR "/skills/snake_demo");
    /* This board has no controllable light; drop the generic light switch. */
    remove_tree(CLAW_SYSTEM_DIR "/skills/light_switch");
    remove_tree(CLAW_DATA_DIR "/skills/snake_demo");
    remove_tree(CLAW_DATA_DIR "/skills/tictactoe");
    remove_tree(CLAW_DATA_DIR "/skills/hello_app");

    /* User scripts are only created once. */
    write_file(CLAW_DATA_DIR "/scripts/hello.lua", s_seed_hello, false);
    write_file(CLAW_DATA_DIR "/scripts/_test.lua", s_seed_test, false);

    /* Official scheduling stack: scheduled_task skill + router manager docs. */
    ensure_dir(CLAW_SYSTEM_DIR "/skills/scheduled_task");
    ensure_dir(CLAW_SYSTEM_DIR "/skills/scheduled_task/scripts");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/scheduled_task/SKILL.md",
                   scheduled_task_SKILL_md_start,
                   scheduled_task_SKILL_md_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/skills/scheduled_task/scripts/add_scheduled_task.lua",
                   scheduled_task_lua_start, scheduled_task_lua_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/cap_router_mgr");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/cap_router_mgr/SKILL.md",
                   cap_router_mgr_SKILL_md_start,
                   cap_router_mgr_SKILL_md_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/cap_scheduler");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/cap_scheduler/SKILL.md",
                   cap_scheduler_SKILL_md_start,
                   cap_scheduler_SKILL_md_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/demo_on.lua",
                   demo_on_lua_start, demo_on_lua_length, true);

    /* Official Lua built-in library: require("arg_schema"). */
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/builtin/lib/arg_schema.lua",
                   arg_schema_lua_start, arg_schema_lua_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/builtin/lib/lib_fuel_gauge.lua",
                   lib_fuel_gauge_lua_start, lib_fuel_gauge_lua_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/builtin/lib/lib_si12t_touch.lua",
                   lib_si12t_touch_lua_start, lib_si12t_touch_lua_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/builtin/lib/ssd1306.lua",
                   ssd1306_lua_start, ssd1306_lua_length, true);
    write_file_bin(CLAW_SYSTEM_DIR "/scripts/builtin/lib/ir_driver.lua",
                   ir_driver_lua_start, ir_driver_lua_length, true);

    /* Official memory skills, enabled with claw_memory. */
    ensure_dir(CLAW_SYSTEM_DIR "/skills/memory_ops");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/memory_ops/SKILL.md",
                   memory_ops_SKILL_md_start,
                   memory_ops_SKILL_md_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/profile_memory_ops");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/profile_memory_ops/SKILL.md",
                   profile_memory_ops_SKILL_md_start,
                   profile_memory_ops_SKILL_md_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/cap_http_request");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/cap_http_request/SKILL.md",
                   cap_http_request_SKILL_md_start,
                   cap_http_request_SKILL_md_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/cap_web_search");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/cap_web_search/SKILL.md",
                   cap_web_search_SKILL_md_start,
                   cap_web_search_SKILL_md_length, true);
    ensure_dir(CLAW_SYSTEM_DIR "/skills/cap_llm_inspect_image");
    write_file_bin(CLAW_SYSTEM_DIR "/skills/cap_llm_inspect_image/SKILL.md",
                   cap_llm_inspect_image_SKILL_md_start,
                   cap_llm_inspect_image_SKILL_md_length, true);

    /* Official workflow/authoring skills (plan_mode, app_creator, skills_lab,
     * skill_creator, lua_package_authoring). */
    claw_skill_seed_write();

    FILE *m = fopen(CLAW_SEED_MARKER, "w");
    if (m) {
        fputs(CLAW_SEED_VERSION, m);
        fclose(m);
    }
}

/* WL FAT puts a dummy 4 KB sector in front of the FAT boot sector. */
static bool sector_is_fat_boot(const uint8_t *sector)
{
    if ((sector[0] != 0xEB && sector[0] != 0xE9) ||
        sector[510] != 0x55 || sector[511] != 0xAA) {
        return false;
    }

    uint16_t bytes_per_sector = (uint16_t)(sector[11] | (sector[12] << 8));
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved_sectors = (uint16_t)(sector[14] | (sector[15] << 8));
    uint8_t fat_count = sector[16];
    if (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
        bytes_per_sector != 2048 && bytes_per_sector != 4096) {
        return false;
    }
    return sectors_per_cluster != 0 && reserved_sectors != 0 && fat_count != 0;
}

static bool partition_has_fat_boot_sector(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CLAW_PARTITION_LABEL);
    if (part == NULL) {
        return false;
    }

    uint8_t sector[512];
    if (esp_partition_read(part, 0, sector, sizeof(sector)) == ESP_OK &&
        sector_is_fat_boot(sector)) {
        return true;
    }
    if (esp_partition_read(part, CONFIG_WL_SECTOR_SIZE, sector, sizeof(sector)) == ESP_OK &&
        sector_is_fat_boot(sector)) {
        return true;
    }
    return false;
}

/* Wear levelling rotates physical sectors, so the FAT signature is not a
 * reliable "is this volume valid" test after runtime. Only a completely
 * erased partition may be formatted automatically. */
static bool partition_is_blank(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CLAW_PARTITION_LABEL);
    if (part == NULL || part->size < 4096) {
        return false;
    }

    const size_t chunk = 4096;
    const size_t offsets[] = { 0, part->size / 2, part->size - chunk };
    static uint8_t buf[4096];
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        size_t offset = offsets[i] & ~(size_t)3;
        if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
            return false;
        }
        for (size_t j = 0; j < chunk; j++) {
            if (buf[j] != 0xFF) {
                return false;
            }
        }
    }
    return true;
}

esp_err_t claw_storage_mount_fs(bool allow_format)
{
    if (s_mounted) {
        return ESP_OK;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ESP_LOGI(TAG, "mounting %s: internal free=%u KB, psram free=%u KB",
             CLAW_PARTITION_LABEL,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        CLAW_STORAGE_MOUNT_POINT, CLAW_PARTITION_LABEL, &mount_cfg, &s_wl);
    if (err != ESP_OK && allow_format && !partition_has_fat_boot_sector()) {
        if (!partition_is_blank()) {
            ESP_LOGE(TAG, "FAT mount failed and partition is not blank; refusing to format");
            return err;
        }
        ESP_LOGW(TAG, "blank Claw partition, formatting once");
        esp_err_t fmt_err = esp_vfs_fat_spiflash_format_rw_wl(
            CLAW_STORAGE_MOUNT_POINT, CLAW_PARTITION_LABEL);
        if (fmt_err == ESP_OK) {
            /* The format helper restores the previous (unmounted) state. */
            err = esp_vfs_fat_spiflash_mount_rw_wl(
                CLAW_STORAGE_MOUNT_POINT, CLAW_PARTITION_LABEL, &mount_cfg, &s_wl);
        } else {
            err = fmt_err;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ensure_dir(CLAW_STORAGE_MOUNT_POINT "/claw");
    return ESP_OK;
}

/* The Agent's module list only shows registered Lua modules, but the seeded
 * builtin_lua_modules skill also carries one doc per module. Remove docs for
 * modules that this board does not register. */
static void prune_unused_hw_lua_docs(void)
{
#if !CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES
    static const char *const docs[] = {
        CLAW_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs/lua_module_button.md",
        CLAW_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs/lua_module_knob.md",
        CLAW_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs/lua_module_led_strip.md",
        CLAW_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs/lua_module_sci.md",
        CLAW_SYSTEM_DIR "/skills/builtin_lua_modules/scripts/docs/lua_module_environmental_sensor.md",
    };
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        if (unlink(docs[i]) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "unlink %s failed: errno %d", docs[i], errno);
        }
    }
#endif
}

static void ensure_default_router_rules(void)
{
    ensure_dir(CLAW_DATA_DIR "/router_rules");
    char path[192];
    snprintf(path, sizeof(path), "%s/router_rules/router_rules.json", CLAW_DATA_DIR);
    if (!file_exists(path) || file_is_empty_rules(path)) {
        write_file_bin(path, router_rules_json_start, router_rules_json_length, true);
    }
}

esp_err_t claw_storage_seed(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_dir(CLAW_DATA_DIR);
    ensure_dir(CLAW_SYSTEM_DIR);
    ensure_dir(CLAW_DATA_DIR "/apps");
    seed_workspace();
    ensure_default_router_rules();
    prune_unused_hw_lua_docs();

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (claw_storage_get_space(&total, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "workspace mounted at %s: %llu KB total, %llu KB free",
                 CLAW_STORAGE_MOUNT_POINT,
                 (unsigned long long)(total / 1024),
                 (unsigned long long)(free_bytes / 1024));
    }
    return ESP_OK;
}

esp_err_t claw_storage_mount(void)
{
    esp_err_t err = claw_storage_mount_fs(true);
    if (err != ESP_OK) {
        return err;
    }
    return claw_storage_seed();
}

esp_err_t claw_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!total_bytes || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t total = 0;
    uint64_t free_b = 0;
    esp_err_t err = esp_vfs_fat_info(CLAW_STORAGE_MOUNT_POINT, &total, &free_b);
    if (err != ESP_OK) {
        return err;
    }
    *total_bytes = total;
    *free_bytes = free_b;
    return ESP_OK;
}

bool claw_storage_is_mounted(void)
{
    return s_mounted;
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        /* Plain file or missing entry. */
        if (unlink(path) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "unlink %s failed: errno %d", path, errno);
        }
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[256];
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child)) {
            continue;
        }
        remove_tree(child);
    }
    closedir(dir);
    if (rmdir(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "rmdir %s failed: errno %d", path, errno);
    }
}

esp_err_t claw_storage_reset_user_data(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    remove_tree(CLAW_DATA_DIR);
    ensure_dir(CLAW_DATA_DIR);
    for (size_t i = 0; i < sizeof(s_data_dirs) / sizeof(s_data_dirs[0]); i++) {
        ensure_dir(s_data_dirs[i]);
    }
    /* Drop the marker so the seed pass recreates user examples. */
    unlink(CLAW_SEED_MARKER);
    seed_workspace();
    ESP_LOGI(TAG, "user workspace reset");
    return ESP_OK;
}
