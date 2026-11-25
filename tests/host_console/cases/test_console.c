/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console.c
 * @brief examples/console_actions over a recording mesh_sys.h: its tables kept to
 *        kodeOS's rules, every function making the mesh_sys.h call its name says
 *        (a command or an immediate on the console's task, a request on the AO's),
 *        the two that transmit held behind mesh_tx_arm, every answer and every
 *        piece of news on the log, every variable a word the console sends, in
 *        every state.
 *
 * The example is built into this file, so a case carries a request as the board
 * does: the row posts it (fake_console_run()), the Active Object's one state
 * takes it and asks the SDK (serve()), and the case plays the owner by giving
 * the answer (answer()) or the news (news()) to the same state. What a row may
 * not do is counted at each step: it never makes a request itself.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fake_console.h"
#include "fake_kode.h"
#include "fake_mesh_sys.h"

#include "console_actions_ao.c"

#define FUNCTION_COUNT 12U
#define VARIABLE_COUNT 23U

/** @brief The example's rows, as its header lists them. */
static const char *const FUNCTION_NAME[FUNCTION_COUNT] = {
    "mesh_tx_arm", "mesh_sf", "mesh_power_x10", "mesh_begin", "mesh_rx", "mesh_radio_rssi",
    "mesh_listen", "mesh_radio_send", "mesh_radio_test", "mesh_standby", "mesh_stop", "mesh_gnss",
};

static const char *const VARIABLE_NAME[VARIABLE_COUNT] = {
    "mesh_present", "mesh_seen", "mesh_tx_armed", "mesh_ready", "mesh_band_now", "mesh_sf_now",
    "mesh_power_now", "mesh_sending", "mesh_receiving", "mesh_testing", "mesh_fault",
    "mesh_duty_ms", "mesh_rx_count", "mesh_rx_dropped", "mesh_rx_waiting", "mesh_tx_count",
    "mesh_rssi_dbm", "mesh_gnss_on", "mesh_gnss_fix", "mesh_gnss_sats", "mesh_gnss_count",
    "mesh_gnss_baud", "mesh_last",
};

/** @brief Functions on the console when the initial transition ran. */
static size_t s_functions_at_initial;

/** @brief What QACTIVE_START() runs, where QP/C takes the initial transition. */
static void initial(void)
{
    s_functions_at_initial = fake_console_functions();
    TEST_ASSERT_EQUAL_UINT8(Q_TRAN(&ConsoleActions_serving), ConsoleActions_initial(&l_ao, NULL));
}

/**
 * @brief Nothing recorded, no module, every answer KODE_OK, and the example's
 *        own settings as a fresh boot has them (they are statics of the file
 *        this one includes, so the previous case's would otherwise stay).
 */
static void fakes_reset(void)
{
    fake_kode_reset();
    fake_mesh_sys_reset();
    fake_console_reset();
    g_kode.initial = initial;
    (void)memset(&l_ao, 0, sizeof(l_ao));
    l_tx_armed = false;
    l_sf       = 9U;
    l_power    = MESH_REGION_SUB_GHZ_DBM_X10;
    l_last     = 0U;
    l_disarms  = 0U;
}

/** @brief The example brought up as its main() does. */
static void console_up(void)
{
    ConsoleActions_ctor();
    ConsoleActions_start();
}

static void console_reset(void)
{
    fakes_reset();
    console_up();
}

/** @brief The AO's step for each event posted to it, oldest first, each block then freed. */
static void serve(void)
{
    QEvt const *e;
    while ((e = fake_kode_take()) != NULL) {
        TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, e));
        fake_kode_free(e);
    }
}

/** @brief The owner's answer to a request that has nothing to hand back. */
static void answer(enum_t sig, kode_status_t rc, mesh_why_t why)
{
    mesh_sys_answer_t const a = { .super = { .sig = (QSignal)sig }, .rc = rc, .why = why };
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &a.super));
}

/** @brief The owner's answer to a send. */
static void answer_send(kode_status_t rc, mesh_why_t why, uint32_t airtime_ms, uint32_t wait_ms)
{
    mesh_sys_send_answer_t const a = { .super = { .sig = (QSignal)MESH_RADIO_SEND_RSP_SIG },
                                       .rc = rc, .why = why, .airtime_ms = airtime_ms,
                                       .wait_ms = wait_ms };
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &a.super));
}

/** @brief The SDK's news @p sig with @p value, delivered to the AO. */
static void news(enum_t sig, int32_t value)
{
    mesh_sys_news_t const n = { .super = { .sig = (QSignal)sig }, .value = value };
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &n.super));
}

static kode_value_t integer(int32_t i)
{
    kode_value_t v = { 0 };
    v.i = i;
    return v;
}

static kode_value_t word(uint8_t e)
{
    kode_value_t v = { 0 };
    v.e = e;
    return v;
}

static kode_value_t text(const char *s)
{
    kode_value_t v = { 0 };
    v.s = s;
    return v;
}

/**
 * @brief `function name=@p name` with @p v, a COMMAND or an IMMEDIATE: the row makes the
 *        mesh_sys.h call @p call itself, at once, and posts nothing to the AO.
 */
static void direct(const char *name, const kode_value_t *v, mesh_call_t call)
{
    unsigned const before = g_mesh.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_run(name, v), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_mesh.calls, name);
    TEST_ASSERT_EQUAL_MESSAGE(call, g_mesh.last.call, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_mesh.requests, name);
    TEST_ASSERT_NULL_MESSAGE(fake_kode_take(), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), name);
}

/**
 * @brief `function name=@p name` with @p v, a REQUEST: the row makes no call, answers `>ok`
 *        and posts it; the AO's step asks the SDK with itself as the asker.
 */
static void asks(const char *name, const kode_value_t *v, mesh_call_t call)
{
    unsigned const before = g_mesh.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_run(name, v), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, g_mesh.calls, name);   /* the row asked nothing */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_kode_blocks_out(), name);
    serve();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_mesh.calls, name);
    TEST_ASSERT_EQUAL_MESSAGE(call, g_mesh.last.call, name);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_ConsoleActions, g_mesh.last.me, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), name);
}

/** @brief `function name=@p name` refused before anything is posted or called. */
static void refused(const char *name, const kode_value_t *v)
{
    unsigned const calls = g_mesh.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(name, v), name);
    TEST_ASSERT_NULL_MESSAGE(fake_kode_take(), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_mesh.calls, name);
}

/** @brief `function name=mesh_tx_arm arg=@p on`. */
static void arm(bool on)
{
    kode_value_t const v = integer(on ? 1 : 0);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_tx_arm", &v));
}

