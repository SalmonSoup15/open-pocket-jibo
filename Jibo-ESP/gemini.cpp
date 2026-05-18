#include "gemini.h"
#include "audio.h"
#include "storage.h"
#include "ble_link.h"
#include "notifications.h"
#include "wifi_portal.h"
#include "time_sync.h"
#include "event_log.h"
#include "dev_overlay.h"
#include "log.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"
#include "mbedtls/platform.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"

static const char *VOICE_MODELS[] = {
    "aura-2-odysseus-en", "aura-2-apollo-en", "aura-2-arcas-en",
    "aura-2-aries-en",    "aura-2-atlas-en",  "aura-2-hermes-en"
};
static const char *GEMINI_MODELS[] = {
    "gemini-flash-lite-latest",
    "gemini-flash-latest",
    "gemini-pro-latest"            // dev-mode only
};

static volatile bool  gBusy          = false;
static volatile bool  gDone          = false;
static volatile bool  gError         = false;
static volatile bool  gCancel        = false;
static volatile bool  gResponseReady = false;
static String         gResponse;        // speak-only text (tool tags stripped)
static String         gToolInvocation;  // raw tool string ("name:args"), or ""
static TaskHandle_t   gTask          = NULL;

// User-facing error context.  Tiny fixed buffers — no heap alloc, safe
// to read from the main task while gemini_task may still be running
// down (we set them BEFORE flipping gError, so by the time the state
// machine sees gError=true these are already in their final form).
static char           gErrSubsys[16] = {0};
static char           gErrDetail[80] = {0};

// Mark an error AND record context for the UI pill.  Always call this
// instead of `gError = true` directly so the user sees a useful
// subtext on the error pill.
static void mark_error(const char *subsys, const char *detail) {
    if (subsys) {
        strncpy(gErrSubsys, subsys, sizeof(gErrSubsys) - 1);
        gErrSubsys[sizeof(gErrSubsys) - 1] = 0;
    }
    if (detail) {
        strncpy(gErrDetail, detail, sizeof(gErrDetail) - 1);
        gErrDetail[sizeof(gErrDetail) - 1] = 0;
    }
    event_log_printf(EVT_ERROR, "%s: %s", subsys ? subsys : "?",
                     detail ? detail : "?");
    gError = true;
}

static int16_t       *gAudioBuf   = NULL;
static size_t         gAudioBytes = 0;

struct GeminiReq {
    const int16_t *pcm;
    size_t         pcmBytes;
    String         apiKey;
    String         ttsKey;
    String         textPrompt;   // non-empty → text mode (skip audio encoding)
};

static GeminiReq gReq;

// ─── Streaming pipeline state ───────────────────────────────────────────────
//
// Two tasks cooperate to overlap Gemini generation with TTS audio playback:
//   Core 0: gemini_do_work() streams SSE from Gemini, detects sentence
//           boundaries, pushes sentences to a FreeRTOS queue.
//   Core 1: tts_consumer_fn() dequeues sentences, POSTs each to Deepgram
//           TTS, writes PCM into the progressive playback buffer.

struct SentenceMsg {
    char *text;   // ps_malloc'd, consumer frees.  NULL = stop signal.
    bool  last;   // no more sentences after this one
};

#define SENTENCE_Q_LEN  8
static QueueHandle_t      gSentenceQ        = NULL;
static TaskHandle_t       gTtsConsumerTask   = NULL;
static volatile bool      gTtsConsumerDone   = false;
static volatile size_t    gTtsWritePos       = 0;
static uint8_t           *gStreamAudioBuf    = NULL;

// One-turn volatile memory: always kept in RAM (independent of persistent
// memory toggle) so the user can naturally say "yes, read it" right after
// being prompted about a sensitive notification.  Cleared on reboot.
static String volatileUserText;
static String volatileModelText;

// ─── JSON helpers (hand-rolled to avoid ArduinoJson dependency) ─────────────

static bool extract_json_text(const String &json, String &out) {
    int idx = json.indexOf("\"text\"");
    if (idx < 0) return false;
    idx = json.indexOf(':', idx + 6);
    if (idx < 0) return false;
    idx = json.indexOf('"', idx + 1);
    if (idx < 0) return false;
    idx++;
    String result;
    while (idx < (int)json.length()) {
        char c = json.charAt(idx);
        if (c == '"') break;
        if (c == '\\' && idx + 1 < (int)json.length()) {
            idx++;
            char esc = json.charAt(idx);
            if (esc == 'n') result += '\n';
            else if (esc == '"') result += '"';
            else if (esc == '\\') result += '\\';
            else { result += '\\'; result += esc; }
        } else {
            result += c;
        }
        idx++;
    }
    out = result;
    return true;
}

// ─── Tool invocation extraction ─────────────────────────────────────────────
//
// Gemini may begin its response with one or more tool calls of the form
//   [tool.name]{ ... brace-balanced args ... }
// optionally separated by whitespace.  Anything that isn't a tool call (or
// comes after the last contiguous one) is the natural-language response that
// gets sent to TTS.
//
// Tools are classified by name prefix:
//   - "show.*"  : DISPLAY tools.  The first one is captured into `outDisplayTool`
//                 (formatted "name:args") for STATE_TOOL_DISPLAY dispatch.
//                 Additional show.* tools after the first are dropped (state
//                 machine only handles one display at a time).
//   - other     : SIDE-EFFECT tools (e.g. "store.memory").  Every occurrence
//                 is appended to `outSideEffects` as "name:args" separated
//                 by '\x1F' (unit separator) so the caller can iterate.
//                 They run as side effects and don't change the spoken text.
//
// Returns the response text with the recognized tool prefix(es) stripped.
// If no tool is present, outputs are set to "" and input is returned as-is.
static String extract_tool_invocation(const String &resp,
                                      String &outDisplayTool,
                                      String &outSideEffects) {
    outDisplayTool = "";
    outSideEffects = "";
    int i = 0;
    int n = resp.length();
    while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++;
    int afterTools = i;

    while (i < n && resp[i] == '[') {
        int closeBracket = resp.indexOf(']', i + 1);
        if (closeBracket < 0) break;

        String name = resp.substring(i + 1, closeBracket);
        name.trim();
        // Tool names look like "show.text" — bail if we're staring at e.g.
        // a bracketed sentence "[note: ...]"
        if (name.indexOf('.') < 0) break;

        int j = closeBracket + 1;
        while (j < n && (resp[j] == ' ' || resp[j] == '\t')) j++;
        if (j >= n || resp[j] != '{') break;

        // Find the matching close brace, counting depth so nested braces
        // (e.g. \frac{a}{b} inside the args) don't terminate early.
        int depth = 1;
        int k = j + 1;
        while (k < n && depth > 0) {
            char c = resp[k];
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) break;
            }
            k++;
        }
        if (depth != 0) break;   // unbalanced — give up, treat as plain text

        String args = resp.substring(j + 1, k);

        if (name.startsWith("show.")) {
            if (outDisplayTool.length() == 0) {
                outDisplayTool = name + ":" + args;
            }
        } else {
            if (outSideEffects.length() > 0) outSideEffects += '\x1F';
            outSideEffects += name + ":" + args;
        }

        i = k + 1;
        while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++;
        afterTools = i;
    }

    return resp.substring(afterTools);
}

// Convenience overload — old call sites only care about the display tool.
static String extract_tool_invocation(const String &resp, String &outDisplayTool) {
    String discard;
    return extract_tool_invocation(resp, outDisplayTool, discard);
}

// ─── Side-effect tool dispatch ──────────────────────────────────────────────
//
// Walk the side-effects string produced by extract_tool_invocation() (entries
// separated by 0x1F unit-separator, each formatted "name:args") and apply
// each one.  Side-effect tools change device state (memory, settings, etc.)
// but do NOT change the spoken text or transition the UI state machine —
// they're fire-and-forget at parse time.
//
// This is called from two places: the Wi-Fi-direct response handler in
// gemini_do_work(), and the BLE-proxy entry point gemini_set_early_response().
// Both produce identical side-effect behavior so the user can't tell whether
// Gemini was reached directly or via the phone.

#include "pill_overlay.h"

static uint64_t make_memory_id() {
    // Compose a 64-bit ID from a millis() timestamp (high 48 bits) and 16
    // random bits at the bottom.  Two memories created in the same ms get
    // distinct IDs unless the 16-bit random space collides (chance ~1/65k,
    // and we de-dupe on insert anyway via upsert-by-id).
    uint64_t ts = (uint64_t)millis();
    uint16_t r  = (uint16_t)esp_random();
    return (ts << 16) | r;
}

static void apply_store_memory(const String &text) {
    String t = text;
    t.trim();
    if (t.length() == 0) return;
    uint64_t id = make_memory_id();
    // Prefer real wall-clock time so the Android app's "saved 5 mins
    // ago" display is meaningful.  If SNTP hasn't landed yet we fall
    // back to 0, which the app treats as "just now" rather than 1970.
    uint64_t ts = time_sync_now_epoch_ms();
    if (!storage_perm_mem_add(id, ts, t)) return;
    LOG1("[mem] stored id=%016llx text=%.80s\n",
         (unsigned long long)id, t.c_str());

    // No live BLE push to the phone — the phone polls the list when
    // the user opens the memories screen.  This is intentional: it
    // halves BT chatter and removes a whole class of sync edge cases.

    // Queue the achievement pill — it will fire when STATE_SPEAKING is
    // entered (so it appears the moment Jibo actually starts talking,
    // not during the silent THINKING wait).  The state machine calls
    // pill_release_pending() on speaking entry.  Subtext is the running
    // total so the user gets a sense of how much Jibo is remembering.
    char sub[40];
    uint16_t total = storage_perm_mem_count();
    snprintf(sub, sizeof(sub), "%u %s saved",
             (unsigned)total, total == 1 ? "memory" : "memories");
    pill_queue_for_speaking(PILL_ICON_SUCCESS, "Saved to memory", sub);
}

static void gemini_dispatch_side_effects(const String &sideEffects) {
    if (sideEffects.length() == 0) return;
    int start = 0;
    int n = sideEffects.length();
    while (start < n) {
        int sep = sideEffects.indexOf((char)0x1F, start);
        int end = (sep < 0) ? n : sep;
        String entry = sideEffects.substring(start, end);
        int colon = entry.indexOf(':');
        String name = (colon > 0) ? entry.substring(0, colon) : entry;
        String args = (colon > 0) ? entry.substring(colon + 1) : String();
        if (name == "store.memory") {
            apply_store_memory(args);
        } else if (name == "send.message") {
            int pipe = args.indexOf('|');
            if (pipe > 0) {
                String contact = args.substring(0, pipe);
                String text = args.substring(pipe + 1);
                contact.trim();
                text.trim();
                if (contact.length() > 0 && text.length() > 0) {
                    extern void states_queue_msg_compose(const String &, const String &);
                    states_queue_msg_compose(contact, text);
                    LOG1("[tool] send.message: to=%s text=%.60s\n", contact.c_str(), text.c_str());
                }
            }
        } else {
            LOG1("[tool] unknown side-effect tool: %s\n", name.c_str());
        }
        start = (sep < 0) ? n : sep + 1;
    }
}

