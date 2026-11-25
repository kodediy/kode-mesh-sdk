/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file module_main.c
 * @brief Mesh example: the module found on J3, and its status on the log, from a task.
 *
 * WHICH LAYER. mesh.h, the standard layer: the calls of a task, which WAIT for their answer
 * and say yes or no with a bool (mesh_error() says why). app_main() is a task, so it may call
 * them; an Active Object may not (it is refused, by name), and has mesh_sys.h instead - see
 * the send, receive and console_actions examples. Both layers go through the same owner, so
 * the two can be used in one app.
 *
 * WHAT IT TEACHES. The first thing every app of this SDK does: mesh_begin() starts the SDK's
 * own Active Object (AO_Mesh) and looks for the module by its fingerprint; then the module
 * is waited for (mesh_wait_present()), and what happens to it is heard by name, in the two
 * ways an event can be: a LISTENER (mesh_event_add_listener(), called on the SDK's own task
 * "mesh_events", so it must be short) and a WAIT (mesh_event_wait(), which blocks the caller
 * until the event comes or the time runs out). mesh_status() and mesh_is_present() are the
 * reads that return at once. Nothing here transmits.
 *
 * WHAT IT NEEDS. The Mesh module on J3 and the Dot on a USB-C cable. No antenna: no band is
 * set and the amplifier is never keyed. CONFIG_KODEOS_RADIO=y (J3's supply is switched by
 * the Dot's C5), as its sdkconfig.defaults has it.
 *
 * WHAT YOU SEE. On the log: the module found, then every five seconds what the SDK knows of
 * it (the radio, the packet counts, the GNSS). Pull the module out and the log says it left;
 * put it back and it is found again. With nothing on J3, or a module that is not a Mesh, the
 * log says what J3 holds instead. A short press of the bottom button ends the SDK and
 * returns to kodeOS; holding it goes home to kodeOS too, as in every app
 * (CONFIG_KODEOS_HOME_HOLD).
 *
 * kode_init() first, always: it brings kodeOS up, and mesh_begin() needs the pools it makes.
 */
#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kode_log.h"
#include "mesh.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Mesh module", "example");

#define TAG "module"

/** How long one turn of the loop waits for the module or for it to leave. */
#define MODULE_TURN_MS    1000U

/** How many turns between two status lines: five seconds. */
#define MODULE_STATUS_TURNS  5U

/**
 * @brief A listener: called on the SDK's task "mesh_events", one event at a time.
 *
 * It only logs. A listener that blocks holds up every event behind it, and the SDK's queue of
 * 16 loses the ones that do not fit (the status counts them in events_dropped).
 *
 * @param event Which event, its number and its words; valid until this returns.
 */
static void on_event(const mesh_event_t *event)
{
    KLOG_I(TAG, "event '%s' (%ld)%s%s", event->name, (long)event->value,
           (event->text[0] != '\0') ? ": " : "", event->text);
}

/**
 * @brief What J3 holds while no Mesh is on it.
 * @param seen The SDK's reading.
 * @return A phrase for the log.
 */
static const char *seen_word(mesh_module_t seen)
{
    const char *word = "nothing reads high (no module, or one without 3V3)";

    if ( MESH_MODULE_OTHER == seen )
    {
        word = "a module that is not a Mesh";
    }
    else if ( MESH_MODULE_MESH == seen )
    {
        word = "a Mesh that is not held yet";
    }
    return word;
}

/**
 * @brief The status, in three lines: the radio, the packets, the GNSS.
 * @param st The status.
 */
static void show_status(const mesh_status_t *st)
{
    if ( st->radio_ready )
    {
        KLOG_I(TAG, "radio: %s, SF%u, %.1f dBm asked, %.1f applied%s%s%s%s",
               (MESH_BAND_SUB_GHZ == st->band) ? "868 MHz on J501" : "2.4 GHz on J502",
               (unsigned)st->spreading_factor, (double)st->power_dbm_x10 / 10.0,
               (double)st->power_applied_dbm_x10 / 10.0,
               st->receiving ? ", receiving" : "", st->sending ? ", sending" : "",
               st->testing ? ", TEST SIGNAL ON AIR" : "",
               st->amplifier_fault ? ", AMPLIFIER FAULT" : "");
    }
    else
    {
        KLOG_I(TAG, "radio: no band set (this example never sets one)%s",
               st->amplifier_fault ? ", AMPLIFIER FAULT" : "");
    }
    KLOG_I(TAG, "packets: %lu sent, %lu received, %lu dropped, %u waiting; radio error word 0x%04X",
           (unsigned long)st->packets_sent, (unsigned long)st->packets_received,
           (unsigned long)st->packets_dropped, (unsigned)st->packets_waiting,
           (unsigned)st->radio_errors);
    if ( !st->gnss_on )
    {
        KLOG_I(TAG, "GNSS: off (this example never opens it)");
    }
    else
    {
        KLOG_I(TAG, "GNSS: %s, %lu sentences at %lu baud, %s",
               st->gnss_talking ? "talking" : "silent", (unsigned long)st->gnss_sentences,
               (unsigned long)st->gnss_baud, st->fix.valid ? "fix" : "no fix");
    }
}

void app_main(void)
{
    mesh_event_t event;
    mesh_status_t st;
    unsigned turn = 0U;

    /* First, always: brings up logging, QF, the event pools and the core Active Objects. */
    kode_init();

    /* The listeners first, so the module's first event cannot come before them. */
    (void)mesh_event_add_listener("module_found", &on_event);
    (void)mesh_event_add_listener("module_lost", &on_event);
    (void)mesh_event_add_listener("wrong_module", &on_event);

    /* Starts the owner and switches J3's supply on; it does not wait for a module. */
    if ( !mesh_begin() )
    {
        KLOG_E(TAG, "the SDK did not start: %s", mesh_error());
        return;
    }
    KLOG_I(TAG, "up; looking for the Mesh on J3");

    /* The press count is shared and starts at boot: the first ask clears any press made before. */
    (void)kode_button_was_pressed("bottom");
    while ( !kode_button_was_pressed("bottom") )
    {
        turn++;
        if ( !mesh_is_present() )
        {
            /* Blocks for up to a turn; true at once if it was there. The listener says it was found. */
            if ( !mesh_wait_present(MODULE_TURN_MS) && (0U == (turn % MODULE_STATUS_TURNS)) )
            {
                if ( mesh_status(&st) )
                {
                    KLOG_I(TAG, "no Mesh yet: J3 holds %s", seen_word(st.seen));
                }
            }
        }
        else if ( mesh_event_wait("module_lost", MODULE_TURN_MS, &event) )
        {
            /* The wait heard the module leave (the listener logged it as well). */
            KLOG_W(TAG, "the Mesh left J3");
        }
        else if ( (0U == (turn % MODULE_STATUS_TURNS)) && mesh_status(&st) )
        {
            show_status(&st);
        }
        else
        {
            /* A turn of nothing: the event did not come. */
        }
    }

    (void)mesh_event_remove_listener("module_found", &on_event);
    (void)mesh_event_remove_listener("module_lost", &on_event);
    (void)mesh_event_remove_listener("wrong_module", &on_event);
    if ( !mesh_end() )
    {
        KLOG_W(TAG, "the SDK did not end: %s", mesh_error());
    }
    KLOG_I(TAG, "J3 let go; leaving");
    kode_exit();
}
