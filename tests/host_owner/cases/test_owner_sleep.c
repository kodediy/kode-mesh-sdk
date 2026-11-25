/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_sleep.c
 * @brief The LR2021 put to sleep when nothing uses it: when it is found, when the
 *        app ends the radio and when the module is let go; never on a standby or a receiver turned
 *        off, which keep the setting; and begun again from it with the reset pulse.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

TEST_CASE("sleep: mesh_radio_end() puts the chip to sleep from a confirmed standby, and begin resets it awake", "[owner][sleep]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const standbys = g_lr.n_standby;

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));

    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_GREATER_THAN_UINT(standbys, g_lr.n_standby);   /* the standby confirmed first */
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_asleep);
    TEST_ASSERT_FALSE(st.radio_ready);
    TEST_ASSERT_FALSE(st.receiving);
    TEST_ASSERT_TRUE(st.present);

    /* A second end, a standby, a receiver off: nothing sent to a sleeping chip. */
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    vTaskDelay(pdMS_TO_TICKS(MESH_BSP_ALIVE_MS * 2U));   /* and the alive poll asks it through the driver */
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);
    TEST_ASSERT_TRUE(g_lr.asleep);

    unsigned const resets = g_lr.n_reset;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL_UINT(resets + 1U, g_lr.n_reset);
    TEST_ASSERT_FALSE(g_lr.asleep);
    TEST_ASSERT_FALSE(h_status().radio_asleep);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);
}

TEST_CASE("sleep: a standby and a receiver turned off keep the setting, and the chip awake", "[owner][sleep]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const sleeps = g_lr.n_sleep;
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(false));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(sleeps, g_lr.n_sleep);
    TEST_ASSERT_FALSE(g_lr.asleep);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));   /* straight back, no reset */
}

TEST_CASE("sleep: a chip that will neither confirm standby nor reset is held, not put to sleep, by end", "[owner][sleep]")
{
    harness_fresh();
    g_lr.standby_fails = 1000U;   /* a reset would cure it, and a cured chip may sleep: */
    g_lr.reset_fails   = true;    /* this one is not cured */
    unsigned const sleeps = g_lr.n_sleep;
    (void)h_call(MESH_REQUEST_RADIO_END, 2000U);
    TEST_ASSERT_EQUAL_UINT(sleeps, g_lr.n_sleep);
    TEST_ASSERT_FALSE(g_lr.asleep);
    TEST_ASSERT_TRUE(g_lr.held);
    TEST_ASSERT_FALSE(h_status().radio_asleep);
    g_lr.standby_fails = 0U;
    g_lr.reset_fails   = false;
}

TEST_CASE("sleep: a SetSleep that fails leaves the chip in standby, and begin works as before", "[owner][sleep]")
{
    harness_fresh();
    g_lr.sleep_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_FALSE(h_status().radio_asleep);
    g_lr.sleep_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("sleep: a module that is found sleeps until its radio is begun, and stays present", "[owner][sleep]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));   /* nothing for the return to put back */
    h_module_away();
    unsigned const sleeps = g_lr.n_sleep;
    unsigned const resets = g_lr.n_reset;
    h_module_back();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_sleep > sleeps, 1000U));
    TEST_ASSERT_GREATER_THAN_UINT(resets, g_lr.n_reset);   /* found: reset, its version asked, then asleep */
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_TRUE(h_status().radio_asleep);
    vTaskDelay(pdMS_TO_TICKS(MESH_BSP_ALIVE_MS * (MESH_BSP_GONE_MISSES + 1U)));
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_lr.n_asleep_calls, (g_lr.asleep_last != NULL) ? g_lr.asleep_last : "-");
}

TEST_CASE("sleep: mesh_end() lets go of a chip that confirms standby asleep, not held in reset", "[owner][sleep]")
{
    harness_fresh();
    unsigned const holds = g_lr.n_hold;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_EQUAL_UINT(holds, g_lr.n_hold);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
}

TEST_CASE("sleep: a chip the alive check left awake is put to sleep again, and the status says so", "[owner][sleep]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_TRUE(h_status().radio_asleep);
    unsigned const sleeps = g_lr.n_sleep;

    g_lr.alive_leaves_awake = true;
    TEST_ASSERT_TRUE(WAIT_FOR(!g_lr.alive_leaves_awake, MESH_BSP_ALIVE_MS * 3U));   /* the check ran */
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_sleep > sleeps, 1000U));
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(h_status().radio_asleep);
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);

    /* And begin still brings it back with the reset pulse. */
    unsigned const resets = g_lr.n_reset;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL_UINT(resets + 1U, g_lr.n_reset);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}
