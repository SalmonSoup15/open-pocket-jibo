#include "crash_handler.h"
#include "event_log.h"
#include "pin_config.h"
#include "dev_console.h"   // g_dev_mode
#include <esp_system.h>
#include <esp_debug_helpers.h>
#include <xtensa/xtensa_api.h>
#include <lvgl.h>

// ─── RTC-resident crash record ─────────────────────────────────────────────
// RTC_NOINIT memory survives software resets (panic, watchdog, brownout)
// but is wiped on power-on.  We write context continuously so it's fresh
// whenever the crash actually happens.

#define CRASH_MAGIC      0xC7A5917E
#define CRASH_MSG_MAX    160
#define CRASH_BT_MAX     12
#define CRASH_REG_COUNT  24   // PC PS A0-A15 SAR EXCCAUSE EXCVADDR LBEG LEND LCOUNT

struct CrashRecord {
    uint32_t magic;
    uint32_t resetReason;
    uint32_t uptimeMs;
    uint32_t freeHeap;
    uint32_t freePsram;
    char     state[24];
    char     message[CRASH_MSG_MAX];
    uint32_t backtrace[CRASH_BT_MAX];
    uint8_t  btCount;
    uint8_t  forceStock;
    uint8_t  hasRegs;
    uint8_t  _pad;
    uint32_t regs[CRASH_REG_COUNT];
};

RTC_NOINIT_ATTR static CrashRecord cr;

// Frozen copy of the crash record from the previous boot, taken before
// we start overwriting cr with current-boot context.
static CrashRecord savedCrash;
static bool sHasCrash      = false;
static bool sForceStock    = false;

// ─── Exception handler for backtrace capture ──────────────────────────────
// Registers Xtensa exception handlers for common crash causes.  When an
// exception fires, our handler runs BEFORE the panic handler, with full
// access to the crashed context's registers.  We walk the backtrace from
// the exception frame and save it to the RTC crash record, then hand off
// to the normal panic flow.

static const int kCrashExcCauses[] = {
    0,   // IllegalInstruction
    2,   // InstrFetchError
    3,   // LoadStoreError
    6,   // IntegerDivideByZero
    9,   // LoadStoreAlignment
    20,  // InstrFetchProhibited
    28,  // LoadProhibited
    29,  // StoreProhibited
};

extern "C" void xt_unhandled_exception(XtExcFrame *frame);

static void IRAM_ATTR crash_exc_handler(XtExcFrame *frame) {
    // Save register dump
    cr.hasRegs  = 1;
    cr.regs[0]  = frame->pc;       cr.regs[1]  = frame->ps;
    cr.regs[2]  = frame->a0;       cr.regs[3]  = frame->a1;
    cr.regs[4]  = frame->a2;       cr.regs[5]  = frame->a3;
    cr.regs[6]  = frame->a4;       cr.regs[7]  = frame->a5;
    cr.regs[8]  = frame->a6;       cr.regs[9]  = frame->a7;
    cr.regs[10] = frame->a8;       cr.regs[11] = frame->a9;
    cr.regs[12] = frame->a10;      cr.regs[13] = frame->a11;
    cr.regs[14] = frame->a12;      cr.regs[15] = frame->a13;
    cr.regs[16] = frame->a14;      cr.regs[17] = frame->a15;
    cr.regs[18] = frame->sar;      cr.regs[19] = frame->exccause;
    cr.regs[20] = frame->excvaddr;  cr.regs[21] = frame->lbeg;
    cr.regs[22] = frame->lend;     cr.regs[23] = frame->lcount;

    // Save backtrace
    esp_backtrace_frame_t bt;
    bt.pc      = frame->pc;
    bt.sp      = frame->a1;
    bt.next_pc = frame->a0;
    cr.btCount = 0;
    for (int i = 0; i < CRASH_BT_MAX; i++) {
        cr.backtrace[i] = bt.pc;
        cr.btCount = i + 1;
        if (!esp_backtrace_get_next_frame(&bt)) break;
        if (bt.pc < 0x40000000) break;
    }
    xt_unhandled_exception(frame);
}

