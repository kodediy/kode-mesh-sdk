/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_rx.c
 * @brief The receiver. The chip keeps ONE payload_len: sent, it is the packet's
 *        length; received, it is the longest packet the receiver will accept.
 *        After a short send a receiver left at that length goes deaf to
 *        everything longer, and SetRx on a chip already in Rx fails. The model
 *        enforces both as the datasheet words them; these cases are the driver
 *        keeping to them.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "radio_harness.h"

static uint32_t take(void)
{
    return mesh_drv_lr2021_interrupts_take();
}

/** @brief The payload_len and preamble of the last SetLoraPacketParams. */
static void packet_shape(uint16_t *pbl, uint8_t *len)
{
    const chip_cmd_t *c = chip_last(OPC_SET_LORA_PACKET);

    TEST_ASSERT_NOT_NULL(c);
    *pbl = (uint16_t)(((uint16_t)c->p[0] << 8) | c->p[1]);
    *len = c->p[2];
}

/* ---- The shape the receiver is configured with -------------------------------------------- */

TEST_CASE("configure: the receiver accepts the whole payload, with no command refused", "[radio][rx]")
{
    radio_up_raw(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    TEST_ASSERT_EQUAL_UINT8(255U, g_chip.payload_len);
    TEST_ASSERT_EQUAL_UINT16(65535U, g_chip.pbl);       /* any preamble heard */
    TEST_ASSERT_EQUAL_HEX8(0x02U, g_chip.pkt_flags);    /* explicit header, CRC, standard IQ */
    TEST_ASSERT_TRUE(g_chip.lora);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_XOSC, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT32(0U, g_chip.irq);           /* configure left no flag behind */

    radio_up_raw(MESH_PLAN_BAND_2_4_GHZ, 9U, 450);
    TEST_ASSERT_EQUAL_UINT8(255U, g_chip.payload_len);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("receive: starts continuous receive on a clean FIFO, shape first", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFU, g_chip.rx_timeout_ticks);   /* continuous */
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_CLEAR_RX_FIFO));
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_SET_RX));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

/* ---- The payload_len that a send leaves behind -------------------------------------------- */

TEST_CASE("send: the packet's length is the chip's payload_len while it is on air", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(5U, g_chip.payload_len);
    TEST_ASSERT_EQUAL_UINT(5U, g_chip.tx_fifo_n);
    TEST_ASSERT_EQUAL_MEMORY(data, g_chip.tx_fifo, 5U);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_tx_len_mismatch);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("receive after a short send: the whole payload is accepted again - a long packet is heard", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[5];
    uint8_t big[100];
    uint8_t got[255];
    fill_pattern(data, sizeof(data));
    fill_pattern(big, sizeof(big));

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    chip_finish_tx();
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_SEND_DONE, take());
    TEST_ASSERT_EQUAL_UINT8(5U, g_chip.payload_len);             /* what the send left */

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());

    /* The receiver was reshaped BEFORE SetRx, not after it. */
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_UINT8(255U, rx->payload_len_before);
    TEST_ASSERT_EQUAL_UINT16(65535U, rx->pbl_before);

    /* A packet 20 times the length of the last one sent comes in whole. */
    TEST_ASSERT_TRUE(chip_rx_packet(big, sizeof(big), true, true));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_header_err);
    uint32_t const irq = take();
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, irq);
    mesh_drv_lr2021_packet_t info;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
    TEST_ASSERT_EQUAL_UINT8(100U, info.len);
    TEST_ASSERT_EQUAL_MEMORY(big, got, 100U);
    TEST_ASSERT_TRUE(info.crc_ok);
}

TEST_CASE("model: a receiver left at a short payload_len throws a longer packet away", "[radio][rx][model]")
{
    /* The same scene without the driver's repair, to show the model has teeth:
     * this is what the case above would see if receive() did not reshape. */
    radio_up_sub();
    uint8_t data[5];
    uint8_t big[100];
    fill_pattern(data, sizeof(data));
    fill_pattern(big, sizeof(big));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    chip_finish_tx();
    (void)take();

    g_chip.mode             = MESH_DRV_LR2021_MODE_RECEIVE;      /* as a SetRx that skipped the reshape */
    g_chip.rx_timeout_ticks = 0xFFFFFFU;
    TEST_ASSERT_FALSE(chip_rx_packet(big, sizeof(big), true, true));
    TEST_ASSERT_EQUAL_UINT(1U, g_chip.n_header_err);
    uint32_t const irq = take();
    TEST_ASSERT_BITS(CHIP_IRQ_HEADER_ERR, CHIP_IRQ_HEADER_ERR, irq);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, 0U, irq);   /* and no RxDone: nobody hears it */
    TEST_ASSERT_TRUE(chip_rx_packet(data, 5U, true, true));   /* one that fits still comes in */
}

