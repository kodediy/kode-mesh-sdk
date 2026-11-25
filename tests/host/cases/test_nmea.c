/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_nmea.c
 * @brief mesh_drv_nmea: the GNSS's sentences read into a fix, without a receiver.
 *
 * The classic GGA/RMC/GSV examples parse to their known fix; a bad checksum
 * is dropped whole; the hemisphere signs and the no-fix states come through.
 * Fed a byte at a time, the way the owner feeds it the UART.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "mesh_drv_nmea.h"

/** @brief Push a sentence and its line ending; return whether it completed
 *  with a good checksum. */
static bool feed(mesh_drv_nmea_t *n, const char *s)
{
    bool done = false;
    for (const char *p = s; *p; p++) {
        if (mesh_drv_nmea_push(n, *p)) { done = true; }
    }
    if (mesh_drv_nmea_push(n, '\r')) { done = true; }
    if (mesh_drv_nmea_push(n, '\n')) { done = true; }
    return done;
}

/* ---- GGA: where, how high, how many ------------------------------------------ */

TEST_CASE("nmea: the classic GGA parses to its known fix", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));

    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(1U, n.good);
    TEST_ASSERT_EQUAL_UINT32(0U, n.bad);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 48.1173, f.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 11.5167, f.lon);
    TEST_ASSERT_EQUAL_UINT8(8U, f.sats_used);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 545.4f, f.alt_m);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, f.hdop);
    TEST_ASSERT_EQUAL_UINT32(123519U, f.utc);
}

/* ---- RMC: the time, date and ground speed ------------------------------------ */

TEST_CASE("nmea: the classic RMC adds the date and the speed", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    (void)feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    TEST_ASSERT_TRUE(feed(&n,
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));

    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(2U, n.good);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_EQUAL_UINT32(230394U, f.date);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 22.4f, f.speed_kn);
}

/* ---- The checksum is mandatory ----------------------------------------------- */

TEST_CASE("nmea: a bad checksum is dropped whole and counted bad", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    (void)feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    uint32_t const good_before = n.good;

    /* the same sentence with a wrong checksum: no change to the fix, +1 bad */
    TEST_ASSERT_FALSE(feed(&n,
        "$GPGGA,999999,0000.000,N,00000.000,E,1,00,9.9,0.0,M,46.9,M,,*00"));
    TEST_ASSERT_EQUAL_UINT32(good_before, n.good);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, n.bad);

    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(123519U, f.utc);   /* still the last good one */
}

