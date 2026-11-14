/* RosterLoader - NBA 2K14: loads a saved roster (.ROS) at boot, through the game's own Load.
 *
 *   RosterLoader.ini  [RosterLoader]
 *     AutoLoad=KMTS2K 26v7 EX.ROS   roster file in the game's Saves folder (empty = the last roster you loaded, below)
 *     AutoLoadLast=1                1.2: with AutoLoad empty, load the roster you loaded last time in the game
 *     LastLoaded=...                1.2: written by RosterLoader whenever the game loads a .ROS
 *     Delay=0                       ms after the frontend starts (0 = its first frame, before the main menu is built)
 *     HidePopup=1                   the screen stays black while it loads (no "Loading Roster..." box)
 *
 *     Fast=1                        no frames drawn while loading (no 60 fps pacing)
 *     Warp=8                        game clock x8 while loading (the box's timed waits)
 *
 * How (found with the 0.1-0.6 probes): the menu's Load Roster calls 004E9300(ecx ctx, edx, arg1, wide file name)
 * (menu 00521A3B; ctx 0 / arg1 0 also refresh the save list first) and returns 0 when loaded. It has to run on the
 * game thread in the frontend's update 004E0190 (from PeekMessageA the game hung).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "slam.h"
#include <d3d9.h>

extern const SLAM_API *g_slam; extern SlamMod *g_slamMe; extern void *g_slamInst;
void glue_migrate(const char *file);

typedef unsigned int u32; typedef unsigned char u8; typedef unsigned short u16;
typedef int (__fastcall *Load_t)(u32 ecx, u32 edx, u32 arg1, const u16 *name);

static char g_log[MAX_PATH], g_name[MAX_PATH];
static u16 g_wname[MAX_PATH];
static CRITICAL_SECTION g_lk;
static void *g_origTick;
static int cfg_delay, cfg_hide, g_done, cfg_last = 1;
static DWORD g_t0;

static void rlog(const char *fmt, ...)
{
    char b[1200]; int n; va_list ap; HANDLE f; DWORD w; SYSTEMTIME t;
    GetLocalTime(&t);
    n = wsprintfA(b, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt); n += wvsprintfA(b + n, fmt, ap); va_end(ap);
    b[n++] = '\r'; b[n++] = '\n';
    EnterCriticalSection(&g_lk);
    f = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, n, &w, 0); CloseHandle(f); }
    LeaveCriticalSection(&g_lk);
}

/* 0.9: the game's "Loading Roster ... from system memory" box is drawn by frames presented while the load runs
 * (the load itself runs inside that box's update). With HidePopup the frames presented during our boot load are
 * dropped: the screen keeps the last picture until the roster is in. Present = IDirect3DDevice9 vtable slot 17, found
 * with a small device made before the game makes its own. */
/* 0.13: the box's waits are timed (fade in / minimum time on screen): while our boot load runs the game's clock
 * (QueryPerformanceCounter) runs Warp times faster. Afterwards it continues from there (never goes back). */
