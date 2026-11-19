/*
 * Sid Meier's Pirates! (2004, GOG 2.0.0.4 / Pirates!.exe sha1 aeca6272...) - couch co-op + Xbox controller mod
 * Loaded as version.dll from the game folder.
 *
 * What it does
 *  - Real XInput (Xbox 360/One/Series, Bluetooth too) fed into the game's own built-in Xbox control layer
 *    (the PC exe still contains the original Xbox gamepad code: FUN_004f0ba0 = XInputGetState emulation).
 *    Plug and play, hot-plug, players follow connected pads (ROG Ally rule).
 *  - Player 2 drives one ship of Player 1's fleet on the sailing map and in sea battles (needs >= 2 ships).
 *    Babysitting: if Player 2 gets too far from Player 1 he is steered back and sped up.
 *  - Sword duels: tap SELECT to pass the sword to the other player (PC10: no automatic pass on mistakes).
 *  - Town/port menus: Player 1 has them, hold BACK to pass to Player 2 (and back).
 *  - On-screen notifications (Direct3D 9 overlay), each shown ~20 s.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <shlwapi.h>
#include <stdlib.h>
#include "font_atlas.h"

#define MOD_VERSION "PC15"

/* ------------------------------------------------------------------ game addresses (GOG 2.0.0.4) */
#define G(t, a) (*(volatile t *)(a))
#define WSHIP_BASE   0x8142f8u  /* world ships, stride 0x45c. 0 = flagship, 1..7 = Player 1's fleet */
#define WSHIP_STRIDE 0x45cu
#define WS_TYPE   0x00 /* short, -1 = empty */
#define WS_X      0x0c
#define WS_Y      0x10
#define WS_HEAD   0x14
#define WS_SPEED  0x18
#define WS_MORALE 0x20 /* short */
#define WS_HULL   0x22 /* short, damage */
#define WS_CREW   0x26 /* short */
#define WS_GUNS   0x28 /* short */
#define WS_SAILQ  0x2a /* short */
#define WS_UPGR   0x2c /* short */
#define BSHIP_BASE   0x8bc468u  /* sea battle ships: base + side*0x12a0 + ship*0x4a8 */
#define BSHIP_STRIDE 0x4a8u
#define BS_TYPE 0x00
#define BS_X    0x04
#define BS_Y    0x08
#define BS_HEAD 0x0c
#define BS_SPEED 0x10
#define BS_TURNS 0x14
#define BS_GUNS 0x18
#define BS_CREW 0x1c
#define BS_SAILQ 0x20
#define BS_HULL 0x28
#define BS_MORALE 0x2c
#define BS_STEER 0x30
#define BS_TARGETHEAD 0x38
#define BS_LOADED 0x44
#define BS_SHOT 0x48
#define BS_FLAGS 0x64
#define BS_UPGR 0x68
#define TYPE_STATS 0x723b08u /* + type*0x44 */
#define GAME_FLAGS   0x85a164u /* bit 2 = paused */
#define SAIL_SLOWDOWN 0x8b98c8u
#define DUEL0_STEP   0x72ccf0u /* duelist 0 (the human) step index; grows when he is pushed back */
#define DUEL1_STEP   (0x72ccf0u + 0x4c)
#define CAMERA_PTR   0x8e9fd8u
#define BATTLE_ORIGX 0x8b99d4u
#define BATTLE_ORIGY 0x8b99d0u

/* code pointers used by stubs.S */
void *p_padmap    = (void *)0x42d320;
void *p_atan2     = (void *)0x4db270;
void *p_trig      = (void *)0x4db210;
void *p_duel      = (void *)0x41b1d0;
void *p_landtest  = (void *)0x476130;
void *p_bhud_draw = (void *)0x47ba85;
void *p_bhud_skip = (void *)0x47bbb8;
unsigned g_duel_ret;
unsigned g_p2_ebpoff = 0xffffffffu;
void *p_wfm_show = (void *)0x468f06;
void *p_wfm_skip = (void *)0x468fa4;
void *p_name_entry = (void *)0x470b90;    /* "Crew Signups" name entry loop */
void *p_ship_name  = (void *)0x471120;    /* "Name your ship" loop (EAX = ship) */
void *p_shipyard   = (void *)0x4d6600;    /* shipwright screen (arg = screen object, focus at +0xc) */
void *p_getkey2    = (void *)0x42e440;    /* key read used by the trade/plunder screen */
void *p_menu_cont  = (void *)0x430195;    /* generic choice menu FUN_00430190, after its first instruction */
void *p_loadcfg    = (void *)0x4d1a60;    /* reads Config.ini */
unsigned g_ret_name, g_ret_ship, g_ret_yard;
void *p_text  = (void *)0x4b06c0;         /* game 2D text: (text,x,y,size,argb,flags,-1,0), EAX = font */
void *p_frame = (void *)0x4d0cd0;         /* draw + show one frame */
void game_text(const char *s, int x, int y, int size, unsigned argb, int flags);
void stub_osk_frame(void);
void *p_rotate = (void *)0x404e50;         /* "change flagship": fleet ship 1 becomes the flagship, old flagship goes last (EAX passed through) */
unsigned g_rot_eax; int g_rot_skip, g_rot_again;
void *p_addship = (void *)0x404b70;       /* PC8: add a copy of world ship EAX to the fleet */
void *p_happy_cont = (void *)0x404817;    /* PC8: crew mood after its first instruction */
void *p_bcam = (void *)0x489360;          /* PC8: sea battle camera placement */
void stub_happy(void); void stub_bcam(void); void stub_tavern_pick(void); void stub_surrender(void);   /* PC11 */
void stub_btn_frustum(void); void stub_shipbtn(void);       /* PC12 */
void stub_trade_ok(void); void *p_tradeok_cont = (void *)0x40fe05; int g_friendly_on;   /* PC10 */
void stub_fame(void); void *p_fame_cont = (void *)0x43fd76;   /* PC9: fame points after the 6 copied bytes */
int g_fame_on, g_fame_value = 100;

typedef unsigned (__cdecl *t_sailship)(int);
typedef int (__cdecl *t_battle)(int);
typedef int (__cdecl *t_bship)(int, int, int);
typedef float (__cdecl *t_axis)(int, const char *);
typedef int (__cdecl *t_fire)(int, int, int, int);
static t_sailship o_sailship = (t_sailship)0x467f90;
static t_battle   o_battle   = (t_battle)0x47a040;
static t_bship    o_bship    = (t_bship)0x477500;
static t_axis     o_axis     = (t_axis)0x42d500;
static t_fire     o_fire     = (t_fire)0x477cb0;

void stub_waitforme(void); void stub_padfill(void); void stub_padmap(void); void stub_fleet_atan(void); void stub_duel(void); void stub_bhud(void);
void stub_rotate(void); void stub_name_entry(void); void stub_ship_name(void); void stub_shipyard(void); void stub_tradekey(void); void stub_menu(void); void stub_frustum(void); void stub_regobj(void);
int game_atan2(int dx, int dy); int game_trig(int r, unsigned angle); int game_battle_blocked(int x, int y);

/* ------------------------------------------------------------------ config / log */
static char g_dir[MAX_PATH];
static FILE *g_log;
static struct {
    int enabled, require_two_ships, notify_ms, hold_back_ms, absolute_steer;
    int sail_leash_start, sail_leash_full, battle_leash_start, battle_leash_full;
    int p2_speed_pct, p2_catchup_pct, duel_autopass, rumble, debug, edge_off, edge_back;
    int upwind_pct, ignore_mouse, idle_sec, unstick;
    int speed_mult, happy_crew, p2_ram_duel, far_zoom_pct, sail_delay_max, friendly_fire_fix, always_fire, zoom_speed_pct, max_fame, fame_value, friendly_nations, never_dizzy; int mirror_sides, edge_band, hide_letterbox, no_stretch; int p1_shot_mod, reduced_fast, trade_wide, map_sides, ram_contact, surrender_rule, tavern_default; unsigned map_side_col; int land_guard, button_fit, shipbtn_guard; int duel_pvp, icons_left, fight_dpad; int width, height, widescreen_fov, black_bars, pillarbox, signup_fit, overlay_test, hide_cursor, osk, menu_default_first, dpad_zoom, zoom_repeat_ms;
} cfg;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static int ini_int(const char *sec, const char *key, int def)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\PiratesCoop.ini", g_dir);
    return GetPrivateProfileIntA(sec, key, def, p);
}

static void load_config(void)
{
    cfg.enabled            = ini_int("Coop", "Enabled", 1);
    cfg.require_two_ships  = ini_int("Coop", "RequireTwoShips", 1);
    cfg.notify_ms          = ini_int("Coop", "NotificationSeconds", 20) * 1000;
    cfg.hold_back_ms       = ini_int("Coop", "HoldBackMs", 0);   /* PC7: 0 = a tap of SELECT passes */
    cfg.absolute_steer     = ini_int("Coop", "Player2PointToSteer", 1);
    cfg.sail_leash_start   = ini_int("Babysit", "NoCameraSailLeash", 2500000);
    cfg.sail_leash_full    = cfg.sail_leash_start;
    cfg.battle_leash_start = ini_int("Babysit", "NoCameraBattleLeash", 2000000);
    cfg.battle_leash_full  = cfg.battle_leash_start;
    cfg.p2_speed_pct       = ini_int("Babysit", "Player2SpeedPercent", 115);
    cfg.edge_off           = ini_int("Babysit", "OffScreenPercent", 102);
    cfg.edge_back          = ini_int("Babysit", "BackOnScreenPercent", 75);
    cfg.width              = ini_int("Display", "Width", 1920);
    cfg.height             = ini_int("Display", "Height", 1080);
    cfg.widescreen_fov     = ini_int("Display", "WidescreenFOV", 1);
    cfg.black_bars         = ini_int("Display", "BlackSideBars", 0);
    cfg.pillarbox          = 0;
    cfg.signup_fit         = ini_int("Display", "Fit2D", 2);   /* 0 off, 1 crew sign-up page only, 2 every 2D screen */
    cfg.mirror_sides       = ini_int("Display", "ExtendBackgrounds", 1);
    cfg.edge_band          = ini_int("Display", "EdgeBand", 28);
    cfg.hide_letterbox     = ini_int("Display", "HideLetterbox", 1);
    cfg.no_stretch         = ini_int("Display", "NoStretch4x3Cameras", 1);
    cfg.overlay_test       = ini_int("Debug", "OverlayTest", 0);
    cfg.hide_cursor        = ini_int("Controller", "HideMouseWithPad", 1);
    cfg.osk                = ini_int("Controller", "OnScreenKeyboard", 1);
    cfg.menu_default_first = ini_int("Controller", "MenuStartsOnFirstChoice", 1);
    cfg.dpad_zoom          = ini_int("Controller", "DpadZoom", 1);
    cfg.zoom_repeat_ms     = ini_int("Controller", "ZoomRepeatMs", 90);
    cfg.p2_catchup_pct     = ini_int("Babysit", "CatchUpSpeedPercent", 170);
    cfg.duel_autopass      = ini_int("Coop", "AutoPassOnMistake", 0);   /* PC10 (user): off - manual SELECT only. Old key DuelMistakePassesSword ignored. */
    cfg.rumble             = ini_int("Controller", "Rumble", 1);
    cfg.debug              = ini_int("Debug", "Verbose", 0);
    cfg.upwind_pct         = ini_int("Sailing", "UpwindSpeedPercent", 100);   /* PC7: with FULL sails only */
    cfg.ignore_mouse       = ini_int("Controller", "IgnoreMouseWithPad", 1);
    cfg.idle_sec           = ini_int("Babysit", "IdleSecondsBeforeReturn", 20);
    cfg.unstick            = ini_int("Sailing", "UnstickFromLand", 1);
    /* PC8: mod menu settings (also changed in game, saved back here) */
    cfg.speed_mult         = ini_int("ModMenu", "ShipSpeed", 1);
    if (cfg.speed_mult < 1) cfg.speed_mult = 1; if (cfg.speed_mult > 3) cfg.speed_mult = 3;
    cfg.happy_crew         = ini_int("ModMenu", "CrewAlwaysHappy", 0);
    cfg.p2_ram_duel        = ini_int("ModMenu", "Player2RamStartsSwordFight", 1);
    cfg.far_zoom_pct       = ini_int("Battle", "FarZoomPercent", 200);
    cfg.sail_delay_max     = ini_int("Battle", "SailChangeMaxFrames", 30);
    cfg.friendly_fire_fix  = ini_int("Battle", "FireUnlessFriendCloser", 1);   /* PC8 key, no longer used */
    cfg.always_fire        = ini_int("Battle", "AlwaysFire", 1);                /* PC9 */
    cfg.max_fame           = ini_int("ModMenu", "MaxFame", 0);                  /* PC9 */
    cfg.fame_value         = ini_int("ModMenu", "MaxFameValue", 100);
    cfg.friendly_nations   = ini_int("ModMenu", "FriendlyNations", 0);
    cfg.never_dizzy        = ini_int("ModMenu", "NeverDizzyInSwordFights", 1);   /* PC10 */
    cfg.zoom_speed_pct     = ini_int("Battle", "ZoomSpeedPercentPerSecond", 35);/* PC9: % of the zoom range per second */
    /* PC11 */
    cfg.p1_shot_mod        = ini_int("Battle", "Player1ShotTypeByMod", 1);       /* X/B for P1 handled like P2 (never lost) */
    cfg.reduced_fast       = ini_int("Sailing", "ReducedSailsFullSpeed", 1);    /* sails DOWN = full speed on every heading, full sails = vanilla */
    cfg.trade_wide         = ini_int("Display", "TradeScreenWideButtons", 0);   /* PC12: off - did not help (wrong camera), crash suspect */
    cfg.land_guard         = ini_int("Sailing", "Player2StaysOffLand", 1);       /* PC12 */
    cfg.button_fit         = ini_int("Display", "FitButtonScreens", 1);          /* PC12: 3D buttons (trade arrows...) not stretched */
    cfg.shipbtn_guard      = 1;
    cfg.duel_pvp           = ini_int("ModMenu", "DuelEachOther", 0);            /* PC13 */
    cfg.icons_left         = ini_int("Display", "TradeIconsLeftOfPrice", 1);    /* PC13 */
    cfg.fight_dpad         = ini_int("Controller", "LandBattleDpadMovesArmy", 1); /* PC15 */
    cfg.map_sides          = ini_int("Display", "MapSides", 1);                 /* 0 = PC10 mirrored copies, 1 = plain parchment */
    cfg.map_side_col       = (unsigned)ini_int("Display", "MapSideColour", (int)0xe0f1e7c8u);   /* ARGB hex as decimal in the ini; default parchment, slightly see-through */
    cfg.ram_contact        = ini_int("Battle", "RamNeedsContact", 1);           /* P2 boarding only when the hulls touch */
    cfg.surrender_rule     = ini_int("Battle", "SurrenderOnlyBelow10Percent", 1);
    cfg.tavern_default     = ini_int("Controller", "TavernStartsOnCrew", 1);
}
static void ini_save_int(const char *sec, const char *key, int v)
{
    char p[MAX_PATH], b[16]; snprintf(p, sizeof p, "%s\\PiratesCoop.ini", g_dir); snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA(sec, key, b, p);
}

/* ------------------------------------------------------------------ notifications */
#define MAX_NOTES 6
static struct { char text[160]; DWORD until, born; int key; } notes[MAX_NOTES];
static CRITICAL_SECTION g_cs;

static void notify_key(int key, const char *fmt, ...)
{
    char buf[160]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    int slot = -1;
    if (key) for (int i = 0; i < MAX_NOTES; i++) if (notes[i].key == key && notes[i].until > now) slot = i;
    if (slot < 0) {               /* oldest / free */
        DWORD best = 0xffffffff; for (int i = 0; i < MAX_NOTES; i++) {
            if (notes[i].until <= now) { slot = i; break; }
            if (notes[i].born < best) { best = notes[i].born; slot = i; }
        }
    }
    lstrcpynA(notes[slot].text, buf, sizeof notes[slot].text);
    notes[slot].born = now; notes[slot].until = now + cfg.notify_ms; notes[slot].key = key;
    LeaveCriticalSection(&g_cs);
    logf_("NOTE: %s", buf);
}
#define notify(...) notify_key(0, __VA_ARGS__)
enum { NK_COOP = 1, NK_CTRL, NK_DUEL, NK_SHIP, NK_LEASH, NK_PAD, NK_BATTLE };

/* ------------------------------------------------------------------ XInput */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT sLX, sLY, sRX, sRY; } XPAD;
typedef struct { DWORD dwPacketNumber; XPAD Gamepad; } XSTATE;
typedef struct { WORD wLeft, wRight; } XVIB;
typedef DWORD (WINAPI *t_XInputGetState)(DWORD, XSTATE *);
typedef DWORD (WINAPI *t_XInputSetState)(DWORD, XVIB *);
static t_XInputGetState pXGet; static t_XInputSetState pXSet;
#define XB_UP 0x0001
#define XB_DOWN 0x0002
#define XB_LEFT 0x0004
#define XB_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LS 0x0040
#define XB_RS 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000

static struct { int connected; XSTATE st; DWORD last_try; WORD prev_buttons; } slot[4];
static int g_p1 = -1, g_p2 = -1;        /* XInput slot of each player (-1 = none) */
static volatile int g_pad_mode = -1;    /* 1 = a controller was used last (mouse cursor hidden), 0 = mouse, -1 = not decided yet */
static void set_pad_mode(int on);
static DWORD g_p1_lost_at;
static DWORD g_last_poll;

static void xinput_load(void)
{
    const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !pXGet; i++) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) { pXGet = (t_XInputGetState)GetProcAddress(h, "XInputGetState");
                 pXSet = (t_XInputSetState)GetProcAddress(h, "XInputSetState");
                 if (pXGet) logf_("XInput: %s", dlls[i]); }
    }
    if (!pXGet) logf_("XInput: not available");
}

static void assign_players(void)
{
    DWORD now = GetTickCount();
    int oldp1 = g_p1, oldp2 = g_p2;
    if (g_p1 >= 0 && !slot[g_p1].connected) { g_p1 = -1; g_p1_lost_at = now; }
    if (g_p2 >= 0 && !slot[g_p2].connected) { g_p2 = -1; }
    for (int i = 0; i < 4; i++) {
        if (!slot[i].connected || i == g_p1 || i == g_p2) continue;
        if (g_p1 < 0) g_p1 = i;                 /* a new pad fills the empty Player 1 seat first (Ally hand-off) */
        else if (g_p2 < 0) g_p2 = i;
    }
    /* Player 1's pad gone for a while and nobody replaced it: Player 2 moves up to Player 1 */
    if (g_p1 < 0 && g_p2 >= 0 && now - g_p1_lost_at > 4000) { g_p1 = g_p2; g_p2 = -1; }
    if (g_p1 != oldp1 || g_p2 != oldp2) {
        logf_("players: P1 slot %d -> %d, P2 slot %d -> %d", oldp1, g_p1, oldp2, g_p2);
        if (g_p2 >= 0 && oldp2 < 0) notify_key(NK_PAD, "Player 2 controller connected!");
        if (g_p2 < 0 && oldp2 >= 0 && g_p1 != oldp2) notify_key(NK_PAD, "Player 2 controller disconnected.");
        if (g_p1 < 0 && oldp1 >= 0) notify_key(NK_PAD, "Player 1 controller disconnected - connect a controller to continue.");
    }
}

static void xinput_poll(void)
{
    DWORD now = GetTickCount();
    if (!pXGet || now - g_last_poll < 4) return;
    g_last_poll = now;
    for (int i = 0; i < 4; i++) {
        if (!slot[i].connected && now - slot[i].last_try < 1000) continue;  /* empty slots are slow to poll */
        slot[i].last_try = now;
        XSTATE s; DWORD r = pXGet(i, &s);
        int c = (r == ERROR_SUCCESS);
        if (c != slot[i].connected) logf_("XInput slot %d %s", i, c ? "connected" : "disconnected");
        slot[i].connected = c;
        if (c) {
            slot[i].prev_buttons = slot[i].st.Gamepad.wButtons; slot[i].st = s;
            XPAD *g = &s.Gamepad;
            if (g->wButtons || g->bLT > 60 || g->bRT > 60 || abs(g->sLX) > 12000 || abs(g->sLY) > 12000 ||
                abs(g->sRX) > 12000 || abs(g->sRY) > 12000) { if (g_pad_mode != 1) set_pad_mode(1); }
        }
        else memset(&slot[i].st, 0, sizeof slot[i].st);
    }
    if (g_pad_mode == -1 || (g_pad_mode == 0 && cfg.ignore_mouse)) { for (int i = 0; i < 4; i++) if (slot[i].connected) { set_pad_mode(1); break; } }
    assign_players();
}

static WORD pad_buttons(int p) { return (p >= 0 && slot[p].connected) ? slot[p].st.Gamepad.wButtons : 0; }
static int pad_pressed(int p, WORD b) /* edge since previous poll; consumed per caller via own latch */
{
    return (p >= 0 && slot[p].connected) && (slot[p].st.Gamepad.wButtons & b) && !(slot[p].prev_buttons & b);
}
static void pad_stick(int p, float *x, float *y)
{
    *x = *y = 0;
    if (p < 0 || !slot[p].connected) return;
    float lx = slot[p].st.Gamepad.sLX / 32767.0f, ly = slot[p].st.Gamepad.sLY / 32767.0f;
    WORD b = slot[p].st.Gamepad.wButtons;            /* the d-pad steers too (easier for little hands) */
    if (b & XB_LEFT) lx = -1; if (b & XB_RIGHT) lx = 1; if (b & XB_UP) ly = 1; if (b & XB_DOWN) ly = -1;
    float m = sqrtf(lx * lx + ly * ly);
    if (m < 0.30f) return;
    if (m > 1) { lx /= m; ly /= m; }
    *x = lx; *y = ly;
}

/* ------------------------------------------------------------------ co-op state */
enum { CTX_OTHER, CTX_SAIL, CTX_BATTLE, CTX_DUEL };
static int g_in_battle, g_in_duel;
static DWORD g_sail_seen;          /* last time the flagship's sailing update ran */
static const char *g_mode_str; static DWORD g_mode_seen;
static int g_menu_owner;           /* 0 = Player 1, 1 = Player 2 (town menus, dancing, sneaking...) */
static int g_duel_owner;
static int g_p2ship = 1;           /* fleet index (1..7) Player 2 drives */
static volatile DWORD g_world_moving_at;   /* PC7: last time an AI ship moved = the game is not paused */
static int g_coop_on, g_coop_on_prev = -1;
static DWORD g_packet;
static int g_two;                  /* PC7: 2 controllers + game live (duels/menus can be shared even with 1 ship) */
static int g_fleet_count;
static void wr(void *at, const void *src, int n);
static int g_game_live;            /* a game is loaded (we have been sailing since the last trip to the main menu) */
static DWORD g_back_down_since[2]; static int g_back_handled[2];
static DWORD g_owner_switch_at;    /* clears port input for a moment after a switch */
static int g_bfire_pending;
static volatile DWORD g_p2_rumble_until;     /* PC7: game rumble right after Player 2 fired goes to Player 2's pad */
static int g_duel_step0, g_duel_step1, g_duel_anim0; static DWORD g_duel_cool;
#define DUEL0_ANIM 0x72ccdcu
static int is_pushback_anim(int a)
{
    switch (a) { case 2: case 6: case 8: case 0x2b: case 0x2e: case 0x44: case 0x52:
                 case 0x19: case 0x1a: case 0x2a: case 0x2f: case 0x45: case 0x53: return 1; }
    return 0;
}
static DWORD g_leash_note_at, g_fleet_note_at;
static int g_sail_leashed, g_battle_p2_active;

static int fleet_count(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++) if (G(short, WSHIP_BASE + i * WSHIP_STRIDE + WS_TYPE) != -1) n++;
    return n;
}

static int ctx_now(void)
{
    if (g_in_duel) return CTX_DUEL;
    if (g_in_battle) return CTX_BATTLE;
    DWORD now = GetTickCount();
    if (now - g_mode_seen < 400) return (g_mode_str == (const char *)0x6fecfc) ? CTX_SAIL : CTX_OTHER;  /* "Sail" */
    if (now - g_sail_seen < 250) return CTX_SAIL;
    return CTX_OTHER;
}

/* who feeds the game's controller port (port 0): 0 = Player 1, 1 = Player 2 */
static int port_owner(void)
{
    if (!g_two) return 0;
    switch (ctx_now()) {
    case CTX_DUEL: return g_duel_owner;
    case CTX_SAIL: case CTX_BATTLE: return 0;   /* Player 2 has his own ship there */
    default: return g_menu_owner;
    }
}
static int owner_slot(void) { int o = port_owner(); return o ? g_p2 : g_p1; }

static void switch_owner(int ctx, int to, int by_mistake)
{
    g_owner_switch_at = GetTickCount();
    const char *who = to ? "Player 2" : "Player 1";
    if (ctx == CTX_DUEL) {
        g_duel_owner = to;
        if (by_mistake) notify_key(NK_DUEL, "Mistake! The sword passes to %s. (Tap SELECT to pass it.)", who);
        else notify_key(NK_DUEL, "%s has the sword now. (Tap SELECT to pass it.)", who);
    } else {
        g_menu_owner = to;
        notify_key(NK_CTRL, "%s has control of the menus. (Tap SELECT to pass control.)", who);
    }
}

/* hold BACK on either pad to pass control (duels, town menus, dancing, sneaking...) */
static void handle_back_button(void)
{
    DWORD now = GetTickCount();
    for (int pl = 0; pl < 2; pl++) {
        int s = pl ? g_p2 : g_p1;
        int down = (pad_buttons(s) & XB_BACK) != 0;
        if (!down) { g_back_down_since[pl] = 0; g_back_handled[pl] = 0; continue; }
        if (!g_back_down_since[pl]) {
            g_back_down_since[pl] = now;
            logf_("BACK pressed by Player %d (ctx %d, mode %s, coop %d) - never sent to the game", pl + 1, ctx_now(),
                  g_mode_str ? g_mode_str : "?", g_coop_on);
        }
        if (!g_two || g_back_handled[pl] || now - g_back_down_since[pl] < (DWORD)cfg.hold_back_ms) continue;
        g_back_handled[pl] = 1;
        int ctx = ctx_now();
        if (ctx == CTX_SAIL || ctx == CTX_BATTLE) continue;   /* at sea SELECT = change ship, nothing to pass */
        int cur = (ctx == CTX_DUEL) ? g_duel_owner : g_menu_owner;
        switch_owner(ctx, !cur, 0);
    }
}

