/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file send_ao.c
 * @brief The transmitter from an Active Object: one short packet on 868 MHz
 *        every SEND_INTERVAL_MS, at the region's power cap, inside the duty
 *        cycle - asked with mesh_sys.h, which never waits.
 *
 * >>> AN ANTENNA OR A 50 OHM LOAD ON J501 BEFORE RUNNING. THE LR2021'S
 *     AMPLIFIER IS GUARANTEED ONLY INTO A LOAD; KEYING IT INTO AN OPEN
 *     CONNECTOR CAN DAMAGE IT. <<<
 *
 * >>> 868 IS SHARED AND THE DUTY CYCLE IS THE LAW. THE SUB-BAND ALLOWS A
 *     TRANSMITTER ON AIR 1 % OF THE TIME. THE SDK KEEPS THAT BUDGET AND
 *     REFUSES A PACKET IT HAS NO ROOM FOR; THIS EXAMPLE ASKS FOR FAR LESS
 *     THAN IT ALLOWS AND NEVER LOOKS FOR A WAY ROUND IT. <<<
 *
 * Wire: the Mesh module on J3 with that antenna or load on the sub-GHz SMA
 * (J501), and the Dot on a USB-C cable. NOTHING IS SENT UNTIL YOU PRESS THE TOP
 * BUTTON: the radio is set up, the log says it is ready, and a press starts the
 * packets - the first at once, the next ones every SEND_INTERVAL_MS. A second
 * press stops them. If the module is pulled out, the sending is off when it
 * comes back: press again once the antenna is on. Another Dot running the
 * receive example hears them.
 *
 * Try it: the log says each packet when it has left, with the time it was on
 * air. The power is the region's cap (MESH_REGION_SUB_GHZ_DBM_X10, 8.5 dBm on
 * the nominal scale): asking for more gets the same. The press
 * is also the person's word that the antenna or the load is on: the example
 * gives it to the SDK (mesh_sys_radio_antennas_confirm()),
 * which until then holds every transmit to its safe power, 0 dBm. If the
 * amplifier trips its over-current alarm, the log asks whether the antenna is
 * loose and the SDK holds the band at 0 dBm until the next press. Before
 * every packet the example asks mesh_sys_radio_send_wait_ms() how long the duty
 * budget still has to refill, and waits that long if it is not zero; and if the
 * SDK refuses a packet all the same, the answer says MESH_WHY_DUTY and how long
 * to wait. Pull the module out and the log says so; put it back and the SDK
 * puts the band back itself (MESH_SETTING_RESTORED_SIG). Leaving, by the bottom
 * button or any restart, takes the radio off air: the SDK holds the module in
 * reset on the way out.
 *
 * ---- What this teaches: a request, and its answer -------------------------
 *
 * mesh_sys.h is the extended layer, the calls of an Active Object. None of
 * them waits. The two this file makes are REQUESTS: mesh_sys_radio_begin(me,
 * ...) and mesh_sys_radio_send(me, ...) take this AO first, return at once
 * (KODE_OK: taken, the answer will come), and the answer arrives later as an
 * event posted to this AO alone: MESH_RADIO_BEGIN_RSP_SIG, a mesh_sys_answer_t,
 * and MESH_RADIO_SEND_RSP_SIG, a mesh_sys_send_answer_t with the airtime and,
 * when the duty budget had no room, how long to wait. A send is answered when
 * the packet is gone, not when it is queued: the state "sending" below is the
 * wait for that, and it waits without blocking anything.
 *
 * There is ONE DOOR per kind of request: a second send before the first is
 * answered is refused at once (KODE_DENIED), which is why each request here
 * is made from a state that has none in flight. The door opens just before
 * the answer is posted, so the answer's handler may ask again.
 *
 * The news - the module found, lost, a setting lost - are signals this AO
 * subscribed to with mesh_sys_subscribe(); they come with no request.
 *
 * ---- The queue, by mesh_sys.h's rule --------------------------------------
 *
 * An answer is posted with no margin, so this AO's queue (KQP_AO_QUEUE_LEN,
 * 16) holds everything that can wait in it at once: one request in flight
 * (the begin or the send, never both), the interval's timer (1), a press and
 * its release (2), and the news (posted with a margin, never the overflow).
 * Five of sixteen.
 */
#include "send_ao.h"

#include <stdio.h>

#include "mesh_sys.h"

#define TAG "send"

/** Between two packets. At SF9 and BW125 a short packet is on air for well
 *  under half a second, so a minute is far inside the 1 % the band allows
 *  (about 36 s of air an hour). The SDK is what enforces the 1 %; this is
 *  what keeps the example polite. */
