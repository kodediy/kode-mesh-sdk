/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_types.h
 * @brief What the Mesh module is made of, in the words of an app: its bands, a radio
 *        setting, a packet, a GNSS fix, its status and the reasons a call is denied.
 *
 * Both layers speak these types: mesh.h (the standard layer, for tasks) and
 * mesh_sys.h (the extended layer, for Active Objects). Neither includes the
 * other, so an Active Object that includes mesh_sys.h sees no call that blocks.
 * This header only describes; it includes no ESP-IDF, FreeRTOS, QP/C or kode-sdk
 * header.
 */
#ifndef MESH_TYPES_H_
#define MESH_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest LoRa payload one packet carries. */
#define MESH_PAYLOAD_MAX   255U

/**
 * @name The longest the power of a setting may be asked for
 * @brief Tenths of a dBm. A value outside is a bad argument, never cut to fit;
 *        inside it, the region's cap holds the transmit (see mesh_region.h).
 * @{
 */
#define MESH_POWER_X10_MIN   (-1000)   /**< -100 dBm. */
#define MESH_POWER_X10_MAX   1000      /**< +100 dBm. */
/** @} */

/** @brief The longest a timed listen may be asked for, ms (the shortest is 1). */
#define MESH_LISTEN_MS_MAX   1000U

/** @brief How many frequencies mesh_radio_calibrate() keeps on the chip. */
#define MESH_CALIBRATIONS_MAX  3U

/** @brief The two bands, each its own amplifier and SMA on the module. */
typedef enum {
    MESH_BAND_SUB_GHZ = 0,   /**< 868 MHz, on J501.   */
    MESH_BAND_2_4_GHZ,       /**< 2.4 GHz, on J502.   */
    MESH_BAND_COUNT
} mesh_band_t;

/**
 * @brief One whole setting of the radio: every number a LoRa network fixes on the
 *        air. mesh_radio_config_default() starts one for a band.
 */
typedef struct {
    uint32_t frequency_hz;     /**< The centre. The band follows from it: sub-GHz
                                *   on J501, or 2.4 GHz on J502 from
                                *   MESH_BSP_2_4_GHZ_FROM_HZ up.                  */
    uint32_t bandwidth_hz;     /**< 31250, 41670, 62500, 83340, 101562, 125000,
                                *   203125, 250000, 406250, 500000, 812500 or
                                *   1000000 (the chip's, datasheet table 9-3).    */
    uint8_t  spreading_factor; /**< 5..12.                                        */
    uint8_t  coding_rate;      /**< The N of 4/N, 5..8.                           */
    uint16_t preamble;         /**< Preamble symbols, 1..65535.                   */
    uint8_t  sync_word;        /**< The LoRa sync word, one byte: 0x12 is private
                                *   and the chip's default, 0x34 public, and the
                                *   datasheet lists no other (table 9-7).         */
    int32_t  power_dbm_x10;    /**< Tenths of a dBm (85 = 8.5 dBm), on
                                *   mesh_radio_begin()'s scale.                   */
} mesh_radio_config_t;

/** @brief What the GNSS last said about where it is. */
typedef struct {
    bool     valid;              /**< A position is reported (GGA quality above 0,
                                  *   or RMC status 'A'); the four below with it. */
    double   latitude_deg;       /**< Degrees north.                             */
    double   longitude_deg;      /**< Degrees east.                              */
    float    altitude_m;         /**< Metres above the geoid.                    */
    float    speed_knots;        /**< Over the ground.                           */
    float    horizontal_dilution;/**< HDOP: lower is a better geometry.          */
    uint8_t  satellites_used;    /**< In the fix.                                */
    uint8_t  satellites_in_view; /**< As the last GSV reported.                  */
    uint32_t time_utc;           /**< Time of day, hhmmss; 0 until one is read.  */
    uint32_t date;               /**< ddmmyy; 0 until one is read.               */
    uint8_t  fix_type;           /**< GSA: 1 none, 2 is 2D, 3 is 3D; 0 until one is
                                  *   read. Not every receiver says it.          */
    uint8_t  quality;            /**< GGA: 0 none, 1 GNSS, 2 differential and so
                                  *   on; 0 until one is read.                   */
    float    position_dilution;  /**< PDOP (GSA); 0 until one is read.           */
    float    vertical_dilution;  /**< VDOP (GSA); 0 until one is read.           */
    float    course_deg;         /**< Over the ground, true north (RMC), with
                                  *   `valid`. Meaningless standing still.       */
    float    geoid_separation_m; /**< The geoid over the WGS84 ellipsoid (GGA), with
                                  *   `valid`: altitude_m plus this is the height
                                  *   over the ellipsoid.                        */
    uint32_t age_ms;             /**< Since a sentence last reported the position,
                                  *   as of AO_Mesh's last tick; UINT32_MAX until
                                  *   one has.                                   */
} mesh_gnss_fix_t;

