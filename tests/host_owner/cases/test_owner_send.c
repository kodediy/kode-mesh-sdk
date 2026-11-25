/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_send.c
 * @brief A packet on air and everything that can go wrong with it: the chip's
 *        own timer, the owner's backstop behind it, a done flag nobody waits for,
 *        a command that fails half-way, and an amplifier fault that must keep the
 *        radio off until it is begun again.
 */
#include <stdint.h>

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

static const uint8_t DATA[5] = { 'p', 'i', 'n', 'g', '!' };

/* ---- A send ------------------------------------------------------------------- */

TEST_CASE("send: goes out from standby, and the chip is Tx until its done flag", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_GREATER_THAN_UINT(0U, g_lr.n_standby);   /* a send starts from standby */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
    TEST_ASSERT_EQUAL_UINT8(5U, g_lr.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(DATA, g_lr.sent, 5U);
    TEST_ASSERT_TRUE(h_status().sending);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_EQUAL_UINT32(1U, h_status().packets_sent);
    uint32_t air = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_send_answer(1000U, &air));   /* the call's answer is the packet gone, with its airtime */
    TEST_ASSERT_GREATER_THAN_UINT32(0U, air);
}

TEST_CASE("send: a second one while the first is on air is refused, not queued behind it", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send_again(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BUSY_SENDING, mesh_aos_why());
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
}

TEST_CASE("send: not over a test signal, and no test over a packet", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
}

TEST_CASE("send: a payload pointer of NULL with a length is refused, whatever the length", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(NULL, 5U, 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, 0U, 2000U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
}

TEST_CASE("send: a SetTx that fails leaves nothing on air, and the receiver comes back", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.send_rc = KODE_IO_ERROR;
    unsigned const standbys = g_lr.n_standby;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_FALSE(h_status().sending);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(standbys + 2U, g_lr.n_standby);   /* before, and again to be sure */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);                  /* listening again */
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_sent);
}

TEST_CASE("send: a chip that will not stand by gets nothing sent", "[owner][send]")
{
    harness_fresh();
    g_lr.standby_fails = 1000U;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_reset);   /* it is reset instead */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY));
    TEST_ASSERT_FALSE(h_status().radio_ready);
}

/* ---- The chip's timer and the owner's backstop ------------------------------- */

TEST_CASE("send: the chip's own timer running out frees the send and tells the app", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_TIMEOUT);

    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_SEND_TIMEOUT), 500));
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_sent);   /* it did not leave whole */
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send_answer(1000U, NULL));   /* it may not have gone whole: the call says so */
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_LISTEN_END));
}

TEST_CASE("send: its timer running out resumes a receiver that was on", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_TIMEOUT);

    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_RECEIVE, 1000));
    TEST_ASSERT_TRUE(h_status().receiving);
}

TEST_CASE("send: if neither a done flag nor the chip's timer comes, the backstop takes it off air", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    /* 5 bytes at SF9 is about 0.12 s on air; the backstop is that and a quarter
     * more, and 200 ms. Nothing else is raised. */
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 3000));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_SEND_TIMEOUT), 500));
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_sent);
}

TEST_CASE("send: a done flag nobody is waiting for is not a packet sent", "[owner][send]")
{
    harness_fresh();
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    vTaskDelay(pdMS_TO_TICKS(120));
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_sent);
}

TEST_CASE("send: the done flag counts once even if the chip raises it with a timeout", "[owner][send]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE | MESH_DRV_LR2021_INTERRUPT_TIMEOUT);

    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    vTaskDelay(pdMS_TO_TICKS(60));
    TEST_ASSERT_EQUAL_UINT32(1U, h_status().packets_sent);
    TEST_ASSERT_EQUAL(KODE_OK, h_send_answer(1000U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_RADIO_ERROR));
}

/* ---- The amplifier ------------------------------------------------------------- */

