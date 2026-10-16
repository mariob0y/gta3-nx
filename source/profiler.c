/* profiler.c -- per-frame engine phase profile, see profiler.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profiler.h"
#include "util.h"

#define PROF_MAX_SLOTS 64

typedef struct {
  const char *name;
  int depth;
  uint64_t frame_ns;           /* accumulated during the current frame */
  uint32_t calls;              /* calls during the current frame */
  uint32_t window_calls;
  float window_ms[PROF_WINDOW];
} ProfSlot;

static ProfSlot s_slots[PROF_MAX_SLOTS];
static int s_num_slots = 0;
static float s_total_ms[PROF_WINDOW];
static float s_draws[PROF_WINDOW];
static int s_frame = 0;

uint32_t g_prof_draws = 0;

int prof_register(const char *name, int depth) {
  if (s_num_slots >= PROF_MAX_SLOTS) return -1;
  s_slots[s_num_slots].name = name;
  s_slots[s_num_slots].depth = depth;
  return s_num_slots++;
}

void prof_add(int slot, uint64_t ns) {
  if (slot < 0) return;
  s_slots[slot].frame_ns += ns;
  s_slots[slot].calls++;
}

static int cmp_float(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

/* avg, p90 and max of n samples; sorts a scratch copy. */
static void stats(const float *v, int n, float *avg, float *p90, float *max) {
  static float tmp[PROF_WINDOW];
  memcpy(tmp, v, sizeof(float) * n);
  qsort(tmp, n, sizeof(float), cmp_float);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += tmp[i];
  *avg = (float)(sum / n);
  *p90 = tmp[(n * 9) / 10];
  *max = tmp[n - 1];
}

static void report(void) {
  float a, p, m;
  stats(s_total_ms, PROF_WINDOW, &a, &p, &m);
  float da, dp, dm;
  stats(s_draws, PROF_WINDOW, &da, &dp, &dm);
  LOGC(LOGC_SYS, "[PROFILE] frame #%u, last %d frames: implOnDrawFrame avg %.2f p90 %.2f max %.2f ms | draws avg %.0f p90 %.0f max %.0f\n",
       g_frame_count, PROF_WINDOW, a, p, m, da, dp, dm);
  for (int i = 0; i < s_num_slots; i++) {
    ProfSlot *s = &s_slots[i];
    if (s->window_calls == 0) continue;
    stats(s->window_ms, PROF_WINDOW, &a, &p, &m);
    LOGC(LOGC_SYS, "[PROFILE]   %*s%-36s avg %6.2f p90 %6.2f max %7.2f ms  (%.1f calls/frame)\n",
         s->depth * 2, "", s->name, a, p, m, (double)s->window_calls / PROF_WINDOW);
    s->window_calls = 0;
  }
}

void prof_end_frame(uint64_t total_ns) {
  s_total_ms[s_frame] = (float)((double)total_ns / 1e6);
  s_draws[s_frame] = (float)g_prof_draws;
  g_prof_draws = 0;
  for (int i = 0; i < s_num_slots; i++) {
    ProfSlot *s = &s_slots[i];
    s->window_ms[s_frame] = (float)((double)s->frame_ns / 1e6);
    s->window_calls += s->calls;
    s->frame_ns = 0;
    s->calls = 0;
  }
  if (++s_frame == PROF_WINDOW) {
    s_frame = 0;
    report();
  }
}
