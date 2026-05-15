#include "sms.h"
#include "log.h"

static SmsConversation convos[MAX_SMS_CONVOS];
static uint16_t convoCount = 0;
static bool convoDirty = false;

static SmsMessage messages[MAX_SMS_MESSAGES];
static uint16_t msgCount = 0;
static bool msgDirty = false;
static bool msgDone  = false;

static uint16_t activeIdx = 0;

void sms_init() {
    convoCount = 0;
    msgCount   = 0;
    convoDirty = false;
    msgDirty   = false;
    msgDone    = false;
    for (int i = 0; i < MAX_SMS_CONVOS; i++) convos[i].valid = false;
    for (int i = 0; i < MAX_SMS_MESSAGES; i++) messages[i].valid = false;
}

void sms_clear_conversations() {
    convoCount = 0;
    for (int i = 0; i < MAX_SMS_CONVOS; i++) convos[i].valid = false;
    convoDirty = true;
    LOG2("[sms] conversations cleared\n");
}

void sms_add_conversation(const String &name, const String &snippet,
                          const String &address, uint32_t ts, uint8_t unread) {
    if (convoCount >= MAX_SMS_CONVOS) return;
    auto &c = convos[convoCount];
    c.name      = name;
    c.snippet   = snippet;
    c.address   = address;
    c.timestamp = ts;
    c.unread    = unread;
    c.valid     = true;
    convoCount++;
    convoDirty = true;
}

uint16_t sms_conversation_count() { return convoCount; }

const SmsConversation *sms_get_conversation(uint16_t idx) {
    if (idx >= convoCount || !convos[idx].valid) return nullptr;
    return &convos[idx];
}

bool sms_conversations_changed() { return convoDirty; }
void sms_conversations_ack()     { convoDirty = false; }

void sms_clear_messages() {
    msgCount = 0;
    msgDone  = false;
    for (int i = 0; i < MAX_SMS_MESSAGES; i++) messages[i].valid = false;
    msgDirty = true;
}

void sms_add_message(bool outgoing, uint32_t ts, const String &body) {
    if (msgCount >= MAX_SMS_MESSAGES) return;
    auto &m = messages[msgCount];
    m.body      = body;
    m.timestamp = ts;
    m.outgoing  = outgoing;
    m.valid     = true;
    msgCount++;
    msgDirty = true;
}

uint16_t sms_message_count()  { return msgCount; }

const SmsMessage *sms_get_message(uint16_t idx) {
    if (idx >= msgCount || !messages[idx].valid) return nullptr;
    return &messages[idx];
}

bool sms_messages_changed() { return msgDirty; }
void sms_messages_ack()     { msgDirty = false; }
bool sms_messages_done()    { return msgDone; }
void sms_set_messages_done(bool done) { msgDone = done; }

void sms_set_active(uint16_t idx) { activeIdx = idx; }
uint16_t sms_active_index()       { return activeIdx; }

const SmsConversation *sms_active_conversation() {
    return sms_get_conversation(activeIdx);
}