typedef BOOL (WINAPI *QPC_t)(LARGE_INTEGER *);
static QPC_t g_qpc; static volatile int g_warp; static int cfg_warp = 8;
static long long g_qOff, g_qBaseReal, g_qBaseOut, g_qLast;
static CRITICAL_SECTION g_qlk;
static BOOL WINAPI MyQPC(LARGE_INTEGER *o)
{
    long long r, v; BOOL ok;
    if (!g_qpc) g_qpc = (QPC_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryPerformanceCounter");
    ok = g_qpc(o);
    if (!ok || !o) return ok;
    r = o->QuadPart;
    EnterCriticalSection(&g_qlk);
    if (g_warp) {
        if (!g_qBaseReal) { g_qBaseReal = r; g_qBaseOut = r + g_qOff; }
        v = g_qBaseOut + (r - g_qBaseReal) * cfg_warp;
    } else {
        if (g_qBaseReal) { g_qOff = g_qLast - r; g_qBaseReal = 0; }   /* warp ended: keep going from where it was */
        v = r + g_qOff;
    }
    if (v < g_qLast) v = g_qLast;
    g_qLast = v;
    LeaveCriticalSection(&g_qlk);
    o->QuadPart = v;
    return ok;
}

typedef HRESULT (WINAPI *Present_t)(void *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r);
int _fltused = 1;
typedef VOID (WINAPI *KE_t)(BYTE, BYTE, DWORD, ULONG_PTR);
static volatile int g_fellBack;
static KE_t g_keybd; static volatile LONG g_enters; static int cfg_fast = 1;
static Present_t g_origPresent;

static volatile int g_hide; static volatile LONG g_dropped;
static HWND g_gameWnd; static volatile int g_bgEnter; static volatile DWORD g_loadT0;
typedef BOOL (WINAPI *GKS_t)(PBYTE);
static GKS_t g_origGKS;
static BOOL WINAPI MyGKS(PBYTE k)   /* background load: the game's key state shows Enter too */
{
    BOOL r = g_origGKS(k);
    if (r && k && g_hide) k[VK_RETURN] = g_bgEnter ? 0x80 : 0;
    return r;
}

/* ---- 1.0.1: Enter for the load box straight into the game's DirectInput keyboard (works while the game is NOT in
 * front - alt-tab while booting): IDirectInputDevice8 GetDeviceState (slot 9) / GetDeviceData (slot 10) are hooked;
 * while a load runs, the keyboard device reports Enter pressed / released on alternate polls, even if Windows took
 * the keyboard away from the game (not acquired). keybd_event is no longer used when this hook is in. */
typedef HRESULT (WINAPI *GDS_t)(void *dev, DWORD cb, void *data);
typedef HRESULT (WINAPI *GDD_t)(void *dev, DWORD cb, void *od, DWORD *n, DWORD fl);
typedef HRESULT (WINAPI *GDI_t)(void *dev, void *inst);
static GDS_t g_origGDS; static GDD_t g_origGDD; static volatile int g_diOn; static volatile LONG g_diEnters;
static void *g_kbDev[8]; static void *g_otherDev[8];
static int is_kb(void *dev)
{
    int i; u8 inst[0x244]; GDI_t gdi;
    for (i = 0; i < 8; i++) { if (g_kbDev[i] == dev) return 1; if (g_otherDev[i] == dev) return 0; }
    for (i = 0; i < (int)sizeof inst; i++) inst[i] = 0;
    *(DWORD *)inst = sizeof inst;
    gdi = (GDI_t)(*(void ***)dev)[15];
    if (FAILED(gdi(dev, inst))) return 0;
    if ((*(DWORD *)(inst + 36) & 0xFF) == 0x13) { for (i = 0; i < 8 && g_kbDev[i]; i++); if (i < 8) g_kbDev[i] = dev; return 1; }   /* DI8DEVTYPE_KEYBOARD */
    for (i = 0; i < 8 && g_otherDev[i]; i++); if (i < 8) g_otherDev[i] = dev;
    return 0;
}
static HRESULT WINAPI MyGDS(void *dev, DWORD cb, void *data)
{
    HRESULT r = g_origGDS(dev, cb, data);
    if (g_hide && g_bgEnter >= 0 && GetForegroundWindow() != g_gameWnd && cb == 256 && data && is_kb(dev)) {
        static int t;
        if (FAILED(r)) { DWORD i; for (i = 0; i < 256; i++) ((u8 *)data)[i] = 0; r = 0; }   /* not acquired (not in front): an empty keyboard */
        t = !t; ((u8 *)data)[0x1C] = t ? 0x80 : 0;                                          /* DIK_RETURN */
        if (t) InterlockedIncrement(&g_diEnters);
    }
    return r;
}
static HRESULT WINAPI MyGDD(void *dev, DWORD cb, void *od, DWORD *n, DWORD fl)
{
    DWORD cap = n ? *n : 0;
    HRESULT r = g_origGDD(dev, cb, od, n, fl);
    if (g_hide && GetForegroundWindow() != g_gameWnd && od && n && cb >= 16 && !(fl & 1) && cap && is_kb(dev)) {               /* not DIGDD_PEEK */
        static int t; u8 *e;
        if (FAILED(r)) { *n = 0; r = 0; }
        if (*n < cap) { e = (u8 *)od + *n * cb; { DWORD i; for (i = 0; i < cb; i++) e[i] = 0; }
            t = !t; ((DWORD *)e)[0] = 0x1C; ((DWORD *)e)[1] = t ? 0x80 : 0; ((DWORD *)e)[2] = GetTickCount();
            (*n)++; if (t) InterlockedIncrement(&g_diEnters); }
    }
    return r;
}
static void hook_dinput(void)
{
    typedef HRESULT (WINAPI *DI8C_t)(HINSTANCE, DWORD, const GUID *, void **, void *);
    static const GUID iid = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };   /* IID_IDirectInput8A */
    static const GUID kb = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };    /* GUID_SysKeyboard */
    char sys[MAX_PATH]; HMODULE m; DI8C_t c; void *di = 0, *dev = 0; int a, b;
    GetSystemDirectoryA(sys, MAX_PATH); lstrcatA(sys, "\\dinput8.dll");
    m = LoadLibraryA(sys); c = m ? (DI8C_t)GetProcAddress(m, "DirectInput8Create") : 0;
    if (!c || FAILED(c(GetModuleHandleA(0), 0x0800, &iid, &di, 0)) || !di) { rlog("HidePopup: DirectInput not reached - Enter is pressed the old way (game must be in front)"); return; }
    if (FAILED(((HRESULT (WINAPI *)(void *, const GUID *, void **, void *))(*(void ***)di)[3])(di, &kb, &dev, 0)) || !dev) {
        ((ULONG (WINAPI *)(void *))(*(void ***)di)[2])(di); rlog("HidePopup: no keyboard device - Enter is pressed the old way"); return; }
    a = g_slam->hook(g_slamMe, (*(void ***)dev)[9], (void *)MyGDS, (void **)&g_origGDS);
    b = g_slam->hook(g_slamMe, (*(void ***)dev)[10], (void *)MyGDD, (void **)&g_origGDD);
    ((ULONG (WINAPI *)(void *))(*(void ***)dev)[2])(dev); ((ULONG (WINAPI *)(void *))(*(void ***)di)[2])(di);
    g_diOn = a == SLAM_OK && b == SLAM_OK;
    rlog("HidePopup: Enter straight into the game's keyboard (works with the game in the background): %s", g_diOn ? "ok" : "FAILED - old way");
}
static HRESULT WINAPI MyPresent(void *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    /* 0.12: the load waits for Enter on its box (0.9's "hang", 0.10/0.11's wait): Enter is pressed for it, and the
     * frames are not shown at all (no 60 fps pacing -> the load runs at full speed) */
    if (g_hide && GetTickCount() - g_loadT0 > 10000) {
        /* safety (log d9814f4a: black screen): nothing answered the box in 10 s - the game's own box is shown */
        g_hide = 0; g_warp = 0; g_fellBack = 1;
    }
    if (g_hide) {
        static int down;
        DWORD pid = 0; HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId() && g_keybd) {           /* in front: the Enter that always worked */
            g_gameWnd = fg; g_bgEnter = 0;
            g_keybd(VK_RETURN, 0x1C, down ? KEYEVENTF_KEYUP : 0, 0);
        } else if (g_gameWnd && IsWindow(g_gameWnd)) {           /* background: Enter as window messages + key state */
            g_bgEnter = !down;
            PostMessageA(g_gameWnd, down ? WM_KEYUP : WM_KEYDOWN, VK_RETURN, down ? 0xC01C0001u : 0x001C0001u);
        } else {                                                 /* no window known: the game's own box */
            down = 0; g_hide = 0; g_warp = 0; g_fellBack = 1;
            return g_origPresent(dev, a, b, w, r);
        }
        if (!down) InterlockedIncrement(&g_enters);
        down = !down;
        if (cfg_fast) { InterlockedIncrement(&g_dropped); return D3D_OK; }
    } else if (!g_gameWnd) { DWORD pid = 0; HWND fg = GetForegroundWindow(); if (fg) GetWindowThreadProcessId(fg, &pid); if (pid == GetCurrentProcessId()) g_gameWnd = fg; }
    if (g_hide) { IDirect3DDevice9 *d = (IDirect3DDevice9 *)dev; IDirect3DSurface9 *bb = 0, *old = 0;
        InterlockedIncrement(&g_dropped);
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(d, 0, &old)) && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC sd; D3DVIEWPORT9 vp, full;
            IDirect3DDevice9_SetRenderTarget(d, 0, bb);
            IDirect3DDevice9_GetViewport(d, &vp);
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &sd))) { full.X = 0; full.Y = 0; full.Width = sd.Width; full.Height = sd.Height; full.MinZ = 0; full.MaxZ = 1; IDirect3DDevice9_SetViewport(d, &full); }
            IDirect3DDevice9_SetRenderState(d, D3DRS_SCISSORTESTENABLE, FALSE);
            IDirect3DDevice9_Clear(d, 0, 0, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            IDirect3DDevice9_SetViewport(d, &vp);
            IDirect3DDevice9_SetRenderTarget(d, 0, old);
        }
        if (bb) IDirect3DSurface9_Release(bb);
        if (old) IDirect3DSurface9_Release(old);
    }
    return g_origPresent(dev, a, b, w, r);
}
static void hook_present(void)
{
    typedef IDirect3D9 *(WINAPI *Create9_t)(UINT);
    HMODULE d = LoadLibraryA("d3d9.dll"); Create9_t c9 = d ? (Create9_t)GetProcAddress(d, "Direct3DCreate9") : 0;
    IDirect3D9 *d3d; IDirect3DDevice9 *dev = 0; D3DPRESENT_PARAMETERS pp; HWND w; HRESULT hr; void *fn; int r, i;
    if (!c9 || !(d3d = c9(D3D_SDK_VERSION))) { rlog("HidePopup: no Direct3D 9 - box stays"); return; }
    w = CreateWindowExA(0, "STATIC", "rl", WS_POPUP, 0, 0, 64, 64, 0, 0, 0, 0);
    for (i = 0; i < (int)sizeof pp; i++) ((u8 *)&pp)[i] = 0;
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = w; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) { rlog("HidePopup: test device failed (%08X) - box stays", (u32)hr); IDirect3D9_Release(d3d); if (w) DestroyWindow(w); return; }
    fn = (*(void ***)dev)[17];
    IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d); if (w) DestroyWindow(w);
    r = g_slam->hook(g_slamMe, fn, (void *)MyPresent, (void **)&g_origPresent);
    rlog("HidePopup: Present %08X hooked: %s", (u32)fn, r == SLAM_OK ? "ok" : "FAILED - box stays");
    g_keybd = (KE_t)GetProcAddress(LoadLibraryA("user32.dll"), "keybd_event");
    hook_dinput();
    { void *next = 0; if (SLAM_HAS(g_slam, hook_import) && g_slam->hook_import(g_slamMe, "USER32.dll", "GetKeyboardState", (void *)MyGKS, &next) == SLAM_OK) g_origGKS = (GKS_t)next;
      rlog("HidePopup: background Enter (window messages%s)", g_origGKS ? " + key state" : ""); }
    if (cfg_warp > 1) { void *next = 0; InitializeCriticalSection(&g_qlk);
        if (SLAM_HAS(g_slam, hook_import) && g_slam->hook_import(g_slamMe, "KERNEL32.dll", "QueryPerformanceCounter", (void *)MyQPC, &next) == SLAM_OK) g_qpc = (QPC_t)next;
        rlog("HidePopup: clock x%d while loading: %s", cfg_warp, g_qpc ? "ok" : "FAILED"); }
}

