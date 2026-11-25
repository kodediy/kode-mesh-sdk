/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_carrier.c
 * @brief A test signal never stays on air. The owner is held to it case by
 *        case: the deadline it enforces whatever the chip does, every way out
 *        that has to take the signal off air, a chip that will not confirm
 *        standby, and the limits on a request for one.
 *
 * The radio here is a model (fakes/fake_lr2021.c) that stays in Tx until
 * something ends it, so each case reads the chip's MODE - whether it is still
 * transmitting - and not how many calls were made.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

#define CARRIER   ((uint32_t)MESH_DRV_LR2021_TEST_CARRIER)
#define PREAMBLE  ((uint32_t)MESH_DRV_LR2021_TEST_PREAMBLE)

/* ---- The power the owner configures ------------------------------------------ */

TEST_CASE("power: any ask is held to the cap the chip is configured with (tx_power 19 / 13)", "[owner][power]")
{
    harness_fresh();
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_TRUE(h_status().radio_ready);

    /* 12.5 dBm on 868 and 5.5 dBm on 2.4 GHz are above the caps; both asks
     * come out at the caps' 19 and 13. */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1250));
    TEST_ASSERT_EQUAL_INT8(19, g_lr.cfg.tx_power);
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 550));
    TEST_ASSERT_EQUAL_INT8(13, g_lr.cfg.tx_power);
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL_INT8(19, g_lr.cfg.tx_power);
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 800));
    TEST_ASSERT_EQUAL_INT8(18, g_lr.cfg.tx_power);
}

/* ---- The deadline -------------------------------------------------------------- */

TEST_CASE("carrier: the owner's deadline takes it off air when the chip's own timer does not", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 100));
    TEST_ASSERT_TRUE(h_status().testing);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_TEST_CARRIER, g_lr.test_mode);
    TEST_ASSERT_EQUAL_UINT32(100U, g_lr.test_ms);        /* the driver was given the time */

    /* The test is 100 ms and the owner allows the chip's timer a quarter second
     * more: at 150 ms it is still up, and nothing has ended it. */
    vTaskDelay(pdMS_TO_TICKS(150));
    TEST_ASSERT_TRUE(h_status().testing);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    /* This model's chip ignores any timer. The owner ends it. */
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1500));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_TEST_END) == 1U, 500));
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_OWNER, h_event_arg(MESH_AOS_EVENT_TEST_END));
    TEST_ASSERT_TRUE(fake_log_seen("the owner's deadline"));
}

TEST_CASE("carrier: if the chip's own timer ended it, the owner says so and does not touch the chip", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 100));
    unsigned const standbys = g_lr.n_standby;

    vTaskDelay(pdMS_TO_TICKS(110));
    lr_fake_chip_stops(0U);   /* the default timer ended the carrier: the mode drops, no flag */

    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1500));
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_TEST_END) == 1U, 500));
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_CHIP, h_event_arg(MESH_AOS_EVENT_TEST_END));
    TEST_ASSERT_EQUAL_UINT(standbys, g_lr.n_standby);   /* nothing was left to take off air */
}

TEST_CASE("preamble: the chip's timeout flag ends the test at once, as the chip's doing", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(PREAMBLE, 100));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_TEST_PREAMBLE, g_lr.test_mode);

    vTaskDelay(pdMS_TO_TICKS(110));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_TIMEOUT);

    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 500));   /* far inside the 250 ms grace */
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_CHIP, h_event_arg(MESH_AOS_EVENT_TEST_END));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_RADIO_ERROR));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_LISTEN_END));   /* not a listen's timeout */
}

TEST_CASE("preamble: one that finishes before its time (the done flag) also ends the test, uncounted", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(PREAMBLE, 5000));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 500));
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_CHIP, h_event_arg(MESH_AOS_EVENT_TEST_END));
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_sent);       /* a test is not a send */
}

/* ---- Every way out ---------------------------------------------------------- */

typedef enum { VIA_STOP, VIA_STANDBY, VIA_END, VIA_BEGIN, VIA_RX_OFF } via_t;

