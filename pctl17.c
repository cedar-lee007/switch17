// Switch17-Curfew — pctl legacy layer (firmware 1.0.0-17.0.1).
// GPLv3-or-later. No warranty.
#include "pctl17.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Make libnx open time:s instead of time:u so setting the network clock is allowed.
TimeServiceType __nx_time_service_type = TimeServiceType_System;

static void reinit(void) { pctlExit(); pctlInitialize(); }

Result pctl17_get(LegacyPlayTimer *out)
{
    memset(out, 0, sizeof(*out));
    reinit();
    return serviceDispatchOut(pctlGetServiceSession_Service(), 1456, *out); // GetPlayTimerSettings (<=17.0.1)
}

Result pctl17_set(const LegacyPlayTimer *in)
{
    reinit();
    Result rc = serviceDispatchIn(pctlGetServiceSession_Service(), 1951, *in); // SetPlayTimerSettingsForDebug (<=17.0.1)
    pctl17_logf("1951 SetPlayTimerSettings rc=0x%08X", (unsigned)rc);
    return rc;
}

// ---- byte-level accessors on the 0x34 blob ----
static u8 *hdr(LegacyPlayTimer *s)       { return (u8 *)s->raw; }
static const u8 *chdr(const LegacyPlayTimer *s) { return (const u8 *)s->raw; }
// group 0 begins at byte 4; each group is 6 bytes.
static u8 *grp(LegacyPlayTimer *s, int g)       { return (u8 *)s->raw + 4 + 6 * g; }
static const u8 *cgrp(const LegacyPlayTimer *s, int g) { return (const u8 *)s->raw + 4 + 6 * g; }

u8 pctl17_header(const LegacyPlayTimer *s, int i) { return chdr(s)[i]; }
void pctl17_set_header(LegacyPlayTimer *s, int i, u8 v) { hdr(s)[i] = v; }

u8 pctl17_group_byte(const LegacyPlayTimer *s, int g, int b) { return cgrp(s, g)[b]; }
void pctl17_set_group_byte(LegacyPlayTimer *s, int g, int b, u8 v) { grp(s, g)[b] = v; }

u16 pctl17_group_minutes(const LegacyPlayTimer *s, int g)
{
    const u8 *p = cgrp(s, g);
    return (u16)(p[4] | (p[5] << 8));   // little-endian u16
}
void pctl17_set_group_minutes(LegacyPlayTimer *s, int g, u16 v)
{
    u8 *p = grp(s, g);
    p[4] = (u8)(v & 0xFF);
    p[5] = (u8)((v >> 8) & 0xFF);
}

void pctl17_build(LegacyPlayTimer *out, u8 mode, bool suspend,
                  bool bedtime_on, u8 bed_h, u8 bed_m,
                  const u16 minutes[7])
{
    memset(out, 0, sizeof(*out));
    u8 *h = hdr(out);
    h[0] = 1;                 // enabled
    h[1] = suspend ? 1 : 0;   // restrictionMode
    h[2] = mode & 1;          // timerMode
    h[3] = 0;                 // reserved

    // We always fill ALL EIGHT groups identically for a clean, predictable blob.
    // mode=0 (DAILY): pctl reads group[0].  mode=1 (WEEKLY): pctl reads group[1..7].
    for (int g = 0; g < 8; g++) {
        u8 *p = grp(out, g);
        if (bedtime_on) {
            p[0] = 1;         // bedtimeEnabled
            p[1] = bed_h;     // hour
            p[2] = bed_m;     // minute
        }
        if (minutes) {
            int day = g ? (g - 1) : 0;    // group1=Sunday..group7=Saturday; group0 reuses Sunday
            u16 m = minutes[day];
            if (m != PT_NOLIMIT) {
                p[3] = 1;                        // limitEnabled
                p[4] = (u8)(m & 0xFF);
                p[5] = (u8)((m >> 8) & 0xFF);
            }
        }
    }
}

// ---- network clock ----
void clock17_status(Clock17Status *out)
{
    memset(out, 0, sizeof(*out));
    out->user_ok    = R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &out->user));
    out->network_ok = R_SUCCEEDED(timeGetCurrentTime(TimeType_NetworkSystemClock, &out->network));
}

