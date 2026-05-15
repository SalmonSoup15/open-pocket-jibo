#include <Arduino.h>
#include "eye.h"
#include "pin_config.h"
#include "log.h"
#include <math.h>

// ─── Constants ──────────────────────────────────────────────────────────────
static const int16_t SCR_W       = LCD_WIDTH;
static const int16_t SCR_H       = LCD_HEIGHT;
static const int16_t SPHERE_DIAM = (int16_t)(SCR_W * 0.40f);
static const int16_t SPHERE_R    = SPHERE_DIAM / 2;
static const float   LERP_POS   = 0.16f;
static const float   LERP_ZOOM_DEFAULT = 0.35f;
static float         lerpZoom   = 0.35f;

// ─── Position / zoom state ──────────────────────────────────────────────────
static float    curX, curY, tgtX, tgtY;
static float    curZoomF = 256.0f;
static uint16_t tgtZoom = 256;

// ─── Lighting ───────────────────────────────────────────────────────────────
static float lightX = 0.0f, lightY = 0.0f;
static bool  lightFollows = false;
static float prevLX = 99.0f, prevLY = 99.0f;

// ─── Blink ──────────────────────────────────────────────────────────────────
static bool        blinking       = false;
static int         blinkPhase     = 0;
static int         blinkRow       = 0;
static uint32_t    blinkStartMs   = 0;
static const int   BLINK_CLOSE_MS = 60;
static const int   BLINK_OPEN_MS  = 80;
static const int   BLINK_HOLD_MS  = 15;
static bool        closeOnly      = false;
static int         closeAnimDurMs = 60;

// ─── Blink loop ─────────────────────────────────────────────────────────────
static lv_timer_t *blinkLoopTmr    = NULL;
static uint32_t    blinkMinMs      = 3000;
static uint32_t    blinkMaxMs      = 6000;

// ─── Look-around ────────────────────────────────────────────────────────────
static lv_timer_t *lookTmr         = NULL;
static uint32_t    lookMinMs       = 2000;
static uint32_t    lookMaxMs       = 4000;
static float       lookCenterX, lookCenterY;
static float       lookRadius     = 60.0f;

// ─── Open animation ────────────────────────────────────────────────────────
static bool  openAnimActive = false;
static void (*openAnimCb)(void) = NULL;

// ─── LVGL objects ───────────────────────────────────────────────────────────
static lv_obj_t   *canvas    = NULL;
static lv_color_t *cbuf      = NULL;
static lv_color_t *cleanBuf  = NULL;   // cached sphere with no blink mask
static lv_timer_t *renderTmr = NULL;

// ─── Cached canvas state (avoid redundant LVGL invalidations) ───────────────
static lv_coord_t lastCanvX = -9999;
static lv_coord_t lastCanvY = -9999;
static uint16_t   lastCanvZoom = 0;

// ─── Normal LUT (combined int16, half the PSRAM of two float arrays) ────────
struct NormalEntry { int16_t nx, nz; };   // scaled by 1024
static NormalEntry *normalLUT = NULL;

// ─── Grayscale palette (avoids lv_color_make per pixel) ─────────────────────
static uint16_t grayPalette[256];

// ─── Tint (cyan-blue overlay for voice interaction) ─────────────────────────
static float    tgtTint = 0.0f, curTint = 0.0f;
// ─── Dim (overall brightness reduction for overlay effects) ─────────────────
static float    tgtDim = 0.0f, curDim = 0.0f;
static uint16_t activePalette[256];

static void rebuild_palette() {
    float bright = 1.0f - curDim;
    for (int i = 0; i < 256; i++) {
        float v = i * bright;
        uint8_t r = (uint8_t)(v * (1.0f - curTint * 0.55f));
        uint8_t g = (uint8_t)(v * (1.0f - curTint * 0.08f));
        uint8_t b = (uint8_t)(v);
        activePalette[i] = lv_color_make(r, g, b).full;
    }
}

