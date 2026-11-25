# The radio driver's suite: the real mesh_drv_lr2021.c, over ESP-IDF's GPIO, SPI and
# timer calls replaced by a model of the LR2021 that works at the level of SPI
# frames - the chip's own rules (the receiver's payload_len filter, SetRx that
# fails in Rx, SetTx's timeout, the interrupt word) written from the datasheet,
# not from the driver. A project of its own: C allows one definition per binary,
# and tests/host links a fake of this driver where this suite links the real one.
#
# `set(COMPONENTS main)` must come BEFORE project(): see tests/host.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(mesh_drv_tests)
