#include "ble_link.h"
#include "storage.h"
#include "audio.h"
#include "gemini.h"
#include "notifications.h"
#include "dev_console.h"
#include "event_log.h"
#include "log.h"

// Defined in gemini.cpp — uses persistent history when memory is enabled,
// otherwise the one-turn volatile pair so single follow-ups still work.
extern String gemini_effective_history_fragment();

// Triggered from gemini.cpp when a [store.memory] tool is executed on
// the firmware-direct path; declared here so the dispatcher can call it.
#include "pill_overlay.h"

#include <NimBLEDevice.h>

// ─── UUIDs ──────────────────────────────────────────────────────────────────
#define SERVICE_UUID        "a0e50000-0001-4d3b-8f1c-2e4a6b8c0d1e"
#define TX_CHAR_UUID        "a0e50001-0001-4d3b-8f1c-2e4a6b8c0d1e"  // Notify: J->P
#define RX_CHAR_UUID        "a0e50002-0001-4d3b-8f1c-2e4a6b8c0d1e"  // Write:  P->J
#define CTRL_CHAR_UUID      "a0e50003-0001-4d3b-8f1c-2e4a6b8c0d1e"  // R/W/N:  control

// ─── State ──────────────────────────────────────────────────────────────────
static NimBLEServer         *pServer   = NULL;
static NimBLECharacteristic *pTxChar   = NULL;
static NimBLECharacteristic *pRxChar   = NULL;
static NimBLECharacteristic *pCtrlChar = NULL;
static NimBLEAdvertising    *pAdv      = NULL;

static volatile bool deviceConnected  = false;
static volatile bool pairingMode      = false;
static volatile bool pairingWaiting   = false;  // waiting for phone to connect before showing code
static uint16_t     pairingCode       = 0;
static volatile bool pairingConfirmed = false;
static volatile bool advActive        = false;

static void ble_update_advertising();

static volatile bool reqBusy     = false;
static volatile bool reqCancel   = false;
static volatile bool reqDone     = false;
static volatile bool reqError    = false;
static String        respText;
static volatile bool respTextReady = false;
static volatile bool audioStarted  = false;

// ─── Stock proxy state ──────────────────────────────────────────────────────
//
// The stock-fetch helper is a separate request track from the main
// voice/text request channel: it can run while a Gemini exchange is
// already in flight (the "show.stock" tool fetch is kicked off as soon
// as Gemini's text response lands, which is well before the audio
// stream finishes).  Keeping its state independent avoids races
// between the two and means a slow phone-side HTTP fetch can't stall
// the next voice round-trip.
static volatile bool stockBusy   = false;
static volatile bool stockDone   = false;
static volatile bool stockError  = false;
static String        stockResp;     // accumulated compact JSON from phone
static String        stockErrMsg;   // populated when phone reports failure

// ─── Remote settings state ──────────────────────────────────────────────
static volatile bool settingsWriteReceived = false;
static String        settingsWritePayload;

// ─── Phone-assisted setup state ─────────────────────────────────────────
static volatile bool setupWifiReceived     = false;
static volatile bool setupKeysReceived     = false;
static volatile bool setupCompleteReceived = false;
static BleSetupWifi  setupWifi;   // last received SETUP_WIFI payload
static String        setupApiKey;
static String        setupTtsKey;

// ─── Helpers ────────────────────────────────────────────────────────────────

static void write_len_prefixed(uint8_t *&p, const String &s) {
    uint16_t len = s.length();
    *p++ = (uint8_t)(len & 0xFF);
    *p++ = (uint8_t)(len >> 8);
    if (len > 0) { memcpy(p, s.c_str(), len); p += len; }
}

// Send data over TX characteristic in MTU-sized chunks via notifications.
// Uses notify(data, len) to send data directly (no setValue race), and
// retries on queue-full to prevent silent notification drops.
static void ble_send_chunked(const uint8_t *data, size_t len) {
    if (!pTxChar || !deviceConnected) {
        LOG1("[ble-tx] abort: txChar=%p connected=%d\n", pTxChar, (int)deviceConnected);
        return;
    }
    size_t mtu = NimBLEDevice::getMTU() - 3;
    if (mtu < 20) mtu = 20;
    size_t expectedChunks = (len + mtu - 1) / mtu;
    LOG1("[ble-tx] sending %u bytes (MTU payload=%u, ~%u chunks)\n",
         (unsigned)len, (unsigned)mtu, (unsigned)expectedChunks);
    size_t pos = 0;
    size_t chunksSent = 0;
    int totalRetries = 0;
    uint32_t t0 = millis();
    while (pos < len && !reqCancel && deviceConnected) {
        size_t chunk = min(mtu, len - pos);

        // Send data directly via notify (bypasses setValue entirely).
        // Retry if the NimBLE notification queue is full.
        int retries = 0;
        while (!pTxChar->notify(data + pos, chunk) && retries < 100 && !reqCancel && deviceConnected) {
            vTaskDelay(pdMS_TO_TICKS(5));
            retries++;
        }
        if (retries >= 100) {
            LOG1("[ble-tx] STUCK at chunk %u (pos=%u), aborting\n",
                 (unsigned)chunksSent, (unsigned)pos);
            break;
        }
        totalRetries += retries;

        pos += chunk;
        chunksSent++;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    uint32_t elapsed = millis() - t0;
    LOG1("[ble-tx] sent %u/%u bytes, %u chunks in %ums (retries=%d)%s\n",
         (unsigned)pos, (unsigned)len, (unsigned)chunksSent, elapsed,
         totalRetries, reqCancel ? " CANCELLED" : "");
}

// ─── Server callbacks ───────────────────────────────────────────────────────

// Set when a MEMORY_LIST_REQ arrives over BLE.  Consumed by
// ble_link_tick() so the actual streaming happens on the main loop
// task, not the NimBLE callback context — iterating NVS while holding
// the BT stack would block the controller.
static volatile bool memSyncPending = false;

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
        deviceConnected = true;
        advActive = false;
        LOG1("[ble] connected: %s\n", info.getAddress().toString().c_str());
        event_log_printf(EVT_BLE_EVENT, "connected %s",
                         info.getAddress().toString().c_str());
        s->updateConnParams(info.getConnHandle(), 6, 8, 0, 400);

        if (pairingMode && pairingWaiting) {
            pairingWaiting = false;
            LOG1("[ble] phone connected during pairing, showing code\n");
        }
        // No auto-stream on connect — the phone polls the memory list
        // when the user actually opens the memories screen.  Saves BT
        // bandwidth on every reconnect for users who never look.
    }
    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
        deviceConnected = false;
        LOG1("[ble] disconnected (reason %d)\n", reason);
        event_log_printf(EVT_BLE_EVENT, "disconnected reason=%d", reason);
        if (reqBusy) { reqCancel = true; }
        // Re-advertise if paired (so phone can reconnect) or if in pairing mode
        ble_update_advertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
        LOG2("[ble] MTU changed to %u\n", mtu);
    }
};

