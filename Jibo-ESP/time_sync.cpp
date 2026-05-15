#include "time_sync.h"
#include "log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <ArduinoJson.h>

// ─── NVS-cached state ───────────────────────────────────────────────────────
//
// We persist three things between boots so the first memory after a
// reboot can still get a sensible timezone-aware timestamp without
// waiting for the IP-geolocation HTTP round-trip:
//
//   tz_name   : e.g. "America/Los_Angeles"
//   tz_posix  : POSIX TZ string e.g. "PST8PDT,M3.2.0/2,M11.1.0/2"
//   tz_offset : current UTC offset in seconds (snapshot — useful as a
//               crude fallback if we don't know how to parse a POSIX TZ
//               string and just need a quick offset).
//
// Note: ip-api.com returns `timezone` (the IANA name) and `offset`
// (current UTC offset in seconds at the time of the call).  It does
// NOT return the full POSIX TZ string with DST rules, so we synthesise
// one ourselves from the offset for non-DST quick-and-dirty handling.

static const char *NVS_NS = "tsync";

static String   gTzName;
static String   gTzPosix;
static int32_t  gTzOffsetSec = 0;
static bool     gTzKnown     = false;

static volatile bool gSyncInFlight  = false;
static volatile bool gWifiKickedOff = false;
static volatile bool gSntpDone      = false;

// ─── Helpers ────────────────────────────────────────────────────────────────

static void apply_tz_to_libc(const String &posix) {
    if (posix.length()) {
        setenv("TZ", posix.c_str(), 1);
        tzset();
    }
}

// Synthesise a basic POSIX TZ string from a UTC offset in seconds.
// We don't have DST rules from ip-api, so this is "fixed offset" —
// good enough for displaying "now in your timezone" at the moment of
// the lookup, and we re-fetch on every boot anyway.
static String posix_from_offset(int32_t offsetSec) {
    // POSIX TZ offsets are inverted (sign-flipped) and in HH[:MM] form.
    int32_t inv = -offsetSec;            // POSIX wants UTC-relative
    int hh = inv / 3600;
    int mm = (abs(inv) % 3600) / 60;
    char buf[32];
    if (mm == 0) snprintf(buf, sizeof(buf), "UTC%+d", hh);
    else         snprintf(buf, sizeof(buf), "UTC%+d:%02d", hh, mm);
    return String(buf);
}

static void load_cached_tz() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;
    gTzName     = p.getString("name", "");
    gTzPosix    = p.getString("posix", "");
    gTzOffsetSec = p.getInt("offset", 0);
    p.end();

    if (gTzPosix.length()) {
        apply_tz_to_libc(gTzPosix);
        gTzKnown = true;
        LOG1("[time] cached TZ: %s (%s, %+ds)\n",
             gTzName.c_str(), gTzPosix.c_str(), (int)gTzOffsetSec);
    }
}

static void save_cached_tz() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString("name", gTzName);
    p.putString("posix", gTzPosix);
    p.putInt("offset", gTzOffsetSec);
    p.end();
}

// ─── Background task ────────────────────────────────────────────────────────
//
// Runs once per boot after WiFi comes up.  Steps:
//   1. configTime() with pool.ntp.org — non-blocking fire-and-forget.
//   2. Poll time(NULL) for up to 8 s waiting for SNTP to land.
//   3. If we don't already have a cached TZ, hit ip-api.com to fetch one.
//
// Stack: 6 KB, priority 1, affined to whichever core has slack.

