# 如何在MoerEngine中添加 景深 效果？

景深是一个经典的光学现象，详情请见 [虚幻引擎 Cinematic Depth of Field文档](https://dev.epicgames.com/documentation/zh-cn/unreal-engine/cinematic-depth-of-field-in-unreal-engine)。

接下来，我们将介绍如何在MoerEngine中实现一个景深效果。

因为目前AI IDE的发展非常迅速，所以此处不再解释具体代码。如果你对代码实现、数据流向有任何的疑问，请优先询问AI。

接下来，我们将直接介绍 **在RasterRenderer中，添加景深效果** 的具体操作步骤。

## 1. 熟悉引擎

启动引擎后，你可以看到如下页面。其中，左侧是 *场景颜色*，右侧是 *配置页*。

![image-20260316154000246](DofExample/image-20260316154000246.png)

> 启用NRD的光线追踪渲染器渲染效果

选择配置页的右上角 **Render Method**，可以切换引擎使用的渲染方法。目前共支持两种渲染方法：**Raytracing 光线追踪** 和 **Rasterization 光栅化**。请选择Render Method，并切换为 *Raster*。

![image-20260316154558116](DofExample/image-20260316154558116.png)

> 光栅化渲染器渲染效果。右侧是配置面板

点击右侧的 `Raster Settings` - `Ambient Occlusion`，选择不同的环境光遮蔽算法，查看对应效果。如果MoerEngine光栅化在你的电脑上帧数比较低，可以将环境光遮蔽算法切换为 *SSAO*。

目前光栅化渲染器，默认启用 RTAO、自动曝光 等后处理算法。你可以修改这些算法的参数，查看对应的效果。（注：至2026.3.16，SSDO、SSR这两个算法损坏，尚未修复，所以渲染效果是错误的）

## 2. 单Pass景深

我们首先实现一个最基础的单Pass景深。

### 2.1 后处理Pass

渲染管线是由非常多Pass组成的，在 `RasterRenderer.cpp` 中，可以找到如下代码：

```c++
// Post Process Passes
// - Ambient Occlusion
auto              ao_result        = ao_pass->Process(raster_context, raster_config, camera, time);
TextureWithHandle processing_image = ao_result.ao_with_color;
uint              ao_only_idx      = ao_result.ao_only_idx;

rtao_denoiser_pass->ProcessInPlace(raster_context, raster_config, ao_only_idx);

// - Denoiser Pass (Bilateral Filter)
processing_image = bfd_pass->Process(raster_context, raster_config, processing_image);

// - Screen Space Reflection
processing_image = ssr_pass->Process(raster_context, raster_config, camera, processing_image);

// - Anti-aliasing
processing_image = aa_pass->Process(raster_context, raster_config, camera, processing_image);

// - Bloom Pass
processing_image = bloom_pass->Process(raster_context, raster_config, processing_image);

// - Tonemapping Pass
processing_image = tonemapping_pass->Process(raster_context, raster_config, processing_image);
```

上述所有Pass都是 **后处理Pass**。即，这个Pass的输入是全屏像素，输出也是全屏像素，这个Pass的Shader会对像素进行一些处理。例如，`ao_pass->Process(..)` 就是Ambient Occlusion环境光遮蔽效果。

### 2.2 添加一个后处理Pass

熟悉一个引擎最好的方式，就是直接上手改。





## 3. 多Pass景深