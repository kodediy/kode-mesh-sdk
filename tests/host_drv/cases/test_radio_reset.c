/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_reset.c
 * @brief The reset line: pulsing it, holding it, what the driver forgets when
 *        the chip does, what an empty header looks like, and what a restart does
 *        to a radio that is on air.
 */
#include <stdint.h>

#include "unity.h"

#include "esp_system.h"
#include "radio_harness.h"

TEST_CASE("reset: pulses the line, asks the version, and the chip is back in standby", "[radio][reset]")
{
    radio_open();
    uint8_t major = 0U;
    uint8_t minor = 0U;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_EQUAL_UINT8(0x01U, major);
    TEST_ASSERT_EQUAL_UINT8(0x18U, minor);
    TEST_ASSERT_EQUAL_UINT(1U, g_chip.n_resets);
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_GET_VERSION));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("reset: an empty header, where BUSY never falls, is a timeout and says why", "[radio][reset]")
{
    radio_open();
    g_chip.absent   = true;
    g_chip.creep_us = 20000U;   /* the driver's clock, fast enough to run out its wait */
    uint8_t major = 0U;
    uint8_t minor = 0U;

    TEST_ASSERT_EQUAL(KODE_TIMEOUT, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_TRUE(fake_log_seen("BUSY stayed high"));
    TEST_ASSERT_TRUE(mesh_drv_lr2021_busy());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
}

TEST_CASE("reset: a version of all zeros or all ones is nothing answering", "[radio][reset]")
{
    uint8_t major = 0U;
    uint8_t minor = 0U;

    radio_open();
    g_chip.fw_major = 0x00U;
    g_chip.fw_minor = 0x00U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_TRUE(fake_log_seen("nothing answers on MISO"));

    radio_open();
    g_chip.fw_major = 0xFFU;
    g_chip.fw_minor = 0xFFU;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_reset(&major, &minor));
}

TEST_CASE("alive: true for an LR2021's version, false for any other major", "[radio][reset]")
{
    radio_up_sub();
    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());
    g_chip.fw_major = 0x02U;
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    g_chip.fw_major = 0x01U;
    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_XOSC, g_chip.mode);   /* it did not disturb the setting */
}

TEST_CASE("reset: the driver forgets its setting with the chip, so nothing is sent until the next configure", "[radio][reset]")
{
    radio_up_sub();
    uint8_t major = 0U;
    uint8_t minor = 0U;
    uint8_t data[5];
    fill_pattern(data, sizeof(data));

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());   /* not a byte reached the chip */

    /* ...and the next configure puts the whole shape back, whatever the driver
     * thought the chip held before. */
    mesh_drv_lora_settings_t cfg;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9U, 850, &cfg));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_configure(&cfg));
    TEST_ASSERT_EQUAL_UINT8(255U, g_chip.payload_len);
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(8U, g_chip.pbl);
}

TEST_CASE("hold_reset: the chip goes silent, BUSY reads high, and a reset brings it back", "[radio][reset]")
{
    radio_up_sub();
    mesh_drv_lr2021_hold_reset();
    TEST_ASSERT_EQUAL_INT(0, pin_level(PIN_RST));
    TEST_ASSERT_TRUE(g_chip.in_reset);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_busy());
    g_chip.creep_us = 20000U;   /* the driver's clock fast, so its wait for BUSY ends in milliseconds */
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    g_chip.creep_us = 20U;

    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_FALSE(g_chip.in_reset);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());
}

TEST_CASE("hold_reset: closing the driver lets the line go, as the module's pull-up does", "[radio][reset]")
{
    radio_up_sub();
    mesh_drv_lr2021_hold_reset();
    TEST_ASSERT_TRUE(g_chip.in_reset);
    mesh_drv_lr2021_close();
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));
    TEST_ASSERT_FALSE(g_chip.in_reset);
}

/* ---- A restart --------------------------------------------------------------------------------- */

TEST_CASE("restart: the shutdown handler silences a carrier that is on air", "[radio][reset][carrier]")
{
    radio_up_sub();
    TEST_ASSERT_TRUE(mesh_drv_lr2021_guarded());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1U, shutdown_count());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 30000U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_TRUE(g_chip.test_cw);
    unsigned const standbys = chip_count(OPC_SET_STANDBY);

    shutdown_run();   /* esp_restart() runs these before the scheduler stops */

    TEST_ASSERT_EQUAL_INT(0, pin_level(PIN_RST));
    TEST_ASSERT_TRUE(g_chip.in_reset);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_FALSE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_UINT(standbys, chip_count(OPC_SET_STANDBY));   /* one pin, no bus: the owner may be mid-frame */
}

TEST_CASE("restart: the handler needs no open driver and does no harm to a closed one", "[radio][reset]")
{
    radio_open();
    mesh_drv_lr2021_close();
    shutdown_run();
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));   /* closed: not the driver's pin to hold */
}
