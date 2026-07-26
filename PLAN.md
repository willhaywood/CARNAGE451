# CARNAGE451 — Technical Plan

A faithful web port of [rRootage](https://github.com/abagames/rrootage) (Kenta Cho / ABA Games, 2003).

**Scope decisions:** desktop browser only · faithful port of all four modes · TypeScript rewrite (not Emscripten).

> Touch/mobile was later explored for the reference build — see [reference/TOUCH-PLAN.md](reference/TOUCH-PLAN.md). Two findings transfer to the TS port regardless of whether touch ships: the ship's movement is a *normalized* 8.8 unit-vector table (`shipMv`) scaled by `ship.speed`, so analog direction is a drop-in that preserves every speed mechanic; and in Normal mode the fire button is also the focus-slow, which rules out always-on autofire.

---

## 1. What the original actually is

Source is small — ~5,000 lines of C/C++ excluding vendored headers:

| File | Lines | Role |
|---|---:|---|
| `boss.cc` | 1003 | Boss assembly, barrage selection, rank scaling — **the procedural core** |
| `screen.c` | 1000 | All rendering. Every GL call in the codebase lives here |
| `attractmanager.c` | 491 | Title / demo / high-score screens |
| `foe.cc` | 437 | Enemy + bullet entities (bullets *are* foes) |
| `ship.c` | 398 | Player, incl. the four mode behaviors |
| `rr.c` | 301 | Main loop, state machine |
| `frag.c` | 231 | Explosion debris |
| `background.c` | 185 | 3D starfield / terrain |
| `mt19937int.c` | 106 | Mersenne Twister |
| `barragemanager.cc` | 64 | Loads barrage XML from disk (just a loader) |

Plus **libBulletML** (vendored as `libbulletml.a`, headers only in the repo) — parser + runner. This is *not* in the line count above and is the single largest piece of net-new porting work.

**Content:** 68 barrage XML files across six directories — `normal` (18), `morph` (14), `morph_heavy` (12), `simple` (12), `reversible` (11), `psy` (1).

---

## 2. Recommended stack

| Layer | Choice | Notes |
|---|---|---|
| Language | TypeScript, `strict` | The BulletML interpreter is where bugs hide |
| Build | Vite | Static output, fast HMR |
| **Render** | **three.js** | Perspective camera — see §3. Not Pixi |
| Post | `EffectComposer` + `UnrealBloomPass` | Reproduces the additive glow bloom |
| Sim | Plain TS, fixed-size arrays | Port the original's allocation model literally |
| Audio | Web Audio API direct | No wrapper library needed |
| Input | Keyboard + Gamepad API | Polled inside the fixed step |
| Test | Vitest | Interpreter + RNG + generator are pure and testable |
| Deploy | Static host | No server component |

---

## 3. The renderer is 3D — this is the critical constraint

`screen.c` sets up a **perspective camera**, not an ortho 2D view:

```c
gluPerspective(45.0f, aspect, 0.1f, 720.0f);       // FAR_PLANE 720
gluLookAt(0, 0, zoom, x, y, 0, 0.0f, 1.0f, 0.0f);  // zoom = 15
```

Shapes are drawn with real 3D transforms — `glRotatef(d2*360/1024, 1, 0, 0)` tilts bullets about the X axis, the background has genuine depth, and screen shake displaces the *eye position* rather than blitting an offset. A 2D renderer cannot reproduce this look; it would flatten every rotating bullet and kill the parallax.

Mapping is direct:

| Original | three.js |
|---|---|
| `gluPerspective(45, a, 0.1, 720)` | `PerspectiveCamera(45, a, 0.1, 720)` |
| `gluLookAt(0,0,15, x,y,0, up)` | `camera.position.set(0,0,15)` + `camera.lookAt(x,y,0)` |
| `glBlendFunc(GL_SRC_ALPHA, GL_ONE)` | `THREE.AdditiveBlending` |
| `GL_LINES` / `LINE_LOOP` / `LINE_STRIP` | `LineSegments` / `LineLoop` / `Line` |
| `GL_TRIANGLE_FAN` | `Mesh` with fan-ordered index buffer |
| `glOrtho(0,640,480,0,-1,1)` (HUD only) | Second `OrthographicCamera` pass |

`glLineWidth(1)` throughout — so plain `LineBasicMaterial` is sufficient. **No need for `Line2`/fat lines**, which is a significant simplification (WebGL ignores `linewidth`, and the fat-line workaround is intrusive).

**Instancing:** the original is immediate-mode, one matrix push per bullet. Naively translating that gives one draw call per bullet. Batch by shape type into `InstancedMesh` / instanced `LineSegments` with a per-instance matrix attribute — roughly a dozen instanced batches total, one per primitive in `screen.h`.

---

## 4. Determinism — the heart of a faithful port

`createBoss(int seed, double rank, int round)` means **a boss is a pure function of its seed**. Reproducing that exactly requires three things:

**a) Port MT19937 bit-exactly.** `mt19937int.c` is 106 lines. The consumers are macros in `genmcr.h`:

