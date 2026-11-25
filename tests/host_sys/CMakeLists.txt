# The extended layer's suite: mesh_sys.c, what an Active Object calls, on kode-sdk's
# own QP/C (real events, real queues, real pools) over an owner that only writes down
# what it is asked and answers when a case says so. A project of its own because a
# suite that linked the real owner would define mesh_aos_* twice.
cmake_minimum_required(VERSION 3.16)

# kode-sdk at the release the apps pin: ../kode_sdk.cmake clones it once.
include(${CMAKE_CURRENT_LIST_DIR}/../kode_sdk.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# QP/C as kode-sdk's own tests/host takes it: its component, as it ships.
set(EXTRA_COMPONENT_DIRS "${KODE_SDK_DIR}/components/qpc")

# Before project(), as in tests/host: only main and what it requires.
set(COMPONENTS main)

project(mesh_sys_tests)
