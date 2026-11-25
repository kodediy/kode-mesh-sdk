/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_sys.h
 * @brief The Mesh module for an Active Object: a LoRa radio on two bands and a GNSS
 *        receiver, on the Kode Dot's expansion header, asked without ever waiting.
 *
 *     #include "mesh_sys.h"
 *
 *     // In your Active Object's initial transition, after kode_init():
 *     (void)mesh_sys_begin();
 *     (void)mesh_sys_subscribe(&me->super);     // MESH_MODULE_FOUND_SIG and the other news
 *
 *     // When MESH_MODULE_FOUND_SIG arrives:
 *     (void)mesh_sys_radio_begin(&me->super, MESH_BAND_SUB_GHZ, 9, 85);   // answer: MESH_RADIO_BEGIN_RSP_SIG
 *
 *     // When the answer arrives and its rc is KODE_OK, after a person pressed a button:
 *     (void)mesh_sys_radio_send(&me->super, (const uint8_t *)"hello", 5); // answer: MESH_RADIO_SEND_RSP_SIG
 *
 * This is the extended layer: the calls of an Active Object (AO), as kode_sys.h is kodeOS's.
 * mesh.h is the standard layer - the same module for a task, whose calls wait for their
 * answer. A call there made from an AO is refused; a call here is safe anywhere and waits for
 * nothing. Both layers go through the same owner, AO_Mesh (the component mesh_aos), which is
 * the only code that touches the module, so an app may use mesh.h in a task and mesh_sys.h in
 * an AO at the same time.
 *
 * THE MODULE IS FOUND, NOT ASSUMED. mesh_sys_begin() starts AO_Mesh, which reads the
 * module's fingerprint on J3 every MESH_BSP_LOOK_MS - the four lines its pull resistors hold
 * high, three readings in a row - and, when it matches, opens the LR2021 and asks its version:
 * only a radio that answers is taken, so a Camera or an F*CK with a like fingerprint is not.
 * MESH_MODULE_FOUND_SIG tells you; until then every request is answered KODE_DENIED with
 * MESH_WHY_MODULE_ABSENT. This release recognises the module upright only; one plugged in
 * turned round reads as "not a Mesh".
 *
 * ==========================================================================================
 * Commands
 *
 * A command posts to AO_Mesh, leaving its queue's margin free, and returns:
 *   KODE_OK            the owner's queue took it.
 *   KODE_NO_RESOURCES  the event pool is dry, or the owner's queue is within the margin;
 *                      nothing was posted, so call again later.
 *   KODE_DENIED        a bad argument, the owner has not been started (by mesh_sys_begin(), or
 *                      by mesh.h's mesh_begin()), or there is no module; nothing was posted.
 * Any one command is safe in a loop: it is refused, never QP's assert. What the owner then
 * does shows in mesh_sys_status_read(), in its log and in the news below. Safe from an AO
 * and from any task. The commands are mesh_sys_radio_standby(), mesh_sys_radio_receive(),
 * mesh_sys_radio_listen(), mesh_sys_stop() and mesh_sys_radio_antennas_confirm(); the last two do not
 * wait for the owner's queue, and the last is taken with no module too.
 *
 * ==========================================================================================
 * Requests
 *
 * A request takes the asking AO, @p me, posts to AO_Mesh and returns at once. Its answer
 * arrives to @p me later, as an event: each call names the answer's signal
 * (MESH_<VERB>_RSP_SIG), and the answer's type names its fields. Every answer carries @c rc and
 * @c why (mesh_sys_why_text() puts it in words).
 *
 * Immediate returns:
 *   KODE_OK            taken; the answer will come.
 *   KODE_DENIED        @p me is NULL or not started, an argument is bad, the owner has not been
 *                      started (by mesh_sys_begin(), or by mesh.h's mesh_begin()), @p me's door
 *                      of that kind is busy, or the owner's priority is not its to take. Nothing
 *                      was posted.
 *   KODE_NO_RESOURCES  the owner's queue is within its margin, both payload slots hold
 *                      requests it has not taken yet, or the event pool is dry. Nothing was posted.
 *
 * Doors: each asker has one door per kind of request - there are fifteen kinds, one for each
 * request below - and a request holds its door until its answer; a second request of the same
 * kind before then is refused at once. The door is free again just before the answer is posted,
 * so @p me may ask again from the answer's handler.
 *
 * Answers: a request taken is answered exactly once, posted to @p me with no margin and never
 * published, with the final status in @c rc. The owner's refusals (the radio busy, absent or
 * not set up) arrive in the answer, not as a return: with no module, a request is still
 * answered. The answer for a payload (a packet to send) is made when the owner has taken a
 * copy: the caller's buffer is its own again when the call returns.
 *
 * Queue depth, the one rule for every asker: an answer that finds @p me's queue completely full
 * is lost - counted in status.answers_dropped and said in the log, never QP's assert - so the
 * queue holds everything that can wait in it at once:
 *   - one slot for each request waiting for its answer: one per door open;
 *   - @p me's own events: its timers, the signals it subscribes to (a button press and its
 *     release are two) and whatever else is posted to it;
 *   - this SDK's news, if @p me subscribed (mesh_sys_subscribe()): they are posted leaving a
 *     margin, so they can never be the one that overflows - but a queue within the margin loses
 *     them, and counts them (status.news_dropped).
 * For example, an AO with 16 slots, a 1 s timer, BUTTON_EVT_SIG and the news asks on one tick
 * for a radio setting, a send and a channel search: three slots wait for their answers, the
 * next tick is one, a press and its release two, and a packet and a header news two more -
 * eight of sixteen. Ask for a dozen kinds at once and the same AO needs a longer queue.
 *
 * Answer signals: the signals the answers arrive on are posted, never published, to the asker
 * alone. An AO never subscribes to one.
 *
 * ==========================================================================================
 * Immediate calls
 *
 * Each reads a copy, a flag or a number, or subscribes the calling AO, and returns: it posts
 * nothing and waits on nothing but a lock held for microseconds. Safe from an AO and from any
 * task. They are mesh_sys_is_present(), mesh_sys_status_read(), mesh_sys_header_position(),
 * mesh_sys_why_text(), mesh_sys_subscribe(), mesh_sys_unsubscribe(), mesh_sys_radio_read(),
 * mesh_sys_radio_rssi(), mesh_sys_radio_channel_busy(), mesh_sys_radio_send_wait_ms(),
 * mesh_sys_radio_config_default(), mesh_sys_gnss_fix() and mesh_sys_gnss_pps().
 *
 * ==========================================================================================
 * The news
 *
 * What happens to the module that nobody asked for arrives to every AO that subscribed, as
 * signals posted to it: MESH_MODULE_FOUND_SIG, MESH_MODULE_LOST_SIG and the others below, each
 * a mesh_sys_news_t. They are posted leaving a margin of the AO's queue free. If they do not
 * fit they are lost, and counted in mesh_sys_status_read()'s news_dropped: a news is never the
 * assert. At most MESH_SUBSCRIBERS_MAX AOs subscribe. A received packet is read with
 * mesh_sys_radio_read(), not carried in the news.
 *
 * SIGNALS. An AO of the app cannot publish, so every signal here is posted, in a range of this
 * SDK's own: from MESH_SIG_FIRST = MAX_SIG + CONFIG_MESH_SIG_OFFSET (1000 by default) for as
 * many as this header lists. Your own signals are free from MAX_SIG to MAX_SIG + 999, and
 * another module SDK has a range of its own above (Robotics 1100, Camera 1200, F*CK 1300).
 *
 * J3's SUPPLY. +3.3V_EXT's state when an app starts is not promised (kode-sdk's kode.h says
 * so). mesh_sys_begin() asks kodeOS to switch it on - it needs CONFIG_KODEOS_RADIO=y, as the pin
 * is the C5's - and learns whether it did from EXP_POWER_SIG; status.supply says. Without the radio (CONFIG_KODEOS_RADIO=n) J3 is
 * always powered: the module is then found as soon as it is plugged, and mesh_sys_end() cannot
 * switch it off.
 *
 * SLEEP. With CONFIG_KODE_LOWPOWER, when the board goes to rest the SDK takes everything off
 * air, turns the receiver off, puts the GNSS to sleep and puts the radio to sleep (held in reset if
 * it will not confirm standby), and keeps what you
 * had set up. When the board wakes it puts back the setting, the receiver and the GNSS that the
 * rest took (MESH_SETTING_RESTORED_SIG), and only those: what you ended yourself stays ended,
 * a packet is never sent again, a test signal never repeated, and waking twice does it once. J3's
 * supply is not switched by a rest. HOME_SIG ends it all, as mesh_sys_end() does.
 *
 * THE AMPLIFIERS ARE GUARANTEED ONLY INTO A LOAD. The SMA for the band in use carries its
 * antenna, or a 50 ohm load, before any transmit - mesh_sys_radio_send(), mesh_sys_radio_test().
 * The module ships with its antennas loose and the chip cannot tell one is missing, so every
 * transmit is held to the safe power until the app confirms them (mesh_sys_radio_antennas_confirm()),
 * and a band whose amplifier faults (MESH_RADIO_FAILED_SIG, MESH_RADIO_ERROR_AMPLIFIER_FAULT) is held
 * there again until the app confirms once more. mesh_sys_status_read() says so: antennas_confirmed,
 * power_capped (the setting in force goes out below what was asked) and antenna_suspected[band].
 * A test signal is a lab build's (CONFIG_MESH_LAB; any other build refuses it) and goes into a
 * 50 ohm load only, never an antenna: it keys the amplifier for as long as it is asked (at most
 * MESH_BSP_TEST_MAX_MS), 100 % of the time on air.
 *
 * WITHIN THE RULES. Unless the build sets CONFIG_MESH_LAB (not for a firmware that is distributed),
 * every transmit is held to the region's limits (mesh_region.h): the power is capped, and on
 * the 868 sub-band the time on air is held to 1 % over a long run by a budget that refills with
 * time - mesh_sys_radio_send() is answered KODE_NO_RESOURCES with MESH_WHY_DUTY when there is no
 * room, and mesh_sys_radio_send_wait_ms() says how long to wait. The budget holds 36 s, starts
 * full and gets 36 s back per hour, so from full up to 72 s can go out in the first hour; it
 * lives in RAM, and an app that restarts starts with it full (mesh.h, "WITHIN THE RULES").
 *
 * Nothing here sends, emits or records by itself: a packet goes on air only when you ask for it.
 */
