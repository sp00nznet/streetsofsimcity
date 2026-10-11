/*
 * Streets of SimCity - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). What is here is only what is
 * specific to this game: the guest image, where it finds its data, the
 * command line, scripted input and the fault report. The display is
 * ddraw.c. docs/architecture.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (Streets.exe) is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "native32.h"
#include "recomp_trace.h"
#include "ddraw_host.h"
#include "present.h"
#include "oracle.h"

extern const uint32_t ssc_entry_va;       /* recomp_dispatch.c */

#define SSC_BASE 0x00400000u

static DWORD g_watchdog_s;
static int   g_headless;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is Streets.exe in the game folder, not this host: its hInstance
 * (icon, strings) is GetModuleHandleA(NULL). */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)) : SSC_BASE, 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == SSC_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, g_guest_exe, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* --headless: nothing reaches the screen (REPO_RULES section 13). The game's
 * own message boxes go to stderr. */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

/* ------------------------------------------------------------ input */

/* --key NAME@MS[+HOLD] and --click X,Y@MS: scripted input, in milliseconds
 * from entry. The game reads keys through window messages (typing a name)
 * and by polling GetKeyState and GetAsyncKeyState (driving), so a scripted
 * key is posted to the focus window and also held for both; a click is
 * posted as a button down and up at a 640x480 position. Headless, the real
 * keyboard is not read at all. */
#define MAX_INPUT 64
static struct { int vk, x, y; DWORD at, hold; } g_in[MAX_INPUT];
static int g_nin;
static volatile LONG g_held[256];
static int vk_of(const char* s) {
    static const struct { const char* n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}, {"LBUTTON", VK_LBUTTON}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);
    return (int)strtol(s, NULL, 0);
}
static int parse_key(const char* arg) {
    char name[16];
    unsigned at = 0, hold = 100;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_in[g_nin].vk = vk_of(name) & 0xFF;
    g_in[g_nin].x = -1;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = hold;
    return 1;
}
static int parse_click(const char* arg) {
    int x, y;
    unsigned at = 0;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%d,%d@%u", &x, &y, &at) != 3) return 0;
    g_in[g_nin].vk = VK_LBUTTON;
    g_in[g_nin].x = x;
    g_in[g_nin].y = y;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = 80;
    return 1;
}
static DWORD WINAPI input_thread(LPVOID p) {
    int i = (int)(intptr_t)p;
    DWORD t0 = GetTickCount();
    while (!ddh_window() || GetTickCount() - t0 < g_in[i].at) Sleep(5);
    /* A popup window up takes the input: clicks stay in main-window
     * coordinates and are mapped onto it. (The game's own dialogs are drawn
     * on its screen, not windows; this is for a system one.) */
    HWND w = ddh_window(), pop = GetLastActivePopup(w);
    int vk = g_in[i].vk;
    if (g_in[i].x >= 0) {
        int cx = g_in[i].x, cy = g_in[i].y;
        present_unmap(&cx, &cy);            /* the presenter maps it back to X,Y */
        /* To the deepest child window under the point, in its coordinates. */
        POINT pt = { cx, cy };
        if (pop != w) { MapWindowPoints(w, pop, &pt, 1); w = pop; }
        for (HWND c; (c = RealChildWindowFromPoint(w, pt)) && c != w; w = c)
            MapWindowPoints(w, c, &pt, 1);
        LPARAM xy = MAKELPARAM(pt.x, pt.y);
        fprintf(stderr, "[input] click %d,%d at %lu ms -> hwnd %p at %ld,%ld\n", g_in[i].x, g_in[i].y,
                GetTickCount() - t0, (void*)w, pt.x, pt.y);
        PostMessageA(w, WM_MOUSEMOVE, 0, xy);
        Sleep(30);
        InterlockedIncrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, xy);
        Sleep(g_in[i].hold);
        InterlockedDecrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONUP, 0, xy);
        return 0;
    }
    /* Keys go where real ones would: the game thread's focus window, else
     * the popup. */
    GUITHREADINFO gi = { sizeof gi };
    w = GetGUIThreadInfo(GetWindowThreadProcessId(pop, NULL), &gi) && gi.hwndFocus ? gi.hwndFocus : pop;
    fprintf(stderr, "[input] key 0x%02X down at %lu ms -> hwnd %p\n", vk, GetTickCount() - t0, (void*)w);
    InterlockedIncrement(&g_held[vk]);
    PostMessageA(w, WM_KEYDOWN, vk, 1);
    Sleep(g_in[i].hold);
    InterlockedDecrement(&g_held[vk]);
    PostMessageA(w, WM_KEYUP, vk, 0xC0000001u);
    return 0;
}
static void shim_GetKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless ? 0 : GetKeyState(vk);
    RET((uint16_t)s, 1);
}
/* Windowed, a real key counts only while the game's window has the focus:
 * GetAsyncKeyState sees the whole desktop's keyboard. */
