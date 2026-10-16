/* profiler.h -- per-frame engine phase profile
 *
 * Wrapped engine functions add their wall time to a slot; once per frame the
 * slots are folded into a window, and every PROF_WINDOW frames one [PROFILE]
 * line reports each slot's average, p90 and worst per-frame cost. Nested slots
 * (CWorld::Process inside CGame::Process) are reported as-is, so the parent
 * already includes the child. */
#ifndef __PROFILER_H__
#define __PROFILER_H__

#include <stdint.h>

#define PROF_WINDOW 300

/* Registers a slot and returns its id, or -1 if the table is full. depth is
 * only used to indent the report. */
int prof_register(const char *name, int depth);
void prof_add(int slot, uint64_t ns);

/* Draw calls, counted by the GL wrappers in imports.c. */
extern uint32_t g_prof_draws;

/* Folds this frame into the window; total_ns is the whole implOnDrawFrame. */
void prof_end_frame(uint64_t total_ns);

#endif
