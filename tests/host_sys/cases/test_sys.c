/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_sys.c
 * @brief mesh_sys.c: the extended layer, called from Active Objects on kode-sdk's own QP/C.
 *
 * The Active Objects here are real QActive objects with real queues, laid out where the port reads
 * them, and the pool is QF's own with a count the cases know, so a post with a margin counts its
 * room as on the board and a leak is a block that does not come back. What is not here is a task:
 * no QACTIVE_START, so nothing runs but the case, which takes events off the queues by hand. The
 * owner is faked (fakes/fake_owner.c): it writes down what it is asked and answers when the case
 * says so, on the case's own task.
 *
 * One case per return value of every public function, and for every kind of request the same
 * checks: taken and asked once, answered exactly once to the asker alone, a door for each asker
 * and kind that opens before the answer is posted, an answer with no margin, and nothing leaked.
 */
#include "mesh_sys.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fake_owner.h"
#include "test_sys.h"

/* ---- QF, brought up once --------------------------------------------------------------------- */

#define T_BLOCK   64U
#define T_BLOCKS  40U

typedef struct { QEvt super; uint8_t pad[T_BLOCK - sizeof(QEvt) - 8U]; } fat_evt_t;

static QSubscrList  s_subscr[MAX_PUB_SIG];
static uint64_t     s_pool[(T_BLOCK * T_BLOCKS) / sizeof(uint64_t)];

typedef struct {
    QActive super;
    uint8_t sto[T_QUEUE_MAX * sizeof(QEvt *)];
    uint8_t depth;
} t_ao_t;

static t_ao_t s_ao[T_AOS];
static const QEvt s_filler = QEVT_INITIALIZER(MAX_SIG);

static QState t_initial(QActive * const me, void const * const par)
{
    (void)me;
    (void)par;
    return Q_SUPER(&QHsm_top);
}

QActive *t_ao(unsigned i)
{
    return &s_ao[i].super;
}

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
    for (unsigned i = 0U; i < T_AOS; i++) {
        t_ao_t *const a = &s_ao[i];
        QActive_ctor(&a->super, Q_STATE_CAST(&t_initial));
        a->super.prio  = (uint8_t)(i + 1U);
        a->super.pthre = 0U;
        QActive_register_(&a->super);
        a->depth = (i == T_AO_SMALL) ? T_SMALL_DEPTH : T_QUEUE_MAX;
        a->super.eQueue = xQueueCreateStatic(a->depth, sizeof(QEvt *), a->sto, &a->super.osObject.queue);
        TEST_ASSERT_NOT_NULL(a->super.eQueue);
    }
}

QEvt const *t_pop(unsigned i)
{
    QEvt const *e = NULL;
    return (xQueueReceive(s_ao[i].super.eQueue, &e, 0U) == pdTRUE) ? e : NULL;
}

unsigned t_waiting(unsigned i)
{
    return (unsigned)uxQueueMessagesWaiting(s_ao[i].super.eQueue);
}

unsigned t_free_slots(unsigned i)
{
    return (unsigned)uxQueueSpacesAvailable(s_ao[i].super.eQueue);
}

/** @brief Take everything off an Active Object's queue and give the blocks back; how many. */
unsigned t_drain(unsigned i)
{
    unsigned    n = 0U;
    QEvt const *e;
    while ((e = t_pop(i)) != NULL) {
        QF_gc(e);
        n++;
    }
    return n;
}

void t_fill(unsigned i, unsigned slots)
{
    for (unsigned k = 0U; k < slots; k++) {
        QACTIVE_POST(&s_ao[i].super, &s_filler, (void *)0);
    }
}

/** @brief The blocks the pool has left, counted by taking them all and giving them back. */
unsigned t_pool_free(void)
{
    QEvt        *held[T_BLOCKS + 4U];
    unsigned     n = 0U;
    while (n < (T_BLOCKS + 4U)) {
        fat_evt_t *const e = Q_NEW_X(fat_evt_t, 0U, MAX_SIG);
        if (e == NULL) {
            break;
        }
        held[n++] = &e->super;
    }
    for (unsigned k = 0U; k < n; k++) {
        QF_gc(held[k]);
    }
    return n;
}

/** @brief Take blocks from the pool until @p left remain; they are given back by t_pool_release(). */
static QEvt *s_hogged[T_BLOCKS];
static unsigned s_hogged_n;

void t_pool_hog(unsigned left)
{
    while (t_pool_free() > left) {
        fat_evt_t *const e = Q_NEW_X(fat_evt_t, 0U, MAX_SIG);
        TEST_ASSERT_NOT_NULL(e);
        s_hogged[s_hogged_n++] = &e->super;
    }
}

void t_pool_release(void)
{
    while (s_hogged_n > 0U) {
        QF_gc(s_hogged[--s_hogged_n]);
    }
}

/* ---- Before and after every case -------------------------------------------------------------- */

void setUp(void)
{
    ao_up();
    for (unsigned i = 0U; i < T_AOS; i++) {
        (void)t_drain(i);
    }
    fake_owner_reset();
}

