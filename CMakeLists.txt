cmake_minimum_required(VERSION 3.16)
project(agentvfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Version reported by `agentvfs --version`. Override at configure time:
#   cmake -B build -DAGENTVFS_VERSION_STRING=0.1.2
# release.yml passes the tag (with the leading 'v' stripped) here.
if(NOT DEFINED AGENTVFS_VERSION_STRING)
    set(AGENTVFS_VERSION_STRING "0.1.2-dev")
endif()
add_compile_definitions(AGENTVFS_VERSION_STRING="${AGENTVFS_VERSION_STRING}")

# Polyfill the CMake 3.25+ LINUX variable so the if(LINUX) gates below
# also work on the distros shipping CMake 3.16–3.24 (Ubuntu 22.04 = 3.22,
# Debian 11 = 3.18, RHEL 9 = 3.20). APPLE / WIN32 / UNIX are defined by
# every supported CMake version, so they don't need a polyfill.
if(NOT DEFINED LINUX)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(LINUX TRUE)
    else()
        set(LINUX FALSE)
    endif()
endif()

set(AGENTVFS_SUPPORTED_BUILD_TYPES Debug Release RelWithDebInfo MinSizeRel)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${AGENTVFS_SUPPORTED_BUILD_TYPES})
    list(FIND AGENTVFS_SUPPORTED_BUILD_TYPES "${CMAKE_BUILD_TYPE}" AGENTVFS_BUILD_TYPE_INDEX)
    if(AGENTVFS_BUILD_TYPE_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Unsupported CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. "
            "Expected one of: ${AGENTVFS_SUPPORTED_BUILD_TYPES}")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BPF_BUILD_FLAGS -O2 -g -DAGENTVFS_BPF_DEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(BPF_BUILD_FLAGS -O3 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(BPF_BUILD_FLAGS -O3 -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(BPF_BUILD_FLAGS -Os -g)
else()
    set(BPF_BUILD_FLAGS -O3 -g)
endif()

find_package(Threads REQUIRED)
# PkgConfig + libfuse3 are only used to build the Linux daemon. The
# macOS daemon block does its own PkgConfig lookup for fuse-t inside
# the if(APPLE AND AGENTVFS_FUSE_T) gate. Windows has neither.
if(LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE3 REQUIRED fuse3)
endif()

option(AGENTVFS_EBPF "Build with eBPF telemetry support" ON)
option(AGENTVFS_FANOTIFY "Build with fanotify telemetry backend" OFF)
option(AGENTVFS_PTRACE "Build with ptrace telemetry backend" OFF)
option(AGENTVFS_LDPRELOAD "Build with LD_PRELOAD telemetry backend" OFF)
option(AGENTVFS_BPFTIME "Build with ubpf/bpftime telemetry backend" OFF)
option(AGENTVFS_WASM "Build Wasm telemetry backend; execution requires WAMR and may be stubbed" OFF)
option(AGENTVFS_LUA "Build with Lua/LuaJIT telemetry backend" OFF)
option(AGENTVFS_ALL_BACKENDS "Enable all telemetry backends" OFF)
option(AGENTVFS_FUSE_T "Build the macOS daemon (requires fuse-t)"
       ${APPLE})
if(AGENTVFS_ALL_BACKENDS)
    set(AGENTVFS_EBPF ON CACHE BOOL "" FORCE)
    set(AGENTVFS_FANOTIFY ON CACHE BOOL "" FORCE)
    set(AGENTVFS_PTRACE ON CACHE BOOL "" FORCE)
    set(AGENTVFS_LDPRELOAD ON CACHE BOOL "" FORCE)
    set(AGENTVFS_BPFTIME ON CACHE BOOL "" FORCE)
    set(AGENTVFS_WASM ON CACHE BOOL "" FORCE)
    set(AGENTVFS_LUA ON CACHE BOOL "" FORCE)
endif()

# Force-OFF Linux-only telemetry backends on non-Linux
if(NOT LINUX)
    foreach(opt IN ITEMS AGENTVFS_EBPF AGENTVFS_FANOTIFY AGENTVFS_PTRACE
                         AGENTVFS_LDPRELOAD AGENTVFS_BPFTIME)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
endif()

option(AGENTVFS_WINFSP "Build the Windows daemon (requires WinFsp 2.0+ SDK)"
       ${WIN32})

if(WIN32 AND AGENTVFS_WINFSP)
    find_path(WINFSP_INCLUDE_DIR NAMES winfsp/winfsp.h
        PATHS "$ENV{ProgramFiles}/WinFsp/inc"
              "$ENV{ProgramFiles\(x86\)}/WinFsp/inc")
    set(WINFSP_LIB_NAME winfsp-x64.lib)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WINFSP_LIB_NAME winfsp-x86.lib)
    endif()
    find_file(WINFSP_LIBRARY NAMES ${WINFSP_LIB_NAME}
        PATHS "$ENV{ProgramFiles}/WinFsp/lib"
              "$ENV{ProgramFiles\(x86\)}/WinFsp/lib")
    if(NOT WINFSP_INCLUDE_DIR OR NOT WINFSP_LIBRARY)
        message(FATAL_ERROR
            "AGENTVFS_WINFSP=ON but WinFsp SDK not found. "
            "Install WinFsp 2.0+ from https://winfsp.dev.")
    endif()
endif()

# ── cas_core static library (always built) ─────────────────────────
set(CAS_CORE_PORTABLE_SOURCES
    src/cas/object_store.cpp
    src/cas/refs.cpp
    src/cas/working_tree.cpp
    src/cas/write_buffer.cpp
    src/cas/tree_serialize.cpp
    src/cas/commit.cpp
    src/cas/checkpoint.cpp
    src/cas/daemon.cpp
    src/cas/inode_map.cpp
    src/cas/bootstrap.cpp
    src/cas/branch_router.cpp
    src/cas/branch_name.cpp
    src/cas/branch_merge.cpp
    src/cas/runtime_state.cpp
    src/cas/agent_state.cpp
    src/cas/agent_state_service.cpp
    src/cas/runtime_supervisor.cpp
    src/cas/telemetry_event.cpp
    src/cas/telemetry_registry.cpp
    src/cas/control_protocol.cpp
    include/blake3/blake3.c
    include/blake3/blake3_dispatch.c
    include/blake3/blake3_portable.c
)
set(CAS_CORE_POSIX_SOURCES
    src/cas/control_socket.cpp
    src/cas/runtime_process_posix.cpp
)

option(AGENTVFS_BLAKE3_SIMD "Enable runtime-dispatched BLAKE3 SIMD paths" ON)

add_library(cas_core STATIC ${CAS_CORE_PORTABLE_SOURCES})
if(UNIX)
    target_sources(cas_core PRIVATE ${CAS_CORE_POSIX_SOURCES})
endif()
target_include_directories(cas_core PUBLIC src/cas include include/blake3)

if(AGENTVFS_BLAKE3_SIMD)
    set(_blake3_amd64_names amd64 AMD64 x86_64)
    set(_blake3_arm64_names aarch64 AArch64 arm64 ARM64 armv8 armv8a)

    if(CMAKE_SYSTEM_PROCESSOR IN_LIST _blake3_amd64_names)
        target_sources(cas_core PRIVATE
            include/blake3/blake3_sse2.c
            include/blake3/blake3_sse41.c
            include/blake3/blake3_avx2.c
            include/blake3/blake3_avx512.c)
        if(MSVC)
            # MSVC has no dedicated /arch:SSE4.1 — /arch:AVX is the
            # narrowest flag that turns on SSE4.1 codegen. /arch:SSE2 is
            # x86-only; on x64 SSE2 is the implicit baseline and MSVC
            # silently ignores the flag, which is harmless.
            set_source_files_properties(include/blake3/blake3_sse2.c
                PROPERTIES COMPILE_OPTIONS "/arch:SSE2")
            set_source_files_properties(include/blake3/blake3_sse41.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX")
            set_source_files_properties(include/blake3/blake3_avx2.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
            set_source_files_properties(include/blake3/blake3_avx512.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
        else()
            set_source_files_properties(include/blake3/blake3_sse2.c
                PROPERTIES COMPILE_OPTIONS "-msse2")
            set_source_files_properties(include/blake3/blake3_sse41.c
                PROPERTIES COMPILE_OPTIONS "-msse4.1")
            set_source_files_properties(include/blake3/blake3_avx2.c
                PROPERTIES COMPILE_OPTIONS "-mavx2")
            set_source_files_properties(include/blake3/blake3_avx512.c
                PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR IN_LIST _blake3_arm64_names)
        target_sources(cas_core PRIVATE include/blake3/blake3_neon.c)
        # 64-bit aarch64 has NEON unconditionally; no -mfpu flag needed.
        # blake3_dispatch.c picks NEON automatically when BLAKE3_USE_NEON
        # is unset (the upstream default).
    else()
        # Unknown arch — fall back to portable, same as
        # AGENTVFS_BLAKE3_SIMD=OFF.
        target_compile_definitions(cas_core PRIVATE
            BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2
            BLAKE3_USE_NEON=0)
    endif()
else()
    target_compile_definitions(cas_core PRIVATE
        BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2
        BLAKE3_USE_NEON=0)
endif()
if(NOT MSVC)
    target_compile_options(cas_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── eBPF build path (optional) ──────────────────────────────────────
if(AGENTVFS_EBPF)
    find_program(CLANG_BIN clang REQUIRED)
    find_program(BPFTOOL_BIN bpftool REQUIRED)
    pkg_check_modules(LIBBPF REQUIRED libbpf)

    set(VMLINUX_H ${CMAKE_CURRENT_BINARY_DIR}/vmlinux.h)
    add_custom_command(
        OUTPUT ${VMLINUX_H}
        COMMAND ${BPFTOOL_BIN} btf dump file /sys/kernel/btf/vmlinux format c > ${VMLINUX_H}
        COMMENT "Generating vmlinux.h from kernel BTF")

    set(CAS_BPF_SRC ${CMAKE_SOURCE_DIR}/src/cas/bpf/cas_probe.bpf.c)
    set(CAS_BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/cas_probe.bpf.o)
    add_custom_command(
        OUTPUT ${CAS_BPF_OBJ}
        COMMAND ${CLANG_BIN}
                -target bpf ${BPF_BUILD_FLAGS} -D__TARGET_ARCH_x86 -D__bpf__
                -I ${CMAKE_SOURCE_DIR}/src/cas/bpf
                -I ${CMAKE_CURRENT_BINARY_DIR}
                -c ${CAS_BPF_SRC} -o ${CAS_BPF_OBJ}
        DEPENDS ${CAS_BPF_SRC}
                ${CMAKE_SOURCE_DIR}/src/cas/bpf/cas_maps.h
                ${VMLINUX_H}
        COMMENT "Compiling cas BPF program")

    set(CAS_BPF_SKEL ${CMAKE_CURRENT_BINARY_DIR}/cas_probe.skel.h)
    add_custom_command(
        OUTPUT ${CAS_BPF_SKEL}
        COMMAND ${BPFTOOL_BIN} gen skeleton ${CAS_BPF_OBJ} name cas_probe > ${CAS_BPF_SKEL}
        DEPENDS ${CAS_BPF_OBJ}
        COMMENT "Generating cas BPF skeleton")
    add_custom_target(cas_bpf_skel DEPENDS ${CAS_BPF_SKEL})

    # The PUBLIC define lets cas_core's headers compile consistently and
    # downstream targets see the same AGENTVFS_EBPF state. cas_core itself
    # no longer references libbpf — that lives in EbpfPolicyInstaller in
    # the daemon binary — so no libbpf includes are needed here.
    target_compile_definitions(cas_core PUBLIC AGENTVFS_EBPF=1)
endif()

# ── agentvfs daemon (Linux) ─────────────────────────────────────────
if(LINUX)
add_executable(agentvfs
    src/cas/platform/linux/fuse_adapter.cpp
    src/cas/main.cpp
    src/cas/workspace_cli.cpp
    src/cas/ebpf_loader.cpp
    src/cas/telemetry_drain.cpp
    src/cas/backends/ebpf_backend.cpp
)
target_include_directories(agentvfs PRIVATE
    src/cas include ${FUSE3_INCLUDE_DIRS})
target_compile_options(agentvfs PRIVATE
    ${FUSE3_CFLAGS_OTHER})
if(NOT MSVC)
    target_compile_options(agentvfs PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(agentvfs PRIVATE
    cas_core ${FUSE3_LIBRARIES} Threads::Threads)

if(AGENTVFS_FANOTIFY)
    target_sources(agentvfs PRIVATE src/cas/backends/fanotify_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_FANOTIFY=1)
endif()

if(AGENTVFS_PTRACE)
    target_sources(agentvfs PRIVATE src/cas/backends/ptrace_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_PTRACE=1)
endif()

if(AGENTVFS_LDPRELOAD)
    target_sources(agentvfs PRIVATE src/cas/backends/ldpreload_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_LDPRELOAD=1)

    add_library(cas_preload SHARED lib/libcas_preload.cpp)
    target_include_directories(cas_preload PRIVATE src/cas)
    if(NOT MSVC)
        target_compile_options(cas_preload PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_features(cas_preload PRIVATE cxx_std_17)
    target_link_libraries(cas_preload PRIVATE dl Threads::Threads)
    set_target_properties(cas_preload PROPERTIES OUTPUT_NAME "cas_preload")
endif()

if(AGENTVFS_BPFTIME)
    find_package(bpftime QUIET)
    if(NOT bpftime_FOUND)
        pkg_check_modules(BPFTIME QUIET bpftime)
    endif()
    if(bpftime_FOUND OR BPFTIME_FOUND)
        message(WARNING
            "bpftime found, but compatible runtime API integration is not "
            "implemented; bpftime dependency will not be linked and backend "
            "will be stubbed")
    else()
        message(WARNING "bpftime not found; bpftime backend will be stubbed")
    endif()

    target_sources(agentvfs PRIVATE src/cas/backends/bpftime_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_BPFTIME=1)
endif()

if(AGENTVFS_WASM)
    message(WARNING
        "Wasm backend requested, but WAMR execution is not integrated; "
        "backend will be built as a degraded stub")
    target_sources(agentvfs PRIVATE src/cas/backends/wasm_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_WASM=1)
endif()

if(AGENTVFS_LUA)
    pkg_check_modules(LUAJIT QUIET luajit)
    pkg_check_modules(LUA54 QUIET lua5.4)
    pkg_check_modules(LUA QUIET lua)
    if(LUAJIT_FOUND OR LUA54_FOUND OR LUA_FOUND)
        message(WARNING
            "Lua/LuaJIT found, but execution is not integrated; Lua "
            "dependency will not be linked and backend will be stubbed")
    else()
        message(WARNING "Lua/LuaJIT not found; Lua backend will be stubbed")
    endif()

    target_sources(agentvfs PRIVATE src/cas/backends/lua_backend.cpp)
    target_compile_definitions(agentvfs PRIVATE AGENTVFS_LUA=1)
endif()

if(AGENTVFS_EBPF)
    target_sources(agentvfs PRIVATE src/cas/platform/linux/ebpf_policy_installer.cpp)
    target_include_directories(agentvfs PRIVATE
        ${LIBBPF_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(agentvfs PRIVATE ${LIBBPF_LIBRARIES} elf z)
    add_dependencies(agentvfs cas_bpf_skel)
endif()
endif()

# ── agentvfs daemon (Windows) ───────────────────────────────────────
if(WIN32 AND AGENTVFS_WINFSP)
add_executable(agentvfs
    src/cas/platform/windows/main_windows.cpp
    src/cas/platform/windows/winfsp_adapter.cpp
    src/cas/platform/windows/winfsp_preflight.cpp
    src/cas/platform/windows/named_pipe_channel.cpp
    src/cas/platform/windows/path_translation.cpp
    src/cas/platform/windows/ntstatus_map.cpp)
target_include_directories(agentvfs PRIVATE
    src/cas include ${WINFSP_INCLUDE_DIR})
target_link_libraries(agentvfs PRIVATE cas_core ${WINFSP_LIBRARY} advapi32)
target_compile_features(agentvfs PRIVATE cxx_std_17)
target_compile_definitions(agentvfs PRIVATE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ── agentvfs daemon (macOS) ─────────────────────────────────────────
if(APPLE AND AGENTVFS_FUSE_T)
    find_package(PkgConfig REQUIRED)
    # Homebrew installs fuse-t.pc under non-default prefixes; prepend
    # the common ones so the probe finds it without extra setup.
    if(NOT DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH}
            "/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig")
    else()
        set(ENV{PKG_CONFIG_PATH}
            "/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
    pkg_check_modules(FUSE_T fuse-t)

    # The fuse-t.pc shipped via the brew cask sometimes reports an
    # include path that does not exist on the host (upstream
    # macos-fuse-t#77). Validate that fuse.h is actually reachable;
    # fall back to a direct search of the standard Homebrew prefixes
    # before failing.
    set(_fuse_t_headers_ok FALSE)
    if(FUSE_T_FOUND)
        foreach(_dir IN LISTS FUSE_T_INCLUDE_DIRS)
            if(EXISTS "${_dir}/fuse.h")
                set(_fuse_t_headers_ok TRUE)
                break()
            endif()
        endforeach()
    endif()
    if(NOT _fuse_t_headers_ok)
        find_path(FUSE_T_FALLBACK_INCLUDE_DIR fuse.h
            HINTS /opt/homebrew/include /usr/local/include
            PATH_SUFFIXES fuse)
        if(NOT FUSE_T_FALLBACK_INCLUDE_DIR)
            message(FATAL_ERROR
                "fuse-t headers (fuse.h) not found. Install fuse-t from "
                "https://www.fuse-t.org or via "
                "`brew install --cask macos-fuse-t/cask/fuse-t`.")
        endif()
        set(FUSE_T_INCLUDE_DIRS "${FUSE_T_FALLBACK_INCLUDE_DIR}")
        if(NOT FUSE_T_LIBRARIES)
            find_library(FUSE_T_FALLBACK_LIB fuse-t
                HINTS /opt/homebrew/lib /usr/local/lib)
            if(NOT FUSE_T_FALLBACK_LIB)
                message(FATAL_ERROR
                    "fuse-t library (libfuse-t) not found.")
            endif()
            set(FUSE_T_LIBRARIES "${FUSE_T_FALLBACK_LIB}")
        endif()
    endif()

    add_executable(agentvfs
        src/cas/platform/macos/main_macos.cpp
        src/cas/platform/macos/fuse_t_adapter.cpp
        src/cas/platform/macos/fuse_t_preflight.cpp
        src/cas/workspace_cli.cpp)
    target_include_directories(agentvfs PRIVATE
        src/cas include ${FUSE_T_INCLUDE_DIRS})
    target_compile_options(agentvfs PRIVATE ${FUSE_T_CFLAGS_OTHER})
    # fuse-t's fuse_common.h hard-errors without _FILE_OFFSET_BITS=64.
    # Match what fuse3 implicitly sets on Linux via its .pc Cflags.
    target_compile_definitions(agentvfs PRIVATE _FILE_OFFSET_BITS=64)
    if(NOT MSVC)
        target_compile_options(agentvfs PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(agentvfs PRIVATE
        cas_core ${FUSE_T_LIBRARIES} Threads::Threads)
    target_compile_features(agentvfs PRIVATE cxx_std_17)
    # libfuse-t.dylib's install_name is `@rpath/libfuse-t.dylib`. The
    # binary needs an LC_RPATH or dyld can't resolve it. Set the rpath
    # to both Homebrew prefixes so the daemon runs from the build tree
    # on Intel and Apple Silicon hosts. BUILD_WITH_INSTALL_RPATH makes
    # CMake bake the install rpath in at link time (otherwise the
    # uninstalled binary uses a build-tree rpath that doesn't point
    # anywhere).
    set_target_properties(agentvfs PROPERTIES
        INSTALL_RPATH "/opt/homebrew/lib;/usr/local/lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# ── agentvfs-ctl client ─────────────────────────────────────────────
add_executable(agentvfs-ctl src/ctl/main.cpp)
if(NOT MSVC)
    target_compile_options(agentvfs-ctl PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_features(agentvfs-ctl PRIVATE cxx_std_17)

# ── install rules ───────────────────────────────────────────────────
include(GNUInstallDirs)
if(LINUX)
    install(TARGETS agentvfs
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS agentvfs-ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(AGENTVFS_LDPRELOAD)
    install(TARGETS cas_preload
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(WIN32 AND AGENTVFS_WINFSP)
    install(TARGETS agentvfs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(APPLE AND AGENTVFS_FUSE_T)
    install(TARGETS agentvfs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ── agentvfs-run cooperative runtime launcher (Linux) ───────────────
if(LINUX)
add_executable(agentvfs-run
    src/runtime/agentvfs_run.cpp)
target_include_directories(agentvfs-run PRIVATE src/cas src/runtime)
target_compile_features(agentvfs-run PRIVATE cxx_std_17)
if(NOT MSVC)
    target_compile_options(agentvfs-run PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS agentvfs-run
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ── uninstall target ────────────────────────────────────────────────
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Removing files listed in install_manifest.txt")

add_subdirectory(tests)