/** @brief Read variable @p name: one word of 1 to 32 characters, with no space in it. */
static void reads(const char *name, const char *want)
{
    char out[KODE_VALUE_STR_MAX + 1U];
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_read(name, out), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, name);
    TEST_ASSERT_NULL_MESSAGE(strpbrk(out, " \t\r\n"), name);
}

/** @brief Every variable gives a word the console would send, whatever the state. */
static void all_variables_read(void)
{
    char out[KODE_VALUE_STR_MAX + 1U];
    for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_read(VARIABLE_NAME[k], out), VARIABLE_NAME[k]);
    }
}

/** @brief What mesh_last says for @p kind (the name) and @p status. */
static void last_is(const char *kind, kode_status_t status)
{
    char want[KODE_VALUE_STR_MAX + 1U];
    (void)snprintf(want, sizeof(want), "%s:%d", kind, (int)status);
    reads("mesh_last", want);
}

/* ---- Registration -------------------------------------------------------- */

TEST_CASE("ConsoleActions_start() starts the SDK, listens to its news, then gives the console twelve functions and twenty-three variables, once", "[console]")
{
    console_reset();
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.begins);
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.subscribes);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_mesh.subscriber);
    TEST_ASSERT_EQUAL_UINT(0U, s_functions_at_initial);   /* the SDK first, then the rows */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);

    /* Its own task and queue, at the app's level. */
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.starts);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_kode.ao);
    TEST_ASSERT_EQUAL_STRING("console_actions", g_kode.task_name);
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_LOW, g_kode.prio);
    TEST_ASSERT_EQUAL_UINT(KQP_AO_QUEUE_LEN, g_kode.queue_len);
    TEST_ASSERT_EQUAL_UINT(KQP_AO_STACK_BYTES, g_kode.stack_bytes);

    /* The names the header lists are the names on the console, and they read. */
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(fake_console_function(FUNCTION_NAME[k]), FUNCTION_NAME[k]);
    }
    all_variables_read();

    /* Transmitting starts disarmed. */
    reads("mesh_tx_armed", "0");

    g_kode.initial = NULL;
    ConsoleActions_start();   /* the names are taken: both tables refused whole */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    TEST_ASSERT_TRUE(fake_kode_logged("the console refused the functions"));
    TEST_ASSERT_TRUE(fake_kode_logged("the console refused the variables"));
    TEST_ASSERT_EQUAL_UINT(2U, g_kode.errors);
}

TEST_CASE("an SDK that does not start, or does not take the subscription, is an error on the log and the rows still stand", "[console]")
{
    fakes_reset();
    g_mesh.begin_answer = KODE_NO_RESOURCES;
    console_up();
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.errors);
    TEST_ASSERT_TRUE(fake_kode_logged("the SDK did not start (status"));
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.subscribes);   /* it asks anyway: the SDK says what it says */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    all_variables_read();
}

static kode_status_t app_get(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return KODE_OK;
}

static kode_status_t app_run(const kode_value_t *arg)
{
    (void)arg;
    return KODE_OK;
}

TEST_CASE("an app's row named like one of the example's is refused, whichever its kind", "[console]")
{
    console_reset();
    static const kode_variable_t SAME_VARIABLE[] = {
        { .name = "mesh_present", .get = app_get, .desc = "the app's" },
    };
    static const kode_function_t SAME_FUNCTION[] = {
        { .name = "mesh_stop", .run = app_run, .desc = "the app's" },
    };
    static const kode_function_t FUNCTION_AS_VARIABLE[] = {
        { .name = "mesh_last", .run = app_run, .desc = "the app's" },
    };
    static const kode_variable_t VARIABLE_AS_FUNCTION[] = {
        { .name = "mesh_radio_send", .get = app_get, .desc = "the app's" },
    };
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(SAME_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(SAME_FUNCTION));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(FUNCTION_AS_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(VARIABLE_AS_FUNCTION));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());

    /* One bad row refuses the table whole: the good row before it is not kept,
     * so the name is still free to be added alone. */
    static const kode_function_t GOOD_THEN_TAKEN[] = {
        { .name = "app_function", .run = app_run, .desc = "the app's" },
        { .name = "mesh_standby", .run = app_run, .desc = "the app's" },
    };
    static const kode_function_t GOOD_ALONE[] = {
        { .name = "app_function", .run = app_run, .desc = "the app's" },
    };
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(GOOD_THEN_TAKEN));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL(KODE_OK, KODE_FUNCTIONS_ADD(GOOD_ALONE));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT + 1U, fake_console_functions());
}

TEST_CASE("the console's room is left to the app: 12 of 64 functions, 23 of 32 variables", "[console]")
{
    console_reset();
    /* Nine more variables fit, the tenth does not: the example may not take more. */
    static const char *const SPARE[10] = { "app_v0", "app_v1", "app_v2", "app_v3", "app_v4",
                                           "app_v5", "app_v6", "app_v7", "app_v8", "app_v9" };
    static kode_variable_t table[10];
    for (size_t k = 0U; k < 10U; k++) {
        table[k] = (kode_variable_t){ .name = SPARE[k], .get = app_get, .desc = "the app's" };
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kode_variables_add(table, 10U));
    TEST_ASSERT_EQUAL(KODE_OK, kode_variables_add(table, 9U));
    TEST_ASSERT_EQUAL_UINT(KODE_VARIABLES_MAX, fake_console_variables());
}

/** @brief Row @p name takes an int from @p lo to @p hi, as function_list will say. */
static void range_is(const char *name, int32_t lo, int32_t hi)
{
    const kode_function_t *const r = fake_console_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_INT, r->arg, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(lo, r->min, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(hi, r->max, name);
}

/** @brief Row @p name takes one of the @p n words listed; one past them, or none, is refused. */
static void words_are(const char *name, uint8_t n, const char *const *list)
{
    const kode_function_t *const r = fake_console_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_ENUM, r->arg, name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(n, r->n_enums, name);
    for (uint8_t k = 0U; k < n; k++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(list[k], r->enums[k], name);
    }
    kode_value_t v = word(n);
    refused(name, &v);
    refused(name, NULL);   /* the line carried no arg=: the row refuses it */
}

TEST_CASE("the rows ask what the header says: types, ranges and words", "[console]")
{
    console_reset();

    range_is("mesh_tx_arm", 0, 1);
    range_is("mesh_sf", 7, 12);
    range_is("mesh_power_x10", -100, 200);
    range_is("mesh_listen", 1, 1000);

    static const char *const BAND_WORD[]   = { "sub_ghz", "2_4_ghz" };
    static const char *const ONOFF_WORD[]  = { "off", "on" };
    static const char *const SIGNAL_WORD[] = { "carrier", "preamble" };
    static const char *const GNSS_WORD[]   = { "on", "off", "reset" };
    words_are("mesh_begin", 2U, BAND_WORD);
    words_are("mesh_rx", 2U, ONOFF_WORD);
    words_are("mesh_radio_test", 2U, SIGNAL_WORD);
    words_are("mesh_gnss", 3U, GNSS_WORD);

    /* The rest take no argument, or a string. */
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("mesh_radio_rssi")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("mesh_standby")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("mesh_stop")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_STRING, fake_console_function("mesh_radio_send")->arg);

    /* No row waits for an answer: each one is `>ok` once accepted. */
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, fake_console_function(FUNCTION_NAME[k])->answer_ms,
                                         FUNCTION_NAME[k]);
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
}

