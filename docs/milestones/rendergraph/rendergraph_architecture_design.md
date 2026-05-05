# RenderGraph Architecture Design

## Goal and Constraints

RenderGraph owns frame-local render work construction, precise resource tracking, barrier compilation, queue synchronization, transient resource allocation, GPU trace scope compilation, command recording, and RHIExecutor submit orchestration.

Hard constraints:

- The existing RenderGraph surface is replaced in place. No compatibility layer is kept for the old Builder/AddGraphicPass/Execute API.
- Raw RHI persistent tracking is whole-resource only. Texture subresource and buffer range correctness are RenderGraph responsibilities.
- Different graphics APIs have different read/read compatibility, write/write ordering, layout transition, queue ownership, and aliasing rules. RenderGraph owns API-neutral intent compilation; backend policy owns final barrier lowering.
- Bindless handles do not imply dependencies. Every bindless read or write must be declared in setup data.
- `AddPass` has exactly one execution lambda. It records commands only.
- `AddPass` execution lambdas record abstract RHI commands into `RHICommandList`. They never receive backend platform command-list handles.
- Platform command-list recording happens only inside RHI backend translate/visitor code after RHIExecutor has selected a backend queue and allocated a native command list.
- Pass parameters are graph-owned through `graph.Alloc<T>()`.
- Setup work is separate and uses `graph.AddSetupPass(...)` plus setup context APIs.
- GPU trace scopes are declared outside execution lambdas with `RG_EVENT_SCOPE(graph, name, ...)`.
- `graph.Dispatch()` owns setup, compile, record, and submit orchestration.
- RHIExecutor remains the only submission and translate pipeline owner.
- Native custom work that does not record through `RHICommandList`, such as NVAPI or DirectML-style command-list extensions, is represented as a typed external pass with explicit resource access. The initial supported external pass kind is platform-command-list interaction only.
- Platform-command-list external pass callbacks are stored as typed RHI commands during RG record and executed during backend translate/platform record. RG does not execute those callbacks directly.
- Direct GPU runtime external passes that submit or synchronize outside the platform command list are out of scope for the initial design.
- Raw RHI work before or after RenderGraph crosses the graph boundary only through explicit import/export receipts containing whole-resource state, owner queue, and completion boundary.
- Multiple RenderGraph instances in one frame share a frame context and global pool, but each graph keeps its own CPU allocator and transient alias allocator. Cross-graph resource flow is explicit export/import, not hidden state sharing.
- Debug single-step mode is controlled by console variables sampled outside the graph scope. It cannot be toggled while a RenderGraph is being constructed or dispatched.
- Validation dumps are read-only outputs from frozen setup data or compiled plans. They must not mutate graph state or change scheduling.

Selected public shape:

`DeclareRGAccess` must be a `const` method so RenderGraph can extract dependencies without mutating frozen parameter data.

```cpp
struct MyPassParameters {
    RGTextureView input;
    RGTextureView output;

    void DeclareRGAccess(RGParameterAccessCollector& collector) const {
        collector.AddTexture(input);
        collector.AddTexture(output);
    }
};

auto* param = graph.Alloc<MyPassParameters>();
param->input = RGTextureView{
    .handle = input_handle,
    .access = ERGAccessMode::Read,
    .state = Render::ETextureState::SHADER_RESOURCE
};
param->output = RGTextureView{
    .handle = output_handle,
    .access = ERGAccessMode::Write,
    .state = Render::ETextureState::RENDER_TARGET
};

RG_EVENT_SCOPE(graph, MOER_TEXT("Lighting"));

graph.AddSetupPass(MOER_TEXT("LightingPrepare"), [param](RGSetupContext& setup) {
    BuildLightingCpuTables(param);
});

graph.AddPass("Lighting", param, ERGPassFlags::Graphics, [](RHICommandList& cmd_list, RGContext context) {
    const auto& resources = context.Resources<MyPassParameters>();
    cmd_list.Gfx(resources.pipeline, resources.input_srv, resources.output_rtv);
});

graph.Dispatch();
```

Selected platform external shape:

```cpp
struct MyPlatformPassParameters {
    RGTextureView input;
    RGTextureView output;

    void DeclareRGAccess(RGParameterAccessCollector& collector) const {
        collector.AddTexture(input);
        collector.AddTexture(output);
    }
};

auto* param = graph.Alloc<MyPlatformPassParameters>();
param->input = RGTextureView{.handle = input_handle, .access = ERGAccessMode::Read, .state = Render::ETextureState::SHADER_RESOURCE};
param->output = RGTextureView{.handle = output_handle, .access = ERGAccessMode::Write, .state = Render::ETextureState::UNORDERED_ACCESS};

graph.AddExternalPass(
    param,
    ERGExternalPassKind::PlatformCommandList,
    ERGPassFlags::Compute,
    [param](RGPlatformCommandListContext& context) {
        context.Native<BackendNativeCommandList>().RecordNativeDenoise(
            context.NativeResource(param->input),
            context.NativeResource(param->output)
        );
    }
);
```

Record boundary decision:

- RG record stage records into `RHICommandList` only.
- RHI translate/platform-record stage records into backend command lists such as `VkCommandBuffer` or `ID3D12GraphicsCommandList`.
- `AddExternalPass(PlatformCommandList)` captures a backend callback and declared native requirements, but RG record emits a typed RHI platform callback command instead of invoking the callback.
- Backend visitors execute the callback when the platform command list and backend resource handles are valid.
- This keeps RG portable, keeps RHIExecutor as translate owner, and gives external passes a precise insertion point between RG-compiled barriers.

## Top-Down Module Tree

### RenderGraph

Reason: one frame needs one owner for pass ordering, resource lifetime, barriers, scopes, and submit shape.

Children:

