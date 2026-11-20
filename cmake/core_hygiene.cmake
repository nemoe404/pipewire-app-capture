# SPDX-License-Identifier: Apache-2.0
#
# core_hygiene.cmake - machine check for two hard constraints of the pure core.
#
#   CORE-01  no PipeWire or SPA header is included under src/core, tests/core or include/pwcap
#   CORE-05  no plain built-in integer type is used there (fixed-width names only)
#
# Run it in script mode. Exactly one of the two variables must be set.
#
#   scan mode:       cmake -DPWCAP_ROOT=<absolute source root> -P cmake/core_hygiene.cmake
#   self-test mode:  cmake -DPWCAP_SELFTEST_FILE=<file>        -P cmake/core_hygiene.cmake
#
# Scan mode prints one line per violation, "core_hygiene: <rule> <file>", then a final
# "core_hygiene: <N> violations", and fails when N is above zero. Self-test mode applies both
# rules to a single file and succeeds only on exactly one violation of each rule.
#
# Known limit: raw string literals (R"(...)") are not recognised.

cmake_minimum_required(VERSION 3.25)

set(_pwcap_hygiene_dirs src/core tests/core include/pwcap)

# Copy `text` to `out_var` with comments removed. When `strip_literals` is true, string and
# character literals are removed too; otherwise they are kept verbatim (the backend-include rule
# needs them, because the quoted include form is itself a quoted token).
#
# This is a small lexer built on string(FIND). CMake regex has no non-greedy match: a greedy
# pattern from a comment opener to its closer would delete the code between two comments and
# hide violations. Each step finds the nearest of the four openers and handles that one token.
function(pwcap_hygiene_strip text strip_literals out_var)
    set(result "")
    set(rest "${text}")
    while(TRUE)
        string(LENGTH "${rest}" rest_len)
        if(rest_len EQUAL 0)
            break()
        endif()

        # Nearest opener among line comment, block comment, string literal, character literal.
        set(best -1)
        set(best_kind "")
        foreach(kind IN ITEMS "//" "/*" "\"" "'")
            string(FIND "${rest}" "${kind}" pos)
            if(NOT pos EQUAL -1 AND (best EQUAL -1 OR pos LESS best))
                set(best ${pos})
                set(best_kind "${kind}")
            endif()
        endforeach()

        if(best EQUAL -1)
            string(APPEND result "${rest}")
            break()
        endif()

        string(SUBSTRING "${rest}" 0 ${best} before)
        string(APPEND result "${before}")
        string(SUBSTRING "${rest}" ${best} -1 rest)

        if(best_kind STREQUAL "//")
            # Line comment: drop up to, not including, the newline.
            string(FIND "${rest}" "\n" eol)
            if(eol EQUAL -1)
                set(rest "")
            else()
                string(SUBSTRING "${rest}" ${eol} -1 rest)
            endif()
        elseif(best_kind STREQUAL "/*")
            # Block comment: drop up to and including the closer. It is replaced by one space
            # so that tokens on either side do not fuse.
            string(SUBSTRING "${rest}" 2 -1 body)
            string(FIND "${body}" "*/" close)
            string(APPEND result " ")
            if(close EQUAL -1)
                set(rest "")
            else()
                math(EXPR skip "${close} + 2")
                string(SUBSTRING "${body}" ${skip} -1 rest)
            endif()
        else()
            # Literal opened by best_kind (a quote). A single quote right after a letter or
            # digit is a C++14 digit separator (1'000), not a literal opener.
            string(LENGTH "${result}" result_len)
            set(is_separator FALSE)
            if(best_kind STREQUAL "'" AND result_len GREATER 0)
                math(EXPR last "${result_len} - 1")
                string(SUBSTRING "${result}" ${last} 1 prev_char)
                if(prev_char MATCHES "[0-9A-Za-z]")
                    set(is_separator TRUE)
                endif()
            endif()
            if(is_separator)
                string(SUBSTRING "${rest}" 1 -1 rest)
                string(APPEND result "'")
                continue()
            endif()

            # Find the closing quote, skipping any quote preceded by an odd run of backslashes.
            string(SUBSTRING "${rest}" 1 -1 body)
            set(offset 0)
            set(close_at -1)
            while(TRUE)
                string(SUBSTRING "${body}" ${offset} -1 tail)
                string(FIND "${tail}" "${best_kind}" rel)
                if(rel EQUAL -1)
                    break()
                endif()
                math(EXPR abs "${offset} + ${rel}")
                set(backslashes 0)
                set(probe ${abs})
                while(probe GREATER 0)
                    math(EXPR probe "${probe} - 1")
                    string(SUBSTRING "${body}" ${probe} 1 ch)
                    if(ch STREQUAL "\\")
                        math(EXPR backslashes "${backslashes} + 1")
                    else()
                        break()
                    endif()
                endwhile()
                math(EXPR odd "${backslashes} % 2")
                if(odd EQUAL 0)
                    set(close_at ${abs})
                    break()
                endif()
                math(EXPR offset "${abs} + 1")
            endwhile()

            if(close_at EQUAL -1)
                # Unterminated literal: the rest of the file is literal text.
                set(literal "${rest}")
                set(rest "")
            else()
                math(EXPR literal_len "${close_at} + 2")
                string(SUBSTRING "${rest}" 0 ${literal_len} literal)
                string(SUBSTRING "${rest}" ${literal_len} -1 rest)
            endif()
            if(strip_literals)
                string(APPEND result " ")
            else()
                string(APPEND result "${literal}")
            endif()
        endif()
    endwhile()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Number of backend-include violations in the file at `path`, stored in `out_var`.
