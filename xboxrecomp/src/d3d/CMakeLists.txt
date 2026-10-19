# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 (d3d8_gl_*.c); GL loaded by
#             libepoxy on Linux, glad on Nintendo Switch

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
else()
    # OpenGL 3.3 core backend (d3d8_gl_*.c) for Linux and Nintendo Switch.
    add_library(xbox_d3d8 STATIC
        d3d8_gl_device.c
        d3d8_gl_resources.c
        d3d8_gl_draw.c
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
endif()
