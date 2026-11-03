# ESP-Claw 集成说明

本工程把 [espressif/esp-claw](https://github.com/espressif/esp-claw) 裁剪后内置到
`components/`，并让它与小智语音协同：

- **小智云端是主脑**：ASR / LLM / TTS / 生成 Lua 都在云端；
- **ESP-Claw 是本地执行层**：Lua 沙箱、文件工作区、后台任务、定时自动化、任务列表；
- 音频和网络由本工程负责；主 UI 仍由本工程 `main/ui` 管理。`display_service`
  作为 Lua display 模块的依赖已引入，但不接管主界面；不引入 Espressif Board
  Manager / audio_hub / wifi_manager。

上游版本：commit `9b23c15bc28703369b4b7fad3e810f8cb535fb59`（Apache-2.0），
裁剪与补丁清单见 `components/UPSTREAM.md`。

## 目录与启动

```
components/
├── claw_port/          ★适配层（唯一入口）
│   ├── claw_port.c       启动顺序、能力注册、调度事件
│   ├── claw_storage.c    /fatfs 挂载、种子、恢复出厂
│   ├── claw_lua_claw.c   Lua 全局 `claw` 表
│   ├── claw_api.c        MCP/UI 共用的运行时 API
│   ├── claw_task_registry.c 任务记录/进度/配额
│   ├── claw_panel.c      面板动作钩子注册
│   └── claw_settings.c   NVS 命名空间 claw
├── claw_cap/ claw_core/ claw_memory/ claw_skill/ claw_event_router/
├── claw_manager/ claw_endpoint/ claw_http/ claw_utils/ claw_hw_registry/
├── cap_lua/ cap_files/ cap_scheduler/ cap_skill_mgr/ cap_router_mgr/
├── cap_agent_mgr/ cap_session_mgr/ cap_llm_config/ cap_llm_inspect/
├── cap_system/ cap_http_request/ cap_web_search/ cap_im_local/
├── lua_module_call_capability/ lua_module_json/ lua_module_system/
├── lua_module_delay/ lua_module_thread/ lua_module_storage/
├── lua_module_event_publisher/ lua_module_http_server/  （vendored）
└── lua_net_common/ lua_module_net/ lua_module_mqtt/ lua_module_websocket/
    （产品扩展：TCP/UDP/DNS/HTTP、MQTT、WebSocket）
main/claw/
├── claw_mcp_bridge.cc   self.claw.* MCP 工具（需要 McpServer，所以放在 main）
└── claw_panel_hooks.c   claw.panel.* -> settings / ai_ui
```

启动顺序（`app_start()`）：

```
NVS → settings → ai_early_init(FAT/字体/模型) → 显示+UI → 外设 → settings_apply
→ claw_panel_hooks_init → claw_port_init（FAT → claw_cap → 官方 Lua 模块
→ 能力组 → 调度器）→ ai_start
```

`CONFIG_TEMPLATE_ENABLE_CLAW=n` 时 `claw_port_init()` 是空函数，链接器丢弃全部 Claw
代码；`components/` 仍会参与编译（IDF 的 Kconfig 在组件 CMake 之后才运行，不能按
`CONFIG_*` 裁剪 SRCS）。

## 分区与工作区

`partitions.csv`：

| 分区 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- |
| claw | 0xC00000 | 4 MB | FAT（wear leveling），挂载 `/fatfs`：`/fatfs/ai` 小智资源 + `/fatfs/claw` 工作区 |

`/fatfs/ai` 放聊天字体、唤醒词模型和提示音（约 1.55 MB），只读使用；Claw 工作区保持在
`/fatfs/claw`，脚本/计划里已保存的绝对路径不变。`build/claw.bin` 是包含 `/ai` 的
4 MB FAT 镜像，首次迁移/整片擦除后执行 `idf.py claw-flash` 写入；普通
`idf.py flash`/`app-flash` 不会包含或覆盖它。

```
/fatfs/claw/data/scripts     用户与 AI 生成的 Lua（AI 临时脚本 _ai_*.lua 用后即删）
/fatfs/claw/data/schedules   定时任务持久化
/fatfs/claw/data/logs        预留
/fatfs/claw/data/skills      用户技能
/fatfs/claw/data/apps        运行时应用（FAT 根，可写；另有 SD 只读根 /sdcard/apps）
/fatfs/claw/data/agent       Agent 会话/记忆（可选）
/fatfs/claw/system/skills    固件种子技能（只读）
/fatfs/claw/system/.seeded   种子版本
```

- 首次启动：只有分区**完全空白**时才格式化一次并写种子；已有 FAT 但挂载失败时
  **绝不静默格式化**，只报错保留数据。
- 种子版本 10 曾移除“AI 生成应用/2D 画布”支线；现在应用注册表已恢复：
  应用存放在可写的 `data/apps`（并可选扫描 SD 只读根 `/sdcard/apps`）；
  不再扫描 `system/apps`（示例 Hello App 已移除）。
- 种子版本 21 修复 `EMBED_TXTFILES` 尾部 NUL 被写入 FAT 的问题（官方技能 Lua
  脚本会因此报 `unexpected symbol`）；同时写入官方 `builtin_lua_modules` 技能
  （24 个模块 README 文档，生成自 vendored `lua_module_*`/`lua_driver_*`）。
  首次从 20 升级到 21 会重写约 90 KB 文档，启动多花十几秒；之后启动不再写。
- 种子版本 11 预置隐藏脚本 `data/scripts/_test.lua`；脚本列表会跳过 `_` 前缀，
  不会出现在用户脚本里（当前 Claw 页不再提供测试定时按钮）。
- 种子版本 14 预置官方定时栈：`system/skills/scheduled_task`、`system/skills/cap_router_mgr`、
  `system/skills/cap_scheduler` 和 `system/scripts/demo_on.lua`；同时创建
  `data/router_rules/` 与 `system/router_rules/`。
- 种子版本 15 写入官方 `arg_schema.lua`（`system/scripts/builtin/lib/`）和
  官方记忆技能 `memory_ops`、`profile_memory_ops`。
- 种子版本 16 增加官方 `cap_http_request`、`cap_web_search`、
  `cap_llm_inspect_image` 技能。
- 恢复出厂：`settings_factory_reset()` 先调 `claw_port_factory_reset()`
  （停任务、清 `data/`、重播种子），再擦 NVS。
- 种子版本 26 起 `/fatfs/ai` 与 `/fatfs/claw` 共用同一 4 MB FAT；启动时若
  `data/router_rules/router_rules.json` 缺失或只有 `[]`，会写回官方恢复规则。
- 种子版本 28 增加 `net`/`mqtt`/`websocket` 三份 Lua 模块文档到
  `builtin_lua_modules`（TCP/UDP/DNS/HTTP、MQTT、WebSocket），升级后首次启动会
  重写这些文档。
- 种子版本 36 刷新 `builtin_lua_modules` 文档：LVGL Lottie/animimg/GIF、
  `D:`/`S:` 文件系统、RS485 使用官方 `uart`，并补充 GPIO 等硬件驱动需要
  先注册资源的说明；升级后首次启动会重写这些文档。
- `idf.py app-flash` 不动 FAT；`idf.py flash` 也不含资源镜像；只有
  `idf.py claw-flash` 会重写整个 4 MB 分区。

FATFS 配置（`sdkconfig.defaults`）：`LFN_HEAP` + `MAX_LFN=255` + `CP936`
（中文文件名），`VFS_MAX_COUNT=16`（控制台/FAT/摄像头/SD/Claw 都要注册 VFS），
`FATFS_VOLUME_COUNT=4`（SD + Claw 留余量）。

## SD 卡素材与应用（可选）

SD 卡挂载在 `/sdcard`，由 BSP 在开机时挂载一次：

- 只在**开机前**插卡有效；没有卡检测/热插拔，运行中拔卡不会被感知；
- 只支持 FAT（不是 exFAT），`max_files=5`，簇大小 64 KB；
- 无卡时启动继续，状态页显示 `no card`；此时 SD 应用根不存在，首页应用列表为空；
- **两个 App 根**：`/fatfs/claw/data/apps`（可写，优先）和 `/sdcard/apps`（只读）。
  注册表先扫 FAT 再扫 SD，同 id 时 FAT 覆盖 SD（SD 副本被忽略）；
- SD 根上的应用**原地启动**（入口脚本直接从卡上加载，不复制到 FAT）；
  `publish_app`/`remove_app` 仍只操作 FAT 根，SD 应用的增删走 Web 文件管理；
- 应用整包放 SD 时，包结构与 FAT 相同（`launcher.json` + `scripts/` + 素材）；
  清单支持可选 `version`（≤31 字符）与 `icon`（96×96 baseline JPEG），首页图标网格显示二者；
- 改完 SD 应用需要重启才会重新扫描（与 FAT 相同）。

面板 App 的素材可以放 `/sdcard/panel-assets/<app_id>/`。应用应优先读 SD，
失败/无卡时退回包内同尺寸 fallback：

- `image.load_file("/sdcard/...jpg")`：baseline JPEG，≤4 MiB、≤1920×1080；
  可传 `{max_width=..., max_height=...}` 直接按 1/2~1/8 解码成缩略图；
- `image.load_raw(path, w, h[, image.RGB565])`：读紧凑 RGB565/GRAY8 原始像素；
  可选：暗部色块明显时，用 `tools/make_raw_background.py` 离线抖动成 RGB565
  再交给 `set_frame`；默认仍用 JPEG + `set_frame`（参考天气 App 实测无明显差异）；
- `storage.*` / `io`：可读写 `/sdcard` 绝对路径；
- `audio.play("/sdcard/...ogg")`：允许；
- `audio.player` 支持本地 MP3 和 HTTP(S) MP3 流：按原生采样率/声道独占 ES8311
  播放，期间小智语音暂停，结束/停止/App 退出后恢复 16 kHz 并重启小智；
- EAF：用 `src_data` 读 SD 字节，不能把 `/sdcard` 传给 `src=`；
- `lvgl.font_load`：支持绝对 `/sdcard/...` 路径（claw_port_init 把 SD 挂载点注册为
  额外字体根），SD 应用可整包带 TTF 运行；同时也仍接受 `D:apps/<id>/...`（FAT 根）；
- **不支持**：`lvgl.image{src}` 读 SD（图片请用 `image.load_file`）。

Web 端点（无鉴权，仅可信 LAN）：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/storage` | FAT/SD 的总量、剩余、挂载状态，以及 `panel-assets` 是否存在 |
| GET | `/api/sd-assets?path=/<rel>` | 列出 `/sdcard/panel-assets/<rel>` |
| GET | `/api/sd-assets/file?path=/<rel>` | 下载文件 |
| POST | `/api/sd-assets/mkdir` | JSON `{"path":"/<rel>","recursive":true}` |
| POST | `/api/sd-assets/upload?path=/<rel>` | raw body，父目录必须存在 |
| DELETE | `/api/sd-assets?path=/<rel>&recursive=1` | 删除文件/目录；禁止删根 |
| GET | `/api/sd-files?path=/<rel>` | 列出整张 SD 卡 `/sdcard/<rel>`（Web 文件页 SD 选项卡） |
| GET | `/api/sd-files/file?path=/<rel>` | 下载文件 |
| POST | `/api/sd-files/mkdir` | JSON `{"path":"/<rel>","recursive":true}`；`"/"` 直接确认根存在 |
| POST | `/api/sd-files/upload?path=/<rel>` | raw body，父目录必须存在 |
| DELETE | `/api/sd-files?path=/<rel>&recursive=1` | 删除文件/目录；禁止删根 |
| GET | `/sd-files/<rel>` | 浏览器直链下载（与 `/files/*` 同风格） |
| GET | `/api/lua-failures?limit=4` | 最近失败的 Lua 任务（id/name/path/status/error），App 调试用 |
| GET | `/api/apps` | 已注册应用（FAT/SD，id/name/path/volume） |
| POST | `/api/apps/refresh` | 重新扫描应用根（上传后免重启） |
| POST | `/api/apps/<id>/launch` | 启动应用，返回 `job_id` |
| POST | `/api/apps/<id>/test?duration_ms=3000` | 启动→运行 N ms→停止，返回 `running/finished/failed` 与 `path:line: reason` |
| GET | `/api/lua-tasks` | 运行中的 Lua 任务 |
| POST | `/api/lua-tasks/<id>/stop` | 停止任务 |

路径相对 `/sdcard/panel-assets`（`/api/sd-assets*`）或 `/sdcard`（`/api/sd-files*`），
拒绝 `..`、反斜杠、控制字符和根删除。未插卡返回 503，空间不足返回 507。
`/api/apps*` 与 `/api/lua-tasks*` 让外部 AI 闭环自检：上传后 `refresh`、`test` 启动
并自动停止，再读 `/api/lua-failures` 的错误；这些接口能运行任意已注册 App
（Lua 可读写/联网），无鉴权，只能用于可信 LAN。

外部 AI 的 `skills/panel-app-design` 用 `/api/sd-assets` 验证/上传素材；旧固件没有
端点时改为手动拷卡。

## Lua 运行时

Lua 运行时为**官方 `cap_lua`，源码保持一致**：

- 每个脚本用 `luaL_newstate()` + `luaL_openlibs()`，标准库全开
  （`io/os/package/require/coroutine` 均可用）；没有应用层内存上限；
- 官方 `cap_lua_restrict_debug()` 仍会限制 `debug`；
- 官方 skill 通过 `require("capability")`、`require("json")`、
  `require("arg_schema")` 加载能力/JSON/参数校验；`arg_schema.lua` 按官方
  目录放在 `system/scripts/builtin/lib/`，由
  `cap_lua_add_package_path_dir()` 加入 `package.path`；
- 时间/停止：cap_lua 每 100 条指令检查一次；后台任务默认 60 s、最长 300 s；
- 脚本上限 64 KB，输出环 4 KB；
- Lua 图像：`image.new()` 画布支持 `fill/fill_rect/line/blit`（C 循环，
  2000 次 4×4 半透明矩形约 40 ms）；`lvgl` 的 `img:set_frame(frame)` 零拷贝接管
  RGB565 缓冲（约 0.16 ms/次），避免 `frame:data()` + `set_raw_rgb565` 的两次大拷贝；
- JPEG 选项：`image.encode` / `save_file` 支持 `quality`(1–100) 与
  `subsample`(444/422/420/gray)；`load_file` 的 `max_width/max_height` 走解码期
  1/2–1/8 缩放；`crop/rotate/flip` 优先走 PPA（JPEG 旋转可走解码器），
  `resize` 对未解码 JPEG 先做解码缩放，RGB565 再走 PPA；`image.load_raw`
  可把离线抖动好的 RGB565 背景直接零拷贝交给 LVGL；
- 任务失败原因写串口 `cap_lua_async` 日志，并保留最近 4 条供
  `GET /api/lua-failures` 查询（App 退出后用它定位 `path:line: reason`）；
- Claw 后台/Lua 任务优先使用 PSRAM 栈；本工程启用 XIP 后 FAT/NVS 写入不再
  关闭 cache，PSRAM 栈可安全使用。`claw_core` 和 `cap_scheduler` 仍固定内部
  RAM 栈；Lua 任务与 LVGL 同为优先级 4（网络/UI 都是短切片，互不抢占）。

`claw` 表是本工程额外注册的官方模块（通过 `cap_lua_register_module`）：

```lua
claw.log(...)                     -- 输出到任务日志
claw.sleep(ms)                    -- 0–5000，停止/超时会中断
claw.time() / claw.uptime_ms()
claw.progress(pct, msg)           -- 任务列表/进度条
claw.abort_requested()
claw.get_args()                   -- args 全局的副本
claw.file.read(path)              -- ≤64 KB；data 优先，其次 system
claw.file.write(path, text)       -- 只允许 data/，≤16 KB/次
claw.file.list(dir) / exists / remove
claw.json.encode(v) / decode(s)
claw.panel.set_volume(pct) / set_brightness(pct)
claw.panel.status() / show_message(text) / demo(on)
```

路径必须是工作区相对路径（如 `scripts/hello.lua`）；拒绝绝对路径、`..`、反斜杠。
`claw.panel.*` 未注册钩子时返回 `nil, err`，不会崩溃。

### Lua 网络模块

本工程额外注册三个网络模块（默认开启，无 Kconfig 门槛）：

| 模块 | 内容 |
| --- | --- |
| `net` | `tcp`（阻塞+超时、可选 TLS）、`udp`（bind/广播/多播）、`resolve`（lwip 单地址）、`http.request`（esp_http_client，状态码/响应头/正文/chunked/重定向/save_path）、`online()/local_ip()` |
| `mqtt` | esp-mqtt 3.1.1；`connect/publish/subscribe/unsubscribe`、LWT、自动重连、`next_event()` 事件流 |
| `websocket` | esp_websocket_client；`wss`、分片重组、ping、自动重连、`next_event()` 事件流 |

- **事件模型**：网络任务只把事件拷进队列，脚本用 `:next_event(ms)` 在 Lua 任务上取事件；不存在跨任务调用 Lua 回调。
- **可取消**：所有阻塞等待 50 ms 切片并检查停止标志；TLS 握手在独立任务上受超时约束。
- **socket 预算**：单脚本 3 个 socket / 1 个 TLS；全局 4 个 socket / 2 个 TLS（`CONFIG_LWIP_MAX_SOCKETS=28`），超限返回 `socket budget exhausted`，保护 Web UI 与小智语音。
- **Lua http_server 路由已启用**：`/lua/<app_id>/*` 静态、`/api/lua/<app_id>/*` 接口，由 App 的 `serve_forever()` 或非阻塞 `poll()` 分发（`CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER`）。
- `insecure=true` 依赖 `CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`（`sdkconfig.defaults` 已开启）。
- DNS 只返回单个地址（lwip 公共 API 限制）；IPv6 实际仅字面量可用（无 SLAAC）。
- 网络模块文档作为 `builtin_lua_modules` 技能种子写入（`CLAW_SEED_VERSION 36`），外部 AI 的 `skills/panel-app-design/references/networking.md` 有同样的契约说明。

### RS485 串口（UART1）

板载 RS485 接 UART1：**TX=GPIO26、RX=GPIO27，115200 8N1**，方向 EN 由收发器硬件自动处理。按官方 ESP-Claw 硬件模型，固件只在启动时做一次引脚/状态自检，并把 `uart_bus:1`、`gpio:26`、`gpio:27` 注册进硬件资源表；随后释放端口，由 Lua 的官方 `uart` 模块按需独占：

```lua
local uart = require("uart")
local u = uart.new(1, 26, 27, 115200)

u:flush_input()
u:write({0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A})
local reply = u:read(64, 500)   -- 超时 500 ms
u:close()
```

- 端口单所有者：脚本 `close()` 或任务退出后其它脚本才能重新打开
- `read` / `read_line` 超时单位 ms，0 为非阻塞；接收缓冲 1 KiB
- 固件不再常驻 RS485 任务，也不做回环测试；请求/应答协议由脚本自行 `flush_input()` 和等待

## 小智 MCP 工具

在 `self.*` 常用工具之后注册：

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `self.claw.status` | – | Lua 开关、任务数、工作区空间、校时 |
| `self.claw.run_lua` | `code`, `args`, `timeout_ms` | 同步执行，≤3 s（可到 8 s），返回 print 输出 |
| `self.claw.start_task` | `name`, `code`/`path`, `args`, `timeout_ms` | 后台任务，立即返回 job id |
| `self.claw.list_tasks` | `state` 可选 | 任务列表（含历史） |
| `self.claw.get_task` | `id` | 状态、输出尾部、进度 |
| `self.claw.stop_task` | `id`, `wait_ms` | 协作停止 |
| `self.claw.add_schedule` | `id`, `kind`, `when`, `script`, `args` | once / interval / cron |
| `self.claw.list_schedules` | – | 计划列表与下次触发 |
| `self.claw.ask` | `prompt`, `origin` | 仅本地 Agent 开启时：向 Agent 提问并等待回答 |
| `self.claw.agent_task` | `name`, `prompt` | 仅本地 Agent 开启时：启动后台 Agent 任务 |

设计约束：

- 所有工具**快速返回**，长任务一律后台；没有延迟 JSON-RPC 回复；
- 同步 `run_lua` 最长 3 s（`CONFIG_TEMPLATE_CLAW_LUA_SYNC_MS`），后台任务不主动 TTS；
- AI 来源任务配额 ≤2，Agent ≤1，至少留 1 个并发给用户脚本；
- 入站 WebSocket 文本帧不再在 4 KB 接收任务上解析：非 hello 帧整帧复制到 PSRAM，
  由 AI 任务（16 KB 栈）解析；cJSON 统一走 PSRAM 分配。上限
  `CONFIG_TEMPLATE_CLAW_MCP_MAX_JSON`（默认 24 KB）。

## 定时与任务

- `cap_scheduler` 支持 once / interval / cron，状态落
  `/fatfs/claw/data/schedules/schedules.json`；
- **官方动作链**：`cap_scheduler` 事件发布到 `claw_event_router`，
  `cap_router_mgr` 管理 `data/router_rules/router_rules.json`；
  `scheduled_task` skill（种子 v14）按 `scheduled_task` 契约创建
  `add_router_rule` + `scheduler_add`，`run_script` 模式由 router 执行脚本；
- `claw_api_add_schedule()` / MCP `self.claw.add_schedule` / Claw 页“测试定时”也按
  官方流程：先 `add_router_rule`（`<id>_run`，match `event_type=schedule`、
  `event_key=<id>`，action `run_script`），再 `scheduler_add`；失败删除规则回滚；
  早期兼容规则 `claw_script_default` 已移除（启动时清理）；
- `payload_json` 只有 512 B，定时项保存脚本路径 + 参数，不内嵌脚本；
- cron 依赖系统时间：启动时优先用板载 RX8025T 播种；RTC 统一存 UTC，SNTP 校时后回写 UTC；旧固件的本地时间 RTC 会通过 NVS 标记一次性迁移。两者都不可用时只跑 interval/once；
- 创建定时入口仍受“允许小智运行 Lua”开关约束（`self.claw.add_schedule` /
  本地 Agent 的 `cap_lua`）；已存在计划由 router 按规则执行；
- 任务记录：16 条环形表（origin / 状态 / 起止时间 / 进度），页面每秒取一次 JSON 快照。
- 端到端验证：本地 Agent 的 `scheduled_task` skill 创建
  `interval=10s + max_runs=1` 的 `demo10`，10 秒后 router 匹配 `demo10_run`、
  执行 `system/scripts/demo_on.lua`，`claw.panel: demo switch -> 1`。
- 面板 Claw 页收敛为四个页签：在线聊天（默认）/ 定时任务 / 工作区（记忆+文件）/
  设置，采用左侧固定轨道 + 右侧详情面板；轨道底部显示 Agent、Lua 权限和运行中
  任务数。定时任务通过对话/语音或 Web 管理端创建，面板只做触发、启停、删除；
  工作区为只读浏览（记忆文件、FAT/SD 文件，点开预览）。
- 聊天页的会话选择弹窗支持新建、切换、删除：每行右侧垃圾桶按钮 + 二次确认，
  删除走官方 `claw_agent_mgr_delete_session`，同时清理该会话的历史与技能状态；
  运行中的会话拒绝删除，删除当前会话时会自动切到其它会话或新建一个。

## 豆包语音 TTS（可选）

本地 Agent 的最终回复可以用豆包 TTS 播报，也可以由 Lua 主动调用。

- Lua：`claw.speak(text[, timeout_ms])`，用一个流式 TTS 会话播完，阻塞到播放结束；
  默认 120 s、最长 600 s。它是显式调用，不受“自动播报”开关影响；emoji/符号会
  在合成前清洗，返回 `true` 或 `nil, err`。
- 本地 Agent 的 OpenAI 兼容后端（DeepSeek 等）使用 SSE 流式请求，并复用同一条
  HTTPS 连接（keep-alive），避免每次提问重新做 TLS 握手；增量文本会实时显示在设备
  聊天页的临时气泡里，并按句提前送 TTS，最终回答仍完整写入会话历史。
- 自动播报：`auto_speak=true` 时，本地 Agent 的最终回复会自动合成并播放；关闭后
  流式自动播报入口直接拒绝，`claw.speak()` / `/api/tts/speak` 仍可显式播报。小智
  通过 `self.claw.ask` 拿到的回复不会重复播报。
- 自动播报复用一条持久 WebSocket，按回复顺序运行多个 TTS session，不再每句重新
  建连接；首次连接后后续问题不会重复做 TLS 握手。
- TTS PCM 先写入 **128 KB PSRAM 流缓冲**（约 4 s）。文本按短 TaskRequest（约 40 字）
  发送，并用**已送达音频字节信用**节流：只有服务端已为先前文本送达足够 PCM 时，才允许
  发送下一段，保持“服务端领先量”不超过约 64 KB。这样 WebSocket 接收任务不会为了整段
  回复连续阻塞——旧版 128 KB 纯背压会在约 60 s 时被服务端断开。不要改回 2 MB：整段
  回复会在播放开始时集中写入 PSRAM，实测会造成一帧整屏白闪；128 KB + 信用节流既能吸收
  网络抖动，也能让长回复完整播完。
- Claw TTS 播放期间会暂停小智的唤醒词/语音处理（`ai_set_external_audio_active()`），
  避免小智听到 TTS 后误执行指令；播放结束后恢复空闲状态。
- HTTP：
  - `GET /api/tts/status`：是否已配置、鉴权方式、音色、资源 ID、自动播报开关；
  - `POST /api/tts/config`：部分更新 NVS，字段 `api_key` 或
    `app_id`+`access_token`、`resource_id`、`speaker`、`auto_speak`；
  - `POST /api/tts/speak`：`{"text":"...","timeout_ms":30000}`，同步合成并播放；
    `timeout_ms` 最大 180 s。
- 协议：`wss://openspeech.bytedance.com/api/v3/tts/bidirection` 二进制帧，输出
  16 kHz 单声道 PCM 交给 ES8311；默认 `X-Api-Key`，也支持 AppID/Access Token。
- 资源 ID 必须与音色所属商品一致（1.0 常用 `volc.service_type.10029`，2.0 用
  `seed-tts-2.0`），否则服务端返回 `55000000`。
- 凭据存 NVS `doubao`；状态接口不回显密钥；配置/播报接口无鉴权，只限可信 LAN。
- 面板 **Claw → 设置 → 豆包语音**：API Key、App ID、Access Token、资源 ID 点开输入；
  「音色」点开选择弹窗，按 **1.0 / 2.0（通用）/ 角色 / 客服 / 多语种 / ICL** 分类，
  内置 5 个常用 1.0 音色和全部 285 个 2.0 官方音色；选「自定义」可手填任意
  `voice_type`。选择后会自动把资源 ID 切到对应代际（1.0 → `volc.service_type.10029`，
  2.0 → `seed-tts-2.0`），避免商品/音色不匹配的 `55000000`。
  自动播报开关与其它设置行使用同一行背景；密钥字段只显示“已设置/未设置”。
- 音色目录随固件静态编译（来源：火山官方音色列表），打开时不联网；普通 API Key
  没有音色列表接口（`ListSpeakers` 需要 AK/SK），新增音色需更新固件或走「自定义」。
  个别 ICL 角色/音色需要账号开通对应商品，未开通时服务端会返回权限或资源错误。
- 应用启动失败会留在任务输出里；`lvgl.font_load()` 在本固件会因
  `LV_USE_TINY_TTF=n` 报错，Agent 指南要求用 `pcall` 包住并回落到内置字体。
- **flash 写与显示**：脚本/定时写到内部 FAT 时，SPI1 默认会关闭 cache，P4 DPI
  从 PSRAM 取帧会欠载、整屏抖。本工程启用官方 `CONFIG_SPIRAM_XIP_FROM_PSRAM`
  （`.text/.rodata` 搬到 PSRAM 执行），SPI1 写走 `SPI_FLASH_CACHE_NO_DISABLE`
  路径，亮屏写 FAT/NVS 不再关闭 cache。代价：PSRAM 多占用约 3.7MB，启动稍慢；
  实测亮屏连续 5 次 FAT 写入无 DPI underrun。
- Claw 页“定时”页签：列出 `cap_scheduler` 计划，支持手动触发、启停、删除；
  “测试定时”按上游 `scheduled_task` 的 interval 形状创建 `5s + max_runs=1` 的
  `_test.lua` 计划，5 秒后只跑一次（interval 不依赖校时，开机早期也能验证）。
  列表/增删/触发都走 `claw_cap_call("scheduler_*")`，与本地 Agent 使用同一套
  `cap_scheduler` 能力，不另做私有 API。
- `cap_scheduler` 任务栈为 8192：定时回调会解析 payload 并提交 Lua 后台任务，
  6144 在 P4 上余量不足。

## 安全开关（NVS 命名空间 `claw`）

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `lua_ai` | 0 | 允许小智生成并运行 Lua；Claw 页设置项，开启需二次确认 |
| `agent_en` | 0 | 本地 Agent（预留，默认关闭） |

`CONFIG_TEMPLATE_ENABLE_CLAW=n` 时上述均不生效。恢复出厂会删除命名空间。

## 本地 Agent（可选）

- 编译：`CONFIG_TEMPLATE_CLAW_AGENT=y`；运行时还需要 `agent_en=1` 以及有效的
  `agent_backend` / `agent_base_url` / `agent_model` / `agent_api_key`。
- 编排使用官方 `claw_agent_mgr`：`claw_agent_mgr_init()` 创建 root agent，
  请求走 `claw_agent_mgr_post_root_message()` / `receive_root_for()`；
  `cap_agent_mgr`、`cap_session_mgr` 已注册，root Agent 可以 `spawn_agent`，
  root 的工具上下文由 `claw_cap_root_agent_tools_provider` 提供。
- 工具迭代上限 64，对齐官方 `CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS` 默认值；
  `cap_app_manage` 对 LLM 可见（`publish_app` / `remove_app`）。Web 里说
  “做个APP”会走官方 `app_creator` → `lua_package_authoring`/`builtin_lua_modules`
  → `publish_app`，发布后主页应用列表通过注册表回调自动刷新。
- 启动顺序与官方 `app_claw_start()` 一致：开关打开且 Key/URL/model 完整时，
  `claw_port_init()` 结束时即初始化 `claw_agent_mgr` 和 root core；Web 管理端的
  会话目录、历史、发送、删除因此从第一屏就可用。
- 状态芯片：已配置并初始化后显示 `Agent 就绪`；未配置 Key/URL/model 时显示
  `Agent 未配置`；关闭时显示 `Agent 关`。
- **会话记忆默认开启**：`CONFIG_TEMPLATE_CLAW_AGENT_MEMORY=y`，使用官方
  `claw_memory`：跨请求会话历史、长期记忆、`memory_*` 工具和自动记忆提取。
  每轮请求开始/结束会写 FAT；本工程启用 `SPIRAM_XIP_FROM_PSRAM` 后，亮屏写
  FAT 不再关闭 flash cache，不会造成 DPI 欠载/整屏闪烁。
- 修改 URL/model/Key 后，下一次请求会重建 core；已开启但未请求时
  `ready=false` 是正常现象，不要误判为“Agent 没打开”。

## 体积

| 配置 | app |
| --- | --- |
| 基线（无 Claw） | 3,192,176 B |
| Claw 基础层（无 Agent） | 3,412,528 B（+220 KB） |
| 含本地 Agent 代码（size probe） | 3,522,128 B（+330 KB） |
| 当前固件（Claw + FATFS LFN/CP936 + Claw/主页应用列表 + XIP + 官方能力/Lua 模块 + display/LVGL + Web UI + SD 素材 API） | 约 4.91 MB（含 tiny_ttf 示例应用支持） |

OTA 槽 5.9375 MB，余量约 1.3 MB。FATFS 长文件名/中文码页约占 200 KB。

内部 RAM：LVGL draw/rotate 缓冲（约 100 KB x2）与 LVGL 对象均放 PSRAM
（`CONFIG_LV_USE_CUSTOM_MALLOC` + `main/ui/lvgl_mem_psram.c`）。启动 30 s 后
内部堆随 AI 语音处理是否静音、网络/IM 初始化状态而波动，实测约
free 100–200 KB / largest 31–118 KB / min 60–155 KB；以启动日志的
`claw_port: heap@30s` 为准。

## 排查

| 现象 | 检查 |
| --- | --- |
| Claw 页面显示不可用 | `CONFIG_TEMPLATE_ENABLE_CLAW=y`；日志是否有 `ESP-Claw port ready` |
| FAT 挂载失败 | 分区表是否含 `claw`；日志 `claw_storage`；`ESP_ERR_NO_MEM` 查 `VFS_MAX_COUNT` |
| 屏幕闪烁/卡顿 | 确认 `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2` + `CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y`；Lua 任务优先级必须低于 LVGL；避免长时间忙等脚本；确认 `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`，否则 Agent 记忆写 FAT 时可能整屏闪 |
| C6 版本不匹配 / RPC 超时 | C6 从机应为 `esp_hosted 2.11.7`（与 host 匹配）；维护用 `POST /api/c6-ota`，固件从 `/sdcard/c6/network_adapter.bin` 读取，详见 README |
| `AI Lua 已关闭` | Claw → 设置打开开关（或 `lua_ai=1`） |
| 脚本无输出 | `print` 输出在任务详情；同步脚本输出上限 4 KB |
| 任务启动失败 | 并发上限 4、AI ≤2；`list_tasks` 查运行中任务后先停止 |
| 定时不触发 | cron 需要有效系统时间；确认 RX8025T 是否存在/`VLF` 是否置位、SNTP 是否成功；`list_schedules` 看 `next_fire_ms` |
| 中文文件名乱码 | 确认 `CONFIG_FATFS_CODEPAGE_936=y` |

## 未接入 / 按设计不做

- `app_claw`、`edge_agent` 由本工程 `claw_port` + 本地 Agent 替代；
  `audio_hub` / `audio_playback` 由 XiaoZhi `AudioService` + `main/audio` 替代；
  `system_ui` / `wifi_manager` 由 `main/ui` / `main/hal/hw_wifi.c` 替代。
- `captive_dns` 未移植：设备只做 STA，不提供 AP 热点配网。
- `cap_mcp_client` / `cap_mcp_server` / `mcp_mdns` 未移植；对外使用 XiaoZhi
  MCP 的 `self.*` 工具和 `main/claw/claw_mcp_bridge.cc` 的 `self.claw.*`。
- Board Manager 未接入（`cap_boards` 只有生成脚本，没有 C 实现）；
  固件改为 seed 一个静态 `board_hardware_info` 技能，列出本板设备和占用 IO。
- 摄像头 / 视觉、蓝牙、触摸（原始坐标）、IMU、IR、磁力计未移植；触摸走 BSP
  的 GT911 + LVGL 指针，IR/触摸的纯 Lua 库已 seed 到 `system/`。
- QQ / 微信 / 飞书 / Telegram 的 IM 传输已移植；凭据在 Web 管理端保存，
  微信扫码登录也已绑定到 `cap_im_wechat_qr_login_*`。IM gateway 在 Wi-Fi 或以太网
  真正拿到 IP 后才启动（`app_start` 的 `im_start_task` 等待），避免开机阶段的
  DNS/token 失败和重试。
- Lua 通用驱动（GPIO/I2C/ADC/PWM/RMT/UART 等）和 display/LVGL/image 模块
  已注册；本板没有的 button/knob/led_strip/sci/environmental_sensor 默认不注册，
  需要外接硬件时打开 `CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES`。
- 本地 Agent（`claw_core` + `claw_memory` + `claw_skill`）默认由 Kconfig 关闭；
  本机 `sdkconfig` 已开启，运行时仍可在 Web 管理端开关。

## 许可证

- ESP-Claw：Apache-2.0，见 `components/LICENSE.esp-claw`；
- Lua：`espressif/lua` 组件（MIT），随 Component Manager 下载；
- XiaoZhi 协议/音频：MIT，见 `main/ai/vendor/LICENSE.xiaozhi`。

## Web 管理端

- 启动：`web_ui_start()` 在 `ai_start()` 之后初始化并启动 `http_server`，端口 80；
- 资源：官方预编译 `index.html.gz` + `favicon.ico` 嵌入固件；
- API：`/api/status`、`/api/storage`、`/api/config`、`/api/capabilities`、`/api/lua-modules`、`/api/files*`、`/api/sd-assets*`、`/api/sd-files*`、`/sd-files/*`、`/api/schedules*`、`/api/tts/*`、`/api/webim/*`、`/api/wechat/login/*`、`/api/restart`，以及 `/ws/webim`；
- 配置映射：Wi-Fi 走 `esp_wifi_set_config` + reconnect；LLM 的地址/模型/Key 以及
  timeout、max_tokens、max_tokens_field、reasoning_effort、supports_tools/vision、
  image 限制等高级字段全部持久化到 NVS `claw` 并应用到 Agent core；搜索
  provider/key/allowlist 映射到 `cap_web_search` / `cap_http_request`；能力可见组
  映射到 `claw_cap_set_llm_visible_groups`；时区写入 `TZ`；
- IM：QQ/微信/飞书/Telegram 凭据持久化并启动对应 gateway；微信扫码登录走
  `cap_im_wechat_qr_login_*`，登录完成后由前端显式保存 token；
- Web 聊天直接使用官方 `claw_agent_mgr`/`claw_memory`：启动时（Agent 开关打开且配置完整）初始化 root core，会话列表、历史、发送、删除均走官方接口；Agent 关闭或未配置时聊天接口不可用，配置完成后重启生效；
- Web 记忆页的 `/memory` 通过文件 API 别名映射到本工程的 `data/agent/memory`；官方 `MEMORY.md` 在列表中同时以 `memory.md` 出现，读取时映射回 `MEMORY.md`，因此四个记忆卡片都能加载；
- 未映射：AP 热点字段（STA-only，`/api/status` 的 AP 字段固定为空）、
  `enabled_lua_modules` 动态启停（`/api/lua-modules` 只读）；
- SD：`/api/sd-assets` 只暴露 `/sdcard/panel-assets`；`/api/sd-files` 管理整张卡（Web 文件页“内部存储/SD 卡”切换），两者都是写接口；App 包上传后调用 `POST /api/apps/refresh` 即可刷新 launcher（`upload_app.py` 自动调用；旧固件用重启），SD 素材变更只需重开 App；
- 安全：没有 HTTP 鉴权，只允许可信 LAN 访问；
- 注意：`CONFIG_LWIP_MAX_SOCKETS=16`，HTTP 服务最多 10 个客户端连接，给 XiaoZhi WSS 留余量。
