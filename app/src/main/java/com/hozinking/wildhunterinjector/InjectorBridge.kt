package com.hozinking.wildhunterinjector

import android.content.Context
import android.content.pm.PackageManager
import java.io.File
import java.util.zip.ZipFile

/**
 * Jembatan Kotlin ↔ native injector (ptrace + remote dlopen) untuk
 * Wild Hunter 3D v1.0.16 (com.julian.wildhunter).
 *
 * Pola staging (pelajaran PvZ v1.1/v1.2 + HCR v1.10): payload HARUS di
 * cache game sendiri (/data/data/<game>/cache, konteks app_data_file
 * milik game) karena /data/local/tmp sering tak bisa dibaca proses game
 * (SELinux) — dlopen dari sana NULL. Injector menangkap dlerror() dan
 * mencoba jalur cadangan.
 *
 * v1.0.2: SELURUH alur root + launch ditulis ulang mengikuti kerangka
 * Traffic Rider (ModInjector.java + RootUtil.java, teruji di POCO user):
 * tiap perintah root lewat `su -c` baru dengan timeout keras (RootUtil),
 * game diluncurkan DULUAN sebelum semua tunggu yang lama, dan dibawa
 * ke depan HANYA bila inject sukses. RootShell persisten v1.0–v1.0.1
 * DIHAPUS (tanpa timeout — alur bisa membeku selamanya, bukti tes
 * v1.0.1: log berhenti di "root: available").
 *
 * Gate versi: aplikasi TIDAK menolak berdasarkan versionCode. Gate ada
 * di payload — semua class/method IL2CPP harus ter-resolve berdasarkan
 * nama. Setelah inject, aplikasi membaca file status payload (tunggu
 * status final s.d. 60 dtk, v2.0.1) dan menampilkan READY / PARTIAL /
 * UNSUPPORTED / CRASH + nama yang gagal.
 *
 * Config toggle (ditulis aplikasi, dibaca payload tiap 500 ms) dan data
 * ESP (ditulis payload, dibaca overlay aplikasi) sama-sama tinggal di
 * cache game dan diakses aplikasi lewat root.
 */
object InjectorBridge {

    const val TARGET_PACKAGE = "com.julian.wildhunter"
    const val TARGET_LIB = "libil2cpp.so"

    private const val INJECTOR_LIB = "libinjector.so"
    private const val PAYLOAD_LIB = "libpayload.so"

    private const val PAYLOAD_TMP = "/data/local/tmp/hozinhunter_payload.so"
    private const val PAYLOAD_GAME =
        "/data/data/com.julian.wildhunter/cache/hozinhunter_payload.so"
    private const val GAME_CACHE_DIR = "/data/data/com.julian.wildhunter/cache"
    private const val GAME_DATA_DIR = "/data/data/com.julian.wildhunter"
    /** Hasil remote-dlopen JSON dari injector TR
     *  ({"dlopen":"ok"|"null"|"crash"|"exited"|"aborted","signal":N,
     *  "handle":"0x..","pc_after":"0x..","dlerror":"..."}). Ditulis
     *  injector sebagai root; dibaca aplikasi via su. */
    const val INJECT_RESULT = "/data/local/tmp/hozinhunter_inject.json"

    /** File status payload (payload menulis ke yang pertama bisa). */
    private const val PAYLOAD_STATUS_TMP = "/data/local/tmp/hozinhunter_payload.log"
    private const val PAYLOAD_STATUS_CACHE =
        "/data/data/com.julian.wildhunter/cache/hozinhunter_payload.log"
    /** Kanal status/ctor KETIGA (v1.0.6): external files game — game UID
     *  bisa tulis dir external-nya sendiri tanpa permission, root bisa
     *  baca /sdcard. Dipakai bila cache + /data/local/tmp tak bisa
     *  ditulis proses game (bukti tes v1.0.5: status + ctor kosong). */
    private const val PAYLOAD_STATUS_EXT =
        "/sdcard/Android/data/com.julian.wildhunter/files/hozinhunter_payload.log"

