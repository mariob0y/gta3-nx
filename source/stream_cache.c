/* stream_cache.c -- background read-ahead prefetch cache for streaming assets
 */

#include "stream_cache.h"
#include "libc_shim.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <strings.h>

#define SC_MAX_SLOTS 8
#define SC_BLOCK_SIZE (1024 * 1024)
#define SC_BLOCKS_PER_SLOT 4
#define MAX_CUSHION_BYTES (SC_BLOCKS_PER_SLOT * SC_BLOCK_SIZE)

#define BLOCK_EMPTY 0
#define BLOCK_LOADING 1
#define BLOCK_READY 2

typedef struct {
  uint64_t file_offset;
  size_t valid_bytes;
  _Atomic uint32_t state;
  uint8_t *data;
} sc_block_t;

typedef struct {
  void *handle;
  FILE *bg_f;
  char resolved_path[512];
  bool active;
  uint64_t file_size;
  _Atomic uint64_t main_read_offset;
  _Atomic uint64_t prefetch_target_offset;
  _Atomic uint64_t last_read_tick;
  _Atomic uint64_t reads_total;
  _Atomic uint64_t hits_demand;
  _Atomic uint64_t hits_prefetch;
  _Atomic uint64_t misses;
  _Atomic uint64_t bytes_requested;
  _Atomic uint64_t bytes_from_cache;
  _Atomic uint64_t bytes_from_storage;
  _Atomic double blocking_read_ms_total;
  _Atomic double max_blocking_read_ms;
  sc_block_t blocks[SC_BLOCKS_PER_SLOT];
} sc_slot_t;

static sc_slot_t g_slots[SC_MAX_SLOTS];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_worker_thread;
static pthread_mutex_t g_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_worker_cond = PTHREAD_COND_INITIALIZER;
static _Atomic bool g_worker_running = false;

static bool sc_has_streaming_extension(const char *path) {
  if (!path) return false;
  size_t len = strlen(path);
  if (len < 4) return false;

  const char *ext = path + len - 4;
  /* .raw (SFX streaming, e.g. AUDIO/SFX.RAW) and .mp3 (radio station streaming)
   * are read sequentially over time and benefit from read-ahead caching.
   * .img/.dir/.txd are excluded: CdStream seeks to scattered offsets in those
   * archives, so a forward cushion adds locking + wasted background I/O. */
  if (strcasecmp(ext, ".raw") == 0 || strcasecmp(ext, ".mp3") == 0) {
    return true;
  }
  return false;
}

static bool sc_should_track(const char *path, const char *mode) {
  if (!path || !mode) return false;
  if (mode[0] == 'w' || strcmp(mode, "1") == 0) {
    return false; // Exclude write modes (e.g. saves)
  }
  return sc_has_streaming_extension(path);
}

static sc_slot_t *find_slot_by_handle_locked(void *handle) {
  if (!handle) return NULL;
  for (int i = 0; i < SC_MAX_SLOTS; i++) {
    if (g_slots[i].active && g_slots[i].handle == handle) {
      return &g_slots[i];
    }
  }
  return NULL;
}

static uint64_t compute_slot_cushion(sc_slot_t *s) {
  if (!s || !s->active) return 0;

  uint64_t main_off = atomic_load_explicit(&s->main_read_offset, memory_order_relaxed);
  uint64_t cushion = 0;

  for (int i = 0; i < SC_BLOCKS_PER_SLOT; i++) {
    sc_block_t *b = &s->blocks[i];
    uint32_t st = atomic_load_explicit(&b->state, memory_order_acquire);
    if (st == BLOCK_READY) {
      if (b->file_offset <= main_off && main_off < b->file_offset + b->valid_bytes) {
        cushion += (b->file_offset + b->valid_bytes - main_off);
      } else if (b->file_offset > main_off && b->file_offset < main_off + MAX_CUSHION_BYTES) {
        cushion += b->valid_bytes;
      }
    }
  }

  if (cushion > MAX_CUSHION_BYTES) cushion = MAX_CUSHION_BYTES;
  return cushion;
}

