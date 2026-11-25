/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_safe_power.c
 * @brief The safe power until the app confirms the antennas, and an amplifier fault that
 *        suspects its band's antenna: capped until confirmed, the confirm
 *        lifting a setting in force to what was asked within the region, a fault holding that
 *        band at the safe power and saying so, a second fault changing nothing and beginning
 *        nothing, and either answer clearing it. The word is forgotten when the module leaves.
 *
 * harness_fresh() confirms the antennas, as every other case assumes a module
 * with its antennas on; these cases take the confirmation back first. The file sorts after
 * test_owner_ao.c on purpose: its last case ends the owner and starts it again, and that file's
 * first case counts the supply's switches from the run's start. This suite is the
 * default build (the region's cap 8.5 / 4.5 dBm); tests/host_lab and host_lab_pa show the
 * same safe power held in the LAB builds.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_region.h"
#include "owner_fakes.h"
#include "owner_harness.h"

static const uint8_t DATA[4] = { 'a', 'n', 't', '!' };

#define TX_SAFE_SUB_GHZ   2    /* 0 dBm: 19 walked down 17 half-dB steps */
#define TX_SAFE_2_4_GHZ   4    /* 0 dBm: 13 walked down 9 */

/** @brief Ask for @p kind with a setting. */
static kode_status_t h_set(mesh_request_kind_t kind, const mesh_drv_lora_config_t *config)
{
    mesh_aos_request_t req = { .kind = kind };

    req.config = *config;
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

/** @brief Take the module's word on the antennas back, and begin 868 at @p cdbm. */
static void unconfirmed_begin(int32_t cdbm)
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, cdbm));
}

/** @brief Put a packet on air and let the chip trip its amplifier's alarm during it. */
static void send_and_trip(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().amplifier_fault, 1000));
    h_send_settle();
}

/* ---- Until the app confirms them ------------------------------------------------- */

TEST_CASE("antennas: unconfirmed, a begin goes out at the safe power and the status says it is capped", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(1950);

    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_FALSE(st.antennas_confirmed);
    TEST_ASSERT_EQUAL_UINT8(0U, st.antenna_suspected);
    TEST_ASSERT_TRUE(st.power_capped);
    TEST_ASSERT_EQUAL_INT32(1950, st.power_cdbm);                  /* what was asked */
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SAFE_CDBM, st.power_applied_cdbm);   /* what goes out */
}

TEST_CASE("antennas: unconfirmed, an ask at or under the safe power is what goes out, and is not capped", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(-500);

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_FALSE(st.power_capped);
    TEST_ASSERT_EQUAL_INT32(-500, st.power_applied_cdbm);
}

TEST_CASE("antennas: unconfirmed, a whole setting, a retune and the 2.4 GHz band are held too", "[owner][antennas]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    mesh_drv_lora_config_t c = {
        .frequency_hz = 868300000U, .bandwidth_hz = 125000U, .spreading_factor = 9U, .coding_rate = 5U,
        .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 1950,
    };
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_CONFIGURE, &c));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);

    c.frequency_hz = 868100000U;   /* in the begin's 4 MHz step: calibrated */
    TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 1000));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_2_4_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SAFE_CDBM, h_status().power_applied_cdbm);
}

TEST_CASE("antennas: the log says the safe power once per change of state, not on every retune", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(1950);
    mesh_drv_lora_config_t c = {
        .frequency_hz = 868100000U, .bandwidth_hz = 125000U, .spreading_factor = 9U, .coding_rate = 5U,
        .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 1950,
    };
    for (unsigned i = 0U; i < 5U; i++) {
        c.frequency_hz = (i & 1U) ? 868100000U : 868300000U;
        TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT(1U, fake_log_matches("held to the safe power"));

    fake_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().power_capped, 1000));
    for (unsigned i = 0U; i < 5U; i++) {
        c.frequency_hz = (i & 1U) ? 868100000U : 868300000U;
        TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    }
    TEST_ASSERT_EQUAL_UINT(1U, fake_log_matches("the power asked goes out"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_log_matches("held to the safe power"));

    fake_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().power_capped, 1000));
    for (unsigned i = 0U; i < 5U; i++) {
        c.frequency_hz = (i & 1U) ? 868100000U : 868300000U;
        TEST_ASSERT_EQUAL(KODE_OK, h_set(MESH_REQUEST_RADIO_RETUNE, &c));
    }
    TEST_ASSERT_EQUAL_UINT(1U, fake_log_matches("held to the safe power"));
}

/* ---- The confirm ------------------------------------------------------------------ */

