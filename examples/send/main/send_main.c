/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file send_main.c
 * @brief Mesh example: one short packet on 868 MHz every minute, from an Active Object.
 *
 * AN ANTENNA OR A 50 OHM LOAD ON J501 BEFORE RUNNING. 868 IS SHARED AND THE
 * DUTY CYCLE IS THE LAW. Read send_ao.c first: this example transmits.
 *
 * WHICH LAYER. mesh_sys.h, the extended layer: the calls of an Active Object, none of
 * which waits. A radio setting and a send are REQUESTS: each takes this AO, returns at once
 * and is answered later by an event posted to it (MESH_RADIO_BEGIN_RSP_SIG,
 * MESH_RADIO_SEND_RSP_SIG). The same module, from a task, is mesh.h (see the module
 * example).
 *
 * WHAT IT TEACHES. Putting a packet on air without ever blocking a step: one door per kind
 * of request, the answer that comes when the packet is gone, the airtime in it, and the
 * 1 % duty cycle of the sub-band kept by the SDK - a packet it has no budget for is
 * answered KODE_NO_RESOURCES with MESH_WHY_DUTY and the wait.
 *
 * WHAT IT NEEDS. The Mesh module on J3 with an antenna or a 50 ohm load on the
 * sub-GHz SMA (J501), and the Dot on a USB-C cable. Another Dot running the receive
 * example hears the packets.
 *
 * WHAT YOU SEE. On the log: the radio ready, and NOTHING SENT until the top button is
 * pressed; then a packet at once and one every minute, each logged when it has left,
 * with its time on air. A second press stops them. The power is the region's cap
 * (8.5 dBm); asking for more gets the same.
 *
 * The same three lines as the other two Active Object examples, receive and
 * console_actions: kode_init() brings kodeOS up, then the example's own Active
 * Object is constructed and started.
 */
#include "kode_sys.h"

#include "send_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Mesh send", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    Send_ctor();
    Send_start();
}
