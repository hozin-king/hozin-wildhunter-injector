// Hozin Wild Hunter 3D Injector — payload v2.0.1 (KERANGKA TRAFFIC RIDER).
//
// v2.0.1 — PERBAIKAN INIT (log tes POCO v2.0 run 09:02: STATUS=INIT,
// keempat hook gagal, PUMP=0, fitur mati total). AKAR: v2.0 menunggu
// libil2cpp.so muncul di /proc/self/maps lalu resolve SEKALI — padahal
// ".so ter-map" BELUM berarti domain IL2CPP siap (il2cpp_init +
// assembly selesai beberapa detik SESUDAH mapping); resolve yang jalan
// di jendela itu menemukan SEMUA class NULL -> UNSUPPORTED -> hook
// tak pernah dipasang, tanpa retry. v1.0 TIDAK kena ini karena
// load+resolve diulang tiap 500 ms s.d. 60 dtk (terbukti READY).
// Perbaikan: kembalikan loop retry (s.d. 90 dtk) + diagnostik granular
// INIT_STEP + ADDRS di file status + retry pasang hook tiap 2 dtk +
// penangkap crash dipasang di start_worker (sebelum thread dibuat).
// Arsitektur TR v2.0 TIDAK berubah (driver Weapon.Update, registry
// mAll, tanpa FindObjectsOfType, tanpa pompa Update generik).
//
// REWRITE TOTAL dari v1.0 (perintah user 18 Nov 08:34, setelah v1.0
// crash saat masuk berburu): v1.0 memakai pompa main-thread via hook
// PlayerCamera.Update + enumerasi FindObjectsOfType berkala dari
// worker/pompa — arsitektur yang SAMA dengan crash loading-ingame
// Sniper v1.0.5–v1.0.8. Di v2.0 TIDAK ADA hook Update generik sebagai
// pompa dan TIDAK ADA FindObjectsOfType dari thread mana pun.
//
// Pola TR (lihat ~/workspace/hozin-trafficrider-v13/.../payload.cpp):
//   - Hook method gameplay SPESIFIK yang menangkap instance dari `thiz`
//     lalu hook ITU SENDIRI yang mengerjakan fitur inline.
//   - Weapon.Update (tiap frame saat berburu) = driver: tangkap Weapon,
//     panggil orig, lalu tick (target+ESP, fitur, koin).
//   - Weapon.Shoot / SpecialShoot = aimbot (target sudah dihitung tick)
//     + no-reload (tulis bulletsNum=99 SETELAH orig mengurangi).
//   - PlayerCamera.get_ScreenCenter = redirect ray aimbot ke kepala.
//   - ESP: baca registry STATIS game ListedUnity<Animal>.mAll (List,
//     dipelihara game sendiri via Awake/OnDestroy) — BUKAN enumerasi.
//     Proyeksi + tulis JSON dilakukan di dalam tick (main thread).
//   - Worker thread HANYA: baca config + tulis status. TIDAK ADA API
//     Unity dari worker. Semua hook dipasang SEKALI dari init thread
//     setelah resolve (ala TR), bukan retry berkala dari worker.
//   - Liveness: SEBELUM panggilan Unity apa pun atas objek, cek
//     UnityEngine.Object.m_CachedPtr (0x10) == 0 berarti hancur —
//     lewati (pelajaran tombstone TR v1.4).
//
// Game: Wild Hunter 3D (com.julian.wildhunter) v1.0.16 / code 17,
// Unity 2022.3.62f2 IL2CPP, metadata v31 — gameplay TERBUKTI full
// offline (nol network/PvP), jadi TANPA gate mode. Semua class game
// di namespace global ("").
//
// Pola verifikasi dari dump arm64 (REPORT-ARM64.md):
//   - Weapon.bulletsNum 0x84, mIsScope 0x80, mWeaponData 0x90,
//     mSight 0xF0, damge 0xE8. Shoot 0x8f2f2c: kurangi bulletsNum,
//     get_ScreenCenter -> ScreenPointToRay -> RaycastAll -> NotifyHit.
//   - Animal.HeadTransform 0x40, mCurrentHealth 0x168, mHealth 0x16C,
//     mAnimalALive 0xFA, mAnimalData 0x150.
//   - Animal : ListedUnity<Animal> — registry statis mAll (List<T>).
//   - Sight.mZoomAmount 0x18, mPower 0x1C, mMaxPower 0x14.
//   - WeaponData.maxRecoil 0x68, Shake 0x6C, shock 0x70,
//     minReticleRadius 0xB4, maxReticleRadius 0xB8.
//   - PlayerCamera.get_ScreenCenter 0x95a430; Camera.WorldToScreenPoint
//     0x119ccc8; Camera.get_main 0x119d374.
//   - PlayerDataManager.currentPlayerEnergy (static int, Max 15),
//     AddCoinsOne(int) static.
//   - Offset/RVA di atas HANYA referensi; resolve runtime lewat nama
//     field/method IL2CPP — TANPA hardcode offset/alamat, TANPA lock
//     versi.
#include "wildhunter.h"

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "dobby.h"
#include "il2cpp_api.h"

#define LOG_TAG "hozinhunter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ------------------------------------------------------------------ paths
// Semuanya di cache game (konteks app_data_file milik game — proses
// game bisa menulisnya; aplikasi membaca via root). Prefix hozinhunter
// supaya tidak bentrok dengan payload injector lain.
static constexpr const char* kCfgPath =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_config.json";
static constexpr const char* kEspPath =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_esp.json";
static constexpr const char* kEspTmpPath =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_esp.json.tmp";
static constexpr const char* kCmdPath =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_cmd.json";
static constexpr const char* kStatusTmp =
    "/data/local/tmp/hozinhunter_payload.log";
static constexpr const char* kStatusCache =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_payload.log";
static constexpr const char* kStatusExt =
    "/sdcard/Android/data/com.julian.wildhunter/files/hozinhunter_payload.log";
// Bukti crash (file TERPISAH — pelajaran Sniper v1.0.7: STATUS=CRASH di
// file status DITIMPA worker tiap 2 dtk, jadi bukti crash hilang.
// File ini ditulis penangkap crash dan TIDAK PERNAH ditulis ulang
// worker; worker hanya membaca keberadaannya supaya status tetap CRASH).
static constexpr const char* kCrashPath =
    "/data/data/com.julian.wildhunter/cache/hozinhunter_crash.json";

// Vector2 Unity (x, y) — return get_ScreenCenter; tidak ada di
// il2cpp_api.h.
struct Vector2 {
    float x, y;
};

// ------------------------------------------------------------- config
// Ditulis aplikasi (InjectorBridge.writeConfig), dibaca worker tiap
// 500 ms. Semua fitur default OFF.
struct Config {
    bool aimbot = false;   // "aimbot"
    bool zoomAim = false;  // "zoomaim" — aimbot hanya saat scope aktif
    bool esp = false;      // "esp"
    bool noRecoil = false; // "norecoil"
    bool noReload = false; // "noreload"
    bool zoomFar = false;  // "zoom"
    bool energy = false;   // "energy"
};

// ------------------------------------------------------------- target
// Target aimbot + entri ESP. Diisi tick (main thread, hook
// Weapon.Update), dibaca hook aimbot + writer ESP — semua akses di
// bawah lock.
struct CachedTarget {
    Vector3 headPos{0, 0, 0};
    Vector3 camPos{0, 0, 0};
    float screenX = 0, screenY = 0;
    long long timestampMs = 0;
    bool valid = false;
};
struct EspEntry {
    float x = 0, y = 0, w = 0, h = 0, hp = 1, dist = 0;
    std::string name;
};

