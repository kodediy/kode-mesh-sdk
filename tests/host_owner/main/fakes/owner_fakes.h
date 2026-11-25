/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file owner_fakes.h
 * @brief What a case controls and reads in the owner host suite: a model of the
 *        LR2021 as the owner sees it (mode, interrupt flags, how it fails), the
 *        pins that make a Mesh look like one, and the log.
 *
 * The radio fake is a state machine, not a recorder: transmit puts it in Tx
 * and it STAYS there until something takes it out (a done flag the case
 * raises, a standby, a reset), receive puts it in Rx, a standby it is told to
 * fail leaves it where it was. That is what makes "the carrier stops on every
 * exit" a thing a case can check: it reads the chip's mode, not a count of
 * calls. tests/host_drv is where the real driver is held to the chip's own
 * rules; this is only the owner's view of one.
 *
 * Fields a case reads while the owner task runs are volatile; a case reads them
 * once the owner has answered, or after wait_until(), never mid-request.
 */
#ifndef OWNER_FAKES_H_
#define OWNER_FAKES_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_sys.h"
#include "mesh_drv_gnss.h"
#include "mesh_drv_lr2021.h"

/** @brief Where a case can make the owner's next radio call stand still. */
typedef enum {
    LR_GATE_NONE = 0,
    LR_GATE_CONFIGURE,   /**< inside mesh_drv_lr2021_configure()                  */
    LR_GATE_SEND,        /**< inside mesh_drv_lr2021_send()                       */
} lr_gate_t;

typedef struct {
    /* ---- the chip ----------------------------------------------------- */
    volatile mesh_drv_lr2021_mode_t mode;      /**< what GetStatus would say         */
    volatile bool              held;      /**< its reset line is low            */
    volatile uint32_t          irq;       /**< flags waiting for irq_take()     */
    volatile uint16_t          errors;    /**< what GetErrors answers           */
    volatile bool              alive;     /**< answers GetVersion               */
    volatile bool              guarded;   /**< a restart hook is registered     */

    /* ---- how it misbehaves -------------------------------------------- */
    volatile unsigned          standby_fails;  /**< the next N standbys fail    */
    volatile bool              reset_fails;    /**< a reset gets no answer      */
    volatile kode_status_t     open_rc;        /**< what opening the driver answers (ESP-IDF refusing the bus or the interrupt) */
    volatile bool              mode_fails;     /**< GetStatus gets no answer    */
    volatile kode_status_t     configure_rc;
    volatile kode_status_t     calibrate_rc;
    volatile kode_status_t     send_rc;
    volatile kode_status_t     test_rc;
    volatile kode_status_t     receive_rc;
    volatile kode_status_t     read_rc;        /**< what reading a received packet answers */
    volatile kode_status_t     take_rc;        /**< what reading the interrupt flags answers:
                                                *   not KODE_OK, they are cleared and lost */
    volatile kode_status_t     listen_rc;
    volatile kode_status_t     cad_rc;
    volatile uint32_t          cad_finish;     /**< What a search raises when it is asked:
                                                *   0 for nothing, a stuck chip.   */

    /* ---- what it was asked --------------------------------------------- */
    volatile unsigned          n_open, n_close, n_reset, n_hold, n_configure, n_calibrate;
    volatile unsigned          n_send, n_test, n_receive, n_listen, n_standby, n_flush;
    volatile unsigned          n_read, n_rssi, n_take;
    uint8_t                    sent[MESH_DRV_LR2021_PAYLOAD_MAX];  /**< the last packet */
    volatile uint8_t           sent_len;
    volatile mesh_drv_lr2021_test_t test_mode;
    volatile uint32_t          test_ms;
    volatile uint32_t          read_irq;       /**< the flags the last read was given */
    volatile unsigned          n_cad;
    volatile unsigned          n_retune, n_calibrate_at, n_send_preamble;
    volatile uint16_t          sent_preamble;  /**< the last send's, 0 the setting's  */
    uint32_t                   calibrated_at[3];
    volatile uint8_t           calibrated_count;
    volatile kode_status_t     retune_rc;
    volatile kode_status_t     calibrate_at_rc;
    volatile kode_status_t     sleep_rc;       /**< What SetSleep answers.               */
    volatile bool              asleep;         /**< SetSleep went out, nothing woke it.  */
    volatile unsigned          n_sleep;        /**< mesh_drv_lr2021_sleep() calls.       */
    const char *volatile       asleep_last;    /**< The last of them, for a case's message. */
    volatile unsigned          n_asleep_calls; /**< Calls the real driver refuses asleep:
                                                *   the owner must never make one.    */
    volatile bool              alive_leaves_awake; /**< The next alive check on a sleeping
                                                *   chip wakes it and it stays awake. */
    volatile uint8_t           cad_symbols;    /**< the last search's                 */
    volatile bool              cad_fast;
    volatile unsigned          n_cad_receive;  /**< searches that go on to receive    */
    mesh_drv_lora_settings_t              cfg;            /**< the last configure()           */

    /* ---- a packet waiting to be read -------------------------------- */
    uint8_t                    rx_data[MESH_DRV_LR2021_PAYLOAD_MAX];
    volatile uint8_t           rx_len;
    volatile bool              rx_crc_on;      /**< it carries a CRC               */
    volatile float             rx_rssi;
    volatile float             rx_snr;

    /* ---- the gate ------------------------------------------------------ */
    volatile lr_gate_t         gate;           /**< stand still here, once         */
    volatile bool              blocked;        /**< the owner is standing there    */
} lr_fake_t;

