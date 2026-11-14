// entry/src/main/cpp/napi_init.cpp
//
// Phase B: bring up the render thread and drive the first frame.
//
//   - loadBlenderLibs()          : dlopen + dlsym probe (kept for debug).
//   - extractRawfile()           : unchanged, called from ArkTS on boot.
//   - notifyDatafilesReady()     : unblocks the render thread.
//   - isBlenderReady()           : polled by ArkTS to hide splash.
//
// XComponent surface callbacks store the OHNativeWindow globally and
// spin up a render thread that: waits for datafiles → dlopens Blender
// → Blender_Init → per-frame Blender_Step at ~60 Hz.

#include "napi/native_api.h"
#include <hilog/log.h>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/ui_input_event.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "resource_extractor.h"

#undef  LOG_TAG
#define LOG_TAG "Blender_NAPI"
#define LOGI(f, ...) OH_LOG_INFO (LOG_APP, f, ##__VA_ARGS__)
#define LOGW(f, ...) OH_LOG_WARN (LOG_APP, f, ##__VA_ARGS__)
#define LOGE(f, ...) OH_LOG_ERROR(LOG_APP, f, ##__VA_ARGS__)

/* -------------------------------------------------------------------- */
/* Blender ABI + dependency list (unchanged).                           */

static const std::vector<std::string> kLibs = {
  "libz.so.1", "liblzma.so.5", "libbz2.so.1.0", "libdeflate.so.0",
  "libzstd.so.1", "libbrotlicommon.so.1", "libbrotlidec.so.1",
  "libexpat.so.1", "libminizip-ng.so.4", "libyaml-cpp.so.0.8",
  "libfmt.so.9", "libtbb.so.12",
  "libjpeg.so.62", "libpng16.so.16", "libtiff.so.6", "libwebp.so.7",
  "libsharpyuv.so.0", "libfreetype.so.6",
  "libImath-3_1.so.29", "libIex-3_2.so.31", "libIlmThread-3_2.so.31",
  "libOpenEXRCore-3_2.so.31", "libOpenEXR-3_2.so.31",
  "libOpenColorIO.so.2.4", "libOpenImageIO_Util.so.3.0",
  "libOpenImageIO.so.3.0", "libshaderc_shared.so.1",
  "libc++_shared.so", "libvulkan.so",
  "libblender.so",
};

// ── Blender public C API ────────────────────────────────────────────────────
typedef int  (*FnInit)(int, const char**);
typedef void (*FnStep)(void);
typedef int  (*FnIsRun)(void);
typedef void (*FnExit)(void);
typedef void (*FnSetNativeWindow)(void*, uint32_t, uint32_t);
/* STAGE_H_MULTIWINDOW: all input/resize ABIs now carry window_id. */
typedef void (*FnOnPointerEvent)(int, int, int, float, float);
typedef void (*FnOnSurfaceResize)(int, uint32_t, uint32_t);
typedef void (*FnOnKeyEvent)(int, int, int, int);
typedef void (*FnSetSubWindowHooks)(void (*)(int, int, int, const char *),
                                    void (*)(int));
typedef void (*FnOnSubWindowSurfaceReady)(int, void *, uint32_t, uint32_t);
typedef void (*FnOnSubWindowClosed)(int);

struct BlenderAPI {
    void*             handle          = nullptr;
    FnInit            Init            = nullptr;
    FnStep            Step            = nullptr;
    FnIsRun           IsRun           = nullptr;
    FnExit            Exit            = nullptr;
    FnSetNativeWindow SetNativeWindow = nullptr;
    FnOnPointerEvent  OnPointerEvent  = nullptr;
    FnOnSurfaceResize OnSurfaceResize = nullptr;
    FnOnKeyEvent      OnKeyEvent      = nullptr;
    FnSetSubWindowHooks       SetSubWindowHooks       = nullptr;
    FnOnSubWindowSurfaceReady OnSubWindowSurfaceReady = nullptr;
    FnOnSubWindowClosed       OnSubWindowClosed       = nullptr;
} g_blender;

// ── Symbol table (add one line here to probe a new export) ──────────────────
struct SymEntry { void** slot; const char* name; };
static const SymEntry kSyms[] = {
  { (void**)&g_blender.Init,            "Blender_Init"            },
  { (void**)&g_blender.Step,            "Blender_Step"            },
  { (void**)&g_blender.IsRun,           "Blender_IsRunning"       },
  { (void**)&g_blender.Exit,            "Blender_Exit"            },
  { (void**)&g_blender.SetNativeWindow, "Blender_SetNativeWindow" },
  { (void**)&g_blender.OnPointerEvent,  "Blender_OnPointerEvent"  },
  { (void**)&g_blender.OnSurfaceResize, "Blender_OnSurfaceResize" },
  { (void**)&g_blender.OnKeyEvent,      "Blender_OnKeyEvent"      },
  { (void**)&g_blender.SetSubWindowHooks,       "Blender_SetSubWindowHooks"       },
  { (void**)&g_blender.OnSubWindowSurfaceReady, "Blender_OnSubWindowSurfaceReady" },
  { (void**)&g_blender.OnSubWindowClosed,       "Blender_OnSubWindowClosed"       },
};

/* -------------------------------------------------------------------- */
/* Render context + signals.                                            */

static struct RenderCtx {
  OHNativeWindow   *win = nullptr;
  uint32_t          w   = 0;
  uint32_t          h   = 0;
  std::atomic<bool> running{false};
  std::atomic<bool> inited{false};
} g_rc;

static std::atomic<bool> g_datafiles_ready{false};

/* Incremented after every successful Blender_Step return in the render
 * thread. ArkTS polls this via getFrameCount() to hide the splash overlay
 * only once real rendering has ticked past a few frames — avoids the
 * "isBlenderReady=true but screen still black" gap. Approximates first
 * present, since each Step() enqueues a Vulkan swap. */
