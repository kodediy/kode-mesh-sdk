/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_standard_any.c
 * @brief Any call of mesh.h, in any state, with any argument, never takes the app down.
 *
 * Nothing is asserted about the values: test_standard.c says what each call returns. This only has
 * to come out the other side - under the sanitizers when the suite is built with them - with
 * NULL, out-of-range arguments, a caller that is an Active Object, an owner that is not running,
 * answers of every kind, and a status of random bits.
 */
#include "mesh.h"

#include <string.h>

#include "unity.h"

#include "fake_mesh_aos.h"

static uint32_t s_rng = 0x2545f491U;

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

static void on_event(const mesh_event_t *event)
{
    (void)event;
}

static void world(void)
{
    static const kode_status_t RCS[] = { KODE_OK, KODE_OK, KODE_DENIED, KODE_NO_RESOURCES, KODE_TIMEOUT, KODE_IO_ERROR };
    g_aos.rc        = RCS[pick(6U)];
    g_aos.recv_rc   = RCS[pick(6U)];
    g_aos.wait_rc   = RCS[pick(6U)];
    g_aos.start_rc  = RCS[pick(6U)];
    g_aos.stop_rc   = RCS[pick(6U)];
    g_aos.end_rc    = RCS[pick(6U)];
    g_aos.present   = (rnd() & 1U) != 0U;
    g_aos.running   = (rnd() & 3U) != 0U;
    g_aos.from_ao   = (rnd() & 3U) == 0U;
    g_aos.add_ok    = (rnd() & 1U) != 0U;
    g_aos.remove_ok = (rnd() & 1U) != 0U;
    g_aos.wait_ok   = (rnd() & 1U) != 0U;
    g_aos.known     = (rnd() & 1U) != 0U;
    g_aos.out1      = rnd();
    g_aos.out2      = rnd();
    uint32_t bits = rnd();
    g_aos.st.present = (bits & 1U) != 0U;
    g_aos.st.radio_ready = (bits & 2U) != 0U;
    g_aos.st.receiving = (bits & 4U) != 0U;
    g_aos.st.arriving = (bits & 8U) != 0U;
    g_aos.st.cad_running = (bits & 16U) != 0U;
    g_aos.st.rssi_valid = (bits & 32U) != 0U;
    g_aos.st.gnss_on = (bits & 64U) != 0U;
    g_aos.st.fix.fix = (bits & 128U) != 0U;
    g_aos.st.band = (uint8_t)rnd();
    g_aos.st.supply = (uint8_t)rnd();
    g_aos.st.seen = (uint8_t)rnd();
    g_aos.st.power_cdbm = (int32_t)rnd();
    g_aos.rx.length = (uint8_t)rnd() % (MESH_PAYLOAD_MAX + 1U);
}

TEST_CASE("any call in any state: never an assert, never a write past what was given", "[standard][any]")
{
    static const uint8_t  bytes[8]  = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint32_t freqs[4]  = { 1, 2, 3, 4 };
    mesh_radio_config_t   cfg;
    mesh_status_t         st;
    mesh_packet_t         rx;
    mesh_gnss_fix_t       fix;
    mesh_event_t          ev;
    float                 dbm;
    bool                  b;
    uint8_t               scl, sda;

    for (unsigned step = 0U; step < 20000U; step++) {
        if ((step % 7U) == 0U) {
            world();
        }
        (void)mesh_radio_config_default((mesh_band_t)pick(4U), &cfg);
        if (pick(4U) == 0U) { cfg.power_dbm_x10 = (int32_t)rnd(); }
        switch (pick(40U)) {
        case 0:  (void)mesh_begin(); break;
        case 1:  (void)mesh_end(); break;
        case 2:  (void)mesh_error(); break;
        case 3:  (void)mesh_is_present(); break;
        case 4:  (void)mesh_wait_present(rnd()); break;
        case 5:  (void)mesh_status((pick(5U) == 0U) ? NULL : &st); break;
        case 6:  (void)mesh_stop(); break;
        case 7:  (void)mesh_header_position((uint8_t)rnd()); break;
        case 8:  (void)mesh_event_add_listener((pick(5U) == 0U) ? NULL : "packet_received", (pick(5U) == 0U) ? NULL : on_event); break;
        case 9:  (void)mesh_event_remove_listener((pick(5U) == 0U) ? NULL : "packet_received", (pick(5U) == 0U) ? NULL : on_event); break;
        case 10: (void)mesh_event_wait((pick(5U) == 0U) ? NULL : "module_found", rnd() % 3U, (pick(5U) == 0U) ? NULL : &ev); break;
        case 11: (void)mesh_radio_begin((mesh_band_t)pick(4U), (uint8_t)rnd(), (int32_t)rnd()); break;
        case 12: (void)mesh_radio_config_default((mesh_band_t)pick(4U), (pick(5U) == 0U) ? NULL : &cfg); break;
        case 13: (void)mesh_radio_configure((pick(5U) == 0U) ? NULL : &cfg); break;
        case 14: (void)mesh_radio_calibrate((pick(5U) == 0U) ? NULL : freqs, (uint8_t)pick(6U)); break;
        case 15: (void)mesh_radio_retune((pick(5U) == 0U) ? NULL : &cfg); break;
        case 16: (void)mesh_radio_end(); break;
        case 17: (void)mesh_radio_standby(); break;
        case 18: (void)mesh_radio_send((pick(5U) == 0U) ? NULL : bytes, (uint8_t)pick(10U)); break;
        case 19: (void)mesh_radio_send_preamble((pick(5U) == 0U) ? NULL : bytes, (uint8_t)pick(10U), (uint16_t)pick(20U)); break;
        case 20: (void)mesh_radio_send_wait_ms(); break;
        case 21: (void)mesh_radio_receive((rnd() & 1U) != 0U); break;
        case 22: (void)mesh_radio_read((pick(5U) == 0U) ? NULL : &rx); break;
        case 23: (void)mesh_radio_rssi((pick(5U) == 0U) ? NULL : &dbm); break;
        case 24: (void)mesh_radio_channel_busy((pick(5U) == 0U) ? NULL : &b); break;
        case 25: (void)mesh_radio_cad((uint8_t)pick(8U), (rnd() & 1U) != 0U, (pick(3U) == 0U) ? NULL : &b); break;
        case 26: (void)mesh_radio_listen(rnd() % 3000U); break;
        case 27: (void)mesh_radio_test((mesh_test_signal_t)pick(4U), rnd() % 20000U); break;
        case 28: (void)mesh_gnss_begin(); break;
        case 29: (void)mesh_gnss_end(); break;
        case 30: (void)mesh_gnss_reset(); break;
        case 31: (void)mesh_gnss_fix((pick(5U) == 0U) ? NULL : &fix); break;
        case 32: (void)mesh_gnss_pps(); break;
        case 33: (void)mesh_i2c_lend((pick(3U) == 0U) ? NULL : &scl, (pick(3U) == 0U) ? NULL : &sda); break;
        case 34: (void)mesh_i2c_return(); break;
        case 35: (void)mesh_radio_cad_receive((uint8_t)pick(8U), (rnd() & 1U) != 0U, (pick(3U) == 0U) ? NULL : &b); break;
        case 36: (void)mesh_radio_antennas_confirm((rnd() & 1U) != 0U); break;
        default: break;
        }
    }
    TEST_PASS();
}
