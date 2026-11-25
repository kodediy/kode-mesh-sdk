/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_owner.h
 * @brief An owner that writes down what it is asked and answers when a case says so:
 *        the extended layer's calls, seen from below.
 *
 * mesh_sys.c is the real layer; this stands in for mesh_aos.c. A case sets what each
 * call of the owner should return, calls a mesh_sys function, reads back what the
 * owner was handed, and - for a request - answers it by hand, on the case's own task,
 * as the owner's task would: fake_owner_answer() calls the done function the layer gave.
 */
#ifndef FAKE_OWNER_H_
#define FAKE_OWNER_H_

#include <stdbool.h>
#include <stdint.h>

#include "mesh_aos.h"

#define FAKE_PENDING_MAX  64U

typedef struct {
    /* What the owner's calls return. */
    bool              kode_ready;   /* mesh_aos_kode_ready()                       */
    kode_status_t     start_rc;     /* mesh_aos_start()                            */
    kode_status_t     end_post_rc;  /* mesh_aos_end_post()                         */
    kode_status_t     post_rc;      /* mesh_aos_post()                             */
    kode_status_t     ask_rc;       /* mesh_aos_ask()                              */
    kode_status_t     stop_rc;      /* mesh_aos_stop()                             */
    kode_status_t     antennas_rc;  /* mesh_aos_antennas_confirm()                 */
    kode_status_t     packet_rc;    /* mesh_aos_packet_read()                      */
    mesh_aos_packet_t packet;       /* what it copies out                          */
    mesh_aos_status_t st;           /* mesh_aos_status()                           */
    uint8_t           pos_ret;      /* mesh_aos_header_position()                  */

    /* What it was asked. */
    unsigned          starts;
    unsigned          end_posts;
    unsigned          stops;
    unsigned          antennas;     /* mesh_aos_antennas_confirm()                 */
    bool              antennas_fitted; /* its last word                            */
    unsigned          posts;
    mesh_aos_request_t last_post;
    unsigned          asks;
    mesh_aos_request_t last_ask;
    unsigned          answers_dropped;   /* mesh_aos_answer_dropped()                   */

    /* The requests taken and not yet answered, in the order they came. */
    unsigned          pending;
    struct {
        void              *ask;
        mesh_aos_done_fn_t done;
        mesh_aos_request_t req;
        bool               answered;
    } p[FAKE_PENDING_MAX];

    /* The sink the layer connected. */
    mesh_aos_news_fn_t sink;
} fake_owner_t;

extern fake_owner_t g_owner;

/** @brief Nothing asked, everything answered OK, a Mesh present, the kode pools up. */
void fake_owner_reset(void);

/**
 * @brief Answer pending request @p index as the owner's task would: call its done function.
 * @return false when there is no such request or it was answered already.
 */
bool fake_owner_answer(unsigned index, kode_status_t rc, uint8_t why, uint32_t out1, uint32_t out2);

/** @brief Answer every request still pending with KODE_OK; how many. */
unsigned fake_owner_answer_all(void);

/** @brief The owner tells @p ev (a mesh_aos_event_t) with @p value to the sink; what the sink says. */
uint8_t fake_owner_tell(uint8_t ev, uint32_t value);

#endif /* FAKE_OWNER_H_ */
