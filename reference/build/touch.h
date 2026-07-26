/*
 * Touch input state, fed from JavaScript.
 *
 * The shell owns all touch handling and pushes the result here via
 * rr_set_touch(), so the tuning can be adjusted and reloaded on a device
 * without a wasm rebuild.
 *
 * Everything is additive: with no touch active these all report "nothing
 * pressed", so the keyboard and joystick paths behave exactly as before.
 */
#ifndef TOUCH_H_
#define TOUCH_H_

#ifdef __EMSCRIPTEN__

/* PAD_UP/DOWN/LEFT/RIGHT synthesized from the drag direction, for the menu
   screens, which are written against the d-pad bits. Not used in play. */
int rrTouchPad(void);

/* PAD_BUTTON1 / PAD_BUTTON2 / PAD_BUTTONP. BUTTON1 is autofire while a finger
   is down in play, or a short pulse from a menu tap. */
int rrTouchButtons(void);

/*
 * POSITIONAL steering, not velocity.
 *
 * The shell reports the finger's accumulated travel since touchdown, already
 * converted to field units. C snapshots ship.pos on the touch-down edge and
 * treats base + accumulated as a target, then moves the ship toward it, capped
 * at ship.speed per frame.
 *
 * This is what makes tight movement possible: the ship goes exactly as far as
 * the thumb did and then stops. A velocity model (hold an offset, keep moving)
 * cannot do that -- it always overshoots, because stopping requires returning
 * the finger to an invisible origin.
 *
 * Accumulated travel rather than per-frame deltas so it is idempotent: C can
 * read it any number of times per frame, or miss a push, without drifting.
 *
 * Returns 0 when touch is not steering.
 */
int rrTouchTarget(int *x, int *y);

/* True while autofire is driving BUTTON1. ship.c uses this to suppress the
   Normal-mode focus-slow, which under positional control would only lower the
   rate cap and make the ship lag behind the finger. */
int rrTouchAutofire(void);

#endif /* __EMSCRIPTEN__ */
#endif