// ─── HTTPS diagnostics ──────────────────────────────────────────────────────
//
// HTTPClient::POST returns -1 ("CONNECTION_REFUSED") for at least three
// completely different real causes:
//   - DNS lookup failed (WiFi.hostByName returned false)
//   - TCP connect failed / RST
//   - TLS handshake failed (and the *real* reason is in mbedtls's lastError)
// All three look identical from the outside, so a "random" -1 is impossible
// to debug.  These helpers split them apart and dump everything we know.

// Explicit DNS lookup with timing.  Returns true if resolved.  Used as a
// pre-flight before HTTPS connects so DNS failures show up as DNS failures
// instead of generic "HTTP -1".
static bool dns_check(const char *host, IPAddress &out, uint32_t *outMs) {
    uint32_t t0 = millis();
    bool ok = WiFi.hostByName(host, out);
    if (outMs) *outMs = millis() - t0;
    return ok;
}

// Dump everything useful when an HTTPS POST returns code <= 0 (transport
// failure).  Pulls the mbedtls error out of the WiFiClientSecure so we see
// the *actual* TLS / socket reason, not just HTTPClient's generic mapping.
static void log_https_failure(const char *tag, int httpCode,
                              WiFiClientSecure &client, uint32_t elapsedMs) {
    LOG1("[%s] FAIL: HTTPClient code=%d (%s) after %ums\n",
         tag, httpCode, HTTPClient::errorToString(httpCode).c_str(), elapsedMs);

    char errBuf[128] = {0};
    int mbedErr = client.lastError(errBuf, sizeof(errBuf));
    if (mbedErr != 0) {
        // mbedtls error codes are negative; printed as -0xXXXX matches the
        // mbedtls source file where they're #define'd (easy to grep).
        LOG1("[%s] FAIL: mbedtls=-0x%04x (%s)\n",
             tag, (unsigned)(-mbedErr), errBuf[0] ? errBuf : "(no description)");
    } else {
        LOG1("[%s] FAIL: no mbedtls error (likely DNS or socket connect)\n", tag);
    }

    LOG1("[%s] FAIL: WiFi status=%d, RSSI=%d, IP=%s, gw=%s, dns=%s\n",
         tag, (int)WiFi.status(), WiFi.RSSI(),
         WiFi.localIP().toString().c_str(),
         WiFi.gatewayIP().toString().c_str(),
         WiFi.dnsIP().toString().c_str());
}

// Counter of consecutive HTTPS transport failures across both Gemini and
// Deepgram paths.  When this gets high enough we kick WiFi to recover from
// stuck DNS / socket state without the user having to power-cycle.
static uint8_t gConsecutiveTransportFails = 0;

// Minimum largest contiguous *internal DRAM* block we need before opening
// a fresh TLS context.  With the mbedtls PSRAM allocator installed
// (gemini_install_mbedtls_psram_alloc), the 16 KB rx/tx record buffers
// go to PSRAM and the only DRAM needed is for small handshake / session
// allocs (a few hundred bytes each, ~6-8 KB total).  8 KB threshold is
// well above that and below the baseline ~26 KB largest-block ceiling
// we observed on this board, so this safety net should never trip.
static const size_t TLS_HEAP_MIN_LARGEST = 8 * 1024;

// IMPORTANT: must check INTERNAL DRAM specifically.  MALLOC_CAP_8BIT
// alone includes PSRAM which is huge (~4 MB) and would always pass the
// check while internal DRAM is actually starved — the bug that hid the
// real fragmentation problem.
static const uint32_t TLS_HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

// ─── mbedtls PSRAM allocator ────────────────────────────────────────────────
//
// THE ROOT CAUSE this fixes:
//   After WiFi/BLE/lwip have done their initial allocations, the largest
//   contiguous internal-DRAM block stabilises around ~26-32 KB no matter
//   how much TOTAL DRAM is free.  mbedtls's default record buffers
//   (16 KB rx + 16 KB tx, allocated as separate calloc calls) plus
//   session state need ~32-36 KB total contiguous, so on any given TLS
//   handshake it returns MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00) — and
//   there is no way to defragment ESP-IDF's heap at runtime.  Pre-
//   reserving a DRAM arena failed because there was already not enough
//   contiguous DRAM at boot to grab a 48 KB block (BLE init had already
//   carved up internal SRAM).
//
// THE FIX:
//   mbedtls exposes mbedtls_platform_set_calloc_free() which lets us
//   replace its allocator at runtime.  We install a hook that routes
//   BIG allocations (>= 4 KB) to PSRAM, where we have ~4 MB of free
//   contiguous space and will never run out.  Small allocations
//   (handshake state, ssl_context, X.509 nodes) stay in internal DRAM
//   for speed via the size threshold.
//
// PERFORMANCE NOTE:
//   PSRAM is ~10x slower than internal DRAM for byte-level access, but
//   mbedtls only does memcpy-style operations on the big record buffers.
//   The actual crypto math (AES, SHA, RSA, ECDHE) runs on small chunks
//   in registers / cache, which still come from DRAM.  Net cost is
//   ~50-100 ms added per TLS handshake, invisible in normal use.
//
// SIDE EFFECT:
//   This is a global mbedtls config — every TLS connection in the
//   firmware (us, OTA, anything else) will use this allocator.  That's
//   fine for our use case; the only consumers are Gemini and Deepgram.

static void *gemini_mbedtls_calloc(size_t n, size_t sz) {
    // Note: this can be called BEFORE Serial is up (during boot, by
    // arduino-esp32's own TLS init paths), so no LOG calls in here.
    size_t total = n * sz;
    void *p = NULL;

    // Threshold chosen so the 16 KB rx/tx buffers and any few-KB cert
    // / DH allocations land in PSRAM, while small structs (ssl_context
    // ~700 B, handshake state ~2 KB, hash contexts ~200 B) stay fast.
    if (total >= 4096) {
        p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!p) {
        // Small alloc, OR PSRAM exhausted (shouldn't happen — we have
        // 4+ MB free).  Plain calloc routes to MALLOC_CAP_DEFAULT which
        // is internal DRAM on this chip.
        p = calloc(n, sz);
    }
    return p;
}

static void gemini_mbedtls_free(void *p) {
    // ESP-IDF's free() handles both DRAM and PSRAM pointers correctly
    // (it looks up which heap the block came from in the metadata).
    free(p);
}

void gemini_install_mbedtls_psram_alloc() {
#if defined(MBEDTLS_PLATFORM_MEMORY)
    int rc = mbedtls_platform_set_calloc_free(gemini_mbedtls_calloc,
                                              gemini_mbedtls_free);
    if (rc == 0) {
        LOG1("[mbedtls] custom allocator installed: allocs >= 4096 B route "
             "to PSRAM, smaller stay in internal DRAM\n");
    } else {
        LOG1("[mbedtls] FAILED to install custom allocator: rc=%d "
             "(MBEDTLS_PLATFORM_MEMORY may be off — TLS will fail under "
             "DRAM fragmentation)\n", rc);
    }
#else
    LOG1("[mbedtls] platform memory hooks not compiled in — TLS may fail "
         "under DRAM fragmentation\n");
#endif
}

// Wait up to maxMs for the largest contiguous internal-DRAM block to
// reach TLS_HEAP_MIN_LARGEST.  Yields aggressively to give lwip /
// mbedtls cleanup tasks time to actually free their buffers.  Returns
// the largest-free-block size at exit (caller compares vs threshold).
static size_t wait_for_heap(const char *tag, uint32_t maxMs) {
    uint32_t t0 = millis();
    size_t largest = heap_caps_get_largest_free_block(TLS_HEAP_CAPS);
    if (largest >= TLS_HEAP_MIN_LARGEST) return largest;

    LOG1("[%s] DRAM cramped (largest=%u, need=%u), waiting up to %ums...\n",
         tag, (unsigned)largest, (unsigned)TLS_HEAP_MIN_LARGEST, maxMs);

    while (millis() - t0 < maxMs) {
        // 50ms is enough for the lwip / mbedtls cleanup task to run on
        // its core and for the heap allocator to merge adjacent freed
        // blocks.  We're not in a hot path — TLS handshake itself takes
        // hundreds of ms, so this is invisible to the user.
        vTaskDelay(pdMS_TO_TICKS(50));
        largest = heap_caps_get_largest_free_block(TLS_HEAP_CAPS);
        if (largest >= TLS_HEAP_MIN_LARGEST) {
            LOG1("[%s] DRAM recovered: largest=%u after %ums\n",
                 tag, (unsigned)largest, millis() - t0);
            return largest;
        }
    }
    LOG1("[%s] DRAM STILL cramped after %ums: free_internal=%u, largest=%u\n",
         tag, millis() - t0,
         (unsigned)heap_caps_get_free_size(TLS_HEAP_CAPS),
         (unsigned)largest);
    return largest;
}

// Best-effort WiFi reconnect.  Cheap if WiFi is already healthy; recovers
// us from stuck DHCP / DNS / socket state if not.  Only used after
// repeated transport failures.
static void wifi_soft_recover(const char *tag) {
    if (WiFi.status() != WL_CONNECTED) {
        LOG1("[%s] WiFi not connected (status=%d) — calling reconnect()\n",
             tag, (int)WiFi.status());
    } else {
        LOG1("[%s] %u consecutive transport failures — kicking WiFi\n",
             tag, gConsecutiveTransportFails);
    }
    WiFi.disconnect(false, false);  // keep SSID/pass, don't erase config
    vTaskDelay(pdMS_TO_TICKS(150));
    WiFi.reconnect();

    // Wait up to 5s for the reconnect to land.  We don't block forever —
    // if WiFi is genuinely down the next request will just fail too and
    // the user-visible state will reflect that.
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    LOG1("[%s] WiFi after recover: status=%d in %ums, IP=%s\n",
         tag, (int)WiFi.status(), millis() - t0,
         WiFi.localIP().toString().c_str());
}

// ─── JSON escape ────────────────────────────────────────────────────────────

static String json_escape(const String &s) {
    String out;
    out.reserve(s.length() + 32);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Append `src` to **pp with JSON escaping inline, advancing *pp.  Returns
// false if we would overrun `end`.  Used by build_system_prompt_psram so
// the prompt never lives in DRAM as an Arduino String — eliminates the
// realloc-doubling fragmentation that was preventing mbedtls from
// finding contiguous DRAM for the next TLS handshake.
static bool append_json_escaped(char **pp, char *end, const char *src) {
    char *p = *pp;
    while (*src) {
        // Worst case for any char is 2 bytes (\X).  Leave 1 byte slack
        // for the final NUL we never actually write but defensively reserve.
        if (p + 2 >= end) { *pp = p; return false; }
        char c = *src++;
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:   *p++ = c; break;
        }
    }
    *pp = p;
    return true;
}

// Same but for an Arduino String (for the dynamic notifications block,
// which is small enough that one transient String allocation is fine).
static bool append_json_escaped(char **pp, char *end, const String &s) {
    char *p = *pp;
    size_t n = s.length();
    const char *src = s.c_str();
    for (size_t i = 0; i < n; i++) {
        if (p + 2 >= end) { *pp = p; return false; }
        char c = src[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:   *p++ = c; break;
        }
    }
    *pp = p;
    return true;
}

