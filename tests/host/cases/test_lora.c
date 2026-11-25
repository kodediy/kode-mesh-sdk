/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_lora.c
 * @brief mesh_drv_lora: LoRa's arithmetic, run without a chip or a clock.
 *
 * The airtime Semtech's worked example pins to the microsecond, the low
 * data-rate option, the channel-activity symbols, and how the LR2021's own
 * timers count: its 32.768 kHz periods in 24 bits, the send timeout that must
 * never be 0, and the preamble that keeps a test signal on air. The owner's
 * rules built on these are test_plan.c's. Plain C, so this is what
 * tools/check_layers.sh rule 4 keeps buildable on the host.
 */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "mesh_drv_lora.h"

/* ---- Airtime: Semtech's formula ---------------------------------------------- */

TEST_CASE("airtime: the Semtech worked example, SF7 BW125 16 B = 51456 us", "[lora]")
{
    /* The datasheet's own worked example. */
    TEST_ASSERT_EQUAL_UINT32(51456U, mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 16, false));
}

TEST_CASE("airtime: longer payload, longer on air", "[lora]")
{
    TEST_ASSERT_GREATER_THAN_UINT32(mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 16, false),
                                    mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 32, false));
    /* Airtime is a whole number of symbols, so one more byte never shortens
     * it but need not lengthen it until a symbol boundary is crossed: the
     * sweep is non-decreasing, and longer overall. */
    uint32_t prev = 0;
    for (uint8_t len = 1; len <= 64; len++) {
        uint32_t const us = mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, len, true);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(prev, us);
        prev = us;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, 1, true),
                                    mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, 64, true));
}

TEST_CASE("airtime: a slower spreading factor is longer on air", "[lora]")
{
    TEST_ASSERT_GREATER_THAN_UINT32(mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 20, false),
                                    mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, 20, true));
    /* SF7 < SF8 < ... < SF12, all else equal */
    uint32_t prev = 0;
    for (uint8_t sf = 7; sf <= 12; sf++) {
        uint32_t const us = mesh_drv_lora_airtime_us(sf, MESH_DRV_LORA_BANDWIDTH_125, 20, false);
        TEST_ASSERT_GREATER_THAN_UINT32(prev, us);
        prev = us;
    }
}

TEST_CASE("airtime: a wider bandwidth is shorter on air", "[lora]")
{
    uint32_t const bw125 = mesh_drv_lora_airtime_us(9, MESH_DRV_LORA_BANDWIDTH_125, 32, false);
    uint32_t const bw500 = mesh_drv_lora_airtime_us(9, MESH_DRV_LORA_BANDWIDTH_500, 32, false);
    uint32_t const bw812 = mesh_drv_lora_airtime_us(9, MESH_DRV_LORA_BANDWIDTH_812, 32, false);
    TEST_ASSERT_GREATER_THAN_UINT32(bw500, bw125);
    TEST_ASSERT_GREATER_THAN_UINT32(bw812, bw500);
}

TEST_CASE("airtime: a bandwidth code it does not know is zero", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_airtime_us(7, 0x99U, 16, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_airtime_us(7, 0x00U, 16, false));
}

TEST_CASE("airtime: an empty payload and a too-low spreading factor are zero", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 0, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_airtime_us(4, MESH_DRV_LORA_BANDWIDTH_125, 16, false));
}

/* ---- The low data-rate option ------------------------------------------------ */

TEST_CASE("low data rate: on when a symbol outlasts 16 ms", "[lora]")
{
    /* At BW125 the symbol passes 16 ms at SF11 (16.4 ms) and SF12 (32.8). */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_125));
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_125));
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(10, MESH_DRV_LORA_BANDWIDTH_125));   /* 8.2 ms */
}

TEST_CASE("low data rate: a wide bandwidth keeps the symbol short", "[lora]")
{
    /* 9.9.1: off at BW500 and BW1000 whatever the SF, and at BW250 up to SF11. */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_500));    /* 8.2 ms */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_500));    /* 4.1 ms */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_1000));   /* 4.1 ms */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_250));    /* 8.2 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_250));     /* 16.4 ms */
}

