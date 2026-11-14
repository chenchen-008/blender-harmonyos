import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import {
  add,
  bindSubXComponent,
  extractRawfile,
  getFrameCount,
  setPublicHome,
  isBlenderReady,
  loadBlenderLibs,
  notifyDatafilesReady,
  registerSubWindowCallback,
} from 'libentry.so';
import EntryAbility from '../entryability/EntryAbility';
import { VirtualSubWin, VirtualSubWindowPanel, makeVirtualSubWin } from '../components/VirtualSubWindowPanel';

const DOMAIN = 0x0000;
const TAG = 'Blender_UI_Index';

/* Title bar height of a virtual panel (must match TITLE_H in
 * VirtualSubWindowPanel.ets); used to clamp initial placement. */
const VSUB_TITLE_H = 28;

/* A minimized virtual sub-window, shown as a restore bar on top of the
 * viewport until the user restores or Blender closes it. */
interface MinimizedBar {
  id: number;
  title: string;
}

const POLL_INTERVAL_MS = 100;
const MAX_WAIT_MS = 60000;
/* Minimum Blender_Step iterations before we accept "first frame is on
 * screen". Approximates Vulkan swapchain depth (~3 in-flight frames)
 * so we don't rip the splash away before the first present actually
 * lands on the display. */
const MIN_FRAMES_TO_HIDE_SPLASH = 3;
/* Hard timeout after isBlenderReady() first flips true. If frame count
 * never grows past MIN_FRAMES_TO_HIDE_SPLASH (e.g. render thread
 * stalls), don't leave the splash covering the app forever. */
const FIRST_FRAME_FALLBACK_MS = 15000;

const BLENDER_VERSION_DIR = '4.5';


interface ResourceEntry {
  name: string;
  path: string;
}

enum Phase {
  Extracting = 1,
  Initializing = 2,
  Done = 3,
}

@Entry
@Component
struct Index {
  @State phase: Phase = Phase.Extracting;
  @State statusText: string = '正在准备 Blender 数据文件...';
  @State detail: string = '';

  private pollTimer: number = -1;
  private pollElapsed: number = 0;
  /* pollElapsed value at which isBlenderReady() first returned true.
   * Used together with FIRST_FRAME_FALLBACK_MS to bound the wait for
   * the first Vulkan present. */
  private readySinceMs: number = 0;
  private ctrl: XComponentController = new XComponentController();

  /* STAGE_I_VIRTUAL_SUBWIN — floating panels for devices where system
   * sub-windows are unavailable. Rendered after the main XComponent in
   * the Stack below, so they sit on top of the 3D viewport. Panel moves
   * and resizes write into the @Observed VirtualSubWin fields, which
   * repaint via @ObjectLink. */
  @State subWindows: VirtualSubWin[] = [];
  /* Minimized panels, shown as restore bars on top of the viewport. */
  @State minimizedBars: MinimizedBar[] = [];
  /* Size of the stacking area (≈ main window, fullscreen) in vp, used to
   * clamp panel placement and size. */
  private vstackAreaW: number = 0;
  private vstackAreaH: number = 0;
  /* px→vp scale factor (px2vp global is deprecated). */
  private densityPixels: number = 1;

