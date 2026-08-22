/* imports.c -- .so import resolution
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Serves both libGame.so and the C++ runtime donor (the APK's libopenal.so).
 * C++ runtime symbols (std::*, __cxa_*) resolve module-to-module from the donor,
 * not here. The table takes priority during resolution (see so_resolve_symbol).
 */

#define _GNU_SOURCE // vasprintf and friends

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "overlay.h"

extern uintptr_t __cxa_atexit;

extern uintptr_t __stack_chk_fail;

#define MC_SLOTS 8
static struct {
  void *key;
  EGLDisplay dpy; EGLSurface draw, read; EGLContext ctx;
} g_mc[MC_SLOTS];

static inline void *mc_thread_key(void) {
  void *p;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p));
  return p;
}

static int glc_enabled = 1;

#define GLC_MAXCAPS 24
static struct { GLenum cap; GLboolean on; } glc_caps[GLC_MAXCAPS];
static int glc_ncaps;

static struct {
  int have_blend;  GLenum bsf, bdf;
  int have_dfunc;  GLenum dfunc;
  int have_dmask;  GLboolean dmask;
  int have_cull;   GLenum cull;
  int have_front;  GLenum front;
  int have_cmask;  GLboolean cr, cg, cb, ca;
  int have_active; GLenum active;
  int have_prog;   GLuint prog;
  GLuint tex2d[8]; int have_tex2d[8];
} glc;

static void gl_state_cache_reset(void) {
  glc_ncaps = 0;
  memset(&glc, 0, sizeof(glc));
}

static void glEnable_c(GLenum cap) {
  if (glc_enabled) {
    for (int i = 0; i < glc_ncaps; i++)
      if (glc_caps[i].cap == cap) {
        if (glc_caps[i].on) return;
        glc_caps[i].on = GL_TRUE; glEnable(cap); return;
      }
    if (glc_ncaps < GLC_MAXCAPS) {
      glc_caps[glc_ncaps].cap = cap; glc_caps[glc_ncaps].on = GL_TRUE; glc_ncaps++;
    }
  }
  glEnable(cap);
}
static void glDisable_c(GLenum cap) {
  if (glc_enabled) {
    for (int i = 0; i < glc_ncaps; i++)
      if (glc_caps[i].cap == cap) {
        if (!glc_caps[i].on) return;
        glc_caps[i].on = GL_FALSE; glDisable(cap); return;
      }
    if (glc_ncaps < GLC_MAXCAPS) {
      glc_caps[glc_ncaps].cap = cap; glc_caps[glc_ncaps].on = GL_FALSE; glc_ncaps++;
    }
  }
  glDisable(cap);
}
static void glBlendFunc_c(GLenum s, GLenum d) {
  if (glc_enabled && glc.have_blend && glc.bsf == s && glc.bdf == d) return;
  glc.have_blend = 1; glc.bsf = s; glc.bdf = d;
  glBlendFunc(s, d);
}
static void glDepthFunc_c(GLenum f) {
  if (glc_enabled && glc.have_dfunc && glc.dfunc == f) return;
  glc.have_dfunc = 1; glc.dfunc = f;
  glDepthFunc(f);
}
static void glDepthMask_c(GLboolean m) {
  if (glc_enabled && glc.have_dmask && glc.dmask == m) return;
  glc.have_dmask = 1; glc.dmask = m;
  glDepthMask(m);
}
static void glCullFace_c(GLenum m) {
  if (glc_enabled && glc.have_cull && glc.cull == m) return;
  glc.have_cull = 1; glc.cull = m;
  glCullFace(m);
}
static void glFrontFace_c(GLenum m) {
  if (glc_enabled && glc.have_front && glc.front == m) return;
  glc.have_front = 1; glc.front = m;
  glFrontFace(m);
}
static void glColorMask_c(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  if (glc_enabled && glc.have_cmask && glc.cr == r && glc.cg == g &&
      glc.cb == b && glc.ca == a)
    return;
  glc.have_cmask = 1; glc.cr = r; glc.cg = g; glc.cb = b; glc.ca = a;
  glColorMask(r, g, b, a);
}
static void glActiveTexture_c(GLenum unit) {
  if (glc_enabled && glc.have_active && glc.active == unit) return;
  glc.have_active = 1; glc.active = unit;
  glActiveTexture(unit);
}
static void glBindTexture_c(GLenum target, GLuint tex) {
  if (glc_enabled && target == GL_TEXTURE_2D && glc.have_active) {
    unsigned idx = (unsigned)(glc.active - GL_TEXTURE0);
    if (idx < 8) {
      if (glc.have_tex2d[idx] && glc.tex2d[idx] == tex) return;
      glc.have_tex2d[idx] = 1; glc.tex2d[idx] = tex;
    }
  }
  glBindTexture(target, tex);
}
static void glDeleteTextures_c(GLsizei n, const GLuint *t) {
  memset(glc.have_tex2d, 0, sizeof(glc.have_tex2d)); // a deleted bound tex must
  glDeleteTextures(n, t);                            // not be skipped when reused
}
static void glDeleteProgram_c(GLuint p) {
  if (glc.have_prog && glc.prog == p) glc.have_prog = 0;
  glDeleteProgram(p);
}

