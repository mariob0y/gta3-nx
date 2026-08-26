/* movie.c -- intro movie playback (H.264 video + AAC audio) for gta3_nx
 *
 * The Android build reaches its intro movies through JNI: OS_MoviePlay calls
 * playMovie(path, skippable), OS_MovieIsPlaying polls isMoviePlaying() and
 * OS_MovieStop calls stopMovie(), all of which were fielded by a Java
 * MediaPlayer. jni_fake.c routes those three names here instead.
 *
 * Layout of the player:
 *
 *   - One decode thread owns libavformat/libavcodec. It demuxes, decodes, and
 *     paces itself against a clock, publishing exactly one video frame at a
 *     time into a mutex-protected staging buffer.
 *   - The GL thread (inside the eglSwapBuffers hook, the one place the context
 *     is guaranteed current) picks up whatever frame is published and draws it.
 *     It never touches ffmpeg state.
 *   - Audio goes out through OpenAL on the decode thread, in its own source and
 *     buffer queue, independent of the engine's audio. Decoded samples land in
 *     a three-second ring first and are handed to OpenAL only when a buffer is
 *     free, so the thread never waits on playback. That matters more than it
 *     looks: an MP4 hands out its streams in chunks, so a run of audio packets
 *     decodes seconds ahead of where the speaker is, and a thread stalled
 *     waiting for a buffer is a thread not decoding video. Blocking here is what
 *     held the logo to a third of its frame rate for the first ten seconds.
 *
 * The clock is the system tick rather than the audio device. Both are derived
 * from the same oscillator, so drift over a 90-second title sequence is far
 * below a frame; the failure mode worth guarding is the decoder falling behind,
 * which shows up as a starved audio queue and is logged.
 *
 * Everything here is best-effort by design: the port must run on an install
 * that has no assets/movies at all. Any failure -- missing file, missing
 * stream, decoder that will not open -- is logged, torn down, and reported as
 * "not playing", which makes the engine's wait state fall through to the next
 * boot step exactly as if the movie had finished.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#include "movie.h"
#include "config.h"
#include "util.h"
#include "libc_shim.h"

/* 44100 matches the rate alcCreateContextHook forces on the shared context, so
 * OpenAL never has to resample our stream. */
#define MOVIE_AUDIO_RATE    44100
#define MOVIE_AL_BUFFERS    8
#define MOVIE_AL_FRAMES     2048   /* samples per buffer, ~46 ms each */
#define MOVIE_IO_BUFSZ      (64 * 1024)
/* Three seconds of stereo surplus. An MP4 interleaves its streams in chunks, so
 * a run of audio packets routinely decodes seconds ahead of playback; this is
 * where that surplus waits instead of blocking the decode thread. */
#define MOVIE_PCM_FRAMES    (MOVIE_AUDIO_RATE * 3)

typedef struct {
  atomic_bool playing;
  atomic_bool stop_request;

  pthread_t thread;
  bool thread_valid;

  /* --- decode thread only --- */
  FILE *fp;
  AVIOContext *avio;
  AVFormatContext *fmt;
  AVCodecContext *vdec, *adec;
  struct SwrContext *swr;
  int vstream, astream;
  ALCcontext *al_ctx;
  ALCdevice *al_own_dev;   /* non-NULL when the movie opened its own device */
  ALCcontext *al_own_ctx;
  ALuint al_source;
  ALuint al_buffer[MOVIE_AL_BUFFERS];
  ALuint al_free[MOVIE_AL_BUFFERS];  /* stack of buffer names not in the queue */
  int al_free_count;
  int al_queued;
  int16_t *al_scratch;
  int16_t *pcm_ring;          /* MOVIE_PCM_FRAMES stereo frames */
  int pcm_head, pcm_tail, pcm_count;

  /* --- shared staging buffer --- */
  uint8_t *plane[3];
  int plane_pitch[3];
  int width, height;
  bool frame_ready;   /* a frame has been published at least once */
  bool frame_dirty;   /* the published frame has not been uploaded yet */
} Movie;

static Movie g_mv;

/* Guards the staging planes between the decode thread and the GL thread. It
 * lives outside the struct and is never destroyed, so movie_play()'s reset of
 * the struct and a teardown racing a draw can never leave it in a bad state. */
