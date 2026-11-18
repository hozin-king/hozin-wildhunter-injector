// Hozin Wild Hunter 3D Injector — payload (IL2CPP resolve-by-name + hook
// Dobby). Adaptasi dari payload Sniper 3D v1.0.6 yang teruji di POCO
// (pompa main-thread, resolve runtime, status 3 kanal, crash handler).
//
// Game: Wild Hunter 3D (com.julian.wildhunter) v1.0.16 / code 17,
// Unity 2022.3.62f2 IL2CPP, metadata v31 — gameplay TERBUKTI full
// offline (nol network/PvP), jadi TANPA gate mode. Semua class game
// di namespace global ("").
//
// Pola verifikasi dari dump arm64 (REPORT-ARM64.md):
//   - Weapon.bulletsNum di [x20,#0x84] (disassembly Shoot 0x8f2f2c)
//     — offset ini HANYA referensi; resolve runtime lewat nama field.
//   - Animal.HeadTransform 0x40, mCurrentHealth 0x168, mHealth 0x16C,
//     mAnimalALive 0xFA, mAnimalData 0x150.
//   - Sight.mZoomAmount 0x18, mPower 0x1C, mMaxPower 0x14.
//   - WeaponData.damage 0x50, maxRecoil 0x68, Shake 0x6C, shock 0x70,
//     minReticleRadius 0xB4, maxReticleRadius 0xB8.
//   - PlayerCamera.mMainCamera 0xD0, mMaxFov 0x304, weapon 0x3E0,
//     currentAnimal 0x78. PlayerCamera.Update RVA 0x95B2E4 (body asli,
//     prolog STP — bukan stub RET/B).
//
// Primitif IL2CPP (dobby.h untuk hook), pola "semua API Unity dari
// main thread via pompa" dan "worker hanya baca config + tulis
// status/ESP + pasang hook" mengikuti payload Sniper 3D.
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
// Target aimbot + entri ESP. Diisi pompa (main thread), dibaca hook
// aimbot + writer ESP (worker thread) — semua akses di bawah lock.
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
Config g_configPump;      // salinan untuk pompa
CachedTarget g_target;    // target aimbot
std::vector<EspEntry> g_entries;
bool g_isScope = false;   // Weapon.mIsScope (dibaca pompa)

std::vector<std::string> g_failedList;  // nama yang gagal resolve
bool g_coreResolved = false;
std::string g_statusOverride;  // "INIT" sebelum resolve selesai
bool g_hookInstalled = false;    // hook PlayerCamera.Update terpasang
bool g_aimbotHookInstalled = false;
// Jejak pompa (diagnosis: apakah pompa main-thread masih jalan).
std::atomic<long long> g_lastPumpMs{0};
std::atomic<long long> g_pumpCount{0};
long long g_activeSinceMs = 0;
long long g_lastEnumerateMs = 0;
long long g_lastAimbotHookMs = 0;
long long g_lastCmdCheckMs = 0;
long long g_lastActiveCheckMs = 0;

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

// Simpan orig fungsi yang di-hook (key = methodPointer agar hook
// multi-instance tidak menimpa orig).
typedef void (*UpdateFn)(void* thiz, void* method);
std::map<void*, UpdateFn> g_origUpdate;
typedef Vector2 (*ScreenCenterFn)(void* thiz, void* method);
std::map<void*, ScreenCenterFn> g_origScreenCenter;

// Il2CppMethodOpaque dengan methodPointer di offset 0 (lihat
// il2cpp_api.h) — dipakai untuk menunjuk orig yang benar.
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
        struct stat st {};
        if (dladdr(reinterpret_cast<void*>(&write_status), &info) &&
            info.dli_fname && stat(info.dli_fname, &st) == 0) {
            s += std::to_string(static_cast<long long>(st.st_size));
        } else {
            s += "-";
        }
    }
    s += "\nMODE=OFFLINE";
    s += "\nHOOK=";
    s += g_aimbotHookInstalled ? "ok" : "gagal";
    long long lp = g_lastPumpMs.load();
    s += "\nPUMP=" + std::to_string(g_pumpCount.load()) + " " +
         (lp ? std::to_string(now_ms() - lp) : std::string("-1"));
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