TEST_CASE("low data rate: on at SF11 and SF12 on the SX1280's bandwidths", "[lora]")
{
    /* 9.9.1, "including SX1280 compatible bandwidths BW800/400/200", and what
     * RadioLib, so Meshtastic, sends at 2.4 GHz: LongFast is SF11 at BW812, a
     * 2.5 ms symbol, and goes with the option. */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_812));     /* 2.5 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_812));     /* 5.0 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_406));     /* 5.0 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_406));     /* 10.1 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(11, MESH_DRV_LORA_BANDWIDTH_203));     /* 10.1 ms */
    TEST_ASSERT_TRUE(mesh_drv_lora_low_data_rate(12, MESH_DRV_LORA_BANDWIDTH_203));     /* 20.2 ms */
    /* Below SF11 they keep the symbol rule: MediumSlow, SF10 at BW812, is off. */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(10, MESH_DRV_LORA_BANDWIDTH_812));    /* 1.3 ms */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(10, MESH_DRV_LORA_BANDWIDTH_203));    /* 5.0 ms */
}

TEST_CASE("low data rate: a spreading factor past 12 is refused, not shifted", "[lora]")
{
    /* SF over 12 would make the symbol-time shift below undefined; the guard
     * returns false instead of shifting. */
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(13, MESH_DRV_LORA_BANDWIDTH_125));
    TEST_ASSERT_FALSE(mesh_drv_lora_low_data_rate(255, MESH_DRV_LORA_BANDWIDTH_125));
}

/* ---- Channel-activity detection ---------------------------------------------- */

TEST_CASE("channel activity: three symbols at SF12, four below", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT8(3U, mesh_drv_lora_cad_symbols(12));
    TEST_ASSERT_EQUAL_UINT8(4U, mesh_drv_lora_cad_symbols(11));
    TEST_ASSERT_EQUAL_UINT8(4U, mesh_drv_lora_cad_symbols(7));
}

/* ---- The chip's own timers ---------------------------------------------------- */

TEST_CASE("timer: microseconds to 32.768 kHz periods, rounded up, never 0", "[lora][timer]")
{
    TEST_ASSERT_EQUAL_UINT32(1U, mesh_drv_lora_timer_ticks(0U));        /* 0 would switch the timer off */
    TEST_ASSERT_EQUAL_UINT32(1U, mesh_drv_lora_timer_ticks(1U));
    TEST_ASSERT_EQUAL_UINT32(1U, mesh_drv_lora_timer_ticks(30U));       /* 30.5 us is one period       */
    TEST_ASSERT_EQUAL_UINT32(2U, mesh_drv_lora_timer_ticks(31U));
    TEST_ASSERT_EQUAL_UINT32(2U, mesh_drv_lora_timer_ticks(61U));
    TEST_ASSERT_EQUAL_UINT32(3U, mesh_drv_lora_timer_ticks(62U));
    TEST_ASSERT_EQUAL_UINT32(32768U, mesh_drv_lora_timer_ticks(1000000U));    /* one second */
    TEST_ASSERT_EQUAL_UINT32(1966080U, mesh_drv_lora_timer_ticks(60000000U)); /* one minute */
}

TEST_CASE("timer: the count stops at the chip's 24 bits, 512 s", "[lora][timer]")
{
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFU, mesh_drv_lora_timer_ticks(512000000U));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFU, mesh_drv_lora_timer_ticks(UINT32_MAX));
    TEST_ASSERT_TRUE(mesh_drv_lora_timer_ticks(511000000U) < 0xFFFFFFU);
}

TEST_CASE("timer: more time is never fewer periods", "[lora][timer]")
{
    uint32_t prev = 0U;
    for (uint32_t us = 0U; us < 3000000U; us += 997U) {
        uint32_t const t = mesh_drv_lora_timer_ticks(us);
        TEST_ASSERT_TRUE(t >= prev);
        TEST_ASSERT_TRUE(t >= 1U);
        prev = t;
    }
}

TEST_CASE("tx timeout: outlasts the packet by a quarter, 20 ms at least", "[lora][timer]")
{
    /* a short packet: the 20 ms floor decides. SF7 BW125 16 B = 51456 us. */
    uint32_t const air = mesh_drv_lora_airtime_us(7, MESH_DRV_LORA_BANDWIDTH_125, 16, false);
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(air) >= mesh_drv_lora_timer_ticks(air + 20000U));
    /* a long one: the quarter decides. SF12 BW125 255 B is over 8 s. */
    uint32_t const slow = mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, 255, true);
    TEST_ASSERT_TRUE(slow > 8000000U);
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(slow) >= mesh_drv_lora_timer_ticks(slow + slow / 4U));
    /* and not by much more than that: a safeguard that outlasts the packet
     * by seconds is no safeguard */
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(slow) <= mesh_drv_lora_timer_ticks(slow + slow / 4U) + 1U);
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(air) <= mesh_drv_lora_timer_ticks(air + 20000U) + 1U);
}

