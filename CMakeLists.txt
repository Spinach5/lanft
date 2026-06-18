cmake_minimum_required(VERSION 3.10)
project(lanft C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_GUI "Build with SDL3 GUI (requires SDL3)" ON)

# ── 检测 Conan 环境 ──────────────────────────────────
if(EXISTS "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
    message(STATUS "Conan toolchain detected — using Conan-provided packages")
    set(CONAN_MANAGED ON)
else()
    set(CONAN_MANAGED OFF)
endif()

# ── 查找依赖（Conan 提供路径，否则走 pkg-config） ──
find_package(PkgConfig QUIET)

if(CONAN_MANAGED)
    find_package(Libwebsockets REQUIRED)
    find_package(LibArchive REQUIRED)
    if(BUILD_GUI)
        find_package(SDL3 REQUIRED)
        find_package(SDL3_ttf REQUIRED)
    endif()
else()
    if(WIN32)
        if(DEFINED ENV{VCPKG_ROOT})
            set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain")
        endif()
        if(MINGW)
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/temp")
            set(ENV{TMPDIR} "${CMAKE_BINARY_DIR}/temp")
            add_compile_options(-pipe)
        endif()
        pkg_check_modules(LIBWEBSOCKETS libwebsockets)
        pkg_check_modules(LIBARCHIVE libarchive)
    else()
        pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)
        pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
    endif()
    if(BUILD_GUI)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    endif()
endif()

find_package(Threads REQUIRED)

# ── 源文件 ───────────────────────────────────────────
set(SOURCES
    src/network.c
    src/transfer.c
    src/cli.c
    src/config.c
    src/log.c
    src/discovery.c
    src/tomlc17/tomlc17.c
)

if(MSVC)
    list(APPEND SOURCES src/compat_getopt.c)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/experimental:c11atomics)
endif()

if(BUILD_GUI)
    message(STATUS "Building with SDL3 GUI")

    if(CONAN_MANAGED)
        add_definitions(-DHAVE_SDL3_TTF)
        message(STATUS "  SDL3_ttf via Conan — system font enabled")
    else()
        pkg_check_modules(SDL3_TTF SDL3_ttf)
        if(SDL3_TTF_FOUND)
            message(STATUS "  SDL3_ttf found — system font enabled")
            add_definitions(-DHAVE_SDL3_TTF)
        else()
            message(STATUS "  SDL3_ttf not found — falling back to built-in 8x16 bitmap font (ASCII only)")
        endif()
    endif()

    list(APPEND SOURCES src/main.c src/scanner.c src/ui.c)
    add_definitions(-DBUILD_GUI)
else()
    message(STATUS "Building CLI-only (no SDL3)")
    list(APPEND SOURCES src/main_cli.c)
endif()

# ── 可执行文件 ──────────────────────────────────────
add_executable(lanft ${SOURCES})

if(WIN32 AND BUILD_GUI)
    set_target_properties(lanft PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(lanft PRIVATE -mconsole)
endif()

# ── 包含目录 ────────────────────────────────────────
target_include_directories(lanft PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT CONAN_MANAGED)
    target_include_directories(lanft PRIVATE
        ${LIBWEBSOCKETS_INCLUDE_DIRS}
        ${LIBARCHIVE_INCLUDE_DIRS}
    )
endif()
if(BUILD_GUI AND NOT CONAN_MANAGED)
    target_include_directories(lanft PRIVATE ${SDL3_INCLUDE_DIRS})
    if(SDL3_TTF_FOUND)
        target_include_directories(lanft PRIVATE ${SDL3_TTF_INCLUDE_DIRS})
    endif()
endif()

# ── 链接库 ──────────────────────────────────────────
if(CONAN_MANAGED)
    target_link_libraries(lanft
        websockets
        LibArchive::LibArchive
        Threads::Threads
    )
    if(BUILD_GUI)
        target_link_libraries(lanft sdl::sdl SDL3_ttf::SDL3_ttf-static)
    endif()
else()
    target_link_libraries(lanft
        ${LIBWEBSOCKETS_LDFLAGS}
        ${LIBARCHIVE_LDFLAGS}
        Threads::Threads
    )
    if(BUILD_GUI)
        target_link_libraries(lanft ${SDL3_LIBRARIES} PkgConfig::SDL3)
        if(SDL3_TTF_FOUND)
            target_link_libraries(lanft ${SDL3_TTF_LDFLAGS})
        endif()
    endif()
endif()

if(WIN32)
    target_link_libraries(lanft ws2_32 iphlpapi shell32 comdlg32 ole32)
    if(MINGW)
        target_link_options(lanft PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()