static void pick_valid_p2ship(int advance)
{
    int start = g_p2ship;
    for (int k = 0; k < 8; k++) {
        int idx = 1 + ((start - 1 + (advance ? k + 1 : k)) % 7);
        if (G(short, WSHIP_BASE + idx * WSHIP_STRIDE + WS_TYPE) != -1) { g_p2ship = idx; return; }
    }
}

static void coop_update_state(void)
{
    g_fleet_count = fleet_count();
    int want = cfg.enabled && g_p1 >= 0 && g_p2 >= 0 && g_game_live;
    int enough = !cfg.require_two_ships || g_fleet_count >= 2;
    g_coop_on = want && enough;
    g_two = want;
    if (g_coop_on) pick_valid_p2ship(0);
    g_p2_ebpoff = g_coop_on ? (unsigned)g_p2ship * WSHIP_STRIDE : 0xffffffffu;
    if (g_coop_on != g_coop_on_prev) {
        if (g_coop_on) {
            notify_key(NK_COOP, "Co-op ON! Player 2 sails fleet ship #%d. SELECT = switch ship (each player), A = fire in battle.", g_p2ship);
        } else if (want && !enough) {
            notify_key(NK_COOP, "Co-op needs at least 2 ships in your fleet - capture or buy another ship to start!");
        } else if (g_coop_on_prev == 1) {
            if (want && !enough) notify_key(NK_COOP, "Co-op paused: you need at least 2 ships.");
            else notify_key(NK_COOP, "Co-op off (Player 2 controller not connected).");
        }
        if (!g_two) { g_menu_owner = 0; g_duel_owner = 0; }
        g_coop_on_prev = g_coop_on;
    } else if (want && !enough && ctx_now() == CTX_SAIL) {
        /* friendly reminder when Player 2 tries to play with only one ship */
        if (pad_buttons(g_p2) && GetTickCount() - g_fleet_note_at > 60000) {
            g_fleet_note_at = GetTickCount();
            notify_key(NK_COOP, "Player 2 can join when the fleet has at least 2 ships.");
        }
    }
}

/* Player 1 changes ship (game function 0x404e50 = rotate the fleet). Keep Player 2 on his own ship and
   never hand Player 2's ship to Player 1: if it would become the flagship, rotate once more. */
void __cdecl c_rotate_pre(void)
{
    g_rot_skip = g_rot_again = 0;
    if (!g_coop_on) return;
    if (fleet_count() <= 2 && g_p2ship == 1) {
        g_rot_skip = 1;
        notify_key(NK_SHIP, "Player 1: your only other ship is Player 2's.");
    }
}
void __cdecl c_rotate_post(void)
{
    if (!g_coop_on) return;
    if (g_p2ship == 1) g_rot_again = 1;          /* his ship just became the flagship: rotate again */
    else if (g_p2ship >= 2) g_p2ship--;
    logf_("fleet rotated: Player 2 ship now %d%s", g_p2ship, g_rot_again ? " (rotating again)" : "");
}
void __cdecl c_rotate_post2(void)
{
    int n = fleet_count();
    g_p2ship = n - 1 >= 1 ? n - 1 : 1;           /* after two turns his ship is the last one */
    logf_("fleet rotated twice: Player 2 ship now %d", g_p2ship);
}

/* duel: Player 1 / Player 2 take turns; getting pushed back = mistake = pass the sword */
static void duel_tick(void)
{
    if (!g_in_duel) return;
    int s0 = G(int, DUEL0_STEP), s1 = G(int, DUEL1_STEP), an = G(int, DUEL0_ANIM);
    DWORD now = GetTickCount();
    int fighting = (now - g_mode_seen < 400) && g_mode_str == (const char *)0x6fbe38;   /* "Duel" input is live */
    int pushed = fighting && ((is_pushback_anim(an) && !is_pushback_anim(g_duel_anim0)) || s0 > g_duel_step0);
    if (pushed && cfg.duel_autopass && g_two && now > g_duel_cool) {
        logf_("duel: human pushed back (anim %d->%d, step %d->%d), owner %d -> %d", g_duel_anim0, an, g_duel_step0, s0, g_duel_owner, !g_duel_owner);
        switch_owner(CTX_DUEL, !g_duel_owner, 1);
        g_duel_cool = now + 2000;
    }
    if (cfg.debug && (s0 != g_duel_step0 || s1 != g_duel_step1 || an != g_duel_anim0)) logf_("duel: steps %d %d anim %d", s0, s1, an);
    g_duel_step0 = s0; g_duel_step1 = s1; g_duel_anim0 = an;
}

/* PC13 "duel each other" (user item 8). The duel routine FUN_0041b1d0 already has a 2-human mode left over from the
   Xbox: for each fighter iVar4 (0 = you, 1 = enemy) it reads keys "like a human" when iVar4 == 0 OR byte [0x8ca91c]
   != 0 (0x41f4f2), from pad table [0x8caba8 + iVar4*4] (copied to [0x8c84dc] before FUN_0042e440). The PC code that
   would use [0x8cabac] (Player 2's port) for "reconnect the controller" is dead (xor eax,eax / je at 0x4f033a,
   0x4f03a8). So for a PvP duel: [0x8ca91c] = 1, [0x8cabac] = [0x8caba8] + 1 (next pad table), XInput port 1 marked
   present ([0x8ca338 + 100] = 1, so the game polls it through c_padfill) and c_padfill fills port 1 from the player
   who does NOT hold the sword. Everything is put back when the duel ends. */
static int g_pvp_on, g_pvp_s91c, g_pvp_sabac, g_pvp_s338, g_pvp_saba8;
static void pvp_off(void)
{
    if (!g_pvp_on) return;
    G(unsigned char, 0x8ca91c) = (unsigned char)g_pvp_s91c; G(int, 0x8cabac) = g_pvp_sabac; G(int, 0x8ca338 + 100) = g_pvp_s338;
    G(int, 0x8caba8) = g_pvp_saba8;
    g_pvp_on = 0; logf_("duel each other: off (game values restored)");
}
static void pvp_on(void)
{
    if (g_pvp_on || !cfg.duel_pvp || !g_two || g_p1 < 0 || g_p2 < 0) return;
    g_pvp_s91c = G(unsigned char, 0x8ca91c); g_pvp_sabac = G(int, 0x8cabac); g_pvp_s338 = G(int, 0x8ca338 + 100);
    int p0 = G(int, 0x8caba8); g_pvp_saba8 = p0;
    if (p0 < 0 || p0 > 4) { logf_("duel each other: unexpected pad index %d - not started", p0); return; }
    /* game pad tables: 0 = SUM of pads 1-4 (what Player 1 normally reads), 1..4 = XInput ports 0..3 only.
       Side 0 must read port 0 alone (else Player 2's port-1 input would move both captains): side 0 -> 1, side 1 -> 2 */
    G(int, 0x8ca338 + 100) = 1; G(int, 0x8caba8) = 1; G(int, 0x8cabac) = 2; G(unsigned char, 0x8ca91c) = 1;
    g_pvp_on = 1;
    logf_("duel each other: ON (pad tables: side 0 -> 1, side 1 -> 2; saved caba8 %d cabac %d 91c %d port1 %d)", p0, g_pvp_sabac, g_pvp_s91c, g_pvp_s338);
}
/* port 1 while a PvP duel runs: the player without the sword drives the enemy captain */
static void pvp_fill(unsigned char *st)
{
    int own = owner_slot(), o = (own == g_p1) ? g_p2 : g_p1;
    if (o < 0 || !slot[o].connected) { G(DWORD, st) = ++g_packet; return; }
    XPAD *g = &slot[o].st.Gamepad; WORD b = g->wButtons, w = 0;
    if (b & XB_UP) w |= 0x01; if (b & XB_DOWN) w |= 0x02; if (b & XB_LEFT) w |= 0x04; if (b & XB_RIGHT) w |= 0x08;
    if (b & XB_START) w |= 0x10;
    G(WORD, st + 4) = w;
    st[6] = (b & XB_A) ? 255 : 0; st[7] = (b & XB_B) ? 255 : 0; st[8] = (b & XB_X) ? 255 : 0; st[9] = (b & XB_Y) ? 255 : 0;
    st[10] = (b & XB_RB) ? 255 : 0; st[11] = (b & XB_LB) ? 255 : 0; st[12] = g->bLT; st[13] = g->bRT;
    G(SHORT, st + 0x0e) = g->sLX; G(SHORT, st + 0x10) = g->sLY; G(SHORT, st + 0x12) = g->sRX; G(SHORT, st + 0x14) = g->sRY;
    G(DWORD, st) = ++g_packet;
}
void __cdecl c_duel_begin(void)
{
    g_in_duel++;
    g_duel_owner = 0;
    g_duel_step0 = G(int, DUEL0_STEP); g_duel_step1 = G(int, DUEL1_STEP); g_duel_anim0 = G(int, DUEL0_ANIM);
    g_duel_cool = GetTickCount() + 2500;
    logf_("duel begin (coop %d)", g_coop_on);
    if (g_coop_on && cfg.duel_pvp) pvp_on();
    if (g_pvp_on) notify_key(NK_DUEL, "Sword fight: Player 1 vs Player 2! The player with the sword is your captain, the other one plays the enemy. SELECT swaps.");
    else if (g_coop_on) notify_key(NK_DUEL, cfg.duel_autopass ? "Sword fight! Player 1 starts. A mistake passes the sword to Player 2. Tap SELECT to pass it." : "Sword fight! Player 1 starts. Tap SELECT to pass the sword to Player 2.");
}
void __cdecl c_duel_end(void)
{
    if (g_in_duel > 0) g_in_duel--;
    g_duel_owner = 0;
    pvp_off();
    logf_("duel end");
}

static void hooks_watchdog(void);
/* The 2D layer is a 640x480 virtual screen drawn as quads through the 2D camera (0x8e9fe0, distance 640, frustum at
   +0x128: L,R,T,B). The game keeps R = 0.5 (640 wide) and cuts the height on 16:9 (T = 0.281 -> only y 60..420 of 480
   visible: menus, crew sign-up, HUD lose their top and bottom). Fit2D=1: T = 0.375 (all 480 rows) and R = T*W/H, so the
   whole 4:3 layout is shown unstretched and the extra width at the sides shows the scene behind; full-width 2D
   backgrounds are extended into the sides (hk_quad, mirrored, ExtendBackgrounds=1).
   IMPORTANT (PC3): the camera's culling planes (+0x18c..) and world-to-screen matrix are only rebuilt in the camera's
   UpdateWorldData, so after writing the frustum we rebuild them (0x5373f0 thiscall(cam, frustum), 0x5366d0 fastcall(cam)).
   PC2 did not: everything outside the old 0.281 view was culled (1680 checkbox missing, dialog box corners missing). */
typedef void (__attribute__((thiscall)) *t_camplanes)(void *cam, void *frustum);
typedef void (__fastcall *t_camw2s)(void *cam);
static void cam_refresh(unsigned cam)
{
    ((t_camplanes)0x5373f0)((void *)cam, (void *)(cam + 0x128));
    ((t_camw2s)0x5366d0)((void *)cam);
}
static DWORD g_fit_until; static int g_fit_on; static float g_fit_save[4];
volatile float g_2d_ext;               /* virtual 2D units visible beyond x=0 and x=640 on each side (0 = none) */
static void ui_fit_tick(void)
{
    unsigned cam = G(unsigned, 0x8e9fe0);
    if (cam < 0x10000 || IsBadReadPtr((void *)(cam + 0x128), 0x1c0 - 0x128)) { g_2d_ext = 0; return; }
    float *f = (float *)(cam + 0x128);
    int W = G(int, 0x8cac18), H = G(int, 0x8cac1c);
    int want = cfg.signup_fit == 2 || (cfg.signup_fit == 1 && (int)(g_fit_until - GetTickCount()) > 0);
    if (!want || W <= 0 || H <= 0 || W * 3 <= H * 4 + 8 || *((unsigned char *)cam + 0x140)) {
        if (g_fit_on) { memcpy(f, g_fit_save, 16); g_fit_on = 0; cam_refresh(cam); logf_("2D layer back to the game's own view"); }
        g_2d_ext = 0; return;
    }
    float t = 0.375f, r = t * W / H;
    if (g_fit_on && f[1] == r && f[2] == t && f[0] == -r && f[3] == -t) { g_2d_ext = 320.0f * (r / 0.5f) - 320.0f; return; }
    /* only touch the game's own 2D frustum (R 0.5, T 0.5*H/W) */
    if (fabsf(f[1] - 0.5f) < 0.002f && fabsf(f[0] + 0.5f) < 0.002f && fabsf(f[2] - 0.5f * H / W) < 0.004f) {
        if (!g_fit_on) { memcpy(g_fit_save, f, 16); g_fit_on = 1; logf_("2D layer fitted: frustum %.3f x %.3f -> %.3f x %.3f (whole 640x480 shown, culling rebuilt)", f[1], f[2], r, t); }
        f[0] = -r; f[1] = r; f[2] = t; f[3] = -t;
        cam_refresh(cam);
        g_2d_ext = 320.0f * (r / 0.5f) - 320.0f;
    } else g_2d_ext = 0;
}
static void cam_watch(void)
{
    static float last[8]; static unsigned lastcam; static int n;
    unsigned cam = G(unsigned, CAMERA_PTR);
    if (n > 60 || cam < 0x10000 || IsBadReadPtr((void *)(cam + 0x128), 0x30)) return;
    float *f = (float *)(cam + 0x128), *pt = (float *)(cam + 0x144), v[8] = { f[0], f[1], f[2], f[3], pt[0], pt[1], pt[2], pt[3] };
    if (cam != lastcam || memcmp(v, last, sizeof v)) {
        memcpy(last, v, sizeof v); lastcam = cam; n++;
        logf_("main camera %08x: frustum L%.3f R%.3f T%.3f B%.3f  port L%.3f R%.3f T%.3f B%.3f (ctx %d)", cam, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], ctx_now());
    }
}
/* PC5: renderer guard (stub_regobj, jmp at 0x54bc20). The PC3 crash 0x54bc49 = an object that passed the main
   camera's culling but has no render-state (+0xb4 = NULL). The wider sea view makes objects at the sides visible that
   the game itself never draws yet. Such an object is now skipped for that frame instead of crashing. */
volatile unsigned g_regskip_n, g_regskip_obj, g_regskip_ret;
static void regskip_report(void)
{
    static unsigned last; static int nlog;
    if (g_regskip_n != last && nlog < 20) {
        nlog++;
        unsigned o = g_regskip_obj, vt = (o > 0x10000 && !IsBadReadPtr((void *)o, 4)) ? G(unsigned, o) : 0;
        logf_("renderer guard: skipped %u object(s) with no render state so far (last %08x vtable %08x, from %08x, ctx %d mode %s) - this was the PC3 crash",
              g_regskip_n, o, vt, g_regskip_ret, ctx_now(), g_mode_str ? g_mode_str : "?");
        last = g_regskip_n;
    }
}
static void friendly_tick(int force);
static void coop_tick(void)
{
    regskip_report();
    friendly_tick(0);     /* PC9: mod menu "all nations friendly" */
    if (cfg.debug) cam_watch();
    static int loaded, last_ctx = -1;
    hooks_watchdog();
    ui_fit_tick();
    if (!loaded) { loaded = 1; xinput_load(); }
    xinput_poll();
    int c = ctx_now();
    if (c != last_ctx) {
        if (cfg.debug) logf_("context %d -> %d", last_ctx, c);
        if (c == CTX_SAIL || c == CTX_BATTLE) g_menu_owner = 0;   /* back at sea: Player 1 has the game's controls */
        if (c == CTX_SAIL && !g_game_live) { g_game_live = 1; logf_("game is live (sailing)"); }
        last_ctx = c;
    }
    coop_update_state();
    handle_back_button();
    duel_tick();
}

/* ================================================================== PC8: mod menu, sea-battle sails/zoom/fire fixes */
/* ---- game helpers (stubs.S) */
int game_addship(int src);              /* FUN_00404b70, EAX = source world ship: copies it into the first free fleet slot */
#define GOLD      0x869ab4u            /* int, gold pieces */
#define CREW      0x869ab0u            /* int, crew (total) */
#define FOOD      0x869ab8u            /* int, tons of food (first of 6 cargo ints 0x869ab8..0x869acc) */
#define ITEMS     0x869b20u            /* 17 bytes: personal items 0 none / 1 basic / 2 best (swords, pistols, vest, shirt...) */
#define SPECIAL   0x869b34u            /* byte: specialists on board (carpenter 1, sailmaker 2, cooper 4, gunner 8, ...) */
#define SHIPNAME_SRC 0x7267d0u          /* char* used by 0x404b70 as the new ship's name */
#define BATTLE_COLLIDE 0x8b99b0u        /* != 0 = the flagships are colliding -> boarding + sword fight after 0x1f frames */
#define B0_ADDR (BSHIP_BASE)
#define B1_ADDR (BSHIP_BASE + BSHIP_STRIDE)
#define BS_SAILTIMER 0x52               /* short: frames until the sails change (raise/lower in progress) */
int g_happy_on;                         /* read by stub_happy (0x404810 = crew mood 0..4) */
static volatile DWORD g_bfar_on; static float g_bfar_scale = 1.0f; static int g_sy_pending;
static volatile int g_bz_dir; static volatile DWORD g_bz_at; static float g_bz_t, g_bz_v;   /* PC9 gradual battle zoom */

static const struct { const char *name, *who; int type; } famous[] = {
    { "Queen Anne's Revenge", "Blackbeard",   24 },   /* Ship of the Line */
    { "Royal Fortune",        "Bart Roberts", 15 },   /* Large Frigate */
    { "Satisfaction",         "Henry Morgan", 25 },   /* Flag Galleon */
    { "Adventure Galley",     "Captain Kidd",  6 },   /* Frigate */
    { "Pride",                "Jean Lafitte", 20 },   /* Brig of War */
    { "Revenge",              "Stede Bonnet", 19 },   /* Royal Sloop */
    { "William",              "Jack Rackham", 10 },   /* Sloop of War */
    { "Nuestra Senora de Atocha", "treasure", 26 },   /* Treasure Galleon */
};
#define NFAMOUS ((int)(sizeof famous / sizeof famous[0]))
static const char *type_name(int t)
{
    static const char *n[27] = { "War Canoe", "Sloop", "Brigantine", "Coastal Barque", "Fluyt", "Merchantman", "Frigate", "Fast Galleon", "Trade Galleon",
        "Pinnace", "Sloop of War", "Brig", "Barque", "Large Fluyt", "Large Merchantman", "Large Frigate", "War Galleon", "Royal Galleon",
        "Mail Runner", "Royal Sloop", "Brig of War", "Ocean Barque", "West Indiaman", "East Indiaman", "Ship of the Line", "Flag Galleon", "Treasure Galleon" };
    return (t >= 0 && t < 27) ? n[t] : "?";
}

enum { MM_SPEED, MM_SHIP, MM_UPGR, MM_GOLD, MM_CREW, MM_HAPPY, MM_RAM, MM_ITEMS, MM_REPAIR, MM_FOOD, MM_SPEC, MM_FAME, MM_MAP, MM_FRIEND, MM_DIZZY, MM_PVP, MM_CLOSE, MM_N };
static volatile int g_mm_open; static int g_mm_sel, g_mm_slot = -1, g_mm_ship, g_mm_crew = 2, g_mm_paused_by_us;
static DWORD g_mm_next, g_mm_opened_at; static WORD g_mm_prev;
static char g_mm_msg[96]; static DWORD g_mm_msg_until;

/* PC9 mod menu extras.
   Locations: 128 entries of 0x20 bytes at 0x860b68 (+0 x, +4 y, +8 flags (bit 31 = unused slot, 0x101 = discovered,
   set by the game when you first sail near it), +0xc nation byte). First 44 = cities.
   Nation attitude toward the player: shorts 0x869a78 + n*2, n = 0..3 (Spain, England, France, Holland); negative =
   hostile (the game halves negatives over time). City attitude: shorts 0x85bf7c + city*0x94 (44 cities); the game
   treats city + nation < -1 as hostile. */
#define LOC_FLAGS(i)   (0x860b70u + (i) * 0x20u)
#define NATION_ATT(n)  (0x869a78u + (n) * 2u)
#define CITY_ATT(c)    (0x85bf7cu + (c) * 0x94u)
static int reveal_map(void)
{
    int n = 0;
    for (int i = 0; i < 128; i++) {
        unsigned f = G(unsigned, LOC_FLAGS(i));
        if ((int)f < 0) continue;                       /* unused slot */
        if ((f & 0x101) != 0x101) { G(unsigned, LOC_FLAGS(i)) = f | 0x101; n++; }
    }
    logf_("reveal map: %d places marked discovered", n);
    return n;
}
/* PC10: duel "Advantage" (-50..+50, local of FUN_0041b1d0). The human gets the dizzy rings + screen blur whenever it
   is below 0; the floor is -50 on every difficulty except the easiest (0). Patch at 0x41ea31 "and eax,0xffffffce"
   (floor = -50 & -(difficulty != 0)) -> "xor eax,eax; nop" = floor 0 like the easiest level: never dizzy. */
static void dizzy_apply(void)
{
    static const unsigned char orig[3] = { 0x83, 0xe0, 0xce }, patched[3] = { 0x31, 0xc0, 0x90 };
    unsigned char *p = (unsigned char *)0x41ea31;
    if (memcmp(p, orig, 3) && memcmp(p, patched, 3)) { logf_("never-dizzy patch: unexpected bytes at 0041ea31"); return; }
    wr(p, cfg.never_dizzy ? patched : orig, 3);
}
static void friendly_tick(int force)
{
    static DWORD last;
    g_friendly_on = cfg.friendly_nations;
    if (!cfg.friendly_nations || !g_game_live) return;
    if (!force && GetTickCount() - last < 1000) return;
    last = GetTickCount();
    int ch = 0;
    for (int k = 0; k < 4; k++) if (G(short, NATION_ATT(k)) < 3) { G(short, NATION_ATT(k)) = 3; ch++; }
    for (int c = 0; c < 44; c++) {
        if (G(short, CITY_ATT(c)) < 0) { G(short, CITY_ATT(c)) = 0; ch++; }
        /* PC10: +2 = the town's offense counter (raised when you attack it; the harbor batteries fire when
           (city + 5 + rank - difficulty) * 50 <= it, FUN_00410d30) -> cleared */
        if (G(short, CITY_ATT(c) + 2) > 0) { G(short, CITY_ATT(c) + 2) = 0; ch++; }
    }
    if (ch && force) logf_("friendly nations: %d attitudes raised", ch);
}

