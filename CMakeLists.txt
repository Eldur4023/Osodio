cmake_minimum_required(VERSION 3.20)
project(osodio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_executable(example main.cpp)
target_link_libraries(example PRIVATE osodio)

# Warnings
target_compile_options(osodio PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(example PRIVATE -Wall -Wextra)

# make distclean → elimina todos los archivos generados por cmake/make
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
