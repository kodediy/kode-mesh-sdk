/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_events.c
 * @brief What the owner tells the standard layer (mesh_aos_events.c): the names, the
 *        listeners, the waiters and the queue between them, on the SDK's own task.
 *
 * The events task and its queue are made the first time anyone asks to hear, and are never
 * taken down, so every case here leaves the listener table empty. The owner may tell
 * events of its own while a case runs (the module found, say): a listener here keeps only
 * the ones that carry MAGIC in their value, which the owner never does.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "owner_fakes.h"
#include "owner_harness.h"

#define MAGIC   0x5EED0000
#define KEPT_MAX 40U

typedef struct {
    int32_t      value;
    char         name[MESH_EVENT_NAME_MAX + 1U];
    char         text[64];
    TaskHandle_t task;
} kept_t;

static kept_t            s_kept[KEPT_MAX];
static volatile unsigned s_n;

static void keep(const mesh_event_t *e)
{
    if (s_n < KEPT_MAX) {
        kept_t *const k = &s_kept[s_n];
        k->value = e->value;
        k->task  = xTaskGetCurrentTaskHandle();
        (void)snprintf(k->name, sizeof(k->name), "%s", e->name);
        (void)snprintf(k->text, sizeof(k->text), "%s", e->text);
        s_n++;
    }
}

/** @brief A listener that writes down everything it is told. The owner may tell events of its
 *         own while a case runs, so the cases that use it settle the owner first. */
static void hear(const mesh_event_t *e)
{
    keep(e);
}

static void hear_other(const mesh_event_t *e)
{
    if ((e->value & ~0xFFFF) == MAGIC) {
        keep(e);
    }
}

static void forget(void)
{
    s_n = 0U;
    (void)memset(s_kept, 0, sizeof(s_kept));
}

TEST_CASE("events: twelve events have a name, the numbers between them have none, and a name is known only whole", "[owner][events]")
{
    harness_fresh();
    unsigned named = 0U;
    for (unsigned ev = 0U; ev <= MESH_AOS_EVENT_MAX + 2U; ev++) {
        const char *const name = mesh_aos_event_name((uint8_t)ev);
        if (name != NULL) {
            named++;
            TEST_ASSERT_TRUE_MESSAGE(strlen(name) < MESH_EVENT_NAME_MAX, name);
            TEST_ASSERT_TRUE_MESSAGE(mesh_aos_event_known(name), name);
        }
    }
    TEST_ASSERT_EQUAL_UINT(12U, named);
    TEST_ASSERT_NULL(mesh_aos_event_name(0U));
    TEST_ASSERT_NULL(mesh_aos_event_name(4U));
    TEST_ASSERT_NULL(mesh_aos_event_name(12U));
    TEST_ASSERT_NULL(mesh_aos_event_name(255U));
    TEST_ASSERT_EQUAL_STRING("module_found", mesh_aos_event_name(MESH_AOS_EVENT_PRESENT));
    TEST_ASSERT_EQUAL_STRING("packet_received", mesh_aos_event_name(MESH_AOS_EVENT_PACKET));
    TEST_ASSERT_EQUAL_STRING("header_failed", mesh_aos_event_name(MESH_AOS_EVENT_HEADER_BAD));

    TEST_ASSERT_FALSE(mesh_aos_event_known(NULL));
    TEST_ASSERT_FALSE(mesh_aos_event_known(""));
    TEST_ASSERT_FALSE(mesh_aos_event_known("module"));
    TEST_ASSERT_FALSE(mesh_aos_event_known("module_found "));
    TEST_ASSERT_FALSE(mesh_aos_event_known("MODULE_FOUND"));
    TEST_ASSERT_FALSE(mesh_aos_event_known("module_found_and_more_text_past_the_limit"));
}

