# The owner's suite: mesh_aos.c as it is, on the linux target's FreeRTOS,
# against a simulated module (main/fakes) and an Active Object of mesh_ao.h
# that is only a queue and a task. The plain-C rules it stands on (mesh_plan,
# mesh_drv_lora, mesh_drv_nmea) run as they are; everything else is a fake.
# Of kode-sdk it takes only kdefs' header, from the clone of ../kode_sdk.cmake;
# the other kode-sdk headers it compiles against are fakes (tests/target is
# where the real ones are checked).
#
# `set(COMPONENTS main)` must come BEFORE project(): without it the configure
# walks the whole component registry looking for host implementations that
# do not exist (kode-sdk's tests/host says the same, and why).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_owner_tests)
