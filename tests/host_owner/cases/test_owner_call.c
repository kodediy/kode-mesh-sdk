/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_call.c
 * @brief A caller that gives up: what it leaves behind. A request the owner had
 *        not begun is withdrawn, so a retry cannot make the same packet twice;
 *        one the owner had begun is waited for and answered with what it did;
 *        and the bytes of a payload are the request's own, so the caller may
 *        reuse its buffer the moment the call returns.
 *
 * To hold the owner in the middle of something, the radio fake stands still at
 * a gate (fakes/fake_lr2021.c): a begin posted without a caller waiting keeps
 * the owner inside configure, with nobody holding the callers' lock, so the
 * cases can queue and abandon requests behind it.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

/* ---- Where a call that waits is not made ------------------------------------ */

static volatile kode_status_t s_hook_rc = KODE_OK;
static volatile bool          s_hook_ran;

/** @brief What an app's callback must not do: wait for the owner it runs on. */
static void hook_calls_owner(uint8_t ev, uint32_t arg)
{
    (void)arg;
    if (ev == MESH_AOS_EVENT_PREAMBLE) {
        s_hook_rc  = h_call(MESH_REQUEST_RADIO_STANDBY, 2000U);
        s_hook_ran = true;
    }
}

TEST_CASE("call: from an app's Active Object it is denied at once, and nothing is asked", "[owner][call]")
{
    static const uint8_t DATA[] = { 1U, 2U };

    harness_fresh();
    unsigned const posts  = g_ao.posts;
    unsigned const begins = g_lr.n_configure;
    fake_ao_become_app_ao();

    uint32_t const t0 = kode_sys_time_ms();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_wait_present(5000U));
    TEST_ASSERT_LESS_THAN_UINT32(100U, kode_sys_time_ms() - t0);   /* it did not wait */
    TEST_ASSERT_EQUAL_UINT(posts, g_ao.posts);
    TEST_ASSERT_EQUAL_UINT(begins, g_lr.n_configure);
    TEST_ASSERT_TRUE(fake_log_seen("made from an Active Object"));

    /* The same task, an ordinary one again (a worker is): the same call goes through. */
    fake_ao_reset();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_EQUAL_UINT(begins + 1U, g_lr.n_configure);
}

TEST_CASE("call: from the app's event callback, which runs on the owner, it is denied too", "[owner][call]")
{
    harness_fresh();
    s_hook_ran = false;
    s_hook_rc  = KODE_OK;
    g_event_hook = hook_calls_owner;
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_PREAMBLE);
    TEST_ASSERT_TRUE(WAIT_FOR(s_hook_ran, 2000));
    g_event_hook = NULL;
    TEST_ASSERT_EQUAL(KODE_DENIED, s_hook_rc);
    TEST_ASSERT_TRUE(h_status().receiving);   /* the standby it asked for did not happen */
}

static const mesh_aos_request_t BEGIN_REQ = {
    .kind = MESH_REQUEST_RADIO_BEGIN, .a = (uint8_t)MESH_PLAN_BAND_SUB_GHZ, .u = 9U, .i = 850,
};

/** @brief The owner stands still inside configure(), nobody waiting on it. */
static void owner_stuck_in_begin(void)
{
    lr_fake_gate(LR_GATE_CONFIGURE);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&BEGIN_REQ));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.blocked, 1000));
}

/** @brief Let it go, and wait until it has worked through everything queued. */
static void owner_released_and_drained(void)
{
    lr_fake_release();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_I2C_RETURN, 3000U));   /* a barrier: queued behind the rest */
}

static void releaser(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    lr_fake_release();
}

/* ---- Withdrawn ----------------------------------------------------------------- */

TEST_CASE("call: a request the owner never began is withdrawn - timeout, and the retry sends once", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t d[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    /* Two callers give up in turn, the older one's request still queued when
     * the newer gives up. A single "cancelled" slot would forget the older. */
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_send(d, 8U, 100U));
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_send(d, 8U, 100U));

    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);   /* neither of them ever ran */

    /* the retry goes once */
    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 8U, 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
}

