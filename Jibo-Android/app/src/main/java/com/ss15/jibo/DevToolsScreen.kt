package com.ss15.jibo

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ─────────────────────────────────────────────────────────────────────────────
//  Developer Tools Screen
//
//  Hidden behind a 10-tap easter egg on the J icon.  Provides:
//    • A serial-monitor-style terminal that mirrors Jibo's USB log output
//      in real time and lets the user send any command the firmware's
//      built-in parser supports (the "help" command shows the list).
//    • A live dashboard with heap/PSRAM/uptime/Wi-Fi/RSSI plus a per-task
//      breakdown (priority, state, stack high-water mark, core).
//
//  All transport happens over the existing BLE link via dev_console.cpp
//  on the firmware side.  The firmware only spends bandwidth forwarding
//  data while OP_DEV_ENABLE is set — flipping back home turns the stream
//  off again so day-to-day usage stays unaffected.
// ─────────────────────────────────────────────────────────────────────────────

@Composable
fun DevToolsScreen(
    service: BleService,
    onBack: () -> Unit
) {
    var tab by remember { mutableStateOf(DevTab.CONSOLE) }
    val connState by service.connectionState.collectAsState()

    // Forwarding is already on by virtue of the user enabling
    // developer mode (the toggle persists across launches), so we don't
    // re-enable on entry.  We *do* leave it on when leaving this screen
    // — the user explicitly opted in via 10 taps and can disable the
    // whole feature with another 10 taps on the J icon.

    Column(modifier = Modifier.fillMaxSize()) {
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
            ConnDot(
                connected = connState == BleService.ConnectionState.CONNECTED
            )
        }

        Spacer(Modifier.height(12.dp))

        // Tab selector
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF111111))
                .padding(4.dp)
        ) {
            DevTab.values().forEach { t ->
                val selected = t == tab
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(36.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(if (selected) Color(0xFF1F2D1F) else Color.Transparent)
                        .clickable { tab = t },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        t.label,
                        color = if (selected) Color(0xFF60FF80) else Color(0xFF888888),
                        fontSize = 13.sp,
                        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal
                    )
                }
            }
        }

        Spacer(Modifier.height(12.dp))

        when (tab) {
            DevTab.CONSOLE   -> DevConsolePane(service)
            DevTab.DASHBOARD -> DevDashboardPane(service)
        }
    }
}

private enum class DevTab(val label: String) {
    CONSOLE("Terminal"),
    DASHBOARD("Dashboard")
}

@Composable
private fun ConnDot(connected: Boolean) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(8.dp)
                .clip(CircleShape)
                .background(if (connected) Color(0xFF60FF80) else Color(0xFFFF6060))
        )
        Spacer(Modifier.width(6.dp))
        Text(
            if (connected) "LINKED" else "OFFLINE",
            color = if (connected) Color(0xFF60FF80) else Color(0xFFFF6060),
            fontSize = 11.sp,
            fontWeight = FontWeight.Medium
        )
    }
}

// ─── Console (terminal) ────────────────────────────────────────────────────

