/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_misc.c
 * @brief The rest of what the owner stands on in its host suite: the pins that
 *        make a Mesh look like one, a GNSS that says nothing, kode.h's clock and
 *        header lending, and the log. See owner_fakes.h.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_sys.h"
#include "kode_log.h"
#include "mesh_bsp.h"
#include "mesh_drv_gnss.h"
#include "mesh_drv_pins.h"
#include "owner_fakes.h"

/* ---- kode.h ------------------------------------------------------------------ */

/* The clock, and the hours a case has let pass at once (h_hour_passes()).
 *
 * ONE CLOCK FOR THE WHOLE SUITE: THE SIMULATOR'S TICKS. The owner waits for an answer in ticks
 * (xSemaphoreTake(), vTaskDelay()) and reads its deadlines here; the harness waits here too
 * (WAIT_FOR). On a board the two clocks run together. On the host they do not: a process that
 * is held back gets its ticks late and loses some, while the computer's clock goes on, so with
 * the computer's clock here a wait of 3100 ms in ticks could outlast a window of 5100 ms and a
 * case failed on a busy machine for no fault of the owner's. Counted in ticks, every wait of
 * the suite stretches by the same amount and the cases say the same on any machine. Its grain
 * is one tick, portTICK_PERIOD_MS. */
static volatile uint32_t s_skip_ms;

void fake_clock_skip(uint32_t ms)
{
    s_skip_ms += ms;
}

