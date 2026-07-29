/*
 * $Id: screen.c,v 1.6 2003/08/10 03:21:28 kenta Exp $
 *
 * Copyright 2003 Kenta Cho. All rights reserved.
 */

/**
 * OpenGL screen handler.
 *
 * @version $Revision: 1.6 $
 */
#include <stdio.h>
#include <stdlib.h>

#include "SDL.h"

#include <math.h>
#include <string.h>

#include "genmcr.h"
#include "screen.h"
#include "rr.h"
#include "degutil.h"
#include "attractmanager.h"
#include "letterrender.h"
#include "boss_mtd.h"
#include "touch.h"
#include <emscripten.h>

#ifdef __EMSCRIPTEN__
/*
 * Retained-mode replacement for immediate mode.
 *
 * Emscripten's LEGACY_GL_EMULATION implements glBegin/glEnd by inferring a
 * vertex stride from whichever attributes it has seen so far. Several routines
 * below set a colour, emit a vertex or two, then change the colour for the
 * remaining vertices to get a gradient across a GL_TRIANGLE_FAN. The emulation
 * records the leading vertices without a colour component, widens the stride
 * partway through, and its vertex count comes out fractional -- it then aborts
 * on "`numVertices` must be an integer", or, with assertions off, silently
 * draws nothing.
 *
 * Rather than contort the drawing code, glBegin/glEnd are redirected into
 * plain arrays here and submitted with glDrawArrays. Every vertex carries an
 * explicit colour, so the layout is fixed for the whole primitive and the
 * inference problem disappears. This uses the ordinary client-array path,
 * which is well supported, instead of the immediate-mode emulation that
 * emscripten itself warns is "a collection of limited workarounds".
 *
 * Only vertex submission changes. Matrices, blending, textures and the fixed
 * function pipeline are untouched, so gluPerspective/gluLookAt/glPushMatrix
 * and friends continue to work exactly as before.
 *
 * The wrappers are defined ahead of the macros so their own calls reach the
 * real GL entry points.
 */

/* Scratch for the one primitive glBegin/glEnd is currently assembling.
   drawCircle is the largest: a centre vertex plus 16 iterations of two. */
#define RR_PRIM_MAX  64
/* The batch. Flushed on every state change, so it rarely approaches this. */
#define RR_BATCH_MAX 8192

static GLfloat rrPX[RR_PRIM_MAX], rrPY[RR_PRIM_MAX], rrPZ[RR_PRIM_MAX];
static GLubyte rrPC[RR_PRIM_MAX*4];
static GLfloat rrPT[RR_PRIM_MAX*2];
static int     rrPN = 0;
static GLenum  rrPMode = GL_TRIANGLE_FAN;
static int     rrPHasTex = 0;

/* Two batches run concurrently, because a draw call takes one primitive type.
   Everything is decomposed into independent triangles or independent lines so
   that consecutive primitives concatenate instead of each needing its own call. */
static GLfloat rrTV[RR_BATCH_MAX*3]; static GLubyte rrTC[RR_BATCH_MAX*4];
static GLfloat rrTT[RR_BATCH_MAX*2]; static int rrTN = 0, rrTTex = 0;
static GLfloat rrLV[RR_BATCH_MAX*3]; static GLubyte rrLC[RR_BATCH_MAX*4];
static GLfloat rrLT[RR_BATCH_MAX*2]; static int rrLN = 0, rrLTex = 0;

static GLubyte rrCurCol[4] = { 255, 255, 255, 255 };
static GLfloat rrCurTex[2] = { 0.0f, 0.0f };

/* Modelview at the time the primitive began. Vertices are transformed by it on
   the way into the batch, so a glTranslatef between two primitives no longer
   forces them apart -- which is what made every object its own draw call. */
static GLfloat rrMV[16];

/* ?batch=0 falls back to a draw call per primitive, so the batching can be
   compared against the old behaviour on a real device without another build. */
static int rrBatchOn = 1;

static void rrFlushOne(GLfloat *v, GLubyte *c, GLfloat *t,
                       int n, int hasTex, GLenum mode) {
  if (n == 0) return;
  /* Vertices arrive already in eye space, so the modelview must be identity for
     the draw itself. Projection is left alone and still done by GL. */
  glPushMatrix();
  glLoadIdentity();
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, v);
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, c);
  if (hasTex) {
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
  }
  glDrawArrays(mode, 0, n);
  if (hasTex) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glPopMatrix();
}

/*
 * Reordering triangles ahead of lines within a flush is safe, and that is what
 * makes this work at all: blending is additive (GL_SRC_ALPHA, GL_ONE) and the
 * depth test is off, so blended geometry composites commutatively -- the result
 * does not depend on the order it was submitted in. Any state change that would
 * break that assumption flushes first.
 */
static void rrFlush(void) {
  rrFlushOne(rrTV, rrTC, rrTT, rrTN, rrTTex, GL_TRIANGLES);
  rrTN = 0; rrTTex = 0;
  rrFlushOne(rrLV, rrLC, rrLT, rrLN, rrLTex, GL_LINES);
  rrLN = 0; rrLTex = 0;
}

static void rrBegin(GLenum mode) {
  rrPMode   = mode;
  rrPN      = 0;
  rrPHasTex = 0;
  glGetFloatv(GL_MODELVIEW_MATRIX, rrMV);
}

/* Colour is latched, not forwarded: every vertex carries an explicit copy, so
   the fixed-function current-colour state is never consulted. */
static void rrColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
  rrCurCol[0] = r; rrCurCol[1] = g; rrCurCol[2] = b; rrCurCol[3] = a;
}

static void rrTexCoord2f(GLfloat u, GLfloat v) {
  rrCurTex[0] = u; rrCurTex[1] = v;
  rrPHasTex = 1;
}

static void rrPushVertex(GLfloat x, GLfloat y, GLfloat z) {
  int i = rrPN;
  if (i >= RR_PRIM_MAX) return;
  /* Column-major, as GL stores it. */
  rrPX[i] = rrMV[0]*x + rrMV[4]*y + rrMV[8] *z + rrMV[12];
  rrPY[i] = rrMV[1]*x + rrMV[5]*y + rrMV[9] *z + rrMV[13];
  rrPZ[i] = rrMV[2]*x + rrMV[6]*y + rrMV[10]*z + rrMV[14];
  rrPC[i*4+0] = rrCurCol[0]; rrPC[i*4+1] = rrCurCol[1];
  rrPC[i*4+2] = rrCurCol[2]; rrPC[i*4+3] = rrCurCol[3];
  rrPT[i*2+0] = rrCurTex[0]; rrPT[i*2+1] = rrCurTex[1];
  rrPN++;
}

static void rrVertex3f(GLfloat x, GLfloat y, GLfloat z) { rrPushVertex(x, y, z); }
static void rrVertex2f(GLfloat x, GLfloat y)            { rrPushVertex(x, y, 0.0f); }

