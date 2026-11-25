/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_nmea.h
 * @brief The GNSS's sentences, read into a fix. Plain C, no hardware.
 *
 * The ME31GR02 streams NMEA: a line that starts with '$', a talker and a
 * sentence name, comma-separated fields, and a '*' with a two-hex checksum.
 * This reads that stream a byte at a time and keeps the latest fix - where,
 * how high, how many satellites, the UTC time and date - from the two
 * sentences that carry it, GGA and RMC. GSA's fix type and dilutions and GSV's
 * satellite-in-view count are also kept; every other sentence (GST, the $POL
 * proprietary ones) is counted and dropped.
 *
 * It computes from what it is given and nothing else: no clock, no pins, no
 * waiting. The owner feeds it the UART's bytes; the facade reads the fix. It
 * is built on the host and run as it is (tools/check_layers.sh, rule 4).
 *
 * The checksum is mandatory: a sentence whose XOR does not match is dropped
 * whole, never half-parsed. Numbers are read digit by digit, so a truncated
 * or malformed field leaves its part of the fix untouched rather than taking
 * a locale's idea of what "48.1,,3" means.
 */
#ifndef MESH_DRV_NMEA_H_
#define MESH_DRV_NMEA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest NMEA line the parser keeps; longer ones are dropped. */
#define MESH_DRV_NMEA_LINE_MAX   100U

/** @brief A position fix, as far as the two sentences carry it.
 *
 * lat and lon are decimal degrees, north and east positive. They, the
 * altitude and the ground speed are valid only when `fix` is true. The times
 * are what the sentence held, zero until one has: utc is hhmmss as an integer
 * (123519 is 12:35:19), date is ddmmyy. The satellite counts are the last the
 * stream gave and stand with or without a fix. */
typedef struct {
    bool     fix;          /**< A position was reported (GGA quality > 0 or
                            *   RMC status 'A').                              */
    double   lat;          /**< Degrees north, valid with `fix`.             */
    double   lon;          /**< Degrees east, valid with `fix`.              */
    float    alt_m;        /**< Metres above the geoid (GGA), with `fix`.    */
    float    speed_kn;     /**< Ground speed in knots (RMC), with `fix`.     */
    float    hdop;         /**< Horizontal dilution of precision (GGA).      */
    uint8_t  sats_used;    /**< Satellites in the fix (GGA).                 */
    uint8_t  sats_in_view; /**< Satellites the last GSV reported in view.    */
    uint32_t utc;          /**< Time of day, hhmmss; 0 until one is read.    */
    uint32_t date;         /**< Date, ddmmyy (RMC); 0 until one is read.     */
    uint8_t  fix_type;     /**< GSA: 1 none, 2 is 2D, 3 is 3D; 0 until one is read. */
    uint8_t  quality;      /**< GGA: 0 none, 1 GNSS, 2 differential and so on; 0
                            *   until one is read.                           */
    float    pdop;         /**< Position dilution of precision (GSA).        */
    float    vdop;         /**< Vertical dilution of precision (GSA).        */
    float    course_deg;   /**< Course over the ground, true north (RMC), with
                            *   `fix`.                                       */
    float    geoid_separation_m; /**< The geoid over the WGS84 ellipsoid (GGA),
                            *   with `fix`: alt_m plus this is the height
                            *   over the ellipsoid.                          */
    uint32_t reports;      /**< Sentences that reported a position: when it
                            *   moves, the position is new, and the owner has
                            *   the clock this parser has not. 0 until one.  */
} mesh_drv_nmea_fix_t;

/** @brief The parser's state. Zeroed by mesh_drv_nmea_reset(); the caller owns it. */
typedef struct {
    char      line[MESH_DRV_NMEA_LINE_MAX];
    uint8_t   len;
    bool      overflow;    /**< The current line passed the buffer; drop it.  */
    bool      started;     /**< A '$' has been seen for this line.           */
    mesh_drv_nmea_fix_t fix;
    uint32_t  good;        /**< Sentences whose checksum held.               */
    uint32_t  bad;         /**< Sentences with a bad or missing checksum.    */
} mesh_drv_nmea_t;

/** @brief Start over: no line in hand, counts at zero, the fix empty. */
void mesh_drv_nmea_reset(mesh_drv_nmea_t *n);

/**
 * @brief Feed one byte of the stream.
 *
 * A '\r' or '\n' ends the line, which is then checked and, if it holds,
 * parsed into the fix. Anything before the first '$' of a line is skipped.
 *
 * @return true when this byte completed a sentence whose checksum held,
 *         whether or not it was one the parser understands.
 */
bool mesh_drv_nmea_push(mesh_drv_nmea_t *n, char c);

/** @brief Copy the latest fix. */
void mesh_drv_nmea_read(const mesh_drv_nmea_t *n, mesh_drv_nmea_fix_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MESH_DRV_NMEA_H_ */
