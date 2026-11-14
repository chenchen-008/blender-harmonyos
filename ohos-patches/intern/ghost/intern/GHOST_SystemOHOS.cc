/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_SystemOHOS.hh"
#include "GHOST_Event.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventWheel.hh"
#include "GHOST_ISystem.hh"

#include "GHOST_TimerManager.hh"
#include "GHOST_WindowManager.hh"
#include "GHOST_WindowOHOS.hh"

#include <cstdio>

#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

#include <chrono>

/* -------------------------------------------------------------------- */
/* Lifecycle. */

GHOST_SystemOHOS::GHOST_SystemOHOS() : GHOST_System()
{
  /* Nothing OHOS-specific to init yet; base class handles timer/event
   * managers, window manager, etc. */
}

GHOST_TSuccess GHOST_SystemOHOS::init()
{
  /* Base init creates the standard managers. On OHOS there is no
   * native event loop to hook, no display connection to open, so
   * there's nothing more we need to do here. */
  return GHOST_System::init();
}

/* -------------------------------------------------------------------- */
/* Windows / contexts. */

extern "C" {
extern void *g_ghost_ohos_native_window;
extern uint32_t g_ghost_ohos_win_w;
extern uint32_t g_ghost_ohos_win_h;
}

GHOST_IWindow *GHOST_SystemOHOS::createWindow(const char *title,
                                              int32_t left,
                                              int32_t top,
                                              uint32_t width,
                                              uint32_t height,
                                              GHOST_TWindowState state,
                                              GHOST_GPUSettings gpuSettings,
                                              const bool /*exclusive*/,
                                              const bool /*is_dialog*/,
                                              const GHOST_IWindow * /*parentWindow*/)
{
  /* First createWindow call is the main window (m_records is empty at that
   * point). Blender_SetNativeWindow has already stashed the XComponent
   * surface globally; we treat that as record[kMainWindowId] and go.
   *
   * Subsequent createWindow calls are sub-windows: ask the ArkTS host via
   * m_request_create to spin up a new XComponent, block on m_reg_cv until
   * onSubWindowSurfaceReady() reports the surface handle, then wrap it. */
  const bool stereo = (gpuSettings.flags & GHOST_gpuStereoVisual) != 0;
  const bool debug = (gpuSettings.flags & GHOST_gpuDebugContext) != 0;

  int win_id = -1;
  void *nw = nullptr;
  uint32_t win_w = width;
  uint32_t win_h = height;
  const bool is_main = m_records.empty();

  if (is_main) {
    if (!g_ghost_ohos_native_window) {
      return nullptr;
    }
    win_id = kMainWindowId;
    nw = g_ghost_ohos_native_window;
    if (g_ghost_ohos_win_w) win_w = g_ghost_ohos_win_w;
    if (g_ghost_ohos_win_h) win_h = g_ghost_ohos_win_h;
    /* Register the main window in the record table so drainInputQueue can
     * route id=0 events to it. */
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    WindowRecord rec;
    rec.id = win_id;
    rec.native_window = nw;
    rec.w = win_w;
    rec.h = win_h;
    rec.surface_ready = true;
    m_records[win_id] = rec;
  }
  else {
    if (!m_request_create) {
      /* No host hook installed: we cannot create sub-windows. */
      return nullptr;
    }
    win_id = allocateSubWindowId(width, height);
    m_request_create(win_id, int(width), int(height), title ? title : "Blender");
    WindowRecord *rec = waitForSurface(win_id, /*timeout_ms=*/5000);
    if (!rec) {
      if (m_request_destroy) m_request_destroy(win_id);
      std::lock_guard<std::mutex> lk(m_reg_mutex);
      m_records.erase(win_id);
      return nullptr;
    }
    nw = rec->native_window;
    win_w = rec->w;
    win_h = rec->h;
  }

  GHOST_WindowOHOS *window = new GHOST_WindowOHOS(this,
                                                  title,
                                                  left,
                                                  top,
                                                  win_w,
                                                  win_h,
                                                  state,
                                                  gpuSettings.context_type,
                                                  stereo,
                                                  debug,
                                                  /*native_window=*/nw,
                                                  /*window_id=*/win_id);
  if (!window->getValid()) {
    delete window;
    if (!is_main && m_request_destroy) {
      m_request_destroy(win_id);
    }
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    m_records.erase(win_id);
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    m_records[win_id].ghost_window = window;
  }
  m_windowManager->addWindow(window);
  m_windowManager->setActiveWindow(window);
  pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window));
  return window;
}

