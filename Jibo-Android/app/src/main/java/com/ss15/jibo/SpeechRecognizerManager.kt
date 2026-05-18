package com.ss15.jibo

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log

/**
 * Manages Android's SpeechRecognizer for voice-to-text.
 *
 * Usage flow (triggered by BLE from ESP32):
 * 1. ESP sends MSG_STT_START -> call start()
 * 2. Phone records audio, processes speech locally
 * 3. ESP sends MSG_STT_STOP -> call stop() (or let silence detection handle it)
 * 4. On result: callback fires -> send MSG_STT_RESULT back to ESP
 * 5. On error: callback fires -> send MSG_STT_ERROR back to ESP
 *
 * Audio never leaves the device — Android's on-device speech model is used.
 */
class SpeechRecognizerManager(
    private val context: Context,
    private val onResult: (String) -> Unit,
    private val onPartialResult: ((String) -> Unit)? = null,
    private val onError: (Int, String) -> Unit
) {
    companion object {
        private const val TAG = "SpeechRecMgr"
    }

    private var recognizer: SpeechRecognizer? = null
    private var isListening = false

    /**
     * Check if on-device speech recognition is available.
     */
    fun isAvailable(): Boolean {
        return SpeechRecognizer.isRecognitionAvailable(context)
    }

    /**
     * Start listening for speech. Creates a new SpeechRecognizer instance
     * each time (recommended by Android docs for reliability).
     */
    fun start() {
        if (isListening) {
            Log.w(TAG, "Already listening, stopping first")
            stop()
        }

        if (!isAvailable()) {
            onError(SpeechRecognizer.ERROR_CLIENT, "Speech recognition not available")
            return
        }

        try {
            recognizer = SpeechRecognizer.createSpeechRecognizer(context)
            recognizer?.setRecognitionListener(listener)

            val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(
                    RecognizerIntent.EXTRA_LANGUAGE_MODEL,
                    RecognizerIntent.LANGUAGE_MODEL_FREE_FORM
                )
                putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
                putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
                // Prefer offline recognition if available
                putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true)
            }

            recognizer?.startListening(intent)
            isListening = true
            Log.d(TAG, "Started listening")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start recognizer", e)
            onError(SpeechRecognizer.ERROR_CLIENT, e.message ?: "Unknown error")
            cleanup()
        }
    }

    /**
     * Stop listening and finalize recognition.
     * The result callback will fire with whatever was recognized so far.
     */
    fun stop() {
        if (!isListening) return
        try {
            recognizer?.stopListening()
            Log.d(TAG, "Stopped listening")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping recognizer", e)
        }
    }

    /**
     * Cancel recognition without triggering result callback.
     */
    fun cancel() {
        cleanup()
        Log.d(TAG, "Cancelled")
    }

    /**
     * Must be called when the service is destroyed.
     */
    fun destroy() {
        cleanup()
    }

    private fun cleanup() {
        isListening = false
        try {
            recognizer?.cancel()
            recognizer?.destroy()
        } catch (e: Exception) {
            Log.e(TAG, "Error during cleanup", e)
        }
        recognizer = null
    }

    private val listener = object : RecognitionListener {
        override fun onReadyForSpeech(params: Bundle?) {
            Log.d(TAG, "Ready for speech")
        }

        override fun onBeginningOfSpeech() {
            Log.d(TAG, "Speech started")
        }

        override fun onRmsChanged(rmsdB: Float) {
            // Could forward audio level to ESP for visual feedback
        }

        override fun onBufferReceived(buffer: ByteArray?) {}

        override fun onEndOfSpeech() {
            Log.d(TAG, "Speech ended")
            isListening = false
        }

        override fun onError(error: Int) {
            isListening = false
            val message = errorToString(error)
            Log.e(TAG, "Recognition error: $message ($error)")
            this@SpeechRecognizerManager.onError(error, message)
            cleanup()
        }

        override fun onResults(results: Bundle?) {
            isListening = false
            val matches = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            val text = matches?.firstOrNull() ?: ""
            Log.d(TAG, "Final result: $text")
            if (text.isNotBlank()) {
                this@SpeechRecognizerManager.onResult(text)
            } else {
                this@SpeechRecognizerManager.onError(
                    SpeechRecognizer.ERROR_NO_MATCH, "No speech detected"
                )
            }
            cleanup()
        }

        override fun onPartialResults(partialResults: Bundle?) {
            val matches = partialResults?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            val text = matches?.firstOrNull() ?: ""
            if (text.isNotBlank()) {
                onPartialResult?.invoke(text)
            }
        }

        override fun onEvent(eventType: Int, params: Bundle?) {}
    }

    private fun errorToString(error: Int): String = when (error) {
        SpeechRecognizer.ERROR_AUDIO -> "Audio recording error"
        SpeechRecognizer.ERROR_CLIENT -> "Client error"
        SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "Insufficient permissions"
        SpeechRecognizer.ERROR_NETWORK -> "Network error"
        SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "Network timeout"
        SpeechRecognizer.ERROR_NO_MATCH -> "No speech match"
        SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> "Recognizer busy"
        SpeechRecognizer.ERROR_SERVER -> "Server error"
        SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "No speech detected"
        else -> "Unknown error ($error)"
    }
}