static pthread_mutex_t g_mv_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- *
 * AVIO glue over the port's own asset resolver
 *
 * ffmpeg's own file protocol would bypass path_cache, which is what makes
 * "movies\Logo.mpg.m4v" find "assets/movies/logo.mpg.m4v" on a case-sensitive
 * SD card. Feeding it a FILE* from open_asset_with_fallback keeps movie lookup
 * identical to every other asset the port opens.
 * ------------------------------------------------------------------------- */

static int mv_io_read(void *opaque, uint8_t *buf, int size) {
  FILE *f = (FILE *)opaque;
  size_t n = fread(buf, 1, (size_t)size, f);
  if (n == 0)
    return ferror(f) ? AVERROR(EIO) : AVERROR_EOF;
  return (int)n;
}

static int64_t mv_io_seek(void *opaque, int64_t offset, int whence) {
  FILE *f = (FILE *)opaque;
  if (whence == AVSEEK_SIZE) {
    long cur = ftell(f);
    if (cur < 0 || fseek(f, 0, SEEK_END) != 0) return AVERROR(EIO);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return (int64_t)end;
  }
  if (fseek(f, (long)offset, whence) != 0)
    return AVERROR(EIO);
  return (int64_t)ftell(f);
}

/* ------------------------------------------------------------------------- *
 * Teardown
 * ------------------------------------------------------------------------- */

static void mv_free_planes(Movie *m) {
  for (int i = 0; i < 3; i++) {
    free(m->plane[i]);
    m->plane[i] = NULL;
    m->plane_pitch[i] = 0;
  }
  m->width = m->height = 0;
  m->frame_ready = false;
  m->frame_dirty = false;
}

static void mv_audio_close(Movie *m) {
  if (m->al_source) {
    alSourceStop(m->al_source);
    /* Detaching the whole queue in one go; individually unqueueing a source
     * that was stopped mid-buffer is not reliable across implementations. */
    alSourcei(m->al_source, AL_BUFFER, 0);
    alDeleteSources(1, &m->al_source);
    m->al_source = 0;
  }
  if (m->al_buffer[0]) {
    alDeleteBuffers(MOVIE_AL_BUFFERS, m->al_buffer);
    memset(m->al_buffer, 0, sizeof(m->al_buffer));
  }
  m->al_queued = 0;
  m->al_free_count = 0;
  free(m->al_scratch);
  m->al_scratch = NULL;
  free(m->pcm_ring);
  m->pcm_ring = NULL;
  m->pcm_head = m->pcm_tail = m->pcm_count = 0;

  /* Hand a private device back promptly: the engine brings up its own audio
   * right after the intros, and it should find no context current. */
  if (m->al_own_ctx) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(m->al_own_ctx);
    m->al_own_ctx = NULL;
  }
  if (m->al_own_dev) {
    alcCloseDevice(m->al_own_dev);
    m->al_own_dev = NULL;
  }
  m->al_ctx = NULL;
}

/* Releases everything the decode thread owns. Only ever called on the decode
 * thread, or on the main thread once that thread has been joined. */
static void mv_teardown(Movie *m) {
  mv_audio_close(m);
  if (m->swr) swr_free(&m->swr);
  if (m->vdec) avcodec_free_context(&m->vdec);
  if (m->adec) avcodec_free_context(&m->adec);
  if (m->fmt) avformat_close_input(&m->fmt);
  if (m->avio) {
    av_freep(&m->avio->buffer);
    avio_context_free(&m->avio);
  }
  if (m->fp) { fclose(m->fp); m->fp = NULL; }
  m->vstream = m->astream = -1;
}

/* ------------------------------------------------------------------------- *
 * Audio
 * ------------------------------------------------------------------------- */

