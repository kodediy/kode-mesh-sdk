/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_main.c
 * @brief Mesh example: the module on kodeOS's console, from an Active Object.
 *
 * WHAT IT TEACHES. How to give the module console rows (KODE_FUNCTIONS_ADD() and
 * KODE_VARIABLES_ADD()) that a person, BLE or kode-cli can type: twelve functions
 * and twenty-three variables. A row runs on the console's task and must not block,
 * so a function either calls a mesh_sys.h command or immediate itself, or posts a
 * request to this example's AO, which asks the SDK and logs the answer; a variable
 * reads mesh_sys_status_read().
 *
 * WHAT IT NEEDS. The Mesh module on J3 and the Dot on a USB-C cable. For the two rows
 * that transmit (mesh_radio_send, mesh_radio_test): an antenna or a 50 ohm load on
 * the SMA of the band in use, and mesh_tx_arm given 1 first. mesh_radio_test goes
 * into a 50 ohm load or an analyser ONLY, never an antenna, and only a build with
 * CONFIG_MESH_LAB has it: as this example is built, the SDK refuses it and says so.
 *
 * WHAT YOU SEE. On the console (the USB-C cable): `function name="mesh_begin"
 * arg=sub_ghz`, then `function name="mesh_rx" arg=on`, and `variable
 * name="mesh_rx_count" watch=500` follows the packets; each answer is `>ok` and what
 * came of it is on the log and in the variable mesh_last. Everything is listed in
 * console_actions_ao.c.
 *
 * The same three lines as every example in this directory: kode_init() brings
 * kodeOS up, then the example's own Active Object is constructed and started.
 */
#include "kode_sys.h"

#include "console_actions_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Mesh console", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    ConsoleActions_ctor();
    ConsoleActions_start();
}
