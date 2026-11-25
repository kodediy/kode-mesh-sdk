/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_region.h
 * @brief What the Mesh's radio is allowed to do, and the numbers that say so.
 *
 * The LR2021 can reach past its bands' edges and well past any power a
 * licence-exempt band allows, so the SDK does not hand the chip to an app
 * bare: mesh_plan.c holds every transmit (mesh_radio_send) to the limits
 * here. The default is the EU plan; a build that sets CONFIG_MESH_LAB (a
 * Kconfig switch, off by default) lifts it, and is not a build for a firmware
 * that is distributed. The test signal (mesh_radio_test) is that build's
 * alone: an unmodulated carrier or a bare preamble for an analyser, not
 * duty-counted, which a build that keeps these limits refuses. It is
 * bounded in time (MESH_BSP_TEST_MAX_MS, and the owner takes it off air at its
 * deadline) and it goes into a 50 ohm load ONLY, never an antenna: a carrier
 * on an antenna on the 868 sub-band is 100 % of the time on air against a 1 %
 * limit.
 *
 * The power figures are NOMINAL (the scale tx_power is set on), not a level at
 * the SMA. This file is the one place these numbers live, and the driver
 * follows.
 *
 * ---- Sub-GHz 868 ------------------------------------------------------------
 * The default sub-band is 868.0-868.6 MHz: at most 1 % of the time on air,
 * 600 kHz occupied bandwidth - which BW125 fits and BW250 does not. The 1 % is
 * 36 seconds an hour, and mesh_plan.c counts airtime against it with a budget
 * (MESH_REGION_SUB_GHZ_DUTY_PPT says exactly what that budget allows). Other
 * sub-bands of 863-870 MHz have their own power and duty rules; the SDK offers
 * this one and refuses the rest.
 *
 * ---- 2.4 GHz -----------------------------------------------------------------
 * LoRa on the HF path, one band, 2400-2483.5 MHz, with no duty-cycle count.
 *
 * ---- Outside Europe ----
 * There is no profile for any other region here: no 902-928 MHz band. 868 MHz
 * is a European band; check your country's rules before transmitting.
 */
#ifndef MESH_REGION_H_
#define MESH_REGION_H_

#include <stdbool.h>
#include <stdint.h>

/* CONFIG_MESH_LAB below: without sdkconfig.h the switch would never be seen
 * and a lab build would keep the caps. */
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sub-GHz 868 ----------------------------------------------------------- */
/** @brief The default sub-GHz centre: the middle of 868.0-868.6 MHz. */
#define MESH_REGION_SUB_GHZ_CENTRE_HZ         868300000U
/** @brief The sub-band's edges, for a frequency an app asks for. */
#define MESH_REGION_SUB_GHZ_LOW_HZ      868000000U
#define MESH_REGION_SUB_GHZ_HIGH_HZ      868600000U
/** @brief The sub-GHz power cap, hundredths of a dBm: 8.5 dBm, NOMINAL. A
 *  request above this cap is held to it. */
#define MESH_REGION_SUB_GHZ_CDBM       850
/** @brief The same cap in tenths of a dBm, the facade's scale (mesh_radio_begin()). */
#define MESH_REGION_SUB_GHZ_DBM_X10    (MESH_REGION_SUB_GHZ_CDBM / 10)
/** @brief The chip's tx_power setting for that cap, 8.5 dBm nominal. The
 *  amplifier row below stays the same at every power. */
#define MESH_REGION_SUB_GHZ_TX_POWER   19
/** @brief The duty-cycle rate for the sub-band, in parts per thousand of real
 *  time: 1 % = 10. mesh_plan.c keeps it with a budget that holds one hour's
 *  allowance (36 s), starts full and gets 36 s back per hour. Over a long run
 *  that is 1 % of the time on air. It is not "36 s in any 3600 s": from a
 *  full budget up to 72 s can go out in the first hour, the 36 s it held and
 *  the 36 s that come back meanwhile. The budget lives in RAM, so an app that
 *  restarts starts with it full. */
#define MESH_REGION_SUB_GHZ_DUTY_PPT   10U
/** @brief The PA row for the sub-GHz amplifier: duty 5, slices 7. It is the
 *  LR2021 915 MHz table's 18 dBm row, kept as the power walks down; the
 *  datasheet has no 868 table. The amplifier row the module uses at this
 *  power; a made-up pair can damage the PA. */
#define MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY    5U
#define MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES  7U
/** @brief Sub-GHz transmits at BW125: the 868 sub-band's 600 kHz occupied
 *  bandwidth holds a 125 kHz channel and not a 250 kHz one. */
