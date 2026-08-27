/* game.c -- hooks and patches for GTA III engine
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#include <stdatomic.h>
#include <math.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "../config.h"
#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../jni_fake.h"

extern so_module game_mod;

void *NVThreadGetCurrentJNIEnv(void) {
  return fake_env;
}

typedef struct {
  void *(*func)(void *);
  void *arg;
  int core;
} NVThreadStart;

static void *nv_thread_trampoline(void *arg) {
  NVThreadStart *start = arg;
  void *(*func)(void *) = start->func;
  void *user_arg = start->arg;
  int core = start->core;
  free(start);
  set_thread_core(core);
  thread_registry_add();
  if (!game_tls_install())
    return (void *)(intptr_t)-1;
  return func(user_arg);
}

int NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name,
                           void *(*fn)(void *), void *arg) {
  (void)attr;
  LOGC(LOGC_THREAD, "[HOOK] NVThreadSpawnJNIThread: Spawning thread '%s' with 512KB stack...\n", name ? name : "(unnamed)");
  NVThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return -1;
  start->func = fn;
  start->arg = arg;
  int is_gamemain = (name && strcmp(name, "GameMain") == 0);
  start->core = is_gamemain ? 0
              : (name && (strstr(name, "Render") != NULL || strstr(name, "RenderQueue") != NULL)) ? 1
              : 2;

  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  pthread_attr_setstacksize(&pattr, 512 * 1024);

  pthread_t pthrd;
  const int create_rc = pthread_create(&pthrd, &pattr, nv_thread_trampoline, start);
  pthread_attr_destroy(&pattr);

  if (create_rc != 0) {
    LOGC(LOGC_THREAD, "[HOOK] ERROR: Failed to create thread '%s'\n", name ? name : "(unnamed)");
    free(start);
    return -1;
  }
  if (tid)
    *tid = (long)pthrd;
  return 0;
}

typedef struct {
  unsigned int (*func)(void *);
  void *arg;
  char *handle;
  int is_gamemain;
  int core;
} OSThreadData;

static void *os_thread_entry(void *arg) {
  OSThreadData *td = arg;
  unsigned int (*func)(void *) = td->func;
  void *user_arg = td->arg;
  char *handle = td->handle;
  int core = td->core;
  free(td);

  set_thread_core(core);
  thread_registry_add();
  if (!game_tls_install())
    return (void *)(uintptr_t)-1;

  if (handle)
    handle[0x69] = 1;

  LOGC(LOGC_THREAD, "[OS_Thread] Thread started (handle=%p, func=%p, arg=%p)\n", handle, (void *)func, user_arg);

  unsigned int ret = func(user_arg);

  if (handle)
    handle[0x69] = 0;

  LOGC(LOGC_THREAD, "[OS_Thread] Thread finished (handle=%p, ret=%u)\n", handle, ret);
  return (void *)(uintptr_t)ret;
}

void *OS_ThreadLaunch_hook(unsigned int (*func)(void *), void *arg, unsigned int stack_size,
                           const char *name, int r4, int priority) {
  (void)r4; (void)priority;
  LOGC(LOGC_THREAD, "[OS_Thread] OS_ThreadLaunch: '%s' (func=%p, arg=%p, stack_size=%u)\n",
              name ? name : "unnamed", (void *)func, arg, stack_size);

  char *handle = calloc(1, 0x400);
  if (!handle)
    return NULL;

  OSThreadData *td = malloc(sizeof(OSThreadData));
  if (!td) {
    free(handle);
    return NULL;
  }
  td->func = func;
  td->arg = arg;
  td->handle = handle;
  td->is_gamemain = (name && strcmp(name, "GameMain") == 0);
  td->core = td->is_gamemain ? 0
           : (name && (strstr(name, "Render") != NULL || strstr(name, "RenderQueue") != NULL)) ? 1
           : 2;

  size_t stsize = (stack_size > 0) ? stack_size : (2 * 1024 * 1024);
  if (stsize < 512 * 1024) stsize = 512 * 1024;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stsize);

  pthread_t thrd;
  int rc = pthread_create(&thrd, &attr, os_thread_entry, td);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    LOGC(LOGC_THREAD, "[OS_Thread] ERROR: pthread_create failed rc=%d for '%s'\n", rc, name ? name : "unnamed");
    free(td);
    free(handle);
    return NULL;
  }

  handle[0x69] = 1;
  memcpy(handle + 0x28, &thrd, sizeof(thrd));
  return handle;
}

void OS_ThreadWait_hook(void *thread) {
  if (!thread)
    return;

  pthread_t thrd;
  memset(&thrd, 0, sizeof(thrd));
  memcpy(&thrd, (char *)thread + 0x28, sizeof(thrd));

  if (!memcmp(&thrd, &(pthread_t){0}, sizeof(thrd)))
    return;

  LOGC(LOGC_THREAD, "[OS_Thread] OS_ThreadWait(handle=%p)\n", thread);
  pthread_join(thrd, NULL);
}

int OS_ThreadIsRunning_hook(void *thread) {
  if (!thread)
    return 0;
  return ((uint8_t *)thread)[0x69] != 0;
}

void OS_ThreadClose_hook(void *thread) {
  if (!thread)
    return;
  LOGC(LOGC_THREAD, "[OS_Thread] OS_ThreadClose(handle=%p)\n", thread);
  free(thread);
}

static char cheat_queue[16][64];
static int cheat_head = 0, cheat_tail = 0;

void cheats_enqueue(const char *code) {
  if (!code || !code[0]) return;
  int next = (cheat_tail + 1) % 16;
  if (next == cheat_head) return;
  strncpy(cheat_queue[cheat_tail], code, 63);
  cheat_tail = next;
  LOGC(LOGC_SYS, "[CHEAT] Enqueued cheat code: '%s'\n", code);
}

static uint8_t *CMenuManager_m_PrefsFrameLimiter = NULL;

void keep_game_frame_limiter_off(void) {
  if (CMenuManager_m_PrefsFrameLimiter) {
    *CMenuManager_m_PrefsFrameLimiter = 0;
  }
}

#include "../libc_shim.h"
#include "../stream_cache.h"

static bool path_is_world_stream_archive(const char *path) {
  if (!path) return false;
  size_t len = strlen(path);
  if (len < 4) return false;
  const char *ext = path + len - 4;
  return strcasecmp(ext, ".img") == 0 ||
         strcasecmp(ext, ".dir") == 0 ||
         strcasecmp(ext, ".txd") == 0;
}

typedef struct {
  FILE *f;
  sc_preload_t *preload;
  long pos;
  long size;
  bool is_world_stream;
  char path[128];
  bool pending_rename;
  char tmp_path[300];
} NvFile;

typedef NvFile OSFile;

void *NvFOpen_hook(const char *path) {
  void *caller = __builtin_return_address(0);
  if (!path) return NULL;
  set_active_io_path(path);
  LOGC(LOGC_FILE, "[NvF] NvFOpen('%s', caller=%p)...\n", path, caller);

  if (sc_is_preload_archive(path)) {
    long size = 0;
    sc_preload_t *pre = sc_get_or_load_preload(path, &size);
    if (pre) {
      NvFile *file = calloc(1, sizeof(NvFile));
      if (file) {
        file->f = NULL;
        file->preload = pre;
        file->pos = 0;
        file->size = size;
        file->is_world_stream = path_is_world_stream_archive(path);
        snprintf(file->path, sizeof(file->path), "%s", path);
        LOGC(LOGC_FILE, "[NvF] NvFOpen('%s', caller=%p) -> SUCCESS (RAM-backed handle=%p, size=%ld)\n", path, caller, file, size);
        return file;
      }
    }
  }

  uint64_t t0 = io_tick_now();
  FILE *f = open_asset_with_fallback(path);
  if (!f) {
    LOGC(LOGC_FILE, "[NvF] NvFOpen('%s', caller=%p) -> FAILED\n", path, caller);
    return NULL;
  }
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  NvFile *file = calloc(1, sizeof(NvFile));
  if (!file) {
    fclose(f);
    return NULL;
  }
  file->f = f;
  file->preload = NULL;
  file->pos = 0;
  file->is_world_stream = path_is_world_stream_archive(path);
  snprintf(file->path, sizeof(file->path), "%s", path);

  struct stat st;
  if (fstat(fileno(f), &st) == 0) {
    file->size = st.st_size;
  } else {
    fseek(f, 0, SEEK_END);
    file->size = ftell(f);
    fseek(f, 0, SEEK_SET);
  }

  double open_ms = io_tick_to_ms(io_tick_now() - t0);
  if (open_ms >= IO_SLOW_MS) {
    LOGC(LOGC_SYS, "[NvF] SLOW OPEN: '%s' took %.2f ms (size=%ld)\n", path, open_ms, file->size);
  }
  sc_open(file, path, file->f, "rb");
  LOGC(LOGC_FILE, "[NvF] NvFOpen('%s', caller=%p) -> handle=%p (size=%ld)\n", path, caller, file, file->size);
  return file;
}

int NvFClose_hook(void *h) {
  void *caller = __builtin_return_address(0);
  if (!h) return -1;
  NvFile *file = (NvFile *)h;
  LOGC(LOGC_FILE, "[NvF] NvFClose(handle=%p, caller=%p)\n", h, caller);
  if (file->preload) {
    free(file);
    return 0;
  }
  sc_close(file);
  if (file->f) fclose(file->f);
  free(file);
  return 0;
}

size_t NvFRead_hook(void *ptr, size_t size, size_t n, void *h) {
  if (!h || !ptr) return 0;
  NvFile *file = (NvFile *)h;
  size_t total_bytes = size * n;
  if (total_bytes == 0) return 0;

  begin_active_io(file->path[0] ? file->path : "unknown", total_bytes);

  if (file->preload) {
    size_t read_bytes = sc_preload_read(file->preload, ptr, total_bytes, &file->pos);
    size_t res = read_bytes / (size ? size : 1);
    if (file->is_world_stream && read_bytes > 0) {
      mark_streaming_activity();
    }
    end_active_io(read_bytes);
    return res;
  }

  uint64_t t0 = io_tick_now();
  size_t read_bytes = sc_read(file, file->f, ptr, total_bytes);
  size_t res = read_bytes / (size ? size : 1);
  double read_ms = io_tick_to_ms(io_tick_now() - t0);

  if (file->is_world_stream && read_bytes > 0) {
    mark_streaming_activity();
  }

  end_active_io(read_bytes);

  if (read_ms >= IO_SLOW_MS) {
    LOGC(LOGC_SYS, "[NvF] SLOW READ: handle=%p size=%zu*%zu took %.2f ms\n", h, size, n, read_ms);
  }
  return res;
}

int NvFSeek_hook(void *h, long offset, int whence) {
  if (!h) return -1;
  NvFile *file = (NvFile *)h;
  if (file->preload) {
    sc_preload_seek(file->preload, offset, whence, &file->pos);
    return 0;
  }
  return fseek(file->f, offset, whence);
}

long NvFTell_hook(void *h) {
  if (!h) return -1;
  NvFile *file = (NvFile *)h;
  if (file->preload) {
    return file->pos;
  }
  return ftell(file->f);
}

long NvFSize_hook(void *h) {
  if (!h) return 0;
  NvFile *file = (NvFile *)h;
  return file->size;
}

int NvFGetc_hook(void *h) {
  if (!h) return EOF;
  NvFile *file = (NvFile *)h;
  if (file->preload) {
    if (file->pos >= file->size) return EOF;
    uint8_t ch = file->preload->data[file->pos++];
    return ch;
  }
  return fgetc(file->f);
}

char *NvFGets_hook(char *str, int num, void *h) {
  if (!h || !str) return NULL;
  NvFile *file = (NvFile *)h;
  if (file->preload) {
    if (num <= 1 || file->pos >= file->size) return NULL;
    int idx = 0;
    while (idx < num - 1 && file->pos < file->size) {
      char c = (char)file->preload->data[file->pos++];
      str[idx++] = c;
      if (c == '\n') break;
    }
    str[idx] = '\0';
    return (idx > 0) ? str : NULL;
  }
  return fgets(str, num, file->f);
}

int NvFEOF_hook(void *h) {
  if (!h) return 1;
  NvFile *file = (NvFile *)h;
  if (file->preload) {
    return file->pos >= file->size ? 1 : 0;
  }
  return feof(file->f);
}

// OSFileAccessType, read off the jump table the original OS_FileOpen builds at
// .rodata 0xea7d4 in libGame.so:
//   0 -> NvFOpen (read-only asset)     2 -> fopen "rb+", falling back to "wb+"
//   1 -> fopen "wb"                    3 -> NvFOpen, streamed (CdStreamAddImage)
// Only 0, 1 and 3 are reached in practice; 1 is what CFileMgr::OpenFile turns
// any non-"r" mode string into, so it carries every save game and gta3.set.
#define OS_ACCESS_READ   0
#define OS_ACCESS_WRITE  1
#define OS_ACCESS_RDWR   2
#define OS_ACCESS_STREAM 3

// OSFileDataArea: 1 is the user-data area. The original OS_FileOpen derives the
// same predicate at 0x36d818 -- (area == 1) || (mode is write or read/write) --
// and uses it to decide whether AND_NormalizePath prefixes StorageRootPath and
// mkdir()s the directory. We replace OS_FileOpen outright, so that work has to
// happen here; without it every write lands on a path nobody prepared.
#define OS_AREA_USER 1

static void os_build_path(const char *dir, const char *path, char *out, size_t out_len) {
  const char *p = path;
  if (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
  while (*p == '/' || *p == '\\') p++;

  if (dir)
    snprintf(out, out_len, "%s/%s/%s", jni_storage_root(), dir, p);
  else
    snprintf(out, out_len, "%s/%s", jni_storage_root(), p);
  for (char *c = out; *c; c++)
    if (*c == '\\') *c = '/';
}

// The settings file stays beside the NRO where it has always been; only the
// save slots move into USER_DATA_DIR.
static bool os_stays_in_root(const char *path) {
  const char *name = path;
  for (const char *c = path; *c; c++)
    if (*c == '/' || *c == '\\') name = c + 1;
  return strcasecmp(name, "gta3.set") == 0;
}

static void os_user_path(const char *path, char *out, size_t out_len) {
  os_build_path(os_stays_in_root(path) ? NULL : USER_DATA_DIR, path, out, out_len);
}

// Where user data landed before it moved into USER_DATA_DIR. Reads fall back to
// it so saves made by an earlier build still load, and deletes clear it so a
// slot erased in the menu cannot reappear from here.
static void os_legacy_user_path(const char *path, char *out, size_t out_len) {
  os_build_path(NULL, path, out, out_len);
}

static void os_make_parent_dirs(char *path) {
  for (char *c = strchr(path + 1, '/'); c; c = strchr(c + 1, '/')) {
    *c = '\0';
    mkdir(path, 0777);
    *c = '/';
  }
}

// Wrap an already-open stream in the handle the OS_File* hooks pass around.
static int os_make_handle(void **out_handle, FILE *f, const char *full) {
  OSFile *file = calloc(1, sizeof(OSFile));
  if (!file) {
    fclose(f);
    *out_handle = NULL;
    return 1;
  }
  file->f = f;
  file->preload = NULL;
  file->pos = 0;
  file->is_world_stream = false;
  snprintf(file->path, sizeof(file->path), "%s", full);

  struct stat st;
  file->size = (fstat(fileno(f), &st) == 0) ? st.st_size : 0;

  *out_handle = file;
  return 0;
}

static int os_file_open_write(void **out_handle, const char *path, int mode) {
  char full[1024];
  os_user_path(path, full, sizeof(full));
  os_make_parent_dirs(full);

  if (mode == OS_ACCESS_WRITE) {
    // Plain fopen(full, "wb") truncates the existing save the instant it's
    // opened. CFileMgr's save sequence writes several files per slot, and if
    // the process dies mid-write -- sleep, HOME-menu kill, power loss -- the
    // slot is left with a zero-length or half-written file and the old save
    // is already gone. Write to a temp sibling and rename over the target on
    // close instead, so a save is atomic: either the old file or the fully
    // written new one, never a partial one.
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
      LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') FAILED to open '%s' for writing\n", path, tmp);
      *out_handle = NULL;
      return 1;
    }

    LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') -> writable (atomic) '%s'\n", path, full);
    int rc = os_make_handle(out_handle, f, full);
    if (rc == 0) {
      NvFile *file = (NvFile *)*out_handle;
      file->pending_rename = true;
      snprintf(file->tmp_path, sizeof(file->tmp_path), "%s", tmp);
    }
    return rc;
  }

  FILE *f = fopen(full, "rb+");
  if (!f)
    f = fopen(full, "wb+");
  if (!f) {
    LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') FAILED to open '%s' for writing\n", path, full);
    *out_handle = NULL;
    return 1;
  }

  LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') -> writable '%s'\n", path, full);
  return os_make_handle(out_handle, f, full);
}

// Returns 0 on success; 1 means "not user data after all", so the caller should
// let the ordinary asset lookup try the name.
static int os_file_open_user_read(void **out_handle, const char *path) {
  char full[1024];
  os_user_path(path, full, sizeof(full));
  FILE *f = fopen(full, "rb");
  if (!f) {
    os_legacy_user_path(path, full, sizeof(full));
    f = fopen(full, "rb");
  }
  if (!f) return 1;

  LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') -> user data '%s'\n", path, full);
  return os_make_handle(out_handle, f, full);
}

static bool has_extension(const char *path, const char *ext) {
  if (!path || !ext) return false;
  size_t l1 = strlen(path), l2 = strlen(ext);
  if (l1 < l2) return false;
  return strcasecmp(path + l1 - l2, ext) == 0;
}

int OS_FileOpen_hook(int area, void **out_handle, const char *path, int mode) {
  void *caller = __builtin_return_address(0);
  if (!out_handle || !path) return 1;
  set_active_io_path(path);
  LOGC(LOGC_FILE, "[OS_File] OS_FileOpen(area=%d, path='%s', mode=%d, caller=%p)...\n", area, path, mode, caller);

  if (mode == OS_ACCESS_WRITE || mode == OS_ACCESS_RDWR)
    return os_file_open_write(out_handle, path, mode);

  if (area == OS_AREA_USER && os_file_open_user_read(out_handle, path) == 0)
    return 0;

  uint64_t t0 = io_tick_now();
  const char *resolved_path = path;
  FILE *f = open_asset_with_fallback(path);
  if (!f && path) {
    if (has_extension(path, ".dir") || strstr(path, ".DIR") || strstr(path, ".dir")) {
      LOGC(LOGC_FILE, "[OS_File] .DIR file '%s' missing! Fallback to 'MODELS/GTA3_DXT.DIR'...\n", path);
      resolved_path = "MODELS/GTA3_DXT.DIR";
      f = open_asset_with_fallback(resolved_path);
      if (!f) {
        resolved_path = "models/gta3_dxt.dir";
        f = open_asset_with_fallback(resolved_path);
      }
    } else if (has_extension(path, ".img") || strstr(path, ".IMG") || strstr(path, ".img")) {
      LOGC(LOGC_FILE, "[OS_File] .IMG file '%s' missing! Fallback to 'MODELS/GTA3_DXT.IMG'...\n", path);
      resolved_path = "MODELS/GTA3_DXT.IMG";
      f = open_asset_with_fallback(resolved_path);
      if (!f) {
        resolved_path = "models/gta3_dxt.img";
        f = open_asset_with_fallback(resolved_path);
      }
    }
  }

  if (!f && path && mode == OS_ACCESS_STREAM) {
    LOGC(LOGC_FILE, "[OS_File] Stream file '%s' (mode=%d) missing! Fallback to 'MODELS/GTA3_DXT.IMG'...\n", path, mode);
    resolved_path = "MODELS/GTA3_DXT.IMG";
    f = open_asset_with_fallback(resolved_path);
    if (!f) {
      resolved_path = "models/gta3_dxt.img";
      f = open_asset_with_fallback(resolved_path);
    }
  }

  if (sc_is_preload_archive(resolved_path)) {
    long size = 0;
    sc_preload_t *pre = sc_get_or_load_preload(resolved_path, &size);
    if (pre) {
      if (f) fclose(f);
      OSFile *file = calloc(1, sizeof(OSFile));
      if (file) {
        file->f = NULL;
        file->preload = pre;
        file->pos = 0;
        file->size = size;
        file->is_world_stream = path_is_world_stream_archive(resolved_path);
        snprintf(file->path, sizeof(file->path), "%s", resolved_path);
        *out_handle = file;
        LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') SUCCESS -> RAM-backed handle=%p (size=%ld)\n", path, file, size);
        return 0;
      }
    }
  }

  if (!f) {
    LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') FAILED\n", path);
    *out_handle = NULL;
    return 1;
  }

  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  OSFile *file = calloc(1, sizeof(OSFile));
  if (!file) {
    fclose(f);
    *out_handle = NULL;
    return 1;
  }
  file->f = f;
  file->preload = NULL;
  file->pos = 0;
  file->is_world_stream = path_is_world_stream_archive(resolved_path);
  snprintf(file->path, sizeof(file->path), "%s", resolved_path);

  struct stat st;
  if (fstat(fileno(f), &st) == 0) {
    file->size = st.st_size;
  } else {
    fseek(f, 0, SEEK_END);
    file->size = ftell(f);
    fseek(f, 0, SEEK_SET);
  }

  *out_handle = file;

  double open_ms = io_tick_to_ms(io_tick_now() - t0);
  if (open_ms >= IO_SLOW_MS) {
    LOGC(LOGC_SYS, "[OS_File] SLOW OPEN: '%s' took %.2f ms (size=%ld)\n", path, open_ms, file->size);
  }
  char mode_str[16];
  snprintf(mode_str, sizeof(mode_str), "%d", mode);
  sc_open(file, resolved_path, file->f, mode_str);

  LOGC(LOGC_FILE, "[OS_File] OS_FileOpen('%s') SUCCESS -> handle=%p (size=%ld)\n", path, file, file->size);
  return 0;
}

int OS_FileClose_hook(void *h) {
  void *caller = __builtin_return_address(0);
  if (!h || (uintptr_t)h < 0x10000) return 0;
  OSFile *file = (OSFile *)h;
  LOGC(LOGC_FILE, "[OS_File] OS_FileClose(handle=%p, caller=%p)\n", h, caller);
  if (file->preload) {
    free(file);
    return 0;
  }
  sc_close(file);
  if (file->f) {
    if (file->pending_rename) {
      fflush(file->f);
      fsync(fileno(file->f));
    }
    fclose(file->f);
  }
  if (file->pending_rename) {
    if (rename(file->tmp_path, file->path) != 0) {
      LOGC(LOGC_FILE, "[OS_File] OS_FileClose: FAILED to rename '%s' -> '%s' (errno=%d)\n",
           file->tmp_path, file->path, errno);
    } else {
      LOGC(LOGC_FILE, "[OS_File] OS_FileClose: committed '%s'\n", file->path);
    }
  }
  free(file);
  return 0;
}

int OS_FileRead_hook(void *h, void *buf, int size) {
  if (!h || (uintptr_t)h < 0x10000 || !buf || size <= 0) return 1;
  OSFile *file = (OSFile *)h;

  begin_active_io(file->path[0] ? file->path : "unknown", (size_t)size);

  if (file->preload) {
    size_t res = sc_preload_read(file->preload, buf, (size_t)size, &file->pos);
    if (file->is_world_stream && res > 0) {
      mark_streaming_activity();
    }
    end_active_io(res);
    return (res == (size_t)size) ? 0 : 1;
  }

  uint64_t t0 = io_tick_now();
  size_t res = sc_read(file, file->f, buf, (size_t)size);
  double read_ms = io_tick_to_ms(io_tick_now() - t0);

  if (file->is_world_stream && res > 0) {
    mark_streaming_activity();
  }

  end_active_io(res);

  if (read_ms >= IO_SLOW_MS) {
    LOGC(LOGC_SYS, "[OS_File] SLOW READ: handle=%p size=%d took %.2f ms\n", h, size, read_ms);
  }
  return (res == (size_t)size) ? 0 : 1;
}

int OS_FileWrite_hook(void *h, const void *buf, int size) {
  if (!h || (uintptr_t)h < 0x10000 || !buf || size <= 0) return 1;
  OSFile *file = (OSFile *)h;
  if (file->preload) return 1;
  size_t res = fwrite(buf, 1, size, file->f);
  return (res == (size_t)size) ? 0 : 1;
}

int OS_FileSize_hook(void *h) {
  if (!h || (uintptr_t)h < 0x10000) return 0;
  OSFile *file = (OSFile *)h;
  return file->size;
}

int OS_FileSetPosition_hook(void *h, int pos) {
  if (!h || (uintptr_t)h < 0x10000) return 3;
  OSFile *file = (OSFile *)h;
  if (file->preload) {
    sc_preload_seek(file->preload, pos, SEEK_SET, &file->pos);
    return 0;
  }
  int res = fseek(file->f, pos, SEEK_SET);
  return (res == 0) ? 0 : 3;
}

int OS_FileGetPosition_hook(void *h) {
  if (!h || (uintptr_t)h < 0x10000) return 0;
  OSFile *file = (OSFile *)h;
  if (file->preload) {
    return (int)file->pos;
  }
  return (int)ftell(file->f);
}

/* Streaming context lifecycle & activation gate */
const char *streaming_context_to_string(StreamingContext ctx) {
  switch (ctx) {
    case STREAM_GAMEPLAY: return "GAMEPLAY";
    case STREAM_LOADING_SCREEN: return "LOADING_SCREEN";
    case STREAM_BOOTSTRAP: return "BOOTSTRAP";
    default: return "UNKNOWN";
  }
}

const char *stream_runtime_mode_to_string(StreamingRuntimeMode mode) {
  switch (mode) {
    case STREAM_RUNTIME_BOOT: return "BOOT";
    case STREAM_RUNTIME_TRANSITION: return "TRANSITION";
    case STREAM_RUNTIME_GAMEPLAY: return "GAMEPLAY";
    default: return "UNKNOWN";
  }
}

_Atomic int g_streaming_context = STREAM_BOOTSTRAP;
_Atomic bool g_gameplay_streaming_enabled = false;
_Atomic int g_stream_runtime_mode = STREAM_RUNTIME_BOOT;
_Atomic unsigned g_stream_transition_frames = 0;
_Atomic bool g_gameplay_transition_pending = false;

static _Atomic uint64_t g_streaming_call_start_tick = 0;
_Atomic int g_models_loaded_this_frame = 0;
_Atomic uint64_t g_streaming_frame_cpu_ns = 0;
static _Atomic bool g_frame_yielded_streaming = false;

bool gameplay_streaming_enabled(void) {
  return atomic_load_explicit(&g_gameplay_streaming_enabled, memory_order_acquire);
}

void set_gameplay_streaming_enabled(bool enabled) {
  bool prev = atomic_exchange_explicit(&g_gameplay_streaming_enabled, enabled, memory_order_acq_rel);
  if (prev != enabled) {
    LOGC(LOGC_SYS, "[STREAM_POLICY] gameplay_streaming_enabled=%d (frame #%u)\n", enabled ? 1 : 0, g_frame_count);
  }
}

StreamingRuntimeMode get_stream_runtime_mode(void) {
  return (StreamingRuntimeMode)atomic_load_explicit(&g_stream_runtime_mode, memory_order_relaxed);
}

void set_stream_runtime_mode(StreamingRuntimeMode mode, unsigned grace_frames) {
  StreamingRuntimeMode prev = (StreamingRuntimeMode)atomic_exchange_explicit(&g_stream_runtime_mode, (int)mode, memory_order_acq_rel);
  atomic_store_explicit(&g_stream_transition_frames, grace_frames, memory_order_release);
  LOGC(LOGC_SYS, "[STREAM_POLICY] runtime_mode=%s (prev=%s, grace_frames=%u, frame #%u)\n",
       stream_runtime_mode_to_string(mode), stream_runtime_mode_to_string(prev), grace_frames, g_frame_count);
}

void process_gradual_prewarm_queue(void);
static void log_preload_feasibility_survey(void);
static void run_one_time_txd_preload(void);
static void run_one_time_area_preload(void);
static bool prewarm_seed_batch_drained(void);

void update_streaming_transition_frame(void) {
  unsigned rem = atomic_load_explicit(&g_stream_transition_frames, memory_order_relaxed);
  if (rem > 0) {
    rem--;
    atomic_store_explicit(&g_stream_transition_frames, rem, memory_order_relaxed);
    if (rem == 0) {
      atomic_store_explicit(&g_stream_runtime_mode, STREAM_RUNTIME_GAMEPLAY, memory_order_release);
      LOGC(LOGC_SYS, "[STREAM_POLICY] TRANSITION GRACE EXPIRED -> STREAM_RUNTIME_GAMEPLAY (frame #%u)\n", g_frame_count);
    } else if (rem % 10 == 0 || rem <= 5) {
      LOGC(LOGC_SYS, "[STREAM_TRANSITION] grace_remaining=%u (frame #%u)\n", rem, g_frame_count);
    }
  }
  
  // Start prewarm queue immediately during transition grace & gameplay
  StreamingRuntimeMode mode = (StreamingRuntimeMode)atomic_load_explicit(&g_stream_runtime_mode, memory_order_relaxed);
  if (mode == STREAM_RUNTIME_TRANSITION || mode == STREAM_RUNTIME_GAMEPLAY) {
    process_gradual_prewarm_queue();
  }
}

StreamingContext get_streaming_context(void) {
  return (StreamingContext)atomic_load_explicit(&g_streaming_context, memory_order_relaxed);
}