#ifndef MESH_SYS_H_
#define MESH_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_sys.h"      /* QActive, QEvt, MAX_SIG, kode_status_t */

#include "mesh_bsp.h"
#include "mesh_region.h"   /* the limits an app may read */
#include "mesh_types.h"    /* the bands, a setting, a packet, a fix, the status */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * The signals
 * ========================================================================= */

/** @brief The offset of this SDK's range from MAX_SIG; the Kconfig says it. */
#ifndef CONFIG_MESH_SIG_OFFSET
#define CONFIG_MESH_SIG_OFFSET 1000
#endif

/** @brief The first signal of this SDK. */
#define MESH_SIG_FIRST  ((uint32_t)MAX_SIG + (uint32_t)CONFIG_MESH_SIG_OFFSET)

/** @brief The most AOs that can subscribe to the news at once. */
#define MESH_SUBSCRIBERS_MAX  4U

/**
 * @brief This SDK's signals, all posted, none published. The news are first, then the answers.
 */
enum {
    /* ---- The news (mesh_sys_news_t), to every AO that subscribed ---- */
    MESH_MODULE_FOUND_SIG = MESH_SIG_FIRST, /**< The module was found; value 1 (upright).      */
    MESH_MODULE_LOST_SIG,                   /**< It left J3, or mesh_sys_end() let it go.      */
    MESH_WRONG_MODULE_SIG,                  /**< Not a Mesh on J3; value a mesh_module_t.      */
    MESH_SETTING_LOST_SIG,                  /**< The radio's setting, its receiver or the GNSS
                                             *   went away without being asked to; value a
                                             *   MESH_SETTING_* mask.                         */
    MESH_SETTING_RESTORED_SIG,              /**< The SDK put them back after the module
                                             *   returned or the board woke; value the mask.   */
    MESH_PACKET_RECEIVED_SIG,               /**< A packet landed; value its length. Read it
                                             *   with mesh_sys_radio_read().                    */
    MESH_LISTEN_ENDED_SIG,                  /**< A timed listen (mesh_sys_radio_listen())
                                             *   ended; value 0.                                */
    MESH_RADIO_FAILED_SIG,                  /**< The radio went wrong; value the chip's error
                                             *   word in the low 16 bits and, above them, one
                                             *   of MESH_RADIO_ERROR_*.                         */
    MESH_TEST_ENDED_SIG,                    /**< A test signal ended without being stopped;
                                             *   value a MESH_TEST_END_*.                       */
    MESH_PREAMBLE_DETECTED_SIG,             /**< A preamble was detected: a packet may follow;
                                             *   value 0. The receiver may also take noise for
                                             *   one; then nothing more is said.                */
    MESH_HEADER_RECEIVED_SIG,               /**< Its header came in valid; value 0.             */
    MESH_HEADER_FAILED_SIG,                 /**< Its header failed: no packet follows; value 0. */

