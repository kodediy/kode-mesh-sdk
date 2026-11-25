/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_lr2021.c
 * @brief The LR2021 over SPI. See mesh_drv_lr2021.h.
 *
 * The frames are the chip's own: a 16-bit opcode, most significant byte
 * first, then its parameters, all inside one chip select; an answer comes in
 * a second frame of zeros whose first two bytes are the status word. The one
 * exception is the FIFO, whose data shares the opcode's frame with no status.
 *
 * The SPI controller runs without DMA, which caps one transaction at 64 bytes
 * and costs no memory per frame. A packet's payload is longer than that, so
 * chip select is a plain output held low across as many transactions as a
 * frame needs. The GPIO is reached through ESP-IDF's driver directly - this
 * is the driver layer, the one place that may (tools/check_layers.sh, rule 1).
 */
#include "mesh_drv_lr2021.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_system.h"      /* esp_register_shutdown_handler(): the restart hook */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode_log.h"
#include "mesh_bsp.h"
#include "mesh_region.h"

#define TAG               "mesh.lr2021"

#define LR_SPI_HZ         MESH_BSP_SPI_HZ
#define LR_PIECE_MAX      MESH_BSP_SPI_PIECE_MAX
#define LR_FRAME_MAX      (2U + MESH_DRV_LR2021_PAYLOAD_MAX)

#define LR_BUSY_MS        MESH_BSP_LORA_BUSY_MS
#define LR_CALIB_MS       MESH_BSP_LORA_CALIB_MS
#define LR_RESET_MS       MESH_BSP_LORA_RESET_MS
#define LR_SPIN_US        2000
#define LR_RESET_LOW_US   1000U
#define LR_RESET_RISE_US  2000U
/* NSS held low this long leaves Sleep (4.4.1 says 100 us). */
#define LR_WAKE_NSS_US    150U
/* SetSleep's sleep_config (6.3.1): bit 0 the 32 kHz clock, bit 1 data retention. Neither: the
 * chip wakes cold, as from a reset (Table 4-1), and draws 0.47 uA asleep (Table 3-4, IDDSL0). */
#define SLEEP_COLD        0x00U

/* ---- Opcodes ------------------------------------------------------------- */
#define OP_READ_RX_FIFO       0x0001U
#define OP_WRITE_TX_FIFO      0x0002U
#define OP_GET_STATUS         0x0100U
#define OP_GET_VERSION        0x0101U
#define OP_WRITE_REG_MASK     0x0105U
#define OP_GET_ERRORS         0x0110U
#define OP_CLEAR_ERRORS       0x0111U
#define OP_SET_DIO_FUNCTION   0x0112U
#define OP_SET_DIO_IRQ        0x0115U
#define OP_CLEAR_IRQ          0x0116U
#define OP_GET_CLEAR_IRQ      0x0117U
#define OP_CLEAR_RX_FIFO      0x011EU
#define OP_CLEAR_TX_FIFO      0x011FU
#define OP_CALIB_FE           0x0123U
#define OP_SET_SLEEP          0x0127U
#define OP_SET_STANDBY        0x0128U
#define OP_SET_RF_FREQUENCY   0x0200U
#define OP_SET_RX_PATH        0x0201U
#define OP_SET_PA_CONFIG      0x0202U
#define OP_SET_TX_PARAMS      0x0203U
#define OP_SET_PACKET_TYPE    0x0207U
#define OP_GET_RSSI_INST      0x020BU
#define OP_SET_RX             0x020CU
#define OP_SET_TX             0x020DU
#define OP_SET_TX_TEST_MODE   0x020EU
#define OP_GET_RX_PKT_LENGTH  0x0212U
#define OP_SET_DEFAULT_TIMER  0x0215U
#define OP_SET_LORA_MOD       0x0220U
#define OP_SET_LORA_PACKET    0x0221U
#define OP_SET_LORA_SYNCWORD  0x0223U
#define OP_SET_LORA_CAD_PARAMS 0x0227U
#define OP_SET_LORA_CAD       0x0228U
#define OP_GET_LORA_PKT_STAT  0x022AU

/* ---- Parameter values ---------------------------------------------------- */
#define STANDBY_RC            0x00U
#define STANDBY_XOSC          0x01U
#define PACKET_TYPE_LORA      0x00U
#define DIO_IRQ_LINE          9U
#define DIO_FUNCTION_IRQ      0x10U
#define LORA_PACKET_FLAGS     0x02U   /* explicit header, CRC, standard IQ */
#define TX_RAMP_32US          0x04U
#define CAD_EXIT_ONLY         0x00U   /* back to the fallback mode, standby by default (table 6-18) */
#define CAD_EXIT_RX           0x01U   /* on a detection, stay in Rx for the packet (table 6-18) */
#define CAD_PREAMBLE_ONLY     0x10U   /* pbl_any: only a preamble counts, and Rx goes on to the sync word (9.4) */
#define CAD_TIMEOUT_MAX       0xFFFFFFU /* the Rx after a detection: no timer of its own (6.3.11) */
#define CAD_FAST_PNR_DELTA    8U      /* section 9.6 */
/* The over-current thresholds behind an unlock word (datasheet 1.8.3, 23.7):
 * six bits an amplifier, 4 mA a code on the LF path and 1 mA on the HF path. A
 * reset puts them back to the reference design's, 164 mA and 55 mA, so they are
 * written with every setting. The datasheet asks a quarter above what the
 * amplifier draws. The default rows draw far under the LF path's 220 mA; a
 * laboratory build with CONFIG_MESH_LAB_PA_TABLE may ask for rows that draw up
 * to 200 mA, which only the
 * field's top code, 252 mA, leaves that quarter to. The HF path's 63 mA is its
 * field's top in every build. */
#define REG_OCP_UNLOCK        0x00F40338UL
#define OCP_UNLOCK_MASK       0x0000FFFFUL
#define OCP_UNLOCK_WORD       0x0000C0DEUL
#define REG_OCP               0x00F40300UL
#define OCP_FIELD             0x3FUL
#define OCP_LF_SHIFT          19U
#define OCP_HF_SHIFT          25U
#if MESH_REGION_LAB_PA_TABLE
#define OCP_LF_CODE           63UL
#else
#define OCP_LF_CODE           55UL
#endif
#define OCP_HF_CODE           63UL
#define PA_LF_DUTY_IDLE       6U
#define PA_LF_SLICES_IDLE     7U
#define PA_HF_DUTY_IDLE       16U
#define PA_HF_DUTY_MAX        31U
#define PA_SELECT_HF          0x80U
/* One receive boost for both paths (SetRxPath, 7.3.2). In SetRxPath the chip
 * calibrates its self-reception cancellation again when the boost changes, not when
 * the path does - seen on the module: some 7 ms of BUSY each
 * time, and CMD_FAIL with GetErrors' bits 13 and 14 when a strong signal is on the
 * band; with one boost on both paths, 40 us and no failure. The datasheet's defaults,
 * 0 on LF and 4 on HF (3.4), made every change of band pay it. 0 is the LF default,
 * and the boost Semtech's reference board support gives both paths; on HF it costs
 * the 0.5 dB that table 3-8 gives for a boost left off (LOSS_S_BOOST). */
#define RX_BOOST              0x00U
#define TX_POWER_LF_MIN       (-19)
#define TX_POWER_LF_MAX       44
#define TX_POWER_HF_MIN       (-39)
#define TX_POWER_HF_MAX       24
/* The datasheet (6.7.4) calls GetErrors' top four bits reserved; Semtech's own
 * driver names them. Self-reception cancellation is calibrated by CalibFE, and by
 * SetRxPath when the boost changes (above): a signal in the band can fail it, the
 * command then answers CMD_FAIL, and asking again is what passes. */