void streaming_set_context(StreamingContext new_ctx) {
  StreamingContext prev = (StreamingContext)atomic_exchange_explicit(&g_streaming_context, (int)new_ctx, memory_order_acq_rel);
  if (prev != new_ctx) {
    LOGC(LOGC_SYS, "[STREAM_POLICY] context=%s (prev=%s, frame #%u)\n",
         streaming_context_to_string(new_ctx), streaming_context_to_string(prev), g_frame_count);
  }
}

static inline StreamingContext push_streaming_context(StreamingContext new_ctx) {
  StreamingContext prev = (StreamingContext)atomic_exchange_explicit(&g_streaming_context, (int)new_ctx, memory_order_acq_rel);
  LOGC(LOGC_SYS, "[STREAM_POLICY] push_context=%s (prev=%s, frame #%u)\n",
       streaming_context_to_string(new_ctx), streaming_context_to_string(prev), g_frame_count);
  return prev;
}

static inline void pop_streaming_context(StreamingContext prev_ctx) {
  StreamingContext curr = (StreamingContext)atomic_exchange_explicit(&g_streaming_context, (int)prev_ctx, memory_order_acq_rel);
  LOGC(LOGC_SYS, "[STREAM_POLICY] pop_context=%s (restored=%s, frame #%u)\n",
       streaming_context_to_string(curr), streaming_context_to_string(prev_ctx), g_frame_count);
}

void reset_frame_streaming_budget(void) {
  bool prev_yield = atomic_load_explicit(&g_frame_yielded_streaming, memory_order_relaxed);
  if (prev_yield) {
    LOGC(LOGC_SYS, "[STREAM_BUDGET] frame=%u used=0.00ms models=0 action=RESUME\n", g_frame_count);
    atomic_store_explicit(&g_frame_yielded_streaming, false, memory_order_relaxed);
  }
  atomic_store_explicit(&g_models_loaded_this_frame, 0, memory_order_relaxed);
  atomic_store_explicit(&g_streaming_frame_cpu_ns, 0, memory_order_relaxed);
}

int OS_FileGetState_hook(void *h) {
  (void)h;
  return 0; // 0 = Ready / Idle (always report ready so no textures or geometry are dropped)
}

void OS_DebugBreak_hook(void) {
  void *caller = __builtin_return_address(0);
  debugPrintf("[HOOK] CRITICAL: OS_DebugBreak called from %p!\n", caller);
}

int OS_FileFlush_hook(void *h) {
  if (!h) return 0;
  OSFile *file = (OSFile *)h;
  if (file->preload) return 0;
  if (file->f) fflush(file->f);
  return 0;
}

int OS_FileRename_hook(int area, const char *oldpath, const char *newpath, bool b) {
  (void)area; (void)b;
  if (!oldpath || !newpath) return -1;
  char from[1024], to[1024];
  os_user_path(oldpath, from, sizeof(from));
  os_user_path(newpath, to, sizeof(to));
  os_make_parent_dirs(to);
  struct stat st;
  if (stat(from, &st) != 0)
    os_legacy_user_path(oldpath, from, sizeof(from));
  debugPrintf("[OS_File] OS_FileRename('%s' -> '%s')\n", from, to);
  return rename(from, to);
}

int OS_FileDelete_hook(int area, const char *path) {
  (void)area;
  if (!path) return -1;
  char full[1024], legacy[1024];
  os_user_path(path, full, sizeof(full));
  os_legacy_user_path(path, legacy, sizeof(legacy));
  debugPrintf("[OS_File] OS_FileDelete('%s')\n", full);
  int rc = remove(full);
  // Clear the pre-USER_DATA_DIR copy too, or the read fallback resurrects it.
  if (remove(legacy) == 0) rc = 0;
  return rc;
}

/* ============================================================================
 * SUBSYSTEM PHASE PROFILING
 * ============================================================================
 * Objective: Track which game subsystem (e.g. CTheScripts::Process, CWorld::Process,
 * CPopulation::Update, CCarCtrl::GenerateRandomCars) is running when non-I/O stalls
 * trigger the watchdog thread, rather than seeing generic "implOnDrawFrame".
 */

#ifdef PHASE_TEST_DELAY
static int g_phase_test_delay_ms = 200;
#else
static int g_phase_test_delay_ms = 0;
#endif

#define DEFINE_MEMBER_PHASE_HOOK(name, symbol_str) \
  typedef void (*MemberFunc_##name)(void *this_ptr); \
  static MemberFunc_##name orig_##name = NULL; \
  static void phase_wrap_##name(void *this_ptr) { \
    const char *prev = g_active_phase; \
    g_active_phase = symbol_str; \
    uint64_t phase_t0 = io_tick_now(); \
    double io_wait_start = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed); \
    if (g_phase_test_delay_ms > 0) usleep(g_phase_test_delay_ms * 1000); \
    if (orig_##name) orig_##name(this_ptr); \
    double total_ms = io_tick_to_ms(io_tick_now() - phase_t0); \
    double io_wait_end = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed); \
    double nested_io_wait = io_wait_end - io_wait_start; \
    if (nested_io_wait < 0) nested_io_wait = 0; \
    double non_io_elapsed = total_ms - nested_io_wait; \
    if (non_io_elapsed < 0) non_io_elapsed = 0; \
    if (total_ms >= 5.0) { \
      LOGC(LOGC_SYS, "[PHASE_METRICS] %s: total=%.2fms (io_wait=%.2fms, non_io=%.2fms)\n", \
           symbol_str, total_ms, nested_io_wait, non_io_elapsed); \
    } \
    g_active_phase = prev; \
  }

DEFINE_MEMBER_PHASE_HOOK(CMenuManager_Process, "CMenuManager::Process")

/* Dedicated wrapper for CStreaming::Update to manage live start tick */
typedef void (*MemberFunc_CStreaming_Update)(void *this_ptr);
static MemberFunc_CStreaming_Update orig_CStreaming_Update = NULL;

static void phase_wrap_CStreaming_Update(void *this_ptr) {
  const char *prev = g_active_phase;
  g_active_phase = "CStreaming::Update";
  uint64_t phase_t0 = io_tick_now();
  double io_wait_start = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed);

  atomic_store_explicit(&g_streaming_call_start_tick, phase_t0, memory_order_release);

  if (orig_CStreaming_Update) orig_CStreaming_Update(this_ptr);

  atomic_store_explicit(&g_streaming_call_start_tick, 0, memory_order_release);

  double total_ms = io_tick_to_ms(io_tick_now() - phase_t0);
  double io_wait_end = atomic_load_explicit(&g_phase_io_wait_ms, memory_order_relaxed);
  double nested_io_wait = io_wait_end - io_wait_start;
  if (nested_io_wait < 0) nested_io_wait = 0;
  double non_io_elapsed = total_ms - nested_io_wait;
  if (non_io_elapsed < 0) non_io_elapsed = 0;
  if (total_ms >= 5.0) {
    LOGC(LOGC_SYS, "[PHASE_METRICS] CStreaming::Update: total=%.2fms (io_wait=%.2fms, non_io=%.2fms)\n",
         total_ms, nested_io_wait, non_io_elapsed);
  }
  g_active_phase = prev;
}

/* Call hierarchy scope tracing */
static _Thread_local int g_stream_scope_depth = 0;

#define STREAM_SCOPE_ENTER(func_name, extra_fmt, ...) \
  int _depth = g_stream_scope_depth++; \
  uint64_t _t0 = armGetSystemTick(); \
  if ((g_log_mask & LOGC_SYS) && _depth <= 3) { \
    LOGC(LOGC_SYS, "[STREAM_SCOPE] %*s> %s (frame #%u" extra_fmt ")\n", \
         _depth * 2, "", func_name, g_frame_count, ##__VA_ARGS__); \
  }

#define STREAM_SCOPE_EXIT(func_name) do { \
  uint64_t _dt = armTicksToNs(armGetSystemTick() - _t0); \
  g_stream_scope_depth--; \
  double _dt_ms = (double)_dt / 1e6; \
  if ((g_log_mask & LOGC_SYS)) { \
    if (_dt_ms >= 2.0) { \
      LOGC(LOGC_SYS, "[STREAM_SCOPE] %*s< %s (took %.2fms [SLOW])\n", \
           _depth * 2, "", func_name, _dt_ms); \
    } else if (_depth <= 3 && _dt_ms >= 0.5) { \
      LOGC(LOGC_SYS, "[STREAM_SCOPE] %*s< %s (took %.2fms)\n", \
           _depth * 2, "", func_name, _dt_ms); \
    } \
  } \
} while (0)

/* Telemetry: Overshoot Reason classification */
typedef enum {
  OVERSHOOT_NONE = 0,
  OVERSHOOT_MODEL_SINGLE_OP,
  OVERSHOOT_MODEL_COUNT_LIMIT,
  OVERSHOOT_BULK_CALLER,
  OVERSHOOT_LOAD_SCENE,
  OVERSHOOT_OTHER
} OvershootReason;

static const char *overshoot_reason_to_string(OvershootReason r) {
  switch (r) {
    case OVERSHOOT_MODEL_SINGLE_OP: return "MODEL_SINGLE_OP";
    case OVERSHOOT_MODEL_COUNT_LIMIT: return "MODEL_COUNT_LIMIT";
    case OVERSHOOT_BULK_CALLER: return "BULK_CALLER";
    case OVERSHOOT_LOAD_SCENE: return "LOAD_SCENE";
    case OVERSHOOT_OTHER: return "OTHER";
    default: return "NONE";
  }
}

static _Atomic double g_frame_max_model_ms = 0.0;
static _Atomic int g_frame_max_model_id = -1;
static _Atomic double g_frame_max_txd_ms = 0.0;
static _Atomic int g_frame_max_txd_id = -1;
static _Atomic int g_frame_overshoot_reason = OVERSHOOT_NONE;

/* Per-frame streaming cost, consumed by the FPS overlay's spike logging so a
 * reported stall can name what caused it instead of only how long it lasted.
 * Kept separate from the accumulators above because the overlay owns the
 * frame boundary for these (it read-and-resets them once per eglSwapBuffers)
 * and must not disturb anyone else's bookkeeping. */
/* Texture-pipeline breakdown accumulators. Totals rather than per-call logs:
 * at ~4300 texture reads a session the interesting number is where the 8.27ms
 * average goes, not any individual call. */
/* Defined in imports.c -- the periodic glFlush that guards the GL upload
 * queue, measured here because it is charged to the texture pipeline. */
extern _Atomic uint64_t g_gl_flush_ns;
extern _Atomic uint64_t g_gl_flush_calls;
extern _Atomic uint64_t g_gl_mip_skipped;
extern _Atomic uint64_t g_gl_mip_uploaded;
extern _Atomic uint64_t g_gl_texstorage_allocs;
extern _Atomic uint64_t g_gl_texstorage_fallbacks;
extern _Atomic uint64_t g_gl_subimage_errors;

static _Atomic uint64_t g_txpipe_texread_ns = 0;
static _Atomic uint64_t g_txpipe_texread_calls = 0;
static _Atomic uint64_t g_txpipe_compressed_ns = 0;
static _Atomic uint64_t g_txpipe_compressed_calls = 0;
static _Atomic uint64_t g_txpipe_raster_ns = 0;
static _Atomic uint64_t g_txpipe_raster_calls = 0;

static _Atomic uint64_t g_overlay_convert_ns = 0;
static _Atomic uint64_t g_overlay_worst_ns = 0;
static _Atomic int g_overlay_worst_index = -1;

/* Read-and-reset this frame's streaming totals. Declared extern in overlay.c.
 * Relaxed ordering throughout: these are diagnostics, and a torn read at a
 * frame boundary costs a slightly misattributed log line, never correctness. */
void gta3_take_frame_stream_stats(double *out_total_ms, int *out_worst_index,
                                  double *out_worst_ms) {
  uint64_t total = atomic_exchange_explicit(&g_overlay_convert_ns, 0, memory_order_relaxed);
  uint64_t worst = atomic_exchange_explicit(&g_overlay_worst_ns, 0, memory_order_relaxed);
  int idx = atomic_exchange_explicit(&g_overlay_worst_index, -1, memory_order_relaxed);
  if (out_total_ms) *out_total_ms = (double)total / 1e6;
  if (out_worst_ms) *out_worst_ms = (double)worst / 1e6;
  if (out_worst_index) *out_worst_index = idx;
}

/* Residency priority definitions */
typedef enum {
  RES_NORMAL = 0,
  RES_RECENTLY_USED = 1,
  RES_PREWARM_AREA = 2,
  RES_ACTIVE_AREA = 3,
  RES_MISSION_PINNED = 4
} ResidencyTier;

typedef struct {
  ResidencyTier tier;
  uint64_t last_used_tick;
  uint32_t load_count;
  uint32_t reload_count;
  uint32_t evict_count;
  float last_duration_ms;
  int last_txd_index;
} ModelResidency;

typedef struct {
  int txd_index;
  ResidencyTier tier;
  uint64_t last_used_tick;
  uint32_t load_count;
  uint32_t reload_count;
  uint32_t evict_count;
} TxdResidency;

/* Cost Classification and Historical Model Database */
typedef enum {
  COST_CAT_NORMAL = 0,     // < 30ms
  COST_CAT_EXPENSIVE = 1,  // 30ms - 100ms
  COST_CAT_SEVERE = 2,     // 100ms - 200ms
  COST_CAT_CATASTROPHIC = 3// >= 200ms
} ModelCostCategory;

typedef struct {
  float last_duration_ms;
  float max_duration_ms;
  float avg_duration_ms;
  uint32_t load_count;
  uint32_t reload_count;
  uint32_t evict_count;
  uint32_t expensive_count;
  int last_txd_index;
  uint64_t last_used_tick;
  bool is_seeded_heavy;
} ModelCostEntry;

/* ---- Streaming index-space constants (see each block for the objdump
 * evidence). Defined here, ahead of every user, because both the
 * candidate table and the load-state accessor below depend on them. ---- */
/* Number of entries in CModelInfo::ms_modelInfoPtrs -- i.e. the highest
 * streaming index that is actually a MODEL. Verified against the binary two
 * independent ways (do not raise this to match our own 7000-entry tracking
 * arrays -- they are different things):
 *   1. objdump -T lib/libGame.so reports _ZN10CModelInfo16ms_modelInfoPtrsE
 *      as a .bss object of size 0xABE0 = 44000 bytes = 5500 pointers.
 *   2. CStreaming::LoadAllRequestedModels itself branches on the boundary:
 *      2bb028: mov w8, #0x157b (5499) / cmp w20, w8.
 * Streaming indices >= this value are TXD entries, NOT models, and indexing
 * ms_modelInfoPtrs with one reads past the end of the array into adjacent
 * .bss. That garbage is often small-but-nonzero (a literal 1 was observed),
 * so a plain "if (mi)" NULL guard does NOT catch it -- the deref of the type
 * byte then faults at 0x37. Our own per-model tracking arrays are sized 7000
 * because they are legitimately indexed by streaming index (TXDs included);
 * only ms_modelInfoPtrs lookups need this tighter bound. */
#define MODELINFO_COUNT 5500
/* A model's texture dictionary, and where TXDs live in the streaming index
 * space. Both verified by disassembling CStreaming::RequestModel (0x2b7b24),
 * which maps a model to its TXD in order to request it recursively:
 *   2b7b74: mov   w9, #0x157c            ; 5500 = STREAM_OFFSET_TXD
 *   2b7b78: cmp   w0, w9 / b.ge          ; only for real models
 *   2b7b90: ldr   x8, [x10, x8, lsl #3]  ; mi = ms_modelInfoPtrs[id]
 *   2b7b94: ldrsh w8, [x8, #52]          ; m_txdIndex (SIGNED int16 @ 52)
 *   2b7b98: add   w0, w8, w9             ; streaming index = txdIndex + 5500
 * The load is ldrsh, not ldrh: m_txdIndex is signed and -1 means "no TXD",
 * so it must be range-checked before use. STREAM_OFFSET_TXD is numerically
 * the same as MODELINFO_COUNT (TXD slots begin exactly where models end);
 * they are spelled separately because they mean different things. */
#define CMODELINFO_OFF_TXDINDEX 52
#define STREAM_OFFSET_TXD MODELINFO_COUNT
/* Total number of streaming slots: models (0..5499) followed by TXDs
 * (5500..6523, i.e. 1024 dictionaries). This is the bound for ANY index
 * handed to the engine or used against CStreaming::ms_aInfoForModel -- it is
 * NOT 7000. Our own tracking arrays are 7000 and stay that way; this bound
 * exists specifically to keep out-of-range indices away from engine memory.
 * Verified twice against libGame.so:
 *   1. _ZN10CStreaming16ms_aInfoForModelE is a .bss object of 0x32F80 =
 *      208768 bytes at 32 bytes/entry = 6524 entries.
 *   2. The array's init loop at 0x2b43e0 counts down from exactly that:
 *      2b43e0: mov w9, #0x197c   ; 6524
 * This matters more than a read bound: CStreaming::RequestModel WRITES to
 * ms_aInfoForModel[index], so passing it an out-of-range index corrupts
 * whatever .bss follows rather than merely returning garbage. */
#define NUMSTREAMINFO 6524

static ModelCostEntry g_model_cost_db[7000];
static TxdResidency g_txd_residency[2048];

/* Frame at which a model first appeared assigned to a streaming channel
 * (i.e. the engine's own request/streaming pipeline picked it up), or -1
 * if never observed. Tracked for EVERY model, not just already-known-heavy
 * ones, specifically to answer: for a model that costs 100ms+ on first
 * conversion, was it actually sitting in the engine's own request queue for
 * a while beforehand (meaning lead time exists but conversion just doesn't
 * happen early), or did it only enter the queue right as it was needed
 * (meaning the sector lookahead isn't reaching far enough ahead)? */
static int32_t g_model_first_seen_frame[7000];

static const struct {
  int id;
  float known_cost_ms;
} kKnownHeavySeeds[] = {
  { 5880, 308.2f },
  { 5881, 343.0f },
  { 5882, 312.8f },
  { 5879, 343.3f },
  { 5759, 374.5f },
  { 5678, 438.5f },
  { 5729, 189.5f },
  { 5816, 217.2f },
  { 5856, 120.0f },
  { 5820, 166.4f },
  { 5666, 110.0f },
  { 6038, 256.9f },
  { 5835, 90.0f },
  { 5817, 85.0f },
  { 5651, 80.0f },
  { 5781, 100.0f },
  { 5839, 46.2f },
  { 5851, 11.0f },
  { 6050, 82.5f },
  { 5740, 18.1f },
  /* CTheScripts::Process-triggered cluster (script REQUEST_MODEL calls,
   * not proximity-based world streaming) -- confirmed via phase tracking:
   * these fire from script logic early in every session regardless of
   * player position, so no position-based preload (sector lookahead, area
   * grid, or a future whole-map preload) can ever get ahead of them. Only
   * seeding them directly works. */
  { 6036, 171.9f },
  { 6037, 131.3f },
  { 6039, 107.8f },
  { 6040, 91.1f },
  { 6041, 59.8f },
  { 6042, 144.8f },
  { 6043, 31.4f },
  { 6044, 41.8f },
  { 6045, 44.3f },
  { 6091, 63.9f }
};

static bool g_heavy_pinned_models[7000] = {false};

/* Bound on how many models the REACTIVE pinning path (perform_convert_and_
 * account's 30ms auto-pin, enqueue_prewarm_candidate, issue_prewarm_request)
 * can hold pinned simultaneously, enforced via LRU eviction using the
 * last_used_tick already tracked in g_model_cost_db. Does NOT cover the
 * ~30 kKnownHeavySeeds, which stay pinned unconditionally/forever (small,
 * fixed, foundational -- see init_prewarm_system).
 *
 * Added after a real regression: scaling the area-preload grid from 3x3 to
 * 7x7 discovered dozens of additional heavy models in one pass, and since
 * reactive pinning had no cap, ALL of them became permanently un-evictable.
 * That didn't just fail to help -- raising ms_memoryAvailable from 1024MB
 * to 2000MB to compensate made it far WORSE (deadlock-breaker firings went
 * from ~10 to 1216 in one session), because more memory just let an even
 * larger permanently-pinned set accumulate, which then choked normal
 * eviction for content actually needed near the player. The real fix is
 * bounding the pinned set, not raising memory to feed its unbounded growth.
 *
 * Tried raising this 100 -> 200 after widening the CWorld sector-walk
 * lookahead's scan radius/frequency (see SECTOR_WALK_STREAM_DIST) grew
 * discovery volume ~5x (197 -> 933 candidates in a comparable session):
 * the hypothesis was that the old 100-slot cap, sized for the smaller
 * pre-widening discovery volume, was now too small to hold legitimate
 * demand. Measured result: deadlock-breaker firings got WORSE, not better
 * (212 at cap=100 -> 274 at cap=200), while PREWARMED successes barely
 * moved (44 -> 46). Reverted back to 100. This is the SAME lesson as the
 * unbounded-pin regression documented above, just at bounded scale: a
 * bigger pinned set doesn't create more physical streaming memory, it just
 * leaves less room for the engine's OWN legitimate reclaim to find
 * anything evictable, forcing MORE deadlock-breaker overrides rather than
 * fewer. The actual fix for thrashing under high discovery volume is
 * throttling how fast new candidates get promoted to REQUESTED (and thus
 * pinned) in the first place -- see PREWARM_MAX_NEW_REQUESTS_PER_CALL and
 * PREWARM_RESERVED_UNKNOWN_SLOTS_PER_CALL below -- not the cap size. */
#define MAX_REACTIVE_PINNED_MODELS 100
static int g_reactive_pinned_count = 0;

/* TXD slots get their own, much smaller pin budget instead of competing with
 * models for the 100 model slots. When TXDs were first added to the candidate
 * pool they shared this budget, and deadlock-breaker firings jumped 68 -> 182
 * in one session: a dictionary is far larger than a model, so pinning many of
 * them starves CStreaming's reclaim of anything it is actually allowed to
 * free, and the veto/deadlock path picks up the slack. Raising the shared cap
 * is known NOT to help (an earlier session tried 100 -> 200 and made it worse,
 * 212 -> 274 firings) because more pinned slots is not more physical memory --
 * it is strictly less room for the engine to work in. Partitioning is the
 * lever that shared sizing does not give: models keep the protection they
 * already had, while TXD pinning is capped hard enough that reclaim always
 * retains a large evictable working set. */
#define MAX_TXD_PINNED_SLOTS 24
static int g_txd_pinned_count = 0;
static inline bool stream_index_is_txd(int idx) { return idx >= STREAM_OFFSET_TXD; }

static void pin_model_bounded(int modelIndex) {
  if (modelIndex < 0 || modelIndex >= 7000) return;
  if (g_heavy_pinned_models[modelIndex]) return; // already pinned (seed or reactive), no-op

  /* Models and TXDs are pinned against separate budgets and evict only within
   * their own class, so a burst of dictionary pins can never displace the
   * model pins (or vice versa). See MAX_TXD_PINNED_SLOTS. */
  bool is_txd = stream_index_is_txd(modelIndex);
  int *count = is_txd ? &g_txd_pinned_count : &g_reactive_pinned_count;
  int cap = is_txd ? MAX_TXD_PINNED_SLOTS : MAX_REACTIVE_PINNED_MODELS;
  const char *cls = is_txd ? "txd" : "model";

  if (*count >= cap) {
    int lru_idx = -1;
    uint64_t lru_tick = UINT64_MAX;
    for (int i = 0; i < 7000; i++) {
      if (!g_heavy_pinned_models[i]) continue;
      if (stream_index_is_txd(i) != is_txd) continue;   // evict within class only
      if (g_model_cost_db[i].is_seeded_heavy) continue; // seeds are never evicted by this
      uint64_t last_used = g_model_cost_db[i].last_used_tick;
      if (last_used < lru_tick) {
        lru_tick = last_used;
        lru_idx = i;
      }
    }
    if (lru_idx >= 0) {
      g_heavy_pinned_models[lru_idx] = false;
      (*count)--;
      LOGC(LOGC_SYS, "[STREAM_PIN] Unpinned %s %d (LRU) to make room for %s %d (pinned=%d/%d)\n",
           cls, lru_idx, cls, modelIndex, *count, cap);
    }
    // If lru_idx stayed -1 (nothing evictable, e.g. all pinned are seeds),
    // fall through and pin anyway -- better to slightly exceed the cap than
    // to silently refuse to protect a model that just cost 30ms+.
  }

  g_heavy_pinned_models[modelIndex] = true;
  (*count)++;
}

/* Consecutive vetoed-eviction counter per model. Eviction pinning can
 * deadlock the engine: if CStreaming's own memory reclaim picks a pinned
 * model as the thing to free room for, and we unconditionally veto that
 * removal, the caller believes the memory was freed when it wasn't -- it
 * retries the same failing reclaim forever, hanging on that frame. This
 * counter lets a pin survive occasional eviction attempts (the common,
 * harmless case) while forcing the eviction through if it's clearly stuck
 * fighting the same reclaim repeatedly, so the engine can make progress. */
#define PIN_VETO_DEADLOCK_THRESHOLD 4
static uint8_t g_pin_evict_veto_streak[7000] = {0};

typedef enum {
  CANDIDATE_FREE = 0,
  CANDIDATE_QUEUED,
  CANDIDATE_REQUESTED,
  CANDIDATE_READY,
  CANDIDATE_FAILED
} CandidateStatus;

typedef struct {
  int model_index;
  CandidateStatus status;
  float historical_ms;
  float priority;
  uint32_t request_frame;
  uint32_t ready_frame;
  double prewarm_cost_ms;
  uint32_t first_gameplay_use_frame;
  double first_gameplay_use_cost_ms;
  uint32_t eviction_count;
} PrewarmCandidate;

/* Raised from 128 to 512: the candidate table never recycles slots (an
 * entry keeps its table row forever once READY/FAILED, there's no reuse),
 * so on a long drive it's a strictly-growing set. Measured hitting the old
 * 128 cap 58% into a single test session -- after that point NO new model
 * could be tracked by ANY path (sector-walk discovery or the >=30ms
 * reactive auto-enqueue), silently, with nothing logged to say so (see the
 * enqueue_prewarm_candidate table-full return value below, added at the
 * same time so this is at least visible in logs if it happens again). */
/* Raised 512 -> 1024 once TXDs became candidates in their own right: a
 * session hit the 512 cap partway through and silently dropped everything
 * discovered after that, including a 347ms dictionary logged as TABLE_FULL.
 * Raising it is only cheap because dedup is now O(1) via g_candidate_slot
 * below -- with the old linear scan this table was walked once per entity
 * per scan (~821 entities x 512 entries every 20 frames). */
#define MAX_PREWARM_CANDIDATES 1024
/* How many *new* async RequestModel calls process_gradual_prewarm_queue()
 * may issue in a single call. These were originally set to 2 to match
 * CStreaming's 2 physical streaming channels, on the assumption that our
 * RequestModel() calls compete 1:1 for those channels. In practice
 * RequestModel() just marks a model as wanted in the engine's own async
 * request pipeline (the same thing AddModelsToRequestList does) -- the
 * engine's own request list already buffers far more than 2 pending wants,
 * so this cap wasn't actually protecting channel throughput, it was just
 * throttling how fast WE hand off desire. With continuous predictive
 * discovery now feeding the queue throughout an entire drive (not just a
 * one-time ~20-model drain at boot), a cap of 2 meant newly-discovered
 * (unknown-cost, lowest-priority) candidates were being permanently
 * starved behind a never-ending stream of higher-priority reactive ones:
 * measured 108 candidates queued in one session, only 24 ever actually
 * requested. Raised so discovery can actually keep up.
 *
 * Lowered back down 4 -> 1 (with PREWARM_RESERVED_UNKNOWN_SLOTS_PER_CALL
 * 2 -> 1 below) once the *opposite* problem showed up: process_gradual_
 * prewarm_queue() runs every frame, and each newly-REQUESTED candidate gets
 * pinned (see pin_model_bounded in issue_prewarm_request) and STAYS pinned
 * until LRU-evicted, so 4+2=6 new pins/frame (up to 360/sec) after the
 * sector-walk widening (see SECTOR_WALK_STREAM_DIST) fed the queue ~5x more
 * candidates was saturating the 100-slot reactive-pin pool almost
 * immediately every session and stayed saturated (measured 99/100 or
 * 199/200 in the vast majority of samples regardless of cap size -- see
 * MAX_REACTIVE_PINNED_MODELS). Lead time from discovery to actual need is
 * on the order of seconds (lead_dist reaches up to 500 units ahead), so
 * there's no need to hand off desire at anywhere close to frame rate;
 * slowing the promotion rate lets the pin pool churn gently via its own
 * LRU instead of bursting to the cap and relying on the engine's more
 * disruptive deadlock-breaker override to make room. */
#define PREWARM_MAX_NEW_REQUESTS_PER_CALL 1
/* Cap on simultaneously CANDIDATE_REQUESTED (in-flight) prewarm entries.
 * See above -- not a physical channel limit, just a sanity bound. */
#define PREWARM_MAX_CONCURRENT_REQUESTS 8
/* Out of each call's PREWARM_MAX_NEW_REQUESTS_PER_CALL budget, how many
 * slots are reserved specifically for unknown-cost (priority == 0, never
 * measured) discovery candidates -- see the reserved-slot pass in
 * process_gradual_prewarm_queue() for why pure cost-priority ordering
 * permanently starves exactly these candidates. Lowered 2 -> 1 alongside
 * PREWARM_MAX_NEW_REQUESTS_PER_CALL above, same reason (promotion-rate
 * throttling to stop saturating the reactive-pin pool every session). */
