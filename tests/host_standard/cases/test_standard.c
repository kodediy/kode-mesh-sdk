/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_standard.c
 * @brief mesh.c: the standard layer, the calls of a task, over a fake owner.
 *
 * One case per return value of every public function of mesh.h. What is checked is what the layer
 * owns: which arguments it refuses before the owner is asked, which request it builds, how it
 * turns the owner's verdict into a bool and a reason (mesh_error()), and that a call from an
 * Active Object is refused with the call's name before anything else happens. The owner's own
 * behaviour is host_owner's.
 */
#include "mesh.h"

#include <string.h>

#include "unity.h"

#include "fake_mesh_aos.h"

#define NOTED(expected)  TEST_ASSERT_EQUAL_UINT8((uint8_t)(expected), g_aos.why)

/** @brief Nothing reached the owner: no request, no start, no stop, no end. */
static void nothing_asked(void)
{
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.starts);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.stops);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.ends);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.waits);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.listener_calls);
}

static const uint8_t  DATA[3]  = { 1U, 2U, 3U };
static const uint32_t FREQS[3] = { 868100000U, 868300000U, 868500000U };

static mesh_radio_config_t config(void)
{
    mesh_radio_config_t c;
    /* the owner's argument function, not mesh_radio_config_default(): that one notes a reason */
    TEST_ASSERT_TRUE(mesh_aos_arg_radio_config_default(MESH_BAND_SUB_GHZ, &c));
    return c;
}

static void on_event(const mesh_event_t *event)
{
    (void)event;
}

/* ---- Begin and end ------------------------------------------------------------------------ */

TEST_CASE("begin: true when the owner starts, and kode_init() ran first", "[standard][begin]")
{
    TEST_ASSERT_TRUE(mesh_begin());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.inits);
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.starts);
}

TEST_CASE("begin: false when the owner cannot start", "[standard][begin]")
{
    static const kode_status_t RCS[] = { KODE_DENIED, KODE_NO_RESOURCES, KODE_TIMEOUT, KODE_IO_ERROR };
    for (unsigned i = 0U; i < (sizeof(RCS) / sizeof(RCS[0])); i++) {
        g_aos.start_rc = RCS[i];
        TEST_ASSERT_FALSE(mesh_begin());
    }
}

TEST_CASE("end: true when the owner lets go, false when it does not", "[standard][begin]")
{
    TEST_ASSERT_TRUE(mesh_end());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.ends);
    g_aos.end_rc = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(mesh_end());
    g_aos.end_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(mesh_end());
}

TEST_CASE("error: is the owner's text", "[standard][error]")
{
    TEST_ASSERT_EQUAL_STRING("the owner's reason", mesh_error());
}

/* ---- The module --------------------------------------------------------------------------- */

TEST_CASE("is_present: the owner's word, true and false", "[standard][module]")
{
    TEST_ASSERT_TRUE(mesh_is_present());
    g_aos.present = false;
    TEST_ASSERT_FALSE(mesh_is_present());
}

TEST_CASE("wait_present: true when it came, the time goes down", "[standard][module]")
{
    TEST_ASSERT_TRUE(mesh_wait_present(1234U));
    TEST_ASSERT_EQUAL_UINT32(1234U, g_aos.last_ms);
    NOTED(MESH_WHY_NONE);
}

TEST_CASE("wait_present: false on time out, with the module absent as the reason", "[standard][module]")
{
    g_aos.wait_rc = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(mesh_wait_present(10U));
    NOTED(MESH_WHY_MODULE_ABSENT);
}

TEST_CASE("wait_present: false when the owner is not running, with no reason of its own", "[standard][module]")
{
    g_aos.wait_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(mesh_wait_present(10U));
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.notes);
}