// ─── RX characteristic callback (Phone -> Jibo data) ────────────────────────

static volatile uint32_t audioChunkCount = 0;
static volatile uint32_t audioTotalBytes = 0;

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
        NimBLEAttValue val = c->getValue();
        if (val.size() < 1) return;
        uint8_t op = val[0];

        switch (op) {
            case BLE_OP_TEXT_RESP: {
                respText += String((const char *)(val.data() + 1), val.size() - 1);
                respTextReady = true;
                gemini_set_early_response(respText);
                LOG1("[ble-rx] TEXT response (%u bytes, total %u): %.80s%s\n",
                     val.size() - 1, respText.length(), respText.c_str(),
                     respText.length() > 80 ? "..." : "");
                break;
            }
            case BLE_OP_AUDIO_CHUNK: {
                size_t pcmLen = val.size() - 1;
                if (!audioStarted) {
                    audio_stream_start();
                    audioStarted = true;
                    audioChunkCount = 0;
                    audioTotalBytes = 0;
                    LOG1("[ble-rx] audio stream started\n");
                }
                audio_stream_write(val.data() + 1, pcmLen);
                audioChunkCount++;
                audioTotalBytes += pcmLen;
                if ((audioChunkCount % 50) == 0) {
                    LOG1("[ble-rx] audio: %u chunks, %u bytes so far\n",
                         (unsigned)audioChunkCount, (unsigned)audioTotalBytes);
                }
                break;
            }
            case BLE_OP_DONE: {
                uint8_t status = (val.size() > 1) ? val[1] : 0;
                LOG1("[ble-rx] DONE (status=%d) textReady=%d audioStarted=%d audioBytes=%u\n",
                     status, (int)respTextReady, (int)audioStarted, (unsigned)audioTotalBytes);
                if (audioStarted) audio_stream_finish();
                if (status != 0) reqError = true;
                reqDone = true;
                break;
            }
            case BLE_OP_NOTIF_PUSH: {
                size_t n = val.size() - 1;
                LOG2("[ble-rx] NOTIF_PUSH (%u bytes)\n", (unsigned)n);
                if (n > 0) notifications_ingest_push(val.data() + 1, n);
                break;
            }
            case BLE_OP_NOTIF_REMOVE: {
                size_t n = val.size() - 1;
                if (n > 0) {
                    String key((const char *)(val.data() + 1), n);
                    LOG2("[ble-rx] NOTIF_REMOVE key=%s\n", key.c_str());
                    notifications_remove(key);
                }
                break;
            }
            case BLE_OP_MEMORY_ADD: {
                // [op][id8][ts8][len2][text...]
                // Phone-side Gemini wants us to persist a memory.  No
                // tombstones, no bidirectional reconciliation — we just
                // upsert and fire the pill if it's a brand-new id.
                if (val.size() < 1 + 8 + 8 + 2) break;
                const uint8_t *d = val.data() + 1;
                uint64_t id = 0;
                for (int k = 0; k < 8; k++) id |= (uint64_t)d[k] << (k * 8);
                uint64_t ts = 0;
                for (int k = 0; k < 8; k++) ts |= (uint64_t)d[8 + k] << (k * 8);
                uint16_t tlen = (uint16_t)d[16] | ((uint16_t)d[17] << 8);
                if (1 + 8 + 8 + 2 + tlen > val.size()) break;
                String text((const char *)(d + 18), tlen);

                bool wasNew = true;
                {
                    uint16_t cnt = storage_perm_mem_count();
                    for (uint16_t i = 0; i < cnt; i++) {
                        PermMemoryEntry e;
                        if (storage_perm_mem_get(i, e) && e.id == id) { wasNew = false; break; }
                    }
                }
                storage_perm_mem_add(id, ts, text);
                LOG1("[ble-rx] MEMORY_ADD id=%016llx %s text=%.80s\n",
                     (unsigned long long)id, wasNew ? "(new)" : "(update)", text.c_str());
                if (wasNew) {
                    // Pill fires when Jibo actually starts speaking the
                    // response (the state machine calls
                    // pill_release_pending on STATE_SPEAKING entry), so
                    // it lines up with the user hearing the answer.
                    char sub[40];
                    uint16_t total = storage_perm_mem_count();
                    snprintf(sub, sizeof(sub), "%u %s saved",
                             (unsigned)total,
                             total == 1 ? "memory" : "memories");
                    pill_queue_for_speaking(PILL_ICON_SUCCESS,
                                            "Saved to memory", sub);
                }
                break;
            }
            case BLE_OP_MEMORY_REMOVE: {
                if (val.size() < 1 + 8) break;
                const uint8_t *d = val.data() + 1;
                uint64_t id = 0;
                for (int k = 0; k < 8; k++) id |= (uint64_t)d[k] << (k * 8);
                bool removed = storage_perm_mem_remove(id);
                if (removed) {
                    LOG1("[ble-rx] MEMORY_REMOVE id=%016llx\n",
                         (unsigned long long)id);
                } else {
                    LOG2("[ble-rx] MEMORY_REMOVE id=%016llx (no-op)\n",
                         (unsigned long long)id);
                }
                break;
            }
            case BLE_OP_MEMORY_LIST_REQ: {
                LOG1("[ble-rx] MEMORY_LIST_REQ -- streaming list\n");
                memSyncPending = true;   // stream from main loop, off the BT cb
                break;
            }
            case BLE_OP_MEMORY_CLEAR: {
                LOG1("[ble-rx] MEMORY_CLEAR\n");
                storage_perm_mem_clear();
                break;
            }
            case BLE_OP_DEV_ENABLE: {
                if (!g_dev_mode) break;   // reject in stock mode
                bool en = (val.size() > 1 && val[1] != 0);
                LOG1("[ble-rx] DEV_ENABLE %d\n", (int)en);
                dev_console_set_enabled(en);
                break;
            }
            case BLE_OP_DEV_CMD: {
                if (!g_dev_mode) break;   // reject in stock mode
                size_t n = val.size() - 1;
                if (n > 0) {
                    LOG2("[ble-rx] DEV_CMD (%u bytes)\n", (unsigned)n);
                    dev_console_on_ble_cmd(val.data() + 1, n);
                }
                break;
            }
            case BLE_OP_STOCK_RESPONSE: {
                if (!stockBusy) {
                    LOG2("[ble-rx] STOCK_RESPONSE while idle, dropping\n");
                    break;
                }
                size_t n = val.size() - 1;
                if (n > 0) {
                    // Cap accumulated body at 4 KB — typical compact
                    // payload is ~700 B.  If the phone misbehaves and
                    // streams more we just stop appending; DONE will
                    // mark whatever we have done.
                    if (stockResp.length() + n <= 4096) {
                        stockResp.concat((const char *)(val.data() + 1), n);
                    }
                    LOG2("[ble-rx] STOCK_RESPONSE +%u bytes (total=%u)\n",
                         (unsigned)n, (unsigned)stockResp.length());
                }
                break;
            }
            case BLE_OP_STOCK_DONE: {
                if (!stockBusy) break;
                uint8_t status = (val.size() > 1) ? val[1] : 0;
                if (status != 0) {
                    stockError = true;
                    // Phone may attach a UTF-8 error message after the
                    // status byte.  Keep it short — the on-device
                    // error card has limited room.
                    if (val.size() > 2) {
                        stockErrMsg = String((const char *)(val.data() + 2),
                                             val.size() - 2);
                    }
                }
                stockDone = true;
                LOG1("[ble-rx] STOCK_DONE status=%u resp=%u bytes\n",
                     status, (unsigned)stockResp.length());
                break;
            }
            case BLE_OP_SETUP_WIFI: {
                // [op][1B enterprise][ssid\0][pass\0]  (+ [username\0] if enterprise)
                if (val.size() < 3) break;  // op + enterprise flag + at least one byte
                const uint8_t *d = val.data() + 1;
                size_t remain = val.size() - 1;
                bool enterprise = (d[0] != 0);
                d++; remain--;

                // Parse null-terminated strings safely
                const char *base = (const char *)d;
                const char *limit = base + remain;

                const char *ssidEnd = (const char *)memchr(base, '\0', remain);
                if (!ssidEnd) break;
                String ssid(base, ssidEnd - base);

                const char *passStart = ssidEnd + 1;
                if (passStart >= limit) break;
                size_t passRemain = limit - passStart;
                const char *passEnd = (const char *)memchr(passStart, '\0', passRemain);
                if (!passEnd) break;
                String password(passStart, passEnd - passStart);

                String username;
                if (enterprise) {
                    const char *userStart = passEnd + 1;
                    if (userStart < limit) {
                        size_t userRemain = limit - userStart;
                        const char *userEnd = (const char *)memchr(userStart, '\0', userRemain);
                        if (userEnd) {
                            username = String(userStart, userEnd - userStart);
                        }
                    }
                }

                setupWifi.ssid = ssid;
                setupWifi.password = password;
                setupWifi.username = username;
                setupWifi.enterprise = enterprise;
                setupWifiReceived = true;
                LOG1("[ble-rx] SETUP_WIFI ssid=%s enterprise=%d\n",
                     ssid.c_str(), (int)enterprise);
                break;
            }
            case BLE_OP_SETUP_KEYS: {
                // [op][gemini_key\0][tts_key\0]
                if (val.size() < 2) break;
                const char *base = (const char *)(val.data() + 1);
                size_t remain = val.size() - 1;
                const char *limit = base + remain;

                const char *gemEnd = (const char *)memchr(base, '\0', remain);
                if (!gemEnd) break;
                setupApiKey = String(base, gemEnd - base);

                const char *ttsStart = gemEnd + 1;
                if (ttsStart >= limit) { setupTtsKey = ""; }
                else {
                    size_t ttsRemain = limit - ttsStart;
                    const char *ttsEnd = (const char *)memchr(ttsStart, '\0', ttsRemain);
                    if (ttsEnd) {
                        setupTtsKey = String(ttsStart, ttsEnd - ttsStart);
                    } else {
                        setupTtsKey = String(ttsStart, ttsRemain);
                    }
                }

                setupKeysReceived = true;
                LOG1("[ble-rx] SETUP_KEYS apiKey=%u bytes, ttsKey=%u bytes\n",
                     setupApiKey.length(), setupTtsKey.length());
                break;
            }
            case BLE_OP_SETUP_COMPLETE: {
                setupCompleteReceived = true;
                LOG1("[ble-rx] SETUP_COMPLETE\n");
                break;
            }
            case BLE_OP_SETTINGS_REQ: {
                LOG1("[ble-rx] SETTINGS_REQ\n");
                ble_send_all_settings();
                break;
            }
            case BLE_OP_SETTINGS_WRITE: {
                if (val.size() > 1) {
                    settingsWritePayload = String((const char *)(val.data() + 1), val.size() - 1);
                    settingsWriteReceived = true;
                    LOG1("[ble-rx] SETTINGS_WRITE: %u bytes\n", val.size() - 1);
                }
                break;
            }
            case BLE_OP_PAIR: {
                LOG1("[ble-rx] PAIR frame, size=%u, pairingMode=%d\n", val.size(), (int)pairingMode);
                if (!pairingMode || val.size() < 3) break;
                uint16_t code = val[1] | (val[2] << 8);
                if (code == pairingCode) {
                    pairingConfirmed = true;
                    String addr = info.getAddress().toString().c_str();
                    storage_set_ble_addr(addr);
                    String name = (val.size() > 3)
                        ? String((const char *)(val.data() + 3), val.size() - 3)
                        : "Phone";
                    storage_set_ble_name(name);
                    storage_set_ble_paired(true);
                    LOG1("[ble] paired with %s (%s)\n", name.c_str(), addr.c_str());

                    uint8_t ack[] = { BLE_OP_PAIR, 0x01 };
                    pCtrlChar->setValue(ack, 2);
                    pCtrlChar->notify();
                } else {
                    LOG1("[ble] pairing code mismatch: got %u, expected %u\n", code, pairingCode);
                    uint8_t nack[] = { BLE_OP_PAIR, 0x00 };
                    pCtrlChar->setValue(nack, 2);
                    pCtrlChar->notify();
                }
                break;
            }
            default:
                LOG1("[ble-rx] unknown opcode 0x%02X (size=%u)\n", op, val.size());
                break;
        }
    }
};

