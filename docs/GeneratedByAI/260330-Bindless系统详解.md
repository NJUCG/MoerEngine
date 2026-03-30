# MoerEngine Bindless 资源系统详解

> 本文面向引擎新手，从零开始介绍 Bindless 技术的背景、原理，以及 MoerEngine 中的具体实现。

---

## 目录

1. [传统资源绑定的困境](#1-传统资源绑定的困境)
2. [什么是 Bindless？](#2-什么是-bindless)
3. [Vulkan 中的 Bindless 扩展](#3-vulkan-中的-bindless-扩展)
4. [D3D12 中的 Bindless 支持](#4-d3d12-中的-bindless-支持)
5. [MoerEngine Bindless 架构总览](#5-moerengine-bindless-架构总览)
6. [Shader 侧实现详解](#6-shader-侧实现详解)
7. [C++ 侧实现详解](#7-c-侧实现详解)
8. [完整数据流：从 CPU 到 Shader](#8-完整数据流从-cpu-到-shader)
9. [实战：一个光照 Shader 的 Bindless 用法](#9-实战一个光照-shader-的-bindless-用法)
10. [Vulkan vs D3D12 差异对比](#10-vulkan-vs-d3d12-差异对比)
11. [总结](#11-总结)

---

## 1. 传统资源绑定的困境

在传统图形管线中，GPU 需要访问纹理、缓冲区等资源。这些资源通过**描述符（Descriptor）** 来引用——描述符就像一张"资源名片"，告诉 GPU 某个资源在显存中的位置和格式。

传统做法是：

```
// 伪代码 —— 传统绑定
BindTexture(slot=0, albedoTexture);    // 把漫反射纹理绑到槽位 0
BindTexture(slot=1, normalTexture);    // 把法线纹理绑到槽位 1
BindTexture(slot=2, roughnessTexture); // 把粗糙度纹理绑到槽位 2
DrawCall();
```

这种方式有以下问题：

| 问题 | 说明 |
|---|---|
| **槽位有限** | 每个 Shader 能绑定的资源数量有上限（通常几十个） |
| **频繁切换** | 每画一个物体就要重新绑定资源，产生大量 API 调用 |
| **不够灵活** | Shader 必须在编译时就确定使用哪些槽位 |
| **难以合批** | 不同材质使用不同纹理，阻碍了 Draw Call 合并 |

想象一个有 1000 个不同材质的场景——传统方式需要 1000 次绑定 + 1000 次 Draw Call。

---

## 2. 什么是 Bindless？

**Bindless**（无绑定）的核心思想很简单：

> 把所有资源一次性放入一个巨大的"资源池"，Shader 通过**整数索引**来访问任意资源，不再需要逐个绑定。

```
// 伪代码 —— Bindless
GlobalResourcePool = [texture0, texture1, texture2, ... texture9999];
// Shader 内部
color = GlobalResourcePool[materialData.albedoIndex].Sample(uv);
```

这样的好处是：

- **零绑定开销**：只需要把"索引号"传给 Shader，不需要频繁切换资源绑定
- **无限资源**：Shader 可以访问池中的任意资源
- **方便合批**：不同材质只是索引不同，可以在一次 Draw Call 中渲染多种材质
- **动态灵活**：材质可以在运行时任意切换引用的纹理

---

## 3. Vulkan 中的 Bindless 扩展

Vulkan 通过以下扩展来支持 Bindless：

### 3.1 Descriptor Indexing（VK_EXT_descriptor_indexing）

这是 Bindless 的基础。它允许：

- **运行时变长数组**：声明一个大小不确定的描述符数组
- **非一致索引**：用 `NonUniformResourceIndex()` 包裹索引，通知 GPU "这个索引在 Warp/Wave 内可能不同"
- **部分绑定**：数组中的某些槽位可以不填充，不会导致错误

```hlsl
// HLSL 中声明无界数组（Vulkan 需要 vk::binding 注解）
[[vk::binding(0, 2)]] Texture2D<float4> gTexture2Dfloat4__114514_bdls[];
[[vk::binding(0, 4)]] SamplerState      gsampler__114514_bdls[];

// 通过索引访问
Texture2D<float4> tex = gTexture2Dfloat4__114514_bdls[NonUniformResourceIndex(texIdx)];
SamplerState      spl = gsampler__114514_bdls[NonUniformResourceIndex(splIdx)];
float4 color = tex.Sample(spl, uv);
```

### 3.2 Descriptor Buffer（VK_EXT_descriptor_buffer）

MoerEngine 更进一步，使用了 **Descriptor Buffer 扩展**。传统 Vulkan 通过 `VkDescriptorSet` 来管理描述符，而 Descriptor Buffer 允许把描述符直接当做普通 buffer 数据来操作：

- 描述符被写成字节数据，存储在 GPU buffer 中
- 不需要 `vkAllocateDescriptorSets` / `vkUpdateDescriptorSets`
- 可以通过 `memcpy` 直接操作描述符数据

这是 MoerEngine 选择的方案，性能更高，也更灵活。

---

## 4. D3D12 中的 Bindless 支持

D3D12 对 Bindless 的支持更加"原生"：

```cpp
// D3D12 Root Signature Flag
D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED
```

设置了这两个 Flag 后，Shader 可以直接通过全局的 `ResourceDescriptorHeap` 和 `SamplerDescriptorHeap` 来索引：

```hlsl
// D3D12 HLSL —— 不需要声明任何数组，直接索引全局堆
Texture2D<float4> tex = Texture2D<float4>(ResourceDescriptorHeap[texIdx]);
SamplerState      spl = (SamplerState)SamplerDescriptorHeap[splIdx];
```

D3D12 的模型更简单——两个全局堆，直接索引。不需要额外扩展。

---

## 5. MoerEngine Bindless 架构总览

MoerEngine 的 Bindless 系统由以下几个核心部分构成：

```
┌─────────────────────────────────────────────────────────────────┐
│                        CPU (C++)                                │
│                                                                 │
│  ┌──────────────────┐     ┌──────────────────────────────────┐  │
│  │  VulkanBindless   │────▶│  VulkanDescriptorHeap             │  │
│  │  Array            │     │  (全局描述符堆)                    │  │
│  │  ├ AllocateTexture│     │  ├ image_desc_data (纹理描述符)    │  │
│  │  ├ AllocateBuffer │     │  ├ buffer_desc_data (缓冲描述符)   │  │
│  │  └ CreateUpdate   │     │  └ accel_desc_data (加速结构)      │  │
│  │    Command()      │     └──────────────────────────────────┘  │
│  └──────────────────┘                                           │
│           │                                                     │
│           │ 生成更新命令                                          │
│           ▼                                                     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  GPU Buffer (3 个)                                        │   │
│  │  ├ bindless_array_buffer  ─── 间接索引数组                 │   │
│  │  ├ bindless_texture_descs ─── 纹理描述符数据               │   │
│  │  └ bindless_buffer_descs  ─── 缓冲描述符数据               │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ Shader 读取
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      GPU (Shader/HLSL)                          │
│                                                                 │
│  g__array_114514_bdls[]      ← 间接索引数组 (StructuredBuffer)  │
│  gbuffer__114514_bdls[]      ← 缓冲描述符数组                   │
│  gTexture2D*__114514_bdls[]  ← 纹理描述符数组                   │
│  gsampler__114514_bdls[]     ← 采样器数组                       │
│                                                                 │
│  TextureHandle / ArrayBuffer ← 用户友好的封装结构体              │
└─────────────────────────────────────────────────────────────────┘
```

### 核心设计思路

MoerEngine 的 Bindless 使用了一层**间接寻址（Indirection）**：

```
Shader Handle (uint)
       │
       ▼
g__array_114514_bdls[handle]  →  packed_value (uint)
       │
       ├── 纹理: (texture_slot << 8) | sampler_idx
       │          ↓                     ↓
       │   gTexture2D[texture_slot]   gsampler[sampler_idx]
       │
       └── 缓冲: buffer_slot
                    ↓
              gbuffer[buffer_slot]
```

为什么需要间接寻址？因为它允许**热替换**：只要修改间接数组中某个位置的值，所有引用该位置的 Shader 都会自动获取新的资源，无需重新编译。

---

## 6. Shader 侧实现详解

Shader 侧代码位于 `shaders/core/common/Bindless.hlsl`。

### 6.1 资源声明：`BINDLESS_BINDINGS` 宏

每个需要使用 Bindless 的 Shader 都需要调用这个宏：

```hlsl
#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(space3, space2, space4, space5)
//                  │        │       │       │
//                  │        │       │       └─ 加速结构 descriptor set
//                  │        │       └───────── 采样器 descriptor set
//                  │        └───────────────── 纹理 descriptor set
//                  └────────────────────────── 缓冲 descriptor set
```

这个宏展开后（以 Vulkan 为例），将声明：

```hlsl
// 1. 间接索引数组 —— 所有 Handle 的入口
[[vk::binding(0, space3)]] StructuredBuffer<uint> g__array_114514_bdls;

// 2. 缓冲描述符的无界数组
[[vk::binding(1, space3)]] ByteAddressBuffer gbuffer__114514_bdls[];

// 3. 采样器的无界数组
[[vk::binding(0, space4)]] SamplerState gsampler__114514_bdls[];

// 4. 纹理描述符的无界数组（每种类型 × 每种格式）
[[vk::binding(0, space2)]] Texture2D<float4> gTexture2Dfloat4__114514_bdls[];
[[vk::binding(0, space2)]] Texture2D<float>  gTexture2Dfloat__114514_bdls[];
[[vk::binding(0, space2)]] Texture2D<uint4>  gTexture2Duint4__114514_bdls[];
// ... 还有 Texture1D, Texture3D, TextureCube 等

// 5. 加速结构（用于光线追踪）
[[vk::binding(0, space5)]] RaytracingAccelerationStructure gaccelg__114514_bdls[];

// 6. 资源访问封装结构体
// struct VKResourceDescriptorHeap { ... };
// struct ArrayBuffer { ... };
// struct TextureHandle { ... };
// struct SamplerHandle { ... };
```

> `_114514_bdls` 这个后缀是引擎内部的命名约定，用于避免与用户代码的命名冲突。

### 6.2 Handle 类型体系

Bindless.hlsl 定义了一套 Handle 类型，每种 Handle 封装一个 `uint` 索引值：

```hlsl
struct ByteBufferHandle   { uint internalIndex; };  // 缓冲
struct TlasHandle         { uint internalIndex; };  // 光追加速结构

template <typename T> struct Texture2DHandle       { uint internalIndex; }; // 纹理（取texel）
template <typename T> struct Texture2DSampleHandle { uint internalIndex; }; // 纹理（带采样器）
struct Texture2DHandleNative       { uint internalIndex; }; // 不带模板的版本
struct Texture2DSampleHandleNative { uint internalIndex; }; // 不带模板的版本

struct SamplerHeapHandle  { uint internalIndex; };  // 采样器
// ... Texture1D, Texture3D, TextureCube 等类似
```

### 6.3 资源访问流程（以纹理采样为例）

当你写下 `TextureHandle(handle).Sample2D<float4>(uv)` 时，背后发生了什么：

```hlsl
// 第 1 步：从间接数组取出打包值
uint tex_handle = g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];

// 第 2 步：解包
uint tex_idx     = tex_handle >> 8;       // 高 24 位 = 纹理在描述符堆中的索引
uint sampler_idx = tex_handle & 0xff;     // 低  8 位 = 采样器索引（最多 256 个采样器）

// 第 3 步：从全局数组中取出实际资源
Texture2D<float4> tex = gTexture2Dfloat4__114514_bdls[NonUniformResourceIndex(tex_idx)];
SamplerState      spl = gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)];

// 第 4 步：采样
return tex.Sample(spl, uv);
```

打包格式图示：

```
g__array_114514_bdls[handle] 的值（32 位 uint）：
┌───────────────────────────┬──────────┐
│  texture_slot (24 bits)   │ sampler  │
│        高 24 位            │ (8 bits) │
│  纹理描述符索引             │ 采样器索引│
└───────────────────────────┴──────────┘
```

### 6.4 用户友好的封装：`TextureHandle` 和 `ArrayBuffer`

为了让 Shader 开发者不需要手写上述流程，Bindless.hlsl 提供了两个高层封装：

```hlsl
struct TextureHandle {
    uint handle;

    // 2D 纹理采样
    template<typename T> T Sample2D(float2 uv);
    template<typename T> T SampleLevel(float2 uv, float level);
    template<typename T> T SampleGrad(float2 uv, float2 grad_x, float2 grad_y);

    // Cube 纹理采样
    template<typename T> T SampleCube(float3 uv);
    template<typename T> T SampleLevelCube(float3 uv, float level);

    // 获取底层 Texture2D 对象（用于 Load 等操作）
    template<typename T> Texture2D<T> GetTexture2D();
};

struct ArrayBuffer {
    uint handle;

    // 获取底层 ByteAddressBuffer
    ByteAddressBuffer GetByteAddressBuffer();

    // 按结构体类型 + 索引读取数据
    template<typename T> T Load(uint index);
    template<typename T> T Load(uint index, uint offset);
};

struct SamplerHandle {
    uint handle;
    SamplerState GetSampler();
};
```

### 6.5 `RenderResourceHandle`：调试友好的资源标识

还有一个辅助结构体，用于资源管理和调试：

```hlsl
struct RenderResourceHandle {
    uint index;
    // 32 位打包布局：
    // [0:22]  23 位 —— 索引（可寻址 800 万+ 资源）
    // [23:24]  2 位 —— ResourceTag（资源类型标识）
    // [25]     1 位 —— IsWritable（是否可写）
    // [26:31]  6 位 —— Version（版本号，防止 use-after-free）

    bool IsValid()    { return index != ~0; }
    uint ReadIndex()  { return index & ((1 << 23) - 1); }
    uint ResourceTag(){ return (index >> 23) & 3; }
    bool IsWritable() { return (index >> 25) & 1; }
    uint Version()    { return (index >> 26) & 0x3F; }
};
```

---

## 7. C++ 侧实现详解

### 7.1 全局描述符堆：`VulkanDescriptorHeap`

这是整个 Bindless 系统的基础设施，管理所有 GPU 描述符：

```cpp
struct VulkanDescriptorHeap {
    // 三类描述符的原始数据（CPU 端副本）
    Array<byte> buffer_desc_data;  // 缓冲描述符
    Array<byte> image_desc_data;   // 图像描述符
    Array<byte> accel_desc_data;   // 加速结构描述符

    // 空闲索引回收列表
    Array<uint> buffer_free_list;
    Array<uint> image_free_list;
    Array<uint> accel_free_list;

    // 核心接口
    uint GetBufferDescIdx(const BufferView& _in_buffer, ...); // 分配缓冲描述符
    uint GetImageDescIdx(const TextureView* _in_image, ...);  // 分配图像描述符
    uint GetSamplerDescIdx(Sampler _sampler);                  // 分配采样器描述符
    uint GetAccelDescIdx(VulkanAccelerationStructure* _as);    // 分配加速结构描述符
};
```

它利用了 `VK_EXT_descriptor_buffer`，把描述符当作字节数据来管理，通过 `memcpy` 操作来写入和复制描述符。

### 7.2 Bindless 数组：`VulkanBindlessArray`

这是 CPU 端的核心类，管理间接索引数组和资源分配：

```cpp
class VulkanBindlessArray : public BindlessArray {
public:
    // ----- 资源类型 -----
    enum EType : uint { Texture, Buffer };

    // ----- 内部 Handle 结构 -----
    struct Handle {
        uint ptr_1;         // 资源指针高 32 位
        uint ptr_2;         // 资源指针低 32 位
        uint slot   : 22;  // 描述符堆中的槽位
        uint attrib : 8;   // 属性（mip level 等）
        uint type   : 2;   // 类型：Texture(0) 或 Buffer(1)
    };

    // ----- 核心接口 -----
    uint AllocateTexture(const TextureView& _texture, Sampler _sampler);
    uint AllocateBuffer(BufferView _buffer);
    void UnbindTexture(uint _slot);
    void UnbindBuffer(uint _slot);

    // ----- GPU 缓冲 -----
    VulkanBuffer* bindless_array_buffer;    // 间接索引数组 → Shader 的 g__array_114514_bdls
    VulkanBuffer* bindless_texture_descs;   // 纹理描述符 → Shader 的 gTexture2D*__114514_bdls
    VulkanBuffer* bindless_buffer_descs;    // 缓冲描述符 → Shader 的 gbuffer__114514_bdls

    // ----- 内部管理 -----
    Array<Handle>        handles;           // 所有已分配的 Handle
    Array<UpdateCmd>     update_cmds;       // 待执行的更新命令队列
    LockFreeQueue<uint>  free_slots;        // 空闲的间接数组槽位
    LockFreeQueue<uint>  free_texture_slots;// 空闲的纹理描述符槽位
    LockFreeQueue<uint>  free_buffer_slots; // 空闲的缓冲描述符槽位
};
```

### 7.3 资源分配流程

以 `AllocateTexture` 为例：

```cpp
uint VulkanBindlessArray::AllocateTexture(const TextureView& _texture, Sampler _sampler) {
    // 第 1 步: 从空闲列表或原子计数器获取一个间接数组槽位
    uint slot_idx = free_slots.Pop();
    if (slot_idx == 0) slot_idx = slot_offset++;

    // 第 2 步: 获取一个纹理描述符槽位
    uint texture_slot = free_texture_slots.Pop();
    if (texture_slot == 0) texture_slot = texture_slot_offset++;

    // 第 3 步: 推入更新命令（延迟执行）
    update_cmds.emplace_back(TextureUpdateInfo{
        _texture.texture, _sampler, _texture.format,
        slot_idx, texture_slot,
        _texture.mip_level, _texture.num_mips,
        _texture.array_layer, _texture.num_array,
        false  // free=false 表示这是分配，不是释放
    });

    // 第 4 步: 返回间接数组索引给调用者
    return slot_idx;  // 这就是 Shader 中 TextureHandle.handle 的值
}
```

调用者获得 `slot_idx` 后，将它作为 `uint` 传入 Shader 的常量缓冲区或 Push Constant。

### 7.4 帧同步更新：`CreateUpdateCommand()`

真正的描述符写入发生在帧开始时，由 `CreateUpdateCommand()` 执行：

```cpp
UniquePtr<Command> VulkanBindlessArray::CreateUpdateCommand() {
    VulkanDescriptorHeap& heap = m_device->GetGlobalDescriptorHeap();

    for (const UpdateCmd& cmd : update_cmds) {
        if (is TextureUpdateInfo && !free) {
            // 1. 从全局描述符堆获取纹理描述符数据
            uint src_idx = heap.GetImageDescIdx(&view, layout);

            // 2. 复制描述符字节数据到上传缓冲
            memcpy(texture_dat + offset, &heap.image_desc_data[src_idx], stride);

            // 3. 构造间接数组的值：(texture_slot << 8) | sampler_idx
            uint indirect_handle = (sampler_idx & 0xff) | (texture_slot & 0xffffff) << 8;
            memcpy(array_dat + offset, &indirect_handle, sizeof(uint));

            // 4. 记录 Handle 信息用于后续释放
            handles[array_idx] = Handle(ptr, slot, attrib, Texture);
        }
        else if (is BufferUpdateInfo && !free) {
            // 缓冲区类似，但间接数组直接存储 buffer_slot
            uint src_idx = heap.GetBufferDescIdx(view, type);
            memcpy(buffer_dat + offset, &heap.buffer_desc_data[src_idx], stride);
            memcpy(array_dat + offset, &buffer_slot, sizeof(uint));
        }
        else if (free) {
            // 释放：回收槽位到空闲列表
            free_slots.Push(array_idx);
            free_texture_slots.Push(slot);
        }
    }

    // 返回一个 Command 对象，在 GPU 时间线上执行 buffer 更新
    return MakeUnique<UpdateBindlessArrayCmd>(...);
}
```

### 7.5 资源释放与版本管理

```cpp
void VulkanBindlessArray::UnbindTexture(uint _array_idx) {
    // 检查是否有 pending 的更新命令
    if (auto iter = temp_slot_to_cmd.find(_array_idx); iter != end) {
        // 如果资源还没真正写入 GPU，直接取消该命令
        update_cmds[iter->second] = InvalidUpdateInfo{_array_idx};
    } else {
        // 资源已在 GPU 上，推入释放命令
        update_cmds.emplace_back(TextureUpdateInfo{nullptr, ..., true/*free=true*/});
    }
}

// 帧结束时，真正回收槽位
void VulkanBindlessArray::OnFree(const Array<uint>& slots, ...) {
    for (uint idx : slots) free_slots.Push(idx);
    for (uint idx : textures) free_texture_slots.Push(idx);
    for (uint idx : buffers) free_buffer_slots.Push(idx);
}
```

释放是延迟的——必须等 GPU 不再使用该资源后才能回收槽位。

---

## 8. 完整数据流：从 CPU 到 Shader

下面用一个完整的例子，追踪一张纹理从"C++ 注册"到"Shader 采样"的完整路径：

```
                          C++ 侧
                          ══════
Step 1: 用户调用
    uint handle = bindlessArray->AllocateTexture(albedoView, linearSampler);
    // 返回: handle = 42 (间接数组中的槽位)

Step 2: 将 handle 写入 Push Constant / Constant Buffer
    pushConstant.albedo_handle = 42;

                          帧同步
                          ══════
Step 3: CreateUpdateCommand() 执行
    ├── 从全局堆分配纹理描述符 → texture_slot = 7
    ├── 查询采样器索引 → sampler_idx = 3
    ├── 写入间接数组:
    │     g__array_114514_bdls[42] = (7 << 8) | 3 = 0x00000703
    ├── 写入纹理描述符堆:
    │     bindless_texture_descs[7] = <albedo 纹理的描述符字节>
    └── GPU Buffer Upload

                          Shader 侧
                          ═════════
Step 4: Shader 代码
    TextureHandle tex = TextureHandle(param.albedo_handle);  // handle = 42
    float4 color = tex.Sample2D<float4>(uv);

Step 5: 展开后的实际执行
    uint packed = g__array_114514_bdls[42];       // = 0x00000703
    uint tex_idx     = packed >> 8;               // = 7
    uint sampler_idx = packed & 0xFF;             // = 3
    Texture2D<float4> texObj = gTexture2Dfloat4_bdls[7];
    SamplerState      spl    = gsampler_bdls[3];
    return texObj.Sample(spl, uv);                // → 最终颜色
```

---

## 9. 实战：一个光照 Shader 的 Bindless 用法

以 `RasterLightingPass.frag.hlsl` 为例，展示 Bindless 在实际 Shader 中的使用模式：

```hlsl
#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)  // 声明所有 Bindless 全局资源

// Push Constant 中只有 uint handle，不需要直接绑定任何纹理
[[vk::push_constant]] ConstantBuffer<MaterialPassBindlessParam> param;

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {

    // -------- 通过 handle 访问纹理 --------
    // 从 VBuffer 纹理中采样材质 ID
    uint material_id = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);

    // 从 GBuffer 纹理中采样几何信息
    float2 uv     = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    float  depth  = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);
    float3 normal = TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv);

    // -------- 通过 handle 访问缓冲区 --------
    // 加载材质数据
    ArrayBuffer material_buf = ArrayBuffer(param.material_buf_hdl);
    GMaterial mat = material_buf.Load<GMaterial>(material_id);

    // 加载光源数据
    ArrayBuffer light_buf = ArrayBuffer(param.light_buf_hdl);
    for (uint i = 0; i < light_count; i++) {
        GLight light = light_buf.Load<GLight>(i);
        // ... 光照计算
    }

    // -------- 材质纹理也是 Bindless 的 --------
    // mat.albedo_map_hdl 也是一个 uint handle
    float3 albedo = TextureHandle(mat.albedo_map_hdl).Sample2D<float3>(uv);

    // ... PBR 计算
    return float4(finalColor, 1.0);
}
```

注意看：整个 Shader 中**没有任何** `Texture2D myTexture : register(t0)` 这样的传统绑定。所有资源都通过 `uint handle` 索引访问。Push Constant 只需要传递几个 `uint` 值。

---

## 10. Vulkan vs D3D12 差异对比

MoerEngine 使用条件编译 (`#if VULKAN` / `#elif DXIL`) 来处理两个 API 的差异：

| 维度 | Vulkan | D3D12 |
|---|---|---|
| **描述符管理** | Descriptor Buffer 扩展，描述符存储为字节数据 | 系统内置 `ResourceDescriptorHeap` |
| **纹理数组声明** | 每种类型 × 格式一个无界数组 `Texture2D<float4> arr[];` | 不需要声明，直接 `ResourceDescriptorHeap[idx]` |
| **采样器声明** | `SamplerState gsampler[];` 无界数组 | `SamplerDescriptorHeap[idx]` |
| **缓冲声明** | `ByteAddressBuffer gbuffer[];` 无界数组 | `ResourceDescriptorHeap[idx]` |
| **绑定注解** | `[[vk::binding(binding, set)]]` | Root Signature Flag 控制 |
| **描述符堆封装** | `VKResourceDescriptorHeap` 静态结构体 | `DXResourceDescriptorHeapAccessor` 静态结构体 |
| **写索引（UAV）** | `WriteIndex() = ReadIndex()` (同一个描述符) | `WriteIndex() = ReadIndex() + 1` (写描述符在读描述符之后) |

尽管底层不同，但上层 API（`TextureHandle`, `ArrayBuffer`）是完全统一的——Shader 开发者不需要关心底层是 Vulkan 还是 D3D12。

---

## 11. 总结

MoerEngine 的 Bindless 系统可以总结为以下要点：

1. **一层间接寻址**：`g__array_114514_bdls` 间接数组是系统的核心，所有资源访问都从这里开始
2. **打包索引**：纹理的间接值中打包了纹理描述符索引（24 位）和采样器索引（8 位）
3. **延迟更新**：CPU 端的分配是立即返回的，但描述符的实际写入是延迟到帧开始的 `CreateUpdateCommand()` 中执行
4. **槽位回收**：使用无锁队列管理空闲槽位，支持安全的延迟回收
5. **跨 API 统一**：Shader 层提供的 `TextureHandle` / `ArrayBuffer` 接口对 Vulkan 和 D3D12 完全一致
6. **宏生成**：使用大量 HLSL 宏来自动生成所有纹理类型 × 格式的访问函数，避免手写重复代码

这套系统让 MoerEngine 能够在一次 Draw Call 中渲染使用不同纹理的多种材质，极大减少了 CPU 端的绑定开销，是现代高性能渲染引擎的标配架构。
