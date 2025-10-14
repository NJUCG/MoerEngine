# MoerEngine DOC

### Tree Structure

```
  .
  ├─LICENSE
  ├─source
  |   ├─editor
  |   ├─runtime
  |   |    ├─core
  |   |    ├─render
  |   |    ├─resource
  |   ├─cuda
  |   ├─configs
  |   ├─test
  ├─doc
  ├─cmake
  ├─asset
```

# Deprecated ↓

## Current Capability

- Render Hardware Interface(vulkan implemented)
- Multi-Thread TaskSystem
- Math
- RHI implemented ui backend
- Deferred gui rendering
- Shader pipeline(compiling, reflection and binding)

## TODO

### Critical

- GPUScene management
- Camera Manager
- Material System design and implementation
- Bindless support
  
### Pending

- Scene Management
- Offline mesh clusterize
- Default pipeline
- DX12 support
- dxc and metis on linux

## Project Structure Notes

- 写一些关于项目结构的注释（后面应该要移到 `doc/` 里面去）

### CMakeLists

- 所有library（通过add_library添加的东西）
  - MoerRuntime、MoerCore、MoerRender、MoerResource
  - 其中，MoerRuntime被其他四个库public链接了，同时，每个库都public include了很多头文件目录
    - 所以，MoerEditor只链接了MoerRuntime，并且不需要添加runtime相关的头文件目录
- 所有executable（通过add_executable添加的东西）
  - MoerEditor、source/test文件夹下的一大团东西

### MoerEditor

- 2025.3.13左右，重构了一下MoerEditor

- **为什么新Editor和Runtime耦合在一起？**

  - 不独立开写的原因是：① 目前需要UI来在Raster和Raytracing两种渲染模式之间切换，而这个切换信息是需要UI来选择的；② 目前想不到什么不需要UI独立运行的场景
  - 独立开写，在技术上也是可以实现的。目前耦合性其实不高

- **如何实现的切换Raster和Raytracing两种渲染方法**？

  - 切换时，只保留特定对象，其他数据（包括场景）直接全部重新初始化
    - 注：场景会直接重新读取SceneCache
  - 具体内容参见 `source/editor/Editor.cpp`，代码很短

- **为什么用namespace独立开 Raster和Raytracing代码？**

  - 注：Moer::Render::Raster、Moer::Render::Raytracing
  - 原因 ①：这两个部分都是接近程序入口的代码，十分末端，不应该被其他模块再调用。所以独立成namespace是不影响后续开发的（希望如此）
  - 原因 ②：LightingPass和一些Pass会重名，导致编译错误

- **为什么把原来代码直接废弃了？**

  - ~~因为改不动，太难了~~
  - 因为重构时，能跑的两个程序入口RHITest和RaytracingTest，和原有的MoerEditor结构差距太大。并且Engine.cpp/h也没用。秉承着化繁为简的思路，我就直接把旧的、不可用的程序入口废弃了。现在代码入口也更清晰明了。

- **为什么把 .h 和 .cpp 放在同一个目录下？**

  - ~~我懒了~~
  - 我其实不太了解为什么.h和.cpp要分开存放。我的理解里，只有做基础库的时候，才需要这么做？为了用户调用方便。.h和.cpp塞在同一个目录下，开发起来会舒服一些。

- **目前MoerEditor的目录结构（只包含目录）**

  ```
  editor
  - common
  - raster
  - raytracing
  - ui
    - raster_ui
    - raytracing_ui
  ```

  - 其中，`common`、`raster`、`raytracing` 分别是两个渲染方法的入口和公共代码
  - 其中，`ui` 储存了所有ui相关的代码
    - 每个渲染方法单独的UI（以下称为SubUI），以 *组合* 的形式嵌入主UI
    - 每个SubUI只需要实现
      - struct Config —— 数据
      - void ShowConfig() —— 操作数据的UI
    - 在主UI中，会public抛出SubUI的config，并调用SubUI的ShowConfig()

- TODO

  - editor模块中，残留了非常多的TODO和FIXME，需要解决

  - 两个渲染方法，仍有许多代码可以合并，需要合并一下

  - 在切换渲染方法时，保留摄像机数据

    - 实现方法一：在主UI（EditorUI）中加对应数据段
    - 实现方法二：直接修改场景数据（缓存文件）；切换渲染方法时，会直接重新从缓存中加载场景数据

  - RHITest的normal，看了下，貌似解压有问题，色调不对

  - Raytracing的NRD貌似有一点小问题

    - 第一个是 `NRD_INTEGRATION_ASSERT(isNormalRoughnessFormatValid, "IN_NORMAL_ROUGHNESS format doesn't match NRD normal encoding");` 这个assert会被触发

    - 第二个是，重置屏幕大小后，再切换渲染方法，m_FrameIndex的数值貌似不对。可能是调用问题，我现在硬编码直接修改了

# Deprecated（更远古） ↓

### Render Module

- seperate thread from main thread
- implement RHI to support Vulkan and DX12
  - low level rhi, like Vulkan and DX12
  
- use cross compiler(currently DXC) to compile HLSL for DX12 and Vulkan

#### RHI

##### Vulkan overview
![Alt text](Vulkan.png)

##### RHI Example(experimental)

