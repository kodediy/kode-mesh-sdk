/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_end.c
 * @brief Letting go of the module (mesh_aos_end()): everything off, the lines back,
 *        J3's supply off, no more looking - and mesh_aos_start() takes it all up again.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

static volatile kode_status_t s_busy_rc = KODE_OK;

/** @brief Ask whether the channel is busy, and wait for the answer, on a task of its own. */
static void busy_waiter(void *arg)
{
    (void)arg;
    uint32_t seen = 0U;
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CAD };
    s_busy_rc = mesh_aos_call(&req, 2000U, &seen, NULL);
}

TEST_CASE("end: everything off, the lines back, the supply off, and nothing looked for", "[owner][end]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_TRUE(h_status().gnss_on);
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_TAG));
    unsigned const off = g_exp.power_off_calls;
    unsigned const closes = g_gnss.closes;
    h_events_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_FALSE(st.present);
    TEST_ASSERT_FALSE(st.radio_ready);
    TEST_ASSERT_FALSE(st.receiving);
    TEST_ASSERT_FALSE(st.gnss_on);
    TEST_ASSERT_EQUAL_UINT8(MESH_MODULE_NONE, st.seen);
    TEST_ASSERT_FALSE(g_gnss.is_open);
    TEST_ASSERT_EQUAL_UINT(closes + 1U, g_gnss.closes);
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_TAG));
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_PAIR));
    TEST_ASSERT_EQUAL_UINT(off + 1U, g_exp.power_off_calls);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_LOST));   /* the app asked for it */

    /* Nothing is looked for: no lend of the fingerprint lines in three looks. */
    unsigned const lends = g_exp.lends_total;
    vTaskDelay(pdMS_TO_TICKS((3U * MESH_BSP_LOOK_MS) + 200U));
    TEST_ASSERT_EQUAL_UINT(lends, g_exp.lends_total);
    TEST_ASSERT_FALSE(h_status().present);

    /* Calls are refused, with the reason; ending again is nothing. */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NOT_STARTED, mesh_aos_why());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_wait_present(100U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_EQUAL_UINT(off + 1U, g_exp.power_off_calls);   /* and the supply is not switched twice */

    /* Start again: the supply on, the module found as the first time, nothing put back. */
    unsigned const on = g_exp.power_on_calls;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_TRUE(WAIT_FOR(g_exp.power_on_calls == on + 1U, 1000U));   /* asked at the first tick */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_FALSE(h_status().radio_ready);   /* the old setting was forgotten */
    TEST_ASSERT_FALSE(h_status().gnss_on);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("end: with the module out it still lets go, and the looking stops", "[owner][end]")
{
    harness_fresh();
    h_module_away();
    unsigned const off = g_exp.power_off_calls;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_EQUAL_UINT(off + 1U, g_exp.power_off_calls);
    unsigned const lends = g_exp.lends_total;
    vTaskDelay(pdMS_TO_TICKS((3U * MESH_BSP_LOOK_MS) + 200U));
    TEST_ASSERT_EQUAL_UINT(lends, g_exp.lends_total);

    h_module_back();   /* it is plugged in again, and nobody is looking */
    vTaskDelay(pdMS_TO_TICKS((2U * MESH_BSP_LOOK_MS) + 200U));
    TEST_ASSERT_FALSE(h_status().present);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(h_status().present);
}

TEST_CASE("end: from an Active Object it is denied, and nothing is let go", "[owner][end]")
{
    harness_fresh();
    unsigned const off = g_exp.power_off_calls;
    fake_ao_become_app_ao();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_end());
    fake_ao_reset();
    TEST_ASSERT_EQUAL_UINT(off, g_exp.power_off_calls);
    TEST_ASSERT_TRUE(h_status().present);
}

TEST_CASE("end: a search that is waited for is answered, not left hanging", "[owner][end]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    h_bg_t bg;
    h_bg_start(&bg, busy_waiter, NULL);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().cad_running, 2000));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_TRUE(h_bg_join(&bg, 3000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, s_busy_rc);
    TEST_ASSERT_FALSE(h_status().cad_running);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
}

