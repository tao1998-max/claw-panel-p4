#include "hw.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "hw.time";
static bool s_started;

void hw_sntp_start_once(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "cn.pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}
