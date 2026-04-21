#include "VulkanSubmissionExecutorPrivate.h"

namespace Moer::Render {

struct AutoGpuEventTokenFactory {
    static QueryToken CreateTimestampToken(std::string_view name) {
        static std::atomic<uint64_t> next_auto_query_id{1ull << 62u};
        const uint64_t token_id = next_auto_query_id.fetch_add(1, std::memory_order_relaxed);
        return QueryToken(token_id, QueryKind::Timestamp, std::string(name), QueryFuture::Create());
    }
};

namespace {

static bool HasGpuEventType(const CmdSubmit& submit, GPUEvent::EType type) {
    return std::any_of(submit.gpu_events.begin(), submit.gpu_events.end(), [type](const GPUEvent& event) {
        return event.type == type;
    });
}

static void AppendTimestampPair(
    Array<UniquePtr<Command>>& commands,
    const QueryToken&          token,
    std::string_view           name
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
        submit.b_tick_profiling && !(has_begin_command_list && has_end_command_list);
    const bool inject_frame_bound = submit.b_tick_profiling && !has_frame_bound;
    if (!inject_command_list_span && !inject_frame_bound) {
        return;
    }

    QueryToken begin_token{};
    QueryToken end_token{};
    QueryToken frame_bound_token{};

    if (inject_command_list_span) {
        begin_token = AutoGpuEventTokenFactory::CreateTimestampToken("AutoBeginCommandList");
        end_token   = AutoGpuEventTokenFactory::CreateTimestampToken("AutoEndCommandList");
        submit.query_tokens.emplace_back(begin_token);
        submit.query_tokens.emplace_back(end_token);
    }
    if (inject_frame_bound) {
        frame_bound_token = AutoGpuEventTokenFactory::CreateTimestampToken("AutoFrameBound");
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
        AppendTimestampPair(rebuilt_commands, begin_token, "AutoBeginCommandList");
    }
    for (size_t cmd_index = body_begin; cmd_index < body_end; ++cmd_index) {
        rebuilt_commands.emplace_back(std::move(submit.cmds[cmd_index]));
    }
    if (inject_command_list_span) {
        AppendTimestampPair(rebuilt_commands, end_token, "AutoEndCommandList");
    }
    if (inject_frame_bound) {
        AppendTimestampPair(rebuilt_commands, frame_bound_token, "AutoFrameBound");
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
            .name = "GPU",
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
            .name = "",
            .query = end_token,
            .depth = 0,
            .timestamp_ns = 0
        });
    }
    if (inject_frame_bound) {
        rebuilt_events.emplace_back(GPUEvent{
            .type = GPUEvent::EType::FrameBoundary,
            .name = "FrameBound",
            .query = frame_bound_token,
            .depth = 0,
            .timestamp_ns = 0
        });
    }
    submit.gpu_events = std::move(rebuilt_events);
}

static TrackerSeed BuildTrackerSeed(const ResourceStateSnapshot& snapshot) {
    TrackerSeed seed{};
    seed.textures.reserve(snapshot.size());
    seed.buffers.reserve(snapshot.size());

    for (const auto& [resource_key, value] : snapshot) {
        if (resource_key.type == ETrackedResourceType::Texture) {
            auto* texture = reinterpret_cast<VulkanTexture*>(resource_key.handle);
            if (texture == nullptr) {
                continue;
            }

            TrackerSeedTextureEntry entry{};
            entry.known         = value.known;
            entry.has_writer    = value.has_writer;
            entry.owner_queue   = value.owner_queue;
            entry.texture_state = value.texture_state;
            entry.texture       = texture;
            entry.mip_level     = resource_key.mip_level;
            entry.mip_count     = ResolveTextureMipCount(texture, resource_key);
            entry.array_layer   = resource_key.array_layer;
            entry.array_count   = ResolveTextureArrayCount(texture, resource_key);
            seed.textures.push_back(entry);
            continue;
        }

        if (resource_key.type == ETrackedResourceType::Buffer) {
            auto* buffer = reinterpret_cast<VulkanBuffer*>(resource_key.handle);
            if (buffer == nullptr) {
                continue;
            }

            TrackerSeedBufferEntry entry{};
            entry.known        = value.known;
            entry.has_writer   = value.has_writer;
            entry.owner_queue  = value.owner_queue;
            entry.buffer_state = value.buffer_state;
            entry.buffer       = buffer;
            seed.buffers.push_back(entry);
        }
    }

    return seed;
}

