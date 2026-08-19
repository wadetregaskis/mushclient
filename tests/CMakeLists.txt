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
        # The tests see the REAL doc.h layout, matching the mushcore /
        # mushcore_testsupport they link.  CRITICAL — the layout-affecting define
        # trio (WIN32 / _NOSOUND / MUSHCLIENT_FULL_BUILD) MUST match the engine libs
        # or CMUSHclientDoc's layout differs across the linked objects → the
        # historical destructor SIGSEGV.  (MUSHCLIENT_HEADLESS_APP is NOT set here:
        # it only gates definitions inside the testsupport TUs, never header layout.)
        MUSHCLIENT_FULL_BUILD
    )
    # Propagate Lua scripting flag so tests can use #ifdef MUSHCLIENT_LUA_SCRIPTING
    if(LUA_FOUND)
        target_compile_definitions(${name} PRIVATE MUSHCLIENT_LUA_SCRIPTING)
    endif()
    target_compile_options(${name} PRIVATE ${MC_TEST_COMPILE_OPTIONS})
    # Stage 9: the engine is the shared `mushcore`; the headless fakes / portable
    # twins live in `mushcore_testsupport` (which PUBLIC-links mushcore).  Both are
    # listed so ld64 resolves the mushcore↔testsupport symbol cycle by rescan.
    # (mushgui_adapters — the wx/GUI + engine set — is deliberately NOT linked, so
    # no view/dialog/embedded-engine weight enters the test executables.)
    target_link_libraries(${name} PRIVATE mushcore_testsupport mushcore)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ── Test executables ──────────────────────────────────────────────────────

mc_test(test_cstring)
mc_test(test_collections)
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
mc_test(test_tabcomplete)
mc_test(test_commandbar)
mc_test(test_hyperlink)
mc_test(test_utilities)
mc_test(test_notepad_convert)
mc_test(test_notepad_methods)
mc_test(test_chat_sockets)
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
# dialogs/InsertUnicodeDlg.cpp (E26-F3) has no wx/MFC-adapter dependency at all
# -- just CDialog/CDataExchange/CString, same as every other class this suite
# already exercises -- so it is compiled into this one test binary too, to
# drive the real DoDataExchange rather than a stand-in.  It stays in
# MUSHGUI_ADAPTERS_SOURCES for the shipping app; this is a second, test-only
# compile of the same TU into a binary that never links mushgui_adapters, so
# there is no ODR clash.
target_sources(test_mfc_ui PRIVATE ${CMAKE_SOURCE_DIR}/dialogs/InsertUnicodeDlg.cpp)
mc_test(test_connection)
mc_test(test_accelerators)
mc_test(test_menu_rules)
mc_test(test_mc_dispatch)
mc_test(test_scriptengine)
# The chat handshake.  chatsock.cpp now lives in mushcore (chat family
# adoption, docs/STUB-ADOPTION-2026-08.md §5 step 2) with a real OnReceive, so
# this target covers only the pure decisions in pal/chat_protocol.h -- the
# ones worth pinning without a socket.  test_chat_sockets (above) is the one
# covering the live accept chain over loopback.
mc_test(test_chat_protocol)

# The sound engine's two pure decisions (platform/ma_sound_rules.h): the
# Windows-parity auto-select scan and the DSBVOLUME decibel scale.
# platform/ma_sound.cpp compiles miniaudio and needs a device, so it is
# mushgui-only and the rest of it is covered by --sound-selftest.
mc_test(test_sound)

# ── Phase 2 Unit B-2 → B-3: headless GDI shim, now a normal mc_test ──────────
# gdi_headless.cpp folded into the flipped (FULL_BUILD) mushcore at B-3, so the
# CDC/CFont/CBitmap shape no longer forks: this is now an ordinary mc_test that
# links mushcore and exercises the same font-metric/image call shapes.  (Pre-flip
# it was a standalone full-build target so it wouldn't ODR-clash with the
# non-full mushcore; that reason is gone.)
mc_test(test_gdi_headless)

# Self-tests for mc_test.h itself (X04-F15, X04-F16): a regression in the
# framework's own comparison/message-building logic is invisible to every
# OTHER suite, which would just silently mis-report pass/fail.
mc_test(test_mc_test_framework)