// ─── Public API ─────────────────────────────────────────────────────────────

void ble_init() {
    NimBLEDevice::init("Jibo");
    NimBLEDevice::setMTU(512);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCB());

    NimBLEService *pSvc = pServer->createService(SERVICE_UUID);

    pTxChar = pSvc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pRxChar = pSvc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxChar->setCallbacks(new RxCB());

    pCtrlChar = pSvc->createCharacteristic(
        CTRL_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::NOTIFY
    );
    pCtrlChar->setCallbacks(new RxCB());

    pSvc->start();

    pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setName("Jibo");
    pAdv->enableScanResponse(true);

    // Start advertising only if already paired (for reconnection)
    if (storage_get_ble_paired()) {
        pAdv->start();
        advActive = true;
        LOG1("[ble] initialized, advertising for paired phone reconnect\n");
    } else {
        LOG1("[ble] initialized, NOT advertising (no paired device)\n");
    }
}

static bool lowPowerHold = false;   // true: keep advertising off across all paths

static void ble_update_advertising() {
    if (!pAdv) return;
    bool shouldAdv = !lowPowerHold && (pairingMode || storage_get_ble_paired());
    if (shouldAdv && !advActive && !deviceConnected) {
        pAdv->start();
        advActive = true;
        LOG1("[ble] advertising started (%s)\n",
             pairingMode ? "pairing" : "reconnect");
    } else if (!shouldAdv && advActive) {
        pAdv->stop();
        advActive = false;
        LOG1("[ble] advertising stopped\n");
    }
}

