/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_contract.c
 * @brief The small rules every call keeps alike: standby with nothing to stop, the
 *        spreading factor refused the same way by begin and configure, the power the
 *        status reports as applied, and a GNSS that will not open.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "owner_fakes.h"
#include "owner_harness.h"

TEST_CASE("standby: with no band set it is fine, and the chip is left alone", "[owner][contract]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_FALSE(h_status().radio_ready);

    unsigned const configured = g_lr.n_configure;
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_EQUAL_UINT(MESH_WHY_NONE, mesh_aos_why());
    TEST_ASSERT_FALSE(h_status().radio_ready);
    TEST_ASSERT_EQUAL_UINT(configured, g_lr.n_configure);

    /* and it still means what it did with a band: off air, receiver off */
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_TRUE(h_status().receiving);
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_TRUE(h_status().radio_ready);
}

TEST_CASE("begin: a spreading factor outside 7..12 is refused and the setting in force stays", "[owner][contract]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 10U, 400));
    unsigned const configured = g_lr.n_configure;
    static const uint8_t BAD[] = { 0U, 1U, 5U, 6U, 13U, 20U, 255U };
    for (unsigned i = 0U; i < sizeof(BAD); i++) {
        TEST_ASSERT_EQUAL(KODE_DENIED, h_begin(MESH_PLAN_BAND_SUB_GHZ, BAD[i], 400));
        TEST_ASSERT_EQUAL_UINT(MESH_WHY_NOT_ALLOWED, mesh_aos_why());
        TEST_ASSERT_EQUAL(KODE_DENIED, h_begin(MESH_PLAN_BAND_2_4_GHZ, BAD[i], 400));
    }
    TEST_ASSERT_EQUAL_UINT(configured, g_lr.n_configure);   /* the chip was not written */
    TEST_ASSERT_TRUE(h_status().radio_ready);
    TEST_ASSERT_EQUAL_UINT8(10U, h_status().spreading_factor);

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 7U, 400));
    TEST_ASSERT_EQUAL_UINT8(7U, h_status().spreading_factor);
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 12U, 400));
    TEST_ASSERT_EQUAL_UINT8(12U, h_status().spreading_factor);
}

TEST_CASE("status: the power applied is the nominal one the setting comes to, the one asked is kept as asked", "[owner][contract]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL_INT32(0, h_status().power_applied_cdbm);   /* no setting, none applied */

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 5000));
    TEST_ASSERT_EQUAL_INT32(5000, h_status().power_cdbm);
    TEST_ASSERT_EQUAL_INT32(850, h_status().power_applied_cdbm);

    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 700));
    TEST_ASSERT_EQUAL_INT32(700, h_status().power_applied_cdbm);
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_2_4_GHZ, 9U, 5000));
    TEST_ASSERT_EQUAL_INT32(450, h_status().power_applied_cdbm);
}

TEST_CASE("gnss begin: a driver that cannot open is an I/O error, the GNSS stays off and is not put back", "[owner][contract]")
{
    harness_fresh();
    unsigned const opens = g_gnss.opens;
    g_gnss.open_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_call(MESH_REQUEST_GNSS_BEGIN, 2000U));
    TEST_ASSERT_FALSE(h_status().gnss_on);

    h_events_clear();
    h_module_away();
    h_module_back();
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 10000));
    TEST_ASSERT_FALSE(h_status().gnss_on);   /* nothing was held, so nothing is put back */
    TEST_ASSERT_EQUAL_UINT(opens, g_gnss.opens);
}

TEST_CASE("codes: a code the app is not told of, from the chip's driver, is the device refusing", "[owner][contract]")
{
    static const uint8_t DATA[] = { 1U, 2U, 3U };
    static const kode_status_t ODD[] = { KODE_ERROR, KODE_EXISTS, KODE_NOT_DIR, KODE_IS_DIR };

    for (unsigned i = 0U; i < sizeof(ODD) / sizeof(ODD[0]); i++) {
        harness_fresh();
        g_lr.send_rc = ODD[i];
        TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_send(DATA, sizeof(DATA), 2000U));
        TEST_ASSERT_FALSE(h_status().sending);
        g_lr.receive_rc = ODD[i];
        TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_receive(true));
        g_lr.cad_rc = ODD[i];
        TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_call(MESH_REQUEST_RADIO_CAD, 2000U));
        g_lr.configure_rc = ODD[i];
        TEST_ASSERT_EQUAL(KODE_IO_ERROR, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    }
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));   /* and the owner is as it was */
}

TEST_CASE("codes: J3's supply is asked by command and answered by signal; a refusal is said in the status, never held against start", "[owner][contract]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());

    /* The link is down: start goes through all the same, and the supply says it failed. */
    g_exp.power_rc = KODE_DENIED;
    unsigned on = g_exp.power_on_calls;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_TRUE(WAIT_FOR(g_exp.power_on_calls > on, 1000U));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().supply == MESH_SUPPLY_FAILED, 1000U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());

    /* A full queue: asked again at every tick, until it fits. */
    g_exp.power_rc = KODE_NO_RESOURCES;
    on = g_exp.power_on_calls;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_TRUE(WAIT_FOR(g_exp.power_on_calls >= on + 3U, 2000U));
    g_exp.power_rc = KODE_OK;
    unsigned const seen = g_exp.power_on_calls;
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_ASSERT_TRUE(g_exp.power_on_calls <= seen + 2U);   /* and then it stops asking */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));

    /* And one that will not switch off: the owner has let go all the same. */
    g_exp.power_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_end());
    g_exp.power_rc = KODE_OK;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_start());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_wait_present(5000U));
}
