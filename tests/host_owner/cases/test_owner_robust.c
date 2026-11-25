/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_robust.c
 * @brief Any call, in any state, in any order: an app is never brought down by the SDK.
 *
 * Every kind of request the owner serves is made thousands of times, in an order
 * drawn from a fixed seed, with good arguments and with garbage, while the module
 * is plugged out and in, the GNSS comes and goes, the chip raises any flag it likes,
 * stops what it was doing by itself, or fails the next command. After each call:
 * the answer is one of the five codes mesh.h documents; a refusal leaves a reason
 * and a success none; the call came back within its own limit; and the status is
 * coherent - nothing on air with no radio set up, nothing held with no module.
 * Nothing asserts, nothing hangs (the suite itself would not return), and when it is
 * over the owner still serves an ordinary begin and send.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

#define ROUNDS  2500U

static uint32_t s_rng;
static unsigned s_rc_n[8];
static unsigned s_why_n[MESH_WHY_COUNT];
static unsigned s_kind_ok[24];

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t below(uint32_t n)
{
    return rnd() % n;
}

static bool documented(kode_status_t rc)
{
    return (rc == KODE_OK) || (rc == KODE_DENIED) || (rc == KODE_TIMEOUT)
           || (rc == KODE_NO_RESOURCES) || (rc == KODE_IO_ERROR);
}

static kode_status_t chip_rc(void)
{
    static const kode_status_t RCS[] = { KODE_OK, KODE_OK, KODE_OK, KODE_OK, KODE_IO_ERROR, KODE_TIMEOUT, KODE_ERROR, KODE_EXISTS };
    return RCS[below(sizeof(RCS) / sizeof(RCS[0]))];
}

/** @brief The chip's flags: the ones it has, now and then every bit. */
static uint32_t any_flags(void)
{
    static const uint32_t FLAGS[] = {
        MESH_DRV_LR2021_INTERRUPT_PREAMBLE, MESH_DRV_LR2021_INTERRUPT_HEADER_VALID,
        MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR, MESH_DRV_LR2021_INTERRUPT_CAD_DONE,
        MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED, MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE,
        MESH_DRV_LR2021_INTERRUPT_SEND_DONE, MESH_DRV_LR2021_INTERRUPT_TIMEOUT,
        MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, MESH_DRV_LR2021_INTERRUPT_ERROR,
        MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR, MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT,
    };
    if (below(8U) == 0U) {
        return rnd();
    }
    uint32_t f = 0U;
    for (unsigned i = 0U; i < 1U + below(3U); i++) {
        f |= FLAGS[below(sizeof(FLAGS) / sizeof(FLAGS[0]))];
    }
    return f;
}

static mesh_drv_lora_config_t any_config(void)
{
    mesh_drv_lora_config_t c = {
        .frequency_hz = (below(2U) != 0U) ? 868300000U : 2445000000U,
        .bandwidth_hz = (below(2U) != 0U) ? 125000U : 812500U,
        .spreading_factor = (uint8_t)(5U + below(9U)),
        .coding_rate = 5U, .preamble = 8U, .sync_word = 0x12U,
        .power_cdbm = (int32_t)below(1500U),
    };
    if (below(2U) == 0U) {
        c.frequency_hz = rnd();
        c.bandwidth_hz = rnd();
        c.spreading_factor = (uint8_t)rnd();
        c.coding_rate = (uint8_t)rnd();
        c.preamble = (uint16_t)rnd();
        c.power_cdbm = (int32_t)rnd();
    }
    return c;
}

static void chip_flaky(void)
{
    g_lr.send_rc = chip_rc();
    g_lr.receive_rc = chip_rc();
    g_lr.listen_rc = chip_rc();
    g_lr.cad_rc = chip_rc();
    g_lr.test_rc = chip_rc();
    g_lr.configure_rc = chip_rc();
    g_lr.calibrate_rc = chip_rc();
    g_lr.retune_rc = chip_rc();
    g_lr.standby_fails = below(3U);
}

static void chip_heals(void)
{
    g_lr.send_rc = KODE_OK;
    g_lr.receive_rc = KODE_OK;
    g_lr.listen_rc = KODE_OK;
    g_lr.cad_rc = KODE_OK;
    g_lr.test_rc = KODE_OK;
    g_lr.configure_rc = KODE_OK;
    g_lr.calibrate_rc = KODE_OK;
    g_lr.retune_rc = KODE_OK;
    g_lr.standby_fails = 0U;
}

