/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_plan.c
 * @brief mesh_plan: the owner's rules, run without a chip or a clock.
 *
 * Each case is a rule from mesh_plan.h or mesh_region.h: the power held to the
 * region's cap and stepped down in half-dBs, the band edges, the 1 % duty
 * cycle as a token bucket, and the four-pull-up fingerprint. LoRa's own
 * arithmetic - airtime, the chip's timers - is test_lora.c's. Nothing here
 * needs the module; this is what tools/check_layers.sh rule 4 keeps buildable
 * on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "mesh_plan.h"
#include "mesh_region.h"










/* ---- The transmit setting ---------------------------------------------------- */

TEST_CASE("tx: sub-GHz at the cap sits at the calibrated row", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    memset(&tx, 0xAA, sizeof(tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_SUB_GHZ_CENTRE_HZ, tx.frequency_hz);
    TEST_ASSERT_FALSE(tx.band_2_4_ghz);
    TEST_ASSERT_EQUAL_UINT8(9U, tx.spreading_factor);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_BANDWIDTH, tx.bandwidth);
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES, tx.amplifier_slices);
}

TEST_CASE("tx: a power under the cap steps tx_power down a half-dB at a time", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    /* 5 dB under the cap = 10 half-dB steps below the calibrated row. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM - 500, &tx));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER - 10, tx.tx_power);
    /* 1 dB under = 2 steps. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM - 100, &tx));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER - 2, tx.tx_power);
}

#if !MESH_REGION_LAB
TEST_CASE("tx: a power over the cap is held to the cap (shipped build)", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 9999, &tx));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER, tx.tx_power);
}
#endif

TEST_CASE("tx: the 2.4 GHz path takes the HF band, row and bandwidth", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 7, MESH_REGION_2_4_GHZ_CDBM, &tx));
    TEST_ASSERT_TRUE(tx.band_2_4_ghz);
    TEST_ASSERT_TRUE(tx.frequency_hz >= MESH_BSP_2_4_GHZ_FROM_HZ);
    TEST_ASSERT_TRUE(tx.frequency_hz >= MESH_REGION_2_4_GHZ_LOW_HZ && tx.frequency_hz <= MESH_REGION_2_4_GHZ_HIGH_HZ);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_2_4_GHZ_BANDWIDTH, tx.bandwidth);
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_2_4_GHZ_TX_POWER, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_2_4_GHZ_AMPLIFIER_DUTY, tx.amplifier_duty);
}

TEST_CASE("tx: an unknown band is refused and leaves out untouched", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    memset(&tx, 0x5A, sizeof(tx));
    mesh_drv_lora_settings_t before = tx;
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_COUNT, 7, 0, &tx));
    TEST_ASSERT_EQUAL_MEMORY(&before, &tx, sizeof(tx));
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 7, 0, NULL));
}

TEST_CASE("tx: a spreading factor outside 7..12 is refused, as configure refuses it", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    mesh_drv_lora_settings_t const before = { .spreading_factor = 99U };

    tx = before;
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 2, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 6, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 13, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 255, MESH_REGION_2_4_GHZ_CDBM, &tx));
    TEST_ASSERT_EQUAL_UINT8(99U, tx.spreading_factor);   /* nothing written */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 7, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_EQUAL_UINT8(7U, tx.spreading_factor);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 12, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_EQUAL_UINT8(12U, tx.spreading_factor);
}

TEST_CASE("power applied: the nominal power the row and tx_power come to", "[plan]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 5000, &tx));   /* asks 50 dBm */
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_CDBM, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 700, &tx));
    TEST_ASSERT_EQUAL_INT32(700, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 703, &tx));    /* half-dB steps, at or below */
    TEST_ASSERT_EQUAL_INT32(700, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 5000, &tx));
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_2_4_GHZ_CDBM, mesh_plan_power_applied(MESH_PLAN_BAND_2_4_GHZ, &tx));
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_2_4_GHZ, NULL));
}

