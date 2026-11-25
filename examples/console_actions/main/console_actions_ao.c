/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.c
 * @brief The Mesh module on kodeOS's console: twelve functions and twenty-three
 *        variables, and an Active Object that asks for what a row cannot.
 *
 * KODE_FUNCTIONS_ADD() and KODE_VARIABLES_ADD() give the console rows of the
 * app's own (kode_functions.h, kode_variables.h). These are the module's, for
 * the USB console, BLE and kode-cli: an app that wants them copies this file's
 * tables, or the rows it needs. Every name starts with mesh_.
 *
 * >>> THE TWO ROWS THAT TRANSMIT REFUSE UNTIL mesh_tx_arm HAS BEEN GIVEN 1.
 *     AN ANTENNA OR A 50 OHM LOAD ON THE SMA OF THE BAND IN USE (J501 FOR
 *     sub_ghz, J502 FOR 2_4_ghz) BEFORE ARMING. mesh_radio_test GOES INTO A 50 OHM
 *     LOAD OR AN ANALYSER ONLY, NEVER AN ANTENNA: ITS CARRIER IS ON AIR 100 %
 *     OF THE TIME. 868 IS SHARED AND THE DUTY CYCLE IS THE LAW. <<<
 *
 * mesh_radio_test is a lab build's: the SDK puts a test signal on air only when
 * the build sets CONFIG_MESH_LAB (`idf.py menuconfig`, not for a firmware that is distributed).
 * In this example as it is built, the row is answered "a test signal is a lab
 * build's: this build has no CONFIG_MESH_LAB", on the log, and nothing is keyed.
 *
 * Try it, on the console (the USB-C cable). The name goes in quotes; an INT
 * or ENUM argument bare, a STRING in quotes:
 *
 *     function name="mesh_sf" arg=9               spreading factor, for the next begin
 *     function name="mesh_begin" arg=sub_ghz          868 MHz (2_4_ghz: 2.4 GHz)
 *     function name="mesh_rx" arg=on              the receiver on; each packet on the log
 *     function name="mesh_tx_arm" arg=1           ONLY with a load or antenna on
 *     function name="mesh_radio_send" arg="hello"
 *     function name="mesh_stop"                   everything off air, now; disarms
 *     variable name="mesh_rx_count" watch=500     follow the packets heard
 *
 * Functions (one argument each; none blocks - a row either calls a mesh_sys.h
 * COMMAND or IMMEDIATE itself and answers with what it returned, or hands a
 * REQUEST to this AO and answers `>ok` when accepted; what came of a request
 * is on the log and in the variable mesh_last):
 *
 *     mesh_tx_arm      1 allows the two transmitting rows, 0 refuses them; it
 *                      is also the SDK's word on the antennas (command)
 *     mesh_sf          spreading factor 7..12, for the next mesh_begin
 *     mesh_power_x10   power in tenths of a dBm (85 = 8.5), for the next mesh_begin
 *     mesh_begin       the radio on a band: sub_ghz or 2_4_ghz              (request)
 *     mesh_rx          the receiver: on or off                              (command)
 *     mesh_radio_rssi  the level the receiver sees now, on the log          (immediate)
 *     mesh_listen      receive for 1..1000 ms; the chip ends it             (command)
 *     mesh_radio_send  one packet of text, up to 48 characters (ARMED)      (request)
 *     mesh_radio_test  a test signal for 300 ms: carrier or preamble (ARMED; (request)
 *                      refused unless the build sets CONFIG_MESH_LAB)
 *     mesh_standby     everything off air, the band kept                    (command)
 *     mesh_stop        everything off air now, no waiting; disarms          (command)
 *     mesh_gnss        the GNSS receiver: on, off or reset                  (request)
 *
 * Variables (read-only; a variable can never be set from the console, which is
 * why arming is a function and mesh_tx_armed only says whether it was):
 *
 *     mesh_present mesh_seen mesh_tx_armed mesh_ready mesh_band_now
 *     mesh_sf_now mesh_power_now mesh_sending mesh_receiving mesh_testing
 *     mesh_fault mesh_duty_ms mesh_rx_count mesh_rx_dropped mesh_rx_waiting
 *     mesh_tx_count mesh_rssi_dbm mesh_gnss_on mesh_gnss_fix mesh_gnss_sats
 *     mesh_gnss_count mesh_gnss_baud mesh_last
 *
 * Twenty-three variables of the firmware's 32 and twelve functions of its 64,
 * in one table each of the four each kind allows. The position of a GNSS fix
 * is not among them: the console says whether there is a fix, never where
 * the Dot is.
 *
 * ---- Who calls what ------------------------------------------------------
 *
 * A function's `run` is called on AO_Console's task, in the middle of its
 * step, and must not block. mesh_sys.h (the extended layer, whose calls never
 * wait) has three kinds of call, and a row may make the first two itself:
 *
 *  - COMMANDS (mesh_sys_radio_receive(), _listen(), _standby(), mesh_sys_stop()):
 *    posted to the owner; the row answers with what the call returned
 *    (`>ok`, or `>err` when the owner's queue was within its margin).
 *  - IMMEDIATES (mesh_sys_radio_rssi(), mesh_sys_status_read()): a copy or a
 *    number, at once. A getter reads mesh_sys_status_read(), a copy the owner
 *    keeps for exactly this.
 *  - REQUESTS (mesh_sys_radio_begin(), _send(), _test(), mesh_sys_gnss_*()):
 *    these take an Active Object that the answer is posted to, and a row is
 *    not one. So the row posts the request to THIS AO and returns; this AO asks,
 *    and the answer (MESH_RADIO_BEGIN_RSP_SIG and the others) arrives to it
 *    later, to be logged and written in mesh_last. A second request of the
 *    same kind before the first is answered is refused by the SDK's door.
 *
 * mesh_stop is a command, so an emergency stop never waits behind a request:
 * it ends what is on air at once, and the send it cut is answered
 * MESH_WHY_INTERRUPTED.
 *
 * The SDK's news - the module found or lost, a packet landed, the radio went
 * wrong - are signals this AO subscribed to (mesh_sys_subscribe()); it logs
 * them, and reads each packet with mesh_sys_radio_read() in a step of its own.
 *
 * ---- The queue, by mesh_sys.h's rule -------------------------------------
 *
 * This AO's queue (KQP_AO_QUEUE_LEN, 16) is posted to by three: the rows (a
 * request is posted leaving KODE_SYS_CMD_MARGIN, 4, slots free), the SDK's news
 * (posted leaving the same margin) and the SDK's answers (no margin, six
 * kinds at most in flight, one per kind of request). The rows and the news
 * never take the last four slots, so four answers always find room. Only with
 * five or six requests in flight at once and the rest of the queue full of
 * news could an answer be lost: the SDK then counts it (answers_dropped in
 * mesh_sys_status_read()) and says so on the log, never QP's assert, and that
 * answer is missing from the log and from mesh_last.
 */