TEST_CASE("status: true copies every field the owner reports, the fix in the app's words", "[standard][module]")
{
    g_aos.st.present = true;
    g_aos.st.radio_ready = true;
    g_aos.st.band = (uint8_t)MESH_BAND_2_4_GHZ;
    g_aos.st.spreading_factor = 7U;
    g_aos.st.frequency_hz = 2403000000U;
    g_aos.st.bandwidth_hz = 812000U;
    g_aos.st.coding_rate = 5U;
    g_aos.st.preamble = 12U;
    g_aos.st.sync_word = 0x12U;
    g_aos.st.power_cdbm = 1000;
    g_aos.st.power_applied_cdbm = 950;
    g_aos.st.sending = true;
    g_aos.st.receiving = true;
    g_aos.st.arriving = true;
    g_aos.st.cad_running = true;
    g_aos.st.testing = true;
    g_aos.st.amplifier_fault = true;
    g_aos.st.duty_wait_ms = 61000U;
    g_aos.st.last_rssi_dbm = -97.5f;
    g_aos.st.rssi_valid = true;
    g_aos.st.packets_waiting = 2U;
    g_aos.st.packets_received = 11U;
    g_aos.st.packets_dropped = 3U;
    g_aos.st.packets_sent = 9U;
    g_aos.st.radio_errors = 0x0102U;
    g_aos.st.gnss_on = true;
    g_aos.st.fix.fix = true;
    g_aos.st.fix.lat = 39.47;
    g_aos.st.fix.lon = -0.37;
    g_aos.st.fix.sats_used = 8U;
    g_aos.st.fix_age_ms = 700U;
    g_aos.st.gnss_sentences = 55U;
    g_aos.st.gnss_baud = 9600U;
    g_aos.st.gnss_talking = true;
    g_aos.st.pps_high = true;
    g_aos.st.i2c_taken = true;
    g_aos.st.supply = (uint8_t)MESH_SUPPLY_ON;
    g_aos.st.events_dropped = 4U;
    g_aos.st.news_dropped = 5U;
    g_aos.st.answers_dropped = 6U;

    mesh_status_t st;
    (void)memset(&st, 0xA5, sizeof(st));
    TEST_ASSERT_TRUE(mesh_status(&st));
    NOTED(MESH_WHY_NONE);
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_TRUE(st.radio_ready);
    TEST_ASSERT_EQUAL(MESH_BAND_2_4_GHZ, st.band);
    TEST_ASSERT_EQUAL_UINT8(7U, st.spreading_factor);
    TEST_ASSERT_EQUAL_UINT32(2403000000U, st.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(812000U, st.bandwidth_hz);
    TEST_ASSERT_EQUAL_UINT8(5U, st.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(12U, st.preamble);
    TEST_ASSERT_EQUAL_UINT8(0x12U, st.sync_word);
    TEST_ASSERT_EQUAL_INT32(100, st.power_dbm_x10);          /* hundredths to tenths */
    TEST_ASSERT_EQUAL_INT32(95, st.power_applied_dbm_x10);
    TEST_ASSERT_TRUE(st.sending && st.receiving && st.arriving && st.cad_running && st.testing && st.amplifier_fault);
    TEST_ASSERT_EQUAL_UINT32(61000U, st.duty_wait_ms);
    TEST_ASSERT_EQUAL_FLOAT(-97.5f, st.last_rssi_dbm);
    TEST_ASSERT_TRUE(st.rssi_valid);
    TEST_ASSERT_EQUAL_UINT8(2U, st.packets_waiting);
    TEST_ASSERT_EQUAL_UINT32(11U, st.packets_received);
    TEST_ASSERT_EQUAL_UINT32(3U, st.packets_dropped);
    TEST_ASSERT_EQUAL_UINT32(9U, st.packets_sent);
    TEST_ASSERT_EQUAL_UINT16(0x0102U, st.radio_errors);
    TEST_ASSERT_TRUE(st.gnss_on);
    TEST_ASSERT_TRUE(st.fix.valid);
    TEST_ASSERT_EQUAL_DOUBLE(39.47, st.fix.latitude_deg);
    TEST_ASSERT_EQUAL_DOUBLE(-0.37, st.fix.longitude_deg);
    TEST_ASSERT_EQUAL_UINT8(8U, st.fix.satellites_used);
    TEST_ASSERT_EQUAL_UINT32(700U, st.fix.age_ms);
    TEST_ASSERT_EQUAL_UINT32(55U, st.gnss_sentences);
    TEST_ASSERT_EQUAL_UINT32(9600U, st.gnss_baud);
    TEST_ASSERT_TRUE(st.gnss_talking && st.pps_high && st.i2c_taken);
    TEST_ASSERT_EQUAL(MESH_SUPPLY_ON, st.supply);
    TEST_ASSERT_EQUAL_UINT32(4U, st.events_dropped);
    TEST_ASSERT_EQUAL_UINT32(5U, st.news_dropped);
    TEST_ASSERT_EQUAL_UINT32(6U, st.answers_dropped);
}

TEST_CASE("status: false for NULL (bad argument), and before begin (not started)", "[standard][module]")
{
    mesh_status_t st;
    TEST_ASSERT_FALSE(mesh_status(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.running = false;
    TEST_ASSERT_FALSE(mesh_status(&st));
    NOTED(MESH_WHY_NOT_STARTED);
}

TEST_CASE("stop: true when the owner stops, false (not started) when it cannot", "[standard][module]")
{
    TEST_ASSERT_TRUE(mesh_stop());
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.stops);
    NOTED(MESH_WHY_NONE);
    g_aos.stop_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(mesh_stop());
    NOTED(MESH_WHY_NOT_STARTED);
}

TEST_CASE("antennas_confirm: true with the word handed to the owner, false (not started) when it cannot", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_antennas_confirm(true));
    TEST_ASSERT_EQUAL_UINT(1U, g_aos.antennas);
    TEST_ASSERT_TRUE(g_aos.antennas_fitted);
    NOTED(MESH_WHY_NONE);
    TEST_ASSERT_TRUE(mesh_radio_antennas_confirm(false));
    TEST_ASSERT_FALSE(g_aos.antennas_fitted);
    TEST_ASSERT_EQUAL_UINT(0U, g_aos.calls);   /* not a request: it never waits for the owner's queue */
    g_aos.antennas_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(mesh_radio_antennas_confirm(true));
    NOTED(MESH_WHY_NOT_STARTED);
}

TEST_CASE("header_position: the owner's answer, as it is", "[standard][module]")
{
    g_aos.pos_ret = 7U;
    TEST_ASSERT_EQUAL_UINT8(7U, mesh_header_position(7U));
    g_aos.pos_ret = 0U;
    TEST_ASSERT_EQUAL_UINT8(0U, mesh_header_position(7U));
}

/* ---- Events ------------------------------------------------------------------------------- */

TEST_CASE("event_add_listener: true when added, false (bad argument) when the owner refuses", "[standard][events]")
{
    TEST_ASSERT_TRUE(mesh_event_add_listener("packet_received", on_event));
    TEST_ASSERT_EQUAL_STRING("packet_received", g_aos.last_event);
    TEST_ASSERT_EQUAL_PTR(on_event, g_aos.last_fn);
    NOTED(MESH_WHY_NONE);
    g_aos.add_ok = false;
    TEST_ASSERT_FALSE(mesh_event_add_listener("nonsense", on_event));
    NOTED(MESH_WHY_BAD_ARGUMENT);
}

TEST_CASE("event_remove_listener: true when removed, false (bad argument) when the owner refuses", "[standard][events]")
{
    TEST_ASSERT_TRUE(mesh_event_remove_listener("packet_received", on_event));
    NOTED(MESH_WHY_NONE);
    g_aos.remove_ok = false;
    TEST_ASSERT_FALSE(mesh_event_remove_listener("packet_received", on_event));
    NOTED(MESH_WHY_BAD_ARGUMENT);
}

TEST_CASE("event_wait: true with the event, the time and the name go down", "[standard][events]")
{
    mesh_event_t ev;
    g_aos.event.name = "module_found";
    g_aos.event.value = 1;
    TEST_ASSERT_TRUE(mesh_event_wait("module_found", 500U, &ev));
    TEST_ASSERT_EQUAL_UINT32(500U, g_aos.last_event_ms);
    TEST_ASSERT_EQUAL_STRING("module_found", ev.name);
    TEST_ASSERT_EQUAL_INT32(1, ev.value);
    NOTED(MESH_WHY_NONE);
}

TEST_CASE("event_wait: false (timed out) for a known name that did not come", "[standard][events]")
{
    mesh_event_t ev;
    g_aos.wait_ok = false;
    TEST_ASSERT_FALSE(mesh_event_wait("packet_received", 10U, &ev));
    NOTED(MESH_WHY_TIMED_OUT);
}

TEST_CASE("event_wait: false (bad argument) for NULL, no time, or a name that is no event", "[standard][events]")
{
    mesh_event_t ev;
    g_aos.wait_ok = false;
    TEST_ASSERT_FALSE(mesh_event_wait(NULL, 10U, &ev));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_event_wait("packet_received", 0U, &ev));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.known = false;
    TEST_ASSERT_FALSE(mesh_event_wait("nonsense", 10U, &ev));
    NOTED(MESH_WHY_BAD_ARGUMENT);
}

/* ---- The radio ---------------------------------------------------------------------------- */

TEST_CASE("radio_begin: true maps band, spreading factor and power (tenths to hundredths) into one request", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_begin(MESH_BAND_2_4_GHZ, 7U, 45));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_BEGIN, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)MESH_BAND_2_4_GHZ, g_aos.last.a);
    TEST_ASSERT_EQUAL_UINT32(7U, g_aos.last.u);
    TEST_ASSERT_EQUAL_INT32(450, g_aos.last.i);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, g_aos.last_ms);
    TEST_ASSERT_TRUE(mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MAX));
    TEST_ASSERT_EQUAL_INT32(MESH_POWER_X10_MAX * 10, g_aos.last.i);
    TEST_ASSERT_TRUE(mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MIN));
}

