/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_aos.c
 * @brief The stand-in owner, and kode_init(). See fake_mesh_aos.h.
 */
#include "fake_mesh_aos.h"

#include <string.h>

#include "kode.h"

fake_mesh_aos_t g_aos;

void fake_mesh_aos_reset(void)
{
    (void)memset(&g_aos, 0, sizeof(g_aos));
    g_aos.rc         = KODE_OK;
    g_aos.recv_rc    = KODE_OK;
    g_aos.wait_rc    = KODE_OK;
    g_aos.start_rc   = KODE_OK;
    g_aos.stop_rc    = KODE_OK;
    g_aos.antennas_rc = KODE_OK;
    g_aos.end_rc     = KODE_OK;
    g_aos.present    = true;
    g_aos.running    = true;
    g_aos.add_ok     = true;
    g_aos.remove_ok  = true;
    g_aos.wait_ok    = true;
    g_aos.known      = true;
    g_aos.st.present = true;
    g_aos.why        = 0xFFU;   /* "no note yet": no reason is ever this */
}

/* ---- kode-sdk ---------------------------------------------------------------- */

void kode_init(void)
{
    g_aos.inits++;
}

/* ---- The module -------------------------------------------------------------- */

kode_status_t mesh_aos_start(void)
{
    g_aos.starts++;
    return g_aos.start_rc;
}

bool mesh_aos_running(void)
{
    return g_aos.running;
}

bool mesh_aos_present(void)
{
    return g_aos.present;
}

kode_status_t mesh_aos_wait_present(uint32_t ms)
{
    g_aos.waits++;
    g_aos.last_ms = ms;
    return g_aos.wait_rc;
}

void mesh_aos_status(mesh_aos_status_t *out)
{
    *out = g_aos.st;
}

kode_status_t mesh_aos_end(void)
{
    g_aos.ends++;
    return g_aos.end_rc;
}

kode_status_t mesh_aos_stop(void)
{
    g_aos.stops++;
    return g_aos.stop_rc;
}

kode_status_t mesh_aos_antennas_confirm(bool fitted)
{
    g_aos.antennas++;
    g_aos.antennas_fitted = fitted;
    /* The owner leaves its own reason, as the real one does. */
    mesh_aos_why_note((g_aos.antennas_rc == KODE_OK) ? MESH_WHY_NONE : MESH_WHY_NOT_STARTED);
    return g_aos.antennas_rc;
}

uint8_t mesh_aos_header_position(uint8_t pos)
{
    (void)pos;
    return g_aos.pos_ret;
}

/* ---- Requests ---------------------------------------------------------------- */

kode_status_t mesh_aos_call(const mesh_aos_request_t *req, uint32_t ms, uint32_t *out1, uint32_t *out2)
{
    g_aos.last    = *req;
    g_aos.last_ms = ms;
    g_aos.calls++;
    if (g_aos.rc == KODE_OK) {
        if (out1 != NULL) { *out1 = g_aos.out1; }
        if (out2 != NULL) { *out2 = g_aos.out2; }
    }
    return g_aos.rc;
}

kode_status_t mesh_aos_packet_read(mesh_aos_packet_t *rx)
{
    if (g_aos.recv_rc == KODE_OK) {
        *rx = g_aos.rx;
    }
    return g_aos.recv_rc;
}

/* ---- Reasons ----------------------------------------------------------------- */

void mesh_aos_why_note(uint8_t why)
{
    g_aos.why = why;
    g_aos.notes++;
}

void mesh_aos_why_note_from_ao(const char *fn, const char *ao)
{
    g_aos.ao_fn   = fn;
    g_aos.ao_name = ao;
}

const char *mesh_aos_error(void)
{
    return "the owner's reason";
}

bool mesh_aos_caller_is_ao(const char **name)
{
    if (g_aos.from_ao && (name != NULL)) {
        *name = "test_ao";
    }
    return g_aos.from_ao;
}

/* ---- Events ------------------------------------------------------------------ */

bool mesh_aos_listener_add(const char *name, mesh_listener_t fn)
{
    g_aos.listener_calls++;
    g_aos.last_event = name;
    g_aos.last_fn    = fn;
    return g_aos.add_ok;
}

bool mesh_aos_listener_remove(const char *name, mesh_listener_t fn)
{
    g_aos.listener_calls++;
    g_aos.last_event = name;
    g_aos.last_fn    = fn;
    return g_aos.remove_ok;
}

bool mesh_aos_listener_wait(const char *name, uint32_t ms, mesh_event_t *out)
{
    g_aos.listener_calls++;
    g_aos.last_event    = name;
    g_aos.last_event_ms = ms;
    if (g_aos.wait_ok && (out != NULL)) {
        *out = g_aos.event;
    }
    return g_aos.wait_ok;
}

bool mesh_aos_event_known(const char *name)
{
    return g_aos.known && (name != NULL);
}
