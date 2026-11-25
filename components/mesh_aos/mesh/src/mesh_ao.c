/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_ao.c
 * @brief AO_Mesh: the module's owner as an Active Object of kode-sdk.
 *        See mesh_ao.h.
 *
 * An Active Object written outside the SDK, on the same QP/C, the same pools and the same
 * bus as kodeOS's own. kode-sdk gives J3 no owner (its README: "the expansion header J3 has
 * no owner"), so a module's firmware is the module's owner. This is the Mesh module's:
 * everything else asks it with an event, and it takes them one at a time, each step to
 * completion. It serves both layers - a task that waits (mesh.h) and an Active Object that
 * asks (mesh_sys.h) - with the one handler, as kode-sdk's own owners do.
 *
 * ---- Its level: KODE_PRIO_USER_HIGH, and why ------------------------------
 *
 * kode-sdk leaves an app three levels and asks a reason for the high one -
 * "only if you can state why the drawing may be delayed". Three, each
 * written down:
 *
 *  - An owner's step is short. A request is a few SPI commands to the
 *    LR2021, each microseconds; a tick is a line read, a UART drained into
 *    the NMEA parser and, once a second, the chip's version asked. A call
 *    that wants an answer waits on the caller's task, never on this one.
 *  - What it guards cannot queue behind a repaint. A test carrier has a
 *    deadline, and a packet the chip never says is done has a backstop:
 *    both take the amplifier off air on this task, whatever the renderer at
 *    KODE_PRIO_USER_GUI is drawing.
 *  - The board's rest (a round for ASLEEP or OFF) is "one run-to-completion step to leave
 *    its device safe": the radio off air and held in reset before the CPU sleeps. It is
 *    only had above the owners it is published to.
 *
 * KODE_PRIO_USER_HIGH does not exist when the image is built with CONFIG_KODE_SCHEDULER
 * (the scheduler has that level), and level 3 is the renderer's (KODE_PRIO_USER_GUI): there
 * the owner asks for CONFIG_MESH_AO_PRIORITY to name a free level, and says so in the log,
 * rather than let QP/C stop the board at a second start on a taken one. It also leaves
 * KODE_PRIO_USER_LOW, where kode-sdk tells an app to start its own Active Object, to the
 * app.
 *
 * ---- What the system tells it ----------------------------------------------
 *
 * Signals published by kode-sdk, each answered with the module safe:
 *
 *  - EXP_POWER_SIG: a switch of J3's supply is over, with how it went (rc). The owner
 *    subscribes before it asks, and keeps the answer in the status (mesh_supply_t).
 *  - LOWPOWER_ROUND_SIG (CONFIG_KODE_LOWPOWER): a round for ASLEEP or OFF. The radio is put
 *    off air and held in reset, the app's setting kept. The owner has not joined the round
 *    (lowpower_join() is in kaos.h, which an app does not include) so it cannot refuse it
 *    and is not waited for; a send in flight is cut short.
 *  - LOWPOWER_STATE_SIG (CONFIG_KODE_LOWPOWER): the new state. AWAKE puts back what the rest
 *    took off the chip; it repeats, and the second time does nothing. Nothing is armed on
 *    ASLEEP: the QP tick is stopped by then. The other states are not the owner's.
 *  - HOME_SIG: the bottom button was held, and the board resets into kodeOS
 *    KODE_HOME_GRACE_MS later, whatever the app is doing. A carrier left up
 *    would stay up through the reset until kodeOS switches J3's supply off:
 *    stopped here, it stops with the gesture. Nothing to save, so no
 *    kode_sys_home_defer().
 *
 * Nothing else tells an Active Object the app is ending - not
 * kode_exit_to_os(), not a crash, not the watchdog. For a restart the radio
 * driver has a hook of its own that holds the chip in reset
 * (mesh_drv_lr2021.h); for a crash there is nothing, and kodeOS switching
 * J3's supply off as it comes back is what ends a carrier then.
 *
 * ---- What it takes from the system -----------------------------------------
 *
 *  - Per request, an event from kode-sdk's pools, back in the pool once
 *    served: taken only while the pool keeps AO_POOL_MARGIN blocks for the
 *    rest of the system. A dry pool is a refused request, never QP/C's
 *    assertion. A packet's payload does not travel in the event - 255 bytes
 *    would reach the jumbo tier kodeOS keeps for installs - but in one of
 *    mesh_aos.c's own slots.
 *  - Its queue, AO_QUEUE_LENGTH deep, posted to only while AO_QUEUE_MARGIN
 *    slots stay free. The tick, the radio's interrupt, the supply's answer, the rest, the
 *    wake and HOME_SIG arrive without asking and with no margin: one of them finding
 *    the queue full would be the assertion, which is why the tick is one
 *    shot (below) and the interrupt can be queued once at most.
 *  - Its own stack, AO_STACK_BYTES of internal RAM, kode-sdk's default for
 *    an AO. No worker: nothing the owner does waits long enough to need one.
 *
 * ---- One tick in the queue at most -----------------------------------------
 *
 * The tick is a one-shot time event, armed again each time it is served, not
 * a periodic one. kode-sdk's QF tick is an esp_timer, and an esp_timer held
 * back fires every tick it missed back to back: a periodic time event would
 * then post one per MESH_AO_TICK_MS into a queue no one drains meanwhile,
 * and with AO_QUEUE_LENGTH 16 a hold of 320 ms is QP/C's assertion
 * (qf_port:130), as an app met it on a board while it opened a network
 * session. A late tick costs nothing here: mesh_aos_on_tick() counts the
 * time that really passed, not MESH_AO_TICK_MS.
 *
 * kode-sdk's watch on every Active Object (CONFIG_KQP_AO_WATCH) times this
 * one's steps as it times kodeOS's. The longest is a look while the module
 * is absent, about 42 ms of sleeps for the lines to settle (mesh_aos.c), and
 * a radio setting, whose calibration the LR2021 finishes in milliseconds;
 * the chip's own limits on those waits (MESH_BSP_LORA_CALIB_MS,
 * MESH_BSP_LORA_RESET_MS) are only reached by a chip that no longer answers.
 */
