/*  DirectInput Gamepad device
 *
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

#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "winuser.h"
#include "winerror.h"
#include "winreg.h"
#include "dinput.h"
#include "winsock2.h"
#include "devguid.h"
#include "hidusage.h"

#include "dinput_private.h"
#include "device_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

#define BUFFER_SIZE 64

#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10
#define REQUEST_CODE_SET_RUMBLE 11

#define FLAG_DINPUT_MAPPER_STANDARD 0x01
#define FLAG_DINPUT_MAPPER_XINPUT 0x02
#define FLAG_INPUT_TYPE_XINPUT 0x04
#define FLAG_INPUT_TYPE_DINPUT 0x08

#define LAUNCH_TYPE_XINPUTONLY 0
#define LAUNCH_TYPE_DINPUTONLY 1
#define LAUNCH_TYPE_MIXED 2

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
#define IDX_BUTTON_HOME 12

/* Config runtime (setabilă prin env) */
static int   g_server_port    = 4601;
static int   g_client_port    = 4600;
static DWORD g_timeout_ms     = 2;      /* ~500 Hz implicit */
static BOOL  g_device_enable  = TRUE;
static BOOL  g_ff_supported   = TRUE;   /* mutată aici să poată fi setată din env */
static USHORT g_max_rumble_ms = 800;
static char  g_name_override[128];      /* "" = nu override */

/* mapper & input type override */
static BOOL  g_mapper_forced      = FALSE;
static char  g_mapper_forced_val  = FLAG_DINPUT_MAPPER_XINPUT; /* default rămâne xinput */
static BOOL  g_input_forced       = FALSE;
static char  g_input_forced_val   = 0;  /* FLAG_INPUT_TYPE_* */

static BOOL g_selftest_rumble = FALSE;
static BOOL g_selftest_done   = FALSE;


struct gamepad_state 
{
    short buttons;
    char dpad;
    short thumb_lx;
    short thumb_ly;
    short thumb_rx;
    short thumb_ry;
    unsigned char thumb_lz;
    unsigned char thumb_rz;    
};

struct gamepad
{
    struct dinput_device base;
    struct gamepad_state state;
    GUID requested_guid;
};

/* ==== Force Feedback shim: types & forwards ==== */
typedef struct gamepad_effect {
    IDirectInputEffect IDirectInputEffect_iface;
    LONG  ref;
    GUID  guid;
    LONG  magnitude;       /* 0..10000 */
    DWORD duration_ms;     /* ms */
    LONG  gain;            /* 0..10000 */
    LONG  period_ms;       /* for Sine; optional */
    BOOL  running;
    DWORD trigger_button;
} gamepad_effect;


/* forward decl pentru toate metodele eff_* (ca vtbl-ul să aibă prototipuri) */
static HRESULT WINAPI eff_QueryInterface(IDirectInputEffect*, REFIID, void**);
static ULONG   WINAPI eff_AddRef(IDirectInputEffect*);
static ULONG   WINAPI eff_Release(IDirectInputEffect*);
static HRESULT WINAPI eff_Initialize(IDirectInputEffect*, HINSTANCE, DWORD, REFGUID);
static HRESULT WINAPI eff_GetEffectGuid(IDirectInputEffect*, LPGUID);
static HRESULT WINAPI eff_GetParameters(IDirectInputEffect*, LPDIEFFECT, DWORD);
static HRESULT WINAPI eff_SetParameters(IDirectInputEffect*, LPCDIEFFECT, DWORD);
static HRESULT WINAPI eff_Start(IDirectInputEffect*, DWORD, DWORD);
static HRESULT WINAPI eff_Stop(IDirectInputEffect*);
static HRESULT WINAPI eff_GetEffectStatus(IDirectInputEffect*, LPDWORD);
static HRESULT WINAPI eff_Download(IDirectInputEffect*);
static HRESULT WINAPI eff_Unload(IDirectInputEffect*);
static HRESULT WINAPI eff_Escape(IDirectInputEffect*, LPDIEFFESCAPE);

/* forward decl pentru vtbl (ca să poți folosi &gamepad_effect_vtbl în create_effect) */
static const IDirectInputEffectVtbl gamepad_effect_vtbl;

static const struct dinput_device_vtbl gamepad_vtbl;
static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;
static int connected_gamepad_id = 0;
static char input_type = FLAG_DINPUT_MAPPER_XINPUT; /* poate fi forțat din env */


static inline struct gamepad *impl_from_IDirectInputDevice8W( IDirectInputDevice8W *iface )
{
    return CONTAINING_RECORD( CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface ), struct gamepad, base );
}

static void close_server_socket( void ) 
{
    if (server_sock != INVALID_SOCKET) 
    {
        closesocket( server_sock );
        server_sock = INVALID_SOCKET;
    }
    
    if (winsock_loaded) 
    {
        WSACleanup();
        winsock_loaded = FALSE;
    }    
}

/* helper: citire int din env cu fallback */
static int env_to_int(const char *key, int defv)
{
    const char *v = getenv(key);
    if (!v || !*v) return defv;
    return atoi(v);
}

