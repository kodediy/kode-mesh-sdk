/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.h
 * @brief The example's own Active Object: the Mesh module on kodeOS's console.
 */
#ifndef CONSOLE_ACTIONS_AO_H_
#define CONSOLE_ACTIONS_AO_H_

#include "mesh_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside console_actions_ao.c can reach its
 *         members. */
extern QActive * const AO_ConsoleActions;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before ConsoleActions_start(). It only
 * initialises memory: nothing is posted until the initial transition runs.
 */
void ConsoleActions_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, let it run, and put the
 *        module's rows on the console.
 *
 * The initial transition runs here, in the caller's thread - the one that
 * called kode_init(), which is where mesh_sys_begin() is called from.
 */
void ConsoleActions_start(void);

#endif /* CONSOLE_ACTIONS_AO_H_ */
