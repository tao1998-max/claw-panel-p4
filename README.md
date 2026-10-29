# P4 HMI Template — JC1060P470C_I_W_Y

ESP32-P4 + ESP32-C6，7 寸 1024×600 IPS 电容触摸屏的**基础模板工程**。
把厂商散落在各个 demo 里的硬件初始化集中到一套可复用的代码里，上电即可看到一块带状态面板的屏幕。

## 已接入的硬件

| 模块 | 器件 / 接口 | 引脚 | 实现 |
|---|---|---|---|
| 显示 | JD9165 MIPI-DSI 1024×600 | DSI 2-lane，背光 GPIO23 | BSP |
| 触摸 | GT911（I2C） | SDA7 / SCL8 | BSP |
| 音频 | ES8311 + 功放 | MCLK13/BCLK12/WS10/DOUT9/DIN48/PA11 | BSP；小智模式 16 kHz 单声道双工 |
| SD 卡 | SDMMC 4-bit | CLK43/CMD44/D0-D3 39-42 | BSP |
| RTC | EPSON RX8025T-UC（I2C 0x32） | SDA7 / SCL8 | 本工程 `hw_rtc.c`；掉电保时，启动播种系统时间 |
| 摄像头 | **OV02C10** MIPI-CSI 1920×1080 RAW10 → ISP → RGB565 | DSI 2-lane，SCCB 复用 SDA7/SCL8 | esp_video 2.5.0 + 本地 esp_cam_sensor 2.1.0 |
| RS485 | UART1 | TX26 / RX27 | 本工程 |
| 以太网 | IP101 RMII | MDC31/MDIO52/RST51/CLK50 | 本工程 |
| Wi-Fi / BLE | ESP32-C6 经 SDIO | CLK18/CMD19/D0-D3 14-17/RST54 | 本工程，默认关闭 |

> 本板 P4 为 **rev < 3.0**，`sdkconfig.defaults` 里已设 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`，不要改掉。
>
> **Flash 写与 DPI**：默认 P4 写 SPI1 flash 会关闭 cache，DPI 从 PSRAM 取帧时会整屏闪。
> 本工程启用官方 `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`（`.text/.rodata` 搬到 PSRAM 执行），
> SPI1 写走 `SPI_FLASH_CACHE_NO_DISABLE` 路径，亮屏写 FAT/NVS 不再闪屏。代价是 PSRAM
> 多占用约 3.7MB、启动稍慢。
>
> **内部 RAM**：LVGL 的 draw/rotate 缓冲和 LVGL 对象都放到 PSRAM；Wi-Fi/LWIP、
> TLS 缓冲同样优先 PSRAM。启动 30s 后内部堆实测 **free 142 KB / largest 53 KB /
> min 100 KB**（优化前约 52/31/43 KB），PSRAM 仍有约 22.5 MB 空闲。

## 目录结构

```
p4_template/
├── CMakeLists.txt               只引入 local_components 下的 P4 BSP + esp_cam_sensor
├── partitions.csv               16MB：双 5.9375M OTA app + 4M FAT（/fatfs/ai + /fatfs/claw）
├── components/                  ★ESP-Claw 裁剪组件 + claw_port 适配层 + Lua 网络模块
│                                （lua_net_common / lua_module_net / mqtt / websocket）
├── sdkconfig.defaults           芯片 / PSRAM / BSP / LVGL / 字体开关
├── local_components/            ★随工程分发的本地组件
│   ├── espressif__esp32_p4_function_ev_board/
│   └── espressif__esp_cam_sensor/  厂商 2.1.0，含 OV02C10 驱动
├── assets/                      ★小智 AI 资源（随工程提交）
│   ├── ai_src/                  字体源文件 + 归属说明
│   └── ai/                      AI 资源：聊天字体 + srmodels.bin（打包进 claw FAT 镜像）
├── tools/fonts/                 ← 字库生成工具（改文案必跑）
│   ├── collect_chars.py         扫 i18n.c 生成 charset.txt
│   ├── gen_fonts.ps1            调 lv_font_conv 出 16/20/28 三档
│   ├── gen_net_font.ps1         从 MDI TTF 生成网络图标 font_net_30.c
│   ├── gen_robot_font.ps1       从 MDI TTF 生成机器人图标 font_robot_30.c
│   ├── charset.txt              当前字库覆盖的字符集
│   └── mdi/                     Material Design Icons TTF + 许可证
│       ├── materialdesignicons-webfont.ttf
│       └── LICENSE
├── tools/ai_assets/             build_assets.py：打包字体 + 唤醒词模型
├── skills/panel-app-design/     ★外部 AI 的面板 App 设计 Skill（FAT/SD 包 + 素材后备 + 网络）
└── main/
    ├── app/                     启动编排：NVS → settings → 显示 → UI → 外设
    │   ├── app_main.c
    │   ├── app.c
    │   └── app.h
    ├── core/                    ★与界面无关的系统服务
    │   ├── settings.c,h         NVS 参数（亮度/音量/语言/息屏）+ 硬件副作用
    │   └── i18n.c,h             中英文案表（所有界面文案的唯一来源）
    ├── hal/                     硬件抽象层
    │   ├── hw.h                 引脚表、模块状态、统一 API
    │   ├── hw_status.c          g_hw 唯一定义与模块名称
    │   ├── hw_display.c         显示 + 触摸 + 背光/息屏
    │   ├── hw_audio.c           ES8311 播放/录音（AI 开启时转发 16 kHz）
    │   ├── hw_storage.c         SD 卡挂载与容量
    │   ├── hw_rtc.c             RX8025T-UC RTC 读写/系统时间同步
    │   ├── hw_rs485.c           RS485 收发（默认回环）
    │   ├── hw_eth.c             IP101 以太网 + DHCP
    │   ├── hw_camera.c          MIPI-CSI 自检取帧
    │   └── hw_wifi.c            C6 Wi-Fi：连接 + 扫描 + 断开 + SNTP 校时
    ├── ai/                      ★小智 AI（可剪裁）
    │   ├── ai_app.cc            半双工状态机 / WebSocket 会话
    │   ├── ai_ota.cc            官方激活/check-version，不含固件升级
    │   ├── ai_audio.cc          16 kHz ES8311 双工
    │   ├── ai_ui.cc             悬浮机器人 + 聊天对话框
    │   ├── ai_assets.c          FAT 字体/唤醒词模型
    │   ├── ai_settings.c        NVS 命名空间 ai
    │   ├── ai_net.cc            EspNetwork / IP 事件 / SNTP
    │   ├── ai_system_info.c     P4 基片 MAC / UUID / OTA JSON
    │   └── vendor/              从 XiaoZhi v2.2.6 裁剪的协议/音频源码
    ├── ui/                      界面层
    │   ├── ui.c,h               对外只有 ui_init() / ui_rebuild()
    │   ├── ui_theme.h           颜色/间距/尺寸/字体令牌
    │   ├── ui_page.h            页面描述结构 ui_page_t
    │   ├── ui_shell.c,h         整屏 Aurora 背景 + 顶栏 + 内容区 + 底栏 + 息屏
    │   ├── ui_widgets.c,h       卡片/滑条/分段/菜单行/确认弹层
    │   ├── pages/               业务页
    │   │   ├── page_home.c      应用主页（双列卡片启动器，FAT/SD 应用）
    │   │   ├── page_sysinfo.c   设备状态（KPI + 模块网格）
    │   │   ├── page_history.c   历史曲线（日期时间轴 + 游标/缩放 + 刻度平移 + 区段查找）
    │   │   ├── page_claw.c      Claw 自动化（任务 / 脚本 / 设置）
    │   │   └── page_settings.c  设置（分类栏 + 详情卡 + Wi-Fi + AI 助手）
    │   ├── claw/                ★Claw 与主程序的桥接
    │   │   ├── claw_mcp_bridge.cc  self.claw.* MCP 工具
    │   │   └── claw_panel_hooks.c  claw.panel.* -> 设置/UI
    │   └── fonts/               font_zh_{16,20,28}.c + font_net_30.c + font_robot_30.c（自生成，勿手改）
    └── Kconfig.projbuild        menuconfig 开关
