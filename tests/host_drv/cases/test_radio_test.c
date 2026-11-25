/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_test.c
 * @brief The test signals. INTO A 50 OHM LOAD ONLY - nothing here ever reaches an
 *        antenna; the model is a model. What these hold the driver to:
 *
 *  - a preamble test is a real packet under SetTx's timeout, so the CHIP ends it;
 *  - a carrier has no timeout in the datasheet: the default Tx timer is written
 *    first as a try, and the driver treats it as nothing more (the cases below run
 *    it both ways) - what ends a carrier is the owner's deadline, a standby, or
 *    the restart hook;
 *  - the default timer comes off again only once the chip is confirmed safe;
 *  - nothing is asked for without a time, or for more than the longest.
 */
#include <stdint.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "radio_harness.h"

static uint64_t ticks_param(const chip_cmd_t *c, unsigned off)
{
    return ((uint64_t)c->p[off] << 16) | ((uint64_t)c->p[off + 1U] << 8) | c->p[off + 2U];
}

static int index_of(uint16_t op, unsigned nth)
{
    unsigned seen = 0U;

    for (unsigned i = 0U; i < chip_log_len(); i++) {
        if (chip_log_at(i)->op == op) {
            if (seen == nth) {
                return (int)i;
            }
            seen++;
        }
    }
    return -1;
}

/* ---- The preamble --------------------------------------------------------------------------------- */

TEST_CASE("preamble: a real packet under SetTx's timeout of the time asked", "[radio][test]")
{
    radio_up_sub();   /* SF9, BW125: a symbol is 4.096 ms */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 200U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);

    /* The preamble lasts as long as the time asked, within a symbol or two. */
    uint64_t const sym_us = 4096U;
    uint64_t const lasts  = (uint64_t)g_chip.pbl * sym_us;
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(200000U, lasts);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(200000U + (3U * sym_us), lasts);
    TEST_ASSERT_EQUAL_UINT8(1U, g_chip.payload_len);       /* one byte after it */
    TEST_ASSERT_EQUAL_UINT(1U, g_chip.tx_fifo_n);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_tx_len_mismatch);

    /* SetTx's timeout is the time asked. */
    uint64_t const to = last_tx_timeout_us();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(200000U, to);
    TEST_ASSERT_LESS_THAN_UINT64(200000U + 100U, to);

    /* Not a test mode: no SetTxTestMode, and no default timer. */
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_TX_TEST_MODE));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));
}

TEST_CASE("preamble: the chip's own timer ends it, with Timeout, and the host is not needed", "[radio][test]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 200U));

    chip_advance_us(190000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_irq_calls);
    chip_advance_us(15000U);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_TIMEOUT, mesh_drv_lr2021_interrupts_take());
}

TEST_CASE("preamble: a time the longest preamble cannot reach ends sooner, with TxDone", "[radio][test]")
{
    radio_up(MESH_PLAN_BAND_2_4_GHZ, 7U, 450);   /* BW812: a symbol is 158 us, 65535 of them 10.3 s */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 60000U));
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);

    chip_advance_us(11ULL * 1000000ULL);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, mesh_drv_lr2021_interrupts_take());
}

TEST_CASE("preamble: the longest time allowed is accepted", "[radio][test]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, MESH_BSP_TEST_MAX_MS));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("preamble: one that cannot be built for the setting (none in force) is refused", "[radio][test]")
{
    radio_open();
    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));   /* a reset forgets the setting */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 100U));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_TX));
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
}

TEST_CASE("test: from a receive, the chip is taken to standby before anything else", "[radio][test]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 100U));
    TEST_ASSERT_GREATER_THAN_INT(0, (int)chip_log_len());
    TEST_ASSERT_EQUAL_HEX16(OPC_SET_STANDBY, chip_log_at(0U)->op);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_rx_over_tx);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

/* ---- The carrier ------------------------------------------------------------------------------------ */

TEST_CASE("carrier: the default Tx timer is written first, then the test mode; the chip is in Tx", "[radio][test][carrier]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 300U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_TRUE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);

    int const timer_at = index_of(OPC_SET_DEFAULT_TIMER, 0U);
    int const mode_at  = index_of(OPC_SET_TX_TEST_MODE, 0U);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, timer_at);
    TEST_ASSERT_GREATER_THAN_INT(timer_at, mode_at);

    const chip_cmd_t *timer = chip_log_at((unsigned)timer_at);
    TEST_ASSERT_EQUAL_UINT8(6U, timer->n);
    TEST_ASSERT_EQUAL_UINT64(0U, ticks_param(timer, 0U));                       /* no Rx default */
    TEST_ASSERT_UINT64_WITHIN(1U, chip_us_to_ticks(300000U), ticks_param(timer, 3U));   /* Tx: the time asked */
    TEST_ASSERT_EQUAL_HEX8(0x02U, chip_log_at((unsigned)mode_at)->p[0]);         /* continuous wave */
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_TX));                          /* not a packet */
}

