#ifndef MOER_RHI_CMD_REORDERER_H
#define MOER_RHI_CMD_REORDERER_H

#include "../RHIImpl.h"
#include "VulkanRHITrace.h"
#include "misc/Hash.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include <format>
#include <limits>
#include <type_traits>
#include <variant>
/**
 * @brief Copy From Luisa Runtime(LC) src/backends/common/command_reorder_visitor.h with respect
 * 
 */
namespace Moer::Render {

struct FunctionTable {
    using IsResourceWrite      = bool (*)(uint64 _flag);
    using IsResourceRead       = bool (*)(uint64 _flag);
    using IsTextureSampled     = bool (*)(uint64 _flag);
    using IsResourceInBindless = bool (*)(uint64 _resource, uint64 _bdls_handle);
    using LockBdlsArray        = void (*)(uint64 _bdls_handle);
    using UnlockBdlsArray      = LockBdlsArray;

    IsResourceWrite      is_resource_write;
    IsResourceRead       is_resource_read;
    IsTextureSampled     is_texture_sampled;
    IsResourceInBindless is_resource_in_bindless;

    LockBdlsArray   lock_bdls_array;
    UnlockBdlsArray unlock_bdls_array;
};
struct ArenaAllocator {
    struct LinkedChunk {
        LinkedChunk* next = nullptr;
        byte*        data = nullptr;
    };
    ArenaAllocator(uint64 _size) : capacity(_size) {
        head       = reinterpret_cast<LinkedChunk*>(Memory::Malloc(sizeof(LinkedChunk)));
        head->data = reinterpret_cast<byte*>(Memory::Malloc(_size));
        head->next = nullptr;
    }
    ArenaAllocator(const ArenaAllocator&)            = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    ArenaAllocator(ArenaAllocator&& _other) noexcept {
        head            = _other.head;
        capacity        = _other.capacity;
        _other.head     = nullptr;
        _other.capacity = 0;
    }
    ArenaAllocator& operator=(ArenaAllocator&& _other) noexcept {
        if (this != &_other) {
            this->~ArenaAllocator();
        }
        head            = _other.head;
        offset          = _other.offset;
        capacity        = _other.capacity;
        _other.head     = nullptr;
        _other.capacity = 0;
        _other.offset   = 0;
        return *this;
    }

    ~ArenaAllocator() {
        LinkedChunk* iter = head;
        while (iter) {
            LinkedChunk* next = iter->next;
            Memory::Free(iter->data);
            Memory::Free(iter);
            iter = next;
        }
        head   = nullptr;
        offset = 0;
    }
    void Expand() {
        LinkedChunk* new_chunk = reinterpret_cast<LinkedChunk*>(Memory::Malloc(sizeof(LinkedChunk)));
        new_chunk->data        = reinterpret_cast<byte*>(Memory::Malloc(capacity));
        new_chunk->next        = head;
        head                   = new_chunk;
        offset                 = 0;
    }
    void* Malloc(uint64 _size) {
        if (offset + _size > capacity) {
            assert(capacity >= _size && "Invalid Size");
            Expand();
        }
        void* ptr = head->data + offset;
        offset += _size;
        return ptr;
    }

    template<typename T>
    T* Malloc() {
        return reinterpret_cast<T*>(Malloc(sizeof(T)));
    }

    //byte*  data     = nullptr;
    uint64       offset   = 0;
    uint64       capacity = 0;
    LinkedChunk* head;
};

template<class T>
class ArenaAllocatorWrapper {
public:
    ArenaAllocatorWrapper(ArenaAllocator& a) : _alloc{a} {}

    template<class U>
    ArenaAllocatorWrapper(const ArenaAllocatorWrapper<U>& rhs) :
        _alloc{const_cast<ArenaAllocatorWrapper<U>&>(rhs).allocator()} {}

    using type  = ArenaAllocatorWrapper<T>;
    using other = ArenaAllocatorWrapper<T>;

    using value_type                             = T;
    using size_type                              = std::size_t;
    using difference_type                        = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal                        = std::true_type;

    template<class U>
    using rebind = ArenaAllocatorWrapper<U>;

    T* allocate(std::size_t n) {
        return reinterpret_cast<T*>(_alloc.Malloc(n * sizeof(T)));
    }

    constexpr void deallocate(T* p, std::size_t n) {
        return;
    }

    ArenaAllocator& allocator() const {
        return _alloc;
    }

private:
    ArenaAllocator& _alloc;
};
class CmdReorderer {

public:
    CmdReorderer(FunctionTable _funcs, const TCachedArgArray& _arg) :
        m_arena(65556),
        m_arena_stl(m_arena),
        m_funcs(_funcs),
        m_cached_arg_refs(_arg) {}
    ~CmdReorderer() {}
    enum class ResourceRW : uint8 {
        Read,
        Write
    };
    enum class ResourceType : uint8 {
        Texture_Buffer,
        Mesh,
        Bindless,
        Accel
    };
    struct ResourceHandle {
        uint64       handle;
        ResourceType type;
    };

    struct Range {
        int64 primary_min;
        int64 primary_max;
        int64 layer_min;
        int64 layer_max;
        Range() :
            primary_min(std::numeric_limits<int64>::min()),
            primary_max(std::numeric_limits<int64>::max()),
            layer_min(std::numeric_limits<int64>::min()),
            layer_max(std::numeric_limits<int64>::max()) {}
        Range(int64 _val) :
            primary_min(_val),
            primary_max(_val + 1),
            layer_min(std::numeric_limits<int64>::min()),
            layer_max(std::numeric_limits<int64>::max()) {}
        Range(int64 _min, int64 _size) :
            primary_min(_min),
            primary_max(_min + _size),
            layer_min(std::numeric_limits<int64>::min()),
            layer_max(std::numeric_limits<int64>::max()) {}
        Range(int64 _min, int64 _size, int64 _layer_min, int64 _layer_size) :
            primary_min(_min),
            primary_max(_min + _size),
            layer_min(_layer_min),
            layer_max(_layer_min + _layer_size) {}
        bool Colide(const Range& _other) const {
            return primary_min < _other.primary_max && _other.primary_min < primary_max &&
                   layer_min < _other.layer_max && _other.layer_min < layer_max;
        }
        bool operator==(const Range& _other) const {
            return primary_min == _other.primary_min && primary_max == _other.primary_max &&
                   layer_min == _other.layer_min && layer_max == _other.layer_max;
        }
        bool operator!=(const Range& _other) const {
            return !operator==(_other);
        }
    };

    struct RangeHash {
        size_t operator()(const Range& _range) const {
            uint64 hash = 0;
            HashCombine(hash, GetHash(_range.primary_min));
            HashCombine(hash, GetHash(_range.primary_max));
            HashCombine(hash, GetHash(_range.layer_min));
            HashCombine(hash, GetHash(_range.layer_max));
            return hash;
        }
    };

