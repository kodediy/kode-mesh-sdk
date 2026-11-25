# The LAB build's host suite with the datasheet's power rows on: the plan
# compiled with CONFIG_MESH_LAB and CONFIG_MESH_LAB_PA_TABLE. C allows one
# definition per binary and the table is a compile-time switch, so the rows above
# the default one are tested here and tests/host_lab holds what the LAB build
# does without them.
#
# `set(COMPONENTS main)` must come BEFORE project(), as in tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_lab_pa_tests)
