/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file receive_ao.c
 * @brief The receiver from an Active Object: a band is set, the receiver is
 *        turned on, and every packet that lands is logged - asked with
 *        mesh_sys.h, which never waits.
 *
 * Wire: the Mesh module on J3 and the Dot on a USB-C cable. A receiver wants
 * an antenna on the sub-GHz SMA (J501) to hear anything beyond the room; it
 * does not need one to run. THIS EXAMPLE NEVER TRANSMITS: it sets 868 MHz at
 * SF9 and turns the receiver on, and the amplifier is never keyed. Another Dot
 * running the send example (or any LoRa node on 868.3 MHz, SF9, BW125) is
 * what it listens to.
 *
 * Try it: when a packet lands the log says its length, its RSSI, its SNR and
 * whether its CRC matched. A packet with a wrong CRC is still delivered by
 * the SDK, for the app to judge; this example logs it and says so. What the
 * payload holds is not logged: a log is read by people the sender never
 * meant it for. Pull the module out and the log says so; put it back and the
 * SDK sets the band and the receiver again by itself, and says so
 * (MESH_SETTING_RESTORED_SIG). Holding the bottom button goes home to kodeOS,
 * as in every app (CONFIG_KODEOS_HOME_HOLD).
 *
 * ---- What this teaches: a request, a command and the news -----------------
 *
 * mesh_sys.h is the extended layer, the calls of an Active Object. None of
 * them waits, and they are of three kinds, all three here:
 *
 *  - a REQUEST, mesh_sys_radio_begin(me, ...): takes this AO, returns at once
 *    and is answered later by an event posted to it,
 *    MESH_RADIO_BEGIN_RSP_SIG. The setting of the band is a request because
 *    the owner calibrates the receiver and the answer says whether it worked.
 *  - a COMMAND, mesh_sys_radio_receive(true): posted to the owner, no answer;
 *    what happens next shows in the news and in mesh_sys_status_read().
 *  - the NEWS, signals this AO subscribed to with mesh_sys_subscribe(): the
 *    module found or lost, a packet landed, the radio went wrong. They come
 *    with no request.
 *
 * A received packet is read with mesh_sys_radio_read(), an immediate: it
 * copies it out and returns. The news only says one is waiting, so a news a
 * full queue dropped costs nothing: the poll reads what it left.
 *
 * ---- The queue, by mesh_sys.h's rule --------------------------------------
 *
 * An answer is posted with no margin, so this AO's queue (KQP_AO_QUEUE_LEN,
 * 16) holds everything that can wait in it at once: one request in flight
 * (the band), the poll's timer and the retry's (2), and the news (posted with
 * a margin, never the overflow). Three of sixteen.
 */
#include "receive_ao.h"

#include "mesh_sys.h"

#define TAG "receive"

/** After a setting up that failed, how long before the next try. */
#define RECEIVE_RETRY_MS       5000U

/** How often the AO asks whether the module is there and reads what a
 *  dropped news may have left in the queue of packets. */
#define RECEIVE_POLL_MS        1000U

/** What is heard: 868 MHz at SF9 (the send example's setting). The power is
 *  the region's cap; it matters to a transmitter, never to this receiver. */
#define RECEIVE_BAND             MESH_BAND_SUB_GHZ
#define RECEIVE_SPREADING_FACTOR 9U
#define RECEIVE_POWER_DBM_X10    MESH_REGION_SUB_GHZ_DBM_X10

/** @brief This AO's private signals. Posted to itself, never published. They are the app's own
 *         range, from MAX_SIG; the SDK's start at MESH_SIG_FIRST. */
enum ReceiveSignals {
    RECEIVE_RETRY_SIG = MAX_SIG,  /**< Time to try the setting up again. */
    RECEIVE_POLL_SIG              /**< Time to look at the module.       */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive       super;
    QTimeEvt      retry;    /**< One-shot, armed after a setting up that failed. */
    QTimeEvt      poll;     /**< Periodic, from the initial transition on.       */
    mesh_packet_t packet;   /**< The packet being read.                          */
    uint32_t      heard;    /**< Packets logged since the AO started.            */
    bool          waiting_to_retry;  /**< A setting up failed and the retry's time has not come:
                                      *   the poll leaves the band alone until it does.       */
} Receive;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Receive l_ao;
QActive * const AO_Receive = &l_ao.super;

