/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_presence.c
 * @brief A module that stops answering is not left keyed: it is stood down, its
 *        reset line pulled low whatever it said to standby, its positions handed
 *        back and the app told - and it is a new module when it is there again.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

TEST_CASE("leave: a module that stops answering is stood down, held in reset and its lines handed back", "[owner][presence]")
{
    harness_fresh();
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_TAG));    /* the radio's and GNSS's lines */
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_PAIR));   /* and the I2C pair */

    /* Gone: it does not answer a version read (twice, a second apart), and the
     * fingerprint is not there to be found again. */
    pins_fake_set_mesh(false);
    g_lr.alive = false;
    g_lr.standby_fails = 1000U;   /* a module that is not there confirms no standby */
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_events(MESH_AOS_EVENT_GONE) == 1U, 5000U), "the owner never gave it up");

    TEST_ASSERT_FALSE(h_status().present);
    TEST_ASSERT_FALSE(h_status().radio_ready);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1U, g_lr.n_standby);   /* asked to stand by, if it will */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1U, g_lr.n_hold);      /* and, as it will not, its line pulled low */
    TEST_ASSERT_TRUE(g_lr.held);
    g_lr.standby_fails = 0U;
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_close);
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_TAG));
    TEST_ASSERT_FALSE(fake_loaned(MESH_BSP_LOAN_PAIR));

    /* It is back: found by its fingerprint, reset, a new module. */
    pins_fake_set_mesh(true);
    g_lr.alive = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_TRUE(h_status().present);
    /* The chip is new, and the owner puts the app's begin on it again. */
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_TAG));
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_PAIR));
}

/* ---- A chip held in reset on purpose is not an unplugged one -------------------------- */

/** @brief Longer than the alive poll takes to give up on a module that does not answer. */
#define SILENCE_MS ((MESH_BSP_ALIVE_MS * (MESH_BSP_GONE_MISSES + 2U)))

/** @brief The reset is held, and across several alive polls the module stays: it is only silent. */
static void held_but_present(unsigned closes)
{
    TEST_ASSERT_TRUE(g_lr.held);
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());   /* the model: a chip in reset answers nothing */
    vTaskDelay(pdMS_TO_TICKS(SILENCE_MS));

    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_GONE));
    TEST_ASSERT_TRUE(g_lr.held);                          /* and its line was not let go */
    TEST_ASSERT_EQUAL_UINT(closes, g_lr.n_close);
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_TAG));
    TEST_ASSERT_TRUE(fake_loaned(MESH_BSP_LOAN_PAIR));
}

/** @brief The radio is begun: the line lets go, the alive poll works, and an unplug is noticed again. */
static void begun_then_unplug_is_noticed(void)
{
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 850));
    TEST_ASSERT_FALSE(g_lr.held);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_alive());
    vTaskDelay(pdMS_TO_TICKS(SILENCE_MS));
    TEST_ASSERT_TRUE(h_status().present);                 /* answering: nothing lost */
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_GONE));

    pins_fake_set_mesh(false);
    g_lr.alive = false;
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_events(MESH_AOS_EVENT_GONE) == 1U, 5000U), "an unplug went unnoticed after the radio was begun");
    TEST_ASSERT_FALSE(h_status().present);
}

TEST_CASE("hold: a module held in reset by a rest that found no standby is not taken for unplugged", "[owner][presence][hold]")
{
    harness_fresh();
    g_lr.standby_fails = 1000U;
    g_lr.reset_fails   = true;
    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.served > served, 1000U));
    g_lr.standby_fails = 0U;
    g_lr.reset_fails   = false;

    held_but_present(g_lr.n_close);
    begun_then_unplug_is_noticed();
}

