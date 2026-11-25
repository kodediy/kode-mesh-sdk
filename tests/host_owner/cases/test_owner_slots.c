/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_slots.c
 * @brief The payload slots: a send's bytes wait in one of two slots of the
 *        owner's, not in the request, and the request carries the slot's
 *        number.
 *
 * A slot is the caller's from the moment mesh_aos_post() or mesh_aos_call()
 * takes it until the owner has served the request or skipped it - skipped
 * because its caller gave up, or because a stop came after it. A third send
 * while both are held is turned away with KODE_NO_RESOURCES, before it waits
 * at all. The radio fake stands still at a gate (fakes/fake_lr2021.c) to hold
 * the owner while the cases fill and free the slots.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

#define SLOT_COUNT  2U   /* mesh_aos.c's SEND_SLOTS */

static const mesh_aos_request_t BEGIN_REQ = {
    .kind = MESH_REQUEST_RADIO_BEGIN, .a = (uint8_t)MESH_PLAN_BAND_SUB_GHZ, .u = 9U, .i = 850,
};

/** @brief A send of @p len bytes of @p data, posted: nobody waits for it. */
static kode_status_t post_send(const uint8_t *data, uint8_t len)
{
    const mesh_aos_request_t send = { .kind = MESH_REQUEST_RADIO_SEND, .data = data, .len = len };
    return mesh_aos_post(&send);
}

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
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_I2C_RETURN, 3000U));   /* queued behind the rest */
}

/** @brief Both slots are free: with the owner held, exactly two sends are taken. */
static void assert_both_slots_free(void)
{
    uint8_t d[4] = { 1, 2, 3, 4 };

    owner_stuck_in_begin();
    for (unsigned i = 0U; i < SLOT_COUNT; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, post_send(d, 4U));
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, post_send(d, 4U));
    owner_released_and_drained();
}

TEST_CASE("slots: two sends in flight keep their own bytes; a third finds both held", "[owner][slots]")
{
    harness_fresh();
    g_lr.send_rc = KODE_IO_ERROR;   /* nothing stays on air: the second send is served too */
    lr_fake_gate(LR_GATE_SEND);

    uint8_t first[8];
    uint8_t second[8];
    (void)memset(first, 'A', sizeof(first));
    (void)memset(second, 'B', sizeof(second));

    TEST_ASSERT_EQUAL(KODE_OK, post_send(first, 8U));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.blocked, 1000));      /* the owner is inside the first send */
    (void)memset(first, 0xEE, sizeof(first));             /* the caller's buffer is free at once */
    TEST_ASSERT_EQUAL_MEMORY("AAAAAAAA", g_lr.sent, 8U);  /* and the chip was handed the slot's copy */

    TEST_ASSERT_EQUAL(KODE_OK, post_send(second, 8U));    /* the second slot */
    (void)memset(second, 0xEE, sizeof(second));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, post_send(second, 8U));   /* the first is held until it is served */
    TEST_ASSERT_EQUAL_UINT(0U, g_ao.refused);             /* the owner's slots said no, not the queue */

    lr_fake_release();
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_send == 2U, 2000));
    TEST_ASSERT_EQUAL_MEMORY("BBBBBBBB", g_lr.sent, 8U);
    TEST_ASSERT_EQUAL_UINT(2U, g_lr.n_send);

    owner_released_and_drained();
    assert_both_slots_free();   /* a slot is free again once its request is served */
}

TEST_CASE("slots: a request whose caller gave up frees its slot when the owner skips it", "[owner][slots]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t d[4] = { 9, 9, 9, 9 };
    for (unsigned i = 0U; i < SLOT_COUNT; i++) {
        TEST_ASSERT_EQUAL(KODE_TIMEOUT, h_send(d, 4U, 60U));   /* withdrawn, but still queued */
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, h_send(d, 4U, 60U));  /* so both slots are still held */

    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);                    /* skipped, never sent */
    assert_both_slots_free();
}

TEST_CASE("slots: a send dropped by a stop frees its slot too", "[owner][slots]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t d[4] = { 5, 6, 7, 8 };
    for (unsigned i = 0U; i < SLOT_COUNT; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, post_send(d, 4U));
    }
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());

    owner_released_and_drained();
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);
    assert_both_slots_free();
}

TEST_CASE("slots: a request the Active Object turns away gives its slot back", "[owner][slots]")
{
    harness_fresh();
    uint8_t d[4] = { 1, 1, 1, 1 };

    g_ao.refuse_posts = true;   /* the queue is nearly full, or the pool is dry */
    for (unsigned i = 0U; i < (SLOT_COUNT + 2U); i++) {
        TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, post_send(d, 4U));   /* never "both held" */
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, h_send(d, 4U, 500U));    /* and a call is refused the same way */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(SLOT_COUNT + 3U, g_ao.refused);   /* the call asked again every 10 ms until its time */
    g_ao.refuse_posts = false;

    TEST_ASSERT_EQUAL(KODE_OK, h_send(d, 4U, 2000U));   /* the callers' lock was given back too */
    assert_both_slots_free();
}

TEST_CASE("slots: a request without bytes takes no slot", "[owner][slots]")
{
    harness_fresh();
    owner_stuck_in_begin();

    uint8_t d[4] = { 3, 3, 3, 3 };
    for (unsigned i = 0U; i < SLOT_COUNT; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, post_send(d, 4U));
    }
    /* Both slots held: what carries no payload still goes through. */
    const mesh_aos_request_t standby = { .kind = MESH_REQUEST_RADIO_STANDBY };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_post(&standby));
    TEST_ASSERT_EQUAL(KODE_OK, post_send(NULL, 4U));    /* a send with no bytes is not a slot's business */
    TEST_ASSERT_EQUAL(KODE_OK, post_send(d, 0U));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, post_send(d, 4U));

    owner_released_and_drained();
    uint8_t const none = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(&none, 0U, 2000U));   /* the owner answers an empty send, whatever was held */
}
