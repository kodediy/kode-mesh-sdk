/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh.c
 * @brief The standard layer: mesh.h, the calls of a task.
 *
 * Every call here turns into one request to the owner (mesh_aos.c) and waits for its verdict.
 * What can be checked on the spot - a null pointer, a length, a band - is checked by the
 * owner's argument functions, the same ones the extended layer uses, so the two layers refuse the
 * same things; the state and the hardware are the owner's, which is the only task that touches the
 * module. Events are heard through the owner's events task (mesh_aos_events.c), never on the
 * owner's own.
 *
 * Every call that fails leaves its reason for the calling task (mesh_error()); a call from an Active
 * Object is refused first, with the name of the call and the object.
 */
#include "mesh.h"

#include <stddef.h>

#include "kode.h"

#include "mesh_aos.h"

/* The standard layer's limits are the owner's: the same numbers, so what is documented is what holds. */
_Static_assert(MESH_LISTENERS_MAX >= 1U, "listeners");
_Static_assert(MESH_EVENT_NAME_MAX >= 16U, "event names fit");

/** @brief How long a call that sets something up waits for the owner's verdict: a receiver
 *         calibration after mesh_radio_begin() is the longest, under a second. */
#define MESH_CALL_MS   2000U

/** @brief Refuse a call made from an Active Object, with the call's name and the object's. */
static bool enter(const char *fn)
{
    const char *ao = NULL;

    if (mesh_aos_caller_is_ao(&ao)) {
        mesh_aos_why_note_from_ao(fn, ao);
        return false;
    }
    return true;
}

/** @brief A bad argument, said. */
static bool bad_argument(void)
{
    mesh_aos_why_note(MESH_WHY_BAD_ARGUMENT);
    return false;
}

/** @brief The owner's verdict on a request, as a bool; the reason is left by the call. */
static bool verdict(const mesh_aos_request_t *req, uint32_t *out1, uint32_t *out2)
{
    return mesh_aos_call(req, MESH_CALL_MS, out1, out2) == KODE_OK;
}

/** @brief A request of a kind that carries nothing. */
static bool simple(mesh_request_kind_t kind)
{
    const mesh_aos_request_t req = { .kind = kind };
    return verdict(&req, NULL, NULL);
}

const char *mesh_error(void)
{
    return mesh_aos_error();
}

/* ---- Begin and end ------------------------------------------------------------ */

bool mesh_begin(void)
{
    if (!enter("mesh_begin")) {
        return false;
    }
    kode_init();   /* idempotent; waits for the first */
    return mesh_aos_start() == KODE_OK;
}

bool mesh_end(void)
{
    if (!enter("mesh_end")) {
        return false;
    }
    return mesh_aos_end() == KODE_OK;
}

/* ---- The module --------------------------------------------------------------- */

bool mesh_is_present(void)
{
    return mesh_aos_present();
}

bool mesh_wait_present(uint32_t ms)
{
    if (!enter("mesh_wait_present")) {
        return false;
    }
    kode_status_t const rc = mesh_aos_wait_present(ms);
    if (rc == KODE_OK) {
        mesh_aos_why_note(MESH_WHY_NONE);
        return true;
    }
    if (rc == KODE_TIMEOUT) {
        mesh_aos_why_note(MESH_WHY_MODULE_ABSENT);
    }
    return false;
}

/** @brief Note that the SDK has not begun, and say so. */
static bool began(void)
{
    if (!mesh_aos_running()) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return false;
    }
    return true;
}

