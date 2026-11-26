/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_aos.h
 * @brief The Mesh module's owner: one Active Object that finds the module,
 *        holds its lines, and is the only code that drives the radio and the
 *        GNSS.
 *
 * kode-sdk's rule, one owner per device, applied to the module: an Active
 * Object of kode-sdk's own kind (kode_sys.h), AO_Mesh, the module's as
 * AO_LedMgr is the LED's. It looks for a Mesh on J3 by its fingerprint - the
 * four lines its pull resistors hold high (SCL, SDA, the LoRa chip select
 * and the LoRa reset) - and, when it matches, opens the LR2021 and asks its
 * version: a radio that answers is the module, and that same answer is how
 * the owner knows later that it is still there. From then on it runs every
 * MESH_AO_TICK_MS: it reads the radio's interrupt line, moves a received
 * packet into a ring the app pops, reads the GNSS, and spends the sub-GHz
 * duty budget. The radio's interrupt wakes it at once as well. Everything
 * else asks it with a request, an event on its queue.
 *
 * TWO LAYERS, ONE OWNER. A request reaches the owner from a task (the
 * standard layer, mesh.h: mesh_aos_call() waits for the answer on a
 * semaphore) or from an Active Object (the extended layer, mesh_sys.h:
 * mesh_aos_ask() posts the request and the owner calls the done function it
 * was given, which posts the answer to the asking Active Object). The owner's
 * handler is the same for both, as kode-sdk's kode_sys_imu.c does it. A
 * command (mesh_aos_post()) has no answer at all.
 *
 * A request that sets something up answers when it has applied it. A send
 * answers when the packet is gone (or refused, or cut short), a channel
 * search when it ends. What happens on its own - a packet landing, the module
 * found or lost - is news: the standard layer hears it as named events
 * (mesh_aos_listener_add()), the extended layer through its sink
 * (mesh_aos_news_connect()). Received packets are not answers: they land in
 * the ring and mesh_aos_packet_read() pops them, after a
 * MESH_AOS_EVENT_PACKET says one has.
 *
 * A call that gives up waiting never leaves a request behind that could run
 * later: one the owner had not yet started is withdrawn, so a retry cannot
 * make the same packet twice; one it had started is waited for a little longer
 * and answered with what it really did (mesh_aos_call()).
 *
 * Whatever is on air is the owner's to take off: every way out of a state that
 * may have the amplifier keyed goes through one routine that confirms standby,
 * and a chip that will not confirm is reset (mesh_drv_lr2021.h).
 *
 * The two layers (mesh.h, mesh_sys.h) are the only intended callers; their
 * comments are the contract. These types are the owner's own (tools/check_layers.sh,
 * rule 6) except what mesh_types.h shares with them.
 */
#ifndef MESH_AOS_H_
#define MESH_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "mesh_types.h"
#include "mesh_drv_nmea.h"
#include "mesh_drv_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest payload a packet carries, the chip's and the plan's. */
#define MESH_AOS_PAYLOAD_MAX   MESH_DRV_LORA_PAYLOAD_MAX

/** @brief How many frequencies the front end keeps. */
#define MESH_AOS_CALIBRATIONS  MESH_CALIBRATIONS_MAX

/** @brief How much longer mesh_aos_call() waits for a request the owner has begun. */
#define MESH_AOS_CALL_GRACE_MS 3000U

/**
 * @brief How much longer mesh_aos_call() waits for a send the owner has begun: its
 *        answer is the packet gone, and the owner's own backstop is the airtime and a
 *        quarter more. The longest packet the region allows is ten seconds on air.
 */
#ifndef MESH_AOS_SEND_GRACE_MS
#define MESH_AOS_SEND_GRACE_MS 120000U
#endif