static void mm_line(int i, char *b, int n)
{
    switch (i) {
    case MM_SPEED: snprintf(b, n, "Ship speed:  < x%d >", cfg.speed_mult); break;
    case MM_SHIP:  snprintf(b, n, "Get ship:  < %s - %s (%s) >  A = add", famous[g_mm_ship].name, type_name(famous[g_mm_ship].type), famous[g_mm_ship].who); break;
    case MM_UPGR:  snprintf(b, n, "Unlock all ship upgrades (every ship)"); break;
    case MM_GOLD:  snprintf(b, n, "+10,000,000 gold   (now %d)", G(int, GOLD)); break;
    case MM_CREW:  snprintf(b, n, "Crew:  < %d >  A = set   (now %d)", (g_mm_crew + 1) * 100, G(int, CREW)); break;
    case MM_HAPPY: snprintf(b, n, "Crew always happy (no dividing the plunder):  %s", cfg.happy_crew ? "ON" : "OFF"); break;
    case MM_RAM:   snprintf(b, n, "Player 2 ramming the enemy starts the sword fight:  %s", cfg.p2_ram_duel ? "ON" : "OFF"); break;
    case MM_ITEMS: snprintf(b, n, "Unlock all sword-fight + personal items (best swords, brace of pistols...)"); break;
    case MM_REPAIR:snprintf(b, n, "Full repair (every ship)"); break;
    case MM_FOOD:  snprintf(b, n, "Refill food   (now %d tons)", G(int, FOOD)); break;
    case MM_SPEC:  snprintf(b, n, "All specialists on board (carpenter, cook, gunner...)"); break;
    case MM_FAME:  snprintf(b, n, "Max fame (%d points):  %s", cfg.fame_value, cfg.max_fame ? "ON" : "OFF"); break;
    case MM_MAP:   snprintf(b, n, "Reveal the whole map (every town, fort, haven, mission)"); break;
    case MM_FRIEND:snprintf(b, n, "All nations friendly (no harbor guns, every merchant trades):  %s", cfg.friendly_nations ? "ON" : "OFF"); break;
    case MM_DIZZY: snprintf(b, n, "Never dizzy in sword fights:  %s", cfg.never_dizzy ? "ON" : "OFF"); break;
    case MM_PVP:   snprintf(b, n, "Sword fights: players duel EACH OTHER (other player = enemy captain):  %s", cfg.duel_pvp ? "ON" : "OFF"); break;
    case MM_CLOSE: snprintf(b, n, "Close menu"); break;
    }
}
static void mm_say(const char *fmt, ...)
{
    va_list a; va_start(a, fmt); vsnprintf(g_mm_msg, sizeof g_mm_msg, fmt, a); va_end(a);
    g_mm_msg_until = GetTickCount() + 4000; logf_("mod menu: %s", g_mm_msg);
}
static int fleet_cargo_free(void)
{
    int cap = 0;
    for (int i = 0; i < 8; i++) { int t = G(short, WSHIP_BASE + i * WSHIP_STRIDE + WS_TYPE); if (t >= 0 && t < 27) cap += G(int, TYPE_STATS + t * 0x44 + 0x14); }
    int used = 0; for (int k = 0; k < 6; k++) used += G(int, FOOD + k * 4);
    return cap - used;
}
static void mm_open(int slot)
{
    if (g_mm_open) return;
    g_mm_open = 1; g_mm_slot = slot; g_mm_opened_at = GetTickCount(); g_mm_next = 0; g_mm_prev = pad_buttons(slot);
    g_mm_paused_by_us = 0;
    int c = ctx_now();
    if (g_game_live && (c == CTX_SAIL || c == CTX_BATTLE) && !(G(unsigned, GAME_FLAGS) & 4)) { G(unsigned, GAME_FLAGS) |= 4; g_mm_paused_by_us = 1; }
    logf_("mod menu opened by slot %d (ctx %d, paused by us %d)", slot, ctx_now(), g_mm_paused_by_us);
}
static void mm_close(void)
{
    if (!g_mm_open) return;
    g_mm_open = 0;
    if (g_mm_paused_by_us) G(unsigned, GAME_FLAGS) &= ~4u;
    g_owner_switch_at = GetTickCount();      /* let go of held buttons before the game sees the pad again */
    logf_("mod menu closed");
}
static void mm_activate(int i, int dir)
{
    int live = g_game_live, n = 0;
    switch (i) {
    case MM_SPEED:
        cfg.speed_mult = 1 + ((cfg.speed_mult - 1 + (dir ? dir : 1) + 3) % 3);
        ini_save_int("ModMenu", "ShipSpeed", cfg.speed_mult); mm_say("Ship speed x%d", cfg.speed_mult); break;
    case MM_SHIP: {
        if (dir) { g_mm_ship = (g_mm_ship + dir + NFAMOUS) % NFAMOUS; return; }
        if (!live) { mm_say("Start or load a game first."); break; }
        if (ctx_now() == CTX_BATTLE || g_in_battle) { mm_say("Add ships while sailing (not during a battle)."); break; }
        short before[8]; for (int k = 0; k < 8; k++) before[k] = G(short, WSHIP_BASE + k * WSHIP_STRIDE + WS_TYPE);
        static struct { int len; char s[40]; } nm; lstrcpynA(nm.s, famous[g_mm_ship].name, sizeof nm.s); nm.len = lstrlenA(nm.s);
        char *old = G(char *, SHIPNAME_SRC);
        G(char *, SHIPNAME_SRC) = nm.s;
        int ok = game_addship(0);
        G(char *, SHIPNAME_SRC) = old;
        int j = -1; for (int k = 1; k < 8; k++) if (before[k] == -1 && G(short, WSHIP_BASE + k * WSHIP_STRIDE + WS_TYPE) != -1) { j = k; break; }
        if (!ok || j < 0) { mm_say("Your fleet is full (8 ships)."); break; }
        unsigned w = WSHIP_BASE + j * WSHIP_STRIDE; int t = famous[g_mm_ship].type; unsigned st = TYPE_STATS + t * 0x44;
        G(short, w + WS_TYPE) = (short)t;
        G(short, w + WS_GUNS) = (short)G(int, st + 0x10);
        G(short, w + WS_CREW) = (short)(G(int, st + 0x0c) / 2);
        G(short, w + 0x20) = 0; G(short, w + 0x22) = 0;        /* sail + hull damage */
        G(short, w + WS_UPGR) |= 0xff;                         /* every upgrade */
        mm_say("Added the %s '%s' to your fleet (ship #%d).", type_name(t), famous[g_mm_ship].name, j + 1);
        break; }
    case MM_UPGR:
        for (int k = 0; k < 8; k++) { unsigned w = WSHIP_BASE + k * WSHIP_STRIDE; if (G(short, w + WS_TYPE) != -1) { G(short, w + WS_UPGR) |= 0xff; n++; } }
        if (g_in_battle) { G(int, B0_ADDR + BS_UPGR) |= 0xff; if (G(unsigned, B1_ADDR + BS_FLAGS) & 2) G(int, B1_ADDR + BS_UPGR) |= 0xff; }
        mm_say("All upgrades on %d ship%s (copper, cotton sails, hammocks, scantlings, chain + grape shot, powder, bronze cannon).", n, n == 1 ? "" : "s"); break;
    case MM_GOLD:
        if (G(int, GOLD) < 2000000000 - 10000000) G(int, GOLD) += 10000000;
        mm_say("Gold: %d", G(int, GOLD)); break;
    case MM_CREW:
        if (dir) { g_mm_crew = (g_mm_crew + dir + 5) % 5; return; }
        G(int, CREW) = (g_mm_crew + 1) * 100; G(short, WSHIP_BASE + WS_CREW) = (short)((g_mm_crew + 1) * 100);
        if (g_in_battle) G(int, B0_ADDR + BS_CREW) = (g_mm_crew + 1) * 100;
        mm_say("Crew set to %d.", (g_mm_crew + 1) * 100); break;
    case MM_HAPPY:
        cfg.happy_crew = !cfg.happy_crew; g_happy_on = cfg.happy_crew; ini_save_int("ModMenu", "CrewAlwaysHappy", cfg.happy_crew);
        mm_say("Crew always happy: %s", cfg.happy_crew ? "ON" : "OFF"); break;
    case MM_RAM:
        cfg.p2_ram_duel = !cfg.p2_ram_duel; ini_save_int("ModMenu", "Player2RamStartsSwordFight", cfg.p2_ram_duel);
        mm_say("Player 2 ramming starts the sword fight: %s", cfg.p2_ram_duel ? "ON" : "OFF"); break;
    case MM_ITEMS:
        for (int k = 0; k < 17; k++) G(unsigned char, ITEMS + k) = 2;
        mm_say("All personal items at their best (perfectly balanced swords, brace of pistols, cuirass, silk shirt...)."); break;
    case MM_REPAIR:
        for (int k = 0; k < 8; k++) { unsigned w = WSHIP_BASE + k * WSHIP_STRIDE; if (G(short, w + WS_TYPE) != -1) { G(short, w + 0x20) = 0; G(short, w + 0x22) = 0; n++; } }
        if (g_in_battle) for (int k = 0; k < 2; k++) { unsigned b = BSHIP_BASE + k * BSHIP_STRIDE; if (G(unsigned, b + BS_FLAGS) & 2) { G(int, b + BS_HULL) = 0; G(int, b + BS_MORALE) = 0; } }
        mm_say("Repaired %d ship%s.", n, n == 1 ? "" : "s"); break;
    case MM_FOOD: {
        int fr = fleet_cargo_free(), need = (G(int, CREW) + 1) * 12 / 20 + 1, f = G(int, FOOD);
        if (fr > 0) f += fr;
        if (f < need) f = need;                       /* at least 12 months for the whole crew */
        G(int, FOOD) = f; mm_say("Food: %d tons.", f); break; }
    case MM_SPEC:
        G(unsigned char, SPECIAL) = 0xff; mm_say("Carpenter, sailmaker, cooper, gunner, surgeon, navigator, quartermaster and cook are on board."); break;
    case MM_FAME:
        cfg.max_fame = !cfg.max_fame; g_fame_on = cfg.max_fame; ini_save_int("ModMenu", "MaxFame", cfg.max_fame);
        mm_say("Max fame: %s", cfg.max_fame ? "ON" : "OFF"); break;
    case MM_MAP:
        if (!live) { mm_say("Start or load a game first."); break; }
        n = reveal_map(); mm_say("Map revealed: %d new place%s.", n, n == 1 ? "" : "s"); break;
    case MM_FRIEND:
        cfg.friendly_nations = !cfg.friendly_nations; ini_save_int("ModMenu", "FriendlyNations", cfg.friendly_nations);
        g_friendly_on = cfg.friendly_nations;
        if (cfg.friendly_nations && live) friendly_tick(1);
        mm_say("All nations friendly: %s", cfg.friendly_nations ? "ON" : "OFF"); break;
    case MM_DIZZY:
        cfg.never_dizzy = !cfg.never_dizzy; dizzy_apply(); ini_save_int("ModMenu", "NeverDizzyInSwordFights", cfg.never_dizzy);
        mm_say("Never dizzy in sword fights: %s", cfg.never_dizzy ? "ON" : "OFF"); break;
    case MM_PVP:
        cfg.duel_pvp = !cfg.duel_pvp; ini_save_int("ModMenu", "DuelEachOther", cfg.duel_pvp);
        mm_say(cfg.duel_pvp ? "Sword fights: Player 1 vs Player 2 (from the next sword fight)" : "Sword fights: both players against the enemy again"); break;
    case MM_CLOSE: mm_close(); break;
    }
}
/* called from c_padfill every poll while the menu is open: the opener's pad drives it, the game gets nothing */
static void mm_input(void)
{
    int s = g_mm_slot; if (s < 0 || !slot[s].connected) s = g_p1;
    WORD b = pad_buttons(s);
    if (cfg.debug) {   /* test hooks: I/K up/down, J/N left/right, O = A, U = B */
        if (GetAsyncKeyState('I') & 0x8000) b |= XB_UP; if (GetAsyncKeyState('K') & 0x8000) b |= XB_DOWN;
        if (GetAsyncKeyState('J') & 0x8000) b |= XB_LEFT; if (GetAsyncKeyState('N') & 0x8000) b |= XB_RIGHT;
        if (GetAsyncKeyState('O') & 0x8000) b |= XB_A; if (GetAsyncKeyState('U') & 0x8000) b |= XB_B;
    }
    if (s >= 0 && slot[s].connected) {
        SHORT ly = slot[s].st.Gamepad.sLY, lx = slot[s].st.Gamepad.sLX;
        if (ly > 16000) b |= XB_UP; if (ly < -16000) b |= XB_DOWN; if (lx < -16000) b |= XB_LEFT; if (lx > 16000) b |= XB_RIGHT;
    }
    WORD fresh = b & ~g_mm_prev; DWORD now = GetTickCount();
    WORD dirs = b & (XB_UP | XB_DOWN | XB_LEFT | XB_RIGHT);
    int rep = 0;
    if (dirs && !(fresh & dirs)) { if (g_mm_next && (int)(now - g_mm_next) >= 0) { rep = 1; g_mm_next = now + 140; } }
    else if (fresh & dirs) g_mm_next = now + 400;
    WORD go = fresh | (rep ? dirs : 0);
    g_mm_prev = b;
    if (now - g_mm_opened_at < 250) return;                 /* the D-pad UP that opened it */
    if (go & XB_UP) g_mm_sel = (g_mm_sel + MM_N - 1) % MM_N;
    if (go & XB_DOWN) g_mm_sel = (g_mm_sel + 1) % MM_N;
    int lr = (go & XB_LEFT) ? -1 : (go & XB_RIGHT) ? 1 : 0;
    if (lr && (g_mm_sel == MM_SPEED || g_mm_sel == MM_SHIP || g_mm_sel == MM_CREW)) mm_activate(g_mm_sel, lr);
    else if (lr && (fresh & (XB_LEFT | XB_RIGHT)) && (g_mm_sel == MM_HAPPY || g_mm_sel == MM_RAM)) mm_activate(g_mm_sel, 0);
    if (fresh & XB_A) mm_activate(g_mm_sel, 0);
    if (fresh & (XB_B | XB_START | XB_BACK)) mm_close();
}

/* ------------------------------------------------------------------ controller port feed (replaces FUN_004f0ba0) */
/* g_packet: declared near the top (PC13) */
static WORD g_rumble_l, g_rumble_r; static int g_rumble_slot = -1;
static HWND g_hwnd;
static volatile int g_osk_active;          /* on-screen keyboard (name entry) owns the controllers */
#define MOUSE_X   0x8dc30cu                /* game's mouse position (virtual 2D coords) */
#define MOUSE_Y   0x8dc310u
#define ZOOM      0x85a0fcu                /* camera zoom step: smaller = closer. PC keys +/- and wheel: -13..10 */

/* controller in use: park the game's mouse at (0,-1) so it never steals a menu highlight
   (menus re-read "what is under the mouse" whenever that position changes; the generic menu starts
   with its last-seen position at (0,0) and compares against y+1, so (0,-1) means "unchanged") */
/* World map screen (input mode "Map", FUN_004461c0) scrolls when the game mouse is within 2 units of a screen edge.
   With a controller connected the mouse is ignored there and kept in the middle (PC4, user request). */
static int any_pad(void) { for (int i = 0; i < 4; i++) if (slot[i].connected) return 1; return 0; }
static volatile DWORD g_mapquad_at;      /* PC7: last time the world-map picture (quad from 0x445d30) was drawn */
static int map_screen(void)
{
    if (GetTickCount() - g_mapquad_at < 300) return 1;   /* the map opened from sailing runs in input mode "Sail" */
    return g_mode_str && GetTickCount() - g_mode_seen < 400 && !IsBadReadPtr(g_mode_str, 4) && !strcmp(g_mode_str, "Map");
}
/* PC10: land battle (input mode "Fight", FUN_00438e30) scrolls its camera while the game mouse is within 2-3 units
   of a screen edge (x<=2, x>=637, y<3, y>=477). The old park spot (0,-1) is such an edge -> the camera ran off to
   the ocean by itself. Now: mouse in the middle, the right stick (or the left stick while A is not held) of either
   player pushes it to that edge = camera scrolls. */
static int fight_mode(void)
{
    const char *m = g_mode_str;
    return m && GetTickCount() - g_mode_seen < 1000 && !IsBadReadPtr(m, 6) && !memcmp(m, "Fight", 6);
}
static void park_mouse(void)
{
    if ((map_screen() || fight_mode()) && any_pad()) { G(int, MOUSE_X) = 320; G(int, MOUSE_Y) = 240; return; }
    if (g_pad_mode == 1 && cfg.hide_cursor) { G(int, MOUSE_X) = 0; G(int, MOUSE_Y) = -1; }
}

/* D-pad left/right = zoom out/in while sailing and in sea battles (the Xbox game has no zoom in battles) */
static void dpad_zoom(WORD b)
{
    static DWORD next; DWORD now = GetTickCount();
    int dir = (b & XB_LEFT) ? 1 : (b & XB_RIGHT) ? -1 : 0;
    if (!dir) { next = 0; return; }
    if (next && (int)(now - next) < 0) return;
    next = now + (next ? (DWORD)cfg.zoom_repeat_ms : 250u);   /* first step, short pause, then repeat */
    int z = G(int, ZOOM) + dir;
    if (z < -13) z = -13; if (z > 10) z = 10;
    G(int, ZOOM) = z;
}

static void sails_press(void);
/* PC11 (user: "P1 can't always switch cannon ammo with X"). The game's Battle key mapper (FUN_0042ceb0) returns ONE key
   per frame in a fixed order START, A, B, X, ...: while A's "pressed" window (or B's) is still open, X is never seen, and
   when X's own short window ends first the press is lost. Player 1's X/B now cycle the shot type directly, exactly
   like Player 2's (round -> chain -> grape -> round, only shot types the ship's upgrades allow), and are not passed
   to the game. Player 2's code is unchanged. */
static void shot_cycle(unsigned me, unsigned other, const char *who)
{
    unsigned up = G(unsigned, me + BS_UPGR) | G(unsigned, other + BS_UPGR);
    int st0 = G(int, me + BS_SHOT), nx = 0;
    if (st0 == 0) nx = (up & 0x10) ? 1 : (up & 0x20) ? 2 : 0;
    else if (st0 == 1) nx = (up & 0x20) ? 2 : 0;
    else nx = 0;
    if (st0 == 3) { logf_("%s shot type: not changed (shot field 3)", who); return; }
    if (nx == st0) { notify_key(NK_SHIP, "%s: round shot only - buy chain/grape shot at a shipwright", who); logf_("%s shot type stays %d (upgrades %x)", who, st0, up); return; }
    G(int, me + BS_SHOT) = nx;
    static const char *nm[] = { "round shot", "chain shot", "grape shot" };
    notify_key(NK_SHIP, "%s: %s", who, nm[nx]);
    logf_("%s shot type %d -> %d (upgrades %x)", who, st0, nx, up);
}
int __cdecl c_padfill(unsigned char *st)
{
    coop_tick();
    int port = ((unsigned)st - 0x8ca33cu) / 100u;
    memset(st, 0, 0x18);
    if (port == 1 && g_pvp_on) { if (!g_in_duel) pvp_off(); else pvp_fill(st); return 1; }   /* PC13 */
    if (port != 0) return 1;
    int ctx = ctx_now();
    /* controller in use: park the game's mouse at (0,-1) in menus so it never steals the highlight
       (every menu re-reads "what is under the mouse" when that position changes) */
    if (ctx == CTX_OTHER) park_mouse();
    int s = owner_slot();
    if (ctx == CTX_OTHER && fight_mode() && any_pad() && !g_mm_open) {
        float bx = 0, by = 0;
        for (int i = 0; i < 4; i++) if (slot[i].connected) {
            XPAD *q = &slot[i].st.Gamepad; float v;
            v = q->sRX / 32767.0f; if (fabsf(v) > fabsf(bx)) bx = v;  v = q->sRY / 32767.0f; if (fabsf(v) > fabsf(by)) by = v;
            if (!(q->wButtons & XB_A)) {
                v = q->sLX / 32767.0f; if (fabsf(v) > fabsf(bx)) bx = v;  v = q->sLY / 32767.0f; if (fabsf(v) > fabsf(by)) by = v;
            }
        }
        int mx = 320, my = 240;
        if (bx > 0.5f) mx = 639; else if (bx < -0.5f) mx = 0;
        if (by > 0.5f) my = 0; else if (by < -0.5f) my = 479;
        G(int, MOUSE_X) = mx; G(int, MOUSE_Y) = my;
        static int logged; if (!logged) { logged = 1; logf_("land battle: mouse kept in the middle, sticks scroll the camera"); }
    }
    /* PC8: mod menu (RB + D-pad UP, either player, in game). While it is open the game gets no controller input. */
    if (g_mm_open) { mm_input(); G(DWORD, st) = ++g_packet; return 1; }
    if (!g_osk_active) {
        static WORD rbprev[2];
        for (int pl = 0; pl < 2; pl++) {
            int ps = pl ? g_p2 : g_p1; WORD pb = pad_buttons(ps);
            if (cfg.debug && pl == 0 && (GetAsyncKeyState('M') & 0x8000)) pb |= XB_RB | XB_UP;    /* test hook: M = RB + UP */
            if ((pb & XB_RB) && (pb & XB_UP) && !((rbprev[pl] & XB_RB) && (rbprev[pl] & XB_UP)) && ps >= 0) {
                rbprev[pl] = pb; mm_open(ps); G(DWORD, st) = ++g_packet; return 1;
            }
            rbprev[pl] = pb;
        }
    }
    /* PC7: world map (also when opened from sailing): mouse parked in the middle; either stick of either player
       pushes the pointer to that screen edge so the map scrolls (the game scrolls when the mouse is at an edge).
       The sticks themselves are not given to the game there (no hidden steering / pointer drift). */
    if (map_screen() && any_pad()) {
        float bx = 0, by = 0;
        for (int i = 0; i < 4; i++) if (slot[i].connected) {
            XPAD *q = &slot[i].st.Gamepad; float v;
            v = q->sLX / 32767.0f; if (fabsf(v) > fabsf(bx)) bx = v;  v = q->sRX / 32767.0f; if (fabsf(v) > fabsf(bx)) bx = v;
            v = q->sLY / 32767.0f; if (fabsf(v) > fabsf(by)) by = v;  v = q->sRY / 32767.0f; if (fabsf(v) > fabsf(by)) by = v;
        }
        int mx = 320, my = 240;
        if (bx > 0.5f) mx = 639; else if (bx < -0.5f) mx = 0;
        if (by > 0.5f) my = 0; else if (by < -0.5f) my = 479;
        G(int, MOUSE_X) = mx; G(int, MOUSE_Y) = my;
        if (s >= 0 && slot[s].connected && !g_osk_active) {
            XPAD *g = &slot[s].st.Gamepad; WORD b = g->wButtons, w = 0;
            if (b & XB_UP) w |= 0x01; if (b & XB_DOWN) w |= 0x02; if (b & XB_LEFT) w |= 0x04; if (b & XB_RIGHT) w |= 0x08;
            if (b & XB_START) w |= 0x10;
            G(WORD, st + 4) = w;
            st[6] = (b & XB_A) ? 255 : 0; st[7] = (b & XB_B) ? 255 : 0; st[8] = (b & XB_X) ? 255 : 0; st[9] = (b & XB_Y) ? 255 : 0;
            st[10] = (b & XB_RB) ? 255 : 0; st[11] = (b & XB_LB) ? 255 : 0;
            st[12] = g->bLT; st[13] = g->bRT;      /* PC11: LT/RT = page left/right in the status book (was missing -> stuck on the map page) */
            { static int tl; int t = g->bLT > 100 || g->bRT > 100; if (t && !tl) logf_("map/status page: %s passed to the game", g->bLT > 100 ? "LT" : "RT"); tl = t; }
        }
        G(DWORD, st) = ++g_packet;
        return 1;
    }
    if (s < 0 || !slot[s].connected || g_osk_active) { G(DWORD, st) = ++g_packet; return 1; }
    if (GetTickCount() - g_owner_switch_at < 250) { G(DWORD, st) = ++g_packet; return 1; } /* let go of held buttons */
    XPAD *g = &slot[s].st.Gamepad;
    WORD b = g->wButtons, w = 0;
    /* PC8: RB is held back while it is down (RB + D-pad UP = mod menu); a plain RB press reaches the game when it is
       let go (map / chase view a fraction later). D-pad UP while RB is down never reaches the game. */
    {
        static int rbheld, rbcombo; static DWORD rbpulse;
        DWORD now = GetTickCount();
        if (b & XB_RB) { if (!rbheld) { rbheld = 1; rbcombo = 0; } if (b & XB_UP) rbcombo = 1; b &= ~(XB_RB | XB_UP); }
        else if (rbheld) { rbheld = 0; if (!rbcombo) rbpulse = now + 150; }
        if ((int)(rbpulse - now) > 0) b |= XB_RB;
    }
    /* PC8: sea battle - D-pad UP = far view (FarZoomPercent of the game's farthest), D-pad DOWN = the game's normal view.
       Y = sails (the game's own key, watched by sails_tick so it always works). */
    if (ctx == CTX_BATTLE && !(G(unsigned, GAME_FLAGS) & 4)) {
        if (cfg.debug) { if (GetAsyncKeyState('I') & 0x8000) b |= XB_UP; if (GetAsyncKeyState('K') & 0x8000) b |= XB_DOWN; }  /* test hooks */
        static WORD bprev; WORD fr = b & ~bprev; bprev = b;
        (void)fr;
        /* PC9: HOLD D-pad UP = camera slowly rises (zoom out), HOLD DOWN = slowly back to the game's normal height;
           it stays where it is let go. c_bcam does the movement every camera frame. */
        g_bz_dir = (b & XB_UP) ? 1 : (b & XB_DOWN) ? -1 : 0; g_bz_at = GetTickCount();
        b &= ~(XB_UP | XB_DOWN);
        static int ylatch; int yd = (b & XB_Y) != 0;
        if (cfg.debug && (GetAsyncKeyState('J') & 0x8000)) { yd = 1; b |= XB_Y; }    /* test hook: J = pad Y */
        if (yd && !ylatch && s == g_p1) sails_press();
        ylatch = yd;
        if (cfg.p1_shot_mod && s == g_p1) {               /* PC11: P1 X (or B) = next shot type, never lost */
            static int xlatch1; int xd = (b & (XB_X | XB_B)) != 0;
            if (xd && !xlatch1 && (G(unsigned, B0_ADDR + BS_FLAGS) & 2)) shot_cycle(B0_ADDR, B1_ADDR, "Player 1");
            xlatch1 = xd;
            b &= ~(XB_X | XB_B);
        }
    }
    /* PC7 (user): B while sailing = change view (the game's keyboard 'v' = toggle chase view; on the Xbox it was
       A + a trigger, which nobody finds). Sent to the game window as a real key press. */
    {
        static int blatch; int bdown = (b & XB_B) != 0;
        if (cfg.debug && (GetAsyncKeyState('K') & 0x8000)) bdown = 1;          /* test hook: keyboard K = pad B */
        if (ctx == CTX_SAIL && s == g_p1 && bdown && !blatch && !(G(unsigned, GAME_FLAGS) & 4) && g_hwnd) {
            PostMessageA(g_hwnd, WM_KEYDOWN, 'V', 0x002f0001);
            PostMessageA(g_hwnd, WM_KEYUP, 'V', 0xc02f0001);
            logf_("B while sailing: change view");
        }
        blatch = bdown;
        if (ctx == CTX_SAIL) b &= ~XB_B;
    }
    if (cfg.dpad_zoom && (ctx == CTX_SAIL || ctx == CTX_BATTLE) && !(G(unsigned, GAME_FLAGS) & 4)) {
        dpad_zoom(b);
        b &= ~(XB_LEFT | XB_RIGHT);
    }
    if (b & XB_UP) w |= 0x01; if (b & XB_DOWN) w |= 0x02; if (b & XB_LEFT) w |= 0x04; if (b & XB_RIGHT) w |= 0x08;
    if (b & XB_START) w |= 0x10;
    if (b & XB_LS) w |= 0x40; if (b & XB_RS) w |= 0x80;
    if ((b & XB_BACK) && ctx == CTX_SAIL && s == g_p1) b |= XB_Y;   /* SELECT while sailing = next ship (the game's Y) */
    /* BACK (0x20) is never given to the game: no Xbox screen uses it, it only becomes stray key code 0x85.
       PC1 crashed when it reached the game while sailing. BACK is only the mod's "pass control" button. */
    G(WORD, st + 4) = w;
    /* original Xbox analog buttons: A B X Y BLACK WHITE LT RT  (360 pads: BLACK = RB, WHITE = LB, like 360 back-compat) */
    st[6]  = (b & XB_A) ? 255 : 0;
    st[7]  = (b & XB_B) ? 255 : 0;
    st[8]  = (b & XB_X) ? 255 : 0;
    st[9]  = (b & XB_Y) ? 255 : 0;
    st[10] = (b & XB_RB) ? 255 : 0;
    st[11] = (b & XB_LB) ? 255 : 0;
    st[12] = g->bLT; st[13] = g->bRT;
    G(SHORT, st + 0x0e) = g->sLX; G(SHORT, st + 0x10) = g->sLY;
    G(SHORT, st + 0x12) = g->sRX; G(SHORT, st + 0x14) = g->sRY;
    G(DWORD, st) = ++g_packet;
    /* rumble requested by the game for port 0 -> the pad that has control */
    if (cfg.rumble && pXSet) {
        WORD l = G(WORD, 0x8ca398), r = G(WORD, 0x8ca39a);
        if (l != g_rumble_l || r != g_rumble_r || s != g_rumble_slot) {
            if (g_rumble_slot >= 0 && g_rumble_slot != s) { XVIB z = {0, 0}; pXSet(g_rumble_slot, &z); }
            int rs = s;
            if (ctx == CTX_BATTLE && g_coop_on && g_p2 >= 0 && GetTickCount() < g_p2_rumble_until) rs = g_p2;
            if (rs != s) { XVIB v = { l, r }; pXSet(rs, &v); g_rumble_l = l; g_rumble_r = r; g_rumble_slot = rs; goto rumble_done; }
            XVIB v = { l, r }; pXSet(s, &v);
            g_rumble_l = l; g_rumble_r = r; g_rumble_slot = s;
        }
    }
rumble_done:
    if (ctx == CTX_BATTLE && cfg.debug) {   /* PC7: Player 1's A in battle - why no shot? */
        static int la; int a1 = st[6] != 0;
        if (a1 && !la) logf_("P1 A in battle: guns loaded %d, hull %d, shot %d, paused %d", G(int, BSHIP_BASE + BS_LOADED), G(int, BSHIP_BASE + BS_HULL), G(int, BSHIP_BASE + BS_SHOT), (G(unsigned, GAME_FLAGS) & 4) != 0);
        la = a1;
    }
    return 1;
}

