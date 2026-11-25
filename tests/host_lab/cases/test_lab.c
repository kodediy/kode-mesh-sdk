/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_lab.c
 * @brief CONFIG_MESH_LAB: what the lab build lifts, and what it leaves.
 *
 * It lifts the power up to the top of the default amplifier row (19 dBm LF, 10 dBm
 * HF; the datasheet's rows above it need CONFIG_MESH_LAB_PA_TABLE, tests/host_lab_pa),
 * the band edges, the packet's free numbers and, for a whole setting, the duty-cycle
 * count. It does not lift a band that does not exist, the chip's own ranges, or the
 * count of mesh_radio_begin(). The default build's side of every one of these is
 * in tests/host.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_plan.h"
#include "mesh_region.h"

TEST_CASE("lab: the switch reaches the region header", "[lab]")
{
    TEST_ASSERT_EQUAL_INT(1, MESH_REGION_LAB);
}

TEST_CASE("lab: the test signal is this build's, and only this build's", "[lab]")
{
    TEST_ASSERT_TRUE(mesh_plan_test_allowed());
}

TEST_CASE("lab: the datasheet's rows above the default one are off unless asked for", "[lab]")
{
    TEST_ASSERT_EQUAL_INT(0, MESH_REGION_LAB_PA_TABLE);
}

TEST_CASE("lab: an ask above the EU cap is honoured", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    /* 12.5 dBm on 868 is tx_power 27, 5.5 dBm on 2.4 GHz is 15, on the default
     * rows. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 1250, &tx));
    TEST_ASSERT_EQUAL_INT8(27, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 550, &tx));
    TEST_ASSERT_EQUAL_INT8(15, tx.tx_power);
}

TEST_CASE("lab: at or under the cap it is the same as the shipped build", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 850, &tx));
    TEST_ASSERT_EQUAL_INT8(19, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 800, &tx));
    TEST_ASSERT_EQUAL_INT8(18, tx.tx_power);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 450, &tx));
    TEST_ASSERT_EQUAL_INT8(13, tx.tx_power);
}

TEST_CASE("lab: a wild ask is held to the top of the default row", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_INT8(40, tx.tx_power);   /* 19 dBm: the sheet's rows above it are not used */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_INT8(24, tx.tx_power);   /* 10 dBm */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, -2000000, &tx));
    TEST_ASSERT_EQUAL_INT8(-19, tx.tx_power);  /* and the chip's own range below */
}

TEST_CASE("lab: any frequency on the chip's path is accepted, none off it", "[lab]")
{
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, 915000000U));
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, 863000000U));
    TEST_ASSERT_TRUE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, 2500000000U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_COUNT, 868300000U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, 149999999U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_SUB_GHZ, 960000001U));
    TEST_ASSERT_FALSE(mesh_plan_frequency_allowed(MESH_PLAN_BAND_2_4_GHZ, 2500000001U));
}

TEST_CASE("lab: the duty cycle on 868 is still counted", "[lab]")
{
    mesh_plan_duty_t d;

    mesh_plan_duty_init(&d, MESH_REGION_SUB_GHZ_DUTY_PPT);
    mesh_plan_duty_charge(&d, 36000000U, 0U);   /* the whole hour's 36 s */
    TEST_ASSERT_FALSE(mesh_plan_duty_allows(&d, 10000U, 0U));
    TEST_ASSERT_EQUAL_UINT32(1000U, mesh_plan_duty_wait_ms(&d, 10000U));
}

TEST_CASE("lab: the default row is walked as far as the datasheet's tables give it", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    /* Table 7-19: 18 dBm is 19/5/7 and 19 dBm is 20/5/7 (tx_power is twice the table's). */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 1800, &tx));
    TEST_ASSERT_EQUAL_INT8(38, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_UINT8(7U, tx.amplifier_slices);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 1900, &tx));
    TEST_ASSERT_EQUAL_INT8(40, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);
    /* Table 7-21: 10 dBm is 12/30. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 1000, &tx));
    TEST_ASSERT_EQUAL_INT8(24, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(30U, tx.amplifier_duty);
}

TEST_CASE("lab: above the default row's top every ask gets that top, not the sheet's", "[lab]")
{
    static const int32_t SUB[] = { 1950, 2000, 2050, 2100, 2150, 2200, 2000000 };
    static const int32_t HF[]  = { 1050, 1100, 1150, 1200, 2000000 };
    mesh_drv_lora_settings_t tx;

    for (size_t i = 0U; i < (sizeof(SUB) / sizeof(SUB[0])); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, SUB[i], &tx));
        TEST_ASSERT_EQUAL_INT8(40, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);
        TEST_ASSERT_EQUAL_UINT8(7U, tx.amplifier_slices);
    }
    for (size_t i = 0U; i < (sizeof(HF) / sizeof(HF[0])); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, HF[i], &tx));
        TEST_ASSERT_EQUAL_INT8(24, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(30U, tx.amplifier_duty);
    }
    /* A whole setting is judged the same way. */
    mesh_drv_lora_config_t c = {
        .frequency_hz = 868300000U, .bandwidth_hz = 125000U, .spreading_factor = 9U, .coding_rate = 5U,
        .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 2200,
    };
    mesh_drv_lora_settings_t s;
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &s));
    TEST_ASSERT_EQUAL_INT8(40, s.tx_power);
}

TEST_CASE("lab: an ask between two steps never comes out above itself", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 1275, &tx));
    TEST_ASSERT_EQUAL_INT8(27, tx.tx_power);   /* 12.5 dBm, not 13 */
}