/** @brief The module is pulled out. The owner may or may not notice before the next call:
 *         one ended by the app is not looking, and one held in reset by a sleep is not
 *         taken for unplugged. So this waits a little and asks nothing of it. */
static void pull_out(void)
{
    pins_fake_set_mesh(false);
    g_lr.alive = false;
    g_ao.tick_on = true;
    (void)WAIT_FOR(!h_status().present, 1500U);
}

/** @brief And plugged in again; the owner finds it at its next look. */
static void plug_in(bool ended)
{
    h_module_back();
    if (ended) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_start(), "start after end");
    }
    TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(h_status().present, 15000U), "the module never came back");
}

/** @brief What must hold of the status whatever has happened. */
static void coherent(const char *what)
{
    mesh_aos_status_t const st = h_status();

    if (!st.present) {
        TEST_ASSERT_FALSE_MESSAGE(st.radio_ready, what);
        TEST_ASSERT_FALSE_MESSAGE(st.gnss_on, what);
        TEST_ASSERT_FALSE_MESSAGE(st.sending, what);
        TEST_ASSERT_FALSE_MESSAGE(st.testing, what);
        TEST_ASSERT_FALSE_MESSAGE(st.receiving, what);
        TEST_ASSERT_FALSE_MESSAGE(st.cad_running, what);
        TEST_ASSERT_FALSE_MESSAGE(st.arriving, what);
        TEST_ASSERT_FALSE_MESSAGE(st.i2c_taken, what);
    }
    if (!st.radio_ready) {
        TEST_ASSERT_FALSE_MESSAGE(st.sending, what);
        TEST_ASSERT_FALSE_MESSAGE(st.testing, what);
        TEST_ASSERT_FALSE_MESSAGE(st.receiving, what);
        TEST_ASSERT_FALSE_MESSAGE(st.cad_running, what);
        TEST_ASSERT_FALSE_MESSAGE(st.arriving, what);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, st.power_applied_cdbm, what);
    }
    TEST_ASSERT_FALSE_MESSAGE(st.sending && st.testing, what);
    TEST_ASSERT_FALSE_MESSAGE(st.testing && st.cad_running, what);
    TEST_ASSERT_FALSE_MESSAGE(st.sending && st.cad_running, what);
    TEST_ASSERT_TRUE_MESSAGE(st.band < MESH_BAND_COUNT, what);
    TEST_ASSERT_TRUE_MESSAGE(st.packets_waiting <= 4U, what);
}