static void *sc_worker_loop(void *arg) {
  (void)arg;
  set_thread_core(2); // Pin prefetch worker to CPU Core 2
  LOGC(LOGC_SYS, "[STREAM_CACHE] Worker thread started and pinned to CPU Core 2\n");

  while (atomic_load_explicit(&g_worker_running, memory_order_relaxed)) {
    int best_slot_idx = -1;
    int highest_priority = -1;
    int target_block_idx = -1;
    uint64_t target_file_offset = 0;

    uint64_t now = io_tick_now();

    // Priority Selection: Inverted cushion priority (highest priority = closest to cache miss)
    for (int i = 0; i < SC_MAX_SLOTS; i++) {
      sc_slot_t *s = &g_slots[i];
      if (!s->active || !s->bg_f) continue;

      uint64_t last_tick = atomic_load_explicit(&s->last_read_tick, memory_order_relaxed);
      double sec_since_read = io_tick_to_ms(now - last_tick) / 1000.0;
      int recent_flag = (sec_since_read < 2.0) ? 1 : 0;

      if (!recent_flag) continue;

      uint64_t cushion = compute_slot_cushion(s);
      int priority = recent_flag * (MAX_CUSHION_BYTES - (int)cushion);

      uint64_t target_off = atomic_load_explicit(&s->prefetch_target_offset, memory_order_relaxed);
      uint64_t block_align_off = (target_off / SC_BLOCK_SIZE) * SC_BLOCK_SIZE;

      for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
        uint64_t cand_off = block_align_off + ((uint64_t)b * SC_BLOCK_SIZE);
        if (s->file_size > 0 && cand_off >= s->file_size) break;

        // Check if candidate offset is missing in blocks
        bool exists = false;
        int empty_idx = -1;

        for (int k = 0; k < SC_BLOCKS_PER_SLOT; k++) {
          sc_block_t *blk = &s->blocks[k];
          uint32_t st = atomic_load_explicit(&blk->state, memory_order_relaxed);
          if (st != BLOCK_EMPTY && blk->file_offset == cand_off) {
            exists = true;
            break;
          }
          if (st == BLOCK_EMPTY && empty_idx == -1) {
            empty_idx = k;
          }
        }

        if (!exists) {
          int pick_idx = empty_idx;
          if (pick_idx == -1) {
            // Evict block furthest behind main_read_offset
            uint64_t main_off = atomic_load_explicit(&s->main_read_offset, memory_order_relaxed);
            uint64_t max_dist = 0;
            for (int k = 0; k < SC_BLOCKS_PER_SLOT; k++) {
              sc_block_t *blk = &s->blocks[k];
              uint32_t st = atomic_load_explicit(&blk->state, memory_order_relaxed);
              if (st == BLOCK_READY && blk->file_offset + blk->valid_bytes <= main_off) {
                uint64_t dist = main_off - blk->file_offset;
                if (dist > max_dist) {
                  max_dist = dist;
                  pick_idx = k;
                }
              }
            }
          }

          if (pick_idx != -1 && priority > highest_priority) {
            highest_priority = priority;
            best_slot_idx = i;
            target_block_idx = pick_idx;
            target_file_offset = cand_off;
          }
          break;
        }
      }
    }

    if (best_slot_idx != -1 && target_block_idx != -1) {
      sc_slot_t *s = &g_slots[best_slot_idx];
      sc_block_t *b = &s->blocks[target_block_idx];

      atomic_store_explicit(&b->state, BLOCK_LOADING, memory_order_release);
      b->file_offset = target_file_offset;
      b->valid_bytes = 0;

      // Perform Unlocked File I/O
      if (fseek(s->bg_f, (long)target_file_offset, SEEK_SET) == 0) {
        size_t n = fread(b->data, 1, SC_BLOCK_SIZE, s->bg_f);
        b->valid_bytes = n;
      }

      // Memory Order Release: ensures buffer bytes are completely written before state becomes BLOCK_READY
      atomic_store_explicit(&b->state, BLOCK_READY, memory_order_release);
    } else {
      // Idle sleep on condition variable
      pthread_mutex_lock(&g_worker_mutex);
      if (atomic_load_explicit(&g_worker_running, memory_order_relaxed)) {
        pthread_cond_wait(&g_worker_cond, &g_worker_mutex);
      }
      pthread_mutex_unlock(&g_worker_mutex);
    }
  }

  LOGC(LOGC_SYS, "[STREAM_CACHE] Worker thread exiting\n");
  return NULL;
}