TEST_CASE("radio_begin: false (bad argument) for a band that is none or a power outside the window, asking nothing", "[standard][radio]")
{
    TEST_ASSERT_FALSE(mesh_radio_begin(MESH_BAND_COUNT, 9U, 85));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MAX + 1));
    TEST_ASSERT_FALSE(mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, MESH_POWER_X10_MIN - 1));
    TEST_ASSERT_FALSE(mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, INT32_MAX));
    nothing_asked();
}

TEST_CASE("radio_config_default: true with the region's setting for each band; false for NULL or no band", "[standard][radio]")
{
    mesh_radio_config_t c;
    TEST_ASSERT_TRUE(mesh_radio_config_default(MESH_BAND_SUB_GHZ, &c));
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_SUB_GHZ_CENTRE_HZ, c.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_SUB_GHZ_BANDWIDTH_HZ, c.bandwidth_hz);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_SUB_GHZ_DBM_X10, c.power_dbm_x10);
    TEST_ASSERT_TRUE(mesh_radio_config_default(MESH_BAND_2_4_GHZ, &c));
    TEST_ASSERT_EQUAL_UINT32(MESH_REGION_2_4_GHZ_CENTRE_HZ, c.frequency_hz);
    TEST_ASSERT_EQUAL_INT32(MESH_REGION_2_4_GHZ_DBM_X10, c.power_dbm_x10);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_CODING_RATE, c.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(MESH_REGION_PREAMBLE, c.preamble);
    TEST_ASSERT_EQUAL_UINT8(MESH_REGION_SYNC_WORD, c.sync_word);
    TEST_ASSERT_FALSE(mesh_radio_config_default(MESH_BAND_SUB_GHZ, NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_config_default(MESH_BAND_COUNT, &c));
    nothing_asked();
}

