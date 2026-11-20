cmake_minimum_required(VERSION 3.28)
project(pwcap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# `import std` is not used (AGENTS.md); skip module scanning under Ninja.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 21)
    message(FATAL_ERROR "pwcap requires clang 21 or newer, found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

option(PWCAP_BUILD_TESTS "Build the tests" OFF)

set(PWCAP_SANITIZER "" CACHE STRING "Sanitizer set: empty, address,undefined or thread")
set_property(CACHE PWCAP_SANITIZER PROPERTY STRINGS "" "address,undefined" "thread")
if(NOT PWCAP_SANITIZER STREQUAL "" AND NOT PWCAP_SANITIZER STREQUAL "address,undefined"
   AND NOT PWCAP_SANITIZER STREQUAL "thread")
    message(FATAL_ERROR "PWCAP_SANITIZER must be empty, 'address,undefined' or 'thread', got '${PWCAP_SANITIZER}'")
endif()

add_library(pwcap_warnings INTERFACE)
target_compile_options(pwcap_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
    -Wnon-virtual-dtor -Wimplicit-fallthrough -Wdouble-promotion -Werror)

add_library(pwcap_sanitizers INTERFACE)
if(NOT PWCAP_SANITIZER STREQUAL "")
    target_compile_options(pwcap_sanitizers INTERFACE
        -fsanitize=${PWCAP_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(pwcap_sanitizers INTERFACE -fsanitize=${PWCAP_SANITIZER})
    if(PWCAP_SANITIZER MATCHES "undefined")
        # Without this UBSan prints a report and the test still passes.
        target_compile_options(pwcap_sanitizers INTERFACE -fno-sanitize-recover=all)
        target_link_options(pwcap_sanitizers INTERFACE -fno-sanitize-recover=all)
    endif()
endif()

# Globs are deliberate: later plans add files here without editing this CMake file.
file(GLOB PWCAP_CORE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/core/*.cpp)
add_library(pwcap_core STATIC ${PWCAP_CORE_SOURCES})
target_include_directories(pwcap_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pwcap_core PUBLIC pwcap_sanitizers PRIVATE pwcap_warnings)

if(PWCAP_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    file(GLOB PWCAP_CORE_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/core/*_test.cpp)
    foreach(test_source IN LISTS PWCAP_CORE_TEST_SOURCES)
        get_filename_component(test_stem ${test_source} NAME_WE)
        set(test_target core_${test_stem})
        add_executable(${test_target} ${test_source})
        target_link_libraries(${test_target} PRIVATE pwcap_core pwcap_warnings GTest::gtest_main)
        gtest_discover_tests(${test_target}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS core TIMEOUT 120)
    endforeach()

    # Hygiene tests come from plan 02-02; register them only once both files exist.
    file(GLOB PWCAP_HYGIENE_SCRIPT CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/cmake/core_hygiene.cmake)
    file(GLOB PWCAP_HYGIENE_FIXTURE CONFIGURE_DEPENDS
         ${PROJECT_SOURCE_DIR}/tests/core/hygiene_fixtures/violations.txt)
    if(PWCAP_HYGIENE_SCRIPT AND PWCAP_HYGIENE_FIXTURE)
        add_test(NAME core_hygiene
                 COMMAND ${CMAKE_COMMAND} -DPWCAP_ROOT=${PROJECT_SOURCE_DIR}
                         -P ${PROJECT_SOURCE_DIR}/cmake/core_hygiene.cmake)
        add_test(NAME core_hygiene_selftest
                 COMMAND ${CMAKE_COMMAND}
                         -DPWCAP_SELFTEST_FILE=${PROJECT_SOURCE_DIR}/tests/core/hygiene_fixtures/violations.txt
                         -P ${PROJECT_SOURCE_DIR}/cmake/core_hygiene.cmake)
        set_tests_properties(core_hygiene core_hygiene_selftest PROPERTIES LABELS core)
    else()
        message(STATUS "core_hygiene: script from plan 02-02 not present; hygiene tests not registered")
    endif()
endif()