```cpp

class TestShader : public Shader {
    DEFINE_SHADER_TYPE(TestShader, Global, RENDER_API)
public:
    BEGIN_SHADER_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_UAV(RWTexture2D, write_target)
    DEFINE_SHADER_PARAM_ATTACHMENT_BINDING()
    END_SHADER_PARAMETER_DEFINITION(Parameters)
};
//use macro to register global shader
IMPLEMENT_SHADER_TYPE(TestShader, "testFile.vert", "main", EShaderType::ST_VERTEX)

void Example(){
    const uint16_t      indices[] = {1, 2, 3};
    RHIBufferCreateInfo buffer_info;
    buffer_info.SetUsage(EBufferUsageFlags::INDEX_BUFFER)
        .SetStride(sizeof(uint16_t))
        .SetSize(sizeof(indices));

    RHIBufferRef indexBuffer = CreateBufferFromData(buffer_info, sizeof(indices), (void*)indices);

    RHIBufferCreateInfo v_info;
    v_info.SetSize(16).SetStride(4).SetUsage(EBufferUsageFlags::VERTEX_BUFFER);

    const float  vertex_data[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 1};
    RHIBufferRef vertex_buffer = CreateBufferFromData(v_info, sizeof(vertex_data), (void*)vertex_data);

    

    RHIGraphicsPipelineStateInfo init;
    init.num_samples                 = 1;
    init.multi_view_count            = 1;
    init.stencil_attachment_store_op = EAttachmentStoreOp::NONE;
    init.stencil_attachment_load_op  = EAttachmentLoadOp::NONE;
    init.depth_attachment_store_op   = EAttachmentStoreOp::NONE;
    init.depth_attachment_load_op    = EAttachmentLoadOp::NONE;
    init.color_attachment_count      = 1;
    init.color_attachment_formats[0] = EPixelFormat::PF_R8G8B8A8_SRGB;
    init.color_attachment_flags[0]   = ETextureUsageFlags::COLOR_ATTACHMENT;
    init.primitive_topology          = EPrimitiveTopology::TRIANGLE_LIST;

    RHIShaderBoundStateInput& shader_state = init.shader_stage;
    shader_state.p_vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_init_list);
    // and fragment shader
    ShaderRef<TestShader> test_shader = ShaderResourceManager::GetShader<TestShader>();
    RHIVertexShader vert = test_shader->GetRHIVertexShader();

    shader_state.vertex_shader = vert;

    VertexInputStateInitializerList vertex_init_list;
    for (int i = 0; i < 1; ++i) {
        vertex_init_list[i].type            = EVertexElementType::VET_Float3;
        vertex_init_list[i].offset          = 0;
        vertex_init_list[i].stride          = sizeof(Moer::Vector3f);
        vertex_init_list[i].input_rate      = EVertexInputRate::VIR_VERTEX;
        vertex_init_list[i].attribute_index = 0;
        vertex_init_list[i].binding_index   = 0;
    }

    const Moer::Vector2i attachment_size(4, 4);
    RHITextureCreateInfo tex_info;
    tex_info.SetDimension(ETextureDimension::TEX_2D)
        .SetFormat(PF_R8G8B8A8_SRGB)
        .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT)
        .SetExtent(attachment_size)
        .SetClearAttachment(RHIClearAttachment())
        .SetDepth(1)
        .SetArraySize(1)
        .SetNumMips(1)
        .SetNumSamples(1)
        .SetUsageFlags(ETextureUsageFlags::COLOR_ATTACHMENT);

    //out_put texture
    RHITextureRef tex = g_rhi->RHICreateTexture(tex_info);

    RHIGraphicsPipelineStateRef pso = g_rhi->RHICreateGraphicsPipelineState(init);


    RHIUAVRef test_view =
        g_rhi->RHICreateUnorderedAccessView(tex,
                                            RHIViewInfo::CreateTextureUAVInfo()
                                                .SetFormat((PF_R8G8B8A8_SRGB)));
    
    //parameter binding
    TestShader::Parameters* params = ShaderParamAllocator<TestShader>::Alloc();
    params->write_target = test_view;


    RHIGraphicsCommandList* command_list = g_rhi->CreateGraphicsCommandList(pso);
    
    command_list->BeginRecording();                                       // 1. start command 
    RHIRenderPassInfo pass_info;
    pass_info.GeneratePipelineAttachmentInfo();
    command_list->BeginRenderPass(pass_info, "triangle pass");  // 2. begin rendering
    command_list->BindParams(params);                           // 3. bind shader params
    command_list->SetPipelineState(pso, shader_state);          // 4. bind pipeline and decriptor set
    command_list->BindVertexBuffers(0, 1, vertex_buffer);       // 5. bind vertex buffers
    command_list->DrawIndexedInstanced(1, 1, 0, 0);             // 6. draw call
    command_list->EndRenderPass();                              // 7. end rendering
    command_list->EndRecording();                                      // 8. end command

    RHICommandQueue*                         graphics_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
    const std::array<RHICommandListBase*, 1> _command_array{command_list};
    graphics_queue->SubmitCommands(1, _command_array.data());   // 9. submit commands
    
}
```


### Core

#### TaskGraph
For CPU parallelism. Ported from UE4(simpler job schedular than UE5), supporting fewer threads(up to 23 worker threads for now).

![Alt text](TaskGraph.png)

##### Example
```cpp
LOG_INFO("=============== task is not executed until its prerequisite is completed success ================");
    {// a task is not executed until all its prerequisites are completed
        bool            bExecuted = false;
        GraphEventArray Prereqs{GraphEvent::CreateGraphEvent(), GraphEvent::CreateGraphEvent()};
        GraphEventRef   MainTask = FunctionGraphTask::ConstructAndDispatchWhenReady([&bExecuted] { bExecuted = true; }, &Prereqs);
        // dummy task that is executed while the main task is waiting for its prereqs
        FunctionGraphTask::ConstructAndDispatchWhenReady([] {})->Wait();
        assert(!bExecuted);

        Prereqs[0]->TryUnlockSubsequents();
        FunctionGraphTask::ConstructAndDispatchWhenReady([] {})->Wait();
        assert(!bExecuted);

        Prereqs[1]->TryUnlockSubsequents();
        MainTask->Wait();
        assert(bExecuted);
    }

```