#pragma once
#include <stdint.h>

void msg_ui_init();
void msg_ui_tick();
void msg_ui_destroy();
void msg_ui_btn_press();
void msg_ui_btn_long_press();
void msg_ui_btn_release();
void msg_ui_fade_out(uint32_t duration);

// Called from Gemini tool path (enters compose directly, not from chat list)
void msg_ui_init_compose(const char *contact_name, const char *message_text);
