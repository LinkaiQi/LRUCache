cmake_minimum_required(VERSION 3.16)

project(lru_cache
    VERSION 1.0.0
    DESCRIPTION "Header-only thread-safe LRU cache with optional TTL"
    HOMEPAGE_URL "https://github.com/LinkaiQi/LRUCache"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Only build tests, examples and benchmarks when this is the top-level project,
# so add_subdirectory() and FetchContent consumers get just the library.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(LRU_CACHE_TOP_LEVEL ON)
else()
    set(LRU_CACHE_TOP_LEVEL OFF)
endif()

option(LRU_CACHE_BUILD_TESTS "Build the test suites" ${LRU_CACHE_TOP_LEVEL})
option(LRU_CACHE_BUILD_EXAMPLES "Build the example programs" ${LRU_CACHE_TOP_LEVEL})
option(LRU_CACHE_BUILD_BENCHMARKS "Build the benchmark" ${LRU_CACHE_TOP_LEVEL})
option(LRU_CACHE_INSTALL "Generate install rules" ${LRU_CACHE_TOP_LEVEL})
set(LRU_CACHE_SANITIZER "none" CACHE STRING "Sanitizer: none, address, undefined or thread")

# ---- the library ------------------------------------------------------------

find_package(Threads REQUIRED)

add_library(lru_cache INTERFACE)
add_library(lru::lru_cache ALIAS lru_cache)

target_include_directories(lru_cache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(lru_cache INTERFACE cxx_std_17)
target_link_libraries(lru_cache INTERFACE Threads::Threads)

# ---- warnings and sanitizers for our own targets ----------------------------

if(MSVC)
    set(LRU_WARNINGS /W4 /WX /permissive-)
else()
    set(LRU_WARNINGS
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Werror)
endif()

function(lru_add_executable target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE lru::lru_cache)
    target_compile_options(${target} PRIVATE ${LRU_WARNINGS})
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(NOT LRU_CACHE_SANITIZER STREQUAL "none" AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=${LRU_CACHE_SANITIZER} -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=${LRU_CACHE_SANITIZER})
    endif()
endfunction()

# ---- tests ------------------------------------------------------------------

if(LRU_CACHE_BUILD_TESTS)
    enable_testing()
    foreach(name test_lru_cache test_lru_cache_classic test_lru_cache_ttl)
        lru_add_executable(${name} tests/${name}.cpp)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()

# ---- examples ---------------------------------------------------------------

if(LRU_CACHE_BUILD_EXAMPLES)
    lru_add_executable(lru_example_basic examples/basic.cpp)
    lru_add_executable(lru_example_ttl examples/ttl.cpp)
    lru_add_executable(cache_shell examples/cache_shell.cpp)
endif()

# ---- benchmark --------------------------------------------------------------

if(LRU_CACHE_BUILD_BENCHMARKS)
    lru_add_executable(lru_bench benchmarks/bench_lru.cpp)
endif()

# ---- install ----------------------------------------------------------------

if(LRU_CACHE_INSTALL)
    install(TARGETS lru_cache EXPORT lru_cacheTargets)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT lru_cacheTargets
        FILE lru_cacheTargets.cmake
        NAMESPACE lru::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lru_cache)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lru_cacheConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/lru_cacheConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lru_cache)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/lru_cacheConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/lru_cacheConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/lru_cacheConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lru_cache)
endif()
