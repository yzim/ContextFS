if(WIN32)
    add_executable(cas_test_named_pipe_channel
        unit/test_named_pipe_channel.cpp
        ../src/cas/platform/windows/named_pipe_channel.cpp)
    target_include_directories(cas_test_named_pipe_channel PRIVATE
        ../src ../src/cas)
    target_compile_features(cas_test_named_pipe_channel PRIVATE cxx_std_17)

    add_executable(cas_test_win_helpers
        unit/test_win_helpers.cpp
        ../src/cas/platform/windows/path_translation.cpp
        ../src/cas/platform/windows/ntstatus_map.cpp)
    target_include_directories(cas_test_win_helpers PRIVATE
        ../src ../src/cas)
    target_compile_features(cas_test_win_helpers PRIVATE cxx_std_17)
endif()

if(UNIX)
add_executable(cas_test_workspace_cli
    unit/test_workspace_cli.cpp
    ../src/cas/workspace_cli.cpp)
target_include_directories(cas_test_workspace_cli PRIVATE ../src/cas)
if(NOT MSVC)
    target_compile_options(cas_test_workspace_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(cas_test_workspace_cli PRIVATE Threads::Threads)
if(AGENTVFS_EBPF)
    target_compile_definitions(cas_test_workspace_cli PRIVATE AGENTVFS_EBPF=1)
endif()
if(AGENTVFS_FANOTIFY)
    target_compile_definitions(cas_test_workspace_cli PRIVATE AGENTVFS_FANOTIFY=1)
endif()
endif() # UNIX

if(APPLE)
add_executable(cas_test_fuse_t_preflight
    unit/test_fuse_t_preflight.cpp
    ../src/cas/platform/macos/fuse_t_preflight.cpp)
target_include_directories(cas_test_fuse_t_preflight PRIVATE
    ../src ../src/cas ../include)
target_compile_features(cas_test_fuse_t_preflight PRIVATE cxx_std_17)
if(NOT MSVC)
    target_compile_options(cas_test_fuse_t_preflight PRIVATE
        -Wall -Wextra -Wpedantic)
endif()
endif()

add_executable(cas_test_working_tree unit/test_working_tree.cpp)
target_link_libraries(cas_test_working_tree PRIVATE cas_core)

add_executable(cas_test_write_buffer unit/test_write_buffer.cpp)
target_link_libraries(cas_test_write_buffer PRIVATE cas_core)

if(UNIX)
add_executable(cas_test_object_store unit/test_object_store.cpp)
target_link_libraries(cas_test_object_store PRIVATE cas_core)
endif()

if(UNIX)
add_executable(cas_test_fh_lifecycle system/test_cas_fh_lifecycle.cpp)
endif()

add_executable(cas_test_branch_context unit/test_branch_context.cpp)
target_link_libraries(cas_test_branch_context PRIVATE cas_core)

add_executable(cas_test_branch_merge unit/test_branch_merge.cpp)
target_link_libraries(cas_test_branch_merge PRIVATE cas_core)

if(UNIX)
add_executable(cas_test_runtime_state unit/test_runtime_state.cpp)
target_link_libraries(cas_test_runtime_state PRIVATE cas_core)
endif()

if(UNIX)
add_executable(cas_test_agent_state unit/test_agent_state.cpp)
target_link_libraries(cas_test_agent_state PRIVATE cas_core)
endif()

if(UNIX)
add_executable(cas_test_agent_state_service unit/test_agent_state_service.cpp)
target_link_libraries(cas_test_agent_state_service PRIVATE cas_core)
endif()

if(UNIX)
add_executable(cas_test_runtime_supervisor unit/test_runtime_supervisor.cpp)
target_link_libraries(cas_test_runtime_supervisor PRIVATE cas_core Threads::Threads)
endif()

if(UNIX)
add_executable(cas_test_runtime_process_posix
    unit/test_runtime_process_posix.cpp)
target_link_libraries(cas_test_runtime_process_posix PRIVATE cas_core Threads::Threads)
endif()

if(UNIX)
add_executable(cas_test_branch_merge_commit unit/test_branch_merge_commit.cpp)
target_link_libraries(cas_test_branch_merge_commit PRIVATE cas_core)
endif()

if(UNIX)
add_executable(cas_test_branch_merge_daemon
    unit/test_branch_merge_daemon.cpp
    support/ebpf_loader_stub.cpp)
target_link_libraries(cas_test_branch_merge_daemon PRIVATE cas_core)
if(AGENTVFS_EBPF)
    target_link_libraries(cas_test_branch_merge_daemon PRIVATE ${LIBBPF_LIBRARIES} elf z)
endif()

add_executable(cas_test_branch_persistence
    unit/test_branch_persistence.cpp
    support/ebpf_loader_stub.cpp)
target_link_libraries(cas_test_branch_persistence PRIVATE cas_core)
if(AGENTVFS_EBPF)
    target_link_libraries(cas_test_branch_persistence PRIVATE ${LIBBPF_LIBRARIES} elf z)
endif()

add_executable(cas_test_runtime_control
    unit/test_runtime_control.cpp
    support/ebpf_loader_stub.cpp)
target_link_libraries(cas_test_runtime_control PRIVATE cas_core Threads::Threads)
if(AGENTVFS_EBPF)
    target_link_libraries(cas_test_runtime_control PRIVATE ${LIBBPF_LIBRARIES} elf z)
endif()

add_executable(cas_test_agent_state_control
    unit/test_agent_state_control.cpp
    support/ebpf_loader_stub.cpp)
target_link_libraries(cas_test_agent_state_control PRIVATE cas_core Threads::Threads)
if(AGENTVFS_EBPF)
    target_link_libraries(cas_test_agent_state_control PRIVATE ${LIBBPF_LIBRARIES} elf z)
endif()
endif() # UNIX

add_executable(cas_test_telemetry_event unit/test_telemetry_event.cpp ../src/cas/telemetry_event.cpp)
target_include_directories(cas_test_telemetry_event PRIVATE ../include ../src ../src/cas)
target_compile_features(cas_test_telemetry_event PRIVATE cxx_std_17)

if(UNIX)
add_executable(cas_test_telemetry_registry unit/test_telemetry_registry.cpp ../src/cas/telemetry_registry.cpp ../src/cas/telemetry_event.cpp)
target_include_directories(cas_test_telemetry_registry PRIVATE ../include ../src ../src/cas)
target_compile_features(cas_test_telemetry_registry PRIVATE cxx_std_17)
endif()

if(AGENTVFS_BLAKE3_SIMD)
add_executable(cas_test_blake3_simd unit/test_blake3_simd.cpp)
target_link_libraries(cas_test_blake3_simd PRIVATE cas_core)
endif()

if(LINUX)
add_executable(cas_test_ebpf_backend
    unit/test_ebpf_backend.cpp
    ../src/cas/backends/ebpf_backend.cpp
    ../src/cas/ebpf_loader.cpp)
target_include_directories(cas_test_ebpf_backend PRIVATE ../include ../src ../src/cas)
target_link_libraries(cas_test_ebpf_backend PRIVATE cas_core)
target_compile_features(cas_test_ebpf_backend PRIVATE cxx_std_17)
if(AGENTVFS_EBPF)
    target_include_directories(cas_test_ebpf_backend PRIVATE
        ${LIBBPF_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(cas_test_ebpf_backend PRIVATE ${LIBBPF_LIBRARIES} elf z)
    add_dependencies(cas_test_ebpf_backend cas_bpf_skel)
endif()

add_executable(cas_test_fanotify_backend
    unit/test_fanotify_backend.cpp
    ../src/cas/backends/fanotify_backend.cpp
    ../src/cas/telemetry_event.cpp)
target_include_directories(cas_test_fanotify_backend PRIVATE ../include ../src ../src/cas)
target_link_libraries(cas_test_fanotify_backend PRIVATE Threads::Threads)
target_compile_features(cas_test_fanotify_backend PRIVATE cxx_std_17)
if(AGENTVFS_FANOTIFY)
    target_compile_definitions(cas_test_fanotify_backend PRIVATE AGENTVFS_FANOTIFY=1)
endif()

add_executable(cas_test_ptrace_backend
    unit/test_ptrace_backend.cpp
    ../src/cas/backends/ptrace_backend.cpp
    ../src/cas/telemetry_event.cpp)
target_include_directories(cas_test_ptrace_backend PRIVATE ../include ../src ../src/cas)
target_link_libraries(cas_test_ptrace_backend PRIVATE Threads::Threads)
target_compile_features(cas_test_ptrace_backend PRIVATE cxx_std_17)
if(AGENTVFS_PTRACE)
    target_compile_definitions(cas_test_ptrace_backend PRIVATE AGENTVFS_PTRACE=1)
endif()

add_executable(cas_test_ldpreload_backend
    unit/test_ldpreload_backend.cpp
    ../src/cas/backends/ldpreload_backend.cpp
    ../src/cas/telemetry_event.cpp)
target_include_directories(cas_test_ldpreload_backend PRIVATE ../include ../src ../src/cas)
target_link_libraries(cas_test_ldpreload_backend PRIVATE Threads::Threads)
target_compile_features(cas_test_ldpreload_backend PRIVATE cxx_std_17)
if(AGENTVFS_LDPRELOAD)
    target_compile_definitions(cas_test_ldpreload_backend PRIVATE AGENTVFS_LDPRELOAD=1)
endif()

add_executable(cas_test_bpftime_backend
    unit/test_bpftime_backend.cpp
    ../src/cas/backends/bpftime_backend.cpp)
target_include_directories(cas_test_bpftime_backend PRIVATE ../include ../src ../src/cas)
target_link_libraries(cas_test_bpftime_backend PRIVATE Threads::Threads)
target_compile_features(cas_test_bpftime_backend PRIVATE cxx_std_17)
if(AGENTVFS_BPFTIME)
    target_compile_definitions(cas_test_bpftime_backend PRIVATE AGENTVFS_BPFTIME=1)
endif()

add_executable(cas_test_wasm_backend
    unit/test_wasm_backend.cpp
    ../src/cas/backends/wasm_backend.cpp)
target_include_directories(cas_test_wasm_backend PRIVATE ../include ../src ../src/cas)
target_compile_features(cas_test_wasm_backend PRIVATE cxx_std_17)
if(AGENTVFS_WASM)
    target_compile_definitions(cas_test_wasm_backend PRIVATE AGENTVFS_WASM=1)
endif()

add_executable(cas_test_lua_backend
    unit/test_lua_backend.cpp
    ../src/cas/backends/lua_backend.cpp)
target_include_directories(cas_test_lua_backend PRIVATE ../include ../src ../src/cas)
target_compile_features(cas_test_lua_backend PRIVATE cxx_std_17)
if(AGENTVFS_LUA)
    target_compile_definitions(cas_test_lua_backend PRIVATE AGENTVFS_LUA=1)
endif()

if(AGENTVFS_LDPRELOAD)
    add_executable(cas_test_ldpreload_preload
        unit/test_ldpreload_preload.cpp)
    target_include_directories(cas_test_ldpreload_preload PRIVATE ../include ../src ../src/cas)
    target_link_libraries(cas_test_ldpreload_preload PRIVATE dl Threads::Threads)
    target_compile_features(cas_test_ldpreload_preload PRIVATE cxx_std_17)
    target_compile_definitions(cas_test_ldpreload_preload PRIVATE
        CAS_PRELOAD_LIB="$<TARGET_FILE:cas_preload>")
    add_dependencies(cas_test_ldpreload_preload cas_preload)
endif()

add_executable(agentvfs_runtime_counter_fixture
    fixtures/runtime_counter_fixture.cpp
    ../src/runtime/runtime_client.cpp)
target_include_directories(agentvfs_runtime_counter_fixture PRIVATE
    ../src/cas ../src/runtime)
target_compile_features(agentvfs_runtime_counter_fixture PRIVATE cxx_std_17)
if(NOT MSVC)
    target_compile_options(agentvfs_runtime_counter_fixture PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif() # LINUX
