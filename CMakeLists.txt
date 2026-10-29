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

# 小智聊天字体 + 唤醒词模型放进合并后的 4 MB FAT 的 /ai 目录。
# 该镜像不能用 FLASH_IN_PROJECT：普通 idf.py flash 不能覆盖 /fatfs/claw
# 工作区；首次迁移/整片擦除后显式执行 `idf.py claw-flash`。
set(CLAW_FAT_STAGE_DIR "${CMAKE_BINARY_DIR}/claw_fat_root")

add_custom_target(claw_fat_stage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLAW_FAT_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAW_FAT_STAGE_DIR}/ai"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/ai" "${CLAW_FAT_STAGE_DIR}/ai"
    COMMENT "Staging assets/ai as /ai for the Claw FAT image")

fatfs_create_spiflash_image(claw "${CLAW_FAT_STAGE_DIR}")
add_dependencies(fatfs_claw_bin claw_fat_stage)
