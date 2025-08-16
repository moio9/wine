/*  DirectInput Fake Gamepad backend (no external bridge)
 *
 *  Drop-in pentru Wine dinput: definește gamepad_enum_device() și gamepad_create_device()
 *  și simulează evenimente în gamepad_read().
 */

#include <string.h>
#include <math.h>
#include <stdarg.h> 
#include <string.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winerror.h"
#include "dinput.h"
#include "hidusage.h"
#include "devguid.h"

#include "dinput_private.h"
#include "device_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

#define IDX_BUTTON_A 0
#define IDX_BUTTON_B 1
#define IDX_BUTTON_X 2
#define IDX_BUTTON_Y 3
#define IDX_BUTTON_L1 4
#define IDX_BUTTON_R1 5
#define IDX_BUTTON_SELECT 6
#define IDX_BUTTON_START 7
#define IDX_BUTTON_L3 8
#define IDX_BUTTON_R3 9

struct gamepad_state
{
    SHORT buttons;
    CHAR  dpad;          /* -1 neutral */
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
    UCHAR thumb_lz;
    UCHAR thumb_rz;
};

struct gamepad
{
    struct dinput_device base;
    struct gamepad_state state;

    DWORD last_tick_ms;
    DWORD last_toggle_div; /* now/250 pentru toggling buton */
};

static const struct dinput_device_vtbl gamepad_vtbl;

/* ---- helpers ---------------------------------------------------------------- */

static inline struct gamepad *impl_from_IDirectInputDevice8W( IDirectInputDevice8W *iface )
{
    return CONTAINING_RECORD( CONTAINING_RECORD( iface, struct dinput_device, IDirectInputDevice8W_iface ),
                              struct gamepad, base );
}

static LONG scale_axis_value( LONG value, struct object_properties *p )
{
    LONG log_min = -32768, log_max = 32767;
    LONG phy_min = 0, phy_max = 65535;
    LONG log_ctr = 0, phy_ctr = 0;

    if (phy_min == 0) phy_ctr = phy_max >> 1; else phy_ctr = (LONG)llround((phy_min + phy_max) / 2.0);
    if (log_min == 0) log_ctr = log_max >> 1; else log_ctr = (LONG)llround((log_min + log_max) / 2.0);

    value -= log_ctr;
    if (value <= 0)
    {
        LONG lo = MulDiv( log_min - log_ctr, p->deadzone, 10000 );
        LONG hi = MulDiv( log_min - log_ctr, p->saturation, 10000 );
        if (value <= lo) return phy_min;
        if (value >= hi) return phy_ctr;
        return phy_min + MulDiv( value - lo, phy_ctr - phy_min, hi - lo );
    }
    else
    {
        LONG lo = MulDiv( log_max - log_ctr, p->deadzone, 10000 );
        LONG hi = MulDiv( log_max - log_ctr, p->saturation, 10000 );
        if (value <= lo) return phy_ctr;
        if (value >= hi) return phy_max;
        return phy_ctr + MulDiv( value - lo, phy_max - phy_ctr, hi - lo );
    }
}

static BOOL init_object_properties( struct dinput_device *device, UINT index, struct hid_value_caps *caps,
                                    const DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    struct object_properties *pr = device->object_properties + index;
    pr->logical_min = -32768;
    pr->logical_max = 32767;
    pr->range_min   = 0;
    pr->range_max   = 65535;
    pr->saturation  = 10000;
    pr->granularity = 1;
    pr->deadzone    = 0;
    return DIENUM_CONTINUE;
}

/* listă de obiecte fixă (XInput-like): LX, LY, Z (trig comb.), RX, RY + 10 butoane + 1 POV */
static void get_device_objects( int *count, DIDEVICEOBJECTINSTANCEW **out )
{
    static DIDEVICEOBJECTINSTANCEW inst[16];
    int i, idx = 0;

    memset( inst, 0, sizeof(inst) );

    #define ADD_AXIS(guid, ofs, makeinst, name, usage) do { \
        inst[idx].guidType = guid; \
        inst[idx].dwOfs = ofs; \
        inst[idx].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(makeinst); \
        inst[idx].dwFlags = DIDOI_ASPECTPOSITION; \
        swprintf( inst[idx].tszName, MAX_PATH, L##name ); \
        inst[idx].wUsagePage = HID_USAGE_PAGE_GENERIC; \
        inst[idx].wUsage = usage; \
        idx++; \
    } while(0)

    ADD_AXIS( GUID_XAxis, DIJOFS_X,  0, "X Axis",  HID_USAGE_GENERIC_X );
    ADD_AXIS( GUID_YAxis, DIJOFS_Y,  1, "Y Axis",  HID_USAGE_GENERIC_Y );
    ADD_AXIS( GUID_ZAxis, DIJOFS_Z,  2, "Z Axis",  HID_USAGE_GENERIC_Z );     /* triggers combinate */
    ADD_AXIS( GUID_RxAxis,DIJOFS_RX, 3, "Rx Axis", HID_USAGE_GENERIC_RX );
    ADD_AXIS( GUID_RyAxis,DIJOFS_RY, 4, "Ry Axis", HID_USAGE_GENERIC_RY );

    for (i = 0; i < 10; ++i)
    {
        inst[idx].guidType = GUID_Button;
        inst[idx].dwOfs = DIJOFS_BUTTON(i);
        inst[idx].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i);
        swprintf( inst[idx].tszName, MAX_PATH, L"Button %d", i );
        inst[idx].wUsagePage = HID_USAGE_PAGE_BUTTON;
        inst[idx].wUsage = i + 1;
        idx++;
    }

    inst[idx].guidType = GUID_POV;
    inst[idx].dwOfs = DIJOFS_POV(0);
    inst[idx].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
    swprintf( inst[idx].tszName, MAX_PATH, L"POV" );
    inst[idx].wUsagePage = HID_USAGE_PAGE_GENERIC;
    inst[idx].wUsage = HID_USAGE_GENERIC_HATSWITCH;
    idx++;

    *count = idx;
    *out = inst;
}

