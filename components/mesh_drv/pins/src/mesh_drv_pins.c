/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_pins.c
 * @brief Header lines by hand, for the look. See mesh_drv_pins.h.
 */
#include "mesh_drv_pins.h"

#include "driver/gpio.h"

void mesh_drv_pins_high_impedance(uint8_t gpio)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
}

void mesh_drv_pins_pull_down(uint8_t gpio)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
}

bool mesh_drv_pins_level(uint8_t gpio)
{
    return gpio_get_level((gpio_num_t)gpio) != 0;
}
