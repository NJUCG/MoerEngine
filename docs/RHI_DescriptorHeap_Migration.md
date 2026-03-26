# RHI Descriptor Heap 迁移设计文档

## 概述

本文档覆盖两个关联工作：

1. **问题一：`BindDescriptors` 线程安全修复** — 将 `VulkanDescriptorHeap` 的 TLS offset 改为真正的原子 ring 分配，修复 parallel translate 阶段的竞态。
2. **问题二：`VK_EXT_descriptor_heap` 迁移** — 用 Vulkan 1.4.340 引入的新扩展替换 `VK_EXT_descriptor_buffer`，与 HLSL SM6.6 `ResourceDescriptorHeap` 语义对齐。

两者可独立实施，问题一是问题二的前置准备（heap 模式同样需要 lock-free slot 分配）。

遵循 [Rule_Codex.md](Rule_Codex.md)：不兜底、不过度设计、基于 Test 验证。

---

## 第一部分：`BindDescriptors` 线程安全修复

### 现有实现的完整分析

#### 当前 `VulkanDescriptorHeap` 的 offset 机制（`VulkanDescriptor.cpp:354–427`）

**实际情况与之前假设不同**：当前实现用的是 **TLS（Thread-Local Storage）**，不是共享 offset：

```cpp
// VulkanDescriptor.cpp:32
namespace {
thread_local uint64 g_tls_descriptor_offset = 0;
}

// GetCurrentOffset()：返回当前线程的 TLS 值
uint64 VulkanDescriptorHeap::GetCurrentOffset() const {
    return g_tls_descriptor_offset;
}

// BeginPushDescriptors()：把本帧该线程的起始 offset 写入 TLS
void VulkanDescriptorHeap::BeginPushDescriptors(uint _frame_idx) {
    _frame_idx = _frame_idx % m_device->cmd_alloc_limits;
    g_tls_descriptor_offset = ring_buffer_offsets[_frame_idx];  // 从 per-frame 分区起点开始
    current_offset = g_tls_descriptor_offset;
}

// IncrementOffset()：只更新 TLS，不更新共享 current_offset（除了赋值记录）
void VulkanDescriptorHeap::IncrementOffset(uint64 _size) {
    g_tls_descriptor_offset += _size;
    current_offset = g_tls_descriptor_offset;  // 写回 current_offset（非原子）
}
```

**关键结构体字段**（`VulkanDescriptor.h:88–160`）：

```cpp
struct VulkanDescriptorHeap {
    // ...
    std::mutex m_mutex;        // 仅用于 GetBufferDescIdx/GetImageDescIdx/GetAccelDescIdx 的 free-list 保护
    uint64 current_offset;     // 非原子，是 g_tls_descriptor_offset 的镜像（无实际并发保护作用）
    uint ring_buffer_cnt;
    uint64 ring_buffer_size;
    Array<uint64> ring_buffer_offsets;  // per-frame 起始 offset 数组（cmd_alloc_limits 个槽位）
    uint8* map_ptr;
};
```

**`BindDescriptors` 调用现场**（`VulkanCommandList.cpp:581–783`）：

```cpp
void VulkanCmdList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {
    VulkanDescriptorHeap& descriptor_heap = device.GetGlobalDescriptorHeap();
    uint64 g_global_desc_offset = descriptor_heap.GetCurrentOffset(); // 读 TLS

    // ... 遍历 set_binders ...
    [&](const VulkanDescriptorSetBinder& _binder) {
        // Push*Desc 函数均使用 g_tls_descriptor_offset + 相对 offset 写入 ring buffer
        descriptor_heap.PushImageDesc(src_handle, _binder.binding_infos[i].offset);

        // 用 TLS 当前值作为 offset，写入 desc_buffer_offsets
        bind_template.desc_buffer_offsets[_binder.offset_idx].offset =
            descriptor_heap.GetCurrentOffset();  // 读 TLS
        descriptor_heap.IncrementOffset(_binder.size); // TLS += size
    }
}
```

**Push*Desc 函数实现**（以 `PushImageDesc` 为例，`VulkanDescriptor.cpp:397–403`）：

```cpp
void VulkanDescriptorHeap::PushImageDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);  // 这里的 mutex 只保护 memcpy
    memcpy(
        map_ptr + g_tls_descriptor_offset + _set_offset,  // 写入地址 = 本线程 TLS offset + 相对 offset
        image_desc_data.data() + _src_offset,
        image_desc_stride
    );
}
```

#### 真正的问题所在

TLS 机制本身是线程隔离的，但问题在于 **`BeginPushDescriptors` 被何时调用、由谁调用**：

- `BeginPushDescriptors(frame_idx)` 将 TLS 重置为 `ring_buffer_offsets[frame_idx]` —— 即该帧的分区起点
- `ring_buffer_offsets[frame_idx]` 是编译期固定的等分 offset（`VulkanDescriptor.cpp:160–162`）：
  ```cpp
  ring_buffer_offsets[i] = AlignUp(buffer_ci.size / m_device->cmd_alloc_limits * i, alignment);
  ```
- **多个并发 translate 线程如果共用同一个 frame_idx，它们的 TLS 起点是相同的** → 从同一 offset 开始增长 → 写入位置冲突

**并发竞态场景**：
```
Thread A (translate task 0): BeginPushDescriptors(frame=0) → TLS = ring_offsets[0] = 0
Thread B (translate task 1): BeginPushDescriptors(frame=0) → TLS = ring_offsets[0] = 0
Thread A: IncrementOffset(512) → TLS_A = 512
Thread B: IncrementOffset(256) → TLS_B = 256
→ A 和 B 写入了重叠的 [0, 512] 和 [0, 256] 范围
```

**此外**，`current_offset` 字段（非原子）被多线程写入（每次 `IncrementOffset` 都写），存在 data race，是 UB。

#### 修复方案：共享原子游标 + 单次 `fetch_add` 分配 range

**核心思路**：废弃 TLS 机制，改用一个**全帧共享的 `std::atomic<uint64>`**，每个 binder 用 `fetch_add` 原子地分配自己所需的区间，分配后独立写入，不需要锁。

#### 修改点 1：`VulkanDescriptor.h` — 替换 offset 字段