static void shim_GetAsyncKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless || !ddh_focused() ? 0 : GetAsyncKeyState(vk);
    RET((uint16_t)s, 1);
}

/* ------------------------------------------------------------ the CD */

/* The game wants its CD: at start it walks GetLogicalDriveStringsA for a
 * drive GetDriveTypeA calls DRIVE_CDROM (sub_00531F50) and asks for the disc
 * until it finds one with its files. The game folder holds the disc's data,
 * so it is offered as that CD: a free drive letter mapped onto it for this
 * logon session (DefineDosDevice, what `subst` does, no admin needed), which
 * GetDriveTypeA reports as a CD-ROM. A letter a crashed run left mapped onto
 * the same folder is reused. docs/architecture.md. */
static char g_cd[4];                     /* "X:\", or "" when no letter was free */
static char g_cd_target[MAX_PATH + 8];   /* what it maps to, for the removal */

static void cd_unmount(void) {
    char dev[3] = { g_cd[0], ':', 0 };
    if (g_cd[0] && DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH,
                                    dev, g_cd_target))
        g_cd[0] = 0;
}

static void cd_mount(void) {
    char folder[MAX_PATH], raw[MAX_PATH + 8], have[MAX_PATH + 8];
    strcpy(folder, g_game);
    folder[strlen(folder) - 1] = 0;                   /* no trailing '\' */
    _snprintf(raw, sizeof raw - 1, "\\??\\%s", folder);
    raw[sizeof raw - 1] = 0;
    for (char l = 'Z'; l >= 'D'; l--) {
        char dev[3] = { l, ':', 0 };
        int mine = QueryDosDeviceA(dev, have, sizeof have) && !_stricmp(have, raw);
        if (!mine && (QueryDosDeviceA(dev, have, sizeof have) || GetLastError() != ERROR_FILE_NOT_FOUND)) continue;
        if (mine || DefineDosDeviceA(DDD_RAW_TARGET_PATH, dev, raw)) {
            strcpy(g_cd_target, raw);
            g_cd[0] = l; g_cd[1] = ':'; g_cd[2] = '\\';
            fprintf(stderr, "[cd] %s is the game folder, as the Streets of SimCity CD\n", g_cd);
            return;
        }
    }
    fprintf(stderr, "[cd] no free drive letter: the game will ask for its CD\n");
}

static void shim_GetDriveTypeA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    if (g_cd[0] && root && toupper((unsigned char)root[0]) == g_cd[0] && root[1] == ':') {
        RET(DRIVE_CDROM, 1);
        return;
    }
    RET(GetDriveTypeA(root), 1);
}

/* ...and then wants the drive full, as a pressed disc is (sub_00513120):
 * no free clusters, the 2048-byte sectors of a CD, about 500 MB in all. */
static void shim_GetDiskFreeSpaceA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    DWORD* out[4] = { (DWORD*)(uintptr_t)ARG(1), (DWORD*)(uintptr_t)ARG(2),
                      (DWORD*)(uintptr_t)ARG(3), (DWORD*)(uintptr_t)ARG(4) };
    if (g_cd[0] && root && toupper((unsigned char)root[0]) == g_cd[0] && root[1] == ':') {
        DWORD v[4] = { 1, 2048, 0, 255500 };   /* sectors/cluster, bytes/sector, free, total */
        for (int i = 0; i < 4; i++) if (out[i]) *out[i] = v[i];
        RET(TRUE, 5);
        return;
    }
    RET(GetDiskFreeSpaceA(root, out[0], out[1], out[2], out[3]), 5);
}