void ble_enter_low_power() {
    lowPowerHold = true;
    if (pAdv && advActive) {
        pAdv->stop();
        advActive = false;
    }
    // Drop any open link too — controller can't go fully idle while
    // a connection is alive.  Use disconnect with reason "remote user
    // terminated" so the phone shows a clean disconnect, not an error.
    if (pServer && deviceConnected) {
        std::vector<uint16_t> peers = pServer->getPeerDevices();
        for (uint16_t h : peers) pServer->disconnect(h);
    }
    LOG1("[ble] low-power: advertising halted\n");
}

void ble_exit_low_power() {
    lowPowerHold = false;
    ble_update_advertising();
    LOG1("[ble] low-power: advertising restored\n");
}

bool ble_is_paired() {
    return storage_get_ble_paired();
}

bool ble_is_connected() {
    return deviceConnected && storage_get_ble_paired();
}

int ble_read_rssi() {
    if (!pServer || !deviceConnected) return 0;
    std::vector<uint16_t> peers = pServer->getPeerDevices();
    if (peers.empty()) return 0;
    int8_t rssi = 0;
    int rc = ble_gap_conn_rssi(peers[0], &rssi);
    return (rc == 0) ? (int)rssi : 0;
}

String ble_phone_name() {
    return storage_get_ble_name();
}