/** @brief What a request asks. */
typedef enum {
    MESH_REQUEST_RADIO_BEGIN = 0, /**< a: band, u: sf, i: cdbm. Configure + calibrate. */
    MESH_REQUEST_RADIO_END,     /**< Receive off, the chip asleep from a standby.  */
    MESH_REQUEST_RADIO_STANDBY, /**< Take everything off air, stay configured.      */
    MESH_REQUEST_RADIO_SEND,          /**< data/len: one packet. Answered when it is gone (out1: its
                            *   airtime, ms), or refused (out1: the wait, ms, when the duty
                            *   budget has no room).                              */
    MESH_REQUEST_RADIO_RECEIVE,       /**< u: 1 receive on, 0 off.                        */
    MESH_REQUEST_RADIO_LISTEN,        /**< u: ms to receive one packet; it or the chip's timer ends it. */
    MESH_REQUEST_RADIO_TEST,       /**< u: a mesh_drv_lr2021_test_t (0 stops one), i: how
                            *   long, ms, 1..MESH_BSP_TEST_MAX_MS. Refused when
                            *   a packet or a test is on air already.          */
    MESH_REQUEST_GNSS_BEGIN,    /**< Open the receiver (or wake it) and listen.     */
    MESH_REQUEST_GNSS_END,      /**< Put it to sleep (close one that cannot sleep). */
    MESH_REQUEST_GNSS_RESET,    /**< Restart the receiver.                          */
    MESH_REQUEST_I2C_LEND,      /**< Answered with the pair's GPIOs in out1 (SCL), out2 (SDA). */
    MESH_REQUEST_I2C_RETURN,    /**< Answered.                                      */
    MESH_REQUEST_RADIO_HOLD_RESET,    /**< Hold the radio in reset; beginning the radio lets
                            *   it go.                                         */
    MESH_REQUEST_RADIO_CONFIGURE, /**< config: a whole setting. Configure + calibrate. */
    MESH_REQUEST_RADIO_CAD,     /**< u: symbols (0 the setting's), a: bit 0 fast, bit 1
                            *   the receive that follows a preamble seen. Answered
                            *   when the search ends: out1 is 1 for activity.      */
    MESH_REQUEST_RADIO_CALIBRATE, /**< hz, u of them: the front end, kept on the chip. */
    MESH_REQUEST_RADIO_RETUNE,  /**< config: another setting, with no calibration.  */
    MESH_REQUEST_END,           /**< Let go of the module: everything off, the lines back,
                            *   J3's supply asked off, no more looking. Served with or
                            *   without a module.                              */
} mesh_request_kind_t;

/**
 * @brief What the owner answers a request with: the code (one of the five of
 *        mesh.h), why not when it is not KODE_OK, and up to two numbers.
 */
typedef struct {
    kode_status_t rc;    /**< KODE_OK, KODE_DENIED, KODE_NO_RESOURCES, KODE_TIMEOUT or KODE_IO_ERROR. */
    uint8_t       why;   /**< A mesh_why_t; MESH_WHY_NONE with KODE_OK.                                */
    uint32_t      out1;  /**< The request's first number (see the kind).                               */
    uint32_t      out2;  /**< Its second.                                                               */
} mesh_aos_answer_t;

/**
 * @brief The done function of a request made with mesh_aos_ask(): called on the owner's
 *        task, exactly once, with @p ask as it was given. It must not block.
 */
typedef void (*mesh_aos_done_fn_t)(void *ask, const mesh_aos_answer_t *answer);

