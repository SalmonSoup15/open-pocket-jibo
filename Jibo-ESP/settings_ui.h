#pragma once
#include <stdint.h>

void     sett_init();           // build main settings page
void     sett_tick();           // per-frame update (page transitions, WiFi status polling, etc.)
void     sett_destroy_all();    // tear down everything (settings containers, portal, nullify pointers)
void     sett_btn_press();      // boot button back-navigation handler
void     sett_fade_out(uint32_t duration);  // fade out visible settings containers

uint32_t sett_sleep_timeout_seconds(uint8_t idx);
uint8_t  sett_num_sleep_opts();
