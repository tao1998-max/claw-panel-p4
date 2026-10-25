#include <stdio.h>
#include <dirent.h>

#include "hw.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "sdmmc_cmd.h"

static const char *TAG = "hw.sd";

esp_err_t hw_storage_init(void)
{
    hw_status_t *st = &g_hw[HW_SDCARD];
    st->enabled = true;

    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "no card");
        ESP_LOGW(TAG, "sdcard mount failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_t *card = bsp_sdcard_get_handle();
    uint64_t bytes = 0;
    if (card != NULL) {
        bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    }

    int files = 0;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir != NULL) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (e->d_name[0] != '.') {
                files++;
            }
        }
        closedir(dir);
    }

    st->ok = true;
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(st->detail, sizeof(st->detail), "%.1f GB, %d files",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0), files);
    } else {
        snprintf(st->detail, sizeof(st->detail), "%.0f MB, %d files",
                 (double)bytes / (1024.0 * 1024.0), files);
    }

    ESP_LOGI(TAG, "sdcard mounted at %s (%s)", BSP_SD_MOUNT_POINT, st->detail);
    return ESP_OK;
}