    //command layer resource view
    struct ResourceView {
        int64 read_layer{-1};
        int64 write_layer{-1};
    };

    struct RangeHandle : public ResourceHandle {
        using Map = UnorderedMap<
            Range,
            ResourceView,
            RangeHash,
            std::equal_to<Range>,
            ArenaAllocatorWrapper<std::pair<const Range, ResourceView>>>;

    private:
        ResourceView          max_view{-1, -1};
        Range                 read_range;
        Range                 write_range;
        Map                   range2view;
        static constexpr uint max_range_size = 16;

    public:
        RangeHandle(const ArenaAllocatorWrapper<ResourceView>& _alloc) :
            read_range(
                std::numeric_limits<int64>::max(),
                std::numeric_limits<int64>::min(),
                std::numeric_limits<int64>::max(),
                std::numeric_limits<int64>::min()
            ),
            write_range(
                std::numeric_limits<int64>::max(),
                std::numeric_limits<int64>::min(),
                std::numeric_limits<int64>::max(),
                std::numeric_limits<int64>::min()
            ),
            range2view(max_range_size, _alloc) {}

        int64 GetMaxReadLayer(const Range& _range) {

            int64 max_layer = -1;
            if (!_range.Colide(read_range)) {
                return max_layer;
            }
            for (auto& [range, view] : range2view) {
                if (range.Colide(_range)) {
                    max_layer = std::max(max_layer, view.read_layer);
                    if (max_layer >= max_view.read_layer) {
                        return max_layer;
                    }
                }
            }
            return max_layer;
        }

        int64 GetMaxWriteLayer(const Range& _range) {
            int64 max_layer = -1;
            if (!_range.Colide(write_range)) {
                return max_layer;
            }
            for (auto& [range, view] : range2view) {
                if (range.Colide(_range)) {
                    max_layer = std::max(max_layer, view.write_layer);
                    if (max_layer >= max_view.write_layer) {
                        return max_layer;
                    }
                }
            }
            return max_layer;
        }

        void ClearLayerViews() {
            range2view.clear();
            auto  iter        = range2view.try_emplace(read_range);
            auto& value       = iter.first->second;
            value.read_layer  = max_view.read_layer;
            value.write_layer = max_view.write_layer;
        }

        void EmplaceReadLayer(const Range& _range, int64 _layer) {

            read_range.primary_min = std::min(read_range.primary_min, _range.primary_min);
            read_range.primary_max = std::max(read_range.primary_max, _range.primary_max);
            read_range.layer_min   = std::min(read_range.layer_min, _range.layer_min);
            read_range.layer_max   = std::max(read_range.layer_max, _range.layer_max);

            max_view.read_layer = std::max(max_view.read_layer, _layer);
            if (range2view.size() >= max_range_size) {
                ClearLayerViews();
            } else {
                auto  iter  = range2view.try_emplace(_range);
                auto& value = iter.first->second.read_layer;
                if (iter.second) {
                    value = _layer;
                } else {
                    value = std::max(value, _layer);
                }
            }
        }

        void EmplaceWriteLayer(const Range& _range, int64 _layer) {
            read_range.primary_min  = std::min(read_range.primary_min, _range.primary_min);
            read_range.primary_max  = std::max(read_range.primary_max, _range.primary_max);
            read_range.layer_min    = std::min(read_range.layer_min, _range.layer_min);
            read_range.layer_max    = std::max(read_range.layer_max, _range.layer_max);
            write_range.primary_min = std::min(write_range.primary_min, _range.primary_min);
            write_range.primary_max = std::max(write_range.primary_max, _range.primary_max);
            write_range.layer_min   = std::min(write_range.layer_min, _range.layer_min);
            write_range.layer_max   = std::max(write_range.layer_max, _range.layer_max);
            max_view.read_layer     = std::max(max_view.read_layer, _layer);
            max_view.write_layer    = std::max(max_view.write_layer, _layer);
            if (range2view.size() >= max_range_size) {
                ClearLayerViews();
            } else {
                auto  iter        = range2view.try_emplace(_range);
                auto& write_layer = iter.first->second.write_layer;
                auto& read_layer  = iter.first->second.read_layer;
                if (iter.second) {
                    write_layer = _layer;
                    read_layer  = _layer;
                } else {
                    write_layer = std::max(write_layer, _layer);
                    read_layer  = std::max(read_layer, _layer);
                }
            }
        }
    };

    struct NoRangeHandle : public ResourceHandle {
        ResourceView view{-1, -1};
    };
    struct BindlessHandle : public ResourceHandle {
        ResourceView view{-1, -1};
    };
    struct CommandListNode {
        Command const*         cmd;
        CommandListNode const* next;
    };
    struct LinkedCommandList {
        CommandListNode const* head = nullptr;
        CommandListNode*       tail = nullptr;
    };
    Array<LinkedCommandList> m_cmd_lists;

    UnorderedMap<uint64, RangeHandle*>    m_range_handles;
    UnorderedMap<uint64, NoRangeHandle*>  m_no_range_handles;
    UnorderedMap<uint64, BindlessHandle*> m_bindless_handles;

    UnorderedSet<uint64> m_write_resources;
    UnorderedSet<uint64> m_writed_geometry;

    //temporal resources
    Array<std::tuple<Range, ResourceHandle*>> m_arg_read_resources;
    Array<std::tuple<Range, ResourceHandle*>> m_arg_write_resources;
    UnorderedSet<uint64>                      temp_writed_resources;

    int64 m_dispatch_layer = -1;
    int64 m_max_bdls_layer = -1; //optimization

    ArenaAllocator                      m_arena;
    ArenaAllocatorWrapper<ResourceView> m_arena_stl;
    FunctionTable                       m_funcs;

    const TCachedArgArray& m_cached_arg_refs;

    int64 layer_offset = -1;

public:
    ResourceHandle* GetHandle(uint64 _handle, ResourceType _type) {
        auto func_emplace = [&](auto& _map) {
            auto   iter  = _map.try_emplace(_handle);
            auto&& value = iter.first->second;
            using TValue = std::remove_pointer_t<std::remove_reference_t<decltype(value)>>;
            if (iter.second) {
                TValue* value_ptr = m_arena.Malloc<TValue>();
                value             = value_ptr;
                if constexpr (std::is_same_v<TValue, RangeHandle>) {
                    new (value) TValue(m_arena_stl);
                } else {
                    new (value) TValue();
                }
                value->handle = _handle;
                value->type   = _type;
            }
            return value;
        };

        switch (_type) {
            case ResourceType::Texture_Buffer:
                return func_emplace(m_range_handles);
            case ResourceType::Mesh:
            case ResourceType::Bindless:
                return func_emplace(m_bindless_handles);
            case ResourceType::Accel:
                return func_emplace(m_no_range_handles);
            default: {
                return func_emplace(m_range_handles);
            }
        }
    }

