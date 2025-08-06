#include "xinput.h"
#include "wine/debug.h"
#include "windows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

static XINPUT_STATE controller_state;
static CRITICAL_SECTION state_lock;

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

/* Mapări butoane din scriptul Python */
static const int BUTTON_MAP[71] = {
    [56] = BTN_A,
    [57] = BTN_B,
    [59] = BTN_Y,
    [60] = BTN_X,
    [62] = BTN_LB,
    [63] = BTN_RB,
    [66] = BTN_START,
    [67] = BTN_BACK,
    [68] = 0,        
    [69] = BTN_LS,
    [70] = BTN_RS
};

static const int BUTTON_MAP2[12] = {
    [1] = BTN_A,
    [2] = BTN_B,
    [3] = BTN_Y,
    [4] = BTN_X,
    [5] = BTN_LB,
    [6] = BTN_RB,
    [7] = BTN_START,
    [8] = BTN_BACK,
    [9] = 0,       
    [10] = BTN_LS,
    [11] = BTN_RS
};

/* Convertire hex string -> int */
static int hex_to_int(const char *hex) {
    int val = 0;
    while (*hex) {
        char c = *hex++;
        if (isdigit(c)) val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
    }
    return val;
}

/* Decodează valori signed din bytes */
static int decode_signed(const unsigned char *bytes, int len) {
    if (len == 1) return (int)(int8_t)bytes[0];
    if (len == 2) return (int)(int16_t)(bytes[0] | (bytes[1] << 8));
    return 0;
}

/* Procesează o linie din strace */
static void process_input_line(const char *line) {
    const char *start;
    const char *p;
    unsigned char raw[16] = {0};
    int raw_count = 0;

    start = strstr(line, "\"\\x");
    if (!start) return;
    p = start;

    while (*p && raw_count < 16) {
        if (*p == '\\' && *(p+1) == 'x') {
            TRACE("Procesare in bucla!\n");
            char hex[3] = {p[2], p[3], 0};  /* <- aici e ok pentru că e o variabilă locală temporară */
            raw[raw_count++] = (unsigned char)hex_to_int(hex);
            p += 4;
        } else p++;
    }

    if (raw[0] == 0x0F) {
        TRACE("Citire butoane!\n");
        int buttonID = raw[2];
        int pressed  = (raw[3] == 0x01);
        int axisX    = decode_signed(&raw[4], 2);
        int axisY    = decode_signed(&raw[6], 2);
        int axisID   = raw[8];

        if (buttonID < 12 && BUTTON_MAP2[buttonID] != 0) {
            if (pressed) controller_state.Gamepad.wButtons |= BUTTON_MAP2[buttonID];
            else controller_state.Gamepad.wButtons &= ~BUTTON_MAP2[buttonID];
        }

        if (axisID == 0) { 
            controller_state.Gamepad.sThumbLX = axisX;
            controller_state.Gamepad.sThumbLY = axisY;
        } else if (axisID == 1) { 
            controller_state.Gamepad.sThumbRX = axisX;
            controller_state.Gamepad.sThumbRY = axisY;
        } else if (axisID == 2) { 
            controller_state.Gamepad.bLeftTrigger = (axisX < 0 ? 0 : (axisX > 255 ? 255 : axisX));
            controller_state.Gamepad.bRightTrigger = (axisY < 0 ? 0 : (axisY > 255 ? 255 : axisY));
        } else if (axisID < 4) { 
            controller_state.Gamepad.wButtons &= ~(BTN_DPAD_UP|BTN_DPAD_DOWN|BTN_DPAD_LEFT|BTN_DPAD_RIGHT);
            if (axisX == -255) controller_state.Gamepad.wButtons |= BTN_DPAD_LEFT;
            else if (axisX == 255) controller_state.Gamepad.wButtons |= BTN_DPAD_RIGHT;
            else if (axisY == -255) controller_state.Gamepad.wButtons |= BTN_DPAD_UP;
            else if (axisY == 255) controller_state.Gamepad.wButtons |= BTN_DPAD_DOWN;
        }
    }

    LeaveCriticalSection(&state_lock);
}

/* Thread pentru rularea strace și parsing */
static DWORD WINAPI strace_thread(LPVOID arg) {
    FILE *pipe;
    char line[1024];
    pipe = popen("strace -xx -p $(pgrep app_process) -e trace=read -f 2>&1", "r");
    if (!pipe) {
        WARN("Nu pot porni strace!\n");
        return 0;
    }
    TRACE("Proces gasit!\n");
    while (fgets(line, sizeof(line), pipe)) {
        TRACE("Citire proces!\n");
        process_input_line(line);
    }

    pclose(pipe);
    return 0;
}

/* XInput API */
DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state) {
    if (!state) return ERROR_BAD_ARGUMENTS;

    EnterCriticalSection(&state_lock);
    *state = controller_state;
    LeaveCriticalSection(&state_lock);

    return ERROR_SUCCESS;
}

/* DLL entry point */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        memset(&controller_state, 0, sizeof(controller_state));
        InitializeCriticalSection(&state_lock);
        CreateThread(NULL, 0, strace_thread, NULL, 0, NULL);

        TRACE("DLL-ul XInput a fost încărcat!\n");
    }
    return TRUE;
}


DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration) {
    return ERROR_SUCCESS;
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *caps) {
    TRACE("DLL-ul XInput a fost apelat!\n");
    
    if (!caps) return ERROR_BAD_ARGUMENTS;
    memset(caps, 0, sizeof(*caps));
    return ERROR_SUCCESS;
}

void WINAPI XInputEnable(BOOL enable) {
    /* Ignorat */
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture) {
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery) {
    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke) {
    return ERROR_EMPTY;
}

DWORD WINAPI XInputGetStateEx(DWORD index, XINPUT_STATE *state) {
    return XInputGetState(index, state);
}

DWORD WINAPI XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    return ERROR_NOT_SUPPORTED;
}



