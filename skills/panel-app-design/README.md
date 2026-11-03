# panel-app-design

给 **1024×600 ESP32-P4 面板** 生成 LVGL Lua App 的 Claude Agent Skill。使用对象是
外部 AI 平台（Claude Code / Claude.ai / ChatGPT / Gemini / Coze 等），不是设备上的
小爪 Agent。

## 能做什么

- 生成可被面板启动器发现的 App 包（`launcher.json` + `scripts/` + `assets/`）；
- 中文 UI：收集文案、生成 TTF 子集、校验缺字；**子集是静态字形集**，改文案后必须
  重建并重传 TTF（只传 `main.lua` 会出现方框）；每次 `lvgl.font_load` 都会把整个
  TTF 读入 PSRAM（N 个字号 = N 份拷贝），子集控制在 100-150 KiB；
- **控件与交互**：flex/grid 布局、滚动容器、`lvgl.keyboard` + `textarea` 文本输入、
  `obj:set_property/get_property` 通用属性读写，以及 `clicked`、`pressed`、
  `value_changed`、`ready`、`cancel` 等事件；
- 海报式分层画面：全屏 JPEG、透明 ARGB 精灵、LVGL UI、局部 EAF/Lottie 动效；
- **FAT 或 SD 整包部署**：`/fatfs/claw/data/apps/<id>/` 或 `/sdcard/apps/<id>/`，
  SD 包原地启动、可自带 TTF；无卡时 SD 应用不出现在首页；
- **SD 运行期素材后备**：高清图/动画/音频放 `/sdcard/panel-assets/<id>/`，无卡自动降级；
- **网络 App**：`net`（TCP/UDP/DNS/HTTP）、`mqtt`、`websocket` 三个 Lua 模块，
  `next_event()` 事件循环、socket 预算、Lua `http_server` LAN 路由；
- **图标与版本**：96×96 baseline JPEG 图标 + `version` 字段，首页网格显示；
  `make_app_icons.py` 生成图标，`run_app.py --list` 查看已装版本；
- **图像效率**：JPEG 解码期 1/2–1/8 缩放（缩略图）、`encode`/`save_file`
  可选 quality/subsample、`crop/rotate/flip` 硬件变换；
- **图表画布**：复用一块 `image.new` RGB565 画布，`fill_rect` 重绘后再
  `set_frame`；920×320 分时图约 50ms，不再逐像素拼 Lua 字符串；
- **语音合成**：`claw.speak(text[, timeout_ms])` 调用豆包 TTS 播报任意文字；
  显式调用、不受“自动播报”开关影响，需要设备已配置豆包凭据，阻塞播放
  （默认 120 s、最长 600 s），emoji 自动清洗；
- **音频**：`audio.play` 短音效、`open_output`/`open_input` PCM、
  OGG/Opus player、WAV 录音、电平/FFT 频谱分析；共享小智 AudioService；
  本地/HTTP(S) MP3 流按原生采样率独占 ES8311 播放（期间小智语音暂停，
  结束自动恢复）；`poll().level` 可驱动频谱动画；
- **大图背景**：默认 JPEG 解码一次 + `set_frame` 零拷贝；若暗部色块明显，
  可选用 `image.load_raw` + `make_raw_background.py`（离线解码 + Bayer 抖动）；
- **动效**：局部 EAF（`src_data`）或小型 Lottie（`lvgl.lottie`，`src` 可直接读 SD，
  也可传 `src_data`）；每个实例占 `w*h*4` 字节 PSRAM，软件渲染，尺寸要小；
- **序列帧/GIF**：`lvgl.animimg` 播放 PNG/JPEG/`.bin` 序列，`lvgl.gif` 播放 GIF；
  解码器已开启，两者都支持 `D:`（FAT）和 `S:`（SD）路径，软件解码，尺寸要小；
- **RS485 串口**：UART1 TX26/RX27，115200 8N1，EN 自动；
  `require("uart").new(1, 26, 27, 115200)`，端口单所有者；App 内一次打开、
  `pcall` 捕获占用错误、主循环非阻塞轮询（1 KiB RX，20-40 ms）、退出关闭；
  可做串口/TCP 双模式切换；
- 包/素材校验、FAT 预算检查、LAN 上传、SD 素材上传与回读校验。

## 不做什么

- 不改固件、驱动、分区表；
- 不做设备侧 ESP-Claw Skill（那是 `components/claw_port/seed/skills` 的 JSON frontmatter 格式）；
- 不调用设备侧 `publish_app`/`read_file`/`lua_run_script`。

## 安装

Claude Code：

```bash
# 用户级
cp -r skills/panel-app-design ~/.claude/skills/
# 或项目级
mkdir -p .claude/skills && cp -r skills/panel-app-design .claude/skills/
```

其他平台：把 `SKILL.md` 作为技能说明导入，保留 `references/`、`scripts/`、
`assets/` 的相对结构；先读 `SKILL.md`，再按需加载 references。

## 依赖

- Python 3.10+；HTTP 脚本只用标准库（`urllib`）。
- `make_cjk_subset.py` / `check_font_coverage.py` 需要 `fonttools`。
- `prepare_sd_assets.py` 需要 `Pillow`。

```bash
pip install fonttools pillow
```

脚本在缺少依赖时会打印 `pip install` 提示；`--help` 不依赖这些包。

## 快速开始