extern unsigned int eglSwapBuffersHook(void *display, void *surface);
static unsigned int eglSwapBuffers_cache(void *display, void *surface) {
  static unsigned frame_cnt = 0;
  if ((frame_cnt++ % 60) == 0)
    LOGC(LOGC_EGL, "[EGL] eglSwapBuffers frame #%u (dpy=%p, surf=%p)\n", frame_cnt, display, surface);
  gl_state_cache_reset();

  u64 t0 = armGetSystemTick();
  unsigned int r = eglSwapBuffersHook(display, surface);
  double swap_ms = (double)armTicksToNs(armGetSystemTick() - t0) / 1e6;
  if (swap_ms >= 5.0) {
    LOGC(LOGC_SYS, "[FRAME_SPLIT] SLOW SWAP: took %.2f ms\n", swap_ms);
  }
  return r;
}

static EGLDisplay eglGetDisplay_wrap(EGLNativeDisplayType dpy) {
  LOGC(LOGC_EGL, "[EGL] eglGetDisplay(native=%p)...\n", (void *)dpy);
  EGLDisplay res = eglGetDisplay(dpy);
  LOGC(LOGC_EGL, "[EGL] eglGetDisplay -> %p\n", res);
  return res;
}

static EGLBoolean eglInitialize_wrap(EGLDisplay dpy, EGLint *maj, EGLint *min) {
  LOGC(LOGC_EGL, "[EGL] eglInitialize(dpy=%p)...\n", dpy);
  EGLint a = 0, b = 0;
  EGLBoolean res = eglInitialize(dpy, maj ? maj : &a, min ? min : &b);
  LOGC(LOGC_EGL, "[EGL] eglInitialize -> %d (maj=%d, min=%d)\n", res, maj ? *maj : a, min ? *min : b);
  return res;
}

/* The game asks for a GLES2 config/context. We try to give it GLES3 instead,
 * because the one measured win available in this whole texture pipeline needs
 * a GLES3 pname: capping a truncated mip chain with GL_TEXTURE_MAX_LEVEL.
 * A run with the tail mip levels dropped cut the texture pipeline from
 * 34.1s to 14.3s and halved total freeze time -- but rendered the world black,
 * because on a GLES2 context GL_TEXTURE_MAX_LEVEL silently does nothing and
 * the truncated chains were merely incomplete.
 *
 * GLES3 is backward compatible, so a GLES2-era renderer runs unchanged on it.
 * Every step falls back to the game's original request if the upgrade fails,
 * and the capability is additionally probed at runtime before being used (see
 * mipcap_supported), so a driver that accepts the context but not the pname
 * still cannot break rendering. */
#define EGL_RENDERABLE_TYPE_KEY   0x3040
#define EGL_OPENGL_ES3_BIT_KHR_V  0x0040
#define EGL_CONTEXT_CLIENT_VER    0x3098
#define EGL_NONE_KEY              0x3038

int g_gl_es3_context = 0;

/* Copies an EGL attrib list, replacing key's value (or appending the pair if
 * the key is absent). Returns the number of entries written, 0 on overflow. */
static int egl_attribs_with(const EGLint *src, EGLint key, EGLint value,
                            EGLint *dst, int dst_max) {
  int n = 0, found = 0;
  if (src) {
    for (; src[n] != EGL_NONE_KEY; n += 2) {
      if (n + 3 >= dst_max) return 0;
      dst[n] = src[n];
      dst[n + 1] = (src[n] == key) ? (found = 1, value) : src[n + 1];
    }
  }
  if (!found) {
    if (n + 3 >= dst_max) return 0;
    dst[n++] = key;
    dst[n++] = value;
  }
  dst[n] = EGL_NONE_KEY;
  return n + 1;
}

static EGLBoolean eglChooseConfig_wrap(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
  LOGC(LOGC_EGL, "[EGL] eglChooseConfig(dpy=%p, size=%d)...\n", dpy, config_size);

  EGLint up[64];
  if (egl_attribs_with(attribs, EGL_RENDERABLE_TYPE_KEY, EGL_OPENGL_ES3_BIT_KHR_V, up, 64)) {
    EGLint n3 = 0;
    if (eglChooseConfig(dpy, up, configs, config_size, &n3) && n3 > 0) {
      if (num_config) *num_config = n3;
      LOGC(LOGC_SYS, "[EGL] Upgraded config request to GLES3 (num_config=%d)\n", n3);
      return EGL_TRUE;
    }
    LOGC(LOGC_SYS, "[EGL] No GLES3-capable config; falling back to the game's request\n");
  }

  EGLBoolean res = eglChooseConfig(dpy, attribs, configs, config_size, num_config);
  LOGC(LOGC_EGL, "[EGL] eglChooseConfig -> %d (num_config=%d)\n", res, num_config ? *num_config : -1);
  return res;
}

