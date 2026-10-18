/*
 * ot_pad.c -- controllers and keyboard on Linux and the Switch, through SDL2,
 * behind the XInput and GetAsyncKeyState calls controls.c makes on Windows.
 *
 * Up to four controllers, in the order SDL reports them (on the Switch: the
 * handheld Joy-Con pair or player 1's controller first). Buttons are read by
 * position, as on an Xbox pad: A is the bottom face button.
 *
 * SDL's event queue is pumped by the renderer's window loop; this file only
 * reads the state it keeps.
 */
#include <windows.h>
#include <string.h>
#include <SDL.h>

static SDL_GameController *s_pad[4];
static SDL_mutex *s_lock;
static DWORD s_packet[4];
static Uint32 s_last_scan;

void ot_pad_init(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
    if (!s_lock) s_lock = SDL_CreateMutex();
#ifdef __SWITCH__
    /* Joy-Cons held sideways are single controllers; the pair in the
     * console's rails is one. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#endif
}

/* Opens newly connected controllers, closes removed ones (twice a second). */
static void scan(void)
{
    Uint32 now = SDL_GetTicks();
    int i, j, n;
    if (s_last_scan && now - s_last_scan < 500) return;
    s_last_scan = now;
    for (i = 0; i < 4; i++)
        if (s_pad[i] && !SDL_GameControllerGetAttached(s_pad[i])) {
            SDL_GameControllerClose(s_pad[i]);
            s_pad[i] = NULL;
        }
    n = SDL_NumJoysticks();
    for (j = 0; j < n; j++) {
        SDL_JoystickID id;
        int have = 0;
        if (!SDL_IsGameController(j)) continue;
        id = SDL_JoystickGetDeviceInstanceID(j);
        for (i = 0; i < 4; i++)
            if (s_pad[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad[i])) == id) have = 1;
        if (have) continue;
        for (i = 0; i < 4; i++)
            if (!s_pad[i]) { s_pad[i] = SDL_GameControllerOpen(j); break; }
    }
}

static SHORT axis(SDL_GameController *c, SDL_GameControllerAxis a, int flip)
{
    int v = SDL_GameControllerGetAxis(c, a);
    if (flip) v = -v - 1;                   /* SDL: down is positive; XInput: up */
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return (SHORT)v;
}

DWORD XInputGetState(DWORD idx, XINPUT_STATE *st)
{
    static const struct { SDL_GameControllerButton b; WORD bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP,       XINPUT_GAMEPAD_DPAD_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     XINPUT_GAMEPAD_DPAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    XINPUT_GAMEPAD_DPAD_RIGHT },
        { SDL_CONTROLLER_BUTTON_START,         XINPUT_GAMEPAD_START },
        { SDL_CONTROLLER_BUTTON_BACK,          XINPUT_GAMEPAD_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK,     XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_A,             XINPUT_GAMEPAD_A },
        { SDL_CONTROLLER_BUTTON_B,             XINPUT_GAMEPAD_B },
        { SDL_CONTROLLER_BUTTON_X,             XINPUT_GAMEPAD_X },
        { SDL_CONTROLLER_BUTTON_Y,             XINPUT_GAMEPAD_Y },
    };
    SDL_GameController *c;
    XINPUT_GAMEPAD g;
    size_t i;

    if (st) memset(st, 0, sizeof *st);
    if (idx >= 4 || !st || !s_lock) return ERROR_DEVICE_NOT_CONNECTED;
    SDL_LockMutex(s_lock);
    scan();
    c = s_pad[idx];
    if (!c) { SDL_UnlockMutex(s_lock); return ERROR_DEVICE_NOT_CONNECTED; }
    memset(&g, 0, sizeof g);
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(c, map[i].b)) g.wButtons |= map[i].bit;
    g.bLeftTrigger  = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    g.bRightTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    g.sThumbLX = axis(c, SDL_CONTROLLER_AXIS_LEFTX, 0);
    g.sThumbLY = axis(c, SDL_CONTROLLER_AXIS_LEFTY, 1);
    g.sThumbRX = axis(c, SDL_CONTROLLER_AXIS_RIGHTX, 0);
    g.sThumbRY = axis(c, SDL_CONTROLLER_AXIS_RIGHTY, 1);
    if (memcmp(&g, &st->Gamepad, sizeof g)) s_packet[idx]++;
    st->dwPacketNumber = s_packet[idx];
    st->Gamepad = g;
    SDL_UnlockMutex(s_lock);
    return ERROR_SUCCESS;
}