```cpp
// 修改前
std::mutex m_mutex;
uint64     texture_desc_offset;
uint64     current_offset;    // 非原子，镜像 TLS

// 修改后
std::mutex m_mutex;           // 保留，继续用于 free-list（GetBufferDescIdx 等）
uint64     texture_desc_offset;
// 删除 current_offset
std::atomic<uint64> ring_head{0};  // 新增：帧内共享原子游标

// 新增接口，删除旧接口：
uint64 AllocateRange(uint64 size) noexcept;  // atomic fetch_add，返回 base_offset
void   ResetFrame(uint _frame_idx);           // 帧末重置游标至该帧分区起点

// 删除：
// uint64 GetCurrentOffset() const;
// void   IncrementOffset(uint64 _size);
// void   BeginPushDescriptors(uint _frame_idx);  // 替换为 ResetFrame
// void   EndPushDescriptors(uint _frame_idx);    // 见下方分析
```

#### 修改点 2：`VulkanDescriptor.cpp` — 实现新接口

```cpp
// 删除
thread_local uint64 g_tls_descriptor_offset = 0;  // 整个 TLS 变量删除

// 新增
uint64 VulkanDescriptorHeap::AllocateRange(uint64 size) noexcept {
    return ring_head.fetch_add(size, std::memory_order_relaxed);
}

void VulkanDescriptorHeap::ResetFrame(uint _frame_idx) {
    _frame_idx = _frame_idx % m_device->cmd_alloc_limits;
    ring_head.store(ring_buffer_offsets[_frame_idx], std::memory_order_release);
}
// 注：EndPushDescriptors 的 vmaFlushAllocation 调用需要保留，
//     但起止 offset 改为记录 AllocateRange 前后的值（见 BindDescriptors 修改）
```

**Push*Desc 函数**：去掉 `m_mutex` 锁（`memcpy` 写入各自独立的 range，不再需要串行化）：

```cpp
// 修改前（以 PushImageDesc 为例）
void VulkanDescriptorHeap::PushImageDesc(uint64 _src_offset, uint64 _set_offset) {
    std::lock_guard<std::mutex> lock(m_mutex);
    memcpy(map_ptr + g_tls_descriptor_offset + _set_offset, ...);
}

// 修改后：_base_offset 由调用方 AllocateRange 返回，_set_offset 是相对 offset
void VulkanDescriptorHeap::PushImageDesc(uint64 _base_offset, uint64 _src_offset, uint64 _set_offset) {
    // 无锁：各线程写入各自 AllocateRange 分配的非重叠区间
    memcpy(map_ptr + _base_offset + _set_offset, image_desc_data.data() + _src_offset, image_desc_stride);
}
// 同理修改 PushUniformDesc / PushStorageDesc / PushSamplerDesc / PushAccelDesc
```

#### 修改点 3：`VulkanCommandList.cpp` — `BindDescriptors`

```cpp
void VulkanCmdList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {
    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(_pso_handle.handle);
    VulkanPipelineParamBinder& bind_template = *vk_pso->bind_template;
    VulkanDescriptorHeap& descriptor_heap = device.GetGlobalDescriptorHeap();
    auto& set_binders = bind_template.set_binders;
    // 删除：uint64 g_global_desc_offset = descriptor_heap.GetCurrentOffset();

    for (auto& [set, binder] : set_binders) {
        std::visit(Overload{
            // bindless 分支不变（不使用 ring_desc_buffer）
            [&](const VulkanBindlessSetArray& _binder) { /* 不变 */ },
            [&](const VulkanBindlessSetSampler& _binder) { /* 不变 */ },
            [&](const VulkanBindlessSetImage& _binder) { /* 不变 */ },

            [&](const VulkanDescriptorSetBinder& _binder) {
                // 一次性原子分配整个 binder 所需区间
                uint64 base_offset = descriptor_heap.AllocateRange(_binder.size);

                for (uint i = 0; i < _binder.writers.size(); ++i) {
                    // ...（所有 Push*Desc 调用额外传 base_offset）
                    switch (writer.descriptorType) {
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                            descriptor_heap.PushImageDesc(
                                base_offset,
                                src_handle,
                                _binder.binding_infos[i].offset  // 相对 base_offset 的 set 内偏移
                            );
                            break;
                        // ... 其他 case 同理 ...
                    }
                }

                // base_offset 就是该 binder 的起点，直接使用
                bind_template.desc_buffer_offsets[_binder.offset_idx].offset = base_offset;
                // 删除：descriptor_heap.IncrementOffset(_binder.size);
            }
        }, binder);
    }

    // vkCmdBindDescriptorBuffersEXT / vkCmdSetDescriptorBufferOffsetsEXT 不变
    // push constants 不变
}
```

#### 修改点 4：`EndPushDescriptors` 的处理

`EndPushDescriptors` 调用 `vmaFlushAllocation` 来 flush 本帧写入。改造后：
- 需要知道"本帧写入范围 `[frame_start, frame_end)`"
- `frame_start = ring_buffer_offsets[frame_idx]`（已知）
- `frame_end` = 帧结束时 `ring_head.load(std::memory_order_acquire)`

```cpp
// 调用位置：帧结束时（interrupt 线程或 submit 完成回调）
void VulkanDescriptorHeap::FlushFrame(uint _frame_idx) {
    _frame_idx = _frame_idx % m_device->cmd_alloc_limits;
    uint64 base_offset = ring_buffer_offsets[_frame_idx];
    uint64 end_offset  = ring_head.load(std::memory_order_acquire);
    vmaFlushAllocation(
        m_device->GetVmaAllocator(),
        ring_desc_buffer->GetAllocation(),
        base_offset,
        end_offset - base_offset
    );
}

// ResetFrame 在 flush 之后调用
void VulkanDescriptorHeap::ResetFrame(uint _frame_idx) {
    _frame_idx = _frame_idx % m_device->cmd_alloc_limits;
    ring_head.store(ring_buffer_offsets[_frame_idx], std::memory_order_release);
}
```

#### 修改点 5：`ResetFrame` 接入点（interrupt 线程）

```cpp
// RHIExecutor / VulkanDevice interrupt thread，帧结束时：
device.GetGlobalDescriptorHeap().FlushFrame(current_frame_idx);
device.GetGlobalDescriptorHeap().ResetFrame(next_frame_idx);
```

#### `m_mutex` 的保留与去除

