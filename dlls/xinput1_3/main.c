/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 * Copyright 2025 Moio 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "winnls.h"
#include "winternl.h"
#include "winsock2.h"

#include "dbt.h"
#include "setupapi.h"
#include "initguid.h"
#include "devguid.h"
#include "xinput.h"

#include "wine/debug.h"

/* Not defined in the headers, used only by XInputGetStateEx */
#define XINPUT_GAMEPAD_GUIDE 0x0400

#define SERVER_PORT 4602
#define CLIENT_PORT 4600
#define BUFFER_SIZE 64

#define REQUEST_CODE_HELLO       1
#define REQUEST_CODE_HELLO_ACK   2
#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10
#define REQUEST_CODE_SET_RUMBLE 11
#define REQUEST_CODE_ENABLE_MASK 12

#define FLAG_DINPUT_MAPPER_STANDARD 0x01
#define FLAG_DINPUT_MAPPER_XINPUT 0x02
#define FLAG_INPUT_TYPE_XINPUT 0x04
#define FLAG_INPUT_TYPE_DINPUT 0x08

#define IDX_BUTTON_A 0
#define IDX_BUTTON_B 1
#define IDX_BUTTON_X 2
#define IDX_BUTTON_Y 3
#define IDX_BUTTON_L1 4
#define IDX_BUTTON_R1 5
#define IDX_BUTTON_L2 10
#define IDX_BUTTON_R2 11
#define IDX_BUTTON_SELECT 6
#define IDX_BUTTON_START 7
#define IDX_BUTTON_L3 8
#define IDX_BUTTON_R3 9

static char input_type = 0;

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

struct xinput_controller
{
    CRITICAL_SECTION crit;
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE state;
    XINPUT_GAMEPAD last_keystroke;
    BOOL enabled;
    BOOL connected;
    int id;
};

static struct xinput_controller controller;

static LONG controller_cs_inited = 0;

static HANDLE start_event;
static BOOL thread_running = FALSE;

static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;
static char xinput_min_index = 3;

static char   cfg_bind_ip[16]   = "127.0.0.1";   /* unde ascultă DLL (recv) */
static char   cfg_peer_ip[16] = "127.0.0.1";
static USHORT cfg_client_port   = CLIENT_PORT;   /* unde TRIMITE DLL -> python */
static USHORT cfg_server_port   = SERVER_PORT;   /* unde ASCULTĂ DLL (recv)   */
static unsigned int cfg_poll_hz = 100; 
static WORD   cfg_max_rumble_ms = 500;
static int    cfg_gamepad_id = 1;
static BOOL   cfg_enabled_default = TRUE;

static BOOL  g_enabled_all = TRUE;         /* XInputEnable(TRUE/FALSE) */
static DWORD g_enabled_mask = 0xFFFFFFFF;  /* bitmask pe ID-uri (1..32) */

static inline DWORD id_bit(int id)
{
    if (id < 1 || id > 32) return 0;
    return 1u << (id - 1);
}
static inline BOOL is_id_enabled(int id)
{
    return g_enabled_all && (g_enabled_mask & id_bit(id));
}

static DWORD env_u32(const char *name, DWORD defval)
{
    char buf[32];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return defval;
    return (DWORD)strtoul(buf, NULL, 0);
}

static BOOL env_bool(const char *name, BOOL defval)
{
    return env_u32(name, defval ? 1 : 0) ? TRUE : FALSE;
}

static void controller_cs_init(void)
{
    /* doar primul thread care schimbă 0->1 execută InitializeCriticalSection */
    if (InterlockedCompareExchange(&controller_cs_inited, 1, 0) == 0)
        InitializeCriticalSection(&controller.crit);
}

static void close_server_socket(void) 
{
    if (server_sock != INVALID_SOCKET) 
    {
        closesocket(server_sock);
        server_sock = INVALID_SOCKET;
    }
    
    if (winsock_loaded) 
    {
        WSACleanup();
        winsock_loaded = FALSE;
    }
}

