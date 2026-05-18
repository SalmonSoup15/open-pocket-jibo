#include "messages_ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include "ui_helpers.h"
#include "states.h"
#include "messages.h"
#include "ble_link.h"
#include "pin_config.h"
#include "log.h"

// ─── Sub-page state machine ───────────────────────────────────────────────
enum MsgSubPage {
    MSG_PAGE_CONVO_LIST,
    MSG_PAGE_THREAD,
    MSG_PAGE_COMPOSE,
    MSG_PAGE_DISAMBIGUATE,
};

// ─── File-scoped statics ──────────────────────────────────────────────────
static lv_obj_t  *msgContainer    = NULL;   // main container (list or thread)
static lv_obj_t  *msgOverlay      = NULL;   // overlay for compose / disambiguate
static MsgSubPage msgPage         = MSG_PAGE_CONVO_LIST;
static int        msgPendingPage  = -1;     // pending sub-page transition

// Thread state — track by index into the conversation array
static uint16_t   activeConvoIdx  = 0;
static String     activeConvoName;

// Compose state
static String     composeTo;
static String     composeText;
static String     composeContactId;
static String     composePhone;
static String     composeThreadId;
static bool       composeSendFlag    = false;
static bool       composeEditFlag    = false;
static bool       composeDiscardFlag = false;

// Disambiguate state
static String     disambigName;

// Gemini compose: waiting for contact search results
static bool       composeNeedsContact = false;

// ─── Avatar color helper ──────────────────────────────────────────────────
static lv_color_t avatar_color_for(const char *name) {
    // Simple hash to pick a consistent color per contact
    uint32_t h = 0;
    for (const char *p = name; *p; p++) h = h * 31 + (uint8_t)*p;
    // 8 distinct hues
    switch (h % 8) {
        case 0: return lv_color_make(220, 80,  80);   // red
        case 1: return lv_color_make(60,  160, 255);   // blue
        case 2: return lv_color_make(80,  200, 120);   // green
        case 3: return lv_color_make(255, 180, 40);    // orange
        case 4: return lv_color_make(180, 100, 255);   // purple
        case 5: return lv_color_make(255, 120, 180);   // pink
        case 6: return lv_color_make(60,  200, 200);   // teal
        default: return lv_color_make(200, 200, 100);  // yellow
    }
}

// ─── Forward declarations ─────────────────────────────────────────────────
static void build_convo_list();
static void build_thread_view();
static void build_compose_overlay();
static void build_disambiguate_overlay();
static void destroy_container();
static void destroy_overlay();

// ─── Conversation row click → open thread ─────────────────────────────────
struct ConvoRowData { uint16_t idx; };
static ConvoRowData convoRowPool[16];  // up to 16 visible conversations
static int convoRowPoolIdx = 0;

static void convo_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    ConvoRowData *d = (ConvoRowData *)lv_event_get_user_data(e);

    const MsgConversation *c = msg_get_conversation(d->idx);
    if (!c) return;

    activeConvoIdx  = d->idx;
    activeConvoName = c->name;
    msg_set_active_thread(d->idx);

    // Request thread messages over BLE
    ble_msg_request_thread(c->thread_id);

    msgPendingPage = MSG_PAGE_THREAD;
}

// ─── Compose button callbacks ─────────────────────────────────────────────
static void compose_send_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    composeSendFlag = true;
}

static void compose_edit_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    composeEditFlag = true;
}

static void compose_discard_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    composeDiscardFlag = true;
}

// ─── Disambiguate row click → select contact ─────────────────────────────
struct DisambigRowData { uint16_t idx; };
static DisambigRowData disambigRowPool[8];
static int disambigRowPoolIdx = 0;

static void disambig_row_cb(lv_event_t *e) {
    if (!is_valid_tap()) return;
    block_touch_until_release();
    DisambigRowData *d = (DisambigRowData *)lv_event_get_user_data(e);

    const MsgContact *contact = msg_get_contact(d->idx);
    if (!contact) return;

    composeContactId = contact->contact_id;
    composePhone     = contact->phone;
    composeTo        = contact->name;
    // Proceed to compose confirmation
    msgPendingPage = MSG_PAGE_COMPOSE;
}

// ─── Destroy helpers ──────────────────────────────────────────────────────

static void destroy_container() {
    if (msgContainer) { lv_obj_del(msgContainer); msgContainer = NULL; }
}

