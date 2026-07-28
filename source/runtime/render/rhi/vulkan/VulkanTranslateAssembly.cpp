#include "VulkanSubmissionExecutorPrivate.h"
#include "VulkanThreadHeartbeat.h"

namespace Moer::Render {

struct AutoGpuEventTokenFactory {
    static QueryToken CreateTimestampToken(StringView name) {
        static std::atomic<uint64_t> next_auto_query_id{1ull << 62u};
        const uint64_t token_id = next_auto_query_id.fetch_add(1, std::memory_order_relaxed);
        return QueryToken(token_id, QueryKind::Timestamp, String(name), QueryFuture::Create());
    }
};

namespace {

static constexpr uint32 s_max_translate_batch_commands = 256;

uint32 CountTranslateCommands(const QueueTranslateInfo& translate_info) {
    return std::max<uint32>(1u, static_cast<uint32>(translate_info.submit.cmds.size()));
}

static bool HasGpuEventType(const CmdSubmit& submit, GPUEvent::EType type) {
    return std::any_of(submit.gpu_events.begin(), submit.gpu_events.end(), [type](const GPUEvent& event) {
        return event.type == type;
    });
}

static void AppendTimestampPair(
    Array<UniquePtr<Command>>& commands,
    const QueryToken&          token,
    StringView                 name
) {
    commands.emplace_back(MakeUnique<QueryCmd>(token, QueryCmd::EOp::BeginTimestamp, name));
    commands.emplace_back(MakeUnique<QueryCmd>(token, QueryCmd::EOp::EndTimestamp, name));
}

static void InjectAutoCommandListGpuEvents(CmdSubmit& submit) {
    if (submit.cmds.empty()) {
        return;
    }

    const bool has_begin_command_list =
        HasGpuEventType(submit, GPUEvent::EType::BeginGPU);
    const bool has_end_command_list =
        HasGpuEventType(submit, GPUEvent::EType::EndGPU);
    const bool has_frame_bound = HasGpuEventType(submit, GPUEvent::EType::FrameBoundary);

    const bool inject_command_list_span =
        (!submit.gpu_events.empty() || submit.b_tick_profiling) &&
        !(has_begin_command_list && has_end_command_list);
    const bool inject_frame_bound = submit.b_tick_profiling && !has_frame_bound;
    if (!inject_command_list_span && !inject_frame_bound) {
        return;
    }

    QueryToken begin_token{};
    QueryToken end_token{};
    QueryToken frame_bound_token{};

    if (inject_command_list_span) {
        begin_token = AutoGpuEventTokenFactory::CreateTimestampToken(MOER_TEXT("AutoBeginCommandList"));
        end_token   = AutoGpuEventTokenFactory::CreateTimestampToken(MOER_TEXT("AutoEndCommandList"));
        submit.query_tokens.emplace_back(begin_token);
        submit.query_tokens.emplace_back(end_token);
    }
    if (inject_frame_bound) {
        frame_bound_token = AutoGpuEventTokenFactory::CreateTimestampToken(MOER_TEXT("AutoFrameBound"));
        submit.query_tokens.emplace_back(frame_bound_token);
    }

    Array<UniquePtr<Command>> rebuilt_commands{};
    rebuilt_commands.reserve(
        submit.cmds.size() +
        (inject_command_list_span ? 4u : 0u) +
        (inject_frame_bound ? 2u : 0u)
    );

    size_t body_begin = 0;
    while (body_begin < submit.cmds.size()) {
        const auto* queue_transfer =
            dynamic_cast<const QueueTransferCmd*>(submit.cmds[body_begin].get());
        if (queue_transfer == nullptr || !queue_transfer->IsImport()) {
            break;
        }
        rebuilt_commands.emplace_back(std::move(submit.cmds[body_begin]));
        ++body_begin;
    }

    size_t body_end = submit.cmds.size();
    while (body_end > body_begin) {
        const auto* queue_transfer =
            dynamic_cast<const QueueTransferCmd*>(submit.cmds[body_end - 1].get());
        if (queue_transfer == nullptr || queue_transfer->IsImport()) {
            break;
        }
        --body_end;
    }

    if (inject_command_list_span) {
        AppendTimestampPair(rebuilt_commands, begin_token, MOER_TEXT("AutoBeginCommandList"));
    }
    for (size_t cmd_index = body_begin; cmd_index < body_end; ++cmd_index) {
        rebuilt_commands.emplace_back(std::move(submit.cmds[cmd_index]));
    }
    if (inject_command_list_span) {
        AppendTimestampPair(rebuilt_commands, end_token, MOER_TEXT("AutoEndCommandList"));
    }
    if (inject_frame_bound) {
        AppendTimestampPair(rebuilt_commands, frame_bound_token, MOER_TEXT("AutoFrameBound"));
    }
    for (size_t cmd_index = body_end; cmd_index < submit.cmds.size(); ++cmd_index) {
        rebuilt_commands.emplace_back(std::move(submit.cmds[cmd_index]));
    }
    submit.cmds = std::move(rebuilt_commands);

    Array<GPUEvent> rebuilt_events{};
    rebuilt_events.reserve(
        submit.gpu_events.size() +
        (inject_command_list_span ? 2u : 0u) +
        (inject_frame_bound ? 1u : 0u)
    );
    if (inject_command_list_span) {
        rebuilt_events.emplace_back(GPUEvent{
            .type = GPUEvent::EType::BeginGPU,
            .name = MOER_TEXT("GPU"),
            .query = begin_token,
            .depth = 0,
            .timestamp_ns = 0
        });
    }
    for (auto& event : submit.gpu_events) {
        rebuilt_events.emplace_back(std::move(event));
    }
    if (inject_command_list_span) {
        rebuilt_events.emplace_back(GPUEvent{
            .type = GPUEvent::EType::EndGPU,
            .name = MOER_TEXT(""),
            .query = end_token,
            .depth = 0,
            .timestamp_ns = 0
        });
    }
    if (inject_frame_bound) {
        rebuilt_events.emplace_back(GPUEvent{
            .type = GPUEvent::EType::FrameBoundary,
            .name = MOER_TEXT("FrameBound"),
            .query = frame_bound_token,
            .depth = 0,
            .timestamp_ns = 0
        });
    }
    submit.gpu_events = std::move(rebuilt_events);
}


static TranslatePipelineBatch AssembleTranslatePipelineOps(
    Array<ExecutorOp>&&             ops,
    const PreprocessTranslateStore& preprocess_store,
    uint64                          op_seq_base
) {
    TRACE_SCOPE_CAT("Vulkan.AssembleTranslatePipelineOps", "RHI");
    TranslatePipelineBatch pipeline_batch{};
    pipeline_batch.translate_ops.reserve(EstimateSubmitCount(ops) * 3 + 1);
    pipeline_batch.submit_ops.reserve(EstimateSubmitCount(ops) * 3 + 1);

    auto build_segment_submit =
        [](CmdSubmit& source_submit,
           const RHISubmitSegment& descriptor,
           bool attach_waits,
           bool attach_signals_and_callbacks,
           bool attach_parent_runtime_payload) -> CmdSubmit {
        Array<UniquePtr<Command>> commands{};
        commands.reserve(descriptor.end - descriptor.begin);
        for (size_t cmd_index = descriptor.begin; cmd_index < descriptor.end; ++cmd_index) {
            commands.emplace_back(std::move(source_submit.cmds[cmd_index]));
        }

        Array<std::function<void(void)>> callbacks{};
        Array<QueryToken>                query_tokens{};
        Array<GPUEvent>                  gpu_events{};
        Array<WaitEvent>                 wait_events{};
        Array<SignalEvent>               signal_events{};
        Array<SyncPointRef>              wait_sync_points{};
        Array<SyncPointRef>              signal_sync_points{};

        if (attach_waits) {
            wait_events = std::move(source_submit.wait_events);
            wait_sync_points = std::move(source_submit.wait_sync_points);
        }
        if (attach_signals_and_callbacks) {
            callbacks     = std::move(source_submit.callbacks);
            signal_events = std::move(source_submit.signal_events);
            signal_sync_points = std::move(source_submit.signal_sync_points);
        }
        if (attach_parent_runtime_payload) {
            query_tokens = source_submit.query_tokens;
            gpu_events   = source_submit.gpu_events;
        }

        CmdSubmit segment_submit{
            std::move(commands),
            std::move(callbacks),
            TCachedArgArray(source_submit.cached_args),
            std::move(query_tokens),
            std::move(gpu_events)
        };
        segment_submit.wait_events = std::move(wait_events);
        segment_submit.signal_events = std::move(signal_events);
        segment_submit.wait_sync_points = std::move(wait_sync_points);
        segment_submit.signal_sync_points = std::move(signal_sync_points);
        segment_submit.translate_execution_class = source_submit.translate_execution_class;
        segment_submit.translate_complete_event  = source_submit.translate_complete_event;
        segment_submit.fence_event  = source_submit.fence_event;
        segment_submit.b_non_parallel = source_submit.b_non_parallel;
        if (attach_signals_and_callbacks) {
            segment_submit.b_sync             = source_submit.b_sync;
            segment_submit.b_tick_profiling   = source_submit.b_tick_profiling && descriptor.queue != EQueueType::Copy;
            segment_submit.b_delete_resources = source_submit.b_delete_resources;
        }
        return segment_submit;
    };

    auto inject_queue_transfer_commands =
        [](CmdSubmit& segment_submit, const TranslateInfo& translate_info) {
        if ((translate_info.prefix_import_textures.size() > 0 ||
             translate_info.prefix_import_buffers.size() > 0) &&
            translate_info.prefix_transfer_queue.has_value()) {
            segment_submit.cmds.insert(
                segment_submit.cmds.begin(),
                MakeUnique<QueueTransferCmd>(
                    translate_info.prefix_transfer_queue.value(),
                    Array<ImportTexture>(translate_info.prefix_import_textures),
                    Array<ImportBuffer>(translate_info.prefix_import_buffers)
                )
            );
        }

        if ((translate_info.suffix_export_textures.size() > 0 ||
             translate_info.suffix_export_buffers.size() > 0) &&
            translate_info.suffix_transfer_queue.has_value()) {
            segment_submit.cmds.emplace_back(
                MakeUnique<QueueTransferCmd>(
                    translate_info.suffix_transfer_queue.value(),
                    Array<ExportTexture>(translate_info.suffix_export_textures),
                    Array<ExportBuffer>(translate_info.suffix_export_buffers)
                )
            );
        }
    };

    UnorderedMap<SubmissionKey, SyncPointRef, SubmissionKeyHash> signal_syncpoint_by_key{};
    signal_syncpoint_by_key.reserve(pipeline_batch.translate_ops.capacity());
    GraphEventRef         last_submit_event{nullptr};

    uint64 op_seq = op_seq_base;
    for (auto& op : ops) {
        std::visit(
            Overload{
                [&](ExecutorSubmitOp& submit_op) {
                    for (uint32 submit_idx = 0; submit_idx < submit_op.submits.size(); ++submit_idx) {
                        auto& submit = submit_op.submits[submit_idx];
                        const SourceSubmitKey source_key{op_seq, submit_idx};
                        const auto* source_plan = preprocess_store.FindSourcePlan(source_key);
                        if (source_plan == nullptr) {
                            continue;
                        }

                        for (size_t segment_plan_index = 0;
                             segment_plan_index < source_plan->segments.size();
                             ++segment_plan_index) {
                            const auto& segment_plan = source_plan->segments[segment_plan_index];
                            const auto* translate_info = preprocess_store.Find(segment_plan.key);
                            if (translate_info == nullptr) {
                                continue;
                            }

                            CmdSubmit segment_submit = build_segment_submit(
                                submit,
                                translate_info->segment,
                                segment_plan.inherit_source_wait_events,
                                segment_plan.inherit_source_signal_events_and_callbacks,
                                segment_plan.inherit_source_runtime_payload
                            );
                            inject_queue_transfer_commands(segment_submit, *translate_info);
                            InjectAutoCommandListGpuEvents(segment_submit);

                            Array<SyncPointRef> explicit_wait_syncpoints =
                                std::move(segment_submit.wait_sync_points);
                            SyncPointRef explicit_signal_syncpoint{};
                            if (!segment_submit.signal_sync_points.empty()) {
                                explicit_signal_syncpoint = std::move(segment_submit.signal_sync_points.front());
                                segment_submit.signal_sync_points.clear();
                            }

                            QueueTranslateInfo translate_task{
                                segment_plan.key,
                                source_key,
                                static_cast<uint32>(segment_plan_index),
                                translate_info->queue,
                                std::move(segment_submit)
                            };
                            translate_task.task_dependencies = translate_info->task_dependencies;
                            translate_task.completion_event = translate_info->completion_event;
                            const uint32 translate_index =
                                static_cast<uint32>(pipeline_batch.translate_ops.size());
                            pipeline_batch.translate_ops.emplace_back(std::move(translate_task));

                            PendingSubmitTask submit_task{};
                            submit_task.key = segment_plan.key;
                            submit_task.queue = translate_info->queue;
                            submit_task.translate_index = translate_index;
                            submit_task.signal_syncpoint = explicit_signal_syncpoint ?
                                std::move(explicit_signal_syncpoint) :
                                SyncPoint::Create(ESyncPointMode::GPU);
                            submit_task.wait_syncpoints = std::move(explicit_wait_syncpoints);
                            submit_task.completion_event = GraphEvent::CreateGraphEvent();
                            AppendUniqueDependency(
                                submit_task.task_dependencies,
                                translate_info->completion_event
                            );
                            AppendUniqueDependency(
                                submit_task.task_dependencies,
                                last_submit_event
                            );
                            if (const auto* logical_waits =
                                    preprocess_store.dependency_graph.FindProducers(translate_info->key);
                                logical_waits != nullptr) {
                                submit_task.wait_syncpoints.reserve(logical_waits->size());
                                for (const SubmissionKey& dependency_key : *logical_waits) {
                                    const auto dependency_it = signal_syncpoint_by_key.find(dependency_key);
                                    if (dependency_it == signal_syncpoint_by_key.end()) {
                                        LOG_ERROR(
                                            MOER_TEXT("Submit ({}, {}) missing dependency submit ({}, {}) during submit task assembly"),
                                            translate_info->key.op_seq,
                                            translate_info->key.submit_idx,
                                            dependency_key.op_seq,
                                            dependency_key.submit_idx
                                        );
                                        assert(false && "logical wait dependency must resolve before submit task assembly");
                                        continue;
                                    }
                                    submit_task.wait_syncpoints.emplace_back(dependency_it->second);
                                }
                            }

                            signal_syncpoint_by_key.emplace(
                                submit_task.key,
                                submit_task.signal_syncpoint
                            );
                            pipeline_batch.submit_ops.emplace_back(std::move(submit_task));
                            last_submit_event =
                                pipeline_batch.submit_ops.back().completion_event;
                        }
                    }
                },
                [&](ExecutorPresentOp& present_op) {
                    SubmitPresentStage present_stage{op_seq, std::move(present_op)};
                    if (const auto* present_preprocess = preprocess_store.FindPresent(op_seq);
                        present_preprocess != nullptr) {
                        present_stage.has_source_texture_state =
                            present_preprocess->has_source_texture_state;
                        present_stage.source_texture_state = present_preprocess->source_texture_state;
                    }
                    pipeline_batch.present_ops.emplace_back(std::move(present_stage));
                }
            },
            op
        );
        ++op_seq;
    }

    return pipeline_batch;
}

} // namespace

TranslatePipelineBatch TranslatePipelineRuntime::Assemble(
    Array<ExecutorOp>&&             ops,
    const PreprocessTranslateStore& preprocess_store,
    uint64                          op_seq_base
) {
    return AssembleTranslatePipelineOps(
        std::move(ops),
        preprocess_store,
        op_seq_base
    );
}

void TranslatePipelineRuntime::Dispatch(
    TranslatePipelineBatch&& pipeline_batch,
    uint64                   trace_frame
) {
    TRACE_SCOPE_CAT("Vulkan.DispatchTranslatePipelineBatch", "RHI");

    if (pipeline_batch.translate_ops.empty() && pipeline_batch.present_ops.empty()) {
        return;
    }

    struct DispatchRequest {
        Array<QueueTranslateInfo> translate_ops{};
        Array<std::optional<PendingSubmitTask>> submit_ops{};
        Array<SubmitPresentStage>               present_ops{};
        uint64                                 batch_trace_frame{0};
    };

    auto request = std::make_shared<DispatchRequest>();
    request->translate_ops     = std::move(pipeline_batch.translate_ops);
    request->submit_ops.resize(request->translate_ops.size());
    request->present_ops       = std::move(pipeline_batch.present_ops);
    request->batch_trace_frame = trace_frame;

    for (PendingSubmitTask& submit_task : pipeline_batch.submit_ops) {
        if (submit_task.translate_index >= request->submit_ops.size()) {
            LOG_ERROR(
                MOER_TEXT("translate submit task index {} is out of range for {} translate ops"),
                submit_task.translate_index,
                request->submit_ops.size()
            );
            assert(false && "translate submit task index must be valid");
            continue;
        }
        if (request->submit_ops[submit_task.translate_index].has_value()) {
            LOG_ERROR(
                MOER_TEXT("translate submit task index {} is duplicated during batch dispatch"),
                submit_task.translate_index
            );
            assert(false && "translate submit task index must be unique");
            continue;
        }
        request->submit_ops[submit_task.translate_index].emplace(std::move(submit_task));
    }

    state.dispatch_pipe.Enqueue(
        [this, request]() mutable {
            auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
            auto  heartbeat_handle =
                thread_heartbeat.Register(MOER_TEXT("TranslateDispatchPipe"), MOER_TEXT("Begin"));
            ScopedRHITraceFrame dispatch_trace_scope(request->batch_trace_frame);
            TRACE_SCOPE_CAT("Vulkan.TranslateDispatchPipe", "RHI");

            {
                thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("AppendPresentStages"));
                std::lock_guard<std::mutex> state_lock(state.mutex);
                for (SubmitPresentStage& present_stage : request->present_ops) {
                    state.submit_state.present_ops.emplace_back(std::move(present_stage));
                }
            }

            for (size_t index = 0; index < request->translate_ops.size(); ++index) {
                thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("ScheduleTranslateTask"));
                QueueTranslateInfo       translate_info = std::move(request->translate_ops[index]);
                GraphEventArray          dependencies   = std::move(translate_info.task_dependencies);
                std::optional<PendingSubmitTask> submit_task = std::move(request->submit_ops[index]);
                std::shared_ptr<TranslateBatch> batch{};
                uint32                        batch_entry_index = 0;

                {
                    std::lock_guard<std::mutex> state_lock(state.mutex);

                    // Submit boundary detection: when source_key changes, finalize the previous submit.
                    if (state.current_submit_key != translate_info.source_key) {
                        // Finalize previous submit's last_submit_event
                        if (!state.current_submit_cl_events.empty()) {
                            GraphEventRef submit_event = GraphEvent::CreateGraphEvent();
                            for (const auto& cl_event : state.current_submit_cl_events) {
                                submit_event->WaitUntil(cl_event);
                            }
                            submit_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                            state.last_submit_event = submit_event;
                            state.current_submit_cl_events.clear();
                        }
                        state.current_submit_key = translate_info.source_key;
                    }

                    // Build dependencies: all CLs depend on last_submit + last_fence
                    AppendUniqueDependency(dependencies, state.last_submit_event);
                    AppendUniqueDependency(dependencies, state.last_fence_event);

                    // Non-parallel CLs additionally depend on all preceding CLs in this submit
                    if (translate_info.b_non_parallel) {
                        for (const auto& prev_event : state.current_submit_cl_events) {
                            AppendUniqueDependency(dependencies, prev_event);
                        }
                    }

                    const uint32 command_count = CountTranslateCommands(translate_info);
                    const bool can_append_to_current =
                        state.submit_state.current_batch != nullptr &&
                        state.submit_state.current_batch->queue == translate_info.queue &&
                        state.submit_state.current_batch->command_count + command_count <=
                            s_max_translate_batch_commands;

                    if (!can_append_to_current) {
                        if (state.submit_state.current_batch != nullptr) {
                            state.submit_state.batches.emplace_back(std::move(state.submit_state.current_batch));
                        }

                        auto new_batch = std::make_shared<TranslateBatch>();
                        new_batch->queue           = translate_info.queue;
                        new_batch->execution_class = ERHITranslateExecutionClass::Parallel;
                        new_batch->trace_frame     = request->batch_trace_frame;
                        state.submit_state.current_batch = std::move(new_batch);
                    }

                    batch = state.submit_state.current_batch;

                    std::lock_guard<std::mutex> batch_lock(batch->mutex);
                    batch_entry_index = static_cast<uint32>(batch->entries.size());
                    batch->entries.emplace_back();
                    batch->command_count += command_count;
                }

                struct TranslateBatchJob {
                    std::shared_ptr<TranslateBatch> batch{};
                    uint32                          batch_entry_index{0};
                    QueueTranslateInfo              translate_info;
                    std::optional<PendingSubmitTask> submit_task{};

                    TranslateBatchJob(
                        std::shared_ptr<TranslateBatch> in_batch,
                        uint32                          in_batch_entry_index,
                        QueueTranslateInfo&&            in_translate_info,
                        std::optional<PendingSubmitTask>&& in_submit_task
                    ) :
                        batch(std::move(in_batch)),
                        batch_entry_index(in_batch_entry_index),
                        translate_info(std::move(in_translate_info)),
                        submit_task(std::move(in_submit_task)) {}
                };

                auto job = std::make_shared<TranslateBatchJob>(
                    batch,
                    batch_entry_index,
                    std::move(translate_info),
                    std::move(submit_task)
                );

                GraphEventRef translate_event = batch->translate_pipe.Enqueue(
                    [job]() mutable {
                        auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
                        auto  heartbeat_handle = thread_heartbeat.Register(
                            MOER_TEXT("TranslateTask"),
                            MOER_TEXT("Begin")
                        );
                        ScopedRHITraceFrame translate_trace_scope(job->batch->trace_frame);
                        TRACE_SCOPE_CAT("Vulkan.TranslateDispatchTask", "RHI");

                        auto complete_translate = [&job]() {
                            if (job->translate_info.completion_event) {
                                job->translate_info.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                            }
                        };

                        VulkanAllocator* allocator_override = nullptr;
                        {
                            thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("AcquireBatchAllocator"));
                            std::lock_guard<std::mutex> batch_lock(job->batch->mutex);
                            allocator_override = job->batch->allocator_cache.get();
                        }

                        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("DispatchSingle"));
                        TranslateResult result = job->translate_info.valid ?
                                                     VulkanTranslateTask::DispatchSingle(
                                                         job->translate_info.queue,
                                                         std::move(job->translate_info.submit),
                                                         allocator_override
                                                     ) :
                                                     VulkanTranslateTask::MakeFailed(
                                                         job->translate_info.queue,
                                                         std::move(job->translate_info.error)
                                                     );
                        if (job->translate_info.completion_event) {
                            result.translate_complete = job->translate_info.completion_event;
                        }
                        if (!result.valid && !result.error.empty()) {
                            LOG_ERROR(MOER_TEXT("{}"), result.error);
                        }

                        if (!job->submit_task.has_value()) {
                            thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("NoSubmitTask"));
                            complete_translate();
                            thread_heartbeat.Unregister(heartbeat_handle);
                            return;
                        }

                        if (result.valid && !result.recorded_submit.has_value()) {
                            thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("NoRecordedSubmit"));
                            complete_translate();
                            thread_heartbeat.Unregister(heartbeat_handle);
                            return;
                        }