/* Set by a rasterizer edge routine that returns to its caller's caller
 * (run_lift.py, "returns to the caller's caller"); the call site returns too. */
uint32_t ssc_unwind;
/* The guarded divide at 0x004C7993 would have raised (run_lift.py PATCHES). */
uint32_t ssc_div_ovf;

/* ------------------------------------------------------------ Glide */

/* The game looks for a 3dfx board at every start (0x004707D5), through
 * glide2x.dll, which a machine without a Voodoo does not have. These answer
 * "no boards", so it picks its software renderer as it would have; the rest
 * of Glide is called only once a board was found. An import left unbound
 * would be a call to 0: the lifted run skips it but leaves its stdcall
 * argument on the stack, and the original faults. */
static void shim_grSstQueryBoards(void) {
    uint32_t hw = ARG(0);                /* GrHwConfiguration*: num_sst first */
    if (hw) MEM32(hw) = 0;
    RET(1, 1);                           /* FXTRUE: the query worked, and found none */
}
static void shim_grGlideInit(void) { RET(0, 0); }

/* ------------------------------------------------------------ exit */

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), ddh_frames());
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    ExitProcess(ARG(0));
}

static native32_shim_t g_shims[] = {
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetKeyState", shim_GetKeyState },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "ExitProcess", shim_ExitProcess },
    { "GetDriveTypeA", shim_GetDriveTypeA },
    { "GetDiskFreeSpaceA", shim_GetDiskFreeSpaceA },
    { "_grSstQueryBoards@4", shim_grSstQueryBoards },
    { "_grGlideInit@0", shim_grGlideInit },
    DDH_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },          /* headless only: the last entry */
};

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    cd_unmount();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --original: the guarded divide's catch (run_lift.py PATCHES has the lifted
 * side). The game catches the divide overflow at 0x004C7993 with a C++
 * catch(...), but Windows will not run an exception handler that sits
 * outside a loaded image in a DEP process, and oracle.c maps Streets.exe
 * itself. So this does what the catch at 0x004C79BB does, less its debug
 * line ("Caught Exception in Divide"), and resumes where the catch returns
 * to, 0x004C79A4, with the stack as the try block left it. */
static LONG CALLBACK guarded_divide(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != STATUS_INTEGER_OVERFLOW &&
        ep->ExceptionRecord->ExceptionCode != STATUS_INTEGER_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Eip != 0x004C7993u) return EXCEPTION_CONTINUE_SEARCH;
    MEM32(0xB27A58) = 1;
    MEM32(0x68AC50) = 1;
    MEM32(0x68AC54) = 0;
    c->Eip = 0x004C79A4u;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Where every other thread is: its eip and the host return addresses on its
 * stack, newest first. `py -3 tools/addr2line.py` names them. */
