#pragma once

#include <functional>
#include <vector>

class Es8311AudioCodec;

Es8311AudioCodec *ai_audio_codec(void);
void ai_audio_set_play_pcm_callback(std::function<bool(std::vector<int16_t> &&)> callback);
