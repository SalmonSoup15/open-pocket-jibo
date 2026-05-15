#include <Arduino.h>
#include "states.h"
#include "eye.h"
#include "wifi_portal.h"
#include "storage.h"
#include "pin_config.h"
#include "audio.h"
#include "gemini.h"
#include "ble_link.h"
#include "latex_render.h"
#include "stock_render.h"
#include "stock_quote.h"
#include "pill_overlay.h"
#include "notifications.h"
#include "imu.h"
#include "phone_finder.h"
#include "crash_handler.h"
#include "event_log.h"
#include "dev_overlay.h"
#include "ui_helpers.h"
#include "ui_home.h"
#include "settings_ui.h"
#include "log.h"

// ─── Constants ──────────────────────────────────────────────────────────────
static const int16_t SCR_W = LCD_WIDTH;
static const int16_t SCR_H = LCD_HEIGHT;
static const int16_t CX    = SCR_W / 2;
static const int16_t CY    = SCR_H / 2;

static const float   EYE_HOME_Y      = CY;
static const float   EYE_TAP_Y       = CY - 30.0f;
static const float   EYE_SETUP_Y     = CY - 100.0f;
static const uint16_t ZOOM_FULL      = 256;
static const uint16_t ZOOM_SMALL     = 140;

// ─── State ──────────────────────────────────────────────────────────────────
static JiboState  state        = STATE_BOOT;
static uint32_t   stateEnterMs = 0;
static uint32_t   subStep      = 0;

// ─── LVGL widgets ───────────────────────────────────────────────────────────
static lv_obj_t  *bootRing     = NULL;
static lv_obj_t  *textLabel    = NULL;
static lv_obj_t  *textLabel2   = NULL;
static lv_obj_t  *touchOverlay = NULL;
static lv_obj_t  *btn1         = NULL;
static lv_obj_t  *btn2         = NULL;

// ─── WiFi connect state ─────────────────────────────────────────────────────
static bool connectStarted = false;

// ─── Boot button state ──────────────────────────────────────────────────────
static bool     btnDown       = false;
static bool     prevBtnDown   = false;
static uint32_t lastBtnChange = 0;
#define BTN_DEBOUNCE_MS 50

static const uint16_t ZOOM_LISTEN = 340;

// ─── Debug mode ─────────────────────────────────────────────────────────────
static lv_obj_t *debugRing  = NULL;
static bool      audioDebug = false;

// ─── Auto-sleep timer ──────────────────────────────────────────────────────
static uint32_t lastActivityMs = 0;

// ─── Sleep / Power-off state ────────────────────────────────────────────────
static JiboState preSleepState = STATE_IDLE;
static bool      sleepWifiWasOn = false;
static bool      sleepBleWasPaired = false;

// ─── Power-off slide UI ─────────────────────────────────────────────────────
static lv_obj_t *powerOffContainer = NULL;
static lv_obj_t *powerOffSlider    = NULL;
static lv_obj_t *powerOffEyeCanvas = NULL;
static uint8_t *poffEyeBuf        = NULL;
static JiboState  prePowerOffState = STATE_IDLE;

// ─── Tool display (e.g. rendered LaTeX from [show.text]{...}) ───────────────
static lv_obj_t *toolDisplayObj = NULL;

// ─── Stock-tool async fetch state ───────────────────────────────────────────
//
// show.stock can't render synchronously — it requires an HTTPS call
// to Yahoo Finance that takes ~0.5-1 s.  We start the fetch in
// enter_tool_display, show a loading card, then swap in the data card
// once tick_tool_display polls stock_is_done().
static bool       stockFetchActive = false;
static char       stockSymbol[12]  = {0};
static StockRange stockRange       = RANGE_1D;
static StockRange stockPendingRange = RANGE_1D;
static bool       stockRangeChangeRequested = false;

// ─── Forward declarations ───────────────────────────────────────────────────
static const char *state_name(JiboState s);

// ─── PMU / display helpers (defined in Jibo-ESP.ino) ────────────────────────────
extern int  pmu_battery_percent();
extern bool pmu_is_charging();

// Set by wake_from_sleep() to the millis() when we should re-enable
// notification pills.  Polled by states_tick() and reset to 0 once
// fired so the resume happens exactly once per wake cycle.
static uint32_t gPostWakeNotifResumeMs = 0;
extern void pmu_shutdown();
extern void display_off();
extern void display_on();
extern void set_display_brightness_raw(uint8_t value);
extern void power_light_sleep_for(uint32_t maxMs);
extern uint8_t get_display_brightness_mapped();

// ─── Spectrum analyzer ──────────────────────────────────────────────────────
#define FFT_N      256
#define SPEC_BARS  16
#define SPEC_BINS  (FFT_N / 2 / SPEC_BARS)   // 8 freq bins per bar
#define BAR_W      16
#define BAR_GAP    4
#define BAR_MAX_H  100

static lv_obj_t *specBars[SPEC_BARS] = {};
static float     barSmooth[SPEC_BARS] = {};
static float     barVals[SPEC_BARS]   = {};
static float     fftR[FFT_N], fftI[FFT_N];
static uint32_t  lastSpecMs   = 0;
static uint32_t  playStartMs  = 0;
static size_t    recSamples   = 0;
static bool      specPlayback = false;

static void simple_fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.2831853f / len;
        float wR = cosf(ang), wI = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cR = 1.0f, cI = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int a = i + j, b = a + len / 2;
                float tR = cR * re[b] - cI * im[b];
                float tI = cR * im[b] + cI * re[b];
                re[b] = re[a] - tR; im[b] = im[a] - tI;
                re[a] += tR;        im[a] += tI;
                float nR = cR * wR - cI * wI;
                cI = cR * wI + cI * wR; cR = nR;
            }
        }
    }
}

static void compute_spectrum(const int16_t *samples, int count) {
    if (count < FFT_N) return;
    const int16_t *src = samples + count - FFT_N;
    for (int i = 0; i < FFT_N; i++) {
        float w = 0.5f * (1.0f - cosf(6.2831853f * i / (FFT_N - 1)));
        fftR[i] = src[i] * w / 32768.0f;
        fftI[i] = 0.0f;
    }
    simple_fft(fftR, fftI, FFT_N);
    for (int b = 0; b < SPEC_BARS; b++) {
        float sum = 0;
        for (int j = 0; j < SPEC_BINS; j++) {
            int bin = b * SPEC_BINS + j + 1;
            sum += sqrtf(fftR[bin] * fftR[bin] + fftI[bin] * fftI[bin]);
        }
        barVals[b] = sum / SPEC_BINS;
    }
}

