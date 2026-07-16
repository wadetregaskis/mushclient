cmake_minimum_required(VERSION 3.14)

# Shared compile options for all tests
set(MC_TEST_COMPILE_OPTIONS
    -Wall -Wno-sign-compare -Wno-unused-variable -Wno-deprecated-declarations
    -Wno-non-pod-varargs -Wno-narrowing
)

# Helper macro: create a self-contained test executable
macro(mc_test name)
    add_executable(${name} ${name}.cpp)
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
mc_test(test_lua_scripting)

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