# ── RcResources: the .rc parser's own standalone harness (P04-F2) ───────────
# platform/wx_dialog_resource.cpp is the parser behind every one of the app's
# 116 dialog templates and 1644 controls, but it has no wx/MFC dependency at
# all -- it is a plain-C++ .rc-subset reader -- so it does not need mushcore,
# mushcore_testsupport, or wxWidgets to test.  Its own -DWX_DIALOG_RESOURCE_TEST
# main() was previously only a "build this by hand" comment; this registers
# that same harness (now asserting, not just printing) as an ordinary ctest so
# a future decompiler regen or hand-edit of the .rc that drops/mislabels a
# dialog fails ctest instead of shipping silently.
add_executable(test_wx_dialog_resource ../platform/wx_dialog_resource.cpp)
target_compile_definitions(test_wx_dialog_resource PRIVATE WX_DIALOG_RESOURCE_TEST)
target_compile_options(test_wx_dialog_resource PRIVATE ${MC_TEST_COMPILE_OPTIONS})
add_test(NAME test_wx_dialog_resource
         COMMAND test_wx_dialog_resource
                 ${CMAKE_SOURCE_DIR}/resource.h
                 ${CMAKE_SOURCE_DIR}/platform/mushclient_dialogs.rc)

# ── Source lint: no non-ASCII in narrow string literals ─────────────────────
# Note()/Tell()/DisplayMsg() take BYTES, decoded with the world's encoding — so
# a UTF-8 em dash in an app-generated message renders as "â€”" in any world that
# is not set to UTF-8 (the default).  Invisible in review; only visible in a
# running app.  wxT()/wide literals are exempt, so UI text keeps real typography.
# The second test is the checker's own fixture: it must still flag a planted
# violation and still ignore every exempt shape, so the lint cannot rot into a
# vacuous pass.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME lint_ascii_literals
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_ascii_literals.py
                     ${CMAKE_SOURCE_DIR})
    add_test(NAME lint_ascii_literals_selftest
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_ascii_literals.py
                     --selftest)
else()
    # Without a Python3 interpreter the two lint tests above are never
    # registered, so a green ctest run no longer means the mojibake guard
    # ran.  Surface that in the configure log rather than letting the gap
    # be silent (the test COUNT dropping is easy to miss).
    message(WARNING "Python3 not found; ASCII-literal lint (mojibake guard) SKIPPED")
endif()

# ── Source lint: no unresolved merge-conflict markers ───────────────────────
# docs/UPSTREAM-BUGS.md shipped at HEAD for over a week with three literal
# `<<<<<<<`/`=======`/`>>>>>>>` markers left behind by a botched merge
# resolution — invisible in review because they read as a slightly odd prose
# divider, not an error.  This greps every `git ls-files`-tracked file for
# git's own marker syntax.  As with the ASCII-literal lint above, the second
# test is the checker's own fixture, so the lint cannot rot into a vacuous
# pass.
if(Python3_Interpreter_FOUND)
    add_test(NAME lint_conflict_markers
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_conflict_markers.py
                     ${CMAKE_SOURCE_DIR})
    add_test(NAME lint_conflict_markers_selftest
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_conflict_markers.py
                     --selftest)
else()
    message(WARNING "Python3 not found; conflict-marker lint SKIPPED")
endif()

# ── Source lint: no duplicated test-helper definitions under tests/ ─────────
# The HookGuard<T> RAII template (with its four-line preceding comment) was
# pasted byte-identical into three test files, by the same commit that, the
# same day, extracted RecordingTransport into tests/fake_transport.h to stop
# exactly this.  Every test .cpp already includes tests/mc_test.h, so a
# shared helper belongs there once, not copied.  As with the two lints above,
# the second test is the checker's own fixture, so this cannot rot into a
# vacuous pass.
if(Python3_Interpreter_FOUND)
    add_test(NAME lint_duplicate_test_helpers
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_duplicate_test_helpers.py
                     ${CMAKE_SOURCE_DIR})
    add_test(NAME lint_duplicate_test_helpers_selftest
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_duplicate_test_helpers.py
                     --selftest)
else()
    message(WARNING "Python3 not found; duplicate-test-helper lint SKIPPED")
endif()

