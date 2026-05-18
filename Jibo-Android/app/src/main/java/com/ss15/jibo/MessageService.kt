package com.ss15.jibo

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.ContactsContract
import android.provider.Telephony
import android.telephony.SmsManager
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

/**
 * Reads SMS/MMS conversations from the system content provider and sends
 * messages via the appropriate app (default SMS, WhatsApp, Telegram).
 *
 * All queries are synchronous and should be called from a background
 * thread / coroutine.  Every public method is exception-safe: failures
 * are logged and a sensible empty/false value is returned.
 */
class MessageService(private val context: Context) {

    companion object {
        private const val TAG = "MessageService"
    }

    // ─── Data classes ───────────────────────────────────────────────────────

    data class Conversation(
        val threadId: String,
        val name: String,         // contact name or phone number
        val snippet: String,      // last message preview
        val timestamp: Long,      // ms since epoch
        val unread: Int,
        val app: String,          // "Messages"
        val appPkg: String,       // default SMS package name
        val isGroup: Boolean
    )

    data class Message(
        val sender: String,       // empty when isMine == true
        val text: String,
        val timestamp: Long,
        val isMine: Boolean
    )

    // ─── Public API ─────────────────────────────────────────────────────────

    /**
     * Get recent conversations from SMS/MMS content provider.
     * Returns up to [limit] conversations sorted by most recent first.
     */
    fun getConversations(limit: Int = 20): List<Conversation> {
        val conversations = mutableListOf<Conversation>()
        try {
            val uri = Uri.parse("content://mms-sms/conversations?simple=true")
            val projection = arrayOf("_id", "date", "snippet", "read", "recipient_ids")
            val defaultPkg = Telephony.Sms.getDefaultSmsPackage(context)
                ?: "com.google.android.apps.messaging"

            context.contentResolver.query(
                uri, projection, null, null, "date DESC"
            )?.use { cursor ->
                val idIdx        = cursor.getColumnIndex("_id")
                val dateIdx      = cursor.getColumnIndex("date")
                val snippetIdx   = cursor.getColumnIndex("snippet")
                val readIdx      = cursor.getColumnIndex("read")
                val recipientIdx = cursor.getColumnIndex("recipient_ids")

                while (cursor.moveToNext() && conversations.size < limit) {
                    val threadId = cursor.getLong(idIdx).toString()
                    val date     = cursor.getLong(dateIdx)
                    val snippet  = cursor.getString(snippetIdx) ?: ""
                    val read     = cursor.getInt(readIdx)

                    val address = getThreadAddress(threadId)
                    val name    = resolveContactName(address) ?: address
                    val unread  = if (read == 0) countUnread(threadId) else 0

                    val truncated = if (snippet.length > 100) {
                        snippet.take(100) + "..."
                    } else {
                        snippet
                    }

                    // Detect group threads from recipient_ids (space-separated)
                    val isGroup = if (recipientIdx >= 0) {
                        val ids = cursor.getString(recipientIdx) ?: ""
                        ids.trim().split("\\s+".toRegex()).size > 1
                    } else {
                        false
                    }

                    conversations.add(
                        Conversation(
                            threadId  = threadId,
                            name      = name,
                            snippet   = truncated,
                            timestamp = date,
                            unread    = unread,
                            app       = "Messages",
                            appPkg    = defaultPkg,
                            isGroup   = isGroup
                        )
                    )
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "getConversations failed", e)
        }
        return conversations
    }

    /**
     * Get messages for a specific SMS thread.
     * Returns up to [limit] messages sorted oldest first.
     */
    fun getThreadMessages(threadId: String, limit: Int = 30): List<Message> {
        val messages = mutableListOf<Message>()
        try {
            // Cache contact lookups -- a thread typically has one or two
            // addresses, so this avoids dozens of redundant PhoneLookup queries.
            val nameCache = mutableMapOf<String, String?>()

            context.contentResolver.query(
                Telephony.Sms.CONTENT_URI,
                null,
                "thread_id = ?",
                arrayOf(threadId),
                "date ASC"
            )?.use { cursor ->
                val bodyIdx    = cursor.getColumnIndex(Telephony.Sms.BODY)
                val dateIdx    = cursor.getColumnIndex(Telephony.Sms.DATE)
                val typeIdx    = cursor.getColumnIndex(Telephony.Sms.TYPE)
                val addressIdx = cursor.getColumnIndex(Telephony.Sms.ADDRESS)

                // Skip to the last N messages when the thread is longer
                val total = cursor.count
                if (total > limit) cursor.moveToPosition(total - limit)

                while (cursor.moveToNext()) {
                    val body    = cursor.getString(bodyIdx) ?: ""
                    val date    = cursor.getLong(dateIdx)
                    val type    = cursor.getInt(typeIdx)
                    val address = cursor.getString(addressIdx) ?: ""
                    val isMine  = (type == Telephony.Sms.MESSAGE_TYPE_SENT)
                    val sender  = if (isMine) {
                        ""
                    } else {
                        nameCache.getOrPut(address) { resolveContactName(address) }
                            ?: address
                    }

                    messages.add(Message(sender, body, date, isMine))
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "getThreadMessages failed for thread=$threadId", e)
        }
        return messages
    }

    /**
     * Send a message.  Routes to the appropriate app based on [appPkg].
     * Returns true on success.
     */
    fun sendMessage(
        threadId: String? = null,
        phoneNumber: String? = null,
        appPkg: String? = null,
        text: String
    ): Boolean {
        try {
            val number = phoneNumber
                ?: getThreadAddress(threadId ?: return false)
            if (number.isBlank()) return false

            val pkg = appPkg ?: Telephony.Sms.getDefaultSmsPackage(context)

            return when {
                pkg?.contains("whatsapp") == true  -> sendViaIntent("com.whatsapp", number, text)
                pkg?.contains("telegram") == true   -> sendViaIntent("org.telegram.messenger", number, text)
                else                                -> sendViaSms(number, text)
            }
        } catch (e: Exception) {
            Log.e(TAG, "sendMessage failed", e)
            return false
        }
    }

    // ─── JSON serialization for BLE transport ───────────────────────────────

    fun conversationsToJson(convos: List<Conversation>): String {
        val arr = JSONArray()
        for (c in convos) {
            arr.put(JSONObject().apply {
                put("id", c.threadId)
                put("name", c.name)
                put("snippet", c.snippet)
                put("ts", c.timestamp)
                put("unread", c.unread)
                put("app", c.app)
                put("app_pkg", c.appPkg)
                put("is_group", c.isGroup)
            })
        }
        return arr.toString()
    }

    fun messagesToJson(messages: List<Message>): String {
        val arr = JSONArray()
        for (m in messages) {
            arr.put(JSONObject().apply {
                put("sender", m.sender)
                put("text", m.text)
                put("ts", m.timestamp)
                put("mine", m.isMine)
            })
        }
        return arr.toString()
    }

    // ─── Send helpers ───────────────────────────────────────────────────────

    @Suppress("DEPRECATION")
    private fun sendViaSms(number: String, text: String): Boolean {
        val smsManager = SmsManager.getDefault()
        val parts = smsManager.divideMessage(text)
        smsManager.sendMultipartTextMessage(number, null, parts, null, null)
        Log.d(TAG, "SMS sent to $number (${parts.size} part(s))")
        return true
    }

    /**
     * Launch an ACTION_SEND intent targeting [pkg].  WhatsApp needs
     * a "jid" extra to route to the right chat; other apps ignore it.
     */
    private fun sendViaIntent(pkg: String, number: String, text: String): Boolean {
        val intent = Intent(Intent.ACTION_SEND).apply {
            setPackage(pkg)
            type = "text/plain"
            putExtra(Intent.EXTRA_TEXT, text)
            if (pkg == "com.whatsapp") putExtra("jid", "$number@s.whatsapp.net")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        return try {
            context.startActivity(intent)
            Log.d(TAG, "$pkg intent launched for $number")
            true
        } catch (e: Exception) {
            Log.e(TAG, "$pkg send failed", e)
            false
        }
    }

    // ─── Private helpers ────────────────────────────────────────────────────

    /**
     * Get the phone-number (address) associated with an SMS thread.
     */
    private fun getThreadAddress(threadId: String): String {
        try {
            val uri = Uri.parse("content://sms")
            context.contentResolver.query(
                uri,
                arrayOf(Telephony.Sms.ADDRESS),
                "thread_id = ?",
                arrayOf(threadId),
                "date DESC LIMIT 1"
            )?.use { cursor ->
                if (cursor.moveToFirst()) {
                    return cursor.getString(0) ?: ""
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "getThreadAddress failed for thread=$threadId", e)
        }
        return ""
    }

    /**
     * Resolve a phone number to a contact display name via the system
     * contacts provider.  Returns null if no matching contact is found.
     */
    private fun resolveContactName(phoneNumber: String): String? {
        if (phoneNumber.isBlank()) return null
        try {
            val uri = Uri.withAppendedPath(
                ContactsContract.PhoneLookup.CONTENT_FILTER_URI,
                Uri.encode(phoneNumber)
            )
            context.contentResolver.query(
                uri,
                arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME),
                null, null, null
            )?.use { cursor ->
                if (cursor.moveToFirst()) {
                    return cursor.getString(0)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "resolveContactName failed for $phoneNumber", e)
        }
        return null
    }

    /**
     * Count unread messages in the given thread.
     */
    private fun countUnread(threadId: String): Int {
        try {
            context.contentResolver.query(
                Telephony.Sms.CONTENT_URI,
                arrayOf("count(*) AS count"),
                "thread_id = ? AND read = 0",
                arrayOf(threadId),
                null
            )?.use { cursor ->
                if (cursor.moveToFirst()) return cursor.getInt(0)
            }
        } catch (e: Exception) {
            Log.e(TAG, "countUnread failed for thread=$threadId", e)
        }
        return 0
    }

}
