#pragma once
#include <Arduino.h>

#define MAX_SMS_CONVOS   20
#define MAX_SMS_MESSAGES 20

struct SmsConversation {
    String name;
    String snippet;
    String address;
    uint32_t timestamp;
    uint8_t unread;
    bool valid;
};

struct SmsMessage {
    String body;
    uint32_t timestamp;
    bool outgoing;
    bool valid;
};

void sms_init();

void sms_clear_conversations();
void sms_add_conversation(const String &name, const String &snippet,
                          const String &address, uint32_t ts, uint8_t unread);
uint16_t sms_conversation_count();
const SmsConversation *sms_get_conversation(uint16_t idx);
bool sms_conversations_changed();
void sms_conversations_ack();

void sms_clear_messages();
void sms_add_message(bool outgoing, uint32_t ts, const String &body);
uint16_t sms_message_count();
const SmsMessage *sms_get_message(uint16_t idx);
bool sms_messages_changed();
void sms_messages_ack();
bool sms_messages_done();
void sms_set_messages_done(bool done);

void sms_set_active(uint16_t idx);
uint16_t sms_active_index();
const SmsConversation *sms_active_conversation();
