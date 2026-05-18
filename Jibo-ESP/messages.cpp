#include "messages.h"
#include "log.h"

// ─── Module state (file-scoped statics) ─────────────────────────────────────

static MsgConversation  gConvos[MAX_MSG_CONVOS];
static uint16_t         gConvoCount    = 0;
static bool             gConvosLoading = false;
static bool             gConvosChanged = false;

static MsgMessage       gMessages[MAX_MSG_MESSAGES];
static uint16_t         gMsgCount      = 0;
static bool             gMsgsLoading   = false;
static bool             gMsgsChanged   = false;

static uint16_t         gActiveIdx     = 0;

static MsgComposeData   gCompose;

static MsgContact       gContacts[MAX_MSG_CONTACTS];
static uint16_t         gContactCount   = 0;
static bool             gContactsChanged = false;

static bool             gNewPush       = false;

// ─── Tiny JSON helpers (same approach as notifications.cpp) ─────────────────
//
// We only need to parse simple flat objects with string / number / bool
// fields produced by our own Android client.

static void skip_ws(const char *s, size_t len, size_t &i) {
    while (i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        break;
    }
}

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
    // number, bool, null — scan until delimiter
    while (i < len) {
        char ch = s[i];
        if (ch == ',' || ch == '}' || ch == ']') break;
        i++;
    }
}

// Parse a JSON number at s[i].  Returns 0 on failure.
static uint64_t parse_number(const char *s, size_t len, size_t &i) {
    uint64_t val = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (uint64_t)(s[i] - '0');
        i++;
    }
    return val;
}

// Parse a JSON bool at s[i].  Returns false on failure.
static bool parse_bool(const char *s, size_t len, size_t &i) {
    bool v = (i < len && s[i] == 't');
    while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
    return v;
}

// ─── Conversation JSON parser ───────────────────────────────────────────────
// Expected: {"id":"thread_123","name":"John","snippet":"Hey",
//            "ts":1716052800000,"unread":2,"app":"Messages",
//            "app_pkg":"com.google.android.apps.messaging","is_group":false}

static bool parse_convo_object(const char *s, size_t len, size_t &i,
                               MsgConversation &out) {
    skip_ws(s, len, i);
    if (i >= len || s[i] != '{') return false;
    i++;
    out = MsgConversation();

    while (i < len) {
        skip_ws(s, len, i);
        if (i >= len) return false;
        if (s[i] == '}') { i++; out.valid = true; return true; }
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
            if      (key == "id")      out.thread_id = val;
            else if (key == "name")    out.name      = val;
            else if (key == "snippet") out.snippet   = val;
            else if (key == "app")     out.app       = val;
            else if (key == "app_pkg") out.app_pkg   = val;
        } else if (s[i] == 't' || s[i] == 'f') {
            bool v = parse_bool(s, len, i);
            if (key == "is_group") out.is_group = v;
        } else if (s[i] >= '0' && s[i] <= '9') {
            uint64_t v = parse_number(s, len, i);
            if      (key == "ts")     out.timestamp = (uint32_t)(v / 1000);
            else if (key == "unread") out.unread    = (uint8_t)v;
        } else {
            skip_value(s, len, i);
        }
    }
    return false;
}

// ─── Message JSON parser ────────────────────────────────────────────────────
// Expected: {"sender":"John","text":"Hey","ts":1716052800000,"mine":false}

static bool parse_msg_object(const char *s, size_t len, size_t &i,
                             MsgMessage &out) {
    skip_ws(s, len, i);
    if (i >= len || s[i] != '{') return false;
    i++;
    out = MsgMessage();

    while (i < len) {
        skip_ws(s, len, i);
        if (i >= len) return false;
        if (s[i] == '}') { i++; out.valid = true; return true; }
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
            if      (key == "sender") out.sender = val;
            else if (key == "text")   out.body   = val;
        } else if (s[i] == 't' || s[i] == 'f') {
            bool v = parse_bool(s, len, i);
            if (key == "mine") out.outgoing = v;
        } else if (s[i] >= '0' && s[i] <= '9') {
            uint64_t v = parse_number(s, len, i);
            if (key == "ts") out.timestamp = (uint32_t)(v / 1000);
        } else {
            skip_value(s, len, i);
        }
    }
    return false;
}

// ─── Contact JSON parser ────────────────────────────────────────────────────
// Expected: {"id":"c_456","name":"John","app":"Messages",
//            "app_pkg":"com.google.android.apps.messaging"}

static bool parse_contact_object(const char *s, size_t len, size_t &i,
                                 MsgContact &out) {
    skip_ws(s, len, i);
    if (i >= len || s[i] != '{') return false;
    i++;
    out = MsgContact();

    while (i < len) {
        skip_ws(s, len, i);
        if (i >= len) return false;
        if (s[i] == '}') { i++; out.valid = true; return true; }
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
            if      (key == "id")      out.contact_id = val;
            else if (key == "name")    out.name       = val;
            else if (key == "phone")   out.phone      = val;
            else if (key == "app")     out.app        = val;
            else if (key == "app_pkg") out.app_pkg    = val;
        } else {
            skip_value(s, len, i);
        }
    }
    return false;
}

// ─── Generic array-of-objects parser ────────────────────────────────────────
// Scans a JSON array, calling `parseFn` for each object, storing into
// `arr[0..maxCount-1]`.  Returns the number of objects parsed.

