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

/* Largest block is drawCircle: a centre vertex plus 16 loop iterations of two. */
#define RR_MAX_BATCH_VERTS 256

static GLfloat rrVtx[RR_MAX_BATCH_VERTS * 3];
static GLubyte rrCol[RR_MAX_BATCH_VERTS * 4];
static GLfloat rrTex[RR_MAX_BATCH_VERTS * 2];

static int     rrCount   = 0;
static GLenum  rrMode    = GL_TRIANGLE_FAN;
static int     rrHasTex  = 0;
static GLubyte rrCurCol[4] = { 255, 255, 255, 255 };
static GLfloat rrCurTex[2] = { 0.0f, 0.0f };

static void rrBegin(GLenum mode) {
  /* GLES2/WebGL has no GL_QUADS, and emscripten's glDrawArrays path excludes
     it explicitly. The one GL_QUADS block here is a single four-vertex
     rectangle, for which a triangle fan over the same corners is identical. */
  rrMode   = (mode == GL_QUADS) ? GL_TRIANGLE_FAN : mode;
  rrCount  = 0;
  rrHasTex = 0;
}

/* Colour is latched, not forwarded: every vertex gets an explicit copy, so the
   fixed-function current-colour state is never consulted. Calls that happen
   outside a block (the common case here) simply set the colour used by the
   next block, which is what the original code relies on. */
static void rrColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
  rrCurCol[0] = r; rrCurCol[1] = g; rrCurCol[2] = b; rrCurCol[3] = a;
}

static void rrTexCoord2f(GLfloat u, GLfloat v) {
  rrCurTex[0] = u; rrCurTex[1] = v;
  rrHasTex = 1;
}

static void rrPushVertex(GLfloat x, GLfloat y, GLfloat z) {
  int i = rrCount;
  if (i >= RR_MAX_BATCH_VERTS) return;
  rrVtx[i*3+0] = x; rrVtx[i*3+1] = y; rrVtx[i*3+2] = z;
  rrCol[i*4+0] = rrCurCol[0]; rrCol[i*4+1] = rrCurCol[1];
  rrCol[i*4+2] = rrCurCol[2]; rrCol[i*4+3] = rrCurCol[3];
  rrTex[i*2+0] = rrCurTex[0]; rrTex[i*2+1] = rrCurTex[1];
  rrCount++;
}

static void rrVertex3f(GLfloat x, GLfloat y, GLfloat z) { rrPushVertex(x, y, z); }
static void rrVertex2f(GLfloat x, GLfloat y)            { rrPushVertex(x, y, 0.0f); }

static void rrEnd(void) {
  if (rrCount == 0) return;

  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, rrVtx);
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_UNSIGNED_BYTE, 0, rrCol);
  if (rrHasTex) {
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, rrTex);
  }

  glDrawArrays(rrMode, 0, rrCount);

  if (rrHasTex) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  rrCount = 0;
}

#define glBegin      rrBegin
#define glEnd        rrEnd
#define glVertex3f   rrVertex3f
#define glVertex2f   rrVertex2f
#define glColor4ub   rrColor4ub
#define glTexCoord2f rrTexCoord2f
#endif /* __EMSCRIPTEN__ */

#define FAR_PLANE 720

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define LOWRES_SCREEN_WIDTH 320
#define LOWRES_SCREEN_HEIGHT 240

static int screenWidth, screenHeight;

// Reset viewport when the screen is resized.
static void screenResized() {
  int viewportWidth = screenWidth;
  int viewportHeight = screenHeight;
  // Maintain aspect ratio between SCREEN_WIDTH and SCREEN_HEIGHT.
  if (screenHeight * SCREEN_WIDTH  > screenWidth * SCREEN_HEIGHT) {
    viewportHeight = SCREEN_HEIGHT * screenWidth / SCREEN_WIDTH;
  } else {
    viewportWidth = SCREEN_WIDTH * screenHeight / SCREEN_HEIGHT;
  }
  int offsetX = (screenWidth - viewportWidth) / 2;
  int offsetY = (screenHeight - viewportHeight) / 2;
  glViewport(offsetX, offsetY, viewportWidth, viewportHeight);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(45.0f, (GLfloat)viewportWidth/(GLfloat)viewportHeight, 0.1f, FAR_PLANE);
  glMatrixMode(GL_MODELVIEW);
}