#define PREWARM_RESERVED_UNKNOWN_SLOTS_PER_CALL 1
static PrewarmCandidate g_prewarm_candidates[MAX_PREWARM_CANDIDATES];
static size_t g_num_candidates = 0;

/* Reverse index: streaming index -> slot in g_prewarm_candidates, or -1 if
 * not tracked. Turns the enqueue dedup check from a linear scan of the whole
 * table into a single load. Safe as a plain index because the candidate table
 * is strictly append-only -- entries change status but are never removed,
 * reordered, or compacted, so a slot number stays valid for the session. */
static int16_t g_candidate_slot[NUMSTREAMINFO];
/* How many of g_prewarm_candidates[0..N) are the FIXED kKnownHeavySeeds
 * batch, set once by init_prewarm_system(). Distinct from g_num_candidates,
 * which keeps growing as reactive/discovery candidates get appended --
 * run_one_time_area_preload() needs to gate on just the fixed seed batch
 * draining, not the whole (never-empty in a live game) candidate table. */
static size_t g_num_seeded_candidates = 0;
static bool g_prewarm_system_initialized = false;

typedef struct {
  float x, y, z;
} CVector;

/* Returns CVector BY VALUE, not a pointer. Confirmed via disassembly of
 * _Z15FindPlayerCoorsv in libGame.so: it loads x/y/z into FP registers
 * S0/S1/S2 (`ldp s0,s1,[x8]; ldr s2,[x8,#8]; ret`) and never touches X0 --
 * AArch64's Homogeneous Floating-point Aggregate return convention for a
 * plain 3-float struct. The previous `const CVector *(*)(void)` typedef
 * expected the return in X0, so every call silently read whatever stale,
 * unrelated value happened to be in X0 at the call site as if it were a
 * pointer -- explains why it never crashed (that value coincidentally was
 * some other already-valid pointer) and never changed (same code path,
 * same leftover X0 every time). Declaring the return type as the struct
 * itself lets the compiler generate the correct S0-S2 read on both sides. */
typedef CVector (*Func_FindPlayerCoors)(void);
static Func_FindPlayerCoors real_FindPlayerCoors = NULL;

typedef void (*Func_AddModelsToRequestList)(const CVector *pos);
static Func_AddModelsToRequestList real_AddModelsToRequestList = NULL;

static uint8_t *g_pCStreaming_ms_aInfoForModel = NULL;

