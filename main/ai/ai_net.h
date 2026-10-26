#pragma once

#include <functional>

#include "network_interface.h"

/* 小智网络适配：复用 78/esp-ml307 的 EspNetwork，监听 Wi-Fi/以太网 IP 事件。 */
NetworkInterface *ai_net_get(void);
void ai_net_start(void);
bool ai_net_is_online(void);
bool ai_net_wait_online(int timeout_ms);
void ai_net_set_callback(std::function<void(bool online)> callback);
