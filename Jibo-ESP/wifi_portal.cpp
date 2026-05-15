#include "wifi_portal.h"
#include "storage.h"
#include "time_sync.h"
#include "event_log.h"
#include "log.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_eap_client.h>

// HTML escape helper — only the bare minimum needed for value="…"
// attributes.  We never put user-supplied text in HTML body context, so
// & < > " is enough; ' is safe inside double-quoted attributes.
static String html_escape(const String &s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

static DNSServer   *dns   = NULL;
static WebServer   *server = NULL;
static bool         saved  = false;
static bool         running = false;
// When true, the portal was launched from in-device Settings (not first-
// boot setup): /save shows an inline banner and keeps the page live, the
// HTML pre-fills with the currently-saved values, and the new "Optional
// APIs" / Exit button are surfaced.  When false, the original
// minimal-friction first-boot flow is used unchanged.
static bool         settingsMode    = false;
static bool         exitRequested   = false;

// ─── HTML page (stored in PROGMEM) ──────────────────────────────────────────
static const char PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jibo Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#fff;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
     display:flex;justify-content:center;padding:24px;min-height:100vh}
.wrap{max-width:420px;width:100%}
h1{text-align:center;font-size:1.6rem;margin-bottom:8px;font-weight:500;letter-spacing:1px}
.sub{text-align:center;color:#888;font-size:.85rem;margin-bottom:28px}
.section{margin-bottom:22px}
.section-title{font-size:.8rem;text-transform:uppercase;letter-spacing:1.5px;color:#666;margin-bottom:10px}
.net{background:#111;border:1px solid #333;border-radius:10px;padding:14px;margin-bottom:10px}
.net-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}
.net-head span{font-size:.85rem;color:#aaa}
label{display:block;font-size:.78rem;color:#999;margin-bottom:4px;margin-top:10px}
input[type=text],input[type=password]{width:100%;background:#1a1a1a;border:1px solid #333;
  border-radius:6px;padding:10px 12px;color:#fff;font-size:.9rem;outline:none}
input[type=text]:focus,input[type=password]:focus{border-color:#fff}
.ent-row{display:flex;align-items:center;gap:8px;margin-top:8px}
.ent-row input[type=checkbox]{width:16px;height:16px;accent-color:#fff}
.ent-row label{margin:0;font-size:.8rem;color:#bbb}
.user-field{display:none;margin-top:6px}
.user-field.show{display:block}
.api-box{background:#111;border:1px solid #333;border-radius:10px;padding:14px}
button{display:block;width:100%;margin-top:28px;padding:14px;background:#fff;color:#000;
  border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;
  letter-spacing:.5px;transition:opacity .2s}
button:active{opacity:.7}
.add-btn{background:transparent;border:1px dashed #444;color:#666;margin-top:0;
  border-radius:10px;padding:10px;font-size:.85rem;font-weight:400;cursor:pointer}
.add-btn:hover{border-color:#888;color:#aaa}
</style>
</head>
<body>
<div class="wrap">
<h1>Jibo</h1>
<p class="sub">Device Setup</p>

<form method="POST" action="/save">

<div class="section">
<div class="section-title">Wi-Fi Networks</div>
<div id="nets"></div>
<div class="add-btn" onclick="addNet()">+ Add Network</div>
</div>

<div class="section">
<div class="section-title">AI Configuration</div>
<div class="api-box">
<label for="api">Gemini API Key</label>
<input type="password" id="api" name="api_key" placeholder="Enter your Gemini key" autocomplete="off">
<label for="tts" style="margin-top:12px">Deepgram TTS Key</label>
<input type="password" id="tts" name="tts_key" placeholder="Enter your Deepgram key" autocomplete="off">
<p style="color:#555;font-size:.7rem;margin-top:6px">Free at deepgram.com</p>
</div>
</div>

<button type="submit">Save</button>
</form>
</div>

<script>
let nc=0;
function addNet(){
 if(nc>=5)return;
 let i=nc++;
 let d=document.createElement('div');d.className='net';
 d.innerHTML=`
  <div class="net-head"><span>Network ${i+1}</span></div>
  <label>SSID</label>
  <input type="text" name="ssid_${i}" placeholder="Network name">
  <label>Password</label>
  <input type="password" name="pass_${i}" placeholder="Password">
  <div class="ent-row">
   <input type="checkbox" id="ent_${i}" name="ent_${i}" value="1"
    onchange="document.getElementById('uf_${i}').classList.toggle('show',this.checked)">
   <label for="ent_${i}">Enterprise network</label>
  </div>
  <div class="user-field" id="uf_${i}">
   <label>Username</label>
   <input type="text" name="user_${i}" placeholder="Enterprise username">
  </div>`;
 document.getElementById('nets').appendChild(d);
}
addNet();
</script>
</body>
</html>
)rawhtml";

// ─── Settings-mode HTML builder ─────────────────────────────────────────────
//
// Builds the same form as the first-boot PROGMEM page but with the existing
// stored values pre-populated, the new "Optional APIs" section, and an
// Exit button that leaves the portal without changing anything.  Built on
// the fly because the values come from NVS at request time.  The result is
// only a few KB so we don't bother streaming it.

static String build_settings_page() {
    // Top of the document — identical to the first-boot page.
    String html;
    html.reserve(8192);
    html += F(
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Jibo Setup</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#000;color:#fff;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "display:flex;justify-content:center;padding:24px;min-height:100vh}"
        ".wrap{max-width:420px;width:100%}"
        "h1{text-align:center;font-size:1.6rem;margin-bottom:8px;font-weight:500;letter-spacing:1px}"
        ".sub{text-align:center;color:#888;font-size:.85rem;margin-bottom:28px}"
        ".section{margin-bottom:22px}"
        ".section-title{font-size:.8rem;text-transform:uppercase;letter-spacing:1.5px;color:#666;margin-bottom:10px}"
        ".net{background:#111;border:1px solid #333;border-radius:10px;padding:14px;margin-bottom:10px}"
        ".net-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}"
        ".net-head span{font-size:.85rem;color:#aaa}"
        "label{display:block;font-size:.78rem;color:#999;margin-bottom:4px;margin-top:10px}"
        "input[type=text],input[type=password]{width:100%;background:#1a1a1a;border:1px solid #333;"
        "border-radius:6px;padding:10px 12px;color:#fff;font-size:.9rem;outline:none}"
        "input[type=text]:focus,input[type=password]:focus{border-color:#fff}"
        ".ent-row{display:flex;align-items:center;gap:8px;margin-top:8px}"
        ".ent-row input[type=checkbox]{width:16px;height:16px;accent-color:#fff}"
        ".ent-row label{margin:0;font-size:.8rem;color:#bbb}"
        ".user-field{display:none;margin-top:6px}"
        ".user-field.show{display:block}"
        ".api-box{background:#111;border:1px solid #333;border-radius:10px;padding:14px}"
        "button{display:block;width:100%;margin-top:14px;padding:14px;background:#fff;color:#000;"
        "border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;letter-spacing:.5px;"
        "transition:opacity .2s}"
        "button:active{opacity:.7}"
        "button.secondary{background:#222;color:#fff;border:1px solid #444}"
        ".add-btn{background:transparent;border:1px dashed #444;color:#666;margin-top:0;"
        "border-radius:10px;padding:10px;font-size:.85rem;font-weight:400;cursor:pointer}"
        ".add-btn:hover{border-color:#888;color:#aaa}"
        ".banner{background:#0d3a1f;border:1px solid #1f6b3a;color:#7fe39c;padding:10px;"
        "border-radius:8px;margin-bottom:16px;font-size:.85rem;text-align:center;display:none}"
        ".banner.show{display:block}"
        ".note{color:#777;font-size:.78rem;text-align:center;margin-top:8px}"
        "</style></head><body>"
        "<div class=\"wrap\"><h1>Jibo</h1><p class=\"sub\">Settings Portal</p>"
        "<div id=\"savedBanner\" class=\"banner\">Saved.</div>"
        "<form id=\"f\" method=\"POST\" action=\"/save\">"
        "<div class=\"section\">"
        "<div class=\"section-title\">Wi-Fi Networks</div>"
        "<div id=\"nets\"></div>"
        "<div class=\"add-btn\" onclick=\"addNet('','',false,'')\">+ Add Network</div>"
        "</div>");

    // AI Configuration — pre-fill with stored values
    String apiKey = storage_get_api_key();
    String ttsKey = storage_get_tts_key();
    html += F(
        "<div class=\"section\"><div class=\"section-title\">AI Configuration</div>"
        "<div class=\"api-box\">"
        "<label for=\"api\">Gemini API Key</label>"
        "<input type=\"password\" id=\"api\" name=\"api_key\" autocomplete=\"off\" value=\"");
    html += html_escape(apiKey);
    html += F(
        "\">"
        "<label for=\"tts\" style=\"margin-top:12px\">Deepgram TTS Key</label>"
        "<input type=\"password\" id=\"tts\" name=\"tts_key\" autocomplete=\"off\" value=\"");
    html += html_escape(ttsKey);
    html += F(
        "\"><p style=\"color:#555;font-size:.7rem;margin-top:6px\">Free at deepgram.com</p>"
        "</div></div>"
        "<button type=\"submit\">Save</button>"
        "</form>"
        "<button type=\"button\" class=\"secondary\" onclick=\"exitPortal()\">Exit Portal</button>"
        "<p class=\"note\">Press Exit to close the portal and return to settings. "
        "Save keeps the portal open so you can change more.</p>"
        "</div>");

    // JS: pre-populate the network rows from NVS, plus AJAX for save/exit so
    // the page stays put.
    html += F(
        "<script>"
        "let nc=0;"
        "function addNet(ssid,pass,ent,user){"
          "let i=nc++;"
          "let d=document.createElement('div');d.className='net';"
          "d.innerHTML=`"
            "<div class=\"net-head\"><span>Network ${i+1}</span></div>"
            "<label>SSID</label>"
            "<input type=\"text\" name=\"ssid_${i}\" value=\"\" placeholder=\"Network name\">"
            "<label>Password</label>"
            "<input type=\"password\" name=\"pass_${i}\" value=\"\" placeholder=\"Password\">"
            "<div class=\"ent-row\">"
              "<input type=\"checkbox\" id=\"ent_${i}\" name=\"ent_${i}\" value=\"1\""
              " onchange=\"document.getElementById('uf_${i}').classList.toggle('show',this.checked)\">"
              "<label for=\"ent_${i}\">Enterprise network</label>"
            "</div>"
            "<div class=\"user-field\" id=\"uf_${i}\">"
              "<label>Username</label>"
              "<input type=\"text\" name=\"user_${i}\" value=\"\" placeholder=\"Enterprise username\">"
            "</div>`;"
          "document.getElementById('nets').appendChild(d);"
          "d.querySelector(`input[name=ssid_${i}]`).value=ssid||'';"
          "d.querySelector(`input[name=pass_${i}]`).value=pass||'';"
          "d.querySelector(`input[name=user_${i}]`).value=user||'';"
          "if(ent){"
            "d.querySelector(`#ent_${i}`).checked=true;"
            "d.querySelector(`#uf_${i}`).classList.add('show');"
          "}"
        "}"
        "document.getElementById('f').addEventListener('submit',function(e){"
          "e.preventDefault();"
          "fetch('/save',{method:'POST',body:new FormData(this)}).then(()=>{"
            "let b=document.getElementById('savedBanner');"
            "b.classList.add('show');setTimeout(()=>b.classList.remove('show'),2500);"
          "});"
        "});"
        "function exitPortal(){fetch('/exit',{method:'POST'});}");

    // Inject the saved networks as JS literal addNet() calls so the form
    // mirrors what's in NVS.
    uint8_t count = storage_wifi_count();
    if (count == 0) {
        html += F("addNet('','',false,'');");
    } else {
        for (uint8_t i = 0; i < count; i++) {
            WifiEntry e;
            if (!storage_get_wifi(i, e)) continue;
            html += "addNet(";
            // String literals — escape single quotes / backslashes.
            auto js = [](const String &s) -> String {
                String r = "'";
                for (size_t k = 0; k < s.length(); k++) {
                    char c = s[k];
                    if (c == '\\' || c == '\'') r += '\\';
                    if (c == '\n') { r += "\\n"; continue; }
                    if (c == '\r') continue;
                    r += c;
                }
                r += "'";
                return r;
            };
            html += js(e.ssid);       html += ",";
            html += js(e.password);   html += ",";
            html += e.enterprise ? "true" : "false"; html += ",";
            html += js(e.username);
            html += ");";
        }
    }
    html += F("</script></body></html>");
    return html;
}

// ─── Handlers ───────────────────────────────────────────────────────────────
static void handleRoot() {
    if (settingsMode) {
        server->send(200, "text/html", build_settings_page());
    } else {
        server->send(200, "text/html", PAGE);
    }
}

static void handleSave() {
    uint8_t count = 0;
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        String ssid = server->arg("ssid_" + String(i));
        if (ssid.isEmpty()) continue;
        WifiEntry e;
        e.ssid       = ssid;
        e.password   = server->arg("pass_" + String(i));
        e.enterprise = server->hasArg("ent_" + String(i));
        e.username   = server->arg("user_" + String(i));
        storage_set_wifi(count, e);
        count++;
    }
    storage_set_wifi_count(count);

    String apiKey = server->arg("api_key");
    if (!apiKey.isEmpty()) {
        storage_set_api_key(apiKey);
    }

    String ttsKey = server->arg("tts_key");
    if (!ttsKey.isEmpty()) {
        storage_set_tts_key(ttsKey);
    }

    if (settingsMode) {
        // AJAX endpoint — JS in the page shows the inline banner and keeps
        // the form populated.  Plain "ok" body so the fetch resolves.
        server->send(200, "text/plain", "ok");
    } else {
        server->send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<style>body{background:#000;color:#fff;display:flex;justify-content:center;"
            "align-items:center;height:100vh;font-family:sans-serif;text-align:center}"
            "</style></head><body><div><h2 style='font-weight:400'>Saved</h2>"
            "<p style='color:#888;margin-top:8px'>You can close this page.</p></div>"
            "</body></html>");
    }

    saved = true;
}

static void handleExit() {
    // Only meaningful in settings mode — first-boot uses save_received instead.
    exitRequested = true;
    server->send(200, "text/plain", "exiting");
}

static void handleNotFound() {
    server->sendHeader("Location", "http://192.168.4.1/", true);
    server->send(302, "text/plain", "");
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Internal: shared start path.  `inSettings` toggles between first-boot
// flow (saves + auto-exits) and the settings flow (saves stay on page,
// Exit button required).
static void portal_start_internal(const String &apName, const String &apPass,
                                  bool inSettings) {
    saved          = false;
    running        = true;
    settingsMode   = inSettings;
    exitRequested  = false;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName.c_str(), apPass.c_str());
    delay(200);
    LOG1("AP started (%s mode): %s / %s  IP: %s\n",
        inSettings ? "settings" : "first-boot",
        apName.c_str(), apPass.c_str(),
        WiFi.softAPIP().toString().c_str());

    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());

    server = new WebServer(80);
    server->on("/", HTTP_GET, handleRoot);
    server->on("/save", HTTP_POST, handleSave);
    server->on("/exit", HTTP_POST, handleExit);
    server->on("/generate_204", handleRoot);
    server->on("/fwlink", handleRoot);
    server->on("/connecttest.txt", handleRoot);
    server->on("/hotspot-detect.html", handleRoot);
    server->on("/canonical.html", handleRoot);
    server->onNotFound(handleNotFound);
    server->begin();
}

void portal_start(const String &apName, const String &apPass) {
    portal_start_internal(apName, apPass, false);
}

void portal_start_settings(const String &apName, const String &apPass) {
    portal_start_internal(apName, apPass, true);
}

void portal_stop() {
    if (server) { server->stop(); delete server; server = NULL; }
    if (dns)    { dns->stop();    delete dns;    dns    = NULL; }
    WiFi.softAPdisconnect(true);
    running = false;
    settingsMode = false;
    exitRequested = false;
}

void portal_tick() {
    if (!running) return;
    if (dns)    dns->processNextRequest();
    if (server) server->handleClient();
}

bool portal_save_received() {
    return saved;
}

bool portal_exit_requested() {
    return exitRequested;
}

// ═══════════════════════════════════════════════════════════════════════════
//  WiFi STA connection state machine
// ═══════════════════════════════════════════════════════════════════════════
//
// Connection happens in three phases:
//
//   1. PRESCAN     — async WiFi scan to learn which saved networks are
//                    actually in range.  Skipped if a recent prescan
//                    result is still on hand from wifi_boot_prescan().
//   2. CONNECTING  — issue WiFi.begin() to the strongest known + in-range
//                    candidate, locking on a specific BSSID when we have
//                    one (so a mesh / repeater picks the best AP, not
//                    whichever the IDF feels like).
//   3. ROAMING     — once associated, periodically rescan in the
//                    background and (a) hop to a stronger BSSID for the
//                    same SSID if a mesh has a better AP, or (b) jump to
//                    a different known SSID if the current one has fallen
//                    below the "about to drop" threshold and a stronger
//                    saved network is reachable.
//
// All three share one set of static state plus one helper for the radio
// reset that's needed between begin() calls (see wifi_radio_reset_for_connect).

// ─── Tunables ───────────────────────────────────────────────────────────────

static const uint32_t CONN_TIMEOUT_MS      = 12000;
// Inter-attempt delay so we don't spin through every saved network in a
// single tick if every begin() fails instantly.  Note the radio reset
// below is what *actually* fixes the "sta is connecting, return error"
// race; this gap is purely about pacing.
static const uint32_t CONN_RETRY_GAP_MS    = 600;
// Pre-scan timeout: how long we'll wait for the boot scan to come back
// before falling through to a sequential connect over the saved list.
// 5 s is well over the typical scan duration (~2 s on this board).
// 9 s gives generous margin even when BLE coexistence is hogging the
// radio.  Without BLE traffic an active scan finishes in ~2.6 s with
// our 200ms-per-channel setting; with BLE active during the scan it
// can stretch to 6–7 s because the WiFi driver yields slots to the
// Bluetooth controller.  5 s used to be enough until we added the BLE
// auto-reconnect behaviour that fires repeatedly at boot.
static const uint32_t PRESCAN_TIMEOUT_MS   = 9000;
// Per-channel dwell time for active scan.  Default Arduino-ESP32 is
// 300 ms which is conservative; 200 ms still picks up every standard
// AP (their beacon interval is ~102 ms) and finishes the full 13-
// channel sweep noticeably faster.  Lower values risk missing weak
// APs that beacon irregularly.
static const uint32_t SCAN_DWELL_MS        = 200;
// How fresh a wifi_boot_prescan() result must be to skip re-scanning.
static const uint32_t PRESCAN_FRESH_MS     = 8000;

// Roam tunables.
//
// Scan cadence is graded by current signal strength so that:
//   - On a strong AP we barely scan (saves the radio for actual traffic).
//   - On a marginal AP we scan often enough that "walking into a new
//     room" gets noticed within a few seconds.
//   - On a critical AP we hammer the scan so we hop the moment a
//     better BSSID becomes visible.
//
// Switch threshold (delta over current RSSI) is *also* graded — when
// already strong, demand a big improvement (avoids flapping between two
// nearly-equal APs); when already weak, accept any meaningful gain.
static const uint32_t ROAM_INTERVAL_STRONG_MS    = 30000;  // RSSI > -55
static const uint32_t ROAM_INTERVAL_HEALTHY_MS   = 12000;  // -55 .. -65
static const uint32_t ROAM_INTERVAL_FAIR_MS      = 6000;   // -65 .. -72
static const uint32_t ROAM_INTERVAL_WEAK_MS      = 3500;   // -72 .. -80
static const uint32_t ROAM_INTERVAL_CRITICAL_MS  = 2000;   // < -80

// Hysteresis (improvement required to switch APs) — tied to current RSSI.
static const int32_t  ROAM_DELTA_STRONG          = 10;
static const int32_t  ROAM_DELTA_HEALTHY         = 6;
static const int32_t  ROAM_DELTA_FAIR            = 4;
static const int32_t  ROAM_DELTA_WEAK            = 3;
// To switch to a different SSID entirely we still demand a bit more —
// crossing networks means re-DHCP, etc.
static const int32_t  ROAM_DELTA_OTHER_SSID      = 8;
// Below this, switch to a different SSID even with a smaller delta —
// the current AP is functionally dead, anything is better.
static const int32_t  ROAM_OTHER_SSID_FALLBACK_RSSI = -82;

// IDF-side RSSI low event — fires (asynchronously, no polling cost) the
// moment the associated AP's RSSI drops below this value.  Lets us react
// instantly to "you just walked into another room" instead of waiting
// for the next scan-tick interval.  The threshold is one-shot per IDF
// docs; we re-arm it after every roam check.
static const int32_t  ROAM_RSSI_LOW_TRIGGER      = -65;

// Wait this long after we associate before we'll consider a roam — let
// DHCP / first traffic complete in peace.
static const uint32_t ROAM_GRACE_AFTER_CONNECT_MS = 5000;
static volatile bool  gRoamUrgent = false;     // set by event handler

// ─── State ──────────────────────────────────────────────────────────────────

// One per attempt the state machine will make.  Built from scan results
// (one per matched SSID, taking the strongest BSSID) when we have them;
// otherwise from the saved storage list directly.
struct ConnectCandidate {
    uint8_t storageIdx;       // index into storage
    int32_t rssi;             // 0 (= unknown) when no scan match
    uint8_t bssid[6];
    bool    hasBssid;
    int32_t channel;
};
static ConnectCandidate gCandidates[MAX_WIFI_NETWORKS];
static uint8_t          gCandidateCount = 0;
static uint8_t          gCandidateIdx   = 0;

static uint32_t connStartMs    = 0;
static bool     connActive     = false;
static bool     connFailed     = false;
static bool     connSingle     = false;   // true = trying just one network (no fallback)
static uint8_t  connLastError  = 0;       // 0=none, 1=auth, 2=no-ssid, 3=timeout
static uint32_t connRetryAtMs  = 0;

// Pre-scan tracking.
enum ConnectPhase { CP_IDLE, CP_PRESCAN, CP_CONNECTING };
static ConnectPhase gPhase            = CP_IDLE;
static uint32_t     gPrescanStartMs   = 0;
static bool         gPrescanResultReady = false;
static uint32_t     gPrescanResultMs    = 0;
// Who started the current WiFi.scanNetworks(true) call — needed because
// the settings UI also drives scans through wifi_scan_start(), and we
// don't want the roam loop to consume its results (or vice versa).
enum ScanOwner { SCAN_NONE, SCAN_OWNER_BOOT, SCAN_OWNER_UI, SCAN_OWNER_ROAM };
static ScanOwner gScanOwner = SCAN_NONE;

// Roam tracking.
static uint32_t gConnectedSinceMs = 0;
static uint32_t gNextRoamCheckMs  = 0;
static bool     gRoamScanInflight = false;

// ─── Radio reset (the "sta is connecting" fix) ─────────────────────────────

// Hard-reset the WiFi radio to clear any lingering STA_CONNECTING_BIT.
// WiFi.disconnect() is async and the IDF's auto-reconnect can re-arm
// STA_CONNECTING_BIT before the disconnect event is even processed, so
// simply waiting after a disconnect is not enough (we tried 600 ms;
// observed "wifi:sta is connecting, return error" anyway).  WIFI_OFF
// calls esp_wifi_stop() which IS synchronous and tears down all STA
// state.  ~50–100 ms cost per call.
static void wifi_radio_reset_for_connect() {
    WiFi.disconnect(false, true);     // eraseap=true — kill IDF's persisted SSID
    WiFi.mode(WIFI_OFF);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);     // we drive retries; the IDF doesn't get to
}

// ─── Event handler — surfaces auth/no-AP failures fast ─────────────────────

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        if (reason == WIFI_REASON_AUTH_FAIL ||
            reason == WIFI_REASON_AUTH_EXPIRE ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_MIC_FAILURE ||
            reason == WIFI_REASON_NOT_AUTHED ||
            reason == WIFI_REASON_ASSOC_EXPIRE ||
            reason == WIFI_REASON_ASSOC_FAIL) {
            connLastError = 1;
            if (connActive) connStartMs = 0;
        } else if (reason == WIFI_REASON_NO_AP_FOUND) {
            connLastError = 2;
            if (connActive) connStartMs = 0;
        }
    }
}

// The Arduino-ESP32 wrapper doesn't bridge WIFI_EVENT_STA_BSS_RSSI_LOW
// up into its own event enum (see NetworkEvents.h — the STA range stops
// at LOST_IP), so we register a raw IDF handler for it ourselves.
// Fires once per arming when the associated AP's RSSI drops below the
// threshold set by esp_wifi_set_rssi_threshold(); we re-arm in
// roam_tick() after each check.
static void on_idf_wifi_event(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
        gRoamUrgent = true;
    }
}

static bool wifiEventRegistered = false;
static void ensure_wifi_event_handler() {
    if (wifiEventRegistered) return;
    WiFi.onEvent(on_wifi_event);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW,
                               on_idf_wifi_event, NULL);
    wifiEventRegistered = true;
}

// ─── Candidate list construction ───────────────────────────────────────────
//
// Scan results from WiFi.scanComplete() are owned by the WiFi driver
// (don't free them).  We walk them once, finding the strongest BSSID
// per saved SSID (handles mesh: same SSID on multiple BSSIDs) and emit
// one ConnectCandidate per match.  Then sort by RSSI descending.

static void build_candidates_from_scan(int scanCount) {
    gCandidateCount = 0;

    uint8_t storedCount = storage_wifi_count();
    if (storedCount == 0) return;

    // For each saved network, find the strongest scan entry whose SSID
    // matches.  O(N*M) but both are tiny (5 saved × ~30 scan results).
    for (uint8_t s = 0; s < storedCount && gCandidateCount < MAX_WIFI_NETWORKS; s++) {
        WifiEntry e;
        if (!storage_get_wifi(s, e)) continue;
        if (e.ssid.length() == 0) continue;

        int      bestIdx  = -1;
        int32_t  bestRssi = -127;
        for (int i = 0; i < scanCount; i++) {
            if (WiFi.SSID(i) != e.ssid) continue;
            int32_t r = WiFi.RSSI(i);
            if (r > bestRssi) {
                bestRssi = r;
                bestIdx  = i;
            }
        }
        if (bestIdx < 0) continue;   // not in range right now

        ConnectCandidate &c = gCandidates[gCandidateCount++];
        c.storageIdx = s;
        c.rssi       = bestRssi;
        c.hasBssid   = true;
        c.channel    = WiFi.channel(bestIdx);
        const uint8_t *b = WiFi.BSSID(bestIdx);
        if (b) memcpy(c.bssid, b, 6);
        else { c.hasBssid = false; memset(c.bssid, 0, 6); }
    }

    // Sort candidates by RSSI descending — strongest first.
    for (uint8_t i = 1; i < gCandidateCount; i++) {
        ConnectCandidate tmp = gCandidates[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && gCandidates[j].rssi < tmp.rssi) {
            gCandidates[j + 1] = gCandidates[j];
            j--;
        }
        gCandidates[j + 1] = tmp;
    }

    LOG2("[wifi] prescan built %u candidates:\n", gCandidateCount);
    for (uint8_t i = 0; i < gCandidateCount; i++) {
        WifiEntry e;
        storage_get_wifi(gCandidates[i].storageIdx, e);
        LOG2("       %u. %s rssi=%d ch=%d\n",
             i, e.ssid.c_str(), (int)gCandidates[i].rssi,
             (int)gCandidates[i].channel);
    }
}

// Fall-through population: no scan available, just queue every saved
// network in saved order (the original behaviour).  Better than nothing
// when the prescan times out / fails — at least we'll try each entry.
static void build_candidates_from_storage() {
    gCandidateCount = 0;
    uint8_t n = storage_wifi_count();
    if (n > MAX_WIFI_NETWORKS) n = MAX_WIFI_NETWORKS;
    for (uint8_t i = 0; i < n; i++) {
        ConnectCandidate &c = gCandidates[gCandidateCount++];
        c.storageIdx = i;
        c.rssi       = 0;
        c.hasBssid   = false;
        c.channel    = 0;
        memset(c.bssid, 0, 6);
    }
    LOG1("[wifi] no prescan match — falling back to %u saved networks in order\n",
         gCandidateCount);
}

// ─── Begin one connect attempt ─────────────────────────────────────────────

static void wifi_begin_current() {
    if (gCandidateIdx >= gCandidateCount) {
        connStartMs = 0;
        connLastError = 3;
        return;
    }
    const ConnectCandidate &c = gCandidates[gCandidateIdx];
    WifiEntry e;
    if (!storage_get_wifi(c.storageIdx, e)) {
        connStartMs = 0;
        connLastError = 3;
        return;
    }

    // Reset the radio BEFORE begin().  Without this, a previous failed
    // attempt leaves STA_CONNECTING_BIT set and begin() is silently
    // rejected ("sta is connecting, return error") and we sit at
    // WL_DISCONNECTED until the 12 s timeout.
    wifi_radio_reset_for_connect();
    connLastError = 0;

    if (c.hasBssid) {
        LOG1("Trying %s (rssi=%d, ch=%d, bssid=%02X:%02X:%02X:%02X:%02X:%02X)\n",
             e.ssid.c_str(), (int)c.rssi, (int)c.channel,
             c.bssid[0], c.bssid[1], c.bssid[2],
             c.bssid[3], c.bssid[4], c.bssid[5]);
    } else {
        LOG1("Trying %s (no scan match — saved-order fallback)\n",
             e.ssid.c_str());
    }

    if (e.enterprise) {
        esp_eap_client_set_identity((uint8_t *)e.username.c_str(), e.username.length());
        esp_eap_client_set_username((uint8_t *)e.username.c_str(), e.username.length());
        esp_eap_client_set_password((uint8_t *)e.password.c_str(), e.password.length());
        esp_wifi_sta_enterprise_enable();
        // Enterprise begin doesn't take a BSSID overload, but enterprise
        // networks aren't typically mesh-with-shared-PSK anyway, so the
        // IDF's own AP selection is fine.
        WiFi.begin(e.ssid.c_str());
    } else if (c.hasBssid) {
        // BSSID-locked connect: tells the IDF "use *this* AP, not just
        // any AP advertising this SSID."  Critical for mesh networks.
        WiFi.begin(e.ssid.c_str(), e.password.c_str(),
                   (int32_t)c.channel, c.bssid);
    } else {
        WiFi.begin(e.ssid.c_str(), e.password.c_str());
    }
    connStartMs = millis();
}

// ─── Public connect API ────────────────────────────────────────────────────

void wifi_boot_prescan() {
    ensure_wifi_event_handler();
    // Just queue an async scan whose result wifi_start_connect() will
    // pick up if it lands in time.  Use the same radio-reset pattern
    // wifi_scan_start() uses so we're not racing any in-flight connect.
    if (gScanOwner != SCAN_NONE) return;   // somebody else is mid-scan
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.scanDelete();
    // Explicit dwell — async, no hidden, active, 200ms/chan, all chans.
    int ret = WiFi.scanNetworks(true, false, false, SCAN_DWELL_MS, 0);
    if (ret == WIFI_SCAN_RUNNING) {
        gScanOwner          = SCAN_OWNER_BOOT;
        gPrescanStartMs     = millis();
        gPrescanResultReady = false;
        LOGLN1("[wifi] boot prescan started");
    } else {
        LOG1("[wifi] boot prescan failed to queue (ret=%d)\n", ret);
    }
}

// Helper — drain whatever's in WiFi.scanComplete() into the candidate
// list and proceed to CP_CONNECTING.  Called from both wifi_start_connect
// (when a fresh prescan result was already on hand) and the CP_PRESCAN
// branch of wifi_connect_tick.
static void consume_prescan_and_connect() {
    int n = WiFi.scanComplete();
    if (n > 0) build_candidates_from_scan(n);
    // Even if the scan returned APs but none matched our saved
    // networks, try the saved list anyway — the user might have a
    // network whose AP was hidden / asleep / on a channel we missed.
    // The cost is a few wasted seconds of timeout per missing network,
    // which is exactly what the old behaviour did.
    if (gCandidateCount == 0) build_candidates_from_storage();

    // Done with the scan results either way — release them so future
    // scans aren't accidentally consuming stale entries.
    WiFi.scanDelete();
    gScanOwner          = SCAN_NONE;
    gPrescanResultReady = false;

    if (gCandidateCount == 0) {
        connActive = false;
        connFailed = true;
        gPhase = CP_IDLE;
        LOGLN1("[wifi] no candidates to try — giving up");
        return;
    }

    gPhase = CP_CONNECTING;
    gCandidateIdx = 0;
    wifi_begin_current();
}

void wifi_start_connect() {
    event_log(EVT_WIFI_EVENT, "connect start");
    ensure_wifi_event_handler();
    connActive    = true;
    connFailed    = false;
    connSingle    = false;
    connLastError = 0;
    connRetryAtMs = 0;
    gCandidateCount = 0;
    gCandidateIdx   = 0;

    if (storage_wifi_count() == 0) {
        connFailed = true;
        connActive = false;
        gPhase = CP_IDLE;
        return;
    }

    // Fast path: a recent boot prescan result is still on hand.  Two
    // ways this can be true: (a) wifi_scan_status() already latched
    // gPrescanResultReady when the UI happened to poll while the scan
    // was finishing, or (b) the scan is silently done because nobody
    // polled — in which case WiFi.scanComplete() will be >= 0 and we
    // can consume it directly here.
    if (gScanOwner == SCAN_OWNER_BOOT) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            consume_prescan_and_connect();
            return;
        }
        // Boot prescan is still running — don't kick off a duplicate.
        // Wait it out via wifi_connect_tick → CP_PRESCAN (timeout
        // PRESCAN_TIMEOUT_MS, which is what we want for stuck scans).
        gPhase = CP_PRESCAN;
        return;
    }
    if (gPrescanResultReady &&
        (millis() - gPrescanResultMs) < PRESCAN_FRESH_MS) {
        consume_prescan_and_connect();
        return;
    }

    // No fresh scan — kick one off.  If a UI/roam scan is in flight we
    // skip the prescan and fall through to saved-order, which still
    // works (it's just less optimal).
    if (gScanOwner == SCAN_NONE) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        // Brief settle — driver needs a few ms to fully shut down before
        // the next mode flip, otherwise the subsequent scan request can
        // get queued before the IDF actually has STA up and the scan
        // never starts (returns RUNNING but completes nothing in 5+ s).
        delay(20);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        delay(20);
        WiFi.scanDelete();
        // Explicit dwell so the scan completes promptly even when BLE
        // coex is stealing radio time.
        int ret = WiFi.scanNetworks(true, false, false, SCAN_DWELL_MS, 0);
        if (ret == WIFI_SCAN_RUNNING) {
            gScanOwner      = SCAN_OWNER_BOOT;
            gPrescanStartMs = millis();
            gPhase          = CP_PRESCAN;
            LOGLN1("[wifi] connect prescan started");
            return;
        }
        LOG1("[wifi] connect prescan failed to queue (ret=%d) — saved-order fallback\n", ret);
    }

    // Couldn't / didn't start a scan — go straight to sequential.
    build_candidates_from_storage();
    if (gCandidateCount == 0) {
        connFailed = true;
        connActive = false;
        gPhase = CP_IDLE;
        return;
    }
    gPhase = CP_CONNECTING;
    wifi_begin_current();
}

