import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';

const DOMAIN = 0x0000;
const TAG = 'Blender_Abl_Entry';

/* Sub-window mode, decided when the first sub-window is created:
 *   'system'  → real OS sub-windows via createSubWindowWithOptions
 *               (2in1 devices and free-form multi-window tablets).
 *   'virtual' → ArkTS-drawn floating panels inside the main window
 *               (phones and tablets without free-form multi-window,
 *               where the OS API returns undefined or hangs). */
export type SubWinMode = 'system' | 'virtual';

/* Subdirectories created under the public HOME returned by the DOWNLOAD
 * picker (Download/<actual bundle name>). The path is passed to native
 * Blender through setPublicHome(), so no bundle name is hardcoded here. */
const PUBLIC_HOME_SUBDIRS: string[] = [
  'autosave'
];

/* STAGE_H_MULTIWINDOW
 *
 * Main-window bootstrap plus a small registry of Blender-owned sub-windows.
 * The native side (napi_init.cpp) asks the ArkTS layer through a TSFN when
 * Blender's wm decides to open or close an auxiliary window (Preferences,
 * Render Result, driver editor, ...). We service those requests here with
 * parent.createSubWindowWithOptions + SubWindowPage. */
export default class EntryAbility extends UIAbility {
  public static mainWindow: window.Window | undefined = undefined;
  private static subWindows: Map<number, window.Window> = new Map();
  private static destroying: Set<number> = new Set();

  /* Sub-window mode: 'system' is the default; the first create failure
   * (missing API, hung promise, setUIContent error) demotes the device
   * to 'virtual' for the rest of the session. */
  static subWinMode: SubWinMode = 'system';

