/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file owner_harness.c
 * @brief See owner_harness.h.
 */
#include "owner_harness.h"

#include <pthread.h>
#include <string.h>

#include "unity.h"

#include "fake_mesh_ao.h"
#include "mesh_ao.h"

#define EVENTS_MAX  64U

static struct {
    uint8_t  ev;
    uint32_t arg;
} s_ev[EVENTS_MAX];
static unsigned        s_ev_n;
static pthread_mutex_t s_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static bool            s_started;

/* The owner's counts run "since the module came"; a case wants them since the
 * case began, so h_status() takes these off. */
static uint32_t s_base_rx;
static uint32_t s_base_tx;
static uint32_t s_base_dropped;

void (*volatile g_event_hook)(uint8_t ev, uint32_t arg);
unsigned g_away_lost_n;
uint32_t g_away_lost_arg;

/* The extended layer's sink: write down what it is told, and answer that none was lost. */
static uint8_t sink(uint8_t ev, uint32_t arg)
{
    (void)pthread_mutex_lock(&s_ev_mu);
    if (s_ev_n < EVENTS_MAX) {
        s_ev[s_ev_n].ev  = ev;
        s_ev[s_ev_n].arg = arg;
        s_ev_n++;
    }
    (void)pthread_mutex_unlock(&s_ev_mu);
    void (*const hook)(uint8_t, uint32_t) = g_event_hook;
    if (hook != NULL) {
        hook(ev, arg);
    }
    return 0U;
}

void h_events_register(void)
{
    mesh_aos_news_connect(sink);
}

void h_events_clear(void)
{
    (void)pthread_mutex_lock(&s_ev_mu);
    s_ev_n = 0U;
    (void)pthread_mutex_unlock(&s_ev_mu);
}

unsigned h_events(uint8_t ev)
{
    unsigned n = 0U;
    (void)pthread_mutex_lock(&s_ev_mu);
    for (unsigned i = 0U; i < s_ev_n; i++) {
        if (s_ev[i].ev == ev) {
            n++;
        }
    }
    (void)pthread_mutex_unlock(&s_ev_mu);
    return n;
}

bool h_event_with(uint8_t ev, uint32_t arg)
{
    bool found = false;
    (void)pthread_mutex_lock(&s_ev_mu);
    for (unsigned i = 0U; i < s_ev_n; i++) {
        if ((s_ev[i].ev == ev) && (s_ev[i].arg == arg)) {
            found = true;
        }
    }
    (void)pthread_mutex_unlock(&s_ev_mu);
    return found;
}

uint32_t h_event_arg(uint8_t ev)
{
    uint32_t arg = 0U;
    (void)pthread_mutex_lock(&s_ev_mu);
    for (unsigned i = 0U; i < s_ev_n; i++) {
        if (s_ev[i].ev == ev) {
            arg = s_ev[i].arg;
        }
    }
    (void)pthread_mutex_unlock(&s_ev_mu);
    return arg;
}

mesh_aos_status_t h_status(void)
{
    mesh_aos_status_t st;
    mesh_aos_status(&st);
    st.packets_received   -= s_base_rx;
    st.packets_sent   -= s_base_tx;
    st.packets_dropped -= s_base_dropped;
    return st;
}

kode_status_t h_call(mesh_request_kind_t kind, uint32_t ms)
{
    const mesh_aos_request_t req = { .kind = kind };
    return mesh_aos_call(&req, ms, NULL, NULL);
}

kode_status_t h_begin(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm)
{
    const mesh_aos_request_t req = {
        .kind = MESH_REQUEST_RADIO_BEGIN, .a = (uint8_t)band, .u = spreading_factor, .i = power_cdbm,
    };
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

/* A send's call is answered when the packet is gone, so a case that wants "the packet is on air" can
 * not wait for the call on its own task: the call runs on a task of its own (the caller that waits),
 * and h_send() returns when it is answered or when the owner has put the packet on air. */
static struct {
    h_bg_t                bg;
    uint8_t               data[MESH_AOS_PAYLOAD_MAX];
    const uint8_t        *use;
    uint8_t               len;
    uint16_t              preamble;
    uint32_t              ms;
    volatile kode_status_t rc;
    volatile uint32_t     out1;
    volatile uint8_t      why;
    volatile bool         active;
} s_send;

static void send_body(void *arg)
{
    (void)arg;
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_SEND, .data = s_send.use, .len = s_send.len,
                                       .preamble = s_send.preamble };
    uint32_t out1 = 0U;
    kode_status_t const rc = mesh_aos_call(&req, s_send.ms, &out1, NULL);
    s_send.out1 = out1;
    s_send.why  = mesh_aos_why();   /* the reason is the calling task's: kept for the case's task */
    s_send.rc   = rc;
}

void h_send_settle(void)
{
    if (s_send.active) {
        (void)mesh_aos_stop();   /* a packet still on air is cut short and its call answered */
        TEST_ASSERT_TRUE_MESSAGE(h_bg_join(&s_send.bg, 20000U), "a send's call never came back");
        s_send.active = false;
    }
}

kode_status_t h_send(const uint8_t *data, uint8_t len, uint32_t ms)
{
    return h_send_preamble(data, len, 0U, ms);
}

