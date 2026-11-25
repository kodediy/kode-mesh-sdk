/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_ask.c
 * @brief The Active Object's way into the owner (mesh_aos_ask()) for a send, the request
 *        whose answer is the longest wait: the done function runs once, on the owner's
 *        task, when the packet is gone; a refusal is an answer and not a return; the
 *        payload is copied before the call returns; and starting is safe from many tasks.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

static const uint8_t PING[5] = { 'p', 'i', 'n', 'g', '!' };

static kode_status_t ask_send(const uint8_t *data, uint8_t len, h_answer_t *to)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_SEND, .data = data, .len = len };

    return h_ask(&req, to);
}

TEST_CASE("ask send: the answer is the packet gone - once, on the owner's task, with its airtime", "[owner][ask]")
{
    h_answer_t ans;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, ask_send(PING, sizeof(PING), &ans));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().sending, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, ans.n);   /* on air: not answered yet */
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT(0U, ans.n);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(h_answered(&ans, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_NONE, ans.a.why);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, ans.a.out1);   /* its airtime, ms */
    TEST_ASSERT_TRUE(ans.in_owner);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT(1U, ans.n);   /* exactly once */
    TEST_ASSERT_FALSE(h_status().sending);
    TEST_ASSERT_EQUAL_UINT32(1U, h_status().packets_sent);
}

TEST_CASE("ask send: refused it is an answer, not a return - a bad payload, and busy", "[owner][ask]")
{
    h_answer_t first;
    h_answer_t second;
    h_answer_t bad;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, ask_send(NULL, 5U, &bad));   /* taken: it will be answered */
    TEST_ASSERT_TRUE(h_answered(&bad, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, bad.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_BAD_ARGUMENT, bad.a.why);
    TEST_ASSERT_TRUE(bad.in_owner);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);

    TEST_ASSERT_EQUAL(KODE_OK, ask_send(PING, sizeof(PING), &first));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().sending, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, ask_send(PING, sizeof(PING), &second));
    TEST_ASSERT_TRUE(h_answered(&second, 1000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, second.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_BUSY_SENDING, second.a.why);
    TEST_ASSERT_TRUE(second.in_owner);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
    TEST_ASSERT_EQUAL_UINT(0U, first.n);   /* the first is still on air */

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(h_answered(&first, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, first.a.rc);
}

TEST_CASE("ask send: no room in the duty budget is an answer that says how long to wait", "[owner][ask]")
{
    static uint8_t longest[MESH_AOS_PAYLOAD_MAX];
    h_answer_t ans;

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    kode_status_t rc = KODE_OK;
    for (unsigned n = 0U; (n < 10U) && (rc == KODE_OK); n++) {
        rc = h_send(longest, sizeof(longest), 2000U);
        if (rc == KODE_OK) {
            lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
            TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
        }
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);   /* the budget is spent */

    unsigned const sends = g_lr.n_send;
    TEST_ASSERT_EQUAL(KODE_OK, ask_send(longest, sizeof(longest), &ans));
    TEST_ASSERT_TRUE(h_answered(&ans, 1000U));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT8(MESH_WHY_DUTY, ans.a.why);
    TEST_ASSERT_GREATER_THAN_UINT32(1000U, ans.a.out1);   /* the wait, ms */
    TEST_ASSERT_EQUAL_UINT32(ans.a.out1, h_status().duty_wait_ms);
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);           /* and nothing went to the chip */
    TEST_ASSERT_EQUAL_UINT(1U, ans.n);
}

TEST_CASE("ask send: the payload is copied before the call returns, so the caller's buffer is free", "[owner][ask]")
{
    uint8_t mine[5];
    h_answer_t ans;

    harness_fresh();
    (void)memcpy(mine, PING, sizeof(mine));
    TEST_ASSERT_EQUAL(KODE_OK, ask_send(mine, sizeof(mine), &ans));
    (void)memset(mine, 0xAA, sizeof(mine));   /* reused at once */
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().sending, 1000));
    TEST_ASSERT_EQUAL_MEMORY(PING, g_lr.sent, sizeof(PING));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(h_answered(&ans, 1000U));
}

TEST_CASE("ask: a request without a done function, or without a request, is refused on the spot and posts nothing", "[owner][ask]")
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_STANDBY };
    h_answer_t ans = { 0 };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_ask(&req, &ans, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_ask(NULL, &ans, NULL));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_UINT(0U, ans.n);
}

static void start_body(void *arg)
{
    *(volatile kode_status_t *)arg = mesh_aos_start();
}

TEST_CASE("start: from four tasks at once it is one owner, one supply switch, and every caller is told OK", "[owner][ao]")
{
    harness_fresh();
    unsigned const on = g_exp.power_on_calls;
    volatile kode_status_t rc[4];
    h_bg_t bg[4];

    for (unsigned k = 0U; k < 4U; k++) {
        rc[k] = KODE_TIMEOUT;
        h_bg_start(&bg[k], start_body, (void *)&rc[k]);
    }
    for (unsigned k = 0U; k < 4U; k++) {
        TEST_ASSERT_TRUE(h_bg_join(&bg[k], 2000U));
        TEST_ASSERT_EQUAL(KODE_OK, rc[k]);
    }
    TEST_ASSERT_TRUE(mesh_aos_running());
    TEST_ASSERT_EQUAL_UINT(on, g_exp.power_on_calls);   /* the supply was switched on once, not again */
}
