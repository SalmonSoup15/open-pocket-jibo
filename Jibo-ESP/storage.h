#pragma once
#include <Arduino.h>

#define MAX_WIFI_NETWORKS 5

struct WifiEntry {
    String ssid;
    String password;
    String username;
    bool   enterprise;
};

void     storage_init();
void     storage_factory_reset();

uint8_t  storage_wifi_count();
bool     storage_get_wifi(uint8_t idx, WifiEntry &out);
void     storage_set_wifi(uint8_t idx, const WifiEntry &entry);
void     storage_set_wifi_count(uint8_t count);

String   storage_get_api_key();
void     storage_set_api_key(const String &key);

String   storage_get_tts_key();
void     storage_set_tts_key(const String &key);

bool     storage_is_setup_done();
void     storage_set_setup_done(bool done);

uint16_t storage_device_suffix();

// Settings
uint8_t  storage_get_brightness();      // 0=low, 1=med, 2=high (default 2)
void     storage_set_brightness(uint8_t level);

uint8_t  storage_get_model();           // 0=fast (flash-lite), 1=smart (flash)
void     storage_set_model(uint8_t m);

uint8_t  storage_get_voice();           // index into voice list (default 0 = Odysseus)
void     storage_set_voice(uint8_t v);

bool     storage_get_memory_enabled();  // conversation memory on/off (default true)
void     storage_set_memory_enabled(bool on);
String   storage_get_conv_history();    // serialised conversation turns
void     storage_set_conv_history(const String &h);
void     storage_clear_conv_history();

// WiFi on/off
bool     storage_get_wifi_enabled();    // default true
void     storage_set_wifi_enabled(bool on);

// BLE phone link
bool     storage_get_ble_paired();
void     storage_set_ble_paired(bool paired);
String   storage_get_ble_addr();
void     storage_set_ble_addr(const String &addr);
String   storage_get_ble_name();
void     storage_set_ble_name(const String &name);

// Auto-sleep timeout index (0..7, see sleep_timeout_options in states.cpp)
uint8_t  storage_get_sleep_timeout();   // default 4 (2 minutes)
void     storage_set_sleep_timeout(uint8_t idx);

// Developer mode (default false — stock firmware)
bool     storage_get_dev_mode();
void     storage_set_dev_mode(bool on);

// Dev-mode transition flag — set before reboot, cleared after the
// "Transitioning to developer mode" boot screen is shown.
bool     storage_get_dev_transitioning();
void     storage_set_dev_transitioning(bool on);

// ─── Dev tools toggles (only meaningful when dev mode is on) ──────────────
bool     storage_get_dev_verbose_overlay();
void     storage_set_dev_verbose_overlay(bool on);
bool     storage_get_dev_stats_pill();
void     storage_set_dev_stats_pill(bool on);
bool     storage_get_dev_verbose_boot();
void     storage_set_dev_verbose_boot(bool on);

// IMU calibration
bool     storage_get_imu_cal(float &bx, float &by, float &bz);
void     storage_set_imu_cal(float bx, float by, float bz);

// WiFi network management
void     storage_remove_wifi(uint8_t idx);  // remove and compact

// ─── Permanent memory ───────────────────────────────────────────────────────
//
// Long-term, user-curated facts saved by Gemini's [store.memory]{...} tool.
// Firmware NVS is the single source of truth; the Android app polls the
// list on demand instead of keeping a mirror.  This removes the whole class
// of bidirectional-sync bugs (tombstone spam, ping-pong reconnects, etc.)
// that the previous design fought with.
//
// Cap: PERM_MEM_MAX entries, ~PERM_MEM_MAX_BYTES total payload.  Adding a
// new entry past the cap drops the oldest.
//
// Persistence layout (NVS blob "pm_blob"):
//   uint16 count
//   for each: uint64 id  uint64 ts_ms  uint16 text_len  bytes[text_len]

#define PERM_MEM_MAX            100
#define PERM_MEM_MAX_BYTES      8192
#define PERM_MEM_TEXT_MAX       240

struct PermMemoryEntry {
    uint64_t id;
    uint64_t ts_ms;
    String   text;
};

uint16_t storage_perm_mem_count();
bool     storage_perm_mem_get(uint16_t idx, PermMemoryEntry &out);
// Add or upsert by id.  If id already exists, the text/ts are updated.
// Returns false if rejected (e.g. text empty / oversize).
bool     storage_perm_mem_add(uint64_t id, uint64_t ts_ms, const String &text);
// Delete an entry by id.  Returns true if an entry was actually removed.
// No tombstone — the phone is a passive viewer, so there's nothing to
// resurrect a delete from on the next reconnect.
bool     storage_perm_mem_remove(uint64_t id);
void     storage_perm_mem_clear();
size_t   storage_perm_mem_total_bytes();   // approximate stored size
