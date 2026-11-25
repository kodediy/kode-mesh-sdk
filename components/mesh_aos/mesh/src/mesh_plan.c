/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_plan.c
 * @brief The owner's rules. See mesh_plan.h.
 *
 * The power an app asks for is walked onto the chip's tx_power in half-dB
 * steps from the amplifier row's calibrated point, never above the region's cap
 * unless the build sets CONFIG_MESH_LAB, and, while the antennas are not
 * confirmed, never above the safe power whatever the build
 * (mesh_plan_power_allowed()). The duty cycle is a token bucket.
 */
#include "mesh_plan.h"

#include <stddef.h>

#include "mesh_bsp.h"
#include "mesh_region.h"

/* The bottom of the chip's tx_power ranges, half-dB steps (the LR2021's datasheet). */
#define TX_POWER_SUB_GHZ_MIN   (-19)
#define TX_POWER_2_4_GHZ_MIN   (-39)
/* One tx_power step is half a dB, i.e. 50 hundredths of a dBm. */
#define STEP_CDBM   50
/* A window around the cap that the requested power is held to before any
 * arithmetic: wide enough to swallow the chip's whole range (256 half-dB
 * steps, 128 dB), narrow enough that cap - want cannot overflow int32 on a
 * wild request. The clamp to [minimum, maximum] does the real bounding. */
#define POWER_WINDOW_CDBM   (STEP_CDBM * 256)

/* One amplifier row: a power, and the setting the datasheet gives for it. */
typedef struct {
    int32_t cdbm;
    int8_t  tx_power;
    uint8_t duty;
    uint8_t slices;
} power_row_t;

/* The tables' rows above the region's row, tx_power as the register holds it (two
 * for each of the tables' dBm). They are used only with MESH_REGION_LAB_PA_TABLE.
 * Under 19 dBm on the LF path and 10 dBm on the HF path the region's row itself is
 * in the table at the same scale - 18 dBm is 38/5/7, 19 dBm is 40/5/7, 10 dBm is
 * 24/30 - so that row is what is walked. Above, its row stops being one.
 * Table 7-19 is at 915 MHz and 7-21 at 2445 MHz, on Semtech's reference design. */
static const power_row_t TOP_SUB_GHZ[] = {
    { 1950, 42, 4U, 7U },
    { 2000, 42, 5U, 6U },
    { 2050, 42, 6U, 7U },
    { 2100, 44, 5U, 6U },
    { 2150, 44, 6U, 7U },
    { 2200, 44, 7U, 7U },
};
static const power_row_t TOP_2_4_GHZ[] = {
    { 1050, 22, 21U, 0U },
    { 1100, 24, 27U, 0U },
    { 1150, 24, 21U, 0U },
    { 1200, 24, 16U, 0U },
};
#define TOP_SUB_GHZ_COUNT   (sizeof(TOP_SUB_GHZ) / sizeof(TOP_SUB_GHZ[0]))
#define TOP_2_4_GHZ_COUNT   (sizeof(TOP_2_4_GHZ) / sizeof(TOP_2_4_GHZ[0]))
/* How far the region's rows are walked: their last tx_power in a table. */
#define WALK_SUB_GHZ_MAX    40
#define WALK_2_4_GHZ_MAX    24

/* The lowest spreading factor either door takes: the region's 7, or the chip's 5 in a LAB build. */
#define SF_LOWEST   (MESH_REGION_LAB ? 5U : 7U)

static int clamp_int(int v, int low, int high)
{
    if (v < low) {
        return low;
    }
    if (v > high) {
        return high;
    }
    return v;
}

mesh_plan_band_t mesh_plan_band_of(uint32_t hz)
{
    return (hz >= MESH_BSP_2_4_GHZ_FROM_HZ) ? MESH_PLAN_BAND_2_4_GHZ : MESH_PLAN_BAND_SUB_GHZ;
}

bool mesh_plan_frequency_allowed(mesh_plan_band_t band, uint32_t hz)
{
    if (band == MESH_PLAN_BAND_SUB_GHZ) {
        if (MESH_REGION_LAB) {
            return (hz >= MESH_BSP_SUB_GHZ_FROM_HZ) && (hz <= MESH_BSP_SUB_GHZ_TO_HZ);
        }
        return (hz >= MESH_REGION_SUB_GHZ_LOW_HZ) && (hz <= MESH_REGION_SUB_GHZ_HIGH_HZ);
    }
    if (band == MESH_PLAN_BAND_2_4_GHZ) {
        if (MESH_REGION_LAB) {
            return (hz >= MESH_BSP_2_4_GHZ_FROM_HZ) && (hz <= MESH_BSP_2_4_GHZ_TO_HZ);
        }
        return (hz >= MESH_REGION_2_4_GHZ_LOW_HZ) && (hz <= MESH_REGION_2_4_GHZ_HIGH_HZ);
    }
    return false;
}

