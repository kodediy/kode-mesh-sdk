/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_retune.c
 * @brief Moving between settings on the wire: what is written and what is not, no
 *        calibration, and across the two bands; and a packet with its own preamble.
 */
#include <stdint.h>

#include "unity.h"

#include "lr_chip.h"
#include "mesh_drv_lora.h"
#include "mesh_drv_lr2021.h"
#include "mesh_plan.h"
#include "radio_harness.h"

/** @brief Meshtastic's LongFast on 869.525 MHz, at 868's region amplifier row. */
static mesh_drv_lora_settings_t sub_a(void)
{
    mesh_drv_lora_settings_t cfg;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 11U, 850, &cfg));
    cfg.frequency_hz  = 869525000U;
    cfg.bandwidth     = MESH_DRV_LORA_BANDWIDTH_250;
    cfg.preamble      = 16U;
    cfg.sync_word     = 0x2BU;
    cfg.low_data_rate = mesh_drv_lora_low_data_rate(11U, cfg.bandwidth);
    return cfg;
}

/** @brief A narrow one nearby: another network on the same band. */
static mesh_drv_lora_settings_t sub_b(void)
{
    mesh_drv_lora_settings_t cfg = sub_a();

    cfg.frequency_hz     = 869618000U;
    cfg.bandwidth        = MESH_DRV_LORA_BANDWIDTH_62;
    cfg.spreading_factor = 8U;
    cfg.coding_rate      = MESH_DRV_LORA_CODING_4_8;
    cfg.preamble         = 32U;
    cfg.sync_word        = 0x12U;
    cfg.low_data_rate    = mesh_drv_lora_low_data_rate(8U, cfg.bandwidth);
    return cfg;
}

/** @brief The HF path, at its region amplifier row. */
static mesh_drv_lora_settings_t hf(void)
{
    mesh_drv_lora_settings_t cfg;

    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 7U, 450, &cfg));
    cfg.frequency_hz = 2403000000U;
    cfg.preamble     = 16U;
    cfg.sync_word    = 0x2BU;
    return cfg;
}

/** @brief The chip reset, set to @p cfg, its log cleared. */
static void up(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t major = 0U;
    uint8_t minor = 0U;

    radio_open();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_configure(cfg));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate());
    chip_log_clear();
}

TEST_CASE("retune: in a band, from a receive, every number reaches the chip and no calibration is run", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const b = sub_b();

    up(&a);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&b));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_XOSC, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT32(869618000U, g_chip.freq_hz);
    TEST_ASSERT_EQUAL_UINT8(8U, g_chip.sf);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_62, g_chip.bw);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_CODING_4_8, g_chip.cr);
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);       /* shaped for the receive that follows */
    TEST_ASSERT_EQUAL_UINT8(0x12U, g_chip.sync_word);
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_CALIB_FE));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_PACKET_TYPE));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_RX_PATH));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_PA_CONFIG));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LR2021_PAYLOAD_MAX, g_chip.payload_len);
}

TEST_CASE("retune: a send after it goes with the new setting's preamble, a receive with the receiver's", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const b = sub_b();
    static const uint8_t DATA[4] = { 0U };

    up(&a);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&b));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT16(32U, g_chip.pbl);
    chip_finish_tx();
    (void)mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("retune: to the same setting only standby goes to the chip", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();

    up(&a);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&a));
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_SET_STANDBY));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_RF_FREQUENCY));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_LORA_MOD));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_LORA_PACKET));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_LORA_SYNCWORD));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_TX_PARAMS));
}

TEST_CASE("retune: across the bands the path and the amplifier follow, power first, and back", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const h = hf();

    up(&a);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&h));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_EQUAL_UINT32(2403000000U, g_chip.freq_hz);
    const chip_cmd_t *path = chip_last(OPC_SET_RX_PATH);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_UINT8(1U, path->p[0]);              /* HF */
    TEST_ASSERT_EQUAL_UINT8(0U, path->p[1]);              /* the one boost, 7.3.2 */
    const chip_cmd_t *pa = chip_last(OPC_SET_PA_CONFIG);
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_EQUAL_HEX8(0x80U, pa->p[0]);              /* the HF amplifier */
    TEST_ASSERT_EQUAL_UINT8(h.amplifier_duty, pa->p[2]);
    TEST_ASSERT_EQUAL_UINT(2U, chip_count(OPC_SET_TX_PARAMS));
    TEST_ASSERT_EQUAL_INT8(h.tx_power, g_chip.tx_power);
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_CALIB_FE));

    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&a));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    path = chip_last(OPC_SET_RX_PATH);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_UINT8(0U, path->p[0]);              /* LF */
    pa = chip_last(OPC_SET_PA_CONFIG);
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_EQUAL_HEX8(0x00U, pa->p[0]);
    TEST_ASSERT_EQUAL_INT8(a.tx_power, g_chip.tx_power);
}

TEST_CASE("retune: across the bands and back the boost never changes, so the chip calibrates nothing", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const h = hf();

    up(&a);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.rx_path);
    for (unsigned i = 0U; i < 3U; i++) {
        TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&h));
        TEST_ASSERT_EQUAL_UINT8(1U, g_chip.rx_path);
        TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&a));
        TEST_ASSERT_EQUAL_UINT8(0U, g_chip.rx_path);
    }
    TEST_ASSERT_EQUAL_UINT(6U, chip_count(OPC_SET_RX_PATH));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_src_calibrations);   /* from the reset on */
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("configure: on either band the boost is the one the chip had from its reset", "[radio][retune]")
{
    mesh_drv_lora_settings_t const h = hf();

    up(&h);
    TEST_ASSERT_EQUAL_UINT8(1U, g_chip.rx_path);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.rx_boost);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_src_calibrations);
}

