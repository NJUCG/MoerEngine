# Moer ProfilerTrace Performance Optimization Plan

## Goal
- Keep timeline interaction smooth when event count is very large (100k to millions).
- Preserve full data fidelity in storage while reducing per-frame UI work.

## Current bottlenecks (inferred)
- Per-frame full scan: for each visible track, iterating over all events.
- Repeated string filtering and hover hit-test on all events.
- Rendering tiny/overlapping spans that are visually indistinguishable.
- Large CPU->ImGui draw command volume each frame.
- Unbounded in-memory growth causing cache misses and allocator pressure.

## Optimization roadmap

### 1. Data indexing (highest priority)
- Build immutable per-track event arrays sorted by `ts_begin`.
- Keep one index per `(track_type, track_id)` to avoid cross-track scanning.
- Add binary-search query by visible time range:
  - `lower_bound(ts_begin >= view_start - max_span_padding)`
  - stop early when `ts_begin > view_end`.
- Search box should query index first, not full global vector.

### 2. Frame cache / incremental model
- Separate ingest store from render cache:
  - ingest thread appends raw events.
  - UI thread snapshots only dirty ranges.
- Recompute layout metadata (`max_depth`, visible row height, color hash) only for changed tracks.
- Use generation counters to avoid rebuilding unchanged track draw lists.

### 3. Level-of-detail (LOD) timeline rendering
- When zoomed out, aggregate events into time bins (per track/depth):
  - render density bars or merged spans instead of every micro-event.
- Hide labels under pixel threshold (`span_px < label_min_px`).
- Skip drawing spans with width below 1px unless selected/highlighted.
- Only enable per-event hover picking when zoomed in enough.

### 4. Rendering path optimization
- Precompute pixel X for visible events once per frame per track.
- Reduce draw calls by batching same-style rectangles.
- Clip aggressively using track rect + timeline rect before text measurement.
- Avoid repeated `CalcTextSize` for same string/font-size in frame:
  - small LRU text-measure cache.

### 5. Search and filter acceleration
- Build optional lowercase name cache at ingest time.
- Maintain inverted index for exact/prefix search (substring fallback only when needed).
- Apply filter pipeline in this order:
  - track visibility
  - time range
  - category
  - name match
- Keep `next/prev` result list as cached indices until data generation changes.

### 6. Memory strategy
- Use chunked storage (arena/chunk vector) for events to avoid frequent realloc/moves.
- Store large strings in string interning table:
  - `name/category/track_name` deduplicated to IDs.
- Add soft memory watermark and UI warning (no forced drop by default).

### 7. Ingest/UI decoupling
- Ensure network ingest never blocks UI thread.
- Move CSV parse/load to worker thread and publish atomic snapshot when done.
- For live stream, append batches with lock minimization:
  - single-producer/single-consumer ring for UI ingest queue.

### 8. Interaction responsiveness
- Throttle expensive recomputation during active drag/zoom:
  - coarse rendering while dragging, refine on release.
- Cap max hover checks per frame (progressive pick) when zoomed out.
- Keep selected event metadata in direct map `event_id -> event*`.

## Suggested implementation phases

### Phase A (quick wins, low risk)
- Per-track sorted index + time-range binary search.
- Early culling and pixel-threshold skip.
- Cached search result list.

### Phase B (mid risk, large gain)
- Render cache generations + incremental rebuild.
- LOD bins for zoomed-out views.
- Text measure cache.

### Phase C (structural)
- String interning + chunked event store.
- Full ingest/render thread decoupling with lock-light queues.

## Verification metrics
- FPS in timeline window at:
  - 100k events
  - 500k events
  - 1M events
- Frame time split:
  - `filter/query`
  - `layout`
  - `draw`
  - `hover/pick`
- Peak memory and allocation count during 30 min live session.
- Input latency (pan/zoom/select) under heavy load.

## Acceptance targets
- 500k events: smooth interaction on mainstream desktop GPU/CPU.
- 1M events: no UI freeze, interaction remains usable.
- Live streaming: no semantic impact on Editor, UI hitches bounded.

## 2026-03-16 Delta Plan (for current heavy-event regression)

### Symptom-focused diagnosis
- `DrawTimelinePanel` currently does high-frequency full-work every frame:
  - copy all events/tracks from store
  - iterate events repeatedly per visible track
  - recompute overlap-depth + string match + hover checks
- Complexity drifts toward `O(tracks * events)` in hot paths, causing severe stutter when event count grows.

### Concrete optimization design (no behavior change)
1. Query path indexing
- Build `TrackViewCache`:
  - key: `track_key`
  - value: sorted event indices by `ts_begin_ns`
- Use binary-search (`lower_bound`) to fetch only `[view_start_ns, view_end_ns]` candidates.
- Expected effect: per-track query from full scan to logarithmic entry + short linear visible range.

2. Visible-range rendering only
- Keep a per-frame `VisibleEventList` generated from query cache.
- Render and hover-test only these visible items, not all events.
- Add coarse cull before text/layout (`if x1 <= x0 || out of clip -> continue`).

3. Stable incremental caches
- Introduce generation counters:
  - `store_generation` (append/clear/load changes)
  - `view_generation` (zoom/pan/filter changes)
- Rebuild expensive caches only when corresponding generation changes.
- Reuse:
  - overlap-depth assignment for unchanged time range
  - search match lists until text/filter changes.

4. Zoom-aware LOD
- At low zoom (many events per pixel), aggregate into time bins per track/depth:
  - draw density bars/merged bands instead of every event rectangle.
- Switch to per-event rendering only above a zoom threshold.

5. Text and tooltip budget control
- Draw event label only if `event_width_px >= min_text_width`.
- Add per-frame text measurement cache (`name + font_size -> width`).
- Limit hover hit-test candidates to currently visible row and small x-range neighborhood.

6. Search acceleration
- Store lowercase cached name per event (or interned string id + lowercase table).
- Precompute filtered result vector for current query and reuse for next/prev navigation.

7. Memory and ingest decoupling
- Move from monolithic vector growth to chunked storage for events.
- Keep ingest append and UI snapshot decoupled (SPSC queue / double buffer).
- Avoid large lock hold during UI frame snapshot.

### Rollout order
1. Indexing + visible-range rendering + generation cache.
2. Hover/text budget controls.
3. LOD aggregation.
4. Memory/chunk store + ingest decouple.

### Implementation status
- Done:
  - generation-based data snapshot cache
  - per-track sorted index + visible-range binary-search rendering
  - cached search-mask rebuild on demand
  - selected-event id -> index cache
  - O(1) approximate profile-size stat path
- Next:
  - hover/text budget optimization
  - zoom-level LOD aggregation
  - chunked storage + ingest/UI decoupling

### Validation targets for this delta
- 200k events: timeline interactions stay fluid (pan/zoom/search) in Debug build.
- 500k events: no long frame spikes from full-event scans.
- 1M events: degraded visual detail allowed (LOD), but interaction remains responsive.