@Composable
private fun DevConsolePane(service: BleService) {
    val log by service.devLog.collectAsState()
    val connected = service.connectionState.collectAsState().value ==
            BleService.ConnectionState.CONNECTED

    var input by remember { mutableStateOf("") }
    val scrollState = rememberScrollState()
    var autoscroll by remember { mutableStateOf(true) }

    // Auto-scroll to bottom whenever the log grows, but only when the
    // user hasn't manually scrolled up to read history.  Detection is
    // approximate (we just check if the user was already near the
    // bottom before the update) but in practice it feels right.
    LaunchedEffect(log) {
        if (autoscroll) scrollState.scrollTo(scrollState.maxValue)
    }
    LaunchedEffect(scrollState.value, scrollState.maxValue) {
        autoscroll = (scrollState.maxValue - scrollState.value) < 80
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Toolbar — clear + autoscroll indicator
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "Serial Monitor",
                color = Color(0xFFAAAAAA),
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium
            )
            Spacer(Modifier.weight(1f))
            if (!autoscroll) {
                TextButton(onClick = { autoscroll = true }) {
                    Text("Jump to bottom", color = Color(0xFF60C0FF), fontSize = 12.sp)
                }
            }
            TextButton(onClick = { service.clearDevLog() }) {
                Text("Clear", color = Color(0xFFCC8866), fontSize = 12.sp)
            }
        }

        // Log view — fixed-height scrollable monospace block.
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF050505))
                .border(1.dp, Color(0xFF1A1A1A), RoundedCornerShape(8.dp))
                .padding(8.dp)
        ) {
            Column(modifier = Modifier.verticalScroll(scrollState)) {
                if (log.isEmpty()) {
                    Text(
                        if (connected)
                            "Connected. Waiting for log output...\n\n" +
                            "Try typing 'help' below to see available commands."
                        else
                            "Not connected. Logs will appear here once Jibo is\n" +
                            "back in range and the BLE link is up.",
                        color = Color(0xFF555555),
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                } else {
                    Text(
                        text = colorizeLog(log),
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        lineHeight = 14.sp,
                        color = Color(0xFFCCCCCC)
                    )
                }
            }
        }

        Spacer(Modifier.height(8.dp))

        // Command input row.  IME action SEND fires the command without
        // closing the keyboard so the user can rapidly issue several in
        // a row, just like Arduino IDE's serial monitor.
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .height(44.dp)
                    .clip(RoundedCornerShape(10.dp))
                    .background(Color(0xFF111111))
                    .border(1.dp, Color(0xFF222222), RoundedCornerShape(10.dp))
                    .padding(horizontal = 12.dp),
                contentAlignment = Alignment.CenterStart
            ) {
                BasicTextField(
                    value = input,
                    onValueChange = { input = it },
                    singleLine = true,
                    textStyle = TextStyle(
                        color = Color(0xFFEEEEEE),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 13.sp
                    ),
                    cursorBrush = androidx.compose.ui.graphics.SolidColor(Color(0xFF60FF80)),
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.None,
                        autoCorrect = false,
                        imeAction = ImeAction.Send
                    ),
                    keyboardActions = KeyboardActions(
                        onSend = {
                            if (input.isNotBlank()) {
                                service.sendDevCommand(input)
                                input = ""
                                autoscroll = true
                            }
                        }
                    ),
                    decorationBox = { inner ->
                        if (input.isEmpty()) {
                            Text(
                                "Type a command (try 'help')",
                                color = Color(0xFF555555),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 13.sp
                            )
                        }
                        inner()
                    },
                    modifier = Modifier.fillMaxWidth()
                )
            }

            Spacer(Modifier.width(8.dp))

            Button(
                onClick = {
                    if (input.isNotBlank()) {
                        service.sendDevCommand(input)
                        input = ""
                        autoscroll = true
                    }
                },
                modifier = Modifier.height(44.dp),
                shape = RoundedCornerShape(10.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF1F2D1F),
                    disabledContainerColor = Color(0xFF1F1F1F)
                ),
                enabled = connected && input.isNotBlank()
            ) {
                Text(
                    "Send",
                    color = if (connected && input.isNotBlank())
                        Color(0xFF60FF80) else Color(0xFF555555),
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 13.sp
                )
            }
        }

        // Quick-pick command chips for the commands the user runs most
        // often.  Saves typing on a phone keyboard.
        Spacer(Modifier.height(6.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            for (q in listOf("help", "tasks", "memory", "notifdebug")) {
                ChipButton(label = q, enabled = connected) {
                    service.sendDevCommand(q)
                    autoscroll = true
                }
            }
        }
    }
}

@Composable
private fun ChipButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(14.dp))
            .background(if (enabled) Color(0xFF1A1A1A) else Color(0xFF101010))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 6.dp)
    ) {
        Text(
            label,
            color = if (enabled) Color(0xFF99CC99) else Color(0xFF444444),
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace
        )
    }
}

// Lightweight syntax-coloring of the log: highlight the local "> cmd"
// echo, error keywords, and our own bracketed tags ([ble], [audio], …).
private fun colorizeLog(text: String) = buildAnnotatedString {
    val lines = text.split('\n')
    for ((i, raw) in lines.withIndex()) {
        val color: Color = when {
            raw.startsWith("> ") -> Color(0xFF60C0FF)
            raw.contains("ERROR", ignoreCase = false) ||
            raw.contains("FAILED", ignoreCase = false) ||
            raw.contains("PANIC",  ignoreCase = false) -> Color(0xFFFF6060)
            raw.startsWith("[devcon]") -> Color(0xFF888888)
            raw.startsWith("[")        -> Color(0xFF99AA99)
            else -> Color(0xFFCCCCCC)
        }
        withStyle(SpanStyle(color = color)) { append(raw) }
        if (i < lines.lastIndex) append("\n")
    }
}

