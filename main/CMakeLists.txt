idf_component_register(
    SRCS
        "app/app_main.c"
        "app/app.c"
        "core/i18n.c"
        "core/settings.c"
        "hal/hw_status.c"
        "hal/hw_display.c"
        "hal/hw_audio.c"
        "hal/hw_storage.c"
        "hal/hw_rs485.c"
        "hal/hw_eth.c"
        "hal/hw_camera.c"
        "hal/hw_wifi.c"
        "ui/ui.c"
        "ui/ui_widgets.c"
        "ui/ui_shell.c"
        "ui/pages/page_home.c"
        "ui/pages/page_sysinfo.c"
        "ui/pages/page_settings.c"
        "ui/pages/page_history.c"
        "ui/fonts/font_zh_16.c"
        "ui/fonts/font_zh_20.c"
        "ui/fonts/font_zh_28.c"
        "ui/fonts/font_net_30.c"
    INCLUDE_DIRS
        "app"
        "core"
        "hal"
        "ui"
        "ui/pages"
    REQUIRES
        espressif__esp32_p4_function_ev_board
        espressif__esp_video
        esp_wifi
        driver
        esp_eth
        esp_timer
        esp_event
        esp_netif
        esp_app_format
        lwip
        nvs_flash
)

# 两点说明：
#
# 1) esp_wifi 必须显式写进来，即使本板是 ESP32-P4（没有射频）。
#    esp_wifi_remote 会把 idf_v5.5/include/injected 注入到 esp_wifi 组件的
#    INTERFACE_INCLUDE_DIRECTORIES，hw_wifi.c 里的 #include "esp_wifi.h"
#    是这么找到的；光靠 BSP 传递的 esp_wifi_remote 头文件目录不够。
#    关掉 CONFIG_TEMPLATE_ENABLE_WIFI 时 esp_wifi 只是个空壳，不会增加体积。
#
# 2) 这里不能用 if(CONFIG_xxx) 去裁剪 REQUIRES。IDF 5.5 在加载 Kconfig 之前
#    会先跑一遍组件 CMakeLists 来收集依赖，那一遍里所有 CONFIG_* 都是空的，
#    条件判断会把依赖减掉，最终编译就报 "esp_wifi.h: No such file or directory"。
