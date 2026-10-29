#include "hw.h"

#include <stdbool.h>
#include <sys/time.h>

#include "cap_scheduler.h"
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "hw.time";
static bool s_started;

static void sntp_time_sync_cb(struct timeval *tv)
{
    (void)tv;
    /* Same wiring as upstream ESP-Claw app_claw's app_time_sync_success:
     * after the first valid wall clock, rebase cron/absolute schedules.
     * Keep the battery-backed RX8025T close to network time as well. */
    (void)hw_rtc_set_from_system();
    (void)cap_scheduler_handle_time_sync();
}

void hw_sntp_start_once(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_setservername(0, "cn.pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}
