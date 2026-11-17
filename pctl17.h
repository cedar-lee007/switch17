// Switch17-Curfew — parental-control play timer for firmware 17.x (offline).
// Data layer: the legacy pctl commands 1456 (Get) / 1951 (Set) and the 0x34-byte
// PlayTimerSettings struct that firmware 1.0.0-17.0.1 use.
//
// Based on the reverse-engineered layout published by:
//   aylz10/Reset-Parental-Controls-NX (playtimer branch)
//   tailiang2008/NX-Pctl-Manager docs/pctl-reverse-engineering.md
//   switchbrew.org/wiki/Parental_Control_services
//
// GPLv3-or-later. No warranty.
#pragma once
#include <switch.h>
#include <stdbool.h>

#define PT_NOLIMIT 0xFFFFu   // sentinel: this day has NO limit (0 means "blocked all day")

// ---- legacy PlayTimerSettings: 0x34 bytes = 4-byte header + 8 groups x 6 bytes ----
// Header:
//   byte0 enabled        1 = timer on
//   byte1 restriction    0 = ALARM (notify only), 1 = FORCED_TERMINATION (suspend software)
//   byte2 timerMode      0 = DAILY (use group[0]), 1 = EACH_DAY_OF_THE_WEEK (use group[1..7])
//   byte3 reserved
// Group 0 = daily; group 1..7 = Sunday..Saturday. Each group is 6 bytes:
//   +0 bedtimeEnabled
//   +1 bedtimeHour
//   +2 bedtimeMinute
//   +3 limitEnabled
//   +4 limitMinutes (u16 little-endian)
// NOTE: firmware 20+ inserts 2 bytes (resetHour/resetMinute) into each group,
// making it 8 bytes (0x44 total). We target 17.x only -> 6-byte groups.
typedef struct {
    u16 raw[26];   // 26 * 2 = 52 = 0x34
} LegacyPlayTimer;

_Static_assert(sizeof(LegacyPlayTimer) == 0x34, "legacy play timer must be 0x34 bytes");

Result pctl17_get(LegacyPlayTimer *out);
Result pctl17_set(const LegacyPlayTimer *in);

// Rebuild a settings blob from scratch.
//  - mode: 0 = DAILY (one limit for all days), 1 = per weekday
//  - suspend: true -> forced termination at time-up, false -> notify only
//  - bedtimeEnabled + bedtimeHour/Minute: curfew start (e.g. 21:30). 0xFF = not set (leave off).
//  - minutes[7]: per-day play limit; PT_NOLIMIT = no limit. Pass NULL to leave all unlimited.
void pctl17_build(LegacyPlayTimer *out, u8 mode, bool suspend,
                  bool bedtime_on, u8 bed_h, u8 bed_m,
                  const u16 minutes[7]);

// Decode helpers (byte offsets are on the 0x34-byte blob).
u8  pctl17_header(const LegacyPlayTimer *s, int index);            // 0..3
u8  pctl17_group_byte(const LegacyPlayTimer *s, int group, int b); // group 0..7, byte 0..5
u16 pctl17_group_minutes(const LegacyPlayTimer *s, int group);
void pctl17_set_group_byte(LegacyPlayTimer *s, int group, int b, u8 v);
void pctl17_set_group_minutes(LegacyPlayTimer *s, int group, u16 v);
void pctl17_set_header(LegacyPlayTimer *s, int index, u8 v);

// ---- network clock (needed so pctl's countdown actually runs offline) ----
// Overriding this libnx global makes time service use time:s instead of time:u,
// which is required for timeSetCurrentTime() on the network clock (QuickNTP trick).
extern TimeServiceType __nx_time_service_type;

typedef struct {
    bool user_ok;
    u64  user;         // POSIX seconds
    bool network_ok;
    u64  network;
} Clock17Status;

void     clock17_status(Clock17Status *out);
Result   clock17_seed_network_from_user(void);
bool     clock17_is_seeded(void);   // network clock readable and plausible (> 2020-01-01)
Result   clock17_set_manual(u64 posix_seconds);  // set user clock (and reseed network)

// ---- diagnostics ----
void pctl17_dump(char *buf, size_t bufsz);   // read-only dump of header/groups/clocks/PIN
void pctl17_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
