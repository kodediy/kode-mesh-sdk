/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_sys.h
 * @brief mesh_sys.h as examples/console_actions reaches it: each call it makes
 *        written down, refused as the extended layer refuses an argument, and
 *        answered with what the case set.
 *
 * Commands and immediates answer at once from g_mesh. A request is only taken
 * and written down (g_mesh.last.me is the AO it was asked with): the case posts
 * the answer itself, as the owner would, to see what the AO does with it. The
 * two that transmit are counted again in g_mesh.transmits, so a case can say
 * that nothing went on air.
 */
#ifndef FAKE_MESH_SYS_H_
#define FAKE_MESH_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_sys.h"

/** @brief mesh_sys.h's calls the example's rows ask for. */
typedef enum {
    MESH_CALL_NONE = 0,
    MESH_CALL_BEGIN,
    MESH_CALL_RECEIVE,
    MESH_CALL_RSSI,
    MESH_CALL_LISTEN,
    MESH_CALL_SEND,
    MESH_CALL_TEST,
    MESH_CALL_STANDBY,
    MESH_CALL_STOP,
    MESH_CALL_GNSS_BEGIN,
    MESH_CALL_GNSS_END,
    MESH_CALL_GNSS_RESET
} mesh_call_t;

typedef struct {
    /* Set by the case; fake_mesh_sys_reset() leaves every answer KODE_OK. */
    mesh_status_t   st;            /**< What mesh_sys_status_read() says.             */
    kode_status_t   begin_answer;  /**< mesh_sys_begin()'s.                           */
    kode_status_t   subscribe_answer; /**< mesh_sys_subscribe()'s.                    */
    kode_status_t   answer;        /**< Every other call's, its arguments good.       */
    float           rssi_dbm;      /**< What mesh_sys_radio_rssi() reads.             */
    unsigned        packets_ready; /**< Packets mesh_sys_radio_read() hands out, one each. */
    mesh_packet_t   packet;        /**< ... each of them this one.                    */

    /* Written down. */
    unsigned        begins;        /**< mesh_sys_begin()s.                            */
    unsigned        subscribes;    /**< mesh_sys_subscribe()s,                        */
    QActive        *subscriber;    /**< and the AO of the last.                       */
    unsigned        reads;         /**< mesh_sys_radio_read()s, the ones that found none too. */
    unsigned        calls;         /**< Calls of the table above, refused ones too.   */
    unsigned        bad_args;      /**< ... refused for an argument mesh_sys.h refuses. */
    unsigned        requests;      /**< ... of them the requests (they take an AO).   */
    unsigned        transmits;     /**< ... of them mesh_sys_radio_send() and _test(). */
    unsigned        stops;         /**< mesh_sys_stop()s.                             */
    unsigned        antennas;      /**< mesh_sys_radio_antennas_confirm()s,           */
    bool            antennas_fitted; /**< and the word of the last.                   */
    struct {
        mesh_call_t        call;
        QActive           *me;
        mesh_band_t        band;
        uint8_t            spreading_factor;
        int32_t            power_dbm_x10;
        bool               on;
        uint32_t           ms;
        mesh_test_signal_t signal;
        size_t             length;
        uint8_t            data[MESH_PAYLOAD_MAX + 1U];   /**< The payload, then a NUL the mock adds. */
    } last;                        /**< The call made last.                           */
} fake_mesh_sys_t;

extern fake_mesh_sys_t g_mesh;

/** @brief No module, nothing called, every answer KODE_OK. */
void fake_mesh_sys_reset(void);

#endif /* FAKE_MESH_SYS_H_ */
