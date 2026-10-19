# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 (d3d8_gl_*.c); GL loaded by
#             libepoxy on Linux, glad on Nintendo Switch
#   POSIX with -DXBOX_VULKAN=ON -> backed by Vulkan 1.3 (d3d8_vk_*.c): the
#             system loader on Linux, NVK (mesa-switch) on Nintendo Switch;
#             shaders compiled at run time by glslang

option(XBOX_VULKAN "Use the Vulkan renderer instead of OpenGL (POSIX)" OFF)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_post.c
        d3d8_softshadow.c
        d3d8_gpuprof.c
        d3d8_sync.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_statecache.c
        d3d8_combiners.c
        d3d8_nv2a.c
        d3d8_shcache.c
        d3d8_nv2a_vsh.c
        d3d8_vsh.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler windowscodecs ole32)
elseif(XBOX_VULKAN)
    add_library(xbox_d3d8 STATIC
        d3d8_vk_core.c
        d3d8_vk_device.c
        d3d8_vk_resources.c
        d3d8_vk_draw.c
        d3d8_gl_vsh.c
    )
    target_compile_definitions(xbox_d3d8 PUBLIC XBOX_VULKAN=1)
    # { VK_STRUCTURE_TYPE_... } initialisers leave the rest zero on purpose
    target_compile_options(xbox_d3d8 PRIVATE -Wno-missing-field-initializers)
    find_package(SDL2 REQUIRED)
    # glslang and SPIRV-Tools as static libraries, in one link group so their
    # order does not matter. GLSLANG_DIR: an install prefix (the system's if
    # empty).
    set(GLSLANG_DIR "" CACHE PATH "glslang install prefix (built for the Switch there)")
    if(GLSLANG_DIR)
        set(_gl_hint PATHS ${GLSLANG_DIR}/lib NO_DEFAULT_PATH)
        target_include_directories(xbox_d3d8 PUBLIC ${GLSLANG_DIR}/include)
    endif()
    foreach(lib glslang glslang-default-resource-limits SPIRV MachineIndependent OSDependent GenericCodeGen SPIRV-Tools-opt SPIRV-Tools)
        find_library(GLSLANG_LIB_${lib} NAMES lib${lib}.a ${lib} ${_gl_hint})
        if(GLSLANG_LIB_${lib})
            list(APPEND GLSLANG_LIBS ${GLSLANG_LIB_${lib}})
        endif()
    endforeach()
    if(NOT GLSLANG_LIB_glslang)
        message(FATAL_ERROR "XBOX_VULKAN: glslang not found (set GLSLANG_DIR)")
    endif()
    if(NINTENDO_SWITCH)
        # Switch: NVK from mesa-switch, linked in (no loader; surfaces through
        # VK_NN_vi_surface). NVK_SDK: its install prefix. danfromtico's
        # mesa-switch installs lib/libvulkan.a with a CMake package that adds
        # its own dependencies (libelf, expat, zstd, zlib); older builds a
        # bare lib/libvulkan_nouveau.a.
        set(NVK_SDK "" CACHE PATH "mesa-switch install prefix (include/vulkan, lib/libvulkan.a)")
        if(EXISTS "${NVK_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake")
            # its dependencies from the SDK itself when they are there (the
            # "Mesa NVK SDK" workflow puts them in), else from portlibs
            foreach(dep ELF:elf EXPAT:expat ZSTD:zstd Z:z)
                string(REPLACE ":" ";" dep "${dep}")
                list(GET dep 0 var)
                list(GET dep 1 lib)
                if(EXISTS "${NVK_SDK}/lib/lib${lib}.a")
                    set(_VULKAN_SWITCH_${var}_LIBRARY "${NVK_SDK}/lib/lib${lib}.a" CACHE FILEPATH "" FORCE)
                endif()
            endforeach()
            # NVK calls nothing in libelf, which devkitPro does not package:
            # without it, a library the link has anyway stands in
            find_library(_VULKAN_SWITCH_ELF_LIBRARY elf)
            if(NOT _VULKAN_SWITCH_ELF_LIBRARY)
                set(_VULKAN_SWITCH_ELF_LIBRARY m CACHE STRING "" FORCE)
            endif()
            # set ahead: the Switch toolchain only searches its own sysroot
            set(Vulkan_LIBRARY "${NVK_SDK}/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
            set(Vulkan_INCLUDE_DIR "${NVK_SDK}/include" CACHE PATH "" FORCE)
            include("${NVK_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake")
            if(NOT Vulkan_FOUND)
                message(FATAL_ERROR "NVK_SDK=${NVK_SDK}: VulkanConfig.cmake found no usable Vulkan")
            endif()
            target_include_directories(xbox_d3d8 PUBLIC ${Vulkan_INCLUDE_DIRS})
            # the package's own list, in one link group of ours ($<LINK_GROUP>
            # is not available to C targets with this toolchain)
            target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2
                -Wl,--start-group ${GLSLANG_LIBS} ${_VULKAN_SWITCH_LIBRARIES} -Wl,--end-group nx stdc++ m
                -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion -pthread)
            # devkitPro's SDL2 links the EGL of its own Mesa 20.1, which
            # clashes with this one: the SDK's EGL and glapi instead (the
            # full mesa-switch SDK has them), and no libdrm_nouveau
            if(EXISTS "${NVK_SDK}/lib/libEGL.a")
                # the static library's target, made global by port/posix.cmake
                get_target_property(_sdl_libs SDL2::SDL2-static INTERFACE_LINK_LIBRARIES)
                if(_sdl_libs)
                    set(_sdl_new "")
                    foreach(l IN LISTS _sdl_libs)
                        if(l STREQUAL "EGL" OR l STREQUAL "-lEGL")
                            list(APPEND _sdl_new "${NVK_SDK}/lib/libEGL.a")
                        elseif(l STREQUAL "glapi" OR l STREQUAL "-lglapi")
                            list(APPEND _sdl_new "${NVK_SDK}/lib/libglapi.a")
                        elseif(NOT (l STREQUAL "drm_nouveau" OR l STREQUAL "-ldrm_nouveau"))
                            list(APPEND _sdl_new "${l}")
                        endif()
                    endforeach()
                    set(_sdl_libs "${_sdl_new}")
                    set_target_properties(SDL2::SDL2-static PROPERTIES INTERFACE_LINK_LIBRARIES "${_sdl_libs}")
                endif()
            else()
                message(WARNING "NVK_SDK has no libEGL.a: SDL2 links devkitPro's own Mesa next to NVK (use the full mesa-switch SDK)")
            endif()
        else()
            find_library(NVK_LIBRARY NAMES vulkan_nouveau vulkan PATHS ${NVK_SDK}/lib REQUIRED NO_DEFAULT_PATH)
            target_include_directories(xbox_d3d8 PUBLIC ${NVK_SDK}/include)
            target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2
                -Wl,--start-group ${GLSLANG_LIBS} -Wl,--end-group ${NVK_LIBRARY} nx stdc++ m)
        endif()
    else()
        find_package(Vulkan REQUIRED)
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 Vulkan::Vulkan
            -Wl,--start-group ${GLSLANG_LIBS} -Wl,--end-group stdc++ m)
    endif()