void __cdecl c_padmap_pre(const char *mode)
{
    if (mode && (unsigned)mode > 0x400000u && (unsigned)mode < 0x900000u) {
        if (!strcmp(mode, "Shell")) {
            if (g_in_duel || g_in_battle) { g_in_duel = g_in_battle = 0; g_battle_p2_active = 0; }
            if (g_game_live) { g_game_live = 0; logf_("main menu: game not live"); }
        }
        if (g_mode_str != mode && cfg.debug) logf_("input mode: %s", mode);
        g_mode_str = mode; g_mode_seen = GetTickCount();
        if (!strcmp(mode, "Map") && any_pad()) {
            static DWORD noted; if (GetTickCount() - noted > 5000) logf_("map screen: mouse ignored (controller connected)");
            noted = GetTickCount();
            G(int, MOUSE_X) = 320; G(int, MOUSE_Y) = 240;
            SetCursor(NULL);
        }
    }
}

/* PC15 (user 13 Nov: "on land battles make the D-pad choose the direction of the army's movement without moving the
   camera"). The land battle key mapper FUN_0042cf90 (input mode "Fight") gives the army-move keys only while A is held
   with the left stick: 0x126 up, 0x128 down, 0x125 left, 0x127 right, 0x124 up-left, 0x123 down-left, 0x121
   up-right, 0x122 down-right (same codes the duel uses for the arrows). The camera only follows the sticks (mouse at
   the screen edge, see fight_mode / c_padfill), so the D-pad never moved it. Now, in "Fight" mode, while the D-pad of
   the player in charge is held, the mapper's answer is replaced by the matching move key, every frame, no A needed. */
int __cdecl c_padmap_post(int key)
{
    if (!cfg.fight_dpad || !fight_mode() || g_mm_open || g_osk_active) return key;
    int s = owner_slot(); if (s < 0) return key;
    WORD b = pad_buttons(s);
    int u = (b & XB_UP) != 0, d = (b & XB_DOWN) != 0, l = (b & XB_LEFT) != 0, r = (b & XB_RIGHT) != 0;
    if (u && d) u = d = 0; if (l && r) l = r = 0;
    int k = 0;
    if (u) k = l ? 0x124 : r ? 0x121 : 0x126;
    else if (d) k = l ? 0x123 : r ? 0x122 : 0x128;
    else if (l) k = 0x125;
    else if (r) k = 0x127;
    if (!k) return key;
    static int last; if (k != last && cfg.debug) logf_("land battle: D-pad -> army move key %03x (game key was %x)", k, key);
    last = k;
    return k;
}

/* ------------------------------------------------------------------ camera-relative stick -> world direction */
static int stick_world_dir(float sx, float sy, float *wx, float *wy)
{
    float *m = 0;
    unsigned cam = G(unsigned, CAMERA_PTR);
    if (cam < 0x10000) return 0;
    if (IsBadReadPtr((void *)(cam + 0x38), 36)) return 0;
    m = (float *)(cam + 0x38);
    /* Gamebryo camera: column 0 = view direction, column 1 = up, column 2 = right */
    float fx = m[0], fy = m[3], ux = m[1], uy = m[4], rx = m[2], ry = m[5];
    float gx = fx + ux, gy = fy + uy;               /* "screen up" laid on the sea */
    float gl = sqrtf(gx * gx + gy * gy), rl = sqrtf(rx * rx + ry * ry);
    if (!(gl > 0.05f) || !(rl > 0.05f)) return 0;
    gx /= gl; gy /= gl; rx /= rl; ry /= rl;
    *wx = sx * rx + sy * gx; *wy = sx * ry + sy * gy;
    return 1;
}

/* camera -> normalized screen position (-1..1 = on screen). Gamebryo NiAVObject: rotate +0x38, translate +0x5c;
   NiCamera frustum (left,right,top,bottom) found once by pattern. */
static int g_frustum_off = -1; static unsigned g_frustum_cam;
static int screen_pos(float px, float py, float pz, float *sx, float *sy)
{
    unsigned cam = G(unsigned, CAMERA_PTR);
    if (cam < 0x10000 || IsBadReadPtr((void *)cam, 0x200)) return 0;
    float *m = (float *)(cam + 0x38), *t = (float *)(cam + 0x5c);
    if (g_frustum_cam != cam) {
        g_frustum_cam = cam; g_frustum_off = -1;
        for (int o = 0x6c; o < 0x1f0; o += 4) {
            float *f = (float *)(cam + o);
            if (f[0] < -0.05f && f[0] > -5 && fabsf(f[0] + f[1]) < 1e-4f && f[2] > 0.05f && f[2] < 5 && fabsf(f[2] + f[3]) < 1e-4f) {
                g_frustum_off = o; break;
            }
        }
        logf_("camera %08x: frustum %s at +0x%x", cam, g_frustum_off >= 0 ? "found" : "NOT found", g_frustum_off);
    }
    if (g_frustum_off < 0) return 0;
    float *fr = (float *)(cam + g_frustum_off);
    float dx = px - t[0], dy = py - t[1], dz = pz - t[2];
    float depth = dx * m[0] + dy * m[3] + dz * m[6];
    if (!(depth > 0.001f)) { *sx = *sy = 9; return 2; }
    float u = dx * m[1] + dy * m[4] + dz * m[7], r = dx * m[2] + dy * m[5] + dz * m[8];
    *sx = (r / depth) / fr[1]; *sy = (u / depth) / fr[2];
    if (!(fabsf(*sx) < 100) || !(fabsf(*sy) < 100)) { *sx = *sy = 9; }
    return 1;
}
/* how far a point is from the screen centre: 0 = centre, 1.0 = screen edge, >1 = off screen; -1 = no camera */
static float screen_edge(float px, float py, float pz)
{
    float sx, sy; int r = screen_pos(px, py, pz, &sx, &sy);
    if (!r) return -1;
    return fabsf(sx) > fabsf(sy) ? fabsf(sx) : fabsf(sy);
}

/* Babysitter: Player 2 is completely free while his ship is ON screen. Only when it goes OFF screen the
   babysitter points it back at Player 1 (and keeps up with him) until it is well back on screen.
   *state: 0 free, 1 being brought back. Returns 1 while bringing back. */
static int babysit(int *state, float p2edge, float p1edge, int dist, int nocam_limit, int *note_once)
{
    int cam_ok = p2edge >= 0 && p1edge >= 0 && p1edge < 0.95f;   /* the camera really shows Player 1 */
    if (cam_ok) {
        if (!*state && p2edge > cfg.edge_off / 100.0f) *state = 1;
        else if (*state && p2edge < cfg.edge_back / 100.0f) *state = 0;
    } else {
        /* camera unknown (other screen): only a very large distance counts */
        if (!*state && dist > nocam_limit) *state = 1;
        else if (*state && dist < nocam_limit / 3) *state = 0;
    }
    if (*state && !*note_once) {
        *note_once = 1;
        if (GetTickCount() - g_leash_note_at > 30000) {
            g_leash_note_at = GetTickCount();
            notify_key(NK_LEASH, "Player 2 sailed off the screen - bringing him back to Player 1!");
        }
    }
    if (!*state) *note_once = 0;
    return *state;
}

/* heading (game angle) for the stick, or 0 if neutral. *ok=0 when the camera can't be read */
static unsigned stick_heading(int s, unsigned cur_heading, int *active)
{
    float sx, sy; pad_stick(s, &sx, &sy);
    *active = (sx != 0 || sy != 0);
    if (!*active) return cur_heading;
    float wx, wy;
    if (cfg.absolute_steer && stick_world_dir(sx, sy, &wx, &wy)) {
        return (unsigned)game_atan2((int)(wx * 10000), (int)(wy * 10000));
    }
    /* fallback: Xbox-style, left/right turns the ship */
    if (fabsf(sx) < 0.3f) { *active = (sy > 0.3f); return cur_heading; }
    return cur_heading + (sx > 0 ? 0x20000000u : 0xe0000000u);
}

static int oct_dist(int dx, int dy) /* the game's distance metric */
{
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    int d = (dx <= dy) ? dx + dy * 2 : dy + dx * 2;
    return d / 2;
}

/* ------------------------------------------------------------------ sailing: Player 2's fleet ship */
static unsigned g_sail_desired; static int g_sail_override; static int g_last_speed0 = 200;

static int sail_dist_metric(int k)
{
    unsigned b0 = WSHIP_BASE, bk = WSHIP_BASE + k * WSHIP_STRIDE;
    int dx = G(int, b0 + WS_X) - G(int, bk + WS_X), dy = G(int, b0 + WS_Y) - G(int, bk + WS_Y);
    int scale = G(int, 0x725678); if (scale < 1) scale = 1;
    return (oct_dist(dx, dy) / scale) * 2;
}

unsigned __cdecl c_fleet_heading(unsigned ebpoff, unsigned angle)
{
    if (!g_coop_on || ebpoff != (unsigned)g_p2ship * WSHIP_STRIDE) return angle;
    if (g_sail_override) return g_sail_desired;
    return angle;
}

/* PC12 (user photo 12 Nov: Player 2's ship sailed deep into the jungle and froze there). The game land-tests only the
   flagship's next position (FUN_00467f90 ~0x46914a: FUN_004e7510(x/1000, y/1000, 1) [AL=1, EDI=[0x72cd78]],
   FUN_005f0ddc(), then FUN_004e7760(x, y, 0x725618, 0) [EAX=[0x72cd78]]; non-zero = land). Fleet ships normally just
   follow in formation, so nothing stopped Player 2's free steering. land_at() (stubs.S) repeats that exact test.
   After each of Player 2's sailing steps: new spot on land -> back to the old spot (he stops at the coast like Player 1).
   Already inland, or stopped at the coast while the babysitter brings him home (2 s) / while steering into it (8 s)
   -> moved to open water next to Player 1. */
int land_at(float x, float y);
void *p_land1 = (void *)0x4e7510, *p_land2 = (void *)0x5f0ddc, *p_land3 = (void *)0x4e7760;
static int p2_rescue(unsigned bk)
{
    unsigned b0 = WSHIP_BASE;
    int x0 = G(int, b0 + WS_X), y0 = G(int, b0 + WS_Y);
    int scale = G(int, 0x725678); if (scale < 1) scale = 1;
    double h = G(unsigned, b0 + WS_HEAD) * (6.283185307179586 / 4294967296.0);
    static const int ang[] = { 180, 140, 220, 100, 260, 60, 300, 0 };
    for (int d = 1; d <= 3; d++)
        for (int i = 0; i < 8; i++) {
            double a = h + ang[i] * 3.14159265358979 / 180.0;
            double r = (double)scale * 60000.0 * d;      /* = sail_dist_metric 120000 per step (near Player 1, on screen) */
            int x = x0 + (int)(sin(a) * r), y = y0 + (int)(cos(a) * r);
            if (!land_at(x / 1000.0f, y / 1000.0f)) {
                G(int, bk + WS_X) = x; G(int, bk + WS_Y) = y; G(unsigned, bk + WS_HEAD) = G(unsigned, b0 + WS_HEAD);
                return 1;
            }
        }
    G(int, bk + WS_X) = x0; G(int, bk + WS_Y) = y0;   /* no water found around: on Player 1's own (water) spot */
    return 0;
}
/* PC14: R6025 "pure virtual function call" 12 Nov 20:54 right as a screen change started (black quad 0x4d3ed1).
   The game only runs this land test when it is safe: not if [0x85a164] & 0x480 (scene change / special states, it
   returns early for the player's ships) or & 4 (paused), and only for a ship whose short +0x5c (0x814354) is 0.
   Calling it during a scene change asked the half-destroyed map object -> pure virtual call. Same guards now. */
static int land_safe(unsigned bk)
{
    unsigned fl = G(unsigned, GAME_FLAGS);
    if (fl & 0x484) return 0;
    if (G(short, bk + 0x5c) != 0) return 0;
    unsigned t = G(unsigned, 0x72cd78);
    if (t < 0x10000 || IsBadReadPtr((void *)t, 8) || IsBadReadPtr((void *)G(unsigned, t), 0x40)) return 0;
    return ctx_now() == CTX_SAIL;
}
static void p2_land_after(int idx, int px, int py, int active, int homing)
{
    static DWORD blocked_since; static int n_stop, n_res;
    unsigned bk = WSHIP_BASE + idx * WSHIP_STRIDE; DWORD now = GetTickCount();
    if (!land_safe(bk) || !land_safe(WSHIP_BASE)) { blocked_since = 0; return; }
    int x = G(int, bk + WS_X), y = G(int, bk + WS_Y);
    if (!land_at(x / 1000.0f, y / 1000.0f)) { if (x != px || y != py) blocked_since = 0; return; }
    int inland = land_at(px / 1000.0f, py / 1000.0f);
    if (!inland) {
        G(int, bk + WS_X) = px; G(int, bk + WS_Y) = py;
        if (!blocked_since) { blocked_since = now; if (n_stop < 20) { n_stop++; logf_("land: Player 2's ship stopped at the coast (pos %d,%d)%s", px, py, homing ? " while being brought back" : ""); } }
        if (now - blocked_since < (DWORD)(homing && !active ? 2000 : 8000)) return;
    }
    int ok = p2_rescue(bk);
    blocked_since = 0;
    if (n_res < 20) { n_res++; logf_("land: Player 2's ship %s (was %d,%d) -> moved to %s next to Player 1", inland ? "was ON LAND" : "kept running into the coast", x, y, ok ? "open water" : "Player 1's spot"); }
    notify_key(NK_SHIP, "Player 2's ship was stuck at the land - it is back next to Player 1.");
}
static unsigned __cdecl hk_sailship(int idx)
{
    if (idx == 0) {
        g_sail_seen = GetTickCount();
        if ((g_in_duel || g_in_battle) && GetTickCount() - g_mode_seen < 400 && g_mode_str == (const char *)0x6fecfc) { logf_("sailing again: clearing stale duel/battle flags (%d/%d)", g_in_duel, g_in_battle); g_in_duel = g_in_battle = 0; g_battle_p2_active = 0; }
        if (cfg.debug) {
            static DWORD t; static int px, py;
            if (GetTickCount() - t > 3000) {
                t = GetTickCount();
                int x = G(int, WSHIP_BASE + WS_X), y = G(int, WSHIP_BASE + WS_Y);
                unsigned cam = G(unsigned, CAMERA_PTR);
                float *m = (cam > 0x10000 && !IsBadReadPtr((void *)(cam + 0x38), 36)) ? (float *)(cam + 0x38) : 0;
                logf_("sail P1: pos %d,%d move %d,%d heading %08x speed %d fleet %d | cam fwd %.2f,%.2f,%.2f up %.2f,%.2f,%.2f right %.2f,%.2f,%.2f",
                      x, y, x - px, y - py, G(unsigned, WSHIP_BASE + WS_HEAD), G(int, WSHIP_BASE + WS_SPEED), g_fleet_count,
                      m ? m[0] : 0, m ? m[3] : 0, m ? m[6] : 0, m ? m[1] : 0, m ? m[4] : 0, m ? m[7] : 0, m ? m[2] : 0, m ? m[5] : 0, m ? m[8] : 0);
                float sx = 0, sy = 0; int sr = screen_pos(x / 1000.0f, y / 1000.0f, 0, &sx, &sy);
                unsigned c2 = G(unsigned, CAMERA_PTR);
                if (c2 > 0x10000 && !IsBadReadPtr((void *)(c2 + 0x5c), 12))
                    logf_("   cam pos %.1f,%.1f,%.1f  P1 on screen %d: %.2f,%.2f  (P1 render %.1f,%.1f)", G(float, c2 + 0x5c), G(float, c2 + 0x60), G(float, c2 + 0x64), sr, sx, sy, x / 1000.0f, y / 1000.0f);
                px = x; py = y;
            }
        }
        int sp = G(int, WSHIP_BASE + WS_SPEED);
        if (sp > 50) g_last_speed0 = sp;
    }
    if (!g_coop_on || idx != g_p2ship || idx < 1 || idx > 7) return o_sailship(idx);

    unsigned bk = WSHIP_BASE + idx * WSHIP_STRIDE, b0 = WSHIP_BASE;
    /* Player 2 picks another ship with Y */
    static int ylatch;
    int y = (pad_buttons(g_p2) & (XB_Y | XB_BACK)) != 0;      /* SELECT (or Y) = next ship */
    if (y && !ylatch) {
        int before = g_p2ship; pick_valid_p2ship(1);
        if (g_p2ship != before) notify_key(NK_SHIP, "Player 2 now sails fleet ship #%d.", g_p2ship);
        else notify_key(NK_SHIP, "Player 2: there is no other ship to switch to.");
        ylatch = 1; return o_sailship(idx);
    }
    ylatch = y;

    int dist = sail_dist_metric(idx);
    int active; unsigned cur = G(unsigned, bk + WS_HEAD);
    unsigned want = stick_heading(g_p2, cur, &active);
    float edge = screen_edge(G(int, bk + WS_X) / 1000.0f, G(int, bk + WS_Y) / 1000.0f, 0);
    float p1edge = screen_edge(G(int, b0 + WS_X) / 1000.0f, G(int, b0 + WS_Y) / 1000.0f, 0);
    static int leash_state, leash_note;
    int leash = babysit(&leash_state, edge, p1edge, dist, cfg.sail_leash_full, &leash_note);
    unsigned to_p1 = (unsigned)game_atan2((G(int, b0 + WS_X) - G(int, bk + WS_X)) / 1000, (G(int, b0 + WS_Y) - G(int, bk + WS_Y)) / 1000);
    /* PC6 (user): Player 2 is free everywhere. Hands off the stick = his ship keeps its own course. Only after
       IdleSecondsBeforeReturn (20 s) without touching the stick, or when it goes off screen, it is brought back. */
    static DWORD last_touch; static unsigned hold_course; DWORD now = GetTickCount();
    if (active || !last_touch) { last_touch = now; hold_course = want; }
    int idle_back = !active && cfg.idle_sec > 0 && now - last_touch > (DWORD)cfg.idle_sec * 1000;
    if (leash) { g_sail_override = 1; g_sail_desired = to_p1; g_sail_leashed = 1; hold_course = to_p1; }
    else if (active) { g_sail_override = 1; g_sail_desired = want; g_sail_leashed = 0; }
    else if (!idle_back) { g_sail_override = 1; g_sail_desired = hold_course; g_sail_leashed = 0; }  /* keeps his course */
    else { g_sail_override = 0; g_sail_leashed = 0; }   /* idle 20 s: ship follows Player 1 like normal */

    unsigned slow = G(unsigned, SAIL_SLOWDOWN);
    int px = G(int, bk + WS_X), py = G(int, bk + WS_Y);
    unsigned r = o_sailship(idx);
    G(unsigned, SAIL_SLOWDOWN) = slow;        /* Player 2 never makes Player 1 wait ("Wait for me!") */
    if (cfg.land_guard && GetTickCount() - g_world_moving_at < 400) p2_land_after(idx, px, py, active, leash || idle_back);   /* PC12 */

    if (g_sail_override) {
        int base = G(int, b0 + WS_SPEED); if (base < g_last_speed0 / 2) base = g_last_speed0 / 2;
        int pct = g_sail_leashed ? cfg.p2_catchup_pct : cfg.p2_speed_pct;
        int sp = base * pct / 100;
        if (G(int, bk + WS_SPEED) < sp || !g_sail_leashed) G(int, bk + WS_SPEED) = sp;   /* never slower than Player 1 */
    }
    if (cfg.debug) { static DWORD t; if (GetTickCount() - t > 2000) { t = GetTickCount();
        logf_("sail P2 ship %d dist %d edge %.2f (P1 %.2f) leash %d active %d idle %lus%s speed %d/%d", idx, dist, edge, p1edge, leash, active, (now - last_touch) / 1000, idle_back ? " (returning)" : "", G(int, bk + WS_SPEED), G(int, b0 + WS_SPEED)); } }
    return r;
}

/* ------------------------------------------------------------------ sea battle: Player 2's ship = side 0, ship 1 */
#define B0 (BSHIP_BASE)
#define B1 (BSHIP_BASE + BSHIP_STRIDE)
static int g_bship_cur = -1, g_b_k = -1, g_b_type = -1, g_battle_leash;

static void battle_add_p2(void)
{
    g_battle_p2_active = 0; g_b_k = -1;
    if (!g_coop_on) return;
    int k = g_p2ship; unsigned wk = WSHIP_BASE + k * WSHIP_STRIDE;
    int type = G(short, wk + WS_TYPE);
    if (type < 0) return;
    if (G(unsigned, B1 + BS_FLAGS) & 2) { logf_("battle: side0 ship1 already used, not adding P2"); return; }
    if (!(G(unsigned, B0 + BS_FLAGS) & 2)) { logf_("battle: flagship slot not active?"); return; }
    unsigned stats = TYPE_STATS + type * 0x44;
    memset((void *)B1, 0, BSHIP_STRIDE);
    unsigned h0 = G(unsigned, B0 + BS_HEAD);
    int x0 = G(int, B0 + BS_X), y0 = G(int, B0 + BS_Y);
    int R = 150000, placed = 0;
    unsigned tries[4] = { h0 + 0x40000000u, h0 + 0xc0000000u, h0 + 0x80000000u, h0 };
    for (int t = 0; t < 4 && !placed; t++) {
        int x = x0 + game_trig(R, tries[t]), y = y0 + game_trig(R, tries[t] + 0x40000000u);
        int rx = (x / 100) / 4 + G(int, BATTLE_ORIGX), ry = (y / 100) / 4 + G(int, BATTLE_ORIGY);
        if (!game_battle_blocked(rx, ry)) { G(int, B1 + BS_X) = x; G(int, B1 + BS_Y) = y; placed = 1; }
    }
    if (!placed) { logf_("battle: no water next to the flagship for Player 2"); return; }
    int maxguns = G(int, stats + 0x10), maxcrew = G(int, stats + 0x0c);
    int guns = G(short, wk + WS_GUNS); if (guns <= 0 || guns > maxguns) guns = maxguns;
    int crew = G(short, wk + WS_CREW); if (crew < maxcrew / 2) crew = maxcrew / 2; if (crew < 8) crew = 8;
    G(int, B1 + BS_TYPE) = type;
    G(unsigned, B1 + BS_HEAD) = h0;
    G(int, B1 + BS_SPEED) = 300;
    G(int, B1 + BS_GUNS) = guns; G(int, B1 + BS_LOADED) = guns;
    G(int, B1 + BS_CREW) = crew;
    G(int, B1 + BS_SAILQ) = G(short, wk + WS_SAILQ);
    G(int, B1 + BS_HULL) = G(short, wk + WS_HULL);
    G(int, B1 + BS_MORALE) = G(short, wk + WS_MORALE);
    G(int, B1 + BS_UPGR) = G(short, wk + WS_UPGR);
    G(unsigned, B1 + BS_FLAGS) = 2 | 1;      /* active, full sails */
    g_b_k = k; g_b_type = type; g_battle_p2_active = 1;
    logf_("battle: Player 2 joins with fleet ship %d type %d guns %d crew %d", k, type, guns, crew);
    notify_key(NK_BATTLE, "Player 2 joins the battle! Player 2: steer with the stick, A = fire cannons.");
}

static void battle_remove_p2(void)
{
    if (!g_battle_p2_active) return;
    unsigned wk = WSHIP_BASE + g_b_k * WSHIP_STRIDE;
    int hull = G(int, B1 + BS_HULL);
    if (G(short, wk + WS_TYPE) == g_b_type) {
        if (hull >= 100) hull = 90;            /* little captains don't lose their ship */
        if (hull < 0) hull = 0;
        G(short, wk + WS_HULL) = (short)hull;
        logf_("battle: Player 2 ship %d hull now %d", g_b_k, hull);
    }
    g_battle_p2_active = 0;
}

static int __cdecl hk_battle(int real)
{
    g_in_battle++;
    g_menu_owner = 0;
    g_bfar_on = 0; g_bfar_scale = 1.0f; g_sy_pending = 0; g_bz_dir = 0; g_bz_v = 0;   /* g_bz_t kept: height stays */
    if (real) battle_add_p2();
    int r = o_battle(real);
    battle_remove_p2();
    if (g_in_battle > 0) g_in_battle--;
    return r;
}

float __cdecl hk_axis(int code, const char *mode)
{
    if (g_bship_cur != 1 || !g_battle_p2_active || !g_coop_on) return o_axis(code, mode);
    /* Player 2's ship: steer by pointing the stick (babysitter keeps him near Player 1) */
    unsigned h = G(unsigned, B1 + BS_HEAD);
    int dx = G(int, B0 + BS_X) - G(int, B1 + BS_X), dy = G(int, B0 + BS_Y) - G(int, B1 + BS_Y);
    int dist = oct_dist(dx, dy);
    int active; unsigned want = stick_heading(g_p2, h, &active);
    unsigned to_p1 = (unsigned)game_atan2(dx / 100, dy / 100);
    float edge = screen_edge((G(int, B1 + BS_X) / 100) / 4.0f + G(int, BATTLE_ORIGX), (G(int, B1 + BS_Y) / 100) / 4.0f + G(int, BATTLE_ORIGY), 0);
    float p1edge = screen_edge((G(int, B0 + BS_X) / 100) / 4.0f + G(int, BATTLE_ORIGX), (G(int, B0 + BS_Y) / 100) / 4.0f + G(int, BATTLE_ORIGY), 0);
    static int bstate, bnote;
    int leash = babysit(&bstate, edge, p1edge, dist, cfg.battle_leash_full, &bnote);
    g_battle_leash = leash ? 2 : 0;
    /* PC6: hands off = keeps course; after IdleSecondsBeforeReturn without the stick, sail back next to Player 1 */
    static DWORD blast; DWORD bnow = GetTickCount();
    if (active || !blast) blast = bnow;
    if (!leash && !active && cfg.idle_sec > 0 && bnow - blast > (DWORD)cfg.idle_sec * 1000 && dist > 300000) { want = to_p1; active = 1; g_battle_leash = 2; }
    if (leash) { want = to_p1; active = 1; }
    if (active) { if (want == 0) want = 1; G(unsigned, B1 + BS_TARGETHEAD) = want; }
    return 0.0f;
}

