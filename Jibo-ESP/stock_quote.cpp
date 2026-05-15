#include "stock_quote.h"
#include "ble_link.h"
#include "log.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <string.h>

// ─── Threading model ────────────────────────────────────────────────────────
//
// Single in-flight fetch.  A short-lived FreeRTOS task does the HTTPS
// work so the UI thread (LVGL) never blocks on TLS handshake / DNS.
// Communication is via flag bytes and a single `gResult` struct, all
// synchronized by the busy/done flags themselves (no real concurrency:
// caller polls is_done() before reading the result, and start_fetch()
// rejects new requests while busy).

static volatile bool gBusy   = false;
static volatile bool gDone   = false;
static volatile bool gCancel = false;
static StockQuote    gResult;
static volatile StockRange gFetchRange = RANGE_1D;

// ─── Range helpers ─────────────────────────────────────────────────────────

const char *stock_range_label(StockRange r) {
    switch (r) {
        case RANGE_1D:  return "1D";
        case RANGE_1W:  return "1W";
        case RANGE_1M:  return "1M";
        case RANGE_6M:  return "6M";
        case RANGE_YTD: return "YTD";
        case RANGE_1Y:  return "1Y";
        default:        return "1D";
    }
}

static const char *yahoo_range(StockRange r) {
    switch (r) {
        case RANGE_1D:  return "1d";
        case RANGE_1W:  return "5d";
        case RANGE_1M:  return "1mo";
        case RANGE_6M:  return "6mo";
        case RANGE_YTD: return "ytd";
        case RANGE_1Y:  return "1y";
        default:        return "1d";
    }
}

static const char *yahoo_interval(StockRange r) {
    switch (r) {
        case RANGE_1D:  return "5m";
        case RANGE_1W:  return "15m";
        case RANGE_1M:  return "1h";
        case RANGE_6M:  return "1d";
        case RANGE_YTD: return "1d";
        case RANGE_1Y:  return "1d";
        default:        return "5m";
    }
}

StockRange stock_range_from_str(const char *s) {
    if (!s || !*s) return RANGE_1D;
    String r = String(s);
    r.trim();
    r.toLowerCase();
    if (r == "1d" || r == "day" || r == "today") return RANGE_1D;
    if (r == "1w" || r == "5d" || r == "week")   return RANGE_1W;
    if (r == "1m" || r == "1mo" || r == "month")  return RANGE_1M;
    if (r == "6m" || r == "6mo")                  return RANGE_6M;
    if (r == "ytd")                               return RANGE_YTD;
    if (r == "1y" || r == "year")                 return RANGE_1Y;
    return RANGE_1D;
}

// ─── Tiny inline JSON scanners ──────────────────────────────────────────────
//
// We avoid pulling in ArduinoJson — it's overkill for this single
// endpoint and its DynamicJsonDocument tends to fragment DRAM.  The
// Yahoo chart JSON is well-defined and the values we need are easy to
// pluck via substring scans, so brute-force is simpler and safer here.

// Find a value like `"key":<numeric>` AFTER `from` and return it as
// float.  Returns NAN on miss / parse failure.  Handles negative,
// decimal, and scientific notation.  `from` lets the caller scope the
// search inside a particular sub-object (e.g. `meta`) so we don't
// accidentally pick up e.g. `previousClose` from a different chunk.
static float json_find_number(const String &body, const char *key, int from = 0) {
    String needle = String("\"") + key + "\"";
    int p = body.indexOf(needle, from);
    if (p < 0) return NAN;
    p = body.indexOf(':', p + needle.length());
    if (p < 0) return NAN;
    p++;
    while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= (int)body.length()) return NAN;
    int q = p;
    while (q < (int)body.length()) {
        char c = body[q];
        bool isNum = (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' ||
                      (c >= '0' && c <= '9'));
        if (!isNum) break;
        q++;
    }
    if (q == p) return NAN;
    return body.substring(p, q).toFloat();
}

// Find a JSON array `"key":[...]` and copy up to `cap` numeric entries
// into `out`.  Returns the actual count read.  `null` entries (Yahoo
// emits these for off-hours / missing samples) are skipped, not stored
// — the resulting `out` has only valid prices.
static int json_find_number_array(const String &body, const char *key,
                                   float *out, int cap) {
    String needle = String("\"") + key + "\"";
    int p = body.indexOf(needle);
    if (p < 0) return 0;
    p = body.indexOf('[', p + needle.length());
    if (p < 0) return 0;
    p++;

    int count = 0;
    while (p < (int)body.length() && count < cap) {
        while (p < (int)body.length() && (body[p] == ' ' || body[p] == ',' ||
                                          body[p] == '\t' || body[p] == '\n' ||
                                          body[p] == '\r')) p++;
        if (p >= (int)body.length() || body[p] == ']') break;
        // Skip JSON literal null without aborting — Yahoo intersperses
        // these in the close array for any pre-market / post-market
        // sample that has no trade data yet.
        if (body[p] == 'n' && body.substring(p, p + 4) == "null") {
            p += 4;
            continue;
        }
        int q = p;
        while (q < (int)body.length()) {
            char c = body[q];
            bool isNum = (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' ||
                          (c >= '0' && c <= '9'));
            if (!isNum) break;
            q++;
        }
        if (q == p) break;
        out[count++] = body.substring(p, q).toFloat();
        p = q;
    }
    return count;
}