#include "mesh_ao.h"

#include <stddef.h>

#include "kode_log.h"
#include "kqp.h"

#include "sdkconfig.h"

#define TAG "mesh"

#if defined(CONFIG_MESH_AO_PRIORITY) && (CONFIG_MESH_AO_PRIORITY > 0)
#define AO_PRIORITY      ((uint8_t)CONFIG_MESH_AO_PRIORITY)
#elif defined(KODE_PRIO_USER_HIGH)
#define AO_PRIORITY      ((uint8_t)KODE_PRIO_USER_HIGH)
#else
#define AO_PRIORITY      0U   /* KODE_PRIO_USER_HIGH does not exist here: mesh_ao_start() says so and refuses */
#endif

#define AO_QUEUE_LENGTH  KQP_AO_QUEUE_LEN
#define AO_QUEUE_MARGIN  8U   /* left after a request: a tick, two interrupts, the supply's answer, a rest, a wake, a home, one spare */
#define AO_POOL_MARGIN   8U   /* blocks a request leaves the system */
#define AO_STACK_BYTES   KQP_AO_STACK_BYTES

/** @brief The owner's private signals: posted, never published. */
enum MeshSignals {
    MESH_REQUEST_SIG = MAX_SIG, /**< A request (MeshRequestEvt). */
    MESH_TICK_SIG,              /**< The time event, MESH_AO_TICK_MS after the last was served. */
    MESH_STOP_SIG,              /**< A mesh_aos_stop() to apply. No payload. */
    MESH_RADIO_INTERRUPT_SIG,   /**< The radio's interrupt line rose. No payload. */
};

/** @brief A request, carried in a pool block. */
typedef struct {
    QEvt               super;
    mesh_aos_request_t req;
} MeshRequestEvt;

typedef struct {
    QActive  super;
    QTimeEvt tick;
} Mesh;

