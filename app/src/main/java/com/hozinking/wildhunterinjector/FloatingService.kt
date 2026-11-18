package com.hozinking.wildhunterinjector

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject

/**
 * Service overlay: tombol mengambang "🏹" + menu mod + kanvas ESP.
 *
 * Standar injector 16 Nov: logo/menu floating WAJIB selalu tampil apa
 * pun hasil inject — service ini dijalankan MainActivity di awal alur
 * inject, jadi tombol muncul bahkan bila inject gagal.
 *
 * Menu berisi 7 toggle (default OFF) + tombol koin sekali tap.
 * Setiap perubahan toggle ditulis ke file config di cache game (via
 * root) — payload membacanya tiap 500 ms. Game ini FULL OFFLINE,
 * jadi TANPA gate mode: fitur aktif saat berburu (senjata jalan).
 * Tombol koin menulis file perintah {"addcoins":1000} — payload
 * memberikannya lewat PlayerDataManager.AddCoinsOne (koin gameplay,
 * bukan IAP gratis).
 *
 * ESP: thread polling membaca file JSON payload tiap 150 ms (via root)
 * lalu menggambar kotak + bar HP + nama/jarak di EspView.
 */
class FloatingService : Service() {

    private var wm: WindowManager? = null
    private var buttonView: TextView? = null
    private var menuView: LinearLayout? = null
    private var espView: EspView? = null
    private var modeText: TextView? = null
    private var menuParams: WindowManager.LayoutParams? = null

    @Volatile
    private var polling = false

