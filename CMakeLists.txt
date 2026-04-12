cmake_minimum_required(VERSION 3.20)
project(osodio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Vendored dependencies (third_party/) — 8 files, no network, no subdirs ───
#
#   third_party/
#     simdjson.h / simdjson.cpp   — amalgamated single-file (v3.10.0)
#     inja.hpp                    — single-include header-only (v3.4.0)
#     nlohmann/json.hpp           — single-include header-only (v3.11.3)
#     llhttp/include/llhttp.h     — Node.js HTTP parser header (v9.2.1)
#     llhttp/src/{api,http,llhttp}.c

# llhttp — compiled directly, no CMakeLists needed
add_library(llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(llhttp PUBLIC third_party/llhttp/include)

# simdjson — amalgamated, compiled directly
add_library(simdjson STATIC third_party/simdjson.cpp)
target_include_directories(simdjson PUBLIC third_party)
# simdjson amalgamation uses #include "simdjson.h" internally
target_compile_options(simdjson PRIVATE -O2)

# inja — header-only interface target
add_library(inja INTERFACE)
target_include_directories(inja INTERFACE third_party)

# ── io_uring optional backend ─────────────────────────────────────────────────
#
# Enable with:  cmake -DUSE_IO_URING=ON ..
# Requires Linux kernel headers with io_uring support (linux/io_uring.h).
# Falls back to epoll if the header is not found or USE_IO_URING is OFF.

option(USE_IO_URING "Use io_uring event loop backend instead of epoll" OFF)

if(USE_IO_URING)
    find_path(IO_URING_INCLUDE_DIR linux/io_uring.h
              PATHS /usr/include /usr/local/include)
    if(IO_URING_INCLUDE_DIR)
        set(HAVE_IO_URING TRUE)
        message(STATUS "io_uring backend enabled (kernel headers found)")
    else()
        message(WARNING "USE_IO_URING=ON but linux/io_uring.h not found — falling back to epoll")
        set(HAVE_IO_URING FALSE)
    endif()
else()
    set(HAVE_IO_URING FALSE)
endif()

# ── osodio library ────────────────────────────────────────────────────────────
set(OSODIO_SOURCES
    src/app.cpp
    src/router.cpp
    src/core/event_loop.cpp
    src/core/tcp_server.cpp
    src/http/http_parser.cpp
    src/http/http_connection.cpp
    src/http/http2_connection.cpp
)

if(HAVE_IO_URING)
    list(APPEND OSODIO_SOURCES src/core/io_uring_loop.cpp)
endif()

add_library(osodio STATIC ${OSODIO_SOURCES})

if(HAVE_IO_URING)
    target_compile_definitions(osodio PUBLIC OSODIO_IO_URING)
endif()

target_include_directories(osodio
    PUBLIC  include third_party
    PRIVATE src
)

find_package(ZLIB    REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(BROTLIENC_LIB brotlienc  REQUIRED)
find_library(NGHTTP2_LIB   nghttp2    REQUIRED)
target_link_libraries(osodio PUBLIC
    llhttp simdjson inja
    ZLIB::ZLIB ${BROTLIENC_LIB}
    OpenSSL::SSL OpenSSL::Crypto
    ${NGHTTP2_LIB})

# ── example binary ───────────────────────────────────────────────────────────
add_executable(example main.cpp)
target_link_libraries(example PRIVATE osodio)

# Warnings
target_compile_options(osodio  PRIVATE -Wall -Wextra -Wpedantic -g)
target_compile_options(example PRIVATE -Wall -Wextra -g)

# make distclean
add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
    COMMAND ${CMAKE_COMMAND} -E remove -f
        ${CMAKE_BINARY_DIR}/CMakeCache.txt
        ${CMAKE_BINARY_DIR}/cmake_install.cmake
        ${CMAKE_BINARY_DIR}/Makefile
        ${CMAKE_BINARY_DIR}/compile_commands.json
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        ${CMAKE_BINARY_DIR}/CMakeFiles
    COMMENT "Limpiando archivos de build y cmake..."
    VERBATIM
)
