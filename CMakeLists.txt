cmake_minimum_required(VERSION 3.10)
project(lanft C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_GUI "Build with SDL2 GUI (requires SDL2)" ON)

# Dependencies required by both modes
find_package(PkgConfig REQUIRED)

if(WIN32)
    # Windows: use vcpkg or manual paths
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    endif()

    # Workaround: MinGW assembler fails when TEMP path contains non-ASCII
    # characters (e.g., Chinese username). Use -pipe to avoid temp files,
    # and set TMPDIR to an ASCII-only directory for any remaining temp usage.
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
find_package(Threads REQUIRED)

# Base sources (always compiled)
set(SOURCES
    src/network.c
    src/transfer.c
    src/cli.c
    src/config.c
    src/log.c
    src/discovery.c
    src/tomlc17/tomlc17.c
)

if(BUILD_GUI)
    message(STATUS "Building with SDL2 GUI")
    find_package(SDL2 REQUIRED)
    # SDL2_ttf — optional, for system font support (Chinese, etc.)
    pkg_check_modules(SDL2_TTF SDL2_ttf)
    if(SDL2_TTF_FOUND)
        message(STATUS "  SDL2_ttf found — system font enabled")
        add_definitions(-DHAVE_SDL2_TTF)
    else()
        message(STATUS "  SDL2_ttf not found — falling back to built-in 8x16 bitmap font (ASCII only)")
    endif()
    # Work around CMake/Win32 appending .lib to SDL2main's shell32 dependency
    if(WIN32 AND NOT MSVC)
        set_property(TARGET SDL2::SDL2main PROPERTY INTERFACE_LINK_LIBRARIES
            "-lshell32" "$<\$<STREQUAL:\$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-Wl,--undefined=WinMain>")
    endif()
    list(APPEND SOURCES src/main.c src/scanner.c src/ui.c)
    add_definitions(-DBUILD_GUI)
else()
    message(STATUS "Building CLI-only (no SDL2)")
    list(APPEND SOURCES src/main_cli.c)
endif()

add_executable(lanft ${SOURCES})
# Windows: force console subsystem so CLI output is visible.
# Without this, SDL2 pulls in -mwindows which hides stdout/stderr.
if(WIN32 AND BUILD_GUI)
    set_target_properties(lanft PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_options(lanft PRIVATE -mconsole)
endif()

target_include_directories(lanft PRIVATE
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${LIBARCHIVE_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

if(BUILD_GUI)
    target_include_directories(lanft PRIVATE ${SDL2_INCLUDE_DIRS})
    if(SDL2_TTF_FOUND)
        target_include_directories(lanft PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
    endif()
endif()

target_link_libraries(lanft
    ${LIBWEBSOCKETS_LDFLAGS}
    ${LIBARCHIVE_LDFLAGS}
    Threads::Threads
)

if(BUILD_GUI)
    target_link_libraries(lanft ${SDL2_LIBRARIES})
    if(SDL2_TTF_FOUND)
        target_link_libraries(lanft ${SDL2_TTF_LDFLAGS})
    endif()
endif()

if(WIN32)
    target_link_libraries(lanft -lws2_32 -liphlpapi -lshell32 -lcomdlg32 -lole32)
    # MSYS2: avoid CRT mismatch by preferring static libgcc
    if(MINGW)
        target_link_options(lanft PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()