GHOST_IContext *GHOST_SystemOHOS::createOffscreenContext(GHOST_GPUSettings gpuSettings)
{
#ifdef WITH_VULKAN_BACKEND
  if (gpuSettings.context_type == GHOST_kDrawingContextTypeVulkan) {
    const bool debug = (gpuSettings.flags & GHOST_gpuDebugContext) != 0;
    /* Null window pointer selects the offscreen path in GHOST_ContextVK's
     * OHOS branch (added in Step 1). */
    GHOST_Context *context = new GHOST_ContextVK(
        false, nullptr, 1, 2, debug, gpuSettings.preferred_device);
    if (context->initializeDrawingContext()) {
      return context;
    }
    delete context;
    return nullptr;
  }
#endif
  (void)gpuSettings;
  return nullptr;
}

GHOST_TSuccess GHOST_SystemOHOS::disposeContext(GHOST_IContext *context)
{
  delete context;
  return GHOST_kSuccess;
}

GHOST_IWindow *GHOST_SystemOHOS::getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/)
{
  /* Single window: it's always the one under the cursor, if it exists. */
  const std::vector<GHOST_IWindow *> &windows = m_windowManager->getWindows();
  return windows.empty() ? nullptr : windows.front();
}

/* -------------------------------------------------------------------- */
/* Event pump. */

bool GHOST_SystemOHOS::processEvents(bool /*waitForEvent*/)
{
  bool any = false;
  if (drainInputQueue()) {
    any = true;
  }
  if (getTimerManager()->fireTimers(getMilliSeconds())) {
    any = true;
  }
  if (generateWindowExposeEvents()) {
    any = true;
  }
  return any;
}

/* Queue `window` for a redraw on the next processEvents() tick.
 * Duplicate pushes are harmless: generateWindowExposeEvents() clears
 * the set after emitting events, and Blender coalesces window updates. */
void GHOST_SystemOHOS::addDirtyWindow(GHOST_WindowOHOS *window)
{
  if (window) {
    m_dirty_windows.push_back(window);
  }
}

/* Emit one GHOST_kEventWindowUpdate per dirty window and clear the set.
 * Returns true iff at least one event was emitted. */
bool GHOST_SystemOHOS::generateWindowExposeEvents()
{
  if (m_dirty_windows.empty()) {
    return false;
  }
  const uint64_t now = getMilliSeconds();
  for (GHOST_WindowOHOS *w : m_dirty_windows) {
    pushEvent(new GHOST_Event(now, GHOST_kEventWindowUpdate, w));
  }
  m_dirty_windows.clear();
  return true;
}

/* -------------------------------------------------------------------- */
/* Time / displays. */

uint64_t GHOST_SystemOHOS::getMilliSeconds() const
{
  /* Monotonic clock — matches what the Unix backend uses via clock_gettime. */
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint8_t GHOST_SystemOHOS::getNumDisplays() const
{
  return 1;
}

void GHOST_SystemOHOS::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  /* No display metrics API from GHOST's side on OHOS; the ArkTS host
   * knows the real size and pushes it through Blender_SetNativeWindow /
   * the future resize hook. Reporting zeros is safe — callers that
   * care use the active window's client bounds. */
  width = 0;
  height = 0;
}

void GHOST_SystemOHOS::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  getMainDisplayDimensions(width, height);
}

/* -------------------------------------------------------------------- */
/* Input state stubs (Step 5 populates these). */

