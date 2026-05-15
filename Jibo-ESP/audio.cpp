#include "audio.h"
#include "pin_config.h"
#include "event_log.h"
#include "log.h"
#include "ESP_I2S.h"
#include "esp_check.h"
#include "es8311.h"
#include <Wire.h>

#define SAMPLE_RATE       16000
#define MAX_RECORD_SEC    10
#define MAX_RECORD_BYTES  (SAMPLE_RATE * 2 * MAX_RECORD_SEC)  // 16-bit mono

#define ES7210_ADDR       0x40

static I2SClass i2s;
// Held so audio_enter_low_power() / audio_exit_low_power() can mute the
// codec for sleep without re-init.
static es8311_handle_t es8311 = NULL;

static int16_t       *recBuf       = NULL;
static volatile size_t recBytes    = 0;
static volatile bool   recording   = false;
static volatile bool     playing     = false;
static volatile bool     paActive    = false;   // true once speaker PA is on
static volatile uint32_t playGen     = 0;        // generation counter to prevent stale task interference
static TaskHandle_t      recTask     = NULL;
static TaskHandle_t      playTask    = NULL;

static const int16_t  *playPtr     = NULL;
static size_t          playLen     = 0;

static volatile float  outAmplitude = 0.0f;

static void update_amplitude(const int16_t *samples, size_t count) {
    if (count == 0) { outAmplitude = 0.0f; return; }
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        int32_t a = (s < 0) ? -s : s;
        if (a > peak) peak = a;
    }
    outAmplitude = (float)peak / 32768.0f;
}

// ─── I2C helper for ES7210 ──────────────────────────────────────────────────

static bool es7210_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// ─── ES7210 mic ADC init (dual microphone) ──────────────────────────────────

static bool es7210_init() {
    // Software reset
    es7210_write_reg(0x00, 0xFF);
    delay(20);
    es7210_write_reg(0x00, 0x32);

    // Power-up timing
    es7210_write_reg(0x09, 0x30);
    es7210_write_reg(0x0A, 0x30);

    // High-pass filter for ADC1-4
    es7210_write_reg(0x23, 0x2A);
    es7210_write_reg(0x22, 0x0A);
    es7210_write_reg(0x21, 0x2A);
    es7210_write_reg(0x20, 0x0A);

    // I2S format: standard I2S (0x00), 16-bit (0x60), no TDM
    es7210_write_reg(0x11, 0x60);
    es7210_write_reg(0x12, 0x00);

    // Analog power
    es7210_write_reg(0x40, 0xC3);

    // MIC bias 2.87V
    es7210_write_reg(0x41, 0x70);
    es7210_write_reg(0x42, 0x70);

    // MIC gain 30dB (value 10 = 30dB, | 0x10 = enable)
    es7210_write_reg(0x43, 0x1A);
    es7210_write_reg(0x44, 0x1A);
    es7210_write_reg(0x45, 0x1A);
    es7210_write_reg(0x46, 0x1A);

    // Power on MIC1-4
    es7210_write_reg(0x47, 0x08);
    es7210_write_reg(0x48, 0x08);
    es7210_write_reg(0x49, 0x08);
    es7210_write_reg(0x4A, 0x08);

    // Clock config for MCLK=4096000Hz (16000*256), LRCK=16000Hz
    // Coefficients: adc_div=0x01, doubler=1, dll=1, osr=0x20, lrck_h=0x01, lrck_l=0x00
    es7210_write_reg(0x07, 0x20);                          // OSR
    es7210_write_reg(0x02, 0x01 | (1 << 6) | (1 << 7));   // MAINCLK = 0xC1
    es7210_write_reg(0x04, 0x01);                          // LRCK divider high
    es7210_write_reg(0x05, 0x00);                          // LRCK divider low

    // Power down DLL
    es7210_write_reg(0x06, 0x04);

    // Power on MIC12 & MIC34 bias + ADC + PGA
    es7210_write_reg(0x4B, 0x0F);
    es7210_write_reg(0x4C, 0x0F);

    // Enable device
    es7210_write_reg(0x00, 0x71);
    es7210_write_reg(0x00, 0x41);

    LOGLN2("[audio] ES7210 mic ADC initialized");
    return true;
}