TEST_CASE("tx: the low data-rate option rides on the chosen spreading factor and bandwidth", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 12, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_TRUE(tx.low_data_rate);                            /* SF12 BW125 */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM, &tx));
    TEST_ASSERT_FALSE(tx.low_data_rate);                           /* SF9 BW125  */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 11, MESH_REGION_2_4_GHZ_CDBM, &tx));
    TEST_ASSERT_TRUE(tx.low_data_rate);                            /* SF11 BW812, Meshtastic's LongFast */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 10, MESH_REGION_2_4_GHZ_CDBM, &tx));
    TEST_ASSERT_FALSE(tx.low_data_rate);                           /* SF10 BW812 */
}

TEST_CASE("tx: a wild power request never asks for more than the cap", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    /* INT32_MIN once overflowed cap - want and chose the loudest row; it must
     * clamp to the quietest, and never sit above either band's cap. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, INT32_MIN, &tx));
    TEST_ASSERT_TRUE(tx.tx_power <= MESH_REGION_SUB_GHZ_TX_POWER);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, INT32_MIN, &tx));
    TEST_ASSERT_TRUE(tx.tx_power <= MESH_REGION_2_4_GHZ_TX_POWER);
    /* a huge-negative and a merely-very-low request reach the same floor: the
     * clamp held, nothing wrapped around. */
    mesh_drv_lora_settings_t lo;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM - 100000, &lo));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, INT32_MIN, &tx));
    TEST_ASSERT_EQUAL_INT8(lo.tx_power, tx.tx_power);
}

TEST_CASE("tx: a part-step under the cap rounds the power down, not up", "[plan]")
{
    mesh_drv_lora_settings_t tx;
    /* 0.1 dB under the cap is less than a half-dB step: the power rounds down
     * to the next step, never up to the cap row (the header's "or less"). */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, MESH_REGION_SUB_GHZ_CDBM - 10, &tx));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER - 1, tx.tx_power);
}

/* ---- Frequency within the band ----------------------------------------------- */

#if !MESH_REGION_LAB
TEST_CASE("freq: inside the sub-band is allowed, outside refused", "[plan]")
{
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, MESH_REGION_SUB_GHZ_CENTRE_HZ));
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, MESH_REGION_SUB_GHZ_LOW_HZ));   /* edge */
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, MESH_REGION_SUB_GHZ_HIGH_HZ));   /* edge */
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, MESH_REGION_SUB_GHZ_LOW_HZ - 1U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, MESH_REGION_SUB_GHZ_HIGH_HZ + 1U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, 915000000U));
}

TEST_CASE("freq: inside the 2.4 GHz band is allowed, the sub-band is not", "[plan]")
{
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, 2442000000U));
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, MESH_REGION_2_4_GHZ_LOW_HZ));
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, MESH_REGION_2_4_GHZ_HIGH_HZ));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, MESH_REGION_2_4_GHZ_HIGH_HZ + 1U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, MESH_REGION_SUB_GHZ_CENTRE_HZ));
}

TEST_CASE("freq: an unknown band is refused", "[plan]")
{
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_COUNT, MESH_REGION_SUB_GHZ_CENTRE_HZ));
}
#endif

/* ---- The duty cycle ---------------------------------------------------------- */

TEST_CASE("duty: 1 % is 36 s an hour; a full bucket passes", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    TEST_ASSERT_EQUAL_UINT64(36000000ULL, d.cap_us);   /* 36 s in microseconds */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 50000U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_plan_duty_wait_ms(&d, 50000U));
}

TEST_CASE("duty: a drained bucket refuses, and says how long to wait", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    mesh_plan_duty_charge(&d, 36000000U, 0U);             /* spend the whole hour */
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 10000U, 0U));
    /* 10000 us of airtime, filling at 10 us a ms, is 1000 ms away. */
    TEST_ASSERT_EQUAL_UINT32(1000U, mesh_plan_duty_wait_ms(&d, 10000U));
}

