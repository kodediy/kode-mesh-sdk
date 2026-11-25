/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_gnss_sleep.c
 * @brief The GNSS driver's Sleep command and its wake, on a model of its UART:
 *        the datasheet's frame on the wire (ME31GR02 datasheet v1.0.0, p.14), the wake with a
 *        byte and no reset, the rate kept through it, a receiver that does not sleep and one
 *        that does not wake.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gnss_wire.h"
#include "mesh_bsp.h"
#include "mesh_drv_gnss.h"

/* A GGA with a fix, as the receiver says one a second. */
#define GGA "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"

static const mesh_drv_gnss_pins_t PINS = { .uart_port = 2U, .rxd = 9U, .txd = 11U, .reset = 8U, .pps = 10U };

/** @brief Open on a fresh wire, and hear one sentence at the first rate. */
static void open_talking(void)
{
    mesh_drv_gnss_close();
    gnss_wire_reset();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_open(&PINS));
    gnss_wire_say(GGA);
    mesh_drv_gnss_poll();
    mesh_drv_gnss_status_t st;
    mesh_drv_gnss_read(&st);
    TEST_ASSERT_TRUE(st.talking);
    TEST_ASSERT_EQUAL_UINT32(MESH_BSP_GNSS_BAUD, st.baud);
}

static mesh_drv_gnss_status_t now(void)
{
    mesh_drv_gnss_status_t st;
    mesh_drv_gnss_read(&st);
    return st;
}

/** @brief Let @p ms of the driver's clock go by. */
static void pass_ms(uint32_t ms)
{
    esp_rom_delay_us(ms * 1000U);
}

/** @brief How many wake-up pairs ("\r\n") were written from @p from on. */
static unsigned wakes_written(size_t from)
{
    unsigned n = 0U;
    for (size_t i = from; (i + 1U) < g_wire.tx_n; i += 2U) {
        if ((g_wire.tx[i] == (uint8_t)'\r') && (g_wire.tx[i + 1U] == (uint8_t)'\n')) {
            n++;
        }
    }
    return n;
}