// ------------------------------------------------------------- globals
namespace {

std::mutex g_mu;
Config g_config;          // config terakhir (dibaca worker)
CachedTarget g_target;    // target aimbot (ditulis tick)
std::vector<EspEntry> g_entries;  // entri ESP (ditulis tick)
bool g_isScope = false;   // Weapon.mIsScope (ditulis tick)
int g_screenW = 0, g_screenH = 0;  // ukuran layar (ditulis tick)

std::vector<std::string> g_failedList;  // nama yang gagal resolve/hook
bool g_coreResolved = false;
std::string g_statusOverride;  // "INIT" sebelum resolve selesai
std::string g_initStep;  // v2.0.1: langkah init terakhir (diagnosis INIT macet di mana)
bool g_hookUpdate = false;     // hook Weapon.Update terpasang (driver)
bool g_hookShoot = false;      // hook Weapon.Shoot terpasang
bool g_hookSpecialShoot = false;  // hook Weapon.SpecialShoot terpasang
bool g_hookCenter = false;     // hook get_ScreenCenter terpasang (aimbot)
// v2.0.1: alamat method hasil resolve (0 = resolve gagal) — bedakan
// "resolve gagal" dari "Dobby gagal" di file status.
uintptr_t g_addrUpdate = 0;
uintptr_t g_addrShoot = 0;
uintptr_t g_addrSpecial = 0;
uintptr_t g_addrCenter = 0;
// Jejak tick (diagnosis: apakah driver main-thread masih jalan).
std::atomic<long long> g_lastTickMs{0};
std::atomic<long long> g_tickCount{0};
void* g_weaponInst = nullptr;  // Weapon aktif (ditangkap hook)
long long g_huntingSinceMs = 0;  // sejak kapan Weapon yang sama terlihat
long long g_lastEspWriteMs = 0;  // throttle tulis JSON ESP (200 ms)
long long g_lastCmdCheckMs = 0;  // cek perintah koin tiap 2 dtk
bool g_espFileOff = false;  // file ESP sudah berisi {"t":[]} (OFF)

struct WeaponDataOrig {
    float maxRecoil = 0, shake = 0, shock = 0;
    float minReticle = 0, maxReticle = 0;
    bool saved = false;
};
WeaponDataOrig g_wdOrig;
struct SightOrig {
    float power = 0, zoomAmount = 0;
    bool saved = false;
};
SightOrig g_sightOrig;
Config g_prevTickConfig;

// Simpan orig fungsi yang di-hook (key = methodPointer agar hook
// multi-instance tidak menimpa orig).
typedef void (*UpdateFn)(void* thiz, void* method);
typedef void (*ShootFn)(void* thiz, void* method);
typedef Vector2 (*ScreenCenterFn)(void* thiz, void* method);
std::map<void*, UpdateFn> g_origUpdate;
std::map<void*, ShootFn> g_origShoot;
std::map<void*, ScreenCenterFn> g_origScreenCenter;

// Il2CppMethodOpaque dengan methodPointer di offset 0 (lihat
// il2cpp_api.h) — dipakai untuk menunjuk orig yang benar.
void* method_ptr(Il2CppMethodOpaque* m) {
    return m ? m->methodPointer : nullptr;
}

// Stempel waktu (ms sejak boot).
long long now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void write_file(const char* path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) out << content;
}

// Tulis atomik (tmp + rename) supaya pembaca file (Kotlin, poll tiap
// 150 ms) tidak pernah membaca JSON setengah tertulis.
void write_file_atomic(const char* path, const char* tmpPath,
                       const std::string& content) {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << content;
    out.close();
    if (out) rename(tmpPath, path);
}

// Tulis status payload ke tiga kanal (tmp, cache game, external game).
// Channel pertama yang bisa ditulis akan dipakai; Kotlin membaca
// kanal mana saja yang berisi data.
// Bila file crash ada (penangkap crash sudah jalan), status DIPERTAHANKAN
// CRASH — worker TIDAK menimpa bukti crash (pelajaran Sniper v1.0.7).
void write_status() {
    struct stat st;
    const bool crashed = ::stat(kCrashPath, &st) == 0;
    std::string failedStr;
    for (size_t i = 0; i < g_failedList.size(); i++) {
        if (i) failedStr += ",";
        failedStr += g_failedList[i];
    }
    std::string s = "STATUS=";
    if (!g_statusOverride.empty()) {
        s += g_statusOverride;
    } else if (crashed) {
        s += "CRASH";
    } else {
        s += g_coreResolved ? (g_failedList.empty() ? "READY" : "PARTIAL")
                            : "UNSUPPORTED";
    }
    s += "\nFAILED=" + (failedStr.empty() ? std::string("-") : failedStr);
    // Ukuran file payload sendiri (diagnosis) via dladdr + stat.
    s += "\nLIBSIZE=";
    {
        Dl_info info{};
        struct stat st2 {};
        if (dladdr(reinterpret_cast<void*>(&write_status), &info) &&
            info.dli_fname && stat(info.dli_fname, &st2) == 0) {
            s += std::to_string(static_cast<long long>(st2.st_size));
        } else {
            s += "-";
        }
    }
    s += "\nMODE=OFFLINE";
    s += "\nHOOK=";
    s += (g_hookUpdate && g_hookCenter) ? "ok" : "gagal";
    s += "\nHOOKS=update:";
    s += g_hookUpdate ? "ok" : "gagal";
    s += ",shoot:";
    s += g_hookShoot ? "ok" : "gagal";
    s += ",special:";
    s += g_hookSpecialShoot ? "ok" : "gagal";
    s += ",center:";
    s += g_hookCenter ? "ok" : "gagal";
    long long lp = g_lastTickMs.load();
    s += "\nPUMP=" + std::to_string(g_tickCount.load()) + " " +
         (lp ? std::to_string(now_ms() - lp) : std::string("-1"));
    // v2.0.1: langkah init terakhir + alamat method hasil resolve.
    s += "\nINIT_STEP=" +
         (g_initStep.empty() ? std::string("-") : g_initStep);
    {
        char abuf[160];
        snprintf(abuf, sizeof(abuf),
                 "update:0x%llx,shoot:0x%llx,special:0x%llx,center:0x%llx",
                 (unsigned long long)g_addrUpdate,
                 (unsigned long long)g_addrShoot,
                 (unsigned long long)g_addrSpecial,
                 (unsigned long long)g_addrCenter);
        s += "\nADDRS=";
        s += abuf;
    }
    s += "\n";
    write_file(kStatusTmp, s);
    write_file(kStatusCache, s);
    write_file(kStatusExt, s);
}

void add_failed(const char* name) {
    if (name == nullptr || name[0] == '\0') return;
    std::string n(name);
    for (const auto& e : g_failedList) {
        if (e == n) return;
    }
    g_failedList.push_back(n);
}

// Hapus entri gagal bila percobaan ulang BERHASIL (retry resolve /
// retry hook) — supaya status tidak macet PARTIAL padahal semua jalan.
void erase_failed(const char* name) {
    if (name == nullptr || name[0] == '\0') return;
    std::string n(name);
    for (auto it = g_failedList.begin(); it != g_failedList.end(); ++it) {
        if (*it == n) {
            g_failedList.erase(it);
            return;
        }
    }
}

// ------------------------------------------------------- nilai primitif
// Baca string JSON sederhana: "key":true / "key":123.
bool json_bool(const std::string& j, const char* key) {
    std::string pat = "\"";
    pat += key;
    pat += "\":";
    size_t i = j.find(pat);
    if (i == std::string::npos) return false;
    i += pat.size();
    return j.compare(i, 4, "true") == 0;
}

