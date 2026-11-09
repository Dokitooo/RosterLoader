/*
 * slam.h - SLAM SDK (Script Loader & Add-on Manager for NBA 2K14, PC 32-bit)
 * ===========================================================================
 *
 * A SLAM mod is a normal .asi (a Windows DLL) that exports two functions:
 *
 *     SlamModInfo()                      who am I (name, version, SLAM API it needs)
 *     SlamModStart(api, self)            SLAM calls this once; start your mod here
 *
 * and calls SlamSdk_OnAttach(hinst) from its DllMain. That last call is what shows the
 * "SLAM is not installed" window when a player has the mod but not SLAM (or a SLAM that is
 * too old): the mod then does nothing and the game boots normally.
 *
 * Minimal mod:
 *
 *     #define SLAM_SDK_IMPLEMENTATION      // in exactly ONE .c file of the mod
 *     #include "slam.h"
 *
 *     SLAM_MOD("HelloSLAM", "1.0.0", "your name")
 *
 *     static const SLAM_API *api; static SlamMod *me;
 *     static void SLAM_CALL ready(void *u) { api->log(me, "game code is ready"); }
 *
 *     SLAM_EXPORT int SLAM_CALL SlamModStart(const SLAM_API *a, SlamMod *self)
 *     { api = a; me = self; api->on_game_ready(me, ready, 0); return 0; }
 *
 *     BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
 *     { if (why == 1) SlamSdk_OnAttach(h); return TRUE; }
 *
 * Rules that keep every mod compatible with every SLAM 1.x:
 *   - SLAM_API only ever GROWS at the end. Check api->size / api->apiVersion before using a
 *     function newer than the SLAM_API_VERSION you need (SLAM_HAS(api, field) does that).
 *   - Hook game functions with api->hook, patch bytes with api->patch. SLAM chains hooks of
 *     several mods on the same function and refuses (and reports) two different patches on the
 *     same bytes, so mods don't silently break each other.
 *   - nba2k14.exe is encrypted on disk and unpacks itself at start: install game hooks in
 *     on_game_ready, not in SlamModStart.
 *
 * License: BSD-2-Clause (same as SLAM). Header-only; no C runtime needed; imports kernel32 only.
 */
#ifndef SLAM_H
#define SLAM_H

#define SLAM_API_VERSION 1          /* bump = new functions appended to SLAM_API */
#define SLAM_CALL __cdecl
#ifdef __cplusplus
#  define SLAM_EXTERN_C extern "C"
#else
#  define SLAM_EXTERN_C
#endif
#define SLAM_EXPORT SLAM_EXTERN_C __declspec(dllexport)

/* results of hook / patch / unhook / unpatch */
#define SLAM_OK             0
#define SLAM_ERR_ARGS      (-1)     /* bad arguments */
#define SLAM_ERR_CONFLICT  (-2)     /* another mod already changed these bytes differently (see SLAM.log) */
#define SLAM_ERR_MEMORY    (-3)     /* address not mapped / not writable */
#define SLAM_ERR_NOT_FOUND (-4)     /* nothing to remove */
#define SLAM_ERR_HOOK      (-5)     /* the hook engine could not hook this address */

typedef struct SlamMod SlamMod;     /* opaque: "this mod", handed to SlamModStart */

typedef void (SLAM_CALL *SlamReadyCb)(void *user);

typedef struct SLAM_ModInfo {
    unsigned    size;               /* sizeof(SLAM_ModInfo) */
    const char *name;               /* "BodyHook" - also the folder name SLAM\<name>\ */
    const char *version;            /* "1.20.0" */
    const char *author;
    unsigned    apiVersion;         /* SLAM_API_VERSION the mod was built with = the SLAM it needs */
} SLAM_ModInfo;

