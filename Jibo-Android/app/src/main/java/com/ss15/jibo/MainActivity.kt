package com.ss15.jibo

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.provider.Settings
import androidx.core.app.NotificationManagerCompat
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.ss15.jibo.ui.theme.JiboTheme

class MainActivity : ComponentActivity() {

    private var bleService: BleService? = null
    private var bound = mutableStateOf(false)

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            bleService = (binder as BleService.LocalBinder).service
            bound.value = true
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            bleService = null
            bound.value = false
        }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        if (results.values.all { it }) startAndBind()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            JiboTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color(0xFF0A0A0A)
                ) {
                    if (bound.value && bleService != null) {
                        JiboApp(bleService!!)
                    } else {
                        LoadingScreen()
                    }
                }
            }
        }

        requestPermissionsAndStart()
    }

    private fun requestPermissionsAndStart() {
        val perms = mutableListOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= 33) {
            perms.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        val needed = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isEmpty()) startAndBind()
        else permLauncher.launch(needed.toTypedArray())
    }

    private fun startAndBind() {
        val intent = Intent(this, BleService::class.java)
        startForegroundService(intent)
        bindService(intent, connection, Context.BIND_AUTO_CREATE)
    }

    override fun onDestroy() {
        if (bound.value) unbindService(connection)
        super.onDestroy()
    }
}

@Composable
fun LoadingScreen() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        CircularProgressIndicator(color = Color.White, strokeWidth = 2.dp)
    }
}

@Composable
fun JiboApp(service: BleService) {
    val connState by service.connectionState.collectAsState()
    val scanResults by service.scanResults.collectAsState()
    val pairingResult by service.pairingResult.collectAsState()
    val jiboName by service.jiboName.collectAsState()
    val paired by service.isPaired.collectAsState()
    val proxyDebug by service.proxyDebug.collectAsState()
    val debugLog by service.debugLog.collectAsState()
    val devEnabled by service.devEnabled.collectAsState()
    val inSetup by service.inSetupMode.collectAsState()
    val setupStat by service.setupStatus.collectAsState()

    // Determine which screen to show based on persistent state
    var screen by remember { mutableStateOf(if (paired) "home" else "scan") }

    // React to pairing completing — wait briefly for a potential
    // SETUP_MODE signal before jumping to the home screen.
    LaunchedEffect(paired) {
        if (paired && screen == "pair") {
            screen = "setup_pending"
        }
        if (!paired && screen == "home") {
            screen = "scan"
        }
    }

    // If the firmware signals setup mode, enter the wizard.
    // If setup_pending doesn't get a SETUP_MODE within 3 s, go home.
    LaunchedEffect(inSetup, screen) {
        if (inSetup && (screen == "setup_pending" || screen == "pair")) {
            screen = "setup"
        }
        if (screen == "setup_pending") {
            kotlinx.coroutines.delay(3000)
            if (screen == "setup_pending") screen = "home"
        }
    }

    // When setup finishes (status 5 = all_done), transition to home.
    LaunchedEffect(setupStat) {
        if (setupStat == 5 && screen == "setup") {
            kotlinx.coroutines.delay(2000)
            service.resetSetupState()
            screen = "home"
        }
    }

    // When connected during scan flow, go to pair screen
    LaunchedEffect(connState) {
        if (connState == BleService.ConnectionState.CONNECTED && screen == "scan") {
            screen = "pair"
        }
    }

    if (proxyDebug) {
        DebugScreen(log = debugLog, connState = connState)
    } else {
        Box(modifier = Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 24.dp)
                    .statusBarsPadding()
            ) {
                Spacer(Modifier.height(32.dp))

                Text(
                    "Jibo",
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Medium,
                    color = Color.White,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center
                )

                Spacer(Modifier.height(4.dp))

                Text(
                    when (screen) {
                        "scan" -> "Find your Jibo"
                        "pair" -> "Pair with Jibo"
                        "home" -> if (connState == BleService.ConnectionState.CONNECTED) "Connected" else "Reconnecting..."
                        "memory" -> "Permanent Memory"
                        "settings" -> "Device Settings"
                        "dev" -> "Developer Tools"
                        "setup" -> "First-time Setup"
                        "setup_pending" -> "Setting up..."
                        else -> ""
                    },
                    fontSize = 14.sp,
                    color = Color(0xFF888888),
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center
                )

                Spacer(Modifier.height(32.dp))

                when (screen) {
                    "scan" -> ScanScreen(
                        service = service,
                        results = scanResults,
                        onSelect = { result ->
                            service.connect(result.device)
                        }
                    )
                    "pair" -> PairScreen(
                        service = service,
                        jiboName = jiboName,
                        pairingResult = pairingResult
                    )
                    "home" -> HomeScreen(
                        service = service,
                        jiboName = jiboName,
                        connState = connState,
                        onForget = {
                            service.forgetJibo()
                            screen = "scan"
                        },
                        onOpenMemory = { screen = "memory" },
                        onOpenSettings = { screen = "settings" }
                    )
                    "settings" -> JiboSettingsScreen(
                        service = service,
                        onBack = { screen = "home" }
                    )
                    "memory" -> PermanentMemoryScreen(
                        service = service,
                        onBack = { screen = "home" }
                    )
                    "setup" -> SetupScreen(service = service)
                    "setup_pending" -> {
                        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator(color = Color.White, strokeWidth = 2.dp)
                        }
                    }
                    "dev" -> DevToolsScreen(
                        service = service,
                        onBack = { screen = "home" }
                    )
                }
            }

            // Dev-mode entry point: a small ">_" terminal glyph in the
            // top-left.  Only visible when the firmware is in developer mode
            // (toggled from Jibo's Settings → About → 10-tap).  Hidden
            // entirely otherwise so the regular UI stays clean.
            if (devEnabled && screen != "dev" && screen != "scan" && screen != "pair") {
                DevModeButton(
                    onClick = { screen = "dev" },
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .statusBarsPadding()
                        .padding(start = 12.dp, top = 12.dp)
                )
            }
        }
    }
}

@Composable
private fun DevModeButton(onClick: () -> Unit, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(36.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(Color(0xFF1A1A1A))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Text(
            ">_",
            color = Color(0xFF60FF80),
            fontSize = 14.sp,
            fontWeight = FontWeight.Bold
        )
    }
}

// ─── Scan Screen ────────────────────────────────────────────────────────────

