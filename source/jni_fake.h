/* jni_fake.h -- fake JNI environment for the GTA:III oswrapper/GameNative layer
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

extern volatile int jni_quit_requested;
extern volatile int jni_frontend_ready;

void jni_init(void);

typedef enum {
  JNI_CB_PLAYLIST_OPEN_COMPLETE = 1,
  JNI_CB_ROCKSTAR_INITIAL_COMPLETE,
  JNI_CB_ROCKSTAR_GATE_COMPLETE,
  JNI_CB_ROCKSTAR_SIGNIN_COMPLETE,
  JNI_CB_ROCKSTAR_SIGNOUT_COMPLETE,
} JniCallbackType;

typedef struct {
  JniCallbackType type;
  int arg0, arg1;
} JniCallback;

int jni_pop_callback(JniCallback *out);

void *jni_make_string(const char *utf);
void *jni_make_string_array(int n, const char **strs);
void *jni_make_object(const char *label);

#ifdef __cplusplus
}
#endif

#endif