/* ---- Commands and immediates: the row makes the call -------------------- */

TEST_CASE("the rows that are commands or immediates make their mesh_sys.h call at once, and post nothing", "[console]")
{
    console_reset();

    kode_value_t on  = word(1U);
    kode_value_t off = word(0U);
    direct("mesh_rx", &on, MESH_CALL_RECEIVE);
    TEST_ASSERT_TRUE(g_mesh.last.on);
    direct("mesh_rx", &off, MESH_CALL_RECEIVE);
    TEST_ASSERT_FALSE(g_mesh.last.on);

    kode_value_t ms = integer(250);
    direct("mesh_listen", &ms, MESH_CALL_LISTEN);
    TEST_ASSERT_EQUAL_UINT32(250U, g_mesh.last.ms);
    ms = integer(1);
    direct("mesh_listen", &ms, MESH_CALL_LISTEN);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mesh.last.ms);
    ms = integer(1000);
    direct("mesh_listen", &ms, MESH_CALL_LISTEN);
    TEST_ASSERT_EQUAL_UINT32(1000U, g_mesh.last.ms);

    direct("mesh_standby", NULL, MESH_CALL_STANDBY);
    direct("mesh_stop", NULL, MESH_CALL_STOP);
    direct("mesh_radio_rssi", NULL, MESH_CALL_RSSI);

    /* A row answers with what the call returned: the owner's queue within its margin is `>err`. */
    g_mesh.answer = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_standby", NULL));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_rx", &on));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_listen", &ms));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_stop", NULL));
    g_mesh.answer = KODE_DENIED;   /* no module */
    TEST_ASSERT_EQUAL(KODE_DENIED, fake_console_run("mesh_standby", NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, fake_console_run("mesh_rx", &off));

    /* Nothing above reached the AO, or any request. */
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.requests);
    TEST_ASSERT_NULL(fake_kode_take());
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("the rows ask only what mesh_sys.h takes: the ends pass, past them is refused", "[console]")
{
    console_reset();

    /* The console checks the range before the row sees it; the row passes it on whole. */
    kode_value_t v = integer(0);
    refused("mesh_listen", &v);
    v = integer(1001);
    refused("mesh_listen", &v);
    v = integer(-1);
    refused("mesh_listen", &v);
    v = integer(2);
    refused("mesh_tx_arm", &v);
    v = integer(6);
    refused("mesh_sf", &v);
    v = integer(13);
    refused("mesh_sf", &v);
    v = integer(-101);
    refused("mesh_power_x10", &v);
    v = integer(201);
    refused("mesh_power_x10", &v);

    /* mesh_sf and mesh_power_x10 take the ends, and ask nothing: they are read at the next begin. */
    v = integer(7);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_sf", &v));
    v = integer(12);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_sf", &v));
    v = integer(-100);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_power_x10", &v));
    v = integer(200);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_power_x10", &v));
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
    TEST_ASSERT_NULL(fake_kode_take());

    /* A row handed no argument at all, when it takes one, is refused by the row itself. */
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        const kode_function_t *const r = fake_console_function(FUNCTION_NAME[k]);
        if (KODE_TYPE_NONE != r->arg) {
            TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, r->run(NULL), FUNCTION_NAME[k]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
    TEST_ASSERT_NULL(fake_kode_take());
}

TEST_CASE("the rssi reads in dBm, rounded on the log, and a reading that is not there is a warning with the status", "[console]")
{
    console_reset();
    g_mesh.rssi_dbm = -97.6f;
    direct("mesh_radio_rssi", NULL, MESH_CALL_RSSI);
    TEST_ASSERT_EQUAL_STRING("mesh_radio_rssi: -98 dBm", fake_kode_last_line());
    g_mesh.rssi_dbm = 0.0f;
    direct("mesh_radio_rssi", NULL, MESH_CALL_RSSI);
    TEST_ASSERT_EQUAL_STRING("mesh_radio_rssi: 0 dBm", fake_kode_last_line());

    unsigned const warnings = g_kode.warnings;
    g_mesh.answer = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_radio_rssi", NULL));
    TEST_ASSERT_EQUAL_UINT(warnings + 1U, g_kode.warnings);
    TEST_ASSERT_TRUE(fake_kode_logged("no reading (status"));
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

/* ---- Requests: the AO asks ---------------------------------------------- */

TEST_CASE("each request row posts to the AO, which asks the SDK with itself as the asker", "[console]")
{
    console_reset();

    kode_value_t band = word(0U);
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_EQUAL(MESH_BAND_SUB_GHZ, g_mesh.last.band);
    TEST_ASSERT_EQUAL_UINT8(9U, g_mesh.last.spreading_factor);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_DBM_X10, g_mesh.last.power_dbm_x10);
    band = word(1U);
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_EQUAL(MESH_BAND_2_4_GHZ, g_mesh.last.band);

    kode_value_t gnss = word(0U);
    asks("mesh_gnss", &gnss, MESH_CALL_GNSS_BEGIN);
    gnss = word(1U);
    asks("mesh_gnss", &gnss, MESH_CALL_GNSS_END);
    gnss = word(2U);
    asks("mesh_gnss", &gnss, MESH_CALL_GNSS_RESET);

    arm(true);
    kode_value_t t = text("hello");
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    kode_value_t sig = word(0U);
    asks("mesh_radio_test", &sig, MESH_CALL_TEST);

    TEST_ASSERT_EQUAL_UINT(7U, g_mesh.requests);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("mesh_sf and mesh_power_x10 are what the next mesh_begin asks, as they were when it was typed", "[console]")
{
    console_reset();
    kode_value_t sf    = integer(12);
    kode_value_t power = integer(-30);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_sf", &sf));
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_power_x10", &power));
    reads("mesh_last", "none:0");

    kode_value_t band = word(1U);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_begin", &band));
    /* Typed after the begin was posted: not part of it. */
    sf = integer(7);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_sf", &sf));
    serve();
    TEST_ASSERT_EQUAL(MESH_CALL_BEGIN, g_mesh.last.call);
    TEST_ASSERT_EQUAL(MESH_BAND_2_4_GHZ, g_mesh.last.band);
    TEST_ASSERT_EQUAL_UINT8(12U, g_mesh.last.spreading_factor);
    TEST_ASSERT_EQUAL_INT32(-30, g_mesh.last.power_dbm_x10);

    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_EQUAL_UINT8(7U, g_mesh.last.spreading_factor);
}

/* ---- Transmitting ------------------------------------------------------- */

TEST_CASE("mesh_radio_send and mesh_radio_test refuse until mesh_tx_arm is given 1, and mesh_sys.h is not called", "[console][transmit]")
{
    console_reset();
    kode_value_t t   = text("hello");
    kode_value_t sig = word(1U);

    for (unsigned round = 0U; round < 2U; round++) {
        unsigned const warnings = g_kode.warnings;
        refused("mesh_radio_send", &t);
        TEST_ASSERT_TRUE(fake_kode_logged("mesh_radio_send refused: mesh_tx_arm arg=1 first"));
        refused("mesh_radio_test", &sig);
        TEST_ASSERT_TRUE(fake_kode_logged("mesh_radio_test refused: mesh_tx_arm arg=1 first"));
        TEST_ASSERT_EQUAL_UINT(warnings + 2U, g_kode.warnings);
        TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);
        /* Armed and disarmed again: refused as at the start. */
        arm(true);
        reads("mesh_tx_armed", "1");
        arm(false);
        reads("mesh_tx_armed", "0");
    }

    arm(true);
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    asks("mesh_radio_test", &sig, MESH_CALL_TEST);
    TEST_ASSERT_EQUAL_UINT(2U, g_mesh.transmits);
}

TEST_CASE("mesh_tx_arm is the SDK's word on the antennas: 1 confirms them, 0 takes it back", "[console][transmit]")
{
    console_reset();
    unsigned const before = g_mesh.antennas;

    arm(true);
    TEST_ASSERT_EQUAL_UINT(before + 1U, g_mesh.antennas);
    TEST_ASSERT_TRUE(g_mesh.antennas_fitted);
    arm(false);
    TEST_ASSERT_EQUAL_UINT(before + 2U, g_mesh.antennas);
    TEST_ASSERT_FALSE(g_mesh.antennas_fitted);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);
}