/* lower-case helper minimal (doar pentru mici comparații) */
static void tolower_inplace(char *s)
{
    if (!s) return;
    for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

/* Încarcă setările din env. Apelată devreme (în create_device). */
static void load_env_config(void)
{
    const char *v;

    g_device_enable  = env_to_int("DINPUT_GAMEPAD_ENABLE", 1) ? TRUE : FALSE;
    g_server_port    = env_to_int("DINPUT_SERVER_PORT", 4601);
    g_client_port    = env_to_int("DINPUT_CLIENT_PORT", 4600);
    g_timeout_ms     = (DWORD)env_to_int("DINPUT_HZ", 0);
    g_selftest_rumble = env_to_int("DINPUT_SELFTEST_RUMBLE", 0) ? TRUE : FALSE;
    if (g_timeout_ms) {
        /* DINPUT_HZ = frecvență; convertim la timeout ms = max(1, 1000/HZ) */
        if (g_timeout_ms <= 0) g_timeout_ms = 2;
        else {
            DWORD hz = g_timeout_ms;
            g_timeout_ms = (hz >= 1000) ? 1 : (1000 / hz);
            if (!g_timeout_ms) g_timeout_ms = 1;
        }
    } else {
        g_timeout_ms = 2; /* default ~500 Hz */
    }

    g_max_rumble_ms  = (USHORT)env_to_int("DINPUT_MAX_RUMBLE_MS", 800);
    g_ff_supported   = env_to_int("DINPUT_FORCEFEEDBACK", 1) ? TRUE : FALSE;

    /* nume override */
    v = getenv("DINPUT_GAMEPAD_NAME");
    if (v && *v) {
        lstrcpynA(g_name_override, v, sizeof(g_name_override));
    } else g_name_override[0] = '\00';

    /* mapper override */
    v = getenv("DINPUT_GAMEPAD_MAPPER");
    if (v && *v) {
        char tmp[32]; lstrcpynA(tmp, v, sizeof(tmp));
        tolower_inplace(tmp);
        if (!strcmp(tmp, "standard")) { g_mapper_forced = TRUE; g_mapper_forced_val = FLAG_DINPUT_MAPPER_STANDARD; }
        else if (!strcmp(tmp, "xinput")) { g_mapper_forced = TRUE; g_mapper_forced_val = FLAG_DINPUT_MAPPER_XINPUT; }
    }

    /* input type override (xinput/dinput – afectează FLAG_INPUT_TYPE_*) */
    v = getenv("DINPUT_GAMEPAD_INPUTTYPE");
    if (v && *v) {
        char tmp[32]; lstrcpynA(tmp, v, sizeof(tmp));
        tolower_inplace(tmp);
        if (!strcmp(tmp, "xinput")) { g_input_forced = TRUE; g_input_forced_val = FLAG_INPUT_TYPE_XINPUT; }
        else if (!strcmp(tmp, "dinput")) { g_input_forced = TRUE; g_input_forced_val = FLAG_INPUT_TYPE_DINPUT; }
    }
}

static BOOL create_server_socket( void )
{
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    const UINT reuse_addr = 1;
    int res;
    int timeout; /* C89: declare before any statements */

    close_server_socket();

    winsock_loaded = WSAStartup( MAKEWORD(2,2), &wsa_data ) == NO_ERROR;
    if (!winsock_loaded) return FALSE;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    server_addr.sin_port = htons( g_server_port );

    server_sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if (server_sock == INVALID_SOCKET) return FALSE;

    res = setsockopt( server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr) );
    if (res == SOCKET_ERROR) return FALSE;

    timeout = (int)g_timeout_ms;
    res = setsockopt( server_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout) );
    if (res < 0) return FALSE;

    res = bind( server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr) );
    if (res == SOCKET_ERROR) return FALSE;

    return TRUE;
}

static BOOL get_gamepad_request( BOOL notify, char* gamepad_name ) 
{
    int res, gamepad_id;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    client_addr.sin_port = htons( g_client_port );
    
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 0;
    buffer[2] = notify ? 1 : 0;
    res = sendto( server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr) );
    if (res == SOCKET_ERROR) return FALSE;
    
    res = recvfrom( server_sock, buffer, BUFFER_SIZE, 0, NULL, 0 );
    if (res == SOCKET_ERROR || buffer[0] != REQUEST_CODE_GET_GAMEPAD) return FALSE;
    
    gamepad_id = *(int*)(buffer + 1);
    if (gamepad_id == 0) 
    {
        connected_gamepad_id = 0;
        return FALSE;
    }
    
    connected_gamepad_id = gamepad_id;
    input_type = buffer[5];
    if (g_input_forced) {
        /* păstrăm și mapper bits existenți, dar suprascriem doar tipul */
        input_type &= ~(FLAG_INPUT_TYPE_XINPUT | FLAG_INPUT_TYPE_DINPUT);
        input_type |= g_input_forced_val;
    }
    if (g_mapper_forced) {
        input_type &= ~(FLAG_DINPUT_MAPPER_STANDARD | FLAG_DINPUT_MAPPER_XINPUT);
        input_type |= g_mapper_forced_val;
    }
    if (!(input_type & FLAG_INPUT_TYPE_DINPUT)) {
        gamepad_id = 0;
        return FALSE;
    }
    
    if (gamepad_name != NULL) 
    {
        int name_len;
        name_len = *(int*)(buffer + 6);
        memcpy( gamepad_name, buffer + 10, name_len );
        gamepad_name[name_len] = '\0';
        if (g_name_override[0]) {
            lstrcpynA(gamepad_name, g_name_override, 63);
            gamepad_name[63] = '\0';
        }
    }
    
    return TRUE;
}

static LONG scale_value( LONG value, struct object_properties *properties )
{
    LONG log_min, log_max, phy_min, phy_max;
    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;

    return phy_min + MulDiv( value - log_min, phy_max - phy_min, log_max - log_min );
}

