#include "notifications.h"
#include "log.h"
#include "pill_overlay.h"
#include <vector>
#include <set>

static const size_t MAX_NOTIFS = 50;
static std::vector<NotifEntry> gNotifs;

// Set of notification "fingerprints" we've already pilled.  A
// fingerprint is `key + text + sender`.  This set is used in two
// places:
//
//   • Snapshot ingest (BLE reconnect / boot): every fingerprint in
//     the snapshot is silently added so the snapshot itself never
//     pills, and so a subsequent single-push that arrives with an
//     identical fingerprint mid-snapshot doesn't double-announce.
//   • Sleep-wake catch-up: resume_pill walks gNotifs and pills any
//     entry whose fingerprint isn't in the set yet.  This dedups
//     same-content re-broadcasts that piled up during sleep.
//
// Single-push (awake) does *not* consult this set for the dedup
// decision — every awake push is announced.  That's what the user
// wants: a "send notification" tap should always pill, even with
// identical content (since the user explicitly re-posted).  The set
// is still updated on every awake announcement so a snapshot that
// follows can't re-pill the same fingerprint.
//
// Cleared on `notifications_clear_all()`.
static std::set<String> gAnnouncedFps;

static String fingerprint_for(const NotifEntry &e) {
    // \x1F (US, "unit separator") is a non-printing ASCII control byte
    // that won't legitimately appear inside any of these fields, so we
    // can safely use it as a delimiter without ambiguity.
    String fp = e.key;
    fp += '\x1F';
    fp += e.text;
    fp += '\x1F';
    fp += e.sender;
    return fp;
}

// Pill-pause state.  We have two flavours of "paused":
//
//   • Silent pause (gSilentPause = true) — used during boot.  Anything
//     that arrives is silently absorbed (marked announced, no pill, no
//     catch-up later).  Cleared by notifications_boot_complete().
//
//   • Sleep pause (gSilentPause = false) — used while Jibo is asleep.
//     Arrivals are NOT marked announced, so when resume_pill() runs
//     it can fire a single catch-up pill summarising everything that
//     landed during the sleep window.
//
// The default state on boot is silent pause — first IDLE entry calls
// notifications_boot_complete() to release.
static bool gPillPaused   = true;
static bool gSilentPause  = true;

// ─── Tiny JSON helpers (hand-rolled to avoid ArduinoJson dependency) ────────
//
// We only need to parse simple flat objects with string / bool fields produced
// by our own Android client, so a forgiving scanner is good enough.

static void skip_ws(const char *s, size_t len, size_t &i) {
    while (i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        break;
    }
}

