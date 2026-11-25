/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.h
 * @brief What tests/host_ao fakes under kode-sdk: klog's kode_log() and khal's khal_task_self().
 */
#ifndef FAKE_KODE_H_
#define FAKE_KODE_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How many times kode_log() was called at warning level or above. */
extern unsigned g_log_warnings;

#ifdef __cplusplus
}
#endif

#endif /* FAKE_KODE_H_ */
