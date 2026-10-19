# ============================================================================
# SSX Tricky -- Linux and Nintendo Switch builds (included by CMakeLists.txt)
#
#   Switch (devkitPro, devkitA64 + libnx + switch-sdl2 + switch-mesa):
#     aarch64-none-elf-cmake -S port -B port/build-switch -DCMAKE_BUILD_TYPE=Release
#     cmake --build port/build-switch -j      -> "SSX Tricky.nro"
#   Linux (SDL2, OpenGL):
#     cmake -S port -B port/build-linux -DCMAKE_BUILD_TYPE=Release
#
# The Win32 calls the game's host code makes come from xboxrecomp's platform
# layer plus port/src/posix/ (headers named after the Windows ones, so the
# shared sources keep their includes). See docs/switch.md.
# ============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(SSX_PS2_BUTTONS "Build the PS2 original button style" ON)
if(SSX_PS2_BUTTONS)
    add_compile_definitions(SSX_PS2_BUTTONS_READY=1)
endif()

# A build check without the game: every function the translated code would
# provide becomes a stub that stops the program (tools/switch/stub_gen.py).
# The result builds and links but does not play; CI uses it.
option(SSX_STUB_GEN "Link without the translated game (build check only)" OFF)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES AND NOT SSX_STUB_GEN)
    message(FATAL_ERROR
        "port/src/recomp/gen/ is empty: the translated game code has not been "
        "generated yet. It is made from your own copy of the game and is never "
        "stored in this repository -- see docs/switch.md.")
endif()

set(GAME_SOURCES
    src/main.c
    src/launcher.c
    src/settings.c
    src/controls.c
    src/hostui.c
    src/recomp_manual.c
    src/pass_tags.c
    src/fps_cap.c
    src/aspect.c
    src/drawdist.c
    src/terrain_seams.c
    src/pumplag.c
    src/strmwalk.c
    src/chantfix.c
    src/ctlscheme.c
    src/ps2legend.c
    src/fullfade.c
    src/hud_anchor.c
    src/ticktrace.c
    src/perf_hooks.c
    src/netplay/np_cmdlog.c
    src/netplay/np_ghost.c
    src/netplay/np_racebench.c
    src/netplay/np_net.c
    src/xapi_input_hle.c
    src/recomp/recomp_probe.c
    src/posix/ot_posix.c
    src/posix/ot_pad.c
    src/posix/ot_crash.c
    src/posix/ot_switch_nx.c
)

