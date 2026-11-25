/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_bsp.h
 * @brief The Mesh module (KDMSH001 rev 1.0), described: what is wired where.
 *
 * Every number about the module lives here and nowhere else, the way every
 * number about the Kode Dot lives in kode-sdk's kbsp. This header describes;
 * it does nothing. A driver or the owner that needs a position, a timing or
 * a limit names it from here (tools/check_layers.sh, rule 3).
 *
 * The module fits J3 one way only, and its position p is J3's position p:
 * there is no rotation to undo. The P4 GPIO behind each position comes back
 * from kode-sdk's kode_sys_exp_lend(), never from here; the number in the
 * comments is only what the module's net is called.
 *
 * The facts are the module's schematic and its chips' datasheets; a figure
 * from a datasheet names it.
 */
#ifndef MESH_BSP_H_
#define MESH_BSP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The header: J3 positions, 1 to 14 ------------------------------------
 *
 * KDMSH001 rev 1.0. The fourteen signals are kode-sdk's positions 1..14
 * (kbsp_exp.h). The LoRa radio (an LR2021) hangs off the SPI block, the GNSS
 * receiver (a Minew ME31GR02) off a UART, and the two share nothing but the
 * header. The I2C pair is the GNSS's second face and is used only to tell the
 * module apart and watch for it leaving - never as a bus, because the stock
 * GNSS firmware can drive SDA push-pull (see MESH_BSP_GNSS_I2C_RISK). */
#define MESH_BSP_POSITION_GNSS_RESET  1U   /**< GPIO8: the GNSS reset, active low, no
                                      *   pull on the module.                   */
#define MESH_BSP_POSITION_GNSS_TXD    2U   /**< GPIO9: the GNSS's TXD; the P4 listens. */
#define MESH_BSP_POSITION_PPS         3U   /**< GPIO10: the GNSS's one-pulse-a-second. */
#define MESH_BSP_POSITION_GNSS_RXD    4U   /**< GPIO11: the GNSS's RXD; the P4 speaks. */
#define MESH_BSP_POSITION_LORA_RESET  5U   /**< GPIO12: the LR2021 reset, active low,
                                      *   10 k to 3V3 (idles released).         */
#define MESH_BSP_POSITION_LORA_DIO9   6U   /**< GPIO13: the LR2021's interrupt line.   */
#define MESH_BSP_POSITION_LORA_DIO8   7U   /**< GPIO14: the LR2021's DIO8, unused.     */
#define MESH_BSP_POSITION_LORA_SCK    8U   /**< GPIO21: SPI clock.                     */
#define MESH_BSP_POSITION_LORA_MISO   9U   /**< GPIO19: SPI data from the chip.        */
#define MESH_BSP_POSITION_LORA_CS     10U  /**< GPIO20: SPI chip select, 10 k to 3V3.  */
#define MESH_BSP_POSITION_LORA_MOSI   11U  /**< GPIO17: SPI data to the chip.          */
#define MESH_BSP_POSITION_LORA_BUSY   12U  /**< GPIO18: high while the chip is busy.   */
#define MESH_BSP_POSITION_I2C_SCL     13U  /**< GPIO15: the GNSS's I2C clock, 2.2 k.   */
#define MESH_BSP_POSITION_I2C_SDA     14U  /**< GPIO16: the GNSS's I2C data, 2.2 k.    */

/* ---- What the owner holds -----------------------------------------------------
 *
 * The owner lends every position it drives for as long as it runs. Positions
 * 1 to 12 - the LoRa block's seven lines (SCK, MISO, CS, MOSI, BUSY, DIO9 and
 * the reset), the GNSS's four (reset, TXD, RXD, 1PPS) and DIO8, held with
 * them and left alone - are held as one from the moment a Mesh is found; the
 * I2C pair is lent before that, to read the fingerprint and to see the module
 * leave. Handing a position back resets the pin, and ESP-IDF's reset turns
 * the pull-up on for a moment, which on the LR2021's chip select or reset is
 * a glitch the chip could take badly - so held positions are not handed back
 * while the module is there. */