// ---------------------------------------------------------- il2cpp API
// Semua symbol IL2CPP di-resolve dari libil2cpp.so yang sudah dimuat
// proses game — tanpa dlopen baru (dlopen bersarang di dalam constructor
// = deadlock linker, pelajaran HCR/PvZ).
struct Api {
    void* handle = nullptr;
    Il2CppClass* (*class_from_name)(void*, const char*, const char*) =
        nullptr;
    Il2CppMethodOpaque* (*class_get_method_from_name)(
        Il2CppClass*, const char*, int) = nullptr;
    Il2CppField* (*class_get_field_from_name)(Il2CppClass*,
                                              const char*) = nullptr;
    Il2CppClass* (*object_get_class)(Il2CppObject*) = nullptr;
    Il2CppObject* (*runtime_invoke)(Il2CppMethodOpaque*, void*, void**,
                                    Il2CppObject**) = nullptr;
    Fn_object_unbox object_unbox = nullptr;
    Fn_domain_get_assemblies domain_get_assemblies = nullptr;
    Fn_assembly_get_image assembly_get_image = nullptr;
    Fn_class_get_parent class_get_parent = nullptr;
    void (*field_get_value)(Il2CppObject*, Il2CppField*, void*) = nullptr;
    void (*field_set_value)(Il2CppObject*, Il2CppField*, void*) = nullptr;
    void (*field_static_get_value)(Il2CppField*, void*) = nullptr;
    void (*field_static_set_value)(Il2CppField*, void*) = nullptr;
    void (*thread_attach)(Il2CppDomain*) = nullptr;
    Il2CppDomain* (*domain_get)() = nullptr;
    Fn_class_get_type class_get_type = nullptr;
    Fn_type_get_object type_get_object = nullptr;
    Fn_array_length array_length = nullptr;
} g_api;

// Class / method / field yang ter-resolve. Class game semua di
// namespace global ("").
struct Resolved {
    Il2CppClass* weaponClass = nullptr;
    Il2CppClass* animalClass = nullptr;
    Il2CppClass* animalDataClass = nullptr;
    Il2CppClass* sightClass = nullptr;
    Il2CppClass* weaponDataClass = nullptr;
    Il2CppClass* playerCameraClass = nullptr;
    Il2CppClass* pdmClass = nullptr;  // PlayerDataManager
    Il2CppClass* cameraClass = nullptr;
    Il2CppClass* transformClass = nullptr;
    Il2CppClass* componentClass = nullptr;
    Il2CppClass* screenClass = nullptr;
    // Registry hewan: parent Animal = ListedUnity<Animal> + field
    // statis mAll (List<Animal>) — dibaca langsung, tanpa enumerasi.
    Il2CppClass* listedAnimalClass = nullptr;
    Il2CppField* listedAnimalAll = nullptr;
    // Unity helpers.
    Il2CppMethodOpaque* cameraGetMain = nullptr;
    Il2CppMethodOpaque* cameraWorldToScreen = nullptr;
    Il2CppMethodOpaque* transformGetPosition = nullptr;
    Il2CppMethodOpaque* componentGetTransform = nullptr;
    Il2CppMethodOpaque* screenGetWidth = nullptr;
    Il2CppMethodOpaque* screenGetHeight = nullptr;
    // Game.
    Il2CppMethodOpaque* weaponUpdate = nullptr;
    Il2CppMethodOpaque* weaponShoot = nullptr;
    Il2CppMethodOpaque* weaponSpecialShoot = nullptr;
    Il2CppMethodOpaque* playerCameraGetScreenCenter = nullptr;
    Il2CppMethodOpaque* animalGetIsALive = nullptr;
    Il2CppMethodOpaque* animalGetPosition = nullptr;
    Il2CppMethodOpaque* animalGetCenterPosition = nullptr;
    Il2CppMethodOpaque* pdmAddCoinsOne = nullptr;
    // Fields.
    Il2CppField* weaponBulletsNum = nullptr;
    Il2CppField* weaponIsScope = nullptr;
    Il2CppField* weaponDataField = nullptr;
    Il2CppField* weaponSightField = nullptr;
    Il2CppField* animalHeadTransform = nullptr;
    Il2CppField* animalCurrentHealth = nullptr;
    Il2CppField* animalHealth = nullptr;
    Il2CppField* animalAlive = nullptr;
    Il2CppField* animalDataField = nullptr;
    Il2CppField* animalDataDisplayName = nullptr;
    Il2CppField* sightZoomAmount = nullptr;
    Il2CppField* sightPower = nullptr;
    Il2CppField* sightMaxPower = nullptr;
    Il2CppField* wdMaxRecoil = nullptr;
    Il2CppField* wdShake = nullptr;
    Il2CppField* wdShock = nullptr;
    Il2CppField* wdMinReticle = nullptr;
    Il2CppField* wdMaxReticle = nullptr;
    Il2CppField* pdmEnergy = nullptr;  // static int
} g_r;

bool load_il2cpp() {
    void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        LOGE("libil2cpp.so tidak termuat (belum load?)");
        return false;
    }
    g_api.handle = h;
    auto sym = [h](const char* name) { return dlsym(h, name); };
    g_api.class_from_name =
        (decltype(g_api.class_from_name))sym("il2cpp_class_from_name");
    g_api.class_get_method_from_name =
        (decltype(g_api.class_get_method_from_name))sym(
            "il2cpp_class_get_method_from_name");
    g_api.class_get_field_from_name =
        (decltype(g_api.class_get_field_from_name))sym(
            "il2cpp_class_get_field_from_name");
    g_api.object_get_class =
        (decltype(g_api.object_get_class))sym("il2cpp_object_get_class");
    g_api.runtime_invoke =
        (decltype(g_api.runtime_invoke))sym("il2cpp_runtime_invoke");
    g_api.object_unbox =
        (Fn_object_unbox)sym("il2cpp_object_unbox");
    g_api.domain_get_assemblies =
        (Fn_domain_get_assemblies)sym("il2cpp_domain_get_assemblies");
    g_api.assembly_get_image =
        (Fn_assembly_get_image)sym("il2cpp_assembly_get_image");
    g_api.class_get_parent =
        (Fn_class_get_parent)sym("il2cpp_class_get_parent");
    g_api.field_get_value =
        (decltype(g_api.field_get_value))sym("il2cpp_field_get_value");
    g_api.field_set_value =
        (decltype(g_api.field_set_value))sym("il2cpp_field_set_value");
    g_api.field_static_get_value = (decltype(g_api.field_static_get_value))sym(
        "il2cpp_field_static_get_value");
    g_api.field_static_set_value = (decltype(g_api.field_static_set_value))sym(
        "il2cpp_field_static_set_value");
    g_api.thread_attach =
        (decltype(g_api.thread_attach))sym("il2cpp_thread_attach");
    g_api.domain_get =
        (decltype(g_api.domain_get))sym("il2cpp_domain_get");
    g_api.class_get_type =
        (Fn_class_get_type)sym("il2cpp_class_get_type");
    g_api.type_get_object =
        (Fn_type_get_object)sym("il2cpp_type_get_object");
    g_api.array_length = (Fn_array_length)sym("il2cpp_array_length");
    if (!g_api.class_from_name || !g_api.class_get_method_from_name ||
        !g_api.class_get_field_from_name || !g_api.object_get_class ||
        !g_api.runtime_invoke || !g_api.field_get_value ||
        !g_api.field_set_value || !g_api.field_static_set_value ||
        !g_api.thread_attach || !g_api.domain_get ||
        !g_api.object_unbox || !g_api.domain_get_assemblies ||
        !g_api.assembly_get_image || !g_api.class_get_type ||
        !g_api.type_get_object || !g_api.array_length) {
        LOGE("symbol il2cpp penting hilang");
        return false;
    }
    return true;
}

// ------------------------------------------------------------ invoke
// HANYA dipanggil dari hook (main thread Unity). Mengembalikan nullptr
// bila terjadi exception IL2CPP — jangan baca field objek setelah itu.
Il2CppObject* invoke(Il2CppMethodOpaque* m, void* thiz, void** args) {
    if (!m || !g_api.runtime_invoke) return nullptr;
    Il2CppObject* exc = nullptr;
    Il2CppObject* ret = g_api.runtime_invoke(m, thiz, args, &exc);
    if (exc != nullptr) {
        LOGW("runtime_invoke exception");
        return nullptr;
    }
    return ret;
}