                        PendingSubmitTask& current_submit = job->submit_task.value();
                        SubmitInfo submit_info{
                            current_submit.key,
                            0,
                            current_submit.queue,
                            std::move(current_submit.signal_syncpoint),
                            std::move(result)
                        };
                        submit_info.wait_syncpoints = std::move(current_submit.wait_syncpoints);

                        std::lock_guard<std::mutex> batch_lock(job->batch->mutex);
                        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("CommitBatchEntry"));
                        if (job->batch->allocator_cache == nullptr &&
                            result.recorded_submit.has_value() &&
                            result.recorded_submit->allocator_owner != nullptr) {
                            job->batch->allocator_cache = std::move(result.recorded_submit->allocator_owner);
                        }
                        job->batch->entries[job->batch_entry_index].submit.emplace(std::move(submit_info));
                        complete_translate();
                        thread_heartbeat.Unregister(heartbeat_handle);
                    },
                    std::move(dependencies),
                    EThread::AnyThread_NormalPri
                );

                {
                    std::lock_guard<std::mutex> batch_lock(batch->mutex);
                    batch->entries[batch_entry_index].translate_event = translate_event;
                }

                // Track CL completion for submit-level dependency chain
                {
                    std::lock_guard<std::mutex> state_lock(state.mutex);
                    state.current_submit_cl_events.push_back(translate_event);

                    // RHIFence: update last_fence_event for subsequent CLs
                    if (translate_info.fence_event) {
                        GraphEventRef chained_fence = GraphEvent::CreateGraphEvent();
                        chained_fence->WaitUntil(translate_event);
                        chained_fence->WaitUntil(translate_info.fence_event);
                        chained_fence->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                        state.last_fence_event = chained_fence;
                    }
                }
            }

            // Finalize the last submit's event chain
            {
                std::lock_guard<std::mutex> state_lock(state.mutex);
                if (!state.current_submit_cl_events.empty()) {
                    GraphEventRef submit_event = GraphEvent::CreateGraphEvent();
                    for (const auto& cl_event : state.current_submit_cl_events) {
                        submit_event->WaitUntil(cl_event);
                    }
                    submit_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                    state.last_submit_event = submit_event;
                    state.current_submit_cl_events.clear();
                }
            }

            thread_heartbeat.Unregister(heartbeat_handle);
        },
        {},
        EThread::AnyThread_NormalPri
    );
}

