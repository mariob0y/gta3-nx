/* stream_cache.h -- background read-ahead prefetch cache for streaming assets
 */

#ifndef STREAM_CACHE_H
#define STREAM_CACHE_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sc_init(void);
void sc_shutdown(void);

void sc_open(void *handle, const char *resolved_path, FILE *main_f, const char *mode);
void sc_close(void *handle);
size_t sc_read(void *handle, FILE *main_f, void *buf, size_t count);
int sc_debug_active_slots(void);

typedef struct {
  uint8_t *data;
  long size;
  char path[256];
} sc_preload_t;

bool sc_is_preload_archive(const char *path);
sc_preload_t *sc_get_or_load_preload(const char *resolved_path, long *out_size);
void sc_preload_warm_all(void);
size_t sc_preload_read(sc_preload_t *p, void *buf, size_t count, long *io_pos);
void sc_preload_seek(sc_preload_t *p, long offset, int whence, long *io_pos);

#ifdef __cplusplus
}
#endif

#endif // STREAM_CACHE_H
