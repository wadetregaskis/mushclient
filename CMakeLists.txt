cmake_minimum_required(VERSION 3.14)
project(MUSHclient VERSION 5.07 LANGUAGES C CXX)

# ── Require C++17 ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Platform detection ────────────────────────────────────────────────────
if(WIN32)
    message(STATUS "Configuring for Windows — use the .sln/.vcxproj for the full build")
    # The existing Visual Studio project handles the Windows build.
    # CMake on Windows can still build the unit tests if desired.
endif()

# ── Apple Silicon: prefer arm64 Homebrew ──────────────────────────────────
# On Apple Silicon Macs, Homebrew installs to /opt/homebrew (arm64 native).
# Rosetta / legacy Intel Homebrew lives at /usr/local.  Prepend /opt/homebrew
# so CMake's find_package / find_program / pkg-config all prefer the arm64
# variants, which is what the Mac version requires.
# On Intel Macs (no /opt/homebrew) this block is a no-op.
if(APPLE AND EXISTS /opt/homebrew)
    list(PREPEND CMAKE_PREFIX_PATH  /opt/homebrew)
    list(PREPEND CMAKE_PROGRAM_PATH /opt/homebrew/bin)
    # Also tell pkg-config to search arm64 Homebrew first
    set(ENV{PKG_CONFIG_PATH}
        "/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    message(STATUS "Apple Silicon detected — preferring arm64 Homebrew at /opt/homebrew")
endif()

# ── Global include paths ──────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}           # root (for stdafx.h, lua.h, etc.)
    ${CMAKE_SOURCE_DIR}/platform  # platform compat headers
)

# ── Compile definitions ───────────────────────────────────────────────────
add_compile_definitions(HAVE_CONFIG_H)  # for PCRE config.h