TEST_CASE("call: callers that give up behind a stuck owner; none runs, the next does", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    /* A withdrawn request still holds its payload slot until the owner skips
     * it (test_owner_slots.c), so of three that give up in turn the third
     * finds both slots taken and is refused before it waits at all. */
    uint8_t d[4] = { 9, 9, 9, 9 };
    for (unsigned i = 0U; i < 2U; i++) {
        TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_send(d, 4U, 60U));
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, h_send(d, 4U, 60U));
    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 4U, 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
}

TEST_CASE("call: a withdrawn request of any other kind does not run either", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    const mesh_aos_request_t test = {
        .kind = MESH_REQUEST_RADIO_TEST, .u = (uint32_t)MESH_DRV_LR2021_TEST_PREAMBLE, .i = 1000,
    };
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, mesh_aos_call(&test, 100U, NULL, NULL));
    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    TEST_ASSERT_FALSE(h_status().testing);
}

TEST_CASE("call: a request that waits behind a stuck owner and is answered in time is not withdrawn", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    h_bg_t rel;
    h_bg_start(&rel, releaser, (void *)(uintptr_t)200U);   /* let it go in 200 ms */
    uint8_t d[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 4U, 2000U));        /* waits it out */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
    TEST_ASSERT_TRUE(h_bg_join(&rel, 1000U));
}

/* ---- Begun ----------------------------------------------------------------------- */

TEST_CASE("call: a request the owner had begun is waited for and answered with what it did", "[owner][call]")
{
    harness_fresh();
    lr_fake_gate(LR_GATE_SEND);
    h_bg_t rel;
    h_bg_start(&rel, releaser, (void *)(uintptr_t)400U);   /* long after the caller's 100 ms */

    uint8_t d[4] = { 1, 2, 3, 4 };
    kode_status_t const rc = h_send(d, 4U, 100U);          /* the owner is inside send() by then */
    TEST_ASSERT_EQUAL(KODE_OK, rc);                        /* not a timeout: it went */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
    TEST_ASSERT_TRUE(h_status().sending);

    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);               /* and nothing sends it again */
    TEST_ASSERT_TRUE(h_bg_join(&rel, 1000U));
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
}

TEST_CASE("call: a request the owner began and that failed answers with the failure, not a timeout", "[owner][call]")
{
    harness_fresh();
    g_lr.send_rc = KODE_IO_ERROR;
    lr_fake_gate(LR_GATE_SEND);
    h_bg_t rel;
    h_bg_start(&rel, releaser, (void *)(uintptr_t)400U);

    uint8_t d[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send(d, 4U, 100U));
    TEST_ASSERT_TRUE(h_bg_join(&rel, 1000U));
}

TEST_CASE("call: after one that gave up in flight, the next call gets its own answer", "[owner][call]")
{
    harness_fresh();
    lr_fake_gate(LR_GATE_SEND);
    h_bg_t rel;
    h_bg_start(&rel, releaser, (void *)(uintptr_t)300U);
    uint8_t d[4] = { 5, 5, 5, 5 };
    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 4U, 100U));
    TEST_ASSERT_TRUE(h_bg_join(&rel, 1000U));

    /* a different call gets a different answer: no stale one leaks across */
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(0x07U, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
}

TEST_CASE("call: one the owner began and is still stuck in, past the grace, is a timeout that says nothing - and leaves no stale answer", "[owner][call][slow]")
{
    harness_fresh();
    lr_fake_gate(LR_GATE_SEND);

    uint8_t d[4] = { 1, 2, 3, 4 };
    uint32_t const t0 = kode_sys_time_ms();
    TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_send(d, 4U, 100U));   /* began, stuck, and the grace runs out */
    uint32_t const waited = kode_sys_time_ms() - t0;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MESH_AOS_CALL_GRACE_MS, waited);   /* it did wait the grace */
    TEST_ASSERT_LESS_THAN_UINT32(MESH_AOS_CALL_GRACE_MS + 1500U, waited);
    TEST_ASSERT_TRUE(fake_log_seen("whether it happened is not known"));

    /* The owner gets going again and finishes the send; its late answer is for a
     * call that has gone, and the next call must not take it for its own. */
    lr_fake_release();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().sending, 1000));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(0x07U, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send);
}

/* ---- The answer's two numbers are the call's own -------------------------------- */

static struct {
    volatile kode_status_t rc;
    volatile uint32_t      scl;
    volatile uint32_t      sda;
} s_lend;