else()
    # OpenGL 3.3 core backend (d3d8_gl_*.c) for Linux and Nintendo Switch.
    add_library(xbox_d3d8 STATIC
        d3d8_gl_device.c
        d3d8_gl_resources.c
        d3d8_gl_draw.c
        d3d8_gl_vsh.c
    )
    find_package(SDL2 REQUIRED)
    if(NINTENDO_SWITCH)
        # Switch: mesa (EGL + glapi + nouveau) from portlibs, GL entry points
        # through glad (no epoxy on Switch).
        find_package(OpenGL REQUIRED)
        find_library(GLAD_LIBRARY glad REQUIRED)
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${GLAD_LIBRARY} OpenGL::EGL OpenGL::GL)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(EPOXY REQUIRED epoxy)
        target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
        target_link_libraries(xbox_d3d8 PUBLIC m)
    endif()
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# Host test of the shader disk cache (no game, no D3D):
#   cmake -DXBOX_BUILD_TESTS=ON ... ; cmake --build ... --target shcache_test
option(XBOX_BUILD_TESTS "Build the host tests of the D3D layer (shcache_test)" OFF)
if(XBOX_BUILD_TESTS)
    add_executable(shcache_test tests/shcache_test.c d3d8_shcache.c)
    target_include_directories(shcache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(shcache_test PRIVATE Threads::Threads)
    endif()
    if(XBOX_VULKAN AND NOT NINTENDO_SWITCH)
        # The Vulkan renderer without the game (tests/vk_smoke_test.c).
        add_executable(vk_smoke_test tests/vk_smoke_test.c ../nv2a/nv2a_psh.c)
        target_link_libraries(vk_smoke_test PRIVATE xbox_d3d8)
    endif()
endif()
