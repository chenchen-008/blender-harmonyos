# DSHM Blender Bridge

让 DSHM（DeepSeek Harness 鸿蒙版）在同一台鸿蒙设备上直接看 Blender、改场景、跑 Python、截图验证、导出模型。

**不是在移植 Blender，也不是把 dsh-blender-plugin 原样搬过来。** Blender 本体由已有的鸿蒙移植版提供；本仓库是面向 DSHM 的通道重写：

- **Blender 侧**：一个精简 addon（Python），监听本地 TCP，所有 bpy 操作回到主线程执行。
- **DSHM 侧**：纯 JS 插件（node:net，无 spawn / 无 undici / 无 WASM / 无原生模块），把通道包装成 19 个模型工具；全部走现有 Blender 主线程，不起子进程。
- 同机使用时必须让 Blender 保持可见（分屏或悬浮窗）：鸿蒙会冻结后台应用的主线程。

## 已实测环境

| 项 | 值 |
|---|---|
| 设备 | HUAWEI MatePad Pro（XYAO-W00） |
| 系统 | HarmonyOS 6.1.0.135(SP9C00E125R1P3)，API 24 |
| Blender | com.suwan.blenderonharmonyos，4.5.12 LTS，Python 3.11.11 |
| DSHM | com.dshm.dshclient 1.1.3，核心 0.2.1-alpha.4+dshm.56 |
| 验收日期 | 2026-10-18 |

## 能力

| 工具 | 作用 |
|---|---|
| blender_status | 连接诊断；失败时给出可操作原因（没启动 / addon 没启用 / 后台冻结） |
| blender_scene | 场景、对象列表、集合、活动对象、帧、渲染引擎 |
| blender_object | 单个对象的变换、尺寸、网格统计、材质、修改器、自定义属性 |
| blender_run | 在 Blender 主线程执行 Python（持久内核 K），默认同时回一帧视口 |
| blender_see | 抓取当前 3D 视口 PNG 并作为图片回传 |
| blender_cmd | 透传任意 addon 命令 |
| blender_commands | 列出 addon 命令 |
| blender_export | 导出 obj / stl / ply / glb / gltf / fbx；inline=true 时把字节写回 DSHM 工作区 |

另外 11 个工具：

| 工具 | 作用 |
|---|---|
| blender_watch | 时间窗采样视口帧 + hash |
| blender_view | 自定义机位取图 |
| blender_txn | 文件快照 / 对象变换标记与回滚 |
| blender_preset | 场景 / 渲染参数预设保存与应用 |
| blender_opt | 减面 / 合并等场景精简 |
| blender_perf | 场景 / 渲染统计与单帧基准 |
| blender_viewport | 通道状态、租约、诊断（不启动新进程） |
| blender_loop | 主线程内参数搜索循环 |
| blender_pick | 让用户在 Blender 里点选对象 |
| blender_plan | 规划目录（当前 28 family / 186 op，仅 catalog/help/search） |
| blender_job | 进程内作业队列（主线程执行，会短暂卡 UI） |
| blender_headless | 鸿蒙原生子进程无头 Blender（R&D：start/run/stop） |

## 安装（两条路）

### A. GitHub 安装（推荐，干净）

1. 把本仓库 push 到 GitHub（根目录就是包根，lib/ 已提交，无需构建）。
2. 在 DSHM 的「插件」页填入仓库地址：
   https://github.com/<你的账号>/<仓库名>
3. 等安装完成，重启 DSHM。
4. 确认工具出现：新会话里问「调用 blender_status」。

> 如果之前用设备内安装写过用户 patch（$DSH_HOME/cordis.patch.yml 里的 dshm-blender 行），
> 先删掉那一行，避免与 bundle 自带的 patch 重复挂载同一个工具集。

### B. 设备内安装（开发/离线）

见 docs/安装与使用.md。概要：

