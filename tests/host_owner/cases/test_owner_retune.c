/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_retune.c
 * @brief The owner moving between settings: refused while something is on, the receiver
 *        carried over, the frequencies it knows are calibrated. This suite is the default
 *        build, so the settings are the region's; tests/host_lab holds what LAB lifts.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_region.h"
#include "owner_fakes.h"
#include "owner_harness.h"

/** @brief Ask for @p kind with a setting. */
static kode_status_t h_set(mesh_request_kind_t kind, const mesh_drv_lora_config_t *config)
{
    mesh_aos_request_t req = { .kind = kind };

    req.config = *config;
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

/** @brief The same, the Active Object's way: a call from another task waits for the one that holds
 *         the owner (a send in the air), so this is asked, and the answer read. */
static kode_status_t h_set_asked(mesh_request_kind_t kind, const mesh_drv_lora_config_t *config)
{
    mesh_aos_request_t req = { .kind = kind };
    h_answer_t         got = { 0 };

    req.config = *config;
    if (h_ask(&req, &got) != KODE_OK) {
        return KODE_NO_RESOURCES;
    }
    return h_answered(&got, 2000U) ? got.a.rc : KODE_TIMEOUT;
}

/** @brief Calibrate @p count frequencies, as mesh_radio_calibrate() does. */
static kode_status_t h_calibrate(uint32_t f0, uint32_t f1, uint32_t count)
{
    mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_CALIBRATE, .u = count };

    req.hz[0] = f0;
    req.hz[1] = f1;
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

/** @brief The region's setting on 868 at @p hz. */
static mesh_drv_lora_config_t sub_at(uint32_t hz)
{
    mesh_drv_lora_config_t c = {
        .frequency_hz = hz, .bandwidth_hz = 125000U, .spreading_factor = 9U, .coding_rate = 5U,
        .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 400,
    };
    return c;
}

/** @brief The region's setting on 2.4 GHz. */
static mesh_drv_lora_config_t hf_at(uint32_t hz)
{
    mesh_drv_lora_config_t c = {
        .frequency_hz = hz, .bandwidth_hz = 812500U, .spreading_factor = 7U, .coding_rate = 5U,
        .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 300,
    };
    return c;
}

TEST_CASE("retune: to a frequency the begin calibrated, the receiver goes on, nothing is calibrated", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const calibrations = g_lr.n_calibrate;
    unsigned const receives     = g_lr.n_receive;
    /* The begin calibrated 868.3 MHz, so anything in its 4 MHz step is known. */
    mesh_drv_lora_config_t const c = sub_at(868100000U);

    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_retune);
    TEST_ASSERT_EQUAL_UINT(calibrations, g_lr.n_calibrate);
    TEST_ASSERT_GREATER_THAN_UINT(receives, g_lr.n_receive);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_EQUAL_UINT32(868100000U, st.frequency_hz);
    TEST_ASSERT_EQUAL_INT32(400, st.power_cdbm);
    TEST_ASSERT_TRUE(st.receiving);
}

TEST_CASE("retune: a frequency not calibrated is refused until it is", "[owner][retune]")
{
    harness_fresh();
    mesh_drv_lora_config_t const h = hf_at(2442000000U);

    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &h));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_retune);

    TEST_ASSERT_EQUAL(KODE_OK, h_calibrate(868300000U, 2442000000U, 2U));
    TEST_ASSERT_EQUAL_UINT8(2U, g_lr.calibrated_count);
    TEST_ASSERT_EQUAL_UINT32(2442000000U, g_lr.calibrated_at[1]);
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &h));

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_EQUAL_UINT8(MESH_BAND_2_4_GHZ, st.band);
    TEST_ASSERT_EQUAL_UINT32(812500U, st.bandwidth_hz);

    mesh_drv_lora_config_t const back = sub_at(868300000U);
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &back));
    TEST_ASSERT_EQUAL_UINT8(MESH_BAND_SUB_GHZ, h_status().band);
}

TEST_CASE("retune: a module that left and came back has lost its calibrations", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_calibrate(868300000U, 2442000000U, 2U));
    h_module_away();
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 10000));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));

    /* The chip was reset on the way: it never calibrated 2.4 GHz since. */
    mesh_drv_lora_config_t const h = hf_at(2442000000U);
    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &h));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_retune);
}

TEST_CASE("configure: a repeat of the first slot's step goes with it: it never had a slot", "[owner][retune]")
{
    harness_fresh();
    mesh_drv_lora_config_t const near = sub_at(868300000U);
    mesh_drv_lora_config_t const h    = hf_at(2442000000U);

    /* Two frequencies in one 4 MHz step: the chip calibrates the first, and skips the second. */
    TEST_ASSERT_EQUAL(KODE_OK, h_calibrate(868300000U, 868400000U, 2U));
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &near));
    /* A whole setting elsewhere takes the first slot: nothing is left for that step. */
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_CONFIGURE, &h));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &near));
}

