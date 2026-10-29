/*
 * The only Lua global exposed to sandboxed scripts: `claw`.
 *
 *   claw.log(...), claw.time(), claw.uptime_ms(), claw.sleep(ms)
 *   claw.progress(pct, msg), claw.abort_requested(), claw.get_args()
 *   claw.file.read/write/list/exists/remove   (sandboxed under claw_paths)
 *   claw.json.encode/decode
 *   claw.panel.set_volume/set_brightness/status/show_message/demo
 */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_claw(lua_State *L);

#ifdef __cplusplus
}
#endif