TEST_CASE("listeners: a pair registers once, eight at most, and what is not a name or a function is refused", "[owner][events]")
{
    harness_fresh();
    static const char *const NAMES[] = {
        "module_found", "module_lost", "wrong_module", "setting_lost", "setting_restored",
        "packet_received", "listen_ended", "radio_failed",
    };
    TEST_ASSERT_EQUAL_UINT(MESH_LISTENERS_MAX, sizeof(NAMES) / sizeof(NAMES[0]));

    TEST_ASSERT_FALSE(mesh_aos_listener_add(NULL, hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_add("module_found", NULL));
    TEST_ASSERT_FALSE(mesh_aos_listener_add("no_such_event", hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_add("", hear_other));

    for (unsigned k = 0U; k < MESH_LISTENERS_MAX; k++) {
        TEST_ASSERT_TRUE_MESSAGE(mesh_aos_listener_add(NAMES[k], hear_other), NAMES[k]);
    }
    /* The same pair again is the same registration, even with the table full. */
    TEST_ASSERT_TRUE(mesh_aos_listener_add("module_found", hear_other));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("radio_failed", hear_other));
    /* A ninth pair has no room; a pair of the same function and another name is a new one. */
    TEST_ASSERT_FALSE(mesh_aos_listener_add("test_ended", hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_add("module_found", hear));

    /* Removing frees a place, once. */
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("module_lost", hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("module_lost", hear_other));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("test_ended", hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove(NULL, hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("module_found", NULL));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("no_such_event", hear_other));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("module_found", hear));   /* another function's */

    for (unsigned k = 0U; k < MESH_LISTENERS_MAX; k++) {
        (void)mesh_aos_listener_remove(NAMES[k], hear_other);
    }
    (void)mesh_aos_listener_remove("test_ended", hear_other);
    TEST_ASSERT_TRUE(mesh_aos_listener_add("module_found", hear_other));   /* the table is empty again */
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("module_found", hear_other));
}

TEST_CASE("listeners: an event reaches each listener of its name, in order, on the SDK's own task", "[owner][events]")
{
    harness_fresh();
    forget();
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", hear_other));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("preamble_detected", hear_other));

    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 1));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_PREAMBLE, MAGIC + 2));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 3));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER, MAGIC + 4));   /* nobody listens to it */
    TEST_ASSERT_TRUE(WAIT_FOR(s_n >= 3U, 1000));
    vTaskDelay(pdMS_TO_TICKS(20));
    TEST_ASSERT_EQUAL_UINT(3U, s_n);

    TEST_ASSERT_EQUAL_INT32(MAGIC + 1, s_kept[0].value);
    TEST_ASSERT_EQUAL_STRING("header_failed", s_kept[0].name);
    TEST_ASSERT_EQUAL_INT32(MAGIC + 2, s_kept[1].value);
    TEST_ASSERT_EQUAL_STRING("preamble_detected", s_kept[1].name);
    TEST_ASSERT_EQUAL_INT32(MAGIC + 3, s_kept[2].value);
    TEST_ASSERT_EQUAL_STRING("", s_kept[0].text);

    /* On the SDK's task: not the case's, and one task for all of them. */
    TEST_ASSERT_NOT_EQUAL(xTaskGetCurrentTaskHandle(), s_kept[0].task);
    TEST_ASSERT_EQUAL_PTR(s_kept[0].task, s_kept[2].task);
    TEST_ASSERT_EQUAL_STRING("mesh_events", pcTaskGetName(s_kept[0].task));

    /* Two listeners of one name both hear it, in the order they were added. */
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", hear));
    forget();
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 5));
    TEST_ASSERT_TRUE(WAIT_FOR(s_n >= 2U, 1000));
    TEST_ASSERT_EQUAL_INT32(MAGIC + 5, s_kept[0].value);
    TEST_ASSERT_EQUAL_INT32(MAGIC + 5, s_kept[1].value);

    /* A removed one hears nothing more. */
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("header_failed", hear_other));
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("header_failed", hear));
    forget();
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 6));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT(0U, s_n);
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("preamble_detected", hear_other));
}

