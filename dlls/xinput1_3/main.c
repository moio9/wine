#include "xinput.h"
#include "wine/debug.h"
#include "windows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

#define PRT(...) do { \
    fprintf(stderr, __VA_ARGS__); fflush(stderr); \
    TRACE(__VA_ARGS__); \
} while (0)

/* Detectăm buildul PE (x86_64-windows) vs unix */
#ifdef __WINE_PE_BUILD
# define WINE_PE 1
#else
# define WINE_PE 0
#endif

#if !WINE_PE
# include <fcntl.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
#endif

#if WINE_PE
# define POPEN  _popen
# define PCLOSE _pclose
#else
# define POPEN  popen
# define PCLOSE pclose
#endif

static XINPUT_STATE controller_state;
static CRITICAL_SECTION state_lock;

static const char *default_cmd =
    "strace -xx -p $(pgrep app_process) -e trace=read -f 2>&1";

/* Butoane XInput */
#define BTN_A    XINPUT_GAMEPAD_A
#define BTN_B    XINPUT_GAMEPAD_B
#define BTN_X    XINPUT_GAMEPAD_X
#define BTN_Y    XINPUT_GAMEPAD_Y
#define BTN_LB   XINPUT_GAMEPAD_LEFT_SHOULDER
#define BTN_RB   XINPUT_GAMEPAD_RIGHT_SHOULDER
#define BTN_START XINPUT_GAMEPAD_START
#define BTN_BACK XINPUT_GAMEPAD_BACK
#define BTN_LS   XINPUT_GAMEPAD_LEFT_THUMB
#define BTN_RS   XINPUT_GAMEPAD_RIGHT_THUMB
#define BTN_DPAD_UP    XINPUT_GAMEPAD_DPAD_UP
#define BTN_DPAD_DOWN  XINPUT_GAMEPAD_DPAD_DOWN
#define BTN_DPAD_LEFT  XINPUT_GAMEPAD_DPAD_LEFT
#define BTN_DPAD_RIGHT XINPUT_GAMEPAD_DPAD_RIGHT

/* Mapări butoane din feed */
static const int BUTTON_MAP2[12] = {
    [1]  = BTN_A,
    [2]  = BTN_B,
    [3]  = BTN_Y,
    [4]  = BTN_X,
    [5]  = BTN_LB,
    [6]  = BTN_RB,
    [7]  = BTN_START,
    [8]  = BTN_BACK,
    [9]  = 0,
    [10] = BTN_LS,
    [11] = BTN_RS
};

static void dump_state_locked(void)
{
    /* presupune că ai deja lock-ul */
    PRT("STATE: buttons=0x%04x LT=%3u RT=%3u  L(%6d,%6d)  R(%6d,%6d)\n",
        controller_state.Gamepad.wButtons,
        controller_state.Gamepad.bLeftTrigger,
        controller_state.Gamepad.bRightTrigger,
        controller_state.Gamepad.sThumbLX,
        controller_state.Gamepad.sThumbLY,
        controller_state.Gamepad.sThumbRX,
        controller_state.Gamepad.sThumbRY);
}

/* Convertire hex string -> int */
static int hex_to_int(const char *hex)
{
    int val = 0;
    while (*hex) {
        unsigned char c = (unsigned char)*hex++;
        if (isdigit(c)) val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
        else break;
    }
    return val;
}

/* Decodează valori signed din bytes */
static int decode_signed(const unsigned char *bytes, int len)
{
    if (len == 1) return (int)(int8_t)bytes[0];
    if (len == 2) return (int)(int16_t)(bytes[0] | (bytes[1] << 8));
    return 0;
}

