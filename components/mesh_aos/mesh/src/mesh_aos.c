/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_aos.c
 * @brief The module's owner. See mesh_aos.h.
 *
 * One Active Object (mesh_ao.c), and the only code that touches the
 * module. It takes requests off its queue as they come; every
 * MESH_AO_TICK_MS it reads the radio's interrupt line, reads the GNSS and
 * checks the module is still answering; and the radio's interrupt wakes it
 * to read the chip at once. The rules that matter here:
 *
 *  - the module is found by its fingerprint - the four lines its pull
 *    resistors hold high - read three times in a row against the P4's
 *    pull-downs, and then confirmed: the LR2021 is opened and asked its
 *    version, and only a radio that answers is taken (a Camera or an F*CK with
 *    a like fingerprint would not);
 *  - that same version read is how the owner knows the module is still there:
 *    every ALIVE_MS, with nothing on air, it asks again, and two silences in a
 *    row mean the module has left (an unplugged header reads all zeros);
 *  - a received packet is read out of the chip into a small ring the app pops
 *    with mesh_aos_packet_read(), oldest first; a full ring drops the newest and
 *    counts it;
 *  - a sub-GHz transmit is held to the duty cycle by a token bucket
 *    (mesh_plan.c): a send that has no budget is refused with the milliseconds
 *    to wait, and never half-charged;
 *  - the amplifiers are only ever driven into their load by the app's choice;
 *    mesh_aos_stop() puts the radio to standby and takes any test signal off
 *    air, and goes round the queue so a full queue cannot lose it;
 *  - nothing is left on air by accident. Every way out of a keyed state goes
 *    through radio_off(), which confirms standby (three tries) and, failing
 *    that, resets the chip; a test signal has a deadline of its own (its time
 *    and a grace) that the owner enforces whatever the chip does; a packet has
 *    the chip's timer (mesh_drv_lr2021_send) and the owner's backstop behind it; an
 *    amplifier fault is latched until mesh_radio_begin() resets the chip;
 *  - the module ships with its antennas loose and the chip cannot tell one is
 *    missing, so until the app confirms them every transmit is
 *    planned for the safe power at most (mesh_plan_power_allowed()), in every
 *    build; an amplifier fault suspects its band's antenna and holds that band
 *    there again until the app confirms once more. The owner never begins the
 *    radio again by itself, so a fault at the safe power cannot loop;
 *  - a caller that gives up on a request cannot make it run later or twice:
 *    mesh_aos_call() withdraws what the owner has not begun and waits for what
 *    it has.
 *
 * The radio driver blocks while the chip is BUSY, which is microseconds for a
 * command and longer only for a reset or a calibration; those happen on the
 * owner's task, so a long calibration delays the GNSS's next read and
 * nothing else. A call waits for its answer on the caller's task
 * (mesh_aos_call()), never on the owner's.
 *
 * mesh_ao.c says at what level it runs, and why.
 *
 * v1 holds the module in its upright orientation only: the rotated pin-map is
 * not yet known for this connector, so a module plugged in turned round is
 * read as "something that is not a Mesh" rather than guessed at.
 */
#include "mesh_aos.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"
#include "sdkconfig.h"

#include "mesh_ao.h"
#include "mesh_bsp.h"
#include "mesh_drv_gnss.h"
#include "mesh_drv_lr2021.h"
#include "mesh_drv_pins.h"
#include "mesh_plan.h"
#include "mesh_region.h"

/* J3's 3V3 is the module's supply, its enable a pin of the Dot's C5, and its
 * state when an app starts is not promised. kode_sys_exp_power() asks AO_Radio to
 * switch it over the link to the C5 and EXP_POWER_SIG says how it went. An image
 * built without the radio (CONFIG_KODEOS_RADIO=n) keeps the C5 in reset and J3
 * always powered: the call is denied, the module is found all the same, and
 * nothing can switch it off (mesh_supply_t, MESH_SUPPLY_ALWAYS_ON). */

#define TAG            "mesh"
#define TICK_MS        MESH_AO_TICK_MS
#define DT_MAX_MS      1000U
#define RETRY_STEP_MS  10U      /* kode_retry's step: a full queue is asked again this often until the deadline */
#define POS_SLOTS      (KODE_SYS_EXP_POS_MAX + 1U)
#define SEND_SLOTS     2U       /* a payload in flight, and the next caller's */
#define RX_SLOTS       4U
#define TX_MARGIN_MS   200U     /* a send's chip timeout plus this, then tx_busy clears */
#define STANDBY_TRIES  3U       /* SetStandby is asked this often before the chip is reset */
#define MESH_LOAN_LOOK  "mesh.look"

/* The radio and GNSS lines (positions 1-12) are lent as one; the I2C pair
 * (13-14) is lent apart, so the app may be given the pair alone. */
#define CORE_FIRST     MESH_BSP_POSITION_GNSS_RESET   /* 1 */
#define CORE_LAST      MESH_BSP_POSITION_LORA_BUSY    /* 12 */
#define CORE_COUNT     ((CORE_LAST - CORE_FIRST) + 1U)

_Static_assert(MESH_AOS_CALIBRATIONS == MESH_DRV_LR2021_CALIBRATIONS, "the chip's slots");
_Static_assert((unsigned)MESH_BAND_SUB_GHZ == (unsigned)MESH_PLAN_BAND_SUB_GHZ, "band numbers");
_Static_assert((unsigned)MESH_BAND_2_4_GHZ == (unsigned)MESH_PLAN_BAND_2_4_GHZ, "band numbers");
_Static_assert((unsigned)MESH_BAND_COUNT == (unsigned)MESH_PLAN_BAND_COUNT, "band numbers");


typedef struct {
    uint8_t  data[MESH_AOS_PAYLOAD_MAX];
    uint8_t  length;
    bool     crc_valid;
    float    rssi_dbm;
    float    snr_db;
} rx_slot_t;

/* What the app last asked to be set up, kept so that a module that leaves and
 * comes back is put as it was (restore()). A setting that is lost for any other
 * reason is forgotten, and so is one the app ended. A send or a test is never
 * part of it. */
typedef struct {
    bool                   radio;        /* a begin, a configure or a retune is in force   */
    bool                   configured;   /* ... by a whole setting (config), not a begin   */
    uint8_t                band;         /* begin's                                        */
    uint8_t                sf;
    int32_t                power_cdbm;
    mesh_drv_lora_config_t config;       /* configure's and retune's                       */
    bool                   receive;      /* the receiver on                                */
    bool                   gnss;         /* the GNSS on                                    */
} want_t;

/* Who a held answer goes to: a task that waits (seq) or an Active Object that asked (done). */
typedef struct {
    uint32_t           seq;    /* the waiting call; 0 none */
    void              *ask;
    mesh_aos_done_fn_t done;   /* the asking Active Object's; NULL none */
    bool               held;   /* the waiting call was told the owner holds it: its answer has a record of its own */
} reply_t;

typedef struct {
    /* The plumbing. */
    SemaphoreHandle_t reply;
    SemaphoreHandle_t callers;
    SemaphoreHandle_t held_sem;  /* the answer of the call the owner holds (a send, a search) */
    kode_status_t     held_rc;
    uint32_t          held_seq;
    uint32_t          held_out1;
    uint32_t          held_out2;
    uint8_t           held_why;
    kode_status_t     rc;
    uint32_t          rc_seq;
    uint32_t          rc_out1;   /* the answer's two numbers: a copy, as out1 and out2  */
    uint32_t          rc_out2;   /* are cleared when the next request begins            */
    uint32_t          seq;
    uint32_t          out1;
    uint32_t          out2;
    uint8_t           why;       /* the reason of the request being served           */
    uint8_t           rc_why;    /* and of the answer given                          */
    uint32_t          epoch;

    /* The module. */
    bool              present;
    bool              radio_open;
    uint8_t           gpio[POS_SLOTS];
    uint8_t           scl;
    uint8_t           sda;
    uint8_t           spi_host;      /* kode_sys_exp_ctrl()'s, while the module is held */
    uint8_t           uart_port;     /* the same */
    uint32_t          look_ms;
    bool              core_busy_said;
    bool              busy_said;     /* busy_say() has spoken since the module was last taken */
    bool              i2c_taken;
    uint32_t          alive_ms;
    uint32_t          misses;
    mesh_module_t        seen;
    uint16_t          seen_level;

    /* The radio. */
    bool                     radio_ready;      /* configured for a band */
    uint8_t                  band;             /* a mesh_plan_band_t */
    uint8_t                  spreading_factor;
    int32_t                  power_cdbm;       /* asked */
    int32_t                  power_applied_cdbm;   /* what the setting comes to */
    mesh_drv_lora_settings_t settings;         /* in force */
    uint32_t                 calibrated[MESH_AOS_CALIBRATIONS]; /* 4 MHz steps, and the path's bit; 0 none */
    bool                     duty_counted;     /* this setting's airtime spends the bucket */
    bool                     sending;
    uint32_t                 send_left_ms;     /* the send's backstop */
    bool                     receiving;
    bool                     listening;
    uint32_t                 listen_left_ms;
    bool                     arriving;         /* a packet is on its way in */
    uint32_t                 arriving_left_ms;
    bool                     cad_running;
    bool                     cad_detected;     /* since the search began */
    bool                     cad_receive;      /* the search goes on to receive what it sees */
    bool                     cad_rx;           /* a receive that a search began is running */
    uint32_t                 cad_left_ms;      /* the search's backstop */
    reply_t                  cad_reply;        /* who waits for the search's end */
    reply_t                  send_reply;       /* who waits for the packet to be gone */
    reply_t                  held_now;         /* set by the request being served when it answers later */
    bool                     answer_held;      /* the request just served answers later */
    bool                     testing;
    uint32_t                 test_left_ms;     /* the test's own deadline: its time + a grace */
    bool                     amplifier_fault;  /* until mesh_radio_begin() */
    bool                     need_reset;       /* mesh_radio_begin() must reset the chip first */
    bool                     radio_asleep;     /* ... because it sleeps (radio_sleep()), NRESET high */
    uint32_t                 antennas_epoch;   /* the app's word on the antennas the setting in force was planned with */
    bool                     power_capped;     /* the setting in force is held below what was asked by the safe power */
    uint8_t                  power_said[MESH_PLAN_BAND_COUNT]; /* what the log last said of each band's power: a said_t */
    mesh_plan_duty_t         duty;
    want_t                   want;             /* what the app last set up, for a module that comes back */
    uint32_t                 last_airtime_us;  /* the last send's, for the wait */
    float                    last_rssi;
    bool                     rssi_valid;       /* a sample was taken since the receiver came on */
    uint32_t                 rssi_ms;          /* counts to the next sample */
    uint16_t                 radio_errors;
    uint32_t                 packets_received;
    uint32_t                 packets_dropped;
    uint32_t                 packets_sent;

    /* The GNSS. */
    bool              gnss_on;
    bool              gnss_asleep;   /* told to sleep (gnss_quiet()); the driver stays open */
    mesh_drv_gnss_status_t   gnss;
    uint32_t          fix_reports;   /* the parser's count, as last seen */
    uint32_t          fix_at_ms;     /* when it last moved */

    /* J3's supply, and the board's rest. */
    uint8_t           supply;        /* a mesh_supply_t */
    bool              power_wish;    /* what was last asked of kode_sys_exp_power() */
    bool              power_retry;   /* the command found no room: asked again at the next tick */
    uint32_t          rest_lost;     /* MESH_SETTING_* the rest took off the chip and AWAKE puts back */
} own_t;

static own_t             s;
static bool              s_started;
static bool              s_duty_ready;          /* the duty bucket was filled once, when the app first started the owner */
static volatile bool     s_ended;               /* the app let go (mesh_aos_end()): no looking until it starts again */
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static mesh_aos_status_t  s_stat;
static volatile uint32_t s_stop_epoch;
static uint32_t          s_cancel_seq;          /* under s_lock: requests up to this are withdrawn */
static uint32_t          s_serving_seq;         /* under s_lock: the one the owner last began      */
static uint32_t          s_tick_at;             /* the owner's: when its tick last ran */
static uint32_t          s_forget;              /* under s_lock: MESH_SETTING_* the app ended while the owner could not hear it */
static mesh_aos_news_fn_t s_news_fn;            /* under s_lock: the extended layer's sink */
static uint32_t          s_news_dropped;        /* under s_lock */
static uint32_t          s_answers_dropped;     /* under s_lock */
static bool              s_starting;            /* under s_lock: a mesh_aos_start() is on its way */
static bool              s_antennas_ok;         /* under s_lock: the app confirmed the antennas */
static uint8_t           s_antennas_suspect;    /* under s_lock: bands whose amplifier faulted since, bit (band) */
static uint32_t          s_antennas_epoch;      /* under s_lock: one more with every change of the two above */

/* The payloads of the sends in flight, one slot a call: a request carries
 * the slot's number and not the bytes, which would take a block from the
 * jumbo tier kodeOS keeps for installs. A slot is the caller's from
 * mesh_aos_call() taking it until the owner has served or skipped its
 * request; s_slot_busy under s_lock. */
static uint8_t           s_slot[SEND_SLOTS][MESH_AOS_PAYLOAD_MAX];
static bool              s_slot_busy[SEND_SLOTS];

/* The received-packet ring, guarded by s_lock. */
static rx_slot_t         s_ring[RX_SLOTS];
static uint8_t           s_ring_head;   /* where the next lands */
static uint8_t           s_ring_tail;   /* the oldest unread */
static uint8_t           s_ring_n;

/* The fingerprint's four positions, lent together for the look. */
static const uint8_t LOOK_POS[4] = {
    MESH_BSP_POSITION_LORA_RESET, MESH_BSP_POSITION_LORA_CS, MESH_BSP_POSITION_I2C_SCL, MESH_BSP_POSITION_I2C_SDA,
};

static uint16_t bit_of(uint8_t pos)
{
    return (uint16_t)(1U << (pos - 1U));   /* bit (position - 1), as mesh_plan expects */
}

/* ---- Events ---------------------------------------------------------------- */

static void reply_give(const reply_t *to, kode_status_t rc);
static uint32_t owed_ms(void);
static void cad_end(kode_status_t rc, bool detected);
static void send_end(kode_status_t rc, uint8_t why);

/**
 * @brief Tell what happened: the standard layer's events task gets it through a queue that never
 *        blocks this one, and the extended layer's sink - if one is connected - posts it to the
 *        Active Objects that subscribed. Neither runs the app's code on this task.
 */
static void emit(uint8_t ev, uint32_t arg)
{
    (void)mesh_aos_events_push(ev, (int32_t)arg);   /* a lost event is counted there */

    taskENTER_CRITICAL(&s_lock);
    mesh_aos_news_fn_t const sink = s_news_fn;
    taskEXIT_CRITICAL(&s_lock);
    if (sink != NULL) {
        uint8_t const lost = sink(ev, arg);
        if (lost != 0U) {
            taskENTER_CRITICAL(&s_lock);
            s_news_dropped += lost;
            taskEXIT_CRITICAL(&s_lock);
        }
    }
}

