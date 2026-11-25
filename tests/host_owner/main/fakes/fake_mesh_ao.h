/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_ao.h
 * @brief What a case controls of the owner's Active Object, which this suite
 *        fakes at mesh_ao.h.
 *
 * tests/host_ao is where mesh_ao.c runs for real, on QP/C. Here the Active
 * Object is a queue and a task of its own: a post lands in the queue, and the
 * task hands it to mesh_aos_on_request(); the tick is mesh_aos_on_tick(),
 * every MESH_AO_TICK_MS after the last was served; the signals the system
 * publishes and the radio's interrupt are queued the same way. So the owner's
 * logic runs on one task, one step at a time, as it does on the board.
 */
#ifndef FAKE_MESH_AO_H_
#define FAKE_MESH_AO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile bool     tick_on;       /**< The tick is delivered; a case turns it off to see what a signal alone does. */
    volatile unsigned posts;         /**< Requests mesh_ao_post() took.                  */
    volatile unsigned refused;       /**< Requests it turned away.                       */
    volatile unsigned nudges;        /**< mesh_ao_nudge() calls.                         */
    volatile unsigned interrupts;    /**< mesh_ao_radio_interrupt_from_isr() calls.      */
    volatile unsigned ticks;         /**< mesh_aos_on_tick() calls.                      */
    volatile unsigned served;        /**< Queued items the owner has finished: a case waits on it to know a signal was taken. */
    volatile unsigned stall_ms;      /**< The next tick is held back this long, as an owner stalled by a slow call is; that one tick runs, then the tick is switched off so a case sees exactly what it did. */
    volatile bool     refuse_posts;  /**< mesh_ao_post() answers KODE_NO_RESOURCES, as it does with the queue full. */
    void * volatile   app_ao_task;   /**< A task (xTaskGetCurrentTaskHandle()) that mesh_ao_caller_is_ao() takes for
                                      *   an app's Active Object: a case sets its own to be one, NULL to stop. */
    volatile bool     kode_ready;    /**< What mesh_ao_kode_ready() answers: kode_init() has run. */
} fake_ao_t;

extern fake_ao_t g_ao;

/** @brief The tick on, nothing refused, counts at zero. The task stays as it is. */
void fake_ao_reset(void);

/** @brief The calling task is an app's Active Object from here until fake_ao_reset(). */
void fake_ao_become_app_ao(void);

/** @brief The radio's interrupt line rises: queue it as the driver's ISR does. */
void fake_ao_interrupt(void);

/** @brief AO_LowPower publishes a round for ASLEEP: the owner takes it in turn and rests. */
void fake_ao_publish_rest(void);

/** @brief AO_LowPower publishes the state AWAKE: the owner takes it in turn. */
void fake_ao_publish_awake(void);

/** @brief AO_Radio publishes EXP_POWER_SIG: a switch of J3's supply is over. */
void fake_ao_publish_power(bool on, uint8_t rc);

/** @brief The system publishes HOME_SIG: the owner takes it in turn. */
void fake_ao_publish_home(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_MESH_AO_H_ */
