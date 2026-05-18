#pragma once
#include <stdint.h>

// ─── Message data structures ───────────────────────────────────────────────
// Owned by the messages back-end (another unit). This header defines the
// types and function declarations that the UI layer depends on.

#define MSG_MAX_NAME     32
#define MSG_MAX_SNIPPET  64
#define MSG_MAX_BODY     512
#define MSG_MAX_APP      16

// A single message inside a conversation thread.
struct MsgMessage {
    char     body[MSG_MAX_BODY];
    char     sender_name[MSG_MAX_NAME];  // empty for outgoing
    bool     outgoing;
    uint32_t timestamp;                  // epoch seconds
};

// A conversation summary shown in the chat list.
struct MsgConversation {
    char     contact_name[MSG_MAX_NAME];
    char     snippet[MSG_MAX_SNIPPET];
    uint8_t  unread_count;
    uint32_t last_timestamp;
    uint16_t convo_id;                   // unique conversation ID
};

// A candidate for contact disambiguation.
struct MsgContact {
    char     name[MSG_MAX_NAME];
    char     app[MSG_MAX_APP];           // "SMS", "WhatsApp", etc.
    uint16_t contact_id;
};

// ─── Back-end API (implemented in messages.cpp) ────────────────────────────

// Conversation list
int                   msg_convo_count();
const MsgConversation *msg_convo_at(int index);
bool                  msg_conversations_changed();  // true if list updated since last call

// Thread messages for a given conversation
int                   msg_thread_count(uint16_t convo_id);
const MsgMessage     *msg_thread_at(uint16_t convo_id, int index);
bool                  msg_thread_changed(uint16_t convo_id);

// Disambiguation candidates
int                   msg_disambig_count();
const MsgContact     *msg_disambig_at(int index);

// Send action
void                  msg_send(uint16_t contact_id, const char *text);

// Loading state
bool                  msg_is_loading();

// ─── Compose state (voice-to-text flow) ───────────────────────────────────
enum MsgComposeState : uint8_t {
    COMPOSE_IDLE = 0,
    COMPOSE_RECORDING,
    COMPOSE_TRANSCRIBING,
    COMPOSE_SHOWING_TEXT,
    COMPOSE_SENDING,
};

struct MsgComposeData {
    String          text;
    String          contact_name;
    MsgComposeState state = COMPOSE_IDLE;
};

MsgComposeData       &msg_compose();

// ─── BLE data ingestion (called from ble_link.cpp handlers) ───────────────
void msg_set_conversations_json(const char *json, size_t len);
void msg_set_conversations_loading(bool loading);
void msg_set_messages_json(const char *json, size_t len);
void msg_set_messages_loading(bool loading);
void msg_set_contacts_json(const char *json, size_t len);
void msg_push_new_message(const char *json, size_t len);