/** @brief LongFast on 869.525: nothing in it is the region's. */
static mesh_drv_lora_config_t long_fast(void)
{
    mesh_drv_lora_config_t c = {
        .frequency_hz = 869525000U, .bandwidth_hz = 250000U, .spreading_factor = 11U, .coding_rate = 5U,
        .preamble = 16U, .sync_word = 0x2BU, .power_cdbm = 2200,
    };
    return c;
}

TEST_CASE("lab: a whole setting no region allows comes out as asked", "[lab]")
{
    mesh_drv_lora_config_t const c = long_fast();
    mesh_drv_lora_settings_t got;

    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_EQUAL_UINT32(869525000U, got.frequency_hz);
    TEST_ASSERT_FALSE(got.band_2_4_ghz);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_250, got.bandwidth);
    TEST_ASSERT_EQUAL_UINT8(11U, got.spreading_factor);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_CODING_4_5, got.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(16U, got.preamble);
    TEST_ASSERT_EQUAL_UINT8(0x2BU, got.sync_word);
    TEST_ASSERT_FALSE(got.low_data_rate);
    TEST_ASSERT_EQUAL_INT8(40, got.tx_power);       /* 22 dBm asked: the default row's top */
    TEST_ASSERT_EQUAL_UINT8(5U, got.amplifier_duty);
}

TEST_CASE("lab: the band follows the frequency, its amplifier with it", "[lab]")
{
    mesh_drv_lora_config_t c = long_fast();
    mesh_drv_lora_settings_t got;

    c.frequency_hz = 2403000000U;
    c.bandwidth_hz = 812500U;
    c.power_cdbm   = 1200;
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_TRUE(got.band_2_4_ghz);
    TEST_ASSERT_EQUAL_INT8(24, got.tx_power);
    TEST_ASSERT_EQUAL_UINT8(30U, got.amplifier_duty);   /* 12 dBm asked: the ladder's 10 dBm row */
    TEST_ASSERT_EQUAL_UINT8(0U, got.amplifier_slices);
}

TEST_CASE("lab: what the chip does not have stays refused", "[lab]")
{
    mesh_drv_lora_settings_t got;
    mesh_drv_lora_config_t   c;

    c = long_fast();
    c.frequency_hz = 1000000000U;       /* between the two paths */
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = long_fast();
    c.bandwidth_hz = 100000U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = long_fast();
    c.spreading_factor = 4U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = long_fast();
    c.coding_rate = 9U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
    c = long_fast();
    c.preamble = 0U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &got));
}

TEST_CASE("lab: a whole setting's airtime is the app's to count; mesh_radio_begin()'s is still the SDK's", "[lab]")
{
    TEST_ASSERT_TRUE(mesh_plan_duty_counted(MESH_PLAN_BAND_SUB_GHZ, false));
    TEST_ASSERT_FALSE(mesh_plan_duty_counted(MESH_PLAN_BAND_SUB_GHZ, true));
    TEST_ASSERT_FALSE(mesh_plan_duty_counted(MESH_PLAN_BAND_2_4_GHZ, false));
}

TEST_CASE("lab: a packet may have a preamble of its own, but not none", "[lab]")
{
    TEST_ASSERT_TRUE(mesh_plan_preamble_allowed(4096U));
    TEST_ASSERT_TRUE(mesh_plan_preamble_allowed(8U));
    TEST_ASSERT_FALSE(mesh_plan_preamble_allowed(0U));
}

TEST_CASE("lab: begin and configure both take SF5..12 here, and refuse the rest", "[lab]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 5, 400, &tx));
    TEST_ASSERT_EQUAL_UINT8(5U, tx.spreading_factor);
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 4, 400, &tx));
    TEST_ASSERT_FALSE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 13, 400, &tx));

    mesh_drv_lora_config_t c = {
        .frequency_hz = MESH_REGION_SUB_GHZ_CENTRE_HZ, .bandwidth_hz = MESH_REGION_SUB_GHZ_BANDWIDTH_HZ,
        .spreading_factor = 5U, .coding_rate = 5U, .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 400,
    };
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &tx));
    c.spreading_factor = 4U;
    TEST_ASSERT_FALSE(mesh_plan_radio_settings(&c, &tx));
}

TEST_CASE("lab: the safe power holds a LAB build too, until the antennas are confirmed", "[lab][antennas]")
{
    mesh_drv_lora_settings_t tx;
    mesh_drv_lora_config_t   ask;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, mesh_plan_power_allowed(1950, false), &tx));
    TEST_ASSERT_EQUAL_INT8(2, tx.tx_power);
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, mesh_plan_power_allowed(1000, false), &tx));
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_2_4_GHZ, &tx));

    /* A whole setting off the region's edges, as a lab app's can be: the power is held the same. */
    ask = (mesh_drv_lora_config_t){ .frequency_hz = 869525000U, .bandwidth_hz = 250000U, .spreading_factor = 11U,
                                    .coding_rate = 5U, .preamble = 16U, .sync_word = 0x2BU,
                                    .power_cdbm = mesh_plan_power_allowed(1950, false) };
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&ask, &tx));
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));

    /* Confirmed, the LAB ladder's top (19 dBm) as before. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, mesh_plan_power_allowed(1950, true), &tx));
    TEST_ASSERT_EQUAL_INT8(40, tx.tx_power);
    TEST_ASSERT_EQUAL_INT32(1900, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
}
