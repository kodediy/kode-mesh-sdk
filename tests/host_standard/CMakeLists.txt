# The standard layer's suite: mesh.c, what a task calls, over an owner that only writes down what
# it is asked. A project of its own because a suite that linked the real owner would define
# mesh_aos_* twice, and C allows one definition per binary.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)

# kode-sdk at the release the apps pin, for kode.h: ../kode_sdk.cmake clones it once.
include(${CMAKE_CURRENT_LIST_DIR}/../kode_sdk.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_standard_tests)