// ─── Roam / mesh background check ──────────────────────────────────────────

// Pick the scan interval and switch threshold based on current signal.
// Steeper grading on both axes than v1: we want to be on the strongest
// AP basically *immediately* once a better one is reachable.
static uint32_t roam_interval_for(int32_t rssi) {
    if (rssi >= -55) return ROAM_INTERVAL_STRONG_MS;
    if (rssi >= -65) return ROAM_INTERVAL_HEALTHY_MS;
    if (rssi >= -72) return ROAM_INTERVAL_FAIR_MS;
    if (rssi >= -80) return ROAM_INTERVAL_WEAK_MS;
    return ROAM_INTERVAL_CRITICAL_MS;
}
static int32_t roam_delta_for(int32_t rssi) {
    if (rssi >= -55) return ROAM_DELTA_STRONG;
    if (rssi >= -65) return ROAM_DELTA_HEALTHY;
    if (rssi >= -72) return ROAM_DELTA_FAIR;
    return ROAM_DELTA_WEAK;
}

// Re-arm the IDF's RSSI-low event so we get a future async kick when
// the current AP next drops below ROAM_RSSI_LOW_TRIGGER.  Threshold is
// one-shot per IDF docs — every time it fires, gRoamUrgent gets set
// once and we have to re-arm here.
static void roam_arm_rssi_event() {
    esp_wifi_set_rssi_threshold(ROAM_RSSI_LOW_TRIGGER);
}