| 操作 | 修改前 | 修改后 |
|---|---|---|
| `GetBufferDescIdx` | 持有 `m_mutex` | **保留**（free-list 不是原子结构） |
| `GetImageDescIdx` | 持有 `m_mutex` | **保留** |
| `GetAccelDescIdx` | 持有 `m_mutex` | **保留** |
| `FreeBufferDescIdx` | 持有 `m_mutex` | **保留** |
| `Push*Desc` (memcpy) | 持有 `m_mutex` | **去掉**（各线程写入不重叠区间） |

#### 一致性与正确性分析

1. **`fetch_add` 无 ABA**：ring buffer 每帧 reset，`ResetFrame` 在上一帧所有 translate task 完成、cmd submit 之后调用（interrupt 线程保证），不存在 ABA。
2. **ring buffer 溢出**：当帧内描述符总量超过 `ring_buffer_offsets[next] - ring_buffer_offsets[current]` 时会写入下一帧区域（现有 TLS 机制同样有此风险）。暂不兜底，靠合理容量配置。
3. **`binding_infos[i].offset` 语义不变**：该字段是 descriptor set layout 内的相对 offset（由 `vkGetDescriptorSetLayoutBindingOffsetEXT` 填充，`VulkanRHIResource.cpp:1447`），与 ring buffer 绝对 offset 无关，逻辑正确。

---

## 第二部分：`VK_EXT_descriptor_heap` 迁移

### 扩展基本信息

| 项目 | 内容 |
|---|---|
| 扩展名 | `VK_EXT_descriptor_heap` |
| 引入版本 | Vulkan 1.4.340（2026-01-23） |
| Spec 文档 | https://github.khronos.org/Vulkan-Site/spec/latest/chapters/descriptorheaps.html |
| 状态 | 正式发布，NVIDIA/AMD 主流驱动已支持 |
| 与 descriptor_buffer 关系 | 后继替代，两者在同一 command buffer 内**互斥** |

### 核心模型差异

`VK_EXT_descriptor_heap` **不引入新的 Vulkan 对象**。Heap 就是普通 GPU-visible buffer，通过 device address 绑定到 command buffer。

与现有 `VK_EXT_descriptor_buffer` 的对比：

| 维度 | 现有 `descriptor_buffer` | 目标 `descriptor_heap` |
|---|---|---|
| Descriptor 写入 | `vkGetDescriptorEXT` → 写入 `buffer_desc_data` / `image_desc_data`（CPU 侧 staging）→ `PushImageDesc` memcpy 到 ring_desc_buffer | `vkWriteResourceDescriptorsEXT(device, count, pResources, pDescriptors)` → 直接写入 heap buffer 的 host 地址 |
| Descriptor 查询 | `vkGetDescriptorSetLayoutBindingOffsetEXT` + `vkGetDescriptorSetLayoutSizeEXT` 确定 set layout | `vkGetPhysicalDeviceDescriptorSizeEXT` 查询每种 type 的 descriptor 字节大小 |
| CMD 绑定 | `vkCmdBindDescriptorBuffersEXT(N, pBindingInfos)` + `vkCmdSetDescriptorBufferOffsetsEXT(set, bufIdx, offset)` | `vkCmdBindSamplerHeapEXT(cmd, pBindInfo)` + `vkCmdBindResourceHeapEXT(cmd, pBindInfo)`（分开，各绑定一次） |
| Shader 寻址 | set+binding → offset（`_set_offset` 相对 ring base） | flat `uint32` index（`ResourceDescriptorHeap[index]`） |
| Pipeline layout | 必须包含 `VkDescriptorSetLayout`（`_DESCRIPTOR_BUFFER_BIT_EXT`） | 可选（SPIR-V set+binding → heap offset 映射通过 `VkShaderDescriptorSetAndBindingMappingInfoEXT` pNext） |
| 现有 `VulkanPipelineParamBinder` | 必须（`desc_buffers`, `desc_buffer_offsets`） | 不再需要 `desc_buffers`/`desc_buffer_offsets`；改为 slot index + push data |
| 现有 `bind_template->set_binders` | 必须（`VulkanDescriptorSetBinder` 含 binding_infos, size 等） | 可大幅简化 |
| `Bindless.hlsl` | `[[vk::binding(N, S)]]` 数组 + `vkResourceDescriptorHeap` wrapper struct | 与 DXIL 路径相同：`ResourceDescriptorHeap[idx]` / `SamplerDescriptorHeap[idx]`，需 `-fvk-use-dx-layout` |

### 完整 API 清单（来自 Spec）

#### 查询 Descriptor 大小

```cpp
// 每种类型查询一次，存入属性结构体
VkDeviceSize vkGetPhysicalDeviceDescriptorSizeEXT(
    VkPhysicalDevice physicalDevice,
    VkDescriptorType descriptorType
);
// 对比现有：
// descriptor_buffer_properties.storageBufferDescriptorSize 等字段
// → 功能等价，但 heap 模式 per-type 查询，不通过 properties 结构体
```

#### 写 Descriptor（CPU 侧）

```cpp
// 写 sampler descriptor 到 host 地址（mapped buffer 内某 slot）
VkResult vkWriteSamplerDescriptorsEXT(
    VkDevice                     device,
    uint32_t                     samplerCount,
    const VkSamplerCreateInfo*   pSamplers,    // sampler 创建信息
    const VkHostAddressRangeEXT* pDescriptors  // 目标 host 地址数组（mapped buffer 内）
);

// 写 resource descriptor（image / buffer / AS）到 host 地址
VkResult vkWriteResourceDescriptorsEXT(
    VkDevice                           device,
    uint32_t                           resourceCount,
    const VkResourceDescriptorInfoEXT* pResources,   // 资源信息数组
    const VkHostAddressRangeEXT*       pDescriptors  // 目标 host 地址数组
);
```

#### 关键结构体

