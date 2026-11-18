// Minimal IL2CPP API declarations — hanya yang dipakai payload.
// Semua fungsi di-resolve runtime lewat dlsym dari libil2cpp.so yang
// sudah termuat di proses game (bukan link-time), dan semua class /
// method / field di-resolve berdasarkan NAMA lewat metadata IL2CPP —
// TIDAK ADA offset hardcoded. Bila game update dan sebuah nama hilang,
// resolve gagal dan payload melaporkan PARTIAL/UNSUPPORTED, bukan crash.
#pragma once

#include <cstddef>
#include <cstdint>

typedef struct Il2CppDomain Il2CppDomain;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppImage Il2CppImage;
typedef struct Il2CppClass Il2CppClass;
typedef struct Il2CppType Il2CppType;
typedef struct Il2CppMethod Il2CppMethod;
typedef struct Il2CppField Il2CppField;
typedef struct Il2CppObject Il2CppObject;
typedef struct Il2CppString Il2CppString;
typedef struct Il2CppArray Il2CppArray;  // opaque — hook hanya mengembalikan pointer-nya

// Il2CppMethod: methodPointer adalah member PERTAMA di header publik
// IL2CPP (il2cpp-class.h) dan stabil lintas versi — ini yang di-hook
// Dobby. Sisanya tidak dibaca payload, jadi sengaja opaque.
struct Il2CppMethodOpaque {
    void* methodPointer;
};

struct Il2CppObjectHeader {
    void* klass;
    void* monitor;
};

// Il2CppArray: header 32 byte di arm64 (klass 8 + monitor 8 + bounds 8
// + max_length 8); elemen mengikuti tepat setelah header.
struct Il2CppArrayHeader {
    Il2CppObjectHeader obj;
    void* bounds;
    size_t max_length;
};

struct Vector3 {
    float x, y, z;
};

struct Quaternion {
    float x, y, z, w;
};

// Ray Unity = 24 byte (origin + direction) — di AAPCS64 dilewat sebagai
// pointer ke salinan pemanggil, jadi hook bisa menulis ulang isinya.
struct Ray {
    Vector3 origin;
    Vector3 direction;
};

typedef Il2CppDomain* (*Fn_domain_get)();
typedef void* (*Fn_thread_attach)(Il2CppDomain* domain);
typedef const Il2CppAssembly** (*Fn_domain_get_assemblies)(Il2CppDomain* domain, size_t* size);
typedef Il2CppImage* (*Fn_assembly_get_image)(const Il2CppAssembly* assembly);
typedef Il2CppClass* (*Fn_class_from_name)(Il2CppImage* image, const char* namespaze, const char* name);
typedef Il2CppMethod* (*Fn_class_get_method_from_name)(Il2CppClass* klass, const char* name, int argsCount);
typedef Il2CppField* (*Fn_class_get_field_from_name)(Il2CppClass* klass, const char* name);
typedef Il2CppClass* (*Fn_class_get_parent)(Il2CppClass* klass);
typedef Il2CppType* (*Fn_class_get_type)(Il2CppClass* klass);
typedef Il2CppObject* (*Fn_type_get_object)(Il2CppType* type);
typedef Il2CppObject* (*Fn_runtime_invoke)(Il2CppMethod* method, void* obj, void** params, void** exc);
typedef void* (*Fn_object_unbox)(Il2CppObject* obj);
typedef Il2CppClass* (*Fn_object_get_class)(Il2CppObject* obj);
typedef size_t (*Fn_field_get_offset)(Il2CppField* field);
typedef void (*Fn_field_get_value)(Il2CppObject* obj, Il2CppField* field, void* value);
typedef void (*Fn_field_set_value)(Il2CppObject* obj, Il2CppField* field, void* value);
typedef void (*Fn_field_static_get_value)(Il2CppField* field, void* value);
typedef void (*Fn_field_static_set_value)(Il2CppField* field, void* value);
typedef uint32_t (*Fn_array_length)(void* array);
