/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief See fake_kode.h.
 */
#include "fake_kode.h"

#include <stdarg.h>
#include <stdio.h>

#include "kode_log.h"
#include "khal_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

unsigned g_log_warnings;

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    if (lvl >= KODE_LOG_WARN) {
        g_log_warnings++;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)printf("[%s] ", tag);
    (void)vprintf(fmt, ap);
    (void)printf("\n");
    va_end(ap);
}

/** @brief khal's name for the calling task: FreeRTOS's handle, as khal's host fake gives it. */
void *khal_task_self(void)
{
    return (void *)xTaskGetCurrentTaskHandle();
}