/* Copy scratch vertex src into the triangle or line batch. */
static void rrEmit(int line, int src) {
  GLfloat *v; GLubyte *c; GLfloat *t; int n;
  if (line) {
    if (rrLN >= RR_BATCH_MAX) return;
    v = rrLV; c = rrLC; t = rrLT; n = rrLN++;
    if (rrPHasTex) rrLTex = 1;
  } else {
    if (rrTN >= RR_BATCH_MAX) return;
    v = rrTV; c = rrTC; t = rrTT; n = rrTN++;
    if (rrPHasTex) rrTTex = 1;
  }
  v[n*3+0] = rrPX[src]; v[n*3+1] = rrPY[src]; v[n*3+2] = rrPZ[src];
  c[n*4+0] = rrPC[src*4+0]; c[n*4+1] = rrPC[src*4+1];
  c[n*4+2] = rrPC[src*4+2]; c[n*4+3] = rrPC[src*4+3];
  t[n*2+0] = rrPT[src*2+0]; t[n*2+1] = rrPT[src*2+1];
}

/* Decompose into independent primitives so batches can simply concatenate.
   GLES2 has no GL_QUADS at all; the one four-vertex block here is identical as
   a fan over the same corners. */
static void rrEnd(void) {
  int i, n = rrPN;
  if (n < 2) { rrPN = 0; return; }
  switch (rrPMode) {
  case GL_TRIANGLE_FAN:
  case GL_QUADS:
    for (i = 1; i+1 < n; i++) { rrEmit(0, 0); rrEmit(0, i); rrEmit(0, i+1); }
    break;
  case GL_TRIANGLE_STRIP:
    for (i = 0; i+2 < n; i++) {
      if (i & 1) { rrEmit(0, i+1); rrEmit(0, i); rrEmit(0, i+2); }
      else       { rrEmit(0, i);   rrEmit(0, i+1); rrEmit(0, i+2); }
    }
    break;
  case GL_LINE_LOOP:
    for (i = 0; i < n; i++) { rrEmit(1, i); rrEmit(1, (i+1) % n); }
    break;
  case GL_LINE_STRIP:
    for (i = 0; i+1 < n; i++) { rrEmit(1, i); rrEmit(1, i+1); }
    break;
  case GL_LINES:
  default:
    for (i = 0; i+1 < n; i += 2) { rrEmit(1, i); rrEmit(1, i+1); }
    break;
  }
  rrPN = 0;
  if (!rrBatchOn) rrFlush();
}

/* Anything that changes how subsequent geometry is rasterised ends the batch.
   Defined ahead of the macros below so their own calls reach real GL. */
static void rrEnableGL(GLenum cap)  { rrFlush(); glEnable(cap); }
static void rrDisableGL(GLenum cap) { rrFlush(); glDisable(cap); }
static void rrBindTexture(GLenum target, GLuint tex) { rrFlush(); glBindTexture(target, tex); }

#define glBegin       rrBegin
#define glEnd         rrEnd
#define glVertex3f    rrVertex3f
#define glVertex2f    rrVertex2f
#define glColor4ub    rrColor4ub
#define glTexCoord2f  rrTexCoord2f
#define glEnable      rrEnableGL
#define glDisable     rrDisableGL
#define glBindTexture rrBindTexture
#endif /* __EMSCRIPTEN__ */

#define FAR_PLANE 720

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define LOWRES_SCREEN_WIDTH 320
#define LOWRES_SCREEN_HEIGHT 240

static int screenWidth, screenHeight;

/*
 * Layout.
 *
 * The playfield is not rendered separately from the side boards: the 3D scene is
 * drawn across the whole viewport and drawSideBoards() paints black over the
 * outer thirds, cropping it to the centre 320 of 640. The field measures
 * 4.096 x 6.144 world units against a 6.213 half-height frustum at zoom 15, so
 * it fills the height and almost exactly half the width of a 4:3 viewport --
 * which is why the 160-wide boards line up with it.
 *
 * Portrait therefore comes almost free: hand the scene a 2:3 viewport instead
 * and the field fills the width edge to edge with no masking, leaving spare
 * height above and below for the HUD. The HUD needs no redrawing, only
 * re-placing, since its ortho pass keeps x at 0..640 and simply extends in y.
 */
static int   rrPortrait  = 0;      /* chosen from the framebuffer aspect */
static int   vpX, vpY, vpW, vpH;   /* the 3D viewport, in pixels */
static float rrHudScale  = 1.0f;   /* pixels per HUD unit */
static float rrHudBottom = 480.0f; /* HUD y at the bottom of the screen */
static float rrFieldTop  = 0.0f;   /* HUD y of the field's top edge */
static float rrFieldBot  = 480.0f; /* ...and its bottom edge */

/* HUD units reserved above the field, enough for the score, the boss shield bar
   and the readout drawRPanel() lays out there in portrait. Reserving it costs no
   playfield on a phone: the field is limited by the width, so the leftover height
   would otherwise just sit unused below. */
#define RR_TOP_STRIP   200

// Reset viewport when the screen is resized.
static void screenResized() {
  if (screenHeight * SCREEN_WIDTH <= screenWidth * SCREEN_HEIGHT) {
    /* 4:3 or wider -- the original letterbox, boards masking the sides. */
    rrPortrait = 0;
    vpW = SCREEN_WIDTH * screenHeight / SCREEN_HEIGHT;
    vpH = screenHeight;
    if (vpW > screenWidth) { vpW = screenWidth; vpH = SCREEN_HEIGHT * screenWidth / SCREEN_WIDTH; }
    vpX = (screenWidth - vpW) / 2;
    vpY = (screenHeight - vpH) / 2;
    rrHudScale  = (float)vpW / (float)SCREEN_WIDTH;
    rrHudBottom = 480.0f;
    rrFieldTop  = 0.0f;
    rrFieldBot  = 480.0f;
  } else {
    /* Taller than 4:3: the field gets its own 2:3 viewport across the full
       width. HUD x stays 0..640 mapped to the screen width so nothing that
       draws into it gets clipped; y just extends past 480.

       The top strip is reserved first and the field fitted into what is left, so
       the readout never ends up overlapping the playfield on a squarer screen. */
    int topPx, availH;
    rrPortrait  = 1;
    rrHudScale  = (float)screenWidth / (float)SCREEN_WIDTH;
    topPx       = (int)(RR_TOP_STRIP * rrHudScale);
    availH      = screenHeight - topPx;
    if (availH < 1) { topPx = 0; availH = screenHeight; }
    vpW = screenWidth;
    vpH = vpW * 3 / 2;
    if (vpH > availH) { vpH = availH; vpW = vpH * 2 / 3; }
    vpX = (screenWidth - vpW) / 2;
    vpY = screenHeight - topPx - vpH;      /* GL y counts from the bottom */
    rrHudBottom = (float)screenHeight / rrHudScale;
    rrFieldTop  = (float)topPx / rrHudScale;
    rrFieldBot  = rrFieldTop + (float)vpH / rrHudScale;
  }

  glViewport(vpX, vpY, vpW, vpH);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(45.0f, (GLfloat)vpW/(GLfloat)vpH, 0.1f, FAR_PLANE);
  glMatrixMode(GL_MODELVIEW);
}

/* Exposed so the shell can lay the touch controls out against the same
   geometry the renderer is using. */
#ifdef __EMSCRIPTEN__
/*
 * Width of the playfield itself, in framebuffer pixels.
 *
 * The shell needs this to convert finger travel into field units, and it is not
 * the viewport width: in landscape the scene viewport is 4:3 and the field is
 * the masked centre half of it, while in portrait the viewport *is* the field.
 * Getting this wrong moves the ship at double or half speed.
 */