@Composable
fun ScanScreen(
    service: BleService,
    results: List<BleService.ScanResult>,
    onSelect: (BleService.ScanResult) -> Unit
) {
    var scanning by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxWidth()) {
        Button(
            onClick = {
                scanning = true
                service.startScan()
            },
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
            shape = RoundedCornerShape(26.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Color.White)
        ) {
            if (scanning && results.isEmpty()) {
                CircularProgressIndicator(
                    modifier = Modifier.size(20.dp),
                    color = Color.Black,
                    strokeWidth = 2.dp
                )
                Spacer(Modifier.width(12.dp))
            }
            Text(
                if (scanning) "Scanning..." else "Scan for Jibo",
                color = Color.Black,
                fontWeight = FontWeight.SemiBold,
                fontSize = 16.sp
            )
        }

        Spacer(Modifier.height(12.dp))

        Text(
            "Make sure you've tapped \"Pair Phone\"\nin Jibo's settings first",
            fontSize = 12.sp,
            color = Color(0xFF666666),
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth()
        )

        Spacer(Modifier.height(24.dp))

        if (results.isNotEmpty()) {
            Text(
                "NEARBY DEVICES",
                fontSize = 12.sp,
                color = Color(0xFF666666),
                letterSpacing = 1.5.sp,
                fontWeight = FontWeight.Medium
            )
            Spacer(Modifier.height(12.dp))
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(results) { result ->
                DeviceRow(result = result, onClick = { onSelect(result) })
            }
        }
    }
}

@Composable
fun DeviceRow(result: BleService.ScanResult, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(Color(0xFF1A1A1A))
            .clickable(onClick = onClick)
            .padding(16.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(40.dp)
                .clip(CircleShape)
                .background(Color(0xFF2A2A2A)),
            contentAlignment = Alignment.Center
        ) {
            Text("J", color = Color.White, fontWeight = FontWeight.Bold, fontSize = 18.sp)
        }

        Spacer(Modifier.width(16.dp))

        Column(modifier = Modifier.weight(1f)) {
            Text(result.name, color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.Medium)
            Text("Signal: ${result.rssi} dBm", color = Color(0xFF888888), fontSize = 12.sp)
        }

        Text("Connect", color = Color(0xFF3CA0FF), fontSize = 14.sp, fontWeight = FontWeight.Medium)
    }
}

// ─── Pair Screen ────────────────────────────────────────────────────────────

@Composable
fun PairScreen(
    service: BleService,
    jiboName: String,
    pairingResult: Boolean?
) {
    var code by remember { mutableStateOf("") }
    var sent by remember { mutableStateOf(false) }

    LaunchedEffect(pairingResult) {
        if (pairingResult == false) {
            sent = false
            code = ""
        }
    }

    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            "Enter the 4-digit code\nshown on $jiboName",
            fontSize = 16.sp,
            color = Color(0xFFCCCCCC),
            textAlign = TextAlign.Center,
            lineHeight = 24.sp
        )

        Spacer(Modifier.height(32.dp))

        OutlinedTextField(
            value = code,
            onValueChange = { if (it.length <= 4 && it.all { c -> c.isDigit() }) code = it },
            label = { Text("Pairing Code") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            singleLine = true,
            colors = OutlinedTextFieldDefaults.colors(
                focusedTextColor = Color.White,
                unfocusedTextColor = Color.White,
                focusedBorderColor = Color.White,
                unfocusedBorderColor = Color(0xFF444444),
                focusedLabelColor = Color(0xFF888888),
                unfocusedLabelColor = Color(0xFF888888),
                cursorColor = Color.White
            ),
            modifier = Modifier.width(200.dp),
            textStyle = LocalTextStyle.current.copy(
                fontSize = 32.sp,
                textAlign = TextAlign.Center,
                letterSpacing = 12.sp
            )
        )

        Spacer(Modifier.height(32.dp))

        if (pairingResult == false) {
            Text(
                "Code didn't match. Try again.",
                color = Color(0xFFFF6060),
                fontSize = 14.sp
            )
            Spacer(Modifier.height(16.dp))
        }

        Button(
            onClick = {
                val c = code.toIntOrNull() ?: return@Button
                service.sendPairingCode(c, Build.MODEL)
                sent = true
            },
            enabled = code.length == 4 && !sent,
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
            shape = RoundedCornerShape(26.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Color.White)
        ) {
            if (sent && pairingResult == null) {
                CircularProgressIndicator(
                    modifier = Modifier.size(20.dp),
                    color = Color.Black,
                    strokeWidth = 2.dp
                )
            } else {
                Text("Pair", color = Color.Black, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
            }
        }
    }
}

// ─── Home Screen (paired) ───────────────────────────────────────────────────

private fun isNotifAccessGranted(ctx: Context): Boolean {
    return NotificationManagerCompat.getEnabledListenerPackages(ctx)
        .contains(ctx.packageName)
}