    /** Marker constructor payload — apakah constructor sempat kembali. */
    private const val CTOR_LOG_TMP = "/data/local/tmp/hozinhunter_ctor.log"
    private const val CTOR_LOG_CACHE =
        "/data/data/com.julian.wildhunter/cache/hozinhunter_ctor.log"
    private const val CTOR_LOG_EXT =
        "/sdcard/Android/data/com.julian.wildhunter/files/hozinhunter_ctor.log"

    /** Config toggle + data ESP (keduanya di cache game). */
    const val CONFIG_PATH = "$GAME_CACHE_DIR/hozinhunter_config.json"
    const val ESP_PATH = "$GAME_CACHE_DIR/hozinhunter_esp.json"
    /** Perintah koin sekali tap (ditulis aplikasi, dibaca + dihapus
     *  pompa main-thread): {"addcoins":1000}. */
    const val CMD_PATH = "$GAME_CACHE_DIR/hozinhunter_cmd.json"
    /** Bukti crash terpisah (ditulis penangkap crash payload, TIDAK
     *  pernah ditimpa worker) — dibaca diagnosis. */
    private const val CRASH_PATH = "$GAME_CACHE_DIR/hozinhunter_crash.json"

    data class InjectResult(val exitCode: Int, val output: String, val diag: String)

    /** Status payload, diparse dari hozinhunter_payload.log. */
    data class PayloadStatus(
        val status: String,      // READY | PARTIAL | UNSUPPORTED | CRASH | ""
        val failed: String,      // daftar nama yang gagal resolve, "-" bila tak ada
        val libSize: String,
        val mode: String,        // OFFLINE | "-"
        val hook: String,        // ok | gagal | "-"
        val pump: String,        // "<jumlah panggilan pompa> <ms sejak pompa terakhir>"
        val raw: String
    )

    fun injectorPath(context: Context): String =
        File(context.applicationInfo.nativeLibraryDir, INJECTOR_LIB).absolutePath

    fun payloadPath(context: Context): String =
        File(context.applicationInfo.nativeLibraryDir, PAYLOAD_LIB).absolutePath

    /**
     * Cari file .so native: utama nativeLibraryDir; bila tidak ada di
     * sana, ekstrak dari APK sendiri (sourceDir) ke filesDir + chmod 755.
     * Return path absolut, atau null bila keduanya gagal.
     *
     * Latar: v1.0 gagal total dengan exit=127 karena manifest TIDAK
     * menyetel extractNativeLibs (AGP default false) → .so hanya
     * tersimpan di APK dan TIDAK PERNAH diekstrak ke nativeLibraryDir,
     * jadi shell su melaporkan "command not found". v1.0.1 menyetel
     * extractNativeLibs=true di manifest (akar diperbaiki, pola HCR);
     * fungsi ini adalah jaring pengaman bila file tetap tidak ada.
     */
    fun resolveNativeLib(context: Context, libName: String): String? {
        val inLibDir = File(context.applicationInfo.nativeLibraryDir, libName)
        if (inLibDir.exists()) return inLibDir.absolutePath
        val dest = File(context.filesDir, libName)
        if (dest.exists() && dest.length() > 0) return dest.absolutePath
        return try {
            val apk = ZipFile(context.applicationInfo.sourceDir)
            val entry = apk.getEntry("lib/arm64-v8a/$libName") ?: return null
            apk.getInputStream(entry).use { inp ->
                dest.outputStream().use { out -> inp.copyTo(out) }
            }
            RootUtil.su("chmod 755 '${dest.absolutePath}'")
            if (dest.exists() && dest.length() > 0) dest.absolutePath else null
        } catch (e: Exception) {
            null
        }
    }