static BOOL create_server_socket(void)
{
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    const UINT reuse_addr = 1;
    ULONG non_blocking = 1;
    int res;

    close_server_socket();

    winsock_loaded = (WSAStartup(MAKEWORD(2,2), &wsa_data) == NO_ERROR);
    if (!winsock_loaded) return FALSE;

    server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_sock == INVALID_SOCKET) return FALSE;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(cfg_bind_ip);
    server_addr.sin_port = htons(cfg_server_port);

    res = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse_addr, sizeof(reuse_addr));
    if (res == SOCKET_ERROR) return FALSE;

    ioctlsocket(server_sock, FIONBIO, &non_blocking);

    res = bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res == SOCKET_ERROR) return FALSE;

    return TRUE;
}

static void send_hello(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;

    memset(buffer, 0, sizeof(buffer));
    buffer[0] = REQUEST_CODE_HELLO;
    buffer[1] = 1; /* ver_major */
    buffer[2] = 0; /* ver_minor */

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(cfg_peer_ip);
    client_addr.sin_port = htons(cfg_client_port);

    sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static void get_gamepad_request(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int id;                    /* <-- declară AICI, înainte de cod */
    int nbytes;                /* dacă vrei să verifici sendto(...) */

    memset(buffer, 0, sizeof(buffer));
    memset(&client_addr, 0, sizeof(client_addr));

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(cfg_peer_ip);
    client_addr.sin_port = htons(cfg_client_port);

    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 1;  /* num (dummy) */
    id = cfg_gamepad_id;                      /* setează */
    memcpy(buffer + 2, &id, sizeof(id));      /* copiază LE pe Windows */

    nbytes = sendto(server_sock, buffer, BUFFER_SIZE, 0,
                    (struct sockaddr*)&client_addr, sizeof(client_addr));
    (void)nbytes;  /* ca să scapi de warning dacă nu-l folosești */
}

static void release_gamepad_request(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;

    memset(buffer, 0, sizeof(buffer));
    memset(&client_addr, 0, sizeof(client_addr));

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(cfg_peer_ip);
    client_addr.sin_port = htons(cfg_client_port);

    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto(server_sock, buffer, BUFFER_SIZE, 0,
           (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static BOOL controller_check_caps(void)
{
    XINPUT_CAPABILITIES *caps = &controller.caps;
    memset(caps, 0, sizeof(XINPUT_CAPABILITIES));
    
    caps->Gamepad.wButtons = 0xffff;
    caps->Gamepad.bLeftTrigger = (1u << (sizeof(caps->Gamepad.bLeftTrigger) + 1)) - 1;
    caps->Gamepad.bRightTrigger = (1u << (sizeof(caps->Gamepad.bRightTrigger) + 1)) - 1;
    caps->Gamepad.sThumbLX = (1u << (sizeof(caps->Gamepad.sThumbLX) + 1)) - 1;
    caps->Gamepad.sThumbLY = (1u << (sizeof(caps->Gamepad.sThumbLY) + 1)) - 1;
    caps->Gamepad.sThumbRX = (1u << (sizeof(caps->Gamepad.sThumbRX) + 1)) - 1;
    caps->Gamepad.sThumbRY = (1u << (sizeof(caps->Gamepad.sThumbRY) + 1)) - 1;

    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return TRUE;
}

static DWORD parse_enabled_ids_from_env(void)
{
    char  buf[128];
    DWORD len;
    DWORD mask;
    char *ctx;
    char *tok;

    len = GetEnvironmentVariableA("XINPUT_IDS", buf, sizeof(buf));
    if (!len || len >= sizeof(buf))
        return 0xFFFFFFFF; /* default: toate pornite */

    mask = 0;
    ctx = NULL;

#if defined(_MSC_VER) || defined(__MINGW64_VERSION_MAJOR)
    /* ai strtok_s? atunci folosește-l */
    tok = strtok_s(buf, " ,;:", &ctx);
    while (tok)
    {
        int id = atoi(tok);
        if (id >= 1 && id <= 32) mask |= id_bit(id);
        tok = strtok_s(NULL, " ,;:", &ctx);
    }
#else
    /* fallback portabil */
    tok = strtok(buf, " ,;:");
    while (tok)
    {
        int id = atoi(tok);
        if (id >= 1 && id <= 32) mask |= id_bit(id);
        tok = strtok(NULL, " ,;:");
    }
#endif

    return mask ? mask : 0; /* dacă lista e goală => nimic permis */
}

static void controller_destroy(void)
{
    if (controller_cs_inited)
    {
        EnterCriticalSection(&controller.crit);
        thread_running = FALSE;
        release_gamepad_request();
        xinput_min_index = 3;
        controller.enabled = FALSE;
        controller.connected = FALSE;
        close_server_socket();
        LeaveCriticalSection(&controller.crit);

        /* doar dacă era inițializat => dărâmăm CS și resetăm flag-ul */
        if (InterlockedCompareExchange(&controller_cs_inited, 0, 1) == 1)
            DeleteCriticalSection(&controller.crit);
    }
}

static void controller_init(void)
{
    memset(&controller.state, 0, sizeof(controller.state));
    controller_check_caps();
    controller.connected = TRUE;
    controller.enabled = TRUE;
}

static void controller_update_state(char *buffer)
{
    int i, gamepad_id;
    char dpad;
    short buttons, thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    XINPUT_STATE *state = &controller.state;
    
    if (!controller_cs_inited) return; /* safety */
    EnterCriticalSection(&controller.crit);
    
    gamepad_id = *(int*)(buffer + 2);
    if (buffer[1] != 1 || gamepad_id != controller.id) 
    {
        controller.connected = FALSE;
        memset(&controller.state, 0, sizeof(controller.state));
        LeaveCriticalSection(&controller.crit);
        return;
    }
    
    buttons = *(short*)(buffer + 6);    
    dpad = buffer[8];
    
    thumb_lx = *(short*)(buffer + 9);
    thumb_ly = *(short*)(buffer + 11);
    thumb_rx = *(short*)(buffer + 13);
    thumb_ry = *(short*)(buffer + 15);

    state->Gamepad.wButtons = 0;
    for (i = 0; i < 10; i++)
    {    
        if ((buttons & (1<<i))) {
            switch (i)
            {
            case IDX_BUTTON_A: state->Gamepad.wButtons |= XINPUT_GAMEPAD_A; break;
            case IDX_BUTTON_B: state->Gamepad.wButtons |= XINPUT_GAMEPAD_B; break;
            case IDX_BUTTON_X: state->Gamepad.wButtons |= XINPUT_GAMEPAD_X; break;
            case IDX_BUTTON_Y: state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y; break;
            case IDX_BUTTON_L1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
            case IDX_BUTTON_R1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
            case IDX_BUTTON_SELECT: state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK; break;
            case IDX_BUTTON_START: state->Gamepad.wButtons |= XINPUT_GAMEPAD_START; break;
            case IDX_BUTTON_L3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
            case IDX_BUTTON_R3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
            }
        }
    }
    
    //state->Gamepad.bLeftTrigger = (buttons & (1<<10)) ? 255 : 0;
    //state->Gamepad.bRightTrigger = (buttons & (1<<11)) ? 255 : 0;
    state->Gamepad.bLeftTrigger = *(unsigned char*)(buffer + 17);
    state->Gamepad.bRightTrigger = *(unsigned char*)(buffer + 18);

    switch (dpad)
    {
    case 0: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
    case 1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 2: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 4: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 6: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 7: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
    }

    state->Gamepad.sThumbLX = thumb_lx;
    state->Gamepad.sThumbLY = -thumb_ly;
    state->Gamepad.sThumbRX = thumb_rx;
    state->Gamepad.sThumbRY = -thumb_ry;
    
    state->dwPacketNumber++;
    LeaveCriticalSection(&controller.crit);
}

static void send_rumble_request(WORD left, WORD right, WORD duration_ms)
{
    char buf[BUFFER_SIZE];
    struct sockaddr_in cli;
    int r;
    int gamepad_id;

    if (server_sock == INVALID_SOCKET) return;

    memset(buf, 0, sizeof(buf));
    buf[0] = REQUEST_CODE_SET_RUMBLE; /* 0x0B */
    buf[1] = 1;                       /* num_gamepads */
    gamepad_id = (controller.id > 0) ? controller.id : 1;
    *(int*)(buf + 2)  = gamepad_id;        /* id LE */
    *(unsigned short*)(buf + 6)  = left;   /* left  LE */
    *(unsigned short*)(buf + 8)  = right;  /* right LE */
    *(unsigned short*)(buf + 10) = duration_ms ? duration_ms : 100; /* dur LE */

    memset(&cli, 0, sizeof(cli));
    cli.sin_family      = AF_INET;
    cli.sin_addr.s_addr = inet_addr(cfg_peer_ip);
    cli.sin_port        = htons(cfg_client_port); /* >>> spre CLIENT_PORT (ex: 4600) <<< */

    r = sendto(server_sock, buf, BUFFER_SIZE, 0, (struct sockaddr*)&cli, sizeof(cli));
}

static DWORD WINAPI controller_read_thread_proc(void *param) {
    int res;
    char buffer[BUFFER_SIZE];
    BOOL started = FALSE;
    DWORD curr_time, last_time;
    
    SetThreadDescription(GetCurrentThread(), L"wine_xinput_controller_read");
    if (server_sock == INVALID_SOCKET && !create_server_socket()) 
    {
        SetEvent(start_event);
        return 0;
    }
    send_hello();
    get_gamepad_request();
    
    last_time = GetCurrentTime();
    while (thread_running)
    {
        res = recvfrom(server_sock, buffer, BUFFER_SIZE, 0, NULL, 0);
        if (res <= 0)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK) break;
            
            curr_time = GetCurrentTime();
            if ((curr_time - last_time) >= 2000) {
                get_gamepad_request();
                last_time = curr_time;
            }
            
            {
	    unsigned int ms = (cfg_poll_hz ? (1000 / cfg_poll_hz) : 10);
	    if (ms == 0) ms = 1;
	    Sleep(ms);
	   }
            continue;
        }
        
        if (buffer[0] == REQUEST_CODE_GET_GAMEPAD && res >= 7)
	{
	int gamepad_id = *(int*)(buffer + 2);
	input_type = buffer[6];

	EnterCriticalSection(&controller.crit);

	if (input_type & FLAG_INPUT_TYPE_XINPUT) {
	    controller.id = gamepad_id;
	    /* opțional: dacă vrei să fixezi complet ID-ul, sincronizează-l */
	    cfg_gamepad_id = gamepad_id;
	    controller.connected = (gamepad_id > 0);
	} else {
	    controller.id = 0;
	    controller.connected = FALSE;
	}

	LeaveCriticalSection(&controller.crit);

	    if (!started) { started = TRUE; SetEvent(start_event); }
	}
        else if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE && controller.connected)
        {
            controller_update_state(buffer);
        }
        else if (buffer[0] == REQUEST_CODE_HELLO_ACK)
	{
	    /* layout: [0]=0x02, [1]=maj, [2]=min, [3]=flags, [4..7]=id, [8..9]=max_ms, [10]=enabled */
	    int ack_id;
	    BOOL enabled;
	    WORD maxms;

	    ack_id = *(int*)(buffer + 4);
	    maxms = *(WORD*)(buffer + 8);
	    enabled = buffer[10] ? TRUE : FALSE;

	    //if (ack_id > 0) cfg_gamepad_id = ack_id;
	    if (maxms)      cfg_max_rumble_ms = maxms;
	    controller.enabled = enabled;
	}
	else if (buffer[0] == REQUEST_CODE_ENABLE_MASK && res >= 5)
	{
	    DWORD new_mask = *(DWORD*)(buffer + 1);  /* LE */
	    g_enabled_mask = new_mask;
	}
	    }
    
    return 0;
}

static BOOL WINAPI start_read_thread_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;
    
    if (controller_cs_inited == 0) {
          if (InterlockedCompareExchange(&controller_cs_inited, 1, 0) == 0)
               InitializeCriticalSection(&controller.crit);
    }
    
    thread_running = TRUE;

    start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!start_event) ERR("failed to create start event, error %lu\n", GetLastError());   
    
    thread = CreateThread(NULL, 0, controller_read_thread_proc, NULL, 0, NULL);
    if (!thread) ERR("failed to create read thread, error %lu\n", GetLastError());
    CloseHandle(thread);
    
    WaitForSingleObject(start_event, 2000);
    CloseHandle(start_event);
    
    return TRUE;
}