```

`managed_components/` 也在工程目录内，是 Component Manager 解析出的 registry 依赖；
分发时连同 `local_components/`、`managed_components/` 一起打包即可，不再依赖工程外的
`common_components/`。

## 界面框架

```
┌──────────────────────────────────────────────────────────────┐
│ [P4] 设置                                (网络图标) 运行 00:12:35 │ 顶栏 64
├──────────────────────────────────────────────────────────────┤
│                        内容区 1024 × 468                     │
├──────────────────────────────────────────────────────────────┤
│  ⌂ 主页(应用)   ▤ 设备状态   ▥ 历史曲线   ✦ Claw   ⚙ 设置  │ 底栏 68
└──────────────────────────────────────────────────────────────┘
```

- 深色「Aurora console」方案：整屏是一张运行时生成的 1024×600 深蓝渐变（右上靛蓝/左下紫色眩光），用 Floyd–Steinberg 误差扩散把 RGB565 的量化误差摊开，渐变肉眼连续且没有规则颗粒；顶栏、内容区、底栏透明，大面板用 84% 深色纯色（不再用 420px 渐变，避免色阶断层）。
- 顶栏 64 px：左侧品牌标 + 页面标题，右侧 20px 运行时长与同一套 Material Design Icons 网络图标（`wifi-strength-1..4` / `ethernet`，可同时显示，按 RSSI 切换 Wi-Fi 级别，离线灰，每 1 s 刷新，不显示文字）。
- 底栏 68 px：选中项 = 顶部 3 px 强调条 + 半透明渐变胶囊 + 亮色图标/文字；强调条在 tab 间平滑滑动，胶囊底淡入淡出。
- 页面首次进入时创建，之后切页只做 hide/show；语言切换才整体销毁重建。开 `CONFIG_TEMPLATE_UI_SELFTEST=y` 跑过 200+ 次切换，三个页面的 heap 数值每轮完全一致（无泄漏）。
- 小智 AI：所有页面右下角悬浮机器人（底栏上方，自绘面罩/眼睛/嘴巴，不依赖图标字体），点击/唤醒词打开 460×370 悬浮气泡（贴着机器人、带小尾巴，点气泡外任意位置关闭）；气泡有状态 chip、消息气泡、静音、结束对话；激活时自动弹层显示 6 位码并点亮屏幕。语音唤醒自动打开的气泡会在回答结束 5 秒后自动收起，但**不结束会话**；若气泡是自动收起的，之后有新的用户语音/小智回答（服务端下发的 STT/TTS 文本）会自动重新弹出；本工程没有网络音乐/歌词播放器。用户手动关闭（点气泡外、关闭按钮或再点机器人）后，会话继续，但新信息只在机器人上方显示 320×54 小气泡，文字超宽时横向循环滚动，8 秒无新信息自动消失；点小气泡可重新打开完整对话。点机器人手动打开的气泡不会自动收。会话结束仍按原来的服务端/超时逻辑。半双工：TTS 播放时麦克风上行暂停，保留唤醒词/点击打断；唤醒/激活/成功/错误使用 XiaoZhi 原版 OGG 提示音。
- 所有页面共用同一套设计令牌（`ui_theme.h`）和控件（`ui_widgets.c`）：
  - 主页：应用列表（统一使用可写的 `/fatfs/claw/data/apps`），注册表变化时刷新，点“启动”以异步 Lua job 运行；切换应用到后台时自动恢复主页；
  - 设备状态：四张 KPI 卡（带进度条）+ 八模块状态网格 + FAT 用量；
  - 历史曲线：单窗口 XY 曲线，下拉框切换温度 / 压力；X 轴显示日期时间，窗口长度可点胶囊中间数值输入（时/分/秒，不设实际最大值）或用 +/- / 双指捏合缩放；显示区内单指只做游标读数，底部 X 刻度左右拖平移时间、左侧 Y 刻度上下拖平移数值；右上角“区段查找”弹窗直接在日历上点起点/终点（靛蓝/紫罗兰两种颜色），再选起止时分，确认后曲线跳到该区段；
  - 设置：272 px 分类栏 + 688 px 详情卡，详情按分类切换并从下方轻轻托起；系统分类顶部有「时间设置」，可调年/月/日/时/分并写回 RX8025T。
- 双指缩放：`sdkconfig.defaults` 已开启 `CONFIG_LV_USE_FLOAT` 和 `CONFIG_LV_USE_GESTURE_RECOGNITION`；GT911 的 5 点触摸由 `esp_lvgl_port` 送进 LVGL 手势识别。历史页把 pinch 阈值调到 1.06 / 0.94，并把旋转、双指平移阈值调高，避免它们抢在 pinch 前面重置手势。
- 时间：板载 RX8025T-UC 经 I²C 0x32 提供掉电保时；启动时优先用 RTC 播种系统时间，SNTP 校时后再回写 RTC。顶栏右侧显示日期时间；设置 → 系统 → 时间设置可手动校时。历史页在时间未初始化时用固件编译时间兜底；时区为 `CST-8`。
- 全屏背景图运行时生成在 PSRAM（1024×600 RGB565，约 1.2 MB）；`hw_display.c` 的 LVGL 任务栈已调到 16 KB，8 KB 会在页面切换绘制时触发 stack protection fault。
- 设计预览见 `docs/mockups/new_*.png`（`python docs/mockups/gen_redesign.py` 可重新生成，需要 Pillow）。

## 二次开发：新增一个业务页

1. 在 `main/core/i18n.h/.c` 加文案（中文 + 英文）。
2. 跑 `python tools/fonts/collect_chars.py` + `tools/fonts/gen_fonts.ps1` 重生成字库。
3. 新建 `main/ui/pages/page_xxx.c/.h`，导出一个 `const ui_page_t`：
   ```c
   const ui_page_t page_xxx = {
       .id = "xxx", .name = STR_NAV_XXX, .symbol = LV_SYMBOL_LIST,
       .create = xxx_create, .destroy = xxx_destroy,
   };
   ```
4. 加进 `ui_shell.c` 的 `s_pages[]`，底栏与切页自动生效。
5. `main/CMakeLists.txt` 的 SRCS 加上新文件。

## 中文与字体

- 字库由 `deng.ttf`（等线）子集化生成，只含 `charset.txt` 里的字符 + ASCII；当前 charset 327 个字符，三档 `.c` 源文件合计约 **1.40 MB**。
- 网络图标用 `tools/fonts/mdi/materialdesignicons-webfont.ttf` 子集生成 `font_net_30.c`（`wifi-strength-1..4` + `ethernet`，Material Design Icons / Pictogrammers，Apache-2.0），重新生成：`tools/fonts/gen_net_font.ps1`。其余符号仍用 LVGL 内置 Montserrat 的 `LV_SYMBOL_*`；**符号必须配 `UI_FONT_ICON*` 或 `UI_FONT_NET`**。
- 改了 `i18n.c` 却没重跑字库 = 新汉字变方框。固定两步：
  ```powershell
  python tools/fonts/collect_chars.py
  tools/fonts/gen_fonts.ps1     # 换字形：加 -CjkFont C:\Windows\Fonts\simhei.ttf
  ```
- `gen_fonts.ps1` 必须保存为 **UTF-8 with BOM**：Windows PowerShell 5.1 会按 ANSI 解析无 BOM 的脚本，中文注释可能把换行吞掉，导致变量变成空值。
- charset 用 `File.ReadAllText(..., UTF8)` 读取；`collect_chars.py` 也把 stdout 改成 UTF-8，避免控制台 GBK 报 `UnicodeEncodeError`。

## 设置项（掉电保存）

NVS namespace `sys`，键 `bright / vol / lang / soff`。

| 设置 | 范围 | 生效方式 |
|---|---|---|
| 显示亮度 | 10–100 % | 拖动即时生效，松手落盘 |
| 息屏时间 | 常亮 / 30 s / 1 min / 5 min | 到点关背光，触摸只亮屏不误触 |
| 扬声器音量 | 0–100 % | 拖动即时生效，松手落盘 |
| 语言 | 中文 / English | 整屏重建 |
| Wi-Fi | 扫描 → 选 AP → 输入密码 → 连接 / 断开 | 立即连接，凭据由 esp_wifi 存 NVS |
| 小智 AI | 启用 / 服务地址 / 唤醒词 / 麦克风静音 / 重新激活 | 配置存 NVS `ai`，启用/地址/唤醒词重启生效；静音即时 |
| 恢复出厂设置 | — | 擦 NVS（含小智激活/配置）并重启 |

> 小智 AI 的聊天字体、唤醒词模型在 4 MB `claw` FAT 的 `/fatfs/ai`，
> 由 `idf.py claw-flash` 写入；普通 `idf.py flash`/`app-flash` 都不会覆盖它，
> 也不会清空 `/fatfs/claw` 工作区。

## 上电自检（实机验证）

固件烧到本板后（P4 rev v1.3 / 32MB PSRAM / 16MB flash）实测启动日志：

| 模块 | 结果 | 日志证据 |
|---|---|---|
| 显示 | OK | `1024x600 JD9165`，`Display initialized`，背光 80% |
| 触摸 | OK | `GT911 TouchPad_ID:0x39,0x31,0x31` |
| 音频 | OK | `ES8311 Work in Slave mode`，AI 模式 16 kHz/16bit 双工；关闭 AI 时 44.1 kHz |
| SD 卡 | OK（当前 480 MB 卡） | `hw.sd: sdcard mounted at /sdcard (480 MB, 8 files)`；无卡时打印 `no card` 并继续启动 |
| RS485 | OK | `rs485 ready: TX=26 RX=27 @115200` |
| 以太网 | OK（未插网线） | `ip101 started (mdc=31 mdio=52 rst=51)`，等待 link |
| 摄像头 | OK | `ov02c10: Detected Camera sensor PID=0x5602`，`captured one frame: 1920x1080, 4147200 bytes` |
| Wi-Fi | OK | C6 经 SDIO 起来，`connected`，拿到 `192.168.1.179` |

整机启动到 `Returned from app_main()` 约 **8.9 s**（ETH PHY 复位 4 s、C6 SDIO 握手 1.5 s、摄像头自检约 0.4 s）。

### 摄像头踩坑记（三步，缺一不可）

本板模组是 **OV02C10**，`PID=0x5602`，走 MIPI-CSI 1-lane 1920×1080 RAW10，
必须经 ISP 转成 RGB565 才能取流。这三步是依次踩出来的：

1. **`esp_cam_sensor` 要有 ov02c10 驱动**
   registry 上的 2.0.1 里没有 ov02c10，探测会报
   `Camera sensor is not OV5647, PID=0x5602` → `failed to detect MIPI-CSI camera sensor`。
   解决：把厂商 `Demo_IDF/components/esp_cam_sensor`（2.1.0）拷到
   `local_components/espressif__esp_cam_sensor`，在 `EXTRA_COMPONENT_DIRS` 里注册。

2. **`esp_video` 必须 ≥ 2.5.0**
   2.0.1 的 ISP 管线在这套组合上跑不起来：`Stream=1` 之后约 56 ms 开始刷
   `ISP: fifo overflow`，中断里刷日志把 CPU0 打到 `Interrupt wdt timeout` 复位。
   官方 `video_lcd_display` 用的就是 2.5.0。
   注意 BSP 5.2.3 的 `idf_component.yml` 把 `esp_video` 钉在 `~2.0`，
   需要把本地 BSP 那份改成 `>=2.0`（BSP 源码其实没用到 esp_video）。

3. **`esp_video_init_sccb_config_t.freq` 必须显式给值**
   留 0 会让 `i2c_master_bus_add_device()` 报 `invalid scl frequency`，
   整个 `esp_video_init()` 直接失败。给 100 kHz（与官方例程一致）。

另外取流缓冲区要按 **RGB565 的 w×h×2** 自己分配（PSRAM，64 字节对齐）再以
`V4L2_MEMORY_USERPTR` 交给驱动。用 MMAP 让驱动按 CSI 原始格式分配会偏小，
同样是 `ISP: fifo overflow`。

最后一个坑：这个 V4L2 设备的 `select()/poll()` 只有**等到超时才**报告可读，
实测 3 s、5 s 两种超时都是刚好卡满才返回（帧其实 88 ms 就到了）。
所以 `hw_camera.c` 里取首帧用非阻塞 `DQBUF` + 10 ms 轮询，而不是 `select()`。

### 其他两个实测提示

1. **Wi-Fi 版本不匹配但不影响使用**
   `W transport: Version mismatch: Host [2.11.0] > Co-proc [2.3.0]`。
   板上 C6 从机固件是 `JC-C6-slave_v2.3.2`，比 `esp_hosted 2.11.x` 期望的旧一档。
   实测仍能连上 AP 并 `got ip`，只是长时间跑可能出现 RPC 超时；要根治就按厂商说明单独给 C6 烧从机固件。

2. **`ledc: GPIO 23 is not usable` 是 BSP 的重复初始化**
   `bsp_display_start_with_config()` 和 `bsp_display_new_with_handles()` 各自调了一次
   `bsp_display_brightness_init()`，第二次 LEDC 申请已被自己占用的 GPIO23 就告警了。
   背光实际工作正常（日志里 100% → 80% 生效），不用管。

## 编译与烧录

```bash
# 激活环境（EIM 安装的 IDF 5.5.5）
C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1

