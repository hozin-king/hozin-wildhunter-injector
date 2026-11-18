package com.hozinking.wildhunterinjector

import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * Helper root ala Traffic Rider (RootUtil.java — TERBUKTI jalan di POCO
 * user): tiap perintah dijalankan lewat `su -c` BARU, dengan TIMEOUT
 * keras, stream dibaca thread terpisah, stderr digabung ke stdout.
 *
 * Menggantikan RootShell (v1.0–v1.0.1): SATU shell `su` persisten dengan
 * protokol marker DAN TANPA TIMEOUT SAMA SEKALI. Bila shell itu macet /
 * mati diam-diam / marker hilang, `readLine()` menggantung selamanya dan
 * seluruh alur inject membeku — log user 20:19 berhenti di
 * "root: available" tanpa baris apa pun sesudahnya, persis gejala ini.
 * Thread polling ESP (tiap 150 ms) juga berbagi shell yang sama lewat
 * @Synchronized, jadi ikut antre di belakang perintah yang lama (scan
 * /proc di findPid bisa makan beberapa detik).
 *
 * Dengan pola ini tidak ada perintah yang bisa menggantung alur:
 * paling buruk ia kembali "TIMEOUT" dalam ≤15 detik dan alur lanjut /
 * gagal secara TERLIHAT, bukan membeku diam-diam.
 */
object RootUtil {

    data class Result(val output: String, val exitCode: Int)

    /** Jalankan perintah sebagai root. Kembalikan stdout + exit code.
     *  Timeout -> output "TIMEOUT", code -1 (pemanggil membedakan dari
     *  output kosong). Stream dibaca thread terpisah agar output besar
     *  tidak deadlock saat proses menunggu giliran baca. */
    fun su(cmd: String, timeoutSecs: Int = 15): Result {
        val sb = StringBuilder()
        var code = -1
        var p: Process? = null
        try {
            p = ProcessBuilder("su", "-c", cmd)
                .redirectErrorStream(true).start()
            val proc = p
            val reader = Thread {
                try {
                    val r = BufferedReader(InputStreamReader(proc.inputStream))
                    var line: String?
                    while (r.readLine().also { line = it } != null) {
                        synchronized(sb) { sb.append(line).append('\n') }
                    }
                    r.close()
                } catch (_: Exception) {
                    // stream ditutup paksa
                }
            }
            reader.start()
            val done = proc.waitFor(
                timeoutSecs.toLong(), java.util.concurrent.TimeUnit.SECONDS
            )
            if (!done) {
                try { proc.destroyForcibly() } catch (_: Exception) {}
                try { reader.join(1000) } catch (_: InterruptedException) {}
                return Result("TIMEOUT", -1)
            }
            try { reader.join(2000) } catch (_: InterruptedException) {}
            code = proc.exitValue()
        } catch (e: Exception) {
            synchronized(sb) { sb.append("EXC: ").append(e.message) }
            if (p != null) {
                try { p.destroyForcibly() } catch (_: Exception) {}
            }
        }
        synchronized(sb) { return Result(sb.toString().trim(), code) }
    }

    /** true bila proses pid masih ada (kill -0 sebagai root). Dipakai
     *  waitForGameReady: pidof bisa mengembalikan PID lain bila game
     *  punya >1 proses — keberadaan PID spesifik lebih bisa dipegang. */
    fun pidAlive(pid: Int): Boolean {
        if (pid <= 0) return false
        val r = su("kill -0 $pid 2>/dev/null", 8)
        return r.exitCode == 0
    }

    /** true bila `needle` muncul di /proc/<pid>/maps. Dipakai cek
     *  libil2cpp.so ter-map + payload sudah ter-load. */
    fun mapsContains(pid: Int, needle: String): Boolean {
        if (pid <= 0) return false
        val r = su("grep -c '$needle' /proc/$pid/maps 2>/dev/null", 8)
        if (r.exitCode != 0 || r.output.isEmpty() || r.output == "TIMEOUT") return false
        return try {
            r.output.trim().split(Regex("\\s+"))[0].toInt() > 0
        } catch (_: Exception) {
            false
        }
    }

    fun isRooted(): Boolean {
        val r = su("id")
        return r.exitCode == 0 && r.output.contains("uid=0")
    }

    /** PID dari package name, atau -1 bila tidak jalan / tidak ketemu.
     *  Timeout 8 dtk: pidof dipanggil tiap poll — su yang macet tidak
     *  boleh menahan pemanggil. */
    fun pidOf(pkg: String): Int {
        val r = su("pidof $pkg", 8)
        if (r.exitCode != 0 || r.output.isEmpty() || r.output == "TIMEOUT") return -1
        return try {
            r.output.split(Regex("\\s+"))[0].toInt()
        } catch (_: Exception) {
            -1
        }
    }
}
