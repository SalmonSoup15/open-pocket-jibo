#pragma once
#include <Arduino.h>

bool audio_init();

void audio_start_record();
void audio_stop_record();
bool audio_is_recording();

const int16_t *audio_get_pcm();
size_t         audio_get_pcm_bytes();

void audio_play_pcm(const int16_t *data, size_t numBytes);
void audio_stop_play();
bool audio_is_playing();
bool audio_output_started();
float audio_get_output_amplitude();

// Progressive playback: play from PSRAM buffer while it's being filled.
// The play task will wait for an adaptive prebuf window before starting the
// PA, sized so the download is guaranteed to stay ahead of playback for the
// whole stream.  See prog_play_task for the algorithm.
void audio_play_progressive(uint8_t *buf);
void audio_progressive_update(size_t bytesAvailable);
void audio_progressive_finish();

// Hint the expected total byte count for adaptive prebuf sizing.  Pass 0 for
// unknown.  Should be called between audio_play_progressive() and the first
// audio_progressive_update() if the total is learned after playback starts
// (e.g., from a Content-Length header that arrives after the play task has
// already been spawned).  Without this hint the prebuf falls back to a
// rate-only heuristic which is more conservative.
void audio_progressive_set_total(size_t totalBytes);

// Streaming playback: feed PCM chunks as they arrive from network
bool audio_stream_start();
bool audio_stream_write(const uint8_t *data, size_t len);
void audio_stream_finish();
void audio_stream_cancel();

// Low-power helpers — called from STATE_SLEEP transitions.  Stops every
// active audio task, kills the PA, and mutes the codec so the analog
// output stage parks at the lowest quiescent current the ES8311 supports.
// Wake re-enables the codec; tasks restart on demand from their callers.
void audio_enter_low_power();
void audio_exit_low_power();
