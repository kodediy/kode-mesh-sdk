/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief What tests/host_sys fakes under kode-sdk: klog's kode_log().
 */
#include <stdarg.h>
#include <stdio.h>

#include "kode_log.h"

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
