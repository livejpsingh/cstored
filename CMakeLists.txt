cmake_minimum_required(VERSION 3.20)

project(cstored
    VERSION 0.2.0
    DESCRIPTION "A Production-Grade Multi-Threaded In-Memory Key-Value Store"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Export compile commands for clang-tidy and LSP navigation
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# Options
option(KVS_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KVS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(KVS_BUILD_TESTS "Build the test suite" ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Core library source files
set(KVS_LIB_SOURCES
    src/common.c
    src/slab.c
    src/hash.c
    src/hash_table.c
    src/thread_pool.c
    src/concurrent_ht.c
)

add_library(kvs_core STATIC ${KVS_LIB_SOURCES})

target_include_directories(kvs_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Apply warnings and sanitizers
kvs_set_target_warnings(kvs_core)
kvs_enable_sanitizers(kvs_core)

# Target-specific platform linkages
find_package(Threads REQUIRED)
target_link_libraries(kvs_core PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(kvs_core PRIVATE rt)
endif()

# Tests
if(KVS_BUILD_TESTS)
    enable_testing()

    # Unity testing framework library
    add_library(unity STATIC
        tests/unity/unity.c
    )
    target_include_directories(unity PUBLIC tests/unity)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(unity PRIVATE -w)
    endif()

    macro(kvs_add_unit_test test_name test_source)
        add_executable(${test_name} ${test_source})
        target_link_libraries(${test_name} PRIVATE kvs_core unity)
        target_include_directories(${test_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        )
        kvs_set_target_warnings(${test_name})
        kvs_enable_sanitizers(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endmacro()

    kvs_add_unit_test(test_slab tests/test_slab.c)
    kvs_add_unit_test(test_hash tests/test_hash.c)
    kvs_add_unit_test(test_hash_table tests/test_hash_table.c)
    kvs_add_unit_test(test_thread_pool tests/test_thread_pool.c)
    kvs_add_unit_test(test_concurrent_ht tests/test_concurrent_ht.c)
endif()