/* Preload archives into RAM at startup to eliminate SD card I/O latency variance.
 * Note: this removes storage-latency noise, but does NOT resolve CPU-bound model
 * instantiation stalls, which are handled by the streaming budget in CStreaming::Update.
 *
 * Tried swapping these to the uncompressed (_UNC) variants to test whether
 * the CPU cost measured inside the shared RenderWare texture loader was
 * specific to the DXT code path -- it wasn't (measured: identical per-model
 * TXD costs, just 2x the RAM/boot-preload time). Reverted to _DXT.
 *
 * Both the "_DXT"-suffixed and plain (no-suffix) entries for the same
 * archive are kept intentionally, not duplicated by mistake: the game
 * itself only ever requests the plain name ("MODELS/GTA3.IMG" -- it has no
 * idea "_dxt" exists, see libc_shim.c's redirect), while this list's own
 * bootstrap warm-up reads the physical on-disk "_DXT" name directly. Both
 * are needed as distinct lookup keys; sc_path_matches_archive() normalizes
 * away the "_dxt" suffix before comparing so they correctly dedupe to one
 * cached read instead of the file being loaded into RAM twice. */
static const char *SC_PRELOAD_ARCHIVES[] = {
  "MODELS/GTA3_DXT.IMG",
  "MODELS/GTA3_DXT.DIR",
  "MODELS/FIXEDVEH_DXT.IMG",
  "MODELS/FIXEDVEH_DXT.DIR",
  "ANIM/CUTS.IMG",
  "ANIM/CUTS.DIR",
  "MODELS/GENERIC_DXT.TXD",
  "MODELS/PARTICLE_DXT.TXD",
  "ANIM/PED.IFP",
  "MODELS/GTA3.IMG",
  "MODELS/GTA3.DIR",
  NULL
};

#define SC_MAX_PRELOAD_FILES 16
static sc_preload_t g_preload_files[SC_MAX_PRELOAD_FILES];
static int g_preload_count = 0;
static pthread_mutex_t g_preload_lock = PTHREAD_MUTEX_INITIALIZER;

/* Strips a trailing "_dxt" immediately before the file extension, in place
 * (e.g. "GTA3_DXT.IMG" -> "GTA3.IMG"). Lets sc_path_matches_archive treat
 * the game's own unredirected request ("MODELS/GTA3.IMG", the only string
 * the game ever actually asks for -- it has no idea "_dxt" exists) and the
 * physical on-disk name used during sc_preload_warm_all()'s own bootstrap
 * ("MODELS/GTA3_DXT.IMG") as the same archive. Without this, both names
 * were tracked as separate cache entries and the same file got read into
 * RAM twice at startup (confirmed: GTA3_DXT.IMG and GTA3.IMG both preloaded
 * separately, each the full archive size). */
static void strip_dxt_suffix(char *buf) {
  size_t len = strlen(buf);
  char *dot = strrchr(buf, '.');
  if (!dot) return;
  size_t base_len = (size_t)(dot - buf);
  if (base_len <= 4) return;
  if (strncasecmp(dot - 4, "_dxt", 4) != 0) return;
  size_t ext_len = len - base_len;
  memmove(dot - 4, dot, ext_len + 1); // shift extension (+NUL) left over "_dxt"
}

static bool sc_path_matches_archive(const char *path, const char *archive) {
  if (!path || !archive) return false;

  char norm_path[512];
  size_t len = strlen(path);
  if (len >= sizeof(norm_path)) len = sizeof(norm_path) - 1;

  for (size_t i = 0; i < len; i++) {
    char c = path[i];
    if (c == '\\') c = '/';
    norm_path[i] = c;
  }
  norm_path[len] = '\0';
  strip_dxt_suffix(norm_path);
  len = strlen(norm_path);

  char norm_archive[512];
  snprintf(norm_archive, sizeof(norm_archive), "%s", archive);
  strip_dxt_suffix(norm_archive);

  if (strcasecmp(norm_path, norm_archive) == 0) return true;

  size_t arch_len = strlen(norm_archive);
  if (len > arch_len) {
    const char *sub = norm_path + (len - arch_len);
    if ((sub[-1] == '/') && strcasecmp(sub, norm_archive) == 0) {
      return true;
    }
  }

  return false;
}

bool sc_is_preload_archive(const char *path) {
  if (!path) return false;
  for (int i = 0; SC_PRELOAD_ARCHIVES[i] != NULL; i++) {
    if (sc_path_matches_archive(path, SC_PRELOAD_ARCHIVES[i])) {
      return true;
    }
  }
  return false;
}