GHOST_TSuccess GHOST_SystemOHOS::getCursorPosition(int32_t &x, int32_t &y) const
{
  x = m_cursor_x.load(std::memory_order_relaxed);
  y = m_cursor_y.load(std::memory_order_relaxed);
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemOHOS::setCursorPosition(int32_t /*x*/, int32_t /*y*/)
{
  /* Touch platforms don't warp a cursor. */
  return GHOST_kFailure;
}

GHOST_TSuccess GHOST_SystemOHOS::getModifierKeys(GHOST_ModifierKeys &keys) const
{
  /* STAGE_G_KEY_APPLIED: report the snapshot maintained by
   * syncModifiersFromArkUI(). Blender queries this every wm tick
   * for keymap matching (Ctrl+something etc). */
  keys = m_ohos_modifier_keys;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemOHOS::getButtons(GHOST_Buttons &buttons) const
{
  const uint32_t m = m_button_mask.load(std::memory_order_relaxed);
  buttons.set(GHOST_kButtonMaskLeft,   (m & (1u << GHOST_kButtonMaskLeft))   != 0);
  buttons.set(GHOST_kButtonMaskMiddle, (m & (1u << GHOST_kButtonMaskMiddle)) != 0);
  buttons.set(GHOST_kButtonMaskRight,  (m & (1u << GHOST_kButtonMaskRight))  != 0);
  return GHOST_kSuccess;
}

GHOST_TCapabilityFlag GHOST_SystemOHOS::getCapabilities() const
{
  /* Mirror Headless's conservative default: enable everything, then
   * mask off features that make no sense on a single-surface,
   * touch-only, no-native-clipboard platform. */
  return GHOST_TCapabilityFlag(
      GHOST_CAPABILITY_FLAG_ALL &
      ~(GHOST_kCapabilityWindowPosition | GHOST_kCapabilityCursorWarp |
        GHOST_kCapabilityPrimaryClipboard | GHOST_kCapabilityDesktopSample |
        GHOST_kCapabilityClipboardImages | GHOST_kCapabilityInputIME |
        GHOST_kCapabilityWindowDecorationStyles | GHOST_kCapabilityKeyboardHyperKey));
}

/* -------------------------------------------------------------------- */
/* Clipboard: no native clipboard binding yet. */

char *GHOST_SystemOHOS::getClipboard(bool /*selection*/) const
{
  return nullptr;
}

void GHOST_SystemOHOS::putClipboard(const char * /*buffer*/, bool /*selection*/) const
{
  /* nop */
}

/* -------------------------------------------------------------------- */
/* Input queue. Producers (post*) run on the ArkTS/XComponent thread,
 * the consumer (drainInputQueue) runs on the Blender render thread from
 * processEvents(). Ordering is preserved via std::deque + mutex; cursor
 * and button snapshots use atomics so the const getters can read them
 * without acquiring the mutex. */

void GHOST_SystemOHOS::postCursorMove(int window_id, int32_t x, int32_t y)
{
  m_cursor_x.store(x, std::memory_order_relaxed);
  m_cursor_y.store(y, std::memory_order_relaxed);
  QueuedInput q{};
  q.kind = QueuedInput::CURSOR_MOVE;
  q.window_id = window_id;
  q.x = x;
  q.y = y;
  q.button = GHOST_kButtonMaskLeft;
  q.time_ms = getMilliSeconds();
  std::lock_guard<std::mutex> lk(m_input_mutex);
  /* Coalesce with the previous cursor move if it targets the same window. */
  if (!m_input_queue.empty() && m_input_queue.back().kind == QueuedInput::CURSOR_MOVE &&
      m_input_queue.back().window_id == window_id) {
    m_input_queue.back() = q;
  }
  else {
    m_input_queue.push_back(q);
  }
}

void GHOST_SystemOHOS::postButtonEvent(int window_id,
                                            bool pressed,
                                            GHOST_TButton button,
                                            int32_t x,
                                            int32_t y)
{
  m_cursor_x.store(x, std::memory_order_relaxed);
  m_cursor_y.store(y, std::memory_order_relaxed);
  const uint32_t bit = 1u << uint32_t(button);
  if (pressed) {
    m_button_mask.fetch_or(bit, std::memory_order_relaxed);
  }
  else {
    m_button_mask.fetch_and(~bit, std::memory_order_relaxed);
  }
  const uint64_t t = getMilliSeconds();
  QueuedInput mv{};
  mv.kind = QueuedInput::CURSOR_MOVE;
  mv.window_id = window_id;
  mv.x = x; mv.y = y;
  mv.button = button;
  mv.time_ms = t;
  QueuedInput bt{};
  bt.kind = pressed ? QueuedInput::BUTTON_DOWN : QueuedInput::BUTTON_UP;
  bt.window_id = window_id;
  bt.x = x; bt.y = y;
  bt.button = button;
  bt.time_ms = t;
  /* Always emit a fresh cursor-move before the button event so Blender's
   * event context is seeded with the click location. */
  std::lock_guard<std::mutex> lk(m_input_mutex);
  m_input_queue.push_back(mv);
  m_input_queue.push_back(bt);
}

void GHOST_SystemOHOS::postTouchEvent(int window_id,
                                           TouchPhase phase,
                                           int32_t touch_id,
                                           int32_t x,
                                           int32_t y)
{
  /* Primary-finger tracking: CAS ensures a stale primary (e.g. dropped UP
   * on gesture cancel or focus loss) does not silently swallow later DOWNs
   * from the same window; force-release the stale left button first. */
  switch (phase) {
    case TOUCH_DOWN: {
      int32_t expected = -1;
      if (!m_primary_touch_id.compare_exchange_strong(expected, touch_id)) {
        postButtonEvent(window_id, false, GHOST_kButtonMaskLeft, x, y);
        m_primary_touch_id.store(touch_id, std::memory_order_relaxed);
      }
      postButtonEvent(window_id, true, GHOST_kButtonMaskLeft, x, y);
      break;
    }
    case TOUCH_MOVE:
      if (m_primary_touch_id.load() == touch_id) {
        postCursorMove(window_id, x, y);
      }
      break;
    case TOUCH_UP:
    case TOUCH_CANCEL: {
      int32_t expected = touch_id;
      if (m_primary_touch_id.compare_exchange_strong(expected, -1)) {
        postButtonEvent(window_id, false, GHOST_kButtonMaskLeft, x, y);
      }
      else {
        /* UP for non-primary or already cleared: still ensure left button is up. */
        const uint32_t m = m_button_mask.load(std::memory_order_relaxed);
        if (m & (1u << uint32_t(GHOST_kButtonMaskLeft))) {
          postButtonEvent(window_id, false, GHOST_kButtonMaskLeft, x, y);
        }
      }
      break;
    }
  }
}

bool GHOST_SystemOHOS::drainInputQueue()
{
  std::deque<QueuedInput> local;
  {
    std::lock_guard<std::mutex> lk(m_input_mutex);
    if (m_input_queue.empty()) {
      return false;
    }
    local.swap(m_input_queue);
  }
  /* STAGE_H_MULTIWINDOW: route each event to its target window
   * (findGhostWindow); fall back to the active/first window when the
   * record is not (yet) bound to a GHOST_WindowOHOS. */
  GHOST_IWindow *fallback = m_windowManager ? m_windowManager->getActiveWindow() : nullptr;
  if (!fallback && m_windowManager) {
    const std::vector<GHOST_IWindow *> &all = m_windowManager->getWindows();
    if (!all.empty()) {
      fallback = all.front();
    }
  }
  if (!fallback) {
    /* No window yet: drop events rather than push to a null target. */
    return false;
  }
  bool any = false;
  for (const QueuedInput &ev : local) {
    GHOST_IWindow *win = findGhostWindow(ev.window_id);
    if (!win) {
      win = fallback;
    }
    switch (ev.kind) {
      case QueuedInput::CURSOR_MOVE:
        pushEvent(new GHOST_EventCursor(
            ev.time_ms, GHOST_kEventCursorMove, win, ev.x, ev.y, GHOST_TABLET_DATA_NONE));
        any = true;
        break;
      case QueuedInput::BUTTON_DOWN:
        pushEvent(new GHOST_EventButton(
            ev.time_ms, GHOST_kEventButtonDown, win, ev.button, GHOST_TABLET_DATA_NONE));
        any = true;
        break;
      case QueuedInput::BUTTON_UP:
        pushEvent(new GHOST_EventButton(
            ev.time_ms, GHOST_kEventButtonUp, win, ev.button, GHOST_TABLET_DATA_NONE));
        any = true;
        break;
      case QueuedInput::KEY_DOWN: {
        char utf8_buf[6] = { ev.utf8_char, 0, 0, 0, 0, 0 };
        pushEvent(new GHOST_EventKey(
            ev.time_ms, GHOST_kEventKeyDown, win, ev.key, false, utf8_buf));
        any = true;
        break;
      }
      case QueuedInput::KEY_UP: {
        char utf8_buf[6] = { 0, 0, 0, 0, 0, 0 };
        pushEvent(new GHOST_EventKey(
            ev.time_ms, GHOST_kEventKeyUp, win, ev.key, false, utf8_buf));
        any = true;
        break;
      }
      case QueuedInput::WHEEL:
        /* One GHOST wheel event per net step; loop in case of coalescing. */
        for (int32_t i = 0; i < ev.wheel_steps; ++i) {
          pushEvent(new GHOST_EventWheel(
              ev.time_ms, win, GHOST_kEventWheelAxisVertical, 1));
        }
        for (int32_t i = 0; i > ev.wheel_steps; --i) {
          pushEvent(new GHOST_EventWheel(
              ev.time_ms, win, GHOST_kEventWheelAxisVertical, -1));
        }
        any = true;
        break;
      case QueuedInput::RESIZE: {
        /* Update the globals napi/creator read for initial window size,
         * then hand the resize to the target window (destroys its
         * Vulkan swapchain) and finally emit a GHOST size event plus
         * a dirty-window mark so wm redraws. */
        extern uint32_t g_ghost_ohos_win_w;
        extern uint32_t g_ghost_ohos_win_h;
        g_ghost_ohos_win_w = ev.rw;
        g_ghost_ohos_win_h = ev.rh;
        GHOST_WindowOHOS *ohos_win = static_cast<GHOST_WindowOHOS *>(win);
        ohos_win->notifyResize(ev.rw, ev.rh);
        pushEvent(new GHOST_Event(ev.time_ms, GHOST_kEventWindowSize, win));
        addDirtyWindow(ohos_win);
        any = true;
        break;
      }
    }
  }
  return any;
}

/* -------------------------------------------------------------------- */
/* C bridge exported from libblender.so.
 *
 * Called by the ArkTS/XComponent host (napi_init.cpp) via dlsym. Keep
 * this ABI stable; the host uses fixed integer codes to avoid coupling
 * to GHOST enums.
 *
 *   kind: 0=mouse-move  1=mouse-down  2=mouse-up
 *        10=touch-down 11=touch-move 12=touch-up 13=touch-cancel
 *   button (for mouse): 0=left 1=middle 2=right
 *   button (for touch): touch id (currently only 0 is honored)
 */

static GHOST_SystemOHOS *ohos_system()
{
  GHOST_ISystem *s = GHOST_ISystem::getSystem();
  return s ? static_cast<GHOST_SystemOHOS *>(s) : nullptr;
}

static GHOST_TButton mouse_button_from_index(int b)
{
  switch (b) {
    case 1: return GHOST_kButtonMaskMiddle;
    case 2: return GHOST_kButtonMaskRight;
    default: return GHOST_kButtonMaskLeft;
  }
}


/* Coalesce with the previous queued resize: continuous drag can
 * emit dozens of size changes per second and only the final one
 * matters. */
void GHOST_SystemOHOS::postSurfaceResized(int window_id,
                                               uint32_t width,
                                               uint32_t height)
{
  QueuedInput q{};
  q.kind = QueuedInput::RESIZE;
  q.window_id = window_id;
  q.x = 0; q.y = 0;
  q.button = GHOST_kButtonMaskLeft;
  q.rw = width; q.rh = height;
  q.time_ms = getMilliSeconds();
  std::lock_guard<std::mutex> lk(m_input_mutex);
  /* Coalesce with a previous resize for the same window: only the latest
   * extent matters. */
  if (!m_input_queue.empty() && m_input_queue.back().kind == QueuedInput::RESIZE &&
      m_input_queue.back().window_id == window_id) {
    m_input_queue.back() = q;
  }
  else {
    m_input_queue.push_back(q);
  }
}

/* STAGE_H_MULTIWINDOW
 *
 * ArkTS host passes window_id (0 = main, sub-window ids from Blender's
 * allocateSubWindowId). kind values match the previous ABI:
 *   0=cursor-move 1=button-down 2=button-up
 *   10..13=touch DOWN/MOVE/UP/CANCEL
 *   20=wheel (fy carries signed step count) */
extern "C" __attribute__((visibility("default")))
void Blender_OnPointerEvent(int window_id, int kind, int button, float fx, float fy)
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  const int32_t x = int32_t(fx);
  const int32_t y = int32_t(fy);
  switch (kind) {
    case 0:  sys->postCursorMove(window_id, x, y); break;
    case 1:  sys->postButtonEvent(window_id, true,  mouse_button_from_index(button), x, y); break;
    case 2:  sys->postButtonEvent(window_id, false, mouse_button_from_index(button), x, y); break;
    case 10: sys->postTouchEvent(window_id, GHOST_SystemOHOS::TOUCH_DOWN,   button, x, y); break;
    case 11: sys->postTouchEvent(window_id, GHOST_SystemOHOS::TOUCH_MOVE,   button, x, y); break;
    case 12: sys->postTouchEvent(window_id, GHOST_SystemOHOS::TOUCH_UP,     button, x, y); break;
    case 13: sys->postTouchEvent(window_id, GHOST_SystemOHOS::TOUCH_CANCEL, button, x, y); break;
    case 20: sys->postWheelEvent(window_id, int32_t(fy)); break;
    default: break;
  }
}


/* STAGE_C_WHEEL_APPLIED (producer) */
void GHOST_SystemOHOS::postWheelEvent(int window_id, int32_t steps)
{
  if (steps == 0) {
    return;
  }
  QueuedInput q{};
  q.kind = QueuedInput::WHEEL;
  q.window_id = window_id;
  q.x = m_cursor_x.load(std::memory_order_relaxed);
  q.y = m_cursor_y.load(std::memory_order_relaxed);
  q.button = GHOST_kButtonMaskLeft;
  q.rw = 0; q.rh = 0;
  q.wheel_steps = steps;
  q.time_ms = getMilliSeconds();
  std::lock_guard<std::mutex> lk(m_input_mutex);
  if (!m_input_queue.empty() && m_input_queue.back().kind == QueuedInput::WHEEL &&
      m_input_queue.back().window_id == window_id) {
    m_input_queue.back().wheel_steps += steps;
  }
  else {
    m_input_queue.push_back(q);
  }
}

/* postKeyEvent enqueues a KEY_DOWN/UP event; drainInputQueue turns it
 * into a GHOST_EventKey on the render thread. syncModifiersFromArkUI
 * refreshes the modifier snapshot before each event so getModifierKeys
 * returns state consistent with the event being processed. */

void GHOST_SystemOHOS::postKeyEvent(int window_id,
                                         bool pressed,
                                         GHOST_TKey key,
                                         char utf8_char)
{
  QueuedInput q{};
  q.kind = pressed ? QueuedInput::KEY_DOWN : QueuedInput::KEY_UP;
  q.window_id = window_id;
  q.x = 0; q.y = 0;
  q.button = GHOST_kButtonMaskLeft;
  q.rw = 0; q.rh = 0;
  q.wheel_steps = 0;
  q.key = key;
  q.utf8_char = utf8_char;
  q.time_ms = getMilliSeconds();
  std::lock_guard<std::mutex> lk(m_input_mutex);
  m_input_queue.push_back(q);
}

void GHOST_SystemOHOS::syncModifiersFromArkUI(uint32_t arkui_mods)
{
  const bool ctrl  = (arkui_mods & (1u << 0)) != 0;
  const bool shift = (arkui_mods & (1u << 1)) != 0;
  const bool alt   = (arkui_mods & (1u << 2)) != 0;
  /* Left slot carries the state; right slot is cleared to avoid stale
   * "still down" reports when the user releases with a different hand. */
  m_ohos_modifier_keys.set(GHOST_kModifierKeyLeftControl,  ctrl);
  m_ohos_modifier_keys.set(GHOST_kModifierKeyRightControl, false);
  m_ohos_modifier_keys.set(GHOST_kModifierKeyLeftShift,    shift);
  m_ohos_modifier_keys.set(GHOST_kModifierKeyRightShift,   false);
  m_ohos_modifier_keys.set(GHOST_kModifierKeyLeftAlt,      alt);
  m_ohos_modifier_keys.set(GHOST_kModifierKeyRightAlt,     false);
}

/* STAGE_H_MULTIWINDOW: resize now carries window_id. */
extern "C" __attribute__((visibility("default")))
void Blender_OnSurfaceResize(int window_id, uint32_t width, uint32_t height)
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  sys->postSurfaceResized(window_id, width, height);
}

