#include "ui_helpers.h"
#include <Arduino.h>

// ─── Screen geometry constants ─────────────────────────────────────────────
const int16_t UI_SCR_W = LCD_WIDTH;
const int16_t UI_SCR_H = LCD_HEIGHT;
const int16_t UI_CX    = LCD_WIDTH  / 2;
const int16_t UI_CY    = LCD_HEIGHT / 2;

// ─── Touch state ───────────────────────────────────────────────────────────
#define TAP_MOVE_LIMIT 20
static lv_point_t tapDownPt = {0, 0};

// ─── Touch helpers ─────────────────────────────────────────────────────────

void block_touch_until_release() {
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            indev->proc.wait_until_release = 1;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void tap_press_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_get_point(indev, &tapDownPt);
}

bool is_valid_tap() {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    return (abs(p.x - tapDownPt.x) < TAP_MOVE_LIMIT &&
            abs(p.y - tapDownPt.y) < TAP_MOVE_LIMIT);
}

// ─── Fade animations ───────────────────────────────────────────────────────

void fade_in_label(lv_obj_t *lbl, uint32_t duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, duration);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
}

void fade_out_label(lv_obj_t *lbl, uint32_t duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, duration);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
}

void fade_in_obj(lv_obj_t *obj, uint32_t duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, duration);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
}

void fade_out_obj(lv_obj_t *obj, uint32_t duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, duration);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
}

void fade_in_children(lv_obj_t *parent, uint32_t duration) {
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        lv_obj_set_style_opa(child, LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, child);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_time(&a, duration);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    }
}

void fade_out_children(lv_obj_t *parent, uint32_t duration) {
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, child);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_time(&a, duration);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
            lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    }
}

// ─── Widget builders ───────────────────────────────────────────────────────

lv_obj_t *ui_create_label(const char *txt, lv_coord_t y, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(lv_scr_act());
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, UI_SCR_W - 40);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_opa(lbl, LV_OPA_TRANSP, 0);
    return lbl;
}

lv_obj_t *ui_create_dark_button(const char *text, lv_coord_t y, lv_event_cb_t cb,
                                lv_obj_t *parent) {
    lv_obj_t *b = lv_btn_create(parent ? parent : lv_scr_act());
    lv_obj_set_size(b, 260, 52);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);

    lv_obj_add_event_cb(b, tap_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    lv_obj_set_style_opa(b, LV_OPA_TRANSP, 0);
    return b;
}

// ─── Small eye sphere renderer ────────────────────────────────────────────

void ui_render_small_eye(uint8_t *buf, int size) {
    int r = size / 2;
    float invR = 1.0f / (float)r;
    int bpp = LV_IMG_PX_SIZE_ALPHA_BYTE;

    for (int py = 0; py < size; py++) {
        float ny = ((float)(py - r) + 0.5f) * invR;
        for (int px = 0; px < size; px++) {
            uint8_t *p = buf + (py * size + px) * bpp;
            float nx = ((float)(px - r) + 0.5f) * invR;
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
            lv_color_t c = lv_color_make(v, v, v);
            memcpy(p, &c, sizeof(lv_color_t));
            p[sizeof(lv_color_t)] = 255;
        }
    }
}
