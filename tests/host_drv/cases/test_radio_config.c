/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_config.c
 * @brief A whole setting on the wire: the numbers a network fixes reach the chip,
 *        and stay in force through a send, a receive and a timed listen.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "lr_chip.h"
#include "mesh_drv_lora.h"
#include "mesh_drv_lr2021.h"
#include "mesh_plan.h"
#include "radio_harness.h"

/** @brief A setting no region allows: 869.525 MHz, BW250, SF11, 4/8, 16 symbols, 0x2B. */
static mesh_drv_lora_settings_t wide(void)
{
    mesh_drv_lora_settings_t cfg;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 11U, 850, &cfg));
    cfg.frequency_hz  = 869525000U;
    cfg.bandwidth     = MESH_DRV_LORA_BANDWIDTH_250;
    cfg.coding_rate   = MESH_DRV_LORA_CODING_4_8;
    cfg.preamble      = 16U;
    cfg.sync_word     = 0x2BU;
    cfg.low_data_rate = mesh_drv_lora_low_data_rate(11U, cfg.bandwidth);
    return cfg;
}

/** @brief The chip reset and set to @p cfg, its log cleared. */
static void up(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t major = 0U;
    uint8_t minor = 0U;

    radio_open();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_configure(cfg));
    chip_log_clear();
}

TEST_CASE("config: frequency, bandwidth, SF, coding rate, preamble and sync word reach the chip", "[radio][config]")
{
    mesh_drv_lora_settings_t const cfg = wide();

    up(&cfg);
    TEST_ASSERT_EQUAL_UINT32(869525000U, g_chip.freq_hz);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_250, g_chip.bw);
    TEST_ASSERT_EQUAL_UINT8(11U, g_chip.sf);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_CODING_4_8, g_chip.cr);
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);       /* the receiver's: any preamble heard */
    TEST_ASSERT_EQUAL_UINT8(0x2BU, g_chip.sync_word);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.ldro);           /* SF11 BW250: an 8.2 ms symbol */

    static const uint8_t DATA[4] = { 0U };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT16(16U, g_chip.pbl);          /* the setting's, on air */
}

TEST_CASE("config: Meshtastic's 2.4 GHz LongFast reaches the chip with the low data-rate option", "[radio][config]")
{
    mesh_drv_lora_settings_t cfg;

    /* SF11 at BW812, as every 2.4 GHz node running RadioLib sends it (9.9.1). */
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 11U, 450, &cfg));
    cfg.frequency_hz = 2420718750U;
    cfg.preamble     = 12U;
    cfg.sync_word    = 0x2BU;
    up(&cfg);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_812, g_chip.bw);
    TEST_ASSERT_EQUAL_UINT8(11U, g_chip.sf);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_CODING_4_5, g_chip.cr);
    TEST_ASSERT_EQUAL_UINT8(1U, g_chip.ldro);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("config: every bandwidth code the chip has is taken and its airtime known", "[radio][config]")
{
    static const uint8_t CODES[] = { 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU };
    static const uint8_t DATA[]  = { 1U, 2U, 3U };

    for (size_t i = 0U; i < sizeof(CODES); i++) {
        mesh_drv_lora_settings_t cfg = wide();
        cfg.bandwidth     = CODES[i];
        cfg.low_data_rate = mesh_drv_lora_low_data_rate(cfg.spreading_factor, cfg.bandwidth);
        up(&cfg);
        TEST_ASSERT_EQUAL_UINT8(CODES[i], g_chip.bw);
        TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
        TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    }
}

TEST_CASE("config: a setting the chip does not have is refused and nothing goes to it", "[radio][config]")
{
    mesh_drv_lora_settings_t const good = wide();
    mesh_drv_lora_settings_t       bad;

    up(&good);
    bad = good;
    bad.bandwidth = 0x08U;      /* RFU in table 9-3 */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = good;
    bad.coding_rate = 0U;       /* RFU in table 9-4 */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = good;
    bad.coding_rate = 5U;       /* the long interleaver: not offered */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = good;
    bad.preamble = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

TEST_CASE("config: a send keeps the setting's preamble, and its timer is the setting's airtime", "[radio][config]")
{
    mesh_drv_lora_settings_t const cfg = wide();
    static const uint8_t DATA[20] = { 0U };

    up(&cfg);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT16(16U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT8(sizeof(DATA), g_chip.payload_len);

    uint64_t const air = chip_lora_air_cr_us(11U, MESH_DRV_LORA_BANDWIDTH_250, MESH_DRV_LORA_CODING_4_8, false, 16U,
                                             sizeof(DATA));
    TEST_ASSERT_EQUAL_UINT32(mesh_drv_lora_timer_ticks((uint32_t)(air + (air / 4U))), g_chip.tx_timeout_ticks);
}

TEST_CASE("config: a receive and a listen put the receiver's preamble back, not the send's", "[radio][config]")
{
    mesh_drv_lora_settings_t const cfg = wide();
    static const uint8_t DATA[4] = { 0U };

    up(&cfg);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LR2021_PAYLOAD_MAX, g_chip.payload_len);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_listen_for(10U));
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);
}

TEST_CASE("config: the region's setting keeps its private word and its eight symbols", "[radio][config]")
{
    static const uint8_t DATA[4] = { 0U };

    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT16(8U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT8(0x12U, g_chip.sync_word);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_CODING_4_5, g_chip.cr);
}