void ble_start_pairing() {
    pairingCode = (uint16_t)(esp_random() % 9000 + 1000);
    pairingConfirmed = false;
    pairingWaiting = true;
    pairingMode = true;
    ble_update_advertising();
    LOG1("[ble] pairing started (waiting for phone), code: %u\n", pairingCode);
}

void ble_stop_pairing() {
    pairingMode = false;
    pairingWaiting = false;
    ble_update_advertising();
    LOG1("[ble] pairing stopped\n");
}

uint16_t ble_get_pairing_code() {
    return pairingCode;
}

bool ble_pairing_active() {
    return pairingMode;
}

bool ble_pairing_phone_connected() {
    return pairingMode && deviceConnected && !pairingWaiting && !pairingConfirmed;
}

bool ble_device_connected_raw() {
    return deviceConnected;
}

void ble_unpair() {
    storage_set_ble_paired(false);
    storage_set_ble_addr("");
    storage_set_ble_name("");
    pairingMode = false;
    pairingWaiting = false;
    pairingConfirmed = false;
    ble_update_advertising();
    LOG1("[ble] unpaired\n");
}

static bool ble_do_request(const uint8_t *payload, size_t payloadLen) {
    if (!deviceConnected) {
        LOG1("[ble] do_request: not connected\n");
        return false;
    }

    LOG1("[ble] do_request: starting, payload=%u bytes\n", (unsigned)payloadLen);

    reqBusy      = true;
    reqCancel    = false;
    reqDone      = false;
    reqError     = false;
    respText     = "";
    respTextReady = false;
    audioStarted  = false;
    audioChunkCount = 0;
    audioTotalBytes = 0;

    // Compute payload checksum for integrity verification
    uint32_t payloadCksum = 0;
    for (size_t i = 0; i < payloadLen; i++) payloadCksum += payload[i];
    LOG1("[ble] payload checksum=%u (%u bytes)\n", payloadCksum, (unsigned)payloadLen);

    ble_send_chunked(payload, payloadLen);

    if (reqCancel) {
        LOG1("[ble] do_request: cancelled during send\n");
        reqBusy = false;
        return false;
    }

    // Signal the phone that all request chunks have been sent.
    // Format: [op(1)] [checksum(4B LE)] [payloadLen(4B LE)]
    if (pCtrlChar && deviceConnected) {
        uint8_t ready[9];
        ready[0] = BLE_OP_REQ_READY;
        ready[1] = (uint8_t)(payloadCksum & 0xFF);
        ready[2] = (uint8_t)((payloadCksum >> 8) & 0xFF);
        ready[3] = (uint8_t)((payloadCksum >> 16) & 0xFF);
        ready[4] = (uint8_t)((payloadCksum >> 24) & 0xFF);
        ready[5] = (uint8_t)(payloadLen & 0xFF);
        ready[6] = (uint8_t)((payloadLen >> 8) & 0xFF);
        ready[7] = (uint8_t)((payloadLen >> 16) & 0xFF);
        ready[8] = (uint8_t)((payloadLen >> 24) & 0xFF);
        pCtrlChar->setValue(ready, 9);
        pCtrlChar->notify();
        LOG1("[ble] do_request: REQ_READY sent (checksum=%u, size=%u)\n",
             payloadCksum, (unsigned)payloadLen);
    }

    LOG1("[ble] do_request: payload sent, waiting for response...\n");

    uint32_t timeout = 60000;
    uint32_t t0 = millis();
    uint32_t lastLog = t0;
    while (!reqDone && !reqCancel && deviceConnected && millis() - t0 < timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t now = millis();
        if (now - lastLog >= 5000) {
            LOG1("[ble] waiting... %us elapsed, textReady=%d audioStarted=%d audioBytes=%u connected=%d\n",
                 (unsigned)((now - t0) / 1000), (int)respTextReady,
                 (int)audioStarted, (unsigned)audioTotalBytes, (int)deviceConnected);
            lastLog = now;
        }
    }

    uint32_t elapsed = millis() - t0;
    reqBusy = false;

    if (reqCancel) {
        LOG1("[ble] do_request: CANCELLED after %ums\n", elapsed);
        if (audioStarted) audio_stream_cancel();
        return false;
    }
    if (!deviceConnected) {
        LOG1("[ble] do_request: DISCONNECTED after %ums\n", elapsed);
        if (audioStarted) audio_stream_cancel();
        return false;
    }
    if (!reqDone) {
        LOG1("[ble] do_request: TIMEOUT after %ums (textReady=%d, audioBytes=%u)\n",
             elapsed, (int)respTextReady, (unsigned)audioTotalBytes);
        if (audioStarted) audio_stream_cancel();
        return false;
    }

    LOG1("[ble] do_request: completed in %ums, ok=%d textReady=%d textLen=%u audioBytes=%u\n",
         elapsed, (int)(!reqError && respTextReady), (int)respTextReady,
         (unsigned)respText.length(), (unsigned)audioTotalBytes);

    return !reqError && respTextReady;
}

