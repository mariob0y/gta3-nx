/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB reserved for the .so load region: it only holds the RW staging copies of
// the mapped libraries, and the rest of RAM goes to the newlib heap (game malloc
// + mesa GPU bos).
#define MEMORY_SO_MB 64

// Dynamic library names
#define SO_NAME "libs/libGame.so"
#define CXX_DONOR_SO_NAME "libs/libc++_shared.so"
#define CONFIG_NAME "gta3_nx.cfg"
#define LOG_NAME "gta3_log.txt"
#define APPSTATE_NAME "appstate.txt"

// Enable debug log file writing and stdout printing (off for release)
#define DEBUG_LOG 1

// Actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int fps_cap_30;
  int log_mask;
  float streaming_budget_ms;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
