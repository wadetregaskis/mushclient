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
mc_test(test_lua_scripting)
