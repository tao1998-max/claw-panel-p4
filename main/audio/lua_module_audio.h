#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_audio(lua_State *L);

/* Stops MP3 playback owned by the exiting Lua job and restores the assistant
 * audio path. Registered once via cap_lua_register_exit_cleanup(). */
void lua_audio_exit_cleanup(lua_State *L);

#ifdef __cplusplus
}
#endif
