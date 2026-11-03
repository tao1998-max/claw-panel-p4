/*
 * Epson RX8025T-UC RTC on the shared BSP I2C bus.
 *
 * The chip keeps UTC calendar time (BCD) on a backup battery. This driver
 * reads it at boot to seed the system clock and writes UTC back after SNTP
 * so the two stay close. Older firmware stored local time; a one-time NVS
 * flag migrates the chip to UTC (see s_rtc_utc). It is intentionally small:
 * only the calendar registers are used; alarm/timer outputs are left alone.
 */
#include "hw.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define RTC_I2C_ADDR       0x32
#define RTC_I2C_HZ         100000
#define RTC_REG_SECONDS    0x00
#define RTC_REG_FLAGS      0x0E
#define RTC_FLAG_VDET      0x01
#define RTC_FLAG_VLF       0x02

static const char *TAG = "hw.rtc";
static i2c_master_dev_handle_t s_rtc_dev;
static esp_timer_handle_t s_sync_timer;

/* The RX8025T is kept in UTC so the stored value does not depend on TZ/DST.
 * Older firmware stored local time; a one-time NVS flag migrates the chip. */
#define RTC_NVS_NAMESPACE "hw_rtc"
#define RTC_NVS_UTC_KEY   "utc"
static bool s_rtc_utc;

static void rtc_load_utc_mode(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(RTC_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, RTC_NVS_UTC_KEY, &value) == ESP_OK) {
            s_rtc_utc = value != 0;
        }
        nvs_close(handle);
    }
}

static void rtc_save_utc_mode(void)
{
    nvs_handle_t handle;
    if (nvs_open(RTC_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u8(handle, RTC_NVS_UTC_KEY, s_rtc_utc ? 1 : 0);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool bcd_is_valid(uint8_t value)
{
    return (value & 0x0F) <= 9 && ((value >> 4) & 0x0F) <= 9;
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, data, len, 100);
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buffer[8];
    if (len + 1 > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[0] = reg;
    memcpy(buffer + 1, data, len);
    return i2c_master_transmit(s_rtc_dev, buffer, len + 1, 100);
}

static bool rtc_decode_now(const uint8_t raw[7], struct tm *out)
{
    if (!raw || !out) {
        return false;
    }
    for (int i = 0; i < 7; i++) {
        /* Register 0x03 is a weekday bitfield, not BCD; the upper bits are
         * reserved/flag bits and may read as non-BCD on a fresh part. */
        if (i == 3) {
            continue;
        }
        if (!bcd_is_valid(raw[i] & (i == 2 ? 0x3F : 0x7F))) {
            return false;
        }
    }

    uint8_t hour;
    if (raw[2] & 0x40) {
        /* 12-hour mode: bit5 is PM, 12 AM/PM means 0/12. */
        bool pm = (raw[2] & 0x20) != 0;
        hour = bcd_to_bin(raw[2] & 0x1F);
        if (hour > 12) {
            return false;
        }
        if (hour == 12) {
            hour = 0;
        }
        if (pm) {
            hour += 12;
        }
    } else {
        hour = bcd_to_bin(raw[2] & 0x3F);
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec = bcd_to_bin(raw[0] & 0x7F);
    out->tm_min = bcd_to_bin(raw[1] & 0x7F);
    out->tm_hour = hour;
    out->tm_wday = raw[3] & 0x07;
    out->tm_mday = bcd_to_bin(raw[4] & 0x3F);
    out->tm_mon = bcd_to_bin(raw[5] & 0x1F) - 1;
    out->tm_year = 100 + bcd_to_bin(raw[6]); /* 2000..2099 */
    out->tm_isdst = -1;

    if (out->tm_sec > 59 || out->tm_min > 59 || out->tm_hour > 23 ||
        out->tm_mday < 1 || out->tm_mday > 31 ||
        out->tm_mon < 0 || out->tm_mon > 11) {
        return false;
    }
    return true;
}

static esp_err_t rtc_open(void)
{
    if (s_rtc_dev) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK) {
            return err;
        }
        bus = bsp_i2c_get_handle();
    }
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2c_master_probe(bus, RTC_I2C_ADDR, 100) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_I2C_ADDR,
        .scl_speed_hz = RTC_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_rtc_dev);
}

static bool rtc_is_leap_year(int year)
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/* ESP-IDF's newlib build does not expose timegm(). Convert a UTC struct tm
 * without touching the process TZ, which is shared with the UI/SNTP tasks. */
static time_t rtc_tm_to_utc_epoch(const struct tm *tm)
{
    static const int month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int year = tm->tm_year + 1900;
    int64_t days = 0;

    for (int y = 1970; y < year; y++) {
        days += rtc_is_leap_year(y) ? 366 : 365;
    }
    for (int m = 0; m < tm->tm_mon; m++) {
        days += month_days[m];
        if (m == 1 && rtc_is_leap_year(year)) {
            days += 1;
        }
    }
    days += tm->tm_mday - 1;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 +
                    tm->tm_min * 60 + tm->tm_sec);
}

