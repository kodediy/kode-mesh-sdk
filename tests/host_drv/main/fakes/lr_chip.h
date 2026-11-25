/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lr_chip.h
 * @brief The LR2021 as the driver sees it on the wire: a model that takes SPI
 *        frames and pin changes and answers as the datasheet says the chip does.
 *
 * The rules in lr_chip.c come from the datasheet (Rev 2.1), not from the driver
 * they hold to account:
 *  - a frame is a 16-bit opcode and its parameters under one chip select; the
 *    first two bytes back are the status word; an answer comes in a second
 *    frame of zeros; ReadRxFifo is the exception, its data follows the opcode;
 *  - SetLoraPacketParams' payload_len is the longest packet an explicit-header
 *    receiver accepts (0 = any), and a longer one is thrown away with a header
 *    error and no RxDone (table 9-5);
 *  - SetRx fails when the chip is already in Rx (6.3.5); 0xFFFFFF is continuous
 *    receive, which stays in Rx after every packet; 0 is single receive;
 *  - SetTx fails when the chip is in Tx; its timeout is in 32.768 kHz periods, 0
 *    disables it, and one that runs out stops the transmit with a Timeout flag
 *    (6.3.6); a finished packet raises TxDone;
 *  - a failed command raises CmdError; a standby ends any transmit;
 *  - the GetLoraPacketStatus crc bit says the packet carried a CRC, not that it
 *    matched; a wrong CRC is the CrcError flag (table 5-17).
 *
 * What the datasheet does not say is a knob, not a rule: whether SetTxTestMode
 * honours SetDefaultRxTxTimeout is `dflt_timer_stops_test`.
 *
 * Time is the case's: the chip's clock moves only with chip_advance_us(). The
 * driver's own clock (esp_timer_get_time) is separate and always moves.
 */
#ifndef LR_CHIP_H_
#define LR_CHIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_drv_lr2021.h"

/** @name The pins the model answers on (GPIO numbers of the Dot's J3 wiring) */
/** @{ */
#define PIN_RST    12
#define PIN_DIO9   13
#define PIN_MOSI   17
#define PIN_BUSY   18
#define PIN_MISO   19
#define PIN_CS     20
#define PIN_SCK    21
/** @} */

/** @name Opcodes, from the datasheet's command tables */
/** @{ */
#define OPC_READ_RX_FIFO       0x0001U
#define OPC_WRITE_TX_FIFO      0x0002U
#define OPC_GET_STATUS         0x0100U
#define OPC_GET_VERSION        0x0101U
#define OPC_SET_SLEEP          0x0127U
#define OPC_WRITE_REG_MASK     0x0105U
#define OPC_GET_ERRORS         0x0110U
#define OPC_CLEAR_ERRORS       0x0111U
#define OPC_SET_DIO_FUNCTION   0x0112U
#define OPC_SET_DIO_IRQ        0x0115U
#define OPC_CLEAR_IRQ          0x0116U
#define OPC_GET_CLEAR_IRQ      0x0117U
#define OPC_CLEAR_RX_FIFO      0x011EU
#define OPC_CLEAR_TX_FIFO      0x011FU
#define OPC_CALIB_FE           0x0123U
#define OPC_SET_STANDBY        0x0128U
#define OPC_SET_RF_FREQUENCY   0x0200U
#define OPC_SET_RX_PATH        0x0201U
#define OPC_SET_PA_CONFIG      0x0202U
#define OPC_SET_TX_PARAMS      0x0203U
#define OPC_SET_PACKET_TYPE    0x0207U
#define OPC_GET_RSSI_INST      0x020BU
#define OPC_SET_RX             0x020CU
#define OPC_SET_TX             0x020DU
#define OPC_SET_TX_TEST_MODE   0x020EU
#define OPC_SET_DEFAULT_TIMER  0x0215U
#define OPC_SET_LORA_MOD       0x0220U
#define OPC_SET_LORA_PACKET    0x0221U
#define OPC_SET_LORA_SYNCWORD  0x0223U
#define OPC_SET_LORA_CAD_PARAMS 0x0227U
#define OPC_SET_LORA_CAD       0x0228U
#define OPC_GET_LORA_PKT_STAT  0x022AU
/** @} */

/** @name The interrupt word's bits the model sets that the driver has no name for */
/** @{ */
#define CHIP_IRQ_HEADER_ERR    (1UL << 9)
/** @} */

#define CHIP_CTRL_TICK_HZ      32768ULL    /**< The RTC the timeouts count in. */

/** @brief CommandStatus values of the status word. */
enum { CMD_FAIL = 0, CMD_PERR = 1, CMD_OK = 2, CMD_DAT = 3 };

#define CHIP_LOG_MAX     512
#define CHIP_LOG_PARAMS  16