TEST_CASE("radio_configure and radio_retune: true maps every field; false for NULL or a power outside the window", "[standard][radio]")
{
    mesh_radio_config_t c = config();
    c.frequency_hz = 868300000U;
    c.bandwidth_hz = 125000U;
    c.spreading_factor = 8U;
    c.coding_rate = 6U;
    c.preamble = 10U;
    c.sync_word = 0x34U;
    c.power_dbm_x10 = 120;

    TEST_ASSERT_TRUE(mesh_radio_configure(&c));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_CONFIGURE, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(868300000U, g_aos.last.config.frequency_hz);
    TEST_ASSERT_EQUAL_UINT32(125000U, g_aos.last.config.bandwidth_hz);
    TEST_ASSERT_EQUAL_UINT8(8U, g_aos.last.config.spreading_factor);
    TEST_ASSERT_EQUAL_UINT8(6U, g_aos.last.config.coding_rate);
    TEST_ASSERT_EQUAL_UINT16(10U, g_aos.last.config.preamble);
    TEST_ASSERT_EQUAL_UINT8(0x34U, g_aos.last.config.sync_word);
    TEST_ASSERT_EQUAL_INT32(1200, g_aos.last.config.power_cdbm);

    TEST_ASSERT_TRUE(mesh_radio_retune(&c));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_RETUNE, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(868300000U, g_aos.last.config.frequency_hz);

    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_configure(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_retune(NULL));
    c.power_dbm_x10 = MESH_POWER_X10_MAX + 1;
    TEST_ASSERT_FALSE(mesh_radio_configure(&c));
    TEST_ASSERT_FALSE(mesh_radio_retune(&c));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_calibrate: true maps its frequencies; false for NULL, none, or more than the chip holds", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_calibrate(FREQS, 3U));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_CALIBRATE, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(3U, g_aos.last.u);
    TEST_ASSERT_EQUAL_UINT32(868500000U, g_aos.last.hz[2]);
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_calibrate(NULL, 1U));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_calibrate(FREQS, 0U));
    TEST_ASSERT_FALSE(mesh_radio_calibrate(FREQS, MESH_CALIBRATIONS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_end and radio_standby: true, each its own request", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_end());
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_END, g_aos.last.kind);
    TEST_ASSERT_TRUE(mesh_radio_standby());
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_STANDBY, g_aos.last.kind);
}

TEST_CASE("radio_send: true hands the owner the pointer and length; false for NULL or no bytes", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_send(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_SEND, g_aos.last.kind);
    TEST_ASSERT_EQUAL_PTR(DATA, g_aos.last.data);
    TEST_ASSERT_EQUAL_UINT8(3U, g_aos.last.len);
    TEST_ASSERT_EQUAL_UINT16(0U, g_aos.last.preamble);
    TEST_ASSERT_TRUE(mesh_radio_send(DATA, 255U));
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_send(NULL, 3U));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_send(DATA, 0U));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_send_preamble: true carries the preamble; false for NULL, no bytes or no preamble", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_send_preamble(DATA, sizeof(DATA), 12U));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_SEND, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT16(12U, g_aos.last.preamble);
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_send_preamble(NULL, 3U, 12U));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_send_preamble(DATA, 0U, 12U));
    TEST_ASSERT_FALSE(mesh_radio_send_preamble(DATA, 3U, 0U));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_send_wait_ms: the duty budget's wait, asking nothing", "[standard][radio]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_radio_send_wait_ms());
    g_aos.st.duty_wait_ms = 54321U;
    TEST_ASSERT_EQUAL_UINT32(54321U, mesh_radio_send_wait_ms());
    nothing_asked();
}

TEST_CASE("radio_receive: true maps on and off to the request's flag", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_receive(true));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_RECEIVE, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, g_aos.last.u);
    TEST_ASSERT_TRUE(mesh_radio_receive(false));
    TEST_ASSERT_EQUAL_UINT32(0U, g_aos.last.u);
}

