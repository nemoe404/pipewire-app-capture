#ifndef PWCAP_TESTS_API_FIXTURES_NON_STD_INCLUDE_HPP
#define PWCAP_TESTS_API_FIXTURES_NON_STD_INCLUDE_HPP

// Self-test input for cmake/public_headers.cmake. It is never compiled. Exactly one include below
// is a violation: the third-party fmt header. The standard header and the pwcap header are fine.

#include <fmt/format.h>
#include <pwcap/capture.hpp>
#include <string>

#endif // PWCAP_TESTS_API_FIXTURES_NON_STD_INCLUDE_HPP