// ─── ES8311 speaker DAC init via I2C ────────────────────────────────────────

static bool es8311_codec_init() {
    es8311 = es8311_create(0, ES8311_ADDRRES_0);
    if (!es8311) { LOGLN1("ES8311 create failed"); return false; }

    const es8311_clock_config_t clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = SAMPLE_RATE * 256,
        .sample_frequency = SAMPLE_RATE
    };

    if (es8311_init(es8311, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_sample_frequency_config(es8311, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(es8311, false);
    es8311_voice_volume_set(es8311, 80, NULL);
    es8311_microphone_gain_set(es8311, (es8311_mic_gain_t)6);
    return true;
}

// ─── FreeRTOS tasks ─────────────────────────────────────────────────────────

static void record_task_fn(void *) {
    int16_t tmp[512];   // stereo frames: 256 frames × 2ch × 2B = 1024 bytes
    bool firstRead = true;
    while (recording && recBytes < MAX_RECORD_BYTES) {
        size_t got = i2s.readBytes((char *)tmp, sizeof(tmp));
        if (firstRead) {
            LOG3("[rec] first read: %u bytes, L=%d R=%d L=%d R=%d\n",
                          got, tmp[0], tmp[1], tmp[2], tmp[3]);
            firstRead = false;
        }
        size_t frames = got / 4;   // 2 channels × 2 bytes per sample
        for (size_t i = 0; i < frames && recBytes < MAX_RECORD_BYTES; i++) {
            int16_t l = tmp[i * 2];
            int16_t r = tmp[i * 2 + 1];
            recBuf[recBytes / 2] = (l / 2) + (r / 2);   // mix both channels
            recBytes += 2;
        }
    }
    recording = false;
    recTask = NULL;
    vTaskDelete(NULL);
}

// Shorthand for "I'm the current play task" — a task whose myGen no
// longer matches playGen has been superseded and must immediately stop
// touching I2S / shared state.  See the long comment above
// audio_stop_play() for why this check is critical.
#define STILL_CURRENT() (playing && playGen == myGen)

static void play_task_fn(void *) {
    uint32_t myGen = playGen;
    const size_t CHUNK = 512;
    int16_t *stereo = (int16_t *)malloc(CHUNK * 2 * sizeof(int16_t));
    if (!stereo) {
        LOGLN1("[play] stereo buf alloc failed");
        if (playGen == myGen) { playing = false; playTask = NULL; }
        vTaskDelete(NULL); return;
    }

    size_t pos = 0;
    size_t totalSamples = playLen / 2;

    pinMode(AUDIO_PA, OUTPUT);
    digitalWrite(AUDIO_PA, HIGH);
    paActive = true;

    while (STILL_CURRENT() && pos < totalSamples) {
        size_t n = min(CHUNK, totalSamples - pos);
        update_amplitude(&playPtr[pos], n);
        for (size_t i = 0; i < n; i++) {
            int16_t s = playPtr[pos + i];
            stereo[i * 2]     = s;
            stereo[i * 2 + 1] = s;
        }
        size_t toWrite = n * 4;
        size_t written = 0;
        while (written < toWrite && STILL_CURRENT()) {
            size_t w = i2s.write((uint8_t *)stereo + written, toWrite - written);
            if (w == 0) { vTaskDelay(1); }
            written += w;
        }
        pos += n;
    }

    free(stereo);
    if (playGen == myGen) {
        outAmplitude = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(150));
        digitalWrite(AUDIO_PA, LOW);
        paActive  = false;
        playing   = false;
        playTask  = NULL;
    }
    // Stale tasks: do NOT touch outAmplitude / paActive / playing /
    // playTask — those belong to whatever task superseded us.
    vTaskDelete(NULL);
}

// ─── Progressive playback state ─────────────────────────────────────────────
static volatile size_t progWritePos   = 0;
static volatile bool   progDone       = false;
// Total expected bytes for adaptive prebuf calculation.  0 = unknown, in
// which case prog_play_task falls back to a rate-only heuristic.  Set by
// audio_progressive_set_total() and reset to 0 by audio_play_progressive().
static volatile size_t progTotalBytes = 0;

// Fixed playback consumption rate for 16 kHz mono 16-bit PCM (the only
// format this path produces — Deepgram returns linear16 @ 16 kHz).  Used
// as the denominator in the adaptive prebuf formula B >= T*(1 - N/P).
static const uint32_t PROG_PLAY_RATE = 16000 /*Hz*/ * 2 /*bytes/sample*/;
                                                             // = 32000 B/s

static void prog_play_task(void *param) {
    uint32_t myGen = playGen;
    uint8_t *buf = (uint8_t *)param;
    const size_t CHUNK = 256;
    int16_t *stereo = (int16_t *)malloc(CHUNK * 2 * sizeof(int16_t));
    if (!stereo) {
        if (playGen == myGen) { playing = false; playTask = NULL; }
        vTaskDelete(NULL); return;
    }

    // ───── Adaptive prebuf ─────
    //
    // Phase 1: bootstrap window.  Wait briefly to measure the actual
    // download rate from the buffer-fill timing.  Bound by both time
    // (200 ms) and bytes (8 KB) so we don't hold up either fast or slow
    // networks unnecessarily.
    //
    // Phase 2: compute the target prebuf needed so playback is guaranteed
    // to never catch up to the download tail.  For known total T bytes,
    // measured network rate N B/s, and playback rate P = 32000 B/s:
    //
    //     B >= T * (1 - N/P)   (from "playback never catches download")
    //
    // Plus a 4 KB jitter cushion.  If total is unknown (chunked encoding
    // with no length hint), we fall back to a rate-only heuristic.
    //
    // Phase 3: wait until progWritePos reaches the computed target, the
    // download finishes (progDone), or playback is cancelled.

    const size_t   MIN_PREBUF      = 4  * 1024;   // ≈ 125 ms — pure jitter
    const size_t   MAX_PREBUF      = 64 * 1024;   // ≈ 2.0 s  — hard cap
    const uint32_t BOOTSTRAP_MS    = 200;
    const size_t   BOOTSTRAP_BYTES = 8 * 1024;

    uint32_t bootStart = millis();
    while (STILL_CURRENT() && !progDone &&
           progWritePos < BOOTSTRAP_BYTES &&
           millis() - bootStart < BOOTSTRAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint32_t bootMs    = millis() - bootStart;
    if (bootMs < 1) bootMs = 1;
    size_t   bootBytes = progWritePos;
    uint32_t netRate   = (uint32_t)((uint64_t)bootBytes * 1000 / bootMs);

    size_t totalBytes = progTotalBytes;
    size_t target;

    if (progDone) {
        // Whole download finished during bootstrap (small response or
        // very fast network).  Just start now.
        target = bootBytes;
    } else if (totalBytes > 0) {
        // Known total: solve for the minimum safe prebuf.
        if (netRate >= PROG_PLAY_RATE) {
            // Network keeps up with playback — minimum jitter cushion only.
            target = MIN_PREBUF;
        } else {
            // Net is slower than playback by some factor.  Buffer enough
            // that the (T - B) bytes left to download finish before the
            // (B + (T - B)) bytes we'll have played reach the end of B.
            float deficit = 1.0f - (float)netRate / (float)PROG_PLAY_RATE;
            target = (size_t)((float)totalBytes * deficit) + MIN_PREBUF;
        }
        if (target > totalBytes) target = totalBytes;
    } else {
        // Unknown total: rate-only heuristic.  More conservative because
        // we can't bound the download tail.
        if (netRate >= (uint32_t)(PROG_PLAY_RATE * 1.2f)) {
            target = MIN_PREBUF;            // ~125 ms — fast network
        } else if (netRate >= PROG_PLAY_RATE) {
            target = 16 * 1024;             // ~500 ms — break-even
        } else {
            target = 48 * 1024;             // ~1.5 s — slow, hope for best
        }
    }

    if (target > MAX_PREBUF) target = MAX_PREBUF;
    if (target < MIN_PREBUF) target = MIN_PREBUF;

    LOG2("[play] adaptive prebuf: bootstrap=%uB in %ums (rate=%uB/s, "
         "play=%uB/s), total=%u, target=%u (%.0fms)\n",
         (unsigned)bootBytes, (unsigned)bootMs, (unsigned)netRate,
         (unsigned)PROG_PLAY_RATE, (unsigned)totalBytes,
         (unsigned)target, target * 1000.0f / PROG_PLAY_RATE);

    uint32_t prebufStart = millis();
    while (STILL_CURRENT() && !progDone && progWritePos < target) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    uint32_t prebufMs = millis() - prebufStart;

    if (!STILL_CURRENT()) {
        LOG3("[play] cancelled during prebuf, exiting (gen=%u cur=%u)\n",
             (unsigned)myGen, (unsigned)playGen);
        free(stereo);
        if (playGen == myGen) { playTask = NULL; }
        vTaskDelete(NULL);
        return;
    }

    LOG2("[play] prebuf done: waited extra %ums, have %u/%u bytes, "
         "starting PA\n",
         prebufMs, (unsigned)progWritePos, (unsigned)target);

    pinMode(AUDIO_PA, OUTPUT);
    digitalWrite(AUDIO_PA, HIGH);
    paActive = true;

    size_t playPos = 0;
    uint32_t playStart = millis();
    uint32_t lastLog = playStart;
    int underruns = 0;
    uint32_t underrunTotalMs = 0;
    size_t minAhead = 999999;

    while (STILL_CURRENT()) {
        size_t avail = progWritePos;
        size_t bytesReady = (avail > playPos) ? (avail - playPos) : 0;

        if (bytesReady < minAhead) minAhead = bytesReady;

        if (bytesReady < 2) {
            if (progDone) break;
            underruns++;
            uint32_t u0 = millis();
            while (STILL_CURRENT() && !progDone && progWritePos <= playPos + 1) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            underrunTotalMs += millis() - u0;
            continue;
        }

        size_t maxSamples = bytesReady / 2;
        size_t n = (maxSamples < CHUNK) ? maxSamples : CHUNK;
        const int16_t *src = (const int16_t *)(buf + playPos);
        update_amplitude(src, n);
        for (size_t i = 0; i < n; i++) {
            stereo[i * 2]     = src[i];
            stereo[i * 2 + 1] = src[i];
        }

        size_t toWrite = n * 4;
        size_t written = 0;
        while (written < toWrite && STILL_CURRENT()) {
            size_t w = i2s.write((uint8_t *)stereo + written, toWrite - written);
            if (w == 0) vTaskDelay(1);
            written += w;
        }
        playPos += n * 2;

        if (millis() - lastLog > 1000) {
            LOG3("[play] pos=%u/%u ahead=%u underruns=%d (%ums) minAhead=%u\n",
                          playPos, avail, bytesReady, underruns, underrunTotalMs, minAhead);
            lastLog = millis();
            minAhead = 999999;
        }
    }

    uint32_t totalMs = millis() - playStart;
    LOG2("[play] done: %u bytes in %ums, %d underruns (%ums total)\n",
                  playPos, totalMs, underruns, underrunTotalMs);

    free(stereo);
    if (playGen == myGen) {
        outAmplitude = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(150));
        digitalWrite(AUDIO_PA, LOW);
        paActive  = false;
        playing   = false;
        playTask  = NULL;
    } else {
        LOG3("[play] superseded (gen=%u cur=%u), exiting without state reset\n",
             (unsigned)myGen, (unsigned)playGen);
    }
    vTaskDelete(NULL);
}

// ─── Streaming state (forward-declared so audio_stop_play can reference it) ──
#include "freertos/stream_buffer.h"

#define STREAM_BUF_SIZE (768 * 1024)

static uint8_t             *streamStorage = NULL;   // PSRAM-backed, allocated once
static StaticStreamBuffer_t streamBufStruct;
static StreamBufferHandle_t streamBuf     = NULL;
static volatile bool        streaming     = false;
static volatile bool        streamEnded   = false;
static TaskHandle_t         streamTask    = NULL;

// ─── Public API ─────────────────────────────────────────────────────────────

bool audio_init() {
    pinMode(AUDIO_PA, OUTPUT);
    digitalWrite(AUDIO_PA, LOW);

    // I2S bus shared by ES8311 (speaker DAC on DOUT) and ES7210 (mic ADC on DIN)
    i2s.setPins(I2S_BCLK, I2S_WS, I2S_SDOUT, I2S_SDIN, I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        LOGLN1("[audio] I2S init failed");
        return false;
    }
    LOGLN2("[audio] I2S OK");

    // ES8311 = speaker/DAC
    if (!es8311_codec_init()) {
        LOGLN1("[audio] ES8311 init FAILED");
        return false;
    }
    LOGLN2("[audio] ES8311 speaker OK");

    // ES7210 = dual microphone ADC (this was the missing piece!)
    if (!es7210_init()) {
        LOGLN1("[audio] ES7210 init FAILED (mic disabled)");
    } else {
        LOGLN2("[audio] ES7210 mic OK");
    }

    recBuf = (int16_t *)ps_malloc(MAX_RECORD_BYTES);
    if (!recBuf) {
        LOGLN1("[audio] Record buffer alloc failed");
        return false;
    }

    // Streaming playback buffer — allocated once, reused with xStreamBufferReset
    streamStorage = (uint8_t *)ps_malloc(STREAM_BUF_SIZE + 1);
    if (!streamStorage) {
        LOGLN1("[audio] Stream storage alloc failed");
        return false;
    }
    streamBuf = xStreamBufferCreateStatic(STREAM_BUF_SIZE, 1,
                                          streamStorage, &streamBufStruct);
    LOG2("[audio] Ready (rec %uKB, stream %uKB)\n",
         MAX_RECORD_BYTES / 1024, STREAM_BUF_SIZE / 1024);
    return true;
}

void audio_start_record() {
    if (recording) return;
    recBytes  = 0;
    recording = true;
    event_log(EVT_AUDIO_EVENT, "rec start");
    xTaskCreatePinnedToCore(record_task_fn, "rec", 4096, NULL, 2, &recTask, 0);
}

void audio_stop_record() {
    recording = false;
    event_log_printf(EVT_AUDIO_EVENT, "rec stop %uB", (unsigned)recBytes);
    if (recTask) { vTaskDelay(pdMS_TO_TICKS(50)); }

    size_t samples = recBytes / 2;
    if (samples > 0) {
        int64_t sum = 0;
        int32_t peak = 0;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = recBuf[i];
            sum += s * s;
            int32_t a = (s < 0) ? -s : s;
            if (a > peak) peak = a;
        }
        float rms = sqrtf((float)(sum / (int64_t)samples));
        LOG2("[audio] %u samples, RMS=%.0f, peak=%d\n", samples, rms, peak);
    }
}