TEST_CASE("sleep: a module whose radio sleeps through a rest stays present, and a pull is still noticed", "[owner][presence][sleep]")
{
    harness_fresh();
    unsigned const served = g_ao.served;
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(WAIT_FOR(g_ao.served > served, 1000U));
    TEST_ASSERT_TRUE(g_lr.asleep);

    vTaskDelay(pdMS_TO_TICKS(SILENCE_MS));   /* the alive poll asks it, the driver wakes it for that */
    TEST_ASSERT_TRUE(h_status().present);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_GONE));
    TEST_ASSERT_TRUE(g_lr.asleep);
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_asleep_calls);

    pins_fake_set_mesh(false);
    g_lr.alive = false;
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_events(MESH_AOS_EVENT_GONE) == 1U, 5000U), "a sleeping module was pulled and nobody noticed");
    TEST_ASSERT_FALSE(h_status().present);
}

TEST_CASE("hold: a module held in reset by the app's request is not taken for unplugged", "[owner][presence][hold]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_HOLD_RESET, 2000U));

    held_but_present(g_lr.n_close);
    begun_then_unplug_is_noticed();
}

TEST_CASE("hold: a chip that would neither stand by nor reset is held, and stays present", "[owner][presence][hold]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_CARRIER, 30000));
    g_lr.standby_fails = 1000U;
    g_lr.reset_fails   = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    TEST_ASSERT_TRUE(WAIT_FOR(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY), 1000U));

    held_but_present(g_lr.n_close);

    g_lr.standby_fails = 0U;
    g_lr.reset_fails   = false;
    begun_then_unplug_is_noticed();
}

/* ---- What the receiver said leaves with the module -------------------------------------- */

TEST_CASE("leave: the receiver's fix does not outlive the module", "[owner][presence][gnss]")
{
    harness_fresh();
    g_gnss.report.fix.fix          = true;
    g_gnss.report.fix.lat          = 39.47;
    g_gnss.report.fix.lon          = -0.376;
    g_gnss.report.fix.sats_used    = 7U;
    g_gnss.report.messages         = 42U;
    g_gnss.report.baud             = 9600U;
    g_gnss.report.talking          = true;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().fix.fix, 2000U));   /* it is being read */
    TEST_ASSERT_EQUAL_UINT8(7U, h_status().fix.sats_used);
    TEST_ASSERT_TRUE(h_status().gnss_talking);

    pins_fake_set_mesh(false);
    g_lr.alive = false;
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_GONE) == 1U, 5000U));

    mesh_aos_status_t const status = h_status();
    TEST_ASSERT_FALSE(status.gnss_on);
    TEST_ASSERT_FALSE(status.fix.fix);
    TEST_ASSERT_EQUAL_UINT8(0U, status.fix.sats_used);
    TEST_ASSERT_EQUAL_UINT32(0U, status.gnss_sentences);
    TEST_ASSERT_EQUAL_UINT32(0U, status.gnss_baud);
    TEST_ASSERT_FALSE(status.gnss_talking);

    /* A module that comes back starts without it, too. */
    g_gnss.report = (mesh_drv_gnss_status_t){ 0 };
    pins_fake_set_mesh(true);
    g_lr.alive = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
    TEST_ASSERT_FALSE(h_status().fix.fix);
}

TEST_CASE("fix age: unknown until a report, then counted from it, and from the next", "[owner][presence]")
{
    harness_fresh();
    const mesh_aos_request_t begin = { .kind = MESH_REQUEST_GNSS_BEGIN };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&begin, 2000U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, h_status().fix_age_ms);

    g_gnss.report.fix.fix     = true;
    g_gnss.report.fix.reports = 1U;
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().fix_age_ms != UINT32_MAX, 1000));
    TEST_ASSERT_LESS_THAN_UINT32(1000U, h_status().fix_age_ms);

    vTaskDelay(pdMS_TO_TICKS(300));
    uint32_t const older = h_status().fix_age_ms;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(200U, older);

    g_gnss.report.fix.reports = 2U;
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().fix_age_ms < older, 1000));
}

