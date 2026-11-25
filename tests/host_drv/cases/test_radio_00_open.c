/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_00_open.c
 * @brief Taking the pins and giving them back, and the restart hook the driver
 *        registers with ESP-IDF. This file runs FIRST: the hook is registered
 *        once per process (ESP-IDF keeps five slots, and the driver remembers
 *        the answer), so the case that checks a full table must be the one that
 *        meets the driver before anything has opened it.
 */
#include <stdint.h>

#include "unity.h"

#include "esp_system.h"
#include "radio_harness.h"

static void nop_irq(void *arg)
{
    (void)arg;
}

/** @brief A chip on the header and the driver closed - a case's own start. */
static void closed_chip(void)
{
    mesh_drv_lr2021_close();
    chip_power_up();
    fake_log_clear();
}

/* ---- The restart hook ------------------------------------------------------------- */

TEST_CASE("hook: registered at the first open; a full table is remembered and a later open tries again", "[radio][open][hook]")
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shutdown_count(),
                                   "this case must run first: the driver registers once per process");

    shutdown_fill(true);   /* all five slots taken by others */
    radio_open();
    TEST_ASSERT_FALSE(mesh_drv_lr2021_guarded());
    TEST_ASSERT_EQUAL_UINT(0U, shutdown_count());
    TEST_ASSERT_TRUE(fake_log_seen("no shutdown handler"));

    shutdown_fill(false);
    radio_open();          /* closed and opened again: not guarded yet, so it asks again */
    TEST_ASSERT_TRUE(mesh_drv_lr2021_guarded());
    TEST_ASSERT_EQUAL_UINT(1U, shutdown_count());

    radio_open();          /* and once it holds a slot it does not ask for a second */
    TEST_ASSERT_TRUE(mesh_drv_lr2021_guarded());
    TEST_ASSERT_EQUAL_UINT(1U, shutdown_count());
}

/* ---- Open and close ------------------------------------------------------------------- */

TEST_CASE("open: takes the bus, the device and the interrupt service; close gives the bus and the device back", "[radio][open]")
{
    radio_open();
    TEST_ASSERT_EQUAL_UINT(1U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(1U, g_hw.devices_up);
    TEST_ASSERT_TRUE(g_hw.bus_acquired);
    TEST_ASSERT_TRUE(g_hw.isr_service_up);
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_CS));    /* idle high, so no clock is taken for a command */
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));
    TEST_ASSERT_FALSE(pin_isr_enabled());           /* on when listening */
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.nss_frames);    /* open sends nothing */

    mesh_drv_lr2021_close();
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.devices_up);
    TEST_ASSERT_FALSE(g_hw.bus_acquired);
    TEST_ASSERT_TRUE(g_hw.isr_service_up);          /* the service is the app's: whoever added a handler since keeps it */

    mesh_drv_lr2021_close();   /* closing a closed driver does nothing */
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
}

TEST_CASE("open: the SPI controller is the one the pins name, for the bus and for the device", "[radio][open]")
{
    mesh_drv_lr2021_pins_t pins = *radio_pins();
    pins.spi_host = 1U;   /* kode_sys_exp_ctrl() hands out whichever controller is free */

    closed_chip();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_open(&pins, nop_irq, NULL));
    TEST_ASSERT_EQUAL_INT(1, g_hw.bus_host);
    TEST_ASSERT_EQUAL_INT(1, g_hw.device_host);
    mesh_drv_lr2021_close();

    closed_chip();
    pins.spi_host = 2U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_open(&pins, nop_irq, NULL));
    TEST_ASSERT_EQUAL_INT(2, g_hw.bus_host);
    TEST_ASSERT_EQUAL_INT(2, g_hw.device_host);
    mesh_drv_lr2021_close();
}

TEST_CASE("open: a second open is refused, a NULL argument is refused", "[radio][open]")
{
    radio_open();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_EQUAL_UINT(1U, g_hw.buses_up);   /* and the bus was not taken twice */

    closed_chip();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_open(NULL, nop_irq, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_open(radio_pins(), NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
}

TEST_CASE("open: a bus that will not come up leaves nothing taken", "[radio][open]")
{
    closed_chip();
    g_hw.spi_init_fails = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.devices_up);
    TEST_ASSERT_FALSE(mesh_drv_lr2021_busy());   /* still closed */
}

TEST_CASE("open: a device that will not attach frees the bus", "[radio][open]")
{
    closed_chip();
    g_hw.spi_add_fails = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.devices_up);
}

TEST_CASE("open: an interrupt service that fails for another reason frees the bus and the device", "[radio][open]")
{
    closed_chip();
    g_hw.isr_service_rc = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(KODE_ERROR, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.devices_up);
    TEST_ASSERT_FALSE(g_hw.bus_acquired);
}

TEST_CASE("open: an interrupt handler that cannot be added frees the bus and the device, and leaves the service", "[radio][open]")
{
    closed_chip();
    g_hw.isr_add_fails = true;
    TEST_ASSERT_EQUAL(KODE_ERROR, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_TRUE(g_hw.isr_service_up);    /* installed by this driver or not, it is never taken down */
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.buses_up);
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.devices_up);
    TEST_ASSERT_FALSE(g_hw.bus_acquired);
}

TEST_CASE("open: an interrupt service someone else installed is used, and left up by close", "[radio][open]")
{
    closed_chip();
    g_hw.isr_service_up = true;                         /* kode-sdk's, say */
    g_hw.isr_service_rc = ESP_ERR_INVALID_STATE;        /* what installing it a second time says */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    mesh_drv_lr2021_close();
    TEST_ASSERT_TRUE(g_hw.isr_service_up);              /* not the driver's to take down */

    closed_chip();
    g_hw.isr_add_fails  = true;
    g_hw.isr_service_up = true;
    g_hw.isr_service_rc = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(KODE_ERROR, mesh_drv_lr2021_open(radio_pins(), nop_irq, NULL));
    TEST_ASSERT_TRUE(g_hw.isr_service_up);              /* and not taken down on a failure either */
}

/* ---- A closed driver ----------------------------------------------------------------------- */

TEST_CASE("closed: every call is refused and nothing goes to the chip", "[radio][open]")
{
    closed_chip();

    uint8_t           major = 0U;
    uint8_t           minor = 0U;
    uint8_t           buf[4];
    float             dbm = 0.0f;
    mesh_drv_lr2021_packet_t   rx;
    mesh_drv_lr2021_mode_t mode;
    mesh_drv_lora_settings_t     cfg = { 0 };

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(&cfg));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_calibrate());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_send(buf, 4U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_read(buf, 4U, 0U, &rx));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_rssi(&dbm));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_flush_receive());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_listen_for(100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_mode(&mode));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, mesh_drv_lr2021_errors());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_alive());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_busy());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_dio9());
    mesh_drv_lr2021_hold_reset();                        /* nothing to hold */
    TEST_ASSERT_EQUAL_INT(1, pin_level(PIN_RST));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.nss_frames);
}

TEST_CASE("open: an argument that is NULL where a result goes is refused", "[radio][open]")
{
    radio_up_sub();
    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_reset(NULL, &minor));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_reset(&major, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_configure(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_rssi(NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_mode(NULL));
    TEST_ASSERT_EQUAL_UINT(0U, g_hw.nss_frames);
}
