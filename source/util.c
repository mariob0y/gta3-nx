#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "util.h"
#include "config.h"
#include "so_util.h"

// Global category bitmask (default: SYS | FILE | THREAD, GL and EGL off)
unsigned g_log_mask = LOGC_SYS | LOGC_FILE | LOGC_THREAD;

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log_file = NULL;

#define LOG_RING_SIZE (1 * 1024 * 1024)
static char s_ring[LOG_RING_SIZE];
static char s_flush_scratch[LOG_RING_SIZE];
static volatile size_t s_ring_head = 0;
static volatile size_t s_ring_tail = 0;
static volatile size_t s_dropped_bytes = 0;
static pthread_mutex_t s_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_log_thread;
static volatile int s_log_thread_exit = 0;
static int s_log_thread_running = 0;

static void *log_flush_thread_entry(void *arg) {
  (void)arg;
  while (!s_log_thread_exit) {
    usleep(250000); // 250ms

    size_t pending = 0;
    size_t dropped = 0;

    pthread_mutex_lock(&s_ring_mutex);
    size_t head = s_ring_head;
    size_t tail = s_ring_tail;
    dropped = s_dropped_bytes;
    s_dropped_bytes = 0;

    if (head != tail) {
      pending = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail + head);
      size_t first_chunk = (head >= tail) ? pending : (LOG_RING_SIZE - tail);
      memcpy(s_flush_scratch, s_ring + tail, first_chunk);
      if (pending > first_chunk) {
        memcpy(s_flush_scratch + first_chunk, s_ring, pending - first_chunk);
      }
      s_ring_tail = head;
    }
    pthread_mutex_unlock(&s_ring_mutex);

    if (s_log_file) {
      if (dropped > 0) {
        fprintf(s_log_file, "[LOG] WARNING: Dropped %zu bytes of log output due to ring buffer overflow\n", dropped);
      }
      if (pending > 0) {
        fwrite(s_flush_scratch, 1, pending, s_log_file);
        fflush(s_log_file);
      }
    }
  }
  return NULL;
}

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
  s_log_thread_exit = 0;
  if (pthread_create(&s_log_thread, NULL, log_flush_thread_entry, NULL) == 0) {
    s_log_thread_running = 1;
  }
}

void userAppExit(void) {
  debugPrintfFlush();
  if (s_log_thread_running) {
    s_log_thread_exit = 1;
    pthread_join(s_log_thread, NULL);
    s_log_thread_running = 0;
  }
  deinitNxLink();
}

#else

void userAppInit(void) {}
void userAppExit(void) {}

#endif

void debugPrintfFlush(void) {
#ifdef DEBUG_LOG
  size_t pending = 0;
  size_t dropped = 0;

  pthread_mutex_lock(&s_ring_mutex);
  size_t head = s_ring_head;
  size_t tail = s_ring_tail;
  dropped = s_dropped_bytes;
  s_dropped_bytes = 0;

  if (head != tail) {
    pending = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail + head);
    size_t first_chunk = (head >= tail) ? pending : (LOG_RING_SIZE - tail);
    memcpy(s_flush_scratch, s_ring + tail, first_chunk);
    if (pending > first_chunk) {
      memcpy(s_flush_scratch + first_chunk, s_ring, pending - first_chunk);
    }
    s_ring_tail = head;
  }
  pthread_mutex_unlock(&s_ring_mutex);

  if (s_log_file) {
    if (dropped > 0) {
      fprintf(s_log_file, "[LOG] WARNING: Dropped %zu bytes of log output due to ring buffer overflow\n", dropped);
    }
    if (pending > 0) {
      fwrite(s_flush_scratch, 1, pending, s_log_file);
    }
    fflush(s_log_file);
  }
#endif
}

