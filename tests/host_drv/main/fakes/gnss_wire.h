/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gnss_wire.h
 * @brief The GNSS receiver's UART for the driver host suite: what the driver wrote, what the
 *        receiver says, and the rate. A model of the wire only; whether the receiver sleeps is
 *        the case's to say (it speaks with gnss_wire_say() or keeps quiet).
 */
#ifndef GNSS_WIRE_H_
#define GNSS_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_WIRE_TX_MAX 256U   /**< Bytes of what the driver wrote that are kept. */

/** @brief The UART as the driver left it. */
typedef struct {
    bool     installed;           /**< uart_driver_install() and no delete since. */
    uint32_t baud;                /**< The rate set last.                         */
    uint8_t  tx[GNSS_WIRE_TX_MAX];/**< Everything written, in order.              */
    size_t   tx_n;
    unsigned tx_waits;            /**< uart_wait_tx_done() calls.                 */
    bool     write_fails;         /**< uart_write_bytes() writes nothing.         */
} gnss_wire_t;

extern gnss_wire_t g_wire;

/** @brief A fresh wire: nothing written, nothing to read. */
void gnss_wire_reset(void);

/** @brief The receiver says @p body as one sentence: "$<body>*<xor>\r\n", checksum added. */
void gnss_wire_say(const char *body);

/** @brief The receiver says @p bytes as they are: half a sentence, or its tail. */
void gnss_wire_raw(const char *bytes);

#ifdef __cplusplus
}
#endif

#endif /* GNSS_WIRE_H_ */