sc_preload_t *sc_get_or_load_preload(const char *resolved_path, long *out_size) {
  if (!resolved_path) return NULL;

  pthread_mutex_lock(&g_preload_lock);
  for (int i = 0; i < g_preload_count; i++) {
    if (sc_path_matches_archive(resolved_path, g_preload_files[i].path) ||
        sc_path_matches_archive(g_preload_files[i].path, resolved_path)) {
      if (out_size) *out_size = g_preload_files[i].size;
      pthread_mutex_unlock(&g_preload_lock);
      return &g_preload_files[i];
    }
  }

  if (g_preload_count >= SC_MAX_PRELOAD_FILES) {
    pthread_mutex_unlock(&g_preload_lock);
    LOGC(LOGC_SYS, "[STREAM_CACHE] ERROR: Preload slot limit reached (%d)\n", SC_MAX_PRELOAD_FILES);
    return NULL;
  }
  pthread_mutex_unlock(&g_preload_lock);

  FILE *f = open_asset_with_fallback(resolved_path);
  if (!f) {
    LOGC(LOGC_SYS, "[STREAM_CACHE] WARNING: Could not open asset '%s' for preloading\n", resolved_path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    return NULL;
  }

  uint8_t *buf = malloc((size_t)size);
  if (!buf) {
    fclose(f);
    LOGC(LOGC_SYS, "[STREAM_CACHE] ERROR: Failed to allocate %ld bytes for preloading '%s'\n", size, resolved_path);
    return NULL;
  }

  uint64_t t0 = io_tick_now();
  size_t got = fread(buf, 1, (size_t)size, f);
  double ms = io_tick_to_ms(io_tick_now() - t0);
  fclose(f);

  if (got != (size_t)size) {
    free(buf);
    LOGC(LOGC_SYS, "[STREAM_CACHE] ERROR: Incomplete read for '%s' (%zu/%ld bytes)\n", resolved_path, got, size);
    return NULL;
  }

  pthread_mutex_lock(&g_preload_lock);
  for (int i = 0; i < g_preload_count; i++) {
    if (sc_path_matches_archive(resolved_path, g_preload_files[i].path) ||
        sc_path_matches_archive(g_preload_files[i].path, resolved_path)) {
      free(buf);
      if (out_size) *out_size = g_preload_files[i].size;
      pthread_mutex_unlock(&g_preload_lock);
      return &g_preload_files[i];
    }
  }

  if (g_preload_count >= SC_MAX_PRELOAD_FILES) {
    free(buf);
    pthread_mutex_unlock(&g_preload_lock);
    return NULL;
  }

  int slot = g_preload_count++;
  g_preload_files[slot].data = buf;
  g_preload_files[slot].size = size;
  strncpy(g_preload_files[slot].path, resolved_path, sizeof(g_preload_files[slot].path) - 1);

  LOGC(LOGC_SYS, "[STREAM_CACHE] Preloaded '%s' into RAM: %ld bytes (%.2f MB) in %.1f ms\n",
       resolved_path, size, (double)size / (1024.0 * 1024.0), ms);

  if (out_size) *out_size = size;
  pthread_mutex_unlock(&g_preload_lock);
  return &g_preload_files[slot];
}

void sc_preload_warm_all(void) {
  LOGC(LOGC_SYS, "[STREAM_CACHE] Warming RAM preload archive cache...\n");
  uint64_t t0 = io_tick_now();
  for (int i = 0; SC_PRELOAD_ARCHIVES[i] != NULL; i++) {
    long size = 0;
    sc_get_or_load_preload(SC_PRELOAD_ARCHIVES[i], &size);
  }
  double total_ms = io_tick_to_ms(io_tick_now() - t0);
  LOGC(LOGC_SYS, "[STREAM_CACHE] Preload warm completed in %.1f ms (%d archives loaded)\n", total_ms, g_preload_count);
}

size_t sc_preload_read(sc_preload_t *p, void *buf, size_t count, long *io_pos) {
  if (!p || !buf || !io_pos || count == 0) return 0;
  long pos = *io_pos;
  if (pos < 0) pos = 0;
  if (pos >= p->size) return 0;
  size_t avail = (size_t)(p->size - pos);
  size_t n = (count < avail) ? count : avail;
  memcpy(buf, p->data + pos, n);
  *io_pos = pos + (long)n;
  return n;
}

void sc_preload_seek(sc_preload_t *p, long offset, int whence, long *io_pos) {
  if (!p || !io_pos) return;
  long base = (whence == SEEK_SET) ? 0
            : (whence == SEEK_CUR) ? *io_pos
            : p->size;
  long newpos = base + offset;
  if (newpos < 0) newpos = 0;
  if (newpos > p->size) newpos = p->size;
  *io_pos = newpos;
}

void sc_init(void) {
  pthread_mutex_lock(&g_table_lock);
  memset(g_slots, 0, sizeof(g_slots));
  for (int i = 0; i < SC_MAX_SLOTS; i++) {
    for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
      g_slots[i].blocks[b].data = malloc(SC_BLOCK_SIZE);
      atomic_store_explicit(&g_slots[i].blocks[b].state, BLOCK_EMPTY, memory_order_relaxed);
    }
  }
  pthread_mutex_unlock(&g_table_lock);

  sc_preload_warm_all();

  atomic_store_explicit(&g_worker_running, true, memory_order_relaxed);
  if (pthread_create(&g_worker_thread, NULL, sc_worker_loop, NULL) == 0) {
    LOGC(LOGC_SYS, "[STREAM_CACHE] Initialized stream cache successfully\n");
  } else {
    LOGC(LOGC_SYS, "[STREAM_CACHE] ERROR: Failed to create worker thread\n");
  }
}

