/* error.c -- fatal error popup and log reporting
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <switch.h>

#include "error.h"
#include "util.h"

void fatal_error(const char *fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  debugPrintf("\n======================================================\n");
  debugPrintf("FATAL ERROR: %s\n", buf);
  debugPrintf("======================================================\n\n");
  debugPrintfFlush();

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);

  // Simple libnx console error screen
  consoleInit(NULL);
  printf("\x1b[2J\x1b[H");
  printf("=========================================\n");
  printf(" GTA III Switch Port - Fatal Error       \n");
  printf("=========================================\n\n");
  printf("%s\n\n", buf);
  printf("Please check gta3_log.txt for details.\n");
  printf("Press + or A to exit.\n");
  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    u64 kDown = padGetButtonsDown(&pad);
    if (kDown & (HidNpadButton_Plus | HidNpadButton_A))
      break;
  }

  consoleExit(NULL);
  exit(1);
}
