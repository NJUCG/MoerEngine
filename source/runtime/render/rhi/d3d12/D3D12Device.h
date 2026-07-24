#ifndef D3D12_DEVICE_H
#define D3D12_DEVICE_H

#include "misc/STL.h"
#include "taskgraph/Event.h"

#include "../RHIImpl.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include "D3D12Macro.h"

#define COM_NO_WINDOWS_H
#include <d3d12.h>
#include <d3dx12/d3dx12.h>
#include <wrl/client.h>

#include "Variant.h"
#include <D3D12MemAlloc.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <optional>

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>; // maybe use custom comptr?

namespace Moer::Render {

inline std::wstring StringWiden(std::string_view utf8str) {
    if (utf8str.empty())
        return std::wstring();
    int          cbMultiByte = static_cast<int>(utf8str.size());
    int          req         = ::MultiByteToWideChar(CP_UTF8, 0, utf8str.data(), cbMultiByte, nullptr, 0);
    std::wstring res(req, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8str.data(), cbMultiByte, &res[0], req);
    return res;
}

inline std::string StringNarrow(std::wstring_view wstr) {
    if (wstr.empty())
        return std::string();
    int cbMultiByte = static_cast<int>(wstr.size());
    int req = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), cbMultiByte, nullptr, 0, nullptr, nullptr);
    std::string res(req, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), cbMultiByte, &res[0], req, nullptr, nullptr);
    return res;
}