/** @brief A task that asks for the I2C pair and waits: the call's two numbers are its GPIOs. */
static void lend_caller(void *arg)
{
    (void)arg;
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_I2C_LEND };
    uint32_t scl = 0U;
    uint32_t sda = 0U;
    kode_status_t const rc = mesh_aos_call(&req, 3000U, &scl, &sda);
    s_lend.scl = scl;
    s_lend.sda = sda;
    s_lend.rc  = rc;
}

TEST_CASE("call: its two numbers are the answer's own, whatever the owner serves right behind it", "[owner][call]")
{
    harness_fresh();
    const mesh_aos_request_t lend = { .kind = MESH_REQUEST_I2C_LEND };
    uint32_t scl = 0U;
    uint32_t sda = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&lend, 2000U, &scl, &sda));   /* with nothing behind it */
    TEST_ASSERT_NOT_EQUAL(0U, scl);
    TEST_ASSERT_NOT_EQUAL(0U, sda);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_I2C_RETURN, 2000U));

    /* Behind a stuck owner: the task's call, and right after it an Active Object's request. Let
     * go, the owner serves the two in a row, on its higher priority, before the caller's task
     * runs again - and as it begins the second it clears the numbers it works with. */
    owner_stuck_in_begin();
    s_lend.rc  = KODE_TIMEOUT;
    s_lend.scl = 0U;
    s_lend.sda = 0U;
    unsigned const posts = g_ao.posts;
    h_bg_t caller;
    h_bg_start(&caller, lend_caller, NULL);
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.posts == (posts + 1U), 1000U));
    const mesh_aos_request_t standby = { .kind = MESH_REQUEST_RADIO_STANDBY };
    h_answer_t behind;
    TEST_ASSERT_EQUAL(KODE_OK, h_ask(&standby, &behind));
    lr_fake_release();

    TEST_ASSERT_TRUE(h_bg_join(&caller, 4000U));
    TEST_ASSERT_TRUE(h_answered(&behind, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, s_lend.rc);
    TEST_ASSERT_EQUAL_UINT32(scl, s_lend.scl);
    TEST_ASSERT_EQUAL_UINT32(sda, s_lend.sda);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_I2C_RETURN, 2000U));
}

/* ---- The payload is the request's own ------------------------------------------- */

TEST_CASE("call: the payload is copied into the request - the caller's buffer is free once it returns", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t buf[8] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };
    const mesh_aos_request_t send = { .kind = MESH_REQUEST_RADIO_SEND, .data = buf, .len = 8U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&send));   /* queued behind the begin, not run yet */
    (void)memset(buf, 0xEE, sizeof(buf));              /* the caller reuses its buffer at once */

    lr_fake_release();
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_send == 1U, 2000));
    TEST_ASSERT_EQUAL_UINT8(8U, g_lr.sent_len);
    TEST_ASSERT_EQUAL_MEMORY("ABCDEFGH", g_lr.sent, 8U);   /* what was sent is what was handed in */
}

/* ---- A stop reaches what was queued before it ----------------------------------- */

TEST_CASE("stop: a send queued before a stop is dropped, not sent after it", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t d[4] = { 1, 2, 3, 4 };
    const mesh_aos_request_t send = { .kind = MESH_REQUEST_RADIO_SEND, .data = d, .len = 4U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&send));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());

    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
}

TEST_CASE("stop: a test signal queued before a stop is dropped, not started after it", "[owner][call]")
{
    harness_fresh();
    owner_stuck_in_begin();

    const mesh_aos_request_t test = {
        .kind = MESH_REQUEST_RADIO_TEST, .u = (uint32_t)MESH_DRV_LR2021_TEST_CARRIER, .i = 30000,
    };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&test));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());

    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_test);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
}

/* ---- A whole setting ------------------------------------------------------------ */

/** @brief Ask the owner for @p config as mesh_radio_configure() does. */
static kode_status_t h_configure(const mesh_drv_lora_config_t *config)
{
    mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CONFIGURE };

    req.config = *config;
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

