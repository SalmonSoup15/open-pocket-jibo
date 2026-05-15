#pragma once
#include <lvgl.h>
#include "pin_config.h"

// ─── Screen geometry constants ─────────────────────────────────────────────
extern const int16_t UI_SCR_W;
extern const int16_t UI_SCR_H;
extern const int16_t UI_CX;
extern const int16_t UI_CY;

// ─── Fade animations ───────────────────────────────────────────────────────
void fade_in_children(lv_obj_t *parent, uint32_t duration);
void fade_out_children(lv_obj_t *parent, uint32_t duration);
void fade_in_label(lv_obj_t *lbl, uint32_t duration);
void fade_out_label(lv_obj_t *lbl, uint32_t duration);
void fade_in_obj(lv_obj_t *obj, uint32_t duration);
void fade_out_obj(lv_obj_t *obj, uint32_t duration);

// ─── Touch helpers ─────────────────────────────────────────────────────────
void block_touch_until_release();
bool is_valid_tap();
void tap_press_cb(lv_event_t *e);    // LVGL event callback

// ─── Widget builders ───────────────────────────────────────────────────────
lv_obj_t *ui_create_label(const char *txt, lv_coord_t y, const lv_font_t *font);
lv_obj_t *ui_create_dark_button(const char *text, lv_coord_t y, lv_event_cb_t cb,
                                lv_obj_t *parent = nullptr);

// ─── Small eye sphere renderer ────────────────────────────────────────────
// Renders a white 3D-shaded sphere into a pre-allocated ARGB buffer.
// Buffer must be at least  size * size * LV_IMG_PX_SIZE_ALPHA_BYTE  bytes.
// Pixels outside the sphere are fully transparent.
void ui_render_small_eye(uint8_t *buf, int size);
