#pragma once
#include <Arduino.h>

// In-memory data model for phone text messages / conversations mirrored
// from the Android companion app over BLE.  Follows the same patterns
// as notifications.h: static arrays, hand-rolled JSON, _changed/_ack
// dirty tracking, _loading flags for async BLE fetches.

#define MAX_MSG_CONVOS   20
#define MAX_MSG_MESSAGES 30
#define MAX_MSG_CONTACTS 10

struct MsgConversation {
    String   thread_id;
    String   name;
    String   snippet;
    String   app;        // "Messages", "WhatsApp", etc.
    String   app_pkg;    // package name for routing
    uint32_t timestamp;
    uint8_t  unread;
    bool     is_group;
    bool     valid;
};

struct MsgMessage {
    String   sender;     // display name (empty for own messages)
    String   body;
    uint32_t timestamp;
    bool     outgoing;   // true = sent by user
    bool     valid;
};

enum MsgComposeState {
    COMPOSE_IDLE,
    COMPOSE_RECORDING,
    COMPOSE_TRANSCRIBING,
    COMPOSE_SHOWING_TEXT,
    COMPOSE_SENDING,
};

struct MsgComposeData {
    MsgComposeState state;
    String thread_id;
    String contact_id;
    String contact_name;
    String app_pkg;
    String text;
};

struct MsgContact {
    String contact_id;
    String name;
    String phone;       // phone number for sending
    String app;
    String app_pkg;
    bool   valid;
};

void msg_init();

// ─── Conversation list ──────────────────────────────────────────────────────
void                    msg_clear_conversations();
void                    msg_set_conversations_json(const char *json, size_t len);
uint16_t                msg_conversation_count();
const MsgConversation  *msg_get_conversation(uint16_t idx);
bool                    msg_conversations_loading();
void                    msg_set_conversations_loading(bool loading);
bool                    msg_conversations_changed();
void                    msg_conversations_ack();

// ─── Thread messages ────────────────────────────────────────────────────────
void                    msg_clear_messages();
void                    msg_set_messages_json(const char *json, size_t len);
uint16_t                msg_message_count();
const MsgMessage       *msg_get_message(uint16_t idx);
bool                    msg_messages_loading();
void                    msg_set_messages_loading(bool loading);
bool                    msg_messages_changed();
void                    msg_messages_ack();

// ─── Active thread tracking ─────────────────────────────────────────────────
void                    msg_set_active_thread(uint16_t idx);
uint16_t                msg_active_index();
const MsgConversation  *msg_active_conversation();

// ─── Compose ────────────────────────────────────────────────────────────────
MsgComposeData         &msg_compose();
void                    msg_compose_reset();

// ─── Contact search results ─────────────────────────────────────────────────
void                    msg_clear_contacts();
void                    msg_set_contacts_json(const char *json, size_t len);
uint16_t                msg_contact_count();
const MsgContact       *msg_get_contact(uint16_t idx);
bool                    msg_contacts_changed();
void                    msg_contacts_ack();

// ─── New message push ───────────────────────────────────────────────────────
void                    msg_push_new_message(const char *json, size_t len);
bool                    msg_has_new_push();
void                    msg_ack_new_push();