bool mesh_status(mesh_status_t *out)
{
    if (!enter("mesh_status")) {
        return false;
    }
    if (out == NULL) {
        return bad_argument();
    }
    if (!began()) {
        return false;
    }
    mesh_aos_status_public(out);
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_stop(void)
{
    if (!enter("mesh_stop")) {
        return false;
    }
    if (mesh_aos_stop() != KODE_OK) {
        mesh_aos_why_note(MESH_WHY_NOT_STARTED);
        return false;
    }
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

uint8_t mesh_header_position(uint8_t pos)
{
    return mesh_aos_header_position(pos);
}

/* ---- Events ------------------------------------------------------------------- */

bool mesh_event_add_listener(const char *event, mesh_listener_t fn)
{
    if (!enter("mesh_event_add_listener")) {
        return false;
    }
    if (!mesh_aos_listener_add(event, fn)) {
        return bad_argument();
    }
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_event_remove_listener(const char *event, mesh_listener_t fn)
{
    if (!enter("mesh_event_remove_listener")) {
        return false;
    }
    if (!mesh_aos_listener_remove(event, fn)) {
        return bad_argument();
    }
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_event_wait(const char *event, uint32_t ms, mesh_event_t *out)
{
    if (!enter("mesh_event_wait")) {
        return false;
    }
    if (mesh_aos_listener_wait(event, ms, out)) {
        mesh_aos_why_note(MESH_WHY_NONE);
        return true;
    }
    /* An unknown name or a wait of no time is a bad argument; the rest is time running out. */
    mesh_aos_why_note(((event == NULL) || (ms == 0U) || !mesh_aos_event_known(event))
                      ? MESH_WHY_BAD_ARGUMENT : MESH_WHY_TIMED_OUT);
    return false;
}

/* ---- The radio ---------------------------------------------------------------- */

bool mesh_radio_antennas_confirm(bool fitted)
{
    if (!enter("mesh_radio_antennas_confirm")) {
        return false;
    }
    return mesh_aos_antennas_confirm(fitted) == KODE_OK;
}

bool mesh_radio_begin(mesh_band_t band, uint8_t spreading_factor, int32_t power_dbm_x10)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_begin")) {
        return false;
    }
    if (!mesh_aos_arg_radio_begin(&req, band, spreading_factor, power_dbm_x10)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_config_default(mesh_band_t band, mesh_radio_config_t *out)
{
    if (!mesh_aos_arg_radio_config_default(band, out)) {
        return bad_argument();
    }
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_radio_configure(const mesh_radio_config_t *config)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_configure")) {
        return false;
    }
    if (!mesh_aos_arg_radio_config(&req, MESH_REQUEST_RADIO_CONFIGURE, config)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_calibrate(const uint32_t *frequencies_hz, uint8_t count)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_calibrate")) {
        return false;
    }
    if (!mesh_aos_arg_radio_calibrate(&req, frequencies_hz, count)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_retune(const mesh_radio_config_t *config)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_retune")) {
        return false;
    }
    if (!mesh_aos_arg_radio_config(&req, MESH_REQUEST_RADIO_RETUNE, config)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_end(void)
{
    return enter("mesh_radio_end") && simple(MESH_REQUEST_RADIO_END);
}

bool mesh_radio_standby(void)
{
    return enter("mesh_radio_standby") && simple(MESH_REQUEST_RADIO_STANDBY);
}

bool mesh_radio_send(const uint8_t *data, uint8_t length)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_send")) {
        return false;
    }
    if (!mesh_aos_arg_radio_send(&req, data, length, 0U)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_send_preamble(const uint8_t *data, uint8_t length, uint16_t preamble)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_send_preamble")) {
        return false;
    }
    if ((preamble == 0U) || !mesh_aos_arg_radio_send(&req, data, length, preamble)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

uint32_t mesh_radio_send_wait_ms(void)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    return st.duty_wait_ms;
}

bool mesh_radio_receive(bool on)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_RECEIVE, .u = on ? 1U : 0U };

    return enter("mesh_radio_receive") && verdict(&req, NULL, NULL);
}

bool mesh_radio_read(mesh_packet_t *rx)
{
    if (!enter("mesh_radio_read")) {
        return false;
    }
    if (rx == NULL) {
        return bad_argument();
    }
    if (!began()) {
        return false;
    }
    kode_status_t const rc = mesh_aos_packet_public(rx);
    mesh_aos_why_note((rc == KODE_OK) ? MESH_WHY_NONE : MESH_WHY_NO_PACKET);
    return rc == KODE_OK;
}

bool mesh_radio_rssi(float *dbm)
{
    mesh_aos_status_t st;

    if (!enter("mesh_radio_rssi")) {
        return false;
    }
    if (dbm == NULL) {
        return bad_argument();
    }
    if (!began()) {
        return false;
    }
    mesh_aos_status(&st);
    if (!st.present) {
        mesh_aos_why_note(MESH_WHY_MODULE_ABSENT);
        return false;
    }
    if (!st.receiving || !st.rssi_valid) {
        mesh_aos_why_note(MESH_WHY_NOT_RECEIVING);
        return false;
    }
    *dbm = st.last_rssi_dbm;
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_radio_channel_busy(bool *busy)
{
    mesh_aos_status_t st;

    if (!enter("mesh_radio_channel_busy")) {
        return false;
    }
    if (busy == NULL) {
        return bad_argument();
    }
    if (!began()) {
        return false;
    }
    mesh_aos_status(&st);
    if (!st.present) {
        mesh_aos_why_note(MESH_WHY_MODULE_ABSENT);
        return false;
    }
    if (!st.radio_ready) {
        mesh_aos_why_note(MESH_WHY_NOT_BEGUN);
        return false;
    }
    *busy = st.arriving || st.cad_running;
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_radio_cad(uint8_t symbols, bool fast, bool *detected)
{
    mesh_aos_request_t req;
    uint32_t           seen = 0U;

    if (!enter("mesh_radio_cad")) {
        return false;
    }
    if (!mesh_aos_arg_radio_cad(&req, symbols, fast)) {
        return bad_argument();
    }
    if (!verdict(&req, &seen, NULL)) {
        return false;
    }
    if (detected != NULL) {
        *detected = (seen != 0U);
    }
    return true;
}

bool mesh_radio_cad_receive(uint8_t symbols, bool fast, bool *detected)
{
    mesh_aos_request_t req;
    uint32_t           seen = 0U;

    if (!enter("mesh_radio_cad_receive")) {
        return false;
    }
    if (!mesh_aos_arg_radio_cad_receive(&req, symbols, fast)) {
        return bad_argument();
    }
    if (!verdict(&req, &seen, NULL)) {
        return false;
    }
    if (detected != NULL) {
        *detected = (seen != 0U);
    }
    return true;
}

bool mesh_radio_listen(uint32_t ms)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_listen")) {
        return false;
    }
    if (!mesh_aos_arg_radio_listen(&req, ms)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

bool mesh_radio_test(mesh_test_signal_t mode, uint32_t ms)
{
    mesh_aos_request_t req;

    if (!enter("mesh_radio_test")) {
        return false;
    }
    if (!mesh_aos_arg_radio_test(&req, mode, ms)) {
        return bad_argument();
    }
    return verdict(&req, NULL, NULL);
}

/* ---- The GNSS ----------------------------------------------------------------- */

bool mesh_gnss_begin(void)
{
    return enter("mesh_gnss_begin") && simple(MESH_REQUEST_GNSS_BEGIN);
}

bool mesh_gnss_end(void)
{
    return enter("mesh_gnss_end") && simple(MESH_REQUEST_GNSS_END);
}

bool mesh_gnss_reset(void)
{
    return enter("mesh_gnss_reset") && simple(MESH_REQUEST_GNSS_RESET);
}

bool mesh_gnss_fix(mesh_gnss_fix_t *fix)
{
    mesh_aos_status_t st;

    if (!enter("mesh_gnss_fix")) {
        return false;
    }
    if (fix == NULL) {
        return bad_argument();
    }
    if (!began()) {
        return false;
    }
    mesh_aos_status(&st);
    if (!st.gnss_on) {
        mesh_aos_why_note(MESH_WHY_GNSS_OFF);
        return false;
    }
    mesh_gnss_fix_t got;
    mesh_aos_fix_public(&got);
    if (!got.valid) {
        mesh_aos_why_note(MESH_WHY_NO_FIX);
        return false;
    }
    *fix = got;
    mesh_aos_why_note(MESH_WHY_NONE);
    return true;
}

bool mesh_gnss_pps(void)
{
    mesh_aos_status_t st;

    mesh_aos_status(&st);
    return st.pps_high;
}

/* ---- The I2C pair ------------------------------------------------------------- */

bool mesh_i2c_lend(uint8_t *scl_gpio, uint8_t *sda_gpio)
{
    uint32_t scl = 0U;
    uint32_t sda = 0U;

    if (!enter("mesh_i2c_lend")) {
        return false;
    }
    if ((scl_gpio == NULL) && (sda_gpio == NULL)) {
        return bad_argument();   /* a take that tells nothing is a take nobody can undo */
    }

    const mesh_aos_request_t req = { .kind = MESH_REQUEST_I2C_LEND };
    if (!verdict(&req, &scl, &sda)) {
        return false;
    }
    if (scl_gpio != NULL) {
        *scl_gpio = (uint8_t)scl;
    }
    if (sda_gpio != NULL) {
        *sda_gpio = (uint8_t)sda;
    }
    return true;
}

bool mesh_i2c_return(void)
{
    return enter("mesh_i2c_return") && simple(MESH_REQUEST_I2C_RETURN);
}
