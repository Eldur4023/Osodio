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
option(OSODIO_EXAMPLES "Compilar los ejemplos de examples/"              ON)

# Modulos de persistencia.  Cada uno se compila solo si su cliente esta
# presente: `import sqlite` en un binario sin el da un error claro al compilar
# el .odio, no un fallo raro en produccion.
option(OSODIO_SQLITE   "Modulo sqlite"                                   ON)
option(OSODIO_POSTGRES "Modulo postgres (libpq)"                         ON)
option(OSODIO_MYSQL    "Modulo mysql (libmysqlclient)"                   ON)

# ── Dependencias del sistema ──────────────────────────────────────────────────
# Osodio 2.0 no enlaza OpenSSL, nghttp2 ni zlib: TLS, HTTP/2 y compresion son
# cosa del reverse proxy.  Solo hacen falta hilos.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── jemalloc ──────────────────────────────────────────────────────────────────
# Con varios event loops y un pool de base de datos, el malloc de glibc se
# convierte en el cuello de botella: sus arenas se serializan y el mutex se come
# el 20% del tiempo.  Medido sobre respuestas JSON grandes, 8 cores:
#
#   /articulos?size=100 (57 KB)   18.782 -> 29.802 req/s   +59%
#   /articulos?page=3   (11 KB)   49.051 -> 65.307 req/s   +33%
#
# Es opcional: si no esta, se compila igual y se sigue usando el de glibc.
option(OSODIO_JEMALLOC "Enlazar jemalloc si esta disponible" ON)
set(OSODIO_ALLOC "")
if(OSODIO_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES jemalloc)
    if(JEMALLOC_LIB)
        set(OSODIO_ALLOC ${JEMALLOC_LIB})
        message(STATUS "asignador: jemalloc (${JEMALLOC_LIB})")
    else()
        message(STATUS "asignador: glibc — instala libjemalloc-dev para el de verdad")
    endif()
endif()

# Sin dependencias que traer de la red: llhttp esta vendorizado y todo lo demas
# —JSON, plantillas, criptografia— es de casa.
#
# ── third_party (vendorizado, sin red durante cmake) ──────────────────────────
add_library(osodio_llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(osodio_llhttp PUBLIC third_party/llhttp/include)
set_target_properties(osodio_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
    Threads::Threads
    ${OSODIO_ALLOC}
)

if(OSODIO_IO_URING)
    find_library(URING_LIB uring REQUIRED)
    target_compile_definitions(osodio PUBLIC OSODIO_IO_URING)
    target_link_libraries(osodio PUBLIC ${URING_LIB})
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
    src/odio/json_parse.cpp
    src/odio/plantilla.cpp
    src/odio/natives.cpp
    src/odio/emitter.cpp
    src/odio/vm.cpp
    src/odio/autotest.cpp
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

# ── Pruebas ───────────────────────────────────────────────────────────────────
# La suite es un script que ejerce el binario por el socket, tal y como se usa.
# No enlaza nada del proyecto, asi que prueba lo que se despliega y no una
# version instrumentada de ello.
enable_testing()

# Las plantillas se prueban aparte y en proceso: compilar y renderizar no pasa
# por el socket, asi que el guion de regresion no llega ahi.
add_executable(prueba_plantillas tests/plantillas.cpp)
target_link_libraries(prueba_plantillas PRIVATE odio)
add_test(NAME plantillas COMMAND prueba_plantillas)

# El traductor de marcadores de postgres es una funcion pura: se prueba sin
# servidor incluyendo el .cpp del driver, para probar EL codigo y no una copia.
if(OSODIO_POSTGRES AND PQ_INC AND PQ_LIB)
    add_executable(prueba_marcadores tests/marcadores.cpp)
    target_include_directories(prueba_marcadores PRIVATE ${PQ_INC})
    target_link_libraries(prueba_marcadores PRIVATE odio ${PQ_LIB})
    add_test(NAME marcadores COMMAND prueba_marcadores)
endif()

add_test(NAME regresion
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:osodio-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(regresion PROPERTIES TIMEOUT 300)

# El modulo de mysql necesita un servidor, asi que su suite se salta sola —codigo
# 77— cuando no lo hay, en vez de poner en rojo una maquina donde no aplica.
# Las instrucciones para montar el entorno estan en la cabecera del guion.
add_test(NAME mysql
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mysql.sh $<TARGET_FILE:osodio-bin>
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mysql PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# ── Ejemplos ──────────────────────────────────────────────────────────────────
if(OSODIO_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    file(GLOB OSODIO_EXAMPLE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
    foreach(src ${OSODIO_EXAMPLE_SRCS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE osodio)
    endforeach()
endif()
