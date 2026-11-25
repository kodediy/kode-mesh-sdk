/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_sleep.c
 * @brief The chip put to sleep: SetSleep on the wire, nothing else sent to a
 *        sleeping chip, the alive check that wakes it for its question and puts it back, and the
 *        reset that brings a setting back.
 */
#include <stdint.h>

#include "unity.h"

#include "radio_harness.h"

TEST_CASE("sleep: SetSleep cold, no retention and no 32 kHz clock, and nothing waits on BUSY after it", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());

    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_SET_SLEEP));
    const chip_cmd_t *const c = chip_last(OPC_SET_SLEEP);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT(5U, c->n);
    TEST_ASSERT_EQUAL_HEX8(0x00U, c->p[0]);                     /* sleep_config: cold */
    TEST_ASSERT_EQUAL_HEX8(0x00U, c->p[1] | c->p[2] | c->p[3] | c->p[4]);   /* sleep_time 0 */
    TEST_ASSERT_TRUE(g_chip.asleep);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_asleep());
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));   /* its reset line high */
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_CS));    /* and its chip select: a fall would wake it */
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);

    /* Asleep again is nothing more. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_SET_SLEEP));
}

TEST_CASE("sleep: a sleeping chip is sent nothing; calls are refused at once, not after a BUSY wait", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());
    unsigned const frames = g_hw.nss_frames;

    mesh_drv_lr2021_mode_t m;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_mode(&m));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_EQUAL_UINT(frames, g_hw.nss_frames);   /* not a single chip select */
    TEST_ASSERT_TRUE(g_chip.asleep);
    TEST_ASSERT_FALSE(fake_log_seen("BUSY stayed high"));
}

TEST_CASE("sleep: the alive check wakes it with NSS, asks the version, and puts it back to sleep", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());

    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());

    TEST_ASSERT_EQUAL_UINT(1U, g_chip.n_wakes);
    TEST_ASSERT_EQUAL_UINT(2U, chip_count(OPC_SET_SLEEP));
    TEST_ASSERT_TRUE(g_chip.asleep);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_asleep());

    /* Pulled out while asleep: nothing lets BUSY fall, so the check says it is gone. */
    g_chip.absent   = true;
    g_chip.creep_us = 20000U;
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    g_chip.absent   = false;
    g_chip.creep_us = 0U;
}

TEST_CASE("sleep: the reset pulse wakes it cold, and a setting goes on it as after any reset", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());
    uint8_t major = 0U;
    uint8_t minor = 0U;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_FALSE(g_chip.asleep);
    TEST_ASSERT_FALSE(mesh_drv_lr2021_asleep());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);

    radio_up_sub();   /* configures and calibrates as on a fresh chip */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("sleep: holding the reset line ends the sleep, and a closed driver refuses", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());
    mesh_drv_lr2021_hold_reset();
    TEST_ASSERT_FALSE(mesh_drv_lr2021_asleep());
    TEST_ASSERT_FALSE(g_chip.asleep);
    TEST_ASSERT_TRUE(g_chip.in_reset);
    mesh_drv_lr2021_close();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_sleep());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_asleep());
}

TEST_CASE("sleep: a wake that did not take leaves it asleep, and the next check pulses NSS again", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());

    /* A contact that bounced: the pulse is lost and BUSY stays high. */
    g_chip.absent   = true;
    g_chip.creep_us = 20000U;
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    g_chip.absent   = false;
    g_chip.creep_us = 0U;
    TEST_ASSERT_TRUE(g_chip.asleep);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_asleep());   /* still known asleep */

    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());     /* woken this time, asked, put back */
    TEST_ASSERT_EQUAL_UINT(1U, g_chip.n_wakes);
    TEST_ASSERT_TRUE(g_chip.asleep);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_asleep());
}

TEST_CASE("sleep: a wrong answer after the wake still puts the chip back to sleep", "[radio][sleep]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_sleep());
    uint8_t const major = g_chip.fw_major;

    g_chip.fw_major = (uint8_t)(major + 1U);
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    g_chip.fw_major = major;
    TEST_ASSERT_EQUAL_UINT(1U, g_chip.n_wakes);
    TEST_ASSERT_TRUE(g_chip.asleep);              /* not left awake at 1.21 mA */
    TEST_ASSERT_TRUE(mesh_drv_lr2021_asleep());
}
