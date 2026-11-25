/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_sys.c
 * @brief The extended layer: mesh_sys.h, the calls of an Active Object.
 *
 * Nothing here waits. A command is posted to the owner (mesh_aos) and returns; a request is
 * posted with an answer event already taken from the pool, so the answer cannot fail for want
 * of one, and the owner's done function (answered(), on the owner's task) fills it in, opens
 * the asker's door and posts it to the asker. The news go the other way: the owner tells
 * news_sink(), which posts one event to each subscribed AO, leaving a margin.
 *
 * The doors are kode-sdk's (its kaos_ask.c): one flag per kind of request and per asker,
 * shut by the request and opened by its answer just before it is posted.
 */
#include "mesh_sys.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "kode_log.h"

#include "mesh_aos.h"

static const char *const TAG = "mesh_sys";

/* ---- The doors -------------------------------------------------------------- */

/** @brief The kinds of request, in the order of their answers' signals. */
typedef enum {
    CLASS_RADIO_BEGIN = 0,
    CLASS_RADIO_CONFIGURE,
    CLASS_RADIO_CALIBRATE,
    CLASS_RADIO_RETUNE,
    CLASS_RADIO_END,
    CLASS_RADIO_SEND,
    CLASS_RADIO_SEND_PREAMBLE,
    CLASS_RADIO_CAD,
    CLASS_RADIO_CAD_RECEIVE,
    CLASS_RADIO_TEST,
    CLASS_GNSS_BEGIN,
    CLASS_GNSS_END,
    CLASS_GNSS_RESET,
    CLASS_I2C_LEND,
    CLASS_I2C_RETURN,
    CLASS_COUNT
} class_t;

_Static_assert((uint32_t)CLASS_COUNT == (uint32_t)(MESH_I2C_RETURN_RSP_SIG - MESH_RADIO_BEGIN_RSP_SIG) + 1U,
               "a class for each answer signal");
_Static_assert((uint32_t)MESH_RADIO_BEGIN_RSP_SIG + (uint32_t)CLASS_RADIO_SEND == (uint32_t)MESH_RADIO_SEND_RSP_SIG,
               "the classes follow the signals");
_Static_assert((uint32_t)MESH_RADIO_BEGIN_RSP_SIG + (uint32_t)CLASS_I2C_LEND == (uint32_t)MESH_I2C_LEND_RSP_SIG,
               "the classes follow the signals");

/** @brief Asker rows: one per QP priority, 1..QF_MAX_ACTIVE, indexed by it. */
#define ASKERS  ((size_t)QF_MAX_ACTIVE + 1U)

static atomic_bool s_door[CLASS_COUNT][ASKERS];

/** @brief Whether @p who is an Active Object that has been started. */
static bool asker_started(QActive const *who)
{
    return (who != NULL) && (who->prio != 0U) && (who->prio <= QF_MAX_ACTIVE);
}

static bool door_take(class_t cls, QActive const *who)
{
    return !atomic_exchange(&s_door[cls][who->prio], true);
}

static void door_release(class_t cls, QActive const *who)
{
    atomic_store(&s_door[cls][who->prio], false);
}

/* ---- The answers: one pool event for each request, taken before it is posted ---- */

/** @brief Pool blocks an answer or a news leaves for the rest of the system. */
#define POOL_MARGIN  2U

/** @brief What fields a request's answer has. */
typedef enum { ANSWER_PLAIN = 0, ANSWER_SEND, ANSWER_CAD, ANSWER_I2C } answer_kind_t;

/* An answer event is its public type followed by what the SDK needs to post it: the asker.
 * The public type is the first member, so the pointer the asker is given is the pool's. */
typedef struct { mesh_sys_answer_t      pub; QActive *me; } plain_t;
typedef struct { mesh_sys_send_answer_t pub; QActive *me; } send_t;
typedef struct { mesh_sys_cad_answer_t  pub; QActive *me; } cad_t;
typedef struct { mesh_sys_i2c_answer_t  pub; QActive *me; } i2c_t;

