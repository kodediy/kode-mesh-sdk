/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lr_chip.c
 * @brief The LR2021 model, and the ESP-IDF calls the driver reaches it by.
 *        See lr_chip.h for what is modelled and where the rules come from.
 */
#include "lr_chip.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mesh_bsp.h"

chip_t g_chip;
hw_t   g_hw;

#define PINS_MAX      64
#define IRQ_CMD_ERROR (1UL << 17)
#define IRQ_TX_DONE   (1UL << 19)
#define IRQ_RX_DONE   (1UL << 18)
#define IRQ_TIMEOUT   (1UL << 21)
#define IRQ_CRC_ERROR (1UL << 22)
#define RX_CONTINUOUS 0xFFFFFFUL

typedef struct {
    int         level;
    gpio_isr_t  isr;
    void       *isr_arg;
    bool        isr_enabled;
} pin_t;

static pin_t s_pin[PINS_MAX];

static struct {
    bool     active;
    unsigned n;
    uint8_t  buf[2 + 256 + 8];
} s_frame;

static uint8_t  s_resp[16];
static unsigned s_resp_n;

static chip_cmd_t s_log[CHIP_LOG_MAX];
static unsigned   s_log_n;

static int64_t s_clock_us;

/* ---- Time --------------------------------------------------------------------- */

uint64_t chip_us_to_ticks(uint64_t us)
{
    return (us * CHIP_CTRL_TICK_HZ) / 1000000ULL;
}

uint64_t chip_ticks_to_us(uint64_t ticks)
{
    return (ticks * 1000000ULL) / CHIP_CTRL_TICK_HZ;
}

int64_t fake_timer_get_time(void)
{
    s_clock_us += (int64_t)g_chip.creep_us;
    return s_clock_us;
}

void fake_rom_delay_us(uint32_t us)
{
    s_clock_us += (int64_t)us;
}

/* ---- The datasheet's airtime ---------------------------------------------------- */

uint64_t chip_lora_air_cr_us(uint8_t sf, uint8_t bw_code, uint8_t cr, bool ldro, uint16_t pbl, uint8_t payload)
{
    uint64_t bw_hz;

    /* Table 9-3, the actual bandwidths. */
    switch (bw_code) {
    case 0x02: bw_hz = 31250ULL;   break;
    case 0x03: bw_hz = 62500ULL;   break;
    case 0x04: bw_hz = 125000ULL;  break;
    case 0x05: bw_hz = 250000ULL;  break;
    case 0x06: bw_hz = 500000ULL;  break;
    case 0x07: bw_hz = 1000000ULL; break;
    case 0x0A: bw_hz = 41667ULL;   break;
    case 0x0B: bw_hz = 83333ULL;   break;
    case 0x0C: bw_hz = 101563ULL;  break;
    case 0x0D: bw_hz = 203125ULL;  break;
    case 0x0E: bw_hz = 406250ULL;  break;
    case 0x0F: bw_hz = 812500ULL;  break;
    default:   return 0U;
    }
    if ((sf < 5U) || (cr < 1U) || (cr > 4U)) {
        return 0U;
    }
    /* T_sym = 2^SF / BW, kept in nanoseconds. Under SF7 no 8-bit head start, and
     * 6.25 symbols after the preamble, not 4.25. */
    bool const     small  = sf < 7U;
    uint64_t const sym_ns = (1000000000ULL << sf) / bw_hz;
    int64_t const  de     = ldro ? 2 : 0;
    int64_t const  num    = (8 * (int64_t)payload) - (4 * (int64_t)sf) + (small ? 20 : 28) + 16;
    int64_t const  den    = 4 * ((int64_t)sf - de);
    int64_t        groups = (num > 0) ? ((num + den - 1) / den) : 0;
    uint64_t const sym_x4 = (4ULL * ((uint64_t)pbl + 8ULL + ((4ULL + (uint64_t)cr) * (uint64_t)groups)))
                            + (small ? 25ULL : 17ULL);
    return (sym_ns * sym_x4) / 4000ULL;
}

uint64_t chip_lora_air_us(uint8_t sf, uint8_t bw_code, bool ldro, uint16_t pbl, uint8_t payload)
{
    return chip_lora_air_cr_us(sf, bw_code, 1U, ldro, pbl, payload);
}

/* ---- The interrupt line ------------------------------------------------------------ */

static bool dio9_high(void)
{
    return !g_chip.absent && !g_chip.in_reset && ((g_chip.irq & g_chip.dio9_mask) != 0U);
}

