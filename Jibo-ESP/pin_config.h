#pragma once

// --- Power Management ---
#define XPOWERS_CHIP_AXP2101

// --- LCD (CO5300 via QSPI) ---
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    38
#define LCD_CS      12
#define LCD_RESET   2
#define LCD_WIDTH   466
#define LCD_HEIGHT  466

// --- Touch (CST9217 via I2C) ---
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RESET    2
#define TP_I2C_ADDR 0x5A

// --- Audio (ES8311 codec via I2S + I2C) ---
#define I2S_MCLK    16
#define I2S_BCLK    9
#define I2S_WS      45
#define I2S_SDOUT   8    // ESP32 output → ES8311 SDIN (playback)
#define I2S_SDIN    10   // ES8311 SDOUT → ESP32 input (recording)
#define AUDIO_PA    46   // power amplifier enable

// --- Boot button ---
#define BOOT_BTN    0