static const struct {
    answer_kind_t kind;
    QSignal       sig;
} CLASSES[CLASS_COUNT] = {
    [CLASS_RADIO_BEGIN]         = { ANSWER_PLAIN, MESH_RADIO_BEGIN_RSP_SIG },
    [CLASS_RADIO_CONFIGURE]     = { ANSWER_PLAIN, MESH_RADIO_CONFIGURE_RSP_SIG },
    [CLASS_RADIO_CALIBRATE]     = { ANSWER_PLAIN, MESH_RADIO_CALIBRATE_RSP_SIG },
    [CLASS_RADIO_RETUNE]        = { ANSWER_PLAIN, MESH_RADIO_RETUNE_RSP_SIG },
    [CLASS_RADIO_END]           = { ANSWER_PLAIN, MESH_RADIO_END_RSP_SIG },
    [CLASS_RADIO_SEND]          = { ANSWER_SEND,  MESH_RADIO_SEND_RSP_SIG },
    [CLASS_RADIO_SEND_PREAMBLE] = { ANSWER_SEND,  MESH_RADIO_SEND_PREAMBLE_RSP_SIG },
    [CLASS_RADIO_CAD]           = { ANSWER_CAD,   MESH_RADIO_CAD_RSP_SIG },
    [CLASS_RADIO_CAD_RECEIVE]   = { ANSWER_CAD,   MESH_RADIO_CAD_RECEIVE_RSP_SIG },
    [CLASS_RADIO_TEST]          = { ANSWER_PLAIN, MESH_RADIO_TEST_RSP_SIG },
    [CLASS_GNSS_BEGIN]          = { ANSWER_PLAIN, MESH_GNSS_BEGIN_RSP_SIG },
    [CLASS_GNSS_END]            = { ANSWER_PLAIN, MESH_GNSS_END_RSP_SIG },
    [CLASS_GNSS_RESET]          = { ANSWER_PLAIN, MESH_GNSS_RESET_RSP_SIG },
    [CLASS_I2C_LEND]            = { ANSWER_I2C,   MESH_I2C_LEND_RSP_SIG },
    [CLASS_I2C_RETURN]          = { ANSWER_PLAIN, MESH_I2C_RETURN_RSP_SIG },
};

/** @brief The answer of class @p cls for @p me, empty; NULL when the pool is dry. */
static QEvt *answer_new(class_t cls, QActive *me)
{
    QSignal const sig = CLASSES[cls].sig;

    switch (CLASSES[cls].kind) {
    case ANSWER_SEND: {
        send_t *const e = Q_NEW_X(send_t, POOL_MARGIN, sig);
        if (e == NULL) { return NULL; }
        e->me = me; e->pub.rc = KODE_OK; e->pub.why = MESH_WHY_NONE; e->pub.airtime_ms = 0U; e->pub.wait_ms = 0U;
        return &e->pub.super;
    }
    case ANSWER_CAD: {
        cad_t *const e = Q_NEW_X(cad_t, POOL_MARGIN, sig);
        if (e == NULL) { return NULL; }
        e->me = me; e->pub.rc = KODE_OK; e->pub.why = MESH_WHY_NONE; e->pub.detected = false;
        return &e->pub.super;
    }
    case ANSWER_I2C: {
        i2c_t *const e = Q_NEW_X(i2c_t, POOL_MARGIN, sig);
        if (e == NULL) { return NULL; }
        e->me = me; e->pub.rc = KODE_OK; e->pub.why = MESH_WHY_NONE; e->pub.scl_gpio = 0U; e->pub.sda_gpio = 0U;
        return &e->pub.super;
    }
    case ANSWER_PLAIN:
    default: {
        plain_t *const e = Q_NEW_X(plain_t, POOL_MARGIN, sig);
        if (e == NULL) { return NULL; }
        e->me = me; e->pub.rc = KODE_OK; e->pub.why = MESH_WHY_NONE;
        return &e->pub.super;
    }
    }
}

/**
 * @brief The owner's done function, on its task, exactly once for each request taken: fill the
 *        answer in, open the asker's door and post it with no margin. A queue that is completely
 *        full all the same loses it - counted (status.answers_dropped) and said, never QP's assert.
 */
