# Touch controls for the reference build — implementation plan

Target: the Emscripten reference build in `build/`. Scope decisions taken up front:

| Decision | Choice |
|---|---|
| Codebase | Reference build (C + wasm), not the TS port |
| Movement model | Analog direction, original speed — swap the `shipMv[8]` lookup, keep `ship.speed` |
| Modes | **Normal only** in the first pass; PSY / IKA / GW deferred |
| Orientation | **Landscape first** — see §2, the geometry is not close |

> Note on scope: the project was set as desktop-only at the outset (`../PLAN.md`), so this is a
> deliberate widening. Nothing here changes desktop behaviour — every hook is additive and the
> keyboard path is untouched.

---

## 1. What the code actually does

Findings that drive the design, all verified against the source rather than assumed.

**Movement is a fixed-speed 8-direction table.** `ship.c:104`:

```c
static int shipMv[8][2] = {
  {0,-256}, {181,-181}, {256,0}, {181,181}, {0,256}, {-181,181}, {-256,0}, {-181,-181},
};
...
ship.pos.x += (ship.speed * shipMv[sd][0]) >> 8;   /* ship.c:317 */
ship.pos.y += (ship.speed * shipMv[sd][1]) >> 8;
```

That is a **normalized unit vector in 8.8 fixed point** (256 = 1.0, 181 ≈ 0.707). So substituting an
arbitrary normalized vector is a drop-in: magnitude stays `ship.speed`, only direction becomes
continuous. Every speed behaviour — Normal's focus-slow, PSY's roll — is preserved untouched. This
is the single cleanest insertion point in the codebase and it is why the "analog direction" option
costs almost nothing in fidelity.

**Shooting is also the focus button.** `ship.c:145`:

```c
if ( btn & PAD_BUTTON1 ) {
  ...
  case NORMAL_MODE:
    if ( ship.speed > SHIP_SLOW_SPEED ) ship.speed -= SHIP_SLOW_DOWN;   /* 1000 -> 500 */
```

**This rules out the usual mobile shortcut of always-on autofire**, which would permanently halve
the ship's speed in Normal mode. Fire has to stay a real held control. Worth stating plainly because
"just add autofire and only steer" is the default assumption for a mobile shmup and it is wrong here.

**BUTTON2 means four different things** (`ship.c:175`), which is why the mode scope matters:

| Mode | BUTTON2 | Interaction shape |
|---|---|---|
| Normal | Bomb | Discrete, limited stock, has a cooldown |
| PSY | Rolling (speeds ship up) | **Continuous hold** |
| IKA | Colour change | **Edge-triggered** — `btn2f` latch requires a clean release |
| GW | Reflect | **Hold-to-charge** against a meter |

A first pass covering Normal needs only a tap target. Adding the others later requires a genuinely
hold-capable button with correct press/release edges — design the button that way now even though
only Normal uses it, so the later modes don't force a rewrite.

**All input funnels through two functions**, `getPadState()` and `getButtonState()` in `screen.c`.
`moveShip()`, `moveTitleMenu()`, the gameover/highscore screens and the pause check in `rr.c` all
call them. That is the only place touch has to be merged.

---

## 2. Orientation: landscape, and it isn't close

The canvas is 4:3 (640×480), but the **playfield inside it is 320×480 — portrait 2:3** — flanked by
two 160-wide side boards that carry only HUD (`drawSideBoards`, `screen.c`).

Fitting that 4:3 canvas to a phone:

| Device | Orientation | Playfield | Thumb room each side |
|---|---|---:|---:|
| iPhone 14 | portrait | 195×292 | 98 px |
| iPhone 14 | **landscape** | **260×390** | **292 px** |
| iPhone 14 Pro Max | landscape | 287×430 | 323 px |
| Pixel 8 | landscape | 275×412 | 320 px |

Landscape wins twice over: the playfield is ~45% larger, and the side boards land exactly where
thumbs naturally rest, so controls sit on dead HUD space instead of over the action. Portrait would
need the HUD reflowed out of the side boards to be worth doing — a much bigger job, and phase 3 at
the earliest.

Phase 1 therefore: **landscape only, with a rotate prompt in portrait.**

---

## 3. Architecture

Keep the touch logic in **JavaScript**, in the shell, and hand C a plain state struct.

```
shell.html  touch handling, on-screen buttons, tuning constants
     |  _rr_set_touch(moveX, moveY, moving, btn1, btn2, btnP)   [exported C fn]
     v
touch.c     stores latest state
     |  rrTouchPad() / rrTouchButtons() / rrTouchMoveVector()
     v
screen.c    getPadState() / getButtonState() OR in the touch state
     |
     v
ship.c      moveShip() uses the analog vector when present
```