EMSCRIPTEN_KEEPALIVE
int rr_field_px_width(void) { return rrPortrait ? vpW : vpW / 2; }
#endif

/* The HUD is laid out differently in portrait, and drawRPanel() needs to know. */
int rrHudPortrait(void) { return rrPortrait; }

void resized(int width, int height) {
  screenWidth = width; screenHeight = height;
  screenResized();
}

#ifdef __EMSCRIPTEN__
/*
 * Match the framebuffer to the canvas's on-screen size at device resolution.
 *
 * SDL_SetVideoMode gives the canvas a fixed 640x480 backing store, which the
 * browser then scales to whatever size the element occupies -- on a 2x display
 * that is a doubling even in a window, and fullscreen stretches 640x480 across
 * the whole screen. Nothing here is a low-resolution asset: the only bitmaps
 * are two 128x128 glow sprites and the 150x36 logo, and everything else is
 * vector geometry generated per frame, so it re-renders sharp at any size.
 *
 * Layout is owned entirely by CSS in shell.html (the canvas is 100% of a 4:3
 * frame); this only reads that size back and gives the element that many device
 * pixels. Setting the canvas size from here as well would fight the stylesheet.
 *
 * screenResized() already letterboxes the viewport to 4:3 and rebuilds the
 * projection from it, and the HUD pass uses its own glOrtho(0,640,480,...)
 * mapped onto the viewport rather than the framebuffer, so the backing store
 * can be any size without disturbing the game's coordinate systems.
 */

/*
 * Ceiling on the framebuffer's long edge.
 *
 * Native density on a large display is a lot of pixels: a 2056x1329 CSS screen
 * at devicePixelRatio 2 is 4112x2658, about 11 megapixels. The game clears and
 * additively blends the whole frame and issues several hundred small primitives,
 * so fill rate gives out first, and vector art gains very little from the last
 * doubling. 1920 stays visibly sharp for roughly a fifth of the pixels.
 *
 * Only the backing store is capped -- the CSS box still fills its frame, so the
 * browser scales the result up. Raise it if you have GPU headroom; 2560 and 3840
 * are reasonable steps.
 */
#define RR_MAX_FB_LONG_EDGE 1920

/*
 * Phones are fill-rate bound long before they run out of pixels to address: the
 * game clears and additively blends the entire frame every step, and a handset
 * at devicePixelRatio 3 reaches the desktop cap trivially. Cap those lower.
 * Detected via the pointer media query rather than a user-agent string, and
 * cached because it cannot change for the life of the page.
 */
#define RR_MAX_FB_LONG_EDGE_COARSE 1280

static int rrMaxFramebufferEdge(void) {
  static int cached = 0;
  if (!cached) {
    /* window.rrFbCap lets ?fb=N override the cap from the URL, so a suspected
       fill-rate or GPU-memory limit can be bisected on a device without a
       rebuild. 0 or absent means use the built-in caps. */
    int override = EM_ASM_INT({
      return (typeof window.rrFbCap === 'number' && window.rrFbCap > 0) ? window.rrFbCap : 0;
    });
    if (override > 0) {
      cached = override;
    } else {
      int coarse = EM_ASM_INT({
        return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0;
      });
      cached = coarse ? RR_MAX_FB_LONG_EDGE_COARSE : RR_MAX_FB_LONG_EDGE;
    }
  }
  return cached;
}

static void syncCanvasToDisplay(void) {
  double dpr = emscripten_get_device_pixel_ratio();
  double cssW = 0, cssH = 0;
  int w, h, longEdge, curW = 0, curH = 0;

  if (emscripten_get_element_css_size("#canvas", &cssW, &cssH) != EMSCRIPTEN_RESULT_SUCCESS)
    return;
  if (cssW <= 0 || cssH <= 0) return;

  w = (int)(cssW * dpr + 0.5);
  h = (int)(cssH * dpr + 0.5);

  longEdge = (w > h) ? w : h;
  if (longEdge > rrMaxFramebufferEdge()) {
    double scale = (double)rrMaxFramebufferEdge() / (double)longEdge;
    w = (int)(w * scale + 0.5);
    h = (int)(h * scale + 0.5);
  }
  if (w <= 0 || h <= 0) return;

  /* Reallocating the drawing buffer every frame would be wasteful and can drop
     the GL state on some drivers, so only touch it when it actually changes. */
  emscripten_get_canvas_element_size("#canvas", &curW, &curH);
  if (curW == w && curH == h) return;

  emscripten_set_canvas_element_size("#canvas", w, h);
  resized(w, h);
}

/*
 * Frames still owed a size check, applied at the top of drawGLSceneStart.
 *
 * A countdown rather than a flag for two reasons. Emscripten's own fullscreen
 * path may also write the canvas size and runs in the same task as our handler,
 * so applying inline would make the winner depend on listener order. And the
 * element's CSS size can take a frame or two to settle after a fullscreen
 * transition, so a single deferred check can read a stale box. Re-checking for
 * a few frames costs nothing: syncCanvasToDisplay() returns immediately when
 * the size already matches.
 */
static int canvasSyncPending = 4;

static EM_BOOL onBrowserResize(int type, const EmscriptenUiEvent *e, void *user) {
  (void)type; (void)e; (void)user;
  canvasSyncPending = 4;
  return EM_FALSE;
}

static EM_BOOL onFullscreenChange(int type, const EmscriptenFullscreenChangeEvent *e, void *user) {
  (void)type; (void)e; (void)user;
  canvasSyncPending = 8;
  return EM_FALSE;
}
#endif

// Init OpenGL.
static void initGL() {
  glViewport(0, 0, screenWidth, screenHeight);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  glLineWidth(1);
  glEnable(GL_LINE_SMOOTH);

  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glEnable(GL_BLEND);

  glDisable(GL_LIGHTING);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_COLOR_MATERIAL);

  resized(screenWidth, screenHeight);
}

// Load bitmaps and convert to textures.
void loadGLTexture(char *fileName, GLuint *texture) {
  SDL_Surface *surface;

  char name[32];
  strcpy(name, "images/");
  strcat(name, fileName);
  surface = SDL_LoadBMP(name);
  if ( !surface ) {
    fprintf(stderr, "Unable to load texture: %s\n", SDL_GetError());
    SDL_Quit();
    exit(1);
  }

  glGenTextures(1, texture);
  glBindTexture(GL_TEXTURE_2D, *texture);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST);
#ifdef __EMSCRIPTEN__
  {
    /* SDL_LoadBMP here goes through the browser's decoder (--use-preload-plugins),
       which hands back a 32-bit RGBA surface rather than the 24-bit the native
       loader produces. Passing GL_RGB against 4-byte pixels would walk the rows
       out of step and overrun the buffer on the last row. */
    int bpp = surface->format->BytesPerPixel;
    GLenum fmt = (bpp == 4) ? GL_RGBA : GL_RGB;
    gluBuild2DMipmaps(GL_TEXTURE_2D, bpp, surface->w, surface->h, fmt, GL_UNSIGNED_BYTE, surface->pixels);
  }