bool mesh_plan_preamble_allowed(uint16_t preamble)
{
    return (preamble != 0U) && (MESH_REGION_LAB || (preamble == MESH_REGION_PREAMBLE));
}

bool mesh_plan_test_allowed(void)
{
    return MESH_REGION_LAB != 0;
}

bool mesh_plan_duty_counted(mesh_plan_band_t band, bool configured)
{
    if (band != MESH_PLAN_BAND_SUB_GHZ) {
        return false;
    }
    return !(MESH_REGION_LAB && configured);
}

/** @brief The amplifier and tx_power for @p power_cdbm on @p band. */
static void plan_power(mesh_plan_band_t band, int32_t power_cdbm, mesh_drv_lora_settings_t *out)
{
    bool const         low      = (band == MESH_PLAN_BAND_SUB_GHZ);
    int32_t const      cap      = low ? MESH_REGION_SUB_GHZ_CDBM : MESH_REGION_2_4_GHZ_CDBM;
    int const          base     = low ? MESH_REGION_SUB_GHZ_TX_POWER : MESH_REGION_2_4_GHZ_TX_POWER;
    int const          minimum  = low ? TX_POWER_SUB_GHZ_MIN : TX_POWER_2_4_GHZ_MIN;
    int const          maximum  = low ? WALK_SUB_GHZ_MAX : WALK_2_4_GHZ_MAX;
    const power_row_t *top      = low ? TOP_SUB_GHZ : TOP_2_4_GHZ;
    size_t const       top_rows = low ? TOP_SUB_GHZ_COUNT : TOP_2_4_GHZ_COUNT;

    out->amplifier_duty   = low ? MESH_REGION_SUB_GHZ_AMPLIFIER_DUTY : MESH_REGION_2_4_GHZ_AMPLIFIER_DUTY;
    out->amplifier_slices = low ? MESH_REGION_SUB_GHZ_AMPLIFIER_SLICES : 0U;

    /* The power wanted: what was asked, held first to a sane window around the
     * cap (so cap - want cannot overflow int32 on a wild value), then to the
     * cap unless this build lifts it. tx_power walks from the row's calibrated
     * point, one step a half-dB, rounded so the result is at or below what was
     * asked and never above the cap. */
    int32_t want = power_cdbm;
    if (want > (cap + POWER_WINDOW_CDBM)) {
        want = cap + POWER_WINDOW_CDBM;
    }
    if (want < (cap - POWER_WINDOW_CDBM)) {
        want = cap - POWER_WINDOW_CDBM;
    }
    if (!MESH_REGION_LAB && (want > cap)) {
        want = cap;
    }
    /* A LAB build without the sheet's rows stops at the top of the default row
     * (19 dBm LF, 10 dBm HF). */
    int32_t const ladder_top = cap + ((int32_t)(maximum - base) * STEP_CDBM);
    if (!MESH_REGION_LAB_PA_TABLE && (want > ladder_top)) {
        want = ladder_top;
    }
    int32_t const difference = cap - want;
    int32_t const steps      = (difference >= 0)
                                   ? ((difference + (STEP_CDBM - 1)) / STEP_CDBM)
                                   : -((-difference) / STEP_CDBM);
    out->tx_power = (int8_t)clamp_int(base - (int)steps, minimum, maximum);

    /* Past where the region's row is in the datasheet's table, the table's. */
    for (size_t i = 0U; MESH_REGION_LAB_PA_TABLE && (i < top_rows); i++) {
        if (want >= top[i].cdbm) {
            out->tx_power         = top[i].tx_power;
            out->amplifier_duty   = top[i].duty;
            out->amplifier_slices = top[i].slices;
        }
    }
}

int32_t mesh_plan_power_allowed(int32_t power_cdbm, bool antennas_confirmed)
{
    /* Before the region's cap and the LAB ladder: the safe power holds every build. */
    if (!antennas_confirmed && (power_cdbm > MESH_REGION_SAFE_CDBM)) {
        return MESH_REGION_SAFE_CDBM;
    }
    return power_cdbm;
}

