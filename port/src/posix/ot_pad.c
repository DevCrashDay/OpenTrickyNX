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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

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

#ifdef __SWITCH__
/* SDL's Switch driver always lists eight controllers, attached or not:
 * index i reads player i+1 (index 0 also the Joy-Cons in the console's
 * rails). Only a player the console has a controller for is connected, so
 * the game sees a second pad when there is one and not before. */
static int npad_connected(DWORD i)
{
    static int all = -1;
    if (all < 0) { const char *e = getenv("OT_PAD_ALL"); all = e && e[0] == '1'; }
    if (all) return 1;                  /* OT_PAD_ALL=1: as before, all four */
    if (hidGetNpadStyleSet((HidNpadIdType)(HidNpadIdType_No1 + i))) return 1;
    return i == 0 && hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0;
}

/* One log line whenever the console's controllers change: which style each
 * player has (handheld 0x2, a pair 0x4, single Joy-Cons 0x8/0x10, Pro 0x1). */
static void npad_log(void)
{
    static u32 last[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    u32 h = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    u32 a = hidGetNpadStyleSet(HidNpadIdType_No1), b = hidGetNpadStyleSet(HidNpadIdType_No2);
    if (h != last[0] || a != last[1] || b != last[2]) {
        fprintf(stderr, "[PAD] controllers: handheld 0x%x, player 1 0x%x, player 2 0x%x\n",
                (unsigned)h, (unsigned)a, (unsigned)b);
        last[0] = h; last[1] = a; last[2] = b;
    }
}

/* The console's own "Controllers" screen, for two players: who is player 1
 * and 2, a Joy-Con each held sideways, a Pro Controller... Opened by holding
 * - and + together (Back and Start on the Xbox pad), on one controller or
 * two; the game waits while it is up. */
static WORD s_chord_held[4];             /* - and + held, per controller */

static void controller_setup(void)
{
    HidLaControllerSupportArg arg;
    HidLaControllerSupportResultInfo info;
    Result rc;
    const char *four = getenv("OT_FOUR_PLAYER");
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 1;
    /* four with the four-player split screen (fourplayer.h) */
    arg.hdr.player_count_max = (four && four[0] == '1') ? 4 : 2;
    arg.hdr.enable_take_over_connection = 1;
    arg.hdr.enable_left_justify = 1;
    arg.hdr.enable_permit_joy_dual = 1;
    arg.hdr.enable_single_mode = 0;
    memset(&info, 0, sizeof info);
    rc = hidLaShowControllerSupport(&info, &arg);
    fprintf(stderr, "[PAD] controller screen: rc 0x%x, %d player(s), selected id %d\n",
            (unsigned)rc, (int)info.player_count, (int)info.selected_id);
}
#endif

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
#ifdef __SWITCH__
    if (idx == 0) npad_log();
    if (c && !npad_connected(idx)) c = NULL;
    if (!c) s_chord_held[idx] = 0;
#endif
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
#ifdef __SWITCH__
    {   /* what SDL reads for player 1, before the bindings: one line per change */
        static WORD last = 0xFFFF;
        static int lines;
        WORD sticks = (WORD)((g.sThumbLX < -16000) | (g.sThumbLX > 16000) << 1 |
                             (g.sThumbLY < -16000) << 2 | (g.sThumbLY > 16000) << 3);
        WORD now = g.wButtons ^ (WORD)(sticks << 12) ^ (WORD)((g.bLeftTrigger > 60) << 10);
        if (idx == 0 && now != last && lines < 300) {
            last = now;
            lines++;
            fprintf(stderr, "[PAD] player 1: buttons 0x%04x, left stick %d %d, triggers %d %d\n",
                    (unsigned)g.wButtons, g.sThumbLX, g.sThumbLY, g.bLeftTrigger, g.bRightTrigger);
        }
    }
    {
        /* - and + held together, on one controller or across two (a single
         * Joy-Con has only one of them) */
        static int chord;
        const WORD both = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
        WORD all = 0;
        int k;
        s_chord_held[idx] = g.wButtons & both;
        for (k = 0; k < 4; k++) all |= s_chord_held[k];
        if (all == both) {
            if (!chord) {
                chord = 1;
                controller_setup();
                memset(s_chord_held, 0, sizeof s_chord_held);
            }
        } else {
            chord = 0;
        }
    }
#endif
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