TEST_CASE("antennas: the confirm lifts a setting in force to what was asked, within the region, with no calibration", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(1950);
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const calibrations = g_lr.n_calibrate;
    unsigned const configures   = g_lr.n_configure;
    unsigned const retunes      = g_lr.n_retune;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));

    /* The owner puts it on at its next turn: the amplifier only, and the receiver back on. */
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.cfg.tx_power == MESH_REGION_SUB_GHZ_TX_POWER, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().power_capped, 500));
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.antennas_confirmed);
    TEST_ASSERT_EQUAL_INT32(1950, st.power_cdbm);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_CDBM, st.power_applied_cdbm);   /* the region's cap, not 19.5 dBm */
    TEST_ASSERT_TRUE(st.receiving);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_EQUAL_UINT(calibrations, g_lr.n_calibrate);
    TEST_ASSERT_EQUAL_UINT(configures, g_lr.n_configure);
    TEST_ASSERT_EQUAL_UINT(retunes + 1U, g_lr.n_retune);
}

TEST_CASE("antennas: the same word twice writes nothing to the chip", "[owner][antennas]")
{
    harness_fresh();   /* confirmed */
    unsigned const retunes = g_lr.n_retune;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(retunes, g_lr.n_retune);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_CDBM, h_status().power_applied_cdbm);
    TEST_ASSERT_FALSE(h_status().power_capped);
}

TEST_CASE("antennas: taken back while a packet is on air, the power waits for it, and the next one goes at the safe power", "[owner][antennas]")
{
    harness_fresh();   /* confirmed, 850 */
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    unsigned const retunes = g_lr.n_retune;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    vTaskDelay(pdMS_TO_TICKS(60));
    TEST_ASSERT_EQUAL_UINT(retunes, g_lr.n_retune);   /* the chip is in Tx: not touched */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, h_send_answer(1000U, NULL));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.cfg.tx_power == TX_SAFE_SUB_GHZ, 1000));
    TEST_ASSERT_TRUE(h_status().power_capped);

    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
}

TEST_CASE("antennas: unconfirmed, a test signal is keyed at the safe power too", "[owner][antennas][test]")
{
    harness_fresh();
    unconfirmed_begin(1950);

    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 100));
    TEST_ASSERT_TRUE(h_status().testing);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);

    lr_fake_chip_stops(0U);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1500));
}

TEST_CASE("antennas: taken back just before a test signal, the signal goes at the safe power", "[owner][antennas][test]")
{
    harness_fresh();   /* confirmed, 850 */
    TEST_ASSERT_FALSE(h_status().power_capped);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 100));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);

    lr_fake_chip_stops(0U);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().testing, 1500));
}

TEST_CASE("antennas: a chip that refuses the new power loses the setting, and says so", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(850);
    g_lr.retune_rc = KODE_IO_ERROR;

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().radio_ready, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_SETTING_LOST, MESH_SETTING_RADIO), 500));
    g_lr.retune_rc = KODE_OK;
}

/* ---- The amplifier's alarm -------------------------------------------------------- */

TEST_CASE("antennas: a fault suspects its band and holds it to the safe power, though the app had confirmed", "[owner][antennas][fault]")
{
    harness_fresh();   /* confirmed, 868 at 850 */
    send_and_trip();

    mesh_aos_status_t st = h_status();
    TEST_ASSERT_FALSE(st.radio_ready);   /* the fault turned it off */
    TEST_ASSERT_TRUE(st.antennas_confirmed);
    TEST_ASSERT_EQUAL_UINT8(1U << MESH_PLAN_BAND_SUB_GHZ, st.antenna_suspected);
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_AMPLIFIER_FAULT), 500));
    TEST_ASSERT_TRUE(fake_log_seen("is its antenna loose?"));

    /* The app begins again, as one that works in rounds does on its next: that band at the safe power. */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    st = h_status();
    TEST_ASSERT_TRUE(st.power_capped);
    TEST_ASSERT_FALSE(st.amplifier_fault);

    /* The other band is not suspected: it goes at what was asked. */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 450));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_2_4_GHZ_TX_POWER, g_lr.cfg.tx_power);
    TEST_ASSERT_FALSE(h_status().power_capped);
}

TEST_CASE("antennas: a fault at the safe power changes nothing and begins nothing: no loop", "[owner][antennas][fault]")
{
    harness_fresh();
    send_and_trip();   /* suspected */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    h_events_clear();
    unsigned const configures = g_lr.n_configure;
    unsigned const resets     = g_lr.n_reset;

    send_and_trip();   /* again, at the safe power */

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_EQUAL_UINT8(1U << MESH_PLAN_BAND_SUB_GHZ, st.antenna_suspected);   /* the same */
    TEST_ASSERT_FALSE(st.radio_ready);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_RADIO_ERROR) == 1U, 500));   /* said once */
    /* Nothing begins by itself: no configure, no reset, the radio stays off. */
    vTaskDelay(pdMS_TO_TICKS(300));
    TEST_ASSERT_EQUAL_UINT(configures, g_lr.n_configure);
    TEST_ASSERT_EQUAL_UINT(resets, g_lr.n_reset);
    TEST_ASSERT_FALSE(h_status().radio_ready);
    TEST_ASSERT_EQUAL_UINT(1U, h_events(MESH_AOS_EVENT_RADIO_ERROR));

    /* And a begin is still at the safe power: never higher. */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
}