/** @brief Level-triggered: while the line is high and the interrupt is on, it fires. */
static void dio9_fire(void)
{
    pin_t *p = &s_pin[PIN_DIO9];

    if (p->isr_enabled && (p->isr != NULL) && dio9_high()) {
        g_chip.n_isr++;
        p->isr(p->isr_arg);
    }
}

static void irq_set(uint32_t flags)
{
    g_chip.irq |= flags;
    dio9_fire();
}

void chip_raise(uint32_t flags)
{
    irq_set(flags);
}

/* ---- The chip ------------------------------------------------------------------------ */

static void reset_state(void)
{
    g_chip.mode             = MESH_DRV_LR2021_MODE_STANDBY_RC;
    g_chip.lora             = false;
    g_chip.stat_cmd         = CMD_OK;
    g_chip.irq              = 0U;
    g_chip.dio9_mask        = 0U;
    g_chip.errors           = 0U;
    g_chip.sf               = 0U;
    g_chip.bw               = 0U;
    g_chip.cr               = 0U;
    g_chip.ldro             = 0U;
    g_chip.pbl              = 0U;
    g_chip.sync_word        = 0x12U;   /* table 9-7: the default */
    g_chip.cr               = 1U;
    g_chip.payload_len      = 0U;
    g_chip.pkt_flags        = 0U;
    g_chip.tx_power         = 0;
    g_chip.freq_hz          = 0U;
    g_chip.tx_timeout_ticks = 0U;
    g_chip.rx_timeout_ticks = 0U;
    g_chip.dflt_rx_ticks    = 0U;
    g_chip.dflt_tx_ticks    = 0U;
    g_chip.test_cw          = false;
    g_chip.test_preamble    = false;
    g_chip.air_us           = 0U;
    g_chip.on_air_us        = 0U;
    g_chip.rx_elapsed_us    = 0U;
    g_chip.tx_fifo_n        = 0U;
    g_chip.rx_fifo_n        = 0U;
    g_chip.rx_fifo_pos      = 0U;
    g_chip.last_len         = 0U;
    g_chip.last_crc_on      = false;
    g_chip.last_snr4        = 0;
    g_chip.last_rssi2       = 0U;
    g_chip.wedged           = false;   /* a reset cures it */
    g_chip.rx_path          = 0U;
    g_chip.rx_boost         = 0U;
    s_resp_n                = 0U;
}

void chip_power_up(void)
{
    (void)memset(&g_chip, 0, sizeof(g_chip));
    (void)memset(&g_hw, 0, sizeof(g_hw));
    g_hw.isr_service_rc = ESP_OK;
    g_chip.fw_major  = MESH_BSP_LORA_FW_MAJOR;
    g_chip.fw_minor  = MESH_BSP_LORA_FW_MINOR;
    g_chip.creep_us  = 20U;
    g_chip.inst_rssi2 = 180U;   /* -90 dBm */
    reset_state();
    (void)memset(s_pin, 0, sizeof(s_pin));
    s_pin[PIN_CS].level  = 1;   /* the module's pull-ups */
    s_pin[PIN_RST].level = 1;
    s_frame.active = false;
    s_log_n        = 0U;
}

static uint16_t status_word(void)
{
    return (uint16_t)(((uint16_t)g_chip.stat_cmd << 9)
                      | ((g_chip.irq != 0U) ? 0x0100U : 0U)
                      | ((uint16_t)g_chip.mode & 0x7U));
}

/** @brief Whatever was on air or listening is not any more. */
static void stop_activity(void)
{
    g_chip.cad_on = false;
    g_chip.test_cw          = false;
    g_chip.test_preamble    = false;
    g_chip.air_us           = 0U;
    g_chip.on_air_us        = 0U;
    g_chip.tx_timeout_ticks = 0U;
    g_chip.rx_elapsed_us    = 0U;
}

static chip_cmd_t *log_new(uint16_t op, const uint8_t *p, unsigned n)
{
    if (s_log_n >= CHIP_LOG_MAX) {
        return NULL;
    }
    chip_cmd_t *e = &s_log[s_log_n++];
    (void)memset(e, 0, sizeof(*e));
    e->op                 = op;
    e->n                  = (uint8_t)((n > 255U) ? 255U : n);
    (void)memcpy(e->p, p, (n < CHIP_LOG_PARAMS) ? n : CHIP_LOG_PARAMS);
    e->mode_before        = (uint8_t)g_chip.mode;
    e->payload_len_before = g_chip.payload_len;
    e->pbl_before         = g_chip.pbl;
    e->dflt_tx_set        = (g_chip.dflt_tx_ticks != 0U) ? 1U : 0U;
    return e;
}

