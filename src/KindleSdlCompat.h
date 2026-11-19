#pragma once
// The simulator's input code was written against SDL. The Kindle build has no
// SDL, so this header supplies the handful of SDL names that code still uses:
// key codes (the Paperwhite has no keys, so they are never pressed), the
// keyboard-state array, and the tick counter.

#include <Arduino.h>

#include <cstdint>

using SDL_Scancode = int;
enum : int {
  SDL_SCANCODE_UNKNOWN = 0,
  SDL_SCANCODE_ESCAPE,
  SDL_SCANCODE_RETURN,
  SDL_SCANCODE_LEFT,
  SDL_SCANCODE_RIGHT,
  SDL_SCANCODE_UP,
  SDL_SCANCODE_DOWN,
  SDL_SCANCODE_P,
  SDL_SCANCODE_S,
  SDL_SCANCODE_H,
  SDL_SCANCODE_T,
  SDL_SCANCODE_PAGEUP,
  SDL_SCANCODE_PAGEDOWN,
  SDL_NUM_SCANCODES_COMPAT
};

inline const uint8_t *SDL_GetKeyboardState(int *) {
  static const uint8_t none[SDL_NUM_SCANCODES_COMPAT] = {};
  return none;
}

inline uint32_t SDL_GetTicks() { return static_cast<uint32_t>(millis()); }
inline void SDL_Delay(uint32_t ms) { delay(ms); }