static void leave_by(via_t how)
{
    switch (how) {
    case VIA_STOP:    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop()); break;
    case VIA_STANDBY: TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U)); break;
    case VIA_END:     TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U)); break;
    case VIA_BEGIN:   TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850)); break;
    case VIA_RX_OFF:  TEST_ASSERT_EQUAL(KODE_OK, h_receive(false)); break;
    }
}

static void signal_stops_on(uint32_t mode, via_t how)
{
    harness_fresh();
    /* A long test, so that only the way out can end it. */
    TEST_ASSERT_EQUAL(KODE_OK, h_test(mode, 30000));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    leave_by(how);

    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode != MESH_DRV_LR2021_MODE_TRANSMIT, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_hold);                       /* a plain standby was enough */
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_TEST_END));     /* and nobody's deadline */
}

TEST_CASE("carrier stops: on mesh_aos_stop", "[owner][carrier]")        { signal_stops_on(CARRIER, VIA_STOP); }
TEST_CASE("carrier stops: on radio standby", "[owner][carrier]")      { signal_stops_on(CARRIER, VIA_STANDBY); }
TEST_CASE("carrier stops: on radio end", "[owner][carrier]")          { signal_stops_on(CARRIER, VIA_END); }
TEST_CASE("carrier stops: on a new radio begin", "[owner][carrier]")  { signal_stops_on(CARRIER, VIA_BEGIN); }
TEST_CASE("carrier stops: on receive off", "[owner][carrier]")        { signal_stops_on(CARRIER, VIA_RX_OFF); }
TEST_CASE("preamble stops: on mesh_aos_stop", "[owner][carrier]")      { signal_stops_on(PREAMBLE, VIA_STOP); }
TEST_CASE("preamble stops: on radio standby", "[owner][carrier]")     { signal_stops_on(PREAMBLE, VIA_STANDBY); }
TEST_CASE("preamble stops: on radio end", "[owner][carrier]")         { signal_stops_on(PREAMBLE, VIA_END); }
TEST_CASE("preamble stops: on a new radio begin", "[owner][carrier]") { signal_stops_on(PREAMBLE, VIA_BEGIN); }
TEST_CASE("preamble stops: on receive off", "[owner][carrier]")       { signal_stops_on(PREAMBLE, VIA_RX_OFF); }

TEST_CASE("carrier stops: a test request with a time of 0 is the stop", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    TEST_ASSERT_EQUAL(KODE_OK, h_test(0U, 0));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 500));
}

TEST_CASE("carrier stops: nothing else can start while it is up", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    uint8_t d[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(d, 4, 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(PREAMBLE, 1000));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_test);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    (void)h_call(MESH_REQUEST_RADIO_STANDBY, 2000U);
}

/* ---- A chip that will not stand by ------------------------------------------ */

TEST_CASE("standby: one that fails twice is retried; the third takes it off air, no reset", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    g_lr.standby_fails = 2U;

    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(3U, g_lr.n_standby);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_reset);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_RADIO_ERROR));
}

TEST_CASE("standby: a chip that will not confirm it is reset, so it cannot stay keyed; the app is told", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    g_lr.standby_fails = 1000U;   /* it never confirms */

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(3U, g_lr.n_standby);          /* three tries */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_reset);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_hold);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);   /* the reset ended the carrier */
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_FALSE(h_status().radio_ready);              /* its setting is gone */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY));

    /* and the way back is a begin */
    g_lr.standby_fails = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("standby: a chip that will not even reset has its reset line held low until begin lets it go", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    g_lr.standby_fails = 1000U;
    g_lr.reset_fails   = true;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_hold);
    TEST_ASSERT_TRUE(g_lr.held);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);   /* in reset it is silent */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY));
    TEST_ASSERT_FALSE(h_status().radio_ready);

    /* A stop does not reset a chip that is already out of play. */
    unsigned const resets = g_lr.n_reset;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(resets, g_lr.n_reset);

    g_lr.standby_fails = 0U;
    g_lr.reset_fails   = false;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_FALSE(g_lr.held);                           /* begin reset it first */
    TEST_ASSERT_GREATER_THAN_UINT(resets, g_lr.n_reset);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("standby: a stop that the chip will not confirm also resets it", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 30000));
    g_lr.standby_fails = 1000U;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_reset == 1U, 1000));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_RADIO_ERROR) == 1U, 500));
}

