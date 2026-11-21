# SPDX-License-Identifier: Apache-2.0
#
# public_headers.cmake - machine check that the public headers include only standard headers.
#
#   API-01  every #include under include/pwcap is a C++ standard header or a pwcap/ header
#
# A standard header name is bare, lowercase letters and underscores only (<string>, <expected>,
# <move_only_function>). The C library wrappers (<cstdint>) fit the same form. A pwcap/ header may
# be written in quotes or in angle brackets. Anything else is a violation: a quoted include of a
# non-pwcap path, a C header with a dot, a path with a slash (<fmt/format.h>, <pipewire/pipewire.h>)
# or a name with a digit or capital letter.
#
# Run it in script mode. Exactly one of the two variables must be set.
#
#   scan mode:       cmake -DPWCAP_ROOT=<absolute source root> -P cmake/public_headers.cmake
#   self-test mode:  cmake -DPWCAP_SELFTEST_FILE=<file>        -P cmake/public_headers.cmake
#
# Scan mode prints one line per violation, "public_headers: non-std-include <file>: <line>", then a
# final "public_headers: <N> violations", and fails when N is above zero. Self-test mode applies the
# same rule to a single file and succeeds only on exactly one violation.
#
# This is a line scanner. It does not strip comments, so an include inside a comment is checked as
# well. That is deliberate: it fails safe, because a commented-out third-party include is one
# uncomment away from a real dependency. The compile-isolation object library
# pwcap_public_header_check (tests/api) is the second line of defence.

cmake_minimum_required(VERSION 3.25)

# Number of violations in the file at `path`; the offending lines are printed when `report` is true.
function(pwcap_public_headers_count path rel report out_var)
    file(STRINGS "${path}" lines)
    set(count 0)
    foreach(line IN LISTS lines)
        # An include directive: optional space, '#', optional space, 'include', then the target.
        if(NOT line MATCHES "^[ \t]*#[ \t]*include[ \t]*([<\"][^>\"]*[>\"])")
            continue()
        endif()
        set(target "${CMAKE_MATCH_1}")
        if(target MATCHES "^<[a-z_]+>$" OR target MATCHES "^[<\"]pwcap/[A-Za-z0-9_./-]+[>\"]$")
            continue()
        endif()
        math(EXPR count "${count} + 1")
        if(report)
            message(NOTICE "public_headers: non-std-include ${rel}: ${line}")
        endif()
    endforeach()
    set(${out_var} ${count} PARENT_SCOPE)
endfunction()

if(DEFINED PWCAP_ROOT AND DEFINED PWCAP_SELFTEST_FILE)
    message(FATAL_ERROR "public_headers: set exactly one of PWCAP_ROOT and PWCAP_SELFTEST_FILE")
endif()

if(DEFINED PWCAP_SELFTEST_FILE)
    if(NOT EXISTS "${PWCAP_SELFTEST_FILE}")
        message(FATAL_ERROR "public_headers selftest: no such file ${PWCAP_SELFTEST_FILE}")
    endif()
    pwcap_public_headers_count("${PWCAP_SELFTEST_FILE}" "${PWCAP_SELFTEST_FILE}" TRUE violations)
    if(violations EQUAL 1)
        message(NOTICE "public_headers selftest: 1 violation")
    else()
        message(FATAL_ERROR "public_headers selftest: ${violations} violations (expected 1)")
    endif()
elseif(DEFINED PWCAP_ROOT)
    if(NOT IS_DIRECTORY "${PWCAP_ROOT}/include/pwcap")
        message(FATAL_ERROR "public_headers: no directory ${PWCAP_ROOT}/include/pwcap")
    endif()
    file(GLOB_RECURSE headers LIST_DIRECTORIES false "${PWCAP_ROOT}/include/pwcap/*.hpp")
    list(SORT headers)
    if(NOT headers)
        message(FATAL_ERROR "public_headers: no header found under ${PWCAP_ROOT}/include/pwcap")
    endif()
    set(total 0)
    foreach(header IN LISTS headers)
        file(RELATIVE_PATH rel "${PWCAP_ROOT}" "${header}")
        pwcap_public_headers_count("${header}" "${rel}" TRUE violations)
        math(EXPR total "${total} + ${violations}")
    endforeach()
    if(total GREATER 0)
        message(FATAL_ERROR "public_headers: ${total} violations")
    endif()
    message(NOTICE "public_headers: 0 violations")
else()
    message(FATAL_ERROR
        "usage: cmake -DPWCAP_ROOT=<source root> -P public_headers.cmake  |  "
        "cmake -DPWCAP_SELFTEST_FILE=<file> -P public_headers.cmake")
endif()
