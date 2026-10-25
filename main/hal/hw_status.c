#include "hw.h"

/* 硬件状态的唯一定义，由 UI 只读、各驱动模块更新。 */
hw_status_t g_hw[HW_MODULE_COUNT];

static const char *const s_names[HW_MODULE_COUNT] = {
    "DISPLAY", "TOUCH", "AUDIO", "SD CARD", "RS485", "ETH", "CAMERA", "WIFI",
};

const char *hw_name(hw_module_t module)
{
    return (module >= 0 && module < HW_MODULE_COUNT) ? s_names[module] : "UNKNOWN";
}