    /* ---- The answers, one for each request (a mesh_sys_*_answer_t), to the asker alone ---- */
    MESH_RADIO_BEGIN_RSP_SIG,
    MESH_RADIO_CONFIGURE_RSP_SIG,
    MESH_RADIO_CALIBRATE_RSP_SIG,
    MESH_RADIO_RETUNE_RSP_SIG,
    MESH_RADIO_END_RSP_SIG,
    MESH_RADIO_SEND_RSP_SIG,
    MESH_RADIO_SEND_PREAMBLE_RSP_SIG,
    MESH_RADIO_CAD_RSP_SIG,
    MESH_RADIO_CAD_RECEIVE_RSP_SIG,
    MESH_RADIO_TEST_RSP_SIG,
    MESH_GNSS_BEGIN_RSP_SIG,
    MESH_GNSS_END_RSP_SIG,
    MESH_GNSS_RESET_RSP_SIG,
    MESH_I2C_LEND_RSP_SIG,
    MESH_I2C_RETURN_RSP_SIG,

    MESH_SIG_END                            /**< One past this SDK's last signal.              */
};

/** @brief How many signals this SDK uses. */
#define MESH_SIG_COUNT  ((uint32_t)MESH_SIG_END - MESH_SIG_FIRST)

_Static_assert((uint32_t)MESH_SIG_END <= 65535U, "mesh_sys.h: the signals do not fit in 16 bits: lower CONFIG_MESH_SIG_OFFSET");
_Static_assert(CONFIG_MESH_SIG_OFFSET >= 1000, "mesh_sys.h: CONFIG_MESH_SIG_OFFSET must leave the app its own 1000 above MAX_SIG");