#define MESH_REGION_SUB_GHZ_BANDWIDTH         0x04U   /**< BW125 code.                      */
#define MESH_REGION_SUB_GHZ_BANDWIDTH_HZ      125000U /**< The same, in hertz.              */

/* ---- 2.4 GHz --------------------------------------------------------------- */
/** @brief The default 2.4 GHz centre: the middle of the band. */
#define MESH_REGION_2_4_GHZ_CENTRE_HZ    2442000000U
/** @brief The 2.4 GHz band's edges. */
#define MESH_REGION_2_4_GHZ_LOW_HZ       2400000000U
#define MESH_REGION_2_4_GHZ_HIGH_HZ       2483500000U
/** @brief The 2.4 GHz power cap, hundredths of a dBm: 4.5 dBm, NOMINAL. A
 *  request above it is held to it. */
#define MESH_REGION_2_4_GHZ_CDBM        450
/** @brief The same cap in tenths of a dBm, the facade's scale (mesh_radio_begin()). */
#define MESH_REGION_2_4_GHZ_DBM_X10     (MESH_REGION_2_4_GHZ_CDBM / 10)
/** @brief The chip's tx_power setting for that cap, 4.5 dBm nominal. */
#define MESH_REGION_2_4_GHZ_TX_POWER    13
/** @brief The PA duty for the 2.4 GHz amplifier: the table's 10 dBm row (the
 *  HF path takes a duty, no slices), kept as the power walks down. */
#define MESH_REGION_2_4_GHZ_AMPLIFIER_DUTY     30U
/** @brief The 2.4 GHz band has no occupied-bandwidth rule like the 868
 *  sub-band, so the HF path transmits at 812.5 kHz, the wide LoRa the chip
 *  codes the SX128x's way. */
#define MESH_REGION_2_4_GHZ_BANDWIDTH          0x0FU   /**< BW812 code.                      */
#define MESH_REGION_2_4_GHZ_BANDWIDTH_HZ       812500U /**< The same, in hertz.              */

/* ---- The packet, on both bands -------------------------------------------- */
/** @brief The packet's shape in the default build: coding rate 4/5, eight
 *  preamble symbols and the chip's default, private, sync word (table 9-7). */
#define MESH_REGION_CODING_RATE         5U      /**< The N of 4/N.      */
#define MESH_REGION_PREAMBLE            8U      /**< Preamble symbols.  */
#define MESH_REGION_SYNC_WORD           0x12U   /**< The private word.  */

/** @brief Is the build one with the limits above lifted? It is the Kconfig switch
 *  MESH_LAB of mesh_aos (components/mesh_aos/Kconfig), off by default; its help
 *  says what it lifts. It is not a build for a firmware that is distributed. */
#ifdef CONFIG_MESH_LAB
#define MESH_REGION_LAB            1
#else
#define MESH_REGION_LAB            0
#endif

/** @brief Does a LAB build drive the amplifier with the datasheet's rows above the
 *  default one (19 dBm on LF, 10 dBm on HF)? The Kconfig switch MESH_LAB_PA_TABLE,
 *  off by default and only with MESH_LAB. Those rows are the datasheet's; use them
 *  at your own risk. Without it a LAB build holds the power to the top of the
 *  default row. */
#if defined(CONFIG_MESH_LAB) && defined(CONFIG_MESH_LAB_PA_TABLE)
#define MESH_REGION_LAB_PA_TABLE   1
#else
#define MESH_REGION_LAB_PA_TABLE   0
#endif

/* ---- The antennas ---------------------------------------------------------- */
/** @brief The safe power, hundredths of a dBm: every transmit, on both bands, until the app
 *  confirms the antennas (mesh_radio_antennas_confirm()), and again on a band whose amplifier
 *  tripped its over-current alarm until the app confirms once more. The Kconfig switch
 *  MESH_SAFE_POWER_CDBM of mesh_aos (components/mesh_aos/Kconfig), 0 dBm by default. Raise
 *  it only with an antenna or a 50 ohm load fitted. It is not a limit of the
 *  region, and CONFIG_MESH_LAB does not lift it. It only lowers: a power asked below it is
 *  what goes out. */
#ifdef CONFIG_MESH_SAFE_POWER_CDBM
#define MESH_REGION_SAFE_CDBM      CONFIG_MESH_SAFE_POWER_CDBM
#else
#define MESH_REGION_SAFE_CDBM      0
#endif
/** @brief The same in tenths of a dBm, the facade's scale. */
#define MESH_REGION_SAFE_DBM_X10   (MESH_REGION_SAFE_CDBM / 10)

#ifdef __cplusplus
}
#endif

#endif /* MESH_REGION_H_ */
