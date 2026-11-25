/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_lora.c
 * @brief LoRa's arithmetic. See mesh_drv_lora.h.
 *
 * The airtime is Semtech's formula, cross-checked to the microsecond once the
 * low data-rate option was accounted for.
 */
#include "mesh_drv_lora.h"

#include <stddef.h>

/* A transmit's guard past its airtime: a quarter of it, never under 20 ms. */
#define SEND_GUARD_MIN_US   20000U

/* Table 9-3 of the datasheet, the actual bandwidths, whole hertz. */
static const struct {
    uint8_t  code;
    uint32_t hz;
} BANDWIDTHS[] = {
    { MESH_DRV_LORA_BANDWIDTH_31,   31250U   },
    { MESH_DRV_LORA_BANDWIDTH_41,   41667U   },
    { MESH_DRV_LORA_BANDWIDTH_62,   62500U   },
    { MESH_DRV_LORA_BANDWIDTH_83,   83333U   },
    { MESH_DRV_LORA_BANDWIDTH_101,  101563U  },
    { MESH_DRV_LORA_BANDWIDTH_125,  125000U  },
    { MESH_DRV_LORA_BANDWIDTH_203,  203125U  },
    { MESH_DRV_LORA_BANDWIDTH_250,  250000U  },
    { MESH_DRV_LORA_BANDWIDTH_406,  406250U  },
    { MESH_DRV_LORA_BANDWIDTH_500,  500000U  },
    { MESH_DRV_LORA_BANDWIDTH_812,  812500U  },
    { MESH_DRV_LORA_BANDWIDTH_1000, 1000000U },
};

uint32_t mesh_drv_lora_bandwidth_hz(uint8_t bandwidth)
{
    for (size_t i = 0U; i < (sizeof(BANDWIDTHS) / sizeof(BANDWIDTHS[0])); i++) {
        if (BANDWIDTHS[i].code == bandwidth) {
            return BANDWIDTHS[i].hz;
        }
    }
    return 0U;
}

uint8_t mesh_drv_lora_bandwidth_code(uint32_t hz)
{
    for (size_t i = 0U; i < (sizeof(BANDWIDTHS) / sizeof(BANDWIDTHS[0])); i++) {
        uint32_t const actual = BANDWIDTHS[i].hz;
        uint32_t const off    = (hz > actual) ? (hz - actual) : (actual - hz);
        if (((uint64_t)off * 200U) <= (uint64_t)actual) {
            return BANDWIDTHS[i].code;
        }
    }
    return 0U;
}

bool mesh_drv_lora_low_data_rate(uint8_t spreading_factor, uint8_t bandwidth)
{
    uint32_t hz = mesh_drv_lora_bandwidth_hz(bandwidth);

    if (spreading_factor > 12U) {
        return false;   /* only SF7..12 are used; keeps the shift below defined */
    }
    /* The SX1280's bandwidths take it at SF11 and SF12 however short the
     * symbol (9.9.1). RadioLib does the same at 2.4 GHz, so every Meshtastic
     * node there has it on. Without it the header still decodes and the
     * payload fails its CRC. */
    if ((spreading_factor >= 11U)
        && ((bandwidth == MESH_DRV_LORA_BANDWIDTH_203) || (bandwidth == MESH_DRV_LORA_BANDWIDTH_406)
            || (bandwidth == MESH_DRV_LORA_BANDWIDTH_812))) {
        return true;
    }
    if (hz == 0U) {
        hz = 500000U;
    }
    /* Elsewhere the symbol time decides, 2^sf / bandwidth: on past 16 ms. */
    uint64_t const symbol_us = ((uint64_t)1000000U << spreading_factor) / (uint64_t)hz;
    return symbol_us > 16000U;
}

uint8_t mesh_drv_lora_cad_symbols(uint8_t spreading_factor)
{
    return (spreading_factor >= 12U) ? 3U : 4U;
}

uint8_t mesh_drv_lora_cad_peak(uint8_t spreading_factor, uint8_t symbols)
{
    /* Table 6-19: by number of symbols, 1..4, and SF5..SF12. */
    static const uint8_t PEAK[4][8] = {
        { 60U, 60U, 60U, 64U, 64U, 66U, 70U, 74U },
        { 56U, 56U, 56U, 58U, 58U, 60U, 64U, 68U },
        { 51U, 51U, 52U, 54U, 56U, 60U, 60U, 65U },
        { 51U, 51U, 51U, 54U, 56U, 60U, 60U, 64U },
    };

    if ((spreading_factor < 5U) || (spreading_factor > 12U) || (symbols < 1U) || (symbols > 4U)) {
        return 55U;
    }
    return PEAK[symbols - 1U][spreading_factor - 5U];
}

