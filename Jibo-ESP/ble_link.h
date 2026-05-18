#pragma once
#include <Arduino.h>

// BLE protocol opcodes
#define BLE_OP_REQUEST      0x01  // J->P: API request (audio or text)
#define BLE_OP_TEXT_RESP    0x02  // P->J: Gemini text response
#define BLE_OP_AUDIO_CHUNK  0x03  // P->J: PCM audio chunk
#define BLE_OP_DONE         0x04  // P->J: request complete (1B status: 0=ok, 1=error)
#define BLE_OP_CANCEL       0x05  // J->P: cancel current request
#define BLE_OP_REQ_READY    0x06  // J->P: all request chunks sent, process now
#define BLE_OP_PAIR         0x10  // bidirectional: pairing handshake
#define BLE_OP_DEBUG        0x11  // J->P: proxy debug mode (1B: 0=off, 1=on)
#define BLE_OP_NOTIF_PUSH   0x20  // P->J: notification(s) JSON
#define BLE_OP_NOTIF_QUERY  0x21  // J->P: request full snapshot
#define BLE_OP_NOTIF_REMOVE 0x22  // P->J: remove single notification by key

// Permanent memory protocol.
//
// Firmware NVS is the single source of truth.  The phone is a passive
// viewer that polls the list when the user opens the memory screen and
// pushes mutations (add / remove / clear) without keeping a local copy.
// This eliminated a whole class of sync bugs (tombstone spam, ping-pong
// reconnects, "1970" timestamps, etc.) that plagued the old bidirectional
// design.
//
//   P->J  MEMORY_ADD       0x23  id(8) + ts(8) + len(2) + utf8 text
//   P->J  MEMORY_REMOVE    0x24  id(8)
//   P->J  MEMORY_LIST_REQ  0x25  empty -- triggers the J->P stream below
//   P->J  MEMORY_CLEAR     0x27  empty -- nuke everything
//   J->P  MEMORY_LIST_ITEM 0x23  id(8) + ts(8) + len(2) + utf8 text  (one per memory)
//   J->P  MEMORY_LIST_END  0x26  empty -- terminates a list response
//
// 0x23 reuses the same wire format in both directions — the receiver
// disambiguates by direction, not opcode.
#define BLE_OP_MEMORY_ADD         0x23  // P->J: store this memory (also: J->P list item)
#define BLE_OP_MEMORY_REMOVE      0x24  // P->J: delete by id
#define BLE_OP_MEMORY_LIST_REQ    0x25  // P->J: phone wants the list
#define BLE_OP_MEMORY_LIST_END    0x26  // J->P: end-of-list marker
#define BLE_OP_MEMORY_CLEAR       0x27  // P->J: wipe all memories

// ─── Developer console (hidden in-app debug feature) ─────────────────────
// The Android app has a hidden "developer mode" toggle (10 taps on the
// J icon).  When enabled it asks the firmware to forward serial output
// over BLE so the user can see logs and run commands without a USB cable.
#define BLE_OP_DEV_ENABLE  0x30  // P->J: 1B (0=disable, 1=enable)
#define BLE_OP_DEV_LOG     0x31  // J->P: UTF-8 log bytes (may span lines)
#define BLE_OP_DEV_CMD     0x32  // P->J: UTF-8 command bytes (no trailing \n)
#define BLE_OP_DEV_STATS   0x33  // J->P: JSON summary (heap, cpu, wifi, ...)
// Tasks are emitted right after each STATS frame on a separate opcode
// because a full FreeRTOS task table (~13 entries) doesn't fit in one
// MTU.  Each chunk JSON: {"t":[{...},{...}],"f":0|1}.  `f` is 1 on the
// final chunk so the phone knows when the cycle is complete; STATS
// itself resets the accumulator so a missed final-flag at most loses a
// single sample instead of permanently jamming the dashboard.
#define BLE_OP_DEV_TASKS   0x34  // J->P: JSON task chunk