/* 0.11: after the load the game shows a message box (0057ABB0: ecx owner, edx text hash, 3 args) that waits for
 * Enter - with HidePopup it was invisible and the game seemed stuck. During our boot load they are skipped. */
static void *g_origMsg; static volatile int g_inLoad; static volatile DWORD g_loadEnd;
/* 1.0.1: [RosterLoader] HideServerMessage=1 - the title page's "The 2K Sports server is temporarily unavailable..."
 * box (the 2K servers are gone) is skipped. The box text comes from the game's text table (0x660050 = table,
 * 0x65FEC0 fastcall(table, hash) = UTF-16 text); a box whose text has "2K Sports server" in it is not shown. */
static int cfg_srv = 1;
typedef void *(__cdecl *TxtTable_t)(void);
typedef const unsigned short *(__fastcall *TxtLookup_t)(void *, u32);
static int has_text(const unsigned short *t, const char *w)
{
    int i, j;
    if (!t) return 0;
    for (i = 0; t[i] && i < 512; i++) {
        for (j = 0; w[j] && t[i + j] == (unsigned short)w[j]; j++) {}
        if (!w[j]) return 1;
    }
    return 0;
}
static int server_msg(u32 hash, u32 ret)
{
    static int n; const unsigned short *t; void *tab;
    if (!cfg_srv) return 0;
    tab = ((TxtTable_t)0x00660050u)();
    t = tab ? ((TxtLookup_t)0x0065FEC0u)(tab, hash) : 0;
    if (!has_text(t, "2K Sports server")) return 0;
    if (n < 5) { n++; rlog("server message %08X (from %08X) skipped - the 2K servers are gone", hash, ret); }
    return 1;
}

/* 1.0.1b: the server check (0054B1AE..) - when the 2K servers don't answer it shows a notice (0x696580, text
 * BCAFA151) and then the "server is temporarily unavailable" box (0054B22C). With HideServerMessage both are
 * skipped at the source: 0054B1E0 jumps straight to 0054B243 (the same state the game sets after the box), and
 * the notice is skipped when its text is about the 2K server / online. Nothing is drawn for a frame. */