/** @brief One request. */
typedef struct {
    mesh_request_kind_t kind;
    bool                answer; /**< Set by mesh_aos_call(): a task waits.        */
    uint32_t            seq;    /**< Set by mesh_aos_call(): which call.          */
    uint32_t            epoch;  /**< Set on the way in: stops before it.          */
    uint8_t             a;
    int32_t             i;
    uint32_t            u;
    const uint8_t      *data;   /**< MESH_REQUEST_RADIO_SEND: the payload, read by
                                 *   mesh_aos_call() and mesh_aos_ask() before they
                                 *   return and never after: the caller's buffer is its
                                 *   own again.                                   */
    uint8_t             len;    /**< Its length.                                  */
    uint8_t             slot;   /**< Set on the way in: where the owner finds
                                 *   its copy of the payload, 1-based; 0 none.   */
    mesh_drv_lora_config_t config; /**< MESH_REQUEST_RADIO_CONFIGURE's and _RETUNE's
                                    *   setting.                                 */
    uint16_t            preamble; /**< MESH_REQUEST_RADIO_SEND: this packet's, 0 is
                                 *   the setting's.                              */
    uint32_t            hz[MESH_AOS_CALIBRATIONS]; /**< MESH_REQUEST_RADIO_CALIBRATE's. */
    void               *ask;    /**< Set by mesh_aos_ask(): given back to @c done.   */
    mesh_aos_done_fn_t  done;   /**< Set by mesh_aos_ask(): an Active Object waits.   */
} mesh_aos_request_t;

/** @brief One received packet, as mesh_aos_packet_read() gives it. */
typedef struct {
    uint8_t  data[MESH_AOS_PAYLOAD_MAX];
    uint8_t  length;
    bool     crc_valid;   /**< It carried a CRC and the CRC matched. */
    float    rssi_dbm;
    float    snr_db;
} mesh_aos_packet_t;

/** @brief Everything the owner says about the module, as of its last tick. */
typedef struct {
    bool     present;          /**< On J3, fingerprint matched and radio answered. */
    bool     radio_ready;      /**< Configured for a band (MESH_REQUEST_RADIO_BEGIN). */
    bool     radio_asleep;     /**< The chip sleeps (no setting on it): begin wakes it. */
    uint8_t  band;             /**< The mesh_plan_band_t of the setting.          */
    uint8_t  spreading_factor; /**< The setting's, 5..12.                         */
    uint32_t frequency_hz;     /**< The setting's centre.                         */
    uint32_t bandwidth_hz;     /**< Its bandwidth.                                */
    uint8_t  coding_rate;      /**< Its N of 4/N.                                 */
    uint16_t preamble;         /**< Its preamble symbols.                         */
    uint8_t  sync_word;        /**< Its sync word.                                */
    int32_t  power_cdbm;       /**< Conducted power asked, hundredths of a dBm.   */
    int32_t  power_applied_cdbm; /**< What the setting comes to, the same scale; 0 with no setting. */
    bool     sending;          /**< A packet is on air.                           */
    bool     receiving;        /**< The receiver is on (continuous).              */
    bool     arriving;         /**< A packet is on its way in, from its preamble
                                *   to its end, or to a timer if the end is lost. */
    bool     cad_running;      /**< A channel search is on.                       */
    bool     testing;          /**< A test signal is on air.                      */
    bool     amplifier_fault;  /**< The amplifier faulted: the radio is off and
                                *   stays so until MESH_REQUEST_RADIO_BEGIN.      */
    bool     antennas_confirmed; /**< The app confirmed the antennas.            */
    uint8_t  antenna_suspected; /**< Bands whose amplifier tripped since the app last
                                *   confirmed, bit (mesh_plan_band_t): held to the
                                *   safe power.                                  */
    bool     power_capped;     /**< The setting in force is held below what was
                                *   asked by the safe power.                     */
    uint32_t duty_wait_ms;     /**< Until the last send's airtime is within the
                                *   sub-GHz duty cycle again; 0 when it is, and
                                *   on 2.4 GHz.                                  */
    float    last_rssi_dbm;    /**< The receiver's RSSI, sampled by the tick.     */
    bool     rssi_valid;       /**< The receiver is on and a sample has been taken since. */
    uint8_t  packets_waiting;  /**< In the ring, unread.                          */
    uint32_t packets_received; /**< Since the module came.                        */
    uint32_t packets_dropped;  /**< Lost to a full ring.                          */
    uint32_t packets_sent;     /**< That left whole.                              */
    uint16_t radio_errors;     /**< The last non-zero GetErrors word.             */

    bool                gnss_on;        /**< The receiver is open and being read. */
    mesh_drv_nmea_fix_t fix;            /**< The latest fix.                      */
    uint32_t            fix_age_ms;     /**< Since a sentence last reported the
                                         *   position; UINT32_MAX until one has.  */
    uint32_t            gnss_sentences; /**< NMEA sentences with a good checksum. */
    uint32_t            gnss_baud;      /**< The rate it locked at; 0 until one.  */
    bool                gnss_talking;   /**< A sentence has checked.              */
    bool                gnss_asleep;    /**< Told to sleep; begin wakes it.       */
    bool                pps_high;       /**< The 1PPS line is high now.           */

    bool     i2c_taken;        /**< The I2C pair is the app's (MESH_REQUEST_I2C_LEND). */
    uint8_t  seen;             /**< A mesh_module_t: what is on J3 while absent.   */
    uint16_t seen_level;       /**< The reading behind it, bit (position - 1).    */

    uint8_t  supply;           /**< A mesh_supply_t: J3's supply, as the last switch left it. */
    uint32_t events_dropped;   /**< Events lost to the listeners' full queue.    */
    uint32_t news_dropped;     /**< News not posted to a subscribed Active Object. */
    uint32_t answers_dropped;  /**< Answers lost to an asker's completely full queue. */
} mesh_aos_status_t;