// Find the best alternative for the currently-connected network and
// switch to it if it's meaningfully better.  Returns true if a roam
// was kicked off (caller should not re-arm RSSI events in that case —
// the connect path handles it on next CONNECTED).
static bool try_roam_from_scan(int scanCount) {
    String   curSsid  = WiFi.SSID();
    int32_t  curRssi  = WiFi.RSSI();
    const uint8_t *curBssidPtr = WiFi.BSSID();
    uint8_t  curBssid[6];
    if (curBssidPtr) memcpy(curBssid, curBssidPtr, 6);
    else             memset(curBssid, 0, 6);

    int32_t  bssidDelta = roam_delta_for(curRssi);

    // ── Mesh: stronger BSSID for the same SSID? ────────────────────────────
    int     bestSameIdx  = -1;
    int32_t bestSameRssi = curRssi;
    for (int i = 0; i < scanCount; i++) {
        if (WiFi.SSID(i) != curSsid) continue;
        const uint8_t *b = WiFi.BSSID(i);
        if (b && memcmp(b, curBssid, 6) == 0) continue;   // skip our own AP
        int32_t r = WiFi.RSSI(i);
        if (r > bestSameRssi + bssidDelta) {
            bestSameRssi = r;
            bestSameIdx  = i;
        }
    }
    if (bestSameIdx >= 0) {
        const uint8_t *b = WiFi.BSSID(bestSameIdx);
        LOG1("[wifi] roam: mesh BSSID hop "
             "(cur=%d new=%d delta=%d) ch=%d\n",
             (int)curRssi, (int)bestSameRssi, (int)bssidDelta,
             (int)WiFi.channel(bestSameIdx));
        // Find the storage index for this SSID.
        uint8_t s = 0;
        bool found = false;
        for (s = 0; s < storage_wifi_count(); s++) {
            WifiEntry e;
            if (storage_get_wifi(s, e) && e.ssid == curSsid) { found = true; break; }
        }
        if (!found) return false;
        gCandidateCount = 1;
        gCandidateIdx   = 0;
        gCandidates[0].storageIdx = s;
        gCandidates[0].rssi       = bestSameRssi;
        gCandidates[0].hasBssid   = true;
        gCandidates[0].channel    = WiFi.channel(bestSameIdx);
        if (b) memcpy(gCandidates[0].bssid, b, 6);
        connActive    = true;
        connFailed    = false;
        connSingle    = false;
        connLastError = 0;
        connRetryAtMs = 0;
        gPhase        = CP_CONNECTING;
        wifi_begin_current();
        return true;
    }

    // ── Other known SSID — switch when (a) current AP is critically weak,
    // or (b) the other SSID is meaningfully stronger than current.
    int     otherDelta = (curRssi <= ROAM_OTHER_SSID_FALLBACK_RSSI)
                            ? roam_delta_for(curRssi)   // be aggressive
                            : ROAM_DELTA_OTHER_SSID;    // normal hysteresis
    int     bestOtherIdx     = -1;
    int32_t bestOtherRssi    = curRssi + otherDelta;
    uint8_t bestOtherStorage = 0;
    for (uint8_t s = 0; s < storage_wifi_count(); s++) {
        WifiEntry e;
        if (!storage_get_wifi(s, e)) continue;
        if (e.ssid == curSsid) continue;
        for (int i = 0; i < scanCount; i++) {
            if (WiFi.SSID(i) != e.ssid) continue;
            int32_t r = WiFi.RSSI(i);
            if (r > bestOtherRssi) {
                bestOtherRssi    = r;
                bestOtherIdx     = i;
                bestOtherStorage = s;
            }
        }
    }
    if (bestOtherIdx < 0) return false;

    LOG1("[wifi] roam: cross-SSID switch "
         "(cur=%d new=%d delta=%d)\n",
         (int)curRssi, (int)bestOtherRssi, (int)otherDelta);
    const uint8_t *b = WiFi.BSSID(bestOtherIdx);
    gCandidateCount = 1;
    gCandidateIdx   = 0;
    gCandidates[0].storageIdx = bestOtherStorage;
    gCandidates[0].rssi       = bestOtherRssi;
    gCandidates[0].hasBssid   = (b != NULL);
    gCandidates[0].channel    = WiFi.channel(bestOtherIdx);
    if (b) memcpy(gCandidates[0].bssid, b, 6);
    connActive    = true;
    connFailed    = false;
    connSingle    = false;
    connLastError = 0;
    connRetryAtMs = 0;
    gPhase        = CP_CONNECTING;
    wifi_begin_current();
    return true;
}

