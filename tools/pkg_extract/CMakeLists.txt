# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Host tool (native arm64 is fine): unpacks fake-signed PS4 packages.
cmake_minimum_required(VERSION 3.25)
project(pkg_extract CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CRYPTOPP REQUIRED IMPORTED_TARGET libcryptopp)
add_executable(pkg_extract src/main.cpp src/core/file_format/pkg.cpp
    src/core/file_format/pkg_type.cpp src/core/crypto/crypto.cpp)
target_include_directories(pkg_extract PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../../gpu/shadps4)
# PKG_EXTRACT_STATIC=ON links Crypto++ statically: a single file for release downloads
# (libz and libc++ come with macOS).
option(PKG_EXTRACT_STATIC "Link Crypto++ statically" OFF)
if(PKG_EXTRACT_STATIC)
    find_library(CRYPTOPP_STATIC NAMES libcryptopp.a HINTS ${CRYPTOPP_LIBRARY_DIRS} REQUIRED)
    target_include_directories(pkg_extract PRIVATE ${CRYPTOPP_INCLUDE_DIRS})
    target_link_libraries(pkg_extract PRIVATE ZLIB::ZLIB ${CRYPTOPP_STATIC})
else()
    target_link_libraries(pkg_extract PRIVATE ZLIB::ZLIB PkgConfig::CRYPTOPP)
endif()
