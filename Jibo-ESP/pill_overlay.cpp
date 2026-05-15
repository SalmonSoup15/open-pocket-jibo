#include "pill_overlay.h"
#include "log.h"
#include "pin_config.h"
#include <lvgl.h>
#include <string.h>

// ─── Layout constants ───────────────────────────────────────────────────────
static const int16_t SCR_W       = LCD_WIDTH;
static const int16_t SCR_H       = LCD_HEIGHT;
static const int16_t PILL_H      = 70;
static const int16_t PILL_FULL_W = 320;
static const int16_t PILL_Y      = SCR_H - 115;
static const int16_t PILL_OFF_Y  = SCR_H;
static const int16_t CHECK_SZ    = 48;
static const int16_t CHECK_INSET = (PILL_H - CHECK_SZ) / 2;

// ─── Icon styling ───────────────────────────────────────────────────────────
//
// Each icon variant defines: circle background colour and the LVGL
// symbol drawn on top of it.  Symbol colour is always white for
// consistent contrast across all four backgrounds.
struct IconSpec {
    lv_color_t  bg;
    const char *symbol;
};

static IconSpec icon_spec(PillIcon icon) {
    switch (icon) {
        case PILL_ICON_SUCCESS:
            return { lv_color_make(60, 200, 90), LV_SYMBOL_OK };
        case PILL_ICON_ERROR:
            // Bright red.  LV_SYMBOL_CLOSE renders as an X glyph.
            return { lv_color_make(220, 60, 60), LV_SYMBOL_CLOSE };
        case PILL_ICON_CHARGING:
            // Bolt is the closest LVGL has to a lightning glyph.
            return { lv_color_make(60, 120, 230), LV_SYMBOL_CHARGE };
        case PILL_ICON_LOW_BAT:
            // Amber/yellow — readable on the dark pill, distinct from red.
            return { lv_color_make(230, 180, 40), LV_SYMBOL_BATTERY_1 };
        case PILL_ICON_NOTIF:
            // Violet/purple bell — distinct from charging blue and
            // success green so the user can tell at a glance "this is
            // about my phone, not Jibo's status."
            return { lv_color_make(150, 90, 220), LV_SYMBOL_BELL };
    }
    return { lv_color_make(80, 80, 80), LV_SYMBOL_OK };
}

// ─── Active pill state (single instance — replaces previous on re-show) ─────
static lv_obj_t   *gPillCont    = NULL;
static lv_obj_t   *gPillCheck   = NULL;
static lv_obj_t   *gPillLabel   = NULL;
static lv_obj_t   *gPillSubLabel = NULL;
// Hold-phase timer pointer.  Tracked globally so that if a new pill comes
// in while the previous one is in its hold phase we can cancel its
// pending collapse — otherwise the stale timer fires, sees the (new)
// gPillCont, and prematurely collapses the fresh pill.
static lv_timer_t *gPillHoldTmr = NULL;
static uint32_t  gHoldMs      = 2000;

// Static storage for the deferred lv_async_call payload (only one
// pending request at a time — overwritten if pill_show is hammered).
static char      gPendingText[96]    = {0};
static char      gPendingSubtext[96] = {0};
static PillIcon  gPendingIcon        = PILL_ICON_SUCCESS;
static uint32_t  gPendingHoldMs      = 2000;
static volatile bool gPendingValid   = false;

// ─── Queued-for-speaking pill ───────────────────────────────────────────────
static char     gQueuedText[96]    = {0};
static char     gQueuedSubtext[96] = {0};
static PillIcon gQueuedIcon        = PILL_ICON_SUCCESS;
static volatile bool gQueuedValid  = false;

// Forward declarations for the animation chain.
static void start_phase1_popup();
static void start_phase2_expand();
static void start_phase3_hold();
static void start_phase4_collapse();
static void start_phase5_dropoff();
static void destroy_pill();

// ─── Animation helpers ──────────────────────────────────────────────────────
static void anim_y(lv_obj_t *obj, int32_t from, int32_t to,
                   uint32_t duration, lv_anim_path_cb_t path,
                   lv_anim_ready_cb_t ready) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, duration);
    if (path)  lv_anim_set_path_cb(&a, path);
    if (ready) lv_anim_set_ready_cb(&a, ready);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_set_y((lv_obj_t *)o, (lv_coord_t)v);
    });
    lv_anim_start(&a);
}

