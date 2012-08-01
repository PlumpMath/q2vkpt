/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//
// vid_win.c
//

#include "client.h"

#define WINDOW_CLASS_NAME   "Quake 2 Pro"

// mode_changed flags
#define MODE_SIZE       (1 << 0)
#define MODE_POS        (1 << 1)
#define MODE_STYLE      (1 << 2)
#define MODE_REPOSITION (1 << 3)

win_state_t     win;

static cvar_t   *vid_flip_on_switch;
static cvar_t   *vid_hwgamma;
static cvar_t   *win_noalttab;
static cvar_t   *win_disablewinkey;
static cvar_t   *win_noresize;
static cvar_t   *win_notitle;
static cvar_t   *win_alwaysontop;
static cvar_t   *win_xpfix;
static cvar_t   *win_rawmouse;

static qboolean Win_InitMouse(void);
static void     Win_ClipCursor(void);

/*
===============================================================================

COMMON WIN32 VIDEO RELATED ROUTINES

===============================================================================
*/

static void Win_SetPosition(void)
{
    RECT            r;
    LONG            style;
    int             x, y, w, h;
    HWND            after;

    // get previous window style
    style = GetWindowLong(win.wnd, GWL_STYLE);
    style &= ~(WS_OVERLAPPEDWINDOW | WS_POPUP | WS_DLGFRAME);

    // set new style bits
    if (win.flags & QVF_FULLSCREEN) {
        after = HWND_TOPMOST;
        style |= WS_POPUP;
    } else {
        if (win_alwaysontop->integer) {
            after = HWND_TOPMOST;
        } else {
            after = HWND_NOTOPMOST;
        }
        style |= WS_OVERLAPPED;
        if (win_notitle->integer) {
            if (win_noresize->integer) {
                style |= WS_DLGFRAME;
            } else {
                style |= WS_THICKFRAME;
            }
        } else {
            style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            if (!win_noresize->integer) {
                style |= WS_THICKFRAME;
            }
        }
    }

    // adjust for non-client area
    r.left = 0;
    r.top = 0;
    r.right = win.rc.width;
    r.bottom = win.rc.height;

    AdjustWindowRect(&r, style, FALSE);

    // figure out position
    x = win.rc.x;
    y = win.rc.y;
    w = r.right - r.left;
    h = r.bottom - r.top;

    // set new window style and position
    SetWindowLong(win.wnd, GWL_STYLE, style);
    SetWindowPos(win.wnd, after, x, y, w, h, SWP_FRAMECHANGED);
    ShowWindow(win.wnd, SW_SHOW);
    SetForegroundWindow(win.wnd);
    SetFocus(win.wnd);

    if (win.mouse.grabbed == IN_GRAB) {
        Win_ClipCursor();
    }
}

/*
============
Win_ModeChanged
============
*/
void Win_ModeChanged(void)
{
#if USE_REF == REF_SOFT
    void SWimp_ModeChanged(void);
    SWimp_ModeChanged();
#endif
    R_ModeChanged(win.rc.width, win.rc.height, win.flags,
                  win.pitch, win.buffer);
    SCR_ModeChanged();
}

/*
============
VID_GetDefaultModeList
============
*/

typedef struct {
    int width, height;
    int freq, depth;
    qboolean desktop;
} vidmode_t;

#define MAX_MODES 256

static int modecmp(const void *p1, const void *p2)
{
    const vidmode_t *m1 = (const vidmode_t *)p1;
    const vidmode_t *m2 = (const vidmode_t *)p2;

    // desktop resolution is always first
    if (m1->desktop)
        return -1;

    return m2->width * m2->height - m1->width * m1->height;
}

// should have at least width and height
#define DM_USABLE(dm) \
    (((dm)->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT))

static void process_mode(vidmode_t *modes, int *num_modes, const DEVMODE *dm, const DEVMODE *desktop)
{
    int freq = 0, depth = 0;
    vidmode_t *m;
    int i;

    if (!DM_USABLE(dm))
        return;

    // sanity check width and height
    if (dm->dmPelsWidth < 64 || dm->dmPelsHeight < 64)
        return;
    if (dm->dmPelsWidth > 8192 || dm->dmPelsHeight > 8192)
        return;

    if (dm->dmFields & DM_DISPLAYFLAGS) {
        if (dm->dmDisplayFlags & (DM_GRAYSCALE | DM_INTERLACED))
            return;
    }

    if (dm->dmFields & DM_DISPLAYFREQUENCY) {
        if (dm->dmDisplayFrequency <= 1 || dm->dmDisplayFrequency > 1000)
            return;
        freq = dm->dmDisplayFrequency;
    }

    if (dm->dmFields & DM_BITSPERPEL) {
        if (dm->dmBitsPerPel < 8 || dm->dmBitsPerPel > 32)
            return;
        // completely ignore non-desktop bit depths for now
        if ((desktop->dmFields & DM_BITSPERPEL) && dm->dmBitsPerPel != desktop->dmBitsPerPel)
            return;
        depth = dm->dmBitsPerPel;
    }

    // see if we already have this resolution
    for (i = 0; i < *num_modes; i++) {
        m = &modes[i];
        if (m->width == dm->dmPelsWidth && m->height == dm->dmPelsHeight) {
            if (freq > m->freq)
                m->freq = freq;
            if (depth > m->depth)
                m->depth = depth;
            return;
        }
    }

    // add new resolution
    if (*num_modes >= MAX_MODES)
        return;

    m = &modes[(*num_modes)++];
    m->width = dm->dmPelsWidth;
    m->height = dm->dmPelsHeight;
    m->freq = freq;
    m->depth = depth;
    m->desktop = qfalse;
    if (dm->dmPelsWidth == desktop->dmPelsWidth && dm->dmPelsHeight == desktop->dmPelsHeight)
        m->desktop = qtrue;
}