bool audio_is_recording() { return recording; }

const int16_t *audio_get_pcm()       { return recBuf; }
size_t         audio_get_pcm_bytes() { return recBytes; }

void audio_play_pcm(const int16_t *data, size_t numBytes) {
    audio_stop_play();
    playPtr  = data;
    playLen  = numBytes;
    playGen++;
    paActive = false;
    playing  = true;
    event_log_printf(EVT_AUDIO_EVENT, "play start %uB", (unsigned)numBytes);
    xTaskCreatePinnedToCore(play_task_fn, "play", 8192, NULL, 5, &playTask, 1);
}

void audio_stop_play() {
    // Bump the generation FIRST.  Any currently-running play task sees
    // playGen != its myGen on its next loop check and bails out
    // immediately (without writing more samples to I2S and without
    // resetting our shared state at exit).  Without this, a stale task
    // can keep pushing audio into the DMA queue while the new task is
    // also pushing — causing audibly jumbled, out-of-order playback.
    playGen++;
    if (playTask) {
        playing = false;
        uint32_t t0 = millis();
        while (playTask && millis() - t0 < 300) vTaskDelay(pdMS_TO_TICKS(5));
        if (playTask) {
            LOGLN1("[audio] play task did not exit in time, force-clearing handle");
            playTask = NULL;
        }
    }
    if (!streaming) {
        playing  = false;
        paActive = false;
        digitalWrite(AUDIO_PA, LOW);
    }
}

