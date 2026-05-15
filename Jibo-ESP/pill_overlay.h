// ─────────────────────────────────────────────────────────────────────────────
// pill_overlay — Xbox-achievement-style toast pill for Jibo
//
// Pops up from the bottom of the screen as a circle, expands sideways
// into a pill with an icon on the left and a body label (plus optional
// subtext) on the right, holds, then collapses and drops away.  Used
// for several different feedback events:
//
//   - PILL_ICON_SUCCESS   green check  → "Saved to memory"
//   - PILL_ICON_ERROR     red X        → "Gemini problem" / "TTS problem"
//   - PILL_ICON_CHARGING  blue bolt    → "Charging - 73%"
//   - PILL_ICON_LOW_BAT   yellow batt  → "Low battery"
//
// All variants share the same animation timing and shape; only the
// circle's background colour and the symbol drawn inside it change.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>

enum PillIcon {
    PILL_ICON_SUCCESS  = 0,
    PILL_ICON_ERROR    = 1,
    PILL_ICON_CHARGING = 2,
    PILL_ICON_LOW_BAT  = 3,
    PILL_ICON_NOTIF    = 4,   // bell glyph, purple/violet bg — phone notifs
};

// Show a pill immediately.  Safe to call from any task — the
// implementation marshals to the LVGL thread via lv_async_call.  Calling
// again while a pill is on screen kills the previous one and replaces
// it (covers rapid back-to-back events).
//
// `subtext` is optional; pass NULL or "" to render a single-line pill.
void pill_show(PillIcon icon, const char *text, const char *subtext = nullptr,
               uint32_t hold_ms = 2000);

// Queue a pill to fire on the next call to `pill_release_pending()`.
// Used so the "Saved to memory" pill doesn't flash during STATE_THINKING
// (when Gemini's response arrives) — instead it shows up at the moment
// Jibo actually starts speaking the response, which feels more connected
// to the user's request.  Safe from any task; replaces any earlier
// queued pill that hasn't fired yet.
void pill_queue_for_speaking(PillIcon icon, const char *text,
                             const char *subtext = nullptr);

// Fire any pill queued by `pill_queue_for_speaking()`.  Called by the
// state machine when STATE_SPEAKING is entered.  No-op if nothing is
// queued.
void pill_release_pending();

// True once a queued pill is waiting for `pill_release_pending()` —
// lets the speaking-state entry know to snap the eye to the centre
// immediately (avoiding lerp animation contention with the pill anim).
bool pill_has_pending();

// True from the moment the pill begins popping up until it has fully
// dropped off-screen.  Used by the speaking-state tick to pause the
// per-frame amplitude-driven eye zoom — that re-renders the eye canvas
// every frame, which contends with LVGL's render thread and makes the
// pill animation jittery on top.
bool pill_is_active();