static void anim_width(lv_obj_t *obj, int32_t from, int32_t to,
                       uint32_t duration, lv_anim_path_cb_t path,
                       lv_anim_ready_cb_t ready) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, duration);
    if (path)  lv_anim_set_path_cb(&a, path);
    if (ready) lv_anim_set_ready_cb(&a, ready);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_t *cont = (lv_obj_t *)o;
        lv_obj_set_width(cont, v);
        lv_obj_set_x(cont, (SCR_W - v) / 2);
    });
    lv_anim_start(&a);
}

// ─── Phase implementations ──────────────────────────────────────────────────

static void destroy_pill() {
    if (gPillCont) lv_obj_del_async(gPillCont);
    gPillCont    = NULL;
    gPillCheck   = NULL;
    gPillLabel   = NULL;
    gPillSubLabel = NULL;
    if (gPillHoldTmr) { lv_timer_del(gPillHoldTmr); gPillHoldTmr = NULL; }
}

static void start_phase5_dropoff() {
    if (!gPillCont) return;
    anim_y(gPillCont, PILL_Y, PILL_OFF_Y, 260, lv_anim_path_ease_in,
        [](lv_anim_t *a) { destroy_pill(); });
}

static void hold_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    if (gPillHoldTmr == t) gPillHoldTmr = NULL;
    start_phase4_collapse();
}

static void start_phase4_collapse() {
    if (!gPillCont) return;
    auto fade_out_label = [](lv_obj_t *obj) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_time(&a, 140);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    };
    if (gPillLabel)    fade_out_label(gPillLabel);
    if (gPillSubLabel) fade_out_label(gPillSubLabel);
    anim_width(gPillCont, PILL_FULL_W, PILL_H, 260, lv_anim_path_ease_in_out,
        [](lv_anim_t *a) { start_phase5_dropoff(); });
}

static void start_phase3_hold() {
    if (!gPillCont) return;
    // Cancel any stale hold timer left over from a pill that was
    // replaced before its previous collapse fired (build_and_animate
    // already does this defensively, but re-doing it here makes the
    // invariant "only one hold timer alive" obvious).
    if (gPillHoldTmr) { lv_timer_del(gPillHoldTmr); gPillHoldTmr = NULL; }
    gPillHoldTmr = lv_timer_create(hold_timer_cb, gHoldMs, NULL);
    lv_timer_set_repeat_count(gPillHoldTmr, 1);
}

static void start_phase2_expand() {
    if (!gPillCont) return;
    auto fade_in_label = [](lv_obj_t *obj) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_time(&a, 180);
        lv_anim_set_delay(&a, 120);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    };
    if (gPillLabel)    fade_in_label(gPillLabel);
    if (gPillSubLabel) fade_in_label(gPillSubLabel);
    anim_width(gPillCont, PILL_H, PILL_FULL_W, 280, lv_anim_path_ease_out,
        [](lv_anim_t *a) { start_phase3_hold(); });
}

static void start_phase1_popup() {
    if (!gPillCont) return;
    anim_y(gPillCont, PILL_OFF_Y, PILL_Y, 280, lv_anim_path_overshoot,
        [](lv_anim_t *a) { start_phase2_expand(); });
}

// ─── Construction ───────────────────────────────────────────────────────────