bool audio_is_playing()      { return playing; }
bool audio_output_started()  { return paActive; }

void audio_play_progressive(uint8_t *buf) {
    audio_stop_play();   // already bumps playGen — see comment there
    if (streaming || streamTask) {
        // streamTask checks `streaming && playGen == myGen`; the
        // playGen++ inside audio_stop_play() already invalidated it,
        // so it should be exiting on its own now.  Wait for it to
        // actually clear streamTask before we spawn a new task that
        // would otherwise step on its tail end.
        streaming   = false;
        streamEnded = true;
        uint32_t t0 = millis();
        while (streamTask && millis() - t0 < 500) vTaskDelay(pdMS_TO_TICKS(10));
        if (streamTask) {
            LOGLN1("[audio] stream task did not exit in time, force-clearing handle");
            streamTask = NULL;
        }
    }
    playGen++;
    progWritePos   = 0;
    progDone       = false;
    progTotalBytes = 0;   // reset; caller may set via audio_progressive_set_total
    paActive       = false;
    playing        = true;
    xTaskCreatePinnedToCore(prog_play_task, "progplay", 8192, (void *)buf, 5, &playTask, 1);
}

void audio_progressive_update(size_t pos) {
    progWritePos = pos;
}

void audio_progressive_finish() {
    progDone = true;
}