#else
  gluBuild2DMipmaps(GL_TEXTURE_2D, 3, surface->w, surface->h, GL_RGB, GL_UNSIGNED_BYTE, surface->pixels);
#endif
}

void generateTexture(GLuint *texture) {
  glGenTextures(1, texture);
}

void deleteTexture(GLuint *texture) {
  glDeleteTextures(1, texture);
}

static GLuint starTexture;
#define STAR_BMP "star.bmp"
static GLuint smokeTexture;
#define SMOKE_BMP "smoke.bmp"
static GLuint titleTexture;
#define TITLE_BMP "title.bmp"

int lowres = 0;
int windowMode = 0;
int brightness = DEFAULT_BRIGHTNESS;
Uint8 *keys;
SDL_Joystick *stick = NULL;

void initSDL() {
#ifdef __EMSCRIPTEN__
  /* ?batch=0 -> one draw call per primitive, as before batching. */
  rrBatchOn = EM_ASM_INT({ return (window.rrBatch === 0) ? 0 : 1; });
#endif
  Uint32 videoFlags;

  if ( lowres ) {
    screenWidth  = LOWRES_SCREEN_WIDTH;
    screenHeight = LOWRES_SCREEN_HEIGHT;
  } else {
    screenWidth  = SCREEN_WIDTH;
    screenHeight = SCREEN_HEIGHT;
  }

  /* Initialize SDL */
  if ( SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0 ) {
    fprintf(stderr, "Unable to initialize SDL: %s\n", SDL_GetError());
    exit(1);
  }

  /* Create an OpenGL screen */
  if ( windowMode ) {
    videoFlags = SDL_OPENGL | SDL_RESIZABLE;
  } else {
    if ( !lowres ) {
      // Use native desktop resolution if -lowres is not specified.
      screenWidth = 0;
      screenHeight = 0;
    }
    videoFlags = SDL_OPENGL | SDL_FULLSCREEN;
  } 
  if ( SDL_SetVideoMode(screenWidth, screenHeight, 0, videoFlags) == NULL ) {
    fprintf(stderr, "Unable to create OpenGL screen: %s\n", SDL_GetError());
    SDL_Quit();
    exit(2);
  }

  SDL_Surface* videoSurface = SDL_GetVideoSurface();
  screenWidth = videoSurface->w;
  screenHeight = videoSurface->h;

  stick = SDL_JoystickOpen(0);

  /* Set the title bar in environments that support it */
  SDL_WM_SetCaption(CAPTION, NULL);

  initGL();
  loadGLTexture(STAR_BMP, &starTexture);
  loadGLTexture(SMOKE_BMP, &smokeTexture);
  loadGLTexture(TITLE_BMP, &titleTexture);

#ifdef __EMSCRIPTEN__
  emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_FALSE, onBrowserResize);
  emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_FALSE, onFullscreenChange);
  syncCanvasToDisplay();
#endif

  SDL_ShowCursor(SDL_DISABLE);
}

void closeSDL() {
  SDL_ShowCursor(SDL_ENABLE);
}

float zoom = 15;
static int screenShakeCnt = 0;
static int screenShakeType = 0;

static void setEyepos() {
  float x, y;
  glPushMatrix();
  if ( screenShakeCnt > 0 ) {
    switch ( screenShakeType ) {
    case 0:
      x = (float)randNS2(256)/5000.0f;
      y = (float)randNS2(256)/5000.0f;
      break;
    default:
      x = (float)randNS2(256)*screenShakeCnt/21000.0f;
      y = (float)randNS2(256)*screenShakeCnt/21000.0f;
      break;
    }
    gluLookAt(0, 0, zoom, x, y, 0, 0.0f, 1.0f, 0.0f);
  } else {
    gluLookAt(0, 0, zoom, 0, 0, 0, 0.0f, 1.0f, 0.0f);
  }
}

void setScreenShake(int type, int cnt) {
  screenShakeType = type; screenShakeCnt = cnt;
}

void moveScreenShake() {
  if ( screenShakeCnt > 0 ) {
    screenShakeCnt--;
  }
}

void drawGLSceneStart() {
#ifdef __EMSCRIPTEN__
  if (canvasSyncPending > 0) {
    canvasSyncPending--;
    syncCanvasToDisplay();
  }
#endif
  glClear(GL_COLOR_BUFFER_BIT);
  setEyepos();
}

void drawGLSceneEnd() {
  glPopMatrix();
}

void swapGLScene() {
#ifdef __EMSCRIPTEN__
  rrFlush();                 /* nothing may outlive the frame it was built in */
#endif
  SDL_GL_SwapBuffers();
}

void drawBox(GLfloat x, GLfloat y, GLfloat width, GLfloat height, 
	     int r, int g, int b) {
  glPushMatrix();
  glTranslatef(x, y, 0);
  glColor4ub(r, g, b, 128);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-width, -height,  0);
  glVertex3f( width, -height,  0);
  glVertex3f( width,  height,  0);
  glVertex3f(-width,  height,  0);
  glEnd();
  glColor4ub(r, g, b, 255);
  glBegin(GL_LINE_LOOP);
  glVertex3f(-width, -height,  0);
  glVertex3f( width, -height,  0);
  glVertex3f( width,  height,  0);
  glVertex3f(-width,  height,  0);
  glEnd();
  glPopMatrix();
}

void drawLine(GLfloat x1, GLfloat y1, GLfloat z1,
	      GLfloat x2, GLfloat y2, GLfloat z2, int r, int g, int b, int a) {
  glColor4ub(r, g, b, a);
  glBegin(GL_LINES);
  glVertex3f(x1, y1, z1);
  glVertex3f(x2, y2, z2);
  glEnd();
}

void drawLinePart(GLfloat x1, GLfloat y1, GLfloat z1,
		  GLfloat x2, GLfloat y2, GLfloat z2, int r, int g, int b, int a, int len) {
  glColor4ub(r, g, b, a);
  glBegin(GL_LINES);
  glVertex3f(x1, y1, z1);
  glVertex3f(x1+(x2-x1)*len/256, y1+(y2-y1)*len/256, z1+(z2-z1)*len/256);
  glEnd();
}

void drawRollLineAbs(GLfloat x1, GLfloat y1, GLfloat z1,
		     GLfloat x2, GLfloat y2, GLfloat z2, int r, int g, int b, int a, int d1) {
  glPushMatrix();
  glRotatef((float)d1*360/1024, 0, 0, 1);
  glColor4ub(r, g, b, a);
  glBegin(GL_LINES);
  glVertex3f(x1, y1, z1);
  glVertex3f(x2, y2, z2);
  glEnd();
  glPopMatrix();
}

void drawRollLine(GLfloat x, GLfloat y, GLfloat z, GLfloat width,
		  int r, int g, int b, int a, int d1, int d2) {
  glPushMatrix();
  glTranslatef(x, y, z);
  glRotatef((float)d1*360/1024, 0, 0, 1);
  glRotatef((float)d2*360/1024, 1, 0, 0);
  glColor4ub(r, g, b, a);
  glBegin(GL_LINES);
  glVertex3f(0, -width, 0);
  glVertex3f(0,  width, 0);
  glEnd();
  glPopMatrix();
}