/* STAGE_G_KEYCODE_TABLE
 *
 * OHOS keyCode -> GHOST_TKey mapping. Codes match
 * @ohos.multimodalInput.keyCode and NDK native_xcomponent_key_event.h.
 * Ranges (letters, digits, numpad, function keys) are handled by simple
 * offset arithmetic; the switch below covers everything else. */

static GHOST_TKey ohos_keycode_to_ghost(int keyCode)
{
  if (keyCode >= 2017 && keyCode <= 2042) {
    return GHOST_TKey(GHOST_kKeyA + (keyCode - 2017));
  }
  if (keyCode >= 2000 && keyCode <= 2009) {
    return GHOST_TKey(GHOST_kKey0 + (keyCode - 2000));
  }
  if (keyCode >= 2103 && keyCode <= 2112) {
    return GHOST_TKey(GHOST_kKeyNumpad0 + (keyCode - 2103));
  }
  if (keyCode >= 2090 && keyCode <= 2101) {
    return GHOST_TKey(GHOST_kKeyF1 + (keyCode - 2090));
  }
  switch (keyCode) {
    case 2012: return GHOST_kKeyUpArrow;
    case 2013: return GHOST_kKeyDownArrow;
    case 2014: return GHOST_kKeyLeftArrow;
    case 2015: return GHOST_kKeyRightArrow;
    case 2016: return GHOST_kKeyEnter;
    case 2043: return GHOST_kKeyComma;
    case 2044: return GHOST_kKeyPeriod;
    case 2056: return GHOST_kKeyAccentGrave;
    case 2057: return GHOST_kKeyMinus;
    case 2058: return GHOST_kKeyEqual;
    case 2059: return GHOST_kKeyLeftBracket;
    case 2060: return GHOST_kKeyRightBracket;
    case 2061: return GHOST_kKeyBackslash;
    case 2062: return GHOST_kKeySemicolon;
    case 2063: return GHOST_kKeyQuote;
    case 2064: return GHOST_kKeySlash;
    case 2045: return GHOST_kKeyLeftAlt;
    case 2046: return GHOST_kKeyRightAlt;
    case 2047: return GHOST_kKeyLeftShift;
    case 2048: return GHOST_kKeyRightShift;
    case 2072: return GHOST_kKeyLeftControl;
    case 2073: return GHOST_kKeyRightControl;
    case 2076: return GHOST_kKeyLeftOS;
    case 2077: return GHOST_kKeyRightOS;
    case 2074: return GHOST_kKeyCapsLock;
    case 2102: return GHOST_kKeyNumLock;
    case 2049: return GHOST_kKeyTab;
    case 2050: return GHOST_kKeySpace;
    case 2054: return GHOST_kKeyEnter;
    case 2055: return GHOST_kKeyBackSpace;
    case 2070: return GHOST_kKeyEsc;
    case 2071: return GHOST_kKeyDelete;
    case 2081: return GHOST_kKeyHome;
    case 2082: return GHOST_kKeyEnd;
    case 2083: return GHOST_kKeyInsert;
    case 2068: return GHOST_kKeyUpPage;
    case 2069: return GHOST_kKeyDownPage;
    case 2113: return GHOST_kKeyNumpadSlash;
    case 2114: return GHOST_kKeyNumpadAsterisk;
    case 2115: return GHOST_kKeyNumpadMinus;
    case 2116: return GHOST_kKeyNumpadPlus;
    case 2117: return GHOST_kKeyNumpadPeriod;
    case 2119: return GHOST_kKeyNumpadEnter;
    default:   return GHOST_kKeyUnknown;
  }
}