void mesh_aos_answer_dropped(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_answers_dropped++;
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief The radio's interrupt: wake the owner so a packet is read without waiting. */
static void radio_isr(void *arg)
{
    (void)arg;
    mesh_ao_radio_interrupt_from_isr();
}

/* ---- The status ------------------------------------------------------------ */

static void publish(void)
{
    mesh_aos_status_t st;

    (void)memset(&st, 0, sizeof(st));
    st.present       = s.present;
    st.radio_ready   = s.radio_ready;
    st.radio_asleep  = s.radio_asleep;
    st.band          = s.band;
    st.spreading_factor            = s.spreading_factor;
    if (s.radio_ready) {
        st.frequency_hz = s.settings.frequency_hz;
        st.bandwidth_hz = mesh_drv_lora_bandwidth_hz(s.settings.bandwidth);
        st.coding_rate  = (uint8_t)(s.settings.coding_rate + 4U);
        st.preamble     = s.settings.preamble;
        st.sync_word    = s.settings.sync_word;
    }
    st.power_cdbm          = s.power_cdbm;
    st.power_applied_cdbm  = s.radio_ready ? s.power_applied_cdbm : 0;
    st.sending       = s.sending;
    st.receiving         = s.receiving;
    st.arriving      = s.arriving;
    st.cad_running   = s.cad_running;
    st.testing       = s.testing;
    st.amplifier_fault      = s.amplifier_fault;
    st.power_capped  = s.radio_ready && s.power_capped;
    taskENTER_CRITICAL(&s_lock);
    st.antennas_confirmed = s_antennas_ok;
    st.antenna_suspected  = s_antennas_suspect;
    taskEXIT_CRITICAL(&s_lock);
    st.duty_wait_ms  = 0U;
    if (s.radio_ready && s.duty_counted) {
        /* Refill the bucket to now (spends nothing), then say how long until it
         * holds the last send's airtime: a live countdown for mesh_radio_send_wait_ms().
         * 0 on 2.4 GHz and before the first send, as the status field says. */
        (void)mesh_plan_duty_allows(&s.duty, 0U, kode_sys_time_ms());
        st.duty_wait_ms = mesh_plan_duty_wait_ms(&s.duty, s.last_airtime_us);
    }
    st.last_rssi_dbm = s.last_rssi;
    st.rssi_valid    = s.rssi_valid && s.receiving;
    st.packets_received      = s.packets_received;
    st.packets_dropped    = s.packets_dropped;
    st.packets_sent      = s.packets_sent;
    st.radio_errors  = s.radio_errors;
    st.gnss_on       = s.gnss_on;
    st.fix           = s.gnss.fix;
    st.fix_age_ms    = (s.gnss_on && (s.fix_reports != 0U)) ? (kode_sys_time_ms() - s.fix_at_ms) : UINT32_MAX;
    st.gnss_sentences = s.gnss.messages;
    st.gnss_baud     = s.gnss.baud;
    st.gnss_talking  = s.gnss.talking;
    st.gnss_asleep   = s.gnss_asleep;
    st.pps_high           = s.gnss_on && mesh_drv_gnss_pps();
    st.i2c_taken       = s.i2c_taken;
    st.seen          = (uint8_t)s.seen;
    st.seen_level    = s.seen_level;
    st.supply        = s.supply;
    st.events_dropped = mesh_aos_events_dropped();

    taskENTER_CRITICAL(&s_lock);
    st.news_dropped    = s_news_dropped;
    st.answers_dropped = s_answers_dropped;
    st.packets_waiting = s_ring_n;
    s_stat        = st;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- What the app set up, and losing it ------------------------------------- */

/** @brief What is held now, as MESH_SETTING_*: the setting, its receiver, the GNSS. */
static uint32_t held_mask(void)
{
    return (s.radio_ready ? MESH_SETTING_RADIO : 0UL)
           | (s.receiving ? MESH_SETTING_RECEIVER : 0UL)
           | (s.gnss_on ? MESH_SETTING_GNSS : 0UL);
}

/** @brief Forget what the app set up, for the parts in @p what. */
static void want_drop(uint32_t what)
{
    if ((what & MESH_SETTING_RADIO) != 0UL) {
        s.want.radio   = false;
        s.want.receive = false;   /* a receiver needs its setting */
        what |= MESH_SETTING_RECEIVER;
    }
    if ((what & MESH_SETTING_RECEIVER) != 0UL) {
        s.want.receive = false;
    }
    if ((what & MESH_SETTING_GNSS) != 0UL) {
        s.want.gnss = false;
    }
    s.rest_lost &= ~what;   /* what the app ended is not for the wake to put back */
}

/**
 * @brief Tell the app that @p what went away without its asking.
 * @param what      MESH_SETTING_* parts; none says nothing.
 * @param keep_want The module left and may come back: what the app set up is kept
 *                  to put back. Otherwise it is forgotten with the loss.
 */
static void setting_lost(uint32_t what, bool keep_want)
{
    if (what == 0UL) {
        return;
    }
    if (!keep_want) {
        want_drop(what);
    }
    publish();
    emit((uint8_t)MESH_AOS_EVENT_SETTING_LOST, what);
}

/* ---- The received-packet ring ---------------------------------------------- */

static void ring_push(const mesh_drv_lr2021_packet_t *rx, const uint8_t *buf)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_ring_n >= (uint8_t)RX_SLOTS) {
        s.packets_dropped++;   /* the ring is full: the newest is dropped */
    } else {
        rx_slot_t * const slot = &s_ring[s_ring_head];
        (void)memcpy(slot->data, buf, rx->len);
        slot->length      = rx->len;
        slot->crc_valid   = rx->crc_ok;
        slot->rssi_dbm = rx->rssi_dbm;
        slot->snr_db   = rx->snr_db;
        s_ring_head    = (uint8_t)((s_ring_head + 1U) % RX_SLOTS);
        s_ring_n++;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- Finding the module, and losing it ------------------------------------- */

/** @brief One reading of the look's positions, as a mask by (position - 1). */
static uint16_t read_lines(void)
{
    uint16_t level = 0U;

    for (uint8_t i = 0U; i < (uint8_t)(sizeof(LOOK_POS)); i++) {
        if (mesh_drv_pins_level(s.gpio[LOOK_POS[i]])) {
            level |= bit_of(LOOK_POS[i]);
        }
    }
    return level;
}

/** @brief What module_open() found. */
typedef enum {
    OPEN_TAKEN = 0,   /* the radio answered: the module is held           */
    OPEN_NOT_MESH,    /* the lines were ours, but no LR2021 answered      */
    OPEN_BUSY,        /* a line or a controller is somebody else's: no
                       * word on what the module is                      */
} open_result_t;

/** @brief Say once, until it changes, that the module could not be taken for want of @p what. */
static void busy_say(const char *what)
{
    if (!s.busy_said) {
        KLOG_W(TAG, "a Mesh's fingerprint is on J3 but %s is somebody else's (`ps` says whose)",
               what);
        s.busy_said = true;
    }
}

/**
 * @brief A chip that is not in use goes to sleep: 0.47 uA instead of Standby
 *        RC's 1.21 mA (LR20xx datasheet, Table 3-4), with its reset line high.
 *
 * Only from a confirmed standby, so that nothing asleep can be keyed; a chip held in reset is left
 * held. mesh_radio_begin() wakes it with its reset pulse, as it does a held chip (need_reset), and
 * the alive check wakes it for its question and puts it back.
 *
 * @return true when it sleeps; false when it would not confirm standby or take the command.
 */
static bool radio_sleep(void)
{
    if (!s.radio_open) {
        return false;
    }
    if (s.radio_asleep) {
        return true;
    }
    if (s.need_reset) {
        return false;   /* held in reset: leave it so */
    }
    for (uint8_t i = 0U; i < STANDBY_TRIES; i++) {
        if (mesh_drv_lr2021_standby() == KODE_OK) {
            if (mesh_drv_lr2021_sleep() != KODE_OK) {
                return false;
            }
            s.radio_asleep = true;
            s.need_reset   = true;   /* its setting went with the sleep: begin resets it */
            (void)memset(s.calibrated, 0, sizeof(s.calibrated));
            return true;
        }
    }
    return false;
}

/** @brief Take the module: lend its lines, open the radio and confirm it. */
static open_result_t module_open(void)
{
    uint8_t pos[CORE_COUNT];
    uint8_t g[CORE_COUNT];

    for (uint8_t i = 0U; i < (uint8_t)CORE_COUNT; i++) {
        pos[i] = (uint8_t)(CORE_FIRST + i);
    }
    if (kode_sys_exp_lend(pos, CORE_COUNT, MESH_BSP_LOAN_TAG, g) != KODE_OK) {
        busy_say("one of its radio and GNSS lines");
        return OPEN_BUSY;
    }
    for (uint8_t i = 0U; i < (uint8_t)CORE_COUNT; i++) {
        s.gpio[pos[i]] = g[i];
    }
    uint8_t const pair[2] = { MESH_BSP_POSITION_I2C_SCL, MESH_BSP_POSITION_I2C_SDA };
    uint8_t gp[2];
    if (kode_sys_exp_lend(pair, 2U, MESH_BSP_LOAN_PAIR, gp) != KODE_OK) {
        busy_say("its I2C pair");
        (void)kode_sys_exp_return(MESH_BSP_LOAN_TAG);
        return OPEN_BUSY;
    }
    s.gpio[MESH_BSP_POSITION_I2C_SCL] = gp[0];
    s.gpio[MESH_BSP_POSITION_I2C_SDA] = gp[1];
    s.scl = gp[0];
    s.sda = gp[1];

    /* The radio's SPI and the GNSS's UART are kodeOS's to hand out, as the
     * lines are: taken under the module's tag, they go back with them. */
    if ((kode_sys_exp_ctrl(KODE_SYS_EXP_SPI, MESH_BSP_LOAN_TAG, &s.spi_host) != KODE_OK)
        || (kode_sys_exp_ctrl(KODE_SYS_EXP_UART, MESH_BSP_LOAN_TAG, &s.uart_port) != KODE_OK)) {
        busy_say("every SPI or every UART controller");
        (void)kode_sys_exp_return(MESH_BSP_LOAN_PAIR);
        (void)kode_sys_exp_return(MESH_BSP_LOAN_TAG);
        return OPEN_BUSY;
    }

    const mesh_drv_lr2021_pins_t rp = {
        .spi_host    = s.spi_host,
        .chip_select = s.gpio[MESH_BSP_POSITION_LORA_CS],
        .clock       = s.gpio[MESH_BSP_POSITION_LORA_SCK],
        .mosi        = s.gpio[MESH_BSP_POSITION_LORA_MOSI],
        .miso        = s.gpio[MESH_BSP_POSITION_LORA_MISO],
        .busy        = s.gpio[MESH_BSP_POSITION_LORA_BUSY],
        .reset       = s.gpio[MESH_BSP_POSITION_LORA_RESET],
        .dio9        = s.gpio[MESH_BSP_POSITION_LORA_DIO9],
    };
    if (mesh_drv_lr2021_open(&rp, radio_isr, NULL) != KODE_OK) {
        /* ESP-IDF would not install the SPI bus or the interrupt: the driver
         * has said why. Nothing is known about the module. */
        busy_say("the SPI bus or the GPIO interrupt");
        (void)kode_sys_exp_return(MESH_BSP_LOAN_PAIR);
        (void)kode_sys_exp_return(MESH_BSP_LOAN_TAG);
        return OPEN_BUSY;
    }
    s.radio_open = true;

    uint8_t major = 0U;
    uint8_t minor = 0U;
    if (mesh_drv_lr2021_reset(&major, &minor) != KODE_OK) {
        /* The fingerprint matched but no LR2021 answered: not a Mesh after all. */
        mesh_drv_lr2021_close();
        s.radio_open = false;
        (void)kode_sys_exp_return(MESH_BSP_LOAN_PAIR);
        (void)kode_sys_exp_return(MESH_BSP_LOAN_TAG);
        return OPEN_NOT_MESH;
    }
    (void)mesh_drv_lr2021_standby();

    s.present     = true;
    s.radio_ready = false;
    s.arriving    = false;
    s.cad_running = false;
    s.receiving       = false;
    s.listening   = false;
    s.sending     = false;
    s.testing     = false;
    s.test_left_ms = 0U;
    s.amplifier_fault    = false;
    s.need_reset  = false;   /* the chip was just reset */
    s.radio_asleep = false;
    /* ... and a reset chip holds no front-end calibration: what the module knew
     * before it left is not on it any more. */
    (void)memset(s.calibrated, 0, sizeof(s.calibrated));
    s.gnss_on     = false;
    s.gnss_asleep = false;
    s.i2c_taken     = false;
    s.alive_ms    = 0U;
    s.misses      = 0U;
    s.packets_received    = 0U;
    s.packets_dropped  = 0U;
    s.packets_sent    = 0U;
    s.radio_errors = 0U;
    s.last_airtime_us = 0U;
    (void)memset(&s.gnss, 0, sizeof(s.gnss));
    s.busy_said = false;
    /* Nobody has begun the radio yet: it sleeps until somebody does. */
    KLOG_I(TAG, "Mesh on J3: LR2021 firmware %u.%u; the radio %s", (unsigned)major, (unsigned)minor,
           radio_sleep() ? "sleeps until it is begun" : "is in standby");
    return OPEN_TAKEN;
}

/** @brief The module is gone (or was not a Mesh): close everything, hand back. */
static void module_leave(bool announce)
{
    if (s.gnss_on || s.gnss_asleep) {
        mesh_drv_gnss_close();
        s.gnss_on     = false;
        s.gnss_asleep = false;
    }
    s.arriving = false;
    if (s.cad_running) {
        cad_end(KODE_DENIED, false);
    }
    if (s.radio_open) {
        /* Whatever it was doing, it does not any more: standby if it will have
         * it, and its reset line low in any case, because a module that has
         * stopped answering may still be keyed. close() lets the line go, and
         * the module's pull-up starts the chip again in standby. */
        /* Asleep from a confirmed standby, it stays asleep when its lines go back (the
         * module's pull-ups hold NSS and NRESET high); otherwise held as before. */
        if (!radio_sleep()) {
            mesh_drv_lr2021_hold_reset();
        }
        mesh_drv_lr2021_close();
        s.radio_open = false;
        s.radio_asleep = false;
    }
    (void)kode_sys_exp_return(MESH_BSP_LOAN_PAIR);
    (void)kode_sys_exp_return(MESH_BSP_LOAN_TAG);
    (void)memset(&s.gnss, 0, sizeof(s.gnss));   /* its fix leaves with it */

    taskENTER_CRITICAL(&s_lock);
    s_ring_head = 0U;
    s_ring_tail = 0U;
    s_ring_n    = 0U;
    /* The app's word on the antennas was about this module: one put in its place goes out at
     * the safe power until the app confirms again. */
    s_antennas_ok      = false;
    s_antennas_suspect = 0U;
    s_antennas_epoch++;
    taskEXIT_CRITICAL(&s_lock);

    s.present     = false;
    s.radio_ready = false;
    s.receiving       = false;
    s.listening   = false;
    if (s.sending) {
        send_end(KODE_DENIED, MESH_WHY_INTERRUPTED);   /* whoever waits for the packet is told it never went */
    }
    s.testing     = false;
    s.test_left_ms = 0U;
    s.amplifier_fault    = false;
    s.need_reset  = false;
    s.i2c_taken     = false;
    if (announce) {
        KLOG_W(TAG, "the Mesh has left J3: its positions are handed back");
    }
}

static void restore(uint32_t only);

/** @brief While absent: look for it every MESH_BSP_LOOK_MS. */
static void look(uint32_t dt)
{
    s.look_ms += dt;
    if (s.look_ms < MESH_BSP_LOOK_MS) {
        return;
    }
    s.look_ms = 0U;

    uint8_t g[sizeof(LOOK_POS)];
    if (kode_sys_exp_lend(LOOK_POS, sizeof(LOOK_POS), MESH_LOAN_LOOK, g) != KODE_OK) {
        if (!s.core_busy_said) {
            KLOG_W(TAG, "the fingerprint lines are somebody else's (`ps` says whose): "
                        "no Mesh can be looked for");
            s.core_busy_said = true;
        }
        return;
    }
    s.core_busy_said = false;
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(LOOK_POS)); i++) {
        s.gpio[LOOK_POS[i]] = g[i];
        mesh_drv_pins_pull_down(g[i]);
    }

    uint16_t first  = 0U;
    bool     steady = true;
    for (uint8_t r = 0U; r < MESH_BSP_LOOK_ROUNDS; r++) {
        vTaskDelay(pdMS_TO_TICKS((r == 0U) ? MESH_BSP_SETTLE_MS : MESH_BSP_LOOK_GAP_MS));
        uint16_t const level = read_lines();
        if (r == 0U) {
            first = level;
        } else if (level != first) {
            steady = false;
            break;
        } else {
            /* agrees so far */
        }
    }
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(LOOK_POS)); i++) {
        mesh_drv_pins_high_impedance(s.gpio[LOOK_POS[i]]);
    }
    (void)kode_sys_exp_return(MESH_LOAN_LOOK);

    if (!steady) {
        return;
    }
    if (!mesh_plan_fingerprint_matches(first)) {
        mesh_module_t const seen = (first != 0U) ? MESH_MODULE_OTHER : MESH_MODULE_NONE;
        if ((seen == MESH_MODULE_OTHER) && (s.seen != MESH_MODULE_OTHER)) {
            KLOG_W(TAG, "J3 holds something that is not a Mesh (it reads 0x%04x)",
                   (unsigned)first);
            publish();
            emit((uint8_t)MESH_AOS_EVENT_WRONG_MODULE, (uint32_t)seen);
        }
        s.seen       = seen;
        s.seen_level = first;
        return;
    }
    open_result_t const opened = module_open();
    if (opened == OPEN_BUSY) {
        return;   /* said once; looked for again at the next look */
    }
    if (opened == OPEN_NOT_MESH) {
        /* Looked like a Mesh, but no radio answered: told once, as a wrong
         * module is in the branch above. */
        if (s.seen != MESH_MODULE_OTHER) {
            KLOG_W(TAG, "J3's fingerprint is a Mesh's but its radio did not answer");
            s.seen       = MESH_MODULE_OTHER;   /* before publish(): the status says it */
            s.seen_level = first;
            publish();
            emit((uint8_t)MESH_AOS_EVENT_WRONG_MODULE, (uint32_t)MESH_MODULE_OTHER);
        }
        s.seen       = MESH_MODULE_OTHER;
        s.seen_level = first;
        return;
    }
    s.seen       = MESH_MODULE_MESH;
    s.seen_level = first;
    publish();
    emit((uint8_t)MESH_AOS_EVENT_PRESENT, 1U);   /* 1: upright, the only way v1 holds it */
    restore(UINT32_MAX);
}