static void answered(void *ask, const mesh_aos_answer_t *a)
{
    QEvt * const e   = ask;
    class_t const cls = (class_t)((uint32_t)e->sig - (uint32_t)MESH_RADIO_BEGIN_RSP_SIG);
    QActive     *me  = NULL;
    mesh_why_t const why = (mesh_why_t)a->why;

    switch (CLASSES[cls].kind) {
    case ANSWER_SEND: {
        send_t *const w = (send_t *)ask;
        w->pub.rc  = a->rc;
        w->pub.why = why;
        if (a->rc == KODE_OK) {
            w->pub.airtime_ms = a->out1;
        } else if (why == MESH_WHY_DUTY) {
            w->pub.wait_ms = a->out1;
        } else {
            /* nothing to hand back */
        }
        me = w->me;
        break;
    }
    case ANSWER_CAD: {
        cad_t *const w = (cad_t *)ask;
        w->pub.rc       = a->rc;
        w->pub.why      = why;
        w->pub.detected = (a->rc == KODE_OK) && (a->out1 != 0U);
        me = w->me;
        break;
    }
    case ANSWER_I2C: {
        i2c_t *const w = (i2c_t *)ask;
        w->pub.rc  = a->rc;
        w->pub.why = why;
        if (a->rc == KODE_OK) {
            w->pub.scl_gpio = (uint8_t)a->out1;
            w->pub.sda_gpio = (uint8_t)a->out2;
        }
        me = w->me;
        break;
    }
    case ANSWER_PLAIN:
    default: {
        plain_t *const w = (plain_t *)ask;
        w->pub.rc  = a->rc;
        w->pub.why = why;
        me = w->me;
        break;
    }
    }
    door_release(cls, me);
    /* No margin: the answer is owed. A failed post hands the block back to its pool. */
    QSignal const sig = e->sig;
    if (!QACTIVE_POST_X(me, e, 0U, NULL)) {
        mesh_aos_answer_dropped();
        KLOG_W(TAG, "an answer (signal %u) found its Active Object's queue full: lost", (unsigned)sig);
    }
}

/**
 * @brief Ask the owner for @p req on behalf of @p me, through @p cls's door.
 * @return As the header's requests' rule.
 */
static kode_status_t ask(QActive *me, class_t cls, const mesh_aos_request_t *req)
{
    if (!asker_started(me)) {
        return KODE_DENIED;
    }
    if (!door_take(cls, me)) {
        return KODE_DENIED;
    }
    QEvt *const rsp = answer_new(cls, me);
    if (rsp == NULL) {
        door_release(cls, me);
        return KODE_NO_RESOURCES;
    }
    kode_status_t const rc = mesh_aos_ask(req, rsp, answered);
    if (rc != KODE_OK) {
        QF_gc(rsp);   /* never posted: back to the pool */
        door_release(cls, me);
    }
    return rc;
}

/* ---- The news --------------------------------------------------------------- */

/** @brief Slots a news leaves free in a subscriber's queue. */
#define NEWS_MARGIN  KODE_SYS_CMD_MARGIN

static _Atomic(QActive *) s_subscriber[MESH_SUBSCRIBERS_MAX];

/** @brief The signal of an owner's event number; 0 for one that is not news. */
static QSignal news_signal(uint8_t ev)
{
    switch (ev) {
    case MESH_AOS_EVENT_PRESENT:           return MESH_MODULE_FOUND_SIG;
    case MESH_AOS_EVENT_GONE:              return MESH_MODULE_LOST_SIG;
    case MESH_AOS_EVENT_PACKET:            return MESH_PACKET_RECEIVED_SIG;
    case MESH_AOS_EVENT_LISTEN_END:        return MESH_LISTEN_ENDED_SIG;
    case MESH_AOS_EVENT_RADIO_ERROR:       return MESH_RADIO_FAILED_SIG;
    case MESH_AOS_EVENT_WRONG_MODULE:      return MESH_WRONG_MODULE_SIG;
    case MESH_AOS_EVENT_TEST_END:          return MESH_TEST_ENDED_SIG;
    case MESH_AOS_EVENT_PREAMBLE:          return MESH_PREAMBLE_DETECTED_SIG;
    case MESH_AOS_EVENT_HEADER:            return MESH_HEADER_RECEIVED_SIG;
    case MESH_AOS_EVENT_HEADER_BAD:        return MESH_HEADER_FAILED_SIG;
    case MESH_AOS_EVENT_SETTING_LOST:      return MESH_SETTING_LOST_SIG;
    case MESH_AOS_EVENT_SETTING_RESTORED:  return MESH_SETTING_RESTORED_SIG;
    default:                               return 0U;
    }
}

/**
 * @brief The owner's sink, on its task: one event for each subscriber, posted with the margin.
 * @return How many subscribers it did not reach, for status.news_dropped.
 */