- `RGFrameContext`: owns frame-sequence interop state shared by raw RHI work and multiple RenderGraph instances.
- `RGFrameAllocator`: owns frame-local CPU memory.
- `RGResourceRegistry`: owns logical texture, buffer, import, export, and view descriptors.
- `RGPassRegistry`: owns setup passes, execution passes, parameter pointers, parameter-declared access views, and pass flags.
- `RGExternalPassRegistry`: owns custom non-RHI execution nodes and native interop requirements.
- `RGScopeRegistry`: owns graph-level GPU trace scope metadata.
- `RGCompiler`: produces the immutable compiled plan.
- `RGBarrierPolicy`: describes backend-specific read compatibility, write ordering, layout transition, queue transfer, and alias barrier rules.
- `RGResourceAllocator`: owns global pooled resources and frame-local transient aliasing.
- `RGRecorder`: records execution lambdas into single-writer RHICommandLists.
- `RGDispatcher`: runs setup, compile, record, and RHIExecutor submit.
- `RHIPlatformCommandCallback`: typed RHI command that defers platform-command-list callbacks to backend translate.
- `RGDebugController`: snapshots debug console variables and selects normal or single-step execution.
- `RGValidationLayer`: validates and dumps parameters, resources, barriers, fences, and formatted graph outputs.

### RGFrameContext

Reason: raw RHI work, external native work, and multiple RenderGraph instances can appear in one frame and need one explicit boundary owner.

State:

- Frame sequence id and graph sequence ids.
- Latest exported whole-resource state and owner queue for resources that leave a graph.
- Completion boundaries returned by RHIExecutor, including command lists that contain platform-command-list external passes.
- Shared `RGGlobalResourcePool` reference for the RHI device.
- Debug validation table that records which boundary last owns each imported/exported resource.

Boundary:

- Renderers create one `RGFrameContext` per frame.
- Every `RenderGraph` in the frame is constructed with that context.
- Raw RHI code can consume only exported receipts or explicitly declared imported states.
- The context does not merge independent graphs. Cross-graph dependencies are explicit export/import edges.

Leaf operations:

- Register a raw RHI submit receipt as a graph import source.
- Publish a graph dispatch receipt for later raw RHI or RenderGraph imports.
- Reject importing a resource whose previous producer boundary is incomplete unless a wait is compiled.
- Reject hidden reuse of graph-local transient resources across two graph instances.

### RGFrameAllocator

Reason: setup, compile, and parallel record need stable lifetimes without per-pass heap churn.

State:

- Setup arena: graph nodes, logical resources, parameter objects with RG resource views, scope declarations.
- Compile arena: hazard edges, lifetime intervals, barrier batches, queue segments, submit plan, resolved tables.
- Scratch arena: temporary sort keys, overlap lists, work queues.

Boundary:

- `Alloc<T>()` constructs `T` in setup arena and returns `T*`.
- Setup arena freezes before compile.
- Compile arena freezes before record.
- Frame teardown resets arenas after record tasks finish and RHIExecutor accepts command lists.

Leaf operations:

- Allocate with bump-pointer pages sized for cache-local linear writes.
- Store parameter type id and byte span beside each pass for debug validation.
- Reject `Alloc<T>()` after setup freeze.

### RGResourceRegistry

Reason: RenderGraph needs stable logical resources before physical allocation and view resolution.

State:

- `RGTextureDesc`, `RGBufferDesc` for created resources.
- Imported RHI handles with declared initial state, queue owner, and external lifetime flag.
- Export requests with required final state and owner.
- View descriptors keyed by logical resource, subresource range, format override, and usage.

Boundary:

- Setup context creates/imports logical resources only.
- Execution lambdas receive resolved RHI views through `RGContext` only.

Leaf operations:

- Normalize texture subresource range: expand remaining mip/array counts once during setup.
- Normalize buffer range: explicit offset and size; whole-buffer is stored as `{0, buffer_size}`.
- Validate first read of imported or external resources requires known state.
- Validate bindless usage by matching declared resource handles against bindless descriptor writes.

### RGPassRegistry

Reason: pass declarations are the only source of dependency and queue planning data.

State:

- `RGSetupPass`: name and setup lambda that runs before pass execution.
- `RGPass`: parameter pointer, pass flags, execution lambda, parameter-declared accesses, scope membership.
- Access arrays are structure-of-arrays for fast compile scans.

Boundary:

- `AddSetupPass` stores CPU preparation work that RG schedules before all pass execution lambdas.
- `AddPass` stores parameter pointer, flags, and one execution lambda.
- `AddPass` does not accept setup callbacks, resource declaration callbacks, or queue transfer callbacks.

Leaf operations:

- Convert pass flags to one execution domain: Graphics, Compute, Copy, or Raytracing.
- Extract each access from graph-owned pass parameters as `{pass_id, resource_id, range, access_mode, state, queue_intent, bindless_flag}`.
- Reject a pass with zero execution domain or multiple execution domains.
- Reject execution lambda mutation of graph structure by exposing only `RHICommandList&` and read-only `RGContext`.

### RGExternalPassRegistry

Reason: some GPU work is expressed through backend platform command-list extensions rather than through `RHICommandList`, but it still participates in resource hazards, queue ordering, and allocator lifetime.

State:

- External pass id, name, parameter pointer, external execution kind, declared access list, and callback pointer.
- Initial supported kind: `PlatformCommandList`.
- Future non-goal kind: direct GPU runtime interaction with independent submit/sync.
- Required native capabilities: backend, native command-list handle class, native resource handle class, memory sharing mode, and command-list queue type.
- Completion contract: the containing platform command list and its RHIExecutor submit boundary.

Boundary:

- External passes use a separate `AddExternalPass` surface and never receive `RHICommandList&`.
- External pass setup uses the same resource declaration model as normal passes.
- External callbacks receive `RGPlatformCommandListContext` with the backend platform command-list handle, resolved native resource handles, and resolved descriptor/native state data, but only when executed by backend translate/platform record.
- External callbacks must not allocate graph resources, mutate graph structure, or submit RHICommandLists.
- External callbacks must not signal or wait external GPU timelines in the initial design.

Leaf operations:

- Validate backend capability before compile accepts the pass.
- Place the external pass inside a compiled command-list segment compatible with its required queue type.
- Emit graph barriers before and after the callback command in the containing RHI command list.
- During RG record, emit one typed RHI platform callback command with resolved resource ids and native capability metadata.
- Reject external access to transient or pooled resources when native sharing, descriptor lifetime, or synchronization cannot be proven.
- Reject direct-GPU external passes until a later milestone explicitly adds independent submit/sync tokens.

### RHIPlatformCommandCallback

Reason: platform command-list callbacks need backend-native handles that do not exist during RG record.

State:

