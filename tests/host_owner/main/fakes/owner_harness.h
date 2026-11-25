/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file owner_harness.h
 * @brief What every case in the owner host suite starts from and asks with.
 *
 * ONE OWNER FOR THE WHOLE RUN. mesh_aos_start() makes a task that never ends,
 * and the owner keeps its state (the duty budget, the counters, the call
 * sequence) for as long as it holds the module, so the cases share it. harness_fresh()
 * is what makes them independent: it lets go of any gate, wipes the radio fake,
 * ends the radio, drains the ring and begins a clean 868 setting, so a case
 * starts from "present, radio ready, nothing on air" whatever the last one did.
 */
#ifndef OWNER_HARNESS_H_
#define OWNER_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "mesh_aos.h"
#include "mesh_plan.h"
#include "fake_mesh_ao.h"
#include "owner_fakes.h"

/** @brief Poll @p cond every tick for up to @p ms of the suite's clock (ticks, as the owner's waits);
 *         true if it came true. */
#define WAIT_FOR(cond, ms)                                                   \
    ({                                                                       \
        uint32_t const t0_ = kode_sys_time_ms();                                 \
        while (!(cond) && ((kode_sys_time_ms() - t0_) < (uint32_t)(ms))) {       \
            vTaskDelay(1);                                                   \
        }                                                                    \
        (bool)(cond);                                                        \
    })

/** @brief Start the owner once and wait until it holds the module; then a clean
 *         868 MHz, SF7 radio and nothing else going on. */
void harness_fresh(void);

/** @brief A snapshot of the owner's status. */
mesh_aos_status_t h_status(void);

/** @brief A call with no arguments beyond its kind. */
kode_status_t h_call(mesh_request_kind_t kind, uint32_t ms);

/** @brief MESH_REQUEST_RADIO_BEGIN. */
kode_status_t h_begin(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm);

/**
 * @brief MESH_REQUEST_RADIO_SEND of @p len bytes of @p data, as a task that waits would ask it.
 *
 * The call's answer is the packet gone, so this returns when the call is answered (a refusal,
 * or a timeout) or when the owner has put the packet on air: KODE_OK means "on air". Its answer
 * is h_send_answer() after the case has let the chip finish. One send at a time; a new h_send()
 * and harness_fresh() settle the last (a packet still on air is cut short).
 */
kode_status_t h_send(const uint8_t *data, uint8_t len, uint32_t ms);

/** @brief h_send() with a preamble of its own (0 = the setting's). */
kode_status_t h_send_preamble(const uint8_t *data, uint8_t len, uint16_t preamble, uint32_t ms);

/** @brief A send called on the case's own task, while h_send()'s is on air: the owner holds one
 *         packet at a time, so it is refused (the line is free while a send is held). */
kode_status_t h_send_again(const uint8_t *data, uint8_t len, uint32_t ms);

/** @brief The answer of the send h_send() put on air: waits up to @p ms; KODE_TIMEOUT when it has
 *         not come. @p out1 gets the airtime on success. */
kode_status_t h_send_answer(uint32_t ms, uint32_t *out1);

/** @brief Whether the send's call has been answered already (it can still be read by h_send_answer()). */
bool h_send_answered(void);

/** @brief Cut a packet still on air short and wait for its call to come back. */
void h_send_settle(void);

/** @brief MESH_REQUEST_RADIO_TEST with the chip's @p mode code and @p ms. */
kode_status_t h_test(uint32_t mode, int32_t ms);

/* ---- The Active Object's way: a request with a done function --------------------- */

/** @brief Where an answer given to mesh_aos_ask() lands: how many came, and the last. */
typedef struct {
    volatile unsigned   n;
    mesh_aos_answer_t   a;
    volatile bool       in_owner;   /**< The done function ran on the owner's task. */
} h_answer_t;

/** @brief mesh_aos_ask() with a done function that writes the answer into @p to. Returns what
 *         mesh_aos_ask() returned: KODE_OK means the answer will come (it may be a refusal). */
kode_status_t h_ask(const mesh_aos_request_t *req, h_answer_t *to);

/** @brief Wait up to @p ms for @p to's first answer; true if it came. */
bool h_answered(const h_answer_t *to, uint32_t ms);

/** @brief MESH_REQUEST_RADIO_RECEIVE on or off. */
kode_status_t h_receive(bool on);

/** @brief Take the module away: no fingerprint, no answer to a version read.
 *         Waits until the owner has given it up, then forgets the events. */
void h_module_away(void);

/** @brief What h_module_away() saw of MESH_AOS_EVENT_SETTING_LOST before it forgot the events:
 *         how many, and the last one's mask. */
extern unsigned g_away_lost_n;
extern uint32_t g_away_lost_arg;

/** @brief Put it back, as it was: the fingerprint and the answers. The owner
 *         finds it at its next look. */
void h_module_back(void);

/* ---- Events, as the app would hear them ------------------------------------- */
void     h_events_clear(void);
/** @brief Register the harness' callback again: the owner's count of slow ones starts over. */
void     h_events_register(void);
unsigned h_events(uint8_t ev);                     /**< how many of that kind    */
bool     h_event_with(uint8_t ev, uint32_t arg);   /**< one of that kind, that arg */
uint32_t h_event_arg(uint8_t ev);                  /**< the last one's arg       */

/** @brief What the app's callback does, besides being written down: run on the owner's
 *         task for every event. NULL for nothing; harness_fresh() takes it off. */
extern void (*volatile g_event_hook)(uint8_t ev, uint32_t arg);

/* ---- A second task, to be the caller that gives up or the one that waits ---- */
typedef struct {
    void           (*fn)(void *);
    void            *arg;
    volatile bool    done;
} h_bg_t;

void h_bg_start(h_bg_t *bg, void (*fn)(void *), void *arg);
bool h_bg_join(h_bg_t *bg, uint32_t ms);

#endif /* OWNER_HARNESS_H_ */
