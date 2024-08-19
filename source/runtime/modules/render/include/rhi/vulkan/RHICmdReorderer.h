#ifndef MOER_RHI_CMD_REORDERER_H
#define MOER_RHI_CMD_REORDERER_H

#include "misc/Hash.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include <limits>
#include <type_traits>
/**
 * @brief Copy From Luisa Runtime(LC) src/backends/common/command_reorder_visitor.h with respect
 * 
 */
namespace Moer::Render {
    struct ArenaAllocator {
        ArenaAllocator(uint64 _size) : capacity(_size) {
            data = reinterpret_cast<byte*>(Memory::Malloc(_size));
        }
        ArenaAllocator(const ArenaAllocator&)            = delete;
        ArenaAllocator& operator=(const ArenaAllocator&) = delete;

        ArenaAllocator(ArenaAllocator&& _other) noexcept {
            data            = _other.data;
            capacity        = _other.capacity;
            _other.data     = nullptr;
            _other.capacity = 0;
        }
        ArenaAllocator& operator=(ArenaAllocator&& _other) noexcept {
            if (this != &_other) {
                Memory::Free(data);
                data            = _other.data;
                capacity        = _other.capacity;
                _other.data     = nullptr;
                _other.capacity = 0;
            }
            return *this;
        }

        ~ArenaAllocator() {
            if (data)
                Memory::Free(data);
        }

        void* Malloc(uint64 _size) {
            if (offset + _size > capacity) {
                assert(false && "Out of memory");
                return nullptr;
            }
            void* ptr = data + offset;
            offset += _size;
            return ptr;
        }

        template<typename T>
        T* Malloc() {
            return reinterpret_cast<T*>(Malloc(sizeof(T)));
        }

        byte*  data     = nullptr;
        uint64 offset   = 0;
        uint64 capacity = 0;
    };
    class CmdReorderer {

    public:
        CmdReorderer() : m_arena(65556) {}
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
            using Map = UnorderedMap<Range, ResourceView, RangeHash>;

        private:
            ResourceView          max_view;
            Range                 read_range;
            Range                 write_range;
            Map                   range2view;
            static constexpr uint max_range_size = 16;

        public:
            RangeHandle() : read_range(std::numeric_limits<int64>::max(), std::numeric_limits<int64>::min()),
                            write_range(std::numeric_limits<int64>::max(), std::numeric_limits<int64>::min()),
                            range2view(max_range_size) {
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
            ResourceView view;
        };
        struct CommandListNode {
            Command const*         cmd;
            CommandListNode const* next;
        };
        Array<CommandListNode*> m_cmd_lists;

        UnorderedMap<uint64, RangeHandle*>   m_range_handles;
        UnorderedMap<uint64, NoRangeHandle*> m_no_range_handles;

        Array<std::tuple<Range, ResourceHandle*>> m_read_resources;
        Array<std::tuple<Range, ResourceHandle*>> m_write_resources;
        uint64                                    m_dispatch_layer;
        ArenaAllocator                            m_arena;

    public:
        ResourceHandle* GetHandle(uint64 _handle, ResourceType _type) {
            auto func_emplace = [&](auto& _map) {
                auto   iter  = _map.try_emplace(_handle);
                auto&& value = iter.first->second;
                using TValue = std::remove_pointer_t<std::remove_reference_t<decltype(value)>>;
                if (iter.second) {
                    TValue* value = m_arena.Malloc<TValue>();
                    new (value) TValue();
                    value->handle = _handle;
                    value->type   = _type;
                }
                return value;
            };

            switch (_type) {
                case ResourceType::Texture_Buffer:
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel:
                    return func_emplace(m_no_range_handles);
                default: {
                    return func_emplace(m_range_handles);
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
            auto*            last = m_cmd_lists[_layer];
            CommandListNode* ptr  = m_arena.Malloc<CommandListNode>();
            new (ptr) CommandListNode(_cmd, last);
            m_cmd_lists[_layer] = ptr;
        }

        uint64 SetRead(ResourceHandle* _handle, const Range& _range) {
            int64 layer = 0;
            switch (_handle->type) {

                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    layer              = GetLastLayerRead(range_handle, _range);
                    range_handle->EmplaceReadLayer(_range, layer);
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                    layer                            = GetLastLayerRead(no_range_handle);
                    no_range_handle->view.read_layer = layer;
                } break;
            }
        }

        uint64 SetRead(uint64 _handle, const Range& _range, ResourceType _type) {
            auto* handle = GetHandle(_handle, _type);
            return SetRead(handle, _range);
        }

        void RecordRead(ResourceHandle* _handle, Range _range, int64 _layer) {
            switch (_handle->type) {
                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    range_handle->EmplaceReadLayer(_range, _layer);
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(_handle);
                    no_range_handle->view.read_layer = _layer;
                } break;
            }
        }

        void RecordWrite(ResourceHandle* _handle, Range _range, int64 _layer) {
            switch (_handle->type) {
                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    range_handle->EmplaceWriteLayer(_range, _layer);
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                    no_range_handle->view.write_layer = _layer;
                } break;
            }
        }

        int64 SetWrite(ResourceHandle* _handle, const Range& _range) {
            int64 layer = 0;
            switch (_handle->type) {

                case ResourceType::Texture_Buffer: {
                    auto* range_handle = static_cast<RangeHandle*>(_handle);
                    layer              = GetLastLayerWrite(range_handle, _range);
                    range_handle->EmplaceWriteLayer(_range, layer);
                }
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(_handle);
                    layer                             = GetLastLayerWrite(no_range_handle);
                    no_range_handle->view.write_layer = layer;
                } break;
            }
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
                case ResourceType::Texture_Buffer:
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle = static_cast<NoRangeHandle*>(read_handle);
                    layer                 = GetLastLayerWrite(no_range_handle);
                }
                default: {

                    auto* range_handle = static_cast<RangeHandle*>(read_handle);
                    layer              = GetLastLayerWrite(range_handle, _read_range);
                }
            }

            switch (_write_type) {
                case ResourceType::Texture_Buffer:
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle             = static_cast<NoRangeHandle*>(write_handle);
                    layer                             = GetLastLayerWrite(no_range_handle);
                    no_range_handle->view.write_layer = layer;
                } default:
                 {
                    auto* range_handle = static_cast<RangeHandle*>(write_handle);
                    layer              = GetLastLayerWrite(range_handle, _write_range);
                    range_handle->EmplaceWriteLayer(_write_range, layer);
                }
            }

            //now set read
            switch (_read_type) {
                case ResourceType::Texture_Buffer:
                case ResourceType::Mesh:
                case ResourceType::Bindless:
                case ResourceType::Accel: {
                    auto* no_range_handle            = static_cast<NoRangeHandle*>(read_handle);
                    no_range_handle->view.read_layer = std::max(no_range_handle->view.read_layer, layer);
                }
                default: {
                    auto* range_handle = static_cast<RangeHandle*>(read_handle);
                    range_handle->EmplaceReadLayer(_read_range, layer);
                }
            }
            return layer;
        }

        void Visit(){}
    };

}// namespace Moer::Render
#endif