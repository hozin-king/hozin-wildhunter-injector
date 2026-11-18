# Hozin Wild Hunter 3D Injector v2.0.1

Root injector (Kotlin + payload native) untuk **Wild Hunter 3D**
(`com.julian.wildhunter`) **v1.0.16 / code 17**, Unity 2022.3.62f2 IL2CPP,
metadata v31. Game **tidak dibundel** — user install sendiri dari Play
Store. Gameplay TERBUKTI full offline (nol network/PvP), jadi injector
ini **TANPA gate mode**.

Kebal update + kebal beda versi: semua class/method/field IL2CPP
di-resolve runtime berdasarkan NAMA — TANPA offset hardcode, TANPA
lock versi. Bila game update dan sebuah nama hilang, payload melaporkan
`PARTIAL`/`UNSUPPORTED` + nama yang gagal ke file status — fitur yang
gagal nonaktif, bukan toggle palsu.

## Fitur (semua default OFF)

1. **Aimbot / aimlock** — hook `PlayerCamera.get_ScreenCenter` (dipakai
   `Weapon.Shoot` / `SpecialShoot` untuk menghitung arah ray). Bila
   aimbot aktif dan target segar (<300 ms), hook mengembalikan posisi
   layar kepala hewan terdekat dari tengah layar (enumerasi `Animal` →
   `HeadTransform` → `Camera.main.WorldToScreenPoint`). Kalau tidak,
   original dipanggil. Spread game ditambahkan DI ATAS titik itu, jadi
   aimbot paling akurat bila no-recoil ikut ON.
2. **Auto-aim saat scope** — aimbot yang sama, aktif hanya saat
   `Weapon.mIsScope` true.
3. **ESP** — overlay Canvas di proses aplikasi (kotak hijau, bar HP
   `mCurrentHealth`/`mHealth`, nama + jarak, crosshair). Data posisi
   dihitung payload lewat `Camera.main.WorldToScreenPoint`, dikirim lewat
   file JSON di cache game. Primitif gambar mengikuti referensi
   `ESP_3_thnu.h`.
4. **No recoil** — `WeaponData.maxRecoil` + `Shake` + `shock` = 0, dan
   `maxReticleRadius` disamakan dengan `minReticleRadius` (bidik tak
   mengembang). Nilai asli disimpan + dikembalikan saat toggle OFF.
5. **No reload** — `Weapon.bulletsNum` ditulis 99 SETELAH orig
   `Shoot`/`SpecialShoot` mengurangi (post-shot, di hook tembakan).
6. **Zoom jauh** — `Sight.mPower` = `mMaxPower` + `Sight.mZoomAmount` = 30.
7. **Energi tak habis** — static `PlayerDataManager.currentPlayerEnergy`
   ditahan di 15 (`Max_Energy`) tiap tick.
8. **Tambah Koin +1000** — tombol sekali tap (bukan toggle): payload
   memanggil static `PlayerDataManager.AddCoinsOne(1000)` di main thread.
   Koin gameplay, bukan IAP gratis.

Fitur aktif saat berburu stabil (instance `Weapon` yang sama sudah
terlihat ≥ 2000 ms) — selama transisi scene, tick tidak mengerjakan
apa-apa (pelajaran crash loading-ingame Sniper v1.0.5/v1.0.7:
FindObjectsOfType di tengah transisi tersangka utama). **v2.0 REWRITE
TOTAL ke kerangka Traffic Rider** (perintah user 18 Nov 08:34 setelah
v1.0 crash saat masuk berburu): TIDAK ADA hook Update generik sebagai
pompa, TIDAK ADA FindObjectsOfType dari thread mana pun. Driver =
hook `Weapon.Update` (tangkap instance, kerja inline); ESP dari
registry statis `ListedUnity<Animal>.mAll`.

## Yang sengaja TIDAK dibangun

IAP gratis (termasuk flag `debugFreeIap`), unlock berbayar, thermal
gratis, pemalsuan hadiah iklan, ubah jam buat farm bonus,
speed/teleport/noclip. Klasifikasi lengkap di
`~/workspace/rea-wildhunter/AUDIT-INJECTOR.md`.

## Teknis

- **Tanpa offset hardcoded.** Offset arm64 dari REA (`Weapon.bulletsNum`
  0x84, `Animal.HeadTransform` 0x40, `mCurrentHealth` 0x168,
  `Sight.mZoomAmount` 0x18, `PlayerCamera.mMaxFov` 0x304 — lihat
  `~/workspace/rea-wildhunter/REPORT-ARM64.md`) hanya referensi
  verifikasi; resolve runtime lewat nama field.