static esp_err_t rtc_read_epoch(time_t *out_epoch, struct tm *out_tm, uint8_t *out_flags)
{
    uint8_t raw[7] = {0};
    uint8_t flags = 0;
    struct tm tm_now = {0};
    esp_err_t err;

    if (!out_epoch || !out_tm) {
        return ESP_ERR_INVALID_ARG;
    }
    err = rtc_read(RTC_REG_FLAGS, &flags, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (flags & RTC_FLAG_VLF) {
        if (out_flags) {
            *out_flags = flags;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = rtc_read(RTC_REG_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    if (!rtc_decode_now(raw, &tm_now)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    time_t epoch = s_rtc_utc ? rtc_tm_to_utc_epoch(&tm_now) : mktime(&tm_now);
    if (epoch <= (time_t)1600000000LL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_epoch = epoch;
    *out_tm = tm_now;
    if (out_flags) {
        *out_flags = flags;
    }
    return ESP_OK;
}

static void rtc_periodic_sync_cb(void *arg)
{
    (void)arg;
    time_t rtc_epoch = 0;
    struct tm tm_now = {0};
    uint8_t flags = 0;
    time_t system_now = time(NULL);

    if (system_now <= (time_t)1600000000LL) {
        return;
    }
    if (rtc_read_epoch(&rtc_epoch, &tm_now, &flags) != ESP_OK) {
        return;
    }

    int64_t delta = (int64_t)rtc_epoch - (int64_t)system_now;
    if (llabs(delta) <= 2) {
        return;
    }
    if (llabs(delta) > 300) {
        /* A large gap means the RTC is stale or was never written; repair it
         * from the system clock instead of pulling the system back in time. */
        (void)hw_rtc_set_from_system();
        return;
    }

    struct timeval tv = {.tv_sec = rtc_epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL) == 0) {
        ESP_LOGI(TAG, "system time resynced from RX8025T: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }
}

static void rtc_start_periodic_sync(void)
{
    if (s_sync_timer) {
        return;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = rtc_periodic_sync_cb,
        .name = "rtc_sync",
    };
    if (esp_timer_create(&timer_args, &s_sync_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_sync_timer, 60ULL * 1000 * 1000);
    }
}

esp_err_t hw_rtc_init(void)
{
    uint8_t flags = 0;
    time_t rtc_epoch = 0;
    struct tm tm_now = {0};
    esp_err_t err = rtc_open();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T not available at 0x%02x: %s",
                 RTC_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    rtc_start_periodic_sync();
    rtc_load_utc_mode();

    err = rtc_read_epoch(&rtc_epoch, &tm_now, &flags);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T time unusable (flags=0x%02x, err=%s); keeping current clock",
                 flags, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RX8025T reads %04d-%02d-%02d %02d:%02d:%02d (flags=0x%02x, %s)",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, flags,
             s_rtc_utc ? "UTC" : "legacy local");

    /* RX8025T is the boot-time source. Override the ESP-IDF RTC-retained
     * boot time as well, so a software reset does not bypass the external
     * RTC. SNTP later refines both the system clock and the RX8025T. */
    struct timeval tv = {.tv_sec = rtc_epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday from RX8025T failed");
        return ESP_FAIL;
    }
    char local_buf[32] = "(localtime unavailable)";
    struct tm local_tm = {0};
    if (localtime_r(&rtc_epoch, &local_tm) != NULL) {
        strftime(local_buf, sizeof(local_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
    }
    ESP_LOGI(TAG, "system time seeded from RX8025T: %04d-%02d-%02d %02d:%02d:%02d UTC (%s local)",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, local_buf);

    if (!s_rtc_utc) {
        /* One-time migration: the old firmware stored local time in the RTC.
         * The system clock above is correct; rewrite the chip in UTC and mark
         * it so later boots use timegm(). */
        (void)hw_rtc_set_from_system();
        ESP_LOGI(TAG, "RX8025T migrated from local to UTC storage");
    }
    return ESP_OK;
}

esp_err_t hw_rtc_set_datetime(int year, int month, int day, int hour, int minute, int second)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm tm_value = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_sec = second,
        .tm_isdst = -1,
    };
    time_t epoch = mktime(&tm_value);
    if (epoch <= (time_t)1600000000LL ||
        tm_value.tm_year != year - 1900 || tm_value.tm_mon != month - 1 ||
        tm_value.tm_mday != day || tm_value.tm_hour != hour ||
        tm_value.tm_min != minute || tm_value.tm_sec != second) {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    /* Best effort: keep the external RTC in sync even if the chip is absent. */
    if (s_rtc_dev) {
        (void)hw_rtc_set_from_system();
    }
    ESP_LOGI(TAG, "system time set manually: %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return ESP_OK;
}

esp_err_t hw_rtc_set_from_system(void)
{
    struct timeval tv = {0};
    struct tm tm_now = {0};
    uint8_t raw[7];

    if (!s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    gettimeofday(&tv, NULL);
    if (tv.tv_sec <= (time_t)1600000000LL) {
        return ESP_ERR_INVALID_STATE;
    }
    time_t seconds = tv.tv_sec;
    if (!gmtime_r(&seconds, &tm_now)) {
        return ESP_FAIL;
    }

    raw[0] = bin_to_bcd((uint8_t)tm_now.tm_sec);
    raw[1] = bin_to_bcd((uint8_t)tm_now.tm_min);
    raw[2] = bin_to_bcd((uint8_t)tm_now.tm_hour); /* 24-hour mode, bit6=0 */
    raw[3] = (uint8_t)(tm_now.tm_wday & 0x07);
    raw[4] = bin_to_bcd((uint8_t)tm_now.tm_mday);
    raw[5] = bin_to_bcd((uint8_t)(tm_now.tm_mon + 1));
    raw[6] = bin_to_bcd((uint8_t)((tm_now.tm_year + 1900) % 100));

    esp_err_t err = rtc_write(RTC_REG_SECONDS, raw, sizeof(raw));
    if (err == ESP_OK) {
        /* Read-modify-write the flag register; only clear VLF/VDET so other
         * control bits are preserved. */
        uint8_t flags = 0;
        if (rtc_read(RTC_REG_FLAGS, &flags, 1) == ESP_OK) {
            flags &= (uint8_t)~(RTC_FLAG_VLF | RTC_FLAG_VDET);
            (void)rtc_write(RTC_REG_FLAGS, &flags, 1);
        }
        ESP_LOGI(TAG, "RX8025T updated from system (UTC): %04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        s_rtc_utc = true;
        rtc_save_utc_mode();
    } else {
        ESP_LOGW(TAG, "write RX8025T failed: %s", esp_err_to_name(err));
    }
    return err;
}