Why this split rather than doing it all in C with `emscripten_set_touchstart_callback`:

- **Tuning without recompiling.** Deadzone, gain, button geometry and dead-thumb timeouts all want
  a dozen iterations on a real phone. In JS that is an edit-and-reload; in C it is a wasm rebuild
  each time. The C-side API stays fixed while the feel is tuned.
- **Controls render as DOM, not GL.** No changes to the retained-mode batcher, no extra draw calls,
  crisp text at any density, trivial to style and to hide on non-touch devices. Drawing them in GL
  would mean new primitives inside the very code path that caused the earlier rendering bugs.
- The C side stays ~80 lines and has no DOM knowledge.

**Coordinate handling.** `EmscriptenTouchPoint` exposes `targetX/targetY` (canvas-relative), but
since this is *relative* drag only deltas matter, so no inverse projection of the perspective camera
is needed — a real simplification. Express gain as a fraction of canvas CSS height so feel is
identical across screen sizes and the framebuffer cap doesn't affect it.

**One conflict to know about.** Emscripten's SDL already listens for `touchstart`/`touchmove`,
calls `preventDefault()`, and synthesizes mouse events (`libsdl.js`). Nothing in rRootage reads the
mouse, so those synthesized events are inert — but both handlers will fire, and SDL's
`preventDefault` is actually helpful here. Do not try to remove it.

---

## 4. Control scheme (Normal mode)

```
+----------------------------------------------------------+
|  [pause]                                                  |
|              +------------------------+                   |
|              |                        |                   |
|   drag zone  |       PLAYFIELD        |      (BOMB)       |
|   (anywhere  |        320x480         |                   |
|    left of   |                        |      (FIRE)       |
|    centre)   |                        |                   |
|              +------------------------+                   |
+----------------------------------------------------------+
```

- **Movement — relative drag, left half.** Touch anywhere in the left half; the ship moves in the
  direction of the finger's offset from its touchdown point. Relative rather than absolute so the
  finger never has to sit on the ship (fatal in a bullet hell — the thing you must see is under your
  thumb) and so the thumb can re-centre by lifting.
- ~~**Fire — hold, bottom right.**~~ **Superseded:** shipped as autofire with no button, at the
  user's direction. The focus-slow that fire used to provide is replaced by a speed ramp on drag
  distance — see the Touch controls section of `README.md`.
- **Bomb — tap, above fire.** Smaller and deliberately offset so it isn't hit while mashing fire.
- ~~**Pause — small, top left**~~ **Superseded:** no button; two-finger tap (right-click with a mouse).

Multi-touch is tracked by `identifier`: the first finger landing in the drag zone owns movement,
fingers landing on buttons own those buttons, and they operate independently.

~~**Direction, not distance.**~~ **Superseded twice.** First by a speed ramp on drag distance, then
by dropping velocity control altogether: the ship now tracks the thumb 1:1 positionally and stops
when it stops. A velocity model always overshoots, which made tight movement impossible. See the
Touch controls section of `README.md`.

---

## 5. Code changes

| File | Change | Est. |
|---|---|---|
| `build/touch.c`, `touch.h` | **new** — touch state, `EMSCRIPTEN_KEEPALIVE` setter, accessors | ~80 lines |
| `build/screen.c` | OR touch state into `getPadState()` / `getButtonState()`; expose `rrGetMoveVector()` | ~25 lines |
| `build/ship.c` | Use analog vector at line 317 when touch is driving; else existing `shipMv[sd]` | ~15 lines |
| `build/shell.html` | Touch handlers, button DOM/CSS, rotate prompt, tuning constants | ~180 lines |
| `build/Makefile.emcc` | Add `touch.o`; `-sEXPORTED_FUNCTIONS=['_main','_rr_set_touch']` | 2 lines |

The `ship.c` change, concretely — the analog path reuses the existing speed multiply:

```c
int mvx, mvy;                                  /* 8.8 normalized, 256 = 1.0 */
if ( rrGetMoveVector(&mvx, &mvy) ) {           /* touch is driving */
  ship.pos.x += (ship.speed * mvx) >> 8;
  ship.pos.y += (ship.speed * mvy) >> 8;
  ...clamp as now...
} else if ( sd >= 0 ) {
  ...existing shipMv[sd] path, untouched...
}
```

Normalization happens once in JS (`atan2`/hypot in float), so C stays integer and no trig is added
to the fixed-timestep path.

### Menus

`moveTitleMenu()` reads the same pad bits, so the drag zone drives stage selection for free — but it
will feel poor, because the menu is a grid and drag-to-repeat is awkward. Phase 2 should map taps
directly onto the stage grid. Deliberately out of the first pass; the drag path is enough to reach a
game and prove the ergonomics.