/** @brief One command the chip was given, with how it stood when it arrived. */
typedef struct {
    uint16_t op;
    uint8_t  n;                          /**< Parameter bytes (after the opcode). */
    uint8_t  p[CHIP_LOG_PARAMS];         /**< The first of them.                  */
    uint8_t  status;                     /**< CMD_* the chip gave it.             */
    uint8_t  mode_before;                /**< mesh_drv_lr2021_mode_t as it arrived.    */
    uint8_t  payload_len_before;         /**< The payload_len the chip held.      */
    uint16_t pbl_before;                 /**< The preamble length it held.        */
    uint8_t  dflt_tx_set;                /**< A default Tx timeout was set.       */
} chip_cmd_t;

typedef struct {
    /* ---- the chip ---------------------------------------------------- */
    mesh_drv_lr2021_mode_t mode;
    bool       in_reset;                 /**< RST is low.                         */
    bool       asleep;                   /**< SetSleep went out (6.3.1): BUSY high, commands
                                          *   lost, NSS low 100 us or a reset wakes it cold. */
    uint8_t    sleep_config;             /**< SetSleep's sleep_config.            */
    unsigned   n_sleeps, n_wakes;
    bool       lora;                     /**< The packet type is LoRa.            */
    uint8_t    stat_cmd;                 /**< CommandStatus of the last command.  */
    uint32_t   irq;                      /**< Pending flags.                      */
    uint32_t   dio9_mask;                /**< The flags routed to DIO9.           */
    uint16_t   errors;                   /**< What GetErrors answers.             */
    uint8_t    sf, bw, cr, ldro;
    uint16_t   pbl;
    uint8_t    payload_len;
    uint8_t    cad_symbols;              /**< SetLoraCadParams' nb_symbols.       */
    uint8_t    cad_pnr_delta;            /**< Its pnr_delta.                      */
    uint8_t    cad_exit;                 /**< Its CadExitMode.                    */
    bool       cad_preamble_only;        /**< Its pbl_any.                        */
    uint32_t   cad_timeout;              /**< Its cad_timeout.                    */
    uint8_t    cad_peak;                 /**< Its det_peak.                       */
    bool       cad_on;                   /**< A search is running.                */
    uint64_t   cad_elapsed_us;
    bool       channel_active;           /**< A knob: somebody is transmitting.   */
    uint8_t    sync_word;                /**< SetLoraSyncword's; 0x12 after a reset. */
    uint8_t    pkt_flags;
    int8_t     tx_power;
    uint32_t   freq_hz;
    uint8_t    rx_path;                  /**< SetRxPath's path: 0 LF, 1 HF.       */
    uint8_t    rx_boost;                 /**< Its boost; 0 after a reset (7.3.2). */
    uint32_t   tx_timeout_ticks;         /**< SetTx's, 0 = none.                  */
    uint32_t   rx_timeout_ticks;         /**< SetRx's; 0xFFFFFF = continuous.     */
    uint32_t   dflt_rx_ticks, dflt_tx_ticks;
    bool       test_cw;                  /**< A carrier is on.                    */
    bool       test_preamble;            /**< An infinite preamble is on.         */
    uint64_t   air_us;                   /**< The packet in flight's airtime.     */
    uint64_t   on_air_us;                /**< Elapsed since Tx began.             */
    uint64_t   rx_elapsed_us;
    uint8_t    tx_fifo[256];
    unsigned   tx_fifo_n;
    uint8_t    rx_fifo[512];
    unsigned   rx_fifo_n, rx_fifo_pos;
    uint8_t    last_len;
    bool       last_crc_on;
    int8_t     last_snr4;
    uint16_t   last_rssi2;               /**< Half-dB steps below 0 dBm.          */
    uint16_t   inst_rssi2;               /**< What GetRssiInst answers.           */

    /* ---- knobs: how it behaves, set by a case -------------------------- */
    bool       absent;                   /**< No module on the header.            */
    bool       busy_stuck;
    bool       wedged;                   /**< SetStandby accepted, does nothing (a reset cures). */
    bool       dflt_timer_stops_test;    /**< SetTxTestMode honours the default Tx timer. */
    bool       refuse_test_with_timer;   /**< ...and fails while one is set.      */
    bool       refuse_default_timer;     /**< SetDefaultRxTxTimeout itself fails. */
    uint64_t   force_air_us;             /**< A transmit's airtime, if not computed. */
    uint8_t    fw_major, fw_minor;
    uint32_t   creep_us;                 /**< The driver's clock per read.        */
    uint16_t   sticky_errors;            /**< What GetErrors answers after ClearErrors. */
    unsigned   refuse_rx_path;           /**< SetRxPaths that fail, as a failed self-reception calibration does. */
    unsigned   fail_calib_fe;            /**< CalibFEs that fail the same way.    */
    uint16_t   calib_fe_error;           /**< The flag those two leave.           */

    /* ---- what happened -------------------------------------------------- */
    unsigned   n_failed;                 /**< Commands refused (FAIL or PERR).    */
    unsigned   n_header_err;             /**< Packets thrown out by payload_len.  */
    unsigned   n_rx_over_tx;             /**< SetRx while transmitting.           */
    unsigned   n_tx_len_mismatch;        /**< SetTx with payload_len != the FIFO. */
    unsigned   n_resets;                 /**< RST rising edges.                   */
    unsigned   n_isr;                    /**< DIO9 interrupts delivered.          */
    unsigned   n_dropped_rx;             /**< Packets that arrived with no receiver. */
    unsigned   n_src_calibrations;       /**< SetRxPaths that changed the boost: the chip
                                              calibrates self-reception cancellation again,
                                              some 7 ms (seen on a module).             */
} chip_t;