void audio_progressive_set_total(size_t totalBytes) {
    progTotalBytes = totalBytes;
}

// ─── Streaming playback ─────────────────────────────────────────────────────

static const size_t STREAM_PREBUF = 48 * 1024;  // ~1.5s at 16kHz mono 16-bit

// Same "still current?" guard as the play tasks — also gated on the
// per-stream `streaming` flag.  Stream tasks share the same playGen
// counter as prog/play tasks so that audio_play_progressive() / etc.
// reliably retire any earlier stream task too.
#define STREAM_STILL_CURRENT() (streaming && playGen == myGen)

static void stream_play_task(void *) {
    uint32_t myGen = playGen;
    const size_t CHUNK = 256;
    uint8_t rawBuf[CHUNK * 2 + 1];   // +1 for potential leftover byte
    int16_t *stereo = (int16_t *)malloc(CHUNK * 2 * sizeof(int16_t));
    if (!stereo) {
        if (playGen == myGen) { streaming = false; streamTask = NULL; }
        vTaskDelete(NULL); return;
    }

    while (STREAM_STILL_CURRENT() && !streamEnded &&
           xStreamBufferBytesAvailable(streamBuf) < STREAM_PREBUF) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!STREAM_STILL_CURRENT()) {
        free(stereo);
        if (playGen == myGen) { streamTask = NULL; streaming = false; }
        vTaskDelete(NULL); return;
    }

    pinMode(AUDIO_PA, OUTPUT);
    digitalWrite(AUDIO_PA, HIGH);
    paActive = true;

    size_t carry = 0;   // leftover bytes from previous read (0 or 1)

    while (STREAM_STILL_CURRENT()) {
        size_t got = xStreamBufferReceive(streamBuf, rawBuf + carry,
                                          CHUNK * 2 - carry, pdMS_TO_TICKS(30));
        got += carry;

        if (got < 2) {
            if (streamEnded) break;     // no more data coming, drain complete
            if (got == 1) carry = 1;
            memset(stereo, 0, CHUNK * 4);
            i2s.write((uint8_t *)stereo, CHUNK * 4);
            continue;
        }

        // Save any odd trailing byte for next iteration
        if (got & 1) {
            carry = 1;
            rawBuf[0] = rawBuf[got - 1];   // move leftover to start of buffer
            got--;
        } else {
            carry = 0;
        }

        size_t samples = got / 2;
        const int16_t *mono = (const int16_t *)rawBuf;
        update_amplitude(mono, samples);
        for (size_t i = 0; i < samples; i++) {
            stereo[i * 2]     = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        size_t toWrite = samples * 4;
        size_t written = 0;
        while (written < toWrite && STREAM_STILL_CURRENT()) {
            size_t w = i2s.write((uint8_t *)stereo + written, toWrite - written);
            if (w == 0) vTaskDelay(1);
            written += w;
        }
    }

    free(stereo);
    if (playGen == myGen) {
        outAmplitude = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(150));   // let I2S DMA flush to speaker
        digitalWrite(AUDIO_PA, LOW);
        paActive   = false;
        playing    = false;
        streaming  = false;
        streamTask = NULL;
    } else {
        LOG3("[splay] superseded (gen=%u cur=%u), exiting without state reset\n",
             (unsigned)myGen, (unsigned)playGen);
    }
    vTaskDelete(NULL);
}

