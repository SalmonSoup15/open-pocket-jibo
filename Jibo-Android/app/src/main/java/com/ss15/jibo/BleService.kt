package com.ss15.jibo

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.ParcelUuid
import android.util.Log
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.*
import org.json.JSONArray
import org.json.JSONObject

@SuppressLint("MissingPermission")
class BleService : LifecycleService() {

    companion object {
        private const val TAG = "BleService"
        private const val CHANNEL_ID = "jibo_ble"
        private const val NOTIF_ID = 1
        private const val PREFS_NAME = "jibo_ble_prefs"
        private const val PREF_PAIRED_ADDR = "paired_addr"
        private const val PREF_PAIRED_NAME = "paired_name"
        private const val PREF_DEV_MODE    = "dev_mode_enabled"

        val SERVICE_UUID: UUID  = UUID.fromString("a0e50000-0001-4d3b-8f1c-2e4a6b8c0d1e")
        val TX_CHAR_UUID: UUID  = UUID.fromString("a0e50001-0001-4d3b-8f1c-2e4a6b8c0d1e")
        val RX_CHAR_UUID: UUID  = UUID.fromString("a0e50002-0001-4d3b-8f1c-2e4a6b8c0d1e")
        val CTRL_CHAR_UUID: UUID = UUID.fromString("a0e50003-0001-4d3b-8f1c-2e4a6b8c0d1e")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val OP_REQUEST      = 0x01.toByte()
        const val OP_TEXT_RESP    = 0x02.toByte()
        const val OP_AUDIO_CHUNK  = 0x03.toByte()
        const val OP_DONE         = 0x04.toByte()
        const val OP_CANCEL       = 0x05.toByte()
        const val OP_REQ_READY    = 0x06.toByte()
        const val OP_PAIR         = 0x10.toByte()
        const val OP_DEBUG        = 0x11.toByte()
        const val OP_NOTIF_PUSH   = 0x20.toByte()
        const val OP_NOTIF_QUERY  = 0x21.toByte()
        const val OP_NOTIF_REMOVE = 0x22.toByte()
        // Permanent memory.  Firmware NVS is the source of truth; the
        // phone polls on demand instead of mirroring.  See ble_link.h
        // for the full direction matrix — same opcode (0x23) carries
        // a P->J "store this" and a J->P "list item" because the wire
        // format is identical and direction disambiguates intent.
        const val OP_MEMORY_ADD        = 0x23.toByte()
        const val OP_MEMORY_REMOVE     = 0x24.toByte()
        const val OP_MEMORY_LIST_REQ   = 0x25.toByte()
        const val OP_MEMORY_LIST_END   = 0x26.toByte()
        const val OP_MEMORY_CLEAR      = 0x27.toByte()

        // Developer-mode console (hidden behind 10-tap easter egg).
        const val OP_DEV_ENABLE   = 0x30.toByte()
        const val OP_DEV_LOG      = 0x31.toByte()
        const val OP_DEV_CMD      = 0x32.toByte()
        const val OP_DEV_STATS    = 0x33.toByte()
        // Tasks arrive on a separate opcode after each STATS frame
        // because a full task table doesn't fit in one MTU.  Each
        // chunk: {"t":[...],"f":0|1}; f=1 marks the last chunk.
        const val OP_DEV_TASKS    = 0x34.toByte()

        // Stock-fetch proxy.  Firmware asks the phone to fetch a quote
        // when it has no Wi-Fi of its own; phone returns a compact
        // JSON blob via STOCK_RESPONSE chunks + STOCK_DONE status.
        const val OP_STOCK_REQUEST  = 0x40.toByte()
        const val OP_STOCK_RESPONSE = 0x41.toByte()
        const val OP_STOCK_DONE     = 0x42.toByte()

        // First-time setup wizard.  After pairing, if the device has
        // never been configured it sends SETUP_MODE(1) and the phone
        // presents a WiFi + API-key wizard.  Data flows P→J via the
        // three SETUP_WIFI / SETUP_KEYS / SETUP_COMPLETE opcodes;
        // status updates flow J→P on SETUP_STATUS.
        const val OP_SETUP_MODE      = 0x50.toByte()  // J→P: 1B (1=in_setup, 0=normal)
        const val OP_SETUP_WIFI      = 0x51.toByte()  // P→J: [op][1B enterprise][ssid\0][pass\0][username\0 if enterprise]
        const val OP_SETUP_KEYS      = 0x52.toByte()  // P→J: [op][gemini_key\0][tts_key\0]
        const val OP_SETUP_COMPLETE  = 0x53.toByte()  // P→J: empty — all config sent, proceed
        const val OP_SETUP_STATUS    = 0x54.toByte()  // J→P: 1B (0=wifi_connecting, 1=wifi_connected, 2=auth_fail, 3=not_found, 4=timeout, 5=all_done)

        // Remote settings — phone reads/writes Jibo config over BLE
        const val OP_SETTINGS_REQ    = 0x60.toByte()  // P→J: empty — request all settings
        const val OP_SETTINGS_DATA   = 0x61.toByte()  // J→P: UTF-8 key=value\n blob
        const val OP_SETTINGS_WRITE  = 0x62.toByte()  // P→J: UTF-8 key=value\n (one or more)

        // Messaging proxy — phone reads SMS/contacts on firmware's behalf
        // and relays data via chunked BLE writes.
        private const val OP_MSG_CONVO_REQ      = 0x70.toByte()
        private const val OP_MSG_CONVO_DATA     = 0x71.toByte()
        private const val OP_MSG_CONVO_DONE     = 0x72.toByte()
        private const val OP_MSG_THREAD_REQ     = 0x73.toByte()
        private const val OP_MSG_THREAD_DATA    = 0x74.toByte()
        private const val OP_MSG_THREAD_DONE    = 0x75.toByte()
        private const val OP_MSG_SEND           = 0x76.toByte()
        private const val OP_MSG_SEND_RESULT    = 0x77.toByte()
        private const val OP_MSG_NEW_PUSH       = 0x78.toByte()
        private const val OP_MSG_STT_START      = 0x79.toByte()
        private const val OP_MSG_STT_STOP       = 0x7A.toByte()
        private const val OP_MSG_STT_RESULT     = 0x7B.toByte()
        private const val OP_MSG_STT_ERROR      = 0x7C.toByte()
        private const val OP_MSG_CONTACT_SEARCH = 0x7D.toByte()
        private const val OP_MSG_CONTACT_RESULT = 0x7E.toByte()

        private const val DEBUG_LOG_MAX = 200
        private const val NOTIF_BUFFER_MAX = 50
        private const val DEV_LOG_MAX = 2000  // lines retained in dev-console buffer

    }

    /**
     * One stored permanent memory.  Mirrored on the firmware side.
     * `id` is a 64-bit globally-unique identifier so additions/removals
     * sync bidirectionally without duplicates.
     */
    data class Memory(
        val id: Long,
        val text: String,
        val tsMs: Long
    )

    data class ScanResult(val device: BluetoothDevice, val name: String, val rssi: Int)

    /**
     * Mirrored on the firmware side as `NotifEntry` in `notifications.h`.
     * `key` is `StatusBarNotification.key` so add/remove stay in sync.
     */
    data class NotifEntry(
        val key: String,
        val app: String,
        val sender: String,
        val text: String,
        val category: String,
        val timeSensitive: Boolean,
        val postTimeMs: Long
    )

    enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED }

    private val _scanResults = MutableStateFlow<List<ScanResult>>(emptyList())
    val scanResults: StateFlow<List<ScanResult>> = _scanResults

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState

    private val _jiboName = MutableStateFlow("")
    val jiboName: StateFlow<String> = _jiboName

    private val _isPaired = MutableStateFlow(false)
    val isPaired: StateFlow<Boolean> = _isPaired

    private val _pairingResult = MutableStateFlow<Boolean?>(null)
    val pairingResult: StateFlow<Boolean?> = _pairingResult

    private val _proxyDebug = MutableStateFlow(false)
    val proxyDebug: StateFlow<Boolean> = _proxyDebug

    private val _debugLog = MutableStateFlow<List<String>>(emptyList())
    val debugLog: StateFlow<List<String>> = _debugLog

    // ─── Developer console (in-app debug terminal + dashboard) ─────────────
    //
    // `_devEnabled` is the user-visible toggle (driven by 10 taps on the J
    // icon and persisted in prefs).  Whenever it flips we tell the firmware
    // via OP_DEV_ENABLE so the device only spends BLE bandwidth forwarding
    // serial output when the dev pages are actually being viewed.
    private val _devEnabled = MutableStateFlow(false)
    val devEnabled: StateFlow<Boolean> = _devEnabled

    // Serial log buffer.  Stored as a single immutable string for the UI
    // (cheap append + scroll-to-bottom; the UI splits on newline as needed).
    private val _devLog = MutableStateFlow("")
    val devLog: StateFlow<String> = _devLog

    private val _devStats = MutableStateFlow<DevStats?>(null)
    val devStats: StateFlow<DevStats?> = _devStats

    /**
     * Rolling history of stats samples — most recent at the end.  Capped at
     * roughly two minutes' worth of 1 Hz samples so the time-series cards on
     * the dev dashboard have something to plot without growing unbounded.
     * Entries are appended in `handleDevStats()` whenever a fresh packet
     * lands; cleared when developer mode is toggled off.
     */
    private val _devStatsHistory = MutableStateFlow<List<DevStats>>(emptyList())
    val devStatsHistory: StateFlow<List<DevStats>> = _devStatsHistory
    private val DEV_STATS_HISTORY_MAX = 180   // ~3 min at 1 Hz

    // Watchdog: the firmware-side `gEnabled` lives in RAM only and resets
    // to `false` on every Jibo reboot.  After a reconnect we re-send
    // OP_DEV_ENABLE once (see [onDevModeReconnected]), but a single
    // write-no-response can lose a race against CCCD subscription
    // settling or get dropped by a transiently full GATT queue.  When
    // that happens the user just sees "stale" forever.  This watchdog
    // notices the absence of fresh STATS frames while we *think* dev
    // mode is on and re-sends OP_DEV_ENABLE on a periodic cadence so
    // the link self-heals without requiring app-data wipes.
    private var lastDevStatsAtMs    = 0L
    private var lastDevEnableSentMs = 0L
    private var devWatchdogJob: Job? = null
    private val DEV_STATS_STALE_MS  = 6_000L
    private val DEV_REARM_MIN_GAP_MS = 4_000L
    private val DEV_WATCHDOG_TICK_MS = 2_000L

    /**
     * Cycle accumulator for the chunked stats protocol.  Each cycle the
     * firmware sends one OP_DEV_STATS frame (which seeds [pendingStats]
     * and resets [pendingTasks]) followed by one or more OP_DEV_TASKS
     * frames.  When a TASKS frame arrives with `f:1` we publish the
     * merged sample.  If a cycle is dropped mid-stream the next STATS
     * frame just resets the accumulator — at most one sample is lost.
     */
    private var pendingStats: DevStats? = null
    private val pendingTasks = mutableListOf<DevTask>()

    private val devLogLock = Object()
    private val devLogBuf = StringBuilder()

    /**
     * Single dashboard snapshot from the firmware (~once per second).
     * `tasks` may be empty if the firmware ran out of room building the
     * payload — UI should render "no data" gracefully in that case.
     */
    data class DevStats(
        val uptimeMs: Long,
        val heapFree: Int,
        val heapMin: Int,
        val heapBig: Int,
        val psramFree: Int,
        val psramBig: Int,
        val wifiConnected: Boolean,
        val rssi: Int,
        val cpu0: Int,            // % busy on core 0; -1 = unknown (first sample)
        val cpu1: Int,            // % busy on core 1; -1 = unknown
        val tasks: List<DevTask>,
        val receivedAtMs: Long
    )

    data class DevTask(
        val name: String,
        val priority: Int,
        val state: String,    // R/Y/B/S/D/?
        val stackHwm: Int,    // bytes free at high-water mark
        val core: Int,        // -1 = any
        val cpu: Int          // % wall-clock per core; 0 if not yet measured
    )

    // ─── First-time setup state ──────────────────────────────────────────────
    //
    // Firmware sends OP_SETUP_MODE(1) after pairing if it has never been
    // configured.  The phone then presents a WiFi + API-key wizard and
    // pushes config via OP_SETUP_WIFI / OP_SETUP_KEYS / OP_SETUP_COMPLETE.

    data class WifiNetwork(val ssid: String, val rssi: Int, val isSecured: Boolean, val isEnterprise: Boolean)

    private val _inSetupMode = MutableStateFlow(false)
    val inSetupMode: StateFlow<Boolean> = _inSetupMode

    private val _setupStatus = MutableStateFlow(-1)  // -1 = no status yet
    val setupStatus: StateFlow<Int> = _setupStatus

    private val _wifiNetworks = MutableStateFlow<List<WifiNetwork>>(emptyList())
    val wifiNetworks: StateFlow<List<WifiNetwork>> = _wifiNetworks

    private val _wifiScanning = MutableStateFlow(false)
    val wifiScanning: StateFlow<Boolean> = _wifiScanning

    // Remote settings — key/value map from firmware
    private val _jiboSettings = MutableStateFlow<Map<String, String>>(emptyMap())
    val jiboSettings: StateFlow<Map<String, String>> = _jiboSettings

    private val timeFmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    private fun dbg(msg: String) {
        val line = "${timeFmt.format(Date())}  $msg"
        Log.d(TAG, "[debug] $line")
        val cur = _debugLog.value.toMutableList()
        cur.add(line)
        if (cur.size > DEBUG_LOG_MAX) cur.removeAt(0)
        _debugLog.value = cur
    }

    private lateinit var prefs: SharedPreferences
    private var bleScanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var ctrlChar: BluetoothGattCharacteristic? = null
    private var negotiatedMtu = 20

    private val gattThread = HandlerThread("ble-gatt").apply { start() }
    private val gattHandler = Handler(gattThread.looper)

    private var requestBuffer = ByteArrayOutputStream(4096)
    private var rxChunkCount = 0
    private var deprecatedCallbackLogged = false
    private var currentJob: Job? = null

    private var reconnectJob: Job? = null
    private var reconnectAttempts = 0
    private var shouldReconnect = false

    // ─── Messaging proxy helpers ─────────────────────────────────────────────
    private var messageService: MessageService? = null
    private var contactResolver: ContactResolver? = null
    private var speechManager: SpeechRecognizerManager? = null

    // ─── Notification buffer (mirrored to firmware) ───────────────────────────
    //
    // Keyed by sbn.key so add/remove from the listener stay in sync.  Held in
    // insertion order so the snapshot reply is naturally chronological; capped
    // at NOTIF_BUFFER_MAX (oldest evicted) to keep BLE payloads bounded.
    private val notifBuffer = LinkedHashMap<String, NotifEntry>()
    private val notifLock = Object()

    fun snapshotNotifications(): List<NotifEntry> = synchronized(notifLock) {
        notifBuffer.values.toList()
    }

    // ─── Permanent memory ─────────────────────────────────────────────────────
    //
    // Firmware NVS is the single source of truth.  The phone is a passive
    // viewer: it polls the list when the user opens the memory screen and
    // pushes mutations (delete / clear / phone-side Gemini add) without
    // keeping a local copy.  This avoids the whole class of bidirectional
    // sync bugs (tombstone spam, ping-pong reconnects, etc.) the previous
    // design fought with.
    //
    // [_permanentMemories] holds whatever the most recent LIST_REQ
    // returned.  Optimistic local edits (remove / clearAll) update it
    // immediately so the UI stays snappy even on slow links — the next
    // refresh re-aligns with the device's actual state.
    private val _permanentMemories = MutableStateFlow<List<Memory>>(emptyList())
    val permanentMemories: StateFlow<List<Memory>> = _permanentMemories

    /** Accumulator for an in-flight LIST_REQ response. */
    private val incomingMemoryList = mutableListOf<Memory>()
    private val memListLock = Object()

    /** Ask firmware to stream its current memory list.  Result lands
     *  asynchronously in [_permanentMemories] when LIST_END arrives. */
    fun requestMemoryList() {
        synchronized(memListLock) { incomingMemoryList.clear() }
        writeToRx(byteArrayOf(OP_MEMORY_LIST_REQ))
        Log.d(TAG, "MEMORY_LIST_REQ sent")
    }

    /** Snapshot of the most recent published list — used by the
     *  Gemini-proxy path to inject memories into the system prompt
     *  when the phone is running Gemini on firmware's behalf.  Stale
     *  by at most one connect cycle, which is fine for that purpose. */
    fun snapshotMemories(): List<Memory> = _permanentMemories.value

    /** UI entry point — delete a single memory. */
    fun removeMemory(id: Long) {
        // Optimistic local update: drop from the published list right
        // away so the row disappears without a round-trip stutter.  If
        // the BLE write fails the next refresh will resurrect it.
        _permanentMemories.value = _permanentMemories.value.filterNot { it.id == id }
        sendMemoryRemove(id)
    }

    /** UI entry point — wipe everything. */
    fun clearMemories() {
        _permanentMemories.value = emptyList()
        writeToRx(byteArrayOf(OP_MEMORY_CLEAR))
        Log.d(TAG, "MEMORY_CLEAR sent")
    }

    private fun handleIncomingMemoryItem(id: Long, text: String, tsMs: Long) {
        // Firmware sends ts=0 when its NTP clock hasn't landed yet.
        // Substitute phone time so the UI doesn't display "1970".
        // Anything before 2020 is treated the same as 0 because old
        // codepaths used millis()-since-boot.
        val safeTs = if (tsMs < 1577836800000L) System.currentTimeMillis() else tsMs
        synchronized(memListLock) {
            incomingMemoryList.add(Memory(id, text, safeTs))
        }
    }

    private fun handleMemoryListEnd() {
        val list = synchronized(memListLock) {
            val out = incomingMemoryList.toList()
            incomingMemoryList.clear()
            out
        }
        _permanentMemories.value = list
        Log.d(TAG, "MEMORY_LIST_END — published ${list.size} entries")
    }

    inner class LocalBinder : Binder() {
        val service: BleService get() = this@BleService
    }

    private val binder = LocalBinder()

    override fun onBind(intent: Intent): IBinder {
        super.onBind(intent)
        return binder
    }

    override fun onCreate() {
        super.onCreate()
        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        createNotificationChannel()
        updateNotification("Jibo Link")

        Log.i(TAG, "BleService started — API level ${Build.VERSION.SDK_INT} " +
                "(TIRAMISU=33, using ${if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) "new" else "deprecated"} GATT callback)")

        messageService = MessageService(this)
        contactResolver = ContactResolver(this)
        speechManager = SpeechRecognizerManager(
            context = this,
            onResult = { text ->
                lifecycleScope.launch(Dispatchers.IO) { handleSttResult(text) }
            },
            onError = { code, msg ->
                lifecycleScope.launch(Dispatchers.IO) { handleSttError(code, msg) }
            }
        )

        loadPairedDevice()
        _devEnabled.value = prefs.getBoolean(PREF_DEV_MODE, false)
        if (_devEnabled.value) {
            // Persisted dev mode → arm the watchdog now so the very
            // first reconnect after process start is also covered, not
            // just user-driven toggles and post-discovery re-arms.
            startDevWatchdog()
        }
    }

    private fun createNotificationChannel() {
        val ch = NotificationChannel(CHANNEL_ID, "Jibo BLE", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun updateNotification(text: String) {
        val notif = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Jibo Link")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
        startForeground(NOTIF_ID, notif)
    }

    // ─── Persistence ──────────────────────────────────────────────────────────

    private fun loadPairedDevice() {
        val addr = prefs.getString(PREF_PAIRED_ADDR, null)
        val name = prefs.getString(PREF_PAIRED_NAME, null)
        if (addr != null && name != null) {
            _isPaired.value = true
            _jiboName.value = name
            Log.d(TAG, "Loaded paired Jibo: $name ($addr)")
            shouldReconnect = true
            startReconnect()
        }
    }

    private fun savePairedDevice(address: String, name: String) {
        prefs.edit()
            .putString(PREF_PAIRED_ADDR, address)
            .putString(PREF_PAIRED_NAME, name)
            .apply()
        _isPaired.value = true
        _jiboName.value = name
        Log.d(TAG, "Saved paired Jibo: $name ($address)")
    }

    fun forgetJibo() {
        shouldReconnect = false
        reconnectJob?.cancel()
        reconnectJob = null
        disconnect()
        prefs.edit().clear().apply()
        _isPaired.value = false
        _jiboName.value = ""
        _connectionState.value = ConnectionState.DISCONNECTED
        Log.d(TAG, "Jibo forgotten")
    }

    fun getPairedAddress(): String? = prefs.getString(PREF_PAIRED_ADDR, null)

    // ─── Scanning ─────────────────────────────────────────────────────────────

    fun startScan() {
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        bleScanner = adapter.bluetoothLeScanner
        _scanResults.value = emptyList()

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        bleScanner?.startScan(listOf(filter), settings, scanCallback)
        Log.d(TAG, "Scan started")

        lifecycleScope.launch {
            delay(15_000)
            stopScan()
        }
    }

    fun stopScan() {
        try { bleScanner?.stopScan(scanCallback) } catch (_: Exception) {}
        Log.d(TAG, "Scan stopped")
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: android.bluetooth.le.ScanResult) {
            val dev = result.device
            val name = dev.name ?: "Jibo"
            val existing = _scanResults.value.toMutableList()
            val idx = existing.indexOfFirst { it.device.address == dev.address }
            val entry = ScanResult(dev, name, result.rssi)
            if (idx >= 0) existing[idx] = entry else existing.add(entry)
            _scanResults.value = existing
        }
    }

    // ─── Connection ───────────────────────────────────────────────────────────

    fun connect(device: BluetoothDevice) {
        stopScan()
        reconnectJob?.cancel()
        _connectionState.value = ConnectionState.CONNECTING
        _jiboName.value = device.name ?: "Jibo"
        Log.d(TAG, "Connecting to ${device.address}...")
        gatt = device.connectGatt(this, false, gattCallback,
            BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, gattHandler)
    }

    fun connectToPaired() {
        val addr = getPairedAddress() ?: return
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val device = adapter.getRemoteDevice(addr)
        connect(device)
    }

    fun disconnect() {
        shouldReconnect = false
        reconnectJob?.cancel()
        currentJob?.cancel()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txChar = null
        rxChar = null
        ctrlChar = null
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    // ─── Auto-reconnect ───────────────────────────────────────────────────────

    private fun startReconnect() {
        if (!shouldReconnect || !_isPaired.value) return
        if (_connectionState.value == ConnectionState.CONNECTED) return

        reconnectJob?.cancel()
        reconnectJob = lifecycleScope.launch {
            val delays = longArrayOf(1000, 2000, 4000, 8000, 15000, 30000)
            reconnectAttempts = 0
            while (isActive && shouldReconnect && _isPaired.value) {
                val delayMs = delays[minOf(reconnectAttempts, delays.size - 1)]
                Log.d(TAG, "Reconnect attempt ${reconnectAttempts + 1} in ${delayMs}ms...")
                _connectionState.value = ConnectionState.CONNECTING
                delay(delayMs)

                if (!isActive || !shouldReconnect) break

                val addr = getPairedAddress() ?: break
                val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
                val device = adapter.getRemoteDevice(addr)

                withContext(Dispatchers.Main) {
                    gatt?.close()
                    gatt = device.connectGatt(this@BleService, false, gattCallback,
                        BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK, gattHandler)
                }

                // Wait up to 15s for connection
                var waited = 0
                while (waited < 15000 && _connectionState.value != ConnectionState.CONNECTED && isActive) {
                    delay(200)
                    waited += 200
                }

                if (_connectionState.value == ConnectionState.CONNECTED) {
                    Log.d(TAG, "Reconnected!")
                    reconnectAttempts = 0
                    return@launch
                }

                reconnectAttempts++
                Log.d(TAG, "Reconnect attempt failed")
                gatt?.close()
                gatt = null
            }
        }
    }

    // ─── GATT callback ────────────────────────────────────────────────────────

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d(TAG, "Connected, requesting MTU 512")
                g.requestMtu(512)
            } else {
                Log.d(TAG, "Disconnected (status=$status)")
                _connectionState.value = ConnectionState.DISCONNECTED
                updateNotification("Disconnected from Jibo")
                // Drop any half-received list response so it doesn't
                // bleed into the next session.  Memories themselves
                // stay in [_permanentMemories] as the last good cache;
                // the next connect re-polls and replaces them.
                synchronized(memListLock) { incomingMemoryList.clear() }
                if (_isPaired.value && shouldReconnect) {
                    startReconnect()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            negotiatedMtu = mtu - 3
            Log.d(TAG, "MTU=$mtu, payload=$negotiatedMtu")
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            synchronized(requestBuffer) { requestBuffer.reset() }
            rxChunkCount = 0

            val svc = g.getService(SERVICE_UUID) ?: run {
                Log.e(TAG, "Jibo service not found")
                return
            }
            txChar = svc.getCharacteristic(TX_CHAR_UUID)
            rxChar = svc.getCharacteristic(RX_CHAR_UUID)
            ctrlChar = svc.getCharacteristic(CTRL_CHAR_UUID)

            txChar?.let { c ->
                g.setCharacteristicNotification(c, true)
                c.getDescriptor(CCCD_UUID)?.let { d ->
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                }
            }
            ctrlChar?.let { c ->
                g.setCharacteristicNotification(c, true)
            }

            _connectionState.value = ConnectionState.CONNECTED
            shouldReconnect = true
            reconnectAttempts = 0
            updateNotification("Connected to ${_jiboName.value}")
            Log.d(TAG, "Services discovered, ready")

            // Send the current notification snapshot so Jibo's prompt is in
            // sync immediately on (re)connect — no need to wait for a query.
            lifecycleScope.launch(Dispatchers.IO) {
                delay(300)  // let CCCD writes settle
                try {
                    pushNotificationSnapshot(snapshotNotifications())
                } catch (e: Exception) {
                    Log.w(TAG, "Initial snapshot push failed: ${e.message}")
                }
                // One-shot memory poll so the Gemini-proxy path has a
                // populated cache to inject into the system prompt.
                // The user-visible memory screen does its own refresh on
                // entry, so this is purely for the prompt-injection
                // case — not a bidirectional sync, just a "tell me
                // what you've got" once per session.
                try { requestMemoryList() }
                catch (e: Exception) { Log.w(TAG, "Initial memory poll failed: ${e.message}") }
                // Request settings to sync dev mode flag (DM key) and
                // other settings.  This drives _devEnabled from firmware.
                try { requestSettings() }
                catch (e: Exception) { Log.w(TAG, "Initial settings request failed: ${e.message}") }
                // If dev mode was cached from a previous session, re-arm
                // the firmware-side log forwarding now that link is up.
                try { onDevModeReconnected() }
                catch (e: Exception) { Log.w(TAG, "dev mode re-arm failed: ${e.message}") }
                // Tell firmware whether the phone has messaging permissions
                // so the ESP can enable/disable message-related UI elements.
                try {
                    val ms = if (hasMessagingPermissions()) "1" else "0"
                    writeSettings(mapOf("MS" to ms))
                } catch (e: Exception) {
                    Log.w(TAG, "Messaging capability sync failed: ${e.message}")
                }
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.characteristic.uuid == TX_CHAR_UUID) {
                ctrlChar?.let { c ->
                    c.getDescriptor(CCCD_UUID)?.let { d ->
                        d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        g.writeDescriptor(d)
                    }
                }
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            processCharChanged(g, characteristic.uuid, value)
        }

        @Suppress("DEPRECATION")
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (!deprecatedCallbackLogged) {
                deprecatedCallbackLogged = true
                Log.w(TAG, "Using DEPRECATED onCharacteristicChanged (API ${Build.VERSION.SDK_INT} < 33) — " +
                        "characteristic.value may be stale under high throughput")
            }
            val data = characteristic.value?.copyOf() ?: return
            processCharChanged(g, characteristic.uuid, data)
        }

        private fun processCharChanged(g: BluetoothGatt, uuid: UUID, data: ByteArray) {
            if (data.isEmpty()) return

            when (uuid) {
                TX_CHAR_UUID -> {
                    synchronized(requestBuffer) {
                        requestBuffer.write(data)
                    }
                    rxChunkCount++
                    if (rxChunkCount % 50 == 0) {
                        Log.d(TAG, "TX chunk #$rxChunkCount: ${data.size}B, buffer=${requestBuffer.size()}B")
                    }
                }
                CTRL_CHAR_UUID -> {
                    val op = data[0]
                    Log.d(TAG, "CTRL opcode=0x${String.format("%02X", op)}, size=${data.size}")
                    when (op) {
                        OP_REQ_READY -> {
                            val full: ByteArray
                            synchronized(requestBuffer) {
                                full = requestBuffer.toByteArray()
                                requestBuffer.reset()
                            }
                            val chunks = rxChunkCount
                            rxChunkCount = 0

                            Log.d(TAG, "REQ_READY: ${full.size}B from $chunks chunks")

                            // Verify size + checksum (9 bytes: op + 4B checksum + 4B size)
                            if (data.size >= 9) {
                                val expectedCksum = (data[1].toLong() and 0xFF) or
                                        ((data[2].toLong() and 0xFF) shl 8) or
                                        ((data[3].toLong() and 0xFF) shl 16) or
                                        ((data[4].toLong() and 0xFF) shl 24)
                                val expectedSize = (data[5].toLong() and 0xFF) or
                                        ((data[6].toLong() and 0xFF) shl 8) or
                                        ((data[7].toLong() and 0xFF) shl 16) or
                                        ((data[8].toLong() and 0xFF) shl 24)

                                var actualCksum = 0L
                                for (b in full) actualCksum += (b.toInt() and 0xFF)

                                val sizeOk = full.size.toLong() == expectedSize
                                val cksumOk = expectedCksum == actualCksum

                                if (sizeOk && cksumOk) {
                                    Log.i(TAG, "INTEGRITY OK: size=${full.size} checksum=$actualCksum ($chunks chunks)")
                                    if (_proxyDebug.value) dbg("INTEGRITY OK — ${full.size}B, $chunks chunks, checksum=$actualCksum")
                                } else {
                                    val msg = buildString {
                                        if (!sizeOk) append("SIZE: expected=$expectedSize got=${full.size} (lost ${expectedSize - full.size}B)  ")
                                        if (!cksumOk) append("CHECKSUM: expected=$expectedCksum got=$actualCksum")
                                    }
                                    Log.e(TAG, "*** INTEGRITY FAIL *** $msg")
                                    if (_proxyDebug.value) dbg("*** INTEGRITY FAIL *** $msg")
                                }
                            } else if (data.size >= 5) {
                                // Older firmware without size field
                                val expectedCksum = (data[1].toLong() and 0xFF) or
                                        ((data[2].toLong() and 0xFF) shl 8) or
                                        ((data[3].toLong() and 0xFF) shl 16) or
                                        ((data[4].toLong() and 0xFF) shl 24)
                                var actualCksum = 0L
                                for (b in full) actualCksum += (b.toInt() and 0xFF)
                                val ok = expectedCksum == actualCksum
                                Log.d(TAG, "Checksum ${if (ok) "OK" else "MISMATCH"}: expected=$expectedCksum actual=$actualCksum")
                                if (_proxyDebug.value) dbg("Checksum ${if (ok) "OK" else "MISMATCH"}: $expectedCksum vs $actualCksum")
                            }

                            handleRequest(full)
                        }
                        OP_CANCEL -> {
                            Log.d(TAG, "Cancel received from Jibo")
                            currentJob?.cancel()
                        }
                        OP_PAIR -> {
                            val ok = data.size > 1 && data[1] == 0x01.toByte()
                            _pairingResult.value = ok
                            Log.d(TAG, "Pairing result: $ok")
                            if (ok) {
                                val addr = g.device.address
                                val name = g.device.name ?: "Jibo"
                                savePairedDevice(addr, name)
                            }
                        }
                        OP_NOTIF_QUERY -> {
                            Log.d(TAG, "NOTIF_QUERY received — sending full snapshot")
                            pushNotificationSnapshot(snapshotNotifications())
                        }
                        OP_MEMORY_ADD -> {
                            // J->P direction = list item (we never receive
                            // unsolicited adds anymore, but if firmware
                            // sends one outside a list response we just
                            // append to the in-flight buffer; it'll
                            // surface when LIST_END lands).
                            if (data.size >= 1 + 8 + 8 + 2) {
                                var id = 0L
                                for (k in 0 until 8) id = id or ((data[1 + k].toLong() and 0xFF) shl (k * 8))
                                var ts = 0L
                                for (k in 0 until 8) ts = ts or ((data[9 + k].toLong() and 0xFF) shl (k * 8))
                                val tlen = (data[17].toInt() and 0xFF) or ((data[18].toInt() and 0xFF) shl 8)
                                if (1 + 8 + 8 + 2 + tlen <= data.size) {
                                    val text = String(data, 19, tlen, Charsets.UTF_8)
                                    handleIncomingMemoryItem(id, text, ts)
                                }
                            }
                        }
                        OP_MEMORY_LIST_END -> {
                            handleMemoryListEnd()
                        }
                        OP_DEBUG -> {
                            val on = data.size > 1 && data[1] == 0x01.toByte()
                            _proxyDebug.value = on
                            if (on) {
                                _debugLog.value = emptyList()
                                dbg("=== Proxy debug mode ON ===")
                            } else {
                                dbg("=== Proxy debug mode OFF ===")
                            }
                        }
                        OP_DEV_LOG -> {
                            // [op][utf8 bytes].  Append raw to dev console;
                            // line splitting is the UI's job (the firmware
                            // streams partial lines to keep BLE chatter low).
                            if (data.size > 1) {
                                val text = String(data, 1, data.size - 1, Charsets.UTF_8)
                                appendDevLog(text)
                            }
                        }
                        OP_DEV_STATS -> {
                            if (data.size > 1) {
                                val json = String(data, 1, data.size - 1, Charsets.UTF_8)
                                handleDevStats(json)
                            }
                        }
                        OP_DEV_TASKS -> {
                            if (data.size > 1) {
                                val json = String(data, 1, data.size - 1, Charsets.UTF_8)
                                handleDevTasks(json)
                            }
                        }
                        OP_STOCK_REQUEST -> {
                            // Firmware has no Wi-Fi but wants stock data.
                            // Run on IO so the BLE callback returns
                            // immediately; the response stream is
                            // chunked via STOCK_RESPONSE writes.
                            if (data.size > 1) {
                                val symbol = String(data, 1, data.size - 1,
                                                    Charsets.UTF_8).trim()
                                Log.d(TAG, "STOCK_REQUEST symbol=\"$symbol\"")
                                lifecycleScope.launch(Dispatchers.IO) {
                                    handleStockRequest(symbol)
                                }
                            }
                        }
                        OP_SETUP_MODE -> {
                            val inSetup = data.size > 1 && data[1] != 0x00.toByte()
                            _inSetupMode.value = inSetup
                            Log.d(TAG, "SETUP_MODE: inSetup=$inSetup")
                        }
                        OP_SETUP_STATUS -> {
                            val status = if (data.size > 1) data[1].toInt() and 0xFF else -1
                            _setupStatus.value = status
                            Log.d(TAG, "SETUP_STATUS: status=$status")
                        }
                        OP_SETTINGS_DATA -> {
                            if (data.size > 1) {
                                val payload = String(data, 1, data.size - 1, Charsets.UTF_8)
                                val map = mutableMapOf<String, String>()
                                payload.split('\n').forEach { line ->
                                    val eq = line.indexOf('=')
                                    if (eq > 0) {
                                        map[line.substring(0, eq)] = line.substring(eq + 1)
                                    }
                                }
                                _jiboSettings.value = map
                                Log.d(TAG, "SETTINGS_DATA: ${map.size} keys")

                                // Sync dev mode from firmware's DM flag
                                val firmwareDev = map["DM"] == "1"
                                if (_devEnabled.value != firmwareDev) {
                                    _devEnabled.value = firmwareDev
                                    prefs.edit().putBoolean(PREF_DEV_MODE, firmwareDev).apply()
                                    if (firmwareDev) {
                                        startDevWatchdog()
                                    } else {
                                        synchronized(devLogLock) {
                                            devLogBuf.setLength(0)
                                            _devLog.value = ""
                                        }
                                        _devStats.value = null
                                        _devStatsHistory.value = emptyList()
                                        pendingStats = null
                                        pendingTasks.clear()
                                        stopDevWatchdog()
                                    }
                                }
                            }
                        }
                        // ─── Messaging proxy opcodes ──────────────────────────
                        OP_MSG_CONVO_REQ -> {
                            lifecycleScope.launch(Dispatchers.IO) {
                                handleMsgConvoRequest()
                            }
                        }
                        OP_MSG_THREAD_REQ -> {
                            if (data.size > 1) {
                                val threadId = String(data, 1, data.size - 1, Charsets.UTF_8)
                                lifecycleScope.launch(Dispatchers.IO) {
                                    handleMsgThreadRequest(threadId)
                                }
                            }
                        }
                        OP_MSG_SEND -> {
                            if (data.size > 1) {
                                val json = String(data, 1, data.size - 1, Charsets.UTF_8)
                                lifecycleScope.launch(Dispatchers.IO) {
                                    handleMsgSend(json)
                                }
                            }
                        }
                        OP_MSG_STT_START -> {
                            // Must run on main thread (SpeechRecognizer requirement)
                            lifecycleScope.launch(Dispatchers.Main) {
                                speechManager?.start()
                            }
                        }
                        OP_MSG_STT_STOP -> {
                            lifecycleScope.launch(Dispatchers.Main) {
                                speechManager?.stop()
                            }
                        }
                        OP_MSG_CONTACT_SEARCH -> {
                            if (data.size > 1) {
                                val query = String(data, 1, data.size - 1, Charsets.UTF_8)
                                lifecycleScope.launch(Dispatchers.IO) {
                                    handleContactSearch(query)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ─── Pairing ──────────────────────────────────────────────────────────────

    fun sendPairingCode(code: Int, phoneName: String) {
        _pairingResult.value = null
        val g = gatt ?: return
        val c = ctrlChar ?: return

        val nameBytes = phoneName.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(3 + nameBytes.size)
        payload[0] = OP_PAIR
        payload[1] = (code and 0xFF).toByte()
        payload[2] = ((code shr 8) and 0xFF).toByte()
        nameBytes.copyInto(payload, 3)

        c.value = payload
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent pairing code $code")
    }

    // ─── Request handling ─────────────────────────────────────────────────────

    private fun handleRequest(data: ByteArray) {
        if (data.isEmpty() || data[0] != OP_REQUEST) {
            Log.e(TAG, "handleRequest: invalid data (size=${data.size})")
            return
        }

        currentJob?.let { prev ->
            if (prev.isActive) {
                Log.w(TAG, "handleRequest: cancelling previous job before starting new one")
                prev.cancel()
            }
        }

        val debug = _proxyDebug.value

        currentJob = lifecycleScope.launch(Dispatchers.IO) {
            try {
                val parsed = parseRequest(data)
                if (parsed == null) {
                    Log.e(TAG, "handleRequest: parse failed")
                    if (debug) dbg("PARSE FAILED (${data.size}B)")
                    sendDone(1)
                    return@launch
                }

                val isAudio = parsed.type == 0x01.toByte()
                val typeStr = if (isAudio) "AUDIO" else "TEXT"
                Log.i(TAG, "handleRequest: type=$typeStr, payload=${parsed.audioOrText.size}B, " +
                        "voice=${parsed.voiceIdx}, model=${parsed.modelIdx}")
                if (debug) {
                    dbg("── Request received ──")
                    dbg("  Type: $typeStr  Payload: ${parsed.audioOrText.size}B")
                    dbg("  Voice: ${parsed.voiceIdx}  Model: ${parsed.modelIdx}")
                    dbg("  History: ${parsed.history.length} chars")
                    if (isAudio) {
                        val pcm = parsed.audioOrText
                        val samples = pcm.size / 2
                        val durationMs = samples * 1000L / 16000
                        dbg("  Audio: $samples samples (${durationMs}ms @ 16kHz)")
                        // Log first 4 sample values for cross-checking with firmware
                        if (samples >= 4) {
                            fun s16(i: Int) = ((pcm[i * 2].toInt() and 0xFF) or
                                    (pcm[i * 2 + 1].toInt() shl 8)).toShort()
                            dbg("  PCM[0..3]: ${s16(0)} ${s16(1)} ${s16(2)} ${s16(3)}  (compare Jibo log)")
                        }
                        var sumSq = 0.0
                        var peak = 0
                        for (i in 0 until samples) {
                            val s = ((pcm[i * 2].toInt() and 0xFF) or
                                    (pcm[i * 2 + 1].toInt() shl 8)).toShort().toInt()
                            sumSq += s.toLong() * s
                            val abs = if (s < 0) -s else s
                            if (abs > peak) peak = abs
                        }
                        val rms = Math.sqrt(sumSq / samples).toInt()
                        dbg("  RMS=$rms  Peak=$peak  (compare with Jibo serial log)")
                    } else {
                        dbg("  Text: \"${String(parsed.audioOrText, Charsets.UTF_8).take(80)}\"")
                    }
                }

                if (debug && isAudio) {
                    dbg("Playing received audio on phone speaker...")
                    playPcmOnSpeaker(parsed.audioOrText)
                }

                if (debug) dbg("Calling Gemini API...")
                val t0 = System.currentTimeMillis()
                // Phone is the one running this code, so by definition the
                // link is live (the firmware only routes via BLE when its own
                // WiFi is down).  The sentence in the system prompt reflects
                // that — Jibo can read notifications right now.
                val notifs = snapshotNotifications()
                val mems = snapshotMemories().map { it.text }
                val geminiResp = if (isAudio) {
                    GeminiClient.sendAudio(
                        parsed.apiKey, parsed.modelIdx, parsed.audioOrText, parsed.history,
                        notifications = notifs, phoneLinked = true, memories = mems
                    )
                } else {
                    GeminiClient.sendText(
                        parsed.apiKey, parsed.modelIdx,
                        String(parsed.audioOrText, Charsets.UTF_8), parsed.history,
                        notifications = notifs, phoneLinked = true, memories = mems
                    )
                }
                val geminiMs = System.currentTimeMillis() - t0

                if (geminiResp == null) {
                    Log.e(TAG, "Gemini API FAILED after ${geminiMs}ms")
                    if (debug) dbg("Gemini FAILED after ${geminiMs}ms")
                    sendDone(1)
                    return@launch
                }
                Log.i(TAG, "Gemini OK in ${geminiMs}ms: \"${geminiResp.take(80)}\"")
                if (debug) {
                    dbg("Gemini OK in ${geminiMs}ms")
                    dbg("  Response: \"${geminiResp.take(120)}\"")
                }

                // Run any side-effect tools (store.memory, ...) BEFORE the
                // text response goes out so the firmware sees them executed
                // before it tries to dispatch the spoken response.  Live
                // additions also trigger an OP_MEMORY_ADD so the firmware
                // persists the entry and shows the pill animation.
                applySideEffects(extractSideEffectTools(geminiResp))

                // Send the full response (with any [show.text]{...} tool tag)
                // to firmware so it can dispatch the on-screen display.  The
                // firmware itself strips the tag from what it stores as the
                // spoken response.
                sendTextResponse(geminiResp)
                if (debug) dbg("Text response sent to Jibo")

                // For TTS, strip the tool prefix so Deepgram doesn't read out
                // the bracketed tool syntax aloud.
                val ttsText = stripToolPrefix(geminiResp)

                if (debug) dbg("Streaming Deepgram TTS...")
                val t1 = System.currentTimeMillis()
                var audioBytes = 0L
                DeepgramClient.streamTts(
                    parsed.ttsKey, ttsText, parsed.voiceIdx
                ) { pcmChunk ->
                    if (!isActive) return@streamTts
                    sendAudioChunk(pcmChunk)
                    audioBytes += pcmChunk.size
                }
                val ttsMs = System.currentTimeMillis() - t1
                Log.i(TAG, "TTS done in ${ttsMs}ms, streamed ${audioBytes}B audio")
                if (debug) dbg("TTS done in ${ttsMs}ms, streamed ${audioBytes}B")

                sendDone(0)
                val totalMs = System.currentTimeMillis() - t0
                Log.i(TAG, "Request complete (total ${totalMs}ms)")
                if (debug) dbg("── Request complete (${totalMs}ms) ──")

            } catch (e: CancellationException) {
                Log.d(TAG, "Request cancelled")
                if (debug) dbg("Request CANCELLED")
            } catch (e: Exception) {
                Log.e(TAG, "Request error", e)
                if (debug) dbg("Request ERROR: ${e.message}")
                sendDone(1)
            }
        }
    }

    private fun playPcmOnSpeaker(pcm16kMono: ByteArray) {
        try {
            // Convert byte[] to short[] with explicit LE byte order —
            // eliminates any ambiguity in how AudioTrack interprets raw bytes
            val sampleCount = pcm16kMono.size / 2
            val shortBuf = ShortArray(sampleCount)
            ByteBuffer.wrap(pcm16kMono).order(ByteOrder.LITTLE_ENDIAN)
                .asShortBuffer().get(shortBuf)

            val minBuf = AudioTrack.getMinBufferSize(
                16000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
            )
            if (minBuf <= 0) {
                dbg("Speaker: getMinBufferSize failed ($minBuf)")
                return
            }

            val track = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(16000)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setBufferSizeInBytes(minBuf)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()

            track.play()

            val chunkSamples = minBuf / 2
            var pos = 0
            while (pos < sampleCount) {
                val toWrite = minOf(chunkSamples, sampleCount - pos)
                val written = track.write(shortBuf, pos, toWrite)
                if (written < 0) {
                    dbg("Speaker: write error $written at sample $pos")
                    break
                }
                pos += written
            }

            val durationMs = sampleCount * 1000L / 16000
            dbg("Speaker playback (${durationMs}ms, $sampleCount samples)")
            Thread.sleep(durationMs + 300)
            track.stop()
            track.release()
            dbg("Speaker playback finished")
        } catch (e: Exception) {
            dbg("Speaker playback error: ${e.message}")
            Log.e(TAG, "playPcmOnSpeaker failed", e)
        }
    }

    private data class ParsedRequest(
        val type: Byte,
        val apiKey: String,
        val ttsKey: String,
        val voiceIdx: Int,
        val modelIdx: Int,
        val history: String,
        val audioOrText: ByteArray
    )

    private fun parseRequest(data: ByteArray): ParsedRequest? {
        try {
            var pos = 1
            val type = data[pos++]

            fun readLenPrefixed(): String {
                if (pos + 2 > data.size) throw IndexOutOfBoundsException("len prefix at pos=$pos, data.size=${data.size}")
                val len = (data[pos].toInt() and 0xFF) or ((data[pos + 1].toInt() and 0xFF) shl 8)
                pos += 2
                if (pos + len > data.size) throw IndexOutOfBoundsException("string at pos=$pos, len=$len, data.size=${data.size}")
                val s = String(data, pos, len, Charsets.UTF_8)
                pos += len
                return s
            }

            val apiKey = readLenPrefixed()
            val ttsKey = readLenPrefixed()
            val voiceIdx = data[pos++].toInt() and 0xFF
            val modelIdx = data[pos++].toInt() and 0xFF
            val history = readLenPrefixed()
            val audioOrText = data.copyOfRange(pos, data.size)

            Log.d(TAG, "Parsed: type=0x${String.format("%02X", type)}, payload=${audioOrText.size}B")

            return ParsedRequest(type, apiKey, ttsKey, voiceIdx, modelIdx, history, audioOrText)
        } catch (e: Exception) {
            Log.e(TAG, "Parse error at data.size=${data.size}", e)
            return null
        }
    }

    // ─── BLE writes ───────────────────────────────────────────────────────────

    private val writeLock = Object()

    private fun writeToRx(payload: ByteArray): Boolean {
        val g = gatt ?: return false
        val c = rxChar ?: return false
        synchronized(writeLock) {
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            c.value = payload
            return g.writeCharacteristic(c)
        }
    }

    /**
     * Like [writeToRx] but with bounded backoff retry when Android's
     * GATT write queue is momentarily full (writeCharacteristic returns
     * `false`).  WRITE_NO_RESPONSE writes are queued internally by the
     * stack and a back-to-back call right after a large packet often
     * gets a transient `false` until the controller flushes it.
     *
     * Caller must already be on a background thread — this blocks.
     */
    private fun writeToRxBlocking(payload: ByteArray): Boolean {
        // ~10 attempts × growing backoff ≈ up to ~150 ms total worst
        // case, which is still an order of magnitude below any of our
        // BLE-side timeouts.
        var delayMs = 8L
        repeat(10) {
            if (writeToRx(payload)) return true
            try { Thread.sleep(delayMs) } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return false
            }
            delayMs = (delayMs + 4L).coerceAtMost(30L)
        }
        return false
    }

    /**
     * Strip leading [tool.name]{ ...balanced braces... } prefixes from a
     * Gemini response so we don't pass tool syntax to TTS.  Mirrors
     * `extract_tool_invocation()` in firmware/Main/gemini.cpp.
     */
    /**
     * Walk the leading `[name]{args}` tool blocks at the start of a Gemini
     * response and return only the side-effect ones (anything not `show.*`).
     * Each entry is `Pair(name, args)`.  Mirrors the firmware classifier in
     * `extract_tool_invocation()` so both paths execute identical tools.
     */
    private fun extractSideEffectTools(resp: String): List<Pair<String, String>> {
        val out = mutableListOf<Pair<String, String>>()
        var i = 0
        val n = resp.length
        while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++
        while (i < n && resp[i] == '[') {
            val close = resp.indexOf(']', i + 1)
            if (close < 0) break
            val name = resp.substring(i + 1, close).trim()
            if (!name.contains('.')) break

            var j = close + 1
            while (j < n && (resp[j] == ' ' || resp[j] == '\t')) j++
            if (j >= n || resp[j] != '{') break

            var depth = 1
            var k = j + 1
            while (k < n && depth > 0) {
                val c = resp[k]
                if (c == '{') depth++
                else if (c == '}') { depth--; if (depth == 0) break }
                k++
            }
            if (depth != 0) break

            val args = resp.substring(j + 1, k)
            if (!name.startsWith("show.")) {
                out.add(Pair(name, args))
            }

            i = k + 1
            while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++
        }
        return out
    }

    private fun applySideEffects(tools: List<Pair<String, String>>) {
        for ((name, args) in tools) {
            when (name) {
                "store.memory" -> applyStoreMemory(args.trim())
                "send.message" -> applySendMessage(args.trim())
                else -> Log.d(TAG, "side-effect tool unknown: $name")
            }
        }
    }

    private fun applyStoreMemory(text: String) {
        if (text.isEmpty()) return
        // 64-bit id: high 48 = ms timestamp, low 16 = random.  Matches the
        // make_memory_id() pattern in firmware/Main/gemini.cpp so collisions
        // across both sides are vanishingly rare.
        val ts = System.currentTimeMillis()
        val id = (ts shl 16) or (java.util.concurrent.ThreadLocalRandom.current().nextInt(0x10000).toLong() and 0xFFFFL)
        Log.i(TAG, "store.memory id=$id text=${text.take(80)}")
        // Hand off to firmware — it's the source of truth.  It'll
        // persist + fire the pill.  If the link's down right now this
        // is a no-op; the memory simply isn't recorded (matches the
        // direct-Wi-Fi behavior, where a write to a disconnected
        // device would also be lost).
        sendMemoryPush(id, ts, text)
    }

    /**
     * Execute a `[send.message]{name|text}` tool call from Gemini.
     * Resolves the contact name via fuzzy search, then sends the SMS.
     * This runs in the Gemini-proxy coroutine (IO dispatcher) so
     * blocking calls are fine.
     */
    private fun applySendMessage(args: String) {
        val pipe = args.indexOf('|')
        if (pipe < 0) {
            Log.w(TAG, "send.message: missing pipe separator in \"${args.take(40)}\"")
            return
        }
        val name = args.substring(0, pipe).trim()
        val message = args.substring(pipe + 1).trim()
        if (name.isEmpty() || message.isEmpty()) return

        val matches = contactResolver?.fuzzySearch(name) ?: emptyList()
        if (matches.isEmpty()) {
            Log.w(TAG, "send.message: no contact found for \"$name\"")
            return
        }
        val best = matches.first()
        val success = messageService?.sendMessage(
            phoneNumber = best.phoneNumber,
            appPkg = best.bestApp,
            text = message
        ) ?: false
        Log.i(TAG, "send.message: name=\"$name\" to=${best.phoneNumber} success=$success")
    }

    private fun stripToolPrefix(resp: String): String {
        var i = 0
        val n = resp.length
        while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++
        var afterTools = i

        while (i < n && resp[i] == '[') {
            val close = resp.indexOf(']', i + 1)
            if (close < 0) break
            val name = resp.substring(i + 1, close).trim()
            if (!name.contains('.')) break

            var j = close + 1
            while (j < n && (resp[j] == ' ' || resp[j] == '\t')) j++
            if (j >= n || resp[j] != '{') break

            var depth = 1
            var k = j + 1
            while (k < n && depth > 0) {
                val c = resp[k]
                if (c == '{') depth++
                else if (c == '}') { depth--; if (depth == 0) break }
                k++
            }
            if (depth != 0) break

            i = k + 1
            while (i < n && (resp[i] == ' ' || resp[i] == '\n' || resp[i] == '\t' || resp[i] == '\r')) i++
            afterTools = i
        }

        return resp.substring(afterTools)
    }

    private fun sendTextResponse(text: String) {
        val textBytes = text.toByteArray(Charsets.UTF_8)
        val maxChunk = negotiatedMtu - 1
        var pos = 0
        while (pos < textBytes.size) {
            val end = minOf(pos + maxChunk, textBytes.size)
            val chunk = ByteArray(1 + (end - pos))
            chunk[0] = OP_TEXT_RESP
            textBytes.copyInto(chunk, 1, pos, end)
            if (!writeToRxBlocking(chunk)) {
                Log.w(TAG, "sendTextResponse: write failed at pos=$pos/${textBytes.size}")
            }
            pos = end
            Thread.sleep(6)
        }
    }

    private fun sendAudioChunk(pcm: ByteArray) {
        val maxChunk = negotiatedMtu - 1
        var pos = 0
        while (pos < pcm.size) {
            val end = minOf(pos + maxChunk, pcm.size)
            val chunk = ByteArray(1 + (end - pos))
            chunk[0] = OP_AUDIO_CHUNK
            pcm.copyInto(chunk, 1, pos, end)
            if (!writeToRxBlocking(chunk)) {
                Log.w(TAG, "sendAudioChunk: write failed at pos=$pos/${pcm.size}")
            }
            pos = end
            Thread.sleep(6)
        }
    }

    private fun sendDone(status: Int) {
        Log.d(TAG, "sendDone(status=$status)")
        if (!writeToRxBlocking(byteArrayOf(OP_DONE, status.toByte()))) {
            Log.e(TAG, "sendDone: write failed after retries!")
        }
    }

    // ─── Notification mirroring ───────────────────────────────────────────────

    /** Called by [JiboNotificationListener] when a new notification is posted. */
    fun notifyAdded(entry: NotifEntry) {
        synchronized(notifLock) {
            notifBuffer.remove(entry.key)  // keep insertion order = freshness
            notifBuffer[entry.key] = entry
            while (notifBuffer.size > NOTIF_BUFFER_MAX) {
                val first = notifBuffer.keys.firstOrNull() ?: break
                notifBuffer.remove(first)
            }
        }
        if (_connectionState.value == ConnectionState.CONNECTED) {
            sendNotifPushSingle(entry)
        }
    }

    /** Called by [JiboNotificationListener] when a notification is dismissed. */
    fun notifyRemoved(key: String) {
        synchronized(notifLock) { notifBuffer.remove(key) }
        if (_connectionState.value == ConnectionState.CONNECTED) {
            sendNotifRemove(key)
        }
    }

    // ─── Developer console helpers ────────────────────────────────────────────

    /**
     * Developer mode is now firmware-controlled (toggled in Jibo Settings →
     * About → 10-tap).  This function only sends OP_DEV_ENABLE to activate
     * BLE log forwarding when the firmware is already in dev mode.
     * The `_devEnabled` flag is synced from the firmware's DM setting.
     */
    fun setDevModeEnabled(on: Boolean) {
        // Don't allow toggling from the app — dev mode is firmware-controlled.
        // Only allow enabling BLE log forwarding if firmware is in dev mode.
        if (!_devEnabled.value) return
        sendDevEnable(on)
    }

    /** Re-tell the firmware our current dev-mode preference. */
    fun onDevModeReconnected() {
        if (!_devEnabled.value) return
        // Push the staleness clock forward so the watchdog doesn't
        // re-fire before the firmware's first post-enable STATS packet
        // has had a chance to land (~1 s STATS_PERIOD_MS in firmware
        // plus link/CCCD settling).
        lastDevStatsAtMs = System.currentTimeMillis()
        sendDevEnable(true)
        startDevWatchdog()
    }

    private fun sendDevEnable(on: Boolean) {
        if (_connectionState.value != ConnectionState.CONNECTED) return
        // Use the retrying writer — a transient queue-full on the
        // immediate-after-connect path used to silently drop this
        // single byte and leave dev mode dead until app-data wipe.
        if (writeToRxBlocking(byteArrayOf(OP_DEV_ENABLE, if (on) 0x01 else 0x00))) {
            if (on) lastDevEnableSentMs = System.currentTimeMillis()
        } else {
            Log.w(TAG, "OP_DEV_ENABLE($on) write failed after retries")
        }
    }

    /**
     * Periodic re-arm.  While dev mode is enabled we expect a STATS
     * frame every ~1 s from firmware.  If none arrive for a while
     * (typical cause: Jibo restarted and lost its in-RAM `gEnabled`
     * flag, or our enable byte got dropped) we re-send OP_DEV_ENABLE.
     * Capped by [DEV_REARM_MIN_GAP_MS] so we don't spam writes faster
     * than firmware can prove it heard us by emitting a STATS frame.
     */
    private fun startDevWatchdog() {
        if (devWatchdogJob?.isActive == true) return
        devWatchdogJob = lifecycleScope.launch {
            while (isActive && _devEnabled.value) {
                delay(DEV_WATCHDOG_TICK_MS)
                if (!_devEnabled.value) break
                if (_connectionState.value != ConnectionState.CONNECTED) continue
                val now = System.currentTimeMillis()
                val sinceStats  = now - lastDevStatsAtMs
                val sinceEnable = now - lastDevEnableSentMs
                if (sinceStats > DEV_STATS_STALE_MS &&
                    sinceEnable > DEV_REARM_MIN_GAP_MS) {
                    Log.d(TAG, "dev watchdog: stats stale ${sinceStats}ms — re-sending OP_DEV_ENABLE")
                    sendDevEnable(true)
                }
            }
        }
    }

    private fun stopDevWatchdog() {
        devWatchdogJob?.cancel()
        devWatchdogJob = null
    }

    /** Send a one-line command (no trailing newline) to the firmware's
     *  serial command parser.  Output flows back via OP_DEV_LOG. */
    fun sendDevCommand(line: String) {
        val s = line.trim()
        if (s.isEmpty()) return
        if (_connectionState.value != ConnectionState.CONNECTED) {
            appendDevLog("\n[app] not connected — command dropped: $s\n")
            return
        }
        // Echo locally so the user sees their command in the terminal,
        // matching how the Arduino IDE's serial monitor renders input.
        appendDevLog("> $s\n")
        val bytes = s.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(1 + bytes.size)
        payload[0] = OP_DEV_CMD
        bytes.copyInto(payload, 1)
        // Most commands fit in a single MTU; chunk anyway for safety.
        var pos = 0
        while (pos < payload.size) {
            val end = minOf(pos + negotiatedMtu, payload.size)
            writeToRx(payload.copyOfRange(pos, end))
            pos = end
            if (pos < payload.size) Thread.sleep(5)
        }
    }

    /** Drop any buffered terminal output (UI "clear" button). */
    fun clearDevLog() {
        synchronized(devLogLock) {
            devLogBuf.setLength(0)
            _devLog.value = ""
        }
    }

    private fun appendDevLog(text: String) {
        synchronized(devLogLock) {
            devLogBuf.append(text)
            // Bound the buffer to ~64 KB so a runaway log spam doesn't
            // blow up RAM or freeze the Compose recomposition loop.
            // Trim from the front (oldest) on overflow.
            val MAX = 64 * 1024
            if (devLogBuf.length > MAX) {
                val drop = devLogBuf.length - (MAX * 3 / 4)
                devLogBuf.delete(0, drop)
            }
            _devLog.value = devLogBuf.toString()
        }
    }

    private fun handleDevStats(json: String) {
        try {
            val o = JSONObject(json)
            // Legacy single-frame format kept the task array inline.
            // Newer firmware streams tasks separately on OP_DEV_TASKS;
            // we still accept inline `tasks` so older device builds
            // keep showing data while the user reflashes.
            val inlineTasks = o.optJSONArray("tasks")?.let { arr ->
                val out = mutableListOf<DevTask>()
                for (i in 0 until arr.length()) {
                    val t = arr.getJSONObject(i)
                    out.add(parseTask(t))
                }
                out
            }

            val sample = DevStats(
                uptimeMs      = o.optLong("uptime_ms", 0),
                heapFree      = o.optInt("heap_free", 0),
                heapMin       = o.optInt("heap_min", 0),
                heapBig       = o.optInt("heap_big", 0),
                psramFree     = o.optInt("psram_free", 0),
                psramBig      = o.optInt("psram_big", 0),
                wifiConnected = o.optInt("wifi", 0) == 1,
                rssi          = o.optInt("rssi", 0),
                cpu0          = o.optInt("cpu0", -1),
                cpu1          = o.optInt("cpu1", -1),
                tasks         = inlineTasks ?: emptyList(),
                receivedAtMs  = System.currentTimeMillis()
            )

            if (inlineTasks != null) {
                // Legacy path — publish immediately, no chunking expected.
                pendingStats = null
                pendingTasks.clear()
                publishDevSample(sample)
            } else {
                // Chunked protocol — stash the summary, await OP_DEV_TASKS.
                pendingStats = sample
                pendingTasks.clear()
            }
        } catch (e: Exception) {
            Log.w(TAG, "dev stats parse failed: ${e.message}")
        }
    }

    private fun handleDevTasks(json: String) {
        try {
            val o = JSONObject(json)
            val arr = o.optJSONArray("t") ?: return
            for (i in 0 until arr.length()) {
                pendingTasks.add(parseTask(arr.getJSONObject(i)))
            }
            // `f` is the "final chunk" flag — until it arrives we keep
            // appending to the accumulator without publishing.  Compose
            // would otherwise see a partial task list flicker by.
            val isFinal = o.optInt("f", 0) == 1
            if (isFinal) {
                val base = pendingStats
                if (base != null) {
                    publishDevSample(base.copy(tasks = pendingTasks.toList()))
                }
                pendingStats = null
                pendingTasks.clear()
            }
        } catch (e: Exception) {
            Log.w(TAG, "dev tasks parse failed: ${e.message}")
        }
    }

    private fun parseTask(t: JSONObject): DevTask = DevTask(
        name      = t.optString("n", "?"),
        priority  = t.optInt("p", 0),
        state     = t.optString("s", "?"),
        stackHwm  = t.optInt("hwm", 0),
        core      = t.optInt("core", -1),
        cpu       = t.optInt("cpu", 0)
    )

    private fun publishDevSample(sample: DevStats) {
        _devStats.value = sample
        lastDevStatsAtMs = System.currentTimeMillis()
        // Rolling history — allocate a fresh list each time so Compose's
        // StateFlow equality comparison sees a new reference and emits
        // (in-place mutation would silently drop updates to subscribers).
        val cur = _devStatsHistory.value
        val next = if (cur.size >= DEV_STATS_HISTORY_MAX)
            cur.drop(cur.size - DEV_STATS_HISTORY_MAX + 1) + sample
        else
            cur + sample
        _devStatsHistory.value = next
    }

    // ─── First-time setup BLE writes ─────────────────────────────────────────

    fun sendSetupWifi(ssid: String, password: String, username: String = "", enterprise: Boolean = false) {
        val g = gatt ?: return
        val c = ctrlChar ?: return

        val ssidBytes = ssid.toByteArray(Charsets.UTF_8)
        val passBytes = password.toByteArray(Charsets.UTF_8)
        val userBytes = if (enterprise) username.toByteArray(Charsets.UTF_8) else ByteArray(0)

        // [op][enterprise:1B][ssid\0][pass\0][username\0 if enterprise]
        val size = 1 + 1 + ssidBytes.size + 1 + passBytes.size + 1 + if (enterprise) userBytes.size + 1 else 0
        val payload = ByteArray(size)
        var offset = 0
        payload[offset++] = OP_SETUP_WIFI
        payload[offset++] = if (enterprise) 1.toByte() else 0.toByte()
        ssidBytes.copyInto(payload, offset); offset += ssidBytes.size
        payload[offset++] = 0  // null terminator
        passBytes.copyInto(payload, offset); offset += passBytes.size
        payload[offset++] = 0  // null terminator
        if (enterprise) {
            userBytes.copyInto(payload, offset); offset += userBytes.size
            payload[offset++] = 0  // null terminator
        }

        c.value = payload
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent SETUP_WIFI: ssid=$ssid enterprise=$enterprise")
    }

    fun sendSetupKeys(geminiKey: String, ttsKey: String) {
        val g = gatt ?: return
        val c = ctrlChar ?: return

        val geminiBytes = geminiKey.toByteArray(Charsets.UTF_8)
        val ttsBytes = ttsKey.toByteArray(Charsets.UTF_8)

        val payload = ByteArray(1 + geminiBytes.size + 1 + ttsBytes.size + 1)
        var offset = 0
        payload[offset++] = OP_SETUP_KEYS
        geminiBytes.copyInto(payload, offset); offset += geminiBytes.size
        payload[offset++] = 0
        ttsBytes.copyInto(payload, offset); offset += ttsBytes.size
        payload[offset++] = 0

        c.value = payload
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent SETUP_KEYS")
    }

    fun sendSetupComplete() {
        val g = gatt ?: return
        val c = ctrlChar ?: return

        c.value = byteArrayOf(OP_SETUP_COMPLETE)
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent SETUP_COMPLETE")
    }

    fun resetSetupState() {
        _inSetupMode.value = false
        _setupStatus.value = -1
    }

    // ─── Remote settings ─────────────────────────────────────────────────────

    fun requestSettings() {
        val g = gatt ?: return
        val c = ctrlChar ?: return
        c.value = byteArrayOf(OP_SETTINGS_REQ)
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent SETTINGS_REQ")
    }

    fun writeSettings(pairs: Map<String, String>) {
        val g = gatt ?: return
        val c = ctrlChar ?: return
        val payload = pairs.entries.joinToString("\n") { "${it.key}=${it.value}" }
        val payloadBytes = payload.toByteArray(Charsets.UTF_8)
        val buf = ByteArray(1 + payloadBytes.size)
        buf[0] = OP_SETTINGS_WRITE
        payloadBytes.copyInto(buf, 1)
        c.value = buf
        g.writeCharacteristic(c)
        Log.d(TAG, "Sent SETTINGS_WRITE: ${pairs.size} keys")
    }

    // ─── WiFi scanning ────────────────────────────────────────────────────────

    fun scanWifiNetworks() {
        _wifiScanning.value = true
        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager

        // Use the current scan results (Android throttles startScan)
        // Calling startScan is deprecated on newer Android but we try anyway
        @Suppress("DEPRECATION")
        wifiManager.startScan()

        lifecycleScope.launch {
            delay(3000)  // Give scan time to complete
            @Suppress("DEPRECATION")
            val results = wifiManager.scanResults
            val networks = results
                .filter { it.SSID.isNotEmpty() }
                .distinctBy { it.SSID }
                .sortedByDescending { it.level }
                .map { result ->
                    val caps = result.capabilities
                    val isSecured = caps.contains("WPA") || caps.contains("WEP") || caps.contains("PSK")
                    val isEnterprise = caps.contains("EAP")
                    WifiNetwork(
                        ssid = result.SSID,
                        rssi = result.level,
                        isSecured = isSecured,
                        isEnterprise = isEnterprise
                    )
                }
            _wifiNetworks.value = networks
            _wifiScanning.value = false
            Log.d(TAG, "WiFi scan: ${networks.size} networks found")
        }
    }

    // ─── Stock proxy ────────────────────────────────────────────────────────
    //
    // Firmware sends OP_STOCK_REQUEST with a ticker symbol when it has
    // no Wi-Fi of its own.  We make the Yahoo Finance call here, parse
    // the response down to a small (~700 B) compact JSON and stream it
    // back over OP_STOCK_RESPONSE chunks, terminated by an OP_STOCK_DONE
    // status byte.  Compact form mirrors firmware's stock_quote.cpp
    // parser (json_find_number / json_find_number_array on "price",
    // "prev", "pts").
    private val stockHttp by lazy {
        okhttp3.OkHttpClient.Builder()
            .connectTimeout(8, java.util.concurrent.TimeUnit.SECONDS)
            .readTimeout(8, java.util.concurrent.TimeUnit.SECONDS)
            .build()
    }

    private fun handleStockRequest(rawSymbol: String) {
        // Payload format: "SYMBOL" or "SYMBOL|range_code" (e.g. "NVDA|ytd").
        val parts = rawSymbol.split("|", limit = 2)
        val sym = parts[0].trim().uppercase().filter {
            it.isLetterOrDigit() || it == '.' || it == '-' || it == '^'
        }
        if (sym.isEmpty() || sym.length > 16) {
            sendStockError("Bad symbol")
            return
        }

        val range = if (parts.size > 1) parts[1].trim() else "1d"
        val interval = when (range) {
            "5d"  -> "15m"
            "1mo" -> "1h"
            "6mo", "ytd", "1y" -> "1d"
            else  -> "5m"
        }
        val url = "https://query1.finance.yahoo.com/v8/finance/chart/$sym" +
                  "?range=$range&interval=$interval"
        try {
            val req = okhttp3.Request.Builder()
                .url(url)
                .header("Accept", "application/json")
                // Yahoo tightens the screws on bare User-Agent strings;
                // a browser-ish UA gets us a clean 200 every time.
                .header("User-Agent",
                    "Mozilla/5.0 (Linux; Android 13; Jibo Companion)")
                .get()
                .build()

            stockHttp.newCall(req).execute().use { resp ->
                if (!resp.isSuccessful) {
                    Log.w(TAG, "STOCK fetch HTTP ${resp.code} for $sym")
                    sendStockError("Yahoo HTTP ${resp.code}")
                    return
                }
                val body = resp.body?.string()
                if (body.isNullOrEmpty()) {
                    sendStockError("Empty response")
                    return
                }
                if (body.contains("\"error\":{")) {
                    sendStockError("No data for $sym")
                    return
                }

                // Parse with org.json.  Walk into chart.result[0].meta
                // for the quote and chart.result[0].indicators.quote[0]
                // .close for the line samples.
                val root = JSONObject(body)
                val result = root.optJSONObject("chart")
                    ?.optJSONArray("result")?.optJSONObject(0)
                if (result == null) {
                    sendStockError("Bad payload")
                    return
                }
                val meta = result.optJSONObject("meta") ?: JSONObject()
                val price = meta.optDouble("regularMarketPrice", Double.NaN)
                val prev  = meta.optDouble("chartPreviousClose",
                            meta.optDouble("previousClose", Double.NaN))
                if (price.isNaN() || price == 0.0) {
                    sendStockError("No data for $sym")
                    return
                }

                // Closes: indicators.quote[0].close — a JSON array
                // containing nulls for off-hours buckets.  We emit only
                // the non-null entries; firmware does the same when
                // parsing `pts`.
                val closes = result.optJSONObject("indicators")
                    ?.optJSONArray("quote")?.optJSONObject(0)
                    ?.optJSONArray("close")
                val pts = StringBuilder("[")
                if (closes != null) {
                    var first = true
                    val n = minOf(closes.length(), 260)
                    for (i in 0 until n) {
                        if (closes.isNull(i)) continue
                        val v = closes.optDouble(i, Double.NaN)
                        if (v.isNaN()) continue
                        if (!first) pts.append(',')
                        pts.append(String.format(Locale.US, "%.4f", v))
                        first = false
                    }
                }
                pts.append(']')

                val compact = String.format(
                    Locale.US,
                    "{\"sym\":\"%s\",\"price\":%.4f,\"prev\":%.4f,\"pts\":%s}",
                    sym,
                    if (price.isFinite()) price else 0.0,
                    if (prev.isFinite())  prev  else price,
                    pts.toString()
                )
                Log.d(TAG, "STOCK $sym ok: ${compact.length}B compact")
                streamStockResponse(compact)
            }
        } catch (e: Exception) {
            Log.w(TAG, "STOCK $sym fetch failed", e)
            sendStockError(e.message?.take(48) ?: "Network error")
        }
    }

    private fun streamStockResponse(json: String) {
        val bytes = json.toByteArray(Charsets.UTF_8)
        // Chunk size: stay well under the negotiated MTU so even a
        // freshly-connected phone (default 23 → 20 B payload) works
        // first try.  We bump up to ~negotiatedMtu - 1 once the link
        // has settled.
        val maxChunk = (negotiatedMtu - 1).coerceAtLeast(18)
        var pos = 0
        while (pos < bytes.size) {
            val end = minOf(pos + maxChunk, bytes.size)
            val chunk = ByteArray(1 + (end - pos))
            chunk[0] = OP_STOCK_RESPONSE
            System.arraycopy(bytes, pos, chunk, 1, end - pos)
            if (!writeToRxBlocking(chunk)) {
                Log.w(TAG, "STOCK_RESPONSE write failed at pos=$pos")
                sendStockError("BLE write failed")
                return
            }
            pos = end
            // Tiny gap between writes so the firmware-side ring buffer
            // has time to drain.  Empirically anything under 5 ms
            // occasionally drops a chunk on busy radios.
            if (pos < bytes.size) Thread.sleep(8)
        }
        // Same flush gap before DONE — without it, Android's BLE write
        // queue can still be busy from the last chunk and the DONE
        // packet's writeCharacteristic() returns false → firmware never
        // sees DONE → "phone proxy timeout" on the Jibo side.
        Thread.sleep(8)
        if (!writeToRxBlocking(byteArrayOf(OP_STOCK_DONE, 0x00))) {
            Log.w(TAG, "STOCK_DONE write failed after retries")
        }
    }

    private fun sendStockError(msg: String) {
        // [op][status=1][optional UTF-8 msg].  Firmware caps the
        // accepted error blob at ~60 B so we trim aggressively.
        val msgBytes = msg.take(60).toByteArray(Charsets.UTF_8)
        val payload = ByteArray(2 + msgBytes.size)
        payload[0] = OP_STOCK_DONE
        payload[1] = 0x01
        msgBytes.copyInto(payload, 2)
        if (!writeToRxBlocking(payload)) {
            Log.w(TAG, "STOCK_DONE(error) write failed after retries")
        }
        Log.d(TAG, "STOCK error: $msg")
    }

    /** Push the entire current set of notifications to Jibo (replaces buffer). */
    fun pushNotificationSnapshot(entries: List<NotifEntry>) {
        // Replace our local mirror as well so further pushes stay consistent.
        synchronized(notifLock) {
            notifBuffer.clear()
            for (e in entries) notifBuffer[e.key] = e
            while (notifBuffer.size > NOTIF_BUFFER_MAX) {
                val first = notifBuffer.keys.firstOrNull() ?: break
                notifBuffer.remove(first)
            }
        }
        if (_connectionState.value != ConnectionState.CONNECTED) return
        val json = notifJsonArray(entries.takeLast(NOTIF_BUFFER_MAX))
        sendNotifPushChunked(json)
        Log.d(TAG, "Pushed snapshot: ${entries.size} notifications, ${json.length}B JSON")
    }

    private fun sendNotifPushSingle(entry: NotifEntry) {
        val json = notifJsonObject(entry)
        sendNotifPushChunked(json)
    }

    private fun sendNotifPushChunked(json: String) {
        val bytes = json.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(1 + bytes.size)
        payload[0] = OP_NOTIF_PUSH
        bytes.copyInto(payload, 1)
        // Notifications are short — fits in a single MTU in the common case.
        // Fall back to chunking for large snapshots.
        var pos = 0
        while (pos < payload.size) {
            val end = minOf(pos + negotiatedMtu, payload.size)
            writeToRx(payload.copyOfRange(pos, end))
            pos = end
            if (pos < payload.size) Thread.sleep(5)
        }
    }

    private fun sendNotifRemove(key: String) {
        val keyBytes = key.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(1 + keyBytes.size)
        payload[0] = OP_NOTIF_REMOVE
        keyBytes.copyInto(payload, 1)
        writeToRx(payload)
    }

    // ─── Memory BLE helpers ───────────────────────────────────────────────────

    /** P->J: phone-side Gemini just stored a new memory; ask firmware
     *  to persist it.  Wire format matches MEMORY_LIST_ITEM exactly. */
    fun sendMemoryPush(id: Long, tsMs: Long, text: String) {
        val tBytes = text.toByteArray(Charsets.UTF_8)
        val tlen = minOf(tBytes.size, 240)
        val payload = ByteArray(1 + 8 + 8 + 2 + tlen)
        payload[0] = OP_MEMORY_ADD
        for (k in 0 until 8) payload[1 + k] = ((id ushr (k * 8)) and 0xFF).toByte()
        for (k in 0 until 8) payload[9 + k] = ((tsMs ushr (k * 8)) and 0xFF).toByte()
        payload[17] = (tlen and 0xFF).toByte()
        payload[18] = ((tlen ushr 8) and 0xFF).toByte()
        System.arraycopy(tBytes, 0, payload, 19, tlen)
        writeToRx(payload)
        Log.d(TAG, "MEMORY_ADD id=$id len=$tlen")
    }

    private fun sendMemoryRemove(id: Long) {
        val payload = ByteArray(1 + 8)
        payload[0] = OP_MEMORY_REMOVE
        for (k in 0 until 8) payload[1 + k] = ((id ushr (k * 8)) and 0xFF).toByte()
        writeToRx(payload)
        Log.d(TAG, "MEMORY_REMOVE id=$id")
    }

    private fun jsonEscape(s: String): String {
        val out = StringBuilder(s.length + 8)
        for (c in s) {
            when (c) {
                '\\' -> out.append("\\\\")
                '"'  -> out.append("\\\"")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else -> if (c.code < 0x20) {
                    out.append(String.format("\\u%04x", c.code))
                } else out.append(c)
            }
        }
        return out.toString()
    }

    private fun notifJsonObject(e: NotifEntry): String {
        return buildString {
            append('{')
            append("\"key\":\"").append(jsonEscape(e.key)).append("\",")
            append("\"app\":\"").append(jsonEscape(e.app)).append("\",")
            append("\"sender\":\"").append(jsonEscape(e.sender)).append("\",")
            append("\"text\":\"").append(jsonEscape(e.text)).append("\",")
            append("\"category\":\"").append(jsonEscape(e.category)).append("\",")
            append("\"timeSensitive\":").append(if (e.timeSensitive) "true" else "false")
            append('}')
        }
    }

    private fun notifJsonArray(entries: List<NotifEntry>): String {
        val sb = StringBuilder()
        sb.append('[')
        for ((i, e) in entries.withIndex()) {
            if (i > 0) sb.append(',')
            sb.append(notifJsonObject(e))
        }
        sb.append(']')
        return sb.toString()
    }

    // ─── Messaging proxy handlers ────────────────────────────────────────────

    private suspend fun handleMsgConvoRequest() {
        try {
            val convos = messageService?.getConversations() ?: emptyList()
            val json = messageService?.conversationsToJson(convos) ?: "[]"
            streamChunkedPayload(OP_MSG_CONVO_DATA, json.toByteArray(Charsets.UTF_8))
            writeToRxBlocking(byteArrayOf(OP_MSG_CONVO_DONE, 0x00))  // success
        } catch (e: Exception) {
            Log.e(TAG, "Convo request failed", e)
            writeToRxBlocking(byteArrayOf(OP_MSG_CONVO_DONE, 0x01))  // error
        }
    }

    private suspend fun handleMsgThreadRequest(threadId: String) {
        try {
            val messages = messageService?.getThreadMessages(threadId) ?: emptyList()
            val json = messageService?.messagesToJson(messages) ?: "[]"
            streamChunkedPayload(OP_MSG_THREAD_DATA, json.toByteArray(Charsets.UTF_8))
            writeToRxBlocking(byteArrayOf(OP_MSG_THREAD_DONE, 0x00))
        } catch (e: Exception) {
            Log.e(TAG, "Thread request failed", e)
            writeToRxBlocking(byteArrayOf(OP_MSG_THREAD_DONE, 0x01))
        }
    }

    private suspend fun handleMsgSend(json: String) {
        try {
            val obj = JSONObject(json)
            val threadId = obj.optString("thread_id", "").ifEmpty { null }
            val phone = obj.optString("phone", "").ifEmpty { null }
            val appPkg = obj.optString("app_pkg", "").ifEmpty { null }
            val text = obj.getString("text")

            val success = messageService?.sendMessage(threadId, phone, appPkg, text) ?: false
            writeToRxBlocking(byteArrayOf(OP_MSG_SEND_RESULT, if (success) 0x00 else 0x01))
        } catch (e: Exception) {
            Log.e(TAG, "Send failed", e)
            writeToRxBlocking(byteArrayOf(OP_MSG_SEND_RESULT, 0x01))
        }
    }

    private suspend fun handleContactSearch(query: String) {
        try {
            val matches = contactResolver?.fuzzySearch(query) ?: emptyList()
            val json = contactResolver?.toJson(matches) ?: "[]"
            streamChunkedPayload(OP_MSG_CONTACT_RESULT, json.toByteArray(Charsets.UTF_8))
        } catch (e: Exception) {
            Log.e(TAG, "Contact search failed", e)
            streamChunkedPayload(OP_MSG_CONTACT_RESULT, "[]".toByteArray(Charsets.UTF_8))
        }
    }

    private fun handleSttResult(text: String) {
        val textBytes = text.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(1 + textBytes.size)
        payload[0] = OP_MSG_STT_RESULT
        textBytes.copyInto(payload, 1)
        writeChunkedFireAndForget(payload)
    }

    private fun handleSttError(code: Int, message: String) {
        val msgBytes = message.toByteArray(Charsets.UTF_8)
        val payload = ByteArray(2 + msgBytes.size)
        payload[0] = OP_MSG_STT_ERROR
        payload[1] = code.toByte()
        msgBytes.copyInto(payload, 2)
        writeChunkedFireAndForget(payload)
    }

    /** Fire-and-forget chunked write (no retry).  Used for STT callbacks
     *  that run on the main thread where blocking is undesirable. */
    private fun writeChunkedFireAndForget(payload: ByteArray) {
        var pos = 0
        while (pos < payload.size) {
            val end = minOf(pos + negotiatedMtu, payload.size)
            writeToRx(payload.copyOfRange(pos, end))
            pos = end
            if (pos < payload.size) Thread.sleep(6)
        }
    }

    /**
     * Stream a chunked BLE response: each chunk is [opcode][payload_slice].
     * Mirrors the pattern used by [streamStockResponse].
     */
    private fun streamChunkedPayload(opcode: Byte, data: ByteArray) {
        val maxChunk = (negotiatedMtu - 1).coerceAtLeast(18)
        var pos = 0
        while (pos < data.size) {
            val end = minOf(pos + maxChunk, data.size)
            val chunk = ByteArray(1 + (end - pos))
            chunk[0] = opcode
            System.arraycopy(data, pos, chunk, 1, end - pos)
            if (!writeToRxBlocking(chunk)) {
                Log.w(TAG, "streamChunkedPayload(0x${String.format("%02X", opcode)}): write failed at pos=$pos")
                return
            }
            pos = end
            if (pos < data.size) Thread.sleep(8)
        }
    }

    /**
     * Check whether the user has granted the SMS + Contacts permissions
     * needed for the messaging feature.  Used by [writeSettings] to
     * inject the MS (messaging-supported) capability flag.
     */
    private fun hasMessagingPermissions(): Boolean {
        return checkSelfPermission(android.Manifest.permission.READ_SMS) ==
                PackageManager.PERMISSION_GRANTED &&
               checkSelfPermission(android.Manifest.permission.READ_CONTACTS) ==
                PackageManager.PERMISSION_GRANTED
    }

    override fun onDestroy() {
        shouldReconnect = false
        reconnectJob?.cancel()
        currentJob?.cancel()
        speechManager?.destroy()
        gatt?.disconnect()
        gatt?.close()
        gattThread.quitSafely()
        super.onDestroy()
    }
}
