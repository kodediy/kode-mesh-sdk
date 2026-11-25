/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_sys_any.c
 * @brief Any call, in any state, in any order: the extended layer never asserts, never leaks a
 *        block and never leaves a door shut.
 *
 * Nothing is asserted about the values: the cases above say what each call returns. This one
 * only has to come out the other side. Every public function is called with good arguments, with
 * NULL and with out-of-range ones, from a started Active Object, from one that is not, with
 * the owner turning calls away, the pool dry, the queues full of other things, the news going
 * out, and the answers coming in any order. Q_onError() fails the suite, and tearDown() checks
 * the pool is whole.
 *
 * The one thing the caller owes is the header's queue-depth rule: an answer is posted with no
 * margin, so the case lets an Active Object's queue empty before it answers into it, as an
 * Active Object that kept the rule would have.
 */
#include "mesh_sys.h"

#include <string.h>

#include "unity.h"

#include "fake_owner.h"
#include "test_sys.h"

static uint32_t s_rng = 0x1badf00dU;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static unsigned pick(unsigned n)
{
    return (unsigned)(rnd() % n);
}

static QActive *any_asker(void)
{
    static QActive unstarted;   /* prio 0: never started */
    unsigned const k = pick(T_AOS + 2U);
    if (k == T_AOS) {
        return NULL;
    }
    if (k == (T_AOS + 1U)) {
        return &unstarted;
    }
    return t_ao(k);
}

/** @brief Let every queue empty, as an AO that kept the depth rule would have. */
static void empty_queues(void)
{
    for (unsigned i = 0U; i < T_AOS; i++) {
        (void)t_drain(i);
    }
}

/** @brief Answer everything still asked, one at a time, each into an empty queue. */
static void settle(void)
{
    for (unsigned i = 0U; i < g_owner.pending; i++) {
        empty_queues();
        (void)fake_owner_answer(i, KODE_OK, MESH_WHY_NONE, 0U, 0U);
    }
    empty_queues();
}