/**
 * @brief What the owner tells, by number. The standard layer hears them by name
 *        (mesh_aos_event_name()), the extended layer as signals.
 *
 * Numbers 4 and 12 were the events of a send gone and a search done; the answers
 * to those requests are what tell that now.
 */
typedef enum {
    MESH_AOS_EVENT_PRESENT      = 1,   /**< The module was found; arg 1 (upright).  */
    MESH_AOS_EVENT_GONE         = 2,   /**< It left J3, or the app let it go.      */
    MESH_AOS_EVENT_PACKET           = 3,   /**< A packet landed; arg its length.       */
    MESH_AOS_EVENT_LISTEN_END      = 5,   /**< A timed listen ended; arg 0.           */
    MESH_AOS_EVENT_RADIO_ERROR  = 6,   /**< arg: the GetErrors word (16 bits).     */
    MESH_AOS_EVENT_WRONG_MODULE = 7,   /**< Not a Mesh on J3; arg a mesh_module_t.    */
    MESH_AOS_EVENT_TEST_END     = 8,   /**< A test signal ended by itself; arg a
                                    *   MESH_TEST_END_*.                          */
    MESH_AOS_EVENT_PREAMBLE     = 9,   /**< A preamble was detected; arg 0.        */
    MESH_AOS_EVENT_HEADER       = 10,  /**< Its header came in valid; arg 0.       */
    MESH_AOS_EVENT_HEADER_BAD   = 11,  /**< Its header failed; arg 0.              */
    MESH_AOS_EVENT_SETTING_LOST     = 13,  /**< The radio's setting, its receiver or the
                                        *   GNSS went away without being asked to;
                                        *   arg a MESH_SETTING_* mask.                */
    MESH_AOS_EVENT_SETTING_RESTORED = 14,  /**< The owner put them back after the module
                                        *   returned or the board woke; arg a
                                        *   MESH_SETTING_* mask.                      */
} mesh_aos_event_t;

/** @brief The highest number in mesh_aos_event_t. */
#define MESH_AOS_EVENT_MAX  14U

/**
 * @brief Start the owner's task. Idempotent, from any task or Active Object; returns
 *        at once and does not wait for the module nor for J3's supply.
 * @return KODE_OK; KODE_NO_RESOURCES when the task or its queue could not be made;
 *         KODE_DENIED when the owner's QP/C level is not its to take (a message in the log says which).
 */
kode_status_t mesh_aos_start(void);

