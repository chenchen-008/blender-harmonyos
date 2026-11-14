# 已知限制：Maleoon 910 + EEVEE Next 渲染纯黑

> 设备：HUAWEI MatePad Pro（XYAO-W00），Maleoon 910
> Blender：4.5.12 / 4.5.16 OHOS 移植版
> 现象：EEVEE / EEVEE Next 渲染结果为纯黑；Workbench 正常。

## 实测能力

通过 Blender Python 查询：

    gpu.platform.backend_type_get() -> VULKAN
    gpu.platform.vendor_get()       -> 6629
    gpu.platform.renderer_get()     -> Maleoon 910
    gpu.platform.version_get()      -> Maleoon 910 B312

Vulkan 扩展列表缺少：

- VK_KHR_dynamic_rendering
- VK_EXT_provoking_vertex
- VK_KHR_fragment_shader_barycentric

## 关键原因

Blender 的 OHOS Vulkan 补丁里有一段：

    bool VKBackend::is_supported()
    {
    #ifdef __OHOS__
      CLOG_INFO(&LOG, 2, "OHOS: skipping VKBackend feature probe, trusting GHOST.");
      return true;
    #endif

即为了绕开 Maleoon 916 的能力误报，OHOS 直接跳过 Vulkan 特性校验。
结果：

- Blender 能初始化 Vulkan 后端；
- Workbench 这种较简单的管线能工作；
- EEVEE Next 需要更完整的 Vulkan 特性/着色器管线，可能着色器编译失败或输出全黑；
- 普通日志里不一定有明确错误，只看到“保存了 black 图”。

## 插件现在的行为

- blender_view 默认 engine=workbench；
- 显式 engine=eevee / cycles 才切换；
- blender_see 截图前临时把视口切到 SOLID（Workbench），截完恢复原 shading；
- 因此普通看图/汇报不会再用 EEVEE 出黑图。

## 同类资料

- Blender Android 移植记录：Mali 驱动缺少 dual source blending / vertex stage stores / clip distance，需要专门绕过。
- 社区讨论：Maleoon 910 的 Vulkan 版本约 1.2.275，缺少 1.3 的动态渲染等硬件特性（未官方确认，但与本机扩展列表一致）。

## 结论

这不是场景、材质或 OpenSubdiv 的问题，是 **Maleoon 910 驱动能力 + Blender OHOS 跳过能力校验** 的组合限制。

## 可用替代

| 用途 | 方案 |
|---|---|
| 视口预览 / QC | Workbench |
| 插件出图 | blender_view 默认 Workbench |
| 正式渲染 | Cycles CPU（当前构建 WITH_CYCLES OFF，需要时打开） |
| 高画质 | PC 远程渲染 |

不要在鸿蒙端承诺 EEVEE 可用。

## 验收口径

- 不再用 EEVEE 出图作为验收；
- 细分/建模验收看 evaluated mesh 的顶点/面数；
- 预览图用 Workbench；
- 需要渲染质量时走 Cycles CPU 或远程。

## 参考

- https://blenderandroid.github.io/updates/2026-09-07-main-android-alpha-3/
- https://bbs.itying.com/topic/68d81cb02cb460013cc180fd