void debugPrintfFlushCrash(void) {
#ifdef DEBUG_LOG
  if (!s_log_file) return;
  int fd = fileno(s_log_file);
  if (fd < 0) return;

  size_t pending = 0;
  int locked = (pthread_mutex_trylock(&s_ring_mutex) == 0);

  size_t head = s_ring_head;
  size_t tail = s_ring_tail;

  if (head != tail) {
    pending = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail + head);
    size_t first_chunk = (head >= tail) ? pending : (LOG_RING_SIZE - tail);
    memcpy(s_flush_scratch, s_ring + tail, first_chunk);
    if (pending > first_chunk) {
      memcpy(s_flush_scratch + first_chunk, s_ring, pending - first_chunk);
    }
    if (locked) {
      s_ring_tail = head;
    }
  }

  if (locked) {
    pthread_mutex_unlock(&s_ring_mutex);
  }

  if (pending > 0) {
    write(fd, s_flush_scratch, pending);
  }
#endif
}

// Log formatted message to RAM ring buffer (addressable out-of-line C function)
int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  static int first_write = 1;

  if (!s_log_file) {
    remove(LOG_NAME); // Explicitly delete old log file on every fresh boot
    s_log_file = fopen(LOG_NAME, "w");
    if (s_log_file) {
      setvbuf(s_log_file, NULL, _IOFBF, 64 * 1024); // 64KB write buffer
      if (first_write) {
        first_write = 0;
        time_t now = time(NULL);
        fprintf(s_log_file, "======================================================\n");
        fprintf(s_log_file, " GTA III Nintendo Switch Port (gta3_nx) Fresh Boot Log \n");
        fprintf(s_log_file, " Log Created: %s", ctime(&now));
        fprintf(s_log_file, "======================================================\n\n");
      }
    }
  }

  char line[512];
  va_list list;
  va_start(list, text);
  int n = vsnprintf(line, sizeof(line), text, list);
  va_end(list);

  if (n > 0) {
    pthread_mutex_lock(&s_ring_mutex);
    for (int i = 0; i < n; i++) {
      s_ring[s_ring_head] = line[i];
      size_t next_head = (s_ring_head + 1) % LOG_RING_SIZE;
      if (next_head == s_ring_tail) { // ring buffer full
        s_ring_tail = (s_ring_tail + 1) % LOG_RING_SIZE;
        s_dropped_bytes++;
      }
      s_ring_head = next_head;
    }
    pthread_mutex_unlock(&s_ring_mutex);
  }

  if (s_nxlinkSock >= 0) {
    va_start(list, text);
    vprintf(text, list); // Output to nxlink console if attached
    va_end(list);
  }
#else
  (void)text;
#endif
  return 0;
}

// Boost CPU to 1785MHz during fast load operations
void cpu_boost(int on) {
  LOGC(LOGC_SYS, "[SYS] CPU boost %s\n", on ? "ENABLED (1785MHz)" : "DISABLED (Normal)");
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

// Pin calling thread to a designated CPU core
void set_thread_core(int core) {
  static u64 mask = 0;
  if (mask == 0)
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  if (core < 0 || !(mask & (1ull << core)))
    return;
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ull << core);
  if (R_FAILED(rc)) {
    LOGC(LOGC_THREAD, "[AFFINITY] Pin to core %d failed: 0x%08x\n", core, rc);
  } else {
    LOGC(LOGC_THREAD, "[AFFINITY] Thread successfully pinned to core %d\n", core);
  }
}

// Dedicated TLS slot management for ARM64 stack canary checking
#define MAX_GAME_TLS_THREADS 64
#define GAME_TLS_SIZE 0x1000
#define GAME_TLS_GUARD UINT64_C(0x4242424242424242)

static uint8_t g_game_tls[MAX_GAME_TLS_THREADS][GAME_TLS_SIZE]
    __attribute__((aligned(GAME_TLS_SIZE)));
static unsigned g_game_tls_count;

void *game_tls_install(void) {
  unsigned slot = __atomic_fetch_add(&g_game_tls_count, 1, __ATOMIC_RELAXED);
  if (slot >= MAX_GAME_TLS_THREADS) {
    LOGC(LOGC_THREAD, "[TLS] ERROR: Exhausted %u dedicated game TLS slots!\n", MAX_GAME_TLS_THREADS);
    return NULL;
  }

  uint8_t *tls = g_game_tls[slot];
  memset(tls, 0, GAME_TLS_SIZE);
  const uint64_t guard = GAME_TLS_GUARD;
  for (size_t off = 0x20; off < 0x200; off += 8) {
    memcpy(tls + off, &guard, sizeof(guard));
  }
  armSetTlsRw(tls);
  LOGC(LOGC_THREAD, "[TLS] Installed thread TLS slot #%u at %p (all canary slots filled)\n", slot, tls);
  return tls;
}