@Composable
fun HomeScreen(
    service: BleService,
    jiboName: String,
    connState: BleService.ConnectionState,
    onForget: () -> Unit,
    onOpenMemory: () -> Unit,
    onOpenSettings: () -> Unit = {}
) {
    val isConnected = connState == BleService.ConnectionState.CONNECTED
    var showForgetConfirm by remember { mutableStateOf(false) }
    val ctx = androidx.compose.ui.platform.LocalContext.current
    // Dev mode is firmware-controlled (Jibo Settings → About → 10-tap).
    // No longer toggled from the app.
    // Re-check on each recomposition triggered by tick — granting access in
    // Settings → Notifications → Special access lives outside our process.
    var notifAccess by remember { mutableStateOf(isNotifAccessGranted(ctx)) }
    LaunchedEffect(Unit) {
        while (true) {
            kotlinx.coroutines.delay(2000)
            notifAccess = isNotifAccessGranted(ctx)
        }
    }

    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(40.dp))

        Box(
            modifier = Modifier
                .size(80.dp)
                .clip(CircleShape)
                .background(if (isConnected) Color(0xFF1A3A20) else Color(0xFF2A2020)),
            contentAlignment = Alignment.Center
        ) {
            if (isConnected) {
                Text("J", color = Color(0xFF60C060), fontSize = 36.sp, fontWeight = FontWeight.Bold)
            } else {
                CircularProgressIndicator(
                    modifier = Modifier.size(32.dp),
                    color = Color(0xFF888888),
                    strokeWidth = 2.dp
                )
            }
        }

        Spacer(Modifier.height(24.dp))

        Text(
            jiboName,
            fontSize = 22.sp,
            color = Color.White,
            fontWeight = FontWeight.Medium
        )

        Spacer(Modifier.height(8.dp))

        Text(
            if (isConnected) "Linked and ready"
            else "Trying to connect...",
            fontSize = 14.sp,
            color = if (isConnected) Color(0xFF60C060) else Color(0xFFCC8844)
        )

        Spacer(Modifier.height(12.dp))

        Text(
            if (isConnected) "Your phone is acting as an internet\nproxy for Jibo. Keep this app running."
            else "Make sure Jibo is powered on and\nwithin Bluetooth range.",
            fontSize = 13.sp,
            color = Color(0xFF888888),
            textAlign = TextAlign.Center,
            lineHeight = 20.sp
        )

        Spacer(Modifier.height(24.dp))

        if (!notifAccess) {
            NotificationAccessCard(
                onGrant = {
                    ctx.startActivity(
                        Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                }
            )
            Spacer(Modifier.height(16.dp))
        } else {
            Spacer(Modifier.height(8.dp))
        }

        MemoryEntryCard(onClick = onOpenMemory)
        Spacer(Modifier.height(12.dp))
        SettingsEntryCard(onClick = onOpenSettings)
        Spacer(Modifier.height(20.dp))

        if (showForgetConfirm) {
            Text(
                "This will unpair from Jibo.\nYou'll need to pair again.",
                fontSize = 13.sp,
                color = Color(0xFF888888),
                textAlign = TextAlign.Center,
                lineHeight = 20.sp
            )
            Spacer(Modifier.height(16.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OutlinedButton(
                    onClick = { showForgetConfirm = false },
                    modifier = Modifier
                        .weight(1f)
                        .height(48.dp),
                    shape = RoundedCornerShape(24.dp),
                    border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
                ) {
                    Text("Cancel", color = Color(0xFFAAAAAA), fontSize = 14.sp)
                }
                Button(
                    onClick = onForget,
                    modifier = Modifier
                        .weight(1f)
                        .height(48.dp),
                    shape = RoundedCornerShape(24.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF882222))
                ) {
                    Text("Forget", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        } else {
            OutlinedButton(
                onClick = { showForgetConfirm = true },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
                shape = RoundedCornerShape(26.dp),
                border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
            ) {
                Text("Forget Jibo", color = Color(0xFF888888), fontWeight = FontWeight.Medium, fontSize = 14.sp)
            }
        }
    }
}

@Composable
private fun NotificationAccessCard(onGrant: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(Color(0xFF1A1A1A))
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            "Let Jibo read notifications",
            color = Color.White,
            fontSize = 15.sp,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center
        )
        Spacer(Modifier.height(6.dp))
        Text(
            "Grant notification access so Jibo can answer questions like \"any messages from Avalon?\"",
            color = Color(0xFF999999),
            fontSize = 12.sp,
            textAlign = TextAlign.Center,
            lineHeight = 16.sp
        )
        Spacer(Modifier.height(12.dp))
        Button(
            onClick = onGrant,
            modifier = Modifier
                .fillMaxWidth()
                .height(44.dp),
            shape = RoundedCornerShape(22.dp),
            colors = ButtonDefaults.buttonColors(containerColor = Color.White)
        ) {
            Text("Grant access", color = Color.Black, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
        }
    }
}

@Composable
private fun MemoryEntryCard(onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(Color(0xFF1A1A1A))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .clip(CircleShape)
                .background(Color(0xFF223344)),
            contentAlignment = Alignment.Center
        ) {
            Text("M", color = Color(0xFF60C0FF), fontSize = 16.sp, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.width(14.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text("Permanent Memory", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Medium)
            Text(
                "Things Jibo remembers about you",
                color = Color(0xFF888888),
                fontSize = 12.sp
            )
        }
        Text("›", color = Color(0xFF666666), fontSize = 24.sp)
    }
}

// ─── Permanent Memory Screen ────────────────────────────────────────────────

@Composable
fun PermanentMemoryScreen(
    service: BleService,
    onBack: () -> Unit
) {
    val memories by service.permanentMemories.collectAsState()
    var showClearConfirm by remember { mutableStateOf(false) }

    // Phone is a passive viewer — firmware NVS is the source of truth.
    // Re-poll whenever the user lands here so the list always reflects
    // the device's current state, including memories Gemini stored
    // since the last time this screen was open.
    LaunchedEffect(Unit) { service.requestMemoryList() }

    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedButton(
                onClick = onBack,
                modifier = Modifier.height(40.dp),
                shape = RoundedCornerShape(20.dp),
                border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
            ) {
                Text("‹ Back", color = Color(0xFFAAAAAA), fontSize = 14.sp)
            }
            Spacer(Modifier.weight(1f))
            if (memories.isNotEmpty()) {
                OutlinedButton(
                    onClick = { showClearConfirm = true },
                    modifier = Modifier.height(40.dp),
                    shape = RoundedCornerShape(20.dp),
                    border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF553333))
                ) {
                    Text("Clear All", color = Color(0xFFCC6666), fontSize = 13.sp)
                }
            }
        }

        Spacer(Modifier.height(20.dp))

        if (showClearConfirm) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color(0xFF1A1A1A))
                    .padding(16.dp)
            ) {
                Text(
                    "Clear all permanent memories?",
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(Modifier.height(6.dp))
                Text(
                    "Jibo will forget every saved memory on both this phone and the device. This cannot be undone.",
                    color = Color(0xFF999999),
                    fontSize = 12.sp,
                    lineHeight = 16.sp
                )
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedButton(
                        onClick = { showClearConfirm = false },
                        modifier = Modifier.weight(1f).height(44.dp),
                        shape = RoundedCornerShape(22.dp),
                        border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
                    ) {
                        Text("Cancel", color = Color(0xFFAAAAAA), fontSize = 14.sp)
                    }
                    Button(
                        onClick = {
                            service.clearMemories()
                            showClearConfirm = false
                        },
                        modifier = Modifier.weight(1f).height(44.dp),
                        shape = RoundedCornerShape(22.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF882222))
                    ) {
                        Text("Clear All", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
            }
            Spacer(Modifier.height(20.dp))
        }

        if (memories.isEmpty()) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 60.dp),
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        "No permanent memories yet",
                        color = Color(0xFFAAAAAA),
                        fontSize = 15.sp,
                        fontWeight = FontWeight.Medium
                    )
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "Ask Jibo to remember something\nlike \"remember that I live in Windsor\"",
                        color = Color(0xFF666666),
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                        lineHeight = 18.sp
                    )
                }
            }
        } else {
            LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(memories, key = { it.id }) { mem ->
                    MemoryRow(
                        text = mem.text,
                        ageMs = System.currentTimeMillis() - mem.tsMs,
                        onDelete = { service.removeMemory(mem.id) }
                    )
                }
            }
        }
    }
}

