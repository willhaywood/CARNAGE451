/*
 * Touch input state. See touch.h for the split of responsibilities with the
 * shell's JavaScript.
 */
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include "screen.h"
#include "rr.h"
#include "ship.h"
#include "touch.h"

/* Written only by rr_set_touch, read only by the accessors. Single-threaded,
   and the setter runs from a DOM event between frames rather than during one,
   so no tearing is possible. */
static int tAccX = 0, tAccY = 0;     /* finger travel since touchdown, field units */
static int tDirX = 0, tDirY = 0;     /* 8.8 normalized offset direction, menus only */
static int tTouching = 0;
static int tTap = 0;
static int tBomb = 0, tPause = 0, tMenu = 0;

/* Snapshot of ship.pos taken on the touch-down edge; the target is this plus
   the accumulated travel. */
static int tBaseX = 0, tBaseY = 0;
static int tHaveBase = 0;

EMSCRIPTEN_KEEPALIVE
void rr_set_touch(int accX, int accY, int dirX, int dirY,
                  int touching, int tap, int bomb, int pause, int menu) {
  if (touching && !tTouching) {
    /* Touch-down edge: anchor the target to wherever the ship currently is, so
       the first move is relative and the ship never jumps to the finger. */
    tBaseX = ship.pos.x;
    tBaseY = ship.pos.y;
    tHaveBase = 1;
  } else if (!touching) {
    tHaveBase = 0;
  }
  tAccX = accX; tAccY = accY;
  tDirX = dirX; tDirY = dirY;
  tTouching = touching; tTap = tap;
  tBomb = bomb; tPause = pause; tMenu = menu;
}

int rrTouchTarget(int *x, int *y) {
  if (!tTouching || !tHaveBase) return 0;
  *x = tBaseX + tAccX;
  *y = tBaseY + tAccY;
  return 1;
}

int rrTouchAutofire(void) {
  return tTouching && status == IN_GAME;
}

int rrTouchPad(void) {
  int pad = 0;
  if (!tTouching) return 0;

  /* Quantize the raw offset direction to 8 ways for the menu screens.
     cos(67.5 deg) * 256 ~ 98. */
  if (tDirX >  98) pad |= PAD_RIGHT;
  if (tDirX < -98) pad |= PAD_LEFT;
  /* Screen y grows downward and so does ship.pos.y, so negative is up. */
  if (tDirY < -98) pad |= PAD_UP;
  if (tDirY >  98) pad |= PAD_DOWN;
  return pad;
}

int rrTouchMenu(void) {
  return tMenu;
}

int rrTouchButtons(void) {
  int btn = 0;
  /* Autofire, but only in play: the menus read the same bit, so firing while
     merely touching would select an entry the instant the screen is touched
     and make navigation impossible. Menus get the tap pulse instead. */
  if (rrTouchAutofire()) btn |= PAD_BUTTON1;
  if (tTap)   btn |= PAD_BUTTON1;
  if (tBomb)  btn |= PAD_BUTTON2;
  if (tPause) btn |= PAD_BUTTONP;
  return btn;
}

#endif /* __EMSCRIPTEN__ */