    /** Cek game terinstall + tampilkan versi apa adanya (tanpa penilaian). */
    fun checkGame(context: Context): String {
        val pid = findPid(TARGET_PACKAGE)
        val pidNote = if (pid != null) " (jalan, pid $pid)" else " (belum jalan)"
        val info = try {
            context.packageManager.getPackageInfo(TARGET_PACKAGE, 0)
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
        if (info != null) {
            // longVersionCode baru ada di API 28; di bawah itu pakai versionCode.
            val code = if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode
            else info.versionCode.toLong()
            val name = info.versionName ?: "?"
            return "Wild Hunter 3D terinstall: $name ($code)$pidNote — target v1.0.16 (code 17); " +
                "bila versi berbeda, payload yang memutuskan (resolve berdasarkan nama)"
        }
        if (pid != null) {
            return "Wild Hunter 3D JALAN (pid $pid) tapi info versi tak terbaca via PackageManager — " +
                "inject tetap dicoba, payload yang memverifikasi (fail-closed)"
        }
        return "Wild Hunter 3D TIDAK terinstall — install com.julian.wildhunter dari Play Store dulu"
    }

    fun installedVersionCode(context: Context): Long? {
        return try {
            val info = context.packageManager.getPackageInfo(TARGET_PACKAGE, 0)
            if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode
            else info.versionCode.toLong()
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
    }

    fun installedVersionName(context: Context): String? {
        return try {
            context.packageManager.getPackageInfo(TARGET_PACKAGE, 0).versionName
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
    }

    /** Baca file status payload (tiga lokasi, via root). "" bila tak ada. */
    fun readPayloadStatus(): String = readPayloadStatusWithSource().first

    /** Baca file status payload dari 3 lokasi (tmp → cache → external).
     *  @return Pair(isi file, sumber: "tmp"|"cache"|"external"|"") */
    fun readPayloadStatusWithSource(): Pair<String, String> {
        val r = RootUtil.su(
            "for f in '$PAYLOAD_STATUS_TMP' '$PAYLOAD_STATUS_CACHE' '$PAYLOAD_STATUS_EXT'; do " +
                "if [ -s \"\$f\" ]; then echo \"SRC=\$f\"; cat \"\$f\"; break; fi; done"
        )
        return splitSrc(r.output.trim())
    }

    /** Baca ctor log payload. "" bila tak ada. */
    fun readCtorLog(): String = readCtorLogWithSource().first

    /** Baca ctor log payload dari 3 lokasi (tmp → cache → external). */
    fun readCtorLogWithSource(): Pair<String, String> {
        val r = RootUtil.su(
            "for f in '$CTOR_LOG_TMP' '$CTOR_LOG_CACHE' '$CTOR_LOG_EXT'; do " +
                "if [ -s \"\$f\" ]; then echo \"SRC=\$f\"; cat \"\$f\"; break; fi; done"
        )
        return splitSrc(r.output.trim())
    }

    /** Pisah baris marker "SRC=<path>" dari isi file. */
    private fun splitSrc(out: String): Pair<String, String> {
        if (!out.startsWith("SRC=")) return "" to ""
        val nl = out.indexOf('\n')
        if (nl < 0) return "" to ""
        val src = when (out.substring(4, nl)) {
            PAYLOAD_STATUS_TMP, CTOR_LOG_TMP -> "tmp"
            PAYLOAD_STATUS_CACHE, CTOR_LOG_CACHE -> "cache"
            PAYLOAD_STATUS_EXT, CTOR_LOG_EXT -> "external"
            else -> "?"
        }
        return out.substring(nl + 1).trim() to src
    }

    /** Parse isi file status payload. status "" = file belum ada/tak terbaca. */
    fun parsePayloadStatus(raw: String): PayloadStatus {
        if (raw.isEmpty()) return PayloadStatus("", "-", "-", "-", "-", "-", "")
        var status = ""
        var failed = "-"
        var libSize = "-"
        var mode = "-"
        var hook = "-"
        var pump = "-"
        for (line in raw.lines()) {
            val parts = line.split("=", limit = 2)
            if (parts.size != 2) continue
            when (parts[0]) {
                "STATUS" -> status = parts[1]
                "FAILED" -> failed = parts[1]
                "LIBSIZE" -> libSize = parts[1]
                "MODE" -> mode = parts[1]
                "HOOK" -> hook = parts[1]
                "PUMP" -> pump = parts[1]
            }
        }
        return PayloadStatus(status, failed, libSize, mode, hook, pump, raw)
    }

    /** Arti exit code injector dalam bahasa manusia.
     *  v1.0.4 (kerangka TR): injector mengembalikan exit 0 bila ia
     *  sempat jalan — hasil remote-dlopen ada di file JSON, BUKAN di
     *  exit code. Status dlopen dipetakan ke kode sintetis 9–12 supaya
     *  MainActivity (exitCode == 0) tak berubah. */
    fun failureReason(exitCode: Int): String = when (exitCode) {
        0 -> "OK"
        1 -> "injector gagal sebelum attach — baca diagnosis (resolusi dlopen / attach ditolak)"
        2 -> "argumen injector salah (bug aplikasi)"
        9 -> "dlopen payload mengembalikan NULL — baca baris dlerror di diagnosis (alasan linker yang sebenarnya)"
        10 -> "target CRASH di dalam dlopen (constructor payload / init native) — sinyal + pc ada di diagnosis"
        11 -> "inject dibatalkan sebelum hijack (pc target bukan executable / stack tidak aman) — game tidak disentuh"
        12 -> "target keluar saat dlopen (game mati di tengah inject)"
        -1 -> "perintah injector TIMEOUT (30 dtk) — game mungkin membeku di dlopen; buka game manual dan cek"
        127 -> "injector tidak ditemukan / tidak bisa dieksekusi (command not found — baca baris 'injector:' di diagnosis)"
        else -> "gagal tak dikenal (exit=$exitCode)"
    }

    /** pidof saja (pola TR — tanpa scan /proc fallback: scan itu
     *  memakan waktu beberapa detik dan menahan SELURUH alur sebelum
     *  game sempat diluncurkan; bukti tes v1.0.1). */
    fun findPid(processName: String): Int? {
        val pid = RootUtil.pidOf(processName)
        return if (pid > 0) pid else null
    }

    /** Apakah lib sudah termuat di proses game (poll /proc/<pid>/maps). */
    fun isLibLoaded(pid: Int, libName: String): Boolean {
        return RootUtil.mapsContains(pid, libName)
    }

    /** Anti-double-load: true bila payload sudah ter-map di proses game. */
    fun isPayloadLoaded(pid: Int): Boolean = isLibLoaded(pid, "payload\\.so")

    /** Tunggu game SIAP sebelum ptrace attach (pola TR verbatim):
     *  libil2cpp.so ter-map (maks 20 dtk) + jeda settle 3 dtk biar main
     *  thread keluar dari rantai dlopen Unity. Tanpa ini remote-dlopen
     *  bisa mendarat saat main thread masih memegang linker lock ->
     *  dlopen bersarang deadlock (pelajaran TR v2.0/v2.1).
     *  Bila lib belum ter-map setelah 20 dtk, LANJUT — payload menunggu
     *  sendiri; hanya PID mati/berubah yang membatalkan.
     *  @return false bila PID mati/berubah selama menunggu. */
    fun waitForGameReady(pid: Int, log: (String) -> Unit): Boolean {
        log("Tunggu libil2cpp.so dimuat (maks 20 dtk)...")
        var mapped = false
        for (i in 0 until 20) {
            if (RootUtil.mapsContains(pid, TARGET_LIB)) {
                mapped = true
                break
            }
            if (!RootUtil.pidAlive(pid)) {
                log("PID game mati saat menunggu lib — batal.")
                return false
            }
            Thread.sleep(1000)
        }
        if (!mapped) {
            log("libil2cpp.so belum ter-map setelah 20 dtk — lanjut, payload akan menunggu sendiri.")
        } else {
            log("libil2cpp.so ter-map — jeda 3 dtk biar Unity selesai load...")
            try {
                Thread.sleep(3000)
            } catch (_: InterruptedException) {
            }
            if (!RootUtil.pidAlive(pid)) {
                log("PID game mati saat jeda — batal.")
                return false
            }
        }
        return true
    }

    /** Buka game via root (pola Traffic Rider verbatim): resolve launcher
     *  activity lalu `am start -n`; fallback `monkey` bila resolve gagal.
     *  Poll pidOf maks 20 detik — return true HANYA bila PID game
     *  benar-benar muncul (v1.0.1 return true langsung setelah `am
     *  start` tanpa verifikasi, jadi kegagalan launch tak ketahuan).
     *  Tanpa fallback intent aplikasi: startActivity dari thread
     *  background bisa diblokir pembatasan background-activity-launch
     *  di Android 16, sementara `am start` via root selalu lolos. */
    fun launchGame(log: (String) -> Unit): Boolean {
        val safe = TARGET_PACKAGE.replace(Regex("[^a-zA-Z0-9_.]"), "")
        if (safe.isEmpty()) return false
        val resolve = RootUtil.su(
            "cmd package resolve-activity --brief -a android.intent.action.MAIN " +
                "-c android.intent.category.LAUNCHER $safe | tail -1"
        )
        val comp = resolve.output.trim()
        if (resolve.exitCode == 0 && comp.contains("/")) {
            log("Buka game: $comp")
            RootUtil.su("am start -n $comp")
        } else {
            log("Resolve activity gagal — pakai monkey.")
            RootUtil.su("monkey -p $safe -c android.intent.category.LAUNCHER 1")
        }
        for (i in 0 until 20) {
            val pid = RootUtil.pidOf(safe)
            if (pid > 0) {
                log("Game jalan (pid $pid).")
                return true
            }
            Thread.sleep(1000)
        }
        return false
    }

    /** Bawa game ke depan (tanpa inject) — pola Traffic Rider verbatim:
     *  resolve launcher activity lalu `am start -n`. Dipakai alur
     *  LUNCURKAN setelah inject sukses agar user langsung mendarat di
     *  game dengan overlay tampil. */
    fun bringGameToFront() {
        val safe = TARGET_PACKAGE.replace(Regex("[^a-zA-Z0-9_.]"), "")
        if (safe.isEmpty()) return
        val resolve = RootUtil.su(
            "cmd package resolve-activity --brief -a android.intent.action.MAIN " +
                "-c android.intent.category.LAUNCHER $safe | tail -1"
        )
        val comp = resolve.output.trim()
        if (resolve.exitCode == 0 && comp.contains("/")) {
            RootUtil.su("am start -n $comp")
        }
    }

    /** Tulis config toggle ke cache game via root (dibaca payload tiap 500 ms).
     *  Pola TR (printf, bukan heredoc): satu perintah `su -c`, tanpa
     *  baris-baris heredoc yang harus lolos lewat shell persisten. */
    fun writeConfig(configJson: String): Boolean {
        val esc = configJson.replace("'", "'\"'\"'")
        val r = RootUtil.su(
            "D='$GAME_CACHE_DIR'; mkdir -p \"\$D\" && " +
                "printf '%s' '$esc' > '$CONFIG_PATH' && chmod 644 '$CONFIG_PATH'"
        )
        return r.exitCode == 0
    }

    /** Baca data ESP (JSON) dari cache game via root. "" bila tak ada. */
    fun readEspJson(): String {
        val r = RootUtil.su("cat '$ESP_PATH' 2>/dev/null")
        return r.output.trim()
    }

    /** Tulis perintah koin sekali tap ke cache game via root.
     *  Payload (pompa) membacanya tiap 2 dtk, memberikannya lewat
     *  PlayerDataManager.AddCoinsOne, lalu menghapus file. */
    fun writeCoinsCommand(amount: Int): Boolean {
        if (amount <= 0) return false
        val json = "{\"addcoins\":$amount}"
        val r = RootUtil.su(
            "D='$GAME_CACHE_DIR'; mkdir -p \"\$D\" && " +
                "printf '%s' '$json' > '$CMD_PATH' && chmod 644 '$CMD_PATH'"
        )
        return r.exitCode == 0
    }

    /** Baca bukti crash payload (JSON). "" bila tak ada / belum crash. */
    fun readCrashFile(): String {
        val r = RootUtil.su("cat '$CRASH_PATH' 2>/dev/null")
        return r.output.trim()
    }

    /** Hapus bukti crash SETELAH dibaca + dicatat (MainActivity).
     *  Tanpa ini, file basi membuat semua run berikutnya dilaporkan
     *  STATUS=CRASH oleh payload (v2.0.1). */
    fun clearCrashFile() {
        RootUtil.su("rm -f '$CRASH_PATH' 2>/dev/null")
    }

    /**
     * Salin payload ke lokasi yang BISA di-dlopen proses game.
     * Utama: cache game sendiri (chown ke UID game + samakan konteks
     * SELinux + chmod 755). Cadangan: /data/local/tmp.
     *
     * v1.0.5: exit 0 dari rantai copy di atas BELUM membuktikan file
     * ada — tes POCO v1.0.4: staging "sukses" tapi dlopen percobaan 1
     * bilang "not found". Karena itu file DIVERIFIKASI terpisah
     * (test -f + ls -lZ, supaya konteks SELinux-nya ikut tercatat)
     * dan hasilnya masuk diagnosis. Bila verifikasi gagal, kembalikan
     * null — inject() lalu memakai path lib injector untuk percobaan
     * ke-2, bukan membuang satu percobaan untuk path yang tidak ada.
     * @return Pair(path yang terverifikasi atau null, log staging)
     */
    private fun stagePayload(context: Context): Pair<String?, String> {
        // Sumber payload: lib dir bila ada, bila tidak ekstrak dari APK
        // (jaring pengaman yang sama seperti injector — v1.0).
        val src = resolveNativeLib(context, PAYLOAD_LIB)
            ?: return null to "staging gagal: libpayload.so tidak ditemukan di aplikasi"
        val gameStage = RootUtil.su(
            "D='$GAME_CACHE_DIR'; " +
            "mkdir -p \"\$D\" && cp '$src' '$PAYLOAD_GAME' && " +
            // v1.0.8: chown dan chcon MELAPOR TERPISAH (tes v1.0.6: keduanya
            // gagal diam-diam, file staging root-owned + system_data_file
            // → dlopen cache "not found"). UID dari stat; bila stat gagal,
            // turunkan dari ls -lnd kolom ke-3.
            "U=\$(stat -c %u '$GAME_DATA_DIR' 2>/dev/null); " +
            "if [ -z \"\$U\" ]; then U=\$(ls -lnd '$GAME_DATA_DIR' 2>/dev/null | awk '{print \$3}'); fi; " +
            "if [ -n \"\$U\" ]; then chown \"\$U\" '$PAYLOAD_GAME' && echo \"chown: ok (uid \$U)\" || echo 'chown GAGAL'; else echo 'chown DILEWATI (UID tak ketemu)'; fi; " +
            "chmod 755 '$PAYLOAD_GAME'; " +
            // v1.0.8: chcon --reference TIDAK didukung toybox HP
            // ("Unknown option 'reference=...'" di tes v1.0.6) — pakai
            // restorecon (konteks benar incl. kategori dari policy);
            // bila tak ada/gagal, salin konteks dir via ls -Zd + chcon.
            "restorecon '$PAYLOAD_GAME' 2>/dev/null && echo 'restorecon: ok' || " +
            "{ C=\$(ls -Zd \"\$D\" 2>/dev/null | awk '{print \$1}'); " +
            "if [ -n \"\$C\" ]; then chcon \"\$C\" '$PAYLOAD_GAME' && echo \"chcon: ok (\$C)\" || echo 'chcon GAGAL'; else echo 'chcon DILEWATI (konteks dir tak ketemu)'; fi; }; " +
            "ls -l '$PAYLOAD_GAME'"
        )
        val verify = RootUtil.su(
            "ls -lZ '$PAYLOAD_GAME' 2>&1; " +
            "test -f '$PAYLOAD_GAME' && echo STAGE_OK || echo STAGE_MISSING"
        )
        val stageLog = "staging cache game:\n${gameStage.output.trim()}\n" +
            "verifikasi:\n${verify.output.trim()}"
        if (verify.output.contains("STAGE_OK")) return PAYLOAD_GAME to stageLog
        val r = RootUtil.su("cp '$src' '$PAYLOAD_TMP' && chmod 755 '$PAYLOAD_TMP'")
        return if (r.exitCode == 0) PAYLOAD_TMP to (stageLog + "\nfallback /data/local/tmp: OK")
        else null to (stageLog + "\nfallback /data/local/tmp: GAGAL")
    }

    /** Ambil nilai string untuk key pada JSON sederhana {"k":"v",...}
     *  (pola TR ModInjector.jsonVal). */
    private fun jsonVal(json: String, key: String): String {
        val pat = "\"$key\":\""
        val i = json.indexOf(pat)
        if (i < 0) return ""
        val s = i + pat.length
        val e = json.indexOf('"', s)
        return if (e > s) json.substring(s, e) else ""
    }

    /** Ambil nilai angka untuk key pada JSON sederhana {"k":123,...}
     *  (dipakai "signal" — satu-satunya nilai numerik di inject.json). */
    private fun jsonNumVal(json: String, key: String): String {
        val pat = "\"$key\":"
        val i = json.indexOf(pat)
        if (i < 0) return ""
        val s = i + pat.length
        val eComma = json.indexOf(',', s)
        val eBrace = json.indexOf('}', s)
        val e = when {
            eComma < 0 -> eBrace
            eBrace < 0 -> eComma
            else -> minOf(eComma, eBrace)
        }
        return if (e > s) json.substring(s, e).trim() else ""
    }

    /** Baca hasil inject JSON yang ditulis injector (TR). "" bila tak ada. */
    private fun readInjectResultJson(): String {
        val r = RootUtil.su("cat '$INJECT_RESULT' 2>/dev/null")
        return r.output.trim()
    }

    /**
     * Inject + kumpulkan diagnosis. Retry s.d. 3x (native lib → cache
     * game → native lib), cek maps antar percobaan — dlopen percobaan
     * sebelumnya bisa selesai TERLAMBAT, dan dlopen kedua ke payload
     * yang sudah ter-map = crash.
     *
     * v1.0.6: lib-dir injector jadi percobaan PERTAMA — dlopen dari
     * sana TERBUKTI OK di v1.0.4 + v1.0.5, sementara cache game gagal
     * 2x ("not found" karena file staging root-owned + konteks
     * SELinux system_data_file, chown/chcon gagal diam-diam). Cache
     * tetap percobaan ke-2 (fallback bila lib-dir bermasalah).
     *
     * v1.0.4: mekanisme inject PINDAH TOTAL ke kerangka Traffic Rider
     * (injector.c). Injector TR mengembalikan exit 0 bila ia sempat
     * jalan — hasil remote-dlopen ada di file JSON, BUKAN di exit code
     * (pola lama v1.0–v1.0.3: exit 0..9). "ok" = dlopen berhasil;
     * "null"/"crash"/"aborted"/"exited" dipetakan ke kode sintetis
     * 9/10/11/12 supaya MainActivity (exitCode == 0) tak berubah.
     */
    fun inject(context: Context, pid: Int): InjectResult {
        // Pre-flight (pelajaran v1.0): injector HARUS ada sebagai file
        // sebelum dieksekusi. Bila tidak ada, JANGAN panggil 3x buta —
        // kembalikan 127 dengan diagnosis yang menyebut path persisnya.
        val injector = resolveNativeLib(context, INJECTOR_LIB)
        if (injector == null) {
            val ls = RootUtil.su(
                "ls -l '${context.applicationInfo.nativeLibraryDir}' 2>&1"
            ).output.trim()
            return InjectResult(
                127, "",
                "libinjector.so TIDAK ditemukan di nativeLibraryDir dan gagal diekstrak dari APK.\n" +
                    "nativeLibraryDir: ${context.applicationInfo.nativeLibraryDir}\n$ls\n" +
                    "Uninstall aplikasi ini lalu install ulang APK v1.0.1."
            )
        }
        RootUtil.su("chmod 755 '$injector'")
        if (isPayloadLoaded(pid)) {
            return InjectResult(
                0, "",
                "payload sudah ter-load di pid $pid — lewati dlopen, langsung tunggu status"
            )
        }
        val (staged, stageLog) = stagePayload(context)
        val nativePayload = resolveNativeLib(context, PAYLOAD_LIB) ?: payloadPath(context)
        val attempts = arrayOf(nativePayload, staged ?: nativePayload, nativePayload)
        var last: InjectResult? = null
        val allDiag = StringBuilder("injector: $injector (kerangka TR v1.0.6)\n$stageLog")
        // Hapus hasil inject lama supaya percobaan ini tidak membaca
        // JSON basi (pola TR).
        RootUtil.su("rm -f '$INJECT_RESULT'")
        for ((i, path) in attempts.withIndex()) {
            if (i > 0) {
                Thread.sleep(1000)
                if (isPayloadLoaded(pid)) {
                    val d = "payload ter-load (percobaan sebelumnya) — stop retry\n" +
                        (last?.diag ?: "")
                    val full = if (allDiag.isNotEmpty()) "$allDiag\n$d" else d
                    return InjectResult(0, "", full)
                }
            }
            // TR: injector memblokir di waitpid sampai trap dlopen
            // (tanpa timeout internal); timeout shell 30 dtk hanya
            // jaring pengaman — dlopen normal <1 dtk.
            val r = RootUtil.su("'$injector' $pid $path", 30)
            var diag = r.output.trim()
            // Hasil utama: JSON injector (dlopen + dlerror + signal).
            val injJson = readInjectResultJson()
            val dlres = jsonVal(injJson, "dlopen")
            val dlerror = jsonVal(injJson, "dlerror")
            val injSignal = jsonNumVal(injJson, "signal")
            val injHandle = jsonVal(injJson, "handle")
            val injPc = jsonVal(injJson, "pc_after")
            // Petakan status dlopen ke exit code (MainActivity: 0 = sukses).
            val code = when {
                r.output == "TIMEOUT" -> -1
                dlres == "ok" -> 0
                dlres == "null" -> 9
                dlres == "crash" -> 10
                dlres == "aborted" -> 11
                dlres == "exited" -> 12
                r.exitCode != 0 -> r.exitCode
                else -> 1
            }
            if (injJson.isNotEmpty()) {
                diag = "$diag\ninject.json: dlopen=$dlres signal=$injSignal handle=$injHandle pc_after=$injPc" +
                    (if (dlerror.isNotEmpty()) "\ndlerror: $dlerror" else "")
            }
            val ctor = readCtorLog()
            if (ctor.isNotEmpty()) diag = "$diag\nctor log:\n$ctor"
            if (diag.isNotEmpty()) diag = "percobaan ${i + 1}/3 — payload path dipakai: $path\n$diag"
            if (code != 0) {
                val failLine = "percobaan ${i + 1}/3 GAGAL: ${failureReason(code)}"
                diag = if (diag.isNotEmpty()) "$diag\n$failLine" else failLine
            }
            if (allDiag.isNotEmpty()) allDiag.append("\n")
            allDiag.append(diag)
            last = InjectResult(code, r.output, diag)
            if (code == 0) return InjectResult(0, r.output, allDiag.toString())
            // "crash" = mekanisme/target rusak — retry sia-sia (pola TR: break).
            if (dlres == "crash") break
        }
        return InjectResult(
            last?.exitCode ?: 1, last?.output ?: "",
            if (allDiag.isNotEmpty()) allDiag.toString() else "inject gagal tanpa percobaan"
        )
    }
}