@Composable
private fun MemoryRow(text: String, ageMs: Long, onDelete: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(Color(0xFF1A1A1A))
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text, color = Color.White, fontSize = 14.sp, lineHeight = 19.sp)
            Spacer(Modifier.height(2.dp))
    Text(
                relativeAge(ageMs),
                color = Color(0xFF666666),
                fontSize = 11.sp
            )
        }
        Spacer(Modifier.width(12.dp))
        Box(
            modifier = Modifier
                .size(36.dp)
                .clip(CircleShape)
                .background(Color(0xFF2A2020))
                .clickable(onClick = onDelete),
            contentAlignment = Alignment.Center
        ) {
            Text("×", color = Color(0xFFCC6666), fontSize = 22.sp, fontWeight = FontWeight.Bold)
        }
    }
}

private fun relativeAge(dtMs: Long): String {
    // Negative ages happen when the firmware's clock hadn't synced yet
    // when the memory was recorded (ts=0 -> "1970", giving a huge dtMs).
    // Treat anything older than 5 years as "unknown" rather than show a
    // nonsense "685 mo ago" in the UI.
    if (dtMs < 0 || dtMs > 5L * 365 * 24 * 60 * 60 * 1000) return "recently"
    if (dtMs < 60_000)              return "just now"
    val mins = dtMs / 60_000
    if (mins < 60)                  return "$mins min ago"
    val hrs = mins / 60
    if (hrs < 24)                   return "$hrs hr ago"
    val days = hrs / 24
    if (days < 30)                  return "$days day${if (days == 1L) "" else "s"} ago"
    val months = days / 30
    return "$months mo ago"
}

// ─── Settings Entry Card (Home screen) ──────────────────────────────────────

@Composable
private fun SettingsEntryCard(onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(Color(0xFF1A1A1A))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .clip(CircleShape)
                .background(Color(0xFF332244)),
            contentAlignment = Alignment.Center
        ) {
            Text("⚙", color = Color(0xFFBB99DD), fontSize = 16.sp)
        }
        Spacer(Modifier.width(14.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text("Settings", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Medium)
            Text(
                "API keys, voice, model & more",
                color = Color(0xFF888888),
                fontSize = 12.sp
            )
        }
        Text("›", color = Color(0xFF666666), fontSize = 24.sp)
    }
}

// ─── Jibo Settings Screen ──────────────────────────────────────────────────

@Composable
fun JiboSettingsScreen(
    service: BleService,
    onBack: () -> Unit
) {
    val settings by service.jiboSettings.collectAsState()
    val connState by service.connectionState.collectAsState()
    val isConnected = connState == BleService.ConnectionState.CONNECTED

    // Request settings from firmware on entry
    LaunchedEffect(Unit) {
        if (isConnected) service.requestSettings()
    }

    // Also refresh if we reconnect while on this screen
    LaunchedEffect(isConnected) {
        if (isConnected && settings.isEmpty()) service.requestSettings()
    }

    val scrollState = rememberScrollState()

    // Editable fields
    var geminiKey by remember(settings["AK"]) { mutableStateOf(settings["AK"] ?: "") }
    var ttsKey by remember(settings["TK"]) { mutableStateOf(settings["TK"] ?: "") }
    var brightness by remember(settings["BR"]) { mutableStateOf(settings["BR"]?.toIntOrNull() ?: 2) }
    var model by remember(settings["MO"]) { mutableStateOf(settings["MO"]?.toIntOrNull() ?: 0) }
    var voice by remember(settings["VO"]) { mutableStateOf(settings["VO"]?.toIntOrNull() ?: 0) }
    var memoryEnabled by remember(settings["ME"]) { mutableStateOf(settings["ME"] != "0") }
    var wifiEnabled by remember(settings["WE"]) { mutableStateOf(settings["WE"] != "0") }
    var sleepTimeout by remember(settings["ST"]) { mutableStateOf(settings["ST"]?.toIntOrNull() ?: 4) }

    val brightnessNames = listOf("Low", "Medium", "High")
    val modelNames = listOf("Fast (Flash Lite)", "Smart (Flash)")
    val voiceNames = listOf("Odysseus", "Apollo", "Arcas", "Aries", "Atlas", "Hermes")
    val sleepNames = listOf("10 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes", "Never")

    var saveToast by remember { mutableStateOf(false) }
    LaunchedEffect(saveToast) {
        if (saveToast) {
            kotlinx.coroutines.delay(2000)
            saveToast = false
        }
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scrollState)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedButton(
                onClick = onBack,
                modifier = Modifier.height(40.dp),
                shape = RoundedCornerShape(20.dp),
                border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
            ) {
                Text("‹ Back", color = Color(0xFFAAAAAA), fontSize = 14.sp)
            }
        }

        Spacer(Modifier.height(20.dp))

        if (!isConnected) {
            Text(
                "Not connected to Jibo — settings will be sent when reconnected.",
                fontSize = 13.sp,
                color = Color(0xFFCC8844),
                lineHeight = 18.sp
            )
            Spacer(Modifier.height(16.dp))
        }

        if (settings.isEmpty() && isConnected) {
            Box(
                modifier = Modifier.fillMaxWidth().padding(top = 40.dp),
                contentAlignment = Alignment.Center
            ) {
                CircularProgressIndicator(color = Color.White, strokeWidth = 2.dp, modifier = Modifier.size(32.dp))
            }
        } else {
            // ── API Keys ──────────────────────────────────────────────
            SettingsSectionHeader("API Keys")
            Spacer(Modifier.height(12.dp))

            Text("Gemini API Key", fontSize = 13.sp, color = Color(0xFFCCCCCC), fontWeight = FontWeight.Medium)
            Spacer(Modifier.height(4.dp))
            OutlinedTextField(
                value = geminiKey,
                onValueChange = { geminiKey = it },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                colors = setupTextFieldColors(),
                textStyle = androidx.compose.ui.text.TextStyle(fontSize = 14.sp)
            )
            Spacer(Modifier.height(12.dp))

            Text("Deepgram TTS Key", fontSize = 13.sp, color = Color(0xFFCCCCCC), fontWeight = FontWeight.Medium)
            Spacer(Modifier.height(4.dp))
            OutlinedTextField(
                value = ttsKey,
                onValueChange = { ttsKey = it },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                colors = setupTextFieldColors(),
                textStyle = androidx.compose.ui.text.TextStyle(fontSize = 14.sp)
            )

            Spacer(Modifier.height(24.dp))

            // ── Voice & Model ─────────────────────────────────────────
            SettingsSectionHeader("Voice & Model")
            Spacer(Modifier.height(12.dp))

            SettingsSelector(
                label = "Model",
                options = modelNames,
                selected = model,
                onSelect = { model = it }
            )
            Spacer(Modifier.height(12.dp))

            SettingsSelector(
                label = "Voice",
                options = voiceNames,
                selected = voice,
                onSelect = { voice = it }
            )

            Spacer(Modifier.height(24.dp))

            // ── Display ───────────────────────────────────────────────
            SettingsSectionHeader("Display")
            Spacer(Modifier.height(12.dp))

            SettingsSelector(
                label = "Brightness",
                options = brightnessNames,
                selected = brightness,
                onSelect = { brightness = it }
            )

            Spacer(Modifier.height(24.dp))

            // ── Behavior ──────────────────────────────────────────────
            SettingsSectionHeader("Behavior")
            Spacer(Modifier.height(12.dp))

            SettingsToggle(
                label = "Conversation Memory",
                checked = memoryEnabled,
                onToggle = { memoryEnabled = it }
            )
            Spacer(Modifier.height(8.dp))

            SettingsToggle(
                label = "WiFi",
                checked = wifiEnabled,
                onToggle = { wifiEnabled = it }
            )
            Spacer(Modifier.height(12.dp))

            SettingsSelector(
                label = "Auto-Sleep",
                options = sleepNames,
                selected = sleepTimeout,
                onSelect = { sleepTimeout = it }
            )

            Spacer(Modifier.height(32.dp))

            // ── Save button ───────────────────────────────────────────
            Button(
                onClick = {
                    val pairs = mutableMapOf<String, String>()
                    if (geminiKey != (settings["AK"] ?: "")) pairs["AK"] = geminiKey
                    if (ttsKey != (settings["TK"] ?: "")) pairs["TK"] = ttsKey
                    if (brightness.toString() != (settings["BR"] ?: "")) pairs["BR"] = brightness.toString()
                    if (model.toString() != (settings["MO"] ?: "")) pairs["MO"] = model.toString()
                    if (voice.toString() != (settings["VO"] ?: "")) pairs["VO"] = voice.toString()
                    val meStr = if (memoryEnabled) "1" else "0"
                    if (meStr != (settings["ME"] ?: "")) pairs["ME"] = meStr
                    val weStr = if (wifiEnabled) "1" else "0"
                    if (weStr != (settings["WE"] ?: "")) pairs["WE"] = weStr
                    if (sleepTimeout.toString() != (settings["ST"] ?: "")) pairs["ST"] = sleepTimeout.toString()

                    if (pairs.isNotEmpty()) {
                        service.writeSettings(pairs)
                        saveToast = true
                    }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
                shape = RoundedCornerShape(26.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Color.White)
            ) {
                Text(
                    if (saveToast) "Saved!" else "Save Changes",
                    color = Color.Black,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 16.sp
                )
            }

            Spacer(Modifier.height(32.dp))
        }
    }
}

