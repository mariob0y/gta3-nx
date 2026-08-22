/* libc_shim.c -- bionic-compatible libc wrappers and path mapping
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"
#include "path_cache.h"

// Fortify (_chk) shims
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
// Misc bionic shims
int __system_property_get_fake(const char *name, char *value) { (void)name; value[0] = '\0'; return 0; }
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }
int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178
long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID)
    return gettid_fake();
  debugPrintf("[LIBC] syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) {
  debugPrintf("\n======================================================\n");
  debugPrintf("[ABORT_MSG] Android abort message: %s\n", msg ? msg : "(null)");
  debugPrintf("======================================================\n");
}

void abort_fake(void) {
  void *caller = __builtin_return_address(0);
  debugPrintf("\n======================================================\n");
  debugPrintf("[ABORT] abort() called from %p!\n", caller);
  debugPrintf("======================================================\n");
  hard_exit();
}

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    case BIONIC_SC_PHYS_PAGES: return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default: debugPrintf("[LIBC] sysconf(%d) -> -1\n", name); return -1;
  }
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int __open_2_fake(const char *path, int flags) {
  return open_fake(path, flags);
}

int puts_fake(const char *str) {
  if (str) debugPrintf("[STDIO] puts: %s\n", str);
  return 0;
}

static void normalize_path_copy(const char *src, char *dst, size_t dst_size) {
  if (!src || !dst || dst_size == 0) return;
  size_t j = 0;
  while (src[j] == '/' || src[j] == '\\') j++;
  size_t k = 0;
  for (; src[j] != '\0' && k < dst_size - 1; j++) {
    dst[k++] = (src[j] == '\\') ? '/' : src[j];
  }
  dst[k] = '\0';
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  if (!path) return -1;
  char norm[1024];
  normalize_path_copy(path, norm, sizeof(norm));

  char lower[1024];
  size_t len = strlen(norm);
  for (size_t i = 0; i < len; i++) lower[i] = tolower((unsigned char)norm[i]);
  lower[len] = '\0';

  char alt[1024];
  int fd = -1;

  // 1. Try assets/ prefix first
  if (strncasecmp(norm, "assets/", 7) != 0) {
    snprintf(alt, sizeof(alt), "assets/%s", norm);
    fd = open(alt, convert_open_flags(flags), mode);
    if (fd >= 0) {
      LOGC(LOGC_FILE, "[FS] open_fake: '%s' -> fd=%d\n", alt, fd);
      return fd;
    }

    snprintf(alt, sizeof(alt), "assets/%s", lower);
    fd = open(alt, convert_open_flags(flags), mode);
    if (fd >= 0) {
      LOGC(LOGC_FILE, "[FS] open_fake: '%s' -> fd=%d\n", alt, fd);
      return fd;
    }

    const char *dot = strrchr(norm, '.');
    if (dot) {
      size_t base_len = dot - norm;
      snprintf(alt, sizeof(alt), "assets/%.*s_dxt%s", (int)base_len, lower, dot);
      fd = open(alt, convert_open_flags(flags), mode);
      if (fd >= 0) {
        LOGC(LOGC_FILE, "[FS] open_fake: '%s' (dxt) -> fd=%d\n", alt, fd);
        return fd;
      }
    }
  }

  // 2. Direct path fallback in root
  fd = open(norm, convert_open_flags(flags), mode);
  if (fd >= 0) {
    LOGC(LOGC_FILE, "[FS] open_fake: '%s' -> fd=%d\n", norm, fd);
    return fd;
  }

  fd = open(lower, convert_open_flags(flags), mode);
  if (fd >= 0) {
    LOGC(LOGC_FILE, "[FS] open_fake: '%s' (lower) -> fd=%d\n", lower, fd);
    return fd;
  }

  LOGC(LOGC_FILE, "[FS] open_fake: '%s' FAILED (flags=0x%x)\n", path, flags);
  return -1;
}

#define BIONIC_SOL_SOCKET 1
struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks; out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  if (!path) return -1;
  const char *clean_path = path;
  while (*clean_path == '/') clean_path++;

  struct stat s;
  int res = -1;
  if (strncasecmp(clean_path, "assets/", 7) != 0) {
    char alt[1024];
    snprintf(alt, sizeof(alt), "assets/%s", clean_path);
    res = stat(alt, &s);
  }
  if (res < 0) {
    res = stat(clean_path, &s);
  }
  if (res == 0) convert_stat(&s, st);
  return res;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat s;
  int res = fstat(fd, &s);
  if (res == 0) convert_stat(&s, st);
  return res;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
  return stat_fake(path, st);
}

void *readdir_fake(void *dirp) { return readdir(dirp); }
char *realpath_fake(const char *path, char *resolved) {
  if (resolved) { strncpy(resolved, path, 1024); return resolved; }
  return strdup(path);
}

int strerror_r_fake(int err, char *buf, size_t len) {
  const char *msg = strerror(err);
  if (!msg) return -1;
  strncpy(buf, msg, len);
  return 0;
}

int statvfs_fake(const char *path, void *buf) { (void)path; (void)buf; return 0; }

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)0x1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)0x1; }

int iswalpha_l_fake(int wc, void *loc) { (void)loc; return iswalpha(wc); }
int iswblank_l_fake(int wc, void *loc) { (void)loc; return iswblank(wc); }
int iswcntrl_l_fake(int wc, void *loc) { (void)loc; return iswcntrl(wc); }
int iswdigit_l_fake(int wc, void *loc) { (void)loc; return iswdigit(wc); }
int iswlower_l_fake(int wc, void *loc) { (void)loc; return iswlower(wc); }
int iswprint_l_fake(int wc, void *loc) { (void)loc; return iswprint(wc); }
int iswpunct_l_fake(int wc, void *loc) { (void)loc; return iswpunct(wc); }
int iswspace_l_fake(int wc, void *loc) { (void)loc; return iswspace(wc); }
int iswupper_l_fake(int wc, void *loc) { (void)loc; return iswupper(wc); }
int iswxdigit_l_fake(int wc, void *loc) { (void)loc; return iswxdigit(wc); }
int towlower_l_fake(int wc, void *loc) { (void)loc; return towlower(wc); }
int towupper_l_fake(int wc, void *loc) { (void)loc; return towupper(wc); }
int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) { (void)ps; return mbsrtowcs(dst, src, len, NULL); }
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) { (void)ps; return wcsrtombs(dst, src, len, NULL); }

int isdigit_l_fake(int c, void *loc) { (void)loc; return isdigit(c); }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int islower_l_fake(int c, void *loc) { (void)loc; return islower(c); }
int isupper_l_fake(int c, void *loc) { (void)loc; return isupper(c); }
int toupper_l_fake(int c, void *loc) { (void)loc; return toupper(c); }
int tolower_l_fake(int c, void *loc) { (void)loc; return tolower(c); }

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *ptr = memalign(align, size);
  if (!ptr) return ENOMEM;
  *out = ptr;
  return 0;
}

uint8_t fake_sF[3][0x100];

static int is_fake_stream(FILE *f) {
  const uintptr_t p = (uintptr_t)f;
  const uintptr_t base = (uintptr_t)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_stream(f)) return n;
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_stream(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) { if (is_fake_stream(f)) return c; return fputc(c, f); }
int fflush_fake(FILE *f) { if (is_fake_stream(f)) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_stream(f)) return 0; return fclose(f); }
int ferror_fake(FILE *f) { if (is_fake_stream(f)) return 0; return ferror(f); }
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_stream(f)) return 0;
  va_list va; va_start(va, fmt); int res = vfprintf(f, fmt, va); va_end(va);
  return res;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_stream(f)) return 0;
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_stream(f)) return 0; return fseek(f, off, whence); }
int getc_fake(FILE *f) { if (is_fake_stream(f)) return EOF; return getc(f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_stream(f)) return c; return ungetc(c, f); }
static FILE *try_fopen_path(const char *path) {
  if (!path) return NULL;
  char clean[1024];
  size_t len = strlen(path);
  if (len >= sizeof(clean)) len = sizeof(clean) - 1;

  // 1. Normalize backslashes to forward slashes and strip leading slashes
  size_t j = 0;
  while (j < len && (path[j] == '/' || path[j] == '\\')) j++;
  size_t k = 0;
  for (; j < len; j++) {
    clean[k++] = (path[j] == '\\') ? '/' : path[j];
  }
  clean[k] = '\0';

  char lower[1024];
  for (size_t i = 0; i <= k; i++) {
    lower[i] = tolower((unsigned char)clean[i]);
  }

  // 2. Try assets/ prefix first
  char alt[1024];
  FILE *f = NULL;
  if (strncasecmp(clean, "assets/", 7) != 0) {
    snprintf(alt, sizeof(alt), "assets/%s", clean);
    f = fopen(alt, "rb");
    if (f) return f;

    snprintf(alt, sizeof(alt), "assets/%s", lower);
    f = fopen(alt, "rb");
    if (f) return f;
  } else {
    f = fopen(clean, "rb");
    if (f) return f;

    f = fopen(lower, "rb");
    if (f) return f;
  }

  // 3. Fallback to direct path in root
  f = fopen(clean, "rb");
  if (f) return f;

  f = fopen(lower, "rb");
  if (f) return f;

  return NULL;
}

FILE *open_asset_with_fallback(const char *path) {
  if (!path) return NULL;

  const char *fast_path = path_cache_lookup(path);
  if (fast_path) {
    FILE *f = fopen(fast_path, "rb");
    if (f) return f;
  }

  FILE *f = try_fopen_path(path);
  if (f) return f;

  char dxt_path[1024];
  const char *dot = strrchr(path, '.');
  if (dot) {
    size_t base_len = dot - path;
    snprintf(dxt_path, sizeof(dxt_path), "%.*s_dxt%s", (int)base_len, path, dot);
    f = try_fopen_path(dxt_path);
    if (f) {
      LOGC(LOGC_FILE, "[AASSET] Suffix redirection: '%s' -> '%s'\n", path, dxt_path);
      return f;
    }
  }

  LOGC(LOGC_FILE, "[AASSET] WARNING: Could not find asset file '%s'\n", path);
  return NULL;
}

FILE *fopen_fake(const char *path, const char *mode) {
  void *caller = __builtin_return_address(0);
  LOGC(LOGC_FILE, "[FS] fopen_fake: '%s' (mode=%s, caller=%p)\n", path ? path : "(null)", mode ? mode : "", caller);
  if (mode && (mode[0] == 'r')) {
    FILE *f = open_asset_with_fallback(path);
    if (f) return f;
  }
  return fopen(path, mode);
}

typedef struct {
  FILE *f;
  long len;
  void *buffer;
} FakeAAsset;

void *AAssetManager_fromJava_fake(void *env, void *mgr) { (void)env; (void)mgr; return (void *)0x1; }

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  void *caller = __builtin_return_address(0);
  LOGC(LOGC_FILE, "[AASSET] AAssetManager_open('%s', caller=%p)\n", path ? path : "(null)", caller);
  FILE *f = open_asset_with_fallback(path);
  if (!f) return NULL;

  setvbuf(f, NULL, _IOFBF, 16 * 1024);
  FakeAAsset *a = calloc(1, sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = ftell(f);
  fseek(f, 0, SEEK_SET);
  LOGC(LOGC_FILE, "[AASSET] Asset '%s' opened -> handle=%p (len=%ld, caller=%p)\n", path, a, a->len, caller);
  return a;
}

void AAsset_close_fake(void *a) {
  void *caller = __builtin_return_address(0);
  LOGC(LOGC_FILE, "[AASSET] AAsset_close(handle=%p, caller=%p)...\n", a, caller);
  if (!a) return;
  FakeAAsset *asset = (FakeAAsset *)a;
  if (asset->buffer) free(asset->buffer);
  if (asset->f) fclose(asset->f);
  free(asset);
  LOGC(LOGC_FILE, "[AASSET] AAsset_close finished (caller=%p)\n", caller);
}

int AAsset_read_fake(void *a, void *buf, size_t count) {
  if (!a) return -1;
  FakeAAsset *asset = (FakeAAsset *)a;
  return (int)fread(buf, 1, count, asset->f);
}

long AAsset_seek_fake(void *a, long off, int whence) {
  if (!a) return -1;
  FakeAAsset *asset = (FakeAAsset *)a;
  fseek(asset->f, off, whence);
  return ftell(asset->f);
}

int64_t AAsset_seek64_fake(void *a, int64_t off, int whence) {
  return AAsset_seek_fake(a, (long)off, whence);
}

long AAsset_getLength_fake(void *a) {
  if (!a) return 0;
  return ((FakeAAsset *)a)->len;
}

int64_t AAsset_getLength64_fake(void *a) {
  return AAsset_getLength_fake(a);
}

long AAsset_getRemainingLength_fake(void *a) {
  if (!a) return 0;
  FakeAAsset *asset = (FakeAAsset *)a;
  long cur = ftell(asset->f);
  return asset->len - cur;
}

int64_t AAsset_getRemainingLength64_fake(void *a) {
  return AAsset_getRemainingLength_fake(a);
}

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  debugPrintf("[NWINDOW] ANativeWindow_fromSurface_fake -> %p (%dx%d)\n", win, screen_width, screen_height);
  return win;
}
int ANativeWindow_getWidth_fake(void *win) { (void)win; return screen_width; }
int ANativeWindow_getHeight_fake(void *win) { (void)win; return screen_height; }
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) { (void)win; (void)w; (void)h; (void)format; return 0; }

typedef struct {
  int count;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *sem = malloc(sizeof(*sem));
  sem->count = value;
  *s = sem;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) { free(*s); *s = NULL; }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s) ((FakeSem *)*s)->count++;
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s) {
    FakeSem *sem = (FakeSem *)*s;
    while (sem->count <= 0) svcSleepThread(1000000ULL);
    sem->count--;
  }
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s) {
    FakeSem *sem = (FakeSem *)*s;
    if (sem->count > 0) { sem->count--; return 0; }
  }
  errno = EAGAIN;
  return -1;
}

int sem_getvalue_fake(void **s, int *val) {
  if (s && *s && val) *val = ((FakeSem *)*s)->count;
  return 0;
}

int pthread_rwlock_rdlock_fake(void **rw) { (void)rw; return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { (void)rw; return 0; }
int pthread_rwlock_unlock_fake(void **rw) { (void)rw; return 0; }
int pthread_attr_getstacksize_fake(const void *attr, size_t *size) { (void)attr; if (size) *size = 1024 * 1024; return 0; }
int pthread_attr_getschedparam_fake(const void *attr, void *param) { (void)attr; (void)param; return 0; }
