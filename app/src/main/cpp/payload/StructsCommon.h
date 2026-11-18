// StructsCommon.h — tipe minimal yang dibutuhkan ESP_3_thnu.h
// (file ESP kiriman user). Ditulis agar header itu ter-compile apa
// adanya; HANYA tipe + operator yang dipakai ESP.h, tanpa tambahan.
//
// Perhatian: class ESP di ESP_3_thnu.h memanggil method DrawLine /
// DrawText / DrawFilledCircle pada sebuah View Android lewat JNI —
// itu hanya bisa jalan di proses yang MEMILIKI View + Canvas tersebut.
// Payload ini jalan di dalam proses GAME, sementara kanvas overlay
// hidup di proses APLIKASI injector (proses berbeda, VM berbeda),
// jadi class ESP tidak bisa dipanggil langsung dari payload. Yang
// dipakai di sini: header itu diverifikasi compile-nya, dan perilaku
// gambarnya (DrawBox 4 garis, DrawHorizontalHealthBar, DrawCrosshair)
// di-port 1:1 ke EspView.kt yang memegang Canvas aslinya. Data posisi
// tetap dihitung payload di dalam game dan dikirim lewat file JSON.
#pragma once

// Color(r, g, b, a) — urutan konstruktor sesuai pemakaian di ESP.h:
// Color(0, 255, 0, 255) = hijau, Color(255, 0, 0, 255) = merah.
struct Color {
    int r, g, b, a;
    Color(int r_, int g_, int b_, int a_) : r(r_), g(g_), b(b_), a(a_) {}
};

struct Vector2 {
    float X, Y;
    Vector2() : X(0), Y(0) {}
    Vector2(float x, float y) : X(x), Y(y) {}
    Vector2 operator-(const Vector2& o) const {
        return Vector2(X - o.X, Y - o.Y);
    }
    Vector2 operator+(const Vector2& o) const {
        return Vector2(X + o.X, Y + o.Y);
    }
    Vector2& operator-=(const Vector2& o) {
        X -= o.X;
        Y -= o.Y;
        return *this;
    }
    Vector2& operator+=(const Vector2& o) {
        X += o.X;
        Y += o.Y;
        return *this;
    }
};

struct Rect {
    float x, y, width, height;
    Rect() : x(0), y(0), width(0), height(0) {}
    Rect(float x_, float y_, float w_, float h_)
        : x(x_), y(y_), width(w_), height(h_) {}
};
