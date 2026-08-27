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
#define SO_NAME "lib/libGame.so"
#define CXX_DONOR_SO_NAME "lib/libc++_shared.so"
#define CONFIG_NAME "gta3_nx.cfg"
#define LOG_NAME "gta3_log.txt"
#define APPSTATE_NAME "appstate.txt"

// Save slots go here rather than beside the NRO; gta3.set stays in the root.
// Relative to the storage root, created on first write.
#define USER_DATA_DIR "saves"

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
  int show_fps_overlay;   // on-screen FPS/frame-time overlay (off by default)
  int debug_log_enabled;  // write gta3_log.txt to disk (off by default)
  int intro_movies;       // play the Rockstar logo / title movies at boot
  /* 0 (default): preload nothing; the engine streams as it always did, and
   *    the 3x3 LoadScene grid still runs. The default because it is the
   *    behaviour the game has always had.
   * 1: instantiate every texture dictionary before gameplay and skip the grid.
   *
   * Measured on hardware, same build and route, stalls per 1000 gameplay
   * frames: 0 gives a ~13s load and 2765ms, 1 gives ~25s and 222ms. So about
   * twelve extra seconds of loading buys an order of magnitude less stutter.
   * See README.md and the block comment above run_one_time_txd_preload. */
  int full_preload;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
