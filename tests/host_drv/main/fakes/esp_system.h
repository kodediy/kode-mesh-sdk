/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_system.h
 * @brief esp_register_shutdown_handler() for the radio driver host suite:
 *        five slots, ESP_ERR_NO_MEM when full, ESP_ERR_INVALID_STATE for a
 *        handler already held - ESP-IDF's rules. shutdown_run() is
 *        esp_restart()'s part before the scheduler stops.
 */
#ifndef FAKE_ESP_SYSTEM_H_
#define FAKE_ESP_SYSTEM_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shutdown_handler_t)(void);

/* The linux build links ESP-IDF's own esp_register_shutdown_handler; the fake goes by
 * another name so that the linker has nothing to choose between. */
esp_err_t fake_register_shutdown_handler(shutdown_handler_t handle);
#define esp_register_shutdown_handler(handle) fake_register_shutdown_handler(handle)

/** @brief Run every registered handler, as esp_restart() does. */
void shutdown_run(void);

/** @brief Handlers registered now. */
unsigned shutdown_count(void);

/** @brief Make every slot look taken by someone else (or free them again). */
void shutdown_fill(bool full);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_SYSTEM_H_ */