static TranslatePipelineBatch AssembleTranslatePipelineOps(
    Array<ExecutorOp>&&             ops,
    const PreprocessTranslateStore& preprocess_store,
    uint64                          op_seq_base,
    std::atomic_uint64_t&           next_syncpoint_id
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
        if (descriptor.UsesCopyScope()) {
            UniquePtr<Command> copy_scope_holder = std::move(source_submit.cmds[descriptor.copy_scope_index]);
            auto* copy_scope = static_cast<CopyScopeCmd*>(copy_scope_holder.get());
            commands = copy_scope->StealCommands();
        } else {
            commands.reserve(descriptor.end - descriptor.begin);
            for (size_t cmd_index = descriptor.begin; cmd_index < descriptor.end; ++cmd_index) {
                commands.emplace_back(std::move(source_submit.cmds[cmd_index]));
            }
        }

        Array<std::function<void(void)>> callbacks{};
        Array<QueryToken>                query_tokens{};
        Array<GPUEvent>                  gpu_events{};
        Array<WaitEvent>                 wait_events{};
        Array<SignalEvent>               signal_events{};

        if (attach_waits) {
            wait_events = std::move(source_submit.wait_events);
        }
        if (attach_signals_and_callbacks) {
            callbacks     = std::move(source_submit.callbacks);
            signal_events = std::move(source_submit.signal_events);
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
        segment_submit.translate_execution_class = source_submit.translate_execution_class;
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

    UnorderedMap<SubmissionKey, SyncPointId, SubmissionKeyHash> signal_syncpoint_by_key{};
    signal_syncpoint_by_key.reserve(pipeline_batch.translate_ops.capacity());
    std::optional<size_t> last_submit_index{};
    EQueueType            last_submit_queue = EQueueType::Ignore;
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

                            QueueTranslateInfo translate_task{
                                segment_plan.key,
                                source_key,
                                static_cast<uint32>(segment_plan_index),
                                translate_info->queue,
                                std::move(segment_submit),
                                BuildTrackerSeed(translate_info->initial_state_snapshot)
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
                            submit_task.signal_syncpoint =
                                next_syncpoint_id.fetch_add(1, std::memory_order_relaxed);
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
                                            "Submit ({}, {}) missing dependency submit ({}, {}) during submit task assembly",
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
                            last_submit_index = pipeline_batch.submit_ops.size() - 1;
                            last_submit_queue = translate_info->queue;
                            last_submit_event =
                                pipeline_batch.submit_ops.back().completion_event;
                        }
                    }
                },
                [&](ExecutorPresentOp& present_op) {
                    std::optional<SubmitPresentStage> present_stage_holder{};
                    present_stage_holder.emplace(op_seq, std::move(present_op));
                    auto& present_stage = present_stage_holder.value();
                    if (const auto* present_preprocess = preprocess_store.FindPresent(op_seq);
                        present_preprocess != nullptr) {
                        present_stage.has_source_texture_state =
                            present_preprocess->has_source_texture_state;
                        present_stage.source_texture_state = present_preprocess->source_texture_state;
                    }
                    if (!last_submit_index.has_value() || last_submit_queue != EQueueType::Graphics) {
                        present_stage.valid = false;
                        present_stage.error =
                            "Present requires the last translated submission to be Graphics";
                        LOG_ERROR("{}", present_stage.error);
                    } else {
                        auto& parent_submit = pipeline_batch.submit_ops[last_submit_index.value()];
                        if (parent_submit.present_stage.has_value()) {
                            present_stage.valid = false;
                            present_stage.error =
                                "Multiple present operations attempted to attach to the same submit";
                            LOG_ERROR(
                                "Multiple present operations attempted to attach to submit ({}, {})",
                                parent_submit.key.op_seq,
                                parent_submit.key.submit_idx
                            );
                        }
                        parent_submit.present_stage = std::move(present_stage_holder);
                    }
                }
            },
            op
        );
        ++op_seq;
    }

    return pipeline_batch;
}