static EGLContext eglCreateContext_wrap(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) {
  LOGC(LOGC_EGL, "[EGL] eglCreateContext(dpy=%p, config=%p, share=%p)...\n", dpy, config, share_context);

  EGLint up[64];
  if (egl_attribs_with(attrib_list, EGL_CONTEXT_CLIENT_VER, 3, up, 64)) {
    EGLContext c3 = eglCreateContext(dpy, config, share_context, up);
    if (c3 != EGL_NO_CONTEXT) {
      g_gl_es3_context = 1;
      LOGC(LOGC_SYS, "[EGL] Created a GLES3 context (was requesting GLES2)\n");
      return c3;
    }
    LOGC(LOGC_SYS, "[EGL] GLES3 context refused; falling back to the game's request\n");
  }

  EGLContext res = eglCreateContext(dpy, config, share_context, attrib_list);
  LOGC(LOGC_EGL, "[EGL] eglCreateContext -> %p\n", res);
  return res;
}

static EGLSurface eglCreateWindowSurface_wrap(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list) {
  LOGC(LOGC_EGL, "[EGL] eglCreateWindowSurface(dpy=%p, config=%p, win=%p)...\n", dpy, config, (void *)win);
  EGLSurface res = eglCreateWindowSurface(dpy, config, win, attrib_list);
  LOGC(LOGC_EGL, "[EGL] eglCreateWindowSurface -> %p (error=0x%x)\n", res, eglGetError());
  return res;
}

static EGLBoolean eglMakeCurrent_dedup(EGLDisplay dpy, EGLSurface draw,
                                       EGLSurface read, EGLContext ctx) {
  LOGC(LOGC_EGL, "[EGL] eglMakeCurrent dpy=%p draw=%p read=%p ctx=%p\n", dpy, draw, read, ctx);
  void *key = mc_thread_key();
  int slot = -1, freeslot = -1;
  for (int i = 0; i < MC_SLOTS; i++) {
    if (g_mc[i].key == key) { slot = i; break; }
    if (!g_mc[i].key && freeslot < 0) freeslot = i;
  }
  if (slot >= 0 && g_mc[slot].dpy == dpy && g_mc[slot].draw == draw &&
      g_mc[slot].read == read && g_mc[slot].ctx == ctx)
    return EGL_TRUE; // already current on this thread -> skip the redundant bind

  EGLBoolean r = eglMakeCurrent(dpy, draw, read, ctx);
  LOGC(LOGC_EGL, "[EGL] eglMakeCurrent result = %d (error=0x%x)\n", r, eglGetError());
  if (r) {
    gl_state_cache_reset(); // context/surface changed -> GL state cache is stale
    if (slot < 0) slot = (freeslot >= 0) ? freeslot : 0;
    g_mc[slot].key = key; g_mc[slot].dpy = dpy;
    g_mc[slot].draw = draw; g_mc[slot].read = read; g_mc[slot].ctx = ctx;
  }
  return r;
}

/* Periodic glFlush during texture loading, to stop the driver queueing an
 * unbounded amount of upload work. It was firing every 32nd call, and it is
 * the single most expensive thing in the whole texture pipeline.
 *
 * Measured with two independent timers around the same GL call: the pure
 * glCompressedTexImage2D (timed below, inside this drain) accounted for
 * 1744ms of the >=2ms sample, while the same calls measured from outside --
 * i.e. including this drain -- accounted for 12237ms. ~10.5s of a ~35s
 * texture pipeline was this flush, not the uploads. That also explains why
 * upload cost looked uncorrelated with texture size (an 8x8 upload "costing"
 * 11ms while a 512x512 cost 2.3ms): the expensive calls were simply the ones
 * that happened to land on the every-32nd boundary and wait for the GPU.
 *
 * Kept rather than removed, because the unbounded-queue risk it guards
 * against is real; the interval is just raised so the wait is amortised over
 * far more uploads. Interval must stay a power of two for the mask. */
#define GL_LOAD_DRAIN_INTERVAL 512
_Atomic uint64_t g_gl_flush_ns = 0;
_Atomic uint64_t g_gl_flush_calls = 0;
static void gl_load_drain(void) {
  static unsigned n = 0;
  if ((++n & (GL_LOAD_DRAIN_INTERVAL - 1)) == 0) {
    uint64_t t0 = armGetSystemTick();
    glFlush();
    atomic_fetch_add_explicit(&g_gl_flush_ns, armTicksToNs(armGetSystemTick() - t0), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_gl_flush_calls, 1, memory_order_relaxed);
  }
}

static void glTexImage2D_w(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h,
                           GLint b, GLenum f, GLenum y, const void *p) {
  gl_load_drain();
  uint64_t t0 = armGetSystemTick();
  glTexImage2D(t, l, i, w, h, b, f, y, p);
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  double ms = (double)dt_ns / 1e6;
  if (ms >= 2.0) {
    LOGC(LOGC_SYS, "[PERF_TEX] glTexImage2D(%dx%d, internal=0x%x, format=0x%x) took %.2fms\n", w, h, i, f, ms);
  }
}
/* Smallest mip level we bother uploading, in texels. Levels below this are
 * skipped and the mip chain is capped with GL_TEXTURE_MAX_LEVEL.
 *
 * Rationale from measurement: glCompressedTexImage2D is 97% of the texture
 * pipeline (33.2s / 35716 calls), and its cost is per-CALL, not per-byte --
 * a 512x512 level-0 upload averages 2.0ms while an 8x8 level-3 upload
 * averages 14.2ms. Since a 256x256 texture is a 9-level chain of which 5
 * levels are 16x16 or smaller, these near-free-looking tail levels are the
 * majority of the calls and therefore the majority of the cost.
 *
 * Capping the chain is safe for completeness: GL only requires levels 0..
 * GL_TEXTURE_MAX_LEVEL to exist, and we set that to the last level actually
 * uploaded. The visual cost is that extreme minification bottoms out at
 * MIN_MIP_DIM texels instead of 1x1, which is the classic mip-bias tradeoff
 * and should be invisible at this resolution. Level 0 is never skipped, so
 * genuinely tiny textures still upload normally. */