void TranslatePipelineRuntime::EnqueuePendingSubmits(
    VulkanSubmissionRuntime& submission_runtime,
    GraphEventRef            completion_event
) {
    state.dispatch_pipe.Enqueue(
        [this, &submission_runtime, completion_event]() mutable {
            auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
            auto  heartbeat_handle =
                thread_heartbeat.Register(MOER_TEXT("TranslateSubmitRequest"), MOER_TEXT("Begin"));
            TRACE_SCOPE_CAT("Vulkan.TranslateSubmitRequest", "RHI");

            Array<std::shared_ptr<TranslateBatch>> batches{};
            Array<SubmitPresentStage>              present_ops{};
            {
                thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("MoveSubmitState"));
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.submit_state.current_batch != nullptr) {
                    state.submit_state.batches.emplace_back(std::move(state.submit_state.current_batch));
                }

                batches = std::move(state.submit_state.batches);
                state.submit_state.batches.clear();
                present_ops = std::move(state.submit_state.present_ops);
                state.submit_state.present_ops.clear();
                state.last_submit_event = nullptr;
                state.last_fence_event = nullptr;
                state.current_submit_cl_events.clear();
            }

            if (!batches.empty() || !present_ops.empty()) {
                thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("EnqueueSubmissionRuntime"));
                submission_runtime.Enqueue(std::move(batches), std::move(present_ops));
            }

            if (completion_event) {
                completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
            }
            thread_heartbeat.Unregister(heartbeat_handle);
        },
        {},
        EThread::AnyThread_NormalPri
    );
}

void TranslatePipelineRuntime::FlushDispatch() {
    if (GraphEventRef boundary = state.dispatch_pipe.Close(); boundary) {
        boundary->Wait(EThread::UNKNOWN_THREAD);
    }
}

} // namespace Moer::Render
