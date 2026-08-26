/* imports.h -- .so import resolution
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

extern FILE *stderr_fake;
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

void update_imports(void);

/* Presents one movie frame through the same swap wrapper the engine uses, so
 * the GL state cache is reset and the engine's own swaps stay suppressed for
 * the duration. See source/movie.c. */
unsigned int nx_present_movie_frame(void *display, void *surface);

#ifdef __cplusplus
}
#endif

#endif
