# RDG Secondary Command Buffer Upgrade Plan

## Current compatibility stage

Raster RDG records one frontend `CommandList` per record pass. Eligible pass
callbacks may run concurrently. After all producers finish, the lists are
appended in compiled graph order to the caller-owned frame `CommandList`; only
that destination is submitted to RHI. This deliberately gives us:

- parallel CPU-side pass recording;
- one continuous backend resource-state tracker lifetime;
- one native graphics submission for the frame stream; and
- deterministic callback, query, signal, and cached-argument ordering.

The current merge still translates the concatenated frontend IR after the
join. It is an intermediate architecture, not the final parallel Vulkan
encoding path.

## Raster graph resource registration follow-up

The remaining `shadow_maps` and `probe_volume` tokens are GPU-topology
proxies, not frontend recording dependencies. GPU graph edges never serialize
immutable `Record` callbacks.

- Probe resources are already created by `ProbeVolumeResource::Create()`.
  Import its concrete buffers and atlas textures, declare their per-pass
  accesses, then remove the aggregate `probe_volume` token.
- Shadow textures may be created or recreated by
  `ShadowDepthPass::PrepareCSMResources()` and
  `PreparePointShadowResources()`. Move this resource-maintenance step before
  graph construction, then import the concrete cascade/cube textures and
  remove the `shadow_maps` token.

Until those changes land, keep both tokens so Compile can retain the required
GPU producer/consumer topology. Do not use them as CPU setup or recording
synchronization.

## Target architecture

Keep the public graph model unchanged and replace the backend implementation:

```text
Prepare tasks -> Compile graph/barriers
              -> parallel pass frontend recording
              -> parallel Vulkan secondary encoding
              -> ordered primary assembly
              -> one/few native queue submissions
```

Each compiled pass should produce a backend packet containing its secondary
command buffer, immutable inheritance/rendering signature, completion gate,
resource keepalives, query ownership, and debug/profiling metadata. Producer
completion order must never determine GPU execution order; the primary command
buffer executes packets in compiled graph order.

## Vulkan ownership split

The primary command buffer owns graph-level behavior:

- resource transitions and queue-family ownership barriers;
- cross-pass ordering and `vkCmdExecuteCommands` calls;
- dynamic-rendering begin/end envelopes when a graphics secondary inherits a
  rendering instance;
- frame-level timestamps/debug labels; and
- the final restore/export epilogue, if the selected state-ownership mode
  requires one.

A secondary command buffer owns only pass-local commands:

- pipeline and descriptor binding;
- draw/dispatch/copy bodies supported by its queue and inheritance mode;
- pass-local labels and timestamp queries; and
- no implicit resource restoration or native submission.

For dynamic rendering, derive `VkCommandBufferInheritanceRenderingInfo` from
the pass attachment formats, sample count, view mask, and depth/stencil state.
The primary begins rendering with
`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`, executes the compatible
secondary, and ends rendering. Compute/copy packets do not need rendering
inheritance.

## Required backend work

1. Add a per-frame, per-record-worker secondary command-pool arena. Pools are
   reset only after the frame completion fence retires; no pool or descriptor
   allocator may be mutated concurrently without explicit ownership.
2. Split Vulkan translation into a pass envelope and pass body. The envelope
   emits primary barriers/rendering boundaries; the body can be encoded into a
   secondary command buffer without consulting mutable global tracker state.
3. Lower RDG resource states before native recording. A secondary receives an
   immutable resolved resource table and must not call the legacy
   `RestoreState()` path.
4. Make descriptor allocation recording-context-local or otherwise
   thread-safe. Descriptor lifetimes must be retained by the frame submission,
   not by worker stacks.
5. Allocate query ranges before worker dispatch. Query indices and profiling
   source order are compile-order identities, independent of worker completion
   order.
6. Assemble secondaries into one primary per native queue batch. Add timeline
   waits/signals only for genuine cross-queue graph edges, not per pass.
7. Keep `SerialRecord`/`SerialControl` as explicit escape hatches. Unsupported
   custom commands or inheritance shapes fall back to primary translation for
   that pass without changing graph order.

## Migration stages

1. **Frontend merge (current):** parallel pass callbacks, concatenated IR, one
   native submit.
2. **Secondary proof:** encode compute-only passes as secondaries and compare
   command/order hashes against serial translation.
3. **Graphics inheritance:** support dynamic-rendering attachment signatures
   and primary rendering envelopes.
4. **Active RDG state:** consume compiled barriers and remove per-pass backend
   restoration for graph-owned resources.
5. **Multi-queue assembly:** one primary per native queue batch with graph
   timeline synchronization.
6. **Remove compatibility merge:** retain it only as a validation/fallback
   backend once secondary recording is stable.

## Acceptance criteria

- CPU tests prove producer overlap and compiled-order primary assembly.
- A raster frame produces one graphics native submit in the single-queue case.
- Vulkan validation reports no command-pool, inheritance, query, descriptor,
  or resource-state violations during resize, hot reload, and shutdown.
- Serial and secondary paths produce equivalent command/order diagnostics and
  rendered output.
- Device-lost stress runs remain stable for at least the existing runtime test
  window before the secondary path becomes the default.