/* Derive a printable ASCII character for text-input fields. Shift state
 * flips letter case and the top-row digit symbols. Non-printable keys
 * return '\0' so GHOST_EventKey.utf8_buf stays empty. */
static char ohos_key_to_utf8(GHOST_TKey k, bool shift)
{
  if (k >= GHOST_kKeyA && k <= GHOST_kKeyZ) {
    const char base = char('a' + (k - GHOST_kKeyA));
    return shift ? char(base - 'a' + 'A') : base;
  }
  if (k >= GHOST_kKey0 && k <= GHOST_kKey9) {
    static const char shifted[] = ")!@#$%^&*(";
    const int idx = k - GHOST_kKey0;
    return shift ? shifted[idx] : char('0' + idx);
  }
  if (k >= GHOST_kKeyNumpad0 && k <= GHOST_kKeyNumpad9) {
    return char('0' + (k - GHOST_kKeyNumpad0));
  }
  switch (k) {
    case GHOST_kKeySpace:        return ' ';
    case GHOST_kKeyComma:        return shift ? '<' : ',';
    case GHOST_kKeyPeriod:       return shift ? '>' : '.';
    case GHOST_kKeySlash:        return shift ? '?' : '/';
    case GHOST_kKeySemicolon:    return shift ? ':' : ';';
    case GHOST_kKeyQuote:        return shift ? '"' : '\'';
    case GHOST_kKeyLeftBracket:  return shift ? '{' : '[';
    case GHOST_kKeyRightBracket: return shift ? '}' : ']';
    case GHOST_kKeyBackslash:    return shift ? '|' : '\\';
    case GHOST_kKeyMinus:        return shift ? '_' : '-';
    case GHOST_kKeyEqual:        return shift ? '+' : '=';
    case GHOST_kKeyAccentGrave:  return shift ? '~' : '`';
    case GHOST_kKeyNumpadPeriod:   return '.';
    case GHOST_kKeyNumpadPlus:     return '+';
    case GHOST_kKeyNumpadMinus:    return '-';
    case GHOST_kKeyNumpadAsterisk: return '*';
    case GHOST_kKeyNumpadSlash:    return '/';
    default:                       return '\0';
  }
}