// Thread registry for clean teardown during application exit
#define MAX_TRACKED_THREADS 64
static Handle g_thread_handles[MAX_TRACKED_THREADS];
static int g_thread_count;

void thread_registry_add(void) {
  int i = __atomic_fetch_add(&g_thread_count, 1, __ATOMIC_RELAXED);
  if (i < MAX_TRACKED_THREADS) {
    g_thread_handles[i] = threadGetCurHandle();
    LOGC(LOGC_THREAD, "[THREAD] Registered thread #%d (Handle=0x%x)\n", i, g_thread_handles[i]);
  }
}

void thread_registry_pause_others(void) {
  Handle self = threadGetCurHandle();
  int n = g_thread_count;
  if (n > MAX_TRACKED_THREADS)
    n = MAX_TRACKED_THREADS;
  int paused = 0;
  for (int i = 0; i < n; i++) {
    Handle h = g_thread_handles[i];
    if (h && h != self && R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)))
      paused++;
  }
  LOGC(LOGC_THREAD, "[EXIT] Paused %d/%d active engine threads\n", paused, n);
}

int ret0(void) { return 0; }
int retm1(void) { return -1; }

#define STALL_POLL_US 20000

extern so_module game_mod;

const char * volatile g_active_phase = "Init";
_Atomic double g_phase_io_wait_ms = 0.0;
_Atomic uint64_t g_last_streaming_activity_tick = 0;

_Atomic bool g_active_io_blocking = false;
_Atomic uint64_t g_active_io_start_tick = 0;
_Atomic uint64_t g_active_io_end_tick = 0;
_Atomic size_t g_active_io_bytes = 0;
char g_active_io_path_buf[256] = "None";

void set_active_io_path(const char *path) {
  if (path) {
    snprintf(g_active_io_path_buf, sizeof(g_active_io_path_buf), "%s", path);
  }
}

void begin_active_io(const char *path, size_t bytes) {
  if (path) {
    snprintf(g_active_io_path_buf, sizeof(g_active_io_path_buf), "%s", path);
  }
  atomic_store_explicit(&g_active_io_bytes, bytes, memory_order_relaxed);
  atomic_store_explicit(&g_active_io_start_tick, armGetSystemTick(), memory_order_relaxed);
  atomic_store_explicit(&g_active_io_blocking, true, memory_order_release);
}

void end_active_io(size_t bytes_read) {
  (void)bytes_read;
  atomic_store_explicit(&g_active_io_end_tick, armGetSystemTick(), memory_order_relaxed);
  atomic_store_explicit(&g_active_io_blocking, false, memory_order_release);
}

