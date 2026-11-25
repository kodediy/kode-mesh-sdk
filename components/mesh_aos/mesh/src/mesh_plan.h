/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_plan.h
 * @brief The owner's rules, in plain C: what a transmit is allowed to be, how
 *        much of the hour is left to send it in, and what a Mesh looks like on
 *        J3.
 *
 * This computes; it touches no chip and no clock. The owner gives it the band
 * and the setting an app asked for and a timestamp, and it answers whether
 * that transmit is within the region's limits and the duty cycle, and with
 * what amplifier settings. The limits are mesh_region.h's; the amplifier rows
 * are the chip's datasheet tables and are never made up (a wrong duty or
 * slice count damages the amplifier). LoRa's own arithmetic - airtime,
 * timers - is mesh_drv_lora.h's. It is built on the host and run as it is
 * (tools/check_layers.sh, rule 4).
 */
#ifndef MESH_PLAN_H_
#define MESH_PLAN_H_

#include <stdbool.h>
#include <stdint.h>

#include "mesh_drv_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The two bands the module has, each its own amplifier and SMA. */
typedef enum {
    MESH_PLAN_BAND_SUB_GHZ = 0, /**< Sub-GHz (868) on J501. */
    MESH_PLAN_BAND_2_4_GHZ,     /**< 2.4 GHz on J502.       */
    MESH_PLAN_BAND_COUNT
} mesh_plan_band_t;

/**
 * @brief Build the transmit setting for @p band at @p spreading_factor and at
 *        @p power_cdbm hundredths of a dBm, or less.
 *
 * The frequency is the band's default centre; @p spreading_factor is
 * refused outside 7..12 (5..12 in a LAB build), as mesh_plan_radio_settings() refuses it; the bandwidth and the amplifier row are the region's for that
 * band; tx_power is the region's cap or @p power_cdbm, whichever is lower,
 * never above the cap. A build with CONFIG_MESH_LAB takes @p power_cdbm up to the
 * chip's top, and changes amplifier row above what the region's row reaches in
 * the datasheet's tables (mesh_plan.c). The low data-rate option is
 * mesh_drv_lora_low_data_rate()'s: a symbol over 16 ms, or SF11 and SF12 at
 * BW203, BW406 or BW812.
 * @param band             The band.
 * @param spreading_factor 7..12 (5..12 in a LAB build).
 * @param power_cdbm       The power asked for, hundredths of a dBm.
 * @param[out] out         The setting.
 * @return true for a known band and a spreading factor in range; false for
 *         MESH_PLAN_BAND_COUNT, a spreading factor out of range or no @p out.
 */
bool mesh_plan_transmit_settings(mesh_plan_band_t band, uint8_t spreading_factor,
                                 int32_t power_cdbm, mesh_drv_lora_settings_t *out);

/**
 * @brief The power a transmit may be planned for, before the region's cap: @p power_cdbm, or
 *        while the antennas are not confirmed no more than the safe power
 *        (MESH_REGION_SAFE_CDBM, CONFIG_MESH_SAFE_POWER_CDBM). A LAB build is held the same.
 * @param power_cdbm         The power asked, hundredths of a dBm.
 * @param antennas_confirmed The app confirmed the antennas and the band's amplifier has not
 *                           tripped since.
 * @return What to plan for: @p power_cdbm, or the lower of it and the safe power.
 */
int32_t mesh_plan_power_allowed(int32_t power_cdbm, bool antennas_confirmed);

/**
 * @brief Plan only the power of @p settings for @p band: the amplifier row and tx_power for
 *        @p power_cdbm, as mesh_plan_transmit_settings() plans them; the rest is left as it is.
 * @return true; false for MESH_PLAN_BAND_COUNT or no @p settings.
 */
bool mesh_plan_power(mesh_plan_band_t band, int32_t power_cdbm, mesh_drv_lora_settings_t *settings);

/**
 * @brief The power a setting really puts out, hundredths of a dBm on the
 *        nominal scale: what the amplifier row and tx_power of @p settings come to,
 *        which is what was asked after the cap and the half-dB steps have had their say.
 */
int32_t mesh_plan_power_applied(mesh_plan_band_t band, const mesh_drv_lora_settings_t *settings);

/**
 * @brief Build the setting for a whole config, as mesh_radio_configure() asks.
 *
 * Without CONFIG_MESH_LAB the frequency, with the bandwidth around it, stays in
 * the band's edges, the bandwidth is the band's, the spreading factor is 7..12
 * and the coding rate, the preamble and the sync word are the region's. With it,
 * they may be anything the chip has. The power is treated as
 * mesh_plan_transmit_settings() treats it.
 * @param ask      What is asked.
 * @param[out] out The setting.
 * @return true when @p out holds it; false for a setting this build refuses, and
 *         @p out is then not to be used.
 */
