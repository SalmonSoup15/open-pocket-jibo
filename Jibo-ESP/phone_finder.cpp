#include "phone_finder.h"
#include "ble_link.h"
#include "pin_config.h"
#include "log.h"
#include <lvgl.h>
#include <math.h>

// ─── Tunables ───────────────────────────────────────────────────────────────

#define RSSI_POLL_MS       300
#define SMOOTH_ALPHA       0.3f
#define TREND_WINDOW       8
#define RING_ANIM_MS       40

// Zone thresholds for TEXT labels only (dBm).
// Shifted down to match real-world ESP32-S3 BLE antenna performance.
#define RSSI_RIGHT_HERE   -80
#define RSSI_CLOSE        -87
#define RSSI_NEARBY       -93
#define RSSI_FAR          -100

// Continuous ring mapping range (dBm → 0..360 arc degrees)
#define RSSI_RING_MIN     -105.0f
#define RSSI_RING_MAX     -75.0f

// ─── State ──────────────────────────────────────────────────────────────────

static float    smoothRssi     = -80.0f;
static bool     hasReading     = false;
static int      rawHistory[TREND_WINDOW];
static int      histCount      = 0;
static int      histHead       = 0;
static uint32_t lastPollMs     = 0;

// Ring pulse animation
static float    ringPhase      = 0.0f;
static uint32_t lastRingMs     = 0;

static const int16_t SCR_W = LCD_WIDTH;
static const int16_t SCR_H = LCD_HEIGHT;

// ─── LVGL widgets ───────────────────────────────────────────────────────────

static lv_obj_t *pfContainer  = NULL;
static lv_obj_t *zoneLabel    = NULL;
static lv_obj_t *trendLabel   = NULL;
static lv_obj_t *ringArc      = NULL;

// ─── Helpers ────────────────────────────────────────────────────────────────

enum PfZone { ZONE_DISCONNECTED, ZONE_RIGHT_HERE, ZONE_CLOSE, ZONE_NEARBY, ZONE_FAR, ZONE_VERY_FAR };

static PfZone classify(float rssi) {
    if (rssi >= RSSI_RIGHT_HERE) return ZONE_RIGHT_HERE;
    if (rssi >= RSSI_CLOSE)      return ZONE_CLOSE;
    if (rssi >= RSSI_NEARBY)     return ZONE_NEARBY;
    if (rssi >= RSSI_FAR)        return ZONE_FAR;
    return ZONE_VERY_FAR;
}

static const char *zone_text(PfZone z) {
    switch (z) {
        case ZONE_RIGHT_HERE: return "Right here!";
        case ZONE_CLOSE:      return "Close";
        case ZONE_NEARBY:     return "Nearby";
        case ZONE_FAR:        return "Far";
        case ZONE_VERY_FAR:   return "Very far";
        default:              return "Searching...";
    }
}

static lv_color_t zone_color(PfZone z) {
    switch (z) {
        case ZONE_RIGHT_HERE: return lv_color_make(60, 255, 60);
        case ZONE_CLOSE:      return lv_color_make(100, 220, 100);
        case ZONE_NEARBY:     return lv_color_make(255, 200, 40);
        case ZONE_FAR:        return lv_color_make(255, 120, 40);
        case ZONE_VERY_FAR:   return lv_color_make(255, 60, 60);
        default:              return lv_color_make(100, 100, 100);
    }
}

// Continuous RSSI → arc value (0..360) with smooth mapping
static uint16_t rssi_to_arc(float rssi) {
    float ratio = (rssi - RSSI_RING_MIN) / (RSSI_RING_MAX - RSSI_RING_MIN);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return (uint16_t)(ratio * 360.0f);
}

// Continuous RSSI → ring color (red → yellow → green)
static lv_color_t rssi_to_ring_color(float rssi) {
    float ratio = (rssi - RSSI_RING_MIN) / (RSSI_RING_MAX - RSSI_RING_MIN);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    uint8_t r, g;
    if (ratio < 0.5f) {
        r = 255;
        g = (uint8_t)(ratio * 2.0f * 220);
    } else {
        r = (uint8_t)((1.0f - ratio) * 2.0f * 255);
        g = 220 + (uint8_t)((ratio - 0.5f) * 2.0f * 35);
    }
    return lv_color_make(r, g, 40);
}

static const char *trend_text() {
    if (histCount < 3) return "";
    float oldest = 0, newest = 0;
    int half = histCount / 2;
    for (int i = 0; i < half; i++) {
        int idx = (histHead - histCount + i + TREND_WINDOW) % TREND_WINDOW;
        oldest += rawHistory[idx];
    }
    for (int i = histCount - half; i < histCount; i++) {
        int idx = (histHead - histCount + i + TREND_WINDOW) % TREND_WINDOW;
        newest += rawHistory[idx];
    }
    oldest /= half;
    newest /= half;
    float diff = newest - oldest;
    if (diff > 3.0f)  return "Getting closer...";
    if (diff < -3.0f) return "Getting farther...";
    return "";
}

