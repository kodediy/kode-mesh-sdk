/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.h
 * @brief What the example asked of kode-sdk, for a case to read, and what
 *        kode-sdk answers, for a case to set: the AO started, its blocks and
 *        its queue, and the log.
 */
#ifndef FAKE_KODE_H_
#define FAKE_KODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_sys.h"

#define FAKE_KODE_BLOCKS 4U    /**< The pool the example's requests come from. */
#define FAKE_KODE_LINES  16U   /**< Log lines kept, the oldest dropped.        */
#define FAKE_KODE_LINE   200U  /**< Characters kept of each.                   */

typedef struct {
    /* Set by the case; fake_kode_reset() leaves every answer KODE_OK. */
    void (*initial)(void);             /**< QACTIVE_START() calls it, as QP/C takes
                                        *   the initial transition there; NULL: none. */
    size_t        blocks_held;         /**< The pool's blocks the rest of the app holds. */
    size_t        queue_held;          /**< The AO's queue slots the rest of the app holds. */

    /* Written down. */
    unsigned      starts;              /**< QACTIVE_START()s.                          */
    QActive      *ao;                  /**< ... the AO,                                */
    uint8_t       prio;                /**< ... its level,                             */
    size_t        queue_len;           /**< ... its queue,                             */
    size_t        stack_bytes;         /**< ... and its stack.                         */
    char const   *task_name;           /**< QActive_setAttr(TASK_NAME_ATTR).           */
    unsigned      infos;               /**< kode_log() lines, by level.                */
    unsigned      warnings;
    unsigned      errors;
} fake_kode_t;

extern fake_kode_t g_kode;

/** @brief Every record cleared, every answer KODE_OK, the pool and the queue empty. */
void fake_kode_reset(void);

/** @brief The oldest event posted and not yet taken; NULL when none. */
QEvt const *fake_kode_take(void);

/** @brief A block back to its pool, as QP/C does once an event is dispatched. */
void fake_kode_free(QEvt const *e);

/** @brief Blocks out of the pool: posted, taken or not yet freed. */
size_t fake_kode_blocks_out(void);

/** @brief Whether one of the last FAKE_KODE_LINES lines logged since
 *         fake_kode_reset() contains @p text. */
bool fake_kode_logged(const char *text);

/** @brief The line logged last; "" when none. */
const char *fake_kode_last_line(void);

#endif /* FAKE_KODE_H_ */
