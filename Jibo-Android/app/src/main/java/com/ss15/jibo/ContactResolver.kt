package com.ss15.jibo

import android.content.Context
import android.provider.ContactsContract
import android.provider.Telephony
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

/**
 * Fuzzy contact search with variant-spelling support for the Gemini
 * "send.message" tool flow.  Resolves a spoken name (e.g. "Kaden",
 * "Caden", "Kayden") to the best-matching device contact and picks
 * the most appropriate messaging app based on notification history.
 */
class ContactResolver(private val context: Context) {

    companion object {
        private const val TAG = "ContactResolver"
        private const val SCORE_THRESHOLD = 0.5f
        private const val SOUNDEX_BOOST = 0.2f

        /** Soundex consonant-code mapping (allocated once, reused across calls). */
        private val SOUNDEX_MAP = mapOf(
            'b' to '1', 'f' to '1', 'p' to '1', 'v' to '1',
            'c' to '2', 'g' to '2', 'j' to '2', 'k' to '2',
            'q' to '2', 's' to '2', 'x' to '2', 'z' to '2',
            'd' to '3', 't' to '3',
            'l' to '4',
            'm' to '5', 'n' to '5',
            'r' to '6'
        )
    }

    data class ContactMatch(
        val contactId: String,
        val displayName: String,
        val phoneNumber: String,
        val bestApp: String,        // package name of most recent messaging app
        val bestAppLabel: String,   // "Messages", "WhatsApp", etc.
        val score: Float            // 0.0 - 1.0 match confidence
    )

    /**
     * Fuzzy search contacts by name.  Handles variant spellings
     * (e.g., Kaden/Caden/Kayden) via Levenshtein distance + phonetic
     * matching (Soundex).
     *
     * @param query The name to search for (from Gemini tool call)
     * @param notifBuffer Notification history for determining best messaging app per contact
     * @param limit Max results to return
     * @return Sorted list of matching contacts (best match first)
     */
    fun fuzzySearch(
        query: String,
        notifBuffer: List<BleService.NotifEntry>? = null,
        limit: Int = 10
    ): List<ContactMatch> {
        if (query.isBlank()) return emptyList()

        val normalizedQuery = query.trim().lowercase()
        val querySoundex = soundex(normalizedQuery)
        val contacts = loadContacts()

        val matches = mutableListOf<ContactMatch>()
        for ((contactId, name, phone) in contacts) {
            val score = scoreMatch(normalizedQuery, querySoundex, name.lowercase())
            if (score > SCORE_THRESHOLD) {
                val (appPkg, appLabel) = bestMessagingApp(name, notifBuffer)
                matches.add(ContactMatch(contactId, name, phone, appPkg, appLabel, score))
            }
        }

        matches.sortByDescending { it.score }
        return matches.take(limit)
    }

    /** Serialize a list of matches to JSON for BLE transport. */
    fun toJson(matches: List<ContactMatch>): String {
        val arr = JSONArray()
        for (m in matches) {
            val obj = JSONObject()
            obj.put("id", m.contactId)
            obj.put("name", m.displayName)
            obj.put("phone", m.phoneNumber)
            obj.put("app", m.bestAppLabel)
            obj.put("app_pkg", m.bestApp)
            obj.put("score", m.score.toDouble())
            arr.put(obj)
        }
        return arr.toString()
    }

    // ─── Contact loading ────────────────────────────────────────────────────

    private data class RawContact(
        val contactId: String,
        val displayName: String,
        val phoneNumber: String
    )

