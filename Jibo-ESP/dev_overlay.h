#pragma once

// ──────────────────────────────────────────────────────────────────────────
//  Dev Overlay — on-screen diagnostics for developer mode
//
//  Two independent overlays, each gated by its own NVS toggle:
//
//    Stats Pill    — small rounded rect at the top of the screen with
//                    three mini ring arcs (DRAM%, PSRAM%, CPU%).
//
//    Verbose Strip — 3-line semi-transparent strip at the bottom showing
//                    the most recent activity (API calls, TTS, state
//                    changes, etc.).
//
//  Both are rendered as top-layer LVGL objects so they appear over the eye,
//  home screen, and settings alike.  They're created/destroyed dynamically
//  based on the toggles and dev-mode state.
// ──────────────────────────────────────────────────────────────────────────

// Call once from setup(), after LVGL and storage are ready.
void dev_overlay_init();

// Call every loop iteration (or at least every ~50 ms).
// Handles periodic stat updates and toggle checks.
void dev_overlay_tick();

// Push a line into the verbose strip's ring buffer.
// Safe to call even when the strip is hidden — it silently discards.
// Max ~60 chars; longer strings are truncated.
void dev_overlay_log(const char *line);

// Force-destroy both overlays (call before entering sleep / power-off).
void dev_overlay_destroy();