// Parse a JSON string starting at s[i]==' " '.  Advances i past the closing
// quote.  Returns the unescaped string.
static String parse_string(const char *s, size_t len, size_t &i) {
    String out;
    if (i >= len || s[i] != '"') return out;
    i++;
    while (i < len) {
        char c = s[i++];
        if (c == '"') break;
        if (c == '\\' && i < len) {
            char esc = s[i++];
            switch (esc) {
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'u': {
                    if (i + 4 > len) break;
                    char hex[5] = {s[i], s[i+1], s[i+2], s[i+3], 0};
                    i += 4;
                    long cp = strtol(hex, NULL, 16);
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += esc; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Skip a JSON value (string / object / array / literal).  Best-effort.
static void skip_value(const char *s, size_t len, size_t &i) {
    skip_ws(s, len, i);
    if (i >= len) return;
    char c = s[i];
    if (c == '"') { (void)parse_string(s, len, i); return; }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 1;
        i++;
        while (i < len && depth > 0) {
            char ch = s[i];
            if (ch == '"') { (void)parse_string(s, len, i); continue; }
            if (ch == open)  depth++;
            if (ch == close) depth--;
            i++;
        }
        return;
    }
    while (i < len) {
        char ch = s[i];
        if (ch == ',' || ch == '}' || ch == ']') break;
        i++;
    }
}

// Parse a single notification object starting at s[i]=='{'.  Advances i past
// the closing '}'.  Returns false if the object is malformed.
static bool parse_object(const char *s, size_t len, size_t &i, NotifEntry &out) {
    skip_ws(s, len, i);
    if (i >= len || s[i] != '{') return false;
    i++;
    out = NotifEntry();
    out.postTimeMs = millis();

    while (i < len) {
        skip_ws(s, len, i);
        if (i >= len) return false;
        if (s[i] == '}') { i++; return true; }
        if (s[i] == ',') { i++; continue; }

        if (s[i] != '"') { skip_value(s, len, i); continue; }
        String key = parse_string(s, len, i);
        skip_ws(s, len, i);
        if (i >= len || s[i] != ':') return false;
        i++;
        skip_ws(s, len, i);
        if (i >= len) return false;

        if (s[i] == '"') {
            String val = parse_string(s, len, i);
            if      (key == "key")      out.key      = val;
            else if (key == "app")      out.app      = val;
            else if (key == "sender")   out.sender   = val;
            else if (key == "text")     out.text     = val;
            else if (key == "category") out.category = val;
        } else if (s[i] == 't' || s[i] == 'f') {
            bool v = (s[i] == 't');
            while (i < len && s[i] != ',' && s[i] != '}') i++;
            if (key == "timeSensitive") out.timeSensitive = v;
        } else {
            skip_value(s, len, i);
        }
    }
    return false;
}

// ─── Public API ─────────────────────────────────────────────────────────────

void notifications_init() {
    gNotifs.clear();
    gNotifs.reserve(8);
    gAnnouncedFps.clear();
    // Boot starts in silent pause — see definitions above.  Cleared by
    // notifications_boot_complete() once the device reaches IDLE.
    gPillPaused  = true;
    gSilentPause = true;
}

static void add_or_replace(const NotifEntry &e) {
    if (e.key.length() == 0) return;
    for (auto &n : gNotifs) {
        if (n.key == e.key) { n = e; return; }
    }
    if (gNotifs.size() >= MAX_NOTIFS) {
        gNotifs.erase(gNotifs.begin());
    }
    gNotifs.push_back(e);
}

// Pick the user-facing "from <X>" label for one notif:
//   - messaging-category w/ a sender   →  "Avalon"
//   - everything else                  →  app name (or "App")
static String label_for(const NotifEntry &n) {
    if (n.category == "messaging" && n.sender.length()) return n.sender;
    if (n.app.length())    return n.app;
    if (n.sender.length()) return n.sender;
    return String("App");
}

// Fire the appropriate pill for the set of newly-arrived notifications.
// `newly` are NotifEntry values pulled from gNotifs in arrival order.
static void announce_new(const std::vector<NotifEntry> &newly) {
    if (newly.empty()) return;
    if (gPillPaused)   return;

    if (newly.size() == 1) {
        String sub = String("From ") + label_for(newly[0]);
        pill_show(PILL_ICON_NOTIF, "New Notification", sub.c_str());
        return;
    }

    // Multiple — figure out if they all share one source label.
    String first = label_for(newly[0]);
    bool   homogeneous = true;
    for (size_t i = 1; i < newly.size(); i++) {
        if (label_for(newly[i]) != first) { homogeneous = false; break; }
    }

    char sub[64];
    if (homogeneous) {
        snprintf(sub, sizeof(sub), "%u new notifications from %s",
                 (unsigned)newly.size(), first.c_str());
    } else {
        snprintf(sub, sizeof(sub), "%u new notifications",
                 (unsigned)newly.size());
    }
    pill_show(PILL_ICON_NOTIF, "New Notifications", sub);
}

void notifications_ingest_push(const uint8_t *json, size_t len) {
    if (!json || len == 0) return;
    const char *s = (const char *)json;
    size_t i = 0;
    skip_ws(s, len, i);
    if (i >= len) return;

    bool isArray = false;
    bool snapshot = false;
    if (s[i] == '[') {
        isArray = true;
        snapshot = true;  // arrays are full-snapshot replies — replace buffer
        i++;
    }
    if (snapshot) gNotifs.clear();

    int parsed = 0;
    // For single-push the rule is "every push pills" — we don't gate
    // on the fingerprint being new.  For the snapshot-on-reconnect
    // path we still want to know which fingerprints are new (so a
    // sleep catch-up can pill only the unannounced ones), so we keep
    // both the full list and the new-fingerprint subset.
    std::vector<NotifEntry> entries;
    std::vector<NotifEntry> arrivedNew;
    while (i < len) {
        skip_ws(s, len, i);
        if (i >= len) break;
        if (isArray && s[i] == ']') { i++; break; }
        if (s[i] == ',') { i++; continue; }
        if (s[i] != '{') { i++; continue; }

        NotifEntry e;
        if (parse_object(s, len, i, e)) {
            String fp = fingerprint_for(e);
            bool isNewFp = (e.key.length() > 0 &&
                            gAnnouncedFps.find(fp) == gAnnouncedFps.end());
            add_or_replace(e);
            if (e.key.length()) entries.push_back(e);
            if (isNewFp) arrivedNew.push_back(e);
            parsed++;
        } else {
            break;
        }
        if (!isArray) break;
    }

    LOG1("[notif] ingest: parsed=%d snapshot=%d total=%u entries=%u newFp=%u paused=%d silent=%d\n",
         parsed, (int)snapshot, (unsigned)gNotifs.size(),
         (unsigned)entries.size(), (unsigned)arrivedNew.size(),
         (int)gPillPaused, (int)gSilentPause);

    // ─── Snapshot path ──────────────────────────────────────────────
    // Snapshots are full re-syncs.  They happen at every BLE connect:
    // initial boot, BLE reconnect mid-session, and the reconnect after
    // wake-from-sleep.  In all non-silent cases the rule is the same:
    //
    //   • pill items whose fingerprint we've never announced before
    //     (those are genuinely new since the last connect)
    //   • silently re-assert items we've already pilled (BLE reconnect
    //     re-asserts state; we don't want to spam the user about
    //     notifications they already saw).
    //
    // The single distinction is the "sleep pause" → that's just a
    // marker that says "we're handling a wake-time snapshot, allow
    // announce_new to fire even though gPillPaused is true."
    if (snapshot) {
        if (gSilentPause) {
            // Silent boot grace: absorb everything without pill.
            for (auto &e : gNotifs) {
                if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
            }
            return;
        }

        // Detect fingerprints we haven't announced before — those are
        // notifs that landed while we were disconnected (BLE drop, or
        // the sleep window).  Items we've already pilled stay quiet.
        std::vector<NotifEntry> newInSnapshot;
        for (auto &e : gNotifs) {
            if (e.key.length() &&
                gAnnouncedFps.find(fingerprint_for(e)) == gAnnouncedFps.end()) {
                newInSnapshot.push_back(e);
            }
        }
        for (auto &e : gNotifs) {
            if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
        }

        bool wasSleepPaused = gPillPaused && !gSilentPause;
        if (wasSleepPaused) {
            // Snapshot arrived after wake — release the sleep pause
            // now, so announce_new() below can actually fire.  This
            // beats the 8 s safety-net timer in states.cpp to the
            // punch in the common case.
            gPillPaused = false;
            LOG1("[notif] sleep pause released by wake snapshot\n");
        }

        if (!newInSnapshot.empty()) {
            LOG1("[notif] snapshot catch-up: %u new fingerprint(s)%s\n",
                 (unsigned)newInSnapshot.size(),
                 wasSleepPaused ? " (post-wake)" : "");
            announce_new(newInSnapshot);
        }
        return;
    }

    // ─── Single-push path ───────────────────────────────────────────
    if (entries.empty()) return;

    if (!gPillPaused) {
        // Awake: pill EVERY pushed entry, even ones with a fingerprint
        // we've already seen.  An app re-posting the same notification
        // (or a user re-tapping a notification tester) is intentional
        // and should re-announce.  We still record fingerprints so a
        // snapshot that arrives next won't double-announce them.
        announce_new(entries);
        for (auto &e : entries) {
            if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
        }
    } else if (gSilentPause) {
        // Boot grace: absorb without pill or catch-up.
        for (auto &e : entries) {
            if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
        }
    }
    // else: sleep pause — leave them unannounced so resume_pill()
    // catches them when Jibo wakes up.  resume_pill uses fingerprint
    // dedup so 8 identical Maps re-broadcasts during sleep collapse
    // into a single catch-up entry.
}

void notifications_remove(const String &key) {
    for (auto it = gNotifs.begin(); it != gNotifs.end(); ++it) {
        if (it->key == key) {
            LOG2("[notif] remove key=%s\n", key.c_str());
            gNotifs.erase(it);
            return;
        }
    }
}

void notifications_clear_all() {
    gNotifs.clear();
    gAnnouncedFps.clear();
}

void notifications_pause_pill() {
    // Sleep pause: arrivals stay unannounced so resume_pill can catch
    // them up.  Always switch out of silent mode — sleep takes
    // precedence over boot grace if both somehow happen.
    gPillPaused  = true;
    gSilentPause = false;
}

void notifications_resume_pill() {
    if (!gPillPaused) return;
    bool wasSilent = gSilentPause;
    gPillPaused  = false;
    gSilentPause = false;

    if (wasSilent) {
        // Boot-grace release: belt-and-suspenders mark everything
        // currently buffered as already-seen so a snapshot that races
        // boot can't pill us later.
        for (auto &e : gNotifs) {
            if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
        }
        LOG1("[notif] boot grace cleared (silent)\n");
        return;
    }

    // Sleep-pause release fallback (called only if no wake-time
    // snapshot arrived within ~8 s of wake).  Same dedup rule as the
    // snapshot path: only pill items we haven't pilled before.
    //
    // In the common "BLE never reconnected" case gNotifs still
    // reflects pre-sleep state, every fingerprint is already in
    // gAnnouncedFps, so this fires no pill — exactly what we want
    // (the user already saw those before sleep).
    std::vector<NotifEntry> pending;
    for (auto &e : gNotifs) {
        if (e.key.length()) {
            String fp = fingerprint_for(e);
            if (gAnnouncedFps.find(fp) == gAnnouncedFps.end()) {
                pending.push_back(e);
                gAnnouncedFps.insert(fp);
            }
        }
    }
    if (!pending.empty()) {
        LOG1("[notif] catch-up pill: %u unannounced after pause\n",
             (unsigned)pending.size());
        announce_new(pending);
    }
}

bool notifications_pill_paused() {
    return gPillPaused;
}

void notifications_boot_complete() {
    // Idempotent: only meaningful while we're still in the silent boot
    // grace; once that's been cleared (or replaced by a sleep pause),
    // do nothing.
    if (!gPillPaused || !gSilentPause) return;
    notifications_resume_pill();
}

void notifications_mark_all_seen() {
    for (auto &e : gNotifs) {
        if (e.key.length()) gAnnouncedFps.insert(fingerprint_for(e));
    }
}

size_t notifications_count() {
    return gNotifs.size();
}

// "5 min ago" / "just now" / "2 hr ago"
static String relative_age(uint32_t postMs) {
    uint32_t now = millis();
    uint32_t dt  = (now >= postMs) ? (now - postMs) : 0;
    if (dt < 60UL * 1000UL)              return String("just now");
    uint32_t mins = dt / 60000UL;
    if (mins < 60)                       return String(mins) + " min ago";
    uint32_t hrs = mins / 60;
    if (hrs < 24)                        return String(hrs) + " hr ago";
    uint32_t days = hrs / 24;
    return String(days) + " day" + (days == 1 ? "" : "s") + " ago";
}

String notifications_format_for_prompt() {
    if (gNotifs.empty()) {
        return String("There are no current phone notifications.");
    }

    String out;
    out.reserve(64 + gNotifs.size() * 96);
    out += "You have ";
    out += String((unsigned)gNotifs.size());
    out += (gNotifs.size() == 1) ? " notification" : " notifications";
    out += " on the user's phone right now:\n";

    // Category is purely descriptive ("messaging", "banking", ...) -- the AI
    // judges sensitivity and urgency from the body text, NOT from the tag.
    for (auto &n : gNotifs) {
        out += "- [";
        out += n.category.length() ? n.category : String("other");
        out += "] ";
        out += n.app.length() ? n.app : String("App");
        if (n.sender.length()) {
            out += " from ";
            out += n.sender;
        }
        out += " (";
        out += relative_age(n.postTimeMs);
        out += "): \"";
        // Trim very long bodies so the system prompt doesn't balloon.
        String t = n.text;
        if (t.length() > 240) { t = t.substring(0, 240); t += "..."; }
        out += t;
        out += "\"\n";
    }
    return out;
}