    int64 GetLayerWithOffset(int64 _layer) {
        return std::max(_layer, (int64)layer_offset);
    }

    static const char* ResourceTypeName(ResourceType _type) {
        switch (_type) {
            case ResourceType::Texture_Buffer:
                return "TextureBuffer";
            case ResourceType::Mesh:
                return "Mesh";
            case ResourceType::Bindless:
                return "Bindless";
            case ResourceType::Accel:
                return "Accel";
            default:
                return "Unknown";
        }
    }

    static std::string_view ResourceName(const ResourceHandle* _handle) {
        if (_handle == nullptr) {
            return "<null>";
        }
        return _handle->type == ResourceType::Texture_Buffer ? "<texture-or-buffer>" : "<non-resource>";
    }

    void TraceResourceLayer(
        std::string_view _op,
        const ResourceHandle* _handle,
        const Range& _range,
        int64 _layer
    ) const {
        RHITRACE_LOG(
            verbose,
            "[RHITrace][Reorder][{}] resource_type={} name={} handle=0x{:x} layer={} range=({},{}) layer_range=({},{})",
            _op,
            ResourceTypeName(_handle != nullptr ? _handle->type : ResourceType::Texture_Buffer),
            ResourceName(_handle),
            _handle != nullptr ? _handle->handle : 0ull,
            _layer,
            _range.primary_min,
            _range.primary_max,
            _range.layer_min,
            _range.layer_max
        );
    }

    int64 GetLastLayerWrite(RangeHandle* _handle, const Range& _range) {
        int64 layer = _handle->GetMaxReadLayer(_range);
        if (m_max_bdls_layer >= layer) {
            //check contains certain resource
            for (auto&& i : m_bindless_handles) {
                m_funcs.lock_bdls_array(i.first);
                if (m_funcs.is_resource_in_bindless(_handle->handle, i.first)) {
                    layer = std::max(layer, i.second->view.read_layer);
                }
                m_funcs.unlock_bdls_array(i.first);
            }
        }
        return GetLayerWithOffset(layer + 1);
    }

    int64 GetLastLayerWrite(NoRangeHandle* _handle) {
        int64 layer = std::max(_handle->view.read_layer, _handle->view.write_layer);
        //todo: specific layer need to be take cared, like mesh blas build or tlas build
        return GetLayerWithOffset(layer + 1);
    }

    int64 GetLastLayerWrite(BindlessHandle* _handle) {
        int64 layer = std::max(_handle->view.read_layer, _handle->view.write_layer);
        return GetLayerWithOffset(layer + 1);
    }

    int64 GetLastLayerRead(RangeHandle* _handle, const Range& _range) {
        int64 layer = _handle->GetMaxWriteLayer(_range);
        return GetLayerWithOffset(layer + 1);
    }

    int64 GetLastLayerRead(NoRangeHandle* _handle) {
        int64 layer = _handle->view.read_layer;
        return GetLayerWithOffset(layer + 1);
    }

    int64 GetLastLayerRead(BindlessHandle* _handle) {
        int64 layer = _handle->view.write_layer + 1;
        return GetLayerWithOffset(layer + 1);
    }

    void AddCmd(Command const* _cmd, uint64 _layer) {
        if (m_cmd_lists.size() <= _layer) {
            m_cmd_lists.resize(_layer + 1);
        }
        auto&            last = m_cmd_lists[_layer];
        CommandListNode* ptr  = m_arena.Malloc<CommandListNode>();
        new (ptr) CommandListNode(_cmd, nullptr);
        last.head = last.head ? last.head : ptr;
        if (last.tail) {
            last.tail->next = ptr;
        }
        m_cmd_lists[_layer].tail = ptr;
        RHITRACE_LOG(
            verbose,
            "[RHITrace][Reorder][AssignCmd] layer={} cmd_type={} name={}",
            _layer,
            uint(_cmd->Type()),
            _cmd->name
        );
    }

    int64 SetRead(ResourceHandle* _handle, const Range& _range) {
        int64 layer = 0;
        switch (_handle->type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle            = static_cast<BindlessHandle*>(_handle);
                layer                            = GetLastLayerRead(bindless_handle);
                bindless_handle->view.read_layer = layer;
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                layer                            = GetLastLayerRead(no_range_handle);
                no_range_handle->view.read_layer = layer;
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(_handle);
                layer              = GetLastLayerRead(range_handle, _range);
                range_handle->EmplaceReadLayer(_range, layer);
            }
        }
        TraceResourceLayer("Read", _handle, _range, layer);
        return layer;
    }

    int64 SetRead(uint64 _handle, const Range& _range, ResourceType _type) {
        auto* handle = GetHandle(_handle, _type);
        return SetRead(handle, _range);
    }

