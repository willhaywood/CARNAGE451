/*
 * $Id: soundmanager.c,v 1.3 2003/04/26 03:24:16 kenta Exp $
 *
 * Copyright 2003 Kenta Cho. All rights reserved.
 */

/**
 * BGM/SE manager(using SDL_mixer).
 *
 * @version $Revision: 1.3 $
 */
#include "SDL.h"
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "SDL_mixer.h"
#include "soundmanager.h"

static int useAudio = 0;

#define MUSIC_NUM 3

/* Base names; the extension is chosen per-browser at load time, see below. */
static char *musicFileName[MUSIC_NUM] = {
  "stg_a", "stg_b", "stg_c",
};

/*
 * Candidate BGM containers, tried in order, first one that decodes wins.
 *
 * Only .mp3 is shipped, so it is first: probing a container that is not in the
 * package costs a failed load and a console error per track on every startup.
 * .ogg is kept as a second candidate purely so that dropping the original
 * Vorbis files back into rr/sounds/ works with no code change -- they are the
 * better encode (the MP3 is a lossy-to-lossy transcode of them), just not worth
 * 2.3 MB of payload that every visitor downloads.
 *
 * MP3 rather than AAC/.m4a, which was tried first and does not work here:
 * emscripten's asset preloader only decodes .ogg, .wav and .mp3 (see
 * audioPlugin.canHandle in src/lib/libbrowser.js). An .m4a is packaged but never
 * decoded, and Mix_LoadMUS still returns a non-NULL handle for it -- one with no
 * audio behind it, so the load reports success and then plays silence. MP3 is
 * decoded by the toolchain and supported by every browser including all iOS
 * versions, so it buys the same compatibility with none of that.
 *
 * The effects are all .wav, which every browser decodes, so they need no
 * fallback. Only the three music tracks do.
 */
static const char *musicExt[] = { ".mp3", ".ogg" };
#define MUSIC_EXT_NUM ((int)(sizeof(musicExt) / sizeof(musicExt[0])))
static Mix_Music *music[MUSIC_NUM];

#define CHUNK_NUM 16

static char *chunkFileName[CHUNK_NUM] = {
  "laser_start.wav", "laser.wav", "damage.wav", "bomb.wav", 
  "destroied.wav", "explosion1.wav", "explosion2.wav", "miss.wav", "extend.wav",
  "grz.wav", "grzinv.wav", 
  "shot.wav", "change.wav",
  "reflec1.wav", "reflec2.wav", "ref_ready.wav",
};
static Mix_Chunk *chunk[CHUNK_NUM];
static int chunkChannel[CHUNK_NUM] = {
  0, 1, 2, 3,
  4, 5, 6, 7, 4,
  6, 7,
  6, 7,
  7, 7, 7,
};

void closeSound() {
  int i;
  if ( !useAudio ) return;
  if ( Mix_PlayingMusic() ) {
    Mix_HaltMusic();
  }
  for ( i=0 ; i<MUSIC_NUM ; i++ ) {
    if ( music[i] ) {
      Mix_FreeMusic(music[i]);
    }
  }
  for ( i=0 ; i<CHUNK_NUM ; i++ ) {
    if ( chunk[i] ) {
      Mix_FreeChunk(chunk[i]);
    }
  }
  Mix_CloseAudio();
}


// Initialize the sound.

/*
 * Load what can be loaded and keep going.
 *
 * Originally this bailed out on the first failure and cleared useAudio, which
 * meant a single undecodable file silenced the entire game. That is a bad trade
 * in a browser: the three .ogg BGM tracks are loaded before the sixteen .wav
 * effects, so any codec, network or truncation problem with the music also took
 * every sound effect with it -- and silently, since audio failure is not fatal
 * to startup and there is no on-screen indication.
 *
 * Missing entries stay NULL and the play* functions below skip them, so a
 * partial load degrades to "no music" or "one missing effect" rather than
 * "no audio at all". useAudio is only cleared if nothing loaded whatsoever.
 */
static void loadSounds() {
  int i;
  int musicOk = 0, chunkOk = 0;
  char name[32];

  for ( i=0 ; i<MUSIC_NUM ; i++ ) {
    int e;
    music[i] = NULL;
    for ( e=0 ; e<MUSIC_EXT_NUM && !music[i] ; e++ ) {
      strcpy(name, "sounds/");
      strcat(name, musicFileName[i]);
      strcat(name, musicExt[e]);
      music[i] = Mix_LoadMUS(name);
      if ( music[i] && e > 0 ) {
        /* Worth knowing: it means the preferred container was rejected. */
        printf("Audio: %s fell back to %s\n", musicFileName[i], musicExt[e]);
      }
    }
    if ( music[i] ) {
      musicOk++;
    } else {
      fprintf(stderr, "Couldn't load %s (tried all %d containers), skipping\n",
              musicFileName[i], MUSIC_EXT_NUM);
    }
  }
  for ( i=0 ; i<CHUNK_NUM ; i++ ) {
    strcpy(name, "sounds/");
    strcat(name, chunkFileName[i]);
    chunk[i] = Mix_LoadWAV(name);
    if ( chunk[i] ) {
      chunkOk++;
    } else {
      fprintf(stderr, "Couldn't load (skipping): %s\n", name);
    }
  }

  /* Always report the tally. A partial load used to be invisible, which is how
     "no sound on that browser" turns into a long hunt somewhere else. */
  printf("Audio: %d/%d music, %d/%d effects loaded\n",
         musicOk, MUSIC_NUM, chunkOk, CHUNK_NUM);

  if ( musicOk == 0 && chunkOk == 0 ) {
    fprintf(stderr, "Audio: nothing loaded, disabling sound\n");
    useAudio = 0;
  }
}

void initSound() {
  int audio_rate;
  Uint16 audio_format;
  int audio_channels;
  int audio_buffers;

  if ( SDL_InitSubSystem(SDL_INIT_AUDIO) < 0 ) {
    fprintf(stderr, "Unable to initialize SDL_AUDIO: %s\n", SDL_GetError());
    return;
  }

  audio_rate = 44100;
  audio_format = AUDIO_S16;
  audio_channels = 1;
  audio_buffers = 4096;
  
  if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers) < 0) {
    fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
    return;
  } else {
#ifndef __EMSCRIPTEN__
    /* emscripten's SDL_mixer aborts on Mix_QuerySpec. The queried values are
       only written back into these locals and never read, so skipping it
       changes nothing. */
    Mix_QuerySpec(&audio_rate, &audio_format, &audio_channels);
#endif
  }

  useAudio = 1;
  loadSounds();
}

// Play/Stop the music/chunk.

void playMusic(int idx) {
  if ( !useAudio ) return;
  if ( idx < 0 || idx >= MUSIC_NUM || !music[idx] ) return;   /* skipped at load */
  Mix_PlayMusic(music[idx], -1);
}

void fadeMusic() {
  if ( !useAudio ) return;
  Mix_FadeOutMusic(1280);
}

void stopMusic() {
  if ( !useAudio ) return;
  if ( Mix_PlayingMusic() ) {
    Mix_HaltMusic();
  }
}

void playChunk(int idx) {
  if ( !useAudio ) return;
  if ( idx < 0 || idx >= CHUNK_NUM || !chunk[idx] ) return;    /* skipped at load */
  Mix_PlayChannel(chunkChannel[idx], chunk[idx], 0);
}

void haltChunk(int idx) {
  if ( !useAudio ) return;
  if ( idx < 0 || idx >= CHUNK_NUM ) return;
  Mix_HaltChannel(chunkChannel[idx]);
}
