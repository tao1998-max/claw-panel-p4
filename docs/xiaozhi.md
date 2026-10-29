# 小智 AI 集成说明

本工程的小智客户端基于本地 `xiaozhi-esp32-main` **v2.2.6** 的协议/音频代码裁剪，
适配 ESP-IDF 5.5.5。官方最新 v2.5.2 需要 IDF 6.0.1+，本工程不升级 IDF。

## 文件边界

| 路径 | 作用 | 剪裁时 |
|---|---|---|
| `main/ai/` | 状态机、OTA/激活、网络、音频、UI、设置 | 可整目录删除 |
| `main/ai/vendor/` | XiaoZhi 协议/音频源码（MIT，保留 LICENSE） | 随 `main/ai/` 删除 |
| `assets/ai_src/` | 字体源文件/归属说明 | 可删 |
| `assets/ai/` | 4 MB FAT 的 `/fatfs/ai` 内容（字体 + srmodels.bin + 提示音） | 可删 |
| `tools/ai_assets/build_assets.py` | 资源生成脚本 | 可删 |
| `main/ui/fonts/font_robot_30.c` | 机器人图标子集 | 可删 |
| `tools/fonts/gen_robot_font.ps1` | 机器人图标生成脚本 | 可删 |

集成点：

- `main/app/app.c`：`ai_early_init()`、`ai_start()`
- `main/hal/hw_audio.c`：AI 开启时转发到 `ai_audio_*`
- `main/hal/hw_time.c` / `hw_eth.c` / `hw_wifi.c`：共用 SNTP
- `main/ui/ui.c` / `ui_shell.c`：`ai_ui_init()`、`ai_ui_attach/detach()`、息屏/唤醒
- `main/ui/pages/page_settings.c`：第 5 个分类 `CAT_AI`
- `main/core/i18n.c`：AI 文案
- `main/Kconfig.projbuild`、`main/CMakeLists.txt`、`main/idf_component.yml`、`CMakeLists.txt`

## 编译裁剪

关闭功能：

```
CONFIG_TEMPLATE_ENABLE_XIAOZHI=n
```

效果：

- `ai_api_stub.c` 提供空 API；
- 不启动 AI 任务、不显示悬浮机器人和设置分类；
- `hw_audio.c` 回到 BSP 44.1 kHz 音频；
- 链接器丢弃未引用的 AI 代码，固件回到约 1.76 MB。

注意：IDF 组件 CMake 在 Kconfig 之前执行，不能按 `CONFIG_*` 条件删除 SRCS/REQUIRES；
所以关闭后 AI 源文件和依赖仍会参与编译/下载，只是不链接。要彻底移除，按下面步骤。

## 完整移除清单

1. 删除 `main/ai/`、`assets/ai/`、`assets/ai_src/`、`tools/ai_assets/`。
2. 删除 `main/ui/fonts/font_robot_30.c` 与 `tools/fonts/gen_robot_font.ps1`。
3. 从 `main/CMakeLists.txt` 删除 `ai/...`、`cbin_font.c`、`font_robot_30.c`、`hw_time.c`
   以及 `ai`/`vendor` 相关 INCLUDE_DIRS、`espressif__esp-sr`、`esp_audio_codec/effects`、
   `78__esp-ml307` 等 REQUIRE。
4. 从 `main/idf_component.yml` 删除 `espressif/esp-sr`、`esp_audio_codec`、
   `esp_audio_effects`、`78/esp-ml307`。
5. 从顶层 `CMakeLists.txt` 删除 `assets/ai` 的 staging 与 `fatfs_create_spiflash_image(claw ...)`
   （`claw` FAT 仍保留给 ESP-Claw 工作区）。
6. 从 `main/Kconfig.projbuild` 删除 `TEMPLATE_ENABLE_XIAOZHI` 段。
7. 从 `page_settings.c` 删除 `CAT_AI` 及 `build_ai` 相关代码；从 `ui.c/ui_shell.c` 删除
   AI UI 调用；从 `app.c` 删除 `ai_*` 调用；从 `hw_audio.c` 删除 AI 分支；删除 `hw_time.c`。
8. 从 `i18n.h/.c` 删除 `STR_AI_*` / `STR_GROUP_AI` / `STR_SET_SUB_AI`，重新生成字库。
9. 删除 `docs/xiaozhi.md` 和 README 的小智章节。

## 激活/服务端

- 默认 OTA：`https://api.tenclass.net/xiaozhi/ota/`（Kconfig 默认值）。
- 设置页可改 OTA 地址；改动会清空 `websocket` 激活配置并需要重启。
- 激活码显示在聊天面板；到 xiaozhi.me 绑定；固件每 5 s 重查一次，5 分钟超时。
- `Device-Id` 是 P4 基片 MAC（`esp_read_mac(ESP_MAC_BASE)`），`Client-Id` 是 NVS
  `board/uuid` 的持久 UUID。
- 服务端返回的 `firmware` 字段只记日志，固件不会下载/升级。

## 音频

- AI 开启：I2S_NUM_0，16 kHz，16 bit，TX+RX，ES8311 IN_OUT 单设备；
  AFE 只做 NS/VAD，不做 AEC；TTS 播放时关语音处理，保留 WakeNet。
