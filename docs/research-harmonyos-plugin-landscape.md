# 鸿蒙开发插件（hdc + hap 签名）先期调研

> 调研时间：2026-10-31 · 结论：同类插件已经存在且相当成熟，"设备层"没有空位，"签名层"是共同缺口。

## 0. 一句话结论

- **有**：`1na-ko/dsh-hdc-bridge`（MIT，v0.10.2，25 个工具）已经把 **hdc 设备闭环 + DevEco CLI 构建/签名 + 设备面板 + 离线知识层**做完了，和你的设想正面重合约 90%。
- 鸿蒙 DSH 插件同类至少 6 个（设备、投屏、模拟器、Inspector、CLI 封装、知识库），再做一个"hdc 封装"没有差异化。
- 安卓 / iOS 侧同样成熟：`dsh-android`(20 工具)、`dsh-ios`(22 工具)、`XcodeBuildMCP`(6.5k★)、`mobile-mcp`(10k★)。
- **三端所有插件的共同空白：签名只被当作"构建工具的副作用"**。iOS 插件明说"设备工具需要你先在 Xcode 里配好签名"，Android 插件只管 Gradle，鸿蒙插件把签名外包给需要 OAuth 的 `devecocli`。
- 建议：**不做第二个 hdc 套件，做"签名工作台"（签名资产台账 + 离线/CI 签名 + 症状诊断），并以三端同构为长期护城河。**

---

## 1. 鸿蒙 / OpenHarmony 侧已有插件

