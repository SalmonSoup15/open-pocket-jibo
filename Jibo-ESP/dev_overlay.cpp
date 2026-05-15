#include "dev_overlay.h"
#include "storage.h"
#include "dev_console.h"   // g_dev_mode
#include "pin_config.h"
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <string.h>

// ──────────────────────────────────────────────────────────────────────────
//  Constants
// ──────────────────────────────────────────────────────────────────────────

static const int16_t SCR_W = LCD_WIDTH;
static const int16_t SCR_H = LCD_HEIGHT;

// ── Stats pill geometry ────────────────────────────────────────────────────
// Three small arcs side by side in a rounded-rect container near the top.
static const int16_t PILL_W         = 160;
static const int16_t PILL_H         = 40;
static const int16_t PILL_Y         = 55;      // from top
static const int16_t ARC_SIZE       = 28;       // arc diameter
static const int16_t ARC_WIDTH      = 3;        // arc line thickness
static const int16_t ARC_SPACING    = 50;       // center-to-center spacing
static const int16_t ARC_Y_OFS      = 0;        // vertical offset inside pill

// ── Verbose strip geometry ─────────────────────────────────────────────────
static const int16_t STRIP_H      = 60;
static const int16_t STRIP_Y      = SCR_H - STRIP_H - 50;  // above bottom bezel
#define VLOG_MAX_LINES  3
#define VLOG_LINE_LEN   60

// ── Update intervals ───────────────────────────────────────────────────────
static const uint32_t STATS_UPDATE_MS = 1000;

// ──────────────────────────────────────────────────────────────────────────
//  Stats pill state
// ──────────────────────────────────────────────────────────────────────────

static lv_obj_t *sPillContainer = NULL;
static lv_obj_t *sArcDram       = NULL;
static lv_obj_t *sArcPsram      = NULL;
static lv_obj_t *sArcCpu        = NULL;
static lv_obj_t *sLblDram       = NULL;
static lv_obj_t *sLblPsram      = NULL;
static lv_obj_t *sLblCpu        = NULL;

static uint32_t sLastStatsMs    = 0;
static bool     sStatsVisible   = false;

// CPU measurement (delta between idle ticks)
static uint32_t sPrevIdleRun0   = 0;
static uint32_t sPrevIdleRun1   = 0;
static uint32_t sPrevTotalRun   = 0;
static bool     sCpuPrimed      = false;

// ──────────────────────────────────────────────────────────────────────────
//  Verbose strip state
// ──────────────────────────────────────────────────────────────────────────

static lv_obj_t *sStripContainer = NULL;
static lv_obj_t *sStripLabel     = NULL;
static bool      sStripVisible   = false;

static char sLogLines[VLOG_MAX_LINES][VLOG_LINE_LEN + 1];
static uint8_t sLogWriteIdx = 0;
static bool    sLogDirty    = false;

// ──────────────────────────────────────────────────────────────────────────
//  Stats pill implementation
// ──────────────────────────────────────────────────────────────────────────

static lv_obj_t *make_arc(lv_obj_t *parent, lv_color_t color, int16_t x) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, ARC_SIZE, ARC_SIZE);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    // Background arc (dark)
    lv_obj_set_style_arc_color(arc, lv_color_make(40, 40, 40), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ARC_WIDTH, LV_PART_MAIN);

    // Indicator arc (colored)
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

    lv_obj_align(arc, LV_ALIGN_CENTER, x, ARC_Y_OFS);
    return arc;
}

static void create_stats_pill() {
    if (sPillContainer) return;

    sPillContainer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sPillContainer);
    lv_obj_set_size(sPillContainer, PILL_W, PILL_H);
    lv_obj_align(sPillContainer, LV_ALIGN_TOP_MID, 0, PILL_Y);
    lv_obj_set_style_bg_color(sPillContainer, lv_color_make(15, 15, 15), 0);
    lv_obj_set_style_bg_opa(sPillContainer, LV_OPA_80, 0);
    lv_obj_set_style_radius(sPillContainer, PILL_H / 2, 0);
    lv_obj_clear_flag(sPillContainer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // DRAM arc — cyan
    sArcDram = make_arc(sPillContainer, lv_color_make(0, 200, 200), -ARC_SPACING);
    // PSRAM arc — green
    sArcPsram = make_arc(sPillContainer, lv_color_make(0, 200, 80), 0);
    // CPU arc — orange
    sArcCpu = make_arc(sPillContainer, lv_color_make(255, 160, 0), ARC_SPACING);

    // Small labels below arcs
    const lv_font_t *fnt = &lv_font_montserrat_10;

    sLblDram = lv_label_create(sPillContainer);
    lv_label_set_text(sLblDram, "D");
    lv_obj_set_style_text_font(sLblDram, fnt, 0);
    lv_obj_set_style_text_color(sLblDram, lv_color_make(0, 200, 200), 0);
    lv_obj_align(sLblDram, LV_ALIGN_CENTER, -ARC_SPACING, ARC_Y_OFS);

    sLblPsram = lv_label_create(sPillContainer);
    lv_label_set_text(sLblPsram, "P");
    lv_obj_set_style_text_font(sLblPsram, fnt, 0);
    lv_obj_set_style_text_color(sLblPsram, lv_color_make(0, 200, 80), 0);
    lv_obj_align(sLblPsram, LV_ALIGN_CENTER, 0, ARC_Y_OFS);

    sLblCpu = lv_label_create(sPillContainer);
    lv_label_set_text(sLblCpu, "C");
    lv_obj_set_style_text_font(sLblCpu, fnt, 0);
    lv_obj_set_style_text_color(sLblCpu, lv_color_make(255, 160, 0), 0);
    lv_obj_align(sLblCpu, LV_ALIGN_CENTER, ARC_SPACING, ARC_Y_OFS);

    sStatsVisible = true;
    sCpuPrimed = false;
}

