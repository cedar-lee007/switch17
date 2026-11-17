// Switch17-Curfew — offline bedtime/curfew + daily play-time control for
// Nintendo Switch firmware 17.x, no phone app, no account, no internet.
// GPLv3-or-later. No warranty. Use at your own risk on CFW (Atmosphere).
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pctl17.h"
#include "menu.h"

#define APP_TITLE "=== Switch17 Curfew  (fw 17.x offline) ==="

static PadState g_pad;

// ---- persisted config (SD card, plain text) ----
#define CFG_PATH "/switch/pctl17.cfg"
typedef struct {
    int  bed_h;      // curfew start hour
    int  bed_m;
    bool bed_on;
    bool suspend;    // true = force-stop software; false = notify only
    int  limit_min;  // -1 = no daily limit
} Config;
static Config g_cfg = { 21, 30, true, true, -1 };

static void cfg_load(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    int eh, em, lim; int bon, sus;
    if (fscanf(f, "bed=%d:%d on=%d suspend=%d limit=%d", &eh,&em,&bon,&sus,&lim) == 5) {
        if (eh>=0&&eh<=23) g_cfg.bed_h=eh;
        if (em>=0&&em<=59) g_cfg.bed_m=em;
        g_cfg.bed_on = bon!=0;
        g_cfg.suspend = sus!=0;
        g_cfg.limit_min = (lim<0)?-1:lim;
    }
    fclose(f);
}
static void cfg_save(void)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "bed=%d:%02d on=%d suspend=%d limit=%d\n",
            g_cfg.bed_h, g_cfg.bed_m, g_cfg.bed_on?1:0,
            g_cfg.suspend?1:0, g_cfg.limit_min);
    fclose(f);
}

// ---- status snapshot ----
static Clock17Status g_clock;
static LegacyPlayTimer g_pt;
static bool g_pt_ok;
static u32 g_pinlen = 0;

static void refresh(void)
{
    clock17_status(&g_clock);
    g_pt_ok = R_SUCCEEDED(pctl17_get(&g_pt));
    Service *srv = pctlGetServiceSession_Service();
    g_pinlen = 0;
    serviceDispatchOut(srv, 1206, g_pinlen);
}

static void draw_status(void)
{
    printf("PIN            : %s\n", g_pinlen ? "set" : "NOT SET (writes will be ignored!)");
    printf("Network clock  : %s\n", clock17_is_seeded() ? "OK (countdown runs)" : "NOT SET (timer won't count!)");
    printf("Daily limit    : ");
    if (g_pt_ok) {
        u8 le = pctl17_group_byte(&g_pt, 1, 3);
        if (le) printf("%u min\n", (unsigned)pctl17_group_minutes(&g_pt,1));
        else    printf("none\n");
    } else printf("(unreadable)\n");
    printf("Curfew(bedtime): ");
    if (g_pt_ok) {
        u8 on = pctl17_group_byte(&g_pt, 1, 0);
        if (on) printf("%02u:%02u -> next 06:00  [%s]\n",
                       pctl17_group_byte(&g_pt,1,1), pctl17_group_byte(&g_pt,1,2),
                       pctl17_header(&g_pt,1) ? "SUSPEND" : "ALARM");
        else printf("off\n");
    } else printf("(unreadable)\n");
}

// ---- helpers ----
static void msg_then_back(const char *line)
{
    menu_clear();
    printf("%s\n", line);
    menu_wait_back();
}

// prompt an integer in [lo,hi]; returns false if cancelled
static bool prompt_int(const char *hdr, const char *guide, int seed, int lo, int hi, int *out)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetStringLenMin(&kbd, 1);
    swkbdConfigSetStringLenMax(&kbd, 4);
    swkbdConfigSetHeaderText(&kbd, hdr);
    swkbdConfigSetGuideText(&kbd, guide);
    char ini[16]; snprintf(ini, sizeof(ini), "%d", seed);
    swkbdConfigSetInitialText(&kbd, ini);
    char s[16] = {0};
    Result rc = swkbdShow(&kbd, s, sizeof(s));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    long v = strtol(s, NULL, 10);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *out = (int)v;
    return true;
}

// Ensure the countdown can actually run on an offline console.
static bool ensure_clock(void)
{
    if (clock17_is_seeded()) return true;
    menu_clear();
    printf("Network clock is empty -> play timer won't count.\n");
    printf("Seeding it from the console clock...\n");
    consoleUpdate(NULL);
    Result rc = clock17_seed_network_from_user();
    refresh();
    if (R_FAILED(rc) || !clock17_is_seeded()) {
        printf("Auto-seed failed (0x%08X).\n", (unsigned)rc);
        printf("Set the console date/time in System Settings, then retry.\n");
        menu_wait_back();
        return false;
    }
    return true;
}

// Write the current config to pctl and read it back to verify.
static Result apply_config(void)
{
    u16 mins[7];
    for (int i=0;i<7;i++) mins[i] = (g_cfg.limit_min < 0) ? PT_NOLIMIT : (u16)g_cfg.limit_min;
    LegacyPlayTimer s;
    pctl17_build(&s, 0 /*DAILY: one setting for all days*/, g_cfg.suspend,
                 g_cfg.bed_on, (u8)g_cfg.bed_h, (u8)g_cfg.bed_m,
                 g_cfg.limit_min < 0 ? NULL : mins);
    Result rc = pctl17_set(&s);
    if (R_SUCCEEDED(rc)) cfg_save();
    return rc;
}

