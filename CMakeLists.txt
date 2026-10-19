cmake_minimum_required(VERSION 3.16)
project(EdgeGuard VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

option(EDGEGUARD_BUILD_TESTS "Build EdgeGuard unit tests" ON)

find_package(Threads REQUIRED)

add_library(edgeguard_core STATIC
    src/common/log.cpp
    src/common/util.cpp
    src/common/config.cpp
    src/common/json.cpp
    src/common/socket.cpp
    src/protocol/modbus.cpp
    src/protocol/simulator.cpp
    src/core/metrics.cpp
    src/core/sample_bus.cpp
    src/core/event_hub.cpp
    src/core/acquisition.cpp
    src/core/rules/lexer.cpp
    src/core/rules/parser.cpp
    src/core/rules/compiler.cpp
    src/core/rules/rule_engine.cpp
    src/core/storage/series_store.cpp
    src/core/storage/wal.cpp
    src/core/http/http_server.cpp
    src/app/application.cpp
)

target_include_directories(edgeguard_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(edgeguard_core PUBLIC
    EDGEGUARD_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(edgeguard_core PUBLIC Threads::Threads)

if (WIN32)
    target_compile_definitions(edgeguard_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
    target_link_libraries(edgeguard_core PUBLIC ws2_32)
endif ()

if (MSVC)
    target_compile_options(edgeguard_core PRIVATE /W4 /permissive- /utf-8)
else ()
    target_compile_options(edgeguard_core PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
    )
endif ()

add_executable(edgeguard src/main.cpp)
target_link_libraries(edgeguard PRIVATE edgeguard_core)
if (MSVC)
    target_compile_options(edgeguard PRIVATE /W4 /permissive- /utf-8)
else ()
    target_compile_options(edgeguard PRIVATE -Wall -Wextra -Wpedantic)
endif ()

if (EDGEGUARD_BUILD_TESTS)
    enable_testing()
    add_executable(edgeguard_tests
        tests/test_main.cpp
        tests/test_common.cpp
        tests/test_spsc.cpp
        tests/test_modbus.cpp
        tests/test_rules.cpp
        tests/test_storage.cpp
    )
    target_link_libraries(edgeguard_tests PRIVATE edgeguard_core)
    if (MSVC)
        target_compile_options(edgeguard_tests PRIVATE /W4 /permissive- /utf-8)
    else ()
        target_compile_options(edgeguard_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
    add_test(NAME edgeguard_tests COMMAND edgeguard_tests)
endif ()