inline uint64_t AlignUpToPowerOfTwo(uint64_t value, uint64_t alignment) {
    const uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

inline uint64_t AlignConstantBuffer(uint64_t value) {
    return AlignUpToPowerOfTwo(value, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
}
inline uint64_t AlignResourceByDefaultPlacement(uint64_t value) {
    return AlignUpToPowerOfTwo(value, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
}
inline uint64_t AlignTextureResource(uint64_t value, bool msaa = false, bool _small = false) {
    auto alignment = 0;
    if (msaa) {
        if (_small) {
            alignment = D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
        } else {
            alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
        }
    } else {
        if (_small) {
            alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
        } else {
            alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        }
    }
    return AlignUpToPowerOfTwo(value, alignment);
}
//#define D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512) // useful when copy
//#define D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (512) // useful when copy
//#define D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT (4096)  // not want to use uavcounter...

//#define D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT (8)
//#define D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT (256)
//#define D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT (16)
//#define D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT (32)
//#define D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT (64)
//#define D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT (16)

namespace D3D12EnumTranslation {
//static VkIndexType           METoVKIndexType(EIndexElementType _type);

//static VkFormat              METoVKFormat(EPixelFormat _format);
DXGI_FORMAT METoDxFormat(EPixelFormat _format);
//static VkImageType           METoVKImageType(ETextureDimension _dim);
D3D12_RESOURCE_DIMENSION METoDxResourceDimension(ETextureDimension _dim);
//static VkImageUsageFlags     METoVKImageUsageFlags(ETextureUsageFlags _me_flags);
D3D12_RESOURCE_FLAGS METoDxTextureResourceFlags(ETextureUsageFlags _me_flags);

//static EPixelFormat          VKToMEFormat(VkFormat _format);

//static VkBufferUsageFlags    METoVKBufferUsageFlags(EBufferUsageFlags _me_flags);
// for EBufferUsageFlags, we only care about uav and blas/tlas

//static VkDescriptorType      METoVkBufferDescriptorType(EBufferUsageFlags _type);
//static VkSampleCountFlagBits METoVKSampleCountFlagBits(uint32_t _me_count);
//static VkImageAspectFlags    METoVKImageAspectFlags(ETextureAspectFlags _flags);

//static VkImageViewType       METoVKImageViewType(ETextureDimension _dim);
//static VkImageLayout         METoVKImageLayout(ETextureLayout _layout);
//static VkAttachmentLoadOp    METoVKAttachmentLoadOp(EAttachmentLoadOp _load_op);
//static VkAttachmentStoreOp   METoVKAttachmentStoreOp(EAttachmentStoreOp _store_op);
//static VkFilter              METoVKImageFilter(ESamplerFilter _filter);

//static VkPipelineStageFlags2 METoVkPipelineStageFlags2(ERHIPipelineStageFlags _flags);
//static VkAccessFlags2        METoVkAccessFlags2(ERHIAccessFlags _flags);

//static VkCullModeFlags     METoVKCullModeFlags(ERasterizerCullMode _cull_mode);
//static VkPrimitiveTopology METoVKPrimitiveTopology(EPrimitiveTopology _primitive_type);
//static VkPolygonMode       METoVKPolygonMode(ERasterizerFillMode _fill_mode);

//static VkDescriptorType   METoVKDescriptorType(EShaderParameterType _type, EShaderCodeResourceBindingType _binding_type);
//static VkShaderStageFlags METoVKShaderStageFlags(EShaderType _type);

//static uint32_t METoVkQueueFamilyIndex(ECommandQueueType _type, const VulkanDevice* _device);
//static uint32_t METoVkQueueFamilyIndex(ECommandListType _type, const VulkanDevice* _device);

//static VkFilter             METoVKMinMagFilterMode(ESamplerFilter _filter);
//static VkSamplerMipmapMode  METoVKMipmapMode(ESamplerFilter _filter);
//static VkSamplerAddressMode METoVKWrapMode(ESamplerAddressMode _address_mode);
//static VkCompareOp          METoVKCompareOpSampler(ESamplerCompareFunction _compare_op);

//static VkCompareOp METoVKCompareOp(ECompareOption _compare_op);
//static VkStencilOp METoVKStencilOp(EStencilOp _stencil_op);

//static VkBlendOp     METoVKBlendOp(EBlendOperation _blend_op);
//static VkBlendFactor METoVKBlendFactor(EBlendFactor _blend_factor);

//static VkVertexInputRate METoVKVertexInputRate(EVertexInputRate _me_rate);

//static VkQueueFlagBits METoVKQueueFlagBits(EQueueType _type);

////Raytracing
//static VkGeometryTypeKHR                    METoVKGeometryType(ERayTracingGeometryType _type);
//static VkGeometryFlagsKHR                   METoVKGeometryFlags(ERayTracingGeometryFlags _flags);
//static VkBuildAccelerationStructureFlagsKHR METoVKAccelerationStructureBuildType(ERayTracingAccelerationStructureBuildFlags _type);
//static VkBuildAccelerationStructureModeKHR  METoVKBuildAccelerationStructureMode(ERaytracingBuildMode _mode);
} // namespace D3D12EnumTranslation

inline bool operator==(ED3D12ShaderVariableType x, uint y) {
    return uint(x) == y;
}

inline bool IsShaderVarRootConstant(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::RootConstant;
}
inline bool IsShaderVarConstantBuffer(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::ConstantBuffer;
}
inline bool IsShaderVarCommonBuffer(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::ByteAddressBuffer ||
           type == ED3D12ShaderVariableType::StructuredBuffer ||
           type == ED3D12ShaderVariableType::TypedBuffer ||
           type == ED3D12ShaderVariableType::RWByteAddressBuffer ||
           type == ED3D12ShaderVariableType::RWStructuredBuffer ||
           type == ED3D12ShaderVariableType::RWTypedBuffer;
}
inline bool IsShaderVarTexture(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::Texture1D || type == ED3D12ShaderVariableType::Texture1DArray ||
           type == ED3D12ShaderVariableType::Texture2D || type == ED3D12ShaderVariableType::Texture2DArray ||
           type == ED3D12ShaderVariableType::Texture3D || type == ED3D12ShaderVariableType::TextureCube ||
           type == ED3D12ShaderVariableType::TextureCubeArray ||
           type == ED3D12ShaderVariableType::RWTexture1D ||
           type == ED3D12ShaderVariableType::RWTexture1DArray ||
           type == ED3D12ShaderVariableType::RWTexture2D ||
           type == ED3D12ShaderVariableType::RWTexture2DArray ||
           type == ED3D12ShaderVariableType::RWTexture3D;
}
inline bool IsShaderVarSampler(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::Sampler;
}
inline bool IsShaderVarRaytracingAccelerationStructure(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::RaytracingAccelerationStructure;
}
inline bool IsShaderVarSrv(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::ByteAddressBuffer ||
           type == ED3D12ShaderVariableType::StructuredBuffer ||
           type == ED3D12ShaderVariableType::TypedBuffer || type == ED3D12ShaderVariableType::Texture1D ||
           type == ED3D12ShaderVariableType::Texture1DArray || type == ED3D12ShaderVariableType::Texture2D ||
           type == ED3D12ShaderVariableType::Texture2DArray || type == ED3D12ShaderVariableType::Texture3D ||
           type == ED3D12ShaderVariableType::TextureCube ||
           type == ED3D12ShaderVariableType::TextureCubeArray ||
           type == ED3D12ShaderVariableType::RaytracingAccelerationStructure;
}
inline bool IsShaderVarUav(ED3D12ShaderVariableType type) {
    return type == ED3D12ShaderVariableType::RWByteAddressBuffer ||
           type == ED3D12ShaderVariableType::RWStructuredBuffer ||
           type == ED3D12ShaderVariableType::RWTypedBuffer || type == ED3D12ShaderVariableType::RWTexture1D ||
           type == ED3D12ShaderVariableType::RWTexture1DArray ||
           type == ED3D12ShaderVariableType::RWTexture2D ||
           type == ED3D12ShaderVariableType::RWTexture2DArray ||
           type == ED3D12ShaderVariableType::RWTexture3D;
}
inline bool IsShaderVarRootConstant(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarRootConstant(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarConstantBuffer(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarConstantBuffer(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarCommonBuffer(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarCommonBuffer(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarTexture(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarTexture(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarSampler(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarSampler(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarRaytracingAccelerationStructure(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarRaytracingAccelerationStructure(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarSrv(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarSrv(ED3D12ShaderVariableType(param.type));
}
inline bool IsShaderVarUav(const ReflectParamInfo::Dxil& param) {
    return IsShaderVarUav(ED3D12ShaderVariableType(param.type));
}

class D3D12Device;
class D3D12GpuGlobalAllocator;
class D3D12GraphicsCommandQueue;
class D3D12Fence;
class D3D12DescriptorHeap; // todo
class D3D12Texture;
class D3D12Buffer;
class D3D12CommandResourceAllocator;
class D3D12PipelineState;
class D3D12BindlessArray; // todo

// only to provide a 'device' as member
class D3D12DeviceChild {
public:
    D3D12DeviceChild(D3D12Device* _device) : device(_device) {}

public:
    D3D12Device* GetDevice() const {
        return device;
    }

protected:
    D3D12Device* device;
    // note here no defer release, compare to VulkanDeviceObject
    // seems not very necessary because of heavily using countableref
    // if need, we can do it explicitly
};

class D3D12PipelineState final : public PipelineState, public D3D12DeviceChild {
public:
    enum EType {
        GFX,
        Compute,
        RT
    };

    struct PipelineLayout {
        // how to group descriptors?
        // current approach:
        // - root constant as root constant
        // - constant buffer as root cbv   (if too many, maybe use a descriptor table
        // - the bindlessarray as root srv!
        // - other resource in SRV/UAV/Sampler table. no more root descriptor  TODO

        struct RootConstant {
            uint8 idx_in_cpp_args = -1;
            uint8 idx_in_root_sig = -1;
            uint8 byte_size       = 4;
            uint8 slot            = 0;
            uint8 space           = 0;
        };

        struct RootDescriptor {
            uint8 idx_in_cpp_args = -1;
            uint8 idx_in_root_sig = -1;
            uint8 slot            = 0;
            uint8 space           = 0;
        };

        struct RootDescriptorTable {
            uint8 idx_in_root_sig = -1;
            struct Entry {
                uint8 idx_in_cpp_args = -1;
                uint8 idx_in_table    = -1;
                uint8 bind_count      = 1;
                uint8 slot            = 0;
                uint8 space           = 0;
            };
            Array<Entry> entries;

            uint GetTotalBindCount() const;
        };

        std::optional<RootConstant>          root_constant      = std::nullopt;
        std::optional<RootDescriptor>        the_bindless_array = std::nullopt;
        std::optional<Array<RootDescriptor>> root_cbvs          = std::nullopt;
        std::optional<RootDescriptorTable>   srv_table          = std::nullopt;
        std::optional<RootDescriptorTable>   uav_table          = std::nullopt;
        std::optional<RootDescriptorTable>   sampler_table      = std::nullopt;
        // maybe lack some flexiblity?

        void
        Add(uint8                         _idx_in_cpp_args,
            const ShaderArgCppInfo&       _arg_cpp_info,
            const ReflectParamInfo::Dxil& _resource_info,
            bool                          _b_special_bindless);
    };

private:
    friend class D3D12Device;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12RootSignature> root_signature;
    PipelineLayout              layout;
    EType                       type;

public:
    D3D12PipelineState(D3D12Device* _device, EType _type) :
        PipelineState(),
        D3D12DeviceChild(_device),
        pipeline_state(nullptr),
        root_signature(nullptr),
        type(_type) {}

    void                  BuildRootSignature(const PipelineLayout& _layout);
    const PipelineLayout& GetLayout() const {
        return layout;
    }

    ID3D12PipelineState* Native() const {
        return pipeline_state.Get();
    }
    ID3D12RootSignature* NativeRootSignature() const {
        return root_signature.Get();
    }

    void Destroy() override; // from PipelineState.RHIResource
};

struct DescriptorIndex {
    //D3D12DescriptorHeapBase* heap;
    uint index;
};

class D3D12DescriptorHeapBase : public D3D12DeviceChild {
protected:
    ComPtr<ID3D12DescriptorHeap> heap                  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  start_handle_cpu      = {0};
    D3D12_GPU_DESCRIPTOR_HANDLE  start_handle_gpu      = {0};
    uint                         descriptor_size       = 0;
    uint                         num_total_descriptors = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE   type;

public:
    D3D12DescriptorHeapBase(
        D3D12Device*               _device,
        D3D12_DESCRIPTOR_HEAP_TYPE _type,
        uint32_t                   _num_descriptors,
        bool                       _is_shader_visible
    );

    bool IsShaderVisible() const {
        return start_handle_gpu.ptr != 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetStartHandleCpu() const {
        return start_handle_cpu;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetStartHandleGpu() const {
        return start_handle_gpu;
    }
    ID3D12DescriptorHeap* Native() const {
        return heap.Get();
    }
    uint GetDescriptorSize() const {
        return descriptor_size;
    }
    uint GetNumTotalDescriptors() const {
        return num_total_descriptors;
    }
    D3D12_DESCRIPTOR_HEAP_TYPE GetType() const {
        return type;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetOffsetHandleCpu(DescriptorIndex index) const {
        return {start_handle_cpu.ptr + index.index * descriptor_size};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetOffsetHandleGpu(DescriptorIndex index) const {
        return {start_handle_gpu.ptr + index.index * descriptor_size};
    }
};

class D3D12CpuDescriptorHeap : public D3D12DescriptorHeapBase {
public:
    D3D12CpuDescriptorHeap(
        D3D12Device*               _device,
        D3D12_DESCRIPTOR_HEAP_TYPE _type,
        uint32_t                   _num_descriptors
    ) :
        D3D12DescriptorHeapBase(_device, _type, _num_descriptors, false) {}

    D3D12_GPU_DESCRIPTOR_HANDLE GetStartHandleGpu() const                       = delete;
    D3D12_GPU_DESCRIPTOR_HANDLE GetOffsetHandleGpu(DescriptorIndex index) const = delete;
};

class D3D12GpuDescriptorHeap : public D3D12DescriptorHeapBase {
public:
    D3D12GpuDescriptorHeap(
        D3D12Device*               _device,
        D3D12_DESCRIPTOR_HEAP_TYPE _type,
        uint32_t                   _num_descriptors
    ) :
        D3D12DescriptorHeapBase(_device, _type, _num_descriptors, true) {}
};

// manage descriptor one by one
class D3D12CpuDescriptorAllocator : public D3D12CpuDescriptorHeap {
private:
    std::queue<DescriptorIndex> free_queue;
    uint                        num_descriptors_allocated = 0;

public:
    using D3D12CpuDescriptorHeap::D3D12CpuDescriptorHeap;

    bool IsHeapFull() const {
        return num_descriptors_allocated == GetNumTotalDescriptors();
    }
    DescriptorIndex Allocate();
    void            Free(DescriptorIndex handle);
};

// in ring buffer fashion. we copy needed descriptors every time, not consider descriptor cache
class D3D12GpuDescriptorAllocator : public D3D12GpuDescriptorHeap {
private:
    uint                          max_descriptors_per_execution = 0; // const
    uint                          num_descriptor_chunk          = 0; // const
    uint                          num_used_chunks               = 0;
    uint                          current_chunk_index           = 0;
    uint                          current_chunk_offset          = 0;
    LockFreeQueueBase<uint, true> ready_chunk_indices;

public:
    D3D12GpuDescriptorAllocator(
        D3D12Device*               _device,
        D3D12_DESCRIPTOR_HEAP_TYPE _type,
        uint32_t                   _num_descriptors_total,
        uint32_t                   _max_descriptors_per_execution
    );

    void BeginPushDescriptors();
    void EndPushDescriptors();

    DescriptorIndex Allocate(uint count = 1); // now should only be called by device...
};

// todo: bindless array,  want keep those descriptor 'static'  (not need copy every time
//       (split half to dynamic gpuallocator, half to static bindlessarray ?
//class D3D12BindlessArray : public BindlessArray {
//private:
//public:
//};

struct Allocation {
    D3D12MA::Allocation* alloc    = nullptr;
    ID3D12Resource*      resource = nullptr;

    Allocation()                             = default;
    Allocation(const Allocation&)            = delete;
    Allocation& operator=(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept :
        alloc(std::exchange(other.alloc, nullptr)),
        resource(std::exchange(other.resource, nullptr)) {}
    Allocation& operator=(Allocation&& other) noexcept {
        alloc    = std::exchange(other.alloc, nullptr);
        resource = std::exchange(other.resource, nullptr);
        return *this;
    }
    ~Allocation() {
        if (resource) {
            resource->Release();
        }
        if (alloc) {
            alloc->Release();
        }
        // order matters
    }
};

class D3D12GpuGlobalAllocator {
private:
    ComPtr<D3D12MA::Allocator> d3d12Allocator;
    D3D12Device*               device;

public:
    D3D12GpuGlobalAllocator(D3D12Device* _device);

    Allocation AllocateBufferHeap(
        std::string_view _name, // name?
        uint64           _byte_size,
        D3D12_HEAP_TYPE  _heap_type = D3D12_HEAP_TYPE_DEFAULT
    );
    Allocation AllocateTextureHeap(
        std::string_view _name,
        uint64           _byte_size,
        uint64           _alignment  = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        bool             _is_rtv_dsv = false
    );
};

class D3D12Texture final : public Texture, public D3D12DeviceChild {
public:
    struct ViewDesc {
        ED3D12ShaderVariableType type            = ED3D12ShaderVariableType::Texture2D;
        DXGI_FORMAT              format          = DXGI_FORMAT_UNKNOWN;
        uint8_t                  first_mip_level = 0; // note we not care array/plane level subresource range
        uint8_t                  num_mip_levels  = uint8_t(-1);
        bool                     b_depth_read_only = false;

        auto operator<=>(const ViewDesc& other) const = default;
    };
    struct ViewRecord {
        ViewDesc        desc;
        DescriptorIndex index; // in cpu side descriptor heap
    };

private:
    Allocation        allocation;
    Array<ViewRecord> srv_uav_views; // srv,uav. todo rtv,dsv?

public:
    D3D12Texture(D3D12Device* _device, const TextureInfo& _info);
    D3D12Texture(D3D12Device* _device, const TextureInfo& _info, Allocation&& _allocation);

    ~D3D12Texture();

    ID3D12Resource* Native() const {
        return allocation.resource;
    }
    uint QuerySubresourceIndex(uint _mip_level, uint _array_slice, uint _plane_slice) const;

    void            Destroy() override;                             // from Texxture.RHIResource
    uint            GetMipByteSize(uint _mip_idx) const override;   // from Texture
    RENDER_API void SetName(const std::string_view _name) override; // from Texture
};

// not for readback/upload
class D3D12Buffer final : public Buffer, public D3D12DeviceChild {
public:
    struct ViewDesc {
        ED3D12ShaderVariableType type        = ED3D12ShaderVariableType::StructuredBuffer;
        DXGI_FORMAT              format      = DXGI_FORMAT_UNKNOWN;
        uint64                   byte_offset = 0;
        // always use full size. stride comes from BufferInfo

        auto operator<=>(const ViewDesc& other) const = default;
    };
    struct ViewRecord {
        ViewDesc        desc;
        DescriptorIndex index; // in cpu side descriptor heap
    };

private:
    Allocation        allocation;
    Array<ViewRecord> srv_uav_views; // srv,uav

public:
    D3D12Buffer(D3D12Device* _device, const BufferInfo& _info);
    D3D12Buffer(D3D12Device* _device, const BufferInfo& _info, Allocation&& _allocation);

    ~D3D12Buffer();

    ID3D12Resource* Native() const {
        return allocation.resource;
    }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const {
        return allocation.resource->GetGPUVirtualAddress();
    }

    void            Destroy() override; // from Buffer.RHIResource, mainly called by CountableRef
    RENDER_API void SetName(const std::string_view _name) override; // from Buffer

    DescriptorIndex CreateSrv(const BufferView& _range, ED3D12ShaderVariableType _type);
    DescriptorIndex CreateUav(const BufferView& _range, ED3D12ShaderVariableType _type);
};

class D3D12Fence final : public Fence, public D3D12DeviceChild {
private:
    ComPtr<ID3D12Fence> fence;
    HANDLE              event;
    std::atomic<uint64> current_value = 0;
    mutable std::mutex   rejection_mutex;
    UnorderedSet<uint64> rejected_values;

public:
    D3D12Fence(D3D12Device* _device);
    ~D3D12Fence();
    D3D12Fence(const D3D12Fence&) = delete;

    ID3D12Fence* Native() const {
        return fence.Get();
    }
    uint64_t GetValue() const override; // by default, get latest value, not cached 'current_value'
    void     Wait(uint64_t _value) override;
    void     Reject(uint64_t _value) override;

    bool IsFenceComplete(uint64 _value);
    bool IsRejected(uint64 _value) const;
    void WaitOnHost(uint64 _value);
    void SignalOnHost(uint64 _value);
};

struct D3D12StagingBufferView {
    D3D12Buffer*              buffer;
    uint64                    byte_offset;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base_address_remapped; // resource->GetGPUVirtualAddress() + byte_offset
    uint8*                    cpu_base_address_remapped;
};

class D3D12CommandList {
private:
    D3D12CommandResourceAllocator&     allocator;
    ComPtr<ID3D12GraphicsCommandList7> list; // 7 for enhanced barrier

public:
    D3D12CommandList(D3D12CommandResourceAllocator&);
    D3D12CommandList(const D3D12CommandList&) = delete;
    D3D12CommandList(D3D12CommandList&&)      = default;

    ID3D12GraphicsCommandList7* Native() const {
        return list.Get();
    }

    void Begin();
    void End();

    void SetPso(const PipelineHandle& _handle);
    void BindDescriptors(const PipelineHandle& _pso_handle, const ArrayArguments& _args);

    void Dispatch(uint _x, uint _y, uint _z);

    void
    CopyBuffer(D3D12Buffer* _src, D3D12Buffer* _dst, uint64 _size, uint64 _src_offset, uint64 _dst_offset);
    void CopyData(D3D12StagingBufferView _dst, const void* _data, uint64 _size);
    void CopyData(void* _dst, D3D12StagingBufferView _src, uint64 _size);
    void CopyBufferToTexture(
        D3D12Buffer*  _src,
        D3D12Texture* _dst,
        uint64        _src_offset,
        uint3         _dst_offset,
        uint3         _dst_extent,
        uint32        _mip_level
    );
    void CopyTextureToBuffer(
        D3D12Texture* _src,
        D3D12Buffer*  _dst,
        uint3         _src_offset,
        uint64        _dst_offset,
        uint3         _src_extent,
        uint32        _mip_level
    );
    void CopyTexture(
        D3D12Texture* _src,
        D3D12Texture* _dst,
        uint3         _extent,
        uint3         _src_offset,
        uint3         _dst_offset,
        uint32        _src_mip_level,
        uint32        _dst_mip_level
    );
};

class D3D12ResourceStateTracker {
private:
    Array<D3D12_GLOBAL_BARRIER>  global_barriers; // not used yet
    Array<D3D12_TEXTURE_BARRIER> texture_barriers;
    Array<D3D12_BUFFER_BARRIER>  buffer_barriers;

    struct TextureStateDescription {
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        D3D12_BARRIER_SYNC   sync   = D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS access =
            D3D12_BARRIER_ACCESS_COMMON; // note the default case SYNC_NONE is not compatible with ACCESS_COMMON, here use COMMON is because it is 0, so can 'or' with other access later
        // D3D12_TEXTURE_BARRIER_FLAGS discard ?

        auto operator<=>(const TextureStateDescription& other) const = default;
    };
    struct BufferStateDescription {
        D3D12_BARRIER_SYNC   sync   = D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;

        auto operator<=>(const BufferStateDescription& other) const = default;
    };

    struct TextureState {
        TextureStateDescription before;
        TextureStateDescription after;

        TextureStateDescription initial; // backup
    };
    struct BufferState {
        BufferStateDescription before;
        BufferStateDescription after;

        //BufferStateDescription initial; // don't need specific initial state
    };

    UnorderedMap<D3D12Texture*, TextureState> texture_states;
    UnorderedMap<D3D12Buffer*, BufferState>   buffer_states;
    Set<D3D12Texture*> pending_textures; // textures need to be transitioned in this layer
    Set<D3D12Buffer*>  pending_buffers;  // buffers need to be transitioned in this layer

public:
    D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT; // !not used yet

    // dont't care about tex subresource
    void RecordState(D3D12Texture* texture, ETextureState _state, EPassType _pass_type, bool _is_write);
    void RecordState(D3D12Texture* texture, TextureStateDescription _state);
    void RecordState(D3D12Buffer* buffer, EBufferState _state, EPassType _pass_type, bool _is_write);
    void RecordState(D3D12Buffer* buffer, BufferStateDescription _state);

    void ResolveBarriers();
    void DispatchBarriers(ID3D12GraphicsCommandList7* list);

    // after whole execute , restore state to initial
    void RestoreState();

    void Reset();
};

// only for upload/readback buffer now. may change in the future
class D3D12BuddyAllocator : public D3D12DeviceChild {
public:
    friend struct BuddyBlock;

    // not consider mem used for multi-frame
    struct BuddyBlock {
        D3D12BuddyAllocator* parent             = nullptr;
        uint32               block_offset       = 0;
        uint32               block_order        = 0;
        uint32               memory_byte_offset = 0; // offset start from the underlying buffer
        uint32               required_size      = 0;

        bool IsValid() const {
            return parent != nullptr; // not accurate but enough
        }

        D3D12Buffer* GetUnderlyingBuffer() const {
            return parent->underlying_buffer.get();
        }

        uint8* GetCpuAddressRemapped() const {
            return parent->ptr_mapped + memory_byte_offset;
        }

        D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddressRemapped() const {
            return parent->underlying_buffer->GetGpuVirtualAddress() + memory_byte_offset;
        }

        // current target usage dont need this, instead, it became a problem
        //~BuddyBlock() {
        //    if (parent) {
        //        parent->Deallocate(*this);
        //    }
        //}
    };

    D3D12BuddyAllocator(
        D3D12Device*    _device,
        D3D12_HEAP_TYPE _heap_type,
        uint32          _total_byte_size,
        uint32          _min_block_byte_size
    );
    D3D12BuddyAllocator(const D3D12BuddyAllocator&) = delete;
    D3D12BuddyAllocator(D3D12BuddyAllocator&&)      = default;
    ~D3D12BuddyAllocator();

    BuddyBlock Allocate(uint32 _size, uint32 _alignment);

    void Deallocate(const BuddyBlock& _block);

    void CleanUpAllocations();

private:
    void Initialize();

    // useful if min_block_size % alignment != 0
    uint32 GetSizeToAllocate(uint32 _size, uint32 _alignment);

    bool CanAllocate(uint32 required_size);

    uint32 SizeToBlockCount(uint32 _size) const {
        return (_size + (min_block_byte_size - 1)) / min_block_byte_size;
    }
    static uint32 BlockCountToOrder(uint32 _size) {
        unsigned long Result;
        _BitScanReverse(&Result, _size + _size - 1); // ceil(log2(size))
        return Result;
    }
    static uint32 OrderToBlockCount(uint32 _order) {
        return ((uint32)1) << _order;
    }

    BuddyBlock AllocateInternal(uint32 _size, uint32 _alignment);

    // return block id/offset, i.e. block serial number
    uint32 AllocateBlock(uint32 _order);

    struct RetiredBlock {
        uint32 offset;
        uint32 order;
    };
    void DeallocateInternal(RetiredBlock _block);

    void DeallocateBlock(uint32 _offset, uint32 _order);

private:
    const uint32 total_byte_size;
    const uint32 min_block_byte_size;

    const uint32 max_order;
    uint32       total_used_byte_size;

    Array<Set<uint32>>  free_blocks;
    Array<RetiredBlock> deferred_deallocate_blocks;

    const D3D12_HEAP_TYPE  heap_type;
    UniquePtr<D3D12Buffer> underlying_buffer;
    uint8*                 ptr_mapped;
};

class D3D12MultiBuddyAllocator : public D3D12DeviceChild {
private:
    Array<D3D12BuddyAllocator> allocators;
    const uint32               each_total_byte_size;
    const uint32               min_block_byte_size;
    const D3D12_HEAP_TYPE      heap_type;

public:
    D3D12MultiBuddyAllocator(
        D3D12Device*    _device,
        D3D12_HEAP_TYPE _heap_type,
        uint32          _each_total_byte_size,
        uint32          _min_block_byte_size
    );

    D3D12BuddyAllocator::BuddyBlock Allocate(uint32 _size, uint32 _alignment);

    void CleanUpAllocations();
};

// avoid buddyblock dtor...// ok it is weired
class D3D12MultiBuddyAllocatorAutoFree : public D3D12DeviceChild {
private:
    D3D12MultiBuddyAllocator                      allocator;
    Array<Array<D3D12BuddyAllocator::BuddyBlock>> allocated_blocks;

public:
    D3D12MultiBuddyAllocatorAutoFree(
        D3D12Device*    _device,
        D3D12_HEAP_TYPE _heap_type,
        uint32          _each_total_byte_size,
        uint32          _min_block_byte_size
    );

    D3D12StagingBufferView Allocate(uint32 _size, uint32 _alignment);

    void CleanUpAllocations();
};

// specialize for constant buffer
class D3D12FastConstantAllocator : public D3D12DeviceChild {
private:
    D3D12MultiBuddyAllocatorAutoFree allocator;
    const uint32                     page_size;
    uint32                           offset;
    D3D12StagingBufferView           underlying_current;

public:
    D3D12FastConstantAllocator(D3D12Device* _device, uint32 _total_byte_size, uint32 _min_block_byte_size);

    D3D12StagingBufferView Allocate(uint32 _size);

    void CleanUpAllocations();
};

// resource & cmd allocator
class D3D12CommandResourceAllocator : public D3D12DeviceChild {
private:
    friend class D3D12CommandList;
    D3D12GraphicsCommandQueue&     queue;
    ComPtr<ID3D12CommandAllocator> cmd_allocator;
    UniquePtr<D3D12CommandList>
        cmd_list; // follow the pattern allocator:cmdlist = 1:1; ptr is to delay the ctor

    Array<std::function<void()>> on_complete_callbacks;
    D3D12ResourceStateTracker    tracker;

    static constexpr uint32 larget_buffer_byte_size_threshold = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    Array<UniquePtr<D3D12Buffer>>    large_buffers; // only for upload/readback
    D3D12MultiBuddyAllocatorAutoFree upload_allocator;
    D3D12MultiBuddyAllocatorAutoFree readback_allocator;
    D3D12FastConstantAllocator       constant_allocator;

    // in terms of each buffer may need different state, this can't use manual sub-allocation from a big buffer
    // rather we prefer sub-allocation from a heap, which simply rely on D3D12MA
    // note we don't have 'explicit' cross-frame cache for this, unlike upload/readback
    struct HeapPlacedBufferAllocator {
        D3D12CommandResourceAllocator& parent;

        Array<UniquePtr<D3D12Buffer>> allocated; // can't simply use array<buffer> because of atomic inside

        D3D12Buffer* Allocate(uint64 _size, EBufferUsageFlags _flag);
        void         Reset();
    };
    HeapPlacedBufferAllocator default_buffer_allocator;
    HeapPlacedBufferAllocator scratch_buffer_allocator;

public:
    D3D12CommandResourceAllocator(D3D12GraphicsCommandQueue& queue);
    D3D12CommandResourceAllocator(const D3D12CommandResourceAllocator&) = delete;
    D3D12CommandResourceAllocator(D3D12CommandResourceAllocator&&)      = default;

    ID3D12CommandAllocator* Native() const {
        return cmd_allocator.Get();
    }
    D3D12Device* GetDevice() const {
        return device;
    }
    D3D12CommandList* GetCommandList() {
        return cmd_list.get();
    }
    D3D12ResourceStateTracker& GetResourceStateTracker() {
        return tracker;
    }

public:
    void Reset();
    void AddOnComplete(std::function<void()>&& _func) {
        on_complete_callbacks.push_back(std::move(_func));
    }
    void OnComplete() {
        for (auto&& cb : on_complete_callbacks) {
            cb();
        }
        on_complete_callbacks.clear();
    }

    D3D12StagingBufferView AllocateConstantBuffer(uint32 _size);
    D3D12StagingBufferView AllocateUploadBuffer(uint64 _size, uint32 _alignment);
    D3D12StagingBufferView AllocateReadbackBuffer(uint64 _size, uint32 _alignment);
    D3D12Buffer*           AllocateScratch(uint64 _size);
    D3D12Buffer*           AllocateDefaultBuffer(
                  uint64 _size
              ); // no special usage(uav), mainly for intermediate uploaded/staging resource vb,ib
};

class D3D12GraphicsCommandQueue final : public CommandQueue, public D3D12DeviceChild {
private:
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_LIST_TYPE    queue_type;
    D3D12Fence                 queue_fence;
    std::atomic<uint64>        last_submitted_fence_value;
    std::atomic<uint64>        last_completed_fence_value;
    // ideally, 'last_completed_fence_value' should be same as queue_fence.current_value.
    // however consider case recycle allocator, we first wait gpu signal queue_fence to X, then reset allocator, then update lastxxx to X
    // this make a difference. so 'last_completed_fence_value' means a bit more than gpufence in terms of cpu time
    CircularQueue<uint64, 3> pending_fence_values; // to limit the number of cmdallocators / on-the-fly frames

    LockFreeQueueBase<D3D12CommandResourceAllocator, false> ready_allocators;
    // ptr size not satisfy the 'inline version' requirement,
    // this internally only hold D3D12CommandResourceAllocator* (not push uniqueptr into

    using EventType = Variant<
        UniquePtr<D3D12CommandResourceAllocator>,
        Array<std::function<void()>>,
        SignalEvent,
        WaitEvent>;

    struct QueueEvent {
        EventType event;
        uint64    timeline;
        bool      wake_thread;
        template<typename Arg>
            requires std::is_constructible_v<EventType, Arg&&>
        QueueEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) :
            event(std::forward<Arg>(_event)),
            timeline(_timeline),
            wake_thread(_wake_thread) {}

        QueueEvent(QueueEvent&& _other) noexcept :
            event(std::move(_other.event)),
            timeline(_other.timeline),
            wake_thread(_other.wake_thread) {}
    };
    DEQueue<QueueEvent>         event_queue;
    std::mutex                  mtx;
    std::condition_variable_any cv;

    std::atomic<bool> is_second_thread_busy = false; // to wait ExecuteThread() to complete when Sync()
    std::jthread      thd;                           // dtor first

private:
    void ExecuteThread(std::stop_token _st);

public:
    ID3D12CommandQueue* Native() const {
        return queue.Get();
    }
    D3D12Device* GetDevice() const {
        return device;
    }
    D3D12_COMMAND_LIST_TYPE GetType() const {
        return queue_type;
    }
    UniquePtr<D3D12CommandResourceAllocator> RequestCommandResourceAllocator();

    void Complete(uint64 fence_value);

public:
    D3D12GraphicsCommandQueue(D3D12Device* _device, EQueueType _type);
    ~D3D12GraphicsCommandQueue();

    // queue.signal is not exposed, exist in cmdsubmit.signalevent
    void        Wait(WaitEvent _event) override;
    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void Present(
        SwapchainRef      _swapchain,
        TextureView       _target,
        PresentReceiptRef _receipt = {}
    ) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override {
        return {};
    }
};

class D3D12Swapchain final : public Swapchain {
public:
    D3D12Swapchain(D3D12Device& device, const SwapchainCreateInfo& info);
    ~D3D12Swapchain() = default;

    void Recreate(const SwapchainCreateInfo&) override;
    void Sync() override;

    uint GetBackbufferIndex() const {
        return frame_index;
    }
    //DescriptorIndex GetBackbufferRTV() const { return m_backbufferRTVs[m_frameIndex]; }
    //GpuTexture*     GetBackbuffer() const { return m_backbufferTextures[m_frameIndex].get(); }

    uint32_t GetWidth() const {
        return width;
    }
    uint32_t GetHeight() const {
        return height;
    }

    void Present();

private:
    void CreateSizeDependentResources();

private:
    D3D12Device&                  device;
    uintptr_t                     window_handle;
    ComPtr<IDXGISwapChain3>       swapchain;
    Array<ComPtr<ID3D12Resource>> backbuffers;
    uint                          width;
    uint                          height;
    uint                          frame_index;
    /*     DescriptorIndex             m_backbufferRTVs[FrameLatency];
        std::unique_ptr<GpuTexture> m_backbufferTextures[FrameLatency];*/
};

//class D3D12DescriptorSetsLayout;
//class D3D12DescriptorSetAllocator;
//class D3D12DescriptorSetWriter;

struct D3D12RHIConfig {
    bool force_sync = true; // true if want to wait for ExecuteThread() in CommandQueue::Sync()
    //bool want_capture = false; // put here because we have to initialize capturer before the device is created
    //uint32 api_version = VK_API_VERSION_1_3; // feature level?
};

class D3D12Device final : public RenderDevice::Impl {
    friend class D3D12GraphicsCommandQueue;

public:
    D3D12Device(const D3D12RHIConfig&& _config);
    ~D3D12Device();

    // not implemented yet
    PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) override;
    PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders) override;

    TextureRef CreateTexture(
        std::string_view   _name,
        ETextureDimension  _dimension,
        Extent3D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt,
        uint               _array_size
    ) override;

    BufferRef CreateBuffer(
        std::string_view  _name,
        uint              _element_cnt,
        uint              _byte_stride,
        EBufferUsageFlags _usage,
        EPixelFormat      _format
    ) override;

    BindlessArrayRef CreateBindlessArray(uint _max_size) override;
    FenceRef         CreateFence() override;

    // Raytracing
    RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) override;

    RaytracingSceneRef CreateRaytracingScene() override;

    CommandQueue& GetCommandQueue(EQueueType _type) override;

    CopyQueue& GetCopyQueue() override;

    SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

private:
    void PostInit() override;

    void GetHardwareAdapter(
        ComPtr<IDXGIFactory4> pFactory,
        IDXGIAdapter1**       ppAdapter,
        bool                  requestHighPerformanceAdapter
    );

public:
    //void           EnqueueDeferredRelease(RHIResource* _object);
    //void           FlushDeferredReleases();
    constexpr uint ImmutableSamplerCount() const {
        return immutable_sampler_count;
    }
    //const VkSampler* GetImmutableSamplers() const { return immutable_samplers.data(); }
    //const VkSampler  GetSampler(Sampler _sampler) const;
    //const uint       GetSamplerIdx(Sampler _sampler) const {
    //    uint filter  = uint(_sampler.filter);
    //    uint address = uint(_sampler.address_mode);
    //    uint compare = uint(_sampler.compare_function);
    //    return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
    //}

    //void SetResourceName(uint64 _object, VkObjectType _object_type, const std::string_view _name);

private:
    D3D12RHIConfig        config;
    CD3DX12FeatureSupport feature_supports;

    ComPtr<IDXGIInfoQueue> dxgi_info_queue;
    //ComPtr<IDXGIDebug>        dxgi_debug;
    //ComPtr<ID3D12DebugDevice> debug_device;
    ComPtr<ID3D12Debug>      debug_interface;
    ComPtr<ID3D12InfoQueue>  debug_info_queue; // can remove this once we find out how to create queue1
    ComPtr<ID3D12InfoQueue1> debug_info_queue1;
    DWORD                    debug_log_callback_cookie;

    ComPtr<ID3D12Device10>             device; // use 10 for enhanced barrier
    ComPtr<IDXGIFactory6>              factory;
    ComPtr<IDXGIAdapter3>              adapter;
    UniquePtr<D3D12GpuGlobalAllocator> gpu_global_allocator; // ptr to construct after device

    UniquePtr<D3D12CpuDescriptorAllocator> csu_heap_cpu;
    UniquePtr<D3D12CpuDescriptorAllocator> rtv_heap_cpu;
    UniquePtr<D3D12CpuDescriptorAllocator> dsv_heap_cpu;
    UniquePtr<D3D12CpuDescriptorAllocator> sampler_heap_cpu;
    UniquePtr<D3D12GpuDescriptorAllocator> csu_heap_gpu;
    UniquePtr<D3D12GpuDescriptorAllocator> sampler_heap_gpu;

    UniquePtr<D3D12GraphicsCommandQueue> gfx_queue{};
    //UniquePtr<D3D12CommandQueue>              compute_queue{};
    //UniquePtr<D3D12CopyQueue>                          copy_queue{};
    //LockFreeQueueBase<RHIResource, false, 64> deferred_release_queue{};
    static constexpr uint immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
    //StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};

public:
    ID3D12Device10* Native() const {
        return device.Get();
    }
    IDXGIFactory6* NativeFactory() const {
        return factory.Get();
    }
    IDXGIAdapter3* NativeAdapter() const {
        return adapter.Get();
    }
    CD3DX12FeatureSupport& GetFeatureSupport() {
        return feature_supports;
    }
    D3D12GpuGlobalAllocator* GetGpuGlobalAllocator() {
        return gpu_global_allocator.get();
    }
    D3D12CpuDescriptorAllocator* GetCsuHeap() {
        return csu_heap_cpu.get();
    }
    D3D12CpuDescriptorAllocator* GetRtvHeap() {
        return rtv_heap_cpu.get();
    }
    D3D12CpuDescriptorAllocator* GetDsvHeap() {
        return dsv_heap_cpu.get();
    }
    D3D12CpuDescriptorAllocator* GetSampleHeap() {
        return sampler_heap_cpu.get();
    }
    D3D12GpuDescriptorAllocator* GetCsuHeapGpuDyn() {
        return csu_heap_gpu.get();
    }
    D3D12GpuDescriptorAllocator* GetSamplerHeapGpuDyn() {
        return sampler_heap_gpu.get();
    }

public:
    D3D12_GPU_DESCRIPTOR_HANDLE PushCsuDescriptor(std::span<const DescriptorIndex> _index_in_cpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE PushSamplerDescriptor(std::span<const DescriptorIndex> _index_in_cpu_heap);

    // template<typename... T>
    //     requires(std::same_as<T, DescriptorIndex> && ...) && (sizeof...(T) > 0)
    // D3D12_GPU_DESCRIPTOR_HANDLE PushCsuDescriptor(T... _index_in_cpu_heap) {
    //     constexpr uint        count = sizeof...(_index_in_cpu_heap);
    //     const DescriptorIndex start = csu_heap_gpu->Allocate(count);

    //     [start = start.index]<size_t... N>(auto&& index_tuple, std::index_sequence<N...>) {
    //         (device->CopyDescriptorsSimple(1, csu_heap_gpu->GetOffsetHandleCpu({start + uint(N)}), csu_heap_cpu->GetOffsetHandleCpu(std::get<N>(index_tuple)), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), ...);
    //     }(std::forward_as_tuple(_index_in_cpu_heap...), std::make_index_sequence<count>());

    //     return csu_heap_gpu->GetOffsetHandleGpu(start);
    // }

    // template<typename... T>
    //     requires(std::same_as<T, DescriptorIndex> && ...) && (sizeof...(T) > 0)
    // D3D12_GPU_DESCRIPTOR_HANDLE PushSapmlerDescriptor(T... _index_in_cpu_heap) {
    //     constexpr uint        count = sizeof...(_index_in_cpu_heap);
    //     const DescriptorIndex start = sampler_heap_gpu->Allocate(count);

    //     [start = start.index]<size_t... N>(auto&& index_tuple, std::index_sequence<N...>) {
    //         (device->CopyDescriptorsSimple(1, sampler_heap_gpu->GetOffsetHandleCpu({start + uint(N)}), sampler_heap_cpu->GetOffsetHandleCpu(std::get<N>(index_tuple)), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER), ...);
    //     }(std::forward_as_tuple(_index_in_cpu_heap...), std::make_index_sequence<count>());

    //     return sampler_heap_gpu->GetOffsetHandleGpu(start);
    // }

public:
    static constexpr uint bindless_sampler_cnt = 256;
    static constexpr uint cmd_alloc_limits     = 3;
    //UniquePtr<DeviceInternalShaders> internal_shaders;

private:
    //friend VkCommandQueue;
};

} // namespace Moer::Render

#endif // D3D12_DEVICE_H