// ─── TTS: progressive download + play from Deepgram ─────────────────────────

// Read a line from the stream (for chunk headers). Returns on \n or timeout.
static String read_line(WiFiClient *s, uint32_t timeoutMs) {
    String line;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (s->available()) {
            char c = s->read();
            if (c == '\n') break;
            if (c != '\r') line += c;
            t0 = millis();
        } else if (!s->connected()) {
            break;
        } else {
            yield();
        }
    }
    return line;
}

// Download HTTP response body into buf, updating the progressive player as
// data arrives.  Handles both fixed-length and chunked transfer encoding.
// Sets *complete to true if the stream terminated cleanly (final chunk or
// all content-length bytes received), false if truncated.
static size_t download_body(WiFiClient *s, uint8_t *buf, size_t bufSize,
                            int contentLen, bool *complete) {
    size_t pos = 0;
    *complete = false;
    uint32_t dlStart = millis();
    uint32_t lastLog = dlStart;
    uint32_t lastYield = dlStart;
    int readCount = 0;
    uint32_t maxReadMs = 0;

    if (contentLen > 0) {
        size_t remaining = (size_t)contentLen;
        while (remaining > 0 && pos < bufSize && !gCancel) {
            int avail = s->available();
            if (avail <= 0) {
                if (!s->connected()) break;
                vTaskDelay(1);
                lastYield = millis();
                continue;
            }
            size_t want = min((size_t)avail, min(remaining, bufSize - pos));
            if (want > 16384) want = 16384;
            uint32_t r0 = millis();
            int got = s->read(buf + pos, want);
            uint32_t rMs = millis() - r0;
            if (rMs > maxReadMs) maxReadMs = rMs;
            if (got <= 0) {
                LOG1("[dl] read returned %d after %ums (want=%u)\n", got, rMs, want);
                break;
            }
            pos += got;
            remaining -= got;
            readCount++;
            audio_progressive_update(pos);
            if (millis() - lastYield > 10) { vTaskDelay(1); lastYield = millis(); }
            if (millis() - lastLog > 500) {
                LOG3("[dl] %u/%d bytes, %d reads, maxRead=%ums\n",
                              pos, contentLen, readCount, maxReadMs);
                lastLog = millis();
            }
        }
        if (remaining == 0) *complete = true;
    } else {
        int chunkNum = 0;
        while (!gCancel) {
            uint32_t hdr0 = millis();
            String line = read_line(s, 30000);
            uint32_t hdrMs = millis() - hdr0;
            if (line.length() == 0) {
                if (!s->connected()) {
                    LOG1("[dl] disconnected waiting for chunk header\n");
                    break;
                }
                continue;
            }
            unsigned long chunkSize = strtoul(line.c_str(), NULL, 16);
            chunkNum++;
            if (chunkSize == 0) {
                LOG3("[dl] final chunk (#%d), total=%u bytes\n", chunkNum, pos);
                *complete = true;
                break;
            }
            if (hdrMs > 100)
                LOG3("[dl] chunk #%d hdr took %ums (size=%lu)\n", chunkNum, hdrMs, chunkSize);

            size_t rem = chunkSize;
            while (rem > 0 && pos < bufSize && !gCancel) {
                int avail = s->available();
                if (avail <= 0) {
                    if (!s->connected()) break;
                    vTaskDelay(1);
                    lastYield = millis();
                    continue;
                }
                size_t want = min((size_t)avail, min(rem, bufSize - pos));
                if (want > 16384) want = 16384;
                uint32_t r0 = millis();
                int got = s->read(buf + pos, want);
                uint32_t rMs = millis() - r0;
                if (rMs > maxReadMs) maxReadMs = rMs;
                if (got <= 0) {
                    LOG1("[dl] read returned %d after %ums (chunk #%d, rem=%u)\n",
                                  got, rMs, chunkNum, rem);
                    break;
                }
                pos += got;
                rem -= got;
                readCount++;
                audio_progressive_update(pos);
                if (millis() - lastYield > 10) { vTaskDelay(1); lastYield = millis(); }
            }
            read_line(s, 5000);
            if (rem > 0) {
                LOG1("[dl] incomplete chunk #%d (%u bytes remaining)\n", chunkNum, rem);
                break;
            }
            if (millis() - lastLog > 500) {
                LOG3("[dl] %u bytes after %d chunks, %d reads, maxRead=%ums\n",
                              pos, chunkNum, readCount, maxReadMs);
                lastLog = millis();
            }
        }
    }

    uint32_t totalMs = millis() - dlStart;
    LOG2("[dl] done: %u bytes in %ums (%d reads, maxRead=%ums, %.0f KB/s) %s\n",
                  pos, totalMs, readCount, maxReadMs,
                  totalMs > 0 ? (pos / 1024.0f) / (totalMs / 1000.0f) : 0.0f,
                  *complete ? "COMPLETE" : "TRUNCATED");
    return pos;
}

// Single TTS download attempt.  Returns bytes downloaded; sets *complete.
//
// attemptNum is purely for logging.  This function does NOT retry — that's
// the caller's job — but it does extensive diagnostics on every failure so
// the next time it breaks we can actually tell *why* instead of just "-1".
static size_t tts_attempt(const String &url, const String &reqBody,
                          const String &ttsKey, uint8_t *dlBuf,
                          size_t bufSize, bool *complete, int attemptNum) {
    *complete = false;

    // Pre-flight DNS.  ESP32's hostByName cache + lwip resolver is the most
    // common source of intermittent "HTTP -1" — if we don't check explicitly
    // a DNS failure looks identical to a TLS failure.
    IPAddress resolved;
    uint32_t dnsMs = 0;
    if (!dns_check("api.deepgram.com", resolved, &dnsMs)) {
        LOG1("[tts] FAIL: DNS api.deepgram.com timeout/refused in %ums "
             "(attempt %d)\n", dnsMs, attemptNum + 1);
        LOG1("[tts] FAIL: WiFi status=%d, RSSI=%d, dns server=%s\n",
             (int)WiFi.status(), WiFi.RSSI(), WiFi.dnsIP().toString().c_str());
        gConsecutiveTransportFails++;
        return 0;
    }
    LOG3("[tts] DNS api.deepgram.com -> %s in %ums\n",
         resolved.toString().c_str(), dnsMs);

    // Pre-flight heap check on INTERNAL DRAM.  With the PSRAM allocator
    // installed mbedtls only needs DRAM for small (< 4 KB) allocs, so a
    // tiny threshold is sufficient.  Kept as a safety net; should
    // basically always pass.
    size_t largest = wait_for_heap("tts", 500);
    LOG2("[tts] DRAM pre-connect: internal_free=%u, largest_internal=%u, "
         "psram_free=%u (attempt %d)\n",
         (unsigned)heap_caps_get_free_size(TLS_HEAP_CAPS),
         (unsigned)largest,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         attemptNum + 1);
    if (largest < TLS_HEAP_MIN_LARGEST) {
        LOG1("[tts] FAIL: DRAM too cramped even for small allocs "
             "(largest=%u, need=%u) — bailing out, will retry\n",
             (unsigned)largest, (unsigned)TLS_HEAP_MIN_LARGEST);
        gConsecutiveTransportFails++;
        return 0;
    }

    WiFiClientSecure client;
    client.setInsecure();
    // Default handshake timeout is 120s — way too long for our use case.
    // We'd rather fail fast and let the retry loop try again with fresh
    // network state.
    client.setHandshakeTimeout(15);   // seconds

    HTTPClient http;
    http.setTimeout(30000);           // ms — full request timeout
    http.setConnectTimeout(15000);    // ms — TCP/TLS connect phase only
    http.setReuse(false);
    if (!http.begin(client, url)) {
        LOG1("[tts] FAIL: http.begin() returned false (attempt %d)\n",
             attemptNum + 1);
        gConsecutiveTransportFails++;
        return 0;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + ttsKey);
    http.addHeader("Connection", "close");

    LOG2("[tts] POST Deepgram (%u bytes), attempt %d/3\n",
         reqBody.length(), attemptNum + 1);

    uint32_t postT0 = millis();
    int code = http.POST(reqBody);
    uint32_t postMs = millis() - postT0;

    if (code != 200) {
        if (code <= 0) {
            // Transport-level failure — pull the real reason out of mbedtls.
            log_https_failure("tts", code, client, postMs);
            gConsecutiveTransportFails++;
        } else {
            // 4xx / 5xx — server actually answered, log the body.  This is
            // where we'd see "out of credits", "invalid voice", rate limits,
            // etc.  No mbedtls involved.
            LOG1("[tts] FAIL: HTTP %d after %ums\n", code, postMs);
            String body = http.getString();
            if (body.length() > 0) {
                LOG1("[tts] response: %.500s\n", body.c_str());
            }
            // 5xx is server-side and worth retrying; 4xx is our request
            // shape and won't change on retry, but we leave the retry loop
            // in charge — it bails after MAX_TRIES regardless.
        }
        http.end();
        return 0;
    }

    // Success — clear the transport failure counter so we don't trigger
    // unnecessary WiFi recovery on the next call.
    gConsecutiveTransportFails = 0;

    int contentLen = http.getSize();
    LOG2("[tts] POST OK, %d bytes content-length, headers in %ums\n",
         contentLen, postMs);

    // Hint the audio task about expected total so it can size the prebuf
    // adaptively from measured network rate.  Skipped for chunked encoding
    // (contentLen <= 0) — play task falls back to rate-only heuristic.
    if (contentLen > 0) {
        audio_progressive_set_total((size_t)contentLen);
    }

    WiFiClient *stream = http.getStreamPtr();
    uint32_t t0 = millis();
    size_t total = download_body(stream, dlBuf, bufSize, contentLen, complete);
    http.end();

    LOG2("[tts] downloaded %u bytes (%.1fs audio) in %ums\n",
         total, total / 32000.0f, millis() - t0);
    return total;
}