static int same(u32 a, const u8 *e, int n);
static int write_code(u32 a, const void *b, int n)
{
    DWORD old; int i;
    if (!VirtualProtect((void *)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < n; i++) ((u8 *)a)[i] = ((const u8 *)b)[i];
    VirtualProtect((void *)a, n, old, &old); FlushInstructionCache(GetCurrentProcess(), (void *)a, n);
    return 1;
}
static int g_noticeDone;
static void notice_check(void)
{
    static const u8 kj[] = { 0x75, 0x20, 0xE8 }, jmp[] = { 0xEB };
    const unsigned short *t; void *tab; char a[96]; int i;
    if (g_noticeDone || !cfg_srv) return;
    tab = ((TxtTable_t)0x00660050u)();
    t = tab ? ((TxtLookup_t)0x0065FEC0u)(tab, 0xBCAFA151u) : 0;
    if (!t || !t[0]) return;                       /* texts not loaded yet: next frame */
    g_noticeDone = 1;
    for (i = 0; t[i] && i < 95; i++) a[i] = t[i] < 128 ? (char)t[i] : '?';
    a[i] = 0;
    if ((has_text(t, "2K") || has_text(t, "erver") || has_text(t, "nline") || has_text(t, "onnect")) && same(0x54B1B5, kj, 3) && write_code(0x54B1B5, jmp, 1))
        rlog("HideServerMessage: server notice \"%s\" skipped", a);
    else rlog("HideServerMessage: notice \"%s\" kept", a);
}

/* 1.0.1c: every game box is built by 0x579AF0 (19 args, ret 4Ch; the box text key is one of them - its place
 * depends on the box type). A box whose text has "2K Sports server" in it is not built at all (no flash). */
static void *g_origCore;
static int rd_ok(u32 p, u32 n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (p < 0x10000u || !VirtualQuery((void *)p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    return (u32)mi.BaseAddress + mi.RegionSize - p >= n;
}
static int key_is_server(void *tab, u32 k)
{
    const unsigned short *t;
    if (k < 0x10000u) return 0;
    t = ((TxtLookup_t)0x0065FEC0u)(tab, k);
    return t && rd_ok((u32)t, 64) && has_text(t, "2K Sports server");
}
/* text of a box: a text key in an argument, a UTF-16 text an argument points to, or a key / text pointer inside
 * the first 0x60 bytes of a block an argument points to */
static int core_is_server(void *tab, u32 *a, int *where)
{
    int i, j;
    for (i = 1; i <= 19; i++) {
        u32 v = a[i];
        if (key_is_server(tab, v)) { *where = i; return 1; }
        if (rd_ok(v, 0x60)) {
            if (has_text((const unsigned short *)v, "2K Sports server")) { *where = 100 + i; return 1; }
            for (j = 0; j < 0x18; j++) { u32 w = ((u32 *)v)[j];
                if (key_is_server(tab, w)) { *where = 200 + i; return 1; }
                if (rd_ok(w, 64) && has_text((const unsigned short *)w, "2K Sports server")) { *where = 300 + i; return 1; } }
        }
    }
    return 0;
}
static int __stdcall core_skip(u32 *a)
{
    static int n, nl; int w = 0; void *tab;
    if (!cfg_srv) return 0;
    tab = ((TxtTable_t)0x00660050u)();
    if (!tab) return 0;
    if (core_is_server(tab, a, &w)) {
        if (n < 5) { n++; rlog("server box (from %08X, text at %d) not built - the 2K servers are gone", a[0], w); }
        return 1;
    }
    if (nl < 30) { nl++; rlog("   box from %08X  args %08X %08X %08X %08X %08X %08X", a[0], a[1], a[2], a[3], a[4], a[5], a[6]); }
    return 0;
}
__declspec(naked) static void FnCore(void)
{
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        call core_skip
        test eax, eax
        popad
        jnz skip
        jmp dword ptr [g_origCore]
    skip:
        xor eax, eax
        ret 4Ch
    }
}
static int __stdcall msg_skip(u32 hash, u32 ret)
{
    if (server_msg(hash, ret)) return 1;
    if (g_inLoad) { rlog("   message %08X (from %08X) skipped", hash, ret); return 1; }
    if (g_loadEnd && GetTickCount() - g_loadEnd < 10000) rlog("   message %08X (from %08X) after the load - shown", hash, ret);
    return 0;
}
__declspec(naked) static void FnMsg(void)
{
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 8]
        push edx
        call msg_skip
        pop edx
        pop ecx
        test eax, eax
        jnz skip
        jmp dword ptr [g_origMsg]
    skip:
        xor eax, eax
        ret 0Ch
    }
}

static void do_load(const char *why)
{
    DWORD t = GetTickCount(); int rc;
    rlog("loading \"%s\" (%s) ...", g_name, why);
    g_dropped = 0; g_enters = 0; g_diEnters = 0; g_fellBack = 0; g_loadT0 = GetTickCount(); g_hide = g_origPresent != 0; g_warp = g_hide && g_qpc && cfg_warp > 1; g_inLoad = g_origMsg != 0;
    rc = ((Load_t)0x004E9300u)(0, 0, 0, g_wname);
    g_hide = 0; g_warp = 0; g_inLoad = 0; g_loadEnd = GetTickCount();
    if (g_origPresent) { if (g_keybd) g_keybd(VK_RETURN, 0x1C, KEYEVENTF_KEYUP, 0);
        rlog("   %d frame(s) %s while loading, Enter pressed %d time(s)%s", g_dropped, cfg_fast ? "not shown" : "shown blank", g_enters,
             g_fellBack ? " - not answered: the game's own box was shown" : ""); }
    rlog("   %s (game result %d, %u ms)", rc == 0 ? "loaded" : rc == 5 ? "NOT loaded: file not found in the Saves folder" : "NOT loaded",
         rc, GetTickCount() - t);
}


/* ---- Default teams (Play Now / quick game): the game's own default is away Spurs (team ID 25) @ home Heat (10).
 * Team IDs are the number in the team's logo / uniform files (logo014.iff = Knicks 14). Two parts:
 *  1. the code that puts the default in a profile (8 places, "mov ecx,10 / call 5FE2A0" ... "mov ecx,25") gets the ini IDs;
 *  2. the profile already in memory gets them once the teams are loaded (profile +14 home / +18 away = team records). */
