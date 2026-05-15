#include "settings_ui.h"
#include <Arduino.h>
#include <WiFi.h>
#include "ui_helpers.h"
#include "states.h"
#include "eye.h"
#include "pin_config.h"
#include "storage.h"
#include "wifi_portal.h"
#include "ble_link.h"
#include "imu.h"
#include "gemini.h"
#include "dev_console.h"   // g_dev_mode
#include "version.h"
#include "log.h"

// ─── Functions from Jibo-ESP.ino ────────────────────────────────────────────────
extern void set_display_brightness(uint8_t level);
extern int  wifi_scan_count();
extern const char *wifi_scan_ssid(int idx);
extern int  wifi_scan_rssi(int idx);
extern bool wifi_scan_open(int idx);
extern void wifi_scan_cleanup();

// ─── Settings data ──────────────────────────────────────────────────────────
static const char *VOICE_NAMES[] = {
    "Odysseus", "Apollo", "Arcas", "Aries", "Atlas", "Hermes"
};
static const int NUM_VOICES = 6;

// ─── Settings UI widgets ────────────────────────────────────────────────────
static lv_obj_t *settingsContainer = NULL;
static lv_obj_t *settSelContainer  = NULL;
static uint8_t   settSubPage       = 0;     // 0=main, 1=brightness, 2=model, 3=voice, 4=memory
static int       settPendingPage   = -1;    // pending sub-page transition

// ─── Settings option callback data ──────────────────────────────────────────
struct SettOptData { uint8_t page; uint8_t index; };
static SettOptData settOptPool[12];

// ─── Touch helpers (see ui_helpers.h for shared touch utilities) ────────────

// Blue text on press for main settings rows
static void sett_row_press_cb(lv_event_t *e) {
    tap_press_cb(e);   // record tap position for is_valid_tap()
    lv_obj_t *row = lv_event_get_target(e);
    lv_color_t blue = lv_color_make(60, 160, 255);
    uint32_t cnt = lv_obj_get_child_cnt(row);
    for (uint32_t i = 0; i < cnt; i++)
        lv_obj_set_style_text_color(lv_obj_get_child(row, i), blue, 0);
}

static void sett_row_release_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    uint32_t cnt = lv_obj_get_child_cnt(row);
    if (cnt >= 1) lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_white(), 0);
    if (cnt >= 2) lv_obj_set_style_text_color(lv_obj_get_child(row, 1), lv_color_make(140, 140, 140), 0);
}

// ─── SETTINGS ───────────────────────────────────────────────────────────────

static lv_obj_t *wifiNetStatus[MAX_WIFI_NETWORKS] = {};
static int8_t    wifiConnectingIdx = -1;
static lv_obj_t *wifiToggleVal = NULL;

static void destroy_settings() {
    if (settingsContainer) { lv_obj_del(settingsContainer); settingsContainer = NULL; }
}

// Forward declarations for scan/T9/connect UI pointers (used by destroy_sett_selection)
static lv_obj_t *scanSpinner       = NULL;
static lv_obj_t *scanStatusLbl     = NULL;
static lv_obj_t *t9Container       = NULL;
static lv_obj_t *t9Display         = NULL;
static lv_obj_t *connectStatusLbl  = NULL;
static lv_obj_t *connectSpinnerObj = NULL;

// About-page eye sphere (used by build_about_page & cleaned up here)
static lv_obj_t *aboutEyeCanvas  = NULL;
static uint8_t  *aboutEyeBuf     = NULL;
static const int16_t ABOUT_EYE_SIZE = 36;

static void destroy_sett_selection() {
    if (settSelContainer) { lv_obj_del(settSelContainer); settSelContainer = NULL; }
    memset(wifiNetStatus, 0, sizeof(wifiNetStatus));
    wifiToggleVal = NULL;
    wifiConnectingIdx = -1;
    scanSpinner = NULL;
    scanStatusLbl = NULL;
    t9Container = NULL;
    t9Display = NULL;
    connectStatusLbl = NULL;
    connectSpinnerObj = NULL;
    // About page eye canvas buffer (canvas obj is already gone with settSelContainer)
    aboutEyeCanvas = NULL;
    if (aboutEyeBuf) { free(aboutEyeBuf); aboutEyeBuf = NULL; }
}

static lv_obj_t *settBrightVal  = NULL;
static lv_obj_t *settModelVal   = NULL;
static lv_obj_t *settVoiceVal   = NULL;
static lv_obj_t *settMemoryVal  = NULL;
static lv_obj_t *settWifiVal    = NULL;
static lv_obj_t *settPhoneVal   = NULL;
static lv_obj_t *settSleepVal   = NULL;

static const char *bright_str(uint8_t b) {
    if (b == 0) return "Low";
    if (b == 1) return "Med";
    return "High";
}

static const char *model_str(uint8_t m) {
    static const char *names[] = {
        "gemini-flash-lite-latest", "gemini-flash-latest", "gemini-pro-latest"
    };
    if (m > 2) m = 0;
    return names[m];
}

static void apply_brightness(uint8_t b) {
    set_display_brightness(b);
}

static void sett_brightness_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 1;
}
static void sett_model_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 2;
}
static void sett_voice_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 3;
}
static void sett_memory_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 4;
}
static void sett_wifi_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 5;
}
static void sett_phone_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 6;
}
static void sett_portal_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 13;
}
static void sett_imu_cal_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 16;
}

// Sleep timer options: index -> seconds (0 = never)
static const uint32_t SLEEP_TIMEOUTS[] = {10, 30, 60, 120, 300, 600, 0};
static const char *SLEEP_TIMEOUT_NAMES[] = {
    "10 seconds", "30 seconds", "1 minute", "2 minutes",
    "5 minutes", "10 minutes", "Never"
};
#define NUM_SLEEP_OPTS 7

static const char *sleep_timeout_str(uint8_t idx) {
    if (idx >= NUM_SLEEP_OPTS) idx = 3;
    return SLEEP_TIMEOUT_NAMES[idx];
}

static void sett_sleep_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 17;
}

static void sett_about_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 18;
}

static void sett_devtools_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    settPendingPage = 19;
}

// Dev tools page toggle value labels
static lv_obj_t *devToolsVerbOvlVal  = NULL;
static lv_obj_t *devToolsStatsPillVal = NULL;
static lv_obj_t *devToolsVerbBootVal  = NULL;

// ─── Phone Link sub-page state ──────────────────────────────────────────────
static lv_obj_t *pairCodeLabel = NULL;
static uint32_t   pairPollMs   = 0;

static void update_selection_colors(lv_obj_t *container, int page, int selected);
static void update_wifi_page_status();
static void build_wifi_page();
static void build_scan_page();
static void build_password_page();
static void build_username_page();
static void build_connecting_page();
static void build_clear_history_confirm();
static void build_setup_portal_page();
static void build_perm_memory_page();
static void build_clear_perm_memory_confirm();
static void build_imu_cal_page();
static void exit_setup_portal();

// Track whether the in-settings captive portal is currently running so
// sett_tick can drive portal_tick() and we can stop the AP cleanly
// when the user navigates away.  Decoupled from settSubPage because we
// need to call portal_stop() from many cleanup paths (sleep, power off,
// long-press reset) and they all funnel through sett_destroy_all().
static bool      settPortalActive = false;
// Restore WiFi STA after the portal exits (the AP toggle takes the radio
// out of STA mode).  We snapshot this at portal start so reconnect logic
// only fires for users who had WiFi on.
static bool      settPortalRestoreWifi = false;

static void sett_option_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    SettOptData *d = (SettOptData *)lv_event_get_user_data(e);
    switch (d->page) {
        case 1: storage_set_brightness(d->index); apply_brightness(d->index); break;
        case 2: storage_set_model(d->index); break;
        case 3: storage_set_voice(d->index); break;
        case 17: storage_set_sleep_timeout(d->index); break;
        case 4:
            if (d->index == 0)      storage_set_memory_enabled(true);
            else if (d->index == 1) storage_set_memory_enabled(false);
            else if (d->index == 2) {
                if (storage_get_conv_history().length() == 0) return;  // disabled
                settPendingPage = 12;
                return;
            } else if (d->index == 3) {
                settPendingPage = 14;
                return;
            }
            break;
        case 12:  // Clear-history confirmation
            if (d->index == 1) {
                storage_clear_conv_history();
                gemini_clear_volatile_turn();
                LOGLN1("[mem] history cleared");
            }
            settPendingPage = 4;
            return;
        case 14:  // Permanent Memory page
            if (d->index == 0) {
                if (storage_perm_mem_count() == 0) return;  // greyed out
                settPendingPage = 15;
                return;
            }
            return;
        case 15:  // Clear-permanent-memory confirmation
            if (d->index == 1) {
                storage_perm_mem_clear();
                LOGLN1("[mem] permanent memory cleared");
            }
            settPendingPage = 14;
            return;
        case 5:  // WiFi sub-page
            if (d->index == 0) {
                bool cur = wifi_is_enabled();
                if (cur) wifi_disable(); else wifi_enable();
                if (wifiToggleVal) {
                    bool now = !cur;
                    lv_color_t blueCol = lv_color_make(60, 160, 255);
                    lv_label_set_text(wifiToggleVal, now ? "On" : "Off");
                    lv_obj_set_style_text_color(wifiToggleVal,
                        now ? blueCol : lv_color_make(140, 140, 140), 0);
                }
            } else if (d->index == 10) {
                settPendingPage = 8;
            } else {
                uint8_t netIdx = d->index - 1;
                wifiConnectingIdx = netIdx;
                wifi_connect_to(netIdx);
                if (netIdx < MAX_WIFI_NETWORKS && wifiNetStatus[netIdx]) {
                    lv_label_set_text(wifiNetStatus[netIdx], "Connecting...");
                }
            }
            return;
        case 7:  // WiFi forget (index = network index)
            storage_remove_wifi(d->index);
            destroy_sett_selection();
            settSubPage = 5;
            build_wifi_page();
            return;
        case 6:  // Phone Link sub-page
            if (d->index == 0) {
                if (ble_is_paired()) {
                    ble_unpair();
                } else if (ble_pairing_active()) {
                    ble_stop_pairing();
                } else {
                    ble_start_pairing();
                }
            }
            settPendingPage = 6;
            return;
    }
    // Pages 1-4, 17: update highlight colors in-place, no fade
    if (settSelContainer && ((d->page >= 1 && d->page <= 4) || d->page == 17)) {
        int cur = -1;
        switch (d->page) {
            case 1: cur = storage_get_brightness(); break;
            case 2: cur = storage_get_model(); break;
            case 3: cur = storage_get_voice(); break;
            case 4: cur = storage_get_memory_enabled() ? 0 : 1; break;
            case 17: cur = storage_get_sleep_timeout(); break;
        }
        update_selection_colors(settSelContainer, d->page, cur);
    }
}