static uint8_t news_sink(uint8_t ev, uint32_t value)
{
    QSignal const sig  = news_signal(ev);
    uint8_t       lost = 0U;

    if (sig == 0U) {
        return 0U;
    }
    for (size_t i = 0U; i < MESH_SUBSCRIBERS_MAX; i++) {
        QActive *const sub = atomic_load(&s_subscriber[i]);
        if (sub == NULL) {
            continue;
        }
        mesh_sys_news_t *const e = Q_NEW_X(mesh_sys_news_t, POOL_MARGIN, sig);
        if (e == NULL) {
            lost++;
            continue;
        }
        e->value = (int32_t)value;
        if (!QACTIVE_POST_X(sub, &e->super, NEWS_MARGIN, NULL)) {
            lost++;   /* QACTIVE_POST_X has put the event back in the pool itself */
        }
    }
    return lost;
}

kode_status_t mesh_sys_subscribe(QActive *me)
{
    if (!asker_started(me)) {
        return KODE_DENIED;
    }
    /* mesh.h's mesh_begin() starts the owner without this layer's mesh_sys_begin(): connect the
     * sink here too, or a subscriber is told KODE_OK and hears nothing. The
     * owner calls the sink only once it runs, after kode_init(), so the pools are there. */
    mesh_aos_news_connect(news_sink);
    for (size_t i = 0U; i < MESH_SUBSCRIBERS_MAX; i++) {
        if (atomic_load(&s_subscriber[i]) == me) {
            return KODE_OK;   /* already */
        }
    }
    for (size_t i = 0U; i < MESH_SUBSCRIBERS_MAX; i++) {
        QActive *free_slot = NULL;
        if (atomic_compare_exchange_strong(&s_subscriber[i], &free_slot, me)) {
            return KODE_OK;
        }
    }
    return KODE_NO_RESOURCES;
}

kode_status_t mesh_sys_unsubscribe(QActive *me)
{
    if (me == NULL) {
        return KODE_DENIED;
    }
    for (size_t i = 0U; i < MESH_SUBSCRIBERS_MAX; i++) {
        QActive *mine = me;
        if (atomic_compare_exchange_strong(&s_subscriber[i], &mine, NULL)) {
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

/* ---- Begin and end ---------------------------------------------------------- */

kode_status_t mesh_sys_begin(void)
{
    if (!mesh_aos_kode_ready()) {
        KLOG_E(TAG, "mesh_sys_begin() before kode_init(): the pools the owner posts from do not exist yet");
        return KODE_DENIED;
    }
    mesh_aos_news_connect(news_sink);
    return mesh_aos_start();
}

kode_status_t mesh_sys_end(void)
{
    return mesh_aos_end_post();
}

/* ---- Commands --------------------------------------------------------------- */

kode_status_t mesh_sys_radio_standby(void)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_STANDBY };
    return mesh_aos_post(&req);
}

kode_status_t mesh_sys_radio_receive(bool on)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_RECEIVE, .u = on ? 1U : 0U };
    return mesh_aos_post(&req);
}

kode_status_t mesh_sys_radio_listen(uint32_t ms)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_listen(&req, ms)) {
        return KODE_DENIED;
    }
    return mesh_aos_post(&req);
}

kode_status_t mesh_sys_stop(void)
{
    return mesh_aos_stop();
}

kode_status_t mesh_sys_radio_antennas_confirm(bool fitted)
{
    return mesh_aos_antennas_confirm(fitted);
}

/* ---- Requests: the radio ---------------------------------------------------- */

kode_status_t mesh_sys_radio_begin(QActive *me, mesh_band_t band, uint8_t spreading_factor, int32_t power_dbm_x10)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_begin(&req, band, spreading_factor, power_dbm_x10)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_BEGIN, &req);
}

kode_status_t mesh_sys_radio_configure(QActive *me, const mesh_radio_config_t *config)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_config(&req, MESH_REQUEST_RADIO_CONFIGURE, config)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_CONFIGURE, &req);
}

kode_status_t mesh_sys_radio_calibrate(QActive *me, const uint32_t *frequencies_hz, uint8_t count)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_calibrate(&req, frequencies_hz, count)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_CALIBRATE, &req);
}

kode_status_t mesh_sys_radio_retune(QActive *me, const mesh_radio_config_t *config)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_config(&req, MESH_REQUEST_RADIO_RETUNE, config)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_RETUNE, &req);
}

kode_status_t mesh_sys_radio_end(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_END };
    return ask(me, CLASS_RADIO_END, &req);
}

kode_status_t mesh_sys_radio_send(QActive *me, const uint8_t *data, uint8_t length)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_send(&req, data, length, 0U)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_SEND, &req);
}