void sc_shutdown(void) {
  if (!atomic_load_explicit(&g_worker_running, memory_order_relaxed))
    return;

  atomic_store_explicit(&g_worker_running, false, memory_order_relaxed);

  pthread_mutex_lock(&g_worker_mutex);
  pthread_cond_signal(&g_worker_cond);
  pthread_mutex_unlock(&g_worker_mutex);

  pthread_join(g_worker_thread, NULL);

  pthread_mutex_lock(&g_table_lock);
  for (int i = 0; i < SC_MAX_SLOTS; i++) {
    if (g_slots[i].bg_f) {
      fclose(g_slots[i].bg_f);
      g_slots[i].bg_f = NULL;
    }
    for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
      if (g_slots[i].blocks[b].data) {
        free(g_slots[i].blocks[b].data);
        g_slots[i].blocks[b].data = NULL;
      }
    }
    g_slots[i].active = false;
  }
  pthread_mutex_unlock(&g_table_lock);

  pthread_mutex_lock(&g_preload_lock);
  for (int i = 0; i < g_preload_count; i++) {
    if (g_preload_files[i].data) {
      free(g_preload_files[i].data);
      g_preload_files[i].data = NULL;
    }
  }
  g_preload_count = 0;
  pthread_mutex_unlock(&g_preload_lock);

  LOGC(LOGC_SYS, "[STREAM_CACHE] Shutdown completed\n");
}

void sc_open(void *handle, const char *resolved_path, FILE *main_f, const char *mode) {
  if (!handle || !resolved_path || !main_f) return;
  if (!sc_should_track(resolved_path, mode)) return;

  uint64_t t0 = io_tick_now();

  pthread_mutex_lock(&g_table_lock);
  double lock_ms = io_tick_to_ms(io_tick_now() - t0);
  if (lock_ms >= 1.0) {
    LOGC(LOGC_SYS, "[STREAM_CACHE] WARNING: sc_open g_table_lock wait took %.2f ms\n", lock_ms);
  }

  sc_slot_t *slot = NULL;
  for (int i = 0; i < SC_MAX_SLOTS; i++) {
    if (!g_slots[i].active) {
      slot = &g_slots[i];
      break;
    }
  }

  if (!slot) {
    pthread_mutex_unlock(&g_table_lock);
    LOGC(LOGC_SYS, "[STREAM_CACHE] WARNING: Out of slots for '%s'\n", resolved_path);
    return;
  }

  slot->handle = handle;
  strncpy(slot->resolved_path, resolved_path, sizeof(slot->resolved_path) - 1);
  slot->active = true;

  atomic_store_explicit(&slot->main_read_offset, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->prefetch_target_offset, 0, memory_order_relaxed);
  // Initial recency grace period
  atomic_store_explicit(&slot->last_read_tick, io_tick_now(), memory_order_relaxed);

  atomic_store_explicit(&slot->reads_total, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->hits_demand, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->hits_prefetch, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->misses, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->bytes_requested, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->bytes_from_cache, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->bytes_from_storage, 0, memory_order_relaxed);
  atomic_store_explicit(&slot->blocking_read_ms_total, 0.0, memory_order_relaxed);
  atomic_store_explicit(&slot->max_blocking_read_ms, 0.0, memory_order_relaxed);

  for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
    atomic_store_explicit(&slot->blocks[b].state, BLOCK_EMPTY, memory_order_relaxed);
    slot->blocks[b].valid_bytes = 0;
    slot->blocks[b].file_offset = 0;
  }

  pthread_mutex_unlock(&g_table_lock);

  // Unlocked handle creation for background worker thread
  FILE *bg_f = open_asset_with_fallback(resolved_path);
  if (bg_f) {
    setvbuf(bg_f, NULL, _IOFBF, 64 * 1024);
    fseek(bg_f, 0, SEEK_END);
    slot->file_size = ftell(bg_f);
    fseek(bg_f, 0, SEEK_SET);
    slot->bg_f = bg_f;

    LOGC(LOGC_SYS, "[STREAM_CACHE] Registered slot handle=%p path='%s' (size=%llu)\n",
         handle, resolved_path, (unsigned long long)slot->file_size);

    pthread_mutex_lock(&g_worker_mutex);
    pthread_cond_signal(&g_worker_cond);
    pthread_mutex_unlock(&g_worker_mutex);
  } else {
    pthread_mutex_lock(&g_table_lock);
    slot->active = false;
    slot->handle = NULL;
    pthread_mutex_unlock(&g_table_lock);
  }
}