static std::atomic<uint64_t> g_frame_count{0};
static std::thread       g_render_thread;

/* -------------------------------------------------------------------- */
/* Library / symbol loading (shared by loadBlenderLibs() and the render
 * thread).                                                              */

static std::vector<std::string> read_maps_paths()
{
  std::vector<std::string> paths;
  std::ifstream f("/proc/self/maps");
  std::string line;
  std::unordered_set<std::string> seen;
  while (std::getline(f, line)) {
    auto slash = line.find('/');
    if (slash == std::string::npos) continue;
    std::string p = line.substr(slash);
    if (p.find(".so") == std::string::npos) continue;
    if (!seen.insert(p).second) continue;
    paths.push_back(std::move(p));
  }
  return paths;
}

static std::string find_loaded_path(const std::string &soname,
                                     const std::vector<std::string> &maps)
{
  for (const auto &p : maps) {
    auto slash = p.find_last_of('/');
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    if (base == soname) return p;
  }
  return {};
}

/* Load every dep + libblender.so, then dlsym Blender_* into g_blender.
 * Idempotent: if handle already resolved, returns success immediately. */
/* Sub-window TSFN + host hooks — defined before LoadBlenderAndDeps so the
 * deferred-install code at the end of that function can reference them. */
static napi_threadsafe_function g_subwin_tsfn = nullptr;
static void host_request_create(int id, int w, int h, const char *title);
static void host_request_destroy(int id);

static bool LoadBlenderAndDeps(std::string *summary_out)
{
  if (g_blender.handle && g_blender.Init) {
    if (summary_out) *summary_out = "already loaded";
    return true;
  }
  const std::string libs_dir = "/data/storage/el1/bundle/libs/arm64";

  int ok = 0, fail = 0;
  void *blender_h = nullptr;
  for (const auto &soname : kLibs) {
    const std::string on_disk = libs_dir + "/" + soname;
    const char *present = (access(on_disk.c_str(), F_OK) == 0) ? "present" : "absent ";
    dlerror();
    void *h = dlopen(soname.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h) {
      ok++;
      /* silent on success; summary line printed at end */
      if (soname == "libblender.so") blender_h = h;
    } else {
      fail++;
      const char *e = dlerror();
      LOGE("[dlopen ERR] %{public}-30s | disk:%{public}s | %{public}s",
           soname.c_str(), present, e ? e : "unknown");
    }
  }

  int sym_ok = 0, sym_fail = 0;
  if (blender_h) {
    g_blender.handle = blender_h;
    for (const auto &e : kSyms) {
      dlerror();
      *e.slot = dlsym(g_blender.handle, e.name);
      const char *err = dlerror();
      if (*e.slot && !err) {
        /* silent on success; summary line printed at end */
        sym_ok++;
      } else {
        LOGE("[dlsym  ERR] %{public}-38s | %{public}s", e.name, err ? err : "null");
        sym_fail++;
      }
    }
  } else {
    LOGE("libblender.so not loaded, skipping dlsym");
    sym_fail = sizeof(kSyms) / sizeof(kSyms[0]);
  }

  std::ostringstream ss;
  ss << "libs: " << ok << "/" << kLibs.size() << " ok (" << fail
     << " fail); syms: " << sym_ok << "/" << (int)(sizeof(kSyms) / sizeof(kSyms[0]))
     << " ok (" << sym_fail << " fail)";
  const std::string summary = ss.str();
  LOGI("SUMMARY: %{public}s", summary.c_str());
  if (summary_out) *summary_out = summary;

    return g_blender.Init && g_blender.Step && g_blender.SetNativeWindow &&
         g_blender.IsRun && g_blender.Exit;
}

/* -------------------------------------------------------------------- */
/* Quick sanity: is Vulkan loader present + does OHOS_surface exist?     */