/** @brief A transmitter test signal, for an analyser (into a 50 ohm load only). */
typedef enum {
    MESH_TEST_SIGNAL_CARRIER  = 1,   /**< An unmodulated carrier.                      */
    MESH_TEST_SIGNAL_PREAMBLE = 2,   /**< A bare LoRa preamble, one byte after it.     */
} mesh_test_signal_t;

/** @brief A received packet. */
typedef struct {
    uint8_t data[MESH_PAYLOAD_MAX];  /**< The payload.                          */
    uint8_t length;                  /**< Its length.                           */
    bool    crc_valid;               /**< The packet carried a CRC and it matched.
                                     *   False for a wrong CRC - the payload is
                                     *   still delivered, for the app to judge -
                                     *   and for a packet sent without one.   */
    float   rssi_dbm;               /**< The packet's RSSI.                    */
    float   snr_db;                 /**< Its signal-to-noise ratio.            */
} mesh_packet_t;

/** @brief What sits on J3 while no Mesh does (mesh_status_t.seen). */
typedef enum {
    MESH_MODULE_NONE = 0,   /**< Nothing reads high: no module, or one without 3V3. */
    MESH_MODULE_MESH,       /**< A Mesh (its radio answered).                      */
    MESH_MODULE_OTHER,      /**< Something reads high, but not a Mesh.             */
} mesh_module_t;

/**
 * @brief Where J3's supply stands (mesh_status_t.supply): the answer of the last
 *        switch the SDK asked for, EXP_POWER_SIG's rc in the SDK's words.
 */
typedef enum {
    MESH_SUPPLY_UNKNOWN = 0,   /**< Not asked yet, or asked and not answered.                */
    MESH_SUPPLY_ON,            /**< The rail is on, as asked.                                */
    MESH_SUPPLY_OFF,           /**< The rail is off, as asked (after mesh_end()).            */
    MESH_SUPPLY_NO_LINK,       /**< The link to the C5 would not come up: the wish stays and the
                                *   next link carries it out. The module is found when the
                                *   rail comes up.                                           */
    MESH_SUPPLY_FAILED,        /**< The C5 refused to drive the rail; the wish is dropped. Said
                                *   in the log as well.                                       */
    MESH_SUPPLY_ALWAYS_ON,     /**< The image has no radio (CONFIG_KODEOS_RADIO=n): the C5 is in
                                *   reset and J3 is always powered. Nothing can switch it.    */
} mesh_supply_t;

/**
 * @brief Why a call did not do what it asked, or what the owner answered a request with.
 *        mesh_error() (standard layer) and mesh_sys_why_text() (extended layer) put it in words.
 */
typedef enum {
    MESH_WHY_NONE = 0,         /**< The call went through.                                   */
    MESH_WHY_NOT_STARTED,      /**< mesh_begin() or mesh_sys_begin() has not run.            */
    MESH_WHY_FROM_AO,          /**< A call that blocks, made from an Active Object.          */
    MESH_WHY_MODULE_ABSENT,    /**< No module: plugged out, or not found yet.                */
    MESH_WHY_BAD_ARGUMENT,     /**< A null pointer, or a length or value out of range.       */
    MESH_WHY_NOT_BEGUN,        /**< No radio setting yet: begin the radio first.              */
    MESH_WHY_BUSY_TEST,        /**< A test signal is on air.                                 */
    MESH_WHY_BUSY_SENDING,     /**< A packet is on air.                                      */
    MESH_WHY_BUSY_SEARCH,      /**< A channel search is running.                             */
    MESH_WHY_BUSY_LISTENING,   /**< A timed listen is running.                               */
    MESH_WHY_BUSY_ARRIVING,    /**< A packet is on its way in.                               */
    MESH_WHY_NOT_ALLOWED,      /**< The setting is outside what the region allows.           */
    MESH_WHY_NOT_CALIBRATED,   /**< The frequency is not one a calibration covered.          */
    MESH_WHY_NOT_RECEIVING,    /**< The receiver is off.                                     */
    MESH_WHY_NO_GUARD,         /**< A carrier test with nothing to hold the radio in reset on
                                *   a restart.                                               */
    MESH_WHY_DROPPED,          /**< Asked before a stop: dropped.                            */
    MESH_WHY_UNKNOWN,          /**< A request the owner does not know.                       */
    MESH_WHY_DUTY,             /**< The duty budget has no room: wait and ask again.         */
    MESH_WHY_NO_SLOT,          /**< Both payload slots hold requests not yet taken.          */
    MESH_WHY_QUEUE_FULL,       /**< The owner's queue is full.                               */
    MESH_WHY_TIMED_OUT,        /**< The owner did not answer in time.                        */
    MESH_WHY_INTERRUPTED,      /**< Cut short by a stop, a standby or the module leaving.    */
    MESH_WHY_GNSS_OFF,         /**< The GNSS is off.                                         */
    MESH_WHY_SEND_FAILED,      /**< The chip never said the packet was done: it may not have
                                *   gone whole.                                              */
    MESH_WHY_LEVEL_TAKEN,      /**< The owner's QP/C level is held by another Active Object, or
                                *   does not exist in this build.                            */
    MESH_WHY_KODE_NOT_READY,   /**< kode_init() has not run: the pools the owner posts from do not
                                *   exist yet.                                               */
    MESH_WHY_NO_PACKET,        /**< No packet is waiting to be read.                         */
    MESH_WHY_NO_FIX,           /**< The GNSS is on and has no fix yet.                       */
    MESH_WHY_LAB_ONLY,         /**< A test signal, in a build without CONFIG_MESH_LAB: it is a
                                *   lab build's.                                             */
    MESH_WHY_CHIP,             /**< The module's chip did not do it: a bus or chip error
                                *   (KODE_IO_ERROR, or KODE_TIMEOUT when its BUSY stuck).     */
    MESH_WHY_COUNT
} mesh_why_t;