bool audio_stream_start() {
    audio_stop_play();   // bumps playGen

    if (streaming || streamTask) {
        streaming   = false;
        streamEnded = true;
        uint32_t t0 = millis();
        while (streamTask && millis() - t0 < 500) vTaskDelay(pdMS_TO_TICKS(10));
        if (streamTask) {
            LOGLN1("[audio] stream task did not exit in time, force-clearing handle");
            streamTask = NULL;
        }
    }

    if (!streamBuf) { LOGLN1("[audio] stream buf not initialized"); return false; }
    xStreamBufferReset(streamBuf);

    playGen++;
    streaming   = true;
    streamEnded = false;
    playing     = true;
    xTaskCreatePinnedToCore(stream_play_task, "splay", 8192, NULL, 5, &streamTask, 1);
    return true;
}

bool audio_stream_write(const uint8_t *data, size_t len) {
    if (!streaming || !streamBuf) return false;
    // Non-blocking write — this is called from the NimBLE onWrite callback
    // so we must never block or we'll starve the BLE stack.  The 768KB
    // PSRAM buffer is large enough that drops are extremely rare; if the
    // buffer IS full it means playback fell far behind and losing tail
    // data is the least-bad option.
    size_t sent = 0;
    while (sent < len && streaming) {
        size_t s = xStreamBufferSend(streamBuf, data + sent, len - sent, 0);
        if (s == 0) break;
        sent += s;
    }
    if (sent < len) {
        LOG1("[audio] stream_write dropped %u/%u bytes (buf full)\n",
             (unsigned)(len - sent), (unsigned)len);
    }
    return streaming;
}