TEST_CASE("standby: the deadline of a test, with a chip that will not stand by, resets it too", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 50));
    g_lr.standby_fails = 1000U;

    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_reset == 1U, 1500));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_TEST_END) == 1U, 500));
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_OWNER, h_event_arg(MESH_AOS_EVENT_TEST_END));
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY));
}

TEST_CASE("standby: a deadline that cannot read the chip's mode takes the radio off air to be safe", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 50));
    g_lr.mode_fails = true;
    unsigned const standbys = g_lr.n_standby;

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_TEST_END) == 1U, 1500));
    TEST_ASSERT_GREATER_THAN_UINT(standbys, g_lr.n_standby);
    TEST_ASSERT_EQUAL_UINT32(MESH_TEST_END_OWNER, h_event_arg(MESH_AOS_EVENT_TEST_END));
    g_lr.mode_fails = false;
}

/* ---- What may be asked for ---------------------------------------------------- */

TEST_CASE("test request: no time, too much, a bad mode never reach the chip", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, 0));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(PREAMBLE, -5));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, (int32_t)MESH_BSP_TEST_MAX_MS + 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, INT32_MAX));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(0x07U, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    TEST_ASSERT_FALSE(h_status().testing);

    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, (int32_t)MESH_BSP_TEST_MAX_MS));   /* the longest is allowed */
    TEST_ASSERT_EQUAL_UINT32(MESH_BSP_TEST_MAX_MS, g_lr.test_ms);
    (void)h_call(MESH_REQUEST_RADIO_STANDBY, 2000U);
}

TEST_CASE("test request: a carrier needs the restart hook; a preamble, which the chip bounds, does not", "[owner][carrier]")
{
    harness_fresh();
    g_lr.guarded = false;

    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_TRUE(fake_log_seen("no carrier test"));

    TEST_ASSERT_EQUAL(KODE_OK, h_test(PREAMBLE, 1000));
    TEST_ASSERT_TRUE(h_status().testing);
    (void)h_call(MESH_REQUEST_RADIO_STANDBY, 2000U);
}

TEST_CASE("test request: a build that keeps the region's limits has no test signal - refused by name, nothing keyed", "[owner][carrier]")
{
    harness_fresh();
    /* This suite's build is an app's, without CONFIG_MESH_LAB: the plan itself says no ... */
    TEST_ASSERT_FALSE(mesh_plan_test_allowed_as_built());
    /* ... and this is what the owner does when it hears that. */
    g_plan_test_allowed = false;

    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, 1000));
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_LAB_ONLY, mesh_aos_why());
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(PREAMBLE, 1000));
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_LAB_ONLY, mesh_aos_why());
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_TEST_END));

    /* The answer is the build's, not the moment's: with no radio setting it is the same one. */
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(CARRIER, 1000));
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_LAB_ONLY, mesh_aos_why());

    /* A send is not a test signal: the same build puts a packet on air. */
    static const uint8_t DATA[] = { 1U, 2U };
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
}

TEST_CASE("test request: a chip that will not go to Tx leaves nothing keyed and no test running", "[owner][carrier]")
{
    harness_fresh();
    g_lr.test_rc = KODE_IO_ERROR;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_test(CARRIER, 1000));
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_GREATER_THAN_UINT(0U, g_lr.n_standby);   /* it may have been part-way up */
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
}

TEST_CASE("test request: on the 2.4 GHz path the same bounds hold", "[owner][carrier]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_EQUAL_INT8(13, g_lr.cfg.tx_power);
    TEST_ASSERT_EQUAL(KODE_OK, h_test(CARRIER, 80));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1500));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
}
