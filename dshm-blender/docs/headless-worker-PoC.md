# Headless Worker PoC（2026-11-14）

## 结论

官方 Native 子进程方案已在真机跑通：

```text
RC 0 PID 6932
worker start
calling Blender_Init
VER 4.5.16 LTS NUMPY 1.24.4 SUB 98 96
```

子进程可以：

- 加载应用自己的 `libblender_worker.so`
- 在子进程里 `dlopen("libblender.so")`
- 调 `Blender_Init(--background ...)`
- 跑 Python / numpy / 细分
- 无 XComponent、无 GUI 窗口

这证明“受控 Native 子进程 + libblender 无头”路线可行。

## 实现

### 1. worker .so

`libblender_worker.so` 导出：

```c
void Main(NativeChildProcess_Args args)
```

内部：

```c
void *h = dlopen("libblender.so", RTLD_NOW | RTLD_GLOBAL);
BlenderInitFn init = dlsym(h, "Blender_Init");
const char *argv[] = {"blender", "--background", "--factory-startup", "--python-expr", expr};
int rc = init(5, argv);
```

### 2. 启动

主进程用官方 API：

```c
OH_Ability_StartNativeChildProcess("libblender_worker.so:Main", args, options, &pid);
```

本次验证通过 Python `ctypes` 调用，避免改 ArkTS 主程序。

### 3. 验收脚本

子进程里的 Python：

```python
import bpy, numpy
bpy.ops.mesh.primitive_cube_add()
o = bpy.context.object
m = o.modifiers.new('S', 'SUBSURF')
m.levels = 2
dg = bpy.context.evaluated_depsgraph_get()
me = o.evaluated_get(dg).to_mesh()
open('/data/storage/el2/base/haps/entry/files/headless_result.txt','w').write(
    f'VER {bpy.app.version_string} NUMPY {numpy.__version__} SUB {len(me.vertices)} {len(me.polygons)}')
```

## 边界

- 子进程 **没有 UI / Context**
- `Main()` 返回后子进程退出
- 还没有父子 IPC、长期 worker、任务队列
- 后台生命周期仍受鸿蒙应用进程管理约束
- 尚未接 DSHM 的 `worker/headless` 工具

## 下一步

1. 给 worker 加 IPC（共享内存/管道/OHIPCRemoteStub）
2. 支持多任务复用，不要每个任务起一次 Blender
3. 处理子进程里的日志、datafiles、Python 路径
4. 把 headless job 接进插件，但保持“长时 R&D”标记，不承诺桌面级后台
## 常驻 worker 版本（第二轮）

在第一轮 `--python-expr` 一次性执行的基础上，子进程里跑了一个 Unix socket 服务：

- socket：`/data/storage/el2/base/haps/entry/files/blender_worker.sock`
- ready 文件：`...sock.ready`
- 协议：按行 JSON，请求 `{code}`，响应 `{ok,stdout,stderr,error}`
- 发送 `{quit:1}` 后 worker 退出

父进程测试结果：

```text
START 0 9264
READY True 9264
RESP {"ok": true, "stdout": "VER 4.5.16 LTS NUMPY 1.24.4 SUB 98 96\n", "stderr": "", "error": ""}
```

说明 Blender 只初始化一次，后续任务通过 socket 复用同一个 headless 进程。

注意：发送 quit 后子进程立即退出，父进程可能收到 `BrokenPipeError`，属于预期行为。

## 当前仍是 PoC

- 没有鉴权，只依赖应用沙箱内的 Unix socket
- 没有任务取消/超时/并发控制
- 没有图像/文件回传协议
- 没有接进 dshm 插件
- HAP 里目前带着实验用 `libworker_test.so` / `libblender_worker.so`