static void one_step(void)
{
    static const uint8_t  bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint32_t big[MESH_CALIBRATIONS_MAX + 2U] = { 1, 2, 3, 4 };
    mesh_radio_config_t   cfg;
    mesh_status_t         st;
    mesh_packet_t         rx;
    mesh_gnss_fix_t       fix;
    float                 dbm;
    bool                  busy;
    QActive              *me = any_asker();

    (void)mesh_sys_radio_config_default((mesh_band_t)pick(4U), &cfg);
    if (pick(4U) == 0U) { cfg.power_dbm_x10 = (int32_t)rnd(); }
    if (pick(8U) == 0U) { cfg.spreading_factor = (uint8_t)rnd(); }

    switch (pick(50U)) {
    case 0:  (void)mesh_sys_begin(); break;
    case 1:  (void)mesh_sys_end(); break;
    case 2:  (void)mesh_sys_radio_standby(); break;
    case 3:  (void)mesh_sys_radio_receive((rnd() & 1U) != 0U); break;
    case 4:  (void)mesh_sys_radio_listen(rnd() % 2000U); break;
    case 5:  (void)mesh_sys_stop(); break;
    case 6:  (void)mesh_sys_radio_begin(me, (mesh_band_t)pick(4U), (uint8_t)rnd(), (int32_t)rnd()); break;
    case 7:  (void)mesh_sys_radio_configure(me, (pick(5U) == 0U) ? NULL : &cfg); break;
    case 8:  (void)mesh_sys_radio_calibrate(me, (pick(5U) == 0U) ? NULL : big, (uint8_t)pick(MESH_CALIBRATIONS_MAX + 3U)); break;
    case 9:  (void)mesh_sys_radio_retune(me, (pick(5U) == 0U) ? NULL : &cfg); break;
    case 10: (void)mesh_sys_radio_end(me); break;
    case 11: (void)mesh_sys_radio_send(me, (pick(5U) == 0U) ? NULL : bytes, (uint8_t)pick(9U)); break;
    case 12: (void)mesh_sys_radio_send_preamble(me, (pick(5U) == 0U) ? NULL : bytes, (uint8_t)pick(9U), (uint16_t)pick(20U)); break;
    case 13: (void)mesh_sys_radio_cad(me, (uint8_t)pick(8U), (rnd() & 1U) != 0U); break;
    case 14: (void)mesh_sys_radio_test(me, (mesh_test_signal_t)pick(4U), rnd() % 20000U); break;
    case 15: (void)mesh_sys_gnss_begin(me); break;
    case 16: (void)mesh_sys_gnss_end(me); break;
    case 17: (void)mesh_sys_gnss_reset(me); break;
    case 18: (void)mesh_sys_i2c_lend(me); break;
    case 19: (void)mesh_sys_i2c_return(me); break;
    case 20: (void)mesh_sys_subscribe(me); break;
    case 21: (void)mesh_sys_unsubscribe(me); break;
    case 22: (void)mesh_sys_is_present(); break;
    case 23: (void)mesh_sys_status_read((pick(5U) == 0U) ? NULL : &st); break;
    case 24: (void)mesh_sys_header_position((uint8_t)rnd()); break;
    case 25: (void)mesh_sys_why_text((mesh_why_t)rnd()); break;
    case 26: (void)mesh_sys_radio_read((pick(5U) == 0U) ? NULL : &rx); break;
    case 27: (void)mesh_sys_radio_rssi((pick(5U) == 0U) ? NULL : &dbm); break;
    case 28: (void)mesh_sys_radio_channel_busy((pick(5U) == 0U) ? NULL : &busy); break;
    case 29: (void)mesh_sys_radio_send_wait_ms(); break;
    case 30: (void)mesh_sys_radio_config_default((mesh_band_t)pick(4U), (pick(5U) == 0U) ? NULL : &cfg); break;
    case 31: (void)mesh_sys_gnss_fix((pick(5U) == 0U) ? NULL : &fix); break;
    case 32: (void)mesh_sys_gnss_pps(); break;

    /* The owner's side: its answers, in any order, and its news. */
    case 33:
    case 34:
    case 35:
        if (g_owner.pending > 0U) {
            empty_queues();
            (void)fake_owner_answer(pick(g_owner.pending), (kode_status_t)pick(6U), (uint8_t)pick(MESH_WHY_COUNT + 2U), rnd(), rnd());
        }
        break;
    case 36:
    case 37:
        (void)fake_owner_tell((uint8_t)pick(20U), rnd());
        break;

    /* The world around it: queues, pool, what the owner will say. */
    case 38: empty_queues(); break;
    case 39: {
        unsigned const i = pick(T_AOS);
        t_fill(i, pick(t_free_slots(i) + 1U));
        break;
    }
    case 40: t_pool_hog(pick(6U)); break;
    case 41: t_pool_release(); break;
    case 42: g_owner.ask_rc = (kode_status_t)pick(6U); break;
    case 43: g_owner.post_rc = (kode_status_t)pick(6U); break;
    case 44: g_owner.start_rc = (kode_status_t)pick(6U); g_owner.stop_rc = (kode_status_t)pick(6U); break;
    case 45: g_owner.kode_ready = (rnd() & 1U) != 0U; break;
    case 47: (void)mesh_sys_radio_cad_receive(me, (uint8_t)pick(8U), (rnd() & 1U) != 0U); break;
    case 48: (void)mesh_sys_radio_antennas_confirm((rnd() & 1U) != 0U); break;
    case 46: g_owner.st.present = (rnd() & 1U) != 0U; g_owner.st.receiving = (rnd() & 1U) != 0U; g_owner.st.gnss_on = (rnd() & 1U) != 0U; break;
    default: g_owner.ask_rc = KODE_OK; g_owner.post_rc = KODE_OK; g_owner.kode_ready = true; break;
    }
}

TEST_CASE("any call in any state: nothing asserts, nothing leaks, no door stays shut", "[sys][any]")
{
    for (unsigned round = 0U; round < 40U; round++) {
        fake_owner_reset();
        (void)mesh_sys_begin();
        for (unsigned step = 0U; step < 400U; step++) {
            one_step();
            if (g_owner.pending >= (FAKE_PENDING_MAX - 4U)) {
                break;
            }
        }
        /* The world back to normal, everything asked answered, and then every door must be open. */
        t_pool_release();
        g_owner.ask_rc = KODE_OK;
        g_owner.post_rc = KODE_OK;
        settle();
        fake_owner_reset();
        (void)mesh_sys_begin();
        for (unsigned a = 0U; a < T_AOS; a++) {
            (void)mesh_sys_unsubscribe(t_ao(a));
            for (unsigned k = 0U; k < T_REQUEST_COUNT; k++) {
                TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, T_REQUESTS[k].ask(t_ao(a)), T_REQUESTS[k].name);
            }
            settle();   /* the pool holds one answer for each, so one asker at a time */
            fake_owner_reset();
            (void)mesh_sys_begin();
        }
        fake_owner_reset();
    }
}
