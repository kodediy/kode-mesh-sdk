/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_console.h
 * @brief kodeOS's console registry, as a case drives it: tables refused on
 *        the registry's rules, rows run and read the way the console would.
 */
#ifndef FAKE_CONSOLE_H_
#define FAKE_CONSOLE_H_

#include "kode_sys.h"

/** @brief No table registered. */
void fake_console_reset(void);

/** @brief Rows registered, all tables. */
size_t fake_console_functions(void);
size_t fake_console_variables(void);

/** @brief The function row named @p name, as registered (function_list's line); NULL if none. */
const kode_function_t *fake_console_function(const char *name);

/**
 * @brief `function name=@p name [arg=]`: the row found, its argument checked
 *        as the console checks it (type, range when min != max, a string's
 *        length), then run.
 * @return KODE_DENIED for an unknown row or an argument the console refuses
 *         before the row sees it; otherwise what the row answers.
 */
kode_status_t fake_console_run(const char *name, const kode_value_t *arg);

/**
 * @brief `variable name=@p name`: the getter into a zeroed buffer, and its
 *        word kept only if the console would send it (1..32 characters).
 * @return KODE_DENIED for an unknown row or a word the console refuses;
 *         otherwise the getter's status, with @p word filled on KODE_OK.
 */
kode_status_t fake_console_read(const char *name, char word[KODE_VALUE_STR_MAX + 1U]);

#endif /* FAKE_CONSOLE_H_ */
