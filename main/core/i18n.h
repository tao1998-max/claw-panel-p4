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
    STR_SYS_STORAGE,        /* 存储 */
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

    /* 小智 AI */
    STR_GROUP_AI,           /* AI 助手 */
    STR_SET_SUB_AI,         /* 小智语音与对话 */
    STR_AI_TITLE,           /* 小智助手 */
    STR_AI_STATUS,          /* 状态 */
    STR_AI_ENABLE,          /* 启用 */
    STR_AI_SERVER,          /* 服务地址 */
    STR_AI_DEVICE_ID,       /* 设备 ID */
    STR_AI_ACTIVATION,      /* 激活 */
    STR_AI_REACTIVATE,      /* 重新激活 */
    STR_AI_WAKE,            /* 离线唤醒词 */
    STR_AI_WAKE_SPEAKING,   /* 回答时检测唤醒词 */
    STR_AI_MUTE,            /* 麦克风静音 */
    STR_AI_CHAT_HINT,       /* 免提提示 */
    STR_AI_START,           /* 开始对话 */
    STR_AI_STOP,            /* 结束对话 */
    STR_AI_RETRY,           /* 重试 */
    STR_AI_CLOSE,           /* 关闭 */
    STR_AI_ST_STARTING,     /* 启动中 */
    STR_AI_ST_ACTIVATING,   /* 激活中 */
    STR_AI_ST_IDLE,         /* 待命 */
    STR_AI_ST_CONNECTING,   /* 连接中 */
    STR_AI_ST_LISTENING,    /* 聆听中 */
    STR_AI_ST_SPEAKING,     /* 回答中 */
    STR_AI_ST_ERROR,        /* 出错 */
    STR_AI_ST_DISABLED,     /* 已停用 */
    STR_AI_OFFLINE,         /* 未联网 */
    STR_AI_ACTIVATION_HINT, /* 绑定提示 */
    STR_AI_NO_ACTIVATION,   /* 尚未激活 */
    STR_AI_MUTED,           /* 麦克风已静音 */
    STR_AI_WAKE_HINT,       /* 唤醒提示 */
    STR_AI_SERVER_HINT,     /* 留空使用官方 OTA */
    STR_AI_REACTIVATE_CONFIRM,
    STR_AI_URL_INVALID,
    STR_AI_SETTINGS_NOTE,
    STR_AI_DEVICE_HINT,
    STR_AI_REBOOT_HINT,
    STR_AI_ACTIVATED,
    STR_HOME_DEMO_TITLE,
    STR_HOME_DEMO_SUB,
    STR_HOME_DEMO_SWITCH,
    STR_HOME_DEMO_ON,
    STR_HOME_DEMO_OFF,
    STR_HOME_NETWORK,       /* 主页：网络 */
    STR_HOME_RECENT_TASKS,  /* 主页：最近任务 */

    /* ESP-Claw 自动化 */
    STR_NAV_CLAW,           /* 底栏：Claw */
    STR_CLAW_TAB_SETTINGS,  /* 设置 */
    STR_CLAW_LUA_AI,        /* 允许小智运行 Lua */
    STR_CLAW_LUA_AI_HINT,   /* 说明 */
    STR_CLAW_ENABLE_CONFIRM,/* 开启确认 */
    STR_CLAW_RUN,           /* 运行 */
    STR_CLAW_JOBS,          /* 运行中 */
    STR_CLAW_CLOUD_ON,      /* 小智 Lua：开 */
    STR_CLAW_CLOUD_OFF,     /* 小智 Lua：关 */
    STR_CLAW_REFRESH,       /* 刷新 */
    STR_CLAW_DELETE,        /* 删除 */
    STR_CLAW_DELETE_CONFIRM,/* 确认删除？ */

    /* Claw 定时任务 */
    STR_CLAW_NO_SCHEDULES,      /* 暂无定时任务 */
    STR_CLAW_SCHEDULE_TRIGGER,  /* 触发 */
    STR_CLAW_SCHEDULE_ENABLE,   /* 启用 */
    STR_CLAW_SCHEDULE_DISABLE,  /* 停用 */
    STR_CLAW_SCHEDULE_ONCE,     /* 单次 */
    STR_CLAW_SCHEDULE_INTERVAL, /* 间隔 */
    STR_CLAW_SCHEDULE_CRON,     /* Cron */
    STR_CLAW_SCHEDULE_NEXT,     /* 下次 */
    STR_CLAW_SCHEDULE_DONE,     /* 已完成 */
    STR_CLAW_SCHEDULE_DUE,      /* 即将触发 */

    /* Claw 管理页（参考 ESP-Claw Web UI 的侧栏/表格布局） */
    STR_CLAW_NAV_FILES,         /* 文件 */
    STR_CLAW_DESC_SCHEDULES,
    STR_CLAW_DESC_AGENT,
    STR_CLAW_DESC_SETTINGS,
    STR_CLAW_FILES_FAT,         /* 内部存储 */
    STR_CLAW_FILES_SD,          /* SD 卡 */
    STR_CLAW_SD_NOT_MOUNTED,    /* SD 卡未检测到 */

    /* 本地 Agent */
    STR_CLAW_AGENT_OFF,         /* Agent 关 */
    STR_CLAW_AGENT_ON,          /* Agent 已开启（未发过请求） */
    STR_CLAW_AGENT_UNCONFIGURED,/* Agent 未配置 */
    STR_CLAW_AGENT_READY,       /* Agent 就绪 */
    STR_CLAW_AGENT_BUSY,        /* Agent 忙 */
    STR_CLAW_AGENT_TITLE,       /* 本地 Agent */
    STR_CLAW_AGENT_DISABLED,    /* 本地 Agent 未开启 */
    STR_CLAW_AGENT_DISABLED_HINT,
    STR_CLAW_AGENT_GO_SETTINGS, /* 去设置 */
    STR_CLAW_AGENT_SETTINGS,    /* Agent 设置 */
    STR_CLAW_AGENT_ENABLE_CONFIRM,
    STR_CLAW_AGENT_BACKEND,     /* 接口类型 */
    STR_CLAW_AGENT_URL,         /* 接口地址 */
    STR_CLAW_AGENT_MODEL,       /* 模型名称 */
    STR_CLAW_AGENT_KEY,         /* API Key */
    STR_CLAW_AGENT_KEY_HINT,
    STR_CLAW_AGENT_KEY_SET,
    STR_CLAW_AGENT_KEY_NONE,
    STR_CLAW_AGENT_NEED_CONFIG,
    STR_CLAW_AGENT_ASK,         /* 向小爪提问 */
    STR_CLAW_AGENT_SEND,        /* 发送 */
    STR_CLAW_AGENT_KBD_HINT,

    /* Claw 管理页：对齐 ESP-Claw Web UI 的栏目与内容 */
    STR_CLAW_NAV_MEMORY,        /* 记忆管理 */
    STR_CLAW_NAV_CHAT,          /* 在线聊天 */
    STR_CLAW_SEC_STATUS_RUN,    /* 运行状态 */
    STR_CLAW_WORKSPACE,
    STR_CLAW_MEMORY_LONG,
    STR_CLAW_MEMORY_SOUL,
    STR_CLAW_MEMORY_IDENTITY,
    STR_CLAW_MEMORY_USER,
    STR_CLAW_WEB_HINT,
    STR_CLAW_UP,

    /* Claw 四页签重构新增 */
    STR_CLAW_TAB_AUTOMATION,    /* 自动化 */
    STR_CLAW_DESC_WORKSPACE,    /* 记忆与文件 */
    STR_CLAW_CLAW_UNAVAILABLE,  /* Claw 不可用 */
    STR_CLAW_FILE_TOO_LARGE,    /* 文件太大，仅显示前 64 KB */
    STR_CLAW_FILE_OPEN_FAILED,  /* 打开文件失败 */
    STR_CLAW_FILES_EMPTY,       /* 暂无文件 */
    STR_CLAW_ACTION_TRIGGERED,  /* 已触发 */
    STR_CLAW_ACTION_ENABLED,    /* 已启用 */
    STR_CLAW_ACTION_DISABLED,   /* 已停用 */
    STR_CLAW_ACTION_DELETED,    /* 已删除 */
    STR_CLAW_ACTION_FAILED,     /* 操作失败 */
    STR_CLAW_SCHEDULE_CREATE_HINT,
    STR_CLAW_SCHEDULE_LIMIT,    /* 显示 */
    STR_CLAW_CHAT_SESSION_UNAVAILABLE,
    STR_CLAW_CHAT_SESSION_FAILED,
    STR_CLAW_NO_MESSAGES,       /* 暂无消息 */
    STR_CLAW_SESSION_PICKER,    /* 会话 */
    STR_CLAW_NEW_SESSION,       /* 新建会话 */
    STR_CLAW_NO_SESSIONS,       /* 暂无会话 */
    STR_CLAW_AGENT_STATUS_UNAVAILABLE,
    STR_CLAW_TTS_TITLE,          /* 豆包语音 */
    STR_CLAW_TTS_HINT,           /* API Key 与 AppID+Token 二选一 */
    STR_CLAW_TTS_RESOURCE,       /* 资源 ID */
    STR_CLAW_TTS_SPEAKER,        /* 音色 */
    STR_CLAW_TTS_AUTO_SPEAK,     /* 自动播报 */

    STR_NAV_APPS,               /* 底栏：应用 */
    STR_APPS_TITLE,             /* 应用页标题 */
    STR_APPS_EMPTY,             /* 暂无应用 */
    STR_APPS_LAUNCH,            /* 启动 */
    STR_APPS_FAILED,            /* 启动失败 */

    /* 时间设置 */
    STR_SET_TIME,               /* 时间设置 */
    STR_SET_TIME_CURRENT,       /* 当前时间 */
    STR_SET_TIME_BUTTON,        /* 设置时间 */
    STR_TIME_INVALID,           /* 时间无效 */

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
