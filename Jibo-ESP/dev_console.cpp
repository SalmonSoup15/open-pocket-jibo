#include "dev_console.h"
#include "ble_link.h"
#include "log.h"

#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <WiFi.h>

// External — we don't include ble_link.cpp's TX char directly; instead
// it exposes a tiny "send opcode + payload" helper for our log bytes.
// To keep the API minimal we re-declare it here as a forward.
extern bool ble_dev_send(uint8_t op, const uint8_t *data, size_t len);

// ─── Dev mode master gate ──────────────────────────────────────────────────
bool g_dev_mode = false;   // set from storage at boot; gates ALL output

// ─── State ─────────────────────────────────────────────────────────────────
static volatile bool      gEnabled       = false;
static SemaphoreHandle_t  gMutex         = nullptr;

// Ring buffer for log bytes.  Lives in PSRAM so it doesn't compete with
// the precious internal heap.  Size: 16 KB — enough to absorb a verbose
// boot dump even if the phone briefly stalls reading.
static const size_t       RING_SIZE      = 16 * 1024;
static uint8_t           *gRing          = nullptr;
static volatile size_t    gHead          = 0;   // write index
static volatile size_t    gTail          = 0;   // read index
static volatile bool      gOverflowed    = false;

// Timing for periodic flush + stats.
static uint32_t           gLastFlushMs   = 0;
static uint32_t           gLastStatsMs   = 0;
static const uint32_t     FLUSH_PERIOD_MS = 80;
static const uint32_t     STATS_PERIOD_MS = 1000;

// Deferred BLE command.  BLE write callbacks run on the nimble_host
// task whose stack is too small for serial_handle_command() (some
// commands do heavy string building / printf chains).  The callback
// stashes the command here; dev_console_tick() picks it up on the
// main loop where the stack is deep enough.
static char               gPendingCmd[128] = {};
static volatile bool      gHasPendingCmd   = false;

// ─── Ring buffer helpers ───────────────────────────────────────────────────

static inline size_t ring_used() {
    size_t h = gHead, t = gTail;
    return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}

static inline size_t ring_free() {
    return RING_SIZE - 1 - ring_used();
}

// Append `len` bytes to the ring.  Drops oldest data on overflow so the
// most recent log lines (the ones the user usually cares about) win.
// Caller must hold gMutex.
static void ring_push(const uint8_t *data, size_t len) {
    if (!gRing || len == 0) return;
    if (len >= RING_SIZE) {
        // Pathological case — payload bigger than the buffer.  Keep
        // the tail end so we still emit the latest content.
        data += (len - (RING_SIZE - 1));
        len   = RING_SIZE - 1;
    }
    if (len > ring_free()) {
        // Drop oldest by advancing tail.
        size_t needToDrop = len - ring_free();
        gTail = (gTail + needToDrop) % RING_SIZE;
        gOverflowed = true;
    }
    size_t first = RING_SIZE - gHead;
    if (first > len) first = len;
    memcpy(gRing + gHead, data, first);
    if (len > first) memcpy(gRing, data + first, len - first);
    gHead = (gHead + len) % RING_SIZE;
}

// ─── Public API ────────────────────────────────────────────────────────────

void dev_console_init() {
    if (gRing) return;
    gRing  = (uint8_t *)ps_malloc(RING_SIZE);
    gMutex = xSemaphoreCreateMutex();
}

void dev_console_set_enabled(bool en) {
    if (!g_dev_mode && en) return;   // can't enable in stock mode
    if (en == gEnabled) return;
    gEnabled = en;
    if (gMutex) xSemaphoreTake(gMutex, portMAX_DELAY);
    if (en) {
        gHead = gTail = 0;
        gOverflowed = false;
    }
    if (gMutex) xSemaphoreGive(gMutex);
    if (g_dev_mode) Serial.printf("[devcon] BLE forwarding %s\n", en ? "ENABLED" : "DISABLED");
}

bool dev_console_enabled() { return gEnabled; }

int dev_console_printf(const char *fmt, ...) {
    // Stock mode: zero output — no serial, no BLE, no overhead.
    if (!g_dev_mode) return 0;

    char stackBuf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    va_end(ap);

    int written = (n < 0) ? 0 : (n < (int)sizeof(stackBuf) ? n : (int)sizeof(stackBuf) - 1);

    // Always write to USB serial for the local cable case.
    Serial.write((const uint8_t *)stackBuf, written);

    if (gEnabled && gRing && written > 0) {
        if (gMutex) xSemaphoreTake(gMutex, portMAX_DELAY);
        ring_push((const uint8_t *)stackBuf, written);
        if (gMutex) xSemaphoreGive(gMutex);
    }
    return n;
}