TEST_CASE("listen_for after a short send puts the whole payload back too", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    chip_finish_tx();
    (void)take();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_listen_for(200U));
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_UINT8(255U, rx->payload_len_before);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("listen_for: a timed receive that ends by its own timer raises Timeout and nothing else", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_listen_for(200U));
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    uint32_t const ticks = ((uint32_t)rx->p[0] << 16) | ((uint32_t)rx->p[1] << 8) | rx->p[2];
    TEST_ASSERT_UINT32_WITHIN(1U, 6554U, ticks);       /* 200 ms of a 32.768 kHz RTC */

    chip_advance_us(150000U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    chip_advance_us(60000U);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_TIMEOUT, take());
    TEST_ASSERT_TRUE(g_irq_calls > 0U);
}

TEST_CASE("listen_for: from a receive already running, it ends that one first", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_listen_for(100U));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);     /* SetRx did not hit a chip already in Rx */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
    TEST_ASSERT_NOT_EQUAL(0xFFFFFFU, g_chip.rx_timeout_ticks);   /* a timed one now */
}

TEST_CASE("listen_for: out-of-range times are refused", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_listen_for(0U));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_listen_for(1001U));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

/* ---- After a test preamble ------------------------------------------------------------------- */

TEST_CASE("receive after a preamble test: both the preamble and the payload_len come back", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 200U));
    TEST_ASSERT_GREATER_THAN_UINT16(8U, g_chip.pbl);      /* a long preamble is up */
    TEST_ASSERT_EQUAL_UINT8(1U, g_chip.payload_len);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    uint16_t pbl;
    uint8_t  len;
    packet_shape(&pbl, &len);
    TEST_ASSERT_EQUAL_UINT16(65535U, pbl);
    TEST_ASSERT_EQUAL_UINT8(255U, len);
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_UINT16(65535U, rx->pbl_before);
    TEST_ASSERT_EQUAL_UINT8(255U, rx->payload_len_before);
}

TEST_CASE("send after a preamble test: an ordinary preamble again, the packet's own length", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_test(MESH_DRV_LR2021_TEST_PREAMBLE, 200U));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());

    uint8_t data[7];
    fill_pattern(data, sizeof(data));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(8U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT8(7U, g_chip.payload_len);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_tx_len_mismatch);
}

/* ---- A preamble longer than the setting's ------------------------------------------------------ */

TEST_CASE("receive after a long-preamble send: any preamble is heard again, and the send went with its own", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[6];
    fill_pattern(data, sizeof(data));

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send_preamble(data, sizeof(data), 46U));
    TEST_ASSERT_EQUAL_UINT16(46U, g_chip.pbl);           /* on air: the packet's own */
    chip_finish_tx();
    (void)take();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_UINT16(65535U, rx->pbl_before);
    TEST_ASSERT_EQUAL_UINT8(255U, rx->payload_len_before);

    /* The next ordinary send is the setting's preamble again, not the receiver's. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(8U, g_chip.pbl);
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("listen_for: the timed receiver hears any preamble too", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT16(8U, g_chip.pbl);
    chip_finish_tx();
    (void)take();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_listen_for(200U));
    const chip_cmd_t *rx = chip_last(OPC_SET_RX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_UINT16(65535U, rx->pbl_before);
}

/* ---- Receive is idempotent --------------------------------------------------------------------- */

TEST_CASE("receive: already in Rx it does nothing at all - SetRx would fail and ClearRxFifo could drop a packet", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_RX));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_CLEAR_RX_FIFO));
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_LORA_PACKET));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_EQUAL_HEX32(0U, g_chip.irq);            /* no CmdError raised on DIO9 */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
}