template<typename T, typename ParseFn>
static uint16_t parse_array(const char *s, size_t len,
                            T *arr, uint16_t maxCount,
                            ParseFn parseFn) {
    size_t i = 0;
    skip_ws(s, len, i);
    if (i >= len) return 0;

    bool isArray = (s[i] == '[');
    if (isArray) i++;

    uint16_t count = 0;
    while (i < len && count < maxCount) {
        skip_ws(s, len, i);
        if (i >= len) break;
        if (isArray && s[i] == ']') break;
        if (s[i] == ',') { i++; continue; }
        if (s[i] != '{') { i++; continue; }

        T entry;
        if (parseFn(s, len, i, entry)) {
            arr[count++] = entry;
        } else {
            break;
        }
        if (!isArray) break;
    }
    return count;
}

// ─── Public API ─────────────────────────────────────────────────────────────

void msg_init() {
    gConvoCount      = 0;
    gConvosLoading   = false;
    gConvosChanged   = false;
    gMsgCount        = 0;
    gMsgsLoading     = false;
    gMsgsChanged     = false;
    gActiveIdx       = 0;
    gContactCount    = 0;
    gContactsChanged = false;
    gNewPush         = false;
    msg_compose_reset();
    LOG1("[msg] init\n");
}

// ─── Conversations ──────────────────────────────────────────────────────────

void msg_clear_conversations() {
    for (uint16_t i = 0; i < gConvoCount; i++) gConvos[i] = MsgConversation();
    gConvoCount    = 0;
    gConvosChanged = true;
}

void msg_set_conversations_json(const char *json, size_t len) {
    if (!json || len == 0) return;
    gConvoCount = parse_array(json, len, gConvos, MAX_MSG_CONVOS,
                              parse_convo_object);
    gConvosChanged = true;
    LOG1("[msg] parsed %u conversations\n", gConvoCount);
}

uint16_t msg_conversation_count() { return gConvoCount; }

const MsgConversation *msg_get_conversation(uint16_t idx) {
    if (idx >= gConvoCount) return nullptr;
    return &gConvos[idx];
}

bool msg_conversations_loading()               { return gConvosLoading; }
void msg_set_conversations_loading(bool loading) { gConvosLoading = loading; }
bool msg_conversations_changed()               { return gConvosChanged; }
void msg_conversations_ack()                   { gConvosChanged = false; }

// ─── Thread messages ────────────────────────────────────────────────────────

void msg_clear_messages() {
    for (uint16_t i = 0; i < gMsgCount; i++) gMessages[i] = MsgMessage();
    gMsgCount    = 0;
    gMsgsChanged = true;
}

void msg_set_messages_json(const char *json, size_t len) {
    if (!json || len == 0) return;
    gMsgCount = parse_array(json, len, gMessages, MAX_MSG_MESSAGES,
                            parse_msg_object);
    gMsgsChanged = true;
    LOG1("[msg] parsed %u messages\n", gMsgCount);
}

uint16_t msg_message_count() { return gMsgCount; }

const MsgMessage *msg_get_message(uint16_t idx) {
    if (idx >= gMsgCount) return nullptr;
    return &gMessages[idx];
}

bool msg_messages_loading()               { return gMsgsLoading; }
void msg_set_messages_loading(bool loading) { gMsgsLoading = loading; }
bool msg_messages_changed()               { return gMsgsChanged; }
void msg_messages_ack()                   { gMsgsChanged = false; }

// ─── Active thread tracking ─────────────────────────────────────────────────

void msg_set_active_thread(uint16_t idx) {
    if (idx < gConvoCount) gActiveIdx = idx;
}

uint16_t msg_active_index() { return gActiveIdx; }

const MsgConversation *msg_active_conversation() {
    if (gActiveIdx >= gConvoCount) return nullptr;
    return &gConvos[gActiveIdx];
}

// ─── Compose ────────────────────────────────────────────────────────────────

MsgComposeData &msg_compose() { return gCompose; }

void msg_compose_reset() {
    gCompose.state        = COMPOSE_IDLE;
    gCompose.text         = "";
    gCompose.thread_id    = "";
    gCompose.contact_id   = "";
    gCompose.contact_name = "";
    gCompose.app_pkg      = "";
}

// ─── Contact search results ─────────────────────────────────────────────────

void msg_clear_contacts() {
    for (uint16_t i = 0; i < gContactCount; i++) gContacts[i] = MsgContact();
    gContactCount    = 0;
    gContactsChanged = true;
}

void msg_set_contacts_json(const char *json, size_t len) {
    if (!json || len == 0) return;
    gContactCount = parse_array(json, len, gContacts, MAX_MSG_CONTACTS,
                                parse_contact_object);
    gContactsChanged = true;
    LOG1("[msg] parsed %u contacts\n", gContactCount);
}

uint16_t msg_contact_count() { return gContactCount; }

const MsgContact *msg_get_contact(uint16_t idx) {
    if (idx >= gContactCount) return nullptr;
    return &gContacts[idx];
}

bool msg_contacts_changed() { return gContactsChanged; }
void msg_contacts_ack()     { gContactsChanged = false; }

// ─── New message push ───────────────────────────────────────────────────────

void msg_push_new_message(const char *json, size_t len) {
    if (!json || len == 0) return;
    // If the user is currently viewing the thread that this message
    // belongs to, append it to the messages array.  Otherwise just
    // set the push flag so the UI layer can decide what to do.
    //
    // For now we just set the flag — thread matching is a UI concern.
    gNewPush = true;
    LOG1("[msg] new message pushed\n");
}

bool msg_has_new_push() { return gNewPush; }
void msg_ack_new_push() { gNewPush = false; }