static void build_luts() {
    const int total = SPHERE_DIAM * SPHERE_DIAM;
    normalLUT = (NormalEntry *)ps_malloc(total * sizeof(NormalEntry));
    if (!normalLUT) return;

    const float invR = 1.0f / (float)SPHERE_R;
    for (int py = 0; py < SPHERE_DIAM; py++) {
        float ny  = ((float)(py - SPHERE_R) + 0.5f) * invR;
        float ny2 = ny * ny;
        int   row = py * SPHERE_DIAM;
        for (int px = 0; px < SPHERE_DIAM; px++) {
            float nx = ((float)(px - SPHERE_R) + 0.5f) * invR;
            float r2 = nx * nx + ny2;
            if (r2 <= 1.0f) {
                normalLUT[row + px].nx = (int16_t)(nx * 1024.0f);
                normalLUT[row + px].nz = (int16_t)(sqrtf(1.0f - r2) * 1024.0f);
            } else {
                normalLUT[row + px].nx = 0;
                normalLUT[row + px].nz = -1;
            }
        }
    }

    for (int i = 0; i < 256; i++) {
        grayPalette[i] = lv_color_make(i, i, i).full;
    }
    memcpy(activePalette, grayPalette, sizeof(grayPalette));
}

// ─── Sphere rasteriser (integer inner loop) ─────────────────────────────────
static void render_sphere(float lx, float ly) {
    float l2 = lx * lx + ly * ly;
    if (l2 > 1.0f) {
        float s = 1.0f / sqrtf(l2);
        lx *= s; ly *= s; l2 = 1.0f;
    }
    float lz = sqrtf(1.0f - l2);

    int32_t lxi = (int32_t)(lx * 1024.0f);
    int32_t lyi = (int32_t)(ly * 1024.0f);
    int32_t lzi = (int32_t)(lz * 1024.0f);

    const float invR = 1.0f / (float)SPHERE_R;

    for (int py = 0; py < SPHERE_DIAM; py++) {
        float ny = ((float)(py - SPHERE_R) + 0.5f) * invR;
        int32_t nyly = (int32_t)(ny * 1024.0f) * lyi;
        int row = py * SPHERE_DIAM;

        for (int px = 0; px < SPHERE_DIAM; px++) {
            int idx = row + px;
            NormalEntry n = normalLUT[idx];
            if (n.nz < 0) { cbuf[idx].full = 0; continue; }

            int32_t diff = (int32_t)n.nx * lxi + nyly + (int32_t)n.nz * lzi;
            if (diff <= 0) { cbuf[idx].full = 0; continue; }

            int v = diff >> 12;
            if (v > 255) v = 255;
            cbuf[idx].full = activePalette[v];
        }
    }
}

// ─── Blink helpers (only touch the rows that changed) ───────────────────────
static void blink_black_rows(int from, int to) {
    for (int py = from; py < to; py++) {
        memset(&cbuf[py * SPHERE_DIAM], 0, SPHERE_DIAM * sizeof(lv_color_t));
        int bot = SPHERE_DIAM - 1 - py;
        if (bot > py)
            memset(&cbuf[bot * SPHERE_DIAM], 0, SPHERE_DIAM * sizeof(lv_color_t));
    }
}

static void blink_restore_rows(int from, int to) {
    if (!cleanBuf) return;
    for (int py = from; py < to; py++) {
        int topOff = py * SPHERE_DIAM;
        memcpy(&cbuf[topOff], &cleanBuf[topOff], SPHERE_DIAM * sizeof(lv_color_t));
        int bot = SPHERE_DIAM - 1 - py;
        if (bot > py) {
            int botOff = bot * SPHERE_DIAM;
            memcpy(&cbuf[botOff], &cleanBuf[botOff], SPHERE_DIAM * sizeof(lv_color_t));
        }
    }
}

static void apply_blink_mask(int rows) {
    if (rows <= 0) return;
    if (rows > SPHERE_R) rows = SPHERE_R;
    blink_black_rows(0, rows);
}

// ─── Clamp ──────────────────────────────────────────────────────────────────
static inline void clamp_pos(float &cx, float &cy) {
    float lo  = (float)SPHERE_R;
    float hiX = (float)(SCR_W - SPHERE_R);
    float hiY = (float)(SCR_H - SPHERE_R);
    if (cx < lo)  cx = lo;
    if (cx > hiX) cx = hiX;
    if (cy < lo)  cy = lo;
    if (cy > hiY) cy = hiY;
}

