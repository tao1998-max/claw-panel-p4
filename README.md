# P4 HMI Template — JC1060P470C_I_W_Y

ESP32-P4 + ESP32-C6，7 寸 1024×600 IPS 电容触摸屏的**基础模板工程**。
把厂商散落在各个 demo 里的硬件初始化集中到一套可复用的代码里，上电即可看到一块带状态面板的屏幕。

## 已接入的硬件

| 模块 | 器件 / 接口 | 引脚 | 实现 |
|---|---|---|---|
| 显示 | JD9165 MIPI-DSI 1024×600 | DSI 2-lane，背光 GPIO23 | BSP |
| 触摸 | GT911（I2C） | SDA7 / SCL8 | BSP |
| 音频 | ES8311 + 功放 | MCLK13/BCLK12/WS10/DOUT9/DIN48/PA11 | BSP |
| SD 卡 | SDMMC 4-bit | CLK43/CMD44/D0-D3 39-42 | BSP |
| 摄像头 | **OV02C10** MIPI-CSI 1920×1080 RAW10 → ISP → RGB565 | DSI 2-lane，SCCB 复用 SDA7/SCL8 | esp_video 2.5.0 + 本地 esp_cam_sensor 2.1.0 |
| RS485 | UART1 | TX26 / RX27 | 本工程 |
| 以太网 | IP101 RMII | MDC31/MDIO52/RST51/CLK50 | 本工程 |
| Wi-Fi / BLE | ESP32-C6 经 SDIO | CLK18/CMD19/D0-D3 14-17/RST54 | 本工程，默认关闭 |

