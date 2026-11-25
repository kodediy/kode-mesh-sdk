/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_owner_rx.c
 * @brief The receive path as the owner runs it: a packet is read with the
 *        interrupt word that announced it (the CRC verdict lives there), the
 *        receiver is never re-armed while it is already receiving, and it comes
 *        back by itself, once, after a send. The chip's own rules for payload
 *        length are tests/host_drv's; here it is the owner's calls that count.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_bsp.h"
#include "owner_fakes.h"
#include "owner_harness.h"

static const uint8_t HELLO[5] = { 'h', 'e', 'l', 'l', 'o' };

static kode_status_t listen_ms(uint32_t ms)
{
    const mesh_aos_request_t req = { .kind = MESH_REQUEST_RADIO_LISTEN, .u = ms };
    return mesh_aos_call(&req, 2000U, NULL, NULL);
}

/* ---- A packet comes in --------------------------------------------------------- */

TEST_CASE("rx: a packet is delivered and the receiver is NOT re-armed (it is still in Rx)", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const arms     = g_lr.n_receive;
    unsigned const standbys = g_lr.n_standby;
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    lr_fake_inject(HELLO, sizeof(HELLO), true);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_waiting == 1U, 1000));

    mesh_aos_packet_t rx;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&rx));
    TEST_ASSERT_EQUAL_UINT8(5U, rx.length);
    TEST_ASSERT_EQUAL_MEMORY(HELLO, rx.data, 5U);
    TEST_ASSERT_TRUE(rx.crc_valid);
    TEST_ASSERT_EQUAL_FLOAT(-90.0f, rx.rssi_dbm);
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, g_lr.read_irq);   /* the read got the flags */
    TEST_ASSERT_TRUE(h_event_with(MESH_AOS_EVENT_PACKET, 5U));

    /* SetRx on a chip already in Rx fails and clearing the FIFO there can drop
     * the next packet: continuous receive needs neither. */
    TEST_ASSERT_EQUAL_UINT(arms, g_lr.n_receive);
    TEST_ASSERT_EQUAL_UINT(standbys, g_lr.n_standby);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("rx: packet after packet, and the receiver is never touched", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const arms = g_lr.n_receive;

    for (unsigned i = 0U; i < 3U; i++) {
        lr_fake_inject(HELLO, sizeof(HELLO), true);
        lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
        TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_received == (i + 1U), 1000));
    }
    TEST_ASSERT_EQUAL_UINT32(3U, h_status().packets_received);
    TEST_ASSERT_EQUAL_UINT(arms, g_lr.n_receive);
    TEST_ASSERT_EQUAL_UINT(3U, g_lr.n_read);
}

TEST_CASE("rx: a long packet is delivered whole", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    uint8_t big[MESH_AOS_PAYLOAD_MAX];
    for (unsigned i = 0U; i < sizeof(big); i++) {
        big[i] = (uint8_t)i;
    }
    lr_fake_inject(big, sizeof(big), true);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_waiting == 1U, 1000));
    mesh_aos_packet_t rx;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&rx));
    TEST_ASSERT_EQUAL_UINT8(255U, rx.length);
    TEST_ASSERT_EQUAL_MEMORY(big, rx.data, sizeof(big));
}

/* ---- The CRC verdict --------------------------------------------------------- */

TEST_CASE("crc: the verdict is the CRC flag of the same interrupt: a bad packet is delivered, flagged", "[owner][rx][crc]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_inject(HELLO, sizeof(HELLO), true);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_waiting == 1U, 1000));

    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, g_lr.read_irq);
    mesh_aos_packet_t rx;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&rx));
    TEST_ASSERT_FALSE(rx.crc_valid);
    TEST_ASSERT_EQUAL_UINT8(5U, rx.length);
    TEST_ASSERT_EQUAL_MEMORY(HELLO, rx.data, 5U);
}

TEST_CASE("crc: a good one right after a bad one is good (nothing is carried over)", "[owner][rx][crc]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_inject(HELLO, sizeof(HELLO), true);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_received == 1U, 1000));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_received == 2U, 1000));

    mesh_aos_packet_t first;
    mesh_aos_packet_t second;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&first));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&second));
    TEST_ASSERT_FALSE(first.crc_valid);
    TEST_ASSERT_TRUE(second.crc_valid);
}

TEST_CASE("crc: a packet sent without a CRC has nothing to vouch for it", "[owner][rx][crc]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_inject(HELLO, sizeof(HELLO), false);
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_waiting == 1U, 1000));
    mesh_aos_packet_t rx;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_aos_packet_read(&rx));
    TEST_ASSERT_FALSE(rx.crc_valid);
}

TEST_CASE("crc: a CRC error with no packet announced flushes the FIFO and delivers nothing", "[owner][rx][crc]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR);
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_flush == 1U, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_read);
    TEST_ASSERT_EQUAL_UINT8(0U, h_status().packets_waiting);
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_received);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);   /* and it keeps listening */
}

TEST_CASE("rx: a packet whose read fails is lost, and the FIFO is emptied so it does not lead the next", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    lr_fake_inject(HELLO, sizeof(HELLO), true);
    g_lr.read_rc = KODE_IO_ERROR;
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_read == 1U, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_flush == 1U, 1000));
    g_lr.read_rc = KODE_OK;
    TEST_ASSERT_EQUAL_UINT8(0U, h_status().packets_waiting);
    TEST_ASSERT_EQUAL_UINT32(0U, h_status().packets_received);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);   /* and it keeps listening */

    /* the next packet is read as it should be */
    lr_fake_raise(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().packets_waiting == 1U, 1000));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_flush);
}

/* ---- Around a send ------------------------------------------------------------- */