void drawSquare(GLfloat x1, GLfloat y1, GLfloat z1, 
		GLfloat x2, GLfloat y2, GLfloat z2, 
		GLfloat x3, GLfloat y3, GLfloat z3, 
		GLfloat x4, GLfloat y4, GLfloat z4, 
		int r, int g, int b) {
  glColor4ub(r, g, b, 64);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(x1, y1, z1);
  glVertex3f(x2, y2, z2);
  glVertex3f(x3, y3, z3);
  glVertex3f(x4, y4, z4);
  glEnd();
}

void drawStar(int f, GLfloat x, GLfloat y, GLfloat z, int r, int g, int b, float size) {
  glEnable(GL_TEXTURE_2D);
  if ( f ) {
    glBindTexture(GL_TEXTURE_2D, starTexture);
  } else {
    glBindTexture(GL_TEXTURE_2D, smokeTexture);
  }
  glColor4ub(r, g, b, 255);
  glPushMatrix();
  glTranslatef(x, y, z);
  glRotatef(rand()%360, 0.0f, 0.0f, 1.0f);
  glBegin(GL_TRIANGLE_FAN);
  glTexCoord2f(0.0f, 1.0f); 
  glVertex3f(-size, -size,  0);
  glTexCoord2f(1.0f, 1.0f);
  glVertex3f( size, -size,  0);
  glTexCoord2f(1.0f, 0.0f);
  glVertex3f( size,  size,  0);
  glTexCoord2f(0.0f, 0.0f);
  glVertex3f(-size,  size,  0);
  glEnd();
  glPopMatrix();
  glDisable(GL_TEXTURE_2D);
}

#define LASER_ALPHA 100
#define LASER_LINE_ALPHA 50
#define LASER_LINE_ROLL_SPEED 17
#define LASER_LINE_UP_SPEED 16

void drawLaser(GLfloat x, GLfloat y, GLfloat width, GLfloat height,
	       int cc1, int cc2, int cc3, int cc4, int cnt, int type) {
  int i, d;
  float gx, gy;
  glBegin(GL_TRIANGLE_FAN);
  if ( type != 0 ) {
    glColor4ub(cc1, cc1, cc1, LASER_ALPHA);
    glVertex3f(x-width, y, 0);
  }
  glColor4ub(cc2, 255, cc2, LASER_ALPHA);
  glVertex3f(x, y, 0);
  glColor4ub(cc4, 255, cc4, LASER_ALPHA);
  glVertex3f(x, y+height, 0);
  glColor4ub(cc3, cc3, cc3, LASER_ALPHA);
  glVertex3f(x-width, y+height, 0);
  glEnd();
  glBegin(GL_TRIANGLE_FAN);
  if ( type != 0 ) {
    glColor4ub(cc1, cc1, cc1, LASER_ALPHA);
    glVertex3f(x+width, y, 0);
  }
  glColor4ub(cc2, 255, cc2, LASER_ALPHA);
  glVertex3f(x, y, 0);
  glColor4ub(cc4, 255, cc4, LASER_ALPHA);
  glVertex3f(x, y+height, 0);
  glColor4ub(cc3, cc3, cc3, LASER_ALPHA);
  glVertex3f(x+width, y+height, 0);
  glEnd();
  if ( type == 2 ) return;
  glColor4ub(80, 240, 80, LASER_LINE_ALPHA);
  glBegin(GL_LINES);
  d = (cnt*LASER_LINE_ROLL_SPEED)&(512/4-1);
  for ( i=0 ; i<4 ; i++, d+=(512/4) ) {
    d &= 1023;
    gx = x + width*sctbl[d+256]/256.0f;
    if ( type == 1 ) {
      glVertex3f(gx, y, 0);
    } else {
      glVertex3f(x, y, 0);
    }
    glVertex3f(gx, y+height, 0);
  }
  if ( type == 0 ) {
    glEnd();
    return;
  }
  gy = y + (height/4/LASER_LINE_UP_SPEED) * (cnt&(LASER_LINE_UP_SPEED-1));
  for ( i=0 ; i<4 ; i++, gy+=height/4 ) {
    glVertex3f(x-width, gy, 0);
    glVertex3f(x+width, gy, 0);
  }
  glEnd();
}

#define SHAPE_POINT_SIZE 0.05f
#define SHAPE_BASE_COLOR_R 250
#define SHAPE_BASE_COLOR_G 240
#define SHAPE_BASE_COLOR_B 180

#define CORE_HEIGHT 0.2f
#define CORE_RING_SIZE 0.6f

#define SHAPE_POINT_SIZE_L 0.07f

static void drawRing(GLfloat x, GLfloat y, int d1, int d2, int r, int g, int b) {
  int i, d;
  float x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4;
  glPushMatrix();
  glTranslatef(x, y, 0);
  glRotatef((float)d1*360/1024, 0, 0, 1);
  glRotatef((float)d2*360/1024, 1, 0, 0);
  glColor4ub(r, g, b, 255);
  x1 = x2 = 0;
  y1 = y4 =  CORE_HEIGHT/2;
  y2 = y3 = -CORE_HEIGHT/2;
  z1 = z2 = CORE_RING_SIZE;
  for ( i=0,d=0 ; i<8 ; i++ ) {
    d+=(1024/8); d &= 1023;
    x3 = x4 = sctbl[d+256]*CORE_RING_SIZE/256;
    z3 = z4 = sctbl[d]    *CORE_RING_SIZE/256;
    drawSquare(x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, r, g, b);
    x1 = x3; y1 = y3; z1 = z3;
    x2 = x4; y2 = y4; z2 = z4;
  }
  glPopMatrix();
}

void drawCore(GLfloat x, GLfloat y, int cnt, int r, int g, int b) {
  int i;
  float cy;
  glPushMatrix();
  glTranslatef(x, y, 0);
  glColor4ub(r, g, b, 255);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-SHAPE_POINT_SIZE_L, -SHAPE_POINT_SIZE_L,  0);
  glVertex3f( SHAPE_POINT_SIZE_L, -SHAPE_POINT_SIZE_L,  0);
  glVertex3f( SHAPE_POINT_SIZE_L,  SHAPE_POINT_SIZE_L,  0);
  glVertex3f(-SHAPE_POINT_SIZE_L,  SHAPE_POINT_SIZE_L,  0);
  glEnd();
  glPopMatrix();
  cy = y - CORE_HEIGHT*2.5f;
  for ( i=0 ; i<4 ; i++, cy+=CORE_HEIGHT ) {
    drawRing(x, cy, (cnt*(4+i))&1023, (sctbl[(cnt*(5+i))&1023]/4)&1023, r, g, b);
  }
}

#define SHIP_DRUM_R 0.4f
#define SHIP_DRUM_WIDTH 0.05f
#define SHIP_DRUM_HEIGHT 0.35f

