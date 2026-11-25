/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_sys.h
 * @brief What the extended layer's case files share: the Active Objects they ask from, the pool
 *        they count, and the table of the fourteen requests.
 */
#ifndef TEST_SYS_H_
#define TEST_SYS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mesh_sys.h"

#define T_AOS          6U    /**< Active Objects the cases ask from (prio 1..6).          */
#define T_QUEUE_MAX    16U   /**< The depth of every one but the small one.               */
#define T_AO_SMALL     5U    /**< The one whose queue is too short for any margin.        */
#define T_SMALL_DEPTH  2U

#define T_REQUEST_COUNT 15U

typedef struct {
    const char     *name;
    kode_status_t (*ask)(QActive *me);          /**< A valid call of this request.          */
    QSignal         rsp_sig;                    /**< The answer's signal.                   */
    int             owner_kind;                 /**< The mesh_request_kind_t the owner sees. */
} t_request_t;

extern const t_request_t T_REQUESTS[T_REQUEST_COUNT];

QActive     *t_ao(unsigned i);
QEvt const  *t_pop(unsigned i);
unsigned     t_waiting(unsigned i);
unsigned     t_free_slots(unsigned i);
unsigned     t_drain(unsigned i);
void         t_fill(unsigned i, unsigned slots);
unsigned     t_pool_free(void);
void         t_pool_hog(unsigned left);
void         t_pool_release(void);

#endif /* TEST_SYS_H_ */