char *VID_GetDefaultModeList(void)
{
    DEVMODE desktop, dm;
    vidmode_t modes[MAX_MODES], *m;
    int i, num_modes;
    size_t size, len;
    char *buf;

    memset(&desktop, 0, sizeof(desktop));
    desktop.dmSize = sizeof(desktop);

    if (!EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS, &desktop))
        return Z_CopyString(VID_MODELIST);

    num_modes = 0;
    for (i = 0; i < MAX_MODES * 4; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);

        if (!EnumDisplaySettings(NULL, i, &dm))
            break;

        process_mode(modes, &num_modes, &dm, &desktop);
    }

    if (!num_modes)
        return Z_CopyString(VID_MODELIST);

    qsort(modes, num_modes, sizeof(modes[0]), modecmp);

    size = num_modes * 20 + 1;
    buf = Z_Malloc(size);

    len = 0;
    for (i = 0; i < num_modes; i++) {
        m = &modes[i];

        len += Q_scnprintf(buf + len, size - len, "%dx%d", m->width, m->height);

        if (m->freq && (desktop.dmFields & DM_DISPLAYFREQUENCY) && m->freq != desktop.dmDisplayFrequency)
            len += Q_scnprintf(buf + len, size - len, "@%d", m->freq);

        if (m->depth && (desktop.dmFields & DM_BITSPERPEL) && m->depth != desktop.dmBitsPerPel)
            len += Q_scnprintf(buf + len, size - len, ":%d", m->depth);

        if (len < size - 1 && i < num_modes - 1)
            buf[len++] = ' ';
    }
    buf[len] = 0;

    return buf;
}

// use desktop frequency by default if using desktop resolution
static int get_desktop_frequency(const DEVMODE *desktop)
{
    const int mask = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    if ((desktop->dmFields & mask) != mask)
        return 0;
    if (desktop->dmPelsWidth != win.rc.width)
        return 0;
    if (desktop->dmPelsHeight != win.rc.height)
        return 0;

    return desktop->dmDisplayFrequency;
}

static qboolean set_fullscreen_mode(void)
{
    DEVMODE desktop, dm;
    LONG ret;
    int freq, depth;

    memset(&desktop, 0, sizeof(desktop));
    desktop.dmSize = sizeof(desktop);

    EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS, &desktop);

    // parse vid_modelist specification
    if (VID_GetFullscreen(&win.rc, &freq, &depth)) {
        Com_DPrintf("...setting fullscreen mode: %dx%d\n",
                    win.rc.width, win.rc.height);
    } else if (DM_USABLE(&desktop)) {
        win.rc.width = desktop.dmPelsWidth;
        win.rc.height = desktop.dmPelsHeight;
        Com_DPrintf("...falling back to desktop mode: %dx%d\n",
                    win.rc.width, win.rc.height);
    } else {
        Com_DPrintf("...falling back to default mode: %dx%d\n",
                    win.rc.width, win.rc.height);
    }

    memset(&dm, 0, sizeof(dm));
    dm.dmSize       = sizeof(dm);
    dm.dmPelsWidth  = win.rc.width;
    dm.dmPelsHeight = win.rc.height;
    dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

    if (freq) {
        dm.dmDisplayFrequency = freq;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
        Com_DPrintf("...using display frequency of %d\n", freq);
    } else {
        freq = get_desktop_frequency(&desktop);
        if (freq > 1) {
            dm.dmDisplayFrequency = freq;
            dm.dmFields |= DM_DISPLAYFREQUENCY;
            Com_DPrintf("...using desktop display frequency of %d\n", freq);
        }
    }

    if (depth) {
        dm.dmBitsPerPel = depth;
        dm.dmFields |= DM_BITSPERPEL;
        Com_DPrintf("...using bitdepth of %d\n", depth);
    } else if (desktop.dmFields & DM_BITSPERPEL) {
        dm.dmBitsPerPel = desktop.dmBitsPerPel;
        dm.dmFields |= DM_BITSPERPEL;
        Com_DPrintf("...using desktop bitdepth of %lu\n", desktop.dmBitsPerPel);
    }

    Com_DPrintf("...calling CDS: ");
    ret = ChangeDisplaySettings(&dm, CDS_FULLSCREEN);
    if (ret != DISP_CHANGE_SUCCESSFUL) {
        Com_DPrintf("failed with error %ld\n", ret);
        return qfalse;
    }
    Com_DPrintf("ok\n");

    win.dm = dm;
    win.flags |= QVF_FULLSCREEN;
    Win_SetPosition();
    win.mode_changed = 0;

    return qtrue;
}