void drawShipShape(GLfloat x, GLfloat y, float d, int inv) {
  int i;
  glPushMatrix();
  glTranslatef(x, y, 0);
  glColor4ub(255, 100, 100, 255);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-SHAPE_POINT_SIZE_L, -SHAPE_POINT_SIZE_L,  0);
  glVertex3f( SHAPE_POINT_SIZE_L, -SHAPE_POINT_SIZE_L,  0);
  glVertex3f( SHAPE_POINT_SIZE_L,  SHAPE_POINT_SIZE_L,  0);
  glVertex3f(-SHAPE_POINT_SIZE_L,  SHAPE_POINT_SIZE_L,  0);
  glEnd();
  if ( inv ) {
    glPopMatrix();
    return;
  }
  glRotatef(d, 0, 1, 0);
    glColor4ub(120, 220, 100, 150);
    /*if ( mode == IKA_MODE ) {
    glColor4ub(180, 200, 160, 150);
  } else {
    glColor4ub(120, 220, 100, 150);
    }*/
  for ( i=0 ; i<8 ; i++ ) {
    glRotatef(45, 0, 1, 0);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-SHIP_DRUM_WIDTH, -SHIP_DRUM_HEIGHT, SHIP_DRUM_R);
    glVertex3f( SHIP_DRUM_WIDTH, -SHIP_DRUM_HEIGHT, SHIP_DRUM_R);
    glVertex3f( SHIP_DRUM_WIDTH,  SHIP_DRUM_HEIGHT, SHIP_DRUM_R);
    glVertex3f(-SHIP_DRUM_WIDTH,  SHIP_DRUM_HEIGHT, SHIP_DRUM_R);
    glEnd();
  }
  glPopMatrix();
}

void drawBomb(GLfloat x, GLfloat y, GLfloat width, int cnt) {
  int i, d, od, c;
  GLfloat x1, y1, x2, y2;
  d = cnt*48; d &= 1023;
  c = 4+(cnt>>3); if ( c > 16 ) c = 16;
  od = 1024/c;
  x1 = (sctbl[d]    *width)/256 + x;
  y1 = (sctbl[d+256]*width)/256 + y;
  for ( i=0 ; i<c ; i++ ) {
    d += od; d &= 1023;
    x2 = (sctbl[d]    *width)/256 + x;
    y2 = (sctbl[d+256]*width)/256 + y;
    drawLine(x1, y1, 0, x2, y2, 0, 255, 255, 255, 255);
    x1 = x2; y1 = y2;
  }
}

void drawCircle(GLfloat x, GLfloat y, GLfloat width, int cnt, 
		int r1, int g1, int b1, int r2, int b2, int g2) {
  int i, d;
  GLfloat x1, y1, x2, y2;
  if ( (cnt&1) == 0 ) {
    glColor4ub(r1, g1, b1, 64);
  } else {
    glColor4ub(255, 255, 255, 64);
  }
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(x, y, 0);
  d = cnt*48; d &= 1023;
  x1 = (sctbl[d]    *width)/256 + x;
  y1 = (sctbl[d+256]*width)/256 + y;
  glColor4ub(r2, g2, b2, 150);
  for ( i=0 ; i<16 ; i++ ) {
    d += 64; d &= 1023;
    x2 = (sctbl[d]    *width)/256 + x;
    y2 = (sctbl[d+256]*width)/256 + y;
    glVertex3f(x1, y1, 0);
    glVertex3f(x2, y2, 0);
    x1 = x2; y1 = y2;
  }
  glEnd();
}

void drawShape(GLfloat x, GLfloat y, GLfloat size, int d, int cnt, int type,
	       int r, int g, int b) {
  GLfloat sz, sz2;
  glPushMatrix();
  glTranslatef(x, y, 0);
  glColor4ub(r, g, b, 255);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-SHAPE_POINT_SIZE, -SHAPE_POINT_SIZE,  0);
  glVertex3f( SHAPE_POINT_SIZE, -SHAPE_POINT_SIZE,  0);
  glVertex3f( SHAPE_POINT_SIZE,  SHAPE_POINT_SIZE,  0);
  glVertex3f(-SHAPE_POINT_SIZE,  SHAPE_POINT_SIZE,  0);
  glEnd();
  switch ( type ) {
  case 0:
    sz = size/2;
    glRotatef((float)d*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz,  0);
    glVertex3f( sz, -sz,  0);
    glVertex3f( 0, size,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 150);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz,  0);
    glVertex3f( sz, -sz,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( 0, size,  0);
    glEnd();
    break;
  case 1:
    sz = size/2;
    glRotatef((float)((cnt*23)&1023)*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(  0, -size,  0);
    glVertex3f( sz,     0,  0);
    glVertex3f(  0,  size,  0);
    glVertex3f(-sz,     0,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 180);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(  0, -size,  0);
    glVertex3f( sz,     0,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f(  0,  size,  0);
    glVertex3f(-sz,     0,  0);
    glEnd();
    break;
  case 2:
    sz = size/4; sz2 = size/3*2;
    glRotatef((float)d*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz2,  0);
    glVertex3f( sz, -sz2,  0);
    glVertex3f( sz,  sz2,  0);
    glVertex3f(-sz,  sz2,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 120);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz2,  0);
    glVertex3f( sz, -sz2,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( sz, sz2,  0);
    glVertex3f(-sz, sz2,  0);
    glEnd();
    break;
  case 3:
    sz = size/2;
    glRotatef((float)((cnt*37)&1023)*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz,  0);
    glVertex3f( sz, -sz,  0);
    glVertex3f( sz,  sz,  0);
    glVertex3f(-sz,  sz,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 180);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz,  0);
    glVertex3f( sz, -sz,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( sz,  sz,  0);
    glVertex3f(-sz,  sz,  0);
    glEnd();
    break;
  case 4:
    sz = size/2;
    glRotatef((float)((cnt*53)&1023)*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz/2, -sz,  0);
    glVertex3f( sz/2, -sz,  0);
    glVertex3f( sz,  -sz/2,  0);
    glVertex3f( sz,   sz/2,  0);
    glVertex3f( sz/2,  sz,  0);
    glVertex3f(-sz/2,  sz,  0);
    glVertex3f(-sz,   sz/2,  0);
    glVertex3f(-sz,  -sz/2,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 220);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz/2, -sz,  0);
    glVertex3f( sz/2, -sz,  0);
    glVertex3f( sz,  -sz/2,  0);
    glVertex3f( sz,   sz/2,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( sz/2,  sz,  0);
    glVertex3f(-sz/2,  sz,  0);
    glVertex3f(-sz,   sz/2,  0);
    glVertex3f(-sz,  -sz/2,  0);
    glEnd();
    break;
  case 5:
    sz = size*2/3; sz2 = size/5;
    glRotatef((float)d*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_STRIP);
    glVertex3f(-sz, -sz+sz2,  0);
    glVertex3f( 0, sz+sz2,  0);
    glVertex3f( sz, -sz+sz2,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 150);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz+sz2,  0);
    glVertex3f( sz, -sz+sz2,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( 0, sz+sz2,  0);
    glEnd();
    break;
  case 6:
    sz = size/2;
    glRotatef((float)((cnt*13)&1023)*360/1024, 0, 0, 1);
    glDisable(GL_BLEND);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz,  0);
    glVertex3f(  0, -sz,  0);
    glVertex3f( sz,   0,  0);
    glVertex3f( sz,  sz,  0);
    glVertex3f(  0,  sz,  0);
    glVertex3f(-sz,   0,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(r, g, b, 210);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz,  0);
    glVertex3f(  0, -sz,  0);
    glVertex3f( sz,   0,  0);
    glColor4ub(SHAPE_BASE_COLOR_R, SHAPE_BASE_COLOR_G, SHAPE_BASE_COLOR_B, 150);
    glVertex3f( sz,  sz,  0);
    glVertex3f(  0,  sz,  0);
    glVertex3f(-sz,   0,  0);
    glEnd();
    break;
  }
  glPopMatrix();
}