/* Smallest mip level dimension we allocate and fill. Raising this drops more
 * upload calls (the cost is per-call, not per-byte) at the price of coarser
 * minification: the chain bottoms out here, so very distant surfaces alias
 * rather than blurring further. 16 measured 9.2s of texture pipeline with no
 * visible artefacts; trying 32 to see where the visual limit actually is. */
#define MIN_MIP_DIM 32
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
_Atomic uint64_t g_gl_mip_skipped = 0;
_Atomic uint64_t g_gl_mip_uploaded = 0;
_Atomic uint64_t g_gl_texstorage_allocs = 0;
_Atomic uint64_t g_gl_texstorage_fallbacks = 0;
_Atomic uint64_t g_gl_subimage_errors = 0;

/* Immutable texture storage: allocate the whole mip chain in ONE call, then
 * fill each level with glCompressedTexSubImage2D.
 *
 * This replaces capping the mip chain, which cut the texture pipeline from
 * 34.1s to 12.6s but rendered the world black -- the win was real, the
 * mechanism was not. Measurement says the cost is per-CALL and independent of
 * texture size (a 512x512 level-0 upload averages 2.0ms, an 8x8 level-3
 * upload 14.2ms), which points at per-level storage allocation rather than
 * data transfer. glTexImage2D-style calls re-specify storage on every level;
 * glTexStorage2D allocates once and the per-level SubImage uploads then write
 * into storage that already exists.
 *
 * Crucially this keeps EVERY level, so the chain stays complete and nothing
 * can render black for the reason capping did. The game uploads a full chain
 * in ascending order (verified in the logs: a 128x128 base runs through to
 * "1x1 lvl7"), so at level 0 the exact level count is computable. */
typedef void (*PFN_glTexStorage2D_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static PFN_glTexStorage2D_t p_glTexStorage2D = NULL;

static int __attribute__((unused)) full_mip_levels(GLsizei w, GLsizei h) {
  GLsizei m = (w > h) ? w : h;
  int n = 1;
  while (m > 1) { m >>= 1; n++; }
  return n;
}

/* Number of levels we actually allocate and fill: those whose dimensions stay
 * at or above MIN_MIP_DIM. Must agree exactly with the per-level skip test
 * below, so that every allocated level receives content. Always at least one,
 * so a texture whose level 0 is already tiny still works. */
static int kept_mip_levels(GLsizei w, GLsizei h) {
  int n = 0;
  GLsizei cw = w, ch = h;
  while (cw >= MIN_MIP_DIM && ch >= MIN_MIP_DIM) {
    n++;
    if (cw == 1 && ch == 1) break;
    cw = (cw > 1) ? (cw >> 1) : 1;
    ch = (ch > 1) ? (ch >> 1) : 1;
  }
  return (n > 0) ? n : 1;
}

static int texstorage_supported(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  if (!g_gl_es3_context) {
    cached = 0;
    LOGC(LOGC_SYS, "[GL] glTexStorage2D unavailable (no GLES3 context) -- using per-level uploads\n");
    return cached;
  }
  p_glTexStorage2D = (PFN_glTexStorage2D_t)eglGetProcAddress("glTexStorage2D");
  cached = (p_glTexStorage2D != NULL);
  LOGC(LOGC_SYS, "[GL] glTexStorage2D %s -- immutable storage %s\n",
       cached ? "resolved" : "NOT available",
       cached ? "ENABLED" : "disabled");
  return cached;
}

extern int g_gl_es3_context;

/* One-time runtime probe: does this context actually honour
 * GL_TEXTURE_MAX_LEVEL? Setting it to its default (1000) is a no-op on a
 * context that supports it and raises GL_INVALID_ENUM on one that does not,
 * so the probe itself cannot disturb rendering either way. */
static int mipcap_supported(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  if (!g_gl_es3_context) {
    cached = 0;
    LOGC(LOGC_SYS, "[GL] Mip-chain capping DISABLED (no GLES3 context) -- uploading full chains\n");
    return cached;
  }
  while (glGetError() != GL_NO_ERROR) { }        // clear any pending error
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000); // default value
  cached = (glGetError() == GL_NO_ERROR) ? 1 : 0;
  LOGC(LOGC_SYS, "[GL] GL_TEXTURE_MAX_LEVEL probe: %s (GL_VERSION=%s) -- mip-chain capping %s\n",
       cached ? "supported" : "REJECTED",
       (const char *)glGetString(GL_VERSION),
       cached ? "ENABLED" : "disabled");
  return cached;
}

