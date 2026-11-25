/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief The target build's app: it links every call of both layers, and starts AO_Mesh.
 *
 * Compiling and linking for the esp32p4 is the whole check. Every call of mesh.h (a task's) and of
 * mesh_sys.h (an Active Object's) is named in ALL so that each one, and everything below it -
 * AO_Mesh, the LR2021 and GNSS drivers, the board facts - is linked here rather than first in
 * somebody's app. Flashed on a Dot, it also starts looking for the module and says when it finds
 * it. It never transmits: it opens no band and calls no mesh_radio_test(), so the amplifiers stay
 * off.
 */
#include "kode.h"
#include "mesh.h"
#include "mesh_sys.h"

#define TAG "mesh_target"

__attribute__((used)) static const void *const ALL[] = {
    /* mesh.h: the standard layer */
    (const void *)mesh_error,               (const void *)mesh_begin,
    (const void *)mesh_end,                 (const void *)mesh_is_present,
    (const void *)mesh_wait_present,        (const void *)mesh_status,
    (const void *)mesh_stop,                (const void *)mesh_header_position,
    (const void *)mesh_event_add_listener,  (const void *)mesh_event_remove_listener,
    (const void *)mesh_event_wait,          (const void *)mesh_radio_antennas_confirm,
    (const void *)mesh_radio_begin,
    (const void *)mesh_radio_config_default, (const void *)mesh_radio_configure,
    (const void *)mesh_radio_calibrate,     (const void *)mesh_radio_retune,
    (const void *)mesh_radio_end,           (const void *)mesh_radio_standby,
    (const void *)mesh_radio_send,          (const void *)mesh_radio_send_preamble,
    (const void *)mesh_radio_send_wait_ms,  (const void *)mesh_radio_receive,
    (const void *)mesh_radio_read,          (const void *)mesh_radio_rssi,
    (const void *)mesh_radio_channel_busy,  (const void *)mesh_radio_cad,
    (const void *)mesh_radio_cad_receive,
    (const void *)mesh_radio_listen,        (const void *)mesh_radio_test,
    (const void *)mesh_gnss_begin,          (const void *)mesh_gnss_end,
    (const void *)mesh_gnss_reset,          (const void *)mesh_gnss_fix,
    (const void *)mesh_gnss_pps,            (const void *)mesh_i2c_lend,
    (const void *)mesh_i2c_return,

    /* mesh_sys.h: the extended layer */
    (const void *)mesh_sys_begin,           (const void *)mesh_sys_end,
    (const void *)mesh_sys_subscribe,       (const void *)mesh_sys_unsubscribe,
    (const void *)mesh_sys_radio_standby,   (const void *)mesh_sys_radio_receive,
    (const void *)mesh_sys_radio_listen,    (const void *)mesh_sys_stop,
    (const void *)mesh_sys_radio_antennas_confirm,
    (const void *)mesh_sys_radio_begin,     (const void *)mesh_sys_radio_configure,
    (const void *)mesh_sys_radio_calibrate, (const void *)mesh_sys_radio_retune,
    (const void *)mesh_sys_radio_end,       (const void *)mesh_sys_radio_send,
    (const void *)mesh_sys_radio_send_preamble, (const void *)mesh_sys_radio_cad,
    (const void *)mesh_sys_radio_cad_receive,
    (const void *)mesh_sys_radio_test,      (const void *)mesh_sys_gnss_begin,
    (const void *)mesh_sys_gnss_end,        (const void *)mesh_sys_gnss_reset,
    (const void *)mesh_sys_i2c_lend,        (const void *)mesh_sys_i2c_return,
    (const void *)mesh_sys_is_present,      (const void *)mesh_sys_status_read,
    (const void *)mesh_sys_header_position, (const void *)mesh_sys_why_text,
    (const void *)mesh_sys_radio_read,      (const void *)mesh_sys_radio_rssi,
    (const void *)mesh_sys_radio_channel_busy, (const void *)mesh_sys_radio_send_wait_ms,
    (const void *)mesh_sys_radio_config_default, (const void *)mesh_sys_gnss_fix,
    (const void *)mesh_sys_gnss_pps,
};

void app_main(void)
{
    kode_init();
    if (!mesh_begin()) {
        KLOG_E(TAG, "AO_Mesh could not start: %s", mesh_error());
        return;
    }
    KLOG_I(TAG, "%u calls linked; looking for a Mesh on J3", (unsigned)(sizeof(ALL) / sizeof(ALL[0])));
    if (mesh_wait_present(5000U)) {
        KLOG_I(TAG, "found: the radio is in standby, nothing transmits");
    } else {
        KLOG_W(TAG, "no Mesh on J3: %s", mesh_error());
    }
}
