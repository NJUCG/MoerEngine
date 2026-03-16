# Moer TraceProfiler Update & Plan

## Current status
- Architecture: `MoerEditor -> TCP (one-way) -> MoerProfiler`.
- Build: `MoerEditor` and `MoerProfiler` release build pass.
- Release policy: trace is disabled by default in `Release` via compile-time macro.
- This document is synced to current implementation state (Play Mode, timeline fixes, and latest trace behavior).

## Implemented features

### Runtime trace pipeline
- Added trace API and macros:
  - `source/runtime/core/include/trace/Trace.h`
  - `source/runtime/core/source/trace/Trace.cpp`
- Supported:
  - `Init/Shutdown`
  - `StartRecording/StopRecording/IsRecording`
  - `BeginSpan/EndSpan`, `EmitScope/EmitInstant/EmitCounter`
  - metadata/events packet serialization
  - bounded async queue, dropped counter, CSV write

### GPU scope semantics (updated)
- `GPU Scope` is defined as a 3-stage pipeline:
  1. `CPU scope first`: opening a GPU scope also creates a CPU trace span for command recording cost.
  2. `CommandList query markers`: GPU scope writes `begin/end timestamp query` commands into `CommandList`.
  3. `Resolve -> GPU event`: resolved query timestamps are converted to timeline span events and appended to the per-queue GPU track (`GPUx/Queue`).
- Added helper macro:
  - `TRACE_GPU_SCOPE_SPAN(cmd_list, "Name")`
  - expands to RAII push/pop of `PushScopeWithTimeScope/PopScopeWithTimeScope`
  - ensures CPU record span and GPU query scope stay paired.

### Editor integration
- Editor starts with trace system initialized but **recording OFF**.
- Trace panel in Editor UI includes:
  - `Start Trace`
  - `Stop Trace`
  - read-only status (`Enabled/Recording/Connected/Queued/Dropped`)
- CSV output path now uses unique file naming per editor start:
  - `<AppPath>/trace/editor_trace_YYYYMMDD_HHMMSS_mmm.csv`
  - if name collision occurs, suffix `_1/_2/...` is appended.
- Within the same editor process, repeated `Start Trace`/`Stop Trace` continues writing to that session file.

### Profiler timeline UI
- Top ruler and timeline tracks.
- Search by event name with next/prev jump.
- Event selection by click + `F` key focus-to-selection.
- Hover tooltip shows only:
  - current event info
  - parent event name
- Timeline rendering now only draws `Scope` events in span lanes; `Counter` events are not rendered as bars.

## Latest implementation sync (important)

1. Track identity fix (CPU/GPU overlap bug)
- Track key is now `track_type + track_id` instead of only `track_id`.
- Prevents CPU/GPU track collisions and wrong mixed rendering.

2. CPU depth robustness
- Runtime side: CPU scopes support automatic nested depth assignment (thread-local per-track depth counter).
- Profiler side: display depth is additionally derived from interval overlap as a fallback to tolerate bad/missing producer depth.

3. Timeline correctness and interaction updates
- Unified timeline canvas with left label column.
- Group headers for `CPU Tracks` and `GPU Tracks`.
- Horizontal separators between rows.
- Zoom/pan/search/selection/F-focus remain available.
- Visible track multi-select keeps hidden tracks recoverable.

4. Counter overlap fix
- `EmitCounter` data remains ingested/stored.
- Counter events are excluded from span-lane drawing to avoid overlap with frame scopes such as `Raytracing.Frame`.
- Counter chart panel is planned separately (future work).

5. Profiler runtime stability fixes
- Shutdown order adjusted in `MoerProfiler` to release renderer resources before device dispose.
- In-flight frame limit now follows swapchain backbuffer count (no hard-coded value).

6. Editor Play Mode (new)
- Added Play Mode state in editor config.
- `F5`: enter Play Mode and capture input (mouse hidden, gameplay camera input active, editor UI not interactive).
- `F8`: toggle inside Play Mode between:
  - `Possess` (capture gameplay input)
  - `Eject` (editor-style input/UI interaction while staying in Play Mode)
- Menu bar includes `Play (F5)`, `Stop Play`, `Eject/Possess (F8)`.
- Console remains callable in Play Mode; cursor policy follows possess/eject state.

7. GPU Track disappearance fix (2026-03-16)
- Root cause:
  - after query-runtime refactor, resolved GPU timestamp samples were collected but not emitted into trace timeline events.
  - result: profiler UI had no `GPUQueue` scope events to build GPU tracks from.