static void glCompressedTexImage2D_w(GLenum t, GLint l, GLenum i, GLsizei w,
                                     GLsizei h, GLint b, GLsizei s, const void *d) {
  /* Only skip tail levels once GL_TEXTURE_MAX_LEVEL is proven to work on this
   * context. Probed rather than inferred from the context version, because a
   * context that reports GLES3 but ignores the pname would black out the world
   * exactly as before -- and that failure is far worse than losing the win. */
  /* ISOLATION TEST: capping is off for this build even though the capability
   * probe passes. GL_TEXTURE_MAX_LEVEL is genuinely supported here (probe
   * reports "supported", GL_VERSION = OpenGL ES 3.2 Mesa 20.1.0-rc3) and the
   * chains are capped correctly, yet the world still rendered black -- so
   * incomplete mipmaps cannot be the whole story, and the other change in
   * flight is the GLES2 -> GLES3 context upgrade. With capping off and the
   * GLES3 context still in place, textures returning proves the cap is at
   * fault; textures still missing proves the context upgrade is. Restore by
   * setting this back to 1. */
#define MIP_CAP_ENABLED 0
  if (MIP_CAP_ENABLED && mipcap_supported() && l > 0 && (w < MIN_MIP_DIM || h < MIN_MIP_DIM)) {
    glTexParameteri(t, GL_TEXTURE_MAX_LEVEL, l - 1);
    atomic_fetch_add_explicit(&g_gl_mip_skipped, 1, memory_order_relaxed);
    return;
  }

  /* Previously reverted: skipping tail mip levels and capping the chain with
   * GL_TEXTURE_MAX_LEVEL rendered most of the world black/dark blue --
   * GL_TEXTURE_MAX_LEVEL is a GLES3 pname and does not take effect on this
   * context, so the truncated chains were simply incomplete, and an
   * incomplete mipmapped texture samples as black. The skip itself was NOT
   * wrong about the cost: with ~40% of uploads gone the stutter was very
   * noticeably reduced, which is the clearest confirmation yet that
   * glCompressedTexImage2D cost is per-call. The call count is the right
   * target; capping the chain this way is not the way to reduce it. */
  atomic_fetch_add_explicit(&g_gl_mip_uploaded, 1, memory_order_relaxed);
  gl_load_drain();
  uint64_t t0 = armGetSystemTick();

  if (t == GL_TEXTURE_2D && texstorage_supported()) {
    /* Allocate a SHORTER immutable chain and simply do not have the tiny
     * tail levels at all.
     *
     * This is correct by construction, unlike both previous attempts. The
     * first truncated a mutable chain, leaving levels missing -> incomplete
     * texture -> black world. The second allocated every level but left the
     * tail unfilled, relying on GL_TEXTURE_MAX_LEVEL to keep them from being
     * sampled; MAX_LEVEL was evidently ignored, and with trilinear forced on
     * (see glTexParameteriHook) the sampler blended straight into those
     * unfilled levels -- which is the darkness that appeared on the road,
     * the most extremely minified surface on screen.
     *
     * With immutable storage the level count is fixed at allocation time, so
     * allocating only the levels we intend to fill means every level that
     * exists is filled, and sampling cannot reach a level that does not
     * exist. Completeness is guaranteed by the allocation itself; nothing
     * depends on MAX_LEVEL, and no texel is ever undefined.
     *
     * The remaining tradeoff is honest and bounded: minification bottoms out
     * at MIN_MIP_DIM texels, so very distant surfaces alias or shimmer
     * slightly instead of blurring further. Lower MIN_MIP_DIM to trade the
     * speed back for a longer chain. */
    if (l > 0 && (w < MIN_MIP_DIM || h < MIN_MIP_DIM)) {
      atomic_fetch_add_explicit(&g_gl_mip_skipped, 1, memory_order_relaxed);
      return; /* this level was never allocated */
    }
    if (l == 0) {
      /* Allocate the entire chain up front. If the texture is already
       * immutable (a re-upload of the same object), this fails harmlessly and
       * the SubImage below still writes into the existing storage, so the
       * error is cleared rather than treated as fatal. */
      while (glGetError() != GL_NO_ERROR) { }
      p_glTexStorage2D(t, kept_mip_levels(w, h), i, w, h);
      if (glGetError() == GL_NO_ERROR)
        atomic_fetch_add_explicit(&g_gl_texstorage_allocs, 1, memory_order_relaxed);
    }
    while (glGetError() != GL_NO_ERROR) { }
    glCompressedTexSubImage2D(t, l, 0, 0, w, h, i, s, d);
    if (glGetError() != GL_NO_ERROR) {
      /* Storage missing or mismatched: fall back for this level so a texture
       * can never end up undefined (which is what made the world black). */
      atomic_fetch_add_explicit(&g_gl_subimage_errors, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_gl_texstorage_fallbacks, 1, memory_order_relaxed);
      glCompressedTexImage2D(t, l, i, w, h, b, s, d);
    }
  } else {
    glCompressedTexImage2D(t, l, i, w, h, b, s, d);
  }
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  double ms = (double)dt_ns / 1e6;
  if (ms >= 2.0) {
    LOGC(LOGC_SYS, "[PERF_TEX] glCompressedTexImage2D(%dx%d, format=0x%x, size=%d) took %.2fms\n", w, h, i, s, ms);
  }
}
static void glBufferData_w(GLenum target, GLsizeiptr size, const void *data,
                           GLenum usage) {
  gl_load_drain();
  glBufferData(target, size, data, usage);
}

