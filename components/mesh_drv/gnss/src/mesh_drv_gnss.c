/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_gnss.c
 * @brief The GNSS receiver's NMEA stream, read into a fix. See mesh_drv_gnss.h.
 *
 * Every byte the UART gives goes straight into mesh_drv_nmea.c, which assembles the
 * line, checks its XOR and, when it holds, reads it into the fix. A separate
 * snapshot (s_pub) is published after each good sentence and read under a
 * spinlock, so mesh_drv_gnss_read() never walks the parser while poll() writes it.
 *
 * The reset and 1PPS lines go through ESP-IDF's GPIO driver directly - this is
 * the driver layer (tools/check_layers.sh, rule 1). The I2C pair is not
 * touched here (MESH_BSP_GNSS_I2C_RISK).
 *
 * Asleep, the receiver is still polled: whatever was in flight when the Sleep
 * command went out is read, and a sentence that checks after
 * MESH_BSP_GNSS_SLEEP_MS says it did not sleep. The rate it was found at is
 * kept through a sleep, so the rate search never runs on a receiver that is
 * silent because it was told to be.
 */
#include "mesh_drv_gnss.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_log.h"
#include "mesh_bsp.h"

#define TAG                "mesh.gnss"

/* UART2: UART0 is the console and UART1 is kept for the C5 rescue line, so the
 * GNSS takes the next controller. One instance, this driver's. */
#define GNSS_RX_BUF        2048
#define GNSS_CHUNK         128
/* How long a rate is listened at with nothing checking before the other is
 * tried. A receiver sends several sentences a second. */
#define GNSS_RATE_TRY_US   3000000

/* How long the Sleep command and the wake-up bytes may take to leave the
 * UART: 16 bytes at 115200 baud take under 2 ms, and a margin. */
#define GNSS_TX_WAIT_MS    50U

/* The two rates a receiver on this module may talk at out of reset: the
 * ME31GR02's, and 9600. */
static const uint32_t RATES[] = { MESH_BSP_GNSS_BAUD, MESH_BSP_GNSS_BAUD_ALT };
#define GNSS_RATE_COUNT    (sizeof(RATES) / sizeof(RATES[0]))

static mesh_drv_gnss_pins_t  s_pins;
static bool             s_open;
static portMUX_TYPE     s_lock = portMUX_INITIALIZER_UNLOCKED;
static mesh_drv_gnss_status_t  s_pub;           /* Guarded by s_lock.              */

static mesh_drv_nmea_t       s_nmea;
static size_t           s_rate;          /* Index into RATES.               */
static int64_t          s_rate_since;    /* When that rate was set.         */
static bool             s_locked;        /* A sentence has checked.         */

/** @brief Whether the receiver was told to sleep. */
typedef enum {
    GNSS_AWAKE = 0,   /* talking, or being listened for                 */
    GNSS_ASLEEP,      /* the Sleep command went out at s_slept_at       */
    GNSS_WAKING,      /* the wake-up bytes went out at s_woke_at        */
} gnss_state_t;

static const uint8_t    SLEEP_FRAME[MESH_BSP_GNSS_SLEEP_FRAME_LEN] = MESH_BSP_GNSS_SLEEP_FRAME;
static const uint8_t    WAKE_BYTES[2] = { (uint8_t)'\r', (uint8_t)'\n' };

static gnss_state_t     s_state;
static int64_t          s_slept_at;      /* When the Sleep command left.    */
static int64_t          s_woke_at;       /* When the last wake-up try left. */
static int64_t          s_wake_first_at; /* When the first one left.        */
static uint32_t         s_wake_tries;    /* Wake-up tries sent.             */

static void gnss_reset_hold(bool low)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_pins.reset,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
    (void)gpio_set_level((gpio_num_t)s_pins.reset, low ? 0U : 1U);
}