DWORD XInputSetState(DWORD idx, XINPUT_VIBRATION *v)
{
    DWORD r = ERROR_DEVICE_NOT_CONNECTED;
    if (idx >= 4 || !v || !s_lock) return r;
    SDL_LockMutex(s_lock);
    if (s_pad[idx]) {
        /* Held until the next call, as XInput does (SDL needs a duration). */
        SDL_GameControllerRumble(s_pad[idx], v->wLeftMotorSpeed, v->wRightMotorSpeed, 10000);
        r = ERROR_SUCCESS;
    }
    SDL_UnlockMutex(s_lock);
    return r;
}

/* Win32 virtual-key code -> SDL scancode (the keys a binding can use). */
static SDL_Scancode vk_scancode(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    switch (vk) {
    case VK_NUMPAD0:  return SDL_SCANCODE_KP_0;
    case VK_SPACE:    return SDL_SCANCODE_SPACE;
    case VK_RETURN:   return SDL_SCANCODE_RETURN;
    case VK_ESCAPE:   return SDL_SCANCODE_ESCAPE;
    case VK_TAB:      return SDL_SCANCODE_TAB;
    case VK_BACK:     return SDL_SCANCODE_BACKSPACE;
    case VK_CAPITAL:  return SDL_SCANCODE_CAPSLOCK;
    case VK_UP:       return SDL_SCANCODE_UP;
    case VK_DOWN:     return SDL_SCANCODE_DOWN;
    case VK_LEFT:     return SDL_SCANCODE_LEFT;
    case VK_RIGHT:    return SDL_SCANCODE_RIGHT;
    case VK_LSHIFT:   return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT:   return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU:    return SDL_SCANCODE_LALT;
    case VK_RMENU:    return SDL_SCANCODE_RALT;
    case VK_INSERT:   return SDL_SCANCODE_INSERT;
    case VK_DELETE:   return SDL_SCANCODE_DELETE;
    case VK_HOME:     return SDL_SCANCODE_HOME;
    case VK_END:      return SDL_SCANCODE_END;
    case VK_PRIOR:    return SDL_SCANCODE_PAGEUP;
    case VK_NEXT:     return SDL_SCANCODE_PAGEDOWN;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
    case VK_ADD:      return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
    case VK_DECIMAL:  return SDL_SCANCODE_KP_PERIOD;
    case VK_DIVIDE:   return SDL_SCANCODE_KP_DIVIDE;
    case VK_OEM_1:    return SDL_SCANCODE_SEMICOLON;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
    case VK_OEM_2:    return SDL_SCANCODE_SLASH;
    case VK_OEM_3:    return SDL_SCANCODE_GRAVE;
    case VK_OEM_4:    return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_5:    return SDL_SCANCODE_BACKSLASH;
    case VK_OEM_6:    return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_7:    return SDL_SCANCODE_APOSTROPHE;
    default:          return SDL_SCANCODE_UNKNOWN;
    }
}

SHORT GetAsyncKeyState(int vk)
{
    int n = 0;
    const Uint8 *k;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    k = SDL_GetKeyboardState(&n);
    if (vk == VK_SHIFT)   return (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? (SHORT)0x8000 : 0;
    if (vk == VK_CONTROL) return (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) ? (SHORT)0x8000 : 0;
    if (vk == VK_MENU)    return (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) ? (SHORT)0x8000 : 0;
    {
        SDL_Scancode sc = vk_scancode(vk);
        return (sc != SDL_SCANCODE_UNKNOWN && (int)sc < n && k[sc]) ? (SHORT)0x8000 : 0;
    }
}