static void destroy_overlay() {
    if (msgOverlay) { lv_obj_del(msgOverlay); msgOverlay = NULL; }
}

// ═══════════════════════════════════════════════════════════════════════════
//  BUILD: Conversation List (MSG_PAGE_CONVO_LIST)
// ═══════════════════════════════════════════════════════════════════════════

static void build_convo_list() {
    destroy_container();
    convoRowPoolIdx = 0;

    msgContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(msgContainer);
    lv_obj_set_size(msgContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(msgContainer, 0, 0);
    lv_obj_clear_flag(msgContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(msgContainer);
    lv_label_set_text(title, "Messages");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 35);

    uint16_t count = msg_conversation_count();

    // Loading state
    if (msg_conversations_loading()) {
        lv_obj_t *lbl = lv_label_create(msgContainer);
        lv_label_set_text(lbl, "Loading...");
        lv_obj_set_style_text_color(lbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        block_touch_until_release();
        fade_in_children(msgContainer, 150);
        return;
    }

    // Empty state
    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(msgContainer);
        lv_label_set_text(lbl, "No conversations");
        lv_obj_set_style_text_color(lbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        block_touch_until_release();
        fade_in_children(msgContainer, 150);
        return;
    }

    // Scrollable list
    lv_obj_t *list = lv_obj_create(msgContainer);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCR_W, UI_SCR_H - 75);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    uint16_t maxRows = count < 16 ? count : 16;
    for (uint16_t i = 0; i < maxRows; i++) {
        const MsgConversation *c = msg_get_conversation(i);
        if (!c) continue;

        // Row button
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, 380, 70);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);

        // Store callback data
        if (convoRowPoolIdx < 16) {
            convoRowPool[convoRowPoolIdx].idx = i;
            lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(row, convo_row_cb, LV_EVENT_CLICKED,
                                &convoRowPool[convoRowPoolIdx]);
            convoRowPoolIdx++;
        }

        // Avatar circle (32x32) with first letter
        lv_obj_t *avatar = lv_obj_create(row);
        lv_obj_remove_style_all(avatar);
        lv_obj_set_size(avatar, 32, 32);
        lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(avatar, 16, 0);
        lv_obj_set_style_bg_color(avatar, avatar_color_for(c->name.c_str()), 0);
        lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        char initial[2] = { 0, 0 };
        if (c->name.length() > 0) initial[0] = toupper((unsigned char)c->name[0]);
        lv_obj_t *initLbl = lv_label_create(avatar);
        lv_label_set_text(initLbl, initial);
        lv_obj_set_style_text_color(initLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(initLbl, &lv_font_montserrat_16, 0);
        lv_obj_center(initLbl);

        // Contact name
        lv_obj_t *nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, c->name.c_str());
        lv_obj_set_style_text_color(nameLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_20, 0);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nameLbl, 260);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 42, 4);

        // Snippet
        lv_obj_t *snippetLbl = lv_label_create(row);
        lv_label_set_text(snippetLbl, c->snippet.c_str());
        lv_obj_set_style_text_color(snippetLbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(snippetLbl, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(snippetLbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(snippetLbl, 260);
        lv_obj_align(snippetLbl, LV_ALIGN_BOTTOM_LEFT, 42, -4);

        // Unread badge
        if (c->unread > 0) {
            lv_obj_t *badge = lv_obj_create(row);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, 22, 22);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_set_style_radius(badge, 11, 0);
            lv_obj_set_style_bg_color(badge, lv_color_make(60, 160, 255), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

            char countBuf[4];
            snprintf(countBuf, sizeof(countBuf), "%d",
                     c->unread > 99 ? 99 : c->unread);
            lv_obj_t *countLbl = lv_label_create(badge);
            lv_label_set_text(countLbl, countBuf);
            lv_obj_set_style_text_color(countLbl, lv_color_white(), 0);
            lv_obj_set_style_text_font(countLbl, &lv_font_montserrat_12, 0);
            lv_obj_center(countLbl);
        }
    }

    // Bottom spacer for round screen scrolling
    lv_obj_t *spacer = lv_obj_create(list);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 100);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    msg_conversations_ack();
    block_touch_until_release();
    fade_in_children(msgContainer, 150);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BUILD: Thread View (MSG_PAGE_THREAD)
// ═══════════════════════════════════════════════════════════════════════════

static void build_thread_view() {
    destroy_container();

    msgContainer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(msgContainer);
    lv_obj_set_size(msgContainer, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(msgContainer, 0, 0);
    lv_obj_clear_flag(msgContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Header: contact name
    lv_obj_t *header = lv_label_create(msgContainer);
    lv_label_set_text(header, activeConvoName.c_str());
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 35);

    // Loading state
    if (msg_messages_loading()) {
        lv_obj_t *lbl = lv_label_create(msgContainer);
        lv_label_set_text(lbl, "Loading...");
        lv_obj_set_style_text_color(lbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        block_touch_until_release();
        fade_in_children(msgContainer, 150);
        return;
    }

    // Scrollable message area
    lv_obj_t *msgArea = lv_obj_create(msgContainer);
    lv_obj_remove_style_all(msgArea);
    lv_obj_set_size(msgArea, UI_SCR_W - 40, UI_SCR_H - 80);
    lv_obj_align(msgArea, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_set_flex_flow(msgArea, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(msgArea, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(msgArea, 6, 0);
    lv_obj_set_style_pad_left(msgArea, 10, 0);
    lv_obj_set_style_pad_right(msgArea, 10, 0);
    lv_obj_add_flag(msgArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(msgArea, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(msgArea, LV_SCROLLBAR_MODE_OFF);

    uint16_t count = msg_message_count();
    bool isGroup = false;

    // Detect group chat: if any incoming message has a non-empty sender
    // different from the convo name, it's a group chat
    for (uint16_t i = 0; i < count && !isGroup; i++) {
        const MsgMessage *m = msg_get_message(i);
        if (m && !m->outgoing && m->sender.length() > 0) {
            if (m->sender != activeConvoName) {
                isGroup = true;
            }
        }
    }

    for (uint16_t i = 0; i < count; i++) {
        const MsgMessage *m = msg_get_message(i);
        if (!m) continue;

        // Group chat: show sender name above incoming bubbles
        if (isGroup && !m->outgoing && m->sender.length() > 0) {
            lv_obj_t *senderLbl = lv_label_create(msgArea);
            lv_label_set_text(senderLbl, m->sender.c_str());
            lv_obj_set_style_text_color(senderLbl,
                avatar_color_for(m->sender.c_str()), 0);
            lv_obj_set_style_text_font(senderLbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_pad_left(senderLbl, 4, 0);
        }

        // Message bubble
        lv_obj_t *bubble = lv_obj_create(msgArea);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_style_radius(bubble, 12, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(bubble, 10, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        if (m->outgoing) {
            lv_obj_set_style_bg_color(bubble, lv_color_make(60, 160, 255), 0);
            lv_obj_set_style_max_width(bubble, 280, 0);
            // Right-align within the flex column
            lv_obj_set_align(bubble, LV_ALIGN_TOP_RIGHT);
        } else {
            lv_obj_set_style_bg_color(bubble, lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_max_width(bubble, 280, 0);
        }

        lv_obj_t *txt = lv_label_create(bubble);
        lv_label_set_text(txt, m->body.c_str());
        lv_obj_set_style_text_color(txt, lv_color_white(), 0);
        lv_obj_set_style_text_font(txt, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_width(txt, 260, 0);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    }

    // Bottom spacer for round screen
    lv_obj_t *spacer = lv_obj_create(msgArea);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 80);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(msgArea, LV_COORD_MAX, LV_ANIM_OFF);

    msg_messages_ack();
    block_touch_until_release();
    fade_in_children(msgContainer, 150);
}

// ─── Shared overlay setup ─────────────────────────────────────────────────
// Creates the dimmed full-screen background and a centered card.
// Returns the card object. Sets msgOverlay to the background.
static lv_obj_t *create_dimmed_card(lv_coord_t card_w, lv_coord_t card_h) {
    destroy_overlay();

    msgOverlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(msgOverlay);
    lv_obj_set_size(msgOverlay, UI_SCR_W, UI_SCR_H);
    lv_obj_set_pos(msgOverlay, 0, 0);
    lv_obj_set_style_bg_color(msgOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(msgOverlay, LV_OPA_70, 0);
    lv_obj_clear_flag(msgOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(msgOverlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(msgOverlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_make(25, 25, 25), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    return card;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BUILD: Compose Confirmation (MSG_PAGE_COMPOSE)
// ═══════════════════════════════════════════════════════════════════════════

static void build_compose_overlay() {
    composeSendFlag = false;
    composeEditFlag = false;
    composeDiscardFlag = false;

    lv_obj_t *card = create_dimmed_card(380, 340);

    // "To: [Name]"
    String toBuf = "To: " + composeTo;
    lv_obj_t *toLbl = lv_label_create(card);
    lv_label_set_text(toLbl, toBuf.c_str());
    lv_obj_set_style_text_color(toLbl, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(toLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(toLbl, LV_ALIGN_TOP_MID, 0, 16);

    // Message text (scrollable if long)
    lv_obj_t *textArea = lv_obj_create(card);
    lv_obj_remove_style_all(textArea);
    lv_obj_set_size(textArea, 350, 200);
    lv_obj_align(textArea, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(textArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(textArea, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(textArea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(textArea, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *msgLbl = lv_label_create(textArea);
    lv_label_set_text(msgLbl, composeText.c_str());
    lv_obj_set_style_text_color(msgLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(msgLbl, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(msgLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msgLbl, 340);

    // Button row at bottom
    lv_obj_t *btnRow = lv_obj_create(card);
    lv_obj_remove_style_all(btnRow);
    lv_obj_set_size(btnRow, 360, 50);
    lv_obj_align(btnRow, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);

    // Helper lambda for creating action buttons
    auto make_btn = [&](const char *text, lv_color_t borderCol,
                        lv_color_t textCol, lv_event_cb_t cb) {
        lv_obj_t *btn = lv_btn_create(btnRow);
        lv_obj_set_size(btn, 100, 40);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, borderCol, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, tap_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, textCol, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);
    };

    make_btn("Send",    lv_color_make(60, 160, 255), lv_color_white(),
             compose_send_cb);
    make_btn("Edit",    lv_color_make(80, 80, 80),   lv_color_white(),
             compose_edit_cb);
    make_btn("Discard", lv_color_make(80, 80, 80),   lv_color_make(140, 140, 140),
             compose_discard_cb);

    block_touch_until_release();
    fade_in_children(msgOverlay, 150);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BUILD: Contact Disambiguation (MSG_PAGE_DISAMBIGUATE)
// ═══════════════════════════════════════════════════════════════════════════

static void build_disambiguate_overlay() {
    disambigRowPoolIdx = 0;

    lv_obj_t *card = create_dimmed_card(380, 360);

    // Title: "Which [name]?"
    String titleBuf = "Which " + disambigName + "?";
    lv_obj_t *titleLbl = lv_label_create(card);
    lv_label_set_text(titleLbl, titleBuf.c_str());
    lv_obj_set_style_text_color(titleLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_24, 0);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 20);

    // Scrollable contact list
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 350, 280);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    uint16_t count = msg_contact_count();
    uint16_t maxRows = count < 8 ? count : 8;

    for (uint16_t i = 0; i < maxRows; i++) {
        const MsgContact *contact = msg_get_contact(i);
        if (!contact) continue;

        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, 330, 56);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);

        if (disambigRowPoolIdx < 8) {
            disambigRowPool[disambigRowPoolIdx].idx = i;
            lv_obj_add_event_cb(row, tap_press_cb, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(row, disambig_row_cb, LV_EVENT_CLICKED,
                                &disambigRowPool[disambigRowPoolIdx]);
            disambigRowPoolIdx++;
        }

        // Contact name
        lv_obj_t *nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, contact->name.c_str());
        lv_obj_set_style_text_color(nameLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_20, 0);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 10, 2);

        // "via [App]"
        String viaBuf = "via " + contact->app;
        lv_obj_t *viaLbl = lv_label_create(row);
        lv_label_set_text(viaLbl, viaBuf.c_str());
        lv_obj_set_style_text_color(viaLbl, lv_color_make(140, 140, 140), 0);
        lv_obj_set_style_text_font(viaLbl, &lv_font_montserrat_16, 0);
        lv_obj_align(viaLbl, LV_ALIGN_BOTTOM_LEFT, 10, -2);
    }

    msg_contacts_ack();
    block_touch_until_release();
    fade_in_children(msgOverlay, 150);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void msg_ui_init() {
    msgPage = MSG_PAGE_CONVO_LIST;
    msgPendingPage = -1;
    activeConvoIdx = 0;
    activeConvoName = "";
    composeTo = "";
    composeText = "";
    composeContactId = "";
    composePhone = "";
    composeThreadId = "";

    // Request conversation list from phone over BLE
    ble_msg_request_conversations();

    build_convo_list();
    LOG1("[msg_ui] init -> convo list\n");
}

void msg_ui_init_compose(const char *contact_name, const char *message_text) {
    // Enter compose directly from Gemini tool path
    msgPage = MSG_PAGE_COMPOSE;
    msgPendingPage = -1;

    composeTo    = contact_name ? contact_name : "";
    composeText  = message_text ? message_text : "";
    composePhone = "";
    composeThreadId = "";
    composeContactId = "";

    // Trigger contact search to resolve phone number
    composeNeedsContact = true;
    ble_msg_contact_search(composeTo);

    build_compose_overlay();
    LOG1("[msg_ui] init_compose -> to=%s (searching contacts)\n", composeTo.c_str());
}

void msg_ui_tick() {
    // Handle compose action flags
    if (msgPage == MSG_PAGE_COMPOSE) {
        if (composeSendFlag) {
            composeSendFlag = false;
            // Build JSON and send over BLE — include thread_id if we have one
            // (from the in-app thread view), otherwise phone from contact search
            String json = "{";
            if (composeThreadId.length() > 0)
                json += "\"thread_id\":\"" + composeThreadId + "\",";
            if (composePhone.length() > 0)
                json += "\"phone\":\"" + composePhone + "\",";
            json += "\"text\":\"" + composeText + "\"}";
            ble_msg_send(json);
            LOG1("[msg_ui] send -> to=%s\n", composeTo.c_str());
            // Return to thread if we came from one, otherwise idle
            destroy_overlay();
            if (activeConvoName.length() > 0) {
                msgPage = MSG_PAGE_THREAD;
                build_thread_view();
            } else {
                // Came from Gemini tool path — return to idle
                msg_ui_destroy();
                enter_state(STATE_IDLE);
            }
            return;
        }
        if (composeEditFlag) {
            composeEditFlag = false;
            // Start STT recording for edit
            ble_msg_stt_start();
            LOG1("[msg_ui] compose edit -> STT start\n");
            return;
        }
        if (composeDiscardFlag) {
            composeDiscardFlag = false;
            destroy_overlay();
            if (activeConvoName.length() > 0) {
                msgPage = MSG_PAGE_THREAD;
            } else {
                // Came from Gemini tool path — return to idle
                msg_ui_destroy();
                enter_state(STATE_IDLE);
                return;
            }
            LOG1("[msg_ui] compose discarded\n");
            return;
        }
        // Check if STT result arrived (via BLE callback)
        MsgComposeData &comp = msg_compose();
        if (comp.state == COMPOSE_SHOWING_TEXT && comp.text.length() > 0) {
            composeText = comp.text;
            comp.state = COMPOSE_IDLE;
            // Rebuild compose overlay with new text
            build_compose_overlay();
        }
    }

    // Handle contact search results (Gemini compose flow)
    if (composeNeedsContact && msg_contacts_changed()) {
        composeNeedsContact = false;
        msg_contacts_ack();
        uint16_t count = msg_contact_count();
        if (count == 1) {
            // Exact match — auto-select
            const MsgContact *c = msg_get_contact(0);
            if (c) {
                composeContactId = c->contact_id;
                composePhone     = c->phone;
                composeTo        = c->name;
            }
            LOG1("[msg_ui] contact resolved: %s\n", composeTo.c_str());
        } else if (count > 1) {
            // Multiple matches — show disambiguation
            disambigName = composeTo;
            msgPendingPage = MSG_PAGE_DISAMBIGUATE;
            LOG1("[msg_ui] %u contacts found, disambiguating\n", count);
        } else {
            LOG1("[msg_ui] no contacts found for '%s'\n", composeTo.c_str());
        }
    }

    // Handle back-to-home sub-step (201 pattern from settings_ui)
    if (states_sub_step() == 201 && millis() - states_enter_ms() >= 200) {
        states_set_sub_step(0);
        enter_state(STATE_UI_HOME);
        return;
    }

    // Handle pending page transitions
    if (msgPendingPage >= 0 && states_sub_step() == 0) {
        // Start fade-out of current content
        if (msgContainer) fade_out_children(msgContainer, 150);
        if (msgOverlay) fade_out_children(msgOverlay, 150);
        states_set_sub_step(100);
        states_set_enter_ms(millis());
    }

    if (states_sub_step() == 100 && millis() - states_enter_ms() >= 200) {
        int page = msgPendingPage;
        msgPendingPage = -1;
        states_set_sub_step(0);

        destroy_overlay();

        switch (page) {
            case MSG_PAGE_CONVO_LIST:
                destroy_container();
                msgPage = MSG_PAGE_CONVO_LIST;
                build_convo_list();
                break;
            case MSG_PAGE_THREAD:
                destroy_container();
                msgPage = MSG_PAGE_THREAD;
                build_thread_view();
                break;
            case MSG_PAGE_COMPOSE:
                msgPage = MSG_PAGE_COMPOSE;
                build_compose_overlay();
                break;
            case MSG_PAGE_DISAMBIGUATE:
                msgPage = MSG_PAGE_DISAMBIGUATE;
                build_disambiguate_overlay();
                break;
        }
    }

    // Check for data changes and rebuild if needed
    if (states_sub_step() == 0 && msgPendingPage < 0) {
        if (msgPage == MSG_PAGE_CONVO_LIST && msg_conversations_changed()) {
            build_convo_list();
        }
        if (msgPage == MSG_PAGE_THREAD && msg_messages_changed()) {
            build_thread_view();
        }
        if (msgPage == MSG_PAGE_DISAMBIGUATE && msg_contacts_changed()) {
            build_disambiguate_overlay();
        }
    }
}

void msg_ui_destroy() {
    destroy_overlay();
    destroy_container();
    msgPage = MSG_PAGE_CONVO_LIST;
    msgPendingPage = -1;
    activeConvoIdx = 0;
    activeConvoName = "";
    composeTo = "";
    composeText = "";
    composeContactId = "";
    composePhone = "";
    composeThreadId = "";
    composeNeedsContact = false;
    LOG1("[msg_ui] destroyed\n");
}

void msg_ui_btn_press() {
    switch (msgPage) {
        case MSG_PAGE_DISAMBIGUATE:
            // Cancel disambiguation -> discard compose
            destroy_overlay();
            if (activeConvoName.length() > 0) {
                msgPage = MSG_PAGE_THREAD;
            } else {
                // Came from Gemini tool path — return to idle
                msg_ui_destroy();
                enter_state(STATE_IDLE);
                return;
            }
            break;

        case MSG_PAGE_COMPOSE:
            // Discard compose -> return to thread or idle
            destroy_overlay();
            if (activeConvoName.length() > 0) {
                msgPage = MSG_PAGE_THREAD;
            } else {
                // Came from Gemini tool path — return to idle
                msg_ui_destroy();
                enter_state(STATE_IDLE);
                return;
            }
            break;

        case MSG_PAGE_THREAD:
            // Back to conversation list
            msgPendingPage = MSG_PAGE_CONVO_LIST;
            activeConvoIdx = 0;
            activeConvoName = "";
            break;

        case MSG_PAGE_CONVO_LIST:
            // Exit messages -> fade out, then transition to home
            if (msgContainer) fade_out_children(msgContainer, 150);
            states_set_enter_ms(millis());
            states_set_sub_step(201);
            break;
    }
}

void msg_ui_btn_long_press() {
    // Long press in thread view -> start voice compose
    if (msgPage == MSG_PAGE_THREAD) {
        const MsgConversation *c = msg_active_conversation();
        if (c) {
            composeTo       = c->name;
            composeThreadId = c->thread_id;
            composePhone    = "";  // thread-based send uses thread_id
        }
        ble_msg_stt_start();
        LOG1("[msg_ui] long press -> STT start\n");
    }
}

void msg_ui_btn_release() {
    // Release after long press -> stop STT recording
    MsgComposeData &comp = msg_compose();
    if (comp.state == COMPOSE_RECORDING) {
        ble_msg_stt_stop();
        LOG1("[msg_ui] release -> STT stop\n");
    }
}

void msg_ui_fade_out(uint32_t duration) {
    if (msgContainer) fade_out_children(msgContainer, duration);
    if (msgOverlay)   fade_out_children(msgOverlay, duration);
}