1. 在 PC 上把本仓库用 HTTP 服务暴露（默认 8002），并做 hdc rport。
2. 在 DSHM 的 python 工具里执行 tools/install_on_device.py。
3. 脚本会写入核心树与 profile、把包加入 dsh-web-app 依赖闭包、写用户 patch。
4. 重启 DSHM。

## Blender 侧 addon

文件：addon/dshm_bridge.py

安装方式二选一：

- Blender 偏好设置 → Add-ons → Install，选择 dshm_bridge.py，启用 **DSHM Blender Bridge**；
- 或在 Blender 的 Python Console 执行安装脚本。

默认监听 127.0.0.1:9876，开机自启。N 面板（3D 视图侧栏 → DSHM）可 Start/Stop、改端口/token。
非回环监听（0.0.0.0）必须设置 token，否则拒绝启动。

## 同机使用的硬约束：分屏

鸿蒙把后台应用的主线程冻结。Blender 在后台时：

- TCP 仍然能 connect（监听线程还活着）；
- bpy 命令永远排不到主线程，表现为「连接成功但 0 字节返回」。

所以必须分屏（或让 Blender 悬浮可见），确认 Blender 画面仍在刷新。
这不是插件 bug，是平台调度行为；blender_status 会直接给出这句提示。

## 配置

优先级：环境变量 > $DSH_HOME/dshm-blender.json > 插件配置 > 默认值。

| 键 | 默认 | 说明 |
|---|---|---|
| host | 127.0.0.1 | Blender addon 地址 |
| port | 9876 | addon 端口 |
| token | 空 | 非回环时必须设置，addon 侧同值 |
| timeoutMs | 30000 | 普通命令超时 |
| imageTimeoutMs | 90000 | 截图超时 |

环境变量：DSHM_BLENDER_HOST / PORT / TOKEN / TIMEOUT_MS；
配置文件也可用 DSHM_BLENDER_CONFIG 指定。

## 验收结果（2026-10-18，真机）

- blender_status：kind=ok | Blender 4.5.12 LTS | Python 3.11.11 | 23ms
- blender_scene：3 objects；Cube / Light / Camera；BLENDER_EEVEE_NEXT
- blender_run：executed=true 5ms；Cube 绕 Z 转 0.5；回帧 640x419 135KB 435ms
- blender_see：640x419 135KB 439ms；图片在 DSHM 对话里正常显示
- blender_export：obj 949 字节 17ms；写入 DSHM 工作区
  /storage/Users/currentUser/Download/com.amcl.launcher/blender-export-*.obj

## 已知边界

- 当前 20 个工具，catalog 28 family / 186 op；上游 v2.6.1 是 33 family / 217 op。
- 缺 5 个 family：assembly、blockout、eval、visualreport、shapesolve；缺 img/gate 的 7 个 op。
- headless / worker：桌面版的 `spawn("blender", ["-b"])` 方式不可移植。官方受控 Native 子进程方案已做 PoC：`libblender_worker.so` 在子进程里 `Blender_Init(--background)`，真机跑通 4.5.16 + numpy + 细分；但还没有 IPC/任务队列，未产品化，见 `docs/headless-worker-PoC.md`。
- DSHM 插件 JS 侧没有 `child_process`；`blender_job` 是进程内分片任务，`blender_headless` 通过 addon 调官方 Native 子进程，不是 JS spawn。
- numpy 取决于 HAP：4.5.12 + A 测试包已带 numpy；官方 4.5.12.6 不带。glTF/GLB 导出在无 numpy 的包上会失败，请用 obj / stl / ply。
- 截图使用 screen.screenshot_area，包含 UI 叠加层；不支持 from/look_at 自定义机位。
- 设备内安装会写核心树；升级 DSHM 核心后需要重装（GitHub 安装不受影响）。

## 许可证

BSD-3-Clause。设计思路参考了 sixtysevenlf/dsh-blender-plugin（BSD-3-Clause），
但本仓库的 addon 与插件代码是面向 DSHM 重新实现的。
