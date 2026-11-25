/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_harness.c
 * @brief See radio_harness.h.
 */
#include "radio_harness.h"

volatile unsigned g_irq_calls;

static const mesh_drv_lr2021_pins_t PINS = {
    .spi_host = 2, .chip_select = PIN_CS, .clock = PIN_SCK, .mosi = PIN_MOSI, .miso = PIN_MISO,
    .busy = PIN_BUSY, .reset = PIN_RST, .dio9 = PIN_DIO9,
};

static void on_irq(void *arg)
{
    (void)arg;
    g_irq_calls++;
}

const mesh_drv_lr2021_pins_t *radio_pins(void)
{
    return &PINS;
}

void radio_open(void)
{
    mesh_drv_lr2021_close();          /* a no-op when the last case left it closed */
    chip_power_up();
    fake_log_clear();
    g_irq_calls = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_open(&PINS, on_irq, NULL));
}

void radio_up_raw(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm)
{
    uint8_t       major = 0U;
    uint8_t       minor = 0U;
    mesh_drv_lora_settings_t cfg;

    radio_open();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(band, spreading_factor, power_cdbm, &cfg));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_configure(&cfg));
}

void radio_up(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm)
{
    radio_up_raw(band, spreading_factor, power_cdbm);
    chip_log_clear();
    g_irq_calls = 0U;
}

void radio_up_sub(void)
{
    radio_up(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
}

void chip_finish_tx(void)
{
    chip_advance_us(g_chip.air_us + 1U);
}

uint64_t last_tx_timeout_us(void)
{
    const chip_cmd_t *c = chip_last(OPC_SET_TX);

    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(3U, c->n);
    uint32_t const ticks = ((uint32_t)c->p[0] << 16) | ((uint32_t)c->p[1] << 8) | (uint32_t)c->p[2];
    return chip_ticks_to_us(ticks);
}

void fill_pattern(uint8_t *buf, unsigned n)
{
    for (unsigned i = 0U; i < n; i++) {
        buf[i] = (uint8_t)('a' + (i % 26U));
    }
}
