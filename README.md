# Blender 4.5.16 LTS on HarmonyOS

把 Blender 4.5.16 LTS 以 HAP + libblender.so 的方式跑到 HarmonyOS 平板上，并提供 DSHM 插件，让 DSHM 能通过自然语言控制 Blender。

这是平台适配版，不是重写 Blender。建模、渲染、几何节点、Python API 仍来自 Blender 4.5.16 LTS。

## 当前状态

- Blender：4.5.16 LTS，arm64 HarmonyOS
- HAP：测试包 com.suwax.blenderonharmonyos
- 原包 com.suwan.blenderonharmonyos 未修改
- 细分曲面：OpenSubdiv 已启用，CPU 求值
- Python：3.11 + numpy 1.24.4
- DSHM 插件：20 个工具

真机验证：

    VER 4.5.16 LTS
    NUMPY 1.24.4
    SUB 98 96

## 目录

| 目录 | 内容 |
|---|---|
| dshm-blender/ | DSHM 插件 + Blender addon 源码 |
| hap-shell/ | HarmonyOS HAP 的 ArkTS/NAPI/CMake 外壳源码 |
| ohos-patches/ | 改过的 Blender OHOS 平台文件，按相对路径保留 |
| tools/ | 依赖分析、NEEDED 修补、headless worker 源码 |
| docs/ | 构建、EEVEE 限制、headless PoC 等记录 |

安装踩坑集中记录：dshm-blender/docs/安装经验-总表.md，包括 namespace、el1/el2、NEEDED、libpython 等。

完整 Blender 源码不放进 Git 仓库，构建时从 Blender 4.5.16 源码树应用 ohos-patches/。

## 构建

### 1. libblender.so（WSL/Linux 交叉编译）

    cd /opt/build/hm
    OHOS_SDK=/opt/ohos-sdk/linux OHOS_ABI=arm64-v8a JOBS=4 bash build.sh blender

依赖、OpenSubdiv、lzma 等修复见：

- dshm-blender/docs/4.5.16-OpenSubdiv修复.md
- dshm-blender/docs/4.5.16-HAP打包.md

### 2. HAP（Windows DevEco）

    $env:DEVECO_SDK_HOME = 'D:/Huawei/DevEco Studio/sdk'
    $env:NODE_HOME = 'D:/Huawei/DevEco Studio/tools/node'
    $env:JAVA_HOME = 'D:/Huawei/DevEco Studio/jbr'
    $env:PATH = "$env:JAVA_HOME/bin;$env:NODE_HOME;$env:PATH"

    cd hap-shell
    & 'D:/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.bat' assembleHap --no-daemon --mode module -p product=default

产物：

    entry/build/default/outputs/default/entry-default-signed.hap

### 3. 安装

    hdc install -r entry-default-signed.hap

## DSHM 插件

插件无 child_process、无 undici、无原生模块，全部走 Blender addon 的 TCP 桥。

    cd dshm-blender
    npm test
    npm pack

设备安装脚本：dshm-blender/tools/install_on_device.py

主要工具：

- blender_status / blender_scene / blender_run
- blender_see / blender_view（截图、自定义视角）
- blender_export / blender_txn / blender_preset
- blender_job（进程内任务）
- blender_headless（R&D，Native 子进程无头 Blender）

## Headless worker（R&D）

官方受控 Native 子进程已在真机跑通：

- 子进程加载 libblender_worker.so
- dlopen("libblender.so") + Blender_Init(--background)
- 子进程内常驻 Unix socket
- 父进程发 Python，返回 stdout/error
- 验证：VER 4.5.16 LTS NUMPY 1.24.4

细节与边界：dshm-blender/docs/headless-worker-PoC.md

鸿蒙不能随便 spawn 任意可执行文件；这里用的是官方 Native 子进程 API，不是桌面版 child_process。

## 已知限制

- EEVEE 在 Maleoon 910 上渲染纯黑，不要用 EEVEE 验收；用 Workbench / Cycles CPU / 远程。见 dshm-blender/docs/EEVEE-Maleoon限制.md
- 当前 HAP 约 395 MB（libblender.so strip 后约 93 MB + 数据/Python）
- headless worker 仍是 PoC：无鉴权、无任务队列、无图片回传，不保证桌面级后台生命周期
- DSHM 侧插件如果装在应用沙箱，升级核心后可能需要重装

## 许可证

- Blender 本体及 HAP 中的 Blender 代码：GPL-3.0-or-later，见 LICENSE
- dshm-blender 插件源码：BSD-3-Clause，见该目录 README.md

## Release

HAP 作为 GitHub Release 资产发布：

    Blender-4.5.16-OHOS-arm64.hap

https://github.com/chenchen-008/blender-harmonyos/releases/tag/v4.5.16-ohos

安装前请确认设备已开启调试、允许安装测试签名包。
