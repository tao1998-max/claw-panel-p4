# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# BSP 和摄像头传感器都放在工程内的 local_components/ 下，工程可以独立分发。
# 不把整个 components/ 加进来，避免把 esp-brookesia（依赖 LVGL 8）扫进来。
#
# esp_cam_sensor 用厂商 2.1.0 的本地版本替换 registry 上的版本：
# 本板摄像头模组是 OV02C10（PID 0x5602），需要 2.1.0 里的 ov02c10 驱动。
set(EXTRA_COMPONENT_DIRS
    local_components/espressif__esp32_p4_function_ev_board
    local_components/espressif__esp_cam_sensor
    )

add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "1.1.0")
project(p4_template)