#define MESH_BSP_LOAN_TAG        "mesh"        /**< The radio and GNSS lines.      */
#define MESH_BSP_LOAN_PAIR       "mesh.pair"   /**< The I2C pair, for the look.    */

/* The positions an app may take on its own with kode-sdk's kode_pin_*() or
 * kode_sys_exp_*(): none. A Mesh uses all fourteen, and while it is there the
 * owner holds positions 1 to 12 and the I2C pair. DIO8 (pos 7) is held with
 * the rest and left floating, as its net is on the module. */

/* ---- The fingerprint -------------------------------------------------------
 *
 * What a Mesh holds against the P4's internal pull-downs, nothing driven: the
 * four lines with a pull-up to 3V3 on the module read high - SCL and SDA on
 * their 2.2 k, the chip select and the LoRa reset on their 10 k. The rest
 * float or are driven and are not judged. Read with pull-downs only: a
 * pull-up on an input before the module is known can feed the module through
 * a signal diode. Three readings in a row must agree. The
 * fingerprint alone does not separate a Mesh from an F*CK (which shares the
 * chip-select line's pull-up and puts its own on others): the owner confirms
 * with the radio's handshake, below. */
#define MESH_BSP_FINGERPRINT_LINES 4U
#define MESH_BSP_LOOK_ROUNDS     3U   /**< Agreeing readings before it is taken.  */
#define MESH_BSP_LOOK_GAP_MS     20U  /**< Between those readings.               */
#define MESH_BSP_SETTLE_MS       2U   /**< A line given this long against the pull
                                      *   before it is read.                     */

/* ---- The radio: a Semtech LR2021 inside a NiceRF LoRa2021 (U501) -----------
 *
 * One chip, two transmitters and no microcontroller: the P4 drives it over
 * SPI. A sub-GHz power amplifier feeds the module's SMA J501 (the LF path, up
 * to its band's limit), a 2.4 GHz amplifier feeds J502 (the HF path), and
 * there is no antenna switch between them - one band is on air at a time. The
 * chip's firmware answers GetVersion with 0x01/0x18; an LR2022 or LR2012 would
 * say 0x02/0x00. A reset leaves the chip in standby with the receiver
 * uncalibrated, so the front end is calibrated for the frequency in use after
 * every reset (CalibFE), or reception is poor and the chip logs
 * RXFREQ_NO_FE_CAL. The schematic; the LR2021 datasheet V1.1. */
#define MESH_BSP_LORA_FW_MAJOR   0x01U  /**< GetVersion's major on an LR2021.     */
#define MESH_BSP_LORA_FW_MINOR   0x18U  /**< Its minor.                           */

/* The SPI link. The controller is kodeOS's to hand out (kode_sys_exp_ctrl()),
 * and SPI3 is the one an app gets while the NFC reader holds SPI2. No DMA:
 * one transaction moves at most 64 bytes and costs no memory, and a payload
 * longer than that is sent under a chip select held low by hand. */
#define MESH_BSP_SPI_HZ          (8U * 1000U * 1000U)
#define MESH_BSP_SPI_PIECE_MAX   64U    /**< Bytes per transaction without DMA.   */

/* The frequency that divides the two amplifiers, and the two SMA bands. */
#define MESH_BSP_2_4_GHZ_FROM_HZ      1500000000U  /**< At or above it, the 2.4 GHz PA. */

/* What each of the chip's paths reaches (datasheet 1.5.1 and 1.5.2). The
 * module's own filters and antennas are for 868 and 2.4 GHz: elsewhere inside
 * these the chip answers, and what leaves the SMA is not known. */
#define MESH_BSP_SUB_GHZ_FROM_HZ      150000000U   /**< The LF path's lowest.  */
#define MESH_BSP_SUB_GHZ_TO_HZ        960000000U   /**< The LF path's highest. */
#define MESH_BSP_2_4_GHZ_TO_HZ        2500000000U  /**< The HF path's highest. */

/* NEVER TRANSMIT INTO AN OPEN SMA. The LR2021's power amplifiers are
 * guaranteed only into a load (datasheet 3-2, VSWR at most 10:1): an open
 * connector is an infinite standing-wave ratio, the amplifier may become
 * unstable, and an unstable amplifier radiates spurious emissions and may
 * not survive it. Both SMA connectors carry
 * their antenna, or a 50 ohm load, before anything transmits. The driver
 * cannot check this; it is a fact about how the board is set up. */