TEST_CASE("configure: the region's setting goes in, calibrated, and the status says it", "[owner][call]")
{
    const mesh_drv_lora_config_t c = {
        .frequency_hz = 868100000U, .bandwidth_hz = 125000U, .spreading_factor = 10U,
        .coding_rate = 5U, .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 400,
    };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_configure(&c));
    TEST_ASSERT_EQUAL_UINT32(868100000U, g_lr.cfg.frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(10U, g_lr.cfg.spreading_factor);

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL_UINT8(MESH_BAND_SUB_GHZ, st.band);
    TEST_ASSERT_EQUAL_UINT32(868100000U, st.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(125000U, st.bandwidth_hz);
    TEST_ASSERT_EQUAL_UINT8(5U, st.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(8U, st.preamble);
    TEST_ASSERT_EQUAL_UINT8(0x12U, st.sync_word);
    TEST_ASSERT_EQUAL_INT32(400, st.power_cdbm);
}

TEST_CASE("configure: a setting this build refuses leaves the one in force, receiver and all", "[owner][call]")
{
    const mesh_drv_lora_config_t bad = {
        .frequency_hz = 869525000U, .bandwidth_hz = 250000U, .spreading_factor = 11U,
        .coding_rate = 5U, .preamble = 16U, .sync_word = 0x2BU, .power_cdbm = 2200,
    };

    harness_fresh();
    mesh_drv_lora_settings_t const before = g_lr.cfg;
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_configure(&bad));
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_lr.cfg, sizeof(before));
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

/* ---- Why a call was refused (mesh_aos_why) ------------------------------------ */

TEST_CASE("why: every refusal leaves its own reason, a call that goes through clears it", "[owner][why]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U };

    harness_fresh();
    mesh_aos_why_note(MESH_WHY_NONE);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));

    /* no setting yet */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NOT_BEGUN, mesh_aos_why());

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NONE, mesh_aos_why());

    /* a frequency the region does not allow */
    mesh_aos_request_t cal = { .kind = MESH_REQUEST_RADIO_CALIBRATE, .u = 1U };
    cal.hz[0] = 1000U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_call(&cal, 2000U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NOT_ALLOWED, mesh_aos_why());

    /* a test signal on air */
    TEST_ASSERT_EQUAL(KODE_OK, h_test(MESH_DRV_LR2021_TEST_PREAMBLE, 1000));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BUSY_TEST, mesh_aos_why());
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(MESH_DRV_LR2021_TEST_PREAMBLE, 0));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));

    /* the owner's own answer for an argument */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_test(MESH_DRV_LR2021_TEST_PREAMBLE, 0));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BAD_ARGUMENT, mesh_aos_why());

    /* no module */
    h_module_away();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_MODULE_ABSENT, mesh_aos_why());
    h_module_back();

    /* from an Active Object: refused at once, and its reason is not noted - an AO is told in
     * its answers, and must not push a task's reason out of the table. The standard layer
     * notes the AO's refusal with its own words (mesh_aos_why_note_from_ao()). */
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 5000U));
    mesh_aos_why_note(MESH_WHY_BAD_ARGUMENT);
    fake_ao_become_app_ao();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(true));
    fake_ao_reset();
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_BAD_ARGUMENT, mesh_aos_why());   /* what this task had is still there */
    fake_ao_become_app_ao();
    mesh_aos_why_note_from_ao("mesh_radio_send", "ao_probe");
    fake_ao_reset();
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_FROM_AO, mesh_aos_why());
    TEST_ASSERT_EQUAL_STRING("mesh_radio_send was called from an Active Object (ao_probe); mesh.h calls block - call mesh_sys_* instead",
                             mesh_aos_error());
}

static volatile uint8_t s_other_why = 0xFFU;

static void other_task_denied(void *arg)
{
    (void)arg;
    (void)h_call(MESH_REQUEST_RADIO_RECEIVE, 1000U);   /* no setting: refused */
    s_other_why = mesh_aos_why();
}

TEST_CASE("why: each task has its own reason, and one task's call does not change another's", "[owner][why]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NONE, mesh_aos_why());

    s_other_why = 0xFFU;
    h_bg_t bg;
    h_bg_start(&bg, other_task_denied, NULL);
    TEST_ASSERT_TRUE(h_bg_join(&bg, 2000U));
    TEST_ASSERT_NOT_EQUAL_UINT(0xFFU, s_other_why);
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NONE, mesh_aos_why());   /* this task's is as it was */

    /* a fifth task gives way to the oldest, and nothing breaks */
    for (unsigned i = 0U; i < 6U; i++) {
        h_bg_t b;
        h_bg_start(&b, other_task_denied, NULL);
        TEST_ASSERT_TRUE(h_bg_join(&b, 2000U));
    }
    TEST_ASSERT_LESS_THAN_UINT(MESH_WHY_COUNT, mesh_aos_why());
}