```bash
# 0) 静态坑点检查（保留字方法名、浮点坐标、and/or 截断多返回值）
python scripts/check_lua_pitfalls.py <pkg>/scripts --strict

# 1) 校验 App 包
python scripts/validate_app.py --app-dir <pkg> --remote-id <id>

# 2) 中文字体
python scripts/make_cjk_subset.py --source-font NotoSerifSC-Medium.ttf \
  --lua <pkg>/scripts/main.lua --output <pkg>/assets/ui_cn.ttf

# 2b) 生成 96x96 图标（需要 ffmpeg；也可手画一张 baseline JPEG）
python scripts/make_app_icons.py --out <pkg> --ids <id>   # 内置常用图形；不存在时自备

# 3) 生成 SD 素材 + 同尺寸 FAT fallback + sd_manifest.json
python scripts/prepare_sd_assets.py --spec <sd_plan.json>

# 4) 看设备容量/SD 状态（新固件）
python scripts/probe_device.py --host http://<device-ip>

# 5) 上传 FAT 包（上传后自动 /api/apps/refresh，无需重启）
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg>

# 5b) 或者把整包部署到 SD（无需 FAT 预算，无卡时不出现在首页）
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg> --volume sd

# 5c) 闭环自检：让设备自己启动 App、运行 3 秒、停止并返回结果/错误
python scripts/run_app.py --host http://<device-ip> --app-id <id> --duration 3 --refresh

# 6) 上传 SD 素材（不需要重启设备）
python scripts/upload_sd_assets.py --host http://<device-ip> --sd-dir <staging>
```

`<pkg>` 的目录名通常等于 App id；如果本地暂存目录叫 `app/`，用
`--remote-id` 或让 `upload_app.py` 从 `launcher.json` 自动取 id。

## 目录

```text
panel-app-design/
├── SKILL.md
├── README.md
├── CHANGELOG.md
├── references/          # 契约、存储、设计、分层、API、字体、排错
├── scripts/             # 12 个黑盒 CLI
├── evals/               # 5 个真实 prompt + 触发边界
└── assets/examples/
    ├── countdown/       # FAT-only 中文倒计时
    ├── mqtt_panel/      # MQTT 网络示例（心跳 + 指令）
    ├── network_radio/   # MP3 电台 + 画布频谱
    ├── anim_lab/        # Lottie + animimg，SD 升级 + 包内降级
    └── sd_fallback/
        ├── app/         # FAT 包（含 fallback 与 sd_manifest）
        ├── sd/          # SD 高清素材树
        ├── sd_source/   # 可复现的源图
        └── sd_plan.json # prepare_sd_assets 的输入
```

## 固件要求

FAT 包上传只依赖 `/api/files`。SD 整包与素材的远程管理需要固件包含：

- `GET /api/storage`：FAT/SD 总量、剩余、挂载状态；
- `/api/sd-files`（GET/DELETE）、`/api/sd-files/mkdir|upload|file`、直链
  `/sd-files/*`：管理整张卡（SD 应用放 `/sdcard/apps/<id>/`）；
- `/api/sd-assets`：受限的 `/sdcard/panel-assets/` 素材接口；
- `/api/apps*`（列表 / `refresh` / `launch` / `test`）与 `/api/lua-tasks*`
  （运行中任务 / 停止）：上传后免重启刷新、无人工点击的闭环自检；
- `/api/lua-failures`：最近失败任务及 `path:line: reason`（`probe_device.py`
  会读取）；
- Lua `http_server` 的 `/lua/<app_id>/*` 与 `/api/lua/<app_id>/*` 路由
  （`CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER=y`），用于让 App 被 LAN 访问。

没有这些端点时：

- `probe_device.py` 会提示旧固件；
- `upload_app.py` 打印「no /api/apps/refresh」提示，改用 `--restart` 重启即可；
- `run_app.py`（launch/test）不可用，失败原因只能看设备 Claw → 任务的红色记录；
- `upload_app.py --volume sd`、`upload_sd_assets.py` 会失败并给出手动拷卡步骤；
- FAT 包上传、App 运行、设计流程完全不受影响。

## SD 约束（重要）

- 卡必须在**开机前**插入；没有热插拔；
- 只支持 FAT（不是 exFAT），`max_files=5`；
- 文件名用 ASCII；SD 簇 64 KB；
- SD 应用在 `/sdcard/apps/<id>/` 原地启动；同 id 的 FAT 包优先，SD 副本被忽略；
- `lvgl.font_load` 支持绝对 `/sdcard/...`（本固件已注册 SD 字体根），TTF 可随包放 SD；
  旧固件只认 FAT，用 `D:apps/<id>/...` 且字体留在 FAT；
- LVGL 的 `src` 支持 `D:`（FAT 工作区）和 `S:`（SD 卡，需已挂载）：
  `lvgl.image{src="S:/panel-assets/<id>/bg.png"}`、`lvgl.animimg`、`lvgl.gif`、
  `lvgl.eaf` 都能读 SD；`lvgl.lottie{src}` 走 ThorVG，仍用绝对 `/sdcard/...`；
- 首屏/开机必需素材仍建议放 FAT 并留同尺寸 fallback，避免无卡时空白；
- SD 图与 FAT fallback 必须同宽高（运行时无缩放 API）；
- 上传 SD 素材后重开 App 即可；上传/更新 App 包后 `upload_app.py` 会自动
  `POST /api/apps/refresh`，无需重启（旧固件没有该端点时才用 `--restart`）。

## 安全

Web API 没有鉴权。`/api/sd-assets` 被限制在 `/sdcard/panel-assets`，但
`/api/sd-files` 可以写整张卡；两者都只应在可信 LAN 使用。App Lua 可以读 SD、
使用 `io`/`os`，只安装可信包。

## 许可

示例字体来自 Noto Serif SC（SIL OFL 1.1）；示例背景/符号为原创生成素材。
见 `assets/examples/NOTICE.md` 与 `assets/examples/OFL.txt`。
