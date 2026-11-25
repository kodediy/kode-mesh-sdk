/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_stall.c
 * @brief An owner that was stalled for longer than a second - a slow call on
 *        its task - and what its radio's countdowns do when it gets back.
 *
 * The look and the alive poll count in steps and are clamped, so a stall does
 * not make them skip ahead. A send's backstop, a listen and a test signal's
 * deadline are wall time, and are not: the first tick after a stall must end
 * what ran out during it, not leave a carrier up for another second a tick.
 *
 * The fake Active Object holds one tick back (g_ao.stall_ms) and then stops
 * ticking, so what a case sees is what that one late tick did.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

/** @brief Let the next tick come late by @p stall_ms. */
static void stall_next_tick(unsigned stall_ms)
{
    g_ao.stall_ms = stall_ms;
    g_ao.tick_on  = true;
}

TEST_CASE("stall: a test signal whose deadline passed during a long stall is ended by the first tick after it", "[owner][stall]")
{
    harness_fresh();
    g_ao.tick_on = false;   /* nothing ends it while the case is arranged */
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 1000));
    TEST_ASSERT_TRUE(h_status().testing);

    /* The deadline is 1000 ms and the grace after it, 1250 ms; the stall is
     * longer than that and longer than the 1000 ms a step may count for. */
    stall_next_tick(1600U);
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.ticks >= 1U, 4000U));

    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);   /* and it is off air, not only unmarked */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_TEST_END, MESH_TEST_END_OWNER));
}

TEST_CASE("stall: a stall shorter than the test's deadline leaves it on air", "[owner][stall]")
{
    harness_fresh();
    g_ao.tick_on = false;
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 5000));

    stall_next_tick(1600U);
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.ticks >= 1U, 4000U));

    TEST_ASSERT_TRUE(h_status().testing);                              /* not cut short by the stall */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_TEST_END));
}

TEST_CASE("stall: a timed listen that ran out during a long stall is over at the first tick after it", "[owner][stall]")
{
    harness_fresh();
    g_ao.tick_on = false;
    mesh_aos_request_t const request = { .kind = MESH_REQUEST_RADIO_LISTEN, .u = 1000U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&request, 2000U, NULL, NULL));
    TEST_ASSERT_TRUE(g_lr.n_listen >= 1U);

    stall_next_tick(1700U);   /* the listen and 200 ms are 1200 ms; a step would count 1000 */
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.ticks >= 1U, 4000U));

    TEST_ASSERT_EQUAL_UINT(1U, h_events(MESH_AOS_EVENT_LISTEN_END));
}

TEST_CASE("stall: a send whose backstop ran out during a long stall is taken off air at the first tick after it", "[owner][stall]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));   /* SF12: a packet that takes its time */
    g_ao.tick_on = false;

    uint8_t data[20] = { 0 };
    TEST_ASSERT_EQUAL(KODE_OK, h_send(data, sizeof(data), 2000U));   /* on air; the call waits for the packet */
    TEST_ASSERT_TRUE(h_status().sending);

    /* The backstop is the airtime and a quarter more, and 200 ms - about 2 s for these 20 bytes at
     * SF12 - and the stall is far longer: more than one clamped step, or the case means nothing. */
    uint32_t const backstop_ms = 4600U;
    TEST_ASSERT_GREATER_THAN_UINT32(1100U, backstop_ms);

    stall_next_tick(backstop_ms + 400U);
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.ticks >= 1U, backstop_ms + 4000U));

    TEST_ASSERT_FALSE(h_status().sending);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_SEND_TIMEOUT));
}