void tearDown(void)
{
    /* What a case left asked is answered, its answers and its news taken off, and the pool must be
     * whole again: a block that did not come back is a leak in the layer. */
    t_pool_release();
    for (unsigned i = 0U; i < T_AOS; i++) {
        (void)t_drain(i);
    }
    (void)fake_owner_answer_all();
    for (unsigned i = 0U; i < T_AOS; i++) {
        (void)t_drain(i);
        (void)mesh_sys_unsubscribe(t_ao(i));
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(T_BLOCKS, t_pool_free(), "a block did not come back to the pool");
}

/* ---- The kinds of request ---------------------------------------------------------------------- */

static const uint8_t DATA[3] = { 1U, 2U, 3U };
static const uint32_t FREQS[1] = { 868300000U };

static mesh_radio_config_t config(void)
{
    mesh_radio_config_t c;
    (void)mesh_sys_radio_config_default(MESH_BAND_SUB_GHZ, &c);
    return c;
}

static kode_status_t r_radio_begin(QActive *me)     { return mesh_sys_radio_begin(me, MESH_BAND_SUB_GHZ, 9U, 85); }
static kode_status_t r_radio_configure(QActive *me) { mesh_radio_config_t c = config(); return mesh_sys_radio_configure(me, &c); }
static kode_status_t r_radio_calibrate(QActive *me) { return mesh_sys_radio_calibrate(me, FREQS, 1U); }
static kode_status_t r_radio_retune(QActive *me)    { mesh_radio_config_t c = config(); return mesh_sys_radio_retune(me, &c); }
static kode_status_t r_radio_end(QActive *me)       { return mesh_sys_radio_end(me); }
static kode_status_t r_radio_send(QActive *me)      { return mesh_sys_radio_send(me, DATA, sizeof(DATA)); }
static kode_status_t r_radio_send_pre(QActive *me)  { return mesh_sys_radio_send_preamble(me, DATA, sizeof(DATA), 8U); }
static kode_status_t r_radio_cad(QActive *me)       { return mesh_sys_radio_cad(me, 0U, false); }
static kode_status_t r_radio_cad_rx(QActive *me)    { return mesh_sys_radio_cad_receive(me, 0U, false); }
static kode_status_t r_radio_test(QActive *me)      { return mesh_sys_radio_test(me, MESH_TEST_SIGNAL_CARRIER, 100U); }
static kode_status_t r_gnss_begin(QActive *me)      { return mesh_sys_gnss_begin(me); }
static kode_status_t r_gnss_end(QActive *me)        { return mesh_sys_gnss_end(me); }
static kode_status_t r_gnss_reset(QActive *me)      { return mesh_sys_gnss_reset(me); }
static kode_status_t r_i2c_lend(QActive *me)        { return mesh_sys_i2c_lend(me); }
static kode_status_t r_i2c_return(QActive *me)      { return mesh_sys_i2c_return(me); }

const t_request_t T_REQUESTS[T_REQUEST_COUNT] = {
    { "radio_begin",         r_radio_begin,     MESH_RADIO_BEGIN_RSP_SIG,         MESH_REQUEST_RADIO_BEGIN },
    { "radio_configure",     r_radio_configure, MESH_RADIO_CONFIGURE_RSP_SIG,     MESH_REQUEST_RADIO_CONFIGURE },
    { "radio_calibrate",     r_radio_calibrate, MESH_RADIO_CALIBRATE_RSP_SIG,     MESH_REQUEST_RADIO_CALIBRATE },
    { "radio_retune",        r_radio_retune,    MESH_RADIO_RETUNE_RSP_SIG,        MESH_REQUEST_RADIO_RETUNE },
    { "radio_end",           r_radio_end,       MESH_RADIO_END_RSP_SIG,           MESH_REQUEST_RADIO_END },
    { "radio_send",          r_radio_send,      MESH_RADIO_SEND_RSP_SIG,          MESH_REQUEST_RADIO_SEND },
    { "radio_send_preamble", r_radio_send_pre,  MESH_RADIO_SEND_PREAMBLE_RSP_SIG, MESH_REQUEST_RADIO_SEND },
    { "radio_cad",           r_radio_cad,       MESH_RADIO_CAD_RSP_SIG,           MESH_REQUEST_RADIO_CAD },
    { "radio_cad_receive",   r_radio_cad_rx,    MESH_RADIO_CAD_RECEIVE_RSP_SIG,   MESH_REQUEST_RADIO_CAD },
    { "radio_test",          r_radio_test,      MESH_RADIO_TEST_RSP_SIG,          MESH_REQUEST_RADIO_TEST },
    { "gnss_begin",          r_gnss_begin,      MESH_GNSS_BEGIN_RSP_SIG,          MESH_REQUEST_GNSS_BEGIN },
    { "gnss_end",            r_gnss_end,        MESH_GNSS_END_RSP_SIG,            MESH_REQUEST_GNSS_END },
    { "gnss_reset",          r_gnss_reset,      MESH_GNSS_RESET_RSP_SIG,          MESH_REQUEST_GNSS_RESET },
    { "i2c_lend",            r_i2c_lend,        MESH_I2C_LEND_RSP_SIG,            MESH_REQUEST_I2C_LEND },
    { "i2c_return",          r_i2c_return,      MESH_I2C_RETURN_RSP_SIG,          MESH_REQUEST_I2C_RETURN },
};

/** @brief A message that names the kind that failed. */
static const char *named(const char *what, unsigned k)
{
    static char text[96];
    (void)snprintf(text, sizeof(text), "%s: %s", T_REQUESTS[k].name, what);
    return text;
}

/* ---- Requests ----------------------------------------------------------------------------------- */

TEST_CASE("request: every kind is taken, asked of the owner once, and answered once to the asker alone", "[sys][request]")
{
    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        fake_owner_reset();
        unsigned const free_before = t_pool_free();

        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("taken", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_owner.asks, named("asked of the owner once", k));
        TEST_ASSERT_EQUAL_MESSAGE(T_REQUESTS[k].owner_kind, g_owner.last_ask.kind, named("the owner's kind", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(free_before - 1U, t_pool_free(), named("the answer's block is taken at the ask", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, t_waiting(0), named("no answer before the owner's", k));

        TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, t_waiting(0), named("one answer posted", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, t_waiting(1), named("and to nobody else", k));
        QEvt const *const e = t_pop(0);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(T_REQUESTS[k].rsp_sig, e->sig, named("its signal", k));
        TEST_ASSERT_TRUE_MESSAGE(e->sig >= MESH_SIG_FIRST, named("in this SDK's range", k));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, ((mesh_sys_answer_t const *)e)->rc, named("rc", k));
        TEST_ASSERT_EQUAL_MESSAGE(MESH_WHY_NONE, ((mesh_sys_answer_t const *)e)->why, named("why", k));
        QF_gc(e);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(free_before, t_pool_free(), named("the block comes back", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, t_waiting(0), named("exactly once", k));
    }
}

TEST_CASE("request: an asker that is NULL or not started is DENIED and nothing is asked", "[sys][request]")
{
    QActive unstarted;   /* prio 0: never started */
    (void)memset(&unstarted, 0, sizeof(unstarted));

    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, T_REQUESTS[k].ask(NULL), named("NULL", k));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, T_REQUESTS[k].ask(&unstarted), named("unstarted", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_owner.asks, named("nothing asked", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(T_BLOCKS, t_pool_free(), named("nothing taken", k));
    }
}

TEST_CASE("request: a second of a kind before its answer is DENIED, and the door is open when the answer is posted", "[sys][request]")
{
    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        fake_owner_reset();
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("first", k));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, T_REQUESTS[k].ask(t_ao(0)), named("second: the door is shut", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_owner.asks, named("only the first asked", k));

        TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
        /* The answer is in the queue and its handler has not run: the door is open already. */
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, t_waiting(0), named("answer waiting", k));
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("asked again from before the handler", k));
        TEST_ASSERT_TRUE(fake_owner_answer(1U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, t_drain(0), named("two answers", k));
    }
}