TEST_CASE("tx timeout: never 0 (0 switches the chip's timer off), never past 24 bits", "[lora][timer]")
{
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(0U) >= 1U);
    TEST_ASSERT_TRUE(mesh_drv_lora_send_timeout_ticks(0U) >= mesh_drv_lora_timer_ticks(20000U));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFU, mesh_drv_lora_send_timeout_ticks(UINT32_MAX));
    for (uint8_t len = 1U; len < 255U; len++) {
        uint32_t const t = mesh_drv_lora_send_timeout_ticks(mesh_drv_lora_airtime_us(12, MESH_DRV_LORA_BANDWIDTH_125, len, true));
        TEST_ASSERT_TRUE((t >= 1U) && (t <= 0xFFFFFFU));
    }
}

TEST_CASE("preamble: enough symbols to last the time asked, one over", "[lora][timer]")
{
    /* SF9 BW125: a symbol is 4.096 ms; one second is 245 of them, plus one. */
    TEST_ASSERT_EQUAL_UINT16(246U, mesh_drv_lora_preamble_symbols(9, MESH_DRV_LORA_BANDWIDTH_125, 1000U));
    /* SF7 BW125: 1.024 ms; 100 ms is 98 symbols, plus one. */
    TEST_ASSERT_EQUAL_UINT16(99U, mesh_drv_lora_preamble_symbols(7, MESH_DRV_LORA_BANDWIDTH_125, 100U));
}

TEST_CASE("preamble: held between the chip's 16 bits and this SDK's 8-symbol minimum", "[lora][timer]")
{
    TEST_ASSERT_EQUAL_UINT16(8U, mesh_drv_lora_preamble_symbols(9, MESH_DRV_LORA_BANDWIDTH_125, 0U));
    TEST_ASSERT_EQUAL_UINT16(8U, mesh_drv_lora_preamble_symbols(12, MESH_DRV_LORA_BANDWIDTH_125, 1U));
    /* SF7 at BW812 is 157 us a symbol: a minute needs far more than 16 bits */
    TEST_ASSERT_EQUAL_UINT16(65535U, mesh_drv_lora_preamble_symbols(7, MESH_DRV_LORA_BANDWIDTH_812, 60000U));
}

TEST_CASE("preamble: a bandwidth or spreading factor it does not know is 0", "[lora][timer]")
{
    TEST_ASSERT_EQUAL_UINT16(0U, mesh_drv_lora_preamble_symbols(9, 0x99U, 1000U));
    TEST_ASSERT_EQUAL_UINT16(0U, mesh_drv_lora_preamble_symbols(4, MESH_DRV_LORA_BANDWIDTH_125, 1000U));
    TEST_ASSERT_EQUAL_UINT16(0U, mesh_drv_lora_preamble_symbols(13, MESH_DRV_LORA_BANDWIDTH_125, 1000U));
}

TEST_CASE("preamble: a longer time is never a shorter preamble", "[lora][timer]")
{
    uint16_t prev = 0U;
    for (uint32_t ms = 0U; ms <= 60000U; ms += 313U) {
        uint16_t const n = mesh_drv_lora_preamble_symbols(10, MESH_DRV_LORA_BANDWIDTH_125, ms);
        TEST_ASSERT_TRUE(n >= prev);
        prev = n;
    }
}

/* ---- The chip's bandwidths, and a packet with every number given ------------------- */

TEST_CASE("bandwidth: hertz and code go both ways, for all twelve", "[lora]")
{
    static const struct { uint32_t hz; uint8_t code; } ALL[] = {
        { 31250U, 0x02U },  { 41667U, 0x0AU },  { 62500U, 0x03U },  { 83333U, 0x0BU },
        { 101563U, 0x0CU }, { 125000U, 0x04U }, { 203125U, 0x0DU }, { 250000U, 0x05U },
        { 406250U, 0x0EU }, { 500000U, 0x06U }, { 812500U, 0x0FU }, { 1000000U, 0x07U },
    };
    for (size_t i = 0U; i < (sizeof(ALL) / sizeof(ALL[0])); i++) {
        TEST_ASSERT_EQUAL_UINT8(ALL[i].code, mesh_drv_lora_bandwidth_code(ALL[i].hz));
        TEST_ASSERT_EQUAL_UINT32(ALL[i].hz, mesh_drv_lora_bandwidth_hz(ALL[i].code));
    }
}

TEST_CASE("bandwidth: the rounded figures other radios use are the chip's", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_41, mesh_drv_lora_bandwidth_code(41670U));
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_83, mesh_drv_lora_bandwidth_code(83340U));
    TEST_ASSERT_EQUAL_UINT8(MESH_DRV_LORA_BANDWIDTH_101, mesh_drv_lora_bandwidth_code(101562U));
}