```cpp
// 资源 descriptor 信息（对比现有 VkDescriptorGetInfoEXT）
typedef struct VkResourceDescriptorInfoEXT {
    VkStructureType             sType;  // VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT
    const void*                 pNext;
    VkDescriptorType            type;   // SAMPLED_IMAGE / STORAGE_IMAGE / UNIFORM_BUFFER / STORAGE_BUFFER / AS ...
    VkResourceDescriptorDataEXT data;   // union，根据 type 选择字段
} VkResourceDescriptorInfoEXT;

typedef union VkResourceDescriptorDataEXT {
    const VkImageDescriptorInfoEXT*       pImage;        // for image types
    const VkTexelBufferDescriptorInfoEXT* pTexelBuffer;
    const VkDeviceAddressRangeEXT*        pAddressRange; // for buffer / AS
} VkResourceDescriptorDataEXT;

typedef struct VkImageDescriptorInfoEXT {
    VkStructureType              sType;
    const void*                  pNext;
    const VkImageViewCreateInfo* pView;  // 注意：传创建信息，不是已有 VkImageView
    VkImageLayout                layout;
} VkImageDescriptorInfoEXT;

// 目标地址范围（mapped buffer 内某 slot 的 host 地址）
typedef struct VkHostAddressRangeEXT {
    void*       pAddress;
    VkDeviceSize size;
} VkHostAddressRangeEXT;

// Heap 绑定信息（sampler heap 和 resource heap 各一个）
typedef struct VkBindHeapInfoEXT {
    VkStructureType         sType;  // VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT
    const void*             pNext;
    VkDeviceAddressRangeEXT heapRange;           // buffer device address + 可访问大小
    VkDeviceSize            reservedRangeOffset; // 驱动内部保留区偏移（query 获取）
    VkDeviceSize            reservedRangeSize;
} VkBindHeapInfoEXT;
```

#### CMD 绑定

```cpp
// 分开绑定两个 heap（替代 vkCmdBindDescriptorBuffersEXT + vkCmdSetDescriptorBufferOffsetsEXT）
void vkCmdBindSamplerHeapEXT(VkCommandBuffer cmd, const VkBindHeapInfoEXT* pBindInfo);
void vkCmdBindResourceHeapEXT(VkCommandBuffer cmd, const VkBindHeapInfoEXT* pBindInfo);
```

#### SPIR-V binding 映射（兼容现有 shader，无需重写 SPIR-V）

```cpp
// 挂在 VkPipelineShaderStageCreateInfo 的 pNext，将 SPIR-V set+binding → heap offset
typedef struct VkShaderDescriptorSetAndBindingMappingInfoEXT {
    VkStructureType                            sType;
    const void*                                pNext;
    uint32_t                                   mappingCount;
    const VkDescriptorSetAndBindingMappingEXT* pMappings;
} VkShaderDescriptorSetAndBindingMappingInfoEXT;

typedef struct VkDescriptorSetAndBindingMappingEXT {
    VkStructureType                  sType;
    const void*                      pNext;
    uint32_t                         descriptorSet;
    uint32_t                         firstBinding;
    uint32_t                         bindingCount;
    VkSpirvResourceTypeFlagsEXT      resourceMask;
    VkDescriptorMappingSourceEXT     source;
    VkDescriptorMappingSourceDataEXT sourceData;
} VkDescriptorSetAndBindingMappingEXT;

typedef enum VkDescriptorMappingSourceEXT {
    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT      = 0,
    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT           = 1,
    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT       = 2,
    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT = 3,
    VK_DESCRIPTOR_MAPPING_SOURCE_RESOURCE_HEAP_DATA_EXT             = 4,
    VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT                      = 5,
    VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT                   = 6,
    VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT               = 7,
    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_SHADER_RECORD_INDEX_EXT  = 8,
    VK_DESCRIPTOR_MAPPING_SOURCE_SHADER_RECORD_DATA_EXT             = 9,
    VK_DESCRIPTOR_MAPPING_SOURCE_SHADER_RECORD_ADDRESS_EXT          = 10,
} VkDescriptorMappingSourceEXT;
```

### 各文件修改点详细分析

#### `VulkanDeviceProperty.h` — 新增字段

```cpp
// 当前（仅有 descriptor_buffer）
VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_properties;

// 修改后：新增
VkPhysicalDeviceDescriptorHeapPropertiesEXT   descriptor_heap_properties;  // 新增
VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_properties; // 保留
```

`VkPhysicalDeviceDescriptorHeapPropertiesEXT` 字段（查 spec）包含 `maxResourceDescriptorHeapSize`、`maxSamplerDescriptorHeapSize`、`reservedSamplerDescriptorHeapSize`、`reservedResourceDescriptorHeapSize` 等。

#### `VulkanExtension.h` — 新增字段

```cpp
class VulkanOptionalDeviceExtensions final {
public:
    bool m_has_ext_descriptor_heap;    // 新增（descriptor_heap 优先）
    bool m_has_ext_descriptor_buffer;  // 保留（fallback）
    bool m_has_khr_acceleration_structure;
    // ... 其余不变
};
```

#### `VulkanExtension.cpp` — 新增扩展类

在 `VulkanEXTDescriptorBufferExtension`（L176–212）之后、`VulkanKHRPushDescriptorExtension`（L214）之前插入：

```cpp
// ***** VK_EXT_descriptor_heap
class VulkanEXTDescriptorHeapExtension final : public VulkanDeviceExtension {
public:
    VulkanEXTDescriptorHeapExtension(bool _is_optional = true) :
        VulkanDeviceExtension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, _is_optional),
        m_features{} {}

    void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) override {
        m_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        AddToPNext(_gpu_features2, m_features);
    }

    void PostGpuFeatures(VulkanOptionalDeviceExtensions& _ext) override {
        m_is_usable = (m_features.descriptorHeap == VK_TRUE);
        _ext.m_has_ext_descriptor_heap = m_is_usable;
    }

    void PreGpuProperties(const VulkanDevice* _device, VkPhysicalDeviceProperties2& _gpu_properties2) override {
        auto& heap_props =
            const_cast<VulkanOptionalDeviceProperties&>(_device->GetOptionalProperties())
                .descriptor_heap_properties;
        heap_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
        AddToPNext(_gpu_properties2, heap_props);
    }

    void PreCreateDevice(VkDeviceCreateInfo& _ci) override {
        if (m_is_usable && m_is_enabled)
            AddToPNext(_ci, m_features);
    }

private:
    VkPhysicalDeviceDescriptorHeapFeaturesEXT m_features;
};
```

在 `GetMERequiredDeviceExtensions()` 中：

```cpp
// 现有：
ADD_CUSTOM_EXTENSION(VulkanEXTDescriptorBufferExtension, VULKAN_EXTENSION_OPTIONAL);

// 修改后（descriptor_heap 优先注册，optional）：
ADD_CUSTOM_EXTENSION(VulkanEXTDescriptorHeapExtension,  VULKAN_EXTENSION_OPTIONAL);
ADD_CUSTOM_EXTENSION(VulkanEXTDescriptorBufferExtension, VULKAN_EXTENSION_OPTIONAL);
```

