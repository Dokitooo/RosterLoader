/* d3d9pick.h - which d3d9.dll a mod should hook.
 *
 * A d3d9.dll in the game folder (ReShade, ENB, SweetFX, an ASI loader...) is a wrapper: the game talks to it, and
 * it passes everything on to the Windows d3d9.dll. Hooking inside the wrapper patches its code (ReShade does not
 * expect that). So: when a wrapper is there, hook the Windows d3d9.dll underneath it instead - the wrapper is
 * never touched and still sees every call first, exactly as without mods.
 *   - no wrapper          -> the Windows d3d9.dll (same as before)
 *   - wrapper             -> the Windows d3d9.dll, loaded by full path AFTER the game's "d3d9.dll" name is bound
 *                            to the wrapper (loading it first would make the game skip the wrapper)
 *   - DXVK (no Windows d3d9 under it) -> DXVK itself, as before
 * Needs: LoadLibraryA, GetModuleFileNameA, GetSystemDirectoryA, GetWindowsDirectoryA.
 */
#ifndef D3D9PICK_H
#define D3D9PICK_H

static int dp_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static void dp_cpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void dp_cat(char *d, const char *s, int n) { int i = 0; while (i < n - 1 && d[i]) i++; dp_cpy(d + i, s, n - i); }
static void dp_dir(char *p) { int i, c = -1; for (i = 0; p[i]; i++) if (p[i] == '\\' || p[i] == '/') c = i; if (c >= 0) p[c] = 0; }

/* text inside a loaded module's readable sections (PE headers walked in memory) */
static int dp_has(HMODULE m, const char *t)
{
    const unsigned char *b = (const unsigned char *)m;
    const unsigned char *nt, *sec;
    int ns, i, L = 0;
    if (!m || b[0] != 'M' || b[1] != 'Z') return 0;
    while (t[L]) L++;
    nt = b + *(const int *)(b + 0x3C);
    if (nt[0] != 'P' || nt[1] != 'E') return 0;
    ns = *(const unsigned short *)(nt + 6);
    sec = nt + 24 + *(const unsigned short *)(nt + 20);
    for (i = 0; i < ns; i++, sec += 40) {
        unsigned va = *(const unsigned *)(sec + 12), vs = *(const unsigned *)(sec + 8), ch = *(const unsigned *)(sec + 36);
        const unsigned char *p = b + va;
        unsigned k;
        if (!(ch & 0x40000000u) || (ch & 0x20000000u) || vs < (unsigned)L) continue;   /* readable data, not code */
        for (k = 0; k + L <= vs; k++) {
            int j = 0;
            while (j < L && (p[k + j] | 32) == (unsigned char)(t[j] | 32)) j++;
            if (j == L) return 1;
        }
    }
    return 0;
}

/* returns the d3d9 module to hook; why (>= 300 chars) gets a line for the log */
static HMODULE d3d9_pick(char *why, int whyLen)
{
    char gp[MAX_PATH], sp[MAX_PATH], w32[MAX_PATH], wow[MAX_PATH], dir[MAX_PATH];
    HMODULE g = LoadLibraryA("d3d9.dll"), s;      /* the same one the game gets (binds the name first) */
    if (why && whyLen) why[0] = 0;
    if (!g) { if (why) dp_cpy(why, "d3d9.dll could not be loaded", whyLen); return 0; }
    gp[0] = 0; GetModuleFileNameA(g, gp, MAX_PATH);
    dp_cpy(dir, gp, MAX_PATH); dp_dir(dir);
    sp[0] = 0; GetSystemDirectoryA(sp, MAX_PATH);
    w32[0] = 0; GetWindowsDirectoryA(w32, MAX_PATH); dp_cpy(wow, w32, MAX_PATH);
    dp_cat(w32, "\\System32", MAX_PATH); dp_cat(wow, "\\SysWOW64", MAX_PATH);
    if (dp_ieq(dir, sp) || dp_ieq(dir, w32) || dp_ieq(dir, wow)) {
        if (why) dp_cpy(why, "d3d9: Windows d3d9.dll", whyLen);
        return g;
    }
    if (dp_has(g, "dxvk")) {
        if (why) { dp_cpy(why, "d3d9: DXVK in the game folder (no Windows d3d9 under it) - hooked directly: ", whyLen); dp_cat(why, gp, whyLen); }
        return g;
    }
    dp_cat(sp, "\\d3d9.dll", MAX_PATH);
    s = LoadLibraryA(sp);                         /* full path: the Windows one the wrapper passes calls to */
    if (!s || s == g) {
        if (why) { dp_cpy(why, "d3d9: wrapper found but the Windows d3d9.dll could not be loaded - hooked the wrapper: ", whyLen); dp_cat(why, gp, whyLen); }
        return g;
    }
    if (why) {
        dp_cpy(why, "d3d9: ", whyLen);
        dp_cat(why, dp_has(g, "reshade") ? "ReShade" : dp_has(g, "enbseries") ? "ENB" : "a d3d9 wrapper", whyLen);
        dp_cat(why, " in the game folder (", whyLen); dp_cat(why, gp, whyLen);
        dp_cat(why, ") - hooking the Windows d3d9 under it, the wrapper itself is not touched", whyLen);
    }
    return s;
}
#endif