// ─── Continuous context update ──────────────────────────────────────────────

void crash_handler_update_state(const char *stateName) {
    cr.uptimeMs  = millis();
    cr.freeHeap  = ESP.getFreeHeap();
    cr.freePsram = ESP.getFreePsram();
    strncpy(cr.state, stateName, sizeof(cr.state) - 1);
    cr.state[sizeof(cr.state) - 1] = '\0';
}

// ─── Explicit crash capture ─────────────────────────────────────────────────

void crash_handler_save(const char *msg) {
    cr.magic       = CRASH_MAGIC;
    cr.resetReason = 0xFF;
    cr.uptimeMs    = millis();
    cr.freeHeap    = ESP.getFreeHeap();
    cr.freePsram   = ESP.getFreePsram();
    if (msg) {
        strncpy(cr.message, msg, CRASH_MSG_MAX - 1);
        cr.message[CRASH_MSG_MAX - 1] = '\0';
    } else {
        cr.message[0] = '\0';
    }

    cr.btCount = 0;
    esp_backtrace_frame_t frame;
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    for (int i = 0; i < CRASH_BT_MAX; i++) {
        cr.backtrace[i] = frame.pc;
        cr.btCount = i + 1;
        if (!esp_backtrace_get_next_frame(&frame)) break;
        if (frame.pc < 0x40000000) break;
    }
}

// ─── Shutdown handler (registered with ESP-IDF) ────────────────────────────
// Called by esp_restart() before the actual reset.  Stamps the crash record
// with fresh heap/uptime so it survives into the next boot.  For clean
// reboots, crash_handler_pre_restart() clears the magic *before* calling
// esp_restart(), so this handler still runs but the next boot ignores the
// record.  For panic/WDT resets this handler does NOT run — but the magic
// is already set and the continuously-updated fields are current.

static void shutdown_handler() {
    cr.uptimeMs  = millis();
    cr.freeHeap  = ESP.getFreeHeap();
    cr.freePsram = ESP.getFreePsram();
}

// ─── Pre-restart (call before intentional esp_restart) ─────────────────────

void crash_handler_pre_restart() {
    cr.magic = 0;   // next boot won't see a crash
}

// ─── Init (call early in setup) ────────────────────────────────────────────

static const char *reset_reason_msg(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC:    return "Software panic";
        case ESP_RST_INT_WDT:  return "Interrupt watchdog";
        case ESP_RST_TASK_WDT: return "Task watchdog";
        case ESP_RST_WDT:      return "Watchdog reset";
        case ESP_RST_BROWNOUT: return "Brownout (low voltage)";
        default:               return "Unknown reset";
    }
}

void crash_handler_init() {
    esp_reset_reason_t reason = esp_reset_reason();

    // ── Detect crash from previous boot ─────────────────────────────
    if (cr.magic == CRASH_MAGIC) {
        cr.resetReason = (uint32_t)reason;
        if (cr.message[0] == '\0') {
            snprintf(cr.message, CRASH_MSG_MAX, "%s", reset_reason_msg(reason));
        }
        savedCrash = cr;
        sHasCrash = true;
        sForceStock = (cr.forceStock == 1);
    } else if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
               reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
        cr.resetReason = (uint32_t)reason;
        snprintf(cr.message, CRASH_MSG_MAX, "%s (no RTC context)", reset_reason_msg(reason));
        cr.btCount     = 0;
        savedCrash = cr;
        sHasCrash = true;
        sForceStock = (cr.forceStock == 1);
    }

    // ── Set up new boot's crash record ──────────────────────────────
    cr.magic       = CRASH_MAGIC;
    strncpy(cr.state, "BOOT", sizeof(cr.state));
    cr.uptimeMs    = 0;
    cr.freeHeap    = ESP.getFreeHeap();
    cr.freePsram   = ESP.getFreePsram();
    cr.message[0]  = '\0';
    cr.btCount     = 0;
    cr.hasRegs     = 0;
    cr.forceStock  = 0;

    // ── Register exception handlers + shutdown handler ─────────────
    for (size_t i = 0; i < sizeof(kCrashExcCauses)/sizeof(kCrashExcCauses[0]); i++)
        xt_set_exception_handler(kCrashExcCauses[i], crash_exc_handler);

    esp_register_shutdown_handler(shutdown_handler);
}