@Composable
private fun SettingsSectionHeader(title: String) {
    Text(
        title,
        fontSize = 16.sp,
        color = Color.White,
        fontWeight = FontWeight.SemiBold
    )
}

@Composable
private fun SettingsSelector(
    label: String,
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(label, fontSize = 13.sp, color = Color(0xFFCCCCCC), fontWeight = FontWeight.Medium)
        Spacer(Modifier.height(6.dp))

        if (options.size <= 4) {
            // Horizontal chips for small option sets
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                options.forEachIndexed { index, name ->
                    val isSelected = index == selected
                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(if (isSelected) Color(0xFF333333) else Color(0xFF1A1A1A))
                            .clickable { onSelect(index) }
                            .padding(vertical = 10.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            name,
                            fontSize = 13.sp,
                            color = if (isSelected) Color.White else Color(0xFF888888),
                            fontWeight = if (isSelected) FontWeight.Medium else FontWeight.Normal,
                            textAlign = TextAlign.Center,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }
            }
        } else {
            // Vertical list for large option sets
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(10.dp))
                    .background(Color(0xFF1A1A1A)),
                verticalArrangement = Arrangement.spacedBy(0.dp)
            ) {
                options.forEachIndexed { index, name ->
                    val isSelected = index == selected
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onSelect(index) }
                            .background(if (isSelected) Color(0xFF333333) else Color.Transparent)
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            name,
                            fontSize = 14.sp,
                            color = if (isSelected) Color.White else Color(0xFF888888),
                            fontWeight = if (isSelected) FontWeight.Medium else FontWeight.Normal,
                            modifier = Modifier.weight(1f)
                        )
                        if (isSelected) {
                            Text("✓", color = Color(0xFF4488CC), fontSize = 16.sp, fontWeight = FontWeight.Bold)
                        }
                    }
                    if (index < options.size - 1) {
                        HorizontalDivider(color = Color(0xFF252525))
                    }
                }
            }
        }
    }
}

@Composable
private fun SettingsToggle(
    label: String,
    checked: Boolean,
    onToggle: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(Color(0xFF1A1A1A))
            .clickable { onToggle(!checked) }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            label,
            fontSize = 14.sp,
            color = Color.White,
            modifier = Modifier.weight(1f)
        )
        Switch(
            checked = checked,
            onCheckedChange = onToggle,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = Color(0xFF4488CC),
                uncheckedThumbColor = Color(0xFF888888),
                uncheckedTrackColor = Color(0xFF333333)
            )
        )
    }
}

// ─── Setup Screen (first-time WiFi + API key wizard) ───────────────────────

@Composable
fun SetupScreen(service: BleService) {
    var step by remember { mutableStateOf(0) }  // 0=wifi, 1=keys, 2=sending

    // WiFi state
    var selectedSsid by remember { mutableStateOf("") }
    var wifiPassword by remember { mutableStateOf("") }
    var wifiUsername by remember { mutableStateOf("") }
    var isEnterprise by remember { mutableStateOf(false) }
    var showManualEntry by remember { mutableStateOf(false) }
    var manualSsid by remember { mutableStateOf("") }

    // API key state
    var geminiKey by remember { mutableStateOf("") }
    var ttsKey by remember { mutableStateOf("") }

    val wifiNetworks by service.wifiNetworks.collectAsState()
    val wifiScanning by service.wifiScanning.collectAsState()
    val setupStatus by service.setupStatus.collectAsState()

    // Start WiFi scan on entry
    LaunchedEffect(Unit) {
        service.scanWifiNetworks()
    }

    when (step) {
        0 -> WifiSetupStep(
            networks = wifiNetworks,
            scanning = wifiScanning,
            selectedSsid = selectedSsid,
            password = wifiPassword,
            username = wifiUsername,
            isEnterprise = isEnterprise,
            showManualEntry = showManualEntry,
            manualSsid = manualSsid,
            onSelectNetwork = { ssid, enterprise ->
                selectedSsid = ssid
                isEnterprise = enterprise
                wifiPassword = ""
                wifiUsername = ""
            },
            onPasswordChange = { wifiPassword = it },
            onUsernameChange = { wifiUsername = it },
            onToggleManual = { showManualEntry = !showManualEntry },
            onManualSsidChange = { manualSsid = it },
            onManualEnterprise = { isEnterprise = it },
            onRescan = { service.scanWifiNetworks() },
            onNext = {
                val ssid = if (showManualEntry) manualSsid else selectedSsid
                selectedSsid = ssid
                step = 1
            }
        )
        1 -> ApiKeysStep(
            geminiKey = geminiKey,
            ttsKey = ttsKey,
            onGeminiKeyChange = { geminiKey = it },
            onTtsKeyChange = { ttsKey = it },
            onBack = { step = 0 },
            onComplete = { step = 2 }
        )
        2 -> SetupProgressStep(
            service = service,
            ssid = selectedSsid,
            password = wifiPassword,
            username = wifiUsername,
            enterprise = isEnterprise,
            geminiKey = geminiKey,
            ttsKey = ttsKey,
            setupStatus = setupStatus,
            onRetry = {
                service.resetSetupState()
                step = 0
            }
        )
    }
}