TEST_CASE("end: letting go and starting again does not refill the duty budget", "[owner][end]")
{
    static uint8_t longest[MESH_AOS_PAYLOAD_MAX];

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    kode_status_t rc = KODE_OK;
    for (unsigned n = 0U; (n < 10U) && (rc == KODE_OK); n++) {
        rc = h_send(longest, sizeof(longest), 2000U);   /* a send waits for its packet: the case lets it go */
        if (rc == KODE_OK) {
            lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
            TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
        }
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));

    unsigned const sends = g_lr.n_send;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, h_send(longest, sizeof(longest), 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_DUTY, mesh_aos_why());
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);
    fake_clock_skip(3600000U);   /* an hour passes: the budget this case spent is back for the next */
}

/* ---- An end and a start from two tasks ----------------------------------------- */

static h_bg_t                 s_starter;
static volatile kode_status_t s_start_rc;
static volatile unsigned      s_starts_in_end;

static void start_body(void *arg)
{
    (void)arg;
    s_start_rc = mesh_aos_start();
}

/** @brief The end is switching J3's supply off: another task starts the owner right then. */
static void start_during_end(bool on)
{
    if (on || (s_starts_in_end > 0U)) {
        return;
    }
    s_starts_in_end++;
    h_bg_start(&s_starter, start_body, NULL);
    (void)h_bg_join(&s_starter, 1000U);
}

TEST_CASE("end: a start from another task while the end is finishing is not undone by it", "[owner][end]")
{
    harness_fresh();
    s_start_rc      = KODE_ERROR;
    s_starts_in_end = 0U;
    unsigned const on = g_exp.power_on_calls;
    g_exp.power_hook = start_during_end;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    g_exp.power_hook = NULL;
    TEST_ASSERT_EQUAL_UINT(1U, s_starts_in_end);
    TEST_ASSERT_EQUAL(KODE_OK, s_start_rc);
    TEST_ASSERT_TRUE(mesh_aos_running());

    /* The start came last, so it holds: the supply asked on again and the module found. */
    TEST_ASSERT_TRUE(WAIT_FOR(g_exp.power_on_calls > on, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
}

static void end_body(void *arg)
{
    (void)arg;
    (void)mesh_aos_end();
}

TEST_CASE("end: an end and a start back to back from two tasks, many times, never leave a start undone", "[owner][end]")
{
    harness_fresh();
    for (unsigned n = 0U; n < 30U; n++) {
        h_bg_t ender;
        h_bg_start(&ender, end_body, NULL);
        vTaskDelay((TickType_t)(n % 3U));
        (void)mesh_aos_start();   /* while the end may still be on its way: either may be last */
        TEST_ASSERT_TRUE(h_bg_join(&ender, 3000U));
        /* The end last: not running, and a start now takes it up. The start last: running, and
         * the module is looked for without another start - never "running" and not looking. */
        if (!mesh_aos_running()) {
            TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
        }
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_wait_present(5000U), "started, but the module is not looked for");
    }
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
}

/** @brief The owner held inside a send it was posted, so that what is posted next waits behind it. */
static void owner_held(void)
{
    static const uint8_t d[4] = { 1, 2, 3, 4 };
    const mesh_aos_request_t send = { .kind = MESH_REQUEST_RADIO_SEND, .data = d, .len = sizeof(d) };

    lr_fake_gate(LR_GATE_SEND);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&send));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.blocked, 1000));
}

TEST_CASE("end: posted, then a start at once, as an Active Object does them: the start holds", "[owner][end]")
{
    harness_fresh();
    owner_held();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end_post());   /* mesh_sys_end() */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());      /* mesh_sys_begin(), before the end is served */
    lr_fake_release();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(mesh_aos_running());
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
}

TEST_CASE("end: two posted, then a start: the start is last, and the owner says it runs", "[owner][end]")
{
    harness_fresh();
    owner_held();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end_post());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end_post());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    lr_fake_release();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(mesh_aos_running());
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));

    /* and an end now lets go for real: the supply off, the module no longer held */
    unsigned const off = g_exp.power_off_calls;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_EQUAL_UINT(off + 1U, g_exp.power_off_calls);
    TEST_ASSERT_FALSE(mesh_aos_running());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
}

TEST_CASE("start: the owner's own request is not the app's to make", "[owner][end]")
{
    harness_fresh();
    const mesh_aos_request_t start = { .kind = MESH_REQUEST_START };
    h_answer_t to;

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_post(&start));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BAD_ARGUMENT, mesh_aos_why());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_call(&start, 1000U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BAD_ARGUMENT, mesh_aos_why());
    TEST_ASSERT_EQUAL(KODE_DENIED, h_ask(&start, &to));
    TEST_ASSERT_TRUE(h_status().present);   /* and nothing was started over */
}
