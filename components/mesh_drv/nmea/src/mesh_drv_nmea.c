/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mesh_drv_nmea.c
 * @brief The NMEA parser. See mesh_drv_nmea.h.
 *
 * A line is kept between a '$' and its terminator, dropped if it overruns the
 * buffer. On the terminator its checksum is checked - the XOR of everything
 * between '$' and '*' against the two hex after it - and only a line that
 * holds is parsed. The two sentences that carry a fix, GGA and RMC, are read
 * field by field; GSA's fix type and dilutions and GSV's satellite-in-view count
 * are taken; everything else is counted good and dropped. Numbers are read a character at a time, so a short
 * or empty field leaves its part of the fix as it was.
 */
#include "mesh_drv_nmea.h"

#include <string.h>

/** @brief Copy field @p idx (comma-separated) of @p body into @p out. */
static void field(const char *body, uint8_t len, uint8_t idx, char *out,
                  uint8_t outcap)
{
    uint8_t f = 0U;
    uint8_t o = 0U;

    out[0] = '\0';
    for (uint8_t i = 0U; i < len; i++) {
        if (body[i] == ',') {
            if (f == idx) {
                break;
            }
            f++;
            continue;
        }
        if (f == idx) {
            if (o < (outcap - 1U)) {
                out[o] = body[i];
                o++;
            }
        }
    }
    if (f >= idx) {
        out[o] = '\0';
    }
}

/** @brief Read an unsigned integer from @p s, a digit at a time. */
static uint32_t to_uint(const char *s)
{
    uint32_t v = 0U;

    for (uint8_t i = 0U; (s[i] >= '0') && (s[i] <= '9'); i++) {
        v = (v * 10U) + (uint32_t)(s[i] - '0');
    }
    return v;
}

/** @brief Read a fixed-point number from @p s, with an optional leading sign. */
static double to_double(const char *s)
{
    double   v     = 0.0;
    double   scale = 0.0;
    bool     frac  = false;
    bool     neg   = false;
    uint8_t  i     = 0U;

    if ((s[0] == '-') || (s[0] == '+')) {
        neg = (s[0] == '-');   /* altitude can be below sea level */
        i   = 1U;
    }
    for (; s[i] != '\0'; i++) {
        if (s[i] == '.') {
            frac  = true;
            scale = 0.1;
            continue;
        }
        if ((s[i] < '0') || (s[i] > '9')) {
            break;
        }
        double const d = (double)(s[i] - '0');
        if (frac) {
            v     += d * scale;
            scale *= 0.1;
        } else {
            v = (v * 10.0) + d;
        }
    }
    return neg ? -v : v;
}

/**
 * @brief A ddmm.mmmm field and its hemisphere, into decimal degrees.
 *
 * A field that is no position reads 0: minutes of 60 or more, or more than @p max_deg degrees
 * (90 for a latitude, 180 for a longitude). Such a field is a corrupt one that happened to pass
 * the checksum.
 */
static double to_degrees(const char *num, const char *hemi, double max_deg)
{
    double const raw = to_double(num);
    /* A ddmm.mmmm field is at most a few thousand; anything outside that is
     * rejected here, before the int cast below could overflow. */
    if ((raw < 0.0) || (raw > 36000.0)) {
        return 0.0;
    }
    double const deg = (double)((int)(raw / 100.0));
    double const min = raw - (deg * 100.0);
    double       val = deg + (min / 60.0);

    if ((min >= 60.0) || (val > max_deg)) {
        return 0.0;
    }
    if ((hemi[0] == 'S') || (hemi[0] == 'W')) {
        val = -val;
    }
    return val;
}

/** @brief Parse a GGA body into the fix. */
static void parse_gga(const char *body, uint8_t len, mesh_drv_nmea_fix_t *f)
{
    char q[4];
    char lat[16];
    char ns[2];
    char lon[16];
    char ew[2];
    char used[4];
    char hdop[8];
    char alt[12];
    char sep[12];

    field(body, len, 6U, q, sizeof(q));
    field(body, len, 7U, used, sizeof(used));
    field(body, len, 8U, hdop, sizeof(hdop));
    field(body, len, 9U, alt, sizeof(alt));

    /* An empty field leaves its part of the fix as it was (mesh_drv_nmea.h). */
    if (used[0] != '\0') { f->sats_used = (uint8_t)to_uint(used); }
    if (hdop[0] != '\0') { f->hdop      = (float)to_double(hdop); }

    if (q[0] != '\0') { f->quality = (uint8_t)to_uint(q); }

    if (to_uint(q) > 0U) {
        field(body, len, 11U, sep, sizeof(sep));
        field(body, len, 2U, lat, sizeof(lat));
        field(body, len, 3U, ns, sizeof(ns));
        field(body, len, 4U, lon, sizeof(lon));
        field(body, len, 5U, ew, sizeof(ew));
        f->fix = true;
        if (lat[0] != '\0') { f->lat   = to_degrees(lat, ns, 90.0); }
        if (lon[0] != '\0') { f->lon   = to_degrees(lon, ew, 180.0); }
        if (alt[0] != '\0') { f->alt_m = (float)to_double(alt); }
        if (sep[0] != '\0') { f->geoid_separation_m = (float)to_double(sep); }
        f->reports++;
    } else {
        f->fix = false;
    }

    char t[12];
    field(body, len, 1U, t, sizeof(t));
    if (t[0] != '\0') { f->utc = to_uint(t); }
}