  /* Virtual sub-window callbacks, installed by Index at startup. */
  static createVirtualSubWindow: ((id: number, w: number, h: number, title: string) => void) | null = null;
  static destroyVirtualSubWindow: ((id: number) => void) | null = null;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'setColorMode failed: %{public}s', JSON.stringify(err));
    }
  }

  /* HarmonyOS trivia: Download/<bundleName> is NOT direct-writable via
   * POSIX mkdir (13900001 EPERM) even with READ_WRITE_USER_FILE. The
   * system only materializes it when the app first goes through the
   * DocumentPickerMode.DOWNLOAD API. That mode is intentionally silent
   * (no user prompt) and returns a persisted URI covering the app's
   * bundle-named subtree; from then on plain POSIX works for any path
   * inside that subtree.
   *
   * Must be called from a component context that has a live UIContext
   * (Index.aboutToAppear is fine, EntryAbility.onCreate is NOT — the
   * picker's UIExtension host is not ready yet there → 13900042).
   *
   * Returns the resolved POSIX path on success, or null on failure. */
  public static async ensurePublicHomeDirs(
    ctx: common.UIAbilityContext): Promise<string | null> {
    try {
      const dvp = new picker.DocumentViewPicker(ctx);
      const opts = new picker.DocumentSaveOptions();
      opts.pickerMode = picker.DocumentPickerMode.DOWNLOAD;
      const result = await dvp.save(opts);
      if (!result || result.length === 0) {
        hilog.warn(DOMAIN, TAG, 'DOWNLOAD picker returned empty');
        return null;
      }
      const uri = result[0];
      const path = new fileUri.FileUri(uri).path;
      hilog.info(DOMAIN, TAG,
        `DOWNLOAD picker granted uri=${uri} path=${path}`);
      /* Bundle root is now materialized — mkdir children via POSIX. */
      for (const sub of PUBLIC_HOME_SUBDIRS) {
        const p = `${path}/${sub}`;
        try {
          fs.mkdirSync(p, true);
        } catch (e) {
          const err = e as BusinessError;
          hilog.warn(DOMAIN, TAG,
            `mkdir ${sub} failed: (${err.code}) ${err.message}`);
        }
      }
      return path;
    } catch (e) {
      const err = e as BusinessError;
      hilog.warn(DOMAIN, TAG,
        `DOWNLOAD picker failed (${err.code}): ${err.message}`);
      return null;
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    try {
      const mainWin = windowStage.getMainWindowSync();
      EntryAbility.mainWindow = mainWin;
      /* Main window as immersive full-screen so system bars don't
       * cover the XComponent's touch area. */
      mainWin.setWindowLayoutFullScreen(true).catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, `setWindowLayoutFullScreen failed: ${e.message}`);
      });
      try {
        mainWin.setWindowSystemBarProperties({
          statusBarColor: '#00000000',
          navigationBarColor: '#00000000',
        }).catch((e: BusinessError) => {
          hilog.warn(DOMAIN, TAG, `setWindowSystemBarProperties failed: ${e.message}`);
        });
        mainWin.setWindowSystemBarEnable([]).catch((e: BusinessError) => {
          hilog.warn(DOMAIN, TAG, `setWindowSystemBarEnable failed: ${e.message}`);
        });
      } catch (e) { /* older SDKs */ }
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'getMainWindowSync failed');
    }
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      /* Reference impl compresses the title bar to its minimum height
       * after content is mounted; only relevant in decorated free-form. */
      try {
        EntryAbility.mainWindow?.setWindowDecorHeight(37);
      } catch (e) { /* ignore */ }
    });
  }

  onWindowStageDestroy(): void {
  }

  /* Unified entry point called from Index's TSFN callback.
   * - 'virtual' mode: create the floating panel directly.
   * - 'system' mode: try a real OS sub-window; demote to 'virtual' on
   *   any failure (missing API / hung promise / setUIContent error) and
   *   create the floating panel for this id instead. */
  public static createBlenderSubWindow(id: number, w: number, h: number, title: string): Promise<boolean> {
    hilog.info(DOMAIN, TAG, `createBlenderSubWindow id=${id} ${w}x${h}`);

    /* Virtual mode: create the floating panel directly. */
    if (EntryAbility.subWinMode === 'virtual') {
      if (EntryAbility.createVirtualSubWindow) {
        EntryAbility.createVirtualSubWindow(id, w, h, title);
        return Promise.resolve(true);
      }
      hilog.error(DOMAIN, TAG, 'virtual mode but createVirtualSubWindow is not installed');
      return Promise.resolve(false);
    }

    const result = EntryAbility.createSystemSubWindow(id, w, h, title);
    if (result !== 0) {
      if (result === 2 && EntryAbility.createVirtualSubWindow) {
        hilog.info(DOMAIN, TAG, 'demoted to virtual (system sub-window unsupported)');
        EntryAbility.createVirtualSubWindow(id, w, h, title);
        EntryAbility.subWinMode = 'virtual';
        return Promise.resolve(true);
      }
      return Promise.resolve(false);
    }
    
    return Promise.resolve(true);
  }

  /* Create a real OS sub-window (PC / free-form multi-window devices).
   * Returns synchronously:
   *   0 → create call accepted (async outcome handled inside);
   *   1 → main window not ready;
   *   2 → createSubWindowWithOptions returned undefined (unsupported). */
  public static createSystemSubWindow(id: number, w: number, h: number, title: string): number {
    const parent = EntryAbility.mainWindow;
    if (!parent) {
      hilog.error(DOMAIN, TAG, 'main window not ready');
      return 1;
    }
    /* SubWindowPage reads the id from AppStorage; we can't pass args
     * directly through setUIContent. */
    AppStorage.setOrCreate('pendingSubWinId', id);

    const options: window.SubWindowOptions = {
      title: title && title.length > 0 ? title : `Blender ${id}`,
      decorEnabled: true,
      isModal: false,
      maximizeSupported: true,
    };

    const subwinPromise = parent.createSubWindowWithOptions(`blender_sub_${id}`, options);
    if (!subwinPromise) {
      /* WMS rejects the call synchronously (returns undefined) on
       * devices without multi-window support. */
      hilog.info(DOMAIN, TAG, 'subWinPromise is undefined, possibly because device not supported');
      return 2;
    } else {
      subwinPromise.then((subWin: window.Window) => {
        EntryAbility.subWindows.set(id, subWin);

        subWin.setUIContent('pages/SubWindowPage', (err: BusinessError) => {
          if (err?.code) {
            hilog.error(DOMAIN, TAG, `setUIContent err ${JSON.stringify(err)}`);
            EntryAbility.subWindows.delete(id);
            subWin.destroyWindow().catch(() => { /* ignore */ });
            return 3;
          }
          try { subWin.resize(w, h); } catch (e) { /* ignore */ }
          /* Shrink the system title bar to its minimum height (37vp).
           * Only takes effect on 2in1 free-form windows; other devices
           * return 801 which we swallow. */
          try { subWin.setWindowDecorHeight(37); } catch (e) { /* ignore */ }
          /* Position the sub-window relative to the main window so it
           * lands inside the visible area. Cascade a small offset per
           * id to reduce overlap when multiple sub-windows spawn. */
          try {
            const parentRect = parent.getWindowProperties().windowRect;
            const step = 32;
            const cascade = (id % 8) * step;
            const x = parentRect.left + 80 + cascade;
            const y = parentRect.top + 80 + cascade;
            subWin.moveWindowTo(x, y).catch(() => { /* not fatal */ });
          } catch (e) { /* older SDKs may not expose windowRect */ }
          subWin.showWindow().catch((e: BusinessError) => {
            hilog.error(DOMAIN, TAG, `showWindow err ${e.message}`);
          });
          return 0;
        });

        /* User closes via title bar × button. Return false so the OS
         * proceeds with closing; XComponent surface destruction then
         * notifies the native side through OnSurfaceDestroyed. */
        subWin.on('subWindowClose', () => {
          hilog.info(DOMAIN, TAG, `subWindowClose id=${id}`);
          EntryAbility.subWindows.delete(id);
          EntryAbility.refocusMainWindow();
          return false;
        });
      })
      .catch((err: BusinessError) => {
        hilog.error(DOMAIN, TAG,
          `createSubWindowWithOptions err ${err.code} ${err.message}`);
        return 4;
      });
      return 0;
    }
    // should not reach
    return 5;
  }

  /* Called from Index.ets when napi requests sub-window destroy (kind=1).
   * Routes to whichever window type owns the id; both paths no-op when
   * the id does not match. */
  public static destroyBlenderSubWindow(id: number): void {
    /* Virtual panels are removed by Index; harmless no-op for system
     * windows (no matching panel). */
    if (EntryAbility.destroyVirtualSubWindow) {
      EntryAbility.destroyVirtualSubWindow(id);
    }
    if (EntryAbility.destroying.has(id)) return;
    const win = EntryAbility.subWindows.get(id);
    if (!win) return;
    EntryAbility.destroying.add(id);
    EntryAbility.subWindows.delete(id);
    win.destroyWindow()
      .catch((e: BusinessError) => {
        hilog.error(DOMAIN, TAG, `destroySubWindow err ${e.message}`);
      })
      .finally(() => {
        EntryAbility.destroying.delete(id);
        EntryAbility.refocusMainWindow();
      });
  }

  /* Return keyboard focus to the main window's root container
   * (Index.ets #blenderRoot) after a sub-window closes, otherwise
   * physical keyboard shortcuts stop working until the user taps. */
  private static refocusMainWindow(): void {
    try {
      EntryAbility.mainWindow?.getUIContext().getFocusController()
        .requestFocus('blenderRoot');
    } catch (e) { /* ignore */ }
  }
}