/* =========================================================================
 * What the news and the answers carry
 * ========================================================================= */

/** @brief A news: @c value as the signal says. */
typedef struct {
    QEvt    super;   /**< The signal. */
    int32_t value;   /**< What the news is about, as its signal says. */
} mesh_sys_news_t;

/**
 * @brief The answer to a request that has nothing to hand back (MESH_RADIO_BEGIN_RSP_SIG,
 *        _CONFIGURE_, _CALIBRATE_, _RETUNE_, _END_, MESH_RADIO_TEST_RSP_SIG, MESH_GNSS_*_RSP_SIG,
 *        MESH_I2C_RETURN_RSP_SIG).
 *
 * @c rc is KODE_OK when it was done, KODE_DENIED when the owner refused it (@c why says why),
 * KODE_IO_ERROR when the chip or the receiver refused, or KODE_TIMEOUT when it did not
 * finish in time.
 */
typedef struct {
    QEvt          super;
    kode_status_t rc;    /**< KODE_OK, KODE_DENIED, KODE_NO_RESOURCES, KODE_TIMEOUT or KODE_IO_ERROR. */
    mesh_why_t    why;   /**< MESH_WHY_NONE with KODE_OK; else the reason (mesh_sys_why_text()).      */
} mesh_sys_answer_t;

/**
 * @brief MESH_RADIO_SEND_RSP_SIG and MESH_RADIO_SEND_PREAMBLE_RSP_SIG: the packet gone, or
 *        refused.
 *
 * KODE_OK: the chip said it left whole; @c airtime_ms is the time it was on air. KODE_NO_RESOURCES
 * with MESH_WHY_DUTY: the 868 duty budget has no room, and @c wait_ms is how long until it has;
 * the packet never went. KODE_DENIED: refused (the radio busy or not set up), or cut short by
 * mesh_sys_stop() or the module leaving (MESH_WHY_INTERRUPTED). KODE_IO_ERROR with
 * MESH_WHY_SEND_FAILED: the chip never said it was done, and it may not have gone whole.
 */
typedef struct {
    QEvt          super;
    kode_status_t rc;
    mesh_why_t    why;
    uint32_t      airtime_ms;   /**< With KODE_OK: how long it was on air, ms.            */
    uint32_t      wait_ms;      /**< With MESH_WHY_DUTY: how long to wait, ms; else 0.     */
} mesh_sys_send_answer_t;

/**
 * @brief MESH_RADIO_CAD_RSP_SIG and MESH_RADIO_CAD_RECEIVE_RSP_SIG: the channel search ended.
 *
 * KODE_OK: @c detected says whether it saw activity. A packet already arriving answers KODE_OK
 * with @c detected true at once, without a search.
 */
