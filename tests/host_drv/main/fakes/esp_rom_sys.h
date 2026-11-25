/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_rom_sys.h
 * @brief esp_rom_delay_us() for the radio driver host suite: it takes no real
 *        time, it moves the driver's clock (lr_chip.c).
 */
#ifndef FAKE_ESP_ROM_SYS_H_
#define FAKE_ESP_ROM_SYS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The linux build already links an esp_rom_delay_us of its own, and two definitions
 * of one symbol let the linker keep either: the fake goes by another name. */
void fake_rom_delay_us(uint32_t us);
#define esp_rom_delay_us(us) fake_rom_delay_us(us)

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ESP_ROM_SYS_H_ */
