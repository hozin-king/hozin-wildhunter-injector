# Hozin Wild Hunter 3D Injector — native build (Gradle externalNativeBuild).
# Membangun 2 file ke dalam APK (arm64-v8a):
#   libpayload.so  — payload yang di-dlopen ke proses Wild Hunter 3D
#   libinjector.so — PIE executable (ptrace remote dlopen), dijalankan root
# Payload di-static-link STL (-static-libstdc++) seperti TR/HCR: game
# target tidak bisa dijamin membundel libc++_shared.so, dan namespace
# linker game tidak bisa resolve SONAME dari /data/local/tmp.
cmake_minimum_required(VERSION 3.22.1)
project(HozinWildHunterInjector CXX C)

include(FetchContent)

# Dobby (hook library) — fetched at build time, NOT vendored.
# Commit 5dfc8546954ce3b3198132ab13fddb89ee92cdd7 dipin (pelajaran Dobby
# 10 Okt 2026: upstream master BROKEN untuk NDK modern — butuh 5 patch).
FetchContent_Declare(
    dobby
    URL https://github.com/jmpews/Dobby/archive/5dfc8546954ce3b3198132ab13fddb89ee92cdd7.zip
)
FetchContent_GetProperties(dobby)
if(NOT dobby_POPULATED)
    FetchContent_Populate(dobby)

    # ---- Patch Dobby: upstream 5dfc854 is "broken" for modern NDK ----

    # 1. closure_bridge_arm64.asm: Mach-O "@PAGE"/"@PAGEOFF" syntax is
    #    rejected by the LLVM assembler on ELF targets.
    set(_f "${dobby_SOURCE_DIR}/source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm")
    file(READ "${_f}" _c)
    string(REPLACE
        "adrp TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGE\nadd TMP_REG_0, TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGEOFF\nblr TMP_REG_0"
        "ldr TMP_REG_0, common_closure_bridge_handler_addr\nblr TMP_REG_0"
        _c "${_c}")
    file(WRITE "${_f}" "${_c}")

    # 2. common/os_arch_features.h: break a circular include
    #    (platform.h -> dobby/common.h -> this file -> platform.h).
    set(_f "${dobby_SOURCE_DIR}/common/os_arch_features.h")
    file(READ "${_f}" _c)
    string(REPLACE
        "#include <sys/types.h>\n#include <stddef.h>\n#include \"pac_kit.h\"\n\n#include \"PlatformUnifiedInterface/platform.h\""
        "#include <sys/types.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <unistd.h>\n#include <sys/mman.h>\n#include \"pac_kit.h\""
        _c "${_c}")
    string(REPLACE
        "  auto page = (void *)ALIGN_FLOOR(address, OSMemory::PageSize());\n  if (!OSMemory::SetPermission(page, OSMemory::PageSize(), kReadExecute)) {\n    return;\n  }"
        "  (void)size;\n  long page_size = sysconf(_SC_PAGESIZE);\n  if (page_size <= 0)\n    return;\n  void *page = (void *)((uintptr_t)address & ~((uintptr_t)page_size - 1));\n  mprotect(page, (size_t)page_size, PROT_READ | PROT_EXEC);"
        _c "${_c}")
    file(WRITE "${_f}" "${_c}")

    # 3. code-patch-tool-posix.cc: upstream removed "core/arch/Cpu.h"
    #    but left the include behind ("file not found").
    set(_f "${dobby_SOURCE_DIR}/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc")
    file(READ "${_f}" _c)
    string(REPLACE
        "#include \"core/arch/Cpu.h\""
        "// (patched: hozin-wildhunter) upstream removed core/arch/Cpu.h but the include stayed"
        _c "${_c}")
    file(WRITE "${_f}" "${_c}")

    # 4. ProcessRuntime.cc: a.start is a method, and the RuntimeModule
    #    field was refactored upstream from "load_address" to "base".
    set(_f "${dobby_SOURCE_DIR}/source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntime.cc")
    file(READ "${_f}" _c)
    string(REPLACE "return (a.start < b.start);" "return (a.start() < b.start());" _c "${_c}")
    string(REPLACE "module.load_address" "module.base" _c "${_c}")
    file(WRITE "${_f}" "${_c}")

    # 5. SymbolResolver plugin: same "load_address" -> "base" refactor.
    set(_f "${dobby_SOURCE_DIR}/builtin-plugin/SymbolResolver/elf/dobby_symbol_resolver.cc")
    file(READ "${_f}" _c)
    string(REPLACE "load_address" "base" _c "${_c}")
    file(WRITE "${_f}" "${_c}")

    message(STATUS "[hozin-wildhunter] Dobby patches applied")

    add_subdirectory(${dobby_SOURCE_DIR} ${dobby_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# --- payload: library yang di-inject ke proses game ---
add_library(payload SHARED
    payload/payload_main.cpp
    payload/wildhunter.cpp
)
target_include_directories(payload PRIVATE ${dobby_SOURCE_DIR}/include)
target_link_libraries(payload log dobby_static)
target_link_options(payload PRIVATE -static-libstdc++)
set_target_properties(payload PROPERTIES OUTPUT_NAME "payload")

# --- injector: standalone PIE binary, dijalankan root via `su -c`. ---
# Output dinamai .so supaya AGP membundelnya ke lib/<ABI>/, bukan
# membuangnya. Confirm setelah build: lib/arm64-v8a/libinjector.so ada
# di APK dan berukuran seperti binary asli (bukan 0 byte).
# v1.0.4: mekanisme inject PINDAH TOTAL ke kerangka Traffic Rider
# (injector.c — port verbatim dari BUSSID, varian TR + hardening
# v1.0.1: validasi stack/pc + WIFSTOPPED). Menggantikan remote dlopen
# C++ injector lama (remote_call_arm64.cpp: brk di scratch RWX) yang tetap
# crash di POCO tanpa diagnosis. TR: path ditulis ke stack target,
# lr=0 -> SIGSEGV trap sebagai marker kembali (TANPA brk di mana pun),
# tanpa timeout — waitpid sampai trap.
add_executable(libinjector
    injector/injector.c
)
target_link_libraries(libinjector log dl)
set_target_properties(libinjector PROPERTIES SUFFIX ".so")