static bool mv_audio_open(Movie *m) {
  /* If the engine already has a context, a second source inside it is all we
   * need. It usually does not: the intros play at boot states 5..8, and the
   * engine's audio does not exist until CGame::InitialiseOnceAfterRW at state 9.
   * That is why the first cut played silent. Open a private device for the
   * duration instead -- both intros are finished and mv_audio_close() has handed
   * it back well before the engine opens its own. */
  m->al_ctx = alcGetCurrentContext();
  if (!m->al_ctx) {
    m->al_own_dev = alcOpenDevice(NULL);
    if (m->al_own_dev) {
      const ALCint attr[] = { ALC_FREQUENCY, MOVIE_AUDIO_RATE, 0 };
      m->al_own_ctx = alcCreateContext(m->al_own_dev, attr);
      if (m->al_own_ctx && alcMakeContextCurrent(m->al_own_ctx)) {
        m->al_ctx = m->al_own_ctx;
      }
    }
  }
  if (!m->al_ctx) {
    debugPrintf("[MOVIE] no OpenAL context available -- playing silent\n");
    mv_audio_close(m);
    return false;
  }

  alGetError();
  alGenBuffers(MOVIE_AL_BUFFERS, m->al_buffer);
  alGenSources(1, &m->al_source);
  if (alGetError() != AL_NO_ERROR || !m->al_source) {
    debugPrintf("[MOVIE] could not allocate OpenAL source -- playing silent\n");
    mv_audio_close(m);
    return false;
  }
  alSourcei(m->al_source, AL_SOURCE_RELATIVE, AL_TRUE);
  alSource3f(m->al_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
  alSourcef(m->al_source, AL_GAIN, 1.0f);

  m->al_scratch = malloc((size_t)MOVIE_AL_FRAMES * 2 * sizeof(int16_t));
  m->pcm_ring = malloc((size_t)MOVIE_PCM_FRAMES * 2 * sizeof(int16_t));
  if (!m->al_scratch || !m->pcm_ring) {
    mv_audio_close(m);
    return false;
  }

  /* Every buffer starts off the queue. Names are recycled through this stack
   * rather than by index, because OpenAL retires them in whatever order the
   * source finishes with them. */
  for (int i = 0; i < MOVIE_AL_BUFFERS; i++)
    m->al_free[i] = m->al_buffer[i];
  m->al_free_count = MOVIE_AL_BUFFERS;
  return true;
}

/* Moves whatever the ring holds into OpenAL, and reclaims buffers the source
 * has finished with. Never waits: if every buffer is still in flight the samples
 * stay in the ring and go out on a later call.
 *
 * This is called from the packet loop for video packets too, so a stretch of the
 * file with no audio in it still keeps the queue topped up. */
static void mv_audio_pump(Movie *m) {
  if (!m->al_source) return;

  ALint processed = 0;
  alGetSourcei(m->al_source, AL_BUFFERS_PROCESSED, &processed);
  while (processed-- > 0 && m->al_free_count < MOVIE_AL_BUFFERS) {
    ALuint done = 0;
    alSourceUnqueueBuffers(m->al_source, 1, &done);
    if (!done) break;
    m->al_free[m->al_free_count++] = done;
    if (m->al_queued > 0) m->al_queued--;
  }

  while (m->al_free_count > 0 && m->pcm_count >= MOVIE_AL_FRAMES) {
    /* Linearise one buffer's worth out of the ring. */
    int first = MOVIE_AL_FRAMES;
    if (m->pcm_tail + first > MOVIE_PCM_FRAMES)
      first = MOVIE_PCM_FRAMES - m->pcm_tail;
    memcpy(m->al_scratch, m->pcm_ring + (size_t)m->pcm_tail * 2,
           (size_t)first * 2 * sizeof(int16_t));
    if (first < MOVIE_AL_FRAMES)
      memcpy(m->al_scratch + (size_t)first * 2, m->pcm_ring,
             (size_t)(MOVIE_AL_FRAMES - first) * 2 * sizeof(int16_t));
    m->pcm_tail = (m->pcm_tail + MOVIE_AL_FRAMES) % MOVIE_PCM_FRAMES;
    m->pcm_count -= MOVIE_AL_FRAMES;

    ALuint use = m->al_free[--m->al_free_count];
    alBufferData(use, AL_FORMAT_STEREO16, m->al_scratch,
                 MOVIE_AL_FRAMES * 2 * (int)sizeof(int16_t), MOVIE_AUDIO_RATE);
    alSourceQueueBuffers(m->al_source, 1, &use);
    m->al_queued++;
  }

  ALint state = 0;
  alGetSourcei(m->al_source, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING) {
    /* Start once a little has accumulated, and restart after an underrun --
     * an underrun means the decoder lost the race, so say so. */
    if (m->al_queued >= 2) {
      if (state == AL_STOPPED)
        debugPrintf("[MOVIE] audio underrun (decoder fell behind)\n");
      alSourcePlay(m->al_source);
    }
  }
}

/* Resamples one decoded audio frame into the ring, then pumps. The ring is what
 * decouples audio from video: an MP4 hands out its streams in chunks, so a run
 * of audio packets can decode far ahead of where playback is, and this holds the
 * surplus instead of stalling the thread until OpenAL catches up. */
static void mv_audio_submit(Movie *m, AVFrame *frame) {
  if (!m->al_source || !m->swr || !m->pcm_ring) return;

  const uint8_t **in = (const uint8_t **)frame->extended_data;
  int in_count = frame->nb_samples;

  for (;;) {
    /* Never write past the ring: convert at most what will fit, and let the
     * resampler hold the rest for the next call. */
    int room = MOVIE_PCM_FRAMES - m->pcm_count;
    if (room > MOVIE_AL_FRAMES) room = MOVIE_AL_FRAMES;
    if (room <= 0) {
      /* Ring full. Three seconds of audio is already far more than the queue
       * can be behind by, so this means playback is not draining at all. */
      debugPrintf("[MOVIE] audio ring full -- dropping samples\n");
      break;
    }

    uint8_t *out = (uint8_t *)m->al_scratch;
    int got = swr_convert(m->swr, &out, room, in, in_count);
    if (got <= 0) break;

    int first = got;
    if (m->pcm_head + first > MOVIE_PCM_FRAMES)
      first = MOVIE_PCM_FRAMES - m->pcm_head;
    memcpy(m->pcm_ring + (size_t)m->pcm_head * 2, m->al_scratch,
           (size_t)first * 2 * sizeof(int16_t));
    if (first < got)
      memcpy(m->pcm_ring, m->al_scratch + (size_t)first * 2,
             (size_t)(got - first) * 2 * sizeof(int16_t));
    m->pcm_head = (m->pcm_head + got) % MOVIE_PCM_FRAMES;
    m->pcm_count += got;

    mv_audio_pump(m);

    /* Only the first pass feeds new input; further passes drain whatever the
     * resampler still holds, and stop as soon as it has nothing left. */
    in = NULL;
    in_count = 0;
    if (got < room) break;
  }
}

/* ------------------------------------------------------------------------- *
 * Video staging
 * ------------------------------------------------------------------------- */

/* Copies a decoded YUV420P frame into the staging planes. Kept as three tight
 * plane copies rather than a colour conversion: the shader does BT.601 on the
 * GPU, which costs nothing here and saves a full-frame CPU pass per frame. */
static void mv_publish_frame(Movie *m, AVFrame *f) {
  const int w = f->width, h = f->height;
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;

  pthread_mutex_lock(&g_mv_lock);

  if (m->width != w || m->height != h || !m->plane[0]) {
    mv_free_planes(m);
    m->plane[0] = malloc((size_t)w * h);
    m->plane[1] = malloc((size_t)cw * ch);
    m->plane[2] = malloc((size_t)cw * ch);
    if (!m->plane[0] || !m->plane[1] || !m->plane[2]) {
      mv_free_planes(m);
      pthread_mutex_unlock(&g_mv_lock);
      return;
    }
    m->plane_pitch[0] = w;
    m->plane_pitch[1] = cw;
    m->plane_pitch[2] = cw;
    m->width = w;
    m->height = h;
  }

  for (int p = 0; p < 3; p++) {
    const int pw = p ? cw : w;
    const int ph = p ? ch : h;
    const uint8_t *src = f->data[p];
    uint8_t *dst = m->plane[p];
    if (f->linesize[p] == pw) {
      memcpy(dst, src, (size_t)pw * ph);
    } else {
      for (int y = 0; y < ph; y++)
        memcpy(dst + (size_t)y * pw, src + (size_t)y * f->linesize[p], (size_t)pw);
    }
  }

  m->frame_ready = true;
  m->frame_dirty = true;
  pthread_mutex_unlock(&g_mv_lock);
}

/* ------------------------------------------------------------------------- *
 * Decode thread
 * ------------------------------------------------------------------------- */

static void mv_sleep_until(uint64_t target_tick) {
  for (;;) {
    if (atomic_load(&g_mv.stop_request)) return;
    uint64_t now = armGetSystemTick();
    if (now >= target_tick) return;
    uint64_t left_ns = armTicksToNs(target_tick - now);
    if (left_ns > 8000000ULL) left_ns = 8000000ULL; /* wake to re-check stop */
    svcSleepThread(left_ns);
  }
}

static void *mv_decode_thread(void *arg) {
  Movie *m = (Movie *)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  const uint64_t tick_freq = armGetSystemTickFreq();
  const uint64_t start_tick = armGetSystemTick();
  bool eof = false;

  if (!pkt || !frame) goto done;

  while (!atomic_load(&m->stop_request)) {
    int rc = av_read_frame(m->fmt, pkt);
    if (rc < 0) { eof = true; break; }

    AVCodecContext *dec = NULL;
    if (pkt->stream_index == m->vstream) dec = m->vdec;
    else if (pkt->stream_index == m->astream) dec = m->adec;

    /* Keep the queue fed across a video-only stretch of the file. */
    mv_audio_pump(m);

    if (!dec) { av_packet_unref(pkt); continue; }

    if (avcodec_send_packet(dec, pkt) == 0) {
      while (avcodec_receive_frame(dec, frame) == 0) {
        if (dec == m->vdec) {
          /* Present-time pacing. A frame whose slot has already passed is
           * published immediately -- catching up beats accumulating lag. */
          int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                          ? frame->best_effort_timestamp : frame->pts;
          if (pts != AV_NOPTS_VALUE) {
            AVRational tb = m->fmt->streams[m->vstream]->time_base;
            double sec = (double)pts * av_q2d(tb);
            mv_sleep_until(start_tick + (uint64_t)(sec * (double)tick_freq));
          }
          mv_publish_frame(m, frame);
        } else {
          mv_audio_submit(m, frame);
        }
        av_frame_unref(frame);
        if (atomic_load(&m->stop_request)) break;
      }
    }
    av_packet_unref(pkt);
  }

  /* Let whatever is already queued finish rather than cutting the last half
   * second of audio, unless the movie was skipped outright. Keep pumping while
   * we wait: the ring can still be holding seconds of decoded audio that the
   * queue has not had room for yet. */
  if (eof && !atomic_load(&m->stop_request) && m->al_source) {
    for (int i = 0; i < 500; i++) {
      mv_audio_pump(m);
      ALint state = 0;
      alGetSourcei(m->al_source, AL_SOURCE_STATE, &state);
      if (state != AL_PLAYING || atomic_load(&m->stop_request)) break;
      if (m->pcm_count == 0 && m->al_queued == 0) break;
      svcSleepThread(10000000ULL); /* 10 ms */
    }
  }

done:
  av_frame_free(&frame);
  av_packet_free(&pkt);
  mv_teardown(m);
  atomic_store(&m->playing, false);
  return NULL;
}

/* ------------------------------------------------------------------------- *
 * Public entry points
 * ------------------------------------------------------------------------- */

static bool mv_open_streams(Movie *m, const char *path) {
  m->fp = open_asset_with_fallback(path);
  if (!m->fp) {
    debugPrintf("[MOVIE] '%s' not present -- skipping\n", path);
    return false;
  }

  uint8_t *io_buf = av_malloc(MOVIE_IO_BUFSZ);
  if (!io_buf) return false;
  m->avio = avio_alloc_context(io_buf, MOVIE_IO_BUFSZ, 0, m->fp,
                               mv_io_read, NULL, mv_io_seek);
  if (!m->avio) { av_free(io_buf); return false; }

  m->fmt = avformat_alloc_context();
  if (!m->fmt) return false;
  m->fmt->pb = m->avio;
  /* Says "this AVIOContext is mine to free", so avformat_close_input() leaves
   * it alone and mv_teardown() stays the single owner of the FILE*. */
  m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (avformat_open_input(&m->fmt, NULL, NULL, NULL) < 0) {
    debugPrintf("[MOVIE] '%s' is not a container ffmpeg can open\n", path);
    return false;
  }
  if (avformat_find_stream_info(m->fmt, NULL) < 0) {
    debugPrintf("[MOVIE] no stream info in '%s'\n", path);
    return false;
  }

  m->vstream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  m->astream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (m->vstream < 0) {
    debugPrintf("[MOVIE] '%s' has no video stream\n", path);
    return false;
  }

  /* --- video --- */
  AVCodecParameters *vpar = m->fmt->streams[m->vstream]->codecpar;
  const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
  if (!vcodec) {
    debugPrintf("[MOVIE] no decoder for video codec %d\n", (int)vpar->codec_id);
    return false;
  }
  m->vdec = avcodec_alloc_context3(vcodec);
  if (!m->vdec || avcodec_parameters_to_context(m->vdec, vpar) < 0) return false;
  /* Decode inline on our own thread rather than letting libavcodec spawn
   * workers: both intros are 640x480 25fps at well under 1 Mbit, which a single
   * A57 core handles with room to spare, and it keeps the whole decoder on a
   * stack whose size we control (libnx's default pthread stack is not
   * generous). */
  m->vdec->thread_count = 1;
  m->vdec->thread_type = 0;
  if (avcodec_open2(m->vdec, vcodec, NULL) < 0) {
    debugPrintf("[MOVIE] could not open video decoder\n");
    return false;
  }

  /* --- audio (optional: a movie with no usable audio still plays) --- */
  if (m->astream >= 0) {
    AVCodecParameters *apar = m->fmt->streams[m->astream]->codecpar;
    const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
    if (acodec) {
      m->adec = avcodec_alloc_context3(acodec);
      if (m->adec && avcodec_parameters_to_context(m->adec, apar) >= 0 &&
          avcodec_open2(m->adec, acodec, NULL) == 0 && mv_audio_open(m)) {
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m->swr, &out_layout, AV_SAMPLE_FMT_S16,
                                MOVIE_AUDIO_RATE, &m->adec->ch_layout,
                                m->adec->sample_fmt, m->adec->sample_rate,
                                0, NULL) < 0 || swr_init(m->swr) < 0) {
          debugPrintf("[MOVIE] resampler init failed -- playing silent\n");
          if (m->swr) swr_free(&m->swr);
          mv_audio_close(m);
        }
      } else {
        debugPrintf("[MOVIE] audio unavailable -- playing silent\n");
        if (m->adec) avcodec_free_context(&m->adec);
        mv_audio_close(m);
      }
    }
    if (!m->swr) m->astream = -1;
  }

  return true;
}