bool ble_send_request(const int16_t *pcm, size_t pcmBytes,
                      const String &apiKey, const String &ttsKey,
                      String &responseOut) {
    uint8_t voiceIdx = storage_get_voice();
    uint8_t modelIdx = storage_get_model();
    String hist = gemini_effective_history_fragment();

    LOG1("[ble] send_request: AUDIO pcm=%u bytes, apiKey=%u, ttsKey=%u, voice=%u, model=%u, hist=%u\n",
         (unsigned)pcmBytes, apiKey.length(), ttsKey.length(), voiceIdx, modelIdx, hist.length());

    size_t total = 1 + 1 + (2 + apiKey.length()) + (2 + ttsKey.length())
                 + 1 + 1 + (2 + hist.length()) + pcmBytes;
    uint8_t *buf = (uint8_t *)ps_malloc(total);
    if (!buf) { LOG1("[ble] payload alloc failed (%u bytes)\n", (unsigned)total); return false; }

    uint8_t *p = buf;
    *p++ = BLE_OP_REQUEST;
    *p++ = 0x01;
    write_len_prefixed(p, apiKey);
    write_len_prefixed(p, ttsKey);
    *p++ = voiceIdx;
    *p++ = modelIdx;
    write_len_prefixed(p, hist);
    memcpy(p, pcm, pcmBytes);
    p += pcmBytes;

    // Log first 4 PCM samples for cross-checking with phone
    if (pcmBytes >= 8) {
        LOG1("[ble] PCM[0..3]: %d %d %d %d\n", pcm[0], pcm[1], pcm[2], pcm[3]);
    }

    bool ok = ble_do_request(buf, total);
    free(buf);
    if (ok) {
        responseOut = respText;
        LOG1("[ble] send_request: SUCCESS, response=%u chars\n", responseOut.length());
    } else {
        LOG1("[ble] send_request: FAILED\n");
    }
    return ok;
}

bool ble_send_text_request(const String &text,
                           const String &apiKey, const String &ttsKey,
                           String &responseOut) {
    uint8_t voiceIdx = storage_get_voice();
    uint8_t modelIdx = storage_get_model();
    String hist = gemini_effective_history_fragment();

    LOG1("[ble] send_text_request: \"%s\" apiKey=%u, ttsKey=%u, voice=%u, model=%u, hist=%u\n",
         text.substring(0, 60).c_str(), apiKey.length(), ttsKey.length(),
         voiceIdx, modelIdx, hist.length());

    size_t total = 1 + 1 + (2 + apiKey.length()) + (2 + ttsKey.length())
                 + 1 + 1 + (2 + hist.length()) + text.length();
    uint8_t *buf = (uint8_t *)ps_malloc(total);
    if (!buf) { LOG1("[ble] payload alloc failed (%u bytes)\n", (unsigned)total); return false; }

    uint8_t *p = buf;
    *p++ = BLE_OP_REQUEST;
    *p++ = 0x02;
    write_len_prefixed(p, apiKey);
    write_len_prefixed(p, ttsKey);
    *p++ = voiceIdx;
    *p++ = modelIdx;
    write_len_prefixed(p, hist);
    memcpy(p, text.c_str(), text.length());

    bool ok = ble_do_request(buf, total);
    free(buf);
    if (ok) {
        responseOut = respText;
        LOG1("[ble] send_text_request: SUCCESS, response=%u chars\n", responseOut.length());
    } else {
        LOG1("[ble] send_text_request: FAILED\n");
    }
    return ok;
}

void ble_cancel() {
    reqCancel = true;
    if (deviceConnected && pCtrlChar) {
        uint8_t cmd[] = { BLE_OP_CANCEL };
        pCtrlChar->setValue(cmd, 1);
        pCtrlChar->notify();
    }
}