static void sails_tick(void); static void sails_note(int full, const char *who); static void p2_ram_tick(void);
static int fire_core(int side, unsigned ship, int x, int y, int report);
static int __cdecl hk_bship(int side, int ship, int flag)
{
    g_bship_cur = side * 4 + ship;
    int r = o_bship(side, ship, flag);
    g_bship_cur = -1;
    if (side == 0 && ship == 0) sails_tick();
    if (side == 0 && ship == 1 && g_battle_p2_active && g_coop_on) {
        /* PC8: Player 2's Y = his sails (full / reduced) */
        static int ylatch2; int y2 = (pad_buttons(g_p2) & XB_Y) != 0;
        if (y2 && !ylatch2 && !(G(unsigned, GAME_FLAGS) & 4)) {
            unsigned f1 = G(unsigned, B1 + BS_FLAGS) ^ 1u; G(unsigned, B1 + BS_FLAGS) = f1; G(short, B1 + BS_SAILTIMER) = 0;
            sails_note(f1 & 1, "Player 2"); logf_("P2 sails -> %s", (f1 & 1) ? "FULL" : "reduced");
        }
        ylatch2 = y2;
        p2_ram_tick();
        int dist = oct_dist(G(int, B0 + BS_X) - G(int, B1 + BS_X), G(int, B0 + BS_Y) - G(int, B1 + BS_Y));
        if (g_battle_leash == 2) {                        /* catch up with Player 1 */
            int want = G(int, B0 + BS_SPEED) * cfg.p2_catchup_pct / 100;
            if (G(int, B1 + BS_SPEED) < want) G(int, B1 + BS_SPEED) = want;
        }
        if (cfg.debug) { static DWORD t; if (GetTickCount() - t > 3000) { t = GetTickCount();
            float e2 = screen_edge((G(int, B1 + BS_X) / 100) / 4.0f + G(int, BATTLE_ORIGX), (G(int, B1 + BS_Y) / 100) / 4.0f + G(int, BATTLE_ORIGY), 0);
            float e1 = screen_edge((G(int, B0 + BS_X) / 100) / 4.0f + G(int, BATTLE_ORIGX), (G(int, B0 + BS_Y) / 100) / 4.0f + G(int, BATTLE_ORIGY), 0);
            logf_("battle P2: edge %.2f (P1 %.2f) leash %d dist %d speed %d/%d hull %d loaded %d heading %08x", e2, e1, g_battle_leash, dist, G(int, B1 + BS_SPEED), G(int, B0 + BS_SPEED),
                  G(int, B1 + BS_HULL), G(int, B1 + BS_LOADED), G(unsigned, B1 + BS_HEAD)); } }
        /* PC7: Player 2 changes his shot type with X (or B): round -> chain -> grape, like the game's 'q' key */
        static int xlatch;
        int xb = (pad_buttons(g_p2) & (XB_X | XB_B)) != 0;
        if (xb && !xlatch && !(G(unsigned, GAME_FLAGS) & 4)) {
            unsigned up = G(unsigned, B1 + BS_UPGR) | G(unsigned, B0 + BS_UPGR);
            int st0 = G(int, B1 + BS_SHOT), nx = 0;
            if (st0 == 0) nx = (up & 0x10) ? 1 : (up & 0x20) ? 2 : 0;
            else if (st0 == 1) nx = (up & 0x20) ? 2 : 0;
            else nx = 0;
            if (st0 != 3 && nx != st0) {
                G(int, B1 + BS_SHOT) = nx;
                static const char *nm[] = { "round shot", "chain shot", "grape shot" };
                notify_key(NK_SHIP, "Player 2: %s", nm[nx]);
                logf_("P2 shot type %d -> %d (upgrades %x)", st0, nx, up);
            }
        }
        xlatch = xb;
        static int alatch;
        int a = (pad_buttons(g_p2) & (XB_A | XB_RB | XB_LB)) != 0;     /* A (or a shoulder button) fires */
        if (a && !alatch) g_bfire_pending = 1;
        alatch = a;
        if (g_bfire_pending) {
            g_bfire_pending = 0;
            if (!(G(unsigned, GAME_FLAGS) & 4) && G(int, B1 + BS_LOADED) != 0 && G(int, B1 + BS_HULL) < 100 &&
                G(int, B1 + BS_SHOT) != 3 && (G(unsigned, B1 + BS_FLAGS) & 2)) {
                g_p2_rumble_until = GetTickCount() + 700;
                fire_core(0, 1, -1, -1, 1);
                if (cfg.debug) logf_("P2 fires");
            }
        }
    }
    return r;
}


/* ------------------------------------------------------------------ PC8: sea battle fixes */
/* Sails (Y). The game: Y = key '/', which starts a sail change only when the ship's sail timer (+0x52) is 0; the timer
   = hull class * 1000 / (crew + 1) frames for raising (250 for lowering) and the flag (+0x64 bit 1) flips when it runs
   out. With a small crew that is many seconds, and the timer only runs while some damage limits allow it, so Y often
   seemed to do nothing. Now: the timer is capped (SailChangeMaxFrames), a timer that stops moving or a press the game
   ignored is completed by the mod, and a note says what happened. */
static int g_sy_flag0, g_sy_last_t; static DWORD g_sy_at, g_sy_t_at;
static void sails_press(void)
{
    if (!(G(unsigned, B0_ADDR + BS_FLAGS) & 2)) return;
    g_sy_pending = 1; g_sy_at = GetTickCount(); g_sy_flag0 = G(unsigned, B0_ADDR + BS_FLAGS) & 1;
    g_sy_last_t = -1; g_sy_t_at = g_sy_at;
    logf_("P1 Y (sails) in battle: sails %s, timer %d, crew %d, hull %d, sail damage %d", g_sy_flag0 ? "FULL" : "reduced",
          G(short, B0_ADDR + BS_SAILTIMER), G(int, B0_ADDR + BS_CREW), G(int, B0_ADDR + BS_HULL), G(int, B0_ADDR + BS_MORALE));
}
static void sails_note(int full, const char *who) { notify_key(NK_BATTLE, "%s: %s", who, full ? "FULL sails" : "sails reduced (battle sails)"); }
static void sails_tick(void)
{
    if (!g_sy_pending) return;
    DWORD now = GetTickCount(); unsigned fl = G(unsigned, B0_ADDR + BS_FLAGS); int t = G(short, B0_ADDR + BS_SAILTIMER);
    if (G(unsigned, GAME_FLAGS) & 4) { g_sy_t_at = now; g_sy_at = now; return; }        /* paused: wait */
    if ((int)(fl & 1) != g_sy_flag0) { g_sy_pending = 0; sails_note(fl & 1, "Player 1"); logf_("sails changed by the game"); return; }
    if (t > cfg.sail_delay_max && cfg.sail_delay_max > 0) { G(short, B0_ADDR + BS_SAILTIMER) = (short)cfg.sail_delay_max; t = cfg.sail_delay_max; }
    if (t != g_sy_last_t) { g_sy_last_t = t; g_sy_t_at = now; }
    int force = 0;
    if (t == 0 && now - g_sy_at > 300) force = 1;                  /* the game ignored the press */
    if (t != 0 && now - g_sy_t_at > 600) force = 2;                /* the timer stopped running */
    if (now - g_sy_at > 2500) force = 3;
    if (force) {
        G(short, B0_ADDR + BS_SAILTIMER) = 0;
        G(unsigned, B0_ADDR + BS_FLAGS) = (fl & ~1u) | (g_sy_flag0 ? 0 : 1);
        g_sy_pending = 0; sails_note(!g_sy_flag0, "Player 1");
        logf_("sails changed by the mod (%s, timer %d)", force == 1 ? "game ignored the press" : force == 2 ? "timer stuck" : "timeout", t);
    }
}

/* Firing. FUN_00477cb0 does not fire at all when the side's OTHER ship (Player 2 for Player 1, and the other way round)
   is active and lies within 30 degrees of the target's direction - however far away it is. That was "A with 14 guns
   loaded and no shot": Player 2's ship was somewhere behind the target. Now the friend only blocks the shot when he is
   closer than the nearest enemy (really in the line of fire); then a note tells why nobody fired. */
static int nearest_enemy_dist(unsigned me)
{
    int best = 0x7fffffff;
    for (int k = 0; k < 4; k++) {
        unsigned e = BSHIP_BASE + 0x12a0 + k * BSHIP_STRIDE;
        if (!(G(unsigned, e + BS_FLAGS) & 2) || G(int, e + BS_HULL) >= 100) continue;
        int d = oct_dist(G(int, e + BS_X) - G(int, me + BS_X), G(int, e + BS_Y) - G(int, me + BS_Y));
        if (d < best) best = d;
    }
    return best;
}
static int fire_core(int side, unsigned ship, int x, int y, int report)
{
    /* PC9 (user): BOTH players can ALWAYS fire. The game refuses a shot when the side's other ship lies within 30 deg of
       the target's bearing, at any distance - that other ship is now the other player. Its "active" bit is hidden for
       the duration of the call, always, no distance test and no note. */
    (void)report;
    if (side != 0 || ship > 1 || !cfg.always_fire) return o_fire(side, ship, x, y);
    unsigned fr = BSHIP_BASE + (ship ^ 1) * BSHIP_STRIDE;
    unsigned ff = G(unsigned, fr + BS_FLAGS);
    if (!(ff & 2)) return o_fire(side, ship, x, y);
    G(unsigned, fr + BS_FLAGS) = ff & ~2u;
    int r = o_fire(side, ship, x, y);
    G(unsigned, fr + BS_FLAGS) = (G(unsigned, fr + BS_FLAGS) & ~2u) | (ff & 2);
    if (cfg.debug) logf_("fire: side 0 ship %u (friend hidden) -> %d", ship, r);
    return r;
}
int __cdecl hk_fire_p1(int side, unsigned ship, int x, int y) { return fire_core(side, ship, x, y, 1); }
int __cdecl hk_fire_volley(int side, unsigned ship, int x, int y) { return fire_core(side, ship, x, y, 0); }

/* Player 2 rams the enemy flagship -> the game's own collision/boarding (BATTLE_COLLIDE), like Player 1 ramming:
   "The ships collide with a mighty crash!" and the sword fight with the enemy captain. */
static void p2_ram_tick(void)
{
    if (!cfg.p2_ram_duel || (G(unsigned, GAME_FLAGS) & 4) || G(int, BATTLE_COLLIDE) != 0) return;
    unsigned e = BSHIP_BASE + 0x12a0;
    if (!(G(unsigned, e + BS_FLAGS) & 2) || G(int, e + BS_HULL) >= 100 || G(int, e + BS_CREW) == 0 || G(int, B0_ADDR + BS_CREW) == 0) return;
    if (G(int, B1_ADDR + BS_HULL) >= 100 || G(int, B0_ADDR + BS_HULL) >= 100) return;
    int d = oct_dist(G(int, e + BS_X) - G(int, B1_ADDR + BS_X), G(int, e + BS_Y) - G(int, B1_ADDR + BS_Y));
    /* PC11 (user: "actually touch the other ship"): the game's own boarding test for Player 1 is distance < 200000 AND
       the two ship MODELS colliding (FUN_004c1a50(model, model), cdecl, call at 0x47e5b7 with [0x8e6694] = side 0 ship 0
       and [0x8e669c] = side 1 ship 0). Player 2's model = [0x8e6698]. Same test now; without a model, distance < 90000. */
    if (d < 200000 && cfg.ram_contact) {
        unsigned m2 = G(unsigned, 0x8e6698), me = G(unsigned, 0x8e669c); int touch;
        if (m2 > 0x10000 && me > 0x10000) touch = (((char (__cdecl *)(unsigned, unsigned))0x4c1a50)(m2, me)) != 0;
        else touch = d < 90000;
        static DWORD t; if (cfg.debug && GetTickCount() - t > 1000) { t = GetTickCount(); logf_("P2 near the enemy flagship: dist %d, hulls touching %d (models %08x %08x)", d, touch, m2, me); }
        if (!touch) return;
    }
    if (d < 200000) {
        G(int, BATTLE_COLLIDE) = 1;
        logf_("Player 2 rammed the enemy flagship (dist %d) - boarding", d);
        notify_key(NK_BATTLE, "Player 2 rammed the enemy! Boarding - sword fight!");
    }
}

/* Sea battle camera (call 0x489360 at 0x47b36d, arg 3 = distance = base + zoom var 0x85a0fc * 50).
   PC9: continuous height g_bz_t (0 = the game's own distance, 1 = FarZoomPercent of the game's FARTHEST distance,
   i.e. base + 10*50). Held D-pad UP/DOWN move it at ZoomSpeedPercentPerSecond with a soft start/stop. */
void __cdecl c_bcam(int *args)
{
    static LARGE_INTEGER f, last; LARGE_INTEGER now;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)f.QuadPart : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    if (!g_in_battle) return;
    int dir = (GetTickCount() - g_bz_at < 250) ? g_bz_dir : 0;
    if (G(unsigned, GAME_FLAGS) & 4) dir = 0;                    /* paused / mod menu */
    float want = dir * (cfg.zoom_speed_pct > 0 ? cfg.zoom_speed_pct : 35) / 100.0f;
    float k = dt * 6.0f; if (k > 1) k = 1;
    g_bz_v += (want - g_bz_v) * k;                               /* ease in / ease out */
    if (!dir && fabsf(g_bz_v) < 0.002f) g_bz_v = 0;
    float before = g_bz_t;
    g_bz_t += g_bz_v * dt;
    if (g_bz_t <= 0) { g_bz_t = 0; if (g_bz_v < 0) g_bz_v = 0; }
    if (g_bz_t >= 1) { g_bz_t = 1; if (g_bz_v > 0) g_bz_v = 0; }
    {   static float logged = -1; static DWORD lt;
        if (before != g_bz_t && fabsf(g_bz_t - logged) > 0.1f && GetTickCount() - lt > 300) {
            logged = g_bz_t; lt = GetTickCount(); logf_("battle zoom: height %d%% (dist %d)", (int)(g_bz_t * 100 + 0.5f), args[2]); } }
    if ((G(unsigned, GAME_FLAGS) & 0x840) || G(int, BATTLE_COLLIDE)) return;   /* chase view / boarding: game's own */
    if (g_bz_t <= 0) return;
    int zoom = G(int, ZOOM), normal = args[2], base = normal - zoom * 50;
    float fard = (base + 10 * 50) * (cfg.far_zoom_pct > 100 ? cfg.far_zoom_pct : 200) / 100.0f;
    if (fard <= normal) return;
    float t = g_bz_t * g_bz_t * (3 - 2 * g_bz_t) * 0.35f + g_bz_t * 0.65f;   /* slightly softer at both ends */
    args[2] = (int)(normal + (fard - normal) * t);
}

/* ------------------------------------------------------------------ PC5: sailing against the wind (user request)
   FUN_00477450(type) with EAX = heading, ECX = wind: speed (x100) from the table at 0x724238 (per hull class type%9,
   9 points from "straight into the wind" (-1/-2 = pushed back) to "wind from behind"). Hooked at the two callers that
   move ships: 0x468881 (sailing, EBP = world ship idx*0x45c) and 0x47782a (sea battle, EBP = side*0x12a0+ship*0x4a8).
   For the player's ships only: speed is at least UpwindSpeedPercent of that hull's best point of sail. AI ships and the
   AI's steering decisions (other callers) are unchanged. */
void *p_wind = (void *)0x477450;
void stub_wind_sail(void); void stub_wind_battle(void);
static int wind_floor(int type)
{
    if (cfg.upwind_pct <= 0) return -100000;
    int t = ((type % 9) + 9) % 9, mx = 1;
    const signed char *row = (const signed char *)(0x724238 + t * 16);
    for (int i = 0; i <= 8; i++) if (row[i] > mx) mx = row[i];
    return mx * 100 * cfg.upwind_pct / 100;
}
/* PC6: ship stuck on land (seen after losing a duel: the game put the ship back half on the beach, it never moved
   again although speed > 0). Called every sailing frame for the player's ships: if the position has not changed for
   4 s while the ship has speed, it is pushed ~2 ship-seconds in the direction it points (the way the player steers),
   then watched again. heading 0 = +y (north), x = sin, y = cos. */
/* g_world_moving_at: declared near the top (PC12) */
static void world_watch(unsigned ebpoff)
{
    static int ax[64], ay[64]; int k = ebpoff / WSHIP_STRIDE; if (k < 8 || k >= 72) return; k -= 8;
    unsigned b = WSHIP_BASE + ebpoff; int x = G(int, b + WS_X), y = G(int, b + WS_Y);
    if (x != ax[k] || y != ay[k]) { if (ax[k] || ay[k]) g_world_moving_at = GetTickCount(); ax[k] = x; ay[k] = y; }
}
static void unstick_tick(unsigned ebpoff)
{
    if (!cfg.unstick) return;
    if (GetTickCount() - g_world_moving_at > 400) return;   /* paused (or nothing else at sea moves): never push */
    int k = ebpoff / WSHIP_STRIDE; if (k < 0 || k > 7) return;
    if (cfg.land_guard && g_coop_on && k == g_p2ship) return;     /* PC12: Player 2's ship has its own land guard */
    static int lx[8], ly[8], push[8]; static DWORD since[8];
    unsigned b = WSHIP_BASE + ebpoff; DWORD now = GetTickCount();
    int x = G(int, b + WS_X), y = G(int, b + WS_Y), sp = G(int, b + WS_SPEED);
    if (push[k] > 0) {
        push[k]--;
        double a = G(unsigned, b + WS_HEAD) * (6.283185307179586 / 4294967296.0);
        G(int, b + WS_X) = x + (int)(sin(a) * 20000); G(int, b + WS_Y) = y + (int)(cos(a) * 20000);
        lx[k] = G(int, b + WS_X); ly[k] = G(int, b + WS_Y); since[k] = now;
        return;
    }
    if (x != lx[k] || y != ly[k] || sp < 100 || now - g_world_moving_at > 400) { lx[k] = x; ly[k] = y; since[k] = now; return; }
    if (now - since[k] > 4000) {
        push[k] = 60; since[k] = now;
        static int n; if (n < 10) { n++; logf_("unstick: ship %d has not moved for 4 s (pos %d,%d speed %d heading %08x) - pushing it the way it points", k, x, y, sp, G(unsigned, b + WS_HEAD)); }
    }
}
#define SAILS_FULL_SAILING 0x869aacu   /* bit 1 = full sails while sailing (D-pad up sets, down clears) */
/* PC11 (user, 19:30): "sails DOWN = full speed" on every heading, full sails = the game's own wind behaviour.
   The game then takes 1/3 off a reduced-sails flagship while sailing (FUN_00467f90 ~0x468a80) and gives +50 % to full
   sails in sea battles (FUN_00477500), so reduced sails get 150 % of the hull's best table speed: after the game's own
   sail rule that is exactly the best full-sails speed, whatever the wind. Fleet ships (incl. Player 2) while sailing get
   no 1/3 cut, so they get 100 %. Returns -100000 = no change. */
static int reduced_speed(int type, int full, int pct_sail_rule)
{
    if (!cfg.reduced_fast) return full ? wind_floor(type) : -100000;    /* PC10 behaviour */
    if (full) return -100000;                                           /* full sails: vanilla */
    int t = ((type % 9) + 9) % 9, mx = 1;
    const signed char *row = (const signed char *)(0x724238 + t * 16);
    for (int i = 0; i <= 8; i++) if (row[i] > mx) mx = row[i];
    return mx * pct_sail_rule;
}
static int c_wind_sail_mult(int r, int type, unsigned ebpoff);
int __cdecl c_wind_sail(int r, int type, unsigned ebpoff)
{
    if (ebpoff >= 8 * WSHIP_STRIDE) { world_watch(ebpoff); return r; }   /* AI ship */
    unstick_tick(ebpoff);
    if (cfg.speed_mult > 1) return c_wind_sail_mult(r, type, ebpoff) * cfg.speed_mult;   /* PC8: mod menu ship speed */
    int f = reduced_speed(type, G(unsigned, SAILS_FULL_SAILING) & 1, ebpoff == 0 ? 150 : 100);   /* PC11 */
    if (ebpoff == 0 && cfg.debug) { static DWORD t; if (GetTickCount() - t > 3000) { t = GetTickCount();
        logf_("wind: from %08x, P1 heading %08x, sails %s, sail speed for this course %d (mod speed %d) -> %d", G(unsigned, 0x7253bc), G(unsigned, WSHIP_BASE + WS_HEAD), (G(unsigned, SAILS_FULL_SAILING) & 1) ? "FULL" : "reduced", r, f, r < f ? f : r); } }
    if (r < f) {
        static int n; if (n < 3 && cfg.debug) { n++; logf_("upwind help (sailing, ship %u): speed %d -> %d", ebpoff / WSHIP_STRIDE, r, f); }
        return f;
    }
    return r;
}
static int c_wind_sail_mult(int r, int type, unsigned ebpoff)
{
    int f = reduced_speed(type, G(unsigned, SAILS_FULL_SAILING) & 1, ebpoff == 0 ? 150 : 100);
    return r < f ? f : r;
}
int __cdecl c_wind_battle(int r, int type, unsigned ebpoff)
{
    if (ebpoff >= 0x12a0) return r;                         /* enemy side */
    if (cfg.speed_mult > 1) { int f = reduced_speed(type, G(unsigned, BSHIP_BASE + ebpoff + BS_FLAGS) & 1, 150); return (r < f ? f : r) * cfg.speed_mult; }
    int f = reduced_speed(type, G(unsigned, BSHIP_BASE + ebpoff + BS_FLAGS) & 1, 150);   /* +0x64 bit 1 = full sails; PC11 */
    if (r < f) {
        static int n; if (n < 3 && cfg.debug) { n++; logf_("upwind help (sea battle, ship %u): speed %d -> %d", ebpoff / 0x4a8, r, f); }
        return f;
    }
    return r;
}

/* ------------------------------------------------------------------ mouse cursor: hidden while a controller is used */
typedef HCURSOR (WINAPI *t_SetCursor)(HCURSOR);
/* ------------------------------------------------------------------ 2D quads (FUN_004af080, cdecl, 11 args) */
/* (tex, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH, argb, cornerFlags) in 640x480 virtual units. With Fit2D the
   TV shows g_2d_ext extra units at each side; a quad that spans the whole 640 width (full-screen backgrounds, fades,
   full-width bars) is continued into the sides with a mirrored copy of its own edge: no bars, nothing stretched. */