static QEvtPtr     l_queue[AO_QUEUE_LENGTH];
static StackType_t l_stack[AO_STACK_BYTES / sizeof(StackType_t)];
static Mesh        l_ao;
static bool        s_ao_started;

/** @brief Every stop is the same event: no block, no payload, posted from any task. */
static QEvt const  STOP_EVT = QEVT_INITIALIZER(MESH_STOP_SIG);
/** @brief Every interrupt is the same event, posted from the line's interrupt. */
static QEvt const  INTERRUPT_EVT = QEVT_INITIALIZER(MESH_RADIO_INTERRUPT_SIG);

static QActive * const AO_Mesh = &l_ao.super;

static QState Mesh_initial(Mesh * const me, void const * const par);
static QState Mesh_running(Mesh * const me, QEvt const * const e);

bool mesh_ao_kode_ready(void)
{
    /* The pools are made by kode_init(): before it there is none, and a post would be QP/C's assertion. */
    return QF_poolGetMaxBlockSize() != 0U;
}

/**
 * @brief The Active Object made, all but the task: what tests/host_ao runs
 *        before it dispatches by hand.
 * @return KODE_OK; KODE_NO_RESOURCES as mesh_ao_start().
 */
static kode_status_t Mesh_ctor(void)
{
    /* A pool too narrow would be QF_newX_()'s assertion at the first request. */
    if (sizeof(MeshRequestEvt) > (size_t)QF_poolGetMaxBlockSize()) {
        KLOG_E(TAG, "a request (%u bytes) fits no event pool (%u at most)",
               (unsigned)sizeof(MeshRequestEvt), (unsigned)QF_poolGetMaxBlockSize());
        return KODE_NO_RESOURCES;
    }
    Mesh * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Mesh_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, (enum_t)MESH_TICK_SIG, 0U);
    return KODE_OK;
}

/**
 * @brief Whether the level this build gives the owner is its to take. QP/C answers a second
 *        start at a taken level with an assertion and a reset; this says so first, by name.
 */
static bool level_is_free(void)
{
    const char *holder = "?";

    if (AO_PRIORITY == 0U) {
        KLOG_E(TAG, "the owner has no level: KODE_PRIO_USER_HIGH does not exist in this image "
                    "(CONFIG_KODE_SCHEDULER takes it); set CONFIG_MESH_AO_PRIORITY to a free level");
        return false;
    }
    if (AO_PRIORITY == (uint8_t)KODE_PRIO_USER_GUI) {
        KLOG_E(TAG, "level %u is the renderer's (KODE_PRIO_USER_GUI) and the standard layer's: "
                    "set CONFIG_MESH_AO_PRIORITY to another", (unsigned)AO_PRIORITY);
        return false;
    }
    if (kqp_prio_taken(AO_PRIORITY, &holder)) {
        KLOG_E(TAG, "the owner's level %u is held by '%s': set CONFIG_MESH_AO_PRIORITY to a free level",
               (unsigned)AO_PRIORITY, holder);
        return false;
    }
    return true;
}