static void finish(chip_cmd_t *e, uint8_t st)
{
    g_chip.stat_cmd = st;
    if (e != NULL) {
        e->status = st;
    }
    if ((st == CMD_FAIL) || (st == CMD_PERR)) {
        g_chip.n_failed++;
        irq_set(IRQ_CMD_ERROR);
    }
}

static void begin_tx(uint32_t ticks, bool cw, bool preamble)
{
    g_chip.tx_timeout_ticks = ticks;
    g_chip.test_cw          = cw;
    g_chip.test_preamble    = preamble;
    g_chip.on_air_us        = 0U;
    if (cw || preamble) {
        g_chip.air_us = 0U;   /* no packet to complete */
    } else {
        if (g_chip.tx_fifo_n != g_chip.payload_len) {
            g_chip.n_tx_len_mismatch++;
        }
        g_chip.air_us = (g_chip.force_air_us != 0U)
                        ? g_chip.force_air_us
                        : chip_lora_air_cr_us(g_chip.sf, g_chip.bw, g_chip.cr, g_chip.ldro != 0U,
                                           g_chip.pbl, (uint8_t)g_chip.tx_fifo_n);
    }
    g_chip.mode = MESH_DRV_LR2021_MODE_TRANSMIT;
}

static void execute(uint16_t op, const uint8_t *p, unsigned n)
{
    chip_cmd_t *e  = log_new(op, p, n);
    uint8_t     st = CMD_OK;

    s_resp_n = 0U;
    switch (op) {
    case OPC_WRITE_TX_FIFO:
        for (unsigned i = 0U; i < n; i++) {
            if (g_chip.tx_fifo_n < sizeof(g_chip.tx_fifo)) {
                g_chip.tx_fifo[g_chip.tx_fifo_n++] = p[i];
            }
        }
        break;

    case OPC_SET_SLEEP:
        stop_activity();
        g_chip.asleep       = true;
        g_chip.sleep_config = (n > 0U) ? p[0] : 0U;
        g_chip.mode         = MESH_DRV_LR2021_MODE_SLEEP;
        g_chip.n_sleeps++;
        break;

    case OPC_GET_VERSION:
        s_resp[0] = g_chip.fw_major;
        s_resp[1] = g_chip.fw_minor;
        s_resp_n  = 2U;
        st        = CMD_DAT;
        break;

    case OPC_GET_ERRORS:
        s_resp[0] = (uint8_t)(g_chip.errors >> 8);
        s_resp[1] = (uint8_t)(g_chip.errors);
        s_resp_n  = 2U;
        st        = CMD_DAT;
        break;

    case OPC_CLEAR_ERRORS:
        g_chip.errors = g_chip.sticky_errors;   /* a fault that is still there comes back */
        break;

    case OPC_GET_CLEAR_IRQ:
        s_resp[0] = (uint8_t)(g_chip.irq >> 24);
        s_resp[1] = (uint8_t)(g_chip.irq >> 16);
        s_resp[2] = (uint8_t)(g_chip.irq >> 8);
        s_resp[3] = (uint8_t)(g_chip.irq);
        s_resp_n  = 4U;
        g_chip.irq = 0U;
        st         = CMD_DAT;
        break;

    case OPC_SET_DIO_FUNCTION:
        if (n != 2U) { st = CMD_PERR; }
        break;

    case OPC_SET_DIO_IRQ:
        if (n != 5U) {
            st = CMD_PERR;
        } else if (p[0] == 9U) {
            g_chip.dio9_mask = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16)
                               | ((uint32_t)p[3] << 8) | (uint32_t)p[4];
        }
        break;

    case OPC_CLEAR_RX_FIFO:
        g_chip.rx_fifo_n   = 0U;
        g_chip.rx_fifo_pos = 0U;
        break;

    case OPC_CLEAR_TX_FIFO:
        g_chip.tx_fifo_n = 0U;
        break;

    case OPC_CALIB_FE:
        if (n != 6U) {
            st = CMD_PERR;
        } else if (g_chip.fail_calib_fe > 0U) {
            g_chip.fail_calib_fe--;
            g_chip.errors |= g_chip.calib_fe_error;
        } else {
            /* It went well. */
        }
        break;

    case OPC_CLEAR_IRQ:
        if (n != 4U) {
            st = CMD_PERR;
        } else {
            g_chip.irq &= ~(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
        }
        break;

    case OPC_WRITE_REG_MASK:
        if (n != 11U) { st = CMD_PERR; }
        break;

    case OPC_SET_STANDBY:
        if ((n != 1U) || (p[0] > 1U)) {
            st = CMD_PERR;
        } else if (!g_chip.wedged) {
            stop_activity();
            g_chip.mode = (p[0] == 0U) ? MESH_DRV_LR2021_MODE_STANDBY_RC : MESH_DRV_LR2021_MODE_STANDBY_XOSC;
        }
        break;

    case OPC_SET_PACKET_TYPE:
        if (n != 1U) {
            st = CMD_PERR;
        } else {
            g_chip.lora = (p[0] == 0U);
        }
        break;

    case OPC_SET_RF_FREQUENCY:
        if (n != 4U) {
            st = CMD_PERR;
        } else {
            g_chip.freq_hz = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                             | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        break;

    case OPC_SET_RX_PATH:
        if (n != 2U) {
            st = CMD_PERR;
        } else if (g_chip.refuse_rx_path > 0U) {
            g_chip.refuse_rx_path--;
            g_chip.errors |= g_chip.calib_fe_error;
            st = CMD_FAIL;
        } else {
            /* It took it. */
            if (p[1] != g_chip.rx_boost) {
                g_chip.n_src_calibrations++;
            }
            g_chip.rx_path  = p[0];
            g_chip.rx_boost = p[1];
        }
        break;

    case OPC_SET_PA_CONFIG:
        if (n != 3U) { st = CMD_PERR; }
        break;

    case OPC_SET_TX_PARAMS:
        if (n != 2U) {
            st = CMD_PERR;
        } else {
            g_chip.tx_power = (int8_t)p[0];
        }
        break;

    case OPC_SET_LORA_MOD:
        if (n != 2U) {
            st = CMD_PERR;
        } else if (!g_chip.lora) {
            st = CMD_FAIL;
        } else {
            g_chip.sf   = (uint8_t)(p[0] >> 4);
            g_chip.bw   = (uint8_t)(p[0] & 0x0FU);
            g_chip.cr   = (uint8_t)(p[1] >> 4);
            g_chip.ldro = (uint8_t)(p[1] & 0x03U);
        }
        break;

    case OPC_SET_LORA_CAD_PARAMS:
        /* Table 6-17: nb_symbols, pbl_any and pnr_delta, CadExitMode, a 3-byte
         * timeout and det_peak. */
        if (n != 7U) {
            st = CMD_PERR;
        } else if (!g_chip.lora) {
            st = CMD_FAIL;
        } else {
            g_chip.cad_symbols   = p[0];
            g_chip.cad_pnr_delta = (uint8_t)(p[1] & 0x0FU);
            g_chip.cad_preamble_only = (p[1] & 0x10U) != 0U;
            g_chip.cad_exit      = p[2];
            g_chip.cad_timeout   = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | (uint32_t)p[5];
            g_chip.cad_peak      = p[6];
        }
        break;

    case OPC_SET_LORA_CAD:
        if (n != 0U) {
            st = CMD_PERR;
        } else if ((g_chip.mode == MESH_DRV_LR2021_MODE_RECEIVE) || (g_chip.mode == MESH_DRV_LR2021_MODE_TRANSMIT)
                   || !g_chip.lora) {
            st = CMD_FAIL;
        } else {
            g_chip.cad_on         = true;
            g_chip.cad_elapsed_us = 0U;
            g_chip.mode           = MESH_DRV_LR2021_MODE_RECEIVE;
        }
        break;

    case OPC_SET_LORA_SYNCWORD:
        /* Table 9-7: one byte. "If the packet type is not LoRa..." holds as elsewhere. */
        if (n != 1U) {
            st = CMD_PERR;
        } else if (!g_chip.lora) {
            st = CMD_FAIL;
        } else {
            g_chip.sync_word = p[0];
        }
        break;

    case OPC_SET_LORA_PACKET:
        /* Table 9-5: preamble (2), payload_len, then header_type / crc / invert_iq. */
        if (n != 4U) {
            st = CMD_PERR;
        } else if (!g_chip.lora) {
            st = CMD_FAIL;   /* "If the packet type is not LoRa..." */
        } else {
            g_chip.pbl         = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
            g_chip.payload_len = p[2];
            g_chip.pkt_flags   = p[3];
        }
        break;

    case OPC_SET_DEFAULT_TIMER:
        if (n != 6U) {
            st = CMD_PERR;
        } else if (g_chip.refuse_default_timer) {
            st = CMD_FAIL;
        } else {
            g_chip.dflt_rx_ticks = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            g_chip.dflt_tx_ticks = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
        }
        break;

    case OPC_SET_RX:
        if ((n != 3U) && (n != 0U)) {
            st = CMD_PERR;
        } else if (!g_chip.lora || (g_chip.mode == MESH_DRV_LR2021_MODE_RECEIVE)) {
            st = CMD_FAIL;   /* "If the device is already in Rx mode, the command fails" */
        } else {
            if (g_chip.mode == MESH_DRV_LR2021_MODE_TRANSMIT) {
                g_chip.n_rx_over_tx++;
            }
            stop_activity();
            g_chip.rx_timeout_ticks = (n == 3U)
                ? (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])
                : g_chip.dflt_rx_ticks;
            g_chip.rx_elapsed_us = 0U;
            g_chip.mode          = MESH_DRV_LR2021_MODE_RECEIVE;
        }
        break;

    case OPC_SET_TX:
        if ((n != 3U) && (n != 0U)) {
            st = CMD_PERR;
        } else if (!g_chip.lora || (g_chip.mode == MESH_DRV_LR2021_MODE_TRANSMIT)) {
            st = CMD_FAIL;   /* "If the device is already in Tx mode the command fails" */
        } else {
            begin_tx((n == 3U) ? (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])
                               : g_chip.dflt_tx_ticks,
                     false, false);
        }
        break;

    case OPC_SET_TX_TEST_MODE:
        if ((n != 1U) || (p[0] > 3U)) {
            st = CMD_PERR;
        } else if (!g_chip.lora || (g_chip.mode == MESH_DRV_LR2021_MODE_TRANSMIT) || (p[0] == 3U)) {
            st = CMD_FAIL;   /* PRBS9 is "not available in LoRa" */
        } else if (p[0] == 0U) {
            begin_tx(g_chip.dflt_tx_ticks, false, false);   /* "same as a SetTx(0)" */
        } else if (g_chip.refuse_test_with_timer && (g_chip.dflt_tx_ticks != 0U)) {
            st = CMD_FAIL;
        } else {
            begin_tx(g_chip.dflt_timer_stops_test ? g_chip.dflt_tx_ticks : 0U,
                     p[0] == 2U, p[0] == 1U);
        }
        break;

    case OPC_GET_RSSI_INST:
        s_resp[0] = (uint8_t)(g_chip.inst_rssi2 >> 1);
        s_resp[1] = (uint8_t)((g_chip.inst_rssi2 & 1U) << 7);
        s_resp_n  = 2U;
        st        = CMD_DAT;
        break;

    case OPC_GET_LORA_PKT_STAT:
        /* Table 9-13: crc / coding_rate, pkt_length, snr_pkt, rssi_pkt(8:1),
         * rssi_signal_pkt(8:1), then detector and the two low RSSI bits. */
        s_resp[0] = (uint8_t)((g_chip.last_crc_on ? 0x10U : 0x00U) | 0x01U);
        s_resp[1] = g_chip.last_len;
        s_resp[2] = (uint8_t)g_chip.last_snr4;
        s_resp[3] = (uint8_t)(g_chip.last_rssi2 >> 1);
        s_resp[4] = (uint8_t)(g_chip.last_rssi2 >> 1);
        s_resp[5] = (uint8_t)(0x04U | ((g_chip.last_rssi2 & 1U) << 1) | (g_chip.last_rssi2 & 1U));
        s_resp_n  = 6U;
        st        = CMD_DAT;
        break;

    default:
        st = CMD_PERR;   /* an opcode this model does not know is the driver's mistake */
        break;
    }
    finish(e, st);
}