- External pass name and stable pass id.
- Backend requirement: Vulkan, D3D12, or future backend.
- Queue requirement: graphics or compute in the initial design.
- Native command-list callback.
- Resolved native resource handle table produced by RG compile.
- Declared resource usage for RHI validation and backend preprocess.

Boundary:

- RG emits this as an RHI command into `RHICommandList`.
- RHI preprocess can inspect declared resource usage without executing the callback.
- Backend visitor executes the callback after pending barriers are emitted and before following commands.
- The command does not submit, wait, signal, or own completion.

Leaf operations:

- Validate queue compatibility before inserting into a segment.
- Expose a backend-specific native command list through `RGPlatformCommandListContext`.
- Expose only resources declared by the external pass setup.
- Invalidate and restore backend descriptor/native state around the callback when required by the backend.
- Report callback name and resource usage to validation dumps.

### RGDebugController

Reason: synchronization bugs need a slow deterministic mode where pass declaration, record, submit, and GPU completion happen one step at a time.

State:

- Snapshot of `RenderGraph.Debug.SingleStep` and validation dump console variables.
- Graph debug mode epoch captured before graph construction starts.
- Single-step boundary receipt after each executed pass.

Console variables:

- `RenderGraph.Debug.SingleStep`: enable declaration-time record, submit, and GPU sync.
- `RenderGraph.Validation.DumpParameters`: dump graph-owned pass parameter metadata.
- `RenderGraph.Validation.DumpResources`: dump RG resources and resolved RHI resources.
- `RenderGraph.Validation.DumpSyncGraph`: dump barrier/fence text tables and DOT graphs.
- `RenderGraph.Validation.DumpDirectory`: output directory for validation dump files.

Boundary:

- Console variables are sampled when a `RenderGraph` begins construction or when `RGFrameContext` opens a graph scope.
- Changing debug mode while a graph is active is rejected or deferred to the next graph.
- Single-step mode is a debug-only execution mode and is not a scheduling model for shipping frame execution.

Leaf operations:

- In normal mode, compile and record follow the asynchronous path.
- In single-step mode, `AddSetupPass` executes declaration work immediately.
- In single-step mode, `AddPass` immediately compiles the pass against the current boundary state, records one command list, submits through RHIExecutor, waits for GPU completion, and publishes a new boundary receipt.
- In single-step mode, external platform-command-list passes record into the same immediate command-list segment and sync through the same RHIExecutor boundary.
- Disable async setup, async compile, parallel record, transient aliasing across later passes, and cross-pass barrier batching in single-step mode.

### RGValidationLayer

Reason: resource and synchronization diagnostics must show the graph's declared data and compiled barrier/fence plan without requiring backend tracker reverse engineering.

State:

- Dump options from console variables.
- Stable names for graph, pass, resource, RHI handle, queue, barrier, fence, and boundary token ids.
- Snapshot views of graph parameters, RG resources, resolved RHI resources, barriers, queue handoffs, fences, and external platform-command-list nodes.

Boundary:

- Validation reads frozen setup data or immutable compiled plans only.
- Validation output is deterministic and does not change scheduling, allocation, or barrier decisions.
- Validation output can be enabled per graph through console variables sampled outside graph scope.

Leaf operations:

- Print `graph.Alloc<T>()` parameter metadata: type id, byte size, owning pass, and referenced RG resources.
- Print RG resource descriptors, import/export state, lifetime interval, alias slot, pool eligibility, and resolved RHI resource identity.
- Print resolved RHI resource data: debug name, backend handle class, usage flags, last known whole-resource state, owner queue, and pool entry id when available.
- Dump barrier and fence plans as deterministic text tables plus a formatted DOT graph.
- Include external platform-command-list nodes, raw RHI boundary receipts, and multi-graph import/export edges in the graph output.

### RGScopeRegistry

Reason: a logical GPU scope can cover passes that compile into multiple command lists or queues.

State:

- Scope id, parent id, name, begin pass id, end pass id, and declared ordering requirements.
- Per-pass scope membership resolved before compile.
- Compiled emission records for begin/end marker placement.

Boundary:

- `RG_EVENT_SCOPE(graph, name, ...)` declares graph-level scope metadata outside execution lambdas.
- Pass-local `GPU_PROFILE_EVENT_SCOPE(cmd_list, name)` remains valid only inside one command list segment.

Leaf operations:

- Assign monotonic scope ids during setup.
- Convert lexical or explicit scope lifetime to begin/end pass ids.
- Reject graph scope crossing unordered queue work without a synchronization edge.
- Emit graph-level GPU events with explicit scope id metadata, then let GPUEventStream reconstruct logical nesting.

### RGCompiler

Reason: recording must be a straight-line execution of an immutable plan.

Children:

- Dependency compiler.
- Barrier compiler.
- Lifetime compiler.
- Queue compiler.
- Interop boundary compiler.
- External pass compiler.
- Scope compiler.
- Submit-plan compiler.

State:

- Immutable `RGCompiledGraph` containing sorted passes, external nodes, hazard edges, barriers, physical allocations, command-list segments, boundary tokens, scope emissions, and submit plan.

Boundary:

- Input is frozen setup data.
- Output is read-only data consumed by record tasks.

Leaf operations:

- Sort accesses by resource id, range key, then pass id.
- Build hazard edges from write-after-read, read-after-write, write-after-write, queue owner change, and external export requirements.
- Batch adjacent barriers when resource, source state, destination state, queue transition, and pass insertion point match.
- Assign transient physical resources by interval coloring.
- Segment passes into queue-compatible command-list ranges.
- Insert external nodes and raw RHI import/export boundary tokens.
- Produce explicit waits/signals for cross-queue edges.

### RGBarrierPolicy

Reason: Vulkan, D3D12, and future APIs do not share exact transition rules, but pass code must stay API-neutral.

State:

- Read compatibility classes: states that can be merged without a layout transition for a backend.
- Write ordering classes: writes that require memory ordering even when the abstract state name is unchanged.
- Layout classes: backend layout/state representation for texture states.
- Queue ownership rules: whether a queue change requires ownership transfer, only timeline order, or no operation.
- Alias rules: backend requirements when one physical allocation backs two logical resources.

Boundary:

- RenderGraph common compile emits `RGBarrierIntent` records.
- Backend policy lowers intents into RHI barrier commands and submit waits/signals.
- Pass setup and execution lambdas never call backend-specific transition helpers.

Leaf operations:

- `ClassifyRead(access)` returns a backend read compatibility class.
- `ClassifyWrite(access)` returns a backend write ordering class.
- `NeedsReadToReadTransition(prev_read, next_read)` returns true only when layout/state class differs.
- `NeedsWriteToWriteOrder(prev_write, next_write)` returns true when previous contents or write ordering are observable.
- `CanDiscardPreviousState(next_write)` returns true for full-range discard writes and clears.
- `LowerBarrierIntent(intent)` maps API-neutral intent to Vulkan image/buffer barriers, D3D12 enhanced barriers, or future API barriers.

Encapsulation decision:

- The whole flow is encapsulated in `RGBarrierCompiler`: access normalization, hazard classification, read/write coalescing, queue handoff planning, barrier batching, and backend lowering.
- Single methods are only leaf policy queries or final lowering functions. There is no public `Transition(old, next)` method used directly by passes.

### RGResourceAllocator

Reason: resource allocation has two different lifetimes: physical resources reused across frames and logical resources aliased inside one frame.

Children:

- `RGGlobalResourcePool`: cross-frame physical resource reuse.
- `RGTransientResourceAllocator`: single-frame logical resource aliasing.

Global pool state:

- Pool key: resource kind, dimension, extent, format, usage flags, sample count, mip count, array count, memory class, sharing mode, API-specific allocation flags.
- Resource entry: RHI handle, last known whole-resource state, owner queue, retire completion boundary, descriptor invalidation state.
- Eligibility: color attachment textures, UAV textures, and UAV buffers only in the initial design.

Transient allocator state:

- Logical resource lifetime intervals expanded by queue handoff and export requirements.
- Physical slot assignments for non-overlapping intervals.
- Alias barrier records between logical resources sharing one physical resource.
- Backing resource checkout records from the global pool or direct RHI creation.

Boundary:

- The global pool stores physical resources across frames and is shared by all graphs through `RGFrameContext`.
- The transient allocator stores one graph's frame-local logical aliasing decisions.
- Imported resources never enter either pool.
- Bindless-visible resources enter pools only when descriptor lifetime is graph-owned and invalidated before return.
- Native external resources enter pools only when the pool key includes the required platform-command-list interop capability and the containing RHIExecutor completion boundary is tracked.

Leaf operations:

- Checkout global resource only after its retire boundary is complete.
- Prefer lazy canonicalization: store last known state on pool return and compile the first-use transition on checkout.
- For full discard first writes, allow old contents to be discarded and avoid preserving previous pool contents.
- Return eligible resources to the global pool after RHIExecutor accepts command lists and later signals completion.
- Reset transient allocator state every frame after record tasks complete.
- Do not alias transient resources across independent RenderGraph instances. Compile them as one graph when cross-graph aliasing is required.

### RGRecorder

Reason: command recording should scale across worker threads while preserving single-writer command list ownership.

State:

- One record task per compiled command-list segment.
- Resolved resource table per segment.
- CommandList array returned to dispatcher after all tasks complete.

Boundary:

- A record task owns one RHICommandList.
- Execution lambda receives `RHICommandList&` and read-only `RGContext`.
- Platform external pass callbacks are not executed by record tasks; record tasks emit typed RHI callback commands.
- Worker threads never call `RHIExecutor::Submit`, `Present`, `Sync`, or graph mutation APIs.

Leaf operations:

- Emit segment prefix barriers.
- Emit scope begin markers scheduled for the segment start.
- Invoke pass execution lambdas in compiled order.
- Emit platform external callback RHI commands at compiled insertion points.
- Emit scope end markers scheduled for the segment end.
- Emit segment suffix barriers and exports.

### RGDispatcher

Reason: public callers need one entry point and RHIExecutor needs one submit source.

State:

- Dispatch options: synchronous or task-graph assisted setup/compile/record.
- Present request, if any, attached to final graphics submit.
- Dispatch receipt containing exported resource states, owner queues, and completion boundaries.

Boundary:

- `Dispatch()` is the only public orchestration call.
- RHIExecutor owns translate and GPU submit.
- External platform-command-list callback commands are submitted through RHIExecutor and invoked by backend translate/platform record. They do not become an RHI submission backend.

Leaf operations:

- Run setup passes and freeze setup arena.
- Compile graph or await async compile.
- Spawn record tasks for independent segments.
- Join record tasks.
- Submit ordered command-list batch to `RHIExecutor::Submit`.
- Publish dispatch receipt into `RGFrameContext`.
- Optionally call `RHIExecutor::Flush` according to dispatch flags.

## Flow Simulation

### End-to-End Setup/AddPass-to-GPU Flow

Goal: prove the selected API can move from pass declaration to GPU execution without hidden backend tracking, platform command-list exposure in ordinary passes, or a second submit path.

Tree:

- CPU declaration layer owns graph-local data construction and parameter resource-view assignment.
- Setup layer owns preparation tasks that must run before pass execution lambdas.
- Compile layer owns dependencies, barriers, queues, allocator decisions, scopes, and submit shape.
- RG record layer owns abstract `RHICommandList` command generation.
- RHIExecutor layer owns preprocess, backend translate, native command-list allocation, submit, and completion.
- GPU layer executes only submitted backend command lists and reports completion through RHIExecutor boundaries.

Simulation:

1. The renderer opens `RGFrameContext`, samples debug and validation cvars, then constructs a `RenderGraph` with graph-local allocators.
2. Pass code calls `graph.Alloc<T>()`. The object is constructed in the setup arena; the returned pointer is stable until graph teardown and is registered for validation dumps.
3. Pass code fills CPU parameters directly. RG resource views stored in those parameters are the source of resource dependencies.
4. Pass code declares graph-level GPU scopes with `RG_EVENT_SCOPE(graph, name, ...)`. The scope registry stores logical begin/end intent before command-list segmentation exists.
5. `AddSetupPass` stores a preparation lambda. In normal mode all setup lambdas execute before any pass execution lambda; in single-step debug mode the relevant setup work executes before the current pass boundary.
6. Resource access is extracted from graph-owned pass parameters through `DeclareRGAccess(RGParameterAccessCollector&)`. `RGSetupContext` does not declare resource dependencies.
7. `AddPass(param, flags, execution_lambda)` stores one execution lambda, one pass domain, and the parameter-declared RG resource views. It does not store a setup lambda, queue transfer callback, platform command-list callback, or submit callback.
8. `AddExternalPass(..., PlatformCommandList, ...)` stores a separate external node with declared native capability requirements. It is not an ordinary `AddPass` and never receives `RHICommandList&`.
9. `Dispatch()` freezes setup data and starts compile. No execution lambda has recorded GPU commands yet.
10. Compile normalizes access ranges, validates imports and first use, builds hazard edges, maps scopes to pass intervals, plans queue segments, selects physical resources, and inserts alias barriers.
11. `RGBarrierCompiler` queries `RGBarrierPolicy` for backend read/read compatibility, write/write ordering, layout transition, queue ownership, and alias requirements. The result is still an RG submit plan with RHI barrier commands, not native API commands.
12. Compile builds immutable segment records: prefix barriers, pass ids, external callback command slots, suffix barriers, waits/signals, resolved resource tables, scope emissions, and export receipts.
13. `RGRecorder` creates one record task per segment. Each task owns one `RHICommandList`, reads only the compiled segment data, and writes abstract RHI commands linearly.
14. For normal passes, the record task invokes the stored execution lambda as `[](RHICommandList& cmd_list, RGContext context)`. `RGContext` resolves only the resources declared by pass parameter views. The lambda cannot mutate graph structure or access native platform command lists.
15. For `PlatformCommandList` external passes, the record task emits one typed `RHIPlatformCommandCallback` command between compiled barriers. It captures callback metadata and resolved native resource requirements but does not execute the callback.
16. The dispatcher joins record tasks and submits the ordered command-list batch to `RHIExecutor`. RenderGraph does not call backend queues directly.
17. RHIExecutor preprocesses the abstract command lists for submit grouping, dependency waits/signals, event stream metadata, and backend translate work. It consumes RG-provided barriers instead of rediscovering subresource hazards.
18. Backend translate allocates native command lists, lowers RHI barriers through backend code, and visits each RHI command.
19. Ordinary RHI draw/dispatch/copy commands become native API commands such as Vulkan or D3D12 command-list records.
20. `RHIPlatformCommandCallback` is visited only after the native command list and backend resource handles are valid. The backend builds `RGPlatformCommandListContext`, invokes the user callback, restores or invalidates backend state as required, and then continues translating following commands.
21. RHIExecutor submits native command lists with compiled queue waits/signals. The GPU sees only ordered backend submissions and never sees graph setup objects.
22. On completion, RHIExecutor publishes completion boundaries. RenderGraph dispatch publishes export receipts into `RGFrameContext` with final whole-resource state, owner queue, and completion token.
23. Global pool return, transient backing retirement, later raw RHI work, and later RenderGraph imports consume those receipts. No hidden backend tracker state is required to continue the frame.

Single-step variant:

- The same layers are used, but `AddPass` compiles a one-pass plan, records one `RHICommandList`, submits through RHIExecutor, waits for GPU completion, and publishes a boundary receipt before returning.
- The variant deliberately disables async compile, parallel record, future-pass transient aliasing, and cross-pass barrier batching so every pass boundary can be inspected.
- `PlatformCommandList` external passes still execute only during backend translate; single-step only changes when the containing command list is submitted and waited.

### Primary Frame Flow

1. The renderer creates one `RGFrameContext` and one or more frame-local `RenderGraph` instances.
2. Pass code allocates parameter objects with `graph.Alloc<T>()` and writes CPU values directly.
3. Pass code declares graph-level GPU scopes with `RG_EVENT_SCOPE(graph, name, ...)`.
4. Pass parameters store `RGTextureView` / `RGBufferView` entries with access mode, range, state, queue intent, and bindless usage.
5. `AddSetupPass` lambdas store CPU preparation work that RG runs before all pass execution lambdas.
6. `AddPass` stores parameter pointer, flags, one execution lambda, and access declarations extracted from parameters.
7. `AddExternalPass`, when used, stores parameter pointer, `PlatformCommandList` kind, declared native capabilities, and one platform-command-list callback.
8. `Dispatch()` freezes setup data, runs setup work before pass execution, and compiles from parameter-declared accesses.
9. Compile builds dependencies, barriers, lifetimes, queue segments, external nodes, scope emissions, and submit plan.
10. Record tasks create RHICommandLists, emit compiled barriers/scopes, and call execution lambdas.
11. Dispatcher joins record tasks and submits command lists to RHIExecutor.
12. Dispatch publishes an export receipt to `RGFrameContext`.
13. RHIExecutor preprocesses, translates, invokes platform-command-list external callbacks during backend platform record, submits, resolves GPU events, and owns frame boundary convergence for RHI work.

### Resource State Flow

1. Imported resources enter with explicit initial state and owner.
2. Created transient resources enter as unknown and can first materialize on write, clear, upload, or discard.
3. First read of unknown resource is a compile error.
4. Each pass access updates graph-local subresource or buffer-range state.
5. Queue ownership changes become explicit submit waits/signals plus import/export barriers.
6. Exported resources publish canonical whole-resource final state for raw RHI frame-boundary tracking.

### Raw RHI / RenderGraph Interop Flow

1. Raw RHI work before a graph submits through RHIExecutor and produces a submit receipt with completion boundary, final whole-resource state, and owner queue for exported resources.
2. The graph imports those resources through `RGFrameContext`; import state becomes the graph-local initial state.
3. Compile inserts waits and acquire/import barriers when the raw RHI producer boundary is not already complete.
4. Graph work runs and exports resources needed by later raw RHI work or later graphs.
5. Later raw RHI command lists consume the graph dispatch receipt and start from the exported whole-resource state.
6. No implicit global flush is inserted between raw RHI and graph work. Ordering is represented by compiled waits/signals and submit receipts.

### Multiple RenderGraph Flow

1. Every graph in the frame shares one `RGFrameContext` and one device-level `RGGlobalResourcePool`.
2. Each graph owns its own setup allocator, compile allocator, scratch allocator, and transient alias allocator.
3. A resource produced by graph A and consumed by graph B must be exported by A and imported by B with a receipt boundary.
4. Independent graphs may dispatch independently if they do not import each other's resources and do not contend for incomplete pooled resources.
5. Cross-graph transient aliasing is not inferred. If two workloads need one shared alias plan, they must be compiled as one graph.
6. The global pool may serve multiple graphs only through completion-checked checkout. A resource returned by graph A is not available to graph B until its retire boundary is satisfied.