/*
============
Win_SetMode
============
*/
void Win_SetMode(void)
{
    // set full screen mode if requested
    if (vid_fullscreen->integer > 0) {
        if (set_fullscreen_mode()) {
            return;
        }

        Cvar_Reset(vid_fullscreen);
        Com_Printf("Full screen mode %dx%d failed.\n",
                   win.rc.width, win.rc.height);
        // fall back to windowed mode
    }

    ChangeDisplaySettings(NULL, 0);

    // parse vid_geometry specification
    VID_GetGeometry(&win.rc);

    // align client area
    win.rc.width &= ~7;
    win.rc.height &= ~1;

    // don't allow too small size
    if (win.rc.width < 320) win.rc.width = 320;
    if (win.rc.height < 240) win.rc.height = 240;

    Com_DPrintf("...setting windowed mode: %dx%d%+d%+d\n",
                win.rc.width, win.rc.height, win.rc.x, win.rc.y);

    memset(&win.dm, 0, sizeof(win.dm));
    win.flags &= ~QVF_FULLSCREEN;
    Win_SetPosition();
    win.mode_changed = 0;

    // set vid_geometry back
    VID_SetGeometry(&win.rc);
}

/*
============
VID_UpdateGamma
============
*/
void VID_UpdateGamma(const byte *table)
{
    WORD v;
    int i;

    if (win.flags & QVF_GAMMARAMP) {
        for (i = 0; i < 256; i++) {
            v = table[i] << 8;
            win.gamma_cust[0][i] = v;
            win.gamma_cust[1][i] = v;
            win.gamma_cust[2][i] = v;
        }

        SetDeviceGammaRamp(win.dc, win.gamma_cust);
    }
}

static void Win_DisableAltTab(void)
{
    if (!win.alttab_disabled) {
        RegisterHotKey(0, 0, MOD_ALT, VK_TAB);
        RegisterHotKey(0, 1, MOD_ALT, VK_RETURN);
        win.alttab_disabled = qtrue;
    }
}

static void Win_EnableAltTab(void)
{
    if (win.alttab_disabled) {
        UnregisterHotKey(0, 0);
        UnregisterHotKey(0, 1);
        win.alttab_disabled = qfalse;
    }
}

static void win_noalttab_changed(cvar_t *self)
{
    if (self->integer) {
        Win_DisableAltTab();
    } else {
        Win_EnableAltTab();
    }
}

static void Win_Activate(WPARAM wParam)
{
    active_t active;

    if (HIWORD(wParam)) {
        // we don't want to act like we're active if we're minimized
        active = ACT_MINIMIZED;
    } else {
        if (LOWORD(wParam)) {
            active = ACT_ACTIVATED;
        } else {
            active = ACT_RESTORED;
        }
    }

    CL_Activate(active);

    if (win_noalttab->integer) {
        if (active == ACT_ACTIVATED) {
            Win_EnableAltTab();
        } else {
            Win_DisableAltTab();
        }
    }

    if (win.flags & QVF_GAMMARAMP) {
        if (active == ACT_ACTIVATED) {
            SetDeviceGammaRamp(win.dc, win.gamma_cust);
        } else {
            SetDeviceGammaRamp(win.dc, win.gamma_orig);
        }
    }

    if (win.flags & QVF_FULLSCREEN) {
        if (active == ACT_ACTIVATED) {
            ShowWindow(win.wnd, SW_RESTORE);
        } else {
            ShowWindow(win.wnd, SW_MINIMIZE);
        }

        if (vid_flip_on_switch->integer) {
            if (active == ACT_ACTIVATED) {
                ChangeDisplaySettings(&win.dm, CDS_FULLSCREEN);
            } else {
                ChangeDisplaySettings(NULL, 0);
            }
        }
    }

    if (active == ACT_ACTIVATED) {
        SetForegroundWindow(win.wnd);
    }
}

STATIC LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    PKBDLLHOOKSTRUCT kb = (PKBDLLHOOKSTRUCT)lParam;
    unsigned key;

    if (nCode != HC_ACTION) {
        goto ignore;
    }

    switch (kb->vkCode) {
    case VK_LWIN:
        key = K_LWINKEY;
        break;
    case VK_RWIN:
        key = K_RWINKEY;
        break;
    default:
        goto ignore;
    }

    switch (wParam) {
    case WM_KEYDOWN:
        Key_Event(key, qtrue, kb->time);
        return TRUE;
    case WM_KEYUP:
        Key_Event(key, qfalse, kb->time);
        return TRUE;
    default:
        break;
    }

ignore:
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void win_disablewinkey_changed(cvar_t *self)
{
    if (self->integer) {
        win.kbdHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hGlobalInstance, 0);
        if (!win.kbdHook) {
            Com_EPrintf("Couldn't set low-level keyboard hook, error %#lX\n", GetLastError());
            Cvar_Set("win_disablewinkey", "0");
        }
    } else {
        if (win.kbdHook) {
            UnhookWindowsHookEx(win.kbdHook);
            win.kbdHook = NULL;
        }
    }
}

