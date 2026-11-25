/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_owner.c
 * @brief The stand-in owner. See fake_owner.h.
 */
#include "fake_owner.h"

#include <string.h>

fake_owner_t g_owner;

void fake_owner_reset(void)
{
    (void)memset(&g_owner, 0, sizeof(g_owner));
    g_owner.kode_ready = true;
    g_owner.packet_rc  = KODE_OK;
    g_owner.st.present = true;
}

bool fake_owner_answer(unsigned index, kode_status_t rc, uint8_t why, uint32_t out1, uint32_t out2)
{
    if ((index >= g_owner.pending) || g_owner.p[index].answered) {
        return false;
    }
    g_owner.p[index].answered = true;
    mesh_aos_answer_t const a = { .rc = rc, .why = why, .out1 = out1, .out2 = out2 };
    g_owner.p[index].done(g_owner.p[index].ask, &a);
    return true;
}

unsigned fake_owner_answer_all(void)
{
    unsigned n = 0U;
    for (unsigned i = 0U; i < g_owner.pending; i++) {
        if (fake_owner_answer(i, KODE_OK, MESH_WHY_NONE, 0U, 0U)) {
            n++;
        }
    }
    return n;
}

uint8_t fake_owner_tell(uint8_t ev, uint32_t value)
{
    return (g_owner.sink != NULL) ? g_owner.sink(ev, value) : 0U;
}

/* ---- The owner's calls ---------------------------------------------------------- */

bool mesh_aos_kode_ready(void)
{
    return g_owner.kode_ready;
}

kode_status_t mesh_aos_start(void)
{
    g_owner.starts++;
    return g_owner.start_rc;
}

kode_status_t mesh_aos_end_post(void)
{
    g_owner.end_posts++;
    return g_owner.end_post_rc;
}

kode_status_t mesh_aos_stop(void)
{
    g_owner.stops++;
    return g_owner.stop_rc;
}

kode_status_t mesh_aos_antennas_confirm(bool fitted)
{
    g_owner.antennas++;
    g_owner.antennas_fitted = fitted;
    return g_owner.antennas_rc;
}

kode_status_t mesh_aos_post(const mesh_aos_request_t *req)
{
    g_owner.posts++;
    g_owner.last_post = *req;
    return g_owner.post_rc;
}

kode_status_t mesh_aos_ask(const mesh_aos_request_t *req, void *ask, mesh_aos_done_fn_t done)
{
    if ((req == NULL) || (done == NULL)) {
        return KODE_DENIED;
    }
    g_owner.asks++;
    g_owner.last_ask = *req;
    if (g_owner.ask_rc != KODE_OK) {
        return g_owner.ask_rc;
    }
    if (g_owner.pending < FAKE_PENDING_MAX) {
        g_owner.p[g_owner.pending].ask      = ask;
        g_owner.p[g_owner.pending].done     = done;
        g_owner.p[g_owner.pending].req      = *req;
        g_owner.p[g_owner.pending].answered = false;
        g_owner.pending++;
    }
    return KODE_OK;
}

void mesh_aos_news_connect(mesh_aos_news_fn_t fn)
{
    g_owner.sink = fn;
}

void mesh_aos_answer_dropped(void)
{
    g_owner.answers_dropped++;
}

bool mesh_aos_present(void)
{
    return g_owner.st.present;
}

void mesh_aos_status(mesh_aos_status_t *out)
{
    *out = g_owner.st;
}

uint8_t mesh_aos_header_position(uint8_t pos)
{
    (void)pos;
    return g_owner.pos_ret;
}

kode_status_t mesh_aos_packet_read(mesh_aos_packet_t *rx)
{
    if (g_owner.packet_rc == KODE_OK) {
        *rx = g_owner.packet;
    }
    return g_owner.packet_rc;
}

const char *mesh_aos_why_text(uint8_t why)
{
    return (why < MESH_WHY_COUNT) ? "a reason" : "unknown";
}
