# The console's suite: examples/console_actions's rows and its Active Object,
# over a mesh_sys.h that only writes down what it is asked. A project of its
# own because tests/host_sys links the real mesh_sys.c, and C allows one
# definition per binary.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
# kode-sdk at the release the apps pin: ../kode_sdk.cmake clones it once. Only its
# console tables (kode_functions.h, kode_variables.h), kode_defs.h and klog's header are used.
include(${CMAKE_CURRENT_LIST_DIR}/../kode_sdk.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_console_tests)
