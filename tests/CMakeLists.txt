cmake_minimum_required(VERSION 3.14)

# Shared compile options for all tests
set(MC_TEST_COMPILE_OPTIONS
    -Wall -Wno-sign-compare -Wno-unused-variable -Wno-deprecated-declarations
    -Wno-non-pod-varargs -Wno-narrowing
)

# Helper macro: create a self-contained test executable
macro(mc_test name)
    add_executable(${name} ${name}.cpp)
    if(MUSHCLIENT_COVERAGE)
        target_compile_options(${name} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
    endif()
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/scripting
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_definitions(${name} PRIVATE
        HAVE_CONFIG_H
        WIN32       # must match mushcore's compile flags to keep CMUSHclientDoc layout identical
        _NOSOUND    # DirectSound not available on macOS/Linux
    )
    # Propagate Lua scripting flag so tests can use #ifdef MUSHCLIENT_LUA_SCRIPTING
    if(LUA_FOUND)
        target_compile_definitions(${name} PRIVATE MUSHCLIENT_LUA_SCRIPTING)
    endif()
    target_compile_options(${name} PRIVATE ${MC_TEST_COMPILE_OPTIONS})
    target_link_libraries(${name} PRIVATE mushcore)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ── Test executables ──────────────────────────────────────────────────────

mc_test(test_cstring)
mc_test(test_regexp)
mc_test(test_mcdatetime)
mc_test(test_colour)
mc_test(test_hashing)
mc_test(test_ansi)
mc_test(test_display)
mc_test(test_telnet)
mc_test(test_mxp)
mc_test(test_triggers)
mc_test(test_world_load)
mc_test(test_aliases)
mc_test(test_timers)
mc_test(test_speedwalk)
mc_test(test_utilities)
mc_test(test_pal_worldview)
mc_test(test_pal_transport)
mc_test(test_pal_surface)
mc_test(test_posix_socket)
mc_test(test_lua_scripting)
mc_test(test_lua_surface)
mc_test(test_plugins)
mc_test(test_database)
mc_test(test_info)
mc_test(test_xmlparse)
mc_test(test_mfc_ui)
mc_test(test_connection)

# ── Phase 2 Unit B-2: headless GDI shim (docs/PHASE2-RECOVERY.md §5/§6) ──────
# The ONE test target compiled WITH -DMUSHCLIENT_FULL_BUILD, so CDC/CFont/CBitmap/
# CGdiObject take the mfc_ui.h *full-build* shape (declarations-only + out-of-line
# virtual dtors → vtables) — the exact shape the real doc.cpp/doc_construct.cpp/
# Utilities.cpp reference.  It links ONLY platform/gdi_headless.cpp (NOT mushcore,
# whose non-full GDI shape would ODR-clash), proving the shim resolves + behaves in
# isolation before B-3 folds gdi_headless.cpp into the full-build mushcore.  It does
# NOT use the mc_test() macro (that links mushcore + the non-full defines).
add_executable(test_gdi_headless
    test_gdi_headless.cpp
    ${CMAKE_SOURCE_DIR}/platform/gdi_headless.cpp)
if(MUSHCLIENT_COVERAGE)
    target_compile_options(test_gdi_headless PRIVATE -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(test_gdi_headless PRIVATE -fprofile-instr-generate)
endif()
target_include_directories(test_gdi_headless PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/scripting
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/pcre
    ${CMAKE_SOURCE_DIR}/sqlite3
    ${CMAKE_SOURCE_DIR}/lsqlite
    ${CMAKE_SOURCE_DIR}/dialogs
    # OpenSSL / PNG / Lua real header dirs (mfc_ui.h pulls <openssl/ssl.h>); mirrors
    # what mc_test() gets transitively via mushcore.  The openssl_fallback dir is
    # inherited from the parent scope's include_directories() when OpenSSL is absent.
    $<TARGET_PROPERTY:mushcore,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(test_gdi_headless PRIVATE
    HAVE_CONFIG_H WIN32 _NOSOUND MUSHCLIENT_FULL_BUILD)
if(LUA_FOUND)
    target_compile_definitions(test_gdi_headless PRIVATE MUSHCLIENT_LUA_SCRIPTING)
endif()
target_compile_options(test_gdi_headless PRIVATE ${MC_TEST_COMPILE_OPTIONS})
add_test(NAME test_gdi_headless COMMAND test_gdi_headless)

# ── In-app conformance selftests ────────────────────────────────────────────
# The hidden --pal-*-selftest flags run inside the real app binary (real wx
# adapter, real transport, real world.Window* engine) and exit before the GUI
# boots — no prefs, worlds, or windows are touched.  The wx-backed ones need
# a window server, so they are registered only when the GUI target exists.
if(TARGET mushgui)
    add_test(NAME selftest_pal_surface
             COMMAND $<TARGET_FILE:mushgui> --pal-surface-selftest)
    add_test(NAME selftest_pal_transport
             COMMAND $<TARGET_FILE:mushgui> --pal-transport-selftest)
    add_test(NAME selftest_pal_miniwindow
             COMMAND $<TARGET_FILE:mushgui> --pal-miniwindow-selftest)
    # RUN_SERIAL: each boots a real wx app instance; two at once race on the
    # window server (an occasional single-selftest flake under `ctest -j`).
    set_tests_properties(selftest_pal_surface selftest_pal_transport
                         selftest_pal_miniwindow
                         PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