---

## 6. Tuning parameters

All in JS, all live-editable. Starting values to be validated on a real device, not trusted:

| Parameter | Start | Rationale |
|---|---|---|
| Deadzone | 6 px | Below finger jitter; too large adds perceived lag |
| Full-deflection distance | 28 px | Only a deadzone test given fixed speed |
| Fire button diameter | 76 px | ~9 mm, comfortably above the ~44 px minimum touch target |
| Bomb button diameter | 56 px | Smaller and offset — mis-hits during fire-mashing are costly |
| Bomb–fire gap | 24 px | Separation matters more than size for accidental presses |
| Control opacity | 0.25 idle / 0.5 active | Visible without competing with bullets |
| Safe-area | `env(safe-area-inset-*)` | Keep controls clear of the notch and home indicator |

---

## 7. Mobile-specific blockers

These are separate from the controls and would each break a phone build on their own.

**Audio on iOS — open question, not a known defect.** The build *does* load and run on an iPhone
(confirmed on-device 2026-07-26), but that says nothing about audio: `loadSounds()` failing is silent
by design, so a working build and a mute build look identical. An earlier draft of this plan asserted
iOS could not decode the Ogg BGM; that was inherited from older Safari behaviour and is **unverified**
— Safari gained Vorbis support around 17.4. See the iPhone status section in `README.md` for how to
settle it with Web Inspector.

~~What *is* a defect regardless:~~ **Done, and the iOS Ogg question is now moot.** `loadSounds()` no
longer bails on the first failure, and the BGM ships as MP3 rather than Ogg — supported by every
browser including all iOS versions. (AAC/`.m4a` does not work with emscripten's preloader; see the
README.) Payload also dropped to 3.66 MB, below the original Ogg-only build.

**Fullscreen is unavailable on iPhone Safari.** The Fullscreen API is not supported for arbitrary
elements there (iPad is fine). The existing fullscreen button will silently no-op. Mitigate with a
web app manifest (`display: standalone`) plus an "Add to Home Screen" hint, and hide the button when
`document.fullscreenEnabled` is false rather than leaving a dead control.

**Fill rate.** The game clears and additively blends the whole frame. `RR_MAX_FB_LONG_EDGE` is
currently 1920, which on a `devicePixelRatio` 3 phone is reached easily. Add a lower cap for
coarse-pointer devices (start at 1280) and measure before assuming it's needed.

**High-refresh phones are already handled** — the timestep fix for 120 Hz displays covers this; see
the timestep section in `README.md`. No further work, but do not regress it.

**Chrome device emulation is not sufficient** for final sign-off: it simulates touch events but not
real finger size, latency, or thermal throttling. Test on hardware over the LAN (`serve.py` binds
all interfaces).

---

## 8. Phasing

**Phase 1 is implemented.** See the "Touch controls" section of `README.md` for what shipped and
what was verified. The table below is the original plan; phase 1's row is kept for the record.

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **0** | ~~non-fatal `loadSounds()`~~ **done**; ~~BGM fallback encode~~ **done (MP3)**; ~~lower FB cap for coarse pointers~~ **done**; confirm on device | Sound confirmed working on iPhone; stable frame rate |
| **1** | ~~Landscape touch: relative-drag movement, fire, bomb, pause. Rotate prompt in portrait~~ **done — not yet played on hardware** | Normal mode completable on a phone |
| **2** | Tap-to-select title/stage menus; hold-capable BUTTON2 for PSY / IKA / GW | All four modes playable |
| **3** | Portrait support with the HUD reflowed out of the side boards | Playable one-handed |

Phase 0 first is deliberate: shipping touch onto a build that might be silent or dropping frames on a
phone means the first round of feedback is dominated by non-touch bugs, and you cannot tell whether
the controls are bad or the build is. It is roughly a day's work and it makes the phase you actually
care about interpretable.

Its audio half may turn out to be smaller than written above — if the BGM already decodes on iOS,
only the non-fatal-loader change is left, which is 20 lines.

---

## 9. Open questions

- **Does relative drag alone give enough precision?** rRootage's hitbox is small and the barrages are
  dense. If phase 1 proves too imprecise, the fallback is a variable-gain curve (fine control near
  the deadzone, faster further out) — this preserves the fixed-speed model but only if gain maps to
  *direction stability*, not speed. Worth prototyping before committing.
- **Should fire default to held or latched?** A latch (tap to toggle) reduces thumb fatigue but
  makes the focus-slow modal and easy to forget. Recommend held, with a latch as an option.
- **Difficulty.** Analog direction is strictly more capable than 8-direction. Scores from touch are
  not comparable to keyboard, which matters if high scores are ever shared. Worth recording the
  input method alongside the score.