#include "console_actions_ao.h"

#include <stdio.h>
#include <string.h>

#include "mesh_sys.h"

#define TAG "console_actions"

/** The longest text mesh_radio_send takes from the console. */
#define CONSOLE_TEXT_MAX       48U

/** How long mesh_radio_test keeps its signal on air. Short on purpose. */
#define CONSOLE_TEST_MS        300U

/** @brief What a request asks for. */
typedef enum {
    REQUEST_BEGIN,     /**< mesh_sys_radio_begin(band, sf, power).       */
    REQUEST_SEND,      /**< mesh_sys_radio_send(text). TRANSMITS.         */
    REQUEST_TEST,      /**< mesh_sys_radio_test(mode). TRANSMITS.         */
    REQUEST_GNSS_ON,   /**< mesh_sys_gnss_begin().                        */
    REQUEST_GNSS_OFF,  /**< mesh_sys_gnss_end().                          */
    REQUEST_GNSS_RESET /**< mesh_sys_gnss_reset().                        */
} request_kind_t;

/** @brief This AO's private signals. Posted to itself, never published. They are the app's own
 *         range, from MAX_SIG; the SDK's start at MESH_SIG_FIRST. */
enum ConsoleActionsSignals {
    CA_REQUEST_SIG = MAX_SIG   /**< From a function's `run`. */
};