/** @brief While present: is it still answering? Its version read twice silent. */
static bool gone(uint32_t dt)
{
    s.alive_ms += dt;
    if (s.alive_ms < MESH_BSP_ALIVE_MS) {
        return false;
    }
    s.alive_ms = 0U;

    if (s.sending || s.testing) {
        return false;   /* the chip is busy on air; do not interrupt it */
    }
    if (s.need_reset && !s.radio_asleep) {
        /* Held in reset on purpose - a hold, a chip that would not confirm
         * standby: it cannot answer, and taking its silence for an unplug
         * would let the line go (module_leave()). mesh_radio_begin() lets it
         * go and the version read starts again; until then a module pulled
         * out is noticed only there. A chip asleep is asked all the same: the
         * driver wakes it for the question and puts it back. */
        s.misses = 0U;
        return false;
    }
    if (mesh_drv_lr2021_alive()) {
        s.misses = 0U;
        if (s.radio_asleep && !mesh_drv_lr2021_asleep()) {
            /* The question woke it and it would not go back to sleep: awake in Standby
             * RC, 1.21 mA, while the status said asleep. Put to sleep again from a
             * confirmed standby, or held in reset as when a sleep does not take. */
            s.radio_asleep = false;
            s.need_reset   = false;
            if (!radio_sleep()) {
                mesh_drv_lr2021_hold_reset();
                s.need_reset = true;
            }
            publish();
        }
        return false;
    }
    s.misses++;
    return s.misses >= MESH_BSP_GONE_MISSES;
}

/* ---- The radio's turn ------------------------------------------------------ */

/**
 * @brief Answer @p to, a request the owner held, with @p rc, @p out1 and @p why.
 *
 * The end of a send or a search can come while another request is being served (a begin, a
 * stop, the module leaving): the numbers and the reason of that answer are put back as they
 * were, so the request being served answers with its own.
 */
static void answer_other(const reply_t *to, kode_status_t rc, uint32_t out1, uint8_t why)
{
    uint32_t const was_out1 = s.out1;
    uint32_t const was_out2 = s.out2;
    uint8_t const  was_why  = s.why;

    s.out1 = out1;
    s.out2 = 0U;
    s.why  = why;
    reply_give(to, rc);
    s.out1 = was_out1;
    s.out2 = was_out2;
    s.why  = was_why;
}

/**
 * @brief A search is over: the call that waits for it is answered, and the status
 *        is the caller's to publish.
 * @param rc       KODE_OK when the chip finished it; why not otherwise.
 * @param detected What it saw.
 */
static void cad_end(kode_status_t rc, bool detected)
{
    reply_t const to = s.cad_reply;

    s.cad_running = false;
    s.cad_receive = false;
    (void)memset(&s.cad_reply, 0, sizeof(s.cad_reply));
    publish();
    if ((to.seq != 0U) || (to.done != NULL)) {
        answer_other(&to, rc, detected ? 1U : 0U, (rc == KODE_OK) ? MESH_WHY_NONE : MESH_WHY_INTERRUPTED);
    }
}

/**
 * @brief A send is over, one way or the other: whoever waits for it is answered.
 * @param rc  KODE_OK when the chip said the packet was gone; why not otherwise.
 * @param why The reason (mesh_why_t) when it is not KODE_OK.
 */
static void send_end(kode_status_t rc, uint8_t why)
{
    reply_t const to = s.send_reply;

    s.sending = false;
    (void)memset(&s.send_reply, 0, sizeof(s.send_reply));
    publish();   /* before the answer: whoever wakes reads what this changed */
    if ((to.seq != 0U) || (to.done != NULL)) {
        answer_other(&to, rc, s.last_airtime_us / 1000U,   /* the airtime, ms */
                     (rc == KODE_OK) ? (uint8_t)MESH_WHY_NONE : why);
    }
}

/* The front end's calibrations go in 4 MHz steps of a path (table 6-29): what is
 * remembered is the same number the chip is given, floor and all. */
#define CALIBRATION_STEP_HZ  4000000U
#define CALIBRATION_PATH_HF  (1UL << 31)

/** @brief What a calibration of @p hz is known by. */
static uint32_t calibration_key(uint32_t hz)
{
    return (hz / CALIBRATION_STEP_HZ) | ((hz >= MESH_BSP_2_4_GHZ_FROM_HZ) ? CALIBRATION_PATH_HF : 0UL);
}

/** @brief Whether the chip holds a calibration for @p hz. */
static bool calibrated(uint32_t hz)
{
    uint32_t const key = calibration_key(hz);

    for (uint8_t i = 0U; i < MESH_AOS_CALIBRATIONS; i++) {
        if (s.calibrated[i] == key) {
            return true;
        }
    }
    return false;
}

/** @brief The radio's activity flags, all down: nothing on air, nothing listening. */
static void flags_idle(void)
{
    s.arriving = false;
    s.cad_rx   = false;
    if (s.cad_running) {
        cad_end(KODE_DENIED, false);
    }
    s.receiving        = false;
    s.listening    = false;
    if (s.sending) {
        send_end(KODE_DENIED, MESH_WHY_INTERRUPTED);
    }
    s.testing      = false;
    s.test_left_ms = 0U;
}

/**
 * @brief Take the radio off air, and be sure of it.
 *
 * Every way out of a state in which the amplifier may be keyed - a stop, a
 * standby, the end of a test, a backstop, a fault - comes through here, and it
 * does not trust a single command: the chip is told SetStandby and asked for
 * its mode, up to STANDBY_TRIES times, because a command that fails once on a
 * busy bus is not a reason to leave a carrier up. If it still will not
 * confirm, the one thing that does not depend on the bus is left: its reset
 * line. The chip is reset, which brings it back in standby with no setting
 * (so the radio is no longer ready), and if even that gets no answer the line
 * is held low and the next mesh_radio_begin() lets it go. Either way the app is
 * told (MESH_RADIO_ERROR_NO_STANDBY).
 *
 * A chip that is already out of play - held in reset, or put to standby by a
 * fault - has nothing to confirm.
 *
 * @return true when the chip is confirmed in standby; false when it had to be
 *         reset, which clears the flags and the setting.
 */
static bool radio_off(void)
{
    s.arriving = false;   /* out of Rx, nothing is arriving any more */
    if (s.need_reset) {
        return true;
    }
    for (uint8_t i = 0U; i < STANDBY_TRIES; i++) {
        if (mesh_drv_lr2021_standby() == KODE_OK) {
            return true;
        }
    }
    KLOG_E(TAG, "the radio would not confirm standby: resetting it so that nothing stays keyed");
    uint32_t const lost = held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER);
    (void)memset(s.calibrated, 0, sizeof(s.calibrated));
    uint8_t major = 0U;
    uint8_t minor = 0U;
    s.radio_asleep = false;
    if (mesh_drv_lr2021_reset(&major, &minor) != KODE_OK) {
        mesh_drv_lr2021_hold_reset();
        s.need_reset = true;
        KLOG_E(TAG, "it did not answer a reset either: its reset line is held low");
    }
    flags_idle();
    s.radio_ready = false;
    publish();
    emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY);
    setting_lost(lost, false);
    return false;
}

/** @brief A test signal is over without anyone asking: say who ended it. */
static void test_ended(uint32_t who)
{
    s.testing      = false;
    s.test_left_ms = 0U;
    if (who == MESH_TEST_END_CHIP) {
        KLOG_I(TAG, "the test signal ended: the chip's own timer");
    } else {
        KLOG_W(TAG, "the test signal ended: the owner's deadline, the chip's timer did not");
    }
    publish();
    emit((uint8_t)MESH_AOS_EVENT_TEST_END, who);
}

/** @brief The test's deadline came: the chip's timer, if it had one, should have won. */
static void test_deadline(void)
{
    mesh_drv_lr2021_mode_t mode = MESH_DRV_LR2021_MODE_SLEEP;
    bool const on_air = (mesh_drv_lr2021_mode(&mode) != KODE_OK) || (mode == MESH_DRV_LR2021_MODE_TRANSMIT);

    if (!on_air) {
        test_ended(MESH_TEST_END_CHIP);   /* off already, and no flag told us */
        return;
    }
    (void)radio_off();                       /* on air, or no way to tell: off */
    test_ended(MESH_TEST_END_OWNER);
}

/* After a preamble the chip's word on the packet should come within these many
 * symbols more than the setting's preamble: its own, the header's eight and the
 * first of a payload, with some over. A sender's longer preamble outlasts this. */
#define ARRIVING_MORE_SYMBOLS  28U
#define ARRIVING_MARGIN_MS     50U
/* The most that "a packet is arriving" is believed with nothing more said, whatever
 * the setting's airtime: a header that came and a packet that was lost would hold
 * retune, calibrate and a plain search for the length of a 255-byte packet, which
 * is ten seconds at SF12 and BW125. */
#define ARRIVING_MAX_MS        2000U

/**
 * @brief The chip has announced a packet.
 * @param ms How long to believe it with nothing more said.
 */
static void arriving_for(uint32_t ms)
{
    uint32_t const total = ms + ARRIVING_MARGIN_MS + owed_ms();

    s.arriving         = true;
    s.arriving_left_ms = (total < ARRIVING_MAX_MS) ? total : ARRIVING_MAX_MS;
}

/** @brief A preamble, or a header, as the chip reports them, in the order they happen. */
static void radio_arrival(uint32_t f)
{
    if (s.sending || s.testing || s.cad_running) {
        return;   /* none of these is announcing a packet to listen for */
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_PREAMBLE) != 0U) {
        arriving_for(mesh_drv_lora_symbols_us(s.settings.spreading_factor, s.settings.bandwidth,
                                              (uint32_t)s.settings.preamble + ARRIVING_MORE_SYMBOLS)
                     / 1000U);
        publish();
        emit((uint8_t)MESH_AOS_EVENT_PREAMBLE, 0U);
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_HEADER_VALID) != 0U) {
        /* The longest packet there can be: the header does not say how long this
         * is until the packet is read. */
        arriving_for(mesh_drv_lora_packet_airtime_us(s.settings.spreading_factor, s.settings.bandwidth,
                                                     s.settings.coding_rate, s.settings.preamble,
                                                     (uint8_t)MESH_AOS_PAYLOAD_MAX, s.settings.low_data_rate)
                     / 1000U);
        publish();
        emit((uint8_t)MESH_AOS_EVENT_HEADER, 0U);
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR) != 0U) {
        s.arriving = false;
        publish();
        emit((uint8_t)MESH_AOS_EVENT_HEADER_BAD, 0U);
    }
}

/**
 * @brief The receiver back on after the chip left Rx for a packet or a search.
 * @return true when it is on, or was not meant to be; false when the chip would
 *         not take it: the receiver is then off, and the flag says so.
 */
static bool receiver_back(void)
{
    if (!s.receiving) {
        return true;
    }
    if (mesh_drv_lr2021_receive() == KODE_OK) {
        return true;
    }
    s.receiving = false;
    return false;
}

/** @brief The receiver would not come back and nobody is waiting on a call to be told. */
static void receiver_lost(void)
{
    KLOG_E(TAG, "the receiver would not come back on: it is off");
    publish();
    emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_RECEIVER);
}

/**
 * @brief The receive a search began is over (a packet, a bad header or CRC, or nothing
 *        that came in time): the receiver back if it was on, else the chip out of Rx.
 *        With the receiver off nothing is left listening behind the app's back.
 */
static void cad_rx_end(void)
{
    s.cad_rx = false;
    if (s.receiving) {
        if (!receiver_back()) {
            receiver_lost();
        }
    } else {
        (void)radio_off();   /* a failure resets the chip and says so itself */
    }
}

/** @brief A search ended: the receiver back if it was on, and the app told. A search that
 *         goes on to receive and saw a preamble leaves the chip in Rx for its packet. */
static void radio_cad_done(void)
{
    bool const was_running = s.cad_running;
    bool const detected    = s.cad_detected;

    if (!was_running) {
        return;   /* a search that was given up on */
    }
    if (s.cad_receive && detected) {
        /* The chip is looking for the sync word (9.4): a packet is on its way in, as
         * after a PreambleDetected, and the receive's end puts the receiver back. */
        s.cad_rx = true;
        cad_end(KODE_OK, true);
        arriving_for(mesh_drv_lora_symbols_us(s.settings.spreading_factor, s.settings.bandwidth,
                                              (uint32_t)s.settings.preamble + ARRIVING_MORE_SYMBOLS)
                     / 1000U);
        publish();
        emit((uint8_t)MESH_AOS_EVENT_PREAMBLE, 0U);
        return;
    }
    bool const back = receiver_back();   /* the chip fell back to standby */
    cad_end(KODE_OK, detected);          /* the answer: what the search saw */
    if (!back) {
        receiver_lost();
    }
}

/** @brief Read the packet the chip just received into the ring. */
static void radio_rx_done(uint32_t irq)
{
    static uint8_t buf[MESH_AOS_PAYLOAD_MAX];
    mesh_drv_lr2021_packet_t rx;

    if (mesh_drv_lr2021_read(buf, sizeof(buf), irq, &rx) != KODE_OK) {
        /* The packet is lost; whatever it left in the FIFO must not lead the next, as after a
         * CRC error with nothing announced. */
        KLOG_W(TAG, "a received packet could not be read: it is lost");
        (void)mesh_drv_lr2021_flush_receive();
        return;
    }
    ring_push(&rx, buf);
    s.packets_received++;
    /* No re-arm: continuous receive stays in Rx after a packet, and SetRx on a
     * chip that is in Rx fails and could drop the next packet. */
    publish();
    emit((uint8_t)MESH_AOS_EVENT_PACKET, rx.len);
}

/** @brief A packet left whole. */
static void radio_tx_done(void)
{
    if (s.testing) {
        test_ended(MESH_TEST_END_CHIP);   /* a preamble that finished by itself */
        return;
    }
    if (!s.sending) {
        return;   /* a done flag nobody waits for: the send was already given up on */
    }
    s.packets_sent++;
    bool const back = receiver_back();   /* back to listening after the send */
    send_end(KODE_OK, MESH_WHY_NONE);    /* the answer: the packet is gone */
    if (!back) {
        receiver_lost();
    }
}

/** @brief The chip's Rx or Tx timer ran out: whose was it? */
static void radio_timeout(void)
{
    if (s.testing) {
        test_ended(MESH_TEST_END_CHIP);   /* SetTx's timeout is how a preamble test ends */
    } else if (s.sending) {
        /* The chip's own safeguard: the packet's airtime and a quarter more went
         * by and no done flag came. The chip has stopped; the packet may not
         * have gone whole. */
        KLOG_E(TAG, "a transmit's own timer ran out without a done flag");
        bool const back = receiver_back();
        send_end(KODE_IO_ERROR, MESH_WHY_SEND_FAILED);
        emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_SEND_TIMEOUT);
        if (!back) {
            receiver_lost();
        }
    } else if (s.listening) {
        s.listening = false;
        publish();
        emit((uint8_t)MESH_AOS_EVENT_LISTEN_END, 0U);
    } else {
        /* a timer nothing was waiting on */
    }
}

/** @brief The chip raised an error or its amplifier faulted. */
static void radio_fault(uint32_t f)
{
    uint16_t const e   = mesh_drv_lr2021_errors();
    uint32_t       arg = e;
    uint32_t       lost = 0UL;

    /* The flags stay on the chip until cleared: without this the status goes on
     * saying what happened once, and the next one looks like it. */
    mesh_drv_lr2021_errors_clear();
    s.radio_errors = e;
    if ((f & MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT) != 0U) {
        /* Off air, and kept off: the amplifier's protection tripped, so nothing
         * is sent again until the app begins the radio afresh, which resets the
         * chip first. The chip would not stop by itself: it raises this flag in
         * the first 200 us and stays in Tx to the end of the packet, so what ends
         * the emission is this. */
        uint8_t const bit = (uint8_t)(1U << s.band);
        taskENTER_CRITICAL(&s_lock);
        s_antennas_suspect |= bit;   /* idempotent: a second fault changes nothing */
        s_antennas_epoch++;
        taskEXIT_CRITICAL(&s_lock);
        KLOG_E(TAG, "the amplifier reported a fault on %s: is its antenna loose? The radio is off until "
                    "mesh_radio_begin(), and that band is held to %d cdBm until the app confirms the antennas again",
               (s.band == (uint8_t)MESH_PLAN_BAND_2_4_GHZ) ? "2.4 GHz (J502)" : "sub-GHz (J501)",
               (int)MESH_REGION_SAFE_CDBM);
        lost = held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER);
        flags_idle();
        s.radio_ready = false;
        s.amplifier_fault    = true;
        (void)radio_off();
        s.need_reset  = true;
        arg |= MESH_RADIO_ERROR_AMPLIFIER_FAULT;
    }
    publish();
    emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, arg);
    setting_lost(lost, false);
}