// ─── Step 0: WiFi network picker ───────────────────────────────────────────

@Composable
private fun WifiSetupStep(
    networks: List<BleService.WifiNetwork>,
    scanning: Boolean,
    selectedSsid: String,
    password: String,
    username: String,
    isEnterprise: Boolean,
    showManualEntry: Boolean,
    manualSsid: String,
    onSelectNetwork: (String, Boolean) -> Unit,
    onPasswordChange: (String) -> Unit,
    onUsernameChange: (String) -> Unit,
    onToggleManual: () -> Unit,
    onManualSsidChange: (String) -> Unit,
    onManualEnterprise: (Boolean) -> Unit,
    onRescan: () -> Unit,
    onNext: () -> Unit
) {
    val scrollState = rememberScrollState()
    var passwordVisible by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scrollState)
    ) {
        Text(
            "Connect to WiFi",
            fontSize = 22.sp,
            color = Color.White,
            fontWeight = FontWeight.SemiBold
        )
        Spacer(Modifier.height(4.dp))
        Text(
            "Choose a network for Jibo",
            fontSize = 14.sp,
            color = Color(0xFF888888)
        )
        Spacer(Modifier.height(20.dp))

        if (!showManualEntry) {
            // Network list
            if (scanning && networks.isEmpty()) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 32.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        CircularProgressIndicator(
                            color = Color.White,
                            strokeWidth = 2.dp,
                            modifier = Modifier.size(24.dp)
                        )
                        Spacer(Modifier.height(12.dp))
                        Text(
                            "Scanning for networks...",
                            fontSize = 14.sp,
                            color = Color(0xFF888888)
                        )
                    }
                }
            } else {
                networks.forEach { network ->
                    val isSelected = selectedSsid == network.ssid
                    WifiNetworkRow(
                        network = network,
                        isSelected = isSelected,
                        onClick = { onSelectNetwork(network.ssid, network.isEnterprise) }
                    )
                    Spacer(Modifier.height(8.dp))
                }

                if (networks.isEmpty() && !scanning) {
                    Text(
                        "No networks found",
                        fontSize = 14.sp,
                        color = Color(0xFF888888),
                        modifier = Modifier.padding(vertical = 16.dp)
                    )
                }

                if (!scanning) {
                    Spacer(Modifier.height(4.dp))
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.Center
                    ) {
                        TextButton(onClick = onRescan) {
                            Text("Scan again", color = Color(0xFF3CA0FF), fontSize = 14.sp)
                        }
                    }
                }
            }

            // Password / username fields shown when a secured network is selected
            if (selectedSsid.isNotEmpty()) {
                val net = networks.find { it.ssid == selectedSsid }
                val secured = net?.isSecured ?: true

                if (secured || isEnterprise) {
                    Spacer(Modifier.height(12.dp))

                    if (isEnterprise) {
                        OutlinedTextField(
                            value = username,
                            onValueChange = onUsernameChange,
                            label = { Text("Username") },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth(),
                            colors = setupTextFieldColors()
                        )
                        Spacer(Modifier.height(12.dp))
                    }

                    OutlinedTextField(
                        value = password,
                        onValueChange = onPasswordChange,
                        label = { Text("Password") },
                        singleLine = true,
                        visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                        trailingIcon = {
                            TextButton(onClick = { passwordVisible = !passwordVisible }) {
                                Text(
                                    if (passwordVisible) "Hide" else "Show",
                                    color = Color(0xFF888888),
                                    fontSize = 12.sp
                                )
                            }
                        },
                        modifier = Modifier.fillMaxWidth(),
                        colors = setupTextFieldColors()
                    )
                }
            }
        } else {
            // Manual entry
            OutlinedTextField(
                value = manualSsid,
                onValueChange = onManualSsidChange,
                label = { Text("Network Name (SSID)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                colors = setupTextFieldColors()
            )

            Spacer(Modifier.height(12.dp))

            OutlinedTextField(
                value = password,
                onValueChange = onPasswordChange,
                label = { Text("Password") },
                singleLine = true,
                visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                trailingIcon = {
                    TextButton(onClick = { passwordVisible = !passwordVisible }) {
                        Text(
                            if (passwordVisible) "Hide" else "Show",
                            color = Color(0xFF888888),
                            fontSize = 12.sp
                        )
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                colors = setupTextFieldColors()
            )

            Spacer(Modifier.height(12.dp))

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color(0xFF1A1A1A))
                    .clickable { onManualEnterprise(!isEnterprise) }
                    .padding(horizontal = 16.dp, vertical = 14.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "Enterprise network (EAP)",
                    color = Color(0xFFCCCCCC),
                    fontSize = 14.sp,
                    modifier = Modifier.weight(1f)
                )
                Switch(
                    checked = isEnterprise,
                    onCheckedChange = onManualEnterprise,
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.White,
                        checkedTrackColor = Color(0xFF3CA0FF),
                        uncheckedThumbColor = Color(0xFF888888),
                        uncheckedTrackColor = Color(0xFF333333)
                    )
                )
            }

            if (isEnterprise) {
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = username,
                    onValueChange = onUsernameChange,
                    label = { Text("Username") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    colors = setupTextFieldColors()
                )
            }
        }

        Spacer(Modifier.height(16.dp))

        // Toggle manual / list entry
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Center
        ) {
            TextButton(onClick = onToggleManual) {
                Text(
                    if (showManualEntry) "Choose from list" else "Enter manually",
                    color = Color(0xFF3CA0FF),
                    fontSize = 14.sp
                )
            }
        }

        Spacer(Modifier.height(20.dp))

        // Next button
        val canProceed = if (showManualEntry) {
            manualSsid.isNotBlank()
        } else {
            selectedSsid.isNotEmpty()
        }

        Button(
            onClick = onNext,
            enabled = canProceed,
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
            shape = RoundedCornerShape(26.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = Color.White,
                disabledContainerColor = Color(0xFF333333)
            )
        ) {
            Text(
                "Next",
                color = if (canProceed) Color.Black else Color(0xFF666666),
                fontWeight = FontWeight.SemiBold,
                fontSize = 16.sp
            )
        }

        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun WifiNetworkRow(
    network: BleService.WifiNetwork,
    isSelected: Boolean,
    onClick: () -> Unit
) {
    val bars = wifiStrengthBars(network.rssi)
    val strengthText = when (bars) {
        3 -> "Strong"
        2 -> "Good"
        1 -> "Weak"
        else -> "Very weak"
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(if (isSelected) Color(0xFF1A2A3A) else Color(0xFF1A1A1A))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Signal bars indicator
        Row(
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(2.dp),
            modifier = Modifier.width(20.dp)
        ) {
            for (i in 0 until 3) {
                val barHeight = when (i) { 0 -> 6; 1 -> 10; else -> 14 }
                Box(
                    modifier = Modifier
                        .width(4.dp)
                        .height(barHeight.dp)
                        .clip(RoundedCornerShape(1.dp))
                        .background(
                            if (i < bars) Color(0xFF60C060) else Color(0xFF333333)
                        )
                )
            }
        }

        Spacer(Modifier.width(14.dp))

        Column(modifier = Modifier.weight(1f)) {
            Text(
                network.ssid,
                color = Color.White,
                fontSize = 15.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                "Signal: $strengthText",
                color = Color(0xFF888888),
                fontSize = 12.sp
            )
        }

        if (network.isEnterprise) {
            Text("EAP", color = Color(0xFF888888), fontSize = 11.sp)
            Spacer(Modifier.width(8.dp))
        }

        Text(
            if (network.isSecured || network.isEnterprise) "🔒" else "Open",
            fontSize = if (network.isSecured || network.isEnterprise) 14.sp else 12.sp,
            color = Color(0xFF888888)
        )

        if (isSelected) {
            Spacer(Modifier.width(8.dp))
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(Color(0xFF3CA0FF))
            )
        }
    }
}