bool mesh_plan_radio_settings(const mesh_drv_lora_config_t *ask, mesh_drv_lora_settings_t *out);

/**
 * @brief Whether a packet may go with a preamble of its own: under
 *        CONFIG_MESH_LAB, any but 0; otherwise only the region's.
 */
bool mesh_plan_preamble_allowed(uint16_t preamble);

/**
 * @brief Whether this build may put a test signal on air (mesh_radio_test()): only under
 *        CONFIG_MESH_LAB. A carrier is 100 % of the time on air and the duty budget does not
 *        count it, so a build that keeps the region's limits has none.
 */
bool mesh_plan_test_allowed(void);

/** @brief The band a frequency falls in; the chip's two paths meet at
 *         MESH_BSP_2_4_GHZ_FROM_HZ. */
mesh_plan_band_t mesh_plan_band_of(uint32_t hz);

/**
 * @brief Whether @p hz is inside @p band's allowed range: the region's edges, or
 *        under CONFIG_MESH_LAB the whole of the chip's path for that band.
 */
bool mesh_plan_frequency_allowed(mesh_plan_band_t band, uint32_t hz);

/**
 * @brief Whether the SDK counts this setting's airtime against the duty cycle.
 * @param band       The band in force.
 * @param configured It came through mesh_plan_radio_settings(): under
 *                   CONFIG_MESH_LAB the app then keeps its region's duty cycle.
 * @return true on the sub-GHz band, unless both of those hold.
 */
bool mesh_plan_duty_counted(mesh_plan_band_t band, bool configured);

/* ---- The duty cycle --------------------------------------------------------
 *
 * The 868 sub-band's limit is a fraction of each hour on air.
 * A token bucket keeps it: the bucket fills at the band's allowed fraction of
 * real time, holds at most one hour's worth, and each transmit spends its
 * airtime. A transmit is allowed when the bucket holds its airtime. The
 * 2.4 GHz band has no such limit, and its bucket is not consulted.
 *
 * It is a token bucket, not a sliding-window log: it caps the long-run average
 * but allows a burst of up to a whole hour's allowance, so across one sliding
 * hour the airtime can reach twice the fraction: at 1 %, 72 s in the hour that
 * begins with a full bucket. The bucket is RAM, and a restart of the app
 * starts it full. Counting "36 s in any 3600 s" instead would be a different
 * rule, a sliding window, and is not what this does. */
typedef struct {
    uint64_t budget_us; /**< Tokens now, in microseconds of airtime.  */
    uint64_t cap_us;    /**< The most it holds: one hour's allowance. */
    uint32_t ppt;       /**< Fill rate, parts per thousand of real time. */
    uint32_t last_ms;   /**< When it was last refilled.               */
    bool     started;   /**< First use seeds last_ms without filling. */
} mesh_plan_duty_t;

/** @brief Set @p duty up for a band whose ceiling is @p ppt parts per thousand. */
void mesh_plan_duty_init(mesh_plan_duty_t *duty, uint32_t ppt);

/**
 * @brief Whether a transmit of @p airtime_us is within the duty cycle at
 *        @p now_ms. Refills the bucket to now; does not spend it.
 */
bool mesh_plan_duty_allows(mesh_plan_duty_t *duty, uint32_t airtime_us, uint32_t now_ms);

/** @brief Spend @p airtime_us from the bucket at @p now_ms (after _allows()). */
void mesh_plan_duty_charge(mesh_plan_duty_t *duty, uint32_t airtime_us, uint32_t now_ms);

/** @brief Milliseconds until the bucket holds @p airtime_us; 0 if it does now. */
uint32_t mesh_plan_duty_wait_ms(const mesh_plan_duty_t *duty, uint32_t airtime_us);

/* ---- The fingerprint -------------------------------------------------------
 *
 * The owner reads the four pull-up positions against the P4's pull-downs and
 * passes their levels as a bitmap, bit (position - 1) set when that position
 * read high. A Mesh holds SCL, SDA, the chip select and the LoRa reset high.
 * This says whether a reading looks like a Mesh; the owner does the rounds
 * and the radio handshake that confirm it. */

/** @brief Whether @p level_bits looks like a Mesh. */
bool mesh_plan_fingerprint_matches(uint16_t level_bits);

/** @brief The bitmap a Mesh's fingerprint should match (for a test's sake). */
uint16_t mesh_plan_fingerprint_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_PLAN_H_ */
