/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ao.c
 * @brief mesh_ao.c: the owner's Active Object on kode-sdk's own QP/C,
 *        dispatched by hand.
 *
 * mesh_ao.c is built into this file, so a case reaches its state machine as
 * QP/C does: Mesh_ctor(), then QASM_INIT, then each event taken off the queue
 * and dispatched, its block handed back. The queue is a FreeRTOS one laid out
 * where the port reads it, so a post with a margin counts its room as on the
 * board; the pool is QF's own, with a count the cases know. What is not here
 * is the task: no QACTIVE_START, so nothing runs but the case. The task, its
 * level and the time it takes are the board's to show, on the board
 * itself.
 *
 * The other side of the seam, mesh_aos.c's mesh_aos_on_*(), is faked below:
 * each counts its calls and keeps what it was given.
 */
#include "mesh_ao.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fake_kode.h"

/* ---- mesh_aos.c, faked at mesh_ao.h --------------------------------------------------- */

static struct {
    unsigned           requests;
    mesh_aos_request_t last;
    unsigned           ticks;
    unsigned           radio_interrupts;
    unsigned           stops;
    unsigned           rests;
    unsigned           awakes;
    unsigned           homes;
    unsigned           powers;
    bool               power_on;
    uint8_t            power_rc;
} g_on;

/** @brief Everything but the requests and the ticks: what the system told the owner. */
static unsigned others(void)
{
    return g_on.radio_interrupts + g_on.stops + g_on.rests + g_on.awakes + g_on.homes + g_on.powers;
}

void mesh_aos_on_request(const mesh_aos_request_t *req)
{
    g_on.requests++;
    g_on.last = *req;
}

void mesh_aos_on_tick(void)
{
    g_on.ticks++;
}

void mesh_aos_on_radio_interrupt(void)
{
    g_on.radio_interrupts++;
}

void mesh_aos_on_stop(void)
{
    g_on.stops++;
}

void mesh_aos_on_rest(void)
{
    g_on.rests++;
}

void mesh_aos_on_awake(void)
{
    g_on.awakes++;
}

void mesh_aos_on_home(void)
{
    g_on.homes++;
}

void mesh_aos_on_power(bool on, uint8_t rc)
{
    g_on.powers++;
    g_on.power_on = on;
    g_on.power_rc = rc;
}

/* ---- QF, brought up once ---------------------------------------------------------- */

/* One pool, of blocks wide enough for anything the owner is sent, and a count
 * the cases rely on: more than the queue takes past its margin, so the queue
 * is what refuses first unless a case drains the pool on purpose. */
#define T_BLOCK  ((sizeof(MeshRequestEvt) + 7U) & ~(size_t)7U)
#define T_BLOCKS 32U

static QSubscrList     s_subscr[MAX_PUB_SIG];
static uint64_t        s_pool[(T_BLOCK * T_BLOCKS) / sizeof(uint64_t)];
static uint8_t         s_queue_sto[AO_QUEUE_LENGTH * sizeof(QEvt *)];
static QEvt const      s_home  = QEVT_INITIALIZER(HOME_SIG);

/* What kode-sdk publishes, as static events, each of its own type. */
static ExpPowerDoneEvt   const s_power_on   = { QEVT_INITIALIZER(EXP_POWER_SIG), true, (uint8_t)KODE_OK };
static ExpPowerDoneEvt   const s_power_off  = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_IO_ERROR };
static LowPowerRoundEvt  const s_round_asleep = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_ASLEEP, .parts = 0U, .round = 1U };
static LowPowerRoundEvt  const s_round_off    = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_OFF, .parts = 0U, .round = 2U };
static LowPowerRoundEvt  const s_round_awake  = { .super = QEVT_INITIALIZER(LOWPOWER_ROUND_SIG), .target = (uint8_t)KODE_LP_AWAKE, .parts = 0U, .round = 3U };
static LowPowerStateEvt  const s_state_awake  = { QEVT_INITIALIZER(LOWPOWER_STATE_SIG), (uint8_t)KODE_LP_AWAKE, 0U, (uint8_t)KODE_LP_ASLEEP };
static LowPowerStateEvt  const s_state_asleep = { QEVT_INITIALIZER(LOWPOWER_STATE_SIG), (uint8_t)KODE_LP_ASLEEP, 0U, (uint8_t)KODE_LP_AWAKE };
static LowPowerStateEvt  const s_state_off    = { QEVT_INITIALIZER(LOWPOWER_STATE_SIG), (uint8_t)KODE_LP_OFF, 0U, (uint8_t)KODE_LP_ASLEEP };
static LowPowerStateEvt  const s_state_dark   = { QEVT_INITIALIZER(LOWPOWER_STATE_SIG), (uint8_t)KODE_LP_DARK, 0U, (uint8_t)KODE_LP_ASLEEP };