#### `VulkanDescriptor.h` — 新增 `VulkanDescriptorHeapNative` 类

在现有 `VulkanDescriptorHeap` 之后（L160 之后）、`#pragma endregion` 之前添加新类：

```cpp
#pragma region[ descriptor heap ext (VK_EXT_descriptor_heap) ]

struct VulkanDescriptorHeapNative {
    VulkanBuffer* sampler_heap   = nullptr;
    VulkanBuffer* resource_heap  = nullptr;
    void*         sampler_ptr    = nullptr;  // persistent mapped
    void*         resource_ptr   = nullptr;

    VkDeviceSize sampler_desc_size  = 0;
    VkDeviceSize image_desc_size    = 0;
    VkDeviceSize storage_image_desc_size = 0;
    VkDeviceSize buffer_desc_size   = 0;  // storage buffer（max across types）
    VkDeviceSize accel_desc_size    = 0;

    std::atomic<uint32_t> sampler_slot{0};
    std::atomic<uint32_t> resource_slot{0};

    VulkanDevice* m_device = nullptr;

    void Init(VulkanDevice& device);
    void Destroy();

    // 写入 sampler，返回 slot index（shader 用 SamplerDescriptorHeap[slot]）
    uint32_t WriteSampler(VkSampler sampler);

    // 写入 resource，返回 slot index（shader 用 ResourceDescriptorHeap[slot]）
    uint32_t WriteImage(VkImageView view, VkImageLayout layout, VkDescriptorType type);
    uint32_t WriteBuffer(VkDeviceAddress addr, VkDeviceSize range, VkDescriptorType type);
    uint32_t WriteAccelStruct(VkDeviceAddress addr);

    // 在 cmd buffer 开头绑定（每个 cmd buffer 开头调用一次）
    void CmdBind(VkCommandBuffer cmd) const;

    void ResetFrame();  // 帧末调用（单线程，interrupt 线程）
};

#pragma endregion
```

**注意**：`VkImageDescriptorInfoEXT.pView` 要求传 `VkImageViewCreateInfo*`（创建信息，非句柄）。这意味着 `WriteImage` 需要保存创建信息或从 `VulkanTexture` 中获取，**不能直接传已有 `VkImageView`**。这与现有 `GetImageDescIdx` 使用 `VkDescriptorImageInfo{imageView, imageLayout}` 的方式不同，需要在 `VulkanTexture` 中补充保存 `VkImageViewCreateInfo`。

#### `VulkanDevice.h` — 新增 accessor

```cpp
// 现有
VulkanDescriptorHeap& GetGlobalDescriptorHeap() { return m_global_descriptor_heap; }

// 新增
VulkanDescriptorHeapNative& GetDescriptorHeapNative() { return m_descriptor_heap_native; }

// private:
VulkanDescriptorHeap       m_global_descriptor_heap{};
VulkanDescriptorHeapNative m_descriptor_heap_native{};  // 新增
```

#### `VulkanRHIResource.h` — `VulkanPipelineParamBinder` 分析

```cpp
// 现有
struct VulkanPipelineParamBinder {
    UnorderedMap<uint, TBinder>            set_binders;
    VkPushConstantsInfoKHR                 push_constants_info;
    Array<VkDescriptorBufferBindingInfoEXT> desc_buffers;        // heap 模式不需要
    Array<DescBufferOffsetInfo>            desc_buffer_offsets;  // heap 模式不需要
};
```

heap 模式下 `desc_buffers` 和 `desc_buffer_offsets` 不使用（heap 通过 device address 绑定，不需要 per-draw offset）。`set_binders` 在 heap 模式下也不需要详细的 `VulkanDescriptorSetBinder`（binding_infos, size 等），但为了保留 fallback 路径的完整性，暂不改动结构体，在 heap 分支的 `InitPipelineLayout` 中不填充这两个数组。

#### `VulkanRHIResource.cpp` — `InitPipelineLayout` 修改

当前 `InitPipelineLayout`（L1239–1556）完整逻辑：
1. **Pass 1**（L1259–1300）：遍历 `_descriptor_set_layouts`，按 bindless/非bindless 类型分配 `descriptor_buffer_count`，确定各 idx
2. **Pass 2**（L1331–1456）：创建 `VkDescriptorSetLayout`（带 `_DESCRIPTOR_BUFFER_BIT_EXT`），调用 `vkGetDescriptorSetLayoutBindingOffsetEXT`/`vkGetDescriptorSetLayoutSizeEXT` 填充 `binding_infos`/`size`
3. **Post-build**（L1469–1538）：填充 `descriptor_buffers[]`（`VkDescriptorBufferBindingInfoEXT`）和 `desc_buffer_offsets`

heap 模式分支：在函数开头检测 `m_has_ext_descriptor_heap`，若为 true 则：
- **跳过** Pass 1 / Pass 2（不创建 `VkDescriptorSetLayout`，不调用 `vkGetDescriptorSetLayoutSizeEXT`）
- Pipeline layout 不含 descriptor set（`setLayoutCount = 0`）
- `bind_template` 初始化为空（`desc_buffers` 和 `desc_buffer_offsets` 保持空）
- `set_binders` 按 param_idx 记录 binding 参数（用于 `BindDescriptors` 知道哪个 arg 对应哪个 binding），但不记录 offset/size

具体位置：在 `InitPipelineLayout` 函数开头（L1239 之后）插入：

