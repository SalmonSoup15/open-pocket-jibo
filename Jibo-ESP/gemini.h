#pragma once
#include <Arduino.h>

void   gemini_init();

// Install a custom mbedtls calloc/free hook that routes BIG allocations
// (>= 4 KB) to PSRAM.  This is the actual fix for the recurring
// MBEDTLS_ERR_SSL_ALLOC_FAILED — without it, mbedtls's 16 KB rx + 16 KB tx
// record buffers can't be allocated when internal DRAM is fragmented to
// its baseline ~26-32 KB largest contiguous block, which it always is
// after WiFi/BLE/lwip have done their initial allocations.  Should be
// called once at boot (from setup()), before any TLS connection is made.
// Safe to call before WiFi.begin().
void   gemini_install_mbedtls_psram_alloc();
void   gemini_send_audio(const int16_t *pcm, size_t numBytes,
                        const String &apiKey, const String &ttsKey);
void   gemini_send_text(const String &text,
                        const String &apiKey, const String &ttsKey);
void   gemini_cancel();
bool   gemini_is_busy();
bool   gemini_is_done();
bool   gemini_is_error();
bool   gemini_response_ready();
String gemini_get_response();

// Last error context for UI display.  Set whenever gError flips true.
//   subsystem: "Gemini" / "TTS" / "Network" — short, user-facing
//   detail:    one-liner like "HTTP 403", "no audio in response",
//              "alloc failed".  Bounded length, safe to display in a
//              pill subtext.
const char *gemini_last_error_subsystem();
const char *gemini_last_error_detail();

bool           gemini_has_audio();
void           gemini_set_early_response(const String &resp);
const int16_t *gemini_get_audio_pcm();
size_t         gemini_get_audio_bytes();

void   gemini_clear();
void   gemini_test_tts(const String &text);

// ─── On-screen tool invocations ─────────────────────────────────────────────
//
// Gemini may prefix its response with one or more tool calls of the form
//     [tool.name]{ ... brace-balanced args ... }
// at the very start.  We strip these from the text before sending to TTS so
// the AI doesn't read out the tool syntax — gemini_get_response() returns
// the speak-only text, while gemini_get_tool_invocation() returns the raw
// tool string ("tool.name:args", or "" if none).  Currently the only
// supported tool is `show.text` which renders LaTeX-formatted math on the
// display.
String gemini_get_tool_invocation();

// One-turn volatile memory: kept in RAM even if persistent memory is OFF, so
// natural follow-ups ("yes, read it") still have prior context.  Clear whenever
// the persistent history is cleared (settings → memory → clear, or via serial).
void   gemini_clear_volatile_turn();