TEST_CASE("duty: the bucket refills with real time", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    mesh_plan_duty_charge(&d, 36000000U, 0U);             /* empty at t=0 */
    /* a second later, 1000 ms x 10 ppt = 10000 us has accrued */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 10000U, 1000U));
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 10001U, 1000U));
}

TEST_CASE("duty: the bucket never fills past the hour's cap", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    /* a whole day later it still holds exactly one hour's airtime */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 36000000U, 86400000U));
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 36000001U, 86400000U));
}

TEST_CASE("duty: a band with no limit always passes and never waits", "[plan]")
{
    mesh_plan_duty_t h;
    mesh_plan_duty_init(&h, 0U);
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&h, 1000000U, 0U));
    mesh_plan_duty_charge(&h, 1000000U, 0U);              /* a no-op */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&h, 1000000U, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_plan_duty_wait_ms(&h, 1000000U));
}

/* ---- The fingerprint --------------------------------------------------------- */

static unsigned popcount16(uint16_t v)
{
    unsigned n = 0;
    while (v) { v &= (uint16_t)(v - 1U); n++; }
    return n;
}

TEST_CASE("fingerprint: the mask is exactly the four pull-up positions", "[plan]")
{
    uint16_t const mask = mesh_plan_fingerprint_mask();
    TEST_ASSERT_EQUAL_UINT(4U, popcount16(mask));
}

TEST_CASE("fingerprint: all four high is a Mesh, any one low is not", "[plan]")
{
    uint16_t const mask = mesh_plan_fingerprint_mask();
    TEST_ASSERT_TRUE(mesh_plan_fingerprint_matches(mask));
    TEST_ASSERT_FALSE(mesh_plan_fingerprint_matches(0U));
    /* drop each required bit in turn: each makes it not a Mesh */
    for (unsigned b = 0; b < 16U; b++) {
        if (mask & (1U << b)) {
            TEST_ASSERT_FALSE(mesh_plan_fingerprint_matches((uint16_t)(mask & ~(1U << b))));
        }
    }
}

TEST_CASE("fingerprint: other positions high do not matter", "[plan]")
{
    uint16_t const mask = mesh_plan_fingerprint_mask();
    /* the four required, plus every other line high: still a Mesh */
    TEST_ASSERT_TRUE(mesh_plan_fingerprint_matches((uint16_t)0xFFFFU));
    /* the four required, with a non-required extra: still a Mesh */
    uint16_t extra = (uint16_t)(mask | (uint16_t)~mask);
    (void)extra;
    TEST_ASSERT_TRUE(mesh_plan_fingerprint_matches(mask));
}

/* ---- Defensive corners of the duty bucket (surfaced by coverage) ------------- */


TEST_CASE("duty: refilling a near-full bucket still caps at the hour", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    /* the first call only seeds the clock (no fill); then spend a little */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 1000U, 1000U));
    mesh_plan_duty_charge(&d, 1000000U, 1000U);           /* budget = cap - 1 s */
    /* an hour on, the refill would add far more than was spent: it clamps to
     * the cap, never above it. */
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 36000000U, 3601000U));
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 36000001U, 3601000U));
}

TEST_CASE("duty: spending more than is left floors the bucket at zero", "[plan]")
{
    mesh_plan_duty_t d;
    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    TEST_ASSERT_TRUE(mesh_plan_duty_allows(&d, 0U, 0U));       /* seed the clock */
    /* ask to spend more than a whole hour: the bucket floors at zero, it does
     * not wrap below it. */
    mesh_plan_duty_charge(&d, 46000000U, 0U);
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 1U, 0U));
}

/* ---- The EU power caps ------------------------------------------------------ *
 *
 * The caps are tx_power 19 on 868 MHz and 13 on 2.4 GHz (8.5 and 4.5 dBm
 * nominal). They are spelled out here as numbers, not as the macros, on
 * purpose: a case that follows the macro cannot notice the macro being raised.
 * Raising one has to cost a failing test and a reason. */