// Issue a background scan to feed try_roam_from_scan().  Uses a short
// per-channel dwell so the radio is only "busy" for ~1 s instead of the
// 2–3 s a default scan takes — keeps audio glitches minimal even on the
// most aggressive roam interval.  No-op if somebody else owns the radio.
static void roam_kick_scan() {
    if (gScanOwner != SCAN_NONE) return;
    WiFi.scanDelete();
    // Args: async=true, show_hidden=false, passive=false,
    //       max_ms_per_chan=100 (vs default ~300, ~13 channels = ~1.3 s).
    int ret = WiFi.scanNetworks(true, false, false, 100);
    if (ret == WIFI_SCAN_RUNNING) {
        gScanOwner       = SCAN_OWNER_ROAM;
        gRoamScanInflight = true;
    }
}

static void roam_tick() {
    if (gPhase != CP_IDLE) return;             // busy connecting/scanning
    if (!wifi_is_connected()) return;
    if (gConnectedSinceMs == 0) {
        gConnectedSinceMs = millis();
        gNextRoamCheckMs  = millis() + ROAM_INTERVAL_HEALTHY_MS;
        roam_arm_rssi_event();
        return;
    }
    if (millis() - gConnectedSinceMs < ROAM_GRACE_AFTER_CONNECT_MS) return;

    // Drain any roam scan we kicked off previously.
    if (gRoamScanInflight) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;       // still going
        bool roamed = false;
        if (n >= 0) {
            roamed = try_roam_from_scan(n);
            WiFi.scanDelete();
        }
        gRoamScanInflight = false;
        gScanOwner        = SCAN_NONE;
        if (!roamed) {
            // Stayed put — schedule the next check based on how good
            // (or bad) the current signal is, and re-arm the
            // edge-triggered RSSI low event.
            int32_t r = WiFi.RSSI();
            gNextRoamCheckMs = millis() + roam_interval_for(r);
            roam_arm_rssi_event();
        }
        return;
    }

    // Urgent kick from the IDF's RSSI low event — scan immediately.
    if (gRoamUrgent) {
        gRoamUrgent = false;
        LOG1("[wifi] roam: RSSI low event triggered (cur=%d), scanning now\n",
             (int)WiFi.RSSI());
        roam_kick_scan();
        return;
    }
    if (millis() < gNextRoamCheckMs) return;
    roam_kick_scan();
}