#define ERRORS_SELF_RECEPTION 0xE000U
#define SELF_RECEPTION_TRIES  8U
#define CALIB_STEP_HZ         4000000U
#define CALIB_PATH_HF         0x8000U
#define TIMER_HZ              32768U
#define RX_FOREVER            0xFFFFFFU
/* The preamble the receiver is shaped with, whatever the setting sends: the longest the
 * field holds. Shaped with the setting's, the chip loses a packet whose preamble is
 * longer, by how much longer: it announces the preamble, no header follows, and it
 * announces it again (seen on the module: listening with 16,
 * preambles of 24 and 46 symbols lost on both bands, 20 and 32 in; listening with 8, 16
 * lost). mesh_drv_lr2021_send_preamble() sends exactly that. Listening with 65535,
 * preambles of 8, 16, 24, 46 and 300 symbols all came in, one PreambleDetected each. A
 * send writes its own preamble, so nothing changes on air. */
#define RX_PREAMBLE           MESH_DRV_LORA_PREAMBLE_MAX

#define STAT_CMD(s)           (((s) >> 9) & 0x7U)
#define STAT_MODE(s)          ((s) & 0x7U)
#define STAT_CMD_FAIL         0x0U
#define STAT_CMD_PERR         0x1U

#define IRQ_MASK              (MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT | MESH_DRV_LR2021_INTERRUPT_ERROR \
                               | MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR | MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE \
                               | MESH_DRV_LR2021_INTERRUPT_SEND_DONE | MESH_DRV_LR2021_INTERRUPT_TIMEOUT \
                               | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR | MESH_DRV_LR2021_INTERRUPT_PREAMBLE \
                               | MESH_DRV_LR2021_INTERRUPT_HEADER_VALID | MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR \
                               | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED | MESH_DRV_LR2021_INTERRUPT_CAD_DONE)

static spi_device_handle_t s_dev;
static mesh_drv_lr2021_pins_t   s_pins;
static bool                s_open;
static mesh_drv_lr2021_interrupt_callback_t   s_on_irq;
static void               *s_irq_arg;
static mesh_drv_lora_settings_t       s_cfg;            /* for the calibration that follows */
static uint16_t            s_pbl;            /* the preamble symbols last set; 0 = unknown */
static uint8_t             s_len;            /* the payload_len last set; 0 = unknown      */
static bool                s_dflt_timer;     /* SetDefaultRxTxTimeout written for a test   */
static bool                s_hooked;         /* a restart handler is registered            */
static bool                s_asleep;         /* SetSleep went out; NSS or a reset wakes it */

/* SetDefaultRxTxTimeout's "no default": the chip's own value after a reset. */
static const uint8_t TIMER_OFF[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

static uint8_t s_tx[LR_FRAME_MAX + 2U];
static uint8_t s_rx[LR_PIECE_MAX];
static uint8_t s_rxbig[LR_FRAME_MAX + 2U];   /* a whole FIFO read */

static const char *const ERROR_NAMES[] = {
    "the 32 MHz oscillator did not start",
    "the 32 kHz oscillator did not start",
    "the PLL did not lock",
    "LF RC calibration",
    "HF RC calibration",
    "PLL calibration",
    "anti-aliasing filter calibration",
    "image calibration",
    "a DIO trigger was not served",
    "no front-end calibration for this receive frequency",
    "measurement ADC calibration",
    "amplifier offset calibration",
    "poly-phase filter calibration",
    "self-reception cancellation calibration",
    "a signal too strong for that calibration",
    "that calibration out of tolerance",
};

/* ---- GPIO, ESP-IDF's driver straight ------------------------------------- */

static void gpio_out(uint8_t pin, bool level)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
    (void)gpio_set_level((gpio_num_t)pin, level ? 1U : 0U);
}

static void gpio_in(uint8_t pin, bool pull_up)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
}

/* DIO9 is level-triggered; its interrupt is turned off as it fires so nothing
 * more comes until mesh_drv_lr2021_interrupts_take() has read and cleared the flags. */
static void dio9_isr(void *arg)
{
    gpio_intr_disable((gpio_num_t)s_pins.dio9);
    if (s_on_irq != NULL) {
        s_on_irq(arg);
    }
}

/**
 * @brief Wait until the chip drops BUSY.
 * @return true when BUSY is low before @p ms.
 */
static bool lr_ready(uint32_t ms)
{
    int64_t const t0    = esp_timer_get_time();
    int64_t const limit = (int64_t)ms * 1000;

    while (gpio_get_level((gpio_num_t)s_pins.busy)) {
        int64_t const waited = esp_timer_get_time() - t0;
        if (waited > limit) {
            return false;
        }
        if (waited > (int64_t)LR_SPIN_US) {
            vTaskDelay(1);
        } else {
            esp_rom_delay_us(10U);
        }
    }
    return true;
}

/** @brief Move @p n bytes under one chip select, in as many pieces as needed. */
static bool lr_frame(const uint8_t *tx, uint8_t *rx, size_t n)
{
    bool   ok   = true;
    size_t done = 0U;

    (void)gpio_set_level((gpio_num_t)s_pins.chip_select, 0U);
    while (ok && (done < n)) {
        size_t piece = n - done;
        if (piece > (size_t)LR_PIECE_MAX) {
            piece = (size_t)LR_PIECE_MAX;
        }
        spi_transaction_t t = { .length = piece * 8U, .tx_buffer = &tx[done] };
        if (rx != NULL) {
            t.rx_buffer = &rx[done];
        }
        ok = (spi_device_polling_transmit(s_dev, &t) == ESP_OK);
        done += piece;
    }
    (void)gpio_set_level((gpio_num_t)s_pins.chip_select, 1U);
    return ok;
}

