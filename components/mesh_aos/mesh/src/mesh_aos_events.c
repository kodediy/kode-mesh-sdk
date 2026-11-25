/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_aos_events.c
 * @brief What the owner tells the standard layer: named events, to listeners and waiters,
 *        on a task of their own. See mesh_aos.h.
 *
 * The owner hands an event to this file's queue without blocking and goes on; a task of the
 * SDK's (mesh_events) takes them one at a time, in order, and calls the listeners and
 * wakes the waiters. A listener therefore never runs on the owner's task, and one that
 * is slow costs the events behind it, not the radio. When the queue (16) is full the event
 * is lost and counted (mesh_status_t.events_dropped).
 *
 * Nothing is queued until someone asks to hear: the first listener or waiter makes the queue
 * and the task, so an app that uses only the Active Object layer pays nothing for them.
 */
#include "mesh_aos.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kode_log.h"

#if defined(__has_include)
#if __has_include("kqp_config.h")
#include "kqp_config.h"
#endif
#endif

#define TAG "mesh"

#define EVENTS_QUEUE_DEPTH  16U
#define EVENTS_STACK_BYTES  4096U
#define WAITERS_MAX         4U

/* The listeners run the app's code, which may take as long as it likes, so their task sits
 * where it can only ever delay itself: KODE_PRIO_STANDARD_EVENTS (kqp_config.h), the level
 * kode-sdk gives its own standard layer's listener task, 1, below every owner AO. */
#ifdef KODE_PRIO_STANDARD_EVENTS
#define EVENTS_PRIORITY     ((UBaseType_t)KODE_PRIO_STANDARD_EVENTS)
#else
#define EVENTS_PRIORITY     1U   /* the host suites' stand-in kode-sdk has no priority map */
#endif

typedef struct {
    uint8_t  ev;
    int32_t  value;
    uint32_t seq;      /* the order they were pushed in: a waiter counts only what comes after it began */
} ev_msg_t;

typedef struct {
    uint8_t             ev;
    mesh_listener_t fn;
} listener_t;

typedef struct {
    bool              used;
    bool              got;
    uint8_t           ev;
    uint32_t          seq0;
    SemaphoreHandle_t sem;
    mesh_event_t  out;
} waiter_t;

static const struct {
    uint8_t     ev;
    const char *name;
} NAMES[] = {
    { MESH_AOS_EVENT_PRESENT,          "module_found"      },
    { MESH_AOS_EVENT_GONE,             "module_lost"       },
    { MESH_AOS_EVENT_WRONG_MODULE,     "wrong_module"      },
    { MESH_AOS_EVENT_SETTING_LOST,     "setting_lost"      },
    { MESH_AOS_EVENT_SETTING_RESTORED, "setting_restored"  },
    { MESH_AOS_EVENT_PACKET,           "packet_received"   },
    { MESH_AOS_EVENT_LISTEN_END,       "listen_ended"      },
    { MESH_AOS_EVENT_RADIO_ERROR,      "radio_failed"      },
    { MESH_AOS_EVENT_TEST_END,         "test_ended"        },
    { MESH_AOS_EVENT_PREAMBLE,         "preamble_detected" },
    { MESH_AOS_EVENT_HEADER,           "header_received"   },
    { MESH_AOS_EVENT_HEADER_BAD,       "header_failed"     },
};

static portMUX_TYPE   s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t  s_queue;
static TaskHandle_t   s_task;
static bool           s_wanted;                       /* someone asked to hear: the queue exists */
static uint32_t       s_dropped;                      /* under s_lock */
static uint32_t       s_push_seq;                     /* under s_lock */
static listener_t     s_listeners[MESH_LISTENERS_MAX];   /* under s_lock */
static waiter_t       s_waiters[WAITERS_MAX];         /* under s_lock */

const char *mesh_aos_event_name(uint8_t ev)
{
    for (size_t i = 0U; i < (sizeof(NAMES) / sizeof(NAMES[0])); i++) {
        if (NAMES[i].ev == ev) {
            return NAMES[i].name;
        }
    }
    return NULL;
}

static int event_of(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0U; i < (sizeof(NAMES) / sizeof(NAMES[0])); i++) {
        if (strncmp(NAMES[i].name, name, MESH_EVENT_NAME_MAX) == 0) {
            return (int)NAMES[i].ev;
        }
    }
    return -1;
}

bool mesh_aos_event_known(const char *name)
{
    return event_of(name) >= 0;
}