TEST_CASE("retune: a change of power alone writes the amplifier and nothing else", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t       more = a;

    up(&a);
    more.tx_power = 27;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&more));
    TEST_ASSERT_EQUAL_INT8(27, g_chip.tx_power);
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_LORA_MOD));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_RF_FREQUENCY));
}

TEST_CASE("retune: with no setting yet, or one the chip does not have, nothing goes to it", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t       bad = a;

    radio_open();
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_retune(&a));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());

    up(&a);
    bad.bandwidth = 0x08U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_retune(&bad));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_retune(NULL));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

TEST_CASE("retune: a receive path refused a few times is asked for again, and leaves no error behind", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const h = hf();

    up(&a);
    chip_log_clear();
    g_chip.refuse_rx_path = 3U;
    g_chip.calib_fe_error = 0x2000U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_retune(&h));
    TEST_ASSERT_EQUAL_UINT(4U, chip_count(OPC_SET_RX_PATH));
    TEST_ASSERT_EQUAL_UINT32(2403000000U, g_chip.freq_hz);
    TEST_ASSERT_EQUAL_HEX16(0U, g_chip.errors);
}

TEST_CASE("retune: a receive path refused every time says so", "[radio][retune]")
{
    mesh_drv_lora_settings_t const a = sub_a();
    mesh_drv_lora_settings_t const h = hf();

    up(&a);
    g_chip.refuse_rx_path = 1000U;
    g_chip.calib_fe_error = 0x4000U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_retune(&h));
}

TEST_CASE("calibrate: an error flag from before is not this calibration's", "[radio][retune]")
{
    static const uint32_t F[2] = { 869525000U, 2403000000U };
    mesh_drv_lora_settings_t const a = sub_a();

    up(&a);
    g_chip.errors = 0x2200U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate_at(F, 2U));
    g_chip.errors = 0x2000U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate());
}

TEST_CASE("calibrate: only self-reception cancellation failing is tried again, and until it fits", "[radio][retune]")
{
    static const uint32_t F[1] = { 869525000U };
    mesh_drv_lora_settings_t const a = sub_a();

    up(&a);
    chip_log_clear();
    g_chip.fail_calib_fe = 2U;
    g_chip.calib_fe_error = 0x4000U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate_at(F, 1U));
    TEST_ASSERT_EQUAL_UINT(3U, chip_count(OPC_CALIB_FE));
    TEST_ASSERT_EQUAL_HEX16(0U, g_chip.errors);

    chip_log_clear();
    g_chip.fail_calib_fe = 1000U;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_calibrate_at(F, 1U));

    chip_log_clear();
    g_chip.fail_calib_fe = 1000U;
    g_chip.calib_fe_error = 0x0080U;   /* image rejection: asking again is no cure */
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_calibrate_at(F, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_CALIB_FE));
    g_chip.fail_calib_fe = 0U;
}

TEST_CASE("calibrate: three frequencies in one order, each on its path, in 4 MHz steps", "[radio][retune]")
{
    static const uint32_t F[3] = { 869525000U, 915000000U, 2403000000U };
    mesh_drv_lora_settings_t const a = sub_a();

    up(&a);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate_at(F, 3U));
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_CALIB_FE));
    const chip_cmd_t *c = chip_last(OPC_CALIB_FE);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(6U, c->n);
    TEST_ASSERT_EQUAL_HEX16(217U, (uint16_t)(((uint16_t)c->p[0] << 8) | c->p[1]));
    TEST_ASSERT_EQUAL_HEX16(228U, (uint16_t)(((uint16_t)c->p[2] << 8) | c->p[3]));
    TEST_ASSERT_EQUAL_HEX16(0x8000U | 600U, (uint16_t)(((uint16_t)c->p[4] << 8) | c->p[5]));
}

TEST_CASE("calibrate: a slot not given stays 0, and more than three is refused", "[radio][retune]")
{
    static const uint32_t F[4] = { 869525000U, 915000000U, 2403000000U, 2480000000U };
    mesh_drv_lora_settings_t const a = sub_a();

    up(&a);
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_calibrate_at(F, 4U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_calibrate_at(F, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_calibrate_at(NULL, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_calibrate_at(F, 1U));
    const chip_cmd_t *c = chip_last(OPC_CALIB_FE);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(0U, c->p[2]);
    TEST_ASSERT_EQUAL_UINT8(0U, c->p[5]);
}

TEST_CASE("preamble: a packet's own, its timer long enough, the receiver's and the setting's after it", "[radio][retune]")
{
    mesh_drv_lora_settings_t const h = hf();
    static const uint8_t DATA[8] = { 0U };

    up(&h);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send_preamble(DATA, sizeof(DATA), 4096U));
    TEST_ASSERT_EQUAL_UINT16(4096U, g_chip.pbl);
    uint64_t const air = chip_lora_air_cr_us(h.spreading_factor, h.bandwidth, h.coding_rate, false, 4096U, sizeof(DATA));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(chip_us_to_ticks(air), g_chip.tx_timeout_ticks);

    chip_advance_us(air + 1000U);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);       /* any preamble heard */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_UINT16(16U, g_chip.pbl);
}
