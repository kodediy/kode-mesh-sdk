/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_cad.c
 * @brief The channel search on the wire, and the flags that say a packet is coming.
 */
#include <stdint.h>

#include "unity.h"

#include "lr_chip.h"
#include "mesh_drv_lora.h"
#include "mesh_drv_lr2021.h"
#include "radio_harness.h"

TEST_CASE("cad: from a receive, standby first, then the setting's threshold and the chip's only exit", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    chip_log_clear();

    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad(2U, false));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_TRUE(g_chip.cad_on);
    TEST_ASSERT_EQUAL_UINT8(2U, g_chip.cad_symbols);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.cad_pnr_delta);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.cad_exit);
    TEST_ASSERT_EQUAL_UINT8(mesh_drv_lora_cad_peak(9U, 2U), g_chip.cad_peak);
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_SET_STANDBY));
}

TEST_CASE("cad: fast is the datasheet's pnr_delta", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad(3U, true));
    TEST_ASSERT_EQUAL_UINT8(8U, g_chip.cad_pnr_delta);
}

TEST_CASE("cad: an empty channel says done alone, and the chip falls back to standby", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad(2U, false));
    chip_advance_us(mesh_drv_lora_cad_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 2U) + 100U);

    uint32_t const f = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U);
    TEST_ASSERT_FALSE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);
}

TEST_CASE("cad: somebody talking says detected and done, and not before its symbols", "[radio][cad]")
{
    radio_up_sub();
    g_chip.channel_active = true;
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad(4U, false));
    chip_advance_us(mesh_drv_lora_cad_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 4U) / 2U);
    TEST_ASSERT_EQUAL_HEX32(0U, mesh_drv_lr2021_interrupts_take());
    chip_advance_us(mesh_drv_lora_cad_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 4U));

    uint32_t const f = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U);
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U);
}

TEST_CASE("cad: refused for a bad number of symbols or no setting, and nothing goes to the chip", "[radio][cad]")
{
    radio_open();
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad(2U, false));   /* no setting yet */
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad(0U, false));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad(5U, false));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}

TEST_CASE("cad: a standby ends a search, and no flag comes of it", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad(4U, false));
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_standby());
    chip_advance_us(1000000U);
    TEST_ASSERT_FALSE((mesh_drv_lr2021_interrupts_take() & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U);
}

TEST_CASE("arrival: the preamble, the header and a bad header reach DIO9", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_receive());
    (void)mesh_drv_lr2021_interrupts_take();

    chip_raise(MESH_DRV_LR2021_INTERRUPT_PREAMBLE);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_PREAMBLE, mesh_drv_lr2021_interrupts_take());
    chip_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_HEADER_VALID, mesh_drv_lr2021_interrupts_take());
    chip_raise(MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR);
    TEST_ASSERT_TRUE(mesh_drv_lr2021_dio9());
    TEST_ASSERT_EQUAL_HEX32(MESH_DRV_LR2021_INTERRUPT_HEADER_ERROR, mesh_drv_lr2021_interrupts_take());
}

TEST_CASE("cad receive: only a preamble counts, a detection stays in Rx, and the receiver's shape is put back",
          "[radio][cad]")
{
    radio_up_sub();
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad_receive(2U, false));
    TEST_ASSERT_EQUAL_UINT(0U, g_chip.n_failed);
    TEST_ASSERT_TRUE(g_chip.cad_on);
    TEST_ASSERT_TRUE(g_chip.cad_preamble_only);
    TEST_ASSERT_EQUAL_UINT8(0x01U, g_chip.cad_exit);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFU, g_chip.cad_timeout);
    TEST_ASSERT_EQUAL_UINT8(0U, g_chip.cad_pnr_delta);
    TEST_ASSERT_EQUAL_UINT8(mesh_drv_lora_cad_peak(9U, 2U), g_chip.cad_peak);
    TEST_ASSERT_EQUAL_UINT(1U, chip_count(OPC_CLEAR_RX_FIFO));

    g_chip.channel_active = true;
    chip_advance_us(mesh_drv_lora_cad_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 2U) + 100U);
    uint32_t const f = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U);
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_RECEIVE, g_chip.mode);

    /* The packet comes in as a single receive's, then the chip falls back. */
    static const uint8_t pkt[5] = { 1U, 2U, 3U, 4U, 5U };
    TEST_ASSERT_TRUE(chip_rx_packet(pkt, sizeof(pkt), true, true));
    TEST_ASSERT_TRUE((mesh_drv_lr2021_interrupts_take() & MESH_DRV_LR2021_INTERRUPT_RECEIVE_DONE) != 0U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
}

TEST_CASE("cad receive: nothing seen falls back to standby; fast is the datasheet's pnr_delta", "[radio][cad]")
{
    radio_up_sub();
    TEST_ASSERT_EQUAL(KODE_OK, mesh_drv_lr2021_cad_receive(1U, true));
    TEST_ASSERT_EQUAL_UINT8(8U, g_chip.cad_pnr_delta);
    TEST_ASSERT_TRUE(g_chip.cad_preamble_only);
    chip_advance_us(mesh_drv_lora_cad_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 1U) + 100U);
    uint32_t const f = mesh_drv_lr2021_interrupts_take();
    TEST_ASSERT_TRUE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DONE) != 0U);
    TEST_ASSERT_FALSE((f & MESH_DRV_LR2021_INTERRUPT_CAD_DETECTED) != 0U);
    TEST_ASSERT_EQUAL(MESH_DRV_LR2021_MODE_STANDBY_RC, g_chip.mode);
}

TEST_CASE("cad receive: refused for a bad number of symbols or no setting, and nothing goes to the chip",
          "[radio][cad]")
{
    radio_open();
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad_receive(2U, false));
    radio_up_sub();
    chip_log_clear();
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad_receive(0U, false));
    TEST_ASSERT_EQUAL(KODE_DENIED, mesh_drv_lr2021_cad_receive(5U, false));
    TEST_ASSERT_EQUAL_UINT(0U, chip_log_len());
}