typedef int (__cdecl *t_quad)(void *, int, int, int, int, int, int, int, int, unsigned, unsigned);
extern int __cdecl o_quad_tramp(void *, int, int, int, int, int, int, int, int, unsigned, unsigned);
void *p_quad_cont = (void *)0x4af088;
void *p_regobj_cont = (void *)0x54bc25;
static int g_quad_ext_n;
int __cdecl hk_quad(void *tex, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned col, unsigned fl)
{
    /* PC12: the PC11 crash log showed text quads 227281100 units wide (from 0x4de616) just before the crash: never
       draw such garbage (it can only be broken data). */
    if (dw > 100000 || dw < -100000 || dh > 100000 || dh < -100000 || dx > 1000000 || dx < -1000000) {
        static int n; if (n < 5) { n++; logf_("2D quad from %08x with impossible size %d,%d %dx%d - not drawn", (unsigned)__builtin_return_address(0), dx, dy, dw, dh); }
        return 0;
    }
    { static DWORD last; DWORD now = GetTickCount(); if (now - last > 100) { last = now; ui_fit_tick(); } }
    int ext = (int)(g_2d_ext + 0.999f);
    /* PC4: CRASH FIX (0x54bc49, null +8 in the renderer, at sea 12:19 and 12:43): only full-screen BACKGROUND images
       (src 1024x768) are filled into the sides/zoomed, never anything at sea (sea overlays are 512x512 tints). */
    /* PC5: square sources (sea tints 512x512, screen captures 1024x1024 behind menus at sea) are a picture of / an
       overlay over the whole 16:9 frame -> drawn as ONE quad across the full TV width (correct proportions, no seam).
       All other full-width backgrounds (1024x768 pages, the 1024x646 world map) are extended as in PC3. */
    if ((unsigned)__builtin_return_address(0) == 0x445d30) {
        if (!g_mapquad_at || GetTickCount() - g_mapquad_at > 2000) logf_("world map open: stick scrolls, mouse ignored");
        g_mapquad_at = GetTickCount();
    }
    int square = sw > 0 && sw == sh && dx == 0 && dy == 0 && dw == 640 && dh == 480;
    int bg_ok = !square;
    if (square && ext > 0 && cfg.mirror_sides && !g_osk_active) {
        static int n; if (n < 6) { n++; logf_("full-screen picture %dx%d from %p drawn across the whole TV width", sw, sh, __builtin_return_address(0)); }
        return o_quad_tramp(tex, sx, sy, sw, sh, -ext, 0, 640 + 2 * ext, 480, col, fl);
    }
    if (cfg.debug && dw >= 300 && !(dw >= 638 && dx <= 1)) {   /* PC5: wide (not full-width) quads, one line per caller */
        static unsigned callers[64]; static int nc; unsigned ra = (unsigned)__builtin_return_address(0); int i;
        for (i = 0; i < nc; i++) if (callers[i] == ra) break;
        if (i == nc && nc < 64) { callers[nc++] = ra;
            logf_("2D wide quad from %08x: tex %p src %d,%d %dx%d -> %d,%d %dx%d argb %08x flags %x (ctx %d, mode %s)", ra, tex, sx, sy, sw, sh, dx, dy, dw, dh, col, fl, ctx_now(), g_mode_str ? g_mode_str : "?"); }
    }
    if (dw >= 638 && dx <= 1 && dx + dw >= 639) {
        static struct { void *t; int y, h; unsigned c; } seen[48]; static int nseen;
        int i; for (i = 0; i < nseen; i++) if (seen[i].t == tex && seen[i].y == dy && seen[i].h == dh && seen[i].c == col) break;
        unsigned ra0 = (unsigned)__builtin_return_address(0);
        if (ra0 == 0x4b6246 || ra0 == 0x4d43f6) i = 0;           /* movie frames: one new texture per frame, not logged */
        else if (i == nseen && nseen < 48) { seen[nseen].t = tex; seen[nseen].y = dy; seen[nseen].h = dh; seen[nseen].c = col; nseen++;
            logf_("2D full-width quad from %p: tex %p src %d,%d %dx%d -> %d,%d %dx%d argb %08x flags %x (ctx %d, mode %s)", __builtin_return_address(0), tex, sx, sy, sw, sh, dx, dy, dw, dh, col, fl, ctx_now(), g_mode_str ? g_mode_str : "?"); }
        /* cinematic letterbox bars (black, full width, at the top or bottom edge): not drawn (HideLetterbox=1) */
        if (cfg.hide_letterbox && dh > 0 && dh <= 140 && (dy <= 0 || dy + dh >= 480) && (col >> 24) >= 0x80 && (col & 0xe0e0e0) == 0 && !tex) {
            static int nl; if (nl < 4) { nl++; logf_("letterbox bar hidden (%d,%d %dx%d)", dx, dy, dw, dh); }
            col &= 0x00ffffff;
        }
    }
    /* PC10: plain colour panels that already reach past the top and bottom (e.g. the plunder/trade screen's dark panels
       -160,-160 480x800 and 320,-160 320x800) are drawn deeper in the 2D layer, so perspective shrinks them toward the
       middle: on 4:3 they still covered the screen edge, on 16:9 they stopped short (background showed on the left).
       Their outer edges are pushed far out (only off-screen parts change). */
    if (!tex && ext > 0 && dy < 0 && dy + dh > 480) {
        int far_ = 6 * ext;
        if (dx <= 0) { dx -= far_; dw += far_; }
        if (dx + dw >= 640) dw += far_;
        static int n; if (n < 4) { n++; logf_("2D panel widened to the TV edges (from %p)", __builtin_return_address(0)); }
    }
    if (bg_ok && ext > 0 && dx == 0 && dy == 0 && dw == 640 && dh == 480 && !g_osk_active && (int)(g_fit_until - GetTickCount()) <= 0) {
        /* movies (0x4d43f6) and the map behind the menus (0x4307dc): scenery, no layout tied to it -> zoomed to fill the
           TV exactly like the game's own widescreen view (nothing stretched, no repeated edges) */
        unsigned ra = (unsigned)__builtin_return_address(0);
        if (ra == 0x4d43f6 || ra == 0x4307dc) {
            float sc = (640.0f + 2.0f * g_2d_ext) / 640.0f;
            int w = (int)(640 * sc + 0.5f), h = (int)(480 * sc + 0.5f);
            return o_quad_tramp(tex, sx, sy, sw, sh, -ext, -(h - 480) / 2, w, h, col, fl);
        }
    }
    /* PC11 (user video: mirrored compass roses / coastlines at the TV sides while scrolling the world map): the map
       picture (0x445d30, 1024x646 -> 640x440) is a window into the big map, so its edge strip changes while scrolling and
       every mirrored copy showed it again. Now each side is the map's outermost texel column smeared outward, covered by
       a parchment-coloured layer (MapSideColour): the sides read as plain map paper, nothing repeats. MapSides=0 = PC10. */
    if (cfg.map_sides == 1 && ext > 0 && (unsigned)__builtin_return_address(0) == 0x445d30 && dw >= 638 && dx <= 1 && dx + dw >= 639 && dh > 0 && tex) {
        int ins = sw * 2 / dw; if (ins < 1) ins = 1;
        o_quad_tramp(tex, sx + ins, sy, 1, sh, dx - ext, dy, ext, dh, col, fl);
        o_quad_tramp(tex, sx + sw - ins - 1, sy, 1, sh, dx + dw, dy, ext, dh, col, fl);
        o_quad_tramp(NULL, 0, 0, 0, 0, dx - ext, dy, ext, dh, cfg.map_side_col, 0);
        o_quad_tramp(NULL, 0, 0, 0, 0, dx + dw, dy, ext, dh, cfg.map_side_col, 0);
        static int n; if (n < 2) { n++; logf_("world map: sides filled with plain map paper (no mirrored copies)"); }
        return o_quad_tramp(tex, sx, sy, sw, sh, dx, dy, dw, dh, col, fl);
    }
    if (bg_ok && cfg.mirror_sides && ext > 0 && dw >= 638 && dx <= 1 && dx + dw >= 639 && dh > 0) {
        /* the outer EdgeBand units of the quad are repeated outward, every other copy mirrored (seamless, no stretch) */
        int band = cfg.edge_band > 0 && cfg.edge_band < dw ? cfg.edge_band : 28;
        /* pirate info screen (0x44ac61): the framed map touches the right edge -> both sides use the left edge (open sea) */
        int left_only = (unsigned)__builtin_return_address(0) == 0x44ac61;
        for (int side = 0; side < 2; side++)
            for (int k = 0, done = 0; done < ext; k++) {
                int w = ext - done < band ? ext - done : band;
                int es = (int)((float)sw * band / dw), esw = (int)((float)sw * w / dw);
                int x = side ? dx + dw + done : dx - done - w, srcx, srcw;
                int ins = sw * 2 / dw, L = sx + ins, R = sx + sw - ins;   /* skip the outermost texels (dark seam lines) */
                if (!side) { if (!(k & 1)) { srcx = L + esw; srcw = -esw; } else { srcx = L + es - esw; srcw = esw; } }
                else       { if (!(k & 1)) { srcx = R;       srcw = -esw; } else { srcx = R - es;       srcw = esw; } }
                if (side && left_only) {           /* the left-side tile k, flipped */
                    int lx = (k & 1) ? L + es - esw : L + esw, lw = (k & 1) ? esw : -esw;
                    srcx = lx + lw; srcw = -lw;
                }
                o_quad_tramp(tex, srcx, sy, srcw, sh, x, dy, w, dh, col, fl);
                done += w;
            }
        if (g_quad_ext_n < 6) { g_quad_ext_n++; logf_("2D full-width quad extended into the sides (tex %p, %dx%d at %d,%d, +%d each side)", tex, dw, dh, dx, dy, ext); }
    }
    return o_quad_tramp(tex, sx, sy, sw, sh, dx, dy, dw, dh, col, fl);
}
/* world point -> 2D virtual coords (FUN_004ae7d0(x,y,z,&outX,&outY), cdecl, 12 callers: names over towns/ships...).
   It returns screen-normalised x * 640; with Fit2D the 640 units are only the middle of the TV. */
typedef char (__cdecl *t_w2v)(int, int, int, int *, int *);
char __cdecl hk_w2v(int x, int y, int z, int *ox, int *oy)
{
    char r = ((t_w2v)0x4ae7d0)(x, y, z, ox, oy);
    if (g_2d_ext > 0 && ox) *ox = (int)((float)*ox * (640.0f + 2.0f * g_2d_ext) / 640.0f - g_2d_ext);
    return r;
}
/* cinematic letterbox bars: the game places a black strip element at the top and bottom of the screen with
   FUN_004bba60(element, x, y, alpha, rot) (stdcall) - from FUN_004afee0 (story/event cut-scenes, 1/8 of the height) and
   FUN_0046fa20 (captain scenes) and FUN_0046fe30 (bartender). On a 16:9 TV the picture is already wide, so with HideLetterbox=1 they are not placed. */
int __stdcall hk_letterbox(int el, int x, int y, float a, float r)
{
    if (cfg.hide_letterbox) { static int n; if (!n++) logf_("cut-scene letterbox bars hidden"); return 0; }
    return ((int (__stdcall *)(int, int, int, float, float))0x4bba60)(el, x, y, a, r);
}
/* PC5 debug: every other caller of the per-frame element placement FUN_004bba60 (stdcall el,x,y,alpha,rot): logs each
   call site once with the element's size (+0x34/+0x38 as the game reads them), to find screen parts that stop at the
   old 4:3 edges (e.g. the dark band under the governor's subtitles). */
int __stdcall hk_place(int el, int x, int y, float a, float r)
{
    static unsigned seen[160]; static int ns; unsigned ra = (unsigned)__builtin_return_address(0); int i;
    /* PC13 (user: goods pictures on top of the prices): the trade screen's row pictures (0x4debd5, FUN_004de1e0:
       x = the price column 4W/13+16, y = row + 17) are moved into the empty gap left of the price, a bit higher. */
    if (ra == 0x4debd5 && cfg.icons_left) { x -= 40; y -= 7; }
    /* PC13: the dark band under cut-scene subtitles (governor, 642x242 at 321,360, scale 100, placed through helper
       FUN_00412900) ended at the old 4:3 edges -> brighter strips at the TV sides. Scaled to the full TV width; its
       top edge stays where it was (the extra height goes below the screen). */
    if (g_2d_ext > 0 && el > 0x10000 && !IsBadReadPtr((void *)el, 0x40) && G(int, el + 0x34) == 642 && G(int, el + 0x38) == 242 && a > 99.0f && a < 101.0f) {
        float k = (640.0f + 2.0f * g_2d_ext) / 640.0f;
        a *= k; y = 360 + (int)(121.0f * (k - 1.0f));
        static int n; if (!n++) logf_("subtitle band widened to the TV edges (scale x%.2f)", k);
    }
    if (ra == 0x41294c || ra == 0x4128f6) ra = ((unsigned *)&r)[1] | 0x80000000u;   /* FUN_00412900 helper: use its caller */
    for (i = 0; i < ns; i++) if (seen[i] == ra) break;
    if (cfg.debug && i == ns && ns < 160) { seen[ns++] = ra;
        int w = (el > 0x10000 && !IsBadReadPtr((void *)el, 0x40)) ? G(int, el + 0x34) : -1, h = w != -1 ? G(int, el + 0x38) : -1;
        logf_("element from %08x: el %08x at %d,%d size %dx%d alpha %.2f (ctx %d, mode %s)", ra, el, x, y, w, h, a, ctx_now(), g_mode_str ? g_mode_str : "?"); }
    return ((int (__stdcall *)(int, int, int, float, float))0x4bba60)(el, x, y, a, r);
}
/* "rival pirate" info screen (FUN_0044a710, key read call 0x44b68b): it only closes on Esc (0x1b) or a mouse click on the
   knife (0x202); its key read uses mode 0, so no pad button worked. A, B or START (fresh press) -> Esc. */
int __cdecl hk_infokey(const char *mode)
{
    int k = ((int (__cdecl *)(const char *))0x42e1d0)(mode);
    static WORD prev; static DWORD last; DWORD now = GetTickCount();
    WORD b = 0; for (int i = 0; i < 4; i++) if (slot[i].connected) b |= slot[i].st.Gamepad.wButtons;
    if (now - last > 500) prev = b;                 /* screen just opened: ignore the button still held */
    last = now;
    WORD pr = b & ~prev; prev = b;
    if (pr & (XB_A | XB_B | XB_START)) { logf_("info screen closed with the controller"); return 0x1b; }
    if (k == 0xd || k == 0x20) return 0x1b;
    return k;
}
/* mouse: the game maps the window to 0..640 x 0..480. With Fit2D the 640 units sit in the middle of the TV. */
static LPARAM fit_mouse(LPARAM lp)
{
    if (g_2d_ext <= 0) return lp;
    int W = G(int, 0x8cac18); if (W <= 0) return lp;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    float vis = 640.0f + 2.0f * g_2d_ext;
    float v = (float)x / W * vis - g_2d_ext;
    if (v < 0) v = 0; if (v > 639) v = 639;
    x = (int)(v * W / 640.0f);
    return MAKELPARAM(x, y);
}
static t_SetCursor o_SetCursor;
static WNDPROC o_WndProc;
static POINT g_mouse_anchor;
static IDirect3DDevice9 *g_cur_dev; static BOOL g_dev_cursor_wanted;
typedef BOOL (WINAPI *t_DevShowCursor)(IDirect3DDevice9 *, BOOL);
static t_DevShowCursor o_DevShowCursor;

static HCURSOR game_cursor(void)
{
    HCURSOR c = G(HCURSOR, 0x8cacd8);
    return c ? c : LoadCursorA(0, IDC_ARROW);
}
static void set_pad_mode(int on)
{
    int was = g_pad_mode;
    g_pad_mode = on;
    if (!cfg.hide_cursor || was == on) return;
    if (on) {
        GetCursorPos(&g_mouse_anchor);
        if (o_SetCursor) o_SetCursor(NULL);
        if (g_cur_dev && o_DevShowCursor) o_DevShowCursor(g_cur_dev, FALSE);
    } else {
        if (o_SetCursor) o_SetCursor(game_cursor());
        if (g_cur_dev && o_DevShowCursor && g_dev_cursor_wanted) o_DevShowCursor(g_cur_dev, TRUE);
    }
    if (cfg.debug) logf_("input: %s", on ? "controller (mouse cursor hidden)" : "mouse");
}
static HCURSOR WINAPI hk_SetCursor(HCURSOR c)
{
    if (cfg.hide_cursor && g_pad_mode == 1) { o_SetCursor(NULL); return c; }
    return o_SetCursor(c);
}
static BOOL WINAPI hk_DevShowCursor(IDirect3DDevice9 *d, BOOL show)
{
    g_dev_cursor_wanted = show;
    if (cfg.hide_cursor && g_pad_mode == 1) show = FALSE;
    return o_DevShowCursor(d, show);
}
static LRESULT CALLBACK hk_WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    /* PC5 (user request): while any controller is connected the game never sees the mouse at all (no map drifting,
       no hover stealing the highlight, no flip to "mouse mode" that hid the on-screen keyboard). */
    if (cfg.ignore_mouse && any_pad() && ((m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_SETCURSOR || m == WM_MOUSEHOVER || m == WM_MOUSELEAVE)) {
        if (g_pad_mode != 1) set_pad_mode(1);
        if (m == WM_SETCURSOR) { if (LOWORD(lp) == HTCLIENT) { if (o_SetCursor) o_SetCursor(NULL); return TRUE; } return CallWindowProcA(o_WndProc, h, m, wp, lp); }
        return 0;
    }
    if (((m >= WM_MOUSEMOVE && m <= WM_MOUSEWHEEL) || m == WM_SETCURSOR) && map_screen() && any_pad()) {
        if (m == WM_SETCURSOR) { if (o_SetCursor) o_SetCursor(NULL); return TRUE; }
        return 0;                                    /* map: mouse switched off while controllers are connected */
    }
    if (cfg.hide_cursor && g_pad_mode == 1) {
        if (m == WM_MOUSEMOVE) {
            POINT p; GetCursorPos(&p);
            if (abs(p.x - g_mouse_anchor.x) + abs(p.y - g_mouse_anchor.y) < 6) return 0;   /* not a real move: ignore */
            set_pad_mode(0);
        } else if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_MOUSEWHEEL) {
            set_pad_mode(0);
        } else if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            o_SetCursor(NULL); return TRUE;
        }
    }
    if (m >= WM_MOUSEMOVE && m <= WM_MBUTTONDBLCLK) lp = fit_mouse(lp);
    return CallWindowProcA(o_WndProc, h, m, wp, lp);
}
static void hook_window(HWND w)
{
    if (!w || w == g_hwnd) return;
    g_hwnd = w;
    o_WndProc = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)hk_WndProc);
    logf_("window %p hooked (mouse/controller switching)", (void *)w);
}

/* ------------------------------------------------------------------ on-screen keyboard (name entry) */
#define OSK_R 5
#define OSK_C 7
static const char *osk_keys[OSK_R][OSK_C] = {
    { "A","B","C","D","E","F","G" },
    { "H","I","J","K","L","M","N" },
    { "O","P","Q","R","S","T","U" },
    { "V","W","X","Y","Z","1","2" },
    { "Space","","Del","","Clear","","Done" } };
static int osk_r, osk_c, osk_caps = 1; static DWORD osk_next_move; static WORD osk_prev[4];
static const char *g_osk_what;

static void osk_send(int ch)
{
    if (g_hwnd) PostMessageA(g_hwnd, WM_CHAR, (WPARAM)ch, 1);
}
static void osk_press(const char *k)
{
    if (!strcmp(k, "Space")) { osk_send(' '); osk_caps = 1; }
    else if (!strcmp(k, "Del")) osk_send(8);
    else if (!strcmp(k, "Clear")) { osk_send(0x1b); osk_caps = 1; }
    else if (!strcmp(k, "Done")) osk_send(13);
    else {
        int ch = k[0];
        if (ch >= 'A' && ch <= 'Z' && !osk_caps) ch += 32;
        osk_send(ch);
        if (isalpha(k[0])) osk_caps = 0;
    }
}
static void osk_tick(void)
{
    if (!g_osk_active) return;
    DWORD now = GetTickCount();
    float x = 0, y = 0; WORD pressed = 0;
    for (int i = 0; i < 4; i++) {
        if (!slot[i].connected) { osk_prev[i] = 0; continue; }
        WORD b = slot[i].st.Gamepad.wButtons;
        pressed |= b & ~osk_prev[i]; osk_prev[i] = b;
        float sx, sy; pad_stick(i, &sx, &sy);
        if (fabsf(sx) > fabsf(x)) x = sx; if (fabsf(sy) > fabsf(y)) y = sy;
    }
    int dx = x > 0.5f ? 1 : x < -0.5f ? -1 : 0, dy = y > 0.5f ? -1 : y < -0.5f ? 1 : 0;
    if (dx || dy) {
        if (!osk_next_move || (int)(now - osk_next_move) >= 0) {
            for (int k = 0; k < OSK_C; k++) {      /* skip the blank cells next to the wide keys */
                osk_c = (osk_c + dx + OSK_C) % OSK_C; osk_r = (osk_r + dy + OSK_R) % OSK_R;
                if (osk_keys[osk_r][osk_c][0]) break;
                if (!dx) dx = -1, dy = 0;
            }
            osk_next_move = now + (osk_next_move ? 130 : 350);
        }
    } else osk_next_move = 0;
    if (pressed & XB_A) osk_press(osk_keys[osk_r][osk_c]);
    if (pressed & XB_B) osk_send(8);
    if (pressed & XB_X) { osk_send(' '); osk_caps = 1; }
    if (pressed & XB_Y) osk_caps = !osk_caps;
    if (pressed & XB_START) osk_send(13);
}
static void osk_begin(const char *what)
{
    if (!cfg.osk) return;
    osk_r = 0; osk_c = 0; osk_caps = 1; osk_next_move = 0;
    for (int i = 0; i < 4; i++) osk_prev[i] = slot[i].connected ? slot[i].st.Gamepad.wButtons : 0;
    g_osk_what = what; g_osk_active = 1;
    logf_("on-screen keyboard: %s", what);
    if (g_pad_mode == 1) notify_key(NK_PAD, "Type the %s with the controller: stick moves, A types, B deletes, START = done.", what);
}
/* drawn with the game's own text routine inside the name-entry loop, on the empty right half of the scroll */
void __cdecl c_osk_gamedraw(void)
{
    if (!g_osk_active) return;
    g_fit_until = GetTickCount() + 400;   /* crew sign-up and ship-name pages: always the whole page */
    ui_fit_tick();
    xinput_poll();
    osk_tick();
    if (g_pad_mode != 1) return;
    int W = G(int, 0x85a26c), H = G(int, 0x85a268);
    if (W < 300 || H < 200) { W = 640; H = 480; }
    int x0 = W * 56 / 100, dx = W * 52 / 1000, y0 = H * 27 / 100, dy = H * 75 / 1000;
    for (int r = 0; r < OSK_R; r++) for (int c = 0; c < OSK_C; c++) {
        const char *k = osk_keys[r][c]; if (!k[0]) continue;
        static char lab[OSK_R][OSK_C][12];
        int sel = (r == osk_r && c == osk_c);
        char t[8]; lstrcpynA(t, k, sizeof t);
        if (!t[1] && t[0] >= 'A' && t[0] <= 'Z' && !osk_caps) t[0] += 32;
        snprintf(lab[r][c], sizeof lab[r][c], sel ? "[%s]" : "%s", t);
        int x = x0 + c * dx + (r == OSK_R - 1 ? dx / 2 : 0);   /* wide bottom keys shifted half a key right (clear of the Skill list) */
        game_text(lab[r][c], x, y0 + r * dy, sel ? 90 : 75, sel ? 0xffc00000u : 0xff3e2816u, 4);
    }
    static char help1[] = "A = type   B = delete   X = space";
    static char help2[] = "Y = capitals   START = done";
    game_text(help1, x0 + dx * 3, y0 + OSK_R * dy + dy / 3, 55, 0xff3e2816u, 4);
    game_text(help2, x0 + dx * 3, y0 + OSK_R * dy + dy, 55, 0xff3e2816u, 4);
}
static void osk_end(void) { if (g_osk_active) logf_("on-screen keyboard closed"); g_osk_active = 0; }
void __cdecl c_name_begin(void) { osk_begin("crew sign-up name"); }
void __cdecl c_name_end(void) { osk_end(); }
void __cdecl c_shipname_begin(void) { osk_begin("ship name"); }
void __cdecl c_shipname_end(void) { osk_end(); }

/* ------------------------------------------------------------------ crew sign-up choices (FUN_0046eb00) */
/* Experience / Skill / Start date list: highlight DAT_00725748 (-1 none, 0-4 experience, 5-9 skill, 10-14 era).
   PC keys: space = tick the highlighted box, Enter = continue, Esc = back. The pad's A sends Enter, so a pad
   could never tick a box. With a pad: A = tick, START = continue, B = back. */
int __cdecl hk_signup_key(const char *mode)
{
    g_fit_until = GetTickCount() + 400; ui_fit_tick();
    int k = ((int (__cdecl *)(const char *))0x42e1d0)(mode);
    if (g_pad_mode != 1) return k;
    { static DWORD last_seen; DWORD now = GetTickCount();     /* every time the choices page opens (after the name) */
      if (!last_seen || now - last_seen > 2000)
          notify_key(NK_PAD, "Name done! Stick = move, A = tick a box, B = back.  Press START to confirm and continue.");
      last_seen = now; }
    if (G(int, 0x725748) == -1) G(int, 0x725748) = 0;
    WORD b = 0; for (int i = 0; i < 4; i++) if (slot[i].connected) b |= slot[i].st.Gamepad.wButtons;
    if (k == 0xd && (b & XB_A)) k = 0x20;
    else if (k == 0x50) k = 0xd;           /* START (the game's pause key 'P') = continue */
    return k;
}

/* ------------------------------------------------------------------ menus: start on the first choice (Xbox behaviour) */
/* FUN_00430190(x, y, bg, disabledMask, timeout, cb, cb, defaultChoice, ...). The PC version starts with
   no choice highlighted (-1) because of the mouse, so A did nothing ("Welcome aboard lads" not taken). */
void __cdecl c_menu_pre(int *args)
{
    park_mouse();
    if (!cfg.menu_default_first || g_pad_mode != 1) return;
    if (args[7] != -1) return;
    unsigned mask = (unsigned)args[3];
    for (int i = 0; i < 32; i++) if (!(mask & (1u << i))) { args[7] = i; break; }
}

/* ------------------------------------------------------------------ plunder / trade screen (FUN_004dd8f0, EBP = screen) */
/* focus p[0x12]: 1..6 cargo rows, 0x12 sell all, 0x13 take all, 0x14 "Keep her", 0x15 "Sink her", 0x16 Continue */
typedef int (__fastcall *t_takeall)(int *);
static t_takeall o_takeall = (t_takeall)0x4e0880;
static int *g_trade_p; static DWORD g_trade_seen; static int g_trade_after = -100, g_trade_downfrom = -100;
#define TR_FOCUS 0x12
void __cdecl c_trade_frame(int *p)
{
    DWORD now = GetTickCount();
    if (p != g_trade_p || now - g_trade_seen > 1500) {        /* screen just opened */
        g_trade_p = p; g_trade_after = g_trade_downfrom = -100;
        park_mouse();
        int ship = p[0x15] != -1 && ((char *)p)[0x59];
        logf_("trade screen: kind %d, captured ship %d, focus %d", p[0], ship, p[TR_FOCUS]);
        if (g_pad_mode == 1 && ship) p[TR_FOCUS] = 0x14;          /* A = keep the captured ship */
    }
    g_trade_seen = now;
    if (g_pad_mode != 1) { g_trade_after = g_trade_downfrom = -100; return; }
    if (g_trade_after != -100) {               /* A was pressed last frame: move on to the next useful button */
        int f = g_trade_after; g_trade_after = -100;
        if (f == 0x14) p[TR_FOCUS] = ((unsigned char)o_takeall(p)) ? 0x13 : 0x16;
        else if (f == 0x12 || f == 0x13) p[TR_FOCUS] = 0x16;
    }
    if (g_trade_downfrom != -100) {            /* DOWN wrapped round to the top: stop on Continue first */
        int f = g_trade_downfrom; g_trade_downfrom = -100;
        if (p[TR_FOCUS] == 1 && f != -1 && f != 0x16 && f != 0) p[TR_FOCUS] = 0x16;
    }
}
int __cdecl c_trade_key(int *p, int key)
{
    if (g_pad_mode != 1) return key;
    int f = p[TR_FOCUS];
    if (key == 0xd || key == 0x10c) { if (f == 0x12 || f == 0x13 || f == 0x14) g_trade_after = f; return key; }
    if (key == 0x128) { if (f == 0x16) p[TR_FOCUS] = -1; else g_trade_downfrom = f; return key; }
    if (key == 0x126) {
        if (f == 0x16) { p[TR_FOCUS] = 1; return key; }      /* up from Continue = the bottom button */
        if (f == 1 || f == -1) { p[TR_FOCUS] = 0x16; return 0; }
        return key;
    }
    if ((key == 0x125 || key == 0x127) && f == 0x16) return 0;
    return key;
}

/* PC11 tavern: "Who would you like to talk to?" (FUN_00406ce0 -> FUN_004068f0, EAX = highlighted person, -1 = none; 0 =
   the crew (only offered when 3+ men wait: [0x72c65c] >= 3), 1/2/3 = others, blocked by bits 8/2/1 of [0x72c650] once
   done). With a controller the highlight now starts on the crew (else the first person still available), so A works. */
void *p_tavern_pick = (void *)0x4068f0;
int __cdecl c_tavern_pre(int cur)
{
    if (!cfg.tavern_default || g_pad_mode != 1 || cur != -1) return cur;
    unsigned done = G(unsigned, 0x72c650);
    int ok[4] = { G(int, 0x72c65c) >= 3, !((done >> 3) & 1), !((done >> 1) & 1), !(done & 1) };
    for (int i = 0; i < 4; i++) if (ok[i]) {
        static DWORD t; if (GetTickCount() - t > 5000) logf_("tavern: highlight starts on %s", i == 0 ? "the crew (recruit men)" : "the first person still to talk to");
        t = GetTickCount();
        return i;
    }
    return cur;
}
/* PC11 boarding after a collision (0x47e651, right after "The ships collide with a mighty crash!"): the game lets the
   enemy surrender at random (crew x quality vs enemy crew, 0x47e693) or always for enemy type [0x8bd768] > 4.
   User: the enemy only surrenders when it is below 10 % health (hull damage [0x8bd730] >= 90), otherwise ALWAYS the
   sword fight. 0 = game's own rule, 1 = surrender (0x47f922), 2 = sword fight (0x47e6a4). */