// ─── Blink tick ─────────────────────────────────────────────────────────────
static void blink_tick() {
    if (!blinking) return;
    uint32_t elapsed = millis() - blinkStartMs;

    if (blinkPhase == 0) {
        int closeDur = closeOnly ? closeAnimDurMs : BLINK_CLOSE_MS;
        float t = (float)elapsed / (float)closeDur;
        if (t >= 1.0f) t = 1.0f;
        blinkRow = (int)(t * SPHERE_R);
        if (elapsed >= (uint32_t)closeDur) {
            if (closeOnly) {
                blinkRow = SPHERE_R;
                return;
            }
            blinkPhase = 1;
            blinkStartMs = millis() + BLINK_HOLD_MS;
        }
    } else {
        if (millis() < blinkStartMs) {
            blinkRow = SPHERE_R;
            return;
        }
        uint32_t openElapsed = millis() - blinkStartMs;
        float t = (float)openElapsed / (float)BLINK_OPEN_MS;
        if (t >= 1.0f) t = 1.0f;
        blinkRow = SPHERE_R - (int)(t * SPHERE_R);
        if (t >= 1.0f) {
            blinking = false;
            blinkRow = 0;
            prevLX = 99.0f;
            if (openAnimActive) {
                openAnimActive = false;
                if (openAnimCb) { openAnimCb(); openAnimCb = NULL; }
            }
        }
    }
}

// ─── Timer callbacks ────────────────────────────────────────────────────────
static void blink_loop_cb(lv_timer_t *tmr) {
    if (!blinking) {
        eye_blink();
        uint32_t next = blinkMinMs + (esp_random() % (blinkMaxMs - blinkMinMs + 1));
        lv_timer_set_period(tmr, next);
    }
}

static void look_cb(lv_timer_t *tmr) {
    float minDist = lookRadius * 0.4f;
    // Guard against `esp_random() % 0` (undefined behavior) if a caller
    // ever sets lookRadius small enough that (lookRadius - minDist)*100
    // truncates to 0.  Floor at 1 so the modulus is always positive.
    int   distRange = (int)((lookRadius - minDist) * 100.0f);
    if (distRange < 1) distRange = 1;
    float newX = curX, newY = curY;
    for (int attempt = 0; attempt < 8; attempt++) {
        float angle = (float)(esp_random() % 360) * (3.14159f / 180.0f);
        float dist  = minDist + (float)(esp_random() % distRange) / 100.0f;
        newX = lookCenterX + cosf(angle) * dist;
        newY = lookCenterY + sinf(angle) * dist;
        clamp_pos(newX, newY);
        float dx = newX - curX;
        float dy = newY - curY;
        if (sqrtf(dx * dx + dy * dy) >= minDist) break;
    }
    tgtX = newX;
    tgtY = newY;
    uint32_t next = lookMinMs + (esp_random() % (lookMaxMs - lookMinMs + 1));
    lv_timer_set_period(tmr, next);
}

