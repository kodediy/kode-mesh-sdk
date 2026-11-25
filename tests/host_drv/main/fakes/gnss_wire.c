/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gnss_wire.c
 * @brief ESP-IDF's UART calls over the model in gnss_wire.h. See there.
 */
#include "gnss_wire.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"

gnss_wire_t g_wire;

#define RX_MAX 1024U

static uint8_t s_rx[RX_MAX];
static size_t  s_rx_n;
static size_t  s_rx_at;

void gnss_wire_reset(void)
{
    (void)memset(&g_wire, 0, sizeof(g_wire));
    s_rx_n  = 0U;
    s_rx_at = 0U;
}

void gnss_wire_say(const char *body)
{
    uint8_t x = 0U;
    for (const char *c = body; *c != '\0'; c++) {
        x ^= (uint8_t)*c;
    }
    char line[128];
    int const n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, (unsigned)x);
    if ((n > 0) && ((s_rx_n + (size_t)n) <= RX_MAX)) {
        (void)memcpy(&s_rx[s_rx_n], line, (size_t)n);
        s_rx_n += (size_t)n;
    }
}

void gnss_wire_raw(const char *bytes)
{
    size_t const n = strlen(bytes);
    if ((s_rx_n + n) <= RX_MAX) {
        (void)memcpy(&s_rx[s_rx_n], bytes, n);
        s_rx_n += n;
    }
}

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size,
                              QueueHandle_t *uart_queue, int intr_alloc_flags)
{
    (void)uart_num;
    (void)rx_buffer_size;
    (void)tx_buffer_size;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;
    g_wire.installed = true;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    (void)uart_num;
    g_wire.installed = false;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
    (void)uart_num;
    g_wire.baud = (uint32_t)uart_config->baud_rate;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num)
{
    (void)uart_num;
    (void)tx_io_num;
    (void)rx_io_num;
    (void)rts_io_num;
    (void)cts_io_num;
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate)
{
    (void)uart_num;
    g_wire.baud = baudrate;
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t uart_num, uint32_t ticks_to_wait)
{
    (void)uart_num;
    (void)ticks_to_wait;
    g_wire.tx_waits++;
    return ESP_OK;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    (void)uart_num;
    if (!g_wire.installed || g_wire.write_fails) {
        return 0;
    }
    const uint8_t *b = (const uint8_t *)src;
    for (size_t i = 0U; (i < size) && (g_wire.tx_n < GNSS_WIRE_TX_MAX); i++) {
        g_wire.tx[g_wire.tx_n] = b[i];
        g_wire.tx_n++;
    }
    return (int)size;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait)
{
    (void)uart_num;
    (void)ticks_to_wait;
    size_t n = s_rx_n - s_rx_at;
    if (n > length) {
        n = length;
    }
    (void)memcpy(buf, &s_rx[s_rx_at], n);
    s_rx_at += n;
    if (s_rx_at == s_rx_n) {
        s_rx_at = 0U;
        s_rx_n  = 0U;
    }
    return (int)n;
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    (void)uart_num;
    s_rx_n  = 0U;
    s_rx_at = 0U;
    return ESP_OK;
}
