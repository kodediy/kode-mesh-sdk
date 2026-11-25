/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief See fake_kode.h. QP/C's margins are kept as QP/C keeps them: a
 *        block or a slot is had while more than the margin are free.
 */
#include "fake_kode.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

fake_kode_t g_kode;

/** @brief A block, large enough for any event the example makes. */
typedef union {
    QEvt     evt;
    uint8_t  bytes[128];
    uint64_t align;
} block_t;

static block_t     s_block[FAKE_KODE_BLOCKS];
static bool        s_out[FAKE_KODE_BLOCKS];
static QEvt const *s_queue[KQP_AO_QUEUE_LEN];
static size_t      s_queued;
static char        s_line[FAKE_KODE_LINES][FAKE_KODE_LINE];
static size_t      s_lines;   /* logged since the reset; the last FAKE_KODE_LINES kept */

void fake_kode_reset(void)
{
    (void)memset(&g_kode, 0, sizeof(g_kode));
    (void)memset(s_out, 0, sizeof(s_out));
    s_queued = 0U;
    s_lines  = 0U;
}

size_t fake_kode_blocks_out(void)
{
    size_t n = 0U;
    for (size_t k = 0U; k < FAKE_KODE_BLOCKS; k++) {
        n += s_out[k] ? 1U : 0U;
    }
    return n;
}

void *fake_kode_new(size_t size, uint16_t margin, enum_t sig)
{
    size_t const used = fake_kode_blocks_out() + g_kode.blocks_held;
    size_t const free_blocks = (used < FAKE_KODE_BLOCKS) ? (FAKE_KODE_BLOCKS - used) : 0U;
    if ((size > sizeof(block_t)) || (free_blocks <= margin)) {
        return NULL;
    }
    for (size_t k = 0U; k < FAKE_KODE_BLOCKS; k++) {
        if (!s_out[k]) {
            s_out[k] = true;
            (void)memset(&s_block[k], 0, sizeof(s_block[k]));
            s_block[k].evt.sig = (QSignal)sig;
            return &s_block[k];
        }
    }
    return NULL;
}

void fake_kode_free(QEvt const *e)
{
    for (size_t k = 0U; k < FAKE_KODE_BLOCKS; k++) {
        if (e == &s_block[k].evt) {
            s_out[k] = false;
        }
    }
}

bool fake_kode_post(QActive * const me, QEvt const * const e, uint16_t margin,
                    void const * const sender)
{
    (void)sender;
    size_t const len  = (g_kode.queue_len < KQP_AO_QUEUE_LEN) ? g_kode.queue_len
                                                               : KQP_AO_QUEUE_LEN;
    size_t const used = s_queued + g_kode.queue_held;
    size_t const room = (used < len) ? (len - used) : 0U;
    if ((me != g_kode.ao) || (room <= margin)) {
        fake_kode_free(e);   /* QActive_post_() hands a refused event back */
        return false;
    }
    s_queue[s_queued] = e;
    s_queued++;
    return true;
}

QEvt const *fake_kode_take(void)
{
    if (s_queued == 0U) {
        return NULL;
    }
    QEvt const *const e = s_queue[0];
    (void)memmove(&s_queue[0], &s_queue[1], (s_queued - 1U) * sizeof(s_queue[0]));
    s_queued--;
    return e;
}

void fake_kode_start(QActive * const me, uint8_t prio, size_t queue_len, size_t stack_bytes)
{
    g_kode.starts++;
    g_kode.ao          = me;
    g_kode.prio        = prio;
    g_kode.queue_len   = queue_len;
    g_kode.stack_bytes = stack_bytes;
    if (g_kode.initial != NULL) {
        g_kode.initial();
    }
}

QState QHsm_top(void * const me, QEvt const * const e)
{
    (void)me;
    (void)e;
    return Q_HANDLED();
}

void QActive_ctor(QActive * const me, QStateHandler const initial)
{
    me->state = initial;
}

void QActive_setAttr(QActive * const me, uint32_t attr, void const * const value)
{
    (void)me;
    if (attr == TASK_NAME_ATTR) {
        g_kode.task_name = value;
    }
}

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    char *const line = s_line[s_lines % FAKE_KODE_LINES];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, FAKE_KODE_LINE, fmt, ap);
    va_end(ap);
    s_lines++;
    if (lvl >= KODE_LOG_ERROR) {
        g_kode.errors++;
    } else if (lvl >= KODE_LOG_WARN) {
        g_kode.warnings++;
    } else {
        g_kode.infos++;
    }
    (void)printf("[%s] %s\n", tag, line);
}

bool fake_kode_logged(const char *text)
{
    size_t const kept = (s_lines < FAKE_KODE_LINES) ? s_lines : FAKE_KODE_LINES;
    for (size_t k = 0U; k < kept; k++) {
        if (strstr(s_line[(s_lines - 1U - k) % FAKE_KODE_LINES], text) != NULL) {
            return true;
        }
    }
    return false;
}

const char *fake_kode_last_line(void)
{
    return (s_lines == 0U) ? "" : s_line[(s_lines - 1U) % FAKE_KODE_LINES];
}
