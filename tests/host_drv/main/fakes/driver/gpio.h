/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file driver/gpio.h
 * @brief The slice of ESP-IDF's GPIO driver that mesh_drv_lr2021.c calls, for the
 *        radio driver host suite. Names, types and return types are ESP-IDF's
 *        (components/esp_driver_gpio at v6.1); the pins are in lr_chip.c.
 */
#ifndef FAKE_DRIVER_GPIO_H_
#define FAKE_DRIVER_GPIO_H_

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;

typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE = 1,
    GPIO_INTR_NEGEDGE = 2,
    GPIO_INTR_ANYEDGE = 3,
    GPIO_INTR_LOW_LEVEL = 4,
    GPIO_INTR_HIGH_LEVEL = 5,
} gpio_int_type_t;

typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;

typedef struct {
    uint64_t         pin_bit_mask;
    gpio_mode_t      mode;
    gpio_pullup_t    pull_up_en;
    gpio_pulldown_t  pull_down_en;
    gpio_int_type_t  intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

esp_err_t gpio_config(const gpio_config_t *pGPIOConfig);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int       gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_uninstall_isr_service(void);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_DRIVER_GPIO_H_ */
