cmake_minimum_required(VERSION 3.10)
project(lanft C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find SDL2
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# Find libwebsockets via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

# Find libarchive
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)

# Find pthreads
find_package(Threads REQUIRED)

set(SOURCES
    src/main.c
    src/network.c
    src/scanner.c
    src/transfer.c
    src/ui.c
    src/cli.c
)

add_executable(lanft ${SOURCES})

target_include_directories(lanft PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${LIBARCHIVE_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(lanft
    ${SDL2_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
    ${LIBARCHIVE_LIBRARIES}
    Threads::Threads
)
