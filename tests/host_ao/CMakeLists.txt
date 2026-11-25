# The owner's Active Object (components/mesh_aos/mesh/src/mesh_ao.c) on kode-sdk's
# own QP/C, built for ESP-IDF's `linux` target.
cmake_minimum_required(VERSION 3.16)

# kode-sdk at the release the apps pin: ../kode_sdk.cmake clones it once.
include(${CMAKE_CURRENT_LIST_DIR}/../kode_sdk.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# QP/C as kode-sdk's own tests/host takes it: its component, as it ships.
set(EXTRA_COMPONENT_DIRS "${KODE_SDK_DIR}/components/qpc")

# Before project(), as in tests/host: only main and what it requires.
set(COMPONENTS main)

project(mesh_ao_tests)