```cpp
void VulkanPipelineState::InitPipelineLayout(...) {
    if (m_device->GetOptionalExtensions().m_has_ext_descriptor_heap) {
        // heap 模式：简化 pipeline layout
        bind_template = MakeUnique<VulkanPipelineParamBinder>();

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (_push_constant_range.has_value()) {
            ci.pushConstantRangeCount = 1;
            ci.pPushConstantRanges    = &_push_constant_range.value();
        }
        VK_CHECK_RESULT(vkCreatePipelineLayout(m_device->GetDevice(), &ci, nullptr, &m_pipeline_layout));

        // 只记录 param_idx 映射，供 BindDescriptors heap 分支使用
        for (auto& [set, layout] : _descriptor_set_layouts) {
            auto& binder = bind_template->set_binders[set];
            for (auto& [binding_idx, m_binding] : layout.bindings) {
                // 简化记录（不需要 binding_infos/size/offset，只需 param_idx）
                // 用现有 VulkanDescriptorSetBinder 的 bind_infos 仅记录 param_idx
                VulkanDescriptorSetBinder& resource_binder =
                    std::get<VulkanDescriptorSetBinder>(binder);
                resource_binder.bind_infos.resize(
                    std::max((size_t)(binding_idx + 1), resource_binder.bind_infos.size())
                );
                resource_binder.bind_infos[binding_idx].param_idx = m_binding.param_idx;
                resource_binder.writers.resize(resource_binder.bind_infos.size());
                resource_binder.writers[binding_idx].descriptorType = layout[binding_idx].binding.descriptorType;
                resource_binder.writers[binding_idx].descriptorCount = layout[binding_idx].binding.descriptorCount;
            }
        }
        return;
    }

    // fallback：原有 descriptor_buffer 路径（不变）
    // ... 现有 L1240–1556 代码 ...
}
```

#### `VulkanCommandList.cpp` — `BindDescriptors` heap 分支

在 `BindDescriptors` 函数最前面（L581 之后）插入分支：

```cpp
void VulkanCmdList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {
    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(_pso_handle.handle);
    assert(vk_pso && vk_pso->bind_template != nullptr);
    VulkanPipelineParamBinder& bind_template = *vk_pso->bind_template;
    auto& set_binders = bind_template.set_binders;

    if (device.GetOptionalExtensions().m_has_ext_descriptor_heap) {
        // heap 模式：为每个 binding 写入 heap，通过 push constants 传 slot index 给 shader
        VulkanDescriptorHeapNative& heap = device.GetDescriptorHeapNative();

        for (auto& [set, binder] : set_binders) {
            // 仅处理 VulkanDescriptorSetBinder（bindless 在 heap 模式下走不同路径，待定）
            if (auto* res_binder = std::get_if<VulkanDescriptorSetBinder>(&binder)) {
                for (uint i = 0; i < res_binder->writers.size(); ++i) {
                    auto& writer = res_binder->writers[i];
                    if (writer.descriptorCount < 1) continue;

                    uint param_idx = res_binder->bind_infos[i].param_idx;
                    if (!(_pso_handle.valid_bits & (1 << param_idx))) continue;

                    uint32_t slot = 0;
                    switch (writer.descriptorType) {
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
                            TextureView& view = std::get<TextureView>(_args[param_idx]);
                            VkImageLayout layout = GetSamplerImageLayout(view);
                            VulkanTexture* tex = ResourceCast(view.texture);
                            slot = heap.WriteImage(tex->GetView(view.mip_level, view.num_mips), layout, writer.descriptorType);
                            break;
                        }
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                            BufferView& buf_view = std::get<BufferView>(_args[param_idx]);
                            VulkanBuffer* buf = ResourceCast(buf_view.GetBuffer());
                            slot = heap.WriteBuffer(buf->DeviceAddress() + buf_view.byte_offset, buf_view.GetByteSize(), writer.descriptorType);
                            break;
                        }
                        // ... 其余 case ...
                    }
                    // slot 需要传给 shader（通过 push constants 或 push data）
                    // 具体接口待 VkPushDataInfoEXT 完整 spec 确认后实现
                }
            }
        }

        // heap 绑定（每个 cmd buffer 开头已调用 CmdBind，draw 时无需再次绑定）
        // push constants 不变（若有）
        if (bind_template.push_constants_info.size > 0) {
            bind_template.push_constants_info.pValues = _args.constants.data();
            const auto& push_info = &bind_template.push_constants_info;
            vkCmdPushConstants(command_buffer, push_info->layout, push_info->stageFlags,
                               push_info->offset, push_info->size, push_info->pValues);
        }
        return;
    }

    // fallback: descriptor_buffer 路径（使用问题一修复后的 AllocateRange）
    VulkanDescriptorHeap& descriptor_heap = device.GetGlobalDescriptorHeap();
    // ... 修复后的 BindDescriptors 代码 ...
}
```

#### `Bindless.hlsl` — 修改点分析

**现有 Vulkan 路径**（L460–521）使用：
- `[[vk::binding(N, S)]]` 数组声明（`g__array_114514_bdls`、`gbuffer__114514_bdls[]`、`gsampler__114514_bdls[]`、`gTexture*__114514_bdls[]`）
- `VKResourceDescriptorHeap` wrapper struct（通过 `g_tls_descriptor_offset` 的 bindless array 间接寻址）
- `ACCESS_GLOBAL_TEXTURE_HEAP` 宏展开为 `TextureType<NativeType>(gTexture*NativeType*__114514_bdls[idx])`

**heap 模式需要的 Vulkan 路径**：
- 与 DXIL 路径（L523–563）完全相同：`ResourceDescriptorHeap[idx]`、`SamplerDescriptorHeap[idx]`
- 只需在 `#if VULKAN` 内部加子条件：

