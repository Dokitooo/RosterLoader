/* slam_lite.c - a SLAM mod without SLAM: a tiny stand-in for the SLAM API so the same mod code runs as a plain .asi
 * under any ASI loader (dinput8.dll / Ultimate ASI Loader).
 *   - folder: <folder of the .asi>\<Mod>\
 *   - patch: VirtualProtect + write;  hook_import: the game's import table entry
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "slam.h"

const SLAM_API *g_slam; SlamMod *g_slamMe; void *g_slamInst;
int mod_start(void);
void glue_migrate(const char *file) { (void)file; }
#ifdef MOD_HAS_STOP
void mod_stop(void);
#endif
static char g_gdir[MAX_PATH], g_mdir[MAX_PATH], g_mini[MAX_PATH], g_logp[MAX_PATH];
static CRITICAL_SECTION g_lk;
static void mcpy_(void *d, const void *s, unsigned n){ BYTE *a=(BYTE*)d; const BYTE *b=(const BYTE*)s; while(n--) *a++=*b++; }
#define memcpy mcpy_

static const char *SLAM_CALL l_game_dir(void) { return g_gdir; }
static const char *SLAM_CALL l_mod_dir(SlamMod *m) { (void)m; return g_mdir; }
static const char *SLAM_CALL l_mod_ini(SlamMod *m) { (void)m; return g_mini; }
static void SLAM_CALL l_log(SlamMod *m, const char *fmt, ...)
{
    char b[1100]; int n; va_list ap; HANDLE f; DWORD w; SYSTEMTIME t; (void)m;
    GetLocalTime(&t); n = wsprintfA(b, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt); n += wvsprintfA(b + n, fmt, ap); va_end(ap); b[n++] = '\r'; b[n++] = '\n';
    EnterCriticalSection(&g_lk);
    f = CreateFileA(g_logp, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, n, &w, 0); CloseHandle(f); }
    LeaveCriticalSection(&g_lk);
}
static int wr(void *a, const void *b, unsigned n)
{
    DWORD o; if (!VirtualProtect(a, n, PAGE_EXECUTE_READWRITE, &o)) return 0;
    memcpy(a, b, n); VirtualProtect(a, n, o, &o); FlushInstructionCache(GetCurrentProcess(), a, n); return 1;
}
/* ---- patches: written directly (the mod never takes them back) */
static int SLAM_CALL l_patch(SlamMod *m, void *addr, const void *bytes, unsigned len)
{ (void)m; return wr(addr, bytes, len) ? SLAM_OK : SLAM_ERR_HOOK; }
static int SLAM_CALL l_unpatch(SlamMod *m, void *addr) { (void)m; (void)addr; return SLAM_ERR_NOT_FOUND; }
static int SLAM_CALL l_read(const void *addr, void *out, unsigned len)
{
    SIZE_T n = 0; return ReadProcessMemory(GetCurrentProcess(), addr, out, len, &n) && n == len;
}
/* ---- detours: instruction lengths for the prologue forms used in nba2k14.exe here */
static int modrm_len(const BYTE *p)       /* p -> modrm; returns modrm + sib + disp bytes */
{
    BYTE mo = p[0] >> 6, rm = p[0] & 7; int n = 1;
    if (mo != 3 && rm == 4) { n++; if (mo == 0 && (p[1] & 7) == 5) n += 4; }
    if (mo == 0 && rm == 5) n += 4; else if (mo == 1) n += 1; else if (mo == 2) n += 4;
    return n;
}
static int ins_len(const BYTE *p)
{
    BYTE o = p[0];
    if (o >= 0x50 && o <= 0x5F) return 1;                                   /* push / pop r32 */
    if (o == 0x6A) return 2; if (o == 0x68) return 5;
    if (o == 0x8B || o == 0x89 || o == 0x8D || o == 0x33 || o == 0x31 || o == 0x2B || o == 0x03 || o == 0x85 || o == 0x3B) return 1 + modrm_len(p + 1);
    if (o == 0x83) return 1 + modrm_len(p + 1) + 1;
    if (o == 0x81) return 1 + modrm_len(p + 1) + 4;
    if (o == 0xFF && ((p[1] >> 3) & 7) != 2) return 1 + modrm_len(p + 1);   /* jmp/push [mem] (not call) */
    if (o == 0xF3 && p[1] == 0x0F && (p[2] == 0x10 || p[2] == 0x11)) return 3 + modrm_len(p + 3);
    if (o == 0x0F && (p[1] == 0xB6 || p[1] == 0xB7 || p[1] == 0x57 || p[1] == 0x28)) return 2 + modrm_len(p + 2);
    if (o == 0xB8 || (o >= 0xB9 && o <= 0xBF)) return 5;
    return 0;                                                               /* unknown: refuse */
}
static int SLAM_CALL l_hook(SlamMod *m, void *target, void *detour, void **original)
{
    BYTE *t = (BYTE *)target, *tr, j[16]; int n = 0, k; (void)m;
    tr = (BYTE *)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!tr) return SLAM_ERR_HOOK;
    if (t[0] == 0xE9) {                                                     /* someone hooked it already: chain */
        DWORD dst = (DWORD)(t + 5) + *(DWORD *)(t + 1);
        tr[0] = 0xE9; *(DWORD *)(tr + 1) = dst - (DWORD)(tr + 5); n = 5;
    } else {
        while (n < 5) { k = ins_len(t + n); if (!k) { VirtualFree(tr, 0, MEM_RELEASE); return SLAM_ERR_HOOK; } n += k; }
        memcpy(tr, t, n); tr[n] = 0xE9; *(DWORD *)(tr + n + 1) = (DWORD)(t + n) - (DWORD)(tr + n + 5);
    }
    *original = tr;
    j[0] = 0xE9; *(DWORD *)(j + 1) = (DWORD)detour - (DWORD)(t + 5); for (k = 5; k < n && k < 16; k++) j[k] = 0x90;
    return wr(t, j, n < 5 ? 5 : n) ? SLAM_OK : SLAM_ERR_HOOK;
}
static int SLAM_CALL l_unhook(SlamMod *m, void *t, void *d) { (void)m; (void)t; (void)d; return SLAM_ERR_HOOK; }
static int SLAM_CALL l_hook_import(SlamMod *m, const char *dll, const char *fn, void *detour, void **original)
{
    BYTE *b = (BYTE *)GetModuleHandleA(0); IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); (void)m;
    if (!dd->VirtualAddress) return SLAM_ERR_NOT_FOUND;
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA *on, *ft;
        if (lstrcmpiA((const char *)(b + d->Name), dll)) continue;
        on = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)); ft = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            IMAGE_IMPORT_BY_NAME *nm;
            if (on->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            nm = (IMAGE_IMPORT_BY_NAME *)(b + on->u1.AddressOfData);
            if (!lstrcmpA((const char *)nm->Name, fn)) {
                DWORD v = (DWORD)detour; *original = (void *)ft->u1.Function;
                return wr(&ft->u1.Function, &v, 4) ? SLAM_OK : SLAM_ERR_HOOK;
            }
        }
    }
    return SLAM_ERR_NOT_FOUND;
}
static int SLAM_CALL l_unhook_import(SlamMod *m, const char *a, const char *b, void *c) { (void)m; (void)a; (void)b; (void)c; return SLAM_ERR_HOOK; }
/* ---- game ready: the call at 008572DF is GotoShotExt's own sign that the code is unpacked */
static int code_ready(void) { const BYTE *s = (const BYTE *)0x008572DF; return s[0] == 0xE8 && (DWORD)(s + 5) + *(const DWORD *)(s + 1) == 0x00856E60u; }
typedef struct { SlamReadyCb cb; void *u; } RD;
static DWORD WINAPI ready_thread(LPVOID p)
{
    RD r = *(RD *)p; DWORD t0 = GetTickCount(); HeapFree(GetProcessHeap(), 0, p);
    while (!code_ready()) { if (GetTickCount() - t0 > 180000) { l_log(0, "ERROR game code not recognised after 180 s - nothing started"); return 0; } Sleep(20); }
    Sleep(200); r.cb(r.u); return 0;
}
static int SLAM_CALL l_on_game_ready(SlamMod *m, SlamReadyCb cb, void *u)
{
    RD *r = (RD *)HeapAlloc(GetProcessHeap(), 0, sizeof(RD)); (void)m; if (!r) return SLAM_ERR_HOOK;
    r->cb = cb; r->u = u; CreateThread(0, 0, ready_thread, r, 0, 0); return SLAM_OK;
}
static int SLAM_CALL l_find_mod(const char *name, char *ver, unsigned cap)
{
    char p[96]; if (ver && cap) ver[0] = 0;
    lstrcpynA(p, name, 80); lstrcatA(p, ".asi"); if (GetModuleHandleA(p)) return 1;
    lstrcpynA(p, name, 80); lstrcatA(p, ".dll"); return GetModuleHandleA(p) != 0;
}
static void *SLAM_CALL l_game_base(void) { return GetModuleHandleA(0); }
static unsigned SLAM_CALL l_exe_ts(void) { BYTE *b = (BYTE *)GetModuleHandleA(0); return ((IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew))->FileHeader.TimeDateStamp; }
static int SLAM_CALL l_game_is_ready(void) { return code_ready(); }
static void SLAM_CALL l_crash_note(SlamMod *m, const char *t) { (void)m; (void)t; }