/** @brief Copy the parser's fix and counts into the shared snapshot. */
static void gnss_publish(void)
{
    mesh_drv_nmea_fix_t fix;
    mesh_drv_nmea_read(&s_nmea, &fix);

    taskENTER_CRITICAL(&s_lock);
    s_pub.fix      = fix;
    s_pub.messages = s_nmea.good;
    s_pub.bad      = s_nmea.bad;
    s_pub.baud     = s_locked ? RATES[s_rate] : 0U;
    s_pub.talking  = s_locked && (s_state == GNSS_AWAKE);
    s_pub.asleep   = (s_state == GNSS_ASLEEP);
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief Send bytes to the receiver and wait until they have left the UART. */
static kode_status_t gnss_send(const uint8_t *bytes, size_t len)
{
    int const put = uart_write_bytes((uart_port_t)s_pins.uart_port, bytes, len);
    if ((put != (int)len)
        || (uart_wait_tx_done((uart_port_t)s_pins.uart_port, pdMS_TO_TICKS(GNSS_TX_WAIT_MS)) != ESP_OK)) {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/** @brief A sentence checked: what it says of the rate and of a sleep or a wake. */
static void gnss_heard(void)
{
    int64_t const now = esp_timer_get_time();

    if (!s_locked) {
        s_locked = true;
        KLOG_I(TAG, "NMEA at %u baud", (unsigned)RATES[s_rate]);
    }
    if ((s_state == GNSS_ASLEEP)
        && ((now - s_slept_at) > ((int64_t)MESH_BSP_GNSS_SLEEP_MS * 1000))) {
        s_state = GNSS_AWAKE;
        KLOG_W(TAG, "the receiver did not sleep: it still talks %u ms after the command",
               (unsigned)((now - s_slept_at) / 1000));
    } else if ((s_state == GNSS_WAKING)
               && ((now - s_slept_at) > ((int64_t)MESH_BSP_GNSS_SLEEP_MS * 1000))) {
        /* Only past the sleep margin: a sentence sooner may be the talk before the
         * command took, from a receiver about to fall asleep, and the wake-up bytes
         * go on until it speaks after it. */
        s_state = GNSS_AWAKE;
        KLOG_I(TAG, "the receiver woke: talking %u ms after the first byte, %u tries",
               (unsigned)((now - s_wake_first_at) / 1000), (unsigned)s_wake_tries);
    } else {
        /* awake, asleep with a sentence that left before the command, or waking
         * with one that may have */
    }
}

/** @brief Waking and still silent: send the bytes again, or at the last try restart it. */
static void gnss_wake_again(void)
{
    int64_t const now = esp_timer_get_time();

    if ((now - s_woke_at) <= ((int64_t)MESH_BSP_GNSS_WAKE_MS * 1000)) {
        return;
    }
    if (s_wake_tries < MESH_BSP_GNSS_WAKE_TRIES) {
        s_wake_tries++;
        s_woke_at = now;
        (void)gnss_send(WAKE_BYTES, sizeof(WAKE_BYTES));
        return;
    }
    KLOG_W(TAG, "the receiver did not wake after %u tries: restarting it with its reset line",
           (unsigned)s_wake_tries);
    mesh_drv_gnss_restart();
}

kode_status_t mesh_drv_gnss_open(const mesh_drv_gnss_pins_t *pins)
{
    if ((pins == NULL) || s_open) {
        return KODE_DENIED;
    }
    s_pins = *pins;

    const uart_config_t cfg = {
        .baud_rate  = (int)RATES[0],
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* The 1PPS line is an input; the receiver drives it. */
    gpio_config_t pps = {
        .pin_bit_mask = 1ULL << s_pins.pps,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&pps);

    /* Held in reset while the UART comes up. */
    gnss_reset_hold(true);
    if (uart_driver_install((uart_port_t)s_pins.uart_port, GNSS_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
        KLOG_E(TAG, "uart_driver_install failed");
        gpio_reset_pin((gpio_num_t)s_pins.reset);
        return KODE_IO_ERROR;
    }
    if ((uart_param_config((uart_port_t)s_pins.uart_port, &cfg) != ESP_OK)
        || (uart_set_pin((uart_port_t)s_pins.uart_port, (int)s_pins.txd, (int)s_pins.rxd,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)) {
        (void)uart_driver_delete((uart_port_t)s_pins.uart_port);
        gpio_reset_pin((gpio_num_t)s_pins.reset);
        return KODE_IO_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(MESH_BSP_GNSS_RESET_MS));
    (void)uart_flush_input((uart_port_t)s_pins.uart_port);
    gnss_reset_hold(false);

    mesh_drv_nmea_reset(&s_nmea);
    s_rate       = 0U;
    s_rate_since = esp_timer_get_time();
    s_locked     = false;
    s_state      = GNSS_AWAKE;
    taskENTER_CRITICAL(&s_lock);
    (void)memset(&s_pub, 0, sizeof(s_pub));
    taskEXIT_CRITICAL(&s_lock);

    s_open = true;
    return KODE_OK;
}

void mesh_drv_gnss_close(void)
{
    if (!s_open) {
        return;
    }
    s_open  = false;
    s_state = GNSS_AWAKE;
    (void)uart_driver_delete((uart_port_t)s_pins.uart_port);
    gpio_reset_pin((gpio_num_t)s_pins.reset);
    gpio_reset_pin((gpio_num_t)s_pins.pps);
}

void mesh_drv_gnss_restart(void)
{
    if (!s_open) {
        return;
    }
    gnss_reset_hold(true);
    vTaskDelay(pdMS_TO_TICKS(MESH_BSP_GNSS_RESET_MS));
    (void)uart_flush_input((uart_port_t)s_pins.uart_port);
    gnss_reset_hold(false);

    mesh_drv_nmea_reset(&s_nmea);
    s_rate       = 0U;
    (void)uart_set_baudrate((uart_port_t)s_pins.uart_port, RATES[0]);   /* match s_rate; auto-baud may have moved it */
    s_rate_since = esp_timer_get_time();
    s_locked     = false;
    s_state      = GNSS_AWAKE;
    taskENTER_CRITICAL(&s_lock);
    (void)memset(&s_pub, 0, sizeof(s_pub));
    taskEXIT_CRITICAL(&s_lock);
}

kode_status_t mesh_drv_gnss_sleep(void)
{
    if (!s_open || (s_state == GNSS_ASLEEP) || !s_locked || (RATES[s_rate] != MESH_BSP_GNSS_BAUD)) {
        return KODE_DENIED;
    }
    kode_status_t const rc = gnss_send(SLEEP_FRAME, sizeof(SLEEP_FRAME));
    if (rc != KODE_OK) {
        return rc;
    }
    /* What it said before the command is not a sign of anything after it: a half
     * sentence in the buffer or the parser would check later, maybe past the margin
     * if the owner is late to read (the board's rest), and read as "it did not sleep". */
    (void)uart_flush_input((uart_port_t)s_pins.uart_port);
    mesh_drv_nmea_reset(&s_nmea);
    s_slept_at = esp_timer_get_time();
    s_state    = GNSS_ASLEEP;
    gnss_publish();
    return KODE_OK;
}

kode_status_t mesh_drv_gnss_wake(void)
{
    if (!s_open) {
        return KODE_DENIED;
    }
    if (s_state == GNSS_AWAKE) {
        return KODE_OK;
    }
    /* What it said before it slept is not what it says now: the fix and the
     * counts start again. The rate is kept, and so is s_locked, so the search
     * for a rate does not run while it wakes. */
    mesh_drv_nmea_reset(&s_nmea);
    (void)uart_flush_input((uart_port_t)s_pins.uart_port);
    s_state      = GNSS_WAKING;
    s_wake_tries = 1U;
    s_woke_at    = esp_timer_get_time();
    s_wake_first_at = s_woke_at;
    gnss_publish();
    return gnss_send(WAKE_BYTES, sizeof(WAKE_BYTES));
}

void mesh_drv_gnss_poll(void)
{
    uint8_t chunk[GNSS_CHUNK];

    if (!s_open) {
        return;
    }

    for (;;) {
        int const got = uart_read_bytes((uart_port_t)s_pins.uart_port, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            break;
        }
        for (int i = 0; i < got; i++) {
            if (mesh_drv_nmea_push(&s_nmea, (char)chunk[i])) {
                gnss_heard();
                gnss_publish();
            }
        }
    }

    if (s_state == GNSS_WAKING) {
        gnss_wake_again();
        return;
    }

    /* Nothing has checked at this rate: the receiver may be the other kind. */
    if (!s_locked && (s_state == GNSS_AWAKE)
        && ((esp_timer_get_time() - s_rate_since) > (int64_t)GNSS_RATE_TRY_US)) {
        s_rate = (s_rate + 1U) % GNSS_RATE_COUNT;
        (void)uart_set_baudrate((uart_port_t)s_pins.uart_port, RATES[s_rate]);
        (void)uart_flush_input((uart_port_t)s_pins.uart_port);
        mesh_drv_nmea_reset(&s_nmea);
        s_rate_since = esp_timer_get_time();
    }
}

void mesh_drv_gnss_zero(void)
{
    if (!s_open) {
        return;
    }
    /* Keep the fix; restart the sentence counts. */
    s_nmea.good = 0U;
    s_nmea.bad  = 0U;
    taskENTER_CRITICAL(&s_lock);
    s_pub.messages = 0U;
    s_pub.bad      = 0U;
    taskEXIT_CRITICAL(&s_lock);
}

void mesh_drv_gnss_read(mesh_drv_gnss_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_pub;
    taskEXIT_CRITICAL(&s_lock);
    if (!s_open) {
        (void)memset(out, 0, sizeof(*out));
    }
}

bool mesh_drv_gnss_pps(void)
{
    return s_open && (gpio_get_level((gpio_num_t)s_pins.pps) != 0);
}