// ─── Periodic render ────────────────────────────────────────────────────────
static void render_tick(lv_timer_t *) {
    curX += (tgtX - curX) * LERP_POS;
    curY += (tgtY - curY) * LERP_POS;
    clamp_pos(curX, curY);

    float zd = (float)tgtZoom - curZoomF;
    if (fabsf(zd) < 0.5f) {
        curZoomF = (float)tgtZoom;
        lerpZoom = LERP_ZOOM_DEFAULT;
    } else {
        curZoomF += zd * lerpZoom;
    }

    // ── Tint & dim animation ─────────────────────────────────────────────
    float tintDelta = tgtTint - curTint;
    float dimDelta  = tgtDim  - curDim;
    bool  paletteChanged = false;
    if (fabsf(tintDelta) > 0.005f || fabsf(dimDelta) > 0.005f) {
        if (fabsf(tintDelta) > 0.005f) curTint += tintDelta * 0.35f;
        else                            curTint = tgtTint;
        if (fabsf(dimDelta)  > 0.005f) curDim  += dimDelta  * 0.25f;
        else                            curDim  = tgtDim;
        rebuild_palette();
        paletteChanged = true;
    } else if (curTint != tgtTint || curDim != tgtDim) {
        curTint = tgtTint;
        curDim  = tgtDim;
        rebuild_palette();
        paletteChanged = true;
    }
    bool tintChanged = paletteChanged;

    float lx = lightX, ly = lightY;
    if (lightFollows) {
        float halfW = (float)(SCR_W / 2);
        float halfH = (float)(SCR_H / 2);
        lx = (curX - halfW) / halfW;
        ly = (curY - halfH) / halfH;
    }

    float delta = fabsf(lx - prevLX) + fabsf(ly - prevLY);
    bool  needRedraw = delta > 0.003f || tintChanged;

    int prevBR = blinkRow;
    blink_tick();
    bool blinkChanged = (blinkRow != prevBR);

    if (needRedraw) {
        render_sphere(lx, ly);
        if (cleanBuf)
            memcpy(cleanBuf, cbuf, SPHERE_DIAM * SPHERE_DIAM * sizeof(lv_color_t));
        if (blinkRow > 0) apply_blink_mask(blinkRow);
        prevLX = lx;
        prevLY = ly;
        lv_obj_invalidate(canvas);
    } else if (blinkChanged) {
        if (blinkRow > prevBR) {
            blink_black_rows(prevBR, blinkRow);
        } else {
            blink_restore_rows(blinkRow, prevBR);
        }
        lv_obj_invalidate(canvas);
    }

    lv_coord_t nx = (lv_coord_t)(curX) - SPHERE_R;
    lv_coord_t ny = (lv_coord_t)(curY) - SPHERE_R;
    if (nx != lastCanvX || ny != lastCanvY) {
        lv_obj_set_pos(canvas, nx, ny);
        lastCanvX = nx;
        lastCanvY = ny;
    }
    uint16_t zoomInt = (uint16_t)(curZoomF + 0.5f);
    if (zoomInt != lastCanvZoom) {
        if (zoomInt < lastCanvZoom)
            lv_obj_invalidate(canvas);   // mark old larger area dirty before shrinking
        lv_img_set_zoom(canvas, zoomInt);
        lastCanvZoom = zoomInt;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void eye_init() {
    curX = tgtX = SCR_W / 2.0f;
    curY = tgtY = SCR_H / 2.0f;
    lookCenterX = curX;
    lookCenterY = curY;

    build_luts();

    const int totalPx = SPHERE_DIAM * SPHERE_DIAM;
    cbuf     = (lv_color_t *)ps_malloc(totalPx * sizeof(lv_color_t));
    cleanBuf = (lv_color_t *)ps_malloc(totalPx * sizeof(lv_color_t));
    if (!cbuf) { LOGLN1("eye cbuf alloc failed"); return; }
    if (!cleanBuf) { LOGLN1("eye cleanBuf alloc failed (blinks will be slower)"); }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, cbuf, SPHERE_DIAM, SPHERE_DIAM, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas, (SCR_W - SPHERE_DIAM) / 2, (SCR_H - SPHERE_DIAM) / 2);
    lv_img_set_antialias(canvas, true);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);

    render_sphere(0.0f, 0.0f);
    if (cleanBuf)
        memcpy(cleanBuf, cbuf, totalPx * sizeof(lv_color_t));
    prevLX = 0.0f; prevLY = 0.0f;

    renderTmr = lv_timer_create(render_tick, 16, NULL);
    lv_timer_pause(renderTmr);
}

void eye_show() {
    if (!canvas) return;
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(renderTmr);
    prevLX = 99.0f;   // force re-render + cache update on next tick
    // Sync tracking to current LVGL state so we don't override pre-set positions
    lastCanvX = lv_obj_get_x(canvas);
    lastCanvY = lv_obj_get_y(canvas);
    lastCanvZoom = lv_img_get_zoom(canvas);
}

void eye_hide() {
    if (!canvas) return;
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(renderTmr);
}

void eye_set_pos(float x, float y) {
    tgtX = x; tgtY = y;
    clamp_pos(tgtX, tgtY);
    lookCenterX = tgtX;
    lookCenterY = tgtY;
}