- **Driver main-thread (kerangka TR)**: hook `Weapon.Update` menangkap
  instance lalu tick mengerjakan fitur inline (target+ESP dari registry
  `ListedUnity<Animal>.mAll`, no-recoil/zoom/energi, perintah koin).
  Semua API Unity HANYA dari hook main-thread. Worker thread hanya
  baca config + tulis status (tanpa API Unity). Guard stub: instruksi
  pertama RET/B → jangan hook.
- **Dobby** (hook) commit `5dfc854` + 5 patch NDK modern (di
  `CMakeLists.txt`).
- **Injector**: ptrace remote dlopen (arm64, kerangka Traffic Rider),
  payload di-stage ke cache game sendiri (bukan `/data/local/tmp` —
  SELinux sering menolak dlopen dari sana), retry 3x, tangkap `dlerror()`
  remote. Percobaan pertama = lib dir injector (terbukti dlopen OK),
  kedua = cache game, ketiga = lib dir lagi.
- **Diagnosis in-app**: file status payload (`STATUS`/`FAILED`/`LIBSIZE`/
  `MODE`/`HOOK`/`PUMP`) + ctor log (truncate per run) + bukti crash
  terpisah `hozinhunter_crash.json` (TIDAK ditimpa worker — pelajaran
  Sniper v1.0.7) + tombol salin diagnosis + file log run di
  `Android/data/com.hozinking.wildhunterinjector/files/logs/`. Jangan
  suruh user jalankan Termux/logcat.
- **Standar 16 Nov**: game auto-launch bila belum jalan; tombol
  mengambang `🏹` + menu selalu tampil apa pun hasil inject; panel menu
  bisa digeser dari baris judul.
- **Keystore persisten**: `debug.keystore` di repo TIDAK di-push
  (`.gitignore`); CI memulihkannya dari secret `DEBUG_KEYSTORE_BASE64`.
  `signingConfigs.debug` kondisional — fallback ke keystore ephemeral
  bila file tak ada.

## Risiko (jujur)

- **Tanpa jaminan anti-ban.** Game ini full offline tanpa anti-cheat
  yang terdeteksi di REA, tapi tidak ada jaminan 100%.
- **v1.0 menulis ulang alur root + launch mengikuti kerangka Traffic
  Rider** (`ModInjector.java` + `RootUtil.java`, teruji di POCO user):
  tiap perintah root lewat `su -c` baru dengan timeout keras, game
  diluncurkan DULUAN sebelum semua tunggu yang lama, dan dibawa ke
  depan hanya bila inject sukses.
- **Manifest menyetel `extractNativeLibs=true`** (pelajaran Sniper
  v1.0: tanpa itu `.so` tidak diekstrak → exit 127) + jaring pengaman
  ekstraksi dari APK.
- Hook `get_ScreenCenter` adalah bagian paling berisiko (return Vector2
  HFA + dipakai juga `SightReticle.CaluAngel` — reticle ikut menunjuk
  kepala saat aimbot ON). Bila hook gagal, status payload menunjukkan
  `HOOK=gagal` dan aimbot nonaktif — fitur lain tetap bisa jalan.

## Build

APK **hanya** di GitHub Actions: push ke `main` → build → tag + rilis otomatis
(`HozinWildHunterInjector-v2.0.1.apk`).

## Riwayat versi
- **v2.0.1** — perbaikan init v2.0: loop retry load+resolve s.d. 90 dtk
  (v2.0 resolve sekali tepat saat .so muncul, padahal domain IL2CPP
  belum siap → semua class NULL → hook tak pernah dipasang);
  diagnostik `INIT_STEP` + `ADDRS` di file status; retry pasang hook;
  injector tunggu status final s.d. 60 dtk; bukti crash dihapus
  setelah dibaca (anti STATUS=CRASH basi).
- **v2.0** — rewrite total ke kerangka Traffic Rider.

## Struktur

```
app/src/main/java/com/hozinking/wildhunterinjector/
  MainActivity.kt      # alur inject otomatis + diagnosis
  RootUtil.kt          # root ala Traffic Rider: `su -c` baru + timeout
  InjectorBridge.kt    # staging, inject, status, config, ESP, koin (via root)
  FloatingService.kt   # tombol 🏹 + menu 7 toggle + tombol koin + polling ESP
  EspView.kt           # kanvas ESP (kotak, bar HP, teks, crosshair)
app/src/main/cpp/
  CMakeLists.txt       # Dobby FetchContent + 5 patch
  injector/            # ptrace remote dlopen (libinjector.so)
  payload/             # payload IL2CPP (libpayload.so)
    il2cpp_api.h       # deklarasi minimal IL2CPP
    wildhunter.h/.cpp  # resolve, pompa, hook aimbot, fitur, ESP
    payload_main.cpp   # constructor cepat + ctor log
```
