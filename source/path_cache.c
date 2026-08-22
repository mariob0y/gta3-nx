/* path_cache.c -- fast case-insensitive asset path resolution cache
 */

#include "path_cache.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define PATH_CACHE_CAPACITY 4096

typedef struct {
  char key[256];   // Lowercase clean relative path
  char value[256]; // Exact disk path
  bool occupied;
} path_entry_t;

static path_entry_t g_path_table[PATH_CACHE_CAPACITY];
static bool g_path_cache_initialized = false;

static uint32_t hash_string(const char *str) {
  uint32_t hash = 5381;
  int c;
  while ((c = (unsigned char)*str++)) {
    hash = ((hash << 5) + hash) + c;
  }
  return hash;
}

static void normalize_key(const char *src, char *dst, size_t dst_size) {
  while (*src == '/' || *src == '\\' || (src[0] == '.' && (src[1] == '/' || src[1] == '\\'))) {
    if (src[0] == '.' && (src[1] == '/' || src[1] == '\\')) {
      src += 2;
    } else {
      src++;
    }
  }

  size_t i = 0;
  for (; src[i] && i < dst_size - 1; i++) {
    char c = src[i];
    if (c == '\\') c = '/';
    dst[i] = (char)tolower((unsigned char)c);
  }
  dst[i] = '\0';
}

static void insert_cache_entry(const char *key, const char *value) {
  if (!key || !key[0] || !value || !value[0]) return;

  char norm_key[256];
  normalize_key(key, norm_key, sizeof(norm_key));

  uint32_t idx = hash_string(norm_key) & (PATH_CACHE_CAPACITY - 1);
  for (size_t i = 0; i < PATH_CACHE_CAPACITY; i++) {
    uint32_t slot = (idx + i) & (PATH_CACHE_CAPACITY - 1);
    if (!g_path_table[slot].occupied) {
      strncpy(g_path_table[slot].key, norm_key, sizeof(g_path_table[slot].key) - 1);
      strncpy(g_path_table[slot].value, value, sizeof(g_path_table[slot].value) - 1);
      g_path_table[slot].occupied = true;
      return;
    }
    if (strcmp(g_path_table[slot].key, norm_key) == 0) {
      // Key already exists; preserve first inserted
      return;
    }
  }
}

static void scan_directory_recursive(const char *dir_path) {
  DIR *d = opendir(dir_path);
  if (!d) return;

  struct dirent *entry;
  char full_path[512];

  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    struct stat st;
    if (stat(full_path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        scan_directory_recursive(full_path);
      } else if (S_ISREG(st.st_mode)) {
        // Register full path
        insert_cache_entry(full_path, full_path);

        // If path starts with assets/, also index without assets/ prefix
        if (strncasecmp(full_path, "assets/", 7) == 0) {
          insert_cache_entry(full_path + 7, full_path);
        }

        // Handle _dxt fallback aliasing (e.g. models/gta3_dxt.img -> models/gta3.img).
        // Tried swapping this to "_unc" (uncompressed) to test whether the CPU
        // cost measured inside the shared RenderWare texture loader was
        // specific to the DXT code path -- it wasn't (measured: identical
        // per-model TXD costs, just 2x the RAM/boot-preload time for the
        // uncompressed archives). Reverted back to "_dxt".
        const char *dot = strrchr(entry->d_name, '.');
        if (dot) {
          size_t name_len = dot - entry->d_name;
          if (name_len > 4 && strncasecmp(dot - 4, "_dxt", 4) == 0) {
            char base_alias[512];
            size_t dir_len = strlen(dir_path);
            size_t base_name_len = name_len - 4;
            snprintf(base_alias, sizeof(base_alias), "%.*s/%.*s%s",
                     (int)dir_len, dir_path, (int)base_name_len, entry->d_name, dot);
            insert_cache_entry(base_alias, full_path);
            if (strncasecmp(base_alias, "assets/", 7) == 0) {
              insert_cache_entry(base_alias + 7, full_path);
            }
          }
        }
      }
    }
  }
  closedir(d);
}

void path_cache_init(void) {
  memset(g_path_table, 0, sizeof(g_path_table));
  LOGC(LOGC_SYS, "[PATH_CACHE] Indexing asset directory tree...\n");

  const char *roots[] = { "assets", "data", "models", "audio", "anim", "TEXT", "." };
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    struct stat st;
    if (stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode)) {
      if (strcmp(roots[i], ".") == 0) {
        // Scan current directory non-recursively for top-level files
        DIR *d = opendir(".");
        if (d) {
          struct dirent *entry;
          while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            struct stat fst;
            if (stat(entry->d_name, &fst) == 0 && S_ISREG(fst.st_mode)) {
              insert_cache_entry(entry->d_name, entry->d_name);
            }
          }
          closedir(d);
        }
      } else {
        scan_directory_recursive(roots[i]);
      }
    }
  }

  g_path_cache_initialized = true;
  LOGC(LOGC_SYS, "[PATH_CACHE] Asset path index built successfully.\n");
}

const char *path_cache_lookup(const char *requested_path) {
  if (!g_path_cache_initialized || !requested_path || !requested_path[0])
    return NULL;

  char norm_key[256];
  normalize_key(requested_path, norm_key, sizeof(norm_key));

  uint32_t idx = hash_string(norm_key) & (PATH_CACHE_CAPACITY - 1);
  for (size_t i = 0; i < PATH_CACHE_CAPACITY; i++) {
    uint32_t slot = (idx + i) & (PATH_CACHE_CAPACITY - 1);
    if (!g_path_table[slot].occupied) {
      return NULL; // Fast-path miss fallthrough
    }
    if (strcmp(g_path_table[slot].key, norm_key) == 0) {
      return g_path_table[slot].value;
    }
  }

  return NULL;
}
