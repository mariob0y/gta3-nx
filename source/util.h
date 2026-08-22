/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logging categories and mask
typedef enum {
  LOGC_SYS    = 1 << 0,  // boot, hooks, config (0x01)
  LOGC_FILE   = 1 << 1,  // OS_FileOpen/Close, asset loads (0x02)
  LOGC_THREAD = 1 << 2,  // thread spawn/join, TLS (0x04)
  LOGC_GL     = 1 << 3,  // per-draw-call GL trace (0x08)
  LOGC_EGL    = 1 << 4,  // per-frame EGL trace (0x10)
} LogCategory;

extern unsigned g_log_mask;
extern uint32_t g_frame_count;

// Log a formatted message to gta3_log.txt and nxlink stdout
int debugPrintf(char *text, ...) __attribute__((format(printf, 1, 2)));
void debugPrintfFlush(void);
void debugPrintfFlushCrash(void);

#define LOGC(cat, ...) do { \
  if ((g_log_mask & (cat)) != 0) debugPrintf(__VA_ARGS__); \
} while (0)


#include <switch.h>

extern volatile uint64_t g_frame_start_tick;
extern Handle g_main_thread_handle;
extern const char * volatile g_active_phase;
extern _Atomic double g_phase_io_wait_ms;

// Updated any time a *world streaming archive* (.img/.dir/.txd) read
// completes, regardless of whether it went through the stream_cache
// slot table. sc_debug_active_slots() only tracks .raw/.mp3 read-ahead
// slots (see stream_cache.c), so it stays at 0 during model/texture
// streaming -- this counter is what actually reflects "the world is
// streaming in right now" and is used to decide when to engage the
// CPU clock boost (see main.c).
extern _Atomic uint64_t g_last_streaming_activity_tick;

typedef struct {
  uint64_t request_created_tick;
  uint64_t request_submitted_tick;
  uint64_t storage_start_tick;
  uint64_t storage_end_tick;
  uint64_t consumer_tick;
} sc_time_trace_t;

extern _Atomic bool g_active_io_blocking;
extern _Atomic uint64_t g_active_io_start_tick;
extern _Atomic uint64_t g_active_io_end_tick;
typedef enum {
  STREAM_GAMEPLAY = 0,
  STREAM_LOADING_SCREEN = 1,
  STREAM_BOOTSTRAP = 2
} StreamingContext;

typedef enum {
  STREAM_RUNTIME_BOOT = 0,
  STREAM_RUNTIME_TRANSITION = 1,
  STREAM_RUNTIME_GAMEPLAY = 2
} StreamingRuntimeMode;

extern _Atomic int g_streaming_context;
extern _Atomic bool g_gameplay_streaming_enabled;
extern _Atomic int g_models_loaded_this_frame;
extern _Atomic uint64_t g_streaming_frame_cpu_ns;
extern _Atomic int g_stream_runtime_mode;
extern _Atomic unsigned g_stream_transition_frames;
extern _Atomic bool g_gameplay_transition_pending;

const char *streaming_context_to_string(StreamingContext ctx);
StreamingContext get_streaming_context(void);
void streaming_set_context(StreamingContext new_ctx);
bool gameplay_streaming_enabled(void);
void set_gameplay_streaming_enabled(bool enabled);

const char *stream_runtime_mode_to_string(StreamingRuntimeMode mode);
StreamingRuntimeMode get_stream_runtime_mode(void);
void set_stream_runtime_mode(StreamingRuntimeMode mode, unsigned grace_frames);
void update_streaming_transition_frame(void);

void set_active_io_path(const char *path);
void begin_active_io(const char *path, size_t bytes);
void end_active_io(size_t bytes_read);
void *stall_watchdog_thread(void *arg);
void emit_and_reset_frame_streaming_summary(double total_frame_ms);
void print_crash_snapshot(int sig);

// CPU clock boost mode toggle
void cpu_boost(int on);

static inline uint64_t io_tick_now(void) { return armGetSystemTick(); }
static inline double io_tick_to_ms(uint64_t delta_ticks) {
  return (double)armTicksToNs(delta_ticks) / 1e6;
}
#define IO_SLOW_MS 5.0

static inline void mark_streaming_activity(void) {
  atomic_store_explicit(&g_last_streaming_activity_tick, armGetSystemTick(), memory_order_relaxed);
}

// True if a world-archive (.img/.dir/.txd) or audio (.raw/.mp3) streaming
// read completed within the last `window_ms` milliseconds.
static inline bool streaming_activity_recent(double window_ms) {
  uint64_t last = atomic_load_explicit(&g_last_streaming_activity_tick, memory_order_relaxed);
  if (last == 0) return false;
  return io_tick_to_ms(io_tick_now() - last) <= window_ms;
}

// Pin the calling thread to a specific CPU core
void set_thread_core(int core);

// Install stack guard canary in ARM64 TLS (TPIDR_EL0)
void *game_tls_install(void);

// Thread registry for clean pause during exit
void thread_registry_add(void);
void thread_registry_pause_others(void);

// Dummy fallback functions
int ret0(void);
int retm1(void);

static inline void armSetTlsRw(void *addr) {
  __asm__ ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

#ifdef __cplusplus
}
#endif

#endif