/* STAGE_H_MULTIWINDOW: window_id routes the event; metaState is the
 * ArkUI modifier bitmask (bit0=CTRL bit1=SHIFT bit2=ALT). */
extern "C" __attribute__((visibility("default")))
void Blender_OnKeyEvent(int window_id, int action, int keyCode, int metaState)
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  sys->syncModifiersFromArkUI(uint32_t(metaState));

  const GHOST_TKey key = ohos_keycode_to_ghost(keyCode);
  if (key == GHOST_kKeyUnknown) {
    return;
  }
  const bool pressed = (action == 1);
  const bool shift   = (uint32_t(metaState) & (1u << 1)) != 0;
  const char ch      = pressed ? ohos_key_to_utf8(key, shift) : '\0';
  sys->postKeyEvent(window_id, pressed, key, ch);
}

/* sub-window registry + hooks. */

void GHOST_SystemOHOS::setSubWindowHooks(RequestCreateFn on_create, RequestDestroyFn on_destroy)
{
  std::lock_guard<std::mutex> lk(m_reg_mutex);
  m_request_create = std::move(on_create);
  m_request_destroy = std::move(on_destroy);
}

int GHOST_SystemOHOS::allocateSubWindowId(uint32_t w, uint32_t h)
{
  std::lock_guard<std::mutex> lk(m_reg_mutex);
  const int id = m_next_window_id++;
  WindowRecord rec;
  rec.id = id;
  rec.w = w;
  rec.h = h;
  rec.surface_ready = false;
  m_records[id] = rec;
  return id;
}

