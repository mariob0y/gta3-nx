/* main.c -- Grand Theft Auto III Nintendo Switch Port (gta3_nx)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <pthread.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "path_cache.h"
#include "stream_cache.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module donor_mod; // libc++_shared.so
so_module game_mod;  // libGame.so

size_t g_mem_total_mb = 0, g_mem_newlib_mb = 0, g_mem_so_mb = 0;
u32 __nx_nv_transfermem_size = 0x60000000; // 1.5 GB GPU memory pool

volatile int g_escape_pressed = 0;
volatile int g_select_pressed = 0;
uint32_t g_frame_count = 0;

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t so_region = (size_t)MEMORY_SO_MB * 1024 * 1024;
  if (so_region > size / 4)
    so_region = size / 4;
  fake_heap_size  = size - so_region;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_mem_total_mb  = size >> 20;
  g_mem_newlib_mb = fake_heap_size >> 20;
  g_mem_so_mb     = so_region >> 20;
}

static const char *s_donor_so_path = CXX_DONOR_SO_NAME;
static const char *s_game_so_path = SO_NAME;

static void check_data(void) {
  struct stat st;
  LOGC(LOGC_SYS, "[CHECK] Verifying data files and shared objects...\n");

  if (stat("libs/libGame.so", &st) == 0)
    s_game_so_path = "libs/libGame.so";
  else if (stat("libGame.so", &st) == 0)
    s_game_so_path = "libGame.so";
  else
    fatal_error("Could not find libGame.so.\nExtract libGame.so into /switch/gta3/libs/.");

  if (stat("libs/libc++_shared.so", &st) == 0)
    s_donor_so_path = "libs/libc++_shared.so";
  else if (stat("libc++_shared.so", &st) == 0)
    s_donor_so_path = "libc++_shared.so";
  else
    fatal_error("Could not find libc++_shared.so.\nExtract libc++_shared.so into /switch/gta3/libs/.");

  if (stat("assets", &st) < 0 && stat("assets/data", &st) < 0 && stat("assets/models", &st) < 0 &&
      stat("data", &st) < 0 && stat("models", &st) < 0 && stat("audio", &st) < 0)
    LOGC(LOGC_SYS, "[CHECK] WARNING: assets/ (or data/ models/ audio/) dir missing -- make sure game assets are extracted!\n");
  else
    LOGC(LOGC_SYS, "[CHECK] Pre-flight file check passed.\n");
}

static void check_syscalls(void) {
  debugPrintf("[CHECK] Checking libnx syscall hints...\n");
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
  debugPrintf("[CHECK] All required Switch kernel syscalls are available.\n");
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("[SYS] Target screen mode: %dx%d\n", screen_width, screen_height);
}

typedef struct {
  u64 hid;
  int button;
} PadMap;

static const PadMap pad_map[] = {
  { HidNpadButton_B,       0 },  // CROSS / Confirm
  { HidNpadButton_A,       1 },  // CIRCLE / Cancel
  { HidNpadButton_Y,       2 },  // SQUARE / Action
  { HidNpadButton_X,       3 },  // TRIANGLE / Enter vehicle
  { HidNpadButton_Plus,    4 },  // START / Menu
  { HidNpadButton_Minus,   5 },  // SELECT / Map
  { HidNpadButton_L,       6 },  // L1
  { HidNpadButton_R,       7 },  // R1
  { HidNpadButton_Up,      8 },  // DPAD_UP
  { HidNpadButton_Down,    9 },  // DPAD_DOWN
  { HidNpadButton_Left,   10 },  // DPAD_LEFT
  { HidNpadButton_Right,  11 },  // DPAD_RIGHT
  { HidNpadButton_StickL, 12 },  // L3
  { HidNpadButton_StickR, 13 },  // R3
};

static void (* implOnActivityCreated)(void *env, void *thiz, void *activity);
static void (* implOnActivityDestroyed)(void *env, void *thiz);
static void (* implOnInitialSetup)(void *env, void *thiz, void *activity, void *apk, void *names, void *paths);
static void (* implOnSurfaceCreated)(void *env, void *thiz);
static void (* implOnSurfaceChanged)(void *env, void *thiz, void *surface, int w, int h);
static void (* implOnSurfaceDestroyed)(void *env, void *thiz);
static void (* implOnDrawFrame)(void *env, void *thiz, float dt);
static void (* implOnResume)(void *env, void *thiz);
static void (* implOnPause)(void *env, void *thiz);
static int  (* implIsInitialized)(void *env, void *thiz);

volatile uint64_t g_frame_start_tick = 0;
Handle g_main_thread_handle = 0;

static void (* implOnTouchStart)(void *env, void *thiz, int id, float x, float y);
static void (* implOnTouchMove)(void *env, void *thiz, int id, float x, float y);
static void (* implOnTouchEnd)(void *env, void *thiz, int id, float x, float y);
static void (* implOnGamepadConnected)(void *env, void *thiz, int pad);
static void (* implOnGamepadButtonDown)(void *env, void *thiz, int pad, int keycode);
static void (* implOnGamepadButtonUp)(void *env, void *thiz, int pad, int keycode);
static void (* implOnGamepadAxesChanged)(void *env, void *thiz, int pad,
                                         float lx, float ly, float rx, float ry, float lt, float rt);
static void (* implOnBackButtonPressed)(void *env, void *thiz);

static void (* implOnPlaylistOpenComplete)(void *env, void *thiz, int success, int count);
static void (* implOnRockstarInitialComplete)(void *env, void *thiz);
static void (* implOnRockstarGateComplete)(void *env, void *thiz, int gate, int success);
static void (* implOnRockstarSignInComplete)(void *env, void *thiz);
static void (* implOnRockstarSignOutComplete)(void *env, void *thiz);
static void (* implOnRockstarStateChanged)(void *env, void *thiz, int state);

#define GN "Java_com_rockstargames_oswrapper_GameNative_"

static void resolve_entry_points(void) {
  debugPrintf("[RESOLVE] Resolving GameNative JNI entry points...\n");
  #define ENT(var, sym) var = (void *)so_find_addr_rx(&game_mod, GN sym); debugPrintf("[RESOLVE] " GN sym " -> %p\n", var)
  #define ENTOPT(var, sym) var = (void *)so_try_find_addr_rx(&game_mod, GN sym); debugPrintf("[RESOLVE] (opt) " GN sym " -> %p\n", var)
  ENT(implOnActivityCreated, "implOnActivityCreated");
  ENT(implOnActivityDestroyed, "implOnActivityDestroyed");
  ENT(implOnInitialSetup, "implOnInitialSetup");
  ENT(implOnSurfaceCreated, "implOnSurfaceCreated");
  ENT(implOnSurfaceChanged, "implOnSurfaceChanged");
  ENT(implOnSurfaceDestroyed, "implOnSurfaceDestroyed");
  ENT(implOnDrawFrame, "implOnDrawFrame");
  ENT(implOnResume, "implOnResume");
  ENT(implOnPause, "implOnPause");
  ENT(implIsInitialized, "implIsInitialized");
  ENT(implOnTouchStart, "implOnTouchStart");
  ENT(implOnTouchMove, "implOnTouchMove");
  ENT(implOnTouchEnd, "implOnTouchEnd");
  ENTOPT(implOnGamepadConnected, "implOnGamepadConnected");
  ENTOPT(implOnGamepadButtonDown, "implOnGamepadButtonDown");
  ENTOPT(implOnGamepadButtonUp, "implOnGamepadButtonUp");
  ENTOPT(implOnGamepadAxesChanged, "implOnGamepadAxesChanged");
  ENTOPT(implOnBackButtonPressed, "implOnBackButtonPressed");
  ENTOPT(implOnPlaylistOpenComplete, "implOnPlaylistOpenComplete");
  ENTOPT(implOnRockstarInitialComplete, "implOnRockstarInitialComplete");
  ENTOPT(implOnRockstarGateComplete, "implOnRockstarGateComplete");
  ENTOPT(implOnRockstarSignInComplete, "implOnRockstarSignInComplete");
  ENTOPT(implOnRockstarSignOutComplete, "implOnRockstarSignOutComplete");
  ENTOPT(implOnRockstarStateChanged, "implOnRockstarStateChanged");
  #undef ENT
  #undef ENTOPT
  (void)implIsInitialized;
  (void)implOnBackButtonPressed;
}

#define MAX_TOUCHES 8
typedef struct { int active; u32 finger_id; float x, y; } TouchSlot;
static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 finger_id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == finger_id)
      return i;
  return -1;
}

static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active)
      return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;

  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;

  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count && i < MAX_TOUCHES; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0) continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
      debugPrintf("[TOUCH] Touch start slot=%d x=%.0f y=%.0f\n", slot, x, y);
      implOnTouchStart(fake_env, NULL, slot, x, y);
    } else if (x != touch_prev[slot].x || y != touch_prev[slot].y) {
      implOnTouchMove(fake_env, NULL, slot, x, y);
    }
    touch_prev[slot].x = x;
    touch_prev[slot].y = y;
    seen[slot] = 1;
  }

  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      debugPrintf("[TOUCH] Touch end slot=%d x=%.0f y=%.0f\n", slot, touch_prev[slot].x, touch_prev[slot].y);
      implOnTouchEnd(fake_env, NULL, slot, touch_prev[slot].x, touch_prev[slot].y);
      touch_prev[slot].active = 0;
    }
  }
}

static void dispatch_jni_callbacks(void) {
  JniCallback cb;
  int n = 0;
  while (n++ < 16 && jni_pop_callback(&cb)) {
    switch (cb.type) {
      case JNI_CB_PLAYLIST_OPEN_COMPLETE:
        debugPrintf("[JNI_DISPATCH] implOnPlaylistOpenComplete(%d, %d)\n", cb.arg0, cb.arg1);
        if (implOnPlaylistOpenComplete) implOnPlaylistOpenComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_INITIAL_COMPLETE:
        debugPrintf("[JNI_DISPATCH] implOnRockstarInitialComplete\n");
        if (implOnRockstarInitialComplete) implOnRockstarInitialComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_GATE_COMPLETE:
        debugPrintf("[JNI_DISPATCH] implOnRockstarGateComplete(%d, %d)\n", cb.arg0, cb.arg1);
        if (implOnRockstarGateComplete) implOnRockstarGateComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_SIGNIN_COMPLETE:
        debugPrintf("[JNI_DISPATCH] implOnRockstarSignInComplete\n");
        if (implOnRockstarSignInComplete) implOnRockstarSignInComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_SIGNOUT_COMPLETE:
        debugPrintf("[JNI_DISPATCH] implOnRockstarSignOutComplete\n");
        if (implOnRockstarSignOutComplete) implOnRockstarSignOutComplete(fake_env, NULL);
        break;
      default:
        break;
    }
  }
}

static PadState pad;
static u64 pad_prev = 0;

static void open_cheat_keyboard(void) {
  debugPrintf("[INPUT] Opening on-screen cheat keyboard...\n");
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetHeaderText(&kbd, "Enter GTA III cheat code");
  swkbdConfigSetStringLenMax(&kbd, 63);
  char out[64] = { 0 };
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc) && out[0])
    cheats_enqueue(out);
}

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  for (unsigned int i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
    if (changed & pad_map[i].hid) {
      if (down & pad_map[i].hid) {
        if (implOnGamepadButtonDown) implOnGamepadButtonDown(fake_env, NULL, 0, pad_map[i].button);
      } else {
        if (implOnGamepadButtonUp) implOnGamepadButtonUp(fake_env, NULL, 0, pad_map[i].button);
      }
    }
  }

  if ((changed & HidNpadButton_Plus) && (down & HidNpadButton_Plus))
    g_escape_pressed = 1;
  if ((changed & HidNpadButton_Minus) && (down & HidNpadButton_Minus))
    g_select_pressed = 1;

  const u64 cheat_combo = HidNpadButton_StickL | HidNpadButton_StickR;
  if ((changed & cheat_combo) && (down & cheat_combo) == cheat_combo)
    open_cheat_keyboard();

  pad_prev = down;

  const float scale = 1.f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  const float lx = (float)ls.x * scale;
  const float ly = (float)ls.y * -scale;
  const float rx = (float)rs.x * scale;
  const float ry = (float)rs.y * -scale;
  const float lt = (down & HidNpadButton_ZL) ? 1.0f : 0.0f;
  const float rt = (down & HidNpadButton_ZR) ? 1.0f : 0.0f;

  static float prev[6];
  if (lx != prev[0] || ly != prev[1] || rx != prev[2] ||
      ry != prev[3] || lt != prev[4] || rt != prev[5]) {
    prev[0] = lx; prev[1] = ly; prev[2] = rx; prev[3] = ry; prev[4] = lt; prev[5] = rt;
    if (implOnGamepadAxesChanged) implOnGamepadAxesChanged(fake_env, NULL, 0, lx, ly, rx, ry, lt, rt);
  }
}

void hard_exit(void) {
  debugPrintf("[EXIT] Performing hard exit routine...\n");
  debugPrintfFlush();
  thread_registry_pause_others();
  deinit_openal();
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
}

#include <signal.h>

static void crash_handler(int sig) {
  print_crash_snapshot(sig);
  exit(1);
}

static void install_crash_handlers(void) {
  signal(SIGSEGV, crash_handler);
  signal(SIGBUS,  crash_handler);
  signal(SIGABRT, crash_handler);
  signal(SIGILL,  crash_handler);
  signal(SIGFPE,  crash_handler);
  debugPrintf("[SYS] Signal-based crash handlers installed.\n");
}

int main(void) {
  install_crash_handlers();
  debugPrintf("[MAIN] Starting GTA III Nintendo Switch Port (gta3_nx v1.2-stalledit " __DATE__ " " __TIME__ ")...\n");

  cpu_boost(1);

  setenv("MESA_GLTHREAD", "true", 1);
  setenv("GALLIUM_THREAD", "0", 1);
  setenv("NOUVEAU_SWITCH_MAPPED_COMPLETION", "1", 1);

  mkdir("/switch/gta3/shadercache", 0777);
  setenv("MESA_SHADER_CACHE_DIR", "/switch/gta3/shadercache", 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);

  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();

  Result sock_rc = socketInitializeDefault();
  if (R_FAILED(sock_rc))
    debugPrintf("[MAIN] socketInitializeDefault failed: 0x%08x (networking disabled)\n", sock_rc);

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("[MAIN] RAM split: Total=%zu MB | Heap=%zu MB | .so region=%zu MB\n",
              g_mem_total_mb, g_mem_newlib_mb, g_mem_so_mb);
  debugPrintf("[MAIN] Transfermem pool: %u MB\n", (unsigned)(__nx_nv_transfermem_size >> 20));

  if (so_load(&donor_mod, s_donor_so_path, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", s_donor_so_path);

  void *game_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + donor_mod.load_size, 0x1000);
  const size_t game_limit = heap_so_limit - ((uintptr_t)game_base - (uintptr_t)heap_so_base);
  if (so_load(&game_mod, s_game_so_path, game_base, game_limit) < 0)
    fatal_error("Could not load\n%s.", s_game_so_path);

  update_imports();

  so_relocate(&donor_mod);
  so_relocate(&game_mod);
  so_resolve(&donor_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();

  resolve_entry_points();
  int (* JNI_OnLoad)(void *vm, void *reserved) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");

  so_finalize(&donor_mod);
  so_finalize(&game_mod);
  so_flush_caches(&donor_mod);
  so_flush_caches(&game_mod);

  so_execute_init_array(&donor_mod);
  so_execute_init_array(&game_mod);

  so_free_temp(&donor_mod);
  so_free_temp(&game_mod);

  jni_init();

  path_cache_init();
  sc_init();

  debugPrintf("[MAIN] Invoking JNI_OnLoad...\n");
  JNI_OnLoad(fake_vm, NULL);

  void *gn_class = jni_make_object("com/rockstargames/oswrapper/GameNative");
  void *activity = jni_make_object("GameActivity");
  void *asset_mgr = jni_make_object("AssetManager");
  void *surface   = jni_make_object("Surface");
  void *names = jni_make_string_array(0, NULL);
  void *paths = jni_make_string_array(0, NULL);

  debugPrintf("[MAIN] Triggering implOnActivityCreated...\n");
  implOnActivityCreated(fake_env, gn_class, activity);
  debugPrintf("[MAIN] implOnActivityCreated finished.\n");

  debugPrintf("[MAIN] Triggering implOnInitialSetup...\n");
  implOnInitialSetup(fake_env, gn_class, activity, asset_mgr, names, paths);
  debugPrintf("[MAIN] implOnInitialSetup finished.\n");

  debugPrintf("[MAIN] Triggering implOnSurfaceCreated...\n");
  implOnSurfaceCreated(fake_env, gn_class);
  debugPrintf("[MAIN] implOnSurfaceCreated finished.\n");

  debugPrintf("[MAIN] Triggering implOnSurfaceChanged (%dx%d)...\n", screen_width, screen_height);
  implOnSurfaceChanged(fake_env, gn_class, surface, screen_width, screen_height);
  debugPrintf("[MAIN] implOnSurfaceChanged finished.\n");

  debugPrintf("[MAIN] Triggering implOnResume...\n");
  implOnResume(fake_env, gn_class);
  debugPrintf("[MAIN] implOnResume finished.\n");

  if (implOnGamepadConnected)
    implOnGamepadConnected(fake_env, gn_class, 0);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  const u64 tick_freq = armGetSystemTickFreq();
  u64 last_tick = armGetSystemTick();
  const u64 frame_ticks = tick_freq / (config.fps_cap_30 ? 30 : 60);
  u64 next_frame_tick = last_tick + frame_ticks;
  int boot_frames = 0;

  g_main_thread_handle = envGetMainThreadHandle();
  pthread_t watchdog;
  if (pthread_create(&watchdog, NULL, stall_watchdog_thread, NULL) == 0) {
    pthread_detach(watchdog);
  }

  debugPrintf("[MAIN] Entering main frame execution loop...\n");

  static int s_heavy_frames = 0;
  static u64 s_boost_turn_off_tick = 0;
  static int s_boost_active = 0;

  while (appletMainLoop() && !jni_quit_requested) {
    g_frame_count++;
    g_active_phase = "JNICallbacks";
    dispatch_jni_callbacks();

    g_active_phase = "InputUpdate";
    update_gamepad();
    update_touch();

    const u64 now = armGetSystemTick();
    float real_dt = (float)(now - last_tick) / (float)tick_freq;
    float dt = real_dt;
    last_tick = now;
    if (dt < 1.0f / 120.0f || dt > 0.5f)
      dt = 1.0f / 60.0f;

    if (boot_frames < 5) {
      debugPrintf("[FRAME] Executing implOnDrawFrame dt=%.4f (frame #%d)\n", dt, boot_frames);
    } else if (boot_frames >= 10 && real_dt > 0.050f) {
      LOGC(LOGC_SYS, "[PERF_SPIKE] Long frame detected: dt=%.4f s (%.1f FPS)\n", real_dt, 1.0f / real_dt);
    }

    // Proactive CPU Boost Evaluation before entering frame work
    if (boot_frames >= 10) {
      bool active_streaming = (sc_debug_active_slots() > 0) || streaming_activity_recent(250.0);
      if (active_streaming) {
        if (!s_boost_active) {
          cpu_boost(1);
          s_boost_active = 1;
          LOGC(LOGC_SYS, "[CPU_BOOST] ON reason=world_stream (frame #%u)\n", g_frame_count);
        }
        s_heavy_frames = 0;
        s_boost_turn_off_tick = armGetSystemTick() + (u64)(tick_freq * 1.5f); // Keep boost active for 1.5s quiet
      } else if (real_dt > 0.040f) {
        s_heavy_frames++;
        if (s_heavy_frames >= 2 && !s_boost_active) {
          cpu_boost(1);
          s_boost_active = 1;
          LOGC(LOGC_SYS, "[CPU_BOOST] ON reason=heavy_frames (frame #%u, dt=%.4fs)\n", g_frame_count, real_dt);
        }
        if (s_boost_active)
          s_boost_turn_off_tick = armGetSystemTick() + (u64)(tick_freq * 1.5f); // Keep boost active for 1.5s quiet
      } else {
        s_heavy_frames = 0;
        if (s_boost_active && s_boost_turn_off_tick > 0 &&
            armGetSystemTick() >= s_boost_turn_off_tick) {
          cpu_boost(0);
          s_boost_active = 0;
          s_boost_turn_off_tick = 0;
          LOGC(LOGC_SYS, "[CPU_BOOST] OFF reason=quiet (frame #%u)\n", g_frame_count);
        }
      }
    }

    g_active_phase = "implOnDrawFrame";
    reset_frame_streaming_budget();
    g_frame_start_tick = armGetSystemTick();
    u64 t_frame0 = g_frame_start_tick;
    implOnDrawFrame(fake_env, NULL, dt);
    g_frame_start_tick = 0;
    g_active_phase = "FrameLimiter";

    double frame_work_ms = (double)armTicksToNs(armGetSystemTick() - t_frame0) / 1e6;
    emit_and_reset_frame_streaming_summary(frame_work_ms);
    if (frame_work_ms >= 5.0) {
      LOGC(LOGC_SYS, "[FRAME_SPLIT] implOnDrawFrame took %.2f ms (frame #%u)\n", frame_work_ms, g_frame_count);
    }
    keep_game_frame_limiter_off();

    // Check if a gameplay transition was observed by CPlayerPed::ProcessControl during this frame
    if (atomic_exchange_explicit(&g_gameplay_transition_pending, false, memory_order_acq_rel)) {
      streaming_set_context(STREAM_GAMEPLAY);
      set_gameplay_streaming_enabled(true);
      set_stream_runtime_mode(STREAM_RUNTIME_TRANSITION, 60);
      LOGC(LOGC_SYS, "[STREAM_POLICY] GAMEPLAY ACTIVATED AFTER FRAME=%u (transition grace=60 frames)\n", g_frame_count);
    } else {
      update_streaming_transition_frame();
    }

    if (boot_frames < 10) {
      if (++boot_frames == 10)
        cpu_boost(0);
    }

    if (config.fps_cap_30) {
      const u64 end = armGetSystemTick();
      if (end < next_frame_tick) {
        svcSleepThread((next_frame_tick - end) * 1000000000ULL / tick_freq);
      } else if (end - next_frame_tick >= frame_ticks) {
        next_frame_tick = end;
      }
      next_frame_tick += frame_ticks;
    } else {
      const u64 used = armGetSystemTick() - now;
      if (used < frame_ticks)
        svcSleepThread((frame_ticks - used) * 1000000000ULL / tick_freq);
    }
  }

  debugPrintf("[MAIN] Exited main frame loop, shutting down...\n");
  sc_shutdown();
  hard_exit();

  return 0;
}