TEST_CASE("request: a door for each kind and each asker", "[sys][request]")
{
    fake_owner_reset();
    /* Every kind from one asker at once... */
    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("asker 0", k));
    }
    /* ... and the same kinds from another asker, who is not held up by the first. */
    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(1)), named("asker 1", k));
    }
    TEST_ASSERT_EQUAL_UINT(2U * T_REQUEST_COUNT, g_owner.asks);

    /* Answered out of order, each answer goes to its own asker, and its own signal. */
    for (unsigned i = g_owner.pending; i-- > 0U;) {
        TEST_ASSERT_TRUE(fake_owner_answer(i, KODE_OK, MESH_WHY_NONE, 0U, 0U));
    }
    for (unsigned a = 0U; a < 2U; a++) {
        unsigned seen = 0U;
        QEvt const *e;
        while ((e = t_pop(a)) != NULL) {
            for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
                if (e->sig == T_REQUESTS[k].rsp_sig) {
                    seen |= (1U << k);
                }
            }
            QF_gc(e);
        }
        TEST_ASSERT_EQUAL_UINT((1U << T_REQUEST_COUNT) - 1U, seen);
    }
}

TEST_CASE("request: the owner turning it away is NO_RESOURCES or DENIED, and gives back the block and the door", "[sys][request]")
{
    static const kode_status_t RCS[2] = { KODE_NO_RESOURCES, KODE_DENIED };

    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        for (unsigned r = 0U; r < 2U; r++) {
            fake_owner_reset();
            g_owner.ask_rc = RCS[r];
            TEST_ASSERT_EQUAL_MESSAGE(RCS[r], T_REQUESTS[k].ask(t_ao(0)), named("turned away", k));
            TEST_ASSERT_EQUAL_UINT_MESSAGE(T_BLOCKS, t_pool_free(), named("the block is back", k));
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_owner.pending, named("nothing pending", k));
            g_owner.ask_rc = KODE_OK;
            TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("and the door is open", k));
            TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
            (void)t_drain(0);
        }
    }
}

TEST_CASE("request: a dry pool is NO_RESOURCES, nothing is asked, and the door is free again", "[sys][request]")
{
    for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
        fake_owner_reset();
        t_pool_hog(0U);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_NO_RESOURCES, T_REQUESTS[k].ask(t_ao(0)), named("dry", k));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_owner.asks, named("not asked", k));
        t_pool_release();
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(0)), named("and then it goes", k));
        TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
        (void)t_drain(0);
    }
}

TEST_CASE("request: the pool keeps blocks for the rest of the system", "[sys][request]")
{
    fake_owner_reset();
    t_pool_hog(2U);   /* the margin the layer leaves is 2: with 2 left, the request is refused */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, r_radio_end(t_ao(0)));
    t_pool_release();
}

TEST_CASE("request: the answer takes the last slot of a nearly full queue - it is posted with no margin", "[sys][request]")
{
    fake_owner_reset();
    unsigned const depth = T_SMALL_DEPTH;
    TEST_ASSERT_EQUAL(KODE_OK, r_radio_end(t_ao(T_AO_SMALL)));
    t_fill(T_AO_SMALL, depth - 1U);                  /* one slot left for the one answer */
    TEST_ASSERT_EQUAL_UINT(1U, t_free_slots(T_AO_SMALL));
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, t_free_slots(T_AO_SMALL));   /* it went in; a margin would have refused it */
    TEST_ASSERT_EQUAL_UINT(depth, t_waiting(T_AO_SMALL));
    QEvt const *last = NULL;
    for (QEvt const *e; (e = t_pop(T_AO_SMALL)) != NULL;) {
        if (last != NULL) { QF_gc(last); }
        last = e;   /* the answer is the last one in */
    }
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_UINT(MESH_RADIO_END_RSP_SIG, last->sig);
    QF_gc(last);
}