TEST_CASE("rx: after a send the receiver comes back exactly once, on the done flag", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    unsigned const arms = g_lr.n_receive;

    TEST_ASSERT_EQUAL(KODE_OK, h_send(HELLO, sizeof(HELLO), 2000U));
    TEST_ASSERT_TRUE(h_status().sending);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_EQUAL_UINT(arms, g_lr.n_receive);   /* not while on air */

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_RECEIVE, 500));
    TEST_ASSERT_EQUAL_UINT(arms + 1U, g_lr.n_receive);
    TEST_ASSERT_EQUAL_UINT32(1U, h_status().packets_sent);
    TEST_ASSERT_EQUAL(KODE_OK, h_send_answer(1000U, NULL));   /* the call's answer is the packet gone */
    TEST_ASSERT_TRUE(h_status().receiving);
}

TEST_CASE("rx: receive on during a send waits for the done flag instead of cutting the packet", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_send(HELLO, sizeof(HELLO), 2000U));
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));

    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_receive);       /* the chip is not asked */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode); /* and the packet is not cut */
    TEST_ASSERT_TRUE(h_status().receiving);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.mode == MESH_DRV_LR2021_MODE_RECEIVE, 1000));
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_receive);
}

TEST_CASE("rx: receive off during a send does not cut the packet short, and nothing is armed after", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_OK, h_send(HELLO, sizeof(HELLO), 2000U));
    unsigned const standbys = g_lr.n_standby;
    unsigned const arms     = g_lr.n_receive;

    TEST_ASSERT_EQUAL(KODE_OK, h_receive(false));
    TEST_ASSERT_EQUAL_UINT(standbys, g_lr.n_standby);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);
    TEST_ASSERT_FALSE(h_status().receiving);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    vTaskDelay(pdMS_TO_TICKS(60));
    TEST_ASSERT_EQUAL_UINT(arms, g_lr.n_receive);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("rx: a listen is refused while a packet is on air, and the RSSI is not read then", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().rssi_valid, 1000));   /* sampled while the receiver listens */
    TEST_ASSERT_EQUAL(KODE_OK, h_send(HELLO, sizeof(HELLO), 2000U));

    TEST_ASSERT_EQUAL(KODE_DENIED, listen_ms(100U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_listen);
    unsigned const reads = g_lr.n_rssi;
    vTaskDelay(pdMS_TO_TICKS(3U * MESH_BSP_RSSI_MS));
    TEST_ASSERT_EQUAL_UINT(reads, g_lr.n_rssi);   /* the chip is on air: it is not asked */
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_TRANSMIT, g_lr.mode);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_SEND_DONE);
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().sending, 1000));
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_rssi > reads, 1000));    /* and again when the receiver is back */
}

TEST_CASE("rssi: sampled while the receiver listens, the last reading in the status, none with it off", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_FALSE(h_status().rssi_valid);                  /* the receiver is off: no sample */
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_rssi);

    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_TRUE(WAIT_FOR(h_status().rssi_valid, 1000));
    TEST_ASSERT_EQUAL_FLOAT(-88.5f, h_status().last_rssi_dbm);
    unsigned const first = g_lr.n_rssi;
    TEST_ASSERT_TRUE(WAIT_FOR(g_lr.n_rssi > first, 1000));    /* kept fresh, not read once */

    TEST_ASSERT_EQUAL(KODE_OK, h_receive(false));
    TEST_ASSERT_TRUE(WAIT_FOR(!h_status().rssi_valid, 1000));
    unsigned const off = g_lr.n_rssi;
    vTaskDelay(pdMS_TO_TICKS(3U * MESH_BSP_RSSI_MS));
    TEST_ASSERT_EQUAL_UINT(off, g_lr.n_rssi);                  /* off: the chip is not asked */

    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_FALSE(h_status().rssi_valid && (g_lr.n_rssi == off));   /* a new listen begins with no reading */
}

/* ---- Listening for a while ------------------------------------------------------ */

TEST_CASE("listen: the chip's timeout flag raises the app's timeout event", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, listen_ms(200U));
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    lr_fake_chip_stops(MESH_DRV_LR2021_INTERRUPT_TIMEOUT);
    TEST_ASSERT_TRUE(WAIT_FOR(h_events(MESH_AOS_EVENT_LISTEN_END) == 1U, 1000));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_RADIO_ERROR));
}

TEST_CASE("listen: receive on during a listen ends the listen first, so its timer cannot cut the receive", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, listen_ms(500U));
    unsigned const standbys = g_lr.n_standby;

    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    TEST_ASSERT_GREATER_THAN_UINT(standbys, g_lr.n_standby);
    TEST_ASSERT_EQUAL_UINT(1U, g_lr.n_receive);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);

    /* and no stale listen timeout is raised later */
    vTaskDelay(pdMS_TO_TICKS(800));
    TEST_ASSERT_EQUAL_UINT(0U, h_events(MESH_AOS_EVENT_LISTEN_END));
}

TEST_CASE("listen: one that the chip will not start leaves the receiver off, truthfully", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_receive(true));
    g_lr.listen_rc = KODE_IO_ERROR;

    TEST_ASSERT_EQUAL(KODE_IO_ERROR, listen_ms(100U));
    TEST_ASSERT_FALSE(h_status().receiving);
    TEST_ASSERT_NOT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_lr.mode);
}

TEST_CASE("receive: on is refused with no radio set", "[owner][rx]")
{
    harness_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, h_call(MESH_REQUEST_RADIO_END, 2000U));
    TEST_ASSERT_EQUAL(KODE_DENIED, h_receive(true));
    TEST_ASSERT_EQUAL(KODE_DENIED, listen_ms(100U));
    TEST_ASSERT_EQUAL_UINT(0U, g_lr.n_receive);
}
