/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_timer.h
 * @brief esp_timer_get_time() for the radio driver host suite: a clock that
 *        moves a little on every read, so a wait for BUSY always ends, and by
 *        esp_rom_delay_us(). It is not the chip's clock: that one only moves
 *        when a case says so (chip_advance_us).
 */
#ifndef FAKE_ESP_TIMER_H_
#define FAKE_ESP_TIMER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Renamed like the other fakes of symbols the linux build may define itself. */
int64_t fake_timer_get_time(void);
#define esp_timer_get_time() fake_timer_get_time()

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_TIMER_H_ */
