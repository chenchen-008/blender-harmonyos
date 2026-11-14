/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_WindowOHOS class.
 */

#pragma once

#include "GHOST_Window.hh"

/* Forward-decls to avoid pulling OHOS/GHOST headers here. */
struct NativeWindow;
typedef struct NativeWindow OHNativeWindow;
class GHOST_SystemOHOS;

/**
 * OHOS (HarmonyOS) window: a thin wrapper over the XComponent surface
 * supplied by the ArkTS host through Blender_SetNativeWindow().
 */
class GHOST_WindowOHOS : public GHOST_Window {
 public:
  GHOST_WindowOHOS(GHOST_SystemOHOS *system,
                   const char *title,
                   int32_t left,
                   int32_t top,
                   uint32_t width,
                   uint32_t height,
                   GHOST_TWindowState state,
                   GHOST_TDrawingContextType type,
                   const bool stereoVisual,
                   const bool is_debug,
                   void *native_window,
                   int window_id);

  ~GHOST_WindowOHOS() override;

  /* GHOST_IWindow interface. */
  bool getValid() const override;
  void setTitle(const char *title) override;
  std::string getTitle() const override;
  void getWindowBounds(GHOST_Rect &bounds) const override;
  void getClientBounds(GHOST_Rect &bounds) const override;
  GHOST_TSuccess setClientWidth(uint32_t width) override;
  GHOST_TSuccess setClientHeight(uint32_t height) override;
  GHOST_TSuccess setClientSize(uint32_t width, uint32_t height) override;

  /* STAGE_D_NOTIFY_RESIZE
   *
   * Called from GHOST_SystemOHOS::drainInputQueue() when the ArkTS
   * host reports a surface size change. Updates the cached extent
   * and tears down the Vulkan swapchain so the next swapBuffers()
   * rebuilds one matching the new surface. */
  void notifyResize(uint32_t width, uint32_t height);
  void screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;
  void clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;
  GHOST_TWindowState getState() const override;
  GHOST_TSuccess setState(GHOST_TWindowState state) override;
  uint16_t getDPIHint() override;
  GHOST_TSuccess setOrder(GHOST_TWindowOrder order) override;
  GHOST_TSuccess invalidate() override;
  bool isDialog() const override;

  /* OHOS-specific accessors. */
  OHNativeWindow *getNativeWindow() const
  {
    return m_nativeWindow;
  }
  int getWindowId() const
  {
    return m_window_id;
  }

 protected:
  /* Vulkan-only drawing context for OHOS. */
  GHOST_Context *newDrawingContext(GHOST_TDrawingContextType type) override;

  /* Cursor stubs: OHOS has no OS cursor to manipulate. */
  GHOST_TSuccess setWindowCursorVisibility(bool visible) override;
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess setWindowCustomCursorShape(uint8_t *bitmap,
                                            uint8_t *mask,
                                            int sizex,
                                            int sizey,
                                            int hotX,
                                            int hotY,
                                            bool canInvertColor) override;

 private:
  GHOST_WindowOHOS(const GHOST_WindowOHOS &) = delete;
  GHOST_WindowOHOS &operator=(const GHOST_WindowOHOS &) = delete;

  GHOST_SystemOHOS *m_system;
  OHNativeWindow *m_nativeWindow;
  uint32_t m_width;
  uint32_t m_height;
  std::string m_title;
  bool m_is_debug_context;
  bool m_valid_setup;
  int m_window_id;
};