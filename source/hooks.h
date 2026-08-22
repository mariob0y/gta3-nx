/* hooks.h -- declarations for game and OpenAL hooks
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <stdint.h>
#include <AL/alc.h>

#ifdef __cplusplus
extern "C" {
#endif

void patch_game(void);
void cheats_enqueue(const char *code);
void keep_game_frame_limiter_off(void);
void reset_frame_streaming_budget(void);

ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused);
ALCdevice *alcOpenDeviceHook(const char *name);
void deinit_openal(void);

#ifdef __cplusplus
}
#endif

#endif
