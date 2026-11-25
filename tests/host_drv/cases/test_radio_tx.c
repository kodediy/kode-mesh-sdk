/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_tx.c
 * @brief A packet on air. SetTx carries the chip's own deadline - an explicit
 *        timeout, never 0 (0 switches it off), long enough for the packet - so a
 *        done flag that never comes does not leave the amplifier keyed. And the
 *        power the chip is configured to, per band.
 */
#include <stdint.h>
#include <stdio.h>

#include "unity.h"

#include "radio_harness.h"

static const uint8_t LENGTHS[] = { 1U, 5U, 51U, 128U, 255U };

/* ---- The chip's deadline for a packet ---------------------------------------------------------- */

static void check_deadline_for(mesh_plan_band_t band, uint8_t sf, int32_t cdbm)
{
    for (unsigned i = 0U; i < sizeof(LENGTHS); i++) {
        uint8_t const len = LENGTHS[i];
        uint8_t       data[255];
        char          what[80];

        radio_up(band, sf, cdbm);
        fill_pattern(data, len);
        (void)snprintf(what, sizeof(what), "band %u sf %u len %u", (unsigned)band, (unsigned)sf, (unsigned)len);

        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_drv_lr2021_send(data, len), what);
        const chip_cmd_t *tx = chip_last(OPC_SET_TX);
        TEST_ASSERT_NOT_NULL_MESSAGE(tx, what);
        uint32_t const ticks = ((uint32_t)tx->p[0] << 16) | ((uint32_t)tx->p[1] << 8) | tx->p[2];
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, ticks, what);   /* 0 would switch the timeout off */
        TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(0xFFFFFFU, ticks, what);

        /* The datasheet's airtime of the packet on the air (not the driver's own). */
        uint64_t const air = chip_lora_air_us(g_chip.sf, g_chip.bw, g_chip.ldro != 0U, g_chip.pbl, len);
        uint64_t const to  = chip_ticks_to_us(ticks);
        TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(0U, air, what);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(air, g_chip.air_us, what);
        TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(air + 19999U, to, what);        /* at least 20 ms to spare */
        TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(air + (air / 4U) - 1U, to, what); /* and a quarter more     */
        TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(air * 2U + 100000U, to, what);      /* not open-ended          */
        TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(512ULL * 1000000ULL, to, what);     /* the chip's own limit    */

        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_chip.n_failed, what);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_chip.n_tx_len_mismatch, what);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(len, g_chip.tx_fifo_n, what);
        TEST_ASSERT_EQUAL_MESSAGE(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode, what);
    }
}

TEST_CASE("send: SetTx carries an explicit timeout that covers the packet, at every SF and length (868)", "[radio][tx]")
{
    for (uint8_t sf = 7U; sf <= 12U; sf++) {
        check_deadline_for(MESH_PLAN_BAND_SUB_GHZ, sf, 850);
    }
}

TEST_CASE("send: SetTx carries an explicit timeout that covers the packet, at every SF and length (2.4 GHz)", "[radio][tx]")
{
    for (uint8_t sf = 7U; sf <= 12U; sf++) {
        check_deadline_for(MESH_PLAN_BAND_2_4_GHZ, sf, 450);
    }
}

TEST_CASE("send: a packet that completes raises TxDone and leaves the chip in standby, FIFO empty", "[radio][tx]")
{
    radio_up_sub();
    uint8_t data[20];
    fill_pattern(data, sizeof(data));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL(0U, g_irq_calls);

    chip_advance_us(g_chip.air_us - 1000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    chip_advance_us(2000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.tx_fifo_n);
}

TEST_CASE("send: if the done flag never comes, the chip's own timeout ends the transmit", "[radio][tx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));
    g_chip.force_air_us = 30ULL * 1000000ULL;   /* a transmit that does not finish */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));

    uint64_t const to = last_tx_timeout_us();
    chip_advance_us(to - 10000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    chip_advance_us(20000U);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);   /* off air, by the chip's doing alone */
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_TIMEOUT, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);                  /* and DIO9 told the host */
}

TEST_CASE("send: a cut-short transmit leaves bytes in the FIFO, and the next send clears them first", "[radio][tx]")
{
    radio_up_sub();
    uint8_t first[5];
    uint8_t second[7];
    fill_pattern(first, sizeof(first));
    fill_pattern(second, sizeof(second));

    g_chip.force_air_us = 30ULL * 1000000ULL;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(first, sizeof(first)));
    chip_advance_us(last_tx_timeout_us() + 1000U);
    (void)mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL_UINT(5U, g_chip.tx_fifo_n);              /* it never left whole */

    g_chip.force_air_us = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(second, sizeof(second)));
    TEST_ASSERT_EQUAL_UINT(7U, g_chip.tx_fifo_n);              /* not 12 */
    TEST_ASSERT_EQUAL_MEMORY(second, g_chip.tx_fifo, 7U);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_tx_len_mismatch);
}

TEST_CASE("send: while a packet is on air the chip refuses a second SetTx and the driver says so", "[radio][tx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_TRUE(fake_log_seen("the chip refused"));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);   /* the first goes on */
}

