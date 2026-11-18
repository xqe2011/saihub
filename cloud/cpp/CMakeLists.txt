cmake_minimum_required(VERSION 3.20)
project(saihub_cloud_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unordered_dense CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
find_library(SQLITE3_STATIC NAMES libsqlite3.a
    PATHS
        /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
        /usr/lib/x86_64-linux-gnu
        /usr/lib/aarch64-linux-gnu
        /usr/lib64
        /usr/local/lib
    NO_DEFAULT_PATH)
if(NOT SQLITE3_STATIC)
    set(SQLITE3_STATIC SQLite::SQLite3)
endif()

add_library(saihub_core STATIC
    src/config.cpp
    src/crypto.cpp
    src/protocol.cpp
    src/store.cpp
    src/html.cpp
    src/device_manager.cpp
    src/server.cpp
)

target_include_directories(saihub_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" ${SQLite3_INCLUDE_DIRS})
target_compile_definitions(saihub_core PUBLIC LIBUS_NO_SSL UWS_NO_ZLIB UWS_HTTPRESPONSE_NO_WRITEMARK)
target_compile_options(saihub_core PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(saihub_core PUBLIC
    unofficial::uwebsockets::uwebsockets
    yyjson::yyjson
    OpenSSL::Crypto
    unordered_dense::unordered_dense
    ZLIB::ZLIB
    ${SQLITE3_STATIC}
)

add_executable(saihub-cloud src/main.cpp)
target_link_libraries(saihub-cloud PRIVATE saihub_core)
if(UNIX AND NOT APPLE)
    target_link_options(saihub-cloud PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(saihub-tests tests/unit_tests.cpp)
target_link_libraries(saihub-tests PRIVATE saihub_core doctest::doctest)

add_executable(bench-load bench/bench_load.cpp)
target_link_libraries(bench-load PRIVATE saihub_core)

enable_testing()
add_test(NAME unit_tests COMMAND saihub-tests)