/**
 * @brief The chip cleared its flags and their answer was lost: what they said is not known.
 *
 * On air, an amplifier fault may have been among them, and it is taken as one: the radio off
 * until it is begun again, and the band held to the safe power until the app confirms the
 * antennas. Off air no amplifier can have faulted: only what a packet left in the FIFO is
 * emptied, and the owner's own deadlines end a listen or a search whose flag went.
 */
static void radio_flags_lost(void)
{
    if (!s.sending && !s.testing) {
        KLOG_W(TAG, "the chip's interrupt flags could not be read: a packet that came with them is lost");
        (void)mesh_drv_lr2021_flush_receive();
        return;
    }
    uint32_t const lost = held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER);
    if (s.band < (uint8_t)MESH_PLAN_BAND_COUNT) {
        uint8_t const bit = (uint8_t)(1U << s.band);
        taskENTER_CRITICAL(&s_lock);
        s_antennas_suspect |= bit;
        s_antennas_epoch++;
        taskEXIT_CRITICAL(&s_lock);
    }
    KLOG_E(TAG, "the chip's interrupt flags could not be read while it was on air: taken as an amplifier "
                "fault. The radio is off until mesh_radio_begin(), and that band is held to %d cdBm until "
                "the app confirms the antennas again", (int)MESH_REGION_SAFE_CDBM);
    if (s.sending) {
        send_end(KODE_IO_ERROR, MESH_WHY_SEND_FAILED);   /* its done flag may have been among them */
    }
    flags_idle();
    s.radio_ready = false;
    (void)radio_off();
    s.need_reset  = true;
    publish();
    emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_FLAGS_LOST);
    setting_lost(lost, false);
}

static void radio_irq(uint32_t f);

/** @brief Read the chip's flags and serve them. */
static void radio_irq_take(void)
{
    uint32_t f = 0U;

    if (mesh_drv_lr2021_interrupts_read(&f) != KODE_OK) {
        radio_flags_lost();
        return;
    }
    radio_irq(f);
}

/** @brief A search's flags: what it saw, and its end. */
static void radio_cad_flags(uint32_t f)
{
    if ((f & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U) {
        s.cad_detected = true;
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U) {
        radio_cad_done();
    }
}

static void radio_irq(uint32_t f)
{
    if (s.cad_receive) {
        /* The search first: a receive it began can report its header in the same word. */
        radio_cad_flags(f);
        radio_arrival(f);
    } else {
        radio_arrival(f);
        radio_cad_flags(f);
    }
    if ((f & (MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR
              | MESH_DRV_LR2021_INTERRUPT_TIMEOUT)) != 0U) {
        s.arriving = false;
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE) != 0U) {
        radio_rx_done(f);
    } else if ((f & MESH_DRV_LR2021_INTERRUPT_CRC_ERROR) != 0U) {
        /* A bad packet with nothing announced to read: whatever the chip left in
         * its FIFO must not lead the next packet. */
        (void)mesh_drv_lr2021_flush_receive();
    } else {
        /* no packet */
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_SEND_DONE) != 0U) {
        radio_tx_done();
    }
    if ((f & MESH_DRV_LR2021_INTERRUPT_TIMEOUT) != 0U) {
        radio_timeout();
    }
    if (s.cad_rx && ((f & (MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR
                            | MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR | MESH_DRV_LR2021_INTERRUPT_TIMEOUT)) != 0U)) {
        /* A receive a search began is a single one: the chip falls back after it, and
         * the receiver goes back on if it was. */
        cad_rx_end();
    }
    if ((f & (MESH_DRV_LR2021_INTERRUPT_ERROR | MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR | MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT)) != 0U) {
        radio_fault(f);
    }
}

/**
 * @brief Time that has passed since the last tick, to be added to a countdown
 *        the moment it starts.
 *
 * radio_tick() takes dt off every running countdown, and dt is counted from
 * the previous tick - which can be earlier than the request that started the
 * countdown, by a tick and by however long that request took (a radio call that
 * waits on the chip). Without this, the first tick charges a countdown for time
 * it was not running yet, and a send's backstop or a test's deadline would run
 * out early. With it, a countdown that starts now is charged exactly from now.
 */
static uint32_t owed_ms(void)
{
    return kode_sys_time_ms() - s_tick_at;
}

static void radio_tick(uint32_t dt)
{
    if (!s.radio_ready) {
        return;
    }
    if (s.sending) {
        s.send_left_ms = (dt < s.send_left_ms) ? (s.send_left_ms - dt) : 0U;
    }
    if (s.listening) {
        s.listen_left_ms = (dt < s.listen_left_ms) ? (s.listen_left_ms - dt) : 0U;
    }
    if (s.testing) {
        s.test_left_ms = (dt < s.test_left_ms) ? (s.test_left_ms - dt) : 0U;
    }
    if (s.arriving) {
        s.arriving_left_ms = (dt < s.arriving_left_ms) ? (s.arriving_left_ms - dt) : 0U;
    }
    if (s.cad_running) {
        s.cad_left_ms = (dt < s.cad_left_ms) ? (s.cad_left_ms - dt) : 0U;
    }

    if (mesh_drv_lr2021_dio9()) {
        radio_irq_take();
    }

    /* The receiver's RSSI, sampled while it listens and nothing else is on the chip: what
     * mesh_radio_rssi() reads, so that reading it never has to wait for the owner. */
    if (!s.receiving) {
        s.rssi_valid = false;
        s.rssi_ms    = 0U;
    } else if (!s.sending && !s.cad_running && !s.testing && !s.listening && !s.arriving) {
        s.rssi_ms += dt;
        if (s.rssi_ms >= MESH_BSP_RSSI_MS) {
            float dbm = 0.0f;
            s.rssi_ms = 0U;
            if (mesh_drv_lr2021_rssi(&dbm) == KODE_OK) {
                s.last_rssi  = dbm;
                s.rssi_valid = true;
            }
        }
    } else {
        /* the chip is busy: the last sample stands */
    }

    /* Backstops, in case a flag was missed: a send cannot stay "on air", a
     * listen "listening" nor a test "on" past its own time plus a margin. */
    if (s.sending && (s.send_left_ms == 0U)) {
        KLOG_W(TAG, "a transmit did not report done in time: taking the radio off air");
        bool const back = !radio_off() || receiver_back();
        if (s.sending) {   /* radio_off() may have reset the chip, which ended it already */
            send_end(KODE_IO_ERROR, MESH_WHY_SEND_FAILED);
        }
        emit((uint8_t)MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_SEND_TIMEOUT);
        if (!back) {
            receiver_lost();
        }
    }
    if (s.testing && (s.test_left_ms == 0U)) {
        test_deadline();
    }
    if (s.listening && (s.listen_left_ms == 0U)) {
        s.listening = false;
        publish();
        emit((uint8_t)MESH_AOS_EVENT_LISTEN_END, 0U);
    }
    if (s.arriving && (s.arriving_left_ms == 0U)) {
        s.arriving = false;   /* nothing came of it */
        if (s.cad_rx) {
            /* A search saw a preamble and no packet followed: the chip has no timer of
             * its own on that Rx, so it is the owner that ends it. */
            cad_rx_end();
        }
        publish();
    }
    if (s.cad_running && (s.cad_left_ms == 0U)) {
        KLOG_W(TAG, "a channel search did not report done in time");
        bool const back = !radio_off() || receiver_back();
        if (s.cad_running) {
            cad_end(KODE_TIMEOUT, false);
        }
        publish();
        if (!back) {
            receiver_lost();
        }
    }
}

/* ---- Requests -------------------------------------------------------------- */

/** @brief Whether @p r asks something onto the air: dropped after a stop. */
static bool moves(const mesh_aos_request_t *r)
{
    switch (r->kind) {
    case MESH_REQUEST_RADIO_SEND:
    case MESH_REQUEST_RADIO_TEST:
        return true;
    default:
        return false;
    }
}

/* ---- The antennas ---------------------------------------------------------- */

/**
 * @brief Whether @p band may go to the power the app asks: the antennas confirmed, and the
 *        band's amplifier not faulted since.
 * @param band       A mesh_plan_band_t.
 * @param[out] epoch The app's word this answer is from.
 */
static bool antennas_ok(uint8_t band, uint32_t *epoch)
{
    taskENTER_CRITICAL(&s_lock);
    bool const ok = s_antennas_ok && ((s_antennas_suspect & (uint8_t)(1U << band)) == 0U);
    *epoch = s_antennas_epoch;
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

/**
 * @brief The power to plan for when @p asked is asked on @p band: the app's, or the safe power
 *        at most while the antennas are not confirmed. Every power that reaches the plan comes
 *        through here: a begin, a whole setting, a retune and what restore() puts back.
 * @param[out] epoch  The app's word the answer is from, for the setting once it is in force.
 * @param[out] capped Whether the answer is below @p asked.
 */
static int32_t power_for(uint8_t band, int32_t asked, uint32_t *epoch, bool *capped)
{
    int32_t const power = mesh_plan_power_allowed(asked, antennas_ok(band, epoch));

    *capped = power < asked;
    return power;
}

/** @brief What the log last said of a band's power, so that it says it once per change. */
typedef enum {
    SAID_NOTHING = 0,  /* not said yet */
    SAID_FULL,         /* what was asked goes out */
    SAID_CAPPED,       /* held to the safe power: the antennas are not confirmed */
    SAID_SUSPECTED     /* held to the safe power: the band's amplifier faulted */
} said_t;

/**
 * @brief Say in the log what the setting in force puts out against the safe power, once per change of
 *        state and band: an app that retunes many times a second would flood it with a line each.
 */
static void power_say(void)
{
    if (!s.radio_ready || (s.band >= (uint8_t)MESH_PLAN_BAND_COUNT)) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    bool const suspected = (s_antennas_suspect & (uint8_t)(1U << s.band)) != 0U;
    taskEXIT_CRITICAL(&s_lock);
    uint8_t const state = !s.power_capped ? (uint8_t)SAID_FULL
                          : (suspected ? (uint8_t)SAID_SUSPECTED : (uint8_t)SAID_CAPPED);
    if (state == s.power_said[s.band]) {
        return;
    }
    s.power_said[s.band] = state;
    char const *const band = (s.band == (uint8_t)MESH_PLAN_BAND_2_4_GHZ) ? "2.4 GHz" : "sub-GHz";
    if (state == (uint8_t)SAID_FULL) {
        KLOG_I(TAG, "%s: the power asked goes out (%ld cdBm): the antennas are confirmed, or it is under the "
                    "safe power", band, (long)s.power_applied_cdbm);
    } else if (state == (uint8_t)SAID_SUSPECTED) {
        KLOG_W(TAG, "%s: held to the safe power, %d cdBm, since its amplifier faulted, until the app confirms the "
                    "antennas again", band, (int)MESH_REGION_SAFE_CDBM);
    } else {
        KLOG_W(TAG, "%s: held to the safe power, %d cdBm (%ld asked), until the app confirms the antennas "
                    "(mesh_radio_antennas_confirm)", band, (int)MESH_REGION_SAFE_CDBM, (long)s.power_cdbm);
    }
}

/**
 * @brief Put the app's latest word on the antennas on the setting in force: its power planned
 *        again from what was asked, and only the amplifier written (a retune of the same
 *        setting: SetPaConfig and SetTxParams, no calibration).
 *
 * The tick and a nudge call it with @p now false, and it waits while the chip is busy - a
 * packet on air or arriving, a search, a listen, a test - for a later turn. A send and a test
 * call it with @p now true from standby, just before they key the amplifier, so nothing goes
 * out at a power the app's word no longer allows.
 * @return false when the chip refused the new power: the setting is lost, and said so.
 */
static bool antennas_apply(bool now)
{
    uint32_t epoch  = 0U;
    bool     capped = false;

    if (!s.radio_ready) {
        return true;   /* the next setting is planned with it */
    }
    int32_t const power = power_for(s.band, s.power_cdbm, &epoch, &capped);
    if (epoch == s.antennas_epoch) {
        return true;
    }
    if (!now && (s.sending || s.testing || s.cad_running || s.listening || s.arriving)) {
        return true;   /* at a later turn, or before the next transmit */
    }
    mesh_drv_lora_settings_t cfg = s.settings;
    (void)mesh_plan_power((mesh_plan_band_t)s.band, power, &cfg);
    if ((cfg.tx_power != s.settings.tx_power) || (cfg.amplifier_duty != s.settings.amplifier_duty)
        || (cfg.amplifier_slices != s.settings.amplifier_slices)) {
        kode_status_t const rc = mesh_drv_lr2021_retune(&cfg);
        if (rc != KODE_OK) {
            /* What the chip holds now is not known: no setting until a begin. */
            uint32_t const lost = held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER);
            s.radio_ready = false;
            flags_idle();
            (void)radio_off();
            setting_lost(lost, false);
            return false;
        }
        s.settings           = cfg;
        s.power_applied_cdbm = mesh_plan_power_applied((mesh_plan_band_t)s.band, &cfg);
        if (!now && !receiver_back()) {
            receiver_lost();
        }
    }
    s.antennas_epoch = epoch;
    s.power_capped   = capped;
    power_say();
    publish();
    return true;
}

kode_status_t mesh_aos_antennas_confirm(bool fitted)
{
    /* s_started is looked at under the lock that do_end() clears it under, with the word: a
     * confirm that meets an end is either before it, and forgotten with the rest, or after it,
     * and refused - never kept past it. */
    taskENTER_CRITICAL(&s_lock);
    bool const started = s_started;
    if (started) {
        s_antennas_ok      = fitted;
        s_antennas_suspect = 0U;   /* either answer is the app having asked */
        s_antennas_epoch++;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!started) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    mesh_ao_nudge();   /* the owner puts the new power on a setting in force at its next turn */
    mesh_aos_why_note(MESH_WHY_NONE);
    return KODE_OK;
}

/**
 * @brief Put a setting the plan has allowed in force: the chip reset if it has to
 *        be, configured and calibrated.
 * @param cfg        The setting.
 * @param power_cdbm The power that was asked, for the status.
 * @param configured It came through mesh_radio_configure().
 */
static kode_status_t radio_apply(const mesh_drv_lora_settings_t *cfg, int32_t power_cdbm, bool configured)
{
    mesh_plan_band_t const band = cfg->band_2_4_ghz ? MESH_PLAN_BAND_2_4_GHZ : MESH_PLAN_BAND_SUB_GHZ;

    /* From here until the new setting is in, there is no setting - and nothing
     * is on air or listening. What the app last set up is not that any more
     * either: the caller records the new one when it is in. */
    s.radio_ready = false;
    flags_idle();
    want_drop(MESH_SETTING_RADIO);

    if (s.need_reset) {
        /* The chip was taken out of play (it would not confirm standby, or its
         * amplifier faulted): bring it back before anything is written to it. */
        uint8_t major = 0U;
        uint8_t minor = 0U;
        kode_status_t const rst = mesh_drv_lr2021_reset(&major, &minor);
        s.radio_asleep = false;   /* the pulse woke it, if it slept */
        if (rst != KODE_OK) {
            return rst;
        }
        s.need_reset = false;
        (void)memset(s.calibrated, 0, sizeof(s.calibrated));
    }
    if (!radio_off()) {
        return KODE_IO_ERROR;
    }
    kode_status_t rc = mesh_drv_lr2021_configure(cfg);
    if (rc != KODE_OK) {
        return rc;
    }
    rc = mesh_drv_lr2021_calibrate();
    if (rc != KODE_OK) {
        return rc;
    }
    /* A plain calibration is the chip's first slot (6.4.2). The first slot goes, and a
     * repeat of it in the list never had a slot of its own: the chip skips the second
     * of two frequencies in one 4 MHz step. */
    for (size_t i = 1U; i < MESH_AOS_CALIBRATIONS; i++) {
        if (s.calibrated[i] == s.calibrated[0]) {
            s.calibrated[i] = 0U;
        }
    }
    s.calibrated[0]     = calibration_key(cfg->frequency_hz);
    s.settings          = *cfg;
    s.band        = (uint8_t)band;
    s.spreading_factor          = cfg->spreading_factor;
    s.power_cdbm        = power_cdbm;
    s.power_applied_cdbm = mesh_plan_power_applied(band, cfg);
    s.duty_counted = mesh_plan_duty_counted(band, configured);
    s.radio_ready = true;
    s.amplifier_fault    = false;
    s.last_airtime_us = 0U;   /* the pending-wait figure resets with the setting */
    KLOG_I(TAG, "radio set: %lu Hz, SF%u, %ld cdBm asked, tx_power %d, row %u/%u",
           (unsigned long)cfg->frequency_hz, (unsigned)cfg->spreading_factor, (long)power_cdbm,
           (int)cfg->tx_power, (unsigned)cfg->amplifier_duty, (unsigned)cfg->amplifier_slices);
    return KODE_OK;
}