TEST_CASE("radio_read: true copies the packet; false for NULL, not started, or none waiting", "[standard][radio]")
{
    mesh_packet_t rx;
    (void)memset(&rx, 0, sizeof(rx));

    g_aos.rx.length = 3U;
    g_aos.rx.data[0] = 9U;
    g_aos.rx.data[2] = 7U;
    g_aos.rx.crc_valid = true;
    g_aos.rx.rssi_dbm = -80.0f;
    g_aos.rx.snr_db = 6.5f;
    TEST_ASSERT_TRUE(mesh_radio_read(&rx));
    NOTED(MESH_WHY_NONE);
    TEST_ASSERT_EQUAL_UINT8(3U, rx.length);
    TEST_ASSERT_EQUAL_UINT8(9U, rx.data[0]);
    TEST_ASSERT_EQUAL_UINT8(7U, rx.data[2]);
    TEST_ASSERT_TRUE(rx.crc_valid);
    TEST_ASSERT_EQUAL_FLOAT(-80.0f, rx.rssi_dbm);
    TEST_ASSERT_EQUAL_FLOAT(6.5f, rx.snr_db);

    TEST_ASSERT_FALSE(mesh_radio_read(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.recv_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_FALSE(mesh_radio_read(&rx));
    NOTED(MESH_WHY_NO_PACKET);
    g_aos.running = false;
    TEST_ASSERT_FALSE(mesh_radio_read(&rx));
    NOTED(MESH_WHY_NOT_STARTED);
}

TEST_CASE("radio_rssi: true with a sample; false for NULL, not started, no module, receiver off or no sample yet", "[standard][radio]")
{
    float dbm = 0.0f;

    TEST_ASSERT_FALSE(mesh_radio_rssi(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.running = false;
    TEST_ASSERT_FALSE(mesh_radio_rssi(&dbm));
    NOTED(MESH_WHY_NOT_STARTED);
    g_aos.running = true;
    g_aos.st.present = false;
    TEST_ASSERT_FALSE(mesh_radio_rssi(&dbm));
    NOTED(MESH_WHY_MODULE_ABSENT);
    g_aos.st.present = true;
    TEST_ASSERT_FALSE(mesh_radio_rssi(&dbm));   /* receiver off */
    NOTED(MESH_WHY_NOT_RECEIVING);
    g_aos.st.receiving = true;
    TEST_ASSERT_FALSE(mesh_radio_rssi(&dbm));   /* on, and no sample yet */
    NOTED(MESH_WHY_NOT_RECEIVING);
    g_aos.st.rssi_valid = true;
    g_aos.st.last_rssi_dbm = -101.25f;
    TEST_ASSERT_TRUE(mesh_radio_rssi(&dbm));
    TEST_ASSERT_EQUAL_FLOAT(-101.25f, dbm);
    NOTED(MESH_WHY_NONE);
    nothing_asked();
}

TEST_CASE("radio_channel_busy: true says whether a packet arrives or a search runs; false for NULL, not started, no module, no setting", "[standard][radio]")
{
    bool busy = true;

    TEST_ASSERT_FALSE(mesh_radio_channel_busy(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.running = false;
    TEST_ASSERT_FALSE(mesh_radio_channel_busy(&busy));
    NOTED(MESH_WHY_NOT_STARTED);
    g_aos.running = true;
    g_aos.st.present = false;
    TEST_ASSERT_FALSE(mesh_radio_channel_busy(&busy));
    NOTED(MESH_WHY_MODULE_ABSENT);
    g_aos.st.present = true;
    TEST_ASSERT_FALSE(mesh_radio_channel_busy(&busy));   /* no setting */
    NOTED(MESH_WHY_NOT_BEGUN);
    TEST_ASSERT_TRUE(busy);                              /* untouched */
    g_aos.st.radio_ready = true;
    TEST_ASSERT_TRUE(mesh_radio_channel_busy(&busy));
    TEST_ASSERT_FALSE(busy);
    g_aos.st.arriving = true;
    TEST_ASSERT_TRUE(mesh_radio_channel_busy(&busy));
    TEST_ASSERT_TRUE(busy);
    g_aos.st.arriving = false;
    g_aos.st.cad_running = true;
    TEST_ASSERT_TRUE(mesh_radio_channel_busy(&busy));
    TEST_ASSERT_TRUE(busy);
    g_aos.st.cad_running = false;
    g_aos.st.sending = true;   /* its own send is not a busy channel */
    TEST_ASSERT_TRUE(mesh_radio_channel_busy(&busy));
    TEST_ASSERT_FALSE(busy);
    nothing_asked();
}

TEST_CASE("radio_cad: true maps symbols and speed and says what it saw; false for more symbols than the table has", "[standard][radio]")
{
    bool detected = false;

    g_aos.out1 = 1U;
    TEST_ASSERT_TRUE(mesh_radio_cad(3U, true, &detected));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_CAD, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(3U, g_aos.last.u);
    TEST_ASSERT_EQUAL_UINT8(1U, g_aos.last.a);
    TEST_ASSERT_TRUE(detected);
    g_aos.out1 = 0U;
    TEST_ASSERT_TRUE(mesh_radio_cad(0U, false, &detected));
    TEST_ASSERT_EQUAL_UINT8(0U, g_aos.last.a);
    TEST_ASSERT_FALSE(detected);
    TEST_ASSERT_TRUE(mesh_radio_cad(4U, false, NULL));   /* the answer is not asked for */
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_cad(5U, false, &detected));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_cad_receive: true asks a search that goes on to receive, and says what it saw; false for more symbols than the table has", "[standard][radio]")
{
    bool detected = false;

    g_aos.out1 = 1U;
    TEST_ASSERT_TRUE(mesh_radio_cad_receive(2U, false, &detected));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_CAD, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(2U, g_aos.last.u);
    TEST_ASSERT_EQUAL_UINT8(MESH_AOS_CAD_RECEIVE, g_aos.last.a);
    TEST_ASSERT_TRUE(detected);
    g_aos.out1 = 0U;
    TEST_ASSERT_TRUE(mesh_radio_cad_receive(1U, true, &detected));
    TEST_ASSERT_EQUAL_UINT8(MESH_AOS_CAD_RECEIVE | MESH_AOS_CAD_FAST, g_aos.last.a);
    TEST_ASSERT_FALSE(detected);
    TEST_ASSERT_TRUE(mesh_radio_cad_receive(4U, false, NULL));   /* the answer is not asked for */
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_cad_receive(5U, false, &detected));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_listen: true carries the milliseconds; false for none or more than a listen holds", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_listen(1U));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_LISTEN, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(1U, g_aos.last.u);
    TEST_ASSERT_TRUE(mesh_radio_listen(MESH_LISTEN_MS_MAX));
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_listen(0U));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_listen(MESH_LISTEN_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

TEST_CASE("radio_test: true maps the names to the chip's codes; false for no mode, no time, or too much", "[standard][radio]")
{
    TEST_ASSERT_TRUE(mesh_radio_test(MESH_TEST_SIGNAL_CARRIER, 250U));
    TEST_ASSERT_EQUAL(MESH_REQUEST_RADIO_TEST, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT32(2U, g_aos.last.u);
    TEST_ASSERT_EQUAL_INT32(250, g_aos.last.i);
    TEST_ASSERT_TRUE(mesh_radio_test(MESH_TEST_SIGNAL_PREAMBLE, MESH_BSP_TEST_MAX_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, g_aos.last.u);
    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_radio_test((mesh_test_signal_t)0, 100U));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_FALSE(mesh_radio_test((mesh_test_signal_t)9, 100U));
    TEST_ASSERT_FALSE(mesh_radio_test(MESH_TEST_SIGNAL_CARRIER, 0U));
    TEST_ASSERT_FALSE(mesh_radio_test(MESH_TEST_SIGNAL_CARRIER, MESH_BSP_TEST_MAX_MS + 1U));
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
}

/* ---- The GNSS and the I2C pair ------------------------------------------------------------- */

TEST_CASE("gnss_begin, gnss_end and gnss_reset: true, each its own request", "[standard][gnss]")
{
    TEST_ASSERT_TRUE(mesh_gnss_begin());
    TEST_ASSERT_EQUAL(MESH_REQUEST_GNSS_BEGIN, g_aos.last.kind);
    TEST_ASSERT_TRUE(mesh_gnss_end());
    TEST_ASSERT_EQUAL(MESH_REQUEST_GNSS_END, g_aos.last.kind);
    TEST_ASSERT_TRUE(mesh_gnss_reset());
    TEST_ASSERT_EQUAL(MESH_REQUEST_GNSS_RESET, g_aos.last.kind);
}

TEST_CASE("gnss_fix: true with a fix; false for NULL, not started, GNSS off, or no fix yet", "[standard][gnss]")
{
    mesh_gnss_fix_t fix;
    (void)memset(&fix, 0, sizeof(fix));

    TEST_ASSERT_FALSE(mesh_gnss_fix(NULL));
    NOTED(MESH_WHY_BAD_ARGUMENT);
    g_aos.running = false;
    TEST_ASSERT_FALSE(mesh_gnss_fix(&fix));
    NOTED(MESH_WHY_NOT_STARTED);
    g_aos.running = true;
    TEST_ASSERT_FALSE(mesh_gnss_fix(&fix));
    NOTED(MESH_WHY_GNSS_OFF);
    g_aos.st.gnss_on = true;
    TEST_ASSERT_FALSE(mesh_gnss_fix(&fix));
    NOTED(MESH_WHY_NO_FIX);
    TEST_ASSERT_FALSE(fix.valid);   /* left as it was */

    g_aos.st.fix.fix = true;
    g_aos.st.fix.lat = 39.4699;
    g_aos.st.fix.lon = -0.3763;
    g_aos.st.fix.alt_m = 15.5f;
    g_aos.st.fix.sats_used = 9U;
    g_aos.st.fix.hdop = 0.9f;
    g_aos.st.fix_age_ms = 800U;
    TEST_ASSERT_TRUE(mesh_gnss_fix(&fix));
    NOTED(MESH_WHY_NONE);
    TEST_ASSERT_TRUE(fix.valid);
    TEST_ASSERT_EQUAL_DOUBLE(39.4699, fix.latitude_deg);
    TEST_ASSERT_EQUAL_DOUBLE(-0.3763, fix.longitude_deg);
    TEST_ASSERT_EQUAL_FLOAT(15.5f, fix.altitude_m);
    TEST_ASSERT_EQUAL_UINT8(9U, fix.satellites_used);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, fix.horizontal_dilution);
    TEST_ASSERT_EQUAL_UINT32(800U, fix.age_ms);
    nothing_asked();
}

TEST_CASE("gnss_pps: the line, true and false, asking nothing", "[standard][gnss]")
{
    TEST_ASSERT_FALSE(mesh_gnss_pps());
    g_aos.st.pps_high = true;
    TEST_ASSERT_TRUE(mesh_gnss_pps());
    nothing_asked();
}

TEST_CASE("i2c_lend: true hands back the pair's GPIOs; false for both NULL or a refusal", "[standard][i2c]")
{
    uint8_t scl = 0U;
    uint8_t sda = 0U;

    g_aos.out1 = 31U;
    g_aos.out2 = 32U;
    TEST_ASSERT_TRUE(mesh_i2c_lend(&scl, &sda));
    TEST_ASSERT_EQUAL(MESH_REQUEST_I2C_LEND, g_aos.last.kind);
    TEST_ASSERT_EQUAL_UINT8(31U, scl);
    TEST_ASSERT_EQUAL_UINT8(32U, sda);
    scl = 0U;
    TEST_ASSERT_TRUE(mesh_i2c_lend(NULL, &sda));   /* one of them is enough */
    TEST_ASSERT_TRUE(mesh_i2c_lend(&scl, NULL));
    TEST_ASSERT_EQUAL_UINT8(31U, scl);

    unsigned const calls = g_aos.calls;
    TEST_ASSERT_FALSE(mesh_i2c_lend(NULL, NULL));   /* a take that tells nothing cannot be undone */
    NOTED(MESH_WHY_BAD_ARGUMENT);
    TEST_ASSERT_EQUAL_UINT(calls, g_aos.calls);
    g_aos.rc = KODE_DENIED;
    scl = 0U;
    TEST_ASSERT_FALSE(mesh_i2c_lend(&scl, &sda));
    TEST_ASSERT_EQUAL_UINT8(0U, scl);
}

TEST_CASE("i2c_return: true", "[standard][i2c]")
{
    TEST_ASSERT_TRUE(mesh_i2c_return());
    TEST_ASSERT_EQUAL(MESH_REQUEST_I2C_RETURN, g_aos.last.kind);
}

/* ---- The rules every call shares ----------------------------------------------------------- */

typedef bool (*call_t)(void);

static bool c_begin(void)       { return mesh_begin(); }
static bool c_end(void)         { return mesh_end(); }
static bool c_wait_present(void){ return mesh_wait_present(10U); }
static bool c_status(void)      { mesh_status_t s; return mesh_status(&s); }
static bool c_stop(void)        { return mesh_stop(); }
static bool c_add(void)         { return mesh_event_add_listener("module_found", on_event); }
static bool c_remove(void)      { return mesh_event_remove_listener("module_found", on_event); }
static bool c_wait(void)        { mesh_event_t e; return mesh_event_wait("module_found", 10U, &e); }
static bool c_antennas(void)    { return mesh_radio_antennas_confirm(true); }
static bool c_radio_begin(void) { return mesh_radio_begin(MESH_BAND_SUB_GHZ, 9U, 85); }
static bool c_configure(void)   { mesh_radio_config_t c = config(); return mesh_radio_configure(&c); }
static bool c_calibrate(void)   { return mesh_radio_calibrate(FREQS, 1U); }
static bool c_retune(void)      { mesh_radio_config_t c = config(); return mesh_radio_retune(&c); }
static bool c_radio_end(void)   { return mesh_radio_end(); }
static bool c_standby(void)     { return mesh_radio_standby(); }
static bool c_send(void)        { return mesh_radio_send(DATA, 3U); }
static bool c_send_pre(void)    { return mesh_radio_send_preamble(DATA, 3U, 8U); }
static bool c_receive(void)     { return mesh_radio_receive(true); }
static bool c_read(void)        { mesh_packet_t p; return mesh_radio_read(&p); }
static bool c_rssi(void)        { float f; return mesh_radio_rssi(&f); }
static bool c_busy(void)        { bool b; return mesh_radio_channel_busy(&b); }
static bool c_cad(void)         { bool d; return mesh_radio_cad(0U, false, &d); }
static bool c_cad_receive(void) { bool d; return mesh_radio_cad_receive(0U, false, &d); }
static bool c_listen(void)      { return mesh_radio_listen(100U); }
static bool c_test(void)        { return mesh_radio_test(MESH_TEST_SIGNAL_CARRIER, 100U); }
static bool c_gnss_begin(void)  { return mesh_gnss_begin(); }
static bool c_gnss_end(void)    { return mesh_gnss_end(); }
static bool c_gnss_reset(void)  { return mesh_gnss_reset(); }
static bool c_gnss_fix(void)    { mesh_gnss_fix_t f; return mesh_gnss_fix(&f); }
static bool c_i2c_lend(void)    { uint8_t a, b; return mesh_i2c_lend(&a, &b); }
static bool c_i2c_return(void)  { return mesh_i2c_return(); }

static const struct { const char *name; call_t call; bool asks; } BLOCKING[] = {
    { "mesh_begin", c_begin, false },
    { "mesh_end", c_end, false },
    { "mesh_wait_present", c_wait_present, false },
    { "mesh_status", c_status, false },
    { "mesh_stop", c_stop, false },
    { "mesh_event_add_listener", c_add, false },
    { "mesh_event_remove_listener", c_remove, false },
    { "mesh_event_wait", c_wait, false },
    { "mesh_radio_antennas_confirm", c_antennas, false },
    { "mesh_radio_begin", c_radio_begin, true },
    { "mesh_radio_configure", c_configure, true },
    { "mesh_radio_calibrate", c_calibrate, true },
    { "mesh_radio_retune", c_retune, true },
    { "mesh_radio_end", c_radio_end, true },
    { "mesh_radio_standby", c_standby, true },
    { "mesh_radio_send", c_send, true },
    { "mesh_radio_send_preamble", c_send_pre, true },
    { "mesh_radio_receive", c_receive, true },
    { "mesh_radio_read", c_read, false },
    { "mesh_radio_rssi", c_rssi, false },
    { "mesh_radio_channel_busy", c_busy, false },
    { "mesh_radio_cad", c_cad, true },
    { "mesh_radio_cad_receive", c_cad_receive, true },
    { "mesh_radio_listen", c_listen, true },
    { "mesh_radio_test", c_test, true },
    { "mesh_gnss_begin", c_gnss_begin, true },
    { "mesh_gnss_end", c_gnss_end, true },
    { "mesh_gnss_reset", c_gnss_reset, true },
    { "mesh_gnss_fix", c_gnss_fix, false },
    { "mesh_i2c_lend", c_i2c_lend, true },
    { "mesh_i2c_return", c_i2c_return, true },
};
#define BLOCKING_COUNT (sizeof(BLOCKING) / sizeof(BLOCKING[0]))

TEST_CASE("every call that blocks is refused from an Active Object, by name, before anything else", "[standard][ao]")
{
    for (unsigned i = 0U; i < BLOCKING_COUNT; i++) {
        fake_mesh_aos_reset();
        g_aos.from_ao = true;
        g_aos.st.gnss_on = true;
        g_aos.st.fix.fix = true;
        TEST_ASSERT_FALSE_MESSAGE(BLOCKING[i].call(), BLOCKING[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(g_aos.ao_fn, BLOCKING[i].name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(BLOCKING[i].name, g_aos.ao_fn, "the refusal names the call");
        TEST_ASSERT_EQUAL_STRING_MESSAGE("test_ao", g_aos.ao_name, BLOCKING[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.notes, BLOCKING[i].name);   /* it did not note a reason of its own on top */
        nothing_asked();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_aos.inits, BLOCKING[i].name);
    }
}

TEST_CASE("the calls that only read are not refused from an Active Object", "[standard][ao]")
{
    g_aos.from_ao = true;
    mesh_radio_config_t c;
    TEST_ASSERT_EQUAL_STRING("the owner's reason", mesh_error());
    TEST_ASSERT_TRUE(mesh_is_present());
    g_aos.pos_ret = 3U;
    TEST_ASSERT_EQUAL_UINT8(3U, mesh_header_position(3U));
    g_aos.st.duty_wait_ms = 99U;
    TEST_ASSERT_EQUAL_UINT32(99U, mesh_radio_send_wait_ms());
    g_aos.st.pps_high = true;
    TEST_ASSERT_TRUE(mesh_gnss_pps());
    TEST_ASSERT_TRUE(mesh_radio_config_default(MESH_BAND_SUB_GHZ, &c));
    TEST_ASSERT_NULL(g_aos.ao_fn);
}

TEST_CASE("a request the owner turns away is false, whatever the turn-away", "[standard][owner]")
{
    static const kode_status_t RCS[] = { KODE_DENIED, KODE_NO_RESOURCES, KODE_TIMEOUT, KODE_IO_ERROR };
    for (unsigned i = 0U; i < BLOCKING_COUNT; i++) {
        if (!BLOCKING[i].asks) {
            continue;
        }
        for (unsigned r = 0U; r < (sizeof(RCS) / sizeof(RCS[0])); r++) {
            fake_mesh_aos_reset();
            g_aos.rc = RCS[r];
            TEST_ASSERT_FALSE_MESSAGE(BLOCKING[i].call(), BLOCKING[i].name);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_aos.calls, BLOCKING[i].name);
        }
    }
}

TEST_CASE("a request the owner accepts is true", "[standard][owner]")
{
    for (unsigned i = 0U; i < BLOCKING_COUNT; i++) {
        if (!BLOCKING[i].asks) {
            continue;
        }
        fake_mesh_aos_reset();
        TEST_ASSERT_TRUE_MESSAGE(BLOCKING[i].call(), BLOCKING[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_aos.calls, BLOCKING[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2000U, g_aos.last_ms, "the wait for the owner's verdict");
    }
}
