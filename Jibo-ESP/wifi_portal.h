#pragma once
#include <Arduino.h>

// Original first-boot portal: asks for WiFi + AI keys, marks save_received
// when the user hits Save (caller stops the portal and proceeds to connect).
void     portal_start(const String &apName, const String &apPass);
void     portal_stop();
void     portal_tick();
bool     portal_save_received();

// Settings relaunch portal: same WiFi / API key form, but pre-fills with the
// saved values so the user can edit-in-place, and stays running until the
// caller explicitly stops it (the page shows an inline "Saved" banner on
// /save instead of routing to a goodbye page).  Returns true once the user
// has hit the in-page Exit button so the caller can leave the sub-page.
void     portal_start_settings(const String &apName, const String &apPass);
bool     portal_exit_requested();

// Drive the WiFi STA state machine.  wifi_start_connect() kicks off an
// async pre-scan, then connects to the strongest *known + in-range*
// network (preferring the strongest BSSID for that SSID — useful for
// mesh networks).  If the scan returns no known matches we still fall
// through to a sequential attempt over the saved list so a temporary
// scan failure can't lock us out of a network the radio could actually
// reach.  wifi_connect_tick() must be called from the main loop while
// connecting AND while connected (the periodic roam/mesh check piggy-
// backs on it).
void     wifi_start_connect();
void     wifi_connect_tick();
bool     wifi_is_connected();
bool     wifi_connect_failed();
uint8_t  wifi_connect_error();   // 0=none, 1=auth fail, 2=AP not found, 3=timeout
void     wifi_disconnect();

void     wifi_enable();
void     wifi_disable();
bool     wifi_is_enabled();
String   wifi_current_ssid();
void     wifi_connect_to(uint8_t idx);

// Boot-time hint: kick off an async scan as early as possible so by the
// time wifi_start_connect() runs the result is already available and we
// can pick the strongest known network without paying the scan latency.
// Safe (and a no-op) if WiFi isn't yet initialized — re-tries on the
// first wifi_start_connect() instead.
void     wifi_boot_prescan();

struct WifiScanResult {
    String  ssid;
    int32_t rssi;
    bool    secured;
    bool    enterprise;
};

// Start an async scan.  Returns true if the scan was queued successfully —
// false means the radio refused (typically because it's mid-connect or in
// an unrecoverable state).  When the scan is started for the settings UI
// and we're currently auto-reconnecting to a saved network, this also
// pauses that reconnect (call wifi_resume_connect() when the user leaves
// the scan page so we keep trying the saved network in the background).
bool            wifi_scan_start();
int             wifi_scan_status();
WifiScanResult  wifi_scan_result(int i);   // raw, per-BSSID
void            wifi_scan_cleanup();
void            wifi_resume_connect();

// Same scan results, but collapsed by SSID — multiple BSSIDs of the
// same network (mesh / repeater scenarios) appear as one row, with the
// rssi field set to the strongest BSSID's rssi.  Used by the settings
// "Add Network" UI so a 5-AP mesh shows up as 1 row instead of 5.
int             wifi_scan_count_unique();
WifiScanResult  wifi_scan_result_unique(int i);