static void do_tts(const String &text, const String &ttsKey) {
    uint8_t vi = storage_get_voice();
    if (vi >= 6) vi = 0;
    String url = String("https://api.deepgram.com/v1/speak?model=")
                 + VOICE_MODELS[vi]
                 + "&encoding=linear16&sample_rate=16000&container=none";

    String reqBody = "{\"text\":\"" + json_escape(text) + "\"}";

    audio_stop_play();
    if (gAudioBuf) { free(gAudioBuf); gAudioBuf = NULL; gAudioBytes = 0; }

    const size_t MAX_AUDIO = 1536 * 1024;
    uint8_t *dlBuf = (uint8_t *)ps_malloc(MAX_AUDIO);
    if (!dlBuf) {
        LOGLN1("[tts] PSRAM alloc failed");
        mark_error("TTS", "Out of memory");
        return;
    }

    audio_play_progressive(dlBuf);

    // Retry up to 3 times on either:
    //   - connect / TLS / DNS failure (total == 0)
    //   - truncated body (total > 0, !complete)
    //   - 4xx/5xx HTTP errors (also surface as total == 0 from tts_attempt)
    //
    // Backoff is staged: 0 → 500ms → 2000ms.  The longer second backoff
    // gives DNS cache and lwip socket state time to recover from whatever
    // brief upstream issue triggered the failure.
    const int MAX_TRIES = 3;
    static const uint32_t BACKOFF_MS[MAX_TRIES] = { 0, 500, 2000 };
    bool complete = false;
    size_t total  = 0;

    for (int attempt = 0; attempt < MAX_TRIES && !gCancel; attempt++) {
        if (attempt > 0) {
            LOG1("[tts] retry %d/%d (prev: total=%u, complete=%d), "
                 "backoff %ums, fails=%u\n",
                 attempt + 1, MAX_TRIES, (unsigned)total, (int)complete,
                 (unsigned)BACKOFF_MS[attempt],
                 (unsigned)gConsecutiveTransportFails);
            audio_stop_play();
            memset(dlBuf, 0, total > 0 ? total : 1024);
            audio_play_progressive(dlBuf);

            // After 2+ back-to-back transport failures the network state is
            // probably stuck (bad DNS cache, dead socket pool, dropped WiFi
            // we haven't noticed yet).  Soft-recover before the next attempt.
            if (gConsecutiveTransportFails >= 2) {
                wifi_soft_recover("tts");
            } else {
                vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS[attempt]));
            }
        }

        complete = false;
        total = tts_attempt(url, reqBody, ttsKey, dlBuf, MAX_AUDIO,
                            &complete, attempt);

        if (gCancel) break;
        if (complete && total > 0) break;  // success
    }

    if (gCancel || total == 0) {
        audio_stop_play();
        free(dlBuf);
        if (!gCancel && total == 0) {
            mark_error("TTS", "No audio after retries");
        }
        return;
    }

    gAudioBuf   = (int16_t *)dlBuf;
    gAudioBytes = total;
    audio_progressive_finish();
}

// ─── Streaming pipeline: helpers ────────────────────────────────────────────

// Reads body bytes from an HTTP response, transparently handling chunked
// transfer encoding so callers can treat the body as a simple byte stream.
struct BodyLineReader {
    WiFiClient *s;
    bool chunked;
    size_t chunkLeft;
    bool eof;

    BodyLineReader(WiFiClient *s_, bool isChunked)
        : s(s_), chunked(isChunked), chunkLeft(0), eof(false) {}

    int readByte(uint32_t timeoutMs) {
        if (eof) return -1;
        if (!chunked) {
            uint32_t t0 = millis();
            while (millis() - t0 < timeoutMs) {
                if (s->available()) return s->read();
                if (!s->connected()) { eof = true; return -1; }
                vTaskDelay(1);
            }
            return -1;
        }
        if (chunkLeft == 0) {
            String hdr;
            uint32_t t0 = millis();
            while (millis() - t0 < timeoutMs) {
                if (s->available()) {
                    char c = s->read();
                    if (c == '\n') break;
                    if (c != '\r') hdr += c;
                } else if (!s->connected()) { eof = true; return -1; }
                else vTaskDelay(1);
            }
            hdr.trim();
            if (hdr.length() == 0) { eof = true; return -1; }
            chunkLeft = strtoul(hdr.c_str(), NULL, 16);
            if (chunkLeft == 0) { eof = true; return -1; }
        }
        uint32_t t0 = millis();
        while (millis() - t0 < timeoutMs) {
            if (s->available()) {
                int b = s->read();
                chunkLeft--;
                if (chunkLeft == 0) {
                    // consume trailing \r\n
                    for (int i = 0; i < 2; i++) {
                        uint32_t t1 = millis();
                        while (millis() - t1 < 500) {
                            if (s->available()) { s->read(); break; }
                            if (!s->connected()) break;
                            vTaskDelay(1);
                        }
                    }
                }
                return b;
            }
            if (!s->connected()) { eof = true; return -1; }
            vTaskDelay(1);
        }
        return -1;
    }

    String readLine(uint32_t timeoutMs = 30000) {
        String line;
        uint32_t t0 = millis();
        while (millis() - t0 < timeoutMs) {
            int b = readByte(timeoutMs);
            if (b < 0) break;
            if ((char)b == '\n') break;
            if ((char)b != '\r') line += (char)b;
        }
        return line;
    }
};

static bool push_sentence(const char *text, bool last) {
    if (!gSentenceQ) return false;
    SentenceMsg msg;
    msg.last = last;
    if (text && text[0]) {
        size_t len = strlen(text);
        msg.text = (char *)ps_malloc(len + 1);
        if (!msg.text) return false;
        memcpy(msg.text, text, len + 1);
    } else {
        msg.text = NULL;
    }
    return xQueueSend(gSentenceQ, &msg, pdMS_TO_TICKS(10000)) == pdTRUE;
}

static int find_sentence_end(const String &text, int start) {
    for (int i = start; i < (int)text.length(); i++) {
        char c = text[i];
        if (c == '.' || c == '!' || c == '?') {
            if (i + 1 >= (int)text.length()) return i + 1;
            char next = text[i + 1];
            if (next == ' ' || next == '\n' || next == '\r' || next == '\t')
                return i + 1;
        }
    }
    return -1;
}

// ─── Streaming pipeline: TTS consumer (Core 1) ─────────────────────────────

struct TtsConsumerArgs {
    uint8_t *audioBuf;
    size_t   audioBufSize;
    String   ttsKey;
};

static void tts_consumer_fn(void *param) {
    TtsConsumerArgs *args = (TtsConsumerArgs *)param;
    uint8_t *audioBuf = args->audioBuf;
    size_t   bufSize  = args->audioBufSize;
    String   ttsKey   = args->ttsKey;
    delete args;

    gTtsConsumerDone = false;
    gTtsWritePos = 0;

    uint8_t vi = storage_get_voice();
    if (vi >= 6) vi = 0;
    String url = String("https://api.deepgram.com/v1/speak?model=")
                 + VOICE_MODELS[vi]
                 + "&encoding=linear16&sample_rate=16000&container=none";

    size_t writePos = 0;
    bool playerStarted = false;
    bool anyAudio = false;

    SentenceMsg msg;
    while (!gCancel) {
        // Short timeout so we re-check gCancel frequently
        if (xQueueReceive(gSentenceQ, &msg, pdMS_TO_TICKS(500)) != pdTRUE)
            continue;

        if (!msg.text) {
            if (msg.last) break;
            continue;
        }

        String sentence(msg.text);
        free(msg.text);
        msg.text = NULL;

        if (sentence.length() == 0) {
            if (msg.last) break;
            continue;
        }

        LOG2("[tts-c] sentence: \"%.80s%s\"\n",
             sentence.c_str(), sentence.length() > 80 ? "..." : "");

        String reqBody = "{\"text\":\"" + json_escape(sentence) + "\"}";

        IPAddress resolved;
        uint32_t dnsMs;
        if (!dns_check("api.deepgram.com", resolved, &dnsMs)) {
            LOG1("[tts-c] DNS failed (%ums)\n", dnsMs);
            gConsecutiveTransportFails++;
            if (msg.last) break;
            continue;
        }

        size_t largest = wait_for_heap("tts-c", 300);
        if (largest < TLS_HEAP_MIN_LARGEST) {
            LOG1("[tts-c] heap too low (%u)\n", (unsigned)largest);
            if (msg.last) break;
            continue;
        }

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(15);

        HTTPClient http;
        http.setTimeout(30000);
        http.setConnectTimeout(15000);
        http.setReuse(false);

        if (!http.begin(client, url)) {
            LOG1("[tts-c] http.begin failed\n");
            if (msg.last) break;
            continue;
        }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Token " + ttsKey);
        http.addHeader("Connection", "close");

        uint32_t t0 = millis();
        int code = http.POST(reqBody);
        uint32_t postMs = millis() - t0;

        if (code != 200) {
            if (code <= 0) log_https_failure("tts-c", code, client, postMs);
            else LOG1("[tts-c] HTTP %d in %ums\n", code, postMs);
            http.end();
            gConsecutiveTransportFails++;
            if (msg.last) break;
            continue;
        }

        gConsecutiveTransportFails = 0;

        int contentLen = http.getSize();
        WiFiClient *stream = http.getStreamPtr();

        // Start progressive player on the first audio data
        if (!playerStarted) {
            audio_play_progressive(audioBuf);
            if (contentLen > 0)
                audio_progressive_set_total((size_t)contentLen);
            playerStarted = true;
        }
        anyAudio = true;

        LOG2("[tts-c] %d bytes, streaming...\n", contentLen);

        // Download TTS audio into progressive buffer
        uint32_t lastYield = millis();
        if (contentLen > 0) {
            size_t remaining = (size_t)contentLen;
            while (remaining > 0 && !gCancel && writePos < bufSize) {
                int avail = stream->available();
                if (avail <= 0) {
                    if (!stream->connected()) break;
                    vTaskDelay(1);
                    lastYield = millis();
                    continue;
                }
                size_t want = min((size_t)avail, min(remaining, bufSize - writePos));
                if (want > 16384) want = 16384;
                int got = stream->read(audioBuf + writePos, want);
                if (got <= 0) break;
                writePos += got;
                remaining -= got;
                gTtsWritePos = writePos;
                audio_progressive_update(writePos);
                if (millis() - lastYield > 10) { vTaskDelay(1); lastYield = millis(); }
            }
        } else {
            // Chunked transfer
            while (!gCancel && writePos < bufSize) {
                String chunkLine = read_line(stream, 30000);
                if (chunkLine.length() == 0 && !stream->connected()) break;
                unsigned long chunkSize = strtoul(chunkLine.c_str(), NULL, 16);
                if (chunkSize == 0) break;
                size_t rem = chunkSize;
                while (rem > 0 && !gCancel && writePos < bufSize) {
                    int avail = stream->available();
                    if (avail <= 0) {
                        if (!stream->connected()) break;
                        vTaskDelay(1);
                        lastYield = millis();
                        continue;
                    }
                    size_t want = min((size_t)avail, min(rem, bufSize - writePos));
                    if (want > 16384) want = 16384;
                    int got = stream->read(audioBuf + writePos, want);
                    if (got <= 0) break;
                    writePos += got;
                    rem -= got;
                    gTtsWritePos = writePos;
                    audio_progressive_update(writePos);
                    if (millis() - lastYield > 10) { vTaskDelay(1); lastYield = millis(); }
                }
                read_line(stream, 5000); // trailing CRLF
            }
        }

        http.end();
        LOG2("[tts-c] done, total buf %u bytes\n", (unsigned)writePos);

        if (msg.last) break;
    }

    // Drain remaining messages
    while (xQueueReceive(gSentenceQ, &msg, 0) == pdTRUE) {
        if (msg.text) free(msg.text);
    }

    gTtsWritePos = writePos;
    if (playerStarted) audio_progressive_finish();

    if (!anyAudio && !gCancel) {
        mark_error("TTS", "No audio produced");
    }

    gTtsConsumerDone = true;
    gTtsConsumerTask = NULL;
    vTaskDelete(NULL);
}

// ─── Streaming pipeline: SSE reader ────────────────────────────────────────
//
// Reads the Gemini streamGenerateContent SSE response, extracts text chunks,
// detects tool invocations at the start, splits spoken text on sentence
// boundaries, and pushes each sentence to the TTS consumer queue.