/** @brief The words that go with an event's value: a constant, "" when there are none. */
static const char *event_text(uint8_t ev, int32_t value)
{
    static const char *const SETTINGS[8] = {
        "", "radio", "receiver", "radio and receiver", "gnss", "radio and gnss", "receiver and gnss",
        "radio, receiver and gnss",
    };
    uint32_t const v = (uint32_t)value;

    switch (ev) {
    case MESH_AOS_EVENT_PRESENT:
        return (value == 1) ? "upright" : "";
    case MESH_AOS_EVENT_WRONG_MODULE:
        return (v == (uint32_t)MESH_MODULE_OTHER) ? "something that is not a Mesh is on J3" : "";
    case MESH_AOS_EVENT_SETTING_LOST:
    case MESH_AOS_EVENT_SETTING_RESTORED:
        return SETTINGS[v & 7UL];
    case MESH_AOS_EVENT_TEST_END:
        return (v == MESH_TEST_END_CHIP) ? "ended by the chip's timer" : "ended by the SDK's deadline";
    case MESH_AOS_EVENT_RADIO_ERROR:
        switch (v & ~MESH_RADIO_ERROR_CHIP_MASK) {
        case MESH_RADIO_ERROR_NO_STANDBY:      return "the chip would not confirm standby and was reset";
        case MESH_RADIO_ERROR_SEND_TIMEOUT:    return "a packet's timer ran out without a done flag";
        case MESH_RADIO_ERROR_AMPLIFIER_FAULT: return "the amplifier faulted: the radio is off";
        case MESH_RADIO_ERROR_RECEIVER:        return "the receiver would not come back on";
        case MESH_RADIO_ERROR_FLAGS_LOST:      return "the chip's flags were lost on air: the radio is off";
        default:                               return "the chip reported an error";
        }
    default:
        return "";
    }
}

static void deliver(const ev_msg_t *m)
{
    mesh_event_t heard = { .name = mesh_aos_event_name(m->ev), .value = m->value,
                               .text = event_text(m->ev, m->value) };
    mesh_listener_t calls[MESH_LISTENERS_MAX];
    SemaphoreHandle_t wake[WAITERS_MAX];
    uint8_t n = 0U;
    uint8_t n_wake = 0U;

    if (heard.name == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < WAITERS_MAX; i++) {
        waiter_t * const w = &s_waiters[i];
        if (w->used && !w->got && (w->ev == m->ev) && ((int32_t)(m->seq - w->seq0) > 0)) {
            w->out = heard;
            w->got = true;
            wake[n_wake++] = w->sem;
        }
    }
    for (uint8_t i = 0U; i < MESH_LISTENERS_MAX; i++) {
        if ((s_listeners[i].fn != NULL) && (s_listeners[i].ev == m->ev)) {
            calls[n++] = s_listeners[i].fn;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    /* The waiters are woken outside the lock: a call that can switch tasks has no place in a
     * critical section. What a waiter trusts is its record (got, out), written under the lock;
     * the semaphore only wakes it, and a give that comes late is told apart by the record. */
    for (uint8_t i = 0U; i < n_wake; i++) {
        (void)xSemaphoreGive(wake[i]);
    }
    for (uint8_t i = 0U; i < n; i++) {
        calls[i](&heard);
    }
}

static void events_task(void *arg)
{
    (void)arg;
    for (;;) {
        ev_msg_t m;
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) == pdTRUE) {
            deliver(&m);
        }
    }
}