/* THE TEST SIGNALS GO INTO A 50 OHM LOAD, NEVER AN ANTENNA. mesh_radio_test()
 * keys the amplifier with a bare carrier or a bare preamble: 100 % of the
 * time on air, against the 1 % an 868 antenna is allowed. It is for a power
 * meter or an analyser on the SMA through a load, and only a build with
 * CONFIG_MESH_LAB has it: any other refuses the call (mesh_plan_test_allowed()).
 *
 * The signal is bounded, and by as many layers as the chip lets it be:
 *  - the longest a caller may ask for is MESH_BSP_TEST_MAX_MS;
 *  - the preamble test is a real packet under SetTx(tx_timeout), so the CHIP's
 *    own timer ends it, panic or watchdog included (datasheet 6.3.6);
 *  - the carrier is SetTxTestMode(CW), to which the datasheet gives NO timeout
 *    (21.2). The driver writes SetDefaultRxTxTimeout before it as a try, which
 *    the datasheet does not say the test modes honour: the owner takes the
 *    carrier off air at its own deadline and logs whether the chip's timer got
 *    there first. The driver holds the chip in reset when the P4 restarts
 *    through esp_restart() (the shutdown handler), and the carrier is refused
 *    when no handler could be registered. A crash or a watchdog reset runs no
 *    shutdown handler, and then a carrier stays on air until the module loses
 *    power; nothing in software prevents that, which is why the load rule
 *    exists.
 * The owner waits MESH_BSP_TEST_GRACE_MS past the deadline for the chip's own
 * timer before it ends a test itself, and says which of the two did. */
#define MESH_BSP_TEST_MAX_MS     60000U   /**< The longest mesh_radio_test() holds on air. */
#define MESH_BSP_TEST_GRACE_MS   250U     /**< The chip's timer's head start.          */

/* How long the chip may hold BUSY high after a command, a calibration and a
 * reset. A command takes microseconds; the first two are polled, the rest
 * slept. Reset wants about 100 us low, then a moment to raise BUSY. */
#define MESH_BSP_LORA_BUSY_MS    100U
#define MESH_BSP_LORA_CALIB_MS   500U
#define MESH_BSP_LORA_RESET_MS   500U

/* ---- The GNSS: a Minew ME31GR02 (U601) -------------------------------------
 *
 * A receiver that transmits nothing: it is listened to, never relied on for
 * anything on air. The chip inside is a Beken BK1661, which speaks NMEA with
 * $POL... proprietary sentences (not the $PMTK/$PAIR/$PCAS of the MediaTek and
 * Quectel parts), at 115200 by default. Its antenna (a passive YPGS001AA) has
 * no matching network, SAW or TVS in front, so a strong nearby transmitter
 * desensitises it: the LR2021 and the GNSS coexist only at a low transmit duty
 * cycle. A 0.22 F supercap
 * on +3V3 keeps the receiver's backup for about three hours, which is a warm
 * start, not a cold one, after a short gap. */
#define MESH_BSP_GNSS_BAUD       115200U /**< The default rate.                   */
#define MESH_BSP_GNSS_BAUD_ALT   9600U   /**< A receiver at this rate is recognised too. */
#define MESH_BSP_GNSS_RESET_MS   150U    /**< The reset pulse, and the banner gap. */

/* The GNSS has no power pin on the module, and its datasheet
 * says nothing of what it draws with RESET held low. Its low-power state is
 * the one the datasheet documents: Sleep by software (ME31GR02 datasheet
 * v1.0.0, p.6 section 2.9), where the DSP core, the digital logic but the RTC
 * and most of the analog and RF stop, and only data on its UART (or its RTC)
 * wakes it. VCC stays on, so the backup RAM is kept and it comes back from a
 * hot start (1 s, p.4); the ephemeris also goes to its flash now and then. It
 * draws ~17.5 mA tracking and ~19 mA acquiring (p.7, table 3.3); its current
 * asleep with VCC on is not in the datasheet. */
