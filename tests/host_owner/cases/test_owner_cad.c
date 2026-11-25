/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_cad.c
 * @brief The owner's channel search, whose answer waits on the caller's task, and
 *        what it says as a packet comes in.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

/** @brief Search the channel and wait for the search to end, as mesh_radio_cad() does; @p seen is its result. */
static kode_status_t h_cad_call(uint32_t symbols, uint8_t fast, uint32_t *seen)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CAD, .u = symbols, .a = fast };

    return mesh_aos_call(&req, 2000U, seen, NULL);
}

/** @brief Start a search as an Active Object does: the answer is the search's end. */
static kode_status_t h_cad_ask(uint32_t symbols, uint8_t fast, h_answer_t *to)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CAD, .u = symbols, .a = fast };

    return h_ask(&req, to);
}

TEST_CASE("cad: an empty channel answers free, and the receiver is back on", "[owner][cad]")
{
    uint32_t seen = 7U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE;

    TEST_ASSERT_EQUAL(KODE_OK, h_cad_call(0U, 0U, &seen));
    TEST_ASSERT_EQUAL_UINT32(0U, seen);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_cad);
    TEST_ASSERT_EQUAL_UINT8(4U, g_lr.cad_symbols);   /* SF9's */
    TEST_ASSERT_GREATER_THAN_UINT(receives, g_lr.n_receive);
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_FALSE(h_status().cad_running);
}

TEST_CASE("cad: somebody talking answers busy", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_call(0U, 0U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
}

TEST_CASE("cad: a packet arriving answers busy at once, and its reception is left alone", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_PREAMBLE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));

    TEST_ASSERT_EQUAL(KODE_OK, h_cad_call(0U, 0U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_cad);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("cad: a chip that never says done is timed out, off air, and the receiver back", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.cad_finish = 0U;
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_cad_call(0U, 0U, &seen));
    TEST_ASSERT_FALSE(h_status().cad_running);
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_RECEIVE, 1000));
}

TEST_CASE("cad: a chip that refuses it answers so, and the receiver is back", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.cad_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_cad_call(0U, 0U, &seen));
    g_lr.cad_rc = KODE_OK;
    TEST_ASSERT_FALSE(h_status().cad_running);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("cad: an Active Object is answered when the search ends, with what it saw, and what it was given reaches the chip", "[owner][cad]")
{
    h_answer_t ans;

    harness_fresh();
    g_lr.cad_finish = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_ask(2U, 1U, &ans));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().cad_running, 1000));
    TEST_ASSERT_EQUAL_UINT8(2U, g_lr.cad_symbols);
    TEST_ASSERT_TRUE(g_lr.cad_fast);
    TEST_ASSERT_EQUAL_UINT(0U, ans.n);   /* not yet: the search is on */

    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED | MESH_DRV_LR2021_INTERRUPT_CAD_DONE);
    TEST_ASSERT_TRUE(h_answered(&ans, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT32(1U, ans.a.out1);
    TEST_ASSERT_TRUE(ans.in_owner);
    TEST_ASSERT_FALSE(h_status().cad_running);
    TEST_ASSERT_EQUAL_UINT(1U, ans.n);   /* exactly once */
}

TEST_CASE("cad: refused with something else going on, a send refused while it runs", "[owner][cad]")
{
    static const uint8_t DATA[] = { 1U, 2U };
    h_answer_t bad;
    h_answer_t running;
    h_answer_t second;

    harness_fresh();
    g_lr.cad_finish = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_ask(5U, 0U, &bad));    /* the table has four: refused in the answer */
    TEST_ASSERT_TRUE(h_answered(&bad, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, bad.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_BAD_ARGUMENT, bad.a.why);
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_ask(0U, 0U, &running));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().cad_running, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_ask(0U, 0U, &second)); /* one at a time */
    TEST_ASSERT_TRUE(h_answered(&second, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, second.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_BUSY_SEARCH, second.a.why);
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    TEST_ASSERT_EQUAL_UINT(0U, running.n);

    /* The search still running owes its answer to a variable of this case: end it here, while
     * the variable is alive, rather than at the next case's reset. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    TEST_ASSERT_TRUE(h_answered(&running, 1000U));
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_INTERRUPTED, running.a.why);
}

TEST_CASE("cad: a stop ends a search, and whoever waits for it is answered cut short", "[owner][cad]")
{
    h_answer_t ans;

    harness_fresh();
    g_lr.cad_finish = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_ask(4U, 0U, &ans));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().cad_running, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    TEST_ASSERT_TRUE(h_answered(&ans, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_INTERRUPTED, ans.a.why);
    TEST_ASSERT_FALSE(h_status().cad_running);
}

TEST_CASE("arrival: a preamble and a header are told in order, a packet ends it", "[owner][cad]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_PREAMBLE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PREAMBLE) == 1U, 1000));
    TEST_ASSERT_TRUE(h_status().arriving);

    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_HEADER) == 1U, 1000));
    TEST_ASSERT_TRUE(h_status().arriving);

    g_lr.rx_len = 3U;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PACKET) == 1U, 1000));
    TEST_ASSERT_FALSE(h_status().arriving);
}

TEST_CASE("arrival: a bad header says so and ends it", "[owner][cad]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_HEADER_BAD) == 1U, 1000));
    TEST_ASSERT_FALSE(h_status().arriving);
}

TEST_CASE("arrival: a preamble nothing comes of falls by itself", "[owner][cad]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_PREAMBLE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    /* SF9, BW125: 4.096 ms a symbol, 8 + 28 of them, and a margin. */
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().arriving, 2000));
    TEST_ASSERT_EQUAL(0, h_events(MESH_AOS_EVENT_PACKET));
}