cd E:\Demo_IDF\ESP-IDF_5.5.4\JC1060P470C_I_W_Y_New_Panel\p4_template
idf.py build
idf.py -p COM27 flash           # 分区表 + app，不含 AI 资源
idf.py -p COM27 claw-flash      # 首次迁移/整片擦除后：写 4 MB AI 资源 FAT
idf.py -p COM27 monitor
```

构建产物（小智开启）：`p4_template.bin` 约 **4.91 MB**（`0x4B0E80`），5.9375 MB OTA app 分区剩余 21%；
FAT 里的 AI 资源约 1.55 MB。关闭 `CONFIG_TEMPLATE_ENABLE_XIAOZHI` 后回落到约 1.76 MB / 72%。
全量编译约 6–8 分钟（ccache 命中时增量编译十几秒）。

> 如果用裸 cmd/Git Bash 而不是 EIM 的 PowerShell profile 重新运行 CMake，需要先设置
> `ESP_IDF_VERSION=5.5`。否则 `esp_wifi_remote` 的 Kconfig 不会加载 C6 slave target，
> `sdkconfig` 会退回 `ESP_HOSTED_IDF_SLAVE_TARGET="invalid"` / SPI，C6 SDIO Wi-Fi 起不来。

### Flash 分区（16 MB）

| 区域 | 起始 | 大小 | 结束 |
| --- | ---: | ---: | ---: |
| bootloader | `0x002000` | 24 KB | `0x008000` |
| partition table | `0x008000` | 4 KB | `0x009000` |
| nvs | `0x009000` | 24 KB | `0x00F000` |
| phy_init | `0x00F000` | 4 KB | `0x010000` |
| otadata | `0x010000` | 8 KB | `0x012000` |
| ota_0 | `0x020000` | 5.9375 MB | `0x610000` |
| ota_1 | `0x610000` | 5.9375 MB | `0xC00000` |
| claw (fat) | `0xC00000` | 4 MB | `0x1000000` |

双 OTA 槽已经就位，`otadata` 初始指向 `ota_0`。当前应用还没有接 HTTP/本地 OTA
下载与升级界面；小智只做 check-version/激活，**不会**执行服务端下发的固件升级。
`claw` 是唯一的 4 MB FAT（带 wear leveling），挂载 `/fatfs`：`/fatfs/ai` 放聊天字体、
唤醒词模型和提示音（约 1.55 MB），`/fatfs/claw` 供 ESP-Claw 保存脚本、计划、应用和
Agent 数据。两个 app 槽各 5.9375 MB，用满 16 MB。`esp-sr` 的组件描述仍声明依赖
`spiffs` 组件，但工程不再注册/挂载任何 SPIFFS 分区，`sdkconfig` 里的 SPIFFS 选项
只是该依赖的默认值。

> AI 资源单独生成 `build/claw.bin`，首次迁移或整片擦除后执行一次
> `idf.py claw-flash`。普通 `idf.py flash` 只写分区表 + app，不会碰该分区。

### 从旧 SPIFFS + 1 MB FAT 布局迁移

旧固件在 `0xC00000` 有 3 MB SPIFFS（`/spiffs` AI 资源）、`0xF00000` 有 1 MB FAT
（`/fatfs/claw`）。换成单一 4 MB FAT 会覆盖这两段，升级前先把工作区备份到电脑：

1. 旧固件 Web 文件管理 → 根目录 → 下载 `data` 文件夹（`.tar`），备份包含
   `apps`、`scripts`、`schedules`、`agent`、`skills`、`skills_state`；`system` 不用备份。
2. `idf.py flash` 写新分区表 + app，再 `idf.py claw-flash` 写 4 MB AI 资源 FAT。
3. 首次启动后，Web 文件管理 → `/fatfs/claw` → 上传备份的 `data` 目录，保留相对路径。
4. 重启一次，让应用注册表和会话目录重新加载。

`router_rules` 可以一起恢复：启动时若规则文件缺失或只有 `[]`，固件会自动写回官方
IM 恢复规则。`idf.py app-flash` 只更新 app，不会清空 `/fatfs/claw`。

### 分发与目录自包含

工程构建只依赖三处：

1. 本工程目录；
2. `local_components/`（BSP + esp_cam_sensor，已随工程提供）；
3. `managed_components/`（registry 依赖，已解析到工程内）。

不再引用工程外的 `../common_components/`。分发前**只删除 `build/` 目录，不要用
`idf.py fullclean`**：IDF 5.5 的 fullclean 会连 `managed_components/` 一起清掉，下一台机器
就需要联网重新拉取。手动删除即可：

```powershell
Remove-Item -Recurse -Force build
```

只打包源码、`assets/`、`local_components/`、`managed_components/`、`sdkconfig.defaults`；
对方用 IDF 5.5.x 重新 `idf.py build` 即可。`dependencies.lock` 里的本地组件路径是当前机器的
绝对路径，不随包分发；首次构建会按工程内的 `local_components/` 重新生成。中文字库的
`.c` 已生成，普通构建不需要 Node / lv_font_conv；只有改 `i18n.c` 文案时才需要跑
`tools/fonts/`。小智的字体/唤醒词资源已生成在 `assets/ai/`，不需要 Python；只有换模型/
字体时才跑 `tools/ai_assets/build_assets.py`。

⚠️ **工程必须放在纯英文路径**。中文路径会让 RISC-V gcc 收到乱码参数，报
`cc1.exe: fatal error: ... No such file or directory`。

## 打开摄像头 / Wi-Fi

```bash
idf.py menuconfig
# P4 Template → 启动 MIPI-CSI 摄像头 / 启动 Wi-Fi
```

- **摄像头**：本板模组是 OV02C10，接好即可用，上电会自检并抓一帧（`Stream=1` 后约 90 ms）。
  取流细节见上面「摄像头踩坑记」；真正把画面刷到屏上参考厂商 `video_lcd_display` 示例。
- **Wi-Fi**：P4 没有射频，必须靠板载 C6。要求 C6 上的从机固件与 `esp_hosted 2.11.x` 匹配，
  否则日志会刷 `Version mismatch ... RPC timeouts`。厂商提供的 `JC-C6-slave_v2.3.2.bin`
  需要通过 C6 的 UART 烧录口单独烧写。

## 小智 AI 语音助手（半双工）

### 开启与激活

```bash
idf.py menuconfig
# P4 Template → 启用小智 AI 语音助手
idf.py -p COM27 flash monitor
```

- 首次联网后固件会 POST 官方 OTA（默认 `https://api.tenclass.net/xiaozhi/ota/`），
  聊天面板会自动弹出 6 位激活码；到 <https://xiaozhi.me> 绑定后自动继续。
