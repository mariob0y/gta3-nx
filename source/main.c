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

  if (stat("lib/libGame.so", &st) == 0)
    s_game_so_path = "lib/libGame.so";
  else if (stat("libs/libGame.so", &st) == 0)
    s_game_so_path = "libs/libGame.so";
  else if (stat("libGame.so", &st) == 0)
    s_game_so_path = "libGame.so";
  else
    fatal_error("Could not find libGame.so.\nExtract libGame.so into /switch/gta3/lib/.");

  if (stat("lib/libc++_shared.so", &st) == 0)
    s_donor_so_path = "lib/libc++_shared.so";
  else if (stat("libs/libc++_shared.so", &st) == 0)
    s_donor_so_path = "libs/libc++_shared.so";
  else if (stat("libc++_shared.so", &st) == 0)
    s_donor_so_path = "libc++_shared.so";
  else
    fatal_error("Could not find libc++_shared.so.\nExtract libc++_shared.so into /switch/gta3/lib/.");

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

/* ---------------------------------------------------------------------------
 * Controller -> engine gamepad.
 *
 * libGame.so already contains a complete gamepad stack; it only ever sat idle
 * because nothing told it a pad was attached. implOnGamepadButtonDown/Up write
 * gamepads[pad].buttons[keycode] and queue a LIB_InputEvent, and every CPad
 * getter (GetSteeringLeftRight, WeaponJustDown, MenuInput*, ToggleMenuJustDown,
 * ...) reads that back through CHID::IsPressed / IsJustPressed. So the whole
 * game -- gameplay, frontend, cutscene skips -- is driven from this one table.
 *
 * Button IDs are NOT raw keycodes; they are the engine's CHIDJoystick numbering,
 * read out of CHIDJoystickXbox360's own default mapping table at 0x14a8dc:
 *   CROSS=0 CIRCLE=1 SQUARE=2 TRIANGLE=3 START=4 SELECT=5 L1=6 R1=7
 *   DPAD_UP=8 DPAD_DOWN=9 DPAD_LEFT=10 DPAD_RIGHT=11 L3=12 R3=13.
 * implOnGamepadButtonDown (0x36e2d4) bounds-checks keycode < 32, but the state
 * container behind it is a ButtonContainer<16>, so only 0..15 are real.
 *
 * L2/R2 have no button ID at all in this build -- the Xbox360 map reaches them
 * only as axes 68/69, so ZL/ZR go through implOnGamepadAxesChanged instead.
 *
 * Face buttons are matched by LETTER, not by position. The engine draws its
 * help icons from assets/es2/buttonsxbox360.png, and FindUVsFromMapping
 * (0x14accc) indexes that atlas by button ID: 0->A, 1->B, 2->X, 3->Y. Mapping
 * Switch A to CROSS therefore makes the on-screen glyph always name the button
 * actually under the thumb, and lands confirm on A / cancel on B, which is the
 * Nintendo convention. The cost is that the layout is rotated relative to the
 * PS2 original (enter-vehicle sits on Y rather than the top button).
 * --------------------------------------------------------------------------- */
typedef struct {
  u64 hid;
  int button;
} PadMap;