GHOST_SystemOHOS::WindowRecord *GHOST_SystemOHOS::waitForSurface(int id, int timeout_ms)
{
  std::unique_lock<std::mutex> lk(m_reg_mutex);
  const bool ok = m_reg_cv.wait_for(
      lk,
      std::chrono::milliseconds(timeout_ms),
      [&]() {
        auto it = m_records.find(id);
        return it != m_records.end() && it->second.surface_ready;
      });
  if (!ok) {
    return nullptr;
  }
  auto it = m_records.find(id);
  return (it == m_records.end()) ? nullptr : &it->second;
}

void GHOST_SystemOHOS::onSubWindowSurfaceReady(int id,
                                               void *native_window,
                                               uint32_t w,
                                               uint32_t h)
{
  {
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    auto it = m_records.find(id);
    if (it == m_records.end()) {
      /* Host reported a surface for an id we did not allocate. Insert a
       * record so subsequent lookups can still find it (defensive; can
       * happen if ArkTS races the main-window setup). */
      WindowRecord rec;
      rec.id = id;
      rec.native_window = native_window;
      rec.w = w;
      rec.h = h;
      rec.surface_ready = true;
      m_records[id] = rec;
    }
    else {
      it->second.native_window = native_window;
      it->second.w = w;
      it->second.h = h;
      it->second.surface_ready = true;
    }
  }
  m_reg_cv.notify_all();
}