static void start_read_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_read_thread_once, NULL, NULL);
}

static BOOL controller_is_connected(DWORD index)
{
    BOOL connected;
    if (!controller_cs_inited) /* încă nu e gata? răspunde conservator */
        return (index == 0) && controller.connected && is_id_enabled(controller.id);

    EnterCriticalSection(&controller.crit);
    connected = (index == 0) && controller.connected && is_id_enabled(controller.id);
    LeaveCriticalSection(&controller.crit);
    return connected;
}


BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    TRACE("inst %p, reason %lu, reserved %p.\n", inst, reason, reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        char v[128];
        DWORD n;
        char *p;
        int a, b;

        DisableThreadLibraryCalls(inst);
        if (InterlockedCompareExchange(&controller_cs_inited, 1, 0) == 0)
            InitializeCriticalSection(&controller.crit);

        g_enabled_mask = parse_enabled_ids_from_env();

        /* XINPUT_ENABLE = 0/1 (default 1) */
        n = GetEnvironmentVariableA("XINPUT_ENABLE", v, sizeof(v));
        if (n && n < sizeof(v)) g_enabled_all = (atoi(v) != 0);

        /* XINPUT_ADDR = "127.0.0.1" */
        n = GetEnvironmentVariableA("XINPUT_ADDR", v, sizeof(v));
	if (n && n < sizeof(v)) {
	    v[sizeof(v)-1] = '\0';
	    lstrcpynA(cfg_bind_ip, v, sizeof(cfg_bind_ip));
	    lstrcpynA(cfg_peer_ip, v, sizeof(cfg_peer_ip)); /* simplu: aceeași */
	}

        /* XINPUT_PORTS = "CLIENT:SERVER" */
        n = GetEnvironmentVariableA("XINPUT_PORTS", v, sizeof(v));
        if (n && n < sizeof(v))
        {
            p = strchr(v, ':');
            if (p)
            {
                *p = '\0';
                a = atoi(v);
                b = atoi(p + 1);
                if (a > 0 && a < 65536) cfg_client_port = (USHORT)a;
                if (b > 0 && b < 65536) cfg_server_port = (USHORT)b;
            }
        }

        /* XINPUT_POLL_HZ = 1..1000 */
        n = GetEnvironmentVariableA("XINPUT_POLL_HZ", v, sizeof(v));
        if (n && n < sizeof(v))
        {
            unsigned int hz = (unsigned int)atoi(v);
            if (hz >= 1 && hz <= 1000) cfg_poll_hz = hz;
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        controller_destroy();
	if (InterlockedCompareExchange(&controller_cs_inited, 0, 1) == 1)
	    DeleteCriticalSection(&controller.crit);
        break;
    }
    return TRUE;
}