private fun wifiStrengthBars(rssi: Int): Int = when {
    rssi >= -55 -> 3
    rssi >= -70 -> 2
    rssi >= -85 -> 1
    else -> 0
}

// ─── Step 1: API key entry ─────────────────────────────────────────────────

@Composable
private fun ApiKeysStep(
    geminiKey: String,
    ttsKey: String,
    onGeminiKeyChange: (String) -> Unit,
    onTtsKeyChange: (String) -> Unit,
    onBack: () -> Unit,
    onComplete: () -> Unit
) {
    val scrollState = rememberScrollState()

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scrollState)
    ) {
        Text(
            "API Keys",
            fontSize = 22.sp,
            color = Color.White,
            fontWeight = FontWeight.SemiBold
        )
        Spacer(Modifier.height(4.dp))
        Text(
            "Required for Jibo to work",
            fontSize = 14.sp,
            color = Color(0xFF888888)
        )
        Spacer(Modifier.height(24.dp))

        // Gemini key
        Text(
            "Gemini API Key",
            fontSize = 14.sp,
            color = Color(0xFFCCCCCC),
            fontWeight = FontWeight.Medium
        )
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(
            value = geminiKey,
            onValueChange = onGeminiKeyChange,
            label = { Text("Paste your key here") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            colors = setupTextFieldColors()
        )
        Spacer(Modifier.height(6.dp))
        Text(
            "Get one at ai.google.dev",
            fontSize = 12.sp,
            color = Color(0xFF3CA0FF)
        )

        Spacer(Modifier.height(28.dp))

        // TTS key
        Text(
            "Deepgram TTS Key",
            fontSize = 14.sp,
            color = Color(0xFFCCCCCC),
            fontWeight = FontWeight.Medium
        )
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(
            value = ttsKey,
            onValueChange = onTtsKeyChange,
            label = { Text("Paste your key here") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            colors = setupTextFieldColors()
        )
        Spacer(Modifier.height(6.dp))
        Text(
            "Get one at deepgram.com",
            fontSize = 12.sp,
            color = Color(0xFF3CA0FF)
        )

        Spacer(Modifier.height(36.dp))

        // Buttons
        val keysReady = geminiKey.isNotBlank() && ttsKey.isNotBlank()

        Button(
            onClick = onComplete,
            enabled = keysReady,
            modifier = Modifier
                .fillMaxWidth()
                .height(52.dp),
            shape = RoundedCornerShape(26.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = Color.White,
                disabledContainerColor = Color(0xFF333333)
            )
        ) {
            Text(
                "Complete Setup",
                color = if (keysReady) Color.Black else Color(0xFF666666),
                fontWeight = FontWeight.SemiBold,
                fontSize = 16.sp
            )
        }

        Spacer(Modifier.height(12.dp))

        OutlinedButton(
            onClick = onBack,
            modifier = Modifier
                .fillMaxWidth()
                .height(48.dp),
            shape = RoundedCornerShape(24.dp),
            border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF444444))
        ) {
            Text("Back", color = Color(0xFFAAAAAA), fontSize = 14.sp)
        }

        Spacer(Modifier.height(24.dp))
    }
}

// ─── Step 2: Sending config + progress ─────────────────────────────────────

