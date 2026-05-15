#pragma once
#include <Arduino.h>

// In-memory store of phone notifications mirrored from the Android app via BLE.
// Persistence is intentionally NOT used — notifications are transient.

struct NotifEntry {
    String   key;            // sbn.key from Android (used for removal)
    String   app;            // human-readable app label, e.g. "WhatsApp"
    String   sender;         // EXTRA_TITLE — usually the contact / sender name
    String   text;           // notification body (EXTRA_TEXT / EXTRA_BIG_TEXT)
    String   category;       // "messaging", "banking", "alarm", "calendar", ...
    bool     timeSensitive;  // alarms, calendar reminders, missed calls, etc.
    uint32_t postTimeMs;     // millis() when ingested (for relative-age rendering)
};

void   notifications_init();

// Ingest a JSON payload coming from the phone over BLE (BLE_OP_NOTIF_PUSH).
// Accepts either a single notification object {"key":...,"app":...} OR a JSON
// array of such objects (used for the full-snapshot reply to NOTIF_QUERY).
void   notifications_ingest_push(const uint8_t *json, size_t len);

void   notifications_remove(const String &key);
void   notifications_clear_all();
size_t notifications_count();

// Build the "current notifications" block that gets injected into Gemini's
// system prompt.  Returns a friendly fallback string when the buffer is empty.
String notifications_format_for_prompt();

// ─── Pill announcement ──────────────────────────────────────────────────────
//
// Fires a notification pill any time we ingest notifs whose `key` we
// haven't shown yet.  Logic:
//
//   • Single new notif         → "New Notification"
//                                 "from <sender>" (if messaging w/ sender)
//                                 "from <app>"    (otherwise)
//   • Multi from same source   → "New Notifications"
//                                 "X new notifications from <sender/app>"
//   • Multi from mixed sources → "New Notifications"
//                                 "X new notifications"
//
// `notifications_pause_pill()` suppresses pills (used while Jibo is
// asleep so we don't fire pills onto a black screen).  Calling
// `notifications_resume_pill()` re-enables them AND immediately fires
// any catch-up pill for notifs that arrived during the pause window.
//
// On boot the system starts in a SILENT pause: notifs ingested before
// the device first reaches IDLE are absorbed without firing a pill and
// without queueing a catch-up.  `notifications_boot_complete()` ends
// the silent pause once boot has finished.

void notifications_pause_pill();
void notifications_resume_pill();
bool notifications_pill_paused();
void notifications_boot_complete();

// Mark every currently-tracked notif as "already shown" without firing
// a pill.  Called at boot so the user doesn't get a pill for stale
// notifs that were on the phone before Jibo even came up.
void notifications_mark_all_seen();