- 已激活的设备重启不会重复弹码。设置 → AI 助手 → 重新激活可清除绑定并重新走流程。
- 设备 ID 使用 **P4 基片 MAC**；与参考 XiaoZhi 固件使用 C6 STA MAC 不同，换固件需要重新激活。
- 服务地址可在设置里改为自建 OTA；传输使用 WebSocket（默认协议 v1，代码也支持服务端下发的 v2/v3 二进制帧），默认 16 kHz 上行 / 服务端可选 24 kHz 下行。

### 交互

- 点右下角机器人：打开对话；空闲时自动开始一次聆听。
- 免提：说“你好小智”唤醒；回答中再说“你好小智”或点机器人可打断。
- **半双工**：TTS 播放时麦克风上行暂停，不做 AEC；不会把扬声器声音回传给服务器。
- 音量沿用设置 → 声音；聊天面板的麦克风按钮只控制麦克风静音。
- AI 可通过 MCP 控制设备：`self.audio_speaker.set_volume`（0–100）、`self.screen.set_brightness`（0–100），`self.get_device_status` 查询当前音量/亮度/网络；语音说“音量调到 60”“屏幕调亮一点”即可。主页改成应用列表后，`claw.panel.demo(on)` 保留为兼容接口，不再有可见开关。
- 聊天面板关闭不结束会话；点「结束对话」才停止并回到待机。

