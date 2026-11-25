/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_crc.c
 * @brief The CRC verdict. GetLoraPacketStatus' crc bit says the packet CARRIED a
 *        CRC (the header said one follows); a wrong one is the CrcError flag of
 *        the interrupt word that announced the packet. The driver reads the
 *        verdict from there, and the flag has to be routed to DIO9 to reach it.
 */
#include <stdint.h>

#include "unity.h"

#include "radio_harness.h"

static const uint8_t PAYLOAD[8] = { 'C', 'R', 'C', ' ', 't', 'e', 's', 't' };

/** @brief A packet arrives and is read with the interrupt word that announced it. */
static void receive_one(bool crc_on, bool crc_good, uint32_t *irq, mesh_drv_lr2021_packet_t *info, uint8_t *got)
{
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_TRUE(chip_rx_packet(PAYLOAD, sizeof(PAYLOAD), crc_on, crc_good));
    *irq = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, 255U, *irq, info));
}

TEST_CASE("crc: a packet whose CRC matched is crc_ok", "[radio][crc]")
{
    radio_up_sub();
    uint32_t        irq;
    mesh_drv_lr2021_packet_t info;
    uint8_t         got[255];

    receive_one(true, true, &irq, &info, got);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, irq);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, 0U, irq);
    TEST_ASSERT_TRUE(info.crc_ok);
    TEST_ASSERT_EQUAL_UINT8(sizeof(PAYLOAD), info.len);
    TEST_ASSERT_EQUAL_MEMORY(PAYLOAD, got, sizeof(PAYLOAD));
}

TEST_CASE("crc: a wrong CRC is not crc_ok - and the payload is still delivered, to be judged", "[radio][crc]")
{
    radio_up_sub();
    uint32_t        irq;
    mesh_drv_lr2021_packet_t info;
    uint8_t         got[255];

    receive_one(true, false, &irq, &info, got);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE, irq);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, irq);
    TEST_ASSERT_FALSE(info.crc_ok);
    TEST_ASSERT_EQUAL_UINT8(sizeof(PAYLOAD), info.len);
    TEST_ASSERT_EQUAL_MEMORY(PAYLOAD, got, sizeof(PAYLOAD));
}

TEST_CASE("crc: the chip's own crc bit is set for a bad packet, so it cannot be the verdict", "[radio][crc]")
{
    /* Not a test of the driver: of why the driver must not use that bit. The
     * model reports what the datasheet says it does - Crc_On, from the header -
     * for a packet whose CRC failed. A driver that read crc_ok from it would
     * call this packet good. */
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_TRUE(chip_rx_packet(PAYLOAD, sizeof(PAYLOAD), true, false));
    TEST_ASSERT_TRUE(g_chip.last_crc_on);
}

TEST_CASE("crc: a packet sent without a CRC has nothing to vouch for it", "[radio][crc]")
{
    radio_up_sub();
    uint32_t        irq;
    mesh_drv_lr2021_packet_t info;
    uint8_t         got[255];

    receive_one(false, true, &irq, &info, got);
    TEST_ASSERT_BITS(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, 0U, irq);
    TEST_ASSERT_FALSE(info.crc_ok);
    TEST_ASSERT_EQUAL_UINT8(sizeof(PAYLOAD), info.len);
}

TEST_CASE("crc: the verdict follows the interrupt word it is given, packet by packet", "[radio][crc]")
{
    radio_up_sub();
    uint32_t        irq;
    mesh_drv_lr2021_packet_t info;
    uint8_t         got[255];

    receive_one(true, false, &irq, &info, got);
    TEST_ASSERT_FALSE(info.crc_ok);
    TEST_ASSERT_TRUE(chip_rx_packet(PAYLOAD, sizeof(PAYLOAD), true, true));   /* still in Rx: continuous */
    irq = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), irq, &info));
    TEST_ASSERT_TRUE(info.crc_ok);                                            /* nothing carried over */

    /* The same packet, read with a word that has the flag: the flag decides. */
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_read(got, sizeof(got), MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, &info));
    TEST_ASSERT_FALSE(info.crc_ok);
}

/* ---- The flag has to reach DIO9 ----------------------------------------------------------- */

TEST_CASE("irq mask: every flag the owner acts on is routed to DIO9, the CRC error among them", "[radio][crc]")
{
    radio_up_raw(MESH_PLAN_BAND_SUB_GHZ, 9U, 850);
    const chip_cmd_t *c = chip_last(OPC_SET_DIO_IRQ);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT8(5U, c->n);
    TEST_ASSERT_EQUAL_UINT8(9U, c->p[0]);                 /* DIO9 */
    uint32_t const mask = ((uint32_t)c->p[1] << 24) | ((uint32_t)c->p[2] << 16)
                          | ((uint32_t)c->p[3] << 8) | (uint32_t)c->p[4];
    uint32_t const want = MESH_DRV_LR2021_INTERRUPT_AMPLIFIER_FAULT | MESH_DRV_LR2021_INTERRUPT_ERROR | MESH_DRV_LR2021_INTERRUPT_COMMAND_ERROR
                          | MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE | MESH_DRV_LR2021_INTERRUPT_SEND_DONE | MESH_DRV_LR2021_INTERRUPT_TIMEOUT
                          | MESH_DRV_LR2021_INTERRUPT_CRC_ERROR | MESH_DRV_LR2021_INTERRUPT_PREAMBLE
                          | MESH_DRV_LR2021_INTERRUPT_HEADER_VALID | MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR
                          | MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED | MESH_DRV_LR2021_INTERRUPT_CAD_DONE;
    TEST_ASSERT_EQUAL_HEX32(want, mask);
    TEST_ASSERT_EQUAL_HEX32(want, g_chip.dio9_mask);
}

TEST_CASE("crc: a CRC error alone raises DIO9", "[radio][crc]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL_UINT(0U, g_irq_calls);

    chip_raise(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR);

    TEST_ASSERT_EQUAL_UINT(1U, g_irq_calls);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_CRC_ERROR, mesh_drv_lr2021_interrupts_take());
    TEST_ASSERT_FALSE(mesh_drv_lr2021_dio9());
}
