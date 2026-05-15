#include "event_log.h"
#include "crash_handler.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// ──────────────────────────────────────────────────────────────────────────
//  Event Log — RTC_NOINIT ring buffer implementation
//
//  100 events x 64 bytes = 6400 bytes + 32-byte header = 6432 bytes total.
//  Combined with CrashRecord (~260 bytes), total RTC_NOINIT usage is
//  ~6692 bytes — well within the ESP32-S3's ~8 KB RTC FAST limit.
// ──────────────────────────────────────────────────────────────────────────

#define EVLOG_MAGIC      0xEF10CAFE
#define EVLOG_MAX_EVENTS 100
#define EVLOG_MSG_MAX    58

// ─── Ring buffer entry ─────────────────────────────────────────────────────

struct EventEntry {
    uint32_t timestamp_ms;      // millis() at log time
    uint8_t  type;              // EventType enum value
    uint8_t  msg_len;           // actual message length (excluding NUL)
    char     msg[EVLOG_MSG_MAX]; // null-terminated message
};  // 64 bytes exactly

static_assert(sizeof(EventEntry) == 64, "EventEntry must be exactly 64 bytes");

// ─── Ring buffer header ────────────────────────────────────────────────────

struct EventLogHeader {
    uint32_t magic;             // EVLOG_MAGIC when valid
    uint16_t writeIdx;          // next write position (0..EVLOG_MAX_EVENTS-1)
    uint16_t count;             // number of valid entries (max EVLOG_MAX_EVENTS)
    uint32_t totalWritten;      // total events ever written (detects overflow)
    uint8_t  padding[20];       // pad header to 32 bytes
};

static_assert(sizeof(EventLogHeader) == 32, "EventLogHeader must be exactly 32 bytes");

// ─── RTC_NOINIT storage ───────────────────────────────────────────────────

RTC_NOINIT_ATTR static EventLogHeader evHeader;
RTC_NOINIT_ATTR static EventEntry     evBuffer[EVLOG_MAX_EVENTS];

// ─── Spinlock for thread safety ────────────────────────────────────────────
// portENTER_CRITICAL / portEXIT_CRITICAL are ISR-safe on ESP32-S3.

static portMUX_TYPE evLogMux = portMUX_INITIALIZER_UNLOCKED;

// ─── Frozen state from previous boot ──────────────────────────────────────
// Captured at init time so we can report preservation status even after
// the current boot starts writing new events.

static bool     sPrevBootValid    = false;
static uint16_t sPrevBootCount    = 0;
static uint32_t sPrevBootTotal    = 0;

// ─── Event type name helper ────────────────────────────────────────────────

static const char *event_type_name(uint8_t t) {
    switch (t) {
        case EVT_STATE_CHANGE: return "STATE";
        case EVT_USER_TAP:     return "TAP";
        case EVT_BTN_PRESS:    return "BTN";
        case EVT_WIFI_EVENT:   return "WIFI";
        case EVT_API_CALL:     return "API";
        case EVT_AUDIO_EVENT:  return "AUDIO";
        case EVT_BLE_EVENT:    return "BLE";
        case EVT_MEMORY_SNAP:  return "MEM";
        case EVT_ERROR:        return "ERROR";
        case EVT_CUSTOM:       return "INFO";
        default:               return "???";
    }
}

// ──────────────────────────────────────────────────────────────────────────
//  Public API
// ──────────────────────────────────────────────────────────────────────────

void event_log_init() {
    if (evHeader.magic == EVLOG_MAGIC) {
        // RTC memory survived — previous boot's log is intact.
        // Snapshot the header state for preservation reporting.
        sPrevBootValid = true;
        sPrevBootCount = evHeader.count;
        sPrevBootTotal = evHeader.totalWritten;
        // Leave the buffer untouched so whathappened / crash screen can
        // read it.  Do NOT reset writeIdx/count — we continue appending
        // so the current boot's events are also captured if it crashes.
    } else {
        // Fresh power-on: RTC memory is garbage.  Initialize cleanly.
        sPrevBootValid = false;
        sPrevBootCount = 0;
        sPrevBootTotal = 0;

        evHeader.magic        = EVLOG_MAGIC;
        evHeader.writeIdx     = 0;
        evHeader.count        = 0;
        evHeader.totalWritten = 0;
        memset(evHeader.padding, 0, sizeof(evHeader.padding));
    }
}

// ─── Core logging ──────────────────────────────────────────────────────────

void event_log(EventType type, const char *msg) {
    portENTER_CRITICAL(&evLogMux);

    EventEntry *e = &evBuffer[evHeader.writeIdx];
    e->timestamp_ms = millis();
    e->type         = (uint8_t)type;

    if (msg) {
        size_t len = strlen(msg);
        if (len >= EVLOG_MSG_MAX) len = EVLOG_MSG_MAX - 1;
        memcpy(e->msg, msg, len);
        e->msg[len] = '\0';
        e->msg_len  = (uint8_t)len;
    } else {
        e->msg[0]  = '\0';
        e->msg_len = 0;
    }

    evHeader.writeIdx = (evHeader.writeIdx + 1) % EVLOG_MAX_EVENTS;
    if (evHeader.count < EVLOG_MAX_EVENTS) {
        evHeader.count++;
    }
    evHeader.totalWritten++;

    portEXIT_CRITICAL(&evLogMux);
}

