#pragma once
#include <Arduino.h>

// ──────────────────────────────────────────────────────────────────────────
//  Event Log — RTC_NOINIT ring buffer
//
//  Records timestamped events continuously into RTC_NOINIT memory so they
//  survive software resets (panic, watchdog, brownout).  After a crash the
//  "whathappened" serial command can dump the full pre-crash event trail
//  while the crash screen is displayed.
//
//  Memory budget: 6432 bytes (32-byte header + 100 x 64-byte entries).
//  Combined with CrashRecord (~260 bytes) the total RTC_NOINIT usage is
//  ~6692 bytes, well within the ESP32-S3's ~8 KB RTC FAST limit.
// ──────────────────────────────────────────────────────────────────────────

// Event types for the ring buffer
enum EventType : uint8_t {
    EVT_STATE_CHANGE    = 1,   // state machine transition
    EVT_USER_TAP        = 2,   // screen tap (x, y)
    EVT_BTN_PRESS       = 3,   // physical button press (short/long)
    EVT_WIFI_EVENT      = 4,   // connect/disconnect/scan
    EVT_API_CALL        = 5,   // Gemini request start/end
    EVT_AUDIO_EVENT     = 6,   // record start/stop, playback start/stop
    EVT_BLE_EVENT       = 7,   // BLE connect/disconnect/command
    EVT_MEMORY_SNAP     = 8,   // periodic heap/psram snapshot
    EVT_ERROR           = 9,   // error condition
    EVT_CUSTOM          = 10,  // generic text event
};

// Initialize the event log (call early in setup, after crash_handler_init)
void event_log_init();

// Log an event — msg is truncated to fit. Thread-safe via critical section.
void event_log(EventType type, const char *msg);
void event_log_printf(EventType type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Check if the log has valid data from a previous boot
bool event_log_has_data();

// Get the number of events in the log
uint16_t event_log_count();

// Dump the entire event log to a serial-friendly report string.
// Caller must free() the returned buffer. Returns NULL on failure.
// The report includes timestamps, event types, and messages, plus
// CrashRecord data (reason, state, heap, backtrace) when available.
char *event_log_dump_report();

// Returns a preservation status for display on crash screen:
// 0 = no log data, 1 = fully preserved, 2 = partially preserved (overflow)
uint8_t event_log_preservation_status();

// Clear the event log (call after whathappened dumps it, or when crash
// screen is dismissed)
void event_log_clear();