static SLAM_API g_api;
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        char *s; DisableThreadLibraryCalls(h); g_slamInst = h;
        InitializeCriticalSection(&g_lk);
        GetModuleFileNameA(0, g_gdir, MAX_PATH); s = g_gdir + lstrlenA(g_gdir); while (s > g_gdir && s[-1] != '\\') s--; *s = 0;
        GetModuleFileNameA(h, g_mdir, MAX_PATH); s = g_mdir + lstrlenA(g_mdir); while (s > g_mdir && s[-1] != '\\') s--; *s = 0;
        lstrcatA(g_mdir, MOD_NAME "\\"); CreateDirectoryA(g_mdir, 0);
        lstrcpyA(g_mini, g_mdir); lstrcatA(g_mini, MOD_NAME ".ini");
        lstrcpyA(g_logp, g_mdir); lstrcatA(g_logp, MOD_NAME "_loader.log"); DeleteFileA(g_logp);
        g_api.size = sizeof(SLAM_API); g_api.apiVersion = SLAM_API_VERSION; g_api.slamVersion = "none (plain .asi)";
        g_api.game_dir = l_game_dir; g_api.mod_dir = l_mod_dir; g_api.mod_ini = l_mod_ini; g_api.log = l_log;
        g_api.hook = l_hook; g_api.unhook = l_unhook; g_api.patch = l_patch; g_api.unpatch = l_unpatch; g_api.read = l_read;
        g_api.on_game_ready = l_on_game_ready; g_api.find_mod = l_find_mod; g_api.game_base = l_game_base;
        g_api.exe_timestamp = l_exe_ts; g_api.game_is_ready = l_game_is_ready; g_api.crash_note = l_crash_note;
        g_api.hook_import = l_hook_import; g_api.unhook_import = l_unhook_import;
        g_slam = &g_api; g_slamMe = (SlamMod *)&g_api;
        l_log(0, MOD_NAME " (no SLAM) loaded, folder %s", g_mdir);
        mod_start();
    }
#ifdef MOD_HAS_STOP
    else if (why == DLL_PROCESS_DETACH && !r) mod_stop();
#endif
    return TRUE;
}