static void DispatchTranslatePipelineBatch(
    TranslatePipelineBatch&& pipeline_batch,
    uint64                   trace_frame,
    TaskPipe&                translate_dispatch_pipe,
    TaskPipe&                translate_pipe,
    VulkanSubmissionRuntime& submission_runtime
) {
    TRACE_SCOPE_CAT("Vulkan.DispatchTranslatePipelineBatch", "RHI");

    if (pipeline_batch.translate_ops.empty() && pipeline_batch.submit_ops.empty()) {
        return;
    }

    struct PipelineRuntimeState {
        Array<QueueTranslateInfo> translate_ops{};
        Array<PendingSubmitTask>  submit_ops{};
        Array<TranslateResult>    translate_results{};
        Array<std::optional<SubmitInfo>> assembled_submit_infos{};
        uint64                    trace_frame{0};
    };

    auto runtime_state = std::make_shared<PipelineRuntimeState>();
    runtime_state->translate_ops          = std::move(pipeline_batch.translate_ops);
    runtime_state->submit_ops             = std::move(pipeline_batch.submit_ops);
    runtime_state->translate_results.resize(runtime_state->translate_ops.size());
    runtime_state->assembled_submit_infos.resize(runtime_state->submit_ops.size());
    runtime_state->trace_frame = trace_frame;

    for (size_t index = 0; index < runtime_state->translate_ops.size(); ++index) {
        auto& translate_task = runtime_state->translate_ops[index];
        if (!translate_task.completion_event) {
            translate_task.completion_event = GraphEvent::CreateGraphEvent();
        }

        translate_dispatch_pipe.Enqueue(
            [runtime_state, index]() mutable {
                ScopedRHITraceFrame trace_scope(runtime_state->trace_frame);
                TRACE_SCOPE_CAT("Vulkan.TranslateDispatchPipe", "RHI");

                auto& current = runtime_state->translate_ops[index];
                auto dispatch = LambdaTask::Create(
                    [runtime_state, index]() mutable {
                        ScopedRHITraceFrame trace_scope(runtime_state->trace_frame);
                        TRACE_SCOPE_CAT("Vulkan.TranslateDispatchTask", "RHI");

                        auto& current = runtime_state->translate_ops[index];
                        TranslateResult result = current.valid ?
                                                     VulkanTranslateTask::DispatchSingle(
                                                         current.queue,
                                                         std::move(current.submit),
                                                         std::move(current.initial_seed)
                                                     ) :
                                                     VulkanTranslateTask::MakeFailed(
                                                         current.queue,
                                                         std::move(current.error)
                                                     );
                        result.translate_complete = current.completion_event;
                        if (!result.valid && !result.error.empty()) {
                            LOG_ERROR("{}", result.error);
                        }
                        runtime_state->translate_results[index] = std::move(result);
                    },
                    EThread::AnyThread_NormalPri
                );
                if (!current.task_dependencies.empty()) {
                    dispatch.Wait(std::move(current.task_dependencies));
                }
                dispatch.Next(current.completion_event).Dispatch();
            },
            {},
            EThread::AnyThread_NormalPri
        );
    }

    for (size_t index = 0; index < runtime_state->submit_ops.size(); ++index) {
        auto& submit_task = runtime_state->submit_ops[index];
        if (!submit_task.completion_event) {
            submit_task.completion_event = GraphEvent::CreateGraphEvent();
        }
        translate_pipe.Enqueue(
            [runtime_state, index]() mutable {
                ScopedRHITraceFrame trace_scope(runtime_state->trace_frame);
                TRACE_SCOPE_CAT("Vulkan.TranslatePipe", "RHI");

                auto& current = runtime_state->submit_ops[index];
                const GraphEventRef completion_event = current.completion_event;
                TranslateResult translate_output =
                    current.translate_index < runtime_state->translate_results.size() ?
                        std::move(runtime_state->translate_results[current.translate_index]) :
                        VulkanTranslateTask::MakeFailed(
                            current.queue,
                            "translate result index mismatch during submit task dispatch"
                        );

                if (translate_output.valid && !translate_output.recorded_submit.has_value()) {
                    if (completion_event) {
                        completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                    }
                    return;
                }

                runtime_state->assembled_submit_infos[index].emplace(SubmitInfo{
                    current.key,
                    0,
                    current.queue,
                    current.signal_syncpoint,
                    std::move(translate_output)
                });
                SubmitInfo& submit_info = runtime_state->assembled_submit_infos[index].value();
                submit_info.wait_syncpoints = current.wait_syncpoints;
                if (current.present_stage.has_value()) {
                    submit_info.present_stage = std::move(current.present_stage);
                }
                if (completion_event) {
                    completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                }
            },
            std::move(submit_task.task_dependencies),
            EThread::AnyThread_NormalPri
        );
    }

    if (!runtime_state->submit_ops.empty()) {
        GraphEventArray handoff_dependencies{};
        if (const GraphEventRef& last_submit_completion =
                runtime_state->submit_ops.back().completion_event;
            last_submit_completion) {
            handoff_dependencies.emplace_back(last_submit_completion);
        }

        // Batch completion stays explicit as task-graph dependencies on the final
        // serial handoff task instead of blocking a translate-pipe closure with a raw wait.
        translate_pipe.Enqueue(
            [runtime_state, &submission_runtime]() mutable {
                ScopedRHITraceFrame trace_scope(runtime_state->trace_frame);
                TRACE_SCOPE_CAT("Vulkan.TranslateRuntimeHandoff", "RHI");

                Array<SubmitInfo> submit_infos{};
                submit_infos.reserve(runtime_state->assembled_submit_infos.size());
                for (auto& submit_info : runtime_state->assembled_submit_infos) {
                    if (!submit_info.has_value()) {
                        continue;
                    }
                    submit_infos.emplace_back(std::move(submit_info.value()));
                }
                if (!submit_infos.empty()) {
                    submission_runtime.Enqueue(std::move(submit_infos));
                }
            },
            std::move(handoff_dependencies),
            EThread::AnyThread_NormalPri
        );
    }
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
        op_seq_base,
        next_syncpoint_id
    );
}

void TranslatePipelineRuntime::Dispatch(
    TranslatePipelineBatch&& pipeline_batch,
    uint64                   trace_frame,
    TaskPipe&                translate_dispatch_pipe,
    TaskPipe&                translate_pipe,
    VulkanSubmissionRuntime& submission_runtime
) {
    DispatchTranslatePipelineBatch(
        std::move(pipeline_batch),
        trace_frame,
        translate_dispatch_pipe,
        translate_pipe,
        submission_runtime
    );
}

} // namespace Moer::Render
