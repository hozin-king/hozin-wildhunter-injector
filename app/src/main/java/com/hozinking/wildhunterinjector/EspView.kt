package com.hozinking.wildhunterinjector

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/**
 * Kanvas ESP transparan di atas game — primitif gambar yang sama dengan
 * file referensi ESP_3_thnu.h (kotak, bar HP hijau/kuning/merah, teks
 * nama+jarak, crosshair), hanya saja digambar dari Kotlin di proses
 * aplikasi (bukan JNI dari dalam game): data musuh (posisi layar, HP,
 * jarak) dihitung payload di dalam game dan dikirim lewat file JSON
 * bersama di cache game. Referensi JNI CanvasView butuh DexClassLoader
 * ke proses game; jalur file ini lebih sederhana dan sama hasilnya.
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

    private val boxPaint = Paint().apply {
        color = Color.GREEN
        style = Paint.Style.STROKE
        strokeWidth = 3f
        isAntiAlias = true
    }
    private val barBgPaint = Paint().apply {
        color = Color.BLACK
        style = Paint.Style.FILL
    }
    private val barPaint = Paint().apply {
        style = Paint.Style.FILL
    }
    private val textPaint = Paint().apply {
        color = Color.WHITE
        textSize = 30f
        isAntiAlias = true
        setShadowLayer(3f, 0f, 0f, Color.BLACK)
    }
    private val crossPaint = Paint().apply {
        color = Color.RED
        style = Paint.Style.STROKE
        strokeWidth = 3f
        isAntiAlias = true
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        // Crosshair tengah layar (sama seperti DrawCrosshair referensi).
        val cx = width / 2f
        val cy = height / 2f
        val s = 20f
        canvas.drawLine(cx - s, cy, cx + s, cy, crossPaint)
        canvas.drawLine(cx, cy - s, cx, cy + s, crossPaint)

        for (t in targets) {
            val left = t.x - t.w / 2f
            val top = t.y - t.h / 2f
            canvas.drawRect(left, top, left + t.w, top + t.h, boxPaint)
            // Bar HP di atas kotak (hijau >60%, kuning 30–60%, merah <30%).
            val barW = t.w
            val barH = 6f
            val barY = top - 10f - barH
            canvas.drawRect(left - 1, barY - 1, left + barW + 1, barY + barH + 1, barBgPaint)
            val ratio = t.hpRatio.coerceIn(0f, 1f)
            barPaint.color = when {
                ratio > 0.6f -> Color.GREEN
                ratio > 0.3f -> Color.YELLOW
                else -> Color.RED
            }
            canvas.drawRect(left, barY, left + barW * ratio, barY + barH, barPaint)
            canvas.drawText(
                "${t.name} ${t.dist.toInt()}m",
                left, barY - 6f, textPaint
            )
        }
    }
}