typedef struct SLAM_API {
    unsigned    size;               /* sizeof(SLAM_API) of the running SLAM */
    unsigned    apiVersion;         /* SLAM_API_VERSION of the running SLAM */
    const char *slamVersion;        /* "1.0.0" */

    /* --- paths (all end with a backslash where they are folders) -------------------------- */
    const char *(SLAM_CALL *game_dir)(void);            /* folder of nba2k14.exe */
    const char *(SLAM_CALL *mod_dir)(SlamMod *m);       /* folder the mod's .asi is in */
    const char *(SLAM_CALL *mod_ini)(SlamMod *m);       /* <mod_dir><name>.ini */

    /* --- log: SLAM\<name>\<name>.log (printf subset of wsprintf: %d %u %x %X %s %c %p; %f = 3 decimals) */
    void (SLAM_CALL *log)(SlamMod *m, const char *fmt, ...);

    /* --- memory ------------------------------------------------------------------------- */
    /* Detour `target` to `detour`. *original receives a pointer to call the rest of the chain
       (the previous mod's detour, or the original function). Several mods may hook the same
       function: the mod loaded LAST runs FIRST. */
    int  (SLAM_CALL *hook)(SlamMod *m, void *target, void *detour, void **original);
    int  (SLAM_CALL *unhook)(SlamMod *m, void *target, void *detour);
    /* Write bytes (code or data). Same bytes as another mod's patch = shared; different bytes
       on the same addresses = SLAM_ERR_CONFLICT, nothing written, both mods named in SLAM.log.
       Original bytes are kept for unpatch and for crash reports. */
    int  (SLAM_CALL *patch)(SlamMod *m, void *addr, const void *bytes, unsigned len);
    int  (SLAM_CALL *unpatch)(SlamMod *m, void *addr);
    /* copies len bytes if all of them are readable; returns 1 / 0 */
    int  (SLAM_CALL *read)(const void *addr, void *out, unsigned len);

    /* --- events ------------------------------------------------------------------------- */
    /* once the game's code is unpacked, on the game's main thread, before its own code runs;
       right away if that already happened. */
    int  (SLAM_CALL *on_game_ready)(SlamMod *m, SlamReadyCb cb, void *user);
    /* SLAM itself only shows its mod list for a few seconds at start and then leaves Direct3D alone:
       a mod that needs the device hooks it itself (with api->hook). */

    /* --- misc ----------------------------------------------------------------------------- */
    int  (SLAM_CALL *find_mod)(const char *name, char *versionOut, unsigned cap); /* 1 = that SLAM mod runs */
    void *(SLAM_CALL *game_base)(void);                 /* nba2k14.exe image base */
    unsigned (SLAM_CALL *exe_timestamp)(void);          /* PE TimeDateStamp: which nba2k14.exe build */
    int  (SLAM_CALL *game_is_ready)(void);
    /* one line for the crash report: what the mod was doing (e.g. "loading player 23 body").
       Kept per mod, shown in SLAM\crash\*.log if the game crashes. Cheap: copies <= 95 chars. */
    void (SLAM_CALL *crash_note)(SlamMod *m, const char *text);
    /* Hook a function nba2k14.exe calls through its import table, e.g. ("KERNEL32.dll",
       "CreateFileA"): only the GAME's calls go through your detour (not other DLLs' or mods').
       Several mods chain like api->hook (last loaded runs first). SLAM_ERR_NOT_FOUND = the exe
       doesn't import that function. Never write into the import table yourself. */
    int  (SLAM_CALL *hook_import)(SlamMod *m, const char *dll, const char *function, void *detour, void **original);
    int  (SLAM_CALL *unhook_import)(SlamMod *m, const char *dll, const char *function, void *detour);
} SLAM_API;

#define SLAM_HAS(api, field) ((api)->size > (unsigned)(unsigned long)&((SLAM_API *)0)->field)

/* what SLAM publishes for the SDK stub (named mapping Local\SLAM_core_<pid in hex>) */
typedef struct SLAM_CoreInfo {
    unsigned magic;                 /* SLAM_CORE_MAGIC */
    unsigned apiVersion;
    char     version[16];
    int (SLAM_CALL *adopt)(void *hmodule);  /* "start this already loaded SLAM mod" */
    int (SLAM_CALL *started)(void *hmodule);/* 1 = SLAM starts / started this mod (waits until SLAM's
                                                   loader is done, so it never answers too early) */
} SLAM_CoreInfo;
#define SLAM_CORE_MAGIC 0x4D414C53u /* 'SLAM' */

/* The oldest SLAM API the mod runs on. Default: the API of this header. A mod that uses newer
   functions only after checking SLAM_HAS() can say "#define SLAM_MOD_NEEDS_API 1" before
   including slam.h, so it also runs on older SLAMs. */
#ifndef SLAM_MOD_NEEDS_API
#  define SLAM_MOD_NEEDS_API SLAM_API_VERSION
#endif

/* mods declare themselves with this (once, in any .c file of the mod) */
#define SLAM_MOD(NAME, VERSION, AUTHOR)                                                          \
    static const SLAM_ModInfo slam__info = { sizeof(SLAM_ModInfo), NAME, VERSION, AUTHOR,          \
                                             SLAM_MOD_NEEDS_API };                                 \
    SLAM_EXPORT const SLAM_ModInfo *SLAM_CALL SlamModInfo(void) { return &slam__info; }

SLAM_EXPORT const SLAM_ModInfo *SLAM_CALL SlamModInfo(void);
SLAM_EXPORT int SLAM_CALL SlamModStart(const SLAM_API *api, SlamMod *self);
SLAM_EXTERN_C void SlamSdk_OnAttach(void *hinstance);   /* call from DllMain(DLL_PROCESS_ATTACH) */
/* Include <windows.h> before this header in the file that defines SLAM_SDK_IMPLEMENTATION. */

/* ===================================================================================== */
#ifdef SLAM_SDK_IMPLEMENTATION
/* Everything below runs inside the MOD. Needs <windows.h> included first (kernel32 only;
   user32 is loaded only when the window has to be shown). */

static void *slamsdk_inst;