static LONG scale_axis_value( LONG value, struct object_properties *properties )
{
    LONG log_ctr, log_min, log_max, phy_ctr, phy_min, phy_max;
    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;

    if (phy_min == 0) phy_ctr = phy_max >> 1;
    else phy_ctr = round( (phy_min + phy_max) / 2.0 );
    if (log_min == 0) log_ctr = log_max >> 1;
    else log_ctr = round( (log_min + log_max) / 2.0 );

    value -= log_ctr;
    if (value <= 0)
    {
        log_max = MulDiv( log_min - log_ctr, properties->deadzone, 10000 );
        log_min = MulDiv( log_min - log_ctr, properties->saturation, 10000 );
        phy_max = phy_ctr;
    }
    else
    {
        log_min = MulDiv( log_max - log_ctr, properties->deadzone, 10000 );
        log_max = MulDiv( log_max - log_ctr, properties->saturation, 10000 );
        phy_min = phy_ctr;
    }

    if (value <= log_min) return phy_min;
    if (value >= log_max) return phy_max;
    return phy_min + MulDiv( value - log_min, phy_max - phy_min, log_max - log_min );
}

static void gamepad_handle_input( IDirectInputDevice8W *iface, short thumb_lx, short thumb_ly, short thumb_rx, short thumb_ry, unsigned char thumb_lz, unsigned char thumb_rz, short buttons, char dpad ) 
{
    int i, j, index;
    DWORD time, seq;
    BOOL notify = FALSE;
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    DIJOYSTATE *state = (DIJOYSTATE *)impl->base.device_state;
    
    time = GetCurrentTime();
    seq = impl->base.dinput->evsequence++;    
    
    if (input_type & FLAG_DINPUT_MAPPER_STANDARD) 
    {
        if (thumb_lx != impl->state.thumb_lx)
        {
            impl->state.thumb_lx = thumb_lx;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ) );
            state->lX = scale_axis_value( thumb_lx, impl->base.object_properties + index );
            queue_event( iface, index, state->lX, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ly != impl->state.thumb_ly) 
        {
            impl->state.thumb_ly = thumb_ly;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ) );
            state->lY = scale_axis_value( thumb_ly, impl->base.object_properties + index );        
            queue_event( iface, index, state->lY, time, seq );
            notify = TRUE;
        }
        
        if (thumb_rx != impl->state.thumb_rx) 
        {
            impl->state.thumb_rx = thumb_rx;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ) );
            state->lZ = scale_axis_value( thumb_rx, impl->base.object_properties + index );
            queue_event( iface, index, state->lZ, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ry != impl->state.thumb_ry) 
        {
            impl->state.thumb_ry = thumb_ry;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ) );
            state->lRz = scale_axis_value( thumb_ry, impl->base.object_properties + index );
            queue_event( iface, index, state->lRz, time, seq );
            notify = TRUE;
        }
        
        if (buttons != impl->state.buttons) 
        {
            impl->state.buttons = buttons;
            for (i = 0, j = 0; i < 12; i++)
            {
                switch (i)
                {
                case IDX_BUTTON_A: j = 1; break;
                case IDX_BUTTON_B: j = 2; break;
                case IDX_BUTTON_X: j = 0; break;
                case IDX_BUTTON_Y: j = 3; break;
                case IDX_BUTTON_L1: j = 4; break;
                case IDX_BUTTON_R1: j = 5; break;
                case IDX_BUTTON_L2: j = 6; break;
                case IDX_BUTTON_R2: j = 7; break;
                case IDX_BUTTON_SELECT: j = 8; break;
                case IDX_BUTTON_START: j = 9; break;
                case IDX_BUTTON_L3: j = 10; break;
                case IDX_BUTTON_R3: j = 11; break;                
                }
                
                state->rgbButtons[j] = (buttons & (1<<i)) ? 0x80 : 0x00;
                index = dinput_device_object_index_from_id( iface, DIDFT_BUTTON | DIDFT_MAKEINSTANCE( j ) );
                queue_event( iface, index, state->rgbButtons[j], time, seq );    
            }
            notify = TRUE;        
        }
        
        if (dpad != impl->state.dpad) 
        {
            impl->state.dpad = dpad;
            index = dinput_device_object_index_from_id( iface, DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ) );
            state->rgdwPOV[0] = dpad != -1 ? dpad * 4500 : -1;
            queue_event( iface, index, state->rgdwPOV[0], time, seq );
            notify = TRUE;
        }        
    }
    else if (input_type & FLAG_DINPUT_MAPPER_XINPUT)
    {
    	static const unsigned char map_x[11] = {0,1,2,3,4,5,7,6,8,9,10};
    	 j = map_x[i];
        if (thumb_lx != impl->state.thumb_lx) 
        {
            impl->state.thumb_lx = thumb_lx;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ) );
            state->lX = scale_axis_value( thumb_lx, impl->base.object_properties + index );
            queue_event( iface, index, state->lX, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ly != impl->state.thumb_ly) 
        {
            impl->state.thumb_ly = thumb_ly;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ) );
            state->lY = scale_axis_value( thumb_ly, impl->base.object_properties + index );        
            queue_event( iface, index, state->lY, time, seq );
            notify = TRUE;
        }
        
        if (thumb_rx != impl->state.thumb_rx) 
        {
            impl->state.thumb_rx = thumb_rx;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ) );
            state->lRx = scale_axis_value( thumb_rx, impl->base.object_properties + index );
            queue_event( iface, index, state->lRx, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ry != impl->state.thumb_ry) 
        {
            impl->state.thumb_ry = thumb_ry;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 ) );
            state->lRy = scale_axis_value( thumb_ry, impl->base.object_properties + index );
            queue_event( iface, index, state->lRy, time, seq );
            notify = TRUE;
        }
        
        if (thumb_lz != impl->state.thumb_lz || thumb_rz != impl->state.thumb_rz)
        {
            LONG value;
            index = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ) );

            // if lz and rz changed at the same time, only deal with lz.
            if (thumb_lz != impl->state.thumb_lz)
                value = MulDiv(thumb_lz, -32768, 255);
            else
                value = MulDiv(thumb_rz, 32767, 255);
            state->lZ = scale_axis_value( value, impl->base.object_properties + index );
            queue_event( iface, index, state->lZ, time, seq );

            impl->state.thumb_lz = thumb_lz;
            impl->state.thumb_rz = thumb_rz;
            notify = TRUE;
        }
        
    if (buttons != impl->state.buttons)
    {
        static const unsigned char map_x[10] = {0,1,2,3,4,5,7,6,8,9}; /* swap START(7) <-> BACK(6) */
        impl->state.buttons = buttons;

        for (i = 0; i < 10; i++)
        {
            j = map_x[i];
            state->rgbButtons[j] = (buttons & (1 << i)) ? 0x80 : 0x00;
            index = dinput_device_object_index_from_id( iface, DIDFT_BUTTON | DIDFT_MAKEINSTANCE(j) );
            queue_event( iface, index, state->rgbButtons[j], time, seq );
        }
        notify = TRUE;
    }
        
        if (dpad != impl->state.dpad) 
        {
            impl->state.dpad = dpad;
            index = dinput_device_object_index_from_id( iface, DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ) );
            state->rgdwPOV[0] = dpad != -1 ? dpad * 4500 : -1;
            queue_event( iface, index, state->rgdwPOV[0], time, seq );
            notify = TRUE;
        }        
    }
    
    if (notify && impl->base.hEvent) SetEvent( impl->base.hEvent );
}

