/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_gnss.c
 * @brief The GNSS put to sleep instead of left running: mesh_gnss_end()
 *        sends the receiver the datasheet's Sleep command, begin wakes it with no reset, a
 *        rest sleeps it and the wake wakes it, mesh_end() sleeps it before the supply goes,
 *        and a receiver that cannot be told is closed as before.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

/** @brief The GNSS on, and talking at the ME31GR02's rate, so it may be told to sleep. */
static void gnss_on_talking(void)
{
    g_gnss.sleep_rc = KODE_OK;
    g_gnss.wake_rc  = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    g_gnss.report.talking = true;
    g_gnss.report.baud    = MESH_BSP_GNSS_BAUD;
    g_gnss.report.messages = 10U;
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().gnss_talking, 1000U));
}

/** @brief A board's AO_LowPower round, waited for. */
static void served_wait(void (*publish)(void))
{
    unsigned const served = g_ao.served;
    publish();
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.served > served, 1000U));
}

TEST_CASE("gnss: end puts a talking receiver to sleep, keeps the driver open, forgets the fix", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    unsigned const sleeps = g_gnss.sleeps;
    unsigned const closes = g_gnss.closes;

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_FALSE(st.gnss_on);
    TEST_ASSERT_TRUE(st.gnss_asleep);
    TEST_ASSERT_FALSE(st.gnss_talking);
    TEST_ASSERT_EQUAL_UINT32(0U, st.gnss_sentences);
    TEST_ASSERT_EQUAL_UINT(sleeps + 1U, g_gnss.sleeps);
    TEST_ASSERT_EQUAL_UINT(closes, g_gnss.closes);   /* not closed: its RESET stays released, its RXD idle */
    TEST_ASSERT_TRUE(g_gnss.is_open);
}

TEST_CASE("gnss: begin wakes a sleeping receiver with no reset and no reopening", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    unsigned const opens = g_gnss.opens;
    unsigned const wakes = g_gnss.wakes;
    unsigned const restarts = g_gnss.restarts;

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.gnss_on);
    TEST_ASSERT_FALSE(st.gnss_asleep);
    TEST_ASSERT_EQUAL_UINT(wakes + 1U, g_gnss.wakes);
    TEST_ASSERT_EQUAL_UINT(opens, g_gnss.opens);         /* open() pulses the reset line: not used */
    TEST_ASSERT_EQUAL_UINT(restarts, g_gnss.restarts);
    TEST_ASSERT_FALSE(g_gnss.asleep);

    /* Begin again while on is nothing more. */
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_EQUAL_UINT(wakes + 1U, g_gnss.wakes);
}

TEST_CASE("gnss: a receiver that never talked, or talks at the LC76F's rate, is closed as before", "[owner][gnss]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));   /* silent */
    unsigned closes = g_gnss.closes;
    unsigned const sleeps = g_gnss.sleeps;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_FALSE(h_status().gnss_on);
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_EQUAL_UINT(sleeps, g_gnss.sleeps);

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    g_gnss.report.talking = true;
    g_gnss.report.baud    = MESH_BSP_GNSS_BAUD_ALT;
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().gnss_talking, 1000U));
    closes = g_gnss.closes;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_EQUAL_UINT(sleeps, g_gnss.sleeps);

    /* Begin after a close opens it again, with its reset pulse, as always. */
    unsigned const opens = g_gnss.opens;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_EQUAL_UINT(opens + 1U, g_gnss.opens);
}

TEST_CASE("gnss: a command that would not go out closes the receiver instead", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    g_gnss.sleep_rc = KODE_IO_ERROR;
    unsigned const closes = g_gnss.closes;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_FALSE(g_gnss.is_open);
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    g_gnss.sleep_rc = KODE_OK;
}

TEST_CASE("gnss: a receiver that goes on talking after the command is closed, and begin opens it afresh", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    TEST_ASSERT_TRUE(h_status().gnss_asleep);
    unsigned const closes = g_gnss.closes;

    g_gnss.report.asleep = false;   /* the driver heard a sentence past its margin */
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().gnss_asleep, 1000U));
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_FALSE(g_gnss.is_open);

    unsigned const opens = g_gnss.opens;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_EQUAL_UINT(opens + 1U, g_gnss.opens);
    TEST_ASSERT_TRUE(h_status().gnss_on);
}

TEST_CASE("gnss: a rest puts it to sleep and says so, and the wake wakes it", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    h_events_clear();
    unsigned const sleeps = g_gnss.sleeps;
    unsigned const wakes  = g_gnss.wakes;

    served_wait(fake_ao_publish_rest);
    TEST_ASSERT_TRUE(h_status().gnss_asleep);
    TEST_ASSERT_FALSE(h_status().gnss_on);
    TEST_ASSERT_EQUAL_UINT(sleeps + 1U, g_gnss.sleeps);
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_SETTING_LOST,
                                  MESH_SETTING_RADIO | MESH_SETTING_GNSS));
    h_events_clear();

    served_wait(fake_ao_publish_awake);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().gnss_on, 1000U));
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_EQUAL_UINT(wakes + 1U, g_gnss.wakes);
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_SETTING_RESTORED,
                                  MESH_SETTING_RADIO | MESH_SETTING_GNSS));
}

TEST_CASE("gnss: a rest with the GNSS ended by the app does not wake it", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    unsigned const wakes = g_gnss.wakes;
    served_wait(fake_ao_publish_rest);
    served_wait(fake_ao_publish_awake);
    vTaskDelay(pdMS_TO_TICKS(100U));
    TEST_ASSERT_TRUE(h_status().gnss_asleep);
    TEST_ASSERT_FALSE(h_status().gnss_on);
    TEST_ASSERT_EQUAL_UINT(wakes, g_gnss.wakes);
}

TEST_CASE("gnss: mesh_end() sends the command before the supply goes, then closes", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    unsigned const sleeps = g_gnss.sleeps;
    unsigned const off    = g_exp.power_off_calls;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());

    TEST_ASSERT_EQUAL_UINT(sleeps + 1U, g_gnss.sleeps);
    TEST_ASSERT_FALSE(g_gnss.is_open);
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_EQUAL_UINT(off + 1U, g_exp.power_off_calls);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
}

TEST_CASE("gnss: a sleeping receiver whose module leaves is closed, and comes back closed", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    unsigned const closes = g_gnss.closes;

    h_module_away();
    TEST_ASSERT_FALSE(g_gnss.is_open);
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    h_module_back();
    TEST_ASSERT_FALSE(h_status().gnss_asleep);
    TEST_ASSERT_FALSE(h_status().gnss_on);
}

TEST_CASE("gnss: a wake whose bytes would not go out closes it, and the next begin opens it afresh", "[owner][gnss]")
{
    harness_fresh();
    gnss_on_talking();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    unsigned const closes = g_gnss.closes;
    unsigned const opens  = g_gnss.opens;

    g_gnss.wake_rc = KODE_IO_ERROR;
    TEST_ASSERT_NOT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    mesh_aos_status_t st = h_status();
    TEST_ASSERT_FALSE(st.gnss_on);
    TEST_ASSERT_FALSE(st.gnss_asleep);   /* not left "asleep" for the watch to call it ignored */
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_FALSE(g_gnss.is_open);

    g_gnss.wake_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    st = h_status();
    TEST_ASSERT_TRUE(st.gnss_on);
    TEST_ASSERT_EQUAL_UINT(opens + 1U, g_gnss.opens);
}