TEST_CASE("gnss sleep: the datasheet's frame, whole, and waited out of the UART", "[gnss]")
{
    static const uint8_t DATASHEET[16] = { 0x42, 0x4b, 0x51, 0x05, 0x00, 0x03, 0x00, 0x08,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    open_talking();
    TEST_ASSERT_EQUAL_UINT(0U, g_wire.tx_n);   /* nothing is said to it before */

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());

    TEST_ASSERT_EQUAL_UINT(sizeof(DATASHEET), g_wire.tx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DATASHEET, g_wire.tx, sizeof(DATASHEET));
    TEST_ASSERT_TRUE(g_wire.tx_waits >= 1U);
    TEST_ASSERT_TRUE(g_wire.installed);   /* the UART stays: its TXD idles high, nothing wakes it */
    mesh_drv_gnss_status_t const st = now();
    TEST_ASSERT_TRUE(st.asleep);
    TEST_ASSERT_FALSE(st.talking);
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_gnss_sleep());   /* asleep already: nothing more sent */
    TEST_ASSERT_EQUAL_UINT(sizeof(DATASHEET), g_wire.tx_n);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss sleep: refused closed, before a sentence, and for a receiver at the LC76F's rate", "[gnss]")
{
    mesh_drv_gnss_close();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_gnss_sleep());
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_gnss_wake());

    gnss_wire_reset();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_open(&PINS));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_gnss_sleep());   /* silent: who knows what it is */

    /* Silent past the rate's try: the driver moves to 9600, where an LC76F answers. */
    pass_ms(3100U);
    mesh_drv_gnss_poll();
    TEST_ASSERT_EQUAL_UINT32(MESH_BSP_GNSS_BAUD_ALT, g_wire.baud);
    gnss_wire_say(GGA);
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().talking);
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_gnss_sleep());
    TEST_ASSERT_EQUAL_UINT(0U, g_wire.tx_n);
    TEST_ASSERT_FALSE(now().asleep);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss sleep: a sentence in flight is no matter; one past the margin means it did not sleep", "[gnss]")
{
    open_talking();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());

    gnss_wire_say(GGA);   /* already on its way when the command went */
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().asleep);

    /* Silent for a long while: still asleep, and the rate search does not start. */
    pass_ms(10000U);
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().asleep);
    TEST_ASSERT_EQUAL_UINT32(MESH_BSP_GNSS_BAUD, g_wire.baud);

    gnss_wire_say(GGA);   /* talking 10 s on: it ignored the command */
    mesh_drv_gnss_poll();
    mesh_drv_gnss_status_t const st = now();
    TEST_ASSERT_FALSE(st.asleep);
    TEST_ASSERT_TRUE(st.talking);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss wake: a byte pair, no reset, the rate kept, talking again at its first sentence", "[gnss]")
{
    open_talking();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());
    size_t const at = g_wire.tx_n;
    pass_ms(60000U);
    mesh_drv_gnss_poll();

    TickType_t const t0 = xTaskGetTickCount();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_wake());
    TickType_t const took = xTaskGetTickCount() - t0;
    TEST_ASSERT_TRUE_MESSAGE(took < pdMS_TO_TICKS(MESH_BSP_GNSS_RESET_MS / 2U), "a wake must not pulse the reset line");

    TEST_ASSERT_EQUAL_UINT(at + 2U, g_wire.tx_n);
    TEST_ASSERT_EQUAL_UINT8('\r', g_wire.tx[at]);
    TEST_ASSERT_EQUAL_UINT8('\n', g_wire.tx[at + 1U]);
    TEST_ASSERT_EQUAL_INT(1, gpio_get_level((gpio_num_t)PINS.reset));   /* released all along */

    mesh_drv_gnss_status_t st = now();
    TEST_ASSERT_FALSE(st.asleep);
    TEST_ASSERT_FALSE(st.talking);              /* not until it speaks */
    TEST_ASSERT_EQUAL_UINT32(0U, st.messages);  /* the counts start again */
    TEST_ASSERT_FALSE(st.fix.fix);

    pass_ms(500U);
    mesh_drv_gnss_poll();
    TEST_ASSERT_EQUAL_UINT32(MESH_BSP_GNSS_BAUD, g_wire.baud);
    gnss_wire_say(GGA);
    mesh_drv_gnss_poll();
    st = now();
    TEST_ASSERT_TRUE(st.talking);
    TEST_ASSERT_EQUAL_UINT32(1U, st.messages);
    TEST_ASSERT_EQUAL_UINT(at + 2U, g_wire.tx_n);   /* no second try once it spoke */

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_wake());   /* awake: nothing sent */
    TEST_ASSERT_EQUAL_UINT(at + 2U, g_wire.tx_n);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss wake: silent, the byte again each try, and at the last try the reset line", "[gnss]")
{
    open_talking();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());
    size_t const at = g_wire.tx_n;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_wake());

    for (unsigned t = 1U; t < MESH_BSP_GNSS_WAKE_TRIES; t++) {
        pass_ms(MESH_BSP_GNSS_WAKE_MS + 10U);
        mesh_drv_gnss_poll();
        TEST_ASSERT_EQUAL_UINT(t + 1U, wakes_written(at));
    }
    pass_ms(MESH_BSP_GNSS_WAKE_MS + 10U);
    mesh_drv_gnss_poll();   /* the last try is spent: restarted */
    TEST_ASSERT_EQUAL_UINT(MESH_BSP_GNSS_WAKE_TRIES, wakes_written(at));
    mesh_drv_gnss_status_t const st = now();
    TEST_ASSERT_FALSE(st.asleep);
    TEST_ASSERT_FALSE(st.talking);
    TEST_ASSERT_EQUAL_UINT32(0U, st.baud);   /* the rate is looked for again after a reset */
    TEST_ASSERT_EQUAL_INT(1, gpio_get_level((gpio_num_t)PINS.reset));

    gnss_wire_say(GGA);
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().talking);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss sleep: a frame that would not go out leaves it awake", "[gnss]")
{
    open_talking();
    g_wire.write_fails = true;
    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_gnss_sleep());
    TEST_ASSERT_FALSE(now().asleep);
    TEST_ASSERT_TRUE(now().talking);
    g_wire.write_fails = false;
    mesh_drv_gnss_close();
    TEST_ASSERT_FALSE(g_wire.installed);
}

TEST_CASE("gnss wake: right after the command, a sentence is not taken for the wake; the bytes go on", "[gnss]")
{
    open_talking();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());
    size_t const at = g_wire.tx_n;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_wake());   /* end() then begin() at once */

    gnss_wire_say(GGA);   /* the talk before the command took: it may still fall asleep */
    mesh_drv_gnss_poll();
    TEST_ASSERT_FALSE(now().talking);

    pass_ms(MESH_BSP_GNSS_WAKE_MS + 10U);
    mesh_drv_gnss_poll();
    TEST_ASSERT_EQUAL_UINT(2U, wakes_written(at));   /* tried again */
    gnss_wire_say(GGA);   /* past the margin: this one is the wake */
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().talking);
    mesh_drv_gnss_close();
}

TEST_CASE("gnss sleep: half a sentence before the command, read late, is not taken for an ignored one", "[gnss]")
{
    open_talking();
    gnss_wire_raw("$GPGGA,123519,4807.038,N,01131.000");   /* half of it, in the buffer */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_gnss_sleep());
    pass_ms(MESH_BSP_GNSS_SLEEP_MS + 100U);   /* the owner late to read, as after the board's rest */
    gnss_wire_raw(",E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
    mesh_drv_gnss_poll();
    TEST_ASSERT_TRUE(now().asleep);
    mesh_drv_gnss_close();
}