TEST_CASE("mesh_radio_send puts the text on air as it is, up to 48 characters", "[console][transmit]")
{
    console_reset();
    arm(true);

    kode_value_t t = text("hello");
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    TEST_ASSERT_EQUAL_UINT(5U, g_mesh.last.length);
    TEST_ASSERT_EQUAL_STRING("hello", (const char *)g_mesh.last.data);

    t = text("a");
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.last.length);

    char longest[CONSOLE_TEXT_MAX + 2U];
    (void)memset(longest, 'x', CONSOLE_TEXT_MAX);
    longest[CONSOLE_TEXT_MAX] = '\0';
    t = text(longest);
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    TEST_ASSERT_EQUAL_UINT(CONSOLE_TEXT_MAX, g_mesh.last.length);
    TEST_ASSERT_EQUAL_STRING(longest, (const char *)g_mesh.last.data);

    /* Spaces and punctuation go as they are: the console hands the string whole. */
    t = text("hi there, 1 2 3!");
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    TEST_ASSERT_EQUAL_STRING("hi there, 1 2 3!", (const char *)g_mesh.last.data);

    /* One more, none, or no string at all: refused with nothing posted. */
    unsigned const transmits = g_mesh.transmits;
    longest[CONSOLE_TEXT_MAX] = 'x';
    longest[CONSOLE_TEXT_MAX + 1U] = '\0';
    t = text(longest);
    refused("mesh_radio_send", &t);
    t = text("");
    refused("mesh_radio_send", &t);
    t = text(NULL);
    refused("mesh_radio_send", &t);
    refused("mesh_radio_send", NULL);
    TEST_ASSERT_EQUAL_UINT(transmits, g_mesh.transmits);
}

TEST_CASE("mesh_radio_test sends a carrier or a preamble for 300 ms, never longer", "[console][transmit]")
{
    console_reset();
    arm(true);
    kode_value_t sig = word(0U);
    asks("mesh_radio_test", &sig, MESH_CALL_TEST);
    TEST_ASSERT_EQUAL(MESH_TEST_SIGNAL_CARRIER, g_mesh.last.signal);
    TEST_ASSERT_EQUAL_UINT32(300U, g_mesh.last.ms);
    sig = word(1U);
    asks("mesh_radio_test", &sig, MESH_CALL_TEST);
    TEST_ASSERT_EQUAL(MESH_TEST_SIGNAL_PREAMBLE, g_mesh.last.signal);
    TEST_ASSERT_EQUAL_UINT32(300U, g_mesh.last.ms);
    TEST_ASSERT_TRUE(CONSOLE_TEST_MS <= MESH_BSP_TEST_MAX_MS);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.bad_args);
}

TEST_CASE("mesh_stop is a command: it calls mesh_sys_stop() at once, disarms, and says what the SDK said", "[console][transmit]")
{
    console_reset();
    arm(true);
    reads("mesh_tx_armed", "1");
    unsigned const disarms = l_disarms;

    direct("mesh_stop", NULL, MESH_CALL_STOP);
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.stops);
    reads("mesh_tx_armed", "0");
    TEST_ASSERT_EQUAL_UINT(disarms + 1U, l_disarms);
    TEST_ASSERT_FALSE(g_mesh.antennas_fitted);   /* the word on the antennas taken back too */

    /* The SDK's queue within its margin: the row says so, and still disarms. */
    arm(true);
    g_mesh.answer = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_stop", NULL));
    reads("mesh_tx_armed", "0");
    TEST_ASSERT_EQUAL_UINT(2U, g_mesh.stops);

    /* A stop with nothing armed is a stop, and counts. */
    unsigned const before = l_disarms;
    g_mesh.answer = KODE_OK;
    direct("mesh_stop", NULL, MESH_CALL_STOP);
    TEST_ASSERT_EQUAL_UINT(before + 1U, l_disarms);
}