kode_status_t mesh_sys_radio_send_preamble(QActive *me, const uint8_t *data, uint8_t length, uint16_t preamble)
{
    mesh_aos_request_t req;

    if ((preamble == 0U) || !mesh_aos_arg_radio_send(&req, data, length, preamble)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_SEND_PREAMBLE, &req);
}

kode_status_t mesh_sys_radio_cad(QActive *me, uint8_t symbols, bool fast)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_cad(&req, symbols, fast)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_CAD, &req);
}

kode_status_t mesh_sys_radio_cad_receive(QActive *me, uint8_t symbols, bool fast)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_cad_receive(&req, symbols, fast)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_CAD_RECEIVE, &req);
}

kode_status_t mesh_sys_radio_test(QActive *me, mesh_test_signal_t mode, uint32_t ms)
{
    mesh_aos_request_t req;

    if (!mesh_aos_arg_radio_test(&req, mode, ms)) {
        return KODE_DENIED;
    }
    return ask(me, CLASS_RADIO_TEST, &req);
}

/* ---- Requests: the GNSS and the I2C pair ------------------------------------ */

kode_status_t mesh_sys_gnss_begin(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_GNSS_BEGIN };
    return ask(me, CLASS_GNSS_BEGIN, &req);
}

kode_status_t mesh_sys_gnss_end(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_GNSS_END };
    return ask(me, CLASS_GNSS_END, &req);
}

kode_status_t mesh_sys_gnss_reset(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_GNSS_RESET };
    return ask(me, CLASS_GNSS_RESET, &req);
}

kode_status_t mesh_sys_i2c_lend(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_I2C_LEND };
    return ask(me, CLASS_I2C_LEND, &req);
}

kode_status_t mesh_sys_i2c_return(QActive *me)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_I2C_RETURN };
    return ask(me, CLASS_I2C_RETURN, &req);
}

/* ---- Immediate calls -------------------------------------------------------- */

bool mesh_sys_is_present(void)
{
    return mesh_aos_present();
}

kode_status_t mesh_sys_status_read(mesh_status_t *out)
{
    if (out == NULL) {
        return KODE_DENIED;
    }
    mesh_aos_status_public(out);
    return KODE_OK;
}

uint8_t mesh_sys_header_position(uint8_t pos)
{
    return mesh_aos_header_position(pos);
}

const char *mesh_sys_why_text(mesh_why_t why)
{
    return mesh_aos_why_text((uint8_t)why);
}

kode_status_t mesh_sys_radio_read(mesh_packet_t *rx)
{
    if (rx == NULL) {
        return KODE_DENIED;
    }
    return mesh_aos_packet_public(rx);
}

kode_status_t mesh_sys_radio_rssi(float *dbm)
{
    mesh_aos_status_t st;

    if (dbm == NULL) {
        return KODE_DENIED;
    }
    mesh_aos_status(&st);
    if (!st.present || !st.receiving) {
        return KODE_DENIED;
    }
    if (!st.rssi_valid) {
        return KODE_NO_RESOURCES;
    }
    *dbm = st.last_rssi_dbm;
    return KODE_OK;
}

kode_status_t mesh_sys_radio_channel_busy(bool *busy)
{
    mesh_aos_status_t st;

    if (busy == NULL) {
        return KODE_DENIED;
    }
    mesh_aos_status(&st);
    if (!st.present || !st.radio_ready) {
        return KODE_DENIED;
    }
    *busy = st.arriving || st.cad_running;
    return KODE_OK;
}

uint32_t mesh_sys_radio_send_wait_ms(void)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    return st.duty_wait_ms;
}

kode_status_t mesh_sys_radio_config_default(mesh_band_t band, mesh_radio_config_t *out)
{
    return mesh_aos_arg_radio_config_default(band, out) ? KODE_OK : KODE_DENIED;
}

kode_status_t mesh_sys_gnss_fix(mesh_gnss_fix_t *fix)
{
    mesh_aos_status_t st;

    if (fix == NULL) {
        return KODE_DENIED;
    }
    mesh_aos_status(&st);
    if (!st.gnss_on) {
        return KODE_DENIED;
    }
    mesh_aos_fix_public(fix);
    return fix->valid ? KODE_OK : KODE_NO_RESOURCES;
}

bool mesh_sys_gnss_pps(void)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    return st.pps_high;
}