bool mesh_plan_power(mesh_plan_band_t band, int32_t power_cdbm, mesh_drv_lora_settings_t *settings)
{
    if ((settings == NULL) || (band >= MESH_PLAN_BAND_COUNT)) {
        return false;
    }
    plan_power(band, power_cdbm, settings);
    return true;
}

bool mesh_plan_transmit_settings(mesh_plan_band_t band, uint8_t spreading_factor,
                                 int32_t power_cdbm, mesh_drv_lora_settings_t *out)
{
    if ((out == NULL) || (band >= MESH_PLAN_BAND_COUNT)) {
        return false;
    }
    /* The same criterion as mesh_plan_radio_settings(): outside it is refused, not folded in. */
    if ((spreading_factor < SF_LOWEST) || (spreading_factor > 12U)) {
        return false;
    }

    if (band == MESH_PLAN_BAND_SUB_GHZ) {
        out->frequency_hz = MESH_REGION_SUB_GHZ_CENTRE_HZ;
        out->band_2_4_ghz = false;
        out->bandwidth    = MESH_REGION_SUB_GHZ_BANDWIDTH;
    } else {
        out->frequency_hz = MESH_REGION_2_4_GHZ_CENTRE_HZ;
        out->band_2_4_ghz = true;
        out->bandwidth    = MESH_REGION_2_4_GHZ_BANDWIDTH;
    }
    plan_power(band, power_cdbm, out);
    out->coding_rate      = (uint8_t)(MESH_REGION_CODING_RATE - 4U);
    out->preamble         = MESH_REGION_PREAMBLE;
    out->sync_word        = MESH_REGION_SYNC_WORD;
    out->spreading_factor = spreading_factor;
    out->low_data_rate    = mesh_drv_lora_low_data_rate(spreading_factor, out->bandwidth);
    return true;
}

/** @brief Whether the region's setting allows @p ask: its shape, and inside its edges. */
static bool plan_region_allows(const mesh_drv_lora_config_t *ask, mesh_plan_band_t band)
{
    bool const     low    = (band == MESH_PLAN_BAND_SUB_GHZ);
    uint32_t const width  = low ? MESH_REGION_SUB_GHZ_BANDWIDTH_HZ : MESH_REGION_2_4_GHZ_BANDWIDTH_HZ;
    uint32_t const bottom = low ? MESH_REGION_SUB_GHZ_LOW_HZ : MESH_REGION_2_4_GHZ_LOW_HZ;
    uint32_t const top    = low ? MESH_REGION_SUB_GHZ_HIGH_HZ : MESH_REGION_2_4_GHZ_HIGH_HZ;

    if (mesh_drv_lora_bandwidth_code(ask->bandwidth_hz) != mesh_drv_lora_bandwidth_code(width)) {
        return false;
    }
    if (((ask->frequency_hz - (width / 2U)) < bottom) || ((ask->frequency_hz + (width / 2U)) > top)) {
        return false;
    }
    return (ask->spreading_factor >= SF_LOWEST) && (ask->coding_rate == MESH_REGION_CODING_RATE)
           && (ask->preamble == MESH_REGION_PREAMBLE) && (ask->sync_word == MESH_REGION_SYNC_WORD);
}

int32_t mesh_plan_power_applied(mesh_plan_band_t band, const mesh_drv_lora_settings_t *settings)
{
    bool const low = (band == MESH_PLAN_BAND_SUB_GHZ);

    if (settings == NULL) {
        return 0;
    }
    if (MESH_REGION_LAB_PA_TABLE) {
        const power_row_t *const top  = low ? TOP_SUB_GHZ : TOP_2_4_GHZ;
        size_t const             rows = low ? TOP_SUB_GHZ_COUNT : TOP_2_4_GHZ_COUNT;
        for (size_t i = 0U; i < rows; i++) {
            if ((top[i].tx_power == settings->tx_power) && (top[i].duty == settings->amplifier_duty)
                && (top[i].slices == settings->amplifier_slices)) {
                return top[i].cdbm;
            }
        }
    }
    int32_t const cap  = low ? MESH_REGION_SUB_GHZ_CDBM : MESH_REGION_2_4_GHZ_CDBM;
    int const     base = low ? MESH_REGION_SUB_GHZ_TX_POWER : MESH_REGION_2_4_GHZ_TX_POWER;
    return cap + (((int32_t)settings->tx_power - (int32_t)base) * STEP_CDBM);
}

