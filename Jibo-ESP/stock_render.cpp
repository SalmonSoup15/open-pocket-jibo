#include "stock_render.h"
#include "log.h"
#include <math.h>

// Round-display geometry.  We keep widgets within an inscribed rectangle
// so corners don't get clipped by the AMOLED's circular mask.  466x466
// display, ~233 px radius — leave a 24 px safety inset and the inscribed
// square is ~324 px, but for a horizontal layout (wide ticker, narrow
// chart band in the middle) we can use a wider band and just keep the
// vertical extents tight.

static const int SCR_DIM = 466;

// ─── Range callback ────────────────────────────────────────────────────────

static stock_range_cb_t sRangeCb = nullptr;

void stock_set_range_callback(stock_range_cb_t cb) { sRangeCb = cb; }

static void range_btn_cb(lv_event_t *e) {
    if (!sRangeCb) return;
    StockRange r = (StockRange)(uintptr_t)lv_event_get_user_data(e);
    sRangeCb(r);
}

// Colors
static lv_color_t col_green() { return lv_color_make(60, 220, 110); }
static lv_color_t col_red()   { return lv_color_make(240, 80,  80); }
static lv_color_t col_grey()  { return lv_color_make(140, 140, 140); }
static lv_color_t col_dim()   { return lv_color_make(70, 70, 70); }

// Free LVGL line points array on widget delete.  Mirrors the helper used
// in latex_render — lv_line doesn't own its point array so we keep it in
// user_data and free it ourselves.
static void free_user_data_cb(lv_event_t *e) {
    void *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (p) free(p);
}

// Make a transparent fullscreen container — common parent for all three
// (loading / error / data) cards so the caller's animation logic doesn't
// have to special-case them.
static lv_obj_t *make_card_container(lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCR_DIM, SCR_DIM);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont, 0, 0);
    return cont;
}

// ─── Loading card ──────────────────────────────────────────────────────────

lv_obj_t *stock_render_loading(lv_obj_t *parent, const char *symbol) {
    lv_obj_t *cont = make_card_container(parent);
    if (!cont) return NULL;

    lv_obj_t *sym = lv_label_create(cont);
    lv_label_set_text(sym, (symbol && *symbol) ? symbol : "?");
    lv_obj_set_style_text_color(sym, lv_color_white(), 0);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_48, 0);
    lv_obj_align(sym, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *spinner = lv_spinner_create(cont, 800, 60);
    lv_obj_set_size(spinner, 50, 50);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_arc_color(spinner, col_dim(), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

    return cont;
}

// ─── Error card ────────────────────────────────────────────────────────────

lv_obj_t *stock_render_error(lv_obj_t *parent, const char *symbol,
                             const char *msg) {
    lv_obj_t *cont = make_card_container(parent);
    if (!cont) return NULL;

    if (symbol && *symbol) {
        lv_obj_t *sym = lv_label_create(cont);
        lv_label_set_text(sym, symbol);
        lv_obj_set_style_text_color(sym, col_grey(), 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_24, 0);
        lv_obj_align(sym, LV_ALIGN_CENTER, 0, -50);
    }

    lv_obj_t *line = lv_label_create(cont);
    lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(line, 280);
    lv_label_set_text(line, (msg && *msg) ? msg : "Couldn't fetch stock data");
    lv_obj_set_style_text_color(line, lv_color_white(), 0);
    lv_obj_set_style_text_font(line, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);

    return cont;
}

// ─── Data card ─────────────────────────────────────────────────────────────

// Format the % change as e.g. "+1.23%" or "-2.40%".  Uses snprintf into a
// caller-owned buffer so we never return a pointer into a temp.
static void format_percent(char *out, size_t outSz, float pct) {
    snprintf(out, outSz, "%s%.2f%%", pct >= 0 ? "+" : "", pct);
}

static void format_price(char *out, size_t outSz, float price) {
    // Pick precision based on magnitude — penny stocks need the decimals,
    // BRK.A doesn't.
    if (price >= 1000.0f)      snprintf(out, outSz, "$%.0f",  price);
    else if (price >= 100.0f)  snprintf(out, outSz, "$%.1f",  price);
    else                       snprintf(out, outSz, "$%.2f",  price);
}

// Build the line-chart in a child object centered horizontally between
// `xMin` and `xMax`, vertically between `yTop` and `yBot`.  Walks
// `points` (non-NaN, non-zero) and renders an lv_line through them.
// Color tracks whether the chart trends up (green) or down (red).
static void render_line_chart(lv_obj_t *parent, const float *points, int count,
                              int xMin, int yTop, int xMax, int yBot,
                              bool trendUp) {
    if (count < 2) {
        // Single sample — just draw a flat horizontal indicator.
        int y = (yTop + yBot) / 2;
        lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
        if (!pts) return;
        pts[0].x = xMin; pts[0].y = y;
        pts[1].x = xMax; pts[1].y = y;
        lv_obj_t *line = lv_line_create(parent);
        lv_line_set_points(line, pts, 2);
        lv_obj_set_style_line_color(line, col_dim(), 0);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_user_data(line, pts);
        lv_obj_add_event_cb(line, free_user_data_cb, LV_EVENT_DELETE, NULL);
        return;
    }

    // Find min/max of valid points so we can scale into [yTop, yBot].
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < count; i++) {
        float v = points[i];
        if (!isfinite(v)) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (lo == INFINITY || hi == lo) {
        // Degenerate — render a flat line at center.
        hi = lo + 1.0f;
    }

    // Allocate point array.  lv_line requires the buffer to outlive the
    // widget; we hand ownership to the widget via user_data + a delete
    // event callback (mirrors render_sqrt in latex_render.cpp).
    lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * count);
    if (!pts) return;

    int chartW = xMax - xMin;
    int chartH = yBot - yTop;
    for (int i = 0; i < count; i++) {
        float t = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        float v = points[i];
        if (!isfinite(v)) v = lo;
        float n = (v - lo) / (hi - lo);     // 0..1
        pts[i].x = xMin + (lv_coord_t)(t * chartW);
        // Invert Y: high price → low pixel
        pts[i].y = yBot - (lv_coord_t)(n * chartH);
    }

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, count);
    lv_obj_set_style_line_color(line, trendUp ? col_green() : col_red(), 0);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_user_data(line, pts);
    lv_obj_add_event_cb(line, free_user_data_cb, LV_EVENT_DELETE, NULL);
}

