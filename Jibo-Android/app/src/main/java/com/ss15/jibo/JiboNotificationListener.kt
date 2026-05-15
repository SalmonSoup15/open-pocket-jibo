package com.ss15.jibo

import android.app.Notification
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.os.Parcelable
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Intercepts system notifications and forwards them to BleService so they can
 * be mirrored over BLE to Jibo and surfaced in Gemini's system prompt.
 *
 * Category here is purely descriptive ("messaging", "email", ...) so the
 * AI knows the rough kind of notification it's looking at.  Sensitivity and
 * urgency are NOT decided here -- Gemini judges those from the actual content
 * via system-prompt rules so e.g. a casual text from a friend isn't treated
 * the same as a 2FA code or a private message from a partner.
 */
class JiboNotificationListener : NotificationListenerService() {

    companion object {
        private const val TAG = "NotifListener"

        // Coarse package → category hint.  Used only for the descriptive label
        // shown to Gemini ("[messaging] WhatsApp from Dad: ...") -- never
        // for auto-tagging sensitivity or urgency.
        private val BANKING = setOf(
            "com.chase.sig.android", "com.bofa.android.app", "com.wf.wellsfargomobile",
            "com.citi.citimobile", "com.usaa.mobile.android.usaa",
            "com.paypal.android.p2pmobile", "com.venmo", "com.squareup.cash",
            "com.discover.mobile.android", "com.americanexpress.android.acctsvcs.us",
            "com.zellepay.zelle"
        )
        private val MESSAGING = setOf(
            "com.whatsapp", "org.telegram.messenger", "org.thoughtcrime.securesms",
            "com.google.android.apps.messaging", "com.facebook.orca",
            "com.snapchat.android", "com.instagram.threadsapp", "com.discord",
            "com.viber.voip", "com.skype.raider", "com.tencent.mm"
        )
        private val EMAIL = setOf(
            "com.google.android.gm", "com.microsoft.office.outlook",
            "com.yahoo.mobile.client.android.mail", "com.samsung.android.email.provider",
            "ch.protonmail.android", "me.proton.android.mail"
        )
        private val SOCIAL = setOf(
            "com.facebook.katana", "com.instagram.android", "com.twitter.android",
            "com.zhiliaoapp.musically", "com.reddit.frontpage",
            "com.linkedin.android", "com.pinterest"
        )
        private val ALARM = setOf(
            "com.google.android.deskclock", "com.android.deskclock",
            "com.sec.android.app.clockpackage"
        )
        private val PHONE = setOf(
            "com.google.android.dialer", "com.android.server.telecom",
            "com.samsung.android.dialer"
        )
        private val CALENDAR = setOf(
            "com.google.android.calendar", "com.samsung.android.app.calendar"
        )

        // Media-player apps to exclude entirely.  Most of these are caught
        // automatically by the EXTRA_MEDIA_SESSION / CATEGORY_TRANSPORT
        // checks in isMediaNotification(), but a few ship custom
        // RemoteViews layouts instead of MediaStyle (notably YouTube's
        // "now playing" notif) and need a package match to catch.
        private val MEDIA_PLAYERS = setOf(
            // Music streaming
            "com.spotify.music",
            "com.google.android.apps.youtube.music",
            "com.aspiro.tidal",
            "com.apple.android.music",
            "com.amazon.mp3",
            "deezer.android.app",
            "com.soundcloud.android",
            "com.pandora.android",
            "com.iheart.iheartradio.controller",
            // Video / streaming
            "com.google.android.youtube",
            "com.google.android.youtube.tv",
            "com.netflix.mediaclient",
            "com.amazon.avod.thirdpartyclient",
            "com.disney.disneyplus",
            "com.hulu.plus",
            "tv.twitch.android.app",
            "com.hbo.hbonow", "com.hbo.max",
            "com.peacocktv.peacockandroid",
            "com.cbs.app",          // Paramount+
            "com.crunchyroll.crunchyroid",
            // Podcasts / audiobooks
            "com.google.android.apps.podcasts",
            "com.audible.application",
            "fm.castbox.audiobook.radio.podcast",
            "au.com.shiftyjelly.pocketcasts",
            "com.bambuna.podcastaddict"
        )

        fun categoryFor(pkg: String, sysCategory: String?): String {
            if (pkg in BANKING)   return "banking"
            if (pkg in MESSAGING) return "messaging"
            if (pkg in ALARM)     return "alarm"
            if (pkg in PHONE)     return "phone"
            if (pkg in CALENDAR)  return "calendar"
            if (pkg in EMAIL)     return "email"
            if (pkg in SOCIAL)    return "social"
            return when (sysCategory) {
                Notification.CATEGORY_MESSAGE     -> "messaging"
                Notification.CATEGORY_EMAIL       -> "email"
                Notification.CATEGORY_CALL,
                Notification.CATEGORY_MISSED_CALL -> "phone"
                Notification.CATEGORY_ALARM       -> "alarm"
                Notification.CATEGORY_REMINDER,
                Notification.CATEGORY_EVENT       -> "calendar"
                Notification.CATEGORY_SOCIAL      -> "social"
                else -> "other"
            }
        }

        /**
         * Pull the most useful body text out of a notification.  Apps put text
         * in many different extras depending on style; we try them all and
         * fall back through them in order of usefulness.  Handles the common
         * `MessagingStyle` (per-message bundles) and `InboxStyle`
         * (text-lines array) cases that vanilla EXTRA_TEXT/BIG_TEXT miss.
         */
        @Suppress("DEPRECATION")
        fun extractText(n: Notification): String {
            val extras = n.extras ?: return ""

            // 1. MessagingStyle: take the most recent message body, prepend
            //    the sender for context (since the title is usually the
            //    conversation name, not the speaker on each message).
            val msgsArr: Array<Parcelable>? = if (Build.VERSION.SDK_INT >= 33) {
                extras.getParcelableArray(Notification.EXTRA_MESSAGES, Parcelable::class.java)
            } else {
                extras.getParcelableArray(Notification.EXTRA_MESSAGES)
            }
            if (msgsArr != null && msgsArr.isNotEmpty()) {
                val sb = StringBuilder()
                for (p in msgsArr) {
                    val b = p as? Bundle ?: continue
                    val sender = b.getCharSequence("sender")?.toString().orEmpty()
                    val text = b.getCharSequence("text")?.toString().orEmpty()
                    if (text.isEmpty()) continue
                    if (sb.isNotEmpty()) sb.append('\n')
                    if (sender.isNotEmpty()) sb.append(sender).append(": ")
                    sb.append(text)
                }
                if (sb.isNotEmpty()) return sb.toString()
            }

            // 2. BigTextStyle / standard text
            val big = extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString().orEmpty()
            if (big.isNotEmpty()) return big

            val text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString().orEmpty()
            if (text.isNotEmpty()) return text

            // 3. InboxStyle: array of CharSequence lines (Floatplane and other
            //    "you have N updates" style aggregators use this).
            val lines = extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)
            if (lines != null && lines.isNotEmpty()) {
                return lines.joinToString("\n") { it.toString() }
            }

            // 4. Fallback chain — sub-text / info / summary / ticker.
            extras.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString()
                ?.takeIf { it.isNotEmpty() }?.let { return it }
            extras.getCharSequence(Notification.EXTRA_INFO_TEXT)?.toString()
                ?.takeIf { it.isNotEmpty() }?.let { return it }
            extras.getCharSequence(Notification.EXTRA_SUMMARY_TEXT)?.toString()
                ?.takeIf { it.isNotEmpty() }?.let { return it }
            n.tickerText?.toString()?.takeIf { it.isNotEmpty() }?.let { return it }

            return ""
        }