// ─── Dashboard ─────────────────────────────────────────────────────────────

// One graphable metric.  `extract` pulls a numeric Y from a stats sample
// (returns null if the sample didn't carry it — e.g. cpu0 = -1 on the
// first packet after dev-mode toggle), `format` renders the latest
// reading for the card face.  Keeping these out of the data class so
// the enum entries can also serve as the click identity.
private enum class DevMetric(
    val title: String,
    val accent: Color,
    val unit: String,
    val format: (Float) -> String,
    val extract: (BleService.DevStats) -> Float?,
    val sub: ((BleService.DevStats) -> String)? = null,
    // y-axis floor / ceiling.  null = autoscale.  Used so e.g. CPU
    // graphs always sit on the 0-100 grid even when the metric has
    // been 30-40 % the whole window.
    val yMin: Float? = null,
    val yMax: Float? = null
) {
    CPU0("CPU 0",       Color(0xFFFF80A0), "%",
        { f -> "${f.toInt()}%" },
        { s -> if (s.cpu0 < 0) null else s.cpu0.toFloat() },
        yMin = 0f, yMax = 100f),
    CPU1("CPU 1",       Color(0xFFFFB060), "%",
        { f -> "${f.toInt()}%" },
        { s -> if (s.cpu1 < 0) null else s.cpu1.toFloat() },
        yMin = 0f, yMax = 100f),
    DRAM_FREE("DRAM free", Color(0xFF60FF80), "KB",
        { f -> "${(f / 1024).toInt()} KB" },
        { s -> s.heapFree.toFloat() },
        sub = { s -> "min ${s.heapMin / 1024} KB · big ${s.heapBig / 1024} KB" },
        yMin = 0f),
    PSRAM_FREE("PSRAM free", Color(0xFFFFCC60), "KB",
        { f -> "${(f / 1024).toInt()} KB" },
        { s -> s.psramFree.toFloat() },
        sub = { s -> "big ${s.psramBig / 1024} KB" },
        yMin = 0f),
    RSSI("Wi-Fi RSSI",  Color(0xFF60C0FF), "dBm",
        { f -> "${f.toInt()} dBm" },
        { s -> if (s.wifiConnected) s.rssi.toFloat() else null }),
    HEAP_BIG("DRAM big block", Color(0xFF99CC99), "KB",
        { f -> "${(f / 1024).toInt()} KB" },
        { s -> s.heapBig.toFloat() },
        yMin = 0f),
}