```hlsl
// 修改后（在 #if VULKAN 块内，L460 处）：

#if VULKAN

#if VULKAN_DESCRIPTOR_HEAP
// VK_EXT_descriptor_heap 路径（与 DXIL 语义相同，需 DXC -fvk-use-dx-layout 编译）
#define ACCESS_GLOBAL_TEXTURE_HEAP(NativeType, TextureType, idx)      TextureType<NativeType>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)])
#define ACCESS_GLOBAL_TEXTURE_HEAP_WITHOUT_TEMPLATE(TextureType, idx) TextureType(ResourceDescriptorHeap[NonUniformResourceIndex(idx)])
#define ACCESS_GLOBAL_SAMPLER_HEAP(idx)                               (SamplerState)SamplerDescriptorHeap[NonUniformResourceIndex(idx)]
#define ACCESS_GLOBAL_BUFFER_HEAP(idx)                                ByteAddressBuffer(ResourceDescriptorHeap[NonUniformResourceIndex(idx)])

// heap 模式不需要 [[vk::binding]] 数组声明，不需要 VKResourceDescriptorHeap wrapper
// BINDLESS_BINDINGS 宏只保留 HANDLES 部分（struct ArrayBuffer, TextureHandle, SamplerHandle 定义）
// 注意：g__array_114514_bdls（bindless handle array）在 heap 模式下仍需要，因为 HANDLES 宏内部引用它
#define BINDLESS_BINDINGS(BufferSpace, TextureSpace, SamplerSpace, AccelSpace)        \
  [[vk::binding(0, BufferSpace)]] StructuredBuffer<uint> g__array_114514_bdls;        \
  DX_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH, INNER_GENERATE_BUFFER_FETCH) \
  HANDLES(DESCRIPTOR_HEAP, DESCRIPTOR_HEAP_SAMPLE, DESCRIPTOR_HEAP_SAMPLE_LEVEL,     \
          DESCRIPTOR_HEAP_SAMPLE_GRAD, DESCRIPTOR_HEAP_SAMPLE_CUBE,                   \
          DESCRIPTOR_HEAP_SAMPLE_LEVEL_CUBE, DESCRIPTOR_HEAP_SAMPLE_GRAD_CUBE)
// 注：此处 DX_DESCRIPTOR_HEAP 宏需要重命名或新建同名 VK_DESCRIPTOR_HEAP_NATIVE 宏，
//     因为 DX_DESCRIPTOR_HEAP 展开的 struct 名是 DXResourceDescriptorHeapAccessor，
//     需要与 DESCRIPTOR_HEAP 宏配合，需仔细对齐

#else // !VULKAN_DESCRIPTOR_HEAP（现有 descriptor_buffer 路径）
// ... 现有 L462–521 代码不变 ...
#endif // VULKAN_DESCRIPTOR_HEAP

#elif DXIL
// ... 现有 L523–563 代码不变 ...
#endif
```

**实际上 `Bindless.hlsl` 修改的核心问题**：现有 `DESCRIPTOR_HEAP` 宏（`vkResourceDescriptorHeap[handle]`）和 DXIL 的 `DESCRIPTOR_HEAP` 宏（`dxResourceDescriptorHeapAccessor[handle]`）都引用不同的 struct 实例。heap 模式下，Vulkan 需要引用 `ResourceDescriptorHeap` 本身（内置），与 DXIL 路径一致，因此 `DESCRIPTOR_HEAP` 宏对应的 accessor struct 需要变为 `dxResourceDescriptorHeapAccessor` 的等价物——这可以通过将 DXIL 的 `DX_DESCRIPTOR_HEAP` 宏抽出到 common 区域来复用。

### 实施步骤

#### Step 1：SDK 版本门控

```cmake
# runtime/render 的 CMakeLists.txt
find_package(Vulkan REQUIRED)

if(Vulkan_VERSION VERSION_GREATER_EQUAL "1.4.340")
    message(STATUS "Vulkan SDK >= 1.4.340: VK_EXT_descriptor_heap support available")
    target_compile_definitions(Moer::Render PUBLIC MOER_HAS_DESCRIPTOR_HEAP_SUPPORT=1)
else()
    message(STATUS "Vulkan SDK < 1.4.340: using descriptor_buffer fallback")
    target_compile_definitions(Moer::Render PUBLIC MOER_HAS_DESCRIPTOR_HEAP_SUPPORT=0)
endif()

# Shader 编译选项
if(MOER_USE_DESCRIPTOR_HEAP)
    list(APPEND DXC_FLAGS "-DVULKAN_DESCRIPTOR_HEAP=1" "-fvk-use-dx-layout")
else()
    list(APPEND DXC_FLAGS "-DVULKAN_DESCRIPTOR_HEAP=0")
endif()
```

#### Step 2：`VulkanDeviceProperty.h` 新增字段

在 `VulkanOptionalDeviceProperties` 的 `descriptor_buffer_properties` 之后添加：
```cpp
VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptor_heap_properties;
```

#### Step 3：`VulkanExtension.h` 新增字段

在 `m_has_ext_descriptor_buffer` 之前添加：
```cpp
bool m_has_ext_descriptor_heap;
```

#### Step 4：`VulkanExtension.cpp` 新增扩展类

在 `VulkanEXTDescriptorBufferExtension` 之后插入 `VulkanEXTDescriptorHeapExtension`（见上方代码）。

`GetMERequiredDeviceExtensions()` 中先注册 heap（optional），再注册 buffer（optional）。

#### Step 5：`VulkanDescriptor.h/cpp` 新增 `VulkanDescriptorHeapNative`

声明在 `VulkanDescriptor.h`，实现在 `VulkanDescriptor.cpp`（或新建 `VulkanDescriptorHeapNative.cpp`）。

**注意事项**：
- `vkWriteResourceDescriptorsEXT` 的 `pResources.data.pImage` 指向 `VkImageDescriptorInfoEXT`，其 `pView` 指向 `VkImageViewCreateInfo*`，需要在 `VulkanTexture` 中保存创建信息
- 或者查询 `vkGetImageViewCreateInfo` (若驱动支持) 获取已有 view 的创建信息

#### Step 6：`VulkanDevice.h/cpp` 新增 heap native 成员

#### Step 7：`InitPipelineLayout` heap 分支

#### Step 8：`BindDescriptors` heap 分支

**未决问题**：heap 模式下 slot index 如何传给 shader：
- 方案 A：`vkCmdPushConstants` 传 `uint32_t slot_indices[]` 数组
- 方案 B：`vkCmdPushDataEXT`（需 spec 确认签名，查 `vulkan/vulkan_ext.h`）
- 方案 C：per-draw uniform buffer 传 indices

推荐方案 A（与现有 push constants 机制一致，改动最小）。

#### Step 9：`Bindless.hlsl` 新增 `VULKAN_DESCRIPTOR_HEAP` 路径

---

## 第三部分：测试计划

### 测试前提

- Vulkan Validation Layer 开启（`VK_LAYER_KHRONOS_validation`）
- 关注 **error** 级别，warning/info 不作为 blocker

### CMake 配置

```bash
# 问题一修复（descriptor_buffer 路径，不依赖新 SDK）
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j16 --target TestRHITranslate

# 问题二（需要 Vulkan SDK >= 1.4.340）
cmake -B build_heap -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DMOER_USE_DESCRIPTOR_HEAP=ON
cmake --build build_heap -j16 --target TestRHITranslate

# 完整构建（含 MoerEditor）
cmake --build build_heap -j16
```

验证日志：

```
-- Vulkan SDK >= 1.4.340: VK_EXT_descriptor_heap support available
```

### Level 1：单元级 — `TestRHITranslate`