/** @brief Whether the owner has been started (and not ended since). */
bool mesh_aos_running(void);

/** @brief Whether kode_init() has brought the pools up: what the extended layer needs before it starts the owner. */
bool mesh_aos_kode_ready(void);

/** @brief Whether a Mesh is on J3 and held. */
bool mesh_aos_present(void);

/**
 * @brief Everything off, without the queue: the radio to standby, the receiver
 *        off, any test signal stopped. Returns at once; the owner does it on
 *        its next turn - within a tick, or when the command it is in the middle
 *        of is done. Requests that were made before it and send or set up are
 *        dropped when the owner takes them. A caller that must know the radio
 *        is silent when it returns asks for MESH_REQUEST_RADIO_STANDBY, which waits.
 * @return KODE_DENIED before mesh_aos_start().
 */
kode_status_t mesh_aos_stop(void);

/** @brief How long mesh_aos_end() waits for the owner to let go. */
#define MESH_AOS_END_MS  2000U

/**
 * @brief Let go of the module: radio and GNSS off, the lines back to kodeOS, J3's supply
 *        asked off, and no more looking for one. mesh_aos_start() starts it all again. BLOCKS.
 * @return KODE_OK (also when nothing was running); KODE_DENIED from an Active Object;
 *         KODE_TIMEOUT if the owner did not answer in MESH_AOS_END_MS.
 */
kode_status_t mesh_aos_end(void);

/**
 * @brief As mesh_aos_end() but posted, for an Active Object: it returns at once and the
 *        owner lets go on its next turn, telling MESH_AOS_EVENT_GONE if a module was held.
 * @return KODE_OK; KODE_DENIED before mesh_aos_start(); KODE_NO_RESOURCES when the owner's
 *         queue or the pool had no room.
 */
kode_status_t mesh_aos_end_post(void);

/**
 * @brief The J3 position of the module's line at upright position @p pos; 0
 *        while it is not there. This release recognises the module upright only.
 */
uint8_t mesh_aos_header_position(uint8_t pos);

/**
 * @brief Post @p req as a command: the owner applies it on its next turn and nobody is
 *        told how it went but the log and the status.
 * @return KODE_DENIED before mesh_aos_start() or with no module;
 *         KODE_NO_RESOURCES when the queue is full.
 */
kode_status_t mesh_aos_post(const mesh_aos_request_t *req);

/**
 * @brief Post @p req as a request for an Active Object: @p done is called with @p ask, on
 *        the owner's task, exactly once, when the request has its answer. Returns at once.
 *
 * A payload is copied before this returns. A request refused by the owner (no module, a
 * busy radio) is answered, not returned.
 *
 * @param req  The request.
 * @param ask  Given back to @p done.
 * @param done Called with the answer; not NULL.
 * @return KODE_OK (taken: it will be answered); KODE_DENIED before mesh_aos_start(), or for
 *         a NULL @p req or @p done; KODE_NO_RESOURCES when the queue, the pool or both payload
 *         slots were full. Nothing was posted for a return other than KODE_OK.
 */
kode_status_t mesh_aos_ask(const mesh_aos_request_t *req, void *ask, mesh_aos_done_fn_t done);

/**
 * @brief Post @p req and wait for the owner's answer. BLOCKS; one caller at a
 *        time, the others wait their turn.
 *
 * If the owner has not started the request when @p ms is up, it is withdrawn:
 * KODE_TIMEOUT then means it did not happen and never will, so asking again
 * cannot do it twice. If the owner had started it, the call waits up to
 * MESH_AOS_CALL_GRACE_MS more for the owner to finish (MESH_AOS_SEND_GRACE_MS for a
 * send) and answers with what the request really did. KODE_TIMEOUT after that grace is
 * the one case that does not say: a request the owner took and is still working on, which
 * may yet complete (the status and the events say).
 *
 * @param ms   How long to wait for the owner to answer.
 * @param out1 Where an answer's first number goes; may be NULL.
 * @param out2 Its second; may be NULL.
 * @return The owner's answer, or KODE_TIMEOUT. The owner's answer can be KODE_TIMEOUT too (a chip
 *         whose BUSY stuck): it comes at once, and its reason is not MESH_WHY_TIMED_OUT.
 */