## ESP-Claw 本地自动化（Lua / 后台任务 / 定时）

小智云端负责听懂和生成脚本，ESP-Claw 负责在本地安全执行。底栏第 4 项 **Claw** 页面按 ESP-Claw Web 管理端的顶层导航组织：左侧可滚动栏（系统状态 / 系统设置 / 记忆管理 / 在线聊天 / 定时任务 / 能力管理 / Lua 模块管理 / 文件管理）、顶部状态 chips、页面标题/描述 + 内容列表。

### 示例应用：21 点

`/fatfs/claw/data/apps/blackjack` 是内置示例（运行时应用，和所有应用一样只放在 `data/apps`）：

- 1024×600 赌桌、金色边框、中文字体（`assets/game_cn.ttf`，Noto Serif SC 子集，25 KB）；
- 发牌动画、庄家暗牌、发牌/要牌/停牌/下一局、胜负/平局统计；
- 字库只含游戏用到的 138 个字符（`assets/chars_cn.txt`）；改文案后要用
  `fontTools` 重新生成子集，否则新字会显示成方框。完整 Noto Serif SC 约
  17 MB，放不进 4 MB 的 Claw FAT 分区；
- 依赖固件的 `CONFIG_LV_USE_TINY_TTF=y` 和 `lua_module_lvgl` 的 DATA 根；
- 源码同时在工程 `apps/blackjack/`，上传后主页会显示「21点」。

