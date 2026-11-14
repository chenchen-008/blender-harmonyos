# 无 spawn 版 dshm-blender 交付说明

## 这是什么

DSHM 端侧的 Blender 直连插件，纯 JS + Blender addon，**不使用 child_process / spawn / subprocess**。  
只驱动当前已打开的 Blender，不启动新进程。

## 包含

- 19 个模型工具：
  status、scene、object、run、see、cmd、commands、export、watch、view、txn、preset、opt、perf、viewport、loop、pick、plan、job。
- Blender addon：dshm_bridge.py，31 条命令。
- plan catalog：28 family / 186 op（仅 catalog/help/search）。
- 进程内 job：主线程 timer 队列，会短暂卡 UI。

## 不包含

- headless / worker / 新起 Blender 进程：端侧没有 spawn，未移植。
- assembly / blockout / eval / visualreport / shapesolve：5 个 family 未移植。
- img / gate 的 7 个 op：未移植。
- 上游 33 family / 217 op 的完整执行器。

## 验证

- npm test：8 protocol + 19 tool integration 断言通过。
- no-spawn selftest：lib/ 与 addon/ 未发现进程创建代码。
- 打包：dsh-external-dshm-blender-0.1.0.tgz。

## 安装

1. 把 tgz 或仓库安装进 DSHM 插件；
2. Blender 侧安装并启用 addon/dshm_bridge.py；
3. 同机使用需分屏保持 Blender 可见；
4. 先调用 blender_status 验证。

## 后续

- 补 img/gate；
- 补 assembly；
- 补 blockout/eval；
- numpy 可用后补 visualreport/shapesolve；
- 目标 catalog 33/217，工具数保持 19（不新增 spawn 能力）。