// ─── Stock fetch via phone proxy ─────────────────────────────────────────
// When the firmware has no Wi-Fi but the phone is connected, the stock
// tool routes its Yahoo Finance fetch through the phone instead.  Phone
// performs the HTTPS request, parses the response down to a compact
// JSON blob, and streams it back in one or more RESPONSE chunks
// followed by a DONE.  Compact form so the whole thing fits in a
// handful of MTUs:
//   {"sym":"NVDA","price":143.5,"prev":138.0,"pts":[...]}
#define BLE_OP_STOCK_REQUEST  0x40  // J->P: UTF-8 ticker symbol
#define BLE_OP_STOCK_RESPONSE 0x41  // P->J: UTF-8 chunk of compact JSON
#define BLE_OP_STOCK_DONE     0x42  // P->J: 1B status (0=ok, 1=error)

// ─── Remote settings (phone reads/writes Jibo config over BLE) ─────────────
#define BLE_OP_SETTINGS_REQ    0x60  // P->J: empty — request all settings
#define BLE_OP_SETTINGS_DATA   0x61  // J->P: UTF-8 key=value\n blob
#define BLE_OP_SETTINGS_WRITE  0x62  // P->J: UTF-8 key=value\n (one or more)

// ─── Messaging (phone-proxied SMS / chat) ──────────────────────────────────
//
// Conversation list:
//   J->P  MSG_CONVO_REQ      0x70  empty — request conversation summaries
//   P->J  MSG_CONVO_DATA     0x71  UTF-8 chunk of conversations JSON
//   P->J  MSG_CONVO_DONE     0x72  1B status (0=ok, 1=error)
//
// Thread messages:
//   J->P  MSG_THREAD_REQ     0x73  UTF-8 thread_id
//   P->J  MSG_THREAD_DATA    0x74  UTF-8 chunk of messages JSON
//   P->J  MSG_THREAD_DONE    0x75  1B status (0=ok, 1=error)
//
// Send a message:
//   J->P  MSG_SEND           0x76  UTF-8 JSON {contact_id, text, ...}
//   P->J  MSG_SEND_RESULT    0x77  1B status (0=ok, 1=error)
//
// Real-time push:
//   P->J  MSG_NEW_PUSH       0x78  UTF-8 JSON (single new message)
//
// Speech-to-text for compose:
//   J->P  MSG_STT_START      0x79  empty — begin STT capture on phone
//   J->P  MSG_STT_STOP       0x7A  empty — stop capture, finalize
//   P->J  MSG_STT_RESULT     0x7B  UTF-8 transcribed text
//   P->J  MSG_STT_ERROR      0x7C  1B error code
//
// Contact search (disambiguation):
//   J->P  MSG_CONTACT_SEARCH 0x7D  UTF-8 name query
//   P->J  MSG_CONTACT_RESULT 0x7E  UTF-8 JSON array of matches
#define BLE_OP_MSG_CONVO_REQ      0x70
#define BLE_OP_MSG_CONVO_DATA     0x71
#define BLE_OP_MSG_CONVO_DONE     0x72
#define BLE_OP_MSG_THREAD_REQ     0x73
#define BLE_OP_MSG_THREAD_DATA    0x74
#define BLE_OP_MSG_THREAD_DONE    0x75
#define BLE_OP_MSG_SEND           0x76
#define BLE_OP_MSG_SEND_RESULT    0x77
#define BLE_OP_MSG_NEW_PUSH       0x78
#define BLE_OP_MSG_STT_START      0x79
#define BLE_OP_MSG_STT_STOP       0x7A
#define BLE_OP_MSG_STT_RESULT     0x7B
#define BLE_OP_MSG_STT_ERROR      0x7C
#define BLE_OP_MSG_CONTACT_SEARCH 0x7D
#define BLE_OP_MSG_CONTACT_RESULT 0x7E

// ─── Phone-assisted setup protocol ─────────────────────────────────────────
#define BLE_OP_SETUP_MODE      0x50  // J->P: 1B (1=in_setup, 0=normal)
#define BLE_OP_SETUP_WIFI      0x51  // P->J: [op][1B enterprise][ssid\0][pass\0][username\0 if enterprise]
#define BLE_OP_SETUP_KEYS      0x52  // P->J: [op][gemini_key\0][tts_key\0]
#define BLE_OP_SETUP_COMPLETE  0x53  // P->J: empty — all config sent, proceed
#define BLE_OP_SETUP_STATUS    0x54  // J->P: 1B (0=wifi_connecting, 1=wifi_connected, 2=auth_fail, 3=not_found, 4=timeout, 5=all_done)

