/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_aos.h
 * @brief An owner that writes down what it is asked and answers what it is told to: the standard
 *        layer's checks, translations and refusals, seen from below.
 *
 * mesh.c is the real layer; this stands in for mesh_aos.c. A case sets what the owner should answer
 * (rc, out1, out2, a status, a packet, whether the caller is an Active Object), calls a mesh.h
 * function, and reads back the one request the owner was handed - its kind and its
 * a/u/i/data/len - to see that the call mapped right and guarded what it should before ever
 * reaching the owner.
 */
#ifndef FAKE_MESH_AOS_H_
#define FAKE_MESH_AOS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mesh_aos.h"

typedef struct {
    mesh_aos_status_t st;        /* what mesh_aos_status() hands back                          */
    kode_status_t     rc;        /* what mesh_aos_call() answers                               */
    uint32_t          out1;      /* mesh_aos_call()'s first answer                             */
    uint32_t          out2;      /* its second                                                 */
    mesh_aos_packet_t rx;        /* what mesh_aos_packet_read() copies out                     */
    kode_status_t     recv_rc;   /* what mesh_aos_packet_read() answers                        */
    bool              present;   /* mesh_aos_present()                                         */
    bool              running;   /* mesh_aos_running()                                         */
    kode_status_t     wait_rc;   /* mesh_aos_wait_present()                                    */
    kode_status_t     start_rc;  /* mesh_aos_start()                                           */
    kode_status_t     stop_rc;   /* mesh_aos_stop()                                            */
    kode_status_t     antennas_rc; /* mesh_aos_antennas_confirm()                              */
    kode_status_t     end_rc;    /* mesh_aos_end()                                             */
    uint8_t           pos_ret;   /* mesh_aos_header_position()                                 */
    bool              from_ao;   /* mesh_aos_caller_is_ao(): the caller is an Active Object    */
    bool              add_ok;    /* mesh_aos_listener_add()                                    */
    bool              remove_ok; /* mesh_aos_listener_remove()                                 */
    bool              wait_ok;   /* mesh_aos_listener_wait()                                   */
    bool              known;     /* mesh_aos_event_known()                                     */
    mesh_event_t      event;     /* what a successful wait hands back                          */

    /* What it was told and asked. */
    uint8_t           why;          /* the last mesh_aos_why_note()                            */
    unsigned          notes;        /* how many notes                                          */
    const char       *ao_fn;        /* mesh_aos_why_note_from_ao()'s call                      */
    const char       *ao_name;      /* and its object                                          */
    unsigned          calls;        /* mesh_aos_call()                                         */
    unsigned          starts;
    unsigned          stops;
    unsigned          antennas;     /* mesh_aos_antennas_confirm()                             */
    bool              antennas_fitted; /* its last word                                        */
    unsigned          ends;
    unsigned          inits;        /* kode_init()                                             */
    unsigned          waits;        /* mesh_aos_wait_present()                                 */
    uint32_t          last_ms;      /* a call's wait, or a wait's time                         */
    mesh_aos_request_t last;        /* the last request                                        */
    const char       *last_event;   /* the event a listener call named                         */
    mesh_listener_t   last_fn;      /* and the listener                                        */
    uint32_t          last_event_ms;
    unsigned          listener_calls;
} fake_mesh_aos_t;

extern fake_mesh_aos_t g_aos;

/** @brief Nothing asked, everything answered OK, a Mesh present and begun, a task calling. */
void fake_mesh_aos_reset(void);

#endif /* FAKE_MESH_AOS_H_ */
