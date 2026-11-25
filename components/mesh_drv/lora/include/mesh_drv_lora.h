/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_lora.h
 * @brief LoRa's arithmetic, in plain C: how long a packet lasts, how the
 *        LR2021's own timers count, and the setting the chip is handed.
 *
 * This computes; it touches no chip and no clock. The LR2021 driver
 * (mesh_drv_lr2021.h) takes a mesh_drv_lora_settings_t and these numbers;
 * the owner's plan (mesh_plan.h) builds the setting from the region's limits.
 * It is built on the host and run as it is (tools/check_layers.sh, rule 4).
 */
#ifndef MESH_DRV_LORA_H_
#define MESH_DRV_LORA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The LoRa bandwidths, as the codes the chip takes (datasheet, table 9-3).
 * @{ */
#define MESH_DRV_LORA_BANDWIDTH_31    0x02U  /**< 31.25 kHz.    */
#define MESH_DRV_LORA_BANDWIDTH_62    0x03U  /**< 62.5 kHz.     */
#define MESH_DRV_LORA_BANDWIDTH_125   0x04U  /**< 125 kHz.      */
#define MESH_DRV_LORA_BANDWIDTH_250   0x05U  /**< 250 kHz.      */
#define MESH_DRV_LORA_BANDWIDTH_500   0x06U  /**< 500 kHz.      */
#define MESH_DRV_LORA_BANDWIDTH_1000  0x07U  /**< 1000 kHz.     */
#define MESH_DRV_LORA_BANDWIDTH_41    0x0AU  /**< 41.67 kHz.    */
#define MESH_DRV_LORA_BANDWIDTH_83    0x0BU  /**< 83.34 kHz.    */
#define MESH_DRV_LORA_BANDWIDTH_101   0x0CU  /**< 101.5625 kHz. */
#define MESH_DRV_LORA_BANDWIDTH_203   0x0DU  /**< 203.125 kHz.  */
#define MESH_DRV_LORA_BANDWIDTH_406   0x0EU  /**< 406.25 kHz.   */
#define MESH_DRV_LORA_BANDWIDTH_812   0x0FU  /**< 812.5 kHz.    */
/** @} */

/** @brief The coding rates the driver takes, as the chip's codes (table 9-4): 1
 *         is 4/5, 4 is 4/8. The long interleaver's codes are not offered. */
#define MESH_DRV_LORA_CODING_4_5      1U
#define MESH_DRV_LORA_CODING_4_8      4U

/** @brief The longest LoRa payload one packet carries. */
#define MESH_DRV_LORA_PAYLOAD_MAX     255U

/**
 * @brief A validated transmit setting: what the LR2021 driver is handed.
 *
 * Built by mesh_plan_transmit_settings() or mesh_plan_radio_settings(); never
 * filled by an app. The fields are the chip's: the centre frequency, which of
 * its two paths, the spreading factor, the bandwidth and coding-rate codes, the
 * preamble and the sync word, the low data-rate option, the signed tx_power in
 * half-dB steps, and the amplifier's duty and slices.
 */
typedef struct {
    uint32_t frequency_hz;      /**< The centre frequency.                       */
    bool     band_2_4_ghz;      /**< The 2.4 GHz path (J502); false is sub-GHz.  */
    uint8_t  spreading_factor;  /**< 5..12.                                      */
    uint8_t  bandwidth;         /**< A MESH_DRV_LORA_BANDWIDTH_* code.           */
    uint8_t  coding_rate;       /**< MESH_DRV_LORA_CODING_4_5 .. _4_8.           */
    uint16_t preamble;          /**< Preamble symbols, at least 1.               */
    uint8_t  sync_word;         /**< The byte SetLoraSyncword takes.             */
    bool     low_data_rate;     /**< The low data-rate option.                   */
    int8_t   tx_power;          /**< The chip's tx_power, half-dB steps.         */
    uint8_t  amplifier_duty;    /**< The amplifier row's duty (datasheet).       */
    uint8_t  amplifier_slices;  /**< Its slices; 0 on the 2.4 GHz path.          */
} mesh_drv_lora_settings_t;

/**
 * @brief A whole setting as an app asks for it, in the world's units, on its way
 *        from the facade to the owner's plan.
 */
typedef struct {
    uint32_t frequency_hz;      /**< The centre frequency.                       */
    uint32_t bandwidth_hz;      /**< The bandwidth.                              */
    uint8_t  spreading_factor;  /**< 5..12.                                      */
    uint8_t  coding_rate;       /**< The N of 4/N, 5..8.                         */
    uint16_t preamble;          /**< Preamble symbols.                           */
    uint8_t  sync_word;         /**< The LoRa sync word.                         */
    int32_t  power_cdbm;        /**< The power asked, hundredths of a dBm.       */
} mesh_drv_lora_config_t;

/**
 * @brief The chip's code for a bandwidth in Hz.
 * @param hz 31250 .. 1000000, within half a percent of a bandwidth the chip has,
 *           so the rounded 41670, 83340 and 101562 that other radios use all fit.
 * @return A MESH_DRV_LORA_BANDWIDTH_* code; 0 when the chip has no such bandwidth.
 */
uint8_t mesh_drv_lora_bandwidth_code(uint32_t hz);

/** @brief The bandwidth a code stands for, Hz; 0 for a code the chip does not have. */
uint32_t mesh_drv_lora_bandwidth_hz(uint8_t bandwidth);

/**
 * @brief A LoRa packet's airtime, Semtech's formula: eight preamble symbols,
 *        an explicit header, a CRC, coding rate 4/5.
 * @param spreading_factor 7..12.
 * @param bandwidth        A MESH_DRV_LORA_BANDWIDTH_* code.
 * @param length           Payload bytes, 1..255.
 * @param low_data_rate    The low data-rate option.
 * @return Microseconds; 0 for a bandwidth code it does not know.
 */
uint32_t mesh_drv_lora_airtime_us(uint8_t spreading_factor, uint8_t bandwidth, uint8_t length,
                                  bool low_data_rate);

/**
 * @brief A LoRa packet's airtime with every number given: Semtech's formula, an
 *        explicit header and a CRC.
 * @param spreading_factor 5..12.
 * @param bandwidth        A MESH_DRV_LORA_BANDWIDTH_* code.
 * @param coding_rate      MESH_DRV_LORA_CODING_4_5 .. _4_8.
 * @param preamble         Preamble symbols, at least 1.
 * @param length           Payload bytes, 1..255.
 * @param low_data_rate    The low data-rate option.
 * @return Microseconds, at most UINT32_MAX; 0 for an argument out of range.
 */
uint32_t mesh_drv_lora_packet_airtime_us(uint8_t spreading_factor, uint8_t bandwidth,
                                         uint8_t coding_rate, uint16_t preamble, uint8_t length,
                                         bool low_data_rate);

/** @brief Whether @p spreading_factor at @p bandwidth needs the low data-rate
 *         option, as the datasheet recommends (9.9.1): a symbol over 16 ms,
 *         or SF11 and SF12 at the SX1280's bandwidths (BW203, BW406, BW812)
 *         however short the symbol. Both ends of a link must agree on it. */
bool mesh_drv_lora_low_data_rate(uint8_t spreading_factor, uint8_t bandwidth);

/** @brief Channel-activity-detection symbols for @p spreading_factor: 4 up to
 *         SF11, 3 at SF12, where four give too many false detections. */
uint8_t mesh_drv_lora_cad_symbols(uint8_t spreading_factor);

/** @brief The detection threshold the datasheet recommends (table 6-19).
 * @param spreading_factor 5..12.
 * @param symbols          Symbols searched, 1..4.
 * @return det_peak; 55, the chip's default, for anything outside the table. */
uint8_t mesh_drv_lora_cad_peak(uint8_t spreading_factor, uint8_t symbols);

/**
 * @brief How long a CAD of @p symbols lasts, by the datasheet: that many symbols.
 * @return Microseconds; 0 for a bandwidth code or a spreading factor it does not know.
 */
uint32_t mesh_drv_lora_cad_us(uint8_t spreading_factor, uint8_t bandwidth, uint8_t symbols);

/**
 * @brief The longest a packet on this setting can have after its header, and how
 *        long a preamble can be expected to go on, for a timer on a packet that was
 *        announced and never finished.
 * @return Microseconds of @p symbols of this setting.
 */
uint32_t mesh_drv_lora_symbols_us(uint8_t spreading_factor, uint8_t bandwidth, uint32_t symbols);

/* ---- The chip's own timers -------------------------------------------------
 *
 * The LR2021 counts a transmit's or a receive's timeout in periods of its
 * 32.768 kHz RTC, in 24 bits: at most 512 s. A count of 0 does NOT mean "as
 * short as possible" - for SetTx it switches the timeout off, and a transmit
 * with no timeout is one nothing stops if its done flag is lost. So every
 * conversion here returns at least 1. */

/** @brief The chip's timer clock. */
#define MESH_DRV_LORA_TIMER_HZ        32768U
/** @brief The most the timer counts: 2^24 - 1 periods, 512 s. */
#define MESH_DRV_LORA_TIMER_MAX       0xFFFFFFU
/** @brief The longest LoRa preamble the chip sends, in symbols. */
#define MESH_DRV_LORA_PREAMBLE_MAX    65535U
/** @brief The shortest preamble this SDK sends, in symbols. */
#define MESH_DRV_LORA_PREAMBLE_MIN    8U

/**
 * @brief @p us microseconds as periods of the chip's 32.768 kHz timer,
 *        rounded up, from 1 to MESH_DRV_LORA_TIMER_MAX.
 */
uint32_t mesh_drv_lora_timer_ticks(uint32_t us);

/**
 * @brief The timeout to give SetTx for a packet of @p airtime_us: the airtime
 *        plus a quarter of it (at least 20 ms), as timer periods. It is the
 *        chip's own safeguard if a transmit never ends, so it must outlast an
 *        honest packet and not much more; never 0.
 */
uint32_t mesh_drv_lora_send_timeout_ticks(uint32_t airtime_us);

/**
 * @brief The preamble length that keeps a LoRa preamble on air for at least
 *        @p ms at @p spreading_factor and @p bandwidth, clamped to
 *        MESH_DRV_LORA_PREAMBLE_MIN .. MESH_DRV_LORA_PREAMBLE_MAX.
 * @return Symbols; 0 for a bandwidth code or a spreading factor it does not
 *         know.
 */
uint16_t mesh_drv_lora_preamble_symbols(uint8_t spreading_factor, uint8_t bandwidth, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* MESH_DRV_LORA_H_ */