TEST_CASE("request: a queue that is completely full loses the answer - counted, never QP's assert; the door opens all the same", "[sys][request]")
{
    fake_owner_reset();
    unsigned const depth       = T_SMALL_DEPTH;
    unsigned const free_before = t_pool_free();
    TEST_ASSERT_EQUAL(KODE_OK, r_radio_end(t_ao(T_AO_SMALL)));
    t_fill(T_AO_SMALL, depth);                        /* no slot left for the answer */
    TEST_ASSERT_EQUAL_UINT(0U, t_free_slots(T_AO_SMALL));
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT(1U, g_owner.answers_dropped);
    TEST_ASSERT_EQUAL_UINT(depth, t_waiting(T_AO_SMALL));   /* only the fillers */
    TEST_ASSERT_EQUAL_UINT(free_before, t_pool_free());     /* the answer's block came back */
    TEST_ASSERT_EQUAL(KODE_OK, r_radio_end(t_ao(T_AO_SMALL)));   /* the door was opened before the post */
    TEST_ASSERT_TRUE(fake_owner_answer(1U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
    TEST_ASSERT_EQUAL_UINT(2U, g_owner.answers_dropped);
    TEST_ASSERT_EQUAL_UINT(depth, t_drain(T_AO_SMALL));
    TEST_ASSERT_EQUAL_UINT(free_before, t_pool_free());
}

TEST_CASE("request: an argument out of range is DENIED, and nothing is asked", "[sys][request]")
{
    mesh_radio_config_t c = config();
    uint32_t freqs[MESH_CALIBRATIONS_MAX + 1U] = { 868300000U, 868300000U, 868300000U, 868300000U };
    QActive *const me = t_ao(0);

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_begin(me, MESH_BAND_COUNT, 9U, 85));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_begin(me, MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MAX + 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_begin(me, MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MIN - 1));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_configure(me, NULL));
    c.power_dbm_x10 = MESH_POWER_X10_MAX + 1;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_configure(me, &c));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_retune(me, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_retune(me, &c));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_calibrate(me, NULL, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_calibrate(me, freqs, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_calibrate(me, freqs, MESH_CALIBRATIONS_MAX + 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_send(me, NULL, 3U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_send(me, DATA, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_send_preamble(me, NULL, 3U, 8U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_send_preamble(me, DATA, 0U, 8U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_send_preamble(me, DATA, 3U, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_cad(me, 5U, false));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_test(me, (mesh_test_signal_t)0, 100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_test(me, (mesh_test_signal_t)9, 100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_test(me, MESH_TEST_SIGNAL_CARRIER, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_test(me, MESH_TEST_SIGNAL_CARRIER, MESH_BSP_TEST_MAX_MS + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.asks);
    TEST_ASSERT_EQUAL_UINT(T_BLOCKS, t_pool_free());

    /* and the edges of what is allowed go through */
    c.power_dbm_x10 = MESH_POWER_X10_MAX;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_configure(me, &c));
    TEST_ASSERT_EQUAL_INT32(MESH_POWER_X10_MAX * 10, g_owner.last_ask.config.power_cdbm);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_calibrate(me, freqs, MESH_CALIBRATIONS_MAX));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_send(me, DATA, 255U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_cad(me, 4U, true));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_test(me, MESH_TEST_SIGNAL_PREAMBLE, MESH_BSP_TEST_MAX_MS));
}

TEST_CASE("request: what the owner is asked is what the app asked, in the owner's words", "[sys][request]")
{
    QActive *const me = t_ao(0);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_begin(me, MESH_BAND_2_4_GHZ, 7U, 45));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_BEGIN, g_owner.last_ask.kind);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MESH_BAND_2_4_GHZ, g_owner.last_ask.a);
    TEST_ASSERT_EQUAL_UINT32(7U, g_owner.last_ask.u);
    TEST_ASSERT_EQUAL_INT32(450, g_owner.last_ask.i);   /* tenths of a dBm to hundredths */

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_send(me, DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT8(3U, g_owner.last_ask.len);
    TEST_ASSERT_EQUAL_PTR(DATA, g_owner.last_ask.data);
    TEST_ASSERT_EQUAL_UINT16(0U, g_owner.last_ask.preamble);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_send_preamble(me, DATA, sizeof(DATA), 12U));
    TEST_ASSERT_EQUAL_UINT16(12U, g_owner.last_ask.preamble);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_cad(me, 3U, true));
    TEST_ASSERT_EQUAL_UINT32(3U, g_owner.last_ask.u);
    TEST_ASSERT_EQUAL_UINT8(1U, g_owner.last_ask.a);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_test(me, MESH_TEST_SIGNAL_CARRIER, 250U));
    TEST_ASSERT_EQUAL_UINT32(2U, g_owner.last_ask.u);   /* the chip's carrier */
    TEST_ASSERT_EQUAL_INT32(250, g_owner.last_ask.i);
    (void)fake_owner_answer_all();   /* the door of this kind is shut until the first is answered */
    (void)t_drain(0);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_test(me, MESH_TEST_SIGNAL_PREAMBLE, 250U));
    TEST_ASSERT_EQUAL_UINT32(1U, g_owner.last_ask.u);   /* the chip's preamble */

    static const uint32_t three[3] = { 868100000U, 868300000U, 868500000U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_calibrate(me, three, 3U));
    TEST_ASSERT_EQUAL_UINT32(3U, g_owner.last_ask.u);
    TEST_ASSERT_EQUAL_UINT32(868500000U, g_owner.last_ask.hz[2]);
}

TEST_CASE("answer: the plain ones carry rc and why as the owner gave them", "[sys][answer]")
{
    static const struct { kode_status_t rc; mesh_why_t why; } CASES[] = {
        { KODE_OK, MESH_WHY_NONE }, { KODE_DENIED, MESH_WHY_NOT_BEGUN }, { KODE_DENIED, MESH_WHY_MODULE_ABSENT },
        { KODE_NO_RESOURCES, MESH_WHY_QUEUE_FULL }, { KODE_TIMEOUT, MESH_WHY_TIMED_OUT }, { KODE_IO_ERROR, MESH_WHY_NOT_ALLOWED },
    };
    for (unsigned c = 0U; c < (sizeof(CASES) / sizeof(CASES[0])); c++) {
        fake_owner_reset();
        TEST_ASSERT_EQUAL(KODE_OK, r_radio_begin(t_ao(0)));
        TEST_ASSERT_TRUE(fake_owner_answer(0U, CASES[c].rc, (uint8_t)CASES[c].why, 7U, 8U));
        QEvt const *const e = t_pop(0);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL(CASES[c].rc, ((mesh_sys_answer_t const *)e)->rc);
        TEST_ASSERT_EQUAL(CASES[c].why, ((mesh_sys_answer_t const *)e)->why);
        QF_gc(e);
    }
}