/**
 * @brief QF, the pool, the queue and the owner, once per binary: QP/C has
 *        no teardown, and a priority is registered once.
 */
static void ao_up(void)
{
    static bool up;
    if (up) {
        return;
    }
    up = true;
    QF_init();
    QActive_psInit(s_subscr, Q_DIM(s_subscr));
    QF_poolInit(s_pool, sizeof(s_pool), T_BLOCK);

    TEST_ASSERT_EQUAL(KODE_OK, Mesh_ctor());
    /* What QACTIVE_START() does besides the task: the level, the registry
     * (QActive_subscribe() checks it), and the queue where the port reads
     * its room. */
    l_ao.super.prio  = AO_PRIORITY;
    l_ao.super.pthre = 0U;
    QActive_register_(&l_ao.super);
    l_ao.super.eQueue = xQueueCreateStatic(AO_QUEUE_LENGTH, sizeof(QEvt *), s_queue_sto,
                                           &l_ao.super.osObject.queue);
    TEST_ASSERT_NOT_NULL(l_ao.super.eQueue);
    QASM_INIT(&l_ao.super, (void *)0, 0U);
    s_ao_started = true;
}

/** @brief Dispatch what waits in the owner's queue, its blocks back to the pool. */
static unsigned settle(void)
{
    unsigned    n = 0U;
    QEvt const *e = NULL;
    while (xQueueReceive(l_ao.super.eQueue, &e, 0U) == pdTRUE) {
        QASM_DISPATCH(&l_ao.super, e, 0U);
        QF_gc(e);
        n++;
    }
    return n;
}

/** @brief QF's ticks for @p ms. */
static void tick_for(uint32_t ms)
{
    for (uint32_t t = 0U; t < KQP_TICKS_PER_MS(ms); t++) {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
}

/** @brief @p count steps of 10 ms of QF's ticks, each followed by what it posted. */
static void steps(unsigned count)
{
    for (unsigned s = 0U; s < count; s++) {
        tick_for(10U);
        (void)settle();
    }
}

/** @brief Requests posted until one is refused. @return How many went. */
static unsigned post_until_refused(kode_status_t *refused)
{
    mesh_aos_request_t const r = { .kind = MESH_REQUEST_RADIO_RECEIVE, .u = 1U };
    unsigned                 n = 0U;
    for (;;) {
        kode_status_t const rc = mesh_ao_post(&r);
        if (rc != KODE_OK) {
            *refused = rc;
            return n;
        }
        n++;
        TEST_ASSERT_TRUE(n <= T_BLOCKS);   /* never unbounded */
    }
}

void setUp(void)
{
    ao_up();
    (void)settle();
    (void)memset(&g_on, 0, sizeof(g_on));
}

void tearDown(void)
{
}

/* ---- The cases --------------------------------------------------------------------- */

TEST_CASE("start: subscribed to the system's four signals, and the tick armed", "[ao]")
{
    /* Subscribed by the initial transition, which ao_up() ran. */
    TEST_ASSERT_TRUE(QPSet_hasElement(&s_subscr[HOME_SIG].set, AO_PRIORITY));
    TEST_ASSERT_TRUE(QPSet_hasElement(&s_subscr[EXP_POWER_SIG].set, AO_PRIORITY));
    TEST_ASSERT_TRUE(QPSet_hasElement(&s_subscr[LOWPOWER_ROUND_SIG].set, AO_PRIORITY));
    TEST_ASSERT_TRUE(QPSet_hasElement(&s_subscr[LOWPOWER_STATE_SIG].set, AO_PRIORITY));

    /* Armed by the state's entry: nothing before MESH_AO_TICK_MS, then one. */
    steps(1U);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.ticks);
    steps(1U);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests + others());
}

