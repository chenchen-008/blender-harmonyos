/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * GHOST window backend for HarmonyOS / OpenHarmony (OHOS).
 *
 * Design notes
 * ------------
 * Unlike X11/Wayland/Win32/Cocoa, on OHOS we do NOT own a top-level
 * window. The only surface we ever get is the one XComponent hands us
 * from the ArkTS side (an OHNativeWindow*). The ArkTS host forwards
 * that pointer via Blender_SetNativeWindow() before Blender_Init() runs;
 * we pick it up here through the g_ghost_ohos_native_window global.
 *
 * Because there is exactly one surface for the whole app, most of the
 * GHOST_IWindow API collapses to trivialities:
 *   - No move/order/state semantics: the OS decides those.
 *   - No screen ↔ client coord translation: they are identical.
 *   - No OS cursor: pointer visuals are the compositor's job.
 * Everything real happens inside newDrawingContext(), which wraps the
 * native window into a Vulkan surface via GHOST_ContextVK's OHOS branch.
 */

#include "GHOST_WindowOHOS.hh"

#include <cstdlib>

#include "GHOST_SystemOHOS.hh"
#include "GHOST_ContextNone.hh"
#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

/* -------------------------------------------------------------------- */
/* Cross-TU handoff from creator.cc.
 *
 * creator.cc defines these globals; the ArkTS/NAPI layer writes them
 * through Blender_SetNativeWindow() *before* Blender_Init(). We read
 * the native-window pointer here at construction time.
 *
 * The size fields live over there too but we take width/height from
 * the ctor args so callers stay in charge of geometry. */
extern "C" void *g_ghost_ohos_native_window;

/* -------------------------------------------------------------------- */
/* Construction / destruction. */

GHOST_WindowOHOS::GHOST_WindowOHOS(GHOST_SystemOHOS *system,
                                   const char *title,
                                   int32_t /*left*/,
                                   int32_t /*top*/,
                                   uint32_t width,
                                   uint32_t height,
                                   GHOST_TWindowState state,
                                   GHOST_TDrawingContextType type,
                                   const bool stereoVisual,
                                   const bool is_debug,
                                   void *native_window,
                                   int window_id)
    : GHOST_Window(width, height, state, stereoVisual, false),
      m_system(system),
      /* STAGE_H_MULTIWINDOW: the surface is supplied by the caller. For
       * the main window this is g_ghost_ohos_native_window; for sub-
       * windows it is whatever the ArkTS host reported to
       * Blender_OnSubWindowSurfaceReady. */
      m_nativeWindow(static_cast<OHNativeWindow *>(native_window)),
      m_width(width),
      m_height(height),
      m_title(title ? title : "Blender"),
      m_is_debug_context(is_debug),
      m_valid_setup(false),
      m_window_id(window_id)
{
  /* Try to build the requested drawing context (Vulkan on OHOS).
   * If it fails, m_valid_setup stays false and getValid() reports
   * false, letting the system layer discard the window cleanly. */
  if (setDrawingContextType(type) == GHOST_kSuccess) {
    m_valid_setup = true;
  }
}

GHOST_WindowOHOS::~GHOST_WindowOHOS()
{
  /* STAGE_H_MULTIWINDOW: notify system so the record is dropped and
   * (for sub-windows) the ArkTS host is asked to tear down its window. */
  if (m_system) {
    m_system->onGhostWindowDestroyed(m_window_id);
  }
  /* We don't own the OHNativeWindow — ArkTS/XComponent does. So we
   * only release GHOST-side handles (the Vulkan context, etc.) and
   * let the surface itself live on. */
  releaseNativeHandles();
}

/* -------------------------------------------------------------------- */
/* Trivial state / geometry accessors. */

bool GHOST_WindowOHOS::getValid() const
{
  /* Valid iff (a) our own setup succeeded and (b) the base class
   * agrees a drawing context is attached. */
  return m_valid_setup && GHOST_Window::getValid();
}

void GHOST_WindowOHOS::setTitle(const char *title)
{
  /* No window-manager title bar on OHOS; we keep it for reporting only. */
  m_title = title ? title : "";
}

std::string GHOST_WindowOHOS::getTitle() const
{
  return m_title;
}

void GHOST_WindowOHOS::getWindowBounds(GHOST_Rect &bounds) const
{
  /* Window bounds == client bounds: OHOS has no decorations that we
   * own, so both queries return the surface rectangle. */
  getClientBounds(bounds);
}

void GHOST_WindowOHOS::getClientBounds(GHOST_Rect &bounds) const
{
  bounds.m_l = 0;
  bounds.m_t = 0;
  bounds.m_r = int32_t(m_width);
  bounds.m_b = int32_t(m_height);
}

/* setClientWidth/Height/Size just update our cached extent. Actual
 * swapchain resize happens later via GHOST_ContextVK when it detects
 * VK_ERROR_OUT_OF_DATE_KHR — same mechanism as Wayland. */
