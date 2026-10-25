#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include "hw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_TEMPLATE_ENABLE_CAMERA
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#endif

static const char *TAG = "hw.cam";

#if CONFIG_TEMPLATE_ENABLE_CAMERA

#define CAMERA_BUF_COUNT 3
/* ISP 输出走 DMA，缓冲区必须按 cache line 对齐 */
#define CAMERA_BUF_ALIGN 64

typedef struct {
    void *start;
    size_t length;
} camera_buffer_t;

static bool s_video_inited;

/* 打开 MIPI-CSI 设备并真正取一帧，确认 sensor、CSI 和 DMA 通路都正常。 */
static esp_err_t camera_capture_test(char *detail, size_t detail_len)
{
    int fd = -1;
    camera_buffer_t buffers[CAMERA_BUF_COUNT] = { 0 };
    bool streaming = false;
    esp_err_t result = ESP_FAIL;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        snprintf(detail, detail_len, "no sensor");
        ESP_LOGW(TAG, "open %s failed: errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        return ESP_ERR_NOT_FOUND;
    }

    /* 后面的取帧要轮询，先切非阻塞 */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct v4l2_capability capability = { 0 };
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        snprintf(detail, detail_len, "querycap failed");
        goto cleanup;
    }

    struct v4l2_format format = { 0 };
    format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        snprintf(detail, detail_len, "get format failed");
        goto cleanup;
    }

    /* OV02C10 原始输出是 RAW10，必须显式把 /dev/video0 设成 ISP 处理后的 RGB565。
     * 不设格式的话设备还停在 RAW10，ISP 的输出缓冲没人消费，会疯狂刷
     * "ISP: fifo overflow" 直到中断看门狗复位（实测 50 ms 内就 panic）。
     * 这一步对应官方 video_lcd_display 里 app_video_open() 的 VIDIOC_S_FMT。 */
    struct v4l2_format rgb_format = { 0 };
    rgb_format.type = type;
    rgb_format.fmt.pix.width = format.fmt.pix.width;
    rgb_format.fmt.pix.height = format.fmt.pix.height;
    rgb_format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &rgb_format) != 0) {
        snprintf(detail, detail_len, "set RGB565 failed");
        goto cleanup;
    }
    format = rgb_format;

    /* 自己按 RGB565 的尺寸分配缓冲，再以 USERPTR 交给驱动。
     * 用 MMAP 让驱动分配是不行的：驱动按 CSI 的 RAW10 尺寸给 buffer，
     * 而 ISP 实际要往 RGB565（每像素 2 字节，比 RAW10 大 1.6 倍）里写，
     * 写越界就是刷屏的 "ISP: fifo overflow"，最终中断看门狗复位。
     * 官方 video_lcd_display 也是这么做的（app_video_get_buf_size = w*h*2）。 */
    const size_t frame_size = (size_t)format.fmt.pix.width * format.fmt.pix.height * 2;
    struct v4l2_requestbuffers request = {
        .count = CAMERA_BUF_COUNT,
        .type = type,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &request) != 0) {
        snprintf(detail, detail_len, "request buffers failed");
        goto cleanup;
    }

    for (unsigned int i = 0; i < CAMERA_BUF_COUNT; i++) {
        buffers[i].start = heap_caps_aligned_calloc(CAMERA_BUF_ALIGN, 1, frame_size,
                                                    MALLOC_CAP_SPIRAM);
        if (buffers[i].start == NULL) {
            snprintf(detail, detail_len, "no psram for buf");
            goto cleanup;
        }
        buffers[i].length = frame_size;

        struct v4l2_buffer buffer = {
            .type = type,
            .memory = V4L2_MEMORY_USERPTR,
            .index = i,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            snprintf(detail, detail_len, "query buffer failed");
            goto cleanup;
        }
        buffer.m.userptr = (unsigned long)buffers[i].start;
        buffer.length = frame_size;
        if (ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            snprintf(detail, detail_len, "queue buffer failed");
            goto cleanup;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        snprintf(detail, detail_len, "stream on failed");
        goto cleanup;
    }
    streaming = true;

    /* 不用 select()：这个 V4L2 设备的 poll 只有等到超时才报告可读，
     * 实测 3 s / 5 s 两种超时都是卡满才返回，白白拖慢启动。
     * 改成非阻塞 DQBUF 轮询，首帧一到就返回（ISP 3A 收敛约 300 ms）。 */
    struct v4l2_buffer done = {
        .type = type,
        .memory = V4L2_MEMORY_USERPTR,
    };
    bool got_frame = false;
    for (int retry = 0; retry < 300; retry++) {
        if (ioctl(fd, VIDIOC_DQBUF, &done) == 0) {
            got_frame = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!got_frame) {
        snprintf(detail, detail_len, "capture timeout");
        goto cleanup;
    }

    snprintf(detail, detail_len, "%ux%u frame OK",
             (unsigned int)format.fmt.pix.width, (unsigned int)format.fmt.pix.height);
    ESP_LOGI(TAG, "captured one frame: %ux%u, %u bytes",
             (unsigned int)format.fmt.pix.width, (unsigned int)format.fmt.pix.height,
             (unsigned int)done.bytesused);
    result = ESP_OK;

cleanup:
    if (streaming) {
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    for (unsigned int i = 0; i < CAMERA_BUF_COUNT; i++) {
        if (buffers[i].start != NULL) {
            heap_caps_free(buffers[i].start);
        }
    }
    close(fd);
    return result;
}

#endif /* CONFIG_TEMPLATE_ENABLE_CAMERA */

esp_err_t hw_camera_init(void)
{
    hw_status_t *st = &g_hw[HW_CAMERA];

#if CONFIG_TEMPLATE_ENABLE_CAMERA
    st->enabled = true;

    if (!s_video_inited) {
        esp_video_init_csi_config_t csi_config = {
            .sccb_config = {
                /* 复用 BSP 已经建好的 I2C 总线（SDA7/SCL8）。
                 * freq 必须显式给值：esp_video 拿它去 i2c_master_bus_add_device()，
                 * 留 0 会直接报 "invalid scl frequency" 并让整个 esp_video_init 失败。 */
                .init_sccb = false,
                .i2c_handle = bsp_i2c_get_handle(),
                .freq = 100000,
            },
            .reset_pin = GPIO_NUM_NC,
            .pwdn_pin = GPIO_NUM_NC,
        };
        esp_video_init_config_t video_config = {
            .csi = &csi_config,
        };

        esp_err_t err = esp_video_init(&video_config);
        if (err != ESP_OK) {
            st->ok = false;
            snprintf(st->detail, sizeof(st->detail), "video init err");
            ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_video_inited = true;
    }

    esp_err_t err = camera_capture_test(st->detail, sizeof(st->detail));
    st->ok = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera test failed: %s", st->detail);
    } else {
        ESP_LOGI(TAG, "camera test passed: %s", st->detail);
    }
    return err;
#else
    st->enabled = false;
    st->ok = false;
    snprintf(st->detail, sizeof(st->detail), "disabled");
    ESP_LOGI(TAG, "camera disabled (CONFIG_TEMPLATE_ENABLE_CAMERA=n)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