/* ---- Time passing in the chip --------------------------------------------------------- */

static void tx_ended(uint32_t flag, bool whole)
{
    stop_activity();
    if (whole) {
        g_chip.tx_fifo_n = 0U;   /* a packet that left whole emptied the FIFO */
    }
    g_chip.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;   /* the fallback mode */
    irq_set(flag);
}

void chip_advance_us(uint64_t us)
{
    if (g_chip.in_reset || g_chip.absent) {
        return;
    }
    if (g_chip.mode == MESH_DRV_LR2021_MODE_TRANSMIT) {
        g_chip.on_air_us += us;
        bool const     has_to = g_chip.tx_timeout_ticks != 0U;
        uint64_t const to_us  = chip_ticks_to_us(g_chip.tx_timeout_ticks);
        bool const     has_end = g_chip.air_us != 0U;

        if (has_end && (!has_to || (g_chip.air_us <= to_us))) {
            if (g_chip.on_air_us >= g_chip.air_us) {
                tx_ended(IRQ_TX_DONE, true);
            }
        } else if (has_to && (g_chip.on_air_us >= to_us)) {
            tx_ended(IRQ_TIMEOUT, false);
        }
    } else if ((g_chip.mode == MESH_DRV_LR2021_MODE_RECEIVE) && g_chip.cad_on) {
        uint64_t const one = chip_lora_air_us(g_chip.sf, g_chip.bw, false, 16U, 1U)
                             - chip_lora_air_us(g_chip.sf, g_chip.bw, false, 15U, 1U);
        g_chip.cad_elapsed_us += us;
        if (g_chip.cad_elapsed_us >= (one * g_chip.cad_symbols)) {
            g_chip.cad_on = false;
            if (g_chip.channel_active && (g_chip.cad_exit == 0x01U)) {
                /* CAD_RX (table 6-18): a detection stays in Rx for the packet, a single
                 * receive with no timer, and the fallback mode after it. */
                g_chip.rx_elapsed_us    = 0U;
                g_chip.rx_timeout_ticks = 0U;
            } else {
                g_chip.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
            }
            irq_set((1UL << 20) | (g_chip.channel_active ? (1UL << 7) : 0UL));
        }
    } else if (g_chip.mode == MESH_DRV_LR2021_MODE_RECEIVE) {
        g_chip.rx_elapsed_us += us;
        if ((g_chip.rx_timeout_ticks != 0U) && (g_chip.rx_timeout_ticks != RX_CONTINUOUS)
            && (g_chip.rx_elapsed_us >= chip_ticks_to_us(g_chip.rx_timeout_ticks))) {
            stop_activity();
            g_chip.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;
            irq_set(IRQ_TIMEOUT);
        }
    }
}

