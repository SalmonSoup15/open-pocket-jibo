/*
 * Jibo Clone Firmware
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466, CO5300 QSPI, CST9217 touch)
 *
 * Required libraries:
 *   - GFX Library for Arduino  (CO5300 QSPI display driver)
 *   - SensorLib v0.3.1+        (CST9217 touch driver)
 *   - lvgl v8.4.0              (graphics framework)
 *   - XPowersLib v0.2.6        (AXP2101 PMIC)
 *
 * Board: ESP32S3 Dev Module
 * Flash: 16MB, PSRAM: OPI 8MB, USB CDC On Boot: Enabled
 */

#include <Wire.h>
#include "mbedtls/base64.h"
#include <esp_system.h>
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "XPowersLib.h"
#include "TouchDrvCSTXXX.hpp"
#include "lv_conf.h"
#include <lvgl.h>

#include "storage.h"
#include "eye.h"
#include "states.h"
#include "wifi_portal.h"
#include "audio.h"
#include "gemini.h"
#include "ble_link.h"
#include "notifications.h"
#include "pill_overlay.h"
#include "time_sync.h"
#include "dev_console.h"
#include "imu.h"
#include "crash_handler.h"
#include "event_log.h"
#include "dev_overlay.h"
#include "log.h"

volatile uint8_t jibo_log_level = 2;  // default: normal

// Persist reset reason across soft resets so we can read it even if serial
// connects late.  RTC memory survives panic/watchdog resets.
RTC_DATA_ATTR int saved_reset_reason = -1;

// ─── PMIC ───────────────────────────────────────────────────────────────────
XPowersPMU pmu;

// ─── Display ────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* no hw rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// ─── Touch ──────────────────────────────────────────────────────────────────
TouchDrvCST92xx touch;
static volatile bool touchIRQ = false;
int16_t tx[5], ty[5];

// ─── LVGL plumbing ─────────────────────────────────────────────────────────
#define LVGL_TICK_MS 2

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;

static void rounder_cb(lv_disp_drv_t *, lv_area_t *a) {
    if (a->x1 & 1) a->x1--;
    if (a->y1 & 1) a->y1--;
    if (!(a->x2 & 1)) a->x2++;
    if (!(a->y2 & 1)) a->y2++;
}

static lv_color_t *rot_buf = NULL;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    const int16_t S = LCD_WIDTH;
    int16_t dst_x = S - 1 - area->y2;
    int16_t dst_y = area->x1;
    uint32_t dst_w = h;
    uint32_t dst_h = w;

    uint16_t *src = (uint16_t *)px;
    uint16_t *dst = (uint16_t *)rot_buf;
    for (uint32_t sy = 0; sy < h; sy++) {
        uint32_t dx = h - 1 - sy;
        const uint16_t *srcRow = &src[sy * w];
        for (uint32_t sx = 0; sx < w; sx++) {
            dst[sx * dst_w + dx] = srcRow[sx];
        }
    }

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(dst_x, dst_y, dst, dst_w, dst_h);
#else
    gfx->draw16bitRGBBitmap(dst_x, dst_y, dst, dst_w, dst_h);
#endif
    lv_disp_flush_ready(drv);
}

static bool lastTouchState = false;
static lv_point_t lastTouchPt = {0, 0};
static uint32_t lastTouchMs = 0;
#define TOUCH_TIMEOUT_MS 50

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
    if (touchIRQ) {
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        touchIRQ = false;
        lastTouchMs = millis();
        if (n) {
            lastTouchPt.x = ty[0];
            lastTouchPt.y = LCD_HEIGHT - 1 - tx[0];
            lastTouchState = true;
        } else {
            lastTouchState = false;
        }
    } else if (lastTouchState && millis() - lastTouchMs > TOUCH_TIMEOUT_MS) {
        lastTouchState = false;
    }
    data->state = lastTouchState ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    if (lastTouchState) data->point = lastTouchPt;
}

