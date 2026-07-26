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
| Audio (3 Ogg BGM + 16 WAV SFX) | works — verified instrumentally, see below |

## Build

Requires `emscripten`. `bml/calc.cpp` is checked in pre-generated, so `bison` is only needed if you
regenerate it from `bml/calc.yy` (macOS system bison 2.3 is sufficient).

```bash
cd build && make -f Makefile.emcc
```

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
  doc-comment links in emscripten's runtime, not fetched).

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

Two caveats:

- **`Mix_QuerySpec` aborts** in emscripten and is skipped under `#ifdef __EMSCRIPTEN__` (see below).
- **Ogg is a Chrome-shaped assumption.** `loadSounds()` bails out on the *first* failed file and sets
  `useAudio = 0`, so in a browser that cannot decode Ogg Vorbis, the BGM failing would silently take
  all 16 sound effects down with it. That is fine for a reference oracle running in Chrome; the
  TypeScript port should ship Opus-in-WebM with an AAC fallback instead, as `../PLAN.md` says.

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