bool chip_rx_packet(const uint8_t *data, uint8_t len, bool crc_on, bool crc_good)
{
    if (g_chip.in_reset || g_chip.absent || (g_chip.mode != MESH_DRV_LR2021_MODE_RECEIVE)) {
        g_chip.n_dropped_rx++;
        return false;
    }
    /* Explicit header: payload_len 0 takes any length, otherwise only 1..payload_len
     * and the rest is a header error with no RxDone (table 9-5). */
    if ((g_chip.payload_len != 0U) && ((len == 0U) || (len > g_chip.payload_len))) {
        g_chip.n_header_err++;
        irq_set(CHIP_IRQ_HEADER_ERR);
        return false;
    }
    for (unsigned i = 0U; i < len; i++) {
        if (g_chip.rx_fifo_n < sizeof(g_chip.rx_fifo)) {
            g_chip.rx_fifo[g_chip.rx_fifo_n++] = data[i];
        }
    }
    g_chip.last_len    = len;
    g_chip.last_crc_on = crc_on;
    g_chip.last_snr4   = 20;     /* +5 dB */
    g_chip.last_rssi2  = 180U;   /* -90 dBm */
    if (g_chip.rx_timeout_ticks != RX_CONTINUOUS) {
        stop_activity();
        g_chip.mode = MESH_DRV_LR2021_MODE_STANDBY_RC;   /* single receive falls back */
    }
    irq_set(IRQ_RX_DONE | ((crc_on && !crc_good) ? IRQ_CRC_ERROR : 0U));
    return true;
}