static void tick_cb(void *) { lv_tick_inc(LVGL_TICK_MS); }

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    crash_handler_init();
    event_log_init();

    // Read dev mode from NVS early — we need storage_init() first, but
    // the NVS subsystem is light enough to call here.  However prefs
    // isn't open yet, so defer the g_dev_mode read to after storage_init().
    // For now, gate all pre-storage serial output behind a flag that will
    // be set after storage init.

    int cur_reason = (int)esp_reset_reason();
    saved_reset_reason = cur_reason;

    // --- I2C bus ---
    Wire.begin(IIC_SDA, IIC_SCL);

    // --- PMIC (AXP2101) - must be first, it powers the display ---
    if (pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        pmu.clearIrqStatus();
        pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
        pmu.setIrqLevelTime(XPOWERS_AXP2101_IRQ_TIME_2S);
        pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
        pmu.enableBattVoltageMeasure();
        pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
        pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
    }

    // --- Display - init ASAP so we get visual feedback ---
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // --- NVS storage ---
    storage_init();
    time_sync_init();
    dev_console_init();

    // Load dev mode flag into the global gate.  This must happen before
    // any LOG* call so the macros know whether to suppress output.
    extern bool g_dev_mode;
    g_dev_mode = storage_get_dev_mode();

    // Apply stored brightness
    uint8_t bl = storage_get_brightness();
    static const uint8_t blMap[] = {60, 140, 200};
    gfx->setBrightness(blMap[bl > 2 ? 2 : bl]);

    // In dev mode, open the serial port.  In stock mode the UART stays
    // completely uninitialised — nothing goes in or out.
    if (g_dev_mode) {
        Serial.begin(115200);
        delay(100);
        Serial.printf("\n\n=== Jibo Clone starting (DEV MODE) ===\n");
        Serial.printf("  ESP-IDF: %s\n", esp_get_idf_version());
        Serial.printf("  Reset reason: %d\n", cur_reason);
    }

    // --- Touch controller ---
    //
    // The CST9217 occasionally gets stuck driving SDA low after a
    // botched/interrupted I2C transaction (e.g. a panic mid-read), and
    // since the chip shares its RESET line with the display panel
    // (GPIO 2) the only way to recover post-boot is to pulse RESET
    // ourselves.  gfx->begin() above already does one pulse, but on
    // some boards the timing isn't quite enough — so do an explicit
    // strobe right before touch.begin() to guarantee a clean state.
    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    delay(10);
    digitalWrite(TP_RESET, HIGH);
    delay(50);                       // CST9217 needs ~30 ms post-reset

    touch.setPins(-1, TP_INT);
    if (touch.begin(Wire, TP_I2C_ADDR, IIC_SDA, IIC_SCL)) {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setMirrorXY(true, true);
        attachInterrupt(TP_INT, []() { touchIRQ = true; }, FALLING);
    }

    // --- LVGL ---
    lv_init();

    uint32_t bufPixels = LCD_WIDTH * LCD_HEIGHT;
    lv_color_t *buf1 = (lv_color_t *)ps_malloc(bufPixels * sizeof(lv_color_t));
    lv_color_t *buf2 = (lv_color_t *)ps_malloc(bufPixels * sizeof(lv_color_t));
    rot_buf = (lv_color_t *)ps_malloc(bufPixels * sizeof(lv_color_t));
    if (!buf1 || !buf2 || !rot_buf) {
        gfx->fillScreen(0xF800);  // red screen = alloc failure
        while (true) delay(1000);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, bufPixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = LCD_WIDTH;
    disp_drv.ver_res      = LCD_HEIGHT;
    disp_drv.flush_cb     = flush_cb;
    disp_drv.rounder_cb   = rounder_cb;
    disp_drv.draw_buf     = &draw_buf;
    disp_drv.full_refresh = 0;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t args = { .callback = tick_cb, .name = "lv_tick" };
    esp_timer_handle_t tmr = NULL;
    esp_timer_create(&args, &tmr);
    esp_timer_start_periodic(tmr, LVGL_TICK_MS * 1000);

    // --- Boot button ---
    pinMode(BOOT_BTN, INPUT_PULLUP);

    // --- Crash screen (if previous boot crashed) ---
    if (crash_handler_has_crash()) {
        crash_handler_show(BOOT_BTN);
    }

    // --- Eye & boot screen (show first frame before heavy inits) ---
    eye_init();
    states_init();
    lv_timer_handler();

    // --- Verbose boot label (dev mode only) ---
    // Shows each subsystem init step on the boot screen so the developer
    // can see where the boot stalls if something goes wrong.
    lv_obj_t *vbootLbl = NULL;
    bool verboseBoot = g_dev_mode && storage_get_dev_verbose_boot();
    if (verboseBoot) {
        vbootLbl = lv_label_create(lv_scr_act());
        lv_label_set_text(vbootLbl, "");
        lv_obj_set_style_text_color(vbootLbl, lv_color_make(0, 180, 0), 0);
        lv_obj_set_style_text_font(vbootLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(vbootLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(vbootLbl, LCD_WIDTH - 100);
        lv_label_set_long_mode(vbootLbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(vbootLbl, LV_ALIGN_BOTTOM_MID, 0, -70);
    }
    #define VBOOT_STEP(msg) do { \
        if (vbootLbl) { lv_label_set_text(vbootLbl, msg); lv_timer_handler(); } \
    } while(0)

    // --- Audio (ES8311 codec + I2S) ---
    VBOOT_STEP("Audio init...");
    audio_init();
    lv_timer_handler();

    // --- BLE ---
    VBOOT_STEP("BLE init...");
    ble_init();
    lv_timer_handler();

    VBOOT_STEP("Notifications init...");
    notifications_init();

    // --- Gemini API ---
    VBOOT_STEP("Gemini init...");
    gemini_init();

    // Install custom mbedtls allocator that routes its big (>= 4 KB) record
    // buffers to PSRAM.  This is the actual fix for SSL_ALLOC_FAILED — the
    // baseline largest contiguous internal-DRAM block on this board is only
    // ~26-32 KB after all subsystems init, but mbedtls needs 32-36 KB
    // contiguous for its rx/tx buffers and there's no way to defragment
    // ESP-IDF's heap at runtime.  Routing to PSRAM (we have 4+ MB free)
    // sidesteps the contiguous-DRAM problem entirely.  Safe to call before
    // WiFi.begin() since this only configures the allocator; no TLS yet.
    VBOOT_STEP("TLS allocator...");
    gemini_install_mbedtls_psram_alloc();

    // Clean up verbose boot label
    if (vbootLbl) { lv_obj_del(vbootLbl); vbootLbl = NULL; }
    #undef VBOOT_STEP

    dev_overlay_init();

    event_log_printf(EVT_CUSTOM, "setup done h=%u ps=%u",
                     ESP.getFreeHeap(), ESP.getFreePsram());

    if (g_dev_mode) {
        Serial.printf("=== Setup complete === free heap: %u, PSRAM free: %u\n\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
    }
}

// ─── Display brightness (called from settings) ─────────────────────────────
void set_display_brightness(uint8_t level) {
    static const uint8_t blMap[] = {60, 140, 200};
    gfx->setBrightness(blMap[level > 2 ? 2 : level]);
}

void set_display_brightness_raw(uint8_t value) {
    gfx->setBrightness(value);
}

uint8_t get_display_brightness_mapped() {
    static const uint8_t blMap[] = {60, 140, 200};
    uint8_t level = storage_get_brightness();
    return blMap[level > 2 ? 2 : level];
}

// ─── PMU helpers (called from states.cpp) ───────────────────────────────────

int pmu_battery_percent() {
    return pmu.getBatteryPercent();
}

bool pmu_is_charging() {
    return pmu.isCharging();
}

void pmu_shutdown() {
    pmu.shutdown();
}

// Brightness 0 alone leaves the AMOLED driver IC fully clocked at
// ~10 mA.  Sending SLPIN (via Arduino_GFX displayOff()) parks the panel
// at ~50 µA — which is the single biggest sleep-mode win on this board.
// We do both: brightness=0 first so the screen blanks visually before
// the panel goes through its 10-100 ms SLPIN delay.
void display_off() {
    gfx->setBrightness(0);
    gfx->displayOff();
}

void display_on() {
    gfx->displayOn();
    uint8_t bl = storage_get_brightness();
    static const uint8_t blMap[] = {60, 140, 200};
    gfx->setBrightness(blMap[bl > 2 ? 2 : bl]);
}

// ─── Light sleep helpers ────────────────────────────────────────────────────
//
// Drop the CPU into ESP32-S3 light sleep for up to maxMs.  Light sleep
// retains all peripheral state and RAM, so wake is essentially instant
// — no boot overhead, no re-init.  CPU draw goes from ~80 mA active to
// ~250 µA for the duration.
//
// We can't ext-wake on the AXP2101 power key (its IRQ pin isn't wired
// to a known RTC GPIO on this board — it's reported over I2C only), so
// the polling approach is: short timer wake → main loop runs poll_pmu()
// → if a power-key press happened, states_pwr_short() handles it.  At
// 50 ms intervals the worst-case wake latency is one tick (~50 ms),
// well below the ~200 ms human reaction-time floor for "felt as
// instant," and the average is half that.

void power_light_sleep_for(uint32_t maxMs) {
    // When a USB host has the CDC port open (i.e. someone is debugging
    // over the cable), light-sleeping kills the USB connection: the
    // controller stops sending SOF tokens, the host drops enumeration,
    // and after wake `Serial` won't print anything until you replug.
    // For dev convenience, skip the actual sleep when the host is
    // attached -- just delay() so the loop still backs off.  Real
    // power savings only matter when running on battery / unplugged
    // anyway, so we lose nothing.
    if (g_dev_mode && Serial) {
        delay(maxMs);
        return;
    }

    // Make sure outgoing serial bytes are on the wire before the UART
    // clock gets gated; otherwise the in-flight characters appear as
    // garbage on the host side.
    if (g_dev_mode) Serial.flush();

    esp_sleep_enable_timer_wakeup((uint64_t)maxMs * 1000);
    esp_light_sleep_start();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    // The WDT hardware timer keeps counting during light sleep while the
    // CPU is gated and can't feed it.  Reset it immediately on wake to
    // prevent accumulated sleep time from tripping the watchdog.
    esp_task_wdt_reset();
}

void pmu_clear_irq() {
    pmu.getIrqStatus();
    pmu.clearIrqStatus();
}

bool pmu_check_key_press() {
    pmu.getIrqStatus();
    bool pressed = pmu.isPekeyShortPressIrq() || pmu.isPekeyLongPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

// ─── PMU power key polling ──────────────────────────────────────────────────
static uint32_t lastPmuPollMs = 0;

static uint32_t lastPmuDebugMs = 0;

static void poll_pmu() {
    if (millis() - lastPmuPollMs < 50) return;
    lastPmuPollMs = millis();

    pmu.getIrqStatus();
    bool shortPress = pmu.isPekeyShortPressIrq();
    bool longPress  = pmu.isPekeyLongPressIrq();
    pmu.clearIrqStatus();

    if (shortPress || longPress) {
        LOG2("[pmu] press detected: short=%d long=%d\n", shortPress, longPress);
    }

    if (longPress)  { states_pwr_long();  return; }
    if (shortPress) { states_pwr_short(); }
}

// ─── Task manager dump ──────────────────────────────────────────────────────
// Two-snapshot CPU% measurement: take a system snapshot, sleep 500ms, take
// another, then per-task CPU% = (deltaRunTime / deltaTotalRunTime) * 100.
// In ESP-IDF SMP FreeRTOS the total runtime accumulates on both cores, so the
// percentages naturally sum to ~100% across the whole system.
static const char *task_state_str(eTaskState st) {
    switch (st) {
        case eRunning:   return "RUN";
        case eReady:     return "RDY";
        case eBlocked:   return "BLK";
        case eSuspended: return "SUS";
        case eDeleted:   return "DEL";
        default:         return "?";
    }
}

static void print_task_manager() {
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;

    TaskStatus_t *s1 = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * cap);
    TaskStatus_t *s2 = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * cap);
    if (!s1 || !s2) {
        if (s1) vPortFree(s1);
        if (s2) vPortFree(s2);
        dev_console_println("[taskmanager] alloc failed");
        return;
    }

    uint32_t total1 = 0, total2 = 0;
    UBaseType_t n1 = uxTaskGetSystemState(s1, cap, &total1);
    vTaskDelay(pdMS_TO_TICKS(500));
    UBaseType_t n2 = uxTaskGetSystemState(s2, cap, &total2);

    uint32_t totalDelta = total2 - total1;

    dev_console_println("");
    dev_console_println("==================== JIBO TASK MANAGER ====================");
    dev_console_println("");

    // ---- Heap summary ----
    size_t intFree    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t intLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t intMin     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t intTotal   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psFree     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psLargest  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t psMin      = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    size_t psTotal    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    dev_console_println("MEMORY:");
    dev_console_printf("  Internal DRAM : %6u / %6u free  (largest=%u, min_ever=%u)\n",
                       (unsigned)intFree, (unsigned)intTotal,
                       (unsigned)intLargest, (unsigned)intMin);
    dev_console_printf("  PSRAM         : %7u / %7u free  (largest=%u, min_ever=%u)\n",
                       (unsigned)psFree, (unsigned)psTotal,
                       (unsigned)psLargest, (unsigned)psMin);
    dev_console_printf("  Sketch / Free : used=%u, free=%u, total=%u (flash size %u)\n",
                       (unsigned)ESP.getSketchSize(),
                       (unsigned)ESP.getFreeSketchSpace(),
                       (unsigned)(ESP.getSketchSize() + ESP.getFreeSketchSpace()),
                       (unsigned)ESP.getFlashChipSize());
    dev_console_println("");

    // ---- CPU summary ----
    dev_console_printf("CPU: SDK %s, %u tasks, sample window=500ms, totalRunTime delta=%u\n",
                       ESP.getSdkVersion(), (unsigned)n2, (unsigned)totalDelta);
    dev_console_println("");

    // ---- Per-task table ----
    // Columns:
    //   PRI  = current dynamic priority
    //   CORE = pinned core (0/1) or ANY for unpinned
    //   STATE = RUN/RDY/BLK/SUS
    //   CPU%  = share of total run-time during the 500ms window
    //   STK_FREE = bytes of stack remaining at high-water mark (smaller=worse)
    //   STK_BASE = stack base address (D for internal DRAM, P for PSRAM)
    dev_console_println("TASKS:");
    dev_console_println("  PRI  CORE  STATE  CPU%    STK_FREE  STK_MEM  NAME");
    dev_console_println("  ---  ----  -----  ------  --------  -------  ------------------------");

    for (UBaseType_t i = 0; i < n2; i++) {
        TaskStatus_t &t = s2[i];

        uint32_t prevRun = 0;
        bool found = false;
        for (UBaseType_t j = 0; j < n1; j++) {
            if (s1[j].xHandle == t.xHandle) {
                prevRun = s1[j].ulRunTimeCounter;
                found = true;
                break;
            }
        }
        uint32_t deltaRun = t.ulRunTimeCounter - prevRun;
        float cpuPct = (totalDelta > 0)
            ? (100.0f * (float)deltaRun / (float)totalDelta)
            : 0.0f;

        // Core affinity: ESP-IDF helper.  Returns tskNO_AFFINITY (0x7FFFFFFF)
        // for tasks that can run on either core, else the pinned core ID.
        char coreStr[8];
        BaseType_t cid = xTaskGetCoreID(t.xHandle);
        if (cid == tskNO_AFFINITY) strcpy(coreStr, "ANY");
        else snprintf(coreStr, sizeof(coreStr), "%d", (int)cid);

        // usStackHighWaterMark is in stack words (4 bytes on Xtensa).
        uint32_t stackFreeBytes = (uint32_t)t.usStackHighWaterMark * sizeof(StackType_t);

        // Where does the task's stack live?  pxStackBase is the lowest stack
        // address.  ESP-IDF marks PSRAM mappings starting at 0x3F800000 and
        // 0x3C000000 on S3; internal SRAM is 0x3FC00000-0x3FCxxxxx.  Cheap
        // discriminator: if the address is in the SPIRAM range it's "P".
        const char *stkMem = "?";
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32)
        uintptr_t sb = (uintptr_t)t.pxStackBase;
        if (sb >= 0x3C000000 && sb < 0x3E000000) stkMem = "PSRAM";
        else if (sb >= 0x3FC00000 && sb < 0x3FD00000) stkMem = "DRAM";
        else if (sb >= 0x3FF80000 && sb < 0x40000000) stkMem = "RTC";
        else stkMem = "DRAM";
#endif

        dev_console_printf("  %3u  %-4s  %-5s  %5.1f%%  %8u  %-7s  %s%s\n",
                           (unsigned)t.uxCurrentPriority,
                           coreStr,
                           task_state_str(t.eCurrentState),
                           cpuPct,
                           (unsigned)stackFreeBytes,
                           stkMem,
                           t.pcTaskName,
                           found ? "" : " (NEW)");
    }

    dev_console_println("");
    dev_console_println("Notes:");
    dev_console_println("  - STK_FREE is high-water-mark (smallest free stack ever observed).");
    dev_console_println("    Values <512 bytes are dangerously close to overflow.");
    dev_console_println("  - CPU% is share of total runtime across BOTH cores combined,");
    dev_console_println("    so all tasks (incl. IDLE0+IDLE1) sum to ~100%.");
    dev_console_println("  - PSRAM is shared (not per-task); FreeRTOS doesn't tag heap");
    dev_console_println("    allocations by owner, so per-task PSRAM/DRAM use isn't tracked.");
    dev_console_println("===========================================================");
    dev_console_println("");

    vPortFree(s1);
    vPortFree(s2);
}

// ─── Serial command handler ─────────────────────────────────────────────────
//
// Pulled out of loop() so it can be invoked from two places:
//   1. The USB serial path in loop() below.
//   2. The BLE dev-console path (BLE_OP_DEV_CMD) — see dev_console.cpp.
//
// All output goes through dev_console_printf/println so when developer
// mode is enabled the phone receives the same response over BLE.
void serial_handle_command(const String &cmdIn) {
    String cmd = cmdIn;
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd == "help" || cmd == "?") {
        dev_console_println("Available commands:");
        dev_console_println("  help / ?              Show this list");
        dev_console_println("  reset                 Factory-reset NVS and reboot");
        dev_console_println("  audiodebug            Enter audio level debug screen");
        dev_console_println("  imudebug              Show 3D IMU orientation on display");
        dev_console_println("  imucal                Calibrate IMU (keep device still)");
        dev_console_println("  proxydebug            Enable BLE proxy debug on phone");
        dev_console_println("  debugexit             Exit any debug overlay");
        dev_console_println("  ttskey <key>          Save Deepgram TTS key");
        dev_console_println("  ttstest [text]        Speak text via Deepgram");
        dev_console_println("  geminitest <text>     Send text query to Gemini");
        dev_console_println("  clearmem              Clear conversation history");
        dev_console_println("  memory                List permanent memories");
        dev_console_println("  pilltest <icon> <text> [subtext]<sub>  Test pill animation");
        dev_console_println("    icons: success | error | charging | lowbat | notif");
        dev_console_println("  notifdebug            Print notifications system-prompt block");
        dev_console_println("  notifsync             Request fresh notification snapshot from phone");
        dev_console_println("  taskmanager / tasks   Print FreeRTOS task table");
        dev_console_println("  log_level 0..3        Set log verbosity");
        dev_console_println("  testtone              Play 1kHz sine for 2s");
        dev_console_println("  export                Dump all config as a copy-pasteable string");
        dev_console_println("  import <string>       Restore config from an export string");
        dev_console_println("  crash [-n] <type>     Force a crash (-n = normal crash screen)");
        dev_console_println("  whathappened / wh     Dump event log (current boot)");
    } else if (cmd == "whathappened" || cmd == "wh") {
        char *report = event_log_dump_report();
        if (report) {
            Serial.print(report);
            free(report);
        } else {
            dev_console_println("(No event log data)");
        }
    } else if (cmd == "memory" || cmd == "mem") {
        uint16_t cnt = storage_perm_mem_count();
        size_t bytes = storage_perm_mem_total_bytes();
        dev_console_printf("[mem] %u entries (%u bytes)\n",
                           cnt, (unsigned)bytes);
        for (uint16_t i = 0; i < cnt; i++) {
            PermMemoryEntry e;
            if (!storage_perm_mem_get(i, e)) continue;
            dev_console_printf("  [%u] id=%016llx ts=%llu  %s\n",
                               i, (unsigned long long)e.id,
                               (unsigned long long)e.ts_ms,
                               e.text.c_str());
        }
    } else if (cmd.startsWith("pilltest")) {
        String rest = cmd.substring(8);
        rest.trim();
        int sp = rest.indexOf(' ');
        String iconStr = (sp < 0) ? rest : rest.substring(0, sp);
        String body    = (sp < 0) ? String("") : rest.substring(sp + 1);
        body.trim();
        iconStr.toLowerCase();

        PillIcon icon = PILL_ICON_SUCCESS;
        if      (iconStr == "success")  icon = PILL_ICON_SUCCESS;
        else if (iconStr == "error")    icon = PILL_ICON_ERROR;
        else if (iconStr == "charging") icon = PILL_ICON_CHARGING;
        else if (iconStr == "lowbat" || iconStr == "battery")
                                        icon = PILL_ICON_LOW_BAT;
        else if (iconStr == "notif" || iconStr == "notification" ||
                 iconStr == "bell")
                                        icon = PILL_ICON_NOTIF;
        else {
            body = rest;
            icon = PILL_ICON_SUCCESS;
        }

        String sub = "";
        int subIdx = body.indexOf("[subtext]");
        if (subIdx >= 0) {
            String main = body.substring(0, subIdx);
            main.trim();
            sub = body.substring(subIdx + 9);
            sub.trim();
            body = main;
        }
        if (body.length() == 0) body = "Test pill";
        dev_console_printf("[pill] showing test pill: icon=%s \"%s\"%s%s\n",
                           iconStr.c_str(), body.c_str(),
                           sub.length() ? " | sub: " : "",
                           sub.length() ? sub.c_str() : "");
        pill_show(icon, body.c_str(), sub.length() ? sub.c_str() : nullptr);
    } else if (cmd == "reset") {
        dev_console_println("Factory reset...");
        storage_factory_reset();
        delay(200);
        crash_handler_pre_restart();
        esp_restart();
    } else if (cmd == "audiodebug") {
        states_enter_audio_debug();
    } else if (cmd == "imudebug") {
        states_enter_imu_debug();
    } else if (cmd == "imucal") {
        dev_console_println("[imucal] Keep device flat and still...");
        bool wasReady = imu_is_ready();
        if (!wasReady) {
            if (!imu_init()) {
                dev_console_println("[imucal] IMU init failed");
                return;
            }
        }
        imu_cal_start();
        while (!imu_cal_tick()) {
            imu_poll();
            delay(5);
        }
        float bx, by, bz;
        storage_get_imu_cal(bx, by, bz);
        dev_console_printf("[imucal] Done. Bias: %.4f %.4f %.4f\n", bx, by, bz);
        if (!wasReady) imu_deinit();
    } else if (cmd == "proxydebug") {
        ble_set_proxy_debug(true);
        dev_console_println("Proxy debug ON — phone will show debug info and play received audio");
    } else if (cmd == "debugexit") {
        states_exit_audio_debug();
        states_exit_imu_debug();
        ble_set_proxy_debug(false);
    } else if (cmd.startsWith("ttskey ")) {
        String k = cmd.substring(7);
        k.trim();
        storage_set_tts_key(k);
        dev_console_printf("VoiceRSS key saved (%u chars)\n", k.length());
    } else if (cmd.startsWith("ttstest")) {
        String t = cmd.substring(7);
        t.trim();
        if (t.length() == 0) t = "Hello world!";
        gemini_test_tts(t);
    } else if (cmd.startsWith("geminitest ")) {
        String t = cmd.substring(11);
        t.trim();
        if (t.length() == 0) { dev_console_println("Usage: geminitest <text>"); }
        else {
            extern String storage_get_api_key();
            extern String storage_get_tts_key();
            String apiKey = storage_get_api_key();
            String ttsKey = storage_get_tts_key();
            if (apiKey.isEmpty()) { dev_console_println("[geminitest] no API key"); }
            else {
                dev_console_printf("[geminitest] sending: %s\n", t.c_str());
                gemini_send_text(t, apiKey, ttsKey);
            }
        }
    } else if (cmd == "clearmem") {
        storage_clear_conv_history();
        gemini_clear_volatile_turn();
        dev_console_println("Conversation history cleared");
    } else if (cmd == "notifdebug") {
        dev_console_println("---- Notifications block ----");
        dev_console_println(notifications_format_for_prompt().c_str());
        dev_console_printf("---- count=%u  ble_connected=%d  ble_raw=%d ----\n",
                           (unsigned)notifications_count(),
                           (int)ble_is_connected(),
                           (int)ble_device_connected_raw());
    } else if (cmd == "notifsync") {
        ble_request_notif_snapshot();
        dev_console_println("Requested notification snapshot from phone");
    } else if (cmd == "taskmanager" || cmd == "tasks" || cmd == "top") {
        print_task_manager();
    } else if (cmd.startsWith("log_level ")) {
        int lv = cmd.substring(10).toInt();
        if (lv >= 0 && lv <= 3) {
            jibo_log_level = lv;
            dev_console_printf("Log level set to %d\n", lv);
        } else {
            dev_console_println("Usage: log_level [0|1|2|3]");
        }
    } else if (cmd.startsWith("crash")) {
        String arg = cmd.substring(5);
        arg.trim();
        // -n flag: force normal (stock) crash screen even in dev mode
        bool forceNormal = false;
        if (arg.startsWith("-n")) {
            forceNormal = true;
            arg = arg.substring(2);
            arg.trim();
        }
        if (forceNormal) crash_handler_force_stock(true);
        if (arg.length() == 0 || arg == "help") {
            dev_console_println("Usage: crash [-n] <type>");
            dev_console_println("  -n              Show normal (stock) crash screen");
            dev_console_println("  nullptr         Dereference a null pointer");
            dev_console_println("  divzero         Integer divide by zero");
            dev_console_println("  stackoverflow   Blow the stack with deep recursion");
            dev_console_println("  wdt             Spin forever (task watchdog)");
            dev_console_println("  abort           Call abort() directly");
            dev_console_println("  oom             Exhaust heap until alloc fails");
            dev_console_println("  reboot          Soft reboot (no crash, control test)");
            if (forceNormal) crash_handler_force_stock(false);  // reset if just help
        } else if (arg == "nullptr") {
            dev_console_println("Dereferencing null pointer...");
            Serial.flush(); delay(50);
            volatile int *p = NULL;
            int x = *p;
            (void)x;
        } else if (arg == "divzero") {
            dev_console_println("Dividing by zero...");
            Serial.flush(); delay(50);
            volatile int a = 1, b = 0;
            volatile int c = a / b;
            (void)c;
        } else if (arg == "stackoverflow") {
            dev_console_println("Blowing the stack...");
            Serial.flush(); delay(50);
            volatile char buf[512];
            buf[0] = 1;
            serial_handle_command(String("crash stackoverflow"));
        } else if (arg == "wdt") {
            dev_console_println("Spinning forever (watchdog will fire)...");
            Serial.flush(); delay(50);
            while (true) { /* starve the task watchdog */ }
        } else if (arg == "abort") {
            dev_console_println("Calling abort()...");
            Serial.flush(); delay(50);
            abort();
        } else if (arg == "oom") {
            dev_console_println("Exhausting heap...");
            Serial.flush(); delay(50);
            while (true) {
                void *p = malloc(1024);
                if (!p) {
                    dev_console_println("Heap exhausted, triggering assert...");
                    Serial.flush(); delay(50);
                    assert(p != NULL);
                }
            }
        } else if (arg == "reboot") {
            dev_console_println("Clean reboot (should NOT trigger crash screen)...");
            Serial.flush(); delay(100);
            crash_handler_pre_restart();
            esp_restart();
        } else {
            dev_console_printf("Unknown crash type: %s (try 'crash help')\n", arg.c_str());
        }
    } else if (cmd == "export") {
        // Build newline-separated key=value config, then base64-encode
        String cfg = "V=1\n";

        // WiFi networks
        uint8_t wc = storage_wifi_count();
        cfg += "WC=" + String(wc) + "\n";
        for (uint8_t i = 0; i < wc; i++) {
            WifiEntry w;
            if (storage_get_wifi(i, w)) {
                String prefix = "W" + String(i);
                cfg += prefix + "S=" + w.ssid + "\n";
                cfg += prefix + "P=" + w.password + "\n";
                cfg += prefix + "U=" + w.username + "\n";
                cfg += prefix + "E=" + String(w.enterprise ? 1 : 0) + "\n";
            }
        }

        // API keys
        cfg += "AK=" + storage_get_api_key() + "\n";
        cfg += "TK=" + storage_get_tts_key() + "\n";

        // Settings
        cfg += "SD=" + String(storage_is_setup_done() ? 1 : 0) + "\n";
        cfg += "BR=" + String(storage_get_brightness()) + "\n";
        cfg += "MO=" + String(storage_get_model()) + "\n";
        cfg += "VO=" + String(storage_get_voice()) + "\n";
        cfg += "ME=" + String(storage_get_memory_enabled() ? 1 : 0) + "\n";
        cfg += "WE=" + String(storage_get_wifi_enabled() ? 1 : 0) + "\n";
        cfg += "ST=" + String(storage_get_sleep_timeout()) + "\n";

        // Base64 encode
        size_t srcLen = cfg.length();
        size_t b64Len = 0;
        mbedtls_base64_encode(NULL, 0, &b64Len, (const unsigned char *)cfg.c_str(), srcLen);
        char *b64 = (char *)ps_malloc(b64Len + 1);
        if (b64) {
            mbedtls_base64_encode((unsigned char *)b64, b64Len + 1, &b64Len,
                                  (const unsigned char *)cfg.c_str(), srcLen);
            b64[b64Len] = '\0';
            dev_console_printf("EXPORT:%s\n", b64);
            free(b64);
        } else {
            dev_console_println("[export] alloc failed");
        }

    } else if (cmd.startsWith("import ")) {
        String b64str = cmd.substring(7);
        b64str.trim();
        // Strip "EXPORT:" prefix if pasted with it
        if (b64str.startsWith("EXPORT:")) b64str = b64str.substring(7);
        if (b64str.length() == 0) {
            dev_console_println("Usage: import <base64_config_string>");
            dev_console_println("  Paste the string from 'export' command");
        } else {
            // Base64 decode
            size_t b64Len = b64str.length();
            size_t decLen = 0;
            int ret = mbedtls_base64_decode(NULL, 0, &decLen,
                         (const unsigned char *)b64str.c_str(), b64Len);
            if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
                dev_console_println("[import] invalid base64");
            } else {
                char *dec = (char *)ps_malloc(decLen + 1);
                if (!dec) { dev_console_println("[import] alloc failed"); }
                else {
                    ret = mbedtls_base64_decode((unsigned char *)dec, decLen + 1, &decLen,
                              (const unsigned char *)b64str.c_str(), b64Len);
                    if (ret != 0) {
                        dev_console_println("[import] decode failed");
                        free(dec);
                    } else {
                        dec[decLen] = '\0';
                        String cfg(dec);
                        free(dec);

                        // Parse key=value pairs (newline separated)
                        int wifiCount = 0;
                        WifiEntry wEntries[MAX_WIFI_NETWORKS] = {};
                        int applied = 0;

                        int pos = 0;
                        while (pos < (int)cfg.length()) {
                            int nl = cfg.indexOf('\n', pos);
                            if (nl < 0) nl = cfg.length();
                            String line = cfg.substring(pos, nl);
                            pos = nl + 1;

                            int eq = line.indexOf('=');
                            if (eq < 0) continue;
                            String key = line.substring(0, eq);
                            String val = line.substring(eq + 1);

                            if (key == "V") {
                                if (val != "1") {
                                    dev_console_printf("[import] unknown version: %s\n", val.c_str());
                                    break;
                                }
                            } else if (key == "WC") {
                                wifiCount = val.toInt();
                                if (wifiCount > MAX_WIFI_NETWORKS) wifiCount = MAX_WIFI_NETWORKS;
                            } else if (key.length() >= 3 && key[0] == 'W' && key[2] == 'S') {
                                int idx = key[1] - '0';
                                if (idx >= 0 && idx < MAX_WIFI_NETWORKS) wEntries[idx].ssid = val;
                            } else if (key.length() >= 3 && key[0] == 'W' && key[2] == 'P') {
                                int idx = key[1] - '0';
                                if (idx >= 0 && idx < MAX_WIFI_NETWORKS) wEntries[idx].password = val;
                            } else if (key.length() >= 3 && key[0] == 'W' && key[2] == 'U') {
                                int idx = key[1] - '0';
                                if (idx >= 0 && idx < MAX_WIFI_NETWORKS) wEntries[idx].username = val;
                            } else if (key.length() >= 3 && key[0] == 'W' && key[2] == 'E') {
                                int idx = key[1] - '0';
                                if (idx >= 0 && idx < MAX_WIFI_NETWORKS) wEntries[idx].enterprise = (val == "1");
                            } else if (key == "AK") {
                                storage_set_api_key(val);            applied++;
                            } else if (key == "TK") {
                                storage_set_tts_key(val);            applied++;
                            } else if (key == "SD") {
                                storage_set_setup_done(val == "1");   applied++;
                            } else if (key == "BR") {
                                storage_set_brightness(val.toInt());  applied++;
                            } else if (key == "MO") {
                                storage_set_model(val.toInt());       applied++;
                            } else if (key == "VO") {
                                storage_set_voice(val.toInt());       applied++;
                            } else if (key == "ME") {
                                storage_set_memory_enabled(val == "1"); applied++;
                            } else if (key == "WE") {
                                storage_set_wifi_enabled(val == "1");   applied++;
                            } else if (key == "ST") {
                                storage_set_sleep_timeout(val.toInt()); applied++;
                            }
                        }

                        // Write WiFi networks
                        if (wifiCount > 0) {
                            storage_set_wifi_count(wifiCount);
                            for (int i = 0; i < wifiCount; i++) {
                                storage_set_wifi(i, wEntries[i]);
                            }
                            applied += wifiCount;
                        }

                        // If we have WiFi + API key, mark setup as done so
                        // the device boots straight to idle instead of setup
                        if (wifiCount > 0 && storage_get_api_key().length() > 0) {
                            storage_set_setup_done(true);
                        }

                        dev_console_printf("[import] Done — %d settings applied. Rebooting...\n", applied);
                        Serial.flush();
                        delay(300);
                        crash_handler_pre_restart();
                        esp_restart();
                    }
                }
            }
        }

    } else if (cmd == "testtone") {
        dev_console_println("[test] Generating 1kHz sine wave (2s)...");
        const int sr = 16000;
        const int dur = 2;
        const int n = sr * dur;
        static int16_t *toneBuf = NULL;
        if (toneBuf) { audio_stop_play(); free(toneBuf); toneBuf = NULL; }
        toneBuf = (int16_t *)ps_malloc(n * sizeof(int16_t));
        if (toneBuf) {
            for (int i = 0; i < n; i++) {
                toneBuf[i] = (int16_t)(10000.0f * sinf(2.0f * 3.14159265f * 1000.0f * i / sr));
            }
            audio_play_pcm(toneBuf, n * 2);
            dev_console_println("[test] Playing...");
        } else {
            dev_console_println("[test] alloc failed");
        }
    } else {
        dev_console_printf("Unknown command: %s  (try 'help')\n", cmd.c_str());
    }
}

// ─── Loop ───────────────────────────────────────────────────────────────────
// Periodic memory snapshot for event log (every 30 s)
static uint32_t sNextMemSnapMs = 0;

void loop() {
    if (g_dev_mode && Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        serial_handle_command(cmd);
    }
    poll_pmu();
    states_tick();
    ble_link_tick();
    dev_overlay_tick();
    lv_timer_handler();

    // Log a memory snapshot every 30 seconds
    if (millis() >= sNextMemSnapMs) {
        event_log_printf(EVT_MEMORY_SNAP, "h=%u ps=%u",
                         ESP.getFreeHeap(), ESP.getFreePsram());
        sNextMemSnapMs = millis() + 30000;
    }

    delay(2);
}