- AI 关闭：BSP 的 44.1 kHz 播放/录音路径保持不变。
- 唤醒词模型：`wn9_nihaoxiaozhi_tts`，`srmodels.bin` 通过 `srmodel_load()` 从 PSRAM 加载。
- 提示音：使用 XiaoZhi 原版 OGG（`assets/ai/*.ogg`，经 `AudioService::PlaySound()` + `OggDemuxer` 播放）：
  唤醒/开始聆听 `popup.ogg`、首次激活码 `activation.ogg`、激活成功 `success.ogg`、错误 `exclamation.ogg`。
- 语音唤醒自动打开的气泡在 TTS 回答结束、播放队列清空后延迟 5 秒**只收起 UI**，不调用 `ai_stop_session()`；会话仍按原超时/结束逻辑。若气泡是自动收起的，新的 STT 或 TTS 文本到达时会自动重新弹出；手动点机器人打开的气泡保持。没有网络音乐/歌词播放器。
- 用户**手动关闭**完整对话后（点气泡外、右上角关闭或再点机器人），会话不结束，也不再自动弹完整窗口；后续用户/小智/系统信息显示在机器人上方 320×54 的小气泡里。文字超出宽度时用 `LV_LABEL_LONG_SCROLL_CIRCULAR` 横向循环滚动，8 秒没有新信息自动隐藏；点小气泡可重新打开完整对话。会话进入 IDLE 或重新激活时会清除“手动关闭”状态。
- 页面切换不卡顿：播放任务优先级提高到 6（output）/5（opus），playback 队列从 2 增加到 4（约 240 ms 缓冲）。
- MCP：hello 声明 `features.mcp=true`；移植官方 `McpServer` 的 Property/McpTool/ParseMessage/DoToolCall 结构，实现 `initialize`、`tools/list`、`tools/call`，提供
  `self.get_device_status`、`self.audio_speaker.set_volume`、`self.screen.set_brightness`。
  主页演示开关改由本地 Lua/Agent 的 `claw.panel.demo(on)` 控制。
  音量/亮度直接调用 `settings_set_volume()` / `settings_set_brightness()`，会同步落 NVS。
- 入站文本帧：只有 ≤1 KB 的消息在 WebSocket 接收任务上解析（用于 hello）；其余整帧
  复制到 PSRAM 后由 AI 任务解析，避免大 MCP 参数（Lua 代码）在 4 KB 接收任务上分配
  cJSON。cJSON 的 malloc/free 钩子改为 PSRAM 优先。上限
  `CONFIG_TEMPLATE_CLAW_MCP_MAX_JSON`（默认 24 KB），超限丢帧并记日志。
- Claw 工具（`self.claw.*`）及 Lua 沙箱见 `docs/claw.md`。

## 资源生成

```bash
python tools/ai_assets/build_assets.py
# 可选：重新生成机器人图标
powershell -File tools/fonts/gen_robot_font.ps1
# 改了 i18n 文案后
python tools/fonts/collect_chars.py
powershell -File tools/fonts/gen_fonts.ps1
```

生成后的 `assets/ai/` 随工程提交，普通构建不需要 Python/Node。资源改完后执行
`idf.py build && idf.py claw-flash`，把重新生成的 4 MB `build/claw.bin` 写到
`0xC00000`；普通 `idf.py flash`/`app-flash` 不会更新 `/fatfs/ai`。

## 排查

| 现象 | 检查 |
|---|---|
| 没有机器人 | `CONFIG_TEMPLATE_ENABLE_XIAOZHI=y`、NVS `ai/en=1`、`ai_available()` |
| 聊天中文方框 | `assets/ai/font_puhui_common_20_4.bin` 是否烧入 `/fatfs/ai`（`idf.py claw-flash`）；`CONFIG_LV_FONT_FMT_TXT_LARGE=y` |
| 唤醒词无效 | FAT 是否挂载、`/fatfs/ai/srmodels.bin` 是否加载、日志模型名是否为 `wn9_nihaoxiaozhi_tts` |
| 按机器人无反应 | 是否已激活；看 `ai.ota` 日志与设置页状态/激活码 |
| WebSocket 连不上 | 时间/证书、Wi-Fi/Ethernet 是否有 IP、token 是否过期；`ai.net`/`WS` 日志 |
| TTS 无声 | 音量是否 0、ES8311 PA、`AudioService` 是否解码（24k→16k 重采样日志） |
| 服务端固件升级 | 不应发生；`grep -R "esp_ota_" main/ai` 应为空 |

## 许可证

- XiaoZhi v2.2.6：MIT，见 `main/ai/vendor/LICENSE.xiaozhi`。
- 78/esp-ml307：Apache-2.0。
- esp-sr：Espressif MIT；esp_audio_codec/effects：Espressif Modified MIT（限 Espressif 产品）。
- puhui 字体来源/许可、MDI Apache-2.0：见 `assets/ai/THIRD_PARTY_NOTICES.txt`。
- “你好小智/XiaoZhi”品牌权利归其所有者；商用唤醒词需自行确认授权。
