#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class Es8311AudioCodec;

Es8311AudioCodec *ai_audio_codec(void);
void ai_audio_set_play_pcm_callback(std::function<bool(std::vector<int16_t> &&)> callback);
void ai_audio_set_play_ogg_callback(std::function<bool(const uint8_t *, size_t)> callback);
void ai_audio_set_input_callbacks(std::function<bool(size_t)> open_cb,
                                  std::function<size_t(int16_t *, size_t, uint32_t)> read_cb,
                                  std::function<void()> close_cb);
void ai_audio_set_playback_callbacks(std::function<bool()> idle_cb,
                                     std::function<void()> stop_cb);