/* Procesează o linie din strace/pipe/file */
static void process_input_line(const char *line)
{
    const char *start = NULL;
    const char *p = NULL;
    unsigned char raw[16];
    int raw_count = 0;

    memset(raw, 0, sizeof(raw));

    if (!line) return;

    /* Log scurt al liniei citite (trunchiat) */
    {
        int L = (int)strlen(line);
        if (L > 160) L = 160;
        PRT("FEED: %.*s%s\n", L, line, (int)strlen(line) > L ? " ..." : "");
    }

    start = strstr(line, "\"\\x");
    if (!start) return;
    p = start;

    while (*p && raw_count < 16) {
        if (p[0] == '\\' && p[1] == 'x' && p[2] && p[3]) {
            char hex[3];
            hex[0] = p[2];
            hex[1] = p[3];
            hex[2] = 0;
            raw[raw_count++] = (unsigned char)hex_to_int(hex);
            p += 4;
        } else {
            p++;
        }
    }

    /* hexdump */
    {
        int i;
        PRT("RAW[%d]:", raw_count);
        for (i = 0; i < raw_count; i++) PRT(" %02X", raw[i]);
        PRT("\n");
    }

    if (raw_count >= 9 && raw[0] == 0x0F) {
        int buttonID = raw[2];
        int pressed  = (raw[3] == 0x01);
        int axisX    = decode_signed(&raw[4], 2);
        int axisY    = decode_signed(&raw[6], 2);
        int axisID   = raw[8];

        PRT("PARSE: id=0x%02X buttonID=%d pressed=%d axisX=%d axisY=%d axisID=%d\n",
            raw[0], buttonID, pressed, axisX, axisY, axisID);

        EnterCriticalSection(&state_lock);

        if (buttonID >= 0 && buttonID < 12 && BUTTON_MAP2[buttonID] != 0) {
            if (pressed) controller_state.Gamepad.wButtons |= BUTTON_MAP2[buttonID];
            else         controller_state.Gamepad.wButtons &= ~BUTTON_MAP2[buttonID];
        }

        if (axisID == 0) { /* stick stânga */
            controller_state.Gamepad.sThumbLX = (SHORT)axisX;
            controller_state.Gamepad.sThumbLY = (SHORT)axisY;
        } else if (axisID == 1) { /* stick dreapta */
            controller_state.Gamepad.sThumbRX = (SHORT)axisX;
            controller_state.Gamepad.sThumbRY = (SHORT)axisY;
        } else if (axisID == 2) { /* triggere 0..255 din axisX/Y */
            int lt = axisX; if (lt < 0) lt = 0; if (lt > 255) lt = 255;
            int rt = axisY; if (rt < 0) rt = 0; if (rt > 255) rt = 255;
            controller_state.Gamepad.bLeftTrigger  = (BYTE)lt;
            controller_state.Gamepad.bRightTrigger = (BYTE)rt;
        } else if (axisID < 4) { /* DPAD pe -255/255 */
            controller_state.Gamepad.wButtons &=
                ~(BTN_DPAD_UP|BTN_DPAD_DOWN|BTN_DPAD_LEFT|BTN_DPAD_RIGHT);
            if (axisX == -255) controller_state.Gamepad.wButtons |= BTN_DPAD_LEFT;
            else if (axisX == 255) controller_state.Gamepad.wButtons |= BTN_DPAD_RIGHT;
            else if (axisY == -255) controller_state.Gamepad.wButtons |= BTN_DPAD_UP;
            else if (axisY == 255)  controller_state.Gamepad.wButtons |= BTN_DPAD_DOWN;
        }

        dump_state_locked();
        LeaveCriticalSection(&state_lock);
    } else {
        PRT("SKIP: pattern not matched (raw_count=%d, raw[0]=0x%02X)\n",
            raw_count, raw_count ? raw[0] : 0xFF);
    }
}