        // (silent detection lives on the instance method below — it
        // needs access to NotificationListenerService.currentRanking)

        /**
         * Detect a media-playback notification (Spotify, YT Music, Tidal,
         * YouTube, podcast apps, etc.) so we can skip it entirely.
         *
         * The user doesn't want Jibo reading "Track: Foo by Bar" every time
         * the song changes, so we drop these regardless of how the notif
         * was constructed.  Detection order:
         *   1. Carries a MediaSession token       — definitive (covers all MediaStyle)
         *   2. CATEGORY_TRANSPORT                 — explicit transport-control category
         *   3. Package is in MEDIA_PLAYERS        — fallback for custom-RemoteViews layouts
         */
        fun isMediaNotification(pkg: String, n: Notification): Boolean {
            if (pkg in MEDIA_PLAYERS) return true
            if (n.category == Notification.CATEGORY_TRANSPORT) return true
            val extras = n.extras ?: return false
            // EXTRA_MEDIA_SESSION is always set by Notification.MediaStyle
            // (it holds the MediaSession.Token).  Presence alone is enough.
            if (extras.containsKey(Notification.EXTRA_MEDIA_SESSION)) return true
            // Some apps build their own templates but still set the
            // template name in EXTRA_TEMPLATE.
            val tpl = extras.getString(Notification.EXTRA_TEMPLATE).orEmpty()
            if (tpl.contains("MediaStyle", ignoreCase = true)) return true
            return false
        }
    }

    // ─── Bound BleService for forwarding ────────────────────────────────────
    private var bleService: BleService? = null
    private var bound = false
    private val pending = ArrayDeque<() -> Unit>()

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            bleService = (binder as BleService.LocalBinder).service
            bound = true
            // Drain any notifications that arrived before the bind completed.
            while (pending.isNotEmpty()) pending.removeFirst().invoke()
            // Push a fresh snapshot so Jibo isn't out of sync after a reconnect.
            try { bleService?.pushNotificationSnapshot(currentSnapshotEntries()) } catch (_: Exception) {}
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            bleService = null
            bound = false
        }
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        Log.i(TAG, "Listener connected")
        val intent = Intent(this, BleService::class.java)
        try { startForegroundService(intent) } catch (_: Exception) {}
        bindService(intent, conn, Context.BIND_AUTO_CREATE)
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        Log.i(TAG, "Listener disconnected")
        try { unbindService(conn) } catch (_: Exception) {}
        bound = false
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val entry = buildEntry(sbn) ?: return
        Log.d(TAG, "+ ${entry.app} <${entry.sender}> [${entry.category}] " +
                "text='${entry.text.take(60)}'")
        forward { bleService?.notifyAdded(entry) }
    }

    /**
     * Turn a [StatusBarNotification] into a [BleService.NotifEntry].  Filtering
     * is intentionally minimal -- the user wants to see *everything* the
     * status bar shows, including silent / ongoing / mirror-only notifications.
     * The only thing we drop is `GROUP_SUMMARY`, which is by definition a
     * duplicate of the child notifications that arrive separately.
     */
    private fun buildEntry(sbn: StatusBarNotification): BleService.NotifEntry? {
        val n = sbn.notification ?: return null
        val flags = n.flags

        // Drop our OWN app's notifications (the BleService foreground-service
        // notification, the listener-permission reminder, etc.).  Mirroring
        // them back to Jibo would surface a "new notification from Jibo"
        // pill on every boot — definitely not what the user wants.  We
        // belt-and-suspenders this with both the runtime applicationContext
        // package name and a hardcoded constant so renaming/refactoring
        // the package can't accidentally let our own notifications through.
        if (sbn.packageName == applicationContext.packageName ||
            sbn.packageName == "com.ss15.jibo") {
            Log.d(TAG, "skip self: ${sbn.packageName} key=${sbn.key}")
            return null
        }

        if ((flags and Notification.FLAG_GROUP_SUMMARY) != 0) {
            Log.d(TAG, "skip group-summary: ${sbn.packageName} key=${sbn.key}")
            return null
        }

        // Drop silent notifications (channel importance < DEFAULT or
        // legacy priority < DEFAULT).  These are notifs the user has
        // explicitly chosen not to be alerted about — quiet downloads,
        // background sync indicators, "now in foreground" stickies,
        // etc.  Forwarding them defeats their purpose.
        if (isSilent(sbn, n)) {
            Log.d(TAG, "skip silent: ${sbn.packageName} key=${sbn.key}")
            return null
        }

        // Media-player notifications (now-playing, transport controls) are
        // pure noise for an assistant — drop them.  This catches Spotify,
        // YT Music, Tidal, YouTube, podcast apps, etc.  See
        // isMediaNotification() for detection details.
        if (isMediaNotification(sbn.packageName, n)) {
            Log.d(TAG, "skip media: ${sbn.packageName} key=${sbn.key}")
            return null
        }

        // We do NOT filter on:
        //   FLAG_LOCAL_ONLY      — Phone Link / "Link to Windows" sets this
        //                          on every notification it mirrors so the
        //                          OS doesn't loop them back; users still
        //                          want to ask Jibo about those.
        //   FLAG_ONGOING_EVENT   — silent (non-media) notifications use this too.
        //   FLAG_FOREGROUND_SERVICE — ride-share / delivery trackers, etc.

        val extras = n.extras
        val title = extras?.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val text  = extractText(n)

        val pkg = sbn.packageName
        val appLabel = try {
            val pm = packageManager
            pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
        } catch (_: Exception) { pkg }

        // Even if the standard extras are empty (custom RemoteViews layouts
        // common in media controls, system notifications, etc.) ship the
        // entry anyway with a placeholder body so the user still hears about
        // it when they ask.  Keeps parity with what the status bar shows.
        val effectiveText = if (text.isNotEmpty()) text
                            else if (title.isNotEmpty()) ""
                            else "(no preview available)"

        val category = categoryFor(pkg, n.category)

        Log.d(TAG, "+ pkg=$pkg app='$appLabel' title='$title' text='${effectiveText.take(60)}' " +
                "flags=0x${Integer.toHexString(flags)} cat=$category")

        return BleService.NotifEntry(
            key = sbn.key,
            app = appLabel,
            sender = title,
            text = effectiveText,
            category = category,
            // Sensitivity / urgency are decided by Gemini from the actual
            // content -- never auto-flagged here.  The field is kept on the
            // wire for forward compatibility and serial debugging.
            timeSensitive = false,
            postTimeMs = sbn.postTime
        )
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        val key = sbn.key ?: return
        forward { bleService?.notifyRemoved(key) }
    }

    /**
     * "Silent" = user has set the channel below IMPORTANCE_DEFAULT (or
     * the legacy priority is below PRIORITY_DEFAULT on pre-O devices).
     * `currentRanking` is the listener-service-only API that exposes
     * the effective importance for any notification, taking user-set
     * channel overrides into account.
     */
    @Suppress("DEPRECATION")
    private fun isSilent(sbn: StatusBarNotification, n: Notification): Boolean {
        try {
            val ranking = currentRanking
            val rk = Ranking()
            if (ranking.getRanking(sbn.key, rk)) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N &&
                        rk.importance < NotificationManager.IMPORTANCE_DEFAULT) {
                    return true
                }
            }
        } catch (_: Exception) {
            // Fall through to legacy priority check.
        }
        if (n.priority < Notification.PRIORITY_DEFAULT) return true
        return false
    }

    private fun forward(block: () -> Unit) {
        if (bound && bleService != null) block() else pending.addLast(block)
    }

    private fun currentSnapshotEntries(): List<BleService.NotifEntry> {
        val active = try { activeNotifications } catch (_: Exception) { null }
            ?: return emptyList()
        val out = ArrayList<BleService.NotifEntry>(active.size)
        for (sbn in active) {
            buildEntry(sbn)?.let { out.add(it) }
        }
        Log.d(TAG, "snapshot: ${active.size} active, ${out.size} forwarded")
        return out
    }
}