bool movie_play(const char *engine_path) {
  if (!engine_path || !engine_path[0]) return false;

  /* Escape hatch: intro_movies=0 in gta3_nx.cfg turns playback off without a
   * rebuild, and the engine's wait state falls straight through. */
  if (!config.intro_movies) {
    debugPrintf("[MOVIE] intro movies disabled in config -- skipping '%s'\n", engine_path);
    return false;
  }

  /* A second playMovie without an intervening stop replaces the first. */
  movie_stop();

  Movie *m = &g_mv;
  memset(m, 0, sizeof(*m));
  m->vstream = m->astream = -1;
  atomic_store(&m->stop_request, false);

  if (!mv_open_streams(m, engine_path)) {
    mv_teardown(m);
    return false;
  }

  atomic_store(&m->playing, true);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  int rc = pthread_create(&m->thread, &attr, mv_decode_thread, m);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    debugPrintf("[MOVIE] could not start decode thread (%d)\n", rc);
    atomic_store(&m->playing, false);
    mv_teardown(m);
    return false;
  }
  m->thread_valid = true;

  /* Decoding competes with the engine's own boot work; hold the clock up for
   * the length of the movie so neither starves. */
  cpu_boost(1);
  return true;
}

bool movie_is_playing(void) {
  return atomic_load(&g_mv.playing);
}