static void slamsdk_hex(char *d, unsigned v) { int i; for (i = 0; i < 8; i++) { unsigned x = (v >> (28 - 4 * i)) & 15; d[i] = (char)(x < 10 ? '0' + x : 'A' + x - 10); } d[8] = 0; }
static void slamsdk_cat(char *d, const char *s, int cap) { int n = 0; while (d[n]) n++; while (*s && n < cap - 1) d[n++] = *s++; d[n] = 0; }

/* shared between all SLAM mods in the game: who is missing SLAM (named mapping Local\SLAM_missing_<pid>) */
typedef struct { volatile LONG count; volatile LONG lastTick; char names[32][64]; } slamsdk_missing_t;
typedef int (WINAPI *slamsdk_box_t)(HWND, LPCSTR, LPCSTR, UINT);

static DWORD WINAPI slamsdk_thread(LPVOID unused)
{
    char name[64], hex[9], line[64];
    const SLAM_ModInfo *mi = SlamModInfo();
    HANDLE coreMap, h;
    SLAM_CoreInfo *core = 0;
    slamsdk_missing_t *miss;
    int i, owner;
    (void)unused;
    slamsdk_hex(hex, (unsigned)GetCurrentProcessId());

    /* SLAM as dinput8.dll is loaded by Windows before any ASI loader runs, so it is usually there at
       once; SLAM.asi loaded by another ASI loader may come a moment later: look for up to 1 s. */
    name[0] = 0; slamsdk_cat(name, "Local\\SLAM_core_", 64); slamsdk_cat(name, hex, 64);
    coreMap = 0;
    for (i = 0; i < 10; i++) {
        if (!coreMap) coreMap = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
        if (coreMap && !core) core = (SLAM_CoreInfo *)MapViewOfFile(coreMap, FILE_MAP_READ, 0, 0, sizeof(SLAM_CoreInfo));
        if (core && core->magic == SLAM_CORE_MAGIC) break;      /* SLAM writes magic last: record complete */
        Sleep(100);
    }
    if (core && core->magic != SLAM_CORE_MAGIC) core = 0;
    if (core && core->apiVersion >= SLAM_MOD_NEEDS_API) {
        /* SLAM is there. If another ASI loader loaded this mod, ask SLAM to start it. */
        if (!(core->started && core->started(slamsdk_inst)) && core->adopt) core->adopt(slamsdk_inst);
        return 0;
    }

    /* SLAM missing or too old: add this mod to the shared list; the first mod shows the window */
    line[0] = 0; slamsdk_cat(line, mi && mi->name ? mi->name : "a mod", 48);
    if (core) { slamsdk_cat(line, " (needs a newer SLAM than ", 64); slamsdk_cat(line, core->version, 64); slamsdk_cat(line, ")", 64); }
    name[0] = 0; slamsdk_cat(name, "Local\\SLAM_missing_", 64); slamsdk_cat(name, hex, 64);
    h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(slamsdk_missing_t), name);
    if (!h) return 0;
    owner = GetLastError() != ERROR_ALREADY_EXISTS;
    miss = (slamsdk_missing_t *)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(slamsdk_missing_t));
    if (!miss) return 0;
    i = (int)InterlockedIncrement(&miss->count) - 1;
    if (i < 32) { int n = 0; while (line[n] && n < 63) { miss->names[i][n] = line[n]; n++; } miss->names[i][n] = 0; }
    miss->lastTick = (LONG)GetTickCount();
    if (!owner) return 0;      /* the mapping (and the list) stays open for the window owner */
    /* wait until no more mods report in for 0.4 s (max 1.5 s), then one window for all of them -
       quick, so it is up before the game switches to fullscreen */
    for (i = 0; i < 15 && (LONG)GetTickCount() - miss->lastTick < 400; i++) Sleep(100);
    {
        static char msg[2600];
        int n = miss->count < 32 ? (int)miss->count : 32, j;
        HMODULE u32 = LoadLibraryA("user32.dll");
        slamsdk_box_t box = u32 ? (slamsdk_box_t)GetProcAddress(u32, "MessageBoxA") : 0;
        msg[0] = 0;
        slamsdk_cat(msg, core ? "Your SLAM is too old for these mods, so they won't work:\n\n"
                              : "SLAM is not installed, so these mods won't work:\n\n", sizeof(msg));
        for (j = 0; j < n; j++) { slamsdk_cat(msg, "    - ", sizeof(msg)); slamsdk_cat(msg, miss->names[j], sizeof(msg)); slamsdk_cat(msg, "\n", sizeof(msg)); }
        slamsdk_cat(msg, "\nInstall the latest SLAM (dinput8.dll + the SLAM folder) next to nba2k14.exe.\n"
                         "The game will start normally without these mods.", sizeof(msg));
        if (box) box(NULL, msg, core ? "SLAM is too old" : "SLAM not installed",
                     MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    return 0;
}

SLAM_EXTERN_C void SlamSdk_OnAttach(void *hinstance)
{
    HANDLE t;
    slamsdk_inst = hinstance;
    t = CreateThread(NULL, 0, slamsdk_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#endif /* SLAM_SDK_IMPLEMENTATION */
#endif /* SLAM_H */