@Composable
private fun DevDashboardPane(service: BleService) {
    val stats by service.devStats.collectAsState()
    val history by service.devStatsHistory.collectAsState()
    val connected = service.connectionState.collectAsState().value ==
            BleService.ConnectionState.CONNECTED

    var selected by remember { mutableStateOf<DevMetric?>(null) }

    val s = stats
    if (s == null) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                CircularProgressIndicator(
                    modifier = Modifier.size(28.dp),
                    color = Color(0xFF60FF80),
                    strokeWidth = 2.dp
                )
                Spacer(Modifier.height(12.dp))
                Text(
                    if (connected) "Waiting for first stats packet..."
                    else "Offline. Stats appear once Jibo is connected.",
                    color = Color(0xFF888888),
                    fontSize = 13.sp,
                    textAlign = TextAlign.Center
                )
            }
        }
        return
    }

    // Drill-down view: full-pane sparkline for one metric.
    val sel = selected
    if (sel != null) {
        DevMetricGraph(
            metric  = sel,
            history = history,
            onBack  = { selected = null }
        )
        return
    }

    val staleness = (System.currentTimeMillis() - s.receivedAtMs)
    val stale = staleness > 4000

    LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        // Uptime / Wi-Fi summary row.  Uptime isn't a useful graph so
        // it's the only un-clickable card; Wi-Fi opens the RSSI graph.
        item {
            Row(modifier = Modifier.fillMaxWidth()) {
                StatCard(
                    title = "Uptime",
                    value = formatUptime(s.uptimeMs),
                    accent = Color(0xFF60C0FF),
                    modifier = Modifier.weight(1f)
                )
                Spacer(Modifier.width(8.dp))
                StatCard(
                    title = "Wi-Fi",
                    value = if (s.wifiConnected) "${s.rssi} dBm" else "off",
                    accent = if (s.wifiConnected) Color(0xFF60FF80) else Color(0xFF888888),
                    modifier = Modifier.weight(1f),
                    onClick = if (s.wifiConnected) ({ selected = DevMetric.RSSI }) else null
                )
            }
        }
        // CPU per core
        item {
            Row(modifier = Modifier.fillMaxWidth()) {
                StatCard(
                    title = DevMetric.CPU0.title,
                    value = if (s.cpu0 < 0) "—" else "${s.cpu0}%",
                    sub   = "core 0",
                    accent = DevMetric.CPU0.accent,
                    modifier = Modifier.weight(1f),
                    onClick = { selected = DevMetric.CPU0 }
                )
                Spacer(Modifier.width(8.dp))
                StatCard(
                    title = DevMetric.CPU1.title,
                    value = if (s.cpu1 < 0) "—" else "${s.cpu1}%",
                    sub   = "core 1",
                    accent = DevMetric.CPU1.accent,
                    modifier = Modifier.weight(1f),
                    onClick = { selected = DevMetric.CPU1 }
                )
            }
        }
        item {
            Row(modifier = Modifier.fillMaxWidth()) {
                StatCard(
                    title = "DRAM free",
                    value = "${s.heapFree / 1024} KB",
                    sub = "min ${s.heapMin / 1024} KB · big ${s.heapBig / 1024} KB",
                    accent = heapColor(s.heapFree),
                    modifier = Modifier.weight(1f),
                    onClick = { selected = DevMetric.DRAM_FREE }
                )
                Spacer(Modifier.width(8.dp))
                StatCard(
                    title = "PSRAM free",
                    value = "${s.psramFree / 1024} KB",
                    sub = "big ${s.psramBig / 1024} KB",
                    accent = Color(0xFFFFCC60),
                    modifier = Modifier.weight(1f),
                    onClick = { selected = DevMetric.PSRAM_FREE }
                )
            }
        }
        item {
            Row(
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "TASKS (${s.tasks.size})",
                    color = Color(0xFF888888),
                    fontSize = 11.sp,
                    letterSpacing = 1.sp,
                    fontWeight = FontWeight.Medium
                )
                Spacer(Modifier.weight(1f))
                if (stale) {
                    Text(
                        "stale ${staleness / 1000}s",
                        color = Color(0xFFCC8844),
                        fontSize = 11.sp
                    )
                }
            }
        }
        item {
            // Header row — added a CPU% column.
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 4.dp, vertical = 4.dp)
            ) {
                Text("name", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.weight(1.6f))
                Text("cpu", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.width(34.dp))
                Text("pri", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.width(24.dp))
                Text("st", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.width(20.dp))
                Text("hwm", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.width(56.dp))
                Text("core", color = Color(0xFF666666), fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace, modifier = Modifier.width(32.dp))
            }
        }
        items(s.tasks) { t -> TaskRow(t) }
        item { Spacer(Modifier.height(20.dp)) }
    }
}