#if !MESH_REGION_LAB
TEST_CASE("test signal: a build that keeps the region's limits has none", "[plan][region]")
{
    /* A carrier is 100 % of the time on air and no duty budget counts it: only a lab build
     * (CONFIG_MESH_LAB, tests/host_lab) may put one on air. */
    TEST_ASSERT_EQUAL_INT(0, MESH_REGION_LAB);
    TEST_ASSERT_FALSE(mesh_plan_test_allowed());
}

TEST_CASE("region: the caps are 8.5 / 4.5 dBm nominal, tx_power 19 / 13", "[plan][region]")
{
    TEST_ASSERT_EQUAL_INT32(850, MESH_REGION_SUB_GHZ_CDBM);
    TEST_ASSERT_EQUAL_INT(19, MESH_REGION_SUB_GHZ_TX_POWER);
    TEST_ASSERT_EQUAL_INT32(450, MESH_REGION_2_4_GHZ_CDBM);
    TEST_ASSERT_EQUAL_INT(13, MESH_REGION_2_4_GHZ_TX_POWER);
    /* the amplifier rows stay the same at every tx_power */
    TEST_ASSERT_EQUAL_UINT8(5U, MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY);
    TEST_ASSERT_EQUAL_UINT8(7U, MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES);
    TEST_ASSERT_EQUAL_UINT8(30U, MESH_REGION_2_4_GHZ_AMPLIFIER_DUTY);
}

TEST_CASE("power 868: a 12.5 dBm ask and every ask above the cap give tx_power 19", "[plan][region]")
{
    mesh_drv_lora_settings_t tx;
    int32_t const asks[] = { 850, 851, 900, 1000, 1250, 1800, 2000, 9999, INT32_MAX };
    for (unsigned i = 0; i < sizeof(asks) / sizeof(asks[0]); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, asks[i], &tx));
        TEST_ASSERT_EQUAL_INT8(19, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);
        TEST_ASSERT_EQUAL_UINT8(7U, tx.amplifier_slices);
    }
}

TEST_CASE("power 868: below the cap it steps down a half-dB at a time from 19", "[plan][region]")
{
    mesh_drv_lora_settings_t tx;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 800, &tx));   /* 0.5 dB down */
    TEST_ASSERT_EQUAL_INT8(18, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 750, &tx));
    TEST_ASSERT_EQUAL_INT8(17, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 0, &tx));     /* 8.5 dB down */
    TEST_ASSERT_EQUAL_INT8(2, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 840, &tx));   /* part-step: rounds down */
    TEST_ASSERT_EQUAL_INT8(18, tx.tx_power);
}

TEST_CASE("power 2.4 GHz: a 5.5 dBm ask and every ask above the cap give tx_power 13", "[plan][region]")
{
    mesh_drv_lora_settings_t tx;
    int32_t const asks[] = { 450, 451, 550, 1000, 1250, 2000, 9999, INT32_MAX };
    for (unsigned i = 0; i < sizeof(asks) / sizeof(asks[0]); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, asks[i], &tx));
        TEST_ASSERT_EQUAL_INT8(13, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(30U, tx.amplifier_duty);
    }
}

TEST_CASE("power 2.4 GHz: below the cap it steps down from 13", "[plan][region]")
{
    mesh_drv_lora_settings_t tx;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 400, &tx));
    TEST_ASSERT_EQUAL_INT8(12, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 0, &tx));      /* 4.5 dB down = 9 steps */
    TEST_ASSERT_EQUAL_INT8(4, tx.tx_power);
}

TEST_CASE("power: no ask, however wild, puts tx_power over 19 (868) or 13 (2.4 GHz)", "[plan][region]")
{
    mesh_drv_lora_settings_t tx;
    for (int32_t ask = -30000; ask <= 30000; ask += 10) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 12, ask, &tx));
        TEST_ASSERT_TRUE(tx.tx_power <= 19);
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 7, ask, &tx));
        TEST_ASSERT_TRUE(tx.tx_power <= 13);
    }
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, INT32_MAX, &tx));
    TEST_ASSERT_TRUE(tx.tx_power <= 19);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, INT32_MAX, &tx));
    TEST_ASSERT_TRUE(tx.tx_power <= 13);
}
#endif