typedef struct {
    QEvt          super;
    kode_status_t rc;
    mesh_why_t    why;
    bool          detected;   /**< With KODE_OK: the search saw activity on the channel. */
} mesh_sys_cad_answer_t;

/**
 * @brief MESH_I2C_LEND_RSP_SIG: the I2C pair is yours - or refused.
 *
 * KODE_OK: @c scl_gpio and @c sda_gpio are the pair's GPIOs; hand it back with
 * mesh_sys_i2c_return(). The stock GNSS firmware drives SDA push-pull: mastering the bus fights
 * the receiver, at your own risk.
 */
typedef struct {
    QEvt          super;
    kode_status_t rc;
    mesh_why_t    why;
    uint8_t       scl_gpio;   /**< With KODE_OK: the ESP32-P4 GPIO of SCL. */
    uint8_t       sda_gpio;   /**< With KODE_OK: the GPIO of SDA.          */
} mesh_sys_i2c_answer_t;

/* =========================================================================
 * Begin and end
 * ========================================================================= */

/**
 * @brief Start AO_Mesh: switch J3's supply on and begin looking for the module. Not a command
 *        nor a request: it starts the owner itself, answers nothing later, and returns at once.
 *
 * kode_init() must have run, or the pools the owner posts from do not exist. Idempotent: a
 * second call is KODE_OK. Returns at once, without waiting for the module nor for the supply.
 * Subscribe to the news before or after.
 * @return KODE_OK; KODE_DENIED when kode_init() has not run, or when the owner's QP/C priority
 *         (CONFIG_MESH_AO_PRIORITY, KODE_PRIO_USER_HIGH by default) is held by another AO or does
 *         not exist in this build (CONFIG_KODE_SCHEDULER): the log names the holder;
 *         KODE_NO_RESOURCES when the owner could not be made.
 */
kode_status_t mesh_sys_begin(void);

/**
 * @brief Let go of the module: the owner takes everything off air, closes the GNSS, hands J3's
 *        lines back to kodeOS and asks the supply off, and looks for no more. It posts the end
 *        to the owner and returns at once, as a command does; MESH_MODULE_LOST_SIG tells it when
 *        a module was held, and status.supply says the rest. mesh_sys_begin() starts it all
 *        again. What you had set up is forgotten.
 * @return KODE_OK; KODE_DENIED before the owner was started (mesh_sys_begin() or mesh_begin()); KODE_NO_RESOURCES when
 *         the owner's queue or the pool had no room.
 */
kode_status_t mesh_sys_end(void);

/* =========================================================================
 * Commands
 * ========================================================================= */

/**
 * @brief Take everything off air - a packet, a test, a search, a listen - and stay configured.
 * @return As the commands' rule above.
 */
kode_status_t mesh_sys_radio_standby(void);

/**
 * @brief Turn the continuous receiver on or off; each packet is MESH_PACKET_RECEIVED_SIG.
 * @param on true to receive.
 * @return As the commands' rule above. The owner refuses it, and says so in its log, while the
 *         radio has no setting or a packet or a test is on air.
 */
kode_status_t mesh_sys_radio_receive(bool on);

/**
 * @brief Listen for a time, then stop: MESH_LISTEN_ENDED_SIG when it is over, and each packet
 *        heard is MESH_PACKET_RECEIVED_SIG.
 * @param ms How long, 1 to MESH_LISTEN_MS_MAX.
 * @return As the commands' rule above; KODE_DENIED also for @p ms out of range.
 */
kode_status_t mesh_sys_radio_listen(uint32_t ms);

/**
 * @brief Stop, now: a packet on air, a test signal, a search and a listen are taken off air, the
 *        receiver is turned off, and what was asked before and not yet taken is dropped. It does
 *        not wait for the owner's queue.
 * @return KODE_OK; KODE_DENIED before the owner was started (mesh_sys_begin() or mesh_begin()). It is never
 *         KODE_NO_RESOURCES.
 */
kode_status_t mesh_sys_stop(void);

/**
 * @brief Say whether the antennas are on: true when the SMA of every band you use carries its
 *        antenna or a 50 ohm load, because the person said so. Until it is true every transmit is
 *        held to the safe power (MESH_REGION_SAFE_DBM_X10, in every build); either answer clears
 *        what an amplifier fault suspected (status.antenna_suspected).
 *
 * It does not wait for the owner's queue, and it is taken with or without the module: the owner
 * puts the new power on a setting in force at its next turn, or just before the next send or test
 * if a packet is on air or arriving then. The SDK keeps it in RAM until mesh_sys_end() or until the
 * module is pulled out; give it after every mesh_sys_begin() and every MESH_MODULE_FOUND_SIG.
 * mesh.h's THE SAFE POWER says the rest.
 * @param fitted true: the antennas (or loads) are on. false takes the confirmation back.
 * @return KODE_OK; KODE_DENIED before the owner was started (mesh_sys_begin() or mesh_begin()). It is
 *         never KODE_NO_RESOURCES.
 */
