/*
 * $Id: rr.h,v 1.4 2003/04/26 03:24:16 kenta Exp $
 *
 * Copyright 2003 Kenta Cho. All rights reserved.
 */

/**
 * rRootage header file.
 *
 * @version $Revision: 1.4 $
 */
#define CAPTION "rRootage"
#define VERSION_NUM 22

#define INTERVAL_BASE 16

extern int status;
extern int interval;
extern int tick;

/* Render interpolation, opt-in with ?interp=1.
   The simulation is a fixed 16ms step, so on a 120Hz panel roughly every other
   frame recomputes nothing and redraws an identical picture. When rrInterp is
   set, movers are drawn between their previous and current position instead,
   rrLerp being how far through the current step the wall clock has got.
   Simulation timing is untouched -- only the drawing moves. */
extern int   rrInterp;
extern float rrLerp;

/* Position to draw a mover at. rrLerp is 1.0 when interpolation is off, so this
   collapses to the current position exactly -- these are integers well inside
   float's exact range, so the flag costs nothing in fidelity when unset. */
#define RR_LERP(prev, cur) ((float)(prev) + (float)((cur) - (prev)) * rrLerp)

#define TITLE 0
#define IN_GAME 1
#define GAMEOVER 2
#define STAGE_CLEAR 3
#define PAUSE 4

void quitLast();
void initTitleStage(int stg);
void initTitle();
void initGame();
void initGameover();