/* ---- The log --------------------------------------------------------------------------- */

unsigned chip_count(uint16_t op)
{
    unsigned n = 0U;

    for (unsigned i = 0U; i < s_log_n; i++) {
        if (s_log[i].op == op) {
            n++;
        }
    }
    return n;
}

const chip_cmd_t *chip_nth(uint16_t op, unsigned nth)
{
    for (unsigned i = 0U; i < s_log_n; i++) {
        if (s_log[i].op == op) {
            if (nth == 0U) {
                return &s_log[i];
            }
            nth--;
        }
    }
    return NULL;
}

const chip_cmd_t *chip_last(uint16_t op)
{
    for (unsigned i = s_log_n; i > 0U; i--) {
        if (s_log[i - 1U].op == op) {
            return &s_log[i - 1U];
        }
    }
    return NULL;
}

unsigned chip_log_len(void)
{
    return s_log_n;
}

const chip_cmd_t *chip_log_at(unsigned i)
{
    return (i < s_log_n) ? &s_log[i] : NULL;
}

void chip_log_clear(void)
{
    s_log_n                  = 0U;
    g_chip.n_failed          = 0U;
    g_chip.n_header_err      = 0U;
    g_chip.n_rx_over_tx      = 0U;
    g_chip.n_tx_len_mismatch = 0U;
    g_chip.n_resets          = 0U;
    g_chip.n_isr             = 0U;
    g_chip.n_dropped_rx      = 0U;
    g_hw.nss_frames          = 0U;
}