/* ---- A whole setting, in the default build -------------------------------------- */

/** @brief The region's setting for a band, as mesh_radio_config_default() gives it. */
static mesh_drv_lora_config_t region_config(mesh_plan_band_t band)
{
    bool const low = (band == MESH_PLAN_BAND_SUB_GHZ);
    mesh_drv_lora_config_t c = {
        .frequency_hz     = low ? MESH_REGION_SUB_GHZ_CENTRE_HZ : MESH_REGION_2_4_GHZ_CENTRE_HZ,
        .bandwidth_hz     = low ? MESH_REGION_SUB_GHZ_BANDWIDTH_HZ : MESH_REGION_2_4_GHZ_BANDWIDTH_HZ,
        .spreading_factor = 9U,
        .coding_rate      = MESH_REGION_CODING_RATE,
        .preamble         = MESH_REGION_PREAMBLE,
        .sync_word        = MESH_REGION_SYNC_WORD,
        .power_cdbm       = low ? MESH_REGION_SUB_GHZ_CDBM : MESH_REGION_2_4_GHZ_CDBM,
    };
    return c;
}

TEST_CASE("config: the region's setting comes out as mesh_radio_begin()'s does", "[plan]")
{
    for (int b = 0; b < (int)MESH_PLAN_BAND_COUNT; b++) {
        mesh_plan_band_t const band = (mesh_plan_band_t)b;
        mesh_drv_lora_config_t const c = region_config(band);
        mesh_drv_lora_settings_t got;
        mesh_drv_lora_settings_t begun;

        (void)memset(&got, 0, sizeof(got));
        (void)memset(&begun, 0, sizeof(begun));
        TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(band, 9U, c.power_cdbm, &begun));
        TEST_ASSERT_EQUAL_MEMORY(&begun, &got, sizeof(got));
    }
}

TEST_CASE("config: in the default build a power above the cap is held to it", "[plan]")
{
    mesh_drv_lora_config_t c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    mesh_drv_lora_settings_t got;

    c.power_cdbm = 2200;
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER, got.tx_power);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY, got.amplifier_duty);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES, got.amplifier_slices);
}

TEST_CASE("config: in the default build a frequency moves only with the channel inside the band", "[plan]")
{
    mesh_drv_lora_config_t c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    mesh_drv_lora_settings_t got;

    c.frequency_hz = MESH_REGION_SUB_GHZ_LOW_HZ + (MESH_REGION_SUB_GHZ_BANDWIDTH_HZ / 2U);
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_EQUAL_UINT32(c.frequency_hz, got.frequency_hz);
    c.frequency_hz = MESH_REGION_SUB_GHZ_LOW_HZ;          /* half the channel is out */
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c.frequency_hz = MESH_REGION_SUB_GHZ_HIGH_HZ;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c.frequency_hz = 869525000U;                          /* Meshtastic's EU channel */
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c.frequency_hz = 915000000U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
}

TEST_CASE("config: in the default build every free number is refused", "[plan]")
{
    mesh_drv_lora_settings_t got;
    mesh_drv_lora_config_t   c;

    c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    c.bandwidth_hz = 250000U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    c.spreading_factor = 6U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    c.coding_rate = 8U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    c.preamble = 16U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = region_config(MESH_PLAN_BAND_SUB_GHZ);
    c.sync_word = 0x2BU;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(NULL, &got));
}

TEST_CASE("config: in the default build the 868 band's airtime is always counted", "[plan]")
{
    TEST_ASSERT_TRUE(mesh_plan_duty_counted(MESH_PLAN_BAND_SUB_GHZ, false));
    TEST_ASSERT_TRUE(mesh_plan_duty_counted(MESH_PLAN_BAND_SUB_GHZ, true));
    TEST_ASSERT_FALSE(mesh_plan_duty_counted(MESH_PLAN_BAND_2_4_GHZ, false));
    TEST_ASSERT_FALSE(mesh_plan_duty_counted(MESH_PLAN_BAND_2_4_GHZ, true));
}