/** @brief A request, from the console's task to this AO. */
typedef struct {
    QEvt           super;
    request_kind_t kind;
    int32_t        arg;          /**< The band or the mode.                    */
    uint8_t        sf;           /**< The spreading factor, for REQUEST_BEGIN. */
    int32_t        power_dbm_x10; /**< The power, for REQUEST_BEGIN.           */
    char           text[CONSOLE_TEXT_MAX + 1U];   /**< For REQUEST_SEND.       */
    uint32_t       disarms;      /**< l_disarms when its row posted it.        */
} RequestEvt;

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive        super;
    mesh_packet_t  packet;     /**< The packet being read. */
} ConsoleActions;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ConsoleActions l_ao;
QActive * const AO_ConsoleActions = &l_ao.super;

/* Written by the console's task, read by this AO. Each is one aligned word, so
 * a read is whole; the worst a read can show is a setting a moment old. */
static volatile bool     l_tx_armed  = false;
static volatile uint8_t  l_sf        = 9U;
static volatile int32_t  l_power     = MESH_REGION_SUB_GHZ_DBM_X10;
static volatile uint32_t l_last      = 0U;   /* request_kind_t + 1, then the status */
/* One more for every stop and every disarm. A transmit carries the count it
 * was posted at, and this AO sends nothing if it has moved: a stop typed while
 * the request waited in the queue is a stop. */
static volatile uint32_t l_disarms   = 0U;

static QState ConsoleActions_initial(ConsoleActions * const me, void const * const par);
static QState ConsoleActions_serving(ConsoleActions * const me, QEvt const * const e);

/* ---- Functions ---------------------------------------------------------- */

/**
 * @brief Hand a request to the AO, from the console's task.
 * @param kind  What it asks for.
 * @param arg   Its argument.
 * @param text  The text of REQUEST_SEND, else NULL.
 * @return KODE_OK once posted; KODE_NO_RESOURCES when no event or no room.
 */
static kode_status_t request(request_kind_t const kind, int32_t const arg, char const * const text)
{
    /* One block is kept back from the pool, and KODE_SYS_CMD_MARGIN slots of the queue below:
     * a row never takes the room the SDK's answers need (the file's head, "The queue"). */
    RequestEvt * const e = Q_NEW_X(RequestEvt, 1U, CA_REQUEST_SIG);
    if ( NULL == e )
    {
        return KODE_NO_RESOURCES;
    }
    e->kind          = kind;
    e->arg           = arg;
    e->sf            = l_sf;
    e->power_dbm_x10 = l_power;
    e->disarms       = l_disarms;   /* a stop from here on cancels it */
    e->text[0]       = '\0';
    if ( NULL != text )
    {
        (void)snprintf(e->text, sizeof(e->text), "%s", text);
    }
    /* A post that is not made hands the block back to its pool. */
    return QACTIVE_POST_X(AO_ConsoleActions, &e->super, KODE_SYS_CMD_MARGIN, (void *)0) ? KODE_OK
                                                                                       : KODE_NO_RESOURCES;
}

/**
 * @brief Whether a transmitting row may go; the log says why not.
 * @param name The row.
 * @return true once mesh_tx_arm has been given 1.
 */
static bool armed(char const * const name)
{
    if ( !l_tx_armed )
    {
        KLOG_W(TAG, "%s refused: mesh_tx_arm arg=1 first, with an antenna or a 50 ohm load on", name);
    }
    return l_tx_armed;
}

static kode_status_t do_tx_arm(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    l_tx_armed = (0 != arg->i);
    if ( !l_tx_armed )
    {
        l_disarms++;
    }
    /* Arming is a person saying an antenna or a 50 ohm load is on the SMA: the SDK's word on the
     * antennas. Without it every transmit stays at the safe power, 0 dBm. */
    (void)mesh_sys_radio_antennas_confirm(l_tx_armed);
    KLOG_W(TAG, "transmitting is %s", l_tx_armed ? "ARMED" : "disarmed");
    return KODE_OK;
}

static kode_status_t do_sf(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    l_sf = (uint8_t)arg->i;
    return KODE_OK;
}

static kode_status_t do_power(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    l_power = arg->i;
    return KODE_OK;
}

static kode_status_t do_begin(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : request(REQUEST_BEGIN, (int32_t)arg->e, NULL);
}

/* A command: the owner takes it, or the row says why not. */
static kode_status_t do_rx(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : mesh_sys_radio_receive(1 == arg->e);
}

