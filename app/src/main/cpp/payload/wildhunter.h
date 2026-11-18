// Hozin Wild Hunter 3D Injector — logika payload (di dalam proses game).
//
// v2.0 REWRITE TOTAL ke kerangka Traffic Rider (perintah user 18 Nov
// 08:34, setelah v1.0 crash saat masuk berburu): hook Weapon.Update
// sebagai driver per-frame (tangkap instance, kerja inline), hook
// Weapon.Shoot/SpecialShoot (aimbot + no-reload), hook
// PlayerCamera.get_ScreenCenter (redirect ray aimbot). ESP dari registry
// statis ListedUnity<Animal>.mAll — TANPA FindObjectsOfType, TANPA
// pompa Update generik. Worker hanya baca config + tulis status.
//
// Fitur (semua default OFF; game full offline — TANPA gate mode):
//   aimbot/aimlock, auto-aim saat scope, ESP, no recoil, no reload,
//   zoom jauh, energi tak habis, tambah koin (tombol sekali tap).
// Semua class/method/field IL2CPP di-resolve runtime berdasarkan NAMA —
// TANPA offset hardcode, TANPA lock versi. Bila game update dan sebuah
// nama hilang, resolve gagal dan payload melaporkan PARTIAL/UNSUPPORTED
// (fitur yang gagal nonaktif, bukan toggle palsu), bukan crash.
//
// Aimbot: hook PlayerCamera.get_ScreenCenter (dipakai Weapon.Shoot /
// SpecialShoot untuk menghitung arah ray). Hook mengembalikan posisi
// layar kepala target bila aimbot aktif dan target masih segar; kalau
// tidak, original dipanggil. Spread game ditambahkan DI ATAS titik itu
// (lihat disassembly Shoot), jadi aimbot paling akurat bila no-recoil
// ikut ON (reticle mengecil).
#pragma once

namespace wildhunter {

// Dipanggil sekali dari constructor payload (harus kembali CEPAT —
// semua kerja berat jalan di worker thread sendiri).
void start_worker();

}  // namespace wildhunter