static void release_gamepad_request( void ) 
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_addr_len;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    client_addr.sin_port = htons( g_client_port );
    client_addr_len = sizeof(client_addr);
    
    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto( server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len );
}

HRESULT gamepad_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version )
{   
    DWORD size;
    char gamepad_name[64];
    
    if (!create_server_socket() || !get_gamepad_request( FALSE, gamepad_name )) return DIERR_INPUTLOST;
    
    size = instance->dwSize;
    memset( instance, 0, size );
    instance->dwSize = size;
    instance->guidInstance = GUID_Joystick;
    instance->guidProduct = GUID_Joystick;
    instance->guidProduct.Data1 = MAKELONG( 0x045e, 0x028e );
    if (version >= 0x0800) instance->dwDevType = DIDEVTYPE_HID | DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    else instance->dwDevType = DIDEVTYPE_HID | DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    instance->wUsagePage = HID_USAGE_PAGE_GENERIC;
    instance->wUsage = HID_USAGE_GENERIC_GAMEPAD;
    MultiByteToWideChar( CP_ACP, 0, gamepad_name, -1, instance->tszInstanceName, MAX_PATH );
    MultiByteToWideChar( CP_ACP, 0, gamepad_name, -1, instance->tszProductName, MAX_PATH );
    
    return DI_OK;
}

static BOOL init_object_properties( struct dinput_device *device, UINT index, struct hid_value_caps *caps,
                                    const DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    struct object_properties *properties;

    if (index == -1) return DIENUM_STOP;
    properties = device->object_properties + index;

    properties->logical_min = -32768;
    properties->logical_max = 32767;
    properties->range_min = 0;
    properties->range_max = 65535;
    properties->saturation = 10000;
    properties->deadzone   = 0; /* added */
    properties->granularity = 1;

    return DIENUM_CONTINUE;
}

HRESULT gamepad_create_effect(IDirectInputDevice8W *iface, IDirectInputEffect **effect)
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W(iface);
    gamepad_effect *eff;

    if (!g_ff_supported) return DIERR_UNSUPPORTED;
    eff = (gamepad_effect*)calloc(1, sizeof(*eff));
    if (!eff) return E_OUTOFMEMORY;

    eff->IDirectInputEffect_iface.lpVtbl = &gamepad_effect_vtbl;
    eff->ref         = 1;
    eff->guid        = impl->requested_guid; /* set by get_effect_info() */
    if (!IsEqualGUID(&eff->guid, &GUID_ConstantForce) && !IsEqualGUID(&eff->guid, &GUID_Sine))
        eff->guid = GUID_ConstantForce; /* safe default */
    eff->gain        = 10000;
    eff->duration_ms = g_max_rumble_ms;

    *effect = &eff->IDirectInputEffect_iface;
    return DI_OK;
}

