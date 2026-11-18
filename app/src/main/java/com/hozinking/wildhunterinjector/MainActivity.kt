package com.hozinking.wildhunterinjector

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.hozinking.wildhunterinjector.databinding.ActivityMainBinding
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Hozin Wild Hunter 3D Injector v1.0 — launcher root untuk Wild Hunter 3D
 * (com.julian.wildhunter) v1.0.16, Unity 2022.3.62f2 IL2CPP. Game TIDAK
 * dibundel — user install sendiri dari Play Store.
 *
 * ALUR (kerangka Traffic Rider, ModInjector.java + RootUtil.java —
 * teruji di POCO user): Tap "Inject ke Wild Hunter 3D" → cek root → cek
 * install/versi (PackageManager saja, cepat, tanpa root) → bila game
 * belum jalan, OTOMATIS launch game DULUAN (sebelum semua tunggu yang
 * lama — user mendarat di game dalam ~2 detik) → tunggu game siap
 * (libil2cpp ter-map maks 20 dtk + jeda settle 3 dtk) → inject otomatis
 * (retry 3x) → tunggu status payload final s.d. 60 dtk (v2.0.1: INIT
 * bukan status akhir) → bila sukses, bawa user
 * ke game (bringGameToFront ala TR). Bila gagal, tetap di aplikasi
 * supaya diagnosis bisa disalin.
 *
 * v1.0.1 gagal di POCO (log berhenti di "root: available", game tak
 * pernah dibawa ke depan): shell `su` persisten tanpa timeout +
 * scan /proc yang lambat menahan alur SEBELUM game sempat diluncurkan.
 * v1.0.2: tiap perintah root lewat `su -c` baru + timeout keras
 * (RootUtil), tanpa scan /proc, launch diverifikasi lewat PID.
 * Tombol mengambang (FloatingService) muncul begitu proses dimulai dan
 * tetap tampil apa pun hasilnya — anti gagal.
 *
 * Gate versi ada di payload (resolve class/method IL2CPP berdasarkan
 * nama — bukan versionCode). Game FULL OFFLINE, jadi TANPA gate mode:
 * fitur aktif saat berburu (senjata jalan).
 *
 * Diagnosis di dalam aplikasi (aturan 14 Nov — JANGAN suruh user
 * jalankan Termux/logcat): log injector langkah-demi-langkah tampil di
 * layar + tombol "Salin diagnosis".
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    @Volatile
    private var autoRunning = false

    /** v1.0.7: file log run inject yang sedang jalan (null di luar run). */
    @Volatile
    private var runLogFile: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Izin overlay diminta sekali di awal supaya tombol mengambang
        // bisa tampil; bila ditolak, alur tetap jalan tanpa overlay.
        if (!FloatingService.canDrawOverlays(this)) {
            FloatingService.requestPermission(this)
            log("Izin 'Tampilkan di atas aplikasi lain' diminta — izinkan supaya menu mengambang bisa muncul.")
        }

        binding.btnCheckRoot.setOnClickListener {
            Thread {
                val available = RootUtil.isRooted()
                log(if (available) "root: available" else "root: NOT available — butuh HP root")
            }.start()
        }

        binding.btnCheckGame.setOnClickListener {
            Thread {
                log(InjectorBridge.checkGame(this))
            }.start()
        }

        binding.btnInject.setOnClickListener {
            if (autoRunning) {
                log("proses inject masih jalan — tunggu selesai")
                return@setOnClickListener
            }
            Thread { runAutoInject() }.start()
        }

        binding.btnCopyDiag.setOnClickListener {
            val text = binding.textLog.text.toString()
            val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("HozinWildHunter diagnosis", text))
            Toast.makeText(this, "Diagnosis disalin", Toast.LENGTH_SHORT).show()
        }
    }

    /** Seluruh alur inject otomatis, dengan progres ke log.
     *  Urutan = kerangka Traffic Rider (ModInjector.inject):
     *  root → info game → launch game DULUAN bila belum jalan →
     *  tunggu game siap → inject → status → bawa ke game bila sukses. */
    private fun runAutoInject() {
        autoRunning = true
        try {
            startRunLog()
            log("== Inject v${BuildConfig.VERSION_NAME} — alur otomatis ==")
            // Tombol mengambang muncul duluan — anti gagal.
            FloatingService.start(this)

            // 1. Root.
            if (!RootUtil.isRooted()) {
                log("root: NOT available — butuh HP root")
                return
            }
            log("root: available")

            // 2. Install / versi — PackageManager saja (cepat, tanpa
            //    root; v1.0.1 menaruh findPid + scan /proc di sini dan
            //    menahan alur sebelum game sempat diluncurkan).
            val version = InjectorBridge.installedVersionCode(this)
            val versionName = InjectorBridge.installedVersionName(this)
            if (version == null) {
                log("Wild Hunter 3D belum terinstall — install com.julian.wildhunter dari Play Store dulu")
                return
            }
            log("Wild Hunter 3D terinstall: ${versionName ?: "?"} (code $version) — target v1.0.16 (code 17); " +
                "bila versi berbeda, payload yang memutuskan (resolve berdasarkan nama)")

            // 3. Game belum jalan → luncurkan otomatis DULUAN (TR:
            //    game dibuka dalam ~2 detik setelah tap Inject,
            //    SEBELUM semua tunggu yang lama).
            var pid = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
            if (pid == null) {
                log("Wild Hunter 3D belum jalan — meluncurkan otomatis…")
                if (!InjectorBridge.launchGame { log(it) }) {
                    log("launch game GAGAL — buka Wild Hunter 3D manual lalu tap Inject lagi")
                    return
                }
                pid = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
                if (pid == null) {
                    log("game tidak muncul (PID tak ketemu setelah launch)")
                    return
                }
            }
            log("pid Wild Hunter 3D: $pid")

            // 4. Tunggu game SIAP (TR waitForGameReady — libil2cpp
            //    ter-map maks 20 dtk + jeda settle 3 dtk).
            if (!InjectorBridge.waitForGameReady(pid) { log(it) }) {
                log("PID game mati/berubah saat menunggu Unity siap — batal")
                return
            }

            // 5. Inject + diagnosis.
            val result = InjectorBridge.inject(this, pid)
            if (result.diag.isNotEmpty()) log(result.diag)
            log("injector exit=${result.exitCode}")
            val pidAfter = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
            log(
                if (pidAfter != null) "game masih jalan setelah inject (pid $pidAfter)"
                else "game MATI setelah inject — kemungkinan crash saat remote call"
            )
            // v1.0.6: pembeda kunci — ter-map tapi tanpa status =
            // constructor/worker diam atau tulis gagal; tidak ter-map =
            // dlopen tidak benar-benar load.
            val mapped = pidAfter?.let { InjectorBridge.isPayloadLoaded(it) } ?: false
            log("libpayload.so ter-map di game: ${if (mapped) "ya" else "tidak"}")
            var injectedOk = false
            if (result.exitCode == 0) {
                val ps = waitForPayloadStatus(60)
                val (rawStatus, statusSrc) = InjectorBridge.readPayloadStatusWithSource()
                log("--- isi file status payload ---")
                if (statusSrc.isNotEmpty()) log("status dari: $statusSrc")
                log(if (rawStatus.isNotEmpty()) rawStatus else "(file status payload belum ada)")
                val (ctorLog, ctorSrc) = InjectorBridge.readCtorLogWithSource()
                if (ctorLog.isNotEmpty()) {
                    if (ctorSrc.isNotEmpty()) log("ctor log dari: $ctorSrc")
                    log("--- ctor log ---\n$ctorLog")
                }
                val crash = InjectorBridge.readCrashFile()
                if (crash.isNotEmpty()) {
                    log("--- bukti crash (hozinhunter_crash.json) ---\n$crash")
                    // v2.0.1: hapus SETELAH dibaca + dicatat ke log run —
                    // kalau tidak, file basi ini membuat SEMUA run
                    // berikutnya dilaporkan STATUS=CRASH.
                    InjectorBridge.clearCrashFile()
                }
                log("pompa main-thread: ${ps.pump} (panggilan, ms sejak terakhir)")
                val pidFinal = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
                log(
                    if (pidFinal != null) "game: masih jalan (pid $pidFinal)"
                    else "game: MATI — crash saat init payload (cari STATUS=CRASH di atas)"
                )
                when {
                    ps.status == "READY" -> {
                        injectedOk = true
                        log("payload READY — semua class/method IL2CPP ketemu (lib ${ps.libSize} B, hook ${ps.hook})")
                        log("inject OK — kembali ke game, tap tombol 🏹 untuk menu mod (game full offline)")
                    }
                    ps.status.startsWith("PARTIAL") -> {
                        injectedOk = true
                        log("payload PARTIAL (${ps.status}) — sebagian resolve gagal: ${ps.failed} — " +
                            "game tetap jalan, fitur yang gagal nonaktif (bukan toggle palsu)")
                    }
                    ps.status.startsWith("CRASH") -> {
                        log("payload CRASH saat init: ${ps.status} — langkah terakhir ada di STEP di atas. Salin diagnosis ini.")
                    }
                    ps.status == "UNSUPPORTED" -> {
                        val v = versionName ?: "?"
                        val c = version ?: -1
                        log("versi $v (code $c) BELUM didukung — tidak ketemu: ${ps.failed}")
                        log("BuildID libil2cpp.so tidak tercatat — payload memakai nama, bukan offset")
                    }
                    else -> {
                        log("inject exit 0 (dlopen OK) tapi status payload tak kunjung final setelah 60 dtk — " +
                            "payload mungkin masih init (lihat INIT_STEP di atas), atau file status tak bisa ditulis. Tap Inject ulang.")
                    }
                }
            } else {
                log("inject GAGAL — ${InjectorBridge.failureReason(result.exitCode)}")
            }
            // Bawa user ke game HANYA bila inject sukses (pola Traffic
            // Rider: bringGameToFront di thread baru setelah status OK,
            // tanpa moveTaskToBack — `am start -n` via root yang
            // memindahkan user). Bila gagal, tetap di sini supaya
            // diagnosis bisa disalin.
            if (injectedOk && pidAfter != null) {
                log("kembali ke game…")
                Thread { InjectorBridge.bringGameToFront() }.start()
            }
        } finally {
            // v1.0.7: tutup run log — baris path ini ikut masuk file
            // karena runLogFile masih terpasang saat log() dipanggil.
            runLogFile?.let { log("log tersimpan: ${it.absolutePath}") }
            runLogFile = null
            autoRunning = false
        }
    }

    /** v1.0.7: mulai file log per-run di penyimpanan external aplikasi
     *  (/sdcard/Android/data/<pkg>/files/logs/inject-*.log — bisa
     *  dibuka dari file manager tanpa root). Bila external tak tersedia,
     *  fallback ke filesDir/logs internal + keterangan di log UI. */
    private fun startRunLog() {
        val stamp = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(Date())
        val stampHuman = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        val ext = getExternalFilesDir(null)
        val dir = if (ext != null) File(ext, "logs") else File(filesDir, "logs")
        try {
            dir.mkdirs()
            val f = File(dir, "inject-$stamp.log")
            f.appendText("Hozin Wild Hunter 3D Injector v${BuildConfig.VERSION_NAME} — run $stampHuman\n")
            runLogFile = f
        } catch (_: Exception) {
            runLogFile = null
        }
        if (ext == null) log("penyimpanan external tak tersedia — log run disimpan internal")
    }

    /** Poll file status payload sampai timeout detik; status "" bila tak muncul.
     *  v2.0.1: INIT BUKAN status akhir — terus tunggu sampai READY /
     *  PARTIAL / UNSUPPORTED / CRASH (init payload yang sehat bisa
     *  butuh 30–60 dtk: cold start + domain IL2CPP). */
    private fun waitForPayloadStatus(timeoutSec: Int): InjectorBridge.PayloadStatus {
        var last = InjectorBridge.PayloadStatus("", "-", "-", "-", "-", "-", "")
        repeat(timeoutSec * 2) {
            val raw = InjectorBridge.readPayloadStatus()
            val ps = InjectorBridge.parsePayloadStatus(raw)
            if (ps.status.isNotEmpty()) last = ps
            if (ps.status == "READY" || ps.status.startsWith("PARTIAL") ||
                ps.status == "UNSUPPORTED" || ps.status.startsWith("CRASH")
            ) return ps
            Thread.sleep(500)
        }
        return last
    }

    private fun log(line: String) {
        runOnUiThread {
            binding.textLog.append(line + "\n")
        }
        // v1.0.7: baris yang sama di-append ke file log run ini
        // (best-effort — kegagalan tulis tidak boleh mengganggu inject).
        try {
            runLogFile?.appendText(line + "\n")
        } catch (_: Exception) {
        }
    }
}