TEST_CASE("pa fault: the radio goes off air and stays off until it is begun again", "[owner][fault]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    unsigned const sends = g_lr.n_send;
    unsigned const tests = g_lr.n_test;
    g_lr.errors = 0x0040U;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);

    TEST_ASSERT_TRUE(WAIT_FOR(h_status().amplifier_fault, 1000));
    mesh_aos_status_t st = h_status();
    TEST_ASSERT_FALSE(st.radio_ready);
    TEST_ASSERT_FALSE(st.sending);
    TEST_ASSERT_FALSE(st.receiving);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);   /* off air */
    TEST_ASSERT_EQUAL_UINT16(0x0040U, st.radio_errors);
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_AMPLIFIER_FAULT | 0x0040U), 500));

    /* latched: nothing goes out, no test, no receive, until a begin */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test((uint32_t)MESH_DRV_LR2021_TEST_PREAMBLE, 100));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 100));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(true));
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);   /* none of them reached the chip */
    TEST_ASSERT_EQUAL_UINT(tests, g_lr.n_test);
    TEST_ASSERT_TRUE(h_status().amplifier_fault);

    /* the way back resets the chip first */
    g_lr.errors = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_reset);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_configure);
    st = h_status();
    TEST_ASSERT_FALSE(st.amplifier_fault);
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
}

TEST_CASE("pa fault: during a test signal it is off air at once, not at the deadline", "[owner][fault]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 30000));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);

    TEST_ASSERT_TRUE(WAIT_FOR(h_status().amplifier_fault, 1000));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_FALSE(h_status().testing);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_TEST_END));
}

TEST_CASE("pa fault: a chip that will not stand by after one is reset", "[owner][fault]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    g_lr.standby_fails = 1000U;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);

    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_reset >= 1U, 1000));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().amplifier_fault, 1000));
    g_lr.standby_fails = 0U;
}

TEST_CASE("chip error: an error flag alone is reported with the chip's word, and changes nothing else", "[owner][fault]")
{
    harness_fresh();
    g_lr.errors = 0x0102U;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_ERROR);

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_RADIO_ERROR) == 1U, 1000));
    TEST_ASSERT_EQUAL_UINT32(0x0102U, h_event_arg(MESH_AOS_EVENT_RADIO_ERROR));
    TEST_ASSERT_EQUAL_UINT16(0x0102U, h_status().radio_errors);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_FALSE(h_status().amplifier_fault);
    g_lr.errors = 0U;
}

TEST_CASE("send: a receiver that does not come back after the packet is said, and off", "[owner][send]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    g_lr.receive_rc = KODE_IO_ERROR;
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);

    TEST_ASSERT_EQUAL(KODE_OK, h_send_answer(1000U, NULL));
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_RECEIVER), 1000));
    g_lr.receive_rc = KODE_OK;
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

/* ---- The 868 band's hour --------------------------------------------------------- */

/** @brief A 255-byte packet, the way mesh_radio_send() asks for it; @p wait gets the milliseconds to wait. */
static kode_status_t send_longest(uint32_t *wait)
{
    static uint8_t longest[MESH_AOS_PAYLOAD_MAX];

    kode_status_t const rc = h_send(longest, sizeof(longest), 2000U);
    *wait = h_status().duty_wait_ms;
    return rc;
}

TEST_CASE("duty: the bucket is the owner's, so a module unplugged and plugged again does not refill it", "[owner][send]")
{
    uint32_t wait = 0U;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    /* Spend the hour: 255 bytes at SF12 and BW125 is several seconds of the 36. */
    kode_status_t rc = KODE_OK;
    for (unsigned n = 0U; (n < 10U) && (rc == KODE_OK); n++) {
        rc = send_longest(&wait);
        if (rc == KODE_OK) {
            lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
            TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
        }
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, wait);
    uint32_t const asked = wait;

    uint32_t const t0 = kode_sys_time_ms();
    h_module_away();
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 10000));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    uint32_t const away = kode_sys_time_ms() - t0;
    TEST_ASSERT_LESS_THAN_UINT32(asked, away);   /* the premise: the budget could not have refilled in that time */

    unsigned const sends = g_lr.n_send;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, send_longest(&wait));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, wait);
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);   /* and nothing went to the chip */
}