/* An immediate: the level now, on the log. */
static kode_status_t do_rssi(const kode_value_t *arg)
{
    float dbm = 0.0f;

    (void)arg;
    kode_status_t const rc = mesh_sys_radio_rssi(&dbm);
    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "mesh_radio_rssi: %.0f dBm", (double)dbm);
    }
    else
    {
        KLOG_W(TAG, "mesh_radio_rssi: no reading (status %d): the receiver must be on and have sampled", (int)rc);
    }
    return rc;
}

/* A command. */
static kode_status_t do_listen(const kode_value_t *arg)
{
    return (NULL == arg) ? KODE_DENIED : mesh_sys_radio_listen((uint32_t)arg->i);
}

static kode_status_t do_send(const kode_value_t *arg)
{
    if ( (NULL == arg) || (NULL == arg->s) || ('\0' == arg->s[0])
         || (strlen(arg->s) > CONSOLE_TEXT_MAX) )
    {
        return KODE_DENIED;
    }
    return armed("mesh_radio_send") ? request(REQUEST_SEND, 0, arg->s) : KODE_DENIED;
}

static kode_status_t do_test(const kode_value_t *arg)
{
    if ( NULL == arg )
    {
        return KODE_DENIED;
    }
    return armed("mesh_radio_test") ? request(REQUEST_TEST, (int32_t)arg->e, NULL) : KODE_DENIED;
}

/* A command. */
static kode_status_t do_standby(const kode_value_t *arg)
{
    (void)arg;
    return mesh_sys_radio_standby();
}

/* A command that never waits: an emergency stop does not queue behind a request. It disarms as
 * well, and takes the word on the antennas back, so the next transmit has to be asked for again
 * and stays at the safe power until it is. */
static kode_status_t do_stop(const kode_value_t *arg)
{
    (void)arg;
    l_tx_armed = false;
    l_disarms++;
    kode_status_t const rc = mesh_sys_stop();
    (void)mesh_sys_radio_antennas_confirm(false);
    return rc;
}

static kode_status_t do_gnss(const kode_value_t *arg)
{
    static const request_kind_t KIND[] = { REQUEST_GNSS_ON, REQUEST_GNSS_OFF, REQUEST_GNSS_RESET };

    return ((NULL == arg) || ((unsigned)arg->e >= (unsigned)Q_DIM(KIND))) ? KODE_DENIED
                                                                          : request(KIND[arg->e], 0, NULL);
}

static const char * const BANDS[]   = { "sub_ghz", "2_4_ghz" };
static const char * const ONOFF[]   = { "off", "on" };
static const char * const SIGNALS[] = { "carrier", "preamble" };
static const char * const GNSS[]    = { "on", "off", "reset" };

static const kode_function_t FUNCTIONS[] = {
    { .name = "mesh_tx_arm", .arg = KODE_TYPE_INT, .min = 0, .max = 1, .run = do_tx_arm,
      .desc = "1 allows send and test, 0 refuses; starts 0" },
    { .name = "mesh_sf", .arg = KODE_TYPE_INT, .min = 7, .max = 12, .run = do_sf,
      .desc = "Spreading factor for the next mesh_begin" },
    { .name = "mesh_power_x10", .arg = KODE_TYPE_INT, .min = -100, .max = 200, .run = do_power,
      .desc = "Power, 1/10 dBm, next begin; 85 is 868 cap" },
    { .name = "mesh_begin", .arg = KODE_TYPE_ENUM, .enums = BANDS, .n_enums = 2U, .run = do_begin,
      .desc = "Set the radio up on a band: sub_ghz or 2_4_ghz" },
    { .name = "mesh_rx", .arg = KODE_TYPE_ENUM, .enums = ONOFF, .n_enums = 2U, .run = do_rx,
      .desc = "The receiver: on or off" },
    { .name = "mesh_radio_rssi", .run = do_rssi,
      .desc = "Read the RSSI now, on the log" },
    { .name = "mesh_listen", .arg = KODE_TYPE_INT, .min = 1, .max = 1000, .run = do_listen,
      .desc = "Receive for N ms; the chip ends it" },
    { .name = "mesh_radio_send", .arg = KODE_TYPE_STRING, .run = do_send,
      .desc = "TRANSMITS text, 48 chars; armed, load on" },
    { .name = "mesh_radio_test", .arg = KODE_TYPE_ENUM, .enums = SIGNALS, .n_enums = 2U, .run = do_test,
      .desc = "LAB build only: test signal into a LOAD; armed" },
    { .name = "mesh_standby", .run = do_standby,
      .desc = "Everything off air, band kept" },
    { .name = "mesh_stop", .run = do_stop,
      .desc = "Everything off air NOW; disarms" },
    { .name = "mesh_gnss", .arg = KODE_TYPE_ENUM, .enums = GNSS, .n_enums = 3U, .run = do_gnss,
      .desc = "The GNSS receiver: on, off or reset" },
};

