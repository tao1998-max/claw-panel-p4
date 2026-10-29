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
- 改完 SD 应用需要重启才会重新扫描（与 FAT 相同）。

面板 App 的素材可以放 `/sdcard/panel-assets/<app_id>/`。应用应优先读 SD，
失败/无卡时退回包内同尺寸 fallback：

- `image.load_file("/sdcard/...jpg")`：baseline JPEG，≤4 MiB、≤1920×1080；
- `storage.*` / `io`：可读写 `/sdcard` 绝对路径；
- `audio.play("/sdcard/...ogg")`：允许；
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

路径相对 `/sdcard/panel-assets`（`/api/sd-assets*`）或 `/sdcard`（`/api/sd-files*`），
拒绝 `..`、反斜杠、控制字符和根删除。未插卡返回 503，空间不足返回 507。
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
- 网络模块文档作为 `builtin_lua_modules` 技能种子写入（`CLAW_SEED_VERSION 28`），外部 AI 的 `skills/panel-app-design/references/networking.md` 有同样的契约说明。

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
- cron 依赖系统时间：启动时优先用板载 RX8025T 播种；SNTP 校时后回写 RTC；两者都不可用时只跑 interval/once；
- 创建定时入口仍受“允许小智运行 Lua”开关约束（`self.claw.add_schedule` /
  本地 Agent 的 `cap_lua`）；已存在计划由 router 按规则执行；
- 任务记录：16 条环形表（origin / 状态 / 起止时间 / 进度），页面每秒取一次 JSON 快照。
- 端到端验证：本地 Agent 的 `scheduled_task` skill 创建
  `interval=10s + max_runs=1` 的 `demo10`，10 秒后 router 匹配 `demo10_run`、
  执行 `system/scripts/demo_on.lua`，`claw.panel: demo switch -> 1`。
- 面板 Claw 页按 ESP-Claw Web 管理端的顶层导航重做：系统状态（含 FAT 用量） / 系统设置
  （LLM、IM）/ 记忆管理 / 在线聊天 / 定时任务 / 能力管理 /
  Lua 模块管理 / 文件管理；左侧导航和内容区都可滚动。
  任务、定时、脚本、技能运行、测试定时等开发向按钮已移除：定时和脚本由
  Agent / Web 管理端管理，面板只做状态查看和必要开关。文件页是可读写工作区
  的只读浏览（点文件查看内容），不提供新建/删除/运行入口。
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
（`CONFIG_LV_USE_CUSTOM_MALLOC` + `main/ui/lvgl_mem_psram.c`）；启动 30 s 后
内部堆实测 free 142 KB / largest 53 KB / min 100 KB，优化前约 52/31/43 KB。

## 排查

| 现象 | 检查 |
| --- | --- |
| Claw 页面显示不可用 | `CONFIG_TEMPLATE_ENABLE_CLAW=y`；日志是否有 `ESP-Claw port ready` |
| FAT 挂载失败 | 分区表是否含 `claw`；日志 `claw_storage`；`ESP_ERR_NO_MEM` 查 `VFS_MAX_COUNT` |
| 屏幕闪烁/卡顿 | Lua 任务优先级必须低于 LVGL；避免长时间忙等脚本；确认 `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`，否则 Agent 记忆写 FAT 时可能整屏闪 |
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
  微信扫码登录也已绑定到 `cap_im_wechat_qr_login_*`。
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
- API：`/api/status`、`/api/storage`、`/api/config`、`/api/capabilities`、`/api/lua-modules`、`/api/files*`、`/api/sd-assets*`、`/api/sd-files*`、`/sd-files/*`、`/api/schedules*`、`/api/webim/*`、`/api/wechat/login/*`、`/api/restart`，以及 `/ws/webim`；
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
- SD：`/api/sd-assets` 只暴露 `/sdcard/panel-assets`；`/api/sd-files` 管理整张卡（Web 文件页“内部存储/SD 卡”切换），两者都是写接口；App 包上传后需重启才能刷新 launcher，SD 素材变更只需重开 App；
- 安全：没有 HTTP 鉴权，只允许可信 LAN 访问；
- 注意：`CONFIG_LWIP_MAX_SOCKETS=16`，HTTP 服务最多 10 个客户端连接，给 XiaoZhi WSS 留余量。