static int ikaClr[2][3][3] = {
  {{230, 230, 255}, {100, 100, 200}, {50, 50, 150}},
  {{0, 0, 0}, {200, 0, 0}, {100, 0, 0}},
};

void drawShapeIka(GLfloat x, GLfloat y, GLfloat size, int d, int cnt, int type, int c) {
  GLfloat sz, sz2, sz3;
  glPushMatrix();
  glTranslatef(x, y, 0);
  glColor4ub(ikaClr[c][0][0], ikaClr[c][0][1], ikaClr[c][0][2], 255);
  glDisable(GL_BLEND);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-SHAPE_POINT_SIZE, -SHAPE_POINT_SIZE,  0);
  glVertex3f( SHAPE_POINT_SIZE, -SHAPE_POINT_SIZE,  0);
  glVertex3f( SHAPE_POINT_SIZE,  SHAPE_POINT_SIZE,  0);
  glVertex3f(-SHAPE_POINT_SIZE,  SHAPE_POINT_SIZE,  0);
  glEnd();
  glColor4ub(ikaClr[c][0][0], ikaClr[c][0][1], ikaClr[c][0][2], 255);
  switch ( type ) {
  case 0:
    sz = size/2; sz2 = sz/3; sz3 = size*2/3;
    glRotatef((float)d*360/1024, 0, 0, 1);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz3,  0);
    glVertex3f( sz, -sz3,  0);
    glVertex3f( sz2, sz3,  0);
    glVertex3f(-sz2, sz3,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(ikaClr[c][1][0], ikaClr[c][1][1], ikaClr[c][1][2], 250);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz, -sz3,  0);
    glVertex3f( sz, -sz3,  0);
    glColor4ub(ikaClr[c][2][0], ikaClr[c][2][1], ikaClr[c][2][2], 250);
    glVertex3f( sz2, sz3,  0);
    glVertex3f(-sz2, sz3,  0);
    glEnd();
    break;
  case 1:
    sz = size/2;
    glRotatef((float)((cnt*53)&1023)*360/1024, 0, 0, 1);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz/2, -sz,  0);
    glVertex3f( sz/2, -sz,  0);
    glVertex3f( sz,  -sz/2,  0);
    glVertex3f( sz,   sz/2,  0);
    glVertex3f( sz/2,  sz,  0);
    glVertex3f(-sz/2,  sz,  0);
    glVertex3f(-sz,   sz/2,  0);
    glVertex3f(-sz,  -sz/2,  0);
    glEnd();
    glEnable(GL_BLEND);
    glColor4ub(ikaClr[c][1][0], ikaClr[c][1][1], ikaClr[c][1][2], 250);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(-sz/2, -sz,  0);
    glVertex3f( sz/2, -sz,  0);
    glVertex3f( sz,  -sz/2,  0);
    glVertex3f( sz,   sz/2,  0);
    glColor4ub(ikaClr[c][2][0], ikaClr[c][2][1], ikaClr[c][2][2], 250);
    glVertex3f( sz/2,  sz,  0);
    glVertex3f(-sz/2,  sz,  0);
    glVertex3f(-sz,   sz/2,  0);
    glVertex3f(-sz,  -sz/2,  0);
    glEnd();
    break;
  }
  glPopMatrix();
}

#define SHOT_WIDTH 0.1
#define SHOT_HEIGHT 0.2

static int shtClr[3][3][3] = {
  {{200, 200, 225}, {50, 50, 200}, {200, 200, 225}},
  {{100, 0, 0}, {100, 0, 0}, {200, 0, 0}},
  {{100, 200, 100}, {50, 100, 50}, {100, 200, 100}},
};

void drawShot(GLfloat x, GLfloat y, GLfloat d, int c, float width, float height) {
  glPushMatrix();
  glTranslatef(x, y, 0);
  glRotatef(d, 0, 0, 1);
  glColor4ub(shtClr[c][0][0], shtClr[c][0][1], shtClr[c][0][2], 240);
  glDisable(GL_BLEND);
  glBegin(GL_LINES);
  glVertex3f(-width, -height, 0);
  glVertex3f(-width,  height, 0);
  glVertex3f( width, -height, 0);
  glVertex3f( width,  height, 0);
  glEnd();
  glEnable(GL_BLEND);

  glColor4ub(shtClr[c][1][0], shtClr[c][1][1], shtClr[c][1][2], 240);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(-width, -height, 0);
  glVertex3f( width, -height, 0);
  glColor4ub(shtClr[c][2][0], shtClr[c][2][1], shtClr[c][2][2], 240);
  glVertex3f( width,  height, 0);
  glVertex3f(-width,  height, 0);
  glEnd();
  glPopMatrix();
}

void startDrawBoards() {
#ifdef __EMSCRIPTEN__
  /* Batched vertices are eye space, still to be projected -- so they must be
     drawn under the projection they were built for, not the next one. */
  rrFlush();
#endif
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  if ( rrPortrait ) {
    /* Cover the whole framebuffer so the strips above and below the field are
       drawable, and extend the ortho box in y only. x stays 0..640, so every
       existing HUD position remains on screen at its designed scale. */
    glViewport(0, 0, screenWidth, screenHeight);
    glOrtho(0, SCREEN_WIDTH, rrHudBottom, 0, -1, 1);
  } else {
    glOrtho(0, 640, 480, 0, -1, 1);
  }
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
}

void endDrawBoards() {
#ifdef __EMSCRIPTEN__
  rrFlush();
#endif
  glPopMatrix();
  screenResized();
}

static void drawBoard(int x, int y, int width, int height) {
  glColor4ub(0, 0, 0, 255);
  glBegin(GL_QUADS);
  glVertex2f(x,y);
  glVertex2f(x+width,y);
  glVertex2f(x+width,y+height);
  glVertex2f(x,y+height);
  glEnd();
}

void drawSideBoards() {
  if ( rrPortrait ) {
    /* No masking needed: the field exactly fills its own viewport, and glClear
       has already blacked out everything outside it.

       drawScore() and drawRPanel() both check the orientation and lay themselves
       out across the top strip, so neither needs a transform here. */
    drawScore();
    drawRPanel();
    return;
  }
  glDisable(GL_BLEND);
  drawBoard(0, 0, 160, 480);
  drawBoard(480, 0, 160, 480);
  glEnable(GL_BLEND);
  drawScore();
  drawRPanel();
}