| 项目 | 形态 | 能力 | 签名深度 | 热度 |
| --- | --- | --- | --- | --- |
| [1na-ko/dsh-hdc-bridge](https://github.com/1na-ko/dsh-hdc-bridge) | DSH 插件（tools + client 面板 + skills） | hdc 设备/截图/UI 树/输入/安装/hilog/崩溃、hms_build（build/run/sign/clean）、hms_emulator、hms_api/lint/docs、离线 28 篇官方知识 | 依赖 `devecocli`，**签名前需人工 OAuth 登录**；有 11 条错误码中文修复指引（含 9568332 未绑 UDID） | 20★ / npm 0.10.2 |
| [MoonlitDropOfBlood/dsh-deveco](https://github.com/MoonlitDropOfBlood/dsh-deveco) | DSH 插件（右侧栏 Tab） | HiLog 检查器、ArkUI Inspector（截图点选 + 组件树 + 盒模型）、.ets 查看器 | 无 | 5★ |
| [fz-lyle/dsh-deveco-cli](https://github.com/fz-lyle/dsh-deveco-cli) | DSH 插件 + CLI | 封装 devecocli：ohpm/hvigor/hdc/emulator/hilog、脚手架、文档检索、MCP | `devecocli signature generate`（p12/csr/p7b/cer + 写 build-profile.json5），偏 debug | 4★ |
| [momo20012002/dsh-hmos-emulator](https://github.com/momo20012002/dsh-hmos-emulator) | DSH 插件（侧边栏） | 模拟器管理、构建部署、日志、截图、8 个调试工具 | 无 | 2★ |
| [ns-zzj/dsh-hos-scrcpy](https://github.com/ns-zzj/dsh-hos-scrcpy) | DSH 插件（投屏 provider） | hdc 投屏、点按滑动、按键、文字注入 | 无 | 8★ |
| [linhay/harmony-next.skills](https://github.com/linhay/harmony-next.skills) | Skill 库（非插件） | API 12–26 声明、ArkTS/ArkUI、NDK、DevEco、模拟器离线知识 | 无 | 366★ |
| [craftysecurity/HAP-Tool](https://github.com/craftysecurity/HAP-Tool) | Python CLI（非 DSH） | pack / unpack / **sign** / install，直调 `hap-sign-tool.jar` | 本地证书签名，但需手工准备材料 | 3★ |

非 DSH 生态还有一批 MCP：`harmonyos-mcp`、`mcp-harmonyos`、`harmony-tools-mcp`(PyPI)、`hdc-mcp`(npm) —— 全是 hdc 能力封装，进一步说明设备层已饱和。

### dsh-hdc-bridge 具体做了什么（我们的对照基线）

- 设备：list/connect/shell/screenshot/install/hilog/ui_dump/ui_find/ui 操作/app 管理/崩溃抓取/diag
- 工程：switch_cwd、build_project（hvigorw 回退）、arkts_check、start_app、hdc_log
- 官方通道：hms_setup（体检）、hms_build（build/run/**sign**/clean）、hms_api、hms_docs、hms_api_change、hms_lint、hms_emulator、hms_knowledge
- 体验：官方侧边栏入口 + 浮动设备面板 + 4 条 REST + 5 个运行时技能 + 11 条错误码修复建议
- **它没有做的**：本地/离线 release 签名、证书与 profile 台账、UDID 与 profile 的交叉校验、签名验证报告、多 product × debug/release 矩阵、CI 导出、Android/iOS 侧

---

## 2. 安卓侧：插件长什么样

| 项目 | 形态 | 能力 | 签名 |
| --- | --- | --- | --- |
| [ZSeven-W/dsh-android](https://github.com/ZSeven-W/dsh-android) | DSH 插件（20 工具 + 实时投屏面板） | adb 设备/启停/截图/UI 操作(资源 id 或 OCR)/Gradle 构建安装/logcat/进程内存 | **无**；release 签名完全交给 Gradle |
| [Android Tools for VS Code](https://marketplace.visualstudio.com/items?itemName=levkosyk.vscode-android-tools) | VS Code 扩展 | 本地优先的 Android 控制中心：设备、logcat、构建 | 无独立签名 UI |
| [android-mcp-server](https://pypi.org/project/android-mcp-server/) | MCP | adb 封装 | 无 |
| 官方工具链 | adb / aapt2 / zipalign / **apksigner** / bundletool | 安装、对齐、签名、AAB | `keystore.properties` + `signingConfigs` + `apksigner verify` |

安卓的"签名"事实标准：keystore(.jks) → signingConfigs(release) → `zipalign` 后 `apksigner sign`（v1/v2/v3/v4）→ `apksigner verify --print-certs`。**没有任何一个 agent 插件把它做成工具**，全靠人配 Gradle。

## 3. iOS 侧：插件长什么样

| 项目 | 形态 | 能力 | 签名 |
| --- | --- | --- | --- |
| [ZSeven-W/dsh-ios](https://github.com/ZSeven-W/dsh-ios) | DSH 插件（22 工具 + MJPEG 面板） | 模拟器/真机(WebDriverAgent)、构建运行、UI 树、日志、进程/泄漏 | 真机仅要求"你已在 Xcode 配好 Apple Development 签名" |
| [harde1/dsh-xcodebuild](https://github.com/harde1/dsh-xcodebuild) | DSH 插件 | build/test/clean/archive/run + 日志流 | 无 |
| [zhz8888/dsh-ios-simulator-helper](https://github.com/zhz8888/dsh-ios-simulator-helper) | DSH 插件（21 工具 + 设置页） | 工程发现、构建、安装、启动、截图、日志 | 无 |
| [cameroncooke/XcodeBuildMCP](https://github.com/cameroncooke/XcodeBuildMCP) | MCP（6.5k★, MIT） | iOS/macOS 构建测试归档 | README 明说"设备工具需要先在 Xcode 配好签名" |
| [mobile-next/mobile-mcp](https://github.com/mobile-next/mobile-mcp) | MCP（10k★, Apache-2.0） | 跨 iOS/Android 驱动模拟器与真机 | 无 |

iOS 签名链：证书(.p12) + Provisioning Profile(.mobileprovision) + 设备 UDID + entitlements → `codesign` → `xcodebuild -exportArchive`（ExportOptions.plist）→ `notarytool`（macOS 分发）。同样是**没人做工具化**。

---

## 4. 横向规律（决定了我们该做什么）

三家插件的功能公式高度一致：

```
设备发现 → 实时画面 → 输入注入 → 构建/安装 → 日志/崩溃 → （知识/文档）
```

而签名在三家都是"断点"：

| 平台 | 插件现状 | 真实痛点 |
| --- | --- | --- |
| 鸿蒙 | 签名 = 调 devecocli（要 OAuth）；debug 自动签名可生成材料 | release 证书/Profile 申请与绑定、多设备 UDID、离线/CI、9568332 类错误的根因定位 |
| 安卓 | 签名 = Gradle 配置 | keystore 丢失/别名错、v2 缺失、上传密钥 vs 发布密钥、AAB 与 APK 不一致 |
| iOS | 签名 = "去 Xcode 配" | 证书过期、Profile 不含新设备 UDID、entitlements 不匹配、ExportOptions 选错方式 |

**共同结构：证书 × Profile × 设备 UDID × 有效期 × 构建变体。** 这是一个可以跨三端抽象的问题域，而现有插件都只做了"构建"这一半。

---

## 5. 建议我们做什么

### 方案 A（不建议）：再做一个鸿蒙 hdc 套件
正面撞 dsh-hdc-bridge（25 工具、面板、知识层、错误码库、v0.10.2 已迭代 10 个月）。除非你能在"更轻/更快/更好看"上赢，否则没有安装理由。

### 方案 B（推荐起步）：签名工作台 Signing Desk
以"签名"为第一公民，hdc 只作为取 UDID / 装包验证的配角（可声明依赖 dsh-hdc-bridge 共存）。

MVP（鸿蒙优先，8–10 个工具）：

1. `sign_doctor` —— 工程体检：解析 `build-profile.json5` 的 signingConfigs，检查 p12/cer/p7b 是否存在、是否过期、bundleName 是否匹配、profile 是否含当前设备 UDID
2. `sign_material` —— 材料台账：列出/导入/生成（keytool 生成 p12 → CSR → 证书；profile 模板渲染）
3. `sign_profile` —— 解析/编辑 .p7b（bundle-name、device-ids、validity、apl、distribution-certificate）并重签
4. `sign_hap` —— 直调 `hap-sign-tool.jar`（debug/release、指定材料、批量），**不依赖 devecocli / OAuth**
5. `sign_verify` —— `verify-app` + 证书链/有效期/UDID 覆盖报告（结构化，不是一段 stdout）
6. `sign_matrix` —— product × buildMode × 设备 矩阵批量签名与产物清单
7. `device_udid` —— hdc 取 UDID，与 profile 做差集，直接给"该补哪台设备"
8. `sign_export_ci` —— 导出 GitHub Actions/GitLab CI 的签名步骤与 secrets 清单（不落明文密钥）

明确的差异化：**离线可用、release 可用、可诊断、可批量、可进 CI**。这四点是 devecocli 与 dsh-hdc-bridge 都不覆盖的。

### 方案 C（长期护城河）：三端统一"移动交付"插件
在同一套资产模型下接 Android（`apksigner`/`bundletool`）与 iOS（`codesign`/`xcodebuild -exportArchive`/`notarytool`）。三端同构的价值：一个"签名台账"管三个平台，一套诊断语言，一套 CI 导出。工作量约为 B 的 2.5 倍，但独特性最高。

**推荐路径：按 C 设计数据模型，按 B 交付 MVP（鸿蒙），再横向扩 Android → iOS。**

### 明确不做
- 不做设备投屏/UI 自动化（dsh-hdc-bridge、dsh-hos-scrcpy、dsh-android 已占）
- 不做 ArkTS 语言服务 / ArkUI Inspector（dsh-deveco、ohosvscode 已占）
- 不做知识库（harmony-next.skills 366★ 已占）
- 不重写构建系统（hvigor/Gradle/xcodebuild 只调用不替代）

---

## 6. 主要风险与前置确认

1. **与 dsh-hdc-bridge 的重叠**：它已有 `hms_build sign` + 错误码指引。我们的定位必须写清"release / 离线 / 多设备 / 三端"，否则会被当成重复品。
2. **合规**：`hap-sign-tool.jar`（OpenHarmony developtools_hapsigner，Apache-2.0）可调用但不宜随包分发 DevEco 私有材料；AGC 证书申请涉及账号条款，插件只做"引导 + 本地编排"，不代持账号。
3. **平台限制**：iOS 侧只能在 macOS 跑；Android 需 JDK；鸿蒙需 DevEco/CLT 工具链（可用性探测要优雅降级）。
4. **密钥安全**：台账只存引用与指纹，私钥留在用户 keystore/钥匙串；日志与 CI 导出必须脱敏。
5. **命名与分发**：npm 包名、DSH 插件清单、是否进 dshplugin.store / dshhub.dev。

## 7. 需要你拍板的三件事

1. 目标用户：自己/团队内部用（那直接用 dsh-hdc-bridge 更省事），还是要发布到插件市场？
2. 范围：只做鸿蒙签名（B），还是一开始就三端同构（C）？
3. 交付形态：纯 agent 工具（headless 可用），还是带一个签名台账的可视化面板（像 dsh-hdc-bridge 那样）？