void *p_surr_vanilla = (void *)0x47e658, *p_surr_yes = (void *)0x47f922, *p_surr_duel = (void *)0x47e6a4;
int g_surr_r;
int __cdecl c_surrender(void)
{
    if (!cfg.surrender_rule || G(int, 0x8caaa4) == 0) return 0;
    int hull = G(int, 0x8bd730), r = hull >= 90 ? 1 : 2;
    logf_("boarding: enemy hull damage %d%% -> %s", hull, r == 1 ? "the enemy surrenders" : "sword fight");
    return r;
}

/* ------------------------------------------------------------------ shipwright: highlight always starts on "Repair" */
static volatile int g_yard;
void __cdecl c_yard_begin(int obj) { g_yard = obj; park_mouse(); logf_("shipwright opened"); }
void __cdecl c_yard_end(void) { g_yard = 0; }
/* PC10 (user): highlight = Repair (0) when the shown ship is damaged, else Sell (1) (only shown with 2+ ships).
   Applied when the highlight is empty (-1, also after every action) and whenever another ship is chosen.
   Shown ship index DAT_007263c0; its sail/hull damage = world ship +0x20/+0x22; fleet count byte-short 0x869ad0. */
#define YARD_SHIP 0x7263c0u
static void yard_tick(void)
{
    static int last_ship = -2, last_yard;
    int y = g_yard;
    if (!y) { last_ship = -2; last_yard = 0; return; }
    if (g_pad_mode != 1 || IsBadReadPtr((void *)(y + 0xc), 4)) return;
    int sh = G(int, YARD_SHIP);
    if (y != last_yard) { last_yard = y; last_ship = -2; }
    if (sh < 0 || sh > 7) return;
    int f = G(int, y + 0xc);
    if (f != -1 && sh == last_ship) return;
    unsigned w = WSHIP_BASE + sh * WSHIP_STRIDE;
    int damaged = G(short, w + 0x20) > 0 || G(short, w + 0x22) > 0;
    int fleet = G(short, 0x869ad0);
    int want = (!damaged && fleet > 1) ? 1 : 0;
    G(int, y + 0xc) = want;
    if (sh != last_ship) logf_("shipwright: ship %d %s -> highlight %s", sh, damaged ? "damaged" : "repaired", want ? "Sell" : "Repair");
    last_ship = sh;
}

/* ------------------------------------------------------------------ resolution + widescreen field of view */
void __cdecl hk_loadcfg(const char *name)
{
    ((void (__cdecl *)(const char *))p_loadcfg)(name);
    int w = G(int, 0x8cac18), h = G(int, 0x8cac1c);
    if (cfg.width >= 640 && cfg.height >= 480) {
        G(int, 0x8cac18) = cfg.width; G(int, 0x8cac1c) = cfg.height; G(int, 0x8cac20) = cfg.width;
        logf_("resolution: Config.ini %dx%d -> %dx%d (PiratesCoop.ini [Display])", w, h, cfg.width, cfg.height);
    } else logf_("resolution: %dx%d from Config.ini", w, h);
}
/* NiFrustum setter (thiscall, 0x54aa00): left,right,top,bottom,near,far,ortho. The game keeps the width of
   the view and cuts the height on wide screens. WidescreenFOV keeps the 4:3 height and adds width instead. */
static struct { float *f; float r0, t0, r1, t1; } g_fset[16]; static int g_fset_n;
static void fset_note(float *f, float r0, float t0, float r1, float t1)
{
    int i; for (i = 0; i < g_fset_n; i++) if (g_fset[i].f == f) break;
    if (i == g_fset_n) { if (g_fset_n < 16) g_fset_n++; else i = 0; }
    g_fset[i].f = f; g_fset[i].r0 = r0; g_fset[i].t0 = t0; g_fset[i].r1 = r1; g_fset[i].t1 = t1;
}
void __cdecl c_frustum(float *f, unsigned *a)
{
    float l = *(float *)&a[0], r = *(float *)&a[1], t = *(float *)&a[2], b = *(float *)&a[3];
    unsigned char ortho = (unsigned char)a[6];
    int W = G(int, 0x8cac18), H = G(int, 0x8cac1c);
    if (cfg.widescreen_fov && !ortho && W > 0 && H > 0 && W * 3 > H * 4 + 8 && r > 0 && fabsf(l + r) < 1e-4f * r && fabsf(t + b) < 1e-4f * fabsf(t) + 1e-6f) {
        float ratio = t / r, scr = (float)H / W;
        /* Every camera except the main world camera: 4:3 picture in the middle of the TV (menus, sign-up, 2D screens;
           this is what gave the correct side bars). The main world camera (sailing, battles, cut-scenes, towns) is
           handled per frame by frustum_tick: wider view only at sea, the game's own view elsewhere. */
        unsigned cam = G(unsigned, CAMERA_PTR);
        int main_cam = cam > 0x10000 && (unsigned)f == cam + (g_frustum_off >= 0 ? (unsigned)g_frustum_off : 0x128u);
        { static int nlog; if (nlog < 12) { nlog++; logf_("frustum set %p: %.3f x %.3f%s", (void *)f, r, t, main_cam ? " (main camera)" : ""); } }
        (void)main_cam;
        if (fabsf(ratio - scr) < 0.01f || fabsf(ratio - 0.75f) < 0.01f) {
            float t2 = r * 0.75f, r2 = t2 * W / H;
            fset_note(f, r, t, r2, t2);
            l = -r2; r = r2; t = t2; b = -t2;
            static int logged; if (!logged++) logf_("4:3 screen camera kept 4:3: frustum %.3f x %.3f -> %.3f x %.3f", *(float *)&a[1], *(float *)&a[2], r, t);
        }
    }
    f[0] = l; f[1] = r; f[2] = t; f[3] = b;
    ((unsigned *)f)[4] = a[4]; ((unsigned *)f)[5] = a[5];
    *((unsigned char *)f + 0x18) = ortho;
}

/* PC12 (trade screen arrows still over the "tons" text in PC11): the 3D "button" screens (trade arrows + the
   ship-choice buttons, FUN_00503ca0, called from the trade screen 0x4dd1dd and 0x4b1794 / 0x4d5ae2) make their own
   camera with a FIXED 4:3 frustum (-0.5, 0.5, 0.375, -0.375, near 921.6, far 1536; call 0x54aa00 at 0x503d01) and the
   button layout is 1024x768. On 16:9 that camera stretches its 4:3 picture over the whole TV width, so every button
   sat 1.33x too far from the middle (and looked 1/3 too wide), while the 2D text (Fit2D) uses the middle 3/4 only.
   (PC11 widened the MAIN camera instead - wrong camera. The old c_frustum hook is not installed any more.)
   Fix: at that one call the frustum gets R = 0.375 * W / H (same height): the 1024x768 layout fills the middle 3/4
   exactly like the 2D layer, unstretched. */
void *p_frustum_set = (void *)0x54aa00;
void __cdecl c_btn_frustum(unsigned *a)
{
    float r = *(float *)&a[1], t = *(float *)&a[2];
    int W = G(int, 0x8cac18), H = G(int, 0x8cac1c);
    if (!cfg.button_fit || cfg.signup_fit != 2 || W <= 0 || H <= 0 || W * 3 <= H * 4 + 8) return;
    if (fabsf(r - 0.5f) > 0.002f || fabsf(t - 0.375f) > 0.002f) return;
    float r2 = t * W / H;
    *(float *)&a[0] = -r2; *(float *)&a[1] = r2;
    static int n; if (n < 4) { n++; logf_("button screen camera: 4:3 frustum %.3f x %.3f -> %.3f x %.3f (buttons line up with the text, not stretched)", r, t, r2, t); }
}
/* PC12 crash 12 Nov 19:43:46 at 0x4e0126: trade screen buttons 0x14/0x15 (the two ship-choice buttons, objects at
   +0xb4/+0xb8 of the trade object, only created when there is a captured ship) were "pressed" (switch case
   0x214/0x215 -> 0x4de15e -> FUN_004e00d0, EDI = trade object) on a plunder screen WITHOUT a captured ship -> null.
   Now skipped when either object is missing. */
void *p_shipbtn = (void *)0x4e00d0;
void __cdecl c_shipbtn_skip(void) { static int n; if (n < 5) { n++; logf_("trade screen: ship-choice button pressed but there is no captured ship - ignored (PC11 crash)"); } }

/* Wider view only at sea (sailing, sea battles): open water has no edges. In towns, cut-scenes and duels the
   sets were built for 4:3 and a wider view shows past their edges, so there the game's own view is kept. */
static float g_hp_base_r, g_hp_base_t, g_hp_r, g_hp_t;
static void frustum_tick(void)
{

    unsigned cam = G(unsigned, CAMERA_PTR);
    if (cam < 0x10000 || IsBadReadPtr((void *)cam, 0x200)) return;
    if (g_frustum_cam != cam) { float a, b; screen_pos(0, 0, 0, &a, &b); }   /* finds the frustum offset */
    if (g_frustum_off < 0 || g_frustum_cam != cam) return;
    float *f = (float *)(cam + g_frustum_off);
    int W = G(int, 0x8cac18), H = G(int, 0x8cac1c);
    if (W <= 0 || H <= 0 || W * 3 <= H * 4 + 8) return;
    float r = f[1], t = f[2], br, bt;
    for (int i = 0; i < g_fset_n; i++)        /* widened at set time before the camera was known: undo it here */
        if (g_fset[i].f == f && r == g_fset[i].r1 && t == g_fset[i].t1) { r = g_fset[i].r0; t = g_fset[i].t0; f[0] = -r; f[1] = r; f[2] = t; f[3] = -t; break; }
    if (r == g_hp_r && t == g_hp_t && g_hp_r != 0) { br = g_hp_base_r; bt = g_hp_base_t; }
    else if (r > 0 && fabsf(t / r - (float)H / W) < 0.01f) { br = r; bt = t; }
    else if (cfg.no_stretch && r > 0 && fabsf(t / r - 0.75f) < 0.01f) {
        /* PC3: cameras with a fixed 4:3 frustum (tavern, bartender, captains, sign-up scene...) were drawn stretched
           sideways on 16:9. Use the game's own widescreen rule instead (same width, less height): nothing stretched,
           no set edges shown. At sea the usual wider view is applied below. */
        br = r; bt = r * H / W;
        static int n; if (n < 6) { n++; logf_("4:3 camera frustum %.3f x %.3f -> %.3f x %.3f (no stretching)", r, t, br, bt); }
    }
    else return;
    int ctx = ctx_now();
    /* PC11: trade/plunder screen. Its arrows and commodity icons are 3D buttons (FUN_004dd760 -> FUN_00506e60, made in a
       1024x768 layout) drawn by THIS camera, while all text is 2D (Fit2D: 640 units = the middle 3/4 of the TV). With the
       game's cropped view the buttons sat 1/3 too far from the middle (icons over the "tons"/"gold" text). Giving the
       camera the same wide view as the 2D layer puts them back where the 4:3 layout has them. The screen's background is
       a 2D picture, so nothing past the 3D set's edges can show. */
    int trade_wide = cfg.trade_wide && g_trade_p && GetTickCount() - g_trade_seen < 400;
    { static int was; if (trade_wide != was) { was = trade_wide; logf_("trade screen: main camera %s", trade_wide ? "widened like the 2D layer (buttons line up with the text)" : "back to normal"); } }
    if (cfg.widescreen_fov && (ctx == CTX_SAIL || ctx == CTX_BATTLE || trade_wide)) {
        float t2 = br * 0.75f, r2 = t2 * W / H;
        if (r != r2 || t != t2) {
            f[0] = -r2; f[1] = r2; f[2] = t2; f[3] = -t2; cam_refresh(cam);
            g_hp_base_r = br; g_hp_base_t = bt; g_hp_r = r2; g_hp_t = t2;
            static int logged; if (!logged++) logf_("widescreen FOV at sea: %.3f x %.3f -> %.3f x %.3f", br, bt, r2, t2);
        }
    } else if (r != br || t != bt) { f[0] = -br; f[1] = br; f[2] = bt; f[3] = -bt; cam_refresh(cam); }
}

/* ------------------------------------------------------------------ crash reporter (writes to PiratesCoop.log) */
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *e)
{
    static int n;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *x = e->ContextRecord;
    HMODULE m = 0; char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)x->Eip, &m))
        GetModuleFileNameA(m, mod, MAX_PATH);
    if (StrStrIA(mod, "kernel")) return EXCEPTION_CONTINUE_SEARCH;     /* IsBadReadPtr probes etc. */
    if (n++ >= 8) return EXCEPTION_CONTINUE_SEARCH;
    logf_("EXCEPTION (first chance) %08lx at %08lx (%s+%lx) addr %08lx | eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx",
          c, x->Eip, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, x->Eip - (DWORD)m,
          e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0,
          x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp);
    if (!IsBadReadPtr((void *)x->Esp, 64)) {
        DWORD *sp = (DWORD *)x->Esp;
        logf_("   stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8], sp[9], sp[10], sp[11]);
    }
    logf_("   mod state: ctx %d mode %s coop %d pad_mode %d osk %d", ctx_now(), g_mode_str ? g_mode_str : "?", g_coop_on, g_pad_mode, g_osk_active);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ Direct3D 9 overlay */
typedef HRESULT (WINAPI *t_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *t_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (WINAPI *t_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef IDirect3D9 *(WINAPI *t_D3DCreate9)(UINT);
static t_Present o_Present; static t_Reset o_Reset; static t_CreateDevice o_CreateDevice; static t_D3DCreate9 o_D3DCreate9;
static IDirect3DTexture9 *g_font; static IDirect3DStateBlock9 *g_sb; static IDirect3DDevice9 *g_dev;

typedef struct { float x, y, z, rhw; DWORD c; float u, v; } VTX;
#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void overlay_release(void)
{
    if (g_sb) { IDirect3DStateBlock9_Release(g_sb); g_sb = 0; }
}

static int overlay_init(IDirect3DDevice9 *d)
{
    if (g_dev != d) { overlay_release(); if (g_font) { IDirect3DTexture9_Release(g_font); g_font = 0; } g_dev = d; }
    if (!g_font) {
        if (FAILED(IDirect3DDevice9_CreateTexture(d, FONT_TEX_W, FONT_TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_font, 0))) { g_font = 0; return 0; }
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_font, 0, &lr, 0, 0))) {
            for (int y = 0; y < FONT_TEX_H; y++) {
                DWORD *row = (DWORD *)((char *)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < FONT_TEX_W; x++) row[x] = ((DWORD)font_alpha[y * FONT_TEX_W + x] << 24) | 0xffffff;
            }
            IDirect3DTexture9_UnlockRect(g_font, 0);
        }
    }
    if (!g_sb && FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb))) { g_sb = 0; return 0; }
    return 1;
}

static int text_width(const char *s, float sc) { float w = 0; for (; *s; s++) { int c = (unsigned char)*s - 32; if (c < 0 || c > 95) c = 0; w += font_adv[c] * sc; } return (int)w; }

static int add_text(VTX *v, int n, int max, const char *s, float x, float y, float sc, DWORD col)
{
    for (; *s && n + 6 <= max; s++) {
        int c = (unsigned char)*s - 32; if (c < 0 || c > 95) c = 0;
        float u0 = (c % 16) * FONT_CW / (float)FONT_TEX_W, v0 = (c / 16) * FONT_CH / (float)FONT_TEX_H;
        float u1 = u0 + FONT_CW / (float)FONT_TEX_W, v1 = v0 + FONT_CH / (float)FONT_TEX_H;
        float x1 = x + FONT_CW * sc, y1 = y + FONT_CH * sc;
        VTX q[6] = { {x, y, 0, 1, col, u0, v0}, {x1, y, 0, 1, col, u1, v0}, {x, y1, 0, 1, col, u0, v1},
                     {x1, y, 0, 1, col, u1, v0}, {x1, y1, 0, 1, col, u1, v1}, {x, y1, 0, 1, col, u0, v1} };
        memcpy(v + n, q, sizeof q); n += 6; x += font_adv[c] * sc;
    }
    return n;
}

static VTX g_vtx[6 * 3000];

static RECT g_present_src; static int g_present_src_on;
static int g_cnt_present, g_cnt_sc, g_cnt_es_bb, g_cnt_es_other, g_cnt_drawn;
static void overlay_draw(IDirect3DDevice9 *d)
{
    DWORD now = GetTickCount();
    char lines[MAX_NOTES][160]; DWORD alpha[MAX_NOTES]; int nl = 0;
    int idx[MAX_NOTES], ni = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_NOTES; i++) if (notes[i].until > now && notes[i].text[0]) idx[ni++] = i;
    for (int i = 1; i < ni; i++) for (int j = i; j > 0 && notes[idx[j]].born < notes[idx[j - 1]].born; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    for (int i = 0; i < ni; i++) {
        strcpy(lines[nl], notes[idx[i]].text);
        DWORD left = notes[idx[i]].until - now; alpha[nl] = left < 1000 ? left * 255 / 1000 : 255;
        nl++;
    }
    LeaveCriticalSection(&g_cs);
    int show_osk = cfg.overlay_test;
    int ctxb = ctx_now();
    int bars = cfg.pillarbox && ctxb != CTX_SAIL && ctxb != CTX_BATTLE;
    if (!nl && !show_osk && !bars && !g_mm_open) return;
    if (!overlay_init(d)) return;
    /* always draw on the back buffer with a full-screen viewport (the game may leave another target/viewport set) */
    IDirect3DSurface9 *old_rt = 0, *old_ds = 0, *bb = 0; D3DVIEWPORT9 old_vp;
    IDirect3DDevice9_GetViewport(d, &old_vp);
    IDirect3DDevice9_GetRenderTarget(d, 0, &old_rt);
    IDirect3DDevice9_GetDepthStencilSurface(d, &old_ds);
    D3DVIEWPORT9 vp = old_vp;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC sd; IDirect3DSurface9_GetDesc(bb, &sd);
        IDirect3DDevice9_SetRenderTarget(d, 0, bb);
        IDirect3DDevice9_SetDepthStencilSurface(d, 0);
        vp.X = 0; vp.Y = 0; vp.Width = sd.Width; vp.Height = sd.Height; vp.MinZ = 0; vp.MaxZ = 1;
        if (g_present_src_on && g_present_src.right > g_present_src.left + 100 && g_present_src.bottom > g_present_src.top + 100 &&
            g_present_src.right <= (LONG)sd.Width && g_present_src.bottom <= (LONG)sd.Height) {
            /* the game presents only part of the back buffer: draw inside that part */
            vp.X = g_present_src.left; vp.Y = g_present_src.top;
            vp.Width = g_present_src.right - g_present_src.left; vp.Height = g_present_src.bottom - g_present_src.top;
        }
        IDirect3DDevice9_SetViewport(d, &vp);
        static int once; if (!once++) logf_("overlay: back buffer %ux%u, game viewport was %lu,%lu %lux%lu, target %s", sd.Width, sd.Height,
            old_vp.X, old_vp.Y, old_vp.Width, old_vp.Height, old_rt == bb ? "back buffer" : "OTHER surface");
    }
    float sc = vp.Height / 1000.0f; if (sc < 0.5f) sc = 0.5f;
    float lh = FONT_CH * sc * 1.0f, maxw = vp.Width * 0.92f;
    int n = 0;
    static VTX bg[6 * 120]; int nb = 0;
    if (bars && vp.Width * 3 > vp.Height * 4 + 8) {
        /* black side bars: everything away from the open sea is shown 4:3 in the middle (as the game was made) */
        float bw = (vp.Width - vp.Height * 4.0f / 3.0f) / 2, x0 = (float)vp.X, x1 = vp.X + bw, x2 = vp.X + vp.Width - bw, x3 = (float)(vp.X + vp.Width);
        float y0 = (float)vp.Y, y1 = (float)(vp.Y + vp.Height); DWORD k = 0xff000000;
        VTX q[12] = { {x0, y0, 0, 1, k, 0, 0}, {x1, y0, 0, 1, k, 0, 0}, {x0, y1, 0, 1, k, 0, 0}, {x1, y0, 0, 1, k, 0, 0}, {x1, y1, 0, 1, k, 0, 0}, {x0, y1, 0, 1, k, 0, 0},
                      {x2, y0, 0, 1, k, 0, 0}, {x3, y0, 0, 1, k, 0, 0}, {x2, y1, 0, 1, k, 0, 0}, {x3, y0, 0, 1, k, 0, 0}, {x3, y1, 0, 1, k, 0, 0}, {x2, y1, 0, 1, k, 0, 0} };
        memcpy(bg, q, sizeof q); nb = 12;
    }
    float y = vp.Y + vp.Height * 0.05f;
    for (int i = 0; i < nl; i++) {
        /* word-wrap this notification into rows that fit the screen */
        char rows[6][160]; int nr = 0; const char *p = lines[i];
        while (*p && nr < 6) {
            int len = (int)strlen(p), cut = len;
            char tmp[160];
            for (;;) {
                lstrcpynA(tmp, p, cut + 1);
                if (text_width(tmp, sc) <= maxw || cut <= 1) break;
                int c = cut - 1; while (c > 0 && p[c] != ' ') c--;
                cut = c > 0 ? c : cut - 1;
            }
            lstrcpynA(rows[nr++], p, cut + 1);
            p += cut; while (*p == ' ') p++;
        }
        float w = 0; for (int r = 0; r < nr; r++) { float rw = (float)text_width(rows[r], sc); if (rw > w) w = rw; }
        DWORD a = alpha[i];
        DWORD bc = ((a * 160 / 255) << 24) | 0x101828;
        float bx0 = vp.X + (vp.Width - w) / 2 - 10 * sc, bx1 = vp.X + (vp.Width + w) / 2 + 10 * sc;
        float by0 = y - 3 * sc, by1 = y + lh * nr + 2 * sc;
        if (nb + 6 <= 6 * 120) {
            VTX q[6] = { {bx0, by0, 0, 1, bc, 0, 0}, {bx1, by0, 0, 1, bc, 0, 0}, {bx0, by1, 0, 1, bc, 0, 0},
                         {bx1, by0, 0, 1, bc, 0, 0}, {bx1, by1, 0, 1, bc, 0, 0}, {bx0, by1, 0, 1, bc, 0, 0} };
            memcpy(bg + nb, q, sizeof q); nb += 6;
        }
        for (int r = 0; r < nr; r++) {
            float rw = (float)text_width(rows[r], sc), x = vp.X + (vp.Width - rw) / 2;
            n = add_text(g_vtx, n, 6 * 3000, rows[r], x + 2 * sc, y + 2 * sc, sc, (a << 24) | 0x000000);
            n = add_text(g_vtx, n, 6 * 3000, rows[r], x, y, sc, (a << 24) | 0xffe8a0);
            y += lh;
        }
        y += 8 * sc;
    }
    /* on-screen keyboard */
    if (show_osk) {
        float ks = sc * 1.3f;
        float cw = (text_width("Clear", ks) + 22 * ks), ch = FONT_CH * ks + 14 * ks;
        if (cw * OSK_C > vp.Width * 0.96f) { float f = vp.Width * 0.96f / (cw * OSK_C); cw *= f; ch *= f; ks *= f; }
        float kx0 = vp.X + (vp.Width - cw * OSK_C) / 2, ky0 = vp.Y + vp.Height - ch * OSK_R - lh * 1.6f - 20 * sc;
        #define BOX(x0, y0, x1, y1, col) do { if (nb + 6 <= 6 * 120) { VTX q_[6] = { {x0, y0, 0, 1, col, 0, 0}, {x1, y0, 0, 1, col, 0, 0}, {x0, y1, 0, 1, col, 0, 0}, \
            {x1, y0, 0, 1, col, 0, 0}, {x1, y1, 0, 1, col, 0, 0}, {x0, y1, 0, 1, col, 0, 0} }; memcpy(bg + nb, q_, sizeof q_); nb += 6; } } while (0)
        BOX(kx0 - 12 * sc, ky0 - 12 * sc, kx0 + cw * OSK_C + 12 * sc, ky0 + ch * OSK_R + lh * 1.6f + 8 * sc, 0xd0101828);
        for (int r = 0; r < OSK_R; r++) for (int c = 0; c < OSK_C; c++) {
            float x0 = kx0 + c * cw, y0 = ky0 + r * ch;
            int sel = (r == osk_r && c == osk_c);
            BOX(x0 + 3 * sc, y0 + 3 * sc, x0 + cw - 3 * sc, y0 + ch - 3 * sc, sel ? 0xf0e0a020 : 0xc0384860);
            const char *k = osk_keys[r][c]; char lab[8];
            lstrcpynA(lab, k, sizeof lab);
            if (lab[1] == 0 && lab[0] >= 'A' && lab[0] <= 'Z' && !osk_caps) lab[0] += 32;
            float tw = (float)text_width(lab, ks);
            n = add_text(g_vtx, n, 6 * 3000, lab, x0 + (cw - tw) / 2, y0 + (ch - FONT_CH * ks) / 2, ks, sel ? 0xff101010 : 0xffffffff);
        }
        const char *help = "A type   B delete   X space   Y capitals   START done";
        float hw = (float)text_width(help, sc);
        n = add_text(g_vtx, n, 6 * 3000, help, vp.X + (vp.Width - hw) / 2, ky0 + ch * OSK_R + 6 * sc, sc, 0xffffe8a0);
        #undef BOX
    }
    /* PC8: mod menu panel */
    if (g_mm_open) {
        float ms = sc * 0.95f, rh = FONT_CH * ms * 1.15f;
        char ln[MM_N][160]; float w = 0;
        for (int i = 0; i < MM_N; i++) { mm_line(i, ln[i], sizeof ln[i]); float tw = (float)text_width(ln[i], ms); if (tw > w) w = tw; }
        const char *title = "MOD MENU", *help = "Up/Down choose   Left/Right change   A = do it   B = close";
        float hw = (float)text_width(help, ms * 0.8f); if (hw > w) w = hw;
        float pw = w + 60 * sc, ph = rh * (MM_N + 4.2f);
        float px = vp.X + (vp.Width - pw) / 2, py = vp.Y + (vp.Height - ph) / 2;
        #define MBOX(x0, y0, x1, y1, col) do { if (nb + 6 <= 6 * 120) { VTX q_[6] = { {x0, y0, 0, 1, col, 0, 0}, {x1, y0, 0, 1, col, 0, 0}, {x0, y1, 0, 1, col, 0, 0}, \
            {x1, y0, 0, 1, col, 0, 0}, {x1, y1, 0, 1, col, 0, 0}, {x0, y1, 0, 1, col, 0, 0} }; memcpy(bg + nb, q_, sizeof q_); nb += 6; } } while (0)
        MBOX(px, py, px + pw, py + ph, 0xe0101828);
        MBOX(px, py, px + pw, py + 3 * sc, 0xffe0a020);
        float tw = (float)text_width(title, ms * 1.2f);
        n = add_text(g_vtx, n, 6 * 3000, title, px + (pw - tw) / 2, py + rh * 0.3f, ms * 1.2f, 0xffffe8a0);
        for (int i = 0; i < MM_N; i++) {
            float y0 = py + rh * (1.8f + i);
            int sel = (i == g_mm_sel);
            if (sel) MBOX(px + 12 * sc, y0 - 2 * sc, px + pw - 12 * sc, y0 + rh - 2 * sc, 0xf0e0a020);
            n = add_text(g_vtx, n, 6 * 3000, ln[i], px + 30 * sc, y0, ms, sel ? 0xff101010 : 0xffffffff);
        }
        n = add_text(g_vtx, n, 6 * 3000, help, px + (pw - hw) / 2, py + rh * (MM_N + 2.0f), ms * 0.8f, 0xffb0c0d0);
        if (g_mm_msg[0] && (int)(g_mm_msg_until - now) > 0) {
            float mw = (float)text_width(g_mm_msg, ms * 0.85f), mx = px + (pw - mw) / 2; if (mx < vp.X + 4) mx = vp.X + 4.0f;
            n = add_text(g_vtx, n, 6 * 3000, g_mm_msg, mx, py + rh * (MM_N + 3.0f), ms * 0.85f, 0xff80ff80);
        }
        #undef MBOX
    }
    IDirect3DStateBlock9_Capture(g_sb);
    IDirect3DDevice9_SetVertexShader(d, 0); IDirect3DDevice9_SetPixelShader(d, 0);
    IDirect3DDevice9_SetFVF(d, VTX_FVF);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    /* pass 1: boxes */
    IDirect3DDevice9_SetTexture(d, 0, 0);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (nb) IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, nb / 3, bg, sizeof(VTX));
    /* pass 2: text */
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)g_font);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    if (n) IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLELIST, n / 3, g_vtx, sizeof(VTX));
    IDirect3DStateBlock9_Apply(g_sb);
    if (bb) {
        IDirect3DDevice9_SetRenderTarget(d, 0, old_rt ? old_rt : bb);
        IDirect3DDevice9_SetDepthStencilSurface(d, old_ds);
        IDirect3DDevice9_SetViewport(d, &old_vp);
        IDirect3DSurface9_Release(bb);
    }
    if (old_rt) IDirect3DSurface9_Release(old_rt);
    if (old_ds) IDirect3DSurface9_Release(old_ds);
}