// ─── HTTPS helper ───────────────────────────────────────────────────────────

static String https_get(const String &url, const char *tag) {
    if (gCancel) return String();

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(8);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(8000);

    if (!http.begin(client, url)) {
        LOG1("[stock:%s] http.begin failed\n", tag);
        return String();
    }
    http.addHeader("Accept", "application/json");
    // Yahoo's chart endpoint is far less picky if we identify as a
    // browser — anonymous "Jibo/1.0" hits the same 200 response in
    // practice, but a real-looking UA avoids occasional 429 throttles.
    http.addHeader("User-Agent",
        "Mozilla/5.0 (compatible; Jibo/1.0; +https://github.com/)");

    uint32_t t0 = millis();
    int code = http.GET();
    uint32_t elapsed = millis() - t0;

    if (code != HTTP_CODE_OK) {
        char errBuf[96] = {0};
        client.lastError(errBuf, sizeof(errBuf));
        LOG1("[stock:%s] HTTP %d in %ums (%s)\n",
             tag, code, elapsed,
             errBuf[0] ? errBuf : HTTPClient::errorToString(code).c_str());
        http.end();
        return String();
    }

    String body = http.getString();
    http.end();
    LOG2("[stock:%s] %ums, %u bytes\n",
         tag, elapsed, (unsigned)body.length());
    return body;
}

// ─── Fetch task ─────────────────────────────────────────────────────────────

// ─── Direct (Wi-Fi) path ────────────────────────────────────────────────────
//
// Fetches Yahoo Finance directly when the device has its own internet.
// Returns true on success and populates gResult; on failure populates
// gResult.errorMsg and returns false.  Caller still decides whether to
// fall back to the BLE proxy.
static bool fetch_via_wifi(const char *symbol) {
    String url = "https://query1.finance.yahoo.com/v8/finance/chart/" +
                 String(symbol) + "?range=" + yahoo_range(gFetchRange) +
                 "&interval=" + yahoo_interval(gFetchRange);

    String body = https_get(url, "chart");
    if (gCancel) return false;

    if (body.length() == 0) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "Couldn't reach Yahoo Finance");
        return false;
    }
    if (body.indexOf("\"error\":{") >= 0) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "No data for %s", symbol);
        return false;
    }

    // Quote: scope the search to inside the `meta` object so we don't
    // accidentally read e.g. `previousClose` from a sibling block.
    int metaStart = body.indexOf("\"meta\":");
    int metaSearchFrom = (metaStart >= 0) ? metaStart : 0;

    float price = json_find_number(body, "regularMarketPrice", metaSearchFrom);
    float prev  = json_find_number(body, "chartPreviousClose",  metaSearchFrom);
    if (isnan(prev)) prev = json_find_number(body, "previousClose", metaSearchFrom);

    if (isnan(price) || price == 0.0f) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "No data for %s", symbol);
        return false;
    }

    gResult.current   = price;
    gResult.prevClose = isnan(prev) ? price : prev;
    gResult.change    = gResult.current - gResult.prevClose;
    gResult.changePercent = (gResult.prevClose > 0)
        ? (gResult.change / gResult.prevClose) * 100.0f
        : 0.0f;

    int n = json_find_number_array(body, "close", gResult.points,
                                    STOCK_GRAPH_POINTS);
    gResult.pointsCount = n;
    LOG1("[stock:wifi] %s: $%.2f (%+.2f%%), %d points\n",
         symbol, gResult.current, gResult.changePercent, n);
    if (n == 0) {
        LOG1("[stock:wifi] no close points (body head: %.80s)\n", body.c_str());
    }
    return true;
}

