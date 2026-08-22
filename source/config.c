/* config.c -- configuration file parsing and saving
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "util.h"

int screen_width = 1280;
int screen_height = 720;

Config config = {
  .screen_width = -1,
  .screen_height = -1,
  .trilinear_filter = 1,
  .fps_cap_30 = 1,
  .log_mask = LOGC_SYS | LOGC_FILE | LOGC_THREAD,
  .streaming_budget_ms = 5.0f,
};

int read_config(const char *file) {
  FILE *f = fopen(file, "r");
  if (!f) {
    debugPrintf("[CONFIG] Config file %s not found, using default configuration.\n", file);
    return -1;
  }

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char key[64], val[64];
    if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
      if (strcmp(key, "screen_width") == 0) config.screen_width = atoi(val);
      else if (strcmp(key, "screen_height") == 0) config.screen_height = atoi(val);
      else if (strcmp(key, "trilinear_filter") == 0) config.trilinear_filter = atoi(val);
      else if (strcmp(key, "fps_cap_30") == 0) config.fps_cap_30 = atoi(val);
      else if (strcmp(key, "log_mask") == 0) config.log_mask = (int)strtoul(val, NULL, 0);
      else if (strcmp(key, "streaming_budget_ms") == 0) config.streaming_budget_ms = (float)atof(val);
    }
  }

  fclose(f);
  g_log_mask = (unsigned)config.log_mask;
  debugPrintf("[CONFIG] Successfully loaded config from %s (log_mask=0x%x, streaming_budget=%.1fms)\n",
              file, g_log_mask, config.streaming_budget_ms);
  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) {
    debugPrintf("[CONFIG] Failed to open config file %s for writing!\n", file);
    return -1;
  }

  fprintf(f, "screen_width=%d\n", config.screen_width);
  fprintf(f, "screen_height=%d\n", config.screen_height);
  fprintf(f, "trilinear_filter=%d\n", config.trilinear_filter);
  fprintf(f, "fps_cap_30=%d\n", config.fps_cap_30);
  fprintf(f, "log_mask=0x%x\n", config.log_mask);
  fprintf(f, "streaming_budget_ms=%.1f\n", config.streaming_budget_ms);

  fclose(f);
  debugPrintf("[CONFIG] Successfully saved config to %s\n", file);
  return 0;
}