TEST_CASE("the owner runs at KODE_PRIO_USER_HIGH unless CONFIG_MESH_AO_PRIORITY moves it, on kode-sdk's queue depth", "[ao]")
{
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_HIGH, AO_PRIORITY);   /* no CONFIG_MESH_AO_PRIORITY here */
    TEST_ASSERT_EQUAL_UINT8(AO_PRIORITY, l_ao.super.prio);
    TEST_ASSERT_EQUAL_UINT(KQP_AO_QUEUE_LEN, AO_QUEUE_LENGTH);
}

TEST_CASE("the tick reaches the owner every MESH_AO_TICK_MS, armed again as each is served", "[ao]")
{
    steps(2U);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(2U * 9U);
    TEST_ASSERT_EQUAL_UINT(10U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests + others());
}

/* Fails with a periodic tick. kode-sdk's QF tick is an esp_timer, and one
 * held back fires every missed tick back to back (no skip_unhandled_events):
 * a periodic time event then posts one per MESH_AO_TICK_MS into a queue no one
 * drains meanwhile, and the 17th is qf_port:130 (an app met it on a board, as
 * it opened a network session). One shot, armed again as it is served, is one in
 * the queue at most, however long the hold. */
TEST_CASE("the tick held back 400 ms leaves one tick in the queue, not twenty", "[ao]")
{
    tick_for(400U);
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - 1U, uxQueueSpacesAvailable(l_ao.super.eQueue));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    steps(2U * 3U);
    TEST_ASSERT_EQUAL_UINT(4U, g_on.ticks);   /* and every MESH_AO_TICK_MS again */
}

TEST_CASE("a request reaches the owner whole, and its block goes back to the pool", "[ao]")
{
    static uint8_t const payload[3] = { 1, 2, 3 };
    mesh_aos_request_t const r = { .kind   = MESH_REQUEST_RADIO_SEND,
                                   .answer = true,
                                   .seq    = 0x12345678U,
                                   .epoch  = 7U,
                                   .a      = 1U,
                                   .i      = -321,
                                   .u      = 0xCAFEU,
                                   .data   = payload,
                                   .len    = 3U,
                                   .slot   = 2U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_post(&r));
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.requests);
    TEST_ASSERT_EQUAL(r.kind, g_on.last.kind);
    TEST_ASSERT_TRUE(g_on.last.answer);
    TEST_ASSERT_EQUAL_UINT32(r.seq, g_on.last.seq);
    TEST_ASSERT_EQUAL_UINT32(r.epoch, g_on.last.epoch);
    TEST_ASSERT_EQUAL_UINT8(r.a, g_on.last.a);
    TEST_ASSERT_EQUAL_INT32(r.i, g_on.last.i);
    TEST_ASSERT_EQUAL_UINT32(r.u, g_on.last.u);
    TEST_ASSERT_EQUAL_PTR(payload, g_on.last.data);
    TEST_ASSERT_EQUAL_UINT8(r.len, g_on.last.len);
    TEST_ASSERT_EQUAL_UINT8(r.slot, g_on.last.slot);

    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, post_until_refused(&rc));   /* as many again: none kept */
    (void)settle();
}

TEST_CASE("a request takes its place in the queue behind the ones before it", "[ao]")
{
    for (unsigned n = 0U; n < 4U; n++) {
        mesh_aos_request_t const r = { .kind = MESH_REQUEST_RADIO_LISTEN, .u = n + 1U };
        TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_post(&r));
    }
    TEST_ASSERT_EQUAL_UINT(4U, settle());
    TEST_ASSERT_EQUAL_UINT(4U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT32(4U, g_on.last.u);   /* the last one served is the last one posted */
}

