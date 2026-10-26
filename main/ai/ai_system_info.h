#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 稳定的设备身份：P4 基片 MAC + 持久 UUID。 */
const char *ai_device_id(void);
const char *ai_device_uuid(void);
const char *ai_user_agent(void);

/* 返回 malloc 的 JSON 字符串，调用方用 ai_system_info_free() 释放。 */
char *ai_system_info_json(void);
void ai_system_info_free(char *json);

#ifdef __cplusplus
}
#endif