```c
#define randN(N)   ((int)(nextRand()>>5) % (N))
#define randNS(N)  (((int)(nextRand()>>5)) % (N<<1) - N)
```

`>>5` caps the value at 2²⁷ so the `(int)` cast never goes negative — in TS use `(next() >>> 5) % N` and there's no signedness trap. Golden-vector test against the C build on day one.

**b) Fix the barrage file ordering.** This is the subtle one. `barragemanager.cc` populates its arrays via `opendir`/`readdir`, then `boss.cc` selects with:

```c
at->barrageIdx = randN(barragePatternNum[at->barrageType]);
```

The index means nothing without the enumeration order. **`readdir` order is filesystem-dependent, so even the original isn't reproducible across machines.** We must pick one canonical order (alphabetical by filename) and freeze it in a generated manifest. Any "same level = same boss" guarantee is relative to *our* chosen ordering, not to a specific historical build. Worth stating in the README rather than implying bit-identity with a 2003 binary.

**c) No `Math.random` anywhere.** One seeded generator, threaded explicitly.

**Rank scaling** (`setAttackRank`, boss.cc:149) derives `rank`, `speedRank`, and `morphRank` through chained `randN` calls — order of evaluation matters, so port statement-by-statement rather than "cleaning it up".

---

## 5. Simulation

**Fixed timestep, 60Hz, decoupled render with interpolation.** Non-negotiable: difficulty is frame-counted and BulletML `wait` is measured in frames. Clamp the accumulator so a backgrounded tab doesn't spiral on resume.

The original already works this way — `rr.c` runs `move()` once per whole 16ms of elapsed wall time, clamped at 5 steps. Two things the reference build proved matter in a browser: **never force a step when one isn't due** (rAF fires every ~8.3ms at 120Hz, so forcing one step per callback runs the game ~2x fast — this exact bug appeared in the reference port), and the original has **no interpolation**, so it judders on refresh rates that aren't multiples of 62.5Hz. The TS port should interpolate, which the original never needed at a locked 60Hz.

**Port the allocation model literally.** `foe.cc` is:

```c
#define FOE_MAX 1024
static Foe foe[FOE_MAX];
static int foeIdx = FOE_MAX;   // scans downward, wrapping
```

A fixed 1024-slot array with a descending rotating cursor. **The 1024 cap is gameplay-visible** — when the array is full, new bullets silently fail to spawn, which caps barrage density in a way patterns were tuned around. Reproduce the cap *and* the scan direction; don't "fix" it with a growable pool.

This also means perf is a non-issue. 1024 entities at 60Hz is trivial in TS — no need for structure-of-arrays, worker threads, or WASM. Pre-allocate the array, never allocate in the loop, and move on.

---

## 6. BulletML

Keep the XML format. `DOMParser` is built in, so parsing costs zero dependencies and all 68 original files load unmodified. Compile to a flat instruction array at load time so the runtime walks a compact structure rather than DOM nodes.

Semantics to get right:
- `wait` suspends an action across frames — the runner is a resumable coroutine per bullet. Model as an explicit stack machine (program counter + frame), not JS generators; it's easier to snapshot and debug.
- `$rank` is the difficulty parameter injected per-barrage from `Barrage.rank` — the primary scaling knob.
- `$1`, `$2`… parameterized `actionRef`/`bulletRef`/`fireRef` need a parameter stack.
- `changeDirection` / `changeSpeed` interpolate over a `term` in frames.