static inline uint8_t streaming_get_model_load_state(int modelIndex) {
  if (modelIndex < 0 || modelIndex >= NUMSTREAMINFO) return 0; // indexes engine memory
  if (!g_pCStreaming_ms_aInfoForModel) {
    g_pCStreaming_ms_aInfoForModel = (uint8_t *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming16ms_aInfoForModelE");
  }
  if (!g_pCStreaming_ms_aInfoForModel) return 0;
  uint8_t *entry = g_pCStreaming_ms_aInfoForModel + (modelIndex * 32);
  return entry[16]; // m_nLoadState (0 = NOT_LOADED, 1 = LOADED, 2 = IN_REQUEST_LIST, 3 = READING)
}

static inline bool streaming_model_is_ready(int modelIndex) {
  return streaming_get_model_load_state(modelIndex) == 1;
}

static void init_prewarm_system(void) {
  memset(g_model_cost_db, 0, sizeof(g_model_cost_db));
  memset(g_heavy_pinned_models, 0, sizeof(g_heavy_pinned_models));
  memset(g_model_first_seen_frame, 0xFF, sizeof(g_model_first_seen_frame)); // all-bits-1 == -1 for int32_t

  for (size_t i = 0; i < MAX_PREWARM_CANDIDATES; i++) {
    g_prewarm_candidates[i].model_index = -1;
    g_prewarm_candidates[i].status = CANDIDATE_FREE;
    g_prewarm_candidates[i].historical_ms = 0.0f;
    g_prewarm_candidates[i].priority = 0.0f;
    g_prewarm_candidates[i].request_frame = 0;
    g_prewarm_candidates[i].ready_frame = 0;
    g_prewarm_candidates[i].prewarm_cost_ms = 0.0;
    g_prewarm_candidates[i].first_gameplay_use_frame = 0;
    g_prewarm_candidates[i].first_gameplay_use_cost_ms = 0.0;
    g_prewarm_candidates[i].eviction_count = 0;
  }
  g_num_candidates = 0;
  memset(g_candidate_slot, 0xFF, sizeof(g_candidate_slot)); // all-bits-1 == -1 for int16_t

  for (size_t i = 0; i < sizeof(kKnownHeavySeeds)/sizeof(kKnownHeavySeeds[0]); i++) {
    int id = kKnownHeavySeeds[i].id;
    if (id >= 0 && id < 7000) {
      g_model_cost_db[id].max_duration_ms = kKnownHeavySeeds[i].known_cost_ms;
      g_model_cost_db[id].last_duration_ms = kKnownHeavySeeds[i].known_cost_ms;
      g_model_cost_db[id].avg_duration_ms = kKnownHeavySeeds[i].known_cost_ms;
      g_model_cost_db[id].is_seeded_heavy = true;
      g_heavy_pinned_models[id] = true;

      if (g_num_candidates < MAX_PREWARM_CANDIDATES && id < NUMSTREAMINFO) {
        g_prewarm_candidates[g_num_candidates].model_index = id;
        g_prewarm_candidates[g_num_candidates].status = CANDIDATE_QUEUED;
        g_prewarm_candidates[g_num_candidates].historical_ms = kKnownHeavySeeds[i].known_cost_ms;
        g_prewarm_candidates[g_num_candidates].priority = kKnownHeavySeeds[i].known_cost_ms;
        // Seeds must register in the reverse index too, otherwise a later
        // enqueue of the same id would not see them and would duplicate.
        g_candidate_slot[id] = (int16_t)g_num_candidates;
        g_num_candidates++;
      }
    }
  }
  g_num_seeded_candidates = g_num_candidates;
  g_prewarm_system_initialized = true;
  LOGC(LOGC_SYS, "[PREWARM] Prewarm system statically initialized (%zu seeded heavy candidates)\n", g_num_candidates);
}

/* Returns true if modelIndex ends up tracked in the candidate table (either
 * just now or already was), false only if the table is full and a genuinely
 * new model couldn't be added -- lets callers (in particular the sector-walk
 * scan's diagnostics) tell "discovered and tracked" apart from "discovered
 * but silently dropped, table full" instead of both looking identical. */
static bool enqueue_prewarm_candidate(int modelIndex, float priority) {
  /* NUMSTREAMINFO, not 7000: every candidate that lands here is eventually
   * passed to the engine's RequestModel (see issue_prewarm_request), which
   * writes to ms_aInfoForModel[index]. */
  if (modelIndex < 0 || modelIndex >= NUMSTREAMINFO) return false;
  if (!g_prewarm_system_initialized) init_prewarm_system();

  if (g_candidate_slot[modelIndex] >= 0) return true; // already tracked, O(1)

  if (g_num_candidates < MAX_PREWARM_CANDIDATES) {
    PrewarmCandidate *c = &g_prewarm_candidates[g_num_candidates];
    c->model_index = modelIndex;
    c->status = CANDIDATE_QUEUED;
    c->historical_ms = g_model_cost_db[modelIndex].max_duration_ms;
    c->priority = priority;
    c->request_frame = 0;
    c->ready_frame = 0;
    c->prewarm_cost_ms = 0.0;
    c->first_gameplay_use_frame = 0;
    c->first_gameplay_use_cost_ms = 0.0;
    c->eviction_count = 0;
    g_candidate_slot[modelIndex] = (int16_t)g_num_candidates;
    // Deliberately NOT pinned here -- a QUEUED candidate isn't resident yet,
    // so it has nothing to protect. Pinning happens once the model is
    // actually requested/resident (issue_prewarm_request, the "already
    // LOADED natively" branch below, and perform_convert_and_account).
    // Pinning speculatively at queue-time used to let the sector-walk
    // lookahead (which can enqueue ~30 candidates in a single scan) flood
    // the 100-slot reactive pin pool with things not even being streamed
    // in yet, causing real LRU/eviction thrashing (511 deadlock-breaker
    // firings in one session, concentrated on 5 models fighting for pin
    // slots) instead of the ~10-firing baseline.

    LOGC(LOGC_SYS, "[PREWARM_CANDIDATE] model=%d historical_ms=%.2f priority=%.2f (queued entry #%zu)\n",
         modelIndex, c->historical_ms, priority, g_num_candidates + 1);
    g_num_candidates++;
    return true;
  }

  static uint32_t s_last_table_full_warn_frame = 0;
  if (g_frame_count - s_last_table_full_warn_frame >= 300) {
    s_last_table_full_warn_frame = g_frame_count;
    LOGC(LOGC_SYS, "[PREWARM_CANDIDATE] TABLE FULL (%d/%d) -- model=%d could NOT be tracked (frame #%u)\n",
         MAX_PREWARM_CANDIDATES, MAX_PREWARM_CANDIDATES, modelIndex, g_frame_count);
  }
  return false;
}

/* Adaptive streaming model allowance based on backlog queue */
static inline int get_streaming_model_allowance(void) {
  uintptr_t num_req_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming21ms_numModelsRequestedE");
  if (num_req_addr) {
    int num_req = *(int *)num_req_addr;
    if (num_req >= 25) {
      return 2; // Catch-up allowance during high streaming backlog
    }
  }
  return 1;
}

/* Admission control (pre-conversion check) */
static inline bool streaming_budget_allows_next_work(void) {
  if (!gameplay_streaming_enabled()) return true;
  if (get_streaming_context() != STREAM_GAMEPLAY) return true;
  uint64_t cpu_ns = atomic_load_explicit(&g_streaming_frame_cpu_ns, memory_order_relaxed);
  int models = atomic_load_explicit(&g_models_loaded_this_frame, memory_order_relaxed);
  return ((double)cpu_ns / 1e6 < (double)config.streaming_budget_ms) &&
         (models < get_streaming_model_allowance());
}

/* Reverse-engineered CStreamingChannel structure (48 bytes) */
typedef struct {
  int m_nModelIndex[3]; // 0x00, 0x04, 0x08
  int m_nOffset[3];     // 0x0C, 0x10, 0x14 (offset in 2048-byte CD sectors)
  int m_nState;         // 0x18 (0=IDLE, 1=READING/READY, 2=LARGE, 3=ERROR)
  int m_nStreamHandle;  // 0x1C
  uint8_t m_pad[16];    // 48 bytes total
} CStreamingChannel;

static CStreamingChannel *g_pCStreaming_ms_channel = NULL;
static char **g_pCStreaming_ms_pStreamingBuffer = NULL;
static int *g_pCStreaming_ms_channelError = NULL;
static bool *g_pCStreaming_ms_bLoadingBigModel = NULL;

typedef int (*Func_CdStreamGetStatus)(int stream);
static Func_CdStreamGetStatus real_CdStreamGetStatus = NULL;

typedef void (*Func_FinishLoadingLargeFile)(char *buf, int modelIndex);
static Func_FinishLoadingLargeFile orig_FinishLoadingLargeFile = NULL;

typedef void (*Func_ProcessLoadingChannel)(int channel);
static Func_ProcessLoadingChannel orig_ProcessLoadingChannel = NULL;

static void wrap_ProcessLoadingChannel(int channel);

static _Thread_local int g_load_scene_depth = 0;

/* Bulk load wrapper to track loading-screen / script requests (Pure observational passthrough) */
typedef void (*Func_LoadAllRequestedModels)(bool bPriorityOnly);
static Func_LoadAllRequestedModels orig_LoadAllRequestedModels = NULL;

static void wrap_LoadAllRequestedModels(bool bPriorityOnly) {
  void *caller = __builtin_return_address(0);
  StreamingContext ctx = get_streaming_context();
  StreamingRuntimeMode mode = get_stream_runtime_mode();
  bool gp_enabled = gameplay_streaming_enabled();

  LOGC(LOGC_SYS, "[STREAM_CALL] LoadAllRequestedModels context=%s mode=%s gameplay_enabled=%d depth=%d priority=%d caller=%p action=ORIGINAL\n",
       streaming_context_to_string(ctx), stream_runtime_mode_to_string(mode), gp_enabled ? 1 : 0, g_load_scene_depth, bPriorityOnly, caller);
  STREAM_SCOPE_ENTER("LoadAllRequestedModels", ", priority=%d, ctx=%d, depth=%d", bPriorityOnly, ctx, g_load_scene_depth);
  if (orig_LoadAllRequestedModels) orig_LoadAllRequestedModels(bPriorityOnly);
  STREAM_SCOPE_EXIT("LoadAllRequestedModels");
}

/* Queue dispatcher throttling: CStreaming::LoadRequestedModels (Pure observational passthrough) */
typedef void (*Func_LoadRequestedModels)(void);
static Func_LoadRequestedModels orig_LoadRequestedModels = NULL;

static void wrap_LoadRequestedModels(void) {
  STREAM_SCOPE_ENTER("LoadRequestedModels", "");
  if (orig_LoadRequestedModels) orig_LoadRequestedModels();
  STREAM_SCOPE_EXIT("LoadRequestedModels");
}

/* Channel stream request throttling: CStreaming::RequestModelStream (Pure observational passthrough) */
typedef bool (*Func_RequestModelStream)(int channel);
static Func_RequestModelStream orig_RequestModelStream = NULL;

static bool wrap_RequestModelStream(int channel) {
  STREAM_SCOPE_ENTER("RequestModelStream", ", ch=%d", channel);
  bool res = orig_RequestModelStream ? orig_RequestModelStream(channel) : false;
  if (channel >= 0 && channel < 2 && g_pCStreaming_ms_channel) {
    CStreamingChannel *ch = &g_pCStreaming_ms_channel[channel];
    for (int i = 0; i < 3; i++) {
      int m = ch->m_nModelIndex[i];
      if (m < 0 || m >= 7000) continue;
      if (g_model_first_seen_frame[m] < 0) {
        g_model_first_seen_frame[m] = (int32_t)g_frame_count;
      }
      if (g_model_cost_db[m].max_duration_ms >= 30.0f) {
        enqueue_prewarm_candidate(m, g_model_cost_db[m].max_duration_ms);
      }
    }
  }
  STREAM_SCOPE_EXIT("RequestModelStream");
  return res;
}

/* Sub-loader timing hooks for granular ConvertBufferToObject profiling */
typedef bool (*Func_LoadAtomicFile)(void *stream, unsigned int modelIndex);
static Func_LoadAtomicFile orig_LoadAtomicFile = NULL;

typedef bool (*Func_LoadClumpFile)(void *stream, unsigned int modelIndex);
static Func_LoadClumpFile orig_LoadClumpFile = NULL;

typedef bool (*Func_LoadTxd)(int txdIndex, void *stream);
static Func_LoadTxd orig_LoadTxd = NULL;

static _Atomic uint64_t g_last_atomic_ns = 0;
static _Atomic uint64_t g_last_clump_ns = 0;
static _Atomic uint64_t g_last_txd_ns = 0;
static _Atomic int g_last_txd_index = -1;

static bool wrap_LoadAtomicFile(void *stream, unsigned int modelIndex) {
  uint64_t t0 = armGetSystemTick();
  bool res = orig_LoadAtomicFile ? orig_LoadAtomicFile(stream, modelIndex) : false;
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  atomic_fetch_add_explicit(&g_last_atomic_ns, dt_ns, memory_order_relaxed);
  return res;
}

static bool wrap_LoadClumpFile(void *stream, unsigned int modelIndex) {
  uint64_t t0 = armGetSystemTick();
  bool res = orig_LoadClumpFile ? orig_LoadClumpFile(stream, modelIndex) : false;
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  atomic_fetch_add_explicit(&g_last_clump_ns, dt_ns, memory_order_relaxed);
  return res;
}

static bool wrap_LoadTxd(int txdIndex, void *stream) {
  atomic_store_explicit(&g_last_txd_index, txdIndex, memory_order_relaxed);
  uint64_t t0 = armGetSystemTick();
  bool res = orig_LoadTxd ? orig_LoadTxd(txdIndex, stream) : false;
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  atomic_fetch_add_explicit(&g_last_txd_ns, dt_ns, memory_order_relaxed);
  return res;
}

/* Observation-only timing hook (no behavior change): confirms whether the
 * TXD-load cost measured above is actually spent inside the original
 * binary's software DXT-to-uncompressed decode path. Symbol found via
 * `nm -D libGame.so`: dxtSwizzler_CreateUncompressedTexture(unsigned int,
 * unsigned int, unsigned int, void const*, unsigned int&). Present as a
 * separate, distinctly-named routine from the stock RenderWare OpenGL
 * native-texture-read path -- almost certainly added to bridge the gap
 * between the DXT-recompressed asset archives this port ships (GTA3_DXT.*)
 * and the original Android build's texture plugin, which never expected
 * DXT input and has no path to hand it straight to glCompressedTexImage2D. */
typedef void (*Func_DxtSwizzlerCreateUncompressedTexture)(unsigned int width, unsigned int height, unsigned int arg2, const void *srcData, unsigned int *outArg);
static Func_DxtSwizzlerCreateUncompressedTexture orig_DxtSwizzlerCreateUncompressedTexture = NULL;

static void wrap_DxtSwizzlerCreateUncompressedTexture(unsigned int width, unsigned int height, unsigned int arg2, const void *srcData, unsigned int *outArg) {
  uint64_t t0 = armGetSystemTick();
  if (orig_DxtSwizzlerCreateUncompressedTexture) orig_DxtSwizzlerCreateUncompressedTexture(width, height, arg2, srcData, outArg);
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  if (ms >= 2.0) {
    LOGC(LOGC_SYS, "[PERF_DXT_SWIZZLE] dxtSwizzler_CreateUncompressedTexture(%ux%u, arg2=%u) took %.2fms (txd=%d)\n",
         width, height, arg2, ms, atomic_load_explicit(&g_last_txd_index, memory_order_relaxed));
  }
}

/* The remaining unmeasured leg of the texture pipeline.
 *
 * _rwOpenGLNativeTextureRead averages 8.27ms per texture over 4314 calls
 * (35.7s a session -- larger than every stall source tracked so far), and
 * disassembly shows where that time can actually be going. It calls
 * ReadTextureDataRaster (0x306458), which per call issues exactly one
 * emu_glCompressedTexImage2D, two emu_glTexImage2D, two emu_glTexParameteri,
 * one emu_glBindTexture and one RwRasterCreate.
 *
 * glTexImage2D is already timed and is NOT the cost (31 calls over 2ms in a
 * whole session, 119ms total). The CPU DXT decode is already ruled out: the
 * dxtSwizzler hook above still reports 0 hits, so emu_glCompressedTexImage2D
 * is taking its hardware path (verified by disassembling it at 0x140790 --
 * DXT formats go to the real glCompressedTexImage2D whenever the three
 * capability flags at 0x3dd481..483 are set, and only CPU-decode otherwise).
 *
 * That leaves the compressed upload itself and the raster allocation as the
 * only unmeasured candidates -- a driver doing CPU-side tiling/swizzling of
 * DXT blocks into the GPU layout would look exactly like this. Instrumenting
 * all three at once rather than one per test cycle, same rationale as the
 * batch below. Pure passthrough timers; no behaviour change. */
typedef void (*Func_EmuGlCompressedTexImage2D)(unsigned int target, int level, unsigned int internalformat,
                                               int width, int height, int border, int imageSize, const void *data);
static Func_EmuGlCompressedTexImage2D orig_EmuGlCompressedTexImage2D = NULL;
static void wrap_EmuGlCompressedTexImage2D(unsigned int target, int level, unsigned int internalformat,
                                           int width, int height, int border, int imageSize, const void *data) {
  uint64_t t0 = armGetSystemTick();
  if (orig_EmuGlCompressedTexImage2D)
    orig_EmuGlCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  atomic_fetch_add_explicit(&g_txpipe_compressed_ns, (uint64_t)(ms * 1e6), memory_order_relaxed);
  atomic_fetch_add_explicit(&g_txpipe_compressed_calls, 1, memory_order_relaxed);
  if (ms >= 2.0)
    LOGC(LOGC_SYS, "[PERF_TXPIPE] glCompressedTexImage2D(%dx%d, internal=0x%x, level=%d, bytes=%d) took %.2fms\n",
         width, height, internalformat, level, imageSize, ms);
}

typedef void *(*Func_RwRasterCreate)(int width, int height, int depth, int flags);
static Func_RwRasterCreate orig_RwRasterCreate = NULL;
static void *wrap_RwRasterCreate(int width, int height, int depth, int flags) {
  uint64_t t0 = armGetSystemTick();
  void *res = orig_RwRasterCreate ? orig_RwRasterCreate(width, height, depth, flags) : NULL;
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  atomic_fetch_add_explicit(&g_txpipe_raster_ns, (uint64_t)(ms * 1e6), memory_order_relaxed);
  atomic_fetch_add_explicit(&g_txpipe_raster_calls, 1, memory_order_relaxed);
  if (ms >= 2.0)
    LOGC(LOGC_SYS, "[PERF_TXPIPE] RwRasterCreate(%dx%d, depth=%d, flags=0x%x) took %.2fms\n",
         width, height, depth, flags, ms);
  return res;
}

/* dxtSwizzler_CreateUncompressedTexture confirmed NOT on the hot path (0
 * hits across a full driving session while TXD: costs stayed 30-500ms+).
 * Instrumenting the rest of the RenderWare texture pipeline at once so the
 * next run localizes the real cost in a single pass instead of another
 * guess-and-retest round trip. All pure passthrough timing wrappers --
 * generic void* signatures are ABI-safe here since AArch64 returns any
 * pointer/int/bool-sized value in X0 regardless of the true C++ type, and
 * we never touch the value, only forward it. */
typedef void *(*Func_RwTexDictionaryGtaStreamRead)(void *stream);
static Func_RwTexDictionaryGtaStreamRead orig_RwTexDictionaryGtaStreamRead = NULL;
static void *wrap_RwTexDictionaryGtaStreamRead(void *stream) {
  uint64_t t0 = armGetSystemTick();
  void *res = orig_RwTexDictionaryGtaStreamRead ? orig_RwTexDictionaryGtaStreamRead(stream) : NULL;
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  if (ms >= 2.0) LOGC(LOGC_SYS, "[PERF_TXPIPE] RwTexDictionaryGtaStreamRead took %.2fms\n", ms);
  return res;
}

typedef void *(*Func_RwTextureGtaStreamRead)(void *stream);
static Func_RwTextureGtaStreamRead orig_RwTextureGtaStreamRead = NULL;
static void *wrap_RwTextureGtaStreamRead(void *stream) {
  uint64_t t0 = armGetSystemTick();
  void *res = orig_RwTextureGtaStreamRead ? orig_RwTextureGtaStreamRead(stream) : NULL;
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  if (ms >= 2.0) LOGC(LOGC_SYS, "[PERF_TXPIPE] RwTextureGtaStreamRead took %.2fms\n", ms);
  return res;
}

typedef void *(*Func_rwOpenGLNativeTextureRead)(void *a, void *b, int c);
static Func_rwOpenGLNativeTextureRead orig_rwOpenGLNativeTextureRead = NULL;
static void *wrap_rwOpenGLNativeTextureRead(void *a, void *b, int c) {
  uint64_t t0 = armGetSystemTick();
  void *res = orig_rwOpenGLNativeTextureRead ? orig_rwOpenGLNativeTextureRead(a, b, c) : NULL;
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  atomic_fetch_add_explicit(&g_txpipe_texread_ns, (uint64_t)(ms * 1e6), memory_order_relaxed);
  atomic_fetch_add_explicit(&g_txpipe_texread_calls, 1, memory_order_relaxed);
  if (ms >= 2.0) LOGC(LOGC_SYS, "[PERF_TXPIPE] _rwOpenGLNativeTextureRead took %.2fms\n", ms);
  return res;
}

/* CreateTextureData is NOT hooked: its prologue is only 3 instructions
 * (12 bytes) before an early RET, but this port's trampoline mechanism
 * always patches a fixed 16-byte/4-instruction window at the hook site.
 * Hooking it overwrote the start of the next function in memory with
 * garbage, causing an Undefined Instruction crash at startup. Confirmed via
 * the trampoline's saved-instruction dump: insns 0xa9007d1f 0xf900091f
 * 0xd65f03c0(RET) 0xd10243ff -- the 4th word isn't part of this function at
 * all. Do not re-add this hook without a trampoline mechanism that checks
 * for an early return/branch within its relocation window. */

typedef void *(*Func_RwTextureRasterGenerateMipmaps)(void *raster, void *image);
static Func_RwTextureRasterGenerateMipmaps orig_RwTextureRasterGenerateMipmaps = NULL;
static void *wrap_RwTextureRasterGenerateMipmaps(void *raster, void *image) {
  uint64_t t0 = armGetSystemTick();
  void *res = orig_RwTextureRasterGenerateMipmaps ? orig_RwTextureRasterGenerateMipmaps(raster, image) : NULL;
  double ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  if (ms >= 2.0) LOGC(LOGC_SYS, "[PERF_TXPIPE] RwTextureRasterGenerateMipmaps took %.2fms\n", ms);
  return res;
}

/* Eviction hooks */
typedef void (*Func_RemoveModel)(int modelIndex);
static Func_RemoveModel orig_RemoveModel = NULL;

static void wrap_RemoveModel(int modelIndex) {
  bool is_pinned = (modelIndex >= 0 && modelIndex < 7000) && g_heavy_pinned_models[modelIndex];

  const char *prewarm_status_str = "NOT_TARGET";
  for (size_t i = 0; i < g_num_candidates; i++) {
    if (g_prewarm_candidates[i].model_index == modelIndex && g_prewarm_candidates[i].model_index != -1) {
      switch (g_prewarm_candidates[i].status) {
        case CANDIDATE_QUEUED: prewarm_status_str = "QUEUED"; break;
        case CANDIDATE_REQUESTED: prewarm_status_str = "REQUESTED"; break;
        case CANDIDATE_READY: prewarm_status_str = "READY"; break;
        case CANDIDATE_FAILED: prewarm_status_str = "FAILED"; break;
        default: prewarm_status_str = "FREE"; break;
      }
      g_prewarm_candidates[i].eviction_count++;
      break;
    }
  }

  if (is_pinned) {
    uint8_t streak = ++g_pin_evict_veto_streak[modelIndex];
    if (streak <= PIN_VETO_DEADLOCK_THRESHOLD) {
      LOGC(LOGC_SYS, "[STREAM_PIN] Prevented eviction of heavy model %d (PIN=1, prewarm=%s, reason=RemoveModel, streak=%u)\n",
           modelIndex, prewarm_status_str, streak);
      return;
    }
    // The engine has retried reclaiming this exact model PIN_VETO_DEADLOCK_THRESHOLD+
    // times in a row -- vetoing again would keep it spinning on the same failed
    // reclaim indefinitely. Let this one eviction through; the pin flag stays set,
    // so it's still eligible for re-pinning/re-prewarm once it's reloaded.
    g_pin_evict_veto_streak[modelIndex] = 0;
    LOGC(LOGC_SYS, "[STREAM_PIN] BREAKING deadlock: forcing eviction of heavy model %d after %u consecutive vetoed attempts (prewarm=%s, reason=RemoveModel)\n",
         modelIndex, streak, prewarm_status_str);
  }

  if (modelIndex >= 0 && modelIndex < 7000) {
    ModelCostEntry *ce = &g_model_cost_db[modelIndex];
    ce->evict_count++;
    if (ce->load_count > 0) {
      LOGC(LOGC_SYS, "[STREAM_EVICT] Model %d evicted (PIN=0, prewarm=%s, loads=%u, reloads=%u, evictions=%u, txd=%d)\n",
           modelIndex, prewarm_status_str, ce->load_count, ce->reload_count,
           ce->evict_count, ce->last_txd_index);
    }
  }
  if (orig_RemoveModel) orig_RemoveModel(modelIndex);
}

typedef void (*Func_SetModelTxdIsDeletable)(int modelIndex);
static Func_SetModelTxdIsDeletable orig_SetModelTxdIsDeletable = NULL;

static void wrap_SetModelTxdIsDeletable(int modelIndex) {
  if (modelIndex >= 0 && modelIndex < 7000 && g_heavy_pinned_models[modelIndex]) {
    LOGC(LOGC_SYS, "[STREAM_PIN] TXD remains non-deletable for heavy model %d (PIN=1)\n", modelIndex);
    return;
  }
  if (orig_SetModelTxdIsDeletable) orig_SetModelTxdIsDeletable(modelIndex);
}

typedef void (*Func_RemoveNonReferencedTxds)(int numTxds);
static Func_RemoveNonReferencedTxds orig_RemoveNonReferencedTxds = NULL;

static void wrap_RemoveNonReferencedTxds(int numTxds) {
  if (orig_RemoveNonReferencedTxds) orig_RemoveNonReferencedTxds(numTxds);
}

typedef bool (*Func_RemoveLeastUsedModel)(void);
static Func_RemoveLeastUsedModel orig_RemoveLeastUsedModel = NULL;

static bool wrap_RemoveLeastUsedModel(void) {
  return orig_RemoveLeastUsedModel ? orig_RemoveLeastUsedModel() : false;
}

/* Diagnostic: why did the sector-walk lookahead (defined further below,
 * around sector_walk_prewarm_scan) NOT enqueue a given model the last time
 * it was encountered? Declared up here (ahead of perform_convert_and_
 * account, which reads it for the PREWARM_LATE log line below) after
 * discovering that every PREWARM_LATE conversion in a test session (up to
 * 811ms!) had a 0-frame gap between "first ever appearance in the candidate
 * table" and "actually needed" -- meaning these weren't found late, they
 * were never found by any proactive mechanism at all. This records, per
 * model, the reason the LAST sector-walk encounter didn't result in an
 * enqueue, to distinguish "the scan genuinely never walks past this entity"
 * (spatial coverage gap -- SCAN_SKIP_NONE, never touched) from "the scan
 * sees it every time but a filter rejects it" (e.g. it's flagged
 * bStreamingDontDelete, which both the real engine and our replica skip by
 * design). */
typedef enum {
  SCAN_SKIP_NONE = 0,
  SCAN_SKIP_DONTDELETE_OR_SUBWAY,
  SCAN_SKIP_TEMP_OBJECT,
  SCAN_SKIP_NO_MODELINFO,
  SCAN_SKIP_TIME_GATED,
  SCAN_SKIP_OUT_OF_BOUNDS,
  SCAN_SKIP_LOD_DISTANCE,
  SCAN_SKIP_KNOWN_CHEAP,
  SCAN_SKIP_TABLE_FULL,
  SCAN_SKIP_ENQUEUED
} ScanSkipReason;
static uint8_t g_model_last_scan_reason[7000] = {0};

static const char *scan_skip_reason_to_string(uint8_t reason) {
  switch (reason) {
    case SCAN_SKIP_NONE: return "NEVER_SCANNED";
    case SCAN_SKIP_DONTDELETE_OR_SUBWAY: return "DONTDELETE_OR_SUBWAY";
    case SCAN_SKIP_TEMP_OBJECT: return "TEMP_OBJECT";
    case SCAN_SKIP_NO_MODELINFO: return "NO_MODELINFO";
    case SCAN_SKIP_TIME_GATED: return "TIME_GATED";
    case SCAN_SKIP_OUT_OF_BOUNDS: return "OUT_OF_BOUNDS";
    case SCAN_SKIP_LOD_DISTANCE: return "LOD_DISTANCE";
    case SCAN_SKIP_KNOWN_CHEAP: return "KNOWN_CHEAP";
    case SCAN_SKIP_TABLE_FULL: return "TABLE_FULL";
    case SCAN_SKIP_ENQUEUED: return "ENQUEUED";
    default: return "?";
  }
}

/* Forward declaration: full CModelInfo::ms_modelInfoPtrs resolution lives
 * with the rest of the sector-walk symbol lookups further below (see
 * ensure_sector_walk_symbols), but perform_convert_and_account also wants
 * to read a model's type byte directly (independent of any entity/sector
 * lookup) for its STREAM_CONVERT diagnostic line -- e.g. to tell whether a
 * NEVER_SCANNED model is a ped/vehicle (never placed as a static CEntity in
 * CWorld's sector or big-building lists at all, so no entity-list walk
 * could ever find it) versus a building-type model that should have been
 * reachable but wasn't. */
static void **g_pCModelInfo_ms_modelInfoPtrs = NULL;
#define CMODELINFO_OFF_TYPEBYTE 54 // CBaseModelInfo model-type byte (MITYPE_* from re3)
static const char *modelinfo_type_to_string(uint8_t t) {
  switch (t) {
    case 0: return "NA";
    case 1: return "SIMPLE";
    case 2: return "MLO";
    case 3: return "TIME";
    case 4: return "CLUMP";
    case 5: return "VEHICLE";
    case 6: return "PED";
    case 7: return "XTRACOMPS";
    default: return "?";
  }
}

/* Single unified conversion & accounting function */
typedef bool (*Func_ConvertBufferToObject)(char *buffer, int modelIndex);
static Func_ConvertBufferToObject orig_ConvertBufferToObject = NULL;

static bool perform_convert_and_account(char *buffer, int modelIndex) {
  STREAM_SCOPE_ENTER("ConvertBufferToObject", ", model=%d", modelIndex);
  atomic_store_explicit(&g_last_atomic_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&g_last_clump_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&g_last_txd_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&g_last_txd_index, -1, memory_order_relaxed);

  uint64_t t0 = armGetSystemTick();
  bool res = orig_ConvertBufferToObject ? orig_ConvertBufferToObject(buffer, modelIndex) : false;
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  atomic_fetch_add_explicit(&g_streaming_frame_cpu_ns, dt_ns, memory_order_relaxed);
  int count = atomic_fetch_add_explicit(&g_models_loaded_this_frame, 1, memory_order_relaxed) + 1;
  double ms = (double)dt_ns / 1e6;

  /* Feed the overlay's per-frame spike attribution (see
   * gta3_take_frame_stream_stats). */
  atomic_fetch_add_explicit(&g_overlay_convert_ns, dt_ns, memory_order_relaxed);
  if (dt_ns > atomic_load_explicit(&g_overlay_worst_ns, memory_order_relaxed)) {
    atomic_store_explicit(&g_overlay_worst_ns, dt_ns, memory_order_relaxed);
    atomic_store_explicit(&g_overlay_worst_index, modelIndex, memory_order_relaxed);
  }

  // Model is resident again: clear its consecutive-veto streak so an
  // occasional, well-spaced eviction attempt against a healthy pin doesn't
  // creep toward the deadlock-breaker threshold.
  if (modelIndex >= 0 && modelIndex < 7000) {
    g_pin_evict_veto_streak[modelIndex] = 0;
  }

  int txd_idx = atomic_load_explicit(&g_last_txd_index, memory_order_relaxed);
  double txd_ms = (double)atomic_load_explicit(&g_last_txd_ns, memory_order_relaxed) / 1e6;

  // Track max model and TXD times for frame telemetry
  if (ms > atomic_load_explicit(&g_frame_max_model_ms, memory_order_relaxed)) {
    atomic_store_explicit(&g_frame_max_model_ms, ms, memory_order_relaxed);
    atomic_store_explicit(&g_frame_max_model_id, modelIndex, memory_order_relaxed);
  }
  if (txd_ms > atomic_load_explicit(&g_frame_max_txd_ms, memory_order_relaxed)) {
    atomic_store_explicit(&g_frame_max_txd_ms, txd_ms, memory_order_relaxed);
    atomic_store_explicit(&g_frame_max_txd_id, txd_idx, memory_order_relaxed);
  }

  // Check for single operation budget overshoot
  if (ms > (double)config.streaming_budget_ms) {
    atomic_store_explicit(&g_frame_overshoot_reason, OVERSHOOT_MODEL_SINGLE_OP, memory_order_relaxed);
  }

  if (modelIndex >= 0 && modelIndex < 7000) {
    ModelCostEntry *ce = &g_model_cost_db[modelIndex];
    ce->load_count++;
    ce->last_duration_ms = (float)ms;
    ce->last_used_tick = t0;
    if (txd_idx >= 0) ce->last_txd_index = txd_idx;
    if (ms > ce->max_duration_ms) ce->max_duration_ms = (float)ms;
    if (ce->avg_duration_ms <= 0.001f) {
      ce->avg_duration_ms = (float)ms;
    } else {
      ce->avg_duration_ms = (ce->avg_duration_ms * 0.7f) + ((float)ms * 0.3f);
    }
    if (ms >= 30.0) {
      ce->expensive_count++;
      pin_model_bounded(modelIndex); // Automatically pin any model demonstrated to take >= 30ms! (bounded, LRU)
      enqueue_prewarm_candidate(modelIndex, (float)ms);
    }
    if (ce->load_count > 1) {
      ce->reload_count++;
      LOGC(LOGC_SYS, "[STREAM_RELOAD] Model %d reloaded (reload_count=%u, evict_count=%u, cost=%.2fms, max=%.2fms, txd=%d)\n",
           modelIndex, ce->reload_count, ce->evict_count, ms, ce->max_duration_ms, ce->last_txd_index);
    }
  }

  if (txd_idx >= 0 && txd_idx < 2048) {
    TxdResidency *tr = &g_txd_residency[txd_idx];
    tr->txd_index = txd_idx;
    tr->load_count++;
    tr->last_used_tick = t0;
    if (tr->load_count > 1) {
      tr->reload_count++;
    }
  }

  // Prewarm candidate lifecycle tracking
  PrewarmCandidate *matched_candidate = NULL;
  for (size_t i = 0; i < g_num_candidates; i++) {
    if (g_prewarm_candidates[i].model_index == modelIndex && g_prewarm_candidates[i].model_index != -1) {
      matched_candidate = &g_prewarm_candidates[i];
      break;
    }
  }

  const char *prewarm_status_str = "PREWARM_STATUS=NOT_TARGET";
  if (matched_candidate) {
    if (matched_candidate->status == CANDIDATE_REQUESTED) {
      matched_candidate->status = CANDIDATE_READY;
      matched_candidate->ready_frame = g_frame_count;
      matched_candidate->prewarm_cost_ms = ms;
      pin_model_bounded(modelIndex);
      prewarm_status_str = "PREWARM_STATUS=PREWARMED";
      LOGC(LOGC_SYS, "[PREWARM_READY] model=%d frame=%u request_frame=%u cost=%.2fms [PINNED]\n",
           modelIndex, g_frame_count, matched_candidate->request_frame, ms);
    } else if (matched_candidate->status == CANDIDATE_READY) {
      if (matched_candidate->first_gameplay_use_frame == 0) {
        matched_candidate->first_gameplay_use_frame = g_frame_count;
        matched_candidate->first_gameplay_use_cost_ms = ms;
        prewarm_status_str = "PREWARM_STATUS=PREWARMED_FIRST_USE";
        LOGC(LOGC_SYS, "[PREWARM_USE] model=%d first_visibility_frame=%u first_visibility_cost=%.2fms (ready_frame=%u)\n",
             modelIndex, g_frame_count, ms, matched_candidate->ready_frame);
      } else {
        prewarm_status_str = "PREWARM_STATUS=PREWARMED_REUSED";
      }
    } else if (matched_candidate->status == CANDIDATE_QUEUED) {
      prewarm_status_str = "PREWARM_STATUS=PREWARM_LATE";
    } else if (matched_candidate->status == CANDIDATE_FAILED) {
      prewarm_status_str = "PREWARM_STATUS=PREWARM_FAILED";
    }
  }

  if (ms >= 1.0) {
    double atomic_ms = (double)atomic_load_explicit(&g_last_atomic_ns, memory_order_relaxed) / 1e6;
    double clump_ms = (double)atomic_load_explicit(&g_last_clump_ns, memory_order_relaxed) / 1e6;
    double rw_other_ms = ms - (atomic_ms + clump_ms + txd_ms);
    if (rw_other_ms < 0) rw_other_ms = 0;

    const char *cost_cat = "NORMAL";
    if (ms >= 200.0) cost_cat = "CATASTROPHIC";
    else if (ms >= 100.0) cost_cat = "SEVERE";
    else if (ms >= 30.0) cost_cat = "EXPENSIVE";

    int32_t first_seen = (modelIndex >= 0 && modelIndex < 7000) ? g_model_first_seen_frame[modelIndex] : -1;
    int lead_frames = (first_seen >= 0) ? (int)(g_frame_count - (uint32_t)first_seen) : -1;
    const char *scan_reason = (modelIndex >= 0 && modelIndex < 7000) ?
        scan_skip_reason_to_string(g_model_last_scan_reason[modelIndex]) : "N/A";
    const char *mi_type = "N/A";
    if (modelIndex >= MODELINFO_COUNT) {
      mi_type = "TXD"; // not a model at all -- a texture-dictionary stream slot
    } else if (g_pCModelInfo_ms_modelInfoPtrs && modelIndex >= 0) {
      void *mi = g_pCModelInfo_ms_modelInfoPtrs[modelIndex];
      if (mi) mi_type = modelinfo_type_to_string(*((uint8_t *)mi + CMODELINFO_OFF_TYPEBYTE));
    }

    LOGC(LOGC_SYS, "[STREAM_CONVERT] Model %d instantiated in %.2fms (#%d this frame) [CAT=%s] [%s] [Atomic: %.2fms, Clump: %.2fms, TXD: %.2fms (txd=%d), Other: %.2fms] [engine_lead_frames=%d] [sector_walk=%s] [mi_type=%s]\n",
         modelIndex, ms, count, cost_cat, prewarm_status_str, atomic_ms, clump_ms, txd_ms, txd_idx, rw_other_ms, lead_frames, scan_reason, mi_type);
  }
  STREAM_SCOPE_EXIT("ConvertBufferToObject");
  return res;
}

/* ---------------- Switch controller input ----------------
 *
 * Nothing here any more, and that is the point.
 *
 * This used to poke CPad::NewState directly from a CPad::UpdatePads hook and
 * force seven CPad::MenuInput* predicates true, because the pad appeared dead:
 * CapturePad only fills NewState from Touchscreen::SetupJoystate, and
 * ToggleMenuJustDown ignores NewState entirely.
 *
 * That was the wrong layer. The Android build has a complete gamepad stack of
 * its own -- CHID / CHIDJoystickXbox360, fed by the GameNative gamepad entry
 * points -- and CPad's getters consult it directly (CHID::IsPressed has 33
 * callers, IsJustPressed 13, Implements 34, all of them CPad accessors). The
 * only reason none of it ran is that CHID::CheckForInputChange (0x149198)
 * refuses to construct a joystick instance until OS_GamepadIsConnected(0) is
 * true, and the port was calling implOnGamepadConnected -- a symbol that
 * exists in San Andreas but not in this binary -- so the connected flag was
 * never set and CHID::GetInputType() stayed 0 (touchscreen) forever.
 *
 * main.c now reports the pad through implOnGamepadCountChanged, so the engine
 * builds its own CHIDJoystickXbox360 on first input and every button works
 * everywhere by the game's own routing, including the frontend and the icons.
 * See the table in main.c for the button numbering and the icon consequences. */

/* ---- splash prompt ----------------------------------------------------------
 *
 * CMenuManager::DrawFrontEndNormal picks the splash caption straight off the
 * input type (0x200ca8):
 *     CText::Get(CHID::GetInputType() == 0 ? "SPLASH" : "SPLASH3")
 * with SPLASH = "Tap To Continue" and SPLASH3 = "Press ::MENUOK:: To Continue."
 * Both are replaced, so the caption never changes as you switch device.
 * Touch never stops working -- the engine only swaps which affordance it names --
 * so the prompt goes from telling you the truth to telling you half of it the
 * moment you touch the pad.
 *
 * Overriding the lookup rather than editing the .gxt keeps the change in our
 * source: the assets are the user's own extracted game data, a GXT edit would
 * have to be repeated for all eight languages, and it would be silently undone
 * by a re-extract. The cost is that this one string is now English regardless of
 * the chosen language; every other string still comes from the GXT.
 *
 * ::MENUOK:: survives because the token is expanded later, by CFont::PrintString
 * -> CHID::QueueHIDHelpIcon, which is what draws the A glyph from the button
 * atlas. Our replacement carries the same token and so keeps the same icon. */
typedef uint16_t *(*Func_CTextGet)(void *self, const char *key);
static Func_CTextGet orig_CTextGet = NULL;

static uint16_t g_splash_prompt[64];

static uint16_t *wrap_CTextGet(void *self, const char *key) {
  /* Both keys, not just the pad one: the engine starts on SPLASH ("Tap To
   * Continue") and only swaps to SPLASH3 once the input type changes, so
   * overriding SPLASH3 alone still left the first screen naming touch only. */
  if (key && key[0] == 'S' &&
      (strcmp(key, "SPLASH") == 0 || strcmp(key, "SPLASH3") == 0) && g_splash_prompt[0])
    return g_splash_prompt;
  return orig_CTextGet ? orig_CTextGet(self, key) : NULL;
}

/* ---- menu auto-repeat -------------------------------------------------------
 *
 * These four predicates are the frontend's only navigation input
 * (CMenuManager::ProcessButtonPresses calls each exactly once per tick), which
 * is what makes hooking them the right scope: the repeat can never leak into
 * gameplay, where the same d-pad and stick mean other things entirely -- the
 * Xbox360 map also binds d-pad up/down to mappings 11 and 14, and the left stick
 * to steering and movement.
 *
 * The original is always consulted first and its answer is never suppressed, so
 * the initial press, the d-pad and touch all behave exactly as before; we only
 * add the held-down repeat that the engine has no notion of. main.c owns the
 * timing. */
extern volatile int g_menu_repeat_up, g_menu_repeat_down;
extern volatile int g_menu_repeat_left, g_menu_repeat_right;

typedef bool (*Func_PadPredicate)(void *self);
static Func_PadPredicate orig_MenuInputUpJustDown = NULL;
static Func_PadPredicate orig_MenuInputDownJustDown = NULL;
static Func_PadPredicate orig_MenuInputLeftJustDown = NULL;
static Func_PadPredicate orig_MenuInputRightJustDown = NULL;

static bool wrap_MenuInputUpJustDown(void *self) {
  return (orig_MenuInputUpJustDown && orig_MenuInputUpJustDown(self)) || g_menu_repeat_up;
}
static bool wrap_MenuInputDownJustDown(void *self) {
  return (orig_MenuInputDownJustDown && orig_MenuInputDownJustDown(self)) || g_menu_repeat_down;
}
static bool wrap_MenuInputLeftJustDown(void *self) {
  return (orig_MenuInputLeftJustDown && orig_MenuInputLeftJustDown(self)) || g_menu_repeat_left;
}
static bool wrap_MenuInputRightJustDown(void *self) {
  return (orig_MenuInputRightJustDown && orig_MenuInputRightJustDown(self)) || g_menu_repeat_right;
}

static bool wrap_ConvertBufferToObject(char *buffer, int modelIndex) {
  return perform_convert_and_account(buffer, modelIndex);
}

/* CStreaming::FinishLoadingLargeFile -- the SECOND texture instantiation path,
 * and until now a complete blind spot.
 *
 * CStreaming::ProcessLoadingChannel calls both ConvertBufferToObject (hooked
 * since forever) and FinishLoadingLargeFile (resolved but never hooked);
 * verified by disassembling ProcessLoadingChannel at 0x2b74d0, which contains
 * exactly one bl to each. Models too large for a single streaming read are
 * finished here instead, and the work is the same _rwOpenGLNativeTextureRead
 * grind -- one measured frame spent 829ms across ~130 texture reads on this
 * path while our per-frame streaming total reported 0.0ms, because nothing
 * was watching it. Over one session this path cost 19.4s versus 24s for the
 * convert path: roughly half the stall budget, entirely untracked, which is
 * why prewarming the convert path alone never changed how the game felt.
 *
 * Accounting deliberately mirrors perform_convert_and_account so a large-file
 * model earns a cost history, a pin, and a prewarm candidacy on exactly the
 * same terms as any other -- the prewarm queue then treats both paths alike
 * without needing to know which one a given model will take. */
static void wrap_FinishLoadingLargeFile(char *buffer, int modelIndex) {
  STREAM_SCOPE_ENTER("FinishLoadingLargeFile", ", model=%d", modelIndex);

  uint64_t t0 = armGetSystemTick();
  if (orig_FinishLoadingLargeFile) orig_FinishLoadingLargeFile(buffer, modelIndex);
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  double ms = (double)dt_ns / 1e6;

  atomic_fetch_add_explicit(&g_streaming_frame_cpu_ns, dt_ns, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_overlay_convert_ns, dt_ns, memory_order_relaxed);
  if (dt_ns > atomic_load_explicit(&g_overlay_worst_ns, memory_order_relaxed)) {
    atomic_store_explicit(&g_overlay_worst_ns, dt_ns, memory_order_relaxed);
    atomic_store_explicit(&g_overlay_worst_index, modelIndex, memory_order_relaxed);
  }

  if (modelIndex >= 0 && modelIndex < 7000) {
    ModelCostEntry *ce = &g_model_cost_db[modelIndex];
    ce->load_count++;
    ce->last_duration_ms = (float)ms;
    ce->last_used_tick = t0;
    if (ms > ce->max_duration_ms) ce->max_duration_ms = (float)ms;
    if (ce->avg_duration_ms <= 0.001f) {
      ce->avg_duration_ms = (float)ms;
    } else {
      ce->avg_duration_ms = (ce->avg_duration_ms * 0.7f) + ((float)ms * 0.3f);
    }
    if (ms >= 30.0) {
      ce->expensive_count++;
      pin_model_bounded(modelIndex);
      enqueue_prewarm_candidate(modelIndex, (float)ms);
    }
    g_pin_evict_veto_streak[modelIndex] = 0;
  }

  if (ms >= 2.0) {
    const char *cat = (ms >= 100.0) ? "CATASTROPHIC" : (ms >= 30.0) ? "EXPENSIVE" : "MODERATE";
    LOGC(LOGC_SYS, "[STREAM_LARGEFILE] Model %d finished in %.2fms [CAT=%s] [path=FinishLoadingLargeFile]\n",
         modelIndex, ms, cat);
  }

  STREAM_SCOPE_EXIT("FinishLoadingLargeFile");
}

static void wrap_ProcessLoadingChannel(int channel) {
  STREAM_SCOPE_ENTER("ProcessLoadingChannel", ", ch=%d", channel);
  if (orig_ProcessLoadingChannel) {
    orig_ProcessLoadingChannel(channel);
  }
  STREAM_SCOPE_EXIT("ProcessLoadingChannel");
}

/* Streaming memory pool boost & initialization.
 * 512MB -> 1024MB held up fine for the 3x3/800x800-unit area-preload grid.
 * Scaling that grid to 7x7/3000x3000 units at 1024MB caused real memory
 * pressure: the deadlock-breaker (see g_pin_evict_veto_streak) started
 * firing repeatedly, force-evicting our own highest-priority PINNED/READY
 * seeded models (5878/5880/5881/5882, 300-440ms each) to make room --
 * meaning the bigger preload was actively undoing its own benefit,
 * confirmed by overshoot count going UP (111) vs the smaller grid's run
 * (82). Raising to test whether more headroom fixes the bigger grid, or
 * whether we're approaching this app's actual RAM ceiling. No hard data on
 * the real ceiling -- watch for allocation failures or an outright crash
 * (rather than the safe eviction fallback seen at 1024MB) as the sign
 * we've gone too far.
 *
 * HARD CEILING: ms_memoryAvailable is a signed 32-bit int in the original
 * engine, so it can never represent more than 2047MB -- 2048 * 1024 * 1024
 * is exactly 2^31, which overflows to a large NEGATIVE value in a signed
 * int (caught by a build warning before this ever got tested: "integer
 * overflow ... results in -2147483648"). A negative "available memory"
 * would almost certainly make the engine think it must evict everything
 * immediately, likely making things much worse, not better. 2000MB stays
 * safely under that boundary. If the whole-map plan ever needs more
 * resident streaming memory than this field can hold, raising this
 * constant further cannot get there -- that would need the engine's field
 * itself widened to 64-bit via binary patching, a much bigger undertaking. */
#define STREAMING_MEMORY_BOOST_MB 2000
#if STREAMING_MEMORY_BOOST_MB > 2047
#error "STREAMING_MEMORY_BOOST_MB * 1024 * 1024 overflows a signed 32-bit int (ms_memoryAvailable's real type) above 2047"
#endif
typedef void (*Func_CStreaming_Init)(void);
static Func_CStreaming_Init orig_CStreaming_Init = NULL;
static Func_CStreaming_Init orig_CStreaming_ReInit = NULL;

static void apply_streaming_memory_boost_staging(void) {
  uintptr_t addr = so_find_addr(&game_mod, "_ZN10CStreaming18ms_memoryAvailableE");
  if (addr) {
    int *p_avail = (int *)addr;
    int old_mb = *p_avail / (1024 * 1024);
    *p_avail = STREAMING_MEMORY_BOOST_MB * 1024 * 1024;
    LOGC(LOGC_SYS, "[STREAM_MEM] Set staging ms_memoryAvailable from %d MB to %d MB\n",
         old_mb, *p_avail / (1024 * 1024));
  }
}

static void apply_streaming_memory_boost_runtime(void) {
  uintptr_t mem_avail_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming18ms_memoryAvailableE");
  uintptr_t buf_size_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22ms_streamingBufferSizeE");

  if (mem_avail_addr) {
    int *p_avail = (int *)mem_avail_addr;
    int old_mb = *p_avail / (1024 * 1024);
    *p_avail = STREAMING_MEMORY_BOOST_MB * 1024 * 1024;
    LOGC(LOGC_SYS, "[STREAM_MEM] Boosted runtime ms_memoryAvailable from %d MB to %d MB\n",
         old_mb, *p_avail / (1024 * 1024));
  }

  if (buf_size_addr) {
    int raw_val = *(int *)buf_size_addr;
    int sectors = raw_val;
    int kb = sectors * 2;
    double mb = (double)kb / 1024.0;
    LOGC(LOGC_SYS, "[STREAM_MEM] ms_streamingBufferSize raw=%d (%d sectors, %d KB, %.2f MB)\n",
         raw_val, sectors, kb, mb);
  }
}

/* Predictive dynamic heavy-model prewarm queue with sector look-ahead */
typedef void (*Func_RequestModel)(int modelIndex, int flags);
static Func_RequestModel real_RequestModel = NULL;

/* Issues an explicit RequestModel() call for the candidate at g_prewarm_candidates[idx]
 * and updates its status/in-flight bookkeeping. Returns false only if
 * real_RequestModel couldn't be resolved (nothing was issued); true
 * otherwise, whether or not it also resolved immediately. */
static bool issue_prewarm_request(ptrdiff_t idx, int *in_flight_count) {
  if (!real_RequestModel) {
    real_RequestModel = (Func_RequestModel)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming12RequestModelEii");
  }
  if (!real_RequestModel) return false;

  PrewarmCandidate *cur = &g_prewarm_candidates[idx];
  int model = cur->model_index;

  cur->status = CANDIDATE_REQUESTED;
  cur->request_frame = g_frame_count;
  pin_model_bounded(model);
  (*in_flight_count)++;

  LOGC(LOGC_SYS, "[PREWARM_REQUEST] model=%d (hist_cost=%.1fms, priority=%.1f, entry %zd/%zu) at frame #%u [ASYNC]\n",
       model, cur->historical_ms, cur->priority, idx + 1, g_num_candidates, g_frame_count);

  real_RequestModel(model, 1); // 1 = STREAMING_KEEP_IN_MEMORY

  if (streaming_model_is_ready(model)) {
    cur->status = CANDIDATE_READY;
    cur->ready_frame = g_frame_count;
    (*in_flight_count)--;
    LOGC(LOGC_SYS, "[PREWARM_READY] model=%d became READY immediately on request (frame #%u)\n",
         model, g_frame_count);
  }
  return true;
}

/* Fires an async RequestModel() for every seeded candidate in one
 * unthrottled burst, called once at CStreaming::Init -- before CTheScripts
 * ever runs. The normal per-frame prewarm queue throttles to
 * PREWARM_MAX_NEW_REQUESTS_PER_CALL (4) requests/frame, which is correct
 * for organically-discovered candidates (avoid flooding the streaming
 * pipeline), but is far too slow for this: measured in practice, the
 * opening script bulk-converts its own required models within the same
 * single burst frame as the very first few frames of gameplay -- fast
 * enough that even priority-sorted seeds ranked ~7th or lower in the ~30
 * item seed list were still sitting in QUEUED, never even requested, when
 * the script grabbed them itself (PREWARM_STATUS=PREWARM_LATE). The seed
 * list is small, fixed, and already vetted (unlike the ever-growing
 * reactive/discovery candidate pool), so there's no flooding risk in
 * requesting all of it immediately instead of rate-limiting it. */
static bool g_seed_burst_requested = false;

static void request_all_seeds_immediately(void) {
  if (g_seed_burst_requested) return;
  g_seed_burst_requested = true;

  if (!g_prewarm_system_initialized) {
    init_prewarm_system();
  }

  int dummy_in_flight = 0; // not bounded by PREWARM_MAX_CONCURRENT_REQUESTS here
  int issued = 0;
  for (size_t i = 0; i < g_num_seeded_candidates; i++) {
    PrewarmCandidate *c = &g_prewarm_candidates[i];
    if (c->status != CANDIDATE_QUEUED && c->status != CANDIDATE_FREE) continue;
    if (issue_prewarm_request((ptrdiff_t)i, &dummy_in_flight)) issued++;
  }
  LOGC(LOGC_SYS, "[PREWARM] Seed burst: requested %d/%zu seeded candidates immediately (frame #%u)\n",
       issued, g_num_seeded_candidates, g_frame_count);
}

/* ---- CWorld sector-walk predictive discovery -----------------------------
 * real_AddModelsToRequestList(&future_pos) looked like the obvious way to
 * get lookahead for a *future* player position, but disassembly of
 * CStreaming::ProcessEntitiesInSectorList (both overloads, both still
 * present as real exported functions in this binary) shows every candidate
 * entity is gated behind CRenderer::IsEntityCullZoneVisible -- which tests
 * visibility from the CURRENT camera, not distance from the position passed
 * in. So calling it at a future position finds nothing until the player is
 * already standing there, defeating the entire point of a lookahead.
 *
 * The fix: walk CWorld's own sector entity lists ourselves, replicating
 * AddModelsToRequestList's logic (re-GTA-re3/src/core/Streaming.cpp) but
 * WITHOUT the cull-zone gate, and feed discovered candidates straight into
 * enqueue_prewarm_candidate -- the same request pipeline already proven to
 * work for seeded models.
 *
 * All struct offsets below were extracted by disassembling the real
 * exported CStreaming::ProcessEntitiesInSectorList in THIS libGame.so
 * (`aarch64-none-elf-objdump -d`, both the plain-list and the
 * x/y/xmin/ymin/xmax/ymax overloads), not copied from the re3 PC
 * decompilation -- re3's declared offsets assume 32-bit pointers throughout
 * CEntity/CPlaceable, which doesn't hold on this 64-bit ARM64 binary (same
 * kind of divergence already seen with ms_memoryAvailable being 32-bit here
 * vs re3's size_t). Re-verify against a fresh objdump of libGame.so before
 * reusing these if the binary is ever rebuilt/updated. */
#define CENTITY_OFF_FLAGS1        100  // uint32: bits0-2=m_type, bit29=bStreamingDontDelete
#define CENTITY_OFF_FLAGS2        104  // uint32: bit4=bIsSubway
#define CENTITY_OFF_SCANCODE      108  // uint16
#define CENTITY_OFF_MODELINDEX    112  // int16
#define CENTITY_OFF_POSX          56   // float
#define CENTITY_OFF_POSY          60   // float
#define CENTITY_OFF_OBJCREATEDBY  460  // uint8, only meaningful when m_type==CENTITY_TYPE_OBJECT
#define CENTITY_TYPE_OBJECT       4
#define CENTITY_FLAG_STREAMINGDONTDELETE_BIT 29 // within FLAGS1
#define CENTITY_FLAG_ISSUBWAY_BIT            4  // within FLAGS2
#define COBJECT_CREATEDBY_TEMP    3

#define CPTRNODE_OFF_ITEM 0
#define CPTRNODE_OFF_NEXT 16

// CMODELINFO_OFF_TYPEBYTE is defined earlier (just above perform_convert_and_account)
#define CMODELINFO_TYPE_TIME     3
#define CMODELINFO_OFF_TIMEON    104
#define CMODELINFO_OFF_TIMEOFF   108

#define CSECTOR_LIST_STRIDE 8 // sizeof(CPtrList) on this 64-bit build (just a `first` pointer)

/* Widened from 80 (the real engine's STREAM_DIST, matched 1:1 in the first
 * cut of this scan) to 200: diagnostics showed 74/76 PREWARM_LATE
 * conversions in a test session (up to 823ms!) were SCAN_SKIP_NONE --
 * never even walked past by the entity-list scan at all, not filtered out
 * by distance/LOD/dontdelete. The 80-unit box matched what the real engine
 * uses for its own per-frame, camera-relative pass, but there's no reason
 * to keep matching it here: this scan runs off the main render path, so a
 * bigger net costs a few more entity-list walks per second, not frame time.
 * Re-verify with fresh sector_walk=NEVER_SCANNED diagnostics after changing
 * this -- if it's still the dominant reason, the fix is scan frequency
 * (see s_last_sector_scan_frame below) or lead_dist, not this radius. */
#define SECTOR_WALK_STREAM_DIST 200.0f
#define SECTOR_WALK_NUMSECTORS 100
#define SECTOR_WALK_WORLD_MIN -2000.0f
#define SECTOR_WALK_SECTOR_SIZE 40.0f

typedef void *(*Func_CWorld_GetSector)(int x, int y);
typedef void (*Func_CWorld_ClearScanCodes)(void);
typedef float (*Func_GetLargestLodDistance)(void *this_);
typedef bool (*Func_GetIsTimeInRange)(uint8_t timeOn, uint8_t timeOff);

static Func_CWorld_GetSector real_CWorld_GetSector = NULL;
static Func_CWorld_ClearScanCodes real_CWorld_ClearScanCodes = NULL;
static Func_GetLargestLodDistance real_GetLargestLodDistance = NULL;
static Func_GetIsTimeInRange real_GetIsTimeInRange = NULL;
static uint16_t *g_pCWorld_ms_nCurrentScanCode = NULL;
/* g_pCModelInfo_ms_modelInfoPtrs is declared earlier in this file (just
 * above perform_convert_and_account) -- resolved here in
 * ensure_sector_walk_symbols(), same as the rest of these. */
/* REVERTED: attempted to also scan CWorld::ms_bigBuildingsList[NUM_LEVELS]
 * (re3 declares this as large structures like skyscrapers, stored outside
 * any CSector::m_lists[] since they're too big for one sector), hoping it
 * would explain why model 5959 (800ms+) stayed SCAN_SKIP_NONE even at a
 * 200-unit scan radius. That caused a REAL crash (Data Abort, NULL fault
 * address) on next boot. Root cause: unlike every other struct/offset in
 * this file, this one was never verified against an actual disassembly of
 * how the symbol is used in THIS binary -- and disassembling
 * CStreaming::RequestBigBuildings (which re3 says reads exactly this list)
 * showed it does NOT touch ms_bigBuildingsList at all in this build; it
 * walks a completely different pool structure (128-byte-stride records,
 * unrelated layout). Reading ms_bigBuildingsList as a CPtrList[4] here was
 * therefore walking garbage as a linked list. Lesson: every raw pointer
 * struct this scan touches needs the same nm/objdump verification the
 * CSector/CPtrList/CEntity offsets got (see the big comment above
 * CENTITY_OFF_FLAGS1) -- re3's declared existence of a symbol is not
 * evidence of how (or whether) this specific binary actually uses it.
 * Model 5959's real container is still unidentified; see mi_type diagnostic
 * added alongside this for the next lead (is it MITYPE_PED/VEHICLE, i.e.
 * never a static CEntity at all?). */
static bool g_sector_walk_symbols_ready = false;
static bool g_sector_walk_symbols_failed = false;

static bool ensure_sector_walk_symbols(void) {
  if (g_sector_walk_symbols_ready) return true;
  if (g_sector_walk_symbols_failed) return false;

  real_CWorld_GetSector = (Func_CWorld_GetSector)so_try_find_addr_rx(&game_mod, "_ZN6CWorld9GetSectorEii");
  real_CWorld_ClearScanCodes = (Func_CWorld_ClearScanCodes)so_try_find_addr_rx(&game_mod, "_ZN6CWorld14ClearScanCodesEv");
  real_GetLargestLodDistance = (Func_GetLargestLodDistance)so_try_find_addr_rx(&game_mod, "_ZN16CSimpleModelInfo21GetLargestLodDistanceEv");
  real_GetIsTimeInRange = (Func_GetIsTimeInRange)so_try_find_addr_rx(&game_mod, "_ZN6CClock16GetIsTimeInRangeEhh");
  g_pCWorld_ms_nCurrentScanCode = (uint16_t *)so_try_find_addr_rx(&game_mod, "_ZN6CWorld19ms_nCurrentScanCodeE");
  g_pCModelInfo_ms_modelInfoPtrs = (void **)so_try_find_addr_rx(&game_mod, "_ZN10CModelInfo16ms_modelInfoPtrsE");

  if (!real_CWorld_GetSector || !real_CWorld_ClearScanCodes || !real_GetLargestLodDistance ||
      !real_GetIsTimeInRange || !g_pCWorld_ms_nCurrentScanCode || !g_pCModelInfo_ms_modelInfoPtrs) {
    g_sector_walk_symbols_failed = true;
    LOGC(LOGC_SYS, "[PREWARM_SECTOR_WALK] Symbol resolution FAILED (GetSector=%p ClearScanCodes=%p "
         "GetLargestLodDistance=%p GetIsTimeInRange=%p scanCode=%p modelInfoPtrs=%p)\n",
         (void *)real_CWorld_GetSector, (void *)real_CWorld_ClearScanCodes, (void *)real_GetLargestLodDistance,
         (void *)real_GetIsTimeInRange, (void *)g_pCWorld_ms_nCurrentScanCode, (void *)g_pCModelInfo_ms_modelInfoPtrs);
    return false;
  }
  g_sector_walk_symbols_ready = true;
  return true;
}

static inline void advance_world_scan_code(void) {
  uint16_t v = (uint16_t)(*g_pCWorld_ms_nCurrentScanCode + 1);
  if (v == 0) {
    real_CWorld_ClearScanCodes();
    v = 1;
  }
  *g_pCWorld_ms_nCurrentScanCode = v;
}

/* Walks one CPtrList of entities (a CSector::m_lists[] slot), discovering
 * heavy/unknown-cost models within (xmin,ymin)-(xmax,ymax) of lookahead_x/y
 * and feeding them to enqueue_prewarm_candidate -- deliberately skipping
 * CRenderer::IsEntityCullZoneVisible (see the big comment above) since the
 * whole point is to find things the camera can't see yet. Mirrors the real
 * ProcessEntitiesInSectorList(list,x,y,xmin,ymin,xmax,ymax) overload
 * instruction-for-instruction except for that one skipped check. */
/* ScanSkipReason / g_model_last_scan_reason are declared earlier in this
 * file (just above perform_convert_and_account), since that function also
 * needs to read them for its PREWARM_LATE log line. */
typedef struct {
  int visited;
  int skip_dontdelete_subway;
  int skip_temp_object;
  int skip_no_modelinfo;
  int skip_time_gated;
  int skip_out_of_bounds;
  int skip_lod_distance;
  int skip_known_cheap;
  int skip_table_full;
  int enqueued;
  int txd_enqueued;
  int txd_cheap;
} ScanStats;

static int scan_entity_list_for_prewarm(void *list_first, float lookahead_x, float lookahead_y,
                                         float xmin, float ymin, float xmax, float ymax, ScanStats *stats) {
  int discovered = 0;
  uint16_t current_scan = *g_pCWorld_ms_nCurrentScanCode;
  uint8_t *node = (uint8_t *)list_first;

  while (node) {
    uint8_t *entity = *(uint8_t **)(node + CPTRNODE_OFF_ITEM);
    uint8_t *next_node = *(uint8_t **)(node + CPTRNODE_OFF_NEXT);
    node = next_node;
    if (!entity) continue;

    uint16_t *scanCodePtr = (uint16_t *)(entity + CENTITY_OFF_SCANCODE);
    if (*scanCodePtr == current_scan) continue;
    *scanCodePtr = current_scan;

    // Read modelIndex up front (purely for diagnostic attribution below) --
    // doesn't change filter order/behavior, we don't act on it until later.
    int16_t modelIndex = *(int16_t *)(entity + CENTITY_OFF_MODELINDEX);
    bool have_model = (modelIndex >= 0 && modelIndex < 7000);
    if (have_model) stats->visited++;

    uint32_t flags1 = *(uint32_t *)(entity + CENTITY_OFF_FLAGS1);
    uint32_t flags2 = *(uint32_t *)(entity + CENTITY_OFF_FLAGS2);
    if ((flags1 & (1u << CENTITY_FLAG_STREAMINGDONTDELETE_BIT)) ||
        (flags2 & (1u << CENTITY_FLAG_ISSUBWAY_BIT))) {
      if (have_model) { g_model_last_scan_reason[modelIndex] = SCAN_SKIP_DONTDELETE_OR_SUBWAY; stats->skip_dontdelete_subway++; }
      continue;
    }

    int mtype = flags1 & 0x7;
    if (mtype == CENTITY_TYPE_OBJECT) {
      uint8_t createdBy = *(entity + CENTITY_OFF_OBJCREATEDBY);
      if (createdBy == COBJECT_CREATEDBY_TEMP) {
        if (have_model) { g_model_last_scan_reason[modelIndex] = SCAN_SKIP_TEMP_OBJECT; stats->skip_temp_object++; }
        continue;
      }
    }

    if (!have_model) continue;

    if (modelIndex >= MODELINFO_COUNT) { g_model_last_scan_reason[modelIndex] = SCAN_SKIP_NO_MODELINFO; stats->skip_no_modelinfo++; continue; }
    void *mi = g_pCModelInfo_ms_modelInfoPtrs[modelIndex];
    if (!mi) { g_model_last_scan_reason[modelIndex] = SCAN_SKIP_NO_MODELINFO; stats->skip_no_modelinfo++; continue; }

    uint8_t modelInfoType = *((uint8_t *)mi + CMODELINFO_OFF_TYPEBYTE);
    if (modelInfoType == CMODELINFO_TYPE_TIME) {
      uint8_t timeOn = *((uint8_t *)mi + CMODELINFO_OFF_TIMEON);
      uint8_t timeOff = *((uint8_t *)mi + CMODELINFO_OFF_TIMEOFF);
      if (!real_GetIsTimeInRange(timeOn, timeOff)) {
        g_model_last_scan_reason[modelIndex] = SCAN_SKIP_TIME_GATED; stats->skip_time_gated++;
        continue;
      }
    }

    float ex = *(float *)(entity + CENTITY_OFF_POSX);
    float ey = *(float *)(entity + CENTITY_OFF_POSY);
    if (!(ex > xmin && ex < xmax && ey > ymin && ey < ymax)) {
      g_model_last_scan_reason[modelIndex] = SCAN_SKIP_OUT_OF_BOUNDS; stats->skip_out_of_bounds++;
      continue;
    }

    float lodDist = real_GetLargestLodDistance(mi);
    float lodDistSq = lodDist * lodDist;
    float capSq = SECTOR_WALK_STREAM_DIST * SECTOR_WALK_STREAM_DIST;
    if (lodDistSq > capSq) lodDistSq = capSq;
    float dx = lookahead_x - ex;
    float dy = lookahead_y - ey;
    if (dx * dx + dy * dy >= lodDistSq) {
      g_model_last_scan_reason[modelIndex] = SCAN_SKIP_LOD_DISTANCE; stats->skip_lod_distance++;
      continue;
    }

    // Cull-zone visibility check deliberately skipped -- see comment above.

    /* Enqueue this model's texture dictionary as a candidate in its own
     * right. This is where nearly all of the stall actually lives: measured
     * over a full session, TXD instantiation cost 25292ms across 354 loads
     * (worst single one 882ms) versus 315ms across 102 real model loads --
     * 98.6% of the total. TXDs are not entities, so a sector walk can never
     * encounter one directly; the only way to reach them predictively is
     * via the models that reference them, which is what this does.
     *
     * Deliberately done BEFORE the model's own known-cheap test below: a
     * cheap model can own a catastrophically expensive TXD (a 20ms building
     * sharing the 882ms dictionary), so letting the model's cost decide
     * whether we even look at its TXD would skip exactly the cases that
     * matter most. The TXD gets its own cheap-test on its own measured cost.
     *
     * Requesting a model does make the engine request its TXD too (the
     * recursive RequestModel call quoted above), but only on the NOTLOADED
     * path and only at that same instant -- so the TXD gets no lead time and
     * lands as a PREWARM_LATE stall. Tracking it as its own candidate is
     * what lets the priority queue promote it early, on its own history. */
    int16_t txdIndexRaw = *(int16_t *)((uint8_t *)mi + CMODELINFO_OFF_TXDINDEX);
    if (txdIndexRaw >= 0 && (int)txdIndexRaw + STREAM_OFFSET_TXD < NUMSTREAMINFO) {
      int txd_stream_index = (int)txdIndexRaw + STREAM_OFFSET_TXD;
      ModelCostEntry *te = &g_model_cost_db[txd_stream_index];
      if (te->load_count > 0 && te->max_duration_ms < 30.0f) {
        g_model_last_scan_reason[txd_stream_index] = SCAN_SKIP_KNOWN_CHEAP;
        stats->txd_cheap++;
      } else if (enqueue_prewarm_candidate(txd_stream_index, te->max_duration_ms)) {
        /* Count/report each dictionary once per scan even though many models
         * share one -- enqueue_prewarm_candidate itself dedups by index. */
        if (g_model_last_scan_reason[txd_stream_index] != SCAN_SKIP_ENQUEUED) {
          stats->txd_enqueued++;
          discovered++;
        }
        g_model_last_scan_reason[txd_stream_index] = SCAN_SKIP_ENQUEUED;
      } else {
        g_model_last_scan_reason[txd_stream_index] = SCAN_SKIP_TABLE_FULL;
        stats->skip_table_full++;
      }
    }

    ModelCostEntry *ce = &g_model_cost_db[modelIndex];
    if (ce->load_count > 0 && ce->max_duration_ms < 30.0f) {
      g_model_last_scan_reason[modelIndex] = SCAN_SKIP_KNOWN_CHEAP; stats->skip_known_cheap++;
      continue; // known-cheap, don't bother
    }
    if (!enqueue_prewarm_candidate(modelIndex, ce->max_duration_ms)) { // 0.0 if never measured yet
      g_model_last_scan_reason[modelIndex] = SCAN_SKIP_TABLE_FULL;
      stats->skip_table_full++;
      continue;
    }
    g_model_last_scan_reason[modelIndex] = SCAN_SKIP_ENQUEUED;
    stats->enqueued++;
    discovered++;
  }
  return discovered;
}

/* Replicates CStreaming::AddModelsToRequestList's sector-walk around an
 * arbitrary position, but skipping the cull-zone gate that makes the real
 * function useless for a future position. Returns the number of newly
 * discovered candidates (0 if symbol resolution failed); fills *out_stats
 * with the aggregate skip-reason breakdown for this scan if non-NULL. */
static int sector_walk_prewarm_scan(const CVector *pos, ScanStats *out_stats) {
  ScanStats stats = {0};
  if (!ensure_sector_walk_symbols()) {
    if (out_stats) *out_stats = stats;
    return 0;
  }

  float xmin = pos->x - SECTOR_WALK_STREAM_DIST;
  float xmax = pos->x + SECTOR_WALK_STREAM_DIST;
  float ymin = pos->y - SECTOR_WALK_STREAM_DIST;
  float ymax = pos->y + SECTOR_WALK_STREAM_DIST;

  int ixmin = (int)((xmin - SECTOR_WALK_WORLD_MIN) / SECTOR_WALK_SECTOR_SIZE);
  int ixmax = (int)((xmax - SECTOR_WALK_WORLD_MIN) / SECTOR_WALK_SECTOR_SIZE);
  int iymin = (int)((ymin - SECTOR_WALK_WORLD_MIN) / SECTOR_WALK_SECTOR_SIZE);
  int iymax = (int)((ymax - SECTOR_WALK_WORLD_MIN) / SECTOR_WALK_SECTOR_SIZE);
  if (ixmin < 0) ixmin = 0;
  if (iymin < 0) iymin = 0;
  if (ixmax >= SECTOR_WALK_NUMSECTORS) ixmax = SECTOR_WALK_NUMSECTORS - 1;
  if (iymax >= SECTOR_WALK_NUMSECTORS) iymax = SECTOR_WALK_NUMSECTORS - 1;

  advance_world_scan_code();

  int discovered = 0;
  static const int kListIndices[] = { 0 /*BUILDINGS*/, 1 /*BUILDINGS_OVERLAP*/, 2 /*OBJECTS*/, 8 /*DUMMIES*/ };

  for (int iy = iymin; iy <= iymax; iy++) {
    for (int ix = ixmin; ix <= ixmax; ix++) {
      uint8_t *sector = (uint8_t *)real_CWorld_GetSector(ix, iy);
      if (!sector) continue;
      for (size_t li = 0; li < sizeof(kListIndices) / sizeof(kListIndices[0]); li++) {
        void *list_first = *(void **)(sector + kListIndices[li] * CSECTOR_LIST_STRIDE);
        if (!list_first) continue;
        discovered += scan_entity_list_for_prewarm(list_first, pos->x, pos->y, xmin, ymin, xmax, ymax, &stats);
      }
    }
  }

  // Big-buildings-list scanning was tried and reverted here -- see the big
  // comment above (where g_pCWorld_ms_bigBuildingsList used to be declared)
  // for why: it caused a real crash from walking an unverified struct
  // layout as a linked list.

  if (out_stats) *out_stats = stats;
  return discovered;
}

void process_gradual_prewarm_queue(void) {
  if (!g_prewarm_system_initialized) {
    init_prewarm_system();
  }

  // Predictive sector lookahead every 20 frames during gameplay (lowered
  // from 60 alongside widening SECTOR_WALK_STREAM_DIST above -- resampling
  // more often as the extrapolated future_pos moves sweeps more of the
  // player's actual path instead of leaving gaps between widely-spaced
  // single-point snapshots).
  static uint32_t s_last_sector_scan_frame = 0;
  if (g_frame_count - s_last_sector_scan_frame >= 20) {
    s_last_sector_scan_frame = g_frame_count;
    if (!real_FindPlayerCoors) {
      real_FindPlayerCoors = (Func_FindPlayerCoors)so_try_find_addr_rx(&game_mod, "_Z15FindPlayerCoorsv");
      if (!real_FindPlayerCoors) {
        LOGC(LOGC_SYS, "[PREWARM_SECTOR] FindPlayerCoors symbol resolution FAILED (frame #%u)\n", g_frame_count);
      }
    }
    if (real_FindPlayerCoors) {
      CVector p_coors = real_FindPlayerCoors();
      if (!(p_coors.z > -100.0f && p_coors.z < 1000.0f)) {
        LOGC(LOGC_SYS, "[PREWARM_SECTOR] SKIPPED: coors=(%.1f, %.1f, %.1f) out of z-range (-100..1000) (frame #%u)\n",
             p_coors.x, p_coors.y, p_coors.z, g_frame_count);
      }
      if (p_coors.z > -100.0f && p_coors.z < 1000.0f) {
        static CVector s_prev_coors = {0.0f, 0.0f, 0.0f};
        static bool s_has_prev = false;
        CVector future_pos = p_coors;
        if (s_has_prev) {
          float vx = (p_coors.x - s_prev_coors.x) * 0.5f;
          float vy = (p_coors.y - s_prev_coors.y) * 0.5f;
          float speed = sqrtf(vx * vx + vy * vy);
          float lead_dist = 250.0f + (speed * 10.0f);
          if (lead_dist > 500.0f) lead_dist = 500.0f;
          if (speed > 0.1f) {
            future_pos.x += (vx / speed) * lead_dist;
            future_pos.y += (vy / speed) * lead_dist;
          }
        }
        s_prev_coors = p_coors;
        s_has_prev = true;

        // Sector-walk directly (see sector_walk_prewarm_scan above) instead
        // of real_AddModelsToRequestList(&future_pos): that engine call
        // gates every candidate on CRenderer::IsEntityCullZoneVisible,
        // which checks visibility from the CURRENT camera -- so it found
        // nothing at a future position until the player was already there.
        ScanStats stats;
        int discovered = sector_walk_prewarm_scan(&future_pos, &stats);
        LOGC(LOGC_SYS, "[PREWARM_SECTOR] Look-ahead scan at (%.1f, %.1f, %.1f) [Player at (%.1f, %.1f, %.1f)] "
             "discovered=%d new candidates, frame #%u [visited=%d dontdel/subway=%d temp=%d no_mi=%d "
             "time_gated=%d bounds=%d lod=%d cheap=%d table_full=%d txd_enq=%d txd_cheap=%d]\n",
             future_pos.x, future_pos.y, future_pos.z, p_coors.x, p_coors.y, p_coors.z, discovered, g_frame_count,
             stats.visited, stats.skip_dontdelete_subway, stats.skip_temp_object, stats.skip_no_modelinfo,
             stats.skip_time_gated, stats.skip_out_of_bounds, stats.skip_lod_distance, stats.skip_known_cheap,
             stats.skip_table_full, stats.txd_enqueued, stats.txd_cheap);
      }
    }
  }

  // Process prewarm candidates, prioritized by known historical cost rather
  // than insertion order. A model known to cost 500ms+ needs to start
  // loading far earlier than one that costs 20ms to have any chance of
  // actually being ready by the time it's needed -- but the previous
  // strict-FIFO cursor requested candidates strictly in the order they were
  // seeded/discovered, so an expensive model queued later than several
  // other expensive models sat waiting behind all of them regardless of
  // cost. Confirmed in practice: model 5678 (known cost 438ms) wasn't
  // requested until frame 404 purely because 5 other seeded models happened
  // to be enumerated ahead of it, leaving it only ~83ms of lead time for a
  // 530ms load. Sweeping the whole (<=128 entry) table each call is cheap
  // enough to run every frame.
  int in_flight_count = 0;
  for (size_t i = 0; i < g_num_candidates; i++) {
    PrewarmCandidate *cur = &g_prewarm_candidates[i];
    int model = cur->model_index;
    if (model < 0 || model >= 7000) continue;

    // Already LOADED natively: mark ready (free, no request needed).
    if (streaming_model_is_ready(model)) {
      if (cur->status != CANDIDATE_READY) {
        cur->status = CANDIDATE_READY;
        cur->ready_frame = g_frame_count;
        pin_model_bounded(model); // now actually resident -- protect it
        LOGC(LOGC_SYS, "[PREWARM_READY] model=%d already LOADED natively (status=READY, entry %zu/%zu, frame #%u)\n",
             model, i + 1, g_num_candidates, g_frame_count);
      }
      continue;
    }

    if (cur->status == CANDIDATE_REQUESTED) {
      // 120-frame timeout recovery:
      if (g_frame_count - cur->request_frame > 120) {
        cur->status = CANDIDATE_FAILED;
        LOGC(LOGC_SYS, "[PREWARM_TIMEOUT] model=%d TIMEOUT after 120 frames (frame #%u)\n",
             model, g_frame_count);
      } else {
        in_flight_count++;
      }
    }
  }

  // Pass 1: reserve budget for unknown-cost (priority == 0, never measured)
  // discovery candidates specifically, ADDITIONAL TO (not carved out of)
  // the known-priority budget in pass 2 below. Pure cost-priority ordering
  // always ranks a known-heavy candidate above an unknown one, and reactive
  // discovery keeps refilling the queue with freshly-measured heavy
  // candidates throughout an entire drive -- so there's nearly always SOME
  // known-heavy backlog, which permanently starves exactly the candidates
  // we most need to get ahead of (the ones we don't yet know are heavy).
  // Measured in practice: 108 candidates queued in one session, only 24
  // ever actually requested, and every single discovered (unknown-cost)
  // one stayed QUEUED the whole time. A first attempt at fixing this
  // carved these reserved slots out of the same fixed per-call budget
  // known-priority candidates use -- which worked (unknown candidates did
  // get requested) but measurably cut known-heavy throughput in half
  // (candidates requested dropped 50 -> 35, PREWARMED successes dropped
  // 12 -> 9), trading a reliable win for a speculative one since most
  // discovered candidates turn out cheap. Giving unknown candidates their
  // own separate budget here avoids that trade-off entirely.
  int unknown_requests_this_call = 0;
  while (unknown_requests_this_call < PREWARM_RESERVED_UNKNOWN_SLOTS_PER_CALL &&
         in_flight_count < PREWARM_MAX_CONCURRENT_REQUESTS) {
    if (!streaming_budget_allows_next_work()) break;

    ptrdiff_t unknown_idx = -1;
    for (size_t i = 0; i < g_num_candidates; i++) {
      PrewarmCandidate *c = &g_prewarm_candidates[i];
      if (c->status != CANDIDATE_QUEUED && c->status != CANDIDATE_FREE) continue;
      if (c->model_index < 0 || c->model_index >= 7000) continue;
      if (c->priority != 0.0f) continue; // only never-measured candidates
      unknown_idx = (ptrdiff_t)i;
      break; // first-found (earliest discovered) among unknowns
    }
    if (unknown_idx < 0) break; // No unknown candidates waiting.
    if (!issue_prewarm_request(unknown_idx, &in_flight_count)) break;
    unknown_requests_this_call++;
  }

  // Pass 2: known-priority candidates get their own full budget, exactly
  // as before this reserved-slot mechanism existed. Unknown-priority ones
  // are only picked up here if nothing known-heavy remains pending.
  int known_requests_this_call = 0;
  while (known_requests_this_call < PREWARM_MAX_NEW_REQUESTS_PER_CALL &&
         in_flight_count < PREWARM_MAX_CONCURRENT_REQUESTS) {
    if (!streaming_budget_allows_next_work()) {
      // Frame's streaming CPU budget is already spent (e.g. a reactive
      // heavy conversion just ran) -- don't pile a new proactive request
      // on top of it. Retry on a later call once the budget resets.
      break;
    }

    ptrdiff_t best_idx = -1;
    float best_priority = -1.0f;
    for (size_t i = 0; i < g_num_candidates; i++) {
      PrewarmCandidate *c = &g_prewarm_candidates[i];
      if (c->status != CANDIDATE_QUEUED && c->status != CANDIDATE_FREE) continue;
      if (c->model_index < 0 || c->model_index >= 7000) continue;
      if (c->priority > best_priority) {
        best_priority = c->priority;
        best_idx = (ptrdiff_t)i;
      }
    }
    if (best_idx < 0) break; // Nothing left to request.
    if (!issue_prewarm_request(best_idx, &in_flight_count)) break;
    known_requests_this_call++;
  }

  // Runs last, not first: the area-preload grid blocks for several seconds
  // (measured: 9.4s for a 3x3/400-unit grid), and running it before the
  // candidate-request passes above starved already-seeded high-priority
  // candidates of their first chance to be requested on this same call --
  // confirmed in practice: seeded models 6038/6050 still arrived
  // PREWARM_LATE despite being in kKnownHeavySeeds, because CTheScripts
  // fired its own (position-independent) REQUEST_MODEL calls for them
  // almost immediately after the grid preload finally returned control.
  // Measurement only, runs once: prints what a whole-map pre-instantiation
  // would cost against the streaming budget. Nothing here requests, loads or
  // removes anything -- see log_preload_feasibility_survey.
  log_preload_feasibility_survey();
  // Runs before the grid: every TXD the grid's LoadScene calls would have
  // faulted in one at a time is already resident by the time it starts.
  run_one_time_txd_preload();      // no-ops after its first run
  /* The grid belongs to the full_preload=0 path only. With every dictionary
   * preloaded it is pure cost: LoadScene purges the loaded list and calls
   * DeleteAllRwObjects before loading, so nine calls largely undo each other. */
  if (!config.full_preload) run_one_time_area_preload();
}

static void wrap_CStreaming_Init(void) {
  StreamingContext prev = push_streaming_context(STREAM_BOOTSTRAP);
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::Init ENTER (frame=%u, gameplay_enabled=%d, context=%s)\n",
       g_frame_count, gameplay_streaming_enabled() ? 1 : 0, streaming_context_to_string(prev));
  if (orig_CStreaming_Init) orig_CStreaming_Init();
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::Init ORIGINAL RETURN (frame=%u)\n", g_frame_count);
  apply_streaming_memory_boost_runtime();
  request_all_seeds_immediately();
  pop_streaming_context(prev);
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::Init EXIT (frame=%u, restored_context=%s)\n",
       g_frame_count, streaming_context_to_string(prev));
}