extern chip_t g_chip;

/** @brief What ESP-IDF's side does when a case asks it to fail, and what it holds. */
typedef struct {
    bool     spi_init_fails;      /**< spi_bus_initialize() fails.                */
    bool     spi_add_fails;       /**< spi_bus_add_device() fails.                */
    int      isr_service_rc;      /**< What gpio_install_isr_service() answers.   */
    bool     isr_add_fails;       /**< gpio_isr_handler_add() fails.              */
    unsigned buses_up;            /**< SPI buses initialised and not freed.       */
    int      bus_host;            /**< The controller the last bus and device were asked of. */
    int      device_host;
    unsigned devices_up;          /**< Devices added and not removed.             */
    bool     bus_acquired;        /**< spi_device_acquire_bus() held.             */
    bool     isr_service_up;      /**< The ISR service is installed.              */
    unsigned nss_frames;          /**< Chip-select frames seen.                   */
    unsigned spi_fail_in;         /**< The transfer this many from now fails (1: the next),
                                   *   and the chip sees none of it; 0 for none. */
    void   (*on_intr_enable)(void);  /**< Run inside gpio_intr_enable(DIO9), before the
                                   *   line is looked at: the chip raising a flag in
                                   *   the gap after a clear.                      */
} hw_t;

extern hw_t g_hw;

/** @brief A new chip on the header, powered, in standby, every knob off, log empty.
 *         Does not touch the driver, which a case closes and opens itself. */
void chip_power_up(void);

/** @brief Let @p us of the chip's time pass: a Tx may finish or time out, a timed
 *         Rx may time out. */
void chip_advance_us(uint64_t us);

/** @brief A packet arrives over the air. Returns whether the chip accepted it (in
 *         Rx, and past the payload_len filter). @p crc_on: the sender used a CRC;
 *         @p crc_good: it matched. */
bool chip_rx_packet(const uint8_t *data, uint8_t len, bool crc_on, bool crc_good);

/** @brief Raise interrupt flags on their own (a PA fault, say). */
void chip_raise(uint32_t flags);

/** @brief Commands of kind @p op the chip was given since the log was cleared. */
unsigned chip_count(uint16_t op);

/** @brief The @p nth (0-based) command of kind @p op, or NULL. */
const chip_cmd_t *chip_nth(uint16_t op, unsigned nth);

/** @brief The last command of kind @p op, or NULL. */
const chip_cmd_t *chip_last(uint16_t op);

/** @brief Every command, in order. */
unsigned chip_log_len(void);
const chip_cmd_t *chip_log_at(unsigned i);

/** @brief Empty the command log (and the counters of what happened). */
void chip_log_clear(void);

/** @brief The level of a pin as the driver would read it. */
int pin_level(int gpio);

/** @brief The DIO9 interrupt as the driver armed it. */
bool pin_isr_enabled(void);

/** @brief The chip's time, in ticks of its 32.768 kHz RTC, for a duration. */
uint64_t chip_us_to_ticks(uint64_t us);
uint64_t chip_ticks_to_us(uint64_t ticks);

/** @brief The datasheet's airtime of a LoRa packet (explicit header, CRC on,
 *         coding rate 4/5), in microseconds - written here, apart from the
 *         driver's plan, for the cases to compare against. */
uint64_t chip_lora_air_us(uint8_t sf, uint8_t bw_code, bool ldro, uint16_t pbl, uint8_t payload);

/** @brief The same, at the coding rate of chip code @p cr (1 is 4/5, 4 is 4/8). */
uint64_t chip_lora_air_cr_us(uint8_t sf, uint8_t bw_code, uint8_t cr, bool ldro, uint16_t pbl, uint8_t payload);

#endif /* LR_CHIP_H_ */