kode_status_t h_send_preamble(const uint8_t *data, uint8_t len, uint16_t preamble, uint32_t ms)
{
    h_send_settle();
    if (mesh_ao_caller_is_ao(NULL)) {
        /* An Active Object's call is refused on the spot: no task of its own to see that. */
        const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_SEND, .data = data, .len = len, .preamble = preamble };
        return mesh_aos_call(&req, ms, NULL, NULL);
    }
    if ((data != NULL) && (len > 0U)) {
        (void)memcpy(s_send.data, data, len);
        s_send.use = s_send.data;
    } else {
        s_send.use = NULL;
    }
    s_send.len    = len;
    s_send.preamble = preamble;
    s_send.ms     = ms;
    s_send.rc     = KODE_TIMEOUT;
    s_send.out1   = 0U;
    s_send.active = true;
    h_bg_start(&s_send.bg, send_body, NULL);
    (void)WAIT_FOR(s_send.bg.done || h_status().sending, ms + MESH_AOS_CALL_GRACE_MS + 2000U);
    if (s_send.bg.done) {
        s_send.active = false;
        mesh_aos_why_note(s_send.why);
        return s_send.rc;
    }
    mesh_aos_why_note(MESH_WHY_NONE);
    return KODE_OK;   /* on air; the answer comes when it is gone: h_send_answer() */
}

kode_status_t h_send_again(const uint8_t *data, uint8_t len, uint32_t ms)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_SEND, .data = data, .len = len };
    return mesh_aos_call(&req, ms, NULL, NULL);
}

kode_status_t h_send_answer(uint32_t ms, uint32_t *out1)
{
    if (!s_send.active) {
        return s_send.rc;
    }
    if (!h_bg_join(&s_send.bg, ms)) {
        return KODE_TIMEOUT;   /* not answered yet; the send stays active */
    }
    s_send.active = false;
    if (out1 != NULL) {
        *out1 = s_send.out1;
    }
    mesh_aos_why_note(s_send.why);
    return s_send.rc;
}

bool h_send_answered(void)
{
    return s_send.active && s_send.bg.done;
}

kode_status_t h_test(uint32_t mode, int32_t ms)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_TEST, .u = mode, .i = ms };
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

static void h_done(void *ask, const mesh_aos_answer_t *answer)
{
    h_answer_t * const to = ask;

    to->in_owner = mesh_ao_is_own_task();
    to->a        = *answer;
    to->n++;
}

kode_status_t h_ask(const mesh_aos_request_t *req, h_answer_t *to)
{
    to->n = 0U;
    return mesh_aos_ask(req, to, h_done);
}

bool h_answered(const h_answer_t *to, uint32_t ms)
{
    return WAIT_FOR(to->n >= 1U, ms);
}

kode_status_t h_receive(bool on)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_RECEIVE, .u = on ? 1U : 0U };
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

void h_module_away(void)
{
    g_ao.tick_on = true;
    pins_fake_set_mesh(false);
    g_lr.alive = false;
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_events(MESH_AOS_EVENT_GONE) == 1U, 5000U), "the owner never gave it up");
    /* The next look sees nothing on J3 and says so in the status. */
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_status().seen == MESH_MODULE_NONE, 5000U), "the owner never looked at the empty J3");
    g_away_lost_n   = h_events(MESH_AOS_EVENT_SETTING_LOST);
    g_away_lost_arg = h_event_arg(MESH_AOS_EVENT_SETTING_LOST);
    h_events_clear();   /* what follows is the look at the module that is back */
    fake_log_clear();
}

void h_module_back(void)
{
    pins_fake_set_mesh(true);
    g_lr.alive = true;
}

void harness_fresh(void)
{
    if (!s_started) {
        lr_fake_reset();
        pins_fake_set_mesh(true);
        mesh_aos_news_connect(sink);
        TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_wait_present(5000U), "the owner never found the Mesh");
        s_started = true;
    }
    /* Back to a known radio: let go of any gate, wipe the model, end whatever the
     * last case left, empty the ring, begin afresh. A case that took the module
     * away and failed before putting it back must not take the rest with it, so
     * the module is put back here, and waited for. */
    pins_fake_set_mesh(true);
    g_event_hook = NULL;
    g_plan_test_allowed = true;   /* the cases about a test signal's handling have one to handle */
    lr_fake_release();
    h_send_settle();   /* the call of a packet the last case left on air */
    lr_fake_reset();
    fake_ao_reset();
    (void)memset(&g_gnss.report, 0, sizeof(g_gnss.report));
    g_gnss.open_rc    = KODE_OK;
    g_gnss.sleep_rc   = KODE_OK;
    g_exp.refuse_lend = NULL;
    g_exp.spi_refused  = false;
    g_exp.uart_refused = false;
    g_exp.spi_id       = 2U;
    g_exp.uart_id      = 1U;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_wait_present(5000U), "the owner does not hold the module");
    (void)h_call(MESH_REQUEST_RADIO_END, 2000U);
    (void)h_call(MESH_REQUEST_GNSS_END, 2000U);   /* and nothing left to be put back */
    mesh_aos_packet_t rx;
    while (mesh_aos_packet_read(&rx) == KODE_OK) {
    }
    lr_fake_reset();
    /* Most cases speak of a module with its antennas on: the app's word,
     * given again for each case (a case may have ended the owner, which forgets it).
     * test_owner_safe_power.c takes it back where it needs to. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    lr_fake_reset();   /* the counts are for the case */
    h_events_clear();
    fake_log_clear();

    mesh_aos_status_t st;
    mesh_aos_status(&st);
    s_base_rx      = st.packets_received;
    s_base_tx      = st.packets_sent;
    s_base_dropped = st.packets_dropped;
}

static void bg_task(void *p)
{
    h_bg_t *bg = p;
    bg->fn(bg->arg);
    bg->done = true;
    vTaskDelete(NULL);
}

void h_bg_start(h_bg_t *bg, void (*fn)(void *), void *arg)
{
    bg->fn   = fn;
    bg->arg  = arg;
    bg->done = false;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(bg_task, "h_bg", 4096, bg, 3, NULL));
}

bool h_bg_join(h_bg_t *bg, uint32_t ms)
{
    return WAIT_FOR(bg->done, ms);
}