static void create_spec_bars() {
    int totalW = SPEC_BARS * (BAR_W + BAR_GAP) - BAR_GAP;
    int startX = (SCR_W - totalW) / 2;
    int bottom = CY + 100;
    for (int i = 0; i < SPEC_BARS; i++) {
        specBars[i] = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(specBars[i]);
        lv_obj_set_size(specBars[i], BAR_W, 2);
        lv_obj_set_pos(specBars[i], startX + i * (BAR_W + BAR_GAP), bottom - 2);
        lv_obj_set_style_bg_color(specBars[i], lv_color_make(40, 200, 255), 0);
        lv_obj_set_style_bg_opa(specBars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(specBars[i], 3, 0);
        barSmooth[i] = 0;
    }
}

static void update_spec_bars(bool playback) {
    lv_color_t col = playback ? lv_color_make(255, 140, 40)
                              : lv_color_make(40, 200, 255);
    int bottom = CY + 100;
    for (int i = 0; i < SPEC_BARS; i++) {
        if (!specBars[i]) continue;
        float tgt = barVals[i] * 600.0f;
        if (tgt > barSmooth[i]) barSmooth[i] = tgt;
        else                    barSmooth[i] *= 0.82f;
        int h = (int)barSmooth[i];
        if (h > BAR_MAX_H) h = BAR_MAX_H;
        if (h < 2) h = 2;
        lv_obj_set_size(specBars[i], BAR_W, h);
        lv_obj_set_y(specBars[i], bottom - h);
        lv_obj_set_style_bg_color(specBars[i], col, 0);
    }
}

static void destroy_spec_bars() {
    for (int i = 0; i < SPEC_BARS; i++) {
        if (specBars[i]) { lv_obj_del(specBars[i]); specBars[i] = NULL; }
        barSmooth[i] = 0;
    }
}

// ─── Deferred state transition (safe from inside event callbacks) ────────────
static JiboState pendingState = STATE_BOOT;
static bool      hasPending   = false;

// ─── Forward declarations ───────────────────────────────────────────────────
// enter_state() declared in states.h (non-static, visible to extracted modules)
static void create_touch_overlay();
static void destroy_touch_overlay();

// ─── Helpers ────────────────────────────────────────────────────────────────
// Shared fade/touch/widget helpers are in ui_helpers.h/cpp.
// create_label and create_dark_button are now ui_create_label / ui_create_dark_button.

static void cleanup_labels() {
    if (textLabel)  { lv_obj_del(textLabel);  textLabel  = NULL; }
    if (textLabel2) { lv_obj_del(textLabel2); textLabel2 = NULL; }
}

static void cleanup_buttons() {
    if (btn1) { lv_obj_del(btn1); btn1 = NULL; }
    if (btn2) { lv_obj_del(btn2); btn2 = NULL; }
}

// create_dark_button moved to ui_helpers.cpp as ui_ui_create_dark_button()

// ─── Eye touch tracking (for IDLE → UI transition) ─────────────────────────
static bool     eyeTouching    = false;
static uint32_t eyeTouchStart  = 0;

// ─── Touch overlay (tap-to-begin & long-press) ─────────────────────────────

static bool touch_hits_eye(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    float ex, ey;
    eye_get_pos(ex, ey);
    float dx = p.x - ex;
    float dy = p.y - ey;
    int16_t r = eye_get_radius();
    return (dx * dx + dy * dy) <= (float)(r * r);
}

static void overlay_press_cb(lv_event_t *e) {
    event_log_printf(EVT_USER_TAP, "screen in %s", state_name(state));
    if (state == STATE_TAP_TO_BEGIN) {
        pendingState = STATE_SETUP_CHOICE;
        hasPending = true;
    }
    if (state == STATE_IDLE) {
        if (!touch_hits_eye(e)) return;
        eyeTouching = true;
        eyeTouchStart = millis();
        eye_stop_idle();
        eye_set_zoom(ZOOM_FULL + 30);
    }
    if (state == STATE_TOOL_DISPLAY) {
        eyeTouching = true;
    }
}

static void overlay_release_idle_cb(lv_event_t *e) {
    if (state == STATE_IDLE && eyeTouching) {
        eyeTouching = false;
        pendingState = STATE_EYE_TO_UI;
        hasPending = true;
    }
}

static void create_touch_overlay() {
    if (touchOverlay) return;
    touchOverlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(touchOverlay);
    lv_obj_set_size(touchOverlay, SCR_W, SCR_H);
    lv_obj_set_pos(touchOverlay, 0, 0);
    lv_obj_set_style_bg_opa(touchOverlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(touchOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touchOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(touchOverlay, 0, 0);
    lv_obj_add_event_cb(touchOverlay, overlay_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touchOverlay, overlay_release_idle_cb, LV_EVENT_RELEASED, NULL);
}

static void destroy_touch_overlay() {
    if (touchOverlay) { lv_obj_del(touchOverlay); touchOverlay = NULL; }
}

// ═══════════════════════════════════════════════════════════════════════════
//  NO-INTERNET OVERLAY  (shown when talk button pressed without connection)
// ═══════════════════════════════════════════════════════════════════════════

static lv_obj_t *noInetOverlay = NULL;

static void destroy_no_inet_overlay() {
    if (noInetOverlay) { lv_obj_del(noInetOverlay); noInetOverlay = NULL; }
}

static void no_inet_ok_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    destroy_no_inet_overlay();
    eye_set_dim(0.0f);
}

static void no_inet_settings_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    destroy_no_inet_overlay();
    destroy_touch_overlay();
    eye_stop_idle();
    eye_set_dim(0.0f);
    eye_set_zoom(10);
    pendingState = STATE_SETTINGS;
    hasPending = true;
}

static void show_no_inet_overlay() {
    if (noInetOverlay) return;

    eye_set_dim(0.65f);

    noInetOverlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(noInetOverlay);
    lv_obj_set_size(noInetOverlay, SCR_W, SCR_H);
    lv_obj_set_pos(noInetOverlay, 0, 0);
    lv_obj_add_flag(noInetOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(noInetOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(noInetOverlay);
    lv_label_set_text(title, "Couldn't Connect");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *sub = lv_label_create(noInetOverlay);
    lv_label_set_text(sub, "Visit settings to\nmanage connections");
    lv_obj_set_style_text_color(sub, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, SCR_W - 60);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -5);

    // Settings button
    lv_obj_t *settBtn = lv_btn_create(noInetOverlay);
    lv_obj_set_size(settBtn, 180, 46);
    lv_obj_align(settBtn, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_bg_opa(settBtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(settBtn, lv_color_white(), 0);
    lv_obj_set_style_border_width(settBtn, 2, 0);
    lv_obj_set_style_border_opa(settBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(settBtn, 23, 0);
    lv_obj_set_style_shadow_width(settBtn, 0, 0);
    lv_obj_set_style_bg_opa(settBtn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(settBtn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(settBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(settBtn, no_inet_settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sLbl = lv_label_create(settBtn);
    lv_label_set_text(sLbl, "Settings");
    lv_obj_set_style_text_color(sLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(sLbl);

    // OK button
    lv_obj_t *okBtn = lv_btn_create(noInetOverlay);
    lv_obj_set_size(okBtn, 180, 46);
    lv_obj_align(okBtn, LV_ALIGN_CENTER, 0, 115);
    lv_obj_set_style_bg_opa(okBtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(okBtn, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_border_width(okBtn, 1, 0);
    lv_obj_set_style_border_opa(okBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(okBtn, 23, 0);
    lv_obj_set_style_shadow_width(okBtn, 0, 0);
    lv_obj_set_style_bg_opa(okBtn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(okBtn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(okBtn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(okBtn, no_inet_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *oLbl = lv_label_create(okBtn);
    lv_label_set_text(oLbl, "OK");
    lv_obj_set_style_text_color(oLbl, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(oLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(oLbl);

    // Fade in all children
    fade_in_children(noInetOverlay, 250);
}

// ═══════════════════════════════════════════════════════════════════════════
//  CONNECT-FAILURE UI  (used after wifi setup → connecting fails)
// ═══════════════════════════════════════════════════════════════════════════

static void try_again_connect_cb(lv_event_t *e) {
    pendingState = STATE_CONNECTING;
    hasPending = true;
}

static void enter_setup_cb(lv_event_t *e) {
    pendingState = STATE_WIFI_SETUP;
    hasPending = true;
}

static void show_connect_failure_ui() {
    eye_set_pos((float)CX, EYE_SETUP_Y);
    eye_set_zoom(ZOOM_SMALL);

    uint8_t err = wifi_connect_error();
    const char *msg;
    switch (err) {
        case 1:  msg = "Wrong password"; break;
        case 2:  msg = "Network not found"; break;
        default: msg = "Couldn't connect"; break;
    }

    lv_coord_t labelY = (lv_coord_t)(EYE_SETUP_Y + 80);
    textLabel = ui_create_label(msg, labelY, &lv_font_montserrat_24);
    fade_in_label(textLabel, 400);

    btn1 = ui_create_dark_button("Try Again", labelY + 50, try_again_connect_cb);
    btn2 = ui_create_dark_button("Enter Setup", labelY + 115, enter_setup_cb);
    fade_in_obj(btn1, 400);
    fade_in_obj(btn2, 400);
}

// ═══════════════════════════════════════════════════════════════════════════
//  STATE ENTER
// ═══════════════════════════════════════════════════════════════════════════

static void enter_boot();
static void enter_tap_to_begin();
static void enter_setup_choice();
static void tick_setup_choice();
static void enter_phone_setup();
static void tick_phone_setup();
static void enter_wifi_setup();
static void enter_connecting();
static void enter_connected();
static void enter_idle();
static void enter_eye_to_ui();
static void enter_ui_to_eye();
static void enter_listening();
static void enter_thinking();
static void enter_speaking();
static void states_check_lowbat_on_wake();
static void enter_tool_display();
static void tick_tool_display();
static void exit_tool_display();
static void destroy_tool_display();
static void enter_audio_debug();
static void enter_sleep();
static void enter_power_off_slide();
static void tick_sleep();
static void tick_power_off_slide();
static void cancel_power_off_slide();
static void wake_from_sleep();
static void enter_phone_finder();
static void tick_phone_finder();
static void enter_imu_debug();
static void tick_imu_debug();
static void exit_imu_debug();
static void enter_dev_transition();
static void tick_dev_transition();
static void destroy_dev_transition_ui();

static const char *state_name(JiboState s) {
    switch (s) {
        case STATE_BOOT:         return "BOOT";
        case STATE_TAP_TO_BEGIN: return "TAP_TO_BEGIN";
        case STATE_SETUP_CHOICE: return "SETUP_CHOICE";
        case STATE_PHONE_SETUP:  return "PHONE_SETUP";
        case STATE_WIFI_SETUP:   return "WIFI_SETUP";
        case STATE_CONNECTING:   return "CONNECTING";
        case STATE_CONNECTED:    return "CONNECTED";
        case STATE_IDLE:         return "IDLE";
        case STATE_EYE_TO_UI:   return "EYE_TO_UI";
        case STATE_UI_HOME:     return "UI_HOME";
        case STATE_SETTINGS:    return "SETTINGS";
        case STATE_UI_TO_EYE:   return "UI_TO_EYE";
        case STATE_LISTENING:   return "LISTENING";
        case STATE_THINKING:    return "THINKING";
        case STATE_SPEAKING:    return "SPEAKING";
        case STATE_TOOL_DISPLAY: return "TOOL_DISPLAY";
        case STATE_AUDIO_DEBUG:      return "AUDIO_DEBUG";
        case STATE_SLEEP:            return "SLEEP";
        case STATE_POWER_OFF_SLIDE:  return "POWER_OFF_SLIDE";
        case STATE_PHONE_FINDER:     return "PHONE_FINDER";
        case STATE_IMU_DEBUG:        return "IMU_DEBUG";
        case STATE_DEV_TRANSITION:   return "DEV_TRANSITION";
        default:                     return "???";
    }
}

void enter_state(JiboState s) {
    LOG1(">> enter_state(%s) heap=%u\n", state_name(s), ESP.getFreeHeap());
    event_log_printf(EVT_STATE_CHANGE, "%s h=%u", state_name(s), ESP.getFreeHeap());
    crash_handler_update_state(state_name(s));
    {
        char _ovl[64];
        snprintf(_ovl, sizeof(_ovl), "> %s", state_name(s));
        dev_overlay_log(_ovl);
    }
    state = s;
    stateEnterMs = millis();
    lastActivityMs = millis();
    subStep = 0;

    switch (s) {
        case STATE_BOOT:          enter_boot();          break;
        case STATE_TAP_TO_BEGIN:  enter_tap_to_begin();  break;
        case STATE_SETUP_CHOICE:  enter_setup_choice();  break;
        case STATE_PHONE_SETUP:   enter_phone_setup();   break;
        case STATE_WIFI_SETUP:    enter_wifi_setup();    break;
        case STATE_CONNECTING:    enter_connecting();    break;
        case STATE_CONNECTED:     enter_connected();     break;
        case STATE_IDLE:          enter_idle();          break;
        case STATE_EYE_TO_UI:    enter_eye_to_ui();    break;
        case STATE_UI_HOME:
            cleanup_labels(); cleanup_buttons();
            sett_destroy_all();
            ui_home_init();
            break;
        case STATE_SETTINGS:
            ui_home_destroy();
            destroy_no_inet_overlay();
            sett_init();
            break;
        case STATE_UI_TO_EYE:    enter_ui_to_eye();    break;
        case STATE_LISTENING:    enter_listening();    break;
        case STATE_THINKING:     enter_thinking();     break;
        case STATE_SPEAKING:     enter_speaking();     break;
        case STATE_TOOL_DISPLAY: enter_tool_display(); break;
        case STATE_AUDIO_DEBUG:      enter_audio_debug();      break;
        case STATE_SLEEP:            enter_sleep();            break;
        case STATE_POWER_OFF_SLIDE:  enter_power_off_slide();  break;
        case STATE_PHONE_FINDER:     enter_phone_finder();     break;
        case STATE_IMU_DEBUG:        enter_imu_debug();        break;
        case STATE_DEV_TRANSITION:   enter_dev_transition();   break;
    }
}

// ─── BOOT ───────────────────────────────────────────────────────────────────

static void set_full_refresh(bool on) {
    lv_disp_t *d = lv_disp_get_default();
    if (d && d->driver) d->driver->full_refresh = on ? 1 : 0;
}

// "Jibo" boot: "Jib" label + eye as "o", then text fades, eye grows to center
static lv_obj_t *bootLabel = NULL;
static const int16_t BOOT_EYE_ZOOM = 72;

static uint32_t bootFadeStart = 0;

// ─── Dev-transition boot screen ────────────────────────────────────────────
// Shown on the first boot after toggling dev mode via the slide-to-transition
// screen.  Displays "Transitioning to developer mode" (or stock) with a
// progress bar, then continues to normal boot.
static bool      devTransBoot      = false;   // true = this boot is a transition
static bool      devTransToDevMode = false;   // what we're transitioning TO
static lv_obj_t *devTransBootScr   = NULL;    // container for the screen
static lv_obj_t *devTransBootBar   = NULL;    // the progress bar

static void destroy_dev_trans_boot_scr() {
    if (devTransBootScr) { lv_obj_del(devTransBootScr); devTransBootScr = NULL; }
    devTransBootBar = NULL;
}

static void enter_boot() {
    eye_hide();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();

    if (bootRing) { lv_obj_del(bootRing); bootRing = NULL; }
    if (bootLabel) { lv_obj_del(bootLabel); bootLabel = NULL; }
    destroy_dev_trans_boot_scr();

    // Check if this boot is a dev-mode transition (flag set before reboot)
    devTransBoot = storage_get_dev_transitioning();
    devTransToDevMode = g_dev_mode;  // we already loaded g_dev_mode from NVS

    // Kick off a WiFi scan now so by the time the boot animation
    // finishes (~5 s later) we already know which saved networks are in
    // range — wifi_start_connect() can then aim at the strongest one
    // instead of trying them in saved order.  Skipped on the very first
    // boot (no networks saved yet) and when WiFi is disabled.
    if (storage_is_setup_done() && wifi_is_enabled() &&
        storage_wifi_count() > 0) {
        wifi_boot_prescan();
    }

    // If this is a dev-transition boot, show the transition screen
    // instead of the normal "Jib" logo animation.
    if (devTransBoot) {
        subStep = 20;

        devTransBootScr = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(devTransBootScr);
        lv_obj_set_size(devTransBootScr, SCR_W, SCR_H);
        lv_obj_set_pos(devTransBootScr, 0, 0);
        lv_obj_set_style_bg_color(devTransBootScr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(devTransBootScr, LV_OPA_COVER, 0);
        lv_obj_clear_flag(devTransBootScr, LV_OBJ_FLAG_SCROLLABLE);

        // Transition message
        const char *msg = devTransToDevMode
            ? "Transitioning to\ndeveloper mode"
            : "Transitioning to\nstock mode";
        lv_obj_t *lbl = lv_label_create(devTransBootScr);
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, 300);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

        // Progress bar
        devTransBootBar = lv_bar_create(devTransBootScr);
        lv_obj_set_size(devTransBootBar, 200, 8);
        lv_obj_align(devTransBootBar, LV_ALIGN_CENTER, 0, 40);
        lv_bar_set_range(devTransBootBar, 0, 100);
        lv_bar_set_value(devTransBootBar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(devTransBootBar, lv_color_make(40, 40, 40), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(devTransBootBar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(devTransBootBar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(devTransBootBar,
            devTransToDevMode ? lv_color_make(60, 120, 200) : lv_color_make(120, 120, 120),
            LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(devTransBootBar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(devTransBootBar, 4, LV_PART_INDICATOR);

        // Subtitle
        lv_obj_t *sub = lv_label_create(devTransBootScr);
        lv_label_set_text(sub, "Please wait...");
        lv_obj_set_style_text_color(sub, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 75);

        // Fade in the whole container
        fade_in_children(devTransBootScr, 400);

        stateEnterMs = millis();
        return;
    }

    bootLabel = lv_label_create(lv_scr_act());
    lv_label_set_text(bootLabel, "Jib");
    lv_obj_set_style_text_color(bootLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(bootLabel, &lv_font_montserrat_48, 0);
    lv_obj_set_style_opa(bootLabel, LV_OPA_COVER, 0);
    lv_obj_align(bootLabel, LV_ALIGN_CENTER, -32, 0);

    eye_set_pos_immediate((float)(CX + 38), (float)(CY + 2));
    eye_set_zoom_immediate(BOOT_EYE_ZOOM);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);

    bootFadeStart = 0;
    subStep = 0;
}

static void tick_boot() {
    // Phase 0: show eye hidden, let render_tick run once to rasterize sphere,
    // then start a quick fade-in
    if (subStep == 0) {
        lv_obj_t *ec = eye_get_canvas();
        if (!ec) { subStep = 1; stateEnterMs = millis(); return; }

        if (bootFadeStart == 0) {
            lv_obj_set_style_opa(ec, LV_OPA_TRANSP, 0);
            eye_show();
            bootFadeStart = millis();
            return;
        }

        uint32_t fadeElapsed = millis() - bootFadeStart;
        if (fadeElapsed < 250) {
            uint8_t opa = (uint8_t)(fadeElapsed * 255 / 250);
            lv_obj_set_style_opa(ec, (lv_opa_t)opa, 0);
        } else {
            lv_obj_set_style_opa(ec, LV_OPA_COVER, 0);
            subStep = 1;
            stateEnterMs = millis();
        }
        return;
    }

    uint32_t elapsed = millis() - stateEnterMs;

    // Phase 1: hold the "Jibo" display, then pulse blue if in dev mode
    if (subStep == 1 && elapsed >= 1200) {
        if (g_dev_mode) {
            // Dev mode: pulse the eye blue while "Jib" text is still showing
            eye_set_tint(1.0f);
            subStep = 10;
            stateEnterMs = millis();
        } else {
            // Stock mode: proceed straight to text fade-out
            fade_out_label(bootLabel, 500);
            subStep = 2;
            stateEnterMs = millis();
        }
    }

    // Phase 10 (dev mode): hold blue pulse for 800ms, then fade tint off
    if (subStep == 10 && millis() - stateEnterMs >= 800) {
        eye_set_tint(0.0f);
        subStep = 11;
        stateEnterMs = millis();
    }

    // Phase 11: blue tint faded, now fade out "Jib" text (same as stock phase 2)
    if (subStep == 11 && millis() - stateEnterMs >= 300) {
        fade_out_label(bootLabel, 500);
        subStep = 2;
        stateEnterMs = millis();
    }

    // Phase 2: text gone, grow and center the eye
    if (subStep == 2 && millis() - stateEnterMs >= 600) {
        if (bootLabel) { lv_obj_del(bootLabel); bootLabel = NULL; }
        eye_set_pos((float)CX, (float)CY);
        eye_set_zoom_speed(0.048f);
        eye_set_zoom(ZOOM_FULL);
        subStep = 3;
        stateEnterMs = millis();
    }

    // Phase 3: wait for the grow transition to complete, then go to idle/setup
    if (subStep == 3 && millis() - stateEnterMs >= 2100) {
        eye_set_zoom_speed(0.35f);
        if (storage_is_setup_done()) {
            enter_state(STATE_IDLE);
        } else {
            enter_state(STATE_TAP_TO_BEGIN);
        }
    }

    // ── Dev-transition boot phases (20-23) ─────────────────────────────
    // Shown when the device just rebooted after toggling dev mode.
    // Phase 20: fill the progress bar over 2 seconds
    static const uint32_t DEV_TRANS_FILL_MS = 2000;
    if (subStep == 20) {
        uint32_t e = millis() - stateEnterMs;
        if (e < DEV_TRANS_FILL_MS) {
            int pct = (int)(e * 100 / DEV_TRANS_FILL_MS);
            if (devTransBootBar) lv_bar_set_value(devTransBootBar, pct, LV_ANIM_OFF);
        } else {
            if (devTransBootBar) lv_bar_set_value(devTransBootBar, 100, LV_ANIM_OFF);
            subStep = 21;
            stateEnterMs = millis();
        }
    }

    // Phase 21: hold full bar for 400ms
    if (subStep == 21 && millis() - stateEnterMs >= 400) {
        // Clear the transition flag so the next normal reboot won't show this
        storage_set_dev_transitioning(false);

        // Fade out the transition screen
        fade_out_children(devTransBootScr, 500);
        subStep = 22;
        stateEnterMs = millis();
    }

    // Phase 22: after fade-out, destroy the screen and restart.
    // The device does a full reboot into the new mode — the blue eye
    // pulse (dev) or normal boot (stock) runs on the fresh start.
    if (subStep == 22 && millis() - stateEnterMs >= 600) {
        destroy_dev_trans_boot_scr();
        display_off();
        if (g_dev_mode) Serial.flush();
        delay(200);
        crash_handler_pre_restart();
        esp_restart();
    }
}

// ─── TAP TO BEGIN ───────────────────────────────────────────────────────────

static void enter_tap_to_begin() {
    cleanup_buttons();
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);
    eye_set_zoom(ZOOM_FULL);
    eye_show();
    subStep = 0;
}

static void tick_tap_to_begin() {
    uint32_t elapsed = millis() - stateEnterMs;

    if (subStep == 0 && elapsed >= 1000) {
        eye_set_pos((float)CX, EYE_TAP_Y);
        subStep = 1;
    }
    if (subStep == 1 && elapsed >= 1800) {
        textLabel = ui_create_label("Tap to begin", (int)(EYE_TAP_Y + 120), &lv_font_montserrat_20);
        fade_in_label(textLabel, 600);

        eye_start_blink_loop(3000, 6000);
        create_touch_overlay();

        subStep = 2;
    }
}

// ─── SETUP CHOICE ──────────────────────────────────────────────────────────

static lv_obj_t *setupChoiceHelper = NULL;

static void destroy_setup_choice_ui() {
    // textLabel, textLabel2, btn1, btn2 are cleaned by cleanup_labels/cleanup_buttons
    if (setupChoiceHelper) { lv_obj_del(setupChoiceHelper); setupChoiceHelper = NULL; }
}

static void phone_setup_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (textLabel)        fade_out_label(textLabel, 200);
    if (textLabel2)       fade_out_label(textLabel2, 200);
    if (btn1)             fade_out_obj(btn1, 200);
    if (btn2)             fade_out_obj(btn2, 200);
    if (setupChoiceHelper) fade_out_label(setupChoiceHelper, 200);
    stateEnterMs = millis();
    subStep = 10;  // fade-out → STATE_PHONE_SETUP
}

static void manual_setup_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (textLabel)        fade_out_label(textLabel, 200);
    if (textLabel2)       fade_out_label(textLabel2, 200);
    if (btn1)             fade_out_obj(btn1, 200);
    if (btn2)             fade_out_obj(btn2, 200);
    if (setupChoiceHelper) fade_out_label(setupChoiceHelper, 200);
    stateEnterMs = millis();
    subStep = 11;  // fade-out → STATE_WIFI_SETUP
}

static void enter_setup_choice() {
    eye_stop_blink_loop();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();

    eye_show();
    eye_set_pos((float)CX, EYE_SETUP_Y);
    eye_set_zoom(ZOOM_SMALL);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);

    subStep = 0;
}

static void tick_setup_choice() {
    uint32_t elapsed = millis() - stateEnterMs;

    if (subStep == 0 && elapsed >= 800) {
        lv_coord_t labelY = (lv_coord_t)(EYE_SETUP_Y + 75);

        textLabel = lv_label_create(lv_scr_act());
        lv_label_set_text(textLabel, "Set up with phone?");
        lv_obj_set_style_text_color(textLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(textLabel, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(textLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(textLabel, SCR_W - 40);
        lv_obj_align(textLabel, LV_ALIGN_TOP_MID, 0, labelY);

        textLabel2 = lv_label_create(lv_scr_act());
        lv_label_set_text(textLabel2, "(Recommended)");
        lv_obj_set_style_text_color(textLabel2, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(textLabel2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(textLabel2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(textLabel2, LV_ALIGN_TOP_MID, 0, labelY + 25);

        btn1 = ui_create_dark_button("Yes, use phone", labelY + 55, phone_setup_cb);
        btn2 = ui_create_dark_button("Set up manually", labelY + 120, manual_setup_cb);

        if (setupChoiceHelper) { lv_obj_del(setupChoiceHelper); setupChoiceHelper = NULL; }
        setupChoiceHelper = lv_label_create(lv_scr_act());
        lv_label_set_text(setupChoiceHelper, "You can set up phone\nlink later in settings");
        lv_obj_set_style_text_color(setupChoiceHelper, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(setupChoiceHelper, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(setupChoiceHelper, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(setupChoiceHelper, SCR_W - 40);
        lv_obj_align(setupChoiceHelper, LV_ALIGN_BOTTOM_MID, 0, -30);

        fade_in_label(textLabel, 400);
        fade_in_label(textLabel2, 400);
        fade_in_obj(btn1, 400);
        fade_in_obj(btn2, 400);
        fade_in_label(setupChoiceHelper, 400);

        subStep = 1;
    }
    // subStep 1: wait for button tap (handled by callbacks → subStep 10/11)

    if (subStep == 10 && millis() - stateEnterMs >= 250) {
        enter_state(STATE_PHONE_SETUP);
        return;
    }
    if (subStep == 11 && millis() - stateEnterMs >= 250) {
        enter_state(STATE_WIFI_SETUP);
        return;
    }
}

// ─── PHONE SETUP (BLE pairing + phone-driven config) ──────────────────────

static lv_obj_t *phoneSetupContainer = NULL;
static lv_obj_t *phoneSetupSpinner   = NULL;
static lv_obj_t *phoneSetupCodeLabel = NULL;
static uint32_t  phoneSetupPollMs    = 0;
static uint8_t   phoneSetupBleErr    = 0;   // cached for fade-out delay
static const char *phoneSetupErrMsg  = NULL;

static void destroy_phone_setup_ui() {
    if (phoneSetupContainer) { lv_obj_del(phoneSetupContainer); phoneSetupContainer = NULL; }
    phoneSetupSpinner = NULL;
    phoneSetupCodeLabel = NULL;
}

static void phone_setup_try_again_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
    if (btn1) fade_out_obj(btn1, 200);
    if (btn2) fade_out_obj(btn2, 200);
    stateEnterMs = millis();
    subStep = 25;  // fade-out → re-enter phone setup
}

static void phone_setup_manual_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
    if (btn1) fade_out_obj(btn1, 200);
    if (btn2) fade_out_obj(btn2, 200);
    stateEnterMs = millis();
    subStep = 26;  // fade-out → enter manual wifi setup
}

static void enter_phone_setup() {
    cleanup_labels();
    cleanup_buttons();
    destroy_setup_choice_ui();
    destroy_phone_setup_ui();

    eye_show();
    eye_set_pos((float)CX, EYE_SETUP_Y);
    eye_set_zoom(ZOOM_SMALL);

    ble_clear_setup_data();
    ble_start_pairing();

    subStep = 0;
}

static void tick_phone_setup() {
    uint32_t elapsed = millis() - stateEnterMs;

    // ── subStep 0: build "waiting for phone" UI after short delay ────────
    if (subStep == 0 && elapsed >= 600) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        phoneSetupSpinner = lv_spinner_create(phoneSetupContainer, 1000, 60);
        lv_obj_set_size(phoneSetupSpinner, 50, 50);
        lv_obj_align(phoneSetupSpinner, LV_ALIGN_CENTER, 0, -10);
        lv_obj_set_style_arc_width(phoneSetupSpinner, 4, 0);
        lv_obj_set_style_arc_color(phoneSetupSpinner, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_arc_width(phoneSetupSpinner, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(phoneSetupSpinner, lv_color_white(), LV_PART_INDICATOR);

        lv_obj_t *lbl = lv_label_create(phoneSetupContainer);
        lv_label_set_text(lbl, "Open the Jibo app\non your phone");
        lv_obj_set_style_text_color(lbl, lv_color_make(180, 180, 180), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, SCR_W - 40);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 50);

        lv_obj_t *sub = lv_label_create(phoneSetupContainer);
        lv_label_set_text(sub, "Waiting for connection...");
        lv_obj_set_style_text_color(sub, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(sub, SCR_W - 40);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 90);

        fade_in_children(phoneSetupContainer, 400);
        phoneSetupPollMs = millis();
        subStep = 1;
        return;
    }

    // ── subStep 1: poll for phone BLE connection → fade out → show code ────
    if (subStep == 1) {
        if (millis() - phoneSetupPollMs < 200) return;
        phoneSetupPollMs = millis();

        if (ble_pairing_phone_connected()) {
            if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
            stateEnterMs = millis();
            subStep = 20;  // wait for fade-out → build code UI
        }
        return;
    }
    if (subStep == 20 && millis() - stateEnterMs >= 250) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(phoneSetupContainer);
        lv_label_set_text(lbl, "Enter this code\nin the Jibo app:");
        lv_obj_set_style_text_color(lbl, lv_color_make(180, 180, 180), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, SCR_W - 40);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

        phoneSetupCodeLabel = lv_label_create(phoneSetupContainer);
        char codeStr[8];
        snprintf(codeStr, sizeof(codeStr), "%04u", ble_get_pairing_code());
        lv_label_set_text(phoneSetupCodeLabel, codeStr);
        lv_obj_set_style_text_color(phoneSetupCodeLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(phoneSetupCodeLabel, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_align(phoneSetupCodeLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(phoneSetupCodeLabel, LV_ALIGN_CENTER, 0, 30);

        lv_obj_t *sub = lv_label_create(phoneSetupContainer);
        lv_label_set_text(sub, "Waiting for code entry...");
        lv_obj_set_style_text_color(sub, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(sub, SCR_W - 40);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 75);

        fade_in_children(phoneSetupContainer, 400);
        subStep = 2;
        return;
    }

    // ── subStep 2: poll for pairing confirmed → fade out → "continue on phone"
    if (subStep == 2) {
        if (millis() - phoneSetupPollMs < 200) return;
        phoneSetupPollMs = millis();

        if (ble_is_paired()) {
            ble_send_setup_mode(true);
            if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
            stateEnterMs = millis();
            subStep = 21;  // wait for fade-out → build "continue on phone" UI
        }
        return;
    }
    if (subStep == 21 && millis() - stateEnterMs >= 250) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(phoneSetupContainer);
        lv_label_set_text(lbl, "Continue setup\non your phone");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, SCR_W - 40);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

        phoneSetupSpinner = lv_spinner_create(phoneSetupContainer, 1000, 60);
        lv_obj_set_size(phoneSetupSpinner, 40, 40);
        lv_obj_align(phoneSetupSpinner, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_style_arc_width(phoneSetupSpinner, 4, 0);
        lv_obj_set_style_arc_color(phoneSetupSpinner, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_arc_width(phoneSetupSpinner, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(phoneSetupSpinner, lv_color_white(), LV_PART_INDICATOR);

        lv_obj_t *sub = lv_label_create(phoneSetupContainer);
        lv_label_set_text(sub, "Waiting for configuration...");
        lv_obj_set_style_text_color(sub, lv_color_make(100, 100, 100), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(sub, SCR_W - 40);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 70);

        fade_in_children(phoneSetupContainer, 400);
        subStep = 3;
        return;
    }

    // ── subStep 3: poll for SETUP_COMPLETE from phone ────────────────────
    if (subStep == 3) {
        if (millis() - phoneSetupPollMs < 200) return;
        phoneSetupPollMs = millis();

        if (ble_setup_complete_received()) {
            // Save WiFi credentials if received
            if (ble_setup_wifi_received()) {
                BleSetupWifi w = ble_get_setup_wifi();
                WifiEntry entry;
                entry.ssid       = w.ssid;
                entry.password   = w.password;
                entry.username   = w.username;
                entry.enterprise = w.enterprise;
                storage_set_wifi(0, entry);
                storage_set_wifi_count(1);
                LOG1("[phone-setup] WiFi saved: %s\n", w.ssid.c_str());
            }

            // Save API keys if received
            if (ble_setup_keys_received()) {
                String apiKey, ttsKey;
                ble_get_setup_keys(apiKey, ttsKey);
                if (apiKey.length() > 0) storage_set_api_key(apiKey);
                if (ttsKey.length() > 0) storage_set_tts_key(ttsKey);
                LOG1("[phone-setup] API keys saved\n");
            }

            ble_send_setup_status(0);  // wifi_connecting

            if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
            stateEnterMs = millis();
            subStep = 22;  // wait for fade-out → build connecting UI
        }
        return;
    }
    if (subStep == 22 && millis() - stateEnterMs >= 250) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(phoneSetupContainer);
        lv_label_set_text(lbl, "Connecting...");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, SCR_W - 40);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        fade_in_children(phoneSetupContainer, 300);

        eye_start_lookaround(800, 2000);
        wifi_start_connect();
        connectStarted = true;
        subStep = 4;
        return;
    }

    // ── subStep 4: poll WiFi connection result ───────────────────────────
    if (subStep == 4) {
        wifi_connect_tick();

        if (wifi_is_connected()) {
            eye_stop_lookaround();
            ble_send_setup_status(1);  // wifi_connected
            storage_set_setup_done(true);
            ble_send_setup_mode(false);
            ble_send_setup_status(5);  // all_done

            if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
            stateEnterMs = millis();
            subStep = 23;  // fade-out → build "Connected!" UI
        } else if (wifi_connect_failed()) {
            eye_stop_lookaround();

            uint8_t err = wifi_connect_error();
            switch (err) {
                case 1:  phoneSetupBleErr = 2; phoneSetupErrMsg = "Wrong password";   break;
                case 2:  phoneSetupBleErr = 3; phoneSetupErrMsg = "Network not found"; break;
                default: phoneSetupBleErr = 4; phoneSetupErrMsg = "Couldn't connect";  break;
            }
            ble_send_setup_status(phoneSetupBleErr);

            if (phoneSetupContainer) fade_out_children(phoneSetupContainer, 200);
            stateEnterMs = millis();
            subStep = 24;  // fade-out → build error UI
        }
        return;
    }
    // ── subStep 23: build "Connected!" after fade-out ────────────────────
    if (subStep == 23 && millis() - stateEnterMs >= 250) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        textLabel = lv_label_create(phoneSetupContainer);
        lv_label_set_text(textLabel, "Connected!");
        lv_obj_set_style_text_color(textLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(textLabel, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(textLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(textLabel, SCR_W - 40);
        lv_obj_align(textLabel, LV_ALIGN_CENTER, 0, 0);

        fade_in_children(phoneSetupContainer, 400);
        stateEnterMs = millis();
        subStep = 5;
        return;
    }
    // ── subStep 24: build error UI after fade-out ────────────────────────
    if (subStep == 24 && millis() - stateEnterMs >= 250) {
        destroy_phone_setup_ui();

        phoneSetupContainer = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(phoneSetupContainer);
        lv_obj_set_size(phoneSetupContainer, SCR_W, SCR_H);
        lv_obj_set_pos(phoneSetupContainer, 0, 0);
        lv_obj_clear_flag(phoneSetupContainer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *errLbl = lv_label_create(phoneSetupContainer);
        lv_label_set_text(errLbl, phoneSetupErrMsg);
        lv_obj_set_style_text_color(errLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(errLbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(errLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(errLbl, SCR_W - 40);
        lv_obj_align(errLbl, LV_ALIGN_CENTER, 0, -30);

        btn1 = ui_create_dark_button("Try Again", CY + 10, phone_setup_try_again_cb);
        btn2 = ui_create_dark_button("Enter Setup", CY + 75, phone_setup_manual_cb);

        fade_in_children(phoneSetupContainer, 400);
        fade_in_obj(btn1, 400);
        fade_in_obj(btn2, 400);

        subStep = 10;
        return;
    }

    // ── subStep 5: "Connected!" display → transition to IDLE ─────────────
    if (subStep == 5) {
        uint32_t sinceConnected = millis() - stateEnterMs;
        if (sinceConnected >= 3500 && textLabel) {
            fade_out_label(textLabel, 500);
            subStep = 6;
        }
        return;
    }
    if (subStep == 6) {
        uint32_t sinceConnected = millis() - stateEnterMs;
        if (sinceConnected >= 4200) {
            cleanup_labels();
            destroy_phone_setup_ui();
            enter_state(STATE_IDLE);
        }
        return;
    }

    // ── subStep 10: error state — wait for button taps ───────────────────
    // (callbacks set subStep 25/26 after triggering fade-outs)

    if (subStep == 25 && millis() - stateEnterMs >= 250) {
        enter_state(STATE_PHONE_SETUP);
        return;
    }
    if (subStep == 26 && millis() - stateEnterMs >= 250) {
        enter_state(STATE_WIFI_SETUP);
        return;
    }
}

// ─── WIFI SETUP ─────────────────────────────────────────────────────────────

static void enter_wifi_setup() {
    eye_stop_blink_loop();
    eye_stop_lookaround();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();
    destroy_setup_choice_ui();
    destroy_phone_setup_ui();

    eye_show();
    eye_set_pos((float)CX, EYE_SETUP_Y);
    eye_set_zoom(ZOOM_SMALL);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);

    subStep = 0;
}

static void tick_wifi_setup() {
    uint32_t elapsed = millis() - stateEnterMs;

    if (subStep == 0 && elapsed >= 1200) {
        uint16_t suffix = storage_device_suffix();
        char passStr[64];
        snprintf(passStr, sizeof(passStr), "Password: Jibo%04d", suffix);

        int labelY = (int)(EYE_SETUP_Y + 80);
        textLabel  = ui_create_label("Wi-Fi: Jibo", labelY, &lv_font_montserrat_20);
        textLabel2 = ui_create_label(passStr, labelY + 32, &lv_font_montserrat_20);
        fade_in_label(textLabel, 400);
        fade_in_label(textLabel2, 400);

        char apPass[16];
        snprintf(apPass, sizeof(apPass), "Jibo%04d", suffix);
        portal_start("Jibo", String(apPass));

        subStep = 1;
    }
    if (subStep == 1) {
        portal_tick();
        if (portal_save_received()) {
            portal_stop();
            enter_state(STATE_CONNECTING);
        }
    }
}

// ─── CONNECTING ─────────────────────────────────────────────────────────────

static void enter_connecting() {
    cleanup_labels();
    cleanup_buttons();

    eye_show();
    eye_set_pos((float)CX, EYE_TAP_Y);
    eye_set_zoom(ZOOM_FULL);
    eye_set_light(0.0f, 0.0f);

    eye_start_lookaround(800, 2000);

    connectStarted = false;
    subStep = 0;
}

static void tick_connecting() {
    uint32_t elapsed = millis() - stateEnterMs;

    if (subStep == 0 && elapsed >= 600) {
        textLabel = ui_create_label("Connecting", (int)(EYE_TAP_Y + 120), &lv_font_montserrat_20);
        fade_in_label(textLabel, 400);
        subStep = 1;
    }
    if (subStep == 1 && elapsed >= 1200) {
        wifi_start_connect();
        connectStarted = true;
        subStep = 2;
    }
    if (subStep == 2 && connectStarted) {
        wifi_connect_tick();

        if (wifi_is_connected()) {
            eye_stop_lookaround();
            storage_set_setup_done(true);
            enter_state(STATE_CONNECTED);
        } else if (wifi_connect_failed()) {
            eye_stop_lookaround();
            cleanup_labels();
            show_connect_failure_ui();
            subStep = 10;
        }
    }
}

// ─── CONNECTED ──────────────────────────────────────────────────────────────

static void enter_connected() {
    eye_stop_lookaround();
    cleanup_labels();
    cleanup_buttons();

    subStep = 0;
}

static void tick_connected() {
    uint32_t elapsed = millis() - stateEnterMs;

    if (subStep == 0 && elapsed >= 300) {
        textLabel = ui_create_label("Connected!", (int)(EYE_TAP_Y + 120), &lv_font_montserrat_24);
        fade_in_label(textLabel, 400);
        subStep = 1;
    }
    if (subStep == 1 && elapsed >= 3500) {
        fade_out_label(textLabel, 500);
        subStep = 2;
    }
    if (subStep == 2 && elapsed >= 4200) {
        cleanup_labels();
        enter_state(STATE_IDLE);
    }
}

// ─── IDLE ───────────────────────────────────────────────────────────────────

static void enter_idle() {
    cleanup_labels();
    cleanup_buttons();
    destroy_no_inet_overlay();
    destroy_tool_display();

    // First-time IDLE entry after boot ends the silent boot-grace
    // window so subsequent genuinely-new notifications can pill.  This
    // is idempotent — only the first call actually does anything.
    notifications_boot_complete();

    eye_set_pos((float)CX, (float)CY);
    eye_set_zoom(ZOOM_FULL);
    eye_set_tint(0.0f);
    eye_set_dim(0.0f);
    eye_set_light_follows_pos(true);
    eye_set_look_radius(60.0f);
    eye_start_idle();

    create_touch_overlay();
    eyeTouching = false;

    if (storage_is_setup_done() && wifi_is_enabled() && !wifi_is_connected()) {
        wifi_start_connect();
    }
}

static void tick_idle() {
    wifi_connect_tick();
}

// ─── EYE → UI transition (eye shrinks to nothing, UI fades in) ──────────────

static void enter_eye_to_ui() {
    eye_stop_idle();
    destroy_touch_overlay();
    cleanup_labels();
    cleanup_buttons();

    eye_set_zoom(10);
    subStep = 0;
}

static void tick_eye_to_ui() {
    uint32_t elapsed = millis() - stateEnterMs;
    if (subStep == 0 && elapsed >= 350) {
        eye_hide();
        subStep = 1;
        enter_state(STATE_UI_HOME);
    }
}

// ─── UI HOME — extracted to ui_home.h/cpp ──────────────────────────────────

// ─── SETTINGS — extracted to settings_ui.h/cpp ────────────────────────────

// ─── UI → EYE transition (UI fades out, eye opens) ──────────────────────────

static void enter_ui_to_eye() {
    subStep = 0;
}

static void tick_ui_to_eye() {
    uint32_t elapsed = millis() - stateEnterMs;
    if (subStep == 0 && elapsed >= 50) {
        sett_destroy_all();
        ui_home_destroy();

        eye_set_pos_immediate(CX, CY);
        eye_set_zoom_immediate(10);
        eye_show();
        eye_set_zoom(ZOOM_FULL);
        subStep = 1;
    }
    if (subStep == 1 && elapsed >= 700) {
        enter_state(STATE_IDLE);
    }
}

// ─── LISTENING (boot button held, recording audio) ──────────────────────────

static void enter_listening() {
    eye_stop_idle();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();

    eye_set_pos((float)CX, (float)CY);
    eye_set_zoom(ZOOM_LISTEN);
    eye_set_tint(0.55f);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);

    audio_start_record();
}

static void tick_listening() {
    // stay here until button is released (handled by states_tick)
}

// ─── THINKING (API call in progress) ────────────────────────────────────────

static void enter_thinking() {
    audio_stop_record();

    eye_set_zoom(ZOOM_FULL);
    eye_set_light_follows_pos(true);
    eye_set_look_radius(120.0f);
    eye_start_lookaround(600, 1800);

    size_t pcmBytes = audio_get_pcm_bytes();
    if (pcmBytes < 3200) {
        LOGLN2("Recording too short, ignoring");
        enter_state(STATE_IDLE);
        return;
    }

    String key = storage_get_api_key();
    String ttsKey = storage_get_tts_key();
    if (key.length() == 0) {
        LOGLN1("No API key configured");
        textLabel = ui_create_label("No API key", (int)(CY + 60), &lv_font_montserrat_20);
        fade_in_label(textLabel, 300);
        subStep = 10;
        return;
    }

    gemini_send_audio(audio_get_pcm(), pcmBytes, key, ttsKey);
}

static void tick_thinking() {
    if (subStep == 10) {
        if (millis() - stateEnterMs > 2500) {
            cleanup_labels();
            enter_state(STATE_IDLE);
        }
        return;
    }

    if (gemini_is_done()) {
        if (gemini_is_error() || !gemini_response_ready()) {
            LOGLN1("Gemini API error or empty response");
            dev_overlay_log("ERR: Gemini API fail");
            // Surface the failure to the user via an error pill.  The
            // gemini module fills in the subsystem ("Gemini" vs "TTS")
            // and a one-line detail like "HTTP 403" so the user knows
            // *what* went wrong, not just *that* something did.
            char body[40];
            snprintf(body, sizeof(body), "%s problem",
                     gemini_last_error_subsystem());
            pill_show(PILL_ICON_ERROR, body, gemini_last_error_detail());
            eye_stop_lookaround();
            eye_set_look_radius(60.0f);
            gemini_clear();
            enter_state(STATE_IDLE);
            return;
        }
        if (!audio_is_playing()) {
            LOGLN1("TTS failed, no audio");
            // Same idea — TTS finished but produced no audio.  Show the
            // last recorded TTS error if we have one, otherwise a
            // generic message.
            const char *sub = gemini_last_error_detail();
            if (!sub || !sub[0]) sub = "No audio returned";
            pill_show(PILL_ICON_ERROR, "TTS problem", sub);
            eye_stop_lookaround();
            eye_set_look_radius(60.0f);
            gemini_clear();
            enter_state(STATE_IDLE);
            return;
        }
    }

    if (audio_output_started() && gemini_response_ready()) {
        eye_stop_lookaround();
        eye_set_look_radius(60.0f);
        // If Gemini's response carried a [tool.name]{...} prefix, take the
        // tool branch so the eye disappears and the rendered output replaces
        // it.  Audio still plays; the user dismisses the display manually.
        if (gemini_get_tool_invocation().length() > 0) {
            enter_state(STATE_TOOL_DISPLAY);
        } else {
            enter_state(STATE_SPEAKING);
        }
    }
}

// ─── SPEAKING (show response, play audio in future) ─────────────────────────

static const uint16_t SPEAK_ZOOM_BASE  = ZOOM_FULL;
static const uint16_t SPEAK_ZOOM_RANGE = 80;
static float smoothAmp = 0.0f;

static void enter_speaking() {
    eye_set_pos((float)CX, (float)CY);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);
    smoothAmp = 0.0f;
    subStep = 0;
    // If a pill is queued, do NOT release it here — wait for the eye
    // to finish its lerp to centre first.  tick_speaking polls
    // eye_at_target() and fires pill_release_pending() the frame the
    // eye settles, so the user sees: eye glides to centre, comes to
    // rest, THEN the pill pops up.  Without a queued pill, normal
    // amplitude-driven zoom kicks in immediately.
    LOG2("[speaking] %s\n", gemini_get_response().c_str());
}

static void tick_speaking() {
    if (subStep == 0) {
        eye_set_tint(0.0f);
        subStep = 1;
    }

    // Release any pill that was queued during THINKING the moment the
    // eye finishes its glide back to centre.  pill_has_pending() is
    // cleared by pill_release_pending(), so this fires exactly once.
    // Safety floor: if for some reason the eye never settles within
    // ~800 ms (e.g. zoom anim still running), release anyway so the
    // pill isn't suppressed forever.
    if (pill_has_pending()) {
        if (eye_at_target(1.5f) || (millis() - stateEnterMs) > 800) {
            pill_release_pending();
        }
    }

    // While the achievement pill is animating, freeze the amplitude-
    // driven eye zoom.  Re-rendering the eye canvas every tick competes
    // with the LVGL render thread for the same frame budget and makes
    // the pill's pop/expand/collapse stutter visibly.  Once the pill
    // has fully dropped off-screen, normal pulsing resumes.  Also
    // freeze it while we're STILL waiting for the eye to centre and a
    // pill is queued, so the zoom doesn't jiggle during the glide-in.
    if (!pill_is_active() && !pill_has_pending()) {
        float raw = audio_get_output_amplitude();
        smoothAmp += (raw - smoothAmp) * 0.3f;
        uint16_t zoom = SPEAK_ZOOM_BASE + (uint16_t)(smoothAmp * SPEAK_ZOOM_RANGE);
        eye_set_zoom(zoom);
    } else {
        // Drift the smoothed amplitude back toward zero so when the pill
        // ends and the eye starts pulsing again it doesn't snap from a
        // stale loud value.
        smoothAmp *= 0.9f;
        eye_set_zoom(SPEAK_ZOOM_BASE);
    }

    if (!audio_is_playing() && gemini_is_done()) {
        eye_set_zoom(ZOOM_FULL);
        gemini_clear();
        enter_state(STATE_IDLE);
    }
}

// ─── TOOL DISPLAY (eye fades out, tool output fades in) ─────────────────────
//
// Entered when Gemini's response begins with a [tool.name]{...} invocation.
// The eye disappears, the rendered tool output (currently just LaTeX text)
// fades in, audio still plays in the background.  The display persists even
// after audio finishes — the user dismisses it explicitly with a screen tap,
// the talk button, or sleep/wake.

static void destroy_tool_display() {
    if (toolDisplayObj) {
        lv_obj_del(toolDisplayObj);
        toolDisplayObj = NULL;
    }
}

static bool dispatch_tool_show_text(const String &args) {
    // Round display: pass the diameter (with a small margin for breathing
    // room) and use circular-fit so wide formulas can use the diagonal of
    // the screen instead of being clamped to the inscribed square.  This
    // lets the renderer pick the largest font pair that physically fits.
    const lv_coord_t margin = 12;
    const lv_coord_t diameter = SCR_W - 2 * margin;
    toolDisplayObj = latex_render(lv_scr_act(), args.c_str(),
                                  diameter, diameter, /*fit_circle=*/true);
    if (!toolDisplayObj) {
        LOGLN1("[tool] show.text: render failed");
        return false;
    }
    lv_obj_align(toolDisplayObj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(toolDisplayObj, LV_OPA_TRANSP, 0);
    fade_in_obj(toolDisplayObj, 350);
    return true;
}

// Replace the current tool display widget with a freshly-built one,
// fading the old out and the new in.  Used by the stock tool to swap
// loading -> data / error once the async fetch returns.
static void replace_tool_display(lv_obj_t *fresh) {
    if (!fresh) return;
    if (toolDisplayObj) {
        fade_out_obj(toolDisplayObj, 200);
        lv_obj_t *victim = toolDisplayObj;
        // Move behind the touch overlay so the overlay blocks all input
        // to the old card's buttons during the fade-out window.
        lv_obj_move_to_index(victim, 0);
        lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) {
            lv_obj_t *o = (lv_obj_t *)tm->user_data;
            if (o) lv_obj_del(o);
            lv_timer_del(tm);
        }, 230, victim);
        (void)t;
    }
    toolDisplayObj = fresh;
    lv_obj_set_style_opa(toolDisplayObj, LV_OPA_TRANSP, 0);
    fade_in_obj(toolDisplayObj, 250);
}

static void on_stock_range_change(StockRange newRange) {
    stockPendingRange = newRange;
    stockRangeChangeRequested = true;
    if (stockFetchActive) stock_cancel();
}

static bool dispatch_tool_show_stock(const String &args) {
    String sym = args;
    sym.trim();

    // Parse optional range: "NVDA|ytd" → symbol="NVDA", range=RANGE_YTD
    StockRange range = RANGE_1D;
    int pipe = sym.indexOf('|');
    if (pipe >= 0) {
        String rangeStr = sym.substring(pipe + 1);
        rangeStr.trim();
        range = stock_range_from_str(rangeStr.c_str());
        sym = sym.substring(0, pipe);
        sym.trim();
    }

    if (sym.length() == 0) {
        LOGLN1("[tool] show.stock: empty symbol");
        return false;
    }
    if (sym.length() >= sizeof(stockSymbol)) {
        sym = sym.substring(0, sizeof(stockSymbol) - 1);
    }
    strncpy(stockSymbol, sym.c_str(), sizeof(stockSymbol) - 1);
    stockSymbol[sizeof(stockSymbol) - 1] = '\0';
    for (char *q = stockSymbol; *q; q++) {
        if (*q >= 'a' && *q <= 'z') *q -= ('a' - 'A');
    }

    stockRange = range;
    stockRangeChangeRequested = false;
    stock_set_range_callback(on_stock_range_change);

    bool started = stock_start_fetch(stockSymbol, stockRange);
    if (started) {
        // Real fetch in flight — show the loading card, tick handler
        // will poll stock_is_done() and swap the card when the result
        // lands.
        stockFetchActive = true;
        toolDisplayObj = stock_render_loading(lv_scr_act(), stockSymbol);
    } else {
        // Fetch was rejected up front (no internet, OOM…).  The
        // result struct is already populated with errorMsg — render
        // the error card directly and skip the polling loop.
        stockFetchActive = false;
        StockQuote q;
        stock_get_result(q);
        toolDisplayObj = stock_render_error(lv_scr_act(), stockSymbol,
                                             q.errorMsg);
    }

    if (!toolDisplayObj) {
        LOGLN1("[tool] show.stock: card alloc failed");
        return false;
    }
    lv_obj_set_style_opa(toolDisplayObj, LV_OPA_TRANSP, 0);
    fade_in_obj(toolDisplayObj, 350);
    return true;
}

static void enter_tool_display() {
    eye_stop_lookaround();
    eye_set_look_radius(60.0f);
    eye_stop_blink_loop();
    eye_set_tint(0.0f);

    // Fade the eye out (it stays at center, just goes invisible).  We keep
    // the eye widget around — exit_tool_display will fade it back.
    eye_set_dim(1.0f);

    String tool = gemini_get_tool_invocation();
    int colon = tool.indexOf(':');
    String name = (colon > 0) ? tool.substring(0, colon) : tool;
    String args = (colon > 0) ? tool.substring(colon + 1) : String();

    bool ok = false;
    if (name == "show.text") {
        ok = dispatch_tool_show_text(args);
    } else if (name == "show.stock") {
        ok = dispatch_tool_show_stock(args);
    } else {
        LOG1("[tool] unknown tool: %s\n", name.c_str());
    }

    if (!ok) {
        // Renderer failed (parse error, OOM, etc.) — fall back to plain
        // SPEAKING so the user still hears the audio.
        eye_set_dim(0.0f);
        enter_state(STATE_SPEAKING);
        return;
    }

    // Reuse the IDLE touch overlay so a screen tap dismisses us.
    create_touch_overlay();
    // Move the tool card above the overlay so stock-chart timeframe
    // buttons are clickable; taps outside buttons still fall through
    // to the overlay (dismiss).
    if (toolDisplayObj) lv_obj_move_foreground(toolDisplayObj);
    eyeTouching = false;
    subStep = 0;
}

static void exit_tool_display() {
    audio_stop_play();
    audio_stream_cancel();
    gemini_cancel();

    // Cancel any in-flight stock fetch so the worker task doesn't write
    // into freed UI state after we leave.
    if (stockFetchActive) {
        stock_cancel();
        stockFetchActive = false;
    }
    stockRangeChangeRequested = false;
    stock_set_range_callback(nullptr);

    if (toolDisplayObj) {
        fade_out_obj(toolDisplayObj, 250);
        // Schedule deletion just after the fade completes — leaving it on
        // screen for an extra frame is harmless and avoids fighting the
        // running animation.
        lv_obj_t *victim = toolDisplayObj;
        toolDisplayObj = NULL;
        lv_timer_t *t = lv_timer_create([](lv_timer_t *tm) {
            lv_obj_t *o = (lv_obj_t *)tm->user_data;
            if (o) lv_obj_del(o);
            lv_timer_del(tm);
        }, 280, victim);
        (void)t;
    }

    eye_set_dim(0.0f);
    gemini_clear();
    enter_state(STATE_IDLE);
}

static void tick_tool_display() {
    // A tap on the screen triggers the IDLE-style press handler which sets
    // eyeTouching = true.  We use that as our dismiss signal so the touch
    // semantics match what the user already expects from IDLE.
    if (eyeTouching) {
        eyeTouching = false;
        exit_tool_display();
        return;
    }

    // Stock fetch polling — once Yahoo responds, swap the loading card
    // for either the data card or the error card.
    if (stockFetchActive && stock_is_done()) {
        stockFetchActive = false;
        if (!stockRangeChangeRequested) {
            StockQuote q;
            stock_get_result(q);
            lv_obj_t *fresh = q.valid
                ? stock_render(lv_scr_act(), q, stockRange)
                : stock_render_error(lv_scr_act(), stockSymbol, q.errorMsg);
            if (fresh) {
                replace_tool_display(fresh);
            } else {
                LOGLN1("[tool] stock fresh card alloc failed");
            }
        }
    }

    // Pending range change — start a new fetch once the old one finishes.
    if (stockRangeChangeRequested && !stock_is_busy()) {
        stockRangeChangeRequested = false;
        stockRange = stockPendingRange;
        bool started = stock_start_fetch(stockSymbol, stockRange);
        if (started) {
            stockFetchActive = true;
            lv_obj_t *loading = stock_render_loading(lv_scr_act(), stockSymbol);
            if (loading) replace_tool_display(loading);
        } else {
            stockFetchActive = false;
            StockQuote q;
            stock_get_result(q);
            lv_obj_t *err = stock_render_error(lv_scr_act(), stockSymbol,
                                                q.errorMsg);
            if (err) replace_tool_display(err);
        }
    }

    // Audio finishing on its own does NOT dismiss us — the result stays
    // put until the user explicitly clears it.
}

// ─── AUDIO DEBUG (record → playback loopback test with spectrum) ────────────

static void enter_audio_debug() {
    eye_stop_idle();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();

    eye_set_pos((float)CX, EYE_SETUP_Y);
    eye_set_zoom(ZOOM_SMALL);
    eye_set_tint(0.0f);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(false);
    eye_show();

    if (!debugRing) {
        debugRing = lv_obj_create(lv_scr_act());
        lv_obj_remove_style_all(debugRing);
        lv_obj_set_size(debugRing, SCR_W, SCR_H);
        lv_obj_set_pos(debugRing, 0, 0);
        lv_obj_set_style_radius(debugRing, SCR_W / 2, 0);
        lv_obj_set_style_bg_opa(debugRing, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(debugRing, lv_color_make(40, 120, 255), 0);
        lv_obj_set_style_border_width(debugRing, 6, 0);
        lv_obj_set_style_border_opa(debugRing, LV_OPA_COVER, 0);
    }

    textLabel = ui_create_label("Audio Debug", (int)(EYE_SETUP_Y + 80), &lv_font_montserrat_20);
    fade_in_label(textLabel, 300);

    create_spec_bars();

    recSamples   = 0;
    specPlayback = false;
    lastSpecMs   = 0;

    LOGLN1("[debug] Audio debug — hold BOOT to record, release to play.");
}

static void tick_audio_debug() {
    // Button press → start recording
    if (btnDown && !audio_is_recording() && !audio_is_playing()) {
        audio_start_record();
        specPlayback = false;
        LOGLN2("[debug] Recording...");
    }
    // Button release → stop recording, play back
    if (!btnDown && audio_is_recording()) {
        audio_stop_record();
        recSamples = audio_get_pcm_bytes() / 2;
        if (recSamples > 0) {
            audio_play_pcm(audio_get_pcm(), audio_get_pcm_bytes());
            playStartMs  = millis();
            specPlayback = true;
            LOG2("[debug] Playing %u samples\n", recSamples);
        }
    }

    // Update spectrum at ~20 fps
    if (millis() - lastSpecMs < 50) return;
    lastSpecMs = millis();

    if (audio_is_recording()) {
        size_t n = audio_get_pcm_bytes() / 2;
        if (n >= FFT_N) compute_spectrum(audio_get_pcm(), n);
        update_spec_bars(false);
    } else if (audio_is_playing() && recSamples > 0) {
        size_t pos = ((millis() - playStartMs) * 16000UL) / 1000;
        if (pos + FFT_N <= recSamples)
            compute_spectrum(audio_get_pcm() + pos, FFT_N);
        update_spec_bars(true);
    } else {
        for (int i = 0; i < SPEC_BARS; i++) barSmooth[i] *= 0.85f;
        update_spec_bars(specPlayback);
    }
}

static void exit_audio_debug() {
    audio_stop_play();
    audio_stop_record();
    destroy_spec_bars();
    if (debugRing) { lv_obj_del(debugRing); debugRing = NULL; }
    cleanup_labels();
    audioDebug = false;
    LOGLN1("[debug] Exiting audio debug mode.");
    enter_state(STATE_IDLE);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SLEEP MODE
// ═══════════════════════════════════════════════════════════════════════════

static void cleanup_all_ui() {
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();
    destroy_no_inet_overlay();
    destroy_setup_choice_ui();
    destroy_phone_setup_ui();
    sett_destroy_all();
    ui_home_destroy();
    destroy_tool_display();
    destroy_dev_trans_boot_scr();
}

static uint8_t  sleepStartBri = 200;
static uint32_t wakeFadeMs = 0;

static void enter_sleep() {
    preSleepState = state;
    sleepWifiWasOn = wifi_is_enabled() && wifi_is_connected();
    sleepBleWasPaired = ble_is_paired();

    if (preSleepState == STATE_LISTENING) audio_stop_record();
    if (preSleepState == STATE_THINKING || preSleepState == STATE_SPEAKING ||
        preSleepState == STATE_TOOL_DISPLAY) {
        gemini_cancel();
        audio_stop_play();
        audio_stream_cancel();
    }
    // If we were displaying a tool result, drop it now so wake comes back to
    // a clean IDLE eye instead of the leftover formula.
    if (preSleepState == STATE_TOOL_DISPLAY) {
        destroy_tool_display();
    }
    if (preSleepState == STATE_PHONE_FINDER) {
        pf_destroy();
    }
    if (preSleepState == STATE_IMU_DEBUG) {
        imu_debug_destroy_ui();
        imu_deinit();
    }

    eye_stop_idle();
    eye_stop_blink_loop();
    eye_stop_lookaround();
    eye_set_tint(0.0f);
    eye_set_dim(0.0f);

    // Suppress notification pills while sleeping — anything that arrives
    // during the sleep window gets buffered and fires as a single
    // catch-up pill when we wake.
    notifications_pause_pill();

    // Hide dev overlays — they'd burn power for nobody to see.
    dev_overlay_destroy();

    sleepStartBri = get_display_brightness_mapped();
    subStep = 0;
}

static void tick_sleep() {
    // Phase 0: fade display brightness down over ~300ms
    if (subStep == 0) {
        uint32_t elapsed = millis() - stateEnterMs;
        if (elapsed < 300) {
            uint8_t bri = (uint8_t)(sleepStartBri - (elapsed * sleepStartBri / 300));
            set_display_brightness_raw(bri);
        } else {
            set_display_brightness_raw(0);
            subStep = 10;
            stateEnterMs = millis();
        }
        return;
    }

    // Phase 10: turn off everything and enter low-power poll loop
    if (subStep == 10) {
        eye_hide();
        cleanup_all_ui();
        wifi_disconnect();
        ble_enter_low_power();
        audio_enter_low_power();
        display_off();           // panel SLPIN — biggest single power win

        // Clear any spurious PMU IRQ flags generated by the power-rail
        // transitions above (radio/display shutdown).  This is a one-time
        // clear — the light-sleep poll loop must NOT clear flags or it
        // will eat real button presses.
        extern void pmu_clear_irq();
        pmu_clear_irq();

        subStep = 11;
        LOG1("[sleep] entering low-power idle (target ~250 µA)\n");
        return;
    }

    // Phase 11: sleeping.  Drop the CPU into light sleep for 50 ms at
    // a time; wake comes from either the BOOT_BTN (instant, via ext
    // GPIO wake) or from the timer expiring, in which case the main
    // loop's poll_pmu() runs and may detect a power-key press.  Either
    // way states_pwr_short() will then transition us out of sleep.
    //
    // Average current with the display + radios + audio off and the
    // CPU in light sleep ~96% of the time works out to roughly:
    //   active poll  ~80 mA  ×  ~2 ms  /  50 ms  =  ~3.2 mA
    //   light sleep  ~0.25 mA × ~48 ms /  50 ms  =  ~0.24 mA
    //   ───────────
    //   total ≈ 3.5 mA  (vs ≈ 90 mA before this change)
    if (subStep == 11) {
        power_light_sleep_for(50);
        return;
    }

    // Phase 20: wake — fade brightness back up over 300ms
    if (subStep == 20) {
        uint32_t elapsed = millis() - wakeFadeMs;
        uint8_t tgtBri = get_display_brightness_mapped();
        if (elapsed < 300) {
            uint8_t bri = (uint8_t)(elapsed * tgtBri / 300);
            set_display_brightness_raw(bri);
        } else {
            set_display_brightness_raw(tgtBri);
            enter_state(STATE_IDLE);
        }
    }
}

static void wake_from_sleep() {
    LOG1("[sleep] waking up\n");

    // Bring the audio codec out of mute first — cheap, and lets the
    // wake "click" of the PA enable settle by the time we ship audio.
    audio_exit_low_power();

    // Radios come back asynchronously so the first frame of the wake
    // animation isn't blocked on TLS / BT init.
    if (sleepWifiWasOn && wifi_is_enabled()) {
        wifi_start_connect();
    }
    ble_exit_low_power();

    eye_set_pos_immediate(CX, CY);
    eye_set_zoom_immediate(ZOOM_FULL);
    eye_set_tint(0.0f);
    eye_set_dim(0.0f);
    eye_set_light(0.0f, 0.0f);
    eye_set_light_follows_pos(true);
    eye_show();

    // Bring the panel out of SLPIN at brightness 0, then fade up to the
    // user's stored setting via the Phase 20 ticker.  Doing the SLPOUT
    // first avoids a brief flash of the previous frame at full brightness.
    set_display_brightness_raw(0);
    display_on();

    wakeFadeMs = millis();
    subStep = 20;

    // If we're waking with a low battery (and not plugged in), the user
    // should know — schedule the pill once the wake fade has landed so
    // it doesn't draw onto a black screen mid-transition.
    states_check_lowbat_on_wake();

    // Re-enable notification pills.  The catch-up pill is normally
    // triggered the moment the post-wake snapshot arrives over BLE
    // (notifications_ingest_push handles that for us), but if the
    // phone never reconnects within a reasonable window we still want
    // to clear the pause — otherwise any notif that arrives later via
    // single-push would silently drop on the floor.  8 s is generous
    // enough to cover BLE reconnect + CCCD setup + snapshot push but
    // short enough that the pill state doesn't feel "stuck".
    gPostWakeNotifResumeMs = millis() + 8000;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SLIDE TO POWER OFF
// ═══════════════════════════════════════════════════════════════════════════

#define POFF_EYE_SIZE 54
#define POFF_SLIDER_W 260
#define POFF_SLIDER_H 60
#define POFF_KNOB_PAD 3

static bool poffShuttingDown = false;
static bool poffSnappingBack = false;

static void destroy_power_off_ui() {
    if (powerOffEyeCanvas) { lv_obj_del(powerOffEyeCanvas); powerOffEyeCanvas = NULL; }
    if (powerOffContainer) { lv_obj_del(powerOffContainer); powerOffContainer = NULL; }
    powerOffSlider = NULL;
    poffShuttingDown = false;
    poffSnappingBack = false;
}

static void render_small_eye(uint8_t *buf, int size) {
    ui_render_small_eye(buf, size);
}

static int poff_eye_start_x = 0;
static int poff_eye_y       = 0;

static void poff_position_eye_at_value(int val) {
    if (!powerOffEyeCanvas || !powerOffSlider) return;
    lv_area_t sc;
    lv_obj_get_coords(powerOffSlider, &sc);
    int trackInner = (sc.x2 - sc.x1) - POFF_EYE_SIZE - POFF_KNOB_PAD * 2;
    int knobX = sc.x1 + POFF_KNOB_PAD + (trackInner * val / 100);
    poff_eye_y = sc.y1 + (POFF_SLIDER_H - POFF_EYE_SIZE) / 2;
    lv_obj_set_pos(powerOffEyeCanvas, knobX, poff_eye_y);
    if (val == 0) poff_eye_start_x = knobX;
}

static void power_off_slider_cb(lv_event_t *e) {
    if (poffShuttingDown) return;
    int val = lv_slider_get_value(powerOffSlider);
    poff_position_eye_at_value(val);

    if (val >= 98) {
        poffShuttingDown = true;
        LOG1("[power] slide to power off — shutting down\n");
    }
}

static void build_power_off_ui() {
    destroy_power_off_ui();

    powerOffContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(powerOffContainer);
    lv_obj_set_size(powerOffContainer, SCR_W, SCR_H);
    lv_obj_set_pos(powerOffContainer, 0, 0);
    lv_obj_clear_flag(powerOffContainer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(powerOffContainer);
    lv_label_set_text(lbl, "Slide to power off");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -50);

    powerOffSlider = lv_slider_create(powerOffContainer);
    lv_obj_set_size(powerOffSlider, POFF_SLIDER_W, POFF_SLIDER_H);
    lv_obj_align(powerOffSlider, LV_ALIGN_CENTER, 0, 40);
    lv_slider_set_range(powerOffSlider, 0, 100);
    lv_slider_set_value(powerOffSlider, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(powerOffSlider, lv_color_make(50, 50, 50), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(powerOffSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(powerOffSlider, POFF_SLIDER_H / 2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(powerOffSlider, lv_color_make(70, 70, 70), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(powerOffSlider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(powerOffSlider, POFF_SLIDER_H / 2, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(powerOffSlider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(powerOffSlider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(powerOffSlider, 0, LV_PART_KNOB);

    lv_obj_add_event_cb(powerOffSlider, power_off_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (poffEyeBuf) { free(poffEyeBuf); poffEyeBuf = NULL; }
    size_t bufSz = POFF_EYE_SIZE * POFF_EYE_SIZE * LV_IMG_PX_SIZE_ALPHA_BYTE;
    poffEyeBuf = (uint8_t *)ps_malloc(bufSz);
    if (poffEyeBuf) {
        render_small_eye(poffEyeBuf, POFF_EYE_SIZE);
        powerOffEyeCanvas = lv_canvas_create(lv_scr_act());
        lv_canvas_set_buffer(powerOffEyeCanvas, poffEyeBuf,
                             POFF_EYE_SIZE, POFF_EYE_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_obj_clear_flag(powerOffEyeCanvas, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_update_layout(powerOffSlider);
        poff_position_eye_at_value(0);
    }

    fade_in_children(powerOffContainer, 300);
    if (powerOffEyeCanvas) {
        lv_obj_set_style_opa(powerOffEyeCanvas, LV_OPA_TRANSP, 0);
        fade_in_obj(powerOffEyeCanvas, 300);
    }
}

static void enter_power_off_slide() {
    prePowerOffState = state;

    if (state == STATE_LISTENING) audio_stop_record();
    if (state == STATE_THINKING || state == STATE_SPEAKING ||
        state == STATE_TOOL_DISPLAY) {
        gemini_cancel();
        audio_stop_play();
        audio_stream_cancel();
    }
    if (state == STATE_TOOL_DISPLAY) {
        destroy_tool_display();
    }
    if (state == STATE_PHONE_FINDER) {
        pf_destroy();
    }
    if (state == STATE_IMU_DEBUG) {
        imu_debug_destroy_ui();
        imu_deinit();
    }

    eye_stop_idle();
    eye_stop_blink_loop();
    eye_stop_lookaround();
    eye_set_tint(0.0f);
    eye_set_dim(0.0f);

    bool hadEye = (prePowerOffState == STATE_IDLE ||
                   prePowerOffState == STATE_LISTENING ||
                   prePowerOffState == STATE_THINKING ||
                   prePowerOffState == STATE_SPEAKING ||
                   prePowerOffState == STATE_TOOL_DISPLAY);
    bool hadUI  = (prePowerOffState == STATE_UI_HOME ||
                   prePowerOffState == STATE_SETTINGS ||
                   prePowerOffState == STATE_PHONE_FINDER);

    if (hadEye) {
        eye_set_dim(1.0f);
        subStep = 60;
    } else if (hadUI) {
        sett_fade_out(150);
        ui_home_fade_out(150);
        subStep = 60;
    } else {
        eye_hide();
        cleanup_all_ui();
        build_power_off_ui();
        subStep = 0;
    }
}

static void tick_power_off_slide() {
    // Phase 60: wait for previous screen to fade out, then build slider UI
    if (subStep == 60) {
        uint32_t elapsed = millis() - stateEnterMs;
        if (elapsed >= 200) {
            eye_hide();
            cleanup_all_ui();
            build_power_off_ui();
            subStep = 0;
        }
        return;
    }

    // Shutdown fade sequence
    if (poffShuttingDown) {
        uint32_t elapsed = millis() - stateEnterMs;
        if (subStep == 0) {
            fade_out_children(powerOffContainer, 400);
            if (powerOffEyeCanvas) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, powerOffEyeCanvas);
                lv_anim_set_values(&a, 255, 0);
                lv_anim_set_time(&a, 400);
                lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
                    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
                });
                lv_anim_start(&a);
            }
            subStep = 50;
            stateEnterMs = millis();
            return;
        }
        if (subStep == 50 && millis() - stateEnterMs >= 500) {
            display_off();
            delay(200);
            pmu_shutdown();
        }
        return;
    }

    // Snap-back: animate the slider AND eye back to 0 when released
    // partway, in lockstep, with eased motion so it feels like the
    // puck has weight to it.  We drive both from a single anim so the
    // background indicator and the eye thumb can never desync.
    if (!powerOffSlider) return;

    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (!indev) return;
    bool released = (indev->proc.state == LV_INDEV_STATE_REL);

    if (!released) {
        // User is touching: cancel any in-flight snap-back so they can
        // grab the puck mid-recovery.
        poffSnappingBack = false;
        return;
    }

    int val = lv_slider_get_value(powerOffSlider);
    if (val <= 0) {
        poffSnappingBack = false;
        return;
    }
    if (poffSnappingBack) return;   // anim already in flight
    poffSnappingBack = true;

    // Anim drives an int 0..1000; we use that to lerp the slider value
    // and the eye x in one place.  power_off_slider_cb's normal
    // VALUE_CHANGED path also runs (calling poff_position_eye_at_value)
    // which keeps the eye glued to the slider; but we set the eye x
    // explicitly here too in case the slider doesn't fire VALUE_CHANGED
    // when set programmatically.
    lv_anim_del(powerOffSlider, NULL);
    static int snapStartVal = 0;
    snapStartVal = val;
    lv_anim_t sa;
    lv_anim_init(&sa);
    lv_anim_set_var(&sa, powerOffSlider);
    lv_anim_set_values(&sa, 0, 1000);
    lv_anim_set_time(&sa, 320);
    lv_anim_set_path_cb(&sa, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&sa, [](void *o, int32_t t) {
        // t goes 0..1000.  newVal = startVal * (1 - t/1000)
        int newVal = (snapStartVal * (1000 - t) + 500) / 1000;
        if (newVal < 0) newVal = 0;
        lv_slider_set_value((lv_obj_t *)o, newVal, LV_ANIM_OFF);
        // Update the eye x explicitly too — same value drives both so
        // they're guaranteed to stay in sync visually.
        if (powerOffEyeCanvas) poff_position_eye_at_value(newVal);
    });
    lv_anim_set_ready_cb(&sa, [](lv_anim_t *) {
        poffSnappingBack = false;
    });
    lv_anim_start(&sa);
}

static void cancel_power_off_slide() {
    destroy_power_off_ui();
    // enter_power_off_slide() called eye_hide() on the way in — without
    // an explicit eye_show() here the screen looks blank after cancel
    // and the user thinks the device went to sleep.  Re-show the eye
    // and reset its render state before handing over to enter_idle().
    eye_set_pos_immediate(CX, CY);
    eye_set_zoom_immediate(ZOOM_FULL);
    eye_set_tint(0.0f);
    eye_set_dim(0.0f);
    eye_show();
    enter_state(STATE_IDLE);
}

// ─── PHONE FINDER ──────────────────────────────────────────────────────────

static void enter_phone_finder() {
    cleanup_labels();
    cleanup_buttons();
    ui_home_destroy();
    eye_hide();

    pf_init();
    block_touch_until_release();
}

static void tick_phone_finder() {
    pf_tick();
}

// ═══════════════════════════════════════════════════════════════════════════
//  IMU DEBUG
// ═══════════════════════════════════════════════════════════════════════════

static void enter_imu_debug() {
    eye_stop_idle();
    eye_stop_blink_loop();
    eye_stop_lookaround();
    cleanup_labels();
    cleanup_buttons();
    destroy_touch_overlay();
    eye_hide();

    if (!imu_is_ready()) {
        if (!imu_init()) {
            LOG1("[imu-dbg] IMU init failed, returning to IDLE\n");
            enter_state(STATE_IDLE);
            return;
        }
    }

    imu_debug_create_ui();
}

static void tick_imu_debug() {
    imu_debug_render();
}

static void exit_imu_debug() {
    imu_debug_destroy_ui();
    imu_deinit();
    eye_show();
    enter_state(STATE_IDLE);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC
// ═══════════════════════════════════════════════════════════════════════════

void states_init() {
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            indev->driver->scroll_limit = 20;  // matches TAP_MOVE_LIMIT in ui_helpers.cpp
            break;
        }
        indev = lv_indev_get_next(indev);
    }
    enter_state(STATE_BOOT);
}

// ─── Power-status pill watchdog ────────────────────────────────────────────
//
// Polls the AXP2101 a few times per second and surfaces two transient
// events as toast pills:
//
//   1. Charging — when VBUS first appears (charge state false→true).
//      If the device is asleep, it wakes first; the pill fires only
//      after the wake-fade lands so it doesn't get clipped by the
//      brightness ramp.  Latched: shown once per plug-in event so
//      yanking and replugging the cable still re-fires.
//
//   2. Low battery — when capacity drops to/below 20% AND we're not
//      charging.  Latched: shown ONCE per low-battery window — the
//      latch only resets when the battery climbs back above the
//      threshold (typically via charging).  Also fires on wake from
//      sleep if the user wakes Jibo and is already low.
//
// The watchdog deliberately does nothing during STATE_BOOT, STATE_TAP_TO_BEGIN,
// STATE_WIFI_SETUP or STATE_CONNECTING so first-boot setup isn't
// interrupted by toast pills.

#define LOW_BAT_THRESHOLD 20

static bool     gPowerPillInit = false;
static bool     gWasCharging   = false;
static bool     gLowBatLatched = false;   // suppresses repeats while still low
static uint32_t gLastPowerCheckMs    = 0;
static uint32_t gPendingChargingMs   = 0;  // fire charge pill after this ms
static bool     gPendingChargingPill = false;
static bool     gPendingLowBatPill   = false;
static uint32_t gPendingLowBatMs     = 0;
static uint8_t  gChargingDebounce    = 0;  // consecutive "charging" reads
#define CHG_DEBOUNCE_NEEDED 3

// Empirical lookup table: minutes elapsed at each percentage.  Models
// the three-phase charge curve we actually see on the 400 mAh cell
// through the AXP2101:
//   • 0 → 80 %   (CC):     ~55 min   (~0.7 min / %)
//   • 80 → 95 %  (CV):     ~38 min   (~2.5 min / %)
//   • 95 → 100 % (trickle):~22 min   (~4.4 min / %)
// Total: ~115 min for empty→full, which matches the bench measurement
// that exposed the bug (the user saw "11 min remaining" then "8 min"
// eight minutes later — old linear model overshot the CV taper).
static int charge_minutes_remaining_from_pct(int pct) {
    if (pct >= 100) return 0;
    if (pct < 0)    pct = 0;

    // Cumulative minutes elapsed at this percentage.
    double t;
    if (pct < 80) {
        t = (pct / 80.0) * 55.0;
    } else if (pct < 95) {
        t = 55.0 + ((pct - 80) / 15.0) * 38.0;
    } else {
        t = 93.0 + ((pct - 95) / 5.0) * 22.0;
    }
    const double total = 115.0;
    int remaining = (int)(total - t + 0.5);
    return remaining < 1 ? 1 : remaining;
}

// Adaptive layer: track recent (millis, pct) samples while charging and
// use the observed dpct/dt to project remaining time.  Self-corrects
// for charger current variation (USB-A 500 mA vs. PD 1 A vs. anaemic
// laptop port) and works around the inevitable model error near 100%.
// Falls back to the lookup curve when we don't yet have enough data.
struct ChargeSample { uint32_t ms; int pct; };
static const int  CHG_HIST_LEN = 8;
static ChargeSample gChgHist[CHG_HIST_LEN] = {};
static int          gChgHistCount = 0;
static int          gChgHistHead  = 0;       // ring index of next write
static int          gChgLastEstMin = -1;     // most-recent estimate (smoothing)

static void charging_history_reset() {
    gChgHistCount  = 0;
    gChgHistHead   = 0;
    gChgLastEstMin = -1;
}

static void charging_history_push(int pct) {
    gChgHist[gChgHistHead] = { millis(), pct };
    gChgHistHead = (gChgHistHead + 1) % CHG_HIST_LEN;
    if (gChgHistCount < CHG_HIST_LEN) gChgHistCount++;
}

// Pull the oldest sample we have (for slope calculation).
static const ChargeSample *charging_history_oldest() {
    if (gChgHistCount == 0) return nullptr;
    int idx = (gChgHistHead + CHG_HIST_LEN - gChgHistCount) % CHG_HIST_LEN;
    return &gChgHist[idx];
}

static String charging_time_remaining_str(int pct) {
    int remainingPct = 100 - pct;
    if (remainingPct <= 0) return String("Fully charged");

    // Curve-based baseline.
    int curveMin = charge_minutes_remaining_from_pct(pct);

    // Try to refine from observed slope.  Need at least two distinct
    // pct readings spanning ≥30 s so a single noisy ADC sample doesn't
    // distort the slope.
    int adaptiveMin = -1;
    const ChargeSample *oldest = charging_history_oldest();
    if (oldest && gChgHistCount >= 2) {
        uint32_t now = millis();
        uint32_t dtMs   = now - oldest->ms;
        int      dPct   = pct - oldest->pct;
        if (dtMs >= 30000 && dPct >= 1) {
            // minutes per percent observed in the recent window.
            double minPerPct = (double)dtMs / 60000.0 / (double)dPct;
            adaptiveMin = (int)(minPerPct * remainingPct + 0.5);
        }
    }

    // Pick the more pessimistic of the two — better to over-estimate a
    // little than promise a finish that never lands.  Weight 60 / 40
    // toward the adaptive value once it's available so the displayed
    // figure tracks reality without jittering when slope is flat.
    int est;
    if (adaptiveMin > 0) {
        est = (int)(0.6 * adaptiveMin + 0.4 * curveMin + 0.5);
    } else {
        est = curveMin;
    }

    // Hard floor at 1 minute and gentle smoothing — clamp jumps to ±2
    // min per update so a transient pct reading doesn't whiplash the
    // displayed time.
    if (gChgLastEstMin > 0) {
        int diff = est - gChgLastEstMin;
        if (diff >  2) est = gChgLastEstMin + 2;
        if (diff < -2) est = gChgLastEstMin - 2;
    }
    if (est < 1) est = 1;
    gChgLastEstMin = est;

    int hh = est / 60;
    int mm = est % 60;
    char buf[24];
    if (hh > 0) snprintf(buf, sizeof(buf), "%d:%02d remaining", hh, mm);
    else        snprintf(buf, sizeof(buf), "%d min remaining", mm);
    return String(buf);
}

static bool power_pill_state_allowed() {
    switch (state) {
        case STATE_BOOT:
        case STATE_TAP_TO_BEGIN:
        case STATE_SETUP_CHOICE:
        case STATE_PHONE_SETUP:
        case STATE_WIFI_SETUP:
        case STATE_CONNECTING:
        case STATE_CONNECTED:
        case STATE_SLEEP:
        case STATE_DEV_TRANSITION:
            return false;
        default:
            return true;
    }
}

static void power_pill_tick() {
    // Poll at ~3 Hz — fast enough to catch a plug event within a frame
    // or two, slow enough that we don't hammer the I2C bus.
    if (millis() - gLastPowerCheckMs < 300) {
        // Process pending fire timers without re-polling the PMU.
        if (gPendingChargingPill && millis() >= gPendingChargingMs &&
            power_pill_state_allowed()) {
            gPendingChargingPill = false;
            int pct = pmu_battery_percent();
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            char body[32];
            snprintf(body, sizeof(body), "Charging - %d%%", pct);
            String sub = charging_time_remaining_str(pct);
            pill_show(PILL_ICON_CHARGING, body, sub.c_str());
        }
        if (gPendingLowBatPill && millis() >= gPendingLowBatMs &&
            power_pill_state_allowed()) {
            gPendingLowBatPill = false;
            int pct = pmu_battery_percent();
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            char sub[24];
            snprintf(sub, sizeof(sub), "%d%% remaining", pct);
            pill_show(PILL_ICON_LOW_BAT, "Low battery", sub);
        }
        return;
    }
    gLastPowerCheckMs = millis();

    bool charging = pmu_is_charging();
    int  pct      = pmu_battery_percent();

    // Debounce charging detection — require CHG_DEBOUNCE_NEEDED consecutive
    // "charging" reads before accepting a plug event.  VBUS noise or I2C
    // glitches during light sleep can cause one-off false positives that
    // would otherwise wake the device.
    if (charging) {
        if (gChargingDebounce < CHG_DEBOUNCE_NEEDED)
            gChargingDebounce++;
    } else {
        gChargingDebounce = 0;
    }
    bool chargingStable = (gChargingDebounce >= CHG_DEBOUNCE_NEEDED);

    // First poll — record current state without firing anything.
    if (!gPowerPillInit) {
        gPowerPillInit = true;
        gWasCharging   = chargingStable;
        gLowBatLatched = (pct >= 0 && pct <= LOW_BAT_THRESHOLD && !chargingStable);
        if (chargingStable && pct >= 0) charging_history_push(pct);
        return;
    }

    if (chargingStable && !gWasCharging) {
        if (state == STATE_SLEEP) {
            states_pwr_short();
        }
        gPendingChargingPill = true;
        gPendingChargingMs   = millis() + 700;
        charging_history_reset();
        if (pct >= 0) charging_history_push(pct);
    }
    if (!chargingStable && gWasCharging) {
        charging_history_reset();
    }
    if (chargingStable && pct >= 0 && gChgHistCount > 0) {
        int newestIdx = (gChgHistHead + CHG_HIST_LEN - 1) % CHG_HIST_LEN;
        int newestPct = gChgHist[newestIdx].pct;
        if (pct != newestPct) charging_history_push(pct);
    }

    if (pct >= 0 && pct <= LOW_BAT_THRESHOLD && !chargingStable && !gLowBatLatched) {
        gLowBatLatched = true;
        gPendingLowBatPill = true;
        gPendingLowBatMs   = millis() + 200;
    }
    if (pct > LOW_BAT_THRESHOLD + 3 || chargingStable) {
        gLowBatLatched = false;
    }

    gWasCharging = chargingStable;
}

// Called from wake_from_sleep() to fire a low-bat pill once the wake
// fade has completed if the user wakes into a sub-20% state.  Charging
// pill on wake is handled by the regular plug-edge detection above.
static void states_check_lowbat_on_wake() {
    int pct = pmu_battery_percent();
    bool charging = pmu_is_charging();
    if (pct >= 0 && pct <= LOW_BAT_THRESHOLD && !charging && !gLowBatLatched) {
        gLowBatLatched = true;
        gPendingLowBatPill = true;
        gPendingLowBatMs   = millis() + 1200;   // wait for wake fade
    }
}

void states_tick() {
    if (hasPending) {
        hasPending = false;
        enter_state(pendingState);
        return;
    }

    power_pill_tick();

    // Deferred notif-pill resume after wake — see wake_from_sleep().
    if (gPostWakeNotifResumeMs != 0 && millis() >= gPostWakeNotifResumeMs) {
        gPostWakeNotifResumeMs = 0;
        notifications_resume_pill();
    }

    // ── Boot button debounce ──────────────────────────────────────────────
    bool raw = (digitalRead(BOOT_BTN) == LOW);
    if (raw != btnDown && millis() - lastBtnChange > BTN_DEBOUNCE_MS) {
        prevBtnDown   = btnDown;
        btnDown       = raw;
        lastBtnChange = millis();
    } else {
        prevBtnDown = btnDown;
    }
    bool btnPressed  = btnDown && !prevBtnDown;
    bool btnReleased = !btnDown && prevBtnDown;

    if (btnPressed)  event_log_printf(EVT_BTN_PRESS, "PRESS in %s", state_name(state));
    if (btnReleased) event_log_printf(EVT_BTN_PRESS, "RELEASE in %s", state_name(state));

    // ── Boot button actions per state ────────────────────────────────────
    if (state == STATE_IDLE && btnPressed) {
        if (noInetOverlay) {
            // Overlay is showing; ignore boot button presses
        } else if (!eyeTouching) {
            if (!wifi_is_connected() && !ble_is_connected()) {
                show_no_inet_overlay();
            } else {
                enter_state(STATE_LISTENING);
            }
            return;
        }
    }
    if (state == STATE_LISTENING && btnReleased) {
        enter_state(STATE_THINKING);
        return;
    }
    if ((state == STATE_THINKING || state == STATE_SPEAKING) && btnPressed) {
        gemini_cancel();
        audio_stop_play();
        audio_stream_cancel();
        eye_set_tint(0.0f);
        enter_state(STATE_IDLE);
        return;
    }
    if (state == STATE_TOOL_DISPLAY && btnPressed) {
        // Talk button while a tool display is up: dismiss the display, stop
        // audio if it's still playing, return to IDLE.  The user is welcome
        // to immediately press-and-hold to start a new query.
        exit_tool_display();
        return;
    }
    if (state == STATE_SETTINGS && btnPressed) {
        sett_btn_press();
        return;
    }
    if (state == STATE_IMU_DEBUG && btnPressed) {
        exit_imu_debug();
        return;
    }
    if (state == STATE_PHONE_SETUP && btnPressed) {
        ble_stop_pairing();
        ble_clear_setup_data();
        destroy_phone_setup_ui();
        enter_state(STATE_WIFI_SETUP);
        return;
    }
    if (state == STATE_PHONE_FINDER && btnPressed) {
        pf_destroy();
        enter_state(STATE_UI_HOME);
        return;
    }
    if (state == STATE_UI_HOME && btnPressed) {
        ui_home_btn_press();
        return;
    }
    if (state == STATE_POWER_OFF_SLIDE && btnPressed) {
        cancel_power_off_slide();
        return;
    }
    if (state == STATE_DEV_TRANSITION && btnPressed) {
        destroy_dev_transition_ui();
        enter_state(STATE_SETTINGS);
        return;
    }

    // ── Auto-sleep timer ─────────────────────────────────────────────────
    // Reset on any button press so we don't have to sprinkle resets
    // everywhere.  Touch activity is covered by state transitions
    // (tap callbacks enter new states, which reset lastActivityMs).
    if (btnPressed || btnReleased) lastActivityMs = millis();

    {
        uint8_t idx = storage_get_sleep_timeout();
        if (idx < sett_num_sleep_opts()) {
            uint32_t timeoutSec = sett_sleep_timeout_seconds(idx);
            if (timeoutSec > 0) {
                bool exempt = (state == STATE_BOOT || state == STATE_TAP_TO_BEGIN ||
                               state == STATE_SETUP_CHOICE || state == STATE_PHONE_SETUP ||
                               state == STATE_WIFI_SETUP || state == STATE_CONNECTING ||
                               state == STATE_CONNECTED ||
                               state == STATE_LISTENING || state == STATE_THINKING ||
                               state == STATE_SPEAKING ||
                               state == STATE_SLEEP || state == STATE_POWER_OFF_SLIDE ||
                               state == STATE_AUDIO_DEBUG || state == STATE_IMU_DEBUG ||
                               state == STATE_PHONE_FINDER ||
                               state == STATE_DEV_TRANSITION);
                if (!exempt && pmu_is_charging()) exempt = true;
                if (!exempt && millis() - lastActivityMs >= timeoutSec * 1000UL) {
                    enter_state(STATE_SLEEP);
                    return;
                }
            }
        }
    }

    switch (state) {
        case STATE_BOOT:             tick_boot();              break;
        case STATE_TAP_TO_BEGIN:     tick_tap_to_begin();      break;
        case STATE_SETUP_CHOICE:     tick_setup_choice();      break;
        case STATE_PHONE_SETUP:      tick_phone_setup();       break;
        case STATE_WIFI_SETUP:       tick_wifi_setup();        break;
        case STATE_CONNECTING:       tick_connecting();        break;
        case STATE_CONNECTED:        tick_connected();         break;
        case STATE_IDLE:             tick_idle();              break;
        case STATE_EYE_TO_UI:        tick_eye_to_ui();        break;
        case STATE_UI_HOME:          ui_home_tick();          break;
        case STATE_SETTINGS:         sett_tick();             break;
        case STATE_UI_TO_EYE:        tick_ui_to_eye();        break;
        case STATE_LISTENING:        tick_listening();         break;
        case STATE_THINKING:         tick_thinking();          break;
        case STATE_SPEAKING:         tick_speaking();          break;
        case STATE_TOOL_DISPLAY:     tick_tool_display();      break;
        case STATE_AUDIO_DEBUG:      tick_audio_debug();       break;
        case STATE_SLEEP:            tick_sleep();             break;
        case STATE_POWER_OFF_SLIDE:  tick_power_off_slide();   break;
        case STATE_PHONE_FINDER:     tick_phone_finder();      break;
        case STATE_IMU_DEBUG:        tick_imu_debug();         break;
        case STATE_DEV_TRANSITION:   tick_dev_transition();    break;
    }
}

JiboState states_current() {
    return state;
}

// ─── State accessors (for extracted UI modules) ───────────────────────────
uint32_t states_sub_step()                { return subStep; }
void     states_set_sub_step(uint32_t s)  { subStep = s; }
uint32_t states_enter_ms()                { return stateEnterMs; }
void     states_set_enter_ms(uint32_t ms) { stateEnterMs = ms; }
void     states_reset_activity()          { lastActivityMs = millis(); }

void states_enter_audio_debug() {
    audioDebug = true;
    enter_state(STATE_AUDIO_DEBUG);
}

void states_exit_audio_debug() {
    if (state == STATE_AUDIO_DEBUG) {
        exit_audio_debug();
    }
}

void states_enter_imu_debug() {
    enter_state(STATE_IMU_DEBUG);
}

void states_exit_imu_debug() {
    if (state == STATE_IMU_DEBUG) {
        exit_imu_debug();
    }
}

// ─── DEV MODE TRANSITION ────────────────────────────────────────────────────
//
// Slide-to-toggle screen modeled on the power-off slider.  Eye is blue-tinted
// and the text says "transition to developer mode" (or "exit" if already in
// dev mode).  After sliding, sets the NVS flag and reboots.  The transition
// boot screen is handled in tick_boot().

static lv_obj_t *devTransContainer   = NULL;
static lv_obj_t *devTransSlider      = NULL;
static lv_obj_t *devTransEyeCanvas   = NULL;
static uint8_t  *devTransEyeBuf      = NULL;
static bool      devTransSliding     = false;
static bool      devTransSnapping    = false;
static bool      devTransEnabling    = true;   // true = entering dev mode

static const int16_t DEV_EYE_SIZE     = 54;   // same as power-off slider
static const int16_t DEV_SLIDER_W     = 260;
static const int16_t DEV_SLIDER_H     = 60;   // same as power-off slider
static const int16_t DEV_KNOB_PAD     = 3;    // same as power-off slider

static void dev_position_eye_at_value(int val) {
    if (!devTransEyeCanvas || !devTransSlider) return;
    lv_area_t sc;
    lv_obj_get_coords(devTransSlider, &sc);
    int trackInner = (sc.x2 - sc.x1) - DEV_EYE_SIZE - DEV_KNOB_PAD * 2;
    int knobX = sc.x1 + DEV_KNOB_PAD + (trackInner * val / 100);
    int knobY = sc.y1 + (DEV_SLIDER_H - DEV_EYE_SIZE) / 2;
    lv_obj_set_pos(devTransEyeCanvas, knobX, knobY);
}

static void dev_slider_cb(lv_event_t *e) {
    if (devTransSliding) return;
    int val = lv_slider_get_value(devTransSlider);
    dev_position_eye_at_value(val);
    if (val >= 98) {
        devTransSliding = true;
    }
}

static void destroy_dev_transition_ui() {
    if (devTransContainer) { lv_obj_del(devTransContainer); devTransContainer = NULL; }
    if (devTransEyeCanvas) { lv_obj_del(devTransEyeCanvas); devTransEyeCanvas = NULL; }
    if (devTransEyeBuf)    { free(devTransEyeBuf); devTransEyeBuf = NULL; }
    devTransSlider = NULL;
}

static void build_dev_transition_ui() {
    destroy_dev_transition_ui();
    devTransSliding = false;
    devTransSnapping = false;

    devTransContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(devTransContainer);
    lv_obj_set_size(devTransContainer, SCR_W, SCR_H);
    lv_obj_set_pos(devTransContainer, 0, 0);
    lv_obj_clear_flag(devTransContainer, LV_OBJ_FLAG_SCROLLABLE);

    const char *text = devTransEnabling
        ? "Slide to enter\ndeveloper mode"
        : "Slide to exit\ndeveloper mode";

    lv_obj_t *lbl = lv_label_create(devTransContainer);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, 300);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -55);

    devTransSlider = lv_slider_create(devTransContainer);
    lv_obj_set_size(devTransSlider, DEV_SLIDER_W, DEV_SLIDER_H);
    lv_obj_align(devTransSlider, LV_ALIGN_CENTER, 0, 40);
    lv_slider_set_range(devTransSlider, 0, 100);
    lv_slider_set_value(devTransSlider, 0, LV_ANIM_OFF);

    // Slider styling — same pattern as power-off but with blue accents
    lv_obj_set_style_bg_color(devTransSlider, lv_color_make(50, 50, 50), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(devTransSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(devTransSlider, DEV_SLIDER_H / 2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(devTransSlider, lv_color_make(40, 80, 140), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(devTransSlider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(devTransSlider, DEV_SLIDER_H / 2, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(devTransSlider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(devTransSlider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(devTransSlider, 0, LV_PART_KNOB);

    lv_obj_add_event_cb(devTransSlider, dev_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Eye knob (blue-tinted small sphere)
    if (devTransEyeBuf) { free(devTransEyeBuf); devTransEyeBuf = NULL; }
    size_t bufSz = DEV_EYE_SIZE * DEV_EYE_SIZE * LV_IMG_PX_SIZE_ALPHA_BYTE;
    devTransEyeBuf = (uint8_t *)ps_malloc(bufSz);
    if (devTransEyeBuf) {
        // Render a blue-tinted sphere
        int r = DEV_EYE_SIZE / 2;
        int bpp = LV_IMG_PX_SIZE_ALPHA_BYTE;
        for (int py = 0; py < DEV_EYE_SIZE; py++) {
            float ny = ((float)(py - r) + 0.5f) / (float)r;
            for (int px = 0; px < DEV_EYE_SIZE; px++) {
                uint8_t *p = devTransEyeBuf + (py * DEV_EYE_SIZE + px) * bpp;
                float nx = ((float)(px - r) + 0.5f) / (float)r;
                float r2 = nx * nx + ny * ny;
                if (r2 > 1.0f) {
                    lv_color_t c = lv_color_black();
                    memcpy(p, &c, sizeof(lv_color_t));
                    p[sizeof(lv_color_t)] = 0;
                    continue;
                }
                float nz = sqrtf(1.0f - r2);
                int v = (int)(nz * 255.0f);
                if (v > 255) v = 255;
                // Blue tint: reduced R/G, full B
                int br = v * 140 / 255;
                int bg = v * 180 / 255;
                int bb = v;
                lv_color_t c = lv_color_make(br, bg, bb);
                memcpy(p, &c, sizeof(lv_color_t));
                p[sizeof(lv_color_t)] = 255;
            }
        }

        devTransEyeCanvas = lv_canvas_create(lv_scr_act());
        lv_canvas_set_buffer(devTransEyeCanvas, devTransEyeBuf,
                             DEV_EYE_SIZE, DEV_EYE_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_obj_clear_flag(devTransEyeCanvas, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_update_layout(devTransSlider);
        dev_position_eye_at_value(0);
    }

    fade_in_children(devTransContainer, 300);
    if (devTransEyeCanvas) {
        lv_obj_set_style_opa(devTransEyeCanvas, LV_OPA_TRANSP, 0);
        fade_in_obj(devTransEyeCanvas, 300);
    }
}

static void enter_dev_transition() {
    // Clean up whatever was on screen — these are all no-ops when
    // the respective containers are already null, so safe to call
    // unconditionally.
    sett_destroy_all();
    ui_home_destroy();
    cleanup_labels();
    cleanup_buttons();
    eye_hide();

    devTransEnabling = !g_dev_mode;  // toggling
    build_dev_transition_ui();
}

static void tick_dev_transition() {
    // Slide completed → fade out, set flag, reboot
    if (devTransSliding) {
        if (subStep == 0) {
            fade_out_children(devTransContainer, 400);
            if (devTransEyeCanvas) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, devTransEyeCanvas);
                lv_anim_set_values(&a, 255, 0);
                lv_anim_set_time(&a, 400);
                lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
                    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
                });
                lv_anim_start(&a);
            }
            subStep = 1;
            stateEnterMs = millis();
            return;
        }
        if (subStep == 1 && millis() - stateEnterMs >= 500) {
            // Toggle dev mode in NVS, set transition flag, reboot
            storage_set_dev_mode(devTransEnabling);
            storage_set_dev_transitioning(true);
            if (g_dev_mode) Serial.flush();
            delay(100);
            crash_handler_pre_restart();
            esp_restart();
        }
        return;
    }

    // Snap-back animation when released early (same pattern as power-off)
    if (!devTransSlider) return;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (!indev) return;
    bool released = (indev->proc.state == LV_INDEV_STATE_REL);

    if (!released) {
        devTransSnapping = false;
        return;
    }

    int val = lv_slider_get_value(devTransSlider);
    if (val <= 0) { devTransSnapping = false; return; }
    if (devTransSnapping) return;
    devTransSnapping = true;

    lv_anim_del(devTransSlider, NULL);
    static int devSnapStartVal = 0;
    devSnapStartVal = val;
    lv_anim_t sa;
    lv_anim_init(&sa);
    lv_anim_set_var(&sa, devTransSlider);
    lv_anim_set_values(&sa, 0, 1000);
    lv_anim_set_time(&sa, 320);
    lv_anim_set_path_cb(&sa, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&sa, [](void *o, int32_t t) {
        int newVal = (devSnapStartVal * (1000 - t) + 500) / 1000;
        if (newVal < 0) newVal = 0;
        lv_slider_set_value((lv_obj_t *)o, newVal, LV_ANIM_OFF);
        if (devTransEyeCanvas) dev_position_eye_at_value(newVal);
    });
    lv_anim_set_ready_cb(&sa, [](lv_anim_t *) {
        devTransSnapping = false;
    });
    lv_anim_start(&sa);
}

void states_pwr_short() {
    if (state == STATE_SLEEP) {
        wake_from_sleep();
        return;
    }
    if (state == STATE_POWER_OFF_SLIDE) {
        cancel_power_off_slide();
        return;
    }
    if (state == STATE_DEV_TRANSITION) return;  // no sleep during transition
    if (state == STATE_BOOT || state == STATE_TAP_TO_BEGIN ||
        state == STATE_SETUP_CHOICE || state == STATE_PHONE_SETUP ||
        state == STATE_WIFI_SETUP || state == STATE_CONNECTING ||
        state == STATE_CONNECTED) {
        return;  // don't sleep during initial setup
    }
    enter_state(STATE_SLEEP);
}

void states_pwr_long() {
    if (state == STATE_SLEEP) return;
    if (state == STATE_POWER_OFF_SLIDE) return;
    if (state == STATE_DEV_TRANSITION) return;
    if (state == STATE_BOOT || state == STATE_TAP_TO_BEGIN ||
        state == STATE_SETUP_CHOICE || state == STATE_PHONE_SETUP ||
        state == STATE_WIFI_SETUP || state == STATE_CONNECTING ||
        state == STATE_CONNECTED) {
        return;
    }
    enter_state(STATE_POWER_OFF_SLIDE);
}