/* ---- The pins ------------------------------------------------------------------------------ */

static int64_t s_nss_low_at;   /* when NSS last fell, on the driver's clock */

static void frame_begin(void)
{
    /* Asleep, the chip hears nothing on the SPI: NSS low only wakes it (4.4.1). */
    s_frame.active = !g_chip.in_reset && !g_chip.absent && !g_chip.asleep;
    s_frame.n      = 0U;
    s_nss_low_at   = s_clock_us;
    g_hw.nss_frames++;
}

static void frame_end(void)
{
    if (g_chip.asleep && !g_chip.absent && ((s_clock_us - s_nss_low_at) >= 100)) {
        /* NSS held low 100 us: a cold start (no retention in this model's use), in standby RC
         * with the defaults, as after a reset. */
        g_chip.asleep = false;
        g_chip.n_wakes++;
        reset_state();
        return;
    }
    if (!s_frame.active) {
        return;
    }
    s_frame.active = false;
    if (s_frame.n < 2U) {
        return;
    }
    uint16_t const op = (uint16_t)(((uint16_t)s_frame.buf[0] << 8) | s_frame.buf[1]);
    if (op == OPC_READ_RX_FIFO) {
        if (g_chip.rx_fifo_pos >= g_chip.rx_fifo_n) {
            g_chip.rx_fifo_n   = 0U;
            g_chip.rx_fifo_pos = 0U;
        }
        return;
    }
    if ((op == 0x0000U) || (op == OPC_GET_STATUS)) {
        return;   /* an answer being read, or the status: nothing to run */
    }
    unsigned const n = (s_frame.n > sizeof(s_frame.buf)) ? (unsigned)sizeof(s_frame.buf) : s_frame.n;
    execute(op, &s_frame.buf[2], n - 2U);
}

static void rst_edge(int level)
{
    if (level == 0) {
        g_chip.in_reset = true;
        g_chip.asleep   = false;   /* a reset wakes it (4.4.1) */
        s_frame.active  = false;
        stop_activity();                           /* no carrier, no packet, no listen */
        g_chip.mode     = MESH_DRV_LR2021_MODE_SLEEP;   /* silent while held */
    } else if (g_chip.in_reset) {
        g_chip.in_reset = false;
        g_chip.n_resets++;
        reset_state();
    }
}

esp_err_t gpio_config(const gpio_config_t *c)
{
    (void)c;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    int const was = s_pin[gpio_num].level;
    s_pin[gpio_num].level = (level != 0U) ? 1 : 0;
    if (gpio_num == PIN_CS) {
        if ((was != 0) && (level == 0U)) {
            frame_begin();
        } else if ((was == 0) && (level != 0U)) {
            frame_end();
        }
    } else if (gpio_num == PIN_RST) {
        if (was != s_pin[gpio_num].level) {
            rst_edge(s_pin[gpio_num].level);
        }
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return 0;
    }
    if (gpio_num == PIN_BUSY) {
        /* Pulled up: an empty header, a chip held in reset and a stuck chip read busy. */
        return (g_chip.absent || g_chip.in_reset || g_chip.busy_stuck || g_chip.asleep) ? 1 : 0;
    }
    if (gpio_num == PIN_DIO9) {
        return dio9_high() ? 1 : 0;
    }
    return s_pin[gpio_num].level;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pin[gpio_num].isr_enabled = false;
    if ((gpio_num == PIN_CS) || (gpio_num == PIN_RST)) {
        /* High-Z: the module's own 10 k pull-ups take the line high. */
        (void)gpio_set_level(gpio_num, 1U);
    }
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    (void)gpio_num;
    (void)intr_type;
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    (void)gpio_num;
    (void)pull;
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pin[gpio_num].isr_enabled = true;
    if (gpio_num == PIN_DIO9) {
        if (g_hw.on_intr_enable != NULL) {
            g_hw.on_intr_enable();
        }
        dio9_fire();   /* level-triggered: already high means it fires now */
    }
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pin[gpio_num].isr_enabled = false;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    if (g_hw.isr_service_rc == ESP_OK) {
        g_hw.isr_service_up = true;
    }
    return (esp_err_t)g_hw.isr_service_rc;
}

