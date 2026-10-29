#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the self.claw.* MCP tools with the XiaoZhi McpServer.
 * Called from AiApp::Start() after AddCommonTools(); safe to call when Claw is
 * disabled (every tool then reports that the runtime is unavailable).
 */
void claw_mcp_bridge_register(void);

#ifdef __cplusplus
}
#endif