void movie_stop(void) {
  Movie *m = &g_mv;
  if (!m->thread_valid) return;

  atomic_store(&m->stop_request, true);
  pthread_join(m->thread, NULL);
  m->thread_valid = false;
  atomic_store(&m->playing, false);

  pthread_mutex_lock(&g_mv_lock);
  mv_free_planes(m);
  pthread_mutex_unlock(&g_mv_lock);

  cpu_boost(0);
}

/* ------------------------------------------------------------------------- *
 * Presentation (GL thread)
 * ------------------------------------------------------------------------- */

static const char *kMovieVert =
  "attribute vec2 aPos;\n"
  "attribute vec2 aUV;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
  "}\n";

/* BT.601 limited range, which is what these clips are tagged as. */
static const char *kMovieFrag =
  "precision mediump float;\n"
  "varying vec2 vUV;\n"
  "uniform sampler2D uY;\n"
  "uniform sampler2D uU;\n"
  "uniform sampler2D uV;\n"
  "void main() {\n"
  "  float y = texture2D(uY, vUV).a;\n"
  "  float u = texture2D(uU, vUV).a - 0.5;\n"
  "  float v = texture2D(uV, vUV).a - 0.5;\n"
  "  y = (y - 0.0625) * 1.164;\n"
  "  gl_FragColor = vec4(y + 1.596 * v,\n"
  "                      y - 0.391 * u - 0.813 * v,\n"
  "                      y + 2.018 * u, 1.0);\n"
  "}\n";