kode_status_t mesh_aos_call(const mesh_aos_request_t *req, uint32_t ms, uint32_t *out1,
                           uint32_t *out2);

/** @brief The reason (mesh_why_t) the calling task's last call to the owner left. */
uint8_t mesh_aos_why(void);

/** @brief Leave @p why as the calling task's last reason: for what a layer refuses itself. */
void mesh_aos_why_note(uint8_t why);

/**
 * @brief The reason in words, valid for ever; "unknown" for a number that is not one. Neutral
 *        between the layers.
 */
const char *mesh_aos_why_text(uint8_t why);

/**
 * @brief Leave "@p fn was called from an Active Object (@p ao)..." as the calling task's last
 *        reason: what the standard layer's refusal says, with the names.
 */
void mesh_aos_why_note_from_ao(const char *fn, const char *ao);

/**
 * @brief The calling task's last reason in words: the sentence a refusal left, else the reason's
 *        text; "" when its last call went through. Valid until that task's next call.
 */
const char *mesh_aos_error(void);

/** @brief Events the standard layer's events task lost because its queue was full. */
uint32_t mesh_aos_events_dropped(void);

/**
 * @brief Whether the caller runs on the task of an Active Object - the owner's or
 *        an app's. A call that waits for the owner must not be made there.
 * @param[out] name The Active Object's task name when true; may be NULL.
 */
bool mesh_aos_caller_is_ao(const char **name);

/** @brief A copy of the owner's last status. */
void mesh_aos_status(mesh_aos_status_t *out);

/**
 * @brief Pop the oldest unread packet into @p rx.
 * @return KODE_OK when one was there; KODE_NO_RESOURCES when the ring is empty;
 *         KODE_DENIED before the owner runs.
 */
kode_status_t mesh_aos_packet_read(mesh_aos_packet_t *rx);

/**
 * @brief The app's word on the antennas: true when the SMA of every band it uses carries its
 *        antenna or a 50 ohm load. Until it is true every transmit is held to the safe power
 *        (MESH_REGION_SAFE_CDBM), and either answer clears what an amplifier fault suspected.
 *
 * Kept in the owner's RAM, never in flash: it survives the board resting, and the module
 * leaving and mesh_aos_end() forget it. Not queued: it is left under the lock and the owner
 * is woken, which puts the new power on a setting in force at its next turn, or before the
 * next transmit when a packet is on air or arriving then. Returns at once, from any task or
 * Active Object, with or without the module.
 * @return KODE_OK; KODE_DENIED before mesh_aos_start().
 */
kode_status_t mesh_aos_antennas_confirm(bool fitted);

/** @brief Wait until the module is held. BLOCKS. KODE_TIMEOUT if not in @p ms. */
kode_status_t mesh_aos_wait_present(uint32_t ms);

/* ---- The news ------------------------------------------------------------------
 *
 * Two ways to hear what the owner tells, one for each layer. Both are called on the
 * owner's task, right after the status shows what the news is about, and neither may
 * block. */

/**
 * @brief The extended layer's sink: called with the news' number (mesh_aos_event_t) and
 *        its value; answers how many of those it should have reached it could not.
 */
typedef uint8_t (*mesh_aos_news_fn_t)(uint8_t ev, uint32_t value);

/** @brief Who hears the news: @p fn, or nobody for NULL. Replaces the previous sink. */
void mesh_aos_news_connect(mesh_aos_news_fn_t fn);

/** @brief The extended layer says an answer found its asker's queue full and was lost: counted. */
void mesh_aos_answer_dropped(void);