void eye_set_pos_immediate(float x, float y) {
    curX = tgtX = x;
    curY = tgtY = y;
    clamp_pos(curX, curY);
    clamp_pos(tgtX, tgtY);
    lookCenterX = curX;
    lookCenterY = curY;
    if (canvas) {
        lv_coord_t nx = (lv_coord_t)(curX) - SPHERE_R;
        lv_coord_t ny = (lv_coord_t)(curY) - SPHERE_R;
        lv_obj_set_pos(canvas, nx, ny);
        lastCanvX = nx;
        lastCanvY = ny;
    }
}

void eye_set_zoom(uint16_t z) {
    tgtZoom = z;
}

void eye_set_zoom_speed(float lerp) {
    lerpZoom = lerp;
}

void eye_set_zoom_immediate(uint16_t z) {
    curZoomF = (float)z;
    tgtZoom = z;
    if (canvas) {
        lv_img_set_zoom(canvas, z);
        lastCanvZoom = z;
    }
}

void eye_set_light(float lx, float ly) {
    lightX = lx; lightY = ly;
    lightFollows = false;
}

void eye_set_light_follows_pos(bool follows) {
    lightFollows = follows;
}

void eye_blink() {
    if (blinking) return;
    blinking = true;
    closeOnly = false;
    blinkPhase = 0;
    blinkRow = 0;
    blinkStartMs = millis();
}

void eye_close_anim(uint32_t duration_ms) {
    blinking = true;
    closeOnly = true;
    closeAnimDurMs = (int)duration_ms;
    blinkPhase = 0;
    blinkRow = 0;
    blinkStartMs = millis();
}

bool eye_is_blinking() {
    return blinking;
}

bool eye_at_target(float threshold) {
    float dx = tgtX - curX;
    float dy = tgtY - curY;
    return (dx * dx + dy * dy) <= (threshold * threshold);
}

void eye_get_pos(float &x, float &y) {
    x = curX;
    y = curY;
}

int16_t eye_get_radius() {
    return SPHERE_R;
}

void eye_start_blink_loop(uint32_t min_ms, uint32_t max_ms) {
    eye_stop_blink_loop();
    blinkMinMs = min_ms;
    blinkMaxMs = max_ms;
    uint32_t first = min_ms + (esp_random() % (max_ms - min_ms + 1));
    blinkLoopTmr = lv_timer_create(blink_loop_cb, first, NULL);
}

void eye_stop_blink_loop() {
    if (blinkLoopTmr) { lv_timer_del(blinkLoopTmr); blinkLoopTmr = NULL; }
}

void eye_start_lookaround(uint32_t min_ms, uint32_t max_ms) {
    eye_stop_lookaround();
    lookMinMs = min_ms;
    lookMaxMs = max_ms;
    uint32_t first = min_ms + (esp_random() % (max_ms - min_ms + 1));
    lookTmr = lv_timer_create(look_cb, first, NULL);
}

void eye_set_look_radius(float radius) {
    lookRadius = radius;
}

void eye_stop_lookaround() {
    if (lookTmr) { lv_timer_del(lookTmr); lookTmr = NULL; }
}

void eye_start_idle() {
    eye_set_light_follows_pos(true);
    eye_start_blink_loop(3000, 7000);
    eye_start_lookaround(2000, 5000);
}

void eye_stop_idle() {
    eye_stop_blink_loop();
    eye_stop_lookaround();
}

void eye_open_anim(void (*on_done)(void)) {
    openAnimCb = on_done;
    openAnimActive = true;
    closeOnly = false;
    blinking = true;
    blinkPhase = 1;
    blinkRow = SPHERE_R;
    blinkStartMs = millis();
    eye_show();
}

void eye_set_tint(float amount) {
    tgtTint = amount;
    if (amount < 0.0f) tgtTint = 0.0f;
    if (amount > 1.0f) tgtTint = 1.0f;
}

void eye_set_dim(float amount) {
    tgtDim = amount;
    if (amount < 0.0f) tgtDim = 0.0f;
    if (amount > 1.0f) tgtDim = 1.0f;
}

lv_obj_t *eye_get_canvas() {
    return canvas;
}