TEST_CASE("receive: a packet that landed while it was called is still there afterwards", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    uint8_t p[6];
    fill_pattern(p, sizeof(p));
    TEST_ASSERT_TRUE(chip_rx_packet(p, sizeof(p), true, true));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());   /* the owner asks again, as it may */

    uint32_t const irq = take();
    uint8_t        got[255];
    mesh_drv_lr2021_packet_t info;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
    TEST_ASSERT_EQUAL_UINT8(6U, info.len);
    TEST_ASSERT_EQUAL_MEMORY(p, got, 6U);
}

TEST_CASE("receive: continuous - packet after packet, the chip stays in Rx", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    unsigned const rxs = chip_count(OPC_SET_RX);

    for (unsigned i = 0U; i < 3U; i++) {
        uint8_t p[4];
        fill_pattern(p, sizeof(p));
        TEST_ASSERT_TRUE(chip_rx_packet(p, sizeof(p), true, true));
        TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
        uint32_t const  irq = take();
        uint8_t         got[255];
        mesh_drv_lr2021_packet_t info;
        TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
        TEST_ASSERT_EQUAL_UINT8(4U, info.len);
        TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    }
    TEST_ASSERT_EQUAL_UINT(rxs, chip_count(OPC_SET_RX));   /* never re-armed */
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
}

TEST_CASE("receive: never aborts a transmit", "[radio][rx]")
{
    radio_up_sub();
    uint8_t data[5];
    fill_pattern(data, sizeof(data));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_send(data, sizeof(data)));
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_rx_over_tx);
    TEST_ASSERT_EQUAL_UINT(0U, chip_count(OPC_SET_RX));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_chip.mode);
}

/* ---- Reading ------------------------------------------------------------------------------------ */

TEST_CASE("read: a packet longer than the buffer is cut to it, and the rest is not delivered", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    uint8_t p[10];
    fill_pattern(p, sizeof(p));
    TEST_ASSERT_TRUE(chip_rx_packet(p, sizeof(p), true, true));
    uint32_t const irq = take();

    uint8_t         got[4];
    mesh_drv_lr2021_packet_t info;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
    TEST_ASSERT_EQUAL_UINT8(4U, info.len);
    TEST_ASSERT_EQUAL_MEMORY(p, got, 4U);
}

TEST_CASE("read: the RSSI and SNR are the packet's, scaled as the datasheet says", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    uint8_t p[2] = { 9, 9 };
    TEST_ASSERT_TRUE(chip_rx_packet(p, sizeof(p), true, true));
    g_chip.last_rssi2 = 181U;     /* -90.5 dBm */
    g_chip.last_snr4  = -22;      /* -5.5 dB   */
    uint32_t const irq = take();

    uint8_t         got[8];
    mesh_drv_lr2021_packet_t info;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
    TEST_ASSERT_EQUAL_FLOAT(-90.5f, info.rssi_dbm);
    TEST_ASSERT_EQUAL_FLOAT(-5.5f, info.snr_db);
}

TEST_CASE("read: refuses no buffer, no room, no result", "[radio][rx]")
{
    radio_up_sub();
    uint8_t         got[4];
    mesh_drv_lr2021_packet_t info;
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_read(NULL, 4U, 0U, &info));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_read(got, 0U, 0U, &info));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_read(got, 4U, 0U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

TEST_CASE("rssi: the instantaneous reading, nine bits, half-dB steps", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    float dbm = 0.0f;

    g_chip.inst_rssi2 = 181U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_rssi(&dbm));
    TEST_ASSERT_EQUAL_FLOAT(-90.5f, dbm);
    g_chip.inst_rssi2 = 240U;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_rssi(&dbm));
    TEST_ASSERT_EQUAL_FLOAT(-120.0f, dbm);
}

TEST_CASE("flush_rx: empties the FIFO of a packet nobody will read", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    uint8_t p[6];
    fill_pattern(p, sizeof(p));
    TEST_ASSERT_TRUE(chip_rx_packet(p, sizeof(p), true, true));
    TEST_ASSERT_EQUAL_UINT(6U, g_chip.rx_fifo_n);

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_flush_receive());
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.rx_fifo_n);
}

TEST_CASE("standby: ends a receive, and the mode says so", "[radio][rx]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    mesh_drv_lr2021_mode_t m;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_mode(&m));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, m);
}