/** @brief Refuse with @p why, which the caller can ask for (mesh_error()). */
static kode_status_t deny(uint8_t why)
{
    s.why = why;
    return KODE_DENIED;
}

/** @brief Why the radio cannot start something now: the first flag that is up. */
static uint8_t why_busy(void)
{
    if (!s.radio_ready) {
        return MESH_WHY_NOT_BEGUN;
    }
    if (s.testing) {
        return MESH_WHY_BUSY_TEST;
    }
    if (s.sending) {
        return MESH_WHY_BUSY_SENDING;
    }
    if (s.cad_running) {
        return MESH_WHY_BUSY_SEARCH;
    }
    if (s.listening) {
        return MESH_WHY_BUSY_LISTENING;
    }
    return MESH_WHY_BUSY_ARRIVING;
}

static kode_status_t do_radio_begin(const mesh_aos_request_t *r)
{
    if (r->a >= MESH_PLAN_BAND_COUNT) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    mesh_drv_lora_settings_t cfg;
    uint32_t                 epoch  = 0U;
    bool                     capped = false;
    int32_t const            power  = power_for(r->a, r->i, &epoch, &capped);
    if (!mesh_plan_transmit_settings((mesh_plan_band_t)r->a, (uint8_t)r->u, power, &cfg)) {
        return deny(MESH_WHY_NOT_ALLOWED);
    }
    kode_status_t const rc = radio_apply(&cfg, r->i, false);
    if (rc == KODE_OK) {
        s.antennas_epoch  = epoch;
        s.power_capped    = capped;
        power_say();
        s.want.radio      = true;
        s.want.configured = false;
        s.want.band       = r->a;
        s.want.sf         = (uint8_t)r->u;
        s.want.power_cdbm = r->i;
    }
    return rc;
}

/** @brief MESH_REQUEST_RADIO_CALIBRATE. */
static kode_status_t do_radio_calibrate(const mesh_aos_request_t *r)
{
    if (!s.radio_ready || s.testing || s.sending || s.cad_running || s.listening || s.arriving) {
        return deny(why_busy());
    }
    if ((r->u == 0U) || (r->u > MESH_AOS_CALIBRATIONS)) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    bool covers = false;   /* the list holds the setting in force */
    for (uint32_t i = 0U; i < r->u; i++) {
        if (!mesh_plan_frequency_allowed(mesh_plan_band_of(r->hz[i]), r->hz[i])) {
            return deny(MESH_WHY_NOT_ALLOWED);
        }
        if (calibration_key(r->hz[i]) == calibration_key(s.settings.frequency_hz)) {
            covers = true;
        }
    }
    if (!covers) {
        /* The call replaces what the chip holds, and the receiver would go on
         * listening on a frequency the SDK no longer counts as calibrated. */
        return deny(MESH_WHY_NOT_CALIBRATED);
    }
    /* CalibFE fails in Rx (6.4.2), and it overwrites the slots it is given. */
    if (!radio_off()) {
        return KODE_IO_ERROR;
    }
    kode_status_t rc = mesh_drv_lr2021_calibrate_at(r->hz, (uint8_t)r->u);
    (void)memset(s.calibrated, 0, sizeof(s.calibrated));
    if (rc == KODE_OK) {
        for (uint32_t i = 0U; i < r->u; i++) {
            s.calibrated[i] = calibration_key(r->hz[i]);
        }
    }
    if (!receiver_back() && (rc == KODE_OK)) {
        rc = KODE_IO_ERROR;   /* calibrated, but the receiver is off: the caller must know */
    }
    return rc;
}

/** @brief MESH_REQUEST_RADIO_RETUNE: no calibration and no reset, and the receiver goes
 *         on if it was on. */
static kode_status_t do_radio_retune(const mesh_aos_request_t *r)
{
    mesh_drv_lora_settings_t cfg;
    mesh_drv_lora_config_t   ask    = r->config;
    uint32_t                 epoch  = 0U;
    bool                     capped = false;

    /* A packet on air or on its way in is left to end: the caller tries again. */
    if (!s.radio_ready || s.testing || s.sending || s.cad_running || s.listening || s.arriving) {
        return deny(why_busy());
    }
    ask.power_cdbm = power_for((uint8_t)mesh_plan_band_of(ask.frequency_hz), ask.power_cdbm, &epoch, &capped);
    if (!mesh_plan_radio_settings(&ask, &cfg)) {
        return deny(MESH_WHY_NOT_ALLOWED);
    }
    if (!calibrated(cfg.frequency_hz)) {
        return deny(MESH_WHY_NOT_CALIBRATED);
    }
    kode_status_t rc = mesh_drv_lr2021_retune(&cfg);
    if (rc != KODE_OK) {
        /* What the chip holds now is not known: no setting until a begin. */
        uint32_t const lost = held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER);
        s.radio_ready = false;
        flags_idle();
        (void)radio_off();
        setting_lost(lost, false);
        return rc;
    }
    mesh_plan_band_t const band = cfg.band_2_4_ghz ? MESH_PLAN_BAND_2_4_GHZ : MESH_PLAN_BAND_SUB_GHZ;

    s.settings         = cfg;
    s.band             = (uint8_t)band;
    s.spreading_factor = cfg.spreading_factor;
    s.power_cdbm       = r->config.power_cdbm;
    s.power_applied_cdbm = mesh_plan_power_applied(band, &cfg);
    s.antennas_epoch   = epoch;
    s.power_capped     = capped;
    power_say();
    s.duty_counted     = mesh_plan_duty_counted(band, true);
    s.last_airtime_us  = 0U;
    s.want.radio       = true;     /* the setting in force is what to put back */
    s.want.configured  = true;
    s.want.config      = r->config;
    if (!receiver_back()) {
        /* The new setting is in force and the receiver is off; the status says so. */
        rc = KODE_IO_ERROR;
    }
    return rc;
}

static kode_status_t do_radio_configure(const mesh_aos_request_t *r)
{
    mesh_drv_lora_settings_t cfg;
    mesh_drv_lora_config_t   ask    = r->config;
    uint32_t                 epoch  = 0U;
    bool                     capped = false;

    /* A refused setting changes nothing: the one in force stays, on air or not. */
    ask.power_cdbm = power_for((uint8_t)mesh_plan_band_of(ask.frequency_hz), ask.power_cdbm, &epoch, &capped);
    if (!mesh_plan_radio_settings(&ask, &cfg)) {
        return deny(MESH_WHY_NOT_ALLOWED);
    }
    kode_status_t const rc = radio_apply(&cfg, r->config.power_cdbm, true);
    if (rc == KODE_OK) {
        s.antennas_epoch  = epoch;
        s.power_capped    = capped;
        power_say();
        s.want.radio      = true;
        s.want.configured = true;
        s.want.config     = r->config;
    }
    return rc;
}

/* do_send() and radio_rx_done() let the length's own type bound it: a uint8_t
 * cannot exceed the 255-byte LoRa maximum, so there is no upper-length guard.
 * These fail the build the day the cap is set to something else, or the length
 * is widened, where a guard (and a check on the RX buffer) would be needed. */
_Static_assert(MESH_AOS_PAYLOAD_MAX == UINT8_MAX,
               "a payload cap other than 255 needs a guard in do_send() and radio_rx_done()");
_Static_assert(sizeof(((mesh_aos_request_t *)0)->len) == 1U,
               "the send length is a uint8_t; widening it needs an upper-length guard in do_send()");