/* ---- Variables ---------------------------------------------------------- */

/* Each one is a single word: the console carries a value bare and refuses one
 * that is empty or longer than KODE_VALUE_STR_MAX. These three write the
 * common shapes - on/off, an unsigned number, a signed one - from a field of
 * the SDK's snapshot. */
#define VAR_ONOFF(fn, expr)                                                    \
    static kode_status_t fn(char *buf, size_t cap)                             \
    {                                                                          \
        mesh_status_t st;                                                      \
        (void)mesh_sys_status_read(&st);                                                      \
        (void)snprintf(buf, cap, "%s", (expr) ? "on" : "off");                 \
        return KODE_OK;                                                        \
    }

#define VAR_UNSIGNED(fn, expr)                                                 \
    static kode_status_t fn(char *buf, size_t cap)                             \
    {                                                                          \
        mesh_status_t st;                                                      \
        (void)mesh_sys_status_read(&st);                                                      \
        (void)snprintf(buf, cap, "%lu", (unsigned long)(expr));                \
        return KODE_OK;                                                        \
    }

#define VAR_SIGNED(fn, expr)                                                   \
    static kode_status_t fn(char *buf, size_t cap)                             \
    {                                                                          \
        mesh_status_t st;                                                      \
        (void)mesh_sys_status_read(&st);                                                      \
        (void)snprintf(buf, cap, "%ld", (long)(expr));                         \
        return KODE_OK;                                                        \
    }

VAR_ONOFF(get_present,   st.present)
VAR_ONOFF(get_ready,     st.radio_ready)
VAR_UNSIGNED(get_sf_now, st.spreading_factor)
VAR_SIGNED(get_power_now, st.power_dbm_x10)
VAR_ONOFF(get_sending,   st.sending)
VAR_ONOFF(get_receiving, st.receiving)
VAR_ONOFF(get_testing,   st.testing)
VAR_ONOFF(get_fault,     st.amplifier_fault)
VAR_UNSIGNED(get_duty,   st.duty_wait_ms)
VAR_UNSIGNED(get_rx_count,   st.packets_received)
VAR_UNSIGNED(get_rx_dropped, st.packets_dropped)
VAR_UNSIGNED(get_rx_waiting, st.packets_waiting)
VAR_UNSIGNED(get_tx_count,   st.packets_sent)
VAR_SIGNED(get_rssi,     st.last_rssi_dbm)
VAR_ONOFF(get_gnss_on,   st.gnss_on)
VAR_UNSIGNED(get_gnss_sats,  st.fix.satellites_used)
VAR_UNSIGNED(get_gnss_count, st.gnss_sentences)
VAR_UNSIGNED(get_gnss_baud,  st.gnss_baud)

static kode_status_t get_seen(char *buf, size_t cap)
{
    mesh_status_t st;
    (void)mesh_sys_status_read(&st);
    (void)snprintf(buf, cap, "%s", (MESH_MODULE_MESH == st.seen)    ? "mesh"
                                   : (MESH_MODULE_OTHER == st.seen) ? "other"
                                                                    : "none");
    return KODE_OK;
}

static kode_status_t get_tx_armed(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%u", l_tx_armed ? 1U : 0U);
    return KODE_OK;
}

static kode_status_t get_band_now(char *buf, size_t cap)
{
    mesh_status_t st;
    (void)mesh_sys_status_read(&st);
    (void)snprintf(buf, cap, "%s", !st.radio_ready ? "none"
                                   : ((MESH_BAND_2_4_GHZ == st.band) ? "2_4_ghz" : "sub_ghz"));
    return KODE_OK;
}

