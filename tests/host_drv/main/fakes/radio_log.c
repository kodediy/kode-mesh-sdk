/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_log.c
 * @brief The log of the radio driver host suite. See kode_log.h.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kode_log.h"

#define LOG_LINES  96
#define LOG_WIDTH  200

static pthread_mutex_t s_log_mu = PTHREAD_MUTEX_INITIALIZER;
static char            s_lines[LOG_LINES][LOG_WIDTH];
static char            s_levels[LOG_LINES];
static unsigned        s_log_n;

void fake_log(char level, const char *tag, const char *fmt, ...)
{
    char    line[LOG_WIDTH];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    (void)pthread_mutex_lock(&s_log_mu);
    unsigned const slot = s_log_n % LOG_LINES;
    (void)memcpy(s_lines[slot], line, sizeof(line));
    s_levels[slot] = level;
    s_log_n++;
    (void)pthread_mutex_unlock(&s_log_mu);

    if (getenv("MESH_TEST_LOG") != NULL) {
        (void)printf("    [%c] %s: %s\n", level, tag, line);
    }
}

void fake_log_clear(void)
{
    (void)pthread_mutex_lock(&s_log_mu);
    s_log_n = 0U;
    (void)memset(s_lines, 0, sizeof(s_lines));
    (void)memset(s_levels, 0, sizeof(s_levels));
    (void)pthread_mutex_unlock(&s_log_mu);
}

bool fake_log_seen(const char *needle)
{
    bool found = false;

    (void)pthread_mutex_lock(&s_log_mu);
    unsigned const n = (s_log_n < LOG_LINES) ? s_log_n : LOG_LINES;
    for (unsigned i = 0U; i < n; i++) {
        if (strstr(s_lines[i], needle) != NULL) {
            found = true;
        }
    }
    (void)pthread_mutex_unlock(&s_log_mu);
    return found;
}

unsigned fake_log_count(char level)
{
    unsigned count = 0U;

    (void)pthread_mutex_lock(&s_log_mu);
    unsigned const n = (s_log_n < LOG_LINES) ? s_log_n : LOG_LINES;
    for (unsigned i = 0U; i < n; i++) {
        if (s_levels[i] == level) {
            count++;
        }
    }
    (void)pthread_mutex_unlock(&s_log_mu);
    return count;
}