kode_status_t mesh_sys_radio_antennas_confirm(bool fitted);

/* =========================================================================
 * Requests: the radio
 * ========================================================================= */

/**
 * @brief Set the radio up for a band: configure it and calibrate it. The receiver stays off.
 * @param me               The asking AO; MESH_RADIO_BEGIN_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @param band             The band; the SMA of that band carries an antenna or a load.
 * @param spreading_factor 5 to 12 (7 to 12 unless CONFIG_MESH_LAB).
 * @param power_dbm_x10    The nominal power, tenths of a dBm (85 = 8.5 dBm; mesh_region.h has the
 *                         caps), MESH_POWER_X10_MIN to
 *                         MESH_POWER_X10_MAX. The region's cap holds it (mesh_region.h); status.power_applied_dbm_x10
 *                         says what it came to.
 * @return As the requests' rules above; KODE_DENIED also for @p band or @p power_dbm_x10 out of range.
 *         The answer is KODE_DENIED with MESH_WHY_NOT_ALLOWED for a spreading factor the region does not allow.
 */
kode_status_t mesh_sys_radio_begin(QActive *me, mesh_band_t band, uint8_t spreading_factor, int32_t power_dbm_x10);

/**
 * @brief Set the radio up with a whole setting, and calibrate it.
 * @param me     The asking AO; MESH_RADIO_CONFIGURE_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @param config The setting; copied before this returns.
 * @return As the requests' rules above; KODE_DENIED also for a NULL @p config.
 */
kode_status_t mesh_sys_radio_configure(QActive *me, const mesh_radio_config_t *config);

/**
 * @brief Calibrate the front end for up to MESH_CALIBRATIONS_MAX frequencies; the chip keeps them.
 *        The receiver, if it was on, comes back on.
 * @param me             The asking AO; MESH_RADIO_CALIBRATE_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @param frequencies_hz The frequencies, Hz; copied before this returns.
 * @param count          How many, 1 to MESH_CALIBRATIONS_MAX.
 * @return As the requests' rules above; KODE_DENIED also for a NULL list or @p count out of range.
 */
kode_status_t mesh_sys_radio_calibrate(QActive *me, const uint32_t *frequencies_hz, uint8_t count);

/**
 * @brief Change the setting without calibrating: a frequency the chip was calibrated for.
 * @param me     The asking AO; MESH_RADIO_RETUNE_RSP_SIG is the answer, a mesh_sys_answer_t.
 *               It is KODE_DENIED with MESH_WHY_NOT_CALIBRATED for a frequency no calibration covered, and
 *               with a busy reason while a packet is on air or on its way in.
 * @param config The setting; copied before this returns.
 * @return As the requests' rules above; KODE_DENIED also for a NULL @p config.
 */
kode_status_t mesh_sys_radio_retune(QActive *me, const mesh_radio_config_t *config);

/**
 * @brief Take the radio off air and forget its setting. The receiver goes off, and the chip sleeps
 *        until the radio is begun again (mesh.h's mesh_radio_end() says what that saves).
 * @param me The asking AO; MESH_RADIO_END_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_radio_end(QActive *me);

/**
 * @brief Send one packet.
 * @param me     The asking AO; MESH_RADIO_SEND_RSP_SIG is the answer, a mesh_sys_send_answer_t, posted when
 *               the packet is gone (or refused). The airtime is the radio's: up to ten seconds for the
 *               longest packet, so a send is a request that takes its time; the door of this kind stays shut
 *               until then. A packet on its way in does not hold a send back (the packet that was arriving
 *               is lost): ask mesh_sys_radio_channel_busy() first, or search with mesh_sys_radio_cad().
 * @param data   The payload; copied before this returns.
 * @param length 1 to MESH_PAYLOAD_MAX.
 * @return As the requests' rules above; KODE_DENIED also for a NULL @p data or @p length 0.
 * @note This puts a signal on air. Do it after a person asked for it, and into an antenna or a load.
 */
kode_status_t mesh_sys_radio_send(QActive *me, const uint8_t *data, uint8_t length);