- Fix:
  - in `VkCommandQueue`, convert `resolved_gpu_samples` to `Trace::EmitScope` events on `TrackType::GPUQueue`.
  - track identity uses `MakeGpuQueueTrackId(0, queue_type)` and track name `GPU0/Queue(...)`.
  - add stable tick->ns anchor mapping per queue to keep GPU spans in timeline time domain.

8. GPU overlap subtrack policy (2026-03-16)
- Timeline overlap-depth derivation is now applied specifically to GPU tracks.
- When GPU events overlap in time on the same queue track, profiler allocates additional display depth lanes (subtracks) without creating new logical tracks.
- CPU tracks continue to honor producer depth semantics.

9. Timeline performance Phase A implemented (2026-03-16)
- Added store generation-based snapshot cache in profiler timeline path:
  - event/track copy and expensive preprocessing are rebuilt only when data changes.
- Added per-track scope index (sorted by `ts_begin_ns`) and visible-range query with binary-search entry.
- Replaced per-track full event scan with indexed subset iteration in render loop.
- Added cached search mask + matched list rebuild only on data/search changes.
- Added event-id lookup cache for O(1) selected-event focus (`F`).
- Reduced status panel profile-size computation to O(1) approximate path (no per-frame full string walk).

## This round UI fixes (latest)

1. Track separators
- Added horizontal separator lines between all track rows (including final bottom boundary) to improve visual readability.

2. Merged track/timeline windows
- Replaced previous dual-child layout (`TrackList` + `TimelineCanvas`) with one unified timeline canvas.
- Track label column is now rendered on the far left in the same scroll space, so labels and event rows are always aligned.

3. Current profile size display
- Added approximate profile memory size display in profiler status area:
  - includes event storage + dynamic string payload + track/session strings.

4. CSV workflow: Browse and load in one step
- Removed two-step `Browse` + `Load` operation.
- `Browse CSV (Load)` now opens file dialog and immediately loads selected CSV.
- Existing in-memory profile is overwritten by newly selected CSV content.

5. Event text fit and clipping
- Event label rendering now uses adaptive text size based on event lane height and available rect width.
- Text is vertically centered and clipped inside event bounds to prevent overflow outside event rectangles.

6. Horizontal navigation
- Added horizontal pan support:
  - middle-mouse drag
  - `Shift + mouse wheel`
  - `Left/Right` arrow keys

7. Color scheme by event name
- Event color is deterministic from **event name hash**.
- Replaced raw random-like RGB with HSV-based pleasant palette:
  - controlled saturation/value range for better readability.

8. Event text clipping and alignment
- Event label is now:
  - vertically centered
  - left aligned inside event rect
  - clipped to rect bounds (no text overflow outside the button/bar)

9. Track visibility workflow
- Removed inline hide checkbox from left list.
- Added `Visible Tracks` multi-select popup menu.
- Left list displays only visible tracks.
- Hidden tracks can always be restored from popup.

10. Track row alignment (left list vs right timeline)
- Unified row height model (`row_h`) across both panes.
- Synced vertical scroll value across both panes.
- Removed extra left-only title row that caused offset drift.
- Left and right now share same top spacer (`top_header_h`) and row accumulation logic.

## Remaining work
- Add category/track/duration advanced filter panel.
- Add manual "Clear Session" action in profiler.
- Add CSV tail-follow mode (currently manual load).
- Add dedicated counter chart panel (do not render counters as scope bars).
- Optional: replace bounded mutex queue with lock-free ring buffer.
- Play Mode follow-up:
  - optional shortcut for stop-play (e.g. `Shift+F5`)
  - explicit on-screen status badge (`PLAYING / EJECTED / POSSESSED`)

## Verification checklist
- `cmake --build build --target MoerProfiler --config Release`
- `cmake --build build --target MoerEditor --config Release`
- Runtime checks:
  - timeline pans/zooms correctly
  - CPU/GPU tracks never merge on same row
  - CPU nested scopes show layered depth (not collapsed to one lane)
- counter events do not overlap scope spans in timeline
- GPU track exists when GPU timestamp query data is produced
- overlapped GPU scopes appear on additional depth lanes (same track, no new logical track)
- visible track popup can hide/show tracks repeatedly
  - event text never overflows
  - hover shows current + parent only
  - `F` focuses selected event correctly
  - Play Mode:
    - `F5` enter capture
    - `F8` possess/eject toggle works without ending play
    - possess: cursor hidden, editor UI not interactive
    - eject: cursor visible, editor UI interactive