// ─── Connect tick ──────────────────────────────────────────────────────────

void wifi_connect_tick() {
    // Phase 1: PRESCAN — wait for the boot scan to land, then build
    // candidates and move to CP_CONNECTING.
    if (gPhase == CP_PRESCAN) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (millis() - gPrescanStartMs >= PRESCAN_TIMEOUT_MS) {
                LOG1("[wifi] prescan timeout (%ums) — saved-order fallback\n",
                     (unsigned)(millis() - gPrescanStartMs));
                WiFi.scanDelete();
                gScanOwner = SCAN_NONE;
                build_candidates_from_storage();
                if (gCandidateCount == 0) {
                    connActive = false; connFailed = true; gPhase = CP_IDLE;
                    return;
                }
                gPhase = CP_CONNECTING;
                gCandidateIdx = 0;
                wifi_begin_current();
            }
            return;
        }
        // Scan finished (or failed with -2) — consume it.  build_*_from_scan
        // gracefully degrades to saved-order if n <= 0.
        consume_prescan_and_connect();
        return;
    }

    // Phase 3: ROAMING (when not actively connecting).
    if (!connActive) { roam_tick(); return; }

    // Phase 2: CONNECTING.  Wait out any inter-attempt gap first.
    if (connRetryAtMs != 0) {
        if (millis() < connRetryAtMs) return;
        connRetryAtMs = 0;
        wifi_begin_current();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        LOG1("Connected! IP: %s  rssi=%d  ssid=%s\n",
             WiFi.localIP().toString().c_str(),
             (int)WiFi.RSSI(), WiFi.SSID().c_str());
        event_log_printf(EVT_WIFI_EVENT, "connected %s %ddBm",
                         WiFi.SSID().c_str(), (int)WiFi.RSSI());
        esp_wifi_set_ps(WIFI_PS_NONE);
        LOG1("[wifi] modem sleep disabled\n");
        // Kick off NTP + IP-geolocated timezone resolution.  Idempotent
        // across reconnects; only does real work on the first connect of
        // this boot.  Without this, memory timestamps stay at "millis()
        // since boot" and the phone displays them as ~50 years ago.
        time_sync_on_wifi_connected();
        connActive       = false;
        connLastError    = 0;
        gPhase           = CP_IDLE;
        gConnectedSinceMs = millis();
        gNextRoamCheckMs  = millis() + roam_interval_for(WiFi.RSSI());
        gRoamUrgent       = false;
        // Edge-triggered: fires once when the AP next drops below
        // ROAM_RSSI_LOW_TRIGGER, and we re-arm after every check.
        roam_arm_rssi_event();
        return;
    }

    bool earlyFail = (connLastError != 0);
    if (!earlyFail && millis() - connStartMs < CONN_TIMEOUT_MS) return;
    if (!earlyFail) connLastError = 3;

    if (connSingle) {
        connActive = false;
        connFailed = true;
        wifi_radio_reset_for_connect();
        gPhase = CP_IDLE;
        LOG1("[wifi] single-target connect failed (reason=%u)\n", connLastError);
        return;
    }

    gCandidateIdx++;
    if (gCandidateIdx >= gCandidateCount) {
        connActive = false;
        connFailed = true;
        wifi_radio_reset_for_connect();
        gPhase = CP_IDLE;
        event_log(EVT_WIFI_EVENT, "all networks failed");
        LOGLN1("All WiFi networks failed.");
        return;
    }
    LOG1("[wifi] candidate %u failed (reason=%u), advancing to %u\n",
         (unsigned)(gCandidateIdx - 1), (unsigned)connLastError,
         (unsigned)gCandidateIdx);
    connLastError = 0;
    connRetryAtMs = millis() + CONN_RETRY_GAP_MS;
}