TEST_CASE("answer: a send carries its airtime when it left, and the wait when the duty budget had no room", "[sys][answer]")
{
    TEST_ASSERT_EQUAL(KODE_OK, r_radio_send(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 1234U, 0U));
    mesh_sys_send_answer_t const *a = (mesh_sys_send_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(MESH_RADIO_SEND_RSP_SIG, a->super.sig);
    TEST_ASSERT_EQUAL(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT32(1234U, a->airtime_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, a->wait_ms);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_radio_send_pre(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(1U, KODE_NO_RESOURCES, MESH_WHY_DUTY, 54321U, 0U));
    a = (mesh_sys_send_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(MESH_RADIO_SEND_PREAMBLE_RSP_SIG, a->super.sig);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, a->rc);
    TEST_ASSERT_EQUAL(MESH_WHY_DUTY, a->why);
    TEST_ASSERT_EQUAL_UINT32(0U, a->airtime_ms);
    TEST_ASSERT_EQUAL_UINT32(54321U, a->wait_ms);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_radio_send(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(2U, KODE_IO_ERROR, MESH_WHY_SEND_FAILED, 999U, 0U));
    a = (mesh_sys_send_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, a->rc);
    TEST_ASSERT_EQUAL(MESH_WHY_SEND_FAILED, a->why);
    TEST_ASSERT_EQUAL_UINT32(0U, a->airtime_ms);   /* nothing to hand back */
    TEST_ASSERT_EQUAL_UINT32(0U, a->wait_ms);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_radio_send(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(3U, KODE_DENIED, MESH_WHY_INTERRUPTED, 0U, 0U));
    a = (mesh_sys_send_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL(MESH_WHY_INTERRUPTED, a->why);
    QF_gc(&a->super);
}

TEST_CASE("request: a search that goes on to receive asks the owner's search with its bit, and is answered as a search", "[sys][request]")
{
    QActive *const me = t_ao(0);

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_cad_receive(me, 5U, false));
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.asks);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_cad_receive(me, 2U, true));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_CAD, g_owner.last_ask.kind);
    TEST_ASSERT_EQUAL_UINT32(2U, g_owner.last_ask.u);
    TEST_ASSERT_EQUAL_UINT8(MESH_AOS_CAD_RECEIVE | MESH_AOS_CAD_FAST, g_owner.last_ask.a);
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 1U, 0U));
    mesh_sys_cad_answer_t const *a = (mesh_sys_cad_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(MESH_RADIO_CAD_RECEIVE_RSP_SIG, a->super.sig);
    TEST_ASSERT_TRUE(a->detected);
    QF_gc(&a->super);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_cad_receive(me, 0U, false));
    TEST_ASSERT_EQUAL_UINT8(MESH_AOS_CAD_RECEIVE, g_owner.last_ask.a);
    (void)fake_owner_answer_all();
    (void)t_drain(0);
}

TEST_CASE("answer: a search says whether it saw activity, and only when it ended", "[sys][answer]")
{
    TEST_ASSERT_EQUAL(KODE_OK, r_radio_cad(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 1U, 0U));
    mesh_sys_cad_answer_t const *a = (mesh_sys_cad_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(MESH_RADIO_CAD_RSP_SIG, a->super.sig);
    TEST_ASSERT_TRUE(a->detected);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_radio_cad(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(1U, KODE_OK, MESH_WHY_NONE, 0U, 0U));
    a = (mesh_sys_cad_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(a->detected);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_radio_cad(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(2U, KODE_DENIED, MESH_WHY_INTERRUPTED, 1U, 0U));   /* cut short: nothing seen */
    a = (mesh_sys_cad_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(KODE_DENIED, a->rc);
    TEST_ASSERT_FALSE(a->detected);
    QF_gc(&a->super);
}

TEST_CASE("answer: the I2C pair's GPIOs come with the loan, and only with it", "[sys][answer]")
{
    TEST_ASSERT_EQUAL(KODE_OK, r_i2c_lend(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(0U, KODE_OK, MESH_WHY_NONE, 31U, 32U));
    mesh_sys_i2c_answer_t const *a = (mesh_sys_i2c_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(MESH_I2C_LEND_RSP_SIG, a->super.sig);
    TEST_ASSERT_EQUAL(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT8(31U, a->scl_gpio);
    TEST_ASSERT_EQUAL_UINT8(32U, a->sda_gpio);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL(KODE_OK, r_i2c_lend(t_ao(0)));
    TEST_ASSERT_TRUE(fake_owner_answer(1U, KODE_DENIED, MESH_WHY_MODULE_ABSENT, 31U, 32U));
    a = (mesh_sys_i2c_answer_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT8(0U, a->scl_gpio);
    TEST_ASSERT_EQUAL_UINT8(0U, a->sda_gpio);
    QF_gc(&a->super);
}

/* ---- Commands ----------------------------------------------------------------------------------- */

TEST_CASE("command: standby, receive, listen are posted to the owner as asked", "[sys][command]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_standby());
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_STANDBY, g_owner.last_post.kind);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_receive(true));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_RECEIVE, g_owner.last_post.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, g_owner.last_post.u);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_receive(false));
    TEST_ASSERT_EQUAL_UINT32(0U, g_owner.last_post.u);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_listen(1U));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_LISTEN, g_owner.last_post.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, g_owner.last_post.u);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_listen(MESH_LISTEN_MS_MAX));
    TEST_ASSERT_EQUAL_UINT32(MESH_LISTEN_MS_MAX, g_owner.last_post.u);
    TEST_ASSERT_EQUAL_UINT(5U, g_owner.posts);
}

TEST_CASE("command: a full queue or a dry pool is NO_RESOURCES, and no module is DENIED, as the owner says", "[sys][command]")
{
    static const kode_status_t RCS[2] = { KODE_NO_RESOURCES, KODE_DENIED };
    for (unsigned r = 0U; r < 2U; r++) {
        g_owner.post_rc = RCS[r];
        TEST_ASSERT_EQUAL(RCS[r], mesh_sys_radio_standby());
        TEST_ASSERT_EQUAL(RCS[r], mesh_sys_radio_receive(true));
        TEST_ASSERT_EQUAL(RCS[r], mesh_sys_radio_listen(100U));
    }
}

TEST_CASE("command: a listen out of range is DENIED and nothing is posted", "[sys][command]")
{
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_listen(0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_listen(MESH_LISTEN_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.posts);
}

TEST_CASE("command: stop is OK, and DENIED before the owner runs; it never waits for the queue", "[sys][command]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_stop());
    TEST_ASSERT_EQUAL_UINT(1U, g_owner.stops);
    g_owner.stop_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_stop());
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.posts);   /* it is not a post: no queue margin to be within */
}

TEST_CASE("command: antennas_confirm is OK with the word handed over, DENIED before the owner runs, never a post", "[sys][command]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_antennas_confirm(true));
    TEST_ASSERT_EQUAL_UINT(1U, g_owner.antennas);
    TEST_ASSERT_TRUE(g_owner.antennas_fitted);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_antennas_confirm(false));
    TEST_ASSERT_FALSE(g_owner.antennas_fitted);
    g_owner.st.present = false;   /* taken with no module too: the owner decides, not the layer */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_antennas_confirm(true));
    g_owner.antennas_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_antennas_confirm(true));
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.posts);   /* it is not a post: no queue margin to be within */
}