static void send_rumble_request(WORD left, WORD right, WORD duration_ms)
{
    char buf[BUFFER_SIZE];
    struct sockaddr_in cli;

    if (server_sock == INVALID_SOCKET) return;
    if (!duration_ms || duration_ms > g_max_rumble_ms) duration_ms = g_max_rumble_ms;

    memset(buf, 0, sizeof(buf));
    buf[0] = REQUEST_CODE_SET_RUMBLE;
    buf[1] = 1;
    *(int*)(buf + 2)           = connected_gamepad_id > 0 ? connected_gamepad_id : 1;
    *(unsigned short*)(buf+6)  = left;
    *(unsigned short*)(buf+8)  = right;
    *(unsigned short*)(buf+10) = duration_ms;

    memset(&cli, 0, sizeof(cli));
    cli.sin_family      = AF_INET;
    cli.sin_addr.s_addr = inet_addr("127.0.0.1");
    cli.sin_port        = htons(g_client_port);

    sendto(server_sock, buf, BUFFER_SIZE, 0, (struct sockaddr*)&cli, sizeof(cli));
}

static inline gamepad_effect *impl_from_IDirectInputEffect(IDirectInputEffect *iface)
{
    return CONTAINING_RECORD(iface, gamepad_effect, IDirectInputEffect_iface);
}

static void gamepad_release( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    CloseHandle( impl->base.read_event );
}

static HRESULT gamepad_read( IDirectInputDevice8W *iface )
{
    int res;
    char buffer[BUFFER_SIZE];

    if (server_sock == INVALID_SOCKET) return DI_OK;
    res = recvfrom( server_sock, buffer, BUFFER_SIZE, 0, NULL, 0 );
    if (res == SOCKET_ERROR) return DI_OK;

    if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE && buffer[1] == 1)
    {
        int gamepad_id;
        char dpad;
        short buttons, thumb_lx, thumb_ly, thumb_rx, thumb_ry;
        unsigned char thumb_lz, thumb_rz;

        gamepad_id = *(int*)(buffer + 2);
        if (gamepad_id != connected_gamepad_id) return DI_OK;

        buttons = *(short*)(buffer + 6);
        dpad = buffer[8];

        thumb_lx = *(short*)(buffer + 9);
        thumb_ly = *(short*)(buffer + 11);
        thumb_rx = *(short*)(buffer + 13);
        thumb_ry = *(short*)(buffer + 15);
        thumb_lz = (unsigned char)buffer[17];
        thumb_rz = (unsigned char)buffer[18];

        gamepad_handle_input( iface, thumb_lx, thumb_ly, thumb_rx, thumb_ry,
                              thumb_lz, thumb_rz, buttons, dpad );
    }
    return DI_OK;
}

static HRESULT gamepad_poll(IDirectInputDevice8W *iface)
{
    gamepad_read(iface);
    return DI_OK;
}

static HRESULT gamepad_acquire(IDirectInputDevice8W *iface)
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W(iface);

    if (server_sock == INVALID_SOCKET && !create_server_socket()) return DIERR_INPUTLOST;
    if (!get_gamepad_request(TRUE, NULL)) return DIERR_INPUTLOST;

    /* snapshot inițial (centrat) ca să miște joy.cpl imediat */
    gamepad_handle_input( iface, 0, 0, 0, 0, 0, 0, 0, -1 );

    SetEvent(impl->base.read_event);

    if (g_selftest_rumble && !g_selftest_done && connected_gamepad_id) {
	USHORT d = g_max_rumble_ms < 300 ? g_max_rumble_ms : 300;
	send_rumble_request(35000, 35000, d);
	Sleep(120);
	send_rumble_request(55000, 55000, d);
        g_selftest_done = TRUE;
    }
    return DI_OK;
}

static HRESULT gamepad_unacquire( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    WaitForSingleObject( impl->base.read_event, INFINITE );

    release_gamepad_request();
    send_rumble_request(1111, 1000, 1000);
    /* NU închide socketul aici – UI face Unacquire/Acquire la schimbarea coop level */
    // close_server_socket();
    return DI_OK;
}

static BOOL try_enum_object( struct dinput_device *impl, const DIPROPHEADER *filter, DWORD flags, enum_object_callback callback,
                             UINT index, DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE( instance->dwType ))) return DIENUM_CONTINUE;

    switch (filter->dwHow)
    {
    case DIPH_DEVICE:
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYOFFSET:
        if (filter->dwObj != instance->dwOfs) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYID:
        if ((filter->dwObj & 0x00ffffff) != (instance->dwType & 0x00ffffff)) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    }

    return DIENUM_CONTINUE;
}

