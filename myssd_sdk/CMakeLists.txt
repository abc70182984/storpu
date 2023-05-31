cmake_minimum_required(VERSION 3.5)
project(myssd_sdk)

set(TOPDIR ${PROJECT_SOURCE_DIR})

set(CMAKE_MODULE_PATH "${TOPDIR}/cmake;${CMAKE_MODULE_PATH}")

# dependencies
INCLUDE(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(UBDSRV)

ADD_SUBDIRECTORY(um)
