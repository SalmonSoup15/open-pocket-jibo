#pragma once
#include <Arduino.h>

// ──────────────────────────────────────────────────────────────────────────
//  Developer Console
//
//  Hidden BLE-side debug bridge.  Mirrors all LOG*() / printf-style output
//  to a ring buffer, which is flushed over BLE_OP_DEV_LOG when the phone
//  has explicitly enabled developer mode.  Also accepts commands from the
//  phone (BLE_OP_DEV_CMD) and routes them through the same parser used
//  by the USB serial console.
//
//  Activation: phone-side only (10 taps on the Jibo "J" icon).  Firmware
//  starts in disabled state — zero overhead until the phone toggles it.
// ──────────────────────────────────────────────────────────────────────────

void dev_console_init();

// Master dev-mode gate.  When false (stock firmware), ALL serial output,
// log macros, serial commands, and BLE dev opcodes are silenced.
// Cached in RAM at boot from storage_get_dev_mode() for zero-overhead
// checks on every LOG* invocation.
extern bool g_dev_mode;

// Toggle BLE forwarding.  When disabled, dev_console_printf() falls
// through to plain Serial.printf with no buffering or BLE traffic.
void dev_console_set_enabled(bool en);
bool dev_console_enabled();

// printf-style writer used by the LOG* macros.  Always writes to USB
// Serial; additionally appends to the BLE ring buffer when enabled.
int  dev_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Plain string variants — convenience for non-formatted lines.
void dev_console_print(const char *s);
void dev_console_println(const char *s);

// Run a single-line command string (no trailing newline) through the
// USB-serial command parser.  The parser's output flows back through
// dev_console_printf so the requesting phone receives the response
// over BLE_OP_DEV_LOG.
//
// Defined in Jibo-ESP.ino — declared here so dev_console.cpp can call it
// from the BLE_OP_DEV_CMD handler.
void serial_handle_command(const String &cmd);

// Flush any pending log bytes via BLE_OP_DEV_LOG and, every ~1 s,
// publish a BLE_OP_DEV_STATS dashboard snapshot.  Call from the main
// loop at any frequency; internal rate-limiting handles pacing.
void dev_console_tick();

// Called from ble_link.cpp on incoming BLE_OP_DEV_CMD.
void dev_console_on_ble_cmd(const uint8_t *data, size_t len);