TEST_CASE("a queue nearly full refuses a request; the tick, the interrupt, the supply's answer, the rest, the wake and home still find room", "[ao]")
{
    kode_status_t rc    = KODE_OK;
    unsigned const went = post_until_refused(&rc);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, rc);   /* answered, not asserted */
    TEST_ASSERT_EQUAL_UINT(AO_QUEUE_LENGTH - AO_QUEUE_MARGIN, went);

    mesh_ao_nudge();   /* refused, and quietly: the next tick applies a stop */
    tick_for(MESH_AO_TICK_MS);                    /* no margin: a full queue would stop the run */
    mesh_ao_radio_interrupt_from_isr();
    QACTIVE_PUBLISH(&s_power_on.super, (void *)0);
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);
    QACTIVE_PUBLISH(&s_home, (void *)0);
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(went, g_on.requests);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.ticks);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.radio_interrupts);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.powers);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.rests);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.awakes);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.stops);
}

TEST_CASE("a pool running low refuses a request before QP/C would stop the board", "[ao]")
{
    QEvt    *held[T_BLOCKS];
    unsigned n = 0U;
    /* Leave the pool one block past the owner's margin. */
    while (n < (T_BLOCKS - AO_POOL_MARGIN - 1U)) {
        held[n] = Q_NEW_X(QEvt, 0U, MESH_TICK_SIG);
        TEST_ASSERT_NOT_NULL(held[n]);
        n++;
    }
    mesh_aos_request_t const r = { .kind = MESH_REQUEST_RADIO_RECEIVE };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_post(&r));            /* the last above the margin */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_ao_post(&r));  /* the margin is the system's */
    TEST_ASSERT_TRUE(uxQueueSpacesAvailable(l_ao.super.eQueue) > AO_QUEUE_MARGIN);   /* not the queue */
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
    TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_post(&r));
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.requests);
}

TEST_CASE("a stop and the radio's interrupt take no block: posted with the pool dry, they reach the owner", "[ao]")
{
    QEvt    *held[T_BLOCKS];
    unsigned n = 0U;
    for (;;) {
        QEvt * const e = Q_NEW_X(QEvt, 0U, MESH_TICK_SIG);
        if (e == NULL) {
            break;
        }
        held[n] = e;
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(T_BLOCKS, n);
    mesh_ao_nudge();
    mesh_ao_nudge();
    mesh_ao_radio_interrupt_from_isr();
    (void)settle();
    TEST_ASSERT_EQUAL_UINT(2U, g_on.stops);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.radio_interrupts);
    for (unsigned i = 0U; i < n; i++) {
        QF_gc(held[i]);
    }
}

TEST_CASE("the radio's interrupt, from the ISR, is delivered once per call", "[ao]")
{
    mesh_ao_radio_interrupt_from_isr();
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.radio_interrupts);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.requests + g_on.ticks + g_on.stops + g_on.rests + g_on.awakes + g_on.homes + g_on.powers);

    TEST_ASSERT_EQUAL_UINT(0U, settle());   /* nothing left behind to deliver again */
    TEST_ASSERT_EQUAL_UINT(1U, g_on.radio_interrupts);

    mesh_ao_radio_interrupt_from_isr();
    mesh_ao_radio_interrupt_from_isr();
    TEST_ASSERT_EQUAL_UINT(2U, settle());
    TEST_ASSERT_EQUAL_UINT(3U, g_on.radio_interrupts);
}

TEST_CASE("EXP_POWER_SIG, published, reaches the owner with the switch and how it went", "[ao]")
{
    QACTIVE_PUBLISH(&s_power_on.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.powers);
    TEST_ASSERT_TRUE(g_on.power_on);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, g_on.power_rc);

    QACTIVE_PUBLISH(&s_power_off.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.powers);
    TEST_ASSERT_FALSE(g_on.power_on);
    TEST_ASSERT_EQUAL_UINT8(KODE_IO_ERROR, g_on.power_rc);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests + g_on.awakes + g_on.homes);
}

TEST_CASE("LOWPOWER_ROUND_SIG for ASLEEP or OFF rests the owner; for any other target it does not", "[ao]")
{
    QACTIVE_PUBLISH(&s_round_asleep.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.rests);
    QACTIVE_PUBLISH(&s_round_off.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.rests);
    QACTIVE_PUBLISH(&s_round_awake.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.rests);   /* a round for AWAKE is nothing to rest for */
    TEST_ASSERT_EQUAL_UINT(0U, g_on.awakes + g_on.homes + g_on.powers);
}

