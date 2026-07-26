# rRootage — Emscripten reference build

A WebAssembly build of the original 2003 C/C++ [rRootage](https://github.com/abagames/rrootage),
intended as a **playable oracle** to A/B the TypeScript port against — not as a shipping artifact.

## Status

Working. Boss, player ship, bullets, lasers, explosions, background, HUD and fonts all render;
barrages fire on correct trajectories; music and sound effects play; the simulation runs; no aborts
with assertions enabled.

| Area | State |
|---|---|
| Compile + link to wasm | works |
| libBulletML (built from source) | works — all 68 barrage XMLs parse |
| Asset preload, `opendir`/`readdir` | works |
| Title / attract screen | works |
| Background, HUD, score, fonts, textures | works |
| Boss, ship, bullets, lasers, explosions | works |
| Bullet trajectories / BulletML formulas | works |
| Simulation (rank, collision, scoring, boss HP) | works |
| Audio (3 MP3 BGM + 16 WAV SFX) | works — verified instrumentally, see below |
| Touch controls (landscape, Normal mode) | implemented — not yet played on hardware |

## Build

Requires `emscripten`. `bml/calc.cpp` is checked in pre-generated, so `bison` is only needed if you
regenerate it from `bml/calc.yy` (macOS system bison 2.3 is sufficient).

```bash
cd build && make -f Makefile.emcc
```

The page is generated from `build/shell.html` via `--shell-file`. **Edit that, not `rr.html`** —
`rr.html` is build output and is overwritten every time.

Serve it with the bundled no-cache server and open `rr.html`:

```bash
python3 serve.py          # serves ./build on :8321
```

**Use this rather than `python3 -m http.server`.** Browsers happily reuse a cached `rr.wasm` and
`rr.data` across rebuilds, and because emscripten's output filenames never change, the page then
silently runs the *previous* build. That looks exactly like a code change having no effect — it cost
real time here twice before being spotted. `serve.py` sends `no-store` on every response.

Controls: arrows move, `Z` shoot, `X` bomb, `P` pause. `Z` also starts the game from the title
screen; there, the arrows pick the stage and `X` cycles the mode.

> Keyboard input must reach `document`. Synthetic `KeyboardEvent`s work; some automation harnesses'
> key injection does not reach emscripten's SDL listener.

## The three bugs that mattered

Everything under "Other porting changes" was mechanical. These three were not. The first two
accounted for the entire rendering failure; the third made every bullet motionless.

### 1. Emscripten's `gluLookAt` is a no-op

This is a bug in emscripten, not in rrootage. `src/lib/libglemu.js` calls:

```js
GLImmediate.matrixLib.mat4.lookAt(GLImmediate.matrix[GLImmediate.currentMatrix],
                                  [ex, ey, ez], [cx, cy, cz], [ux, uy, uz]);
```

but the gl-matrix it bundles (`src/gl-matrix.js`) declares the *old* signature:

```js
mat4.lookAt = function (eye, center, up, dest)
```

So the current matrix is passed as `eye`, the eye as `center`, the centre as `up`, and the up vector
is used as `dest` — a three-element scratch array that receives the result and is discarded. The
modelview matrix is never modified. Compare `gluPerspective` immediately above it in the same file,
which assigns its result back and works correctly.

The failure mode is nasty because it looks like a partial rendering bug rather than a broken camera.
`setEyepos()` intends to place the camera at z = +15; instead it stays at the origin. Geometry at
negative z — the background planes at z = −10 — is still in front of the camera and renders fine, so
the screen looks alive. But the ship, every bullet, and every other object drawn at **z = 0** lands
exactly on the eye point, inside the 0.1 near plane, and disappears.

Confirmed by dumping `GL_MODELVIEW_MATRIX` at draw time: identity before the fix, correct
`-15` translation after.

`glu_compat.c` now defines a correct `gluLookAt`. A C definition takes precedence over emscripten's
JS library version, which is only linked for otherwise-undefined symbols.

### 2. Immediate mode can't take a mid-primitive attribute change

Emscripten's `LEGACY_GL_EMULATION` infers a vertex stride from the attributes it has seen and aborts
on `Assertion failed: numVertices must be an integer` (`numVertices = 4 * vertexCounter / stride`
in `libglemu.js`) when an attribute first appears partway through a `glBegin`/`glEnd`.

Four routines in `screen.c` do exactly that — they set a colour, emit a vertex or two, then change
the colour for the remaining vertices to make a gradient across a `GL_TRIANGLE_FAN`: `drawShape`
(7 blocks), `drawShapeIka` (2), `drawShot` (1), `drawCircle` (1).

`screen.c` now redirects `glBegin`/`glEnd`/`glVertex*`/`glColor4ub`/`glTexCoord2f` into plain arrays
and submits each primitive with `glDrawArrays` (the `__EMSCRIPTEN__` block at the top of the file).
Every vertex carries an explicit colour, so the layout is fixed for the whole primitive. `GL_QUADS`
is remapped to `GL_TRIANGLE_FAN` — the single quad block is one four-vertex rectangle, and
emscripten's `glDrawArrays` path excludes `GL_QUADS` explicitly. Only vertex submission changes;
matrices, blending and textures are untouched.

Two narrower fixes were tried first and abandoned: re-issuing the active colour just after `glBegin`
in the affected blocks (insufficient), and applying that re-issue globally (made it abort *earlier*).
`-sGL_UNSAFE_OPTS=0` had no effect.

**Do not "fix" this with `-sASSERTIONS=0`.** It runs, but the fractional vertex count is then used
silently and the affected geometry never draws — a correct simulation against an empty playfield.

### 3. `sscanf("%f")` writing into a `double` — every bullet frozen

Self-inflicted, from the `float` → `double` reversion described under libBulletML below.

libBulletML's expression grammar (`bml/calc.yy`) sets `#define YYSTYPE float` upstream and its lexer
reads numeric literals with:

```c
sscanf (yyinStr, "%f", &yylval);
```

Reverting the pspdev fork's `float` API back to `double` changed `YYSTYPE` to `double`, but a blanket
`s/float/double/` does not touch a `%f` format specifier. `sscanf` then wrote 4 bytes through an
8-byte lvalue, so every numeric literal in every BulletML formula decoded as garbage — in practice a
denormal reading as 0.

The symptom was bullets stuck on the boss with no trajectory: `<speed>0.7</speed>` evaluated to 0,
and `createSimpleBullet` does `(int)(speed * 512)`, so the bullet got integer speed 0 and never
moved. Instrumenting bullet creation showed 100% of simple bullets at speed 0, and active bullets at
*only* exactly 0.0 or 1.0 — 1.0 being `getDefaultSpeed()`'s fallback, never a parsed value. Probing
`calc()` directly then showed `"1"` → 0, `"0.7"` → 0, `"90"` → 0 while `$1` → 1 and operators still
worked, which pinned it to number tokens specifically.

Fixed to `%lf`. `bml/calc.yy` is now checked in carrying both the `double` conversion and this fix,
and `bml/calc.cpp` is regenerated from it, so regenerating cannot silently reintroduce the bug.

> Debugging note: do **not** evaluate a formula at parse time to check it. `Random::value()` calls
> `Variables::runner->getRand()`, and `Variables::runner` is null until a runner exists, so a probe
> that prints `val_->value()` from `BulletMLNode::setValue` traps on the first `$rand` it meets
> (`normal/88way.xml`, `<speed>$rand*0.5+0.5</speed>`). That crash is the probe's, not the parser's —
> it cost a detour here.

## Other porting changes

The original `src/Makefile` targets `i686-w64-mingw32`. Beyond swapping the toolchain:

**libBulletML had to be rebuilt from source.** The repo ships `libbulletml.a` prebuilt (MinGW i686
objects: `bulletmltree.o`, `calc.o`, `tinyxml*.o`) and only *headers* under `src/bulletml/`. Sources
came from [pspdev/libbulletml](https://github.com/pspdev/libbulletml), whose API matches rrootage's
vendored headers exactly — except that the fork narrowed the whole API from `double` to `float` for
the PSP. That is reverted in `build/bml/` (`s/float/double/`, excluding tinyxml) — see bug 3 above
for what that reversion missed. `calc.cpp` is pre-generated from `calc.yy` by bison and checked in.
[thejustinwalsh/libbulletml](https://github.com/thejustinwalsh/libbulletml) keeps `double` but
modernised the API (`boost::shared_ptr` → `std::shared_ptr`, renamed members), which rrootage's call
sites do not match.

**`main()` was restructured for `emscripten_set_main_loop`.** The original blocks in `while(!done)`
with `SDL_Delay` pacing, which would stall the browser event loop. The loop body is now
`mainLoopIteration()`, driven by `requestAnimationFrame`; `SDL_Delay` is compiled out. `event` became
file-scope `static` to preserve the original's cross-iteration persistence.

**`emscripten_compat.h`** declares `SDL_GetKeyState`. Emscripten ships SDL *1.3* headers, which
dropped it in favour of `SDL_GetKeyboardState` — but its JS implementation still exports
`SDL_GetKeyState`, and indexes the state array by `SDLK_` keycode, exactly as rrootage assumes. Only
the declaration was missing.

**`glu_compat.c`** also implements `gluBuild2DMipmaps`, which emscripten's GLU does not provide.
WebGL 1 refuses to mipmap non-power-of-two textures, and `title.bmp` is 150×36, so NPOT images get
`GL_LINEAR` + `CLAMP_TO_EDGE` instead.

**`loadGLTexture`** derives the pixel format from `surface->format->BytesPerPixel`. With
`--use-preload-plugins`, `SDL_LoadBMP` goes through the browser's decoder and returns 32-bit RGBA;
the hardcoded `GL_RGB` would misread rows and overrun the last one.

**`Mix_QuerySpec` is skipped.** Emscripten's SDL_mixer aborts on it. Its outputs were written into
locals that are never read.

## Page shell

`build/shell.html` replaces emscripten's stock template, which shipped an emscripten logo and
banner, a console `<textarea>`, and unstyled checkboxes. The replacement is a dark, self-contained
page — no external fonts, scripts, or images, so nothing extra to fetch and nothing to break the CSP
on a static host.

- **Loading state**: wordmark, progress bar driven by `monitorRunDependencies` (indeterminate sweep
  until byte counts arrive), fading out when the runtime is ready. `window.onerror` swaps it for a
  readable failure message instead of leaving a black rectangle.
- **Console output is gone from the page.** `Module.print`/`printErr` now go to the devtools console
  only. The barrage filenames the game prints at startup are diagnostics, not UI.
- **Click-to-play gate**, because browsers need a trusted gesture before audio starts. It doubles as
  the thing that puts keyboard focus on the canvas for SDL. Dismissed by click *or* keypress.
- **Controls legend and a fullscreen button**, revealed on hover so they stay out of the way.
- Arrow keys and space are `preventDefault`ed so the page can't scroll under the game.

### Canvas sizing contract

Layout is owned entirely by CSS: the canvas is 100% of a 4:3 `#frame` sized to fit the viewport.
`syncCanvasToDisplay()` in `screen.c` only *reads* that size back and matches the framebuffer to it
(x `devicePixelRatio`, capped — see below). **Do not set width/height attributes on the canvas in the
shell**; the two would fight.

The fullscreen rule sizes the *canvas* to 4:3 inside a full-screen frame rather than sizing the frame
itself to `100vw/100vh` — the latter overrides `aspect-ratio` and hands the canvas a non-4:3 box. The
game's own viewport letterboxing hides that, but only by drawing a second set of bars.

The shell also bypasses `Module.requestFullscreen` in favour of `frame.requestFullscreen()`, because
emscripten's version rewrites `canvas.width`/`height` and fights the sizing above.

Verified: 4:3 preserved and no page scrolling at 375x812 (mobile) through 1600px-wide desktop;
960x720 CSS / 1920x1440 backing store at `devicePixelRatio` 2.

### A note on `-sASSERTIONS`

Left at `1` deliberately. Turning it off saves 43 KB of `rr.js` (8% of that file, but only 0.9% of
the 4.6 MB payload) — not worth it here, because assertions are what make the immediate-mode GL
regression described above fail loudly instead of silently drawing nothing. Flip it in
`Makefile.emcc` if you disagree; the tradeoff is one line.

## Touch controls

Landscape, Normal mode. **Verified in a desktop browser with synthetic pointer events; not yet
played on a phone.**

### Scheme

- **Move — drag anywhere, 1:1 with the thumb.** No joystick and no fixed zone; the whole screen is
  the surface. The ship travels exactly as far as the finger does and **stops the instant the finger
  stops**. It is never required to sit on the ship, which in a bullet hell would hide the one thing
  you must see.
- **Fire — automatic** while a finger is down. No button.
- **Bomb — the only visible control**, in the dead space right of the playfield.
- **Pause — two-finger tap.** Fires when the second finger lands rather than on release, so it
  responds immediately. The bomb button stops propagation, so reaching for it never counts as the
  second finger.
- **Speed comes from drag distance**, not just direction. See below — this is load-bearing.

### Positional, not velocity — this is the whole thing

The first attempt was a velocity model: hold the finger offset from where it landed, and the ship
keeps moving in that direction. **That cannot do tight movement.** Stopping requires returning the
finger to an invisible origin, so every small correction overshoots. It was rejected in play for
exactly that reason.

What ships instead is positional. The shell integrates the finger's travel and reports the
accumulated total, converted to field units. `touch.c` snapshots `ship.pos` on the touch-down edge
and treats base + accumulated as a **target**; `ship.c` closes the gap to it, capped at `ship.speed`
per frame. When the thumb stops, the gap is zero and so is the movement.

Accumulated travel rather than per-frame deltas, so it is idempotent: C can read it any number of
times per frame, or miss a push entirely, without drifting.

Measured, in game: a 40px drag moved the ship 6259 field units against 6259 expected — exact 1:1,
40.0px on screen — and after 40 further frames holding still the drift was **0 units**.

`TOUCH.gain` is 1.0 (true 1:1). Raise it for faster traversal at the cost of fine control. The
`ship.speed` clamp still applies, so a fast flick is followed at the ship's own maximum rather than
teleporting.

The pixel→field factor is one number for both axes: the playfield is the centre half of the 4:3
canvas and 320×480 field units in 8.8, which works out to `163840 / canvasCssWidth` either way.

### Why the focus-slow is suppressed

In Normal mode `PAD_BUTTON1` is *both* fire and the focus-slow (`ship.c`): holding it drops the ship
from `SHIP_SPEED` 1000 to `SHIP_SLOW_SPEED` 500. Autofire holds that button permanently.

Under positional control `ship.speed` is only a rate ceiling — precision comes from the finger — so
leaving the focus-slow active would just halve the ceiling and make the ship lag behind the thumb.
`rrTouchAutofire()` therefore suppresses it. This is the only divergence from the original input
model.

### Why autofire is gated on IN_GAME

The menus read the same `PAD_BUTTON1`. Firing merely because a finger is down would select an entry
the instant the screen was touched, making navigation impossible. `rrTouchButtons()` therefore only
adds BUTTON1 from autofire when `status == IN_GAME`; menus are driven by a separate short **tap**
pulse (press under 250ms that moved less than 12px, held for 140ms so the fixed-step sim can't miss
it) plus the synthesized d-pad from dragging.

### Testing it with a mouse

Pointer Events already deliver mouse input to the same handlers, so the whole scheme can be exercised
on a desktop — only the UI needed a way to appear on a device that reports a fine pointer.

- **`?touch=1`** on the URL, or the **Touch test** button in the toolbar.
- Left-drag anywhere = move (and autofire, exactly as a finger does).
- **Right-click = pause**, standing in for the two-finger tap. The context menu is suppressed on the
  drag surface.

In this mode the keyboard legend stays visible so the toggle remains reachable; on a real touch
device it is hidden as before.

### Architecture

All touch handling is in `shell.html`, pushed into C through one exported function:

```c
rr_set_touch(mx, my, moving, touching, tap, bomb, pause)
```

`screen.c` ORs the result into `getPadState()`/`getButtonState()` — both additive, so the keyboard
and joystick paths are untouched and desktop is unaffected. Tuning constants live in the `TOUCH`
object in the shell, so the deadzone, ramp and leash can be adjusted on a device with a reload rather
than a wasm rebuild.

### Not done

- Portrait (needs the HUD reflowed out of the side boards).
- PSY/IKA/GW BUTTON2 semantics — hold, edge-trigger and hold-to-charge respectively. Note that PSY
  and GW also interact with `ship.speed`, so the speed-ramp decision above will need revisiting for
  them.
- Any real-device play test. The deadzone/ramp/leash numbers are starting values, not tuned.

## Deploying

The build is a fully static bundle — `rr.html` (rename to `index.html`), `rr.js`, `rr.wasm`,
`rr.data`, ~4.6 MB total — and works on any plain static host. **No server logic, no special
headers.**

The one thing that would have ruled out GitHub Pages is cross-origin isolation: an emscripten build
that uses threads needs `SharedArrayBuffer`, which requires COOP/COEP response headers that Pages
cannot set. This build is single-threaded — no `-pthread`, and `rr.js` references neither
`SharedArrayBuffer` nor `Atomics` — so that requirement never arises. Verified:

- Payload is fetched with same-origin **relative** requests; `scriptDirectory` resolves against the
  script URL, so a project-page subpath (`user.github.io/CARNAGE451/`) works. Tested by serving from
  a `/CARNAGE451/` subdirectory — loads and runs.
- MIME types from a default static server are correct: `.wasm` → `application/wasm`, `.data` →
  `application/octet-stream` (emscripten fetches it as an ArrayBuffer, so the type is irrelevant).
- No absolute URLs or hardcoded origins in the runtime path (the `http://` strings in `rr.js` are
  doc-comment links in emscripten's runtime, not fetched). The shell is self-contained too — inline
  CSS and JS, an inline SVG favicon, no web fonts — so there are no third-party requests at all.

`.github/workflows/deploy-reference.yml` builds and publishes on push. It adds `.nojekyll` so Jekyll
doesn't mangle the payload, and copies `rr.html` to `index.html`. Enable once under
**Settings → Pages → Source: GitHub Actions**.

Same applies to Netlify, Cloudflare Pages, itch.io (zip upload), or `python3 serve.py` locally. If a
host ever serves `.wasm` as `application/octet-stream`, modern browsers still accept it via
`WebAssembly.instantiate`, but `application/wasm` enables faster streaming compilation.

## Resolution and sharpness

The build originally rendered into a fixed 640x480 canvas backing store, which the browser then
scaled to whatever size the element occupied. On a 2x display that is a doubling even in a window,
and fullscreen stretches 640x480 across the entire screen — hence the blur.

This is **not** an asset-resolution problem. The only bitmaps in the game are two 128x128 glow
sprites and a 150x36 logo; everything else is vector geometry emitted per frame (186 vertex calls in
`screen.c`), so it re-renders sharp at any size for free.

`screen.c` now sizes the canvas to the display's real pixel density:

- The **CSS box** and the **backing store** are set independently. This matters: a canvas with no CSS
  size takes its layout size *from* its backing store, so raising `canvas.width` alone just makes the
  element twice as big at the same density — no sharper. The CSS size is pinned first, then the
  backing store is set to that many device pixels.
- Windowed, the CSS box stays 640x480 (the original window size) and the backing store becomes
  640x480 x `devicePixelRatio`. Fullscreen uses the screen size as the CSS box.
- Re-synced on `resize` and `fullscreenchange`, but **deferred to the top of the next frame**.
  Emscripten's own `requestFullscreen` may also set the canvas size — the shell's "Resize canvas"
  checkbox decides — and both handlers run in the same task, so applying ours inline would make the
  winner depend on listener order.

`screenResized()` already letterboxed the viewport to 4:3 and rebuilt the projection from it, and the
HUD pass uses its own `glOrtho(0,640,480,...)` mapped onto the viewport rather than the framebuffer,
so no game coordinate system had to change.

### Framebuffer cap

Fullscreen at true native density is a lot of pixels — a 2056x1329 CSS screen at
`devicePixelRatio` 2 is 4112x2658, about 11 megapixels. The game clears and additively blends the
whole frame and issues several hundred small primitives per frame, so fill rate is what gives out
first, and vector art gains very little from the last doubling.

`RR_MAX_FB_LONG_EDGE` in `screen.c` (default **1920**) caps the framebuffer's long edge, preserving
aspect. Only the backing store is capped; the CSS box still fills the screen, so the browser scales
the result up.

| | pixels |
|---|---|
| Original fixed 640x480 | 0.31 MPix |
| Capped fullscreen 1920x1241 | 2.4 MPix (7.8x sharper than before) |
| Uncapped native 4112x2658 | 10.9 MPix (4.6x the cost of capped) |

Raise the constant if you have GPU headroom — 2560 or 3840 are reasonable steps.

Verified windowed: 640x480 CSS, 1280x960 backing store, ratio 2.00x on a `devicePixelRatio` 2
display (below the cap, so unaffected). **The fullscreen path is implemented but not verified end to
end** — the automation browser used here would not enter fullscreen.

## Is the game speed tied to frame rate?

**No — it is a wall-clock fixed-timestep accumulator, and that had to be repaired for the browser.**

`rr.c` computes how many whole 16ms steps (`INTERVAL_BASE`) have elapsed and runs `move()` that many
times, so the simulation advances in fixed increments regardless of how often the frame is drawn:

```c
frame = (int)(nowTick - prvTickCount) / interval;   /* interval = 16 */
...
for (i = 0; i < frame; i++) { move(); tick++; }
```

Two consequences worth knowing:

- **Slow frames don't speed the game up** — elapsed time is consumed in whole steps. There is a
  `frame > 5` clamp, so a stall longer than ~80ms *drops* time rather than fast-forwarding; the game
  briefly runs slower than real time instead of spiralling. Confirmed in a throttled tab: 0.1 fps
  still produced ~2 steps per render rather than a burst.
- **There is no interpolation.** Rendering shows the latest whole simulation step, so a refresh rate
  that isn't a multiple of 62.5Hz gives slight judder — not a speed change.

The repair: natively, the `frame <= 0` branch forces one step *and* calls `SDL_Delay` to sleep until
a step is genuinely due — the sleep is what makes forcing a step correct. That delay cannot stay in
the browser, since blocking stalls the event loop. Leaving `frame = 1` behind without it meant one
simulation step per `requestAnimationFrame` callback, which is fine at 60Hz but runs the whole game
**about 2x fast on a 120Hz display** and ~2.3x on 144Hz. It now renders without stepping when a step
isn't due yet, leaving `prvTickCount` alone so the elapsed time carries over.

This was reasoned from the code and verified at 60Hz; it has **not** been tested on a high-refresh
panel, since the browser used here is 60Hz. If you have a 120Hz display, that is worth a look.

## Audio

Works. All 19 files load (`stg_a/b/c.ogg` plus 16 `.wav`), `useAudio` ends up 1, `Mix_PlayMusic`
returns 0 and `Mix_PlayChannel` returns real channel numbers — never `-1`, which is what
`Mix_PlayChannelTimed` returns when a chunk has neither an audio element nor a Web Audio buffer
behind it.

Emscripten's SDL_mixer routes everything through detached HTML `<audio>` elements rather than Web
Audio (`AudioBufferSourceNode.start` is never called; no `AudioContext` is created). Sampling the BGM
element mid-game showed `duration` 66.21 s, `paused: false`, `currentTime` 20.4 s, `volume` 1, not
muted, `readyState: HAVE_ENOUGH_DATA`, no error — i.e. Chrome decoded the Ogg Vorbis and was playing
it. SFX elements reach `ended: true` after their ~1 s duration, and `Mix_HaltMusic` correctly pauses
and rewinds the BGM on game over. Across every `play()` call there were **zero** rejections, so the
autoplay policy is not blocking playback.

This verification is instrumental, not auditory — the checks above establish that decoded audio is
playing at full volume, but nobody listened to it.

### BGM encoding

The three music tracks ship as **MP3 only** (mono, 96 kbps, matching the original Vorbis bitrate).
`loadSounds()` probes `.mp3` then `.ogg` and keeps the first that decodes, so dropping the original
Vorbis files back into `rr/sounds/` works with no code change — they are the better encode, since the
MP3 is a lossy-to-lossy transcode of them. The 16 effects are `.wav`, which every browser decodes, so
they need no fallback.

**Why MP3 and not AAC/`.m4a`, which was the obvious choice and was tried first.** Emscripten's asset
preloader only decodes `.ogg`, `.wav` and `.mp3` (`audioPlugin.canHandle` in
`src/lib/libbrowser.js`). An `.m4a` is packaged but never decoded — and `Mix_LoadMUS` still returns a
**non-NULL handle** for it, so the load reports success and then plays silence. Confirmed by
inspection: with `.m4a` the three `SDL.audios` entries had no duration and no source, against real
durations for everything else. That is precisely the silent-failure mode the loader fix above exists
to eliminate, so it was abandoned. MP3 is decoded by the toolchain and supported by every browser
including all iOS versions, so it buys the same compatibility with none of that.

**Why Ogg was dropped rather than kept as a fallback.** Emscripten packages assets into a single blob
with no content negotiation, so shipping both meant every visitor downloading both encodings —
`rr.data` went from 3.9 MB to 6.1 MB. MP3 alone covers every browser, so the second copy bought
nothing but bytes:

| Packaged | `rr.data` |
|---|---|
| Ogg only (original) | 3.94 MB |
| Ogg + MP3 | 6.12 MB |
| **MP3 only (current)** | **3.66 MB** |

Verified: `Audio: 3/3 music, 16/16 effects loaded`, package manifest contains 16 `.wav` + 3 `.mp3` and
no `.ogg`, and the decoded durations are 66.2 / 37.2 / 77.4 s — matching the sources exactly.

Two caveats:Two caveats:

- **`Mix_QuerySpec` aborts** in emscripten and is skipped under `#ifdef __EMSCRIPTEN__` (see below).
- **The loader is now non-fatal** (it wasn't originally — see below). A file that fails to load is
  skipped, the entry stays NULL, and `playMusic`/`playChunk` skip NULL entries, so a partial load
  degrades to "no music" or "one missing effect" rather than "no audio at all". `useAudio` is cleared
  only if *nothing* loaded.
- The tally is always printed — `Audio: 3/3 music, 16/16 effects loaded` — so a partial load is
  visible instead of silent.

## Note for anyone debugging this further

**Keep the Browser pane visible while testing.** When it is hidden the tab is backgrounded,
`requestAnimationFrame` stops, and `emscripten_set_main_loop` freezes. The canvas keeps showing its
last frame and `printf` output stops, which looks exactly like a render bug. Several readings during
this work were invalidated that way. Confirm the loop is live (a changing score, or a debug counter)
before trusting any negative visual result.

When geometry goes in and nothing comes out, dump `GL_MODELVIEW_MATRIX`, `GL_PROJECTION_MATRIX` and
`GL_VIEWPORT` at draw time before theorising about primitives, colours or batching. That is what
found bug 1, after several wrong guesses that cost more time than the measurement would have.

## Licensing

rRootage is BSD 2-clause, "Copyright 2003 Kenta Cho. All rights reserved." (`rr/LICENSE.txt`).
Redistribution in source and binary form is permitted with the notice retained.

Note that five barrage files under `rr/normal/` and `rr/morph_heavy/` are community transcriptions
of patterns from commercial arcade games and carry those titles in their filenames
(`[Guwange]_…`, `[ketsui]_…`, `[daiouzyou]_…`, `[Progear]_…`, `[Psyvariar]_…`). Cho's grant covers
his own work. See the licensing section of `../PLAN.md`.
