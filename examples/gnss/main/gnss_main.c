/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gnss_main.c
 * @brief Mesh example: the GNSS receiver on, its fix and its sentences logged, from a task.
 *
 * Wire: the Mesh module on J3 with a GNSS antenna on its receiver, and the Dot on a USB-C
 * cable, by a window or outside: a cold receiver can take minutes to its first fix, and
 * indoors it may never get one. This example touches no radio and NEVER TRANSMITS: a GNSS
 * receiver only listens.
 *
 * WHICH LAYER. mesh.h, the standard layer: the calls of a task, which WAIT for their answer
 * and say yes or no with a bool. Opening the receiver (mesh_gnss_begin()) asks the owner and
 * waits for it, so it belongs in a task: here app_main(), which is one. An Active Object has
 * mesh_sys_gnss_begin() instead, a request answered later by an event.
 *
 * WHAT IT TEACHES. Opening the GNSS and reading it without waiting: mesh_status() returns at
 * once with the fix the SDK has parsed from the receiver's NMEA stream, and mesh_gnss_fix()
 * says whether there is one (false, with a reason in mesh_error(), until there is). The I2C
 * pair is the receiver's second face and this example never masters it (mesh_i2c_lend() is
 * for an app that wants the pair). And what the SDK does by itself when the module is pulled
 * out and plugged in again: it opens the receiver again and says so (event
 * "setting_restored").
 *
 * WHAT YOU SEE. On the log, every two seconds: whether the receiver is talking, how many
 * sentences with a good checksum have arrived, the baud it locked at (the SDK looks for it
 * itself), the satellites in view and in use and, once there is a fix, its HDOP and the UTC
 * time. The position itself (mesh_gnss_fix(), the latitude and longitude) is there for an
 * app to use, but this example does not write it to the log: a log is read and kept by people
 * who were not told where the Dot was. A short press of the bottom button closes the receiver
 * and returns to kodeOS; holding it goes home to kodeOS too, as in every app
 * (CONFIG_KODEOS_HOME_HOLD).
 */
#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kode_log.h"
#include "mesh.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Mesh GNSS", "example");

#define TAG "gnss"

/** One wait for the module; refused, it is asked again. */
#define GNSS_WAIT_MS        5000U

/** How often the receiver's state is logged. */
#define GNSS_POLL_MS        2000U

/**
 * @brief A listener: the SDK put the receiver back after the module returned or the board woke.
 *
 * Called on the SDK's task "mesh_events"; it only logs.
 *
 * @param event Which event; valid until this returns.
 */
static void on_restored(const mesh_event_t *event)
{
    KLOG_I(TAG, "the SDK opened the receiver again (mask 0x%lx)", (unsigned long)event->value);
}

/**
 * @brief The receiver's state: sentences, baud, satellites, and a fix's quality and time.
 *        Never its place.
 * @param st The status.
 */
static void show(const mesh_status_t *st)
{
    const mesh_gnss_fix_t *f = &st->fix;

    if ( !st->gnss_talking )
    {
        KLOG_I(TAG, "no sentences yet (%lu good so far): the SDK is looking for the receiver's baud",
               (unsigned long)st->gnss_sentences);
    }
    else if ( f->valid )
    {
        KLOG_I(TAG, "fix: %u satellites used of %u in view, HDOP %.1f, %.1f kn", (unsigned)f->satellites_used,
               (unsigned)f->satellites_in_view, (double)f->horizontal_dilution, (double)f->speed_knots);
    }
    else
    {
        KLOG_I(TAG, "no fix yet: %u satellites in view, %u used", (unsigned)f->satellites_in_view,
               (unsigned)f->satellites_used);
    }
    if ( st->gnss_talking )
    {
        KLOG_I(TAG, "%lu sentences with a good checksum at %lu baud, 1PPS %s",
               (unsigned long)st->gnss_sentences, (unsigned long)st->gnss_baud,
               st->pps_high ? "high" : "low");
    }
    if ( 0U != f->time_utc )
    {
        KLOG_I(TAG, "UTC %02lu:%02lu:%02lu%s", (unsigned long)(f->time_utc / 10000U),
               (unsigned long)((f->time_utc / 100U) % 100U), (unsigned long)(f->time_utc % 100U),
               (0U != f->date) ? " (dated)" : "");
    }
}

void app_main(void)
{
    mesh_status_t st;
    mesh_gnss_fix_t fix;

    /* First, always: brings up logging, QF, the event pools and the core Active Objects. */
    kode_init();

    (void)mesh_event_add_listener("setting_restored", &on_restored);
    if ( !mesh_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", mesh_error());
        return;
    }
    KLOG_I(TAG, "up; looking for the Mesh on J3");

    /* Blocks until the module is found, a few seconds at a time, or the bottom button is pressed. */
    (void)kode_button_was_pressed("bottom");
    while ( !mesh_wait_present(GNSS_WAIT_MS) )
    {
        if ( kode_button_was_pressed("bottom") )
        {
            (void)mesh_end();
            kode_exit();
            return;
        }
        KLOG_I(TAG, "no Mesh yet");
    }

    KLOG_I(TAG, "found; opening the receiver");
    if ( !mesh_gnss_begin() )
    {
        KLOG_E(TAG, "the receiver did not open: %s", mesh_error());
    }
    else
    {
        KLOG_I(TAG, "the receiver is open; its state every %u s", (unsigned)(GNSS_POLL_MS / 1000U));
        while ( !kode_button_was_pressed("bottom") )
        {
            kode_delay(GNSS_POLL_MS);
            if ( !mesh_status(&st) )
            {
                KLOG_W(TAG, "no status: %s", mesh_error());
            }
            else if ( !st.present )
            {
                KLOG_W(TAG, "the Mesh left J3; the SDK opens the receiver again when it is back");
            }
            else
            {
                show(&st);
                if ( mesh_gnss_fix(&fix) )
                {
                    /* A position is there for the app. It is not logged. */
                    KLOG_I(TAG, "mesh_gnss_fix(): a fix, %lu ms old", (unsigned long)fix.age_ms);
                }
            }
        }
        (void)mesh_gnss_end();
    }

    (void)mesh_event_remove_listener("setting_restored", &on_restored);
    (void)mesh_end();
    KLOG_I(TAG, "J3 let go; leaving");
    kode_exit();
}