### External Custom Pass Flow

1. The external pass declares `PlatformCommandList` kind, the same resource reads/writes as a normal pass, and native command-list capability requirements.
2. Compile validates backend support for the platform command-list handle and native resource handles.
3. Compile places the external pass in a command-list segment whose queue type matches the native callback requirement.
4. RG record emits graph barriers before the platform callback RHI command.
5. RG record emits one platform callback RHI command carrying callback metadata and declared resource usage.
6. Backend translate/platform record invokes the callback with `RGPlatformCommandListContext`, the platform command-list handle, and resolved native resource handles.
7. Backend translate continues with following RG-emitted barriers and commands when later graph consumers need a different state.
8. The external pass completes through the containing RHIExecutor submit boundary.
9. Direct GPU runtime external passes with independent submit/sync are rejected in the initial design.

### Single-Step Debug Flow

1. `RGDebugController` snapshots `RenderGraph.Debug.SingleStep` before graph construction starts.
2. If single-step mode is disabled, graph execution follows the normal setup, compile, record, and submit path.
3. If single-step mode is enabled, queued `AddSetupPass` preparation work runs before the current pass execution boundary.
4. Each `AddPass` compiles parameter-declared accesses for the current pass against the current boundary state, records one command list, submits through RHIExecutor, waits for GPU completion, and publishes a new boundary receipt.
5. Each `PlatformCommandList` external pass emits an immediate platform callback RHI command; backend translate invokes the callback and the same per-pass GPU wait synchronizes it.
6. Single-step mode disables async compile, parallel record, transient aliasing across future passes, and cross-pass barrier batching so the failing pass boundary is visible.
7. Console variable changes inside an active graph are ignored until the next graph or rejected by debug validation.

### Validation Dump Flow

1. `RGValidationLayer` snapshots dump console variables before graph construction starts.
2. After setup freeze, it can dump parameter metadata, RG resources, imports, exports, and parameter-declared accesses.
3. After compile, it can dump resolved RHI resources, lifetimes, alias slots, pool entries, barriers, queue handoffs, fences, and boundary tokens.
4. Text dumps use deterministic tables sorted by graph id, pass id, resource id, and barrier insertion point.
5. Graph dumps emit DOT files for pass/resource dependency graphs and barrier/fence graphs.
6. Dumping never mutates scheduling or resource state.

### API-Specific Barrier Flow

1. Setup records API-neutral access states: resource, range, read/write mode, logical state, pass domain, queue intent, preserve/discard mode.
2. Common compile builds hazards and candidate transitions without knowing Vulkan layouts or D3D12 barrier layouts.
3. `RGBarrierPolicy` classifies read/read compatibility and write/write ordering for the active backend.
4. Common compile removes no-op intents, merges compatible intents, and keeps only required ordering or layout transitions.
5. Backend lowering turns each remaining intent into RHI barrier commands plus queue wait/signal metadata.
6. RHIExecutor translates those commands mechanically; it does not rediscover resource hazards.

Rules:

- Read/read is merged when ranges overlap, no write is between them, and the backend read compatibility class is identical.
- Read/read may still require a layout-only transition when backend layouts differ, for example shader-read versus attachment-read classes.
- Buffer read/read merges by OR-ing access/sync classes unless the backend policy marks the combination illegal.
- Write/write does not automatically require preserving the previous write. If the later write fully discards or overwrites the range and no consumer observes the earlier write, the resource edge can be removed.
- Write/write still requires ordering when previous contents are observable, the writes are partial or overlapping, an UAV/order class requires serialization, or cross-queue final value must be deterministic.
- Write/write state changes can emit a layout/state transition without a memory dependency when old contents are discarded.

### GPU Scope Flow

1. `RG_EVENT_SCOPE` creates a graph scope record before compile.
2. Compile maps scope begin and end to concrete pass boundaries.
3. Queue segmentation may split the scope across command lists.
4. If all begin/end points remain in one segment, recorder emits ordinary command-list begin/end events.
5. If the scope spans segments, recorder emits graph-level events carrying scope id and ordering metadata.
6. GPUEventStream reconstructs logical nesting from ids and compiled ordering instead of relying only on queue-local depth.

### Copy Flow

1. Graph copy passes declare copy domain and source/destination access ranges.
2. Compile decides whether the copy is represented as a CopyScope inside a graphics/compute segment or as executor-derived copy submit work.
3. Graph-level scopes cannot be emitted across active `CopyCommandScope`; scope emission points are moved outside copy scope boundaries.
4. Copy queue handoffs are compiled, not user-authored.

### Queue Transfer Flow

1. Compile assigns each pass to one queue domain.
2. For every cross-queue hazard, compile creates `RGQueueHandoff` with producer segment, consumer segment, resource range, source state, destination state, and ownership mode.
3. Same-queue hazards use command-list order and local barriers only.
4. Cross-queue hazards use one syncpoint per producer-consumer segment pair, not one fence per resource.
5. Vulkan lowering emits release/acquire queue-family transfers only when the policy requires exclusive ownership transfer.
6. D3D12 lowering emits queue synchronization and resource state barriers without Vulkan-style queue-family ownership.
7. Pure read fan-out from a known read state may avoid ownership churn when the backend policy supports shared read ownership; otherwise compile serializes ownership conservatively.

### Resource Reuse Flow

1. Compile computes logical lifetimes after queue handoff expansion.
2. `RGTransientResourceAllocator` assigns non-overlapping logical resources to physical slots.
3. Each physical slot chooses a backing resource: checkout from `RGGlobalResourcePool` if eligible, otherwise create a direct transient RHI resource.
4. Alias barriers are inserted between logical resources that share one physical slot.
5. First use of a pooled resource uses either stored last state or discard transition depending on access mode.
6. Frame completion returns eligible physical resources to the global pool after RHIExecutor completion boundary.
7. Non-pooled transient backing resources are destroyed or retired after the same completion boundary.

## Algorithm-Level Leaves

### Access Key

Use a compact key for sorting and hazard detection:

```text
ResourceAccessKey = resource_id, resource_kind, mip_begin, mip_end, array_begin, array_end, byte_begin, byte_end
```

Rules:

