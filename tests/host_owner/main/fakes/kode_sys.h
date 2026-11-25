/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.h
 * @brief The slice of kode_sys.h the owner uses, for the owner host suite: the
 *        clock, the header lending, the controllers and J3's supply. The real header is the SDK's.
 *
 * Positions lend as themselves (position 5 is GPIO 5), so a fake pin can be
 * told apart by the position it stands for. tests/target compiles mesh_aos.c
 * against the real kode_sys.h, which is what proves this still matches.
 */
#ifndef KODE_SYS_H_
#define KODE_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODE_SYS_EXP_POS_MAX   14U

/** @brief kode_sys.h's controllers. */
typedef enum {
    KODE_SYS_EXP_I2C = 0,
    KODE_SYS_EXP_SPI,
    KODE_SYS_EXP_UART,
} kode_sys_exp_bus_t;

/** @brief Milliseconds on a clock that only goes forward: here, the simulator's ticks (fake_misc.c). */
uint32_t kode_sys_time_ms(void);

/** @brief Lend @p n positions under @p tag; each GPIO written is its position. */
kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio);

/** @brief Hand back what @p tag lent and booked. */
kode_status_t kode_sys_exp_return(const char *tag);

/** @brief Book a controller of J3's @p bus under @p tag; its number in @p id. */
kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id);

/** @brief J3's supply, switched over the link to the C5: a command, its answer is EXP_POWER_SIG. */
kode_status_t kode_sys_exp_power(bool on);

#ifdef __cplusplus
}
#endif

#endif /* KODE_SYS_H_ */
