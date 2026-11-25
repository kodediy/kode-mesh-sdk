/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_aos_args.c
 * @brief What both layers do alike before they ask the owner: check an argument, build the request,
 *        and turn the owner's numbers into the app's words.
 *
 * mesh.h (tasks) and mesh_sys.h (Active Objects) are two ways to ask for the same thing; they
 * must refuse the same arguments and read the same status. So the checks and the copies live
 * here once, in plain C - no FreeRTOS, no QP/C - and a layer that finds one false answers
 * MESH_WHY_BAD_ARGUMENT.
 */
#include "mesh_aos.h"

#include <stddef.h>
#include <string.h>

#include "mesh_bsp.h"
#include "mesh_plan.h"
#include "mesh_region.h"

_Static_assert((int)MESH_BAND_SUB_GHZ == (int)MESH_PLAN_BAND_SUB_GHZ, "band numbers");
_Static_assert((int)MESH_BAND_2_4_GHZ == (int)MESH_PLAN_BAND_2_4_GHZ, "band numbers");
_Static_assert((int)MESH_BAND_COUNT == (int)MESH_PLAN_BAND_COUNT, "band numbers");
_Static_assert(MESH_PAYLOAD_MAX == MESH_AOS_PAYLOAD_MAX, "payload cap");
_Static_assert((uint8_t)MESH_AOS_EVENT_MAX < 255U, "event numbers");

/** @brief The app's tenths of a dBm to the owner's hundredths; false outside the window. */
static bool power_cdbm(int32_t x10, int32_t *cdbm)
{
    if ((x10 < MESH_POWER_X10_MIN) || (x10 > MESH_POWER_X10_MAX)) {
        return false;
    }
    *cdbm = x10 * 10;
    return true;
}

/** @brief A setting in the owner's words; false when its power is outside the window. */
static bool config_copy(const mesh_radio_config_t *from, mesh_drv_lora_config_t *to)
{
    int32_t cdbm = 0;

    if (!power_cdbm(from->power_dbm_x10, &cdbm)) {
        return false;
    }
    to->frequency_hz     = from->frequency_hz;
    to->bandwidth_hz     = from->bandwidth_hz;
    to->spreading_factor = from->spreading_factor;
    to->coding_rate      = from->coding_rate;
    to->preamble         = from->preamble;
    to->sync_word        = from->sync_word;
    to->power_cdbm       = cdbm;
    return true;
}