TEST_CASE("command: a thousand in a row are refused by the owner's margin, never an assert", "[sys][command]")
{
    unsigned refused = 0U;
    for (unsigned i = 0U; i < 1000U; i++) {
        g_owner.post_rc = ((i % 3U) == 0U) ? KODE_NO_RESOURCES : KODE_OK;
        kode_status_t const rc = mesh_sys_radio_receive((i & 1U) != 0U);
        refused += (rc == KODE_NO_RESOURCES) ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT(334U, refused);
}

/* ---- Begin and end ------------------------------------------------------------------------------ */

TEST_CASE("begin: connects the news, starts the owner, and is idempotent", "[sys][begin]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_NOT_NULL(g_owner.sink);
    TEST_ASSERT_EQUAL_UINT(1U, g_owner.starts);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_EQUAL_UINT(2U, g_owner.starts);   /* the owner's own start is idempotent */
}

TEST_CASE("begin: DENIED before kode_init(), and the owner is not started", "[sys][begin]")
{
    g_owner.kode_ready = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_begin());
    TEST_ASSERT_EQUAL_UINT(0U, g_owner.starts);
    TEST_ASSERT_NULL(g_owner.sink);
}

TEST_CASE("begin: what the owner says is what it returns - DENIED for a level taken, NO_RESOURCES for no owner", "[sys][begin]")
{
    g_owner.start_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_begin());
    g_owner.start_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_begin());
}

TEST_CASE("end: posts the owner's end and returns", "[sys][begin]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_end());
    TEST_ASSERT_EQUAL_UINT(1U, g_owner.end_posts);
    g_owner.end_post_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_end());
    g_owner.end_post_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_end());
}

/* ---- Subscribers and the news -------------------------------------------------------------------- */

TEST_CASE("subscribe: OK, idempotent, DENIED for NULL or unstarted, NO_RESOURCES for a fifth", "[sys][news]")
{
    QActive unstarted;
    (void)memset(&unstarted, 0, sizeof(unstarted));

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_subscribe(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_subscribe(&unstarted));
    for (unsigned i = 0U; i < MESH_SUBSCRIBERS_MAX; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(i)));
    }
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));   /* the same one again: still one place */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_subscribe(t_ao(MESH_SUBSCRIBERS_MAX)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_unsubscribe(t_ao(2)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(MESH_SUBSCRIBERS_MAX)));   /* a place is free again */
}

TEST_CASE("unsubscribe: OK for a subscriber, DENIED for NULL or one that was not", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_unsubscribe(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_unsubscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_unsubscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_unsubscribe(t_ao(0)));
}

static const struct { uint8_t ev; QSignal sig; } NEWS[] = {
    { MESH_AOS_EVENT_PRESENT,           MESH_MODULE_FOUND_SIG },
    { MESH_AOS_EVENT_GONE,              MESH_MODULE_LOST_SIG },
    { MESH_AOS_EVENT_PACKET,            MESH_PACKET_RECEIVED_SIG },
    { MESH_AOS_EVENT_LISTEN_END,        MESH_LISTEN_ENDED_SIG },
    { MESH_AOS_EVENT_RADIO_ERROR,       MESH_RADIO_FAILED_SIG },
    { MESH_AOS_EVENT_WRONG_MODULE,      MESH_WRONG_MODULE_SIG },
    { MESH_AOS_EVENT_TEST_END,          MESH_TEST_ENDED_SIG },
    { MESH_AOS_EVENT_PREAMBLE,          MESH_PREAMBLE_DETECTED_SIG },
    { MESH_AOS_EVENT_HEADER,            MESH_HEADER_RECEIVED_SIG },
    { MESH_AOS_EVENT_HEADER_BAD,        MESH_HEADER_FAILED_SIG },
    { MESH_AOS_EVENT_SETTING_LOST,      MESH_SETTING_LOST_SIG },
    { MESH_AOS_EVENT_SETTING_RESTORED,  MESH_SETTING_RESTORED_SIG },
};

TEST_CASE("news: each event of the owner is its signal, with its value, to every subscriber", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(1)));

    for (unsigned n = 0U; n < (sizeof(NEWS) / sizeof(NEWS[0])); n++) {
        TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(NEWS[n].ev, 100U + n));
        for (unsigned a = 0U; a < 2U; a++) {
            mesh_sys_news_t const *const e = (mesh_sys_news_t const *)t_pop(a);
            TEST_ASSERT_NOT_NULL(e);
            TEST_ASSERT_EQUAL_UINT(NEWS[n].sig, e->super.sig);
            TEST_ASSERT_EQUAL_INT32(100 + (int32_t)n, e->value);
            QF_gc(&e->super);
        }
        TEST_ASSERT_EQUAL_UINT(0U, t_waiting(2));   /* and nobody who did not subscribe */
    }
}

TEST_CASE("news: an owner started by mesh.h's mesh_begin() alone still tells a subscriber", "[sys][news]")
{
    /* What mesh_begin() does: the owner starts, and mesh_sys_begin() is never called. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(MESH_AOS_EVENT_PRESENT, 7U));
    mesh_sys_news_t const *const e = (mesh_sys_news_t const *)t_pop(0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT(MESH_MODULE_FOUND_SIG, e->super.sig);
    TEST_ASSERT_EQUAL_INT32(7, e->value);
    QF_gc(&e->super);
}

TEST_CASE("news: with no subscriber, or for an event that is not news, nothing is posted and nothing is lost", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(MESH_AOS_EVENT_PACKET, 5U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(0U, 5U));
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(4U, 5U));    /* a number that was an event and is not any more */
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(12U, 5U));
    TEST_ASSERT_EQUAL_UINT8(0U, fake_owner_tell(200U, 5U));
    TEST_ASSERT_EQUAL_UINT(0U, t_waiting(0));
}