**目标**：验证问题一（线程安全 ring）和基本 heap 绑定。

```bash
./target/bin/Debug/TestRHITranslate.exe
```

**需覆盖的用例**（在 `rhi_translate_multiqueue_test.cpp` 中添加）：

| 用例 | 验证点 |
|---|---|
| `DescriptorRingConcurrent` | 4 线程并发 `AllocateRange`，验证 `base_offset` 不重叠（区间两两不交） |
| `DescriptorRingReset` | 帧末 `FlushFrame`+`ResetFrame` 后，下帧 `AllocateRange` 从正确起点开始 |
| `HeapBindSamplerResource` | `vkCmdBindSamplerHeapEXT` + `vkCmdBindResourceHeapEXT` 无 validation error |
| `HeapWriteImageDescriptor` | `vkWriteResourceDescriptorsEXT` 写 SampledImage，compute dispatch 读取正确 |
| `HeapWriteSamplerDescriptor` | `vkWriteSamplerDescriptorsEXT` 写 sampler，采样结果无黑块 |
| `HeapWriteBufferDescriptor` | `vkWriteResourceDescriptorsEXT` 写 StorageBuffer，compute shader 读写正确 |
| `HeapFallbackDescriptorBuffer` | 驱动不支持 heap 时走 descriptor_buffer 路径，无 crash |

验收标准：exit code 0，Validation 无 error。

### Level 2：渲染集成 — `TestVertexFactory`

**目标**：验证 heap 模式下 bindless 基础渲染链路。

```bash
./target/bin/Debug/TestVertexFactory.exe
```

**验证点**：几何 pass 正常渲染，纹理采样正确，Validation 无 error。

### Level 3：功能完整 — `TestRHITranslate`（MultiQueue 场景）

**目标**：验证 parallel translate + multi-queue 端到端正确性。

| 用例 | 验证点 |
|---|---|
| `CopyScopeUploadToCompute` | CopyScope upload → Compute dispatch 读 bindless buffer |
| `CopyScopeUploadToGraphicsSample` | CopyScope upload → Graphics 纹理采样 → Present |
| `ParallelTranslateDescriptorNoConflict` | 8 个 CommandList 并行 translate，`AllocateRange` 区间互不重叠 |
| `BindlessArrayUpdateBarrier` | `UpdateBindlessArray` → draw，验证三个内部 buffer 的 barrier 正确 |

验收标准：CPU readback 数值与 CPU 参考一致，Validation 无 error。

### Level 4：完整渲染器 — `MoerEditor`

```bash
./target/bin/Debug/MoerEditor.exe
```

**验收步骤**：
1. 启动 MoerEditor，加载默认场景
2. 观察渲染画面：几何、光照、阴影、后处理均正常
3. 触发 scene copy（上传 mesh/texture）
4. 切换光栅化 / 光追渲染器（若启用）
5. 等待约 30 秒，观察无崩溃、无花屏
6. 关闭编辑器，查看日志：Validation 无 error

**验收标准**：
- 画面与 descriptor_buffer 路径视觉一致（无回归）
- GPU 帧时间无明显劣化（±5% 以内）
- 日志 `[ERROR]` 级别零输出

---

## 风险与约束

| 风险 | 说明 | 缓解 |
|---|---|---|
| 驱动覆盖率 | descriptor_heap 为 2026 新扩展，中低端移动 GPU 暂不支持 | `m_has_ext_descriptor_heap` 运行时检测，自动 fallback |
| 两扩展互斥 | 同一 cmd buffer 内不能混用 heap 和 descriptor_buffer 命令 | device 初始化时选定分支，全程不混用 |
| `VkImageDescriptorInfoEXT.pView` | 传的是 `VkImageViewCreateInfo*`（创建信息），不是已有 `VkImageView` | 需在 `VulkanTexture` 中补充保存 view 创建信息，或查询 `vkGetImageViewCreateInfo` |
| `vkCmdPushDataEXT` 细节 | `VkPushDataInfoEXT` 完整签名需 SDK 头文件确认 | 实施 Step 8 前查阅 `vulkan/vulkan_ext.h`，或改用 push constants |
| SDK 版本要求 | 需 Vulkan SDK 1.4.340+；问题一与 SDK 版本无关 | 问题一可先合并；问题二待 SDK 升级后启用 |
| `BeginPushDescriptors` 调用时机 | 改造后需确认 parallel translate 前 `ResetFrame` 已被调用 | interrupt 线程保证帧序；translate 开始前 reset 当帧 |
| Ring buffer 容量 | `ring_buffer_offsets` 按 `buffer_ci.size / cmd_alloc_limits` 等分，并发写超出分区会越界 | 当前 buffer size `= s_queue_max_frame_in_flight * 256 * 16 * 100`，实测是否够用 |

---

## 实施顺序

```
Phase 1（不依赖新 SDK）
  └── 问题一：VulkanDescriptorHeap 原子 ring
       VulkanDescriptor.h: 删除 current_offset/GetCurrentOffset/IncrementOffset/BeginPushDescriptors
                           新增 ring_head(atomic), AllocateRange, FlushFrame, ResetFrame
       VulkanDescriptor.cpp: 删除 g_tls_descriptor_offset
                              Push*Desc 增加 _base_offset 参数，去掉 m_mutex
                              实现 AllocateRange/FlushFrame/ResetFrame
       VulkanCommandList.cpp: BindDescriptors 用 AllocateRange，Push*Desc 传 base_offset
       interrupt 线程: 接入 FlushFrame + ResetFrame
       验证：Level 1 (DescriptorRingConcurrent / DescriptorRingReset)

Phase 2（升级 Vulkan SDK 至 1.4.340+）
  └── Step 1: CMake 版本检测 + 宏定义
  └── Step 2-4: VulkanDeviceProperty.h / VulkanExtension.h/cpp 新增扩展类和字段

Phase 3
  └── Step 5-6: VulkanDescriptorHeapNative 实现（Descriptor.h/cpp）
               VulkanDevice.h/cpp 新增 accessor
  └── 验证：Level 1 (HeapBind* / HeapWrite*)

Phase 4
  └── Step 7-9: InitPipelineLayout heap 分支
                BindDescriptors heap 分支
                Bindless.hlsl VULKAN_DESCRIPTOR_HEAP 路径 + DXC 编译选项
  └── 验证：Level 2 → Level 3 → Level 4
```