# ── In-app conformance selftests ────────────────────────────────────────────
# The hidden --pal-*-selftest flags run inside the real app binary (real wx
# adapter, real transport, real world.Window* engine) and exit before the GUI
# boots — no prefs, worlds, or windows are touched.  The wx-backed ones need
# a window server, so they are registered only when the GUI target exists.
if(TARGET mushgui)
    # -ApplePersistenceIgnoreState YES is NOT decoration, and it belongs on
    # EVERY selftest rather than only the ones that build a window.  All of them
    # exit from OnInit, which AppKit counts as "crashed while launching"; after a
    # few of those it greets the NEXT launch with the modal "reopen windows?"
    # alert (NSPersistentUIRestorer promptToIgnorePersistentStateWithCrashHistory),
    # which nothing can answer in a headless ctest run — that launch then hangs
    # until its test times out.  The victim is therefore whichever selftest runs
    # after the history fills up, NOT the one that added the launches: adding the
    # outputpane selftest made selftest_pal_composite (untouched, and green both
    # before and after in a standalone run) time out at 120s.  Restricting the
    # flag to the window-building tests just moves the blast radius around.
    #
    # NSUserDefaults reads argv into its argument domain, so this turns
    # restoration off FOR THIS LAUNCH ONLY — no machine state is written and no
    # `defaults write` is needed.  It is a no-op argument off macOS (the flags
    # are matched by name in OnInit, and everything else is ignored), but keep it
    # APPLE-gated so a Linux run's argv stays clean.
    if(APPLE)
        set(_selftest_args -ApplePersistenceIgnoreState YES)
    else()
        set(_selftest_args)
    endif()

    add_test(NAME selftest_pal_surface
             COMMAND $<TARGET_FILE:mushgui> --pal-surface-selftest ${_selftest_args})
    add_test(NAME selftest_pal_transport
             COMMAND $<TARGET_FILE:mushgui> --pal-transport-selftest ${_selftest_args})
    add_test(NAME selftest_pal_miniwindow
             COMMAND $<TARGET_FILE:mushgui> --pal-miniwindow-selftest ${_selftest_args})
    # The notepad windows: CTextDocument, its wxFrame and the quit hook are all
    # GUI-only, so this is the only place the modified-flag / SaveModified /
    # Convert-menu behaviour can be executed at all.
    # Extended (loop-running) mode, unconditionally: this harness creates,
    # MOVES and hides real windows, and doing that with no event loop leaves
    # CoreAnimation's async commit workers racing the main thread -- an
    # intermittent write-after-free the allocator aborted on (~1 run in 3,
    # 2026-08-18; 0/40 with the loop).  The --extended-selftest flag exists in
    # every build; the MUSHCLIENT_EXTENDED_TESTS option gates only the extra
    # harnesses below.  --notepad-selftest remains for manual loop-free runs.
    add_test(NAME selftest_notepad
             COMMAND $<TARGET_FILE:mushgui> --extended-selftest=notepad ${_selftest_args})
    # Window geometry persistence, the dialog-template -> wx control mapping and
    # the world-creation defaults: wx adapter code mushcore does not link.
    add_test(NAME selftest_gui_bridge
             COMMAND $<TARGET_FILE:mushgui> --gui-bridge-selftest ${_selftest_args})
    # The command-input bar: a real CWorldChildFrame driven with real key
    # events, which is the only place the port's Ctrl+Backspace / arrow-gate /
    # spell-check-on-send wiring can be exercised (it all lives in
    # mushgui_adapters, which the headless test executables never link).
    add_test(NAME selftest_input
             COMMAND $<TARGET_FILE:mushgui> --input-selftest ${_selftest_args})
    # The wx miniwindow compositor + tooltip gate: their bodies are in the
    # executable (platform/wx_world_child.cpp, wxDC-typed), so mushcore's
    # headless suites cannot link them.
    add_test(NAME selftest_pal_composite
             COMMAND $<TARGET_FILE:mushgui> --pal-composite-selftest ${_selftest_args})
    # The world window adapter (CWorldChildFrame = the wx pal::IWorldView):
    # bookmarks, window geometry, the title, and the active-world state, all of
    # which are GUI-side and so unreachable from the headless executables.
    add_test(NAME selftest_world_frame
             COMMAND $<TARGET_FILE:mushgui> --world-frame-selftest ${_selftest_args})

    # The real menu bar: every accelerator parses, none is claimed twice, and
    # the four macOS-shaped clears are where they say they are.  Both failure
    # modes it covers are silent -- see platform/menubar_selftest.cpp.
    # ── extended (loop-running) selftests ───────────────────────────────────
    # OPT-IN: -DMUSHCLIENT_EXTENDED_TESTS=ON.  These are the only tests that
    # reach wxApp::OnRun, so they are the only ones where a paint is delivered
    # or a modal can complete -- and the only ones whose failure mode is a HANG
    # rather than a failure.  Three things make that safe enough to have:
    #
    #   * a watchdog inside the app turns a wedged harness into exit code 2
    #     with a message naming it (platform/extended_selftest.cpp);
    #   * RUN_SERIAL -- the same serialisation the other fifteen in-app
    #     selftests already use.  RESOURCE_LOCK was not enough: it only excludes
    #     tests holding the SAME lock, so an unlocked GUI test could still start
    #     alongside one of these while it had a modal dialog up;
    #   * the app points its preferences at a scratch file first, so a mode
    #     that runs far enough to WRITE preferences cannot touch the ones
    #     belonging to the user's installed copy.
    #
    # TIMEOUT is above the watchdog's default so the watchdog reports first --
    # its message says which harness hung, where a ctest timeout does not.
    if (MUSHCLIENT_EXTENDED_TESTS)
        add_test(NAME extended_paint
                 COMMAND $<TARGET_FILE:mushgui> --extended-selftest=paint ${_selftest_args})
        set_tests_properties(extended_paint PROPERTIES
                             LABELS "extended"
                             RUN_SERIAL TRUE
                             TIMEOUT 120)

        # The MFC->wx dialog and property-sheet bridges: the page-activation
        # contract, the mapped-handler-wins command routing, and the update-UI
        # pump that stands in for WM_KICKIDLE.  All of it sits behind a
        # ShowModal, which is exactly what only this mode can complete
        # (docs/UNREACHABLE-PATHS-2026-08.md findings 16, 17, 19).
        add_test(NAME extended_dialog_bridge
                 COMMAND $<TARGET_FILE:mushgui> --extended-selftest=dialog-bridge ${_selftest_args})
        set_tests_properties(extended_dialog_bridge PROPERTIES
                             LABELS "extended"
                             RUN_SERIAL TRUE
                             TIMEOUT 120)

        # The watchdog is the safety net the whole mode rests on, so it is
        # itself a test: a harness that deliberately never finishes must be
        # killed by it, not by ctest.  WILL_FAIL inverts the verdict -- the
        # PASS here is the app exiting non-zero of its own accord.
        add_test(NAME extended_clear_semantics
                 COMMAND $<TARGET_FILE:mushgui> --extended-selftest=clear-semantics ${_selftest_args})
        set_tests_properties(extended_clear_semantics PROPERTIES
                             LABELS "extended"
                             RUN_SERIAL TRUE
                             TIMEOUT 120)

        # A measurement rather than a behaviour test: it prints the paint cost
        # that refuted 11(b) (docs/AUDIT-2026-08.md).  Registered so the number
        # can be re-taken in one command instead of re-derived, and so the
        # harness cannot rot.
        add_test(NAME extended_paint_cost
                 COMMAND $<TARGET_FILE:mushgui> --extended-selftest=paint-cost ${_selftest_args})
        set_tests_properties(extended_paint_cost PROPERTIES
                             LABELS "extended"
                             RUN_SERIAL TRUE
                             TIMEOUT 120)

        add_test(NAME extended_watchdog
                 COMMAND $<TARGET_FILE:mushgui> --extended-selftest=hang ${_selftest_args})
        set_tests_properties(extended_watchdog PROPERTIES
                             LABELS "extended"
                             RUN_SERIAL TRUE
                             ENVIRONMENT "MUSHCLIENT_EXTENDED_WATCHDOG_SECS=5"
                             WILL_FAIL TRUE
                             TIMEOUT 60)
    endif()

    add_test(NAME selftest_menubar
             COMMAND $<TARGET_FILE:mushgui> --menubar-selftest ${_selftest_args})
    # COutputPane is a wxScrolledWindow in mushgui_adapters, so the headless
    # tests cannot construct it: the selection/freeze/scrollback behaviour is
    # only reachable from inside the app.
    add_test(NAME selftest_outputpane
             COMMAND $<TARGET_FILE:mushgui> --outputpane-selftest ${_selftest_args})
    # The miniaudio sound engine: platform/ma_sound.cpp compiles miniaudio, so
    # it links here and nowhere else, and per-owner buffer isolation + the
    # exit-time teardown both need a real device.  SKIP_RETURN_CODE 77 covers a
    # machine with no audio device (the --engines-selftest convention).
    add_test(NAME selftest_sound
             COMMAND $<TARGET_FILE:mushgui> --sound-selftest ${_selftest_args})
    set_tests_properties(selftest_sound PROPERTIES
                         SKIP_RETURN_CODE 77
                         TIMEOUT 120 RUN_SERIAL TRUE)
    # The six scripting languages: the only place five of them can be exercised,
    # since their interpreters are linked into the app and deliberately kept out
    # of mushcore.  One ctest entry per language, so a build without (say) Ruby
    # reports selftest_engine_ruby as SKIPPED rather than passing vacuously --
    # scripting support is expected but not required.  Exit 77 is the skip.
    foreach(_lang lua python perl js tcl ruby)
        add_test(NAME selftest_engine_${_lang}
                 COMMAND $<TARGET_FILE:mushgui> --engines-selftest=${_lang}
                         ${_selftest_args})
        set_tests_properties(selftest_engine_${_lang} PROPERTIES
                             SKIP_RETURN_CODE 77
                             RUN_SERIAL TRUE)
    endforeach()
    # RUN_SERIAL: each boots a real wx app instance; two at once race on the
    # window server (an occasional single-selftest flake under `ctest -j`).
    set_tests_properties(selftest_pal_surface selftest_pal_transport
                         selftest_pal_miniwindow selftest_notepad
                         selftest_gui_bridge selftest_input
                         selftest_pal_composite selftest_world_frame
                         selftest_outputpane selftest_menubar
                         PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
