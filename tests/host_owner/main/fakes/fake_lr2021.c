/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_lr2021.c
 * @brief The LR2021 as the owner sees it: a state machine with the behaviours
 *        a case needs to provoke. See owner_fakes.h.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mesh_bsp.h"
#include "mesh_drv_lr2021.h"
#include "owner_fakes.h"

lr_fake_t g_lr;
mesh_drv_lr2021_pins_t g_lr_pins;

static SemaphoreHandle_t s_gate_sem;

/* ---- Controls --------------------------------------------------------------- */

void lr_fake_release(void)
{
    if (s_gate_sem == NULL) {
        return;
    }
    g_lr.gate = LR_GATE_NONE;
    if (g_lr.blocked) {
        (void)xSemaphoreGive(s_gate_sem);
    }
}

void lr_fake_reset(void)
{
    lr_fake_release();
    for (int i = 0; (i < 200) && g_lr.blocked; i++) {
        vTaskDelay(1);   /* let the owner out of a gate before the state is wiped */
    }
    if (s_gate_sem == NULL) {
        s_gate_sem = xSemaphoreCreateBinary();
    }
    (void)xSemaphoreTake(s_gate_sem, 0);   /* no stale release */

    (void)memset(&g_lr, 0, sizeof(g_lr));
    g_lr.mode         = MESH_DRV_LR2021_MODE_STANDBY_RC;
    g_lr.alive        = true;
    g_lr.guarded      = true;
    g_lr.configure_rc = KODE_OK;
    g_lr.calibrate_rc = KODE_OK;
    g_lr.send_rc      = KODE_OK;
    g_lr.test_rc      = KODE_OK;
    g_lr.receive_rc   = KODE_OK;
    g_lr.read_rc      = KODE_OK;
    g_lr.take_rc      = KODE_OK;
    g_lr.listen_rc    = KODE_OK;
    g_lr.cad_rc       = KODE_OK;
    g_lr.retune_rc    = KODE_OK;
    g_lr.calibrate_at_rc = KODE_OK;
    g_lr.sleep_rc     = KODE_OK;
    g_lr.rx_rssi      = -90.0f;
    g_lr.rx_snr       = 5.0f;
}

void lr_fake_gate(lr_gate_t where)
{
    g_lr.gate = where;
}

/** @brief Stand still here if a case asked for it, until it lets go. */
static void gate_point(lr_gate_t where)
{
    if (g_lr.gate == where) {
        g_lr.gate    = LR_GATE_NONE;
        g_lr.blocked = true;
        (void)xSemaphoreTake(s_gate_sem, portMAX_DELAY);
        g_lr.blocked = false;
    }
}

void lr_fake_raise(uint32_t flags)
{
    (void)__atomic_fetch_or(&g_lr.irq, flags, __ATOMIC_SEQ_CST);
}

void lr_fake_chip_stops(uint32_t flags)
{
    g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
    lr_fake_raise(flags);
}

void lr_fake_inject(const uint8_t *data, uint8_t len, bool crc_on)
{
    (void)memcpy(g_lr.rx_data, data, len);
    g_lr.rx_len    = len;
    g_lr.rx_crc_on = crc_on;
}

/** @brief A call the real driver refuses while the chip sleeps: counted, and refused. */
#define asleep_refuses() asleep_refuses_at(__func__)
static bool asleep_refuses_at(const char *who)
{
    if (g_lr.asleep) {
        g_lr.n_asleep_calls++;
        g_lr.asleep_last = who;
        return true;
    }
    return false;
}

/* ---- The driver's calls ------------------------------------------------------ */

kode_status_t mesh_drv_lr2021_open(const mesh_drv_lr2021_pins_t *pins, mesh_drv_lr2021_interrupt_callback_t on_irq, void *arg)
{
    (void)on_irq;
    (void)arg;
    g_lr_pins = *pins;
    if (g_lr.open_rc != KODE_OK) {
        return g_lr.open_rc;
    }
    g_lr.n_open++;
    return KODE_OK;
}

void mesh_drv_lr2021_close(void)
{
    g_lr.n_close++;
}

