# The runner, the cases, the driver under test and the model it talks to.
# Production sources here: the radio driver (mesh_drv_lr2021.c), the GNSS
# driver (mesh_drv_gnss.c) with its NMEA parser (mesh_drv_nmea.c, plain C) over
# a model of its UART (fakes/gnss_wire.c, ESP-IDF's driver/uart.h in fakes),
# LoRa's arithmetic
# it stands on (mesh_drv_lora.c, plain C) and the owner's plan (mesh_plan.c,
# plain C), which the harness uses to build a setting as the owner does.
# ESP-IDF's driver/gpio.h, driver/spi_master.h, esp_system.h, esp_rom_sys.h and
# esp_timer.h are the headers in fakes, which come first on the include
# path, and their functions are in fakes/lr_chip.c.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
list(SORT CASE_SOURCES)

# kode_status_t: kdefs' one header, from the kode-sdk clone of ../../kode_sdk.cmake.
# Not through the component manager: from v0.1.0 kdefs declares esp32p4 only and
# the manager refuses it for `linux` (kode-sdk's own host suites take it this way).
include(${CMAKE_CURRENT_LIST_DIR}/../../kode_sdk.cmake)

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/mesh_drv/lr2021/src/mesh_drv_lr2021.c"
         "${SDK_DIR}/components/mesh_drv/gnss/src/mesh_drv_gnss.c"
         "${SDK_DIR}/components/mesh_drv/nmea/src/mesh_drv_nmea.c"
         "${SDK_DIR}/components/mesh_drv/lora/src/mesh_drv_lora.c"
         "${SDK_DIR}/components/mesh_aos/mesh/src/mesh_plan.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/lr_chip.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/radio_log.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/radio_harness.c"
         "${CMAKE_CURRENT_LIST_DIR}/fakes/gnss_wire.c"
    INCLUDE_DIRS "."
                 "${KODE_SDK_DIR}/components/kdefs/include"
                 "${CMAKE_CURRENT_LIST_DIR}/fakes"
                 "${SDK_DIR}/components/mesh_bsp/board/include"
                 "${SDK_DIR}/components/mesh_drv/lr2021/include"
                 "${SDK_DIR}/components/mesh_drv/lora/include"
                 "${SDK_DIR}/components/mesh_drv/gnss/include"
                 "${SDK_DIR}/components/mesh_drv/nmea/include"
                 "${SDK_DIR}/components/mesh_aos/mesh/src"
    REQUIRES unity freertos esp_common
    WHOLE_ARCHIVE
)

# Sanitizers, only when MESH_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