static QState Receive_initial(Receive * const me, void const * const par);
static QState Receive_waiting(Receive * const me, QEvt const * const e);
static QState Receive_setting_up(Receive * const me, QEvt const * const e);
static QState Receive_listening(Receive * const me, QEvt const * const e);
static QState Receive_away(Receive * const me, QEvt const * const e);

/**
 * @brief Ask for the band and, once it is set, the receiver follows.
 * @param me The AO.
 * @return true when the request was taken and its answer will come.
 */
static bool ask_band(Receive * const me)
{
    kode_status_t const rc = mesh_sys_radio_begin(&me->super, RECEIVE_BAND, RECEIVE_SPREADING_FACTOR,
                                                  RECEIVE_POWER_DBM_X10);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the band request was not taken (status %d)", (int)rc);
    }
    return KODE_OK == rc;
}

/**
 * @brief A setting up failed: try it again in RECEIVE_RETRY_MS, and not before.
 *
 * The timer is disarmed before it is armed. It may still be running from the
 * try before - the found news asks for the band at once, whatever the timer -
 * and QP/C asserts on arming a time event that is already armed.
 * @param me The AO.
 */
static void retry_later(Receive * const me)
{
    (void)QTimeEvt_disarm(&me->retry);
    (void)QTimeEvt_armX(&me->retry, KQP_TICKS_PER_MS(RECEIVE_RETRY_MS), 0U);
    me->waiting_to_retry = true;
}

/**
 * @brief Read every packet that is waiting and log each: its length, its RSSI
 *        and SNR, the CRC's verdict. Never the payload.
 * @param me The AO.
 */
static void drain(Receive * const me)
{
    while ( KODE_OK == mesh_sys_radio_read(&me->packet) )
    {
        me->heard++;
        KLOG_I(TAG, "packet %lu: %u bytes, RSSI %.0f dBm, SNR %.1f dB, CRC %s", (unsigned long)me->heard,
               (unsigned)me->packet.length, (double)me->packet.rssi_dbm, (double)me->packet.snr_db,
               me->packet.crc_valid ? "good" : "BAD or none (the payload is not to be trusted)");
    }
}

void Receive_ctor(void)
{
    Receive * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Receive_initial));
    QTimeEvt_ctorX(&me->retry, &me->super, (enum_t)RECEIVE_RETRY_SIG, 0U);
    QTimeEvt_ctorX(&me->poll, &me->super, (enum_t)RECEIVE_POLL_SIG, 0U);
    me->heard = 0U;
    me->waiting_to_retry = false;
}

void Receive_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_Receive, TASK_NAME_ATTR, "receive");
    QACTIVE_START(AO_Receive, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: the SDK, the news, the poll.
 *
 * Runs in the thread that called QACTIVE_START(), the one that called kode_init().
 * mesh_sys_begin() switches J3's supply on and starts the owner; it does not wait for a module.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Receive_waiting.
 */
static QState Receive_initial(Receive * const me, void const * const par)
{
    (void)par;

    kode_status_t const rc = mesh_sys_begin();
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the SDK did not start (status %d)", (int)rc);
    }
    (void)mesh_sys_subscribe(&me->super);

    (void)QTimeEvt_armX(&me->poll, KQP_TICKS_PER_MS(RECEIVE_POLL_MS),
                        KQP_TICKS_PER_MS(RECEIVE_POLL_MS));
    KLOG_I(TAG, "up; looking for the Mesh on J3");
    return Q_TRAN(&Receive_waiting);
}

