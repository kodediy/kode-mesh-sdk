/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_lab_pa.c
 * @brief CONFIG_MESH_LAB with CONFIG_MESH_LAB_PA_TABLE: the datasheet's power rows
 *        above the default one, which tests/host_lab leaves unused.
 *
 * The rows are Semtech's reference design at 915 and 2445 MHz and have not been checked
 * against the datasheet's Rev 2.2 nor tried on this module; this is only where it is
 * shown that the switch uses them, and that it takes both switches to do so.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_plan.h"
#include "mesh_region.h"

TEST_CASE("lab pa: both switches reach the region header", "[lab_pa]")
{
    TEST_ASSERT_EQUAL_INT(1, MESH_REGION_LAB);
    TEST_ASSERT_EQUAL_INT(1, MESH_REGION_LAB_PA_TABLE);
}

TEST_CASE("lab pa: a wild ask is stopped by the chip's own range", "[lab_pa]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_INT8(44, tx.tx_power);   /* +22 dBm, the sub-GHz amplifier's top */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_INT8(24, tx.tx_power);   /* +12 dBm, the 2.4 GHz amplifier's top */
}

TEST_CASE("lab pa: above the default row's top the tables' own rows, up to the chip's real top", "[lab_pa]")
{
    static const struct { int32_t cdbm; int8_t tx; uint8_t duty; uint8_t slices; } SUB[] = {
        { 1950, 42, 4U, 7U }, { 2000, 42, 5U, 6U }, { 2050, 42, 6U, 7U },
        { 2100, 44, 5U, 6U }, { 2150, 44, 6U, 7U }, { 2200, 44, 7U, 7U },
    };
    static const struct { int32_t cdbm; int8_t tx; uint8_t duty; } HF[] = {
        { 1050, 22, 21U }, { 1100, 24, 27U }, { 1150, 24, 21U }, { 1200, 24, 16U },
    };
    mesh_drv_lora_settings_t tx;

    for (size_t i = 0U; i < (sizeof(SUB) / sizeof(SUB[0])); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, SUB[i].cdbm, &tx));
        TEST_ASSERT_EQUAL_INT8(SUB[i].tx, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(SUB[i].duty, tx.amplifier_duty);
        TEST_ASSERT_EQUAL_UINT8(SUB[i].slices, tx.amplifier_slices);
    }
    for (size_t i = 0U; i < (sizeof(HF) / sizeof(HF[0])); i++) {
        TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, HF[i].cdbm, &tx));
        TEST_ASSERT_EQUAL_INT8(HF[i].tx, tx.tx_power);
        TEST_ASSERT_EQUAL_UINT8(HF[i].duty, tx.amplifier_duty);
    }
    /* Over the top, the top. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_UINT8(7U, tx.amplifier_duty);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 2000000, &tx));
    TEST_ASSERT_EQUAL_UINT8(16U, tx.amplifier_duty);
}

TEST_CASE("lab pa: the default row is the same as without the rows", "[lab_pa]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9, 1900, &tx));
    TEST_ASSERT_EQUAL_INT8(40, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9, 1000, &tx));
    TEST_ASSERT_EQUAL_INT8(24, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(30U, tx.amplifier_duty);
}

TEST_CASE("lab pa: a whole setting takes the rows too", "[lab_pa]")
{
    mesh_drv_lora_config_t c = {
        .frequency_hz = 869525000U, .bandwidth_hz = 250000U, .spreading_factor = 11U, .coding_rate = 5U,
        .preamble = 16U, .sync_word = 0x2BU, .power_cdbm = 2200,
    };
    mesh_drv_lora_settings_t got;

    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_EQUAL_INT8(44, got.tx_power);
    TEST_ASSERT_EQUAL_UINT8(7U, got.amplifier_duty);

    c.frequency_hz = 2403000000U;
    c.bandwidth_hz = 812500U;
    c.power_cdbm   = 1200;
    TEST_ASSERT_TRUE(mesh_plan_radio_settings(&c, &got));
    TEST_ASSERT_EQUAL_INT8(24, got.tx_power);
    TEST_ASSERT_EQUAL_UINT8(16U, got.amplifier_duty);
}

TEST_CASE("lab pa: the safe power holds with the sheet's rows too: 19.5 dBm asked, 0 dBm on the region's row", "[lab_pa][antennas]")
{
    mesh_drv_lora_settings_t tx;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 11, mesh_plan_power_allowed(1950, false), &tx));
    TEST_ASSERT_EQUAL_INT8(2, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(5U, tx.amplifier_duty);    /* not 42/4/7 */
    TEST_ASSERT_EQUAL_UINT8(7U, tx.amplifier_slices);
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 11, mesh_plan_power_allowed(1000, false), &tx));
    TEST_ASSERT_EQUAL_INT8(4, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(30U, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_INT32(0, mesh_plan_power_applied(MESH_PLAN_BAND_2_4_GHZ, &tx));

    /* Confirmed, the row for 19.5 dBm: 42/4/7. */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 11, mesh_plan_power_allowed(1950, true), &tx));
    TEST_ASSERT_EQUAL_INT8(42, tx.tx_power);
    TEST_ASSERT_EQUAL_UINT8(4U, tx.amplifier_duty);
    TEST_ASSERT_EQUAL_INT32(1950, mesh_plan_power_applied(MESH_PLAN_BAND_SUB_GHZ, &tx));
}
