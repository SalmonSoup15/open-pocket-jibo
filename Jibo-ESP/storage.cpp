#include "storage.h"
#include <Preferences.h>
#include <vector>

static Preferences prefs;
static const char *NS = "jibo";

// ─── Permanent memory cache ─────────────────────────────────────────────────
// Loaded from NVS at boot, kept in RAM, persisted on every write.  Reading
// is hot (every Gemini request builds the system prompt) so we don't want
// to hit NVS for it.
static std::vector<PermMemoryEntry> permMem;
static bool                         permLoaded = false;

static void perm_mem_serialize_and_save();

static void perm_mem_load() {
    if (permLoaded) return;
    permLoaded = true;
    permMem.clear();

    size_t blobLen = prefs.getBytesLength("pm_blob");
    if (blobLen >= 2) {
        std::vector<uint8_t> buf(blobLen);
        prefs.getBytes("pm_blob", buf.data(), blobLen);
        size_t p = 0;
        if (p + 2 <= blobLen) {
            uint16_t count = (uint16_t)buf[p] | ((uint16_t)buf[p + 1] << 8);
            p += 2;
            for (uint16_t i = 0; i < count && p + 18 <= blobLen; i++) {
                PermMemoryEntry e;
                e.id = 0;
                for (int k = 0; k < 8; k++) e.id |= (uint64_t)buf[p + k] << (k * 8);
                p += 8;
                e.ts_ms = 0;
                for (int k = 0; k < 8; k++) e.ts_ms |= (uint64_t)buf[p + k] << (k * 8);
                p += 8;
                uint16_t tlen = (uint16_t)buf[p] | ((uint16_t)buf[p + 1] << 8);
                p += 2;
                if (p + tlen > blobLen) break;
                e.text.reserve(tlen);
                for (uint16_t k = 0; k < tlen; k++) e.text += (char)buf[p + k];
                p += tlen;
                permMem.push_back(e);
            }
        }
    }

    // Old builds also kept a "pm_tomb" tombstone blob.  The new design
    // doesn't need it — wipe any leftover so it doesn't waste NVS on
    // upgraded devices.
    if (prefs.getBytesLength("pm_tomb") > 0) {
        prefs.remove("pm_tomb");
    }
}

static void perm_mem_serialize_and_save() {
    // Header: uint16 count.  Body: 18 + text bytes per entry.
    size_t total = 2;
    for (auto &e : permMem) total += 18 + e.text.length();
    std::vector<uint8_t> buf(total);
    size_t p = 0;
    buf[p++] = (uint8_t)(permMem.size() & 0xFF);
    buf[p++] = (uint8_t)((permMem.size() >> 8) & 0xFF);
    for (auto &e : permMem) {
        for (int k = 0; k < 8; k++) buf[p++] = (uint8_t)((e.id >> (k * 8)) & 0xFF);
        for (int k = 0; k < 8; k++) buf[p++] = (uint8_t)((e.ts_ms >> (k * 8)) & 0xFF);
        uint16_t tlen = (uint16_t)e.text.length();
        buf[p++] = (uint8_t)(tlen & 0xFF);
        buf[p++] = (uint8_t)((tlen >> 8) & 0xFF);
        for (uint16_t k = 0; k < tlen; k++) buf[p++] = (uint8_t)e.text[k];
    }
    prefs.putBytes("pm_blob", buf.data(), total);
}

void storage_init() {
    prefs.begin(NS, false);
    perm_mem_load();
}

void storage_factory_reset() {
    prefs.clear();
    prefs.end();
    prefs.begin(NS, false);
    permMem.clear();
}

uint8_t storage_wifi_count() {
    return prefs.getUChar("wifi_count", 0);
}

void storage_set_wifi_count(uint8_t count) {
    if (count > MAX_WIFI_NETWORKS) count = MAX_WIFI_NETWORKS;
    prefs.putUChar("wifi_count", count);
}

bool storage_get_wifi(uint8_t idx, WifiEntry &out) {
    if (idx >= MAX_WIFI_NETWORKS) return false;
    char key[20];
    snprintf(key, sizeof(key), "w%d_ssid", idx);
    out.ssid = prefs.getString(key, "");
    if (out.ssid.isEmpty()) return false;

    snprintf(key, sizeof(key), "w%d_pass", idx);
    out.password = prefs.getString(key, "");
    snprintf(key, sizeof(key), "w%d_user", idx);
    out.username = prefs.getString(key, "");
    snprintf(key, sizeof(key), "w%d_ent", idx);
    out.enterprise = prefs.getBool(key, false);
    return true;
}