TEST_CASE("listeners: each event carries its value and the words that go with it", "[owner][events]")
{
    harness_fresh();
    TEST_ASSERT_TRUE(mesh_aos_listener_add("module_found", hear));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("setting_lost", hear));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("setting_restored", hear));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("test_ended", hear));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("radio_failed", hear));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("wrong_module", hear));
    h_module_away();   /* settles the owner: no event of its own comes in what follows */
    h_module_back();
    (void)WAIT_FOR(h_status().present, 5000U);
    vTaskDelay(pdMS_TO_TICKS(30));
    forget();

    static const struct {
        uint8_t     ev;
        int32_t     value;
        const char *text;
    } WORDS[] = {
        { MESH_AOS_EVENT_PRESENT,          1,  "upright" },
        { MESH_AOS_EVENT_PRESENT,          0,  "" },
        { MESH_AOS_EVENT_SETTING_LOST,     1,  "radio" },
        { MESH_AOS_EVENT_SETTING_LOST,     2,  "receiver" },
        { MESH_AOS_EVENT_SETTING_LOST,     3,  "radio and receiver" },
        { MESH_AOS_EVENT_SETTING_LOST,     4,  "gnss" },
        { MESH_AOS_EVENT_SETTING_LOST,     7,  "radio, receiver and gnss" },
        { MESH_AOS_EVENT_SETTING_RESTORED, 5,  "radio and gnss" },
        { MESH_AOS_EVENT_SETTING_RESTORED, 6,  "receiver and gnss" },
        { MESH_AOS_EVENT_TEST_END,         (int32_t)MESH_TEST_END_CHIP,  "ended by the chip's timer" },
        { MESH_AOS_EVENT_TEST_END,         (int32_t)MESH_TEST_END_OWNER, "ended by the SDK's deadline" },
        { MESH_AOS_EVENT_RADIO_ERROR,      (int32_t)(MESH_RADIO_ERROR_NO_STANDBY | 3U),
          "the chip would not confirm standby and was reset" },
        { MESH_AOS_EVENT_RADIO_ERROR,      (int32_t)(MESH_RADIO_ERROR_SEND_TIMEOUT),
          "a packet's timer ran out without a done flag" },
        { MESH_AOS_EVENT_RADIO_ERROR,      (int32_t)(MESH_RADIO_ERROR_AMPLIFIER_FAULT),
          "the amplifier faulted: the radio is off" },
        { MESH_AOS_EVENT_RADIO_ERROR,      (int32_t)(MESH_RADIO_ERROR_RECEIVER | 0x10U),
          "the receiver would not come back on" },
        { MESH_AOS_EVENT_RADIO_ERROR,      0x1234,  "the chip reported an error" },
        { MESH_AOS_EVENT_WRONG_MODULE,     (int32_t)MESH_MODULE_OTHER, "something that is not a Mesh is on J3" },
        { MESH_AOS_EVENT_WRONG_MODULE,     (int32_t)MESH_MODULE_MESH,  "" },
    };
    for (unsigned k = 0U; k < (sizeof(WORDS) / sizeof(WORDS[0])); k++) {
        forget();
        TEST_ASSERT_TRUE(mesh_aos_events_push(WORDS[k].ev, WORDS[k].value));
        TEST_ASSERT_TRUE_MESSAGE(WAIT_FOR(s_n >= 1U, 1000), WORDS[k].text);
        TEST_ASSERT_EQUAL_INT32(WORDS[k].value, s_kept[0].value);
        TEST_ASSERT_EQUAL_STRING(WORDS[k].text, s_kept[0].text);
        TEST_ASSERT_EQUAL_STRING(mesh_aos_event_name(WORDS[k].ev), s_kept[0].name);
    }
    (void)mesh_aos_listener_remove("module_found", hear);
    (void)mesh_aos_listener_remove("setting_lost", hear);
    (void)mesh_aos_listener_remove("setting_restored", hear);
    (void)mesh_aos_listener_remove("test_ended", hear);
    (void)mesh_aos_listener_remove("radio_failed", hear);
    (void)mesh_aos_listener_remove("wrong_module", hear);
}

typedef struct {
    const char  *name;
    uint32_t     ms;
    mesh_event_t out;
    volatile bool began;
    volatile bool got;
} waiting_t;

static void waiter_body(void *arg)
{
    waiting_t *const w = arg;
    w->began = true;
    w->got   = mesh_aos_listener_wait(w->name, w->ms, &w->out);
}

