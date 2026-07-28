/*
 * $Id: rr.c,v 1.4 2003/04/26 03:24:16 kenta Exp $
 *
 * Copyright 2003 Kenta Cho. All rights reserved.
 */

/**
 * rRootage main routine.
 *
 * @version $Revision: 1.4 $
 */
#include "SDL.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rr.h"
#include "screen.h"
#include "vector.h"
#include "foe_mtd.h"
#include "brgmng_mtd.h"
#include "degutil.h"
#include "boss_mtd.h"
#include "ship.h"
#include "laser.h"
#include "frag.h"
#include "shot.h"
#include "background.h"
#include "soundmanager.h"
#include "attractmanager.h"
#include "touch.h"

static int noSound = 0;

// Initialize and load preference.
static void initFirst() {
  time_t timer;
  time(&timer);
  srand(timer);

  loadPreference();
  initBarragemanager();
  initAttractManager();
  if ( !noSound ) initSound();
  initGameStateFirst();
}

// Quit and save preference.
void quitLast() {
  if ( !noSound ) closeSound();
  savePreference();
  closeFoes();
  closeBarragemanager();
  closeSDL();
  SDL_Quit();
  exit(1);
}

int status;

void initTitleStage(int stg) {
  initFoes();
  initStageState(stg);
}

void initTitle() {
  int stg;
  status = TITLE;

  stg = initTitleAtr();
  initBoss();
  initShip();
  initLasers();
  initFrags();
  initShots();
  initBackground(0);
  initTitleStage(stg);
  left = -1;
}

void initGame(int stg) {
  int sn;
  status = IN_GAME;

  initBoss();
  initFoes();
  initShip();
  initLasers();
  initFrags();
  initShots();

  initGameState(stg);
  sn = stg%SAME_RANK_STAGE_NUM;
  initBackground(sn);
  if ( sn == SAME_RANK_STAGE_NUM-1 ) {
    playMusic(rand()%(SAME_RANK_STAGE_NUM-1));
  } else {
    playMusic(sn);
  }
}

void initGameover() {
  status = GAMEOVER;
  initGameoverAtr();
}

static void move() {
  switch ( status ) {
  case TITLE:
    moveTitleMenu();
    moveBoss();
    moveFoes();
    moveBackground();
    break;
  case IN_GAME:
  case STAGE_CLEAR:
    moveShip();
    moveBoss();
    moveLasers();
    moveShots();
    moveFoes();
    moveFrags();
    moveBackground();
    break;
  case GAMEOVER:
    moveGameover();
    moveBoss();
    moveFoes();
    moveFrags();
    moveBackground();
    break;
  case PAUSE:
    movePause();
    break;
  }
  moveScreenShake();
}

static void draw() {
  switch ( status ) {
  case TITLE:
    drawBackground();
    drawBoss();
    drawBulletsWake();
    drawBullets();
    startDrawBoards();
    drawSideBoards();
    drawTitle();
    endDrawBoards();
    break;
  case IN_GAME:
  case STAGE_CLEAR:
    drawBackground();
    drawBoss();
    drawLasers();
    drawShots();
    drawBulletsWake();
    drawFrags();
    drawShip();
    drawBullets();
    startDrawBoards();
    drawSideBoards();
    drawBossState();
    endDrawBoards();
    break;
  case GAMEOVER:
    drawBackground();
    drawBoss();
    drawBulletsWake();
    drawFrags();
    drawBullets();
    startDrawBoards();
    drawSideBoards();
    drawGameover();
    endDrawBoards();
    break;
  case PAUSE:
    drawBackground();
    drawBoss();
    drawLasers();
    drawShots();
    drawBulletsWake();
    drawFrags();
    drawShip();
    drawBullets();
    startDrawBoards();
    drawSideBoards();
    drawBossState();
    drawPause();
    endDrawBoards();
    break;
  }
}

static int accframe = 0;

static void usage(char *argv0) {
  fprintf(stderr, "Usage: %s [-lowres] [-nosound] [-fullscreen] [-reverse] [-nowait] [-accframe]\n", argv0);
}