TEST_CASE("antennas: confirming again clears the suspicion, and the setting goes back to what was asked", "[owner][antennas][fault]")
{
    harness_fresh();
    send_and_trip();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));

    TEST_ASSERT_TRUE(WAIT_FOR(h_status().antenna_suspected == 0U, 500));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.cfg.tx_power == MESH_REGION_SUB_GHZ_TX_POWER, 1000));
    TEST_ASSERT_FALSE(h_status().power_capped);
}

TEST_CASE("antennas: a begin alone does not clear the suspicion; the app's 'no' clears it but stays capped", "[owner][antennas][fault]")
{
    harness_fresh();
    send_and_trip();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_NOT_EQUAL(0U, h_status().antenna_suspected);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(false));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().antenna_suspected == 0U, 500));
    vTaskDelay(pdMS_TO_TICKS(60));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);
    TEST_ASSERT_FALSE(h_status().antennas_confirmed);
}

/* ---- Flags lost on the bus ---------------------------------------------------------- */

TEST_CASE("antennas: flags lost on the bus while on air are taken as a fault: the radio off, the band held", "[owner][antennas][fault]")
{
    harness_fresh();   /* confirmed, 868 at 850 */
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    g_lr.take_rc = KODE_IO_ERROR;   /* the chip clears its flags; their answer never comes back */
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().radio_ready, 1000));
    g_lr.take_rc = KODE_OK;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send_answer(2000U, NULL));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_SEND_FAILED, mesh_aos_why());
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_UINT8(1U << MESH_PLAN_BAND_SUB_GHZ, h_status().antenna_suspected);
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_FLAGS_LOST), 500));

    /* Nothing goes until the radio is begun again, and then that band at the safe power. */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);
}

TEST_CASE("antennas: flags lost off air suspect nothing: the FIFO is emptied and the receiver keeps on", "[owner][antennas][fault]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const flushes = g_lr.n_flush;
    g_lr.take_rc = KODE_IO_ERROR;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_flush == (flushes + 1U), 1000));
    g_lr.take_rc = KODE_OK;

    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_TRUE(st.receiving);
    TEST_ASSERT_EQUAL_UINT8(0U, st.antenna_suspected);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

/* ---- What survives ----------------------------------------------------------------- */

TEST_CASE("antennas: forgotten when the module leaves: one put in its place goes at the safe power until confirmed", "[owner][antennas]")
{
    harness_fresh();   /* confirmed */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 1950));
    TEST_ASSERT_EQUAL_INT8(MESH_REGION_SUB_GHZ_TX_POWER, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().antennas_confirmed);

    h_module_away();
    TEST_ASSERT_FALSE(h_status().antennas_confirmed);
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) >= 1U, 5000));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);   /* the send goes at the safe power */
    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));

    /* the app's word again, and the power it asked */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.cfg.tx_power == MESH_REGION_SUB_GHZ_TX_POWER, 1000));
    TEST_ASSERT_FALSE(h_status().power_capped);
}

TEST_CASE("antennas: unconfirmed stays so across the module leaving, and a confirm given while it is out is put on what comes back", "[owner][antennas]")
{
    harness_fresh();
    unconfirmed_begin(1950);

    h_module_away();
    TEST_ASSERT_FALSE(h_status().antennas_confirmed);
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) >= 1U, 5000));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);
    TEST_ASSERT_TRUE(h_status().power_capped);

    /* The confirm, given while the module is out, is taken and put on what comes back. */
    h_module_away();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_antennas_confirm(true));
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().radio_ready && (g_lr.cfg.tx_power == MESH_REGION_SUB_GHZ_TX_POWER), 5000));
    TEST_ASSERT_TRUE(h_status().antennas_confirmed);
    TEST_ASSERT_FALSE(h_status().power_capped);
}

TEST_CASE("antennas: mesh_aos_end() forgets the word and the suspicion; before a start it is refused", "[owner][antennas]")
{
    harness_fresh();
    send_and_trip();
    TEST_ASSERT_TRUE(h_status().antennas_confirmed);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_aos_antennas_confirm(true));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NOT_STARTED, mesh_aos_why());
    mesh_aos_status_t st = h_status();
    TEST_ASSERT_FALSE(st.antennas_confirmed);
    TEST_ASSERT_EQUAL_UINT8(0U, st.antenna_suspected);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_EQUAL_INT8(TX_SAFE_SUB_GHZ, g_lr.cfg.tx_power);   /* a new start is capped until confirmed */
    TEST_ASSERT_TRUE(h_status().power_capped);
}