void erase_failed(const char* name) {
    if (name == nullptr) return;
    g_failedList.erase(
        std::remove(g_failedList.begin(), g_failedList.end(),
                    std::string(name)),
        g_failedList.end());
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

int json_int(const std::string& j, const char* key) {
    std::string pat = "\"";
    pat += key;
    pat += "\":";
    size_t i = j.find(pat);
    if (i == std::string::npos) return -1;
    i += pat.size();
    size_t e = j.find_first_not_of("-0123456789", i);
    if (e == std::string::npos) e = j.size();
    if (e == i) return -1;
    return std::atoi(j.substr(i, e - i).c_str());
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
    Il2CppClass* objectClass = nullptr;
    Il2CppClass* componentClass = nullptr;
    Il2CppClass* screenClass = nullptr;
    // Unity helpers.
    Il2CppMethodOpaque* cameraGetMain = nullptr;
    Il2CppMethodOpaque* cameraWorldToScreen = nullptr;
    Il2CppMethodOpaque* transformGetPosition = nullptr;
    Il2CppMethodOpaque* componentGetTransform = nullptr;
    Il2CppMethodOpaque* objectFindObjects = nullptr;  // FindObjectsOfType<T>
    Il2CppMethodOpaque* objectFindObjectsByType = nullptr;
    Il2CppMethodOpaque* screenGetWidth = nullptr;
    Il2CppMethodOpaque* screenGetHeight = nullptr;
    // Game.
    Il2CppMethodOpaque* playerCameraUpdate = nullptr;
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
// Boleh dipanggil DARI POMPA (main thread) dan dari worker SETELAH
// il2cpp_thread_attach. Mengembalikan nullptr bila terjadi exception
// IL2CPP — jangan baca field objek setelah itu.
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

// Cari semua instance hidup dari sebuah class. Pola teruji payload
// Sniper 3D: Il2CppType dari class → System.Type object → panggil
// FindObjectsOfType(Type) (fallback Unity 6: FindObjectsByType).
// Array hasil: header 32 byte (Il2CppArrayHeader), elemen setelahnya.
std::vector<Il2CppObject*> find_objects(Il2CppClass* klass) {
    std::vector<Il2CppObject*> out;
    if (!klass) return out;
    Il2CppType* type = g_api.class_get_type(klass);
    if (!type) return out;
    Il2CppObject* sysType = g_api.type_get_object(type);
    if (!sysType) return out;
    Il2CppObject* arrObj = nullptr;
    if (g_r.objectFindObjects) {
        void* params[1] = {&sysType};
        arrObj = invoke(g_r.objectFindObjects, nullptr, params);
    } else if (g_r.objectFindObjectsByType) {
        int sortMode = 0;  // FindObjectsSortMode.None
        void* params[2] = {&sysType, &sortMode};
        arrObj = invoke(g_r.objectFindObjectsByType, nullptr, params);
    }
    if (!arrObj) return out;
    uint32_t len = g_api.array_length(arrObj);
    Il2CppObject** items = reinterpret_cast<Il2CppObject**>(
        reinterpret_cast<char*>(arrObj) + sizeof(Il2CppArrayHeader));
    for (uint32_t i = 0; i < len; i++) {
        if (items[i]) out.push_back(items[i]);
    }
    return out;
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

Config g_prevPumpConfig;
int g_screenW = 0, g_screenH = 0;  // ukuran layar (pompa → hook aimbot)
long long g_lastConfigReadMs = 0, g_lastHookCheckMs = 0;
long long g_lastEspWriteMs = 0, g_lastStatusWriteMs = 0;

// ------------------------------------------------------------ resolve
// Semua class/method/field berdasarkan NAMA. Yang gagal masuk
// g_failedList → status PARTIAL (fitur terkait nonaktif), kecuali yang
// ditandai core → UNSUPPORTED.
bool resolve_all() {
    auto needClass = [&](Il2CppClass*& slot, const char* ns,
                         const char* name) {
        slot = find_class(ns, name);
        if (!slot) add_failed((std::string("class:") + name).c_str());
    };
    needClass(g_r.weaponClass, "", "Weapon");
    needClass(g_r.animalClass, "", "Animal");
    needClass(g_r.animalDataClass, "", "AnimalData");
    needClass(g_r.sightClass, "", "Sight");
    needClass(g_r.weaponDataClass, "", "WeaponData");
    needClass(g_r.playerCameraClass, "", "PlayerCamera");
    needClass(g_r.pdmClass, "", "PlayerDataManager");
    needClass(g_r.cameraClass, "UnityEngine", "Camera");
    needClass(g_r.transformClass, "UnityEngine", "Transform");
    needClass(g_r.objectClass, "UnityEngine", "Object");
    needClass(g_r.componentClass, "UnityEngine", "Component");
    needClass(g_r.screenClass, "UnityEngine", "Screen");

    auto needMethod = [&](Il2CppMethodOpaque*& slot, Il2CppClass* klass,
                          const char* name, int argc) {
        if (!klass) return;
        slot = find_method(klass, name, argc);
        if (!slot) add_failed((std::string("method:") + name).c_str());
    };
    needMethod(g_r.cameraGetMain, g_r.cameraClass, "get_main", 0);
    needMethod(g_r.cameraWorldToScreen, g_r.cameraClass,
               "WorldToScreenPoint", 1);
    needMethod(g_r.transformGetPosition, g_r.transformClass,
               "get_position", 0);
    needMethod(g_r.componentGetTransform, g_r.componentClass,
               "get_transform", 0);
    needMethod(g_r.objectFindObjects, g_r.objectClass, "FindObjectsOfType",
               1);
    if (!g_r.objectFindObjects) {
        needMethod(g_r.objectFindObjectsByType, g_r.objectClass,
                   "FindObjectsByType", 2);
        if (g_r.objectFindObjectsByType) {
            // Fallback Unity 6 berhasil — hapus entri gagal supaya
            // status tidak jadi PARTIAL padahal semua jalan.
            erase_failed("method:FindObjectsOfType");
        }
    }
    needMethod(g_r.screenGetWidth, g_r.screenClass, "get_width", 0);
    needMethod(g_r.screenGetHeight, g_r.screenClass, "get_height", 0);
    needMethod(g_r.playerCameraUpdate, g_r.playerCameraClass, "Update", 0);
    needMethod(g_r.playerCameraGetScreenCenter, g_r.playerCameraClass,
               "get_ScreenCenter", 0);
    needMethod(g_r.animalGetIsALive, g_r.animalClass, "get_IsALive", 0);
    needMethod(g_r.animalGetPosition, g_r.animalClass, "get_Position", 0);
    needMethod(g_r.animalGetCenterPosition, g_r.animalClass,
               "get_CenterPosition", 0);
    needMethod(g_r.pdmAddCoinsOne, g_r.pdmClass, "AddCoinsOne", 1);

    auto needField = [&](Il2CppField*& slot, Il2CppClass* klass,
                         const char* name) {
        if (!klass) return;
        slot = g_api.class_get_field_from_name(klass, name);
        if (!slot) add_failed((std::string("field:") + name).c_str());
    };
    needField(g_r.weaponBulletsNum, g_r.weaponClass, "bulletsNum");
    needField(g_r.weaponIsScope, g_r.weaponClass, "mIsScope");
    needField(g_r.weaponDataField, g_r.weaponClass, "mWeaponData");
    needField(g_r.weaponSightField, g_r.weaponClass, "mSight");
    needField(g_r.animalHeadTransform, g_r.animalClass, "HeadTransform");
    needField(g_r.animalCurrentHealth, g_r.animalClass, "mCurrentHealth");
    needField(g_r.animalHealth, g_r.animalClass, "mHealth");
    needField(g_r.animalAlive, g_r.animalClass, "mAnimalALive");
    needField(g_r.animalDataField, g_r.animalClass, "mAnimalData");
    needField(g_r.animalDataDisplayName, g_r.animalDataClass,
              "displayName");
    needField(g_r.sightZoomAmount, g_r.sightClass, "mZoomAmount");
    needField(g_r.sightPower, g_r.sightClass, "mPower");
    needField(g_r.sightMaxPower, g_r.sightClass, "mMaxPower");
    needField(g_r.wdMaxRecoil, g_r.weaponDataClass, "maxRecoil");
    needField(g_r.wdShake, g_r.weaponDataClass, "Shake");
    needField(g_r.wdShock, g_r.weaponDataClass, "shock");
    needField(g_r.wdMinReticle, g_r.weaponDataClass, "minReticleRadius");
    needField(g_r.wdMaxReticle, g_r.weaponDataClass, "maxReticleRadius");
    needField(g_r.pdmEnergy, g_r.pdmClass, "currentPlayerEnergy");

    // Core = yang WAJIB ada supaya pompa + ESP + fitur utama jalan.
    // BUKAN core (fitur terkait saja yang mati, status PARTIAL):
    // AnimalData/displayName (nama ESP), get_IsALive/get_Position/
    // get_CenterPosition (ada fallback field), get_ScreenCenter
    // (aimbot saja — HOOK=gagal), AddCoinsOne (tombol koin saja),
    // currentPlayerEnergy (energi saja).
    bool core = g_r.weaponClass && g_r.animalClass && g_r.sightClass &&
                g_r.weaponDataClass && g_r.playerCameraClass &&
                g_r.pdmClass && g_r.cameraClass && g_r.transformClass &&
                g_r.objectClass && g_r.componentClass && g_r.screenClass &&
                (g_r.objectFindObjects || g_r.objectFindObjectsByType) &&
                g_r.cameraGetMain && g_r.cameraWorldToScreen &&
                g_r.transformGetPosition && g_r.componentGetTransform &&
                g_r.screenGetWidth && g_r.screenGetHeight &&
                g_r.playerCameraUpdate && g_r.weaponBulletsNum &&
                g_r.weaponIsScope && g_r.weaponDataField &&
                g_r.weaponSightField && g_r.animalHeadTransform &&
                g_r.animalCurrentHealth && g_r.animalHealth &&
                g_r.animalAlive && g_r.animalDataField &&
                g_r.sightZoomAmount && g_r.sightPower && g_r.sightMaxPower &&
                g_r.wdMaxRecoil && g_r.wdShake && g_r.wdShock &&
                g_r.wdMinReticle && g_r.wdMaxReticle;
    g_coreResolved = core;
    return core;
}

// Guard stub (pelajaran Dobby — jangan hook fungsi ≤8 byte): method
// yang asli punya prolog STP; bila instruksi pertama RET (0xD65F03C0)
// atau B . (0x14000000), lewati — jangan rusak game.
bool is_stub(void* addr) {
    if (!addr) return true;
    uint32_t first = *reinterpret_cast<volatile uint32_t*>(addr);
    return first == 0xD65F03C0u || first == 0x14000000u;
}

// ------------------------------------------------------- hook aimbot
// get_ScreenCenter dipakai Weapon.Shoot / SpecialShoot untuk menghitung
// titik layar asal ray (lihat disassembly Shoot 0x8f2f2c: titik =
// ScreenCenter + reticleRadius x insideUnitCircle, lalu
// ScreenPointToRay + RaycastAll). Hook mengembalikan posisi layar
// kepala target bila aimbot aktif dan target masih segar (<300 ms);
// kalau tidak, original dipanggil. Efek samping: reticle (SightReticle.
// CaluAngel, pemanggil get_ScreenCenter yang lain) ikut menunjuk ke
// kepala saat aimbot ON — kosmetik, bisa diterima.
Vector2 hook_ScreenCenter(void* thiz, void* method) {
    void* mp = method
                   ? reinterpret_cast<Il2CppMethodOpaque*>(method)
                         ->methodPointer
                   : nullptr;
    ScreenCenterFn orig = nullptr;
    if (mp) {
        auto it = g_origScreenCenter.find(mp);
        if (it != g_origScreenCenter.end()) orig = it->second;
    } else if (g_origScreenCenter.size() == 1) {
        orig = g_origScreenCenter.begin()->second;
    }
    bool wantAim = false;
    CachedTarget t;
    int sw = 0, sh = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        wantAim = g_config.aimbot || (g_config.zoomAim && g_isScope);
        t = g_target;
        sw = g_screenW;
        sh = g_screenH;
    }
    if (wantAim && t.valid && (now_ms() - t.timestampMs) < 300) {
        return Vector2{t.screenX, t.screenY};
    }
    if (orig) return orig(thiz, method);
    // Orig tak dikenal — kembalikan tengah layar (fallback aman: game
    // menembak ke tengah seperti tanpa aimbot).
    return Vector2{sw > 0 ? sw / 2.0f : 0.0f, sh > 0 ? sh / 2.0f : 0.0f};
}

void try_install_aimbot_hook() {
    if (g_aimbotHookInstalled) return;
    Il2CppMethodOpaque* m = g_r.playerCameraGetScreenCenter;
    if (!m) {
        add_failed("method:get_ScreenCenter");
        return;
    }
    void* addr = method_ptr(m);
    if (!addr) return;
    if (g_origScreenCenter.count(addr)) return;
    if (is_stub(addr)) {
        add_failed("method:get_ScreenCenter-stub");
        return;
    }
    ScreenCenterFn orig = nullptr;
    int rc = DobbyHook(addr, reinterpret_cast<void*>(&hook_ScreenCenter),
                       reinterpret_cast<void**>(&orig));
    if (rc == 0 && orig) {
        g_origScreenCenter[addr] = orig;
        g_aimbotHookInstalled = true;
        LOGI("PlayerCamera.get_ScreenCenter hooked at %p", addr);
    } else {
        LOGE("DobbyHook get_ScreenCenter failed rc=%d", rc);
    }
}

// ------------------------------------------------- enumerasi + target
// Jalan di pompa (main thread) tiap 100 ms bila berburu stabil.
// Mengisi g_entries (ESP) + g_target (aimbot) + g_isScope.
void enumerate_and_target() {
    // isScope dari Weapon pertama (untuk hook zoomAim).
    bool sc = false;
    auto weapons = find_objects(g_r.weaponClass);
    if (!weapons.empty() && g_api.object_get_class(weapons[0])) {
        field_get(weapons[0], g_r.weaponIsScope, sc);
    }
    Il2CppObject* cam = invoke(g_r.cameraGetMain, nullptr, nullptr);
    if (!cam) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_isScope = sc;
        g_entries.clear();
        g_target.valid = false;
        return;
    }
    Vector3 camPos{0, 0, 0};
    Il2CppObject* camTr = invoke(g_r.componentGetTransform, cam, nullptr);
    if (camTr) {
        invoke_value(g_r.transformGetPosition, camTr, nullptr, camPos);
    }
    int sw = 0, sh = 0;
    invoke_value(g_r.screenGetWidth, nullptr, nullptr, sw);
    invoke_value(g_r.screenGetHeight, nullptr, nullptr, sh);
    if (sw <= 0 || sh <= 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_isScope = sc;
        g_entries.clear();
        g_target.valid = false;
        return;
    }
    auto animals = find_objects(g_r.animalClass);
    std::vector<EspEntry> entries;
    CachedTarget best;
    float bestScore = 1e9f;
    const float cx = sw / 2.0f, cy = sh / 2.0f;
    for (auto* a : animals) {
        if (!a || !g_api.object_get_class(a)) continue;
        bool alive = true;
        if (g_r.animalGetIsALive) {
            bool v = false;
            if (invoke_value(g_r.animalGetIsALive, a, nullptr, v)) {
                alive = v;
            } else {
                field_get(a, g_r.animalAlive, alive);
            }
        } else {
            field_get(a, g_r.animalAlive, alive);
        }
        if (!alive) continue;
        float hp = 0, maxHp = 0;
        field_get(a, g_r.animalCurrentHealth, hp);
        field_get(a, g_r.animalHealth, maxHp);
        if (hp <= 0) continue;
        Il2CppObject* atr = invoke(g_r.componentGetTransform, a, nullptr);
        if (!atr) continue;
        Vector3 pos;
        if (!invoke_value(g_r.transformGetPosition, atr, nullptr, pos)) {
            continue;
        }
        // Kepala: HeadTransform → get_position; fallback CenterPosition;
        // fallback posisi + 1,0 m.
        Vector3 head = pos;
        head.y += 1.0f;
        Il2CppObject* headTr = nullptr;
        field_get(a, g_r.animalHeadTransform, headTr);
        if (headTr && g_api.object_get_class(headTr)) {
            Vector3 hp2;
            if (invoke_value(g_r.transformGetPosition, headTr, nullptr,
                             hp2)) {
                head = hp2;
            }
        } else if (g_r.animalGetCenterPosition) {
            Vector3 cp;
            if (invoke_value(g_r.animalGetCenterPosition, a, nullptr, cp)) {
                head = cp;
            }
        }
        // Proyeksi kepala ke layar.
        Vector3 sp;
        void* args[1] = {&head};
        if (!invoke_value(g_r.cameraWorldToScreen, cam, args, sp)) continue;
        if (sp.z <= 0) continue;  // di belakang kamera
        float sx = sp.x;
        float sy = sh - sp.y;  // Unity y dari bawah → overlay y dari atas
        if (sx < 0 || sx > sw || sy < 0 || sy > sh) continue;
        float dx = pos.x - camPos.x, dy = pos.y - camPos.y,
              dz = pos.z - camPos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist <= 0.01f) continue;
        // Kotak ESP: tinggi hewan ±1,8 m diproyeksikan.
        float h = 1.8f * sh / dist;
        float w = h * 0.6f;
        std::string name;
        Il2CppObject* ad = nullptr;
        field_get(a, g_r.animalDataField, ad);
        if (ad && g_api.object_get_class(ad)) {
            Il2CppObject* nameObj = nullptr;
            field_get(ad, g_r.animalDataDisplayName, nameObj);
            name = read_il2cpp_string(nameObj);
        }
        EspEntry e;
        e.x = sx;
        e.y = sy;
        e.w = w;
        e.h = h;
        e.hp = maxHp > 0 ? hp / maxHp : 1.0f;
        e.dist = dist;
        e.name = name;
        entries.push_back(e);
        float sdx = sx - cx, sdy = sy - cy;
        float score = sdx * sdx + sdy * sdy;
        if (score < bestScore) {
            bestScore = score;
            best.headPos = head;
            best.camPos = camPos;
            best.screenX = sx;
            best.screenY = sy;
            best.timestampMs = now_ms();
            best.valid = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_isScope = sc;
        g_screenW = sw;
        g_screenH = sh;
        g_entries = entries;
        g_target = best;
    }
}

// ------------------------------------------------------------ fitur
// Jalan di pompa (main thread) tiap 100 ms bila berburu stabil.
// Restore nilai asli saat toggle dimatikan (bukan toggle palsu).
void apply_features(const Config& cfg, const Config& prev) {
    auto weapons = find_objects(g_r.weaponClass);
    if (weapons.empty()) return;
    Il2CppObject* weapon = weapons[0];
    if (!g_api.object_get_class(weapon)) return;
    // No reload: tahan bulletsNum di 99 (magasin penuh terus).
    if (cfg.noReload) {
        int v = 99;
        field_set(weapon, g_r.weaponBulletsNum, v);
    }
    // No recoil: nol-kan recoil + shake + shock WeaponData; reticle
    // maksimum disamakan dengan minimum (bidik tak mengembang).
    Il2CppObject* data = nullptr;
    field_get(weapon, g_r.weaponDataField, data);
    if (data && g_api.object_get_class(data)) {
        if (cfg.noRecoil) {
            if (!g_wdOrig.saved) {
                field_get(data, g_r.wdMaxRecoil, g_wdOrig.maxRecoil);
                field_get(data, g_r.wdShake, g_wdOrig.shake);
                field_get(data, g_r.wdShock, g_wdOrig.shock);
                field_get(data, g_r.wdMinReticle, g_wdOrig.minReticle);
                field_get(data, g_r.wdMaxReticle, g_wdOrig.maxReticle);
                g_wdOrig.saved = true;
            }
            float zero = 0.0f;
            field_set(data, g_r.wdMaxRecoil, zero);
            field_set(data, g_r.wdShake, zero);
            field_set(data, g_r.wdShock, zero);
            field_set(data, g_r.wdMaxReticle, g_wdOrig.minReticle);
        } else if (prev.noRecoil && g_wdOrig.saved) {
            field_set(data, g_r.wdMaxRecoil, g_wdOrig.maxRecoil);
            field_set(data, g_r.wdShake, g_wdOrig.shake);
            field_set(data, g_r.wdShock, g_wdOrig.shock);
            field_set(data, g_r.wdMinReticle, g_wdOrig.minReticle);
            field_set(data, g_r.wdMaxReticle, g_wdOrig.maxReticle);
            g_wdOrig.saved = false;
        }
    }
    // Zoom jauh: Sight.mPower = mMaxPower + mZoomAmount = 30.
    Il2CppObject* sight = nullptr;
    field_get(weapon, g_r.weaponSightField, sight);
    if (sight && g_api.object_get_class(sight)) {
        if (cfg.zoomFar) {
            if (!g_sightOrig.saved) {
                field_get(sight, g_r.sightPower, g_sightOrig.power);
                field_get(sight, g_r.sightZoomAmount, g_sightOrig.zoomAmount);
                g_sightOrig.saved = true;
            }
            float maxPow = 0;
            field_get(sight, g_r.sightMaxPower, maxPow);
            if (maxPow <= 0) maxPow = 8.0f;
            field_set(sight, g_r.sightPower, maxPow);
            float z = 30.0f;
            field_set(sight, g_r.sightZoomAmount, z);
        } else if (prev.zoomFar && g_sightOrig.saved) {
            field_set(sight, g_r.sightPower, g_sightOrig.power);
            field_set(sight, g_r.sightZoomAmount, g_sightOrig.zoomAmount);
            g_sightOrig.saved = false;
        }
    }
    // Energi tak habis: static PlayerDataManager.currentPlayerEnergy
    // ditahan di 15 (Max_Energy) tiap tick.
    if (cfg.energy && g_r.pdmEnergy && g_api.field_static_set_value) {
        int e = 15;
        g_api.field_static_set_value(g_r.pdmEnergy, &e);
    }
}

// Perintah koin dari aplikasi (file, sekali tap): {"addcoins":1000}.
// Dijalankan DI POMPA (main thread) supaya AddCoinsOne (+ simpan)
// jalan di thread Unity. File dihapus dulu — jangan grant 2x.
void check_coins_command() {
    std::string cmd = read_file(kCmdPath);
    if (cmd.empty()) return;
    int n = json_int(cmd, "addcoins");
    ::remove(kCmdPath);
    if (n > 0 && g_r.pdmAddCoinsOne) {
        void* args[1] = {&n};
        invoke(g_r.pdmAddCoinsOne, nullptr, args);
        LOGI("koin gameplay +%d diberikan", n);
    } else if (n > 0) {
        LOGW("AddCoinsOne tidak ter-resolve — koin gagal");
    }
}

// -------------------------------------------------------------- pompa
// Hook PlayerCamera.Update → original dulu (state game fresh), baru
// pompa. Crash tes Sniper v1.0.5 pindah ke "loading mau ke ingame" —
// saat transisi scene, objek dibuat/dihancurkan dan FindObjectsOfType
// di tengah transisi tersangka utamanya. Karena itu enumerate/apply/
// hook aimbot HANYA jalan bila berburu sudah stabil >= 2000 ms;
// selama transisi, pompa hanya mengecek "Weapon ada?" tiap 500 ms.
void pump_main_thread() {
    if (!g_coreResolved) return;
    long long t = now_ms();
    g_lastPumpMs.store(t);
    g_pumpCount.fetch_add(1);
    Config cfg;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        cfg = g_configPump;
    }
    if (t - g_lastActiveCheckMs >= 500) {
        bool active = !find_objects(g_r.weaponClass).empty();
        if (active) {
            if (g_activeSinceMs == 0) g_activeSinceMs = t;
        } else {
            g_activeSinceMs = 0;
        }
        g_lastActiveCheckMs = t;
    }
    const bool activeStabil =
        g_activeSinceMs != 0 && t - g_activeSinceMs >= 2000;
    if (activeStabil) {
        if (t - g_lastEnumerateMs >= 100) {
            enumerate_and_target();
            apply_features(cfg, g_prevPumpConfig);
            g_lastEnumerateMs = t;
        }
    } else {
        std::lock_guard<std::mutex> lock(g_mu);
        g_target.valid = false;
    }
    if (activeStabil && t - g_lastAimbotHookMs >= 2000) {
        try_install_aimbot_hook();
        g_lastAimbotHookMs = t;
    }
    if (t - g_lastCmdCheckMs >= 2000) {
        check_coins_command();
        g_lastCmdCheckMs = t;
    }
    g_prevPumpConfig = cfg;
}

