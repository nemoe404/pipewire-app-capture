cmake_minimum_required(VERSION 3.28)
project(pwcap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# `import std` is not used (AGENTS.md); skip module scanning under Ninja.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(GNUInstallDirs)

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

# PipeWire comes in through the imported target so its headers arrive as -isystem; hand-added
# include directories would flood the build with warnings from PipeWire's own headers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(Threads REQUIRED)

# The shell library. Empty until the first src/shell/*.cpp exists; CONFIGURE_DEPENDS re-globs.
file(GLOB PWCAP_SHELL_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/shell/*.cpp)
if(PWCAP_SHELL_SOURCES)
    add_library(pwcap STATIC ${PWCAP_SHELL_SOURCES})
    target_include_directories(pwcap
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(pwcap
        PUBLIC pwcap_sanitizers Threads::Threads
        PRIVATE pwcap_core PkgConfig::PIPEWIRE pwcap_warnings)
endif()

# The CLI. Globs again: later plans add files under src/cli without editing this file. main.cpp is
# the executable; every other .cpp in src/cli is the logic library that the tests link.
if(TARGET pwcap)
    file(GLOB PWCAP_CLI_LIB_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/cli/*.cpp)
    list(FILTER PWCAP_CLI_LIB_SOURCES EXCLUDE REGEX "/main\\.cpp$")
    if(PWCAP_CLI_LIB_SOURCES)
        add_library(pwcap_cli_lib STATIC ${PWCAP_CLI_LIB_SOURCES})
        target_include_directories(pwcap_cli_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(pwcap_cli_lib PUBLIC pwcap PRIVATE pwcap_warnings)
    endif()

    if(EXISTS ${PROJECT_SOURCE_DIR}/src/cli/main.cpp)
        add_executable(pwcap_cli ${PROJECT_SOURCE_DIR}/src/cli/main.cpp)
        # The library already owns the target name pwcap, so the executable is named by OUTPUT_NAME.
        set_target_properties(pwcap_cli PROPERTIES OUTPUT_NAME pwcap)
        target_link_libraries(pwcap_cli PRIVATE pwcap_warnings)
        if(TARGET pwcap_cli_lib)
            target_link_libraries(pwcap_cli PRIVATE pwcap_cli_lib)
        endif()
        install(TARGETS pwcap_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

if(PWCAP_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    # nlohmann_json is the pw-dump oracle for tests/pipewire (plan 04-02); test executables only.
    find_package(nlohmann_json CONFIG REQUIRED)

    if(TARGET pwcap)
        # Silent helper executables for subprocess checks. No gtest: they must print nothing.
        file(GLOB PWCAP_SHELL_PROBE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/shell/*_probe.cpp)
        foreach(probe_source IN LISTS PWCAP_SHELL_PROBE_SOURCES)
            get_filename_component(probe_stem ${probe_source} NAME_WE)
            set(probe_target shell_${probe_stem})
            add_executable(${probe_target} ${probe_source})
            target_link_libraries(${probe_target} PRIVATE pwcap pwcap_warnings)
        endforeach()

        set(PWCAP_PROBE_DEPENDENT_TESTS "")
        # Test targets that run the pwcap executable (cli_* and pipewire_*); see the wiring below.
        set(PWCAP_CLI_EXE_TESTS "")

        file(GLOB PWCAP_SHELL_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/shell/*_test.cpp)
        foreach(test_source IN LISTS PWCAP_SHELL_TEST_SOURCES)
            get_filename_component(test_stem ${test_source} NAME_WE)
            set(test_target shell_${test_stem})
            add_executable(${test_target} ${test_source})
            target_include_directories(${test_target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
            target_link_libraries(${test_target}
                PRIVATE pwcap pwcap_core PkgConfig::PIPEWIRE pwcap_warnings GTest::gtest_main)
            gtest_discover_tests(${test_target}
                DISCOVERY_MODE PRE_TEST
                PROPERTIES LABELS shell TIMEOUT 120)
            list(APPEND PWCAP_PROBE_DEPENDENT_TESTS ${test_target})
        endforeach()

        # Opt-in integration tests (files arrive in plan 04-02). They need a PipeWire session and
        # skip themselves without PWCAP_TEST_PIPEWIRE=1; ctest must report that as Skipped, not Passed.
        # One session, so they never run in parallel with each other.
        file(GLOB PWCAP_PIPEWIRE_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/pipewire/*_test.cpp)
        foreach(test_source IN LISTS PWCAP_PIPEWIRE_TEST_SOURCES)
            get_filename_component(test_stem ${test_source} NAME_WE)
            set(test_target pipewire_${test_stem})
            add_executable(${test_target} ${test_source})
            target_link_libraries(${test_target}
                PRIVATE pwcap nlohmann_json::nlohmann_json pwcap_warnings GTest::gtest_main)
            gtest_discover_tests(${test_target}
                DISCOVERY_MODE PRE_TEST
                PROPERTIES LABELS pipewire TIMEOUT 120 RESOURCE_LOCK pipewire_session
                           SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
            list(APPEND PWCAP_PROBE_DEPENDENT_TESTS ${test_target})
            list(APPEND PWCAP_CLI_EXE_TESTS ${test_target})
        endforeach()

        # CLI unit tests: the pure logic library only, no PipeWire. Labelled cli.
        if(TARGET pwcap_cli_lib)
            file(GLOB PWCAP_CLI_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/cli/*_test.cpp)
            foreach(test_source IN LISTS PWCAP_CLI_TEST_SOURCES)
                get_filename_component(test_stem ${test_source} NAME_WE)
                set(test_target cli_${test_stem})
                add_executable(${test_target} ${test_source})
                target_link_libraries(${test_target}
                    PRIVATE pwcap_cli_lib pwcap pwcap_warnings GTest::gtest_main)
                gtest_discover_tests(${test_target}
                    DISCOVERY_MODE PRE_TEST
                    PROPERTIES LABELS cli TIMEOUT 120)
                list(APPEND PWCAP_CLI_EXE_TESTS ${test_target})
            endforeach()
        endif()

        # Every cli and pipewire test gets the path of the built pwcap executable, so a test can
        # spawn it. The executable is built first.
        if(TARGET pwcap_cli)
            foreach(test_target IN LISTS PWCAP_CLI_EXE_TESTS)
                target_compile_definitions(${test_target} PRIVATE
                    "PWCAP_CLI_EXE=\"$<TARGET_FILE:pwcap_cli>\"")
                add_dependencies(${test_target} pwcap_cli)
            endforeach()
        endif()

        if(TARGET shell_capture_probe)
            foreach(test_target IN LISTS PWCAP_PROBE_DEPENDENT_TESTS)
                target_compile_definitions(${test_target} PRIVATE
                    "PWCAP_CAPTURE_PROBE=\"$<TARGET_FILE:shell_capture_probe>\"")
                add_dependencies(${test_target} shell_capture_probe)
            endforeach()
        endif()
    endif()

    # Public-header gates, filled in by plan 04-06. Nothing is created here when the files are absent.
    file(GLOB PWCAP_API_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/api/*.cpp)
    if(PWCAP_API_SOURCES)
        add_library(pwcap_public_header_check OBJECT ${PWCAP_API_SOURCES})
        target_include_directories(pwcap_public_header_check PRIVATE ${PROJECT_SOURCE_DIR}/include)
        target_link_libraries(pwcap_public_header_check PRIVATE pwcap_warnings)
    endif()
    if(EXISTS ${PROJECT_SOURCE_DIR}/cmake/public_headers.cmake
       AND EXISTS ${PROJECT_SOURCE_DIR}/tests/api/fixtures/non_std_include.hpp)
        add_test(NAME public_headers_std_only
                 COMMAND ${CMAKE_COMMAND} -DPWCAP_ROOT=${PROJECT_SOURCE_DIR}
                         -P ${PROJECT_SOURCE_DIR}/cmake/public_headers.cmake)
        add_test(NAME public_headers_std_only_selftest
                 COMMAND ${CMAKE_COMMAND}
                         -DPWCAP_SELFTEST_FILE=${PROJECT_SOURCE_DIR}/tests/api/fixtures/non_std_include.hpp
                         -P ${PROJECT_SOURCE_DIR}/cmake/public_headers.cmake)
        set_tests_properties(public_headers_std_only public_headers_std_only_selftest PROPERTIES LABELS api)
    else()
        message(STATUS "public_headers: gate files from plan 04-06 not present")
    endif()

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
