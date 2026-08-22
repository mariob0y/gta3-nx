/* overlay.h -- on-screen FPS counter drawn in the eglSwapBuffers hook
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __OVERLAY_H__
#define __OVERLAY_H__

#ifdef __cplusplus
extern "C" {
#endif

// eglSwapBuffers hook drawing FPS overlay
unsigned int eglSwapBuffersHook(void *display, void *surface);

#ifdef __cplusplus
}
#endif

#endif