void audio_stream_finish() {
    streamEnded = true;
}

void audio_stream_cancel() {
    playGen++;            // invalidate stream task immediately
    streaming   = false;
    streamEnded = true;
    uint32_t t0 = millis();
    while (streamTask && millis() - t0 < 300) vTaskDelay(pdMS_TO_TICKS(10));
    if (streamTask) {
        LOGLN1("[audio] stream task did not exit in time on cancel, force-clearing");
        streamTask = NULL;
    }
    playing = false;
    outAmplitude = 0.0f;
    digitalWrite(AUDIO_PA, LOW);
}

float audio_get_output_amplitude() {
    return outAmplitude;
}

// ─── Low-power transitions ──────────────────────────────────────────────────
//
// Called from the sleep state.  We don't tear down the I2S driver
// itself (re-init is slow and fiddly with the ES7210 mic) — just stop
// any in-flight playback/recording, drop the PA enable line, and mute
// the codec so it parks at its quiescent current (≈ 0.5 mA → ~10 µA on
// the ES8311 once muted, and the PA pulls another ~5 mA when on).
//
// I2S clocks themselves keep running but with no traffic the codec
// doesn't actually do anything; for max savings we'd kill the I2S
// peripheral too, but that means full re-init on wake which adds
// hundreds of ms.  Not worth it for the few mA saved here when the
// real wins are in the display panel and the CPU light-sleep loop.

void audio_enter_low_power() {
    audio_stop_play();
    audio_stop_record();
    audio_stream_cancel();
    digitalWrite(AUDIO_PA, LOW);
    paActive = false;
    if (es8311) {
        es8311_voice_mute(es8311, true);
    }
}

void audio_exit_low_power() {
    if (es8311) {
        es8311_voice_mute(es8311, false);
    }
}