### 面板 App 设计 Skill（外部 AI）

`skills/panel-app-design/` 是给 Claude Code / Claude.ai / 其他 AI 平台使用的
Claude Agent Skill，不是设备侧小爪 Skill。它教外部 AI 生成 1024×600 的 LVGL
Lua App，包括：

- 包契约、`launcher.json`、64 KiB 脚本上限、上传后重启；**FAT 或 SD 整包部署**
  （`/fatfs/claw/data/apps/<id>/` 或 `/sdcard/apps/<id>/`，SD 包原地启动）；
- 中文字体子集（`fontTools`）与缺字校验；TTF 现在也可随 SD 包放在 SD 上；
- 海报式分层：全屏 JPEG、透明 ARGB 精灵、LVGL UI、局部 EAF；
- **SD 运行期素材后备**：高清素材放 `/sdcard/panel-assets/<app_id>/`，包内保留
  同尺寸低清 fallback；无卡时 App 仍能启动；
- **网络参考**：`references/networking.md`（TCP/UDP/DNS/HTTP、MQTT、WebSocket、
  `next_event` 循环、socket 预算、Lua HTTP 路由）；
- 校验/上传脚本、3 个 eval 和三个示例（`countdown`、`sd_fallback`、`mqtt_panel`）。

设备侧新增只读 `/api/storage`、受限 `/api/sd-assets`（只允许
`/sdcard/panel-assets`）和整卡管理的 `/api/sd-files`（供 SD 应用部署）；没有这些
端点的旧固件仍可手动拷卡。详见
[`skills/panel-app-design/README.md`](skills/panel-app-design/README.md)。

