# The macOS/Linux port's dialogs, menus, bitmaps and icons, built from en.rc:
# the same source Windows builds locale\en.dll from (README.md here), so the
# two cannot drift apart.
#
#   en.rc --llvm-rc--> en.res --tools/port_resources.py--> port/
#                                  port/mushclient_dialogs.rc   (wx dialog bridge)
#                                  port/art/bitmap/<id>.bmp, port/art/icon/<id>.ico
#
# llvm-rc is LLVM's rc.exe work-alike: Homebrew's llvm (brew install llvm) or
# the llvm and clang packages on Linux (apt install llvm clang).  It runs clang
# to preprocess en.rc, and llvm-rc/afxres.h here stands in for the Windows SDK
# headers.  The other output, port/, is what the app bundles; the variable
# MC_PORT_RESOURCES_DIR, set in the parent scope, names it.

# Homebrew's llvm is keg-only (not linked onto PATH), and Debian/Ubuntu name
# theirs llvm-rc-<version> under /usr/lib/llvm-<version>/bin.  Newest first.
file(GLOB _mc_llvm_dirs
    /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@*/bin
    /usr/local/opt/llvm/bin    /usr/local/opt/llvm@*/bin
    /usr/lib/llvm-*/bin)
list(SORT _mc_llvm_dirs)
list(REVERSE _mc_llvm_dirs)
set(_mc_llvm_rc_names)
foreach(_v RANGE 14 30)
    list(APPEND _mc_llvm_rc_names llvm-rc-${_v})
endforeach()
list(REVERSE _mc_llvm_rc_names)
find_program(MC_LLVM_RC NAMES llvm-rc ${_mc_llvm_rc_names} PATHS ${_mc_llvm_dirs})
if(NOT MC_LLVM_RC)
    message(FATAL_ERROR
        "llvm-rc not found: it compiles resources/en.rc, the app's dialogs, menus and art.\n"
        "  macOS: brew install llvm\n"
        "  Linux: apt install llvm clang   (or your distribution's equivalent)\n"
        "or pass -DMC_LLVM_RC=/path/to/llvm-rc.")
endif()
find_program(MC_PYTHON NAMES python3 python)
if(NOT MC_PYTHON)
    message(FATAL_ERROR "python3 not found: tools/port_resources.py converts the compiled resources/en.rc for the app")
endif()
message(STATUS "Resources: llvm-rc at ${MC_LLVM_RC}")

# The repository root, from here rather than CMAKE_SOURCE_DIR, so that this
# directory builds the same whichever project includes it.
get_filename_component(_mc_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(_mc_res  "${CMAKE_CURRENT_BINARY_DIR}/en.res")
set(_mc_port "${CMAKE_CURRENT_BINARY_DIR}/port")
# Everything en.rc names: its art, its texts, and the files it includes.
file(GLOB _mc_rc_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/text/*"
    "${_mc_root}/src/platform/dialog_text/*")

add_custom_command(
    OUTPUT "${_mc_res}"
    # The options tests/check_port_resources.py compiles with on Windows,
    # where it checks the result against rc.exe's.  -C 1252: llvm-rc does not
    # act on en.rc's #pragma code_page.
    COMMAND "${MC_LLVM_RC}" -nologo -C 1252 -D_WIN32 -DNDEBUG
            -I "${CMAKE_CURRENT_SOURCE_DIR}/llvm-rc"
            -I "${_mc_root}/src"
            -fo "${_mc_res}" "${CMAKE_CURRENT_SOURCE_DIR}/en.rc"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS en.rc llvm-rc/afxres.h "${_mc_root}/src/resource.h" ${_mc_rc_inputs}
    COMMENT "Compiling resources/en.rc (llvm-rc)"
    VERBATIM)
add_custom_command(
    OUTPUT "${_mc_port}/mushclient_dialogs.rc"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_mc_port}"
    COMMAND "${MC_PYTHON}" "${_mc_root}/tools/port_resources.py"
            "${_mc_res}" "${_mc_root}/src/resource.h" "${_mc_port}"
    DEPENDS "${_mc_res}" "${_mc_root}/tools/port_resources.py"
            "${_mc_root}/src/resource.h"
    COMMENT "Converting resources/en.rc's dialogs, menus and art for the app"
    VERBATIM)
add_custom_target(port_resources ALL DEPENDS "${_mc_port}/mushclient_dialogs.rc")

set(MC_PORT_RESOURCES_DIR "${_mc_port}" PARENT_SCOPE)