- Texture accesses overlap when mip ranges and array ranges overlap.
- Buffer accesses overlap when byte ranges intersect.
- Whole-resource raw RHI state is updated only at import/export/frame boundary.

### Hazard Edge Builder

Algorithm:

1. Group accesses by resource id.
2. Sort each group by pass id.
3. Keep the last writer list and active reader list per overlapping range.
4. On read: add edges from overlapping last writers to current pass, then append current read.
5. On write: add edges from overlapping last writers and active readers to current pass, replace overlapping state with current writer, clear consumed readers.
6. Add export edge from last access to export tail.

This is selected over command scanning because parameter-owned access declarations are compact, cache-friendly, and available before record.

### Barrier Batch Compiler

Algorithm:

1. Walk passes in compiled order.
2. For each incoming edge, compare source state, destination state, source queue, destination queue, range, and preserve/discard mode.
3. Query `RGBarrierPolicy` for backend read compatibility, write ordering, layout transition, queue ownership, and alias requirements.
4. Drop read/read barriers when read compatibility class matches and queue ownership is unchanged.
5. Keep read/read layout-only transitions when compatibility class differs.
6. Drop write/write ordering when the previous write is dead and the next write fully discards the overlapping range.
7. Keep write/write ordering for partial overlap, observable previous contents, UAV/order requirements, or deterministic cross-queue final value.
8. Merge adjacent barriers with identical insertion point, queue transition, state transition, and resource handle.
9. Split cross-queue barriers into release signal and acquire wait records.
10. Attach queue-local barriers to segment prefix or pass-local insertion point.

API policy examples:

- Vulkan texture read/read can be no-op only when the layout class is compatible; layout changes still need an image barrier even if both states are reads.
- D3D12 buffer read/read usually merges by OR-ing sync/access bits; texture read/read can still require enhanced-barrier layout compatibility.
- UAV write/write needs an ordering barrier when order is observable. Full discard writes can remove the dependency only when no later read/export observes the previous write.

### Lifetime and Alias Compiler

Algorithm:

1. For every transient resource, compute first pass id and last pass id from parameter-owned access declarations.
2. Sort resources by first pass id, then size descending.
3. Reuse physical allocations when intervals do not overlap, descriptors are compatible, and required alias barrier can be emitted.
4. Never alias imported resources.
5. Never globally pool sampled-only textures, depth textures, upload buffers, readback buffers, or external resources in the initial design.

### Resource Pool Compiler

Algorithm:

1. Build physical slot descriptors from transient alias assignments.
2. Mark a slot globally pool-eligible only when all logical resources in the slot are allowed pool categories.
3. Build a strict pool key from descriptor and backend allocation compatibility data.
4. Checkout an available resource whose retire boundary is complete.
5. If no resource is available, create a new RHI resource and register it with the pool owner.
6. Record the checkout initial state from pool metadata.
7. Compile the first-use barrier from checkout state to first logical access state, or discard previous contents for full overwrite first use.
8. On frame completion, return the physical resource with final known whole-resource state and owner queue.

### Interop Boundary Compiler

Algorithm:

1. Convert every raw RHI receipt, graph export receipt, and external pass completion into `RGBoundaryToken`.
2. For each graph import, find the latest token for the imported resource in `RGFrameContext`.
3. Validate the imported state, owner queue, and completion boundary are explicit.
4. Insert wait/acquire work when the producer token may still be in flight.
5. For every graph export, publish final whole-resource state, owner queue, and completion boundary into the dispatch receipt.
6. Reject hidden boundary transitions when the caller attempts raw RHI -> graph -> raw RHI reuse without import/export metadata.

### External Pass Compiler

Algorithm:

1. Treat platform-command-list external passes as graph nodes with declared accesses and a required command-list queue type.
2. Validate native capability: backend, platform command-list handle type, native resource handle type, memory sharing mode, and queue type.
3. Place the external pass into a compatible compiled command-list segment.
4. Emit barrier intents before and after the platform callback RHI command inside the containing command list.
5. Build resolved native resource handle table for resources declared by the external pass.
6. Emit an `RHIPlatformCommandCallback` command during RG record; do not execute the callback during RG record.
7. Expose only `RGPlatformCommandListContext` to the callback during backend translate/platform record.
8. Extend resource lifetime through the containing RHIExecutor completion boundary.
9. Reject direct-GPU external passes with independent submit/sync in the initial implementation.
10. Reject any external pass that touches a graph transient or pooled resource without proven native handle stability and descriptor lifetime.

### Single-Step Debug Executor

Algorithm:

1. Snapshot `RenderGraph.Debug.SingleStep` before graph construction starts.
2. Store the snapshot in `RGDebugController`; do not read the cvar again until the next graph.
3. On `AddSetupPass`, store preparation work in graph-owned setup order.
4. On `AddPass`, extract resource access from parameters and validate that required parameter views exist for the pass.
5. Compile a one-pass plan from the current `RGFrameContext` boundary and existing graph-local state.
6. Record one RHICommandList, including barriers, scopes, validation markers, and platform callback RHI command if present.
7. Submit through RHIExecutor and wait for GPU completion before returning from `AddPass`.
8. Publish the pass output as a boundary receipt for the next declared pass.
9. Reject attempts to toggle single-step mode while a graph scope is active.

### Validation Dump Compiler

Algorithm:

1. Snapshot validation cvars before graph construction starts.
2. Assign stable ids to passes, parameters, RG resources, RHI resources, barriers, fences, queue handoffs, and boundary tokens.
3. Dump setup tables after setup freeze: pass parameters, RG resource descriptors, imports, exports, parameter-declared accesses, and external platform-command-list requirements.
4. Dump compile tables after compile: resolved RHI resources, lifetime intervals, alias slots, pool entries, barrier batches, queue handoffs, fences, and submit plan.
5. Emit DOT graph output for pass/resource dependencies and barrier/fence synchronization edges.
6. Sort every dump deterministically by graph id, pass id, resource id, and insertion point.
7. Never feed dump data back into scheduling, allocation, or barrier decisions.

### Queue Segment Compiler

Algorithm:

1. Assign every pass to one queue from pass flags and resource constraints.
2. Topologically order passes by hazard edges.
3. Build maximal contiguous segments with same queue and no unsatisfied external dependency.
4. Split segments when a cross-queue edge requires a signal/wait boundary.
5. Collapse cross-queue edges between the same producer segment and consumer segment into one logical syncpoint.
6. Insert platform-command-list external nodes and raw RHI boundary tokens into the same topological order.
7. Emit submit plan in topological order with explicit wait/signal metadata.

### Record Scheduler

Algorithm:

1. Build record tasks from compiled segments.
2. A task depends on compile completion and on record tasks whose command-list data must be produced first for CPU-side ordering.
3. Independent segments record in parallel.
4. Each task owns one RHICommandList and writes it linearly.
5. Dispatcher joins all tasks before submitting to RHIExecutor.

### Scope Compiler

Algorithm:

1. Resolve each graph scope to `{scope_id, parent_id, begin_pass, end_pass}`.
2. Validate parent interval contains child interval.
3. Find the compiled segment containing begin and end.
4. If begin and end are in one segment, emit ordinary begin/end events at segment-local positions.
5. If they are in different segments, emit graph scope begin/end events with explicit ids and require a submit-order path from begin segment to end segment.
6. Reject scopes whose begin/end segments are unordered.

## Performance Decision

Selected design choices:

- Use graph-owned bump allocation instead of per-pass heap allocation. This minimizes allocator overhead and makes async lifetime simple.
- Keep CPU graph allocators per RenderGraph instance and global resource pooling per frame/device context. This prevents cross-graph lifetime mutation while still allowing safe physical reuse after completion.
- Use explicit parameter-owned access declarations instead of RHICommandList scanning. This removes expensive backend inference and makes compile parallelizable.
- Use sorted access arrays instead of pointer-heavy dependency node traversal for hazard compilation. This improves cache locality and deterministic output.
- Use immutable compiled plans for recording. Worker threads avoid locks around graph mutation.
- Use RHIExecutor for submission. RenderGraph gains orchestration without duplicating translate scheduling or GPU submit runtime.
- Limit initial external pass support to platform-command-list callbacks. This keeps completion under RHIExecutor and avoids a second synchronization model.
- Implement single-step as a debug-only immediate path with per-pass GPU sync. It sacrifices batching, async record, and aliasing optimization to make one pass boundary inspectable.
- Generate validation dumps from frozen setup and compiled plans. Dumping stays deterministic and side-effect free.
- Keep first implementation latest-only. Removing legacy API branches reduces compile-time and runtime branching.

Tradeoff:

- The API requires accurate setup declarations. Incorrect missing declarations become compile errors or debug validation failures instead of backend guesswork. This is deliberate because correctness belongs to RenderGraph, not hidden RHI scanning.

## Rejected Alternatives

- Keep old Builder/AddGraphicPass API: rejected because it preserves obsolete RHIGraphicsCommandList-era ownership and forces compatibility wrappers.
- Add two lambdas to `AddPass`: rejected because setup and execution lifetimes differ and a setup callback inside AddPass makes async phase ownership less explicit.
- Infer bindless dependencies from descriptor heap dirtiness: rejected because it recreates backend complexity and misses shader-side handle usage intent.
- Let execution lambdas choose queue or emit queue transfers: rejected because compile must own cross-queue correctness.
- Hide NVAPI, tensor runtime, or other native GPU work inside ordinary `AddPass`: rejected because RenderGraph would lose resource state, queue ownership, and completion visibility.
- Give ordinary `AddPass` lambdas direct platform command-list access: rejected because backend command lists are created during RHI translate, not during RG record, and it would collapse RG into backend-specific code.
- Execute platform external callbacks during RG record: rejected because the native command list and final backend descriptor state are not available until RHI translate/platform record.
- Support direct-GPU external passes with independent submit/sync in the initial implementation: rejected because it requires a second completion-token model; the initial design supports only platform-command-list interaction.
- Allow debug cvars to switch single-step mode inside an active graph: rejected because half the graph would be compiled under one execution model and half under another.
- Make validation graph dumps influence scheduling: rejected because diagnostics must be observational.
- Transparently share transient aliases across independent RenderGraph instances: rejected because it requires whole-frame lifetime compilation; callers should compile one graph when they need cross-workload alias optimization.
- Let raw RHI and RenderGraph exchange resources through backend tracker side effects: rejected because resource state boundaries must be explicit import/export receipts.
- Model graph GPU scopes as a CommandList-local stack: rejected because one logical scope can span multiple compiled command lists.
- Add a RenderGraph submission backend: rejected because RHIExecutor already owns translate scheduling, submit runtime, interrupt handling, and profiling completion.
- Preserve raw RHI subresource persistent tracking: rejected because it keeps the main source of resource tracking complexity in the backend.

## Implementation Order

1. Replace public RenderGraph headers with the new frame-local API: `Alloc<T>()`, `AddSetupPass`, `AddPass`, `Dispatch`, `RGSetupContext`, `RGContext`, and `RG_EVENT_SCOPE`.
2. Implement `RGFrameAllocator` and freeze checks.
3. Implement logical resource and access declaration storage.
4. Implement pass registry and execution-lambda storage.
5. Implement compile validation for imports, exports, first use, bindless declarations, pass flags, external pass capabilities, and queue intent.
6. Implement hazard edge builder and deterministic topological ordering.
7. Implement backend barrier policy classification and API-neutral barrier intents.
8. Implement barrier batch compiler and queue handoff plan.
9. Implement transient resource lifetime and alias allocation.
10. Implement global resource pool checkout, lazy state metadata, delayed return, and discard first-use handling.
11. Implement `RGFrameContext`, import/export receipts, and raw RHI boundary tokens.
12. Implement typed RHI platform callback command and backend visitor execution hook.
13. Implement platform-command-list external pass nodes with native capability validation.
14. Implement multi-graph sequencing through `RGFrameContext` without cross-graph transient aliasing.
15. Implement scope registry and scope compiler.
16. Implement record scheduler and single-writer RHICommandList recording.
17. Implement dispatcher submission through RHIExecutor.
18. Implement single-step debug mode and cvar snapshot validation.
19. Implement validation dumps for graph parameters, RG resources, RHI resources, barriers, fences, and DOT graph output.
20. Remove old RenderGraph skeleton paths in the same implementation branch.
21. Validate with existing `testscripts/windows/test_rhi_translate.ps1`, then `testscripts/windows/test_all.ps1` after milestone-sized landing.
