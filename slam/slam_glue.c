/* slam_glue.c - the SLAM side of a mod: who it is, DllMain, SlamModStart.
 * Compiled with <windows.h> (the SDK part of slam.h needs it); the mod's own code keeps its own
 * headers and only provides   int mod_start(void)   (0 = started).
 * Build with  -DMOD_NAME=\"CrowdTune\" -DMOD_VERSION=\"1.5.1\"  (see build_mods_linux.sh).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SLAM_SDK_IMPLEMENTATION
#include "slam.h"

#ifndef MOD_AUTHOR
#define MOD_AUTHOR "Alexiconn2K"
#endif
SLAM_MOD(MOD_NAME, MOD_VERSION, MOD_AUTHOR)

const SLAM_API *g_slam;     /* the running SLAM's functions */
SlamMod *g_slamMe;          /* this mod, for every g_slam->... call */
void *g_slamInst;           /* this .asi (HINSTANCE) */
int mod_start(void);
#ifdef MOD_HAS_STOP
void mod_stop(void);
#endif

/* The mod's settings files (<Mod>.ini, ...) live in its folder (SLAM\<Mod>\). SLAM never changes them:
   no marker line, no .backup, no .new. The only help: if a settings file is MISSING there and the player
   still has it in the old place (scripts\<Mod>\, scripts\, next to the exe, ...), it is copied over once.
   Leftovers of older SLAM editions (<file>.backup, <file>.new, the "[SLAM] InUse=1" line) are removed. */
static void mod_path(char *out, const char *file, const char *suffix)
{
    lstrcpynA(out, g_slam->mod_dir(g_slamMe), MAX_PATH); lstrcatA(out, file); if (suffix) lstrcatA(out, suffix);
}
static int has_file(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

void glue_migrate(const char *file)
{
    static const char *const old[] = { "scripts\\" MOD_NAME "\\", "scripts\\", "", "plugins\\" MOD_NAME "\\", "plugins\\",
                                       "asi\\" MOD_NAME "\\", "asi\\", 0 };
    char dst[MAX_PATH], tmp[MAX_PATH], from[MAX_PATH], b[4]; int i;
    mod_path(dst, file, 0);
    /* old SLAM editions' leftovers */
    mod_path(tmp, file, ".backup"); if (has_file(tmp) && DeleteFileA(tmp)) g_slam->log(g_slamMe, "settings: removed old %s.backup", file);
    mod_path(tmp, file, ".new");    if (has_file(tmp) && DeleteFileA(tmp)) g_slam->log(g_slamMe, "settings: removed old %s.new", file);
    if (has_file(dst)) {
        GetPrivateProfileStringA("SLAM", "InUse", "", b, sizeof(b), dst);
        if (b[0]) { WritePrivateProfileStringA("SLAM", NULL, NULL, dst); g_slam->log(g_slamMe, "settings: removed the old [SLAM] InUse line from %s", file); }
        return;                                                  /* the player's file: never touched */
    }
    for (i = 0; old[i]; i++) {
        lstrcpynA(from, g_slam->game_dir(), MAX_PATH); lstrcatA(from, old[i]); lstrcatA(from, file);
        if (lstrcmpiA(from, dst) && has_file(from)) {
            if (CopyFileA(from, dst, TRUE))
                g_slam->log(g_slamMe, "settings: %s was missing - copied yours from %s", file, old[i][0] ? old[i] : "the game folder");
            return;
        }
    }
}

SLAM_EXPORT int SLAM_CALL SlamModStart(const SLAM_API *api, SlamMod *self)
{
    g_slam = api; g_slamMe = self;
    glue_migrate(MOD_NAME ".ini");
    return mod_start();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID reserved)
{
    (void)reserved;
    if (why == DLL_PROCESS_ATTACH) {
        g_slamInst = inst;
        DisableThreadLibraryCalls(inst);
        SlamSdk_OnAttach(inst);         /* "SLAM not installed" window if SLAM is missing/too old */
    }
    else if (why == DLL_PROCESS_DETACH && g_slam) {
#ifdef MOD_HAS_STOP
        mod_stop();                     /* game closing: the mod's last log lines */
#endif
    }
    return TRUE;
}