@Composable
private fun StatCard(
    title: String,
    value: String,
    sub: String? = null,
    accent: Color,
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null
) {
    val base = modifier
        .clip(RoundedCornerShape(12.dp))
        .background(Color(0xFF101010))
        .border(1.dp, Color(0xFF202020), RoundedCornerShape(12.dp))
    val withClick = if (onClick != null) base.clickable(onClick = onClick) else base
    Column(
        modifier = withClick.padding(12.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, color = Color(0xFF888888), fontSize = 11.sp,
                letterSpacing = 1.sp, fontWeight = FontWeight.Medium)
            if (onClick != null) {
                Spacer(Modifier.weight(1f))
                Text("›", color = Color(0xFF555555), fontSize = 14.sp)
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(value, color = accent, fontSize = 18.sp,
            fontWeight = FontWeight.SemiBold, fontFamily = FontFamily.Monospace)
        if (sub != null) {
            Spacer(Modifier.height(2.dp))
            Text(sub, color = Color(0xFF666666), fontSize = 10.sp,
                fontFamily = FontFamily.Monospace)
        }
    }
}

// ─── Metric drill-down (sparkline graph) ──────────────────────────────────

@Composable
private fun DevMetricGraph(
    metric: DevMetric,
    history: List<BleService.DevStats>,
    onBack: () -> Unit
) {
    // Project the history into a list of (sampleAtMs, value) pairs.
    // Drop nulls (samples where the metric wasn't reported, e.g. cpu0 = -1
    // on the first sample after toggle, or RSSI while Wi-Fi was off).
    val series: List<Pair<Long, Float>> = remember(history, metric) {
        history.mapNotNull { s ->
            val v = metric.extract(s) ?: return@mapNotNull null
            s.receivedAtMs to v
        }
    }

    val latest = series.lastOrNull()?.second
    val sub = history.lastOrNull()?.let { metric.sub?.invoke(it) }

    Column(modifier = Modifier.fillMaxSize()) {
        // Header
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(
                onClick = onBack,
                modifier = Modifier.height(36.dp),
                shape = RoundedCornerShape(18.dp),
                border = androidx.compose.foundation.BorderStroke(1.dp, Color(0xFF333333))
            ) {
                Text("‹ Dashboard", color = Color(0xFFAAAAAA), fontSize = 12.sp)
            }
            Spacer(Modifier.weight(1f))
            Text(
                metric.title,
                color = Color(0xFFCCCCCC),
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold
            )
        }

        Spacer(Modifier.height(12.dp))

        // Big value + sub
        Text(
            latest?.let { metric.format(it) } ?: "—",
            color = metric.accent,
            fontSize = 36.sp,
            fontWeight = FontWeight.SemiBold,
            fontFamily = FontFamily.Monospace
        )
        if (sub != null) {
            Spacer(Modifier.height(2.dp))
            Text(sub, color = Color(0xFF666666), fontSize = 12.sp,
                fontFamily = FontFamily.Monospace)
        }

        Spacer(Modifier.height(16.dp))

        // Sparkline
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(220.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(Color(0xFF080808))
                .border(1.dp, Color(0xFF1F1F1F), RoundedCornerShape(12.dp))
                .padding(12.dp)
        ) {
            if (series.size < 2) {
                Text(
                    "Need at least two samples to draw a graph.\n" +
                    "Wait a couple seconds...",
                    color = Color(0xFF555555),
                    fontSize = 12.sp,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center)
                )
            } else {
                MetricSparkline(
                    series  = series,
                    accent  = metric.accent,
                    yMinFix = metric.yMin,
                    yMaxFix = metric.yMax,
                    formatY = metric.format,
                    modifier = Modifier.fillMaxSize()
                )
            }
        }

        // Footer stats: min/avg/max over the visible window.
        if (series.size >= 2) {
            Spacer(Modifier.height(10.dp))
            val ys = series.map { it.second }
            val minV = ys.min()
            val maxV = ys.max()
            val avgV = ys.sum() / ys.size
            val span = (series.last().first - series.first().first) / 1000
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                MiniStat("min", metric.format(minV), Modifier.weight(1f))
                MiniStat("avg", metric.format(avgV), Modifier.weight(1f))
                MiniStat("max", metric.format(maxV), Modifier.weight(1f))
                MiniStat("span", "${span}s", Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun MiniStat(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFF0E0E0E))
            .border(1.dp, Color(0xFF181818), RoundedCornerShape(8.dp))
            .padding(8.dp)
    ) {
        Text(label, color = Color(0xFF666666), fontSize = 9.sp,
            letterSpacing = 1.sp, fontWeight = FontWeight.Medium)
        Spacer(Modifier.height(2.dp))
        Text(value, color = Color(0xFFCCCCCC), fontSize = 12.sp,
            fontFamily = FontFamily.Monospace)
    }
}