static void destroy_stats_pill() {
    if (sPillContainer) { lv_obj_del(sPillContainer); sPillContainer = NULL; }
    sArcDram = sArcPsram = sArcCpu = NULL;
    sLblDram = sLblPsram = sLblCpu = NULL;
    sStatsVisible = false;
    sCpuPrimed = false;
}

// Measure CPU% as (1 - idle_fraction) * 100 across both cores.
// Uses FreeRTOS uxTaskGetSystemState to find IDLE tasks.
static uint8_t measure_cpu_percent() {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) return 0;

    // Small temp array — we only need idle task handles
    uint32_t totalRun = 0;
    uint32_t idle0Run = 0;
    uint32_t idle1Run = 0;

    UBaseType_t cap = count + 4;
    TaskStatus_t *tasks = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * cap);
    if (!tasks) return 0;

    UBaseType_t n = uxTaskGetSystemState(tasks, cap, &totalRun);
    for (UBaseType_t i = 0; i < n; i++) {
        const char *name = tasks[i].pcTaskName;
        if (strcmp(name, "IDLE0") == 0 || strcmp(name, "IDLE") == 0)
            idle0Run = tasks[i].ulRunTimeCounter;
        else if (strcmp(name, "IDLE1") == 0)
            idle1Run = tasks[i].ulRunTimeCounter;
    }
    vPortFree(tasks);

    if (!sCpuPrimed) {
        sPrevIdleRun0 = idle0Run;
        sPrevIdleRun1 = idle1Run;
        sPrevTotalRun = totalRun;
        sCpuPrimed = true;
        return 0;
    }

    uint32_t dt = totalRun - sPrevTotalRun;
    if (dt == 0) dt = 1;  // avoid div/0
    uint32_t idleDelta = (idle0Run - sPrevIdleRun0) + (idle1Run - sPrevIdleRun1);
    // Total run counter covers both cores, so max idle = dt*2 when both cores
    // are fully idle.  But uxTaskGetSystemState returns totalRunTime as the
    // sum across all tasks, so idleDelta/dt gives idle fraction directly.
    uint32_t busy = (dt > idleDelta) ? dt - idleDelta : 0;
    uint8_t pct = (uint8_t)((busy * 100ULL) / dt);
    if (pct > 100) pct = 100;

    sPrevIdleRun0 = idle0Run;
    sPrevIdleRun1 = idle1Run;
    sPrevTotalRun = totalRun;

    return pct;
}

static void update_stats() {
    if (!sPillContainer) return;

    size_t intFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t intTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psFree   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psTotal   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    uint8_t dramPct  = (intTotal > 0) ? (uint8_t)(((intTotal - intFree) * 100) / intTotal) : 0;
    uint8_t psramPct = (psTotal > 0)  ? (uint8_t)(((psTotal - psFree) * 100) / psTotal) : 0;
    uint8_t cpuPct   = measure_cpu_percent();

    if (sArcDram)  lv_arc_set_value(sArcDram,  dramPct);
    if (sArcPsram) lv_arc_set_value(sArcPsram, psramPct);
    if (sArcCpu)   lv_arc_set_value(sArcCpu,   cpuPct);

    char buf[8];
    if (sLblDram) {
        snprintf(buf, sizeof(buf), "%u%%", dramPct);
        lv_label_set_text(sLblDram, buf);
        lv_obj_align(sLblDram, LV_ALIGN_CENTER, -ARC_SPACING, ARC_Y_OFS);
    }
    if (sLblPsram) {
        snprintf(buf, sizeof(buf), "%u%%", psramPct);
        lv_label_set_text(sLblPsram, buf);
        lv_obj_align(sLblPsram, LV_ALIGN_CENTER, 0, ARC_Y_OFS);
    }
    if (sLblCpu) {
        snprintf(buf, sizeof(buf), "%u%%", cpuPct);
        lv_label_set_text(sLblCpu, buf);
        lv_obj_align(sLblCpu, LV_ALIGN_CENTER, ARC_SPACING, ARC_Y_OFS);
    }
}

