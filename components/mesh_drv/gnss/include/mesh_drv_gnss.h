/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_gnss.h
 * @brief The Mesh module's GNSS receiver: a Minew ME31GR02 on a UART.
 *
 * The receiver streams NMEA and this reads it into a fix - where, how high,
 * how fast, how many satellites, the UTC time and date - with mesh_drv_nmea.c. It
 * is reset once and listened to, and told one thing: to sleep (the datasheet's
 * binary Sleep command, mesh_drv_gnss_sleep()), from which a byte on its RXD
 * wakes it (mesh_drv_gnss_wake()).
 * The one subtlety is the rate: the ME31GR02 talks at 115200 baud, and a
 * receiver at 9600 is recognised too, so while nothing has checked the driver
 * moves between the two and the caller need not know which it is.
 *
 * The I2C pair (GPIO15/16) is the receiver's second face and this driver never
 * touches it: the stock firmware drives SDA push-pull, so mastering that bus
 * fights it (MESH_BSP_GNSS_I2C_RISK). The pair is the app's to take, at its own
 * risk, through the facade.
 *
 * mesh_drv_gnss_poll() is the only call that reads the UART, and one task makes it;
 * mesh_drv_gnss_read() and mesh_drv_gnss_pps() may be called from any task. The pins are
 * the header's and arrive as GPIO numbers from whoever borrowed them.
 */
#ifndef MESH_DRV_GNSS_H_
#define MESH_DRV_GNSS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#include "mesh_drv_nmea.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The GPIOs the receiver is wired to (the J3 positions' GPIOs). */
typedef struct {
    uint8_t uart_port; /**< The UART, IDF's uart_port_t, as kode_sys_exp_ctrl() gave it. */
    uint8_t rxd;    /**< The receiver's TXD: the P4 listens here.          */
    uint8_t txd;    /**< The receiver's RXD: the P4 speaks, held idle.     */
    uint8_t reset;  /**< The receiver's reset, active low.                 */
    uint8_t pps;    /**< The one-pulse-a-second line; read, never driven.  */
} mesh_drv_gnss_pins_t;

/** @brief What the receiver has said since the counts were last zeroed. */
typedef struct {
    mesh_drv_nmea_fix_t fix;        /**< The latest fix (mesh_drv_nmea.h).                 */
    uint32_t  messages;   /**< NMEA sentences with a good checksum.         */
    uint32_t  bad;        /**< Sentences with a bad or missing checksum.    */
    uint32_t  baud;       /**< The rate a sentence checked at; 0 until one
                           *   has.                                         */
    bool      talking;    /**< A sentence has checked: the receiver is on
                           *   and at a rate the driver found. False while
                           *   it sleeps, and after a wake until a sentence
                           *   checks again.                                */
    bool      asleep;     /**< It was told to sleep and has not spoken since
                           *   (after MESH_BSP_GNSS_SLEEP_MS a sentence
                           *   means it did not sleep, and this falls).     */
} mesh_drv_gnss_status_t;

/**
 * @brief Take the pins, restart the receiver and open its UART.
 *
 * The reset is held low while the UART comes up, so what the receiver says on
 * the way out of reset is heard from its first character.
 *
 * @param pins The GPIOs; copied.
 * @return KODE_OK; KODE_DENIED for NULL or a driver already open; KODE_ERROR
 *         when the UART or a pin would not come up.
 */
kode_status_t mesh_drv_gnss_open(const mesh_drv_gnss_pins_t *pins);

/** @brief Close the UART and give the pins back, high-Z. */
void mesh_drv_gnss_close(void);

/**
 * @brief Put the receiver to sleep with the datasheet's Sleep command.
 *
 * The 16 bytes of MESH_BSP_GNSS_SLEEP_FRAME go out and the call waits until
 * they have left the UART. The UART stays open and the lines stay as they
 * are - the P4's TXD idle high, RESET released - so nothing wakes it but the
 * next byte sent, and the rate found is kept. Polling goes on: a sentence that
 * checks more than MESH_BSP_GNSS_SLEEP_MS after the command means the
 * receiver did not sleep, which is logged and ends the sleep.
 *
 * @return KODE_OK; KODE_DENIED when closed, already asleep, or not a receiver
 *         that has talked at MESH_BSP_GNSS_BAUD (a ME31GR02): one at 9600
 *         is not sent the command, and a silent one cannot be told;
 *         KODE_IO_ERROR when the bytes would not go out.
 */
kode_status_t mesh_drv_gnss_sleep(void);

/**
 * @brief Wake a receiver mesh_drv_gnss_sleep() put to sleep: a byte on its RXD.
 *
 * Sends "\r\n" (an empty text command, if it is read as one) and returns. The
 * fix and the counts start again from zero, the rate is kept. mesh_drv_gnss_poll()
 * sends the bytes again when no sentence has checked within
 * MESH_BSP_GNSS_WAKE_MS, and after MESH_BSP_GNSS_WAKE_TRIES tries restarts the
 * receiver with its reset line.
 *
 * @return KODE_OK, also for a receiver that is awake; KODE_DENIED when closed;
 *         KODE_IO_ERROR when the bytes would not go out.
 */
kode_status_t mesh_drv_gnss_wake(void);

/**
 * @brief Pulse the reset line and start the stream over.
 *
 * The counts, the fix and the rate search begin again: a cold receiver is the
 * way to prove a fix is really being acquired rather than held from before.
 */
void mesh_drv_gnss_restart(void);

/**
 * @brief Read whatever the receiver has sent and parse it. Never blocks.
 *
 * Call it several times a second. While no sentence has checked it also moves
 * the UART between the two rates the module may talk at.
 */
void mesh_drv_gnss_poll(void);

/** @brief Start the message counts again from zero; the fix is kept. */
void mesh_drv_gnss_zero(void);

/**
 * @brief Take a consistent copy of the fix and counts.
 * @param[out] out Zeroed when the driver is closed.
 */
void mesh_drv_gnss_read(mesh_drv_gnss_status_t *out);

/** @brief The level of the 1PPS line now: true is high. False when closed. */
bool mesh_drv_gnss_pps(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_DRV_GNSS_H_ */