bool crash_handler_has_crash() {
    return sHasCrash;
}

void crash_handler_force_stock(bool force) {
    sForceStock = force;
    cr.forceStock = force ? 1 : 0;   // persist in RTC memory across reboot
}

void crash_handler_clear() {
    sHasCrash = false;
    cr.magic       = CRASH_MAGIC;
    cr.message[0]  = '\0';
    cr.btCount     = 0;
    cr.hasRegs     = 0;
}

// ─── Crash screen (LVGL) ───────────────────────────────────────────────────
// Layout sized for a 466×466 circular AMOLED — keep text well inside the
// inscribed rectangle (~330 px wide) so nothing gets clipped by the bezel.

static const char *reason_str(uint32_t r) {
    switch ((esp_reset_reason_t)r) {
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
        case ESP_RST_WDT:      return "WATCHDOG";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        default:               return "UNKNOWN";
    }
}

// Simple numeric error code for stock-mode crash screen (like Windows STOP codes)
static int crash_error_code(uint32_t r) {
    switch ((esp_reset_reason_t)r) {
        case ESP_RST_PANIC:    return 1;
        case ESP_RST_INT_WDT:  return 2;
        case ESP_RST_TASK_WDT: return 3;
        case ESP_RST_WDT:      return 4;
        case ESP_RST_BROWNOUT: return 5;
        default:               return 0;
    }
}

