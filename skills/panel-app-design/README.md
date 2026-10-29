# panel-app-design

给 **1024×600 ESP32-P4 面板** 生成 LVGL Lua App 的 Claude Agent Skill。使用对象是
外部 AI 平台（Claude Code / Claude.ai / ChatGPT / Gemini / Coze 等），不是设备上的
小爪 Agent。

## 能做什么

- 生成可被面板启动器发现的 App 包（`launcher.json` + `scripts/` + `assets/`）；
- 中文 UI：收集文案、生成 TTF 子集、校验缺字；
- 海报式分层画面：全屏 JPEG、透明 ARGB 精灵、LVGL UI、局部 EAF 动效；
- **FAT 或 SD 整包部署**：`/fatfs/claw/data/apps/<id>/` 或 `/sdcard/apps/<id>/`，
  SD 包原地启动、可自带 TTF；无卡时 SD 应用不出现在首页；
- **SD 运行期素材后备**：高清图/动画/音频放 `/sdcard/panel-assets/<id>/`，无卡自动降级；
- **网络 App**：`net`（TCP/UDP/DNS/HTTP）、`mqtt`、`websocket` 三个 Lua 模块，
  `next_event()` 事件循环、socket 预算、Lua `http_server` LAN 路由；
- 包/素材校验、FAT 预算检查、LAN 上传、SD 素材上传与回读校验。

## 不做什么

- 不改固件、驱动、分区表；
- 不做设备侧 ESP-Claw Skill（那是 `components/*/skills` 的 JSON frontmatter 格式）；
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
# 1) 校验 App 包
python scripts/validate_app.py --app-dir <pkg> --remote-id <id>

# 2) 中文字体
python scripts/make_cjk_subset.py --source-font NotoSerifSC-Medium.ttf \
  --lua <pkg>/scripts/main.lua --output <pkg>/assets/ui_cn.ttf

# 3) 生成 SD 素材 + 同尺寸 FAT fallback + sd_manifest.json
python scripts/prepare_sd_assets.py --spec <sd_plan.json>

# 4) 看设备容量/SD 状态（新固件）
python scripts/probe_device.py --host http://<device-ip>

# 5) 上传 FAT 包并重启
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg> --restart

# 5b) 或者把整包部署到 SD（无需 FAT 预算，无卡时不出现在首页）
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg> --volume sd --restart

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
├── scripts/             # 8 个黑盒 CLI
├── evals/               # 3 个真实 prompt + 触发边界
└── assets/examples/
    ├── countdown/       # FAT-only 中文倒计时
    ├── mqtt_panel/      # MQTT 网络示例（心跳 + 指令）
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
- Lua `http_server` 的 `/lua/<app_id>/*` 与 `/api/lua/<app_id>/*` 路由
  （`CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER=y`），用于让 App 被 LAN 访问。

没有这些端点时：

- `probe_device.py` 会提示旧固件；
- `upload_app.py --volume sd`、`upload_sd_assets.py` 会失败并给出手动拷卡步骤；
- FAT 包上传、App 运行、设计流程完全不受影响。

## SD 约束（重要）

- 卡必须在**开机前**插入；没有热插拔；
- 只支持 FAT（不是 exFAT），`max_files=5`；
- 文件名用 ASCII；SD 簇 64 KB；
- SD 应用在 `/sdcard/apps/<id>/` 原地启动；同 id 的 FAT 包优先，SD 副本被忽略；
- `lvgl.font_load` 支持绝对 `/sdcard/...`（本固件已注册 SD 字体根），TTF 可随包放 SD；
  旧固件只认 FAT，用 `D:apps/<id>/...` 且字体留在 FAT；
- `lvgl.image{src}`/`lvgl.eaf{src}` 仍不能读 SD，用 `image.load_file`/`src_data`；
- SD 图与 FAT fallback 必须同宽高（运行时无缩放 API）；
- 上传 SD 素材后重开 App 即可；上传/更新 App 包后需要重启设备，launcher 才刷新。

## 安全

Web API 没有鉴权。`/api/sd-assets` 被限制在 `/sdcard/panel-assets`，但
`/api/sd-files` 可以写整张卡；两者都只应在可信 LAN 使用。App Lua 可以读 SD、
使用 `io`/`os`，只安装可信包。

## 许可

示例字体来自 Noto Serif SC（SIL OFL 1.1）；示例背景/符号为原创生成素材。
见 `assets/examples/NOTICE.md` 与 `assets/examples/OFL.txt`。
