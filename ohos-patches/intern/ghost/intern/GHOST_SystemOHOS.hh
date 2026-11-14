/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * GHOST system backend for HarmonyOS / OpenHarmony (OHOS).
 *
 * Rationale
 * ---------
 * OHOS is a single-surface, no-desktop platform: the ArkTS host owns
 * the event loop and hands us one XComponent OHNativeWindow* to draw
 * into. There is no native event queue for us to pump; input events
 * arrive from the UI thread via NAPI callbacks and are forwarded into
 * GHOST's queue by external glue code (added in Step 5).
 *
 * This system class therefore acts mostly as a factory:
 *   - createWindow() wraps the current XComponent surface into a
 *     GHOST_WindowOHOS.
 *   - createOffscreenContext() delegates to GHOST_ContextVK's OHOS
 *     branch with a null native window.
 *   - processEvents() is a no-op stub for now; Step 5 will drive the
 *     dirty-window / input-queue plumbing here.
 */

#pragma once

#include "GHOST_System.hh"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class GHOST_WindowOHOS;

class GHOST_SystemOHOS : public GHOST_System {
 public:
  GHOST_SystemOHOS();
  ~GHOST_SystemOHOS() override = default;

  /* --- Lifecycle --- */
  GHOST_TSuccess init() override;