GHOST_TSuccess GHOST_WindowOHOS::setClientWidth(uint32_t width)
{
  m_width = width;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::setClientHeight(uint32_t height)
{
  m_height = height;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::setClientSize(uint32_t width, uint32_t height)
{
  m_width = width;
  m_height = height;
  return GHOST_kSuccess;
}

/* Screen and client coordinate systems coincide on OHOS — we always
 * treat the XComponent surface as if it were the whole "screen". */
void GHOST_WindowOHOS::screenToClient(
    int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

void GHOST_WindowOHOS::clientToScreen(
    int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

/* Window state semantics don't map: OHOS decides fullscreen/minimize
 * lifecycle for us. We always advertise "Normal" and no-op writes. */
GHOST_TWindowState GHOST_WindowOHOS::getState() const
{
  return GHOST_kWindowStateNormal;
}

uint16_t GHOST_WindowOHOS::getDPIHint()
{
  /* HarmonyOS tablets/phones are high-DPI. Blender derives the UI scale from
   * this hint (U.dpi = dpi * ui_scale * 72/96), so report a 2x density to
   * match the desktop Hi-DPI look instead of the tiny 96 DPI default. */
  const char *env = getenv("DSHM_DPI");
  if (env && *env) {
    const int v = atoi(env);
    if (v >= 72 && v <= 384) {
      return uint16_t(v);
    }
  }
  return 192;
}

GHOST_TSuccess GHOST_WindowOHOS::setState(GHOST_TWindowState /*state*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::setOrder(GHOST_TWindowOrder /*order*/)
{
  /* Z-order is meaningless with a single surface — accept and ignore. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::invalidate()
{
  // STAGE_A_INVALIDATE_APPLIED
  /* OHOS: mark this window dirty so GHOST_System pushes a
   * GHOST_kEventWindowUpdate on the next processEvents() tick,
   * which drives wm_draw_update. Without this the dirty set
   * stays empty and no UI interaction triggers a redraw. */
  if (m_system) {
    m_system->addDirtyWindow(this);
  }
  return GHOST_kSuccess;
}

bool GHOST_WindowOHOS::isDialog() const
{
  /* Dialog windows don't exist in a single-surface world. */
  return false;
}

/* -------------------------------------------------------------------- */
/* Drawing context creation.
 *
 * Only Vulkan is supported on OHOS. We forward m_nativeWindow into the
 * OHOS branch of GHOST_ContextVK (added in Step 1), which will call
 * vkCreateSurfaceOHOS() to wrap it into a VkSurfaceKHR. */
GHOST_Context *GHOST_WindowOHOS::newDrawingContext(GHOST_TDrawingContextType type)
{
#ifdef WITH_VULKAN_BACKEND
  if (type == GHOST_kDrawingContextTypeVulkan) {
    /* Vulkan 1.2 target; matches the other GHOST backends.
     * A default-constructed GHOST_GPUDevice means "let the driver
     * pick" — Blender has no user-visible GPU picker on OHOS yet. */
    GHOST_Context *context = new GHOST_ContextVK(
        m_wantStereoVisual, m_nativeWindow, 1, 2, m_is_debug_context, GHOST_GPUDevice{});

    if (context->initializeDrawingContext()) {
      return context;
    }
    /* Init failed — clean up rather than leak. Caller will observe
     * a null context and mark the window invalid. */
    delete context;
    return nullptr;
  }
#endif
  /* GHOST asks for a context type we don't support (OpenGL / Metal /
   * None). On OHOS anything but Vulkan is a hard "no". */
  (void)type;
  return nullptr;
}

/* -------------------------------------------------------------------- */
/* Cursor stubs.
 *
 * OHOS phones/tablets don't have an OS-level mouse cursor to
 * manipulate. The compositor draws its own touch/stylus indicators,
 * and Blender's software cursor still works on top of the surface.
 * We therefore accept every cursor request and no-op, keeping the
 * WM's cursor-management code happy. */

GHOST_TSuccess GHOST_WindowOHOS::setWindowCursorVisibility(bool /*visible*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::setWindowCursorShape(GHOST_TStandardCursor /*shape*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::hasCursorShape(GHOST_TStandardCursor /*shape*/)
{
  /* Advertising "yes" for every shape avoids Blender falling back to
   * software cursor rendering for shapes we simply don't care about. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowOHOS::setWindowCustomCursorShape(uint8_t * /*bitmap*/,
                                                             uint8_t * /*mask*/,
                                                             int /*sizex*/,
                                                             int /*sizey*/,
                                                             int /*hotX*/,
                                                             int /*hotY*/,
                                                             bool /*canInvertColor*/)
{
  /* Custom cursors: same story. Silently accept. */
  return GHOST_kSuccess;
}

/* STAGE_D_NOTIFY_RESIZE_PASSIVE
 *
 * Just update the cached extent; do NOT touch the Vulkan swapchain
 * here. swapBuffers() already handles OUT_OF_DATE / SUBOPTIMAL by
 * calling recreateSwapchain() at safe points in its frame loop,
 * with proper fence sync. Triggering a rebuild from the input drain
 * thread races with in-flight fences and produces the intermittent
 * garbage frames / driver aborts seen during window animations. */
void GHOST_WindowOHOS::notifyResize(uint32_t width, uint32_t height)
{
  if (m_width == width && m_height == height) {
    return;
  }
  m_width = width;
  m_height = height;
}