/* Thread pentru FIFO/FILE/FEED */
static DWORD WINAPI strace_thread(LPVOID arg)
{
    const char *fifo = getenv("XINPUT_FIFO");
    const char *path = getenv("XINPUT_FILE");
    const char *cmd  = getenv("XINPUT_FEED");
    char line[1024];

    /* Fallback în /tmp dacă pui XINPUT_USE_TMP=1 */
    const char *use_tmp = getenv("XINPUT_USE_TMP");
    const char *tmp = getenv("XINPUT_TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/home/moioyoyo/Documente/wine";

#if !WINE_PE
    char default_fifo[512];
#endif
    char default_file[512];

#if !WINE_PE
    if (use_tmp && (!fifo || !*fifo)) {
        snprintf(default_fifo, sizeof(default_fifo), "%s/xinput-feed", tmp);
        fifo = default_fifo;
    }
#endif
    if (use_tmp && (!path || !*path)) {
        snprintf(default_file, sizeof(default_file), "%s/xinput-feed.log", tmp);
        path = default_file;
    }

    PRT("XINPUT thread started. Build=%s  FIFO=%s  FILE=%s  FEED=%s\n",
        WINE_PE ? "PE" : "unix",
        fifo ? fifo : "(null)",
        path ? path : "(null)",
        cmd  ? cmd  : "(null)");

#if !WINE_PE
    if (fifo && *fifo) {
        int fd = -1;
        PRT("MODE: FIFO -> %s\n", fifo);
        mkfifo(fifo, 0666); /* ignoră EEXIST */

        for (;;) {
            fd = open(fifo, O_RDONLY | O_NONBLOCK);
            if (fd < 0) { PRT("WARN: Can't open FIFO %s (retry)\n", fifo); Sleep(100); continue; }

            for (;;) {
                ssize_t n = read(fd, line, sizeof(line) - 1);
                if (n > 0) {
                    line[n] = 0;
                    process_input_line(line);
                } else if (n == 0) {
                    close(fd); /* writerii au închis -> redeschide */
                    break;
                } else {
                    usleep(20000);
                }
            }
        }
        return 0;
    }
#else
    if (fifo && *fifo)
        PRT("NOTE: XINPUT_FIFO ignorat în build PE; folosește XINPUT_FILE sau XINPUT_FEED.\n");
#endif

    if (path && *path) {
        int poll_ms = 30; /* ms */
        const char *pint = getenv("XINPUT_FILE_POLL");
        if (pint && *pint) { int v = atoi(pint); if (v >= 5 && v <= 1000) poll_ms = v; }

        TRACE("MODE: FILE (first-line) -> %s (interval=%dms)\n", path, poll_ms);

        for (;;) {
            FILE *fp = fopen(path, "rb");
            if (fp) {
                char first[1024];

                if (fgets(first, sizeof(first), fp)) {
                    /* taie CR/LF */
                    size_t L = strlen(first);
                    while (L && (first[L-1] == '\n' || first[L-1] == '\r')) first[--L] = 0;

                    TRACE("FILE: first line -> %s\n", first);
                    process_input_line(first);
                } else {
                    TRACE("FILE: empty or unreadable\n");
                }
                fclose(fp);
            } else {
                TRACE("FILE: can't open %s\n", path);
            }

            Sleep(poll_ms);
        }
        return 0;
    }
    if (!cmd || !*cmd) cmd = default_cmd;
    PRT("MODE: FEED -> %s\n", cmd);

    {
        FILE *pipe = POPEN(cmd, "r");
        if (!pipe) { PRT("ERROR: Nu pot porni feed-ul!\n"); return 0; }

        while (fgets(line, sizeof(line), pipe))
            process_input_line(line);

        PCLOSE(pipe);
    }
    return 0;
}

/* Thread de AUTOPRESS – apasă BTN_A on/off ca să știi că merge chiar fără feed.
   Activezi cu: export XINPUT_AUTOPRESS=500 (ms). Orice valoare >0 pornește. */
static DWORD WINAPI autopress_thread(LPVOID arg)
{
    int interval = 500;
    const char *env = getenv("XINPUT_AUTOPRESS");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) interval = v;
    } else {
        return 0; /* nu e activat */
    }

    PRT("AUTOPRESS: enabled, interval=%dms, toggling BTN_A\n", interval);

    for (;;) {
        EnterCriticalSection(&state_lock);
        controller_state.Gamepad.wButtons |= BTN_A;
        dump_state_locked();
        LeaveCriticalSection(&state_lock);
        Sleep(interval);

        EnterCriticalSection(&state_lock);
        controller_state.Gamepad.wButtons &= ~BTN_A;
        dump_state_locked();
        LeaveCriticalSection(&state_lock);
        Sleep(interval);
    }
    /* not reached */
    /* return 0; */
}

/* XInput API */
DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    EnterCriticalSection(&state_lock);
    *state = controller_state;
    LeaveCriticalSection(&state_lock);

    return ERROR_SUCCESS;
}

/* DLL entry point */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE th1, th2;

        memset(&controller_state, 0, sizeof(controller_state));
        InitializeCriticalSection(&state_lock);

        th1 = CreateThread(NULL, 0, strace_thread, NULL, 0, NULL);
        if (th1) CloseHandle(th1);

        th2 = CreateThread(NULL, 0, autopress_thread, NULL, 0, NULL);
        if (th2) CloseHandle(th2);

        TRACE("XInput custom DLL loaded\n");
        PRT("DLL: XInput custom loaded (build=%s). Use env: XINPUT_FIFO / XINPUT_FILE / XINPUT_FEED / XINPUT_AUTOPRESS\n",
            WINE_PE ? "PE" : "unix");
    }
    return TRUE;
}

DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    if (!caps) return ERROR_BAD_ARGUMENTS;
    memset(caps, 0, sizeof(*caps));
    return ERROR_SUCCESS;
}

void WINAPI XInputEnable(BOOL enable) { /* Ignorat */ }

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture)
{
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    return ERROR_EMPTY;
}

DWORD WINAPI XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    return XInputGetState(index, state);
}

/* Dacă nu ai XINPUT_CAPABILITIES_EX în headers, comentează funcția asta. */
DWORD WINAPI XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    return ERROR_NOT_SUPPORTED;
}