// ──────────────────────────────────────────────────────────────────────────
//  Verbose strip implementation
// ──────────────────────────────────────────────────────────────────────────

static void create_verbose_strip() {
    if (sStripContainer) return;

    memset(sLogLines, 0, sizeof(sLogLines));
    sLogWriteIdx = 0;
    sLogDirty = false;

    sStripContainer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sStripContainer);
    lv_obj_set_size(sStripContainer, SCR_W - 60, STRIP_H);
    lv_obj_align(sStripContainer, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(sStripContainer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sStripContainer, LV_OPA_70, 0);
    lv_obj_set_style_radius(sStripContainer, 8, 0);
    lv_obj_set_style_pad_all(sStripContainer, 4, 0);
    lv_obj_clear_flag(sStripContainer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    sStripLabel = lv_label_create(sStripContainer);
    lv_label_set_text(sStripLabel, "");
    lv_obj_set_style_text_color(sStripLabel, lv_color_make(0, 220, 0), 0);
    lv_obj_set_style_text_font(sStripLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_width(sStripLabel, SCR_W - 80);
    lv_label_set_long_mode(sStripLabel, LV_LABEL_LONG_CLIP);
    lv_obj_align(sStripLabel, LV_ALIGN_TOP_LEFT, 0, 0);

    sStripVisible = true;
}

static void destroy_verbose_strip() {
    if (sStripContainer) { lv_obj_del(sStripContainer); sStripContainer = NULL; }
    sStripLabel = NULL;
    sStripVisible = false;
}

static void refresh_strip_text() {
    if (!sStripLabel || !sLogDirty) return;
    sLogDirty = false;

    // Build the display text from the ring buffer, oldest first.
    // sLogWriteIdx points to the next write slot, so the oldest visible
    // line is at sLogWriteIdx (wraps around).
    char combined[VLOG_MAX_LINES * (VLOG_LINE_LEN + 2) + 1];
    int pos = 0;
    for (int i = 0; i < VLOG_MAX_LINES; i++) {
        int idx = (sLogWriteIdx + i) % VLOG_MAX_LINES;
        if (sLogLines[idx][0]) {
            if (pos > 0) combined[pos++] = '\n';
            int len = strlen(sLogLines[idx]);
            memcpy(combined + pos, sLogLines[idx], len);
            pos += len;
        }
    }
    combined[pos] = '\0';
    lv_label_set_text(sStripLabel, combined);
}

// ──────────────────────────────────────────────────────────────────────────
//  Public API
// ──────────────────────────────────────────────────────────────────────────

void dev_overlay_init() {
    // Overlays are created/destroyed dynamically in tick() based on toggles.
    // Nothing to do here except clear state.
    sStatsVisible = false;
    sStripVisible = false;
    sPillContainer = NULL;
    sStripContainer = NULL;
}

void dev_overlay_tick() {
    if (!g_dev_mode) {
        if (sStatsVisible) destroy_stats_pill();
        if (sStripVisible) destroy_verbose_strip();
        return;
    }

    // ── Stats pill toggle ─────────────────────────────────────────────
    bool wantStats = storage_get_dev_stats_pill();
    if (wantStats && !sStatsVisible) {
        create_stats_pill();
    } else if (!wantStats && sStatsVisible) {
        destroy_stats_pill();
    }
    if (sStatsVisible && millis() - sLastStatsMs >= STATS_UPDATE_MS) {
        sLastStatsMs = millis();
        update_stats();
    }

    // ── Verbose strip toggle ──────────────────────────────────────────
    bool wantStrip = storage_get_dev_verbose_overlay();
    if (wantStrip && !sStripVisible) {
        create_verbose_strip();
    } else if (!wantStrip && sStripVisible) {
        destroy_verbose_strip();
    }
    if (sStripVisible) {
        refresh_strip_text();
    }
}

void dev_overlay_log(const char *line) {
    if (!line || !line[0]) return;
    strncpy(sLogLines[sLogWriteIdx], line, VLOG_LINE_LEN);
    sLogLines[sLogWriteIdx][VLOG_LINE_LEN] = '\0';
    sLogWriteIdx = (sLogWriteIdx + 1) % VLOG_MAX_LINES;
    sLogDirty = true;
}

void dev_overlay_destroy() {
    destroy_stats_pill();
    destroy_verbose_strip();
}
