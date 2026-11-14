import { hilog } from '@kit.PerformanceAnalysisKit';
import { bindSubXComponent } from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'Blender_UI_SubWin';

/* A single Blender sub-window. The numeric id is stashed in AppStorage by
 * EntryAbility.createBlenderSubWindow just before setUIContent, so this
 * page knows which XComponent id to render into. Once XComponent onLoad
 * fires we call bindSubXComponent so napi wires up mouse/key/axis dispatch
 * for this specific surface. */
@Entry
@Component
struct SubWindowPage {
  @State winId: number = 0;
  private ctrl: XComponentController = new XComponentController();

  /* Modifier tracking mirrors Index.ets: bit0=CTRL bit1=SHIFT bit2=ALT. */
  private modCtrl: boolean = false;
  private modShift: boolean = false;
  private modAlt: boolean = false;

  private trackModifierMask(event: KeyEvent): number {
    const down = event.type === KeyType.Down;
    switch (event.keyCode) {
      case 2072: case 2073: this.modCtrl  = down; break;
      case 2047: case 2048: this.modShift = down; break;
      case 2045: case 2046: this.modAlt   = down; break;
      default: break;
    }
    return (this.modCtrl  ? 1 : 0)
         | (this.modShift ? 2 : 0)
         | (this.modAlt   ? 4 : 0);
  }

  aboutToAppear(): void {
    const v = AppStorage.get<number>('pendingSubWinId');
    this.winId = (v === undefined || v === null) ? 0 : v;
    hilog.info(DOMAIN, TAG, `SubWindowPage aboutToAppear winId=${this.winId}`);
  }

  build() {
    Stack() {
      XComponent({
        id: `blender_sub_${this.winId}`,
        type: XComponentType.SURFACE,
        libraryname: 'entry',
        controller: this.ctrl,
      })
        .width('100%')
        .height('100%')
        .focusable(true)
        .focusOnTouch(true)
        .onLoad((ctx?: object) => {
          hilog.info(DOMAIN, TAG, `sub XComponent onLoad id=${this.winId}`);
          /* Bind this XComponent's native handle to napi so mouse/key
           * dispatchers are registered. The onLoad ctx argument is the
           * NativeXComponent object we hand off unchanged. */
          if (ctx) {
            try {
              bindSubXComponent(ctx);
            } catch (e) {
              hilog.error(DOMAIN, TAG, `bindSubXComponent failed: ${(e as Error).message}`);
            }
          }
        })
        .onDestroy(() => {
          hilog.info(DOMAIN, TAG, `sub XComponent onDestroy id=${this.winId}`);
        })
    }
  }
}
