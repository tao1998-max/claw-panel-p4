#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  多语言文案表
 *
 *  约定：
 *   - 所有界面文字都必须从这里取，ui 层不允许出现裸中文字符串字面量
 *   - 新增条目追加到枚举末尾（STR_COUNT 之前），并在 s_zh / s_en 两张表补全
 *   - 改完文案要重跑 tools/fonts/collect_chars.py + gen_fonts.ps1，
 *     否则新增的汉字不在字库里，屏上会显示成方框
 * ========================================================================== */

typedef enum {
    /* 通用 */
    STR_APP_NAME = 0,       /* 产品名，顶栏左侧 */
    STR_NAV_HOME,           /* 底栏：主页 */
    STR_NAV_SYSINFO,        /* 底栏：设备状态 */
    STR_NAV_SETTINGS,       /* 底栏：设置 */
    STR_NAV_HISTORY,        /* 底栏：历史曲线 */

    /* 顶栏 */
    STR_CHROME_UPTIME,      /* 运行 */
    STR_NET_ETH,            /* 有线 */
    STR_NET_WIFI,           /* 无线 */
    STR_NET_OFFLINE,        /* 离线 */

    /* 主页占位 */
    STR_HOME_PRODUCT,       /* 工业控制终端 */
    STR_HOME_VERSION,       /* 固件版本 */
    STR_HOME_MODULES,       /* 就绪模块 */
    STR_HOME_HINT,          /* 业务页面在 ui/pages/ 下添加 */

    /* 设置 */
    STR_SET_TITLE,          /* 设置 */
    STR_SET_BRIGHTNESS,     /* 显示亮度 */
    STR_SET_VOLUME,         /* 扬声器音量 */
    STR_SET_LANGUAGE,       /* 语言 */
    STR_LANG_ZH,            /* 中文 */
    STR_LANG_EN,            /* English */
    STR_SET_SCREEN_OFF,     /* 息屏时间 */
    STR_OFF_NEVER,          /* 常亮 */
    STR_OFF_30S,            /* 30 秒 */
    STR_OFF_1M,             /* 1 分钟 */
    STR_OFF_5M,             /* 5 分钟 */
    STR_SET_FACTORY,        /* 恢复出厂设置 */
    STR_FACTORY_CONFIRM,    /* 确认恢复出厂设置？所有参数将回到默认值 */
    STR_BTN_CONFIRM,        /* 确认 */
    STR_BTN_CANCEL,         /* 取消 */

    /* 设备状态页 */
    STR_SYS_TITLE,          /* 设备状态 */
    STR_SYS_HEAP,           /* 内部内存 */
    STR_SYS_PSRAM,          /* PSRAM 空闲 */
    STR_SYS_UPTIME,         /* 运行时长 */
    STR_SYS_MODULES,        /* 模块状态 */
    STR_SYS_FIRMWARE,       /* 固件 */
    STR_SYS_BOARD,          /* 板型 */
    STR_SYS_CHIP,           /* 芯片 */
    STR_SYS_MAC,            /* MAC 地址 */

    /* 模块名（顺序与 hw_module_t 一致，ui 层按下标映射） */
    STR_MOD_DISPLAY,
    STR_MOD_TOUCH,
    STR_MOD_AUDIO,
    STR_MOD_SDCARD,
    STR_MOD_RS485,
    STR_MOD_ETH,
    STR_MOD_CAMERA,
    STR_MOD_WIFI,

    /* 设置分组标题 */
    STR_GROUP_DISPLAY,      /* 显示 */
    STR_GROUP_SOUND,        /* 声音 */
    STR_GROUP_NETWORK,      /* 网络 */
    STR_GROUP_SYSTEM,       /* 系统 */
    STR_SET_BACK,           /* 返回 */

    /* 网络 / Wi-Fi */
    STR_WIFI_CONNECTED,     /* 已连接 */
    STR_WIFI_DISCONNECTED,  /* 未连接 */
    STR_WIFI_OFF,           /* Wi-Fi 已关闭 */
    STR_WIFI_SCAN,          /* 扫描网络 */
    STR_WIFI_SCANNING,      /* 扫描中… */
    STR_WIFI_NO_AP,         /* 没有找到网络 */
    STR_WIFI_PASSWORD,      /* 密码 */
    STR_WIFI_CONNECT,       /* 连接 */
    STR_WIFI_DISCONNECT,    /* 断开 */
    STR_WIFI_CONNECTING,    /* 连接中… */
    STR_WIFI_FAILED,        /* 连接失败 */
    STR_WIFI_IP,            /* IP 地址 */
    STR_WIFI_SAVED,         /* 已保存 */
    STR_SWITCH_ON,          /* 开 */
    STR_SWITCH_OFF,         /* 关 */

    /* 设置项的说明文字（Material supporting text） */
    STR_HINT_SCREEN_OFF,
    STR_HINT_FACTORY,

    /* 状态词 */
    STR_ST_OK,              /* 就绪 */
    STR_ST_FAIL,            /* 异常 */
    STR_ST_OFF,             /* 未启用 */
    STR_ST_WAIT,            /* 初始化 */

    /* 新版界面：设置页副标题 / 主页状态 */
    STR_SET_CENTER,         /* 设置中心 */
    STR_SET_SUB_DISPLAY,    /* 亮度、息屏与显示行为 */
    STR_SET_SUB_SOUND,      /* 音量与提示音 */
    STR_SET_SUB_NETWORK,    /* Wi-Fi 连接与网络状态 */
    STR_SET_SUB_SYSTEM,     /* 语言、版本与恢复出厂 */
    STR_WIFI_AVAILABLE,     /* 可用网络 */

    /* 历史曲线页 */
    STR_HISTORY_TEMP,       /* 温度 */
    STR_HISTORY_PRESSURE,   /* 压力 */
    STR_HISTORY_SUB,        /* 模拟数据 · 拖动查看 */
    STR_HISTORY_MODE_AUTO,  /* 自适应 */
    STR_HISTORY_MODE_FIXED, /* 定时长 */
    STR_HISTORY_ZOOM_AUTO,  /* 自动 */
    STR_HISTORY_WINDOW,     /* 显示窗口 */
    STR_UNIT_HOUR,          /* 时 */
    STR_UNIT_MIN,           /* 分 */
    STR_UNIT_SEC,           /* 秒 */
    STR_HISTORY_RANGE,      /* 区段查找 */
    STR_HISTORY_RANGE_START,/* 起始 */
    STR_HISTORY_RANGE_END,  /* 结束 */

    STR_COUNT
} i18n_id_t;

typedef enum {
    I18N_LANG_ZH = 0,
    I18N_LANG_EN,
    I18N_LANG_COUNT
} i18n_lang_t;

/* 设置当前语言；只改状态，不触发重绘（重绘由调用方负责） */
void i18n_set_lang(i18n_lang_t lang);

i18n_lang_t i18n_get_lang(void);

/* 取当前语言下的文案；id 越界返回空串，不返回 NULL */
const char *i18n_str(i18n_id_t id);

#ifdef __cplusplus
}
#endif