void drawTitleBoard() {
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, titleTexture);
  glColor4ub(255, 255, 255, 255);
  glBegin(GL_TRIANGLE_FAN);
  glTexCoord2f(0.0f, 0.0f); 
  glVertex3f(350, 78,  0);
  glTexCoord2f(1.0f, 0.0f);
  glVertex3f(470, 78,  0);
  glTexCoord2f(1.0f, 1.0f);
  glVertex3f(470, 114,  0);
  glTexCoord2f(0.0f, 1.0f);
  glVertex3f(350, 114,  0);
  glEnd();
  glDisable(GL_TEXTURE_2D);
  glColor4ub(200, 200, 200, 255);
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(350, 30, 0);
  glVertex3f(400, 30, 0);
  glVertex3f(380, 56, 0);
  glVertex3f(380, 80, 0);
  glVertex3f(350, 80, 0);
  glEnd();
  glBegin(GL_TRIANGLE_FAN);
  glVertex3f(404, 80, 0);
  glVertex3f(404, 8, 0);
  glVertex3f(440, 8, 0);
  glVertex3f(440, 44, 0);
  glVertex3f(465, 80, 0);
  glEnd();
  glColor4ub(255, 255, 255, 255);
  glBegin(GL_LINE_LOOP);
  glVertex3f(350, 30, 0);
  glVertex3f(400, 30, 0);
  glVertex3f(380, 56, 0);
  glVertex3f(380, 80, 0);
  glVertex3f(350, 80, 0);
  glEnd();
  glBegin(GL_LINE_LOOP);
  glVertex3f(404, 80, 0);
  glVertex3f(404, 8, 0);
  glVertex3f(440, 8, 0);
  glVertex3f(440, 44, 0);
  glVertex3f(465, 80, 0);
  glEnd();
}

// Draw the numbers.
int drawNum(int n, int x ,int y, int s, int r, int g, int b) {
  for ( ; ; ) {
    drawLetter(n%10, x, y, s, 3, r, g, b);
    y += s*1.7f;
    n /= 10;
    if ( n <= 0 ) break;
  }
  return y;
}

int drawNumRight(int n, int x ,int y, int s, int r, int g, int b) {
  int d, nd, drawn = 0;
  for ( d = 100000000 ; d > 0 ; d /= 10 ) {
    nd = (int)(n/d);
    if ( nd > 0 || drawn ) {
      n -= d*nd;
      drawLetter(nd%10, x, y, s, 1, r, g, b);
      y += s*1.7f;
      drawn = 1;
    }
  }
  if ( !drawn ) {
    drawLetter(0, x, y, s, 1, r, g, b);
    y += s*1.7f;
  }
  return y;
}

int drawNumCenter(int n, int x ,int y, int s, int r, int g, int b) {
  for ( ; ; ) {
    drawLetter(n%10, x, y, s, 0, r, g, b);
    x -= s*1.7f;
    n /= 10;
    if ( n <= 0 ) break;
  }
  return y;
}

int drawTimeCenter(int n, int x ,int y, int s, int r, int g, int b) {
  int i;
  for ( i=0 ; i<7 ; i++ ) {
    if ( i != 4 ) {
      drawLetter(n%10, x, y, s, 0, r, g, b);
      n /= 10;
    } else {
      drawLetter(n%6, x, y, s, 0, r, g, b);
      n /= 6;
    }
    if ( (i&1) == 1 || i == 0 ) {
      switch ( i ) {
      case 3:
	drawLetter(41, x+s*1.16f, y, s, 0, r, g, b);
	break;
      case 5:
	drawLetter(40, x+s*1.16f, y, s, 0, r, g, b);
	break;
      }
      x -= s*1.7f;
    } else {
      x -= s*2.2f;
    }
    if ( n <= 0 ) break;
  }
  return y;
}

#define JOYSTICK_AXIS 16384

int getPadState() {
  int x = 0, y = 0;
  int hat = SDL_HAT_CENTERED;
  int pad = 0;
#ifdef __EMSCRIPTEN__
  /* Additive: zero unless a finger is steering, so keyboard and joystick are
     unaffected. moveShip() prefers the analog vector and ignores these bits;
     they exist for the menu screens, which are written against the d-pad. */
  pad |= rrTouchPad();
#endif
  if ( stick != NULL ) {
    x = SDL_JoystickGetAxis(stick, 0);
    y = SDL_JoystickGetAxis(stick, 1);
    if (SDL_JoystickNumHats(stick) > 0) {
      hat = SDL_JoystickGetHat(stick, 0);
    }
  }
  if ( keys[SDLK_RIGHT] == SDL_PRESSED || keys[SDLK_KP6] == SDL_PRESSED || x > JOYSTICK_AXIS || (hat & SDL_HAT_RIGHT)) {
    pad |= PAD_RIGHT;
  }
  if ( keys[SDLK_LEFT] == SDL_PRESSED || keys[SDLK_KP4] == SDL_PRESSED || x < -JOYSTICK_AXIS || (hat & SDL_HAT_LEFT)) {
    pad |= PAD_LEFT;
  }
  if ( keys[SDLK_DOWN] == SDL_PRESSED || keys[SDLK_KP2] == SDL_PRESSED || y > JOYSTICK_AXIS || (hat & SDL_HAT_DOWN)) {
    pad |= PAD_DOWN;
  }
  if ( keys[SDLK_UP] == SDL_PRESSED ||  keys[SDLK_KP8] == SDL_PRESSED || y < -JOYSTICK_AXIS || (hat & SDL_HAT_UP)) {
    pad |= PAD_UP;
  }
  return pad;
}

int buttonReversed = 0;

int getButtonState() {
  int btn = 0;
  int btn1 = 0, btn2 = 0, btn3 = 0, btn4 = 0;
  int btn5 = 0, btn6 = 0, btn7 = 0, btn8 = 0, btn9 = 0;
  if ( stick != NULL ) {
    btn1 = SDL_JoystickGetButton(stick, 0);
    btn2 = SDL_JoystickGetButton(stick, 1);
    btn3 = SDL_JoystickGetButton(stick, 2);
    btn4 = SDL_JoystickGetButton(stick, 3);
    btn5 = SDL_JoystickGetButton(stick, 4);
    btn6 = SDL_JoystickGetButton(stick, 5);
    btn7 = SDL_JoystickGetButton(stick, 6);
    btn8 = SDL_JoystickGetButton(stick, 7);
    btn9 = SDL_JoystickGetButton(stick, 9);
  }
  if ( keys[SDLK_z] == SDL_PRESSED || btn1 || btn4 ) {
    if ( !buttonReversed ) {
      btn |= PAD_BUTTON1;
    } else {
      btn |= PAD_BUTTON2;
    }
  }
  if ( keys[SDLK_x] == SDL_PRESSED || btn2 || btn3 ) {
    if ( !buttonReversed ) {
      btn |= PAD_BUTTON2;
    } else {
      btn |= PAD_BUTTON1;
    }
  }
  if (keys [SDLK_p] == SDL_PRESSED || btn5 || btn6 || btn7 || btn8 || btn9) {
    btn |= PAD_BUTTONP;
  }
#ifdef __EMSCRIPTEN__
  /* Escape pauses too, matching the on-screen button. It cannot keep its usual
     job of quitting here -- there is nothing to quit to in a browser tab, and
     doing so ends the run outright. See the quit check in rr.c.

     Note the browser keeps Escape to itself while the canvas is fullscreen, so
     there it leaves fullscreen instead of reaching the game. */
  if (keys[SDLK_ESCAPE] == SDL_PRESSED) {
    btn |= PAD_BUTTONP;
  }
#endif
#ifdef __EMSCRIPTEN__
  btn |= rrTouchButtons();
#endif
  return btn;
}