TEST_CASE("why: the duty budget says so", "[owner][why]")
{
    static uint8_t big[MESH_AOS_PAYLOAD_MAX];

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 850));
    kode_status_t rc = KODE_OK;
    for (unsigned n = 0U; (n < 10U) && (rc == KODE_OK); n++) {
        rc = h_send(big, sizeof(big), 2000U);
        if (rc == KODE_OK) {
            lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
            TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
        }
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_DUTY, mesh_aos_why());
}

/* ---- The chip's own timeout ------------------------------------------------------- */

TEST_CASE("call: a chip whose BUSY stuck is an answer, not the owner's silence - no grace, and the line is free", "[owner][call]")
{
    harness_fresh();
    g_lr.configure_rc = KODE_TIMEOUT;   /* the driver: BUSY stayed high, as a module just pulled out */
    uint32_t const t0 = kode_sys_time_ms();
    kode_status_t const rc = h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    uint32_t const took = kode_sys_time_ms() - t0;
    uint8_t const why = mesh_aos_why();
    g_lr.configure_rc = KODE_OK;

    TEST_ASSERT_NOT_EQUAL(KODE_OK, rc);
    /* The owner answered at once: the call did not take it for a request it began and then
     * wait MESH_AOS_CALL_GRACE_MS more for an answer that had already come. */
    TEST_ASSERT_LESS_THAN_UINT32(1000U, took);
    TEST_ASSERT_NOT_EQUAL_UINT(MESH_WHY_TIMED_OUT, why);

    /* and the next call goes through at once */
    uint32_t const t1 = kode_sys_time_ms();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_LESS_THAN_UINT32(1000U, kode_sys_time_ms() - t1);
}

/* ---- The chip refused ----------------------------------------------------------- */

TEST_CASE("why: a chip that refuses has a reason of its own, for a task and for an Active Object", "[owner][why]")
{
    harness_fresh();
    g_lr.configure_rc = KODE_IO_ERROR;
    kode_status_t const rc = h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    uint8_t const why = mesh_aos_why();
    char const *const text = mesh_aos_error();
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, rc);
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_CHIP, why);
    TEST_ASSERT_EQUAL_STRING("the module's chip did not do it (a bus or chip error)", text);

    const mesh_aos_request_t req = {
        .kind = MESH_REQUEST_RADIO_BEGIN, .a = (uint8_t)MESH_PLAN_BAND_SUB_GHZ, .u = 9U, .i = 850,
    };
    static h_answer_t ans;
    TEST_ASSERT_EQUAL(KODE_OK, h_ask(&req, &ans));
    TEST_ASSERT_TRUE(h_answered(&ans, 2000U));
    g_lr.configure_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_CHIP, ans.a.why);
}

TEST_CASE("why: a send cut short by a begin does not lend the begin its reason or its airtime", "[owner][why]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U, 4U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_TRUE(h_status().sending);

    /* The begin ends the packet on air (its call is told INTERRUPTED), then the chip refuses
     * the new setting: the begin's answer is the chip's, with no number of the send's. */
    g_lr.configure_rc = KODE_IO_ERROR;
    const mesh_aos_request_t req = {
        .kind = MESH_REQUEST_RADIO_BEGIN, .a = (uint8_t)MESH_PLAN_BAND_SUB_GHZ, .u = 9U, .i = 850,
    };
    static h_answer_t ans;
    TEST_ASSERT_EQUAL(KODE_OK, h_ask(&req, &ans));
    TEST_ASSERT_TRUE(h_answered(&ans, 2000U));
    g_lr.configure_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, ans.a.rc);
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_CHIP, ans.a.why);
    TEST_ASSERT_EQUAL_UINT32(0U, ans.a.out1);

    TEST_ASSERT_EQUAL(KODE_DENIED, h_send_answer(2000U, NULL));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_INTERRUPTED, mesh_aos_why());
}