// ─── Public API ─────────────────────────────────────────────────────────────

void pf_init() {
    smoothRssi = -80.0f;
    hasReading = false;
    histCount  = 0;
    histHead   = 0;
    ringPhase  = 0.0f;
    lastPollMs = lastRingMs = millis();

    pfContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(pfContainer);
    lv_obj_set_size(pfContainer, SCR_W, SCR_H);
    lv_obj_set_pos(pfContainer, 0, 0);
    lv_obj_clear_flag(pfContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Pulsing ring arc
    ringArc = lv_arc_create(pfContainer);
    lv_obj_set_size(ringArc, 340, 340);
    lv_obj_align(ringArc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(ringArc, 270);
    lv_arc_set_range(ringArc, 0, 360);
    lv_arc_set_value(ringArc, 0);
    lv_arc_set_bg_angles(ringArc, 0, 360);
    lv_obj_remove_style(ringArc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ringArc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ringArc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ringArc, lv_color_make(30, 30, 30), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ringArc, lv_color_make(100, 100, 100), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ringArc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(ringArc, LV_OBJ_FLAG_CLICKABLE);

    // Zone label (big text)
    zoneLabel = lv_label_create(pfContainer);
    lv_label_set_text(zoneLabel, "Searching...");
    lv_obj_set_style_text_color(zoneLabel, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(zoneLabel, &lv_font_montserrat_24, 0);
    lv_obj_align(zoneLabel, LV_ALIGN_CENTER, 0, -20);

    // Trend label (smaller, below zone)
    trendLabel = lv_label_create(pfContainer);
    lv_label_set_text(trendLabel, "");
    lv_obj_set_style_text_color(trendLabel, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(trendLabel, &lv_font_montserrat_16, 0);
    lv_obj_align(trendLabel, LV_ALIGN_CENTER, 0, 20);

    LOG1("[pf] init complete\n");
}

void pf_tick() {
    uint32_t now = millis();

    // Poll RSSI
    if (now - lastPollMs >= RSSI_POLL_MS) {
        lastPollMs = now;
        int rssi = ble_read_rssi();

        if (rssi == 0) {
            hasReading = false;
            if (zoneLabel)  lv_label_set_text(zoneLabel, "Phone not connected");
            if (trendLabel) lv_label_set_text(trendLabel, "");
            lv_obj_set_style_text_color(zoneLabel, lv_color_make(140, 140, 140), 0);
            lv_arc_set_value(ringArc, 0);
            lv_obj_set_style_arc_color(ringArc, lv_color_make(60, 60, 60), LV_PART_INDICATOR);
            return;
        }

        // Exponential moving average
        if (!hasReading) {
            smoothRssi = (float)rssi;
            hasReading = true;
        } else {
            smoothRssi = SMOOTH_ALPHA * rssi + (1.0f - SMOOTH_ALPHA) * smoothRssi;
        }

        // Push into trend history
        rawHistory[histHead] = rssi;
        histHead = (histHead + 1) % TREND_WINDOW;
        if (histCount < TREND_WINDOW) histCount++;

        // Update UI — text uses discrete zones, ring is continuous
        PfZone zone = classify(smoothRssi);
        lv_color_t textCol = zone_color(zone);

        if (zoneLabel) {
            lv_label_set_text(zoneLabel, zone_text(zone));
            lv_obj_set_style_text_color(zoneLabel, textCol, 0);
            lv_obj_align(zoneLabel, LV_ALIGN_CENTER, 0, -20);
        }
        if (trendLabel) {
            lv_label_set_text(trendLabel, trend_text());
            lv_obj_align(trendLabel, LV_ALIGN_CENTER, 0, 20);
        }

        // Continuous ring: smooth arc size and color from RSSI
        lv_arc_set_value(ringArc, rssi_to_arc(smoothRssi));
        lv_obj_set_style_arc_color(ringArc, rssi_to_ring_color(smoothRssi), LV_PART_INDICATOR);
    }

    // Pulse ring width for visual feedback
    if (hasReading && now - lastRingMs >= RING_ANIM_MS) {
        lastRingMs = now;
        PfZone zone = classify(smoothRssi);
        float speed = (zone <= ZONE_CLOSE) ? 0.15f : (zone <= ZONE_NEARBY) ? 0.08f : 0.04f;
        ringPhase += speed;
        if (ringPhase > 2.0f * M_PI) ringPhase -= 2.0f * M_PI;
        int width = 10 + (int)(6.0f * (0.5f + 0.5f * sinf(ringPhase)));
        lv_obj_set_style_arc_width(ringArc, width, LV_PART_INDICATOR);
    }
}

void pf_destroy() {
    if (pfContainer) { lv_obj_del(pfContainer); pfContainer = NULL; }
    ringArc = NULL;
    zoneLabel = trendLabel = NULL;
    hasReading = false;
    LOG1("[pf] destroyed\n");
}