// Shared: pump LVGL until user taps/presses button.
// In dev mode, also polls Serial for diagnostic commands:
//   whathappened — dump full event log + crash record
//   continue     — dismiss the crash screen
static void wait_for_input(uint8_t bootBtnPin) {
    extern bool pmu_check_key_press();
    lv_timer_handler();
    pmu_check_key_press();   // drain any stale IRQ flags
    delay(500);

    // Serial command buffer (dev mode only)
    char cmdBuf[32];
    uint8_t cmdLen = 0;

    while (true) {
        lv_timer_handler();
        if (digitalRead(bootBtnPin) == LOW) break;
        if (pmu_check_key_press()) break;
        lv_indev_t *indev = lv_indev_get_next(NULL);
        if (indev && indev->driver->read_cb) {
            lv_indev_data_t d;
            indev->driver->read_cb(indev->driver, &d);
            if (d.state == LV_INDEV_STATE_PR) break;
        }

        // ── Serial command handling (dev mode only) ───────────────────
        if (g_dev_mode && Serial && Serial.available()) {
            char ch = (char)Serial.read();
            if (ch == '\n' || ch == '\r') {
                if (cmdLen > 0) {
                    cmdBuf[cmdLen] = '\0';

                    if (strcmp(cmdBuf, "whathappened") == 0 ||
                        strcmp(cmdBuf, "wh") == 0) {
                        // ── Dump event log ────────────────────────────
                        char *report = event_log_dump_report();
                        if (report) {
                            Serial.print(report);
                            free(report);
                        } else {
                            Serial.println("(No event log data)\n");
                        }
                        // ── Dump crash record ─────────────────────────
                        Serial.println("--- CRASH RECORD ---");
                        Serial.printf("Reason : %s\n",
                                      reason_str(savedCrash.resetReason));
                        Serial.printf("State  : %s\n", savedCrash.state);
                        Serial.printf("Uptime : %lu ms\n",
                                      (unsigned long)savedCrash.uptimeMs);
                        Serial.printf("Heap   : %lu  PSRAM: %lu\n",
                                      (unsigned long)savedCrash.freeHeap,
                                      (unsigned long)savedCrash.freePsram);
                        if (savedCrash.message[0]) {
                            Serial.printf("Msg    : %s\n",
                                          savedCrash.message);
                        }
                        if (savedCrash.btCount > 0) {
                            Serial.println("Backtrace:");
                            for (int i = 0; i < savedCrash.btCount; i++) {
                                Serial.printf("  0x%08lx\n",
                                    (unsigned long)savedCrash.backtrace[i]);
                            }
                        }
                        if (savedCrash.hasRegs == 1) {
                            const uint32_t *r = savedCrash.regs;
                            Serial.println("Register dump:");
                            Serial.printf("  PC      : 0x%08lx  PS      : 0x%08lx\n", (unsigned long)r[0],  (unsigned long)r[1]);
                            Serial.printf("  A0      : 0x%08lx  A1      : 0x%08lx\n", (unsigned long)r[2],  (unsigned long)r[3]);
                            Serial.printf("  A2      : 0x%08lx  A3      : 0x%08lx\n", (unsigned long)r[4],  (unsigned long)r[5]);
                            Serial.printf("  A4      : 0x%08lx  A5      : 0x%08lx\n", (unsigned long)r[6],  (unsigned long)r[7]);
                            Serial.printf("  A6      : 0x%08lx  A7      : 0x%08lx\n", (unsigned long)r[8],  (unsigned long)r[9]);
                            Serial.printf("  A8      : 0x%08lx  A9      : 0x%08lx\n", (unsigned long)r[10], (unsigned long)r[11]);
                            Serial.printf("  A10     : 0x%08lx  A11     : 0x%08lx\n", (unsigned long)r[12], (unsigned long)r[13]);
                            Serial.printf("  A12     : 0x%08lx  A13     : 0x%08lx\n", (unsigned long)r[14], (unsigned long)r[15]);
                            Serial.printf("  A14     : 0x%08lx  A15     : 0x%08lx\n", (unsigned long)r[16], (unsigned long)r[17]);
                            Serial.printf("  SAR     : 0x%08lx  EXCCAUSE: 0x%08lx\n", (unsigned long)r[18], (unsigned long)r[19]);
                            Serial.printf("  EXCVADDR: 0x%08lx  LBEG    : 0x%08lx\n", (unsigned long)r[20], (unsigned long)r[21]);
                            Serial.printf("  LEND    : 0x%08lx  LCOUNT  : 0x%08lx\n", (unsigned long)r[22], (unsigned long)r[23]);
                        }
                        Serial.println("--- END ---\n");
                    } else if (strcmp(cmdBuf, "continue") == 0) {
                        break;   // dismiss crash screen via serial
                    } else {
                        Serial.printf("? '%s'\n", cmdBuf);
                        Serial.println("Commands: whathappened  continue");
                    }
                    cmdLen = 0;
                }
            } else if (cmdLen < sizeof(cmdBuf) - 1) {
                cmdBuf[cmdLen++] = ch;
            }
        }

        delay(10);
    }
    while (digitalRead(bootBtnPin) == LOW) delay(10);
    delay(50);
}

// ─── Stock-mode crash screen ───────────────────────────────────────────────
// Friendly, non-technical — like a Windows BSOD for normal users.
static void crash_show_stock(uint8_t bootBtnPin) {
    const CrashRecord &c = savedCrash;

    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Sad face
    lv_obj_t *face = lv_label_create(scr);
    lv_label_set_text(face, ":(");
    lv_obj_set_style_text_color(face, lv_color_white(), 0);
    lv_obj_set_style_text_font(face, &lv_font_montserrat_48, 0);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, -80);

    // Friendly message
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Jibo ran into an issue\nand needed to restart.");
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 300);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    // Error code
    char codeBuf[24];
    snprintf(codeBuf, sizeof(codeBuf), "Error code: %02d",
             crash_error_code(c.resetReason));
    lv_obj_t *code = lv_label_create(scr);
    lv_label_set_text(code, codeBuf);
    lv_obj_set_style_text_color(code, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(code, &lv_font_montserrat_14, 0);
    lv_obj_align(code, LV_ALIGN_CENTER, 0, 55);

    // Preservation status (subtle, for advanced users)
    uint8_t pstat = event_log_preservation_status();
    if (pstat > 0) {
        lv_obj_t *pres = lv_label_create(scr);
        lv_label_set_text(pres, pstat == 1
            ? "Crash data saved"
            : "Crash data partially saved");
        lv_obj_set_style_text_color(pres, lv_color_make(60, 60, 60), 0);
        lv_obj_set_style_text_font(pres, &lv_font_montserrat_12, 0);
        lv_obj_align(pres, LV_ALIGN_CENTER, 0, 80);
    }

    // Footer
    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, "Tap to continue");
    lv_obj_set_style_text_color(footer, lv_color_make(80, 80, 80), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -70);

    wait_for_input(bootBtnPin);
    lv_obj_del(scr);
    lv_timer_handler();
    // Clear event log along with crash record
    event_log_clear();
    crash_handler_clear();
}