TEST_CASE("carrier: a chip that honours the default timer ends it by itself", "[radio][test][carrier]")
{
    radio_up_sub();
    g_chip.dflt_timer_stops_test = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 100U));

    chip_advance_us(90000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    chip_advance_us(20000U);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_FALSE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_TIMEOUT, mesh_drv_lr2021_interrupts_take());
}

TEST_CASE("carrier: a chip that does not honour it keeps transmitting - only the host ends it", "[radio][test][carrier]")
{
    /* The datasheet does not say a test mode reaches the default timer. This is
     * the chip the driver must be safe with: the owner's deadline, a standby or the
     * restart hook are what end a carrier, and the driver's docs say so. */
    radio_up_sub();
    g_chip.dflt_timer_stops_test = false;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 100U));

    chip_advance_us(10ULL * 1000000ULL);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_TRUE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_HEX32(0U, mesh_drv_lr2021_interrupts_take());

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_FALSE(g_chip.test_cw);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
}

TEST_CASE("carrier: standby ends it, and only then does the default timer go back to 0", "[radio][test][carrier]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 1000U));
    TEST_ASSERT_NOT_EQUAL(0U, g_chip.dflt_tx_ticks);
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    int const standby_at = index_of(OPC_SET_STANDBY, 0U);
    int const timer_at   = index_of(OPC_SET_DEFAULT_TIMER, 0U);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, standby_at);
    TEST_ASSERT_GREATER_THAN_INT(standby_at, timer_at);   /* after the radio is safe, not before */
    TEST_ASSERT_EQUAL_UINT64(0U, ticks_param(chip_log_at((unsigned)timer_at), 0U));
    TEST_ASSERT_EQUAL_UINT64(0U, ticks_param(chip_log_at((unsigned)timer_at), 3U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_chip.dflt_tx_ticks);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);

    chip_log_clear();                                      /* and it is written once, not on every standby */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));
}

TEST_CASE("carrier: a standby the chip does not confirm is an error, and the default timer is left in place", "[radio][test][carrier]")
{
    radio_up_sub();
    g_chip.dflt_timer_stops_test = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 5000U));
    uint32_t const armed = g_chip.dflt_tx_ticks;
    TEST_ASSERT_NOT_EQUAL(0U, armed);
    g_chip.wedged = true;   /* it takes the command and stays in Tx */
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));   /* the one stop it may have stays */
    TEST_ASSERT_EQUAL_UINT32(armed, g_chip.dflt_tx_ticks);
}

TEST_CASE("carrier: a chip that fails SetTxTestMode while a default timer is set is asked again without one", "[radio][test][carrier]")
{
    radio_up_sub();
    g_chip.refuse_test_with_timer = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 500U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_TRUE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_UINT32(0U, g_chip.dflt_tx_ticks);   /* cleared for the second try */
    TEST_ASSERT_EQUAL_UINT(2U, chip_count(OPC_SET_TX_TEST_MODE));
    TEST_ASSERT_EQUAL_UINT8(CMD_FAIL, chip_nth(OPC_SET_TX_TEST_MODE, 0U)->status);
    TEST_ASSERT_EQUAL_UINT8(CMD_OK, chip_nth(OPC_SET_TX_TEST_MODE, 1U)->status);
    TEST_ASSERT_TRUE(fake_log_seen("carrier refused with a default timer set"));

    /* The retry cleared the driver's note too: a standby has nothing to put back. */
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));
}

TEST_CASE("carrier: a chip that takes no default timer still gets its carrier, and the log says it has only the host's deadline", "[radio][test][carrier]")
{
    radio_up_sub();
    g_chip.refuse_default_timer = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 500U));
    TEST_ASSERT_TRUE(g_chip.test_cw);
    TEST_ASSERT_TRUE(fake_log_seen("only the owner's deadline"));

    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));   /* none was set, none to clear */
}

TEST_CASE("carrier: on the 2.4 GHz path as well", "[radio][test][carrier]")
{
    radio_up(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 100U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_INT8(13, g_chip.tx_power);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
}

TEST_CASE("carrier: a reset also ends it, and the driver forgets the default timer with the chip", "[radio][test][carrier]")
{
    radio_up_sub();
    g_chip.dflt_timer_stops_test = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 1000U));
    uint8_t major = 0U;
    uint8_t minor = 0U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_reset(&major, &minor));
    TEST_ASSERT_FALSE(g_chip.test_cw);
    TEST_ASSERT_EQUAL_UINT32(0U, g_chip.dflt_tx_ticks);

    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_DEFAULT_TIMER));
}

/* ---- What may be asked for -------------------------------------------------------------------------------- */

TEST_CASE("test: no time, too much time or an unknown mode never reaches the chip", "[radio][test]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_CARRIER, MESH_BSP_TEST_MAX_MS + 1U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 0xFFFFFFFFU));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test((mesh_drv_lr2021_test_t)0, 100U));    /* "normal Tx" is not a test */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test((mesh_drv_lr2021_test_t)3, 100U));    /* PRBS is not LoRa's */
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_test((mesh_drv_lr2021_test_t)0x80, 100U));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}