static bool ProbeVulkan()
{
  auto get_instance =
      (PFN_vkGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr");
  if (!get_instance) {
    LOGE("vkGetInstanceProcAddr missing — libvulkan.so not loaded?");
    return false;
  }
  auto enum_ext = (PFN_vkEnumerateInstanceExtensionProperties)
                  get_instance(nullptr, "vkEnumerateInstanceExtensionProperties");
  if (!enum_ext) { LOGE("vkEnumerateInstanceExtensionProperties missing"); return false; }

  uint32_t n = 0;
  enum_ext(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> props(n);
  enum_ext(nullptr, &n, props.data());

  bool khr = false, ohos = false;
  for (auto &p : props) {
    if (std::string(p.extensionName) == "VK_KHR_surface")  khr  = true;
    if (std::string(p.extensionName) == "VK_OHOS_surface") ohos = true;
  }
  // LOGI("Vulkan ext scan: KHR_surface=%{public}d OHOS_surface=%{public}d",
  //      khr ? 1 : 0, ohos ? 1 : 0);
  return khr && ohos;
}

/* -------------------------------------------------------------------- */
/* Render thread: waits for datafiles, dlopens Blender, drives Step().  */

static void RenderThreadMain()
{
  LOGI(">>> RenderThread enter");

  if (!ProbeVulkan()) { LOGE("Vulkan unusable, bailing"); return; }
  if (!LoadBlenderAndDeps(nullptr)) { LOGE("Blender load failed, bailing"); return; }

  /* Wait for ArkTS to signal "extraction done" (up to 30 s). */
  int waited = 0;
  while (!g_datafiles_ready.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    waited += 50;
    if (waited % 2000 == 0) {
      LOGI("RenderThread: waiting for datafiles (%{public}d ms)", waited);
    }
    if (waited >= 30000) { LOGE("datafiles wait timed out, proceeding anyway"); break; }
  }
  LOGI("RenderThread: datafiles ready, proceeding");

  g_blender.SetNativeWindow(g_rc.win, g_rc.w, g_rc.h);

  /* No --factory-startup: Blender reads $HOME/.config/blender/<ver>/
   * config/{startup,userpref}.blend on init, so user preference tweaks
   * persist across launches. ArkTS seeds userpref.blend on first
   * launch based on device type (see Index.seedUserPref). */
  const char *argv[] = { "blender", "--gpu-backend", "vulkan", nullptr };
  const int ret = g_blender.Init(3, argv);
  LOGI("Blender_Init returned %{public}d", ret);
  g_rc.inited = (ret == 0);
  /* Install sub-window hooks now that the GHOST system is alive.
   * Blender_Init creates GHOST_SystemOHOS; ohos_system() returns
   * non-null only after that. */
  if (g_subwin_tsfn && g_blender.SetSubWindowHooks) {
    g_blender.SetSubWindowHooks(host_request_create, host_request_destroy);
    LOGI("[SubWin] hooks installed (after Blender_Init)");
  }

  /* 60 Hz frame loop. Blender_Step runs one iteration of the WM main
   * loop; we cap the pace here so we don't burn a CPU core spinning. */
  while (g_rc.running.load()) {
    if (g_rc.inited && g_blender.IsRun()) {
      const auto t0 = std::chrono::steady_clock::now();
      g_blender.Step();
      g_frame_count.fetch_add(1, std::memory_order_relaxed);
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
      if (elapsed < 16) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  if (g_rc.inited && g_blender.Exit) g_blender.Exit();
  LOGI("<<< RenderThread exit");
}

/* -------------------------------------------------------------------- */
/* XComponent surface callbacks — the only inputs Step 5a needs.        */

static void ohos_configure_native_window(OHNativeWindow *nw, uint32_t w, uint32_t h)
{
  if (!nw || w == 0 || h == 0) return;
  int32_t ret = OH_NativeWindow_NativeWindowHandleOpt(
      nw, SET_BUFFER_GEOMETRY, (int32_t)w, (int32_t)h);
  if (ret != 0) {
    LOGE("SET_BUFFER_GEOMETRY failed ret=%{public}d %{public}ux%{public}u", ret, w, h);
  }
}

/* STAGE_H_XCOMP_MAP
 *
 * Per-XComponent binding: each XComponent has a string id ("blender_canvas"
 * for the main window, "blender_sub_<N>" for sub-windows). We derive the
 * numeric window_id from the id string once, cache it, and use it in every
 * dispatch callback to route events to the correct Blender window. */
static std::mutex g_xcomp_mu;
static std::unordered_map<OH_NativeXComponent *, int> g_xcomp_to_wid;

static int parse_wid_from_xcomp_id(const char *id_str)
{
  if (!id_str || id_str[0] == '\0') {
    return -1;
  }
  /* Main window canvas gets id 0. */
  if (std::strcmp(id_str, "blender_canvas") == 0) {
    return 0;
  }
  /* Sub-window canvases follow the "blender_sub_<N>" convention. */
  static const char kPrefix[] = "blender_sub_";
  const size_t plen = sizeof(kPrefix) - 1;
  if (std::strncmp(id_str, kPrefix, plen) == 0) {
    const char *n = id_str + plen;
    int v = 0;
    while (*n >= '0' && *n <= '9') {
      v = v * 10 + (*n - '0');
      ++n;
    }
    return (*n == '\0') ? v : -1;
  }
  return -1;
}

static int xcomp_id_of(OH_NativeXComponent *comp)
{
  if (!comp) return -1;
  {
    std::lock_guard<std::mutex> lk(g_xcomp_mu);
    auto it = g_xcomp_to_wid.find(comp);
    if (it != g_xcomp_to_wid.end()) return it->second;
  }
  char id_buf[128] = {0};
  uint64_t id_len = sizeof(id_buf);
  OH_NativeXComponent_GetXComponentId(comp, id_buf, &id_len);
  const int wid = parse_wid_from_xcomp_id(id_buf);
  if (wid >= 0) {
    std::lock_guard<std::mutex> lk(g_xcomp_mu);
    g_xcomp_to_wid[comp] = wid;
  }
  return wid;
}

/* STAGE_H_SURFACE_DISPATCH
 *
 * Sub-windows go through their own life cycle: the render thread for the
 * main window (id 0) is started on its surface creation and shared across
 * every window. Sub-window surface events are just forwarded to
 * libblender.so so GHOST_SystemOHOS can wake createWindow's wait. */
static void OnSurfaceCreated(OH_NativeXComponent *comp, void *window)
{
  uint64_t w = 0, h = 0;
  OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
  const int wid = xcomp_id_of(comp);
  LOGI("[XComp] surfaceCreated wid=%{public}d nw=%{public}p %{public}llux%{public}llu",
       wid, window, (unsigned long long)w, (unsigned long long)h);
  auto *nw = static_cast<OHNativeWindow *>(window);
  if (nw) OH_NativeWindow_NativeObjectReference(nw);
  if (wid > 0) {
    /* Sub-window: unblock GHOST_SystemOHOS::createWindow via the C ABI.
     * The main-window fast path below is skipped entirely. */
    if (g_blender.OnSubWindowSurfaceReady) {
      g_blender.OnSubWindowSurfaceReady(wid, nw, (uint32_t)w, (uint32_t)h);
    }
    ohos_configure_native_window(nw, (uint32_t)w, (uint32_t)h);
    return;
  }
  /* Main window (wid=0 or wid=-1 if the id string was not recognised). */

  /* nw was already retained above; here just swap with the previous
   * cached surface if it differs. */
  if (g_rc.win && g_rc.win != nw) OH_NativeWindow_NativeObjectUnreference(g_rc.win);

  g_rc.win = nw;
  g_rc.w   = (uint32_t)w;
  g_rc.h   = (uint32_t)h;
  ohos_configure_native_window(g_rc.win, g_rc.w, g_rc.h);

  if (!g_render_thread.joinable()) {
    g_rc.running = true;
    g_render_thread = std::thread(RenderThreadMain);
  }
}

static void OnSurfaceChanged(OH_NativeXComponent *comp, void *window)
{
  uint64_t w = 0, h = 0;
  OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
  const int wid = xcomp_id_of(comp);
  if (wid > 0) {
    /* Sub-window: just forward the new extent; no swapchain owned here. */
    if (g_blender.OnSurfaceResize) {
      g_blender.OnSurfaceResize(wid, (uint32_t)w, (uint32_t)h);
    }
    return;
  }
  auto *nw = static_cast<OHNativeWindow *>(window);
  if (nw && nw != g_rc.win) {
    OH_NativeWindow_NativeObjectReference(nw);
    if (g_rc.win) OH_NativeWindow_NativeObjectUnreference(g_rc.win);
    g_rc.win = nw;
    if (g_blender.SetNativeWindow) {
      g_blender.SetNativeWindow(g_rc.win, (uint32_t)w, (uint32_t)h);
    }
  }
  g_rc.w = (uint32_t)w;
  g_rc.h = (uint32_t)h;
  ohos_configure_native_window(g_rc.win, g_rc.w, g_rc.h);
  /* Push the new size into the render thread. GHOST_SystemOHOS
   * queues it, then on the next processEvents() tick tears down
   * the Vulkan swapchain and emits a size/expose event so wm
   * relayouts and redraws against the real surface. */
  if (g_blender.OnSurfaceResize) {
    g_blender.OnSurfaceResize(/*window_id=*/0, g_rc.w, g_rc.h);
  }
  LOGI("[XComp] surfaceChanged %{public}ux%{public}u",
       (unsigned)w, (unsigned)h);
}

static void OnSurfaceDestroyed(OH_NativeXComponent *comp, void * /*window*/)
{
  const int wid = xcomp_id_of(comp);
  LOGI("[XComp] surfaceDestroyed wid=%{public}d", wid);
  if (wid > 0) {
    /* Sub-window: tell libblender its surface is gone. It will push a
     * WindowClose event through the normal wm path. */
    if (g_blender.OnSubWindowClosed) {
      g_blender.OnSubWindowClosed(wid);
    }
    {
      std::lock_guard<std::mutex> lk(g_xcomp_mu);
      g_xcomp_to_wid.erase(comp);
    }
    return;
  }
  /* Main window: stop the render thread and drop the surface reference. */
  g_rc.running = false;
  if (g_render_thread.joinable()) g_render_thread.join();
  if (g_rc.win) { OH_NativeWindow_NativeObjectUnreference(g_rc.win); g_rc.win = nullptr; }
}

/* -------------------------------------------------------------------- */
/* STAGE_C_GESTURE_APPLIED
 *
 * XComponent touch bridge with two-finger gesture recognition.
 *   1 finger  -> left-button click / drag (Stage B behaviour preserved).
 *   2 fingers -> MMB drag (viewport rotate/pan) + pinch wheel (zoom).
 *
 * Mouse and touchpad synthesised touch events (physical HID reported
 * via the touch channel) are filtered by source type so a real mouse
 * click does not also fire a spurious touch-down. */

/* Persistent gesture state; lives on the UI thread only. */
/* STAGE_C_GESTURE_LOCKED
 *
 * Blender's view3d rotate modal (entered on MMB down) intercepts
 * wheel events, so we cannot drive rotate and zoom simultaneously.
 * Instead lock the gesture into one mode on first significant
 * motion: pinch dominates -> ZOOM (wheel only); pan dominates ->
 * PAN (MMB drag only). */
enum GestureMode { GESTURE_NONE, GESTURE_ZOOM, GESTURE_PAN };

static struct GestureState {
  bool  active          = false;
  bool  suppress_single = false;
  GestureMode mode      = GESTURE_NONE;
  /* Anchor at gesture start; used to decide lock direction. */
  float start_mid_x     = 0.0f;
  float start_mid_y     = 0.0f;
  float start_dist      = 0.0f;
  /* Previous frame values; used for per-frame deltas. */
  float prev_mid_x      = 0.0f;
  float prev_mid_y      = 0.0f;
  float prev_dist       = 0.0f;
  float wheel_acc       = 0.0f;
} g_gesture;

static void OnDispatchTouchEvent(OH_NativeXComponent *comp, void *window)
{
  const int wid = xcomp_id_of(comp);  /* STAGE_H_DISPATCH_WID */
  if (!g_blender.OnPointerEvent) {
    return;
  }
  OH_NativeXComponent_TouchEvent ev;
  if (OH_NativeXComponent_GetTouchEvent(comp, window, &ev) != 0) {
    return;
  }

  /* Route mouse/touchpad-synthesised touch to the mouse-move path only;
   * their button transitions come via the dedicated mouse callback. */
  OH_NativeXComponent_EventSourceType src_type = OH_NATIVEXCOMPONENT_SOURCE_TYPE_UNKNOWN;
  OH_NativeXComponent_GetTouchEventSourceType(comp, ev.id, &src_type);
  if (src_type == OH_NATIVEXCOMPONENT_SOURCE_TYPE_MOUSE ||
      src_type == OH_NATIVEXCOMPONENT_SOURCE_TYPE_TOUCHPAD) {
    if (ev.type == OH_NATIVEXCOMPONENT_MOVE) {
      g_blender.OnPointerEvent(wid, 0 /* mouse-move */, 0, ev.x, ev.y);
    }
    return;
  }

  const int   n  = (int)ev.numPoints;
  const float x1 = ev.touchPoints[0].x, y1 = ev.touchPoints[0].y;
  const float x2 = (n >= 2) ? ev.touchPoints[1].x : 0.0f;
  const float y2 = (n >= 2) ? ev.touchPoints[1].y : 0.0f;

  /* ── Two-finger gesture: MMB drag + pinch-to-zoom ─────────────── */
  if (n >= 2) {
    const float mid_x = (x1 + x2) * 0.5f;
    const float mid_y = (y1 + y2) * 0.5f;
    const float dx    = x2 - x1, dy = y2 - y1;
    const float dist  = sqrtf(dx * dx + dy * dy);

    if (!g_gesture.active) {
      /* Enter gesture: anchor start pose, defer mode selection. */
      g_gesture.active          = true;
      g_gesture.suppress_single = true;
      g_gesture.mode            = GESTURE_NONE;
      g_gesture.wheel_acc       = 0.0f;
      g_gesture.start_mid_x     = mid_x;
      g_gesture.start_mid_y     = mid_y;
      g_gesture.start_dist      = dist;
      g_gesture.prev_mid_x      = mid_x;
      g_gesture.prev_mid_y      = mid_y;
      g_gesture.prev_dist       = dist;
      /* Cancel any single-finger drag that was in progress. */
      g_blender.OnPointerEvent(wid, 13 /* touch-cancel */, 0, mid_x, mid_y);
      return;
    }

    /* Continue: lock mode if not yet locked, then dispatch. */
    const float LOCK_PX = 15.0f;
    if (g_gesture.mode == GESTURE_NONE) {
      const float ddist = fabsf(dist - g_gesture.start_dist);
      const float dmid_x = mid_x - g_gesture.start_mid_x;
      const float dmid_y = mid_y - g_gesture.start_mid_y;
      const float dmove = sqrtf(dmid_x * dmid_x + dmid_y * dmid_y);
      if (ddist < LOCK_PX && dmove < LOCK_PX) {
        /* Not decisive yet; keep observing. */
        g_gesture.prev_mid_x = mid_x;
        g_gesture.prev_mid_y = mid_y;
        g_gesture.prev_dist  = dist;
        return;
      }
      if (ddist >= dmove) {
        g_gesture.mode = GESTURE_ZOOM;
      }
      else {
        g_gesture.mode = GESTURE_PAN;
        /* Enter rotate modal at the current midpoint. */
        g_blender.OnPointerEvent(wid, 0 /* cursor-move */, 0, mid_x, mid_y);
        g_blender.OnPointerEvent(wid, 1 /* MMB-down    */, 1, mid_x, mid_y);
      }
    }

    if (g_gesture.mode == GESTURE_ZOOM) {
      /* Accumulate pinch delta into wheel steps. */
      const float STEP_PX = 30.0f;
      g_gesture.wheel_acc += dist - g_gesture.prev_dist;
      while (g_gesture.wheel_acc >= STEP_PX) {
        g_blender.OnPointerEvent(wid, 20 /* wheel */, 0, 0.0f, +1.0f);
        g_gesture.wheel_acc -= STEP_PX;
      }
      while (g_gesture.wheel_acc <= -STEP_PX) {
        g_blender.OnPointerEvent(wid, 20 /* wheel */, 0, 0.0f, -1.0f);
        g_gesture.wheel_acc += STEP_PX;
      }
    }
    else if (g_gesture.mode == GESTURE_PAN) {
      /* Drive MMB drag with the midpoint. */
      g_blender.OnPointerEvent(wid, 0 /* cursor-move */, 0, mid_x, mid_y);
    }

    g_gesture.prev_mid_x = mid_x;
    g_gesture.prev_mid_y = mid_y;
    g_gesture.prev_dist  = dist;
    return;
  }

  /* ── Second finger lifted: release MMB if we were panning ───── */
  if (g_gesture.active) {
    if (g_gesture.mode == GESTURE_PAN) {
      g_blender.OnPointerEvent(wid, 2 /* MMB-up */, 1, g_gesture.prev_mid_x, g_gesture.prev_mid_y);
    }
    g_gesture.active    = false;
    g_gesture.mode      = GESTURE_NONE;
    g_gesture.wheel_acc = 0.0f;
    return;
  }

  /* ── Suppress residual single-finger events after a gesture ─── */
  if (g_gesture.suppress_single) {
    if (ev.type == OH_NATIVEXCOMPONENT_DOWN) {
      g_gesture.suppress_single = false;
    }
    else {
      if ((ev.type == OH_NATIVEXCOMPONENT_UP ||
           ev.type == OH_NATIVEXCOMPONENT_CANCEL) && n <= 1) {
        g_gesture.suppress_single = false;
      }
      return;
    }
  }

  /* ── Single finger: left-button click / drag ──────────────────── */
  int kind;
  switch (ev.type) {
    case OH_NATIVEXCOMPONENT_DOWN:   kind = 10; break;
    case OH_NATIVEXCOMPONENT_MOVE:   kind = 11; break;
    case OH_NATIVEXCOMPONENT_UP:     kind = 12; break;
    case OH_NATIVEXCOMPONENT_CANCEL: kind = 13; break;
    default: return;
  }
  g_blender.OnPointerEvent(wid, kind, (int)ev.id, ev.x, ev.y);
}



static OH_NativeXComponent_Callback g_xcomp_cb = {
  OnSurfaceCreated, OnSurfaceChanged, OnSurfaceDestroyed, OnDispatchTouchEvent,
};

/* -------------------------------------------------------------------- */
/* NAPI exports.                                                        */

static napi_value Add(napi_env env, napi_callback_info info)
{
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double a = 0, b = 0;
  napi_get_value_double(env, argv[0], &a);
  napi_get_value_double(env, argv[1], &b);
  napi_value r;
  napi_create_double(env, a + b, &r);
  return r;
}

static napi_value LoadBlenderLibs(napi_env env, napi_callback_info /*info*/)
{
  std::string summary;
  LoadBlenderAndDeps(&summary);
  napi_value r;
  napi_create_string_utf8(env, summary.c_str(), summary.size(), &r);
  return r;
}

static napi_value ExtractRawfile(napi_env env, napi_callback_info info)
{
  size_t argc = 4;
  napi_value argv[4] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  NativeResourceManager *rm = OH_ResourceManager_InitNativeResourceManager(env, argv[0]);
  if (!rm) {
    const char *err = "extract: cannot init NativeResourceManager";
    LOGE("%{public}s", err);
    napi_value o; napi_create_string_utf8(env, err, strlen(err), &o); return o;
  }

  char archive[512] = {0}, extract_to[1024] = {0};
  size_t slen = 0;
  napi_get_value_string_utf8(env, argv[1], archive,    sizeof(archive)    - 1, &slen);
  napi_get_value_string_utf8(env, argv[2], extract_to, sizeof(extract_to) - 1, &slen);
  bool force = false;
  napi_get_value_bool(env, argv[3], &force);

  // LOGI("extractRawfile: %{public}s -> %{public}s (force=%{public}d)",
  //      archive, extract_to, (int)force);

  auto r = blender_ohos::ExtractRawfileArchive(rm, archive, extract_to, force);
  OH_ResourceManager_ReleaseNativeResourceManager(rm);

  std::ostringstream ss;
  ss << (r.ok ? (r.skipped ? "[skip] " : "[ok  ] ") : "[FAIL] ")
     << r.archive << ": " << r.message;
  if (r.ok && !r.skipped) {
    ss << " (" << r.files_written << " files, " << r.dirs_created << " dirs, "
       << (r.uncompressed_bytes / 1024) << " KB out)";
  }
  const std::string summary = ss.str();
  LOGI("%{public}s", summary.c_str());

  napi_value out;
  napi_create_string_utf8(env, summary.c_str(), summary.size(), &out);
  return out;
}

static napi_value SetPublicHome(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  char path[1024] = {0};
  size_t len = 0;
  if (argc > 0) {
    napi_get_value_string_utf8(env, argv[0], path, sizeof(path) - 1, &len);
  }
  if (len > 0) {
    setenv("DSHM_PUBLIC_HOME", path, 1);
    LOGI("DSHM_PUBLIC_HOME=%{public}s", path);
  }
  napi_value out;
  napi_get_boolean(env, len > 0, &out);
  return out;
}

static napi_value NotifyDatafilesReady(napi_env /*env*/, napi_callback_info /*info*/)
{
  g_datafiles_ready = true;
  LOGI("notifyDatafilesReady: signal received");
  return nullptr;
}

static napi_value IsBlenderReady(napi_env env, napi_callback_info /*info*/)
{
  napi_value r;
  napi_get_boolean(env, g_rc.inited.load(), &r);
  return r;
}

/* NAPI: getFrameCount() → number of Blender_Step iterations executed
 * by the render thread. ArkTS polls this to detect that Blender has
 * moved past init and is actually producing frames, so the splash
 * overlay can be hidden without leaving the user staring at a black
 * XComponent for a second or two. */
static napi_value GetFrameCount(napi_env env, napi_callback_info /*info*/)
{
  napi_value r;
  /* uint64 doesn't fit napi_create_int64 for extreme values, but a
   * render session capped at ~60 Hz would take ~4900 years to overflow
   * int64 so this is fine in practice. */
  napi_create_int64(env, static_cast<int64_t>(g_frame_count.load()), &r);
  return r;
}

/* -------------------------------------------------------------------- */



/* -------------------------------------------------------------------- */
/* STAGE_G_INPUT_CALLBACKS
 *
 * Mouse / axis / key input callbacks. All are forwarded straight into
 * libblender.so through the stable Blender_On* C ABI (kind codes match
 * GHOST_SystemOHOS.cc). Modifier tracking for keys lives GHOST-side. */

/* PTR_* kind codes must stay in sync with GHOST_SystemOHOS.cc. */
enum : int {
  PTR_MOUSE_MOVE = 0,
  PTR_MOUSE_DOWN = 1,
  PTR_MOUSE_UP   = 2,
  PTR_WHEEL      = 20,
};

/* Physical mouse: MOVE/PRESS/RELEASE/CANCEL. Compensates for a lost UP
 * on CANCEL by releasing whatever buttons the mask still holds. */
static void OnDispatchMouseEvent(OH_NativeXComponent *comp, void *window)
{
  const int wid = xcomp_id_of(comp);  /* STAGE_H_DISPATCH_WID */
  if (!g_blender.OnPointerEvent) {
    return;
  }
  OH_NativeXComponent_MouseEvent ev;
  if (OH_NativeXComponent_GetMouseEvent(comp, window, &ev) != 0) {
    return;
  }
  int btn = 0;
  switch (ev.button) {
    case OH_NATIVEXCOMPONENT_LEFT_BUTTON:   btn = 0; break;
    case OH_NATIVEXCOMPONENT_MIDDLE_BUTTON: btn = 1; break;
    case OH_NATIVEXCOMPONENT_RIGHT_BUTTON:  btn = 2; break;
    default: break;
  }
  static uint32_t s_btn_mask = 0;
  int kind = -1;
  switch (ev.action) {
    case OH_NATIVEXCOMPONENT_MOUSE_MOVE:
      kind = PTR_MOUSE_MOVE;
      break;
    case OH_NATIVEXCOMPONENT_MOUSE_PRESS:
      kind = PTR_MOUSE_DOWN;
      s_btn_mask |= (1u << btn);
      break;
    case OH_NATIVEXCOMPONENT_MOUSE_RELEASE:
      kind = PTR_MOUSE_UP;
      s_btn_mask &= ~(1u << btn);
      break;
    case OH_NATIVEXCOMPONENT_MOUSE_CANCEL:
      /* Emit an UP for every button the mask still remembers as pressed
       * so GHOST's mask does not stay stuck after focus loss / DND. */
      for (int b = 0; b < 3; ++b) {
        if (s_btn_mask & (1u << b)) {
          g_blender.OnPointerEvent(wid, PTR_MOUSE_UP, b, ev.x, ev.y);
        }
      }
      s_btn_mask = 0;
      return;
    default:
      return;
  }
  g_blender.OnPointerEvent(wid, kind, btn, ev.x, ev.y);
}

/* Hover is currently a no-op; kept because RegisterMouseEventCallback's
 * table needs a non-null pointer to avoid crashes on some ROM builds. */
static void OnDispatchHover(OH_NativeXComponent * /*comp*/, bool /*enter*/) {}

/* Axis events: touchpad two-finger scroll or physical mouse wheel.
 * Fractional deltas are accumulated so slow scrolls still generate steps. */
static void OnAxisEvent(OH_NativeXComponent *comp,
                        ArkUI_UIInputEvent *event,
                        ArkUI_UIInputEvent_Type type)
{
  const int wid = xcomp_id_of(comp);  /* STAGE_H_DISPATCH_WID */
  if (type != ARKUI_UIINPUTEVENT_TYPE_AXIS || !g_blender.OnPointerEvent) {
    return;
  }
  const double v = OH_ArkUI_AxisEvent_GetVerticalAxisValue(event);
  static double s_acc = 0.0;
  s_acc += v;
  int steps = 0;
  /* Physical mouse wheel reports ±45.0 per click. */
  const double STEP = 45.0;
  while (s_acc >= STEP)  { steps++; s_acc -= STEP; }
  while (s_acc <= -STEP) { steps--; s_acc += STEP; }
  if (steps != 0) {
    g_blender.OnPointerEvent(wid, PTR_WHEEL, 0, 0.0f, float(-steps));
  }
}

/* Physical / soft keyboard events. metaState is the ArkUI modifier
 * bitmask (bit0=CTRL bit1=SHIFT bit2=ALT), forwarded verbatim so GHOST
 * can refresh its snapshot before mapping the key. Returns true to
 * consume the event so ArkUI doesn't bubble it further. */
static void OnKeyEvent(OH_NativeXComponent *comp, void * /*window*/)
{
  LOGI("==================== keyevent ====================");
  if (!g_blender.OnKeyEvent) {
    return;
  }
  OH_NativeXComponent_KeyEvent *ke = nullptr;
  if (OH_NativeXComponent_GetKeyEvent(comp, &ke) != 0 || !ke) {
    return;
  }
  OH_NativeXComponent_KeyAction action = OH_NATIVEXCOMPONENT_KEY_ACTION_UNKNOWN;
  OH_NativeXComponent_KeyCode   code   = OH_NativeXComponent_KeyCode(0);
  uint64_t mods = 0;
  OH_NativeXComponent_GetKeyEventAction(ke, &action);
  OH_NativeXComponent_GetKeyEventCode(ke, &code);
  OH_NativeXComponent_GetKeyEventModifierKeyStates(ke, &mods);

  const int wid = xcomp_id_of(comp);  /* STAGE_H_DISPATCH_WID */
  const int arkui_action = (action == OH_NATIVEXCOMPONENT_KEY_ACTION_DOWN) ? 1 : 0;
  g_blender.OnKeyEvent(wid, arkui_action, int(code), int(mods & 0xFFFFFFFFull));
}

static OH_NativeXComponent_MouseEvent_Callback g_mouse_cb = {
    OnDispatchMouseEvent,
    OnDispatchHover,
};

/* NAPI: sendKeyEvent(windowId, action, keyCode, metaState). Called from
 * ArkTS page-level onKeyEvent as a fallback for ROMs where the native
 * key callback misses events (soft-IME, some external keyboards). */
static napi_value SendKeyEvent(napi_env env, napi_callback_info info)
{
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 4 || !g_blender.OnKeyEvent) {
    return nullptr;
  }
  int32_t window_id = 0, action = 0, keyCode = 0, metaState = 0;
  napi_get_value_int32(env, argv[0], &window_id);
  napi_get_value_int32(env, argv[1], &action);
  napi_get_value_int32(env, argv[2], &keyCode);
  napi_get_value_int32(env, argv[3], &metaState);
  g_blender.OnKeyEvent(window_id, action, keyCode, metaState);
  return nullptr;
}



/* -------------------------------------------------------------------- */
/* STAGE_H_TSFN_BRIDGE
 *
 * Sub-window request bridge: Blender's main thread (inside createWindow)
 * asks us to create or destroy a sub-window; those requests are marshalled
 * to the ArkTS/UI thread via napi_threadsafe_function so ArkTS can call
 * parent.createSubWindowWithOptions. The reverse direction (surface ready /
 * closed / dispatched input) still goes through the C ABI. */

enum SubWinReqKind { SUBWIN_CREATE = 0, SUBWIN_DESTROY = 1, SUBWIN_FIRST_FRAME = 2 };

struct SubWinReq {
  int kind;
  int id;
  int w;
  int h;
  std::string title;
};


/* Called on the UI thread by the TSFN infrastructure. Wraps the request
 * into ArkTS-friendly values and calls the JS callback registered via
 * registerSubWindowCallback. */
static void tsfn_call_js(napi_env env, napi_value js_cb, void * /*ctx*/, void *data)
{
  std::unique_ptr<SubWinReq> req(static_cast<SubWinReq *>(data));
  if (!env || !js_cb || !req) {
    return;
  }
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  napi_value argv[5];
  napi_create_int32(env, req->kind, &argv[0]);
  napi_create_int32(env, req->id, &argv[1]);
  napi_create_int32(env, req->w, &argv[2]);
  napi_create_int32(env, req->h, &argv[3]);
  napi_create_string_utf8(env, req->title.c_str(), req->title.size(), &argv[4]);
  napi_call_function(env, undefined, js_cb, 5, argv, nullptr);
}

/* Called from the render thread (GHOST_SystemOHOS::createWindow) via
 * Blender_SetSubWindowHooks. */
static void host_request_create(int id, int w, int h, const char *title)
{
  if (!g_subwin_tsfn) {
    LOGE("[SubWin] request_create id=%{public}d but TSFN is null", id);
    return;
  }
  auto *req = new SubWinReq{SUBWIN_CREATE, id, w, h, title ? title : ""};
  napi_call_threadsafe_function(g_subwin_tsfn, req, napi_tsfn_nonblocking);
}

static void host_request_destroy(int id)
{
  if (!g_subwin_tsfn) return;
  auto *req = new SubWinReq{SUBWIN_DESTROY, id, 0, 0, ""};
  napi_call_threadsafe_function(g_subwin_tsfn, req, napi_tsfn_nonblocking);
}

/* NAPI: registerSubWindowCallback(cb). ArkTS side keeps a reference to
 * the callback function; we wrap it in a TSFN and hand ownership to the
 * native side. Calling again releases the previous TSFN. */
static napi_value RegisterSubWindowCallback(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    return nullptr;
  }
  if (g_subwin_tsfn) {
    napi_release_threadsafe_function(g_subwin_tsfn, napi_tsfn_release);
    g_subwin_tsfn = nullptr;
  }
  napi_value name;
  napi_create_string_utf8(env, "SubWinTSFN", NAPI_AUTO_LENGTH, &name);
  napi_status s = napi_create_threadsafe_function(
      env, argv[0], nullptr, name, 0, 1, nullptr, nullptr, nullptr,
      tsfn_call_js, &g_subwin_tsfn);
  if (s != napi_ok) {
    LOGE("[SubWin] napi_create_threadsafe_function failed: %{public}d", (int)s);
    return nullptr;
  }
  /* Install hooks now that we have a TSFN to route through. */
  if (g_blender.SetSubWindowHooks) {
    g_blender.SetSubWindowHooks(host_request_create, host_request_destroy);
    LOGI("[SubWin] hooks installed");
  }
  else {
    LOGI("[SubWin] hooks pending (libblender not loaded yet)");
  }
  return nullptr;
}

/* NAPI: bindSubXComponent(xcompObj). SubWindowPage.ets calls this from
 * onLoad so the second/third/... XComponent gets its own callback
 * registrations. Without this, only the XComponent found in `exports`
 * during module Init would be wired up. */
static napi_value BindSubXComponent(napi_env env, napi_callback_info info)
{
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    return nullptr;
  }
  OH_NativeXComponent *nx = nullptr;
  if (napi_unwrap(env, argv[0], (void **)&nx) != napi_ok || !nx) {
    LOGE("[SubWin] bindSubXComponent: napi_unwrap failed");
    return nullptr;
  }
  OH_NativeXComponent_RegisterCallback(nx, &g_xcomp_cb);
  OH_NativeXComponent_RegisterMouseEventCallback(nx, &g_mouse_cb);
  OH_NativeXComponent_RegisterKeyEventCallback(nx, OnKeyEvent);
  OH_NativeXComponent_RegisterUIInputEventCallback(nx, OnAxisEvent,
                                                   ARKUI_UIINPUTEVENT_TYPE_AXIS);
  const int wid = xcomp_id_of(nx);
  LOGI("[SubWin] bindSubXComponent wired wid=%{public}d", wid);
  return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
  const napi_property_descriptor desc[] = {
    { "add",                  nullptr, Add,                  nullptr, nullptr, nullptr, napi_default, nullptr },
    { "loadBlenderLibs",      nullptr, LoadBlenderLibs,      nullptr, nullptr, nullptr, napi_default, nullptr },
    { "extractRawfile",       nullptr, ExtractRawfile,       nullptr, nullptr, nullptr, napi_default, nullptr },
    { "setPublicHome",        nullptr, SetPublicHome,        nullptr, nullptr, nullptr, napi_default, nullptr },
    { "notifyDatafilesReady", nullptr, NotifyDatafilesReady, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "isBlenderReady",       nullptr, IsBlenderReady,       nullptr, nullptr, nullptr, napi_default, nullptr },
    { "getFrameCount",        nullptr, GetFrameCount,        nullptr, nullptr, nullptr, napi_default, nullptr },
    { "sendKeyEvent",             nullptr, SendKeyEvent,          nullptr, nullptr, nullptr, napi_default, nullptr },
    { "sendKeyEventW",            nullptr, SendKeyEvent,          nullptr, nullptr, nullptr, napi_default, nullptr },
    { "registerSubWindowCallback",nullptr, RegisterSubWindowCallback, nullptr, nullptr, nullptr, napi_default, nullptr },
    { "bindSubXComponent",        nullptr, BindSubXComponent,     nullptr, nullptr, nullptr, napi_default, nullptr },
  };
  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

  /* If this module was loaded via XComponent(libraryname:'entry'),
   * exports will carry OH_NATIVE_XCOMPONENT_OBJ — grab it and register
   * surface callbacks. Otherwise (plain import), skip silently. */
  napi_value xc;
  if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xc) == napi_ok) {
    OH_NativeXComponent *nx = nullptr;
    if (napi_unwrap(env, xc, (void **)&nx) == napi_ok && nx) {
      OH_NativeXComponent_RegisterCallback(nx, &g_xcomp_cb);
      /* Input dispatchers: mouse buttons/move, physical wheel/touchpad
       * axis, and hardware/soft keyboard. All forward into libblender.so. */
      OH_NativeXComponent_RegisterMouseEventCallback(nx, &g_mouse_cb);
      OH_NativeXComponent_RegisterKeyEventCallback(nx, OnKeyEvent);
      OH_NativeXComponent_RegisterUIInputEventCallback(nx, OnAxisEvent,
                                                       ARKUI_UIINPUTEVENT_TYPE_AXIS);
      char id_buf[128] = {0};
      uint64_t id_len = sizeof(id_buf);
      OH_NativeXComponent_GetXComponentId(nx, id_buf, &id_len);
      LOGI("[NapiInit] XComponent surface callbacks registered id=%{public}s", id_buf);
    }
  }
  return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&demoModule);
}