template <typename T>
bool invoke_value(Il2CppMethodOpaque* m, void* thiz, void** args, T& out) {
    Il2CppObject* boxed = invoke(m, thiz, args);
    if (!boxed) return false;
    void* unboxed = g_api.object_unbox(boxed);
    if (!unboxed) return false;
    out = *reinterpret_cast<T*>(unboxed);
    return true;
}

// Cari class di semua assembly (Assembly-CSharp + UnityEngine.*).
Il2CppClass* find_class(const char* namespaze, const char* name) {
    Il2CppDomain* domain = g_api.domain_get();
    if (!domain) return nullptr;
    size_t count = 0;
    const Il2CppAssembly** assemblies =
        g_api.domain_get_assemblies(domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; i++) {
        Il2CppImage* image = g_api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        Il2CppClass* klass = g_api.class_from_name(image, namespaze, name);
        if (klass) return klass;
    }
    return nullptr;
}

// Cari method, jalan menelusuri parent (override di subclass dicari dulu).
Il2CppMethodOpaque* find_method(Il2CppClass* klass, const char* name,
                                int argc) {
    Il2CppClass* cur = klass;
    for (int depth = 0; cur && depth < 8; depth++) {
        Il2CppMethodOpaque* m =
            g_api.class_get_method_from_name(cur, name, argc);
        if (m) return m;
        cur = g_api.class_get_parent ? g_api.class_get_parent(cur) : nullptr;
    }
    return nullptr;
}

// Baca field instance (objek nullable → out default bila obj null).
template <typename T>
bool field_get(Il2CppObject* obj, Il2CppField* f, T& out) {
    if (!obj || !f || !g_api.field_get_value) return false;
    g_api.field_get_value(obj, f, &out);
    return true;
}

template <typename T>
bool field_set(Il2CppObject* obj, Il2CppField* f, const T& v) {
    if (!obj || !f || !g_api.field_set_value) return false;
    T tmp = v;
    g_api.field_set_value(obj, f, &tmp);
    return true;
}

// Liveness UnityEngine.Object — m_CachedPtr (0x10) == 0 berarti objek
// sudah dihancurkan: panggilan Unity apa pun atasnya (get_position,
// WorldToScreenPoint, get_name) crash di native. Cek ini SEBELUM
// setiap panggilan Unity (pelajaran tombstone TR v1.4).
bool obj_alive(void* o) {
    if (o == nullptr) return false;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(o) + 0x10) !=
           nullptr;
}

bool vec_finite(Vector3 v) {
    return v.x == v.x && v.y == v.y && v.z == v.z && v.x > -1e6f &&
           v.x < 1e6f && v.y > -1e6f && v.y < 1e6f && v.z > -1e6f &&
           v.z < 1e6f;
}

// Baca string IL2CPP (Il2CppString: length int32 di 0x10, chars UTF-16
// di 0x14). ASCII cepat; karakter non-ASCII jadi '?'.
std::string read_il2cpp_string(Il2CppObject* strObj) {
    if (!strObj || !g_api.object_get_class) return "";
    int32_t len = *reinterpret_cast<int32_t*>(
        reinterpret_cast<char*>(strObj) + 0x10);
    if (len <= 0 || len > 64) return "";
    const uint16_t* chars = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<char*>(strObj) + 0x14);
    std::string out;
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = chars[i];
        if (c == 0) break;
        out += (c < 0x80) ? static_cast<char>(c) : '?';
    }
    return out;
}