#define SEND_INTERVAL_MS    60000U

/** From a press of the top button (or the radio set again while sending) to the packet. */
#define SEND_START_MS       100U

/** Added to what the SDK says the duty budget needs, so the retry is after it. */
#define SEND_DUTY_MARGIN_MS 100U

/** 868 MHz, SF9, at the region's cap. */
#define SEND_SPREADING_FACTOR 9U

/** The payload: "kode mesh " and a counter. */
#define SEND_PAYLOAD_MAX    24U

/** @brief This AO's private signals. Posted to itself, never published. They are the app's own
 *         range, from MAX_SIG; the SDK's start at MESH_SIG_FIRST. */
enum SendSignals {
    SEND_NEXT_SIG = MAX_SIG   /**< Time for the next packet. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    QTimeEvt next;      /**< One-shot, armed after each send or refusal. */
    bool     on;        /**< The top button started the packets.         */
    uint32_t counter;   /**< Packets put on air since the AO started.    */
} Send;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Send l_ao;
QActive * const AO_Send = &l_ao.super;

static QState Send_initial(Send * const me, void const * const par);
static QState Send_waiting(Send * const me, QEvt const * const e);
static QState Send_setting_up(Send * const me, QEvt const * const e);
static QState Send_ready(Send * const me, QEvt const * const e);
static QState Send_sending(Send * const me, QEvt const * const e);
static QState Send_away(Send * const me, QEvt const * const e);

/**
 * @brief Ask for the band: 868 MHz, SF9, the region's power cap. Nothing goes on air.
 * @param me The AO.
 * @return true when the request was taken and its answer will come.
 */
static bool ask_band(Send * const me)
{
    kode_status_t const rc = mesh_sys_radio_begin(&me->super, MESH_BAND_SUB_GHZ, SEND_SPREADING_FACTOR,
                                                  MESH_REGION_SUB_GHZ_DBM_X10);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the band request was not taken (status %d)", (int)rc);
    }
    return KODE_OK == rc;
}

/**
 * @brief Whether @p e is the press of @p which.
 * @param e     A BUTTON_EVT_SIG event.
 * @param which A KODE_SYS_BUTTON_x.
 * @return true for the press, false for a release or another button.
 */
static bool pressed(QEvt const * const e, uint32_t const which)
{
    ButtonEvt const * const be = (ButtonEvt const *)e;

    return be->pressed && (which == (uint32_t)be->id);
}

void Send_ctor(void)
{
    Send * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Send_initial));
    QTimeEvt_ctorX(&me->next, &me->super, (enum_t)SEND_NEXT_SIG, 0U);
    me->on      = false;
    me->counter = 0U;
}

void Send_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts. */
    QActive_setAttr(AO_Send, TASK_NAME_ATTR, "send");
    QACTIVE_START(AO_Send, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: the buttons, the SDK, the news.
 *
 * Runs in the thread that called QACTIVE_START(), the one that called kode_init().
 * mesh_sys_begin() switches J3's supply on and starts the owner; it does not wait for a module.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Send_waiting.
 */
static QState Send_initial(Send * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    kode_status_t const rc = mesh_sys_begin();
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the SDK did not start (status %d)", (int)rc);
    }
    (void)mesh_sys_subscribe(&me->super);

    KLOG_W(TAG, "this example TRANSMITS on 868 MHz: an antenna or a 50 ohm load must be on J501");
    KLOG_I(TAG, "up; looking for the Mesh on J3");
    return Q_TRAN(&Send_waiting);
}

