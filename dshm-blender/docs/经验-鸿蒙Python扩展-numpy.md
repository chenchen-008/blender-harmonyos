# 经验：鸿蒙上让 Python C 扩展（numpy）真正可用

> 环境：Blender 4.5.12 LTS OHOS fork、HarmonyOS SDK 6.1.1（API 24）、MatePad Pro、Python 3.11.11  
> 结论：**A 方案（共享 libpython + 扩展 NEEDED libpython）成功；C 方案（RTLD_LAZY）失败。**  
> 实测：`OK numpy=1.24.4 det=-2.000000`

## 一、最终架构

```text
libblender.so
  └─ NEEDED libpython3.11.so.1.0        （HAP libs/arm64-v8a）
numpy 扩展 .so（19 个）
  └─ HAP libs/arm64-v8a/numpy/...      （必须在 el1 bundle）
  └─ NEEDED libpython3.11.so.1.0
numpy 的 .py
  └─ python_home/lib/python3.11/numpy  （el2 可写目录）
numpy_loader.py（scripts/startup）
  └─ 安装 import hook：numpy.* 扩展从 HAP libs 加载
```

## 二、两个根因

### 1. 路径：只允许 el1 bundle
鸿蒙只允许从安装目录加载 so：

- 允许：`/data/storage/el1/bundle/libs/arm64/...`
- 拒绝：`/data/storage/el2/base/.../files/...`

报错：`MUSL-LDSO ... check ns accessible failed`

### 2. 符号：BIND_NOW + 没有 NEEDED libpython
musl 默认 BIND_NOW，dlopen 时要求所有符号立即可解析。numpy 扩展原本只有：

```text
NEEDED: libc.so
```

而 `PyType_Ready`、`PyLong_AsSsize_t` 都在 Python 里，所以：

```text
Error relocating ...: PyType_Ready: symbol not found
```

另外 OpenHarmony 的 `namespace.default.ignore_global_library=true` 会让 RTLD_GLOBAL 库对后续 dlopen 不可见，所以 `dlsym(RTLD_DEFAULT)` 能查到也没用。

**必须让扩展显式依赖 libpython。**

## 三、A 方案步骤

1. CPython 重建时加 `--enable-shared`，得到 `libpython3.11.so.1.0`。
2. `platform_ohos.cmake` 的 `PYTHON_LIBRARY` 指向 `.so`，重链 libblender；确认 NEEDED 有 `libpython3.11.so.1.0`。
3. 重编 numpy，`LDSHARED` 加 `-L<python lib> -lpython3.11`；确认扩展 NEEDED 有 libpython。
4. HAP 打包：
   - `libs/arm64-v8a/libpython3.11.so.1.0`
   - `libs/arm64-v8a/numpy/**/*.so`
   - rawfile `numpy-ohos-py.tar.zst`（只含 .py，解压到 `python_home/lib/python3.11`）
   - rawfile `numpy-loader.tar.zst`（解压到 `blender/scripts/startup`）
5. `numpy_loader.py` 安装 import hook：

```python
class NumpyExtFinder:
    def find_spec(self, fullname, path=None, target=None):
        if not fullname.startswith('numpy.'):
            return None
        base = os.path.join('/data/storage/el1/bundle/libs/arm64/numpy',
                            *fullname.split('.')[1:])
        for suf in ('.cpython-311.so', '.abi3.so', '.so'):
            p = base + suf
            if os.path.exists(p):
                return importlib.util.spec_from_file_location(fullname, p)
        return None

sys.meta_path.insert(0, NumpyExtFinder())
```

## 四、C 方案为什么失败

去掉扩展的 `NEEDED libpython`，用：

```python
sys.setdlopenflags(os.RTLD_LAZY | os.RTLD_GLOBAL)
```

结果：

```text
FLAGS_OK
LAZY_FAIL ... Error relocating libnp_lazy.so: PyLong_AsSsize_t: symbol not found
```

结论：**鸿蒙 musl 的 BIND_NOW 不能靠 RTLD_LAZY 绕过**；C 只能当排障经验。

## 五、其他踩坑

| 问题 | 现象 | 修法 |
|---|---|---|
| NEEDED 绝对路径 | `/opt/build/.../libtbb.so` | 原地改 ELF .dynstr 为 soname；不要反复 patchelf |
| NEEDED 相对路径 | `src/lib/Iex/libIex-3_2.so` | 所有含 `/` 的 NEEDED 改 basename |
| soname 不同 | libpng vs libpng16 | HAP 补兼容名 |
| 数据文件 LFS 占位符 | startup/splash/preview 131 字节 | 从 blender-data 补真实文件 |
| 预写 userpref | Quick Setup 语言选择消失 | 不要预写 userpref.blend |
| DPI 默认 96 | 界面太小 | GHOST getDPIHint 返回 192，支持 DSHM_DPI |
| libentry 被 patchelf | find_sym2 崩溃 | 用 CMake 重建，不要改 libentry |
| 包名硬编码 | 换 bundle 字体/目录错 | setPublicHome(path) 传真实路径 |

## 六、验证

```bash
llvm-readelf -d libpython3.11.so.1.0 | grep NEEDED
llvm-readelf -d numpy/core/_multiarray_umath.cpython-311.so | grep -E 'NEEDED|BIND'
hdc shell hilog -x | grep -E 'Blender_Init|numpy|dlopen|symbol'
```

期望探针日志：`OK numpy=1.24.4 det=-2.000000`。

## 七、给 4.5.15 的提醒

- 源码必须用含真实 assets 的 tarball，不要用 GitHub 仓库/LFS 快照；
- Python 继续 shared libpython，否则 A 要重做；
- OHOS 补丁逐项移植：GHOST OHOS、platform_ohos.cmake、creator HOME、Vulkan guards、wm_add_default、datafiles；
- numpy 的 LDSHARED 必须带 `-lpython3.11`。
