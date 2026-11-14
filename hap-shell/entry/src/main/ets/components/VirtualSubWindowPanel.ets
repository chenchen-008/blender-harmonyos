import { hilog } from '@kit.PerformanceAnalysisKit';
import { bindSubXComponent } from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'Blender_UI_VSubWin';

/* A single floating (virtual) sub-window drawn inside the main window's
 * Stack instead of a real OS sub-window. Used on phones and on tablets
 * without free-form multi-window where createSubWindowWithOptions is
 * rejected by WMS ("device not support" → promise hangs / throws).
 *
 * The panel is a title bar + a plain XComponent whose id follows the
 * "blender_sub_<N>" convention, so the existing napi bridge
 * (bindSubXComponent / g_xcomp_cb in napi_init.cpp) routes input and
 * surfaces exactly like a real sub-window. Blender/GHOST need zero
 * changes: createWindow just waits for OnSubWindowSurfaceReady.
 *
 * win.w / win.h are the *XComponent* (content) size in vp, matching the
 * pixel size Blender requested, so the Vulkan swapchain aspect stays
 * correct. The panel's total height is win.h + TITLE_H.
 *
 * Drag/resize use raw onTouch (PanGesture's onActionUpdate never
 * delivered updates on the test device). Position/size changes write
 * straight into the @Observed win — @ObjectLink repaints the panel
 * without going through ForEach diffing (ForEach reuses the component
 * when the key is unchanged and would NOT refresh item.x/y).
 */

/* Whole-object reactivity: gesture handlers mutate fields in place;
 * the parent passes this instance through @ObjectLink so every write
 * repaints the panel. */
@Observed
export class VirtualSubWin {
  id: number = 0;
  title: string = '';
  x: number = 0;
  y: number = 0;
  w: number = 0;
  h: number = 0;
  ctrl: XComponentController = new XComponentController();
  /* Window-state flags + maximized restore anchor. */
  maximized: boolean = false;
  minimized: boolean = false;
  saveX: number = 0;
  saveY: number = 0;
  saveW: number = 0;
  saveH: number = 0;
}

const TITLE_H = 28;        // vp
const MIN_W = 280;         // vp
const MIN_H = 220;         // vp
const HANDLE = 28;         // vp (resize handle)

@Component
export struct VirtualSubWindowPanel {
  @ObjectLink win: VirtualSubWin;
  /** Called when the user hits the title-bar × button. */
  onClose: (id: number) => void = () => {};
  /** Called when the user minimizes; Index shows a restore bar. */
  onMinimize: (id: number, title: string) => void = () => {};
  /** Parent stacking area size (vp); used to clamp drag/resize. */
  areaW: number = 0;
  areaH: number = 0;

  /* Touch baselines: TouchEvent coordinates are relative to this
   * component, so we store the anchor at Down and apply deltas. */
  private dragBaseX: number = 0;
  private dragBaseY: number = 0;
  private dragOffX: number = 0;
  private dragOffY: number = 0;
  private dragging: boolean = false;
  private resBaseW: number = 0;
  private resBaseH: number = 0;
  private resOffX: number = 0;
  private resOffY: number = 0;
  private resizing: boolean = false;

  aboutToAppear(): void {
    hilog.info(DOMAIN, TAG, `panel appear id=${this.win.id} ${this.win.w}x${this.win.h}`);
  }

  aboutToDisappear(): void {
    hilog.info(DOMAIN, TAG, `panel disappear id=${this.win.id}`);
  }

  private panelHeight(): number {
    return this.win.h + TITLE_H;
  }

  private clampX(v: number): number {
    return Math.min(Math.max(0, v), Math.max(0, this.areaW - this.win.w));
  }

  private clampY(v: number): number {
    return Math.min(Math.max(0, v), Math.max(0, this.areaH - this.panelHeight()));
  }

  /* Toggle maximized: save/restore geometry, fill the stacking area.
   * The XComponent resizes with the panel → OnSurfaceChanged →
   * napi OnSurfaceResize → GHOST rebuilds the swapchain, so Blender's
   * content goes fullscreen automatically. */
  private toggleMaximize(): void {
    const win = this.win;
    if (win.maximized) {
      win.x = win.saveX;
      win.y = win.saveY;
      win.w = win.saveW;
      win.h = win.saveH;
      win.maximized = false;
      hilog.info(DOMAIN, TAG, `vsub unmaximize id=${win.id} ${win.w}x${win.h}`);
    } else {
      win.saveX = win.x;
      win.saveY = win.y;
      win.saveW = win.w;
      win.saveH = win.h;
      win.x = 0;
      win.y = 0;
      win.w = this.areaW;
      win.h = Math.max(MIN_H, this.areaH - TITLE_H);
      win.maximized = true;
      hilog.info(DOMAIN, TAG, `vsub maximize id=${win.id} ${win.w}x${win.h}`);
    }
  }

