package com.hozinking.wildhunterinjector

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/**
 * Kanvas ESP transparan di atas game — port 1:1 perilaku gambar dari
 * file referensi kiriman user ESP_3_thnu.h (class ESP berbasis
 * Canvas-JNI): DrawLine / DrawText / DrawFilledCircle / DrawBox /
 * DrawHorizontalHealthBar / DrawCrosshair, dengan geometri dan warna
 * yang sama persis (kotak hijau stroke 3, bar HP hitam + isi
 * hijau/kuning/merah, crosshair merah ukuran 20).
 *
 * Kenapa di-port ke Kotlin, bukan memanggil class ESP itu langsung:
 * class ESP memanggil method JNI pada sebuah View Android — View itu
 * harus hidup di proses yang sama. Payload jalan di dalam proses GAME,
 * sementara kanvas ini hidup di proses APLIKASI injector (proses
 * berbeda, VM berbeda), jadi JNI lintas proses tidak mungkin. Jalur
 * datanya tetap file JSON: payload menghitung posisi layar + HP di
 * dalam game, aplikasi menggambarnya di sini. File ESP_3_thnu.h
 * disalin verbatim ke payload (app/src/main/cpp/payload/) sebagai
 * referensi yang terverifikasi compile.
 */
class EspView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    data class Target(
        val x: Float, val y: Float, val w: Float, val h: Float,
        val hpRatio: Float, val name: String, val dist: Float
    )

    @Volatile
    var targets: List<Target> = emptyList()

    // Satu Paint per gaya gambar — warna/ketebalan disetel tiap
    // panggilan, mengikuti argumen (a, r, g, b) ala ESP.h.
    private val strokePaint = Paint().apply {
        style = Paint.Style.STROKE
        isAntiAlias = true
    }
    private val fillPaint = Paint().apply {
        style = Paint.Style.FILL
        isAntiAlias = true
    }
    private val textPaint = Paint().apply {
        isAntiAlias = true
        setShadowLayer(3f, 0f, 0f, Color.BLACK)
    }

    // ESP.DrawLine(color, thickness, start, end).
    private fun drawLine(
        canvas: Canvas, a: Int, r: Int, g: Int, b: Int, thickness: Float,
        startX: Float, startY: Float, endX: Float, endY: Float
    ) {
        strokePaint.color = Color.argb(a, r, g, b)
        strokePaint.strokeWidth = thickness
        canvas.drawLine(startX, startY, endX, endY, strokePaint)
    }

    // ESP.DrawText(color, txt, pos, size).
    private fun drawText(
        canvas: Canvas, a: Int, r: Int, g: Int, b: Int, txt: String,
        x: Float, y: Float, size: Float
    ) {
        textPaint.color = Color.argb(a, r, g, b)
        textPaint.textSize = size
        canvas.drawText(txt, x, y, textPaint)
    }

    // ESP.DrawFilledCircle(color, pos, radius).
    private fun drawFilledCircle(
        canvas: Canvas, a: Int, r: Int, g: Int, b: Int,
        x: Float, y: Float, radius: Float
    ) {
        fillPaint.color = Color.argb(a, r, g, b)
        canvas.drawCircle(x, y, radius, fillPaint)
    }

    // ESP.DrawBox(color, stroke, rect) — 4 garis.
    private fun drawBox(
        canvas: Canvas, a: Int, r: Int, g: Int, b: Int, stroke: Float,
        left: Float, top: Float, w: Float, h: Float
    ) {
        drawLine(canvas, a, r, g, b, stroke, left, top, left + w, top)
        drawLine(canvas, a, r, g, b, stroke, left + w, top, left + w, top + h)
        drawLine(canvas, a, r, g, b, stroke, left + w, top + h, left, top + h)
        drawLine(canvas, a, r, g, b, stroke, left, top + h, left, top)
    }

    // ESP.DrawHorizontalHealthBar(screenPos, width, maxHealth,
    // currentHealth) — screenPos = sudut kiri-atas kotak. Bar digambar
    // 8 px DI ATAS kotak: kotak hitam stroke 3 (lebar+2 x 5), isi
    // digeser (+1, +1) setinggi 3 px. Warna isi: hijau >60%, kuning
    // 30–60%, merah <30%.
    private fun drawHorizontalHealthBar(
        canvas: Canvas, x: Float, y: Float, width: Float, hpRatio: Float
    ) {
        val barY = y - 8f
        drawBox(canvas, 255, 0, 0, 0, 3f, x, barY, width + 2f, 5f)
        val ratio = hpRatio.coerceIn(0f, 1f)
        val (r, g, b) = when {
            ratio > 0.6f -> Triple(0, 255, 0)
            ratio > 0.3f -> Triple(255, 255, 0)
            else -> Triple(255, 0, 0)
        }
        fillPaint.color = Color.argb(255, r, g, b)
        canvas.drawRect(x + 1f, barY + 1f, x + 1f + width * ratio, barY + 1f + 3f, fillPaint)
    }

    // ESP.DrawCrosshair(clr, center, size = 20) — 2 garis tebal 3.
    private fun drawCrosshair(
        canvas: Canvas, a: Int, r: Int, g: Int, b: Int,
        centerX: Float, centerY: Float, size: Float = 20f
    ) {
        val x = centerX - size / 2f
        val y = centerY - size / 2f
        drawLine(canvas, a, r, g, b, 3f, x, centerY, x + size, centerY)
        drawLine(canvas, a, r, g, b, 3f, centerX, y, centerX, y + size)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        drawCrosshair(canvas, 255, 255, 0, 0, width / 2f, height / 2f)

        for (t in targets) {
            val left = t.x - t.w / 2f
            val top = t.y - t.h / 2f
            // Kotak hijau (ESP.DrawBox, stroke 3).
            drawBox(canvas, 255, 0, 255, 0, 3f, left, top, t.w, t.h)
            // Bar HP di atas kotak (ESP.DrawHorizontalHealthBar).
            drawHorizontalHealthBar(canvas, left, top, t.w, t.hpRatio)
            // Nama + jarak di atas bar (ESP.DrawText, putih, 30).
            drawText(
                canvas, 255, 255, 255, 255,
                "${t.name} ${t.dist.toInt()}m", left, top - 8f - 6f, 30f
            )
            // Titik kepala (ESP.DrawFilledCircle, hijau, r=4) — posisi
            // kepala hasil proyeksi, titik yang dibidik aimbot.
            drawFilledCircle(canvas, 255, 0, 255, 0, t.x, t.y, 4f)
        }
    }
}
