/*
 * dos_input_keycodes.h — Linux evdev → SDL3 mapping for the "dos" video driver.
 *
 * Included only by SDL_dosvideo.c (one TU), so the table + helpers are static.
 * The virtio-input driver delivers raw evdev (type/code/value); this header maps
 * EV_KEY codes to SDL scancodes and mouse buttons. See dos_virtio_input.h and
 * riscv/docs/input/00-virtio-input-plan.md.
 */
#ifndef DOS_INPUT_KEYCODES_H
#define DOS_INPUT_KEYCODES_H

#include <SDL3/SDL_scancode.h>
#include <stdint.h>

/* evdev event types (linux input-event-codes.h) */
#define EVT_SYN 0
#define EVT_KEY 1
#define EVT_REL 2
#define EVT_ABS 3

/* evdev EV_REL codes */
#define EVREL_X     0
#define EVREL_Y     1
#define EVREL_WHEEL 8
/* evdev EV_ABS codes */
#define EVABS_X 0
#define EVABS_Y 1
/* evdev EV_KEY mouse buttons */
#define EVBTN_LEFT   0x110
#define EVBTN_RIGHT  0x111
#define EVBTN_MIDDLE 0x112
/* evdev EV_SYN code */
#define EVSYN_REPORT 0

/* Linux evdev keycode -> SDL scancode. 0 (SDL_SCANCODE_UNKNOWN) = unmapped. */
static const uint16_t k_evdev_to_sdl[256] = {
    [1]  = SDL_SCANCODE_ESCAPE,
    [2]  = SDL_SCANCODE_1, [3] = SDL_SCANCODE_2, [4] = SDL_SCANCODE_3,
    [5]  = SDL_SCANCODE_4, [6] = SDL_SCANCODE_5, [7] = SDL_SCANCODE_6,
    [8]  = SDL_SCANCODE_7, [9] = SDL_SCANCODE_8, [10] = SDL_SCANCODE_9,
    [11] = SDL_SCANCODE_0,
    [12] = SDL_SCANCODE_MINUS, [13] = SDL_SCANCODE_EQUALS,
    [14] = SDL_SCANCODE_BACKSPACE, [15] = SDL_SCANCODE_TAB,
    [16] = SDL_SCANCODE_Q, [17] = SDL_SCANCODE_W, [18] = SDL_SCANCODE_E,
    [19] = SDL_SCANCODE_R, [20] = SDL_SCANCODE_T, [21] = SDL_SCANCODE_Y,
    [22] = SDL_SCANCODE_U, [23] = SDL_SCANCODE_I, [24] = SDL_SCANCODE_O,
    [25] = SDL_SCANCODE_P,
    [26] = SDL_SCANCODE_LEFTBRACKET, [27] = SDL_SCANCODE_RIGHTBRACKET,
    [28] = SDL_SCANCODE_RETURN, [29] = SDL_SCANCODE_LCTRL,
    [30] = SDL_SCANCODE_A, [31] = SDL_SCANCODE_S, [32] = SDL_SCANCODE_D,
    [33] = SDL_SCANCODE_F, [34] = SDL_SCANCODE_G, [35] = SDL_SCANCODE_H,
    [36] = SDL_SCANCODE_J, [37] = SDL_SCANCODE_K, [38] = SDL_SCANCODE_L,
    [39] = SDL_SCANCODE_SEMICOLON, [40] = SDL_SCANCODE_APOSTROPHE,
    [41] = SDL_SCANCODE_GRAVE, [42] = SDL_SCANCODE_LSHIFT,
    [43] = SDL_SCANCODE_BACKSLASH,
    [44] = SDL_SCANCODE_Z, [45] = SDL_SCANCODE_X, [46] = SDL_SCANCODE_C,
    [47] = SDL_SCANCODE_V, [48] = SDL_SCANCODE_B, [49] = SDL_SCANCODE_N,
    [50] = SDL_SCANCODE_M,
    [51] = SDL_SCANCODE_COMMA, [52] = SDL_SCANCODE_PERIOD,
    [53] = SDL_SCANCODE_SLASH, [54] = SDL_SCANCODE_RSHIFT,
    [55] = SDL_SCANCODE_KP_MULTIPLY, [56] = SDL_SCANCODE_LALT,
    [57] = SDL_SCANCODE_SPACE, [58] = SDL_SCANCODE_CAPSLOCK,
    [59] = SDL_SCANCODE_F1, [60] = SDL_SCANCODE_F2, [61] = SDL_SCANCODE_F3,
    [62] = SDL_SCANCODE_F4, [63] = SDL_SCANCODE_F5, [64] = SDL_SCANCODE_F6,
    [65] = SDL_SCANCODE_F7, [66] = SDL_SCANCODE_F8, [67] = SDL_SCANCODE_F9,
    [68] = SDL_SCANCODE_F10,
    [87] = SDL_SCANCODE_F11, [88] = SDL_SCANCODE_F12,
    [96] = SDL_SCANCODE_KP_ENTER, [97] = SDL_SCANCODE_RCTRL,
    [98] = SDL_SCANCODE_KP_DIVIDE, [100] = SDL_SCANCODE_RALT,
    [102] = SDL_SCANCODE_HOME, [103] = SDL_SCANCODE_UP,
    [104] = SDL_SCANCODE_PAGEUP, [105] = SDL_SCANCODE_LEFT,
    [106] = SDL_SCANCODE_RIGHT, [107] = SDL_SCANCODE_END,
    [108] = SDL_SCANCODE_DOWN, [109] = SDL_SCANCODE_PAGEDOWN,
    [110] = SDL_SCANCODE_INSERT, [111] = SDL_SCANCODE_DELETE,
    [125] = SDL_SCANCODE_LGUI, [126] = SDL_SCANCODE_RGUI,
};

static inline SDL_Scancode dos_evdev_scancode(uint16_t code)
{
    return (code < 256) ? (SDL_Scancode)k_evdev_to_sdl[code] : SDL_SCANCODE_UNKNOWN;
}

/* evdev mouse button -> SDL button index (0 = not a button). */
static inline uint8_t dos_evdev_button(uint16_t code)
{
    switch (code) {
        case EVBTN_LEFT:   return 1;   /* SDL_BUTTON_LEFT   */
        case EVBTN_MIDDLE: return 2;   /* SDL_BUTTON_MIDDLE */
        case EVBTN_RIGHT:  return 3;   /* SDL_BUTTON_RIGHT  */
        default:           return 0;
    }
}

#endif /* DOS_INPUT_KEYCODES_H */