kode_status_t mesh_ao_start(void)
{
    if (s_ao_started) {
        return KODE_OK;
    }
    if (!level_is_free()) {
        return KODE_DENIED;
    }
    kode_status_t const rc = Mesh_ctor();
    if (rc != KODE_OK) {
        return rc;
    }

    KLOG_I(TAG, "the owner starts at priority %u (CONFIG_MESH_AO_PRIORITY moves it)",
           (unsigned)AO_PRIORITY);
    /* Named, so a full queue or an assertion names this owner and not "AO". */
    QActive_setAttr(AO_Mesh, TASK_NAME_ATTR, "mesh");
    QACTIVE_START(AO_Mesh, AO_PRIORITY,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
    s_ao_started = true;
    return KODE_OK;
}

kode_status_t mesh_ao_post(const mesh_aos_request_t *req)
{
    if (!s_ao_started || (req == NULL)) {
        return KODE_DENIED;
    }
    MeshRequestEvt *const e = Q_NEW_X(MeshRequestEvt, AO_POOL_MARGIN, MESH_REQUEST_SIG);
    if (e == NULL) {
        return KODE_NO_RESOURCES;
    }
    e->req = *req;
    /* Not posted, QActive_post_() hands the block back to its pool. */
    return QACTIVE_POST_X(AO_Mesh, &e->super, AO_QUEUE_MARGIN, (void *)0)
           ? KODE_OK : KODE_NO_RESOURCES;
}

void mesh_ao_nudge(void)
{
    if (s_ao_started) {
        /* Refused only with the queue nearly full: the next tick applies it. */
        (void)QACTIVE_POST_X(AO_Mesh, &STOP_EVT, AO_QUEUE_MARGIN, (void *)0);
    }
}

void mesh_ao_radio_interrupt_from_isr(void)
{
    if (s_ao_started) {
        kqp_post_from_isr(AO_Mesh, &INTERRUPT_EVT);
    }
}

bool mesh_ao_is_own_task(void)
{
    return s_ao_started && (xTaskGetCurrentTaskHandle() == l_ao.super.thread);
}

bool mesh_ao_caller_is_ao(const char **name)
{
    return kqp_current_is_ao(name);
}

/**
 * @brief The initial transition: the system's signals for an owner, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Mesh_running.
 */
static QState Mesh_initial(Mesh * const me, void const * const par)
{
    (void)par;
    QActive_subscribe(&me->super, HOME_SIG);
    QActive_subscribe(&me->super, EXP_POWER_SIG);
#if CONFIG_KODE_LOWPOWER
    QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
#endif
    return Q_TRAN(&Mesh_running);
}

/**
 * @brief The one state: requests as they come, a tick MESH_AO_TICK_MS after the last.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Mesh_running(Mesh * const me, QEvt const * const e)
{
    QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG:
        /* One shot, armed again as each is served: never more than one in the
         * queue (the file's head, "One tick in the queue at most"). */
        QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(MESH_AO_TICK_MS), 0U);
        status = Q_HANDLED();
        break;
    case Q_EXIT_SIG:
        (void)QTimeEvt_disarm(&me->tick);
        status = Q_HANDLED();
        break;
    case MESH_REQUEST_SIG:
        mesh_aos_on_request(&((MeshRequestEvt const *)e)->req);
        status = Q_HANDLED();
        break;
    case MESH_TICK_SIG:
        QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(MESH_AO_TICK_MS), 0U);
        mesh_aos_on_tick();
        status = Q_HANDLED();
        break;
    case MESH_RADIO_INTERRUPT_SIG:
        mesh_aos_on_radio_interrupt();
        status = Q_HANDLED();
        break;
    case MESH_STOP_SIG:
        mesh_aos_on_stop();
        status = Q_HANDLED();
        break;
    case EXP_POWER_SIG:
        mesh_aos_on_power(((ExpPowerDoneEvt const *)e)->on, ((ExpPowerDoneEvt const *)e)->rc);
        status = Q_HANDLED();
        break;
#if CONFIG_KODE_LOWPOWER
    case LOWPOWER_ROUND_SIG:
        /* Ready or busy would be the answer of a participant (lowpower_join(), kaos.h): not joined,
         * the owner rests and is not waited for. */
        if ((((LowPowerRoundEvt const *)e)->target == (uint8_t)KODE_LP_ASLEEP)
            || (((LowPowerRoundEvt const *)e)->target == (uint8_t)KODE_LP_OFF)) {
            mesh_aos_on_rest();
        }
        status = Q_HANDLED();
        break;
    case LOWPOWER_STATE_SIG:
        if (((LowPowerStateEvt const *)e)->state == (uint8_t)KODE_LP_AWAKE) {
            mesh_aos_on_awake();
        }
        status = Q_HANDLED();
        break;
#endif
    case HOME_SIG:
        mesh_aos_on_home();
        status = Q_HANDLED();
        break;
    default:
        status = Q_SUPER(&QHsm_top);
        break;
    }
    return status;
}
