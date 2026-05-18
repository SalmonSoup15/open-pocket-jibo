#pragma once
#include <lvgl.h>

void eye_init();

void eye_show();
void eye_hide();

void eye_set_pos(float x, float y);
void eye_set_pos_immediate(float x, float y);

void eye_set_zoom(uint16_t z);
void eye_set_zoom_speed(float lerp);
void eye_set_zoom_immediate(uint16_t z);

void eye_set_light(float lx, float ly);
void eye_set_light_follows_pos(bool follows);

void eye_set_tint(float amount);   // 0.0 = white/gray, 1.0 = full cyan-blue
void eye_set_dim(float amount);    // 0.0 = full bright, 1.0 = fully dark

void eye_blink();
void eye_close_anim(uint32_t duration_ms);
void eye_start_blink_loop(uint32_t min_ms, uint32_t max_ms);
void eye_stop_blink_loop();

void eye_start_lookaround(uint32_t min_ms, uint32_t max_ms);
void eye_set_look_radius(float radius);
void eye_stop_lookaround();

void eye_start_idle();
void eye_stop_idle();

void eye_open_anim(void (*on_done)(void));

void eye_loading_start();
void eye_loading_stop(void (*on_done)(void) = nullptr);
bool eye_is_loading();

bool eye_is_blinking();

// True when the eye's lerped position has settled within `threshold`
// pixels of its target.  Used by the speaking-state tick to wait for
// the centring lerp to complete before releasing a queued pill — that
// way the eye glides smoothly to centre and only then does the pill
// pop up, instead of either fighting the lerp or snapping abruptly.
bool eye_at_target(float threshold = 1.5f);

lv_obj_t *eye_get_canvas();

void eye_get_pos(float &x, float &y);
int16_t eye_get_radius();
