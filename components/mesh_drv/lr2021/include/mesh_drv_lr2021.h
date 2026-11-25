/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_lr2021.h
 * @brief The Mesh module's radio: a Semtech LR2021 inside a NiceRF LoRa2021.
 *
 * One chip, two transmitters: a sub-GHz path on the module's SMA J501 and a
 * 2.4 GHz path on J502, each with its own power amplifier and no antenna
 * switch between them. The module carries no microcontroller, so the P4
 * drives the chip itself over SPI: a 16-bit opcode and its parameters in one
 * chip select, an answer in a second frame of zeros whose first two bytes are
 * the status word, and a DIO line that rises when something the host asked to
 * hear about happened. The receive FIFO is the one frame that carries its data
 * right after the opcode, with no status word.
 *
 * This is the chip and nothing above it: which frequency, which spreading
 * factor and how loud come in as a mesh_drv_lora_settings_t (mesh_plan.c has built it to
 * the region's limits), and so do the pins, which are the header's and arrive
 * as GPIO numbers from whoever borrowed them. Only LoRa is spoken.
 *
 * **Every call here blocks** for as long as the chip keeps BUSY high, so none
 * may be made from an Active Object, and they are not reentrant: one task owns
 * the radio.
 *
 * >>> The amplifiers are guaranteed only into a load. Both SMA connectors
 *     carry their antenna, or a 50 ohm load, before anything here transmits. <<<
 */
#ifndef MESH_DRV_LR2021_H_
#define MESH_DRV_LR2021_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "mesh_drv_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The GPIOs the chip is wired to. */
typedef struct {
    uint8_t spi_host;    /**< The SPI controller, IDF's spi_host_device_t, as
                          *   kode_sys_exp_ctrl() gave it.                   */
    uint8_t chip_select; /**< SPI chip select, active low.                   */
    uint8_t clock;       /**< SPI clock.                                     */
    uint8_t mosi;        /**< SPI data to the chip.                          */
    uint8_t miso;        /**< SPI data from the chip.                        */
    uint8_t busy;        /**< High while the chip cannot take a command.     */
    uint8_t reset;       /**< Reset, active low.                             */
    uint8_t dio9;        /**< The interrupt line, high while a flag is pending. */
} mesh_drv_lr2021_pins_t;

/**
 * @name Interrupt flags
 * @brief The bits of the chip's interrupt word this driver routes to DIO9.
 * @{
 */
#define MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT   (1UL << 11)  /**< Amplifier over-current or
                                                *   over-voltage. The chip stays
                                                *   in Tx: this only says so. */
#define MESH_DRV_LR2021_INTERRUPT_ERROR      (1UL << 16)  /**< A flag in GetErrors.    */
#define MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR  (1UL << 17)  /**< A command was refused.  */
#define MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE    (1UL << 18)  /**< A packet was received.  */
#define MESH_DRV_LR2021_INTERRUPT_SEND_DONE    (1UL << 19)  /**< A packet left whole.    */
#define MESH_DRV_LR2021_INTERRUPT_TIMEOUT    (1UL << 21)  /**< The Rx or Tx timer ran
                                                *   out.                    */
#define MESH_DRV_LR2021_INTERRUPT_CRC_ERROR  (1UL << 22)  /**< A packet came in with a
                                                *   wrong CRC. The datasheet
                                                *   does not say whether
                                                *   RX_DONE comes with it (it
                                                *   does say so for a length
                                                *   error). The owner serves
                                                *   both: with RX_DONE the
                                                *   packet is delivered, marked
                                                *   bad; alone, the FIFO is
                                                *   emptied.                 */
#define MESH_DRV_LR2021_INTERRUPT_PREAMBLE     (1UL << 5)  /**< A preamble was detected.  */
#define MESH_DRV_LR2021_INTERRUPT_HEADER_VALID (1UL << 6)  /**< Its header came in valid. */
#define MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED (1UL << 7)  /**< A CAD saw activity. It can
                                                *   come more than once before
                                                *   CAD_DONE (6.3.12).      */
#define MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR (1UL << 9)  /**< Its header's CRC failed: no
                                                *   packet follows.         */
#define MESH_DRV_LR2021_INTERRUPT_CAD_DONE     (1UL << 20) /**< A CAD ended.              */
/** @} */

/** @brief What the chip's status word says it is doing. */
typedef enum {
    MESH_DRV_LR2021_MODE_SLEEP = 0,
    MESH_DRV_LR2021_MODE_STANDBY_RC,     /**< Idle on the RC oscillator.         */
    MESH_DRV_LR2021_MODE_STANDBY_XOSC,   /**< Idle with the crystal running.     */
    MESH_DRV_LR2021_MODE_FREQUENCY_SYNTHESIS,           /**< Synthesiser locked, nothing on air. */
    MESH_DRV_LR2021_MODE_RECEIVE,           /**< Receiving.                         */
    MESH_DRV_LR2021_MODE_TRANSMIT            /**< Transmitting.                      */
} mesh_drv_lr2021_mode_t;

/** @brief The transmitter's test signals; the values are SetTxTestMode's. */
typedef enum {
    MESH_DRV_LR2021_TEST_PREAMBLE = 0x01,  /**< A bare LoRa preamble, as long as the
                                       *   test lasts. Sent as a real packet
                                       *   under SetTx's timeout, so the chip's
                                       *   own timer ends it.                 */
    MESH_DRV_LR2021_TEST_CARRIER  = 0x02   /**< An unmodulated carrier. The chip has
                                       *   no timeout for it.                 */
} mesh_drv_lr2021_test_t;

/** @brief A received packet, as mesh_drv_lr2021_read() gives it. */
typedef struct {
    uint8_t len;        /**< Bytes received into the caller's buffer.      */
    bool    crc_ok;     /**< The packet carried a CRC and it matched: false for
                         *   a wrong CRC, and also for a packet sent without
                         *   one, which has nothing to vouch for it.        */
    float   rssi_dbm;   /**< The packet's RSSI.                            */
    float   snr_db;     /**< Its signal-to-noise ratio.                    */
} mesh_drv_lr2021_packet_t;

/** @brief The longest LoRa payload one packet carries. */
#define MESH_DRV_LR2021_PAYLOAD_MAX   MESH_DRV_LORA_PAYLOAD_MAX

/**
 * @brief Called when DIO9 rises. Runs in interrupt context.
 *
 * The line is level-triggered and its interrupt is masked before this runs,
 * so nothing more arrives until mesh_drv_lr2021_interrupts_take() has read the flags.
 *
 * @param arg What mesh_drv_lr2021_open() was given.
 */
typedef void (*mesh_drv_lr2021_interrupt_callback_t)(void *arg);

/**
 * @brief Take the pins: the SPI bus, the reset and BUSY lines and DIO9.
 *
 * Nothing is sent. BUSY is read through a pull-up and MISO and DIO9 through
 * pull-downs, so a header with no module on it reads as a chip that never
 * becomes ready rather than as whatever the lines float to.
 *
 * @param pins   The GPIOs; copied.
 * @param on_irq Told when DIO9 rises. Required.
 * @param arg    Handed back to @p on_irq untouched.
 * @return KODE_OK; KODE_DENIED for a NULL argument or a driver already open;
 *         KODE_ERROR when the SPI controller or a pin would not come up.
 */
kode_status_t mesh_drv_lr2021_open(const mesh_drv_lr2021_pins_t *pins,
                              mesh_drv_lr2021_interrupt_callback_t on_irq, void *arg);

/** @brief Give the pins back, high-Z. Closing a closed driver does nothing. */
void mesh_drv_lr2021_close(void);

/**
 * @brief Pulse the reset line, wait for the chip, and ask what it is.
 *
 * A reset leaves the chip in standby with every setting at its default and
 * its receiver uncalibrated. One that gets no answer logs the step it stopped
 * at and what the lines read there.
 *
 * @param[out] major The firmware's major number: 0x01 on an LR2021.
 * @param[out] minor Its minor number: 0x18.
 * @return KODE_OK when the chip answered; KODE_TIMEOUT when BUSY never fell,
 *         which is what an empty header looks like; KODE_IO_ERROR when a frame
 *         would not go out or the answer was all zeros or all ones.
 */
kode_status_t mesh_drv_lr2021_reset(uint8_t *major, uint8_t *minor);

/**
 * @brief Set the chip up for LoRa as @p cfg says, in standby, nothing on air.
 *
 * Frequency, receive path, modulation, packet shape (the receiver's: the whole
 * payload and a preamble of any length; the setting's sync word, an explicit
 * header and a CRC), the amplifier and its power, and
 * DIO9 as the interrupt line. The owner's plan built @p cfg to the limits of this
 * build; this driver does not second-guess it, but it does refuse a setting
 * the chip itself cannot take.
 *
 * @return KODE_OK; KODE_DENIED for a NULL or out-of-range setting;
 *         KODE_TIMEOUT when BUSY stuck; KODE_IO_ERROR when the chip refused or
 *         raised an error flag, logged by name.
 */
kode_status_t mesh_drv_lr2021_configure(const mesh_drv_lora_settings_t *cfg);

/**
 * @brief Calibrate the receiver's front end for the frequency and path set.
 *
 * The chip does none by itself after a reset, and receives worse without it.
 * Call in standby, after mesh_drv_lr2021_configure().
 */
kode_status_t mesh_drv_lr2021_calibrate(void);

/** @brief How many frequencies one CalibFE takes (table 6-29). */
#define MESH_DRV_LR2021_CALIBRATIONS  3U

/**
 * @brief Calibrate the receiver's front end for up to three frequencies at once, each
 *        on the path it belongs to, and keep them.
 *
 * The chip stores them (6.4.2), so moving between them later asks for no calibration,
 * until a reset or a large change of temperature (20 C, 6.4.1). A slot not given is not
 * touched. Call in standby.
 * @param frequencies_hz The frequencies, Hz.
 * @param count          How many, 1..MESH_DRV_LR2021_CALIBRATIONS.
 */
kode_status_t mesh_drv_lr2021_calibrate_at(const uint32_t *frequencies_hz, uint8_t count);

/**
 * @brief Move to another setting with no calibration, writing only what differs,
 *        and leave the chip in standby with its crystal on, for a receive.
 *
 * A receive running is ended. Its band may differ: the receive path and the amplifier
 * follow, for little more than a retune in the band costs, since the receive boost is
 * the same on both paths and SetRxPath then calibrates nothing.
 * The frequency's front end must have been calibrated
 * (mesh_drv_lr2021_calibrate_at()), or the chip says RXFREQ_NO_FE_CAL_ERR.
 * @return KODE_OK; KODE_DENIED for a setting the chip does not have, or none in force
 *         to start from; another when the chip refused, and a setting must then be
 *         given to mesh_drv_lr2021_configure() afresh.
 */
kode_status_t mesh_drv_lr2021_retune(const mesh_drv_lora_settings_t *cfg);

/**
 * @brief mesh_drv_lr2021_send() with a preamble of its own, for this packet alone.
 *        The next send is the setting's again; a receiver hears any preamble.
 * @param data     The payload.
 * @param len      Its length, 1..MESH_DRV_LR2021_PAYLOAD_MAX.
 * @param preamble The symbols of this packet's preamble, at least 1.
 */
kode_status_t mesh_drv_lr2021_send_preamble(const uint8_t *data, uint8_t len, uint16_t preamble);

/**
 * @brief Put one packet on air and return while it is leaving.
 *
 * MESH_DRV_LR2021_INTERRUPT_SEND_DONE on DIO9 is the chip's word that it left whole. The
 * transmit carries the chip's own deadline (SetTx's timeout: the packet's
 * airtime and a quarter more, mesh_drv_lora_send_timeout_ticks()), so a done flag
 * that never comes does not leave the amplifier keyed: the chip stops itself
 * and raises MESH_DRV_LR2021_INTERRUPT_TIMEOUT. The packet's length becomes the chip's
 * payload_len; mesh_drv_lr2021_receive() and mesh_drv_lr2021_listen_for() put the whole
 * payload back, which is why a receiver is never started any other way.
 *
 * @param data The payload.
 * @param len  Its length, 1..MESH_DRV_LR2021_PAYLOAD_MAX.
 * @return KODE_OK when the chip took SetTx; KODE_DENIED for no payload or when
 *         no setting is in force (mesh_drv_lr2021_configure() not yet called), with
 *         nothing sent to the chip; KODE_IO_ERROR when it refused SetTx (a
 *         transmit already on air, say), which leaves nothing on air.
 */
kode_status_t mesh_drv_lr2021_send(const uint8_t *data, uint8_t len);

/**
 * @brief Read the packet the chip just received into @p buf.
 *
 * Call after MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE. The length, the RSSI and the SNR come from
 * the chip's packet status; the payload from its FIFO; the CRC verdict from
 * the interrupt word that announced the packet.
 *
 * @param buf     Where the payload goes.
 * @param cap     Its size; a longer packet is truncated to it.
 * @param irq     The flags mesh_drv_lr2021_interrupts_take() returned for this packet: a
 *                set MESH_DRV_LR2021_INTERRUPT_CRC_ERROR is what makes crc_ok false. The
 *                packet status alone cannot say - its CRC bit only says the
 *                packet carried one.
 * @param[out] rx The length read, the CRC verdict, the RSSI and SNR.
 * @return KODE_OK when a packet was read; KODE_DENIED before open;
 *         KODE_IO_ERROR when the chip would not answer.
 */
kode_status_t mesh_drv_lr2021_read(uint8_t *buf, uint8_t cap, uint32_t irq, mesh_drv_lr2021_packet_t *rx);

/**
 * @brief The instantaneous RSSI the receiver sees now, for a listen before a
 *        transmit. The receiver must be on (mesh_drv_lr2021_receive()).
 * @param[out] dbm The level, dBm.
 */
kode_status_t mesh_drv_lr2021_rssi(float *dbm);

/**
 * @brief Hold a test signal on air for @p ms, or until mesh_drv_lr2021_standby().
 *
 * INTO A 50 OHM LOAD ONLY (mesh_bsp.h). The chip is put in standby first.
 *
 * The preamble is a real packet - a preamble as long as @p ms and one byte -
 * under SetTx's timeout of @p ms, so the CHIP ends it, with
 * MESH_DRV_LR2021_INTERRUPT_TIMEOUT, even if the host is gone. (A preamble that 65535
 * symbols cannot stretch to @p ms ends sooner, with MESH_DRV_LR2021_INTERRUPT_SEND_DONE.)
 *
 * The carrier is SetTxTestMode(CW), which has no timeout in the datasheet: it
 * stays on until a command ends it. The driver writes @p ms as the chip's
 * default Tx timer first, on the chance that a test mode honours it - the
 * datasheet does not say so - and the caller must not rely on that: the owner
 * ends the carrier at its own deadline, and mesh_drv_lr2021_hold_reset() is what
 * a restart does to it.
 *
 * @param mode Preamble or carrier.
 * @param ms   1..MESH_BSP_TEST_MAX_MS.
 * @return KODE_OK with the chip in Tx; KODE_DENIED for a bad argument or a
 *         preamble that cannot be built for the setting; KODE_IO_ERROR when the
 *         chip would not go to Tx.
 */
kode_status_t mesh_drv_lr2021_test(mesh_drv_lr2021_test_t mode, uint32_t ms);

/**
 * @brief Receive until mesh_drv_lr2021_standby(): the receiver on, nothing sent.
 *
 * Idempotent: with the chip already in Rx - continuous receive stays in Rx
 * after every packet - it does nothing and answers KODE_OK, because SetRx
 * fails on a chip that is in Rx and clearing the FIFO could drop a packet.
 * From any other state it restores the receiver's packet shape (the whole
 * payload accepted and a preamble of any length heard, both of which a send
 * changes), clears the FIFO and starts receiving.
 *
 * @return KODE_OK with the chip in Rx; KODE_DENIED before open or while the
 *         chip is transmitting (it never aborts a transmit);
 *         KODE_IO_ERROR when the chip did not go to Rx.
 */
kode_status_t mesh_drv_lr2021_receive(void);

/** @brief Empty the receive FIFO: the bytes of a packet nobody will read. */
kode_status_t mesh_drv_lr2021_flush_receive(void);

/**
 * @brief Receive for a moment and let the chip's timer end it.
 *
 * The timer running out raises MESH_DRV_LR2021_INTERRUPT_TIMEOUT on DIO9 without the
 * amplifier ever turning on, which makes it the way to prove the interrupt
 * line is wired. A receive already running is ended first (SetRx cannot
 * replace one), and the packet shape is the receiver's.
 * @param ms How long, 1..1000.
 */
kode_status_t mesh_drv_lr2021_listen_for(uint32_t ms);

/**
 * @brief Search the channel for LoRa activity, leave the chip in its fallback mode,
 *        standby, and say so on DIO9: MESH_DRV_LR2021_INTERRUPT_CAD_DONE, with
 *        MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED before or with it if it saw any.
 *
 * It lasts @p symbols symbols, mesh_drv_lora_cad_us(). The chip goes to standby
 * first, so a receive running is ended, and the caller starts it again. The
 * threshold is the datasheet's for the setting (table 6-19). Nothing is transmitted.
 * @param symbols 1..4.
 * @param fast    Stop as soon as there is clearly nothing, which the datasheet says
 *                costs up to 3 dB of detection (9.6).
 * @return KODE_OK with the search running; KODE_DENIED for a bad argument or no
 *         setting in force; KODE_IO_ERROR when the chip refused it.
 */
kode_status_t mesh_drv_lr2021_cad(uint8_t symbols, bool fast);

/**
 * @brief Search the channel for a LoRa preamble and, when there is one, go on
 *        receiving its packet: CAD_RX with only a preamble counting (6.3.11, 9.4).
 *
 * MESH_DRV_LR2021_INTERRUPT_CAD_DONE says the search is over, with
 * MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED before or with it if it saw a preamble. With
 * nothing seen the chip is in its fallback mode, standby, as after
 * mesh_drv_lr2021_cad(). With one seen it stays in Rx and goes straight to the sync
 * word, without detecting the preamble again: the packet ends as a single receive
 * does (MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, or a header or CRC error, then the
 * fallback mode), and a header that never comes leaves the chip in Rx until a
 * command ends it. The receiver's packet shape is put back first, as
 * mesh_drv_lr2021_receive() does. Nothing is transmitted.
 * @param symbols 1..4.
 * @param fast    Stop as soon as there is clearly nothing (9.6).
 * @return KODE_OK with the search running; KODE_DENIED for a bad argument or no
 *         setting in force; KODE_IO_ERROR when the chip refused it.
 */
kode_status_t mesh_drv_lr2021_cad_receive(uint8_t symbols, bool fast);

/** @brief Take everything off air and leave the chip idle. */
kode_status_t mesh_drv_lr2021_standby(void);

/**
 * @brief Read the pending interrupt flags, clear them, and listen to DIO9
 *        again.
 * @return The flags; 0 when none was pending or the chip did not answer.
 *         mesh_drv_lr2021_interrupts_read() says which of the two.
 */
uint32_t mesh_drv_lr2021_interrupts_take(void);

/**
 * @brief mesh_drv_lr2021_interrupts_take(), saying whether the flags were read.
 *
 * The chip clears its flags when it takes the command, before its answer is read: when the
 * answer is lost, so are the flags, an amplifier fault among them. A caller that is told so
 * treats the radio as failed - off, and begun again - and never as "nothing happened".
 * @param[out] flags The flags; 0 when none was pending or they could not be read.
 * @return KODE_OK; KODE_DENIED when the driver is not open or @p flags is NULL; KODE_TIMEOUT
 *         or KODE_IO_ERROR when the chip did not answer, the flags then lost.
 */
kode_status_t mesh_drv_lr2021_interrupts_read(uint32_t *flags);

/** @brief What the chip says it is doing now. */
kode_status_t mesh_drv_lr2021_mode(mesh_drv_lr2021_mode_t *mode);

/**
 * @brief Read the chip's error flags, and log each one that is set by name.
 * @return The 16-bit word of GetErrors; 0xFFFF when the chip did not answer.
 */
uint16_t mesh_drv_lr2021_errors(void);

/** @brief Clear the chip's error flags: GetErrors keeps each until this. */
void mesh_drv_lr2021_errors_clear(void);

/**
 * @brief Pull the chip's reset line low and leave it there.
 *
 * For the moment before the P4 itself restarts: a test signal needs no host
 * to keep going, so a chip left to itself would transmit on through the
 * restart. Held in reset it transmits nothing, and when the P4's pins let go
 * the module's own pull-up starts it again in standby. One pin, no bus: it may
 * be called from any task, and from a shutdown handler.
 *
 * mesh_drv_lr2021_open() registers it as an esp_restart() shutdown handler, so
 * every restart that goes through esp_restart() - kode_exit_to_os() among
 * them - silences the radio. A panic, a watchdog and a reset button do not run
 * shutdown handlers.
 */
void mesh_drv_lr2021_hold_reset(void);

/**
 * @brief Put the chip to sleep: SetSleep (6.3.1) with no retention and no 32 kHz clock.
 *
 * Call it from a confirmed standby. Asleep it draws 0.47 uA (Table 3-4, IDDSL0) against
 * Standby RC's 1.21 mA, its BUSY stays high, and only NSS low for 100 us or a reset wakes it
 * (4.4.1), cold: with its defaults, as after mesh_drv_lr2021_reset(), which is how a setting
 * goes back on it. Every other call is refused while it sleeps, except mesh_drv_lr2021_alive(),
 * which wakes it for the question and puts it back.
 *
 * @return KODE_OK, also when asleep already; KODE_DENIED when closed; what the command met.
 */
kode_status_t mesh_drv_lr2021_sleep(void);

/** @brief Whether mesh_drv_lr2021_sleep() put the chip to sleep and nothing has woken it since. */
bool mesh_drv_lr2021_asleep(void);

/**
 * @brief Whether the restart hook is registered: a restart will hold the
 *        radio in reset.
 *
 * ESP-IDF has five shutdown-handler slots, and the others can fill them. The
 * owner refuses a carrier test when this is false.
 */
bool mesh_drv_lr2021_guarded(void);

/**
 * @brief Ask the chip its version and say whether the answer is an LR2021's.
 *
 * A cheap liveness check the owner makes while the module should be there: an
 * unplugged header reads all zeros on MISO, so a plausible major number
 * (MESH_BSP_LORA_FW_MAJOR) is the chip still answering. It asks GetVersion
 * without a reset and does not disturb a receive in progress. A chip that
 * mesh_drv_lr2021_sleep() put to sleep is woken for it and put back to sleep:
 * a sleeping module that is pulled out is noticed as an awake one is.
 * @return true when the chip answered with the expected major number.
 */
bool mesh_drv_lr2021_alive(void);

/** @brief The level of the BUSY line now: true is high. */
bool mesh_drv_lr2021_busy(void);

/** @brief The level of DIO9 now: true is high. */
bool mesh_drv_lr2021_dio9(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_DRV_LR2021_H_ */
