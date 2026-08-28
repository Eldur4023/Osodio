cmake_minimum_required(VERSION 3.20)
project(osodio CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Osodio requiere Linux: usa epoll, sendfile(2) y SO_REUSEPORT.")
endif()

option(OSODIO_IO_URING "Backend io_uring en lugar de epoll (Linux 5.1+)" OFF)
option(OSODIO_BROTLI   "Compresion Brotli ademas de gzip"                OFF)
option(OSODIO_EXAMPLES "Compilar los ejemplos de examples/"              ON)

# Modulos de persistencia.  Cada uno se compila solo si su cliente esta
# presente: `import sqlite` en un binario sin el da un error claro al compilar
# el .odio, no un fallo raro en produccion.
option(OSODIO_SQLITE   "Modulo sqlite"                                   ON)
option(OSODIO_POSTGRES "Modulo postgres (libpq)"                         ON)
option(OSODIO_MYSQL    "Modulo mysql (libmysqlclient)"                   ON)

# ── Dependencias del sistema ──────────────────────────────────────────────────
# Osodio 2.0 no enlaza OpenSSL ni nghttp2: TLS y HTTP/2 son cosa del reverse
# proxy.  zlib es la unica dependencia obligatoria.
find_package(ZLIB REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── Jinja2Cpp ─────────────────────────────────────────────────────────────────
# Motor de plantillas.  Se usa Jinja2Cpp y no inja porque implementa Jinja2 de
# verdad: es la sintaxis de Flask, y la que generan por defecto tanto la gente
# como los LLM.  inja soporta un subconjunto y diverge en detalles.
#
# No esta empaquetado en apt ni es cabecera unica, asi que se trae con
# FetchContent y un tag fijado.  Requiere red en el PRIMER configure; despues
# queda cacheado en build/_deps.
include(FetchContent)
FetchContent_Declare(Jinja2Cpp
    GIT_REPOSITORY https://github.com/jinja2cpp/Jinja2Cpp.git
    GIT_TAG        1.3.2
    GIT_SHALLOW    TRUE
)
set(JINJA2CPP_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(JINJA2CPP_BUILD_SHARED     OFF CACHE BOOL "" FORCE)
set(JINJA2CPP_INSTALL          OFF CACHE BOOL "" FORCE)
set(JINJA2CPP_STRICT_WARNINGS  OFF CACHE BOOL "" FORCE)

# rapidjson (dependencia transitiva de Jinja2Cpp) declara un
# cmake_minimum_required anterior a 3.5, que CMake >= 4 ya rechaza.  Esta es la
# via de compatibilidad documentada; solo afecta a las dependencias traídas
# aqui, no al proyecto.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

FetchContent_MakeAvailable(Jinja2Cpp)

# ── third_party (vendorizado, sin red durante cmake) ──────────────────────────
add_library(osodio_llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(osodio_llhttp PUBLIC third_party/llhttp/include)
set_target_properties(osodio_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(osodio_simdjson STATIC third_party/simdjson.cpp)
target_include_directories(osodio_simdjson PUBLIC third_party)
set_target_properties(osodio_simdjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Biblioteca principal ──────────────────────────────────────────────────────
set(OSODIO_SOURCES
    src/app.cpp
    src/router.cpp
    src/core/event_loop.cpp
    src/core/tcp_server.cpp
    src/http/http_parser.cpp
    src/http/http_connection.cpp
)

if(OSODIO_IO_URING)
    list(APPEND OSODIO_SOURCES src/core/io_uring_loop.cpp)
endif()

add_library(osodio STATIC ${OSODIO_SOURCES})

target_include_directories(osodio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(osodio PUBLIC
    osodio_llhttp
    osodio_simdjson
    ZLIB::ZLIB
    Threads::Threads
    jinja2cpp
)

if(OSODIO_IO_URING)
    find_library(URING_LIB uring REQUIRED)
    target_compile_definitions(osodio PUBLIC OSODIO_IO_URING)
    target_link_libraries(osodio PUBLIC ${URING_LIB})
endif()

if(OSODIO_BROTLI)
    find_library(BROTLI_ENC brotlienc REQUIRED)
    target_compile_definitions(osodio PUBLIC OSODIO_HAS_BROTLI)
    target_link_libraries(osodio PUBLIC ${BROTLI_ENC})
endif()

target_compile_options(osodio PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── Odio: frontend del lenguaje ───────────────────────────────────────────────
add_library(odio STATIC
    src/odio/token.cpp
    src/odio/diagnostic.cpp
    src/odio/lexer.cpp
    src/odio/parser.cpp
    src/odio/crypto.cpp
    src/odio/db.cpp
    src/odio/value.cpp
    src/odio/natives.cpp
    src/odio/emitter.cpp
    src/odio/vm.cpp
    src/odio/project.cpp
)
target_include_directories(odio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(odio PUBLIC osodio)

if(OSODIO_SQLITE)
    find_path(SQLITE3_INC sqlite3.h)
    find_library(SQLITE3_LIB sqlite3)
    if(SQLITE3_INC AND SQLITE3_LIB)
        target_sources(odio PRIVATE src/odio/db_sqlite.cpp)
        target_compile_definitions(odio PRIVATE OSODIO_SQLITE)
        target_include_directories(odio PRIVATE ${SQLITE3_INC})
        target_link_libraries(odio PRIVATE ${SQLITE3_LIB})
        message(STATUS "modulo sqlite: activado")
    else()
        message(STATUS "modulo sqlite: OMITIDO (falta libsqlite3-dev)")
    endif()
endif()

if(OSODIO_POSTGRES)
    find_path(PQ_INC libpq-fe.h PATH_SUFFIXES postgresql)
    find_library(PQ_LIB pq)
    if(PQ_INC AND PQ_LIB)
        target_sources(odio PRIVATE src/odio/db_postgres.cpp)
        target_compile_definitions(odio PRIVATE OSODIO_POSTGRES)
        target_include_directories(odio PRIVATE ${PQ_INC})
        target_link_libraries(odio PRIVATE ${PQ_LIB})
        message(STATUS "modulo postgres: activado")
    else()
        message(STATUS "modulo postgres: OMITIDO (falta libpq-dev)")
    endif()
endif()

if(OSODIO_MYSQL)
    find_path(MYSQL_INC mysql.h PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIB NAMES mysqlclient mariadb)
    if(MYSQL_INC AND MYSQL_LIB)
        target_sources(odio PRIVATE src/odio/db_mysql.cpp)
        target_compile_definitions(odio PRIVATE OSODIO_MYSQL)
        target_include_directories(odio PRIVATE ${MYSQL_INC})
        target_link_libraries(odio PRIVATE ${MYSQL_LIB})
        message(STATUS "modulo mysql: activado")
    else()
        message(STATUS "modulo mysql: OMITIDO (falta libmysqlclient-dev)")
    endif()
endif()
target_compile_options(odio PRIVATE -Wall -Wextra)

# El binario de Osodio 2.0.
add_executable(osodio-bin src/odio/main.cpp)
set_target_properties(osodio-bin PROPERTIES OUTPUT_NAME osodio)
target_link_libraries(osodio-bin PRIVATE odio)

# Herramientas de desarrollo del frontend.
add_executable(dump_tokens src/odio/tools/dump_tokens.cpp)
target_link_libraries(dump_tokens PRIVATE odio)

add_executable(crypto_check src/odio/tools/crypto_check.cpp)
target_link_libraries(crypto_check PRIVATE odio)

# ── Ejemplos ──────────────────────────────────────────────────────────────────
if(OSODIO_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    file(GLOB OSODIO_EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(src ${OSODIO_EXAMPLE_SRCS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE osodio)
    endforeach()
endif()
