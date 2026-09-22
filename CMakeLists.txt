# Melee PC port build (32-bit x86).
#
# The port MUST be built as 32-bit: game file structs assume 4-byte
# pointers (GameCube layout), so on x86-64 every field after the first
# pointer sits at the wrong offset. Endianness is fixed separately
# (Stage 2, src/pc/pc_endian.h); pointer width is fixed by the target.
#
#   Windows: cmake -S . -B build_pc -A Win32
#   Linux:   cmake -S . -B build_pc -DCMAKE_BUILD_TYPE=Release  (adds -m32;
#            needs gcc-multilib/g++-multilib and i386 GL/X11 dev packages)
#   cmake --build build_pc --config Release
#
# Needs: a C99/C++17 toolchain, system OpenGL, network access on the first
# configure (GLFW + Dear ImGui via FetchContent). Offline configure falls
# back to no-ImGui automatically (MELEE_WANT_IMGUI=OFF also forces it).
#
# Run: build_pc/bin/melee_pc --disc <GALE01 root> [--configure|--help]
cmake_minimum_required(VERSION 3.20)
project(melee_pc_port LANGUAGES C CXX)

# ---------------------------------------------------------------- 32-bit only
# GameCube pointers are 4 bytes; a 64-bit build silently shifts every
# struct field that follows a pointer. Fail fast with instructions.
if(APPLE)
    message(FATAL_ERROR "[melee] macOS cannot target 32-bit x86; "
        "Windows (MSVC -A Win32) or Linux (GCC -m32) required.")
endif()
set(MELEE_USE_M32 OFF)
if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "[melee] MSVC build must target Win32 "
            "(4-byte pointers, like the GameCube): re-configure with "
            "`cmake -A Win32 ...`.")
    endif()
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES
        "^(x86_64|AMD64|x64)$")
    message(STATUS "[melee] 64-bit host compiler: adding -m32 for a "
        "32-bit target (needs multilib).")
    add_compile_options(-m32)
    add_link_options(-m32)
    set(MELEE_USE_M32 ON)
    if(UNIX AND NOT APPLE)
        # find_*() calls otherwise keep probing the 64-bit multiarch
        # tree (CMAKE_LIBRARY_ARCHITECTURE was detected pre-m32), so
        # OpenGL/X11 resolve to unusable 64-bit libs or nothing at all.
        set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
    endif()
endif()

# Port-wide feature flag (must be visible to every target: e.g. the
# SDK os.h header switches bus-clock address macros to real variables).
add_compile_definitions(MELEE_PC_PORT=1)
# MSVC's <math.h> only exposes M_PI/M_PI_2/... with this defined; the
# decomp uses them freely (MWERKS MSL always provides them). No-op on
# GCC/glibc, which define them unconditionally.
add_compile_definitions(_USE_MATH_DEFINES)

# C11 for _Static_assert (used by STATIC_ASSERT in game headers); the
# decomp itself builds these headers with MWERKS, MSVC needs the mode
# spelled out explicitly.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- options
option(MELEE_WANT_IMGUI "Fetch Dear ImGui for the settings overlay" ON)
option(MELEE_BUILD_GAME "Compile the game sources (melee+sysdolphin)" ON)

# ---------------------------------------------------------------- platform
if(WIN32)
    set(MELEE_PLATFORM_WINDOWS 1)
elseif(APPLE)
    set(MELEE_PLATFORM_MACOS 1)
elseif(UNIX)
    set(MELEE_PLATFORM_LINUX 1)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------- deps
