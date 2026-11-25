/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file receive_main.c
 * @brief Mesh example: the receiver on 868 MHz, every packet on the log.
 *
 * WHICH LAYER. mesh_sys.h, the extended layer: the calls of an Active Object, none of
 * which waits. The band is a REQUEST (mesh_sys_radio_begin(), answered later by an event
 * posted to this AO), the receiver a COMMAND (mesh_sys_radio_receive(), posted, no answer),
 * and what happens to the module - found, lost, a packet landed - arrives as NEWS the AO
 * subscribed to. The same module, from a task, is mesh.h (see the module example).
 *
 * WHAT IT TEACHES. Listening from an Active Object: setting the radio up with a request,
 * turning the receiver on with a command, reading each packet with mesh_sys_radio_read()
 * when MESH_PACKET_RECEIVED_SIG says one landed, and how the setting comes back by itself
 * after the module is pulled out and plugged in again (MESH_SETTING_RESTORED_SIG).
 *
 * WHAT IT NEEDS. The Mesh module on J3 and the Dot on a USB-C cable. An antenna on
 * the sub-GHz SMA (J501) to hear beyond the room; it runs without one. It NEVER
 * TRANSMITS: the amplifier is not keyed. Another Dot running the send example (or
 * any LoRa node on 868.3 MHz, SF9, BW125) is what it listens to.
 *
 * WHAT YOU SEE. On the log, for every packet: its length, its RSSI, its SNR and
 * whether its CRC matched (a packet with a wrong CRC is delivered all the same, and
 * logged as such). The payload is never logged. Pull the module out and the log says
 * so; put it back and the SDK sets the band and the receiver again.
 *
 * The same three lines as every example in this directory: kode_init() brings
 * kodeOS up, then the example's own Active Object is constructed and started.
 * Everything worth reading is in receive_ao.c.
 */
#include "kode_sys.h"

#include "receive_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Mesh receive", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    Receive_ctor();
    Receive_start();
}