static GLuint mv_prog = 0;
static GLuint mv_tex[3] = { 0, 0, 0 };
static GLint mv_loc_pos = -1, mv_loc_uv = -1;
static GLint mv_loc_y = -1, mv_loc_u = -1, mv_loc_v = -1;
static int mv_tex_w[3] = { 0, 0, 0 }, mv_tex_h[3] = { 0, 0, 0 };
static bool mv_gl_failed = false;

static GLuint mv_compile(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  if (!sh) return 0;
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    GLsizei n = 0;
    glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
    log[n] = 0;
    debugPrintf("[MOVIE] shader compile failed: %s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static bool mv_gl_init(void) {
  if (mv_prog) return true;
  if (mv_gl_failed) return false;

  GLuint vs = mv_compile(GL_VERTEX_SHADER, kMovieVert);
  GLuint fs = mv_compile(GL_FRAGMENT_SHADER, kMovieFrag);
  if (!vs || !fs) { mv_gl_failed = true; return false; }

  mv_prog = glCreateProgram();
  glAttachShader(mv_prog, vs);
  glAttachShader(mv_prog, fs);
  glLinkProgram(mv_prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(mv_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    debugPrintf("[MOVIE] program link failed\n");
    glDeleteProgram(mv_prog);
    mv_prog = 0;
    mv_gl_failed = true;
    return false;
  }

  mv_loc_pos = glGetAttribLocation(mv_prog, "aPos");
  mv_loc_uv  = glGetAttribLocation(mv_prog, "aUV");
  mv_loc_y   = glGetUniformLocation(mv_prog, "uY");
  mv_loc_u   = glGetUniformLocation(mv_prog, "uU");
  mv_loc_v   = glGetUniformLocation(mv_prog, "uV");
  if (mv_loc_pos < 0 || mv_loc_uv < 0) { mv_gl_failed = true; return false; }

  glGenTextures(3, mv_tex);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, mv_tex[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  return true;
}

/* GL_ALPHA is the one single-channel format GLES2 guarantees, which is why the
 * shader samples .a rather than .r. */
static void mv_upload_plane(int i, const uint8_t *px, int w, int h) {
  glActiveTexture(GL_TEXTURE0 + i);
  glBindTexture(GL_TEXTURE_2D, mv_tex[i]);
  if (mv_tex_w[i] != w || mv_tex_h[i] != h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px);
    mv_tex_w[i] = w;
    mv_tex_h[i] = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_ALPHA, GL_UNSIGNED_BYTE, px);
  }
}

bool movie_should_present(void) {
  static u64 last_present = 0;
  const u64 now = armGetSystemTick();
  const u64 freq = armGetSystemTickFreq();

  bool dirty;
  pthread_mutex_lock(&g_mv_lock);
  dirty = g_mv.frame_dirty || !g_mv.frame_ready;
  pthread_mutex_unlock(&g_mv_lock);

  /* Refresh at least four times a second even with nothing new, so the surface
   * never sits untouched long enough for the compositor to lose interest. */
  if (dirty || last_present == 0 || now - last_present >= freq / 4) {
    last_present = now;
    return true;
  }
  return false;
}

void movie_render(void *display, void *surface) {
  Movie *m = &g_mv;
  if (!atomic_load(&m->playing)) return;
  if (!mv_gl_init()) return;

  EGLint sw = 0, sh = 0;
  eglQuerySurface((EGLDisplay)display, (EGLSurface)surface, EGL_WIDTH, &sw);
  eglQuerySurface((EGLDisplay)display, (EGLSurface)surface, EGL_HEIGHT, &sh);
  if (sw <= 0 || sh <= 0) { sw = 1280; sh = 720; }

  /* Save every piece of state we disturb: the engine is mid-render and gets
   * the context back untouched, exactly as the FPS overlay does. */
  GLint old_prog = 0, old_abuf = 0, old_active = 0, old_fbo = 0;
  GLint old_vp[4], old_tex_bind[3] = { 0, 0, 0 };
  GLfloat old_clear[4];
  GLboolean old_blend, old_depth, old_cull, old_scissor, old_stencil;
  GLboolean old_colormask[4];
  GLint a0_en = 0, a1_en = 0;

  glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abuf);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
  glGetIntegerv(GL_VIEWPORT, old_vp);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  glGetBooleanv(GL_COLOR_WRITEMASK, old_colormask);
  old_blend   = glIsEnabled(GL_BLEND);
  old_depth   = glIsEnabled(GL_DEPTH_TEST);
  old_cull    = glIsEnabled(GL_CULL_FACE);
  old_scissor = glIsEnabled(GL_SCISSOR_TEST);
  old_stencil = glIsEnabled(GL_STENCIL_TEST);
  glGetVertexAttribiv(mv_loc_pos, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0_en);
  glGetVertexAttribiv(mv_loc_uv,  GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a1_en);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex_bind[i]);
  }

  /* Straight to the window surface. The engine renders through an alternate
   * render target at times, and a movie that landed in an FBO nobody is going
   * to blit would simply never appear. */
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  /* The engine leaves the write mask in whatever state its last draw wanted. A
   * masked-off channel would silently swallow both the clear and the video. */
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, sw, sh);

  /* Black first: whatever the engine drew underneath is a boot-time artefact,
   * and the letterbox bars need a defined colour anyway. */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  pthread_mutex_lock(&g_mv_lock);
  if (m->frame_ready && m->plane[0]) {
    const int vw = m->width, vh = m->height;
    const int cw = (vw + 1) / 2, chh = (vh + 1) / 2;
    if (m->frame_dirty) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      mv_upload_plane(0, m->plane[0], vw, vh);
      mv_upload_plane(1, m->plane[1], cw, chh);
      mv_upload_plane(2, m->plane[2], cw, chh);
      m->frame_dirty = false;
    }
    pthread_mutex_unlock(&g_mv_lock);

    /* Letterbox: fit the video inside the surface without distorting it. */
    float scale = (float)sw / (float)vw;
    if ((float)vh * scale > (float)sh) scale = (float)sh / (float)vh;
    const float hx = ((float)vw * scale) / (float)sw;  /* half-width in NDC */
    const float hy = ((float)vh * scale) / (float)sh;

    const GLfloat quad[6][4] = {
      { -hx,  hy, 0.0f, 0.0f }, {  hx,  hy, 1.0f, 0.0f }, { -hx, -hy, 0.0f, 1.0f },
      {  hx,  hy, 1.0f, 0.0f }, {  hx, -hy, 1.0f, 1.0f }, { -hx, -hy, 0.0f, 1.0f },
    };

    glUseProgram(mv_prog);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* Bind on every draw, not just on the frames that upload. The video decodes
     * at 25 fps while this runs at the main loop's rate, so most draws reuse the
     * textures already on the GPU -- and the binding does not survive, because
     * the tail of this function restores whatever the engine had on units 0..2.
     * Sampling an unbound unit yields alpha 1.0, which the BT.601 maths below
     * turns into chroma +0.5 on both axes: high red, high blue, low green. That
     * was the magenta frame between every video frame. */
    for (int i = 0; i < 3; i++) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, mv_tex[i]);
    }

    glUniform1i(mv_loc_y, 0);
    glUniform1i(mv_loc_u, 1);
    glUniform1i(mv_loc_v, 2);
    glEnableVertexAttribArray(mv_loc_pos);
    glEnableVertexAttribArray(mv_loc_uv);
    glVertexAttribPointer(mv_loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(mv_loc_uv,  2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), &quad[0][2]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (!a0_en) glDisableVertexAttribArray(mv_loc_pos);
    if (!a1_en) glDisableVertexAttribArray(mv_loc_uv);
  } else {
    /* Cleared to black, but the decoder has not published a frame yet. */
    pthread_mutex_unlock(&g_mv_lock);
  }

  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex_bind[i]);
  }
  glActiveTexture((GLenum)old_active);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_abuf);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
  glUseProgram((GLuint)old_prog);
  glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  glColorMask(old_colormask[0], old_colormask[1], old_colormask[2], old_colormask[3]);
  if (old_blend)   glEnable(GL_BLEND);
  if (old_depth)   glEnable(GL_DEPTH_TEST);
  if (old_cull)    glEnable(GL_CULL_FACE);
  if (old_scissor) glEnable(GL_SCISSOR_TEST);
  if (old_stencil) glEnable(GL_STENCIL_TEST);
}