// ─── Other public helpers ──────────────────────────────────────────────────

bool wifi_is_connected() { return WiFi.status() == WL_CONNECTED; }
bool wifi_connect_failed() { return connFailed; }
uint8_t wifi_connect_error() { return connLastError; }

void wifi_disconnect() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    connActive       = false;
    connRetryAtMs    = 0;
    gPhase           = CP_IDLE;
    gConnectedSinceMs = 0;
    gRoamScanInflight = false;
    gRoamUrgent       = false;
    gScanOwner       = SCAN_NONE;
}

void wifi_enable() {
    storage_set_wifi_enabled(true);
    wifi_start_connect();
}

void wifi_disable() {
    storage_set_wifi_enabled(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    connActive       = false;
    connFailed       = false;
    connRetryAtMs    = 0;
    gPhase           = CP_IDLE;
    gConnectedSinceMs = 0;
    gRoamScanInflight = false;
    gRoamUrgent       = false;
    gScanOwner       = SCAN_NONE;
}

bool wifi_is_enabled() { return storage_get_wifi_enabled(); }

String wifi_current_ssid() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
//  WiFi scan API for the settings UI
// ═══════════════════════════════════════════════════════════════════════════

static bool scanPausedConnect = false;

bool wifi_scan_start() {
    // If a roam/boot scan is already going, cancel it so the UI gets a
    // fresh one (matches the user's expectation that "Search" actually
    // searches).
    if (gScanOwner != SCAN_NONE) {
        WiFi.scanDelete();
        gScanOwner        = SCAN_NONE;
        gRoamScanInflight = false;
        gPrescanResultReady = false;
    }
    WiFi.scanDelete();

    bool isConnected = (WiFi.status() == WL_CONNECTED);

    if (!isConnected) {
        // Same WIFI_OFF/STA cycle the connect path uses — clears
        // STA_CONNECTING_BIT so scanNetworks doesn't get refused.
        if (connActive) {
            scanPausedConnect = true;
            connActive        = false;
            connRetryAtMs     = 0;
            gPhase            = CP_IDLE;
        }
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        WiFi.mode(WIFI_STA);
    } else {
        WiFi.mode(WIFI_STA);
    }

    int ret = WiFi.scanNetworks(true, false, false, SCAN_DWELL_MS, 0);
    if (ret == WIFI_SCAN_RUNNING) gScanOwner = SCAN_OWNER_UI;
    LOG2("[wifi] UI scan start -> %d (paused_connect=%d, status=%d, was_connected=%d)\n",
         ret, (int)scanPausedConnect, WiFi.status(), (int)isConnected);
    return ret == WIFI_SCAN_RUNNING;
}

int wifi_scan_status() {
    int n = WiFi.scanComplete();
    // If a boot prescan completes while the UI is on the scan page (rare
    // race), latch it as "fresh" so a subsequent wifi_start_connect can
    // skip rescanning — but only when we don't *also* own the scan from
    // wifi_scan_start().
    if (n >= 0 && gScanOwner == SCAN_OWNER_BOOT) {
        gPrescanResultReady = true;
        gPrescanResultMs    = millis();
    }
    return n;
}

void wifi_resume_connect() {
    if (!scanPausedConnect) return;
    scanPausedConnect = false;
    if (wifi_is_enabled() && WiFi.status() != WL_CONNECTED) {
        wifi_start_connect();
    }
}

WifiScanResult wifi_scan_result(int i) {
    WifiScanResult r;
    r.ssid = WiFi.SSID(i);
    r.rssi = WiFi.RSSI(i);
    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    r.secured    = (auth != WIFI_AUTH_OPEN);
    r.enterprise = (auth == WIFI_AUTH_WPA2_ENTERPRISE
#ifdef WIFI_AUTH_WPA3_ENTERPRISE
                 || auth == WIFI_AUTH_WPA3_ENTERPRISE
#endif
                   );
    return r;
}

// ─── Deduped scan results (for the settings UI) ────────────────────────────
//
// A mesh / repeater advertises the same SSID across multiple BSSIDs.
// For the user, that's still "one network" — they don't care which AP
// they typed the password into.  These helpers walk the raw scan list
// and return one entry per unique SSID, taking the strongest BSSID as
// representative.  Implemented as an O(N²) scan because N is tiny
// (~30 max APs in range).

static int unique_count_cached_for = -1;
static int unique_count_cached     = 0;

static int compute_unique_count() {
    int total = WiFi.scanComplete();
    if (total < 0) return 0;
    if (total == unique_count_cached_for) return unique_count_cached;

    int unique = 0;
    for (int i = 0; i < total; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (WiFi.SSID(j) == WiFi.SSID(i)) { dup = true; break; }
        }
        if (!dup) unique++;
    }
    unique_count_cached_for = total;
    unique_count_cached     = unique;
    return unique;
}