void hook_PlayerCameraUpdate(void* thiz, void* method) {
    void* mp = method
                   ? reinterpret_cast<Il2CppMethodOpaque*>(method)
                         ->methodPointer
                   : nullptr;
    UpdateFn orig = nullptr;
    if (mp) {
        auto it = g_origUpdate.find(mp);
        if (it != g_origUpdate.end()) orig = it->second;
    } else if (g_origUpdate.size() == 1) {
        orig = g_origUpdate.begin()->second;
    }
    if (orig) orig(thiz, method);  // original dulu
    // Bila orig tak dikenal, satu frame Update terlewat (aman) — pompa
    // tetap jalan, ia tidak bergantung pada orig.
    pump_main_thread();
}

void try_install_pump_hook() {
    if (g_hookInstalled) return;
    Il2CppMethodOpaque* m = g_r.playerCameraUpdate;
    if (!m) {
        add_failed("method:PlayerCameraUpdate");
        return;
    }
    void* addr = method_ptr(m);
    if (!addr) return;
    if (g_origUpdate.count(addr)) return;
    if (is_stub(addr)) {
        add_failed("method:PlayerCameraUpdate-stub");
        return;
    }
    UpdateFn orig = nullptr;
    int rc = DobbyHook(addr, reinterpret_cast<void*>(&hook_PlayerCameraUpdate),
                       reinterpret_cast<void**>(&orig));
    if (rc == 0 && orig) {
        g_origUpdate[addr] = orig;
        g_hookInstalled = true;
        LOGI("PlayerCamera.Update hooked at %p", addr);
    } else {
        LOGE("DobbyHook PlayerCamera.Update failed rc=%d", rc);
    }
}