> 本板 P4 为 **rev < 3.0**，`sdkconfig.defaults` 里已设 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`，不要改掉。

## 目录结构

```
p4_template/
├── CMakeLists.txt               只引入 local_components 下的 P4 BSP + esp_cam_sensor
├── partitions.csv               16MB：双 5.9375M OTA app + 4M storage(spiffs)
├── sdkconfig.defaults           芯片 / PSRAM / BSP / LVGL / 字体开关
├── local_components/            ★随工程分发的本地组件
│   ├── espressif__esp32_p4_function_ev_board/
│   └── espressif__esp_cam_sensor/  厂商 2.1.0，含 OV02C10 驱动
├── tools/fonts/                 ← 字库生成工具（改文案必跑）
│   ├── collect_chars.py         扫 i18n.c 生成 charset.txt
│   ├── gen_fonts.ps1            调 lv_font_conv 出 16/20/28 三档
│   ├── gen_net_font.ps1         从 MDI TTF 生成网络图标 font_net_30.c
│   ├── charset.txt              当前字库覆盖的字符集
│   └── mdi/                     Material Design Icons TTF + 许可证
│       ├── materialdesignicons-webfont.ttf
│       └── LICENSE
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
    │   ├── hw_audio.c           ES8311 播放/录音
    │   ├── hw_storage.c         SD 卡挂载与容量
    │   ├── hw_rs485.c           RS485 收发（默认回环）
    │   ├── hw_eth.c             IP101 以太网 + DHCP
    │   ├── hw_camera.c          MIPI-CSI 自检取帧
    │   └── hw_wifi.c            C6 Wi-Fi：连接 + 扫描 + 断开 + SNTP 校时
    ├── ui/                      界面层
    │   ├── ui.c,h               对外只有 ui_init() / ui_rebuild()
    │   ├── ui_theme.h           颜色/间距/尺寸/字体令牌
    │   ├── ui_page.h            页面描述结构 ui_page_t
    │   ├── ui_shell.c,h         整屏 Aurora 背景 + 顶栏 + 内容区 + 底栏 + 息屏
    │   ├── ui_widgets.c,h       卡片/滑条/分段/菜单行/确认弹层
    │   ├── pages/               业务页
    │   │   ├── page_home.c      占位页（暂时留空，仅一行提示）
    │   │   ├── page_sysinfo.c   设备状态（KPI + 模块网格）
    │   │   ├── page_history.c   历史曲线（日期时间轴 + 游标/缩放 + 刻度平移 + 区段查找）
    │   │   └── page_settings.c  设置（分类栏 + 详情卡 + Wi-Fi）
    │   └── fonts/               font_zh_{16,20,28}.c + font_net_30.c（自生成，勿手改）
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
│    ⌂ 主页         ▤ 设备状态        ▥ 历史曲线        ⚙ 设置   │ 底栏 68
└──────────────────────────────────────────────────────────────┘
```

- 深色「Aurora console」方案：整屏是一张运行时生成的 1024×600 深蓝渐变（右上靛蓝/左下紫色眩光），用 Floyd–Steinberg 误差扩散把 RGB565 的量化误差摊开，渐变肉眼连续且没有规则颗粒；顶栏、内容区、底栏透明，大面板用 84% 深色纯色（不再用 420px 渐变，避免色阶断层）。
- 顶栏 64 px：左侧品牌标 + 页面标题，右侧 20px 运行时长与同一套 Material Design Icons 网络图标（`wifi-strength-1..4` / `ethernet`，可同时显示，按 RSSI 切换 Wi-Fi 级别，离线灰，每 1 s 刷新，不显示文字）。
- 底栏 68 px：选中项 = 顶部 3 px 强调条 + 半透明渐变胶囊 + 亮色图标/文字；强调条在 tab 间平滑滑动，胶囊底淡入淡出。
- 页面首次进入时创建，之后切页只做 hide/show；语言切换才整体销毁重建。开 `CONFIG_TEMPLATE_UI_SELFTEST=y` 跑过 200+ 次切换，三个页面的 heap 数值每轮完全一致（无泄漏）。
- 四个页面共用同一套设计令牌（`ui_theme.h`）和控件（`ui_widgets.c`）：
  - 主页：暂时留空，只保留一行「业务页面在 ui/pages/ 下添加」的提示；
  - 设备状态：四张 KPI 卡（带进度条）+ 八模块状态网格；
  - 历史曲线：单窗口 XY 曲线，下拉框切换温度 / 压力；X 轴显示日期时间，窗口长度可点胶囊中间数值输入（时/分/秒，不设实际最大值）或用 +/- / 双指捏合缩放；显示区内单指只做游标读数，底部 X 刻度左右拖平移时间、左侧 Y 刻度上下拖平移数值；右上角“区段查找”弹窗直接在日历上点起点/终点（靛蓝/紫罗兰两种颜色），再选起止时分，确认后曲线跳到该区段；
  - 设置：272 px 分类栏 + 688 px 详情卡，详情按分类切换并从下方轻轻托起。
- 双指缩放：`sdkconfig.defaults` 已开启 `CONFIG_LV_USE_FLOAT` 和 `CONFIG_LV_USE_GESTURE_RECOGNITION`；GT911 的 5 点触摸由 `esp_lvgl_port` 送进 LVGL 手势识别。历史页把 pinch 阈值调到 1.06 / 0.94，并把旋转、双指平移阈值调高，避免它们抢在 pinch 前面重置手势。
- 时间：板子没有 RTC，历史页用固件编译时间兜底；Wi-Fi 拿到 IP 后启动 SNTP（`cn.pool.ntp.org`），时区固定为 `CST-8`，有网时显示真实日期时间。
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

- 字库由 `deng.ttf`（等线）子集化生成，只含 `charset.txt` 里的字符 + ASCII；当前 charset 170 个字符，三档 `.c` 源文件合计约 **810 KB**。
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
| 恢复出厂设置 | — | 擦 NVS 并重启 |

## 上电自检（实机验证）

固件烧到本板后（P4 rev v1.3 / 32MB PSRAM / 16MB flash）实测启动日志：

| 模块 | 结果 | 日志证据 |
|---|---|---|
| 显示 | OK | `1024x600 JD9165`，`Display initialized`，背光 80% |
| 触摸 | OK | `GT911 TouchPad_ID:0x39,0x31,0x31` |
| 音频 | OK | `ES8311 Work in Slave mode` ×2（播放+录音），44.1kHz/16bit |
| SD 卡 | FAIL（未插卡） | `sdmmc_init_ocr ... 0x107` → `ESP_ERR_TIMEOUT` |
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
idf.py -p COM27 flash monitor
```

构建产物：`p4_template.bin` 约 **1.68 MB**（`0x1AE960`），5.9375 MB OTA app 分区剩余约 72%；
全量编译约 6 分钟（ccache 命中时增量编译十几秒）。

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
| storage (spiffs) | `0xC00000` | 4 MB | `0x1000000` |

双 OTA 槽已经就位，`otadata` 初始指向 `ota_0`。当前应用还没有接 HTTP/本地 OTA
下载与升级界面，后续可以基于这组分区直接调用 `esp_ota_*`；`storage` 固定 4 MB，
代码目前未挂载 SPIFFS，留作以后存日志/配置/升级包。两个 app 槽各 5.9375 MB，
是为了同时满足 4 MB storage 和用满 16 MB。

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

只打包源码、`local_components/`、`managed_components/`、`sdkconfig.defaults`；对方用
IDF 5.5.x 重新 `idf.py build` 即可。`dependencies.lock` 里的本地组件路径是当前机器的
绝对路径，不随包分发；首次构建会按工程内的 `local_components/` 重新生成。中文字库的
`.c` 已生成，普通构建不需要 Node / lv_font_conv；只有改 `i18n.c` 文案时才需要跑
`tools/fonts/`。

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