static void get_device_objects( int *instance_count, DIDEVICEOBJECTINSTANCEW **out ) 
{
    int i, index = 0;
    
    *instance_count = 0;
    *out = NULL;

    if (input_type & FLAG_DINPUT_MAPPER_STANDARD) 
    {
   	static DIDEVICEOBJECTINSTANCEW instances[17];
 	memset(instances, 0, sizeof(instances));
  	*instance_count = 17;
        
        instances[index].guidType = GUID_XAxis;
        instances[index].dwOfs = DIJOFS_X;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"X Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_X;
        index++;
        
        instances[index].guidType = GUID_YAxis;
        instances[index].dwOfs = DIJOFS_Y;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Y Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Y;    
        index++;
        
        instances[index].guidType = GUID_ZAxis;
        instances[index].dwOfs = DIJOFS_Z;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Z Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Z;    
        index++;    

        instances[index].guidType = GUID_RzAxis;
        instances[index].dwOfs = DIJOFS_RZ;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Rz Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RZ;    
        index++;
        
        for (i = 0; i < 12; i++) 
        {
            instances[index].guidType = GUID_Button,
            instances[index].dwOfs = DIJOFS_BUTTON( i ),
            instances[index].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE( i ),
            swprintf( instances[index].tszName, MAX_PATH, L"Button %d", i );
            instances[index].wUsagePage = HID_USAGE_PAGE_BUTTON;
            instances[index].wUsage = i + 1;
            index++;
        }
        
        instances[index].guidType = GUID_POV;
        instances[index].dwOfs = DIJOFS_POV( 0 );
        instances[index].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 );
        swprintf( instances[index].tszName, MAX_PATH, L"POV" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_HATSWITCH;
        
        *out = instances;
    }
    else if (input_type & FLAG_DINPUT_MAPPER_XINPUT) 
    {
        static DIDEVICEOBJECTINSTANCEW instances[16];

        memset(instances, 0, sizeof(instances));
        *instance_count = 16;
        
        instances[index].guidType = GUID_XAxis;
        instances[index].dwOfs = DIJOFS_X;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"X Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_X;
        index++;
        
        instances[index].guidType = GUID_YAxis;
        instances[index].dwOfs = DIJOFS_Y;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Y Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Y;
        index++;
        
        instances[index].guidType = GUID_ZAxis;
        instances[index].dwOfs = DIJOFS_Z;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Z Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Z;
        index++;

        instances[index].guidType = GUID_RxAxis;
        instances[index].dwOfs = DIJOFS_RX;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Rx Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RX;
        index++;

        instances[index].guidType = GUID_RyAxis;
        instances[index].dwOfs = DIJOFS_RY;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION | DIDOI_FFACTUATOR;
        swprintf( instances[index].tszName, MAX_PATH, L"Ry Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RY;    
        index++;
        
        for (i = 0; i < 10; i++) 
        {
            instances[index].guidType = GUID_Button,
            instances[index].dwOfs = DIJOFS_BUTTON( i ),
            instances[index].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE( i ),
            swprintf( instances[index].tszName, MAX_PATH, L"Button %d", i );
            instances[index].wUsagePage = HID_USAGE_PAGE_BUTTON;
            instances[index].wUsage = i + 1;
            index++;
        }
        
        instances[index].guidType = GUID_POV;
        instances[index].dwOfs = DIJOFS_POV( 0 );
        instances[index].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 );
        swprintf( instances[index].tszName, MAX_PATH, L"POV" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_HATSWITCH;
        
        *out = instances;
    }
}

static HRESULT gamepad_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                     DWORD flags, enum_object_callback callback, void *context )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    int instance_count;
    DIDEVICEOBJECTINSTANCEW* instances;
    BOOL ret;
    DWORD i;
    
    get_device_objects( &instance_count, &instances );

    for (i = 0; i < instance_count; i++)
    {
        DIDEVICEOBJECTINSTANCEW *instance = instances + i;
        instance->dwSize = sizeof(DIDEVICEOBJECTINSTANCEW);
        instance->wReportId = 1;
        
        ret = try_enum_object( &impl->base, filter, flags, callback, i, instance, context );
        if (ret != DIENUM_CONTINUE) return DIENUM_STOP;
    }

    return DIENUM_CONTINUE;
}

static HRESULT gamepad_enum_effects(IDirectInputDevice8W *iface,
                                    LPDIENUMEFFECTSCALLBACKW cb, void *ref, DWORD dwEffType)
{
    DIEFFECTINFOW info;
    BOOL cont;

    if (!g_ff_supported) return DI_OK;

    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    info.guid = GUID_ConstantForce;
    info.dwEffType = DIEFT_CONSTANTFORCE;
    lstrcpynW(info.tszName, L"     Constant Force", MAX_PATH);
    cont = cb(&info, ref);
    if (!cont) return DI_OK;

    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    info.guid = GUID_Sine;
    info.dwEffType = DIEFT_PERIODIC;
    lstrcpynW(info.tszName, L"Sine", MAX_PATH);
    cb(&info, ref);

    return DI_OK;
}

static HRESULT WINAPI eff_QueryInterface(IDirectInputEffect *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectInputEffect)) {
        *ppv = iface; eff_AddRef(iface); return DI_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}

static ULONG WINAPI eff_AddRef(IDirectInputEffect *iface)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    return ++eff->ref;
}

static ULONG WINAPI eff_Release(IDirectInputEffect *iface)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    ULONG ref = --eff->ref;
    if (!ref) {
        send_rumble_request(0, 0, 100);
        free(eff);
    }
    return ref;
}

static HRESULT WINAPI eff_Initialize(IDirectInputEffect *iface, HINSTANCE a, DWORD b, REFGUID c)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    if (c) eff->guid = *c;
    return DI_OK;
}


static HRESULT WINAPI eff_GetEffectGuid(IDirectInputEffect *iface, LPGUID out)
{
    if (!out) return E_POINTER;
    *out = impl_from_IDirectInputEffect(iface)->guid; return DI_OK;
}

static HRESULT WINAPI eff_GetParameters(IDirectInputEffect *iface, LPDIEFFECT peff, DWORD flags)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    if (!peff) return E_POINTER;
    if (peff->dwSize < sizeof(*peff)) return DIERR_INVALIDPARAM;

    if (flags & DIEP_AXES) {
        peff->cAxes = 2;
        if (peff->rgdwAxes) { peff->rgdwAxes[0] = DIJOFS_X; peff->rgdwAxes[1] = DIJOFS_Y; }
    }
    if (flags & DIEP_DIRECTION) {
        if (peff->rglDirection) { peff->rglDirection[0] = 0; peff->rglDirection[1] = 0; }
    }
    if (flags & DIEP_GAIN)      peff->dwGain     = eff->gain;
    if (flags & DIEP_DURATION)  peff->dwDuration = eff->duration_ms ? eff->duration_ms * 1000 : INFINITE;
    /* typespecific: opțional */

    return DI_OK;
}