// Buat dir external game per komponen (abaikan EEXIST) —
// /sdcard/Android/data biasanya sudah ada; yang belum ada hanya
// .../com.julian.wildhunter/files.
void mkdir_p_ext() {
    const char* parts[] = {
        "/sdcard/Android",
        "/sdcard/Android/data",
        "/sdcard/Android/data/com.julian.wildhunter",
        "/sdcard/Android/data/com.julian.wildhunter/files",
    };
    for (const char* p : parts) {
        mkdir(p, 0777);
    }
}

// ------------------------------------------------------------ ESP
// Ditulis worker tiap 150 ms; dibaca FloatingService (Kotlin) tiap
// 150 ms. Format SAMA dengan payload Sniper (EspView: x/y = tengah,
// w/h = ukuran, hp = rasio, n = nama, d = jarak meter).
void write_esp() {
    Config cfg;
    std::vector<EspEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        cfg = g_config;
        entries = g_entries;
    }
    auto empty = []() {
        write_file(kEspPath, "{\"mode\":\"OFFLINE\",\"t\":[]}");
    };
    if (!cfg.esp) {
        empty();
        return;
    }
    long long lp = g_lastPumpMs.load();
    if (lp == 0 || now_ms() - lp > 1000) {
        empty();
        return;
    }
    std::string s = "{\"mode\":\"OFFLINE\",\"t\":[";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i) s += ",";
        const auto& e = entries[i];
        s += "{\"x\":" + std::to_string(static_cast<int>(e.x)) +
             ",\"y\":" + std::to_string(static_cast<int>(e.y)) +
             ",\"w\":" + std::to_string(static_cast<int>(e.w)) +
             ",\"h\":" + std::to_string(static_cast<int>(e.h)) +
             ",\"hp\":" + std::to_string(e.hp) +
             ",\"n\":\"";
        for (char c : e.name) {
            if (c == '"' || c == '\\') s += '\\';
            s += c;
        }
        s += "\",\"d\":" + std::to_string(static_cast<int>(e.dist)) + "}";
    }
    s += "]}";
    write_file(kEspPath, s);
}