static BOOL try_enum_object( struct dinput_device *impl, const DIPROPHEADER *filter, DWORD flags, enum_object_callback cb,
                             UINT index, DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE(instance->dwType))) return DIENUM_CONTINUE;

    switch (filter->dwHow)
    {
    case DIPH_DEVICE:   return cb( impl, index, NULL, instance, data );
    case DIPH_BYOFFSET: if (filter->dwObj != instance->dwOfs) return DIENUM_CONTINUE; return cb( impl, index, NULL, instance, data );
    case DIPH_BYID:     if ((filter->dwObj & 0x00ffffff) != (instance->dwType & 0x00ffffff)) return DIENUM_CONTINUE; return cb( impl, index, NULL, instance, data );
    }
    return DIENUM_CONTINUE;
}

/* ---- vtbl funcs ------------------------------------------------------------ */

static void gamepad_release( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    CloseHandle( impl->base.read_event );
}

static HRESULT gamepad_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                     DWORD flags, enum_object_callback cb, void *ctx )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    DIDEVICEOBJECTINSTANCEW *arr;
    int n, i;

    get_device_objects( &n, &arr );
    for (i = 0; i < n; ++i)
    {
        DIDEVICEOBJECTINSTANCEW *ins = &arr[i];
        ins->dwSize = sizeof(*ins);
        ins->wReportId = 1;
        if (!try_enum_object( &impl->base, filter, flags, cb, i, ins, ctx )) return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

static HRESULT gamepad_get_property( IDirectInputDevice8W *iface, DWORD prop,
                                     DIPROPHEADER *hdr, const DIDEVICEOBJECTINSTANCEW *instance )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );

    switch (prop)
    {
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
        lstrcpynW( ((DIPROPSTRING*)hdr)->wsz, impl->base.instance.tszProductName, MAX_PATH ); return DI_OK;
    case (DWORD_PTR)DIPROP_INSTANCENAME:
        lstrcpynW( ((DIPROPSTRING*)hdr)->wsz, impl->base.instance.tszInstanceName, MAX_PATH ); return DI_OK;
    case (DWORD_PTR)DIPROP_VIDPID:
        ((DIPROPDWORD*)hdr)->dwData = MAKELONG(0x045e, 0x028e); return DI_OK;
    case (DWORD_PTR)DIPROP_JOYSTICKID:
        ((DIPROPDWORD*)hdr)->dwData = 1; return DI_OK;
    case (DWORD_PTR)DIPROP_GUIDANDPATH:
        ((DIPROPGUIDANDPATH*)hdr)->guidClass = GUID_DEVCLASS_HIDCLASS;
        lstrcpynW( ((DIPROPGUIDANDPATH*)hdr)->wszPath, L"virtual#vid_045e&pid_028e&ig_00", MAX_PATH );
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

/* Simulare input: toggling A și mișcare LX, ~60Hz */
static HRESULT gamepad_read( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    DIJOYSTATE *st = (DIJOYSTATE *)impl->base.device_state;

    DWORD now = GetTickCount();
    if (now - impl->last_tick_ms < 16) return DI_OK; /* ~60Hz */
    impl->last_tick_ms = now;

    /* toggle A la fiecare 250ms */
    DWORD div = now / 250;
    if (div != impl->last_toggle_div)
    {
        impl->last_toggle_div = div;
        impl->state.buttons ^= (1 << IDX_BUTTON_A);
    }

    /* “animăm” LX în range [-32768..32767] */
    LONG lx = (LONG)((INT)((now / 5) % 65536) - 32768);
    LONG ly = 0, rx = 0, ry = 0;
    UCHAR lz = 0, rz = 0;
    CHAR dpad = (CHAR)-1; /* neutral */

    /* emitem evenimente doar dacă s-a schimbat ceva */
    DWORD time = GetCurrentTime();
    DWORD seq  = impl->base.dinput->evsequence++;

    /* AXE */
    int idx;
    if (impl->state.thumb_lx != (SHORT)lx)
    {
        impl->state.thumb_lx = (SHORT)lx;
        idx = dinput_device_object_index_from_id( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0) );
        st->lX = scale_axis_value( lx, impl->base.object_properties + idx );
        queue_event( iface, idx, st->lX, time, seq );
    }

    /* Buton A */
    {
        int a_on = !!(impl->state.buttons & (1 << IDX_BUTTON_A));
        st->rgbButtons[IDX_BUTTON_A] = a_on ? 0x80 : 0x00;
        idx = dinput_device_object_index_from_id( iface, DIDFT_BUTTON | DIDFT_MAKEINSTANCE( IDX_BUTTON_A ) );
        queue_event( iface, idx, st->rgbButtons[IDX_BUTTON_A], time, seq );
    }

    /* POV neutral */
    {
        st->rgdwPOV[0] = -1;
        idx = dinput_device_object_index_from_id( iface, DIDFT_POV | DIDFT_MAKEINSTANCE(0) );
        queue_event( iface, idx, st->rgdwPOV[0], time, seq );
    }

    if (impl->base.hEvent) SetEvent( impl->base.hEvent );
    return DI_OK;
}

