#pragma once

#include "lvgl.h"

/* ==========================================================================
 *  设计令牌：颜色 / 间距 / 字号 / 尺寸
 *
 *  「Aurora console」深色方案：
 *  - 背景接近黑蓝，面板用低对比的垂直渐变 + 1 px 冷色描边分层；
 *  - 强调色是靛蓝 → 紫罗兰的渐变，青色/绿色/红色只用于状态；
 *  - 大面板不投阴影（1024×600 全屏重绘时阴影太贵），只有滑块、弹层、
 *    图标块这类小元素用一点点柔光。
 *  所有 ui 代码只从本文件取样式常量，不要在页面里写裸数字/裸颜色。
 * ========================================================================== */

/* ---------- 表面层次 ---------- */
#define UI_COLOR_BG          0x060A14   /* 页面底，最深 */
#define UI_COLOR_BG_BOT      0x091120   /* 页面底渐变的下端 */
#define UI_COLOR_SURFACE     0x0E1727   /* 顶栏 / 底栏 */
#define UI_COLOR_SURFACE_TOP 0x101A2D   /* 大面板渐变上端 */
#define UI_COLOR_SURFACE_BOT 0x0B1322   /* 大面板渐变下端 */
#define UI_COLOR_PANEL       0x0D1728   /* 大面板半透明纯色（避免渐变断层） */
#define UI_COLOR_CARD_TOP    0x131E33   /* 面板内卡片渐变上端 */
#define UI_COLOR_CARD_BOT    0x0E1627   /* 面板内卡片渐变下端 */
#define UI_COLOR_SURFACE_HI  0x131E33   /* 卡片高亮（按下 / 悬停） */
#define UI_COLOR_TILE        0x1A2740   /* 控件底：分段器、次要按钮、胶囊 */
#define UI_COLOR_TILE_DARK   0x10192B   /* 分段器轨道 */
#define UI_COLOR_BORDER      0x243350   /* 常规描边 */
#define UI_COLOR_BORDER_SOFT 0x1A253B   /* 极低对比描边 */

/* ---------- 文字 ---------- */
#define UI_COLOR_TEXT        0xF4F7FC
#define UI_COLOR_MUTED       0x93A0B8
#define UI_COLOR_DISABLED    0x5C6A84

/* ---------- 强调色与状态 ---------- */
#define UI_COLOR_ACCENT      0x7B88FF   /* 主强调：靛蓝 */
#define UI_COLOR_ACCENT_DEEP 0x6D76F5   /* 渐变起点 */
#define UI_COLOR_VIOLET      0xA07CFF   /* 渐变终点 */
#define UI_COLOR_ACCENT_SOFT 0xA5B0FF
#define UI_COLOR_ON_ACCENT   0x0B0F1D   /* 实心强调色块上的文字 */
#define UI_COLOR_CYAN        0x4DD6CB
#define UI_COLOR_OK          0x3DDC97
#define UI_COLOR_WARN        0xF6B84C
#define UI_COLOR_ERROR       0xFF6B81
#define UI_COLOR_DECOR       0x2B3760   /* 大号装饰序号，浮在面板上 */

/* 透明度：31 ≈ 12%，45 ≈ 18%，60 ≈ 24% */
#define UI_OPA_FAINT         18
#define UI_OPA_SOFT          31
#define UI_OPA_TINT          45
#define UI_OPA_STRONG        70

/* ---------- 间距 ---------- */
#define UI_SPACE_XS          4
#define UI_SPACE_S           8
#define UI_SPACE_M           16
#define UI_SPACE_L           24
#define UI_SPACE_XL          32

/* ---------- 尺寸 ---------- */
#define UI_SCREEN_W          1024
#define UI_SCREEN_H          600
#define UI_HEADER_H          64
#define UI_NAV_H             68
#define UI_CONTENT_H         (UI_SCREEN_H - UI_HEADER_H - UI_NAV_H)   /* 468 */

/* 页面统一留白：内容区 1024×468，减去四周 24 → 页面可用 976×420 */
#define UI_PAGE_PAD          24
#define UI_PAGE_W            (UI_SCREEN_W - UI_PAGE_PAD * 2)          /* 976 */
#define UI_PAGE_H            (UI_CONTENT_H - UI_PAGE_PAD * 2)         /* 420 */

/* ---------- 圆角 ---------- */
#define UI_RADIUS_XS         8
#define UI_RADIUS_S          10
#define UI_RADIUS            14
#define UI_RADIUS_M          18
#define UI_RADIUS_L          22
#define UI_RADIUS_XL         26

/* ---------- 行高 / 栏宽 ---------- */
#define UI_ROW_H             56
#define UI_ROW_L             64
#define UI_NAV_ITEM_W        200

/* 设置页双栏：左侧分类栏 + 右侧详情卡片（272 + 16 + 688 = 976） */
#define UI_RAIL_W            272
#define UI_RAIL_ITEM_H       54
#define UI_DETAIL_W          688
#define UI_PANEL_PAD         24

/* ---------- 字体 ---------- */
/* 中文三档来自 tools/fonts（deng.ttf 子集，含 ASCII），图标用 LVGL 内置 Montserrat 的符号 */
LV_FONT_DECLARE(font_zh_16);
LV_FONT_DECLARE(font_zh_20);
LV_FONT_DECLARE(font_zh_28);
LV_FONT_DECLARE(font_net_30);

#define UI_FONT_SMALL        (&font_zh_16)
#define UI_FONT_BODY         (&font_zh_20)
#define UI_FONT_TITLE        (&font_zh_28)
/* Material Design Icons 子集：wifi-strength-1..4 + ethernet，24px 视觉大小 */
#define UI_FONT_NET          (&font_net_30)
/* 图标/符号必须用内置 Montserrat：中文字库是子集，没有 LV_SYMBOL_* 字形，
 * 用中文字体画符号会变成方框。 */
#define UI_FONT_ICON         (&lv_font_montserrat_20)
#define UI_FONT_ICON_SMALL   (&lv_font_montserrat_14)