/** @brief Send one command: the opcode and its parameters. */
static kode_status_t lr_cmd(uint16_t op, const uint8_t *params, size_t n)
{
    if (((n + 2U) > sizeof(s_tx)) || s_asleep) {
        /* Asleep its BUSY stays high (Table 4-1, note 1): a command would wait it out and
         * wake it besides. mesh_drv_lr2021_reset() and alive() wake it first. */
        return KODE_DENIED;
    }
    if (!lr_ready(LR_BUSY_MS)) {
        KLOG_W(TAG, "BUSY stayed high before 0x%04X", (unsigned)op);
        return KODE_TIMEOUT;
    }
    s_tx[0] = (uint8_t)(op >> 8);
    s_tx[1] = (uint8_t)(op & 0xFFU);
    if (n > 0U) {
        (void)memcpy(&s_tx[2], params, n);
    }
    if (!lr_frame(s_tx, NULL, n + 2U)) {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/** @brief Read the answer to the command just sent: two status bytes, then data. */
static kode_status_t lr_answer(uint8_t *out, size_t n)
{
    if (n > sizeof(s_rx)) {
        return KODE_DENIED;
    }
    if (!lr_ready(LR_BUSY_MS)) {
        return KODE_TIMEOUT;
    }
    (void)memset(s_tx, 0, n);
    if (!lr_frame(s_tx, s_rx, n)) {
        return KODE_IO_ERROR;
    }
    (void)memcpy(out, s_rx, n);
    return KODE_OK;
}

/** @brief Read the status word: how the last command went, and the mode. */
static kode_status_t lr_status(uint16_t *stat)
{
    if (s_asleep) {
        return KODE_DENIED;   /* as lr_cmd(): asleep, BUSY stays high */
    }
    if (!lr_ready(LR_BUSY_MS)) {
        return KODE_TIMEOUT;
    }
    (void)memset(s_tx, 0, 6U);
    s_tx[0] = (uint8_t)(OP_GET_STATUS >> 8);
    s_tx[1] = (uint8_t)(OP_GET_STATUS & 0xFFU);
    if (!lr_frame(s_tx, s_rx, 6U)) {
        return KODE_IO_ERROR;
    }
    *stat = (uint16_t)(((uint16_t)s_rx[0] << 8) | (uint16_t)s_rx[1]);
    return KODE_OK;
}

/** @brief Send one command and ask the chip whether it took it. */
static kode_status_t lr_set(uint16_t op, const uint8_t *params, size_t n)
{
    uint16_t      stat = 0U;
    kode_status_t st   = lr_cmd(op, params, n);

    if (st == KODE_OK) {
        st = lr_status(&stat);
    }
    if (st != KODE_OK) {
        return st;
    }
    if ((STAT_CMD(stat) == STAT_CMD_FAIL) || (STAT_CMD(stat) == STAT_CMD_PERR)) {
        KLOG_E(TAG, "the chip refused 0x%04X (status 0x%04X)", (unsigned)op,
               (unsigned)stat);
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief SetLoraPacketParams, written only when it would change something.
 *
 * The chip keeps ONE payload_len, and it means two things: on the way out it is
 * the length of the packet, and on the way in, with an explicit header, it is
 * the longest packet the receiver accepts - a longer one is thrown away with a
 * header error nobody hears (datasheet 9.9.3, table 9-5). So a send sets it to
 * its own length and a receive must put it back to the whole payload, or the
 * radio goes deaf to everything longer than the last packet it sent. The
 * preamble length rides in the same command and means two things as well: on
 * the way out the packet's, on the way in the receiver's (RX_PREAMBLE).
 * Both are remembered so the command is not repeated for nothing; a failed
 * write forgets them, because the chip then holds who knows what.
 *
 * Call with the chip in standby: the packet is not shaped while it is on air.
 */
static kode_status_t lr_packet(uint16_t pbl, uint8_t len)
{
    if ((pbl == s_pbl) && (len == s_len)) {
        return KODE_OK;
    }
    const uint8_t p[4] = {
        (uint8_t)(pbl >> 8), (uint8_t)(pbl & 0xFFU), len, LORA_PACKET_FLAGS,
    };
    kode_status_t const st = lr_set(OP_SET_LORA_PACKET, p, sizeof(p));
    if (st == KODE_OK) {
        s_pbl = pbl;
        s_len = len;
    } else {
        s_pbl = 0U;
        s_len = 0U;
    }
    return st;
}

/**
 * @brief esp_restart()'s turn: pull the radio's reset line low and leave it.
 *
 * Run by esp_restart() in whichever task asked for the restart - kodeOS's way
 * home, kode_exit_to_os(), is one - before the scheduler stops. The owner's
 * task may be in the middle of a frame, so nothing is asked of the chip: one
 * pin is driven, which silences it whatever it was doing. Whether the line
 * then stays low through the restart or is let go does not matter: let go,
 * the module's pull-up starts the chip afresh, in standby with nothing set,
 * which is silent as well. A reset that does not come through esp_restart() -
 * a panic, a watchdog, the board's reset button - never gets here, and a
 * carrier then stays up until the module loses power.
 */
static void on_restart(void)
{
    mesh_drv_lr2021_hold_reset();
}

static void lr_log_errors(uint16_t errors)
{
    for (size_t bit = 0U; bit < (sizeof(ERROR_NAMES) / sizeof(ERROR_NAMES[0])); bit++) {
        if ((errors & (uint16_t)(1U << bit)) != 0U) {
            KLOG_E(TAG, "error flag %u: %s", (unsigned)bit, ERROR_NAMES[bit]);
        }
    }
}

/** @brief Is @p cfg a setting the chip and this driver both have? */
static bool lr_cfg_valid(const mesh_drv_lora_settings_t *cfg)
{
    if ((cfg->spreading_factor < 5U) || (cfg->spreading_factor > 12U)) {
        return false;
    }
    if (mesh_drv_lora_bandwidth_hz(cfg->bandwidth) == 0U) {
        return false;
    }
    if ((cfg->coding_rate < MESH_DRV_LORA_CODING_4_5) || (cfg->coding_rate > MESH_DRV_LORA_CODING_4_8)
        || (cfg->preamble == 0U)) {
        return false;
    }
    if (cfg->band_2_4_ghz != (cfg->frequency_hz >= MESH_BSP_2_4_GHZ_FROM_HZ)) {
        return false;
    }
    if (cfg->band_2_4_ghz) {
        return (cfg->tx_power >= TX_POWER_HF_MIN) && (cfg->tx_power <= TX_POWER_HF_MAX)
               && (cfg->amplifier_duty >= PA_HF_DUTY_IDLE) && (cfg->amplifier_duty <= PA_HF_DUTY_MAX);
    }
    return (cfg->tx_power >= TX_POWER_LF_MIN) && (cfg->tx_power <= TX_POWER_LF_MAX)
           && (cfg->amplifier_duty <= 7U) && (cfg->amplifier_slices <= 7U);
}

kode_status_t mesh_drv_lr2021_open(const mesh_drv_lr2021_pins_t *pins,
                              mesh_drv_lr2021_interrupt_callback_t on_irq, void *arg)
{
    if ((pins == NULL) || (on_irq == NULL) || s_open) {
        return KODE_DENIED;
    }
    s_pins    = *pins;
    s_on_irq  = on_irq;
    s_irq_arg = arg;

    /* Chip select and reset rest high before the bus exists, so the chip
     * never sees a clock it could take for a command. */
    gpio_out(s_pins.chip_select, true);
    gpio_out(s_pins.reset, true);
    gpio_in(s_pins.busy, true);     /* a pull-up: an empty header reads busy */
    gpio_in(s_pins.dio9, false);    /* a pull-down: no spurious flag          */

    const spi_bus_config_t bus = {
        .mosi_io_num     = (int)s_pins.mosi,
        .miso_io_num     = (int)s_pins.miso,
        .sclk_io_num     = (int)s_pins.clock,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = (int)LR_PIECE_MAX,
    };
    if (spi_bus_initialize((spi_host_device_t)s_pins.spi_host, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        KLOG_E(TAG, "the SPI controller would not come up");
        return KODE_ERROR;
    }
    (void)gpio_set_pull_mode((gpio_num_t)s_pins.miso, GPIO_PULLDOWN_ONLY);
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = LR_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };
    if (spi_bus_add_device((spi_host_device_t)s_pins.spi_host, &dev, &s_dev) != ESP_OK) {
        (void)spi_bus_free((spi_host_device_t)s_pins.spi_host);
        KLOG_E(TAG, "the SPI device would not attach");
        return KODE_ERROR;
    }
    (void)spi_device_acquire_bus(s_dev, portMAX_DELAY);

    /* The GPIO ISR service may already be up (another driver, kode-sdk): take
     * what is there, and install it only if it is not. It is never uninstalled
     * here, not even when this driver was the one that installed it: the
     * service is the whole app's, and whoever added a handler to it since
     * would lose its interrupts with it. */
    esp_err_t const isr = gpio_install_isr_service(0);
    if ((isr != ESP_OK) && (isr != ESP_ERR_INVALID_STATE)) {
        spi_device_release_bus(s_dev);
        (void)spi_bus_remove_device(s_dev);
        (void)spi_bus_free((spi_host_device_t)s_pins.spi_host);
        return KODE_ERROR;
    }
    (void)gpio_set_intr_type((gpio_num_t)s_pins.dio9, GPIO_INTR_HIGH_LEVEL);
    if (gpio_isr_handler_add((gpio_num_t)s_pins.dio9, dio9_isr, arg) != ESP_OK) {
        spi_device_release_bus(s_dev);
        (void)spi_bus_remove_device(s_dev);
        (void)spi_bus_free((spi_host_device_t)s_pins.spi_host);
        return KODE_ERROR;
    }
    gpio_intr_disable((gpio_num_t)s_pins.dio9);   /* enabled when listening */

    s_pbl        = 0U;
    s_len        = 0U;
    s_dflt_timer = false;
    (void)memset(&s_cfg, 0, sizeof(s_cfg));   /* no setting in force yet */
    s_open       = true;

    /* Register the restart hook once. ESP-IDF keeps five slots and no more,
     * and says ESP_ERR_INVALID_STATE for one it already holds; either way the
     * answer is kept, because a carrier test is refused without it. */
    if (!s_hooked) {
        esp_err_t const hook = esp_register_shutdown_handler(on_restart);
        s_hooked = (hook == ESP_OK) || (hook == ESP_ERR_INVALID_STATE);
        if (!s_hooked) {
            KLOG_W(TAG, "no shutdown handler for the radio (%d): a restart would not "
                        "hold it in reset, so a carrier test is refused", (int)hook);
        }
    }
    return KODE_OK;
}

void mesh_drv_lr2021_close(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    s_asleep = false;
    gpio_intr_disable((gpio_num_t)s_pins.dio9);
    (void)gpio_isr_handler_remove((gpio_num_t)s_pins.dio9);   /* the handler only: the service stays */
    spi_device_release_bus(s_dev);
    (void)spi_bus_remove_device(s_dev);
    (void)spi_bus_free((spi_host_device_t)s_pins.spi_host);
    gpio_reset_pin((gpio_num_t)s_pins.chip_select);
    gpio_reset_pin((gpio_num_t)s_pins.reset);
    gpio_reset_pin((gpio_num_t)s_pins.busy);
    gpio_reset_pin((gpio_num_t)s_pins.dio9);
    s_on_irq = NULL;
}

kode_status_t mesh_drv_lr2021_reset(uint8_t *major, uint8_t *minor)
{
    uint8_t r[4] = { 0U, 0U, 0U, 0U };

    if (!s_open || (major == NULL) || (minor == NULL)) {
        return KODE_DENIED;
    }

    (void)gpio_set_level((gpio_num_t)s_pins.reset, 0U);
    esp_rom_delay_us(LR_RESET_LOW_US);
    (void)gpio_set_level((gpio_num_t)s_pins.reset, 1U);
    esp_rom_delay_us(LR_RESET_RISE_US);
    s_asleep = false;   /* any reset wakes it (4.4.1) */

    /* From the pulse on, the chip holds its defaults whatever it answers next:
     * nothing this driver remembered about its settings is true any more. */
    s_pbl        = 0U;
    s_len        = 0U;
    s_dflt_timer = false;
    (void)memset(&s_cfg, 0, sizeof(s_cfg));

    int64_t const t0 = esp_timer_get_time();
    if (!lr_ready(LR_RESET_MS)) {
        KLOG_W(TAG, "BUSY stayed high for %u ms after the reset pulse (DIO9 %u)",
               (unsigned)LR_RESET_MS, (unsigned)gpio_get_level((gpio_num_t)s_pins.dio9));
        return KODE_TIMEOUT;
    }
    uint32_t const ready_us = (uint32_t)(esp_timer_get_time() - t0) + LR_RESET_RISE_US;
    KLOG_D(TAG, "ready %u us after reset", (unsigned)ready_us);

    kode_status_t st = lr_cmd(OP_GET_VERSION, NULL, 0U);
    if (st == KODE_OK) {
        st = lr_answer(r, sizeof(r));
    }
    if (st != KODE_OK) {
        KLOG_W(TAG, "the version could not be asked for after reset (%d)", (int)st);
        return st;
    }
    /* An empty bus reads as its pull: all zeros, or all ones the other way. */
    if (((r[2] == 0x00U) && (r[3] == 0x00U)) || ((r[2] == 0xFFU) && (r[3] == 0xFFU))) {
        KLOG_W(TAG, "version read back %02X %02X: nothing answers on MISO",
               (unsigned)r[2], (unsigned)r[3]);
        return KODE_IO_ERROR;
    }
    *major = r[2];
    *minor = r[3];
    return KODE_OK;
}

/** @brief Write some bits of a register and leave the rest. */
static kode_status_t lr_reg_write(uint32_t addr, uint32_t mask, uint32_t data)
{
    const uint8_t p[11] = {
        (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
        (uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8), (uint8_t)mask,
        (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)data,
    };
    return lr_set(OP_WRITE_REG_MASK, p, sizeof(p));
}

/** @brief Set both amplifiers' over-current thresholds to this module's. */
static kode_status_t lr_thresholds(void)
{
    kode_status_t st = lr_reg_write(REG_OCP_UNLOCK, OCP_UNLOCK_MASK, OCP_UNLOCK_WORD);
    if (st == KODE_OK) {
        st = lr_reg_write(REG_OCP,
                          (OCP_FIELD << OCP_LF_SHIFT) | (OCP_FIELD << OCP_HF_SHIFT),
                          (OCP_LF_CODE << OCP_LF_SHIFT) | (OCP_HF_CODE << OCP_HF_SHIFT));
    }
    return st;
}

/**
 * @brief Forget what a self-reception calibration that was tried again left behind:
 *        the error flags, and the two interrupts they raised on the way.
 */
static void lr_errors_forget(void)
{
    uint32_t const mask = MESH_DRV_LR2021_INTERRUPT_ERROR | MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR;
    const uint8_t  p[4] = { (uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8), (uint8_t)mask };

    (void)lr_cmd(OP_CLEAR_ERRORS, NULL, 0U);
    (void)lr_cmd(OP_CLEAR_IRQ, p, sizeof(p));
}

/** @brief SetRxPath for the setting's band, always with RX_BOOST (7.3.2). */
static kode_status_t lr_rx_path(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t       p[2] = { 0x00U, RX_BOOST };
    uint16_t      stat = 0U;
    kode_status_t st   = KODE_OK;

    if (cfg->band_2_4_ghz) {
        p[0] = 0x01U;
    }
    for (uint8_t i = 0U; i < SELF_RECEPTION_TRIES; i++) {
        st = lr_cmd(OP_SET_RX_PATH, p, sizeof(p));
        if (st == KODE_OK) {
            st = lr_status(&stat);
        }
        if (st != KODE_OK) {
            return st;
        }
        if (STAT_CMD(stat) != STAT_CMD_FAIL) {
            if (STAT_CMD(stat) == STAT_CMD_PERR) {
                KLOG_E(TAG, "the chip refused the receive path's parameters");
                return KODE_IO_ERROR;
            }
            if (i > 0U) {
                lr_errors_forget();
            }
            return KODE_OK;
        }
    }
    KLOG_E(TAG, "the chip refused the receive path %u times (status 0x%04X)", (unsigned)SELF_RECEPTION_TRIES,
           (unsigned)stat);
    return KODE_IO_ERROR;
}

/** @brief SetLoraModulationParams for the setting. */
static kode_status_t lr_modulation(const mesh_drv_lora_settings_t *cfg)
{
    const uint8_t p[2] = {
        (uint8_t)(((cfg->spreading_factor & 0x0FU) << 4) | (cfg->bandwidth & 0x0FU)),
        (uint8_t)((cfg->coding_rate << 4) | (cfg->low_data_rate ? 1U : 0U)),
    };
    return lr_set(OP_SET_LORA_MOD, p, sizeof(p));
}

/**
 * @brief The setting's amplifier and its power.
 *
 * The chip refuses to select an amplifier while the tx_power it holds is outside
 * that amplifier's range (firmware 01.18). Every HF figure is a legal LF one, so
 * on the HF path tx_power is written before the select as well as after; the
 * other way needs nothing. One amplifier is chosen and set; the other is left at
 * its idle values.
 */
static kode_status_t lr_amplifier(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t       p[3];
    kode_status_t st;

    if (cfg->band_2_4_ghz) {
        p[0] = (uint8_t)cfg->tx_power;
        p[1] = TX_RAMP_32US;
        st = lr_set(OP_SET_TX_PARAMS, p, 2U);
        if (st != KODE_OK) { return st; }
        p[0] = PA_SELECT_HF;
        p[1] = (uint8_t)((PA_LF_DUTY_IDLE << 4) | PA_LF_SLICES_IDLE);
        p[2] = cfg->amplifier_duty;
    } else {
        p[0] = 0x00U;
        p[1] = (uint8_t)((cfg->amplifier_duty << 4) | cfg->amplifier_slices);
        p[2] = PA_HF_DUTY_IDLE;
    }
    st = lr_set(OP_SET_PA_CONFIG, p, 3U);
    if (st != KODE_OK) { return st; }

    p[0] = (uint8_t)cfg->tx_power;
    p[1] = TX_RAMP_32US;
    return lr_set(OP_SET_TX_PARAMS, p, 2U);
}

kode_status_t mesh_drv_lr2021_configure(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t       p[5];
    kode_status_t st;

    if (!s_open || (cfg == NULL) || !lr_cfg_valid(cfg)) {
        return KODE_DENIED;
    }

    p[0] = STANDBY_XOSC;
    st = lr_set(OP_SET_STANDBY, p, 1U);
    if (st != KODE_OK) { return st; }

    st = lr_thresholds();
    if (st != KODE_OK) { return st; }

    p[0] = PACKET_TYPE_LORA;
    st = lr_set(OP_SET_PACKET_TYPE, p, 1U);
    if (st != KODE_OK) { return st; }

    p[0] = (uint8_t)(cfg->frequency_hz >> 24);
    p[1] = (uint8_t)(cfg->frequency_hz >> 16);
    p[2] = (uint8_t)(cfg->frequency_hz >> 8);
    p[3] = (uint8_t)(cfg->frequency_hz);
    st = lr_set(OP_SET_RF_FREQUENCY, p, 4U);
    if (st != KODE_OK) { return st; }

    st = lr_rx_path(cfg);
    if (st != KODE_OK) { return st; }

    st = lr_modulation(cfg);
    if (st != KODE_OK) { return st; }

    /* The receiver's shape: the whole payload accepted, and any preamble heard
     * (RX_PREAMBLE). Forced, since what the chip holds is not known here. */
    s_pbl = 0U;
    s_len = 0U;
    st = lr_packet(RX_PREAMBLE, (uint8_t)MESH_DRV_LR2021_PAYLOAD_MAX);
    if (st != KODE_OK) { return st; }

    /* One byte (table 9-7). A reset puts it back to the private word, so it is
     * written with every setting. */
    st = lr_set(OP_SET_LORA_SYNCWORD, &cfg->sync_word, 1U);
    if (st != KODE_OK) { return st; }

    st = lr_amplifier(cfg);
    if (st != KODE_OK) { return st; }

    p[0] = DIO_IRQ_LINE;
    p[1] = DIO_FUNCTION_IRQ;
    st = lr_set(OP_SET_DIO_FUNCTION, p, 2U);
    if (st != KODE_OK) { return st; }

    p[0] = DIO_IRQ_LINE;
    p[1] = (uint8_t)(IRQ_MASK >> 24);
    p[2] = (uint8_t)(IRQ_MASK >> 16);
    p[3] = (uint8_t)(IRQ_MASK >> 8);
    p[4] = (uint8_t)(IRQ_MASK);
    st = lr_set(OP_SET_DIO_IRQ, p, 5U);
    if (st != KODE_OK) { return st; }

    (void)lr_cmd(OP_CLEAR_ERRORS, NULL, 0U);
    (void)mesh_drv_lr2021_interrupts_take();

    if (mesh_drv_lr2021_errors() != 0U) {
        return KODE_IO_ERROR;
    }
    s_cfg = *cfg;
    return KODE_OK;
}

/**
 * @brief CalibFE, judged by what it alone left, and again while only self-reception
 *        cancellation is what failed: an error from before must not fail it, and on
 *        a failure the frequency has no calibration at all, so that the next SetRx
 *        reports RXFREQ_NO_FE_CAL_ERR.
 *
 * @param p  CalibFE's six bytes.
 * @param ms How long a run may take.
 * @return KODE_OK; KODE_IO_ERROR when it failed each time; KODE_TIMEOUT.
 */
static kode_status_t lr_calibrate_fe(const uint8_t *p, uint32_t ms)
{
    uint16_t errors = 0U;

    for (uint8_t i = 0U; i < SELF_RECEPTION_TRIES; i++) {
        (void)lr_cmd(OP_CLEAR_ERRORS, NULL, 0U);
        kode_status_t const st = lr_cmd(OP_CALIB_FE, p, 6U);
        if (st != KODE_OK) {
            return st;
        }
        if (!lr_ready(ms)) {
            return KODE_TIMEOUT;
        }
        uint8_t r[4] = { 0U, 0U, 0U, 0U };
        if ((lr_cmd(OP_GET_ERRORS, NULL, 0U) != KODE_OK) || (lr_answer(r, sizeof(r)) != KODE_OK)) {
            return KODE_IO_ERROR;
        }
        errors = (uint16_t)(((uint16_t)r[2] << 8) | (uint16_t)r[3]);
        if (errors == 0U) {
            if (i > 0U) {
                lr_errors_forget();
            }
            return KODE_OK;
        }
        if ((errors & (uint16_t)~ERRORS_SELF_RECEPTION) != 0U) {
            break;
        }
    }
    lr_log_errors(errors);
    return KODE_IO_ERROR;
}

kode_status_t mesh_drv_lr2021_calibrate(void)
{
    uint8_t p[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

    if (!s_open) {
        return KODE_DENIED;
    }
    uint16_t f4 = (uint16_t)(s_cfg.frequency_hz / CALIB_STEP_HZ);
    if (s_cfg.band_2_4_ghz) {
        f4 |= CALIB_PATH_HF;
    }
    p[0] = (uint8_t)(f4 >> 8);
    p[1] = (uint8_t)(f4 & 0xFFU);
    return lr_calibrate_fe(p, LR_CALIB_MS);
}

kode_status_t mesh_drv_lr2021_calibrate_at(const uint32_t *frequencies_hz, uint8_t count)
{
    uint8_t p[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

    if (!s_open || (frequencies_hz == NULL) || (count == 0U) || (count > MESH_DRV_LR2021_CALIBRATIONS)) {
        return KODE_DENIED;
    }
    for (uint8_t i = 0U; i < count; i++) {
        uint16_t f4 = (uint16_t)(frequencies_hz[i] / CALIB_STEP_HZ);
        if (frequencies_hz[i] >= MESH_BSP_2_4_GHZ_FROM_HZ) {
            f4 |= CALIB_PATH_HF;
        }
        p[2U * i]        = (uint8_t)(f4 >> 8);
        p[(2U * i) + 1U] = (uint8_t)(f4 & 0xFFU);
    }
    return lr_calibrate_fe(p, LR_CALIB_MS * (uint32_t)count);
}

kode_status_t mesh_drv_lr2021_retune(const mesh_drv_lora_settings_t *cfg)
{
    uint8_t p[4];

    if (!s_open || (cfg == NULL) || !lr_cfg_valid(cfg) || (s_cfg.preamble == 0U)) {
        return KODE_DENIED;
    }
    /* The crystal stays on: from here the receiver is 84 us away, where standby on
     * the RC oscillator is 115 (table 3-23). */
    p[0] = STANDBY_XOSC;
    kode_status_t st = lr_set(OP_SET_STANDBY, p, 1U);

    if ((st == KODE_OK) && (cfg->band_2_4_ghz != s_cfg.band_2_4_ghz)) {
        st = lr_rx_path(cfg);
    }
    if ((st == KODE_OK) && (cfg->frequency_hz != s_cfg.frequency_hz)) {
        p[0] = (uint8_t)(cfg->frequency_hz >> 24);
        p[1] = (uint8_t)(cfg->frequency_hz >> 16);
        p[2] = (uint8_t)(cfg->frequency_hz >> 8);
        p[3] = (uint8_t)(cfg->frequency_hz);
        st = lr_set(OP_SET_RF_FREQUENCY, p, 4U);
    }
    if ((st == KODE_OK)
        && ((cfg->spreading_factor != s_cfg.spreading_factor) || (cfg->bandwidth != s_cfg.bandwidth)
            || (cfg->coding_rate != s_cfg.coding_rate) || (cfg->low_data_rate != s_cfg.low_data_rate))) {
        st = lr_modulation(cfg);
    }
    if (st == KODE_OK) {
        st = lr_packet(RX_PREAMBLE, (uint8_t)MESH_DRV_LR2021_PAYLOAD_MAX);
    }
    if ((st == KODE_OK) && (cfg->sync_word != s_cfg.sync_word)) {
        st = lr_set(OP_SET_LORA_SYNCWORD, &cfg->sync_word, 1U);
    }
    if ((st == KODE_OK)
        && ((cfg->band_2_4_ghz != s_cfg.band_2_4_ghz) || (cfg->tx_power != s_cfg.tx_power)
            || (cfg->amplifier_duty != s_cfg.amplifier_duty) || (cfg->amplifier_slices != s_cfg.amplifier_slices))) {
        st = lr_amplifier(cfg);
    }
    if (st != KODE_OK) {
        /* Part-way: what the chip holds is not known. */
        s_pbl = 0U;
        s_len = 0U;
        (void)memset(&s_cfg, 0, sizeof(s_cfg));
        return st;
    }
    s_cfg = *cfg;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_send(const uint8_t *data, uint8_t len)
{
    return mesh_drv_lr2021_send_preamble(data, len, s_cfg.preamble);
}

kode_status_t mesh_drv_lr2021_send_preamble(const uint8_t *data, uint8_t len, uint16_t preamble)
{
    uint8_t       p[3];
    kode_status_t st;

    if (!s_open || (data == NULL) || (len == 0U)) {
        return KODE_DENIED;
    }

    /* The chip's deadline is worked out from the setting in force. With none -
     * nothing configured since the driver opened - there is no airtime to bound
     * a transmit by, and nothing is put on air. */
    uint32_t const air = mesh_drv_lora_packet_airtime_us(s_cfg.spreading_factor, s_cfg.bandwidth, s_cfg.coding_rate,
                                                         preamble, len, s_cfg.low_data_rate);
    if (air == 0U) {
        return KODE_DENIED;
    }

    /* The packet's length is the chip's payload_len; mesh_drv_lr2021_receive() and
     * mesh_drv_lr2021_listen_for() put the whole payload back (see lr_packet()). */
    st = lr_packet(preamble, len);
    if (st != KODE_OK) { return st; }

    /* A packet that left whole emptied the FIFO; one cut short did not, and
     * its remains must not lead the next. */
    st = lr_cmd(OP_CLEAR_TX_FIFO, NULL, 0U);
    if (st != KODE_OK) { return st; }
    st = lr_cmd(OP_WRITE_TX_FIFO, data, (size_t)len);
    if (st != KODE_OK) { return st; }

    /* The chip's own deadline: SetTx with a timeout ends the transmit by
     * itself if its done flag never comes, whatever the host is doing. A zero
     * would switch that off, and mesh_drv_lora_send_timeout_ticks() is never zero. */
    uint32_t const ticks = mesh_drv_lora_send_timeout_ticks(air);
    p[0] = (uint8_t)(ticks >> 16);
    p[1] = (uint8_t)(ticks >> 8);
    p[2] = (uint8_t)(ticks);
    return lr_set(OP_SET_TX, p, sizeof(p));
}

kode_status_t mesh_drv_lr2021_read(uint8_t *buf, uint8_t cap, uint32_t irq, mesh_drv_lr2021_packet_t *rx)
{
    uint8_t r[8] = { 0U };

    if (!s_open || (buf == NULL) || (rx == NULL) || (cap == 0U)) {
        return KODE_DENIED;
    }

    /* GetLoRaPacketStatus: the length, the SNR and the RSSI of the packet just
     * received, and whether it carried a CRC. Six data bytes after the status.
     * Its crc bit is that last thing - the header said a CRC follows - and not
     * whether the CRC matched: that verdict is the CrcError flag (bit 22) of
     * the interrupt word that announced the packet. */
    kode_status_t st = lr_cmd(OP_GET_LORA_PKT_STAT, NULL, 0U);
    if (st == KODE_OK) {
        st = lr_answer(r, 8U);
    }
    if (st != KODE_OK) {
        return st;
    }
    bool const crc_on = (r[2] & 0x10U) != 0U;
    rx->crc_ok = crc_on && ((irq & MESH_DRV_LR2021_INTERRUPT_CRC_ERROR) == 0U);
    uint8_t plen = r[3];
    rx->snr_db = (float)((int8_t)r[4]) / 4.0f;
    uint16_t const rssi_raw = ((uint16_t)r[5] << 1) | (uint16_t)((r[7] >> 1) & 0x01U);
    rx->rssi_dbm = (float)rssi_raw / -2.0f;

    uint8_t n = (plen < cap) ? plen : cap;
    rx->len = n;
    if (n == 0U) {
        return KODE_OK;
    }

    /* ReadRxFifo: the opcode, then the bytes in the same chip select, no
     * status word. The first two bytes clocked back are the opcode's echo. */
    if ((size_t)(n + 2U) > sizeof(s_rxbig)) {
        return KODE_DENIED;
    }
    if (!lr_ready(LR_BUSY_MS)) {
        return KODE_TIMEOUT;
    }
    (void)memset(s_tx, 0, (size_t)n + 2U);
    s_tx[0] = (uint8_t)(OP_READ_RX_FIFO >> 8);
    s_tx[1] = (uint8_t)(OP_READ_RX_FIFO & 0xFFU);
    if (!lr_frame(s_tx, s_rxbig, (size_t)n + 2U)) {
        return KODE_IO_ERROR;
    }
    (void)memcpy(buf, &s_rxbig[2], n);
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_rssi(float *dbm)
{
    uint8_t r[4] = { 0U };

    if (!s_open || (dbm == NULL)) {
        return KODE_DENIED;
    }
    kode_status_t st = lr_cmd(OP_GET_RSSI_INST, NULL, 0U);
    if (st == KODE_OK) {
        st = lr_answer(r, 4U);
    }
    if (st != KODE_OK) {
        return st;
    }
    uint16_t const raw = ((uint16_t)r[2] << 1) | (uint16_t)((r[3] >> 7) & 0x01U);
    *dbm = (float)raw / -2.0f;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_test(mesh_drv_lr2021_test_t mode, uint32_t ms)
{
    mesh_drv_lr2021_mode_t now = MESH_DRV_LR2021_MODE_SLEEP;
    uint8_t           p[6];
    kode_status_t     st;

    if (!s_open || (ms == 0U) || (ms > MESH_BSP_TEST_MAX_MS)) {
        return KODE_DENIED;
    }
    if ((mode != MESH_DRV_LR2021_TEST_PREAMBLE) && (mode != MESH_DRV_LR2021_TEST_CARRIER)) {
        return KODE_DENIED;
    }
    st = mesh_drv_lr2021_standby();   /* from a known state, and out of any receive */
    if (st != KODE_OK) { return st; }

    uint32_t const ticks = mesh_drv_lora_timer_ticks(ms * 1000U);
    p[0] = (uint8_t)(ticks >> 16);
    p[1] = (uint8_t)(ticks >> 8);
    p[2] = (uint8_t)(ticks);

    if (mode == MESH_DRV_LR2021_TEST_PREAMBLE) {
        /* A real packet - a preamble as long as the time asked for, and one
         * byte - sent under SetTx's timeout: the chip's own timer ends it, so
         * this test dies by itself even if the host does not. (SetTxTestMode's
         * "infinite preamble" has no timeout to give.) */
        uint16_t const pbl = mesh_drv_lora_preamble_symbols(s_cfg.spreading_factor, s_cfg.bandwidth, ms);
        if (pbl == 0U) {
            return KODE_DENIED;
        }
        const uint8_t one = 0x00U;
        st = lr_packet(pbl, 1U);
        if (st != KODE_OK) { return st; }
        st = lr_cmd(OP_CLEAR_TX_FIFO, NULL, 0U);
        if (st != KODE_OK) { return st; }
        st = lr_cmd(OP_WRITE_TX_FIFO, &one, 1U);
        if (st != KODE_OK) { return st; }
        st = lr_set(OP_SET_TX, p, 3U);
    } else {
        /* A bare carrier. SetTxTestMode takes no timeout, and the datasheet
         * does not say the default timer reaches it; it is written anyway,
         * as a try, so that a chip that honours it stops by itself. The
         * owner's deadline and the restart hook do not depend on it, and the
         * owner logs which of the chip's timer and its own ended the test. */
        p[3] = p[0];
        p[4] = p[1];
        p[5] = p[2];
        p[0] = 0U;
        p[1] = 0U;
        p[2] = 0U;
        if (lr_set(OP_SET_DEFAULT_TIMER, p, sizeof(p)) == KODE_OK) {
            s_dflt_timer = true;
        } else {
            KLOG_W(TAG, "the chip took no default timer: this carrier has only "
                        "the owner's deadline");
        }
        uint8_t const m = (uint8_t)MESH_DRV_LR2021_TEST_CARRIER;
        st = lr_set(OP_SET_TX_TEST_MODE, &m, 1U);
        if ((st != KODE_OK) && s_dflt_timer) {
            /* If the timer is what the chip objects to, the carrier must still
             * be possible: clear it and ask once more. */
            KLOG_W(TAG, "carrier refused with a default timer set; trying without");
            (void)lr_cmd(OP_SET_DEFAULT_TIMER, TIMER_OFF, sizeof(TIMER_OFF));
            s_dflt_timer = false;
            st = lr_set(OP_SET_TX_TEST_MODE, &m, 1U);
        }
    }
    if (st == KODE_OK) {
        st = mesh_drv_lr2021_mode(&now);
    }
    if (st != KODE_OK) {
        return st;
    }
    return (now == MESH_DRV_LR2021_MODE_TRANSMIT) ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t mesh_drv_lr2021_receive(void)
{
    mesh_drv_lr2021_mode_t  now  = MESH_DRV_LR2021_MODE_SLEEP;
    const uint8_t      p[3] = { (uint8_t)(RX_FOREVER >> 16), (uint8_t)(RX_FOREVER >> 8),
                                (uint8_t)(RX_FOREVER) };

    if (!s_open) {
        return KODE_DENIED;
    }
    kode_status_t st = mesh_drv_lr2021_mode(&now);
    if (st != KODE_OK) {
        return st;
    }
    if (now == MESH_DRV_LR2021_MODE_RECEIVE) {
        /* Already receiving, and continuous receive stays in Rx after every
         * packet: SetRx would fail on a chip that is in Rx ("the timeout
         * cannot be updated", datasheet 6.3.5), and clearing the FIFO now
         * could throw away a packet that landed since the last read. */
        return KODE_OK;
    }
    if (now == MESH_DRV_LR2021_MODE_TRANSMIT) {
        return KODE_DENIED;   /* never abort a transmit from here */
    }

    /* The receiver accepts the whole payload and any preamble (a send leaves its
     * own length and preamble), and the FIFO must not lead with an old packet's bytes. */
    st = lr_packet(RX_PREAMBLE, (uint8_t)MESH_DRV_LR2021_PAYLOAD_MAX);
    if (st != KODE_OK) {
        return st;
    }
    (void)lr_cmd(OP_CLEAR_RX_FIFO, NULL, 0U);
    st = lr_set(OP_SET_RX, p, sizeof(p));
    if (st == KODE_OK) {
        st = mesh_drv_lr2021_mode(&now);
    }
    if (st != KODE_OK) {
        return st;
    }
    return (now == MESH_DRV_LR2021_MODE_RECEIVE) ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t mesh_drv_lr2021_flush_receive(void)
{
    if (!s_open) {
        return KODE_DENIED;
    }
    return lr_cmd(OP_CLEAR_RX_FIFO, NULL, 0U);
}

kode_status_t mesh_drv_lr2021_listen_for(uint32_t ms)
{
    uint8_t p[3];

    if (!s_open || (ms == 0U) || (ms > 1000U)) {
        return KODE_DENIED;
    }
    /* A timed receive cannot replace a running one - SetRx fails in Rx - and
     * the packet shape must be the receiver's, not the last send's. */
    kode_status_t st = mesh_drv_lr2021_standby();
    if (st != KODE_OK) {
        return st;
    }
    st = lr_packet(RX_PREAMBLE, (uint8_t)MESH_DRV_LR2021_PAYLOAD_MAX);
    if (st != KODE_OK) {
        return st;
    }
    (void)lr_cmd(OP_CLEAR_RX_FIFO, NULL, 0U);
    uint32_t const ticks = mesh_drv_lora_timer_ticks(ms * 1000U);
    p[0] = (uint8_t)(ticks >> 16);
    p[1] = (uint8_t)(ticks >> 8);
    p[2] = (uint8_t)(ticks);
    return lr_set(OP_SET_RX, p, sizeof(p));
}

/**
 * @brief Set a search up and start it, from standby.
 * @param symbols 1..4.
 * @param any     pbl_any and pnr_delta, the byte SetLoraCadParams takes.
 * @param exit    CadExitMode.
 * @param timeout cad_timeout.
 */
static kode_status_t lr_cad(uint8_t symbols, uint8_t any, uint8_t exit, uint32_t timeout)
{
    const uint8_t p[7] = {
        symbols, any, exit, (uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8), (uint8_t)timeout,
        mesh_drv_lora_cad_peak(s_cfg.spreading_factor, symbols),
    };
    kode_status_t const st = lr_set(OP_SET_LORA_CAD_PARAMS, p, sizeof(p));

    if (st != KODE_OK) {
        return st;
    }
    return lr_set(OP_SET_LORA_CAD, NULL, 0U);
}

kode_status_t mesh_drv_lr2021_cad(uint8_t symbols, bool fast)
{
    if (!s_open || (symbols == 0U) || (symbols > 4U) || (s_cfg.preamble == 0U)) {
        return KODE_DENIED;
    }
    /* SetLoraCAD is a mode of its own, and a receive cannot turn into it. */
    kode_status_t const st = mesh_drv_lr2021_standby();
    if (st != KODE_OK) {
        return st;
    }
    /* Any LoRa symbol counts, not only a preamble: this is for not talking over
     * someone. No cad_timeout: it is for the exits this driver does not use. */
    return lr_cad(symbols, fast ? CAD_FAST_PNR_DELTA : 0U, CAD_EXIT_ONLY, 0U);
}

kode_status_t mesh_drv_lr2021_cad_receive(uint8_t symbols, bool fast)
{
    if (!s_open || (symbols == 0U) || (symbols > 4U) || (s_cfg.preamble == 0U)) {
        return KODE_DENIED;
    }
    kode_status_t st = mesh_drv_lr2021_standby();
    if (st != KODE_OK) {
        return st;
    }
    /* What follows a detection is a receive: the whole payload and a preamble of
     * any length, and a FIFO that does not lead with an old packet's bytes. */
    st = lr_packet(RX_PREAMBLE, (uint8_t)MESH_DRV_LR2021_PAYLOAD_MAX);
    if (st != KODE_OK) {
        return st;
    }
    (void)lr_cmd(OP_CLEAR_RX_FIFO, NULL, 0U);
    /* Only a preamble counts, so that a detection goes on to the sync word, and the
     * Rx after it has no timer of its own: the owner ends a receive that comes to
     * nothing. */
    return lr_cad(symbols, (uint8_t)(CAD_PREAMBLE_ONLY | (fast ? CAD_FAST_PNR_DELTA : 0U)), CAD_EXIT_RX,
                  CAD_TIMEOUT_MAX);
}

kode_status_t mesh_drv_lr2021_standby(void)
{
    mesh_drv_lr2021_mode_t now = MESH_DRV_LR2021_MODE_TRANSMIT;
    uint8_t           m   = STANDBY_RC;

    if (!s_open) {
        return KODE_DENIED;
    }
    kode_status_t st = lr_set(OP_SET_STANDBY, &m, 1U);
    if (st == KODE_OK) {
        st = mesh_drv_lr2021_mode(&now);
    }
    if (st != KODE_OK) {
        return st;
    }
    bool const safe = (now == MESH_DRV_LR2021_MODE_STANDBY_RC) || (now == MESH_DRV_LR2021_MODE_STANDBY_XOSC);
    if (safe && s_dflt_timer) {
        /* The carrier test's default timer goes back to the chip's own 0, after
         * the radio is safe and not before: a chip that took the standby but
         * still says Tx keeps the one stop it may have. */
        (void)lr_cmd(OP_SET_DEFAULT_TIMER, TIMER_OFF, sizeof(TIMER_OFF));
        s_dflt_timer = false;
    }
    return safe ? KODE_OK : KODE_IO_ERROR;
}

kode_status_t mesh_drv_lr2021_interrupts_read(uint32_t *flags)
{
    uint8_t r[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

    if (flags == NULL) {
        return KODE_DENIED;
    }
    *flags = 0U;
    if (!s_open) {
        return KODE_DENIED;
    }
    kode_status_t st = lr_cmd(OP_GET_CLEAR_IRQ, NULL, 0U);
    if (st == KODE_OK) {
        st = lr_answer(r, sizeof(r));
    }
    if (st == KODE_OK) {
        *flags = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16)
                 | ((uint32_t)r[4] << 8) | (uint32_t)r[5];
    }
    /* The flags are cleared and the line is low again: listen to it. */
    gpio_intr_enable((gpio_num_t)s_pins.dio9);
    return st;
}

uint32_t mesh_drv_lr2021_interrupts_take(void)
{
    uint32_t flags = 0U;

    (void)mesh_drv_lr2021_interrupts_read(&flags);
    return flags;
}

kode_status_t mesh_drv_lr2021_mode(mesh_drv_lr2021_mode_t *mode)
{
    uint16_t stat = 0U;

    if (!s_open || (mode == NULL)) {
        return KODE_DENIED;
    }
    kode_status_t const st = lr_status(&stat);
    if (st != KODE_OK) {
        return st;
    }
    *mode = (mesh_drv_lr2021_mode_t)STAT_MODE(stat);
    return KODE_OK;
}

void mesh_drv_lr2021_errors_clear(void)
{
    if (s_open) {
        (void)lr_cmd(OP_CLEAR_ERRORS, NULL, 0U);
    }
}

uint16_t mesh_drv_lr2021_errors(void)
{
    uint8_t r[4] = { 0U, 0U, 0U, 0U };

    if (!s_open) {
        return 0xFFFFU;
    }
    if ((lr_cmd(OP_GET_ERRORS, NULL, 0U) != KODE_OK) || (lr_answer(r, sizeof(r)) != KODE_OK)) {
        KLOG_E(TAG, "the chip did not answer for its error flags");
        return 0xFFFFU;
    }
    uint16_t const errors = (uint16_t)(((uint16_t)r[2] << 8) | (uint16_t)r[3]);
    if (errors != 0U) {
        lr_log_errors(errors);
    }
    return errors;
}

/**
 * @brief Wake a sleeping chip: NSS low for LR_WAKE_NSS_US, then BUSY low (a cold start
 *        is about 6 ms, Table 3-23, TSPDRC). It comes back in Standby RC with its defaults.
 */
static kode_status_t lr_wake(void)
{
    (void)gpio_set_level((gpio_num_t)s_pins.chip_select, 0U);
    esp_rom_delay_us(LR_WAKE_NSS_US);
    (void)gpio_set_level((gpio_num_t)s_pins.chip_select, 1U);
    /* LR_BUSY_MS, not the reset's wait: a cold start is 6 ms, and a pulled module must cost
     * the alive check little. Awake only once BUSY says so: a pulse that did not take (a
     * contact that bounced) leaves it asleep, and the next check pulses NSS again rather
     * than wait on a BUSY that a sleeping chip holds high and call the module gone. */
    if (!lr_ready(LR_BUSY_MS)) {
        return KODE_TIMEOUT;
    }
    s_asleep = false;
    return KODE_OK;
}

kode_status_t mesh_drv_lr2021_sleep(void)
{
    static const uint8_t p[5] = { SLEEP_COLD, 0U, 0U, 0U, 0U };   /* sleep_time 0: no timer */

    if (!s_open) {
        return KODE_DENIED;
    }
    if (s_asleep) {
        return KODE_OK;
    }
    /* No status read after it: asleep, BUSY stays high. */
    kode_status_t const st = lr_cmd(OP_SET_SLEEP, p, sizeof(p));
    if (st != KODE_OK) {
        return st;
    }
    s_asleep = true;
    /* It wakes cold: nothing this driver remembered about its settings is true any more. */
    s_pbl        = 0U;
    s_len        = 0U;
    s_dflt_timer = false;
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    return KODE_OK;
}

bool mesh_drv_lr2021_asleep(void)
{
    return s_open && s_asleep;
}

bool mesh_drv_lr2021_alive(void)
{
    uint8_t r[4] = { 0U, 0U, 0U, 0U };
    bool const was_asleep = s_asleep;

    if (!s_open) {
        return false;
    }
    if (was_asleep && (lr_wake() != KODE_OK)) {
        return false;   /* no chip to wake: BUSY stays high on an empty header */
    }
    bool const ok = (lr_cmd(OP_GET_VERSION, NULL, 0U) == KODE_OK) && (lr_answer(r, sizeof(r)) == KODE_OK)
                    && (r[2] == MESH_BSP_LORA_FW_MAJOR);
    if (was_asleep) {
        /* Back to sleep, whatever it answered: it was asked a question, not woken. If
         * SetSleep fails it stays awake, and mesh_drv_lr2021_asleep() says so. */
        (void)mesh_drv_lr2021_sleep();
    }
    return ok;
}

void mesh_drv_lr2021_hold_reset(void)
{
    if (s_open) {
        (void)gpio_set_level((gpio_num_t)s_pins.reset, 0U);
        s_asleep = false;   /* in reset now, which is not asleep */
    }
}

bool mesh_drv_lr2021_guarded(void)
{
    return s_hooked;
}

bool mesh_drv_lr2021_busy(void)
{
    return s_open && (gpio_get_level((gpio_num_t)s_pins.busy) != 0);
}

bool mesh_drv_lr2021_dio9(void)
{
    return s_open && (gpio_get_level((gpio_num_t)s_pins.dio9) != 0);
}