static const byte scantokey[128] = {
//  0           1           2           3               4           5               6           7
//  8           9           A           B               C           D               E           F
    0,          K_ESCAPE,   '1',        '2',            '3',        '4',            '5',         '6',
    '7',        '8',        '9',        '0',            '-',        '=',            K_BACKSPACE,  K_TAB,    // 0
    'q',        'w',        'e',        'r',            't',        'y',            'u',         'i',
    'o',        'p',        '[',        ']',            K_ENTER,    K_CTRL,         'a',         's',       // 1
    'd',        'f',        'g',        'h',            'j',        'k',            'l',         ';',
    '\'' ,      '`',        K_LSHIFT,   '\\',           'z',        'x',            'c',         'v',       // 2
    'b',        'n',        'm',        ',',            '.',        '/',            K_RSHIFT,    K_KP_MULTIPLY,
    K_ALT,      K_SPACE,    K_CAPSLOCK, K_F1,           K_F2,       K_F3,           K_F4,        K_F5,      // 3
    K_F6,       K_F7,       K_F8,       K_F9,           K_F10,      K_PAUSE,        K_SCROLLOCK, K_HOME,
    K_UPARROW,  K_PGUP,     K_KP_MINUS, K_LEFTARROW,    K_KP_5,     K_RIGHTARROW,   K_KP_PLUS,   K_END,     // 4
    K_DOWNARROW,K_PGDN,     K_INS,      K_DEL,          0,          0,              0,           K_F11,
    K_F12,      0,          0,          K_LWINKEY,      K_RWINKEY,  K_MENU,         0,           0,         // 5
    0,          0,          0,          0,              0,          0,              0,           0,
    0,          0,          0,          0,              0,          0,              0,           0,         // 6
    0,          0,          0,          0,              0,          0,              0,           0,
    0,          0,          0,          0,              0,          0,              0,           0          // 7
};

// Map from windows to quake keynums
static void legacy_key_event(WPARAM wParam, LPARAM lParam, qboolean down)
{
    unsigned scancode = (lParam >> 16) & 255;
    unsigned extended = (lParam >> 24) & 1;
    unsigned result;

    if (scancode > 127) {
        return;
    }

    result = scantokey[scancode];
    if (!result) {
        Com_DPrintf("%s: unknown scancode: %u\n", __func__, scancode);
        return;
    }

    if (!extended) {
        switch (result) {
        case K_HOME:
            result = K_KP_HOME;
            break;
        case K_UPARROW:
            result = K_KP_UPARROW;
            break;
        case K_PGUP:
            result = K_KP_PGUP;
            break;
        case K_LEFTARROW:
            result = K_KP_LEFTARROW;
            break;
        case K_RIGHTARROW:
            result = K_KP_RIGHTARROW;
            break;
        case K_END:
            result = K_KP_END;
            break;
        case K_DOWNARROW:
            result = K_KP_DOWNARROW;
            break;
        case K_PGDN:
            result = K_KP_PGDN;
            break;
        case K_INS:
            result = K_KP_INS;
            break;
        case K_DEL:
            result = K_KP_DEL;
            break;
        case K_LSHIFT:
            Key_Event(K_SHIFT, down, win.lastMsgTime);
            Key_Event(K_LSHIFT, down, win.lastMsgTime);
            return;
        case K_RSHIFT:
            Key_Event(K_SHIFT, down, win.lastMsgTime);
            Key_Event(K_RSHIFT, down, win.lastMsgTime);
            return;
        case K_ALT:
            Key_Event(K_ALT, down, win.lastMsgTime);
            Key_Event(K_LALT, down, win.lastMsgTime);
            return;
        case K_CTRL:
            Key_Event(K_CTRL, down, win.lastMsgTime);
            Key_Event(K_LCTRL, down, win.lastMsgTime);
            return;
        }
    } else {
        switch (result) {
        case K_ENTER:
            result = K_KP_ENTER;
            break;
        case '/':
            result = K_KP_SLASH;
            break;
        case K_PAUSE:
            result = K_NUMLOCK;
            break;
        case K_ALT:
            Key_Event(K_ALT, down, win.lastMsgTime);
            Key_Event(K_RALT, down, win.lastMsgTime);
            return;
        case K_CTRL:
            Key_Event(K_CTRL, down, win.lastMsgTime);
            Key_Event(K_RCTRL, down, win.lastMsgTime);
            return;
        }
    }

    Key_Event(result, down, win.lastMsgTime);
}

static void mouse_wheel_event(int delta)
{
    UINT lines, key;

    // FIXME: handle WHEEL_DELTA and partial scrolls...
    if (delta > 0) {
        key = K_MWHEELUP;
    } else if (delta < 0) {
        key = K_MWHEELDOWN;
    } else {
        return;
    }

    if (Key_GetDest() & KEY_CONSOLE) {
        SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        clamp(lines, 1, 9);
    } else {
        lines = 1;
    }

    do {
        Key_Event(key, qtrue, win.lastMsgTime);
        Key_Event(key, qfalse, win.lastMsgTime);
    } while (--lines);
}