TEST_CASE("nmea: a sentence with no checksum is not good", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_FALSE(feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
    TEST_ASSERT_EQUAL_UINT32(0U, n.good);
}

/* ---- Satellites in view ------------------------------------------------------ */

TEST_CASE("nmea: GSV carries the satellites in view", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT8(11U, f.sats_in_view);
}

/* ---- Hemisphere signs -------------------------------------------------------- */

TEST_CASE("nmea: south and west come through negative", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,010203,4807.038,S,01131.000,W,1,06,1.2,100.0,M,46.9,M,,*40"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -48.1173, f.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -11.5167, f.lon);
}

/* ---- The no-fix states ------------------------------------------------------- */

TEST_CASE("nmea: an RMC with status V is not a fix", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPRMC,235959,V,,,,,,,310197,,*3D"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_FALSE(f.fix);
    TEST_ASSERT_EQUAL_UINT32(310197U, f.date);   /* the date still reads */
}

TEST_CASE("nmea: a GGA with quality 0 is not a fix", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,0,00,99.9,,M,,M,,*45"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_FALSE(f.fix);
}

TEST_CASE("nmea: an RMC status V clears a fix held from before", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    (void)feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);                       /* a fix is held */
    /* then the receiver loses it: an RMC marked void must clear the fix */
    TEST_ASSERT_TRUE(feed(&n, "$GPRMC,235959,V,,,,,,,310197,,*3D"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_FALSE(f.fix);
}

/* ---- Numbers that are not just a count -------------------------------------- */

TEST_CASE("nmea: a negative altitude comes through, not zeroed", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,-12.3,M,46.9,M,,*5A"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -12.3f, f.alt_m);   /* below sea level, not 0 */
}

TEST_CASE("nmea: an empty field leaves its part of the fix as it was", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    (void)feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    /* the same fix with the time, satellites, HDOP and altitude all blank */
    TEST_ASSERT_TRUE(feed(&n, "$GNGGA,,4807.038,N,01131.000,E,1,,,,M,,M,,*40"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_EQUAL_UINT32(123519U, f.utc);        /* kept, not reset to 0 */
    TEST_ASSERT_EQUAL_UINT8(8U, f.sats_used);        /* kept, not reset to 0 */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, f.hdop);   /* kept, not a false 0.0 "perfect" */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 545.4f, f.alt_m); /* kept */
}

/* ---- Robustness -------------------------------------------------------------- */

TEST_CASE("nmea: noise before the $ is skipped", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    /* a half line of rubbish, a newline, then a good sentence */
    (void)feed(&n, "xyzzy not a sentence");
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_EQUAL_UINT32(1U, n.good);
}

TEST_CASE("nmea: an over-long line is dropped, not parsed", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    char big[400];
    big[0] = '$';
    memset(big + 1, '7', sizeof(big) - 2);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_FALSE(feed(&n, big));        /* far past MESH_DRV_NMEA_LINE_MAX */
    TEST_ASSERT_EQUAL_UINT32(0U, n.good);
    /* and the parser still takes a good sentence afterwards */
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    TEST_ASSERT_EQUAL_UINT32(1U, n.good);
}

/* ---- Defensive corners (surfaced by coverage) -------------------------------- */

TEST_CASE("nmea: a stray character ends a numeric field where it sits", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    /* HDOP "0.9z": the number reads up to the letter and stops there, it is
     * not read past, and the sentence still parses. */
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9z,545.4,M,46.9,M,,*3D"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_TRUE(f.fix);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.9f, f.hdop);
}

TEST_CASE("nmea: a latitude far out of range drops to zero, not an overflow", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    /* 99999.000 is past any real ddmm.mmmm: rejected before the int cast that
     * would otherwise overflow, so the latitude reads 0, not a wild value. */
    TEST_ASSERT_TRUE(feed(&n,
        "$GPGGA,123519,99999.000,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*7E"));
    mesh_drv_nmea_fix_t f;
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lat);
}

TEST_CASE("nmea: a latitude past 90 degrees and a longitude past 180 drop to zero, each on its own", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,9100.000,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4F"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lat);        /* 91 degrees north is nowhere */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 11.5167, f.lon);    /* and the good field beside it stands */

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,4807.038,N,18100.000,E,1,08,0.9,545.4,M,46.9,M,,*4D"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 48.1173, f.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lon);        /* 181 degrees east is nowhere */

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,9000.001,N,18000.001,E,1,08,0.9,545.4,M,46.9,M,,*45"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lat);        /* a thousandth of a minute past the pole */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lon);        /* and past the date line */
}

TEST_CASE("nmea: minutes of 60 or more are not a position; the pole and the date line are", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,4860.000,N,01175.500,E,1,08,0.9,545.4,M,46.9,M,,*48"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lat);        /* 48 degrees and 60 minutes */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 0.0, f.lon);        /* 11 degrees and 75.5 minutes */

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,9000.000,S,18000.000,W,1,08,0.9,545.4,M,46.9,M,,*4A"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -90.0, f.lat);      /* the limits themselves are places */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -180.0, f.lon);
}

TEST_CASE("nmea: a checksum written in lower-case hex is accepted", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    /* the same RMC, its checksum letters in lower case: still a good line */
    TEST_ASSERT_TRUE(feed(&n,
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6a"));
    TEST_ASSERT_EQUAL_UINT32(1U, n.good);
}

TEST_CASE("nmea: a non-hex checksum digit is rejected", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    /* 'Z' is not a hex digit: the checksum cannot be read, the line is dropped */
    TEST_ASSERT_FALSE(feed(&n,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*6Z"));
    TEST_ASSERT_EQUAL_UINT32(0U, n.good);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, n.bad);
}

TEST_CASE("nmea: a line too short to hold a sentence is dropped", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_FALSE(feed(&n, "$GP"));      /* under four characters */
    TEST_ASSERT_EQUAL_UINT32(0U, n.good);
}

/* ---- GSA, the course, the separation and the count of reports --------------------- */

TEST_CASE("nmea: GSA gives the fix type and the dilutions", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT8(3U, f.fix_type);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, f.pdop);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.1f, f.vdop);
    TEST_ASSERT_FALSE(f.fix);                /* it says nothing about where */
    TEST_ASSERT_EQUAL_UINT32(0U, f.reports);
}

TEST_CASE("nmea: a GSA with no fix says 1, and empty dilutions leave what was there", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39"));
    TEST_ASSERT_TRUE(feed(&n, "$GNGSA,A,1,,,,,,,,,,,,,,,*00"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT8(1U, f.fix_type);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, f.pdop);
}

TEST_CASE("nmea: RMC gives the course, and GGA the quality and the geoid's separation", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 84.4f, f.course_deg);
    TEST_ASSERT_EQUAL_UINT8(1U, f.quality);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 46.9f, f.geoid_separation_m);
}

TEST_CASE("nmea: only a sentence with a position counts as a report", "[nmea]")
{
    mesh_drv_nmea_t n;
    mesh_drv_nmea_fix_t f;

    mesh_drv_nmea_reset(&n);
    TEST_ASSERT_TRUE(feed(&n, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(1U, f.reports);
    TEST_ASSERT_TRUE(feed(&n, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(2U, f.reports);
    TEST_ASSERT_TRUE(feed(&n, "$GPGSV,1,1,04,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*7A"));
    mesh_drv_nmea_read(&n, &f);
    TEST_ASSERT_EQUAL_UINT32(2U, f.reports);
}