static void update_selection_colors(lv_obj_t *container, int page, int selected) {
    lv_color_t blueCol = lv_color_make(60, 160, 255);
    lv_color_t disabledCol = lv_color_make(80, 80, 80);
    bool memoryEmpty = (page == 4) && (storage_get_conv_history().length() == 0);
    lv_obj_t *list = lv_obj_get_child(container, 1);
    if (!list) return;
    uint32_t cnt = lv_obj_get_child_cnt(list);
    int optIdx = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (lv_obj_get_height(child) < 10) continue;
        lv_obj_t *lbl = lv_obj_get_child(child, 0);
        if (lbl) {
            bool disabled = (page == 4 && optIdx == 2 && memoryEmpty);
            lv_color_t col = disabled ? disabledCol
                                      : ((optIdx == selected) ? blueCol : lv_color_white());
            lv_obj_set_style_text_color(lbl, col, 0);
        }
        optIdx++;
    }
}

static lv_obj_t *create_setting_row(lv_obj_t *parent, const char *label,
                                     const char *value, lv_coord_t y,
                                     lv_event_cb_t cb, lv_obj_t **valOut) {
    if (y > 25) {
        lv_obj_t *line = lv_obj_create(parent);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, UI_SCR_W - 100, 1);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y - 12);
        lv_obj_set_style_bg_color(line, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_size(row, UI_SCR_W - 60, 50);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_add_event_cb(row, sett_row_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(row, sett_row_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(row, sett_row_release_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_color(val, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);

    if (valOut) *valOut = val;
    return row;
}

static const char *wifi_status_str() {
    if (!wifi_is_enabled()) return "Off";
    String ssid = wifi_current_ssid();
    if (ssid.length() > 0) {
        static char buf[24];
        snprintf(buf, sizeof(buf), "%s", ssid.substring(0, 16).c_str());
        return buf;
    }
    return "Not Connected";
}

static const char *phone_status_str() {
    if (ble_is_connected()) return "Connected";
    if (ble_is_paired())    return "Paired";
    return "Not Paired";
}

static void build_settings_main() {
    destroy_settings();

    settingsContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settingsContainer);
    lv_obj_set_size(settingsContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settingsContainer, 0, 0);
    lv_obj_clear_flag(settingsContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(settingsContainer);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 85);

    uint8_t br = storage_get_brightness();
    uint8_t md = storage_get_model();
    uint8_t vc = storage_get_voice();
    bool mem = storage_get_memory_enabled();

    lv_obj_t *list = lv_obj_create(settingsContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W, UI_SCR_H - 120);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    create_setting_row(list, "Wi-Fi", wifi_status_str(),
                       10, sett_wifi_row_cb, &settWifiVal);
    create_setting_row(list, "Phone Link", phone_status_str(),
                       70, sett_phone_row_cb, &settPhoneVal);
    create_setting_row(list, "Brightness", bright_str(br),
                       130, sett_brightness_row_cb, &settBrightVal);
    create_setting_row(list, "Model", model_str(md),
                       190, sett_model_row_cb, &settModelVal);
    create_setting_row(list, "Voice", VOICE_NAMES[vc],
                       250, sett_voice_row_cb, &settVoiceVal);
    create_setting_row(list, "Memory", mem ? "On" : "Off",
                       310, sett_memory_row_cb, &settMemoryVal);
    create_setting_row(list, "Setup Portal", "Launch",
                       370, sett_portal_row_cb, NULL);
    create_setting_row(list, "Calibrate IMU",
                       imu_is_calibrated() ? "Done" : "Not set",
                       430, sett_imu_cal_row_cb, NULL);
    create_setting_row(list, "Sleep Timer",
                       sleep_timeout_str(storage_get_sleep_timeout()),
                       490, sett_sleep_row_cb, &settSleepVal);

    lv_coord_t nextY = 550;
    if (g_dev_mode) {
        create_setting_row(list, "Developer Tools", "",
                           nextY, sett_devtools_row_cb, NULL);
        nextY += 60;
    }

    create_setting_row(list, "About", "",
                       nextY, sett_about_row_cb, NULL);

    // Bottom spacer so last row can scroll into visible area on round screen
    lv_obj_t *spacer = lv_obj_create(list);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 100);
    lv_obj_align(spacer, LV_ALIGN_TOP_MID, 0, nextY + 60);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    block_touch_until_release();
    fade_in_children(settingsContainer, 150);
}

// ─── About page ─────────────────────────────────────────────────────────────
//
// Shows Jibo logo (tappable), serial number, build date.  10 taps on the logo
// reveals the "Dev Mode" button.  In dev mode, shows a "Disable Developer Mode"
// button instead.

static uint8_t   aboutTapCount   = 0;
static uint32_t  aboutLastTapMs  = 0;
static lv_obj_t *aboutDevBtn     = NULL;
static lv_obj_t *aboutHintLabel  = NULL;

static void about_logo_tap_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;

    // Already in dev mode — logo taps do nothing
    if (g_dev_mode) return;

    uint32_t now = millis();
    if (now - aboutLastTapMs > 3000) aboutTapCount = 0;
    aboutTapCount++;
    aboutLastTapMs = now;

    if (aboutTapCount >= 4 && aboutTapCount < 10) {
        int remaining = 10 - aboutTapCount;
        if (!aboutHintLabel && settSelContainer) {
            aboutHintLabel = lv_label_create(settSelContainer);
            lv_obj_set_style_text_color(aboutHintLabel, lv_color_make(80, 80, 80), 0);
            lv_obj_set_style_text_font(aboutHintLabel, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_align(aboutHintLabel, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(aboutHintLabel, LV_ALIGN_CENTER, 0, 60);
        }
        if (aboutHintLabel) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d more tap%s...",
                     remaining, remaining == 1 ? "" : "s");
            lv_label_set_text(aboutHintLabel, buf);
        }
    }

    if (aboutTapCount >= 10) {
        aboutTapCount = 0;
        // Remove hint
        if (aboutHintLabel) {
            lv_obj_del(aboutHintLabel);
            aboutHintLabel = NULL;
        }
        // Show Dev Mode button
        if (!aboutDevBtn && settSelContainer) {
            aboutDevBtn = ui_create_dark_button(
                "Dev Mode", 0,
                [](lv_event_t *ev) {
                    if (!is_valid_tap()) return;
                    block_touch_until_release();
                    // Navigate to dev transition screen
                    enter_state(STATE_DEV_TRANSITION);
                }, settSelContainer);
            lv_obj_align(aboutDevBtn, LV_ALIGN_CENTER, 0, 100);
            lv_obj_set_style_text_color(
                lv_obj_get_child(aboutDevBtn, 0),
                lv_color_make(100, 160, 255), 0);
            fade_in_obj(aboutDevBtn, 300);
        }
    }
}

static void about_disable_dev_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    enter_state(STATE_DEV_TRANSITION);
}

// ─── Dev Tools page (page 19) ───────────────────────────────────────────────

static void devtools_toggle_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    SettOptData *d = (SettOptData *)lv_event_get_user_data(e);
    lv_color_t blueCol = lv_color_make(60, 160, 255);
    lv_color_t gray    = lv_color_make(140, 140, 140);

    if (d->index == 0) {        // Verbose Overlay
        bool cur = storage_get_dev_verbose_overlay();
        storage_set_dev_verbose_overlay(!cur);
        if (devToolsVerbOvlVal) {
            lv_label_set_text(devToolsVerbOvlVal, !cur ? "On" : "Off");
            lv_obj_set_style_text_color(devToolsVerbOvlVal, !cur ? blueCol : gray, 0);
        }
    } else if (d->index == 1) { // Stats Pill
        bool cur = storage_get_dev_stats_pill();
        storage_set_dev_stats_pill(!cur);
        if (devToolsStatsPillVal) {
            lv_label_set_text(devToolsStatsPillVal, !cur ? "On" : "Off");
            lv_obj_set_style_text_color(devToolsStatsPillVal, !cur ? blueCol : gray, 0);
        }
    } else if (d->index == 2) { // Verbose Boot
        bool cur = storage_get_dev_verbose_boot();
        storage_set_dev_verbose_boot(!cur);
        if (devToolsVerbBootVal) {
            lv_label_set_text(devToolsVerbBootVal, !cur ? "On" : "Off");
            lv_obj_set_style_text_color(devToolsVerbBootVal, !cur ? blueCol : gray, 0);
        }
    }
}