/* Whether there is a fix, never where. */
static kode_status_t get_gnss_fix(char *buf, size_t cap)
{
    mesh_status_t st;
    (void)mesh_sys_status_read(&st);
    (void)snprintf(buf, cap, "%s", st.fix.valid ? "fix" : "nofix");
    return KODE_OK;
}

/* The last request and its answer, as one word: kind:status. */
static kode_status_t get_last(char *buf, size_t cap)
{
    static const char * const NAME[] = { "none", "begin", "send", "test", "gnss_on", "gnss_off", "gnss_reset" };
    uint32_t const word = l_last;
    unsigned const kind = (word >> 16) % (unsigned)Q_DIM(NAME);
    (void)snprintf(buf, cap, "%s:%d", NAME[kind], (int)(int16_t)(word & 0xFFFFU));
    return KODE_OK;
}

static const kode_variable_t VARIABLES[] = {
    { .name = "mesh_present", .get = get_present, .desc = "A Mesh is on J3 and held: on, off" },
    { .name = "mesh_seen", .get = get_seen, .desc = "What J3 holds: none, mesh or other" },
    { .name = "mesh_tx_armed", .get = get_tx_armed, .desc = "1 once mesh_tx_arm allowed transmitting" },
    { .name = "mesh_ready", .get = get_ready, .desc = "A band is set (mesh_begin done): on, off" },
    { .name = "mesh_band_now", .get = get_band_now, .desc = "The band set: sub_ghz, 2_4_ghz, none" },
    { .name = "mesh_sf_now", .get = get_sf_now, .desc = "The spreading factor set" },
    { .name = "mesh_power_now", .get = get_power_now, .desc = "The power asked at the last begin, 1/10 dBm" },
    { .name = "mesh_sending", .get = get_sending, .desc = "A packet is on air: on, off" },
    { .name = "mesh_receiving", .get = get_receiving, .desc = "The receiver is on: on, off" },
    { .name = "mesh_testing", .get = get_testing, .desc = "A test signal is on air: on, off" },
    { .name = "mesh_fault", .get = get_fault, .desc = "Amplifier fault: on after one" },
    { .name = "mesh_duty_ms", .get = get_duty, .desc = "Ms until the 868 duty budget has room" },
    { .name = "mesh_rx_count", .get = get_rx_count, .desc = "Packets received since the module came" },
    { .name = "mesh_rx_dropped", .get = get_rx_dropped, .desc = "Packets lost to a full queue" },
    { .name = "mesh_rx_waiting", .get = get_rx_waiting, .desc = "Packets received and not yet read" },
    { .name = "mesh_tx_count", .get = get_tx_count, .desc = "Packets that left whole" },
    { .name = "mesh_rssi_dbm", .get = get_rssi, .desc = "The last RSSI reading, dBm" },
    { .name = "mesh_gnss_on", .get = get_gnss_on, .desc = "The GNSS is open: on, off" },
    { .name = "mesh_gnss_fix", .get = get_gnss_fix, .desc = "fix or nofix (never the position)" },
    { .name = "mesh_gnss_sats", .get = get_gnss_sats, .desc = "Satellites used in the fix" },
    { .name = "mesh_gnss_count", .get = get_gnss_count, .desc = "Sentences with a good checksum" },
    { .name = "mesh_gnss_baud", .get = get_gnss_baud, .desc = "The rate the GNSS locked at; 0: none" },
    { .name = "mesh_last", .get = get_last, .desc = "The last request and its status, kind:status" },
};

/* ---- What a request came to ----------------------------------------------- */

/** @brief What to try, for a status the SDK gave with no answer to come. */
static char const *hint(kode_status_t const rc)
{
    switch ( rc )
    {
        case KODE_DENIED:       return "no module, or the same kind of request is still running";
        case KODE_NO_RESOURCES: return "no room just now; ask again";
        default:                return "see the status";
    }
}

/**
 * @brief What a request came to, on the log and in mesh_last.
 * @param kind The request.
 * @param rc   What it answered.
 * @param why  Why, when the SDK said (mesh_sys_why_text()); MESH_WHY_NONE when the SDK did not
 *             take the request and there is no answer to come, and rc says it alone.
 */