#define MESH_BSP_GNSS_HAS_POWER  0

/* The Sleep command, in binary, as the datasheet gives it (p.14, section 6.4,
 * "Sleep mode (can be woken up after receiving any data on the serial port)").
 * It is Beken's Power/DeepSleep (0x00/0x03) with its two uint32 parameters,
 * the wake-up timer's, at 0: no timer, only the UART wakes it. A ME31GR02
 * command: a receiver at 9600 baud is not sent it. */
#define MESH_BSP_GNSS_SLEEP_FRAME { 0x42U, 0x4BU, 0x51U, 0x05U, 0x00U, 0x03U, 0x00U, 0x08U, \
                                    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U }
#define MESH_BSP_GNSS_SLEEP_FRAME_LEN 16U

/* The datasheet gives neither how long the receiver takes to fall silent
 * after the command nor how long it takes to talk after the byte that wakes
 * it; the nearest figure is power-up to its first output, about 0.5 s (p.12,
 * section 5.6). So the driver watches rather than waits: a sentence that
 * checks later than MESH_BSP_GNSS_SLEEP_MS after the command means it did not
 * sleep, and one that does not come within MESH_BSP_GNSS_WAKE_MS of the byte
 * has the byte sent again, MESH_BSP_GNSS_WAKE_TRIES times, after which the
 * receiver is reset. Numbers of this SDK, not the datasheet's. */
#define MESH_BSP_GNSS_SLEEP_MS     1000U  /**< A sentence after this, it is awake. */
#define MESH_BSP_GNSS_WAKE_MS      1500U  /**< Per try, until a sentence checks.   */
#define MESH_BSP_GNSS_WAKE_TRIES   3U     /**< Tries before the reset pulse.       */

/* NEVER MASTER THE I2C PAIR. The stock ME31GR02 firmware can drive SDA
 * push-pull soon after its reset is released, so a P4 acting as I2C master on
 * GPIO15/16 fights it. The pair is read passively for the fingerprint and to
 * see the module leave, and that is all. */
#define MESH_BSP_GNSS_I2C_RISK   1

/* ---- Watching the module ---------------------------------------------------
 *
 * Looked for this often while it is not there; once found, its I2C pair is
 * read against the pull-downs this often, and two silences in a row mean it
 * has gone. The radio's version is re-asked at the same cadence
 * as a second, surer witness that a Mesh is still on the header. */
#define MESH_BSP_LOOK_MS         500U
#define MESH_BSP_ALIVE_MS        1000U
#define MESH_BSP_GONE_MISSES     2U
/** @brief While the receiver listens, its RSSI is sampled this often: what the status carries and the RSSI call reads. */
#define MESH_BSP_RSSI_MS         100U

/* ---- The supply the module needs -------------------------------------------
 *
 * J3's +3.3V_EXT (U18) feeds the module; its enable is the C5's IO5, and the
 * rail's state when an app starts is not promised, so mesh_aos_start() asks for
 * it with kode_sys_exp_power(true), which goes over the link to the C5: that is
 * why the owner may find the module some seconds later while the C5 is busy,
 * and why an image without the radio (CONFIG_KODEOS_RADIO=n) finds J3 always
 * powered and cannot switch it off. A chip that is keyed stays keyed until something pulls its
 * reset line or removes the rail, which is why the SDK stops the radio on every
 * exit it can see (mesh_drv_lr2021.h, mesh_drv_lr2021_hold_reset()). The module's
 * +3V3 is split across two header pins (positions 2 and 19 of J401): both are
 * needed, one feeds the radio and GNSS, the other the I2C pull-ups. The 5 V
 * pins are not used. The rail carries about 27 uF of ceramic to charge, so a
 * module is plugged in with the Dot's rail down. */
#define MESH_BSP_CURRENT_MA_SENDING   142U   /**< The module at 22 dBm on 868 MHz
                                        *   (NiceRF's LoRa2021 datasheet, table
                                        *   8): far above what this SDK sends
                                        *   without CONFIG_MESH_LAB.             */

#ifdef __cplusplus
}
#endif

#endif /* MESH_BSP_H_ */