// ------------------------------------------------------------ resolve
// Semua class/method/field game di-resolve berdasarkan NAMA — TANPA
// offset/alamat hardcode, TANPA lock versi. Nama yang hilang dari dump
// (update game) masuk g_failedList -> status PARTIAL (fitur terkait
// nonaktif, bukan toggle palsu). Nama yang core (bila hilang game
// tidak bisa dimodifikasi sama sekali) -> UNSUPPORTED.
void resolve_all() {
    // Percobaan ulang (retry init): bersihkan hasil percobaan
    // sebelumnya — kegagalan sesaat (domain belum siap) tidak boleh
    // menempel bila percobaan berikut berhasil.
    g_failedList.clear();
    g_coreResolved = false;
    g_addrUpdate = 0;
    g_addrShoot = 0;
    g_addrSpecial = 0;
    g_addrCenter = 0;
    auto needClass = [](Il2CppClass*& dst, const char* ns, const char* name,
                        const char* fail, bool core) {
        dst = find_class(ns, name);
        if (!dst) {
            LOGW("class tidak ada: %s", name);
            add_failed(fail);
            if (!core) return;
            g_coreResolved = false;
        }
    };
    // Kelas game — semuanya di namespace global ("").
    needClass(g_r.weaponClass, "", "Weapon", "class:Weapon", true);
    needClass(g_r.animalClass, "", "Animal", "class:Animal", true);
    needClass(g_r.animalDataClass, "", "AnimalData", "class:AnimalData",
              true);
    needClass(g_r.sightClass, "", "Sight", "class:Sight", true);
    needClass(g_r.weaponDataClass, "", "WeaponData", "class:WeaponData",
              true);
    needClass(g_r.playerCameraClass, "", "PlayerCamera",
              "class:PlayerCamera", true);
    needClass(g_r.pdmClass, "", "PlayerDataManager",
              "class:PlayerDataManager", true);
    needClass(g_r.cameraClass, "UnityEngine", "Camera", "class:Camera",
              true);
    needClass(g_r.transformClass, "UnityEngine", "Transform",
              "class:Transform", true);
    needClass(g_r.componentClass, "UnityEngine", "Component",
              "class:Component", true);
    needClass(g_r.screenClass, "UnityEngine", "Screen", "class:Screen",
              true);
    if (!g_r.weaponClass || !g_r.animalClass || !g_r.animalDataClass ||
        !g_r.sightClass || !g_r.weaponDataClass ||
        !g_r.playerCameraClass || !g_r.pdmClass || !g_r.cameraClass ||
        !g_r.transformClass || !g_r.componentClass || !g_r.screenClass) {
        g_coreResolved = false;
        LOGE("resolve core class gagal -> UNSUPPORTED");
        return;
    }
    g_coreResolved = true;

    auto needMethod = [](Il2CppMethodOpaque*& dst, Il2CppClass* klass,
                         const char* name, int argc, const char* fail,
                         bool core) {
        dst = find_method(klass, name, argc);
        if (!dst) {
            LOGW("method tidak ada: %s", name);
            add_failed(fail);
            if (core) g_coreResolved = false;
        }
    };
    auto needField = [](Il2CppField*& dst, Il2CppClass* klass,
                        const char* name, const char* fail, bool core) {
        dst = g_api.class_get_field_from_name(klass, name);
        if (!dst) {
            LOGW("field tidak ada: %s", name);
            add_failed(fail);
            if (core) g_coreResolved = false;
        }
    };

    // Method Unity (dipakai ESP/proyeksi).
    needMethod(g_r.cameraGetMain, g_r.cameraClass, "get_main", 0,
               "method:Camera.get_main", true);
    needMethod(g_r.cameraWorldToScreen, g_r.cameraClass,
               "WorldToScreenPoint", 1, "method:WorldToScreenPoint", true);
    needMethod(g_r.transformGetPosition, g_r.transformClass, "get_position",
               0, "method:get_position", true);
    needMethod(g_r.componentGetTransform, g_r.componentClass,
               "get_transform", 0, "method:get_transform", true);
    needMethod(g_r.screenGetWidth, g_r.screenClass, "get_width", 0,
               "method:Screen.get_width", true);
    needMethod(g_r.screenGetHeight, g_r.screenClass, "get_height", 0,
               "method:Screen.get_height", true);

    // Method gameplay — driver (core) + tembakan + aimbot.
    // Weapon.Update = DRIVER: hook ini satu-satunya sumber kerja
    // per-frame; bila method ini tidak ada -> core gagal.
    needMethod(g_r.weaponUpdate, g_r.weaponClass, "Update", 0,
               "method:Weapon.Update", true);
    needMethod(g_r.weaponShoot, g_r.weaponClass, "Shoot", 0,
               "method:Weapon.Shoot", false);
    needMethod(g_r.weaponSpecialShoot, g_r.weaponClass, "SpecialShoot", 0,
               "method:Weapon.SpecialShoot", false);
    needMethod(g_r.playerCameraGetScreenCenter, g_r.playerCameraClass,
               "get_ScreenCenter", 0, "method:get_ScreenCenter", false);
    needMethod(g_r.animalGetIsALive, g_r.animalClass, "get_IsALive", 0,
               "method:Animal.get_IsALive", false);
    needMethod(g_r.animalGetPosition, g_r.animalClass, "get_Position", 0,
               "method:Animal.get_Position", false);
    needMethod(g_r.animalGetCenterPosition, g_r.animalClass,
               "get_CenterPosition", 0, "method:Animal.get_CenterPosition",
               false);
    needMethod(g_r.pdmAddCoinsOne, g_r.pdmClass, "AddCoinsOne", 1,
               "method:AddCoinsOne", false);

    // Field instance Weapon.
    needField(g_r.weaponBulletsNum, g_r.weaponClass, "bulletsNum",
              "field:bulletsNum", false);
    needField(g_r.weaponIsScope, g_r.weaponClass, "mIsScope",
              "field:mIsScope", true);
    needField(g_r.weaponDataField, g_r.weaponClass, "mWeaponData",
              "field:mWeaponData", true);
    needField(g_r.weaponSightField, g_r.weaponClass, "mSight",
              "field:mSight", true);
    // Field Animal.
    needField(g_r.animalHeadTransform, g_r.animalClass, "HeadTransform",
              "field:HeadTransform", true);
    needField(g_r.animalCurrentHealth, g_r.animalClass, "mCurrentHealth",
              "field:mCurrentHealth", true);
    needField(g_r.animalHealth, g_r.animalClass, "mHealth",
              "field:mHealth", true);
    needField(g_r.animalAlive, g_r.animalClass, "mAnimalALive",
              "field:mAnimalALive", true);
    needField(g_r.animalDataField, g_r.animalClass, "mAnimalData",
              "field:mAnimalData", true);
    needField(g_r.animalDataDisplayName, g_r.animalDataClass, "displayName",
              "field:displayName", false);
    // Field Sight / WeaponData (no-recoil + zoom).
    needField(g_r.sightZoomAmount, g_r.sightClass, "mZoomAmount",
              "field:mZoomAmount", true);
    needField(g_r.sightPower, g_r.sightClass, "mPower", "field:mPower",
              true);
    needField(g_r.sightMaxPower, g_r.sightClass, "mMaxPower",
              "field:mMaxPower", false);
    needField(g_r.wdMaxRecoil, g_r.weaponDataClass, "maxRecoil",
              "field:maxRecoil", true);
    needField(g_r.wdShake, g_r.weaponDataClass, "Shake", "field:Shake",
              true);
    needField(g_r.wdShock, g_r.weaponDataClass, "shock", "field:shock",
              true);
    needField(g_r.wdMinReticle, g_r.weaponDataClass, "minReticleRadius",
              "field:minReticleRadius", true);
    needField(g_r.wdMaxReticle, g_r.weaponDataClass, "maxReticleRadius",
              "field:maxReticleRadius", true);
    // Energi (static).
    needField(g_r.pdmEnergy, g_r.pdmClass, "currentPlayerEnergy",
              "field:currentPlayerEnergy", false);

    // Registry hewan — parent Animal = ListedUnity<Animal>, field
    // statis mAll (List<Animal>). Tanpa ini ESP + aimbot tidak bisa
    // jalan (PARTIAL), tapi fitur lain (recoil/zoom/energi/koin) tetap
    // jalan — karena itu field ini TIDAK core.
    if (g_r.animalClass && g_api.class_get_parent) {
        g_r.listedAnimalClass = g_api.class_get_parent(g_r.animalClass);
        if (g_r.listedAnimalClass) {
            g_r.listedAnimalAll = g_api.class_get_field_from_name(
                g_r.listedAnimalClass, "mAll");
        }
    }
    if (!g_r.listedAnimalAll) {
        LOGW("registry ListedUnity<Animal>.mAll tidak ada -> ESP + aimbot "
             "PARTIAL");
        add_failed("field:mAll");
    }
    // Catat alamat hasil resolve untuk diagnosis (ADDRS di status).
    g_addrUpdate = (uintptr_t)method_ptr(g_r.weaponUpdate);
    g_addrShoot = (uintptr_t)method_ptr(g_r.weaponShoot);
    g_addrSpecial = (uintptr_t)method_ptr(g_r.weaponSpecialShoot);
    g_addrCenter = (uintptr_t)method_ptr(g_r.playerCameraGetScreenCenter);
    LOGI("resolve selesai: core=%d failed=%zu", (int)g_coreResolved,
         g_failedList.size());
}

// ------------------------------------------------------------- stub
// JANGAN hook method stub (<=8 byte): trampoline Dobby menimpa
// tetangga (pelajaran BUSSID v1.0.7: patch 12 byte menghancurkan
// dua stub 4-byte bertetangga -> crash). Method Dobby-hook yang aman
// harus >= 16 byte dan tanpa return instan.
bool is_stub(void* addr) {
    if (!addr) return true;
    uint32_t first = *reinterpret_cast<uint32_t*>(addr);
    // ret (d65f03c0) atau br x?? (14000000 + offset) di instruksi pertama
    if (first == 0xd65f03c0) return true;
    if ((first & 0xfc000000u) == 0x14000000u) return true;
    return false;
}

// -------------------------------------------------------------- tick
// Driver per-frame, dipanggil dari hook Weapon.Update (main thread
// Unity) SETELAH orig. v2.0 TIDAK PUNYA pompa terpisah — inilah
// satu-satunya tempat kerja berkala dilakukan.
//   - Stability gate: fitur hanya aktif bila Weapon yang sama sudah
//     hidup >= 2000 ms (transisi scene = tersangka crash, pelajaran
//     Sniper v1.0.6).
//   - Target + ESP dihitung tiap frame dari registry mAll (tanpa
//     FindObjectsOfType).
//   - Fitur diterapkan dari instance Weapon yang ditangkap hook.
void update_target_and_esp();
void esp_write();
void apply_features(const Config& cfg);
void check_coins_command();

bool tick_stable(void* weapon, long long t) {
    if (weapon != g_weaponInst) {
        g_weaponInst = weapon;
        g_huntingSinceMs = t;
        std::lock_guard<std::mutex> lk(g_mu);
        g_target.valid = false;
        return false;
    }
    return (t - g_huntingSinceMs) >= 2000;
}

void tick(void* weapon) {
    long long t = now_ms();
    g_lastTickMs.store(t);
    g_tickCount.fetch_add(1);
    if (!tick_stable(weapon, t)) return;

    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cfg = g_config;
    }
    update_target_and_esp();
    esp_write();
    apply_features(cfg);
    check_coins_command();
}