void storage_set_wifi(uint8_t idx, const WifiEntry &entry) {
    if (idx >= MAX_WIFI_NETWORKS) return;
    char key[20];
    snprintf(key, sizeof(key), "w%d_ssid", idx);
    prefs.putString(key, entry.ssid);
    snprintf(key, sizeof(key), "w%d_pass", idx);
    prefs.putString(key, entry.password);
    snprintf(key, sizeof(key), "w%d_user", idx);
    prefs.putString(key, entry.username);
    snprintf(key, sizeof(key), "w%d_ent", idx);
    prefs.putBool(key, entry.enterprise);
}

String storage_get_api_key() {
    return prefs.getString("api_key", "");
}

void storage_set_api_key(const String &key) {
    prefs.putString("api_key", key);
}

String storage_get_tts_key() {
    return prefs.getString("tts_key", "");
}

void storage_set_tts_key(const String &key) {
    prefs.putString("tts_key", key);
}

bool storage_is_setup_done() {
    return prefs.getBool("setup_done", false);
}

void storage_set_setup_done(bool done) {
    prefs.putBool("setup_done", done);
}

uint16_t storage_device_suffix() {
    uint64_t mac = ESP.getEfuseMac();
    return (uint16_t)(mac % 10000);
}

uint8_t storage_get_brightness() {
    return prefs.getUChar("bright", 2);
}
void storage_set_brightness(uint8_t level) {
    if (level > 2) level = 2;
    prefs.putUChar("bright", level);
}

uint8_t storage_get_model() {
    return prefs.getUChar("model", 0);
}
void storage_set_model(uint8_t m) {
    if (m > 2) m = 2;
    prefs.putUChar("model", m);
}

uint8_t storage_get_voice() {
    return prefs.getUChar("voice", 0);
}
void storage_set_voice(uint8_t v) {
    if (v > 5) v = 5;
    prefs.putUChar("voice", v);
}

bool storage_get_memory_enabled() {
    return prefs.getBool("mem_on", true);
}
void storage_set_memory_enabled(bool on) {
    prefs.putBool("mem_on", on);
}

String storage_get_conv_history() {
    return prefs.getString("conv_hist", "");
}
void storage_set_conv_history(const String &h) {
    if (h.length() < 3900) {
        prefs.putString("conv_hist", h);
    } else {
        // Trim oldest turns: find a model turn boundary "{\"role\":\"model\""
        // then take everything from the user turn after it
        int cut = h.indexOf("{\"role\":\"user\"", 10);
        if (cut > 0) {
            String trimmed = h.substring(cut);
            prefs.putString("conv_hist", trimmed);
        } else {
            prefs.remove("conv_hist");
        }
    }
}
void storage_clear_conv_history() {
    prefs.remove("conv_hist");
}

bool storage_get_wifi_enabled() {
    return prefs.getBool("wifi_on", true);
}
void storage_set_wifi_enabled(bool on) {
    prefs.putBool("wifi_on", on);
}

bool storage_get_ble_paired() {
    return prefs.getBool("ble_pair", false);
}
void storage_set_ble_paired(bool paired) {
    prefs.putBool("ble_pair", paired);
}

String storage_get_ble_addr() {
    return prefs.getString("ble_addr", "");
}
void storage_set_ble_addr(const String &addr) {
    prefs.putString("ble_addr", addr);
}

String storage_get_ble_name() {
    return prefs.getString("ble_name", "");
}
void storage_set_ble_name(const String &name) {
    prefs.putString("ble_name", name);
}

uint8_t storage_get_sleep_timeout() {
    return prefs.getUChar("slp_time", 4);
}
void storage_set_sleep_timeout(uint8_t idx) {
    if (idx > 7) idx = 7;
    prefs.putUChar("slp_time", idx);
}

