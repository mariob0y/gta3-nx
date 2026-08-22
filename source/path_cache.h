/* path_cache.h -- fast case-insensitive asset path resolution cache
 */

#ifndef PATH_CACHE_H
#define PATH_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

void path_cache_init(void);
const char *path_cache_lookup(const char *requested_path);

#ifdef __cplusplus
}
#endif

#endif // PATH_CACHE_H