static void effect_apply_to_rumble(gamepad_effect *eff)
{
    LONG mag;
    WORD v, dur;

    mag = eff->magnitude;
    if (mag < 0) mag = -mag;
    if (mag > 10000) mag = 10000;

    v   = (WORD)((mag * 65535) / 10000);
    dur = (WORD)(eff->duration_ms ? eff->duration_ms : 200);
    if (dur > g_max_rumble_ms) dur = g_max_rumble_ms;

    send_rumble_request(v, v, dur);
}

static HRESULT WINAPI eff_SetParameters(IDirectInputEffect *iface,
        LPCDIEFFECT peff, DWORD dwFlags)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    if (!peff) return E_POINTER;

    /* Duration */
    if (peff->dwDuration == INFINITE) eff->duration_ms = g_max_rumble_ms;
    else                              eff->duration_ms = (DWORD)(peff->dwDuration / 1000);

    eff->gain = peff->dwGain ? (LONG)peff->dwGain : 10000;

    /* Type-specific params */
    if (IsEqualGUID(&eff->guid, &GUID_ConstantForce) &&
        peff->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE))
    {
        const DICONSTANTFORCE *cf = (const DICONSTANTFORCE*)peff->lpvTypeSpecificParams;
        eff->magnitude = cf->lMagnitude;
    }
    else if (IsEqualGUID(&eff->guid, &GUID_Sine) &&
             peff->cbTypeSpecificParams >= sizeof(DIPERIODIC))
    {
        const DIPERIODIC *pp = (const DIPERIODIC*)peff->lpvTypeSpecificParams;
        eff->magnitude = (LONG)pp->dwMagnitude;
        eff->period_ms = (LONG)(pp->dwPeriod / 1000);
    }

    /* fallback dacă magnitude e 0 */
    if (eff->magnitude == 0)
        eff->magnitude = eff->gain ? eff->gain : 6000;

    if (dwFlags & DIEP_START)
    {
        eff->running = TRUE;
        effect_apply_to_rumble(eff);
    }
    
    /* Dacă UI a setat un trigger, emulăm hardware-trigger: pornim imediat */
    if (peff->dwTriggerButton != DIEB_NOTRIGGER && !eff->running) {
        eff->running = TRUE;
        effect_apply_to_rumble(eff);
    }
    
    return DI_OK;
}

static HRESULT WINAPI eff_Start(IDirectInputEffect *iface, DWORD iterations, DWORD flags)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    if (eff->magnitude == 0) eff->magnitude = 5000; /* fallback dacă UI nu trimite magnitude */
    eff->running = TRUE;
    effect_apply_to_rumble(eff);
    return DI_OK;
}

static HRESULT WINAPI eff_Stop(IDirectInputEffect *iface)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    eff->running = FALSE; send_rumble_request(0, 0, 100); return DI_OK;
}

static HRESULT WINAPI eff_GetEffectStatus(IDirectInputEffect *iface, LPDWORD st)
{
    if (!st) return E_POINTER;
    *st = impl_from_IDirectInputEffect(iface)->running ? DIEGES_PLAYING : 0;
    return DI_OK;
}

static HRESULT WINAPI eff_Download(IDirectInputEffect *iface)
{
    gamepad_effect *eff = impl_from_IDirectInputEffect(iface);
    
    // Ensure effect is properly initialized before downloading
    if (eff->magnitude == 0) {
        eff->magnitude = 5000; // Default value
    }
    
    // Send initial rumble command to ensure device is ready
    send_rumble_request(0, 0, 10);
    
    return DI_OK;
}
static HRESULT WINAPI eff_Unload(IDirectInputEffect *iface)   { return DI_OK; }
static HRESULT WINAPI eff_Escape(IDirectInputEffect *iface, LPDIEFFESCAPE esc) { return DIERR_UNSUPPORTED; }
static HRESULT gamepad_get_property( IDirectInputDevice8W *iface, DWORD property,
                                     DIPROPHEADER *header, const DIDEVICEOBJECTINSTANCEW *instance )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    
    switch (property)
    {
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszProductName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_INSTANCENAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, impl->base.instance.tszInstanceName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_VIDPID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = MAKELONG( 0x045e, 0x028e );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_JOYSTICKID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = connected_gamepad_id;
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_GUIDANDPATH:
    {
        DIPROPGUIDANDPATH *value = (DIPROPGUIDANDPATH *)header;
        value->guidClass = GUID_DEVCLASS_HIDCLASS;
        lstrcpynW( value->wszPath, L"virtual#vid_045e&pid_028e&ig_00", MAX_PATH );
        return DI_OK;
    }
    }

    return DIERR_UNSUPPORTED;
}

/* Single global vtbl (not inside a function) */
static const IDirectInputEffectVtbl gamepad_effect_vtbl = {
    eff_QueryInterface, eff_AddRef, eff_Release, eff_Initialize,
    eff_GetEffectGuid,  eff_GetParameters, eff_SetParameters,
    eff_Start,          eff_Stop,          eff_GetEffectStatus,
    eff_Download,       eff_Unload,        eff_Escape
};

typedef BOOL (CALLBACK *LPDIENUMEFFECTSCALLBACKW)(LPCDIEFFECTINFOW, LPVOID);