/** @brief Parse an RMC body into the fix. */
static void parse_rmc(const char *body, uint8_t len, mesh_drv_nmea_fix_t *f)
{
    char status[2];
    char t[12];
    char d[8];

    field(body, len, 2U, status, sizeof(status));
    field(body, len, 1U, t, sizeof(t));
    field(body, len, 9U, d, sizeof(d));
    /* An empty field leaves its part of the fix as it was (mesh_drv_nmea.h). */
    if (t[0] != '\0') { f->utc  = to_uint(t); }
    if (d[0] != '\0') { f->date = to_uint(d); }

    if (status[0] == 'A') {
        char lat[16];
        char ns[2];
        char lon[16];
        char ew[2];
        char spd[10];
        char cog[10];
        field(body, len, 3U, lat, sizeof(lat));
        field(body, len, 4U, ns, sizeof(ns));
        field(body, len, 5U, lon, sizeof(lon));
        field(body, len, 6U, ew, sizeof(ew));
        field(body, len, 7U, spd, sizeof(spd));
        field(body, len, 8U, cog, sizeof(cog));
        f->fix = true;
        if (lat[0] != '\0') { f->lat      = to_degrees(lat, ns, 90.0); }
        if (lon[0] != '\0') { f->lon      = to_degrees(lon, ew, 180.0); }
        if (spd[0] != '\0') { f->speed_kn = (float)to_double(spd); }
        if (cog[0] != '\0') { f->course_deg = (float)to_double(cog); }
        f->reports++;
    } else if (status[0] == 'V') {
        f->fix = false;   /* a void RMC clears the fix */
    }
}

/** @brief Parse a GSA body: its fix type, and its dilutions after the twelve
 *         satellite fields. */
static void parse_gsa(const char *body, uint8_t len, mesh_drv_nmea_fix_t *f)
{
    char type[4];
    char pdop[8];
    char vdop[8];

    field(body, len, 2U, type, sizeof(type));
    field(body, len, 15U, pdop, sizeof(pdop));
    field(body, len, 17U, vdop, sizeof(vdop));
    if (type[0] != '\0') { f->fix_type = (uint8_t)to_uint(type); }
    if (pdop[0] != '\0') { f->pdop     = (float)to_double(pdop); }
    if (vdop[0] != '\0') { f->vdop     = (float)to_double(vdop); }
}

/** @brief The hex value of one checksum digit, or -1. */
static int hexval(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'A') && (c <= 'F')) { return (c - 'A') + 10; }
    if ((c >= 'a') && (c <= 'f')) { return (c - 'a') + 10; }
    return -1;
}

/** @brief Check the line's checksum and, if it holds, parse it. */
static bool take_line(mesh_drv_nmea_t *n)
{
    char   *line = n->line;
    uint8_t len  = n->len;

    if ((len < 4U) || (line[0] != '$')) {
        return false;
    }

    /* Find the '*', XOR everything between it and the '$'. */
    uint8_t star = 0U;
    uint8_t sum  = 0U;
    for (uint8_t i = 1U; i < len; i++) {
        if (line[i] == '*') {
            star = i;
            break;
        }
        sum ^= (uint8_t)line[i];
    }
    if ((star == 0U) || ((star + 3U) != len)) {
        n->bad++;   /* no '*', too few checksum digits, or trailing rubbish */
        return false;
    }
    int const hi = hexval(line[star + 1U]);
    int const lo = hexval(line[star + 2U]);
    if ((hi < 0) || (lo < 0) || (((hi << 4) | lo) != (int)sum)) {
        n->bad++;
        return false;
    }
    n->good++;

    /* The body is from after the '$' to before the '*'. The sentence name is
     * its talker (two or more chars) and the three-letter type: GGA and RMC
     * are the last three of the first field. */
    const char *body = &line[1];
    uint8_t     blen = (uint8_t)(star - 1U);

    char head[8];
    field(body, blen, 0U, head, sizeof(head));
    uint8_t const hl = (uint8_t)strlen(head);
    if (hl >= 3U) {
        const char *type = &head[hl - 3U];
        if (strcmp(type, "GGA") == 0) {
            parse_gga(body, blen, &n->fix);
        } else if (strcmp(type, "RMC") == 0) {
            parse_rmc(body, blen, &n->fix);
        } else if (strcmp(type, "GSA") == 0) {
            parse_gsa(body, blen, &n->fix);
        } else if (strcmp(type, "GSV") == 0) {
            char v[4];
            field(body, blen, 3U, v, sizeof(v));
            n->fix.sats_in_view = (uint8_t)to_uint(v);
        }
    }
    return true;
}

void mesh_drv_nmea_reset(mesh_drv_nmea_t *n)
{
    (void)memset(n, 0, sizeof(*n));
}

bool mesh_drv_nmea_push(mesh_drv_nmea_t *n, char c)
{
    if (c == '$') {
        n->len      = 0U;
        n->overflow = false;
        n->started  = true;
        n->line[0]  = '$';
        n->len      = 1U;
        return false;
    }
    if ((c == '\r') || (c == '\n')) {
        bool took = false;
        if (n->started && !n->overflow) {
            took = take_line(n);
        }
        n->len     = 0U;
        n->started = false;
        return took;
    }
    if (!n->started) {
        return false;
    }
    if (n->len >= (uint8_t)(MESH_DRV_NMEA_LINE_MAX - 1U)) {
        n->overflow = true;
        return false;
    }
    n->line[n->len] = c;
    n->len++;
    return false;
}

void mesh_drv_nmea_read(const mesh_drv_nmea_t *n, mesh_drv_nmea_fix_t *out)
{
    *out = n->fix;
}