  /* Minimize: hide the panel (XComponent surface stays alive, so Blender
   * keeps the window open) and ask Index to show a restore bar. */
  private minimize(): void {
    this.win.minimized = true;
    hilog.info(DOMAIN, TAG, `vsub minimize id=${this.win.id}`);
    this.onMinimize(this.win.id, this.win.title);
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column() {
        /* ── Title bar: drag + minimize/maximize/close ── */
        Row() {
          Text(this.win.title || 'Blender')
            .fontSize(14).fontColor('#FFFFFF')
            .layoutWeight(1).margin({ left: 12 })
          Text('—')
            .fontSize(20).fontColor('#FFFFFF')
            .width(36).height('100%')
            .textAlign(TextAlign.Center)
            .onClick(() => { this.minimize(); })
          Text('□')
            .fontSize(16).fontColor('#FFFFFF')
            .width(36).height('100%')
            .textAlign(TextAlign.Center)
            .backgroundColor(this.win.maximized ? '#5A5A5A' : '#00000000')
            .onClick(() => { this.toggleMaximize(); })
          Text('×')
            .fontSize(20).fontColor('#FFFFFF')
            .width(36).height('100%')
            .textAlign(TextAlign.Center)
            .onClick(() => {
              hilog.info(DOMAIN, TAG, `vsub × id=${this.win.id}`);
              /* Remove in UI first; XComponent destroy then fires
               * OnSubWindowClosed → Blender-side window close. */
              this.onClose(this.win.id);
            })
        }
        .width('100%').height(TITLE_H)
        .backgroundColor('#2E2E2E')
        /* Drag on the title bar only, so it never conflicts with the
         * XComponent's own input. Raw onTouch is used because PanGesture's
         * onActionUpdate is never delivered on the target device. Once the
         * touch sequence is bound to this component, Move events keep
         * arriving even when the finger leaves the title bar. */
        .onTouch((e: TouchEvent) => {
          if (e.type === TouchType.Down) {
            this.dragging = true;
            this.dragBaseX = this.win.x;
            this.dragBaseY = this.win.y;
            this.dragOffX = e.touches[0].x;
            this.dragOffY = e.touches[0].y;
          } else if (e.type === TouchType.Move) {
            if (!this.dragging || this.win.maximized || e.touches.length < 1) return;
            const dx = e.touches[0].x - this.dragOffX;
            const dy = e.touches[0].y - this.dragOffY;
            /* @Observed field write → @ObjectLink repaint. */
            this.win.x = this.clampX(this.dragBaseX + dx);
            this.win.y = this.clampY(this.dragBaseY + dy);
          } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
            this.dragging = false;
          }
        })

        /* ── XComponent body (renders Blender) ── */
        XComponent({
          id: `blender_sub_${this.win.id}`,
          type: XComponentType.SURFACE,
          libraryname: 'entry',
          controller: this.win.ctrl,
        })
          .layoutWeight(1)
          .width('100%')
          .focusable(true)
          .focusOnTouch(true)
          .onLoad((ctx?: object) => {
            hilog.info(DOMAIN, TAG, `vsub XComponent onLoad id=${this.win.id}`);
            if (ctx) {
              try {
                bindSubXComponent(ctx);
              } catch (e) {
                hilog.error(DOMAIN, TAG,
                  `bindSubXComponent failed: ${(e as Error).message}`);
              }
            }
          })
      }
      .width('100%').height('100%')

      /* ── Bottom-right resize handle (onTouch; hidden while maximized) ── */
      Row()
        .width(HANDLE).height(HANDLE)
        .backgroundColor('#22000000')
        .visibility(this.win.maximized ? Visibility.Hidden : Visibility.Visible)
        .onTouch((e: TouchEvent) => {
          if (e.type === TouchType.Down) {
            this.resizing = true;
            this.resBaseW = this.win.w;
            this.resBaseH = this.win.h;
            this.resOffX = e.touches[0].x;
            this.resOffY = e.touches[0].y;
          } else if (e.type === TouchType.Move) {
            if (!this.resizing || this.win.maximized || e.touches.length < 1) return;
            const dx = e.touches[0].x - this.resOffX;
            const dy = e.touches[0].y - this.resOffY;
            let w = Math.max(MIN_W, this.resBaseW + dx);
            let h = Math.max(MIN_H, this.resBaseH + dy);
            w = Math.min(w, this.areaW);
            h = Math.min(h, Math.max(MIN_H, this.areaH - TITLE_H));
            /* XComponent resizes with the panel; its surface change fires
             * OnSurfaceChanged → napi OnSurfaceResize → GHOST rebuilds the
             * swapchain. @Observed writes repaint. */
            this.win.w = w;
            this.win.h = h;
            /* Keep the right/bottom edges inside the stacking area. */
            if (this.win.x + w > this.areaW) {
              this.win.x = Math.max(0, this.areaW - w);
            }
            if (this.win.y + h + TITLE_H > this.areaH) {
              this.win.y = Math.max(0, this.areaH - h - TITLE_H);
            }
          } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
            this.resizing = false;
          }
        })
    }
    .width(this.win.w)
    .height(this.panelHeight())
    .position({ x: this.win.x, y: this.win.y })   /* driven by @ObjectLink */
    .visibility(this.win.minimized ? Visibility.Hidden : Visibility.Visible)
    .backgroundColor('#FFFFFF')
    .border({ width: 1, color: '#888888', radius: 6 })
    .shadow({ radius: 20, color: '#66000000', offsetX: 0, offsetY: 8 })
    .clip(true)
  }
}

/* Convenience constructor used by Index.addVirtualSubWindow. */
export function makeVirtualSubWin(id: number, title: string, x: number, y: number,
                                  w: number, h: number): VirtualSubWin {
  const v = new VirtualSubWin();
  v.id = id;
  v.title = title;
  v.x = x;
  v.y = y;
  v.w = w;
  v.h = h;
  return v;
}
