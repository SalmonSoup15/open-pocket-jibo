// ─────────────────────────────────────────────────────────────────────────────
// time_sync — wall-clock time + IP-based timezone lookup
//
// On the first WiFi connection after boot we kick off two pieces of work
// in the background:
//
//   1. SNTP against pool.ntp.org so `time(NULL)` returns real epoch
//      seconds.  This makes timestamps stored alongside permanent
//      memories actually correspond to wall-clock time instead of
//      uptime-ms-since-boot (which the Android app interprets as 1970-
//      ish, displayed as "685 months ago").
//
//   2. A one-shot HTTP GET to http://ip-api.com/json which gives us
//      the device's timezone name + UTC offset based on its public IP.
//      We persist this in NVS so subsequent boots skip the lookup and
//      just call `setenv("TZ", ...)` from cache.
//
// All of the work is non-blocking: the wifi-connected hook spawns a
// short-lived FreeRTOS task and returns immediately.  Callers that want
// the current time just call `time_sync_now_epoch_ms()`; if SNTP hasn't
// landed yet (typical for the first ~1-3 seconds after WiFi up) it
// returns 0 to signal "unknown" and the caller can fall back gracefully.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>

// Call once at boot from setup() — registers NVS namespace, restores any
// cached TZ string, but does NOT initiate the network sync (no radio yet).
void time_sync_init();

// Call from the wifi_connect_tick when WL_CONNECTED is first observed.
// Idempotent — safe to call on every reconnect; only the first call per
// boot kicks off the geolocation lookup.
void time_sync_on_wifi_connected();

// Real epoch ms, or 0 if SNTP hasn't completed yet (first ~1-3 s after
// WiFi up, or no WiFi at all).
uint64_t time_sync_now_epoch_ms();

// True once SNTP has returned a valid timestamp (system clock is past
// 2020).  Useful for "are we good to record real timestamps yet" gates.
bool time_sync_clock_ok();

// Human-readable local time string for the Gemini system prompt, e.g.
// "Sunday, May 3 2026, 1:31 PM PDT".  Returns empty string until SNTP
// has landed AND the TZ has been resolved.
String time_sync_local_str();

// Short timezone name (e.g. "America/Los_Angeles") if known, "" otherwise.
String time_sync_tz_name();