int wifi_scan_count_unique() {
    return compute_unique_count();
}

WifiScanResult wifi_scan_result_unique(int idx) {
    WifiScanResult r{};
    int total = WiFi.scanComplete();
    if (total <= 0) return r;

    // Walk the list, skipping duplicates; when we land on the idx-th
    // unique entry, find the strongest BSSID for that SSID across all
    // scan results.
    int seen = 0;
    for (int i = 0; i < total; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (WiFi.SSID(j) == WiFi.SSID(i)) { dup = true; break; }
        }
        if (dup) continue;
        if (seen == idx) {
            String ssid = WiFi.SSID(i);
            int    bestI = i;
            int32_t bestR = WiFi.RSSI(i);
            for (int k = i + 1; k < total; k++) {
                if (WiFi.SSID(k) != ssid) continue;
                int32_t kr = WiFi.RSSI(k);
                if (kr > bestR) { bestR = kr; bestI = k; }
            }
            return wifi_scan_result(bestI);
        }
        seen++;
    }
    return r;
}

void wifi_scan_cleanup() {
    WiFi.scanDelete();
    unique_count_cached_for = -1;
    if (gScanOwner == SCAN_OWNER_UI) gScanOwner = SCAN_NONE;
    wifi_resume_connect();
}

// ─── Connect to specific saved network ─────────────────────────────────────

void wifi_connect_to(uint8_t idx) {
    ensure_wifi_event_handler();
    if (idx >= storage_wifi_count()) return;

    // Build a single-element candidate list for this slot.  No BSSID —
    // we let the IDF pick.  If a recent scan is on hand we *could* lift
    // the strongest BSSID for this SSID, but the manual connect path
    // is rare and the saved-order fallback is fine here.
    gCandidateCount = 1;
    gCandidateIdx   = 0;
    gCandidates[0].storageIdx = idx;
    gCandidates[0].rssi       = 0;
    gCandidates[0].hasBssid   = false;
    gCandidates[0].channel    = 0;
    memset(gCandidates[0].bssid, 0, 6);

    connActive    = true;
    connFailed    = false;
    connSingle    = true;
    connLastError = 0;
    connRetryAtMs = 0;
    gPhase        = CP_CONNECTING;
    LOG1("[wifi] connecting to slot %u\n", (unsigned)idx);
    wifi_begin_current();
}
