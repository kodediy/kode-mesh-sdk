/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file send_ao.h
 * @brief The example's own Active Object: one short packet on 868 MHz, now
 *        and then. THIS ONE TRANSMITS.
 */
#ifndef SEND_AO_H_
#define SEND_AO_H_

#include "mesh_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside send_ao.c can reach its members. */
extern QActive * const AO_Send;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before Send_start(). It only initialises
 * memory: nothing is posted and the module is not looked for until the
 * initial transition runs.
 */
void Send_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * The initial transition runs here, in the caller's thread - the one that
 * called kode_init(), which is where mesh_sys_begin() is called from.
 */
void Send_start(void);

#endif /* SEND_AO_H_ */
