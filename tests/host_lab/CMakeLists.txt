# The LAB build's host suite: the same plan the default build runs, compiled
# with CONFIG_MESH_LAB, to show that the one switch of the SDK does what its
# Kconfig help says and nothing more. tests/host is the shipped build; this
# is its opposite, kept small.
#
# `set(COMPONENTS main)` must come BEFORE project(), as in tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_lab_tests)
