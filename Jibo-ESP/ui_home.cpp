#include "ui_home.h"
#include <Arduino.h>
#include "ui_helpers.h"
#include "states.h"
#include "eye.h"
#include "pin_config.h"
#include "ble_link.h"

// PMU functions defined in Jibo-ESP.ino
extern int  pmu_battery_percent();
extern bool pmu_is_charging();

// ─── Icon image declarations ──────────────────────────────────────────────
LV_IMG_DECLARE(icon_settings);
LV_IMG_DECLARE(icon_phone);
LV_IMG_DECLARE(icon_messages);

// ─── File-scoped statics ──────────────────────────────────────────────────
static lv_obj_t  *uiHomeContainer = NULL;
static lv_obj_t  *battArc         = NULL;
static uint32_t   lastBattUpdateMs = 0;

// ─── Battery ring ─────────────────────────────────────────────────────────

static lv_color_t batt_color(int pct, bool charging) {
    if (charging && pct >= 100) return lv_color_make(60, 220, 60);
    if (charging)               return lv_color_make(60, 160, 255);
    if (pct > 30)               return lv_color_white();
    if (pct > 15)               return lv_color_make(255, 180, 0);
    return lv_color_make(255, 50, 50);
}

static void update_battery_ring() {
    if (!battArc) return;
    int pct = pmu_battery_percent();
    if (pct < 0) return;
    if (pct > 100) pct = 100;

    lv_arc_set_value(battArc, pct);
    lv_obj_set_style_arc_color(battArc, batt_color(pct, pmu_is_charging()), LV_PART_INDICATOR);
}

static void build_battery_ring() {
    if (battArc) { lv_obj_del(battArc); battArc = NULL; }

    int pct = pmu_battery_percent();
    if (pct < 0) return;
    if (pct > 100) pct = 100;

    battArc = lv_arc_create(lv_scr_act());
    lv_obj_set_size(battArc, UI_SCR_W - 4, UI_SCR_H - 4);
    lv_obj_align(battArc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(battArc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(battArc, lv_color_make(30, 30, 30), LV_PART_MAIN);
    lv_obj_set_style_arc_width(battArc, 3, LV_PART_MAIN);

    lv_obj_set_style_arc_color(battArc, batt_color(pct, pmu_is_charging()), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(battArc, 3, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(battArc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(battArc, 0, LV_PART_KNOB);

    lv_arc_set_bg_angles(battArc, 0, 360);
    lv_arc_set_mode(battArc, LV_ARC_MODE_NORMAL);
    lv_arc_set_rotation(battArc, 270);

    lv_arc_set_range(battArc, 0, 100);
    lv_arc_set_value(battArc, pct);
    lastBattUpdateMs = millis();
}

static void fade_out_battery_ring(uint32_t duration) {
    if (battArc) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, battArc);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_time(&a, duration);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    }
}

// ─── Icon callbacks ───────────────────────────────────────────────────────

static void settings_icon_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (uiHomeContainer) fade_out_children(uiHomeContainer, 150);
    fade_out_battery_ring(150);
    states_set_enter_ms(millis());
    states_set_sub_step(202);
}

static void draw_settings_icon(lv_obj_t *parent) {
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &icon_settings);
    lv_obj_set_style_img_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void phone_finder_icon_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (uiHomeContainer) fade_out_children(uiHomeContainer, 150);
    fade_out_battery_ring(150);
    states_set_enter_ms(millis());
    states_set_sub_step(203);
}

static void draw_phone_icon(lv_obj_t *parent) {
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &icon_phone);
    lv_obj_set_style_img_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void messages_icon_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    if (uiHomeContainer) fade_out_children(uiHomeContainer, 150);
    fade_out_battery_ring(150);
    states_set_enter_ms(millis());
    states_set_sub_step(204);
}

static void draw_messages_icon(lv_obj_t *parent) {
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &icon_messages);
    lv_obj_set_style_img_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

// ─── App button / label builders ──────────────────────────────────────────

static lv_obj_t *make_app_btn(lv_obj_t *parent, lv_coord_t xOff, lv_coord_t yOff,
                               lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 120, 120);
    lv_obj_align(btn, LV_ALIGN_CENTER, xOff, yOff);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 60, 0);
    lv_obj_add_event_cb(btn, tap_press_cb, LV_EVENT_PRESSED, NULL);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *make_app_label(lv_obj_t *parent, const char *text,
                                 lv_coord_t xOff, lv_coord_t yOff) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, xOff, yOff);
    return lbl;
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void ui_home_init() {
    eye_hide();

    // Destroy any leftover home container
    if (uiHomeContainer) { lv_obj_del(uiHomeContainer); uiHomeContainer = NULL; }

    uiHomeContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(uiHomeContainer);
    lv_obj_set_size(uiHomeContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(uiHomeContainer, 0, 0);
    lv_obj_clear_flag(uiHomeContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Center Settings if phone isn't paired, otherwise show both side by side
    if (ble_is_paired()) {
        lv_obj_t *msgBtn = make_app_btn(uiHomeContainer, 0, -80, messages_icon_cb);
        draw_messages_icon(msgBtn);
        make_app_label(uiHomeContainer, "Messages", 0, -15);

        lv_obj_t *settBtn = make_app_btn(uiHomeContainer, -100, 20, settings_icon_cb);
        draw_settings_icon(settBtn);
        make_app_label(uiHomeContainer, "Settings", -100, 95);

        lv_obj_t *findBtn = make_app_btn(uiHomeContainer, 100, 20, phone_finder_icon_cb);
        draw_phone_icon(findBtn);
        make_app_label(uiHomeContainer, "Find Phone", 100, 95);
    } else {
        lv_obj_t *settBtn = make_app_btn(uiHomeContainer, 0, -20, settings_icon_cb);
        draw_settings_icon(settBtn);
        make_app_label(uiHomeContainer, "Settings", 0, 55);
    }

    build_battery_ring();

    block_touch_until_release();
    fade_in_children(uiHomeContainer, 150);
}

void ui_home_tick() {
    uint32_t sub = states_sub_step();
    uint32_t enterMs = states_enter_ms();

    if (sub == 201 && millis() - enterMs >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_UI_TO_EYE);
        return;
    }
    if (sub == 202 && millis() - enterMs >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_SETTINGS);
        return;
    }
    if (sub == 203 && millis() - enterMs >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_PHONE_FINDER);
        return;
    }
    if (sub == 204 && millis() - enterMs >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_MESSAGES);
        return;
    }
    if (millis() - lastBattUpdateMs >= 5000) {
        update_battery_ring();
        lastBattUpdateMs = millis();
    }
}

void ui_home_destroy() {
    if (uiHomeContainer) { lv_obj_del(uiHomeContainer); uiHomeContainer = NULL; }
    if (battArc)         { lv_obj_del(battArc);         battArc = NULL; }
}

void ui_home_fade_out(uint32_t duration) {
    if (uiHomeContainer) fade_out_children(uiHomeContainer, duration);
    fade_out_battery_ring(duration);
}

void ui_home_btn_press() {
    if (uiHomeContainer) fade_out_children(uiHomeContainer, 150);
    fade_out_battery_ring(150);
    states_set_enter_ms(millis());
    states_set_sub_step(201);
}