static void mouse_hwheel_event(int delta)
{
    UINT key;

    // FIXME: handle WHEEL_DELTA and partial scrolls...
    if (delta > 0) {
        key = K_MWHEELRIGHT;
    } else if (delta < 0) {
        key = K_MWHEELLEFT;
    } else {
        return;
    }

    Key_Event(key, qtrue, win.lastMsgTime);
    Key_Event(key, qfalse, win.lastMsgTime);
}

// this is complicated because Win32 seems to pack multiple mouse events into
// one update sometimes, so we always check all states and look for events
static void legacy_mouse_event(WPARAM wParam)
{
    int i, mask, temp = 0;

    if (wParam & MK_LBUTTON)
        temp |= 1;

    if (wParam & MK_RBUTTON)
        temp |= 2;

    if (wParam & MK_MBUTTON)
        temp |= 4;

    if (wParam & MK_XBUTTON1)
        temp |= 8;

    if (wParam & MK_XBUTTON2)
        temp |= 16;

    if (temp == win.mouse.state)
        return;

    // perform button actions
    for (i = 0, mask = 1; i < MOUSE_BUTTONS; i++, mask <<= 1) {
        if ((temp & mask) && !(win.mouse.state & mask)) {
            Key_Event(K_MOUSE1 + i, qtrue, win.lastMsgTime);
        }
        if (!(temp & mask) && (win.mouse.state & mask)) {
            Key_Event(K_MOUSE1 + i, qfalse, win.lastMsgTime);
        }
    }

    win.mouse.state = temp;
}

// returns TRUE if mouse cursor inside client area
static BOOL check_cursor_pos(void)
{
    POINT pt;

    if (win.mouse.grabbed == IN_GRAB)
        return TRUE;

    if (!GetCursorPos(&pt))
        return FALSE;

    return PtInRect(&win.screen_rc, pt);
}

#define BTN_DN(i) (1<<(i*2+0))
#define BTN_UP(i) (1<<(i*2+1))

static void raw_mouse_event(PRAWMOUSE rm)
{
    int i;

    if (!check_cursor_pos()) {
        // cursor is over non-client area
        // perform just button up actions
        for (i = 0; i < MOUSE_BUTTONS; i++) {
            if (rm->usButtonFlags & BTN_UP(i)) {
                Key_Event(K_MOUSE1 + i, qfalse, win.lastMsgTime);
            }
        }
        return;
    }

    if (rm->usButtonFlags) {
        // perform button actions
        for (i = 0; i < MOUSE_BUTTONS; i++) {
            if (rm->usButtonFlags & BTN_DN(i)) {
                Key_Event(K_MOUSE1 + i, qtrue, win.lastMsgTime);
            }
            if (rm->usButtonFlags & BTN_UP(i)) {
                Key_Event(K_MOUSE1 + i, qfalse, win.lastMsgTime);
            }
        }

        if (rm->usButtonFlags & RI_MOUSE_WHEEL) {
            mouse_wheel_event((short)rm->usButtonData);
        }

        // this flag is undocumented, but confirmed to work on Win7
        if (rm->usButtonFlags & 0x0800) {
            mouse_hwheel_event((short)rm->usButtonData);
        }
    }

    if ((rm->usFlags & (MOUSE_MOVE_RELATIVE | MOUSE_MOVE_ABSOLUTE)) == MOUSE_MOVE_RELATIVE) {
        win.mouse.mx += rm->lLastX;
        win.mouse.my += rm->lLastY;
    }
}

static void raw_input_event(HANDLE handle)
{
    BYTE buffer[64];
    UINT len, ret;
    PRAWINPUT ri;

    len = sizeof(buffer);
    ret = GetRawInputData(handle, RID_INPUT, buffer, &len, sizeof(RAWINPUTHEADER));
    if (ret == (UINT) - 1) {
        Com_EPrintf("GetRawInputData failed with error %#lx\n", GetLastError());
        return;
    }

    ri = (PRAWINPUT)buffer;
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        raw_mouse_event(&ri->data.mouse);
    }
}

static void pos_changing_event(HWND wnd, WINDOWPOS *pos)
{
    int w, h, nc_w, nc_h;
    LONG style;
    RECT rc;

    if (win.flags & QVF_FULLSCREEN)
        return;

    if (pos->flags & SWP_NOSIZE)
        return;

    style = GetWindowLong(wnd, GWL_STYLE);

    // calculate size of non-client area
    rc.left = 0;
    rc.top = 0;
    rc.right = 1;
    rc.bottom = 1;

    AdjustWindowRect(&rc, style, FALSE);

    nc_w = rc.right - rc.left - 1;
    nc_h = rc.bottom - rc.top - 1;

    // align client area
    w = (pos->cx - nc_w) & ~7;
    h = (pos->cy - nc_h) & ~1;

    // don't allow too small size
    if (w < 320) w = 320;
    if (h < 240) h = 240;

    // convert back to window size
    pos->cx = w + nc_w;
    pos->cy = h + nc_h;
}