static void parseArgs(int argc, char *argv[]) {
  int i;
  for ( i=1 ; i<argc ; i++ ) {
    if ( strcmp(argv[i], "-lowres") == 0 ) {
      lowres = 1;
    } else if ( strcmp(argv[i], "-nosound") == 0 ) {
      noSound = 1;
    } else if ( strcmp(argv[i], "-fullscreen") == 0 ) {
      windowMode = 0;
    } else if ( strcmp(argv[i], "-reverse") == 0 ) {
      buttonReversed = 1;
    }
    /* else if ( (strcmp(argv[i], "-brightness") == 0) && argv[i+1] ) {
      i++;
      brightness = (int)atoi(argv[i]);
      if ( brightness < 0 || brightness > 256 ) {
	brightness = DEFAULT_BRIGHTNESS;
      }
      }*/ 
    else if ( strcmp(argv[i], "-nowait") == 0 ) {
      nowait = 1;
    } else if ( strcmp(argv[i], "-accframe") == 0 ) {
      accframe = 1;
    } else {
      usage(argv[0]);
      exit(1);
    }
  }
}

int interval = INTERVAL_BASE;
int tick = 0;
static int pPrsd = 1;

/* Loop state hoisted to file scope so the body can run as a callback.
   `event` was a main() local that persisted across iterations; keeping it
   static preserves that (SDL_PollEvent leaves it untouched when idle). */
static int done = 0;
static long prvTickCount = 0;
static SDL_Event event;

static void mainLoopIteration(void) {
  int i;
  long nowTick;
  int frame;
  int buttons;

  SDL_PollEvent(&event);
  keys = SDL_GetKeyState(NULL);
  buttons = getButtonState();
#ifdef __EMSCRIPTEN__
  /* Escape is a pause here, not a quit -- getButtonState() reports it as
     PAD_BUTTONP -- so only a real quit event ends the loop. */
  if ( event.type == SDL_QUIT ) done = 1;
#else
  if ( keys[SDLK_ESCAPE] == SDL_PRESSED || event.type == SDL_QUIT ) done = 1;
#endif
#ifdef __EMSCRIPTEN__
  /* Menu button: abandon the run and go back to the title. Banks the score
     first, exactly as dying does, so quitting never loses it. No edge flag is
     needed -- the pulse outlives one frame, but initTitle() changes status, so
     the test fails on every frame after the first. */
  if ( rrTouchMenu() && ( status == IN_GAME || status == PAUSE ) ) {
    setHiScore(0);
    initTitle();
  }
#endif
  if ( buttons & PAD_BUTTONP ) {
    if ( !pPrsd ) {
      if ( status == IN_GAME ) {
	status = PAUSE;
      } else if ( status == PAUSE ) {
	status = IN_GAME;
      }
    }
    pPrsd = 1;
  } else {
    pPrsd = 0;
  }
  if ( event.type == SDL_VIDEORESIZE ) {
    resized(event.resize.w, event.resize.h);
  }

  nowTick = SDL_GetTicks();
  frame = (int)(nowTick-prvTickCount) / interval;
  if ( frame <= 0 ) {
#ifdef __EMSCRIPTEN__
    /* Not enough wall time has passed for a 16ms step yet.
       Natively the SDL_Delay below sleeps until a step is genuinely due, which
       is what makes forcing frame=1 correct. That delay cannot stay in the
       browser (it would block the event loop), and requestAnimationFrame fires
       every ~8.3ms on a 120Hz display and ~6.9ms on 144Hz -- so forcing a step
       here would advance the simulation once per refresh and run the whole game
       fast, roughly 2x on 120Hz. Render this callback without stepping and
       leave prvTickCount alone so the elapsed time carries into the next one. */
    frame = 0;
#else
    frame = 1;
    SDL_Delay(prvTickCount+interval-nowTick);
    if ( accframe ) {
      prvTickCount = SDL_GetTicks();
    } else {
      prvTickCount += interval;
    }
#endif
  } else if ( frame > 5 ) {
    frame = 5;
    prvTickCount = nowTick;
  } else {
    prvTickCount += frame*interval;
  }
  for ( i=0 ; i<frame ; i++ ) {
    move();
    tick++;
  }

  drawGLSceneStart();
  draw();
  drawGLSceneEnd();
  swapGLScene();

#ifdef __EMSCRIPTEN__
  if ( done ) {
    quitLast();
    emscripten_cancel_main_loop();
  }
#endif
}

int main(int argc, char *argv[]) {
  windowMode = 1;
  parseArgs(argc, argv);

  initDegutil();
  initSDL();
  initFirst();
  initTitle();

#ifdef __EMSCRIPTEN__
  /* 0 fps => drive from requestAnimationFrame. */
  emscripten_set_main_loop(mainLoopIteration, 0, 1);
#else
  while ( !done ) {
    mainLoopIteration();
  }
  quitLast();
#endif
  return 0;
}