void dev_console_print(const char *s) {
    if (!g_dev_mode || !s) return;
    dev_console_printf("%s", s);
}

void dev_console_println(const char *s) {
    if (!g_dev_mode) return;
    if (!s) s = "";
    dev_console_printf("%s\n", s);
}

void dev_console_on_ble_cmd(const uint8_t *data, size_t len) {
    if (!g_dev_mode) return;   // reject commands in stock mode
    if (!data || len == 0) return;

    // Stash for deferred execution on the main loop — the nimble_host
    // stack is too small to run serial_handle_command() inline.
    size_t copyLen = (len < sizeof(gPendingCmd) - 1) ? len : sizeof(gPendingCmd) - 1;
    memcpy(gPendingCmd, data, copyLen);
    gPendingCmd[copyLen] = '\0';

    // Trim trailing whitespace in-place
    while (copyLen > 0 && (gPendingCmd[copyLen - 1] == ' ' ||
           gPendingCmd[copyLen - 1] == '\r' || gPendingCmd[copyLen - 1] == '\n')) {
        gPendingCmd[--copyLen] = '\0';
    }

    if (copyLen > 0) {
        Serial.printf("[devcon] CMD queued: %s\n", gPendingCmd);
        gHasPendingCmd = true;
    }
}

// ─── Stats blob ────────────────────────────────────────────────────────────
//
// Single short JSON line — phone parses with org.json.  Kept compact
// because BLE tx bandwidth is precious.  We include:
//
//   uptime_ms, heap_free, heap_min_free, heap_largest, psram_free,
//   psram_largest, wifi_connected, wifi_rssi, ble_connected,
//   tasks: [{name, prio, stack_hwm, state}]
//
// State values map to FreeRTOS eTaskState (R/B/S/D/I).

static const char *task_state_short(eTaskState s) {
    switch (s) {
        case eRunning:   return "R";
        case eReady:     return "Y";
        case eBlocked:   return "B";
        case eSuspended: return "S";
        case eDeleted:   return "D";
        case eInvalid:
        default:         return "?";
    }
}

// Per-task previous runtime cache, keyed by handle.  Used to compute
// per-task CPU% deltas across publish_stats() calls so the phone-side
// task manager can show a busy column.  Capped to keep the static
// footprint small — tasks beyond the cap simply don't get a CPU%
// reading (they show 0 / "—" in the UI).
struct PrevRuntime { TaskHandle_t h; uint32_t rt; };
static const int PREV_RT_CAP = 40;
static PrevRuntime gPrevRt[PREV_RT_CAP] = {};
static int         gPrevRtCount = 0;

static uint32_t prev_runtime_lookup_and_set(TaskHandle_t h, uint32_t now) {
    for (int i = 0; i < gPrevRtCount; i++) {
        if (gPrevRt[i].h == h) {
            uint32_t prev = gPrevRt[i].rt;
            gPrevRt[i].rt = now;
            return prev;
        }
    }
    if (gPrevRtCount < PREV_RT_CAP) {
        gPrevRt[gPrevRtCount++] = { h, now };
    }
    return now;       // first sighting → delta of 0
}

// Per-core idle time tracking — gives us a reliable "core busy %"
// without needing any one task's runtime to be a particular value.
// IDLE0 / IDLE1 are pinned by FreeRTOS so we look them up by name+core.
static uint32_t gPrevIdle[2]   = {0, 0};
static uint32_t gPrevTotal     = 0;
static bool     gHasPrevTotal  = false;

