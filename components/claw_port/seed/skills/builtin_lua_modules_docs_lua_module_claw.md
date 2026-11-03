# Lua Claw Runtime

`claw` is registered by the ESP-Claw runtime. It is available as a global and
via `require("claw")`. It provides task logging, timing, cancellation and the
script `args` copy, plus small file/JSON/panel helpers.

## Functions

- `claw.log(msg, ...)` - append a line to the task log.
- `claw.sleep(ms)` - sleep 0-5000 ms; returns early when the job is stopped or
  the timeout fires.
- `claw.time()` - Unix timestamp in seconds; `claw.uptime_ms()` - device uptime
  in milliseconds.
- `claw.progress(pct, msg)` - update the task list/progress (0-100).
- `claw.abort_requested()` - true after the launcher or user stops the job.
  Poll it in long loops.
- `claw.get_args()` - a copy of the `args` global passed by the launcher.
- `claw.file.read(path)` - read a workspace-relative text file (<=64 KiB;
  the DATA root is searched first, then the SYSTEM root).
- `claw.file.write(path, text)` - write a DATA-root file (<=16 KiB per call).
- `claw.file.exists(path)`, `claw.file.list(dir)`, `claw.file.remove(path)`.
- `claw.json.encode(value)` / `claw.json.decode(string)`.
- `claw.panel.set_volume(pct)`, `claw.panel.set_brightness(pct)`,
  `claw.panel.status()`, `claw.panel.show_message(text)`,
  `claw.panel.demo(on)` - return `nil, err` when the panel hook is not
  registered.
- `claw.speak(text[, timeout_ms])` - synthesize text with Doubao TTS and play
  it through the ES8311. See **TTS / Voice** below.
- `claw.ask(prompt[, timeout_ms])` - ask the LOCAL agent (小爪) and wait for
  its reply. See **Local agent (小爪)** below.
- `claw.agent_task(prompt[, name[, timeout_ms]])` - start a background 小爪 run
  and return its `task_id` immediately.
- `claw.agent_task_output(task_id)` / `claw.agent_task_stop(task_id[, wait_ms])`
  - poll / cancel that background run.

Paths passed to `claw.file.*` are relative to the workspace (for example
`scripts/hello.lua`); absolute paths, `..` and backslashes are rejected.

## TTS / Voice

`claw.speak(text[, timeout_ms])` is available in Claw Lua scripts and launcher
Apps (`require("claw")` or the global `claw`).

- It is an **explicit** call: the “自动播报” switch only controls automatic
  playback of Agent replies. `claw.speak` still speaks when auto-speak is off.
- Requires Doubao credentials configured in **Claw → 设置 → 豆包语音**
  (or `POST /api/tts/config`). Do not put API keys in App files.
- Blocks the Lua task until playback ends. Default 120 s, maximum 600 s; use
  short phrases for UI feedback and do not call it in a tight loop.
- Speech is synthesized in one streaming session. Emoji/pictographs are
  stripped before synthesis, so text containing 👋/😊 still plays.
- Returns `true` on success, or `nil, err` when not configured, the audio path
  is busy, or synthesis fails.

```lua
local claw = require("claw")

local ok, err = claw.speak("温度 26 度，湿度 48%", 30000)
if not ok then
  claw.log("TTS failed: " .. tostring(err))
end
```

## Local agent (小爪)

These calls talk to the on-device LOCAL agent (小爪), the LLM configured on
the Claw page. They return `nil, err` when the Agent switch or LLM config is
missing (e.g. "本地 Agent 未开启"), and "小爪正在处理上一个请求，请稍后再试。"
when the agent is busy. When 自动播报 is on, a `claw.ask()` reply is spoken
like a panel chat reply.

- `claw.ask(prompt[, timeout_ms])` - synchronous question or short task.
  **Blocks the Lua job task** until the reply or the timeout (default 30 s,
  max 120 s; larger values are capped). Returns the reply text, or `nil, err`.
  Do not call it in tight loops or from animation/timer callbacks.
- `claw.agent_task(prompt[, name[, timeout_ms]])` - start a background run and
  return its `task_id` (e.g. `agent-3`) immediately. Only one agent task runs
  at a time; a running one rejects new starts. `timeout_ms` 0 = agent default
  (3 min).
- `claw.agent_task_output(task_id)` - poll the final output; returns
  `"Agent task agent-N is running."` while it runs, the result text when
  done, or `nil, err` for an unknown id.
- `claw.agent_task_stop(task_id[, wait_ms])` - request cancellation (default
  wait 1500 ms, max 5000 ms); returns a JSON string with the task state.

```lua
local claw = require("claw")

local reply = claw.ask("现在设备温度是多少？", 30000)
if reply then
  claw.log("小爪回复: " .. reply)
end

local task_id = claw.agent_task("把这份数据总结成三句话", "数据总结")
if task_id then
  claw.log("task started: " .. task_id)
end
```

## Example

```lua
local claw = require("claw")   -- the global works too

claw.log("job start")
for i = 1, 10 do
  if claw.abort_requested() then break end
  claw.progress(i * 10, "working")
  claw.sleep(100)
end
claw.progress(100, "done")
```