bool ble_request_stock(const String &symbol,
                       uint32_t timeoutMs,
                       String &outJson,
                       String &errOut) {
    if (!deviceConnected || !pCtrlChar) {
        errOut = "Phone not connected";
        return false;
    }
    if (stockBusy) {
        errOut = "Stock fetch already in flight";
        return false;
    }
    if (symbol.length() == 0 || symbol.length() > 16) {
        errOut = "Bad symbol";
        return false;
    }

    stockBusy   = true;
    stockDone   = false;
    stockError  = false;
    stockResp   = "";
    stockErrMsg = "";

    // Send request on the CTRL channel — that's where the Android side
    // dispatches per-opcode handlers.  TX is reserved for raw audio /
    // request payloads which don't have per-chunk opcodes.  Symbol
    // payload is tiny (<= 16 chars) so a single notify covers it
    // regardless of negotiated MTU.
    uint8_t buf[1 + 16];
    buf[0] = BLE_OP_STOCK_REQUEST;
    memcpy(buf + 1, symbol.c_str(), symbol.length());
    int retries = 0;
    while (!pCtrlChar->notify(buf, 1 + symbol.length()) &&
           retries < 50 && deviceConnected) {
        vTaskDelay(pdMS_TO_TICKS(5));
        retries++;
    }
    if (retries >= 50 || !deviceConnected) {
        stockBusy = false;
        errOut = "Couldn't reach phone";
        LOG1("[ble-stock] notify failed (retries=%d, conn=%d)\n",
             retries, (int)deviceConnected);
        return false;
    }
    LOG1("[ble-stock] requested \"%s\" (timeout=%ums)\n",
         symbol.c_str(), (unsigned)timeoutMs);

    // Block until the phone streams the result back, hits the timeout,
    // or the link drops.  Sleep in small slices so the watchdog stays
    // happy and a sudden disconnect breaks us out within ~50 ms.
    uint32_t t0 = millis();
    while (!stockDone && deviceConnected && (millis() - t0) < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bool gotResp = stockDone;
    bool err     = stockError;
    String msg   = stockErrMsg;
    String body  = stockResp;
    stockBusy = false;
    stockDone = false;
    stockResp = "";
    stockErrMsg = "";

    if (!deviceConnected) {
        errOut = "Phone disconnected";
        return false;
    }
    if (!gotResp) {
        errOut = "Phone proxy timeout";
        return false;
    }
    if (err || body.length() == 0) {
        errOut = msg.length() ? msg : String("Phone proxy error");
        return false;
    }
    outJson = body;
    LOG1("[ble-stock] received %u bytes of compact JSON\n",
         (unsigned)outJson.length());
    return true;
}

bool ble_is_busy() {
    return reqBusy;
}

void ble_set_proxy_debug(bool on) {
    if (!pCtrlChar || !deviceConnected) {
        LOG1("[ble] proxy debug: not connected\n");
        return;
    }
    uint8_t cmd[] = { BLE_OP_DEBUG, (uint8_t)(on ? 0x01 : 0x00) };
    pCtrlChar->setValue(cmd, 2);
    pCtrlChar->notify();
    LOG1("[ble] proxy debug %s\n", on ? "ON" : "OFF");
}

void ble_request_notif_snapshot() {
    if (!pCtrlChar || !deviceConnected) {
        LOG2("[ble] notif snapshot: not connected\n");
        return;
    }
    uint8_t cmd[] = { BLE_OP_NOTIF_QUERY };
    pCtrlChar->setValue(cmd, 1);
    pCtrlChar->notify();
    LOG2("[ble] notif snapshot requested\n");
}

// ─── Permanent memory ──────────────────────────────────────────────────────

// Send a single memory list item (J->P).  Used only while servicing a
// BLE_OP_MEMORY_LIST_REQ — the firmware never pushes live additions to
// the phone any more, since the phone polls on demand.
static bool send_memory_list_item(uint64_t id, uint64_t ts, const String &text) {
    if (!pCtrlChar || !deviceConnected) return false;
    uint16_t tlen = (uint16_t)text.length();
    if (tlen > 240) tlen = 240;
    size_t len = 1 + 8 + 8 + 2 + tlen;
    uint8_t buf[1 + 8 + 8 + 2 + 240];
    buf[0] = BLE_OP_MEMORY_ADD;          // J->P direction = list item
    for (int k = 0; k < 8; k++) buf[1 + k] = (uint8_t)((id >> (k * 8)) & 0xFF);
    for (int k = 0; k < 8; k++) buf[9 + k] = (uint8_t)((ts >> (k * 8)) & 0xFF);
    buf[17] = (uint8_t)(tlen & 0xFF);
    buf[18] = (uint8_t)((tlen >> 8) & 0xFF);
    memcpy(buf + 19, text.c_str(), tlen);
    pCtrlChar->setValue(buf, len);
    pCtrlChar->notify();
    LOG2("[ble] MEMORY_LIST_ITEM id=%016llx len=%u\n",
         (unsigned long long)id, (unsigned)tlen);
    return true;
}

// Stream the current memory list to the phone in response to a
// LIST_REQ.  Runs from ble_link_tick() (main loop task), not the
// NimBLE callback, so iterating NVS doesn't block the BT controller.
static void ble_run_memory_list() {
    if (!pCtrlChar || !deviceConnected) return;
    uint16_t cnt = storage_perm_mem_count();
    LOG1("[ble] MEMORY_LIST: streaming %u entries\n", cnt);
    for (uint16_t i = 0; i < cnt && deviceConnected; i++) {
        PermMemoryEntry e;
        if (!storage_perm_mem_get(i, e)) continue;
        send_memory_list_item(e.id, e.ts_ms, e.text);
        // Light pacing so a big batch doesn't blow past the BT TX queue.
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    if (deviceConnected) {
        uint8_t op = BLE_OP_MEMORY_LIST_END;
        pCtrlChar->setValue(&op, 1);
        pCtrlChar->notify();
        LOG2("[ble] MEMORY_LIST_END sent\n");
    }
}

// Defined in Jibo-ESP.ino — needed to live-apply brightness changes
extern void set_display_brightness(uint8_t level);

static void apply_settings_write() {
    String payload = settingsWritePayload;
    settingsWriteReceived = false;
    settingsWritePayload  = "";

    int pos = 0;
    int applied = 0;
    while (pos < (int)payload.length()) {
        int nl = payload.indexOf('\n', pos);
        if (nl < 0) nl = payload.length();
        String line = payload.substring(pos, nl);
        pos = nl + 1;

        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);

        if (key == "AK") {
            storage_set_api_key(val);             applied++;
        } else if (key == "TK") {
            storage_set_tts_key(val);             applied++;
        } else if (key == "BR") {
            uint8_t b = (uint8_t)val.toInt();
            storage_set_brightness(b);
            set_display_brightness(b);            applied++;
        } else if (key == "MO") {
            storage_set_model((uint8_t)val.toInt()); applied++;
        } else if (key == "VO") {
            storage_set_voice((uint8_t)val.toInt()); applied++;
        } else if (key == "ME") {
            storage_set_memory_enabled(val == "1");  applied++;
        } else if (key == "WE") {
            storage_set_wifi_enabled(val == "1");    applied++;
        } else if (key == "ST") {
            storage_set_sleep_timeout((uint8_t)val.toInt()); applied++;
        }
    }
    LOG1("[ble] SETTINGS_WRITE applied %d settings\n", applied);

    // Send back the updated settings so the phone confirms the change
    ble_send_all_settings();
}

void ble_link_tick() {
    if (memSyncPending && deviceConnected) {
        memSyncPending = false;
        ble_run_memory_list();
    }
    if (settingsWriteReceived && deviceConnected) {
        apply_settings_write();
    }
    dev_console_tick();
}

// Used by dev_console.cpp to push log / stats frames over BLE.
// One-shot notify (no chunking; payload must already fit a single MTU)
// — caller is expected to size to NimBLEDevice::getMTU() - 4.
//
// Routed through the CTRL characteristic, NOT the TX one — the TX
// char is a raw byte pipe for audio-request payloads (unprefixed),
// while CTRL is opcode-dispatched on the phone side.  Mixing them
// would confuse the phone's request-buffer accumulator.
//
// Returns false if the link is down or notify queue stays full long
// enough to hit our retry cap (no-op for the caller in that case).
bool ble_dev_send(uint8_t op, const uint8_t *data, size_t len) {
    if (!pCtrlChar || !deviceConnected) return false;
    // Sized for the maximum MTU we negotiate (512) plus opcode + slack.
    // Caller is responsible for keeping `len` ≤ MTU − GATT_HDR(3); we
    // just clamp here as a last line of defence.
    uint8_t stack[540];
    if (len > sizeof(stack) - 1) len = sizeof(stack) - 1;
    stack[0] = op;
    memcpy(stack + 1, data, len);

    int retries = 0;
    while (!pCtrlChar->notify(stack, 1 + len) && retries < 30 && deviceConnected) {
        vTaskDelay(pdMS_TO_TICKS(2));
        retries++;
    }
    return retries < 30;
}

// ─── Phone-assisted setup ──────────────────────────────────────────────────

void ble_send_setup_mode(bool inSetup) {
    if (!pCtrlChar || !deviceConnected) return;
    uint8_t buf[2] = { BLE_OP_SETUP_MODE, (uint8_t)(inSetup ? 1 : 0) };
    pCtrlChar->setValue(buf, 2);
    pCtrlChar->notify();
    LOG1("[ble] SETUP_MODE sent: %d\n", (int)inSetup);
}

void ble_send_setup_status(uint8_t status) {
    if (!pCtrlChar || !deviceConnected) return;
    uint8_t buf[2] = { BLE_OP_SETUP_STATUS, status };
    pCtrlChar->setValue(buf, 2);
    pCtrlChar->notify();
    LOG1("[ble] SETUP_STATUS sent: %u\n", status);
}

bool ble_setup_wifi_received()     { return setupWifiReceived; }
bool ble_setup_keys_received()     { return setupKeysReceived; }
bool ble_setup_complete_received() { return setupCompleteReceived; }

BleSetupWifi ble_get_setup_wifi() {
    return setupWifi;
}

void ble_get_setup_keys(String &apiKey, String &ttsKey) {
    apiKey = setupApiKey;
    ttsKey = setupTtsKey;
}

void ble_clear_setup_data() {
    setupWifiReceived     = false;
    setupKeysReceived     = false;
    setupCompleteReceived = false;
    setupWifi.ssid       = "";
    setupWifi.password   = "";
    setupWifi.username   = "";
    setupWifi.enterprise = false;
    setupApiKey          = "";
    setupTtsKey          = "";
    LOG1("[ble] setup data cleared\n");
}

// ─── Remote settings ──────────────────────────────────────────────────────

void ble_send_all_settings() {
    if (!pCtrlChar || !deviceConnected) return;

    // Build key=value blob with all current settings
    String cfg;
    cfg += "AK=" + storage_get_api_key() + "\n";
    cfg += "TK=" + storage_get_tts_key() + "\n";
    cfg += "BR=" + String(storage_get_brightness()) + "\n";
    cfg += "MO=" + String(storage_get_model()) + "\n";
    cfg += "VO=" + String(storage_get_voice()) + "\n";
    cfg += "ME=" + String(storage_get_memory_enabled() ? 1 : 0) + "\n";
    cfg += "WE=" + String(storage_get_wifi_enabled() ? 1 : 0) + "\n";
    cfg += "ST=" + String(storage_get_sleep_timeout()) + "\n";
    cfg += "DM=" + String(g_dev_mode ? 1 : 0) + "\n";

    size_t len = cfg.length();
    uint8_t *buf = (uint8_t *)malloc(1 + len);
    if (!buf) return;
    buf[0] = BLE_OP_SETTINGS_DATA;
    memcpy(buf + 1, cfg.c_str(), len);

    // Use CTRL notify — fits in one MTU for typical settings (~200 bytes)
    pCtrlChar->setValue(buf, 1 + len);
    pCtrlChar->notify();
    free(buf);
    LOG1("[ble] SETTINGS_DATA sent: %u bytes\n", (unsigned)len);
}

bool ble_settings_write_received() { return settingsWriteReceived; }

String ble_get_settings_write_payload() {
    return settingsWritePayload;
}

void ble_clear_settings_write() {
    settingsWriteReceived = false;
    settingsWritePayload  = "";
}