static kode_status_t do_send(const mesh_aos_request_t *r)
{
    if (!s.radio_ready || s.testing || s.sending || s.cad_running) {
        return deny(why_busy());   /* a packet already on air is a busy answer */
    }
    if ((r->data == NULL) || (r->len == 0U)) {   /* upper end bounded by the type */
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    /* A packet's own preamble is for this packet alone. */
    if ((r->preamble != 0U) && !mesh_plan_preamble_allowed(r->preamble)) {
        return deny(MESH_WHY_NOT_ALLOWED);
    }
    uint16_t const preamble = (r->preamble != 0U) ? r->preamble : s.settings.preamble;
    uint32_t const air = mesh_drv_lora_packet_airtime_us(s.settings.spreading_factor, s.settings.bandwidth,
                                                         s.settings.coding_rate, preamble, r->len,
                                                         s.settings.low_data_rate);
    uint32_t const now = kode_sys_time_ms();
    s.last_airtime_us = air;   /* for mesh_radio_send_wait_ms(), whether this goes or waits */
    if (s.duty_counted && !mesh_plan_duty_allows(&s.duty, air, now)) {
        s.out1 = mesh_plan_duty_wait_ms(&s.duty, air);   /* ms until there is room */
        s.why  = MESH_WHY_DUTY;
        return KODE_NO_RESOURCES;
    }
    /* A transmit starts from standby, whatever the chip was doing - receiving,
     * or in a timed listen - and from nothing less: if standby cannot be
     * confirmed the chip is reset and nothing is sent. */
    if (!radio_off()) {
        return KODE_IO_ERROR;
    }
    s.listening = false;
    /* The app's latest word on the antennas, before the amplifier is keyed. */
    if (!antennas_apply(true)) {
        return KODE_IO_ERROR;
    }
    kode_status_t const rc = (r->preamble != 0U) ? mesh_drv_lr2021_send_preamble(r->data, r->len, preamble)
                                                  : mesh_drv_lr2021_send(r->data, r->len);
    if (rc != KODE_OK) {
        /* Nothing is meant to be on air - but a SetTx whose status never came
         * back may have started, so make sure, and put the receiver back. */
        if (radio_off()) {
            (void)receiver_back();   /* the caller is told rc; the receiver's state is the flag's */
        }
        return rc;
    }
    if (s.duty_counted) {
        mesh_plan_duty_charge(&s.duty, air, now);
    }
    s.sending    = true;
    /* The chip's own timer is the airtime and a quarter more; the backstop is
     * that and a margin, so the chip's word comes first. */
    s.send_left_ms = (air / 1000U) + (air / 4000U) + TX_MARGIN_MS + owed_ms();
    s.out1       = air / 1000U;   /* the airtime, ms: what a command's nobody hears */
    /* The answer to this request is the packet gone (send_end()): held. */
    if (r->answer || (r->done != NULL)) {
        s.send_reply.seq  = r->answer ? r->seq : 0U;
        s.send_reply.ask  = r->ask;
        s.send_reply.done = r->done;
        s.send_reply.held = r->answer;
        s.answer_held     = true;
    }
    return KODE_OK;
}

static kode_status_t do_gnss_begin(void)
{
    if (s.gnss_on) {
        s.want.gnss = true;
        return KODE_OK;
    }
    if (s.gnss_asleep) {
        /* Asleep: a byte on its RXD wakes it, with no reset, so it comes back from a
         * hot start (its backup RAM was powered all along). */
        kode_status_t const rc = mesh_drv_gnss_wake();
        if (rc == KODE_OK) {
            s.gnss_on     = true;
            s.gnss_asleep = false;
            s.want.gnss   = true;
            s.fix_reports = 0U;
            (void)memset(&s.gnss, 0, sizeof(s.gnss));
            KLOG_I(TAG, "GNSS on: woken from its sleep");
        } else {
            /* The bytes did not go out: closed, as one that cannot be told, so that
             * nothing reads its silence later as an ignored Sleep command. The next
             * begin opens it afresh, with its reset pulse. */
            mesh_drv_gnss_close();
            s.gnss_asleep = false;
            KLOG_W(TAG, "the GNSS could not be woken: closed");
        }
        return rc;
    }
    const mesh_drv_gnss_pins_t gp = {
        .uart_port = s.uart_port,
        .rxd   = s.gpio[MESH_BSP_POSITION_GNSS_TXD],   /* the receiver's TXD; the P4 listens */
        .txd   = s.gpio[MESH_BSP_POSITION_GNSS_RXD],   /* the receiver's RXD; the P4 speaks  */
        .reset = s.gpio[MESH_BSP_POSITION_GNSS_RESET],
        .pps   = s.gpio[MESH_BSP_POSITION_PPS],
    };
    kode_status_t const rc = mesh_drv_gnss_open(&gp);
    if (rc == KODE_OK) {
        s.gnss_on     = true;
        s.want.gnss   = true;
        s.fix_reports = 0U;
        KLOG_I(TAG, "GNSS on: listening for NMEA");
    }
    return rc;
}

/**
 * @brief The GNSS is no longer read: put it to sleep with its Sleep command, or, when it
 *        cannot be told (one at 9600 baud, one that never talked), close it.
 *
 * Asleep, the driver stays open and the owner goes on polling it, to see that it stays
 * silent; mesh_gnss_begin() wakes it. Its fix is forgotten either way.
 */
static void gnss_quiet(void)
{
    if (!s.gnss_on) {
        return;
    }
    if (mesh_drv_gnss_sleep() == KODE_OK) {
        s.gnss_asleep = true;
        KLOG_I(TAG, "GNSS asleep: told to sleep, a byte on its UART wakes it");
    } else {
        mesh_drv_gnss_close();
        KLOG_I(TAG, "GNSS closed: it has not talked at %u baud, so it cannot be told to sleep",
               (unsigned)MESH_BSP_GNSS_BAUD);
    }
    s.gnss_on = false;
    (void)memset(&s.gnss, 0, sizeof(s.gnss));
}

/**
 * @brief MESH_REQUEST_RADIO_CAD: start a search of the channel, whose end comes on
 *        the radio's interrupt.
 *
 * Nothing waits here. The one that asked holds its place - a task in
 * mesh_aos_call(), an Active Object with its door - while this task goes on with
 * its tick, and cad_end() answers it, whichever way the search ends. With nobody
 * to answer (a command) the search still runs and its result is nowhere.
 */
static kode_status_t do_cad(const mesh_aos_request_t *r)
{
    if (!s.radio_ready || s.testing || s.sending || s.listening || s.cad_running) {
        return deny(why_busy());
    }
    if (r->u > 4U) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    if (s.arriving) {
        /* A packet is on its way in: the channel is busy, and a search would take the
         * receiver out of Rx and cut the packet. Answered at once, its reception left alone. */
        s.out1 = 1U;
        s.out2 = 0U;
        return KODE_OK;
    }
    uint8_t const symbols = (r->u != 0U) ? (uint8_t)r->u : mesh_drv_lora_cad_symbols(s.settings.spreading_factor);
    bool const fast    = (r->a & MESH_AOS_CAD_FAST) != 0U;
    bool const receive = (r->a & MESH_AOS_CAD_RECEIVE) != 0U;
    kode_status_t const rc = receive ? mesh_drv_lr2021_cad_receive(symbols, fast)
                                     : mesh_drv_lr2021_cad(symbols, fast);
    if (rc != KODE_OK) {
        if (radio_off()) {
            (void)receiver_back();
        }
        return rc;
    }
    s.arriving     = false;   /* the chip is out of Rx for the search */
    s.cad_rx       = false;
    s.cad_running  = true;
    s.cad_receive  = receive;
    s.cad_detected = false;
    s.cad_left_ms  = (mesh_drv_lora_cad_us(s.settings.spreading_factor, s.settings.bandwidth, symbols) / 1000U)
                     + TX_MARGIN_MS + owed_ms();
    if (r->answer || (r->done != NULL)) {
        s.cad_reply.seq  = r->answer ? r->seq : 0U;
        s.cad_reply.ask  = r->ask;
        s.cad_reply.done = r->done;
        s.cad_reply.held = r->answer;
        s.answer_held    = true;
    }
    return KODE_OK;
}

/** @brief The receiver on or off, as asked. */
static kode_status_t receive_now(bool on)
{
    if (!s.radio_ready) {
        return deny(MESH_WHY_NOT_BEGUN);
    }
    if (on) {
        if (s.testing) {
            return deny(MESH_WHY_BUSY_TEST);
        }
        if (s.cad_running) {
            /* The chip is searching: the receiver comes on when the search ends. */
            s.receiving = true;
            return KODE_OK;
        }
        if (s.sending) {
            /* The chip is on air and a receive would abort it: the receiver
             * comes on when the packet is gone (the done flag arms it). */
            s.receiving = true;
            return KODE_OK;
        }
        if (s.listening) {
            /* A timed listen is running, and it would end this one with its
             * timer: end it first. */
            if (!radio_off()) {
                return KODE_IO_ERROR;
            }
            s.listening = false;
        }
        kode_status_t const rc = mesh_drv_lr2021_receive();
        if (rc == KODE_OK) {
            s.receiving = true;
        }
        return rc;
    }
    if (s.sending) {
        s.receiving = false;   /* turning the receiver off does not cut a packet short */
        return KODE_OK;
    }
    /* Off is off: the standby below ends a listen, a search and a test signal as
     * well as a receive, and the flags say so. */
    if (s.cad_running) {
        cad_end(KODE_DENIED, false);
    }
    s.arriving         = false;
    s.receiving        = false;
    s.listening    = false;
    s.testing      = false;
    s.test_left_ms = 0U;
    return radio_off() ? KODE_OK : KODE_IO_ERROR;
}

/** @brief MESH_REQUEST_RADIO_RECEIVE: and what is set up is the receiver on, or off. */
static kode_status_t do_receive(bool on)
{
    kode_status_t const rc = receive_now(on);

    if (rc == KODE_OK) {
        s.want.receive = on;
    }
    return rc;
}

/** @brief MESH_REQUEST_RADIO_LISTEN. */
static kode_status_t do_listen(const mesh_aos_request_t *r)
{
    if (!s.radio_ready || s.testing || s.sending || s.cad_running) {
        return deny(why_busy());
    }
    if ((r->u == 0U) || (r->u > MESH_LISTEN_MS_MAX)) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    kode_status_t const rc = mesh_drv_lr2021_listen_for(r->u);
    if (rc != KODE_OK) {
        /* The listen began with a standby and may not have got further: the
         * receiver is not on, whatever it was. */
        s.receiving     = false;
        s.listening = false;
        (void)radio_off();
        return rc;
    }
    s.listening      = true;
    s.receiving          = false;
    s.listen_left_ms = r->u + TX_MARGIN_MS + owed_ms();
    return KODE_OK;
}

/** @brief MESH_REQUEST_RADIO_TEST. */
static kode_status_t do_tx_test(const mesh_aos_request_t *r)
{
    /* A test signal keys the amplifier for as long as it is asked and is not counted in the
     * duty budget: it is a lab build's (CONFIG_MESH_LAB), and a build that keeps the region's
     * limits refuses it before anything else. Taking one off air (0) is never refused. */
    if ((r->u != 0U) && !mesh_plan_test_allowed()) {
        return deny(MESH_WHY_LAB_ONLY);
    }
    if (!s.radio_ready) {
        return deny(MESH_WHY_NOT_BEGUN);
    }
    if (r->u == 0U) {
        flags_idle();
        return radio_off() ? KODE_OK : KODE_IO_ERROR;
    }
    if (s.testing || s.sending || s.cad_running) {
        return deny(why_busy());   /* one thing on air at a time */
    }
    /* A test signal is never open-ended: it has a time, and the owner holds it
     * to it. A request without one, or past the longest, is not made. */
    if ((r->i <= 0) || ((uint32_t)r->i > MESH_BSP_TEST_MAX_MS)) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    if ((r->u != (uint32_t)MESH_DRV_LR2021_TEST_CARRIER) && (r->u != (uint32_t)MESH_DRV_LR2021_TEST_PREAMBLE)) {
        return deny(MESH_WHY_BAD_ARGUMENT);
    }
    /* A bare carrier is the one thing the chip cannot bound by itself, and a
     * restart (kodeOS's way home is one) must not leave it on air: without the
     * driver's restart hook there is nothing behind the owner's deadline. */
    if ((r->u == (uint32_t)MESH_DRV_LR2021_TEST_CARRIER) && !mesh_drv_lr2021_guarded()) {
        KLOG_E(TAG, "no carrier test: nothing would hold the radio in reset on a restart");
        return deny(MESH_WHY_NO_GUARD);
    }
    /* The app's latest word on the antennas, before the amplifier is keyed. */
    if (!antennas_apply(true)) {
        return KODE_IO_ERROR;
    }
    uint32_t const ms = (uint32_t)r->i;
    kode_status_t const rc = mesh_drv_lr2021_test((mesh_drv_lr2021_test_t)r->u, ms);
    if (rc != KODE_OK) {
        (void)radio_off();   /* it may be part-way up */
        return rc;
    }
    s.testing      = true;
    s.receiving        = false;
    s.listening    = false;
    s.test_left_ms = ms + MESH_BSP_TEST_GRACE_MS + owed_ms();
    return KODE_OK;
}

/**
 * @brief Ask AO_Radio to switch J3's supply to @p on, and take the answer from EXP_POWER_SIG.
 *
 * The state at boot is not assumed: the supply is "unknown" until the signal says. A command
 * that finds no room is asked again at the next tick; one that is denied is an image with no
 * radio (the C5 is in reset and J3 is always powered), or one that cannot be asked.
 */
static void power_ask(bool on)
{
    s.power_wish = on;
    kode_status_t const rc = kode_sys_exp_power(on);

    if (rc == KODE_OK) {
        s.power_retry = false;
        s.supply      = (uint8_t)MESH_SUPPLY_UNKNOWN;   /* until EXP_POWER_SIG says */
    } else if (rc == KODE_NO_RESOURCES) {
        s.power_retry = true;
    } else {
#if CONFIG_KODEOS_RADIO
        s.power_retry = false;
        s.supply      = (uint8_t)MESH_SUPPLY_FAILED;
        KLOG_E(TAG, "J3's supply could not be switched %s: kode_sys_exp_power() was denied", on ? "on" : "off");
#else
        s.power_retry = false;
        s.supply      = (uint8_t)MESH_SUPPLY_ALWAYS_ON;
        KLOG_I(TAG, "this image has no radio: J3 is always powered and nothing can switch it");
#endif
    }
}

/**
 * @brief MESH_REQUEST_END: the app is done with the module. What is on air is taken off,
 *        the GNSS closed, the lines handed back and J3's supply asked off; nothing the app
 *        set up is remembered, and nothing is looked for until mesh_aos_start().
 */
static kode_status_t do_end(void)
{
    bool const had = s.present;

    flags_idle();
    gnss_quiet();   /* the datasheet's order (p.6): the Sleep command, then VCC may go */
    module_leave(false);
    want_drop(MESH_SETTING_RADIO | MESH_SETTING_RECEIVER | MESH_SETTING_GNSS);
    s.rest_lost = 0UL;
    taskENTER_CRITICAL(&s_lock);
    s_forget = 0UL;
    s_stop_epoch++;   /* what was asked to go on air before is dropped */
    (void)memset(s.power_said, 0, sizeof(s.power_said));
    s_antennas_ok      = false;   /* the app's word on the antennas goes with the rest */
    s_antennas_suspect = 0U;
    s_antennas_epoch++;
    s_started = false;            /* with the word: see mesh_aos_antennas_confirm() */
    taskEXIT_CRITICAL(&s_lock);
    s.seen          = MESH_MODULE_NONE;
    s.seen_level    = 0U;
    s.core_busy_said = false;
    s_ended   = true;
    KLOG_I(TAG, "the app let go of the module: the lines are handed back and J3's supply is asked off");
    power_ask(false);
    if (had) {
        publish();
        emit((uint8_t)MESH_AOS_EVENT_GONE, 0U);   /* the app hears its own end, as it hears a pull */
    }
    return KODE_OK;
}

/**
 * @brief The app ended something while the module is away: the request is refused, but what
 *        the owner would put back when the module returns must not include it.
 */
static void absent_forget(const mesh_aos_request_t *r)
{
    switch (r->kind) {
    case MESH_REQUEST_RADIO_END:
        want_drop(MESH_SETTING_RADIO);
        break;
    case MESH_REQUEST_RADIO_STANDBY:
        want_drop(MESH_SETTING_RECEIVER);
        break;
    case MESH_REQUEST_RADIO_RECEIVE:
        if (r->u == 0U) {
            want_drop(MESH_SETTING_RECEIVER);
        }
        break;
    case MESH_REQUEST_GNSS_END:
        want_drop(MESH_SETTING_GNSS);
        break;
    default:
        break;
    }
}

/** @brief Apply one request. */
static kode_status_t handle(const mesh_aos_request_t *r)
{
    if (r->kind == MESH_REQUEST_END) {
        return do_end();
    }
    if (!s.present) {
        absent_forget(r);
        return deny(MESH_WHY_MODULE_ABSENT);
    }
    switch (r->kind) {
    case MESH_REQUEST_RADIO_BEGIN:
        return do_radio_begin(r);

    case MESH_REQUEST_RADIO_CONFIGURE:
        return do_radio_configure(r);

    case MESH_REQUEST_RADIO_CALIBRATE:
        return do_radio_calibrate(r);

    case MESH_REQUEST_RADIO_RETUNE:
        return do_radio_retune(r);

    case MESH_REQUEST_RADIO_END:
        want_drop(MESH_SETTING_RADIO);
        if (s.radio_ready) {
            flags_idle();
            (void)radio_off();
            s.radio_ready = false;
        }
        (void)radio_sleep();   /* no setting: nothing for it to do awake */
        return KODE_OK;

    case MESH_REQUEST_RADIO_STANDBY:
        flags_idle();
        want_drop(MESH_SETTING_RECEIVER);
        if (!s.radio_ready) {
            return KODE_OK;   /* no setting: the chip is not in use, and it is quiet */
        }
        return radio_off() ? KODE_OK : KODE_IO_ERROR;

    case MESH_REQUEST_RADIO_SEND:
        return do_send(r);

    case MESH_REQUEST_RADIO_RECEIVE:
        return do_receive(r->u != 0U);

    case MESH_REQUEST_RADIO_LISTEN:
        return do_listen(r);

    case MESH_REQUEST_RADIO_CAD:
        return do_cad(r);

    case MESH_REQUEST_RADIO_TEST:
        return do_tx_test(r);

    case MESH_REQUEST_GNSS_BEGIN:
        return do_gnss_begin();

    case MESH_REQUEST_GNSS_END:
        gnss_quiet();
        want_drop(MESH_SETTING_GNSS);
        return KODE_OK;

    case MESH_REQUEST_GNSS_RESET:
        if (s.gnss_on) {
            mesh_drv_gnss_restart();
        }
        return KODE_OK;

    case MESH_REQUEST_I2C_LEND:
        /* The SDK never masters the pair (MESH_BSP_GNSS_I2C_RISK): taking it is
         * only handing the app the GPIOs it already holds, high-Z. */
        s.i2c_taken = true;
        s.out1    = s.scl;
        s.out2    = s.sda;
        return KODE_OK;

    case MESH_REQUEST_I2C_RETURN:
        s.i2c_taken = false;
        return KODE_OK;

    case MESH_REQUEST_RADIO_HOLD_RESET:
        mesh_drv_lr2021_hold_reset();
        s.radio_asleep = false;
        flags_idle();
        s.radio_ready = false;
        s.need_reset  = true;   /* mesh_radio_begin() lets the line go before it writes */
        return KODE_OK;

    default:
        return deny(MESH_WHY_UNKNOWN);
    }
}

/** @brief A mesh_aos_stop() not applied yet: off air, now. */
static void stops_apply(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t const forget = s_forget;
    s_forget = 0UL;
    taskEXIT_CRITICAL(&s_lock);
    want_drop(forget);   /* what the app ended while this task could not hear it */

    uint32_t const e = s_stop_epoch;

    if (e == s.epoch) {
        return;
    }
    s.epoch = e;
    if (s.present && s.radio_open) {
        flags_idle();
        (void)radio_off();
    }
}

/**
 * @brief The module came back, or the board woke: put on it what the app had set up when it
 *        left - the setting, the receiver, the GNSS - and say so. Never a send and never a test.
 *
 * It can run twice for the same loss (AO_LowPower repeats its AWAKE) and does no harm the
 * second time: what is already in force is not asked again, and nothing is said of it.
 *
 * @param only The MESH_SETTING_* parts that are to be put back; the rest is left as it is.
 *
 * What is put back is what the app last asked for, as it asked: the same begin or
 * whole setting, judged again by the plan. Whatever cannot be put back is lost, and
 * the app is told that too.
 */
static void restore(uint32_t only)
{
    uint32_t wanted = 0UL;
    uint32_t done   = 0UL;

    stops_apply();   /* what the app ended while it was away */
    if (s.want.radio) {
        wanted |= MESH_SETTING_RADIO | (s.want.receive ? MESH_SETTING_RECEIVER : 0UL);
    }
    if (s.want.gnss) {
        wanted |= MESH_SETTING_GNSS;
    }
    wanted &= only;
    if (wanted == 0UL) {
        return;
    }
    if ((wanted & MESH_SETTING_RADIO) != 0UL) {
        mesh_aos_request_t r = { .kind = s.want.configured ? MESH_REQUEST_RADIO_CONFIGURE : MESH_REQUEST_RADIO_BEGIN };
        bool const receive   = s.want.receive;

        if (s.want.configured) {
            r.config = s.want.config;
        } else {
            r.a = s.want.band;
            r.u = s.want.sf;
            r.i = s.want.power_cdbm;
        }
        kode_status_t const rc = s.want.configured ? do_radio_configure(&r) : do_radio_begin(&r);
        if (rc == KODE_OK) {
            done |= MESH_SETTING_RADIO;
            if (receive && ((wanted & MESH_SETTING_RECEIVER) != 0UL) && (do_receive(true) == KODE_OK)) {
                done |= MESH_SETTING_RECEIVER;
            }
        }
    }
    if (((wanted & MESH_SETTING_GNSS) != 0UL) && (do_gnss_begin() == KODE_OK)) {
        done |= MESH_SETTING_GNSS;
    }
    publish();
    if (done != 0UL) {
        KLOG_I(TAG, "the module is back: the app's setting is put on it again (0x%lx)", (unsigned long)done);
        emit((uint8_t)MESH_AOS_EVENT_SETTING_RESTORED, done);
    }
    if ((wanted & ~done) != 0UL) {
        KLOG_W(TAG, "the module is back but the app's setting could not be put on it again (0x%lx)",
               (unsigned long)(wanted & ~done));
        setting_lost(wanted & ~done, false);
    }
}

/* ---- The tick -------------------------------------------------------------- */

/**
 * @brief Asleep: read what was in flight, and if the receiver goes on talking past the
 *        driver's margin it did not sleep. It is then closed as mesh_gnss_end() did
 *        before the Sleep command, and the next begin restarts it.
 */
static void gnss_watch_sleep(void)
{
    mesh_drv_gnss_status_t now;

    mesh_drv_gnss_poll();
    mesh_drv_gnss_read(&now);
    if (!now.asleep) {
        mesh_drv_gnss_close();
        s.gnss_asleep = false;
        KLOG_W(TAG, "the GNSS ignored its Sleep command: closed instead");
    }
}

static void tick(uint32_t elapsed)
{
    /* The look and the alive poll count in steps, and a stall of the owner
     * must not make them skip ahead: clamped. The radio's deadlines - a
     * send's backstop, a listen, a test signal's - are wall time, and a
     * stall must not stretch a carrier past its own: not clamped. */
    uint32_t const dt = (elapsed > DT_MAX_MS) ? DT_MAX_MS : elapsed;
    if (s.power_retry) {
        power_ask(s.power_wish);   /* the command found no room before; ended or not */
    }
    if (s_ended) {
        return;   /* let go: nothing is looked for, nothing is held */
    }
    if (!s.present) {
        look(dt);
        publish();
        return;
    }
    radio_tick(elapsed);
    (void)antennas_apply(false);
    if (s.gnss_asleep) {
        gnss_watch_sleep();
    }
    if (s.gnss_on) {
        mesh_drv_gnss_poll();
        mesh_drv_gnss_read(&s.gnss);
        /* The parser has no clock: this is where a report gets its time. A restart
         * takes its count back to 0, which is no report at all. */
        if (s.gnss.fix.reports != s.fix_reports) {
            s.fix_reports = s.gnss.fix.reports;
            s.fix_at_ms   = kode_sys_time_ms();
        }
    }
    if (gone(dt)) {
        uint32_t const lost = held_mask();
        module_leave(true);
        publish();
        emit((uint8_t)MESH_AOS_EVENT_GONE, 0U);
        setting_lost(lost, true);   /* kept: it is put back if the module returns */
        return;
    }
    publish();
}

static kode_status_t serve_one(const mesh_aos_request_t *r)
{
    if (moves(r) && (r->epoch != s.epoch)) {
        return deny(MESH_WHY_DROPPED);   /* asked before a stop: dropped */
    }
    return handle(r);
}

/**
 * @brief Take @p r for serving, unless its caller has already given up on it.
 *
 * The caller that gives up (mesh_aos_call()) and this check meet under the same
 * lock, so exactly one of two things is true: the caller withdrew the request
 * first and it is skipped here, or this marks it begun first and the caller
 * waits for the result. Never both - which is what stops a request that
 * "timed out" from running anyway, and a retry from doing it twice.
 *
 * @return false when the request is withdrawn and must not run.
 */
static bool claim(const mesh_aos_request_t *r)
{
    bool withdrawn = false;

    if (!r->answer) {
        return true;   /* posted: nobody waits, nobody withdraws */
    }
    taskENTER_CRITICAL(&s_lock);
    if ((int32_t)(r->seq - s_cancel_seq) <= 0) {
        withdrawn = true;
    } else {
        s_serving_seq = r->seq;
    }
    taskEXIT_CRITICAL(&s_lock);
    return !withdrawn;
}

/**
 * @brief The code the app is told. mesh.h documents five: KODE_OK, KODE_DENIED,
 *        KODE_TIMEOUT, KODE_NO_RESOURCES and KODE_IO_ERROR. A code a driver or kode-sdk
 *        below could answer with and the owner did not think of - KODE_ERROR, KODE_EXISTS -
 *        is the device refusing: it is KODE_IO_ERROR.
 */
static kode_status_t app_code(kode_status_t rc)
{
    if ((rc != KODE_OK) && (rc != KODE_DENIED) && (rc != KODE_TIMEOUT) && (rc != KODE_NO_RESOURCES)) {
        return KODE_IO_ERROR;
    }
    return rc;
}

/**
 * @brief Answer @p to with @p rc: out1, out2 and the reason are the owner's s.out1, s.out2 and s.why.
 *
 * A waiting task is woken (its semaphore, with the code and numbers left for it to read); an
 * Active Object that asked has its done function called, on this task, once.
 */
static void reply_give(const reply_t *to, kode_status_t rc)
{
    rc = app_code(rc);
    if (to->done != NULL) {
        mesh_aos_answer_t const a = {
            .rc   = rc,
            .why  = (rc == KODE_OK) ? (uint8_t)MESH_WHY_NONE : s.why,
            .out1 = s.out1,
            .out2 = s.out2,
        };
        to->done(to->ask, &a);
        return;
    }
    if (to->seq == 0U) {
        return;   /* a command: nobody is told */
    }
    if (to->held) {
        /* The call was told long ago that the owner holds it, and has let the line go: the
         * answer has a record and a semaphore of its own, so it never meets another call's. */
        taskENTER_CRITICAL(&s_lock);
        s.held_rc   = rc;
        s.held_why  = s.why;
        s.held_out1 = s.out1;
        s.held_out2 = s.out2;
        s.held_seq  = to->seq;
        taskEXIT_CRITICAL(&s_lock);
        (void)xSemaphoreGive(s.held_sem);
        return;
    }
    /* The two numbers go with the code, under the same lock: the owner may begin its next
     * request - an Active Object's, a command - before the waiting task has run, and that
     * clears s.out1 and s.out2. */
    taskENTER_CRITICAL(&s_lock);
    s.rc      = rc;
    s.rc_why  = s.why;
    s.rc_out1 = s.out1;
    s.rc_out2 = s.out2;
    s.rc_seq  = to->seq;
    taskEXIT_CRITICAL(&s_lock);
    (void)xSemaphoreGive(s.reply);
}

/** @brief Payload slot @p slot (1-based) is free again; 0 is no slot. */
static void slot_free(uint8_t slot)
{
    if ((slot == 0U) || (slot > SEND_SLOTS)) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_slot_busy[slot - 1U] = false;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- What the Active Object's signals do (mesh_ao.h) ------------------------- */

/** @brief What the owner tells a waiting call whose request it has taken and will answer later. */
#define RC_HELD  ((kode_status_t)0x7E57)

/** @brief What a wait returns when no answer came in its time: never a code the owner answers
 *         with, so an answer of KODE_TIMEOUT - a driver whose BUSY stuck - is not taken for it. */
#define RC_NO_ANSWER  ((kode_status_t)0x7E58)

/** @brief Tell the call waiting on @p seq that the owner holds its request: it lets the line go. */
static void held_notify(uint32_t seq)
{
    taskENTER_CRITICAL(&s_lock);
    s.rc      = RC_HELD;
    s.rc_why  = MESH_WHY_NONE;
    s.rc_out1 = 0U;
    s.rc_out2 = 0U;
    s.rc_seq  = seq;
    taskEXIT_CRITICAL(&s_lock);
    (void)xSemaphoreGive(s.reply);
}

void mesh_aos_on_request(const mesh_aos_request_t *req)
{
    stops_apply();
    mesh_aos_request_t r  = *req;
    bool const         go = claim(&r);
    /* The payload is the slot's, never the caller's buffer: a caller that
     * gave up is free to reuse that. */
    r.data = ((r.slot > 0U) && (r.slot <= SEND_SLOTS)) ? s_slot[r.slot - 1U] : NULL;
    if (r.data == NULL) {
        r.len = 0U;
    }
    s.answer_held = false;
    s.why         = MESH_WHY_NONE;
    s.out1        = 0U;
    s.out2        = 0U;
    kode_status_t const rc = go ? serve_one(&r) : deny(MESH_WHY_DROPPED);
    if ((rc != KODE_OK) && (s.why == (uint8_t)MESH_WHY_NONE)) {
        s.why = (uint8_t)MESH_WHY_CHIP;   /* refused by nobody: the chip or the bus did not do it */
    }
    slot_free(r.slot);
    publish();   /* before the answer: a waiter reads what this changed */
    if ((r.answer || (r.done != NULL)) && go && !s.answer_held) {
        reply_t const to = { .seq = r.answer ? r.seq : 0U, .ask = r.ask, .done = r.done, .held = false };
        reply_give(&to, rc);
    } else if (r.answer && go && s.answer_held) {
        held_notify(r.seq);
    } else {
        /* a command, or a request withdrawn: nobody to tell */
    }
}

void mesh_aos_on_tick(void)
{
    stops_apply();
    uint32_t const now = kode_sys_time_ms();
    uint32_t const dt  = now - s_tick_at;
    s_tick_at = now;
    tick(dt);
}

void mesh_aos_on_radio_interrupt(void)
{
    stops_apply();
    /* The line is read, not trusted: an interrupt that the tick has already
     * served finds it low. One that comes before the radio is set up is left
     * for the tick, as it always was. */
    if (s.present && s.radio_ready && mesh_drv_lr2021_dio9()) {
        radio_irq_take();
        publish();
    }
}

void mesh_aos_on_stop(void)
{
    stops_apply();
    if (s.present) {
        (void)antennas_apply(false);   /* the nudge of mesh_aos_antennas_confirm() */
    }
    publish();
}

/**
 * @brief Off air and held there: the board is about to go or to rest, and nothing after
 *        this is promised. What was asked to go on air before is dropped too. A chip that
 *        confirms standby is put to sleep with its reset line high (no
 *        current through the module's 10 k pull-up R502); one that does not has its reset
 *        line held low, so that a chip that did not hear the standby cannot stay keyed.
 * @param why  What the log says is coming.
 * @param rest The board will wake again (AO_LowPower): what the app had set up is kept and
 *             put back by mesh_aos_on_awake(). Otherwise (going home) it is forgotten.
 */
static void everything_off(const char *why, bool rest)
{
    /* What is held, before the stop below takes the receiver off and the GNSS sleeps. */
    uint32_t const lost = ((s.present && s.radio_open)
                               ? (held_mask() & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER))
                               : 0UL)
                          | (s.gnss_on ? MESH_SETTING_GNSS : 0UL);

    taskENTER_CRITICAL(&s_lock);
    s_stop_epoch++;
    taskEXIT_CRITICAL(&s_lock);
    stops_apply();
    bool asleep = false;
    if (s.present && s.radio_open) {
        flags_idle();
        s.radio_ready = false;
        asleep = radio_sleep();
        if (!asleep) {
            mesh_drv_lr2021_hold_reset();
            s.radio_asleep = false;
        }
        s.need_reset  = true;   /* mesh_radio_begin() resets it: lets the line go, or wakes it */
    }
    gnss_quiet();   /* asleep, not running unread; the wake (restore()) wakes it */
    if (rest) {
        s.rest_lost |= lost;
    }
    publish();
    KLOG_W(TAG, "%s: the radio is off air and %s", why, asleep ? "asleep" : "held in reset");
    setting_lost(lost, rest);
}

void mesh_aos_on_rest(void)
{
    everything_off("the board is going to rest", true);
}

void mesh_aos_on_awake(void)
{
    /* AO_LowPower repeats AWAKE after an abandoned round, and says it for a board that never
     * rested: the second time finds nothing lost and does nothing. */
    uint32_t const only = s.rest_lost;

    s.rest_lost = 0UL;
    if ((only != 0UL) && s.present) {
        restore(only);   /* what the app had set up, as it asked; never a send, never a test */
    }
}

void mesh_aos_on_home(void)
{
    everything_off("the bottom button was held, the board goes home", false);
}

void mesh_aos_on_power(bool on, uint8_t rc)
{
    if (on != s.power_wish) {
        return;   /* the answer to a wish that was changed since */
    }
    switch ((kode_status_t)rc) {
    case KODE_OK:
        s.supply = (uint8_t)(on ? MESH_SUPPLY_ON : MESH_SUPPLY_OFF);
        break;
    case KODE_DENIED:
        s.supply = (uint8_t)MESH_SUPPLY_NO_LINK;
        KLOG_W(TAG, "J3's supply is not switched yet: the link to the C5 is down; the wish stays and the next link carries it out");
        break;
    default:
        s.supply = (uint8_t)MESH_SUPPLY_FAILED;
        KLOG_E(TAG, "the C5 refused to switch J3's supply %s", on ? "on" : "off");
        break;
    }
    publish();
}

/* ---- The calls ------------------------------------------------------------- */

bool mesh_aos_kode_ready(void)
{
    return mesh_ao_kode_ready();
}

bool mesh_aos_running(void)
{
    return s_started;
}

bool mesh_aos_caller_is_ao(const char **name)
{
    return mesh_ao_caller_is_ao(name);
}

kode_status_t mesh_aos_end(void)
{
    if (!s_started) {
        mesh_aos_why_note(MESH_WHY_NONE);
        return KODE_OK;   /* nothing is running */
    }
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_END };
    if (!mesh_aos_caller_is_ao(NULL)) {
        /* Calls are served one at a time: a caller that waits for a search or a packet holds
         * the line until it ends. The stop cuts that short (the caller is answered
         * "interrupted") so this call is next and does not wait it out. */
        (void)mesh_aos_stop();
    }
    return mesh_aos_call(&req, MESH_AOS_END_MS, NULL, NULL);
}