TEST_CASE("news: posted with a margin - a queue within it loses the news and the owner is told how many", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(1)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(T_AO_SMALL)));   /* a queue too short for any margin */

    TEST_ASSERT_EQUAL_UINT8(1U, fake_owner_tell(MESH_AOS_EVENT_PACKET, 1U));   /* the small one is lost */
    TEST_ASSERT_EQUAL_UINT(1U, t_waiting(0));
    TEST_ASSERT_EQUAL_UINT(1U, t_waiting(1));
    TEST_ASSERT_EQUAL_UINT(0U, t_waiting(T_AO_SMALL));

    /* A queue filled up to the margin: KODE_SYS_CMD_MARGIN slots free, not one more than it keeps. */
    (void)t_drain(0);
    unsigned const room = t_free_slots(0);
    t_fill(0, room - KODE_SYS_CMD_MARGIN);
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_CMD_MARGIN, t_free_slots(0));
    TEST_ASSERT_EQUAL_UINT8(2U, fake_owner_tell(MESH_AOS_EVENT_GONE, 0U));   /* 0 within the margin, and the small one */
    t_drain(0);
    t_fill(0, room - KODE_SYS_CMD_MARGIN - 1U);   /* one more slot than the margin: it fits */
    TEST_ASSERT_EQUAL_UINT8(1U, fake_owner_tell(MESH_AOS_EVENT_GONE, 0U));
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_CMD_MARGIN, t_free_slots(0));
}

TEST_CASE("news: a dry pool loses the news, counted, and takes nothing", "[sys][news]")
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_begin());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(0)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_subscribe(t_ao(1)));
    t_pool_hog(0U);
    TEST_ASSERT_EQUAL_UINT8(2U, fake_owner_tell(MESH_AOS_EVENT_PRESENT, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, t_waiting(0));
    t_pool_release();
}

TEST_CASE("news: an answer signal is never a news and a news is never an answer", "[sys][news]")
{
    /* No signal is both: the news come first in the range, the answers after them. */
    for (unsigned n = 0U; n < (sizeof(NEWS) / sizeof(NEWS[0])); n++) {
        for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
            TEST_ASSERT_NOT_EQUAL(NEWS[n].sig, T_REQUESTS[k].rsp_sig);
        }
    }
}

TEST_CASE("signals: this SDK's own range, from MAX_SIG + the offset, distinct and inside 16 bits", "[sys][signals]")
{
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MAX_SIG + (uint32_t)CONFIG_MESH_SIG_OFFSET, MESH_SIG_FIRST);
    TEST_ASSERT_EQUAL_UINT32(MESH_SIG_FIRST, MESH_MODULE_FOUND_SIG);
    TEST_ASSERT_TRUE(MESH_SIG_FIRST >= (uint32_t)MAX_SIG + 1000U);
    TEST_ASSERT_TRUE((uint32_t)MESH_SIG_END <= 65535U);
    TEST_ASSERT_EQUAL_UINT32(12U + 15U, MESH_SIG_COUNT);
    TEST_ASSERT_TRUE(MESH_SIG_COUNT <= 100U);   /* the range is 100 wide: another SDK starts above it */
}

/* ---- Immediates ---------------------------------------------------------------------------------- */

TEST_CASE("immediate: status_read carries the antennas: confirmed, capped and each band suspected", "[sys][immediate]")
{
    mesh_status_t st;

    g_owner.st.radio_ready        = true;
    g_owner.st.antennas_confirmed = true;
    g_owner.st.power_capped       = true;
    g_owner.st.antenna_suspected  = 1U << MESH_BAND_SUB_GHZ;
    (void)memset(&st, 0xA5, sizeof(st));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_status_read(&st));
    TEST_ASSERT_TRUE(st.antennas_confirmed);
    TEST_ASSERT_TRUE(st.power_capped);
    TEST_ASSERT_TRUE(st.antenna_suspected[MESH_BAND_SUB_GHZ]);
    TEST_ASSERT_FALSE(st.antenna_suspected[MESH_BAND_2_4_GHZ]);

    g_owner.st.antennas_confirmed = false;
    g_owner.st.power_capped       = false;
    g_owner.st.antenna_suspected  = 1U << MESH_BAND_2_4_GHZ;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_status_read(&st));
    TEST_ASSERT_FALSE(st.antennas_confirmed);
    TEST_ASSERT_FALSE(st.power_capped);
    TEST_ASSERT_FALSE(st.antenna_suspected[MESH_BAND_SUB_GHZ]);
    TEST_ASSERT_TRUE(st.antenna_suspected[MESH_BAND_2_4_GHZ]);
}

TEST_CASE("immediate: is_present, status_read, header_position and why_text", "[sys][immediate]")
{
    TEST_ASSERT_TRUE(mesh_sys_is_present());
    g_owner.st.present = false;
    TEST_ASSERT_FALSE(mesh_sys_is_present());

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_status_read(NULL));
    g_owner.st.present = true;
    g_owner.st.radio_ready = true;
    g_owner.st.power_cdbm = 850;
    g_owner.st.power_applied_cdbm = 800;
    g_owner.st.supply = MESH_SUPPLY_ON;
    g_owner.st.events_dropped = 3U;
    g_owner.st.news_dropped = 4U;
    g_owner.st.answers_dropped = 5U;
    g_owner.st.rssi_valid = true;
    g_owner.st.last_rssi_dbm = -91.5f;
    mesh_status_t st;
    (void)memset(&st, 0xA5, sizeof(st));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_status_read(&st));
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL_INT32(85, st.power_dbm_x10);
    TEST_ASSERT_EQUAL_INT32(80, st.power_applied_dbm_x10);
    TEST_ASSERT_EQUAL(MESH_SUPPLY_ON, st.supply);
    TEST_ASSERT_EQUAL_UINT32(3U, st.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(4U, st.news_dropped);
    TEST_ASSERT_EQUAL_UINT32(5U, st.answers_dropped);
    TEST_ASSERT_TRUE(st.rssi_valid);
    TEST_ASSERT_EQUAL_FLOAT(-91.5f, st.last_rssi_dbm);

    g_owner.pos_ret = 7U;
    TEST_ASSERT_EQUAL_UINT8(7U, mesh_sys_header_position(7U));
    g_owner.pos_ret = 0U;
    TEST_ASSERT_EQUAL_UINT8(0U, mesh_sys_header_position(7U));

    for (unsigned w = 0U; w < MESH_WHY_COUNT; w++) {
        TEST_ASSERT_NOT_NULL(mesh_sys_why_text((mesh_why_t)w));
        TEST_ASSERT_TRUE(mesh_sys_why_text((mesh_why_t)w)[0] != '\0');
    }
    TEST_ASSERT_EQUAL_STRING("unknown", mesh_sys_why_text((mesh_why_t)200));
}

