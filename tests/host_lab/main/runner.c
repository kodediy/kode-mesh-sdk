/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the lab-build host suite. See tests/host/main/runner.c.
 */
#include <stdlib.h>

#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    exit(UNITY_END());
}