/**
 * @brief The name the standard layer hears event @p ev by.
 * @return The name; NULL for a number that has none.
 */
const char *mesh_aos_event_name(uint8_t ev);

/** @brief Whether @p name is the name of an event. */
bool mesh_aos_event_known(const char *name);

/**
 * @brief Call @p fn every time @p name happens. Adding the same pair twice registers it once.
 * @return true when registered; false for an unknown name, a NULL @p fn, a full table or
 *         when the events task could not be made.
 */
bool mesh_aos_listener_add(const char *name, mesh_listener_t fn);

/**
 * @brief Stop calling @p fn for @p name. Once it returns no call of it is running or to come: from
 *        another task it waits for the one running; on the events task it does not wait.
 * @return true when it was registered.
 */
bool mesh_aos_listener_remove(const char *name, mesh_listener_t fn);

/**
 * @brief Wait for @p name: only an event that arrives after the wait began counts.
 * @param ms  The longest to wait; 0 returns false at once.
 * @param out Receives a copy of the event; its text is a constant. May be NULL.
 * @return true when it happened; false on a timeout, an unknown name, a full table of
 *         waiters, or when called from a listener.
 */
bool mesh_aos_listener_wait(const char *name, uint32_t ms, mesh_event_t *out);

/**
 * @brief The owner's side: hand an event to the events task without blocking.
 * @param ev    A mesh_aos_event_t.
 * @param value Its value.
 * @return true when the queue took it, or when no one has asked to hear events; false when it
 *         was full and the event is lost.
 */
bool mesh_aos_events_push(uint8_t ev, int32_t value);

/* ---- What both layers do alike -------------------------------------------------
 *
 * mesh.h and mesh_sys.h refuse the same arguments, build the same requests and read the same
 * status, so the checks and the copies are here once (mesh_aos_args.c, plain C). The mesh_arg
 * functions fill @p r and return true, or return false for an argument outside what the
 * layers' headers allow - a null pointer, a length, a band, a time, a power outside
 * MESH_POWER_X10_MIN..MAX - and the caller answers MESH_WHY_BAD_ARGUMENT. */

bool mesh_aos_arg_radio_begin(mesh_aos_request_t *r, mesh_band_t band, uint8_t spreading_factor, int32_t power_dbm_x10);
bool mesh_aos_arg_radio_config(mesh_aos_request_t *r, mesh_request_kind_t kind, const mesh_radio_config_t *config);
bool mesh_aos_arg_radio_calibrate(mesh_aos_request_t *r, const uint32_t *frequencies_hz, uint8_t count);
bool mesh_aos_arg_radio_send(mesh_aos_request_t *r, const uint8_t *data, uint8_t length, uint16_t preamble);
/** @brief MESH_REQUEST_RADIO_CAD's bits in a: the shorter search, and the receive after it. @{ */
#define MESH_AOS_CAD_FAST     0x01U
#define MESH_AOS_CAD_RECEIVE  0x02U
/** @} */
bool mesh_aos_arg_radio_cad(mesh_aos_request_t *r, uint8_t symbols, bool fast);
bool mesh_aos_arg_radio_cad_receive(mesh_aos_request_t *r, uint8_t symbols, bool fast);
bool mesh_aos_arg_radio_test(mesh_aos_request_t *r, mesh_test_signal_t mode, uint32_t ms);
bool mesh_aos_arg_radio_listen(mesh_aos_request_t *r, uint32_t ms);
bool mesh_aos_arg_radio_config_default(mesh_band_t band, mesh_radio_config_t *out);

/** @brief The status in the app's words (mesh_status_t). */
void mesh_aos_status_public(mesh_status_t *out);

/** @brief The latest GNSS fix in the app's words; valid or not. */
void mesh_aos_fix_public(mesh_gnss_fix_t *out);

/** @brief mesh_aos_packet_read() in the app's words. */
kode_status_t mesh_aos_packet_public(mesh_packet_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* MESH_AOS_H_ */
