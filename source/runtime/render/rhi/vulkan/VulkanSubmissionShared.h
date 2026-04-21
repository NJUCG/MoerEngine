#pragma once

#include "VulkanQueue.h"

#include <optional>
#include <string>

namespace Moer::Render {

using SyncPointId = uint64;

struct SubmissionKey {
    uint64 op_seq{0};
    uint32 submit_idx{0};

    bool operator==(const SubmissionKey& other) const {
        return op_seq == other.op_seq && submit_idx == other.submit_idx;
    }
};

struct SubmissionKeyHash {
    size_t operator()(const SubmissionKey& key) const {
        size_t hash = std::hash<uint64>{}(key.op_seq);
        hash ^= std::hash<uint32>{}(key.submit_idx) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

struct SourceSubmitKey {
    uint64 op_seq{0};
    uint32 submit_idx{0};

    bool operator==(const SourceSubmitKey& other) const {
        return op_seq == other.op_seq && submit_idx == other.submit_idx;
    }
};

struct SourceSubmitKeyHash {
    size_t operator()(const SourceSubmitKey& key) const {
        size_t hash = std::hash<uint64>{}(key.op_seq);
        hash ^= std::hash<uint32>{}(key.submit_idx) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

struct TranslateResult {
    EQueueType queue{EQueueType::Ignore};
    std::optional<VulkanRecordedSubmit> recorded_submit{};
    GraphEventRef translate_complete{nullptr};
    bool          valid{true};
    std::string   error{};
};

struct QueueTranslateInfo {
    SubmissionKey key{};
    SourceSubmitKey source_key{};
    uint32 source_segment_index{0};
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit submit;
    TrackerSeed initial_seed{};
    GraphEventArray task_dependencies{};
    GraphEventRef   completion_event{nullptr};
    ERHITranslateExecutionClass execution_class{ERHITranslateExecutionClass::Parallel};
    bool         valid{true};
    std::string  error{};

    QueueTranslateInfo(
        SubmissionKey in_key,
        SourceSubmitKey in_source_key,
        uint32 in_source_segment_index,
        EQueueType in_queue,
        CmdSubmit&& in_submit,
        TrackerSeed&& in_seed
    ) :
        key(in_key),
        source_key(in_source_key),
        source_segment_index(in_source_segment_index),
        queue(in_queue),
        submit(std::move(in_submit)),
        initial_seed(std::move(in_seed)),
        execution_class(submit.translate_execution_class) {}

    QueueTranslateInfo(QueueTranslateInfo&&) noexcept            = default;
    QueueTranslateInfo& operator=(QueueTranslateInfo&&) noexcept = default;
    QueueTranslateInfo(const QueueTranslateInfo&)                = delete;
    QueueTranslateInfo& operator=(const QueueTranslateInfo&)     = delete;
};

struct ResourceStateValue {
    bool                         known{false};
    bool                         has_writer{false};
    EQueueType                   owner_queue{EQueueType::Ignore};
    std::optional<SubmissionKey> last_submission{};
    EBufferState                 buffer_state{EBufferState::UNDEFINED};
    ETextureState                texture_state{ETextureState::UNDEFINED};
};

struct ExecutorPresentOp {
    SwapchainRef swapchain{};
    TextureView  target{};
    EQueueType   queue{EQueueType::Graphics};
};

struct SubmissionHostFence {
    VkDevice device{VK_NULL_HANDLE};
    VkFence  handle{VK_NULL_HANDLE};
    bool     owned{false};

    void Reset() {
        device = VK_NULL_HANDLE;
        handle = VK_NULL_HANDLE;
        owned  = false;
    }
};

struct SubmitPresentStage {
    uint64             op_seq{0};
    ExecutorPresentOp  present{};
    bool               has_source_texture_state{false};
    ResourceStateValue source_texture_state{};
    bool               valid{true};
    std::string        error{};

    SubmitPresentStage(uint64 in_op_seq, ExecutorPresentOp&& in_present) :
        op_seq(in_op_seq),
        present(std::move(in_present)) {}

    SubmitPresentStage() = default;
    SubmitPresentStage(SubmitPresentStage&&) noexcept            = default;
    SubmitPresentStage& operator=(SubmitPresentStage&&) noexcept = default;
    SubmitPresentStage(const SubmitPresentStage&)                = delete;
    SubmitPresentStage& operator=(const SubmitPresentStage&)     = delete;
};

struct RootRhiBoundary {
    GraphEventRef   host_event{nullptr};
    Array<WaitEvent> gpu_waits{};
};

struct ResolvedSyncPoint {
    EQueueType queue{EQueueType::Ignore};
    uint64     timeline_handle{0};
    uint64     value{0};
};

struct SubmitInfo {
    SubmissionKey                    key{};
    uint64                           submit_seq{0};
    EQueueType                       queue{EQueueType::Ignore};
    SyncPointId                      signal_syncpoint{0};
    Array<SyncPointId>               wait_syncpoints{};
    Array<GraphEventRef>             interrupt_completion_events{};
    TranslateResult                  translate_result{};

    SubmitInfo(
        SubmissionKey   in_key,
        uint64          in_submit_seq,
        EQueueType      in_queue,
        SyncPointId     in_signal_syncpoint,
        TranslateResult&& in_translate_result
    ) :
        key(in_key),
        submit_seq(in_submit_seq),
        queue(in_queue),
        signal_syncpoint(in_signal_syncpoint),
        translate_result(std::move(in_translate_result)) {}

    SubmitInfo() = default;
    SubmitInfo(SubmitInfo&&) noexcept            = default;
    SubmitInfo& operator=(SubmitInfo&&) noexcept = default;
    SubmitInfo(const SubmitInfo&)                = delete;
    SubmitInfo& operator=(const SubmitInfo&)     = delete;
};

} // namespace Moer::Render