kode_status_t mesh_drv_lr2021_reset(uint8_t *major, uint8_t *minor)
{
    g_lr.n_reset++;
    if (g_lr.reset_fails) {
        return KODE_IO_ERROR;
    }
    g_lr.held          = false;
    g_lr.asleep        = false;   /* any reset wakes it */
    g_lr.mode          = MESH_DRV_LR2021_MODE_STANDBY_RC;
    g_lr.standby_fails = 0U;   /* a reset cures a wedged chip */
    g_lr.irq           = 0U;
    *major = MESH_BSP_LORA_FW_MAJOR;
    *minor = 18U;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_configure(const mesh_drv_lora_settings_t *cfg)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_configure++;
    g_lr.cfg = *cfg;
    gate_point(LR_GATE_CONFIGURE);
    return g_lr.configure_rc;
}

kode_status_t mesh_drv_lr2021_calibrate(void)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_calibrate++;
    return g_lr.calibrate_rc;
}

kode_status_t mesh_drv_lr2021_send(const uint8_t *data, uint8_t len)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_send++;
    (void)memcpy(g_lr.sent, data, len);
    g_lr.sent_len = len;
    gate_point(LR_GATE_SEND);
    if (g_lr.send_rc != KODE_OK) {
        return g_lr.send_rc;
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_TRANSMIT;   /* and it stays there until something ends it */
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_read(uint8_t *buf, uint8_t cap, uint32_t irq, mesh_drv_lr2021_packet_t *rx)
{
    g_lr.n_read++;
    g_lr.read_irq = irq;
    if (g_lr.read_rc != KODE_OK) {
        return g_lr.read_rc;   /* the bus failed: nothing was read, the FIFO is as it was */
    }
    uint8_t const n = (g_lr.rx_len < cap) ? g_lr.rx_len : cap;
    (void)memcpy(buf, g_lr.rx_data, n);
    rx->len      = n;
    /* the driver's formula (mesh_drv_lr2021.c): a CRC was carried and no CrcError */
    rx->crc_ok   = g_lr.rx_crc_on && ((irq & MESH_DRV_LR2021_INTERRUPT_CRC_ERROR) == 0U);
    rx->rssi_dbm = g_lr.rx_rssi;
    rx->snr_db   = g_lr.rx_snr;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_rssi(float *dbm)
{
    g_lr.n_rssi++;
    *dbm = -88.5f;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_test(mesh_drv_lr2021_test_t mode, uint32_t ms)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_test++;
    g_lr.test_mode = mode;
    g_lr.test_ms   = ms;
    if (g_lr.test_rc != KODE_OK) {
        return g_lr.test_rc;
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_TRANSMIT;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_receive(void)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_receive++;
    if (g_lr.receive_rc != KODE_OK) {
        return g_lr.receive_rc;
    }
    if (g_lr.mode == MESH_DRV_LR2021_MODE_TRANSMIT) {
        return KODE_DENIED;   /* the driver never aborts a transmit from here */
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_RECEIVE;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_flush_receive(void)
{
    g_lr.n_flush++;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_calibrate_at(const uint32_t *frequencies_hz, uint8_t count)
{
    g_lr.n_calibrate_at++;
    for (uint8_t i = 0U; i < count; i++) {
        g_lr.calibrated_at[i] = frequencies_hz[i];
    }
    g_lr.calibrated_count = count;
    return g_lr.calibrate_at_rc;
}

kode_status_t mesh_drv_lr2021_retune(const mesh_drv_lora_settings_t *cfg)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_retune++;
    if (g_lr.retune_rc != KODE_OK) {
        return g_lr.retune_rc;
    }
    g_lr.cfg  = *cfg;
    g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_XOSC;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_send_preamble(const uint8_t *data, uint8_t len, uint16_t preamble)
{
    g_lr.n_send_preamble++;
    g_lr.sent_preamble = preamble;
    return mesh_drv_lr2021_send(data, len);
}

kode_status_t mesh_drv_lr2021_cad(uint8_t symbols, bool fast)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_cad++;
    g_lr.cad_symbols = symbols;
    g_lr.cad_fast    = fast;
    if (g_lr.cad_rc != KODE_OK) {
        g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
        return g_lr.cad_rc;
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;   /* where it falls back to */
    if (g_lr.cad_finish != 0U) {
        lr_fake_raise(g_lr.cad_finish);
    }
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_cad_receive(uint8_t symbols, bool fast)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_cad_receive++;
    g_lr.cad_symbols = symbols;
    g_lr.cad_fast    = fast;
    if (g_lr.cad_rc != KODE_OK) {
        g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
        return g_lr.cad_rc;
    }
    /* CAD_RX: a detection stays in Rx for the packet, nothing seen falls back. */
    if ((g_lr.cad_finish & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U) {
        g_lr.mode = MESH_DRV_LR2021_MODE_RECEIVE;
    } else {
        g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
    }
    if (g_lr.cad_finish != 0U) {
        lr_fake_raise(g_lr.cad_finish);
    }
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_listen_for(uint32_t ms)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    (void)ms;
    g_lr.n_listen++;
    if (g_lr.listen_rc != KODE_OK) {
        return g_lr.listen_rc;
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_RECEIVE;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_sleep(void)
{
    g_lr.n_sleep++;
    if (g_lr.asleep) {
        return KODE_OK;
    }
    if (g_lr.sleep_rc != KODE_OK) {
        return g_lr.sleep_rc;
    }
    g_lr.asleep = true;
    g_lr.mode   = MESH_DRV_LR2021_MODE_SLEEP;
    return KODE_OK;
}

bool mesh_drv_lr2021_asleep(void)
{
    return g_lr.asleep;
}

kode_status_t mesh_drv_lr2021_standby(void)
{
    if (asleep_refuses()) {
        return KODE_DENIED;
    }
    g_lr.n_standby++;
    if (g_lr.standby_fails > 0U) {
        g_lr.standby_fails--;
        return KODE_IO_ERROR;   /* the chip stays exactly as it was */
    }
    g_lr.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_interrupts_read(uint32_t *flags)
{
    g_lr.n_take++;
    uint32_t const f = __atomic_exchange_n(&g_lr.irq, 0U, __ATOMIC_SEQ_CST);
    if (g_lr.take_rc != KODE_OK) {
        *flags = 0U;   /* the chip cleared them; the answer never came back */
        return g_lr.take_rc;
    }
    *flags = f;
    return KODE_OK;
}

uint32_t mesh_drv_lr2021_interrupts_take(void)
{
    uint32_t flags = 0U;
    (void)mesh_drv_lr2021_interrupts_read(&flags);
    return flags;
}

kode_status_t mesh_drv_lr2021_mode(mesh_drv_lr2021_mode_t *mode)
{
    if (g_lr.mode_fails) {
        return KODE_IO_ERROR;
    }
    *mode = g_lr.mode;
    return KODE_OK;
}

uint16_t mesh_drv_lr2021_errors(void)
{
    return g_lr.errors;
}

void mesh_drv_lr2021_errors_clear(void)
{
    g_lr.errors = 0U;
}

bool mesh_drv_lr2021_alive(void)
{
    if (g_lr.asleep && g_lr.alive_leaves_awake && g_lr.alive) {
        g_lr.alive_leaves_awake = false;   /* once: woken for the question, SetSleep refused */
        g_lr.asleep = false;
        g_lr.mode   = MESH_DRV_LR2021_MODE_STANDBY_RC;
    }
    return g_lr.alive && !g_lr.held;   /* a chip in reset answers nothing */
}

void mesh_drv_lr2021_hold_reset(void)
{
    g_lr.n_hold++;
    g_lr.held = true;
    g_lr.asleep = false;
    g_lr.mode = MESH_DRV_LR2021_MODE_SLEEP;   /* in reset it is silent */
}

bool mesh_drv_lr2021_guarded(void)
{
    return g_lr.guarded;
}

bool mesh_drv_lr2021_busy(void)
{
    return false;
}

bool mesh_drv_lr2021_dio9(void)
{
    return g_lr.irq != 0U;
}
