/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh.h
 * @brief The Mesh module, for an app's own tasks: a LoRa radio on two bands and a GNSS
 *        receiver, on the Kode Dot's expansion header. The standard layer.
 *
 *     #include "mesh.h"
 *
 *     void app_main(void)
 *     {
 *         if (!mesh_begin()) {                       // kode_init() is done for you
 *             KLOG_E("app", "mesh: %s", mesh_error());
 *             return;
 *         }
 *         if (!mesh_wait_present(3000)) {            // true at once if it is there already
 *             KLOG_W("app", "no Mesh on J3");
 *             return;
 *         }
 *         // Only after the person said the antennas are on, and asked for a send:
 *         mesh_radio_antennas_confirm(true);         // until then every send is held to 0 dBm
 *         mesh_radio_begin(MESH_BAND_SUB_GHZ, 9, 85);    // 868 MHz, SF9, 8.5 dBm
 *         mesh_radio_send((const uint8_t *)"hello", 5);  // returns when the packet is gone
 *
 *         mesh_gnss_begin();                         // start the GNSS too
 *     }
 *
 * This is the standard layer: blocking calls for a task, as kode.h is kodeOS's. An Active
 * Object (AO) uses mesh_sys.h, the extended layer, whose calls never wait: a call here made from an
 * AO is refused, and mesh_error() says "mesh_foo was called from an Active Object (<ao>);
 * mesh.h calls block - call mesh_sys_* instead". Both layers go through the same owner, AO_Mesh
 * (the component mesh_aos), which is the only code that touches the module, so a task may use this
 * header and an AO mesh_sys.h at the same time. Nothing here may be called from an interrupt.
 *
 * A CALL RETURNS A BOOL: true when it was done; false when it was not, and mesh_error() says
 * why in English, for the task that called (a call that fails sets it, a call that goes through
 * clears it; valid until that task's next call to this header). What a call hands back goes
 * through an out parameter, which is filled on true only. The exceptions are five calls that
 * cannot fail: mesh_error() returns the text, mesh_header_position() a position (0 for none),
 * mesh_radio_send_wait_ms() milliseconds, and mesh_is_present() and mesh_gnss_pps() a bool that
 * is the answer itself. Those five, and mesh_radio_config_default(), may be called from an Active
 * Object too. Every call says whether it blocks or returns at once: a call that blocks comes
 * back with the owner's verdict within 2 s (a send, within the packet's airtime; a search, within
 * its length), and a call that finds the owner's queue full asks again every 10 ms until its time
 * is up and then says "busy".
 * A call made at any time, in any state or order - before mesh_begin(), with the module out, after
 * mesh_end(), from an AO, in a loop - answers false and leaves the SDK as it was: a null pointer or
 * a value out of range is false, never a crash and never a value cut to fit.
 *
 * WHY A CALL IS REFUSED. In the app's words (what mesh_error() says, one line each):
 *
 *     the SDK has not begun: call mesh_begin() first
 *     a call that blocks was made from an Active Object
 *     no Mesh module on J3 (not plugged in, or not found yet)
 *     a bad argument: a null pointer, or a value or length out of range
 *     the radio has no setting yet: begin the radio first
 *     a test signal is on air / a packet is on air / a channel search is running /
 *         a timed listen is running / a packet is on its way in
 *     the setting is outside what the region allows (mesh_region.h)
 *     the frequency is not one the radio was calibrated for
 *     the receiver is off, or has taken no sample yet
 *     the duty cycle has no room yet: see mesh_radio_send_wait_ms()
 *     busy: the owner's queue is full / both payload slots are in use
 *     the owner did not answer in time
 *     cut short by a stop, a standby or the module leaving
 *     the GNSS is off / the GNSS has no fix yet / no packet is waiting
 *     a test signal is a lab build's: this build has no CONFIG_MESH_LAB
 *     the module's chip did not do it (a bus or chip error)
 *
 * THE MODULE IS FOUND, NOT ASSUMED. mesh_begin() starts an Active Object, AO_Mesh, that reads the
 * module's fingerprint on J3 every MESH_BSP_LOOK_MS - the four lines its pull resistors hold
 * high, three readings in a row - and, when it matches, opens the LR2021 and asks its version:
 * only a radio that answers is taken, so a Camera or an F*CK with a like fingerprint is not.
 * That same version read is how the SDK knows later that the module is still there. Until it is
 * found, every call that needs the module is refused. This release recognises the module upright
 * only; one plugged in turned round reads as "not a Mesh".
 *
 * EVENTS, BY NAME. What happens without a call arrives as an event, found by its name
 * (mesh_event_add_listener(), mesh_event_wait()). The names, and what value each carries:
 *
 *     module_found       the module was found; 1 (upright)
 *     module_lost        it left J3, or mesh_end() let it go; 0
 *     wrong_module       not a Mesh on J3; a mesh_module_t
 *     setting_lost       the radio's setting, its receiver or the GNSS went away without your
 *                        asking (the module left, the chip would not confirm standby, the
 *                        amplifier faulted, the board is resting, a retune failed); a mask of
 *                        MESH_SETTING_*. If the module left the SDK puts them back when it
 *                        returns; for the rest, begin again
 *     setting_restored   the module returned or the board woke and the SDK put back what you
 *                        had set up; a mask of MESH_SETTING_*. A send or a test is never repeated
 *     packet_received    a packet landed; its length. Read it with mesh_radio_read()
 *     listen_ended       a timed listen (mesh_radio_listen()) ended; 0
 *     radio_failed       the radio went wrong; the chip's error word in the low 16 bits and,
 *                        above them, one of MESH_RADIO_ERROR_*. With MESH_RADIO_ERROR_AMPLIFIER_FAULT
 *                        that band is also held to the safe power until you confirm the antennas
 *                        again (status.antenna_suspected): ask the person whether one is loose
 *     test_ended         a test signal ended without being stopped; a MESH_TEST_END_*
 *     preamble_detected  a preamble was detected: a packet may follow; 0. The receiver may take
 *                        noise for one; then nothing more is said and status.arriving falls by itself
 *     header_received    its header came in valid; 0
 *     header_failed      its header failed: no packet follows; 0
 *
 * Listeners run on a task of the SDK (mesh_events), one at a time and in order, never on the
 * owner's task; inside one, any call of this header is fine except mesh_event_wait(). The owner
 * hands events to that task through a queue of 16 without ever waiting for it: when a listener
 * is too slow and the queue is full, the event is lost and counted in status.events_dropped. Only
 * an event that arrives after mesh_event_wait() began counts. The task is made by the first
 * mesh_event_add_listener() or mesh_event_wait().
 *
 * WHAT SURVIVES. What the SDK keeps when the module goes, the board rests or goes home, or the
 * app ends it:
 *
 *                              module pulled out      rest (sleep)           home, mesh_end()
 *     radio setting            lost; put back when    kept; put back when    forgotten
 *     (begin, configure,       it returns             the board wakes
 *     retune)                  (setting_lost, then    (setting_restored)
 *                              setting_restored)
 *     the receiver             as the setting         as the setting         forgotten
 *     the GNSS                 closed; reopened       asleep; woken when     asleep (mesh_end():
 *                              when it returns        the board wakes        then its supply goes)
 *     calibrate() list         lost: the chip         lost with the chip's   lost
 *                              forgot it              sleep; the setting is
 *                                                     calibrated again
 *     a packet on air, a       ended                  ended                  ended
 *     test, a search
 *     received packets         dropped                kept                   dropped
 *     the I2C pair lent        handed back (lend      kept                   handed back
 *                              it again)
 *     the 868 duty budget      kept (an unplug does   kept                   kept (mesh_begin()
 *                              not refund airtime)                           does not refill it)
 *     the listeners            kept                   kept                   kept
 *     the antennas confirmed   forgotten: the safe    kept                   forgotten: the safe
 *     (and a band suspected)   power until you                               power until you
 *                              confirm again                                 confirm again
 *
 * A rest is only with CONFIG_KODE_LOWPOWER: when the board goes to rest the SDK takes everything
 * off air, turns the receiver off, puts the GNSS to sleep (mesh_gnss_end()'s Sleep command) and
 * puts the radio to sleep with its reset line high (or, if the chip will not confirm standby, holds it
 * in reset); when the board wakes it puts the setting, the receiver and the GNSS back
 * (setting_restored, the GNSS woken by a byte; one that cannot be told to sleep - not the
 * ME31GR02 at 115200 - is closed instead and opened again), and only those the
 * rest took: what you ended yourself stays ended. Nothing is sent, and no test repeated. Home ends it
 * all, as mesh_end() does. A module that comes back is found again by its fingerprint; module_lost and
 * module_found say so. A module pulled out while its radio sleeps is noticed as an awake one is (the
 * alive check wakes the chip for its question); one pulled out while its radio is held in reset, which
 * nobody can ask, is noticed at the next mesh_radio_begin().
 *
 * TWO RADIOS IN ONE CHIP. The LR2021 has a sub-GHz transmitter on the module's SMA J501 and a 2.4 GHz
 * one on J502, each with its own amplifier and no switch between them. mesh_radio_begin() chooses the
 * band; a send or a receive uses whichever was chosen. Only LoRa is spoken.
 *
 * >>> THE AMPLIFIERS ARE GUARANTEED ONLY INTO A LOAD. The SMA for the band in use carries its
 *     antenna, or a 50 ohm load, before any transmit - mesh_radio_send(), mesh_radio_test().
 *     Transmitting into an open connector can damage the amplifier. <<<
 *
 * THE SAFE POWER UNTIL THE ANTENNAS ARE CONFIRMED. The module ships with its antennas loose, and the
 * chip cannot tell that one is missing. So until you say they are on, with
 * mesh_radio_antennas_confirm(true) after the person has said so, every transmit on both bands is held
 * to the safe power, MESH_REGION_SAFE_DBM_X10 (0 dBm unless CONFIG_MESH_SAFE_POWER_CDBM says otherwise),
 * in every build, CONFIG_MESH_LAB included. status.power_capped says when the setting in force goes out
 * below what you asked because of it. If a band's amplifier then trips its over-current alarm, the radio
 * goes off as it always did (radio_failed, MESH_RADIO_ERROR_AMPLIFIER_FAULT) and that band is held to the
 * safe power again (status.antenna_suspected) until you confirm once more: ask the person whether an
 * antenna is loose. The SDK never begins the radio again by itself. The SDK keeps your answer in RAM
 * only, and a module pulled out takes it with it: give it after every mesh_begin() and every
 * module_found.
 *
 * >>> A TEST SIGNAL IS A LAB BUILD'S, AND GOES INTO A 50 OHM LOAD ONLY, NEVER AN ANTENNA.
 *     mesh_radio_test() exists only in a build with CONFIG_MESH_LAB; in any other it is refused ("a test
 *     signal is a lab build's") and nothing is keyed. It keys the amplifier for as long as it is asked
 *     (at most MESH_BSP_TEST_MAX_MS), 100 % of the time on air, and the duty budget does not count it:
 *     on an antenna that is a hundred times the 868 band's 1 % limit. The SDK
 *     takes the signal off air at its end, on mesh_stop(), mesh_radio_standby() and when the app
 *     restarts or goes home; a crash or a watchdog reset runs none of that, and a carrier then stays up
 *     until the module loses power. <<<
 *
 * WITHIN THE RULES. Unless the build sets CONFIG_MESH_LAB (whose Kconfig help says what it lifts, and
 * which is not for a firmware that is distributed; the safe power above is not among it), every transmit is
 * held to the region's limits
 * (mesh_region.h): the power is capped (8.5 dBm nominal on 868 MHz and 4.5 dBm on 2.4 GHz),
 * and on the 868 sub-band the time on air is held to 1 % over a long
 * run by a budget that refills with time - mesh_radio_send() refuses a packet it has no budget for
 * and mesh_radio_send_wait_ms() says how long to wait. The budget holds 36 s, starts full and gets
 * 36 s back per hour: from full, up to 72 s can go out in the first hour (2 % of it), and 36 s in
 * every hour after. It lives in RAM, so an app that restarts starts with it full: the SDK does not
 * remember what the app sent before. An app that needs "36 s in any hour", or that restarts often,
 * counts its own airtime on top. The numbers live in mesh_region.h, the one place they
 * change.
 *
 * Nothing here sends, emits or records by itself: a packet goes on air only when you call for it.
 *
 * WHAT IS LEFT FOR THE APP. The I2C pair is the GNSS's second face; this SDK never masters
 * it - the stock GNSS firmware drives SDA push-pull, so mastering it fights the receiver - but an app
 * may take it with mesh_i2c_lend() and drive its own bus, at its own risk. No other J3 line is free:
 * the module uses all fourteen positions, and while it is there the SDK holds every one it can drive,
 * so kode-sdk's kode_pin_*() and kode_sys_exp_*() refuse them (they are lent to "mesh"). mesh_header_position() says which J3
 * position one of the module's lines is on.
 *
 * J3's SUPPLY. +3.3V_EXT's state when an app starts is not promised (kode-sdk's kode.h says so).
 * mesh_begin() asks it on - it needs CONFIG_KODEOS_RADIO=y, as the pin is the C5's - and learns
 * whether it came on from kodeOS's EXP_POWER_SIG; status.supply says. mesh_end() asks it off. Without the radio (CONFIG_KODEOS_RADIO=n)
 * J3 is always powered: the module is then found as soon as it is plugged, and mesh_end() cannot
 * switch it off.
 */
#ifndef MESH_H_
#define MESH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_bsp.h"
#include "mesh_region.h"   /* the limits an app may read */
#include "mesh_types.h"    /* the bands, a setting, a packet, a fix, the status, an event */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The SDK's version: the release this tree is, vMAJOR.MINOR.PATCH ---------------- */

#define MESH_VERSION_MAJOR 0U /**< Major version of the Mesh SDK. */
#define MESH_VERSION_MINOR 1U /**< Minor version of the Mesh SDK. */
#define MESH_VERSION_PATCH 0U /**< Patch version of the Mesh SDK. */

/* ---- Why a call was refused ------------------------------------------------------ */

/**
 * @brief Why this task's last call to this header failed, in English.
 *
 * A call that fails sets it, a call that goes through clears it. Valid until this task's next
 * call to this header.
 *
 * The SDK remembers the reason of the last FOUR tasks that called this header. A fifth task takes
 * the place of the one that came first, which reads "" from then on until its next call. An app
 * that calls mesh.h from more than four tasks reads mesh_error() right after the call that failed,
 * on the same task, and keeps the text if it needs it later.
 * Returns at once.
 * @return Short text; "" when the last call went through, or when this task's reason gave way to
 *         a fifth task's. Never NULL.
 */
const char *mesh_error(void);

/* ---- Begin and end ---------------------------------------------------------------- */

/**
 * @brief Start the SDK: bring kodeOS up (kode_init()), start AO_Mesh, ask J3's supply on and begin
 *        looking for the module.
 *
 * Blocks until the owner runs, not until it finds the module (mesh_wait_present()). Idempotent.
 * @return true; false when called from an Active Object, or when the owner's QP/C priority
 *         (CONFIG_MESH_AO_PRIORITY, KODE_PRIO_USER_HIGH by default) is held by another AO or does
 *         not exist in this build (the log names the holder), or the owner could not be made.
 */
bool mesh_begin(void);

/**
 * @brief Let go of the module: everything off air, the GNSS closed, J3's lines handed back, the
 *        supply asked off, and no more looking. mesh_begin() starts it again. What you had set up is
 *        forgotten; the listeners are kept.
 *
 * Blocks until the owner confirms (2 s at most). With nothing running it is true.
 * @return true; false from an Active Object, or when the owner did not answer in time.
 */
bool mesh_end(void);

/* ---- The module ------------------------------------------------------------------- */

/**
 * @brief Whether a Mesh is on J3 and held. Returns at once.
 * @return true when it is; false when not, and before mesh_begin().
 */
bool mesh_is_present(void);

/**
 * @brief Wait for the module to be found.
 *
 * Blocks up to @p ms; true at once if it is there already.
 * @param ms The longest to wait.
 * @return true when the module is held; false when it was not found in time ("no Mesh module on J3"),
 *         before mesh_begin(), or from an Active Object.
 */
bool mesh_wait_present(uint32_t ms);

/**
 * @brief A copy of everything the SDK knows about the module, as of the owner's last tick.
 *
 * Returns at once.
 * @param[out] out Where it goes; filled on true only.
 * @return true; false for a NULL @p out or before mesh_begin().
 */
bool mesh_status(mesh_status_t *out);

/**
 * @brief Stop, now: a packet on air, a test signal, a search and a listen are taken off air, the
 *        receiver is turned off, and what was asked before and not yet taken is dropped.
 *
 * Returns at once, without waiting for the owner's queue: the owner does it on its next turn. A
 * caller that must know the radio is silent calls mesh_radio_standby(), which waits.
 * @return true; false before mesh_begin().
 */
bool mesh_stop(void);

/**
 * @brief The J3 position of the module's line at upright position @p pos. Returns at once.
 * @param pos 1 to 14.
 * @return The position; 0 while the module is not there or for @p pos out of range.
 */
uint8_t mesh_header_position(uint8_t pos);

/* ---- Events ----------------------------------------------------------------------- */

/**
 * @brief Call @p fn every time the event @p event happens (the names are in the table above).
 *        Adding the same pair twice registers it once.
 *
 * Returns at once; the first call makes the events task.
 * @param event An event's name, up to MESH_EVENT_NAME_MAX - 1 characters.
 * @param fn    The listener; it runs on the SDK's task `mesh_events`, so keep it short.
 * @return true when registered; false for an unknown name, a NULL @p fn, MESH_LISTENERS_MAX
 *         already registered, or when the events task could not be made.
 */
bool mesh_event_add_listener(const char *event, mesh_listener_t fn);

/**
 * @brief Stop calling @p fn for @p event. Returns at once.
 * @return true when it was registered; false otherwise.
 */
bool mesh_event_remove_listener(const char *event, mesh_listener_t fn);

/**
 * @brief Wait for the event @p event: only one that arrives after the wait began counts.
 *
 * Blocks up to @p ms.
 * @param event An event's name.
 * @param ms    The longest to wait; 0 returns false at once.
 * @param[out] out Receives a copy of the event; its strings are valid until this task's next wait.
 *                 May be NULL. Filled on true only.
 * @return true when it happened; false on a timeout, an unknown name, from an Active Object or from
 *         a listener, or when four tasks are already waiting.
 */
bool mesh_event_wait(const char *event, uint32_t ms, mesh_event_t *out);

/* ---- The radio -------------------------------------------------------------------- */

/**
 * @brief Say whether the antennas are on: true when the SMA of every band you use carries its antenna or
 *        a 50 ohm load, because the person said so. Until it is true every transmit is held to the safe
 *        power (MESH_REGION_SAFE_DBM_X10); either answer clears what an amplifier fault suspected.
 *
 * Returns at once, with or without the module: the owner puts the new power on a setting in force at its
 * next turn (only the amplifier: no calibration), or just before the next send or test if a packet is on
 * air or arriving then. The SDK keeps it in RAM until mesh_end() or until the module is pulled out; the
 * app keeps what the person said, and gives it after every mesh_begin() and every module_found.
 * @param fitted true: the antennas (or loads) are on. false takes the confirmation back.
 * @return true; false before mesh_begin(), or from an Active Object (mesh_sys_radio_antennas_confirm()).
 */
bool mesh_radio_antennas_confirm(bool fitted);

/**
 * @brief Set the radio up for a band: configure it and calibrate it. The receiver stays off.
 *
 * Blocks (under a second: the calibration is the longest).
 * @param band             The band; the SMA of that band carries an antenna or a load.
 * @param spreading_factor 5 to 12 (7 to 12 unless CONFIG_MESH_LAB).
 * @param power_dbm_x10    The nominal power, tenths of a dBm (85 = 8.5 dBm; mesh_region.h has the
 *                         caps), MESH_POWER_X10_MIN to
 *                         MESH_POWER_X10_MAX. The region's cap holds it (mesh_region.h), and the safe power
 *                         until the antennas are confirmed (mesh_radio_antennas_confirm()); status.
 *                         power_applied_dbm_x10 says what it came to.
 * @return true; false for @p band or @p power_dbm_x10 out of range, a spreading factor the region does not
 *         allow, a module not there, a packet or a test on air, or a chip that refused.
 */
bool mesh_radio_begin(mesh_band_t band, uint8_t spreading_factor, int32_t power_dbm_x10);

/**
 * @brief Start a setting for a band: the region's centre frequency, bandwidth, coding rate, preamble,
 *        sync word and power, with SF9. Returns at once.
 * @param band The band.
 * @param[out] out Where it goes; filled on true only.
 * @return true; false for a NULL @p out or @p band out of range.
 */
bool mesh_radio_config_default(mesh_band_t band, mesh_radio_config_t *out);

/**
 * @brief Set the radio up with a whole setting, and calibrate it.
 *
 * Blocks. Without CONFIG_MESH_LAB the setting is the region's (mesh_region.h) to the field: frequency
 * inside the band's edges, the band's bandwidth, SF7 to SF12, coding rate 4/5, eight preamble symbols and
 * the private sync word; one outside is refused ("the setting is outside what the region allows") and the
 * one in force stays.
 * @param config The setting.
 * @return true; false for a NULL @p config or a power outside the window, a setting refused, no module, or
 *         a chip that refused.
 */
bool mesh_radio_configure(const mesh_radio_config_t *config);

/**
 * @brief Calibrate the front end for up to MESH_CALIBRATIONS_MAX frequencies; the chip keeps them. The
 *        receiver, if it was on, comes back on.
 *
 * Blocks.
 * @param frequencies_hz The frequencies, Hz.
 * @param count          How many, 1 to MESH_CALIBRATIONS_MAX.
 * @return true; false for a NULL list or @p count out of range, a frequency the region does not allow, no
 *         radio setting, or a chip that refused.
 */
bool mesh_radio_calibrate(const uint32_t *frequencies_hz, uint8_t count);

/**
 * @brief Change the setting without calibrating: to a frequency the chip was calibrated for.
 *
 * Blocks, briefly: there is no calibration, so it is far quicker than mesh_radio_begin(). The
 * receiver, if it was on, stays on.
 * @param config The setting.
 * @return true; false for a NULL @p config, a frequency no calibration covered, a setting refused, no
 *         radio setting, a packet on air or on its way in (ask again), or a chip that refused (then no
 *         setting is in force: begin again).
 */
bool mesh_radio_retune(const mesh_radio_config_t *config);

/**
 * @brief Take the radio off air and forget its setting. The receiver goes off. Blocks.
 *
 * With nothing to do, the chip is put to sleep (its SetSleep, no retention): 0.47 uA instead of the
 * 1.21 mA of the standby it was left in before (LR20xx datasheet, Table 3-4); status.radio_asleep
 * says so. The next mesh_radio_begin() or mesh_radio_configure() wakes it with a reset, which costs
 * it nothing it still had, and calibrates it again. A module found and not yet begun sleeps the same
 * way. mesh_radio_standby() and turning the receiver off keep the setting, and the chip awake.
 * @return true; false with no module.
 */
bool mesh_radio_end(void);

/**
 * @brief Take everything off air - a packet, a test, a search, a listen - and stay configured. Blocks
 *        until the chip confirms.
 * @return true; false with no module or when the chip refused.
 */
bool mesh_radio_standby(void);

/**
 * @brief Send one packet.
 *
 * Blocks until the packet is gone: the time it takes on air, up to ten seconds for the longest. A task that
 * wants to do something else meanwhile sends from a task of its own. Calls from other tasks are not held
 * up (the receiver, the status, a stop); a second send is refused ("a packet is on air").
 *
 * A packet on its way in does NOT hold a send back: the radio leaves the receiver, and the packet that
 * was arriving is lost. An app that must not talk over someone asks mesh_radio_channel_busy() first, or
 * searches the channel with mesh_radio_cad().
 * @param data   The payload; read before this returns and never after.
 * @param length 1 to MESH_PAYLOAD_MAX.
 * @return true when the chip said the packet left whole; false for a NULL @p data or @p length 0, no
 *         module, no radio setting, a test or a search on air, a packet already on air,
 *         no room in the duty budget (mesh_error() says so; mesh_radio_send_wait_ms() how long), the
 *         packet cut short by mesh_stop(), mesh_radio_standby() or the module leaving, or a chip that
 *         never said it was done (it may not have gone whole).
 * @note This puts a signal on air. Do it after a person asked for it, and into an antenna or a load.
 */
bool mesh_radio_send(const uint8_t *data, uint8_t length);

/**
 * @brief Send one packet with a preamble of its own, in symbols. As mesh_radio_send().
 *
 * The receiver listens for a preamble of any length, whatever the setting sends, so another Dot on this
 * SDK hears the packet too.
 * @param data     The payload.
 * @param length   1 to MESH_PAYLOAD_MAX.
 * @param preamble Symbols, 1 to 65535 (the region allows MESH_REGION_PREAMBLE unless CONFIG_MESH_LAB).
 * @return As mesh_radio_send(), and false for @p preamble 0 or one the region does not allow.
 */
bool mesh_radio_send_preamble(const uint8_t *data, uint8_t length, uint16_t preamble);

/**
 * @brief How long until the 868 duty budget has room for another packet. Returns at once.
 * @return Milliseconds; 0 when it has room, and always on 2.4 GHz.
 */
uint32_t mesh_radio_send_wait_ms(void);

/**
 * @brief Turn the continuous receiver on or off; each packet is the event packet_received.
 *
 * Blocks. On while a packet is on air or a search runs, it comes on when that ends.
 * @param on true to receive.
 * @return true; false with no module, no radio setting, or a test on air.
 */
bool mesh_radio_receive(bool on);

/**
 * @brief Take the oldest packet received and not yet read. Returns at once.
 * @param[out] rx Where it goes; filled on true only.
 * @return true; false for a NULL @p rx, before mesh_begin(), or when none is waiting ("no packet is waiting").
 */
bool mesh_radio_read(mesh_packet_t *rx);

/**
 * @brief The receiver's RSSI, as the owner last sampled it: every MESH_BSP_RSSI_MS while the receiver is on
 *        and nothing else is going on. Returns at once.
 * @param[out] dbm Where it goes; filled on true only.
 * @return true; false for a NULL @p dbm, the receiver off, or no sample taken yet.
 */
bool mesh_radio_rssi(float *dbm);

/**
 * @brief Whether a packet is arriving or a channel search is running: what the status says at once. For a
 *        search of the channel, call mesh_radio_cad(). Returns at once.
 * @param[out] busy Where it goes; filled on true only.
 * @return true; false for a NULL @p busy, no module or no radio setting.
 */
bool mesh_radio_channel_busy(bool *busy);

/**
 * @brief Listen for activity on the channel (channel activity detection).
 *
 * Blocks until the search ends, a few symbols. A packet already arriving answers true with @p detected
 * set, without a search.
 * @param symbols  0 for the setting's, else 1 to 4.
 * @param fast     true for the shorter search.
 * @param[out] detected Where the answer goes: true when the search saw activity. May be NULL. Filled on
 *                      true only.
 * @return true when the search ended; false for @p symbols above 4, no module or radio setting, anything
 *         else on air, or a search cut short.
 */
bool mesh_radio_cad(uint8_t symbols, bool fast, bool *detected);

/**
 * @brief Search the channel for a preamble and, when there is one, go on receiving its packet with
 *        no time lost: the chip goes from the search straight to the sync word, without detecting the
 *        preamble again. For an app that visits several settings in turn and has a few symbols on each.
 *
 * Blocks until the search ends, a few symbols. A packet already arriving answers true with @p detected
 * set, without a search. With @p detected the packet comes as the events preamble_detected (at once),
 * header_received and packet_received, or header_failed; the receiver is then as it was before, on
 * if mesh_radio_receive() had it on. With nothing seen it is that way already.
 * @param symbols  0 for the setting's, else 1 to 4.
 * @param fast     true for the shorter search.
 * @param[out] detected Where the answer goes: true when the search saw a preamble. May be NULL. Filled
 *                      on true only.
 * @return true when the search ended; false for @p symbols above 4, no module or radio setting, anything
 *         else on air, or a search cut short.
 */
bool mesh_radio_cad_receive(uint8_t symbols, bool fast, bool *detected);

/**
 * @brief Listen for one packet, for a time at most, then stop: the packet heard is the event
 *        packet_received, and the event listen_ended says the listen is over, right after the packet
 *        or when the time is up with none. A packet that comes in bad ends it too. A listen you cut
 *        short yourself (a send, mesh_radio_receive(), a standby, mesh_stop()) ends with no event.
 *
 * Blocks until the chip has begun, not until the time is up.
 * @param ms How long, 1 to MESH_LISTEN_MS_MAX.
 * @return true; false for @p ms out of range, no module or radio setting, or anything else on air.
 */
bool mesh_radio_listen(uint32_t ms);

/**
 * @brief Put a test signal on air for a time, for an analyser: into a 50 ohm load only. Only in a
 *        build with CONFIG_MESH_LAB.
 *
 * A carrier or a bare preamble is on air all the time it lasts and is not counted in the duty
 * budget, so a build that keeps the region's limits does not have it: there this returns false,
 * mesh_error() says "a test signal is a lab build's: this build has no CONFIG_MESH_LAB", and
 * the amplifier is never keyed.
 *
 * Blocks until the signal is on air; the event test_ended says it is over.
 * @param mode MESH_TEST_SIGNAL_CARRIER or MESH_TEST_SIGNAL_PREAMBLE.
 * @param ms   1 to MESH_BSP_TEST_MAX_MS.
 * @return true; false in a build without CONFIG_MESH_LAB, for @p mode or @p ms out of range, no module or
 *         radio setting, a packet or a test already on air, or no protection to hold the radio in reset on
 *         a restart.
 * @note This puts a signal on air. Do it after a person asked for it, into a load, never an antenna.
 */
bool mesh_radio_test(mesh_test_signal_t mode, uint32_t ms);

/* ---- The GNSS --------------------------------------------------------------------- */

/**
 * @brief Open the GNSS receiver and listen to it; its fix is mesh_gnss_fix(). Blocks.
 *
 * A receiver mesh_gnss_end() put to sleep is woken by a byte on its UART, with no reset: it comes
 * back from a hot start (about 1 s to a fix, ME31GR02 datasheet p.4, with the sky it had). The
 * fix is empty until it reports again. One that does not talk again within a few seconds is
 * sent the byte again and, at the last try, restarted with its reset line.
 * @return true; false with no module or when the receiver would not open.
 */
bool mesh_gnss_begin(void);

/**
 * @brief Stop listening to the GNSS receiver, and put it to sleep. Blocks.
 *
 * A receiver that runs draws about 17.5 mA whether it is read or not (datasheet p.7). This sends
 * it the datasheet's Sleep command (p.14): its core and most of its radio stop, its backup RAM
 * stays powered, and mesh_gnss_begin() wakes it. Its fix is forgotten (mesh_gnss_fix() says
 * the GNSS is off) and status.gnss_asleep is true. A receiver that has not talked at
 * MESH_BSP_GNSS_BAUD - one at 9600 baud, or one that never spoke - cannot be told and is closed,
 * still running; so is one that goes on talking after the command (the log says so).
 * @return true; false with no module.
 */
bool mesh_gnss_end(void);

/**
 * @brief Restart the GNSS receiver: its fix is lost until it finds one again. Blocks.
 * @return true; false with no module or the GNSS off.
 */
bool mesh_gnss_reset(void);

/**
 * @brief The GNSS receiver's latest fix, as the owner last read it. Returns at once.
 * @param[out] fix Where it goes; filled on true only (mesh_status() has the fix in any case).
 * @return true for a valid fix; false for a NULL @p fix, the GNSS off, or no fix yet.
 */
bool mesh_gnss_fix(mesh_gnss_fix_t *fix);

/** @brief Whether the GNSS's 1PPS line is high now. Returns at once. */
bool mesh_gnss_pps(void);

/* ---- The I2C pair ----------------------------------------------------------------- */

/**
 * @brief Take the I2C pair (the GNSS's second face): the SDK lets it go, and its GPIOs are yours. Blocks.
 * @param[out] scl_gpio The ESP32-P4 GPIO of SCL; may be NULL if @p sda_gpio is not. Filled on true only.
 * @param[out] sda_gpio The GPIO of SDA. May be NULL if @p scl_gpio is not.
 * @return true; false for both NULL, no module, or the pair already lent.
 */
bool mesh_i2c_lend(uint8_t *scl_gpio, uint8_t *sda_gpio);

/** @brief Give the I2C pair back. Blocks. @return true; false with no module. */
bool mesh_i2c_return(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_H_ */