TEST_CASE("fix age: a receiver closed has none", "[owner][presence]")
{
    harness_fresh();
    const mesh_aos_request_t begin = { .kind = MESH_REQUEST_GNSS_BEGIN };
    const mesh_aos_request_t end   = { .kind = MESH_REQUEST_GNSS_END };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&begin, 2000U, NULL, NULL));
    g_gnss.report.fix.reports = 1U;
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().fix_age_ms != UINT32_MAX, 1000));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&end, 2000U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, h_status().fix_age_ms);
    g_gnss.report.fix.reports = 0U;
}

/* ---- The setting is lost, and said so; and put back when the module is --------------- */

#define RADIO     MESH_SETTING_RADIO
#define RECEIVER  MESH_SETTING_RECEIVER
#define GNSS      MESH_SETTING_GNSS

/** @brief The module is back, found and reset; wait for what the owner does with it. */
static void module_returns(void)
{
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 10000U));
}

TEST_CASE("setting: a module that leaves loses the setting, and the app is told", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    h_module_away();

    TEST_ASSERT_EQUAL_UINT(1U, g_away_lost_n);
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER | GNSS, g_away_lost_arg);
    TEST_ASSERT_FALSE(h_status().radio_ready);
    module_returns();
}

TEST_CASE("setting: nothing set up, nothing lost", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    h_module_away();
    TEST_ASSERT_EQUAL_UINT(0U, g_away_lost_n);
    module_returns();
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_FALSE(h_status().radio_ready);
}

TEST_CASE("setting: a module that returns gets the app's begin, its receiver and its GNSS back", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 8U, 450));
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    unsigned const opens = g_gnss.opens;
    h_module_away();
    module_returns();

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER | GNSS, h_event_arg(MESH_AOS_EVENT_SETTING_RESTORED));
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL_UINT8(MESH_BAND_2_4_GHZ, st.band);
    TEST_ASSERT_EQUAL_UINT8(8U, st.spreading_factor);
    TEST_ASSERT_EQUAL_INT32(450, st.power_cdbm);
    TEST_ASSERT_TRUE(st.receiving);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
    TEST_ASSERT_TRUE(st.gnss_on);
    TEST_ASSERT_EQUAL_UINT(opens + 1U, g_gnss.opens);
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_LOST));
}

TEST_CASE("setting: a whole setting and a retune are put back as they were", "[owner][presence][setting]")
{
    harness_fresh();
    mesh_aos_request_t cfg = { .kind = MESH_REQUEST_RADIO_CONFIGURE };
    cfg.config = (mesh_drv_lora_config_t){ .frequency_hz = 868100000U, .bandwidth_hz = 125000U, .spreading_factor = 10U,
                                           .coding_rate = 5U, .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 400 };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&cfg, 2000U, NULL, NULL));
    cfg.kind = MESH_REQUEST_RADIO_RETUNE;
    cfg.config.frequency_hz = 868500000U;
    cfg.config.spreading_factor = 11U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_call(&cfg, 2000U, NULL, NULL));
    h_module_away();
    module_returns();

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    mesh_aos_status_t const st = h_status();
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL_UINT32(868500000U, st.frequency_hz);
    TEST_ASSERT_EQUAL_UINT8(11U, st.spreading_factor);
    TEST_ASSERT_FALSE(st.receiving);
}

TEST_CASE("setting: what the app ended is not put back", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(false));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_END, 2000U));
    h_module_away();
    TEST_ASSERT_EQUAL_UINT32(RADIO, g_away_lost_arg);   /* only the setting was held */
    module_returns();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO, h_event_arg(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_FALSE(h_status().gnss_on);

    /* radio_end forgets the band; a stop, the receiver. */
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_stop());
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().receiving, 1000U));
    h_module_away();
    module_returns();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_FALSE(h_status().receiving);

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    h_module_away();
    module_returns();
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
}