void WINAPI DECLSPEC_HOTPATCH XInputEnable(BOOL enable)
{
    TRACE("enable %d.\n", enable);

    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    start_read_thread();

    g_enabled_all = enable;
    if (!controller.connected) return;
    controller.enabled = enable;    
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    int r;
    char buf[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    WORD left, right, dur;

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    /* clamp & pregătește payload */
    left = vibration ? vibration->wLeftMotorSpeed : 0;
    right = vibration ? vibration->wRightMotorSpeed : 0;
    dur = cfg_max_rumble_ms; /* durata maximă din ENV, poate fi zero => fără vibrație lungă */

    memset(buf, 0, sizeof(buf));
    buf[0] = REQUEST_CODE_SET_RUMBLE;     /* 0x0B */
    buf[1] = 1;                           /* num */
    *(int*)(buf + 2) = cfg_gamepad_id;    /* id */
    *(WORD*)(buf + 6) = left;             /* LE */
    *(WORD*)(buf + 8) = right;            /* LE */
    *(WORD*)(buf + 10) = dur;             /* LE */

    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(cfg_peer_ip);
    client_addr.sin_port = htons(cfg_client_port);

    r = sendto(server_sock, buf, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
    (void)r;

    return ERROR_SUCCESS;
}

/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (index < xinput_min_index) xinput_min_index = index;
    if (index == xinput_min_index) index = 0;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;
    
    EnterCriticalSection(&controller.crit);
    *state = controller.state;
    LeaveCriticalSection(&controller.crit);
    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret;

    TRACE("index %lu, state %p.\n", index, state);

    ret = xinput_get_state(index, state);
    if (ret != ERROR_SUCCESS) return ret;

    /* The main difference between this and the Ex version is the media guide button */
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    TRACE("index %lu, state %p.\n", index, state);

    return xinput_get_state(index, state);
}

static const int JS_STATE_OFF = 0;
static const int JS_STATE_LOW = 1;
static const int JS_STATE_HIGH = 2;

static int joystick_state(const SHORT value)
{
    if (value > 20000) return JS_STATE_HIGH;
    if (value < -20000) return JS_STATE_LOW;
    return JS_STATE_OFF;
}

static WORD js_vk_offs(const int x, const int y)
{
    if (y == JS_STATE_OFF)
    {
      /*if (x == JS_STATE_OFF) shouldn't get here */
        if (x == JS_STATE_LOW) return 3; /* LEFT */
      /*if (x == JS_STATE_HIGH)*/ return 2; /* RIGHT */
    }
    if (y == JS_STATE_HIGH)
    {
        if (x == JS_STATE_OFF) return 0; /* UP */
        if (x == JS_STATE_LOW) return 4; /* UPLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 5; /* UPRIGHT */
    }
  /*if (y == JS_STATE_LOW)*/
    {
        if (x == JS_STATE_OFF) return 1; /* DOWN */
        if (x == JS_STATE_LOW) return 7; /* DOWNLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 6; /* DOWNRIGHT */
    }
}

static DWORD check_joystick_keystroke(XINPUT_KEYSTROKE *keystroke, const SHORT *cur_x, const SHORT *cur_y, 
                                      SHORT *last_x, SHORT *last_y, const WORD base_vk)
{
    int cur_vk = 0, cur_x_st, cur_y_st;
    int last_vk = 0, last_x_st, last_y_st;

    cur_x_st = joystick_state(*cur_x);
    cur_y_st = joystick_state(*cur_y);
    if (cur_x_st || cur_y_st)
        cur_vk = base_vk + js_vk_offs(cur_x_st, cur_y_st);

    last_x_st = joystick_state(*last_x);
    last_y_st = joystick_state(*last_y);
    if (last_x_st || last_y_st)
        last_vk = base_vk + js_vk_offs(last_x_st, last_y_st);

    if (cur_vk != last_vk)
    {
        if (last_vk)
        {
            /* joystick was set, and now different. send a KEYUP event, and set
             * last pos to centered, so the appropriate KEYDOWN event will be
             * sent on the next call. */
            keystroke->VirtualKey = last_vk;
            keystroke->Unicode = 0; /* unused */
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;

            *last_x = 0;
            *last_y = 0;

            return ERROR_SUCCESS;
        }

        /* joystick was unset, send KEYDOWN. */
        keystroke->VirtualKey = cur_vk;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;

        *last_x = *cur_x;
        *last_y = *cur_y;

        return ERROR_SUCCESS;
    }

    *last_x = *cur_x;
    *last_y = *cur_y;

    return ERROR_EMPTY;
}

static BOOL trigger_is_on(const BYTE value)
{
    return value > 30;
}

static DWORD check_for_keystroke(XINPUT_KEYSTROKE *keystroke)
{
    const XINPUT_GAMEPAD *cur;
    DWORD ret = ERROR_EMPTY;
    int i;

    static const struct
    {
        int mask;
        WORD vk;
    } buttons[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_START, VK_PAD_START },
        { XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
        { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
        { XINPUT_GAMEPAD_A, VK_PAD_A },
        { XINPUT_GAMEPAD_B, VK_PAD_B },
        { XINPUT_GAMEPAD_X, VK_PAD_X },
        { XINPUT_GAMEPAD_Y, VK_PAD_Y },
        /* note: guide button does not send an event */
    };

    cur = &controller.state.Gamepad;

    /*** buttons ***/
    for (i = 0; i < ARRAY_SIZE(buttons); ++i)
    {
        if ((cur->wButtons & buttons[i].mask) ^ (controller.last_keystroke.wButtons & buttons[i].mask))
        {
            keystroke->VirtualKey = buttons[i].vk;
            keystroke->Unicode = 0; /* unused */
            if (cur->wButtons & buttons[i].mask)
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
                controller.last_keystroke.wButtons |= buttons[i].mask;
            }
            else
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
                controller.last_keystroke.wButtons &= ~buttons[i].mask;
            }
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;
            ret = ERROR_SUCCESS;
            goto done;
        }
    }

    /*** triggers ***/
    if (trigger_is_on(cur->bLeftTrigger) ^ trigger_is_on(controller.last_keystroke.bLeftTrigger))
    {
        keystroke->VirtualKey = VK_PAD_LTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bLeftTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bLeftTrigger = cur->bLeftTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    if (trigger_is_on(cur->bRightTrigger) ^ trigger_is_on(controller.last_keystroke.bRightTrigger))
    {
        keystroke->VirtualKey = VK_PAD_RTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bRightTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bRightTrigger = cur->bRightTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    /*** joysticks ***/
    ret = check_joystick_keystroke(keystroke, &cur->sThumbLX, &cur->sThumbLY,
            &controller.last_keystroke.sThumbLX,
            &controller.last_keystroke.sThumbLY, VK_PAD_LTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

    ret = check_joystick_keystroke(keystroke, &cur->sThumbRX, &cur->sThumbRY,
            &controller.last_keystroke.sThumbRX,
            &controller.last_keystroke.sThumbRY, VK_PAD_RTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

done:

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{    
    TRACE("index %lu, reserved %lu, keystroke %p.\n", index, reserved, keystroke);

    if (index >= XUSER_MAX_COUNT && index != XUSER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index != XUSER_INDEX_ANY ? index : 0)) return ERROR_DEVICE_NOT_CONNECTED;
  
    return check_for_keystroke(keystroke);
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    DWORD ret;

    ret = XInputGetCapabilitiesEx(1, index, flags, &caps_ex);

    if (!ret) *capabilities = caps_ex.Capabilities;
    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    TRACE("unk %lu, index %lu, flags %#lx, capabilities %p.\n", unk, index, flags, caps);

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;
    
    EnterCriticalSection(&controller.crit);

    if (flags & XINPUT_FLAG_GAMEPAD && controller.caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD) 
        return ERROR_DEVICE_NOT_CONNECTED;
    else
    {
        caps->Capabilities = controller.caps;
        caps->VendorId = 0x045E; // Wireless Xbox 360 Controller
        caps->ProductId = 0x02A1;
    }

    LeaveCriticalSection(&controller.crit);
    return ERROR_SUCCESS;
}