static const PadMap pad_map[] = {
  { HidNpadButton_A,       0 },  // CROSS    -- accelerate / confirm   (icon A)
  { HidNpadButton_B,       1 },  // CIRCLE   -- fire / cancel          (icon B)
  { HidNpadButton_X,       2 },  // SQUARE   -- brake                  (icon X)
  { HidNpadButton_Y,       3 },  // TRIANGLE -- enter vehicle          (icon Y)
  { HidNpadButton_Plus,    4 },  // START    -- pause menu
  { HidNpadButton_Minus,   5 },  // SELECT   -- radar / map
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
static void (* implOnGamepadCountChanged)(void *env, void *thiz, int count);
static int  (* CHID_GetInputType)(void);
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
  ENTOPT(implOnGamepadCountChanged, "implOnGamepadCountChanged");
  CHID_GetInputType = (void *)so_try_find_addr_rx(&game_mod, "_ZN4CHID12GetInputTypeEv");
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

/* CHID::CheckForInputChange (0x149198) will not build a CHIDJoystickXbox360 --
 * and so will not switch the game off touch controls or onto pad icons --
 * until OS_GamepadIsConnected(0) returns true. That flag lives in
 * gamepads[0]+56 and the only thing that sets it is implOnGamepadCountChanged.
 * The engine's own copy is edge-guarded, so calling it every frame would be
 * harmless, but tracking the change ourselves keeps the log readable and makes
 * hot-plug (Joy-Cons detached, docking, a Pro Controller paired mid-game)
 * report exactly once. */
static int pad_reported_count = -1;

static void report_gamepad_count(int count) {
  if (count == pad_reported_count || !implOnGamepadCountChanged)
    return;
  pad_reported_count = count;
  /* thiz is unused by the entry point (it is a static JNI method and the
   * disassembly never touches x1), so NULL matches the other input calls. */
  implOnGamepadCountChanged(fake_env, NULL, count);
  LOGC(LOGC_SYS, "[PAD] Reported %d gamepad(s) to the engine\n", count);
}

/* Whether any of this worked is a single observable: CHID::GetInputType()
 * returns 0 while the active HID instance is the touchscreen (none), 1 once
 * CheckForInputChange has built a CHIDJoystickXbox360, 2 for a keyboard. It is
 * a plain read of m_pInstance[currentInstanceIndex] plus one vtable call, so
 * polling it costs nothing and it is safe before the engine is up (a NULL
 * instance reports 0). Logging the transition is what distinguishes "the pad
 * is wired" from "the pad is wired and the engine agrees". */
static int chid_input_type_last = -1;

static void log_input_type_change(void) {
  if (!CHID_GetInputType)
    return;
  const int t = CHID_GetInputType();
  if (t == chid_input_type_last)
    return;
  chid_input_type_last = t;
  static const char *kNames[] = { "touchscreen", "joystick", "keyboard" };
  LOGC(LOGC_SYS, "[PAD] CHID input type -> %d (%s)\n", t,
       (t >= 0 && t <= 2) ? kNames[t] : "unknown");
}

/* ---- eager pad detection ------------------------------------------------------
 *
 * CHID::CheckForInputChange (0x149198) only builds a CHIDJoystickXbox360 --
 * which is also what makes CHID::Implements() start drawing pad icons -- once
 * it samples a button the engine's own ButtonContainer reports as currently
 * held (state 2) or just released (state 3), or a stick axis past 75%. Until
 * then CHID::GetInputType() stays 0 (touchscreen), so no icon can be shown no
 * matter what implOnGamepadButtonDown we send it.
 *
 * Left to the user, the first thing that satisfies that sample is whatever
 * they press first -- typically A on the "Tap Or Press A" splash, which is
 * ALSO CPad::MenuInputAcceptJustDown's button (mapping 37, bound to both
 * button 0 and button 4 in the Xbox360 map at 0x14a8dc). So the very press
 * that reveals the A icon is the same press that dismisses the screen the
 * icon was drawn on, and it is visible for at most one frame.
 *
 * L3 (button 12) is not bound to any CMenuManager navigation action -- only to
 * CPad::GetToggleSubmission (gameplay-only) and, jointly with R3, to
 * CPad::ToggleCheatMenu (0x219a48), which requires a release/press pair
 * across BOTH sticks and so cannot fire from a solo L3 tap (verified: with R3
 * never touched, every branch that could set the result reads R3's state and
 * stays false). A single synthetic L3 press the moment the pad is seen is
 * therefore invisible everywhere it might land, and it satisfies
 * CheckForInputChange before the user's first real press -- so every icon,
 * including this one, is already showing when the splash screen first draws,
 * and that first real press goes back to meaning only what it actually means. */
static void eager_pad_detect(void) {
  enum { PD_WAIT, PD_HOLDING, PD_DONE };
  static int state = PD_WAIT;
  static int hold_frames = 0;

  if (state == PD_WAIT) {
    if (pad_reported_count > 0) {
      if (implOnGamepadButtonDown) implOnGamepadButtonDown(fake_env, NULL, 0, 12);
      hold_frames = 0;
      state = PD_HOLDING;
    }
  } else if (state == PD_HOLDING) {
    /* A few frames of "held" comfortably clears the state-2 sample window
     * regardless of exactly when in the frame CheckForInputChange runs. */
    if (++hold_frames >= 3) {
      if (implOnGamepadButtonUp) implOnGamepadButtonUp(fake_env, NULL, 0, 12);
      state = PD_DONE;
    }
  }
}

/* ---- menu auto-repeat -------------------------------------------------------
 *
 * The engine gives the frontend one step per press and never repeats: the four
 * CPad::MenuInput*JustDown predicates resolve to CHID::IsJustPressed, which for
 * a digital button means LIB_GamepadState == 2 (true for a single frame) and for
 * a stick axis means a flag that CHIDJoystick::CacheAnalogValues (0x14a534) sets
 * only on the centre-to-deflected edge -- it clears the flag at the top of every
 * call and re-arms it only when the PREVIOUS cached sample was inside the 0.1
 * deadzone. So holding a direction moves the highlight exactly once, which is
 * what makes long lists feel slow.
 *
 * These latches add the missing repeat. They deliberately do not fire on the
 * initial press -- the engine already reports that one -- so a tap still moves
 * exactly one row and only a sustained hold repeats. Timing is in milliseconds
 * rather than frames because the port runs at either 30 or 60 fps depending on
 * config.fps_cap_30, and a frame-counted repeat would run at half speed on one
 * of them. game.c ORs these into the predicates; they live for exactly one main
 * loop iteration, the same lifetime as the engine's own just-pressed state, so
 * an unconsumed pulse can never leak into a later menu. */
/* One interval, used both before the first repeat and between every repeat
 * after it. An initial delay longer than the repeat gap is what made the
 * highlight sit still and then bolt: the movement has to read as one steady
 * rate, only a little quicker than re-flicking the stick by hand. The interval
 * still has to outlast a deliberate tap, or a single nudge would move two rows. */
#define MENU_REPEAT_MS 220
#define MENU_STICK_THRESHOLD 16000 /* ~50% of the +-32767 range */

volatile int g_menu_repeat_up = 0, g_menu_repeat_down = 0;
volatile int g_menu_repeat_left = 0, g_menu_repeat_right = 0;

static void update_menu_repeat(u64 down, HidAnalogStickState ls) {
  /* -1 / +1 per axis, from the d-pad or the left stick, whichever is active. */
  int x = 0, y = 0;
  if ((down & HidNpadButton_Left)  || ls.x < -MENU_STICK_THRESHOLD) x = -1;
  if ((down & HidNpadButton_Right) || ls.x >  MENU_STICK_THRESHOLD) x =  1;
  if ((down & HidNpadButton_Down)  || ls.y < -MENU_STICK_THRESHOLD) y = -1;
  if ((down & HidNpadButton_Up)    || ls.y >  MENU_STICK_THRESHOLD) y =  1;

  static int prev_x = 0, prev_y = 0;
  static u64 last_pulse_x = 0, last_pulse_y = 0;

  const u64 freq = armGetSystemTickFreq();
  const u64 now = armGetSystemTick();
  const u64 step = freq * MENU_REPEAT_MS / 1000;

  g_menu_repeat_up = g_menu_repeat_down = 0;
  g_menu_repeat_left = g_menu_repeat_right = 0;

  if (x != prev_x) { prev_x = x; last_pulse_x = now; }
  else if (x && now - last_pulse_x >= step) {
    last_pulse_x = now;
    if (x < 0) g_menu_repeat_left = 1; else g_menu_repeat_right = 1;
  }

  if (y != prev_y) { prev_y = y; last_pulse_y = now; }
  else if (y && now - last_pulse_y >= step) {
    last_pulse_y = now;
    if (y < 0) g_menu_repeat_down = 1; else g_menu_repeat_up = 1;
  }
}

static void update_gamepad(void) {
  padUpdate(&pad);
  report_gamepad_count(padIsConnected(&pad) ? 1 : 0);
  log_input_type_change();
  eager_pad_detect();

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
  update_menu_repeat(down, ls);

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