static void dump_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    uintptr_t lo = (uintptr_t)GetModuleHandleA(NULL), hi = lo + 0x4000000;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_CONTROL;
        if (t && SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                fprintf(stderr, "  thread %lu: eip=%08lX esp=%08lX host frames:", te.th32ThreadID, c.Eip, c.Esp);
                MEMORY_BASIC_INFORMATION mbi;
                int n = 0;
                if (VirtualQuery((void*)(uintptr_t)c.Esp, &mbi, sizeof mbi))
                    for (uint32_t* p = (uint32_t*)(uintptr_t)c.Esp;
                         (uint8_t*)p + 4 <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize && n < 16; p++)
                        if (*p >= lo && *p < hi) { fprintf(stderr, " %08X", *p); n++; }
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    dump_threads();
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, ddh_frames());
    native32_dump_icalls(8);
    recomp_dump_trace("watchdog");          /* with -DSSC_TRACE=ON: the last function entries */
    ddh_finish();
    cd_unmount();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game";
    int run = 0, scale = 0, original = 0, nlift = 0;
    static uint32_t lift[256];
    ddh_opts_t o = { 0 };
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            if (!parse_click(argv[++i])) { fprintf(stderr, "bad --click %s (want X,Y@MS)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            o.record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.stop_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            static char shot[MAX_PATH];
            char* colon = strchr(argv[++i], ':');
            o.shot_frame = atol(argv[i]);
            if (!colon || o.shot_frame < 1) { fprintf(stderr, "bad --shot %s (want N:file.bmp)\n", argv[i]); return 1; }
            GetFullPathNameA(colon + 1, MAX_PATH, shot, NULL);
            o.shot_path = shot;
        }
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld,%ld", &o.diff_a, &o.diff_b) != 2 || o.diff_b <= o.diff_a) {
                fprintf(stderr, "bad --diff %s (want A,B in ms, A < B)\n", argv[i]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--ddraw-trace")) o.trace = 1;
        else if (!strcmp(argv[i], "--original")) original = 1;
        else if (!strcmp(argv[i], "--lifted") && i + 1 < argc)      /* with --original */
            for (char* t = strtok(argv[++i], ","); t && nlift < 256; t = strtok(NULL, ","))
                lift[nlift++] = strtoul(t, NULL, 16);
        else {
            printf("usage: ssc [--run] [--headless] [--record out.mp4] [--frames N]\n"
                   "             [--shot N:file.bmp] [--diff A,B] [--key NAME@MS[+HOLD]] [--click X,Y@MS]\n"
                   "             [--scale N] [--game game] [--watchdog S] [--native-trace]\n"
                   "             [--callbacks] [--ddraw-trace] [--original [--lifted VA,...]]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sStreets.exe", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    o.headless = g_headless;
    o.scale = scale;
    ddh_init(&o);
    native32_init();
    SetDllDirectoryA(g_game);            /* SMACKW32.DLL sits in the game folder */
    printf("Streets of SimCity recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
    int nshims = (int)(sizeof g_shims / sizeof g_shims[0]) - !g_headless;
    LoadLibraryA("smackw32.dll");        /* now, so its imports can be pointed at the screen */
    ddh_hook_module("smackw32.dll");
    if (original && run) {
        /* The shipping machine code under the same host, shims and input
         * (oracle.c): the reference a lifted run is compared against. */
        if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   /* a crash ends it, no dialog */
        AddVectoredExceptionHandler(1, guarded_divide);
        cd_mount();
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        for (int i = 0; i < g_nin; i++)
            CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
        oracle_lift(lift, nlift);
        return oracle_run(g_guest_exe, SSC_BASE, g_shims, nshims, NULL, 0);
    }
    /* Lifted runs only: the original handles its own exceptions with SEH
     * (a guarded divide, 0x004C7993), and this would end it at the first. */
    AddVectoredExceptionHandler(0, crash);
    uint32_t span = native32_map(g_guest_exe, SSC_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, SSC_BASE); return 1; }
    printf("  mapped Streets.exe: 0x%08X-0x%08X\n", SSC_BASE, SSC_BASE + span);
    /* Streets.exe imports glide2x.dll statically, but calls it only when the
     * 3dfx renderer is picked; with no Voodoo there is no glide2x.dll, so its
     * other 35 slots stay 0 (the two it calls at start are shimmed: "Glide"
     * above) and the software renderer runs (docs/architecture.md). */
    int unresolved = native32_bind(SSC_BASE, g_shims, nshims);
    if (unresolved && !GetModuleHandleA("glide2x.dll") && unresolved == 35)
        fprintf(stderr, "[bind] no glide2x.dll: the 3dfx renderer is unavailable\n");
    else if (unresolved) return 1;
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", ssc_entry_va);
        return 0;
    }
    /* The game opens its data by relative path (BMP\, Scenarios\%s), from the current folder. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    cd_mount();
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    for (int i = 0; i < g_nin; i++)
        CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
    printf("  entering 0x%08X\n\n", ssc_entry_va);
    fflush(stdout);
    native32_call_guest(ssc_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    ddh_finish();
    cd_unmount();
    return (int)g_eax;
}