// ------------------------------------------------------- target + ESP
// Baca registry statis ListedUnity<Animal>.mAll (List<Animal> yang
// dipelihara game sendiri via Awake/OnDestroy) — tanpa enumerasi
// FindObjectsOfType. Dipanggil dari tick (main thread), jadi semua
// panggilan Unity (get_position, WorldToScreenPoint) aman.
// Layout List<T> IL2CPP (sama dengan TR patterns.h): _items 0x10
// (Il2CppArray*), _size 0x18 (int). Layout Il2CppArray: max_length
// 0x18, data 0x20.
void update_target_and_esp() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_target.valid = false;
    g_entries.clear();
    if (!g_r.listedAnimalAll) return;

    Il2CppObject* camObj = invoke(g_r.cameraGetMain, nullptr, nullptr);
    if (!camObj || !obj_alive(camObj)) return;
    Il2CppObject* camTransform =
        invoke(g_r.componentGetTransform, camObj, nullptr);
    if (!camTransform || !obj_alive(camTransform)) return;
    Vector3 camPos{};
    if (!invoke_value(g_r.transformGetPosition, camTransform, nullptr,
                      camPos) ||
        !vec_finite(camPos)) {
        return;
    }
    int sw = 0, sh = 0;
    if (!invoke_value(g_r.screenGetWidth, nullptr, nullptr, sw) ||
        !invoke_value(g_r.screenGetHeight, nullptr, nullptr, sh) ||
        sw <= 0 || sh <= 0) {
        return;
    }
    g_screenW = sw;
    g_screenH = sh;

    void* listObj = nullptr;
    g_api.field_static_get_value(g_r.listedAnimalAll, &listObj);
    if (!listObj || !obj_alive(listObj)) return;
    void* items = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(listObj) + 0x10);
    int32_t size = *reinterpret_cast<int32_t*>(
        reinterpret_cast<char*>(listObj) + 0x18);
    if (!items || size <= 0) return;
    int32_t arrLen = *reinterpret_cast<int32_t*>(
        reinterpret_cast<char*>(items) + 0x18);
    int32_t n = size;
    if (n > arrLen) n = arrLen;
    if (n > 32) n = 32;  // cap (main thread tetap murah)
    void** arr = reinterpret_cast<void**>(
        reinterpret_cast<char*>(items) + 0x20);

    float bestDist = 1e30f;
    for (int32_t i = 0; i < n; i++) {
        Il2CppObject* animal = reinterpret_cast<Il2CppObject*>(arr[i]);
        if (!animal || !obj_alive(animal)) continue;
        bool alive = true;
        if (g_r.animalGetIsALive) {
            if (!invoke_value(g_r.animalGetIsALive, animal, nullptr,
                              alive) ||
                !alive) {
                continue;
            }
        } else {
            bool al = false;
            field_get(animal, g_r.animalAlive, al);
            if (!al) continue;
        }
        Vector3 pos{};
        if (g_r.animalGetPosition) {
            if (!invoke_value(g_r.animalGetPosition, animal, nullptr,
                              pos) ||
                !vec_finite(pos)) {
                continue;
            }
        } else {
            Il2CppObject* tr =
                invoke(g_r.componentGetTransform, animal, nullptr);
            if (!tr || !obj_alive(tr)) continue;
            if (!invoke_value(g_r.transformGetPosition, tr, nullptr,
                              pos) ||
                !vec_finite(pos)) {
                continue;
            }
        }
        Vector3 head = pos;
        bool haveHead = false;
        Il2CppObject* headTr = nullptr;
        field_get(animal, g_r.animalHeadTransform, headTr);
        if (headTr && obj_alive(headTr)) {
            if (invoke_value(g_r.transformGetPosition, headTr, nullptr,
                             head) &&
                vec_finite(head)) {
                haveHead = true;
            }
        }
        if (!haveHead && g_r.animalGetCenterPosition) {
            if (invoke_value(g_r.animalGetCenterPosition, animal, nullptr,
                             head) &&
                vec_finite(head)) {
                haveHead = true;
            }
        }
        if (!haveHead) head = Vector3{pos.x, pos.y + 1.0f, pos.z};

        void* args[1] = {&head};
        Vector3 scr{};
        if (!invoke_value(g_r.cameraWorldToScreen, camObj, args, scr)) {
            continue;
        }
        // z <= 0 berarti di belakang kamera; di luar layar dilewati.
        if (!(scr.z > 0.0f) || scr.x < -50 || scr.x > sw + 50 ||
            scr.y < -50 || scr.y > sh + 50) {
            continue;
        }
        float dist = std::sqrt((pos.x - camPos.x) * (pos.x - camPos.x) +
                               (pos.y - camPos.y) * (pos.y - camPos.y) +
                               (pos.z - camPos.z) * (pos.z - camPos.z));
        float hp = 1.0f;
        float cur = 0, max = 0;
        if (field_get(animal, g_r.animalCurrentHealth, cur) &&
            field_get(animal, g_r.animalHealth, max) && max > 0) {
            hp = cur / max;
            if (hp < 0) hp = 0;
            if (hp > 1) hp = 1;
        }
        std::string name = "hewan";
        Il2CppObject* ad = nullptr;
        if (field_get(animal, g_r.animalDataField, ad) && ad &&
            obj_alive(ad) && g_r.animalDataDisplayName) {
            Il2CppObject* strObj = nullptr;
            field_get(ad, g_r.animalDataDisplayName, strObj);
            std::string nm = read_il2cpp_string(strObj);
            if (!nm.empty()) name = nm;
        }
        float boxH = (dist > 0.5f) ? (1.8f * sh / dist) : sh;
        if (boxH > sh * 1.5f) boxH = sh * 1.5f;
        EspEntry e;
        e.x = scr.x;
        e.y = sh - scr.y;  // y Unity (bawah) -> layar (atas)
        e.w = boxH * 0.6f;
        e.h = boxH;
        e.hp = hp;
        e.dist = dist;
        e.name = name;
        g_entries.push_back(e);
        float dx = scr.x - sw * 0.5f;
        float dy = scr.y - sh * 0.5f;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestDist) {
            bestDist = d2;
            g_target.headPos = head;
            g_target.camPos = camPos;
            g_target.screenX = scr.x;
            g_target.screenY = sh - scr.y;
            g_target.timestampMs = now_ms();
            g_target.valid = true;
        }
    }
}

// Tulis file ESP JSON untuk overlay aplikasi (FloatingService — poll
// tiap 150 ms). Format sama dengan v1.0. Throttle 200 ms + tulis
// atomik (tmp + rename) supaya pembaca tidak pernah baca JSON setengah.
// Bila ESP OFF: tulis {"mode":"OFFLINE","t":[]} SEKALI (saat transisi).
void esp_write() {
    long long t = now_ms();
    bool want;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        want = g_config.esp;
    }
    if (!want) {
        if (!g_espFileOff) {
            write_file_atomic(kEspPath, kEspTmpPath,
                              "{\"mode\":\"OFFLINE\",\"t\":[]}");
            g_espFileOff = true;
        }
        return;
    }
    if (t - g_lastEspWriteMs < 200) return;
    g_lastEspWriteMs = t;
    std::string json = "{\"mode\":\"OFFLINE\",\"t\":[";
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (size_t i = 0; i < g_entries.size(); i++) {
            const EspEntry& e = g_entries[i];
            if (i) json += ",";
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                          "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,"
                          "\"hp\":%.2f,\"n\":\"%s\",\"d\":%.1f}",
                          e.x, e.y, e.w, e.h, e.hp,
                          e.name.c_str(), e.dist);
            json += buf;
        }
    }
    json += "]}";
    write_file_atomic(kEspPath, kEspTmpPath, json);
    g_espFileOff = false;
}