static void wrap_CStreaming_ReInit(void) {
  StreamingContext prev = push_streaming_context(STREAM_BOOTSTRAP);
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::ReInit ENTER (frame=%u, gameplay_enabled=%d, context=%s)\n",
       g_frame_count, gameplay_streaming_enabled() ? 1 : 0, streaming_context_to_string(prev));
  if (orig_CStreaming_ReInit) orig_CStreaming_ReInit();
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::ReInit ORIGINAL RETURN (frame=%u)\n", g_frame_count);
  apply_streaming_memory_boost_runtime();
  pop_streaming_context(prev);
  LOGC(LOGC_SYS, "[STREAM_LIFECYCLE] CStreaming::ReInit EXIT (frame=%u, restored_context=%s)\n",
       g_frame_count, streaming_context_to_string(prev));
}

/* Gameplay transition trigger: CPlayerPed::ProcessControl (pure observation hook) */
typedef void (*MemberFunc_CPlayerPed_ProcessControl)(void *this_ptr);
static MemberFunc_CPlayerPed_ProcessControl orig_CPlayerPed_ProcessControl = NULL;

static void wrap_CPlayerPed_ProcessControl(void *this_ptr) {
  bool was_disabled = !gameplay_streaming_enabled();

  if (orig_CPlayerPed_ProcessControl) {
    orig_CPlayerPed_ProcessControl(this_ptr);
  }

  if (was_disabled && !atomic_load_explicit(&g_gameplay_transition_pending, memory_order_relaxed)) {
    atomic_store_explicit(&g_gameplay_transition_pending, true, memory_order_release);
    LOGC(LOGC_SYS, "[STREAM_POLICY] GAMEPLAY TRANSITION OBSERVED frame=%u (pending post-frame activation)\n",
         g_frame_count);
  }
}