[bulletml.js](https://github.com/daishihmr/bulletml.js) is ES5 and unmaintained — read it as a reference implementation, don't depend on it.

**Morphing** is rRootage-specific: `boss.cc` hands each battery up to `MORPH_PATTERN_MAX` (8) additional parsers, and bullets transition between patterns mid-flight. This lives in the foe/runner layer, not in stock BulletML.

---

## 7. Audio

19 files: 16 `.wav` SFX + 3 `.ogg` BGM (`stg_a/b/c`).

- **Re-encode the BGM.** Safari's Ogg Vorbis support is unreliable; ship Opus-in-WebM with an AAC/m4a fallback. In the TS port this is free — `<audio>` with multiple `<source>` elements negotiates per browser and only fetches one. Note the reference build could *not* use AAC: emscripten's asset preloader decodes only `.ogg`/`.wav`/`.mp3`, and silently hands back a working-looking handle for anything else. That constraint is emscripten's, not the browser's, so it does not apply to the TS port.
- WAVs decode natively everywhere — leave them alone.
- One `AudioContext`, resumed on first input gesture (browsers block autoplay).
- Schedule on the audio clock, never `setTimeout`.
- `soundmanager.c` is 148 lines and maps almost 1:1 onto `AudioBufferSourceNode`.

---

## 8. Licensing

`LICENSE.txt` is **BSD 2-clause**, "Copyright 2003 Kenta Cho. All rights reserved." Redistribution in source and binary form is permitted with the notice retained. Shipping the code, sounds, and barrage data is fine — retain the notice and attribute clearly.

One nuance worth a decision: several barrage files are community transcriptions of patterns from commercial arcade games, and say so in their filenames — `[Guwange]_round_2_boss_circle_fire.xml`, `[ketsui]_r4_boss_rb_rockets.xml`, `[daiouzyou]_r1_boss_1.xml`, `[Progear]_round_3_boss_wave_bullets.xml`, `[Psyvariar]_X-A_boss_opening.xml`. Cho's BSD grant covers his own work. These have been redistributed for two decades without issue and bullet patterns are weak copyright subject matter, but a public web deployment carrying third-party game titles in asset filenames is a different exposure profile than a 2003 freeware zip. Cheapest mitigation if it matters: keep the files, rename to neutral slugs, credit the transcribers in a `CREDITS` file.

---

## 9. Build order

1. ~~**Emscripten reference build.**~~ **Done and playable** — see [reference/README.md](reference/README.md). Compiles to wasm, loads all 68 barrages through real libBulletML, and renders boss, ship, bullets, lasers, explosions, background and HUD; simulation runs clean with assertions on. Three bugs mattered: emscripten's `gluLookAt` is a no-op (argument-order bug against its bundled gl-matrix), which left the camera at the origin so everything at z=0 fell inside the near plane; its immediate-mode emulation cannot take a mid-primitive attribute change, which needed the retained-mode batcher; and reverting the pspdev fork's `float` API to `double` left libBulletML's lexer reading literals with `sscanf("%f")` into a `double`, so every formula evaluated to 0 and every bullet had speed 0. All fixed in `reference/build/`. Two things this feeds into step 4: the renderer really is only ~20 primitives wide, and batching is unavoidable on any GL-derived target.
2. **MT19937 + golden vectors.** Smallest piece, everything downstream depends on it.
3. **BulletML parser → instruction array, + runner.** Pure logic, no rendering. Test headlessly: run a barrage for N frames, snapshot bullet positions.
4. **three.js primitive layer.** Reimplement `screen.h`'s ~20 draw calls, instanced. Rendering is fully isolated in `screen.c`, so this is a clean seam — game logic never touches GL.
5. **Vertical slice:** one boss, `NORMAL_BARRAGE` only, player movement + shot, collision. Proves the loop.
6. **`boss.cc` generation** — morphing, rank scaling, battery groups.
7. **Modes:** Normal → PSY → IKA → GW. Each is a localized `ship.c` variation.
8. **Shell:** attract mode, high scores (localStorage), pause, settings.

---

## 10. Open items

- Fullscreen/resize: original maintains 640×480 aspect with letterboxing (`resized()`). Match, or allow wider FOV? Wider changes difficulty — bullets become visible earlier. Resolved for the reference build: it now renders at `devicePixelRatio` and letterboxes to 4:3, so sharpness and FOV are independent knobs — see [reference/README.md](reference/README.md#resolution-and-sharpness). The TS port should size its three.js renderer the same way (`setPixelRatio`) rather than rendering at a fixed 640×480.
- Original has a `lowres` 320×240 mode. Drop, or keep as a rendering-scale option?
- Replay/ghost data isn't in the original. Deterministic sim makes it nearly free later — worth keeping input handling replay-shaped from the start.