TEST_CASE("a stop that comes while a transmit waits in the AO's queue wins", "[console][transmit]")
{
    console_reset();
    arm(true);
    kode_value_t t = text("hello");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_send", &t));
    kode_value_t sig = word(0U);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_test", &sig));
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_stop", NULL));
    serve();

    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.requests);
    TEST_ASSERT_TRUE(fake_kode_logged("transmitting was disarmed; this request is dropped"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
    last_is("none", 0);   /* nothing was asked, so nothing came of it */
}

TEST_CASE("a disarm that comes while a transmit waits wins, armed again or not", "[console][transmit]")
{
    console_reset();
    kode_value_t t = text("hello");
    kode_value_t sig = word(1U);

    arm(true);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_send", &t));
    arm(false);
    serve();
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);

    /* Disarmed and armed again before the AO's step: still dropped, it was typed under the old arming. */
    arm(true);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_test", &sig));
    arm(false);
    arm(true);
    serve();
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);

    /* A stop and a new arm in between: the same. */
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_send", &t));
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_stop", NULL));
    arm(true);
    serve();
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);

    /* The one typed after the last arm goes. */
    asks("mesh_radio_send", &t, MESH_CALL_SEND);
    TEST_ASSERT_EQUAL_UINT(1U, g_mesh.transmits);
}

TEST_CASE("the module leaving J3 disarms, before or after the request reaches the AO", "[console][transmit]")
{
    console_reset();
    kode_value_t t = text("hello");

    arm(true);
    news(MESH_MODULE_LOST_SIG, 0);
    reads("mesh_tx_armed", "0");
    TEST_ASSERT_FALSE(g_mesh.antennas_fitted);   /* the word on the antennas taken back too */
    refused("mesh_radio_send", &t);

    /* After: the request is already posted when the module goes. */
    arm(true);
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_radio_send", &t));
    news(MESH_MODULE_LOST_SIG, 0);
    serve();
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.transmits);
    TEST_ASSERT_TRUE(fake_kode_logged("transmitting was disarmed"));

    /* Back on J3, still disarmed until it is armed again. */
    news(MESH_MODULE_FOUND_SIG, 1);
    reads("mesh_tx_armed", "0");
    refused("mesh_radio_send", &t);
}

TEST_CASE("a request the SDK refuses never leaves a transmit made, or a request open", "[console][transmit]")
{
    console_reset();
    arm(true);
    kode_value_t t   = text("hello");
    kode_value_t sig = word(0U);

    static const kode_status_t REFUSAL[] = { KODE_DENIED, KODE_NO_RESOURCES };
    for (size_t k = 0U; k < Q_DIM(REFUSAL); k++) {
        g_mesh.answer = REFUSAL[k];
        unsigned const warnings = g_kode.warnings;
        asks("mesh_radio_send", &t, MESH_CALL_SEND);
        TEST_ASSERT_EQUAL_UINT(warnings + 1U, g_kode.warnings);
        last_is("send", REFUSAL[k]);
        asks("mesh_radio_test", &sig, MESH_CALL_TEST);
        last_is("test", REFUSAL[k]);
    }
    g_mesh.answer = KODE_OK;
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
    reads("mesh_tx_armed", "1");   /* a refusal does not disarm */
}

/* ---- Lost room ----------------------------------------------------------- */