    void RecordRead(ResourceHandle* _handle, Range _range, int64 _layer) {
        switch (_handle->type) {

            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle            = static_cast<BindlessHandle*>(_handle);
                bindless_handle->view.read_layer = std::max(bindless_handle->view.read_layer, _layer);
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                no_range_handle->view.read_layer = _layer;
                break;
            }
            default: {

                auto* range_handle = static_cast<RangeHandle*>(_handle);
                range_handle->EmplaceReadLayer(_range, _layer);
            }
        }
        TraceResourceLayer("RecordRead", _handle, _range, _layer);
    }

    void RecordWrite(ResourceHandle* _handle, Range _range, int64 _layer) {
        switch (_handle->type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle             = static_cast<BindlessHandle*>(_handle);
                bindless_handle->view.write_layer = _layer;
                bindless_handle->view.read_layer  = _layer;
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                no_range_handle->view.write_layer = _layer;
                no_range_handle->view.read_layer  = _layer;
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(_handle);
                range_handle->EmplaceWriteLayer(_range, _layer);
            }
        }
        TraceResourceLayer("RecordWrite", _handle, _range, _layer);
    }

    int64 SetWrite(ResourceHandle* _handle, const Range& _range) {
        int64 layer = 0;
        switch (_handle->type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle             = static_cast<BindlessHandle*>(_handle);
                layer                             = GetLastLayerWrite(bindless_handle);
                bindless_handle->view.write_layer = layer;
                bindless_handle->view.read_layer  = layer;
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                layer                             = GetLastLayerWrite(no_range_handle);
                no_range_handle->view.write_layer = layer;
                no_range_handle->view.read_layer  = layer;
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(_handle);
                layer              = GetLastLayerWrite(range_handle, _range);
                range_handle->EmplaceWriteLayer(_range, layer);
                m_write_resources.emplace(_handle->handle);
            }
        }
        TraceResourceLayer("Write", _handle, _range, layer);
        return layer;
    }

    int64 SetWrite(uint64 _handle, const Range& _range, ResourceType _type) {
        auto* handle = GetHandle(_handle, _type);
        return SetWrite(handle, _range);
    }

    int64 SetRW(
        uint64       _handle,
        Range        _read_range,
        ResourceType _read_type,
        uint64       _write_handle,
        Range        _write_range,
        ResourceType _write_type
    ) {
        int64 layer        = 0;
        auto* read_handle  = GetHandle(_handle, _read_type);
        auto* write_handle = GetHandle(_write_handle, _write_type);
        switch (_read_type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle = static_cast<BindlessHandle*>(read_handle);
                layer                 = GetLastLayerRead(bindless_handle);
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle = static_cast<NoRangeHandle*>(read_handle);
                layer                 = GetLastLayerRead(no_range_handle);
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(read_handle);
                layer              = GetLastLayerRead(range_handle, _read_range);
            }
        }

        switch (_write_type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless:
            case ResourceType::Accel: {
                auto* no_range_handle             = static_cast<NoRangeHandle*>(write_handle);
                layer                             = std::max(layer, GetLastLayerWrite(no_range_handle));
                no_range_handle->view.write_layer = layer;
                no_range_handle->view.read_layer  = layer;
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(write_handle);
                layer              = std::max(GetLastLayerWrite(range_handle, _write_range), layer);
                range_handle->EmplaceWriteLayer(_write_range, layer);
                m_write_resources.emplace(range_handle->handle);
            }
        }

        //now set read
        switch (_read_type) {
            case ResourceType::Mesh:
            case ResourceType::Bindless: {
                auto* bindless_handle            = static_cast<BindlessHandle*>(read_handle);
                bindless_handle->view.read_layer = std::max(bindless_handle->view.read_layer, layer);
                break;
            }
            case ResourceType::Accel: {
                auto* no_range_handle            = static_cast<NoRangeHandle*>(read_handle);
                no_range_handle->view.read_layer = std::max(no_range_handle->view.read_layer, layer);
                break;
            }
            default: {
                auto* range_handle = static_cast<RangeHandle*>(read_handle);
                range_handle->EmplaceReadLayer(_read_range, layer);
            }
        }
        return layer;
    }

    void EmplaceArg(uint64 _handle, ResourceType _type, const Range& _range, bool _b_write) {
        ResourceHandle* handle = GetHandle(_handle, _type);
        if (_b_write) {
            switch (_type) {
                case ResourceType::Bindless: {
                    m_dispatch_layer =
                        std::max(m_dispatch_layer, GetLastLayerWrite(static_cast<BindlessHandle*>(handle)));
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Accel: {
                    m_dispatch_layer =
                        std::max(m_dispatch_layer, GetLastLayerWrite(static_cast<NoRangeHandle*>(handle)));
                    break;
                }
                default: {
                    m_dispatch_layer = std::max(
                        m_dispatch_layer, GetLastLayerWrite(static_cast<RangeHandle*>(handle), _range)
                    );
                }
            }
            m_arg_write_resources.emplace_back(_range, handle);
            temp_writed_resources.emplace(_handle);
        } else {
            switch (_type) {
                case ResourceType::Bindless: {
                    m_dispatch_layer =
                        std::max(m_dispatch_layer, GetLastLayerRead(static_cast<BindlessHandle*>(handle)));
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Accel: {
                    m_dispatch_layer =
                        std::max(m_dispatch_layer, GetLastLayerRead(static_cast<NoRangeHandle*>(handle)));
                    break;
                }
                default: {
                    m_dispatch_layer = std::max(
                        m_dispatch_layer, GetLastLayerRead(static_cast<RangeHandle*>(handle), _range)
                    );
                }
            }
            m_arg_read_resources.emplace_back(_range, handle);
        }
    }

    void VisitArgs(const TArg& _arg, uint64 _flag) {

        std::visit(
            [&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    bool b_write = m_funcs.is_resource_write(_flag);
                    EmplaceArg(
                        (uint64)(_arg.GetBuffer()),
                        ResourceType::Texture_Buffer,
                        Range(_arg.GetByteOffset(), _arg.GetByteSize()),
                        b_write
                    );
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    bool b_write = m_funcs.is_resource_write(_flag);
                    EmplaceArg(
                        (uint64)(_arg.GetTexture()),
                        ResourceType::Texture_Buffer,
                        Range(_arg.mip_level, _arg.num_mips, _arg.array_layer, _arg.num_array),
                        b_write
                    );
                } else if constexpr (std::is_same_v<T, std::span<TextureView>>) {
                    for (auto&& tex : _arg) {
                        EmplaceArg(
                            (uint64)(tex.GetTexture()),
                            ResourceType::Texture_Buffer,
                            Range(tex.mip_level, tex.num_mips, tex.array_layer, tex.num_array),
                            m_funcs.is_resource_write(_flag)
                        );
                    }
                } else if constexpr (std::is_same_v<T, std::span<BufferView>>) {
                    for (auto&& buf : _arg) {
                        EmplaceArg(
                            (uint64)(buf.GetBuffer()),
                            ResourceType::Texture_Buffer,
                            Range(buf.GetByteOffset(), buf.GetByteSize()),
                            m_funcs.is_resource_write(_flag)
                        );
                    }
                }

                else if constexpr (std::is_same_v<T, RaytracingTlasRef>) {
                    EmplaceArg((uint64)(_arg.Get()), ResourceType::Accel, Range{}, false);
                } else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    assert(false && "Not support iterate BindlessArrayRef with other args");
                }
            },
            _arg
        );
    }

    void VisitBindlessArg(BindlessArrayRef _bdls, const UnorderedSet<uint64>& _temp_write_resources) {
        m_funcs.lock_bdls_array((uint64)(_bdls.Get()));
        for (auto&& res : m_write_resources) {
            if (!_temp_write_resources.contains(res) &&
                m_funcs.is_resource_in_bindless(res, (uint64)(_bdls.Get()))) {
                EmplaceArg(res, ResourceType::Texture_Buffer, Range{}, false);
            }
        }
        m_funcs.unlock_bdls_array((uint64)(_bdls.Get()));
        //emplace self
        EmplaceArg((uint64)(_bdls.Get()), ResourceType::Bindless, Range{}, false);
    }

    void VisitCmd(const UploadBufferCmd* _cmd) {
        AddCmd(
            _cmd,
            SetWrite(_cmd->Handle(), Range(_cmd->Offset(), _cmd->ByteSize()), ResourceType::Texture_Buffer)
        );
    }

    void VisitCmd(const CopyBackBufferCmd* _cmd) {
        AddCmd(
            _cmd,
            SetRead(_cmd->Handle(), Range(_cmd->Offset(), _cmd->ByteSize()), ResourceType::Texture_Buffer)
        );
    }

    void VisitCmd(const CopyBackTextureCmd* _cmd) {
        AddCmd(_cmd, SetRead(_cmd->Handle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer));
    }

    void VisitCmd(const CopyBufferCmd* _cmd) {
        AddCmd(
            _cmd,
            SetRW(
                _cmd->SrcHandle(),
                Range(_cmd->SrcOffset(), _cmd->ByteSize()),
                ResourceType::Texture_Buffer,
                _cmd->DstHandle(),
                Range(_cmd->DstOffset(), _cmd->ByteSize()),
                ResourceType::Texture_Buffer
            )
        );
    }
    void VisitCmd(const CopyTextureCmd* _cmd) {
        AddCmd(
            _cmd,
            SetRW(
                _cmd->SrcHandle(),
                Range(_cmd->SrcMipLevel()),
                ResourceType::Texture_Buffer,
                _cmd->DstHandle(),
                Range(_cmd->DstMipLevel()),
                ResourceType::Texture_Buffer
            )
        );
    }
    void VisitCmd(const CopyBufferToTextureCmd* _cmd) {
        AddCmd(
            _cmd,
            SetRW(
                _cmd->SrcHandle(),
                Range(_cmd->SrcOffset(), _cmd->ByteSize()),
                ResourceType::Texture_Buffer,
                _cmd->DstHandle(),
                Range(_cmd->MipLevel()),
                ResourceType::Texture_Buffer
            )
        );
    }
    void VisitCmd(const CopyTextureToBufferCmd* _cmd) {
        AddCmd(
            _cmd,
            SetRW(
                _cmd->SrcHandle(),
                Range(_cmd->MipLevel()),
                ResourceType::Texture_Buffer,
                _cmd->DstHandle(),
                Range(_cmd->DstOffset(), _cmd->ByteSize()),
                ResourceType::Texture_Buffer
            )
        );
    }

    void VisitCmd(const UploadTextureCmd* _cmd) {
        AddCmd(_cmd, SetWrite(_cmd->Handle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer));
    }

    void VisitCmd(const BarrierCmd* _cmd) {
        int64               layer = 0;
        Array<RangeHandle*> barrier_resources;
        Array<Range>        barrier_ranges;
        //reserve
        barrier_resources.reserve(
            _cmd->ReadBuffers().size() + _cmd->ReadTextures().size() + _cmd->WriteBuffers().size() +
            _cmd->WriteTextures().size()
        );
        barrier_ranges.reserve(
            _cmd->ReadBuffers().size() + _cmd->ReadTextures().size() + _cmd->WriteBuffers().size() +
            _cmd->WriteTextures().size()
        );

        for (const auto& [handle, state, pass_type, offset, size] : _cmd->ReadBuffers()) {
            RangeHandle* range_handle =
                static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
            layer = GetLastLayerRead(range_handle, Range(offset, size));
        }
        for (const auto& [handle, state, pass_type, mip_level, mip_cnt] : _cmd->ReadTextures()) {
            RangeHandle* range_handle =
                static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
            layer = GetLastLayerRead(range_handle, Range(mip_level, mip_cnt));
        }

        for (auto& [handle, state, pass_type, offset, size] : _cmd->WriteBuffers()) {
            RangeHandle* range_handle =
                static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
            layer = GetLastLayerRead(range_handle, Range(offset, size));
            barrier_resources.emplace_back(range_handle);
            barrier_ranges.emplace_back(Range(offset, size));
        }

        for (const auto& [handle, state, pass_type, mip_level, mip_cnt] : _cmd->WriteTextures()) {
            RangeHandle* range_handle =
                static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
            layer = GetLastLayerRead(range_handle, Range(mip_level, mip_cnt));
            barrier_resources.emplace_back(range_handle);
            barrier_ranges.emplace_back(Range(mip_level, mip_cnt));
        }
        for (uint i = 0; i < barrier_resources.size(); ++i) {
            RangeHandle* range_handle = barrier_resources[i];
            Range        range        = barrier_ranges[i];
            range_handle->EmplaceWriteLayer(range, layer);
            m_write_resources.emplace(range_handle->handle);
        }

        AddCmd(_cmd, layer);
    }

    void VisitCmd(const QueueTransferCmd* _cmd) {
        int64               layer = 0;
        Array<RangeHandle*> barrier_resources;
        Array<Range>        barrier_ranges;
        //reserve
        if (_cmd->IsImport()) {
            barrier_resources.reserve(_cmd->ImportTextures().size());
            barrier_ranges.reserve(_cmd->ImportTextures().size());

            for (const auto& barrier : _cmd->ImportTextures()) {
                const auto& handle = barrier.texture;
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer)
                );
                layer = GetLastLayerRead(
                    range_handle,
                    Range(handle.mip_level, handle.num_mips, handle.array_layer, handle.num_array)
                );
                assert(
                    layer == 0 &&
                    std::format(
                        "Import Texture {} should be the first command", handle.GetTexture()->GetName()
                    )
                        .c_str()
                );
            }

            for (const auto& barrier : _cmd->ImportBuffers()) {
                const auto& handle = barrier.buffer;
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetBuffer()), ResourceType::Texture_Buffer)
                );
                layer = GetLastLayerRead(range_handle, Range(handle.GetByteOffset(), handle.GetByteSize()));
                assert(
                    layer == 0 &&
                    std::format("Import Buffer {} should be the first command", handle.GetBuffer()->GetName())
                        .c_str()
                );
            }

            for (const auto& barrier : _cmd->ImportTextures()) {
                const auto& handle = barrier.texture;
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer)
                );
                range_handle->EmplaceWriteLayer(
                    Range(handle.mip_level, handle.num_mips, handle.array_layer, handle.num_array), layer
                );
            }

            for (const auto& barrier : _cmd->ImportBuffers()) {
                const auto& handle = barrier.buffer;
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetBuffer()), ResourceType::Texture_Buffer)
                );
                range_handle->EmplaceWriteLayer(Range(handle.GetByteOffset(), handle.GetByteSize()), layer);
            }
        } else {
            barrier_resources.reserve(_cmd->ExportTextures().size());
            barrier_ranges.reserve(_cmd->ExportTextures().size());

            for (const auto& [handle, state] : _cmd->ExportTextures()) {
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer)
                );
                layer = GetLastLayerWrite(
                    range_handle,
                    Range(handle.mip_level, handle.num_mips, handle.array_layer, handle.num_array)
                );
            }

            for (const auto& [handle, state] : _cmd->ExportBuffers()) {

                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetBuffer()), ResourceType::Texture_Buffer)
                );
                layer = GetLastLayerWrite(range_handle, Range(handle.GetByteOffset(), handle.GetByteSize()));
            }

            ++layer;

            for (const auto& [handle, state] : _cmd->ExportTextures()) {
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer)
                );
                range_handle->EmplaceWriteLayer(
                    Range(handle.mip_level, handle.num_mips, handle.array_layer, handle.num_array), layer
                );
            }

            for (const auto& [handle, state] : _cmd->ExportBuffers()) {
                RangeHandle* range_handle = static_cast<RangeHandle*>(
                    GetHandle(uint64(handle.GetBuffer()), ResourceType::Texture_Buffer)
                );
                range_handle->EmplaceWriteLayer(Range(handle.GetByteOffset(), handle.GetByteSize()), layer);
            }
        }

        AddCmd(_cmd, layer);
    }

    void VisitCmd(const SetDrawStateCmd* _cmd) {
        int64 layer      = 0;
        bool  b_use_bdls = false;
        m_arg_read_resources.clear();
        m_arg_write_resources.clear();
        temp_writed_resources.clear();

        const auto& pipeline = _cmd->Pipeline();
        auto        func     = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags);
        };

        auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx)) {
                VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
                b_use_bdls = true;
            }
        };

        _cmd->IterateArgs(func, bdls_post_func);

        const auto& vbs = _cmd->VertexBuffers();
        for (const auto& vb : vbs) {
            EmplaceArg(
                (uint64)(vb.first),
                ResourceType::Texture_Buffer,
                Range(vb.second.min, vb.second.max - vb.second.min),
                false
            );
        }
        const auto& ibs = _cmd->IndexBuffers();
        for (const auto& ib : ibs) {
            EmplaceArg(
                (uint64)(ib.first),
                ResourceType::Texture_Buffer,
                Range(ib.second.min, ib.second.max - ib.second.min),
                false
            );
        }

        const auto& indirect = _cmd->IndirectBuffers();
        for (const auto& ind : indirect) {
            EmplaceArg(
                (uint64)(ind.first),
                ResourceType::Texture_Buffer,
                Range(ind.second.min, ind.second.max - ind.second.min),
                false
            );
        }

        const auto& count_buffers = _cmd->DrawCountBuffers();
        for (const auto& count : count_buffers) {
            EmplaceArg(
                (uint64)(count.first),
                ResourceType::Texture_Buffer,
                Range(count.second.min, count.second.max - count.second.min),
                false
            );
        }

        //depth and render targets
        const auto& pass_info = _cmd->RenderPassInfo();
        if (pass_info.depth_attachment.Valid()) {
            const auto& depth          = pass_info.depth_attachment;
            auto        depth_store_op = GetStoreOp(GetDepthAction(depth.action));
            if (GetLoadOp(GetDepthAction(depth.action)) == EAttachmentLoadOp::LOAD) {
                EmplaceArg(
                    (uint64)(depth.target),
                    ResourceType::Texture_Buffer,
                    Range(depth.mip_level, 1, depth.array_layer, 1),
                    false
                );
            }
            if (depth_store_op == EAttachmentStoreOp::STORE) {
                EmplaceArg(
                    (uint64)(depth.target),
                    ResourceType::Texture_Buffer,
                    Range(depth.mip_level, 1, depth.array_layer, 1),
                    true
                );
            }
        }
        for (const auto& target : pass_info.color_attachments) {
            auto color_store_op = GetStoreOp(target.action);
            if (GetLoadOp(target.action) == EAttachmentLoadOp::LOAD) {
                EmplaceArg(
                    (uint64)(target.target),
                    ResourceType::Texture_Buffer,
                    Range(target.mip_level, 1, target.array_layer, 1),
                    false
                );
            }
            if (color_store_op == EAttachmentStoreOp::STORE) {
                EmplaceArg(
                    (uint64)(target.target),
                    ResourceType::Texture_Buffer,
                    Range(target.mip_level, 1, target.array_layer, 1),
                    true
                );
            }
        }
        for (const auto& write_res : m_arg_write_resources) {
            RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
        }

        for (const auto& read_res : m_arg_read_resources) {
            RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
        }

        if (b_use_bdls) {
            m_max_bdls_layer = std::max(m_max_bdls_layer, m_dispatch_layer);
        }
        AddCmd(_cmd, m_dispatch_layer);
    }

    void VisitCmd(const MultiDrawCmd* _cmd) {
        bool b_use_bdls = false;
        m_arg_write_resources.clear();
        m_arg_read_resources.clear();
        temp_writed_resources.clear();

        TArg*                               bdls = nullptr;
        UnorderedSet<const ArrayArguments*> param_set;
        for (const auto& draw_cmd : _cmd->draw_batch.draw_cmds) {
            const auto& pipeline = draw_cmd.handle;

            auto func = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx))
                    VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags);
            };

            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx)) {
                    VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
                    b_use_bdls = true;
                }
            };
            const ArrayArguments* arg =
                std::holds_alternative<ArrayArguments>(draw_cmd.args) ?
                    &std::get<ArrayArguments>(draw_cmd.args) :
                    (std::holds_alternative<ArrayArgReference>(draw_cmd.args) ?
                         &m_cached_arg_refs[std::get<ArrayArgReference>(draw_cmd.args)()] :
                         nullptr);
            if (arg && param_set.emplace(arg).second) {
                IterateArgs(*arg, func, bdls_post_func);
            }
        }

        const auto& vbs = _cmd->VertexBuffers();
        for (const auto& vb : vbs) {
            EmplaceArg(
                (uint64)(vb.first),
                ResourceType::Texture_Buffer,
                Range(vb.second.min, vb.second.max - vb.second.min),
                false
            );
        }

        const auto& ibs = _cmd->IndexBuffers();
        for (const auto& ib : ibs) {
            EmplaceArg(
                (uint64)(ib.first),
                ResourceType::Texture_Buffer,
                Range(ib.second.min, ib.second.max - ib.second.min),
                false
            );
        }

        const auto& indirect = _cmd->IndirectBuffers();
        for (const auto& ind : indirect) {
            EmplaceArg(
                (uint64)(ind.first),
                ResourceType::Texture_Buffer,
                Range(ind.second.min, ind.second.max - ind.second.min),
                false
            );
        }
        //depth and render targets
        const auto& pass_info = _cmd->RenderPassInfo();
        if (pass_info.depth_attachment.Valid()) {
            const auto& depth          = pass_info.depth_attachment;
            auto        depth_store_op = GetStoreOp(GetDepthAction(depth.action));
            if (GetLoadOp(GetDepthAction(depth.action)) == EAttachmentLoadOp::LOAD) {
                EmplaceArg(
                    (uint64)(depth.target),
                    ResourceType::Texture_Buffer,
                    Range(depth.mip_level, 1, depth.array_layer, 1),
                    false
                );
            }
            if (depth_store_op == EAttachmentStoreOp::STORE) {
                EmplaceArg(
                    (uint64)(depth.target),
                    ResourceType::Texture_Buffer,
                    Range(depth.mip_level, 1, depth.array_layer, 1),
                    true
                );
            }
        }
        for (const auto& target : pass_info.color_attachments) {
            auto color_store_op = GetStoreOp(target.action);
            if (GetLoadOp(target.action) == EAttachmentLoadOp::LOAD) {
                EmplaceArg(
                    (uint64)(target.target),
                    ResourceType::Texture_Buffer,
                    Range(target.mip_level, 1, target.array_layer, 1),
                    false
                );
            }
            if (color_store_op == EAttachmentStoreOp::STORE) {
                EmplaceArg(
                    (uint64)(target.target),
                    ResourceType::Texture_Buffer,
                    Range(target.mip_level, 1, target.array_layer, 1),
                    true
                );
            }
        }

        for (const auto& write_res : m_arg_write_resources) {
            RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
        }
        for (const auto& read_res : m_arg_read_resources) {
            RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
        }
        if (b_use_bdls) {
            m_max_bdls_layer = std::max(m_max_bdls_layer, m_dispatch_layer);
        }
        AddCmd(_cmd, m_dispatch_layer);
    }

    // void VisitCmd(const SetGeometryPassDrawStateCmd* _cmd) {
    //     int64 layer = 0;
    //     m_arg_read_resources.clear();
    //     m_arg_write_resources.clear();
    //     temp_writed_resources.clear();

    //     auto func = [&](const TArg& _arg, uint _idx) {
    //         for (const auto& [bitmask, pso] : _cmd->PipelineMap()) {
    //             VisitArgs(_arg, pso.binding_infos[_idx].state_flags);
    //         }
    //     };

    //     auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
    //         VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
    //     };

    //     _cmd->IterateArgs(func, bdls_post_func);

    //     const auto& vbs = _cmd->VertexBuffers();
    //     for (const auto& vb : vbs) {
    //         EmplaceArg((uint64)(vb.first), ResourceType::Texture_Buffer, Range(vb.second.min, vb.second.max - vb.second.min), false);
    //     }
    //     const auto& ibs = _cmd->IndexBuffers();
    //     for (const auto& ib : ibs) {
    //         EmplaceArg((uint64)(ib.first), ResourceType::Texture_Buffer, Range(ib.second.min, ib.second.max - ib.second.min), false);
    //     }
    //     //depth and render targets
    //     const auto& pass_info = _cmd->RenderPassInfo();
    //     if (pass_info.depth_attachment.Valid()) {
    //         const auto& depth          = pass_info.depth_attachment;
    //         auto        depth_store_op = GetStoreOp(GetDepthAction(depth.action));
    //         if (GetLoadOp(GetDepthAction(depth.action)) == EAttachmentLoadOp::LOAD) {
    //             EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), false);
    //         }
    //         if (depth_store_op == EAttachmentStoreOp::STORE) {
    //             EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), true);
    //         }
    //     }
    //     for (const auto& target : pass_info.color_attachments) {
    //         auto color_store_op = GetStoreOp(target.action);
    //         if (GetLoadOp(target.action) == EAttachmentLoadOp::LOAD) {
    //             EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), false);
    //         }
    //         if (color_store_op == EAttachmentStoreOp::STORE) {
    //             EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), true);
    //         }
    //     }
    //     for (const auto& write_res : m_arg_write_resources) {
    //         RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
    //     }

    //     for (const auto& read_res : m_arg_read_resources) {
    //         RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
    //     }
    //     AddCmd(_cmd, m_dispatch_layer);
    // }

    void VisitCmd(const UpdateBindlessArrayCmd* _cmd) {
        //TODO: important here
        AddCmd(_cmd, SetWrite((uint64)(_cmd->Handle()), Range(), ResourceType::Bindless));
    }

    void VisitCmd(const ClearResourceCmd* _cmd) {
        std::visit(
            [&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    AddCmd(
                        _cmd,
                        SetWrite(
                            (uint64)(_arg.GetBuffer()),
                            Range(_arg.GetByteOffset(), _arg.GetByteSize()),
                            ResourceType::Texture_Buffer
                        )
                    );
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    AddCmd(
                        _cmd,
                        SetWrite(
                            (uint64)(_arg.GetTexture()),
                            Range(_arg.mip_level, _arg.num_mips),
                            ResourceType::Texture_Buffer
                        )
                    );
                }
            },
            _cmd->Resource()
        );
    }

    void VisitCmd(const DispatchCmd* _cmd) {
        bool b_use_bdls = false;
        m_arg_write_resources.clear();
        m_arg_read_resources.clear();
        temp_writed_resources.clear();

        const auto& pipeline = _cmd->Pipeline();

        auto func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags);
        };

        auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx)) {
                VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
                b_use_bdls = true;
            }
        };

        IterateArgs(_cmd->Args(m_cached_arg_refs), func, bdls_post_func);

        for (const auto& write_res : m_arg_write_resources) {
            RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
        }
        for (const auto& read_res : m_arg_read_resources) {
            RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
        }
        if (b_use_bdls) {
            m_max_bdls_layer = std::max(m_max_bdls_layer, m_dispatch_layer);
        }
        AddCmd(_cmd, m_dispatch_layer);
    }

    void VisitCmd(const BuildAccelerationStructuresCmd* _cmd) {
        int64 layer = 0;
        for (const auto& cmd : _cmd->Params()) {
            layer = std::max(
                layer,
                GetLastLayerWrite(
                    static_cast<NoRangeHandle*>(GetHandle((uint64)cmd.geometry.Get(), ResourceType::Accel))
                )
            );
            m_writed_geometry.emplace((uint64)cmd.geometry.Get());
        }

        for (const auto& vtx : _cmd->VtxBuffers()) {
            auto* vtx_range = static_cast<RangeHandle*>(GetHandle((uint64)vtx, ResourceType::Texture_Buffer));
            layer           = std::max(layer, GetLastLayerRead(vtx_range, Range(0, vtx->GetByteSize())));
        }

        for (const auto& idx : _cmd->IdxBuffers()) {
            auto* idx_range = static_cast<RangeHandle*>(GetHandle((uint64)idx, ResourceType::Texture_Buffer));
            layer           = std::max(layer, GetLastLayerRead(idx_range, Range(0, idx->GetByteSize())));
        }

        // Record Reads and Writes
        for (const auto& cmd : _cmd->Params()) {
            RecordWrite(GetHandle((uint64)cmd.geometry.Get(), ResourceType::Accel), Range{}, layer);
        }
        for (const auto& vtx : _cmd->VtxBuffers()) {
            auto* vtx_range = static_cast<RangeHandle*>(GetHandle((uint64)vtx, ResourceType::Texture_Buffer));
            RecordRead(vtx_range, Range(0, vtx->GetByteSize()), layer);
        }
        for (const auto& idx : _cmd->IdxBuffers()) {
            auto* idx_range = static_cast<RangeHandle*>(GetHandle((uint64)idx, ResourceType::Texture_Buffer));
            RecordRead(idx_range, Range(0, idx->GetByteSize()), layer);
        }

        for (const auto& vtx_buf : _cmd->VtxBuffers()) {
            auto* vtx_handle =
                static_cast<RangeHandle*>(GetHandle((uint64)vtx_buf, ResourceType::Texture_Buffer));
            //record read
            RecordRead(vtx_handle, Range(0, vtx_buf->GetByteSize()), layer);
        }
        for (const auto& idx_buf : _cmd->IdxBuffers()) {
            auto* idx_handle =
                static_cast<RangeHandle*>(GetHandle((uint64)idx_buf, ResourceType::Texture_Buffer));
            //record read
            RecordRead(idx_handle, Range(0, idx_buf->GetByteSize()), layer);
        }

        AddCmd(_cmd, layer);
    }

    void VisitCmd(const UpdateRaytracingSceneCmd* _cmd) {
        if (_cmd->InstancesToUpdate().size() == 0 && !_cmd->ForceUpdate()) {
            return;
        }
        const bool track_all_written_geometries =
            _cmd->ForceUpdate() || _cmd->RelatedGeometries().empty();
        int64 layer = 0;
        auto* tlas_handle =
            static_cast<NoRangeHandle*>(GetHandle((uint64)_cmd->TlasHandle(), ResourceType::Accel));

        layer = GetLastLayerWrite(tlas_handle);

        for (const uint64& handle : m_writed_geometry) {
            if (track_all_written_geometries || _cmd->HasGeometry(handle)) {
                auto* geo_handle = GetHandle((uint64)handle, ResourceType::Accel);
                layer            = std::max(layer, GetLastLayerRead(static_cast<NoRangeHandle*>(geo_handle)));
            }
        }

        //set read and write
        for (const uint64& handle : m_writed_geometry) {
            if (track_all_written_geometries || _cmd->HasGeometry(handle)) {
                auto* geo_handle            = (NoRangeHandle*)GetHandle((uint64)handle, ResourceType::Accel);
                geo_handle->view.read_layer = layer;

                RecordRead(geo_handle, Range{}, layer);
            }
        }

        RecordWrite(tlas_handle, Range{}, layer);

        AddCmd(_cmd, layer);
    }

    void VisitCmd(const ScopeCmd* _cmd) {
        layer_offset = m_cmd_lists.size();
        AddCmd(_cmd, layer_offset);
    }

    void VisitCmd(const QueryCmd* _cmd) {
        layer_offset = m_cmd_lists.size();
        AddCmd(_cmd, layer_offset);
    }

    void VisitCmd(const CustomCmd* _cmd) {
        switch (_cmd->CustomId()) {
            case CustomCmd::CustomCmdId::CUSTOM_RASTER:
                assert(false && "Custom raster draw scene not implemented");
                break;
            case CustomCmd::CustomCmdId::CUSTOM_DISPATCH:
                VisitCmd(static_cast<const CustomDispatchCmd*>(_cmd));
                break;
            default:
                assert(false && "Custom Command Not Supported for Reorder");
        }
    }

    void VisitCmd(const CustomDispatchCmd* _cmd) {
        m_arg_write_resources.clear();
        m_arg_read_resources.clear();
        layer_offset = m_cmd_lists.size(); // make sure the custom dispatch command is in a separate scope
        auto func    = [&](const TArg& _arg, ParamInfoFlags _flag) {
            VisitArgs(_arg, _flag.state_flags);
        };
        _cmd->IterateArgs(func);

        for (const auto& write_res : m_arg_write_resources) {
            RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
        }
        for (const auto& read_res : m_arg_read_resources) {
            RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
        }
        AddCmd(_cmd, m_dispatch_layer);
        layer_offset = m_cmd_lists.size();
    }

    void AcceptCmd(const Command* _cmd) {
        assert(_cmd && "Invalid Command");
        switch (_cmd->Type()) {
            case Command::EType::UploadBuffer:
                VisitCmd(static_cast<const UploadBufferCmd*>(_cmd));
                break;
            case Command::EType::CopyBackBuffer:
                VisitCmd(static_cast<const CopyBackBufferCmd*>(_cmd));
                break;
            case Command::EType::CopyBackTexture:
                VisitCmd(static_cast<const CopyBackTextureCmd*>(_cmd));
                break;
            case Command::EType::BufferToBuffer:
                VisitCmd(static_cast<const CopyBufferCmd*>(_cmd));
                break;
            case Command::EType::BufferToTexture:
                VisitCmd(static_cast<const CopyBufferToTextureCmd*>(_cmd));
                break;
            case Command::EType::TextureToBuffer:
                VisitCmd(static_cast<const CopyTextureToBufferCmd*>(_cmd));
                break;
            case Command::EType::UploadTexture:
                VisitCmd(static_cast<const UploadTextureCmd*>(_cmd));
                break;
            case Command::EType::TextureToTexture:
                VisitCmd(static_cast<const CopyTextureCmd*>(_cmd));
                break;
            case Command::EType::ShaderDispatch:
                VisitCmd(static_cast<const DispatchCmd*>(_cmd));
                break;
            case Command::EType::Barrier:
                VisitCmd(static_cast<const BarrierCmd*>(_cmd));
                break;
            case Command::EType::QueueTransfer:
                VisitCmd(static_cast<const QueueTransferCmd*>(_cmd));
                break;
            case Command::EType::SetDrawState:
                VisitCmd(static_cast<const SetDrawStateCmd*>(_cmd));
                break;
            case Command::EType::MultiDraw:
                VisitCmd(static_cast<const MultiDrawCmd*>(_cmd));
                break;
            // case Command::EType::SetGeometryPassDrawState:
            //     VisitCmd(static_cast<const SetGeometryPassDrawStateCmd*>(_cmd));
            //     break;
            case Command::EType::UpdateBindlessArray:
                VisitCmd(static_cast<const UpdateBindlessArrayCmd*>(_cmd));
                break;
            case Command::EType::BuildAccel:
                VisitCmd(static_cast<const BuildAccelerationStructuresCmd*>(_cmd));
                break;
            case Command::EType::BuildTLAS:
                VisitCmd(static_cast<const UpdateRaytracingSceneCmd*>(_cmd));
                break;
            case Command::EType::ClearResource:
                VisitCmd(static_cast<const ClearResourceCmd*>(_cmd));
                break;
            case Command::EType::Scope:
                VisitCmd(static_cast<const ScopeCmd*>(_cmd));
                break;
            case Command::EType::Query:
                VisitCmd(static_cast<const QueryCmd*>(_cmd));
                break;
            case Command::EType::Custom:
                VisitCmd(static_cast<const CustomCmd*>(_cmd));
                break;
            case Command::EType::CopyScope:
                assert(false && "CopyScope must be split before command reorder");
                break;
            default:
                assert(false && "Command Type Not Supported for Reorder");
        }
    }
};

} // namespace Moer::Render
#endif
