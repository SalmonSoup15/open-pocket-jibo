#pragma once
#include <Arduino.h>

// Call early in setup() to snapshot the reset reason before anything
// overwrites it.  Does NOT need the display or LVGL.
void crash_handler_init();

// Call from enter_state() so the crash report always knows what the
// device was doing when it died.
void crash_handler_update_state(const char *stateName);

// Stamp the RTC crash record with a message and backtrace right now.
// Call this before a deliberate crash (assert, test command, etc.) so
// the crash screen can show the details.  For unhandled panics the
// continuously-updated state/heap/uptime fields are all that's available.
void crash_handler_save(const char *msg);

// Returns true if the previous boot ended in a crash.  Valid any time
// after crash_handler_init().
bool crash_handler_has_crash();

// Show a full-screen crash report via LVGL.  Blocks (pumps lv_timer)
// until the user presses the boot button, then clears the stored crash
// and returns so the normal boot can continue.
void crash_handler_show(uint8_t bootBtnPin);

// Force the stock (normal-user) crash screen on next show, even in dev mode.
// Resets after the screen is displayed.
void crash_handler_force_stock(bool force);

// Call immediately before any intentional esp_restart() so the crash
// handler knows it was a clean reboot, not an uncontrolled panic.
void crash_handler_pre_restart();

// Wipe stored crash data (called automatically by _show, or manually).
void crash_handler_clear();