static void pos_changed_event(HWND wnd, WINDOWPOS *pos)
{
    RECT rc;

    // get window position
    GetWindowRect(wnd, &rc);
    win.rc.x = rc.left;
    win.rc.y = rc.top;

    // get size of client area
    GetClientRect(wnd, &rc);
    win.rc.width = rc.right - rc.left;
    win.rc.height = rc.bottom - rc.top;

    // get rectangle of client area in screen coordinates
    MapWindowPoints(wnd, NULL, (POINT *)&rc, 2);
    win.screen_rc = rc;
    win.center_x = (rc.right + rc.left) / 2;
    win.center_y = (rc.top + rc.bottom) / 2;

    // set mode_changed flags unless in full screen
    if (win.flags & QVF_FULLSCREEN)
        return;

    if (!pos) {
        win.mode_changed |= MODE_STYLE;
        return;
    }

    if (!(pos->flags & SWP_NOSIZE))
        win.mode_changed |= MODE_SIZE;

    if (!(pos->flags & SWP_NOMOVE))
        win.mode_changed |= MODE_POS;
}

// main window procedure
STATIC LONG WINAPI Win_MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_MOUSEWHEEL:
        if (win.mouse.initialized == WIN_MOUSE_LEGACY)
            mouse_wheel_event((short)HIWORD(wParam));
        break;

    case WM_MOUSEHWHEEL:
        if (win.mouse.initialized == WIN_MOUSE_LEGACY)
            mouse_hwheel_event((short)HIWORD(wParam));
        break;

    case WM_NCMOUSEMOVE:
        // don't hide cursor
        if (win.mouse.initialized)
            IN_MouseEvent(-1, -1);
        break;

    case WM_MOUSEMOVE:
        if (win.mouse.initialized)
            IN_MouseEvent((short)LOWORD(lParam), (short)HIWORD(lParam));
        // fall through

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (win.mouse.initialized == WIN_MOUSE_LEGACY)
            legacy_mouse_event(wParam);
        break;

    case WM_HOTKEY:
        return FALSE;

    case WM_INPUT:
        if (wParam == RIM_INPUT && win.mouse.initialized == WIN_MOUSE_RAW)
            raw_input_event((HANDLE)lParam);
        break;

    case WM_CLOSE:
        PostQuitMessage(0);
        return FALSE;

    case WM_ACTIVATE:
        Win_Activate(wParam);
        break;

    case WM_WINDOWPOSCHANGING:
        pos_changing_event(hWnd, (WINDOWPOS *)lParam);
        break;

    case WM_WINDOWPOSCHANGED:
        pos_changed_event(hWnd, (WINDOWPOS *)lParam);
        return FALSE;

    case WM_STYLECHANGED:
    case WM_THEMECHANGED:
        pos_changed_event(hWnd, NULL);
        break;

    case WM_SYSCOMMAND:
        switch (wParam & 0xFFF0) {
        case SC_SCREENSAVE:
            return FALSE;
        case SC_MAXIMIZE:
            if (!vid_fullscreen->integer)
                VID_ToggleFullscreen();
            return FALSE;
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        legacy_key_event(wParam, lParam, qtrue);
        return FALSE;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        legacy_key_event(wParam, lParam, qfalse);
        return FALSE;

    case WM_SYSCHAR:
    case WM_CHAR:
#if USE_CHAR_EVENTS
        Key_CharEvent(wParam);
#endif
        return FALSE;

    default:
        break;
    }

    // pass all unhandled messages to DefWindowProc
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

/*
============
VID_SetMode
============
*/
void VID_SetMode(void)
{
    Win_SetMode();
    Win_ModeChanged();
}

/*
============
VID_PumpEvents
============
*/
void VID_PumpEvents(void)
{
    MSG        msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            Com_Quit(NULL, ERR_DISCONNECT);
            break;
        }
        win.lastMsgTime = msg.time;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (win.mode_changed) {
        if (win.mode_changed & MODE_REPOSITION) {
            Win_SetPosition();
        }
        if (win.mode_changed & (MODE_SIZE | MODE_POS | MODE_STYLE)) {
            VID_SetGeometry(&win.rc);
            if (win.mouse.grabbed == IN_GRAB) {
                Win_ClipCursor();
            }
        }
        if (win.mode_changed & MODE_SIZE) {
            Win_ModeChanged();
        }
        win.mode_changed = 0;
    }
}

static void win_style_changed(cvar_t *self)
{
    if (win.wnd && !(win.flags & QVF_FULLSCREEN)) {
        win.mode_changed |= MODE_REPOSITION;
    }
}