static void timesync_task(void *) {
    LOG1("[time] sync task starting\n");

    // (1) SNTP — uses libc settimeofday() under the hood when responses come back.
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    // (2) Wait for SNTP to land.  configTime returns immediately; the
    // actual response arrives from a background lwip callback.  We poll
    // time(NULL) until it crosses the "2020" sentinel.
    uint32_t startMs = millis();
    while (millis() - startMs < 8000) {
        time_t now = time(nullptr);
        if (now > 1577836800) {     // > Jan 1 2020 = real time
            gSntpDone = true;
            LOG1("[time] SNTP landed: %lld\n", (long long)now);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!gSntpDone) {
        LOG1("[time] SNTP timeout (8s) — clock will catch up later if NTP responds\n");
    }

    // (3) IP-based geolocation for TZ.  Skip if we've got a cached one
    // and SNTP succeeded (cached TZ is plenty good for memory display).
    if (!gTzKnown) {
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(5000);
        // ip-api.com is HTTP-only on the free tier; that's fine for
        // non-secret data like our public IP / coarse location.
        if (http.begin("http://ip-api.com/json/?fields=status,timezone,offset")) {
            int code = http.GET();
            if (code == 200) {
                String body = http.getString();
                StaticJsonDocument<512> doc;
                DeserializationError err = deserializeJson(doc, body);
                if (!err && String(doc["status"] | "") == "success") {
                    String tz       = doc["timezone"] | "";
                    int32_t offset  = doc["offset"] | 0;
                    if (tz.length()) {
                        gTzName      = tz;
                        gTzOffsetSec = offset;
                        gTzPosix     = posix_from_offset(offset);
                        apply_tz_to_libc(gTzPosix);
                        gTzKnown     = true;
                        save_cached_tz();
                        LOG1("[time] resolved TZ via ip-api: %s (%+ds) -> %s\n",
                             gTzName.c_str(), (int)gTzOffsetSec, gTzPosix.c_str());
                    }
                } else {
                    LOG1("[time] ip-api parse failed: %s\n", err.c_str());
                }
            } else {
                LOG1("[time] ip-api HTTP %d\n", code);
            }
            http.end();
        } else {
            LOG1("[time] ip-api begin() failed\n");
        }
    }

    gSyncInFlight = false;
    vTaskDelete(nullptr);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void time_sync_init() {
    load_cached_tz();
}

void time_sync_on_wifi_connected() {
    if (gWifiKickedOff) return;       // only first connection per boot
    gWifiKickedOff = true;
    if (gSyncInFlight) return;
    gSyncInFlight = true;
    xTaskCreatePinnedToCore(timesync_task, "tsync", 6144, nullptr, 1, nullptr,
                            tskNO_AFFINITY);
}

uint64_t time_sync_now_epoch_ms() {
    if (!gSntpDone && time(nullptr) <= 1577836800) return 0;
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

bool time_sync_clock_ok() {
    return time(nullptr) > 1577836800;
}

String time_sync_local_str() {
    if (!time_sync_clock_ok()) return "";
    time_t now = time(nullptr);
    struct tm tmLocal;
    localtime_r(&now, &tmLocal);

    // newlib's strftime (used by ESP-IDF) does NOT support the GNU
    // `%-d` / `%-I` "no-zero-pad" extensions — they fall through and
    // emit the literal characters or the padded form.  Build the
    // unpadded day/hour ourselves so we get e.g. "May 3 2026, 1:31 PM"
    // instead of "May 03 2026, 01:31 PM".
    char weekday[16] = {0};
    char month[16]   = {0};
    char ampm[8]     = {0};
    char tzAbbr[8]   = {0};
    strftime(weekday, sizeof(weekday), "%A", &tmLocal);
    strftime(month,   sizeof(month),   "%B", &tmLocal);
    strftime(ampm,    sizeof(ampm),    "%p", &tmLocal);
    strftime(tzAbbr,  sizeof(tzAbbr),  "%Z", &tmLocal);

    int hour12 = tmLocal.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;

    char buf[96];
    snprintf(buf, sizeof(buf), "%s, %s %d %d, %d:%02d %s%s%s",
             weekday, month, tmLocal.tm_mday, tmLocal.tm_year + 1900,
             hour12, tmLocal.tm_min, ampm,
             tzAbbr[0] ? " " : "", tzAbbr);

    String s(buf);
    if (gTzName.length()) {
        s += " (";
        s += gTzName;
        s += ")";
    }
    return s;
}

String time_sync_tz_name() {
    return gTzName;
}