  /* --- Windows / contexts --- */
  GHOST_IWindow *createWindow(const char *title,
                              int32_t left,
                              int32_t top,
                              uint32_t width,
                              uint32_t height,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpuSettings,
                              const bool exclusive,
                              const bool is_dialog,
                              const GHOST_IWindow *parentWindow) override;
  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpuSettings) override;
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override;
  GHOST_IWindow *getWindowUnderCursor(int32_t x, int32_t y) override;

  /* --- Event pump (Step 4: no-op; Step 5: real drain) --- */
  bool processEvents(bool waitForEvent) override;

  /* Mark a window for redraw on the next processEvents() tick. Called
   * from GHOST_WindowOHOS::invalidate() and from event handlers that
   * need to trigger a repaint. */
  void addDirtyWindow(GHOST_WindowOHOS *window);

  /* Touch phase for postTouchEvent; mirrors the OHOS XComponent
   * touch lifecycle (DOWN -> MOVE... -> UP or CANCEL). */
  enum TouchPhase { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP, TOUCH_CANCEL };

  /* STAGE_H_MULTIWINDOW
   *
   * Multi-window model:
   *   - Main window has fixed id kMainWindowId (0).
   *   - Sub-windows get monotonically increasing ids from allocateSubWindowId().
   *   - createWindow() runs on Blender's main thread; for sub-windows it
   *     asks the ArkTS host (via m_request_create) to create the OS window
   *     and blocks on m_reg_cv until the XComponent surface is ready and
   *     napi calls onSubWindowSurfaceReady(). */
  static constexpr int kMainWindowId = 0;

  /* Host hooks installed by napi via Blender_SetSubWindowHooks. */
  using RequestCreateFn = std::function<void(int, int, int, const char *)>;
  using RequestDestroyFn = std::function<void(int)>;
  void setSubWindowHooks(RequestCreateFn on_create, RequestDestroyFn on_destroy);

  /* Sub-window lifecycle (called by C ABI Blender_OnSubWindow*). */
  void onSubWindowSurfaceReady(int window_id, void *native_window,
                               uint32_t width, uint32_t height);
  void onSubWindowClosed(int window_id);

  /* Called from GHOST_WindowOHOS dtor. Removes the record and, for
   * sub-windows, tells ArkTS to tear down its window. */
  void onGhostWindowDestroyed(int window_id);

  /* Reverse lookup used by drainInputQueue when dispatching events. */
  GHOST_WindowOHOS *findGhostWindow(int window_id) const;
  int findWindowIdByNativeWindow(void *native_window) const;

  /* Input producers. Called on the ArkTS/XComponent thread via the
   * C bridge in napi_init.cpp. Enqueue events for drainInputQueue()
   * to translate into GHOST events on the render thread. */

  /* Enqueue a cursor move and refresh the cached cursor position. */
  void postCursorMove(int window_id, int32_t x, int32_t y);

  /* Enqueue a mouse button transition preceded by a cursor move so
   * Blender always sees the correct location for the click. */
  void postButtonEvent(int window_id, bool pressed, GHOST_TButton button,
                            int32_t x, int32_t y);

  /* Translate an OHOS touch phase into cursor/button events. */
  void postTouchEvent(int window_id, TouchPhase phase, int32_t touch_id,
                           int32_t x, int32_t y);

  /* Enqueue a surface resize from the ArkTS UI thread. */
  void postSurfaceResized(int window_id, uint32_t width, uint32_t height);

  /* Enqueue a mouse-wheel step. */
  void postWheelEvent(int window_id, int32_t steps);

  /* STAGE_G_KEY_APPLIED
   *
   * Enqueue a key event from the ArkTS host. */
  void postKeyEvent(int window_id, bool pressed, GHOST_TKey key,
                          char utf8_char);

  /* Update the tracked modifier snapshot from an ArkUI modifier
   * bitmask (bit0=CTRL, bit1=SHIFT, bit2=ALT). Called by the C
   * bridge before each key event so getModifierKeys() reflects
   * the state at the moment of the event. */
  void syncModifiersFromArkUI(uint32_t arkui_mods);

  /* --- Time / displays --- */
  uint64_t getMilliSeconds() const override;
  uint8_t getNumDisplays() const override;
  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  /* --- Input state (stubs; Step 5 wires this up) --- */
  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;
  GHOST_TSuccess setCursorPosition(int32_t x, int32_t y) override;
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;
  GHOST_TCapabilityFlag getCapabilities() const override;

  /* --- Clipboard (stubs) --- */
  char *getClipboard(bool selection) const override;
  void putClipboard(const char *buffer, bool selection) const override;

  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }

 private:
  /* STAGE_A_DIRTY_WINDOWS_APPLIED */

  /* Convert the accumulated dirty-window set into
   * GHOST_kEventWindowUpdate events; returns true iff any was pushed. */
  bool generateWindowExposeEvents();

  /* Windows that requested a redraw since the last processEvents() tick. */
  std::vector<GHOST_WindowOHOS *> m_dirty_windows;

  /* STAGE_B_INPUT_QUEUE_APPLIED */

  /* One queued input awaiting translation into a GHOST event. */
  struct QueuedInput {
    enum Kind { CURSOR_MOVE, BUTTON_DOWN, BUTTON_UP, RESIZE, WHEEL, KEY_DOWN, KEY_UP };
    Kind kind;
    int window_id; /* STAGE_H_MULTIWINDOW: route event to this window. */
    int32_t x, y;
    GHOST_TButton button;
    uint32_t rw, rh; /* Used by RESIZE only. */
    int32_t wheel_steps; /* Used by WHEEL only. */
    GHOST_TKey key; /* Used by KEY_DOWN/KEY_UP only. */
    char utf8_char; /* Used by KEY_DOWN only. */
    uint64_t time_ms;
  };

  /* Drain queued inputs and push corresponding GHOST events to the
   * active window. Returns true iff at least one event was pushed. */
  bool drainInputQueue();

  std::mutex m_input_mutex;
  std::deque<QueuedInput> m_input_queue;

  /* Cached input state, updated from post*() and read by the const
   * getters. Atomic so cross-thread reads don't need the mutex. */
  std::atomic<int32_t> m_cursor_x{0};
  std::atomic<int32_t> m_cursor_y{0};
  std::atomic<uint32_t> m_button_mask{0};

  /* Local modifier snapshot; updated by syncModifiersFromArkUI()
   * and returned from getModifierKeys(). We keep our own instead
   * of touching base-class protected state. */
  GHOST_ModifierKeys m_ohos_modifier_keys;

  /* STAGE_H_MULTIWINDOW: window registry. Populated for main window in
   * createWindow (using the ArkTS-provided native window) and for
   * sub-windows via onSubWindowSurfaceReady. */
  struct WindowRecord {
    int id = -1;
    void *native_window = nullptr;
    GHOST_WindowOHOS *ghost_window = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    bool surface_ready = false;
  };

  /* Reserve an id for a sub-window before asking the host to create it. */
  int allocateSubWindowId(uint32_t width, uint32_t height);
  /* Block Blender's main thread until napi reports the surface ready. */
  WindowRecord *waitForSurface(int window_id, int timeout_ms);

  mutable std::mutex m_reg_mutex;
  std::condition_variable m_reg_cv;
  std::unordered_map<int, WindowRecord> m_records;
  int m_next_window_id = 100; /* Sub-window ids start well above kMainWindowId. */

  RequestCreateFn m_request_create;
  RequestDestroyFn m_request_destroy;

  /* Primary-finger tracking for single-touch coalescing. Set to touch_id
   * on TOUCH_DOWN, cleared on matching TOUCH_UP/CANCEL. */
  std::atomic<int32_t> m_primary_touch_id{-1};
};