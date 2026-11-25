/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_ao.h
 * @brief AO_Mesh, the owner as an Active Object: what mesh_aos.c needs of it,
 *        and what its signals do, which mesh_aos.c provides.
 *
 * Private to mesh_aos. mesh_ao.c is the QP/C side - the queue, the tick, the
 * signals the system publishes - and calls into mesh_aos.c, which keeps the
 * module's state, for each one. tests/host_ao runs mesh_ao.c on kode-sdk's
 * real QP/C over a mesh_aos.c of its own.
 */
#ifndef MESH_AO_H_
#define MESH_AO_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "mesh_aos.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How often the owner reads the radio, the GNSS and the module, ms. */
#define MESH_AO_TICK_MS 20U

/* ---- mesh_ao.c ------------------------------------------------------------------ */

/**
 * @brief Whether kode_init() has brought up the event pools the owner posts from.
 *
 * The owner posts requests and takes news from kode-sdk's pools; before kode_init() they do
 * not exist and the first post would be QP/C's assertion.
 */
bool mesh_ao_kode_ready(void);

/**
 * @brief Construct the Active Object and start it. Call after kode_init(),
 *        once: mesh_aos_start() is the only caller.
 *
 * The initial transition subscribes to HOME_SIG and EXP_POWER_SIG (and, with
 * CONFIG_KODE_LOWPOWER, LOWPOWER_ROUND_SIG and LOWPOWER_STATE_SIG) and arms the tick, in the
 * caller's context, as QACTIVE_START() runs it. The level is checked first: one that another
 * Active Object holds, one that does not exist in this build (KODE_PRIO_USER_HIGH with
 * CONFIG_KODE_SCHEDULER) and level 3 (the renderer's) are refused with a message that names
 * what holds it, where QP/C would stop the board.
 * @return KODE_OK; KODE_NO_RESOURCES when a request fits no event pool; KODE_DENIED when the
 *         level is not the owner's to take.
 */
kode_status_t mesh_ao_start(void);

/**
 * @brief A copy of @p req to the owner, as an event from kode-sdk's pools.
 *
 * Never asserts: an event is taken only while the pools keep some blocks for
 * the rest of the system, and posted only while the queue keeps room for
 * what arrives without asking - the tick, the radio's interrupt, the supply's
 * answer, the board's rest and wake, HOME_SIG.
 * @return KODE_NO_RESOURCES when there was no block or no room.
 */
kode_status_t mesh_ao_post(const mesh_aos_request_t *req);

/** @brief Wake the owner to apply a mesh_aos_stop(); the next tick does it otherwise. */
void mesh_ao_nudge(void);

/**
 * @brief From the radio's interrupt: wake the owner to read the chip now
 *        rather than at the next tick. Interrupt context; a static event.
 *
 * The driver turns the line's interrupt off as it fires and on again only
 * once the owner has read the flags. So one is queued at a time, and two only
 * when a tick read the flags while one waited behind it - which the queue's
 * margin (AO_QUEUE_MARGIN in mesh_ao.c) leaves room for.
 */
void mesh_ao_radio_interrupt_from_isr(void);

/** @brief Whether the caller runs on the owner's own task. */
bool mesh_ao_is_own_task(void);

/**
 * @brief Whether the caller runs on the task of an Active Object - the owner's or the
 *        app's - by kqp_current_is_ao(). A call that waits for the owner must not be made
 *        there: it stops that Active Object's queue for as long as it waits. A kode_worker
 *        and an ordinary task are not Active Objects.
 * @param[out] name The Active Object's task name when true; may be NULL.
 */
bool mesh_ao_caller_is_ao(const char **name);

/* ---- mesh_aos.c: what the Active Object's signals do --------------------------------- */

/** @brief A request off the queue: served, published, answered if anyone waits. */
void mesh_aos_on_request(const mesh_aos_request_t *req);

/** @brief The time event: the radio, the GNSS and the module, MESH_AO_TICK_MS after the last. */
void mesh_aos_on_tick(void);

/** @brief The radio's interrupt line rose: read the chip's flags now. */
void mesh_aos_on_radio_interrupt(void);

/** @brief A mesh_aos_stop() not applied yet, applied now. */
void mesh_aos_on_stop(void);

/**
 * @brief The board is going to rest (a round for ASLEEP or OFF): the radio off air and held
 *        in reset, what the app had set up kept to put back. Safe to repeat.
 */
void mesh_aos_on_rest(void);

/**
 * @brief The board is awake (state AWAKE, which AO_LowPower repeats): what the rest took off
 *        the chip is put back as the app asked, once. Never emits, never moves by itself.
 */
void mesh_aos_on_awake(void);

/**
 * @brief The bottom button was held: the board goes back to kodeOS
 *        KODE_HOME_GRACE_MS from now. The radio off air and held in reset
 *        first.
 */
void mesh_aos_on_home(void);

/**
 * @brief EXP_POWER_SIG: a switch of J3's supply is over.
 * @param on The switch.
 * @param rc A kode_status_t: KODE_OK as asked, KODE_IO_ERROR refused (the wish is dropped),
 *           KODE_DENIED no link yet (the wish stays).
 */
void mesh_aos_on_power(bool on, uint8_t rc);

#ifdef __cplusplus
}
#endif

#endif /* MESH_AO_H_ */