void     ble_init();

bool     ble_is_paired();
bool     ble_is_connected();
int      ble_read_rssi();   // dBm, or 0 if not connected
String   ble_phone_name();

void     ble_start_pairing();
void     ble_stop_pairing();
uint16_t ble_get_pairing_code();
bool     ble_pairing_active();
bool     ble_pairing_phone_connected();  // phone connected during pairing, ready to show code
bool     ble_device_connected_raw();     // raw BLE connection state (regardless of pairing)

void     ble_unpair();

// Send a voice request via phone proxy.
// Blocks until response text + audio streaming completes (or error/cancel).
// Returns true if response was received, false on error/cancel.
// Sets responseOut to the text response from Gemini.
bool     ble_send_request(const int16_t *pcm, size_t pcmBytes,
                          const String &apiKey, const String &ttsKey,
                          String &responseOut);

bool     ble_send_text_request(const String &text,
                               const String &apiKey, const String &ttsKey,
                               String &responseOut);

void     ble_cancel();
bool     ble_is_busy();

void     ble_set_proxy_debug(bool on);

// Ask the phone to send a fresh snapshot of all current notifications.
void     ble_request_notif_snapshot();

// ─── Stock proxy ────────────────────────────────────────────────────────────
//
// Synchronous helper used by stock_quote.cpp's worker task when direct
// Wi-Fi isn't available.  Sends BLE_OP_STOCK_REQUEST with `symbol` and
// blocks (up to `timeoutMs`) waiting for the phone to stream back a
// compact JSON response.  On success returns true with `outJson`
// populated; on timeout / error returns false with a human-readable
// `errOut`.  Safe to call only from a non-UI task — it sleeps the
// caller while waiting.  Returns false immediately if the BLE link
// isn't up or another stock request is already in flight.
bool     ble_request_stock(const String &symbol,
                           uint32_t timeoutMs,
                           String &outJson,
                           String &errOut);

// Pump pending background BLE work (e.g. streaming a memory snapshot
// after a fresh CONNECT).  Call from the main loop().
void     ble_link_tick();

// Low-power transitions for STATE_SLEEP.  enter_low_power() halts
// advertising and disconnects any active link so the BT controller
// drops to its idle baseline; exit_low_power() restores advertising
// per the normal pair/connect rules.  We deliberately don't deinit
// NimBLE — re-init takes ~300 ms and causes a noticeable wake delay.
void     ble_enter_low_power();
void     ble_exit_low_power();

// ─── Phone-assisted setup ──────────────────────────────────────────────────
void     ble_send_setup_mode(bool inSetup);      // notify phone of setup state
void     ble_send_setup_status(uint8_t status);   // send wifi/setup status to phone

bool     ble_setup_wifi_received();     // true if SETUP_WIFI was received
bool     ble_setup_keys_received();     // true if SETUP_KEYS was received
bool     ble_setup_complete_received(); // true if SETUP_COMPLETE was received

struct BleSetupWifi { String ssid; String password; String username; bool enterprise; };
BleSetupWifi ble_get_setup_wifi();
void         ble_get_setup_keys(String &apiKey, String &ttsKey);

void     ble_clear_setup_data();  // reset all setup flags/data for reuse

// ─── Remote settings ──────────────────────────────────────────────────────
void     ble_send_all_settings();           // send full settings blob to phone
bool     ble_settings_write_received();     // true if a SETTINGS_WRITE arrived
String   ble_get_settings_write_payload();  // consume the payload
void     ble_clear_settings_write();

// ─── Messaging ────────────────────────────────────────────────────────────
void     ble_msg_request_conversations();
void     ble_msg_request_thread(const String &thread_id);
void     ble_msg_send(const String &json);
void     ble_msg_stt_start();
void     ble_msg_stt_stop();
void     ble_msg_contact_search(const String &name);