TEST_CASE("send: no payload, no length, or no setting in force sends nothing to the chip", "[radio][tx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_send(NULL, 5U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_send(data, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());

    radio_open();   /* open, never configured: no airtime to bound a transmit by */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

TEST_CASE("send: a chip that cannot be asked in time reports it, and leaves nothing on air", "[radio][tx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));
    g_chip.busy_stuck = true;
    g_chip.creep_us   = 20000U;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_TRUE(fake_log_seen("BUSY stayed high"));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
}

/* ---- The power the chip is configured to --------------------------------------------------------- */

static int8_t configured_power(mesh_plan_band_t band, uint8_t sf, int32_t cdbm)
{
    radio_up_raw(band, sf, cdbm);
    const chip_cmd_t *c = chip_last(OPC_SET_TX_PARAMS);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    return (int8_t)c->p[0];
}

TEST_CASE("power: any ask is held to the region's cap by the time it reaches the chip", "[radio][tx][power]")
{
    TEST_ASSERT_EQUAL_INT8(19, configured_power(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));    /* the cap */
    TEST_ASSERT_EQUAL_INT8(19, configured_power(MESH_PLAN_BAND_SUB_GHZ, 9U, 1250));   /* above the cap */
    TEST_ASSERT_EQUAL_INT8(19, configured_power(MESH_PLAN_BAND_SUB_GHZ, 9U, 3000));
    TEST_ASSERT_EQUAL_INT8(18, configured_power(MESH_PLAN_BAND_SUB_GHZ, 9U, 800));
    TEST_ASSERT_EQUAL_INT8(13, configured_power(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_EQUAL_INT8(13, configured_power(MESH_PLAN_BAND_2_4_GHZ, 9U, 550));     /* above the cap */
    TEST_ASSERT_EQUAL_INT8(12, configured_power(MESH_PLAN_BAND_2_4_GHZ, 9U, 400));
}

TEST_CASE("power: the amplifier selected is the band's, the other left at idle", "[radio][tx][power]")
{
    radio_up_raw(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    const chip_cmd_t *pa = chip_last(OPC_SET_PA_CONFIG);
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_EQUAL_HEX8(0x00U, pa->p[0]);   /* the sub-GHz amplifier */
    TEST_ASSERT_EQUAL_UINT8(16U, pa->p[2]);    /* the 2.4 GHz one idles */

    radio_up_raw(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    pa = chip_last(OPC_SET_PA_CONFIG);
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_EQUAL_HEX8(0x80U, pa->p[0]);   /* the 2.4 GHz amplifier */
    TEST_ASSERT_EQUAL_HEX8(0x67U, pa->p[1]);   /* and the sub-GHz one at its idle duty and slices */
}

TEST_CASE("power: on 2.4 GHz tx_power is written before the amplifier is selected and again after", "[radio][tx][power]")
{
    radio_up_raw(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    int pa_at = -1;
    unsigned params_before_pa = 0U;
    unsigned params_after_pa  = 0U;
    for (unsigned i = 0U; i < chip_log_len(); i++) {
        const chip_cmd_t *c = chip_log_at(i);
        if (c->op == OPC_SET_PA_CONFIG) {
            pa_at = (int)i;
        } else if (c->op == OPC_SET_TX_PARAMS) {
            if (pa_at < 0) { params_before_pa++; } else { params_after_pa++; }
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pa_at);
    TEST_ASSERT_EQUAL_UINT(1U, params_before_pa);
    TEST_ASSERT_EQUAL_UINT(1U, params_after_pa);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);

    radio_up_raw(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);   /* and on the sub-GHz path it needs only the one */
    pa_at = -1;
    params_before_pa = 0U;
    params_after_pa  = 0U;
    for (unsigned i = 0U; i < chip_log_len(); i++) {
        const chip_cmd_t *c = chip_log_at(i);
        if (c->op == OPC_SET_PA_CONFIG) {
            pa_at = (int)i;
        } else if (c->op == OPC_SET_TX_PARAMS) {
            if (pa_at < 0) { params_before_pa++; } else { params_after_pa++; }
        }
    }
    TEST_ASSERT_EQUAL_UINT(0U, params_before_pa);
    TEST_ASSERT_EQUAL_UINT(1U, params_after_pa);
}

TEST_CASE("power: the over-current thresholds are written with every setting", "[radio][tx][power]")
{
    radio_up_raw(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    TEST_ASSERT_EQUAL_UINT(2U, chip_count(OPC_WRITE_REG_MASK));   /* the unlock word, then the thresholds */
    radio_up_raw(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    TEST_ASSERT_EQUAL_UINT(2U, chip_count(OPC_WRITE_REG_MASK));
}

TEST_CASE("calibrate: the front end for the frequency and path set", "[radio][tx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate());
    const chip_cmd_t *c = chip_last(OPC_CALIB_FE);
    TEST_ASSERT_NOT_NULL(c);
    uint16_t const f4 = (uint16_t)(((uint16_t)c->p[0] << 8) | c->p[1]);
    TEST_ASSERT_EQUAL_UINT16(g_chip.freq_hz / 4000000U, f4);   /* 868.1 MHz in 4 MHz steps, 868 path */

    radio_up(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate());
    c = chip_last(OPC_CALIB_FE);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_BITS(0x80U, 0x80U, c->p[0]);                    /* the HF path bit */
}