    /**
     * Load all contacts that have at least one phone number.
     * Deduplicates by contact ID, keeping the first (normalized) number found.
     */
    private fun loadContacts(): List<RawContact> {
        val seen = LinkedHashMap<String, RawContact>()
        val projection = arrayOf(
            ContactsContract.CommonDataKinds.Phone.CONTACT_ID,
            ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME,
            ContactsContract.CommonDataKinds.Phone.NUMBER,
            ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER
        )
        var cursor: android.database.Cursor? = null
        try {
            cursor = context.contentResolver.query(
                ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
                projection,
                null, null, null
            )
            if (cursor == null) return emptyList()
            val idIdx   = cursor.getColumnIndex(ContactsContract.CommonDataKinds.Phone.CONTACT_ID)
            val nameIdx = cursor.getColumnIndex(ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME)
            val numIdx  = cursor.getColumnIndex(ContactsContract.CommonDataKinds.Phone.NUMBER)
            val normIdx = cursor.getColumnIndex(ContactsContract.CommonDataKinds.Phone.NORMALIZED_NUMBER)

            while (cursor.moveToNext()) {
                val id   = cursor.getString(idIdx) ?: continue
                val name = cursor.getString(nameIdx) ?: continue
                if (seen.containsKey(id)) continue
                val norm = cursor.getString(normIdx)
                val num  = norm ?: cursor.getString(numIdx) ?: continue
                seen[id] = RawContact(id, name, num)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error querying contacts", e)
        } finally {
            cursor?.close()
        }
        return seen.values.toList()
    }

    // ─── Fuzzy matching ─────────────────────────────────────────────────────

    /**
     * Score a query against a contact name.  Considers:
     *   1. Full-name Levenshtein distance
     *   2. First-name-only Levenshtein distance (for multi-word names)
     *   3. Soundex phonetic boost when codes match
     */
    private fun scoreMatch(query: String, querySoundex: String, name: String): Float {
        // Full-name distance
        var best = levenshteinScore(query, name)

        // First-name match: if the contact has multiple words, compare
        // against just the first token (handles "John Smith" vs "John").
        val firstName = name.split(" ").firstOrNull() ?: name
        if (firstName != name) {
            val firstScore = levenshteinScore(query, firstName)
            if (firstScore > best) best = firstScore
        }

        // Soundex boost: if the phonetic codes match, bump the score.
        val nameSoundex = soundex(firstName)
        if (querySoundex.isNotEmpty() && nameSoundex.isNotEmpty() && querySoundex == nameSoundex) {
            best = (best + SOUNDEX_BOOST).coerceAtMost(1.0f)
        }

        return best
    }

    /** Normalized Levenshtein score: 1.0 = exact match, 0.0 = completely different. */
    private fun levenshteinScore(a: String, b: String): Float {
        val maxLen = maxOf(a.length, b.length)
        if (maxLen == 0) return 1.0f
        val dist = levenshteinDistance(a, b)
        return 1.0f - dist.toFloat() / maxLen.toFloat()
    }

    /** Standard dynamic-programming Levenshtein distance. */
    private fun levenshteinDistance(a: String, b: String): Int {
        val dp = Array(a.length + 1) { IntArray(b.length + 1) }
        for (i in 0..a.length) dp[i][0] = i
        for (j in 0..b.length) dp[0][j] = j
        for (i in 1..a.length) {
            for (j in 1..b.length) {
                dp[i][j] = minOf(
                    dp[i - 1][j] + 1,
                    dp[i][j - 1] + 1,
                    dp[i - 1][j - 1] + if (a[i - 1] == b[j - 1]) 0 else 1
                )
            }
        }
        return dp[a.length][b.length]
    }

    /**
     * American Soundex: first letter + up to 3 consonant digit codes.
     * Catches phonetic variants like Kaden/Caden/Kayden (all K350).
     */
    private fun soundex(s: String): String {
        if (s.isEmpty()) return ""
        val clean = s.lowercase().filter { it.isLetter() }
        if (clean.isEmpty()) return ""
        val result = StringBuilder().append(clean[0].uppercaseChar())
        var lastCode = SOUNDEX_MAP[clean[0]]
        for (i in 1 until clean.length) {
            val code = SOUNDEX_MAP[clean[i]]
            if (code != null && code != lastCode) {
                result.append(code)
                if (result.length == 4) break
            }
            lastCode = code ?: lastCode
        }
        while (result.length < 4) result.append('0')
        return result.toString()
    }

    // ─── Best messaging app ─────────────────────────────────────────────────

    /**
     * Determine the best messaging app for a contact by checking which
     * messaging app most recently delivered a notification whose sender
     * matches the contact name.  Falls back to the system default SMS app.
     */
    private fun bestMessagingApp(
        contactName: String,
        notifBuffer: List<BleService.NotifEntry>?
    ): Pair<String, String> {
        if (notifBuffer != null) {
            val nameLower = contactName.lowercase()
            // The buffer is in insertion order (oldest first), so walk
            // backwards to find the most recent messaging notification
            // whose sender matches the contact name.
            val match = notifBuffer
                .lastOrNull { it.category == "messaging" &&
                              (it.sender.lowercase().contains(nameLower) ||
                               nameLower.contains(it.sender.lowercase())) }
            if (match != null) {
                // NotifEntry.key follows the StatusBarNotification format
                // "userId|packageName|notifId|tag|uid" -- extract the
                // package name from the second segment.
                val pkg = match.key.split("|").getOrNull(1) ?: match.app
                return Pair(pkg, match.app)
            }
        }

        // Fall back to the system default SMS app.
        val defaultPkg = try {
            Telephony.Sms.getDefaultSmsPackage(context)
        } catch (_: Exception) { null }
            ?: "com.google.android.apps.messaging"

        val label = try {
            val pm = context.packageManager
            pm.getApplicationLabel(pm.getApplicationInfo(defaultPkg, 0)).toString()
        } catch (_: Exception) { "Messages" }

        return Pair(defaultPkg, label)
    }
}