bool storage_get_imu_cal(float &bx, float &by, float &bz) {
    float buf[3];
    size_t len = prefs.getBytes("imu_cal", buf, sizeof(buf));
    if (len != sizeof(buf)) { bx = by = bz = 0; return false; }
    bx = buf[0]; by = buf[1]; bz = buf[2];
    return true;
}
void storage_set_imu_cal(float bx, float by, float bz) {
    float buf[3] = {bx, by, bz};
    prefs.putBytes("imu_cal", buf, sizeof(buf));
}

bool storage_get_dev_mode() {
    return prefs.getBool("dev_mode", false);
}
void storage_set_dev_mode(bool on) {
    prefs.putBool("dev_mode", on);
}

bool storage_get_dev_transitioning() {
    return prefs.getBool("dev_trans", false);
}
void storage_set_dev_transitioning(bool on) {
    prefs.putBool("dev_trans", on);
}

bool storage_get_dev_verbose_overlay() {
    return prefs.getBool("dev_verb", false);
}
void storage_set_dev_verbose_overlay(bool on) {
    prefs.putBool("dev_verb", on);
}

bool storage_get_dev_stats_pill() {
    return prefs.getBool("dev_stat", false);
}
void storage_set_dev_stats_pill(bool on) {
    prefs.putBool("dev_stat", on);
}

bool storage_get_dev_verbose_boot() {
    return prefs.getBool("dev_vboot", false);
}
void storage_set_dev_verbose_boot(bool on) {
    prefs.putBool("dev_vboot", on);
}

void storage_remove_wifi(uint8_t idx) {
    uint8_t count = storage_wifi_count();
    if (idx >= count) return;
    for (uint8_t i = idx; i < count - 1; i++) {
        WifiEntry e;
        storage_get_wifi(i + 1, e);
        storage_set_wifi(i, e);
    }
    char key[20];
    uint8_t last = count - 1;
    snprintf(key, sizeof(key), "w%d_ssid", last); prefs.remove(key);
    snprintf(key, sizeof(key), "w%d_pass", last); prefs.remove(key);
    snprintf(key, sizeof(key), "w%d_user", last); prefs.remove(key);
    snprintf(key, sizeof(key), "w%d_ent",  last); prefs.remove(key);
    storage_set_wifi_count(count - 1);
}

// ─── Permanent memory ───────────────────────────────────────────────────────

uint16_t storage_perm_mem_count() {
    perm_mem_load();
    return (uint16_t)permMem.size();
}

bool storage_perm_mem_get(uint16_t idx, PermMemoryEntry &out) {
    perm_mem_load();
    if (idx >= permMem.size()) return false;
    out = permMem[idx];
    return true;
}

size_t storage_perm_mem_total_bytes() {
    perm_mem_load();
    size_t total = 2;
    for (auto &e : permMem) total += 18 + e.text.length();
    return total;
}

bool storage_perm_mem_add(uint64_t id, uint64_t ts_ms, const String &text) {
    perm_mem_load();
    if (text.length() == 0) return false;
    String t = text;
    if (t.length() > PERM_MEM_TEXT_MAX) t.remove(PERM_MEM_TEXT_MAX);

    // Upsert by id.
    for (auto &e : permMem) {
        if (e.id == id) {
            e.ts_ms = ts_ms;
            e.text  = t;
            perm_mem_serialize_and_save();
            return true;
        }
    }

    // Drop oldest until we're under both caps.  permMem is kept in
    // insertion order (first is oldest).
    PermMemoryEntry add;
    add.id = id;
    add.ts_ms = ts_ms;
    add.text = t;

    auto over_caps = [&]() {
        if (permMem.size() >= PERM_MEM_MAX) return true;
        size_t total = 18 + add.text.length();
        for (auto &e : permMem) total += 18 + e.text.length();
        return total > PERM_MEM_MAX_BYTES;
    };
    while (!permMem.empty() && over_caps()) {
        permMem.erase(permMem.begin());
    }
    permMem.push_back(add);
    perm_mem_serialize_and_save();
    return true;
}

bool storage_perm_mem_remove(uint64_t id) {
    perm_mem_load();
    bool removed = false;
    for (auto it = permMem.begin(); it != permMem.end(); ) {
        if (it->id == id) { it = permMem.erase(it); removed = true; }
        else ++it;
    }
    if (removed) perm_mem_serialize_and_save();
    return removed;
}

void storage_perm_mem_clear() {
    perm_mem_load();
    permMem.clear();
    perm_mem_serialize_and_save();
}