void resized(int width, int height) {
  screenWidth = width; screenHeight = height;
  screenResized();
}

#ifdef __EMSCRIPTEN__
/*
 * Render at the display's real pixel density instead of a fixed 640x480.
 *
 * SDL_SetVideoMode gives the canvas a 640x480 backing store, which the browser
 * then scales to whatever size the element occupies. On a 2x display that is
 * already a doubling when windowed, and going fullscreen stretches 640x480
 * across the whole screen -- hence the blur. Nothing here is a low-resolution
 * asset: the only bitmaps are two 128x128 glow sprites and the 150x36 logo,
 * and everything else is vector geometry generated per frame, so it re-renders
 * sharp at any size for free.
 *
 * screenResized() already letterboxes the viewport to 4:3 and rebuilds the
 * projection from it, and the HUD pass uses its own glOrtho(0,640,480,...)
 * that maps onto the viewport rather than the framebuffer. So the backing
 * store can be any size and the game's own coordinate systems are unaffected.
 */
/*
 * Ceiling on the framebuffer's long edge.
 *
 * Native density on a large display is a lot of pixels: a 2056x1329 CSS screen
 * at devicePixelRatio 2 works out to 4112x2658, about 11 megapixels. The game
 * clears and additively blends the whole frame and issues several hundred small
 * primitives, so fill rate is what gives out first, and vector art gains very
 * little from the last doubling. 1920 keeps it visibly sharp for roughly a fifth
 * of the pixels.
 *
 * Only the backing store is capped -- the CSS box still fills the screen, so the
 * browser scales the result up. Raise this if you have GPU headroom and want
 * fullscreen closer to native; 2560 or 3840 are reasonable steps.
 */
#define RR_MAX_FB_LONG_EDGE 1920

static void syncCanvasToDisplay(void) {
  EmscriptenFullscreenChangeEvent fs;
  double dpr = emscripten_get_device_pixel_ratio();
  double cssW, cssH;
  int w, h, longEdge;

  /* The on-screen box and the backing store have to be set independently. A
     canvas with no CSS size takes its layout size *from* its backing store, so
     raising canvas.width on its own just makes the element twice as big at the
     same density -- no sharper. Pin the CSS size first, then give it that many
     device pixels. */
  if (emscripten_get_fullscreen_status(&fs) == EMSCRIPTEN_RESULT_SUCCESS && fs.isFullscreen) {
    cssW = fs.screenWidth;
    cssH = fs.screenHeight;
  } else {
    cssW = SCREEN_WIDTH;
    cssH = SCREEN_HEIGHT;
  }
  if (cssW <= 0 || cssH <= 0) return;

  emscripten_set_element_css_size("#canvas", cssW, cssH);

  w = (int)(cssW * dpr + 0.5);
  h = (int)(cssH * dpr + 0.5);
  if (w <= 0 || h <= 0) return;

  longEdge = (w > h) ? w : h;
  if (longEdge > RR_MAX_FB_LONG_EDGE) {
    double scale = (double)RR_MAX_FB_LONG_EDGE / (double)longEdge;
    w = (int)(w * scale + 0.5);
    h = (int)(h * scale + 0.5);
    if (w <= 0 || h <= 0) return;
  }

  emscripten_set_canvas_element_size("#canvas", w, h);
  resized(w, h);
}

/* Applied at the top of the next frame rather than inline. Emscripten's own
   requestFullscreen path may also set the canvas size (the shell's "Resize
   canvas" checkbox decides), and both handlers run in the same task, so doing
   this immediately makes the winner depend on listener order. Deferring one
   frame means ours is always the last write. */
static int canvasSyncPending = 1;

static EM_BOOL onBrowserResize(int type, const EmscriptenUiEvent *e, void *user) {
  (void)type; (void)e; (void)user;
  canvasSyncPending = 1;
  return EM_FALSE;
}

static EM_BOOL onFullscreenChange(int type, const EmscriptenFullscreenChangeEvent *e, void *user) {
  (void)type; (void)e; (void)user;
  canvasSyncPending = 1;
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
  if (canvasSyncPending) {
    canvasSyncPending = 0;
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
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0, 640, 480, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
}

void endDrawBoards() {
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
  return btn;
}
