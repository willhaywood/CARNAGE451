/*
 * SDL 1.2 -> emscripten compatibility shims.
 *
 * Emscripten ships SDL 1.3 headers (SDL_MAJOR_VERSION 1, SDL_MINOR_VERSION 3)
 * but its JS implementation still exports several 1.2 entry points that 1.3
 * dropped from the public headers. Declaring them here is enough to link.
 *
 * Force-included via -include; not referenced by the original sources.
 */
#ifndef EMSCRIPTEN_COMPAT_H_
#define EMSCRIPTEN_COMPAT_H_

#ifdef __EMSCRIPTEN__

#include "SDL.h"
#include <emscripten/html5.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented in libsdl.js as an alias for SDL_GetKeyboardState, and indexed
   by SDLK_ keycode (not SDL2 scancode), which is what rrootage assumes. */
extern DECLSPEC Uint8 * SDLCALL SDL_GetKeyState(int *numkeys);

#ifdef __cplusplus
}
#endif

#endif /* __EMSCRIPTEN__ */
#endif