static bool gemini_stream_sse(WiFiClient *rawStream, bool isChunked,
                               String &fullText,
                               String &outDisplayTool,
                               String &outSideEffects) {
    BodyLineReader reader(rawStream, isChunked);

    String accumulated;
    String spoken;
    bool toolsDone = false;
    int sentPos = 0;

    outDisplayTool = "";
    outSideEffects = "";

    uint32_t lastDataMs = millis();

    while (!gCancel && !reader.eof) {
        String line = reader.readLine(30000);

        if (line.length() == 0) {
            if (reader.eof) break;
            if (millis() - lastDataMs > 30000) break;
            continue;
        }

        lastDataMs = millis();
        if (!line.startsWith("data: ")) continue;

        String json = line.substring(6);

        // Skip thinking chunks (thinkingConfig may emit them before the response)
        if (json.indexOf("\"thought\"") >= 0 && json.indexOf("true") >= 0)
            continue;

        String chunk;
        if (!extract_json_text(json, chunk)) continue;
        if (chunk.length() == 0) continue;

        accumulated += chunk;

        if (!toolsDone) {
            String dt, se;
            String sp = extract_tool_invocation(accumulated, dt, se);

            int ws = 0;
            while (ws < (int)accumulated.length() &&
                   (accumulated[ws] == ' ' || accumulated[ws] == '\n' ||
                    accumulated[ws] == '\r' || accumulated[ws] == '\t'))
                ws++;

            bool looksLikeTools = (ws < (int)accumulated.length() &&
                                   accumulated[ws] == '[');

            if (!looksLikeTools) {
                toolsDone = true;
                outDisplayTool = "";
                outSideEffects = "";
                spoken = accumulated;
            } else if (sp.length() > 0 && sp.length() < accumulated.length()) {
                toolsDone = true;
                outDisplayTool = dt;
                outSideEffects = se;
                spoken = sp;
            }
        } else {
            spoken += chunk;
        }

        if (toolsDone && spoken.length() > 0) {
            int boundary = find_sentence_end(spoken, sentPos);
            while (boundary >= 0 && boundary > sentPos) {
                String sentence = spoken.substring(sentPos, boundary);
                sentence.trim();
                if (sentence.length() > 0) {
                    push_sentence(sentence.c_str(), false);
                    LOG3("[sse] pushed: \"%.60s\"\n", sentence.c_str());
                }
                sentPos = boundary;
                while (sentPos < (int)spoken.length() &&
                       (spoken[sentPos] == ' ' || spoken[sentPos] == '\n'))
                    sentPos++;
                boundary = find_sentence_end(spoken, sentPos);
            }
        }
    }

    fullText = accumulated;

    if (!toolsDone) {
        String dt, se;
        spoken = extract_tool_invocation(accumulated, dt, se);
        outDisplayTool = dt;
        outSideEffects = se;
    }

    // Push remaining spoken text as final sentence
    if (sentPos < (int)spoken.length()) {
        String remaining = spoken.substring(sentPos);
        remaining.trim();
        if (remaining.length() > 0) {
            push_sentence(remaining.c_str(), true);
        } else {
            push_sentence(NULL, true);
        }
    } else {
        push_sentence(NULL, true);
    }

    return accumulated.length() > 0;
}

// ─── Background task ────────────────────────────────────────────────────────

// Build a minimal WAV header for 16kHz 16-bit mono PCM
static void build_wav_header(uint8_t *hdr, uint32_t pcmBytes, uint32_t sampleRate) {
    uint32_t fileSize = pcmBytes + 36;
    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;

    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &fileSize, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(hdr + 16, &fmtSize, 4);
    uint16_t audioFmt = 1;
    memcpy(hdr + 20, &audioFmt, 2);
    memcpy(hdr + 22, &numChannels, 2);
    memcpy(hdr + 24, &sampleRate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bitsPerSample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &pcmBytes, 4);
}

// Separated from the FreeRTOS task so all C++ objects (WiFiClientSecure,
// HTTPClient, String) are properly destructed when this function returns.
// vTaskDelete skips destructors, which leaks ~40KB of TLS context per call.
// Format a single text-only volatile turn pair as a JSON history fragment
// (the same shape NVS stores: comma-separated user/model objects, no outer []).
static String volatile_history_fragment() {
    if (volatileUserText.length() == 0 || volatileModelText.length() == 0) return "";
    String out;
    out.reserve(volatileUserText.length() + volatileModelText.length() + 128);
    out += "{\"role\":\"user\",\"parts\":[{\"text\":\"";
    out += json_escape(volatileUserText);
    out += "\"}]},{\"role\":\"model\",\"parts\":[{\"text\":\"";
    out += json_escape(volatileModelText);
    out += "\"}]}";
    return out;
}

// Build the JSON history fragment to send with a request.  Persistent history
// when memory is enabled, otherwise the one-turn volatile pair (or empty).
// Returned shape matches NVS layout: comma-separated turn objects with no
// enclosing brackets.  Used by both the WiFi and BLE request paths.  Exposed
// (non-static) so ble_link.cpp can pull the same fragment when proxying.
String gemini_effective_history_fragment() {
    if (storage_get_memory_enabled()) {
        String hist = storage_get_conv_history();
        if (hist.length() > 0) return hist;
    }
    return volatile_history_fragment();
}

// Build the entire Gemini JSON request directly into a single PSRAM buffer.
//
// Why: Arduino String lives in DRAM and silently *drops* data when realloc
// fails under fragmentation.  The audio path used to do roughly:
//
//     userPartJson += b64;                       // ~70 KB DRAM grow
//     contents     += "..." + userPartJson + "..."; // another ~70 KB temp
//
// Either of those concatenations could fail invisibly, leaving the JSON
// missing its closing `"}}]}` and producing the "Expected , or } after
// key:value pair" 400 we kept seeing — confirmed by a 9 KB POST when the
// payload should have been ~71 KB.
//
// This helper does the math up front, allocates ONE PSRAM block (4 MB
// available there), and writes everything sequentially with bounds checks.
// Base64 is encoded straight into the payload buffer at the right offset
// so the audio body never lives in a separate String.
//
// On success returns the buffer (caller free()s) and writes the byte length
// into *outLen.  Returns NULL on alloc failure or any encode/format error.
//
// sysPromptEsc / sysPromptLen is the already-JSON-escaped system prompt as
// a raw byte buffer (not an Arduino String — see build_system_prompt_psram
// for why).  Both buffers live in PSRAM, so the only DRAM work this
// function does is the snprintf() call for the fixed JSON scaffolding.
static char *build_payload_psram(const char *sysPromptEsc, size_t sysPromptLen,
                                 const String &hist,
                                 bool isAudio,
                                 const uint8_t *wavBuf, size_t wavLen,
                                 const String &userTextEsc,
                                 size_t *outLen) {
    *outLen = 0;

    // mbedtls writes ((slen+2)/3)*4 chars + a trailing NUL byte (we'll
    // overwrite the NUL with the next snprintf, so just leave room for it).
    size_t b64Cap   = isAudio ? (4 * ((wavLen + 2) / 3) + 4) : 0;
    size_t userCap  = isAudio ? (b64Cap + 64) : (userTextEsc.length() + 32);
    // 1024 covers all the fixed JSON scaffolding plus generous headroom.
    size_t cap      = sysPromptLen + hist.length() + userCap + 1024;

    char *payload = (char *)ps_malloc(cap);
    if (!payload) {
        LOG1("[gemini] payload PSRAM alloc failed (wanted %u)\n", (unsigned)cap);
        return NULL;
    }

    char *p   = payload;
    char *end = payload + cap;

    // Header — write the fixed JSON scaffolding around the pre-escaped
    // system prompt manually (no snprintf %s on the prompt because it can
    // be many KB and we don't need format conversion anyway).
    static const char SYS_HDR[] =
        "{\"system_instruction\":{\"parts\":[{\"text\":\"";
    static const char SYS_TAIL[] = "\"}]},\"contents\":[";

    if (p + sizeof(SYS_HDR) - 1 + sysPromptLen + sizeof(SYS_TAIL) - 1 >= end) {
        free(payload);
        return NULL;
    }
    memcpy(p, SYS_HDR, sizeof(SYS_HDR) - 1); p += sizeof(SYS_HDR) - 1;
    memcpy(p, sysPromptEsc, sysPromptLen);   p += sysPromptLen;
    memcpy(p, SYS_TAIL, sizeof(SYS_TAIL) - 1); p += sizeof(SYS_TAIL) - 1;
    int n = 0;

    if (hist.length() > 0) {
        if (p + hist.length() + 1 >= end) { free(payload); return NULL; }
        memcpy(p, hist.c_str(), hist.length());
        p += hist.length();
        *p++ = ',';
    }

    n = snprintf(p, end - p, "{\"role\":\"user\",\"parts\":[");
    if (n < 0 || p + n >= end) { free(payload); return NULL; }
    p += n;

    if (isAudio) {
        n = snprintf(p, end - p,
            "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
        if (n < 0 || p + n >= end) { free(payload); return NULL; }
        p += n;

        // Encode base64 directly into the payload buffer at the current
        // write position.  No intermediate String, no DRAM realloc.
        size_t b64Written = 0;
        int rc = mbedtls_base64_encode((unsigned char *)p, end - p,
                                       &b64Written, wavBuf, wavLen);
        if (rc != 0) {
            LOG1("[gemini] base64 encode failed rc=%d (cap=%u, wav=%u)\n",
                 rc, (unsigned)(end - p), (unsigned)wavLen);
            free(payload);
            return NULL;
        }
        p += b64Written;

        n = snprintf(p, end - p, "\"}}");
        if (n < 0 || p + n >= end) { free(payload); return NULL; }
        p += n;
    } else {
        n = snprintf(p, end - p, "{\"text\":\"%s\"}", userTextEsc.c_str());
        if (n < 0 || p + n >= end) { free(payload); return NULL; }
        p += n;
    }

    n = snprintf(p, end - p,
        "]}],"
        "\"generationConfig\":{\"thinkingConfig\":{\"thinkingLevel\":\"MINIMAL\"}},"
        "\"tools\":[{\"googleSearch\":{}},{\"urlContext\":{}}]"
        "}");
    if (n < 0 || p + n >= end) { free(payload); return NULL; }
    p += n;

    *outLen = (size_t)(p - payload);

    // Loud sanity check — if this ever trips we want to know immediately
    // rather than send malformed JSON to Google.
    if (*outLen < 64 || payload[0] != '{' || payload[*outLen - 1] != '}') {
        LOG1("[gemini] payload sanity FAIL (len=%u, first=%02x, last=%02x)\n",
             (unsigned)*outLen, (uint8_t)payload[0],
             *outLen > 0 ? (uint8_t)payload[*outLen - 1] : 0);
        free(payload);
        return NULL;
    }

    return payload;
}

// HTTPS POST to Gemini with retry on transport / 5xx errors.
//
// Returns the final HTTP status (>0) or HTTPClient negative error code.
// On any return path that produces a body (HTTP success or HTTP error), the
// body is written to responseBody for the caller to parse / log.  4xx is
// NOT retried because it's a request-shape error and won't help; 5xx and
// transport-level errors (-1, -11, etc) get up to MAX_TRIES attempts with
// a staged backoff (0 → 500ms → 2000ms).  Same diagnostic treatment as
// tts_attempt: DNS pre-check, mbedtls lastError capture, WiFi state dump.
static int gemini_post_with_retry(const String &url,
                                  const char *payload, size_t pLen,
                                  String &responseBody) {
    const int MAX_TRIES = 3;
    static const uint32_t BACKOFF_MS[MAX_TRIES] = { 0, 500, 2000 };
    int code = -1;
    responseBody = "";

    for (int attempt = 0; attempt < MAX_TRIES && !gCancel; attempt++) {
        if (attempt > 0) {
            LOG1("[gemini] retry %d/%d (prev code=%d), backoff %ums, fails=%u\n",
                 attempt + 1, MAX_TRIES, code, (unsigned)BACKOFF_MS[attempt],
                 (unsigned)gConsecutiveTransportFails);
            if (gConsecutiveTransportFails >= 2) {
                wifi_soft_recover("gemini");
            } else {
                vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS[attempt]));
            }
        }

        // DNS pre-check — same rationale as tts_attempt.
        IPAddress resolved;
        uint32_t dnsMs = 0;
        if (!dns_check("generativelanguage.googleapis.com", resolved, &dnsMs)) {
            LOG1("[gemini] FAIL: DNS lookup timeout/refused in %ums "
                 "(attempt %d)\n", dnsMs, attempt + 1);
            LOG1("[gemini] FAIL: WiFi status=%d, RSSI=%d, dns server=%s\n",
                 (int)WiFi.status(), WiFi.RSSI(),
                 WiFi.dnsIP().toString().c_str());
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }
        LOG3("[gemini] DNS resolved to %s in %ums\n",
             resolved.toString().c_str(), dnsMs);

        // Pre-flight INTERNAL DRAM check.  With the PSRAM allocator
        // installed mbedtls's big buffers go to PSRAM; we just need
        // enough DRAM for the small handshake state allocs.
        size_t largest = wait_for_heap("gemini", 500);
        LOG2("[gemini] DRAM pre-connect: internal_free=%u, largest_internal=%u, "
             "psram_free=%u (attempt %d)\n",
             (unsigned)heap_caps_get_free_size(TLS_HEAP_CAPS),
             (unsigned)largest,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             attempt + 1);
        if (largest < TLS_HEAP_MIN_LARGEST) {
            LOG1("[gemini] FAIL: DRAM too cramped even for small allocs "
                 "(largest=%u, need=%u) — will retry\n",
                 (unsigned)largest, (unsigned)TLS_HEAP_MIN_LARGEST);
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }

        // Each attempt gets its own client + http so the TLS context is
        // freshly allocated/freed (avoids stuck-state on the previous one).
        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(15);   // seconds — fail fast, retry

        HTTPClient http;
        http.setTimeout(30000);
        http.setConnectTimeout(15000);
        http.setReuse(false);
        if (!http.begin(client, url)) {
            LOG1("[gemini] FAIL: http.begin() returned false (attempt %d)\n",
                 attempt + 1);
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }
        http.addHeader("Content-Type", "application/json");

        uint32_t postT0 = millis();
        code = http.POST((uint8_t *)payload, pLen);
        uint32_t postMs = millis() - postT0;

        if (gCancel) {
            http.end();
            return code;
        }

        if (code > 0) {
            responseBody = http.getString();
            http.end();
            // 2xx → done.  4xx → giving up (our payload is the problem).
            // 5xx → retry, server-side hiccup.  Either way, a real HTTP
            // response means the transport is healthy.
            gConsecutiveTransportFails = 0;
            if (code < 500) return code;
            LOG1("[gemini] HTTP %d (5xx) after %ums — will retry\n",
                 code, postMs);
            continue;
        }

        // Transport-level failure — pull the real reason out of mbedtls.
        log_https_failure("gemini", code, client, postMs);
        gConsecutiveTransportFails++;
        http.end();
    }

    return code;
}

