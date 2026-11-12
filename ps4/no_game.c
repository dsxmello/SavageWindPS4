// A runtime-only eboot (no SW_GENERATED): no game code linked in. Instead of stopping, it tests on the console what the
// game will need (docs/ps4.md, "first test"), logging to /data/savagewind/savagewind.log:
//   1. guest memory: chunks of the guest window get memory on first touch (runtime/platform/ps4.c's fault handler)
//   2. Vulkan: RADV on GNM, the VideoOut swapchain - the screen is cleared in a color that cycles
//   3. the pads: the color follows the left stick, Cross / Circle / Square / Triangle are logged; Options ends it
#include <stdio.h>
#include <string.h>

#include "gamelib.h"
#include "rt.h"
#include "vulkan/vk.h"

#include <SDL3/SDL.h>

const GameLibTables *swgame_tables(void) { return NULL; }
int simd_fma_unfused;  // (runtime/gamelib.c: the game code defines it when it is linked in)

uint64_t plat_guest_backed_bytes(void);  // runtime/platform/ps4.c
void ui_window_init(void);
void ps4_pad_poll(void);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);

static void memory_test(void) {
    plat_crash_handler_install();
    uint64_t before = plat_guest_backed_bytes();
    uint64_t t0 = plat_now_ns();
    for (uint64_t i = 0; i < 64; i++) {  // 128 MB of the heap, 2 MB apart: one fault each
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(HEAP_BASE + i * 0x200000 + 0x1234 * 8);
        if (*p != 0) LOG("[test] memory: %llx not zero\n", (unsigned long long)(uintptr_t)p);
        *p = i;
    }
    bool ok = true;
    for (uint64_t i = 0; i < 64; i++)
        ok &= *(volatile uint64_t *)(uintptr_t)(HEAP_BASE + i * 0x200000 + 0x1234 * 8) == i;
    LOG("[test] memory: %s, %llu MB backed on first touch in %.1f ms\n", ok ? "OK" : "FAILED",
        (unsigned long long)((plat_guest_backed_bytes() - before) >> 20), (plat_now_ns() - t0) / 1e6);
    plat_commit(HEAP_BASE + 0x10000000, 0x1000000);  // (as before a file read: 16 MB more)
    LOG("[test] memory: commit -> %llu MB backed\n", (unsigned long long)(plat_guest_backed_bytes() >> 20));
}

void ps4_no_game_test(void) {
    LOG("[test] this eboot has no game code (docs/ps4.md): console test\n");
    memory_test();
    ui_window_init();
    LOG("[test] vulkan: %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
    if (!SDL_Init(SDL_INIT_GAMEPAD)) LOG("[test] SDL gamepad: %s\n", SDL_GetError());
    SDL_Gamepad *pad = NULL;
    uint64_t t0 = plat_now_ns(), frames = 0;
    bool was[4] = {0};
    for (;;) {
        ps4_pad_poll();
        SDL_PumpEvents();
        if (!pad) {
            int n = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&n);
            if (n > 0) { pad = SDL_OpenGamepad(ids[0]); LOG("[test] pad: %s\n", SDL_GetGamepadName(pad)); }
            SDL_free(ids);
        }
        float t = (plat_now_ns() - t0) / 1e9f, lx = 0, ly = 0;
        if (pad) {
            lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
            ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.f;
            static const char *names[4] = {"Cross (south)", "Circle (east)", "Square (west)", "Triangle (north)"};
            for (int b = 0; b < 4; b++) {
                bool down = SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
                if (down && !was[b]) LOG("[test] pad: %s\n", names[b]);
                was[b] = down;
            }
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) break;
        }
        float clear[4] = {0.5f + 0.5f * (lx ? lx : SDL_sinf(t)), 0.5f + 0.5f * (ly ? ly : SDL_sinf(t * 1.3f)), 0.6f, 1};
        if (vk_frame_begin(clear)) { vk_frame_end(NULL); frames++; }
        if (frames && frames % 600 == 0)
            LOG("[test] %llu frames, %.1f fps\n", (unsigned long long)frames, frames / ((plat_now_ns() - t0) / 1e9));
    }
    LOG("[test] done: %llu frames in %.1f s\n", (unsigned long long)frames, (plat_now_ns() - t0) / 1e9);
    plat_terminate();
}
