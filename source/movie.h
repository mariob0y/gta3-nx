/* movie.h -- intro movie playback for the GTA III Switch port
 *
 * The engine's boot state machine already asks for movies/Logo.mpg.m4v and
 * movies/GTAtitles.mpg.m4v; on Android those calls landed on a Java
 * MediaPlayer. These entry points stand in for it. Every one of them is safe
 * to call when there is no movie, no decoder and no assets/movies directory:
 * movie_play() then simply reports failure and movie_is_playing() stays false,
 * which makes the engine's wait state fall straight through.
 */

#ifndef __MOVIE_H__
#define __MOVIE_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* engine_path is the path the engine passed to OS_MoviePlay, backslashes and
 * all (e.g. "movies\\Logo.mpg.m4v"). Returns false if nothing will play. */
bool movie_play(const char *engine_path);

/* True while a movie is still on screen. Backs the isMoviePlaying() JNI call
 * the engine polls once per frame. */
bool movie_is_playing(void);

/* Tears the player down. Idempotent; called by the engine's OS_MovieStop and
 * again at shutdown. */
void movie_stop(void);

/* Presents the current video frame over the game's framebuffer. Called from
 * the eglSwapBuffers hook, which is the only place the GL context is known to
 * be current. A no-op unless a movie is playing. */
void movie_render(void *display, void *surface);

/* True when there is something new to put on screen: a freshly decoded frame,
 * or enough time since the last present that the surface should be refreshed
 * anyway. The video runs at 25 fps while the main loop turns over at 60, so
 * without this the player would swap the same picture out two or three times
 * per decoded frame for no benefit. */
bool movie_should_present(void);

#ifdef __cplusplus
}
#endif

#endif
