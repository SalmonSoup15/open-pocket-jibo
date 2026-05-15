#pragma once
#include <stdint.h>

void ui_home_init();       // creates home screen + battery ring
void ui_home_tick();       // handles subStep transitions, battery updates
void ui_home_destroy();    // cleans up uiHomeContainer + battArc
void ui_home_fade_out(uint32_t duration);  // fade out children + battery ring (visual only)
void ui_home_btn_press();  // boot button → fade out + set subStep=201
