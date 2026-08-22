/* error.h -- fatal error reporting
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

#ifdef __cplusplus
extern "C" {
#endif

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