/**
 * @brief No module: the SDK looks for one and says when it finds it.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() once the module is found, Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Send_waiting(Send * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_MODULE_FOUND_SIG: {
            KLOG_I(TAG, "found; setting 868 MHz at SF%u, power at the region's cap",
                   (unsigned)SEND_SPREADING_FACTOR);
            if ( ask_band(me) )
            {
                status = Q_TRAN(&Send_setting_up);
            }
            break;
        }
        case MESH_WRONG_MODULE_SIG: {
            KLOG_W(TAG, "J3 holds a module that is not a Mesh");
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
 * @brief The band is asked for; the answer says whether it is set.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to ready, or back to waiting; Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Send_setting_up(Send * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_RADIO_BEGIN_RSP_SIG: {
            mesh_sys_answer_t const * const a = Q_EVT_CAST(mesh_sys_answer_t);

            if ( KODE_OK == a->rc )
            {
                if ( me->on )
                {
                    KLOG_I(TAG, "radio set again; sending goes on");
                    (void)QTimeEvt_armX(&me->next, KQP_TICKS_PER_MS(SEND_START_MS), 0U);
                }
                else
                {
                    KLOG_W(TAG, "radio set, NOTHING SENT: press the top button to send a packet every %u s "
                                "(with the antenna or the load on J501); press again to stop",
                           (unsigned)(SEND_INTERVAL_MS / 1000U));
                }
                status = Q_TRAN(&Send_ready);
            }
            else
            {
                KLOG_W(TAG, "the band was refused: %s; waiting for the module", mesh_sys_why_text(a->why));
                status = Q_TRAN(&Send_waiting);
            }
            break;
        }
        case MESH_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the Mesh left J3");
            status = Q_TRAN(&Send_waiting);
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
 * @brief The radio is set: the top button starts and stops the packets, and each turn of the
 *        timer is one.
 *
 * Also the parent of "sending": what is true in both - the module leaving, the setting lost, the
 * button - is handled here once.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() when a packet is asked for, the module leaves or the radio must be set again,
 *         Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Send_ready(Send * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case SEND_NEXT_SIG: {
            uint32_t const wait = mesh_sys_radio_send_wait_ms();

            if ( !mesh_sys_is_present() )
            {
                /* The lost news may have been dropped by a full queue: the module is what is asked. */
                KLOG_W(TAG, "the Mesh left J3; the SDK keeps the band for when it is back");
                me->on = false;
                status = Q_TRAN(&Send_away);
            }
            else if ( 0U != wait )
            {
                KLOG_I(TAG, "the duty cycle has no room yet; holding off %lu ms", (unsigned long)wait);
                (void)QTimeEvt_armX(&me->next, KQP_TICKS_PER_MS(wait + SEND_DUTY_MARGIN_MS), 0U);
            }
            else
            {
                /* The SDK copies the bytes before the call returns: this buffer is ours again at once. */
                uint8_t payload[SEND_PAYLOAD_MAX];
                int const n = snprintf((char *)payload, sizeof(payload), "kode mesh %lu",
                                       (unsigned long)me->counter);
                kode_status_t const rc = (n > 0) ? mesh_sys_radio_send(&me->super, payload, (uint8_t)n)
                                                 : KODE_DENIED;
                if ( KODE_OK == rc )
                {
                    status = Q_TRAN(&Send_sending);
                }
                else
                {
                    KLOG_W(TAG, "the send was not taken (status %d); trying again at the next turn", (int)rc);
                    (void)QTimeEvt_armX(&me->next, KQP_TICKS_PER_MS(SEND_INTERVAL_MS), 0U);
                }
            }
            break;
        }
        case BUTTON_EVT_SIG: {
            if ( pressed(e, KODE_SYS_BUTTON_TOP) )
            {
                me->on = !me->on;
                if ( me->on )
                {
                    /* The person pressed, with the antenna or the load on J501: the SDK's word on
                     * the antennas, and what clears an alarm it held the band at 0 dBm for. */
                    (void)mesh_sys_radio_antennas_confirm(true);
                    KLOG_W(TAG, "sending ON: a packet now, then one every %u s; the top button stops it",
                           (unsigned)(SEND_INTERVAL_MS / 1000U));
                    (void)QTimeEvt_armX(&me->next, KQP_TICKS_PER_MS(SEND_START_MS), 0U);
                }
                else
                {
                    KLOG_I(TAG, "sending OFF; the top button starts it again");
                    (void)QTimeEvt_disarm(&me->next);
                }
            }
            break;
        }
        case MESH_SETTING_LOST_SIG: {
            /* The chip would not confirm standby and was reset, or the amplifier faulted: the
             * radio's setting is gone. Set it up again; the packets go on after it. */
            mesh_sys_news_t const * const n = Q_EVT_CAST(mesh_sys_news_t);

            if ( mesh_sys_is_present() && (0U != ((uint32_t)n->value & MESH_SETTING_RADIO)) )
            {
                /* (A module pulled out says it too, with the module gone: that is the away state's.) */
                KLOG_W(TAG, "the SDK lost the radio's setting; setting the band again");
                (void)QTimeEvt_disarm(&me->next);
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Send_setting_up);
                }
            }
            break;
        }
        case MESH_MODULE_LOST_SIG: {
            KLOG_W(TAG, "the Mesh left J3; the SDK keeps the band for when it is back");
            me->on = false;   /* a module that comes back is asked again, with a press */
            (void)QTimeEvt_disarm(&me->next);
            status = Q_TRAN(&Send_away);
            break;
        }
        case MESH_RADIO_FAILED_SIG: {
            mesh_sys_news_t const * const n = Q_EVT_CAST(mesh_sys_news_t);
            KLOG_W(TAG, "radio error: chip word 0x%04lX, reason %lu",
                   (unsigned long)((uint32_t)n->value & MESH_RADIO_ERROR_CHIP_MASK),
                   (unsigned long)((uint32_t)n->value >> 16));
            if ( MESH_RADIO_ERROR_AMPLIFIER_FAULT == ((uint32_t)n->value & ~MESH_RADIO_ERROR_CHIP_MASK) )
            {
                KLOG_W(TAG, "the amplifier tripped its alarm: is the antenna on J501 loose? The SDK holds the "
                            "band at %d dBm until the top button is pressed again", (int)(MESH_REGION_SAFE_DBM_X10 / 10));
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
 * @brief A packet is asked for: the answer comes when it is gone, or refused.
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to ready once answered, Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Send_sending(Send * const me, QEvt const * const e)
{
    QState status = Q_SUPER(&Send_ready);

    switch ( e->sig )
    {
        case MESH_RADIO_SEND_RSP_SIG: {
            mesh_sys_send_answer_t const * const a = Q_EVT_CAST(mesh_sys_send_answer_t);
            uint32_t again = SEND_INTERVAL_MS;

            if ( KODE_OK == a->rc )
            {
                KLOG_I(TAG, "packet %lu left whole: %lu ms on air", (unsigned long)me->counter,
                       (unsigned long)a->airtime_ms);
                me->counter++;
            }
            else if ( (KODE_NO_RESOURCES == a->rc) && (MESH_WHY_DUTY == a->why) )
            {
                /* The 1 % budget has no room: the answer says for how long. */
                KLOG_I(TAG, "the duty cycle has no room; %lu ms to go", (unsigned long)a->wait_ms);
                again = a->wait_ms + SEND_DUTY_MARGIN_MS;
            }
            else
            {
                KLOG_W(TAG, "the send did not go: %s (status %d)", mesh_sys_why_text(a->why), (int)a->rc);
                if ( MESH_WHY_NOT_BEGUN == a->why )
                {
                    /* No setting at all: set the band up, then go on. */
                    (void)QTimeEvt_disarm(&me->next);
                    status = ask_band(me) ? Q_TRAN(&Send_setting_up) : Q_TRAN(&Send_ready);
                    break;
                }
            }
            if ( me->on )
            {
                (void)QTimeEvt_armX(&me->next, KQP_TICKS_PER_MS(again), 0U);
            }
            status = Q_TRAN(&Send_ready);
            break;
        }
        case SEND_NEXT_SIG: {
            /* A turn of the timer while a packet is still on its way: the answer arms the next. */
            status = Q_HANDLED();
            break;
        }
        default: {
            break;   /* the parent's: the module leaving, the setting lost, the button */
        }
    }

    return status;
}

/**
 * @brief The module is out: the SDK keeps the band and puts it back when the module returns.
 *
 * Nothing is asked here, and nothing is sent: the packets are off until the top button says
 * otherwise, with the antenna on. MESH_SETTING_RESTORED_SIG says the band is back; if the SDK
 * could not, MESH_SETTING_LOST_SIG says that, with the module present, and the band is asked again.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() to ready or to the setting up, Q_HANDLED() or Q_SUPER() otherwise.
 */
static QState Send_away(Send * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case MESH_SETTING_RESTORED_SIG: {
            mesh_sys_news_t const * const n = Q_EVT_CAST(mesh_sys_news_t);

            if ( 0U != ((uint32_t)n->value & MESH_SETTING_RADIO) )
            {
                KLOG_W(TAG, "the Mesh is back and the SDK set the band again, NOTHING SENT: press the top "
                            "button to send (with the antenna or the load on J501)");
                status = Q_TRAN(&Send_ready);
            }
            break;
        }
        case MESH_SETTING_LOST_SIG: {
            if ( mesh_sys_is_present() )
            {
                KLOG_W(TAG, "the module is back but the band could not be put on it; setting it again");
                if ( ask_band(me) )
                {
                    status = Q_TRAN(&Send_setting_up);
                }
            }
            break;
        }
        case MESH_MODULE_FOUND_SIG:
        case MESH_MODULE_LOST_SIG: {
            break;
        }
        case BUTTON_EVT_SIG: {
            break;   /* no module: nothing to start */
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