TEST_CASE("wait: it hands back the event that comes after it began, and nothing that came before", "[owner][events]")
{
    harness_fresh();
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_received", hear_other));   /* makes the events task */

    /* Before it began: not its. */
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER, MAGIC + 10));
    vTaskDelay(pdMS_TO_TICKS(30));
    mesh_event_t out = { 0 };
    uint32_t const t0 = kode_sys_time_ms();
    TEST_ASSERT_FALSE(mesh_aos_listener_wait("header_received", 60U, &out));
    TEST_ASSERT_TRUE((kode_sys_time_ms() - t0) >= 55U);   /* it waited the time asked */

    /* After: its, whole. */
    waiting_t w = { .name = "header_received", .ms = 2000U };
    h_bg_t bg;
    h_bg_start(&bg, waiter_body, &w);
    TEST_ASSERT_TRUE(WAIT_FOR(w.began, 1000));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER, MAGIC + 11));
    TEST_ASSERT_TRUE(h_bg_join(&bg, 2000U));
    TEST_ASSERT_TRUE(w.got);
    TEST_ASSERT_EQUAL_STRING("header_received", w.out.name);
    TEST_ASSERT_EQUAL_INT32(MAGIC + 11, w.out.value);
    TEST_ASSERT_EQUAL_STRING("", w.out.text);

    /* Another event does not wake it. */
    waiting_t other = { .name = "header_received", .ms = 100U };
    h_bg_start(&bg, waiter_body, &other);
    TEST_ASSERT_TRUE(WAIT_FOR(other.began, 1000));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_PREAMBLE, MAGIC + 12));
    TEST_ASSERT_TRUE(h_bg_join(&bg, 2000U));
    TEST_ASSERT_FALSE(other.got);
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("header_received", hear_other));
}

TEST_CASE("wait: no time, an unknown name and a fifth waiter are refused, and one event wakes every waiter of its name", "[owner][events]")
{
    harness_fresh();
    mesh_event_t out = { 0 };
    TEST_ASSERT_FALSE(mesh_aos_listener_wait("header_received", 0U, &out));
    TEST_ASSERT_FALSE(mesh_aos_listener_wait(NULL, 10U, &out));
    TEST_ASSERT_FALSE(mesh_aos_listener_wait("no_such_event", 10U, &out));

    /* Four waiters at once is the limit; the fifth is refused at once, the others wait on. */
    waiting_t w[5];
    h_bg_t    bg[5];
    for (unsigned k = 0U; k < 5U; k++) {
        w[k] = (waiting_t){ .name = "header_received", .ms = 400U };
    }
    for (unsigned k = 0U; k < 4U; k++) {
        h_bg_start(&bg[k], waiter_body, &w[k]);
        TEST_ASSERT_TRUE(WAIT_FOR(w[k].began, 1000));
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    uint32_t const t0 = kode_sys_time_ms();
    TEST_ASSERT_FALSE(mesh_aos_listener_wait("header_received", 400U, &out));
    TEST_ASSERT_TRUE((kode_sys_time_ms() - t0) < 200U);   /* refused, not timed out */
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER, MAGIC + 20));
    for (unsigned k = 0U; k < 4U; k++) {
        TEST_ASSERT_TRUE(h_bg_join(&bg[k], 2000U));
        TEST_ASSERT_TRUE(w[k].got);   /* one event wakes every waiter of its name */
        TEST_ASSERT_EQUAL_INT32(MAGIC + 20, w[k].out.value);
    }
}

static SemaphoreHandle_t s_gate;
static volatile bool     s_inside;
static volatile unsigned s_heard;
static volatile int      s_wait_in_listener = -1;

static void slow(const mesh_event_t *e)
{
    if ((e->value & ~0xFFFF) != MAGIC) {
        return;
    }
    if (s_heard == 0U) {
        mesh_event_t other;
        /* The first one also tries to wait from the events task, which is refused at once. */
        s_wait_in_listener = mesh_aos_listener_wait("header_received", 500U, &other) ? 1 : 0;
        s_inside = true;
        (void)xSemaphoreTake(s_gate, pdMS_TO_TICKS(5000));
    }
    s_heard++;
}

