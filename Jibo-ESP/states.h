#pragma once
#include <lvgl.h>

enum JiboState {
    STATE_BOOT,
    STATE_TAP_TO_BEGIN,
    STATE_SETUP_CHOICE,   // "Continue setup on phone?" prompt
    STATE_PHONE_SETUP,    // BLE pairing + phone-driven config
    STATE_WIFI_SETUP,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_IDLE,
    STATE_EYE_TO_UI,
    STATE_UI_HOME,
    STATE_SETTINGS,
    STATE_UI_TO_EYE,
    STATE_LISTENING,
    STATE_THINKING,
    STATE_SPEAKING,
    STATE_TOOL_DISPLAY,
    STATE_AUDIO_DEBUG,
    STATE_SLEEP,
    STATE_POWER_OFF_SLIDE,
    STATE_PHONE_FINDER,
    STATE_IMU_DEBUG,
    STATE_DEV_TRANSITION     // slide-to-toggle developer mode
};

void states_init();
void states_tick();

JiboState states_current();

// ─── State accessors (for extracted UI modules) ───────────────────────────
void     enter_state(JiboState s);
uint32_t states_sub_step();
void     states_set_sub_step(uint32_t s);
uint32_t states_enter_ms();
void     states_set_enter_ms(uint32_t ms);
void     states_reset_activity();

void states_enter_audio_debug();
void states_exit_audio_debug();

void states_enter_imu_debug();
void states_exit_imu_debug();

void states_pwr_short();
void states_pwr_long();