// Build a "no graph available" placeholder: a horizontal dotted bar where
// the line chart would be.  Less jarring than an empty middle area.
static void render_chart_unavailable(lv_obj_t *parent,
                                     int xMin, int yTop, int xMax, int yBot) {
    int y = (yTop + yBot) / 2;
    int dotSize = 4;
    int spacing = 14;
    for (int x = xMin; x <= xMax; x += spacing) {
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, dotSize, dotSize);
        lv_obj_set_pos(dot, x, y - dotSize / 2);
        lv_obj_set_style_bg_color(dot, col_dim(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, dotSize, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t *stock_render(lv_obj_t *parent, const StockQuote &q,
                       StockRange activeRange) {
    lv_obj_t *cont = make_card_container(parent);
    if (!cont) return NULL;

    bool up = q.changePercent >= 0;
    lv_color_t changeCol = up ? col_green() : col_red();

    // ── Top-left: ticker ────────────────────────────────────────────────────
    lv_obj_t *symLbl = lv_label_create(cont);
    lv_label_set_text(symLbl, q.symbol);
    lv_obj_set_style_text_color(symLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(symLbl, &lv_font_montserrat_24, 0);
    // Align to circular-safe top-left — the round mask cuts the corners,
    // so we inset both axes from the absolute corner.
    lv_obj_align(symLbl, LV_ALIGN_TOP_LEFT, 90, 95);

    // ── Top-right: % change ─────────────────────────────────────────────────
    char pctBuf[16];
    format_percent(pctBuf, sizeof(pctBuf), q.changePercent);
    lv_obj_t *pctLbl = lv_label_create(cont);
    lv_label_set_text(pctLbl, pctBuf);
    lv_obj_set_style_text_color(pctLbl, changeCol, 0);
    lv_obj_set_style_text_font(pctLbl, &lv_font_montserrat_24, 0);
    lv_obj_align(pctLbl, LV_ALIGN_TOP_RIGHT, -90, 95);

    // ── Center: price ───────────────────────────────────────────────────────
    char priceBuf[16];
    format_price(priceBuf, sizeof(priceBuf), q.current);
    lv_obj_t *priceLbl = lv_label_create(cont);
    lv_label_set_text(priceLbl, priceBuf);
    lv_obj_set_style_text_color(priceLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(priceLbl, &lv_font_montserrat_48, 0);
    lv_obj_align(priceLbl, LV_ALIGN_CENTER, 0, -30);

    // ── Center bottom: chart ────────────────────────────────────────────────
    // Reserve a band wide enough to look intentional but narrow enough to
    // fit inside the round mask near the bottom of the screen.  At y≈300
    // the inscribed-circle width is ~360 px, so 80..386 leaves a healthy
    // safety margin on both sides.
    int xMin = 80;
    int xMax = SCR_DIM - 80;
    int yTop = 290;
    int yBot = 350;
    if (q.pointsCount >= 2) {
        render_line_chart(cont, q.points, q.pointsCount,
                          xMin, yTop, xMax, yBot, up);
    } else {
        render_chart_unavailable(cont, xMin, yTop, xMax, yBot);
    }

    // ── Timeframe buttons ──────────────────────────────────────────────────
    static const StockRange kRanges[] = {
        RANGE_1D, RANGE_1W, RANGE_1M, RANGE_6M, RANGE_YTD, RANGE_1Y
    };
    static const int kN = sizeof(kRanges) / sizeof(kRanges[0]);
    int btnW = 48, btnH = 28, gap = 4;
    int totalW = kN * btnW + (kN - 1) * gap;
    int startX = (SCR_DIM - totalW) / 2;
    int btnY = 370;

    for (int i = 0; i < kN; i++) {
        bool active = (kRanges[i] == activeRange);

        lv_obj_t *btn = lv_btn_create(cont);
        lv_obj_set_size(btn, btnW, btnH);
        lv_obj_set_pos(btn, startX + i * (btnW + gap), btnY);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, stock_range_label(kRanges[i]));
        lv_obj_set_style_text_color(lbl,
            active ? lv_color_white() : col_grey(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, range_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)kRanges[i]);
    }

    return cont;
}