TEST_CASE("immediate: radio_read is OK with a packet, NO_RESOURCES with none, DENIED for NULL or before the owner", "[sys][immediate]")
{
    mesh_packet_t rx;
    (void)memset(&rx, 0, sizeof(rx));

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_read(NULL));
    g_owner.packet_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_radio_read(&rx));
    TEST_ASSERT_EQUAL_UINT8(0U, rx.length);   /* untouched */
    g_owner.packet_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_read(&rx));

    g_owner.packet_rc = KODE_OK;
    g_owner.packet.length = 3U;
    g_owner.packet.data[0] = 9U;
    g_owner.packet.data[2] = 7U;
    g_owner.packet.crc_valid = true;
    g_owner.packet.rssi_dbm = -80.0f;
    g_owner.packet.snr_db = 6.5f;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_read(&rx));
    TEST_ASSERT_EQUAL_UINT8(3U, rx.length);
    TEST_ASSERT_EQUAL_UINT8(9U, rx.data[0]);
    TEST_ASSERT_EQUAL_UINT8(7U, rx.data[2]);
    TEST_ASSERT_TRUE(rx.crc_valid);
    TEST_ASSERT_EQUAL_FLOAT(-80.0f, rx.rssi_dbm);
    TEST_ASSERT_EQUAL_FLOAT(6.5f, rx.snr_db);
}

TEST_CASE("immediate: radio_rssi is OK with a sample, NO_RESOURCES before the first, DENIED with the receiver off", "[sys][immediate]")
{
    float dbm = 0.0f;

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_rssi(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_rssi(&dbm));   /* receiver off */
    g_owner.st.receiving = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_radio_rssi(&dbm));
    g_owner.st.rssi_valid = true;
    g_owner.st.last_rssi_dbm = -101.25f;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_rssi(&dbm));
    TEST_ASSERT_EQUAL_FLOAT(-101.25f, dbm);
    g_owner.st.present = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_rssi(&dbm));
}

TEST_CASE("immediate: radio_channel_busy reads the status - arriving or a search - and is DENIED without a setting", "[sys][immediate]")
{
    bool busy = true;

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_channel_busy(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_channel_busy(&busy));   /* no setting */
    TEST_ASSERT_TRUE(busy);                                               /* untouched */
    g_owner.st.radio_ready = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_channel_busy(&busy));
    TEST_ASSERT_FALSE(busy);
    g_owner.st.arriving = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_channel_busy(&busy));
    TEST_ASSERT_TRUE(busy);
    g_owner.st.arriving = false;
    g_owner.st.cad_running = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_channel_busy(&busy));
    TEST_ASSERT_TRUE(busy);
    g_owner.st.present = false;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_channel_busy(&busy));
}

TEST_CASE("immediate: radio_send_wait_ms is the duty budget's wait", "[sys][immediate]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_sys_radio_send_wait_ms());
    g_owner.st.duty_wait_ms = 61234U;
    TEST_ASSERT_EQUAL_UINT32(61234U, mesh_sys_radio_send_wait_ms());
}

TEST_CASE("immediate: radio_config_default is the region's for each band, DENIED for NULL or a band that is not", "[sys][immediate]")
{
    mesh_radio_config_t c;

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_config_default(MESH_BAND_SUB_GHZ, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_radio_config_default(MESH_BAND_COUNT, &c));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_config_default(MESH_BAND_SUB_GHZ, &c));
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_SUB_GHZ_CENTRE_HZ, c.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_SUB_GHZ_BANDWIDTH_HZ, c.bandwidth_hz);
    TEST_ASSERT_EQUAL_UINT8(9U, c.spreading_factor);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_DBM_X10, c.power_dbm_x10);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_radio_config_default(MESH_BAND_2_4_GHZ, &c));
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_2_4_GHZ_CENTRE_HZ, c.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_2_4_GHZ_BANDWIDTH_HZ, c.bandwidth_hz);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_2_4_GHZ_DBM_X10, c.power_dbm_x10);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_CODING_RATE, c.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(MESH_REGION_PREAMBLE, c.preamble);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SYNC_WORD, c.sync_word);
}

TEST_CASE("immediate: gnss_fix is OK with a fix, NO_RESOURCES with the GNSS on and none yet, DENIED when off or NULL", "[sys][immediate]")
{
    mesh_gnss_fix_t fix;
    (void)memset(&fix, 0, sizeof(fix));

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_gnss_fix(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_sys_gnss_fix(&fix));   /* the GNSS is off */
    g_owner.st.gnss_on = true;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, mesh_sys_gnss_fix(&fix));
    TEST_ASSERT_FALSE(fix.valid);                              /* filled: what is known */
    g_owner.st.fix.fix = true;
    g_owner.st.fix.lat = 39.4699;
    g_owner.st.fix.lon = -0.3763;
    g_owner.st.fix.sats_used = 9U;
    g_owner.st.fix_age_ms = 800U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_sys_gnss_fix(&fix));
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_EQUAL_DOUBLE(39.4699, fix.latitude_deg);
    TEST_ASSERT_EQUAL_DOUBLE(-0.3763, fix.longitude_deg);
    TEST_ASSERT_EQUAL_UINT8(9U, fix.satellites_used);
    TEST_ASSERT_EQUAL_UINT32(800U, fix.age_ms);
}

TEST_CASE("immediate: gnss_pps is the line", "[sys][immediate]")
{
    TEST_ASSERT_FALSE(mesh_sys_gnss_pps());
    g_owner.st.pps_high = true;
    TEST_ASSERT_TRUE(mesh_sys_gnss_pps());
}

/* QP's assert: reaching it is a bug in the layer, so the suite fails on the spot. */
Q_NORETURN Q_onError(char const *module, int_t const id)
{
    (void)printf("Q_onError %s:%d\n", module, (int)id);
    TEST_FAIL_MESSAGE("QP asserted");
    for (;;) { }
}
