// DualShock 4s as SDL virtual gamepads (the PS4 build of SDL3 has no controller driver of its own): every logged-in
// user's pad is opened and its state fed to a virtual gamepad on each poll, so runtime/hle/input.c sees an ordinary SDL
// gamepad. Buttons by position, as everywhere in Savage Wind: Cross south, Circle east, Square west, Triangle north;
// the touch pad is Back (the Switch's Minus: map), Options is Start (Plus: inventory).
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#define PLAYERS 4

typedef struct {
    int32_t user, handle;
    SDL_JoystickID id;
    SDL_Joystick *joy;
} Player;
static Player players[PLAYERS];
static bool g_ready;

static bool rumble(void *data, Uint16 low, Uint16 high) {
    Player *p = data;
    OrbisPadVibeParam v = {.lgMotor = (uint8_t)(low >> 8), .smMotor = (uint8_t)(high >> 8)};
    return scePadSetVibration(p->handle, &v) == 0;
}

static bool led(void *data, Uint8 r, Uint8 g, Uint8 b) {
    Player *p = data;
    OrbisPadColor c = {r, g, b, 255};
    return scePadSetLightBar(p->handle, &c) == 0;
}

static void detach(Player *p) {
    if (p->joy) SDL_CloseJoystick(p->joy);
    if (p->id) SDL_DetachVirtualJoystick(p->id);
    p->joy = NULL;
    p->id = 0;
}

static bool attach(Player *p, int slot) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    d.axis_mask = (1u << d.naxes) - 1;
    d.button_mask = (1u << d.nbuttons) - 1;
    d.name = "DUALSHOCK 4";
    d.vendor_id = 0x054c;  // Sony DualShock 4 (CUH-ZCT2)
    d.product_id = 0x09cc;
    d.userdata = p;
    d.Rumble = rumble;
    d.SetLED = led;
    p->id = SDL_AttachVirtualJoystick(&d);
    if (!p->id) { fprintf(stderr, "[pad] attach failed: %s\n", SDL_GetError()); return false; }
    p->joy = SDL_OpenJoystick(p->id);
    if (!p->joy) { detach(p); return false; }
    SDL_SetJoystickPlayerIndex(p->joy, slot);
    return true;
}

// The logged-in users' pads, opened once each (a user logging in later gets one on the next poll).
static void open_users(void) {
    OrbisUserServiceLoginUserIdList list;
    if (sceUserServiceGetLoginUserIdList(&list) != 0) return;
    for (int i = 0; i < PLAYERS; i++) {
        int32_t user = list.userId[i];
        if (user < 0) continue;  // (-1: no user in that slot)
        bool known = false;
        for (int k = 0; k < PLAYERS; k++) known |= players[k].handle > 0 && players[k].user == user;
        if (known) continue;
        for (int k = 0; k < PLAYERS; k++)
            if (players[k].handle <= 0) {
                int32_t h = scePadOpen(user, 0, 0, NULL);
                if (h < 0) h = scePadGetHandle(user, 0, 0);  // (already open)
                if (h > 0) {
                    players[k].user = user;
                    players[k].handle = h;
                    fprintf(stderr, "[pad] user %d: pad %d\n", user, k);
                }
                break;
            }
    }
}

static Sint16 stick_axis(uint8_t v) {  // 0..255, 128 centered -> SDL's -32768..32767
    int s = ((int)v - 128) * 256;
    return (Sint16)(s > 32767 ? 32767 : s);
}
// 0..255 -> SDL's full axis range: a virtual gamepad's trigger rests at -32768 (SDL_virtualjoystick.c), and 0 read
// as half pressed (ZL held: Link always walked locked on)
static Sint16 trigger(uint8_t v) { return (Sint16)(v * 65535 / 255 - 32768); }

void ps4_pad_poll(void) {
    if (!g_ready) {
        g_ready = true;
        sceUserServiceInitialize(NULL);
        int32_t r = scePadInit();
        if (r) fprintf(stderr, "[pad] scePadInit: %#x\n", (unsigned)r);
        for (int k = 0; k < PLAYERS; k++) players[k].handle = -1;
    }
    static uint32_t polls;
    if (polls++ % 120 == 0) open_users();  // (about every 2 s)
    static const uint32_t map[SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = ORBIS_PAD_BUTTON_CROSS,       [SDL_GAMEPAD_BUTTON_EAST] = ORBIS_PAD_BUTTON_CIRCLE,
        [SDL_GAMEPAD_BUTTON_WEST] = ORBIS_PAD_BUTTON_SQUARE,       [SDL_GAMEPAD_BUTTON_NORTH] = ORBIS_PAD_BUTTON_TRIANGLE,
        [SDL_GAMEPAD_BUTTON_BACK] = ORBIS_PAD_BUTTON_TOUCH_PAD,    [SDL_GAMEPAD_BUTTON_START] = ORBIS_PAD_BUTTON_OPTIONS,
        [SDL_GAMEPAD_BUTTON_LEFT_STICK] = ORBIS_PAD_BUTTON_L3,     [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = ORBIS_PAD_BUTTON_R3,
        [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = ORBIS_PAD_BUTTON_L1,  [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = ORBIS_PAD_BUTTON_R1,
        [SDL_GAMEPAD_BUTTON_DPAD_UP] = ORBIS_PAD_BUTTON_UP,        [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = ORBIS_PAD_BUTTON_DOWN,
        [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = ORBIS_PAD_BUTTON_LEFT,    [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = ORBIS_PAD_BUTTON_RIGHT,
    };
    for (int k = 0; k < PLAYERS; k++) {
        Player *p = &players[k];
        if (p->handle <= 0) continue;
        OrbisPadData s;
        memset(&s, 0, sizeof s);
        if (scePadReadState(p->handle, &s) != 0 || !s.connected) {
            if (p->id) detach(p);
            continue;
        }
        if (!p->id && !attach(p, k)) continue;
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTX, stick_axis(s.leftStick.x));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTY, stick_axis(s.leftStick.y));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTX, stick_axis(s.rightStick.x));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTY, stick_axis(s.rightStick.y));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(s.analogButtons.l2));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(s.analogButtons.r2));
        for (int b = 0; b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; b++)
            if (map[b]) SDL_SetJoystickVirtualButton(p->joy, b, (s.buttons & map[b]) != 0);
    }
}
