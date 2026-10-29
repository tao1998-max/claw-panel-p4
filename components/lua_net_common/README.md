# lua_net_common

Internal support layer shared by the Lua network modules (`lua_module_net`,
`lua_module_mqtt`, `lua_module_websocket`). It does not register a Lua module
of its own.

## Contents

`include/lua_net_common.h` + `src/lua_net_common.c`:

- **Socket budget**: per-job and global socket/TLS counters keyed by
  `cap_lua_runtime_job_id()`. Caps: 3 sockets and 1 TLS connection per job,
  4 sockets and 2 TLS connections globally. `lua_net_budget_acquire()` returns
  `false` with a stable reason (`"socket budget exhausted (job|global)"`,
  `"tls budget exhausted ..."`); release is idempotent.
- **Deadline helpers**: `lua_net_deadline(L, timeout_ms)` folds the Lua job's
  own deadline (`cap_lua_runtime_deadline_us`) into the call deadline;
  `lua_net_remaining_ms()` clamps to `INT32_MAX`.
- **Abortable wait**: `lua_net_wait_fd()` uses `select()` in 50 ms slices and
  checks `cap_lua_runtime_stop_requested()` between slices
  (1 ready / 0 deadline / -1 stopped / -2 select error).

## Why a separate component

`net`, `mqtt` and `websocket` are independent components, but they must share
one accounting of LWIP sockets so a script cannot open three MQTT clients plus
raw sockets and starve the Web UI or the XiaoZhi voice link. Keeping the
accounting in one tiny component avoids a dependency between the modules.

No Lua state lives here and nothing blocks for longer than a slice.
