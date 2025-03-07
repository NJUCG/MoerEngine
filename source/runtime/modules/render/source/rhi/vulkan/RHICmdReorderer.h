#ifndef MOER_RHI_CMD_REORDERER_H
#define MOER_RHI_CMD_REORDERER_H

#include "misc/Hash.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include <format>
#include <limits>
#include <type_traits>
#include <variant>
#include "../RHIImpl.h"
#include "rhi/RHIResource.h"
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
        ArenaAllocatorWrapper(const ArenaAllocatorWrapper<U>& rhs)
            : _alloc{const_cast<ArenaAllocatorWrapper<U>&>(rhs).allocator()} {}

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
        CmdReorderer(FunctionTable _funcs) : m_arena(65556), m_arena_stl(m_arena), m_funcs(_funcs) {}
        ~CmdReorderer() {
        }
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
            int64 min;
            int64 max;
            Range() : min(std::numeric_limits<int64>::min()), max(std::numeric_limits<int64>::max()) {}
            Range(int64 _val) : min(_val), max(_val + 1) {}
            Range(int64 _min, int64 _size) : min(_min), max(_min + _size) {}
            bool Colide(const Range& _other) const {
                return min < _other.max && _other.min < max;
            }
            bool operator==(const Range& _other) const {
                return min == _other.min && max == _other.max;
            }
            bool operator!=(const Range& _other) const {
                return !operator==(_other);
            }
        };

        struct RangeHash {
            size_t operator()(const Range& _range) const {
                uint64 hash = 0;
                HashCombine(hash, GetHash(_range.min));
                HashCombine(hash, GetHash(_range.max));
                return hash;
            }
        };

        //command layer resource view
        struct ResourceView {
            int64 read_layer;
            int64 write_layer;
        };

        struct RangeHandle : public ResourceHandle {
            using Map = UnorderedMap<Range, ResourceView, RangeHash, std::equal_to<Range>, ArenaAllocatorWrapper<std::pair<const Range, ResourceView>>>;

        private:
            ResourceView          max_view;
            Range                 read_range;
            Range                 write_range;
            Map                   range2view;
            static constexpr uint max_range_size = 16;

        public:
            RangeHandle(const ArenaAllocatorWrapper<ResourceView>& _alloc) : read_range(std::numeric_limits<int64>::max(), std::numeric_limits<int64>::min()),
                                                                             write_range(std::numeric_limits<int64>::max(), std::numeric_limits<int64>::min()),
                                                                             range2view(max_range_size, _alloc) {
            }

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

                read_range.min = std::min(read_range.min, _range.min);
                read_range.max = std::max(read_range.max, _range.max);

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
                read_range.min       = std::min(read_range.min, _range.min);
                read_range.max       = std::max(read_range.max, _range.max);
                write_range.min      = std::min(write_range.min, _range.min);
                write_range.max      = std::max(write_range.max, _range.max);
                max_view.read_layer  = std::max(max_view.read_layer, _layer);
                max_view.write_layer = std::max(max_view.write_layer, _layer);
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
        struct CommandListNode {
            Command const*         cmd;
            CommandListNode const* next;
        };
        struct LinkedCommandList {
            CommandListNode const* head = nullptr;
            CommandListNode*       tail = nullptr;
        };
        Array<LinkedCommandList> m_cmd_lists;

        UnorderedMap<uint64, RangeHandle*>   m_range_handles;
        UnorderedMap<uint64, NoRangeHandle*> m_no_range_handles;

        UnorderedSet<uint64> m_write_resources;
        UnorderedSet<uint64> m_writed_geometry;

        //temporal resources
        Array<std::tuple<Range, ResourceHandle*>> m_arg_read_resources;
        Array<std::tuple<Range, ResourceHandle*>> m_arg_write_resources;
        UnorderedSet<uint64>                      temp_writed_resources;

        int64 m_dispatch_layer = -1;
        int64 m_max_bdls_layer = -1;//optimization

        ArenaAllocator                      m_arena;
        ArenaAllocatorWrapper<ResourceView> m_arena_stl;
        FunctionTable                       m_funcs;

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
                case ResourceType::Accel:
                    return func_emplace(m_no_range_handles);
                default: {
                    return func_emplace(m_range_handles);
                }
            }
        }

        //Important: sometimes we use barrier as previous states, last read-write already set by the last command, so we just read
        int64 GetLastLayer(uint64 _handle, const Range& _range, ResourceType _type) {
            auto* handle = GetHandle(_handle, _type);
            switch (_type) {
                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(handle);
                    return std::max((range_handle->GetMaxReadLayer(_range) - 1), 0ll);
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle = static_cast<NoRangeHandle*>(handle);
                    return std::max(no_range_handle->view.read_layer - 1, 0ll);
                }
            }
        }

        int64 GetLastLayerWrite(RangeHandle* _handle, const Range& _range) {
            int64 layer = _handle->GetMaxReadLayer(_range);
            return layer + 1;
        }

        int64 GetLastLayerWrite(NoRangeHandle* _handle) {
            int64 layer = std::max(_handle->view.read_layer, _handle->view.write_layer);
            //todo: specific layer need to be take cared, like mesh blas build or tlas build
            return layer + 1;
        }

        int64 GetLastLayerRead(RangeHandle* _handle, const Range& _range) {
            int64 layer = _handle->GetMaxWriteLayer(_range);
            return layer + 1;
        }

        int64 GetLastLayerRead(NoRangeHandle* _handle) {
            int64 layer = _handle->view.read_layer;
            return layer + 1;
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
        }

        int64 SetRead(ResourceHandle* _handle, const Range& _range) {
            int64 layer = 0;
            switch (_handle->type) {

                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    layer              = GetLastLayerRead(range_handle, _range);
                    range_handle->EmplaceReadLayer(_range, layer);
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                    layer                            = GetLastLayerRead(no_range_handle);
                    no_range_handle->view.read_layer = layer;
                }
            }
            return layer;
        }

        int64 SetRead(uint64 _handle, const Range& _range, ResourceType _type) {
            auto* handle = GetHandle(_handle, _type);
            return SetRead(handle, _range);
        }

        void RecordRead(ResourceHandle* _handle, Range _range, int64 _layer) {
            switch (_handle->type) {
                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    range_handle->EmplaceReadLayer(_range, _layer);
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                    no_range_handle->view.read_layer = _layer;
                }
            }
        }

        void RecordWrite(ResourceHandle* _handle, Range _range, int64 _layer) {
            switch (_handle->type) {
                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    range_handle->EmplaceWriteLayer(_range, _layer);
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                    no_range_handle->view.write_layer = _layer;
                }
            }
        }

        int64 SetWrite(ResourceHandle* _handle, const Range& _range) {
            int64 layer = 0;
            switch (_handle->type) {

                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    layer              = GetLastLayerWrite(range_handle, _range);
                    range_handle->EmplaceWriteLayer(_range, layer);
                    m_write_resources.emplace(_handle->handle);
                    break;
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                    layer                             = GetLastLayerWrite(no_range_handle);
                    no_range_handle->view.write_layer = layer;
                    no_range_handle->view.read_layer  = layer;
                    break;
                }
            }
            return layer;
        }

        int64 SetWrite(uint64 _handle, const Range& _range, ResourceType _type) {
            auto* handle = GetHandle(_handle, _type);
            return SetWrite(handle, _range);
        }

        int64 SetRW(uint64       _handle,
                    Range        _read_range,
                    ResourceType _read_type,
                    uint64       _write_handle,
                    Range        _write_range,
                    ResourceType _write_type) {
            int64 layer        = 0;
            auto* read_handle  = GetHandle(_handle, _read_type);
            auto* write_handle = GetHandle(_write_handle, _write_type);
            switch (_read_type) {
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle = static_cast<NoRangeHandle*>(read_handle);
                    layer                 = GetLastLayerWrite(no_range_handle);
                    break;
                }
                case ResourceType::Texture_Buffer:
                default: {

                    auto* range_handle = static_cast<RangeHandle*>(read_handle);
                    layer              = GetLastLayerWrite(range_handle, _read_range);
                }
            }

            switch (_write_type) {
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(write_handle);
                    layer                             = std::max(layer, GetLastLayerWrite(no_range_handle));
                    no_range_handle->view.write_layer = layer;
                    break;
                }
                case ResourceType::Texture_Buffer:
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
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(read_handle);
                    no_range_handle->view.read_layer = std::max(no_range_handle->view.read_layer, layer);
                    break;
                }
                case ResourceType::Texture_Buffer:

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

                    case ResourceType::Texture_Buffer: {
                        m_dispatch_layer = std::max(m_dispatch_layer, GetLastLayerWrite(static_cast<RangeHandle*>(handle), _range));
                        break;
                    }
                    case ResourceType::Bindless:
                    case ResourceType::Mesh:
                    case ResourceType::Accel: {
                        m_dispatch_layer = std::max(m_dispatch_layer, GetLastLayerWrite(static_cast<NoRangeHandle*>(handle)));

                    } break;
                }
                m_arg_write_resources.emplace_back(_range, handle);
                temp_writed_resources.emplace(_handle);
            } else {
                switch (_type) {
                    case ResourceType::Texture_Buffer: {
                        m_dispatch_layer = std::max(m_dispatch_layer, GetLastLayerRead(static_cast<RangeHandle*>(handle), _range));
                        break;
                    }
                    case ResourceType::Bindless:
                    case ResourceType::Mesh:
                    case ResourceType::Accel: {
                        m_dispatch_layer = std::max(m_dispatch_layer, GetLastLayerRead(static_cast<NoRangeHandle*>(handle)));
                    } break;
                }
                m_arg_read_resources.emplace_back(_range, handle);
            }
        }

        void VisitArgs(const TArg& _arg, uint64 _flag) {

            std::visit([&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    bool b_write = m_funcs.is_resource_write(_flag);
                    EmplaceArg((uint64)(_arg.GetBuffer()), ResourceType::Texture_Buffer, Range(_arg.GetByteOffset(), _arg.GetByteSize()), b_write);
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    bool b_write = m_funcs.is_resource_write(_flag);
                    EmplaceArg((uint64)(_arg.GetTexture()), ResourceType::Texture_Buffer, Range(_arg.mip_level, _arg.num_mips), b_write);
                } else if constexpr (std::is_same_v<T, std::span<TextureView>>) {
                    for (auto&& tex : _arg) {
                        EmplaceArg((uint64)(tex.GetTexture()), ResourceType::Texture_Buffer, Range(tex.mip_level, tex.num_mips), m_funcs.is_resource_write(_flag));
                    }
                } else if constexpr (std::is_same_v<T, std::span<BufferView>>) {
                    for (auto&& buf : _arg) {
                        EmplaceArg((uint64)(buf.GetBuffer()), ResourceType::Texture_Buffer, Range(buf.GetByteOffset(), buf.GetByteSize()), m_funcs.is_resource_write(_flag));
                    }
                }

                else if constexpr (std::is_same_v<T, RaytracingTlasRef>) {
                    EmplaceArg((uint64)(_arg.Get()), ResourceType::Accel, Range{}, false);
                } else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    // m_funcs.lock_bdls_array((uint64)(_arg.Get()));
                    // for (auto&& res : m_write_resources) {
                    //     if (m_funcs.is_resource_in_bindless(res, (uint64)(_arg.Get()))) {
                    //         EmplaceArg(res, ResourceType::Texture_Buffer, Range{}, false);
                    //     }
                    // }
                    // m_funcs.unlock_bdls_array((uint64)(_arg.Get()));
                    // //emplace self
                    // EmplaceArg((uint64)(_arg->ArrayHandle()), ResourceType::Bindless, Range{}, false);

                    assert(false && "Not support iterate BindlessArrayRef with other args");
                }
            },
                       _arg);
        }

        void VisitBindlessArg(BindlessArrayRef _bdls, const UnorderedSet<uint64>& _temp_write_resources) {
            m_funcs.lock_bdls_array((uint64)(_bdls.Get()));
            for (auto&& res : m_write_resources) {
                if (!_temp_write_resources.contains(res) && m_funcs.is_resource_in_bindless(res, (uint64)(_bdls.Get()))) {
                    EmplaceArg(res, ResourceType::Texture_Buffer, Range{}, false);
                }
            }
            m_funcs.unlock_bdls_array((uint64)(_bdls.Get()));
            //emplace self
            EmplaceArg((uint64)(_bdls->ArrayHandle()), ResourceType::Bindless, Range{}, false);
        }

        void VisitCmd(const UploadBufferCmd* _cmd) {
            AddCmd(_cmd, SetWrite(_cmd->Handle(), Range(_cmd->Offset(), _cmd->ByteSize()), ResourceType::Texture_Buffer));
        }

        void VisitCmd(const CopyBackBufferCmd* _cmd) {
            AddCmd(_cmd, SetRead(_cmd->Handle(), Range(_cmd->Offset(), _cmd->ByteSize()), ResourceType::Texture_Buffer));
        }

        void VisitCmd(const CopyBackTextureCmd* _cmd) {
            AddCmd(_cmd, SetRead(_cmd->Handle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer));
        }

        void VisitCmd(const CopyBufferCmd* _cmd) {
            AddCmd(_cmd, SetRW(_cmd->SrcHandle(), Range(_cmd->SrcOffset(), _cmd->ByteSize()), ResourceType::Texture_Buffer, _cmd->DstHandle(), Range(_cmd->DstOffset(), _cmd->ByteSize()), ResourceType::Texture_Buffer));
        }
        void VisitCmd(const CopyTextureCmd* _cmd) {
            AddCmd(_cmd, SetRW(_cmd->SrcHandle(), Range(_cmd->SrcMipLevel()), ResourceType::Texture_Buffer, _cmd->DstHandle(), Range(_cmd->DstMipLevel()), ResourceType::Texture_Buffer));
        }
        void VisitCmd(const CopyBufferToTextureCmd* _cmd) {
            AddCmd(_cmd, SetRW(_cmd->SrcHandle(), Range(_cmd->SrcOffset(), _cmd->ByteSize()), ResourceType::Texture_Buffer, _cmd->DstHandle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer));
        }
        void VisitCmd(const CopyTextureToBufferCmd* _cmd) {
            AddCmd(_cmd, SetRW(_cmd->SrcHandle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer, _cmd->DstHandle(), Range(_cmd->DstOffset(), _cmd->ByteSize()), ResourceType::Texture_Buffer));
        }

        void VisitCmd(const UploadTextureCmd* _cmd) {
            AddCmd(_cmd, SetWrite(_cmd->Handle(), Range(_cmd->MipLevel()), ResourceType::Texture_Buffer));
        }

        void VisitCmd(const BarrierCmd* _cmd) {
            int64               layer = 0;
            Array<RangeHandle*> barrier_resources;
            Array<Range>        barrier_ranges;
            //reserve
            barrier_resources.reserve(_cmd->ReadBuffers().size() + _cmd->ReadTextures().size() + _cmd->WriteBuffers().size() + _cmd->WriteTextures().size());
            barrier_ranges.reserve(_cmd->ReadBuffers().size() + _cmd->ReadTextures().size() + _cmd->WriteBuffers().size() + _cmd->WriteTextures().size());

            for (const auto& [handle, state, pass_type, offset, size] : _cmd->ReadBuffers()) {
                // layer = std::max(
                //     SetRead(handle, Range(offset, size), ResourceType::Texture_Buffer),
                //     layer);
                RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
                layer                     = GetLastLayerRead(range_handle, Range(offset, size));
                // barrier_resources.emplace_back(range_handle);
                // barrier_ranges.emplace_back(Range(offset, size));
            }
            for (const auto& [handle, state, pass_type, mip_level, mip_cnt] : _cmd->ReadTextures()) {
                // layer = std::max(
                //     SetRead(handle, Range(mip_level, mip_cnt), ResourceType::Texture_Buffer),
                //     layer);
                RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
                layer                     = GetLastLayerRead(range_handle, Range(mip_level, mip_cnt));
                // barrier_resources.emplace_back(range_handle);
                // barrier_ranges.emplace_back(Range(mip_level, mip_cnt));
            }

            for (auto& [handle, state, pass_type, offset, size] : _cmd->WriteBuffers()) {
                // layer = std::max(
                //     SetWrite(handle, Range(offset, size), ResourceType::Texture_Buffer),
                //     layer);
                RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
                layer                     = GetLastLayerRead(range_handle, Range(offset, size));
                barrier_resources.emplace_back(range_handle);
                barrier_ranges.emplace_back(Range(offset, size));
            }

            for (const auto& [handle, state, pass_type, mip_level, mip_cnt] : _cmd->WriteTextures()) {
                // layer = std::max(
                //     SetWrite(handle, Range(mip_level, mip_cnt), ResourceType::Texture_Buffer),
                //     layer);
                RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(handle, ResourceType::Texture_Buffer));
                layer                     = GetLastLayerRead(range_handle, Range(mip_level, mip_cnt));
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

                for (const auto& [handle, state] : _cmd->ImportTextures()) {
                    RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer));
                    layer                     = GetLastLayerWrite(range_handle, Range(handle.mip_level, handle.num_mips));
                    assert(layer == 0 && std::format("Import Texture {} should be the first command", handle.GetTexture()->GetName()).c_str());
                }
                for (const auto& [handle, state] : _cmd->ImportTextures()) {
                    RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer));
                    range_handle->EmplaceWriteLayer(Range(handle.mip_level, handle.num_mips), layer);
                }
            } else {
                barrier_resources.reserve(_cmd->ExportTextures().size());
                barrier_ranges.reserve(_cmd->ExportTextures().size());

                for (const auto& [handle, state] : _cmd->ExportTextures()) {
                    RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer));
                    layer                     = GetLastLayerWrite(range_handle, Range(handle.mip_level, handle.num_mips));
                    //TODO: store export resources and check later usages
                    // assert(layer == 0 && std::format("Export Texture {} should be the first command", handle.GetTexture()->GetName()).c_str());
                }

                for (const auto& [handle, state] : _cmd->ExportTextures()) {
                    RangeHandle* range_handle = static_cast<RangeHandle*>(GetHandle(uint64(handle.GetTexture()), ResourceType::Texture_Buffer));
                    range_handle->EmplaceWriteLayer(Range(handle.mip_level, handle.num_mips), layer);
                }
            }

            AddCmd(_cmd, layer);
        }

        void VisitCmd(const SetDrawStateCmd* _cmd) {
            int64 layer = 0;
            m_arg_read_resources.clear();
            m_arg_write_resources.clear();
            temp_writed_resources.clear();

            const auto& pipeline = _cmd->Pipeline();
            auto        func     = [&](const TArg& _arg, uint _idx) {
                VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags);
            };

            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
            };

            _cmd->IterateArgs(func, bdls_post_func);

            const auto& vbs = _cmd->VertexBuffers();
            for (const auto& vb : vbs) {
                EmplaceArg((uint64)(vb.first), ResourceType::Texture_Buffer, Range(vb.second.min, vb.second.max - vb.second.min), false);
                // m_dispatch_layer = std::max(SetRead((uint64)(vb.first), Range(vb.second.min, vb.second.max - vb.second.min), ResourceType::Texture_Buffer), layer);
            }
            const auto& ibs = _cmd->IndexBuffers();
            for (const auto& ib : ibs) {
                // m_dispatch_layer = std::max(SetRead((uint64)(ib.first), Range(ib.second.min, ib.second.max - ib.second.min), ResourceType::Texture_Buffer), layer);
                EmplaceArg((uint64)(ib.first), ResourceType::Texture_Buffer, Range(ib.second.min, ib.second.max - ib.second.min), false);
            }
            //depth and render targets
            const auto& pass_info = _cmd->RenderPassInfo();
            if (pass_info.depth_attachment.Valid()) {
                const auto& depth          = pass_info.depth_attachment;
                auto        depth_store_op = GetStoreOp(GetDepthAction(depth.action));
                if (GetLoadOp(GetDepthAction(depth.action)) == EAttachmentLoadOp::LOAD) {
                    EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), false);
                }
                if (depth_store_op == EAttachmentStoreOp::STORE) {
                    EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), true);
                }
            }
            for (const auto& target : pass_info.color_attachments) {
                auto color_store_op = GetStoreOp(target.action);
                if (GetLoadOp(target.action) == EAttachmentLoadOp::LOAD) {
                    EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), false);
                }
                if (color_store_op == EAttachmentStoreOp::STORE) {
                    EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), true);
                }
            }
            for (const auto& write_res : m_arg_write_resources) {
                RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
            }

            for (const auto& read_res : m_arg_read_resources) {
                RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
            }
            AddCmd(_cmd, m_dispatch_layer);
        }

        void VisitCmd(const SetGeometryPassDrawStateCmd* _cmd) {
            int64 layer = 0;
            m_arg_read_resources.clear();
            m_arg_write_resources.clear();
            temp_writed_resources.clear();

            auto func = [&](const TArg& _arg, uint _idx) {
                for (const auto& [bitmask, pso] : _cmd->PipelineMap()) {
                    VisitArgs(_arg, pso.binding_infos[_idx].state_flags);
                }
            };

            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
            };

            _cmd->IterateArgs(func, bdls_post_func);

            const auto& vbs = _cmd->VertexBuffers();
            for (const auto& vb : vbs) {
                EmplaceArg((uint64)(vb.first), ResourceType::Texture_Buffer, Range(vb.second.min, vb.second.max - vb.second.min), false);
            }
            const auto& ibs = _cmd->IndexBuffers();
            for (const auto& ib : ibs) {
                EmplaceArg((uint64)(ib.first), ResourceType::Texture_Buffer, Range(ib.second.min, ib.second.max - ib.second.min), false);
            }
            //depth and render targets
            const auto& pass_info = _cmd->RenderPassInfo();
            if (pass_info.depth_attachment.Valid()) {
                const auto& depth          = pass_info.depth_attachment;
                auto        depth_store_op = GetStoreOp(GetDepthAction(depth.action));
                if (GetLoadOp(GetDepthAction(depth.action)) == EAttachmentLoadOp::LOAD) {
                    EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), false);
                }
                if (depth_store_op == EAttachmentStoreOp::STORE) {
                    EmplaceArg((uint64)(depth.target), ResourceType::Texture_Buffer, Range(0), true);
                }
            }
            for (const auto& target : pass_info.color_attachments) {
                auto color_store_op = GetStoreOp(target.action);
                if (GetLoadOp(target.action) == EAttachmentLoadOp::LOAD) {
                    EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), false);
                }
                if (color_store_op == EAttachmentStoreOp::STORE) {
                    EmplaceArg((uint64)(target.target), ResourceType::Texture_Buffer, Range(0), true);
                }
            }
            for (const auto& write_res : m_arg_write_resources) {
                RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
            }

            for (const auto& read_res : m_arg_read_resources) {
                RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
            }
            AddCmd(_cmd, m_dispatch_layer);
        }

        void VisitCmd(const UpdateBindlessArrayCmd* _cmd) {
            //TODO: important here
            AddCmd(_cmd, SetWrite((uint64)(_cmd->Handle()->ArrayHandle()), Range(), ResourceType::Bindless));
        }

        void VisitCmd(const ClearResourceCmd* _cmd) {
            std::visit([&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    AddCmd(_cmd, SetWrite((uint64)(_arg.GetBuffer()), Range(_arg.GetByteOffset(), _arg.GetByteSize()), ResourceType::Texture_Buffer));
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    AddCmd(_cmd, SetWrite((uint64)(_arg.GetTexture()), Range(_arg.mip_level, _arg.num_mips), ResourceType::Texture_Buffer));
                }
            },
                       _cmd->Resource());
        }

        void VisitCmd(const DispatchCmd* _cmd) {
            m_arg_write_resources.clear();
            m_arg_read_resources.clear();
            temp_writed_resources.clear();
            // auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
            //     VisitArgs(_arg, _flag.state_flags);
            // };
            // _cmd->IterateArgs(func);

            const auto& pipeline = _cmd->Pipeline();

            auto func = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx))
                    VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags);
            };

            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx))
                    VisitBindlessArg(std::get<BindlessArrayRef>(_arg), temp_writed_resources);
            };

            _cmd->IterateArgs(func, bdls_post_func);

            for (const auto& write_res : m_arg_write_resources) {
                RecordWrite(std::get<1>(write_res), std::get<0>(write_res), m_dispatch_layer);
            }
            for (const auto& read_res : m_arg_read_resources) {
                RecordRead(std::get<1>(read_res), std::get<0>(read_res), m_dispatch_layer);
            }
            AddCmd(_cmd, m_dispatch_layer);
        }

        void VisitCmd(const BuildAccelerationStructuresCmd* _cmd) {
            int64 layer = 0;
            for (const auto& cmd : _cmd->Params()) {
                // FIXME: 不确定这里的修改是否正确。因为MeshBuffers的信息被分散到了每个MeshGeometry中，所以这里就需要对应遍历所有Segment
                //        类似场景见 VulkanQueue.cpp:393附近
                for (const auto& segment : cmd.geometry->GetInfo().segments) {
                    Buffer* vtx = segment.vertex_buffer.Get();
                    Buffer* idx = segment.index_buffer.Get();

                    layer = std::max(layer, SetRead((uint64)vtx, Range(0, vtx->GetByteSize()), ResourceType::Texture_Buffer));
                    layer = std::max(layer, SetRead((uint64)idx, Range(0, idx->GetByteSize()), ResourceType::Texture_Buffer));
                }
                layer = std::max(layer, SetWrite((uint64)cmd.geometry.Get(), Range(0), ResourceType::Accel));

                m_writed_geometry.emplace((uint64)cmd.geometry.Get());
            }

            AddCmd(_cmd, layer);
        }

        void VisitCmd(const UpdateRaytracingSceneCmd* _cmd) {
            if (_cmd->InstancesToUpdate().size() == 0 && !_cmd->ForceUpdate()) {
                return;
            }
            int64 layer       = SetWrite((uint64)_cmd->TlasHandle(), Range(0), ResourceType::Accel);
            auto* tlas_handle = static_cast<NoRangeHandle*>(GetHandle((uint64)_cmd->TlasHandle(), ResourceType::Accel));

            {
                layer = GetLastLayerWrite(tlas_handle);
            }
            for (const uint64& handle : m_writed_geometry) {
                if (_cmd->HasGeometry(handle)) {
                    // layer = std::max(layer, SetRead(handle, Range(0), ResourceType::Accel));

                    auto* geo_handle = GetHandle((uint64)handle, ResourceType::Accel);
                    layer            = std::max(layer, GetLastLayerRead(static_cast<NoRangeHandle*>(geo_handle)));
                }
            }

            //set read and write
            for (const uint64& handle : m_writed_geometry) {
                if (_cmd->HasGeometry(handle)) {
                    auto* geo_handle            = (NoRangeHandle*)GetHandle((uint64)handle, ResourceType::Accel);
                    geo_handle->view.read_layer = layer;
                }
            }

            tlas_handle->view.write_layer = layer;
            tlas_handle->view.read_layer  = layer;

            AddCmd(_cmd, layer);
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
            auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
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
            ++m_dispatch_layer;// make custom dispatch command in a separate layer
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
                case Command::EType::SetGeometryPassDrawState:
                    VisitCmd(static_cast<const SetGeometryPassDrawStateCmd*>(_cmd));
                    break;
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
                case Command::EType::Custom:
                    VisitCmd(static_cast<const CustomCmd*>(_cmd));
                    break;
                default:
                    assert(false && "Command Type Not Supported for Reorder");
            }
        }
    };

}// namespace Moer::Render
#endif