/** @brief The queue and the task, made the first time anyone asks to hear. */
static bool events_ensure(void)
{
    if (s_wanted) {
        return true;
    }
    taskENTER_CRITICAL(&s_lock);
    bool const have = (s_queue != NULL);
    taskEXIT_CRITICAL(&s_lock);
    if (have) {
        for (uint8_t i = 0U; !s_wanted && (i < 100U); i++) {
            vTaskDelay(1);   /* another caller is making the task: the few microseconds it takes */
        }
        return s_wanted;
    }
    QueueHandle_t q = xQueueCreate(EVENTS_QUEUE_DEPTH, sizeof(ev_msg_t));
    if (q == NULL) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    bool const mine = (s_queue == NULL);
    if (mine) {
        s_queue = q;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (mine) {
        for (uint8_t i = 0U; i < WAITERS_MAX; i++) {
            if (s_waiters[i].sem == NULL) {
                s_waiters[i].sem = xSemaphoreCreateBinary();
                if (s_waiters[i].sem == NULL) {
                    /* No memory for it: the slot is never handed out (mesh_aos_listener_wait()). */
                    KLOG_W(TAG, "no semaphore for waiter %u: one wait less at a time", (unsigned)i);
                }
            }
        }
    }
    if (!mine) {
        vQueueDelete(q);   /* another caller made it first */
    } else if (xTaskCreate(events_task, "mesh_events", EVENTS_STACK_BYTES, NULL, EVENTS_PRIORITY, &s_task)
               != pdPASS) {
        KLOG_E(TAG, "the events task could not be made");
        taskENTER_CRITICAL(&s_lock);
        s_queue = NULL;
        taskEXIT_CRITICAL(&s_lock);
        vQueueDelete(q);
        return false;
    } else {
        /* made */
    }
    s_wanted = true;
    return true;
}

bool mesh_aos_events_push(uint8_t ev, int32_t value)
{
    if (!s_wanted) {
        return true;   /* nobody asked to hear */
    }
    ev_msg_t m = { .ev = ev, .value = value };
    taskENTER_CRITICAL(&s_lock);
    m.seq = ++s_push_seq;
    taskEXIT_CRITICAL(&s_lock);
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_dropped++;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

uint32_t mesh_aos_events_dropped(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t const n = s_dropped;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

bool mesh_aos_listener_add(const char *name, mesh_listener_t fn)
{
    int const ev = event_of(name);

    if ((ev < 0) || (fn == NULL) || !events_ensure()) {
        return false;
    }
    bool added = false;
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < MESH_LISTENERS_MAX; i++) {
        if ((s_listeners[i].fn == fn) && (s_listeners[i].ev == (uint8_t)ev)) {
            added = true;   /* the same pair twice registers once */
            break;
        }
    }
    for (uint8_t i = 0U; !added && (i < MESH_LISTENERS_MAX); i++) {
        if (s_listeners[i].fn == NULL) {
            s_listeners[i].fn = fn;
            s_listeners[i].ev = (uint8_t)ev;
            added = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return added;
}

bool mesh_aos_listener_remove(const char *name, mesh_listener_t fn)
{
    int const ev = event_of(name);
    bool removed = false;

    if ((ev < 0) || (fn == NULL)) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < MESH_LISTENERS_MAX; i++) {
        if ((s_listeners[i].fn == fn) && (s_listeners[i].ev == (uint8_t)ev)) {
            s_listeners[i].fn = NULL;
            removed = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return removed;
}

bool mesh_aos_listener_wait(const char *name, uint32_t ms, mesh_event_t *out)
{
    int const ev = event_of(name);

    if ((ev < 0) || (ms == 0U) || !events_ensure()) {
        return false;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        return false;   /* a listener waiting would stop the events it waits for */
    }
    uint8_t slot = WAITERS_MAX;
    taskENTER_CRITICAL(&s_lock);
    for (uint8_t i = 0U; i < WAITERS_MAX; i++) {
        if (!s_waiters[i].used && (s_waiters[i].sem != NULL)) {
            s_waiters[i].used = true;
            s_waiters[i].got  = false;
            s_waiters[i].ev   = (uint8_t)ev;
            s_waiters[i].seq0 = s_push_seq;
            slot = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (slot == WAITERS_MAX) {
        return false;
    }
    waiter_t * const w = &s_waiters[slot];
    TickType_t const began = xTaskGetTickCount();
    TickType_t const total = pdMS_TO_TICKS(ms);
    bool got = false;
    bool over = false;

    while (!over) {
        TickType_t const spent = xTaskGetTickCount() - began;
        bool const timed_out = (xSemaphoreTake(w->sem, (spent < total) ? (total - spent) : 0) != pdTRUE);

        taskENTER_CRITICAL(&s_lock);
        got  = w->got;
        over = got || timed_out;
        if (over) {
            if (got && (out != NULL)) {
                *out = w->out;
            }
            w->used = false;   /* from here deliver() gives it nothing */
            w->got  = false;
        }
        taskEXIT_CRITICAL(&s_lock);
        /* Woken with nothing recorded: the give meant for the wait that had this slot before,
         * which came after that wait had read its event. Not this wait's, so it waits on. */
    }
    /* A give that came as this wait ended is left on the semaphore: the next wait on the slot
     * finds nothing recorded and waits on (above). Taking it here could take the give of a wait
     * that has had the slot since it was let go. */
    return got;
}