static void publish_stats() {
    // The full FreeRTOS task table (commonly ~13 entries on this
    // firmware) doesn't fit in a single BLE notification — each task
    // entry is ~70-90 B serialized and the GATT MTU caps payloads at
    // ~480 B usable.  So we send one BLE_OP_DEV_STATS frame with the
    // summary fields, then stream the task list across one or more
    // BLE_OP_DEV_TASKS frames.  The phone treats a fresh STATS as the
    // start of a new cycle (resets its accumulator) and treats a
    // TASKS frame with `f:1` as the cycle's terminator.
    const size_t maxPayload = 480;     // <= MTU(512) - GATT_HDR(3) - opcode(1) - slack
    char buf[512];

    int free_dram = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    int min_dram  = (int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    int big_dram  = (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    int free_psram = (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int big_psram  = (int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    int rssi = 0;
    bool wifi_conn = (WiFi.status() == WL_CONNECTED);
    if (wifi_conn) rssi = WiFi.RSSI();

    // ── Run-time counters ─────────────────────────────────────────────
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > 32) n = 32;
    TaskStatus_t *list = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * n);
    UBaseType_t got = 0;
    uint32_t totalRt = 0;
    if (list) {
        got = uxTaskGetSystemState(list, n, &totalRt);
    }

    // Compute per-core CPU%.  Strategy: each core spends 100% of wall
    // time either running tasks or running its IDLE task.  IDLE
    // run-time delta on that core gives us the idle fraction; the
    // remainder is busy.  Wall-time per core ≈ totalRt / 2 because
    // FreeRTOS sums the run-time counter across both cores and we
    // assume both are clocked equally (always true on the S3).
    int cpu0 = -1, cpu1 = -1;
    uint32_t curIdle[2] = {0, 0};
    bool     foundIdle[2] = {false, false};
    for (UBaseType_t i = 0; i < got; i++) {
        const TaskStatus_t &t = list[i];
        if (t.pcTaskName && strncmp(t.pcTaskName, "IDLE", 4) == 0) {
            int c = (t.xCoreID == tskNO_AFFINITY) ? -1 : (int)t.xCoreID;
            if (c == 0 || c == 1) {
                curIdle[c]   = t.ulRunTimeCounter;
                foundIdle[c] = true;
            }
        }
    }
    if (gHasPrevTotal) {
        uint32_t dTotal = totalRt - gPrevTotal;
        if (dTotal > 0) {
            uint32_t perCore = dTotal / 2;
            if (perCore > 0) {
                if (foundIdle[0]) {
                    uint32_t d = curIdle[0] - gPrevIdle[0];
                    int idlePct = (int)((uint64_t)d * 100 / perCore);
                    if (idlePct > 100) idlePct = 100;
                    if (idlePct < 0)   idlePct = 0;
                    cpu0 = 100 - idlePct;
                }
                if (foundIdle[1]) {
                    uint32_t d = curIdle[1] - gPrevIdle[1];
                    int idlePct = (int)((uint64_t)d * 100 / perCore);
                    if (idlePct > 100) idlePct = 100;
                    if (idlePct < 0)   idlePct = 0;
                    cpu1 = 100 - idlePct;
                }
            }
        }
    }
    gPrevIdle[0]  = curIdle[0];
    gPrevIdle[1]  = curIdle[1];
    gPrevTotal    = totalRt;
    gHasPrevTotal = true;

    // ── 1. Summary frame ──────────────────────────────────────────────
    int p = snprintf(buf, sizeof(buf),
        "{\"uptime_ms\":%lu,"
        "\"heap_free\":%d,\"heap_min\":%d,\"heap_big\":%d,"
        "\"psram_free\":%d,\"psram_big\":%d,"
        "\"wifi\":%d,\"rssi\":%d,"
        "\"cpu0\":%d,\"cpu1\":%d,"
        "\"task_count\":%u}",
        (unsigned long)millis(),
        free_dram, min_dram, big_dram,
        free_psram, big_psram,
        wifi_conn ? 1 : 0, rssi,
        cpu0, cpu1, (unsigned)got);
    if (p > 0 && p <= (int)maxPayload) {
        ble_dev_send(BLE_OP_DEV_STATS, (const uint8_t *)buf, (size_t)p);
    }

    // ── 2. Task chunks ────────────────────────────────────────────────
    //
    // STATS_PERIOD_MS is 1000 ms.  At 1 µs runtime-counter resolution
    // one wall-clock second on each core is 1_000_000 ticks, which is
    // the right denominator for a "% of one core, this second" reading.
    if (list) {
        const uint64_t perCore = 1000000ULL;
        UBaseType_t i = 0;

        // Always emit at least one TASKS frame so the phone reliably
        // gets the `f:1` terminator even on a zero-task edge case.
        do {
            int q = snprintf(buf, sizeof(buf), "{\"t\":[");
            bool first = true;
            // Reserve room for the closing `],"f":1}` (~10 B).
            const size_t closeReserve = 12;
            // Keep adding tasks until the next one would push us over
            // the per-frame budget.  ~120 B is a generous upper bound
            // for a single task entry given the 16-char name cap.
            while (i < got) {
                if ((size_t)q + 120 + closeReserve >= maxPayload) break;
                const TaskStatus_t &t = list[i];
                uint32_t prev = prev_runtime_lookup_and_set(
                    t.xHandle, t.ulRunTimeCounter);
                uint32_t dTask = t.ulRunTimeCounter - prev;
                int pct = (int)((uint64_t)dTask * 100 / perCore);
                if (pct < 0)   pct = 0;
                if (pct > 100) pct = 100;

                q += snprintf(buf + q, sizeof(buf) - q,
                    "%s{\"n\":\"%.16s\",\"p\":%u,\"s\":\"%s\",\"hwm\":%u,\"core\":%d,\"cpu\":%d}",
                    first ? "" : ",",
                    t.pcTaskName ? t.pcTaskName : "?",
                    (unsigned)t.uxCurrentPriority,
                    task_state_short(t.eCurrentState),
                    (unsigned)t.usStackHighWaterMark,
                    (int)((t.xCoreID == tskNO_AFFINITY) ? -1 : (int)t.xCoreID),
                    pct);
                first = false;
                i++;
            }
            bool isLast = (i >= got);
            q += snprintf(buf + q, sizeof(buf) - q,
                          "],\"f\":%d}", isLast ? 1 : 0);
            if (q > 0 && q <= (int)maxPayload) {
                ble_dev_send(BLE_OP_DEV_TASKS,
                             (const uint8_t *)buf, (size_t)q);
            }
        } while (i < got);
        free(list);
    } else {
        // Heap was so squeezed we couldn't even allocate the task
        // table — still send a terminator so the phone doesn't sit
        // forever waiting for one.
        const char empty[] = "{\"t\":[],\"f\":1}";
        ble_dev_send(BLE_OP_DEV_TASKS,
                     (const uint8_t *)empty, sizeof(empty) - 1);
    }
}

// ─── Tick: drain ring, publish stats ───────────────────────────────────────
//
// Drains the ring buffer in MTU-sized chunks and emits one BLE_OP_DEV_LOG
// notification per chunk.  Pacing matters here: BLE notifications queue
// up in the controller, and shoving a 5 KB boot dump in one tick will
// blow past the queue depth.  We emit at most ~512 B per tick (one or
// two MTUs) and let multiple ticks drain the rest.

void dev_console_tick() {
    // Execute deferred BLE command on the main-loop stack (safe).
    if (gHasPendingCmd) {
        gHasPendingCmd = false;
        serial_handle_command(String(gPendingCmd));
    }

    if (!gEnabled || !gRing) return;
    uint32_t now = millis();

    if (now - gLastFlushMs >= FLUSH_PERIOD_MS) {
        gLastFlushMs = now;

        size_t mtu = NimBLEDevice::getMTU() - 4;  // -3 GATT, -1 opcode
        if (mtu < 16)  mtu = 16;
        if (mtu > 480) mtu = 480;

        uint8_t  scratch[512];
        size_t   total = 0;
        for (int round = 0; round < 2; round++) {
            if (gMutex) xSemaphoreTake(gMutex, portMAX_DELAY);
            size_t used = ring_used();
            size_t take = used < mtu ? used : mtu;
            for (size_t i = 0; i < take; i++) {
                scratch[i] = gRing[(gTail + i) % RING_SIZE];
            }
            if (take > 0) gTail = (gTail + take) % RING_SIZE;
            bool overflow = gOverflowed;
            if (take > 0) gOverflowed = false;
            if (gMutex) xSemaphoreGive(gMutex);

            if (take == 0) break;
            // Prepend a marker on the very first chunk after an overflow
            // so the phone can render a "log truncated" indicator.
            if (overflow) {
                static const char marker[] = "\n[devcon] -- buffer overflowed, log truncated --\n";
                ble_dev_send(BLE_OP_DEV_LOG,
                             (const uint8_t *)marker, sizeof(marker) - 1);
            }
            ble_dev_send(BLE_OP_DEV_LOG, scratch, take);
            total += take;
        }
        (void)total;
    }

    if (now - gLastStatsMs >= STATS_PERIOD_MS) {
        gLastStatsMs = now;
        publish_stats();
    }
}
