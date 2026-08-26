/* jni_fake.c -- fake JNI environment for the oswrapper/GameNative layer
 *
 * The JNIEnv/JavaVM function tables here are the standard JNI ABI and are
 * framework-agnostic; only the dispatch in hal_* (keyed by the Java method
 * name) is specific to the com.rockstargames.oswrapper engine GTA III uses.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "movie.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

typedef struct {
  uint32_t tag;
  int len;
  int elem_size;
  void *data;
} FakePriArray;

// method/field IDs are pointers to these records; calls dispatch by (class, name).
typedef struct {
  uint32_t tag;
  char cls[64];
  char name[80];
  char sig[80];
} FakeID;

volatile int jni_quit_requested = 0;
volatile int jni_frontend_ready = 0;

// ---------------------------------------------------------------------------
// deferred native-callback queue (the Java->native completion direction)
// ---------------------------------------------------------------------------

#define CB_QUEUE_LEN 32
static JniCallback cb_queue[CB_QUEUE_LEN];
static int cb_head = 0, cb_tail = 0;

static void push_cb(JniCallbackType type, int arg0, int arg1) {
  const int next = (cb_tail + 1) % CB_QUEUE_LEN;
  if (next == cb_head) {
    debugPrintf("JNI: callback queue full, dropping %d\n", type);
    return;
  }
  cb_queue[cb_tail].type = type;
  cb_queue[cb_tail].arg0 = arg0;
  cb_queue[cb_tail].arg1 = arg1;
  cb_tail = next;
}

int jni_pop_callback(JniCallback *out) {
  if (cb_head == cb_tail)
    return 0;
  if (out)
    *out = cb_queue[cb_head];
  cb_head = (cb_head + 1) % CB_QUEUE_LEN;
  return 1;
}

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  o->label[sizeof(o->label) - 1] = '\0';   // strncpy may not NUL-terminate
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = n;
  a->items = calloc(n ? n : 1, sizeof(void *));
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(strs[i]);
  return a;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// ---------------------------------------------------------------------------
// app-local key/value store (get/setAppLocalValue), persisted as tab-separated
// lines. Backs STORAGE_ROOT and the engine's misc settings.
// ---------------------------------------------------------------------------

// Every engine file root hangs off this. The NRO runs with its own directory as
// the working directory, so "." is /switch/gta3 -- readable for the shipped data
// and writable for saves. OS_FileOpen builds user-data paths from it too, which
// is why it is a function rather than a literal in two places.
const char *jni_storage_root(void) {
  return ".";
}

#define KV_MAX 256

typedef struct {
  char key[64];
  char val[256];
} KvPair;

static KvPair kv_store[KV_MAX];
static int kv_count = 0;

static const char *kv_get(const char *key) {
  for (int i = 0; i < kv_count; i++)
    if (!strcmp(kv_store[i].key, key))
      return kv_store[i].val;
  return NULL;
}

static void kv_save(void) {
  FILE *f = fopen(APPSTATE_NAME, "w");
  if (!f)
    return;
  for (int i = 0; i < kv_count; i++)
    fprintf(f, "%s\t%s\n", kv_store[i].key, kv_store[i].val);
  fclose(f);
}

static void kv_set(const char *key, const char *val) {
  for (int i = 0; i < kv_count; i++) {
    if (!strcmp(kv_store[i].key, key)) {
      strlcpy(kv_store[i].val, val, sizeof(kv_store[i].val));
      kv_save();
      return;
    }
  }
  if (kv_count >= KV_MAX) {
    debugPrintf("JNI: app-local store full, dropping %s\n", key);
    return;
  }
  strlcpy(kv_store[kv_count].key, key, sizeof(kv_store[kv_count].key));
  strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
  kv_count++;
  kv_save();
}

static void kv_load(void) {
  FILE *f = fopen(APPSTATE_NAME, "r");
  if (!f)
    return;
  char line[360];
  while (kv_count < KV_MAX && fgets(line, sizeof(line), f)) {
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = 0;
    char *val = tab + 1;
    val[strcspn(val, "\r\n")] = 0;
    strlcpy(kv_store[kv_count].key, line, sizeof(kv_store[kv_count].key));
    strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
    kv_count++;
  }
  fclose(f);
}

// method/field ID pool
#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++) {
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  }
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted!\n");
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->cls, sizeof(id->cls), "%s", cls);
  snprintf(id->name, sizeof(id->name), "%s", name);
  snprintf(id->sig, sizeof(id->sig), "%s", sig);
  return id;
}

// label of the FakeObject that FindClass produced for this jclass
static const char *class_label(void *cls) {
  FakeObject *o = cls;
  if (o && o->tag == TAG_OBJECT)
    return o->label;
  return "";
}

// va_list arg helpers (JNI variadic: small ints promote to int, jobject is a pointer)
static const char *next_str(va_list va) { return obj_str(va_arg(va, void *)); }
static int next_int(va_list va) { return va_arg(va, int); }

// ---------------------------------------------------------------------------
// HAL dispatch, keyed by the Java method name. Anything not handled returns a
// sane default (the engine treats missing services as "feature unavailable").
// ---------------------------------------------------------------------------

static int name_is(const FakeID *id, const char *n) { return strcmp(id->name, n) == 0; }

static void hal_void(const FakeID *id, va_list va) {
  const char *name = id->name;

  // app-local key/value store (STORAGE_ROOT etc. are written here at boot)
  if (name_is(id, "setAppLocalValue")) {
    const char *key = next_str(va);
    const char *val = next_str(va);
    kv_set(key, val);
    debugPrintf("JNI: setAppLocalValue(%s = %s)\n", key, val);
    return;
  }

  // splash / loading screen: no Java UI, so these are no-ops. hideSplashScreen
  // is the engine's "loading finished" signal.
  if (name_is(id, "showSplashScreen") || name_is(id, "setSplashImage") ||
      name_is(id, "setSplashText")) {
    return;
  }
  if (name_is(id, "hideSplashScreen")) {
    cpu_boost(0);
    jni_frontend_ready = 1;
    debugPrintf("JNI: hideSplashScreen -> frontend ready\n");
    return;
  }

  // --- async platform operations the engine blocks on until we call back ---
  // The engine stalls in early boot states until these completion callbacks
  // fire; we queue them and the main loop drives the matching impl* entry point.
  if (name_is(id, "playlistOpen")) {
    // report an empty user playlist opened so boot advances
    debugPrintf("JNI: playlistOpen -> queue OnPlaylistOpenComplete(1, 0)\n");
    push_cb(JNI_CB_PLAYLIST_OPEN_COMPLETE, 1, 0);
    return;
  }
  if (name_is(id, "rockstarShowInitial")) {
    debugPrintf("JNI: rockstarShowInitial -> queue OnRockstarInitialComplete\n");
    push_cb(JNI_CB_ROCKSTAR_INITIAL_COMPLETE, 0, 0);
    return;
  }
  if (name_is(id, "rockstarShowGate")) {
    const int gate = next_int(va);
    debugPrintf("JNI: rockstarShowGate(%d) -> queue OnRockstarGateComplete(%d, 1)\n", gate, gate);
    push_cb(JNI_CB_ROCKSTAR_GATE_COMPLETE, gate, 1); // 1 = gate passed
    return;
  }
  if (name_is(id, "rockstarSignIn")) {
    debugPrintf("JNI: rockstarSignIn -> queue OnRockstarSignInComplete\n");
    push_cb(JNI_CB_ROCKSTAR_SIGNIN_COMPLETE, 0, 0);
    return;
  }
  if (name_is(id, "rockstarSignOut")) {
    debugPrintf("JNI: rockstarSignOut -> queue OnRockstarSignOutComplete\n");
    push_cb(JNI_CB_ROCKSTAR_SIGNOUT_COMPLETE, 0, 0);
    return;
  }

  // --- intro movies ---------------------------------------------------------
  // OS_MoviePlay / OS_MovieStop land here. movie.c reports failure for a
  // missing or undecodable file, and isMoviePlaying() below then answers false
  // straight away, so the engine's wait state falls through to the next boot
  // step exactly as if the movie had finished.
  if (name_is(id, "playMovie")) {
    const char *path = next_str(va);
    debugPrintf("JNI: playMovie('%s')\n", path ? path : "(null)");
    movie_play(path);
    return;
  }
  if (name_is(id, "stopMovie")) {
    debugPrintf("JNI: stopMovie\n");
    movie_stop();
    return;
  }
  // Subtitle overlay for the movies -- the intros carry none, and the engine
  // is happy for these to do nothing.
  if (name_is(id, "setMovieText") || name_is(id, "clearMovieText") ||
      name_is(id, "displayMovieText") || name_is(id, "setMovieTextScale")) {
    return;
  }

  // lifecycle / misc
  if (name_is(id, "finish") || name_is(id, "exitGame") || name_is(id, "quit") ||
      name_is(id, "QuitApp")) {
    debugPrintf("JNI: %s -> request quit\n", name);
    jni_quit_requested = 1;
    return;
  }
}

static juint hal_int(const FakeID *id, va_list va) {
  (void)id; (void)va;
  return 0;
}

static juint hal_bool(const FakeID *id, va_list va) {
  (void)va;

  // no Java splash, so it is never "visible"
  if (name_is(id, "isSplashScreenVisible"))
    return 0;

  // Polled once per frame by OS_MovieIsPlaying while the boot state machine
  // waits for an intro to finish.
  if (name_is(id, "isMoviePlaying"))
    return movie_is_playing() ? 1 : 0;

  // Switch is a console/TV device, not a phone (affects mobile UI scaling)
  if (name_is(id, "isPhone") || name_is(id, "isPhoneDevice"))
    return 0;

  return 0;
}

static float hal_float(const FakeID *id, va_list va) {
  (void)id; (void)va;
  return 0.0f;
}

static void *hal_object(const FakeID *id, va_list va) {
  // STORAGE_ROOT / STORAGE_ROOT_BASE anchor every engine file root; "." points
  // them at the NRO's directory, where data is read and saves are written.
  if (name_is(id, "getAppLocalValue")) {
    const char *key = next_str(va);
    if (!strcmp(key, "STORAGE_ROOT") || !strcmp(key, "STORAGE_ROOT_BASE"))
      return jni_make_string(jni_storage_root());
    const char *v = kv_get(key);
    return jni_make_string(v ? v : "");
  }

  if (name_is(id, "getAppVersion") || name_is(id, "GetVersionName"))
    return jni_make_string("2.11");

  if (name_is(id, "getDeviceLocale") || name_is(id, "GetDeviceLanguage"))
    return jni_make_string("en");

  // toImage / getInstance / getParameter / other getters -> a fresh fake obj
  return jni_make_object("osobject");
}

// ---------------------------------------------------------------------------
// field reads: sane defaults keyed by field name
// ---------------------------------------------------------------------------

static juint get_boolean_field(const char *name) {
  if (!strcmp(name, "isTvDevice"))     return 1;
  if (!strcmp(name, "hasTouchScreen")) return 1;
  if (!strcmp(name, "hasVibrator"))    return 0;
  return 0;
}

static juint get_int_field(const char *name) {
  if (!strcmp(name, "osVersion"))    return 30;
  if (!strcmp(name, "cpuFrequency")) return 1785;
  return 0;
}

static void *get_object_field(const char *name) {
  if (!strcmp(name, "manufacturer")) return jni_make_string("Nintendo");
  if (!strcmp(name, "model"))        return jni_make_string("Switch");
  if (!strcmp(name, "hardware"))     return jni_make_string("nx");
  if (!strcmp(name, "product"))      return jni_make_string("switch");
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return get_id(class_label(cls), name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return get_id(class_label(cls), name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env; (void)obj;
  return jni_make_object("class");
}

static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

// --- Call<type>Method ---

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_bool(id, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_bool(id, va);
  va_end(va);
  return r;
}

static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_int(id, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_int(id, va);
  va_end(va);
  return r;
}

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_object(id, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = hal_object(id, va);
  va_end(va);
  return r;
}

static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  hal_void(id, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  hal_void(id, va);
  va_end(va);
}

static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_float(id, va);
}
static float j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = hal_float(id, va);
  va_end(va);
  return r;
}

static juint j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallLongMethod %s.%s -> 0\n", id->cls, id->name);
  return 0;
}

// static variants share the dispatchers (the receiver doesn't matter)
static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_object(id, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = hal_object(id, va);
  va_end(va);
  return r;
}
static juint j_CallStaticBooleanMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_bool(id, va);
}
static juint j_CallStaticBooleanMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_bool(id, va);
  va_end(va);
  return r;
}
static juint j_CallStaticIntMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_int(id, va);
}
static juint j_CallStaticIntMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_int(id, va);
  va_end(va);
  return r;
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  hal_void(id, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  hal_void(id, va);
  va_end(va);
}
static float j_CallStaticFloatMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_float(id, va);
}
static float j_CallStaticFloatMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = hal_float(id, va);
  va_end(va);
  return r;
}

static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)id; (void)va;
  return jni_make_object(class_label(cls));
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = j_NewObjectV(env, cls, id, va);
  va_end(va);
  return r;
}

// --- fields ---

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_object_field(id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_boolean_field(id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_int_field(id->name);
}
static juint j_GetLongField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; (void)id;
  return 0;
}
static float j_GetFloatField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; (void)id;
  return 0.0f;
}

// --- strings ---

static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return jni_make_string(utf);
}

static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  return obj_str(jstr);
}

static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}

static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1;
  const char *utf = obj_str(jstr);
  const int len = strlen(utf);
  uint16_t *out = calloc(len + 1, sizeof(uint16_t));
  for (int i = 0; i < len; i++)
    out[i] = (uint8_t)utf[i];
  return out;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

// --- arrays ---

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_OBJARR || a->tag == TAG_PRIARR))
    return a->len;
  return 0;
}

static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    return a->items[idx];
  return jni_make_string("");
}

static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    a->items[idx] = val;
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++)
    a->items[i] = init;
  return a;
}

static void *new_pri_array(int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = calloc(len ? len : 1, elem_size);
  return a;
}

static void *j_NewByteArray(void *env, int len)  { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len)   { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR)
    return a->data;
  return NULL;
}

static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}

static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + start * a->elem_size, len * a->elem_size);
}

static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + start * a->elem_size, buf, len * a->elem_size);
}

// --- misc ---

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods;
  debugPrintf("JNI: RegisterNatives(%d methods) ignored\n", n);
  return 0;
}

static juint j_GetJavaVM(void *env, void **vm) {
  (void)env;
  *vm = fake_vm;
  return JNI_OK;
}

static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_ExceptionClearDescribe(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

static juint j_unimplemented_slot(int slot) {
  debugPrintf("[JNI] WARNING: Call to unimplemented JNIEnv slot #%d\n", slot);
  return 0;
}
static juint j_unimpl_0(void) { return j_unimplemented_slot(0); }
static juint j_unimpl_1(void) { return j_unimplemented_slot(1); }
static juint j_unimpl_2(void) { return j_unimplemented_slot(2); }
static juint j_unimpl_3(void) { return j_unimplemented_slot(3); }
static juint j_unimpl_4(void) { return j_unimplemented_slot(4); }
static juint j_unimpl_5(void) { return j_unimplemented_slot(5); }
static juint j_unimpl_6(void) { return j_unimplemented_slot(6); }
static juint j_unimpl_7(void) { return j_unimplemented_slot(7); }
static juint j_unimpl_8(void) { return j_unimplemented_slot(8); }
static juint j_unimpl_9(void) { return j_unimplemented_slot(9); }
static juint j_unimpl_10(void) { return j_unimplemented_slot(10); }
static juint j_unimpl_11(void) { return j_unimplemented_slot(11); }
static juint j_unimpl_12(void) { return j_unimplemented_slot(12); }
static juint j_unimpl_13(void) { return j_unimplemented_slot(13); }
static juint j_unimpl_14(void) { return j_unimplemented_slot(14); }
static juint j_unimpl_15(void) { return j_unimplemented_slot(15); }
static juint j_unimpl_16(void) { return j_unimplemented_slot(16); }
static juint j_unimpl_17(void) { return j_unimplemented_slot(17); }
static juint j_unimpl_18(void) { return j_unimplemented_slot(18); }
static juint j_unimpl_19(void) { return j_unimplemented_slot(19); }
static juint j_unimpl_20(void) { return j_unimplemented_slot(20); }
static juint j_unimpl_21(void) { return j_unimplemented_slot(21); }
static juint j_unimpl_22(void) { return j_unimplemented_slot(22); }
static juint j_unimpl_23(void) { return j_unimplemented_slot(23); }
static juint j_unimpl_24(void) { return j_unimplemented_slot(24); }
static juint j_unimpl_25(void) { return j_unimplemented_slot(25); }
static juint j_unimpl_26(void) { return j_unimplemented_slot(26); }
static juint j_unimpl_27(void) { return j_unimplemented_slot(27); }
static juint j_unimpl_28(void) { return j_unimplemented_slot(28); }
static juint j_unimpl_29(void) { return j_unimplemented_slot(29); }
static juint j_unimpl_30(void) { return j_unimplemented_slot(30); }
static juint j_unimpl_31(void) { return j_unimplemented_slot(31); }
static juint j_unimpl_32(void) { return j_unimplemented_slot(32); }
static juint j_unimpl_33(void) { return j_unimplemented_slot(33); }
static juint j_unimpl_34(void) { return j_unimplemented_slot(34); }
static juint j_unimpl_35(void) { return j_unimplemented_slot(35); }
static juint j_unimpl_36(void) { return j_unimplemented_slot(36); }
static juint j_unimpl_37(void) { return j_unimplemented_slot(37); }
static juint j_unimpl_38(void) { return j_unimplemented_slot(38); }
static juint j_unimpl_39(void) { return j_unimplemented_slot(39); }
static juint j_unimpl_40(void) { return j_unimplemented_slot(40); }
static juint j_unimpl_41(void) { return j_unimplemented_slot(41); }
static juint j_unimpl_42(void) { return j_unimplemented_slot(42); }
static juint j_unimpl_43(void) { return j_unimplemented_slot(43); }
static juint j_unimpl_44(void) { return j_unimplemented_slot(44); }
static juint j_unimpl_45(void) { return j_unimplemented_slot(45); }
static juint j_unimpl_46(void) { return j_unimplemented_slot(46); }
static juint j_unimpl_47(void) { return j_unimplemented_slot(47); }
static juint j_unimpl_48(void) { return j_unimplemented_slot(48); }
static juint j_unimpl_49(void) { return j_unimplemented_slot(49); }
static juint j_unimpl_50(void) { return j_unimplemented_slot(50); }
static juint j_unimpl_51(void) { return j_unimplemented_slot(51); }
static juint j_unimpl_52(void) { return j_unimplemented_slot(52); }
static juint j_unimpl_53(void) { return j_unimplemented_slot(53); }
static juint j_unimpl_54(void) { return j_unimplemented_slot(54); }
static juint j_unimpl_55(void) { return j_unimplemented_slot(55); }
static juint j_unimpl_56(void) { return j_unimplemented_slot(56); }
static juint j_unimpl_57(void) { return j_unimplemented_slot(57); }
static juint j_unimpl_58(void) { return j_unimplemented_slot(58); }
static juint j_unimpl_59(void) { return j_unimplemented_slot(59); }
static juint j_unimpl_60(void) { return j_unimplemented_slot(60); }
static juint j_unimpl_61(void) { return j_unimplemented_slot(61); }
static juint j_unimpl_62(void) { return j_unimplemented_slot(62); }
static juint j_unimpl_63(void) { return j_unimplemented_slot(63); }
static juint j_unimpl_64(void) { return j_unimplemented_slot(64); }
static juint j_unimpl_65(void) { return j_unimplemented_slot(65); }
static juint j_unimpl_66(void) { return j_unimplemented_slot(66); }
static juint j_unimpl_67(void) { return j_unimplemented_slot(67); }
static juint j_unimpl_68(void) { return j_unimplemented_slot(68); }
static juint j_unimpl_69(void) { return j_unimplemented_slot(69); }
static juint j_unimpl_70(void) { return j_unimplemented_slot(70); }
static juint j_unimpl_71(void) { return j_unimplemented_slot(71); }
static juint j_unimpl_72(void) { return j_unimplemented_slot(72); }
static juint j_unimpl_73(void) { return j_unimplemented_slot(73); }
static juint j_unimpl_74(void) { return j_unimplemented_slot(74); }
static juint j_unimpl_75(void) { return j_unimplemented_slot(75); }
static juint j_unimpl_76(void) { return j_unimplemented_slot(76); }
static juint j_unimpl_77(void) { return j_unimplemented_slot(77); }
static juint j_unimpl_78(void) { return j_unimplemented_slot(78); }
static juint j_unimpl_79(void) { return j_unimplemented_slot(79); }
static juint j_unimpl_80(void) { return j_unimplemented_slot(80); }
static juint j_unimpl_81(void) { return j_unimplemented_slot(81); }
static juint j_unimpl_82(void) { return j_unimplemented_slot(82); }
static juint j_unimpl_83(void) { return j_unimplemented_slot(83); }
static juint j_unimpl_84(void) { return j_unimplemented_slot(84); }
static juint j_unimpl_85(void) { return j_unimplemented_slot(85); }
static juint j_unimpl_86(void) { return j_unimplemented_slot(86); }
static juint j_unimpl_87(void) { return j_unimplemented_slot(87); }
static juint j_unimpl_88(void) { return j_unimplemented_slot(88); }
static juint j_unimpl_89(void) { return j_unimplemented_slot(89); }
static juint j_unimpl_90(void) { return j_unimplemented_slot(90); }
static juint j_unimpl_91(void) { return j_unimplemented_slot(91); }
static juint j_unimpl_92(void) { return j_unimplemented_slot(92); }
static juint j_unimpl_93(void) { return j_unimplemented_slot(93); }
static juint j_unimpl_94(void) { return j_unimplemented_slot(94); }
static juint j_unimpl_95(void) { return j_unimplemented_slot(95); }
static juint j_unimpl_96(void) { return j_unimplemented_slot(96); }
static juint j_unimpl_97(void) { return j_unimplemented_slot(97); }
static juint j_unimpl_98(void) { return j_unimplemented_slot(98); }
static juint j_unimpl_99(void) { return j_unimplemented_slot(99); }
static juint j_unimpl_100(void) { return j_unimplemented_slot(100); }
static juint j_unimpl_101(void) { return j_unimplemented_slot(101); }
static juint j_unimpl_102(void) { return j_unimplemented_slot(102); }
static juint j_unimpl_103(void) { return j_unimplemented_slot(103); }
static juint j_unimpl_104(void) { return j_unimplemented_slot(104); }
static juint j_unimpl_105(void) { return j_unimplemented_slot(105); }
static juint j_unimpl_106(void) { return j_unimplemented_slot(106); }
static juint j_unimpl_107(void) { return j_unimplemented_slot(107); }
static juint j_unimpl_108(void) { return j_unimplemented_slot(108); }
static juint j_unimpl_109(void) { return j_unimplemented_slot(109); }
static juint j_unimpl_110(void) { return j_unimplemented_slot(110); }
static juint j_unimpl_111(void) { return j_unimplemented_slot(111); }
static juint j_unimpl_112(void) { return j_unimplemented_slot(112); }
static juint j_unimpl_113(void) { return j_unimplemented_slot(113); }
static juint j_unimpl_114(void) { return j_unimplemented_slot(114); }
static juint j_unimpl_115(void) { return j_unimplemented_slot(115); }
static juint j_unimpl_116(void) { return j_unimplemented_slot(116); }
static juint j_unimpl_117(void) { return j_unimplemented_slot(117); }
static juint j_unimpl_118(void) { return j_unimplemented_slot(118); }
static juint j_unimpl_119(void) { return j_unimplemented_slot(119); }
static juint j_unimpl_120(void) { return j_unimplemented_slot(120); }
static juint j_unimpl_121(void) { return j_unimplemented_slot(121); }
static juint j_unimpl_122(void) { return j_unimplemented_slot(122); }
static juint j_unimpl_123(void) { return j_unimplemented_slot(123); }
static juint j_unimpl_124(void) { return j_unimplemented_slot(124); }
static juint j_unimpl_125(void) { return j_unimplemented_slot(125); }
static juint j_unimpl_126(void) { return j_unimplemented_slot(126); }
static juint j_unimpl_127(void) { return j_unimplemented_slot(127); }
static juint j_unimpl_128(void) { return j_unimplemented_slot(128); }
static juint j_unimpl_129(void) { return j_unimplemented_slot(129); }
static juint j_unimpl_130(void) { return j_unimplemented_slot(130); }
static juint j_unimpl_131(void) { return j_unimplemented_slot(131); }
static juint j_unimpl_132(void) { return j_unimplemented_slot(132); }
static juint j_unimpl_133(void) { return j_unimplemented_slot(133); }
static juint j_unimpl_134(void) { return j_unimplemented_slot(134); }
static juint j_unimpl_135(void) { return j_unimplemented_slot(135); }
static juint j_unimpl_136(void) { return j_unimplemented_slot(136); }
static juint j_unimpl_137(void) { return j_unimplemented_slot(137); }
static juint j_unimpl_138(void) { return j_unimplemented_slot(138); }
static juint j_unimpl_139(void) { return j_unimplemented_slot(139); }
static juint j_unimpl_140(void) { return j_unimplemented_slot(140); }
static juint j_unimpl_141(void) { return j_unimplemented_slot(141); }
static juint j_unimpl_142(void) { return j_unimplemented_slot(142); }
static juint j_unimpl_143(void) { return j_unimplemented_slot(143); }
static juint j_unimpl_144(void) { return j_unimplemented_slot(144); }
static juint j_unimpl_145(void) { return j_unimplemented_slot(145); }
static juint j_unimpl_146(void) { return j_unimplemented_slot(146); }
static juint j_unimpl_147(void) { return j_unimplemented_slot(147); }
static juint j_unimpl_148(void) { return j_unimplemented_slot(148); }
static juint j_unimpl_149(void) { return j_unimplemented_slot(149); }
static juint j_unimpl_150(void) { return j_unimplemented_slot(150); }
static juint j_unimpl_151(void) { return j_unimplemented_slot(151); }
static juint j_unimpl_152(void) { return j_unimplemented_slot(152); }
static juint j_unimpl_153(void) { return j_unimplemented_slot(153); }
static juint j_unimpl_154(void) { return j_unimplemented_slot(154); }
static juint j_unimpl_155(void) { return j_unimplemented_slot(155); }
static juint j_unimpl_156(void) { return j_unimplemented_slot(156); }
static juint j_unimpl_157(void) { return j_unimplemented_slot(157); }
static juint j_unimpl_158(void) { return j_unimplemented_slot(158); }
static juint j_unimpl_159(void) { return j_unimplemented_slot(159); }
static juint j_unimpl_160(void) { return j_unimplemented_slot(160); }
static juint j_unimpl_161(void) { return j_unimplemented_slot(161); }
static juint j_unimpl_162(void) { return j_unimplemented_slot(162); }
static juint j_unimpl_163(void) { return j_unimplemented_slot(163); }
static juint j_unimpl_164(void) { return j_unimplemented_slot(164); }
static juint j_unimpl_165(void) { return j_unimplemented_slot(165); }
static juint j_unimpl_166(void) { return j_unimplemented_slot(166); }
static juint j_unimpl_167(void) { return j_unimplemented_slot(167); }
static juint j_unimpl_168(void) { return j_unimplemented_slot(168); }
static juint j_unimpl_169(void) { return j_unimplemented_slot(169); }
static juint j_unimpl_170(void) { return j_unimplemented_slot(170); }
static juint j_unimpl_171(void) { return j_unimplemented_slot(171); }
static juint j_unimpl_172(void) { return j_unimplemented_slot(172); }
static juint j_unimpl_173(void) { return j_unimplemented_slot(173); }
static juint j_unimpl_174(void) { return j_unimplemented_slot(174); }
static juint j_unimpl_175(void) { return j_unimplemented_slot(175); }
static juint j_unimpl_176(void) { return j_unimplemented_slot(176); }
static juint j_unimpl_177(void) { return j_unimplemented_slot(177); }
static juint j_unimpl_178(void) { return j_unimplemented_slot(178); }
static juint j_unimpl_179(void) { return j_unimplemented_slot(179); }
static juint j_unimpl_180(void) { return j_unimplemented_slot(180); }
static juint j_unimpl_181(void) { return j_unimplemented_slot(181); }
static juint j_unimpl_182(void) { return j_unimplemented_slot(182); }
static juint j_unimpl_183(void) { return j_unimplemented_slot(183); }
static juint j_unimpl_184(void) { return j_unimplemented_slot(184); }
static juint j_unimpl_185(void) { return j_unimplemented_slot(185); }
static juint j_unimpl_186(void) { return j_unimplemented_slot(186); }
static juint j_unimpl_187(void) { return j_unimplemented_slot(187); }
static juint j_unimpl_188(void) { return j_unimplemented_slot(188); }
static juint j_unimpl_189(void) { return j_unimplemented_slot(189); }
static juint j_unimpl_190(void) { return j_unimplemented_slot(190); }
static juint j_unimpl_191(void) { return j_unimplemented_slot(191); }
static juint j_unimpl_192(void) { return j_unimplemented_slot(192); }
static juint j_unimpl_193(void) { return j_unimplemented_slot(193); }
static juint j_unimpl_194(void) { return j_unimplemented_slot(194); }
static juint j_unimpl_195(void) { return j_unimplemented_slot(195); }
static juint j_unimpl_196(void) { return j_unimplemented_slot(196); }
static juint j_unimpl_197(void) { return j_unimplemented_slot(197); }
static juint j_unimpl_198(void) { return j_unimplemented_slot(198); }
static juint j_unimpl_199(void) { return j_unimplemented_slot(199); }
static juint j_unimpl_200(void) { return j_unimplemented_slot(200); }
static juint j_unimpl_201(void) { return j_unimplemented_slot(201); }
static juint j_unimpl_202(void) { return j_unimplemented_slot(202); }
static juint j_unimpl_203(void) { return j_unimplemented_slot(203); }
static juint j_unimpl_204(void) { return j_unimplemented_slot(204); }
static juint j_unimpl_205(void) { return j_unimplemented_slot(205); }
static juint j_unimpl_206(void) { return j_unimplemented_slot(206); }
static juint j_unimpl_207(void) { return j_unimplemented_slot(207); }
static juint j_unimpl_208(void) { return j_unimplemented_slot(208); }
static juint j_unimpl_209(void) { return j_unimplemented_slot(209); }
static juint j_unimpl_210(void) { return j_unimplemented_slot(210); }
static juint j_unimpl_211(void) { return j_unimplemented_slot(211); }
static juint j_unimpl_212(void) { return j_unimplemented_slot(212); }
static juint j_unimpl_213(void) { return j_unimplemented_slot(213); }
static juint j_unimpl_214(void) { return j_unimplemented_slot(214); }
static juint j_unimpl_215(void) { return j_unimplemented_slot(215); }
static juint j_unimpl_216(void) { return j_unimplemented_slot(216); }
static juint j_unimpl_217(void) { return j_unimplemented_slot(217); }
static juint j_unimpl_218(void) { return j_unimplemented_slot(218); }
static juint j_unimpl_219(void) { return j_unimplemented_slot(219); }
static juint j_unimpl_220(void) { return j_unimplemented_slot(220); }
static juint j_unimpl_221(void) { return j_unimplemented_slot(221); }
static juint j_unimpl_222(void) { return j_unimplemented_slot(222); }
static juint j_unimpl_223(void) { return j_unimplemented_slot(223); }
static juint j_unimpl_224(void) { return j_unimplemented_slot(224); }
static juint j_unimpl_225(void) { return j_unimplemented_slot(225); }
static juint j_unimpl_226(void) { return j_unimplemented_slot(226); }
static juint j_unimpl_227(void) { return j_unimplemented_slot(227); }
static juint j_unimpl_228(void) { return j_unimplemented_slot(228); }
static juint j_unimpl_229(void) { return j_unimplemented_slot(229); }
static juint j_unimpl_230(void) { return j_unimplemented_slot(230); }
static juint j_unimpl_231(void) { return j_unimplemented_slot(231); }
static juint j_unimpl_232(void) { return j_unimplemented_slot(232); }


// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (env) *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version;
  if (env) *env = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  kv_load();

  env_table[0] = (void *)j_unimpl_0;
  env_table[1] = (void *)j_unimpl_1;
  env_table[2] = (void *)j_unimpl_2;
  env_table[3] = (void *)j_unimpl_3;
  env_table[4] = (void *)j_unimpl_4;
  env_table[5] = (void *)j_unimpl_5;
  env_table[6] = (void *)j_unimpl_6;
  env_table[7] = (void *)j_unimpl_7;
  env_table[8] = (void *)j_unimpl_8;
  env_table[9] = (void *)j_unimpl_9;
  env_table[10] = (void *)j_unimpl_10;
  env_table[11] = (void *)j_unimpl_11;
  env_table[12] = (void *)j_unimpl_12;
  env_table[13] = (void *)j_unimpl_13;
  env_table[14] = (void *)j_unimpl_14;
  env_table[15] = (void *)j_unimpl_15;
  env_table[16] = (void *)j_unimpl_16;
  env_table[17] = (void *)j_unimpl_17;
  env_table[18] = (void *)j_unimpl_18;
  env_table[19] = (void *)j_unimpl_19;
  env_table[20] = (void *)j_unimpl_20;
  env_table[21] = (void *)j_unimpl_21;
  env_table[22] = (void *)j_unimpl_22;
  env_table[23] = (void *)j_unimpl_23;
  env_table[24] = (void *)j_unimpl_24;
  env_table[25] = (void *)j_unimpl_25;
  env_table[26] = (void *)j_unimpl_26;
  env_table[27] = (void *)j_unimpl_27;
  env_table[28] = (void *)j_unimpl_28;
  env_table[29] = (void *)j_unimpl_29;
  env_table[30] = (void *)j_unimpl_30;
  env_table[31] = (void *)j_unimpl_31;
  env_table[32] = (void *)j_unimpl_32;
  env_table[33] = (void *)j_unimpl_33;
  env_table[34] = (void *)j_unimpl_34;
  env_table[35] = (void *)j_unimpl_35;
  env_table[36] = (void *)j_unimpl_36;
  env_table[37] = (void *)j_unimpl_37;
  env_table[38] = (void *)j_unimpl_38;
  env_table[39] = (void *)j_unimpl_39;
  env_table[40] = (void *)j_unimpl_40;
  env_table[41] = (void *)j_unimpl_41;
  env_table[42] = (void *)j_unimpl_42;
  env_table[43] = (void *)j_unimpl_43;
  env_table[44] = (void *)j_unimpl_44;
  env_table[45] = (void *)j_unimpl_45;
  env_table[46] = (void *)j_unimpl_46;
  env_table[47] = (void *)j_unimpl_47;
  env_table[48] = (void *)j_unimpl_48;
  env_table[49] = (void *)j_unimpl_49;
  env_table[50] = (void *)j_unimpl_50;
  env_table[51] = (void *)j_unimpl_51;
  env_table[52] = (void *)j_unimpl_52;
  env_table[53] = (void *)j_unimpl_53;
  env_table[54] = (void *)j_unimpl_54;
  env_table[55] = (void *)j_unimpl_55;
  env_table[56] = (void *)j_unimpl_56;
  env_table[57] = (void *)j_unimpl_57;
  env_table[58] = (void *)j_unimpl_58;
  env_table[59] = (void *)j_unimpl_59;
  env_table[60] = (void *)j_unimpl_60;
  env_table[61] = (void *)j_unimpl_61;
  env_table[62] = (void *)j_unimpl_62;
  env_table[63] = (void *)j_unimpl_63;
  env_table[64] = (void *)j_unimpl_64;
  env_table[65] = (void *)j_unimpl_65;
  env_table[66] = (void *)j_unimpl_66;
  env_table[67] = (void *)j_unimpl_67;
  env_table[68] = (void *)j_unimpl_68;
  env_table[69] = (void *)j_unimpl_69;
  env_table[70] = (void *)j_unimpl_70;
  env_table[71] = (void *)j_unimpl_71;
  env_table[72] = (void *)j_unimpl_72;
  env_table[73] = (void *)j_unimpl_73;
  env_table[74] = (void *)j_unimpl_74;
  env_table[75] = (void *)j_unimpl_75;
  env_table[76] = (void *)j_unimpl_76;
  env_table[77] = (void *)j_unimpl_77;
  env_table[78] = (void *)j_unimpl_78;
  env_table[79] = (void *)j_unimpl_79;
  env_table[80] = (void *)j_unimpl_80;
  env_table[81] = (void *)j_unimpl_81;
  env_table[82] = (void *)j_unimpl_82;
  env_table[83] = (void *)j_unimpl_83;
  env_table[84] = (void *)j_unimpl_84;
  env_table[85] = (void *)j_unimpl_85;
  env_table[86] = (void *)j_unimpl_86;
  env_table[87] = (void *)j_unimpl_87;
  env_table[88] = (void *)j_unimpl_88;
  env_table[89] = (void *)j_unimpl_89;
  env_table[90] = (void *)j_unimpl_90;
  env_table[91] = (void *)j_unimpl_91;
  env_table[92] = (void *)j_unimpl_92;
  env_table[93] = (void *)j_unimpl_93;
  env_table[94] = (void *)j_unimpl_94;
  env_table[95] = (void *)j_unimpl_95;
  env_table[96] = (void *)j_unimpl_96;
  env_table[97] = (void *)j_unimpl_97;
  env_table[98] = (void *)j_unimpl_98;
  env_table[99] = (void *)j_unimpl_99;
  env_table[100] = (void *)j_unimpl_100;
  env_table[101] = (void *)j_unimpl_101;
  env_table[102] = (void *)j_unimpl_102;
  env_table[103] = (void *)j_unimpl_103;
  env_table[104] = (void *)j_unimpl_104;
  env_table[105] = (void *)j_unimpl_105;
  env_table[106] = (void *)j_unimpl_106;
  env_table[107] = (void *)j_unimpl_107;
  env_table[108] = (void *)j_unimpl_108;
  env_table[109] = (void *)j_unimpl_109;
  env_table[110] = (void *)j_unimpl_110;
  env_table[111] = (void *)j_unimpl_111;
  env_table[112] = (void *)j_unimpl_112;
  env_table[113] = (void *)j_unimpl_113;
  env_table[114] = (void *)j_unimpl_114;
  env_table[115] = (void *)j_unimpl_115;
  env_table[116] = (void *)j_unimpl_116;
  env_table[117] = (void *)j_unimpl_117;
  env_table[118] = (void *)j_unimpl_118;
  env_table[119] = (void *)j_unimpl_119;
  env_table[120] = (void *)j_unimpl_120;
  env_table[121] = (void *)j_unimpl_121;
  env_table[122] = (void *)j_unimpl_122;
  env_table[123] = (void *)j_unimpl_123;
  env_table[124] = (void *)j_unimpl_124;
  env_table[125] = (void *)j_unimpl_125;
  env_table[126] = (void *)j_unimpl_126;
  env_table[127] = (void *)j_unimpl_127;
  env_table[128] = (void *)j_unimpl_128;
  env_table[129] = (void *)j_unimpl_129;
  env_table[130] = (void *)j_unimpl_130;
  env_table[131] = (void *)j_unimpl_131;
  env_table[132] = (void *)j_unimpl_132;
  env_table[133] = (void *)j_unimpl_133;
  env_table[134] = (void *)j_unimpl_134;
  env_table[135] = (void *)j_unimpl_135;
  env_table[136] = (void *)j_unimpl_136;
  env_table[137] = (void *)j_unimpl_137;
  env_table[138] = (void *)j_unimpl_138;
  env_table[139] = (void *)j_unimpl_139;
  env_table[140] = (void *)j_unimpl_140;
  env_table[141] = (void *)j_unimpl_141;
  env_table[142] = (void *)j_unimpl_142;
  env_table[143] = (void *)j_unimpl_143;
  env_table[144] = (void *)j_unimpl_144;
  env_table[145] = (void *)j_unimpl_145;
  env_table[146] = (void *)j_unimpl_146;
  env_table[147] = (void *)j_unimpl_147;
  env_table[148] = (void *)j_unimpl_148;
  env_table[149] = (void *)j_unimpl_149;
  env_table[150] = (void *)j_unimpl_150;
  env_table[151] = (void *)j_unimpl_151;
  env_table[152] = (void *)j_unimpl_152;
  env_table[153] = (void *)j_unimpl_153;
  env_table[154] = (void *)j_unimpl_154;
  env_table[155] = (void *)j_unimpl_155;
  env_table[156] = (void *)j_unimpl_156;
  env_table[157] = (void *)j_unimpl_157;
  env_table[158] = (void *)j_unimpl_158;
  env_table[159] = (void *)j_unimpl_159;
  env_table[160] = (void *)j_unimpl_160;
  env_table[161] = (void *)j_unimpl_161;
  env_table[162] = (void *)j_unimpl_162;
  env_table[163] = (void *)j_unimpl_163;
  env_table[164] = (void *)j_unimpl_164;
  env_table[165] = (void *)j_unimpl_165;
  env_table[166] = (void *)j_unimpl_166;
  env_table[167] = (void *)j_unimpl_167;
  env_table[168] = (void *)j_unimpl_168;
  env_table[169] = (void *)j_unimpl_169;
  env_table[170] = (void *)j_unimpl_170;
  env_table[171] = (void *)j_unimpl_171;
  env_table[172] = (void *)j_unimpl_172;
  env_table[173] = (void *)j_unimpl_173;
  env_table[174] = (void *)j_unimpl_174;
  env_table[175] = (void *)j_unimpl_175;
  env_table[176] = (void *)j_unimpl_176;
  env_table[177] = (void *)j_unimpl_177;
  env_table[178] = (void *)j_unimpl_178;
  env_table[179] = (void *)j_unimpl_179;
  env_table[180] = (void *)j_unimpl_180;
  env_table[181] = (void *)j_unimpl_181;
  env_table[182] = (void *)j_unimpl_182;
  env_table[183] = (void *)j_unimpl_183;
  env_table[184] = (void *)j_unimpl_184;
  env_table[185] = (void *)j_unimpl_185;
  env_table[186] = (void *)j_unimpl_186;
  env_table[187] = (void *)j_unimpl_187;
  env_table[188] = (void *)j_unimpl_188;
  env_table[189] = (void *)j_unimpl_189;
  env_table[190] = (void *)j_unimpl_190;
  env_table[191] = (void *)j_unimpl_191;
  env_table[192] = (void *)j_unimpl_192;
  env_table[193] = (void *)j_unimpl_193;
  env_table[194] = (void *)j_unimpl_194;
  env_table[195] = (void *)j_unimpl_195;
  env_table[196] = (void *)j_unimpl_196;
  env_table[197] = (void *)j_unimpl_197;
  env_table[198] = (void *)j_unimpl_198;
  env_table[199] = (void *)j_unimpl_199;
  env_table[200] = (void *)j_unimpl_200;
  env_table[201] = (void *)j_unimpl_201;
  env_table[202] = (void *)j_unimpl_202;
  env_table[203] = (void *)j_unimpl_203;
  env_table[204] = (void *)j_unimpl_204;
  env_table[205] = (void *)j_unimpl_205;
  env_table[206] = (void *)j_unimpl_206;
  env_table[207] = (void *)j_unimpl_207;
  env_table[208] = (void *)j_unimpl_208;
  env_table[209] = (void *)j_unimpl_209;
  env_table[210] = (void *)j_unimpl_210;
  env_table[211] = (void *)j_unimpl_211;
  env_table[212] = (void *)j_unimpl_212;
  env_table[213] = (void *)j_unimpl_213;
  env_table[214] = (void *)j_unimpl_214;
  env_table[215] = (void *)j_unimpl_215;
  env_table[216] = (void *)j_unimpl_216;
  env_table[217] = (void *)j_unimpl_217;
  env_table[218] = (void *)j_unimpl_218;
  env_table[219] = (void *)j_unimpl_219;
  env_table[220] = (void *)j_unimpl_220;
  env_table[221] = (void *)j_unimpl_221;
  env_table[222] = (void *)j_unimpl_222;
  env_table[223] = (void *)j_unimpl_223;
  env_table[224] = (void *)j_unimpl_224;
  env_table[225] = (void *)j_unimpl_225;
  env_table[226] = (void *)j_unimpl_226;
  env_table[227] = (void *)j_unimpl_227;
  env_table[228] = (void *)j_unimpl_228;
  env_table[229] = (void *)j_unimpl_229;
  env_table[230] = (void *)j_unimpl_230;
  env_table[231] = (void *)j_unimpl_231;
  env_table[232] = (void *)j_unimpl_232;

  env_table[4]  = (void *)j_GetVersion;
  env_table[6]  = (void *)j_FindClass;
  env_table[15] = (void *)j_ExceptionOccurred;
  env_table[16] = (void *)j_ExceptionClearDescribe; // ExceptionDescribe
  env_table[17] = (void *)j_ExceptionClearDescribe; // ExceptionClear
  env_table[19] = (void *)j_PushLocalFrame;
  env_table[20] = (void *)j_PopLocalFrame;
  env_table[21] = (void *)j_NewGlobalRef;
  env_table[22] = (void *)j_DeleteRef;  // DeleteGlobalRef
  env_table[23] = (void *)j_DeleteRef;  // DeleteLocalRef
  env_table[24] = (void *)j_ret0_3;     // IsSameObject
  env_table[25] = (void *)j_NewLocalRef;
  env_table[26] = (void *)j_ret0_2;     // EnsureLocalCapacity
  env_table[28] = (void *)j_NewObject;
  env_table[29] = (void *)j_NewObjectV;
  env_table[31] = (void *)j_GetObjectClass;
  env_table[33] = (void *)j_GetMethodID;
  env_table[34] = (void *)j_CallObjectMethod;
  env_table[35] = (void *)j_CallObjectMethodV;
  env_table[37] = (void *)j_CallBooleanMethod;
  env_table[38] = (void *)j_CallBooleanMethodV;
  env_table[49] = (void *)j_CallIntMethod;
  env_table[50] = (void *)j_CallIntMethodV;
  env_table[53] = (void *)j_CallLongMethodV;
  env_table[55] = (void *)j_CallFloatMethod;
  env_table[56] = (void *)j_CallFloatMethodV;
  env_table[61] = (void *)j_CallVoidMethod;
  env_table[62] = (void *)j_CallVoidMethodV;
  env_table[94] = (void *)j_GetFieldID;
  env_table[95] = (void *)j_GetObjectField;
  env_table[96] = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID;               // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;   // s0 must be set for float returns
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[36]  = (void *)j_CallObjectMethodV;
  env_table[39]  = (void *)j_CallBooleanMethodV;
  env_table[51]  = (void *)j_CallIntMethodV;
  env_table[54]  = (void *)j_CallLongMethodV;
  env_table[57]  = (void *)j_CallFloatMethodV;
  env_table[63]  = (void *)j_CallVoidMethodV;
  env_table[116] = (void *)j_CallStaticObjectMethodV;
  env_table[119] = (void *)j_CallStaticBooleanMethodV;
  env_table[131] = (void *)j_CallStaticIntMethodV;
  env_table[137] = (void *)j_CallStaticFloatMethodV;
  env_table[143] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;
  env_table[145] = (void *)j_GetObjectField;
  env_table[146] = (void *)j_GetBooleanField;
  env_table[150] = (void *)j_GetIntField;
  env_table[151] = (void *)j_GetLongField;
  env_table[152] = (void *)j_GetFloatField;
  env_table[144] = (void *)j_GetFieldID;                // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;            // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;           // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;               // GetStaticIntField
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[183] = (void *)j_GetPriArrayElements; // boolean
  env_table[184] = (void *)j_GetPriArrayElements; // byte
  env_table[185] = (void *)j_GetPriArrayElements; // char
  env_table[186] = (void *)j_GetPriArrayElements; // short
  env_table[187] = (void *)j_GetPriArrayElements; // int
  env_table[188] = (void *)j_GetPriArrayElements; // long
  env_table[189] = (void *)j_GetPriArrayElements; // float
  env_table[190] = (void *)j_GetPriArrayElements; // double
  for (int i = 191; i <= 198; i++)
    env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++)
    env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++)
    env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2;    // UnregisterNatives
  env_table[217] = (void *)j_ret0_2;    // MonitorEnter
  env_table[218] = (void *)j_ret0_2;    // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef; // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteRef;    // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