if(MELEE_PLATFORM_LINUX AND MELEE_USE_M32)
    # 32-bit Mesa ships libGL.so without the libOpenGL/libGLX GLVND
    # split: use the legacy libGL target (already covered by the -dev
    # packages in CI). We link only GL 1.1 entry points anyway; the rest
    # is loaded at runtime by gl_loader.
    # (Both spellings: the exact FindOpenGL variable name varies by
    # CMake version/docs; setting both is harmless.)
    set(OpenGL_GL_PREFERENCE LEGACY)
    set(OPENGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# GLFW (window + input). Pinned for reproducible builds.
FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# X11 only: Wayland backend needs wayland-scanner (often missing for
# multilib/CI); XWayland covers Wayland desktops at runtime.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Dear ImGui (optional settings overlay).
set(MELEE_HAVE_IMGUI OFF)
if(MELEE_WANT_IMGUI)
    FetchContent_Declare(
        imgui
        URL https://github.com/ocornut/imgui/archive/refs/heads/master.zip
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        message(STATUS "[melee] fetching Dear ImGui...")
        FetchContent_Populate(imgui)
    endif()
    if(EXISTS "${imgui_SOURCE_DIR}/imgui.cpp")
        set(MELEE_HAVE_IMGUI ON)
        add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )
        target_include_directories(imgui PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        target_link_libraries(imgui PUBLIC glfw)
        message(STATUS "[melee] Dear ImGui enabled")
    else()
        message(WARNING "[melee] ImGui sources missing, overlay disabled")
    endif()
endif()

# ---------------------------------------------------------------- includes
# NOTE: src/hal/shim comes FIRST so the PC replacements for
# <dolphin/gx/GXVert.h> and <dolphin/gx/GXGeometry.h> shadow the originals
# (the originals write the GP FIFO at 0xCC008000 / are empty inlines).
set(MELEE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/dolphin/include
)

# ---------------------------------------------------------------- flags
if(MSVC)
    add_compile_options(/utf-8 /W3 /wd4100 /wd4101 /wd4189 /wd4200 /wd4204
        /wd4214 /wd4221 /wd4244 /wd4267 /wd4305 /wd4311 /wd4312 /wd4334
        /wd4477 /wd4701 /wd4702 /wd4706 /wd4804 /wd4819 /wd4828 /wd4996)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS=1)
else()
    add_compile_options(-fno-strict-aliasing -fno-math-errno
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
        -Wno-missing-braces -Wno-parentheses -Wno-sign-compare
        -Wno-implicit-function-declaration)
endif()

# NOTE: no low-image-base tricks needed. The build is 32-bit, so every
# pointer fits u32 losslessly under ASLR/PIE; MRAM-vs-ARAM is decided by
# range (sdk2_addr_is_aram), and the arena has a 32 MB floor
# (see src/sdk2/sdk2_lowmem.*).

# ---------------------------------------------------------------- HAL
file(GLOB HAL_SOURCES CONFIGURE_DEPENDS src/hal/opengl/*.c)
add_library(melee_hal STATIC ${HAL_SOURCES})
target_include_directories(melee_hal PUBLIC
    ${MELEE_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hal/opengl
)
target_link_libraries(melee_hal PUBLIC
    ${OPENGL_LIBRARIES} glfw Threads::Threads
)
if(MELEE_PLATFORM_WINDOWS)
    target_link_libraries(melee_hal PUBLIC opengl32 gdi32 user32 shell32
        winmm)
elseif(MELEE_PLATFORM_LINUX)
    target_link_libraries(melee_hal PUBLIC dl m)
elseif(MELEE_PLATFORM_MACOS)
    target_link_libraries(melee_hal PUBLIC
        "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

# ---------------------------------------------------------------- SDK2
file(GLOB SDK2_SOURCES CONFIGURE_DEPENDS src/sdk2/*.c)
add_library(melee_sdk2 STATIC ${SDK2_SOURCES})
target_include_directories(melee_sdk2 PUBLIC
    ${MELEE_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk2
)
target_link_libraries(melee_sdk2 PUBLIC melee_hal)
if(MELEE_PLATFORM_LINUX)
    target_link_libraries(melee_sdk2 PUBLIC m)
endif()

# ---------------------------------------------------------------- GUI
set(GUI_SOURCES
    src/gui/settings.c
    src/gui/gui_console.c
)
if(MELEE_HAVE_IMGUI)
    list(APPEND GUI_SOURCES src/gui/gui_imgui.cpp)
    set_source_files_properties(src/gui/gui_imgui.cpp PROPERTIES LANGUAGE CXX)
else()
    list(APPEND GUI_SOURCES src/gui/gui_nogui.c)
endif()
add_library(melee_gui STATIC ${GUI_SOURCES})
target_include_directories(melee_gui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
    ${MELEE_INCLUDES}
)
if(MELEE_HAVE_IMGUI)
    target_link_libraries(melee_gui PUBLIC imgui glfw)
    target_compile_definitions(melee_gui PUBLIC MELEE_HAVE_IMGUI=1)
endif()
if(MELEE_PLATFORM_WINDOWS)
    target_link_libraries(melee_gui PUBLIC shell32) # SHGetFolderPathA
endif()

# Font blobs (.inc) only exist after extracting the retail DOL; generate
# blank fallbacks into the build dir (real dumps in src/ win by order).
# NOTE: keep this BEFORE melee_game so the fallback dir is in its includes.
set(MELEE_GEN_DIR ${CMAKE_BINARY_DIR}/pc_gen)
execute_process(
    COMMAND ${CMAKE_COMMAND} -DOUT_DIR=${MELEE_GEN_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools_pc/gen_font_inc.cmake
)
list(APPEND MELEE_INCLUDES ${MELEE_GEN_DIR})

# ---------------------------------------------------------------- game
if(MELEE_BUILD_GAME)
    file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
        src/melee/*.c
        src/sysdolphin/baselib/*.c
    )
    # PC replacement (see src/pc): archive.c does endian-aware parsing.
    list(REMOVE_ITEM GAME_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/sysdolphin/baselib/archive.c
    )
    # MSL float tables (NaN/Inf constants used by lbtrigf/gm_1832).
    list(APPEND GAME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/MSL/float.c)
    add_library(melee_game STATIC ${GAME_SOURCES})
    # The decomp targeted MWERKS (stdint types ambient); force-include
    # <stdint.h> in every game TU instead of patching dozens of headers.
    if(MSVC)
        target_compile_options(melee_game PRIVATE /FI"stdint.h")
    else()
        # MWERKS accepts implicit function-pointer conversions (e.g. MWERKS
        # 4-byte bool vs GCC _Bool); GCC 14 promotes them to hard errors.
        # bool/int interop is ABI-safe on x86 for 0/1 values, so demote.
        target_compile_options(melee_game PRIVATE
            -include stdint.h
            -Wno-error=incompatible-pointer-types
        )
    endif()
    target_include_directories(melee_game PUBLIC ${MELEE_INCLUDES})
    target_compile_definitions(melee_game PUBLIC
        MELEE_PC_PORT=1
        VERSION_GALE01=1
        BUILD_VERSION=0
    )
    # gm/gmmain.c defines main(); rename it, pc_main provides the real one.
    set_source_files_properties(
        src/melee/gm/gmmain.c PROPERTIES COMPILE_DEFINITIONS main=melee_main
    )
    target_link_libraries(melee_game PUBLIC melee_sdk2 melee_hal)
endif()

# ---------------------------------------------------------------- pc platform
add_library(melee_pc_platform STATIC
    src/pc/pc_endian.c
    src/pc/pc_archive.c
)
target_include_directories(melee_pc_platform PUBLIC ${MELEE_INCLUDES})
target_link_libraries(melee_pc_platform PUBLIC
    melee_gui melee_sdk2 melee_hal
)
# NOTE: melee_game is intentionally NOT linked here. Game loaders call
# pc_rb32/pc_archive (game -> platform), so the exe links game BEFORE
# platform; GNU ld resolves left-to-right and a platform -> game edge
# would hide platform objects from game references.

# ---------------------------------------------------------------- exe
add_executable(melee_pc src/pc/pc_main.c)
target_include_directories(melee_pc PRIVATE ${MELEE_INCLUDES})
if(MELEE_HAVE_IMGUI)
    # gui links the C++ ImGui lib: link the exe as C++ for libstdc++.
    set_target_properties(melee_pc PROPERTIES LINKER_LANGUAGE CXX)
endif()
# Game first: GNU ld pulls platform objects (pc_endian/pc_archive)
# to satisfy game references.
if(MELEE_BUILD_GAME)
    target_link_libraries(melee_pc PRIVATE melee_game)
endif()
target_link_libraries(melee_pc PRIVATE melee_pc_platform)

if(MELEE_PLATFORM_WINDOWS)
    target_link_libraries(melee_pc PRIVATE shell32)
endif()

# ---------------------------------------------------------------- tools
# Host-side helpers (dependency-free C99, build with the same flags).
add_executable(melee_romextract tools_pc/romextract.c)

install(TARGETS melee_pc melee_romextract RUNTIME DESTINATION bin)

message(STATUS "[melee] platform : ${CMAKE_SYSTEM_NAME}")
message(STATUS "[melee] build type: ${CMAKE_BUILD_TYPE}")
# NOTE: CMAKE_SIZEOF_VOID_P is probed before -m32 is appended, so on a
# 64-bit Linux host it still reads 8 here; the actual game objects are
# compiled with -m32 (4-byte pointers, verified by the build itself).
message(STATUS "[melee] host void*: ${CMAKE_SIZEOF_VOID_P} bytes "
    "(target is always 32-bit x86)")
message(STATUS "[melee] ImGui GUI : ${MELEE_HAVE_IMGUI}")
message(STATUS "[melee] game lib  : ${MELEE_BUILD_GAME}")
