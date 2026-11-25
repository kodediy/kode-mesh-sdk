/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file driver/spi_master.h
 * @brief The slice of ESP-IDF's SPI master that mesh_drv_lr2021.c calls, for the
 *        radio driver host suite. Names and types are ESP-IDF's
 *        (components/esp_driver_spi at v6.1); the bus is in lr_chip.c.
 */
#ifndef FAKE_DRIVER_SPI_MASTER_H_
#define FAKE_DRIVER_SPI_MASTER_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPI1_HOST = 0,
    SPI2_HOST = 1,
    SPI3_HOST = 2,
} spi_host_device_t;

typedef enum {
    SPI_DMA_DISABLED = 0,
    SPI_DMA_CH_AUTO = 3,
} spi_dma_chan_t;

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    int      mode;
    int      clock_speed_hz;
    int      spics_io_num;
    int      queue_size;
} spi_device_interface_config_t;

typedef struct {
    size_t       length;       /**< Total bits.                    */
    const void  *tx_buffer;
    void        *rx_buffer;
} spi_transaction_t;

typedef struct spi_device_t *spi_device_handle_t;

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config,
                             spi_dma_chan_t dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);
esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc);
/* ESP-IDF declares the wait as uint32_t, and on the P4 portMAX_DELAY is 32 bits.
 * The host port's TickType_t is 64 bits wide, so the idiom spi_device_acquire_bus(h,
 * portMAX_DELAY) would narrow there and trip -Wconstant-conversion. The wait goes
 * through a wider parameter instead; production code is the same on both. */
esp_err_t spi_device_acquire_bus_host(spi_device_handle_t device, uint64_t wait);
#define spi_device_acquire_bus(device, wait) spi_device_acquire_bus_host((device), (uint64_t)(wait))
void      spi_device_release_bus(spi_device_handle_t dev);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DRIVER_SPI_MASTER_H_ */
