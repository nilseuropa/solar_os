# SC2 portable crypto/session core. Plain CMake, no SDK.
#   cmake -S protocol/sc2 -B build/sc2 -DSC2_BUILD_TESTS=ON && cmake --build build/sc2 && ctest --test-dir build/sc2
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(sc2 C)

set(SC2_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "SolarOS repository root (vendor crypto, tests/host)")
set(SC2_ED25519_DIR "${SC2_REPO_ROOT}/src/vendor/meshcore/ed25519" CACHE PATH "orlp Ed25519 sources")
set(SC2_WG_DIR "${SC2_REPO_ROOT}/components/wireguard_lwip/src" CACHE PATH "WireGuard component sources (crypto.c, crypto/refc)")
option(SC2_USE_MBEDTLS "Use mbedTLS for SHA-256/HMAC instead of the portable code" OFF)
set(SC2_MBEDTLS_TARGET "MbedTLS::mbedcrypto" CACHE STRING "mbedTLS target to link when SC2_USE_MBEDTLS=ON")
option(SC2_BUILD_TESTS "Build the SC2 host tests from tests/host" OFF)

file(GLOB SC2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set(SC2_VENDOR_SOURCES
  "${SC2_ED25519_DIR}/fe.c"
  "${SC2_ED25519_DIR}/ge.c"
  "${SC2_ED25519_DIR}/sc.c"
  "${SC2_ED25519_DIR}/sha512.c"
  "${SC2_WG_DIR}/crypto/refc/x25519.c"
  "${SC2_WG_DIR}/crypto/refc/chacha20.c"
  "${SC2_WG_DIR}/crypto/refc/chacha20poly1305.c"
  "${SC2_WG_DIR}/crypto/refc/poly1305-donna.c"
  "${SC2_WG_DIR}/crypto.c")

add_library(sc2 STATIC ${SC2_SOURCES} ${SC2_VENDOR_SOURCES})
target_include_directories(sc2
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${SC2_ED25519_DIR}" "${SC2_WG_DIR}/crypto/refc" "${SC2_WG_DIR}")
target_compile_features(sc2 PUBLIC c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(sc2 PRIVATE -Wall -Wextra)
  # vendor code is not -Wsign-compare clean; keep SC2's own sources strict
  set_source_files_properties(${SC2_VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")
  set_source_files_properties(${SC2_SOURCES} PROPERTIES COMPILE_OPTIONS "-Werror")
endif()
if(SC2_USE_MBEDTLS)
  target_compile_definitions(sc2 PUBLIC SC2_USE_MBEDTLS=1)
  target_link_libraries(sc2 PUBLIC ${SC2_MBEDTLS_TARGET})
endif()

if(SC2_BUILD_TESTS)
  enable_testing()
  set(SC2_TEST_DIR "${SC2_REPO_ROOT}/tests/host")
  foreach(t crypto session frame admin)
    add_executable(sc2_${t}_test "${SC2_TEST_DIR}/sc2_${t}_test.c")
    target_include_directories(sc2_${t}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vectors" "${SC2_TEST_DIR}")
    target_link_libraries(sc2_${t}_test PRIVATE sc2)
    add_test(NAME sc2_${t} COMMAND sc2_${t}_test)
  endforeach()
endif()
