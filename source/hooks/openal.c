/* openal.c -- OpenAL audio hooks
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "../util.h"
#include "../hooks.h"

static ALCcontext *al_ctx = NULL;
static ALCdevice *al_dev = NULL;

ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused) {
  debugPrintf("[AUDIO] alcCreateContextHook: Overriding sample rate to 44100Hz (dev=%p)...\n", dev);
  const ALCint attr[] = { ALC_FREQUENCY, 44100, 0 };
  int cls = thread_class_push(THREAD_CLASS_AUDIO);
  al_ctx = alcCreateContext(dev, attr);
  thread_class_pop(cls);
  debugPrintf("[AUDIO] alcCreateContext -> %p\n", al_ctx);
  return al_ctx;
}

ALCdevice *alcOpenDeviceHook(const char *name) {
  debugPrintf("[AUDIO] alcOpenDeviceHook: Opening audio device '%s'...\n", name ? name : "default");
  int cls = thread_class_push(THREAD_CLASS_AUDIO);
  al_dev = alcOpenDevice(name);
  thread_class_pop(cls);
  debugPrintf("[AUDIO] alcOpenDevice -> %p\n", al_dev);
  return al_dev;
}

void deinit_openal(void) {
  if (al_dev) {
    debugPrintf("[AUDIO] Closing OpenAL device\n");
    if (al_ctx) {
      alcMakeContextCurrent(NULL);
      alcDestroyContext(al_ctx);
      al_ctx = NULL;
    }
    alcCloseDevice(al_dev);
    al_dev = NULL;
  }
}