// ─── Phone proxy path ───────────────────────────────────────────────────────
//
// Asks the Android app to do the HTTPS fetch, parse it down to a
// compact JSON, and stream the result back over BLE.  Reuses the same
// inline scanners on the firmware side — the compact form intentionally
// uses a small subset of Yahoo's field names so we don't need a
// separate parser.
//
//   {"sym":"NVDA","price":143.5,"prev":138.0,"pts":[140.1,141.3,...]}
//
// Returns true on success and populates gResult; on failure sets
// gResult.errorMsg.
static bool fetch_via_ble(const char *symbol) {
    String resp;
    String err;
    // Include range in the BLE payload so the phone proxy fetches the
    // right timeframe.  Format: "SYMBOL|range_code" (e.g. "NVDA|ytd").
    String payload = String(symbol) + "|" + yahoo_range(gFetchRange);
    bool ok = ble_request_stock(payload, 10000, resp, err);
    if (gCancel) return false;
    if (!ok) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "%s", err.length() ? err.c_str() : "Phone proxy error");
        return false;
    }

    float price = json_find_number(resp, "price");
    float prev  = json_find_number(resp, "prev");
    if (isnan(price) || price == 0.0f) {
        // Phone may also send {"err":"..."} for soft errors.  Surface
        // it instead of the generic "no data" so the user knows what
        // went wrong (bad ticker vs. phone offline vs. Yahoo 5xx).
        int eq = resp.indexOf("\"err\":\"");
        if (eq >= 0) {
            int end = resp.indexOf('"', eq + 7);
            if (end > eq) {
                String e = resp.substring(eq + 7, end);
                snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                         "%s", e.c_str());
                return false;
            }
        }
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "No data for %s", symbol);
        return false;
    }

    gResult.current   = price;
    gResult.prevClose = isnan(prev) ? price : prev;
    gResult.change    = gResult.current - gResult.prevClose;
    gResult.changePercent = (gResult.prevClose > 0)
        ? (gResult.change / gResult.prevClose) * 100.0f
        : 0.0f;

    int n = json_find_number_array(resp, "pts", gResult.points,
                                    STOCK_GRAPH_POINTS);
    gResult.pointsCount = n;
    LOG1("[stock:ble] %s: $%.2f (%+.2f%%), %d points\n",
         symbol, gResult.current, gResult.changePercent, n);
    return true;
}

static void stock_task(void *arg) {
    char *symbol = (char *)arg;            // owned by us, freed below

    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
        ok = fetch_via_wifi(symbol);
        // If the direct path failed *and* the phone is still connected,
        // try the proxy as a fallback.  Captures the (rare) case where
        // Wi-Fi is associated but routes are flaky / DNS is broken /
        // Yahoo blacklisted our IP.
        if (!ok && !gCancel && ble_is_connected()) {
            LOG1("[stock] Wi-Fi fetch failed (%s), trying phone proxy\n",
                 gResult.errorMsg);
            // Wipe the Wi-Fi error so the proxy result speaks for itself.
            gResult.errorMsg[0] = '\0';
            ok = fetch_via_ble(symbol);
        }
    } else if (ble_is_connected()) {
        // No Wi-Fi but the phone is linked — happens whenever the user
        // hasn't joined a network on-device but is paired to their
        // phone.  Use the proxy directly; the user shouldn't see "no
        // internet" when there's a perfectly good route via BLE.
        ok = fetch_via_ble(symbol);
    } else {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "No internet connection");
    }

    if (gCancel) {
        free(symbol);
        gDone = true;
        gBusy = false;
        vTaskDelete(NULL);
        return;
    }

    gResult.valid = ok;
    if (ok) gResult.errorMsg[0] = '\0';
    free(symbol);
    gDone = true;
    gBusy = false;
    vTaskDelete(NULL);
}

// ─── Public API ─────────────────────────────────────────────────────────────

bool stock_is_busy() { return gBusy; }
bool stock_is_done() { return gDone; }

void stock_cancel() {
    if (gBusy) gCancel = true;
}

void stock_get_result(StockQuote &out) {
    out = gResult;
}

bool stock_start_fetch(const char *symbol, StockRange range) {
    if (gBusy) {
        LOGLN1("[stock] start: already busy");
        return false;
    }

    memset(&gResult, 0, sizeof(gResult));
    gResult.valid = false;
    gResult.range = range;
    gFetchRange   = range;

    if (!symbol || !*symbol) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg), "No symbol");
        gDone = true;
        return false;
    }

    // Normalize: strip whitespace, upper-case, clip to 11 chars.
    int j = 0;
    for (int i = 0; symbol[i] && j < (int)sizeof(gResult.symbol) - 1; i++) {
        char ch = symbol[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
        gResult.symbol[j++] = ch;
    }
    gResult.symbol[j] = '\0';
    if (j == 0) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg), "Bad symbol");
        gDone = true;
        return false;
    }

    // Don't reject up front for missing Wi-Fi — the worker task will
    // try the phone proxy as a fallback when only BLE is available.
    // Only error early if we have neither route at all.
    if (WiFi.status() != WL_CONNECTED && !ble_is_connected()) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg),
                 "No internet connection");
        gDone = true;
        return false;
    }

    char *symCopy = strdup(gResult.symbol);
    if (!symCopy) {
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg), "Out of memory");
        gDone = true;
        return false;
    }

    gBusy   = true;
    gDone   = false;
    gCancel = false;

    // 8 KB stack — TLS handshake inside HTTPClient peaks at ~6 KB.  Pinned
    // to core 0 to keep it off the LVGL flush thread on core 1.
    BaseType_t r = xTaskCreatePinnedToCore(
        stock_task, "stock", 8192, symCopy, 5, NULL, 0);
    if (r != pdPASS) {
        free(symCopy);
        gBusy = false;
        snprintf(gResult.errorMsg, sizeof(gResult.errorMsg), "Task create failed");
        gDone = true;
        return false;
    }
    return true;
}