// ─── Dev-mode crash screen ─────────────────────────────────────────────────
// Full technical detail: reason, state, heap, backtrace.
static void crash_show_dev(uint8_t bootBtnPin) {
    const CrashRecord &c = savedCrash;
    const int16_t TEXT_W = 300;

    lv_obj_t *scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_make(15, 0, 0), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "CRASH REPORT");
    lv_obj_set_style_text_color(title, lv_color_make(255, 60, 60), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 65);

    char detail[600];
    int pos = 0;
    pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "Reason: %s\n", reason_str(c.resetReason));
    pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "State: %s\n", c.state);
    pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "Uptime: %lus\n", (unsigned long)(c.uptimeMs / 1000));
    pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "Heap: %lu  PSRAM: %lu\n",
                    (unsigned long)c.freeHeap,
                    (unsigned long)c.freePsram);
    if (c.message[0]) {
        pos += snprintf(detail + pos, sizeof(detail) - pos,
                        "\n%s\n", c.message);
    }
    if (c.btCount > 0) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "\nBacktrace:\n");
        for (int i = 0; i < c.btCount && pos < (int)sizeof(detail) - 16; i++) {
            pos += snprintf(detail + pos, sizeof(detail) - pos,
                            " 0x%08lx\n", (unsigned long)c.backtrace[i]);
        }
    }

    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_text(body, detail);
    lv_obj_set_style_text_color(body, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_12, 0);
    lv_obj_set_width(body, TEXT_W);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 100);

    // ── Event log preservation status ────────────────────────────────
    uint8_t pstat = event_log_preservation_status();
    uint16_t ecount = event_log_count();
    char preserveBuf[80];
    if (pstat == 1)
        snprintf(preserveBuf, sizeof(preserveBuf),
                 "Event log: %u events preserved", ecount);
    else if (pstat == 2)
        snprintf(preserveBuf, sizeof(preserveBuf),
                 "Event log: %u events (overflow)", ecount);
    else
        snprintf(preserveBuf, sizeof(preserveBuf),
                 "Event log: no data");

    lv_obj_t *presLabel = lv_label_create(scr);
    lv_label_set_text(presLabel, preserveBuf);
    lv_obj_set_style_text_color(presLabel,
        pstat > 0 ? lv_color_make(80, 200, 80)
                  : lv_color_make(120, 120, 120),
        0);
    lv_obj_set_style_text_font(presLabel, &lv_font_montserrat_12, 0);
    lv_obj_align(presLabel, LV_ALIGN_BOTTOM_MID, 0, -90);

    // Footer with serial hint
    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, "Tap to continue\nSerial: whathappened");
    lv_obj_set_style_text_color(footer, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -60);

    wait_for_input(bootBtnPin);
    lv_obj_del(scr);
    lv_timer_handler();
    // Clear event log along with crash record
    event_log_clear();
    crash_handler_clear();
}

void crash_handler_show(uint8_t bootBtnPin) {
    if (!sHasCrash) return;
    if (g_dev_mode && !sForceStock) {
        crash_show_dev(bootBtnPin);
    } else {
        crash_show_stock(bootBtnPin);
    }
    sForceStock = false;   // auto-reset after showing
}
