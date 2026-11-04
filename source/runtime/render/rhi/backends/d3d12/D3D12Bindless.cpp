#include "D3D12Backend.h"

namespace Moer::Render::D3D12 {

Bindless::Bindless(Context& context, uint32 capacity) :
    context(context),
    capacity(capacity) {
    if (capacity < 3 || capacity >= (1u << 23))
        throw std::invalid_argument("Invalid D3D12 bindless capacity");
    desired.resize(1);
    committed.resize(1);
    desired_generations.resize(1);
    committed_generations.resize(1);
}
uint32 Bindless::AllocateTexture(const TextureView& view, Sampler sampler) {
    if (!view.texture)
        throw std::invalid_argument("Null bindless texture");
    std::lock_guard lock(mutex);
    uint32          index;
    if (!free_indices.empty()) {
        index = free_indices.back();
        free_indices.pop_back();
    } else {
        index = uint32(desired.size());
        if (index + 2 > capacity)
            throw std::runtime_error("D3D12 bindless capacity exhausted");
        desired.resize(index + 2);
        desired_generations.resize(index + 2);
    }
    Entry entry;
    entry.texture              = view.texture;
    entry.texture_view         = view;
    entry.sampler              = sampler;
    desired[index]             = entry;
    desired_generations[index] = ++generation;
    updates.emplace_back(TextureUpdateInfo{view.texture, sampler, view.format, index, index, view.mip_level, view.num_mips, view.array_layer, view.num_array, generation, generation, 0, false});
    return index;
}
uint32 Bindless::AllocateBuffer(BufferView view) {
    if (!view.buffer || view.byte_offset % 4 || view.GetByteSize() % 4)
        throw std::invalid_argument("Bindless raw buffer range must be DWORD aligned");
    std::lock_guard lock(mutex);
    uint32          index;
    if (!free_indices.empty()) {
        index = free_indices.back();
        free_indices.pop_back();
    } else {
        index = uint32(desired.size());
        if (index + 2 > capacity)
            throw std::runtime_error("D3D12 bindless capacity exhausted");
        desired.resize(index + 2);
        desired_generations.resize(index + 2);
    }
    Entry entry;
    entry.buffer               = view.buffer;
    entry.buffer_view          = view;
    desired[index]             = entry;
    desired_generations[index] = ++generation;
    updates.emplace_back(BufferUpdateInfo{view.buffer, index, index, view.format, generation, generation, 0, false, view.byte_offset, view.GetByteSize()});
    return index;
}
void Bindless::UnbindTexture(uint32 handle) {
    std::lock_guard lock(mutex);
    if (!handle || handle >= desired.size() || (!desired[handle].texture && !desired[handle].buffer))
        return;
    desired[handle]             = {};
    desired_generations[handle] = ++generation;
    TextureUpdateInfo update{nullptr, {SF_LINEAR, SAM_REPEAT}, PF_UNDEFINED};
    update.array_idx        = handle;
    update.array_generation = generation;
    update.free             = true;
    updates.emplace_back(std::move(update));
    free_indices.push_back(handle);
}
void Bindless::UnbindBuffer(uint32 handle) {
    UnbindTexture(handle);
}
UniquePtr<Command> Bindless::CreateUpdateCommand() {
    std::lock_guard lock(mutex);
    if (updates.empty())
        return {};
    Array<UpdateCmd> commands;
    commands.swap(updates);
    return MakeUnique<UpdateBindlessArrayCmd>(BindlessArrayRef{this}, std::move(commands), Array<byte>{}, Array<std::pair<uint32, uint32>>{}, Array<byte>{}, Array<std::pair<uint32, uint32>>{}, Array<byte>{}, Array<std::pair<uint32, uint32>>{});
}
void Bindless::DiscardUpdateCommand(const Array<UpdateCmd>& commands) {
    std::lock_guard lock(mutex);
    for (const auto& command : commands)
        std::visit([&](const auto& update) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(update)>, InvalidUpdateInfo>) {
                const uint32 index   = update.array_idx;
                const uint64 applied = index < committed_generations.size() ? committed_generations[index] : 0;
                if (index < desired_generations.size() && update.array_generation == desired_generations[index] && applied < update.array_generation)
                    updates.push_back(command);
            }
        },
                   command);
}
std::function<void()> Bindless::Apply(const UpdateBindlessArrayCmd& command) {
    std::lock_guard lock(mutex);
    auto            old_entries     = committed;
    auto            old_generations = committed_generations;
    for (const auto& cmd : command.UpdateCommands())
        std::visit([&](const auto& update) {
            using T = std::decay_t<decltype(update)>;
            if constexpr (!std::is_same_v<T, InvalidUpdateInfo>) {
                const uint32 index = update.array_idx;
                if (index + 2 > committed.size()) {
                    committed.resize(index + 2);
                    committed_generations.resize(index + 2);
                }
                if (update.array_generation < committed_generations[index])
                    return;
                Entry entry;
                if (!update.free) {
                    if constexpr (std::is_same_v<T, TextureUpdateInfo>) {
                        entry.texture      = update.texture;
                        entry.texture_view = TextureView(update.texture.Get(), update.format, update.mip_level, update.num_mips).Slice(update.array_layer, update.array_count);
                        entry.sampler      = update.sampler;
                    } else {
                        entry.buffer      = update.buffer;
                        entry.buffer_view = update.buffer->GetView(update.format, update.byte_offset, update.byte_size ? update.byte_size : update.buffer->GetByteSize() - update.byte_offset);
                    }
                }
                committed[index]             = std::move(entry);
                committed_generations[index] = update.array_generation;
            }
        },
                   cmd);
    return [this, entries = std::move(old_entries), generations = std::move(old_generations)]() mutable {
        std::lock_guard rollback_lock(mutex);
        committed.swap(entries);
        committed_generations.swap(generations);
    };
}
Array<Bindless::Entry> Bindless::Snapshot() {
    std::lock_guard lock(mutex);
    return committed;
}
} // namespace Moer::Render::D3D12