kode_status_t mesh_aos_end_post(void)
{
    if (!s_started) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    mesh_aos_request_t r = { .kind = MESH_REQUEST_END };
    r.epoch = s_stop_epoch;
    kode_status_t const rc = mesh_ao_post(&r);
    mesh_aos_why_note((rc == KODE_OK) ? MESH_WHY_NONE : MESH_WHY_QUEUE_FULL);
    return rc;
}

/** @brief mesh_aos_start()'s work, done by one caller at a time. */
static kode_status_t start_work(void)
{
    if (s.reply == NULL) {
        s.reply    = xSemaphoreCreateBinary();
        s.callers  = xSemaphoreCreateMutex();
        s.held_sem = xSemaphoreCreateBinary();
    }
    if ((s.reply == NULL) || (s.callers == NULL) || (s.held_sem == NULL)) {
        return KODE_NO_RESOURCES;
    }
    /* The 868 duty bucket lives for as long as the app does, not per radio_begin
     * nor per module nor per mesh_end(): re-begins, a module unplugged and plugged
     * again, and an end and a start, must not refund the hour's airtime. It is RAM,
     * so a restart of the app starts it full. The band guard in do_send() is what
     * keeps it off the 2.4 GHz path. */
    if (!s_duty_ready) {
        mesh_plan_duty_init(&s.duty, MESH_REGION_SUB_GHZ_DUTY_PPT);
        s_duty_ready = true;
    }
    s.look_ms = MESH_BSP_LOOK_MS;   /* the first look on the first tick */
    s.seen    = MESH_MODULE_NONE;
    s_ended   = false;
    s.epoch   = s_stop_epoch;
    s.rest_lost = 0UL;
    /* The module's supply is asked from the owner's first tick, with the owner already
     * subscribed to EXP_POWER_SIG: the module comes up a few ms later, or after a Wi-Fi
     * scan the C5 is busy with, and the owner finds it whenever its pull-ups appear. The
     * state it was in before is not assumed. */
    s.power_wish  = true;
    s.power_retry = true;
    s.supply      = (uint8_t)MESH_SUPPLY_UNKNOWN;
    publish();   /* mesh_status() before the first tick: no module */
    s_tick_at = kode_sys_time_ms();
    kode_status_t const rc = mesh_ao_start();
    if (rc != KODE_OK) {
        return rc;
    }
    s_started = true;
    return KODE_OK;
}