void sc_close(void *handle) {
  if (!handle) return;

  FILE *close_f = NULL;
  pthread_mutex_lock(&g_table_lock);
  sc_slot_t *s = find_slot_by_handle_locked(handle);
  if (s) {
    uint64_t reads = atomic_load_explicit(&s->reads_total, memory_order_relaxed);
    uint64_t hits_d = atomic_load_explicit(&s->hits_demand, memory_order_relaxed);
    uint64_t hits_p = atomic_load_explicit(&s->hits_prefetch, memory_order_relaxed);
    uint64_t misses = atomic_load_explicit(&s->misses, memory_order_relaxed);
    uint64_t bytes_req = atomic_load_explicit(&s->bytes_requested, memory_order_relaxed);
    uint64_t bytes_store = atomic_load_explicit(&s->bytes_from_storage, memory_order_relaxed);
    double block_ms = atomic_load_explicit(&s->blocking_read_ms_total, memory_order_relaxed);
    double max_ms = atomic_load_explicit(&s->max_blocking_read_ms, memory_order_relaxed);

    LOGC(LOGC_SYS, "[STREAM_STATS] Closed handle=%p path='%s': reads=%llu hits_d=%llu hits_p=%llu misses=%llu req_bytes=%llu store_bytes=%llu total_blocking_ms=%.2f max_blocking_ms=%.2f\n",
         handle, s->resolved_path, (unsigned long long)reads, (unsigned long long)hits_d,
         (unsigned long long)hits_p, (unsigned long long)misses, (unsigned long long)bytes_req,
         (unsigned long long)bytes_store, block_ms, max_ms);

    s->active = false;
    s->handle = NULL;
    close_f = s->bg_f;
    s->bg_f = NULL;
    for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
      atomic_store_explicit(&s->blocks[b].state, BLOCK_EMPTY, memory_order_relaxed);
    }
    LOGC(LOGC_SYS, "[STREAM_CACHE] Closed slot for handle=%p\n", handle);
  }
  pthread_mutex_unlock(&g_table_lock);

  if (close_f) {
    fclose(close_f);
  }
}