/**
 * @brief Send one packet with a preamble of its own, in symbols.
 * @param me       The asking AO; MESH_RADIO_SEND_PREAMBLE_RSP_SIG is the answer, a mesh_sys_send_answer_t.
 * @param data     The payload; copied before this returns.
 * @param length   1 to MESH_PAYLOAD_MAX.
 * @param preamble Symbols, 1 to 65535 (the region allows MESH_REGION_PREAMBLE unless CONFIG_MESH_LAB: the
 *                 answer is MESH_WHY_NOT_ALLOWED for another).
 * @return As the requests' rules above; KODE_DENIED also for a NULL @p data, @p length or @p preamble 0.
 */
kode_status_t mesh_sys_radio_send_preamble(QActive *me, const uint8_t *data, uint8_t length, uint16_t preamble);

/**
 * @brief Listen for activity on the channel (channel activity detection).
 * @param me      The asking AO; MESH_RADIO_CAD_RSP_SIG is the answer, a mesh_sys_cad_answer_t, posted when
 *                the search ends.
 * @param symbols 0 for the setting's, else 1 to 4.
 * @param fast    true for the shorter search.
 * @return As the requests' rules above; KODE_DENIED also for @p symbols above 4.
 */
kode_status_t mesh_sys_radio_cad(QActive *me, uint8_t symbols, bool fast);

/**
 * @brief Search the channel for a preamble and, when there is one, go on receiving its packet with no
 *        time lost: the chip goes from the search straight to the sync word. mesh_radio_cad_receive()
 *        says what follows.
 * @param me      The asking AO; MESH_RADIO_CAD_RECEIVE_RSP_SIG is the answer, a mesh_sys_cad_answer_t, posted
 *                when the search ends. The owner runs one search at a time, of either kind.
 * @param symbols 0 for the setting's, else 1 to 4.
 * @param fast    true for the shorter search.
 * @return As the requests' rules above; KODE_DENIED also for @p symbols above 4.
 */
kode_status_t mesh_sys_radio_cad_receive(QActive *me, uint8_t symbols, bool fast);

/**
 * @brief Put a test signal on air for a time, for an analyser: into a 50 ohm load only. Only in a
 *        build with CONFIG_MESH_LAB: in any other the answer is KODE_DENIED with MESH_WHY_LAB_ONLY
 *        and the amplifier is never keyed.
 * @param me   The asking AO; MESH_RADIO_TEST_RSP_SIG is the answer, a mesh_sys_answer_t, posted when the
 *             signal is on air; MESH_TEST_ENDED_SIG says it is over.
 * @param mode MESH_TEST_SIGNAL_CARRIER or MESH_TEST_SIGNAL_PREAMBLE.
 * @param ms   1 to MESH_BSP_TEST_MAX_MS.
 * @return As the requests' rules above; KODE_DENIED also for @p mode or @p ms out of range.
 * @note This puts a signal on air. Do it after a person asked for it, into a load, never an antenna.
 */
kode_status_t mesh_sys_radio_test(QActive *me, mesh_test_signal_t mode, uint32_t ms);

/* =========================================================================
 * Requests: the GNSS
 * ========================================================================= */

/**
 * @brief Open the GNSS receiver and listen to it; its fix is mesh_sys_gnss_fix(). One that
 *        mesh_sys_gnss_end() put to sleep is woken with a byte on its UART, from a hot start.
 * @param me The asking AO; MESH_GNSS_BEGIN_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_gnss_begin(QActive *me);

/**
 * @brief Stop listening to the GNSS receiver and put it to sleep with the datasheet's Sleep
 *        command (mesh.h's mesh_gnss_end() says which receivers cannot be told, and are closed).
 * @param me The asking AO; MESH_GNSS_END_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_gnss_end(QActive *me);

/**
 * @brief Restart the GNSS receiver: its fix is lost until it finds one again.
 * @param me The asking AO; MESH_GNSS_RESET_RSP_SIG is the answer, a mesh_sys_answer_t; KODE_DENIED with
 *           MESH_WHY_GNSS_OFF when the GNSS is not open.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_gnss_reset(QActive *me);

/* =========================================================================
 * Requests: the I2C pair
 * ========================================================================= */

/**
 * @brief Take the I2C pair (the GNSS's second face): the SDK lets it go, and its GPIOs are yours.
 * @param me The asking AO; MESH_I2C_LEND_RSP_SIG is the answer, a mesh_sys_i2c_answer_t.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_i2c_lend(QActive *me);

/**
 * @brief Give the I2C pair back.
 * @param me The asking AO; MESH_I2C_RETURN_RSP_SIG is the answer, a mesh_sys_answer_t.
 * @return As the requests' rules above.
 */