void *stall_watchdog_thread(void *arg) {
  (void)arg;
  int last_reported_interval = 0;
  for (;;) {
    usleep(STALL_POLL_US);

    uint64_t start = g_frame_start_tick;
    if (start == 0) {
      last_reported_interval = 0;
      continue;
    }

    double elapsed_ms = (double)armTicksToNs(armGetSystemTick() - start) / 1e6;
    int interval = (int)(elapsed_ms / 100.0);
    if (interval <= 0 || interval == last_reported_interval) continue;
    last_reported_interval = interval;

    bool is_io_blocking = atomic_load_explicit(&g_active_io_blocking, memory_order_acquire);
    uint64_t io_start = atomic_load_explicit(&g_active_io_start_tick, memory_order_relaxed);
    uint64_t io_end = atomic_load_explicit(&g_active_io_end_tick, memory_order_relaxed);
    size_t io_bytes = atomic_load_explicit(&g_active_io_bytes, memory_order_relaxed);

    char io_desc[256];
    if (is_io_blocking && io_start > 0) {
      double cur_io_ms = (double)armTicksToNs(armGetSystemTick() - io_start) / 1e6;
      snprintf(io_desc, sizeof(io_desc), "current_io=YES ('%s' %zu bytes, active %.1f ms)",
               g_active_io_path_buf, io_bytes, cur_io_ms);
    } else if (io_end > 0) {
      double io_ms_ago = (double)armTicksToNs(armGetSystemTick() - io_end) / 1e6;
      snprintf(io_desc, sizeof(io_desc), "current_io=NO (last file: '%s' %.0f ms ago)",
               g_active_io_path_buf, io_ms_ago);
    } else {
      snprintf(io_desc, sizeof(io_desc), "current_io=NO (none)");
    }

    ThreadContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Result pause_rc = 0;
    Result rc = -1;

    if (g_main_thread_handle) {
      pause_rc = svcSetThreadActivity(g_main_thread_handle, ThreadActivity_Paused);
      if (R_SUCCEEDED(pause_rc)) {
        rc = svcGetThreadContext3(&ctx, g_main_thread_handle);
        // Unconditional resume regardless of svcGetThreadContext3 result
        svcSetThreadActivity(g_main_thread_handle, ThreadActivity_Runnable);
      }
    }

    if (R_SUCCEEDED(rc)) {
      uintptr_t pc_addr = (uintptr_t)ctx.pc.x;
      char pc_sym_desc[256];
      resolve_pc_to_module_and_symbol(pc_addr, pc_sym_desc, sizeof(pc_sym_desc));
      LOGC(LOGC_SYS, "[STALL] main thread stuck %.1f ms (phase: '%s', %s), PC=%s\n",
           elapsed_ms, g_active_phase ? g_active_phase : "?", io_desc, pc_sym_desc);
    } else {
      LOGC(LOGC_SYS, "[STALL] main thread stuck %.1f ms (phase: '%s', %s) [svcGetThreadContext3 rc=0x%x, pause_rc=0x%x]\n",
           elapsed_ms, g_active_phase ? g_active_phase : "?", io_desc, rc, pause_rc);
    }
  }
  return NULL;
}

void print_crash_snapshot(int sig) {
  const char *sig_name = (sig == SIGSEGV ? "SIGSEGV" :
                          sig == SIGBUS  ? "SIGBUS"  :
                          sig == SIGABRT ? "SIGABRT" :
                          sig == SIGILL  ? "SIGILL"  :
                          sig == SIGFPE  ? "SIGFPE"  : "UNKNOWN");

  debugPrintf("\n======================================================\n");
  debugPrintf("[CRASH SNAPSHOT]\n");
  debugPrintf("signal=%d (%s)\n", sig, sig_name);
  debugPrintf("phase=%s\n", g_active_phase ? g_active_phase : "none");
  debugPrintf("stream_context=%s\n", streaming_context_to_string(get_streaming_context()));
  debugPrintf("gameplay_enabled=%d\n", gameplay_streaming_enabled() ? 1 : 0);
  debugPrintf("frame=%u\n", g_frame_count);
  debugPrintf("stream_cpu_ms=%.2f\n", (double)atomic_load_explicit(&g_streaming_frame_cpu_ns, memory_order_relaxed) / 1e6);
  debugPrintf("models_loaded_this_frame=%d\n", atomic_load_explicit(&g_models_loaded_this_frame, memory_order_relaxed));
  debugPrintf("active_io=%s\n", atomic_load_explicit(&g_active_io_blocking, memory_order_relaxed) ? "YES" : "NO");
  debugPrintf("active_io_path=%s\n", g_active_io_path_buf[0] ? g_active_io_path_buf : "none");

  if (g_main_thread_handle) {
    ThreadContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (R_SUCCEEDED(svcGetThreadContext3(&ctx, g_main_thread_handle))) {
      char sym_buf[256];
      resolve_pc_to_module_and_symbol((uintptr_t)ctx.pc.x, sym_buf, sizeof(sym_buf));
      debugPrintf("pc=0x%016lx (%s)\n", (unsigned long)ctx.pc.x, sym_buf);
      debugPrintf("lr=0x%016lx\n", (unsigned long)ctx.lr);
      debugPrintf("sp=0x%016lx\n", (unsigned long)ctx.sp);
    }
  }
  debugPrintf("======================================================\n");
  debugPrintfFlushCrash();
}