HRESULT gamepad_create_device( struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out )
{
    static const DIPROPHEADER filter =
    {
        .dwSize = sizeof(filter),
        .dwHeaderSize = sizeof(filter),
        .dwHow = DIPH_DEVICE,
    };
    struct gamepad *impl;
    HRESULT hr;

    *out = NULL;
    /* Încarcă env la început */
    load_env_config();

    if (!g_device_enable) return DIERR_DEVICENOTREG;
    if (!IsEqualGUID( &GUID_Joystick, guid )) return DIERR_DEVICENOTREG;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    dinput_device_init( &impl->base, &gamepad_vtbl, guid, dinput );
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": struct gamepad*->base.crit");
    impl->base.read_event = CreateEventW( NULL, TRUE, FALSE, NULL );

    gamepad_enum_device( 0, 0, &impl->base.instance, dinput->dwVersion );
    impl->base.caps.dwDevType = impl->base.instance.dwDevType;
    impl->base.caps.dwFirmwareRevision = 100;
    impl->base.caps.dwHardwareRevision = 100;
    //impl->base.caps.dwButtons = (input_type & FLAG_DINPUT_MAPPER_STANDARD) ? 12 : 10;
    //impl->base.caps.dwAxes    = (input_type & FLAG_DINPUT_MAPPER_STANDARD) ?  4 :  5; /* X,Y,Z,Rz vs X,Y,Z,Rx,Ry */
    impl->base.caps.dwPOVs    = 1;
    if (g_ff_supported) impl->base.caps.dwFlags |= DIDC_FORCEFEEDBACK;
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    
    if (FAILED(hr = dinput_device_init_device_format( &impl->base.IDirectInputDevice8W_iface ))) goto failed;
    gamepad_enum_objects( &impl->base.IDirectInputDevice8W_iface, &filter, DIDFT_AXIS, init_object_properties, NULL );

    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;
    
failed:
    IDirectInputDevice_Release( &impl->base.IDirectInputDevice8W_iface );
    return hr;    
}

static void gamepad_destroy( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    if (impl->base.read_event) CloseHandle( impl->base.read_event );
    release_gamepad_request();
    close_server_socket(); /* aici e locul potrivit */
}

static HRESULT gamepad_get_effect_info(IDirectInputDevice8W *iface, DIEFFECTINFOW *info, const GUID *guid)
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W(iface);
    if (guid) impl->requested_guid = *guid; /* remember for create_effect */

    TRACE("get_effect_info %s\n", debugstr_guid(guid));

    ZeroMemory(info, sizeof(*info));
    info->dwSize = sizeof(*info);
    info->guid = *guid;

    if (IsEqualGUID(guid, &GUID_ConstantForce))
    {
        info->dwEffType        = DIEFT_CONSTANTFORCE;
        info->dwStaticParams   = DIEP_GAIN | DIEP_DURATION | DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS;
        info->dwDynamicParams  = DIEP_GAIN | DIEP_TYPESPECIFICPARAMS;
        lstrcpynW(info->tszName, L"     Constant Force", MAX_PATH);
        return DI_OK;
    }
    if (IsEqualGUID(guid, &GUID_Sine))
    {
        info->dwEffType        = DIEFT_PERIODIC;
        info->dwStaticParams   = DIEP_GAIN | DIEP_DURATION | DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS;
        info->dwDynamicParams  = DIEP_GAIN | DIEP_TYPESPECIFICPARAMS;
        lstrcpynW(info->tszName, L"     Sine", MAX_PATH);
        return DI_OK;
    }

    return DIERR_NOTFOUND;
}


static HRESULT gamepad_send_ff_command(IDirectInputDevice8W *iface, DWORD cmd, BOOL unacquire)
{
    switch (cmd)
    {
    case DISFFC_SETACTUATORSON:
        if (connected_gamepad_id) send_rumble_request(0, 0, 50); /* ACK simbolic */
        break;
    case DISFFC_SETACTUATORSOFF:
    case DISFFC_STOPALL:
    case DISFFC_RESET:
    case DISFFC_PAUSE:
        send_rumble_request(0, 0, 100);
        break;
    case DISFFC_CONTINUE:
        /* no-op; efectele tale pornesc din Start/SetParameters */
        break;
    default:
        break;
    }
    return DI_OK;
}

static HRESULT gamepad_send_device_gain( IDirectInputDevice8W *iface, LONG device_gain )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    impl->base.device_gain = device_gain;
    return DI_OK;
}
static HRESULT gamepad_enum_created_effect_objects( IDirectInputDevice8W *iface,
    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ctx, DWORD flags )
{
    /* Nu menținem o listă; dacă vrei, poți ține un list<gamepad_effect*> și să-i dai callback. */
    return DI_OK;
}

static const struct dinput_device_vtbl gamepad_vtbl =
{
    /* destroy */                   gamepad_destroy,
    /* poll */                      gamepad_poll,      // <<< aici
    /* read */                      gamepad_read,
    /* acquire */                   gamepad_acquire,
    /* unacquire */                 gamepad_unacquire,
    /* enum_objects */              gamepad_enum_objects,
    /* get_property */              gamepad_get_property,
    /* get_effect_info */           gamepad_get_effect_info,
    /* create_effect */             gamepad_create_effect,
    /* send_force_feedback_command */ gamepad_send_ff_command,
    /* send_device_gain */          gamepad_send_device_gain,
    /* enum_created_effect_objects */ gamepad_enum_created_effect_objects,
};