static void build_and_animate(PillIcon icon, const char *text,
                              const char *subtext, uint32_t hold_ms) {
    // Kill any in-flight hold timer from a previous pill BEFORE we
    // tear the old objects down — otherwise the timer fires later
    // against gPillCont (which by then is the new pill) and collapses
    // it prematurely.
    if (gPillHoldTmr) { lv_timer_del(gPillHoldTmr); gPillHoldTmr = NULL; }
    if (gPillCont) {
        lv_anim_del(gPillCont, NULL);
        if (gPillLabel)    lv_anim_del(gPillLabel, NULL);
        if (gPillSubLabel) lv_anim_del(gPillSubLabel, NULL);
        if (gPillCheck)    lv_anim_del(gPillCheck, NULL);
        lv_obj_del(gPillCont);
        gPillCont = gPillLabel = gPillSubLabel = gPillCheck = NULL;
    }

    gHoldMs = hold_ms;
    bool hasSub = (subtext && subtext[0]);
    IconSpec spec = icon_spec(icon);

    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    gPillCont = cont;
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, PILL_H, PILL_H);
    lv_obj_set_pos(cont, (SCR_W - PILL_H) / 2, PILL_OFF_Y);
    lv_obj_set_style_radius(cont, PILL_H / 2, 0);
    lv_obj_set_style_bg_color(cont, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_shadow_width(cont, 14, 0);
    lv_obj_set_style_shadow_color(cont, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(cont, LV_OPA_50, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    // Icon circle (left side) — colour and symbol per variant.
    lv_obj_t *chk = lv_obj_create(cont);
    gPillCheck = chk;
    lv_obj_remove_style_all(chk);
    lv_obj_set_size(chk, CHECK_SZ, CHECK_SZ);
    lv_obj_set_pos(chk, CHECK_INSET, CHECK_INSET);
    lv_obj_set_style_radius(chk, CHECK_SZ / 2, 0);
    lv_obj_set_style_bg_color(chk, spec.bg, 0);
    lv_obj_set_style_bg_opa(chk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chk, 0, 0);
    lv_obj_clear_flag(chk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chk, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *checkLbl = lv_label_create(chk);
    lv_label_set_text(checkLbl, spec.symbol);
    lv_obj_set_style_text_color(checkLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(checkLbl, &lv_font_montserrat_24, 0);
    lv_obj_center(checkLbl);

    // Body label, with optional stacked subtext.
    int16_t textLeft = CHECK_INSET + CHECK_SZ + 14;
    lv_obj_t *lbl = lv_label_create(cont);
    gPillLabel = lbl;
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, 0);
    if (hasSub) lv_obj_align(lbl, LV_ALIGN_LEFT_MID, textLeft, -10);
    else        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, textLeft, 0);

    if (hasSub) {
        lv_obj_t *sub = lv_label_create(cont);
        gPillSubLabel = sub;
        lv_label_set_text(sub, subtext);
        lv_obj_set_style_text_color(sub, lv_color_make(170, 170, 170), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_opa(sub, LV_OPA_TRANSP, 0);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, textLeft, 12);
    }

    LOG2("[pill] show: icon=%d \"%s\"%s%s (hold=%ums)\n",
         (int)icon,
         text ? text : "",
         hasSub ? " | " : "",
         hasSub ? subtext : "",
         (unsigned)hold_ms);
    start_phase1_popup();
}

static void pill_show_async_cb(void *userData) {
    if (!gPendingValid) return;
    gPendingValid = false;
    build_and_animate(gPendingIcon,
                      gPendingText,
                      gPendingSubtext[0] ? gPendingSubtext : NULL,
                      gPendingHoldMs);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void pill_show(PillIcon icon, const char *text, const char *subtext, uint32_t hold_ms) {
    if (!text) text = "";
    strncpy(gPendingText, text, sizeof(gPendingText) - 1);
    gPendingText[sizeof(gPendingText) - 1] = 0;
    if (subtext) {
        strncpy(gPendingSubtext, subtext, sizeof(gPendingSubtext) - 1);
        gPendingSubtext[sizeof(gPendingSubtext) - 1] = 0;
    } else {
        gPendingSubtext[0] = 0;
    }
    gPendingIcon   = icon;
    gPendingHoldMs = hold_ms;
    gPendingValid  = true;
    lv_async_call(pill_show_async_cb, NULL);
}

void pill_queue_for_speaking(PillIcon icon, const char *text, const char *subtext) {
    if (!text) text = "";
    strncpy(gQueuedText, text, sizeof(gQueuedText) - 1);
    gQueuedText[sizeof(gQueuedText) - 1] = 0;
    if (subtext) {
        strncpy(gQueuedSubtext, subtext, sizeof(gQueuedSubtext) - 1);
        gQueuedSubtext[sizeof(gQueuedSubtext) - 1] = 0;
    } else {
        gQueuedSubtext[0] = 0;
    }
    gQueuedIcon  = icon;
    gQueuedValid = true;
    LOG2("[pill] queued for speaking: icon=%d \"%s\"\n", (int)icon, text);
}

void pill_release_pending() {
    if (!gQueuedValid) return;
    gQueuedValid = false;
    pill_show(gQueuedIcon,
              gQueuedText,
              gQueuedSubtext[0] ? gQueuedSubtext : NULL);
}

bool pill_has_pending() {
    return gQueuedValid;
}

bool pill_is_active() {
    return gPillCont != NULL || gPendingValid;
}