/**
 * @brief No module: the SDK looks for one and says when it finds it.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() once the module is found, Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Receive_waiting(Receive * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_MODULE_FOUND_SIG:
        case RECEIVE_RETRY_SIG: {
            me->waiting_to_retry = false;
            if ( mesh_sys_is_present() )
            {
                KLOG_I(TAG, "found; setting 868 MHz at SF%u", (unsigned)RECEIVE_SPREADING_FACTOR);
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Receive_setting_up);
                }
            }
            break;
        }
        case MESH_WRONG_MODULE_SIG: {
            KLOG_W(TAG, "J3 holds a module that is not a Mesh");
            break;
        }
        case RECEIVE_POLL_SIG: {
            /* The found news may have been dropped by a full queue: the module is what is asked.
             * Not while a failed setting up waits for its retry: that one asks when its time comes. */
            if ( !me->waiting_to_retry && mesh_sys_is_present() )
            {
                KLOG_I(TAG, "found; setting 868 MHz at SF%u", (unsigned)RECEIVE_SPREADING_FACTOR);
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Receive_setting_up);
                }
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief The band is asked for; the answer says whether it is set, and the receiver follows.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to listening, or back to waiting; Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Receive_setting_up(Receive * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_RADIO_BEGIN_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);

            if ( KODE_OK == a->rc )
            {
                /* A command: no answer comes. The receiver is on when the owner has taken it. */
                kode_status_t const rc = mesh_sys_radio_receive(true);

                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "listening; nothing is transmitted");
                    status = Q_TRAN(&Receive_listening);
                }
                else
                {
                    KLOG_W(TAG, "the receiver was not turned on (status %d); trying again in %u s",
                           (int)rc, (unsigned)(RECEIVE_RETRY_MS / 1000U));
                    retry_later(me);
                    status = Q_TRAN(&Receive_waiting);
                }
            }
            else
            {
                KLOG_W(TAG, "the band was refused: %s; trying again in %u s", mesh_sys_why_text(a->why),
                       (unsigned)(RECEIVE_RETRY_MS / 1000U));
                retry_later(me);
                status = Q_TRAN(&Receive_waiting);
            }
            break;
        }
        case MESH_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the Mesh left J3");
            status = Q_TRAN(&Receive_waiting);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief The receiver is on: every packet is read and logged.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() when the module leaves or the radio must be set up again,
 *         Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Receive_listening(Receive * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_PACKET_RECEIVED_SIG: {
            drain(me);
            break;
        }
        case RECEIVE_POLL_SIG: {
            if ( mesh_sys_is_present() )
            {
                drain(me);
            }
            else
            {
                /* The lost news was dropped. */
                KLOG_W(TAG, "the Mesh left J3; the SDK keeps the setting for when it is back");
                status = Q_TRAN(&Receive_away);
            }
            break;
        }
        case MESH_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the Mesh left J3; the SDK keeps the setting for when it is back");
            status = Q_TRAN(&Receive_away);
            break;
        }
        case MESH_SETTING_LOST_SIG: {
            /* The chip lost its setting, or the receiver would not come back: the SDK says so, and
             * it is up to the app to set it up again. (A module pulled out says it too, with the
             * module gone: that is the away state's.) */
            if ( mesh_sys_is_present() )
            {
                KLOG_W(TAG, "the SDK lost the radio's setting; setting the band and the receiver again");
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Receive_setting_up);
                }
            }
            break;
        }
        case MESH_RADIO_FAILED_SIG: {
            mesh_sys_news_t const * const n = Q_EVT_CAST(mesh_sys_news_t);
            KLOG_W(TAG, "radio error: chip word 0x%04lX, reason %lu",
                   (unsigned long)((uint32_t)n->value & MESH_RADIO_ERROR_CHIP_MASK),
                   (unsigned long)((uint32_t)n->value >> 16));
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief The module is out: the SDK keeps what was set up and puts it back when the module returns.
 *
 * Nothing is asked here. The module being found again is not enough to listen: the SDK puts the
 * band and the receiver on it, and MESH_SETTING_RESTORED_SIG says it did. If it could not,
 * MESH_SETTING_LOST_SIG says that, with the module present, and the app sets it up again.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to listening or to the setting up, Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Receive_away(Receive * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_SETTING_RESTORED_SIG: {
            mesh_sys_news_t const * const n = Q_EVT_CAST(mesh_sys_news_t);

            if ( (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER)
                 == ((uint32_t)n->value & (MESH_SETTING_RADIO | MESH_SETTING_RECEIVER)) )
            {
                KLOG_I(TAG, "the Mesh is back and the SDK put the band and the receiver on it; listening");
                status = Q_TRAN(&Receive_listening);
            }
            break;
        }
        case MESH_SETTING_LOST_SIG: {
            /* Lost with the module present: the SDK could not put the setting back. */
            if ( mesh_sys_is_present() )
            {
                KLOG_W(TAG, "the module is back but the setting could not be put on it; setting it again");
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Receive_setting_up);
                }
            }
            break;
        }
        case MESH_MODULE_FOUND_SIG:
        case MESH_MODULE_LOST_SIG: {
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