bool mesh_plan_radio_settings(const mesh_drv_lora_config_t *ask, mesh_drv_lora_settings_t *out)
{
    if ((ask == NULL) || (out == NULL)) {
        return false;
    }
    mesh_plan_band_t const band = mesh_plan_band_of(ask->frequency_hz);
    uint8_t const          code = mesh_drv_lora_bandwidth_code(ask->bandwidth_hz);

    if (!mesh_plan_frequency_allowed(band, ask->frequency_hz) || (code == 0U)
        || (ask->spreading_factor < 5U) || (ask->spreading_factor > 12U)
        || (ask->coding_rate < 5U) || (ask->coding_rate > 8U) || (ask->preamble == 0U)) {
        return false;
    }
    if (!MESH_REGION_LAB && !plan_region_allows(ask, band)) {
        return false;
    }
    out->frequency_hz     = ask->frequency_hz;
    out->band_2_4_ghz     = (band == MESH_PLAN_BAND_2_4_GHZ);
    out->spreading_factor = ask->spreading_factor;
    out->bandwidth        = code;
    out->coding_rate      = (uint8_t)(ask->coding_rate - 4U);
    out->preamble         = ask->preamble;
    out->sync_word        = ask->sync_word;
    out->low_data_rate    = mesh_drv_lora_low_data_rate(ask->spreading_factor, code);
    plan_power(band, ask->power_cdbm, out);
    return true;
}

/* ---- The duty cycle -------------------------------------------------------- */

void mesh_plan_duty_init(mesh_plan_duty_t *duty, uint32_t ppt)
{
    duty->ppt       = ppt;
    duty->cap_us    = (uint64_t)3600000U * (uint64_t)ppt;   /* one hour's airtime */
    duty->budget_us = duty->cap_us;                         /* starts full        */
    duty->last_ms   = 0U;
    duty->started   = false;
}

/** @brief Add the tokens that accrued since last_ms, up to the cap. */
static void duty_refill(mesh_plan_duty_t *duty, uint32_t now_ms)
{
    if (!duty->started) {
        duty->last_ms = now_ms;
        duty->started = true;
        return;
    }
    uint32_t const elapsed = now_ms - duty->last_ms;   /* wrap-safe for uint32 */
    duty->last_ms = now_ms;
    /* ppt parts per thousand of real time: ppt microseconds of airtime a
     * millisecond. */
    duty->budget_us += (uint64_t)elapsed * (uint64_t)duty->ppt;
    if (duty->budget_us > duty->cap_us) {
        duty->budget_us = duty->cap_us;
    }
}

bool mesh_plan_duty_allows(mesh_plan_duty_t *duty, uint32_t airtime_us, uint32_t now_ms)
{
    if (duty->ppt == 0U) {
        return true;   /* no duty rule on this band */
    }
    duty_refill(duty, now_ms);
    return duty->budget_us >= (uint64_t)airtime_us;
}

void mesh_plan_duty_charge(mesh_plan_duty_t *duty, uint32_t airtime_us, uint32_t now_ms)
{
    if (duty->ppt == 0U) {
        return;
    }
    duty_refill(duty, now_ms);
    if (duty->budget_us >= (uint64_t)airtime_us) {
        duty->budget_us -= (uint64_t)airtime_us;
    } else {
        duty->budget_us = 0U;
    }
}

uint32_t mesh_plan_duty_wait_ms(const mesh_plan_duty_t *duty, uint32_t airtime_us)
{
    if ((duty->ppt == 0U) || (duty->budget_us >= (uint64_t)airtime_us)) {
        return 0U;
    }
    uint64_t const need = (uint64_t)airtime_us - duty->budget_us;
    /* need microseconds of airtime accrue at ppt a millisecond. */
    return (uint32_t)((need + (uint64_t)duty->ppt - 1U) / (uint64_t)duty->ppt);
}

/* ---- The fingerprint ------------------------------------------------------- */

uint16_t mesh_plan_fingerprint_mask(void)
{
    return (uint16_t)((1U << (MESH_BSP_POSITION_I2C_SCL - 1U))
                      | (1U << (MESH_BSP_POSITION_I2C_SDA - 1U))
                      | (1U << (MESH_BSP_POSITION_LORA_CS - 1U))
                      | (1U << (MESH_BSP_POSITION_LORA_RESET - 1U)));
}

bool mesh_plan_fingerprint_matches(uint16_t level_bits)
{
    uint16_t const mask = mesh_plan_fingerprint_mask();
    return (level_bits & mask) == mask;
}