static void build_dev_tools_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Developer Tools");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *list = lv_obj_create(settSelContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W - 40, UI_SCR_H - 155);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_color_t blueCol = lv_color_make(60, 160, 255);
    lv_color_t gray    = lv_color_make(140, 140, 140);
    int poolIdx = 0;

    devToolsVerbOvlVal   = NULL;
    devToolsStatsPillVal = NULL;
    devToolsVerbBootVal  = NULL;

    // ── Toggle: Verbose Overlay ──────────────────────────────────────
    {
        bool on = storage_get_dev_verbose_overlay();
        settOptPool[poolIdx] = {19, 0};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, devtools_toggle_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Verbose Overlay");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, on ? "On" : "Off");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, on ? blueCol : gray, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
        devToolsVerbOvlVal = val;
        poolIdx++;
    }

    // ── Toggle: Stats Pill ───────────────────────────────────────────
    {
        bool on = storage_get_dev_stats_pill();
        settOptPool[poolIdx] = {19, 1};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, devtools_toggle_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Stats Pill");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, on ? "On" : "Off");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, on ? blueCol : gray, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
        devToolsStatsPillVal = val;
        poolIdx++;
    }

    // ── Toggle: Verbose Boot ─────────────────────────────────────────
    {
        bool on = storage_get_dev_verbose_boot();
        settOptPool[poolIdx] = {19, 2};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, devtools_toggle_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Verbose Boot");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, on ? "On" : "Off");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, on ? blueCol : gray, 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
        devToolsVerbBootVal = val;
        poolIdx++;
    }

    // Description text
    lv_obj_t *desc = lv_label_create(list);
    lv_label_set_text(desc,
        "Verbose Overlay: Activity log at bottom\n"
        "Stats Pill: DRAM/PSRAM/CPU at top\n"
        "Verbose Boot: Show init steps on boot");
    lv_obj_set_style_text_color(desc, lv_color_make(80, 80, 80), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_width(desc, UI_SCR_W - 120);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// ─── About page ─────────────────────────────────────────────────────────────

static void build_about_page() {
    destroy_sett_selection();
    aboutTapCount  = 0;
    aboutDevBtn    = NULL;
    aboutHintLabel = NULL;

    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "About");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    // Jibo logo — "Jib" text, tappable area
    lv_obj_t *logoBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(logoBtn, 220, 70);
    lv_obj_align(logoBtn, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_opa(logoBtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(logoBtn, 0, 0);
    lv_obj_set_style_border_width(logoBtn, 0, 0);
    lv_obj_add_event_cb(logoBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(logoBtn, about_logo_tap_cb, LV_EVENT_CLICKED, NULL);

    // "Jib" text shifted left — the "o" is a rendered eye sphere
    // Same offsets as the boot screen: "Jib" at -32, eye at +38
    lv_obj_t *logoLbl = lv_label_create(logoBtn);
    lv_label_set_text(logoLbl, "Jib");
    lv_obj_set_style_text_color(logoLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(logoLbl, &lv_font_montserrat_48, 0);
    lv_obj_align(logoLbl, LV_ALIGN_CENTER, -19, 0);

    // 3D-shaded eye sphere as the "o" — same renderer as boot/power-off
    if (aboutEyeBuf) { free(aboutEyeBuf); aboutEyeBuf = NULL; }
    size_t eyeBufSz = ABOUT_EYE_SIZE * ABOUT_EYE_SIZE * LV_IMG_PX_SIZE_ALPHA_BYTE;
    aboutEyeBuf = (uint8_t *)ps_malloc(eyeBufSz);
    if (aboutEyeBuf) {
        ui_render_small_eye(aboutEyeBuf, ABOUT_EYE_SIZE);
        aboutEyeCanvas = lv_canvas_create(logoBtn);
        lv_canvas_set_buffer(aboutEyeCanvas, aboutEyeBuf,
                             ABOUT_EYE_SIZE, ABOUT_EYE_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_obj_clear_flag(aboutEyeCanvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aboutEyeCanvas, LV_ALIGN_CENTER, 39, 4);
    }

    // Device info: serial number
    uint64_t mac = ESP.getEfuseMac();
    char serialBuf[32];
    snprintf(serialBuf, sizeof(serialBuf), "S/N: %04X%08X",
             (uint16_t)(mac >> 32), (uint32_t)(mac & 0xFFFFFFFF));

    lv_obj_t *serialLbl = lv_label_create(settSelContainer);
    lv_label_set_text(serialLbl, serialBuf);
    lv_obj_set_style_text_color(serialLbl, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(serialLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(serialLbl, LV_ALIGN_CENTER, 0, -10);

    // Version
    char verBuf[48];
    snprintf(verBuf, sizeof(verBuf), "Version: %s", JIBO_VERSION);

    lv_obj_t *verLbl = lv_label_create(settSelContainer);
    lv_label_set_text(verLbl, verBuf);
    lv_obj_set_style_text_color(verLbl, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(verLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(verLbl, LV_ALIGN_CENTER, 0, -10);

    // Shift serial number up to make room
    lv_obj_align(serialLbl, LV_ALIGN_CENTER, 0, -34);

    // Build date
    char dateBuf[48];
    snprintf(dateBuf, sizeof(dateBuf), "Build: %s %s", __DATE__, __TIME__);

    lv_obj_t *dateLbl = lv_label_create(settSelContainer);
    lv_label_set_text(dateLbl, dateBuf);
    lv_obj_set_style_text_color(dateLbl, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(dateLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(dateLbl, LV_ALIGN_CENTER, 0, 18);

    // Dev mode status
    if (g_dev_mode) {
        lv_obj_t *devLbl = lv_label_create(settSelContainer);
        lv_label_set_text(devLbl, "Developer Mode");
        lv_obj_set_style_text_color(devLbl, lv_color_make(100, 160, 255), 0);
        lv_obj_set_style_text_font(devLbl, &lv_font_montserrat_18, 0);
        lv_obj_align(devLbl, LV_ALIGN_CENTER, 0, 50);

        // "Disable Developer Mode" button
        aboutDevBtn = ui_create_dark_button(
            "Disable Dev Mode", 0,
            about_disable_dev_cb, settSelContainer);
        lv_obj_align(aboutDevBtn, LV_ALIGN_CENTER, 0, 100);
        lv_obj_set_style_text_color(
            lv_obj_get_child(aboutDevBtn, 0),
            lv_color_make(255, 100, 100), 0);
    }

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

static void build_wifi_page();
static void build_phone_page();

static void build_sett_selection(int page) {
    destroy_sett_selection();

    if (page == 5)  { build_wifi_page(); return; }
    if (page == 6)  { build_phone_page(); return; }
    if (page == 8)  { build_scan_page(); return; }
    if (page == 9)  { build_password_page(); return; }
    if (page == 10) { build_username_page(); return; }
    if (page == 11) { build_connecting_page(); return; }
    if (page == 12) { build_clear_history_confirm(); return; }
    if (page == 13) { build_setup_portal_page(); return; }
    if (page == 14) { build_perm_memory_page(); return; }
    if (page == 15) { build_clear_perm_memory_confirm(); return; }
    if (page == 16) { build_imu_cal_page(); return; }
    if (page == 18) { build_about_page(); return; }
    if (page == 19) { build_dev_tools_page(); return; }
    // page 17 = Sleep Timer -- handled by the generic option list below

    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    const char *titleStr;
    const char **options;
    int count;
    int current;

    static const char *brightOpts[] = {"Low", "Med", "High"};
    static const char *modelOpts[]  = {"gemini-flash-lite-latest", "gemini-flash-latest"};
    static const char *modelOptsDev[] = {"gemini-flash-lite-latest", "gemini-flash-latest", "gemini-pro-latest"};
    static const char *memOpts[] = {"On", "Off", "Clear History", "Permanent Memory"};

    switch (page) {
        case 1: titleStr = "Brightness"; options = brightOpts; count = 3;
                current = storage_get_brightness(); break;
        case 2: titleStr = "Model";
                if (g_dev_mode) { options = modelOptsDev; count = 3; }
                else            { options = modelOpts;    count = 2; }
                current = storage_get_model();
                if (!g_dev_mode && current > 1) current = 0;  // clamp if was set to pro
                break;
        case 3: titleStr = "Voice"; options = VOICE_NAMES; count = NUM_VOICES;
                current = storage_get_voice(); break;
        case 4: titleStr = "Memory"; options = memOpts; count = 4;
                current = storage_get_memory_enabled() ? 0 : 1; break;
        case 17: titleStr = "Sleep Timer"; options = SLEEP_TIMEOUT_NAMES; count = NUM_SLEEP_OPTS;
                 current = storage_get_sleep_timeout(); break;
        default: return;
    }

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, titleStr);
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *list = lv_obj_create(settSelContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W - 40, UI_SCR_H - 155);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_color_t blueCol = lv_color_make(60, 160, 255);
    lv_color_t disabledCol = lv_color_make(80, 80, 80);
    bool memoryEmpty = (page == 4) && (storage_get_conv_history().length() == 0);

    for (int i = 0; i < count && i < 12; i++) {
        settOptPool[i] = {(uint8_t)page, (uint8_t)i};

        if (i > 0) {
            lv_obj_t *div = lv_obj_create(list);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, UI_SCR_W - 120, 1);
            lv_obj_set_style_bg_color(div, lv_color_make(60, 60, 60), 0);
            lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
            lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
        }

        bool disabled = (page == 4 && i == 2 && memoryEmpty);

        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        if (!disabled) {
            lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[i]);
        } else {
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, options[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_color_t col = disabled ? disabledCol
                                  : ((i == current) ? blueCol : lv_color_white());
        lv_obj_set_style_text_color(lbl, col, 0);
        lv_obj_center(lbl);
    }

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// ─── WiFi sub-page ──────────────────────────────────────────────────────────

static void build_wifi_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Wi-Fi");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *list = lv_obj_create(settSelContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W - 40, UI_SCR_H - 155);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_color_t blueCol = lv_color_make(60, 160, 255);
    bool wifiOn = wifi_is_enabled();
    int poolIdx = 0;

    memset(wifiNetStatus, 0, sizeof(wifiNetStatus));
    wifiToggleVal = NULL;

    // Toggle row: Wi-Fi On/Off
    {
        settOptPool[poolIdx] = {5, 0};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Wi-Fi");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, wifiOn ? "On" : "Off");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, wifiOn ? blueCol : lv_color_make(140, 140, 140), 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);
        wifiToggleVal = val;
        poolIdx++;
    }

    // Saved networks
    uint8_t netCount = storage_wifi_count();
    String curSSID = wifi_current_ssid();

    for (uint8_t i = 0; i < netCount && poolIdx < 10; i++) {
        WifiEntry e;
        if (!storage_get_wifi(i, e)) continue;

        lv_obj_t *div = lv_obj_create(list);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, UI_SCR_W - 120, 1);
        lv_obj_set_style_bg_color(div, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);

        settOptPool[poolIdx] = {5, (uint8_t)(i + 1)};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        bool connected = (curSSID.length() > 0 && curSSID == e.ssid);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, e.ssid.c_str());
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, connected ? blueCol : lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *st = lv_label_create(row);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(st, lv_color_make(100, 100, 100), 0);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, -10, 0);
        if (connected) {
            char connBuf[32];
            snprintf(connBuf, sizeof(connBuf), "Connected  %d dBm",
                     (int)WiFi.RSSI());
            lv_label_set_text(st, connBuf);
        } else if (wifiConnectingIdx == i) {
            lv_label_set_text(st, "Connecting...");
        } else {
            lv_label_set_text(st, "");
        }
        if (i < MAX_WIFI_NETWORKS) wifiNetStatus[i] = st;

        settOptPool[poolIdx + 5] = {7, i};
        poolIdx++;
    }

    // "Add Network" row
    {
        lv_obj_t *div = lv_obj_create(list);
        lv_obj_remove_style_all(div);
        lv_obj_set_size(div, UI_SCR_W - 120, 1);
        lv_obj_set_style_bg_color(div, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
        lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);

        settOptPool[poolIdx] = {5, 10};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[poolIdx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "+ Add Network");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_make(100, 100, 100), 0);
        lv_obj_center(lbl);
    }

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// ─── Phone Link sub-page ────────────────────────────────────────────────────

static void update_wifi_page_status() {
    if (settSubPage != 5 || !settSelContainer) return;
    lv_color_t blueCol = lv_color_make(60, 160, 255);
    String curSSID = wifi_current_ssid();
    bool wifiOn = wifi_is_enabled();
    uint8_t netCount = storage_wifi_count();

    if (wifiToggleVal) {
        lv_label_set_text(wifiToggleVal, wifiOn ? "On" : "Off");
        lv_obj_set_style_text_color(wifiToggleVal,
            wifiOn ? blueCol : lv_color_make(140, 140, 140), 0);
    }

    for (uint8_t i = 0; i < netCount && i < MAX_WIFI_NETWORKS; i++) {
        if (!wifiNetStatus[i]) continue;
        WifiEntry e;
        if (!storage_get_wifi(i, e)) continue;

        bool connected = (curSSID.length() > 0 && curSSID == e.ssid);
        if (connected) {
            lv_label_set_text(wifiNetStatus[i], "Connected");
            wifiConnectingIdx = -1;
        } else if (wifiConnectingIdx == i) {
            lv_label_set_text(wifiNetStatus[i], "Connecting...");
        } else {
            lv_label_set_text(wifiNetStatus[i], "");
        }

        lv_obj_t *row = lv_obj_get_parent(wifiNetStatus[i]);
        if (row) {
            lv_obj_t *lbl = lv_obj_get_child(row, 0);
            if (lbl) lv_obj_set_style_text_color(lbl,
                connected ? blueCol : lv_color_white(), 0);
        }
    }
}

static uint8_t phonePairState = 0;  // 0=idle, 1=waiting, 2=code shown

static void build_phone_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Phone Link");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *list = lv_obj_create(settSelContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W - 40, UI_SCR_H - 155);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 8, 0);

    pairCodeLabel = NULL;

    if (ble_is_paired()) {
        // -- Paired state --
        phonePairState = 0;

        lv_obj_t *nameLabel = lv_label_create(list);
        String pName = ble_phone_name();
        lv_label_set_text(nameLabel, pName.c_str());
        lv_obj_set_style_text_color(nameLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_20, 0);

        lv_obj_t *statusLabel = lv_label_create(list);
        lv_label_set_text(statusLabel, ble_is_connected() ? "Connected" : "Not connected");
        lv_obj_set_style_text_color(statusLabel,
            ble_is_connected() ? lv_color_make(60, 160, 255) : lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_16, 0);

        lv_obj_t *sp = lv_obj_create(list);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 10, 20);

        settOptPool[0] = {6, 0};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 100, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(row, lv_color_make(200, 60, 60), 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 23, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_make(200, 60, 60), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[0]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Unpair");
        lv_obj_set_style_text_color(lbl, lv_color_make(200, 60, 60), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);

    } else if (ble_pairing_active() && !ble_pairing_phone_connected()) {
        // -- Waiting for phone to connect --
        phonePairState = 1;

        lv_obj_t *spinner = lv_spinner_create(list, 1000, 60);
        lv_obj_set_size(spinner, 50, 50);
        lv_obj_set_style_arc_width(spinner, 4, 0);
        lv_obj_set_style_arc_color(spinner, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, lv_color_white(), LV_PART_INDICATOR);

        lv_obj_t *sp = lv_obj_create(list);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 10, 12);

        lv_obj_t *instr = lv_label_create(list);
        lv_label_set_text(instr, "Open the Jibo app\non your phone");
        lv_obj_set_style_text_color(instr, lv_color_make(180, 180, 180), 0);
        lv_obj_set_style_text_font(instr, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *sub = lv_label_create(list);
        lv_label_set_text(sub, "Waiting for connection...");
        lv_obj_set_style_text_color(sub, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);

        lv_obj_t *sp2 = lv_obj_create(list);
        lv_obj_remove_style_all(sp2);
        lv_obj_set_size(sp2, 10, 16);

        // Cancel button
        settOptPool[0] = {6, 0};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 120, 42);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(row, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 21, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[0]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Cancel");
        lv_obj_set_style_text_color(lbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);

        pairPollMs = millis();

    } else if (ble_pairing_phone_connected()) {
        // -- Phone connected, show code --
        phonePairState = 2;

        lv_obj_t *instr = lv_label_create(list);
        lv_label_set_text(instr, "Enter this code\nin the Jibo app:");
        lv_obj_set_style_text_color(instr, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(instr, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);

        pairCodeLabel = lv_label_create(list);
        char codeStr[8];
        snprintf(codeStr, sizeof(codeStr), "%04u", ble_get_pairing_code());
        lv_label_set_text(pairCodeLabel, codeStr);
        lv_obj_set_style_text_color(pairCodeLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(pairCodeLabel, &lv_font_montserrat_48, 0);

        lv_obj_t *waiting = lv_label_create(list);
        lv_label_set_text(waiting, "Waiting for code entry...");
        lv_obj_set_style_text_color(waiting, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(waiting, &lv_font_montserrat_14, 0);

        pairPollMs = millis();

    } else {
        // -- Not paired, not pairing --
        phonePairState = 0;

        settOptPool[0] = {6, 0};
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 100, 52);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 26, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, sett_option_cb, LV_EVENT_CLICKED, &settOptPool[0]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Pair Phone");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);

        lv_obj_t *desc = lv_label_create(list);
        lv_label_set_text(desc, "Use the Jibo app to\nconnect your phone");
        lv_obj_set_style_text_color(desc, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    }

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  T9 KEYBOARD WIDGET
// =====================================================================

static char      t9Input[65]     = {};
static uint8_t   t9Len           = 0;
static uint8_t   t9ActiveKey     = 0xFF;
static uint8_t   t9CycleIdx     = 0;
static uint32_t  t9LastTapMs     = 0;
static uint8_t   t9Mode          = 0;   // 0=lower, 1=upper, 2=num/sym
static bool      t9Masked        = false;

static const char *t9Maps[][10] = {
    // mode 0: lowercase
    { "1.,!?", "2abc",  "3def",
      "4ghi",  "5jkl",  "6mno",
      "7pqrs", "8tuv",  "9wxyz",
      "0 " },
    // mode 1: uppercase
    { "1.,!?", "2ABC",  "3DEF",
      "4GHI",  "5JKL",  "6MNO",
      "7PQRS", "8TUV",  "9WXYZ",
      "0 " },
    // mode 2: numbers/symbols
    { "1",     "2",     "3",
      "4",     "5",     "6",
      "7",     "8",     "9",
      "0@#$%&*-+_=/" },
};

static const char *t9Labels[] = {
    "1 .,!",  "2 abc",  "3 def",
    "4 ghi",  "5 jkl",  "6 mno",
    "7 pqrs", "8 tuv",  "9 wxyz",
};

static const char *t9LabelsUpper[] = {
    "1 .,!",  "2 ABC",  "3 DEF",
    "4 GHI",  "5 JKL",  "6 MNO",
    "7 PQRS", "8 TUV",  "9 WXYZ",
};

static const char *t9LabelsSym[] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
};

static void t9_commit_pending() {
    if (t9ActiveKey == 0xFF || t9Len == 0) return;
    t9ActiveKey = 0xFF;
    t9CycleIdx = 0;
}

static void t9_update_display() {
    if (!t9Display) return;
    if (t9Masked && t9Len > 0) {
        char masked[66];
        for (uint8_t i = 0; i < t9Len - 1 && i < 64; i++) masked[i] = '*';
        if (t9ActiveKey != 0xFF)
            masked[t9Len - 1] = t9Input[t9Len - 1];
        else if (t9Len > 0)
            masked[t9Len - 1] = '*';
        masked[t9Len] = '_';
        masked[t9Len + 1] = '\0';
        lv_label_set_text(t9Display, masked);
    } else {
        char buf[66];
        memcpy(buf, t9Input, t9Len);
        buf[t9Len] = '_';
        buf[t9Len + 1] = '\0';
        lv_label_set_text(t9Display, buf);
    }
}

struct T9KeyData { uint8_t keyIdx; };
static T9KeyData t9KeyPool[12];

static void t9_key_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    T9KeyData *kd = (T9KeyData *)lv_event_get_user_data(e);
    uint8_t idx = kd->keyIdx;

    if (idx == 10) {
        // Delete
        t9ActiveKey = 0xFF;
        if (t9Len > 0) { t9Len--; t9Input[t9Len] = '\0'; }
        t9_update_display();
        return;
    }

    if (idx == 11) {
        // Mode toggle
        t9_commit_pending();
        t9Mode = (t9Mode + 1) % 3;
        // Rebuild keyboard labels
        lv_obj_t *grid = lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e));
        if (!grid) return;
        uint32_t cnt = lv_obj_get_child_cnt(grid);
        const char **labels = (t9Mode == 0) ? t9Labels : (t9Mode == 1) ? t9LabelsUpper : t9LabelsSym;
        for (uint32_t i = 0; i < cnt && i < 12; i++) {
            lv_obj_t *btn = lv_obj_get_child(grid, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (!lbl) continue;
            if (i < 9) lv_label_set_text(lbl, labels[i]);
            else if (i == 9) lv_label_set_text(lbl, (t9Mode == 2) ? "0 @#$" : "0 spc");
            else if (i == 11) {
                const char *modeStr = (t9Mode == 0) ? "ABC" : (t9Mode == 1) ? "123" : "abc";
                lv_label_set_text(lbl, modeStr);
            }
        }
        t9_update_display();
        return;
    }

    // Normal key (0-9)
    const char *chars = t9Maps[t9Mode][idx];
    uint8_t numChars = strlen(chars);
    if (numChars == 0) return;

    uint32_t now = millis();
    if (idx == t9ActiveKey && (now - t9LastTapMs) < 600) {
        t9CycleIdx = (t9CycleIdx + 1) % numChars;
        t9Input[t9Len - 1] = chars[t9CycleIdx];
    } else {
        t9_commit_pending();
        if (t9Len >= 64) return;
        t9ActiveKey = idx;
        t9CycleIdx = 0;
        t9Input[t9Len] = chars[0];
        t9Len++;
        t9Input[t9Len] = '\0';
    }
    t9LastTapMs = now;
    t9_update_display();
}

static void build_t9_keyboard(lv_obj_t *parent, int yOffset) {
    t9Container = lv_obj_create(parent);
    lv_obj_remove_style_all(t9Container);
    int kbW = 300;
    int kbH = 210;
    lv_obj_set_size(t9Container, kbW, kbH);
    lv_obj_align(t9Container, LV_ALIGN_TOP_MID, 0, yOffset);
    lv_obj_clear_flag(t9Container, LV_OBJ_FLAG_SCROLLABLE);

    int btnW = 90;
    int btnH = 44;
    int padX = 8;
    int padY = 5;

    const char **labels = (t9Mode == 0) ? t9Labels : (t9Mode == 1) ? t9LabelsUpper : t9LabelsSym;

    for (int i = 0; i < 12; i++) {
        int row, col;
        if (i < 9) { row = i / 3; col = i % 3; }
        else if (i == 9) { row = 3; col = 1; }  // 0/space
        else if (i == 10) { row = 3; col = 0; }  // del
        else { row = 3; col = 2; }               // mode

        int x = col * (btnW + padX);
        int y = row * (btnH + padY);

        t9KeyPool[i] = { (uint8_t)i };

        lv_obj_t *btn = lv_btn_create(t9Container);
        lv_obj_set_size(btn, btnW, btnH);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_make(40, 40, 40), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_make(70, 70, 70), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn, t9_key_cb, LV_EVENT_CLICKED, &t9KeyPool[i]);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);

        if (i < 9) {
            lv_label_set_text(lbl, labels[i]);
        } else if (i == 9) {
            lv_label_set_text(lbl, "0 spc");
        } else if (i == 10) {
            lv_label_set_text(lbl, "<-");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        } else {
            lv_label_set_text(lbl, "ABC");
        }
    }
}

static void destroy_t9_keyboard() {
    if (t9Container) { lv_obj_del(t9Container); t9Container = NULL; }
    t9Display = NULL;
}

static void t9_reset() {
    memset(t9Input, 0, sizeof(t9Input));
    t9Len = 0;
    t9ActiveKey = 0xFF;
    t9CycleIdx = 0;
    t9LastTapMs = 0;
    t9Mode = 0;
    t9Masked = false;
}

// =====================================================================
//  SCAN PAGE (page 8) -- WiFi network scanner
// =====================================================================

static bool     scanActive      = false;
static bool     scanDone        = false;
static int      scanCount       = 0;
static uint32_t scanStartMs     = 0;
static uint8_t  scanRetries     = 0;
static uint32_t scanRetryAtMs   = 0;     // 0 = no retry pending

static String   selectedSSID;
static bool     selectedSecured    = false;
static bool     selectedEnterprise = false;

// In-device connect tracking (for connecting page 11)
static int8_t   pendingConnectIdx   = -1;   // storage idx of the entry being attempted
static uint8_t  pendingConnectErr   = 0;    // 0=in-progress, 1=auth, 2=no-ap, 3=timeout, 99=success
static uint32_t connectShowErrMs   = 0;

static SettOptData scanOptPool[20];

static void scan_net_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    SettOptData *d = (SettOptData *)lv_event_get_user_data(e);
    int i = d->index;
    WifiScanResult r = wifi_scan_result_unique(i);
    selectedSSID       = r.ssid;
    selectedSecured    = r.secured;
    selectedEnterprise = r.enterprise;
    wifi_scan_cleanup();
    scanActive = false;

    if (!r.secured) {
        // Open network: save and connect directly
        uint8_t cnt = storage_wifi_count();
        if (cnt >= MAX_WIFI_NETWORKS) cnt = MAX_WIFI_NETWORKS - 1;
        WifiEntry we;
        we.ssid = r.ssid;
        we.password = "";
        we.username = "";
        we.enterprise = false;
        storage_set_wifi(cnt, we);
        storage_set_wifi_count(cnt + 1);
        wifiConnectingIdx = cnt;
        wifi_connect_to(cnt);
        settPendingPage = 5;
    } else if (r.enterprise) {
        settPendingPage = 10;
    } else {
        settPendingPage = 9;
    }
}

static void build_scan_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Add Network");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 85);

    scanDone = false;
    scanCount = 0;

    scanSpinner = lv_spinner_create(settSelContainer, 1000, 60);
    lv_obj_set_size(scanSpinner, 50, 50);
    lv_obj_align(scanSpinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_arc_color(scanSpinner, lv_color_make(60, 160, 255), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(scanSpinner, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_arc_width(scanSpinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(scanSpinner, 5, LV_PART_MAIN);

    scanStatusLbl = lv_label_create(settSelContainer);
    lv_label_set_text(scanStatusLbl, "Searching...");
    lv_obj_set_style_text_color(scanStatusLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(scanStatusLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(scanStatusLbl, LV_ALIGN_CENTER, 0, 30);

    scanActive    = true;
    scanStartMs   = millis();
    scanRetries   = 0;
    scanRetryAtMs = 0;
    wifi_scan_start();

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

static void build_scan_results() {
    if (scanSpinner) { lv_obj_del(scanSpinner); scanSpinner = NULL; }
    if (scanStatusLbl) { lv_obj_del(scanStatusLbl); scanStatusLbl = NULL; }

    lv_obj_t *list = lv_obj_create(settSelContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W - 40, UI_SCR_H - 155);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (scanCount == 0) {
        lv_obj_t *noLbl = lv_label_create(list);
        lv_label_set_text(noLbl, "No networks found");
        lv_obj_set_style_text_color(noLbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(noLbl, &lv_font_montserrat_16, 0);
        return;
    }

    lv_color_t blueCol = lv_color_make(60, 160, 255);

    for (int i = 0; i < scanCount && i < 20; i++) {
        WifiScanResult r = wifi_scan_result_unique(i);
        if (r.ssid.length() == 0) continue;

        if (i > 0) {
            lv_obj_t *div = lv_obj_create(list);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, UI_SCR_W - 120, 1);
            lv_obj_set_style_bg_color(div, lv_color_make(60, 60, 60), 0);
            lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
            lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
        }

        scanOptPool[i] = {8, (uint8_t)i};

        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, UI_SCR_W - 80, 44);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, scan_net_cb, LV_EVENT_CLICKED, &scanOptPool[i]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, r.ssid.c_str());
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_decor(lbl, LV_TEXT_DECOR_NONE, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, UI_SCR_W - 230);

        // dBm label
        char dbmBuf[12];
        snprintf(dbmBuf, sizeof(dbmBuf), "%d", (int)r.rssi);
        lv_obj_t *dbmLbl = lv_label_create(row);
        lv_label_set_text(dbmLbl, dbmBuf);
        lv_obj_set_style_text_font(dbmLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(dbmLbl, lv_color_make(100, 100, 100), 0);
        lv_obj_align(dbmLbl, LV_ALIGN_RIGHT_MID, -34, 0);

        // Signal circle: size = signal strength, color = security type
        int circSize;
        if (r.rssi > -50)      circSize = 16;
        else if (r.rssi > -70) circSize = 12;
        else                   circSize = 8;

        lv_color_t circColor;
        if (r.enterprise)      circColor = lv_color_make(240, 180, 80);
        else if (r.secured)    circColor = lv_color_make(60, 160, 255);
        else                   circColor = lv_color_white();

        lv_obj_t *circ = lv_obj_create(row);
        lv_obj_remove_style_all(circ);
        lv_obj_set_size(circ, circSize, circSize);
        lv_obj_set_style_bg_color(circ, circColor, 0);
        lv_obj_set_style_bg_opa(circ, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(circ, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(circ, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_clear_flag(circ, LV_OBJ_FLAG_CLICKABLE);
    }

    fade_in_children(list, 150);
}

// =====================================================================
//  PASSWORD PAGE (page 9)
// =====================================================================

static String t9Username;

static void pass_connect_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    t9_commit_pending();

    uint8_t cnt = storage_wifi_count();
    bool overwriting = false;
    if (cnt >= MAX_WIFI_NETWORKS) {
        cnt = MAX_WIFI_NETWORKS - 1;
        overwriting = true;
    }

    WifiEntry we;
    we.ssid       = selectedSSID;
    we.password   = String(t9Input);
    we.username   = selectedEnterprise ? t9Username : "";
    we.enterprise = selectedEnterprise;
    storage_set_wifi(cnt, we);
    if (!overwriting) storage_set_wifi_count(cnt + 1);

    pendingConnectIdx = cnt;
    pendingConnectErr = 0;
    wifiConnectingIdx = cnt;
    wifi_connect_to(cnt);

    t9_reset();
    settPendingPage = 11;
}

static void build_password_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, selectedSSID.c_str());
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_20, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 68);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ttl, UI_SCR_W - 100);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *fieldLbl = lv_label_create(settSelContainer);
    lv_label_set_text(fieldLbl, "Password");
    lv_obj_set_style_text_color(fieldLbl, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(fieldLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(fieldLbl, LV_ALIGN_TOP_MID, 0, 96);

    t9_reset();
    t9Masked = true;

    t9Display = lv_label_create(settSelContainer);
    lv_label_set_text(t9Display, "_");
    lv_obj_set_style_text_color(t9Display, lv_color_white(), 0);
    lv_obj_set_style_text_font(t9Display, &lv_font_montserrat_20, 0);
    lv_obj_align(t9Display, LV_ALIGN_TOP_MID, 0, 116);
    lv_label_set_long_mode(t9Display, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(t9Display, UI_SCR_W - 120);
    lv_obj_set_style_text_align(t9Display, LV_TEXT_ALIGN_CENTER, 0);

    build_t9_keyboard(settSelContainer, 153);

    // Connect button
    lv_obj_t *connectBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(connectBtn, 120, 38);
    lv_obj_align(connectBtn, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(connectBtn, lv_color_make(60, 160, 255), 0);
    lv_obj_set_style_bg_opa(connectBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connectBtn, 19, 0);
    lv_obj_set_style_shadow_width(connectBtn, 0, 0);
    lv_obj_set_style_border_width(connectBtn, 0, 0);
    lv_obj_add_event_cb(connectBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(connectBtn, pass_connect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cLbl = lv_label_create(connectBtn);
    lv_label_set_text(cLbl, "Connect");
    lv_obj_set_style_text_color(cLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(cLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  USERNAME PAGE (page 10) -- enterprise networks
// =====================================================================

static void user_next_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    t9_commit_pending();
    t9Username = String(t9Input);
    t9_reset();
    settPendingPage = 9;
}

static void build_username_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, selectedSSID.c_str());
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_20, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 68);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ttl, UI_SCR_W - 100);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *fieldLbl = lv_label_create(settSelContainer);
    lv_label_set_text(fieldLbl, "Username");
    lv_obj_set_style_text_color(fieldLbl, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(fieldLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(fieldLbl, LV_ALIGN_TOP_MID, 0, 96);

    t9_reset();
    t9Masked = false;

    t9Display = lv_label_create(settSelContainer);
    lv_label_set_text(t9Display, "_");
    lv_obj_set_style_text_color(t9Display, lv_color_white(), 0);
    lv_obj_set_style_text_font(t9Display, &lv_font_montserrat_20, 0);
    lv_obj_align(t9Display, LV_ALIGN_TOP_MID, 0, 116);
    lv_label_set_long_mode(t9Display, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(t9Display, UI_SCR_W - 120);
    lv_obj_set_style_text_align(t9Display, LV_TEXT_ALIGN_CENTER, 0);

    build_t9_keyboard(settSelContainer, 153);

    lv_obj_t *nextBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(nextBtn, 120, 38);
    lv_obj_align(nextBtn, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(nextBtn, lv_color_make(60, 160, 255), 0);
    lv_obj_set_style_bg_opa(nextBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nextBtn, 19, 0);
    lv_obj_set_style_shadow_width(nextBtn, 0, 0);
    lv_obj_set_style_border_width(nextBtn, 0, 0);
    lv_obj_add_event_cb(nextBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(nextBtn, user_next_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *nLbl = lv_label_create(nextBtn);
    lv_label_set_text(nLbl, "Next");
    lv_obj_set_style_text_color(nLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(nLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(nLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  CONNECTING PAGE (page 11) -- in-device WiFi connect progress
// =====================================================================

static void build_connecting_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Connecting");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *ssidLbl = lv_label_create(settSelContainer);
    lv_label_set_text(ssidLbl, selectedSSID.c_str());
    lv_obj_set_style_text_color(ssidLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(ssidLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(ssidLbl, LV_ALIGN_TOP_MID, 0, 122);
    lv_label_set_long_mode(ssidLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssidLbl, UI_SCR_W - 100);
    lv_obj_set_style_text_align(ssidLbl, LV_TEXT_ALIGN_CENTER, 0);

    connectSpinnerObj = lv_spinner_create(settSelContainer, 1000, 60);
    lv_obj_set_size(connectSpinnerObj, 60, 60);
    lv_obj_align(connectSpinnerObj, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_arc_color(connectSpinnerObj, lv_color_make(60, 160, 255), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(connectSpinnerObj, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_arc_width(connectSpinnerObj, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(connectSpinnerObj, 5, LV_PART_MAIN);

    connectStatusLbl = lv_label_create(settSelContainer);
    lv_label_set_text(connectStatusLbl, "Authenticating...");
    lv_obj_set_style_text_color(connectStatusLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(connectStatusLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(connectStatusLbl, LV_ALIGN_CENTER, 0, 70);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  CLEAR HISTORY CONFIRMATION (page 12)
// =====================================================================

static SettOptData clearConfirmPool[2];

static void build_clear_history_confirm() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Clear memory\nhistory?");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 110);

    lv_obj_t *sub = lv_label_create(settSelContainer);
    lv_label_set_text(sub, "This cannot be undone.");
    lv_obj_set_style_text_color(sub, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 195);

    // No (cancel) -- left
    clearConfirmPool[0] = {12, 0};
    lv_obj_t *noBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(noBtn, 110, 44);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_MID, -65, -90);
    lv_obj_set_style_bg_color(noBtn, lv_color_make(50, 50, 50), 0);
    lv_obj_set_style_bg_opa(noBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(noBtn, 22, 0);
    lv_obj_set_style_shadow_width(noBtn, 0, 0);
    lv_obj_set_style_border_width(noBtn, 0, 0);
    lv_obj_add_event_cb(noBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(noBtn, sett_option_cb, LV_EVENT_CLICKED, &clearConfirmPool[0]);
    lv_obj_t *noLbl = lv_label_create(noBtn);
    lv_label_set_text(noLbl, "No");
    lv_obj_set_style_text_color(noLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(noLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(noLbl);

    // Yes (destructive) -- right
    clearConfirmPool[1] = {12, 1};
    lv_obj_t *yesBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(yesBtn, 110, 44);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_MID, 65, -90);
    lv_obj_set_style_bg_color(yesBtn, lv_color_make(200, 60, 60), 0);
    lv_obj_set_style_bg_opa(yesBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yesBtn, 22, 0);
    lv_obj_set_style_shadow_width(yesBtn, 0, 0);
    lv_obj_set_style_border_width(yesBtn, 0, 0);
    lv_obj_add_event_cb(yesBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(yesBtn, sett_option_cb, LV_EVENT_CLICKED, &clearConfirmPool[1]);
    lv_obj_t *yesLbl = lv_label_create(yesBtn);
    lv_label_set_text(yesLbl, "Yes");
    lv_obj_set_style_text_color(yesLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yesLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(yesLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  PERMANENT MEMORY (page 14) -- list + manage button
// =====================================================================

static SettOptData permMemPool[1];

static void build_perm_memory_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Permanent\nMemory");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 90);

    char countBuf[48];
    uint16_t count = storage_perm_mem_count();
    snprintf(countBuf, sizeof(countBuf),
             "%u %s saved", count, count == 1 ? "memory" : "memories");
    lv_obj_t *countLbl = lv_label_create(settSelContainer);
    lv_label_set_text(countLbl, countBuf);
    lv_obj_set_style_text_color(countLbl, lv_color_make(60, 160, 255), 0);
    lv_obj_set_style_text_font(countLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(countLbl, LV_ALIGN_TOP_MID, 0, 175);

    lv_obj_t *body = lv_label_create(settSelContainer);
    lv_label_set_text(body,
        "Open the Jibo app on\nyour phone to view and\nmanage saved memories.");
    lv_obj_set_style_text_color(body, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 215);

    bool empty = (count == 0);
    permMemPool[0] = {14, 0};
    lv_obj_t *clrBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(clrBtn, UI_SCR_W - 120, 46);
    lv_obj_align(clrBtn, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_set_style_bg_color(clrBtn,
        empty ? lv_color_make(40, 40, 40) : lv_color_make(140, 50, 50), 0);
    lv_obj_set_style_bg_opa(clrBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(clrBtn, 23, 0);
    lv_obj_set_style_shadow_width(clrBtn, 0, 0);
    lv_obj_set_style_border_width(clrBtn, 0, 0);
    if (!empty) {
        lv_obj_add_event_cb(clrBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(clrBtn, sett_option_cb, LV_EVENT_CLICKED, &permMemPool[0]);
    } else {
        lv_obj_clear_flag(clrBtn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *clrLbl = lv_label_create(clrBtn);
    lv_label_set_text(clrLbl, "Clear All");
    lv_obj_set_style_text_color(clrLbl,
        empty ? lv_color_make(80, 80, 80) : lv_color_white(), 0);
    lv_obj_set_style_text_font(clrLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(clrLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  CLEAR PERMANENT MEMORY (page 15) -- destructive confirmation
// =====================================================================

static SettOptData clearPermMemPool[2];

static void build_clear_perm_memory_confirm() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Clear permanent\nmemory?");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 110);

    lv_obj_t *sub = lv_label_create(settSelContainer);
    lv_label_set_text(sub, "This cannot be undone.");
    lv_obj_set_style_text_color(sub, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 195);

    clearPermMemPool[0] = {15, 0};
    lv_obj_t *noBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(noBtn, 110, 44);
    lv_obj_align(noBtn, LV_ALIGN_BOTTOM_MID, -65, -90);
    lv_obj_set_style_bg_color(noBtn, lv_color_make(50, 50, 50), 0);
    lv_obj_set_style_bg_opa(noBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(noBtn, 22, 0);
    lv_obj_set_style_shadow_width(noBtn, 0, 0);
    lv_obj_set_style_border_width(noBtn, 0, 0);
    lv_obj_add_event_cb(noBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(noBtn, sett_option_cb, LV_EVENT_CLICKED, &clearPermMemPool[0]);
    lv_obj_t *noLbl = lv_label_create(noBtn);
    lv_label_set_text(noLbl, "No");
    lv_obj_set_style_text_color(noLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(noLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(noLbl);

    clearPermMemPool[1] = {15, 1};
    lv_obj_t *yesBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(yesBtn, 110, 44);
    lv_obj_align(yesBtn, LV_ALIGN_BOTTOM_MID, 65, -90);
    lv_obj_set_style_bg_color(yesBtn, lv_color_make(200, 60, 60), 0);
    lv_obj_set_style_bg_opa(yesBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(yesBtn, 22, 0);
    lv_obj_set_style_shadow_width(yesBtn, 0, 0);
    lv_obj_set_style_border_width(yesBtn, 0, 0);
    lv_obj_add_event_cb(yesBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(yesBtn, sett_option_cb, LV_EVENT_CLICKED, &clearPermMemPool[1]);
    lv_obj_t *yesLbl = lv_label_create(yesBtn);
    lv_label_set_text(yesLbl, "Yes");
    lv_obj_set_style_text_color(yesLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(yesLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(yesLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  IMU CALIBRATION (page 16)
// =====================================================================

static lv_obj_t *imuCalStatusLbl = NULL;
static uint32_t  imuCalDoneMs    = 0;

static void build_imu_cal_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "IMU Calibration");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *sub = lv_label_create(settSelContainer);
    lv_label_set_text(sub, "Place on a flat surface\nand keep still.");
    lv_obj_set_style_text_color(sub, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -20);

    imuCalStatusLbl = lv_label_create(settSelContainer);
    lv_label_set_text(imuCalStatusLbl, "Calibrating...");
    lv_obj_set_style_text_color(imuCalStatusLbl, lv_color_make(100, 200, 255), 0);
    lv_obj_set_style_text_font(imuCalStatusLbl, &lv_font_montserrat_20, 0);
    lv_obj_align(imuCalStatusLbl, LV_ALIGN_CENTER, 0, 40);

    imuCalDoneMs = 0;

    if (!imu_is_ready()) imu_init();
    imu_cal_start();

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

// =====================================================================
//  SETUP PORTAL (page 13) -- relaunch the captive-portal config UI
// =====================================================================

static void portal_exit_btn_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    exit_setup_portal();
}

static void build_setup_portal_page() {
    settSelContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(settSelContainer);
    lv_obj_set_size(settSelContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(settSelContainer, 0, 0);
    lv_obj_clear_flag(settSelContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(settSelContainer);
    lv_label_set_text(ttl, "Setup Portal");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_24, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *sub = lv_label_create(settSelContainer);
    lv_label_set_text(sub, "Connect your phone or laptop\nto edit settings");
    lv_obj_set_style_text_color(sub, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 130);

    // Stop the STA so the radio is free for AP-only mode.
    settPortalRestoreWifi = wifi_is_enabled();
    if (settPortalRestoreWifi) {
        wifi_disconnect();
    }

    // SSID / password lines
    uint16_t suffix = storage_device_suffix();
    char apSSID[16];
    char apPass[16];
    snprintf(apSSID, sizeof(apSSID), "Jibo");
    snprintf(apPass, sizeof(apPass), "Jibo%04d", suffix);

    portal_start_settings(String(apSSID), String(apPass));
    settPortalActive = true;

    char ssidLine[32];
    snprintf(ssidLine, sizeof(ssidLine), "Wi-Fi: %s", apSSID);
    lv_obj_t *ssidLbl = lv_label_create(settSelContainer);
    lv_label_set_text(ssidLbl, ssidLine);
    lv_obj_set_style_text_color(ssidLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ssidLbl, &lv_font_montserrat_20, 0);
    lv_obj_align(ssidLbl, LV_ALIGN_TOP_MID, 0, 200);

    char passLine[32];
    snprintf(passLine, sizeof(passLine), "Password: %s", apPass);
    lv_obj_t *passLbl = lv_label_create(settSelContainer);
    lv_label_set_text(passLbl, passLine);
    lv_obj_set_style_text_color(passLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(passLbl, &lv_font_montserrat_20, 0);
    lv_obj_align(passLbl, LV_ALIGN_TOP_MID, 0, 230);

    lv_obj_t *ipLbl = lv_label_create(settSelContainer);
    lv_label_set_text(ipLbl, "Then visit 192.168.4.1");
    lv_obj_set_style_text_color(ipLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(ipLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(ipLbl, LV_ALIGN_TOP_MID, 0, 268);

    // Exit button
    lv_obj_t *exitBtn = lv_btn_create(settSelContainer);
    lv_obj_set_size(exitBtn, 160, 50);
    lv_obj_align(exitBtn, LV_ALIGN_BOTTOM_MID, 0, -90);
    lv_obj_set_style_bg_color(exitBtn, lv_color_make(60, 160, 255), 0);
    lv_obj_set_style_bg_opa(exitBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(exitBtn, 25, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_set_style_border_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(exitBtn, portal_exit_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, "Exit Portal");
    lv_obj_set_style_text_color(exitLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(exitLbl);

    block_touch_until_release();
    fade_in_children(settSelContainer, 150);
}

static void exit_setup_portal() {
    if (settPortalActive) {
        portal_stop();
        settPortalActive = false;
        if (settPortalRestoreWifi) {
            wifi_enable();
            settPortalRestoreWifi = false;
        }
    }
    // Return to the main settings page
    settPendingPage = 0;
}

// =====================================================================
//  PUBLIC API
// =====================================================================

void sett_init() {
    // NOTE: ui_home_destroy() is called by enter_state() before us
    destroy_settings();
    destroy_sett_selection();
    eye_hide();

    settSubPage = 0;
    settPendingPage = -1;
    build_settings_main();
}

static uint32_t lastSettStatusMs = 0;

void sett_tick() {
    wifi_connect_tick();

    // Settings-mode captive portal: keep DNS / web server alive while the
    // user is on the portal sub-page.
    if (settPortalActive) {
        portal_tick();
        if (portal_exit_requested()) {
            exit_setup_portal();
        }
    }

    // Live-update status labels
    if (states_sub_step() == 0 && millis() - lastSettStatusMs >= 500) {
        lastSettStatusMs = millis();
        if (settSubPage == 0) {
            if (settWifiVal)  lv_label_set_text(settWifiVal,  wifi_status_str());
            if (settPhoneVal) lv_label_set_text(settPhoneVal, phone_status_str());
        } else if (settSubPage == 5) {
            update_wifi_page_status();
        }
    }

    // Poll WiFi scan on page 8.
    if (settSubPage == 8 && scanActive && !scanDone) {
        const uint32_t MIN_SPINNER_MS  = 700;
        const uint32_t HARD_TIMEOUT_MS = 12000;
        const uint32_t RETRY_WAIT_MS   = 600;
        const uint8_t  MAX_RETRIES     = 3;

        // If a retry was scheduled, wait it out before doing anything else.
        if (scanRetryAtMs != 0) {
            if (millis() >= scanRetryAtMs) {
                scanRetryAtMs = 0;
                scanStartMs = millis();
                wifi_scan_start();
            }
            return;
        }

        int st = wifi_scan_status();
        uint32_t elapsed = millis() - scanStartMs;

        bool finishedClean   = (st >= 0 && elapsed >= MIN_SPINNER_MS);
        bool stillbornResult = (st >= 0 && elapsed < 200 && st == 0);
        bool failed          = (st == -2);
        bool hardTimeout     = (elapsed >= HARD_TIMEOUT_MS);

        if (finishedClean) {
            scanCount = wifi_scan_count_unique();
            scanDone = true;
            scanActive = false;
            build_scan_results();
        } else if ((stillbornResult || failed) && scanRetries < MAX_RETRIES) {
            scanRetries++;
            LOG1("[wifi] scan attempt failed (st=%d elapsed=%ums), retrying %u/%u in %ums\n",
                 st, (unsigned)elapsed, scanRetries, MAX_RETRIES, (unsigned)RETRY_WAIT_MS);
            scanRetryAtMs = millis() + RETRY_WAIT_MS;
        } else if ((failed && scanRetries >= MAX_RETRIES) || hardTimeout) {
            LOG1("[wifi] scan giving up (st=%d elapsed=%ums retries=%u)\n",
                 st, (unsigned)elapsed, scanRetries);
            scanCount = 0;
            scanDone = true;
            scanActive = false;
            build_scan_results();
        }
    }

    // T9 auto-commit timeout
    if ((settSubPage == 9 || settSubPage == 10) && t9ActiveKey != 0xFF) {
        if (millis() - t9LastTapMs >= 600) {
            t9_commit_pending();
            t9_update_display();
        }
    }

    // IMU calibration page (16) -- poll calibration progress
    if (settSubPage == 16) {
        imu_poll();
        if (imuCalDoneMs == 0 && imu_cal_tick()) {
            imuCalDoneMs = millis();
            if (imuCalStatusLbl)
                lv_label_set_text(imuCalStatusLbl, "Calibrated!");
            imu_deinit();
        }
        if (imuCalDoneMs > 0 && millis() - imuCalDoneMs >= 1500) {
            imuCalStatusLbl = NULL;
            settPendingPage = 0;
        }
    }

    // Connecting page (11) -- poll connection state
    if (settSubPage == 11 && pendingConnectErr == 0) {
        if (wifi_is_connected()) {
            pendingConnectErr = 99;
            if (connectStatusLbl) lv_label_set_text(connectStatusLbl, "Connected!");
            if (connectSpinnerObj) { lv_obj_del(connectSpinnerObj); connectSpinnerObj = NULL; }
            connectShowErrMs = millis();
        } else if (wifi_connect_failed()) {
            uint8_t err = wifi_connect_error();
            pendingConnectErr = err ? err : 3;
            // Remove the bad entry from storage
            if (pendingConnectIdx >= 0) {
                storage_remove_wifi((uint8_t)pendingConnectIdx);
                pendingConnectIdx = -1;
            }
            wifiConnectingIdx = -1;
            if (connectSpinnerObj) { lv_obj_del(connectSpinnerObj); connectSpinnerObj = NULL; }
            const char *msg;
            switch (pendingConnectErr) {
                case 1:  msg = "Wrong password"; break;
                case 2:  msg = "Network not found"; break;
                default: msg = "Connection failed"; break;
            }
            if (connectStatusLbl) {
                lv_label_set_text(connectStatusLbl, msg);
                lv_obj_set_style_text_color(connectStatusLbl, lv_color_make(255, 100, 100), 0);
                lv_obj_set_style_text_font(connectStatusLbl, &lv_font_montserrat_20, 0);
                lv_obj_align(connectStatusLbl, LV_ALIGN_CENTER, 0, 0);
            }
            connectShowErrMs = millis();
        }
    }
    // After showing result, transition away
    if (settSubPage == 11 && pendingConnectErr != 0 && connectShowErrMs > 0
        && millis() - connectShowErrMs >= 1800) {
        connectShowErrMs = 0;
        if (pendingConnectErr == 99) {
            pendingConnectErr = 0;
            settPendingPage = 5;
        } else {
            pendingConnectErr = 0;
            settPendingPage = 8;
        }
    }

    // Poll for pairing state changes on phone link page
    if (settSubPage == 6 && ble_pairing_active()) {
        if (ble_is_paired()) {
            ble_stop_pairing();
            settPendingPage = 6;
        } else if (phonePairState == 1 && ble_pairing_phone_connected()) {
            settPendingPage = 6;
        }
    }

    if (states_sub_step() == 201 && millis() - states_enter_ms() >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_UI_HOME);
        return;
    }

    if (settPendingPage < 0 || states_sub_step() != 0) {
        if (states_sub_step() == 100 && millis() - states_enter_ms() >= 200) {
            int page = settPendingPage;
            settPendingPage = -1;
            states_set_sub_step(0);

            if (page == 0) {
                destroy_sett_selection();
                settSubPage = 0;
                settBrightVal = settModelVal = settVoiceVal = settMemoryVal = NULL;
                settWifiVal = settPhoneVal = settSleepVal = NULL;
                pairCodeLabel = NULL;
                build_settings_main();
            } else {
                destroy_settings();
                settBrightVal = settModelVal = settVoiceVal = settMemoryVal = NULL;
                settWifiVal = settPhoneVal = settSleepVal = NULL;
                pairCodeLabel = NULL;
                settSubPage = (uint8_t)page;
                build_sett_selection(page);
            }
        }
        return;
    }

    if (settSubPage == 0 && settingsContainer) {
        fade_out_children(settingsContainer, 150);
    } else if (settSelContainer) {
        fade_out_children(settSelContainer, 150);
    }
    states_set_sub_step(100);
    states_set_enter_ms(millis());
}

void sett_destroy_all() {
    destroy_settings();
    destroy_sett_selection();
    if (settPortalActive) {
        portal_stop();
        settPortalActive = false;
        settPortalRestoreWifi = false;
    }
    settBrightVal = settModelVal = settVoiceVal = settMemoryVal = NULL;
    settWifiVal = settPhoneVal = settSleepVal = NULL;
    pairCodeLabel = NULL;
    devToolsVerbOvlVal = devToolsStatsPillVal = devToolsVerbBootVal = NULL;
    settSubPage = 0;
    settPendingPage = -1;
}

void sett_btn_press() {
    if (settSubPage == 8) {
        wifi_scan_cleanup();
        scanActive = false;
        settPendingPage = 5;
    } else if (settSubPage == 9) {
        t9_reset();
        settPendingPage = 5;
    } else if (settSubPage == 10) {
        t9_reset();
        settPendingPage = 8;
    } else if (settSubPage == 12) {
        settPendingPage = 4;
    } else if (settSubPage == 14) {
        settPendingPage = 4;
    } else if (settSubPage == 15) {
        settPendingPage = 14;
    } else if (settSubPage == 11) {
        // Cancel in-progress connection and clean up the entry
        wifi_disconnect();
        if (pendingConnectIdx >= 0) {
            storage_remove_wifi((uint8_t)pendingConnectIdx);
            pendingConnectIdx = -1;
        }
        wifiConnectingIdx = -1;
        pendingConnectErr = 0;
        connectShowErrMs = 0;
        settPendingPage = 5;
    } else if (settSubPage == 13) {
        exit_setup_portal();
    } else if (settSubPage == 16) {
        imu_cal_abort();
        imu_deinit();
        imuCalStatusLbl = NULL;
        settPendingPage = 0;
    } else if (settSubPage == 18) {
        aboutTapCount = 0;
        aboutDevBtn = NULL;
        aboutHintLabel = NULL;
        settPendingPage = 0;
    } else if (settSubPage == 19) {
        devToolsVerbOvlVal = devToolsStatsPillVal = devToolsVerbBootVal = NULL;
        settPendingPage = 0;
    } else if (settSubPage != 0) {
        settPendingPage = 0;
    } else {
        if (settingsContainer) fade_out_children(settingsContainer, 150);
        states_set_enter_ms(millis());
        states_set_sub_step(201);
    }
}

void sett_fade_out(uint32_t duration) {
    if (settingsContainer) fade_out_children(settingsContainer, duration);
    if (settSelContainer)  fade_out_children(settSelContainer, duration);
}

uint32_t sett_sleep_timeout_seconds(uint8_t idx) {
    if (idx >= NUM_SLEEP_OPTS) return 0;
    return SLEEP_TIMEOUTS[idx];
}

uint8_t sett_num_sleep_opts() {
    return NUM_SLEEP_OPTS;
}
