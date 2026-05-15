#pragma once
#include <Arduino.h>

// Log levels:  0 = silent (boot only)
//              1 = minimal (errors, state changes, key events)
//              2 = normal  (default — API calls, audio stats, download summaries)
//              3 = verbose (per-chunk download, playback position, deep diagnostics)

extern volatile uint8_t jibo_log_level;

// Master dev-mode gate — defined in dev_console.cpp, set at boot from NVS.
// When false (stock firmware), ALL log output is suppressed.
extern bool g_dev_mode;

// All log output flows through the dev-console layer (see
// dev_console.h).  When developer mode is OFF, dev_console_printf is
// a no-op — zero serial, zero BLE, zero overhead.  When ON, it writes
// to USB serial and optionally queues for BLE forwarding.
int  dev_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dev_console_println(const char *s);

// The g_dev_mode check short-circuits before the function call when in
// stock mode, saving the vsnprintf overhead on every hot LOG* site.
#define LOG1(fmt, ...) do { if (g_dev_mode && jibo_log_level >= 1) dev_console_printf(fmt, ##__VA_ARGS__); } while(0)
#define LOG2(fmt, ...) do { if (g_dev_mode && jibo_log_level >= 2) dev_console_printf(fmt, ##__VA_ARGS__); } while(0)
#define LOG3(fmt, ...) do { if (g_dev_mode && jibo_log_level >= 3) dev_console_printf(fmt, ##__VA_ARGS__); } while(0)

#define LOGLN1(msg) do { if (g_dev_mode && jibo_log_level >= 1) dev_console_println(msg); } while(0)
#define LOGLN2(msg) do { if (g_dev_mode && jibo_log_level >= 2) dev_console_println(msg); } while(0)
#define LOGLN3(msg) do { if (g_dev_mode && jibo_log_level >= 3) dev_console_println(msg); } while(0)