static void show(request_kind_t const kind, kode_status_t const rc, mesh_why_t const why)
{
    static const char * const NAME[] = { "mesh_begin", "mesh_radio_send", "mesh_radio_test",
                                         "mesh_gnss on", "mesh_gnss off", "mesh_gnss reset" };
    char const * const name = ((unsigned)kind < Q_DIM(NAME)) ? NAME[kind] : "?";

    l_last = (((uint32_t)kind + 1U) << 16) | ((uint32_t)rc & 0xFFFFU);
    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "%s: done", name);
    }
    else
    {
        KLOG_W(TAG, "%s: refused (status %d): %s", name, (int)rc,
               (MESH_WHY_NONE != why) ? mesh_sys_why_text(why) : hint(rc));
    }
}

/**
 * @brief Ask for what a row posted.
 * @param me The AO.
 * @param r  The request.
 */
static void ask(ConsoleActions * const me, RequestEvt const * const r)
{
    kode_status_t rc;

    if ( ((REQUEST_SEND == r->kind) || (REQUEST_TEST == r->kind))
         && (!l_tx_armed || (l_disarms != r->disarms)) )
    {
        /* Disarmed or stopped between the row and here, even if armed again since: the row's
         * answer was `>ok`. */
        KLOG_W(TAG, "transmitting was disarmed; this request is dropped");
        return;
    }
    switch ( r->kind )
    {
        case REQUEST_BEGIN:
            rc = mesh_sys_radio_begin(&me->super, (1 == r->arg) ? MESH_BAND_2_4_GHZ : MESH_BAND_SUB_GHZ,
                                      r->sf, r->power_dbm_x10);
            break;
        case REQUEST_SEND:
            rc = mesh_sys_radio_send(&me->super, (uint8_t const *)r->text, (uint8_t)strlen(r->text));
            break;
        case REQUEST_TEST:
            rc = mesh_sys_radio_test(&me->super,
                                     (1 == r->arg) ? MESH_TEST_SIGNAL_PREAMBLE : MESH_TEST_SIGNAL_CARRIER,
                                     CONSOLE_TEST_MS);
            break;
        case REQUEST_GNSS_ON:
            rc = mesh_sys_gnss_begin(&me->super);
            break;
        case REQUEST_GNSS_OFF:
            rc = mesh_sys_gnss_end(&me->super);
            break;
        case REQUEST_GNSS_RESET:
            rc = mesh_sys_gnss_reset(&me->super);
            break;
        default:
            rc = KODE_DENIED;
            break;
    }
    if ( KODE_OK != rc )
    {
        /* Not taken: no answer will come. DENIED is a door still shut (the same kind is still
         * running) or a module that is not there; NO_RESOURCES a queue or a pool within its margin. */
        show(r->kind, rc, MESH_WHY_NONE);
    }
}

/**
 * @brief What the SDK told, on the log; a packet is read and described.
 * @param me The AO.
 * @param e  The news.
 */
static void handle_news(ConsoleActions * const me, QEvt const * const e)
{
    int32_t const value = ((mesh_sys_news_t const *)e)->value;

    switch ( e->sig )
    {
        case MESH_MODULE_FOUND_SIG:
            KLOG_I(TAG, "the Mesh is on J3");
            break;
        case MESH_MODULE_LOST_SIG:
            l_tx_armed = false;
            l_disarms++;
            (void)mesh_sys_radio_antennas_confirm(false);
            KLOG_W(TAG, "the Mesh left J3; transmitting is disarmed");
            break;
        case MESH_PACKET_RECEIVED_SIG:
            while ( KODE_OK == mesh_sys_radio_read(&me->packet) )
            {
                KLOG_I(TAG, "packet: %u bytes, RSSI %.0f dBm, SNR %.1f dB, CRC %s", (unsigned)me->packet.length,
                       (double)me->packet.rssi_dbm, (double)me->packet.snr_db,
                       me->packet.crc_valid ? "good" : "BAD or none");
            }
            break;
        case MESH_LISTEN_ENDED_SIG:
            KLOG_I(TAG, "the timed listen ended");
            break;
        case MESH_RADIO_FAILED_SIG:
            KLOG_W(TAG, "radio error: chip word 0x%04lX, reason %lu; mesh_begin sets it up again",
                   (unsigned long)((uint32_t)value & MESH_RADIO_ERROR_CHIP_MASK),
                   (unsigned long)((uint32_t)value >> 16));
            break;
        case MESH_WRONG_MODULE_SIG:
            KLOG_W(TAG, "J3 holds a module that is not a Mesh");
            break;
        case MESH_SETTING_LOST_SIG:
            KLOG_W(TAG, "the setting was lost (0x%lX: 1 radio, 2 receiver, 4 GNSS); mesh_begin sets it up again",
                   (unsigned long)value);
            break;
        case MESH_SETTING_RESTORED_SIG:
            KLOG_I(TAG, "the SDK put the setting back (0x%lX)", (unsigned long)value);
            break;
        case MESH_TEST_ENDED_SIG:
            KLOG_I(TAG, "the test signal ended by itself (%s)",
                   (MESH_TEST_END_CHIP == (uint32_t)value) ? "the chip's timer" : "the SDK's deadline");
            break;
        default:
            break;   /* the preamble, header and search news come in dozens and say nothing here */
    }
}

