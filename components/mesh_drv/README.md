mesh_drv
========

The drivers of the module, one folder per part: lr2021 (the radio over SPI), lora (LoRa's arithmetic), gnss (the receiver over UART), nmea (its sentences) and pins (the fingerprint). Only the owner calls a driver.

A driver holds none of the region's limits: those are the owner's. An app must not include these headers.

Depends on: mesh_bsp, and kdefs and klog from kode-sdk

Docs: https://docs.kode.diy/modules/mesh