TEST_CASE("a request with no block, or no room in the queue beyond the margin, is refused to the row and nothing stays", "[console]")
{
    console_reset();
    kode_value_t band = word(0U);
    kode_value_t gnss = word(0U);

    /* The pool at its last block: the block that is kept back is not given. */
    g_kode.blocks_held = FAKE_KODE_BLOCKS - 1U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_begin", &band));
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_gnss", &gnss));
    g_kode.blocks_held = 0U;
    TEST_ASSERT_NULL(fake_kode_take());
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());

    /* The queue at its margin: the rows never take the slots the SDK's answers need. */
    g_kode.queue_held = KQP_AO_QUEUE_LEN - KODE_SYS_CMD_MARGIN;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("mesh_begin", &band));
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());   /* the block went back to its pool */
    g_kode.queue_held = KQP_AO_QUEUE_LEN - KODE_SYS_CMD_MARGIN - 1U;
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_begin", &band));   /* one slot over the margin */
    g_kode.queue_held = 0U;
    serve();
    TEST_ASSERT_EQUAL(MESH_CALL_BEGIN, g_mesh.last.call);

    /* Nothing is left busy: the next one goes. */
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("a request refused for the module or a door still shut is a warning with the status, and mesh_last keeps it", "[console]")
{
    console_reset();
    kode_value_t band = word(0U);

    g_mesh.answer = KODE_DENIED;
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
    TEST_ASSERT_TRUE(fake_kode_logged("mesh_begin: refused (status"));
    TEST_ASSERT_TRUE(fake_kode_logged("no module, or the same kind of request is still running"));
    last_is("begin", KODE_DENIED);

    g_mesh.answer = KODE_NO_RESOURCES;
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_TRUE(fake_kode_logged("no room just now; ask again"));
    last_is("begin", KODE_NO_RESOURCES);

    g_mesh.answer = KODE_IO_ERROR;
    asks("mesh_begin", &band, MESH_CALL_BEGIN);
    TEST_ASSERT_TRUE(fake_kode_logged("see the status"));
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

/* ---- The answers --------------------------------------------------------- */

TEST_CASE("each answer is on the log, and mesh_last names the request and its status", "[console]")
{
    console_reset();
    last_is("none", 0);

    static const struct {
        enum_t      sig;
        const char *kind;
        const char *line;
    } ANSWER[] = {
        { MESH_RADIO_BEGIN_RSP_SIG, "begin",      "mesh_begin: done" },
        { MESH_RADIO_TEST_RSP_SIG,  "test",       "mesh_radio_test: done" },
        { MESH_GNSS_BEGIN_RSP_SIG,  "gnss_on",    "mesh_gnss on: done" },
        { MESH_GNSS_END_RSP_SIG,    "gnss_off",   "mesh_gnss off: done" },
        { MESH_GNSS_RESET_RSP_SIG,  "gnss_reset", "mesh_gnss reset: done" },
    };
    for (size_t k = 0U; k < Q_DIM(ANSWER); k++) {
        answer(ANSWER[k].sig, KODE_OK, MESH_WHY_NONE);
        TEST_ASSERT_EQUAL_STRING(ANSWER[k].line, fake_kode_last_line());
        last_is(ANSWER[k].kind, KODE_OK);
        answer(ANSWER[k].sig, KODE_TIMEOUT, MESH_WHY_BUSY_SENDING);
        TEST_ASSERT_TRUE(fake_kode_logged(mesh_sys_why_text(MESH_WHY_BUSY_SENDING)));
        last_is(ANSWER[k].kind, KODE_TIMEOUT);
    }
    TEST_ASSERT_EQUAL_UINT(Q_DIM(ANSWER), g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);   /* an answer asks nothing */
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("a send's answer says how long it was on air, or how long the duty cycle asks to wait", "[console]")
{
    console_reset();

    answer_send(KODE_OK, MESH_WHY_NONE, 123U, 0U);
    TEST_ASSERT_EQUAL_STRING("the packet left whole: 123 ms on air", fake_kode_last_line());
    TEST_ASSERT_TRUE(fake_kode_logged("mesh_radio_send: done"));
    last_is("send", KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);

    answer_send(KODE_NO_RESOURCES, MESH_WHY_DUTY, 0U, 36000U);
    TEST_ASSERT_EQUAL_STRING("the duty cycle has no room; 36000 ms to go", fake_kode_last_line());
    last_is("send", KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);

    /* Any other reason is the one line the status gave, and no wait. */
    unsigned const infos = g_kode.infos;
    answer_send(KODE_DENIED, MESH_WHY_INTERRUPTED, 0U, 0U);
    TEST_ASSERT_TRUE(fake_kode_logged("mesh_radio_send: refused (status"));
    TEST_ASSERT_TRUE(fake_kode_logged(mesh_sys_why_text(MESH_WHY_INTERRUPTED)));
    TEST_ASSERT_EQUAL_UINT(infos, g_kode.infos);
    last_is("send", KODE_DENIED);
}

TEST_CASE("an answer the example never asked for goes up to the top state, and nothing is said", "[console]")
{
    console_reset();
    static const enum_t OTHER[] = {
        MESH_RADIO_CONFIGURE_RSP_SIG, MESH_RADIO_CALIBRATE_RSP_SIG, MESH_RADIO_RETUNE_RSP_SIG,
        MESH_RADIO_END_RSP_SIG, MESH_RADIO_SEND_PREAMBLE_RSP_SIG, MESH_RADIO_CAD_RSP_SIG,
        MESH_I2C_LEND_RSP_SIG, MESH_I2C_RETURN_RSP_SIG, (enum_t)(CA_REQUEST_SIG + 1),
    };
    unsigned const lines = g_kode.infos + g_kode.warnings;
    for (size_t k = 0U; k < Q_DIM(OTHER); k++) {
        QEvt const other = { .sig = (QSignal)OTHER[k] };
        TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &other));
    }
    TEST_ASSERT_EQUAL_UINT(lines, g_kode.infos + g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
    last_is("none", 0);
}

/* ---- What the SDK tells -------------------------------------------------- */

TEST_CASE("each piece of news the SDK tells is on the log, in the AO's step", "[console]")
{
    console_reset();
    unsigned const lines = g_kode.infos + g_kode.warnings;

    news(MESH_MODULE_FOUND_SIG, 1);
    TEST_ASSERT_EQUAL_STRING("the Mesh is on J3", fake_kode_last_line());
    news(MESH_LISTEN_ENDED_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("the timed listen ended", fake_kode_last_line());
    news(MESH_WRONG_MODULE_SIG, MESH_MODULE_OTHER);
    TEST_ASSERT_EQUAL_STRING("J3 holds a module that is not a Mesh", fake_kode_last_line());
    news(MESH_TEST_ENDED_SIG, MESH_TEST_END_CHIP);
    TEST_ASSERT_EQUAL_STRING("the test signal ended by itself (the chip's timer)", fake_kode_last_line());
    news(MESH_TEST_ENDED_SIG, MESH_TEST_END_OWNER);
    TEST_ASSERT_EQUAL_STRING("the test signal ended by itself (the SDK's deadline)", fake_kode_last_line());
    news(MESH_RADIO_FAILED_SIG, (int32_t)(0x0003U | MESH_RADIO_ERROR_NO_STANDBY));
    TEST_ASSERT_EQUAL_STRING("radio error: chip word 0x0003, reason 1; mesh_begin sets it up again",
                             fake_kode_last_line());
    news(MESH_RADIO_FAILED_SIG, (int32_t)(0xFFFFU | MESH_RADIO_ERROR_AMPLIFIER_FAULT));
    TEST_ASSERT_EQUAL_STRING("radio error: chip word 0xFFFF, reason 3; mesh_begin sets it up again",
                             fake_kode_last_line());
    news(MESH_SETTING_LOST_SIG, 5);
    TEST_ASSERT_EQUAL_STRING("the setting was lost (0x5: 1 radio, 2 receiver, 4 GNSS); mesh_begin sets it up again",
                             fake_kode_last_line());
    news(MESH_SETTING_RESTORED_SIG, 3);
    TEST_ASSERT_EQUAL_STRING("the SDK put the setting back (0x3)", fake_kode_last_line());
    news(MESH_MODULE_LOST_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("the Mesh left J3; transmitting is disarmed", fake_kode_last_line());
    TEST_ASSERT_TRUE(g_kode.infos + g_kode.warnings > lines);

    /* The quiet news, and an unknown one, say nothing and cost nothing. */
    static const enum_t QUIET[] = { MESH_PREAMBLE_DETECTED_SIG, MESH_HEADER_RECEIVED_SIG, MESH_HEADER_FAILED_SIG };
    unsigned const before = g_kode.infos + g_kode.warnings;
    for (size_t k = 0U; k < Q_DIM(QUIET); k++) {
        news(QUIET[k], 1);
    }
    TEST_ASSERT_EQUAL_UINT(before, g_kode.infos + g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());

    /* None of it asked the SDK for anything. */
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.reads);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("a packet is read in the AO's step, every one waiting, and described", "[console]")
{
    console_reset();

    g_mesh.packets_ready      = 3U;
    g_mesh.packet.length      = 12U;
    g_mesh.packet.rssi_dbm    = -80.4f;
    g_mesh.packet.snr_db      = 5.5f;
    g_mesh.packet.crc_valid   = true;
    unsigned const infos = g_kode.infos;
    news(MESH_PACKET_RECEIVED_SIG, 12);
    TEST_ASSERT_EQUAL_UINT(infos + 3U, g_kode.infos);
    TEST_ASSERT_EQUAL_UINT(4U, g_mesh.reads);   /* three, and the one that found none */
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);   /* mesh_sys_radio_read() is an immediate, not a call of the table */
    TEST_ASSERT_EQUAL_STRING("packet: 12 bytes, RSSI -80 dBm, SNR 5.5 dB, CRC good", fake_kode_last_line());

    g_mesh.packets_ready    = 1U;
    g_mesh.packet.length    = 255U;
    g_mesh.packet.rssi_dbm  = -120.0f;
    g_mesh.packet.snr_db    = -12.5f;
    g_mesh.packet.crc_valid = false;
    news(MESH_PACKET_RECEIVED_SIG, 255);
    TEST_ASSERT_EQUAL_STRING("packet: 255 bytes, RSSI -120 dBm, SNR -12.5 dB, CRC BAD or none",
                             fake_kode_last_line());

    /* News with nothing waiting reads once and says nothing. */
    g_mesh.packets_ready = 0U;
    unsigned const lines = g_kode.infos + g_kode.warnings;
    unsigned const reads_before = g_mesh.reads;
    news(MESH_PACKET_RECEIVED_SIG, 0);
    TEST_ASSERT_EQUAL_UINT(reads_before + 1U, g_mesh.reads);
    TEST_ASSERT_EQUAL_UINT(lines, g_kode.infos + g_kode.warnings);
}

TEST_CASE("mesh_last follows the last answer, one word, and a word the state never made reads within the table", "[console]")
{
    console_reset();
    last_is("none", 0);
    answer(MESH_RADIO_BEGIN_RSP_SIG, KODE_OK, MESH_WHY_NONE);
    last_is("begin", KODE_OK);
    answer_send(KODE_OK, MESH_WHY_NONE, 5U, 0U);
    last_is("send", KODE_OK);
    answer(MESH_GNSS_RESET_RSP_SIG, KODE_IO_ERROR, MESH_WHY_NONE);
    last_is("gnss_reset", KODE_IO_ERROR);

    /* A command or an immediate says nothing of itself: the next answer does. */
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_stop", NULL));
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("mesh_standby", NULL));
    last_is("gnss_reset", KODE_IO_ERROR);

    l_last = (UINT32_C(9) << 16) | 7U;
    reads("mesh_last", "send:7");   /* 9 % 7 = 2: wrapped inside the table, never past it */
    l_last = (UINT32_C(0xFFFF) << 16) | 0xFFFFU;
    all_variables_read();
    l_last = 0xFFFFFFFFU;
    all_variables_read();
    l_last = (UINT32_C(7) << 16) | 3U;
    reads("mesh_last", "none:3");
}

/* ---- Variables ----------------------------------------------------------- */

TEST_CASE("each console variable is one word the console sends, with no module", "[console][variables]")
{
    console_reset();

    /* Nothing found yet: everything off, nothing counted. */
    reads("mesh_present", "off");
    reads("mesh_seen", "none");
    reads("mesh_tx_armed", "0");
    reads("mesh_ready", "off");
    reads("mesh_band_now", "none");
    reads("mesh_sf_now", "0");
    reads("mesh_power_now", "0");
    reads("mesh_sending", "off");
    reads("mesh_receiving", "off");
    reads("mesh_testing", "off");
    reads("mesh_fault", "off");
    reads("mesh_duty_ms", "0");
    reads("mesh_rx_count", "0");
    reads("mesh_rx_dropped", "0");
    reads("mesh_rx_waiting", "0");
    reads("mesh_tx_count", "0");
    reads("mesh_rssi_dbm", "0");
    reads("mesh_gnss_on", "off");
    reads("mesh_gnss_fix", "nofix");
    reads("mesh_gnss_sats", "0");
    reads("mesh_gnss_count", "0");
    reads("mesh_gnss_baud", "0");
    reads("mesh_last", "none:0");
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
}

TEST_CASE("the booleans read on and off, each on its own field", "[console][variables]")
{
    static const struct {
        const char *name;
        size_t      offset;
    } FLAG[] = {
        { "mesh_present",   offsetof(mesh_status_t, present) },
        { "mesh_ready",     offsetof(mesh_status_t, radio_ready) },
        { "mesh_sending",   offsetof(mesh_status_t, sending) },
        { "mesh_receiving", offsetof(mesh_status_t, receiving) },
        { "mesh_testing",   offsetof(mesh_status_t, testing) },
        { "mesh_fault",     offsetof(mesh_status_t, amplifier_fault) },
        { "mesh_gnss_on",   offsetof(mesh_status_t, gnss_on) },
    };
    console_reset();
    for (size_t k = 0U; k < Q_DIM(FLAG); k++) {
        (void)memset(&g_mesh.st, 0, sizeof(g_mesh.st));
        *((bool *)((uint8_t *)&g_mesh.st + FLAG[k].offset)) = true;
        /* Only this one is on. */
        for (size_t j = 0U; j < Q_DIM(FLAG); j++) {
            reads(FLAG[j].name, (j == k) ? "on" : "off");
        }
    }
}

TEST_CASE("the module seen reads none, mesh or other, and none for a value it does not know", "[console][variables]")
{
    console_reset();
    g_mesh.st.seen = MESH_MODULE_NONE;
    reads("mesh_seen", "none");
    g_mesh.st.seen = MESH_MODULE_MESH;
    reads("mesh_seen", "mesh");
    g_mesh.st.seen = MESH_MODULE_OTHER;
    reads("mesh_seen", "other");
    g_mesh.st.seen = (mesh_module_t)3;   /* a module kind from a newer SDK */
    reads("mesh_seen", "none");
    g_mesh.st.seen = (mesh_module_t)255;
    reads("mesh_seen", "none");
    g_mesh.st.seen_level = UINT16_MAX;   /* the fingerprint reading is not a variable */
    reads("mesh_seen", "none");
}

TEST_CASE("the band reads none until one is set, then sub_ghz or 2_4_ghz", "[console][variables]")
{
    console_reset();

    /* Not ready: none, whatever band the status was left at. */
    g_mesh.st.radio_ready = false;
    g_mesh.st.band        = MESH_BAND_2_4_GHZ;
    reads("mesh_band_now", "none");
    g_mesh.st.band = MESH_BAND_SUB_GHZ;
    reads("mesh_band_now", "none");

    g_mesh.st.radio_ready = true;
    reads("mesh_band_now", "sub_ghz");
    g_mesh.st.band = MESH_BAND_2_4_GHZ;
    reads("mesh_band_now", "2_4_ghz");
    reads("mesh_ready", "on");

    /* A band from a newer SDK is not claimed to be the 2.4 GHz one. */
    g_mesh.st.band = MESH_BAND_COUNT;
    reads("mesh_band_now", "sub_ghz");
}

TEST_CASE("the counters read whole at their ends, the signed ones with their sign", "[console][variables]")
{
    console_reset();
    mesh_status_t *const st = &g_mesh.st;

    st->spreading_factor = UINT8_MAX;
    reads("mesh_sf_now", "255");
    st->spreading_factor = 7U;
    reads("mesh_sf_now", "7");

    st->power_dbm_x10 = INT32_MAX;
    reads("mesh_power_now", "2147483647");
    st->power_dbm_x10 = INT32_MIN;
    reads("mesh_power_now", "-2147483648");
    st->power_dbm_x10 = -100;
    reads("mesh_power_now", "-100");
    st->power_dbm_x10 = 85;
    reads("mesh_power_now", "85");

    st->duty_wait_ms     = UINT32_MAX;
    st->packets_received = UINT32_MAX;
    st->packets_dropped  = UINT32_MAX - 1U;
    st->packets_sent     = UINT32_MAX - 2U;
    st->gnss_sentences   = UINT32_MAX - 3U;
    st->gnss_baud        = UINT32_MAX - 4U;
    st->packets_waiting  = UINT8_MAX;
    st->fix.satellites_used = UINT8_MAX;
    reads("mesh_duty_ms", "4294967295");
    reads("mesh_rx_count", "4294967295");
    reads("mesh_rx_dropped", "4294967294");
    reads("mesh_tx_count", "4294967293");
    reads("mesh_gnss_count", "4294967292");
    reads("mesh_gnss_baud", "4294967291");
    reads("mesh_rx_waiting", "255");
    reads("mesh_gnss_sats", "255");

    st->duty_wait_ms     = 0U;
    st->packets_received = 1U;
    st->gnss_baud        = 115200U;
    st->fix.satellites_used = 12U;
    reads("mesh_duty_ms", "0");
    reads("mesh_rx_count", "1");
    reads("mesh_gnss_baud", "115200");
    reads("mesh_gnss_sats", "12");

    /* The radio error word and the other fields of the status are not variables. */
    st->radio_errors = UINT16_MAX;
    st->fix.satellites_in_view = UINT8_MAX;
    all_variables_read();
}

TEST_CASE("the rssi reads as a whole number of dBm, the way a long truncates", "[console][variables]")
{
    console_reset();
    static const struct {
        float       dbm;
        const char *word;
    } CASE[] = {
        { 0.0f,      "0" },
        { -97.0f,    "-97" },
        { -97.9f,    "-97" },
        { 12.9f,     "12" },
        { -0.4f,     "0" },
        { -157.0f,   "-157" },
        { -100000.0f, "-100000" },
        { 100000.0f, "100000" },
    };
    for (size_t k = 0U; k < Q_DIM(CASE); k++) {
        g_mesh.st.last_rssi_dbm = CASE[k].dbm;
        reads("mesh_rssi_dbm", CASE[k].word);
    }
}

TEST_CASE("the GNSS says whether there is a fix, never where", "[console][variables]")
{
    console_reset();
    g_mesh.st.gnss_on = true;
    reads("mesh_gnss_fix", "nofix");
    g_mesh.st.fix.valid        = true;
    g_mesh.st.fix.latitude_deg  = 39.4699;
    g_mesh.st.fix.longitude_deg = -0.3763;
    g_mesh.st.fix.altitude_m    = 15.0f;
    reads("mesh_gnss_fix", "fix");

    /* No variable carries the position, whichever the fix. */
    char out[KODE_VALUE_STR_MAX + 1U];
    for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
        TEST_ASSERT_EQUAL(KODE_OK, fake_console_read(VARIABLE_NAME[k], out));
        TEST_ASSERT_NULL_MESSAGE(strstr(out, "39.46"), VARIABLE_NAME[k]);
        TEST_ASSERT_NULL_MESSAGE(strstr(out, "0.376"), VARIABLE_NAME[k]);
    }

    g_mesh.st.fix.valid = false;   /* a fix that was lost: the position is still there to hide */
    reads("mesh_gnss_fix", "nofix");
}