# ── Resources ──────────────────────────────────────────────────────────────
# What the Windows build links into the executable as RCDATA (ssx_recomp.rc.in)
# is copied to res/<NAME>: in the Switch romfs, or beside the Linux binary.
# FindResourceA (xboxrecomp's win32_compat.c) reads them back from there.
set(SSX_RES_DIR "${CMAKE_BINARY_DIR}/romfs/res")
set(SSX_BTNICONS "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/juliocacko")
set(SSX_BTNICONS_DRAWN "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/drawn")
set(SSX_BTNICONS_PADS "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/pads")
set(SSX_HDTEX_INDEX "${CMAKE_CURRENT_SOURCE_DIR}/assets/hdtex_index.txt")
file(GLOB SSX_BTNICONS_PS2 "${CMAKE_CURRENT_SOURCE_DIR}/assets/btnicons/ps2/ps2_*.png")
set(SSX_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(SSX_APP_ICON "") # unused here
set(SSX_MANIFEST "")
set(SSX_BTNICONS_PS2_RC "")
if(SSX_PS2_BUTTONS)
    foreach(f IN LISTS SSX_BTNICONS_PS2)
        get_filename_component(n "${f}" NAME_WE)
        string(TOUPPER "${n}" n)
        string(APPEND SSX_BTNICONS_PS2_RC "${n} RCDATA \"${f}\"\n")
    endforeach()
endif()
configure_file(src/ssx_recomp.rc.in "${CMAKE_BINARY_DIR}/ssx_recomp_posix.rc" @ONLY)
file(STRINGS "${CMAKE_BINARY_DIR}/ssx_recomp_posix.rc" _rc_lines REGEX "^[A-Z0-9_]+ +RCDATA +\"")
file(MAKE_DIRECTORY "${SSX_RES_DIR}")
set(SSX_RES_FILES "")
foreach(line IN LISTS _rc_lines)
    string(REGEX REPLACE "^([A-Z0-9_]+) +RCDATA +\"([^\"]+)\".*" "\\1;\\2" _pair "${line}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _src)
    add_custom_command(OUTPUT "${SSX_RES_DIR}/${_name}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${SSX_RES_DIR}/${_name}"
        DEPENDS "${_src}" VERBATIM)
    list(APPEND SSX_RES_FILES "${SSX_RES_DIR}/${_name}")
endforeach()
add_custom_target(ssx_resources DEPENDS ${SSX_RES_FILES})

# ── Translated game code, or the build-check stubs ─────────────────────────
add_library(ssx_host OBJECT ${GAME_SOURCES})
# The usage requirements (include paths) of the runtime.
target_link_libraries(ssx_host PRIVATE xboxrecomp)
# Some host sources only include <windows.h> under _WIN32 and still use its
# types; giving every one the compat header keeps them unchanged.
target_compile_options(ssx_host PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/posix/compat/windows.h"
                                        -Werror=implicit-function-declaration)
# WCHAR is wchar_t in host code, as on Windows (xbox_winnt.h).
target_compile_definitions(ssx_host PRIVATE XBOX_HOST_WCHAR=1)
set(SSX_STUB_SOURCES "")
if(SSX_STUB_GEN AND NOT RECOMP_GEN_SOURCES)
    set(SSX_STUB_C "${CMAKE_BINARY_DIR}/stub_gen/stubs.c")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Stubs for every symbol the host code and runtime expect from the
    # translated game: whatever is still undefined after a trial link.
    add_custom_command(OUTPUT "${SSX_STUB_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/stub_gen.py"
                --nm "${CMAKE_NM}" --out "${SSX_STUB_C}"
                $<TARGET_OBJECTS:ssx_host>
                $<TARGET_FILE:xbox_kernel> $<TARGET_FILE:xbox_d3d8> $<TARGET_FILE:xbox_dsound>
                $<TARGET_FILE:xbox_input> $<TARGET_FILE:xbox_apu> $<TARGET_FILE:xbox_nv2a>
                $<TARGET_FILE:platform>
        DEPENDS ssx_host xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_apu xbox_nv2a platform
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/stub_gen.py"
        COMMAND_EXPAND_LISTS
        COMMENT "Stubs for the translated game (build check)")
    set(SSX_STUB_SOURCES "${SSX_STUB_C}")
endif()

add_executable(${PROJECT_NAME}
    $<TARGET_OBJECTS:ssx_host>
    ${RECOMP_GEN_SOURCES}
    ${SSX_STUB_SOURCES}
)
add_dependencies(${PROJECT_NAME} ssx_resources)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ssx_tricky")

foreach(t ssx_host ${PROJECT_NAME})
    target_include_directories(${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/posix/compat
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
        ${XBOXRECOMP_DIR}/include
        ${XBOXRECOMP_DIR}/src
    )
    # The generated code and the host sources were written for GCC on
    # Windows; these keep its warnings from stopping the build.
    target_compile_options(${t} PRIVATE -Wno-int-conversion -Wno-incompatible-pointer-types
                                        -fno-strict-aliasing -fwrapv)
endforeach()

option(OT_RELEASE "Release build (version without -dev)" OFF)
if(OT_RELEASE)
    target_compile_definitions(ssx_host PRIVATE OT_RELEASE=1)
endif()

find_package(SDL2 REQUIRED)
target_include_directories(ssx_host PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp SDL2::SDL2 m)

if(NINTENDO_SWITCH)
    # Title, author and version on the Homebrew Menu.
    file(STRINGS "${SSX_SRC_DIR}/version.h" _v REGEX "#define OT_VERSION_(MAJOR|MINOR|PATCH) ")
    string(REGEX REPLACE ".*MAJOR +([0-9]+).*MINOR +([0-9]+).*PATCH +([0-9]+).*" "\\1.\\2.\\3" OT_VERSION "${_v}")
    if(NOT OT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        set(OT_VERSION "0.0.0")
    endif()
    nx_generate_nacp(ssx_tricky.nacp
        NAME "SSX Tricky (OpenTricky)"
        AUTHOR "OpenTricky contributors"
        VERSION "${OT_VERSION}")
    set(_nro_icon "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch/icon.jpg")
        set(_nro_icon ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch/icon.jpg")
    endif()
    nx_create_nro(${PROJECT_NAME}
        OUTPUT "${CMAKE_BINARY_DIR}/SSX Tricky.nro"
        NACP ssx_tricky.nacp
        ROMFS "${CMAKE_BINARY_DIR}/romfs"
        ${_nro_icon})
endif()
