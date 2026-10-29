idf_component_register(
    SRCS "app_config.c"
    INCLUDE_DIRS "include"
    REQUIRES
        claw_port
        claw_cap
        cap_im_platform
        cap_web_search
        cap_http_request
        esp_wifi
        nvs_flash
        json
)
