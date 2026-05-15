package com.ss15.jibo

import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.util.concurrent.TimeUnit

object DeepgramClient {
    private const val TAG = "DeepgramClient"

    private val VOICES = arrayOf(
        "aura-2-odysseus-en", "aura-2-apollo-en", "aura-2-arcas-en",
        "aura-2-aries-en", "aura-2-atlas-en", "aura-2-hermes-en"
    )

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .writeTimeout(15, TimeUnit.SECONDS)
        .build()

    /**
     * Stream Deepgram TTS as raw linear16 PCM, calling [onChunk] with each
     * received byte buffer. The callback runs on the caller's thread.
     */
    fun streamTts(ttsKey: String, text: String, voiceIdx: Int, onChunk: (ByteArray) -> Unit) {
        val vi = if (voiceIdx >= VOICES.size) 0 else voiceIdx
        val url = "https://api.deepgram.com/v1/speak" +
                "?model=${VOICES[vi]}" +
                "&encoding=linear16&sample_rate=16000&container=none"

        val jsonBody = JSONObject().put("text", text).toString()
        val body = jsonBody.toRequestBody("application/json".toMediaType())

        val req = Request.Builder()
            .url(url)
            .post(body)
            .addHeader("Authorization", "Token $ttsKey")
            .build()

        try {
            client.newCall(req).execute().use { resp ->
                if (!resp.isSuccessful) {
                    Log.e(TAG, "HTTP ${resp.code}: ${resp.body?.string()?.take(300)}")
                    return
                }

                val stream = resp.body?.byteStream() ?: return
                val buf = ByteArray(4096)
                var total = 0L

                while (true) {
                    val n = stream.read(buf)
                    if (n <= 0) break
                    onChunk(buf.copyOf(n))
                    total += n
                }

                Log.d(TAG, "TTS stream complete: ${total}B")
            }
        } catch (e: Exception) {
            Log.e(TAG, "TTS stream error", e)
        }
    }
}
