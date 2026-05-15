# Open Pocket Jibo

Open-pocket-jibo is a project inspired by Jibo, a now dead robot from 2017. This project aims to not perfectly replicate, but take inspiration from Jibo, such as his signature eye. Much of the project though is entirely original.

## Hardware

- **Board:** [Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm) — 466×466 round AMOLED, capacitive touch, onboard mic + speaker
- **Connectivity:** WiFi + BLE 5.0
- **Companion App:** Android (Kotlin / Jetpack Compose)

## Current Features

- **Voice assistant** — powered by Gemini with Google Search and URL context grounding for real-time answers
- **Live stock cards** — on-screen stock prices and graphs with switchable timeframes (1D / 1W / 1M / 6M / YTD / 1Y)
- **LaTeX rendering** — display formatted math equations and formulas on screen
- **Phone notifications** — read and triage notifications from a linked Android phone over BLE, with automatic sensitivity detection
- **Phone finder** — locate your phone via BLE RSSI with a live signal strength display
- **Chat memory** — volatile conversation history and persistent user memories that survive reboots
- **Multiple voices** — selectable TTS voices via Deepgram


## Planned Features

- Text messaging (assistant-driven and as a standalone applet)
- Weather applet
- Email integration
- Interaction with other Jibo devices
- Calendar integration
- Todo integration
- iOS companion app

## Project Structure

```
Jibo-ESP/               ESP32-S3 firmware (Arduino + LVGL + FreeRTOS)
Jibo-Android/           Android companion app (Kotlin / Jetpack Compose)
```

## Developer Mode

To enable developer mode, go into **Settings > About** and tap the Jibo logo 10 times, then it's pretty self explanatory from there. Here's a list of features developer mode enables:

### On-Device Overlays

| Overlay | Description |
|---------|-------------|
| **Stats Pill** | Small rounded rect at the top of the screen showing DRAM%, PSRAM%, and CPU% as mini ring arcs |
| **Verbose Strip** | 3-line semi-transparent strip at the bottom showing live activity (API calls, TTS events, state changes) lwk kinda broken atm but i cant be bothered to fix it |
| **Verbose Boot** | also broken womp womp |
### Console Commands

Available via the companion app's built-in terminal or USB serial:

| Command | Description |
|---------|-------------|
| `help` | Show all available commands |
| `audiodebug` | Enter audio level debug screen |
| `imudebug` | Show 3D IMU orientation on display |
| `imucal` | Calibrate IMU (keep device still) |
| `proxydebug` | Enable BLE proxy debug on phone |
| `debugexit` | Exit any debug overlay |
| `ttskey <key>` | Save Deepgram TTS key |
| `ttstest [text]` | Speak text via Deepgram |
| `geminitest <text>` | Send a text query to Gemini |
| `clearmem` | Clear conversation history |
| `memory` | List permanent memories |
| `pilltest <icon> <text>` | Test pill notification animation (`success`, `error`, `charging`, `lowbat`, `notif`) |
| `notifdebug` | Print the notifications system-prompt block |
| `notifsync` | Request fresh notification snapshot from phone |
| `taskmanager` | Print FreeRTOS task table |
| `log_level 0..3` | Set log verbosity |
| `testtone` | Play 1kHz sine for 2 seconds |
| `export` | Dump all config as a copy-pasteable string |
| `import <string>` | Restore config from an export string |
| `crash <type>` | Force a crash for testing (`nullptr`, `divzero`, `stackoverflow`, `wdt`, `abort`, `oom`, `reboot`) |
| `whathappened` | Dump the event log for the current boot |
| `reset` | Factory-reset NVS and reboot |

### Companion App Dev Tools

The Android app's dev tools screen (which is enabled once jibo is in developer mdoe) has a couple simple features:

- **Live terminal** — serial-monitor-style console that mirrors the firmware's log output in real time over BLE, with full command input
- **Dashboard** — live stats for heap, PSRAM, uptime, WiFi, RSSI, and a per-task breakdown showing priority, state, stack high-water mark, and core affinity

## License

TBD