size_t sc_read(void *handle, FILE *main_f, void *buf, size_t count) {
  if (!handle || !main_f || !buf || count == 0) return 0;

  uint64_t t0 = io_tick_now();
  pthread_mutex_lock(&g_table_lock);
  double lock_ms = io_tick_to_ms(io_tick_now() - t0);
  if (lock_ms >= 1.0) {
    LOGC(LOGC_SYS, "[STREAM_CACHE] WARNING: sc_read g_table_lock wait took %.2f ms\n", lock_ms);
  }

  sc_slot_t *s = find_slot_by_handle_locked(handle);
  pthread_mutex_unlock(&g_table_lock);

  if (!s || !s->active) {
    uint64_t fread_t0 = io_tick_now();
    size_t res = fread(buf, 1, count, main_f);
    double read_ms = io_tick_to_ms(io_tick_now() - fread_t0);
    for (;;) {
      double old_val = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed);
      double new_val = old_val + read_ms;
      if (atomic_compare_exchange_weak_explicit(&g_phase_io_wait_ms, &old_val, new_val, memory_order_relaxed, memory_order_relaxed))
        break;
    }
    return res;
  }

  atomic_fetch_add_explicit(&s->reads_total, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s->bytes_requested, count, memory_order_relaxed);

  uint64_t cur_pos = ftell(main_f);
  atomic_store_explicit(&s->last_read_tick, io_tick_now(), memory_order_relaxed);
  atomic_store_explicit(&s->main_read_offset, cur_pos, memory_order_relaxed);

  // Check if requested range hits a BLOCK_READY block
  for (int b = 0; b < SC_BLOCKS_PER_SLOT; b++) {
    sc_block_t *blk = &s->blocks[b];
    // Memory Order Acquire: guarantees buffer contents are visible before reading
    uint32_t st = atomic_load_explicit(&blk->state, memory_order_acquire);
    if (st == BLOCK_READY) {
      if (blk->file_offset <= cur_pos && cur_pos + count <= blk->file_offset + blk->valid_bytes) {
        size_t offset_in_block = (size_t)(cur_pos - blk->file_offset);
        memcpy(buf, blk->data + offset_in_block, count);

        // Advance main thread position
        fseek(main_f, (long)(cur_pos + count), SEEK_SET);

        atomic_store_explicit(&s->main_read_offset, cur_pos + count, memory_order_relaxed);
        atomic_store_explicit(&s->prefetch_target_offset, cur_pos + count, memory_order_relaxed);

        atomic_fetch_add_explicit(&s->hits_demand, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->bytes_from_cache, count, memory_order_relaxed);

        pthread_mutex_lock(&g_worker_mutex);
        pthread_cond_signal(&g_worker_cond);
        pthread_mutex_unlock(&g_worker_mutex);

        return count;
      }
    }
  }

  // Cache miss: fall back to blocking fread on main_f
  uint64_t fread_t0 = io_tick_now();
  size_t res = fread(buf, 1, count, main_f);
  double read_ms = io_tick_to_ms(io_tick_now() - fread_t0);

  atomic_fetch_add_explicit(&s->misses, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&s->bytes_from_storage, res, memory_order_relaxed);

  // Accumulate storage wait time into global phase I/O tracker
  for (;;) {
    double old_val = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed);
    double new_val = old_val + read_ms;
    if (atomic_compare_exchange_weak_explicit(&g_phase_io_wait_ms, &old_val, new_val, memory_order_relaxed, memory_order_relaxed))
      break;
  }

  for (;;) {
    double old_val = atomic_load_explicit(&s->blocking_read_ms_total, memory_order_relaxed);
    double new_val = old_val + read_ms;
    if (atomic_compare_exchange_weak_explicit(&s->blocking_read_ms_total, &old_val, new_val, memory_order_relaxed, memory_order_relaxed))
      break;
  }

  for (;;) {
    double cur_max = atomic_load_explicit(&s->max_blocking_read_ms, memory_order_relaxed);
    if (read_ms <= cur_max) break;
    if (atomic_compare_exchange_weak_explicit(&s->max_blocking_read_ms, &cur_max, read_ms, memory_order_relaxed, memory_order_relaxed))
      break;
  }

  if (read_ms >= 2.0) {
    LOGC(LOGC_SYS, "[IO_TRACE] MISS: path='%s' handle=%p offset=0x%llx size=%zu blocking=%.2fms phase='%s'\n",
         s->resolved_path, handle, (unsigned long long)cur_pos, count, read_ms, g_active_phase ? g_active_phase : "None");
  }

  atomic_store_explicit(&s->main_read_offset, cur_pos + res, memory_order_relaxed);
  atomic_store_explicit(&s->prefetch_target_offset, cur_pos + res, memory_order_relaxed);

  pthread_mutex_lock(&g_worker_mutex);
  pthread_cond_signal(&g_worker_cond);
  pthread_mutex_unlock(&g_worker_mutex);

  return res;
}

int sc_debug_active_slots(void) {
  int count = 0;
  pthread_mutex_lock(&g_table_lock);
  for (int i = 0; i < SC_MAX_SLOTS; i++) {
    if (g_slots[i].active) count++;
  }
  pthread_mutex_unlock(&g_table_lock);
  return count;
}
