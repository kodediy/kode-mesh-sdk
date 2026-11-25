/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_mesh_ao.c
 * @brief The owner's Active Object, faked at mesh_ao.h. See fake_mesh_ao.h.
 */
#include "fake_mesh_ao.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "mesh_ao.h"

#define QUEUE_LENGTH  16U   /* kode-sdk's KQP_AO_QUEUE_LEN                                  */
#define QUEUE_MARGIN  8U    /* mesh_ao.c's AO_QUEUE_MARGIN: left free after a request       */
#define TASK_PRIORITY 5U
#define TASK_STACK    8192U

typedef enum {
    ITEM_REQUEST = 0,
    ITEM_STOP,
    ITEM_INTERRUPT,
    ITEM_REST,
    ITEM_AWAKE,
    ITEM_POWER,
    ITEM_HOME,
} item_kind_t;

typedef struct {
    item_kind_t        kind;
    mesh_aos_request_t request;
    bool               on;      /* ITEM_POWER: the switch  */
    uint8_t            rc;      /* ITEM_POWER: its answer  */
} item_t;

fake_ao_t g_ao = { .tick_on = true, .kode_ready = true };

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;

void fake_ao_reset(void)
{
    g_ao.tick_on      = true;
    g_ao.posts        = 0U;
    g_ao.refused      = 0U;
    g_ao.nudges       = 0U;
    g_ao.interrupts   = 0U;
    g_ao.ticks        = 0U;
    g_ao.served       = 0U;
    g_ao.refuse_posts = false;
    g_ao.stall_ms     = 0U;
    g_ao.app_ao_task  = NULL;
    g_ao.kode_ready   = true;
}

void fake_ao_become_app_ao(void)
{
    g_ao.app_ao_task = (void *)xTaskGetCurrentTaskHandle();
}

/** @brief Queue a signal that arrives without asking: no margin, as on the board. */
static void signal_queue(item_kind_t kind)
{
    item_t item = { .kind = kind };
    if (s_queue != NULL) {
        (void)xQueueSend(s_queue, &item, 0);
    }
}

void fake_ao_interrupt(void)
{
    mesh_ao_radio_interrupt_from_isr();
}

void fake_ao_publish_rest(void)
{
    signal_queue(ITEM_REST);
}

void fake_ao_publish_awake(void)
{
    signal_queue(ITEM_AWAKE);
}

void fake_ao_publish_power(bool on, uint8_t rc)
{
    item_t item = { .kind = ITEM_POWER, .on = on, .rc = rc };
    if (s_queue != NULL) {
        (void)xQueueSend(s_queue, &item, 0);
    }
}

void fake_ao_publish_home(void)
{
    signal_queue(ITEM_HOME);
}

/** @brief The Active Object's loop: a signal as it comes, a tick when one is due. */
static void ao_task(void *arg)
{
    (void)arg;
    uint32_t armed_at = kode_sys_time_ms();

    for (;;) {
        uint32_t const since = kode_sys_time_ms() - armed_at;
        TickType_t const wait = (since >= MESH_AO_TICK_MS) ? 0 : pdMS_TO_TICKS(MESH_AO_TICK_MS - since);
        item_t item;

        if (xQueueReceive(s_queue, &item, wait) == pdTRUE) {
            switch (item.kind) {
            case ITEM_REQUEST:
                mesh_aos_on_request(&item.request);
                break;
            case ITEM_STOP:
                mesh_aos_on_stop();
                break;
            case ITEM_INTERRUPT:
                mesh_aos_on_radio_interrupt();
                break;
            case ITEM_REST:
                mesh_aos_on_rest();
                break;
            case ITEM_AWAKE:
                mesh_aos_on_awake();
                break;
            case ITEM_POWER:
                mesh_aos_on_power(item.on, item.rc);
                break;
            case ITEM_HOME:
                mesh_aos_on_home();
                break;
            default:
                break;
            }
            g_ao.served++;
        }
        if ((kode_sys_time_ms() - armed_at) >= MESH_AO_TICK_MS) {
            armed_at = kode_sys_time_ms();   /* armed again as it is served: one shot */
            if (g_ao.tick_on) {
                unsigned const stall = g_ao.stall_ms;
                if (stall != 0U) {
                    g_ao.stall_ms = 0U;
                    vTaskDelay(pdMS_TO_TICKS(stall));
                }
                mesh_aos_on_tick();
                g_ao.ticks++;
                if (stall != 0U) {
                    g_ao.tick_on = false;
                }
            }
        }
    }
}

kode_status_t mesh_ao_start(void)
{
    if (s_queue != NULL) {
        return KODE_OK;
    }
    s_queue = xQueueCreate(QUEUE_LENGTH, sizeof(item_t));
    if (s_queue == NULL) {
        return KODE_NO_RESOURCES;
    }
    if (xTaskCreate(ao_task, "mesh", TASK_STACK, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t mesh_ao_post(const mesh_aos_request_t *req)
{
    if ((s_queue == NULL) || (req == NULL)) {
        return KODE_DENIED;
    }
    /* A request leaves the room a tick, two interrupts, the supply, a rest, a wake and a home need. */
    if (g_ao.refuse_posts || (uxQueueSpacesAvailable(s_queue) <= QUEUE_MARGIN)) {
        g_ao.refused++;
        return KODE_NO_RESOURCES;
    }
    item_t item = { .kind = ITEM_REQUEST, .request = *req };
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        g_ao.refused++;
        return KODE_NO_RESOURCES;
    }
    g_ao.posts++;
    return KODE_OK;
}

void mesh_ao_nudge(void)
{
    g_ao.nudges++;
    signal_queue(ITEM_STOP);
}

void mesh_ao_radio_interrupt_from_isr(void)
{
    g_ao.interrupts++;
    signal_queue(ITEM_INTERRUPT);
}

bool mesh_ao_is_own_task(void)
{
    return (s_task != NULL) && (xTaskGetCurrentTaskHandle() == s_task);
}

bool mesh_ao_caller_is_ao(const char **name)
{
    bool const own = mesh_ao_is_own_task();
    bool const app = (g_ao.app_ao_task != NULL) && ((void *)xTaskGetCurrentTaskHandle() == g_ao.app_ao_task);

    if ((name != NULL) && (own || app)) {
        *name = own ? "mesh" : "app";
    }
    return own || app;
}

bool mesh_ao_kode_ready(void)
{
    return g_ao.kode_ready;
}