/* LoadScene wrapper: Safe passthrough for stability with nested call depth guard */
typedef void (*Func_LoadScene)(const void *pos);
static Func_LoadScene orig_LoadScene = NULL;

typedef void (*Func_StreamRadarSections)(const void *pos);
static Func_StreamRadarSections real_StreamRadarSections = NULL;

static void wrap_LoadScene(const void *pos) {
  void *caller = __builtin_return_address(0);
  StreamingContext ctx = get_streaming_context();
  StreamingRuntimeMode mode = get_stream_runtime_mode();
  bool gp_enabled = gameplay_streaming_enabled();

  g_load_scene_depth++;
  LOGC(LOGC_SYS, "[STREAM_CALL] LoadScene context=%s mode=%s gameplay_enabled=%d depth=%d caller=%p action=ORIGINAL\n",
       streaming_context_to_string(ctx), stream_runtime_mode_to_string(mode), gp_enabled ? 1 : 0, g_load_scene_depth, caller);

  STREAM_SCOPE_ENTER("LoadScene", ", ctx=%d, depth=%d", ctx, g_load_scene_depth);
  /* This is the engine's real loading screen, and the only one this build has.
   * The JNI showSplashScreen/hideSplashScreen pair would have been the obvious
   * place to hold the clock, but the engine never calls either in this port --
   * zero occurrences across a full session log -- so the 7.4s the engine spends
   * loading the world before our own preload even starts was running at the
   * normal clock.
   *
   * LoadScene covers starting a new game, loading a save, and island
   * transitions. It blocks while it runs, so nothing is being presented and
   * FastLoad's GPU throttle costs nothing. The reference is counted, so nesting
   * with the TXD preload or anything else is safe. */
  cpu_boost_acquire("load_scene");
  if (orig_LoadScene) orig_LoadScene(pos);
  cpu_boost_release("load_scene");
  STREAM_SCOPE_EXIT("LoadScene");
  g_load_scene_depth--;
}

/* ---------------------------------------------------------------------------
 * The 3x3 LoadScene area-preload grid, kept for config.full_preload == 0.
 *
 * It is genuinely counterproductive alongside a full preload: LoadScene
 * (0x2bb1a4) walks the loaded list and RemoveModel()s every entry whose flags
 * miss "tst w9, #0xf", then calls DeleteAllRwObjects(), and only then requests
 * around the new point -- so nine calls spend much of their time destroying
 * what the previous eight built. Its cost fell from 4088ms to 2102ms once the
 * TXD preload made textures resident ahead of it, which says it was mostly
 * faulting in what the preload now handles.
 *
 * It stays because full_preload=0 is meant to reproduce the configuration that
 * was measured on hardware (~30s load, 1.09s of gameplay stalls), and that
 * configuration included this grid. Changing both at once would make that
 * fallback something nobody has actually tested.
 * ------------------------------------------------------------------------- */
#define AREA_PRELOAD_GRID_SIZE 3
#define AREA_PRELOAD_SPACING 400.0f
static bool g_area_preload_done = false;

static void run_one_time_area_preload(void) {
  if (g_area_preload_done) return;

  // Don't block the seed backlog. Moving this call to run after the
  // candidate-request passes (see process_gradual_prewarm_queue) wasn't
  // enough on its own: a single call can only issue a handful of requests
  // (PREWARM_MAX_NEW_REQUESTS_PER_CALL + reserved slots), so most of the
  // ~30 seeded candidates hadn't even been REQUESTED yet by the time this
  // ran and then blocked the next several frames for 9-10+ seconds --
  // exactly the window CTheScripts needs to grab several of them directly.
  //
  // First fix (gating on the WHOLE candidate table being drained) still
  // didn't work: reactive/discovery candidates keep getting appended
  // continuously as the game runs (including by CTheScripts' own
  // conversions crossing the 30ms reactive-pin threshold), so the table
  // never actually empties -- the gate just waited arbitrarily long
  // (measured: frame 639 instead of ~250) without ever targeting what
  // actually mattered, by which point the script had already grabbed
  // everything it needed. Scope the gate to ONLY the fixed seed batch
  // (g_prewarm_candidates[0..g_num_seeded_candidates)), which is what
  // actually needs to drain before CTheScripts' early trigger point.
  if (!prewarm_seed_batch_drained()) return; // still waiting, retry next call

  g_area_preload_done = true;

  if (!real_FindPlayerCoors) {
    real_FindPlayerCoors = (Func_FindPlayerCoors)so_try_find_addr_rx(&game_mod, "_Z15FindPlayerCoorsv");
  }
  if (!real_FindPlayerCoors || !orig_LoadScene) {
    LOGC(LOGC_SYS, "[AREA_PRELOAD] Skipped: FindPlayerCoors or LoadScene not available (frame #%u)\n", g_frame_count);
    return;
  }

  CVector center = real_FindPlayerCoors();
  if (!(center.z > -100.0f && center.z < 1000.0f)) {
    LOGC(LOGC_SYS, "[AREA_PRELOAD] Skipped: player coords out of range (%.1f, %.1f, %.1f) (frame #%u)\n",
         center.x, center.y, center.z, g_frame_count);
    return;
  }

  const int total_points = AREA_PRELOAD_GRID_SIZE * AREA_PRELOAD_GRID_SIZE;
  const float span = (AREA_PRELOAD_GRID_SIZE - 1) * AREA_PRELOAD_SPACING;
  LOGC(LOGC_SYS, "[AREA_PRELOAD] Starting %dx%d grid (%.0f-unit spacing, %.0fx%.0f unit span) around (%.1f, %.1f, %.1f), frame #%u\n",
       AREA_PRELOAD_GRID_SIZE, AREA_PRELOAD_GRID_SIZE, AREA_PRELOAD_SPACING, span, span,
       center.x, center.y, center.z, g_frame_count);

  /* Boosted for the same reason the TXD preload is, and it needs its own
   * reference for the same reason: this runs at frame 646 against a gameplay
   * transition at 645, so main()'s pre_gameplay reference has already been
   * released by the time we get here. Nine blocking LoadScene calls measured
   * 4811ms unboosted, with the player nominally in control while they run. */
  cpu_boost_acquire("area_grid");

  uint64_t t0 = armGetSystemTick();
  int calls = 0;
  double running_ms = 0.0;
  for (int ix = 0; ix < AREA_PRELOAD_GRID_SIZE; ix++) {
    for (int iy = 0; iy < AREA_PRELOAD_GRID_SIZE; iy++) {
      CVector p = center;
      p.x += (ix - AREA_PRELOAD_GRID_SIZE / 2) * AREA_PRELOAD_SPACING;
      p.y += (iy - AREA_PRELOAD_GRID_SIZE / 2) * AREA_PRELOAD_SPACING;
      calls++;
      uint64_t call_t0 = armGetSystemTick();
      orig_LoadScene(&p);
      double call_ms = (double)armTicksToNs(armGetSystemTick() - call_t0) / 1e6;
      running_ms += call_ms;
      LOGC(LOGC_SYS, "[AREA_PRELOAD] LoadScene #%d/%d at (%.1f, %.1f, %.1f) took %.1fms\n",
           calls, total_points, p.x, p.y, p.z, call_ms);
      if (calls % 10 == 0) {
        LOGC(LOGC_SYS, "[AREA_PRELOAD] Progress: %d/%d calls, %.1fs elapsed so far\n",
             calls, total_points, running_ms / 1000.0);
      }
    }
  }
  double total_ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  cpu_boost_release("area_grid");
  LOGC(LOGC_SYS, "[AREA_PRELOAD] Completed %d LoadScene calls in %.1fms (%.2fs), frame #%u\n",
       calls, total_ms, total_ms / 1000.0, g_frame_count);
}

/* ---------------------------------------------------------------------------
 * Whole-map pre-instantiation feasibility survey (MEASUREMENT ONLY)
 *
 * Answers, with real numbers off the running console rather than an estimate,
 * the question that decides whether "preload everything during the loading
 * screen" is viable at all: how much streaming memory would a fully resident
 * map actually charge to CStreaming, and does that fit under the budget?
 *
 * Every layout constant below was read out of this libGame.so, not re3:
 *
 *   CPools::Initialise (0x2233f0) builds each pool as a 24-byte header --
 *   { void *entries; int8 *flags; int32 size; int32 allocPtr; } -- with the
 *   storage and the flag byte array as two separate new[] calls. Sizes it
 *   passes: building 5500 x 128, treadable 1214 x 176, dummy 2802 x 144,
 *   ped 140 x 1944, vehicle 110 x 1728, object 500 x 552, ptrNode 30000 x 24,
 *   entryInfoNode 5400 x 40, audioScriptObject 256 x 20. None of those are
 *   allocated by streaming a model in, so a preload cannot overflow them --
 *   which is what makes this approach worth measuring at all.
 *
 *   CStreaming::RequestAllModels (0x2b87f4) walks exactly three of those --
 *   building (stride 128), treadable (176), dummy (144), reached through the
 *   GOT slots that objdump -R names ms_pBuildingPool / ms_pTreadablePool /
 *   ms_pDummyPool -- skips any slot whose flag byte is negative (bit 7 set =
 *   free), reads a SIGNED int16 model index at entity+112, and calls
 *   RequestModel(index, 0). So the set it would request is precisely the
 *   distinct model indices of every live building, treadable and dummy.
 *
 *   CStreamingInfo is 32 bytes: m_loadState u8 @16, m_flags u8 @17,
 *   m_offset u32 @20, m_size u32 @24 (GetCdSize at 0x2b4390 returns [x0,#24]).
 *   m_size is in 2048-byte CD sectors: CStreaming::RemoveModel (0x2b7124)
 *   does "sub w8, w10, w8, lsl #11" -- ms_memoryUsed -= m_size * 2048 -- so
 *   summing m_size * 2048 predicts ms_memoryUsed at full residency directly,
 *   in the same units the eviction test uses.
 *
 *   CStreaming::MakeSpaceFor (0x2ba6b8) is that eviction test, and it compares
 *   in exactly those units: while (ms_memoryUsed >= ms_memoryAvailable - want)
 *   RemoveLeastUsedModel(). Which is why the verdict below is a straight
 *   comparison of the projected total against ms_memoryAvailable.
 * ------------------------------------------------------------------------- */

typedef struct {
  void    *entries;
  int8_t  *flags;
  int32_t  size;
  int32_t  allocPtr;
} EnginePool;

#define POOL_FLAG_SLOT_FREE      0x80  /* bit 7; RequestAllModels tests it via ldrsb + tbnz #31 */
#define ENTITY_OFF_MODELINDEX    112   /* signed int16 */
#define STREAMINFO_STRIDE        32
#define STREAMINFO_OFF_LOADSTATE 16
#define STREAMINFO_OFF_SIZE      24
#define CDSTREAM_SECTOR_SIZE     2048
#define NUM_TXD_SLOTS            (NUMSTREAMINFO - STREAM_OFFSET_TXD)

static bool g_preload_survey_done = false;

/* One streaming slot's CD size in bytes. Reads 0 for a slot with no CD entry,
 * which is how unused slots read. */
static inline uint64_t streaminfo_size_bytes(const uint8_t *info_base, int idx) {
  const uint8_t *e = info_base + (size_t)idx * STREAMINFO_STRIDE;
  uint32_t sectors;
  memcpy(&sectors, e + STREAMINFO_OFF_SIZE, sizeof(sectors));
  return (uint64_t)sectors * CDSTREAM_SECTOR_SIZE;
}

static inline uint8_t streaminfo_load_state(const uint8_t *info_base, int idx) {
  return info_base[(size_t)idx * STREAMINFO_STRIDE + STREAMINFO_OFF_LOADSTATE];
}

/* Marks one pool's live entities' model indices in seen[], returning the number
 * of occupied slots inspected. Every index is bounded against MODELINFO_COUNT
 * before use: the field is a signed int16 and nothing guarantees a live-flagged
 * slot holds a sane one. */
static int survey_mark_pool_models(const EnginePool *pool, size_t stride,
                                   uint8_t *seen, int *out_distinct) {
  int live = 0;
  if (!pool || !pool->entries || !pool->flags || pool->size <= 0) return 0;
  for (int i = 0; i < pool->size; i++) {
    if (pool->flags[i] & POOL_FLAG_SLOT_FREE) continue;
    live++;
    const uint8_t *ent = (const uint8_t *)pool->entries + (size_t)i * stride;
    int16_t model;
    memcpy(&model, ent + ENTITY_OFF_MODELINDEX, sizeof(model));
    if (model < 0 || model >= MODELINFO_COUNT) continue;
    if (!seen[model]) { seen[model] = 1; (*out_distinct)++; }
  }
  return live;
}

