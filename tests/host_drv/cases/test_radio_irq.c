/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_irq.c
 * @brief DIO9: level-triggered, masked by its own interrupt until the flags are
 *        taken, and the error word behind the error flag.
 */
#include <stdint.h>

#include "unity.h"

#include "radio_harness.h"

TEST_CASE("irq: a flag raises DIO9; the interrupt masks itself until the flags are taken", "[radio][irq]")
{
    radio_up_sub();
    TEST_ASSERT_FALSE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_TRUE(pin_isr_enabled());

    chip_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_FALSE(pin_isr_enabled());       /* nothing more arrives until irq_take() */

    chip_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);          /* a second flag while masked */
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);

    uint32_t const flags = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE | MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, flags);
    TEST_ASSERT_FALSE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_TRUE(pin_isr_enabled());         /* listening to the line again */
    TEST_ASSERT_EQUAL_HEX32(0U, mesh_drv_lr2021_interrupts_take());   /* and the flags are gone */
}

static void flag_in_the_gap(void)
{
    g_hw.on_intr_enable = NULL;                  /* once */
    g_chip.irq |= MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE;        /* raised after the clear, before the line is looked at */
}

TEST_CASE("irq: a flag raised between the clear and the re-enable is not lost - it fires at once", "[radio][irq]")
{
    radio_up_sub();
    chip_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);

    g_hw.on_intr_enable = flag_in_the_gap;
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, mesh_drv_lr2021_interrupts_take());

    /* The driver cleared first and enabled after, and the line is level-triggered:
     * the flag that came in the gap pulls the interrupt as soon as it is on. */
    TEST_ASSERT_EQUAL_UINT(2U, g_irq_calls);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, mesh_drv_lr2021_interrupts_take());
}

TEST_CASE("irq: take with nothing pending is zero and still re-arms", "[radio][irq]")
{
    radio_up_sub();
    chip_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    (void)mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL_HEX32(0U, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_TRUE(pin_isr_enabled());
}

TEST_CASE("irq: an answer lost after the clear is said, not taken for no flags", "[radio][irq]")
{
    radio_up_sub();
    chip_raise(MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT);

    /* GetAndClearIrq's frame goes; the frame that reads its answer fails. */
    g_hw.spi_fail_in = 2U;
    uint32_t flags = 0xFFFFFFFFU;
    TEST_ASSERT_NOT_EQUAL(KODE_OK, mesh_drv_lr2021_interrupts_read(&flags));
    TEST_ASSERT_EQUAL_HEX32(0U, flags);
    TEST_ASSERT_EQUAL_HEX32(0U, g_chip.irq);      /* the chip cleared them all the same */
    TEST_ASSERT_TRUE(pin_isr_enabled());          /* and the line is listened to again */

    /* read whole, it is KODE_OK */
    chip_raise(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_interrupts_read(&flags));
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, flags);
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_interrupts_read(NULL));
}

TEST_CASE("errors: reads GetErrors and names each flag that is set", "[radio][irq]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL_HEX16(0U, mesh_drv_lr2021_errors());
    TEST_ASSERT_EQUAL_UINT(0U, fake_log_count('E'));

    g_chip.errors = 0x0005U;   /* the 32 MHz oscillator, the PLL */
    TEST_ASSERT_EQUAL_HEX16(0x0005U, mesh_drv_lr2021_errors());
    TEST_ASSERT_TRUE(fake_log_seen("the 32 MHz oscillator did not start"));
    TEST_ASSERT_TRUE(fake_log_seen("the PLL did not lock"));
    TEST_ASSERT_EQUAL_UINT(2U, fake_log_count('E'));
}

TEST_CASE("errors: a chip that does not answer reads as every flag set, and the log says so", "[radio][irq]")
{
    radio_up_sub();
    g_chip.absent   = true;
    g_chip.creep_us = 20000U;
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, mesh_drv_lr2021_errors());
    TEST_ASSERT_TRUE(fake_log_seen("did not answer for its error flags"));
}

TEST_CASE("configure: an error flag still set after the setting makes it a failed configure, named in the log", "[radio][irq]")
{
    radio_open();
    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    g_chip.sticky_errors = 0x0004U;   /* the PLL does not lock, and clearing it does not help */
    mesh_drv_lora_settings_t cfg;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9U, 850, &cfg));

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_configure(&cfg));
    TEST_ASSERT_TRUE(fake_log_seen("the PLL did not lock"));
}

TEST_CASE("configure: refuses a setting the chip cannot take, before sending any of it", "[radio][irq]")
{
    radio_open();
    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    chip_log_clear();

    mesh_drv_lora_settings_t cfg;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_SUB_GHZ, 9U, 850, &cfg));
    mesh_drv_lora_settings_t bad = cfg;

    bad.spreading_factor = 4U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = cfg; bad.spreading_factor = 13U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = cfg; bad.bandwidth = 0x01U;   /* RFU in table 9-3 */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = cfg; bad.band_2_4_ghz = true;                 /* 868 MHz is not the 2.4 GHz path */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = cfg; bad.tx_power = 50;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = cfg; bad.amplifier_duty = 9U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());

    mesh_drv_lora_settings_t high_band;
    TEST_ASSERT_TRUE(mesh_plan_transmit_settings(MESH_PLAN_BAND_2_4_GHZ, 9U, 450, &high_band));
    bad = high_band; bad.tx_power = 30;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    bad = high_band; bad.amplifier_duty = 5U;               /* below the HF amplifier's idle value */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&bad));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}