TEST_CASE("retune: a packet arriving, on air or a search on, is left alone and refused", "[owner][retune]")
{
    static const uint8_t DATA[] = { 1U, 2U };
    mesh_drv_lora_config_t const c = sub_at(868100000U);

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().arriving, 1000));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_retune);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_set_asked(MESH_REQUEST_RADIO_RETUNE, &c));   /* a call would wait its turn */
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_retune);
}

TEST_CASE("retune: without a setting, or one this build refuses, nothing moves", "[owner][retune]")
{
    harness_fresh();
    mesh_drv_lora_config_t bad = sub_at(868100000U);

    bad.preamble = 16U;
    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &bad));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_retune);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("retune: a chip that does not take it leaves no setting, and off air", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.retune_rc = KODE_IO_ERROR;
    mesh_drv_lora_config_t const c = sub_at(868100000U);

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    g_lr.retune_rc = KODE_OK;
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_FALSE(st.radio_ready);
    TEST_ASSERT_FALSE(st.receiving);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("calibrate: the receiver is back after it, and a frequency off the region is refused", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_calibrate(868100000U, 0U, 1U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_EQUAL(KODE_DENIED, h_calibrate(915000000U, 0U, 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_calibrate(868100000U, 0U, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_calibrate(868100000U, 0U, 4U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_calibrate_at);
}

TEST_CASE("calibrate: a list without the setting in force is refused, and nothing changes", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const calibrations = g_lr.n_calibrate_at;

    /* The setting is 868.3 MHz: a list of 2.4 GHz alone would leave the receiver on a
     * frequency the SDK no longer counts as calibrated. */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_calibrate(2442000000U, 0U, 1U));
    TEST_ASSERT_EQUAL_UINT(calibrations, g_lr.n_calibrate_at);
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    /* The setting back in the list, after the other one: it is whole again, and
     * retuning to the setting in force is what it was. */
    TEST_ASSERT_EQUAL(KODE_OK, h_calibrate(2442000000U, 868300000U, 2U));
    mesh_drv_lora_config_t const c = sub_at(868300000U);
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    TEST_ASSERT_TRUE(h_status().receiving);
}

TEST_CASE("calibrate: a receiver that does not come back is an error, not an OK", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.receive_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_calibrate(868300000U, 0U, 1U));
    g_lr.receive_rc = KODE_OK;

    /* The calibration itself was done, and the status says what is not on. */
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_calibrate_at);
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_FALSE(h_status().receiving);
}

TEST_CASE("calibrate: a calibration the chip refused is the chip's answer, receiver or not", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.calibrate_at_rc = KODE_IO_ERROR;
    g_lr.receive_rc      = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_calibrate(868300000U, 0U, 1U));
    g_lr.calibrate_at_rc = KODE_OK;
    g_lr.receive_rc      = KODE_OK;
    TEST_ASSERT_FALSE(h_status().receiving);
}

TEST_CASE("retune: a receiver that does not come back leaves the new setting in force, and says IO_ERROR", "[owner][retune]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.receive_rc = KODE_IO_ERROR;
    mesh_drv_lora_config_t const c = sub_at(868100000U);

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    g_lr.receive_rc = KODE_OK;

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);                       /* the chip took the retune */
    TEST_ASSERT_EQUAL_UINT32(868100000U, st.frequency_hz);  /* ... so this is the setting */
    TEST_ASSERT_FALSE(st.receiving);                        /* ... and the receiver is what is off */
}

TEST_CASE("calibrate: a failure forgets what was known, so nothing retunes on a guess", "[owner][retune]")
{
    harness_fresh();
    g_lr.calibrate_at_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_calibrate(868100000U, 0U, 1U));
    g_lr.calibrate_at_rc = KODE_OK;
    mesh_drv_lora_config_t const c = sub_at(868300000U);

    TEST_ASSERT_EQUAL(KODE_DENIED, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
}

TEST_CASE("preamble: this build refuses another, and takes its own", "[owner][retune]")
{
    static const uint8_t DATA[] = { 1U, 2U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send_preamble(DATA, sizeof(DATA), 4096U, 2000U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_send);

    TEST_ASSERT_EQUAL(KODE_OK, h_send_preamble(DATA, sizeof(DATA), MESH_REGION_PREAMBLE, 2000U));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_send_preamble);
    TEST_ASSERT_EQUAL_UINT16(MESH_REGION_PREAMBLE, g_lr.sent_preamble);
}