static void log_preload_feasibility_survey(void) {
  if (g_preload_survey_done) return;

  const uint8_t *info = g_pCStreaming_ms_aInfoForModel;
  if (!info) {
    info = (const uint8_t *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming16ms_aInfoForModelE");
    if (!info) return; /* nothing resolvable yet -- retry on the next call */
  }

  EnginePool **pp_building  = (EnginePool **)so_try_find_addr_rx(&game_mod, "_ZN6CPools16ms_pBuildingPoolE");
  EnginePool **pp_treadable = (EnginePool **)so_try_find_addr_rx(&game_mod, "_ZN6CPools17ms_pTreadablePoolE");
  EnginePool **pp_dummy     = (EnginePool **)so_try_find_addr_rx(&game_mod, "_ZN6CPools13ms_pDummyPoolE");
  void **modelinfo_ptrs     = (void **)so_try_find_addr_rx(&game_mod, "_ZN10CModelInfo16ms_modelInfoPtrsE");
  int *p_mem_used           = (int *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming13ms_memoryUsedE");
  int *p_mem_avail          = (int *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming18ms_memoryAvailableE");

  if (!pp_building || !*pp_building || !modelinfo_ptrs) return;
  /* The pools exist from CPools::Initialise but only fill once the IPLs load.
   * Surveying an empty building pool would record a meaningless zero, so wait
   * for it instead. */
  if ((*pp_building)->size <= 0 || (*pp_building)->allocPtr <= 0) return;

  g_preload_survey_done = true;

  static uint8_t seen_model[MODELINFO_COUNT];
  static uint8_t seen_txd[NUM_TXD_SLOTS];
  memset(seen_model, 0, sizeof(seen_model));
  memset(seen_txd, 0, sizeof(seen_txd));

  /* ---- 1. The whole CD directory, as the absolute upper bound ---- */
  uint64_t cd_model_bytes = 0, cd_txd_bytes = 0;
  int cd_models = 0, cd_txds = 0;
  uint64_t resident_bytes = 0;
  int resident_count = 0;
  for (int i = 0; i < NUMSTREAMINFO; i++) {
    uint64_t b = streaminfo_size_bytes(info, i);
    if (b == 0) continue;
    if (i < STREAM_OFFSET_TXD) { cd_models++; cd_model_bytes += b; }
    else                       { cd_txds++;   cd_txd_bytes   += b; }
    if (streaminfo_load_state(info, i) == 1) { resident_count++; resident_bytes += b; }
  }

  /* ---- 2. What CStreaming::RequestAllModels would actually request ---- */
  int distinct_models = 0;
  int live_buildings  = survey_mark_pool_models(*pp_building, 128, seen_model, &distinct_models);
  int live_treadables = (pp_treadable && *pp_treadable)
                      ? survey_mark_pool_models(*pp_treadable, 176, seen_model, &distinct_models) : 0;
  int live_dummies    = (pp_dummy && *pp_dummy)
                      ? survey_mark_pool_models(*pp_dummy, 144, seen_model, &distinct_models) : 0;

  uint64_t req_model_bytes = 0;
  int distinct_txds = 0;
  for (int m = 0; m < MODELINFO_COUNT; m++) {
    if (!seen_model[m]) continue;
    req_model_bytes += streaminfo_size_bytes(info, m);
    const uint8_t *mi = (const uint8_t *)modelinfo_ptrs[m];
    if (!mi) continue;
    int16_t txd;
    memcpy(&txd, mi + CMODELINFO_OFF_TXDINDEX, sizeof(txd)); /* SIGNED; -1 = none */
    if (txd < 0 || txd >= NUM_TXD_SLOTS) continue;
    if (!seen_txd[txd]) { seen_txd[txd] = 1; distinct_txds++; }
  }
  uint64_t req_txd_bytes = 0;
  for (int t = 0; t < NUM_TXD_SLOTS; t++) {
    if (seen_txd[t]) req_txd_bytes += streaminfo_size_bytes(info, STREAM_OFFSET_TXD + t);
  }

  const double MB = 1024.0 * 1024.0;
  uint64_t req_total = req_model_bytes + req_txd_bytes;
  uint64_t cd_total  = cd_model_bytes + cd_txd_bytes;
  int mem_avail = p_mem_avail ? *p_mem_avail : 0;
  int mem_used  = p_mem_used  ? *p_mem_used  : 0;

  LOGC(LOGC_SYS, "[PRELOAD_SURVEY] cd_directory: models=%d (%.1f MB) txds=%d (%.1f MB) total=%.1f MB\n",
       cd_models, cd_model_bytes / MB, cd_txds, cd_txd_bytes / MB, cd_total / MB);
  LOGC(LOGC_SYS, "[PRELOAD_SURVEY] pools: building=%d/%d treadable=%d/%d dummy=%d/%d (live/capacity)\n",
       live_buildings, (*pp_building)->size,
       live_treadables, (pp_treadable && *pp_treadable) ? (*pp_treadable)->size : 0,
       live_dummies, (pp_dummy && *pp_dummy) ? (*pp_dummy)->size : 0);
  LOGC(LOGC_SYS, "[PRELOAD_SURVEY] request_all_models: distinct_models=%d (%.1f MB) distinct_txds=%d (%.1f MB) total=%.1f MB\n",
       distinct_models, req_model_bytes / MB, distinct_txds, req_txd_bytes / MB, req_total / MB);
  LOGC(LOGC_SYS, "[PRELOAD_SURVEY] resident_now: %d slots (%.1f MB) | ms_memoryUsed=%.1f MB ms_memoryAvailable=%.1f MB\n",
       resident_count, resident_bytes / MB, mem_used / MB, mem_avail / MB);
  LOGC(LOGC_SYS, "[PRELOAD_SURVEY] verdict: whole_map=%.1f MB vs budget=%.1f MB -> %s (headroom %.1f MB) | whole_cd=%.1f MB -> %s\n",
       req_total / MB, mem_avail / MB,
       ((uint64_t)mem_avail > req_total) ? "FITS" : "OVERFLOWS",
       ((double)mem_avail - (double)req_total) / MB,
       cd_total / MB,
       ((uint64_t)mem_avail > cd_total) ? "FITS" : "OVERFLOWS");
}


/* True once every FIXED seed candidate has been requested, i.e. none is still
 * sitting in QUEUED/FREE. The TXD preload gates on this moment: late enough
 * that the
 * seeded batch is not starved, early enough to beat CTheScripts' first
 * REQUEST_MODEL calls. Deliberately scoped to the seed batch rather than the
 * whole candidate table -- reactive candidates keep getting appended as the
 * game runs, so the table never empties and a gate on it never fires. */
static bool prewarm_seed_batch_drained(void) {
  for (size_t i = 0; i < g_num_seeded_candidates; i++) {
    const PrewarmCandidate *c = &g_prewarm_candidates[i];
    if (c->status == CANDIDATE_QUEUED || c->status == CANDIDATE_FREE) return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Whole-map TXD pre-instantiation
 *
 * Measured on hardware ([PRELOAD_SURVEY] plus the spike attribution in the same
 * log), the case for doing TXDs and only TXDs:
 *
 *   - Of 67 gameplay spikes, 56 name a streaming index >= STREAM_OFFSET_TXD.
 *     They account for 4.24s of the 4.25s of gameplay streaming time. Models
 *     contribute 0.01s. The stutter is texture dictionaries, essentially
 *     nothing else.
 *   - Every TXD in the game is 68.1 MB of streaming budget (719 slots with a CD
 *     entry); the 384 the static map actually references are 51.0 MB. Against
 *     ms_memoryAvailable = 2000 MB. Residency is simply not a constraint.
 *   - The survey's own arithmetic was cross-checked against the engine: our
 *     computed resident total and ms_memoryUsed agreed exactly (38.3 MB), so
 *     m_size * 2048 is the same unit CStreaming charges itself.
 *
 * Why the preloaded TXDs then stay resident, all verified by disassembly:
 *
 *   - CStreaming::RemoveLeastUsedModel (0x2b8b74) skips any entry whose flags
 *     hit "tst w10, #0x3", so STREAMFLAGS_DONT_REMOVE is enough to be passed
 *     over -- and at 68 MB of 2000 MB, MakeSpaceFor (0x2ba6b8) never enters its
 *     eviction loop in the first place.
 *   - CStreaming::LoadScene (0x2bb1a4) purges the loaded list before it loads
 *     anything, but skips entries matching "tst w9, #0xf", which includes
 *     DONT_REMOVE. So the area-preload grid below cannot undo this.
 *   - CStreaming::RemoveNonReferencedTxds (0x2b9c74) WOULD ignore those flags
 *     and drop any TXD with no CTxdStore refs -- exactly what a preloaded TXD
 *     looks like -- but it has no callers anywhere in libGame.so. Neither does
 *     RequestAllModels. Both are dead exports in this build.
 *
 * The requests are issued in batches with a blocking drain after each, rather
 * than all 719 followed by one drain, so the work is punctuated and its cost
 * is visible per batch in the log instead of as one opaque stall.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * TXD preloading
 *
 * Two behaviours, chosen by config.full_preload, with nothing in between:
 *
 *   1 -- instantiate every texture dictionary in the game before gameplay
 *        starts, and skip the 3x3 LoadScene grid.
 *   0 -- do nothing here at all; the engine streams as it always did.
 *
 * Measured on hardware, same build, same route, stalls per 1000 gameplay
 * frames:
 *
 *   full_preload=0        ~13s load    2765ms
 *   full_preload=1        ~25s load     222ms   (~40s without the CPU boost)
 *
 * Preloading is a roughly 1:1 conversion of gameplay stall into loading time,
 * because texture instantiation is priced per upload CALL and happens on the
 * main thread inside the engine's own update, where it cannot be subdivided.
 * No scheduling trick removes that work -- an attempt to spread it across
 * gameplay frames simply converted the loading wall into 30.7s of stutter.
 *
 * A usage-profile mode used to live here, preloading only the dictionaries a
 * player's own routes had touched. It worked (~14s load, 367ms) but it was a
 * third behaviour to reason about, it only ever grew, and it converged on the
 * full preload anyway. Removed deliberately; do not reintroduce it without a
 * reason the two endpoints above do not already cover.
 * ------------------------------------------------------------------------- */

#define STREAMFLAGS_DONT_REMOVE 0x01
#define TXD_PRELOAD_BATCH 32
/* A safety stop, not a target: frame-rate stability is what this optimises.
 * The full set measures ~19s boosted, so this only exists so a bad data set
 * cannot hang the console indefinitely. */
#define TXD_PRELOAD_BUDGET_MS 60000.0

/* One preload candidate: a TXD streaming slot and its CD size. */
typedef struct {
  int32_t  index;    /* streaming index, i.e. STREAM_OFFSET_TXD + slot */
  uint32_t bytes;    /* CD size, m_size * 2048 */
} TxdPreloadCandidate;

/* Ascending by size, cheapest first, so a budget cut-off loses the least.
 *
 * Descending was tried and is measurably worse: cost is per upload CALL rather
 * than per byte, so a dictionary's cost tracks how many textures it holds, and
 * biggest-first spent an entire 12s budget on 56 of 604 dictionaries (9%),
 * leaving 2.88s of gameplay stalls where index order's 384 (64%) left 0.14s.
 * Do not reintroduce a descending sort. */
static int txd_candidate_cmp_asc(const void *a, const void *b) {
  uint32_t ba = ((const TxdPreloadCandidate *)a)->bytes;
  uint32_t bb = ((const TxdPreloadCandidate *)b)->bytes;
  if (ba < bb) return -1;
  if (ba > bb) return 1;
  return 0;
}

static TxdPreloadCandidate g_txd_candidates[NUM_TXD_SLOTS];
static int      g_txd_num_candidates = 0;
static int      g_txd_cursor = 0;
static uint64_t g_txd_candidate_bytes = 0;
static uint64_t g_txd_covered_bytes = 0;
static bool     g_txd_scan_done = false;
static bool     g_txd_preload_complete = false;

static void run_one_time_txd_preload(void) {
  if (g_txd_preload_complete) return;
  if (!config.full_preload) {
    g_txd_preload_complete = true;
    LOGC(LOGC_SYS, "[TXD_PRELOAD] Disabled (full_preload=0) -- the engine streams normally (frame #%u)\n",
         g_frame_count);
    return;
  }
  if (!prewarm_seed_batch_drained()) return; // still waiting, retry next call

  const uint8_t *info = g_pCStreaming_ms_aInfoForModel;
  if (!real_RequestModel) {
    real_RequestModel = (Func_RequestModel)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming12RequestModelEii");
  }
  if (!info || !real_RequestModel || !orig_LoadAllRequestedModels) {
    g_txd_preload_complete = true; /* nothing to retry -- these never appear later */
    LOGC(LOGC_SYS, "[TXD_PRELOAD] Skipped: info=%p RequestModel=%p LoadAll=%p (frame #%u)\n",
         (const void *)info, (void *)real_RequestModel,
         (void *)orig_LoadAllRequestedModels, g_frame_count);
    return;
  }

  int *p_mem_used = (int *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming13ms_memoryUsedE");
  const double MB = 1024.0 * 1024.0;

  if (!g_txd_scan_done) {
    g_txd_scan_done = true;
    int skipped_resident = 0, skipped_no_cd = 0;
    for (int slot = 0; slot < NUM_TXD_SLOTS; slot++) {
      int idx = STREAM_OFFSET_TXD + slot;
      uint64_t bytes = streaminfo_size_bytes(info, idx);
      if (bytes == 0) { skipped_no_cd++; continue; }
      if (streaminfo_load_state(info, idx) == 1) { skipped_resident++; continue; }
      g_txd_candidates[g_txd_num_candidates].index = idx;
      g_txd_candidates[g_txd_num_candidates].bytes = (uint32_t)bytes;
      g_txd_num_candidates++;
      g_txd_candidate_bytes += bytes;
    }
    qsort(g_txd_candidates, (size_t)g_txd_num_candidates, sizeof(g_txd_candidates[0]),
          txd_candidate_cmp_asc);

    LOGC(LOGC_SYS, "[TXD_PRELOAD] Loading every dictionary: %d to load (%.1f MB), %d already resident, %d without a CD entry; budget=%.0fms ms_memoryUsed=%.1f MB (frame #%u)\n",
         g_txd_num_candidates, g_txd_candidate_bytes / MB,
         skipped_resident, skipped_no_cd, TXD_PRELOAD_BUDGET_MS,
         p_mem_used ? *p_mem_used / MB : 0.0, g_frame_count);

    if (g_txd_num_candidates == 0) {
      g_txd_preload_complete = true;
      LOGC(LOGC_SYS, "[TXD_PRELOAD] Nothing to load -- everything is already resident (frame #%u)\n",
           g_frame_count);
      return;
    }
  }

  /* The forced drain is correct HERE and nowhere else. On the loading screen
   * nothing else is queued, so LoadAllRequestedModels only pulls in what we
   * just asked for. The same call during gameplay was a real bug: it flushes
   * the channels and drains the entire request list, dragging the engine's own
   * player-relevant streaming through synchronously -- measured at 71ms/frame
   * against a 6ms budget, roughly 1 fps. Do not move this. */
  g_txd_preload_complete = true;
  /* Its own boost reference, NOT covered by main()'s pre_gameplay one.
   * This function is gated on prewarm_seed_batch_drained(), which does not
   * fire until after the gameplay transition -- measured: the transition logs
   * at frame 645 and this runs at frame 646, well after pre_gameplay has been
   * released. The clock is back to Normal by the time we get here. */
  cpu_boost_acquire("txd_preload");

  uint64_t t0 = armGetSystemTick();
  int pending = 0, batches = 0;
  double elapsed_ms = 0.0;
  bool budget_hit = false;

  for (int c = 0; c < g_txd_num_candidates; c++) {
    real_RequestModel(g_txd_candidates[c].index, STREAMFLAGS_DONT_REMOVE);
    g_txd_covered_bytes += g_txd_candidates[c].bytes;
    g_txd_cursor++;
    pending++;

    if (pending >= TXD_PRELOAD_BATCH) {
      orig_LoadAllRequestedModels(false);
      pending = 0;
      batches++;
      elapsed_ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
      if (elapsed_ms > TXD_PRELOAD_BUDGET_MS) { budget_hit = true; break; }
    }
  }
  if (pending > 0) {
    orig_LoadAllRequestedModels(false);
    batches++;
  }
  elapsed_ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  cpu_boost_release("txd_preload");

  int resident_txds = 0;
  uint64_t resident_bytes = 0;
  for (int t = 0; t < NUM_TXD_SLOTS; t++) {
    int idx = STREAM_OFFSET_TXD + t;
    if (streaminfo_load_state(info, idx) == 1) {
      resident_txds++;
      resident_bytes += streaminfo_size_bytes(info, idx);
    }
  }

  LOGC(LOGC_SYS, "[TXD_PRELOAD] Completed in %.1fms (%.2fs): loaded %d/%d dictionaries (%.1f/%.1f MB) in %d batches, budget_hit=%d (frame #%u)\n",
       elapsed_ms, elapsed_ms / 1000.0, g_txd_cursor, g_txd_num_candidates,
       g_txd_covered_bytes / MB, g_txd_candidate_bytes / MB, batches,
       budget_hit ? 1 : 0, g_frame_count);
  LOGC(LOGC_SYS, "[TXD_PRELOAD] Result: %d/%d TXD slots resident (%.1f MB) | ms_memoryUsed=%.1f MB\n",
       resident_txds, NUM_TXD_SLOTS, resident_bytes / MB,
       p_mem_used ? *p_mem_used / MB : 0.0);
}


void emit_and_reset_frame_streaming_summary(double total_frame_ms) {
  int models = atomic_load_explicit(&g_models_loaded_this_frame, memory_order_relaxed);
  uint64_t cpu_ns = atomic_load_explicit(&g_streaming_frame_cpu_ns, memory_order_relaxed);
  double stream_cpu_ms = (double)cpu_ns / 1e6;

  if (models > 0 || stream_cpu_ms >= 0.5) {
    double max_m_ms = atomic_load_explicit(&g_frame_max_model_ms, memory_order_relaxed);
    int max_m_id = atomic_load_explicit(&g_frame_max_model_id, memory_order_relaxed);
    double max_t_ms = atomic_load_explicit(&g_frame_max_txd_ms, memory_order_relaxed);
    int max_t_id = atomic_load_explicit(&g_frame_max_txd_id, memory_order_relaxed);
    OvershootReason reason = (OvershootReason)atomic_load_explicit(&g_frame_overshoot_reason, memory_order_relaxed);

    double budget_ms = (double)config.streaming_budget_ms;
    double overshoot_ms = (stream_cpu_ms > budget_ms) ? (stream_cpu_ms - budget_ms) : 0.0;
    int overshoot_model = (overshoot_ms > 0.0) ? max_m_id : -1;

    /* Where the texture pipeline's time actually goes. Cumulative, logged
     * alongside the frame line so it can be read at any point in a session:
     * texread is the parent, the other two are its only unmeasured children. */
    {
      uint64_t tr_ns = atomic_load_explicit(&g_txpipe_texread_ns, memory_order_relaxed);
      uint64_t tr_n  = atomic_load_explicit(&g_txpipe_texread_calls, memory_order_relaxed);
      uint64_t ct_ns = atomic_load_explicit(&g_txpipe_compressed_ns, memory_order_relaxed);
      uint64_t ct_n  = atomic_load_explicit(&g_txpipe_compressed_calls, memory_order_relaxed);
      uint64_t rc_ns = atomic_load_explicit(&g_txpipe_raster_ns, memory_order_relaxed);
      uint64_t rc_n  = atomic_load_explicit(&g_txpipe_raster_calls, memory_order_relaxed);
      uint64_t fl_ns = atomic_load_explicit(&g_gl_flush_ns, memory_order_relaxed);
      uint64_t fl_n  = atomic_load_explicit(&g_gl_flush_calls, memory_order_relaxed);
      if (tr_n > 0) {
        LOGC(LOGC_SYS, "[TXPIPE_SUMMARY] texread=%.0fms/%llu (avg %.2fms) | compressedUpload=%.0fms/%llu (avg %.2fms, %.0f%% of texread) | rasterCreate=%.0fms/%llu (avg %.2fms, %.0f%%) | glFlushDrain=%.0fms/%llu (avg %.2fms) | mipUploads=%llu skipped=%llu (%.0f%% dropped) | texStorageAllocs=%llu fallbacks=%llu subImageErrors=%llu\n",
             tr_ns / 1e6, (unsigned long long)tr_n, (tr_ns / 1e6) / (double)tr_n,
             ct_ns / 1e6, (unsigned long long)ct_n, ct_n ? (ct_ns / 1e6) / (double)ct_n : 0.0,
             tr_ns ? 100.0 * (double)ct_ns / (double)tr_ns : 0.0,
             rc_ns / 1e6, (unsigned long long)rc_n, rc_n ? (rc_ns / 1e6) / (double)rc_n : 0.0,
             tr_ns ? 100.0 * (double)rc_ns / (double)tr_ns : 0.0,
             fl_ns / 1e6, (unsigned long long)fl_n, fl_n ? (fl_ns / 1e6) / (double)fl_n : 0.0,
             (unsigned long long)atomic_load_explicit(&g_gl_mip_uploaded, memory_order_relaxed),
             (unsigned long long)atomic_load_explicit(&g_gl_mip_skipped, memory_order_relaxed),
             (atomic_load_explicit(&g_gl_mip_uploaded, memory_order_relaxed) + atomic_load_explicit(&g_gl_mip_skipped, memory_order_relaxed))
               ? 100.0 * (double)atomic_load_explicit(&g_gl_mip_skipped, memory_order_relaxed) /
                 (double)(atomic_load_explicit(&g_gl_mip_uploaded, memory_order_relaxed) + atomic_load_explicit(&g_gl_mip_skipped, memory_order_relaxed))
               : 0.0,
             (unsigned long long)atomic_load_explicit(&g_gl_texstorage_allocs, memory_order_relaxed),
             (unsigned long long)atomic_load_explicit(&g_gl_texstorage_fallbacks, memory_order_relaxed),
             (unsigned long long)atomic_load_explicit(&g_gl_subimage_errors, memory_order_relaxed));
      }
    }

    LOGC(LOGC_SYS, "[STREAM_FRAME] frame=%u total_frame_ms=%.2f stream_cpu_ms=%.2f models_loaded=%d max_model_ms=%.2f (model=%d) max_txd_ms=%.2f (txd=%d) budget_ms=%.2f budget_overshoot_ms=%.2f overshoot_model=%d overshoot_reason=%s\n",
         g_frame_count, total_frame_ms, stream_cpu_ms, models, max_m_ms, max_m_id, max_t_ms, max_t_id,
         budget_ms, overshoot_ms, overshoot_model, overshoot_reason_to_string(reason));
  }

  // Reset per-frame tracking for the next frame
  atomic_store_explicit(&g_models_loaded_this_frame, 0, memory_order_relaxed);
  atomic_store_explicit(&g_streaming_frame_cpu_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&g_frame_max_model_ms, 0.0, memory_order_relaxed);
  atomic_store_explicit(&g_frame_max_model_id, -1, memory_order_relaxed);
  atomic_store_explicit(&g_frame_max_txd_ms, 0.0, memory_order_relaxed);
  atomic_store_explicit(&g_frame_max_txd_id, -1, memory_order_relaxed);
  atomic_store_explicit(&g_frame_overshoot_reason, OVERSHOOT_NONE, memory_order_relaxed);
}

DEFINE_MEMBER_PHASE_HOOK(CTheScripts_Process, "CTheScripts::Process")
DEFINE_MEMBER_PHASE_HOOK(CParticle_Update, "CParticle::Update")
DEFINE_MEMBER_PHASE_HOOK(CPopulation_Update, "CPopulation::Update")
DEFINE_MEMBER_PHASE_HOOK(CWorld_Process, "CWorld::Process")
DEFINE_MEMBER_PHASE_HOOK(CCamera_Process, "CCamera::Process")
DEFINE_MEMBER_PHASE_HOOK(CShadows_UpdateStaticShadows, "CShadows::UpdateStaticShadows")
DEFINE_MEMBER_PHASE_HOOK(CCarCtrl_GenerateRandomCars, "CCarCtrl::GenerateRandomCars")
DEFINE_MEMBER_PHASE_HOOK(CClouds_Update, "CClouds::Update")
DEFINE_MEMBER_PHASE_HOOK(CSkidmarks_Update, "CSkidmarks::Update")
DEFINE_MEMBER_PHASE_HOOK(CWaterLevel_Update, "CWaterLevel::Update")
DEFINE_MEMBER_PHASE_HOOK(CAnimManager_Update, "CAnimManager::Update")
DEFINE_MEMBER_PHASE_HOOK(CPathFind_Update, "CPathFind::Update")
DEFINE_MEMBER_PHASE_HOOK(CExplosion_Update, "CExplosion::Update")
DEFINE_MEMBER_PHASE_HOOK(CFire_Update, "CFire::Update")
DEFINE_MEMBER_PHASE_HOOK(CProjectileInfo_Update, "CProjectileInfo::Update")
DEFINE_MEMBER_PHASE_HOOK(CPacmanPickups_Update, "CPacmanPickups::Update")
DEFINE_MEMBER_PHASE_HOOK(CGlass_Update, "CGlass::Update")
DEFINE_MEMBER_PHASE_HOOK(CRopes_Update, "CRopes::Update")
DEFINE_MEMBER_PHASE_HOOK(CCoronas_Update, "CCoronas::Update")

void setup_subsystem_phase_hooks(void) {
  LOGC(LOGC_SYS, "[PHASE_HOOK] Stage 0: Disassembling CGame::Process() (0x1fb700) BL targets...\n");

  if (game_mod.load_base) {
    uint32_t *cgame_insns = (uint32_t *)((uint8_t *)game_mod.load_base + 0x1fb700);
    for (int i = 0; i < 128; i++) {
      uint32_t insn = cgame_insns[i];
      if ((insn & 0xfc000000u) == 0x94000000u) { // BL instruction
        int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
        if (imm26 & 0x02000000u) imm26 |= (int32_t)0xfc000000u; // Sign extend
        uintptr_t target_rva = 0x1fb700 + i * 4 + (imm26 * 4);
        const char *sym_name = so_nearest_symbol(&game_mod, target_rva);
        uintptr_t sym_addr = so_try_find_addr_rx(&game_mod, sym_name ? sym_name : "");

        if (sym_addr && (sym_addr - (uintptr_t)game_mod.load_virtbase) == target_rva) {
          LOGC(LOGC_SYS, "  [STAGE 0 EXACT] Call site 0x%06x -> BL target 0x%06x matches '%s'\n",
               0x1fb700 + i * 4, (uint32_t)target_rva, sym_name);
        } else if (sym_name) {
          LOGC(LOGC_SYS, "  [STAGE 0 INSIDE] Call site 0x%06x -> BL target 0x%06x inside '%s' (+0x%lx)\n",
               0x1fb700 + i * 4, (uint32_t)target_rva, sym_name,
               (unsigned long)(target_rva - (sym_addr ? sym_addr - (uintptr_t)game_mod.load_virtbase : 0)));
        } else {
          LOGC(LOGC_SYS, "  [STAGE 0 UNKNOWN] Call site 0x%06x -> BL target 0x%06x\n",
               0x1fb700 + i * 4, (uint32_t)target_rva);
        }
      }
    }
  }

  uintptr_t streaming_update_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming6UpdateEv");
  if (streaming_update_addr && game_mod.load_base && game_mod.load_virtbase) {
    uintptr_t streaming_rva = streaming_update_addr - (uintptr_t)game_mod.load_virtbase;
    uint32_t *cstream_insns = (uint32_t *)((uint8_t *)game_mod.load_base + streaming_rva);
    LOGC(LOGC_SYS, "[PHASE_HOOK] Stage 0: Disassembling CStreaming::Update() (RVA 0x%06lx) BL targets...\n", (unsigned long)streaming_rva);
    for (int i = 0; i < 64; i++) {
      uint32_t insn = cstream_insns[i];
      if ((insn & 0xfc000000u) == 0x94000000u) { // BL instruction
        int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
        if (imm26 & 0x02000000u) imm26 |= (int32_t)0xfc000000u; // Sign extend
        uintptr_t target_rva = streaming_rva + i * 4 + (imm26 * 4);
        const char *sym_name = so_nearest_symbol(&game_mod, target_rva);
        uintptr_t sym_addr = so_try_find_addr_rx(&game_mod, sym_name ? sym_name : "");

        if (sym_addr && (sym_addr - (uintptr_t)game_mod.load_virtbase) == target_rva) {
          LOGC(LOGC_SYS, "  [CSTREAM BL EXACT] Call site 0x%06lx -> BL target 0x%06lx matches '%s'\n",
               (unsigned long)(streaming_rva + i * 4), (unsigned long)target_rva, sym_name);
        } else if (sym_name) {
          LOGC(LOGC_SYS, "  [CSTREAM BL INSIDE] Call site 0x%06lx -> BL target 0x%06lx inside '%s' (+0x%lx)\n",
               (unsigned long)(streaming_rva + i * 4), (unsigned long)target_rva, sym_name,
               (unsigned long)(target_rva - (sym_addr ? sym_addr - (uintptr_t)game_mod.load_virtbase : 0)));
        } else {
          LOGC(LOGC_SYS, "  [CSTREAM BL UNKNOWN] Call site 0x%06lx -> BL target 0x%06lx\n",
               (unsigned long)(streaming_rva + i * 4), (unsigned long)target_rva);
        }
      }
    }
  }

  LOGC(LOGC_SYS, "[PHASE_HOOK] Scanning game_mod for CStreaming/CdStream dynsym entries...\n");
  if (game_mod.syms && game_mod.dynstrtab) {
    int cstream_found = 0;
    for (int i = 0; i < game_mod.num_syms; i++) {
      const char *name = game_mod.dynstrtab + game_mod.syms[i].st_name;
      if (name && (strstr(name, "CStreaming") != NULL || strstr(name, "CdStream") != NULL)) {
        LOGC(LOGC_SYS, "  Found streaming symbol: '%s' at RVA 0x%06lx\n", name, (unsigned long)game_mod.syms[i].st_value);
        cstream_found++;
      }
    }
    LOGC(LOGC_SYS, "[PHASE_HOOK] Found %d streaming symbols in .dynsym\n", cstream_found);
  }

  LOGC(LOGC_SYS, "[PHASE_HOOK] Registering ABI-safe subsystem entry phase profiling hooks...\n");
  int installed_count = 0;

  #define INSTALL_PHASE_HOOK(name, symbol_mangled, human_str) do { \
    uintptr_t addr = so_try_find_addr_rx(&game_mod, symbol_mangled); \
    if (addr) { \
      orig_##name = (MemberFunc_##name)hook_arm64_trampoline(addr, (uintptr_t)phase_wrap_##name); \
      if (orig_##name) { \
        installed_count++; \
        LOGC(LOGC_SYS, "[PHASE_HOOK] Subsystem '%s' hooked at %p (orig trampoline %p)\n", human_str, (void *)addr, (void *)orig_##name); \
      } else { \
        LOGC(LOGC_SYS, "[PHASE_HOOK] REJECTED hook for '%s' at %p (unrelocatable prologue)\n", human_str, (void *)addr); \
      } \
    } else { \
      LOGC(LOGC_SYS, "[PHASE_HOOK] Optional symbol '%s' (%s) not found in binary\n", human_str, symbol_mangled); \
    } \
  } while (0)

  INSTALL_PHASE_HOOK(CMenuManager_Process, "_ZN12CMenuManager7ProcessEv", "CMenuManager::Process");
  INSTALL_PHASE_HOOK(CStreaming_Update, "_ZN10CStreaming6UpdateEv", "CStreaming::Update");
  INSTALL_PHASE_HOOK(CTheScripts_Process, "_ZN11CTheScripts7ProcessEv", "CTheScripts::Process");
  INSTALL_PHASE_HOOK(CParticle_Update, "_ZN9CParticle6UpdateEv", "CParticle::Update");
  INSTALL_PHASE_HOOK(CPopulation_Update, "_ZN11CPopulation6UpdateEv", "CPopulation::Update");
  INSTALL_PHASE_HOOK(CWorld_Process, "_ZN6CWorld7ProcessEv", "CWorld::Process");
  INSTALL_PHASE_HOOK(CCamera_Process, "_ZN7CCamera7ProcessEv", "CCamera::Process");
  INSTALL_PHASE_HOOK(CShadows_UpdateStaticShadows, "_ZN8CShadows19UpdateStaticShadowsEv", "CShadows::UpdateStaticShadows");
  INSTALL_PHASE_HOOK(CCarCtrl_GenerateRandomCars, "_ZN8CCarCtrl18GenerateRandomCarsEv", "CCarCtrl::GenerateRandomCars");
  INSTALL_PHASE_HOOK(CClouds_Update, "_ZN7CClouds6UpdateEv", "CClouds::Update");
  INSTALL_PHASE_HOOK(CSkidmarks_Update, "_ZN10CSkidmarks6UpdateEv", "CSkidmarks::Update");
  INSTALL_PHASE_HOOK(CWaterLevel_Update, "_ZN11CWaterLevel6UpdateEv", "CWaterLevel::Update");
  INSTALL_PHASE_HOOK(CAnimManager_Update, "_ZN12CAnimManager6UpdateEv", "CAnimManager::Update");
  INSTALL_PHASE_HOOK(CPathFind_Update, "_ZN9CPathFind6UpdateEv", "CPathFind::Update");
  INSTALL_PHASE_HOOK(CExplosion_Update, "_ZN10CExplosion6UpdateEv", "CExplosion::Update");
  INSTALL_PHASE_HOOK(CFire_Update, "_ZN5CFire6UpdateEv", "CFire::Update");
  INSTALL_PHASE_HOOK(CProjectileInfo_Update, "_ZN15CProjectileInfo6UpdateEv", "CProjectileInfo::Update");
  INSTALL_PHASE_HOOK(CPacmanPickups_Update, "_ZN14CPacmanPickups6UpdateEv", "CPacmanPickups::Update");
  INSTALL_PHASE_HOOK(CGlass_Update, "_ZN6CGlass6UpdateEv", "CGlass::Update");
  INSTALL_PHASE_HOOK(CRopes_Update, "_ZN6CRopes6UpdateEv", "CRopes::Update");
  INSTALL_PHASE_HOOK(CCoronas_Update, "_ZN8CCoronas6UpdateEv", "CCoronas::Update");

  #undef INSTALL_PHASE_HOOK

  uintptr_t loadall_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22LoadAllRequestedModelsEb");
  if (!loadall_addr) {
    loadall_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22LoadAllRequestedModelsEv");
  }
  if (loadall_addr) {
    orig_LoadAllRequestedModels = (Func_LoadAllRequestedModels)hook_arm64_trampoline(loadall_addr, (uintptr_t)wrap_LoadAllRequestedModels);
    if (orig_LoadAllRequestedModels) {
      installed_count++;
      LOGC(LOGC_SYS, "[PHASE_HOOK] Subsystem 'CStreaming::LoadAllRequestedModels' hooked at %p (orig trampoline %p)\n",
           (void *)loadall_addr, (void *)orig_LoadAllRequestedModels);
    }
  }

  uintptr_t loadreq_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming19LoadRequestedModelsEv");
  if (loadreq_addr) {
    orig_LoadRequestedModels = (Func_LoadRequestedModels)hook_arm64_trampoline(loadreq_addr, (uintptr_t)wrap_LoadRequestedModels);
    if (orig_LoadRequestedModels) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::LoadRequestedModels hooked at %p (orig trampoline %p)\n",
           (void *)loadreq_addr, (void *)orig_LoadRequestedModels);
    } else {
      LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for CStreaming::LoadRequestedModels at %p (unrelocatable prologue)\n", (void *)loadreq_addr);
    }
  }

  uintptr_t proc_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming21ProcessLoadingChannelEi");
  if (proc_addr) {
    orig_ProcessLoadingChannel = (Func_ProcessLoadingChannel)hook_arm64_trampoline(proc_addr, (uintptr_t)wrap_ProcessLoadingChannel);
    if (orig_ProcessLoadingChannel) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::ProcessLoadingChannel hooked at %p (orig trampoline %p)\n",
           (void *)proc_addr, (void *)orig_ProcessLoadingChannel);
    } else {
      LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for CStreaming::ProcessLoadingChannel at %p (unrelocatable prologue)\n", (void *)proc_addr);
    }
  }

  uintptr_t reqstream_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming18RequestModelStreamEi");
  if (reqstream_addr) {
    orig_RequestModelStream = (Func_RequestModelStream)hook_arm64_trampoline(reqstream_addr, (uintptr_t)wrap_RequestModelStream);
    if (orig_RequestModelStream) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::RequestModelStream hooked at %p (orig trampoline %p)\n",
           (void *)reqstream_addr, (void *)orig_RequestModelStream);
    } else {
      LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for CStreaming::RequestModelStream at %p (unrelocatable prologue)\n", (void *)reqstream_addr);
    }
  }

  uintptr_t conv_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming21ConvertBufferToObjectEPci");
  if (conv_addr) {
    orig_ConvertBufferToObject = (Func_ConvertBufferToObject)hook_arm64_trampoline(conv_addr, (uintptr_t)wrap_ConvertBufferToObject);
    if (orig_ConvertBufferToObject) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::ConvertBufferToObject hooked at %p (orig trampoline %p)\n",
           (void *)conv_addr, (void *)orig_ConvertBufferToObject);
    } else {
      LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for CStreaming::ConvertBufferToObject at %p (unrelocatable prologue)\n", (void *)conv_addr);
    }
  }

  uintptr_t atomic_addr = so_try_find_addr_rx(&game_mod, "_ZN11CFileLoader14LoadAtomicFileEP8RwStreamj");
  if (atomic_addr) {
    orig_LoadAtomicFile = (Func_LoadAtomicFile)hook_arm64_trampoline(atomic_addr, (uintptr_t)wrap_LoadAtomicFile);
    if (orig_LoadAtomicFile) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CFileLoader::LoadAtomicFile hooked at %p (orig trampoline %p)\n",
           (void *)atomic_addr, (void *)orig_LoadAtomicFile);
    }
  }

  uintptr_t clump_addr = so_try_find_addr_rx(&game_mod, "_ZN11CFileLoader13LoadClumpFileEP8RwStreamj");
  if (clump_addr) {
    orig_LoadClumpFile = (Func_LoadClumpFile)hook_arm64_trampoline(clump_addr, (uintptr_t)wrap_LoadClumpFile);
    if (orig_LoadClumpFile) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CFileLoader::LoadClumpFile hooked at %p (orig trampoline %p)\n",
           (void *)clump_addr, (void *)orig_LoadClumpFile);
    }
  }

  uintptr_t txd_addr = so_try_find_addr_rx(&game_mod, "_ZN9CTxdStore7LoadTxdEiP8RwStream");
  if (txd_addr) {
    orig_LoadTxd = (Func_LoadTxd)hook_arm64_trampoline(txd_addr, (uintptr_t)wrap_LoadTxd);
    if (orig_LoadTxd) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CTxdStore::LoadTxd hooked at %p (orig trampoline %p)\n",
           (void *)txd_addr, (void *)orig_LoadTxd);
    }
  }

  {
    /* GXT strings are UTF-16; widening an ASCII literal at runtime avoids
     * depending on how the toolchain lays out a u"" literal, and the Latin
     * range is identical in both. */
    static const char kPrompt[] = "Tap Or Press ::MENUOK:: To Continue.";
    for (size_t i = 0; i < sizeof(kPrompt) && i < sizeof(g_splash_prompt) / sizeof(g_splash_prompt[0]); i++)
      g_splash_prompt[i] = (uint16_t)(unsigned char)kPrompt[i];

    uintptr_t a = so_try_find_addr_rx(&game_mod, "_ZN5CText3GetEPKc");
    if (a) {
      orig_CTextGet = (Func_CTextGet)hook_arm64_trampoline(a, (uintptr_t)wrap_CTextGet);
      if (orig_CTextGet) {
        installed_count++;
        LOGC(LOGC_SYS, "[PAD] CText::Get hooked at %p -- splash prompt names touch and pad\n", (void *)a);
      }
    }
  }

  {
    static const struct {
      const char *sym;
      Func_PadPredicate *orig;
      void *wrap;
      const char *what;
    } kMenuRepeat[] = {
      { "_ZN4CPad19MenuInputUpJustDownEv",    &orig_MenuInputUpJustDown,    (void *)wrap_MenuInputUpJustDown,    "MenuUp" },
      { "_ZN4CPad21MenuInputDownJustDownEv",  &orig_MenuInputDownJustDown,  (void *)wrap_MenuInputDownJustDown,  "MenuDown" },
      { "_ZN4CPad21MenuInputLeftJustDownEv",  &orig_MenuInputLeftJustDown,  (void *)wrap_MenuInputLeftJustDown,  "MenuLeft" },
      { "_ZN4CPad22MenuInputRightJustDownEv", &orig_MenuInputRightJustDown, (void *)wrap_MenuInputRightJustDown, "MenuRight" },
    };
    for (size_t i = 0; i < sizeof(kMenuRepeat) / sizeof(kMenuRepeat[0]); i++) {
      uintptr_t a = so_try_find_addr_rx(&game_mod, kMenuRepeat[i].sym);
      if (!a) {
        LOGC(LOGC_SYS, "[PAD] %s symbol not found -- no auto-repeat\n", kMenuRepeat[i].what);
        continue;
      }
      *kMenuRepeat[i].orig =
          (Func_PadPredicate)hook_arm64_trampoline(a, (uintptr_t)kMenuRepeat[i].wrap);
      if (*kMenuRepeat[i].orig) {
        installed_count++;
        LOGC(LOGC_SYS, "[PAD] %s auto-repeat hooked at %p\n", kMenuRepeat[i].what, (void *)a);
      } else {
        LOGC(LOGC_SYS, "[PAD] REJECTED auto-repeat hook for %s at %p\n", kMenuRepeat[i].what, (void *)a);
      }
    }
  }

  uintptr_t comptex_addr = so_try_find_addr_rx(&game_mod, "_Z26emu_glCompressedTexImage2DjijiiiiPKv");
  if (comptex_addr) {
    orig_EmuGlCompressedTexImage2D =
        (Func_EmuGlCompressedTexImage2D)hook_arm64_trampoline(comptex_addr, (uintptr_t)wrap_EmuGlCompressedTexImage2D);
    if (orig_EmuGlCompressedTexImage2D) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] emu_glCompressedTexImage2D hooked at %p (orig trampoline %p)\n",
           (void *)comptex_addr, (void *)orig_EmuGlCompressedTexImage2D);
    }
  }

  uintptr_t raster_addr = so_try_find_addr_rx(&game_mod, "_Z14RwRasterCreateiiii");
  if (raster_addr) {
    orig_RwRasterCreate = (Func_RwRasterCreate)hook_arm64_trampoline(raster_addr, (uintptr_t)wrap_RwRasterCreate);
    if (orig_RwRasterCreate) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] RwRasterCreate hooked at %p (orig trampoline %p)\n",
           (void *)raster_addr, (void *)orig_RwRasterCreate);
    }
  }

  uintptr_t dxt_swizzle_addr = so_try_find_addr_rx(&game_mod, "_Z37dxtSwizzler_CreateUncompressedTexturejjjPKvRj");
  if (dxt_swizzle_addr) {
    orig_DxtSwizzlerCreateUncompressedTexture = (Func_DxtSwizzlerCreateUncompressedTexture)hook_arm64_trampoline(dxt_swizzle_addr, (uintptr_t)wrap_DxtSwizzlerCreateUncompressedTexture);
    if (orig_DxtSwizzlerCreateUncompressedTexture) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] dxtSwizzler_CreateUncompressedTexture hooked at %p (orig trampoline %p)\n",
           (void *)dxt_swizzle_addr, (void *)orig_DxtSwizzlerCreateUncompressedTexture);
    }
  } else {
    LOGC(LOGC_SYS, "[STREAM_HOOK] dxtSwizzler_CreateUncompressedTexture symbol not found (nothing to hook)\n");
  }

  {
    static const struct { const char *symbol; const char *label; void *wrapper; void **orig_slot; } tx_pipeline_hooks[] = {
      { "_Z28RwTexDictionaryGtaStreamReadP8RwStream", "RwTexDictionaryGtaStreamRead", (void *)wrap_RwTexDictionaryGtaStreamRead, (void **)&orig_RwTexDictionaryGtaStreamRead },
      { "_Z22RwTextureGtaStreamReadP8RwStream", "RwTextureGtaStreamRead", (void *)wrap_RwTextureGtaStreamRead, (void **)&orig_RwTextureGtaStreamRead },
      { "_Z26_rwOpenGLNativeTextureReadPvS_i", "_rwOpenGLNativeTextureRead", (void *)wrap_rwOpenGLNativeTextureRead, (void **)&orig_rwOpenGLNativeTextureRead },
      { "_Z30RwTextureRasterGenerateMipmapsP8RwRasterP7RwImage", "RwTextureRasterGenerateMipmaps", (void *)wrap_RwTextureRasterGenerateMipmaps, (void **)&orig_RwTextureRasterGenerateMipmaps },
    };
    for (size_t i = 0; i < sizeof(tx_pipeline_hooks) / sizeof(tx_pipeline_hooks[0]); i++) {
      uintptr_t addr = so_try_find_addr_rx(&game_mod, tx_pipeline_hooks[i].symbol);
      if (!addr) {
        LOGC(LOGC_SYS, "[STREAM_HOOK] %s symbol not found (nothing to hook)\n", tx_pipeline_hooks[i].label);
        continue;
      }
      void *orig = (void *)hook_arm64_trampoline(addr, (uintptr_t)tx_pipeline_hooks[i].wrapper);
      if (orig) {
        *tx_pipeline_hooks[i].orig_slot = orig;
        installed_count++;
        LOGC(LOGC_SYS, "[STREAM_HOOK] %s hooked at %p (orig trampoline %p)\n", tx_pipeline_hooks[i].label, (void *)addr, orig);
      } else {
        LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for %s at %p (unrelocatable prologue)\n", tx_pipeline_hooks[i].label, (void *)addr);
      }
    }
  }

  uintptr_t init_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming4InitEv");
  if (init_addr) {
    orig_CStreaming_Init = (Func_CStreaming_Init)hook_arm64_trampoline(init_addr, (uintptr_t)wrap_CStreaming_Init);
    if (orig_CStreaming_Init) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::Init hooked at %p (orig trampoline %p)\n",
           (void *)init_addr, (void *)orig_CStreaming_Init);
    }
  }

  uintptr_t reinit_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming6ReInitEv");
  if (reinit_addr) {
    orig_CStreaming_ReInit = (Func_CStreaming_Init)hook_arm64_trampoline(reinit_addr, (uintptr_t)wrap_CStreaming_ReInit);
    if (orig_CStreaming_ReInit) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::ReInit hooked at %p (orig trampoline %p)\n",
           (void *)reinit_addr, (void *)orig_CStreaming_ReInit);
    }
  }

  uintptr_t loadscene_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming9LoadSceneERK7CVector");
  if (loadscene_addr) {
    orig_LoadScene = (Func_LoadScene)hook_arm64_trampoline(loadscene_addr, (uintptr_t)wrap_LoadScene);
    if (orig_LoadScene) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::LoadScene hooked at %p (orig trampoline %p)\n",
           (void *)loadscene_addr, (void *)orig_LoadScene);
    }
  }

  uintptr_t remove_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming20RemoveLeastUsedModelEv");
  if (remove_addr) {
    orig_RemoveLeastUsedModel = (Func_RemoveLeastUsedModel)hook_arm64_trampoline(remove_addr, (uintptr_t)wrap_RemoveLeastUsedModel);
    if (orig_RemoveLeastUsedModel) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::RemoveLeastUsedModel hooked at %p (orig trampoline %p)\n",
           (void *)remove_addr, (void *)orig_RemoveLeastUsedModel);
    }
  }

  uintptr_t remove_model_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming11RemoveModelEi");
  if (remove_model_addr) {
    orig_RemoveModel = (Func_RemoveModel)hook_arm64_trampoline(remove_model_addr, (uintptr_t)wrap_RemoveModel);
    if (orig_RemoveModel) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::RemoveModel hooked at %p (orig trampoline %p)\n",
           (void *)remove_model_addr, (void *)orig_RemoveModel);
    }
  }

  uintptr_t settxd_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22SetModelTxdIsDeletableEi");
  if (settxd_addr) {
    orig_SetModelTxdIsDeletable = (Func_SetModelTxdIsDeletable)hook_arm64_trampoline(settxd_addr, (uintptr_t)wrap_SetModelTxdIsDeletable);
    if (orig_SetModelTxdIsDeletable) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::SetModelTxdIsDeletable hooked at %p (orig trampoline %p)\n",
           (void *)settxd_addr, (void *)orig_SetModelTxdIsDeletable);
    }
  }

  uintptr_t remtxd_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming23RemoveNonReferencedTxdsEi");
  if (remtxd_addr) {
    orig_RemoveNonReferencedTxds = (Func_RemoveNonReferencedTxds)hook_arm64_trampoline(remtxd_addr, (uintptr_t)wrap_RemoveNonReferencedTxds);
    if (orig_RemoveNonReferencedTxds) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::RemoveNonReferencedTxds hooked at %p (orig trampoline %p)\n",
           (void *)remtxd_addr, (void *)orig_RemoveNonReferencedTxds);
    }
  }

  uintptr_t player_process_addr = so_try_find_addr_rx(&game_mod, "_ZN10CPlayerPed14ProcessControlEv");
  if (player_process_addr) {
    orig_CPlayerPed_ProcessControl = (MemberFunc_CPlayerPed_ProcessControl)hook_arm64_trampoline(player_process_addr, (uintptr_t)wrap_CPlayerPed_ProcessControl);
    if (orig_CPlayerPed_ProcessControl) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CPlayerPed::ProcessControl hooked at %p (orig trampoline %p)\n",
           (void *)player_process_addr, (void *)orig_CPlayerPed_ProcessControl);
    }
  }

  // Resolve native streaming helper sub-functions
  real_AddModelsToRequestList = (Func_AddModelsToRequestList)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22AddModelsToRequestListERK7CVector");
  real_StreamRadarSections = (Func_StreamRadarSections)so_try_find_addr_rx(&game_mod, "_ZN6CRadar19StreamRadarSectionsERK7CVector");

  // Resolve native streaming state machine variables & sub-functions
  g_pCStreaming_ms_channel = (CStreamingChannel *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming10ms_channelE");
  g_pCStreaming_ms_pStreamingBuffer = (char **)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming19ms_pStreamingBufferE");
  g_pCStreaming_ms_channelError = (int *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming15ms_channelErrorE");
  g_pCStreaming_ms_aInfoForModel = (uint8_t *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming16ms_aInfoForModelE");
  g_pCStreaming_ms_bLoadingBigModel = (bool *)so_try_find_addr_rx(&game_mod, "_ZN10CStreaming19ms_bLoadingBigModelE");
  real_CdStreamGetStatus = (Func_CdStreamGetStatus)so_try_find_addr_rx(&game_mod, "CdStreamGetStatus");
  uintptr_t largefile_addr = so_try_find_addr_rx(&game_mod, "_ZN10CStreaming22FinishLoadingLargeFileEPci");
  if (largefile_addr) {
    orig_FinishLoadingLargeFile =
        (Func_FinishLoadingLargeFile)hook_arm64_trampoline(largefile_addr, (uintptr_t)wrap_FinishLoadingLargeFile);
    if (orig_FinishLoadingLargeFile) {
      installed_count++;
      LOGC(LOGC_SYS, "[STREAM_HOOK] CStreaming::FinishLoadingLargeFile hooked at %p (orig trampoline %p)\n",
           (void *)largefile_addr, (void *)orig_FinishLoadingLargeFile);
    } else {
      /* Fall back to the plain resolved pointer so behaviour is unchanged if
       * the prologue cannot be relocated -- we simply stay blind to it. */
      orig_FinishLoadingLargeFile = (Func_FinishLoadingLargeFile)largefile_addr;
      LOGC(LOGC_SYS, "[STREAM_HOOK] REJECTED hook for CStreaming::FinishLoadingLargeFile at %p (unrelocatable prologue)\n",
           (void *)largefile_addr);
    }
  }

  init_prewarm_system();

  LOGC(LOGC_SYS, "[STREAM_HOOK] Resolved state machine: ms_channel=%p, ms_pStreamingBuffer=%p, ms_aInfoForModel=%p, AddModelsToRequestList=%p, StreamRadarSections=%p\n",
       (void *)g_pCStreaming_ms_channel, (void *)g_pCStreaming_ms_pStreamingBuffer, (void *)g_pCStreaming_ms_aInfoForModel,
       (void *)real_AddModelsToRequestList, (void *)real_StreamRadarSections);

  LOGC(LOGC_SYS, "[TRAMPOLINE] Pool usage: %zu / %d bytes (%d subsystem hooks installed)\n",
       game_mod.tramp_used, SO_TRAMPOLINE_SIZE, installed_count);
}

/* ---------------------------------------------------------------------------
 * Frontend trims
 *
 * Two things the mobile frontend does that make no sense on a console: it makes
 * you page through two screens of Take-Two copyright text every single launch,
 * and it offers Rockstar Social Club rows that this port can never satisfy.
 *
 * The menu table is shared by both. aScreens is an array of fixed-size screen
 * records; the sizes below were read out of this exact libGame.so rather than
 * from re3, whose CMenuScreen is a 32-bit PC layout and does not match:
 *
 *   screen record  412 bytes  (the 0x19c that CMenuManager::DrawFrontEnd,
 *                              ProcessButtonPresses and Process all multiply
 *                              m_nCurrScreen by)
 *   header          52 bytes  char m_ScreenName[8] then eleven int32s
 *   entry           20 bytes  int32 action, char name[8], int32 slot, int32 target
 *   rows per screen 18        (412 - 52) / 20
 *   screens         59        aScreens is 0x5ef4 bytes = 59 * 412
 *
 * Everything below re-checks the screen name it found before writing, so a
 * libGame.so with a different table is left alone rather than corrupted.
 * ------------------------------------------------------------------------- */

#define FE_SCREEN_STRIDE 412
#define FE_ENTRY_OFFSET   52
#define FE_NUM_ROWS       18
#define FE_NUM_SCREENS    59

typedef struct {
  int32_t action;
  char    name[8];
  int32_t save_slot;
  int32_t target;
} FeEntry;

_Static_assert(sizeof(FeEntry) == 20, "menu entry must match the 20-byte engine layout");

/* The Options page's two Rockstar Social Club rows, by GXT key: "Sign In"
 * (action 122) and "Delete Account" (action 123). Neither can do anything in a
 * port with no Social Club backing, and Delete Account without a sign-in is
 * doubly dead, so both come out.
 *
 * Each row is dropped by closing the gap behind it and blanking the freed tail
 * row. Shifting rather than blanking in place matters: the frontend stops at
 * the first empty row, so zeroing Sign In where it sat would also have hidden
 * everything below it, Main Menu included. */
static const char *const kDeadOptionRows[] = { "MM_SGI", "MM_DEL" };

static void frontend_remove_social_club_rows(void) {
  /* This edit lands during patch_game, before so_finalize() has mapped the
   * executable alias, so it has to go through load_base -- the same alias every
   * other one-shot patch here writes through. (The legal-screen globals below
   * are the mirror case: they are only touched at runtime, once load_base has
   * become Perm_None, so those use the load_virtbase alias instead.) */
  if (!so_try_find_addr_rx(&game_mod, "aScreens")) {
    debugPrintf("[FRONTEND] WARNING: aScreens symbol not found -- Social Club rows left in place\n");
    return;
  }
  uint8_t *screens = (uint8_t *)so_find_addr(&game_mod, "aScreens");

  for (int s = 0; s < FE_NUM_SCREENS; s++) {
    uint8_t *screen = screens + (size_t)s * FE_SCREEN_STRIDE;
    if (strncmp((const char *)screen, "FET_OPT", 8) != 0)
      continue;

    FeEntry *rows = (FeEntry *)(screen + FE_ENTRY_OFFSET);
    int removed = 0;
    for (size_t k = 0; k < sizeof(kDeadOptionRows) / sizeof(*kDeadOptionRows); k++) {
      for (int r = 0; r < FE_NUM_ROWS; r++) {
        if (strncmp(rows[r].name, kDeadOptionRows[k], 8) != 0)
          continue;
        memmove(&rows[r], &rows[r + 1], (size_t)(FE_NUM_ROWS - 1 - r) * sizeof(FeEntry));
        memset(&rows[FE_NUM_ROWS - 1], 0, sizeof(FeEntry));
        LOGC(LOGC_SYS, "[FRONTEND] Removed Options row '%s' at index %d (screen %d)\n",
             kDeadOptionRows[k], r, s);
        removed++;
        break;
      }
    }
    if (removed == 0)
      debugPrintf("[FRONTEND] WARNING: Options page has none of the Social Club rows -- nothing removed\n");
    return;
  }
  debugPrintf("[FRONTEND] WARNING: no FET_OPT screen in aScreens -- Social Club rows left in place\n");
}

/* The legal screens live inside CMenuManager::DrawFrontEndNormal, driven by
 * three file-scope globals:
 *
 *   legalScreenState  0 = the "Tap / Press A To Continue" title screen
 *                     1 = copyright page one    2 = page two    3 = done
 *   legalScreenSlerp  crossfade timer; ProcessButtonPresses sets it to 1.0 when
 *                     the player confirms, and the fade crossing 0.5 is what
 *                     advances the state
 *   shownLegalScreen  latched to 1 once state reaches 3, after which the draw
 *                     goes straight to the normal menu
 *
 * So the confirm press on the title screen is the moment the legal sequence
 * begins, and latching shownLegalScreen right there skips both copyright pages
 * without touching the title screen itself. Doing it in a wrapper around the
 * draw, rather than polling from the main loop, means the very frame that would
 * have shown page one already takes the "legal finished" branch. */
static uint8_t *g_shown_legal_screen = NULL;
static int32_t *g_legal_screen_state = NULL;
static float   *g_legal_screen_slerp = NULL;

typedef void (*Func_DrawFrontEndNormal)(void *this_ptr);
static Func_DrawFrontEndNormal orig_DrawFrontEndNormal = NULL;

static void wrap_DrawFrontEndNormal(void *this_ptr) {
  if (g_shown_legal_screen && g_legal_screen_state && g_legal_screen_slerp &&
      !*g_shown_legal_screen &&
      (*g_legal_screen_state != 0 || *g_legal_screen_slerp != 0.0f)) {
    *g_shown_legal_screen = 1;
    *g_legal_screen_state = 3;
    *g_legal_screen_slerp = 0.0f;
    LOGC(LOGC_SYS, "[FRONTEND] Skipped the legal/copyright pages\n");
  }
  if (orig_DrawFrontEndNormal) orig_DrawFrontEndNormal(this_ptr);
}

static void frontend_skip_legal_pages(void) {
  g_shown_legal_screen = (uint8_t *)so_try_find_addr_rx(&game_mod, "shownLegalScreen");
  g_legal_screen_state = (int32_t *)so_try_find_addr_rx(&game_mod, "legalScreenState");
  g_legal_screen_slerp = (float *)so_try_find_addr_rx(&game_mod, "legalScreenSlerp");
  if (!g_shown_legal_screen || !g_legal_screen_state || !g_legal_screen_slerp) {
    debugPrintf("[FRONTEND] WARNING: legal screen globals not found -- pages left in place\n");
    return;
  }

  uintptr_t addr = so_try_find_addr_rx(&game_mod, "_ZN12CMenuManager18DrawFrontEndNormalEv");
  if (!addr) {
    debugPrintf("[FRONTEND] WARNING: CMenuManager::DrawFrontEndNormal not found -- pages left in place\n");
    return;
  }
  orig_DrawFrontEndNormal =
      (Func_DrawFrontEndNormal)hook_arm64_trampoline(addr, (uintptr_t)wrap_DrawFrontEndNormal);
  if (orig_DrawFrontEndNormal) {
    LOGC(LOGC_SYS, "[FRONTEND] CMenuManager::DrawFrontEndNormal hooked at %p (legal pages will be skipped)\n",
         (void *)addr);
  } else {
    debugPrintf("[FRONTEND] WARNING: REJECTED hook for CMenuManager::DrawFrontEndNormal (unrelocatable prologue)\n");
  }
}

/* ---------------------------------------------------------------------------
 * Help-icon letters
 *
 * The pad follows the PS2 positions with A and B swapped back (see the table in
 * main.c), so CROSS/CIRCLE/SQUARE/TRIANGLE land on Switch A/B/Y/X. The help
 * icons would then be half wrong: CHIDJoystickXbox360::FindUVsFromMapping picks
 * a cell out of assets/es2/buttonsxbox360.png, and for the four face buttons it
 * uses the button ID directly as the atlas row -- 0 to A, 1 to B, 2 to X, 3 to
 * Y. CROSS and CIRCLE already agree; SQUARE would draw "X" while sitting under
 * Y, and TRIANGLE would draw "Y" while sitting under X.
 *
 * Two instructions assign that cell:
 *
 *   14ad0c  mov w10, #1     column 1, the face-button column
 *   14ad10  mov w12, w13    row = button ID
 *
 * and the function's dispatch table at 0xe2aa5 sends only IDs 0..3 there (4..13
 * are START/SELECT/shoulders/d-pad/sticks, each with its own case; 14 and above
 * fall through to a no-op), so this row assignment governs the face buttons and
 * nothing else.
 *
 * What is needed is 0->0, 1->1, 2->3, 3->2: leave A and B alone, swap X and Y.
 * On two-bit values that is "keep bit 1, flip bit 0 when bit 1 is set", which
 * `eor w12, w13, w13, lsr #1` computes in the single instruction slot available.
 *
 * The icons stay Xbox-styled, since that is the artwork the game ships; only
 * the letters are corrected.
 * ------------------------------------------------------------------------- */
static void patch_glyph_button_letters(void) {
  if (!so_try_find_addr_rx(&game_mod, "_ZN19CHIDJoystickXbox36018FindUVsFromMappingE10HIDMappingb11SpecialFlag")) {
    debugPrintf("[FRONTEND] WARNING: FindUVsFromMapping not found -- help icons will show the wrong letters\n");
    return;
  }
  uintptr_t fn = so_find_addr(&game_mod,
      "_ZN19CHIDJoystickXbox36018FindUVsFromMappingE10HIDMappingb11SpecialFlag");

  /* Offset 0x44 into the function is the `mov w12, w13`. Verify the exact
   * encoding before writing: if this libGame.so differs, leave it alone rather
   * than corrupt an instruction stream we have not understood. */
  uint32_t *insn = (uint32_t *)(fn + 0x44);
  const uint32_t expect_mov = 0x2a0d03ec; /* mov w12, w13              */
  const uint32_t patch_eor  = 0x4a4d05ac; /* eor w12, w13, w13, lsr #1 */

  if (*insn != expect_mov) {
    debugPrintf("[FRONTEND] WARNING: FindUVsFromMapping+0x44 is 0x%08x, expected 0x%08x -- "
                "help icon letters left unpatched\n", *insn, expect_mov);
    return;
  }
  *insn = patch_eor;
  LOGC(LOGC_SYS, "[FRONTEND] Help icon letters remapped for the Switch face layout (X<->Y)\n");
}

void patch_game(void) {
  debugPrintf("[HOOKS] Applying GTA III game patches and symbol hooks...\n");
  apply_streaming_memory_boost_staging();

  // 1. NVThread spawner & env lookup
  if (so_try_find_addr_rx(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_")) {
    hook_arm64(so_find_addr(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"),
               (uintptr_t)NVThreadSpawnJNIThread);
    debugPrintf("[HOOKS] Hooked NVThreadSpawnJNIThread successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: NVThreadSpawnJNIThread symbol not found\n");
  }

  if (so_try_find_addr_rx(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv"),
               (uintptr_t)NVThreadGetCurrentJNIEnv);
    debugPrintf("[HOOKS] Hooked NVThreadGetCurrentJNIEnv successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: NVThreadGetCurrentJNIEnv symbol not found\n");
  }

  // 1b. OS_Thread hardware abstraction hooks
  if (so_try_find_addr_rx(&game_mod, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority")) {
    hook_arm64(so_find_addr(&game_mod, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"), (uintptr_t)OS_ThreadLaunch_hook);
    debugPrintf("[HOOKS] Hooked OS_ThreadLaunch successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: OS_ThreadLaunch symbol not found\n");
  }

  if (so_try_find_addr_rx(&game_mod, "_Z13OS_ThreadWaitPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z13OS_ThreadWaitPv"), (uintptr_t)OS_ThreadWait_hook);
    debugPrintf("[HOOKS] Hooked OS_ThreadWait successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: OS_ThreadWait symbol not found\n");
  }

  if (so_try_find_addr_rx(&game_mod, "_Z18OS_ThreadIsRunningPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z18OS_ThreadIsRunningPv"), (uintptr_t)OS_ThreadIsRunning_hook);
    debugPrintf("[HOOKS] Hooked OS_ThreadIsRunning successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: OS_ThreadIsRunning symbol not found\n");
  }

  if (so_try_find_addr_rx(&game_mod, "_Z14OS_ThreadClosePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z14OS_ThreadClosePv"), (uintptr_t)OS_ThreadClose_hook);
    debugPrintf("[HOOKS] Hooked OS_ThreadClose successfully\n");
  } else {
    debugPrintf("[HOOKS] WARNING: OS_ThreadClose symbol not found\n");
  }

  // 1c. Engine assertions & debug break hook
  if (so_try_find_addr_rx(&game_mod, "_Z13OS_DebugBreakv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z13OS_DebugBreakv"), (uintptr_t)OS_DebugBreak_hook);
    debugPrintf("[HOOKS] Hooked OS_DebugBreak successfully\n");
  }

  // 2. Display resolution patches (inline 8-byte MOVZ W0, #imm; RET patches to avoid 16B trampoline collision)
  if (so_try_find_addr_rx(&game_mod, "_Z17OS_ScreenGetWidthv")) {
    uint32_t *p_w = (uint32_t *)so_find_addr(&game_mod, "_Z17OS_ScreenGetWidthv");
    p_w[0] = 0x52800000u | ((screen_width & 0xffff) << 5); // MOVZ W0, #screen_width
    p_w[1] = 0xd65f03c0u; // RET
    debugPrintf("[HOOKS] Patched OS_ScreenGetWidth inline (MOVZ W0, #%d; RET)\n", screen_width);
  }
  if (so_try_find_addr_rx(&game_mod, "_Z18OS_ScreenGetHeightv")) {
    uint32_t *p_h = (uint32_t *)so_find_addr(&game_mod, "_Z18OS_ScreenGetHeightv");
    p_h[0] = 0x52800000u | ((screen_height & 0xffff) << 5); // MOVZ W0, #screen_height
    p_h[1] = 0xd65f03c0u; // RET
    debugPrintf("[HOOKS] Patched OS_ScreenGetHeight inline (MOVZ W0, #%d; RET)\n", screen_height);
  }

  // 3. Install stack guard TLS on main thread
  if (!game_tls_install())
    debugPrintf("[HOOKS] WARNING: Failed to install main-thread game TLS\n");

  // 4. Disable nag screen / app ratings if present
  if (so_try_find_addr_rx(&game_mod, "_Z12Menu_ShowNagv")) {
    *(uint32_t *)so_find_addr(&game_mod, "_Z12Menu_ShowNagv") = 0xd65f03c0; // RET
    debugPrintf("[HOOKS] Patched Menu_ShowNag (disabled nag popup)\n");
  }

  // 5. Disable cloud save requirement if present
  if (so_try_find_addr_rx(&game_mod, "UseCloudSaves")) {
    *(uint8_t *)so_find_addr(&game_mod, "UseCloudSaves") = 0;
    LOGC(LOGC_SYS, "[HOOKS] Disabled UseCloudSaves flag\n");
  }

  // 5b. Resolve internal frame limiter flag
  CMenuManager_m_PrefsFrameLimiter = (uint8_t *)so_try_find_addr_rx(&game_mod, "_ZN12CMenuManager19m_PrefsFrameLimiterE");
  if (CMenuManager_m_PrefsFrameLimiter) {
    LOGC(LOGC_SYS, "[HOOKS] Resolved CMenuManager::m_PrefsFrameLimiter at %p\n", CMenuManager_m_PrefsFrameLimiter);
  } else {
    debugPrintf("[HOOKS] WARNING: _ZN12CMenuManager19m_PrefsFrameLimiterE symbol not found\n");
  }

  // 6. Hook Nvidia NvF File I/O functions to prevent stock NvFOpen NULL-faults
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFOpenPKc")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFOpenPKc"), (uintptr_t)NvFOpen_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFOpen successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z8NvFClosePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z8NvFClosePv"), (uintptr_t)NvFClose_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFClose successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFReadPvmmS_")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFReadPvmmS_"), (uintptr_t)NvFRead_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFRead successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFSeekPvli")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFSeekPvli"), (uintptr_t)NvFSeek_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFSeek successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFTellPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFTellPv"), (uintptr_t)NvFTell_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFTell successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFSizePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFSizePv"), (uintptr_t)NvFSize_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFSize successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFGetcPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFGetcPv"), (uintptr_t)NvFGetc_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFGetc successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z7NvFGetsPciPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z7NvFGetsPciPv"), (uintptr_t)NvFGets_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFGets successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z6NvFEOFPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z6NvFEOFPv"), (uintptr_t)NvFEOF_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked NvFEOF successfully\n");
  }

  // 7. Hook Rockstar OS_File abstraction layer
  if (so_try_find_addr_rx(&game_mod, "_Z11OS_FileOpen14OSFileDataAreaPPvPKc16OSFileAccessType")) {
    hook_arm64(so_find_addr(&game_mod, "_Z11OS_FileOpen14OSFileDataAreaPPvPKc16OSFileAccessType"), (uintptr_t)OS_FileOpen_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileOpen successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z12OS_FileClosePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z12OS_FileClosePv"), (uintptr_t)OS_FileClose_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileClose successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z11OS_FileReadPvS_i")) {
    hook_arm64(so_find_addr(&game_mod, "_Z11OS_FileReadPvS_i"), (uintptr_t)OS_FileRead_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileRead successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z12OS_FileWritePvPKvi")) {
    hook_arm64(so_find_addr(&game_mod, "_Z12OS_FileWritePvPKvi"), (uintptr_t)OS_FileWrite_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileWrite successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z11OS_FileSizePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z11OS_FileSizePv"), (uintptr_t)OS_FileSize_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileSize successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z18OS_FileSetPositionPvi")) {
    hook_arm64(so_find_addr(&game_mod, "_Z18OS_FileSetPositionPvi"), (uintptr_t)OS_FileSetPosition_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileSetPosition successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z18OS_FileGetPositionPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z18OS_FileGetPositionPv"), (uintptr_t)OS_FileGetPosition_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileGetPosition successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z15OS_FileGetStatePv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z15OS_FileGetStatePv"), (uintptr_t)OS_FileGetState_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileGetState successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z12OS_FileFlushPv")) {
    hook_arm64(so_find_addr(&game_mod, "_Z12OS_FileFlushPv"), (uintptr_t)OS_FileFlush_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileFlush successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z13OS_FileRename14OSFileDataAreaPKcS1_b")) {
    hook_arm64(so_find_addr(&game_mod, "_Z13OS_FileRename14OSFileDataAreaPKcS1_b"), (uintptr_t)OS_FileRename_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileRename successfully\n");
  }
  if (so_try_find_addr_rx(&game_mod, "_Z13OS_FileDelete14OSFileDataAreaPKc")) {
    hook_arm64(so_find_addr(&game_mod, "_Z13OS_FileDelete14OSFileDataAreaPKc"), (uintptr_t)OS_FileDelete_hook);
    LOGC(LOGC_SYS, "[HOOKS] Hooked OS_FileDelete successfully\n");
  }

  // Pre-initialize .bss / RsGlobal in load_base (writeable buffer before finalized mapping)
  uint8_t *base = (uint8_t *)game_mod.load_base;
  if (base) {
    // Try dynamic symbol resolution for RsGlobal
    uintptr_t rs_sym = so_try_find_addr_rx(&game_mod, "RsGlobal");
    uint8_t *RsGlobal = NULL;
    if (rs_sym && game_mod.load_virtbase) {
      RsGlobal = base + (rs_sym - (uintptr_t)game_mod.load_virtbase);
    } else {
      RsGlobal = base + 0x7e1418; // Fallback
    }

    if (RsGlobal) {
      *(int32_t *)(RsGlobal + 8)  = screen_width;  // maximumWidth (64-bit struct offset +8)
      *(int32_t *)(RsGlobal + 12) = screen_height; // maximumHeight (64-bit struct offset +12)
      LOGC(LOGC_SYS, "[RSGLOBAL] Initialized RsGlobal at %p (sym=%p) -> %dx%d\n", RsGlobal, (void*)rs_sym, screen_width, screen_height);
    }
  }

  setup_subsystem_phase_hooks();

  frontend_remove_social_club_rows();
  frontend_skip_legal_pages();
  patch_glyph_button_letters();

  set_thread_core(0);
  LOGC(LOGC_SYS, "[HOOKS] Finished applying GTA III game patches.\n");
}
