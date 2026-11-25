/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_log.h
 * @brief klog for the radio driver host suite: every line is kept, so a case
 *        can ask what the driver said. MESH_TEST_LOG=1 prints them as they come.
 */
#ifndef KODE_LOG_H_
#define KODE_LOG_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void fake_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void     fake_log_clear(void);
bool     fake_log_seen(const char *needle);   /**< any line holds it */
unsigned fake_log_count(char level);          /**< lines at one level */

#define KLOG_E(tag, ...)  fake_log('E', (tag), __VA_ARGS__)
#define KLOG_W(tag, ...)  fake_log('W', (tag), __VA_ARGS__)
#define KLOG_I(tag, ...)  fake_log('I', (tag), __VA_ARGS__)
#define KLOG_D(tag, ...)  fake_log('D', (tag), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* KODE_LOG_H_ */