// ------------------------------------------------------------ fitur
// Diterapkan dari tick (main thread), hanya saat berburu stabil.
// Semua menulis field dari instance Weapon yang ditangkap hook —
// tanpa enumerasi, tanpa API Unity dari worker.
void apply_features(const Config& cfg) {
    void* weapon = g_weaponInst;
    if (!weapon || !obj_alive(weapon)) return;

    bool scope = false;
    field_get(reinterpret_cast<Il2CppObject*>(weapon), g_r.weaponIsScope,
              scope);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_isScope = scope;
    }

    // No-recoil: nol-kan maxRecoil/Shake/shock + kunci reticle ke
    // minReticleRadius (cara audit: spread dihitung dari min/max
    // reticle; recoil murni client-side).
    Il2CppObject* wd = nullptr;
    field_get(reinterpret_cast<Il2CppObject*>(weapon),
              g_r.weaponDataField, wd);
    if (wd && obj_alive(wd)) {
        if (cfg.noRecoil) {
            if (!g_wdOrig.saved) {
                field_get(wd, g_r.wdMaxRecoil, g_wdOrig.maxRecoil);
                field_get(wd, g_r.wdShake, g_wdOrig.shake);
                field_get(wd, g_r.wdShock, g_wdOrig.shock);
                field_get(wd, g_r.wdMinReticle, g_wdOrig.minReticle);
                field_get(wd, g_r.wdMaxReticle, g_wdOrig.maxReticle);
                g_wdOrig.saved = true;
            }
            float z = 0.0f;
            field_set(wd, g_r.wdMaxRecoil, z);
            field_set(wd, g_r.wdShake, z);
            field_set(wd, g_r.wdShock, z);
            field_set(wd, g_r.wdMaxReticle, g_wdOrig.minReticle);
        } else if (g_wdOrig.saved && g_prevTickConfig.noRecoil) {
            field_set(wd, g_r.wdMaxRecoil, g_wdOrig.maxRecoil);
            field_set(wd, g_r.wdShake, g_wdOrig.shake);
            field_set(wd, g_r.wdShock, g_wdOrig.shock);
            field_set(wd, g_r.wdMaxReticle, g_wdOrig.maxReticle);
            g_wdOrig.saved = false;
        }
    }

    // Zoom jauh: mPower = mMaxPower + mZoomAmount = 30 (FOV via
    // Sight, client-side murni).
    Il2CppObject* sight = nullptr;
    field_get(reinterpret_cast<Il2CppObject*>(weapon),
              g_r.weaponSightField, sight);
    if (sight && obj_alive(sight)) {
        if (cfg.zoomFar) {
            if (!g_sightOrig.saved) {
                field_get(sight, g_r.sightPower, g_sightOrig.power);
                field_get(sight, g_r.sightZoomAmount,
                          g_sightOrig.zoomAmount);
                g_sightOrig.saved = true;
            }
            float maxP = 8.0f;
            field_get(sight, g_r.sightMaxPower, maxP);
            if (maxP <= 0 || maxP > 100) maxP = 8.0f;
            field_set(sight, g_r.sightPower, maxP);
            float zm = 30.0f;
            field_set(sight, g_r.sightZoomAmount, zm);
        } else if (g_sightOrig.saved && g_prevTickConfig.zoomFar) {
            field_set(sight, g_r.sightPower, g_sightOrig.power);
            field_set(sight, g_r.sightZoomAmount, g_sightOrig.zoomAmount);
            g_sightOrig.saved = false;
        }
    }

    // Energi: tahan currentPlayerEnergy (static) di 15 (Max_Energy).
    if (cfg.energy && g_r.pdmEnergy) {
        int e = 15;
        g_api.field_static_set_value(g_r.pdmEnergy, &e);
    }
    g_prevTickConfig = cfg;
}

// -------------------------------------------------------------- koin
// Perintah dari aplikasi: {"cmd":"coins"} -> PlayerDataManager.
// AddCoinsOne(1000) — DIIVOKE DARI TICK (main thread Unity), BUKAN
// dari worker (v1.0 salah di sini: invoke dari worker butuh attach +
// race). File perintah DIHAPUS setelah dibaca; cek tiap 2 dtk.
void check_coins_command() {
    long long t = now_ms();
    if (t - g_lastCmdCheckMs < 2000) return;
    g_lastCmdCheckMs = t;
    if (!g_r.pdmAddCoinsOne) return;
    std::string cmd = read_file(kCmdPath);
    if (cmd.empty()) return;
    unlink(kCmdPath);
    if (json_bool(cmd, "coins")) {
        int amount = 1000;
        void* args[1] = {&amount};
        if (invoke(g_r.pdmAddCoinsOne, nullptr, args)) {
            LOGI("Tambah Koin +1000 (main thread)");
        } else {
            LOGW("AddCoinsOne gagal");
        }
    }
}

// -------------------------------------------------------------- hooks
// Hook get_ScreenCenter: aimbot. Weapon.Shoot memakai nilai ini untuk
// menghitung arah ray (ScreenPointToRay -> RaycastAll -> NotifyHit),
// jadi mengganti return-nya = membidik kepala hewan. Target dihitung
// tick (segar tiap frame); hook ini hanya memakai hasil yang sudah
// dihitung — TIDAK menghitung target sendiri (murah, tanpa panggilan
// Unity tambahan).
Vector2 hook_screen_center(void* thiz, void* method) {
    auto it = g_origScreenCenter.find(method);
    ScreenCenterFn orig = (it != g_origScreenCenter.end()) ? it->second
                                                           : nullptr;
    Config cfg;
    CachedTarget tgt;
    bool scope = false;
    int sw = 0, sh = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cfg = g_config;
        tgt = g_target;
        scope = g_isScope;
        sw = g_screenW;
        sh = g_screenH;
    }
    bool aimOn = cfg.aimbot && (!cfg.zoomAim || scope);
    if (aimOn && tgt.valid && sw > 0 && sh > 0 &&
        (now_ms() - tgt.timestampMs) < 300) {
        Vector2 r;
        r.x = tgt.screenX;
        r.y = tgt.screenY;
        return r;
    }
    if (orig) return orig(thiz, method);
    Vector2 c;
    c.x = sw > 0 ? sw * 0.5f : 0;
    c.y = sh > 0 ? sh * 0.5f : 0;
    return c;
}

// Hook Shoot / SpecialShoot: tangkap instance, panggil orig dulu,
// lalu no-reload: Shoot mengurangi bulletsNum 1 -> tahan di 99.
// Aimbot TIDAK di sini — target sudah dihitung tick, dan arah ray
// dibelokkan hook get_ScreenCenter yang dipanggil Shoot sendiri.
void hook_shoot(void* thiz, void* method) {
    auto it = g_origShoot.find(method);
    ShootFn orig = (it != g_origShoot.end()) ? it->second : nullptr;
    if (thiz) {
        long long t = now_ms();
        if (thiz != g_weaponInst) {
            g_weaponInst = thiz;
            g_huntingSinceMs = t;
        }
    }
    if (orig) orig(thiz, method);
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cfg = g_config;
    }
    if (!cfg.noReload || !thiz) return;
    if (now_ms() - g_huntingSinceMs < 2000) return;  // stability gate
    int ammo = 99;
    field_set(reinterpret_cast<Il2CppObject*>(thiz), g_r.weaponBulletsNum,
              ammo);
}

// Hook Weapon.Update: DRIVER utama. Tangkap Weapon, panggil orig,
// lalu tick (target+ESP, fitur, koin). Satu-satunya kerja berkala
// di payload — tidak ada pompa/worker enumerasi.
void hook_weapon_update(void* thiz, void* method) {
    auto it = g_origUpdate.find(method);
    UpdateFn orig = (it != g_origUpdate.end()) ? it->second : nullptr;
    if (orig) orig(thiz, method);
    if (thiz) tick(thiz);
}

// Pasang hook Dobby; gagal (stub / alamat tidak valid) -> catat di
// FAILED, jangan crash. Semua dipasang SEKALI dari init thread.
template <typename Fn>
bool install_hook(void* addr, void* hook, Fn* origOut,
                  const char* failName) {
    if (!addr || is_stub(addr)) {
        LOGW("hook dilewati (stub/null): %s", failName);
        add_failed(failName);
        return false;
    }
    void* orig = nullptr;
    if (DobbyHook(addr, hook, &orig) != 0 || !orig) {
        LOGE("DobbyHook gagal: %s", failName);
        add_failed(failName);
        return false;
    }
    *origOut = reinterpret_cast<Fn>(orig);
    erase_failed(failName);  // v2.0.1: retry yang berhasil hapus entri gagal
    return true;
}