kode_status_t mesh_aos_start(void)
{
    if (s_started) {
        mesh_aos_why_note(MESH_WHY_NONE);
        return KODE_OK;
    }
    if (!mesh_ao_kode_ready()) {
        mesh_aos_why_note(MESH_WHY_KODE_NOT_READY);
        return KODE_DENIED;
    }
    /* Two callers at once: one starts it, the other waits the few microseconds it takes. */
    for (uint32_t waited = 0U; ; waited++) {
        taskENTER_CRITICAL(&s_lock);
        bool const mine = !s_starting && !s_started;
        bool const busy = s_starting;
        if (mine) {
            s_starting = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (mine) {
            break;
        }
        if (!busy || (waited > 1000U)) {
            mesh_aos_why_note(s_started ? MESH_WHY_NONE : MESH_WHY_NOT_STARTED);
            return s_started ? KODE_OK : KODE_NO_RESOURCES;
        }
        vTaskDelay(1);
    }
    kode_status_t const rc = start_work();
    taskENTER_CRITICAL(&s_lock);
    s_starting = false;
    taskEXIT_CRITICAL(&s_lock);
    mesh_aos_why_note((rc == KODE_OK) ? MESH_WHY_NONE
                      : ((rc == KODE_DENIED) ? MESH_WHY_LEVEL_TAKEN : MESH_WHY_QUEUE_FULL));
    return rc;
}

bool mesh_aos_present(void)
{
    mesh_aos_status_t st;
    mesh_aos_status(&st);
    return st.present;
}

kode_status_t mesh_aos_stop(void)
{
    if (!s_started) {
        return KODE_DENIED;
    }
    taskENTER_CRITICAL(&s_lock);
    s_stop_epoch++;
    s_forget |= MESH_SETTING_RECEIVER;   /* the receiver is off: not to be put back */
    taskEXIT_CRITICAL(&s_lock);
    mesh_ao_nudge();
    return KODE_OK;
}

uint8_t mesh_aos_header_position(uint8_t pos)
{
    /* v1 is upright only: a position is itself. A position outside J3's range
     * is not the module's, present or not. */
    if ((pos < 1U) || (pos > KODE_SYS_EXP_POS_MAX)) {
        return 0U;
    }
    return mesh_aos_present() ? pos : 0U;
}

/**
 * @brief Make a request safe to queue: its payload goes into a slot of the
 *        owner's, and the request carries the slot's number.
 *
 * The queue copies the request and not what it points at, and a caller that
 * gives up is free to reuse its buffer. So the bytes are copied here, on the
 * caller's side, before anything is queued, and the owner reads that copy and
 * frees the slot once it has served or skipped the request.
 * @return KODE_NO_RESOURCES when every slot still holds a request the owner
 *         has not taken: it is stuck, or callers gave up on it.
 */
static kode_status_t capture(mesh_aos_request_t *r)
{
    r->slot = 0U;
    if ((r->data == NULL) || (r->len == 0U)) {
        r->data = NULL;
        r->len  = 0U;
        return KODE_OK;
    }
    uint8_t found = 0U;
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < (uint8_t)SEND_SLOTS; i++) {
        if (!s_slot_busy[i]) {
            s_slot_busy[i] = true;
            found          = (uint8_t)(i + 1U);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (found == 0U) {
        return KODE_NO_RESOURCES;
    }
    (void)memcpy(s_slot[found - 1U], r->data, r->len);
    r->slot = found;
    r->data = NULL;
    return KODE_OK;
}

/**
 * @brief The app ended something while the module is away: the call is refused, but
 *        what the owner would put back when the module returns must not include it.
 */
static void forget_noted(mesh_request_kind_t kind, uint32_t u)
{
    uint32_t what = 0UL;

    switch (kind) {
    case MESH_REQUEST_RADIO_END:
        what = MESH_SETTING_RADIO;
        break;
    case MESH_REQUEST_RADIO_STANDBY:
        what = MESH_SETTING_RECEIVER;
        break;
    case MESH_REQUEST_RADIO_RECEIVE:
        what = (u == 0U) ? MESH_SETTING_RECEIVER : 0UL;
        break;
    case MESH_REQUEST_GNSS_END:
        what = MESH_SETTING_GNSS;
        break;
    default:
        break;
    }
    if (what != 0UL) {
        taskENTER_CRITICAL(&s_lock);
        s_forget |= what;
        taskEXIT_CRITICAL(&s_lock);
    }
}

kode_status_t mesh_aos_post(const mesh_aos_request_t *req)
{
    if (!s_started || (req == NULL)) {
        mesh_aos_why_note((req == NULL) ? MESH_WHY_BAD_ARGUMENT : MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    if (!mesh_aos_present()) {
        forget_noted(req->kind, req->u);   /* the owner is not there to end it: it will be told */
        mesh_aos_why_note(MESH_WHY_MODULE_ABSENT);
        return KODE_DENIED;
    }
    mesh_aos_request_t r = *req;
    r.answer = false;
    r.done   = NULL;
    r.ask    = NULL;
    r.epoch  = s_stop_epoch;
    kode_status_t rc = capture(&r);
    if (rc != KODE_OK) {
        mesh_aos_why_note(MESH_WHY_NO_SLOT);
        return rc;
    }
    rc = mesh_ao_post(&r);
    if (rc != KODE_OK) {
        slot_free(r.slot);
        mesh_aos_why_note(MESH_WHY_QUEUE_FULL);
    } else {
        mesh_aos_why_note(MESH_WHY_NONE);
    }
    return rc;
}

kode_status_t mesh_aos_ask(const mesh_aos_request_t *req, void *ask, mesh_aos_done_fn_t done)
{
    if ((req == NULL) || (done == NULL)) {
        mesh_aos_why_note(MESH_WHY_BAD_ARGUMENT);
        return KODE_DENIED;
    }
    if (!s_started) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    mesh_aos_request_t r = *req;
    r.answer = false;
    r.seq    = 0U;
    r.ask    = ask;
    r.done   = done;
    r.epoch  = s_stop_epoch;
    kode_status_t rc = capture(&r);
    if (rc != KODE_OK) {
        mesh_aos_why_note(MESH_WHY_NO_SLOT);
        return rc;
    }
    rc = mesh_ao_post(&r);
    if (rc != KODE_OK) {
        slot_free(r.slot);
        mesh_aos_why_note(MESH_WHY_QUEUE_FULL);
    } else {
        mesh_aos_why_note(MESH_WHY_NONE);
    }
    return rc;
}

/** @brief Wait up to @p ms for the owner's answer to @p r; RC_NO_ANSWER if none. */
static kode_status_t await_answer(const mesh_aos_request_t *r, uint32_t ms, uint32_t *out1,
                                  uint32_t *out2, uint8_t *why)
{
    TickType_t const until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);

    for (;;) {
        TickType_t const now  = xTaskGetTickCount();
        TickType_t const left = ((TickType_t)(until - now) > pdMS_TO_TICKS(ms))
                                ? 0 : (TickType_t)(until - now);
        if (xSemaphoreTake(s.reply, left) != pdTRUE) {
            return RC_NO_ANSWER;
        }
        /* The answer as reply_give() left it, read in one piece. */
        taskENTER_CRITICAL(&s_lock);
        bool const          ours   = (s.rc_seq == r->seq);
        kode_status_t const rc     = s.rc;
        uint32_t const      first  = s.rc_out1;
        uint32_t const      second = s.rc_out2;
        uint8_t const       reason = s.rc_why;
        taskEXIT_CRITICAL(&s_lock);
        if (ours) {
            if (out1 != NULL) {
                *out1 = first;
            }
            if (out2 != NULL) {
                *out2 = second;
            }
            *why = reason;
            return rc;
        }
        /* an answer to a call that gave up before: not ours */
    }
}

/**
 * @brief Wait up to @p ms for the answer of the request the owner holds for call @p seq.
 *
 * Its record and its semaphore are the owner's one held request's, not the line's: an answer
 * to a call that gave up before is skipped. RC_NO_ANSWER if none came.
 */
static kode_status_t await_held(uint32_t seq, uint32_t ms, uint32_t *out1, uint32_t *out2,
                                uint8_t *why)
{
    TickType_t const until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);

    for (;;) {
        TickType_t const now  = xTaskGetTickCount();
        TickType_t const left = ((TickType_t)(until - now) > pdMS_TO_TICKS(ms))
                                ? 0 : (TickType_t)(until - now);
        if (xSemaphoreTake(s.held_sem, left) != pdTRUE) {
            return RC_NO_ANSWER;
        }
        /* The answer as reply_give() left it, read in one piece. */
        taskENTER_CRITICAL(&s_lock);
        bool const          ours   = (s.held_seq == seq);
        kode_status_t const rc     = s.held_rc;
        uint32_t const      first  = s.held_out1;
        uint32_t const      second = s.held_out2;
        uint8_t const       reason = s.held_why;
        taskEXIT_CRITICAL(&s_lock);
        if (ours) {
            if (out1 != NULL) {
                *out1 = first;
            }
            if (out2 != NULL) {
                *out2 = second;
            }
            *why = reason;
            return rc;
        }
        /* the answer of a call that gave up before: not ours */
    }
}

/* ---- Why the last call of a task did not go through ------------------------- */

#define WHY_TASKS      4U    /* tasks that are remembered; the oldest gives way to a fifth */
#define WHY_DETAIL_MAX 112U

static struct {
    TaskHandle_t task;
    uint8_t      why;
    char         detail[WHY_DETAIL_MAX];   /* the sentence, when the reason alone does not say it */
} s_why[WHY_TASKS];                          /* under s_lock */
static uint8_t s_why_next;                   /* under s_lock: the entry a new task takes */

/** @brief The calling task's entry; a new task takes the oldest. Under s_lock. */
static uint8_t why_entry(TaskHandle_t me)
{
    for (uint8_t i = 0U; i < WHY_TASKS; i++) {
        if (s_why[i].task == me) {
            return i;
        }
    }
    uint8_t const slot = s_why_next;
    s_why_next = (uint8_t)((s_why_next + 1U) % WHY_TASKS);
    s_why[slot].task      = me;
    s_why[slot].why       = MESH_WHY_NONE;
    s_why[slot].detail[0] = '\0';
    return slot;
}

void mesh_aos_why_note(uint8_t why)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();

    if (mesh_aos_caller_is_ao(NULL)) {
        return;   /* an Active Object is told in its answers, and must not push a task's reason out of the table */
    }
    taskENTER_CRITICAL(&s_lock);
    uint8_t const slot = why_entry(me);
    s_why[slot].why       = why;
    s_why[slot].detail[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);
}

void mesh_aos_why_note_from_ao(const char *fn, const char *ao)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();

    taskENTER_CRITICAL(&s_lock);
    uint8_t const slot = why_entry(me);
    s_why[slot].why = MESH_WHY_FROM_AO;
    (void)snprintf(s_why[slot].detail, sizeof(s_why[slot].detail),
                   "%s was called from an Active Object (%s); mesh.h calls block - call mesh_sys_* instead",
                   (fn != NULL) ? fn : "a mesh.h call", (ao != NULL) ? ao : "?");
    taskEXIT_CRITICAL(&s_lock);
}

uint8_t mesh_aos_why(void)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    uint8_t why = MESH_WHY_NONE;

    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < WHY_TASKS; i++) {
        if (s_why[i].task == me) {
            why = s_why[i].why;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return why;
}

const char *mesh_aos_error(void)
{
    TaskHandle_t const me = xTaskGetCurrentTaskHandle();
    const char *text = "";

    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < WHY_TASKS; i++) {
        if (s_why[i].task == me) {
            if (s_why[i].detail[0] != '\0') {
                text = s_why[i].detail;
            } else if (s_why[i].why != MESH_WHY_NONE) {
                text = mesh_aos_why_text(s_why[i].why);
            } else {
                /* the last call went through */
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return text;
}

const char *mesh_aos_why_text(uint8_t why)
{
    switch (why) {
    case MESH_WHY_NONE:           return "no error";
    case MESH_WHY_NOT_STARTED:    return "the SDK has not begun: call mesh_begin() (tasks) or mesh_sys_begin() (Active Objects) first";
    case MESH_WHY_FROM_AO:        return "a call that blocks was made from an Active Object: call mesh_sys_* instead";
    case MESH_WHY_MODULE_ABSENT:  return "no Mesh module on J3 (not plugged in, or not found yet)";
    case MESH_WHY_BAD_ARGUMENT:   return "a bad argument: a null pointer, or a value or length out of range";
    case MESH_WHY_NOT_BEGUN:      return "the radio has no setting yet: begin the radio (or configure it) first";
    case MESH_WHY_BUSY_TEST:      return "a test signal is on air";
    case MESH_WHY_BUSY_SENDING:   return "a packet is on air";
    case MESH_WHY_BUSY_SEARCH:    return "a channel search is running";
    case MESH_WHY_BUSY_LISTENING: return "a timed listen is running";
    case MESH_WHY_BUSY_ARRIVING:  return "a packet is on its way in";
    case MESH_WHY_NOT_ALLOWED:    return "the setting is outside what the region allows (mesh_region.h)";
    case MESH_WHY_NOT_CALIBRATED: return "the frequency is not one the radio was calibrated for (calibrate it first)";
    case MESH_WHY_NOT_RECEIVING:  return "the receiver is off, or has taken no sample yet";
    case MESH_WHY_NO_GUARD:       return "a carrier test needs the driver's restart protection, which is not available";
    case MESH_WHY_DROPPED:        return "dropped: a stop came after this was asked";
    case MESH_WHY_UNKNOWN:        return "the owner does not know this request";
    case MESH_WHY_DUTY:           return "the duty cycle has no room yet: wait for the send wait and ask again";
    case MESH_WHY_NO_SLOT:        return "busy: both payload slots hold requests the owner has not taken yet";
    case MESH_WHY_QUEUE_FULL:     return "busy: the owner's queue is full";
    case MESH_WHY_TIMED_OUT:      return "the owner did not answer in time";
    case MESH_WHY_INTERRUPTED:    return "cut short by a stop, a standby or the module leaving";
    case MESH_WHY_GNSS_OFF:       return "the GNSS is off: begin it first";
    case MESH_WHY_SEND_FAILED:    return "the chip never said the packet was done: it may not have gone whole";
    case MESH_WHY_LEVEL_TAKEN:    return "the owner's QP/C level is taken by another Active Object, or does not exist in this build: move CONFIG_MESH_AO_PRIORITY";
    case MESH_WHY_KODE_NOT_READY: return "kode_init() has not run";
    case MESH_WHY_NO_PACKET:      return "no packet is waiting to be read";
    case MESH_WHY_NO_FIX:         return "the GNSS has no fix yet";
    case MESH_WHY_LAB_ONLY:       return "a test signal is a lab build's: this build has no CONFIG_MESH_LAB";
    case MESH_WHY_CHIP:           return "the module's chip did not do it (a bus or chip error)";
    default:                      return "unknown";
    }
}

static kode_status_t call_work(const mesh_aos_request_t *req, uint32_t ms, uint32_t *out1,
                               uint32_t *out2, uint8_t *why)
{
    if (!s_started || (req == NULL)) {
        *why = (req == NULL) ? MESH_WHY_BAD_ARGUMENT : MESH_WHY_NOT_STARTED;
        return KODE_DENIED;
    }
    if (mesh_aos_caller_is_ao(NULL)) {
        /* The owner's own task and an app's Active Object: the wait would hold that
         * object's queue for up to the call's time and more. */
        KLOG_W(TAG, "a call that waits was made from an Active Object: "
                    "call mesh_sys_* from it, or this one from a task");
        *why = MESH_WHY_FROM_AO;
        return KODE_DENIED;
    }
    if (!mesh_aos_present() && (req->kind != MESH_REQUEST_END)) {
        forget_noted(req->kind, req->u);   /* the owner is not there to end it: it will be told */
        *why = MESH_WHY_MODULE_ABSENT;
        return KODE_DENIED;
    }
    if (xSemaphoreTake(s.callers, pdMS_TO_TICKS(ms)) != pdTRUE) {
        *why = MESH_WHY_TIMED_OUT;
        return KODE_TIMEOUT;
    }
    mesh_aos_request_t r = *req;
    r.answer = true;
    r.done   = NULL;
    r.ask    = NULL;
    r.seq    = ++s.seq;
    r.epoch  = s_stop_epoch;

    /* A full queue or no free slot is "busy": asked again every RETRY_STEP_MS until the deadline,
     * as kode_retry does, and only then refused. */
    kode_status_t rc = KODE_NO_RESOURCES;
    for (uint32_t waited = 0U; ; waited += RETRY_STEP_MS) {
        r.data = req->data;
        r.len  = req->len;
        rc = capture(&r);
        if (rc != KODE_OK) {
            *why = MESH_WHY_NO_SLOT;
        } else {
            rc = mesh_ao_post(&r);
            if (rc != KODE_OK) {
                slot_free(r.slot);
                *why = MESH_WHY_QUEUE_FULL;
            }
        }
        if ((rc != KODE_NO_RESOURCES) || (waited >= ms)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_STEP_MS));
    }
    bool line_given = false;
    if (rc == KODE_OK) {
        uint32_t const grace = (req->kind == MESH_REQUEST_RADIO_SEND) ? MESH_AOS_SEND_GRACE_MS
                                                                    : MESH_AOS_CALL_GRACE_MS;
        rc = await_answer(&r, ms, out1, out2, why);
        if (rc == RC_NO_ANSWER) {
            /* Giving up. A request the owner has not begun is withdrawn - it
             * will be skipped, so it never happens and a retry cannot double
             * it. One it has begun is going to happen: wait for it, and answer
             * with what it did. */
            bool begun;
            taskENTER_CRITICAL(&s_lock);
            begun = (s_serving_seq == r.seq);
            if (!begun) {
                s_cancel_seq = r.seq;
            }
            taskEXIT_CRITICAL(&s_lock);
            if (begun) {
                rc = await_answer(&r, grace, out1, out2, why);
                if (rc == RC_NO_ANSWER) {
                    KLOG_W(TAG, "a request the owner began did not finish in %lu ms more: "
                                "whether it happened is not known",
                           (unsigned long)grace);
                }
            }
        }
        if (rc == RC_HELD) {
            /* The owner took it and will answer when the packet is gone or the search is over.
             * That can be seconds: the line is let go so that a call from another task - a stop,
             * the receiver, a read of the status - is not held up behind it. */
            (void)xSemaphoreGive(s.callers);
            line_given = true;
            rc = await_held(r.seq, grace, out1, out2, why);
        }
    }
    if (rc == RC_NO_ANSWER) {
        rc   = KODE_TIMEOUT;   /* the owner's silence; a KODE_TIMEOUT it answered keeps its own reason */
        *why = MESH_WHY_TIMED_OUT;
    }
    if (!line_given) {
        (void)xSemaphoreGive(s.callers);
    }
    return rc;
}

kode_status_t mesh_aos_call(const mesh_aos_request_t *req, uint32_t ms, uint32_t *out1,
                           uint32_t *out2)
{
    uint8_t why = MESH_WHY_NONE;
    kode_status_t const rc = call_work(req, ms, out1, out2, &why);

    mesh_aos_why_note((rc == KODE_OK) ? MESH_WHY_NONE : why);
    return rc;
}

void mesh_aos_status(mesh_aos_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_stat;
    taskEXIT_CRITICAL(&s_lock);
}

kode_status_t mesh_aos_packet_read(mesh_aos_packet_t *rx)
{
    if (rx == NULL) {
        mesh_aos_why_note(MESH_WHY_BAD_ARGUMENT);
        return KODE_DENIED;
    }
    if (!s_started) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    kode_status_t rc = KODE_NO_RESOURCES;
    taskENTER_CRITICAL(&s_lock);
    if (s_ring_n > 0U) {
        rx_slot_t * const slot = &s_ring[s_ring_tail];
        (void)memcpy(rx->data, slot->data, slot->length);
        rx->length   = slot->length;
        rx->crc_valid = slot->crc_valid;
        rx->rssi_dbm = slot->rssi_dbm;
        rx->snr_db   = slot->snr_db;
        s_ring_tail  = (uint8_t)((s_ring_tail + 1U) % RX_SLOTS);
        s_ring_n--;
        rc = KODE_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (rc == KODE_OK) {
        mesh_aos_why_note(MESH_WHY_NONE);
    }
    return rc;
}

kode_status_t mesh_aos_wait_present(uint32_t ms)
{
    if (!s_started || mesh_aos_caller_is_ao(NULL)) {
        mesh_aos_why_note(s_started ? MESH_WHY_FROM_AO : MESH_WHY_NOT_STARTED);
        return KODE_DENIED;
    }
    for (uint32_t waited = 0U; ; waited += TICK_MS) {
        if (mesh_aos_present()) {
            return KODE_OK;
        }
        if (waited >= ms) {
            return KODE_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void mesh_aos_news_connect(mesh_aos_news_fn_t fn)
{
    taskENTER_CRITICAL(&s_lock);
    s_news_fn = fn;
    taskEXIT_CRITICAL(&s_lock);
}
