package com.ss15.jibo

import android.util.Base64
import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.util.concurrent.TimeUnit

object GeminiClient {
    private const val TAG = "GeminiClient"
    private val MODELS = arrayOf("gemini-flash-lite-latest", "gemini-flash-latest")

    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .writeTimeout(30, TimeUnit.SECONDS)
        .build()

    fun sendAudio(
        apiKey: String,
        modelIdx: Int,
        pcmData: ByteArray,
        history: String,
        notifications: List<BleService.NotifEntry> = emptyList(),
        phoneLinked: Boolean = true,
        memories: List<String> = emptyList()
    ): String? {
        val mi = if (modelIdx > 1) 0 else modelIdx
        val model = MODELS[mi]

        val wavData = buildWav(pcmData, 8000)
        val b64 = Base64.encodeToString(wavData, Base64.NO_WRAP)

        val userPart = JSONObject().apply {
            put("inline_data", JSONObject().apply {
                put("mime_type", "audio/wav")
                put("data", b64)
            })
        }

        return doRequest(apiKey, model, userPart, history, notifications, phoneLinked, memories)
    }

    fun sendText(
        apiKey: String,
        modelIdx: Int,
        text: String,
        history: String,
        notifications: List<BleService.NotifEntry> = emptyList(),
        phoneLinked: Boolean = true,
        memories: List<String> = emptyList()
    ): String? {
        val mi = if (modelIdx > 1) 0 else modelIdx
        val model = MODELS[mi]
        val userPart = JSONObject().put("text", text)
        return doRequest(apiKey, model, userPart, history, notifications, phoneLinked, memories)
    }