    // State toggle (satu sumber kebenaran untuk config yang ditulis).
    @Volatile var aimbot = false
    @Volatile var zoomAim = false
    @Volatile var esp = false
    @Volatile var noRecoil = false
    @Volatile var noReload = false
    @Volatile var zoomFar = false
    @Volatile var energy = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForegroundNotification()
        wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        if (!canDrawOverlays(this)) {
            Toast.makeText(
                this,
                "Izin overlay belum diberikan — menu mengambang tidak bisa tampil. " +
                    "Beri izin 'Tampilkan di atas aplikasi lain' lalu tap Inject lagi.",
                Toast.LENGTH_LONG
            ).show()
            return
        }
        writeDefaultConfig()
        addButton()
        startEspPolling()
    }

    override fun onDestroy() {
        polling = false
        try {
            buttonView?.let { wm?.removeView(it) }
            menuView?.let { wm?.removeView(it) }
            espView?.let { wm?.removeView(it) }
        } catch (_: Exception) {
        }
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        return START_STICKY
    }

    private fun startForegroundNotification() {
        val channelId = "hozinhunter_fg"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                channelId, "Hozin Wild Hunter 3D",
                NotificationManager.IMPORTANCE_LOW
            )
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(ch)
        }
        val notif = Notification.Builder(this, channelId)
            .setContentTitle("Hozin Wild Hunter 3D Injector")
            .setContentText("Menu mod + ESP mengambang aktif (game full offline)")
            .setSmallIcon(android.R.drawable.ic_menu_info_details)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            @Suppress("DEPRECATION")
            startForeground(1, notif)
        }
    }

    /** Tombol mengambang (draggable; tap = buka/tutup menu). */
    private fun addButton() {
        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = 80
            y = 320
        }
        val btn = TextView(this).apply {
            text = "🏹"
            textSize = 30f
            setBackgroundColor(0x88000000.toInt())
            setPadding(12, 8, 12, 8)
        }
        var downX = 0f
        var downY = 0f
        var moved = false
        btn.setOnTouchListener { _, ev ->
            when (ev.action) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.rawX - params.x
                    downY = ev.rawY - params.y
                    moved = false
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val nx = (ev.rawX - downX).toInt()
                    val ny = (ev.rawY - downY).toInt()
                    if (kotlin.math.abs(nx - params.x) + kotlin.math.abs(ny - params.y) > 10) {
                        moved = true
                    }
                    params.x = nx
                    params.y = ny
                    try {
                        wm?.updateViewLayout(btn, params)
                    } catch (_: Exception) {
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    if (!moved) toggleMenu()
                    true
                }
                else -> false
            }
        }
        buttonView = btn
        wm?.addView(btn, params)
    }

    /** Menu mod: 7 toggle + tombol koin + indikator mode + tombol tutup. */
    private fun buildMenu(): LinearLayout {
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xDD111111.toInt())
            setPadding(24, 20, 24, 20)
        }
        // Judul = gagang geser menu (v1.0.5 Sniper — keluhan user: "kotak
        // floating gak bisa digeser"). Tombol 🏹 sudah bisa digeser
        // sejak awal; yang tidak bisa digeser adalah PANEL menu ini.
        // Drag hanya di judul supaya tap pada Switch tetap bekerja.
        val title = TextView(this).apply {
            text = "Hozin Wild Hunter 3D v${BuildConfig.VERSION_NAME}"
            textSize = 17f
            setTextColor(0xFFFFFFFF.toInt())
        }
        var dragDownX = 0f
        var dragDownY = 0f
        title.setOnTouchListener { _, ev ->
            val p = menuParams ?: return@setOnTouchListener false
            when (ev.action) {
                MotionEvent.ACTION_DOWN -> {
                    dragDownX = ev.rawX - p.x
                    dragDownY = ev.rawY - p.y
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    p.x = (ev.rawX - dragDownX).toInt()
                    p.y = (ev.rawY - dragDownY).toInt()
                    try {
                        menuView?.let { wm?.updateViewLayout(it, p) }
                    } catch (_: Exception) {
                    }
                    true
                }
                else -> false
            }
        }
        layout.addView(title)
        modeText = TextView(this).apply {
            text = "Mode: OFFLINE"
            textSize = 14f
            setTextColor(0xFFFFFF00.toInt())
        }
        layout.addView(modeText!!)
        val hint = TextView(this).apply {
            text = "Game full offline — tanpa gate mode.\nFitur aktif saat berburu (senjata jalan).\nGeser menu dari baris judul."
            textSize = 12f
            setTextColor(0xFFCCCCCC.toInt())
        }
        layout.addView(hint)

        fun addToggle(label: String, initial: Boolean, onChange: (Boolean) -> Unit) {
            val sw = Switch(this).apply {
                text = label
                isChecked = initial
                setTextColor(0xFFFFFFFF.toInt())
                setOnCheckedChangeListener { _, checked -> onChange(checked) }
            }
            layout.addView(sw)
        }
        addToggle("Aimbot / Aimlock", aimbot) { v -> aimbot = v; writeConfigAsync() }
        addToggle("Auto-aim saat scope", zoomAim) { v -> zoomAim = v; writeConfigAsync() }
        addToggle("ESP (kotak + bar HP)", esp) { v ->
            esp = v
            writeConfigAsync()
            updateEspView()
        }
        addToggle("No recoil", noRecoil) { v -> noRecoil = v; writeConfigAsync() }
        addToggle("No reload (ammo tak habis)", noReload) { v -> noReload = v; writeConfigAsync() }
        addToggle("Zoom jauh", zoomFar) { v -> zoomFar = v; writeConfigAsync() }
        addToggle("Energi tak habis", energy) { v -> energy = v; writeConfigAsync() }

        // Tombol koin sekali tap (bukan toggle): +1000 koin gameplay
        // per tap via AddCoinsOne. Bukan IAP gratis.
        val coins = TextView(this).apply {
            text = "＋ Tambah Koin 1000"
            textSize = 15f
            setTextColor(0xFF7CFC00.toInt())
            setPadding(0, 16, 0, 0)
            setOnClickListener {
                Thread {
                    val ok = InjectorBridge.writeCoinsCommand(1000)
                    android.os.Handler(android.os.Looper.getMainLooper()).post {
                        Toast.makeText(
                            this@FloatingService,
                            if (ok) "Koin +1000 dikirim — cek dalam ~2 detik"
                            else "Gagal kirim perintah koin (root?)",
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                }.start()
            }
        }
        layout.addView(coins)

        val close = TextView(this).apply {
            text = "Tutup menu"
            textSize = 14f
            setTextColor(0xFF66CCFF.toInt())
            setPadding(0, 16, 0, 0)
            setOnClickListener { toggleMenu() }
        }
        layout.addView(close)
        return layout
    }

    private fun toggleMenu() {
        val manager = wm ?: return
        if (menuView == null) {
            val params = WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                PixelFormat.TRANSLUCENT
            ).apply {
                gravity = Gravity.TOP or Gravity.START
                x = 80
                y = 420
            }
            val menu = buildMenu()
            menuParams = params
            menuView = menu
            manager.addView(menu, params)
        } else {
            try {
                manager.removeView(menuView)
            } catch (_: Exception) {
            }
            menuView = null
            menuParams = null
        }
    }

    private fun updateEspView() {
        val manager = wm ?: return
        if (esp && espView == null) {
            val params = WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT
            )
            val v = EspView(this)
            espView = v
            manager.addView(v, params)
        } else if (!esp && espView != null) {
            try {
                manager.removeView(espView)
            } catch (_: Exception) {
            }
            espView = null
        }
    }

    /** Config default (semua OFF) supaya payload punya file sejak awal. */
    private fun writeDefaultConfig() {
        Thread { writeConfigNow() }.start()
    }

    private fun writeConfigAsync() {
        Thread { writeConfigNow() }.start()
    }

    private fun configJson(): String {
        return "{\"aimbot\":$aimbot,\"zoomaim\":$zoomAim,\"esp\":$esp," +
            "\"norecoil\":$noRecoil,\"noreload\":$noReload,\"zoom\":$zoomFar," +
            "\"energy\":$energy}"
    }

    private fun writeConfigNow() {
        InjectorBridge.writeConfig(configJson())
    }

    /** Poll file ESP payload tiap 150 ms; update kanvas + indikator mode. */
    private fun startEspPolling() {
        polling = true
        Thread {
            while (polling) {
                try {
                    val json = InjectorBridge.readEspJson()
                    if (json.isNotEmpty()) {
                        val obj = JSONObject(json)
                        val mode = obj.optString("mode", "UNKNOWN")
                        val arr: JSONArray = obj.optJSONArray("t") ?: JSONArray()
                        val list = ArrayList<EspView.Target>()
                        for (i in 0 until arr.length()) {
                            val o = arr.getJSONObject(i)
                            list.add(
                                EspView.Target(
                                    x = o.optDouble("x", 0.0).toFloat(),
                                    y = o.optDouble("y", 0.0).toFloat(),
                                    w = o.optDouble("w", 0.0).toFloat(),
                                    h = o.optDouble("h", 0.0).toFloat(),
                                    hpRatio = o.optDouble("hp", 0.0).toFloat(),
                                    name = o.optString("n", ""),
                                    dist = o.optDouble("d", 0.0).toFloat()
                                )
                            )
                        }
                        val targets: List<EspView.Target> = list
                        espView?.post {
                            espView?.targets = targets
                            espView?.invalidate()
                        }
                        modeText?.post { modeText?.text = "Mode: $mode" }
                    }
                } catch (_: Exception) {
                    // File belum ada / JSON rusak sesaat — diam, coba lagi.
                }
                try {
                    Thread.sleep(150)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }.start()
    }

    companion object {
        fun start(context: Context) {
            val intent = Intent(context, FloatingService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun canDrawOverlays(context: Context): Boolean {
            return Settings.canDrawOverlays(context)
        }

        fun requestPermission(context: Context) {
            val intent = Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:${context.packageName}")
            )
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        }
    }
}
