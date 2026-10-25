#include "i18n.h"

#include <stddef.h>

/* 两张表的下标必须与 i18n_id_t 完全一致，顺序错一个整屏文案就错位。 */

static const char *const s_zh[STR_COUNT] = {
    [STR_APP_NAME]        = "P4 控制台",
    [STR_NAV_HOME]        = "主页",
    [STR_NAV_SYSINFO]     = "设备状态",
    [STR_NAV_SETTINGS]    = "设置",
    [STR_NAV_HISTORY]     = "历史曲线",

    [STR_CHROME_UPTIME]   = "运行",
    [STR_NET_ETH]         = "有线",
    [STR_NET_WIFI]        = "无线",
    [STR_NET_OFFLINE]     = "离线",

    [STR_HOME_PRODUCT]    = "工业控制终端",
    [STR_HOME_VERSION]    = "固件版本",
    [STR_HOME_MODULES]    = "就绪模块",
    [STR_HOME_HINT]       = "业务页面在 ui/pages/ 下添加",

    [STR_SET_TITLE]       = "设置",
    [STR_SET_BRIGHTNESS]  = "显示亮度",
    [STR_SET_VOLUME]      = "扬声器音量",
    [STR_SET_LANGUAGE]    = "语言",
    [STR_LANG_ZH]         = "中文",
    [STR_LANG_EN]         = "English",
    [STR_SET_SCREEN_OFF]  = "息屏时间",
    [STR_OFF_NEVER]       = "常亮",
    [STR_OFF_30S]         = "30 秒",
    [STR_OFF_1M]          = "1 分钟",
    [STR_OFF_5M]          = "5 分钟",
    [STR_SET_FACTORY]     = "恢复出厂设置",
    [STR_FACTORY_CONFIRM] = "确认恢复出厂设置？所有参数将回到默认值",
    [STR_BTN_CONFIRM]     = "确认",
    [STR_BTN_CANCEL]      = "取消",

    [STR_SYS_TITLE]       = "设备状态",
    [STR_SYS_HEAP]        = "内部内存",
    [STR_SYS_PSRAM]       = "PSRAM 空闲",
    [STR_SYS_UPTIME]      = "运行时长",
    [STR_SYS_MODULES]     = "模块状态",
    [STR_SYS_FIRMWARE]    = "固件",
    [STR_SYS_BOARD]       = "板型",
    [STR_SYS_CHIP]        = "芯片",
    [STR_SYS_MAC]         = "MAC 地址",

    [STR_MOD_DISPLAY]     = "显示",
    [STR_MOD_TOUCH]       = "触摸",
    [STR_MOD_AUDIO]       = "音频",
    [STR_MOD_SDCARD]      = "SD 卡",
    [STR_MOD_RS485]       = "RS485",
    [STR_MOD_ETH]         = "以太网",
    [STR_MOD_CAMERA]      = "摄像头",
    [STR_MOD_WIFI]        = "Wi-Fi",

    [STR_GROUP_DISPLAY]   = "显示",
    [STR_GROUP_SOUND]     = "声音",
    [STR_GROUP_NETWORK]   = "网络",
    [STR_GROUP_SYSTEM]    = "系统",
    [STR_SET_BACK]        = "返回",

    [STR_WIFI_CONNECTED]   = "已连接",
    [STR_WIFI_DISCONNECTED] = "未连接",
    [STR_WIFI_OFF]         = "Wi-Fi 已关闭",
    [STR_WIFI_SCAN]        = "扫描网络",
    [STR_WIFI_SCANNING]    = "扫描中…",
    [STR_WIFI_NO_AP]       = "没有找到网络",
    [STR_WIFI_PASSWORD]    = "密码",
    [STR_WIFI_CONNECT]     = "连接",
    [STR_WIFI_DISCONNECT]  = "断开",
    [STR_WIFI_CONNECTING]  = "连接中…",
    [STR_WIFI_FAILED]      = "连接失败",
    [STR_WIFI_IP]          = "IP 地址",
    [STR_WIFI_SAVED]       = "已保存",
    [STR_SWITCH_ON]        = "开",
    [STR_SWITCH_OFF]       = "关",

    [STR_HINT_SCREEN_OFF] = "无操作到时自动关闭背光，触摸即可点亮",
    [STR_HINT_FACTORY]    = "清空全部参数并重启设备",

    [STR_ST_OK]           = "就绪",
    [STR_ST_FAIL]         = "异常",
    [STR_ST_OFF]          = "未启用",
    [STR_ST_WAIT]         = "初始化",

    [STR_SET_CENTER]      = "设置中心",
    [STR_SET_SUB_DISPLAY] = "亮度、息屏与显示行为",
    [STR_SET_SUB_SOUND]   = "音量控制",
    [STR_SET_SUB_NETWORK] = "Wi-Fi 连接与网络状态",
    [STR_SET_SUB_SYSTEM]  = "语言、版本与恢复出厂",
    [STR_WIFI_AVAILABLE]  = "可用网络",

    [STR_HISTORY_TEMP]      = "温度",
    [STR_HISTORY_PRESSURE]  = "压力",
    [STR_HISTORY_SUB]       = "模拟数据 · 拖动查看 · 双指缩放",
    [STR_HISTORY_MODE_AUTO] = "自适应",
    [STR_HISTORY_MODE_FIXED] = "定时长",
    [STR_HISTORY_ZOOM_AUTO] = "自动",
    [STR_HISTORY_WINDOW]    = "显示窗口",
    [STR_UNIT_HOUR]         = "时",
    [STR_UNIT_MIN]          = "分",
    [STR_UNIT_SEC]          = "秒",
    [STR_HISTORY_RANGE]     = "区段查找",
    [STR_HISTORY_RANGE_START] = "起始",
    [STR_HISTORY_RANGE_END] = "结束",
};

