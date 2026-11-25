# mesh_drv - the module's drivers. Each one knows a part or a peripheral and
# nothing about who calls it or when: that is mesh_aos's.
#
# mesh_drv_lora.c and mesh_drv_nmea.c are plain C on purpose - no ESP-IDF, no
# FreeRTOS - because they are LoRa's arithmetic and the GNSS's sentences, and
# tests/host runs them as they are.
#
# Laid out as kdrv is: one folder per driver, its header in include/ and its
# source in src/. Every include/ is listed below, so an #include names the
# header alone and the folders are on disk only.
idf_component_register(
    SRCS "gnss/src/mesh_drv_gnss.c"
         "lora/src/mesh_drv_lora.c"
         "lr2021/src/mesh_drv_lr2021.c"
         "nmea/src/mesh_drv_nmea.c"
         "pins/src/mesh_drv_pins.c"
    INCLUDE_DIRS "gnss/include"
                 "lora/include"
                 "lr2021/include"
                 "nmea/include"
                 "pins/include"
    REQUIRES kdefs mesh_bsp
    PRIV_REQUIRES klog freertos esp_driver_gpio esp_driver_spi esp_driver_uart
                  esp_timer esp_rom esp_system
)