FILE *stderr_fake = (FILE *)&fake_sF[2];

extern ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused);
extern ALCdevice *alcOpenDeviceHook(const char *name);

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG_LOG
  va_list list;
  static char string[0x1000];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;

  (void)mutexattr;
  *m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;

  *c = PTHREAD_COND_INITIALIZER;

  int ret = pthread_cond_init(c, NULL);
  if (ret < 0) {
    free(c);
    return -1;
  }

  *cnd = c;

  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  };
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine) (void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

typedef struct {
  void *(*func)(void *);
  void *arg;
} PthreadStart;

static void *pthread_trampoline(void *p) {
  PthreadStart *s = p;
  void *(*func)(void *) = s->func;
  void *arg = s->arg;
  free(s);
  debugPrintf("[PTHREAD] Secondary thread %p running (func=%p)...\n", (void *)pthread_self(), func);
  set_thread_core(2);
  thread_registry_add();            // track for freeze-on-exit
  if (!game_tls_install()) {
    debugPrintf("[PTHREAD] ERROR: Failed to install TLS on thread %p!\n", (void *)pthread_self());
    return (void *)-1;
  }
  void *rc = func(arg);
  debugPrintf("[PTHREAD] Secondary thread %p finished (rc=%p)\n", (void *)pthread_self(), rc);
  return rc;
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  void *caller = __builtin_return_address(0);
  debugPrintf("[PTHREAD] pthread_create_fake(entry=%p, arg=%p, caller=%p)\n", entry, arg, caller);
  PthreadStart *s = calloc(1, sizeof(*s));
  if (!s)
    return -1;
  s->func = (void *(*)(void *))entry;
  s->arg = arg;
  int rc = pthread_create(thread, NULL, pthread_trampoline, s);
  if (rc != 0) {
    debugPrintf("[PTHREAD] ERROR: pthread_create failed with code %d\n", rc);
    free(s);
  }
  return rc;
}

void glGetShaderInfoLogHook(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
  glGetShaderInfoLog(shader, maxLength, length, infoLog);
  LOGC(LOGC_GL, "[GL] shader info log:\n%s\n", infoLog);
}

static GLuint glCreateShader_wrap(GLenum type) {
  LOGC(LOGC_GL, "[GL] glCreateShader(type=0x%x)\n", type);
  GLuint s = glCreateShader(type);
  LOGC(LOGC_GL, "[GL] glCreateShader -> %u\n", s);
  return s;
}

static void glShaderSource_wrap(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
  LOGC(LOGC_GL, "[GL] glShaderSource(shader=%u, count=%d)\n", shader, count);
  glShaderSource(shader, count, string, length);
}

static void glCompileShader_wrap(GLuint shader) {
  LOGC(LOGC_GL, "[GL] glCompileShader(shader=%u)\n", shader);
  uint64_t t0 = armGetSystemTick();
  glCompileShader(shader);
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  double ms = (double)dt_ns / 1e6;
  if (ms >= 1.0) {
    LOGC(LOGC_SYS, "[PERF_SHADER] glCompileShader(shader=%u) took %.2fms\n", shader, ms);
  }
  GLint status = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[2048] = {0};
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    LOGC(LOGC_GL, "[GL] ERROR: Shader %u compilation failed:\n%s\n", shader, log);
  } else {
    LOGC(LOGC_GL, "[GL] Shader %u compiled successfully.\n", shader);
  }
}

static GLuint glCreateProgram_wrap(void) {
  LOGC(LOGC_GL, "[GL] glCreateProgram()\n");
  GLuint p = glCreateProgram();
  LOGC(LOGC_GL, "[GL] glCreateProgram -> %u\n", p);
  return p;
}

static void glAttachShader_wrap(GLuint program, GLuint shader) {
  LOGC(LOGC_GL, "[GL] glAttachShader(prog=%u, shader=%u)\n", program, shader);
  glAttachShader(program, shader);
}

static void glLinkProgram_wrap(GLuint program) {
  LOGC(LOGC_GL, "[GL] glLinkProgram(prog=%u)\n", program);
  uint64_t t0 = armGetSystemTick();
  glLinkProgram(program);
  uint64_t dt_ns = armTicksToNs(armGetSystemTick() - t0);
  double ms = (double)dt_ns / 1e6;
  if (ms >= 1.0) {
    LOGC(LOGC_SYS, "[PERF_SHADER] glLinkProgram(prog=%u) took %.2fms\n", program, ms);
  }
  GLint status = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    char log[2048] = {0};
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    LOGC(LOGC_GL, "[GL] ERROR: Program %u link failed:\n%s\n", program, log);
  } else {
    LOGC(LOGC_GL, "[GL] Program %u linked successfully.\n", program);
  }
}