kode_status_t mesh_sys_i2c_return(QActive *me);

/* =========================================================================
 * Immediate calls
 * ========================================================================= */

/** @brief Whether a Mesh is on J3 and held. */
bool mesh_sys_is_present(void);

/**
 * @brief A copy of everything the SDK knows about the module, as of its last tick.
 * @param[out] out Where it goes.
 * @return KODE_OK; KODE_DENIED for a NULL @p out.
 */
kode_status_t mesh_sys_status_read(mesh_status_t *out);

/**
 * @brief The J3 position of the module's line at upright position @p pos.
 * @param pos 1 to 14 (the header's position, upright).
 * @return The position; 0 while the module is not there or for @p pos out of range.
 */
uint8_t mesh_sys_header_position(uint8_t pos);

/**
 * @brief A reason in words, in English, valid for ever.
 * @param why An answer's @c why.
 * @return The text; "unknown" for a number that is not a reason.
 */
const char *mesh_sys_why_text(mesh_why_t why);

/**
 * @brief Hear the news: @p me gets MESH_MODULE_FOUND_SIG and the others as they happen. Idempotent.
 *
 * Whichever layer started the owner: an app that called only mesh.h's mesh_begin() gets them too.
 * @param me The AO to tell.
 * @return KODE_OK; KODE_DENIED for a NULL or unstarted @p me; KODE_NO_RESOURCES when MESH_SUBSCRIBERS_MAX are
 *         already subscribed.
 */
kode_status_t mesh_sys_subscribe(QActive *me);

/**
 * @brief Hear no more news.
 * @param me The AO that subscribed.
 * @return KODE_OK; KODE_DENIED for a NULL @p me, or one that had not subscribed.
 */
kode_status_t mesh_sys_unsubscribe(QActive *me);

/**
 * @brief Take the oldest packet received and not yet read.
 * @param[out] rx Where it goes; filled on KODE_OK only.
 * @return KODE_OK; KODE_NO_RESOURCES when none is waiting; KODE_DENIED for a NULL @p rx or before the owner was
 *         started (mesh_sys_begin() or mesh_begin()).
 */
kode_status_t mesh_sys_radio_read(mesh_packet_t *rx);

/**
 * @brief The receiver's RSSI, as the owner last sampled it (every MESH_BSP_RSSI_MS while it receives and
 *        nothing else is going on).
 * @param[out] dbm Where it goes; filled on KODE_OK only.
 * @return KODE_OK; KODE_NO_RESOURCES when the receiver is on and has taken no sample yet; KODE_DENIED when the
 *         receiver is off, there is no module or @p dbm is NULL.
 */
kode_status_t mesh_sys_radio_rssi(float *dbm);

/**
 * @brief Whether a packet is arriving, or a channel search is running: what the status says, at once.
 *        For a search of the channel, ask mesh_sys_radio_cad().
 * @param[out] busy Where it goes; filled on KODE_OK only.
 * @return KODE_OK; KODE_DENIED when the radio has no setting, there is no module or @p busy is NULL.
 */
kode_status_t mesh_sys_radio_channel_busy(bool *busy);

/**
 * @brief How long until the 868 duty budget has room for another packet.
 * @return Milliseconds; 0 when it has room, and always on 2.4 GHz.
 */
uint32_t mesh_sys_radio_send_wait_ms(void);

/**
 * @brief Start a setting for a band: the region's centre frequency, bandwidth, coding rate, preamble, sync word
 *        and power, with SF9.
 * @param band The band.
 * @param[out] out Where it goes.
 * @return KODE_OK; KODE_DENIED for a NULL @p out or @p band out of range.
 */
kode_status_t mesh_sys_radio_config_default(mesh_band_t band, mesh_radio_config_t *out);

/**
 * @brief The GNSS receiver's latest fix, as the owner last read it.
 * @param[out] fix Where it goes; always filled when the GNSS is open, valid or not.
 * @return KODE_OK for a valid fix; KODE_NO_RESOURCES when the GNSS is open and has none yet; KODE_DENIED when it
 *         is not open (mesh_sys_gnss_begin()) or @p fix is NULL.
 */
kode_status_t mesh_sys_gnss_fix(mesh_gnss_fix_t *fix);

/** @brief Whether the GNSS's 1PPS line is high now. */
bool mesh_sys_gnss_pps(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_SYS_H_ */