// ---------------------------------------------------------- config
void read_config() {
    std::string j = read_file(kCfgPath);
    if (j.empty()) return;
    Config c;
    c.aimbot = json_bool(j, "aimbot");
    c.zoomAim = json_bool(j, "zoomaim");
    c.esp = json_bool(j, "esp");
    c.noRecoil = json_bool(j, "norecoil");
    c.noReload = json_bool(j, "noreload");
    c.zoomFar = json_bool(j, "zoom");
    c.energy = json_bool(j, "energy");
    std::lock_guard<std::mutex> lock(g_mu);
    g_config = c;
    g_configPump = c;
}

// ---------------------------------------------------- penangkap crash
// Async-signal-safe: hanya open/write/close + clock_gettime + atomic
// load. Bukti crash ditulis ke file TERPISAH (tidak ditimpa worker) +
// STATUS=CRASH ke 3 kanal status (worker mempertahankannya — lihat
// write_status). Pelajaran Sniper v1.0.7: tanpa file terpisah, bukti
// crash hilang karena worker menulis PARTIAL tiap 2 dtk.
void crash_handler(int sig) {
    char buf[128];
    char* p = buf;
    auto put = [&](const char* s) {
        while (*s) *p++ = *s++;
    };
    auto put_ll = [&](long long v) {
        char tmp[24];
        int n = 0;
        bool neg = v < 0;
        unsigned long long u =
            neg ? static_cast<unsigned long long>(-(v + 1)) + 1
                : static_cast<unsigned long long>(v);
        if (u == 0) tmp[n++] = '0';
        while (u) {
            tmp[n++] = static_cast<char>('0' + u % 10);
            u /= 10;
        }
        if (neg) *p++ = '-';
        while (n) *p++ = tmp[--n];
    };
    put("{\"signal\":");
    put_ll(sig);
    put(",\"pump\":");
    put_ll(static_cast<long long>(g_pumpCount.load()));
    put(",\"t\":");
    put_ll(now_ms());
    put("}\n");
    int fd = open(kCrashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, buf, static_cast<size_t>(p - buf));
        close(fd);
    }
    const char* msg =
        "STATUS=CRASH\nFAILED=signal\nLIBSIZE=-\nMODE=OFFLINE\nHOOK=gagal\n";
    fd = open(kStatusCache, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, msg, std::strlen(msg));
        close(fd);
    }
    fd = open(kStatusTmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, msg, std::strlen(msg));
        close(fd);
    }
    fd = open(kStatusExt, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, msg, std::strlen(msg));
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// ----------------------------------------------------------- worker
void* worker_main(void*) {
    // Jejak pertama SEBELUM resolve: bila crash terjadi saat resolve,
    // status INIT tetap ada (v1.0.2 Sniper: tanpa ini, crash sebelum
    // READY tidak meninggalkan jejak apa pun).
    mkdir_p_ext();
    g_statusOverride = "INIT";
    write_status();
    // Hapus bukti crash run lama — run ini baru; penangkap crash akan
    // menulis ulang bila crash terjadi lagi.
    ::remove(kCrashPath);
    // Resolve (retry s.d. 60 dtk — libil2cpp mungkin belum siap saat
    // payload di-dlopen di awal start game).
    long long start = now_ms();
    while (!g_coreResolved) {
        if (load_il2cpp() && resolve_all()) break;
        if (now_ms() - start > 60000) {
            LOGE("resolve gagal setelah 60 dtk — UNSUPPORTED");
            g_statusOverride = "";
            write_status();
            return nullptr;
        }
        usleep(500 * 1000);
    }
    g_statusOverride = "";
    // Thread attach untuk runtime_invoke dari worker (find_objects
    // di try_install_* tidak dipakai — hook install hanya butuh
    // methodPointer, tanpa API Unity).
    Il2CppDomain* domain = g_api.domain_get();
    if (domain) g_api.thread_attach(domain);
    write_status();
    LOGI("resolve selesai: %s (failed=%zu)",
         g_failedList.empty() ? "READY" : "PARTIAL", g_failedList.size());
    read_config();

    // Worker thread TIDAK LAGI memanggil API Unity apa pun (pelajaran
    // Sniper v1.0.4: worker panggil API Unity = crash). Yang tersisa:
    // baca file config + tulis file status/ESP (I/O file biasa, aman
    // dari thread mana pun) + pasang hook pompa (hanya resolve metadata
    // + DobbyHook, tanpa API Unity). SEMUA FindObjectsOfType /
    // get_position / WorldToScreenPoint / invoke / field-get-set jalan
    // di pompa main-thread.
    while (true) {
        long long t = now_ms();
        if (t - g_lastConfigReadMs >= 500) {
            read_config();
            g_lastConfigReadMs = t;
        }
        if (t - g_lastHookCheckMs >= 2000) {
            try_install_pump_hook();
            g_lastHookCheckMs = t;
        }
        if (t - g_lastEspWriteMs >= 150) {
            write_esp();
            g_lastEspWriteMs = t;
        }
        if (t - g_lastStatusWriteMs >= 2000) {
            write_status();
            g_lastStatusWriteMs = t;
        }
        usleep(100 * 1000);
    }
    return nullptr;
}

}  // namespace

namespace wildhunter {

void start_worker() {
    // Pasang penangkap crash SEBELUM worker dibuat (crash bisa terjadi
    // di thread mana pun, termasuk saat resolve).
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGTRAP, crash_handler);
    signal(SIGSYS, crash_handler);
    pthread_t th;
    if (pthread_create(&th, nullptr, worker_main, nullptr) == 0) {
        pthread_detach(th);
    }
}

}  // namespace wildhunter