// After a successful response, update both volatile and (if enabled) persistent
// memory.  Volatile is *always* updated so single-follow-up flows keep working
// when persistent memory is off.
static void save_turn(const String &userText, const String &modelText) {
    volatileUserText  = userText;
    volatileModelText = modelText;

    if (!storage_get_memory_enabled()) return;

    String hist = storage_get_conv_history();
    String newTurns;
    newTurns.reserve(userText.length() + modelText.length() + 128);
    newTurns += "{\"role\":\"user\",\"parts\":[{\"text\":\"";
    newTurns += json_escape(userText);
    newTurns += "\"}]},{\"role\":\"model\",\"parts\":[{\"text\":\"";
    newTurns += json_escape(modelText);
    newTurns += "\"}]}";

    if (hist.length() > 0) {
        hist += ',';
        hist += newTurns;
    } else {
        hist = newTurns;
    }
    storage_set_conv_history(hist);
}

// ─── System prompt builder ──────────────────────────────────────────────────
//
// Assembled per-request so notifications + phone-link state are always fresh.
// Mirrored on the Android side in GeminiClient.kt::buildSystemPrompt() so BLE-
// proxied requests get identical instructions.
//
// CRITICAL: this writes the JSON-escaped prompt directly into a PSRAM buffer.
// We deliberately do NOT use Arduino String for the bulk text — String's
// realloc-doubling growth scattered ~16 KB of fragmentation across DRAM
// every call, which is exactly what was preventing mbedtls from finding a
// contiguous 32 KB block for the next TLS context.  Returns a heap-allocated
// (PSRAM) buffer with the already-escaped prompt; caller free()s.  Sets
// *outLen to the byte length (no NUL terminator written).  Returns NULL on
// alloc failure.
static char *build_system_prompt_psram(size_t *outLen) {
    *outLen = 0;
    // ~3 KB fixed text + up to 50 notifications × ~400 chars escaped ≈ 23 KB
    // worst case.  Round up to 24 KB so we never have to realloc.  This whole
    // block lives in PSRAM (4 MB free) so size is essentially free.
    const size_t cap = 24 * 1024;
    char *buf = (char *)ps_malloc(cap);
    if (!buf) {
        LOG1("[gemini] sysPrompt PSRAM alloc failed (wanted %u)\n",
             (unsigned)cap);
        return NULL;
    }
    char *p   = buf;
    char *end = buf + cap;

    // Base personality
    if (!append_json_escaped(&p, end,
        "You are Jibo, a friendly social robot companion. "
        "Keep responses brief and conversational, one or two sentences.\n\n"
        "REAL-TIME INFO:\n"
        "- You have Google Search and URL-context tools wired in by the "
        "API. When the user asks anything time-sensitive -- today's "
        "weather, news, current events, prices, scores, who-is-someone, "
        "recent releases, etc. -- USE Google Search and answer from the "
        "results. Never tell the user you don't have access to "
        "real-time information; you do.\n"
        "- When you're not 100% sure of a current fact, prefer searching "
        "over guessing.\n\n")) {
        free(buf);
        return NULL;
    }

    // Notification rules — only include when a phone is paired
    if (ble_is_paired()) {
        if (!append_json_escaped(&p, end,
            "NOTIFICATION RULES:\n"
            "- DO NOT proactively offer to check notifications during normal "
            "conversation. Never ask things like \"do you have any messages "
            "you want me to check?\" or \"want me to read your latest "
            "notifications?\" -- the user will ask if they want them. The "
            "notification list below is purely background context.\n"
            "- If the user asks about notifications (\"do I have any messages "
            "from X?\", \"any new emails?\"), use the list below as the source "
            "of truth. Never invent notifications.\n"
            "- The bracketed tag (e.g. [messaging], [banking], [email]) is just "
            "a descriptive category, NOT a sensitivity flag. You judge "
            "sensitivity yourself from the actual body text.\n"
            "- Treat a notification as SENSITIVE when its content is plausibly "
            "private or embarrassing if overheard: 2FA / verification codes, "
            "specific banking amounts or account numbers, medical information, "
            "intimate or romantic messages, anything explicitly about something "
            "the user would not want a roommate or guest to hear. For sensitive "
            "ones, do NOT read the body unprompted -- say something like: \"Yes, "
            "you have a message from <sender>, but you might want to read it in "
            "private. Want me to read it anyway?\" and wait for confirmation.\n"
            "- Most casual messaging is NOT sensitive. A friend or family member "
            "asking about plans, sending a link, or making small talk is fine "
            "to read aloud directly. When in doubt and the content is clearly "
            "mundane, just read it.\n"
            "- Treat a notification as TIME-SENSITIVE only when the content "
            "itself is urgent: alarms going off now, calendar reminders for "
            "events starting soon, missed calls, delivery / ride ETAs in the "
            "next few minutes, severe-weather alerts. Promotional pushes, "
            "social-media engagement, news headlines, app updates, and most "
            "media notifications are NOT urgent.\n"
            "- For genuinely time-sensitive items you MAY proactively slip them "
            "in after answering the user's actual question: \"By the way, you "
            "have a time-sensitive notification from <sender>...\". If it's "
            "also sensitive, only mention that it exists and suggest the user "
            "check their phone.\n"
            "- Don't list every notification unless asked. Pick the relevant "
            "one(s) for the user's question.\n"
            "- If the user just said \"yes\", \"please\", or \"go ahead\" right "
            "after you offered to read a sensitive notification, read the body "
            "of the notification you offered.\n\n")) {
            free(buf);
            return NULL;
        }
    }

    if (!append_json_escaped(&p, end,
        "TOOLS:\n"
        "- You have these tools available:\n"
        "    [show.text]{ ...LaTeX... }     -- on-screen math / equations\n"
        "    [show.stock]{ TICKER | RANGE } -- on-screen live stock card (RANGE: 1d,1w,1m,6m,ytd,1y)\n"
        "    [store.memory]{ short fact }   -- save a durable fact about the user\n"
        "    [send.message]{ name | message } -- send a text message to a contact\n"
        "- Place tool calls at the VERY START of your response, before "
        "any spoken text. They are not heard by the user.\n"
        "- Multiple tools may be chained: e.g. "
        "[store.memory]{user's dog is named Rex} [show.text]{...} ...\n"
        "- Use a display tool (show.*) ONLY when the answer is best shown "
        "rather than spoken. Don't use one for plain prose.\n"
        "- show.text: renderer takes LaTeX. Supported: plain letters/digits, "
        "\\frac{a}{b}, \\sqrt{x}, x^{2}, x_{i}, parentheses, "
        "\\pm \\mp \\cdot \\times \\div \\ast, "
        "\\leq \\geq \\neq \\approx \\equiv \\sim \\propto, "
        "\\in \\notin \\subset \\supset \\cup \\cap \\emptyset, "
        "\\forall \\exists \\neg \\land \\lor \\implies \\iff, "
        "\\to \\rightarrow \\leftarrow \\Rightarrow \\Leftarrow \\mapsto, "
        "\\sum \\prod \\int \\oint \\partial \\nabla (with optional "
        "_{lower}^{upper} limits), \\infty \\angle \\degree \\prime, "
        "all Greek letters \\alpha .. \\omega and \\Gamma \\Delta \\Theta "
        "\\Lambda \\Xi \\Pi \\Sigma \\Phi \\Psi \\Omega (rendered as "
        "spelled-out names — the device font has no Greek glyphs but the "
        "names read clearly), function names \\sin \\cos \\tan \\log \\ln "
        "\\exp \\lim \\max \\min \\det etc., accents \\hat{x} \\bar{x} "
        "\\vec{x} \\tilde{x} \\dot{x} \\ddot{x}, \\overline{...} "
        "\\underline{...} \\overrightarrow{...} \\boxed{...}, "
        "\\text{...} \\mathbf{...} \\mathit{...} (no font swap on this "
        "hardware — content renders normally), \\left( \\right) and "
        "\\big( ... \\big), spacing \\, \\; \\quad \\qquad. "
        "NOT supported: matrices, multi-line equations, n-th roots "
        "(\\sqrt[3]{x} → still drawn but the [3] is dropped). Keep "
        "expressions compact — the round screen is small.\n"
        "- show.stock: pass a ticker symbol like AAPL, MSFT, NVDA. "
        "Optionally add a pipe and timeframe: [show.stock]{NVDA|ytd}. "
        "Available ranges: 1d (today, default), 1w, 1m, 6m, ytd, 1y. "
        "Match the range to the user's request -- 'how has Apple done "
        "this year' -> ytd, 'Tesla this month' -> 1m, 'NVDA today' -> "
        "1d. If no timeframe is mentioned, default to 1d. The card has "
        "buttons to switch ranges so don't overthink it. Don't try to "
        "read the actual price -- the card shows it.\n"
        "- store.memory: use ONLY for durable facts the user clearly wants "
        "remembered across sessions -- name, location, family / pets, "
        "long-term preferences (\"I'm vegetarian\", \"I work nights\"), or "
        "anything they explicitly ask you to remember. DO NOT use it for "
        "transient context like \"I'm going to the store later\". Keep the "
        "stored fact short, declarative, third-person about the user "
        "(\"User lives in Windsor, California\"). After storing, briefly "
        "confirm in the spoken response (\"Got it, I'll remember that\").\n"
        "- send.message: name is the contact's display name. message is the text "
        "to send. The pipe | separates name from message. The device shows the "
        "message for user confirmation before sending. Example:\n"
        "    user: \"text John that I'm running late\" -> "
        "[send.message]{John|I'm running late!} Sure, I'll send that message.\n"
        "- After any tool call, still speak a short natural sentence so "
        "the response isn't silent.\n"
        "- Examples:\n"
        "    user: \"what's the quadratic formula?\" -> "
        "[show.text]{x = \\frac{-b \\pm \\sqrt{b^{2} - 4ac}}{2a}} "
        "Here's the quadratic formula.\n"
        "    user: \"how's nvidia doing today?\" -> "
        "[show.stock]{NVDA} Here's the latest on NVIDIA.\n"
        "    user: \"show me apple stock this year\" -> "
        "[show.stock]{AAPL|ytd} Here's Apple's performance so far this year.\n"
        "    user: \"remember that I live in Windsor, California\" -> "
        "[store.memory]{User lives in Windsor, California} "
        "Got it, I'll remember that.\n"
        "    user: \"text John that I'm running late\" -> "
        "[send.message]{John|I'm running late!} Sure, I'll send that message.\n\n")) {
        free(buf);
        return NULL;
    }

    // Phone link status — tells Gemini exactly what to say when offline.
    const char *phoneSection;
    if (!ble_is_paired()) {
        phoneSection = "PHONE LINK: No phone is linked. Phone features like "
                       "notifications are not available. If the user asks about "
                       "notifications or phone features, suggest they pair their "
                       "phone in Settings > Phone Link.\n\n";
    } else if (ble_device_connected_raw() || wifi_is_connected()) {
        phoneSection = "PHONE LINK: Phone is connected, notifications are live.\n\n";
    } else {
        phoneSection = "PHONE LINK: Phone is NOT connected. If the user asks about "
                       "notifications, tell them their phone isn't connected so you "
                       "can't read notifications right now.\n\n";
    }
    if (!append_json_escaped(&p, end, phoneSection)) {
        free(buf);
        return NULL;
    }

    // Current local time (from SNTP + IP-geolocated TZ).  Gives Gemini
    // enough context to answer "what time is it?" / "what day is it?"
    // and to reason about timestamps in stored memories.  Skipped when
    // we don't have wall-clock time yet so we don't lie.
    {
        String localStr = time_sync_local_str();
        if (localStr.length()) {
            if (!append_json_escaped(&p, end, "CURRENT TIME: ") ||
                !append_json_escaped(&p, end, localStr) ||
                !append_json_escaped(&p, end, "\n\n")) {
                free(buf);
                return NULL;
            }
        }
    }

    // Stored permanent memories.  Treated as global background context —
    // not every memory is relevant to every query, but having them all
    // in the prompt lets Gemini draw on them when appropriate.
    {
        uint16_t pmCount = storage_perm_mem_count();
        if (pmCount == 0) {
            if (!append_json_escaped(&p, end,
                "STORED MEMORIES:\n(none yet)\n\n")) {
                free(buf);
                return NULL;
            }
        } else {
            if (!append_json_escaped(&p, end, "STORED MEMORIES:\n")) {
                free(buf);
                return NULL;
            }
            for (uint16_t i = 0; i < pmCount; i++) {
                PermMemoryEntry e;
                if (!storage_perm_mem_get(i, e)) continue;
                if (!append_json_escaped(&p, end, "- ") ||
                    !append_json_escaped(&p, end, e.text) ||
                    !append_json_escaped(&p, end, "\n")) {
                    free(buf);
                    return NULL;
                }
            }
            if (!append_json_escaped(&p, end, "\n")) {
                free(buf);
                return NULL;
            }
        }
    }

    // Current notifications block — only include when a phone is paired.
    if (ble_is_paired()) {
        if (!append_json_escaped(&p, end, "CURRENT PHONE NOTIFICATIONS:\n")) {
            free(buf);
            return NULL;
        }
        {
            String notifs = notifications_format_for_prompt();
            bool ok = append_json_escaped(&p, end, notifs);
            notifs = String();   // explicit free, don't wait for scope exit
            if (!ok) {
                free(buf);
                return NULL;
            }
        }
    }

    *outLen = (size_t)(p - buf);
    return buf;
}