void GHOST_SystemOHOS::onSubWindowClosed(int id)
{
  GHOST_WindowOHOS *w = nullptr;
  {
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    auto it = m_records.find(id);
    if (it != m_records.end()) {
      w = it->second.ghost_window;
    }
  }
  if (w) {
    /* Feed a normal window-close event; wm will call disposeWindow which
     * eventually reaches GHOST_WindowOHOS::~. That dtor calls
     * onGhostWindowDestroyed to remove the record. */
    pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowClose, w));
  }
  else {
    /* No GHOST_WindowOHOS bound yet (creation timed out / raced): erase
     * the pending record so it does not leak. */
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    m_records.erase(id);
  }
}

void GHOST_SystemOHOS::onGhostWindowDestroyed(int id)
{
  bool notify_host = false;
  {
    std::lock_guard<std::mutex> lk(m_reg_mutex);
    auto it = m_records.find(id);
    if (it != m_records.end()) {
      /* Only ask the host to tear down sub-windows; main window (id=0) is
       * owned by the ArkTS ability lifecycle. */
      notify_host = (id != kMainWindowId);
      m_records.erase(it);
    }
  }
  if (notify_host && m_request_destroy) {
    m_request_destroy(id);
  }
}

GHOST_WindowOHOS *GHOST_SystemOHOS::findGhostWindow(int id) const
{
  std::lock_guard<std::mutex> lk(m_reg_mutex);
  auto it = m_records.find(id);
  return (it == m_records.end()) ? nullptr : it->second.ghost_window;
}

int GHOST_SystemOHOS::findWindowIdByNativeWindow(void *nw) const
{
  std::lock_guard<std::mutex> lk(m_reg_mutex);
  for (const auto &kv : m_records) {
    if (kv.second.native_window == nw) {
      return kv.first;
    }
  }
  return -1;
}

/* STAGE_H_MULTIWINDOW: sub-window C bridge exports.
 *
 * Blender_SetSubWindowHooks lets napi install create/destroy callbacks so
 * GHOST_SystemOHOS::createWindow can ask ArkTS to spin up a new XComponent.
 * Blender_OnSubWindowSurfaceReady is the acknowledgement from napi once the
 * new XComponent has an OHNativeWindow*. Blender_OnSubWindowClosed is fired
 * when the user closes the sub-window from its title bar. */

extern "C" __attribute__((visibility("default")))
void Blender_SetSubWindowHooks(void (*on_create)(int, int, int, const char *),
                               void (*on_destroy)(int))
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  GHOST_SystemOHOS::RequestCreateFn c;
  GHOST_SystemOHOS::RequestDestroyFn d;
  if (on_create) {
    c = [on_create](int id, int w, int h, const char *title) {
      on_create(id, w, h, title);
    };
  }
  if (on_destroy) {
    d = [on_destroy](int id) { on_destroy(id); };
  }
  sys->setSubWindowHooks(std::move(c), std::move(d));
}

extern "C" __attribute__((visibility("default")))
void Blender_OnSubWindowSurfaceReady(int window_id, void *native_window,
                                     uint32_t width, uint32_t height)
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  sys->onSubWindowSurfaceReady(window_id, native_window, width, height);
}

extern "C" __attribute__((visibility("default")))
void Blender_OnSubWindowClosed(int window_id)
{
  GHOST_SystemOHOS *sys = ohos_system();
  if (!sys) {
    return;
  }
  sys->onSubWindowClosed(window_id);
}