uint32_t mesh_drv_lora_symbols_us(uint8_t spreading_factor, uint8_t bandwidth, uint32_t symbols)
{
    uint32_t const hz = mesh_drv_lora_bandwidth_hz(bandwidth);

    if ((hz == 0U) || (spreading_factor < 5U) || (spreading_factor > 12U)) {
        return 0U;
    }
    uint64_t const us = (((uint64_t)1000000U << spreading_factor) * (uint64_t)symbols) / (uint64_t)hz;

    return (us > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

uint32_t mesh_drv_lora_cad_us(uint8_t spreading_factor, uint8_t bandwidth, uint8_t symbols)
{
    return mesh_drv_lora_symbols_us(spreading_factor, bandwidth, symbols);
}

/* ---- The chip's own timers -------------------------------------------------- */

uint32_t mesh_drv_lora_timer_ticks(uint32_t us)
{
    /* us x 32768 / 1e6 = us x 4096 / 125000, rounded up, in 64 bits. */
    uint64_t const ticks = (((uint64_t)us * 4096U) + 124999U) / 125000U;

    if (ticks < 1U) {
        return 1U;   /* 0 would switch the chip's timer off */
    }
    if (ticks > (uint64_t)MESH_DRV_LORA_TIMER_MAX) {
        return MESH_DRV_LORA_TIMER_MAX;
    }
    return (uint32_t)ticks;
}

uint32_t mesh_drv_lora_send_timeout_ticks(uint32_t airtime_us)
{
    uint64_t guard = (uint64_t)airtime_us / 4U;

    if (guard < (uint64_t)SEND_GUARD_MIN_US) {
        guard = (uint64_t)SEND_GUARD_MIN_US;
    }
    uint64_t const total = (uint64_t)airtime_us + guard;
    /* Past what 32 bits of microseconds hold the timer is at its 512 s cap
     * anyway. */
    return mesh_drv_lora_timer_ticks((total > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)total);
}

uint16_t mesh_drv_lora_preamble_symbols(uint8_t spreading_factor, uint8_t bandwidth, uint32_t ms)
{
    uint32_t const hz = mesh_drv_lora_bandwidth_hz(bandwidth);

    if (hz == 0U) {
        return 0U;
    }
    if ((spreading_factor < 5U) || (spreading_factor > 12U)) {
        return 0U;
    }
    /* One symbol lasts 2^sf / bandwidth: 1e9 << sf / hz nanoseconds. The
     * count rounds up and keeps one symbol over, so the preamble outlasts the
     * request rather than ending a hair before it. */
    uint64_t const symbol_ns = ((uint64_t)1000000000U << spreading_factor) / (uint64_t)hz;
    uint64_t const symbols   = ((((uint64_t)ms * 1000000U) + symbol_ns - 1U) / symbol_ns) + 1U;

    if (symbols < (uint64_t)MESH_DRV_LORA_PREAMBLE_MIN) {
        return (uint16_t)MESH_DRV_LORA_PREAMBLE_MIN;
    }
    if (symbols > (uint64_t)MESH_DRV_LORA_PREAMBLE_MAX) {
        return (uint16_t)MESH_DRV_LORA_PREAMBLE_MAX;
    }
    return (uint16_t)symbols;
}

uint32_t mesh_drv_lora_packet_airtime_us(uint8_t spreading_factor, uint8_t bandwidth,
                                         uint8_t coding_rate, uint16_t preamble, uint8_t length,
                                         bool low_data_rate)
{
    uint32_t const hz = mesh_drv_lora_bandwidth_hz(bandwidth);

    if ((hz == 0U) || (spreading_factor < 5U) || (spreading_factor > 12U) || (length == 0U)
        || (coding_rate < MESH_DRV_LORA_CODING_4_5) || (coding_rate > MESH_DRV_LORA_CODING_4_8)
        || (preamble == 0U)) {
        return 0U;
    }
    /* Under SF7 the header costs 20 bits with no 8-bit head start, and the
     * preamble's tail is 6.25 symbols instead of 4.25 (quarter symbols below). */
    bool const     small = spreading_factor < 7U;
    int32_t const  head  = small ? 36 : 44;   /* the CRC's 16 and the header's 20, plus 8 from SF7 */
    uint32_t const tail  = small ? 25U : 17U;
    uint32_t const coded = low_data_rate ? ((uint32_t)spreading_factor - 2U) : (uint32_t)spreading_factor;
    int32_t const  raw   = (8 * (int32_t)length) - (4 * (int32_t)spreading_factor) + head;
    uint32_t const bits  = (raw > 0) ? (uint32_t)raw : 0U;
    uint32_t const groups = (bits + (4U * coded) - 1U) / (4U * coded);
    /* 8 fixed symbols, the groups at 4 + CR symbols a group, the preamble and its
     * tail, counted in quarter symbols. */
    uint64_t const quarters = (4U * (8ULL + ((4ULL + (uint64_t)coding_rate) * (uint64_t)groups)
                                     + (uint64_t)preamble))
                              + (uint64_t)tail;
    uint64_t const symbol_ns = ((uint64_t)1000000000U << spreading_factor) / (uint64_t)hz;
    uint64_t const us = (symbol_ns * quarters) / 4000U;

    return (us > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)us;
}

uint32_t mesh_drv_lora_airtime_us(uint8_t spreading_factor, uint8_t bandwidth, uint8_t length,
                                  bool low_data_rate)
{
    return mesh_drv_lora_packet_airtime_us(spreading_factor, bandwidth, MESH_DRV_LORA_CODING_4_5,
                                           MESH_DRV_LORA_PREAMBLE_MIN, length, low_data_rate);
}