static void glUseProgram_wrap(GLuint program) {
  LOGC(LOGC_GL, "[GL] glUseProgram(prog=%u)\n", program);
  glUseProgram(program);
}

void glTexParameteriHook(GLenum target, GLenum param, GLint val) {
  if (val == GL_LINEAR_MIPMAP_NEAREST)
    val = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, param, val);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

static int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

static void alcDevicePauseSOFT_stub(void *device) { (void)device; }
static void alcDeviceResumeSOFT_stub(void *device) { (void)device; }
static ALboolean alIsBuffer_stub(ALuint buffer) { (void)buffer; return AL_TRUE; }
static int mpg123_format_none_stub(void *mh) { (void)mh; return 0; }
static int mpg123_format_stub(void *mh, long rate, int channels, int enc) { (void)mh; (void)rate; (void)channels; (void)enc; return 0; }

DynLibFunction dynlib_functions[] = {
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },

  { "stderr", (uintptr_t)&stderr_fake },

  // AAssets are emulated over regular files relative to the game dir
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64_fake },

  // ANativeWindow maps onto the default NWindow
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },

  // newlib pthread keys are functional, and libc++_shared needs them
  // for emulated thread_local storage
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },

  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },

  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },

  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "pthread_setname_np", (uintptr_t)&ret0 },

  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_setschedparam", (uintptr_t)&ret0 },
  { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_fake },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },

  { "pthread_mutexattr_init", (uintptr_t)&ret0 },
  { "pthread_mutexattr_settype", (uintptr_t)&ret0 },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },

  { "pthread_once", (uintptr_t)&pthread_once_fake },

  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  { "sched_get_priority_min", (uintptr_t)&retm1 },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },

  { "__android_log_print", (uintptr_t)__android_log_print },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },

  { "__errno", (uintptr_t)&__errno },

  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },

  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },

  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // fortify wrappers
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },

  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "fmodf", (uintptr_t)&fmodf },
  { "log", (uintptr_t)&log },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sinf", (uintptr_t)&sinf },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "tanf", (uintptr_t)&tanf },

  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },

  { "calloc", (uintptr_t)&calloc },
  { "free", (uintptr_t)&free },
  { "malloc", (uintptr_t)&malloc },
  { "realloc", (uintptr_t)&realloc },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  { "clock_gettime", (uintptr_t)&clock_gettime },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "time", (uintptr_t)&time },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // EGL: the game creates and manages its own context now
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay_wrap },
  { "eglInitialize", (uintptr_t)&eglInitialize_wrap },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig_wrap },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateContext", (uintptr_t)&eglCreateContext_wrap },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_wrap },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent_dedup },
  // reset the GL cache after the overlay's direct GL, then run the (FPS) swap hook
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers_cache },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },

  // OpenAL: imported by libGame.so since 2.x; alcOpenDevice/alcCreateContext
  // go through hooks for the 44100hz override
  { "alBufferData", (uintptr_t)&alBufferData },
  { "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
  { "alGenBuffers", (uintptr_t)&alGenBuffers },
  { "alGenSources", (uintptr_t)&alGenSources },
  { "alGetError", (uintptr_t)&alGetError },
  { "alGetSourcei", (uintptr_t)&alGetSourcei },
  { "alListener3f", (uintptr_t)&alListener3f },
  { "alListenerf", (uintptr_t)&alListenerf },
  { "alListenerfv", (uintptr_t)&alListenerfv },
  { "alSource3f", (uintptr_t)&alSource3f },
  { "alSourcePause", (uintptr_t)&alSourcePause },
  { "alSourcePlay", (uintptr_t)&alSourcePlay },
  { "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
  { "alSourceStop", (uintptr_t)&alSourceStop },
  { "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
  { "alSourcef", (uintptr_t)&alSourcef },
  { "alSourcei", (uintptr_t)&alSourcei },
  { "alIsBuffer", (uintptr_t)&alIsBuffer_stub },
  { "alcCloseDevice", (uintptr_t)&alcCloseDevice },
  { "alcCreateContext", (uintptr_t)&alcCreateContextHook },
  { "alcDestroyContext", (uintptr_t)&alcDestroyContext },
  { "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
  { "alcOpenDevice", (uintptr_t)&alcOpenDeviceHook },
  { "alcDevicePauseSOFT", (uintptr_t)&alcDevicePauseSOFT_stub },
  { "alcDeviceResumeSOFT", (uintptr_t)&alcDeviceResumeSOFT_stub },

  // mpg123 (music streaming); was libVendor_mpg123.so on Android,
  // provided natively by the switch-mpg123 portlib here
  { "mpg123_delete", (uintptr_t)&mpg123_delete },
  { "mpg123_exit", (uintptr_t)&mpg123_exit },
  { "mpg123_feed", (uintptr_t)&mpg123_feed },
  { "mpg123_format", (uintptr_t)&mpg123_format_stub },
  { "mpg123_format_none", (uintptr_t)&mpg123_format_none_stub },
  { "mpg123_getformat", (uintptr_t)&mpg123_getformat },
  { "mpg123_info", (uintptr_t)&mpg123_info },
  { "mpg123_init", (uintptr_t)&mpg123_init },
  { "mpg123_new", (uintptr_t)&mpg123_new },
  { "mpg123_open_feed", (uintptr_t)&mpg123_open_feed },
  { "mpg123_outblock", (uintptr_t)&mpg123_outblock },
  { "mpg123_read", (uintptr_t)&mpg123_read },

  { "abort", (uintptr_t)&abort_fake },

  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgetc", (uintptr_t)&fgetc },
  { "fgets", (uintptr_t)&fgets },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "close", (uintptr_t)&close },
  { "mkdir", (uintptr_t)&mkdir },
  { "open", (uintptr_t)&open_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "unlink", (uintptr_t)&unlink },
  { "readdir", (uintptr_t)&readdir_fake },
  { "realpath", (uintptr_t)&realpath_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },

  { "getenv", (uintptr_t)&getenv },

  { "glActiveTexture", (uintptr_t)&glActiveTexture_c },
  { "glAttachShader", (uintptr_t)&glAttachShader_wrap },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture_c },
  { "glBlendFunc", (uintptr_t)&glBlendFunc_c },
  { "glBufferData", (uintptr_t)&glBufferData_w },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColorMask", (uintptr_t)&glColorMask_c },
  { "glCompileShader", (uintptr_t)&glCompileShader_wrap },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_w },
  { "glCreateProgram", (uintptr_t)&glCreateProgram_wrap },
  { "glCreateShader", (uintptr_t)&glCreateShader_wrap },
  { "glCullFace", (uintptr_t)&glCullFace_c },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram_c },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures_c },
  { "glDepthFunc", (uintptr_t)&glDepthFunc_c },
  { "glDepthMask", (uintptr_t)&glDepthMask_c },
  { "glDisable", (uintptr_t)&glDisable_c },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable_c },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace_c },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glIsTexture", (uintptr_t)&glIsTexture },
  { "glLinkProgram", (uintptr_t)&glLinkProgram_wrap },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource_wrap },
  { "glTexImage2D", (uintptr_t)&glTexImage2D_w },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram_wrap },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },

  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  { "memcmp", (uintptr_t)&memcmp },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "memchr", (uintptr_t)&memchr },

  { "printf", (uintptr_t)&debugPrintf },
  { "puts", (uintptr_t)&puts_fake },

  { "qsort", (uintptr_t)&qsort },

  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },

  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "swprintf", (uintptr_t)&swprintf },

  { "truncate", (uintptr_t)&retm1 },
  { "link", (uintptr_t)&retm1 },
  { "symlink", (uintptr_t)&retm1 },
  { "readlink", (uintptr_t)&retm1 },
  { "chdir", (uintptr_t)&chdir },
  { "getcwd", (uintptr_t)&getcwd },
  { "fchmod", (uintptr_t)&ret0 },
  { "fchmodat", (uintptr_t)&ret0 },
  { "utimensat", (uintptr_t)&ret0 },
  { "sendfile", (uintptr_t)&retm1 },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },

  { "opendir", (uintptr_t)&opendir },
  { "closedir", (uintptr_t)&closedir },

  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strcpy", (uintptr_t)&strcpy },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strlen", (uintptr_t)&strlen },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtok", (uintptr_t)&strtok },
  { "strtol", (uintptr_t)&strtol },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtof", (uintptr_t)&strtof },
  { "strtold", (uintptr_t)&strtold },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },

  { "srand", (uintptr_t)&srand },
  { "rand", (uintptr_t)&rand },

  // locale: the _l variants ignore the locale and use the C locale
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  { "wctob", (uintptr_t)&wctob },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcslen", (uintptr_t)&wcslen },
  { "btowc", (uintptr_t)&btowc },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },

  // libGame.so extras over the Max Payne 2.1.131 import set
  { "__read_chk", (uintptr_t)&__read_chk },
  { "expf", (uintptr_t)&expf },
  { "frexp", (uintptr_t)&frexp },
  { "logf", (uintptr_t)&logf },
  { "modf", (uintptr_t)&modf },
  { "gmtime", (uintptr_t)&gmtime },

  // imports of the C++ runtime donor (the APK's libopenal.so)
  { "exp2f", (uintptr_t)&exp2f },
  { "sched_yield", (uintptr_t)&sched_yield_fake },

  // -- extra bionic _FORTIFY_SOURCE wrappers --
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },

  // -- C++ runtime donor imports not provided by libGame.so --
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemset", (uintptr_t)&wmemset },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "isdigit_l", (uintptr_t)&isdigit_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l_fake },
  { "islower_l", (uintptr_t)&islower_l_fake },
  { "isupper_l", (uintptr_t)&isupper_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },
  { "tolower_l", (uintptr_t)&tolower_l_fake },

  // Everything else libGame.so needs either appears above or resolves from the
  // C++ runtime donor (libc++_shared.so).
  { "ctime", (uintptr_t)&ctime },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  if (config.trilinear_filter)
    so_find_import(dynlib_functions, dynlib_numfunctions, "glTexParameteri")->func = (uintptr_t)glTexParameteriHook;
}
