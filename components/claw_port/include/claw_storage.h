/*
 * Unified 4 MB FAT volume at /fatfs.
 *
 *   /fatfs/ai                     XiaoZhi font, wake model, prompt OGGs
 *   /fatfs/claw/data/scripts      user + AI generated Lua scripts
 *   /fatfs/claw/data/schedules    scheduler persistence
 *   /fatfs/claw/data/logs         free-form logs
 *   /fatfs/claw/data/agent        agent sessions / memory (optional agent)
 *   /fatfs/claw/system/skills     firmware-seeded skill templates (read-only)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_STORAGE_MOUNT_POINT "/fatfs"
#define CLAW_STORAGE_DATA_DIR    CLAW_STORAGE_MOUNT_POINT "/claw/data"
#define CLAW_STORAGE_SYSTEM_DIR  CLAW_STORAGE_MOUNT_POINT "/claw/system"

/** Mount the shared FAT at /fatfs. allow_format only permits formatting a
 *  completely blank partition; foreign/valid data is never auto-formatted. */
esp_err_t claw_storage_mount_fs(bool allow_format);

/** Create the workspace tree and run the seed pass; idempotent. */
esp_err_t claw_storage_seed(void);

/** Mount the FAT and create/seed the workspace (early mount + seed). */
esp_err_t claw_storage_mount(void);

/** Total/free bytes of the workspace (0 when not mounted). */
esp_err_t claw_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief Erase user data (scripts/schedules/logs/agent) and re-seed.
 *
 * Runs before the NVS factory reset. System skill seeds are restored.
 */
esp_err_t claw_storage_reset_user_data(void);

bool claw_storage_is_mounted(void);

#ifdef __cplusplus
}
#endif