void event_log_printf(EventType type, const char *fmt, ...) {
    char buf[EVLOG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    event_log(type, buf);
}

// ─── Query / status ────────────────────────────────────────────────────────

bool event_log_has_data() {
    return (evHeader.magic == EVLOG_MAGIC) && (evHeader.count > 0);
}

uint16_t event_log_count() {
    return (evHeader.magic == EVLOG_MAGIC) ? evHeader.count : 0;
}

uint8_t event_log_preservation_status() {
    if (!sPrevBootValid) return 0;                          // no data from prev boot
    if (sPrevBootTotal > EVLOG_MAX_EVENTS) return 2;        // overflow — oldest lost
    return 1;                                               // fully preserved
}

// ─── Dump report ───────────────────────────────────────────────────────────
// Builds a human-readable report string in PSRAM.  Caller must free().

char *event_log_dump_report() {
    if (evHeader.magic != EVLOG_MAGIC || evHeader.count == 0) return NULL;

    // Worst case: ~100 chars per event line + header/footer.  16 KB is generous.
    const size_t BUF_SIZE = 16384;
    char *buf = (char *)ps_malloc(BUF_SIZE);
    if (!buf) return NULL;

    int pos = 0;

    // ── Report header ──────────────────────────────────────────────────
    pos += snprintf(buf + pos, BUF_SIZE - pos,
        "========================================\n"
        "  EVENT LOG DUMP\n"
        "========================================\n");
    pos += snprintf(buf + pos, BUF_SIZE - pos,
        "Events stored : %u\n", evHeader.count);
    pos += snprintf(buf + pos, BUF_SIZE - pos,
        "Total written : %lu\n", (unsigned long)evHeader.totalWritten);

    if (evHeader.totalWritten > EVLOG_MAX_EVENTS) {
        pos += snprintf(buf + pos, BUF_SIZE - pos,
            "Overflow      : YES (%lu events lost)\n",
            (unsigned long)(evHeader.totalWritten - EVLOG_MAX_EVENTS));
    } else {
        pos += snprintf(buf + pos, BUF_SIZE - pos,
            "Overflow      : NO\n");
    }

    // ── CrashRecord data (if a crash was detected) ────────────────────
    if (crash_handler_has_crash()) {
        pos += snprintf(buf + pos, BUF_SIZE - pos,
            "\n--- CRASH RECORD ---\n");
        // We can't access CrashRecord fields directly (they're file-static
        // in crash_handler.cpp), but crash_handler_has_crash() confirms one
        // exists.  The crash screen already displays the details, so we note
        // the presence here.  Full crash data is shown on the crash screen.
        pos += snprintf(buf + pos, BUF_SIZE - pos,
            "(Crash detected — see crash screen for reason/backtrace)\n");
    }

    // ── Event entries (chronological, oldest first) ───────────────────
    pos += snprintf(buf + pos, BUF_SIZE - pos,
        "\n--- EVENTS (oldest first) ---\n");

    // Determine the starting index for chronological order.
    // If the buffer hasn't wrapped, start at 0.
    // If it has wrapped, start at writeIdx (the oldest entry).
    uint16_t count = evHeader.count;
    uint16_t startIdx;
    if (count < EVLOG_MAX_EVENTS) {
        startIdx = 0;  // buffer hasn't wrapped yet
    } else {
        startIdx = evHeader.writeIdx;  // oldest entry is at the write cursor
    }

    for (uint16_t i = 0; i < count && pos < (int)(BUF_SIZE - 128); i++) {
        uint16_t idx = (startIdx + i) % EVLOG_MAX_EVENTS;
        const EventEntry *e = &evBuffer[idx];

        // Format: [timestamp_ms] TYPE: message
        uint32_t secs = e->timestamp_ms / 1000;
        uint32_t ms   = e->timestamp_ms % 1000;

        pos += snprintf(buf + pos, BUF_SIZE - pos,
            "[%5lu.%03lu] %-5s: %s\n",
            (unsigned long)secs, (unsigned long)ms,
            event_type_name(e->type),
            e->msg);
    }

    // ── Footer ────────────────────────────────────────────────────────
    pos += snprintf(buf + pos, BUF_SIZE - pos,
        "========================================\n"
        "  END OF EVENT LOG\n"
        "========================================\n");

    return buf;
}

// ─── Clear ─────────────────────────────────────────────────────────────────

void event_log_clear() {
    portENTER_CRITICAL(&evLogMux);
    evHeader.magic        = 0;
    evHeader.writeIdx     = 0;
    evHeader.count        = 0;
    evHeader.totalWritten = 0;
    portEXIT_CRITICAL(&evLogMux);

    sPrevBootValid = false;
    sPrevBootCount = 0;
    sPrevBootTotal = 0;
}