@Composable
private fun SetupProgressStep(
    service: BleService,
    ssid: String,
    password: String,
    username: String,
    enterprise: Boolean,
    geminiKey: String,
    ttsKey: String,
    setupStatus: Int,
    onRetry: () -> Unit
) {
    // Send all data on entering this step
    LaunchedEffect(Unit) {
        service.sendSetupWifi(ssid, password, username, enterprise)
        kotlinx.coroutines.delay(200)
        service.sendSetupKeys(geminiKey, ttsKey)
        kotlinx.coroutines.delay(200)
        service.sendSetupComplete()
    }

    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(48.dp))

        when (setupStatus) {
            -1, 0 -> {
                // Sending / connecting
                CircularProgressIndicator(
                    color = Color.White,
                    strokeWidth = 2.dp,
                    modifier = Modifier.size(48.dp)
                )
                Spacer(Modifier.height(24.dp))
                Text(
                    if (setupStatus == 0) "Connecting to WiFi..." else "Sending configuration...",
                    fontSize = 16.sp,
                    color = Color(0xFFCCCCCC)
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    ssid,
                    fontSize = 14.sp,
                    color = Color(0xFF888888)
                )
            }
            1 -> {
                // WiFi connected
                Box(
                    modifier = Modifier
                        .size(64.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF1A3A20)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("✓", color = Color(0xFF60C060), fontSize = 32.sp, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(24.dp))
                Text(
                    "WiFi connected!",
                    fontSize = 18.sp,
                    color = Color(0xFF60C060),
                    fontWeight = FontWeight.Medium
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "Waiting for setup to finish...",
                    fontSize = 14.sp,
                    color = Color(0xFF888888)
                )
            }
            2 -> {
                // Auth fail
                Box(
                    modifier = Modifier
                        .size(64.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF3A1A1A)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("✗", color = Color(0xFFFF6060), fontSize = 32.sp, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(24.dp))
                Text(
                    "Wrong WiFi password",
                    fontSize = 18.sp,
                    color = Color(0xFFFF6060),
                    fontWeight = FontWeight.Medium
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "Check your password and try again",
                    fontSize = 14.sp,
                    color = Color(0xFF888888)
                )
                Spacer(Modifier.height(32.dp))
                SetupRetryButton(onRetry)
            }
            3 -> {
                // Not found
                Box(
                    modifier = Modifier
                        .size(64.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF3A2A1A)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("?", color = Color(0xFFCC8844), fontSize = 32.sp, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(24.dp))
                Text(
                    "Network not found",
                    fontSize = 18.sp,
                    color = Color(0xFFCC8844),
                    fontWeight = FontWeight.Medium
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "\"$ssid\" wasn't found nearby.\nMake sure the router is on and in range.",
                    fontSize = 14.sp,
                    color = Color(0xFF888888),
                    textAlign = TextAlign.Center,
                    lineHeight = 20.sp
                )
                Spacer(Modifier.height(32.dp))
                SetupRetryButton(onRetry)
            }
            4 -> {
                // Timeout
                Box(
                    modifier = Modifier
                        .size(64.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF3A2A1A)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("⏱", color = Color(0xFFCC8844), fontSize = 28.sp)
                }
                Spacer(Modifier.height(24.dp))
                Text(
                    "Connection timed out",
                    fontSize = 18.sp,
                    color = Color(0xFFCC8844),
                    fontWeight = FontWeight.Medium
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "Jibo couldn't connect in time.\nTry again or pick a different network.",
                    fontSize = 14.sp,
                    color = Color(0xFF888888),
                    textAlign = TextAlign.Center,
                    lineHeight = 20.sp
                )
                Spacer(Modifier.height(32.dp))
                SetupRetryButton(onRetry)
            }
            5 -> {
                // All done
                Box(
                    modifier = Modifier
                        .size(80.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF1A3A20)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("✓", color = Color(0xFF60C060), fontSize = 42.sp, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(24.dp))
                Text(
                    "Setup complete!",
                    fontSize = 22.sp,
                    color = Color(0xFF60C060),
                    fontWeight = FontWeight.SemiBold
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "Jibo is ready to go",
                    fontSize = 14.sp,
                    color = Color(0xFF888888)
                )
            }
            else -> {
                // Unknown status
                CircularProgressIndicator(
                    color = Color.White,
                    strokeWidth = 2.dp,
                    modifier = Modifier.size(48.dp)
                )
                Spacer(Modifier.height(24.dp))
                Text(
                    "Setting up...",
                    fontSize = 16.sp,
                    color = Color(0xFFCCCCCC)
                )
            }
        }
    }
}

@Composable
private fun SetupRetryButton(onRetry: () -> Unit) {
    Button(
        onClick = onRetry,
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp),
        shape = RoundedCornerShape(26.dp),
        colors = ButtonDefaults.buttonColors(containerColor = Color.White)
    ) {
        Text("Try Again", color = Color.Black, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
    }
}

@Composable
private fun setupTextFieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = Color.White,
    unfocusedTextColor = Color.White,
    focusedBorderColor = Color.White,
    unfocusedBorderColor = Color(0xFF444444),
    focusedLabelColor = Color(0xFF888888),
    unfocusedLabelColor = Color(0xFF888888),
    cursorColor = Color.White
)

// ─── Debug Screen (proxy debug overlay) ─────────────────────────────────────

@Composable
fun DebugScreen(
    log: List<String>,
    connState: BleService.ConnectionState
) {
    val listState = rememberLazyListState()

    LaunchedEffect(log.size) {
        if (log.isNotEmpty()) {
            listState.animateScrollToItem(log.size - 1)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF0A0F0A))
            .statusBarsPadding()
            .padding(horizontal = 12.dp)
    ) {
        Spacer(Modifier.height(16.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "PROXY DEBUG",
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                color = Color(0xFF00FF88),
                letterSpacing = 2.sp
            )
            Spacer(Modifier.weight(1f))
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(
                        if (connState == BleService.ConnectionState.CONNECTED)
                            Color(0xFF00FF88) else Color(0xFFFF4444)
                    )
            )
            Spacer(Modifier.width(6.dp))
            Text(
                if (connState == BleService.ConnectionState.CONNECTED) "LINKED"
                else "DISCONNECTED",
                fontSize = 11.sp,
                color = if (connState == BleService.ConnectionState.CONNECTED)
                    Color(0xFF00FF88) else Color(0xFFFF4444),
                fontWeight = FontWeight.Medium
            )
        }

        Spacer(Modifier.height(4.dp))

        Text(
            "Send \"debugexit\" to Jibo serial to close",
            fontSize = 11.sp,
            color = Color(0xFF446644)
        )

        Spacer(Modifier.height(8.dp))

        HorizontalDivider(color = Color(0xFF1A2A1A))

        LazyColumn(
            state = listState,
            modifier = Modifier
                .fillMaxSize()
                .padding(top = 4.dp),
            verticalArrangement = Arrangement.spacedBy(1.dp)
        ) {
            items(log) { line ->
                val color = when {
                    "FAILED" in line || "ERROR" in line -> Color(0xFFFF6666)
                    "──" in line -> Color(0xFF00FF88)
                    "OK" in line || "success" in line -> Color(0xFF88DD88)
                    line.contains("Speaker playback") -> Color(0xFF88BBFF)
                    else -> Color(0xFF99AA99)
                }
                Text(
                    text = line,
                    fontSize = 11.sp,
                    color = color,
                    fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace,
                    lineHeight = 14.sp
                )
            }

            if (log.isEmpty()) {
                item {
                    Spacer(Modifier.height(40.dp))
                    Text(
                        "Waiting for requests...\nSpeak to Jibo or send a geminitest command.",
                        fontSize = 13.sp,
                        color = Color(0xFF446644),
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth(),
                        lineHeight = 20.sp
                    )
                }
            }
        }
    }
}