TEST_CASE("robust: every request, in any order and state, with any argument, is answered and leaves the owner coherent", "[owner][robust]")
{
    static uint8_t buffer[300];
    bool           away = false;
    bool           ended = false;
    unsigned       trips = 0U;
    unsigned       away_left = 0U;
    unsigned       ended_left = 0U;

    s_rng = 0x6D657368U;
    harness_fresh();
    for (unsigned i = 0U; i < sizeof(buffer); i++) {
        buffer[i] = (uint8_t)(i * 7U);
    }

    for (unsigned round = 0U; round < ROUNDS; round++) {
        mesh_aos_request_t req;
        (void)memset(&req, 0, sizeof(req));
        bool       call = true;
        uint32_t   ms = 600U;
        uint32_t   out1 = 0U;
        uint32_t   out2 = 0U;
        uint32_t   op = below(34U);
        kode_status_t rc = KODE_OK;
        bool       by_owner = true;   /* the call reaches the owner's code, not the wrapper alone */

        /* The chip is flaky for a while, and then it is not. */
        if (below(14U) == 0U) {
            chip_flaky();
        } else if (below(6U) == 0U) {
            chip_heals();
        }

        /* Now and then the world changes under the app's feet. */
        if (away && (--away_left == 0U)) {
            plug_in(ended);
            ended = false;
            away = false;
        }
        if (ended && (--ended_left == 0U)) {
            TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, mesh_aos_start(), "start after end");
            ended = false;
            TEST_ASSERT_TRUE_MESSAGE(away || WAIT_FOR(h_status().present, 15000U), "the module never came back after a start");
        }
        switch (below(120U)) {
        case 0:
            if (!away && (trips < 25U)) {
                pull_out();
                away = true;
                away_left = 3U + below(8U);
                trips++;
            }
            break;
        case 1:
            fake_ao_publish_rest();
            break;
        case 2:
            fake_ao_publish_home();
            break;
        case 3:
            if (!ended) {
                rc = mesh_aos_end();
                TEST_ASSERT_TRUE_MESSAGE(documented(rc), "end: an undocumented code");
                ended = true;
                ended_left = 3U + below(6U);
            }
            break;
        case 4:
            rc = mesh_aos_start();   /* already started: nothing */
            TEST_ASSERT_TRUE_MESSAGE(documented(rc), "start: an undocumented code");
            break;
        default:
            break;
        }

        switch (op) {
        case 0: case 1: case 2:
            req.kind = MESH_REQUEST_RADIO_BEGIN;
            if (below(8U) < 5U) {   /* mostly a setting a person would ask for */
                req.a = (uint8_t)below(MESH_BAND_COUNT);
                req.u = 5U + below(8U);
                req.i = (int32_t)(100U + below(1300U));
            } else {
                req.a = (uint8_t)below(4U);
                req.u = below(20U);
                req.i = (below(4U) == 0U) ? (int32_t)rnd() : (int32_t)below(2500U);
            }
            break;
        case 3:
            req.kind = MESH_REQUEST_RADIO_END;
            break;
        case 4:
            req.kind = MESH_REQUEST_RADIO_STANDBY;
            break;
        case 5: case 6: case 7:
            req.kind = MESH_REQUEST_RADIO_SEND;
            req.data = (below(6U) == 0U) ? NULL : buffer;
            req.len = (uint8_t)((below(4U) == 0U) ? 255U : below(256U));
            req.preamble = (uint16_t)((below(3U) == 0U) ? rnd() : 0U);
            call = false;   /* a call to send waits for the airtime: asked, not waited for, here */
            break;
        case 8: case 9:
            req.kind = MESH_REQUEST_RADIO_RECEIVE;
            req.u = below(3U);
            break;
        case 10:
            req.kind = MESH_REQUEST_RADIO_LISTEN;
            req.u = (below(3U) == 0U) ? rnd() : below(1200U);
            break;
        case 11:
            req.kind = MESH_REQUEST_I2C_LEND;
            break;
        case 12: case 13:
            req.kind = MESH_REQUEST_RADIO_TEST;
            req.u = below(5U);
            req.i = (below(3U) == 0U) ? (int32_t)rnd() : (int32_t)below(1500U);
            break;
        case 14:
            req.kind = MESH_REQUEST_GNSS_BEGIN;
            break;
        case 15:
            req.kind = MESH_REQUEST_GNSS_END;
            break;
        case 16:
            req.kind = MESH_REQUEST_GNSS_RESET;
            call = false;
            break;
        case 17:
            req.kind = MESH_REQUEST_I2C_LEND;
            break;
        case 18:
            req.kind = MESH_REQUEST_I2C_RETURN;
            break;
        case 19: case 20:
            req.kind = MESH_REQUEST_RADIO_CONFIGURE;
            req.config = any_config();
            break;
        case 21:
            req.kind = MESH_REQUEST_RADIO_RETUNE;
            req.config = any_config();
            break;
        case 22:
            req.kind = MESH_REQUEST_RADIO_CAD;
            req.u = (below(3U) == 0U) ? rnd() : below(6U);
            req.a = (uint8_t)below(2U);
            break;
        case 23:
            req.kind = MESH_REQUEST_RADIO_CAD;
            ms = 300U;
            break;
        case 24:
            req.kind = MESH_REQUEST_RADIO_CALIBRATE;
            req.u = (below(3U) == 0U) ? rnd() : below(5U);
            for (unsigned i = 0U; i < MESH_AOS_CALIBRATIONS; i++) {
                req.hz[i] = (below(3U) == 0U) ? rnd() : 868300000U;
            }
            break;
        case 25:
            /* a kind that does not exist, and the one the owner keeps to itself */
            req.kind = (mesh_request_kind_t)((below(2U) == 0U) ? (50U + below(200U)) : (uint32_t)MESH_REQUEST_RADIO_HOLD_RESET);
            call = (below(2U) == 0U);
            break;
        case 26:
            /* ends the radio's chance of being quiet: the chip leaves what it was doing */
            lr_fake_chip_stops(any_flags());
            by_owner = false;
            break;
        case 27:
            lr_fake_raise(any_flags());
            by_owner = false;
            break;
        case 28: {
            uint8_t pkt[40];
            for (unsigned i = 0U; i < sizeof(pkt); i++) {
                pkt[i] = (uint8_t)rnd();
            }
            lr_fake_inject(pkt, (uint8_t)(1U + below(sizeof(pkt))), below(2U) != 0U);
            by_owner = false;
            break;
        }
        case 29:
            vTaskDelay(pdMS_TO_TICKS(below(40U)));
            by_owner = false;
            break;
        case 30:
            chip_heals();
            by_owner = false;
            break;
        case 31: {
            mesh_aos_packet_t pk;
            rc = mesh_aos_packet_read((below(5U) == 0U) ? NULL : &pk);
            TEST_ASSERT_TRUE_MESSAGE(documented(rc), "packet_read: an undocumented code");
            by_owner = false;
            break;
        }
        case 32:
            rc = mesh_aos_stop();
            TEST_ASSERT_TRUE_MESSAGE(documented(rc), "stop: an undocumented code");
            by_owner = false;
            break;
        default: {
            /* a call with nothing at all, and the reads */
            rc = mesh_aos_call(NULL, 100U, NULL, NULL);
            TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, rc, "a null request");
            mesh_aos_status(NULL);
            (void)mesh_aos_header_position((uint8_t)rnd());
            (void)mesh_aos_present();
            by_owner = false;
            break;
        }
        }

        if (by_owner) {
            uint32_t const t0 = kode_sys_time_ms();
            if (call) {
                rc = mesh_aos_call(&req, ms, &out1, &out2);
            } else {
                rc = mesh_aos_post(&req);
            }
            uint32_t const took = kode_sys_time_ms() - t0;
            TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(ms + MESH_AOS_CALL_GRACE_MS + 500U, took, "a call that did not come back in its limit");
            TEST_ASSERT_TRUE_MESSAGE(documented(rc), "a code mesh.h does not document");
            if (call) {
                s_rc_n[(unsigned)rc & 7U]++;
                s_why_n[mesh_aos_why() % MESH_WHY_COUNT]++;
                if ((rc == KODE_OK) && ((unsigned)req.kind < 24U)) {
                    s_kind_ok[(unsigned)req.kind]++;
                }
                if (rc == KODE_OK) {
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(MESH_WHY_NONE, mesh_aos_why(), "OK with a reason");
                } else if (rc == KODE_DENIED) {
                    TEST_ASSERT_TRUE_MESSAGE(mesh_aos_why() != MESH_WHY_NONE, "DENIED with no reason");
                }
                TEST_ASSERT_TRUE_MESSAGE(mesh_aos_why() < MESH_WHY_COUNT, "a reason out of range");
            }
        }
        coherent("the status after a call");
    }

    /* The walk did reach the owner's code: a good share of the calls went through, and
     * through more than a few kinds of request. */
    unsigned kinds = 0U;
    for (unsigned k = 0U; k < 24U; k++) {
        kinds += (s_kind_ok[k] != 0U) ? 1U : 0U;
    }
    printf("robust: ok %u denied %u timeout %u no_resources %u io %u; %u kinds went through\n",
           s_rc_n[KODE_OK], s_rc_n[KODE_DENIED], s_rc_n[KODE_TIMEOUT], s_rc_n[KODE_NO_RESOURCES], s_rc_n[KODE_IO_ERROR], kinds);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(ROUNDS / 8U, s_rc_n[KODE_OK], "the walk hardly got anything through");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(11U, kinds, "the walk got too few kinds of request through");
    TEST_ASSERT_TRUE_MESSAGE(s_why_n[MESH_WHY_NOT_BEGUN] > 0U, "no call met a radio that was not set up");
    TEST_ASSERT_TRUE_MESSAGE(s_why_n[MESH_WHY_MODULE_ABSENT] > 0U, "no call met a module that was not there");
    fake_clock_skip(3600000U);   /* the packets of the walk spent the duty budget: an hour passes */
    /* Over: the owner is still the owner. */
    if (away) {
        plug_in(ended);
    }
    (void)mesh_aos_start();
    chip_heals();
    harness_fresh();
    static const uint8_t DATA[] = { 1U, 2U, 3U };
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_STANDBY, 2000U));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().present, 10000));
    TEST_ASSERT_EQUAL(KODE_OK, h_begin(MESH_PLAN_BAND_SUB_GHZ, 9U, 400));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(DATA, sizeof(DATA), 2000U));
}
