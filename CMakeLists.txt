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

# ── osodio library ────────────────────────────────────────────────────────────
add_library(osodio STATIC
    src/app.cpp
    src/router.cpp
    src/core/event_loop.cpp
    src/core/tcp_server.cpp
    src/http/http_parser.cpp
    src/http/http_connection.cpp
)

target_include_directories(osodio
    PUBLIC  include third_party
    PRIVATE src
)

target_link_libraries(osodio PUBLIC llhttp simdjson inja)

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
