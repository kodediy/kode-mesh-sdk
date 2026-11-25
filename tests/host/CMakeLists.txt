# The host suite: the rules that are plain C - the owner's plan (mesh_plan),
# LoRa's arithmetic (mesh_drv_lora) and the GNSS's sentences (mesh_drv_nmea) -
# built for ESP-IDF's `linux` target and run as they are, with Unity and
# nothing else. The owner that uses them is tests/host_owner.
#
# `set(COMPONENTS main)` must come BEFORE project(): without it the configure
# walks the whole component registry looking for host implementations that
# do not exist (kode-sdk's tests/host says the same, and why).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_host_tests)