static int cfg_home = 14, cfg_away = 25, g_teamsSet;
static const u32 kHomeSites[] = { 0x4A7750, 0x6035F0, 0x6038FF, 0x5E8527, 0xAC7B36, 0x4B9012, 0x5BD367, 0xB291EA };
static const u32 kAwaySites[] = { 0x4A7761, 0x603601, 0x603910, 0x5E8538, 0xAC7B40, 0x4B902B, 0x5BD371, 0xB29200 };
static void teams_patch(void)
{
    int i, nh = 0, na = 0;
    for (i = 0; i < 8; i++) {
        static const u8 h[] = { 0xB9, 0x0A, 0, 0, 0 }, a[] = { 0xB9, 0x19, 0, 0, 0 };
        u8 b[5] = { 0xB9, 0, 0, 0, 0 };
        if (cfg_home != 10 && same(kHomeSites[i], h, 5)) { *(u32 *)(b + 1) = (u32)cfg_home; nh += write_code(kHomeSites[i], b, 5); }
        if (cfg_away != 25 && same(kAwaySites[i], a, 5)) { *(u32 *)(b + 1) = (u32)cfg_away; na += write_code(kAwaySites[i], b, 5); }
    }
    {   static const u8 lh[] = { 0x8D, 0x4E, 0x0A }, la[] = { 0x8D, 0x4E, 0x19 };   /* 603A64 / 603A73: lea ecx,[esi+10] / [esi+25], esi = 0 */
        u8 b[3] = { 0x8D, 0x4E, 0 };
        if (cfg_home != 10 && cfg_home < 128 && same(0x603A64, lh, 3)) { b[2] = (u8)cfg_home; nh += write_code(0x603A64, b, 3); }
        if (cfg_away != 25 && cfg_away < 128 && same(0x603A73, la, 3)) { b[2] = (u8)cfg_away; na += write_code(0x603A73, b, 3); }
    }
    rlog("DefaultTeams: away %d @ home %d - %d/9 home and %d/9 away default(s) changed in the game code", cfg_away, cfg_home, nh, na);
}
static int rd32(u32 a, u32 *v) { MEMORY_BASIC_INFORMATION m;
    if (a < 0x10000 || !VirtualQuery((void *)a, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    if ((u32)m.BaseAddress + m.RegionSize < a + 4) return 0; *v = *(u32 *)a; return 1; }
static u32 team_rec(int id)             /* same search as the game's 0x5FE2A0 */
{
    u32 g, t, n, base, i, v;
    if (!rd32(0x1F207A8, &g) || !g || !rd32(g + 0x350CB4, &v) || !v || !rd32(g + 0x350CB0, &t) || !t) return 0;
    if (!rd32(t + 0x6C, &n) || !rd32(t + 0x70, &base) || !n || n > 400) return 0;
    for (i = 0; i < n; i++) if (rd32(base + i * 0x2F8 + 0x74, &v) && (v & 0xFFFF) == (u32)id) return base + i * 0x2F8;
    return 0;
}
static void teams_profile(void)         /* game thread (frontend tick) */
{
    u32 g, prof, h, a, oh = 0, oa = 0;
    if (g_teamsSet || (cfg_home == 10 && cfg_away == 25)) return;
    if (!rd32(0x1F207A8, &g) || !g || !rd32(g + 0x350CAC, &prof) || !prof) return;
    prof += 0x28;                                                     /* profile 0 (0x00A09FD0) */
    h = team_rec(cfg_home); a = team_rec(cfg_away);
    if (!h || !a || !rd32(prof + 0x14, &oh) || !rd32(prof + 0x18, &oa)) return;
    *(u32 *)(prof + 0x14) = h; *(u32 *)(prof + 0x18) = a; g_teamsSet = 1;
    rlog("DefaultTeams: Play Now starts with away %d @ home %d", cfg_away, cfg_home);
}

static void __stdcall ui_tick(void)
{
    static volatile int busy;
    DWORD t = GetTickCount();
    if (!g_t0) g_t0 = t ? t : 1;
    if (!g_noticeDone && t - g_t0 >= 1500) notice_check();   /* menus up, texts loaded: safe to look the text up */
    if (!g_teamsSet && !busy && (g_done || !g_name[0]) && t - g_t0 >= 2000) teams_profile();   /* after the roster is in */
    if (g_done || busy) return;                    /* once per boot; the load runs the frontend again inside (re-entry) */
    if (g_name[0] && t - g_t0 >= (DWORD)cfg_delay) { busy = 1; g_done = 1; do_load("boot"); busy = 0; }
}
__declspec(naked) static void FnTick(void)
{
    __asm {
        pushad
        call ui_tick
        popad
        jmp dword ptr [g_origTick]
    }
}

static const u8 kTick[] = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0xE9, 0xE8 };
static const u8 kLoad[] = { 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x33, 0xDB };
static int same(u32 a, const u8 *e, int n) { int i; for (i = 0; i < n; i++) if (((const u8 *)a)[i] != e[i]) return 0; return 1; }

static void SLAM_CALL game_ready(void *u)
{
    u8 *b = (u8 *)GetModuleHandleA(0); IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    int r;
    (void)u;
    if (nt->FileHeader.TimeDateStamp != 0x52202C7Fu) { rlog("not the known NBA 2K14 exe (%08X) - off", nt->FileHeader.TimeDateStamp); return; }
    if (!same(0x4E0190, kTick, 7) || !same(0x4E9300, kLoad, 7)) { rlog("unexpected game code - off"); return; }
    r = g_slam->hook(g_slamMe, (void *)0x4E0190, (void *)FnTick, &g_origTick);
    rlog("frontend hook: %s", r == SLAM_OK ? "ok" : "FAILED - off");
    if (r == SLAM_OK && cfg_hide) hook_present();
    if (cfg_home != 10 || cfg_away != 25) teams_patch();
    if (cfg_hide || cfg_srv) {
        static const u8 km[] = { 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x56, 0x57, 0x8B, 0xF9 };
        static const u8 kt[] = { 0x83, 0xEC, 0x0C, 0x8B, 0x41, 0x10 };   /* text lookup start (another mod may own it: then it is skipped) */
        if (cfg_srv && !same(0x65FEC0, kt, 6) && *(u8 *)0x65FEC0 != 0xE9) { cfg_srv = 0; rlog("HideServerMessage: game text lookup not as expected - off"); }
        {   static const u8 kb[] = { 0x39, 0x1D, 0xF8, 0x77, 0x0C, 0x01 }, jb[] = { 0xEB, 0x61 };   /* 54B1E0: jmp 54B243 */
            if (cfg_srv) rlog("HideServerMessage: server box branch %s", same(0x54B1E0, kb, 6) && write_code(0x54B1E0, jb, 2) ? "skipped" : "not as expected - only the box text check");
        }
        {   static const u8 kc[] = { 0x81, 0xEC, 0x8C, 0x01, 0x00, 0x00, 0x53 };
            if (cfg_srv) rlog("HideServerMessage: box builder hook %s", same(0x579AF0, kc, 7) && g_slam->hook(g_slamMe, (void *)0x579AF0, (void *)FnCore, &g_origCore) == SLAM_OK ? "ok" : "FAILED");
        }
        if (same(0x57ABB0, km, 10) && g_slam->hook(g_slamMe, (void *)0x57ABB0, (void *)FnMsg, &g_origMsg) == SLAM_OK)
            rlog("message box hook ok (HidePopup %d, HideServerMessage %d)", cfg_hide, cfg_srv);
        else { g_origMsg = 0; rlog("message box hook FAILED"); }
    }
}


#ifdef RL_RESEARCH
/* RESEARCH: where the Play Now default teams live. F9 = dump the exe's writable memory, F10 = find "away 0x17 Spurs /
 * home 0x09 Heat" pairs everywhere, F11 = keep the ones whose home value is now 0x0C (Knicks), F12 = log what is left. */
static u32 *g_cand; static u32 g_candN; static int g_dumpN;
static int rs_rw(MEMORY_BASIC_INFORMATION *m) { return m->State == MEM_COMMIT && !(m->Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
    (m->Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)); }
static void rs_dump(void)
{
    char path[MAX_PATH]; HANDLE h; MEMORY_BASIC_INFORMATION m; u32 a = 0x00400000, end = 0x00400000 + 0x01D7C000, n = 0; DWORD w;
    lstrcpynA(path, g_slam->mod_dir(g_slamMe), MAX_PATH); wsprintfA(path + lstrlenA(path), "rs_dump%d.bin", ++g_dumpN);
    h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); if (h == INVALID_HANDLE_VALUE) return;
    while (a < end && VirtualQuery((void *)a, &m, sizeof m)) {
        u32 lo = (u32)m.BaseAddress, hi = lo + m.RegionSize; if (hi > end) hi = end;
        if (rs_rw(&m)) { WriteFile(h, &lo, 4, &w, 0); { u32 len = hi - lo; WriteFile(h, &len, 4, &w, 0); } WriteFile(h, (void *)lo, hi - lo, &w, 0); n += hi - lo; }
        a = hi;
    }
    CloseHandle(h); rlog("RESEARCH dump %d: %u bytes of the exe's writable memory -> rs_dump%d.bin", g_dumpN, n, g_dumpN);
}
/* every read goes through ReadProcessMemory: memory the game frees while we scan can't crash us (crash report 04:05) */
static u8 g_rsBuf[0x10000 + 64];
static int rs_read(u32 a, void *out, u32 n) { SIZE_T got = 0; return ReadProcessMemory(GetCurrentProcess(), (void *)a, out, n, &got) && got == n; }
/* team table: T = [[0x1F207A8]+0x350CB0] (0x00A09D00), count T+6C, records T+70, 0x2F8 apart, team ID (logo number) at +74 */
static u32 g_rsTeam[64];
static u32 rs_team(u32 id, int *idx)
{
    u32 g = 0, on = 0, t = 0, n = 0, base = 0, i; u16 v;
    if (!rs_read(0x1F207A8, &g, 4) || !g || !rs_read(g + 0x350CB4, &on, 4) || !on || !rs_read(g + 0x350CB0, &t, 4) || !t) return 0;
    if (!rs_read(t + 0x6C, &n, 4) || !rs_read(t + 0x70, &base, 4) || n > 400) return 0;
    for (i = 0; i < n; i++) if (rs_read(base + i * 0x2F8 + 0x74, &v, 2) && v == id) { if (idx) *idx = (int)i; return base + i * 0x2F8; }
    return 0;
}
static void rs_scan(void)
{
    MEMORY_BASIC_INFORMATION m; u32 a = 0x10000, nh = 0, na = 0, rh, ra; int ih = -1, ia = -1, ik = -1, i5 = -1;
    if (!g_cand) g_cand = (u32 *)VirtualAlloc(0, 4 * 1048576, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_cand) return;
    g_candN = 0;
    rh = rs_team(10, &ih); ra = rs_team(25, &ia); g_rsTeam[14] = rs_team(14, &ik); g_rsTeam[15] = rs_team(15, &i5);
    rlog("RESEARCH teams: Heat %08X (#%d)  Spurs %08X (#%d)  Knicks %08X (#%d)  id15 %08X (#%d)", rh, ih, ra, ia, g_rsTeam[14], ik, g_rsTeam[15], i5);
    if (!rh || !ra) { rlog("RESEARCH: team table not found"); return; }
    while (a < 0x7FFF0000 && VirtualQuery((void *)a, &m, sizeof m)) {
        u32 lo = (u32)m.BaseAddress, hi = lo + m.RegionSize, c;
        if (rs_rw(&m) && m.Type != MEM_MAPPED) {
            for (c = lo; c < hi && g_candN < 1048000; c += 0x10000) {
                u32 n = hi - c < 0x10000 ? hi - c : 0x10000, i;
                if (!rs_read(c, g_rsBuf, n)) continue;
                for (i = 0; i + 4 <= n && g_candN < 1048000; i += 4) {
                    u32 v = *(const u32 *)(g_rsBuf + i);
                    if (v == rh) { g_cand[g_candN++] = c + i; nh++; }
                    else if (v == ra) na++;
                }
            }
        }
        a = hi; if (!hi) break;
    }
    rlog("RESEARCH F10: %u pointers to the Heat record (and %u to the Spurs record) - now change HOME to Knicks and press F11", nh, na);
}
static void rs_refine(u32 home, const char *what)
{
    u32 i, k = 0, want = g_rsTeam[home];
    if (!want) { rlog("RESEARCH %s: team %u not found", what, home); return; }
    for (i = 0; i < g_candN; i++) { u32 x = g_cand[i], v = 0; if (rs_read(x, &v, 4) && v == want) g_cand[k++] = x; }
    g_candN = k; rlog("RESEARCH %s: %u left", what, k);
    for (i = 0; i < k && i < 60; i++) { u32 x = g_cand[i]; u8 b[64]; int j; char line[300], *p = line;
        if (!rs_read(x - 32, b, 64)) continue;
        p += wsprintfA(p, "  %08X:", x); for (j = 0; j < 64; j++) p += wsprintfA(p, " %02X", b[j]); rlog("%s", line); }
}
static DWORD WINAPI rs_keys(LPVOID u)
{
    int prev[4] = { 0 }; (void)u;
    rlog("RESEARCH keys: F9 dump, F10 scan (Play Now, Spurs @ Heat shown), F11 after HOME -> Knicks, F12 after HOME one team further");
    for (;;) {
        int k[4], i; static const int vk[4] = { 0x78, 0x79, 0x7A, 0x7B };
        for (i = 0; i < 4; i++) k[i] = (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
        if (k[0] && !prev[0]) rs_dump();
        if (k[1] && !prev[1]) rs_scan();
        if (k[2] && !prev[2]) rs_refine(14, "F11 (home = Knicks, id 14)");
        if (k[3] && !prev[3]) rs_refine(15, "F12 (home one team further, id 15)");
        for (i = 0; i < 4; i++) prev[i] = k[i];
        Sleep(30);
    }
}
#endif

/* ---------------- 1.2: remember the roster the game loads ----------------
 * The game's own CreateFileA / ReadFile (import hooks, chained by SLAM with any other mod that follows them).
 * A roster counts as loaded when 256 KB of one .ROS were read: the Load Roster list only reads the headers. */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPVOID, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
typedef BOOL (WINAPI *CloseHandle_t)(HANDLE);
static CreateFileA_t o_CreateFileA; static ReadFile_t o_ReadFile; static CloseHandle_t o_CloseHandle;
typedef struct { HANDLE h; u32 bytes; int done; char key[MAX_PATH]; } Track;
static Track g_tr[8];
static CRITICAL_SECTION g_tlk;
static char g_ini[MAX_PATH], g_lastSaved[MAX_PATH];
#define LOAD_BYTES (256u * 1024u)

static int ros_key(const char *path, char *key)      /* "...\Name.ROS" -> "Name.ROS"; 0 if not a .ROS */
{
    int n = lstrlenA(path), i, b = 0;
    if (n < 5 || path[n - 4] != '.') return 0;
    if ((path[n - 3] | 32) != 'r' || (path[n - 2] | 32) != 'o' || (path[n - 1] | 32) != 's') return 0;
    for (i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') b = i + 1;
    lstrcpynA(key, path + b, MAX_PATH);
    return 1;
}

static void on_roster_loaded(const char *key)
{
    if (!lstrcmpiA(key, g_lastSaved)) return;
    lstrcpynA(g_lastSaved, key, MAX_PATH);
    WritePrivateProfileStringA("RosterLoader", "LastLoaded", key, g_ini);
    rlog("roster loaded in the game: \"%s\" (saved as LastLoaded)", key);
}

static HANDLE WINAPI hk_CreateFileA(LPCSTR n, DWORD acc, DWORD sh, LPVOID sa, DWORD disp, DWORD fl, HANDLE t)
{
    HANDLE h = o_CreateFileA(n, acc, sh, sa, disp, fl, t);
    char key[MAX_PATH]; int i;
    if (h != INVALID_HANDLE_VALUE && n && !(acc & GENERIC_WRITE) && ros_key(n, key)) {
        EnterCriticalSection(&g_tlk);
        for (i = 0; i < 8; i++) if (!g_tr[i].h) { g_tr[i].h = h; g_tr[i].bytes = 0; g_tr[i].done = 0; lstrcpynA(g_tr[i].key, key, MAX_PATH); break; }
        LeaveCriticalSection(&g_tlk);
    }
    return h;
}

static BOOL WINAPI hk_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD rd, LPVOID ov)
{
    BOOL r = o_ReadFile(h, b, n, rd, ov); int i, fire = 0; char key[MAX_PATH];
    if (r && h) {
        EnterCriticalSection(&g_tlk);
        for (i = 0; i < 8; i++) if (g_tr[i].h == h) {
            g_tr[i].bytes += rd ? *rd : n;
            if (!g_tr[i].done && g_tr[i].bytes >= LOAD_BYTES) { g_tr[i].done = 1; lstrcpynA(key, g_tr[i].key, MAX_PATH); fire = 1; }
            break;
        }
        LeaveCriticalSection(&g_tlk);
        if (fire) on_roster_loaded(key);
    }
    return r;
}

static BOOL WINAPI hk_CloseHandle(HANDLE h)
{
    int i;
    if (h) { EnterCriticalSection(&g_tlk); for (i = 0; i < 8; i++) if (g_tr[i].h == h) { g_tr[i].h = 0; break; } LeaveCriticalSection(&g_tlk); }
    return o_CloseHandle(h);
}

static int file_hook(const char *fn, void *hook, void **orig)
{
    int r;
    if (!SLAM_HAS(g_slam, hook_import)) return 0;
    r = g_slam->hook_import(g_slamMe, "KERNEL32.dll", fn, hook, orig);
    if (r == SLAM_ERR_CONFLICT) rlog("WARNING another mod blocks the hook of %s", fn);
    return r == SLAM_OK;
}

static void install_follow(void)
{
    int a, b;
    InitializeCriticalSection(&g_tlk);
    a = file_hook("CreateFileA", (void *)hk_CreateFileA, (void **)&o_CreateFileA);
    b = file_hook("ReadFile", (void *)hk_ReadFile, (void **)&o_ReadFile);
    file_hook("CloseHandle", (void *)hk_CloseHandle, (void **)&o_CloseHandle);
    if (!o_CloseHandle) o_CloseHandle = (CloseHandle_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CloseHandle");
    if (!o_ReadFile) o_ReadFile = (ReadFile_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ReadFile");
    if (a && b) rlog("following the rosters you load in the game (for LastLoaded)");
    else rlog("WARNING could not follow roster loads (CreateFileA %d, ReadFile %d) - LastLoaded is not updated", a, b);
}

static int roster_exists(const char *name)
{
    char p[MAX_PATH * 2]; DWORD n = GetEnvironmentVariableA("APPDATA", p, MAX_PATH);
    if (!n || n >= MAX_PATH) return 1;               /* cannot tell: let the game try */
    lstrcatA(p, "\\2K Sports\\NBA 2K14\\Saves\\"); lstrcatA(p, name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

int mod_start(void)
{
    const char *ini;
    int i;
    InitializeCriticalSection(&g_lk);
    lstrcpynA(g_log, g_slam->mod_dir(g_slamMe), MAX_PATH); lstrcatA(g_log, "RosterLoader.log"); DeleteFileA(g_log);
    rlog("RosterLoader " MOD_VERSION);
    ini = g_slam->mod_ini(g_slamMe);
    lstrcpynA(g_ini, ini, MAX_PATH);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("RosterLoader", "AutoLoad", "", ini);
        WritePrivateProfileStringA("RosterLoader", "AutoLoadLast", "1", ini);
        WritePrivateProfileStringA("RosterLoader", "Delay", "0", ini);
        WritePrivateProfileStringA("RosterLoader", "HidePopup", "1", ini);
        WritePrivateProfileStringA("RosterLoader", "HideServerMessage", "1", ini);
        WritePrivateProfileStringA("RosterLoader", "DefaultHome", "14", ini);
        WritePrivateProfileStringA("RosterLoader", "DefaultAway", "25", ini);
        rlog("RosterLoader.ini made - put your roster file name after AutoLoad=");
    }
    GetPrivateProfileStringA("RosterLoader", "AutoLoad", "", g_name, MAX_PATH, ini);
    if (GetPrivateProfileIntA("RosterLoader", "AutoLoadLast", -1, ini) == (UINT)-1)        /* older ini: add the key */
        WritePrivateProfileStringA("RosterLoader", "AutoLoadLast", "1", ini);
    cfg_last = (int)GetPrivateProfileIntA("RosterLoader", "AutoLoadLast", 1, ini);
    GetPrivateProfileStringA("RosterLoader", "LastLoaded", "", g_lastSaved, MAX_PATH, ini);
    if (!g_name[0] && cfg_last && g_lastSaved[0]) {
        if (roster_exists(g_lastSaved)) { lstrcpynA(g_name, g_lastSaved, MAX_PATH); rlog("AutoLoad is empty: loading the last roster you loaded (\"%s\")", g_name); }
        else rlog("LastLoaded \"%s\" is not in the Saves folder any more - nothing to load", g_lastSaved);
    }
    cfg_delay = (int)GetPrivateProfileIntA("RosterLoader", "Delay", 0, ini);
    cfg_hide = (int)GetPrivateProfileIntA("RosterLoader", "HidePopup", 1, ini);
    cfg_fast = (int)GetPrivateProfileIntA("RosterLoader", "Fast", 1, ini);
    cfg_warp = (int)GetPrivateProfileIntA("RosterLoader", "Warp", 8, ini);
    cfg_srv = (int)GetPrivateProfileIntA("RosterLoader", "HideServerMessage", 1, ini);
    if (GetPrivateProfileIntA("RosterLoader", "DefaultHome", -1, ini) == (UINT)-1) {   /* older ini: add the keys */
        WritePrivateProfileStringA("RosterLoader", "DefaultHome", "14", ini);
        WritePrivateProfileStringA("RosterLoader", "DefaultAway", "25", ini);
    }
    cfg_home = (int)GetPrivateProfileIntA("RosterLoader", "DefaultHome", 14, ini);
    cfg_away = (int)GetPrivateProfileIntA("RosterLoader", "DefaultAway", 25, ini);
    if (cfg_home <= 0 || cfg_home > 1000) cfg_home = 10;
    if (cfg_away <= 0 || cfg_away > 1000) cfg_away = 25;
    if (cfg_delay < 0) cfg_delay = 0;
    for (i = 0; g_name[i] && i < MAX_PATH - 1; i++) g_wname[i] = (u8)g_name[i];
    g_wname[i] = 0;
    install_follow();
    if (!g_name[0]) rlog(cfg_last ? "nothing to load yet - load a roster in the game once and it loads at every start"
                                  : "AutoLoad is empty and AutoLoadLast=0 - nothing to load");
    else rlog("roster: \"%s\"  delay %d ms", g_name, cfg_delay);
#ifdef RL_RESEARCH
    CreateThread(0, 0, rs_keys, 0, 0, 0);
#endif
    if (!g_name[0] && !cfg_srv && cfg_home == 10 && cfg_away == 25) return 0;
    if (!SLAM_HAS(g_slam, on_game_ready) || g_slam->on_game_ready(g_slamMe, game_ready, 0) != SLAM_OK) game_ready(0);
    return 0;
}
#ifdef MOD_HAS_STOP
void mod_stop(void) { }
#endif