TEST_CASE("bandwidth: what the chip does not have is 0", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT8(0U, mesh_drv_lora_bandwidth_code(0U));
    TEST_ASSERT_EQUAL_UINT8(0U, mesh_drv_lora_bandwidth_code(100000U));
    TEST_ASSERT_EQUAL_UINT8(0U, mesh_drv_lora_bandwidth_code(10000000U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_bandwidth_hz(0x00U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_bandwidth_hz(0x08U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_bandwidth_hz(0xFFU));
}

TEST_CASE("airtime: a packet with every number given, worked by hand", "[lora]")
{
    /* SF11, BW250: a symbol is 8.192 ms. 20 bytes: 160 - 44 + 44 = 160 bits, 44 bits
     * a group, 4 groups. 4/5: 8 + 20 = 28 symbols; 16 of preamble and 4.25 more:
     * 48.25 symbols, 395.264 ms. 4/8: 8 + 32 = 40 symbols, 60.25 in all, 493.568 ms. */
    TEST_ASSERT_EQUAL_UINT32(395264U, mesh_drv_lora_packet_airtime_us(11U, MESH_DRV_LORA_BANDWIDTH_250,
                                                                      MESH_DRV_LORA_CODING_4_5, 16U, 20U, false));
    TEST_ASSERT_EQUAL_UINT32(493568U, mesh_drv_lora_packet_airtime_us(11U, MESH_DRV_LORA_BANDWIDTH_250,
                                                                      MESH_DRV_LORA_CODING_4_8, 16U, 20U, false));
    /* SF5, BW500: a symbol is 64 us. 10 bytes: 80 - 20 + 36 = 96 bits, 20 bits a
     * group, 5 groups. 4/5: 8 + 25 = 33; 8 of preamble and 6.25 more: 47.25 symbols. */
    TEST_ASSERT_EQUAL_UINT32(3024U, mesh_drv_lora_packet_airtime_us(5U, MESH_DRV_LORA_BANDWIDTH_500,
                                                                    MESH_DRV_LORA_CODING_4_5, 8U, 10U, false));
}

TEST_CASE("airtime: the short call is the long one at 4/5 and eight symbols", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT32(mesh_drv_lora_packet_airtime_us(9U, MESH_DRV_LORA_BANDWIDTH_125, MESH_DRV_LORA_CODING_4_5,
                                                             8U, 32U, false),
                             mesh_drv_lora_airtime_us(9U, MESH_DRV_LORA_BANDWIDTH_125, 32U, false));
}

TEST_CASE("airtime: out of range is 0", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(4U, 0x04U, 1U, 8U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(13U, 0x04U, 1U, 8U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(7U, 0x08U, 1U, 8U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(7U, 0x04U, 0U, 8U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(7U, 0x04U, 5U, 8U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(7U, 0x04U, 1U, 0U, 10U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_packet_airtime_us(7U, 0x04U, 1U, 8U, 0U, false));
}

TEST_CASE("cad: the thresholds are table 6-19's", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT8(60U, mesh_drv_lora_cad_peak(5U, 1U));
    TEST_ASSERT_EQUAL_UINT8(74U, mesh_drv_lora_cad_peak(12U, 1U));
    TEST_ASSERT_EQUAL_UINT8(56U, mesh_drv_lora_cad_peak(7U, 2U));
    TEST_ASSERT_EQUAL_UINT8(60U, mesh_drv_lora_cad_peak(11U, 3U));
    TEST_ASSERT_EQUAL_UINT8(64U, mesh_drv_lora_cad_peak(12U, 4U));
    TEST_ASSERT_EQUAL_UINT8(55U, mesh_drv_lora_cad_peak(4U, 2U));    /* the chip's default */
    TEST_ASSERT_EQUAL_UINT8(55U, mesh_drv_lora_cad_peak(7U, 5U));
}

TEST_CASE("cad: it lasts its symbols", "[lora]")
{
    TEST_ASSERT_EQUAL_UINT32(16384U, mesh_drv_lora_cad_us(11U, MESH_DRV_LORA_BANDWIDTH_250, 2U));
    TEST_ASSERT_EQUAL_UINT32(2048U, mesh_drv_lora_cad_us(7U, MESH_DRV_LORA_BANDWIDTH_125, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_cad_us(7U, 0x08U, 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, mesh_drv_lora_symbols_us(13U, MESH_DRV_LORA_BANDWIDTH_125, 2U));
}