### Web 管理端

- 设备联网后访问 `http://<设备IP>/`（当前示例 `http://192.168.1.179/`）；
- 官方 ESP-Claw SolidJS 前端 + HTTP server 已内置，支持状态、配置（Wi-Fi/LLM/搜索/能力可见性）、能力列表、Lua 模块、文件浏览/上传/下载/删除、重启；
- `/api/storage` 返回 FAT/SD 容量与挂载状态；`/api/sd-assets*` 只在
  `/sdcard/panel-assets` 下读写面板素材，`/api/sd-files*` 管理整张卡（Web 文件页
  的“内部存储 / SD 卡”切换），未插卡返回 503；
- `/ws/webim` 会话接口保留，IM/微信登录页依赖官方外部服务，未配置时只显示错误；
- **没有登录鉴权**，只应在可信 LAN 使用；不要直接暴露到公网。SD 写接口同样
  只应在可信 LAN 使用。

- **系统状态**：Wi-Fi/IP、FAT 用量、工作区、Agent/Lua/任务/能力/Lua 模块数量，可滚动。
- **系统设置**：LLM 设置（本地 Agent 开关、接口、模型、Key、Lua 权限）与 IM 平台配置状态；基本显示/声音设置仍在主「设置」页，网络/搜索配置在 Web 管理端，避免重复。
- **记忆管理**：长期记忆、灵魂、身份、用户信息四个文件，点开只读预览。
- **在线聊天**：与本地 Agent 对话（与 Web 端共享会话/记忆）。
- **定时任务**：`cap_scheduler` 计划列表，支持手动触发、启停、删除，下次触发倒计时。
- **能力管理 / Lua 模块管理**：已注册的能力组与底层 Lua 模块列表。
- **文件管理**：只读浏览 `/fatfs/claw/data` 与 `/system`，进目录、点文件查看；不提供新建/删除/运行入口。
- 任务、定时、脚本、技能运行、测试定时等开发向入口已从面板移除；这些由 Agent 和 Web 管理端管理。

语音用法（需先在 Claw → 设置里打开开关）：

- “小智，让小爪运行一个脚本，把亮度调到 40%”
- “小智，让小爪在后台跑一个 30 秒的循环任务”
- “小智，小爪现在有哪些任务？” / “停掉那个任务”

安全边界：

- Lua 运行时为官方 `cap_lua`：标准库全开（脚本可用 `io/os/package/require`），设备操作优先用 `claw` 表；官方 skill 通过 `require("capability"|"json"|"arg_schema")` 取能力/JSON/参数校验；
- 同步最多 3 s、后台任务可停止；官方运行时没有应用层 Lua 内存上限；
- `claw.file.*` 读写锁定在 `/fatfs/claw`，写只允许 `data/`，单次写 16 KB、读 64 KB；标准库 `io` 不受此限制；
- AI 生成脚本默认**不允许**，面板手动开启；恢复出厂会清空工作区并关闭开关。

### Lua 网络能力（TCP / UDP / HTTP / MQTT / WebSocket）

固件默认注册三个网络模块（`GET /api/lua-modules` 可见，免 Kconfig 开关）：

| 模块 | 能力 |
| --- | --- |
| `net` | `tcp`（阻塞+超时、TLS/insecure/自定义 CA、`send_all/recv_exact/recv_line/wait_readable/local_addr/shutdown`）、`tcp.listen/accept`、`udp`（bind/广播/TTL/多播）、`resolve()`、`online()/local_ip()`、`http.request()`（状态码+响应头+正文、chunked、重定向、`save_path`） |
| `mqtt` | MQTT 3.1.1：连接/发布/订阅、遗嘱 LWT、keepalive、自动重连、`next_event()` 事件流 |
| `websocket` | `ws://`/`wss://`：文本/二进制、分片重组、ping、自动重连、`next_event()` 事件流 |

```lua
local net, mqtt = require("net"), require("mqtt")
local r = net.http.request({ url = "http://192.168.1.10:8080/status", timeout_ms = 5000 })
print(r.status, r.body)

local m = mqtt.new({ uri = "mqtt://192.168.1.10:1883", client_id = "panel-01" })
if m:connect(5000) then
  m:subscribe("panel/cmd", 1)
  while not claw.abort_requested() do
    local ev = m:next_event(50)
    if ev and ev.type == "message" then print(ev.topic, ev.payload) end
    lvgl.process_events(20)
  end
  m:close()
end
```

- **事件模型**：网络任务只把事件拷进队列，脚本用 `:next_event(ms)` 在 Lua 任务上取；不存在跨任务 Lua 回调。
- **可取消**：所有阻塞等待 50 ms 切片并检查停止标志（TLS 握手在独立任务上受超时约束）。
- **socket 预算**：单脚本 3 个 socket / 1 个 TLS；全局 4 / 2（`CONFIG_LWIP_MAX_SOCKETS=28`），超限返回 `socket budget exhausted`，保护 Web UI 与语音链路。
- **Lua HTTP 服务端**：`http_server` 的 `/lua/<app_id>/*`、`/api/lua/<app_id>/*` 已启用，由 App 的 `serve_forever()` 或非阻塞 `poll()` 分发（`CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER=y`）。
- `insecure=true` 需要 `CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`（`sdkconfig.defaults` 已开）；证书默认用内置 bundle。
- 限制：lwip 只返回单个 DNS 地址；IPv6 实际仅字面量可用（无 SLAAC）；无 HTTP keep-alive/MQTT 5。
- 模块文档随种子写入 `builtin_lua_modules`（种子版本 28）；设计与 API 细节见
  [`docs/claw.md`](docs/claw.md) 与
  [`skills/panel-app-design/references/networking.md`](skills/panel-app-design/references/networking.md)。