TEST_CASE("LOWPOWER_STATE_SIG to AWAKE wakes the owner; the other states do not", "[ao]")
{
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.awakes);
    QACTIVE_PUBLISH(&s_state_awake.super, (void *)0);   /* it repeats: the owner's own handler does nothing the second time */
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.awakes);

    QACTIVE_PUBLISH(&s_state_asleep.super, (void *)0);
    QACTIVE_PUBLISH(&s_state_off.super, (void *)0);
    QACTIVE_PUBLISH(&s_state_dark.super, (void *)0);
    TEST_ASSERT_EQUAL_UINT(3U, settle());
    TEST_ASSERT_EQUAL_UINT(2U, g_on.awakes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests + g_on.homes + g_on.powers);
}

/* Fails if the owner stops subscribing to HOME_SIG: a held bottom button
 * would reset the board with a carrier still up. */
TEST_CASE("HOME_SIG, published, reaches the owner", "[ao]")
{
    QACTIVE_PUBLISH(&s_home, (void *)0);
    TEST_ASSERT_EQUAL_UINT(1U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.homes);
    TEST_ASSERT_EQUAL_UINT(0U, g_on.rests + g_on.awakes + g_on.powers);
}

TEST_CASE("a stop is applied by its own signal, whatever the queue holds before it", "[ao]")
{
    mesh_aos_request_t const r = { .kind = MESH_REQUEST_RADIO_RECEIVE };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_post(&r));
    mesh_ao_nudge();
    TEST_ASSERT_EQUAL_UINT(2U, settle());
    TEST_ASSERT_EQUAL_UINT(1U, g_on.requests);
    TEST_ASSERT_EQUAL_UINT(1U, g_on.stops);
}

TEST_CASE("the case's task is not the owner's; before the start nothing is posted", "[ao]")
{
    TEST_ASSERT_FALSE(mesh_ao_is_own_task());
    s_ao_started = false;
    mesh_aos_request_t const r = { .kind = MESH_REQUEST_RADIO_RECEIVE };
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_ao_post(&r));
    mesh_ao_nudge();
    mesh_ao_radio_interrupt_from_isr();
    TEST_ASSERT_FALSE(mesh_ao_is_own_task());
    s_ao_started = true;
    TEST_ASSERT_EQUAL_UINT(0U, settle());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_ao_post(NULL));
}

TEST_CASE("a task is an Active Object's when the registry holds an AO with that task, and the call says which", "[ao]")
{
    ao_up();
    TaskHandle_t const task = l_ao.super.thread;
    const char        *name = NULL;

    /* The case's task is an ordinary one, as a kode_worker's is. */
    TEST_ASSERT_FALSE(mesh_ao_caller_is_ao(&name));
    TEST_ASSERT_NULL(name);
    TEST_ASSERT_FALSE(mesh_ao_caller_is_ao(NULL));
    /* The same task, as the thread of the registered Active Object. */
    l_ao.super.thread = xTaskGetCurrentTaskHandle();
    TEST_ASSERT_TRUE(mesh_ao_caller_is_ao(&name));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(mesh_ao_caller_is_ao(NULL));
    TEST_ASSERT_TRUE(mesh_ao_is_own_task());
    l_ao.super.thread = task;
    TEST_ASSERT_FALSE(mesh_ao_caller_is_ao(&name));
}

TEST_CASE("a second start is OK; a start at a level that is taken is DENIED, says so, and starts nothing", "[ao]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_ao_start());   /* already started: idempotent */

    /* The level is the owner's own here, which is a level held: what a second owner, or an app
     * that took it first, would meet. */
    unsigned const warnings = g_log_warnings;
    s_ao_started = false;
    TEST_ASSERT_FALSE(level_is_free());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_ao_start());
    s_ao_started = true;
    TEST_ASSERT_TRUE(g_log_warnings > warnings);
    TEST_ASSERT_EQUAL_UINT(0U, settle());
}

TEST_CASE("the owner sees the pools once kode-sdk has made them", "[ao]")
{
    TEST_ASSERT_TRUE(mesh_ao_kode_ready());   /* QF_poolInit() ran in ao_up() */
}

/**
 * @brief What QP/C calls instead of carrying on: the run stops, naming it.
 * @param module The QP/C file that raised it.
 * @param id     Its assertion id.
 */
Q_NORETURN Q_onError(char const * const module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    abort();
}