TEST_CASE("queue: a slow listener costs the events behind it, sixteen wait and the rest are lost and counted, in order", "[owner][events]")
{
    harness_fresh();
    s_gate = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_gate);
    s_inside = false;
    s_heard  = 0U;
    s_wait_in_listener = -1;
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", slow));

    uint32_t const dropped0 = mesh_aos_events_dropped();
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 0));
    TEST_ASSERT_TRUE(WAIT_FOR(s_inside, 1000));
    TEST_ASSERT_EQUAL_INT(0, s_wait_in_listener);   /* a listener that waits is told no, at once */

    /* The task holds one; the queue holds sixteen more. */
    for (unsigned k = 1U; k <= 16U; k++) {
        TEST_ASSERT_TRUE_MESSAGE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + (int32_t)k), "queued");
    }
    TEST_ASSERT_FALSE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 17));
    TEST_ASSERT_FALSE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 18));
    TEST_ASSERT_EQUAL_UINT32(dropped0 + 2U, mesh_aos_events_dropped());
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().events_dropped == (dropped0 + 2U), 500));   /* the status is the owner's tick's */

    (void)xSemaphoreGive(s_gate);
    TEST_ASSERT_TRUE(WAIT_FOR(s_heard == 17U, 2000));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_UINT(17U, s_heard);   /* the first and the sixteen: none of the lost ones */
    TEST_ASSERT_TRUE(mesh_aos_listener_remove("header_failed", slow));
    vSemaphoreDelete(s_gate);
    s_gate = NULL;

    /* The queue is free again. */
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 30));
}

/* ---- Removing a listener ------------------------------------------------------- */

static volatile bool     s_long_in;
static volatile bool     s_long_out;
static volatile unsigned s_long_calls;

/** @brief A listener that takes its time, as an app's may. */
static void long_call(const mesh_event_t *e)
{
    if ((e->value & ~0xFFFF) != MAGIC) {
        return;
    }
    s_long_calls++;
    s_long_in = true;
    vTaskDelay(pdMS_TO_TICKS(200));
    s_long_out = true;
}

TEST_CASE("remove: from another task it returns only once the call running has ended, and none comes after", "[owner][events]")
{
    harness_fresh();
    s_long_in    = false;
    s_long_out   = false;
    s_long_calls = 0U;
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", long_call));

    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 1));
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 2));   /* queued behind it */
    TEST_ASSERT_TRUE(WAIT_FOR(s_long_in, 1000));

    TEST_ASSERT_TRUE(mesh_aos_listener_remove("header_failed", long_call));
    /* What the listener uses may be freed from here: its call is over. */
    TEST_ASSERT_TRUE(s_long_out);

    vTaskDelay(pdMS_TO_TICKS(400));
    TEST_ASSERT_EQUAL_UINT(1U, s_long_calls);   /* not for the event queued behind */
}

static volatile unsigned s_self_calls;
static volatile int      s_self_removed = -1;

/** @brief A listener that removes itself and the one after it, from inside its call. */
static void self_remove(const mesh_event_t *e)
{
    if ((e->value & ~0xFFFF) != MAGIC) {
        return;
    }
    s_self_calls++;
    s_self_removed = mesh_aos_listener_remove("header_failed", self_remove) ? 1 : 0;
    (void)mesh_aos_listener_remove("header_failed", hear_other);
}

TEST_CASE("remove: from inside a listener it does not wait for itself, and one removed there is not called after", "[owner][events]")
{
    harness_fresh();
    forget();
    s_self_calls   = 0U;
    s_self_removed = -1;
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", self_remove));
    TEST_ASSERT_TRUE(mesh_aos_listener_add("header_failed", hear_other));   /* after it, same event */

    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 1));
    TEST_ASSERT_TRUE(WAIT_FOR(s_self_calls == 1U, 1000));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_INT(1, s_self_removed);
    TEST_ASSERT_EQUAL_UINT(0U, s_n);   /* hear_other was removed before its turn came */

    /* both are gone: the next one reaches neither */
    TEST_ASSERT_TRUE(mesh_aos_events_push(MESH_AOS_EVENT_HEADER_BAD, MAGIC + 2));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT(1U, s_self_calls);
    TEST_ASSERT_EQUAL_UINT(0U, s_n);
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("header_failed", self_remove));
    TEST_ASSERT_FALSE(mesh_aos_listener_remove("header_failed", hear_other));
}