uint32_t kode_sys_time_ms(void)
{
    return ((uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS) + s_skip_ms;
}

exp_fake_t g_exp = { .power_rc = KODE_OK, .spi_id = 2U, .uart_id = 1U };

/* ---- The plan's one question that this suite answers (owner_fakes.h) ----------- */

volatile bool g_plan_test_allowed = true;

bool mesh_plan_test_allowed(void)
{
    return g_plan_test_allowed;
}

/* Which loans are out: positions lent and controllers booked, each under its
 * tag. A tag is given back whole, as kodeOS does; a case reads whether the
 * owner still holds the module. */
#define LOANS_MAX  12U

static pthread_mutex_t s_loan_mu = PTHREAD_MUTEX_INITIALIZER;
static const char     *s_loans[LOANS_MAX];

kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio)
{
    g_exp.lends_total++;
    if (strcmp(tag, MESH_BSP_LOAN_TAG) == 0) {
        g_exp.lends_of_module++;
    }
    if ((g_exp.refuse_lend != NULL) && (strcmp(tag, g_exp.refuse_lend) == 0)) {
        return KODE_NO_RESOURCES;
    }
    for (size_t i = 0U; i < n; i++) {
        gpio[i] = pos[i];
    }
    (void)pthread_mutex_lock(&s_loan_mu);
    for (unsigned i = 0U; i < LOANS_MAX; i++) {
        if (s_loans[i] == NULL) {
            s_loans[i] = tag;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_loan_mu);
    return KODE_OK;
}

kode_status_t kode_sys_exp_return(const char *tag)
{
    (void)pthread_mutex_lock(&s_loan_mu);
    for (unsigned i = 0U; i < LOANS_MAX; i++) {
        if ((s_loans[i] != NULL) && (strcmp(s_loans[i], tag) == 0)) {
            s_loans[i] = NULL;
        }
    }
    (void)pthread_mutex_unlock(&s_loan_mu);
    return KODE_OK;
}

kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id)
{
    if (((bus == KODE_SYS_EXP_SPI) && g_exp.spi_refused) || ((bus == KODE_SYS_EXP_UART) && g_exp.uart_refused)) {
        return KODE_NO_RESOURCES;
    }
    (void)pthread_mutex_lock(&s_loan_mu);
    for (unsigned i = 0U; i < LOANS_MAX; i++) {
        if (s_loans[i] == NULL) {
            s_loans[i] = tag;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_loan_mu);
    *id = (bus == KODE_SYS_EXP_SPI) ? g_exp.spi_id : g_exp.uart_id;
    return KODE_OK;
}

kode_status_t kode_sys_exp_power(bool on)
{
    if (on) {
        g_exp.power_on_calls++;
    } else {
        g_exp.power_off_calls++;
    }
    void (*const hook)(bool) = g_exp.power_hook;
    if (hook != NULL) {
        hook(on);
    }
    return g_exp.power_rc;
}

bool fake_loaned(const char *tag)
{
    bool out = false;

    (void)pthread_mutex_lock(&s_loan_mu);
    for (unsigned i = 0U; i < LOANS_MAX; i++) {
        if ((s_loans[i] != NULL) && (strcmp(s_loans[i], tag) == 0)) {
            out = true;
        }
    }
    (void)pthread_mutex_unlock(&s_loan_mu);
    return out;
}

/* ---- The pins: a Mesh's fingerprint ------------------------------------------ */

static volatile bool s_mesh_present = true;

void pins_fake_set_mesh(bool present)
{
    s_mesh_present = present;
}

void mesh_drv_pins_high_impedance(uint8_t gpio)
{
    (void)gpio;
}

void mesh_drv_pins_pull_down(uint8_t gpio)
{
    (void)gpio;
}

bool mesh_drv_pins_level(uint8_t gpio)
{
    /* kode_sys_exp_lend() above lends a position as its own GPIO number. The
     * module's pull-ups hold the four lines of its fingerprint high. */
    return s_mesh_present
           && ((gpio == MESH_BSP_POSITION_LORA_RESET) || (gpio == MESH_BSP_POSITION_LORA_CS)
               || (gpio == MESH_BSP_POSITION_I2C_SCL) || (gpio == MESH_BSP_POSITION_I2C_SDA));
}

/* ---- The GNSS: open and silent ----------------------------------------------- */

gnss_fake_t g_gnss;

kode_status_t mesh_drv_gnss_open(const mesh_drv_gnss_pins_t *pins)
{
    if (g_gnss.open_rc != KODE_OK) {
        return g_gnss.open_rc;
    }
    g_gnss.pins    = *pins;
    g_gnss.is_open = true;
    g_gnss.opens++;
    return KODE_OK;
}

void mesh_drv_gnss_close(void)
{
    g_gnss.is_open = false;
    g_gnss.asleep  = false;
    g_gnss.closes++;
}

void mesh_drv_gnss_restart(void)
{
    g_gnss.restarts++;
}

/* The driver's rule: open, awake, and found talking at the ME31GR02's rate. */
kode_status_t mesh_drv_gnss_sleep(void)
{
    if (!g_gnss.is_open || g_gnss.asleep || !g_gnss.report.talking
        || (g_gnss.report.baud != MESH_BSP_GNSS_BAUD)) {
        return KODE_DENIED;
    }
    if (g_gnss.sleep_rc != KODE_OK) {
        return g_gnss.sleep_rc;
    }
    g_gnss.asleep = true;
    g_gnss.report.asleep = true;
    g_gnss.sleeps++;
    return KODE_OK;
}

kode_status_t mesh_drv_gnss_wake(void)
{
    if (!g_gnss.is_open) {
        return KODE_DENIED;
    }
    if (g_gnss.asleep) {
        g_gnss.asleep = false;
        g_gnss.report.asleep = false;
        g_gnss.wakes++;
        return g_gnss.wake_rc;
    }
    return KODE_OK;
}

void mesh_drv_gnss_poll(void)
{
}

void mesh_drv_gnss_zero(void)
{
}

void mesh_drv_gnss_read(mesh_drv_gnss_status_t *out)
{
    if (g_gnss.is_open) {
        *out = g_gnss.report;
    } else {
        (void)memset(out, 0, sizeof(*out));   /* zeroed when closed, as the driver's */
    }
}

bool mesh_drv_gnss_pps(void)
{
    return false;
}

/* ---- The log ----------------------------------------------------------------- */

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

unsigned fake_log_matches(const char *needle)
{
    unsigned count = 0U;

    (void)pthread_mutex_lock(&s_log_mu);
    unsigned const n = (s_log_n < LOG_LINES) ? s_log_n : LOG_LINES;
    for (unsigned i = 0U; i < n; i++) {
        if (strstr(s_lines[i], needle) != NULL) {
            count++;
        }
    }
    (void)pthread_mutex_unlock(&s_log_mu);
    return count;
}