static const char *const s_en[STR_COUNT] = {
    [STR_APP_NAME]        = "P4 Console",
    [STR_NAV_HOME]        = "Home",
    [STR_NAV_SYSINFO]     = "Status",
    [STR_NAV_SETTINGS]    = "Settings",
    [STR_NAV_HISTORY]     = "History",

    [STR_CHROME_UPTIME]   = "Up",
    [STR_NET_ETH]         = "ETH",
    [STR_NET_WIFI]        = "WiFi",
    [STR_NET_OFFLINE]     = "Offline",

    [STR_HOME_PRODUCT]    = "Industrial HMI",
    [STR_HOME_VERSION]    = "Firmware",
    [STR_HOME_MODULES]    = "Modules",
    [STR_HOME_HINT]       = "Add business pages under ui/pages/",

    [STR_SET_TITLE]       = "Settings",
    [STR_SET_BRIGHTNESS]  = "Brightness",
    [STR_SET_VOLUME]      = "Volume",
    [STR_SET_LANGUAGE]    = "Language",
    [STR_LANG_ZH]         = "Chinese",
    [STR_LANG_EN]         = "English",
    [STR_SET_SCREEN_OFF]  = "Screen off",
    [STR_OFF_NEVER]       = "Never",
    [STR_OFF_30S]         = "30 s",
    [STR_OFF_1M]          = "1 min",
    [STR_OFF_5M]          = "5 min",
    [STR_SET_FACTORY]     = "Factory reset",
    [STR_FACTORY_CONFIRM] = "Reset all settings to defaults?",
    [STR_BTN_CONFIRM]     = "Confirm",
    [STR_BTN_CANCEL]      = "Cancel",

    [STR_SYS_TITLE]       = "Device status",
    [STR_SYS_HEAP]        = "Internal RAM",
    [STR_SYS_PSRAM]       = "PSRAM free",
    [STR_SYS_UPTIME]      = "Uptime",
    [STR_SYS_MODULES]     = "Modules",
    [STR_SYS_FIRMWARE]    = "Firmware",
    [STR_SYS_BOARD]       = "Board",
    [STR_SYS_CHIP]        = "Chip",
    [STR_SYS_MAC]         = "MAC",

    [STR_MOD_DISPLAY]     = "Display",
    [STR_MOD_TOUCH]       = "Touch",
    [STR_MOD_AUDIO]       = "Audio",
    [STR_MOD_SDCARD]      = "SD card",
    [STR_MOD_RS485]       = "RS485",
    [STR_MOD_ETH]         = "Ethernet",
    [STR_MOD_CAMERA]      = "Camera",
    [STR_MOD_WIFI]        = "Wi-Fi",

    [STR_GROUP_DISPLAY]   = "Display",
    [STR_GROUP_SOUND]     = "Sound",
    [STR_GROUP_NETWORK]   = "Network",
    [STR_GROUP_SYSTEM]    = "System",
    [STR_SET_BACK]        = "Back",

    [STR_WIFI_CONNECTED]   = "Connected",
    [STR_WIFI_DISCONNECTED] = "Disconnected",
    [STR_WIFI_OFF]         = "Wi-Fi off",
    [STR_WIFI_SCAN]        = "Scan networks",
    [STR_WIFI_SCANNING]    = "Scanning...",
    [STR_WIFI_NO_AP]       = "No networks found",
    [STR_WIFI_PASSWORD]    = "Password",
    [STR_WIFI_CONNECT]     = "Connect",
    [STR_WIFI_DISCONNECT]  = "Disconnect",
    [STR_WIFI_CONNECTING]  = "Connecting...",
    [STR_WIFI_FAILED]      = "Connect failed",
    [STR_WIFI_IP]          = "IP address",
    [STR_WIFI_SAVED]       = "Saved",
    [STR_SWITCH_ON]        = "On",
    [STR_SWITCH_OFF]       = "Off",

    [STR_HINT_SCREEN_OFF] = "Backlight turns off when idle, tap to wake",
    [STR_HINT_FACTORY]    = "Clears all settings and reboots",

    [STR_ST_OK]           = "OK",
    [STR_ST_FAIL]         = "FAIL",
    [STR_ST_OFF]          = "OFF",
    [STR_ST_WAIT]         = "INIT",

    [STR_SET_CENTER]      = "Settings center",
    [STR_SET_SUB_DISPLAY] = "Brightness, timeout and display",
    [STR_SET_SUB_SOUND]   = "Volume control",
    [STR_SET_SUB_NETWORK] = "Wi-Fi connection and network",
    [STR_SET_SUB_SYSTEM]  = "Language, version and reset",
    [STR_WIFI_AVAILABLE]  = "Available networks",

    [STR_HISTORY_TEMP]      = "Temperature",
    [STR_HISTORY_PRESSURE]  = "Pressure",
    [STR_HISTORY_SUB]       = "Simulated · drag · pinch to zoom",
    [STR_HISTORY_MODE_AUTO] = "Auto",
    [STR_HISTORY_MODE_FIXED] = "Fixed",
    [STR_HISTORY_ZOOM_AUTO] = "Auto",
    [STR_HISTORY_WINDOW]    = "Window",
    [STR_UNIT_HOUR]         = "h",
    [STR_UNIT_MIN]          = "min",
    [STR_UNIT_SEC]          = "s",
    [STR_HISTORY_RANGE]     = "Range",
    [STR_HISTORY_RANGE_START] = "Start",
    [STR_HISTORY_RANGE_END] = "End",
};

static i18n_lang_t s_lang = I18N_LANG_ZH;

void i18n_set_lang(i18n_lang_t lang)
{
    s_lang = (lang >= 0 && lang < I18N_LANG_COUNT) ? lang : I18N_LANG_ZH;
}

i18n_lang_t i18n_get_lang(void)
{
    return s_lang;
}

const char *i18n_str(i18n_id_t id)
{
    if (id < 0 || id >= STR_COUNT) {
        return "";
    }
    const char *text = (s_lang == I18N_LANG_EN) ? s_en[id] : s_zh[id];
    return (text != NULL) ? text : "";
}