esp_err_t gpio_uninstall_isr_service(void)
{
    g_hw.isr_service_up = false;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX) || g_hw.isr_add_fails) {
        return ESP_FAIL;
    }
    s_pin[gpio_num].isr     = isr_handler;
    s_pin[gpio_num].isr_arg = args;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    if ((gpio_num < 0) || (gpio_num >= PINS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pin[gpio_num].isr     = NULL;
    s_pin[gpio_num].isr_arg = NULL;
    return ESP_OK;
}

int pin_level(int gpio)
{
    return gpio_get_level(gpio);
}

bool pin_isr_enabled(void)
{
    return s_pin[PIN_DIO9].isr_enabled;
}

/* ---- The SPI bus ----------------------------------------------------------------------------- */

struct spi_device_t {
    int unused;
};

static struct spi_device_t s_dev_obj;

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *bus_config,
                             spi_dma_chan_t dma_chan)
{
    (void)bus_config;
    (void)dma_chan;
    g_hw.bus_host = (int)host_id;
    if (g_hw.spi_init_fails) {
        return ESP_FAIL;
    }
    g_hw.buses_up++;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    (void)host_id;
    if (g_hw.buses_up > 0U) {
        g_hw.buses_up--;
    }
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle)
{
    g_hw.device_host = (int)host_id;
    (void)dev_config;
    if (g_hw.spi_add_fails) {
        return ESP_FAIL;
    }
    g_hw.devices_up++;
    *handle = &s_dev_obj;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
    (void)handle;
    if (g_hw.devices_up > 0U) {
        g_hw.devices_up--;
    }
    return ESP_OK;
}

esp_err_t spi_device_acquire_bus_host(spi_device_handle_t device, uint64_t wait)
{
    (void)device;
    (void)wait;
    g_hw.bus_acquired = true;
    return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t dev)
{
    (void)dev;
    g_hw.bus_acquired = false;
}

/** @brief One byte clocked through the chip: what it hears, what it says. */
static uint8_t frame_byte(uint8_t in)
{
    if (!s_frame.active) {
        return 0U;
    }
    unsigned const idx = s_frame.n;
    if (idx < sizeof(s_frame.buf)) {
        s_frame.buf[idx] = in;
    }
    s_frame.n++;

    uint16_t const stat = status_word();
    if (idx == 0U) {
        return (uint8_t)(stat >> 8);
    }
    if (idx == 1U) {
        return (uint8_t)(stat & 0xFFU);
    }
    uint16_t const op = (uint16_t)(((uint16_t)s_frame.buf[0] << 8) | s_frame.buf[1]);
    unsigned const k  = idx - 2U;
    if (op == 0x0000U) {
        return (k < s_resp_n) ? s_resp[k] : 0U;   /* the answer to the last command */
    }
    if (op == OPC_READ_RX_FIFO) {
        return (g_chip.rx_fifo_pos < g_chip.rx_fifo_n) ? g_chip.rx_fifo[g_chip.rx_fifo_pos++] : 0U;
    }
    /* GetStatus and every command frame: the interrupt word follows the status. */
    return (k < 4U) ? (uint8_t)(g_chip.irq >> (24U - (8U * k))) : 0U;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans_desc)
{
    (void)handle;
    unsigned const    n  = (unsigned)(trans_desc->length / 8U);
    const uint8_t    *tx = trans_desc->tx_buffer;
    uint8_t          *rx = trans_desc->rx_buffer;

    if (g_hw.spi_fail_in > 0U) {
        g_hw.spi_fail_in--;
        if (g_hw.spi_fail_in == 0U) {
            return ESP_FAIL;
        }
    }
    for (unsigned i = 0U; i < n; i++) {
        uint8_t const out = frame_byte((tx != NULL) ? tx[i] : 0U);
        if (rx != NULL) {
            rx[i] = out;
        }
    }
    return ESP_OK;
}

/* ---- esp_restart()'s shutdown handlers ------------------------------------------------------------ */

#define SHUTDOWN_SLOTS 5U

static shutdown_handler_t s_sd[SHUTDOWN_SLOTS];
static unsigned           s_sd_n;
static bool               s_sd_full;

esp_err_t fake_register_shutdown_handler(shutdown_handler_t handle)
{
    for (unsigned i = 0U; i < s_sd_n; i++) {
        if (s_sd[i] == handle) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_sd_full || (s_sd_n >= SHUTDOWN_SLOTS)) {
        return ESP_ERR_NO_MEM;
    }
    s_sd[s_sd_n++] = handle;
    return ESP_OK;
}

void shutdown_run(void)
{
    for (unsigned i = 0U; i < s_sd_n; i++) {
        s_sd[i]();
    }
}

unsigned shutdown_count(void)
{
    return s_sd_n;
}

void shutdown_fill(bool full)
{
    s_sd_full = full;
}