/** @brief Everything the SDK knows about the module, as of its last tick. */
typedef struct {
    bool          present;          /**< On J3 and held.                           */
    bool          radio_ready;      /**< A band is set (mesh_radio_begin()).        */
    bool          radio_asleep;     /**< No setting on it, so the chip sleeps
                                     *   (0.47 uA): mesh_radio_begin() wakes it.   */
    mesh_band_t   band;             /**< The band set.                             */
    uint8_t       spreading_factor; /**< The one set, 5..12.                       */
    uint32_t      frequency_hz;     /**< The centre set.                           */
    uint32_t      bandwidth_hz;     /**< The bandwidth set.                        */
    uint8_t       coding_rate;      /**< The N of 4/N set.                         */
    uint16_t      preamble;         /**< The preamble symbols set.                 */
    uint8_t       sync_word;        /**< The sync word set.                        */
    int32_t       power_dbm_x10;    /**< The power asked, tenths of a dBm (85 = 8.5 dBm). */
    int32_t       power_applied_dbm_x10; /**< The power the setting really puts out, tenths of a
                                     *   dBm (85 = 8.5 dBm): what was asked after the band's cap
                                     *   and the chip's half-dB steps. 0 with no setting.     */
    bool          sending;          /**< A packet is on air.                       */
    bool          receiving;        /**< The receiver is on (mesh_radio_receive()). */
    bool          arriving;         /**< A packet is on its way in: from its preamble
                                     *   to its end, or to a timer if the end is lost:
                                     *   the preamble set and 28 symbols more, the
                                     *   longest packet from its header on, never more
                                     *   than 2 s in all. A sender with a longer
                                     *   preamble outlasts the first. It falls too
                                     *   when the radio leaves Rx for a send, a search
                                     *   or a standby. */
    bool          cad_running;      /**< A channel search is on (mesh_radio_cad()). */
    bool          testing;          /**< A test signal is on air.                  */
    bool          amplifier_fault;  /**< The amplifier faulted: the radio is off
                                     *   and stays so until mesh_radio_begin().   */
    bool          antennas_confirmed; /**< The app said the antennas are on
                                     *   (mesh_radio_antennas_confirm(true)).     */
    bool          antenna_suspected[MESH_BAND_COUNT]; /**< That band's amplifier
                                     *   tripped its over-current alarm since the app
                                     *   last confirmed: is its antenna loose? The
                                     *   band is held to MESH_REGION_SAFE_DBM_X10
                                     *   until the app confirms again.            */
    bool          power_capped;     /**< The setting in force puts out less than was
                                     *   asked because of the safe power: the antennas
                                     *   are not confirmed, or its band is suspected.
                                     *   power_applied_dbm_x10 says what goes out.  */
    uint32_t      duty_wait_ms;     /**< Until a sub-GHz send is within the duty
                                     *   cycle; 0 on 2.4 GHz or when one may go.  */
    float         last_rssi_dbm;    /**< The last RSSI sample of the receiver, taken every
                                     *   MESH_BSP_RSSI_MS while it is on and nothing else is
                                     *   going on (see mesh_radio_rssi()).         */
    bool          rssi_valid;       /**< The receiver is on and has taken a sample since. */
    uint8_t       packets_waiting;  /**< Received and not yet read.                */
    uint32_t      packets_received; /**< Since the module came.                    */
    uint32_t      packets_dropped;  /**< Lost to a full queue: read them faster.   */
    uint32_t      packets_sent;     /**< That left whole.                          */
    uint16_t      radio_errors;     /**< The chip's last error word; 0 is none.    */

    bool            gnss_on;        /**< The GNSS is on (mesh_gnss_begin()).        */
    mesh_gnss_fix_t fix;            /**< The latest fix (valid when fix.valid).    */
    uint32_t        gnss_sentences; /**< NMEA sentences with a good checksum.      */
    uint32_t        gnss_baud;      /**< The rate the GNSS locked at; 0 until one. */
    bool            gnss_talking;   /**< It is sending good sentences.             */
    bool            gnss_asleep;    /**< mesh_gnss_end() put it to sleep: it draws
                                     *   next to nothing and mesh_gnss_begin()
                                     *   wakes it from a hot start.               */
    bool            pps_high;       /**< The 1PPS line is high now.                */

    bool          i2c_taken;        /**< The I2C pair is the app's (mesh_i2c_lend()). */
    mesh_module_t seen;             /**< What is on J3 while no Mesh is.           */
    uint16_t      seen_level;       /**< The fingerprint reading behind it.        */

    mesh_supply_t supply;           /**< J3's supply, as the last switch left it.  */
    uint32_t      events_dropped;   /**< Events (standard layer) lost because its queue of
                                     *   16 was full: the listeners were too slow.    */
    uint32_t      news_dropped;     /**< News (extended layer) not posted to a subscribed
                                     *   Active Object because its queue was within the
                                     *   margin.                                      */
    uint32_t      answers_dropped;  /**< Answers (extended layer) lost because the asking
                                     *   Active Object's queue was completely full.   */
} mesh_status_t;