分区与工作区（16 MB 布局已调整）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| claw | 0xC00000 | 4 MB | FAT：`/ai` 小智资源 + `/claw` 脚本、计划、日志、应用、Agent 记忆 |

首次启动只会格式化**完全空白**的 `claw` 分区并写入示例脚本；已有 FAT 挂载失败时
拒绝格式化，避免丢工作区。`idf.py flash`/`app-flash` 都不会动它；只有
`idf.py claw-flash`（写 AI 资源镜像）或整片擦除会重建。详细设计、Lua API、MCP
工具表和安全说明见 [`docs/claw.md`](docs/claw.md)。

### 资源与剪裁

- 资源：`assets/ai/font_puhui_common_20_4.bin`（1.24 MB，XiaoZhi cbin 字体）+
  `assets/ai/srmodels.bin`（约 290 KB，`wn9_nihaoxiaozhi_tts`）；由
  `python tools/ai_assets/build_assets.py` 生成并随工程提交。
- 构建时 `assets/ai` 被 staging 到 `build/claw_fat_root/ai`，再由
  `fatfs_create_spiflash_image(claw ...)` 生成 4 MB `build/claw.bin`（不在普通 flash
  依赖里）：`idf.py claw-flash` 单独烧到 `0xC00000`，普通 `idf.py flash`/`app-flash`
  不更新资源、也不清空工作区。
- 总开关：`CONFIG_TEMPLATE_ENABLE_XIAOZHI`。关闭后不启动 AI、不显示机器人和设置分类，
  固件回到约 1.76 MB；`managed_components` 仍会下载/编译，但链接器会丢弃 AI 代码。
- 完整移除步骤见 `docs/xiaozhi.md`。字体/模型缺失时降级：仍可点机器人对话，聊天中文字体回退子集字库。
- 许可证：XiaoZhi MIT、78/esp-ml307 Apache-2.0、esp-sr / esp_audio_codec 的 Espressif
  许可、puhui 字体来源说明、MDI Apache-2.0，详见 `assets/ai/THIRD_PARTY_NOTICES.txt` 与
  `main/ai/vendor/LICENSE.xiaozhi`。

### 已知限制

- 无 AEC，不支持任意语音插话；只有唤醒词/点击能打断 TTS。
- C6 从机固件与 esp_hosted 2.11.7 版本不匹配时，长连接可能出现 RPC 超时；
  根治需按厂商说明刷 C6 从机固件（见上文 Wi-Fi 说明）。
- 设备端动态中文（聊天历史、会话标题、应用名等）使用 puhui 常用字库（约 5,377 个 CJK，
  含 ASCII）；emoji 和极生僻字仍可能显示方框。
- `CONFIG_TEMPLATE_XIAOZHI_AUTO_LISTEN` / `AUTO_OPEN_CHAT` 仅用于台架验证，正常固件保持关闭。

## 二次开发入口

```c
// 任何地方拿到模块状态
g_hw[HW_ETH].ok / g_hw[HW_ETH].detail

// 常用操作
hw_display_set_brightness(60);   // 0-100
hw_audio_set_volume(80);         // 0-100
hw_audio_beep();                 // 播放 1kHz 测试音
```

新增界面时注意：**LVGL 不是线程安全的**，任何 `lv_*` 调用都要包在
`bsp_display_lock(0) / bsp_display_unlock()` 之间；或者直接用 `lv_timer_create()`
（回调本身就在 LVGL 任务里执行，参考 `ui.c` 的 `refresh_timer`）。

## 排查

| 现象 | 原因 |
|---|---|
| 编译报 `esp_wifi.h: No such file or directory` | `main/CMakeLists.txt` 的 `REQUIRES` 少了 `esp_wifi`。P4 的头文件是 `esp_wifi_remote` 注入到 `esp_wifi` 组件的，光靠 BSP 传递不够 |
| 开流后狂刷 `ISP: fifo overflow` 并复位 | esp_video 被 BSP 钉在 2.0.x。2.0.1 的 ISP 管线不支持这套 OV02C10 组合，必须 ≥2.5.0 |
| `failed to detect MIPI-CSI camera sensor`，但日志里有 `PID=0x5602` | 缺 ov02c10 驱动（registry 的 esp_cam_sensor 2.0.1 没有），要用本地 2.1.0 |
| `sccb_new_i2c_io: failed to add device` / `invalid scl frequency` | `esp_video_init_sccb_config_t.freq` 没填 |
| 屏幕全黑但有日志 | 背光未开，或面板初始化失败，试改 `lane_bit_rate_mbps`（550/750/900） |
| 白屏 + 竖噪声带 | 面板版本不对（V2 屏刷了 V1 固件） |
| 编译报 LVGL 版本冲突 | `EXTRA_COMPONENT_DIRS` 被改回工程外的 `common_components` 了；应指向 `local_components/` |
| 编译报 `No such file or directory` 且路径含 `?` | 工程放在中文路径下 |
| 资源打包脚本崩溃 / `SHFileOperationW` | 某些工具会注入 `PYTHONPATH`，编译前 `set PYTHONPATH=` |