function(pwcap_hygiene_count_backend path out_var)
    file(READ "${path}" content)
    pwcap_hygiene_strip("${content}" FALSE code)
    # CMake's ^ anchors the whole string, so match a line start as "start or newline".
    string(REGEX MATCHALL "(^|\n)[ \t]*#[ \t]*include[ \t]*[<\"](pipewire|spa)/" hits "${code}")
    list(LENGTH hits count)
    set(${out_var} ${count} PARENT_SCOPE)
endfunction()

# Number of plain-int violations in the file at `path`, stored in `out_var`.
function(pwcap_hygiene_count_plain_int path out_var)
    file(READ "${path}" content)
    pwcap_hygiene_strip("${content}" TRUE code)
    # Turn every character that cannot be part of an identifier into two spaces, so a whole
    # word is exactly " int " and two neighbouring words never share a separator.
    string(REGEX REPLACE "[^A-Za-z0-9_]" "  " code "${code}")
    set(code " ${code} ")
    string(REGEX MATCHALL " int " hits "${code}")
    list(LENGTH hits count)
    set(${out_var} ${count} PARENT_SCOPE)
endfunction()

# Print `count` violation lines for `rule` in `rel` and add them to the caller's `total`.
function(pwcap_hygiene_report rule rel count)
    set(n 0)
    while(n LESS count)
        message(NOTICE "core_hygiene: ${rule} ${rel}")
        math(EXPR n "${n} + 1")
    endwhile()
    math(EXPR new_total "${total} + ${count}")
    set(total ${new_total} PARENT_SCOPE)
endfunction()

if(DEFINED PWCAP_ROOT AND DEFINED PWCAP_SELFTEST_FILE)
    message(FATAL_ERROR "core_hygiene: set exactly one of PWCAP_ROOT and PWCAP_SELFTEST_FILE")
endif()

if(DEFINED PWCAP_SELFTEST_FILE)
    if(NOT EXISTS "${PWCAP_SELFTEST_FILE}")
        message(FATAL_ERROR "core_hygiene selftest: no such file ${PWCAP_SELFTEST_FILE}")
    endif()
    pwcap_hygiene_count_backend("${PWCAP_SELFTEST_FILE}" backend_count)
    pwcap_hygiene_count_plain_int("${PWCAP_SELFTEST_FILE}" int_count)
    if(backend_count EQUAL 1 AND int_count EQUAL 1)
        message(NOTICE "core_hygiene selftest: backend-include=1 plain-int=1")
    else()
        message(FATAL_ERROR
            "core_hygiene selftest: backend-include=${backend_count} plain-int=${int_count} "
            "(expected 1 and 1)")
    endif()
elseif(DEFINED PWCAP_ROOT)
    set(total 0)
    foreach(dir IN LISTS _pwcap_hygiene_dirs)
        if(NOT IS_DIRECTORY "${PWCAP_ROOT}/${dir}")
            continue()
        endif()
        file(GLOB_RECURSE files LIST_DIRECTORIES false
             "${PWCAP_ROOT}/${dir}/*.hpp" "${PWCAP_ROOT}/${dir}/*.cpp")
        list(SORT files)
        foreach(file IN LISTS files)
            file(RELATIVE_PATH rel "${PWCAP_ROOT}" "${file}")
            pwcap_hygiene_count_backend("${file}" backend_count)
            pwcap_hygiene_count_plain_int("${file}" int_count)
            pwcap_hygiene_report(backend-include "${rel}" ${backend_count})
            pwcap_hygiene_report(plain-int "${rel}" ${int_count})
        endforeach()
    endforeach()
    if(total GREATER 0)
        message(FATAL_ERROR "core_hygiene: ${total} violations")
    endif()
    message(NOTICE "core_hygiene: 0 violations")
else()
    message(FATAL_ERROR
        "usage: cmake -DPWCAP_ROOT=<source root> -P core_hygiene.cmake  |  "
        "cmake -DPWCAP_SELFTEST_FILE=<file> -P core_hygiene.cmake")
endif()