TEST_CASE("config: in the default build a packet has the region's preamble", "[plan]")
{
    TEST_ASSERT_TRUE(mesh_plan_preamble_allowed(MESH_REGION_PREAMBLE));
    TEST_ASSERT_FALSE(mesh_plan_preamble_allowed(4096U));
    TEST_ASSERT_FALSE(mesh_plan_preamble_allowed(0U));
}

/* ---- The antennas: the safe power until the app confirms them ------------------------------- */

TEST_CASE("antennas: the safe power is 0 dBm by default, and never above either band's cap", "[plan][antennas]")
{
    TEST_ASSERT_EQUAL_INT32(0, MESH_REGION_SAFE_CDBM);
    TEST_ASSERT_EQUAL_INT32(0, MESH_REGION_SAFE_DBM_X10);
    TEST_ASSERT_LESS_OR_EQUAL_INT32(MESH_REGION_2_4_GHZ_CDBM, MESH_REGION_SAFE_CDBM);
    TEST_ASSERT_LESS_OR_EQUAL_INT32(MESH_REGION_SUB_GHZ_CDBM, MESH_REGION_SAFE_CDBM);
}

TEST_CASE("antennas: unconfirmed, an ask above the safe power is held to it; confirmed, it is the app's", "[plan][antennas]")
{
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_allowed(1950, false));
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_allowed(1, false));
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_allowed(INT32_MAX, false));
    TEST_ASSERT_EQUAL_INT32(1950, mesh_plan_power_allowed(1950, true));
    /* It only lowers: an ask at or under it is what goes out, confirmed or not. */
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_allowed(0, false));
    TEST_ASSERT_EQUAL_INT32(-500, mesh_plan_power_allowed(-500, false));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mesh_plan_power_allowed(INT32_MIN, false));
}

TEST_CASE("antennas: the safe power is 0 dBm out of both amplifiers, on the region's own rows", "[plan][antennas]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, mesh_plan_power_allowed(1950, false), &tx));
    TEST_ASSERT_EQUAL_INT8(2, tx.tx_power);   /* 19 walked down 17 half-dB steps */
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES, tx.amplifier_slices);
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, mesh_plan_power_allowed(1000, false), &tx));
    TEST_ASSERT_EQUAL_INT8(4, tx.tx_power);   /* 13 walked down 9 */
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_2_4_GHZ_AMPLIFIER_DUTY, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_2_4_GHZ, &tx));
}

TEST_CASE("antennas: confirmed, the shipped build's cap is the region's, as before", "[plan][antennas]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, mesh_plan_power_allowed(1950, true), &tx));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER, tx.tx_power);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_CDBM, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
}

TEST_CASE("antennas: mesh_plan_power() plans only the amplifier, and refuses what is not a band", "[plan][antennas]")
{
    mesh_drv_lora_settings_t tx;
    mesh_drv_lora_settings_t before;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 11, 850, &tx));
    before = tx;
    TEST_ASSERT_TRUE(mesh_plan_power(MESH_PLAN_BAND_SUB_GHZ, 0, &tx));
    TEST_ASSERT_EQUAL_INT8(2, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT32(before.frequency_hz, tx.frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(before.spreading_factor, tx.spreading_factor);
    TEST_ASSERT_EQUAL_UINT8(before.bandwidth, tx.bandwidth);
    TEST_ASSERT_EQUAL(before.low_data_rate, tx.low_data_rate);
    TEST_ASSERT_TRUE(mesh_plan_power(MESH_PLAN_BAND_SUB_GHZ, 850, &tx));
    TEST_ASSERT_EQUAL_MEMORY(&before, &tx, sizeof(tx));   /* and back, to the byte */
    TEST_ASSERT_FALSE(mesh_plan_power(MESH_PLAN_BAND_COUNT, 0, &tx));
    TEST_ASSERT_FALSE(mesh_plan_power(MESH_PLAN_BAND_SUB_GHZ, 0, NULL));
}