TEST_CASE("cad: a receiver that does not come back after the search is said, and off", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE;
    g_lr.receive_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_call(0U, 0U, &seen));
    g_lr.receive_rc = KODE_OK;

    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_RECEIVER), 1000));
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("cad: a search while a packet arrives is answered busy at once, and the reception is left alone", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));

    TEST_ASSERT_EQUAL(KODE_OK, h_cad_call(0U, 0U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_cad);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_TRUE(h_status().arriving);
    TEST_ASSERT_FALSE(h_status().cad_running);
}

TEST_CASE("arrival: it falls when the radio leaves Rx for a send or a standby", "[owner][cad]")
{
    static const uint8_t DATA[] = { 1U, 2U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_FALSE(h_status().arriving);

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_FALSE(h_status().arriving);
}

TEST_CASE("arrival: a header whose packet is lost holds nothing for more than two seconds", "[owner][cad]")
{
    harness_fresh();
    /* SF12 at BW125: a 255-byte packet is ten seconds in the air. */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));

    uint32_t const t0 = kode_sys_time_ms();
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().arriving, 3500));
    uint32_t const held = kode_sys_time_ms() - t0;
    TEST_ASSERT_GREATER_THAN_UINT32(1500U, held);   /* it was believed, not dropped at once */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2200U, held);
}

/** @brief Search the channel and receive what it sees, as mesh_radio_cad_receive() does. */
static kode_status_t h_cad_receive_call(uint32_t symbols, uint32_t *seen)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CAD, .u = symbols, .a = MESH_AOS_CAD_RECEIVE };

    return mesh_aos_call(&req, 2000U, seen, NULL);
}

TEST_CASE("cad receive: a preamble seen leaves the chip in Rx with a packet on its way, told as a preamble", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;

    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(2U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_cad_receive);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_cad);
    TEST_ASSERT_EQUAL_UINT8(2U, g_lr.cad_symbols);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PREAMBLE) == 1U, 1000));
    TEST_ASSERT_TRUE(h_status().arriving);
    TEST_ASSERT_FALSE(h_status().cad_running);
    /* The chip is in Rx already: nothing starts it again over the packet. */
    TEST_ASSERT_EQUAL_UINT(receives, g_lr.n_receive);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    /* Its header, then the packet; the single receive falls back and the receiver comes back on. */
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_HEADER) == 1U, 1000));
    g_lr.rx_len = 3U;
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PACKET) == 1U, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_receive > receives, 1000));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_TRUE(h_status().receiving);
}

TEST_CASE("cad receive: nothing seen answers free, and the receiver is back on as after a plain search", "[owner][cad]")
{
    uint32_t seen = 7U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE;

    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(1U, &seen));
    TEST_ASSERT_EQUAL_UINT32(0U, seen);
    TEST_ASSERT_GREATER_THAN_UINT(receives, g_lr.n_receive);
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_FALSE(h_status().arriving);
    TEST_ASSERT_EQUAL(0, h_events(MESH_AOS_EVENT_PREAMBLE));
}

TEST_CASE("cad receive: a bad header after a preamble seen puts the receiver back too", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(0U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_HEADER_BAD) == 1U, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_receive > receives, 1000));
    TEST_ASSERT_FALSE(h_status().arriving);
}

TEST_CASE("cad receive: with the receiver off, a packet after a preamble seen leaves the chip out of Rx", "[owner][cad]")
{
    uint32_t seen = 0U;

    harness_fresh();
    TEST_ASSERT_FALSE(h_status().receiving);
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(2U, &seen));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PREAMBLE) == 1U, 1000));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    g_lr.rx_len = 3U;
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_PACKET) == 1U, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_STANDBY_RC, 1000));
    TEST_ASSERT_EQUAL_UINT(receives, g_lr.n_receive);   /* the receiver was off: not started */
    TEST_ASSERT_FALSE(h_status().receiving);
}

TEST_CASE("cad receive: a preamble seen with no packet after it does not leave the chip listening", "[owner][cad]")
{
    uint32_t seen = 0U;

    /* Receiver off: the chip must not stay in Rx once the packet is given up on. */
    harness_fresh();
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(1U, &seen));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().arriving, 3000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_STANDBY_RC, 1000));
    TEST_ASSERT_FALSE(h_status().receiving);

    /* Receiver on: it is put back, a continuous Rx again. */
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const receives = g_lr.n_receive;
    g_lr.cad_finish = MESH_DRV_LR2021_INTERRUPT_CAD_DONE | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED;
    TEST_ASSERT_EQUAL(KODE_OK, h_cad_receive_call(1U, &seen));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().arriving, 3000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_receive > receives, 1000));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_TRUE(h_status().receiving);
}
