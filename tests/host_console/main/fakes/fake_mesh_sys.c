/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_sys.c
 * @brief See fake_mesh_sys.h. The arguments are checked as mesh_sys.h documents
 *        them: a request names its AO, a band is one of the two, a payload is 1 to
 *        MESH_PAYLOAD_MAX bytes that are really there, a listen lasts 1 to 1000 ms,
 *        a test signal is one of the two modes for 1 to MESH_BSP_TEST_MAX_MS, and
 *        an out pointer is never NULL.
 */
#include "fake_mesh_sys.h"

#include <string.h>

fake_mesh_sys_t g_mesh;

void fake_mesh_sys_reset(void)
{
    (void)memset(&g_mesh, 0, sizeof(g_mesh));
    g_mesh.begin_answer     = KODE_OK;
    g_mesh.subscribe_answer = KODE_OK;
    g_mesh.answer           = KODE_OK;
}

/** @brief One call written down; KODE_DENIED when @p good is false. */
static kode_status_t called(mesh_call_t call, QActive *me, bool request, bool good)
{
    g_mesh.calls++;
    g_mesh.last.call = call;
    g_mesh.last.me   = me;
    if (request) {
        g_mesh.requests++;
        good = good && (me != NULL);
    }
    if (!good) {
        g_mesh.bad_args++;
        return KODE_DENIED;
    }
    return g_mesh.answer;
}

kode_status_t mesh_sys_begin(void)
{
    g_mesh.begins++;
    return g_mesh.begin_answer;
}

kode_status_t mesh_sys_subscribe(QActive *me)
{
    g_mesh.subscribes++;
    g_mesh.subscriber = me;
    return g_mesh.subscribe_answer;
}

kode_status_t mesh_sys_status_read(mesh_status_t *out)
{
    if (out == NULL) {
        return KODE_DENIED;
    }
    *out = g_mesh.st;
    return KODE_OK;
}

const char *mesh_sys_why_text(mesh_why_t why)
{
    return (why == MESH_WHY_NONE) ? "ok" : "refused";
}

kode_status_t mesh_sys_stop(void)
{
    g_mesh.stops++;
    return called(MESH_CALL_STOP, NULL, false, true);
}

kode_status_t mesh_sys_radio_antennas_confirm(bool fitted)
{
    /* Not a row of the table: the arming row gives it, and it is counted apart. */
    g_mesh.antennas++;
    g_mesh.antennas_fitted = fitted;
    return KODE_OK;
}

kode_status_t mesh_sys_radio_standby(void)
{
    return called(MESH_CALL_STANDBY, NULL, false, true);
}

kode_status_t mesh_sys_radio_receive(bool on)
{
    g_mesh.last.on = on;
    return called(MESH_CALL_RECEIVE, NULL, false, true);
}

kode_status_t mesh_sys_radio_listen(uint32_t ms)
{
    g_mesh.last.ms = ms;
    return called(MESH_CALL_LISTEN, NULL, false, (ms >= 1U) && (ms <= 1000U));
}

kode_status_t mesh_sys_radio_read(mesh_packet_t *rx)
{
    g_mesh.reads++;
    if ((rx == NULL) || (g_mesh.packets_ready == 0U)) {
        return KODE_NO_RESOURCES;
    }
    g_mesh.packets_ready--;
    *rx = g_mesh.packet;
    return KODE_OK;
}

kode_status_t mesh_sys_radio_rssi(float *dbm)
{
    kode_status_t const rc = called(MESH_CALL_RSSI, NULL, false, dbm != NULL);
    if (rc == KODE_OK) {
        *dbm = g_mesh.rssi_dbm;
    }
    return rc;
}

kode_status_t mesh_sys_radio_begin(QActive *me, mesh_band_t band, uint8_t spreading_factor,
                                   int32_t power_dbm_x10)
{
    g_mesh.last.band             = band;
    g_mesh.last.spreading_factor = spreading_factor;
    g_mesh.last.power_dbm_x10    = power_dbm_x10;
    return called(MESH_CALL_BEGIN, me, true, (unsigned)band < (unsigned)MESH_BAND_COUNT);
}

kode_status_t mesh_sys_radio_send(QActive *me, const uint8_t *data, uint8_t length)
{
    g_mesh.transmits++;
    g_mesh.last.length = length;
    /* A uint8_t never passes MESH_PAYLOAD_MAX (255): the length's type is the upper check. */
    _Static_assert(MESH_PAYLOAD_MAX == 255U, "the fake counts on a uint8_t length being the whole range");
    bool const good = (data != NULL) && (length >= 1U);
    if (good) {
        (void)memcpy(g_mesh.last.data, data, length);
        g_mesh.last.data[length] = 0U;
    }
    return called(MESH_CALL_SEND, me, true, good);
}

kode_status_t mesh_sys_radio_test(QActive *me, mesh_test_signal_t mode, uint32_t ms)
{
    g_mesh.transmits++;
    g_mesh.last.signal = mode;
    g_mesh.last.ms     = ms;
    bool const good = ((mode == MESH_TEST_SIGNAL_CARRIER) || (mode == MESH_TEST_SIGNAL_PREAMBLE))
                      && (ms >= 1U) && (ms <= MESH_BSP_TEST_MAX_MS);
    return called(MESH_CALL_TEST, me, true, good);
}

kode_status_t mesh_sys_gnss_begin(QActive *me)
{
    return called(MESH_CALL_GNSS_BEGIN, me, true, true);
}

kode_status_t mesh_sys_gnss_end(QActive *me)
{
    return called(MESH_CALL_GNSS_END, me, true, true);
}

kode_status_t mesh_sys_gnss_reset(QActive *me)
{
    return called(MESH_CALL_GNSS_RESET, me, true, true);
}