extern lr_fake_t g_lr;

/** @brief What the owner last opened the radio driver with, kept across lr_fake_reset(). */
extern mesh_drv_lr2021_pins_t g_lr_pins;

/** @brief The chip as it comes out of reset, every misbehaviour off, counts zero.
 *         Lets go of a gate if the owner is held at one. */
void lr_fake_reset(void);

/** @brief Hold the owner's next call at @p where until lr_fake_release(). */
void lr_fake_gate(lr_gate_t where);

/** @brief Let the owner go. */
void lr_fake_release(void);

/** @brief Raise interrupt flags, as the chip would. DIO9 is high until taken. */
void lr_fake_raise(uint32_t flags);

/** @brief The chip leaves Tx or Rx on its own - a timer, a finished packet - and
 *         raises @p flags (0 for none). */
void lr_fake_chip_stops(uint32_t flags);

/** @brief Put a packet in the FIFO for the next mesh_drv_lr2021_read(). */
void lr_fake_inject(const uint8_t *data, uint8_t len, bool crc_on);

/** @brief Make the four lines of a Mesh's fingerprint read high (or all low). */
void pins_fake_set_mesh(bool present);

/** @brief The GNSS driver, as the owner opened it. */
typedef struct {
    volatile unsigned       opens;     /**< mesh_drv_gnss_open() calls that took.   */
    volatile unsigned       closes;
    volatile bool           is_open;
    volatile kode_status_t  open_rc;   /**< What opening answers; KODE_OK is zero.   */
    mesh_drv_gnss_pins_t    pins;      /**< What the last open was given.           */
    mesh_drv_gnss_status_t  report;    /**< What mesh_drv_gnss_read() hands back while open. */
    unsigned                sleeps;    /**< mesh_drv_gnss_sleep() that put it to sleep.  */
    unsigned                wakes;     /**< mesh_drv_gnss_wake() calls on a sleeping one. */
    unsigned                restarts;  /**< mesh_drv_gnss_restart() calls.               */
    bool                    asleep;    /**< As the driver: told to sleep, not woken.     */
    kode_status_t           sleep_rc;  /**< What mesh_drv_gnss_sleep() answers when it may
                                        *   sleep (the driver's own rule decides first). */
    kode_status_t           wake_rc;   /**< What mesh_drv_gnss_wake() answers on a sleeping one:
                                        *   not KODE_OK, the bytes did not go out (the driver
                                        *   is waking all the same, as the real one). */
} gnss_fake_t;

extern gnss_fake_t g_gnss;

/** @brief Where the owner's other seams stand: J3's supply and the controllers. */
typedef struct {
    volatile unsigned power_on_calls;    /**< kode_sys_exp_power(true)                   */
    volatile unsigned power_off_calls;
    volatile kode_status_t power_rc;     /**< what kode_sys_exp_power() answers          */
    volatile bool     spi_refused;       /**< kode_sys_exp_ctrl(KODE_SYS_EXP_SPI) has none free  */
    volatile bool     uart_refused;      /**< the same for the UART                  */
    volatile uint8_t  spi_id;            /**< the SPI controller it hands out        */
    volatile uint8_t  uart_id;           /**< the UART it hands out                  */
    const char *volatile refuse_lend;    /**< kode_sys_exp_lend() refuses this tag (somebody else holds the lines) */
    volatile unsigned lends_total;       /**< kode_sys_exp_lend() calls of any tag: looking included */
    volatile unsigned lends_of_module;   /**< kode_sys_exp_lend() calls under MESH_BSP_LOAN_TAG: the owner's attempts */
} exp_fake_t;

extern exp_fake_t g_exp;

/** @brief What the plan answers to "may this build put a test signal on air?" (mesh_plan_test_allowed()).
 *         true unless a case says otherwise, so the owner's handling of a test signal can be checked
 *         on a build that keeps the region's limits; harness_fresh() puts it back to true. */
extern volatile bool g_plan_test_allowed;

/** @brief mesh_plan.c's own answer to the same question, as this suite's build has it: false,
 *         since the suite is built without CONFIG_MESH_LAB. */
bool mesh_plan_test_allowed_as_built(void);

/** @brief Whether a loan with this tag is out: kode_sys_exp_lend() with it, and no
 *         kode_sys_exp_return() since. */
bool fake_loaned(const char *tag);

/** @brief Let @p ms of the clock pass at once: the duty budget refills by the time it was told. */
void fake_clock_skip(uint32_t ms);

/** @brief The log, as the owner wrote it. */
void     fake_log_clear(void);
bool     fake_log_seen(const char *needle);          /**< any line holds it    */
unsigned fake_log_count(char level);                  /**< lines at one level   */
unsigned fake_log_matches(const char *needle);        /**< lines that hold it   */

#endif /* OWNER_FAKES_H_ */