  aboutToAppear(): void {
    /* STAGE_H_SUBWIN_ROUTE
     * Route Blender's create/destroy sub-window requests to EntryAbility
     * so it can invoke parent.createSubWindowWithOptions on the UI thread. */
    try {
      registerSubWindowCallback(
        (kind: number, id: number, w: number, h: number, title: string) => {
          hilog.info(DOMAIN, TAG, `SubWinReq kind=${kind} id=${id} ${w}x${h}`);
          if (kind === 0) {
            EntryAbility.createBlenderSubWindow(id, w, h, title);
          }
          else if (kind === 1) {
            EntryAbility.destroyBlenderSubWindow(id);
          }
        }
      );
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'registerSubWindowCallback failed');
    }
    /* Install the virtual sub-window callbacks that EntryAbility uses
     * when the device is (or gets demoted to) 'virtual' mode. */
    EntryAbility.createVirtualSubWindow =
      (id: number, w: number, h: number, title: string) => {
        this.addVirtualSubWindow(id, w, h, title);
      };
    EntryAbility.destroyVirtualSubWindow =
      (id: number) => {
        this.removeVirtualSubWindow(id);
      };
    try {
      const d = display.getDefaultDisplaySync();
      this.densityPixels = d.densityPixels;
      this.vstackAreaW = d.width / d.densityPixels;
      this.vstackAreaH = d.height / d.densityPixels;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'display size unavailable, clamp disabled');
    }
    // Kick off resource extraction on the UI thread; we don't need a
    // worker because zstd of ~100MB is fast enough not to jank the
    // splash and the render thread hasn't started yet anyway.
    this.doExtractAndSignal();
  }

  private addVirtualSubWindow(id: number, w: number, h: number, title: string): void {
    /* Guard against double-add: the timeout bypass hook and the awaited
     * fallback path can both fire for the same id. */
    if (this.subWindows.findIndex((s) => s.id === id) >= 0) {
      hilog.warn(DOMAIN, TAG, `virtual sub-window already exists id=${id}, skip`);
      return;
    }
    /* Blender requests size in px; ArkUI layout is vp. */
    let wVp = w / this.densityPixels;
    let hVp = h / this.densityPixels;
    /* Cap at 90% of the stacking area so small screens (phones) keep the
     * title bar + content reachable. */
    const maxW = Math.max(280, this.vstackAreaW * 0.9);
    const maxH = Math.max(220, this.vstackAreaH * 0.9);
    wVp = Math.min(wVp, maxW);
    hVp = Math.min(hVp, maxH);
    /* Cascade like the system-window path (EntryAbility: offset per id). */
    const step = 32;
    const cascade = (id % 8) * step;
    const x = Math.min(24 + cascade, Math.max(0, this.vstackAreaW - wVp));
    const y = Math.min(24 + cascade, Math.max(0, this.vstackAreaH - hVp - VSUB_TITLE_H));
    const titleText = title && title.length > 0 ? title : `Blender ${id}`;
    this.subWindows.push(makeVirtualSubWin(id, titleText, x, y, wVp, hVp));
    hilog.info(DOMAIN, TAG,
      `virtual sub-window added id=${id} ${wVp.toFixed(0)}x${hVp.toFixed(0)} @${x.toFixed(0)},${y.toFixed(0)}`);
  }

  /* Called both from the × button (panel removed → XComponent destroyed →
   * OnSubWindowClosed drives the Blender-side close) and from Blender
   * closing the window itself (kind=1 → already gone here, no-op). */
  private removeVirtualSubWindow(id: number): void {
    const idx = this.subWindows.findIndex((s) => s.id === id);
    if (idx >= 0) {
      this.subWindows.splice(idx, 1);
      hilog.info(DOMAIN, TAG, `virtual sub-window removed id=${id}`);
    }
    /* Drop any matching minimize restore bar. */
    const bidx = this.minimizedBars.findIndex((b) => b.id === id);
    if (bidx >= 0) {
      this.minimizedBars.splice(bidx, 1);
      hilog.info(DOMAIN, TAG, `minimize bar removed id=${id}`);
    }
  }

  /* Minimize → hide panel + show restore bar. */
  private addMinimizeBar(id: number, title: string): void {
    if (this.minimizedBars.findIndex((b) => b.id === id) >= 0) return;
    this.minimizedBars.push({ id: id, title: title });
    hilog.info(DOMAIN, TAG, `minimize bar added id=${id} "${title}"`);
  }

  /* Restore bar clicked → unhide panel, drop the bar. */
  private restoreVirtualSubWindow(id: number): void {
    const win = this.subWindows.find((s) => s.id === id);
    if (win) {
      win.minimized = false;
      hilog.info(DOMAIN, TAG, `virtual sub-window restored id=${id}`);
    }
    const bidx = this.minimizedBars.findIndex((b) => b.id === id);
    if (bidx >= 0) {
      this.minimizedBars.splice(bidx, 1);
    }
  }

  aboutToDisappear(): void {
    this.stopPolling();
  }

  private async doExtractAndSignal(): Promise<void> {
    try {
      const ctx = getContext(this) as common.UIAbilityContext;
      const filesDir = ctx.filesDir;
      const rm = ctx.resourceManager;

      const publicHome = await EntryAbility.ensurePublicHomeDirs(ctx);
      const home = publicHome ?? filesDir;
      setPublicHome(home);

      const items: ResourceEntry[] = [
        { name: 'blender-data.tar.zst',  path: filesDir + '/blender' },
        { name: 'python-stdlib.tar.zst', path: filesDir + '/python_home' },
        /* Blender demo-scene downloader addon (packed by tools/stage_libs.sh).
         * Landed in the user scripts dir so the user can install/enable it
         * manually; it downloads demo scenes into <home>/demos at runtime. */
        { name: 'addons.tar.zst', path: `${home}/.config/blender/${BLENDER_VERSION_DIR}/scripts/addons` },
        { name: 'numpy-ohos-py.tar.zst', path: filesDir + '/python_home/lib/python3.11' },
        { name: 'numpy-loader.tar.zst', path: filesDir + '/blender/scripts/startup' },
      ];

      const lines: string[] = [];
      for (const it of items) {
        const line: string = extractRawfile(rm, it.name, it.path, false);
        lines.push(line);
        hilog.info(DOMAIN, TAG, 'extract: %{public}s', line);
      }
      this.detail = lines.join('\n');
      // Green-light the render thread; it's been sleeping in a
      // wait loop since XComponent surface arrived.
      notifyDatafilesReady();

      this.phase = Phase.Initializing;
      this.statusText = 'Blender 引擎启动中（约 10 秒）...';
      this.startPolling();
    } catch (e) {
      const err = e as Error;
      this.statusText = '准备失败: ' + err.message;
      hilog.error(DOMAIN, TAG, 'extract failed: %{public}s', err.message);
    }
  }


  private startPolling(): void {
    this.pollTimer = setInterval(() => {
      this.pollElapsed += POLL_INTERVAL_MS;

      /* Two-stage gate: Blender_Init finished AND the render thread has
       * ticked past a few Step() iterations. Step() drives one Vulkan
       * present per iteration, so waiting for a small frame count is a
       * cheap proxy for "first pixels visible" without instrumenting
       * GHOST_ContextVK. */
      let ready: boolean = false;
      try {
        ready = isBlenderReady();
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'isBlenderReady threw');
      }

      if (ready) {
        if (this.readySinceMs === 0) {
          this.readySinceMs = this.pollElapsed;
          hilog.info(DOMAIN, TAG, 'Blender_Init done, waiting for first frames');
        }
        let frames: number = 0;
        try {
          frames = getFrameCount();
        } catch (e) {
          hilog.error(DOMAIN, TAG, 'getFrameCount threw');
        }
        const waitedForFrame = this.pollElapsed - this.readySinceMs;
        if (frames >= MIN_FRAMES_TO_HIDE_SPLASH
            || waitedForFrame >= FIRST_FRAME_FALLBACK_MS) {
          hilog.info(DOMAIN, TAG,
            'hiding splash after %{public}dms (frames=%{public}d)',
            this.pollElapsed, frames);
          this.phase = Phase.Done;
          this.stopPolling();
          return;
        }
        /* Ready-but-not-yet-drawing: keep the same elapsed-seconds
         * counter but change the label so the user sees progress. */
        const secs = (this.pollElapsed / 1000).toFixed(1);
        this.statusText = `首帧绘制中... (${secs}s)`;
      } else {
        const secs = (this.pollElapsed / 1000).toFixed(1);
        this.statusText = `Blender 引擎启动中... (${secs}s)`;
      }

      if (this.pollElapsed >= MAX_WAIT_MS) {
        hilog.warn(DOMAIN, TAG, 'splash wait timeout, hiding anyway');
        this.phase = Phase.Done;
        this.stopPolling();
      }
    }, POLL_INTERVAL_MS);
  }

  private stopPolling(): void {
    if (this.pollTimer !== -1) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      // Render target. libraryname:'entry' triggers our NapiInit with
      // OH_NATIVE_XCOMPONENT_OBJ present so we can register surface
      // callbacks from C++.
      XComponent({
        id: 'blender_canvas',
        type: XComponentType.SURFACE,
        libraryname: 'entry',
        controller: this.ctrl,
      })
        .width('100%')
        .height('100%')
        .onDestroy(() => { hilog.info(DOMAIN, TAG, 'XComponent onDestroy'); })
        .focusable(true).focusOnTouch(true)

      /* Floating sub-windows on top of the viewport. Each panel's XComponent 
       * routes input/surfaces through the same napi bridge as a real sub-window. */
      ForEach(this.subWindows, (item: VirtualSubWin) => {
        VirtualSubWindowPanel({
          win: item,
          onClose: (id: number) => this.removeVirtualSubWindow(id),
          onMinimize: (id: number, title: string) => this.addMinimizeBar(id, title),
          areaW: this.vstackAreaW,
          areaH: this.vstackAreaH,
        })
      }, (item: VirtualSubWin) => `${item.id}`)

      /* restore bars for minimized panels, shown on top of the viewport; 
       * click ↩ to unhide the panel. */
      Row({ space: 8 }) {
        ForEach(this.minimizedBars, (bar: MinimizedBar) => {
          Row() {
            Text(bar.title)
              .fontSize(12).fontColor('#FFFFFF')
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .constraintSize({ maxWidth: 160 })
            Text('↩')
              .fontSize(14).fontColor('#FFFFFF')
              .margin({ left: 8, right: 4 })
              .onClick(() => {
                hilog.info(DOMAIN, TAG, `restore bar click id=${bar.id}`);
                this.restoreVirtualSubWindow(bar.id);
              })
          }
          .height(28)
          .padding({ left: 10, right: 6 })
          .backgroundColor('#3C3C3C')
          .border({ width: 1, color: '#5A5A5A', radius: 4 })
        }, (bar: MinimizedBar) => `${bar.id}`)
      }
      .position({ x: 10, y: 10 })
      .zIndex(100)

      if (this.phase !== Phase.Done) {
        Column({ space: 16 }) {
          LoadingProgress()
            .width(48).height(48)
            .color('#E8E8E8')
          Text(this.statusText)
            .fontSize(14).fontColor('#CCCCCC')
            .textAlign(TextAlign.Center)
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
        .backgroundColor('#1A1A1A')
      }
    }
    .width('100%').height('100%')
  }
}