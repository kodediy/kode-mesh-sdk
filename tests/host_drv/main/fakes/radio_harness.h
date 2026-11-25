/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_harness.h
 * @brief What every case of the radio driver suite starts from: a chip on the
 *        header, the driver opened on it, and - for most cases - reset and
 *        configured as the owner does.
 */
#ifndef RADIO_HARNESS_H_
#define RADIO_HARNESS_H_

#include <stdint.h>

#include "unity.h"

#include "kode_log.h"
#include "lr_chip.h"
#include "mesh_drv_lr2021.h"
#include "mesh_plan.h"

/** @brief Times the driver's DIO9 callback ran. */
extern volatile unsigned g_irq_calls;

/** @brief The pins the driver is opened on. */
const mesh_drv_lr2021_pins_t *radio_pins(void);

/** @brief A new chip; the driver closed, then opened on it. Nothing sent yet. */
void radio_open(void);

/** @brief radio_open(), then reset and configure for @p band as the owner does;
 *         the chip's command log is NOT cleared, so a case can read the setup. */
void radio_up_raw(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm);

/** @brief radio_up_raw(), then clear the log and the counters: a case counts only
 *         what it does. */
void radio_up(mesh_plan_band_t band, uint8_t spreading_factor, int32_t power_cdbm);

/** @brief The usual: sub-GHz, SF9, the region's cap. */
void radio_up_sub(void);

/** @brief The chip leaves Tx by the end of its packet: advance its clock past the
 *         airtime. */
void chip_finish_tx(void);

/** @brief SetTx's timeout of the last SetTx command, in microseconds. */
uint64_t last_tx_timeout_us(void);

/** @brief Packet bytes 0..n-1 as 'a' + i, for a payload with a pattern. */
void fill_pattern(uint8_t *buf, unsigned n);

#endif /* RADIO_HARNESS_H_ */
