# Out-of-tree consumer of an installed pwcap. This project is never added as a subdirectory of the
# main build: it configures against an install prefix, through find_package(pwcap).
cmake_minimum_required(VERSION 3.28)
project(pwcap_install_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The version the consumer asks for. The package's version file accepts 0.1 and rejects 0.2.
set(PWCAP_WANT_VERSION 0.1 CACHE STRING "pwcap package version to request")
find_package(pwcap ${PWCAP_WANT_VERSION} CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
# The package brings libpipewire-0.3 with it. The consumer adds no PipeWire flags by hand.
target_link_libraries(consumer PRIVATE pwcap::pwcap)