static void gemini_do_ble() {
    String resp;
    bool ok;

    LOG1("[gemini-ble] starting, type=%s, connected=%d, paired=%d\n",
         gReq.textPrompt.length() > 0 ? "text" : "audio",
         (int)ble_is_connected(), (int)ble_is_paired());

    if (gReq.textPrompt.length() > 0) {
        ok = ble_send_text_request(gReq.textPrompt, gReq.apiKey, gReq.ttsKey, resp);
    } else {
        ok = ble_send_request(gReq.pcm, gReq.pcmBytes, gReq.apiKey, gReq.ttsKey, resp);
    }

    if (ok && resp.length() > 0) {
        // gResponse/gResponseReady already set by RX callback via gemini_set_early_response
        String userText = gReq.textPrompt.length() > 0 ? gReq.textPrompt : "[audio]";
        save_turn(userText, resp);
        LOG1("[gemini-ble] success: \"%s\"\n", resp.substring(0, 80).c_str());
    } else if (gCancel) {
        LOG1("[gemini-ble] cancelled\n");
    } else {
        LOG1("[gemini-ble] FAILED (ok=%d, resp.len=%u)\n", (int)ok, resp.length());
        mark_error("Gemini", ok ? "Empty response from proxy" : "Phone proxy failed");
    }
}

// Cleanup helper for the streaming pipeline — sends stop signal, waits for
// consumer, deletes queue, frees audio buffer if no audio was produced.
static void streaming_cleanup(bool freeAudio) {
    push_sentence(NULL, true);
    uint32_t t0 = millis();
    while (!gTtsConsumerDone && millis() - t0 < 5000) vTaskDelay(pdMS_TO_TICKS(50));
    if (!gTtsConsumerDone) {
        LOG1("[gemini] TTS consumer did not exit, force-clearing\n");
        gTtsConsumerTask = NULL;
    }
    if (freeAudio && gStreamAudioBuf) {
        audio_stop_play();
        free(gStreamAudioBuf);
        gStreamAudioBuf = NULL;
    }
    if (gSentenceQ) { vQueueDelete(gSentenceQ); gSentenceQ = NULL; }
}