@Composable
private fun MetricSparkline(
    series: List<Pair<Long, Float>>,
    accent: Color,
    yMinFix: Float?,
    yMaxFix: Float?,
    formatY: (Float) -> String,
    modifier: Modifier = Modifier
) {
    val ys = series.map { it.second }
    val rawMin = ys.min()
    val rawMax = ys.max()

    // Bake in a tiny visual margin so a flat line doesn't sit on the
    // axis edge.  When yMinFix / yMaxFix are set we honour them
    // exactly so the y-axis is comparable across sessions.
    val span = (rawMax - rawMin).let { if (it < 1e-3f) 1f else it }
    val yMin = yMinFix ?: (rawMin - 0.05f * span)
    val yMax = yMaxFix ?: (rawMax + 0.05f * span)
    val yRange = (yMax - yMin).let { if (it < 1e-3f) 1f else it }

    androidx.compose.foundation.Canvas(modifier = modifier) {
        val w = size.width
        val h = size.height

        // Grid lines (4 horizontal divisions).
        val gridPaint = Color(0xFF1A1A1A)
        for (i in 0..4) {
            val y = h * i / 4f
            drawLine(
                color = gridPaint,
                start = Offset(0f, y),
                end   = Offset(w, y),
                strokeWidth = 1f
            )
        }

        // Build the polyline.  X is interpolated linearly across the
        // x-axis (we don't bother with timestamps — for a 1 Hz feed
        // the irregularity is invisible at this scale).
        val path = Path()
        val n = series.size
        series.forEachIndexed { i, (_, y) ->
            val px = w * i / (n - 1).coerceAtLeast(1).toFloat()
            val py = h - ((y - yMin) / yRange).coerceIn(0f, 1f) * h
            if (i == 0) path.moveTo(px, py) else path.lineTo(px, py)
        }

        // Accent fill below the line — translucent.
        val fillPath = Path().apply {
            addPath(path)
            lineTo(w, h)
            lineTo(0f, h)
            close()
        }
        drawPath(fillPath, color = accent.copy(alpha = 0.15f))
        drawPath(
            path,
            color = accent,
            style = Stroke(width = 2.5f)
        )

        // Latest-value dot.
        val lastY = ys.last()
        val lastPx = w
        val lastPy = h - ((lastY - yMin) / yRange).coerceIn(0f, 1f) * h
        drawCircle(
            color = accent,
            radius = 4f,
            center = Offset(lastPx - 2f, lastPy)
        )
    }
    // Y-axis labels overlay
    Box(modifier = modifier) {
        Column(
            modifier = Modifier.fillMaxHeight(),
            verticalArrangement = Arrangement.SpaceBetween
        ) {
            Text(formatY(yMax), color = Color(0xFF555555),
                fontSize = 9.sp, fontFamily = FontFamily.Monospace)
            Text(formatY(yMin), color = Color(0xFF555555),
                fontSize = 9.sp, fontFamily = FontFamily.Monospace)
        }
    }
}

@Composable
private fun TaskRow(t: BleService.DevTask) {
    val stkColor = when {
        t.stackHwm * 4 < 256  -> Color(0xFFFF6060)
        t.stackHwm * 4 < 1024 -> Color(0xFFCC8844)
        else                  -> Color(0xFF99CC99)
    }
    val stateColor = when (t.state) {
        "R" -> Color(0xFF60FF80)
        "Y" -> Color(0xFFCCDD60)
        "B" -> Color(0xFF60C0FF)
        "S" -> Color(0xFF888888)
        "D" -> Color(0xFFFF6060)
        else -> Color(0xFF888888)
    }
    val cpuColor = when {
        t.cpu >= 50 -> Color(0xFFFF8060)
        t.cpu >= 15 -> Color(0xFFFFCC60)
        t.cpu > 0   -> Color(0xFF99CC99)
        else        -> Color(0xFF555555)
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(6.dp))
            .background(Color(0xFF0C0C0C))
            .padding(horizontal = 4.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            t.name,
            color = Color(0xFFCCCCCC),
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.weight(1.6f),
            maxLines = 1
        )
        Text(
            "${t.cpu}%",
            color = cpuColor,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(34.dp)
        )
        Text(
            "${t.priority}",
            color = Color(0xFFAAAAAA),
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(24.dp)
        )
        Text(
            t.state,
            color = stateColor,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.width(20.dp)
        )
        // hwm is in stack words (4B) per FreeRTOS — show free bytes.
        Text(
            "${t.stackHwm * 4}B",
            color = stkColor,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(56.dp)
        )
        Text(
            if (t.core < 0) "any" else "${t.core}",
            color = Color(0xFF888888),
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(32.dp)
        )
    }
}

private fun heapColor(bytes: Int): Color = when {
    bytes < 16 * 1024  -> Color(0xFFFF6060)
    bytes < 48 * 1024  -> Color(0xFFCC8844)
    else               -> Color(0xFF60FF80)
}

private fun formatUptime(ms: Long): String {
    if (ms <= 0) return "0s"
    val s = ms / 1000
    val h = s / 3600
    val m = (s % 3600) / 60
    val sec = s % 60
    return when {
        h > 0 -> "${h}h ${m}m"
        m > 0 -> "${m}m ${sec}s"
        else  -> "${sec}s"
    }
}