// ---- actions ----
static void act_apply(void)
{
    if (g_pinlen == 0) { msg_then_back("Set a parental-control PIN in System Settings first.\nWithout a PIN pctl silently discards all writes."); return; }
    if (!ensure_clock()) return;
    Result rc = apply_config();
    refresh();
    menu_clear();
    if (R_FAILED(rc)) {
        printf("Write FAILED (0x%08X).\n", (unsigned)rc);
    } else {
        printf("Written OK. Read-back:\n\n");
        if (g_pt_ok) {
            u8 on = pctl17_group_byte(&g_pt,1,0);
            printf("  Curfew      : %s\n", on ? "ON" : "off");
            if (on) printf("  Starts      : %02u:%02u (until next 06:00)\n",
                           pctl17_group_byte(&g_pt,1,1), pctl17_group_byte(&g_pt,1,2));
            u8 le = pctl17_group_byte(&g_pt,1,3);
            printf("  Daily limit : %s\n", le ? "set" : "none");
        } else printf("  (could not read back)\n");
        printf("\nLeave this app and launch a game to activate.\n");
    }
    menu_wait_back();
}

static void act_toggle_curfew(void)
{
    g_cfg.bed_on = !g_cfg.bed_on;
    act_apply();
}

static void act_set_time(void)
{
    int h=g_cfg.bed_h, m=g_cfg.bed_m;
    int hh, mm;
    if (!prompt_int("Curfew start HOUR (0-23)", "e.g. 21 for 21:30", h, 0, 23, &hh)) return;
    if (!prompt_int("Curfew start MINUTE (0-59)", "e.g. 30 for 21:30", m, 0, 59, &mm)) return;
    g_cfg.bed_h=hh; g_cfg.bed_m=mm; g_cfg.bed_on=true;
    act_apply();
}

static void act_set_mode(void)
{
    g_cfg.suspend = !g_cfg.suspend;
    act_apply();
}

static void act_set_limit(void)
{
    int seed = g_cfg.limit_min < 0 ? 0 : g_cfg.limit_min;
    int v;
    if (!prompt_int("Daily play-time limit (minutes)", "0-1440, or 0 to remove limit", seed, 0, 1440, &v)) return;
    g_cfg.limit_min = v;
    act_apply();
}

static void act_remove_limit(void)
{
    g_cfg.limit_min = -1;
    act_apply();
}

static void act_clock(void)
{
    menu_clear();
    printf("Network clock : %s\n", clock17_is_seeded() ? "OK" : "NOT SET");
    printf("Console clock : %llu\n\n", (unsigned long long)g_clock.user);
    printf("A: seed network clock from console clock\n");
    printf("B: back\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 k = padGetButtonsDown(&g_pad);
        if (k & HidNpadButton_B) break;
        if (k & HidNpadButton_A) {
            Result rc = clock17_seed_network_from_user();
            refresh();
            menu_clear();
            printf("Seed result: 0x%08X\n", (unsigned)rc);
            printf("Network clock now: %s\n", clock17_is_seeded()?"OK":"still empty");
            printf("Make sure System Settings date/time is correct.\n");
            menu_wait_back();
            break;
        }
        consoleUpdate(NULL);
    }
}

static void act_set_pin(void)
{
    pctlExit();
    Result rc = pctlauthRegisterPasscode();
    pctlInitialize();
    refresh();
    menu_clear();
    printf( R_SUCCEEDED(rc) ? "PIN screen done.\n" : "PIN screen failed (0x%08X).\n", (unsigned)rc);
    menu_wait_back();
}

static void act_dump(void)
{
    static char rep[8192];
    pctl17_dump(rep, sizeof(rep));
    FILE *f = fopen("/switch/pctl17_dump.txt", "w");
    if (f) { fputs(rep, f); fclose(f); }
    menu_clear();
    printf("%s\n", rep);
    printf("--- saved to /switch/pctl17_dump.txt ---\n");
    menu_wait_back();
}

static const MenuItem g_items[] = {
    { "Apply curfew + settings now",        NULL, act_apply },
    { "Toggle curfew ON/OFF",               NULL, act_toggle_curfew },
    { "Set curfew start time (HH:MM)",      NULL, act_set_time },
    { "Toggle SUSPEND / NOTIFY-only",       NULL, act_set_mode },
    { "Set daily play-time limit (min)",    NULL, act_set_limit },
    { "Remove daily play-time limit",       NULL, act_remove_limit },
    { "Network clock (fix 'no reaction')",  NULL, act_clock },
    { "Set / change parental PIN",          NULL, act_set_pin },
    { "Dump config + diagnostics to SD",    NULL, act_dump },
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    Result rc = pctlInitialize();
    if (R_FAILED(rc)) {
        printf("pctlInitialize failed 0x%08X\n", (unsigned)rc);
        printf("Requires CFW (Atmosphere). Press + to exit.\n");
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }

    cfg_load();
    refresh();

    // Auto-seed the network clock once on startup so the countdown works offline.
    if (!clock17_is_seeded()) {
        clock17_seed_network_from_user();
        refresh();
    }

    menu_run(APP_TITLE, draw_status, g_items,
             sizeof(g_items)/sizeof(g_items[0]), &g_pad);

    pctlExit();
    consoleExit(NULL);
    return 0;
}