static void frustum_tick(void);
static void frame_tick(IDirect3DDevice9 *d, int path)
{
    static int welcomed, seen[2];
    if (!seen[path]) { seen[path] = 1; logf_("overlay drawn from %s", path ? "EndScene" : "Present"); }
    coop_tick();
    yard_tick();
    if (cfg.hide_cursor && g_pad_mode == 1 && o_SetCursor && GetCursor()) o_SetCursor(NULL);
    if (!welcomed) {
        welcomed = 1;
        int n = (g_p1 >= 0) + (g_p2 >= 0);
        notify_key(NK_PAD, "Pirates! co-op mod %s - %d controller%s connected. Co-op starts with 2 controllers and 2+ ships.",
                   MOD_VERSION, n, n == 1 ? "" : "s");
        if (g_pad_mode == 1 && cfg.hide_cursor) { if (o_SetCursor) o_SetCursor(NULL); }
    }
    frustum_tick();
    overlay_draw(d); g_cnt_drawn++;
    if (g_osk_active) { static int n; if (n++ == 30) logf_("on-screen keyboard being drawn (pad mode %d)", g_pad_mode); } else { static int *dummy; (void)dummy; }
}
typedef HRESULT (WINAPI *t_EndScene)(IDirect3DDevice9 *);
static t_EndScene o_EndScene; static volatile int g_in_overlay; static DWORD g_last_present;
static void count_report(void)
{
    static DWORD t; DWORD now = GetTickCount();
    if (now - t < 5000) return; t = now;
    logf_("frames (5 s): Present %d, swapchain Present %d, EndScene on back buffer %d, EndScene other %d, overlay drawn %d (mode %s)",
          g_cnt_present, g_cnt_sc, g_cnt_es_bb, g_cnt_es_other, g_cnt_drawn, g_mode_str ? g_mode_str : "?");
    g_cnt_present = g_cnt_sc = g_cnt_es_bb = g_cnt_es_other = g_cnt_drawn = 0;
}
static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *d)
{
    if (!g_in_overlay) {
        IDirect3DSurface9 *rt = 0, *bb = 0; int on_bb = 0;
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(d, 0, &rt)) && rt) {
            if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { on_bb = (rt == bb); IDirect3DSurface9_Release(bb); }
            IDirect3DSurface9_Release(rt);
        }
        /* only a fallback for screens whose frames never reach our Present (e.g. the name entry loop) */
        if (on_bb) g_cnt_es_bb++; else g_cnt_es_other++;
        count_report();
        if (on_bb && GetTickCount() - g_last_present > 300) {
            static DWORD last; DWORD now = GetTickCount();
            if (now - last >= 4) { last = now; g_in_overlay = 1; frame_tick(d, 1); g_in_overlay = 0; }
        }
    }
    return o_EndScene(d);
}
static HRESULT WINAPI hk_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    g_last_present = GetTickCount(); g_cnt_present++; count_report();
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(d))) { g_in_overlay = 1; frame_tick(d, 0); g_in_overlay = 0; IDirect3DDevice9_EndScene(d); }
    static RECT la, lb; static int lha = -1, lhb = -1;
    int ha = a != 0, hb = b != 0;
    if (ha != lha || hb != lhb || (a && memcmp(a, &la, sizeof la)) || (b && memcmp(b, &lb, sizeof lb))) {
        logf_("Present source %s %ld,%ld-%ld,%ld  dest %s %ld,%ld-%ld,%ld (mode %s)", a ? "rect" : "whole", a ? a->left : 0, a ? a->top : 0,
              a ? a->right : 0, a ? a->bottom : 0, b ? "rect" : "whole", b ? b->left : 0, b ? b->top : 0, b ? b->right : 0, b ? b->bottom : 0,
              g_mode_str ? g_mode_str : "?");
        lha = ha; lhb = hb; if (a) la = *a; if (b) lb = *b;
    }
    g_present_src_on = a != 0; if (a) g_present_src = *a;
    return o_Present(d, a, b, w, r);
}
typedef HRESULT (WINAPI *t_SCPresent)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static t_SCPresent o_SCPresent;
static HRESULT WINAPI hk_SCPresent(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f)
{
    IDirect3DDevice9 *d = 0;
    (void)d; g_cnt_sc++; count_report();
    return o_SCPresent(sc, a, b, w, r, f);
}
/* side bars: the game clears the screen to blue outside its 4:3 picture -> black */
typedef HRESULT (WINAPI *t_Clear)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static t_Clear o_Clear;
static HRESULT WINAPI hk_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *rc, DWORD fl, D3DCOLOR c, float z, DWORD st)
{
    if (cfg.debug) {
        static int nlog; D3DVIEWPORT9 vp;
        static DWORD lx, ly, lw, lh, ln;
        if (nlog < 40 && SUCCEEDED(d->lpVtbl->GetViewport(d, &vp)) && (vp.X != lx || vp.Y != ly || vp.Width != lw || vp.Height != lh || n != ln)) {
            lx = vp.X; ly = vp.Y; lw = vp.Width; lh = vp.Height; ln = n; nlog++;
            logf_("Clear: viewport %lu,%lu %lux%lu, %lu rects%s, flags %lx colour %08lx (ctx %d)", vp.X, vp.Y, vp.Width, vp.Height, n,
                  n ? "" : "", fl, c, ctx_now());
            for (DWORD i = 0; i < n && i < 4; i++) logf_("   rect %ld,%ld-%ld,%ld", rc[i].x1, rc[i].y1, rc[i].x2, rc[i].y2);
        }
    }
    if ((fl & D3DCLEAR_TARGET) && cfg.black_bars) {
        static D3DCOLOR logged[6]; static int nl;
        int known = 0; for (int i = 0; i < nl; i++) if (logged[i] == c) known = 1;
        if (!known && nl < 6) { logged[nl++] = c; logf_("clear colour %08lx", c); }
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        if (b > 170 && r < 130 && g < 200 && b > r + 60) c = 0xff000000;
    }
    return o_Clear(d, n, rc, fl, c, z, st);
}

typedef HRESULT (WINAPI *t_SetViewport)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
static t_SetViewport o_SetViewport;
static HRESULT WINAPI hk_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *vp)
{
    if (cfg.debug && vp) {
        static D3DVIEWPORT9 seen[24]; static int n;
        int i; for (i = 0; i < n; i++) if (!memcmp(&seen[i], vp, 16)) break;
        if (i == n && n < 24) { seen[n++] = *vp; logf_("SetViewport %lu,%lu %lux%lu (ctx %d, mode %s)", vp->X, vp->Y, vp->Width, vp->Height, ctx_now(), g_mode_str ? g_mode_str : "?"); }
    }
    return o_SetViewport(d, vp);
}
static HRESULT WINAPI hk_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    overlay_release();
    return o_Reset(d, pp);
}

static void rehook(void **vt, int idx, void *fn, void **orig, const char *name)
{
    if (vt[idx] == fn) return;
    logf_("%s hook was replaced (now %p) - hooking again", name, vt[idx]);
    DWORD old; VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[idx];        /* chain to whoever replaced it */
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}
static void hooks_watchdog(void)
{
    static DWORD t; DWORD now = GetTickCount();
    if (!g_cur_dev || now - t < 500) return; t = now;
    void **vt = *(void ***)g_cur_dev;
    if (IsBadReadPtr(vt, 50 * sizeof(void *))) return;
    rehook(vt, 17, (void *)hk_Present, (void **)&o_Present, "Present");
    rehook(vt, 42, (void *)hk_EndScene, (void **)&o_EndScene, "EndScene");
    rehook(vt, 16, (void *)hk_Reset, (void **)&o_Reset, "Reset");
    rehook(vt, 12, (void *)hk_DevShowCursor, (void **)&o_DevShowCursor, "ShowCursor");
    rehook(vt, 43, (void *)hk_Clear, (void **)&o_Clear, "Clear");
    if (cfg.debug) rehook(vt, 47, (void *)hk_SetViewport, (void **)&o_SetViewport, "SetViewport");
}
static void patch_vtable(void **vt, int idx, void *fn, void **orig)
{
    if (vt[idx] == fn) return;
    DWORD old; VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    if (!*orig) *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}

static HRESULT WINAPI hk_CreateDevice(IDirect3D9 *self, UINT ad, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = o_CreateDevice(self, ad, t, w, f, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        patch_vtable(vt, 17, (void *)hk_Present, (void **)&o_Present);
        patch_vtable(vt, 16, (void *)hk_Reset, (void **)&o_Reset);
        patch_vtable(vt, 12, (void *)hk_DevShowCursor, (void **)&o_DevShowCursor);
        patch_vtable(vt, 43, (void *)hk_Clear, (void **)&o_Clear);
        if (cfg.debug) patch_vtable(vt, 47, (void *)hk_SetViewport, (void **)&o_SetViewport);
        patch_vtable(vt, 42, (void *)hk_EndScene, (void **)&o_EndScene);
        IDirect3DSwapChain9 *sc = 0;
        if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(*out, 0, &sc)) && sc) {
            patch_vtable(*(void ***)sc, 3, (void *)hk_SCPresent, (void **)&o_SCPresent);
            IDirect3DSwapChain9_Release(sc);
        }
        g_cur_dev = *out;
        hook_window(pp && pp->hDeviceWindow ? pp->hDeviceWindow : w);
        logf_("D3D9 device created %ux%u windowed=%d, overlay hooked", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    }
    return hr;
}

static IDirect3D9 *WINAPI hk_D3DCreate9(UINT sdk)
{
    IDirect3D9 *d = o_D3DCreate9(sdk);
    if (d) patch_vtable(*(void ***)d, 16, (void *)hk_CreateDevice, (void **)&o_CreateDevice);
    return d;
}

typedef FARPROC (WINAPI *t_GetProcAddress)(HMODULE, LPCSTR);
static t_GetProcAddress o_GetProcAddress;
static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC p = o_GetProcAddress(m, name);
    if (p && (unsigned)name > 0xffff && !strcmp(name, "Direct3DCreate9")) {
        o_D3DCreate9 = (t_D3DCreate9)p;
        logf_("Direct3DCreate9 hooked");
        return (FARPROC)hk_D3DCreate9;
    }
    return p;
}

/* ------------------------------------------------------------------ patching */
static int g_patch_ok = 1;
static void wr(void *at, const void *src, int n)
{
    DWORD old; VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old); FlushInstructionCache(GetCurrentProcess(), at, n);
}
static int patch_call(unsigned site, unsigned expect_target, void *fn)
{
    unsigned char *p = (unsigned char *)site;
    if (p[0] != 0xe8 || site + 5 + G(int, site + 1) != expect_target) {
        logf_("PATCH FAIL call @%08x (expected call %08x)", site, expect_target); g_patch_ok = 0; return 0;
    }
    int rel = (int)fn - (int)(site + 5);
    wr(p + 1, &rel, 4);
    return 1;
}
static int patch_jmp(unsigned site, const unsigned char *expect, int len, void *fn)
{
    if (memcmp((void *)site, expect, len)) { logf_("PATCH FAIL jmp @%08x", site); g_patch_ok = 0; return 0; }
    unsigned char b[16]; memset(b, 0x90, sizeof b); b[0] = 0xe9;
    int rel = (int)fn - (int)(site + 5); memcpy(b + 1, &rel, 4);
    wr((void *)site, b, len);
    return 1;
}

static void install(void)
{
    /* the exe must be the one this mod was built for */
    static const unsigned char sig1[] = { 0x83, 0xec, 0x34, 0x57, 0x33, 0xc0, 0xb9, 0x0d, 0x00, 0x00, 0x00 }; /* 0x4f0ba0 */
    static const unsigned char sig2[] = { 0x83, 0xec, 0x68, 0x53, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x78 };     /* 0x467f90 */
    if (memcmp((void *)0x4f0ba0, sig1, sizeof sig1) || memcmp((void *)0x467f90, sig2, sizeof sig2)) {
        logf_("This Pirates!.exe is not the GOG 2.0.0.4 build the mod was made for - mod disabled.");
        MessageBoxA(NULL, "Pirates! Co-op mod: this Pirates!.exe is not the version the mod was made for "
                    "(GOG 2.0.0.4, game version 1.0.2.0).\n\nThe mod is switched off; the game runs normally.\n"
                    "To remove this message, delete version.dll from the game folder.",
                    "Pirates! Co-op mod", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    /* Direct3D overlay via GetProcAddress IAT slot */
    unsigned iat = 0x6c00b8;
    o_GetProcAddress = (t_GetProcAddress)G(void *, iat);
    void *h = (void *)hk_GetProcAddress; wr((void *)iat, &h, 4);

    /* controller: the game's XInputGetState emulation -> real XInput */
    patch_call(0x4f03f3, 0x4f0ba0, stub_padfill);
    patch_call(0x4f0815, 0x4f0ba0, stub_padfill);
    patch_call(0x42e2a9, 0x42d320, stub_padmap);

    /* crash reporter */
    AddVectoredExceptionHandler(0, crash_handler);
    /* resolution (before the window is made) and widescreen field of view */
    patch_call(0x4d406d, 0x4d1a60, (void *)hk_loadcfg);
    /* 4:3 menu camera kept 4:3 (side bars); wider view at sea is applied per frame (frustum_tick) */
    /* PC2 final: no frustum-setter patch. 3D = game's own full-screen 16:9 view; wider only at sea (frustum_tick);
       fixed 4:3 2D screens (crew sign-up) are fitted by ui_fit_tick. */
    /* 2D layer: full-width backgrounds continued into the widescreen sides */
    static const unsigned char quad[] = { 0x83, 0xec, 0x28, 0xa1, 0x7c, 0x63, 0x72, 0x00 };
    patch_jmp(0x4af080, quad, sizeof quad, (void *)hk_quad);
    /* PC5: renderer guard against the 0x54bc49 crash (object without render state) */
    static const unsigned char regobj[] = { 0x53, 0x8b, 0x5c, 0x24, 0x08 };
    patch_jmp(0x54bc20, regobj, sizeof regobj, stub_regobj);
    { unsigned w2v[] = { 0x422351, 0x437701, 0x437931, 0x437ba2, 0x46200f, 0x463df3, 0x489eb6 };   /* world -> 2D labels */
      for (unsigned i = 0; i < sizeof w2v / 4; i++) patch_call(w2v[i], 0x4ae7d0, (void *)hk_w2v); }
    { unsigned lb[] = { 0x4aff1b, 0x4aff68, 0x46fc98, 0x46fce0, 0x4700f4, 0x47013d };   /* cut-scene letterbox bars */
      for (unsigned i = 0; i < sizeof lb / 4; i++) patch_call(lb[i], 0x4bba60, (void *)hk_letterbox); }
    {   /* PC5: element placement sites (PC13: always installed - trade icons + subtitle band; logging only with Verbose) */
        static const unsigned pl[] = { 0x40536f,0x4128f1,0x412947,0x4180cc,0x418187,0x42fdcf,0x42ffb8,0x430b2a,0x432351,0x43249e,0x43d922,0x43db06,0x43dbdd,0x43de24,0x43ded2,0x43dfd1,0x43f196,0x43f256,0x43f609,0x440876,0x4408d1,0x4409fd,0x440c20,0x440f6d,0x4410ad,0x4410f3,0x4413bf,0x44141f,0x441588,0x44175b,0x441a43,0x441ac2,0x443ed0,0x44416a,0x444393,0x444439,0x444c6b,0x445684,0x445874,0x44753a,0x4475bb,0x44765d,0x4476e9,0x44772d,0x4479c0,0x447b75,0x447b8b,0x448288,0x44835d,0x4483d7,0x448456,0x4484ad,0x44b25d,0x44b368,0x44b45f,0x453cff,0x454008,0x455248,0x455337,0x456740,0x4567d1,0x45778a,0x45782c,0x457c97,0x457d47,0x457e05,0x457ee6,0x457fea,0x4580a9,0x458183,0x458258,0x473ccf,0x473dd3,0x473dfc,0x473e25,0x473e4e,0x473e77,0x473e9d,0x473ec3,0x473ee9,0x473ff3,0x47457f,0x474779,0x474a0f,0x474a47,0x474ab6,0x474b0b,0x474b4d,0x474c61,0x476326,0x476373,0x4763db,0x476490,0x4764c3,0x476516,0x476558,0x47af85,0x47b749,0x47b8db,0x47bb08,0x493b80,0x494199,0x494422,0x4947ff,0x4948ac,0x4b4a8f,0x4b5b85,0x4b6693,0x4b672f,0x4b69cc,0x4b7536,0x4b7641,0x4b78cf,0x4b8bfd,0x4c6811,0x4c6846,0x4d6db2,0x4d6ef5,0x4d7028,0x4debd0,0x4ded90,0x4dfcd0,0x4f086d,0x4f08cb,0x4f09cb };
        for (unsigned i = 0; i < sizeof pl / 4; i++) patch_call(pl[i], 0x4bba60, (void *)hk_place); }
    patch_call(0x44b68b, 0x42e1d0, (void *)hk_infokey);   /* rival pirate info screen: A/B/START close it */
    /* mouse cursor hidden while a controller is used */
    o_SetCursor = (t_SetCursor)G(void *, 0x6c0388);
    { void *hc = (void *)hk_SetCursor; wr((void *)0x6c0388, &hc, 4); }
    /* on-screen keyboard for the name entry screens */
    patch_call(0x471a64, 0x470b90, stub_name_entry);
    patch_call(0x43fc91, 0x471120, stub_ship_name);
    patch_call(0x46eba4, 0x42e1d0, (void *)hk_signup_key);
    patch_call(0x470e43, 0x4d0cd0, stub_osk_frame);
    patch_call(0x471520, 0x4d0cd0, stub_osk_frame);
    /* menus start on the first choice when a controller is used */
    static const unsigned char menu[] = { 0xa1, 0xd8, 0xac, 0x8c, 0x00 };
    patch_jmp(0x430190, menu, sizeof menu, stub_menu);
    /* plunder/trade screen: Continue reachable, A keeps the captured ship */
    patch_call(0x4ddfb7, 0x42e440, stub_tradekey);
    /* shipwright: start on Repair */
    patch_call(0x4d5b37, 0x4d6600, stub_shipyard);
    /* PC8: crew always happy (mod menu) */
    g_happy_on = cfg.happy_crew;
    static const unsigned char mood[] = { 0x0f, 0xbf, 0x05, 0x76, 0x9a, 0x86, 0x00 };
    patch_jmp(0x404810, mood, sizeof mood, stub_happy);
    /* PC9: max fame (mod menu) - fame points function 0x43fd70: push ecx/ebx/ebp/esi, xor ebx,ebx (6 bytes) */
    g_fame_on = cfg.max_fame; g_fame_value = cfg.fame_value;
    static const unsigned char fame[] = { 0x51, 0x53, 0x55, 0x56, 0x33, 0xdb };
    patch_jmp(0x43fd70, fame, sizeof fame, stub_fame);
    /* PC10: never dizzy in duels (mod menu) + merchants always trade while "all nations friendly" is on
       (FUN_0040fe00 = "may trade here", fastcall EDX = town; refuses Spanish towns by rank: "We do not trade with
       @NATIONALITY heretics") */
    dizzy_apply();
    g_friendly_on = cfg.friendly_nations;
    static const unsigned char tradeok[] = { 0x8b, 0xca, 0xc1, 0xe1, 0x05 };
    patch_jmp(0x40fe00, tradeok, sizeof tradeok, stub_trade_ok);
    if (!cfg.enabled) { logf_("co-op disabled in PiratesCoop.ini (controller support only)"); return; }

    /* sailing: every caller of the per-ship sailing update */
    unsigned sail_sites[] = { 0x40dce6, 0x45f611, 0x45f741, 0x4607b2, 0x46b81d, 0x46b960, 0x47255c };
    for (unsigned i = 0; i < sizeof sail_sites / 4; i++) patch_call(sail_sites[i], 0x467f90, (void *)hk_sailship);
    patch_call(0x46857b, 0x4db270, stub_fleet_atan);
    /* PC5: sailing against the wind */
    patch_call(0x468881, 0x477450, stub_wind_sail);
    patch_call(0x47782a, 0x477450, stub_wind_battle);
    static const unsigned char wfm[] = { 0x81, 0xfb, 0x40, 0x42, 0x0f, 0x00, 0x0f, 0x8e, 0x9e, 0x00, 0x00, 0x00 };
    patch_jmp(0x468efa, wfm, sizeof wfm, stub_waitforme);

    /* change flagship: keep Player 2's ship his */
    patch_call(0x43f9f3, 0x404e50, stub_rotate);
    patch_call(0x472c39, 0x404e50, stub_rotate);
    patch_call(0x472dfc, 0x404e50, stub_rotate);

    /* sea battle */
    patch_call(0x480915, 0x47a040, (void *)hk_battle);
    patch_call(0x406db5, 0x4068f0, stub_tavern_pick);                           /* PC11 */
    patch_call(0x503d01, 0x54aa00, stub_btn_frustum);                           /* PC12 */
    patch_call(0x4de162, 0x4e00d0, stub_shipbtn);                               /* PC12 */
    { static const unsigned char surr[] = { 0x83, 0xc4, 0x14, 0x85, 0xc0 }; patch_jmp(0x47e651, surr, sizeof surr, stub_surrender); }   /* PC11 */
    patch_call(0x47bbcd, 0x477500, (void *)hk_bship);
    patch_call(0x477591, 0x42d500, (void *)hk_axis);
    /* PC8: firing next to Player 2's ship, far battle view */
    patch_call(0x47f2fe, 0x477cb0, (void *)hk_fire_p1);
    patch_call(0x47e4e6, 0x477cb0, (void *)hk_fire_volley);
    patch_call(0x47b36d, 0x489360, stub_bcam);
    static const unsigned char hud[] = { 0x8b, 0x44, 0x24, 0x20, 0x83, 0xc4, 0x08, 0x85, 0xc0, 0x0f, 0x85, 0x33, 0x01, 0x00, 0x00 };
    patch_jmp(0x47ba76, hud, sizeof hud, stub_bhud);

    /* sword duels */
    unsigned duel_sites[] = { 0x40582b, 0x408203, 0x40a0b6, 0x40dfa9, 0x40ee23, 0x419990, 0x44e219, 0x45dfa9, 0x4799a6, 0x47fcab };
    for (unsigned i = 0; i < sizeof duel_sites / 4; i++) patch_call(duel_sites[i], 0x41b1d0, stub_duel);
    logf_("patches installed (%s)", g_patch_ok ? "all OK" : "SOME FAILED - see above");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID r)
{
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
        const char *base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
        if (_stricmp(base, "Pirates!.exe")) return TRUE;          /* only inside the game */
        lstrcpynA(g_dir, exe, MAX_PATH); char *sl = strrchr(g_dir, '\\'); if (sl) *sl = 0;
        char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\PiratesCoop.log", g_dir);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        /* PC9 (user): a log is NEVER overwritten or truncated. Over 2 MB it is renamed to
           PiratesCoop_<date>_<time>.log (unique name) and a new PiratesCoop.log is started. */
        int big = GetFileAttributesExA(lp, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 2000000);
        char rotated[MAX_PATH] = "";
        if (big) {
            SYSTEMTIME t; GetLocalTime(&t);
            for (int n = 0; n < 100; n++) {
                snprintf(rotated, sizeof rotated, n ? "%s\\PiratesCoop_%04d%02d%02d_%02d%02d%02d_%d.log" : "%s\\PiratesCoop_%04d%02d%02d_%02d%02d%02d.log",
                         g_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
                if (MoveFileA(lp, rotated)) break;
                rotated[0] = 0;
                if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
            }
        }
        g_log = fopen(lp, "a");                                  /* always append, never "w" */
        if (g_log) fprintf(g_log, "\n===== game started (pid %lu) =====\n", GetCurrentProcessId());
        InitializeCriticalSection(&g_cs);
        load_config();
        logf_("Pirates! co-op mod %s loaded (%s)", MOD_VERSION, exe);
        if (big) logf_(rotated[0] ? "previous log was over 2 MB - kept as %s" : "previous log over 2 MB could not be renamed - appending%s", rotated);
        install();
    }
    return TRUE;
}