/**
 * @name Settings, as the events setting_lost and setting_restored carry them
 * @brief A mask: what the event is about.
 * @{
 */
#define MESH_SETTING_RADIO     1UL   /**< The radio's setting (begin, configure, retune). */
#define MESH_SETTING_RECEIVER  2UL   /**< The receiver, on (mesh_radio_receive(true)).    */
#define MESH_SETTING_GNSS      4UL   /**< The GNSS, on (mesh_gnss_begin()).               */
/** @} */

/**
 * @name radio_failed's value
 * @brief The chip's own error word is (value & MESH_RADIO_ERROR_CHIP_MASK); the SDK's
 *        reasons sit above it.
 * @{
 */
#define MESH_RADIO_ERROR_CHIP_MASK   0x0000FFFFUL
#define MESH_RADIO_ERROR_NO_STANDBY  (1UL << 16)  /**< The chip would not confirm standby
                                                *   and was reset: the setting is
                                                *   gone, begin the radio again.      */
#define MESH_RADIO_ERROR_SEND_TIMEOUT  (2UL << 16)  /**< A packet's own timer ran out
                                                *   without a done flag: it may not
                                                *   have gone whole.                 */
#define MESH_RADIO_ERROR_AMPLIFIER_FAULT    (3UL << 16)  /**< The amplifier faulted: the radio
                                                *   is off until the radio is begun, and
                                                *   its band is held to the safe power
                                                *   until mesh_radio_antennas_confirm():
                                                *   is an antenna loose?             */
#define MESH_RADIO_ERROR_RECEIVER    (4UL << 16)  /**< The receiver would not come back on
                                                *   after a packet or a search: it is off
                                                *   (mesh_status_t.receiving says so);
                                                *   turn it on again.                   */
#define MESH_RADIO_ERROR_FLAGS_LOST  (5UL << 16)  /**< The chip's interrupt flags were
                                                *   cleared and could not be read while it
                                                *   was on air: an amplifier fault may be
                                                *   among them, so it is taken as one - the
                                                *   radio is off until it is begun, and its
                                                *   band held to the safe power until
                                                *   mesh_radio_antennas_confirm().      */
/** @} */

/**
 * @name test_ended's value
 * @brief What took the test signal off air.
 * @{
 */
#define MESH_TEST_END_CHIP   1UL  /**< The chip: its own timer, or the preamble finished. */
#define MESH_TEST_END_OWNER  2UL  /**< The SDK's deadline; the chip's timer did not.      */
/** @} */

/* ---- Events, for the standard layer's tasks ---------------------------------------- */

/** @brief The most listeners at once (mesh_event_add_listener()). */
#define MESH_LISTENERS_MAX    8U

/** @brief The longest name an event is asked for by, NUL included. */
#define MESH_EVENT_NAME_MAX   32U

/**
 * @brief What a listener is told, and what mesh_event_wait() hands back.
 *
 * @c name and @c text are valid until the listener returns, or until the task that waited
 * waits again.
 */
typedef struct {
    const char *name;    /**< The event's name, lower case: "packet_received".  */
    int32_t     value;   /**< Its number, as the event's entry in mesh.h says.  */
    const char *text;    /**< Its words, English; "" when it has none.          */
} mesh_event_t;

/** @brief A listener: called on the SDK's events task, one at a time and in order. */
typedef void (*mesh_listener_t)(const mesh_event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* MESH_TYPES_H_ */