static void gemini_do_work() {
    // Route via BLE phone proxy if WiFi is unavailable
    if (!wifi_is_connected() && ble_is_connected()) {
        LOG1("[gemini] using BLE phone proxy\n");
        gemini_do_ble();
        return;
    }

    const char *host = "generativelanguage.googleapis.com";
    uint8_t mi = storage_get_model();
    if (mi > 2) mi = 0;
    String url = String("https://") + host +
                 "/v1beta/models/" + GEMINI_MODELS[mi] +
                 ":streamGenerateContent?key=" + gReq.apiKey + "&alt=sse";

    String userPlainText;
    String userTextEsc;
    uint8_t *wavBuf = NULL;
    size_t   wavLen = 0;
    bool     isAudio = (gReq.textPrompt.length() == 0);

    if (!isAudio) {
        userPlainText = gReq.textPrompt;
        userTextEsc   = json_escape(gReq.textPrompt);
    } else {
        userPlainText = "[audio]";
        size_t origSamples = gReq.pcmBytes / 2;
        size_t dsSamples   = origSamples / 2;
        size_t dsPcmBytes  = dsSamples * 2;
        wavLen = 44 + dsPcmBytes;
        wavBuf = (uint8_t *)ps_malloc(wavLen);
        if (!wavBuf) {
            LOGLN1("[gemini] wav alloc failed");
            mark_error("Gemini", "Out of memory");
            return;
        }
        build_wav_header(wavBuf, dsPcmBytes, 8000);
        int16_t *dst = (int16_t *)(wavBuf + 44);
        for (size_t i = 0; i < dsSamples; i++)
            dst[i] = (gReq.pcm[i * 2] >> 1) + (gReq.pcm[i * 2 + 1] >> 1);
    }

    size_t sysPromptLen = 0;
    char *sysPromptEsc = build_system_prompt_psram(&sysPromptLen);
    if (!sysPromptEsc) {
        if (wavBuf) free(wavBuf);
        LOGLN1("[gemini] sysPrompt build failed");
        mark_error("Gemini", "Prompt build failed");
        return;
    }

    String hist = gemini_effective_history_fragment();

    size_t pLen = 0;
    char *payload = build_payload_psram(sysPromptEsc, sysPromptLen, hist,
                                        isAudio, wavBuf, wavLen,
                                        userTextEsc, &pLen);
    if (wavBuf) { free(wavBuf); wavBuf = NULL; }
    free(sysPromptEsc);
    sysPromptEsc = NULL;
    hist        = String();
    userTextEsc = String();

    if (!payload) {
        LOGLN1("[gemini] payload build failed");
        mark_error("Gemini", "Payload build failed");
        return;
    }
    if (gCancel) { free(payload); return; }

    // ── Allocate audio buffer for progressive playback ──
    const size_t AUDIO_BUF_SIZE = 768 * 1024;
    uint8_t *audioBuf = (uint8_t *)ps_malloc(AUDIO_BUF_SIZE);
    if (!audioBuf) {
        free(payload);
        mark_error("Gemini", "Audio buffer alloc failed");
        return;
    }
    gStreamAudioBuf = audioBuf;

    // ── Create sentence queue + TTS consumer on Core 1 ──
    gSentenceQ = xQueueCreate(SENTENCE_Q_LEN, sizeof(SentenceMsg));
    if (!gSentenceQ) {
        free(audioBuf); gStreamAudioBuf = NULL;
        free(payload);
        mark_error("Gemini", "Queue create failed");
        return;
    }

    TtsConsumerArgs *ttsArgs = new TtsConsumerArgs();
    ttsArgs->audioBuf     = audioBuf;
    ttsArgs->audioBufSize = AUDIO_BUF_SIZE;
    ttsArgs->ttsKey       = gReq.ttsKey;
    gTtsConsumerDone = false;
    gTtsWritePos     = 0;

    xTaskCreatePinnedToCore(tts_consumer_fn, "tts-c", 16384,
                            ttsArgs, 4, &gTtsConsumerTask, 1);

    LOG2("[gemini] streaming POST %u bytes\n", (unsigned)pLen);

    // ── Connect and stream SSE from Gemini (retry loop) ──
    const int MAX_TRIES = 3;
    static const uint32_t BACKOFF_MS[MAX_TRIES] = { 0, 500, 2000 };
    int code = -1;
    String fullText;
    String displayTool;
    String sideEffects;
    bool streamOk = false;

    for (int attempt = 0; attempt < MAX_TRIES && !gCancel; attempt++) {
        if (attempt > 0) {
            LOG1("[gemini] retry %d/%d (prev code=%d)\n",
                 attempt + 1, MAX_TRIES, code);
            if (gConsecutiveTransportFails >= 2)
                wifi_soft_recover("gemini");
            else
                vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS[attempt]));
        }

        IPAddress resolved;
        uint32_t dnsMs;
        if (!dns_check(host, resolved, &dnsMs)) {
            LOG1("[gemini] DNS failed (%ums)\n", dnsMs);
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }

        size_t largest = wait_for_heap("gemini", 500);
        if (largest < TLS_HEAP_MIN_LARGEST) {
            LOG1("[gemini] heap too low (%u)\n", (unsigned)largest);
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(15);

        HTTPClient http;
        http.setTimeout(30000);
        http.setConnectTimeout(15000);
        http.setReuse(false);

        if (!http.begin(client, url)) {
            LOG1("[gemini] http.begin failed\n");
            gConsecutiveTransportFails++;
            code = -1;
            continue;
        }
        http.addHeader("Content-Type", "application/json");

        uint32_t postT0 = millis();
        code = http.POST((uint8_t *)payload, pLen);
        uint32_t postMs = millis() - postT0;

        if (gCancel) { http.end(); break; }

        if (code <= 0) {
            log_https_failure("gemini", code, client, postMs);
            gConsecutiveTransportFails++;
            http.end();
            continue;
        }

        gConsecutiveTransportFails = 0;

        if (code != 200) {
            String errBody = http.getString();
            LOG1("[gemini] HTTP %d: %.500s\n", code, errBody.c_str());
            http.end();
            if (code < 500) break;
            continue;
        }

        LOG2("[gemini] SSE stream started (headers in %ums)\n", postMs);

        // getSize() returns -1 for chunked transfer encoding (unknown length)
        bool isChunked = (http.getSize() <= 0);

        WiFiClient *stream = http.getStreamPtr();
        streamOk = gemini_stream_sse(stream, isChunked, fullText,
                                      displayTool, sideEffects);

        http.end();
        break;
    }

    free(payload);
    payload = NULL;

    // ── Handle errors / cancellation ──
    if (gCancel) {
        streaming_cleanup(true);
        return;
    }

    if (!streamOk || fullText.length() == 0) {
        if (code <= 0) {
            char det[40];
            snprintf(det, sizeof(det), "Connection failed (%d)", code);
            mark_error("Gemini", det);
        } else if (code != 200) {
            char det[24];
            snprintf(det, sizeof(det), "HTTP %d", code);
            mark_error("Gemini", det);
        } else {
            mark_error("Gemini", "Empty response");
        }
        streaming_cleanup(true);
        return;
    }

    // ── Process response ──
    gToolInvocation = displayTool;

    String spoken;
    {
        String dt2, se2;
        spoken = extract_tool_invocation(fullText, dt2, se2);
    }
    gResponse      = spoken;
    gResponseReady = true;

    if (displayTool.length() > 0)
        LOG2("[gemini] display tool: %s\n", displayTool.c_str());
    if (sideEffects.length() > 0) {
        LOG2("[gemini] side effects: %.200s\n", sideEffects.c_str());
        gemini_dispatch_side_effects(sideEffects);
    }
    LOG2("[gemini] response: %s\n", spoken.c_str());
    save_turn(userPlainText, fullText);

    // ── Wait for TTS consumer to finish writing audio ──
    uint32_t t0 = millis();
    while (!gTtsConsumerDone && !gCancel && millis() - t0 < 60000)
        vTaskDelay(pdMS_TO_TICKS(50));

    if (gSentenceQ) { vQueueDelete(gSentenceQ); gSentenceQ = NULL; }

    // Transfer audio buffer ownership — progressive player may still be
    // reading from it.  The state machine will wait for audio_is_playing()
    // to go false before the next request, at which point it's safe.
    gAudioBuf   = (int16_t *)gStreamAudioBuf;
    gAudioBytes = gTtsWritePos;
    gStreamAudioBuf = NULL;
}

static void gemini_task_fn(void *) {
    LOG2("[gemini] task start, heap=%u\n", ESP.getFreeHeap());
    gemini_do_work();
    LOG2("[gemini] task end, heap=%u\n", ESP.getFreeHeap());
    event_log_printf(EVT_API_CALL, "gemini done err=%d h=%u",
                     (int)gError, ESP.getFreeHeap());
    gDone = true;
    gBusy = false;
    gTask = NULL;
    vTaskDelete(NULL);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void gemini_init() {
    gBusy = false;
    gDone = false;
    gError = false;
    gCancel = false;
    gResponse = "";
    gToolInvocation = "";
    gErrSubsys[0] = 0;
    gErrDetail[0] = 0;
}

void gemini_send_audio(const int16_t *pcm, size_t numBytes,
                       const String &apiKey, const String &ttsKey) {
    // If a cancelled task is still finishing, wait for it
    if (gBusy && gCancel) {
        uint32_t t0 = millis();
        while (gBusy && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (gBusy) return;

    if (gAudioBuf) { free(gAudioBuf); gAudioBuf = NULL; gAudioBytes = 0; }

    gReq.pcm        = pcm;
    gReq.pcmBytes   = numBytes;
    gReq.apiKey      = apiKey;
    gReq.ttsKey      = ttsKey;
    gReq.textPrompt  = "";

    gBusy          = true;
    gDone          = false;
    gError         = false;
    gCancel        = false;
    gResponseReady = false;
    gResponse      = "";
    gToolInvocation = "";

    event_log(EVT_API_CALL, "gemini audio req");
    dev_overlay_log("API: audio query");
    xTaskCreatePinnedToCore(gemini_task_fn, "gemini", 16384, NULL, 4, &gTask, 0);
}

void gemini_send_text(const String &text,
                      const String &apiKey, const String &ttsKey) {
    if (gBusy && gCancel) {
        uint32_t t0 = millis();
        while (gBusy && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (gBusy) return;

    if (gAudioBuf) { free(gAudioBuf); gAudioBuf = NULL; gAudioBytes = 0; }

    gReq.pcm         = NULL;
    gReq.pcmBytes    = 0;
    gReq.apiKey      = apiKey;
    gReq.ttsKey      = ttsKey;
    gReq.textPrompt  = text;

    gBusy          = true;
    gDone          = false;
    gError         = false;
    gCancel        = false;
    gResponseReady = false;
    gResponse      = "";
    gToolInvocation = "";

    event_log_printf(EVT_API_CALL, "gemini text: %.40s", text.c_str());
    {
        char _ovl[64];
        snprintf(_ovl, sizeof(_ovl), "API: %.50s", text.c_str());
        dev_overlay_log(_ovl);
    }
    xTaskCreatePinnedToCore(gemini_task_fn, "gemini", 16384, NULL, 4, &gTask, 0);
}

void gemini_cancel() {
    gCancel = true;
}

bool   gemini_is_busy()       { return gBusy; }
bool   gemini_is_done()       { return gDone; }
bool   gemini_is_error()      { return gError; }
const char *gemini_last_error_subsystem() {
    return gErrSubsys[0] ? gErrSubsys : "Network";
}
const char *gemini_last_error_detail() {
    return gErrDetail[0] ? gErrDetail : "Unknown";
}
bool   gemini_response_ready(){ return gResponseReady; }
String gemini_get_response()  { return gResponse; }
String gemini_get_tool_invocation() { return gToolInvocation; }

bool           gemini_has_audio()      { return gAudioBuf != NULL && gAudioBytes > 0; }

void gemini_set_early_response(const String &resp) {
    // Used by the BLE proxy path which doesn't go through gemini_do_work().
    // Strip any tool prefix so the early response also benefits from the
    // tool plumbing (TTS doesn't speak the tag, state machine sees it).
    //
    // Side-effect tools (store.*) are NOT dispatched here — when the
    // BLE proxy is in use, the Android side has already executed them
    // locally and pushed the resulting state changes (e.g. the new
    // memory) over BLE via OP_MEMORY_ADD.  Dispatching here too would
    // double-trigger the pill animation and double-store the entry.
    String tool;
    String sideEffects;
    String spoken = extract_tool_invocation(resp, tool, sideEffects);
    gToolInvocation = tool;
    gResponse = spoken;
    gResponseReady = true;
}
const int16_t *gemini_get_audio_pcm()  { return gAudioBuf; }
size_t         gemini_get_audio_bytes(){ return gAudioBytes; }

void gemini_clear() {
    gBusy = false;
    gDone = false;
    gError = false;
    gCancel = false;
    gResponseReady = false;
    gResponse = "";
    gToolInvocation = "";
    gErrSubsys[0] = 0;
    gErrDetail[0] = 0;
    if (gAudioBuf) { free(gAudioBuf); gAudioBuf = NULL; }
    gAudioBytes = 0;
    if (gStreamAudioBuf) { free(gStreamAudioBuf); gStreamAudioBuf = NULL; }
    if (gSentenceQ) { vQueueDelete(gSentenceQ); gSentenceQ = NULL; }
    gTtsWritePos = 0;
}

void gemini_clear_volatile_turn() {
    volatileUserText  = "";
    volatileModelText = "";
}

void gemini_test_tts(const String &text) {
    extern String storage_get_tts_key();
    String key = storage_get_tts_key();
    if (key.isEmpty()) { LOGLN1("[ttstest] no Deepgram key"); return; }
    gemini_clear();
    LOG1("[ttstest] sending \"%s\" to Deepgram...\n", text.c_str());
    do_tts(text, key);
}