void install_hooks() {
    // Guard flag: hanya pasang hook yang BELUM terpasang — retry dari
    // worker loop (v2.0.1) tidak boleh memanggil DobbyHook dua kali
    // pada alamat yang sama.
    if (g_r.weaponUpdate && !g_hookUpdate) {
        UpdateFn orig = nullptr;
        if (install_hook(method_ptr(g_r.weaponUpdate),
                         reinterpret_cast<void*>(&hook_weapon_update),
                         &orig, "hook:WeaponUpdate")) {
            g_origUpdate[method_ptr(g_r.weaponUpdate)] = orig;
            g_hookUpdate = true;
        }
    } else if (!g_r.weaponUpdate) {
        add_failed("hook:WeaponUpdate");
    }
    if (g_r.weaponShoot && !g_hookShoot) {
        ShootFn orig = nullptr;
        if (install_hook(method_ptr(g_r.weaponShoot),
                         reinterpret_cast<void*>(&hook_shoot), &orig,
                         "hook:WeaponShoot")) {
            g_origShoot[method_ptr(g_r.weaponShoot)] = orig;
            g_hookShoot = true;
        }
    }
    if (g_r.weaponSpecialShoot && !g_hookSpecialShoot) {
        ShootFn orig = nullptr;
        if (install_hook(method_ptr(g_r.weaponSpecialShoot),
                         reinterpret_cast<void*>(&hook_shoot), &orig,
                         "hook:WeaponSpecialShoot")) {
            g_origShoot[method_ptr(g_r.weaponSpecialShoot)] = orig;
            g_hookSpecialShoot = true;
        }
    }
    if (g_r.playerCameraGetScreenCenter && !g_hookCenter) {
        ScreenCenterFn orig = nullptr;
        if (install_hook(method_ptr(g_r.playerCameraGetScreenCenter),
                         reinterpret_cast<void*>(&hook_screen_center),
                         &orig, "hook:get_ScreenCenter")) {
            g_origScreenCenter[method_ptr(g_r.playerCameraGetScreenCenter)] =
                orig;
            g_hookCenter = true;
        }
    }
    LOGI("hook terpasang: update=%d shoot=%d special=%d center=%d",
         (int)g_hookUpdate, (int)g_hookShoot, (int)g_hookSpecialShoot,
         (int)g_hookCenter);
}

// -------------------------------------------------------- crash handler
// Penangkap crash native (SIGSEGV / SIGABRT / SIGBUS / SIGILL /
// SIGFPE / SIGTRAP): tulis bukti ke hozinhunter_crash.json (file
// TERPISAH — TIDAK PERNAH ditimpa worker). Isinya: sinyal + fase
// (INIT/READY/PARTIAL) + jumlah tick terakhir (PUMP). Bila game crash
// saat loading berburu, file ini dibaca diagnosis inject BERIKUTNYA.
// Handler async-signal-safe (hanya write/open/close + snprintf).
void crash_handler(int sig, siginfo_t*, void*) {
    const char* phase = "INIT";
    if (g_coreResolved) {
        phase = g_failedList.empty() ? "READY" : "PARTIAL";
    }
    int fd = open(kCrashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
                         "{\"signal\":%d,\"phase\":\"%s\","
                         "\"pump\":%lld,\"last_tick_ms_ago\":%lld,"
                         "\"hook_update\":%d,\"hook_shoot\":%d,"
                         "\"hook_center\":%d}\n",
                         sig, phase, g_tickCount.load(),
                         g_lastTickMs.load()
                             ? now_ms() - g_lastTickMs.load()
                             : -1,
                         (int)g_hookUpdate, (int)g_hookShoot,
                         (int)g_hookCenter);
        if (n > 0) {
            ssize_t w = write(fd, buf, (size_t)n);
            (void)w;
        }
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_crash_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE,
                        SIGTRAP};
    for (int s : sigs) {
        sigaction(s, &sa, nullptr);
    }
}

// ------------------------------------------------------------ worker
// Worker HANYA: baca config + tulis status. TIDAK ADA API Unity dari
// worker (pelajaran v1.0: invoke FindObjectsOfType dari worker =
// tersangka crash). Semua hook dipasang SEKALI dari sini (init
// thread) setelah resolve selesai.
void* worker_main(void*) {
    g_statusOverride = "INIT";
    g_initStep = "wait_libil2cpp";
    write_status();

    // Cold start: libil2cpp.so baru ter-map >20 dtk setelah game
    // dibuka (bukti tes POCO v1.0/v2.0), DAN ".so ter-map" BELUM
    // berarti domain IL2CPP siap — il2cpp_init (domain + assembly)
    // selesai beberapa detik SESUDAH mapping. v2.0 menunggu .so
    // muncul di /proc/self/maps lalu resolve SEKALI: bila resolve
    // jalan di jendela itu, SEMUA class NULL -> UNSUPPORTED -> hook
    // tak pernah dipasang, tanpa retry (akar macet INIT v2.0).
    // Perbaikan (pola v1.0 yang terbukti READY): ulangi load+resolve
    // tiap 500 ms s.d. 90 dtk. INIT_STEP di file status menunjukkan
    // percobaan ke berapa + apa yang gagal terakhir.
    const long long initStart = now_ms();
    long long lastStepWrite = 0;
    int attempt = 0;
    while (!g_coreResolved) {
        attempt++;
        g_initStep = "resolve:attempt " + std::to_string(attempt);
        if (load_il2cpp()) {
            resolve_all();
            if (g_coreResolved) break;
            g_initStep = "resolve_failed:" +
                         (g_failedList.empty() ? std::string("?")
                                               : g_failedList.front());
        } else {
            g_initStep = "wait_libil2cpp";
        }
        const long long t = now_ms();
        if (t - lastStepWrite >= 2000) {
            write_status();
            lastStepWrite = t;
        }
        if (t - initStart > 90000) {
            LOGE("init gagal setelah 90 dtk -> UNSUPPORTED");
            add_failed("init:timeout");
            g_initStep = "timeout";
            g_statusOverride.clear();
            write_status();
            return nullptr;
        }
        usleep(500 * 1000);
    }

    Il2CppDomain* domain = g_api.domain_get();
    if (domain && g_api.thread_attach) g_api.thread_attach(domain);

    g_initStep = "hook_install";
    write_status();
    install_hooks();

    g_initStep = "ready";
    g_statusOverride.clear();
    write_status();

    long long lastStatus = 0;
    long long lastHookRetry = 0;
    while (true) {
        std::string cfg = read_file(kCfgPath);
        if (!cfg.empty()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_config.aimbot = json_bool(cfg, "aimbot");
            g_config.zoomAim = json_bool(cfg, "zoomaim");
            g_config.esp = json_bool(cfg, "esp");
            g_config.noRecoil = json_bool(cfg, "norecoil");
            g_config.noReload = json_bool(cfg, "noreload");
            g_config.zoomFar = json_bool(cfg, "zoom");
            g_config.energy = json_bool(cfg, "energy");
        }
        long long t = now_ms();
        // Retry pasang hook yang sempat gagal (Dobby sesaat gagal
        // saat .so masih di-relokasi): hanya hook yang belum
        // terpasang (guard flag di install_hooks).
        if (t - lastHookRetry >= 2000) {
            install_hooks();
            lastHookRetry = t;
        }
        if (t - lastStatus >= 2000) {
            write_status();
            lastStatus = t;
        }
        usleep(500 * 1000);
    }
    return nullptr;
}

}  // namespace

// Definisi berkualifikasi: header mendeklarasikan
// wildhunter::start_worker() — definisi global tanpa kualifikasi
// TIDAK memenuhi deklarasi itu (linker: undefined symbol).
void wildhunter::start_worker() {
    // Pasang penangkap crash SEBELUM worker dibuat (v2.0.1: v2.0
    // memasangnya di dalam worker SETELAH load_il2cpp — crash saat
    // resolve awal tidak meninggalkan bukti).
    install_crash_handler();
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, worker_main, nullptr) != 0) {
        LOGE("gagal membuat worker thread");
    }
    pthread_attr_destroy(&attr);
}