    private fun doRequest(
        apiKey: String,
        model: String,
        userPart: JSONObject,
        history: String,
        notifications: List<BleService.NotifEntry>,
        phoneLinked: Boolean,
        memories: List<String>
    ): String? {
        val url = "https://generativelanguage.googleapis.com/v1beta/models/$model:generateContent?key=$apiKey"

        val contents = buildContents(userPart, history)
        val sysPrompt = buildSystemPrompt(notifications, phoneLinked, memories)

        val payload = JSONObject().apply {
            put("system_instruction", JSONObject().put("parts", JSONArray().put(
                JSONObject().put("text", sysPrompt)
            )))
            put("contents", contents)
            put("generationConfig", JSONObject().put("thinkingConfig",
                JSONObject().put("thinkingLevel", "MINIMAL")))
            put("tools", JSONArray()
                .put(JSONObject().put("googleSearch", JSONObject()))
                .put(JSONObject().put("urlContext", JSONObject())))
        }

        val body = payload.toString().toRequestBody("application/json".toMediaType())
        val req = Request.Builder().url(url).post(body).build()

        try {
            client.newCall(req).execute().use { resp ->
                if (!resp.isSuccessful) {
                    Log.e(TAG, "HTTP ${resp.code}: ${resp.body?.string()?.take(500)}")
                    return null
                }
                val respBody = resp.body?.string() ?: return null
                return extractText(respBody)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Request failed", e)
            return null
        }
    }

    /**
     * Mirror of `build_system_prompt()` in firmware/Main/gemini.cpp so BLE-
     * proxied requests behave identically to direct WiFi requests.
     */
    fun buildSystemPrompt(
        notifications: List<BleService.NotifEntry>,
        phoneLinked: Boolean,
        memories: List<String> = emptyList()
    ): String = buildString {
        append("You are Jibo, a friendly social robot companion. ")
        append("Keep responses brief and conversational, one or two sentences.\n\n")

        append("REAL-TIME INFO:\n")
        append("- You have Google Search and URL-context tools wired in by the API. When the user asks anything time-sensitive -- today's weather, news, current events, prices, scores, who-is-someone, recent releases, etc. -- USE Google Search and answer from the results. Never tell the user you don't have access to real-time information; you do.\n")
        append("- When you're not 100% sure of a current fact, prefer searching over guessing.\n\n")

        append("NOTIFICATION RULES:\n")
        append("- DO NOT proactively offer to check notifications during normal conversation. Never ask things like \"do you have any messages you want me to check?\" or \"want me to read your latest notifications?\" -- the user will ask if they want them. The notification list below is purely background context.\n")
        append("- If the user asks about notifications (\"do I have any messages from X?\", \"any new emails?\"), use the list below as the source of truth. Never invent notifications.\n")
        append("- The bracketed tag (e.g. [messaging], [banking], [email]) is just a descriptive category, NOT a sensitivity flag. You judge sensitivity yourself from the actual body text.\n")
        append("- Treat a notification as SENSITIVE when its content is plausibly private or embarrassing if overheard: 2FA / verification codes, specific banking amounts or account numbers, medical information, intimate or romantic messages, anything explicitly about something the user would not want a roommate or guest to hear. For sensitive ones, do NOT read the body unprompted -- say something like: \"Yes, you have a message from <sender>, but you might want to read it in private. Want me to read it anyway?\" and wait for confirmation.\n")
        append("- Most casual messaging is NOT sensitive. A friend or family member asking about plans, sending a link, or making small talk is fine to read aloud directly. When in doubt and the content is clearly mundane, just read it.\n")
        append("- Treat a notification as TIME-SENSITIVE only when the content itself is urgent: alarms going off now, calendar reminders for events starting soon, missed calls, delivery / ride ETAs in the next few minutes, severe-weather alerts. Promotional pushes, social-media engagement, news headlines, app updates, and most media notifications are NOT urgent.\n")
        append("- For genuinely time-sensitive items you MAY proactively slip them in after answering the user's actual question: \"By the way, you have a time-sensitive notification from <sender>...\". If it's also sensitive, only mention that it exists and suggest the user check their phone.\n")
        append("- Don't list every notification unless asked. Pick the relevant one(s) for the user's question.\n")
        append("- If the user just said \"yes\", \"please\", or \"go ahead\" right after you offered to read a sensitive notification, read the body of the notification you offered.\n\n")

        append("TOOLS:\n")
        append("- You have these tools available:\n")
        append("    [show.text]{ ...LaTeX... }     -- on-screen math / equations\n")
        append("    [show.stock]{ TICKER | RANGE } -- on-screen live stock card (RANGE: 1d,1w,1m,6m,ytd,1y)\n")
        append("    [store.memory]{ short fact }   -- save a durable fact about the user\n")
        append("- Place tool calls at the VERY START of your response, before any spoken text. They are not heard by the user.\n")
        append("- Multiple tools may be chained: e.g. [store.memory]{user's dog is named Rex} [show.text]{...} ...\n")
        append("- Use a display tool (show.*) ONLY when the answer is best shown rather than spoken. Don't use one for plain prose.\n")
        append("- show.text: renderer supports plain letters and digits, \\frac{a}{b}, \\sqrt{x}, x^{2}, x_{i}, \\pm, \\cdot, \\times, \\div, \\leq, \\geq, \\neq, parentheses. Greek letters and integrals are NOT supported -- avoid them. Keep expressions compact (small round screen).\n")
        append("- show.stock: pass a ticker symbol like AAPL, MSFT, NVDA. Optionally add a pipe and timeframe: [show.stock]{NVDA|ytd}. Available ranges: 1d (today, default), 1w, 1m, 6m, ytd, 1y. Match the range to the user's request -- 'how has Apple done this year' -> ytd, 'Tesla this month' -> 1m, 'NVDA today' -> 1d. If no timeframe is mentioned, default to 1d. The card has buttons to switch ranges so don't overthink it. Don't try to read the actual price -- the card shows it.\n")
        append("- store.memory: use ONLY for durable facts the user clearly wants remembered across sessions -- name, location, family / pets, long-term preferences (\"I'm vegetarian\", \"I work nights\"), or anything they explicitly ask you to remember. DO NOT use it for transient context like \"I'm going to the store later\". Keep the stored fact short, declarative, third-person about the user (\"User lives in Windsor, California\"). After storing, briefly confirm in the spoken response (\"Got it, I'll remember that\").\n")
        append("- After any tool call, still speak a short natural sentence so the response isn't silent.\n")
        append("- Examples:\n")
        append("    user: \"what's the quadratic formula?\" -> [show.text]{x = \\frac{-b \\pm \\sqrt{b^{2} - 4ac}}{2a}} Here's the quadratic formula.\n")
        append("    user: \"how's nvidia doing today?\" -> [show.stock]{NVDA} Here's the latest on NVIDIA.\n")
        append("    user: \"show me apple stock this year\" -> [show.stock]{AAPL|ytd} Here's Apple's performance so far this year.\n")
        append("    user: \"remember that I live in Windsor, California\" -> [store.memory]{User lives in Windsor, California} Got it, I'll remember that.\n\n")

        if (phoneLinked) {
            append("PHONE LINK: Phone is connected, notifications are live.\n\n")
        } else {
            append("PHONE LINK: Phone is NOT connected. If the user asks about notifications, tell them their phone isn't connected so you can't read notifications right now.\n\n")
        }

        // Phone-side current time so Gemini can answer "what time is it"
        // while we're routing through the BLE proxy.  Uses the phone's
        // own clock + locale, which is the most reliable source we
        // have when the firmware's SNTP is unavailable.
        run {
            val now = java.util.Date()
            val fmt = java.text.SimpleDateFormat(
                "EEEE, MMMM d yyyy, h:mm a zzz", java.util.Locale.getDefault())
            append("CURRENT TIME: ").append(fmt.format(now)).append("\n\n")
        }

        append("STORED MEMORIES:\n")
        if (memories.isEmpty()) {
            append("(none yet)\n\n")
        } else {
            for (m in memories) append("- ").append(m).append('\n')
            append('\n')
        }

        append("CURRENT PHONE NOTIFICATIONS:\n")
        append(formatNotifications(notifications))
    }

    private fun formatNotifications(list: List<BleService.NotifEntry>): String {
        if (list.isEmpty()) return "There are no current phone notifications."
        val now = System.currentTimeMillis()
        val sb = StringBuilder()
        sb.append("You have ").append(list.size)
            .append(if (list.size == 1) " notification" else " notifications")
            .append(" on the user's phone right now:\n")
        // Category is descriptive only -- sensitivity / urgency are judged by
        // Gemini from the actual body text via the system-prompt rules.
        for (n in list) {
            sb.append("- [")
                .append(n.category.ifEmpty { "other" })
                .append("] ").append(n.app.ifEmpty { "App" })
            if (n.sender.isNotEmpty()) sb.append(" from ").append(n.sender)
            sb.append(" (").append(relativeAge(now - n.postTimeMs)).append("): \"")
            val t = if (n.text.length > 240) n.text.substring(0, 240) + "..." else n.text
            sb.append(t).append("\"\n")
        }
        return sb.toString()
    }

    private fun relativeAge(dtMs: Long): String {
        val dt = if (dtMs < 0) 0 else dtMs
        if (dt < 60_000)             return "just now"
        val mins = dt / 60_000
        if (mins < 60)               return "$mins min ago"
        val hrs = mins / 60
        if (hrs < 24)                return "$hrs hr ago"
        val days = hrs / 24
        return "$days day${if (days == 1L) "" else "s"} ago"
    }

    private fun buildContents(userPart: JSONObject, history: String): JSONArray {
        val contents = JSONArray()

        if (history.isNotEmpty()) {
            try {
                // NVS stores turns without outer brackets, wrap them
                val wrapped = if (history.startsWith("[")) history else "[$history]"
                val hist = JSONArray(wrapped)
                for (i in 0 until hist.length()) {
                    contents.put(hist.get(i))
                }
            } catch (e: Exception) {
                Log.e(TAG, "History parse error", e)
            }
        }

        contents.put(JSONObject().apply {
            put("role", "user")
            put("parts", JSONArray().put(userPart))
        })

        return contents
    }

    private fun extractText(json: String): String? {
        try {
            val obj = JSONObject(json)
            val candidates = obj.optJSONArray("candidates") ?: return null
            val first = candidates.optJSONObject(0) ?: return null
            val content = first.optJSONObject("content") ?: return null
            val parts = content.optJSONArray("parts") ?: return null

            for (i in 0 until parts.length()) {
                val part = parts.optJSONObject(i) ?: continue
                if (part.has("text") && !part.has("thought")) {
                    return part.getString("text")
                }
            }
            // Fallback: return last text part even if it's a thought
            for (i in parts.length() - 1 downTo 0) {
                val part = parts.optJSONObject(i) ?: continue
                if (part.has("text")) return part.getString("text")
            }
        } catch (e: Exception) {
            Log.e(TAG, "JSON parse error", e)
        }
        return null
    }

    private fun buildWav(pcm16k: ByteArray, sampleRate: Int): ByteArray {
        // Downsample 16kHz -> 8kHz by averaging pairs
        val origSamples = pcm16k.size / 2
        val dsSamples = origSamples / 2
        val dsPcm = ByteArray(dsSamples * 2)

        for (i in 0 until dsSamples) {
            val s1 = (pcm16k[i * 4].toInt() and 0xFF) or (pcm16k[i * 4 + 1].toInt() shl 8)
            val s2 = (pcm16k[i * 4 + 2].toInt() and 0xFF) or (pcm16k[i * 4 + 3].toInt() shl 8)
            val avg = ((s1.toShort().toInt() + s2.toShort().toInt()) / 2).toShort()
            dsPcm[i * 2] = (avg.toInt() and 0xFF).toByte()
            dsPcm[i * 2 + 1] = (avg.toInt() shr 8).toByte()
        }

        val wavLen = 44 + dsPcm.size
        val bos = ByteArrayOutputStream(wavLen)
        val dos = DataOutputStream(bos)

        dos.writeBytes("RIFF")
        dos.writeInt(Integer.reverseBytes(wavLen - 8))
        dos.writeBytes("WAVE")
        dos.writeBytes("fmt ")
        dos.writeInt(Integer.reverseBytes(16))
        dos.writeShort(java.lang.Short.reverseBytes(1).toInt())       // PCM
        dos.writeShort(java.lang.Short.reverseBytes(1).toInt())       // mono
        dos.writeInt(Integer.reverseBytes(sampleRate))
        dos.writeInt(Integer.reverseBytes(sampleRate * 2))            // byte rate
        dos.writeShort(java.lang.Short.reverseBytes(2).toInt())       // block align
        dos.writeShort(java.lang.Short.reverseBytes(16).toInt())      // bits per sample
        dos.writeBytes("data")
        dos.writeInt(Integer.reverseBytes(dsPcm.size))
        dos.write(dsPcm)

        return bos.toByteArray()
    }
}