bool mesh_aos_arg_radio_begin(mesh_aos_request_t *r, mesh_band_t band, uint8_t spreading_factor,
                              int32_t power_dbm_x10)
{
    int32_t cdbm = 0;

    if ((r == NULL) || ((unsigned)band >= (unsigned)MESH_BAND_COUNT) || !power_cdbm(power_dbm_x10, &cdbm)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = MESH_REQUEST_RADIO_BEGIN;
    r->a    = (uint8_t)band;
    r->u    = spreading_factor;
    r->i    = cdbm;
    return true;
}

bool mesh_aos_arg_radio_config(mesh_aos_request_t *r, mesh_request_kind_t kind,
                               const mesh_radio_config_t *config)
{
    if ((r == NULL) || (config == NULL)
        || ((kind != MESH_REQUEST_RADIO_CONFIGURE) && (kind != MESH_REQUEST_RADIO_RETUNE))) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = kind;
    return config_copy(config, &r->config);
}

bool mesh_aos_arg_radio_calibrate(mesh_aos_request_t *r, const uint32_t *frequencies_hz, uint8_t count)
{
    if ((r == NULL) || (frequencies_hz == NULL) || (count == 0U) || (count > MESH_AOS_CALIBRATIONS)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = MESH_REQUEST_RADIO_CALIBRATE;
    r->u    = count;
    for (uint8_t i = 0U; i < count; i++) {
        r->hz[i] = frequencies_hz[i];
    }
    return true;
}

bool mesh_aos_arg_radio_send(mesh_aos_request_t *r, const uint8_t *data, uint8_t length, uint16_t preamble)
{
    /* length is a uint8_t, so 1..MESH_PAYLOAD_MAX (255) is its whole positive range; the
     * owner's do_send() holds the authoritative bound. */
    if ((r == NULL) || (data == NULL) || (length == 0U)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind     = MESH_REQUEST_RADIO_SEND;
    r->data     = data;
    r->len      = length;
    r->preamble = preamble;
    return true;
}

bool mesh_aos_arg_radio_cad(mesh_aos_request_t *r, uint8_t symbols, bool fast)
{
    if ((r == NULL) || (symbols > 4U)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = MESH_REQUEST_RADIO_CAD;
    r->u    = symbols;
    r->a    = fast ? MESH_AOS_CAD_FAST : 0U;
    return true;
}

bool mesh_aos_arg_radio_cad_receive(mesh_aos_request_t *r, uint8_t symbols, bool fast)
{
    if (!mesh_aos_arg_radio_cad(r, symbols, fast)) {
        return false;
    }
    r->a = (uint8_t)(r->a | MESH_AOS_CAD_RECEIVE);
    return true;
}

bool mesh_aos_arg_radio_test(mesh_aos_request_t *r, mesh_test_signal_t mode, uint32_t ms)
{
    uint32_t u;

    /* The app's names to the chip's SetTxTestMode values: carrier is 2, preamble is 1
     * (mesh_drv_lr2021.h); the owner passes u straight through. A test signal always has a
     * time, and never a longer one than a measurement needs. */
    switch (mode) {
    case MESH_TEST_SIGNAL_CARRIER:  u = 2U; break;
    case MESH_TEST_SIGNAL_PREAMBLE: u = 1U; break;
    default:                        return false;
    }
    if ((r == NULL) || (ms == 0U) || (ms > MESH_BSP_TEST_MAX_MS)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = MESH_REQUEST_RADIO_TEST;
    r->u    = u;
    r->i    = (int32_t)ms;
    return true;
}

bool mesh_aos_arg_radio_listen(mesh_aos_request_t *r, uint32_t ms)
{
    if ((r == NULL) || (ms == 0U) || (ms > MESH_LISTEN_MS_MAX)) {
        return false;
    }
    (void)memset(r, 0, sizeof(*r));
    r->kind = MESH_REQUEST_RADIO_LISTEN;
    r->u    = ms;
    return true;
}

bool mesh_aos_arg_radio_config_default(mesh_band_t band, mesh_radio_config_t *out)
{
    if ((out == NULL) || ((unsigned)band >= (unsigned)MESH_BAND_COUNT)) {
        return false;
    }
    bool const low = (band == MESH_BAND_SUB_GHZ);

    out->frequency_hz     = low ? MESH_REGION_SUB_GHZ_CENTRE_HZ : MESH_REGION_2_4_GHZ_CENTRE_HZ;
    out->bandwidth_hz     = low ? MESH_REGION_SUB_GHZ_BANDWIDTH_HZ : MESH_REGION_2_4_GHZ_BANDWIDTH_HZ;
    out->spreading_factor = 9U;
    out->coding_rate      = MESH_REGION_CODING_RATE;
    out->preamble         = MESH_REGION_PREAMBLE;
    out->sync_word        = MESH_REGION_SYNC_WORD;
    out->power_dbm_x10    = low ? MESH_REGION_SUB_GHZ_DBM_X10 : MESH_REGION_2_4_GHZ_DBM_X10;
    return true;
}

/** @brief The driver's fix, and its age, in the app's words. */
static void fix_copy(const mesh_drv_nmea_fix_t *from, uint32_t age_ms, mesh_gnss_fix_t *to)
{
    to->fix_type            = from->fix_type;
    to->quality             = from->quality;
    to->position_dilution   = from->pdop;
    to->vertical_dilution   = from->vdop;
    to->course_deg          = from->course_deg;
    to->geoid_separation_m  = from->geoid_separation_m;
    to->age_ms              = age_ms;
    to->valid               = from->fix;
    to->latitude_deg        = from->lat;
    to->longitude_deg       = from->lon;
    to->altitude_m          = from->alt_m;
    to->speed_knots         = from->speed_kn;
    to->horizontal_dilution = from->hdop;
    to->satellites_used     = from->sats_used;
    to->satellites_in_view  = from->sats_in_view;
    to->time_utc            = from->utc;
    to->date                = from->date;
}

void mesh_aos_fix_public(mesh_gnss_fix_t *out)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    fix_copy(&st.fix, st.fix_age_ms, out);
}

void mesh_aos_status_public(mesh_status_t *out)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    out->present               = st.present;
    out->radio_ready           = st.radio_ready;
    out->radio_asleep          = st.radio_asleep;
    out->band                  = (mesh_band_t)st.band;
    out->spreading_factor      = st.spreading_factor;
    out->frequency_hz          = st.frequency_hz;
    out->bandwidth_hz          = st.bandwidth_hz;
    out->coding_rate           = st.coding_rate;
    out->preamble              = st.preamble;
    out->sync_word             = st.sync_word;
    out->power_dbm_x10         = st.power_cdbm / 10;
    out->power_applied_dbm_x10 = st.power_applied_cdbm / 10;
    out->sending               = st.sending;
    out->receiving             = st.receiving;
    out->arriving              = st.arriving;
    out->cad_running           = st.cad_running;
    out->testing               = st.testing;
    out->amplifier_fault       = st.amplifier_fault;
    out->antennas_confirmed    = st.antennas_confirmed;
    for (size_t band = 0U; band < (size_t)MESH_BAND_COUNT; band++) {
        out->antenna_suspected[band] = (st.antenna_suspected & (uint8_t)(1U << band)) != 0U;
    }
    out->power_capped          = st.power_capped;
    out->duty_wait_ms          = st.duty_wait_ms;
    out->last_rssi_dbm         = st.last_rssi_dbm;
    out->rssi_valid            = st.rssi_valid;
    out->packets_waiting       = st.packets_waiting;
    out->packets_received      = st.packets_received;
    out->packets_dropped       = st.packets_dropped;
    out->packets_sent          = st.packets_sent;
    out->radio_errors          = st.radio_errors;
    out->gnss_on               = st.gnss_on;
    fix_copy(&st.fix, st.fix_age_ms, &out->fix);
    out->gnss_sentences        = st.gnss_sentences;
    out->gnss_baud             = st.gnss_baud;
    out->gnss_talking          = st.gnss_talking;
    out->gnss_asleep           = st.gnss_asleep;
    out->pps_high              = st.pps_high;
    out->i2c_taken             = st.i2c_taken;
    out->seen                  = (mesh_module_t)st.seen;
    out->seen_level            = st.seen_level;
    out->supply                = (mesh_supply_t)st.supply;
    out->events_dropped        = st.events_dropped;
    out->news_dropped          = st.news_dropped;
    out->answers_dropped       = st.answers_dropped;
}

kode_status_t mesh_aos_packet_public(mesh_packet_t *rx)
{
    mesh_aos_packet_t got;

    kode_status_t const rc = mesh_aos_packet_read(&got);
    if (rc != KODE_OK) {
        return rc;
    }
    for (uint8_t i = 0U; i < got.length; i++) {
        rx->data[i] = got.data[i];
    }
    rx->length    = got.length;
    rx->crc_valid = got.crc_valid;
    rx->rssi_dbm  = got.rssi_dbm;
    rx->snr_db    = got.snr_db;
    return KODE_OK;
}