/*
============
Win_Init
============
*/
void Win_Init(void)
{
    WNDCLASSEX wc;

    // register variables
    vid_flip_on_switch = Cvar_Get("vid_flip_on_switch", "0", 0);
    vid_hwgamma = Cvar_Get("vid_hwgamma", "0", CVAR_REFRESH);
    win_noalttab = Cvar_Get("win_noalttab", "0", CVAR_ARCHIVE);
    win_noalttab->changed = win_noalttab_changed;
    win_disablewinkey = Cvar_Get("win_disablewinkey", "0", 0);
    win_disablewinkey->changed = win_disablewinkey_changed;
    win_noresize = Cvar_Get("win_noresize", "0", 0);
    win_noresize->changed = win_style_changed;
    win_notitle = Cvar_Get("win_notitle", "0", 0);
    win_notitle->changed = win_style_changed;
    win_alwaysontop = Cvar_Get("win_alwaysontop", "0", 0);
    win_alwaysontop->changed = win_style_changed;
    win_xpfix = Cvar_Get("win_xpfix", "0", 0);
    win_rawmouse = Cvar_Get("win_rawmouse", "1", 0);

    win_disablewinkey_changed(win_disablewinkey);

    // register the frame class
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = (WNDPROC)Win_MainWndProc;
    wc.hInstance = hGlobalInstance;
    wc.hIcon = LoadImage(hGlobalInstance, MAKEINTRESOURCE(IDI_APP),
                         IMAGE_ICON, 32, 32, LR_CREATEDIBSECTION);
    wc.hIconSm = LoadImage(hGlobalInstance, MAKEINTRESOURCE(IDI_APP),
                           IMAGE_ICON, 16, 16, LR_CREATEDIBSECTION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = _T(WINDOW_CLASS_NAME);

    if (!RegisterClassEx(&wc)) {
        Com_Error(ERR_FATAL, "Couldn't register main window class");
    }

    // create the window
    win.wnd = CreateWindow(
                  _T(WINDOW_CLASS_NAME),
                  _T(PRODUCT),
                  0, //style
                  0, 0, 0, 0,
                  NULL,
                  NULL,
                  hGlobalInstance,
                  NULL);

    if (!win.wnd) {
        Com_Error(ERR_FATAL, "Couldn't create main window");
    }

    win.dc = GetDC(win.wnd);
    if (!win.dc) {
        Com_Error(ERR_FATAL, "Couldn't get DC of the main window");
    }

    // init gamma ramp
    if (vid_hwgamma->integer) {
        if (GetDeviceGammaRamp(win.dc, win.gamma_orig)) {
            Com_DPrintf("...enabling hardware gamma\n");
            win.flags |= QVF_GAMMARAMP;
            memcpy(win.gamma_cust, win.gamma_orig, sizeof(win.gamma_cust));
        } else {
            Com_DPrintf("...hardware gamma not supported\n");
            Cvar_Set("vid_hwgamma", "0");
        }
    }
}

/*
============
Win_Shutdown
============
*/
void Win_Shutdown(void)
{
    if (win.flags & QVF_GAMMARAMP) {
        SetDeviceGammaRamp(win.dc, win.gamma_orig);
    }

    // prevents leaving empty slots in the taskbar
    ShowWindow(win.wnd, SW_SHOWNORMAL);
    ReleaseDC(win.wnd, win.dc);
    DestroyWindow(win.wnd);
    UnregisterClass(_T(WINDOW_CLASS_NAME), hGlobalInstance);

    if (win.kbdHook) {
        UnhookWindowsHookEx(win.kbdHook);
    }

    if (win.flags & QVF_FULLSCREEN) {
        ChangeDisplaySettings(NULL, 0);
    }

    memset(&win, 0, sizeof(win));
}

/*
===============================================================================

MOUSE

===============================================================================
*/

static void Win_HideCursor(void)
{
    while (ShowCursor(FALSE) >= 0)
        ;
}

static void Win_ShowCursor(void)
{
    while (ShowCursor(TRUE) < 0)
        ;
}

// Called when the window gains focus or changes in some way
static void Win_ClipCursor(void)
{
    SetCursorPos(win.center_x, win.center_y);
    ClipCursor(&win.screen_rc);
}

// Called when the window gains focus
static void Win_AcquireMouse(void)
{
    int parms[3];

    if (win.mouse.parmsvalid) {
        if (win_xpfix->integer) {
            parms[0] = parms[1] = parms[2] = 0;
        } else {
            parms[0] = parms[1] = 0;
            parms[2] = 1;
        }
        win.mouse.restoreparms = SystemParametersInfo(
                                     SPI_SETMOUSE, 0, parms, 0);
    }

    Win_ClipCursor();
    SetCapture(win.wnd);

    SetWindowText(win.wnd, "[" PRODUCT "]");
}

// Called when the window loses focus
static void Win_DeAcquireMouse(void)
{
    if (win.mouse.restoreparms)
        SystemParametersInfo(SPI_SETMOUSE, 0, win.mouse.originalparms, 0);

    SetCursorPos(win.center_x, win.center_y);

    ClipCursor(NULL);
    ReleaseCapture();

    SetWindowText(win.wnd, PRODUCT);
}

static qboolean Win_GetMouseMotion(int *dx, int *dy)
{
    POINT pt;

    if (!win.mouse.initialized) {
        return qfalse;
    }

    if (win.mouse.grabbed != IN_GRAB) {
        return qfalse;
    }

    if (win.mouse.initialized == WIN_MOUSE_RAW) {
        *dx = win.mouse.mx;
        *dy = win.mouse.my;
        win.mouse.mx = 0;
        win.mouse.my = 0;
        return qtrue;
    }

    // find mouse movement
    if (!GetCursorPos(&pt)) {
        return qfalse;
    }

    *dx = pt.x - win.center_x;
    *dy = pt.y - win.center_y;

    // force the mouse to the center, so there's room to move
    SetCursorPos(win.center_x, win.center_y);
    return qtrue;
}

static BOOL register_raw_mouse(DWORD flags)
{
    RAWINPUTDEVICE rid;

    memset(&rid, 0, sizeof(rid));
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = flags;
    rid.hwndTarget = win.wnd;

    return RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

static void Win_ShutdownMouse(void)
{
    if (!win.mouse.initialized) {
        return;
    }

    Win_DeAcquireMouse();
    Win_ShowCursor();

    if (win.mouse.initialized == WIN_MOUSE_RAW) {
        register_raw_mouse(RIDEV_REMOVE);
    }

    win_xpfix->changed = NULL;
    win_rawmouse->changed = NULL;

    memset(&win.mouse, 0, sizeof(win.mouse));
}

static void win_xpfix_changed(cvar_t *self)
{
    if (win.mouse.grabbed == IN_GRAB) {
        Win_AcquireMouse();
    }
}

static void win_rawmouse_changed(cvar_t *self)
{
    if (win.mouse.initialized) {
        Win_ShutdownMouse();
        Win_InitMouse();
    }
}

static qboolean Win_InitMouse(void)
{
    if (!win.wnd) {
        return qfalse;
    }

    win.mouse.initialized = WIN_MOUSE_LEGACY;

    if (win_rawmouse->integer) {
        if (!register_raw_mouse(/*RIDEV_NOLEGACY*/ 0)) {
            Com_EPrintf("RegisterRawInputDevices failed with error %#lx\n", GetLastError());
            Cvar_Set("win_rawmouse", "0");
        } else {
            Com_Printf("Raw mouse initialized.\n");
            win.mouse.initialized = WIN_MOUSE_RAW;
        }
    }

    if (win.mouse.initialized == WIN_MOUSE_LEGACY) {
        win.mouse.parmsvalid = SystemParametersInfo(SPI_GETMOUSE, 0,
                               win.mouse.originalparms, 0);
        win_xpfix->changed = win_xpfix_changed;
        Com_Printf("Legacy mouse initialized.\n");
    }

    win_rawmouse->changed = win_rawmouse_changed;

    return qtrue;
}

// Called when the main window gains or loses focus.
static void Win_GrabMouse(grab_t grab)
{
    if (!win.mouse.initialized) {
        return;
    }

    if (win.mouse.grabbed == grab) {
        if (win.mouse.initialized == WIN_MOUSE_LEGACY) {
            SetCursorPos(win.center_x, win.center_y);
        }
        win.mouse.mx = 0;
        win.mouse.my = 0;
        return;
    }

    if (grab == IN_GRAB) {
        Win_AcquireMouse();
        Win_HideCursor();
    } else {
        if (win.mouse.grabbed == IN_GRAB) {
            Win_DeAcquireMouse();
        }
        if (grab == IN_HIDE) {
            Win_HideCursor();
        } else {
            Win_ShowCursor();
        }
    }

    win.mouse.grabbed = grab;
    win.mouse.state = 0;
    win.mouse.mx = 0;
    win.mouse.my = 0;
}

static void Win_WarpMouse(int x, int y)
{
    SetCursorPos(win.screen_rc.left + x, win.screen_rc.top + y);
}

/*
================
VID_GetClipboardData
================
*/
char *VID_GetClipboardData(void)
{
    HANDLE clipdata;
    char *cliptext, *data;

    if (!OpenClipboard(NULL)) {
        Com_DPrintf("Couldn't open clipboard.\n");
        return NULL;
    }

    data = NULL;
    if ((clipdata = GetClipboardData(CF_TEXT)) != NULL) {
        if ((cliptext = GlobalLock(clipdata)) != NULL) {
            data = Z_CopyString(cliptext);
            GlobalUnlock(clipdata);
        }
    }

    CloseClipboard();
    return data;
}

/*
================
VID_SetClipboardData
================
*/
void VID_SetClipboardData(const char *data)
{
    HANDLE clipdata;
    char *cliptext;
    size_t length;

    if (!data || !*data) {
        return;
    }

    if (!OpenClipboard(NULL)) {
        Com_DPrintf("Couldn't open clipboard.\n");
        return;
    }

    EmptyClipboard();

    length = strlen(data) + 1;
    if ((clipdata = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, length)) != NULL) {
        if ((cliptext = GlobalLock(clipdata)) != NULL) {
            memcpy(cliptext, data, length);
            GlobalUnlock(clipdata);
            SetClipboardData(CF_TEXT, clipdata);
        }
    }

    CloseClipboard();
}

/*
@@@@@@@@@@@@@@@@@@@
VID_FillInputAPI
@@@@@@@@@@@@@@@@@@@
*/
void VID_FillInputAPI(inputAPI_t *api)
{
    api->Init = Win_InitMouse;
    api->Shutdown = Win_ShutdownMouse;
    api->Grab = Win_GrabMouse;
    api->Warp = Win_WarpMouse;
    api->GetEvents = NULL;
    api->GetMotion = Win_GetMouseMotion;
}