/* ---- The Active Object -------------------------------------------------- */

void ConsoleActions_ctor(void)
{
    ConsoleActions * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&ConsoleActions_initial));
}

void ConsoleActions_start(void)
{
    QActive_setAttr(AO_ConsoleActions, TASK_NAME_ATTR, "console_actions");
    QACTIVE_START(AO_ConsoleActions, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);

    /* A table with one bad row is refused whole, with a log line naming the
     * row; a name the app already uses answers KODE_EXISTS. */
    if ( KODE_FUNCTIONS_ADD(FUNCTIONS) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the functions");
    }
    if ( KODE_VARIABLES_ADD(VARIABLES) != KODE_OK )
    {
        KLOG_E(TAG, "the console refused the variables");
    }
}

/**
 * @brief The initial transition: the SDK, and the news.
 *
 * Runs in the thread that called kode_init(). mesh_sys_begin() switches J3's supply on and starts
 * the owner; it does not wait for a module.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ConsoleActions_serving.
 */
static QState ConsoleActions_initial(ConsoleActions * const me, void const * const par)
{
    (void)par;

    kode_status_t const rc = mesh_sys_begin();
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the SDK did not start (status %d)", (int)rc);
    }
    (void)mesh_sys_subscribe(&me->super);

    KLOG_I(TAG, "up; the module's rows are on the console, transmitting is disarmed");
    return Q_TRAN(&ConsoleActions_serving);
}

/**
 * @brief The one state: requests to the SDK, answers to the log, and what the SDK tells.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for its own events, Q_SUPER() otherwise.
 */
static QState ConsoleActions_serving(ConsoleActions * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case CA_REQUEST_SIG: {
            ask(me, Q_EVT_CAST(RequestEvt));
            break;
        }
        case MESH_RADIO_BEGIN_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);
            show(REQUEST_BEGIN, a->rc, a->why);
            break;
        }
        case MESH_RADIO_SEND_RSP_SIG: {
            mesh_sys_send_answer_t const * const a = Q_EVT_CAST(mesh_sys_send_answer_t);
            show(REQUEST_SEND, a->rc, a->why);
            if ( KODE_OK == a->rc )
            {
                KLOG_I(TAG, "the packet left whole: %lu ms on air", (unsigned long)a->airtime_ms);
            }
            else if ( MESH_WHY_DUTY == a->why )
            {
                KLOG_I(TAG, "the duty cycle has no room; %lu ms to go", (unsigned long)a->wait_ms);
            }
            else
            {
                /* the reason is on the line above */
            }
            break;
        }
        case MESH_RADIO_TEST_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);
            show(REQUEST_TEST, a->rc, a->why);
            break;
        }
        case MESH_GNSS_BEGIN_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);
            show(REQUEST_GNSS_ON, a->rc, a->why);
            break;
        }
        case MESH_GNSS_END_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);
            show(REQUEST_GNSS_OFF, a->rc, a->why);
            break;
        }
        case MESH_GNSS_RESET_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);
            show(REQUEST_GNSS_RESET, a->rc, a->why);
            break;
        }
        default: {
            if ( (e->sig >= MESH_MODULE_FOUND_SIG) && (e->sig <= MESH_HEADER_FAILED_SIG) )
            {
                handle_news(me, e);
            }
            else
            {
                status = Q_SUPER(&QHsm_top);
            }
            break;
        }
    }

    return status;
}