# ── Find system libraries (macOS / Linux) ─────────────────────────────────
if(NOT WIN32)
    find_package(OpenSSL QUIET)
    find_package(ZLIB REQUIRED)

    # libpng: on macOS find_package(PNG) can mis-detect a stale header dir
    # (e.g. /Library/Frameworks/Mono.framework/Headers — libpng 1.4) while
    # linking Homebrew's libpng 1.6, a latent header/ABI mismatch masked only
    # by -isystem include ordering.  Steer detection to Homebrew's libpng so the
    # header and library always match.  Uses `brew --prefix` so it works on both
    # Apple Silicon (/opt/homebrew) and Intel (/usr/local).
    if(APPLE)
        find_program(BREW_EXECUTABLE brew)
        if(BREW_EXECUTABLE)
            execute_process(COMMAND ${BREW_EXECUTABLE} --prefix libpng
                            OUTPUT_VARIABLE _png_prefix
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_png_prefix AND EXISTS "${_png_prefix}/include/png.h")
                set(PNG_ROOT "${_png_prefix}")
                # Override any stale cached include dir from a prior configure.
                set(PNG_PNG_INCLUDE_DIR "${_png_prefix}/include"
                    CACHE PATH "libpng include dir" FORCE)
            endif()
        endif()
    endif()
    find_package(PNG REQUIRED)

    # Lua 5.1 — try several common names
    find_package(Lua 5.1 QUIET)
    if(NOT LUA_FOUND)
        # Try pkg-config
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LUA51 QUIET lua5.1 lua-5.1 lua51)
            if(LUA51_FOUND)
                set(LUA_INCLUDE_DIR ${LUA51_INCLUDE_DIRS})
                set(LUA_LIBRARIES   ${LUA51_LIBRARIES})
                set(LUA_FOUND TRUE)
            endif()
        endif()
    endif()

    if(NOT LUA_FOUND)
        # Fall back to building Lua 5.1 from source via FetchContent.
        # If the network is unavailable the download is skipped gracefully and
        # Lua scripting is disabled for this build (all other targets still build).
        include(FetchContent)
        set(FETCHCONTENT_UPDATES_DISCONNECTED ON)   # never re-download once cached
        FetchContent_Declare(
            lua51_src
            URL      https://www.lua.org/ftp/lua-5.1.5.tar.gz
            URL_HASH MD5=2e115fe26e435a33deb048c90c3d78e0
        )
        FetchContent_GetProperties(lua51_src)

        # Check if sources were already extracted in a previous configure run.
        set(_lua51_cached_src "${CMAKE_BINARY_DIR}/_deps/lua51_src-src/src/lua.h")
        if(NOT lua51_src_POPULATED AND EXISTS "${_lua51_cached_src}")
            # Populate the properties manually so the build targets below work.
            set(lua51_src_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/lua51_src-src")
            set(lua51_src_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/lua51_src-build")
            set(lua51_src_POPULATED TRUE)
            message(STATUS "Using cached Lua 5.1 source (${lua51_src_SOURCE_DIR})")
        endif()

        if(NOT lua51_src_POPULATED)
            message(STATUS "Lua 5.1 not found — fetching from lua.org...")
            # Download may fail in network-restricted environments; make it non-fatal.
            # cmake has no try/catch, so we pre-check connectivity via file(DOWNLOAD).
            set(_lua_tarball "${CMAKE_BINARY_DIR}/_deps/lua-5.1.5.tar.gz")
            file(DOWNLOAD
                "https://www.lua.org/ftp/lua-5.1.5.tar.gz"
                "${_lua_tarball}"
                TIMEOUT 30
                STATUS _lua_dl_status
                LOG _lua_dl_log
            )
            list(GET _lua_dl_status 0 _lua_dl_code)
            if(_lua_dl_code EQUAL 0)
                FetchContent_Populate(lua51_src)
            else()
                message(WARNING
                    "Lua 5.1 download failed (${_lua_dl_status}). "
                    "Lua scripting will be disabled. "
                    "Install lua5.1 (brew install lua@5.1 / apt install liblua5.1-dev) "
                    "and re-run cmake to enable it.")
            endif()
        endif()

        if(lua51_src_POPULATED)
            message(STATUS "Building Lua 5.1 from source (vendored)")
        endif()

        # Build only the Lua VM core (no interpreter/compiler executables).
        # These are the files needed for the liblua5.1 runtime.
        set(_LUA_SRC_DIR "${lua51_src_SOURCE_DIR}/src")
        set(_LUA_CORE_SRCS
            ${_LUA_SRC_DIR}/lapi.c    ${_LUA_SRC_DIR}/lauxlib.c
            ${_LUA_SRC_DIR}/lbaselib.c ${_LUA_SRC_DIR}/lcode.c
            ${_LUA_SRC_DIR}/ldblib.c  ${_LUA_SRC_DIR}/ldebug.c
            ${_LUA_SRC_DIR}/ldo.c     ${_LUA_SRC_DIR}/ldump.c
            ${_LUA_SRC_DIR}/lfunc.c   ${_LUA_SRC_DIR}/lgc.c
            ${_LUA_SRC_DIR}/linit.c   ${_LUA_SRC_DIR}/liolib.c
            ${_LUA_SRC_DIR}/llex.c    ${_LUA_SRC_DIR}/lmathlib.c
            ${_LUA_SRC_DIR}/lmem.c    ${_LUA_SRC_DIR}/loadlib.c
            ${_LUA_SRC_DIR}/lobject.c ${_LUA_SRC_DIR}/lopcodes.c
            ${_LUA_SRC_DIR}/loslib.c  ${_LUA_SRC_DIR}/lparser.c
            ${_LUA_SRC_DIR}/lstate.c  ${_LUA_SRC_DIR}/lstring.c
            ${_LUA_SRC_DIR}/lstrlib.c ${_LUA_SRC_DIR}/ltable.c
            ${_LUA_SRC_DIR}/ltablib.c ${_LUA_SRC_DIR}/ltm.c
            ${_LUA_SRC_DIR}/lundump.c ${_LUA_SRC_DIR}/lvm.c
            ${_LUA_SRC_DIR}/lzio.c
        )
        if(lua51_src_POPULATED AND EXISTS "${_LUA_SRC_DIR}/lua.h")
            add_library(lua51_vendored STATIC ${_LUA_CORE_SRCS})
            target_include_directories(lua51_vendored PUBLIC "${_LUA_SRC_DIR}")
            # Suppress warnings from Lua C sources
            target_compile_options(lua51_vendored PRIVATE
                -w -DLUA_USE_POSIX -DLUA_USE_DLOPEN)
            set_target_properties(lua51_vendored PROPERTIES
                POSITION_INDEPENDENT_CODE ON)

            set(LUA_INCLUDE_DIR "${_LUA_SRC_DIR}")
            set(LUA_LIBRARIES   lua51_vendored)
            set(LUA_FOUND TRUE)
        endif()
    endif()
endif()

# ── Vendored library: PCRE ────────────────────────────────────────────────

set(PCRE_SOURCES
    pcre/pcre_compile.c
    pcre/pcre_config.c
    pcre/pcre_dfa_exec.c
    pcre/pcre_exec.c
    pcre/pcre_fullinfo.c
    pcre/pcre_get.c
    pcre/pcre_globals.c
    pcre/pcre_maketables.c
    pcre/pcre_newline.c
    pcre/pcre_ord2utf8.c
    pcre/pcre_refcount.c
    pcre/pcre_study.c
    pcre/pcre_tables.c
    pcre/pcre_ucd.c
    pcre/pcre_valid_utf8.c
    pcre/pcre_version.c
    pcre/pcre_xclass.c
    pcre/pcre_chartables.c
)
add_library(mcpcre STATIC ${PCRE_SOURCES})
target_include_directories(mcpcre PUBLIC ${CMAKE_SOURCE_DIR}/pcre)
target_compile_definitions(mcpcre PUBLIC HAVE_CONFIG_H)
# Suppress warnings in vendored code
if(NOT WIN32)
    target_compile_options(mcpcre PRIVATE -w)
endif()

# ── Vendored library: SQLite3 ─────────────────────────────────────────────

add_library(mcsqlite3 STATIC sqlite3/sqlite3.c)
target_include_directories(mcsqlite3 PUBLIC ${CMAKE_SOURCE_DIR}/sqlite3)
if(NOT WIN32)
    target_link_libraries(mcsqlite3 PUBLIC pthread dl)
    target_compile_options(mcsqlite3 PRIVATE -w)
endif()

# ── Vendored library: zlib (Windows only — use system on macOS/Linux) ─────

if(WIN32)
    file(GLOB ZLIB_SOURCES zlib/*.c)
    add_library(mczlib STATIC ${ZLIB_SOURCES})
    target_include_directories(mczlib PUBLIC ${CMAKE_SOURCE_DIR}/zlib)
else()
    # On macOS/Linux, zlib is already found above as ZLIB::ZLIB
    add_library(mczlib INTERFACE)
    target_link_libraries(mczlib INTERFACE ZLIB::ZLIB)
endif()

# ── Core portable library (non-Windows) ──────────────────────────────────

if(NOT WIN32)

    set(MUSHCORE_SOURCES
        # Date/time
        mcdatetime.cpp

        # Regex wrapper (wraps PCRE)
        regexp.cpp

        # Colour utilities
        Color.cpp

        # Portable string utilities extracted from Utilities.cpp
        utilities_portable.cpp
        Replace.cpp
        Dmetaph.cpp

        # Hashing / crypto (vendored, pure C, no Windows deps)
        scripting/md5.c
        scripting/sha256.c
        scripting/shs.c

        # ANSI colour parser + supporting doc helpers (Phase 2)
        ansi.cpp
        doc_portable.cpp

        # Telnet protocol parser (Phase 2)
        telnet_phases.cpp

        # MXP protocol parser (Phase 2)
        mxp/mxp.cpp
        mxp/mxp_phases.cpp
        mxp/mxpMode.cpp
        mxp/mxpError.cpp
        mxp/mxpOnOff.cpp
        mxp/mxpinit.cpp
        mxp/mxputils.cpp
        mxp/mxpStart.cpp
        mxp/mxpEnd.cpp
        mxp/mxpClose.cpp
        mxp/mxpCloseAtomic.cpp
        mxp/mxpOpenAtomic.cpp
        mxp/mxpDefs.cpp
        mxp/mxpEntities.cpp

        # scriptengine.cpp is always compiled — provides COM→Lua dispatch and,
        # when Lua is absent, stubs for all CScriptEngine Lua methods.
        scripting/scriptengine.cpp

        # Networking (Phase 3)
        worldsock.cpp

        # Core document logic (triggers, aliases, timers, line buffer)
        Line.cpp
        ProcessPreviousLine.cpp
        evaluate.cpp
        timers.cpp
        serialize.cpp

        # Output-window line/style types (CPaneLine, CPaneStyle, utf8_strlen)
        paneline.cpp

        # Non-Windows stubs for Windows-only classes (CMainFrame Frame,
        # CMUSHView::classCMUSHView, CSendView, CChatSocket, CPaneLine, etc.)
        platform/windows_stubs.cpp

        # CPlugin destructor — separate file so it includes only doc_stub.h
        # (windows_stubs.cpp sees the full plugins.h layout via doc.h, which
        # would cause a struct-size mismatch with the stub CPlugin).
        platform/plugin_portable.cpp

        # Hook function pointer storage (definitions for wx_mfc_hooks.h externs).
        # Also compiled into mushapp so mushgui (which links mushapp, not mushcore)
        # can resolve the same symbols without linking both libraries.
        platform/wx_mfc_hooks_storage.cpp

        # World options tables (numeric + alpha option name→default mappings)
        scriptingoptions_portable.cpp

        # XML parser (world-file deserialisation)
        xml/xmlparse.cpp

        # Portable world-file loader (triggers, aliases, variables, timers)
        xml/xml_load_world_portable.cpp

        # Portable world-file writer (mushcli save)
        xml/xml_save_world_portable.cpp
    )

    add_library(mushcore STATIC ${MUSHCORE_SOURCES})

    target_compile_definitions(mushcore PRIVATE
        _NOSOUND    # DirectSound not available on macOS/Linux
        WIN32       # MUSHclient uses WIN32 guards for Windows-specific feature paths
    )

    if(LUA_FOUND)
        target_sources(mushcore PRIVATE
            # Lua C extensions
            scripting/bits.c
            scripting/lrexlib.c
            scripting/lua_compress.c
            scripting/lbc.c
            scripting/mt19937ar.c
            scripting/number.c

            # Lua scripting engine
            scripting/lua_scripting.cpp

            # LPeg pattern-matching library (vendored, pure C)
            scripting/lpeg/lptree.c
            scripting/lpeg/lpvm.c
            scripting/lpeg/lpcap.c
            scripting/lpeg/lpcode.c

            # SQLite3 Lua binding (vendored)
            lsqlite/lsqlite3.c

            # Portable Lua utilities (MakeLuaState, MakeTableItem, etc.)
            # On Windows these come from Utilities.cpp / lua_methods.cpp.
            scripting/lua_utils_portable.cpp

            # Portable "world" Lua library (Note, SetVariable, Send, Execute, …)
            # On Windows the full ~8000-line lua_methods.cpp is used instead.
            scripting/lua_world_api_portable.cpp

            # Portable world.Debug() — trigger/alias/timer/variable/telnet listings.
            # On Windows the full implementation is in world_debug.cpp (mushapp only).
            scripting/world_debug_portable.cpp
        )
        # Signal to scriptengine.cpp that the real Lua impl is compiled in
        target_compile_definitions(mushcore PRIVATE MUSHCLIENT_LUA_SCRIPTING)
    endif()

    # Note: md5.h / sha256.h / shs.h have been patched to use 'unsigned int'
    # instead of 'unsigned long' for their 32-bit word types, ensuring correct
    # behaviour on LP64 platforms (Linux/macOS 64-bit) where 'unsigned long'
    # is 8 bytes.  See scripting/md5.h, sha256.h, shs.h.

    target_include_directories(mushcore PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/pcre
        ${CMAKE_SOURCE_DIR}/scripting
        ${CMAKE_SOURCE_DIR}/scripting/lpeg
        ${CMAKE_SOURCE_DIR}/sqlite3
        ${CMAKE_SOURCE_DIR}/lsqlite
    )

    target_compile_options(mushcore PRIVATE
        -Wall
        -Wno-deprecated-declarations
        -Wno-unused-variable
        -Wno-sign-compare
        -Wno-missing-field-initializers
        # A non-trivially-copyable CString (it holds a std::string) CANNOT be
        # passed through a C variadic ("...") on clang: despite operator const
        # char*, clang emits a runtime TRAP (SIGTRAP) at the call site, which
        # silently crashes the app on otherwise-rare paths (error reporting,
        # world/plugin-state save, …).  Every such call site is cast to
        # (LPCTSTR); keep this a hard error so the latent-crash class can't
        # return.  (On Windows CString is a thin pointer wrapper, so the cast is
        # a harmless no-op there.)
        -Werror=non-pod-varargs
        # Windows source initialises double struct fields with LONG_MAX; MSVC
        # accepts this silently.  Suppress the GCC/Clang narrowing error.
        -Wno-narrowing
    )

    target_link_libraries(mushcore PUBLIC
        mcpcre
        mcsqlite3
        mczlib
        PNG::PNG
    )

    if(LUA_FOUND)
        target_include_directories(mushcore PUBLIC ${LUA_INCLUDE_DIR})
        target_link_libraries(mushcore PUBLIC ${LUA_LIBRARIES})
    endif()

    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        target_include_directories(mushcore PUBLIC ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(mushcore PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        # Signal to worldsock.cpp / doc_stub.h that real OpenSSL headers are available
        target_compile_definitions(mushcore PRIVATE OPENSSL_FOUND)
    endif()

    # ── mushapp — full-application static library ─────────────────────────────
    # This library contains the complete MUSHclient source (non-portable).
    # It replaces the portable_*.cpp stubs with the real implementations and
    # adds all dialogs, scripting methods, views, and app-level code.
    # mushgui links mushapp (not mushcore) to avoid duplicate symbols.
    # mushcore remains the portable layer for mushcli and the test suite.

    set(MUSHAPP_SOURCES
        # ── Vendored C libraries (same as mushcore) ───────────────────────
        scripting/md5.c
        scripting/sha256.c
        scripting/shs.c

        # ── Core portable logic (non-conflicting subset from mushcore) ────
        mcdatetime.cpp
        regexp.cpp
        Color.cpp
        Replace.cpp
        Dmetaph.cpp
        ansi.cpp
        telnet_phases.cpp
        worldsock.cpp
        Line.cpp
        ProcessPreviousLine.cpp
        evaluate.cpp
        timers.cpp
        serialize.cpp
        paneline.cpp
        # scriptingoptions_portable.cpp is intentionally OMITTED here:
        # scriptingoptions.cpp (in the app-level section below) provides the full
        # OptionsTable[] and AlphaOptionsTable[], which are a superset.
        # Including both causes duplicate symbol errors at link time.
        xml/xmlparse.cpp

        # ── Full implementations replacing portable stubs ─────────────────
        # Global singletons (App, Frame, g_debug) — replaces doc_portable.cpp
        platform/mushapp_globals.cpp
        # Non-Windows stubs/forwarders needed by mushapp but not by mushcore
        platform/mushapp_stubs.cpp
        # Hook pointer definitions (also in mushcore for mushcli; no ODR conflict
        # because no target links both mushapp and mushcore simultaneously).
        platform/wx_mfc_hooks_storage.cpp
        # (utilities_portable.cpp → Utilities.cpp)
        Utilities.cpp
        # (doc_portable.cpp → doc.cpp + doc_construct.cpp)
        doc.cpp
        doc_construct.cpp
        # (xml/xml_load_world_portable.cpp → xml/xml_load_world.cpp)
        xml/xml_load_world.cpp
        xml/xml_save_world.cpp
        xml/xml_serialize.cpp
        # windows_stubs.cpp is deliberately OMITTED — real implementations below

        # ── MXP protocol parser ───────────────────────────────────────────
        mxp/mxp.cpp
        mxp/mxp_phases.cpp
        mxp/mxpMode.cpp
        mxp/mxpError.cpp
        mxp/mxpOnOff.cpp
        mxp/mxpinit.cpp
        mxp/mxputils.cpp
        mxp/mxpStart.cpp
        mxp/mxpEnd.cpp
        mxp/mxpClose.cpp
        mxp/mxpCloseAtomic.cpp
        mxp/mxpOpenAtomic.cpp
        mxp/mxpDefs.cpp
        mxp/mxpEntities.cpp

        # ── XML world I/O (full) ──────────────────────────────────────────
        # (already listed above under "Full implementations")

        # ── Scripting engine ──────────────────────────────────────────────
        scripting/scriptengine.cpp
        scripting/scripting.cpp
        scripting/functionlist.cpp
        # Mersenne Twister RNG — used widely (miniwindow, dialogs, etc.), not Lua-only
        scripting/mt19937ar.c

        # ── App-level source files ────────────────────────────────────────
        ActivityDoc.cpp
        ActivityView.cpp
        DDV_validation.cpp
        exceptions.cpp
        Finding.cpp
        HyprLink.cpp
        MDIClientWnd.cpp
        MDITabs.cpp
        MUSHclient.cpp
        MakeWindowTransparent.cpp
        Mapping.cpp
        MySplitterWnd.cpp
        MyStatusBar.cpp
        NameGeneration.cpp
        StatLink.cpp
        TextDocument.cpp
        TextView.cpp
        TimerWnd.cpp
        UDPsocket.cpp
        accelerators.cpp
        activitychildfrm.cpp
        art.cpp
        chatlistensock.cpp
        chatsock.cpp
        childfrm.cpp
        format.cpp
        genprint.cpp
        globalregistryoptions.cpp
        mainfrm.cpp
        miniwindow.cpp
        mushview.cpp
        plugins.cpp
        scriptingoptions.cpp
        sendvw.cpp
        textchildfrm.cpp
        winplace.cpp
        world_debug.cpp

        # ── Scripting method implementations (Lua API) ────────────────────
        scripting/methods/methods.cpp
        scripting/methods/methods_accelerators.cpp
        scripting/methods/methods_aliases.cpp
        scripting/methods/methods_arrays.cpp
        scripting/methods/methods_chat.cpp
        scripting/methods/methods_clipboard.cpp
        scripting/methods/methods_colours.cpp
        scripting/methods/methods_commands.cpp
        scripting/methods/methods_database.cpp
        scripting/methods/methods_defaults.cpp
        scripting/methods/methods_info.cpp
        scripting/methods/methods_infobar.cpp
        scripting/methods/methods_logging.cpp
        scripting/methods/methods_mapping.cpp
        scripting/methods/methods_miniwindows.cpp
        scripting/methods/methods_notepad.cpp
        scripting/methods/methods_noting.cpp
        scripting/methods/methods_output.cpp
        scripting/methods/methods_panes.cpp
        scripting/methods/methods_plugins.cpp
        scripting/methods/methods_sending.cpp
        scripting/methods/methods_sounds.cpp
        scripting/methods/methods_speedwalks.cpp
        scripting/methods/methods_spellchecker.cpp
        scripting/methods/methods_timers.cpp
        scripting/methods/methods_tracing.cpp
        scripting/methods/methods_triggers.cpp
        scripting/methods/methods_udp.cpp
        scripting/methods/methods_utilities.cpp
        scripting/methods/methods_variables.cpp
        scripting/methods/methods_worlds.cpp
        scripting/methods/methods_xml.cpp

        # ── Dialogs — world preferences ───────────────────────────────────
        dialogs/world_prefs/ButtonColour.cpp
        dialogs/world_prefs/CommandOptionsDlg.cpp
        dialogs/world_prefs/EditVariable.cpp
        dialogs/world_prefs/MissingEntryPoints.cpp
        dialogs/world_prefs/PrefsPropertySheet.cpp
        dialogs/world_prefs/TabDefaultsDlg.cpp
        dialogs/world_prefs/TimerDlg.cpp
        dialogs/world_prefs/TreePropertySheet.cpp
        dialogs/world_prefs/aliasdlg.cpp
        dialogs/world_prefs/configuration.cpp
        dialogs/world_prefs/editmac.cpp
        dialogs/world_prefs/genpropertypage.cpp
        dialogs/world_prefs/prefspropertypages.cpp
        dialogs/world_prefs/triggdlg.cpp

        # ── Dialogs — global preferences ─────────────────────────────────
        dialogs/global_prefs/GlobalPrefs.cpp
        dialogs/global_prefs/GlobalPrefsSheet.cpp

        # ── Dialogs — plugins ─────────────────────────────────────────────
        dialogs/plugins/PluginWizard.cpp
        dialogs/plugins/PluginWizardSheet.cpp
        dialogs/plugins/PluginsDlg.cpp

        # ── Dialogs — chat ────────────────────────────────────────────────
        dialogs/chat/ChatCallDlg.cpp
        dialogs/chat/ChatDetailsDlg.cpp
        dialogs/chat/ChatListDlg.cpp
        dialogs/chat/ChatToAllDlg.cpp
        dialogs/chat/EmoteToAllDlg.cpp

        # ── Dialogs — general ─────────────────────────────────────────────
        dialogs/AsciiArtDlg.cpp
        dialogs/ChooseNotepadDlg.cpp
        dialogs/ColourComboBox.cpp
        dialogs/ColourPickerDlg.cpp
        dialogs/CompleteWordDlg.cpp
        dialogs/ConfirmPreamble.cpp
        dialogs/CreditsDlg.cpp
        dialogs/DebugLuaDlg.cpp
        dialogs/DebugWorldInputDlg.cpp
        dialogs/EditDlg.cpp
        dialogs/EditMultiLine.cpp
        dialogs/FindDlg.cpp
        dialogs/FunctionListDlg.cpp
        dialogs/GeneratedNameDlg.cpp
        dialogs/GlobalChangeDlg.cpp
        dialogs/GoToLineDlg.cpp
        dialogs/HighlightPhraseDlg.cpp
        dialogs/ImmediateDlg.cpp
        dialogs/ImportXMLdlg.cpp
        dialogs/InsertUnicodeDlg.cpp
        dialogs/KeyNameDlg.cpp
        dialogs/LuaChooseBox.cpp
        dialogs/LuaChooseList.cpp
        dialogs/LuaChooseListMulti.cpp
        dialogs/LuaGsubDlg.cpp
        dialogs/LuaInputBox.cpp
        dialogs/LuaInputEditDlg.cpp
        dialogs/MXPscriptRoutinesDlg.cpp
        dialogs/MapCommentDlg.cpp
        dialogs/MapDlg.cpp
        dialogs/MapMoveDlg.cpp
        dialogs/MultiLineTriggerDlg.cpp
        dialogs/ProgDlg.cpp
        dialogs/ProxyServerPasswordDlg.cpp
        dialogs/QuickConnectDlg.cpp
        dialogs/RecallDlg.cpp
        dialogs/RecallSearchDlg.cpp
        dialogs/RegexpProblemDlg.cpp
        dialogs/ScriptErrorDlg.cpp
        dialogs/SendToAllDlg.cpp
        dialogs/SpellCheckDlg.cpp
        dialogs/Splash.cpp
        dialogs/TextAttributesDlg.cpp
        dialogs/TipDlg.cpp
        dialogs/UniqueIDDlg.cpp
        dialogs/WinsockInfoDlg.cpp
        dialogs/cmdhist.cpp
        dialogs/logdlg.cpp
        dialogs/password.cpp
        dialogs/welcome.cpp
        dialogs/welcome1.cpp
    )

    add_library(mushapp STATIC ${MUSHAPP_SOURCES})

    if(LUA_FOUND)
        target_sources(mushapp PRIVATE
            # Lua C extensions (same as mushcore)
            scripting/bits.c
            scripting/lrexlib.c
            scripting/lua_compress.c
            scripting/lbc.c
            # mt19937ar.c is in unconditional MUSHAPP_SOURCES (used by miniwindow, dialogs, etc.)
            scripting/number.c

            # Full Lua scripting engine (replaces lua_scripting.cpp portable subset)
            scripting/lua_scripting.cpp

            # LPeg pattern-matching library
            scripting/lpeg/lptree.c
            scripting/lpeg/lpvm.c
            scripting/lpeg/lpcap.c
            scripting/lpeg/lpcode.c

            # SQLite3 Lua binding
            lsqlite/lsqlite3.c

            # Full Lua utilities (replaces lua_utils_portable.cpp)
            scripting/lua_utils.cpp

            # Full Lua method implementations (replaces lua_world_api_portable.cpp)
            scripting/lua_methods.cpp

            # Progress dialog for Lua (Windows-style, no-op with stubs)
            scripting/lua_progressdlg.cpp
        )
        target_compile_definitions(mushapp PRIVATE MUSHCLIENT_LUA_SCRIPTING)
    endif()

    target_include_directories(mushapp PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
        ${CMAKE_SOURCE_DIR}/pcre
        ${CMAKE_SOURCE_DIR}/scripting
        ${CMAKE_SOURCE_DIR}/scripting/lpeg
        ${CMAKE_SOURCE_DIR}/sqlite3
        ${CMAKE_SOURCE_DIR}/lsqlite
        ${CMAKE_SOURCE_DIR}/dialogs
    )

    target_compile_definitions(mushapp PRIVATE
        _NOSOUND              # DirectSound not available on macOS/Linux
        WIN32                 # MUSHclient uses WIN32 guards for Windows-specific paths
        MUSHCLIENT_FULL_BUILD # suppress portable stubs where real impls are compiled
        # MUSHCLIENT_LUA_SCRIPTING is added below only when Lua is found
    )

    target_compile_options(mushapp PRIVATE
        -Wall
        -Wno-deprecated-declarations
        -Wno-unused-variable
        -Wno-sign-compare
        -Wno-missing-field-initializers
        -Werror=non-pod-varargs
        -Wno-narrowing
        -Wno-unused-function
    )

    # mushapp vendors its own copies of the C libraries to avoid duplicate
    # symbols with mushcore (which also links mcpcre/mcsqlite3/mczlib).
    # mushgui links mushapp only — not mushcore — to prevent duplicate symbols.
    target_link_libraries(mushapp PUBLIC
        mcpcre
        mcsqlite3
        mczlib
        PNG::PNG
    )

    if(LUA_FOUND)
        target_include_directories(mushapp PUBLIC ${LUA_INCLUDE_DIR})
        target_link_libraries(mushapp PUBLIC ${LUA_LIBRARIES})
    endif()

    if(OpenSSL_FOUND OR OPENSSL_FOUND)
        target_include_directories(mushapp PUBLIC ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(mushapp PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(mushapp PRIVATE OPENSSL_FOUND)
    endif()

    # ── mushcli — minimal CLI driver (Phase 4) ───────────────────────────────
    add_executable(mushcli platform/cli_main.cpp)
    target_link_libraries(mushcli PRIVATE mushcore)
    target_include_directories(mushcli PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform
    )

    # Optional: readline (GNU readline or libedit) for command history /
    # line editing.  Present on most systems; mushcli builds without it too.
    find_library(READLINE_LIB NAMES readline)
    find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h)
    if(READLINE_LIB AND READLINE_INCLUDE_DIR)
        message(STATUS "readline found: ${READLINE_LIB}")
        target_link_libraries(mushcli PRIVATE ${READLINE_LIB})
        target_include_directories(mushcli PRIVATE ${READLINE_INCLUDE_DIR})
        target_compile_definitions(mushcli PRIVATE HAVE_READLINE)
    else()
        message(STATUS "readline not found — mushcli will use basic line input (no history/editing)")
    endif()

    # ── mushgui — wxWidgets GUI client (Phase 5) ─────────────────────────────
    # Requires wxWidgets 3.x AND Lua 5.1 (mushapp links both).
    # On macOS: brew install wxwidgets lua@5.1
    # On Linux: apt install libwxgtk3.0-gtk3-dev liblua5.1-dev
    find_package(wxWidgets QUIET COMPONENTS core base)

    # Fallback: if find_package didn't find wx but a wx-config binary is known,
    # query it directly.  This handles the case where wx was built from source
    # and the system pkg-config entries (gtk3 etc.) are not in the standard path.
    if(NOT wxWidgets_FOUND AND wxWidgets_CONFIG_EXECUTABLE)
        execute_process(
            COMMAND "${wxWidgets_CONFIG_EXECUTABLE}" --cxxflags
            OUTPUT_VARIABLE _wx_cxx OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(
            COMMAND "${wxWidgets_CONFIG_EXECUTABLE}" --libs core,base
            OUTPUT_VARIABLE _wx_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(
            COMMAND "${wxWidgets_CONFIG_EXECUTABLE}" --version
            OUTPUT_VARIABLE _wx_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_wx_ver)
            set(wxWidgets_FOUND TRUE)
            # Parse cxxflags: -I entries → INCLUDE_DIRS, -D entries → DEFINITIONS
            string(REGEX MATCHALL "-I[^ ]+" _wx_inc_raw "${_wx_cxx}")
            string(REGEX MATCHALL "-D[^ ]+" _wx_def_raw "${_wx_cxx}")
            string(REGEX REPLACE "-I" "" wxWidgets_INCLUDE_DIRS "${_wx_inc_raw}")
            string(REPLACE ";" " " wxWidgets_DEFINITIONS "${_wx_def_raw}")
            # Remove the -D/-I entries from cxxflags to leave only other flags
            string(REGEX REPLACE "-[ID][^ ]+ ?" "" wxWidgets_CXX_FLAGS "${_wx_cxx}")
            string(STRIP "${wxWidgets_CXX_FLAGS}" wxWidgets_CXX_FLAGS)
            # Split the lib flags into a CMake list for target_link_libraries
            separate_arguments(wxWidgets_LIBRARIES UNIX_COMMAND "${_wx_libs}")
            set(wxWidgets_VERSION_STRING "${_wx_ver}")
            message(STATUS "wxWidgets found via wx-config: ${_wx_ver} — building mushgui")
        endif()
    endif()

    if(wxWidgets_FOUND AND LUA_FOUND)
        message(STATUS "wxWidgets found: ${wxWidgets_VERSION_STRING} — building mushgui")
        # Do NOT use include(wxWidgets_USE_FILE) — it sets flags globally and
        # injects -arch x86_64 (or whatever arch wx was built for) into every
        # target in the project.  Apply wx settings to mushgui only.

        add_executable(mushgui MACOSX_BUNDLE
            # wx entry point (wx headers only — no MFC/MUSHclient headers)
            platform/wx_app.cpp
            # bridge: includes BOTH wx and MUSHclient headers; implements hooks
            platform/wx_mfc_impl.cpp
            # GDI compatibility layer: wx-backed CDC/CBitmap/CPen/CBrush/CFont
            # Resolves the GDI symbols left undefined by mushapp's miniwindow.cpp
            platform/wx_gdi_compat.cpp
            # GUI widgets (wx headers first, then MUSHclient headers)
            platform/wx_outputpane.cpp
            platform/wx_world_child.cpp
            platform/wx_notepad.cpp
            platform/wx_entity_dlgs.cpp
            # MFC dialog bridge: parses the recovered dialog/menu resources so the
            # canonical dialogs/**/*.cpp render natively (vs hand-rebuilt in wx)
            platform/wx_dialog_resource.cpp
            platform/wx_dialog_bridge.cpp
        )
        # mushgui links mushapp (full MUSHclient engine with doc.cpp, dialogs,
        # scripting, etc.) rather than the portable mushcore stub.
        # wx_mfc_impl.cpp bridges the MFC compatibility layer to real wxWidgets.
        target_link_libraries(mushgui PRIVATE mushapp ${wxWidgets_LIBRARIES})
        target_include_directories(mushgui PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/platform
            ${wxWidgets_INCLUDE_DIRS}
        )
        target_compile_definitions(mushgui PRIVATE
            ${wxWidgets_DEFINITIONS}
            MUSHCLIENT_WX_BACKEND   # signals that wx is the GUI backend
        )
        target_compile_options(mushgui PRIVATE
            ${wxWidgets_CXX_FLAGS}
            -Wall
            -Wno-deprecated-declarations
            -Werror=non-pod-varargs
            -Wno-unused-variable
            -Wno-sign-compare
            -Wno-missing-field-initializers
            -Wno-narrowing
            -Wno-unused-function
        )
        # wxWidgets on macOS needs the bundle type for proper menu bar / event loop
        if(APPLE)
            set_target_properties(mushgui PROPERTIES
                MACOSX_BUNDLE_GUI_IDENTIFIER  "au.com.gammon.MUSHclient"
                MACOSX_BUNDLE_BUNDLE_NAME     "MUSHclient"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "5.07"
            )
            # Bundle the Lua module library into the .app so plugins can
            # require() the shipped modules (movewindow, mapper, gauge, …)
            # regardless of the working directory.  OpenLuaDelayed() adds
            # Contents/Resources/lua to package.path on non-Windows.
            add_custom_command(TARGET mushgui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/lua"
                    "$<TARGET_BUNDLE_DIR:mushgui>/Contents/Resources/lua"
                COMMENT "Bundling lua/ modules into mushgui.app/Contents/Resources")
            # Bundle the recovered dialog resources + the resource.h symbol table
            # so the MFC-dialog bridge can parse them at runtime.
            add_custom_command(TARGET mushgui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_SOURCE_DIR}/platform/mushclient_dialogs.rc"
                    "${CMAKE_SOURCE_DIR}/resource.h"
                    "$<TARGET_BUNDLE_DIR:mushgui>/Contents/Resources/"
                COMMENT "Bundling dialog resources into mushgui.app/Contents/Resources")
        endif()
    elseif(NOT wxWidgets_FOUND)
        message(STATUS "wxWidgets not found — mushgui target skipped")
        message(STATUS "  macOS: brew install wxwidgets")
        message(STATUS "  Linux: apt install libwxgtk3.0-gtk3-dev wx-common")
    else()
        message(STATUS "Lua not found — mushgui target skipped (mushgui requires both wxWidgets and Lua)")
        message(STATUS "  macOS: brew install lua@5.1")
        message(STATUS "  Linux: apt install liblua5.1-dev")
    endif()

endif() # NOT WIN32

# ── Unit tests ────────────────────────────────────────────────────────────

if(NOT WIN32)
    enable_testing()
    add_subdirectory(tests)
endif()
