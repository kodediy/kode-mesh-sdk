/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_pins.h
 * @brief The three header lines the owner needs by hand, for the look.
 *
 * Finding the module means reading its pull-up resistors: the owner drives a
 * position with the P4's own pull-down and sees whether the module's pull-up
 * still wins. That is all this does - set a borrowed GPIO to high-Z or to a
 * pull-down, and read its level - through ESP-IDF's GPIO driver, the one place
 * that may (tools/check_layers.sh, rule 1). The GPIOs are the header's and
 * come from kode_sys_exp_lend(); this never decides which.
 */
#ifndef MESH_DRV_PINS_H_
#define MESH_DRV_PINS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Leave @p gpio an input with no pull: it floats to what drives it. */
void mesh_drv_pins_high_impedance(uint8_t gpio);

/** @brief Make @p gpio an input the P4 pulls down, to read a module's pull-up. */
void mesh_drv_pins_pull_down(uint8_t gpio);

/** @brief The level of @p gpio now: true is high. */
bool mesh_drv_pins_level(uint8_t gpio);

#ifdef __cplusplus
}
#endif

#endif /* MESH_DRV_PINS_H_ */