TEST_CASE("every variable gives a word in every state: absent, either band, a module seen, the ends", "[console][variables]")
{
    console_reset();
    mesh_status_t *const st = &g_mesh.st;
    static const mesh_module_t SEEN[] = { MESH_MODULE_NONE, MESH_MODULE_MESH, MESH_MODULE_OTHER,
                                          (mesh_module_t)7 };
    static const mesh_band_t BAND[] = { MESH_BAND_SUB_GHZ, MESH_BAND_2_4_GHZ, MESH_BAND_COUNT };

    for (size_t s = 0U; s < Q_DIM(SEEN); s++) {
        for (size_t b = 0U; b < Q_DIM(BAND); b++) {
            for (int present = 0; present < 2; present++) {
                for (int ready = 0; ready < 2; ready++) {
                    (void)memset(st, 0, sizeof(*st));
                    st->seen        = SEEN[s];
                    st->band        = BAND[b];
                    st->present     = (present != 0);
                    st->radio_ready = (ready != 0);
                    all_variables_read();
                }
            }
        }
    }

    /* Everything at its largest, and at its smallest. */
    (void)memset(st, 0, sizeof(*st));
    st->present = st->radio_ready = st->sending = st->receiving = st->testing = true;
    st->amplifier_fault = st->gnss_on = st->fix.valid = true;
    st->spreading_factor = UINT8_MAX;
    st->power_dbm_x10       = INT32_MIN;
    st->duty_wait_ms = st->packets_received = st->packets_dropped = UINT32_MAX;
    st->packets_sent = st->gnss_sentences = st->gnss_baud = UINT32_MAX;
    st->packets_waiting = st->fix.satellites_used = UINT8_MAX;
    st->last_rssi_dbm = -100000.0f;
    l_last = 0xFFFFFFFFU;
    all_variables_read();
    st->power_dbm_x10    = INT32_MAX;
    st->last_rssi_dbm = 100000.0f;
    all_variables_read();
    TEST_ASSERT_EQUAL_UINT(0U, g_mesh.calls);
}

TEST_CASE("a getter that is handed the console's buffer fills it within the cap", "[console][variables]")
{
    console_reset();
    g_mesh.st.power_dbm_x10 = INT32_MIN;
    g_mesh.st.present    = true;
    char guard[40];
    for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
        (void)memset(guard, 'Z', sizeof(guard));
        /* A cap that is just the word and its end: nothing is written past it. */
        char small[12];
        (void)memset(small, 'Z', sizeof(small));
        const kode_variable_t *probe = NULL;
        for (size_t t = 0U; t < Q_DIM(VARIABLES); t++) {
            if (strcmp(VARIABLES[t].name, VARIABLE_NAME[k]) == 0) {
                probe = &VARIABLES[t];
            }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(probe, VARIABLE_NAME[k]);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, probe->get(small, 8U), VARIABLE_NAME[k]);
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('Z', small[8], VARIABLE_NAME[k]);   /* past the cap: untouched */
        TEST_ASSERT_TRUE_MESSAGE(strlen(small) <= 7U, VARIABLE_NAME[k]);
    }
}
