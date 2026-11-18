// Hozin Wild Hunter 3D Injector — payload entry point.
//
// Constructor WAJIB kembali CEPAT: ia hanya menulis ctor log, lalu
// spawn worker thread (wildhunter::start_worker) dan kembali. Semua
// kerja berat (resolve IL2CPP, hook, fitur) jalan di worker thread —
// kalau constructor menahan dlopen, remote call injector (budget
// dlopen 10 dtk sejak v1.0.3) timeout dan MainActivity mengira inject gagal.
//
// Ctor log membuktikan constructor sempat KEMBALI:
//   CTOR=masuk  → constructor mulai
//   CTOR=kembali → constructor selesai (worker jalan)
// Bila file hanya berisi "masuk" (tanpa "kembali"), constructor macet
// di tengah — diagnosis langsung, tanpa logcat.
#include "wildhunter.h"

#include <android/log.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>

#define LOG_TAG "HozinWildHunter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kCtorCache = "/data/data/com.julian.wildhunter/cache/hozinhunter_ctor.log";
constexpr const char* kCtorTmp = "/data/local/tmp/hozinhunter_ctor.log";
// Kanal ctor KETIGA (pola v1.0.6 Sniper): external files game — lihat
// kStatusExt di wildhunter.cpp.
constexpr const char* kCtorExt = "/sdcard/Android/data/com.julian.wildhunter/files/hozinhunter_ctor.log";

void mkdir_p_ext() {
    const char* parts[] = {
        "/sdcard/Android",
        "/sdcard/Android/data",
        "/sdcard/Android/data/com.julian.wildhunter",
        "/sdcard/Android/data/com.julian.wildhunter/files",
    };
    for (const char* p : parts) mkdir(p, 0755);
}

// "masuk" MEMOTONG log lama (truncate) — satu run = satu log bersih.
// Pelajaran Sniper v1.0.7: mode append membuat ctor log terakumulasi
// antar run (CTOR 2x) sehingga tak bisa dibedakan run yang mana.
// "kembali" append ke log yang sama.
void write_ctor(const char* line, bool truncate) {
    std::string content = line;
    content += "\n";
    const char* mode = truncate ? "w" : "a";
    FILE* f = std::fopen(kCtorCache, mode);
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
    f = std::fopen(kCtorTmp, mode);
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
    mkdir_p_ext();
    f = std::fopen(kCtorExt, mode);
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

}  // namespace

__attribute__((constructor)) static void on_load() {
    write_ctor("CTOR=masuk", true);
    LOGI("payload Hozin Wild Hunter 3D v2.2 masuk ke proses game");
    wildhunter::start_worker();
    write_ctor("CTOR=kembali", false);
    LOGI("payload constructor kembali — worker jalan");
}