bool clock17_is_seeded(void)
{
    u64 t = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_NetworkSystemClock, &t))) return false;
    return t > 1577836800ULL;   // > 2020-01-01 UTC: plausible, not empty/garbage
}

Result clock17_seed_network_from_user(void)
{
    u64 now = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &now);
    if (R_SUCCEEDED(rc) && now > 1577836800ULL)
        rc = timeSetCurrentTime(TimeType_NetworkSystemClock, now);
    pctl17_logf("seed NetworkSystemClock from UserSystemClock rc=0x%08X", (unsigned)rc);
    return rc;
}

Result clock17_set_manual(u64 posix_seconds)
{
    Result rc = timeSetCurrentTime(TimeType_UserSystemClock, posix_seconds);
    if (R_SUCCEEDED(rc)) rc = timeSetCurrentTime(TimeType_NetworkSystemClock, posix_seconds);
    return rc;
}

// ---- logging ----
void pctl17_logf(const char *fmt, ...)
{
    FILE *f = fopen("/switch/pctl17.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---- read-only diagnostic dump ----
static void rep(char **p, char *end, const char *fmt, ...)
{
    if (*p >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);
    if (n > 0) { *p += n; if (*p > end) *p = end; }
}

void pctl17_dump(char *buf, size_t bufsz)
{
    if (!bufsz) return;
    char *p = buf, *e = buf + bufsz;
    buf[0] = '\0';

    u32 hv = hosversionGet();
    rep(&p, e, "=== Switch17-Curfew diagnostic dump (read-only) ===\n");
    rep(&p, e, "firmware %u.%u.%u\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv));

    Clock17Status c;
    clock17_status(&c);
    rep(&p, e, "UserSystemClock   : ok=%d  %llu\n", c.user_ok, (unsigned long long)c.user);
    rep(&p, e, "NetworkSystemClock: ok=%d  %llu  seeded=%d\n", c.network_ok,
        (unsigned long long)c.network, clock17_is_seeded());

    reinit();
    Service *srv = pctlGetServiceSession_Service();
    u32 pinlen = 0;
    Result rp = serviceDispatchOut(srv, 1206, pinlen);
    rep(&p, e, "1206 GetPinCodeLength: rc=0x%08X len=%u\n", (unsigned)rp, (unsigned)pinlen);

    bool en = false;
    Result re = serviceDispatchOut(srv, 1031, en);
    rep(&p, e, "1031 IsRestrictionEnabled: rc=0x%08X %s\n", (unsigned)re, R_SUCCEEDED(re)?(en?"true":"false"):"-");

    LegacyPlayTimer s;
    Result rs = pctl17_get(&s);
    rep(&p, e, "\n1456 GetPlayTimerSettings: rc=0x%08X\n", (unsigned)rs);
    if (R_SUCCEEDED(rs)) {
        rep(&p, e, "header: enabled=%u restriction=%u (%s) mode=%u (%s)\n",
            pctl17_header(&s,0), pctl17_header(&s,1),
            pctl17_header(&s,1) ? "SUSPEND" : "ALARM",
            pctl17_header(&s,2), pctl17_header(&s,2) ? "WEEKLY" : "DAILY");
        rep(&p, e, "\ngrp  bed  HH:MM  limitEn  minutes\n");
        static const char *const names[8] = {"daily","Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        for (int g = 0; g < 8; g++) {
            u8 b0 = pctl17_group_byte(&s, g, 0);
            u8 hh = pctl17_group_byte(&s, g, 1);
            u8 mm = pctl17_group_byte(&s, g, 2);
            u8 le = pctl17_group_byte(&s, g, 3);
            u16 mn = pctl17_group_minutes(&s, g);
            rep(&p, e, "%-5s %3u  %02u:%02u  %5u  %7u\n",
                names[g], b0, hh, mm, le, (unsigned)mn);
        }
        rep(&p, e, "\nraw u16[26]:\n");
        for (int i = 0; i < 26; i++) {
            rep(&p, e, "%u%s", s.raw[i], (i%8==7)?"\n":" ");
        }
        rep(&p, e, "\n\nraw bytes (0x34):\n");
        const u8 *b = (const u8 *)s.raw;
        for (int i = 0; i < 0x34; i++) rep(&p, e, "%02X%s", b[i], (i%16==15)?"\n":" ");
        rep(&p, e, "\n");
    }
}