static HRESULT gamepad_acquire( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    impl->last_tick_ms = 0;
    SetEvent( impl->base.read_event );
    return DI_OK;
}

static HRESULT gamepad_unacquire( IDirectInputDevice8W *iface )
{
    struct gamepad *impl = impl_from_IDirectInputDevice8W( iface );
    WaitForSingleObject( impl->base.read_event, INFINITE );
    return DI_OK;
}

/* ---- API cerute de dinput.c ------------------------------------------------ */

HRESULT gamepad_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *inst, DWORD version )
{
    DWORD size = inst->dwSize;
    memset( inst, 0, size );
    inst->dwSize = size;

    inst->guidInstance = GUID_Joystick;
    inst->guidProduct  = GUID_Joystick;
    inst->guidProduct.Data1 = MAKELONG(0x045e, 0x028e);

    if (version >= 0x0800) inst->dwDevType = DIDEVTYPE_HID | DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    else                   inst->dwDevType = DIDEVTYPE_HID | DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);

    inst->wUsagePage = HID_USAGE_PAGE_GENERIC;
    inst->wUsage     = HID_USAGE_GENERIC_GAMEPAD;
    lstrcpynW( inst->tszInstanceName, L"Fake Gamepad", MAX_PATH );
    lstrcpynW( inst->tszProductName,  L"Fake Gamepad", MAX_PATH );

    return DI_OK;
}

HRESULT gamepad_create_device( struct dinput *di, const GUID *guid, IDirectInputDevice8W **out )
{
    static const DIPROPHEADER filter =
    {
        .dwSize = sizeof(DIPROPHEADER),
        .dwHeaderSize = sizeof(DIPROPHEADER),
        .dwHow = DIPH_DEVICE,
    };

    struct gamepad *impl;
    HRESULT hr;

    *out = NULL;
    if (!IsEqualGUID( &GUID_Joystick, guid )) return DIERR_DEVICENOTREG;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    dinput_device_init( &impl->base, &gamepad_vtbl, guid, di );
    impl->base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": struct gamepad*->base.crit");
    impl->base.read_event = CreateEventW( NULL, TRUE, FALSE, NULL );

    /* umplem caps/instance */
    if (FAILED( gamepad_enum_device( 0, 0, &impl->base.instance, di->dwVersion ) ))
    {
        IDirectInputDevice_Release( &impl->base.IDirectInputDevice8W_iface );
        return DIERR_DEVICENOTREG;
    }
    impl->base.caps.dwDevType = impl->base.instance.dwDevType;
    impl->base.caps.dwFirmwareRevision = 1;
    impl->base.caps.dwHardwareRevision = 1;
    impl->base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;

    /* format + proprietăți */
    if (FAILED( hr = dinput_device_init_device_format( &impl->base.IDirectInputDevice8W_iface )))
    {
        IDirectInputDevice_Release( &impl->base.IDirectInputDevice8W_iface );
        return hr;
    }
    gamepad_enum_objects( &impl->base.IDirectInputDevice8W_iface, &filter, DIDFT_AXIS, init_object_properties, NULL );

    /* stare inițială */
    impl->state.dpad = -1;

    *out = &impl->base.IDirectInputDevice8W_iface;
    return DI_OK;
}

static const struct dinput_device_vtbl gamepad_vtbl =
{
    gamepad_release,         /* release */
    NULL,                    /* set_property */
    gamepad_read,            /* read */
    gamepad_acquire,         /* acquire */
    gamepad_unacquire,       /* unacquire */
    gamepad_enum_objects,    /* enum_objects */
    gamepad_get_property,    /* get_property */
    NULL, NULL, NULL, NULL, NULL
};