TEST_CASE("setting: ended while the module is away, it is not put back either", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    h_module_away();
    /* The calls are refused: there is no module. They are heard all the same. */
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(false));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_call(MESH_REQUEST_GNSS_END, 2000U));
    module_returns();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO, h_event_arg(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_FALSE(h_status().gnss_on);

    harness_fresh();
    h_module_away();
    TEST_ASSERT_EQUAL(KODE_DENIED, h_call(MESH_REQUEST_RADIO_END, 2000U));
    module_returns();
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_FALSE(h_status().radio_ready);
}

TEST_CASE("setting: a packet on air or a test when the module left is never repeated", "[owner][presence][setting]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
    unsigned const sends = g_lr.n_send;
    h_module_away();
    module_returns();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT(sends, g_lr.n_send);
    TEST_ASSERT_FALSE(h_status().sending);

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_test((uint32_t)MESH_DRV_LR2021_TEST_PREAMBLE, 1000));
    unsigned const tests = g_lr.n_test;
    h_module_away();
    module_returns();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_RESTORED) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT(tests, g_lr.n_test);
    TEST_ASSERT_FALSE(h_status().testing);
}

TEST_CASE("setting: what cannot be put back is lost, and said", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    h_module_away();
    g_lr.configure_rc = KODE_IO_ERROR;   /* the module comes back, and will not take the setting */
    module_returns();

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));
    TEST_ASSERT_EQUAL_UINT32(GNSS, h_event_arg(MESH_AOS_EVENT_SETTING_RESTORED));   /* the GNSS did come back */
    TEST_ASSERT_FALSE(h_status().radio_ready);
    g_lr.configure_rc = KODE_OK;

    /* and it is not tried again by itself on the next return */
    h_module_away();
    TEST_ASSERT_EQUAL_UINT32(GNSS, g_away_lost_arg);
    module_returns();
    TEST_ASSERT_FALSE(h_status().radio_ready);
}

TEST_CASE("setting: a chip that would not confirm standby loses it, and it is not put back by a return", "[owner][presence][setting]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U };

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.standby_fails = 1000U;
    g_lr.reset_fails   = false;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send(DATA, sizeof(DATA), 2000U));
    g_lr.standby_fails = 0U;

    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_RADIO_ERROR, MESH_RADIO_ERROR_NO_STANDBY));
    TEST_ASSERT_FALSE(h_status().radio_ready);

    h_events_clear();
    h_module_away();
    TEST_ASSERT_EQUAL_UINT(0U, g_away_lost_n);   /* nothing left to lose */
    module_returns();
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_RESTORED));
    TEST_ASSERT_FALSE(h_status().radio_ready);
}

TEST_CASE("setting: an amplifier fault, a sleep, a home and a failed retune lose it, and say so", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    fake_ao_publish_rest();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));

    harness_fresh();
    fake_ao_publish_home();
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));

    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    mesh_aos_request_t re = { .kind = MESH_REQUEST_RADIO_RETUNE };
    re.config = (mesh_drv_lora_config_t){ .frequency_hz = 868100000U, .bandwidth_hz = 125000U, .spreading_factor = 9U,
                                          .coding_rate = 5U, .preamble = 8U, .sync_word = 0x12U, .power_cdbm = 400 };
    g_lr.retune_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_aos_call(&re, 2000U, NULL, NULL));
    g_lr.retune_rc = KODE_OK;
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_SETTING_LOST) == 1U, 1000U));
    TEST_ASSERT_EQUAL_UINT32(RADIO | RECEIVER, h_event_arg(MESH_AOS_EVENT_SETTING_LOST));
}

TEST_CASE("setting: a begin the app asks for, and one that fails, are not losses", "[owner][presence][setting]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 10U, 400));
    g_lr.configure_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_begin(MESH_PLAN_BAND_SUB_GHZ, 11U, 400));
    g_lr.configure_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_SETTING_LOST));
}
