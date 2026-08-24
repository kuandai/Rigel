# Debug Tooling

This document describes the runtime debug overlays and diagnostic hooks in
Rigel. These tools are intended for development builds and are not configurable
through gameplay UI.

---

## 1. Overview

`FrameRenderer` owns the debug overlay state and GPU resources. It draws the GL
overlays after the main scene; Application renders the ImGui profiler
window from the same state. When enabled the tooling draws:

- Chunk streaming field (colored cubes for pipeline state).
- Chunk streaming legend (when ImGui is available).
- Frame time graph (ms per frame).
- ImGui profiler window (flame graph of per-frame scopes).
- Entity bounds wireframes.

The GL overlays are toggled by the `debug_overlay` action (F1 by default). The
ImGui profiler window is toggled independently by the `imgui_overlay` action
(F3 by default). Both start disabled; releasing the corresponding action opts
in to that instrumentation.

---

## 2. Toggle and Lifetime

- `FrameRenderer` owns the `Render::DebugState` containing `DebugField`,
  `FrameTimeGraph`, and `EntityDebug`.
- The application-owned `InputState` notifies `Input::DebugOverlayListener` and
  `Input::ImGuiOverlayListener` on action releases. The listeners toggle
  `DebugState::overlayEnabled` and `DebugState::imguiEnabled`, respectively.
- `FrameRenderer::initialize` calls:
  - `Render::initDebugField`
  - `Render::initFrameGraph`
  - `Render::initEntityDebug`
- `FrameRenderer::release` calls `Render::releaseDebugResources` on shutdown.

If a shader asset is missing, the corresponding overlay logs a warning and is
skipped.

---

## 3. Chunk Streaming Field

### 3.1 Data Source

- `WorldView::getChunkDebugStates` collects a value snapshot from the streamer,
  CPU mesh store, retained visibility-trace records, and renderer draw cache.
- Collection walks only the center/radius cube requested by the enabled
  visualizer. Programmatic radii are clamped to the supported maximum view
  radius before iteration. Coordinates without cheap production ownership are
  discarded before the mesh-store snapshot lookup; returned values contain no
  pointers and do not retain mesh-store entries or trace owners.
- Only coordinates with production lifecycle tracking or a reported failure
  appear; their installed CPU mesh is included when present. The field is
  centered on the camera chunk and clipped to the current
  `viewDistanceChunks` radius.
- Pipeline owner, voxel occupancy, installed CPU geometry, dirty/remesh intent,
  failure category, and current-revision draw evidence are separate fields.
  Trace detail is the configured tracer's latest retained historical key, kind,
  and build/draw outcomes when that record's coordinate matches. It is never
  proof of the current lifecycle owner. The presentation color is a summary,
  not an authoritative visibility result.
- The legend shows one detail record from the same bounded snapshot: the chunk
  with retained trace history when present, otherwise the tracked chunk nearest
  the camera center. Its rows expose those independent fields without
  rescanning streamer state.

### 3.2 Layout and Scale

- The field is rendered in a fixed-size viewport in the top-left corner.
- Constants live in `src/render/DebugOverlay.cpp`:
  - `kDebugViewportSize = 130`
  - `kDebugViewportMargin = 12`
  - `kDebugTargetSpan = 6.0f`
- Cell size is `kDebugTargetSpan / diameter`, so the overall field size stays
  constant as view distance changes.
- The field is positioned `debugDistance` units in front of the camera
  (`Render::DebugState::debugDistance`, default 8.0).

### 3.3 Colors and Meanings

The runtime legend and cube colors use this state mapping:

- Red: waiting for chunk data, including pending load, generation, and their
  capacity waits.
- Amber: chunk data exists but required neighbor data is still missing.
- Cyan: mesh work is eligible and waiting in the bounded scheduler.
- Blue: the current mesh task is submitted or building.
- Gray: the voxel chunk is empty and its lifecycle completed without a mesh
  build. Voxel-empty chunks have no CPU mesh-store entry.
- Light violet: nonempty voxel data whose accepted CPU mesh has empty geometry;
  this lifecycle retains an empty mesh-store entry.
- Violet: the accepted CPU mesh has nonempty geometry.
- Pink: a dirty/remesh owner is pending while prior CPU geometry may remain
  installed.
- Magenta: a load, generation, mesh, or eviction failure currently owns the
  coordinate.

Lifecycle-complete, voxel-empty, accepted-empty, and accepted-nonempty chunks
are not necessarily drawn. In particular, accepted nonempty CPU geometry is
not a visibility claim. `Drawn` is reported separately and only when the
current mesh revision has produced a real main-pass draw call; store presence,
GPU upload, and the streamer's lifecycle-complete state do not imply it.

### 3.4 Rendering Rules

- Shader: `shaders/chunk_debug`.
- Per-state meshes are built so faces between same-state neighbors are culled.
- Faces between different states are not culled (state boundaries remain
  visible).
- Backface culling is disabled; depth testing is off; alpha blending is on.
- One checked presentation table owns the state index, legend label, color,
  mesh bucket, and VBO count, so buffer allocation and rendering cardinality
  cannot diverge.

---

## 4. Frame Time Graph

- Shader: `shaders/frame_graph`.
- `FrameRenderer::recordFrameTime` appends delta time in milliseconds.
- Ring buffer size is 180 samples; newest samples render on the right.
- Values are clamped to 50 ms and drawn as vertical bars at the bottom of the
  screen.
- Graph rendering disables depth test and uses alpha blending.

---

## 5. Profiler Window (ImGui)

- The ImGui profiler window displays a flame graph for the last frame.
- ImGui is a required build dependency.
- The window is toggled by `imgui_overlay` (F3 by default), independently of the
  GL overlays.

---

## 6. Entity Bounds Overlay

- Shader: `shaders/entity_debug`.
- `renderEntityDebugBoxes` draws a wireframe AABB for every entity.
- Bounds use `Entity::worldBounds`, not model geometry.
- Depth testing is enabled; depth writes are disabled.
- Wireframes render as `GL_LINE` polygons.

If TAA is enabled, entity debug boxes are drawn before the TAA resolve and are
subject to the jitter/resolve pass. The chunk field and frame graph render after
TAA, so they are stable.

---

## 7. Benchmark Logging

- `RIGEL_CHUNK_BENCH=1` enables chunk benchmark statistics and disables swap
  synchronization so rendering does not cap measured throughput. Normal runs
  synchronize buffer swaps to the display.
- When enabled, `Application` prints a summary on exit:
  - Generated, processed, meshed counts and rates.
  - Timing breakdown for generation and meshing.

---

## 8. Streaming Lifecycle Signal

`WorldView::streamingDiagnostics()` exposes the current streaming lifecycle and
generation, chunk-load, mesh, and eviction work counts. Each scheduled category
reports pending requests, in-flight work, and a cumulative started count.
The same bounded snapshot includes region scheduler lifetime counters split by
immutable direct/speculative admission origin, plus demand-owned/speculative-
owned queued and dispatched-undrained gauges. The application consumes these
values in `streaming.region_scheduler` records, including admission-to-worker-
start and worker-execution durations used by benchmark capture.
The speculative pool-pending gauge counts only unstarted submissions that can
still yield capacity; running and completed reads remain in the applicable
dispatched-undrained ownership gauge until their results are drained.
Pending load counts include deferred region requests. Pending generation and
mesh counts include scheduler and capacity wait queues; mesh requests waiting
for neighbor data are also pending. Eviction pending counts cover deferred
persistence and generation-version replacement until it completes or is
canceled. Terminal generation, load, and mesh errors retain an operation and
coordinate diagnostic while they leave a desired coordinate unresolved.

The lifecycle states are:

- `discovering_spawn`: initial camera placement is not complete.
- `awaiting_initial_stream`: spawn discovery completed, but no streaming update
  has run.
- `streaming`: work was pending, in flight, unresolved, or started during the
  current update.
- `stabilizing`: an entire update completed without streaming work.
- `quiescent`: three consecutive full updates completed without pending,
  in-flight, or newly started work.

The application writes a `streaming.lifecycle` log record whenever the lifecycle
state or unresolved failure signature changes. The record contains the state,
all work counts, operation errors, and the stable update count. Repeated updates
do not log an unchanged failure. In particular, this record is the readiness
signal for automated performance capture:

```text
streaming.lifecycle state=quiescent generation.pending=0 generation.in_flight=0 ...
```

The started counts remain cumulative, so tests can take a snapshot at
quiescence and verify that stationary updates do not start additional work.
`generationJobsCompleted` counts submitted results observed by the owner-thread
completion drain, including stale or cancelled running results;
`generationJobsCancelled` counts jobs removed physically before worker start.
At owner-thread observation boundaries, `generationJobsStarted` equals those
two counters plus the current physical generation-owner count.
The diagnostic snapshot also exposes exact source-resolution, logical
generation, retired-work, generation-completion, and mesh-completion owner
gauges. These are observation-only views of the canonical containers, not
parallel lifecycle owners. Quiescent motion and vertical benchmark samples
require every gauge, all pending/in-flight/terminal counts, and both completion
queues to be zero in addition to the `quiescent` state. The overlay assessment
is deliberately non-quiescent: it instead requires a precisely classified
logical load/dependency backlog while all physical execution, completion,
terminal, and canonical-source gauges are zero.
Quiescence bookkeeping examines active requests and explicitly retained
unresolved state; it does not rescan the desired chunk set or poll persistence
for discovery.

### 8.1 Chunk visibility latency trace

`WorldView::setVisibilityTracer` installs an opt-in trace for one identified
chunk coordinate. Construct `ChunkVisibilityTracer` with the coordinate and a
nonzero record capacity, then retain the shared tracer and call `measurement()`
to atomically copy the records and accounting. Each measurement has a sequence
identifying the captured tracer state. A capacity of zero disables the trace
without reading its clock or adding streamer inspection work.

Each record has an immutable coordinate, lifecycle ID, and trace-instance key
and a lifecycle kind: `camera_demand` or `remesh`. Tracers allocate lifecycle
IDs from one process-wide monotonic sequence, while the trace instance keeps a
key associated with its issuing tracer across streamer replacement. ID
exhaustion stops new lifecycle admission rather than wrapping. A
record receives an optional MeshTask identity only when the streamer physically
dispatches that task. The task identity is the actual mesh request ID, work
epoch, chunk instance ID, and mesh revision. Voxel-empty and cached lifecycles
therefore have no MeshTask identity, while a stale completion remains correlated
with its own dispatched input rather than a replacement lifecycle.

The record origin classifies observed persisted loads, observed generation,
remeshes, and already-resident data whose earlier history is left-censored.
`unresolved` means the data source had not reached an observed terminal choice
when the measurement was taken. A trace installed after eligibility can begin
at physical mesh dispatch; its resident-left-censored origin and absent earlier
stages make that late start explicit.

Retention is FIFO and includes pending records in the configured capacity.
The accounting returned with `measurement()` reports retained, dropped,
dropped-unfinished, unmatched-event, and clock-failure counts, so capacity
eviction, unmatched nonterminal events, and unusable timestamps are visible.
Each tracer retains an O(1) evicted-lifecycle high-water mark. A missing key
issued by that trace instance at or below the watermark remains a no-op for the
rest of the tracer's lifetime, regardless of how many later records pass
through the FIFO. Foreign trace-instance keys and future lifecycle IDs remain
unmatched events.

The trace timestamps these stages:

- `desired`, `source_resolution_pending`, and `data_request` cover actual
  camera demand, canonical pre-source ownership for absent data, and the
  scheduler visit that begins resolving the source.
- `generation_scheduler_pending`, `generation_capacity_wait`, and
  `generation_executor_submitted` distinguish logical admission,
  configured-capacity delay, and the executor submission boundary. The final
  stage is recorded immediately before enqueue or inline execution so callable
  entry cannot precede it or occupy a worker waiting for trace publication.
- `generation_worker_start`, `generation_worker_finish`, and
  `generation_ready` distinguish executor queueing, generation execution, and
  a result published for main-thread application. `generation_ready` is
  committed after completion-queue insertion while the queue still excludes
  consumers. `data_ready` is the successful application transition and records
  a generated or persisted origin.
- `neighbor_ready` records the event that supplies the final required neighbor;
  `mesh_eligible` records the transition to dispatchable mesh work.
- `mesh_scheduler_pending`, `mesh_pool_submit`, and `mesh_worker_start`
  isolate mesh scheduler and pool delay from worker execution. These names are
  returned for the existing `SchedulerWait`, `PoolSubmit`, and `WorkerStart`
  stage values.
- `mesh_worker_finish` and `result_accepted` cover build completion and
  main-thread validation.
- `first_draw` is recorded only after the renderer issues a nonempty main-pass
  draw. Mesh-store insertion and the streamer's `ReadyMesh` state do not set it.
  A correlated draw in the narrow interval after mesh-store publication and
  before accepted-outcome publication is retained.

The final-neighbor event can precede the next scheduler visit. In that case
dependency wait ends at the neighbor event and the intervening backlog is part
of mesh scheduler wait, not dependency wait.
`ChunkVisibilityTraceRecord::durations()`
returns an interval only when both of its endpoint stages exist.
`eligibleToWorkerStart` spans `mesh_eligible` through the mesh worker start,
including both scheduler and pool delay. Each dispatched camera or remesh
lifecycle also records `firstObservedMissingDesiredCardinalNeighborCount`: the
number of desired face neighbors absent at its first observed eligibility
check. This is a retained first observation, not a live count. A value of zero
is retained rather than treated as a missing observation, and partial or final
neighbor arrivals do not rewrite the cohort.

For the traced chunk, a record retains fixed-capacity first and current
snapshots containing all six face values. Each value names the direction and
coordinate, whether that face is currently required, and its current state.
The array stays fixed while active blockers can transition from two to one to
zero; resident and no-longer-required faces therefore remain observable
without losing the first snapshot. `source_resolution_pending` identifies a
coordinate in the streamer's canonical load/generation queue before the
scheduler has called a loader or chosen generation. `load_request_pending`
identifies an accepted opaque load request for which no explicit execution
owner is available, and `load_terminal_failed` identifies its ownerless failure
record after completion is drained. Explicit region and payload states
distinguish scheduler pending, physical pool queued, actual worker running,
completion published but undrained, and owner-attributed terminal failure.
Region failures additionally distinguish retry waiting; payload failures
transition directly to terminal failure.
Generation states distinguish scheduler pending, capacity waiting, executor
queued, worker running, result published, and terminal failure. Ready resident,
no longer required, and explicitly unowned are separate; the presence of a
loader callback alone never creates a load owner. The streamer resolves this
only for the six face neighbors of the traced chunk. Their current values are
refreshed once at the end of an owner-thread streaming update and describe that
last observation; they are not a continuously atomic cross-thread view. With
tracing absent or capacity zero, the classifier is not called, no debug-volume
scan is added, and no scheduling state is mutated.

`sourceResolutionWait` covers canonical pre-source admission through the
source-resolution scheduler visit. Derived generation intervals are
`generationQueueWait` (logical generation
pending to the start of capacity wait when capacity blocks, otherwise to
executor admission), `generationCapacityWait` (capacity wait to executor
admission), `generationPoolWait`, `generationExecution`, and
`generationResultWait` (published result to successful application). Scheduler
and capacity waits therefore never overlap. Existing dependency, mesh
scheduler, mesh pool, mesh execution, acceptance, and first-draw intervals
remain separate. `dataWait` continues to span the complete data request through
apply path and can therefore include load or generation sub-intervals.

Stage absence is intentional and follows the lifecycle:

| Lifecycle or outcome | Legitimately absent stages |
| --- | --- |
| Camera demand for already-resident data | `source_resolution_pending`, `data_request`, all generation stages, and `data_ready`; `neighbor_ready` is also absent when required neighbors were already resident. |
| Remesh, including retained fringe work | `desired`, `source_resolution_pending`, `data_request`, and `data_ready`; `neighbor_ready` is absent unless the remesh actually waits for a missing required neighbor. |
| Cached mesh | No MeshTask identity and no data, generation, dependency, mesh scheduler/pool/worker, or `result_accepted` stages. Cached empty geometry also has no `first_draw`. |
| Voxel-empty chunk | No MeshTask identity, pool/worker, `result_accepted`, or `first_draw` stages. Earlier demand or data stages remain only if those transitions occurred. |
| Dispatched stale or failed task | `result_accepted` and `first_draw`; pre-dispatch stages remain absent when tracing began after those transitions. |
| Accepted empty geometry | `first_draw`; resident or remesh rules still determine which earlier stages are absent. |
| Accepted nonempty geometry | `first_draw` remains absent until an actual main-pass draw submission. |
| Camera leave, tracer replacement, reset, generator replacement, or streamer destruction before dispatch | MeshTask identity and all task stages; any earlier stages remain recorded. |
| Clock callback failure | The timestamp for that event; lifecycle outcomes and scheduler work still advance normally. |

Camera departure after physical mesh dispatch preserves the original record's
MeshTask identity. Its late successful or failed result is rejected as stale
without publishing a mesh or terminal error. Re-entry starts a new camera
lifecycle; after the stale result is drained, the replacement dispatch receives
a distinct MeshTask identity.

Camera departure, reset, generator replacement, and tracer replacement freeze
the captured pre-draw lifecycle. Late worker and completion callbacks cannot
change its origin, task identity, blocker snapshots, stages, timestamps,
sequence, or terminal outcome, including after record eviction. The issuing
trace instance and evicted-lifecycle watermark make that immutability
independent of subsequent FIFO churn. An accepted nonempty
geometry record remains eligible for the separate real `first_draw`
observation until its draw outcome terminates.

Build outcomes distinguish cached empty/nonempty geometry, voxel-empty chunks,
accepted empty/nonempty geometry, stale results, failures, and each lifecycle
exit listed above. A nonempty cached or accepted record separately ends its draw
lifecycle as `drawn`, `camera_left_before_draw`, `mesh_removed_before_draw`,
`mesh_replaced_before_draw`, or `trace_replaced_before_draw`. Mesh entries retain
strong tracer ownership until one of those transitions, and `first_draw` is set
only by the renderer after a real nonempty main-pass draw call.

Clock callbacks are serialized for worker safety, are invoked without the
record mutex held, and cannot propagate exceptions into streaming or rendering
work. A callback failure leaves that event timestamp absent while terminal and
draw outcomes still close normally. `observed(stage)` remains true for such a
transition, and a later idempotent notification does not retimestamp it.
Installing or disabling a tracer does not synthesize stages, requeue scheduler
work, or add stationary desired-set scans. When installation targets a resident
chunk already blocked by a desired neighbor's canonical generation flight, it
takes one bounded six-face value snapshot so the existing queued, running, or
published phase is observable without a second ownership map.

Do not treat an absent timestamp or duration as zero. Reject a measurement
window used for latency claims when it contains pending build outcomes,
nonempty outcomes still awaiting a draw transition, retention drops, unmatched
events, or clock failures. Also reject `unresolved` and
resident-left-censored records from end-to-end demand latency aggregates;
classify them separately if resident reuse is the metric being studied. Use
persisted, generated, camera-demand, and remesh populations separately, and
include a duration only when both endpoint stages are present. These rules also
apply when a measurement sequence is internally consistent: atomic capture
prevents mixed accounting, but it cannot make an incomplete or censored sample
complete.

### 8.2 Near-camera visibility benchmark

`Rigel_near_camera_visibility_benchmark` is a controlled cold-generation
fixture for stationary, continuous +X, continuous +Z, and diagonal XZ camera
workloads. A moving sample traces the final camera-containing chunk while the
camera advances one chunk on each of six updates, then holds that position
until the real streaming lifecycle becomes quiescent. It uses the production
generator, streamer, worker pools, mesh builder, and visibility trace, but it
is neither shipped nor interactive scheduling. Persistence returns a
controlled missing-probe outcome, so every sample follows generation without
changing production scheduler settings.

The application-like mode is the default and starts updates at 16.667 ms
intervals, approximately 60 Hz. Every successful sample still terminates only
after the production lifecycle reports `StreamingLifecycleState::Quiescent`;
there is no fixed startup or completion sleep. The deadline only converts a
failure to reach quiescence into a failed sample. An explicit
`--scheduler-lower-bound-stress` mode runs the old unpaced loop and labels its
timings as non-representative scheduler lower-bound stress, so those timings
must not be used as representative time-to-visible evidence.

The machine-readable `evidence_scope` is
`controlled_fixture_application_like_cadence` for paced runs and
`nonrepresentative_scheduler_lower_bound` for stress runs. Both modes report
`shipped_time_to_visible_evidence=false` and
`interactive_time_to_visible_evidence=false`.

Build and run it from an out-of-tree build directory:

```text
cmake -S <source-directory> -B <build-directory> \
    -DRIGEL_BUILD_BENCHMARKS=ON
cmake --build <build-directory> --target Rigel_near_camera_visibility_benchmark
<build-directory>/Rigel_near_camera_visibility_benchmark --samples 20
```

`RIGEL_BUILD_BENCHMARKS` defaults to `OFF`; enabling it does not change normal
production scheduling or configuration.

Version 4 emits each raw sample followed by nearest-rank P50/P95/P99 summaries
for workload and workload/first-observed-missing-desired-cardinal-neighbor
cohorts. It reports desired-to-generation-start, aggregate generation queue
wait, logical generation scheduler wait, generation capacity wait, generation
pool wait, generation execution, data-ready-to-neighbors-ready,
neighbors-ready-to-mesh-start, mesh execution, desired-to-accepted geometry,
and desired-to-first-draw. The current runner is headless, so every raw sample
and zero-sample first-draw summary prints `unavailable`, and the header
explicitly reports `accepted` as the available endpoint. A generated trace
without a capacity-wait transition contributes zero capacity wait; other
absent trace durations continue to invalidate the sample rather than being
coerced to zero.

The conversion accepts only generated `camera_demand` records ending in
accepted nonempty geometry. A first-draw endpoint additionally requires the
record's real `drawn` outcome; persisted, resident-left-censored, remesh,
voxel-empty, accepted-empty, stale, failed, and replaced-before-draw records
are rejected. When the first missing desired-cardinal-neighbor count is zero,
there is no final-neighbor event to observe: the runner uses `data_ready` as
the zero-duration boundary and labels it `inferred_data_ready`, even if a
conflicting `neighbor_ready` timestamp is present. A focused conversion
fixture supplies both timestamps and requires the data-ready-to-worker-start
interval and inferred boundary source. Nonzero cohorts label the boundary
`observed_final_neighbor`.

Nearest-rank P99 for a 20-sample cohort is the observed cohort maximum. It is
a noisy tail observation, not an interpolated or stable population estimate;
the runner prints that interpretation in its header.

The CLI rejects integer suffixes and overflow, more than 1,000 samples per
workload, view radii above the production maximum, worker or mesh-queue values
above production limits, more than 100,000 motion steps, timeouts above one
hour, and comparison budgets above one hour. These bounds also keep coordinate
motion and all sample-vector allocations finite. Every successful raw sample
prints the exact generation/load/mesh/eviction pending, in-flight, completion,
terminal-failure, source-resolution, logical-generation, and retired-work
gauges alongside the cumulative completion/cancellation counts and
`completion_state=quiescent`.

`--collect-debug-detail` is a separate opt-in measurement mode. It calls the
same bounded streamer snapshot used by the chunk field after each streaming
update, but it does not create a GPU context or render the overlay. Normal
benchmark runs leave this collection disabled. This switch controls only the
headless benchmark snapshot; it is not the production overlay toggle or a
measurement of `WorldView` draw-cache decoration, presentation construction,
GL drawing, or ImGui.

The configured mesh queue setting and effective submission limit are distinct.
Version 3 reads an effective limit from the streamer's runtime diagnostic
snapshot, the same source used by production diagnostics, rather than
reconstructing it from worker configuration in the runner. When the same
benchmark source is built against the baseline scheduler, whose diagnostic
snapshot has no effective-limit field, it reports the authoritative configured
behavior instead: an unbounded setting is labeled `configured_unbounded` and
`effective_mesh_submission_limit=unavailable`. It does not assign repaired-tree
limit semantics to that scheduler.

The assessment takes an operator-supplied comparison budget, defaulting to
50 ms because that is approximately three default update intervals and makes
large stage residuals easy to identify. It is a comparison budget only: meeting
or exceeding it classifies only the measured numeric result. It is not proof of
interactive acceptability, does not imply a neighbor-policy conclusion, and
does not change production behavior.

#### Matched paced capture

The exact revisions used to define this comparison were:

- baseline engine revision:
  `677a4439afa3a40aa6799fa78ed4f1d8b00e30e3`;
- trace instrumentation revision:
  `b1cda21b11b953c1e7c159c94d2c07f6d6a44cd8`, which retains and names the
  first observation from the initial dependency instrumentation at
  `5d04e609865449b7679331441360a29dadebf5ce`;
- paced benchmark revision:
  `5e92575aa380786f69f55588f85510984ca02933`;
- repaired engine revision:
  `8a3b526e402da4f9a2083d5a0479f6af598e5c74`, including the bounded priority
  dispatcher introduced at
  `b11dce614d89dba9b9e32612123afdb88a3c3006`.

The evidence-scope, cadence-boundary, runtime-limit, and assessment-label
hardening is revision `482e6dfc4ef4e0d3b128d1017e4611c819b619f9`.
Version 3's cross-scheduler limit metadata and initial CLI evidence-contract
regressions are revision
`034eda5ef39539c1031171025fca06cf80a5e321`. The scheduling-arithmetic
upper-bound guard and its CLI mutation regression are revision
`5d61aff033c0f331515c366a8481373904b40a59`.

Both builds used byte-identical version 2 benchmark sources. The capture ran
sequentially on the same 12th Gen Intel Core i7-12700 host with 20 logical CPUs,
Linux 7.0.12-201.fc44.x86_64, Debug builds, and otherwise uncontrolled host
load. Each build used 10 samples per distance, view distance 2, two workers,
unbounded generation, mesh, update, and apply settings, a 16.667 ms cadence,
and the 50 ms comparison budget. The exact invocation from each build directory
was:

```text
./Rigel_near_camera_visibility_benchmark --samples 10 --view-distance 2 --worker-threads 2 --timeout-seconds 30 --update-interval-ms 16.667 --comparison-budget-ms 50
```

All 40 samples reached the real quiescent state, reported three stable updates,
had no stale mesh results, and used accepted geometry as the endpoint. Times
below are nearest-rank P50/P95 in milliseconds; the neighbor count is the first
observed missing desired-cardinal-neighbor count, not a live count.

Both configurations set `mesh_queue_limit=0`. At the baseline revision that
means configured-unbounded submission: ready mesh work can be submitted beyond
the physical worker count into the worker backlog. In the repaired revision the
same setting remains unbounded as configuration, while the scheduler's runtime
diagnostic reports an effective submission limit of one for the configured two
workers. The capture benchmark originally duplicated that repaired-tree value
from the worker setting and therefore printed `effective_mesh_submission_limit=1`
for both executables; that baseline field is not used as evidence. The current
runner instead emits `mesh_submission_limit_source=runtime_diagnostics` only
when that field exists and emits `runtime_configuration`,
`configured_unbounded`, and an unavailable effective limit for the baseline.
A focused regression covers both diagnostic shapes. The conditional CLI
regression uses four configured workers with an explicit queue limit of one;
the runtime effective limit is one, whereas the removed worker-count formula
would report two. Baseline results are described only as configured-unbounded
and are not assigned a false effective limit.

| Distance / first count | Scheduler | Desired to visible | Dependency wait | Eligible to worker | Scheduler wait | Pool wait | Worker execution |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Camera / 6 | Baseline | 583.420 / 616.771 | 467.826 / 484.490 | 16.982 / 17.116 | 16.900 / 17.030 | 0.079 / 0.190 | 1.736 / 1.926 |
| Camera / 6 | Repaired | 583.418 / 616.782 | 467.832 / 501.246 | 16.972 / 17.073 | 16.911 / 16.976 | 0.079 / 0.132 | 1.780 / 1.930 |
| Adjacent / 5 | Baseline | 2266.735 / 2300.108 | 1916.693 / 1950.046 | 19.971 / 20.006 | 19.525 / 19.624 | 0.466 / 0.542 | 1.447 / 1.491 |
| Adjacent / 5 | Repaired | 2266.759 / 2283.469 | 1916.712 / 1933.430 | 18.097 / 18.231 | 18.034 / 18.177 | 0.055 / 0.074 | 1.444 / 1.776 |

For the adjacent cohort, bounded priority dispatch reduced
eligible-to-worker-start P95 by 8.9%, scheduler-wait P95 by 7.4%, and pool-wait
P95 by 86%. Desired-to-visible P95 was 0.7% lower and the camera cohort was
unchanged, so this small controlled sample does not establish a material
end-to-end improvement. Dependency wait still dominates both cohorts and
exceeds the operator's 50 ms comparison budget.

This controlled cold-generation capture is not evidence of interactive
acceptability. It has no GPU context or main-pass draw, uses a controlled
missing probe instead of a shipped persistence backend, is a Debug build, and
does not control competing host load. No neighbor-policy conclusion is drawn
from the comparison budget. The required external validation remains a Release
interactive capture on identified hardware using the shipped persistence
backend, the renderer's actual `first_draw` event, the same camera-containing
and adjacent cohorts, and quiescence as the terminal signal.

#### Generation dispatch Release capture

A second matched capture isolates generation dispatch cadence at these engine
revisions:

- FIFO generation baseline: `217b54448b94464836b9dcc7f52ef03d862f987a`;
- one-worker-width submission bound:
  `c56ef994f6312e6de9b1c137a24126981c809d6c`;
- one running plus one standby worker-width bound:
  `8382751353e0680717c4451ed42acc9e275f46b2`.

The benchmark and build-system sources are byte-identical across the three
revisions. All builds used `Release`, GCC 16.1.1, and the same 12th Gen Intel
Core i7-12700 host with 20 logical CPUs running Linux
7.0.12-201.fc44.x86_64. The executables ran sequentially with otherwise
uncontrolled host load. Each build used 20 samples per distance, view distance
2, two total workers (one generation and one mesh worker), unbounded
generation, mesh, update, and apply limits, a 16.667 ms cadence, and the
accepted-geometry endpoint:

```text
./Rigel_near_camera_visibility_benchmark --samples 20 --view-distance 2 --worker-threads 2 --timeout-seconds 30 --update-interval-ms 16.667 --comparison-budget-ms 50
```

All 120 samples reached quiescence, used accepted geometry, and reported no
stale mesh results. Times below are nearest-rank P50/P95 in milliseconds.
The dependency column measures mesh dependency wait. The eligible-to-worker,
scheduler, pool, and worker columns are mesh-stage P95 timings, not generation
queue latency, and are reported separately rather than added.

| Distance | Generation dispatch | Desired to accepted P50/P95 | P95 residual from FIFO | Dependency P95 | Eligible to worker P95 | Scheduler P95 | Pool P95 | Worker P95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Camera | FIFO baseline | 216.843 / 233.438 | reference | 166.737 | 16.928 | 16.832 | 0.091 | 0.280 |
| Camera | One worker width | 266.827 / 266.934 | +14.3% | 200.135 | 16.963 | 16.869 | 0.094 | 0.295 |
| Camera | Running plus standby | 216.839 / 216.943 | -7.1% | 150.102 | 16.908 | 16.827 | 0.122 | 0.278 |
| Adjacent | FIFO baseline | 800.132 / 800.196 | reference | 633.544 | 33.380 | 33.328 | 0.065 | 0.176 |
| Adjacent | One worker width | 1133.408 / 1133.538 | +41.7% | 866.818 | 16.861 | 16.795 | 0.075 | 0.197 |
| Adjacent | Running plus standby | 883.424 / 916.785 | +14.6% | 666.815 | 50.147 | 50.094 | 0.074 | 0.197 |

The standby wave removes the worker bubble visible with the exact one-worker
submission bound: the camera P95 is 18.7% lower than that bound and the
adjacent P95 is 19.1% lower. Relative to the FIFO baseline, the final residual
is -7.1% for the camera cohort and +14.6%, or 116.589 ms, for the adjacent
cohort. This is not a claim of parity. The remaining adjacent residual is
not fully decomposed by independently ranked stage percentiles;
dependency-wait P95 is 33.271 ms above the FIFO capture, while scheduler-wait
P95 is 16.766 ms above it.

No interactive first-draw capture was possible in this headless environment;
this Release evidence stops at accepted geometry. Interactive first-draw
validation with the shipped persistence backend remains an explicit external
gate.

#### Moving-camera Release capture

The hardened moving-camera comparison used the FIFO generation engine at
`217b54448b94464836b9dcc7f52ef03d862f987a` as the baseline and the bounded
priority behavior completed at
`e17b6899cedc0f3200d2a3a4b62c41160dfea0a0` as the repaired engine. Both were
rebuilt and rerun with the byte-identical version 5 benchmark sources at
`acabd94f52c30f57e57f5cd13c7533f3059af6ad`. SHA-256 was
`7ac6ee044490a0b8c94d2bb83fe8833cf6493f15c6c0807846118b8b602a23c6`
for the converter, `68018e4585dcf0c4fd8386b300afc2347db200cbb80afa87350d50011daa6eda`
for its header, and
`0b3b83b87ff38410aed845ffceb251f3078c481a6591f521372bf55a30f55f26`
for the runner in both source trees.

The detached FIFO tree added only 14 lines of observation support: completion
counters and canonical/completion queue-size diagnostics. That baseline-only
patch has SHA-256
`f6f64bd34187d56829b6f9fcb56386690090accd2f0918de6da99adf83b2b865`;
it does not add priority, capacity, cancellation, or wake behavior. The raw
FIFO and priority logs have SHA-256
`b253d04c97f2413cc137e4b78b7130a979d87030d5c20a44494adba261bdc637`
and `6d46bb563d8f376917ec53b2167d2f53159623008fcf58734b20bac2f3f44c67`.

Both builds used Release, GCC 16.1.1, Linux 7.0.12, and the same 12th Gen Intel
Core i7-12700 host with 20 logical CPUs. Runs were sequential with otherwise
uncontrolled host load. Each build used 20 samples per workload, view distance
2, six movement updates, two total workers split into one generation and one
mesh worker, unbounded configured generation, mesh, update, and apply limits,
a 16.667 ms cadence, and controlled missing persistence:

```text
./Rigel_near_camera_visibility_benchmark --samples 20 --view-distance 2 --worker-threads 2 --motion-steps 6 --timeout-seconds 30 --update-interval-ms 16.667 --comparison-budget-ms 50
```

All 160 samples reached quiescence, accepted geometry, reported three stable
updates and zero stale mesh results, and retained clean trace accounting.
Times are P50/P95/P99 milliseconds. `Gen queue` spans generation scheduler
admission through actual generation worker start; the narrower logical,
capacity, and pool waits remain in the raw capture. `Data-neighbors` is the
remaining desired-cardinal-neighbor dependency. `Neighbors-mesh` ends at the
actual mesh worker start. No first-draw sample exists because the runner is
headless.

| Workload | Engine | Desired-gen start | Gen queue | Gen execution | Data-neighbors | Neighbors-mesh | Mesh execution | Desired-accepted | Desired-first draw |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Stationary | FIFO | 0.098/0.115/0.160 | 0.069/0.109/0.154 | 27.413/29.902/32.782 | 150.062/166.725/166.902 | 16.812/16.851/16.902 | 0.266/0.317/0.413 | 216.839/233.431/233.448 | unavailable (0) |
| Stationary | Priority | 0.074/0.106/0.121 | 0.065/0.078/0.116 | 25.995/34.076/34.195 | 150.005/150.084/166.677 | 16.858/16.940/17.121 | 0.260/0.330/0.461 | 216.819/233.442/233.453 | unavailable (0) |
| Continuous +X | FIFO | 285.641/291.033/292.559 | 285.633/291.026/292.550 | 25.204/25.586/25.722 | 233.340/233.528/233.551 | 16.875/33.328/33.363 | 0.151/0.164/0.251 | 583.387/600.134/600.214 | unavailable (0) |
| Continuous +X | Priority | 33.392/33.464/33.464 | 33.386/33.457/33.458 | 25.205/25.416/25.453 | 150.004/150.019/150.070 | 16.845/16.902/16.917 | 0.251/0.266/0.280 | 250.068/250.086/250.120 | unavailable (0) |
| Continuous +Z | FIFO | 287.930/291.245/292.910 | 287.922/291.236/292.903 | 25.386/25.880/26.110 | 233.445/233.513/233.523 | 33.279/33.340/33.340 | 0.147/0.252/0.261 | 599.979/600.079/600.109 | unavailable (0) |
| Continuous +Z | Priority | 33.433/33.502/33.528 | 33.427/33.496/33.523 | 25.280/25.643/26.118 | 150.007/150.087/150.101 | 16.842/16.902/16.920 | 0.250/0.258/0.292 | 250.071/250.131/250.147 | unavailable (0) |
| Diagonal XZ | FIFO | 269.621/274.022/276.022 | 269.613/274.013/276.014 | 25.272/25.851/26.096 | 183.176/183.251/183.382 | 16.836/16.898/16.931 | 0.154/0.255/0.260 | 516.721/516.786/516.807 | unavailable (0) |
| Diagonal XZ | Priority | 33.733/35.424/35.811 | 33.727/35.418/35.804 | 25.228/26.154/26.194 | 150.006/150.055/166.678 | 16.846/16.910/16.919 | 0.260/0.272/0.280 | 250.079/250.134/266.686 | unavailable (0) |

The target's generation-queue P95 fell 88.5% in +X, 88.5% in +Z, and
87.1% diagonally. Desired-to-accepted P95 fell 58.3%, 58.3%, and 51.6%,
respectively, while stationary P95 remained within 0.1 ms. The FIFO target
spent nearly all of its pre-generation latency in the physical pool queue;
the priority target instead started within 36 ms at P99. This, together with
the constrained-worker ordering regressions, demonstrates that an approached
chunk is no longer buried behind the older unstarted generation backlog.

The independent diagonal regression uses one generation worker with a bounded
standby wave. It holds the first demanded job running, moves the camera one
chunk in both X and Z, and proves the newly entered diagonal-near coordinate
starts before retained older farther work. The running job remains demanded
and is not cancelled; after release, all logical, physical, completion, and
retired owners drain to stable quiescence.

Neighbor dependency remains the largest repaired P95 stage: about 150.1 ms in
all three moving workloads, versus at most 35.4 ms for generation queue wait
and 26.2 ms for generation execution. This controlled
result identifies the residual without establishing interactive impact. It
does not justify adding a provisional neighbor policy; a separately bounded
interactive first-draw study would be required before such a change.

The two-worker fixture exercises the production split as one generation plus
one mesh worker. The shipped `worker_threads=12` setting remains a six/six
split, with generation submission narrowed to twelve submitted-but-undrained
jobs. No split or worker-pool policy changed in this validation.

The shipped view radius is 12 and the shipped vertical bounds are -64 through
320. A direct enumeration of the spherical desired set around chunk Y=0
contains 7,153 coordinates. Of those, 2,482 chunks are wholly below the world,
70 are wholly above it, and 2,552 total (35.7%) are wholly out of world. For
surface camera chunk Y values 1, 2, and 3, the wholly out-of-world shares are
31.7%, 28.7%, and 27.0%.

The shipped-config assessment sampled nine X/Z columns at chunk Y=-3, wholly
below `min_y`, and nine at chunk Y=11, wholly above `max_y`, in the same Release
configuration, build directory, and host as the CPU overlay assessment. The
direct generator results were:

| Position | Nonempty chunks | Total non-air blocks | Generation P50/P95/P99 (ms) |
| --- | ---: | ---: | ---: |
| Wholly below | 9 / 9 | 291,833 | 17.084 / 19.281 / 19.281 |
| Wholly above | 0 / 9 | 0 | 16.235 / 18.178 / 18.178 |

This hardened rerun used assessment source
`16e241f1027153e182c3f5ec07bc59f503ba3e55`; its raw log has SHA-256
`f54128df9b6eb64b1f7e48cf6404d00ac3504115e029672d66d48f11b398bbd5`.
It emits all 18 coordinate, occupancy, and execution-time samples before the
two aggregate lines, so the percentiles and occupancy totals are independently
recomputable. With nine nearest-rank samples, both P95 and P99 select the
observed cohort maximum; they are the same noisy tail observation, not
independent population-tail estimates.

The production-lifecycle cohorts used the shipped view radius 12, unload
radius 13, and 12-worker six/six generation/mesh split. Each generated the
full 7,153-coordinate desired sphere and reached quiescence. The below-bound
target contained 32,768 non-air voxels; 5,761 mesh jobs completed and were
accepted, and the fully occluded target ended as accepted empty geometry rather
than voxel-empty. The above-bound run likewise completed 7,153 generation jobs;
335 mesh jobs completed and were accepted even though the target itself was
voxel-empty. Generation cancellation/failure and mesh stale/failure counters
were zero in both cohorts. Pending, in-flight, completion, terminal-failure,
source-resolution, logical-generation, retired-work, load, and eviction owners
were all zero at quiescence. The below and above runs required 7,184,743 and
8,211,817 updates respectively; the combined process took 42.24 seconds and
peaked at 6,318,664 KiB RSS on this host. This is measurement evidence only;
no regression asserts that out-of-bounds chunks must remain nonempty or
continue consuming mesh work.

Thus vertical bounds do not clip generation: above-bound empty results still
consume generation time, while below-bound results can consume both generation
and mesh capacity. This is a separate finite-world P1; it does not invalidate
the measured moving-camera priority repair, but it does prevent the overall
streaming program from being declared merge-ready. No vertical clipping or
provisional neighbor policy is introduced in this change.

This is a substantial finite-world P1 assigned to the immediately dependent
finite-world clipping change. Overall streaming-program merge readiness is
withheld until that repair prevents wholly out-of-world desired coordinates
from consuming generation and mesh capacity. The generation-priority runtime
result remains valid and distinct from this blocking finite-world defect.

#### Overlay instrumentation comparison

The repaired stationary workload was repeated on the same Release build and
host with `--collect-debug-detail`. The disabled run's P95
desired-to-generation-start and desired-to-accepted times were 0.103 ms and
233.371 ms. With detail collection they were 0.170 ms and 233.438 ms. The
enabled run collected 1,584 full snapshots and 52,272 returned records across
20 samples, averaging 33 records per snapshot. The 0.067 ms accepted-geometry
P95 difference is 0.03%, so the bounded radius-2 snapshot was not a material
end-to-end perturbation in this capture.

This comparison excludes GL field drawing and ImGui presentation and is not an
interactive overlay cost claim. It is retained only as the radius-2 streamer
snapshot comparison.

`Rigel_streaming_assessment_benchmark` adds two production-path comparisons at
the shipped radius. Its renderer-independent mode invokes
the CPU presentation boundary used by `Render::renderDebugField`, which calls
`WorldView::getChunkDebugStates`,
decorates installed geometry from the renderer draw cache, selects detail,
and builds the presentation maps and exposed-face meshes. It makes no GL calls,
so its automated test is safe without a graphics context. It intentionally
excludes driver/GPU work and ImGui and labels both exclusions in its output.
The full mode creates a real context,
initializes shipped block and texture resources, runs `FrameRenderer`, executes
the GL field and frame graph, builds the ImGui legend/detail window, submits the
ImGui draw data, and synchronizes each timed frame with `glFinish`.

The overlay captures used runner source
`499292343d871e5a65df4da55301cbc5a0aeb024`, descended from the initial
assessment at `ce1dd039bf0e37db89aec45efc0cb3acfffc4139` and the isolated CPU
presentation boundary at `89afbefc2ba4f5b90f66cba396ae23f6b8223f3a`.
The retained version 5 runner at
`16e241f1027153e182c3f5ec07bc59f503ba3e55` adds the raw vertical sample lines
described above and the associated schema-version bump.

For a new capture, build once in Release and run the modes from that binary:

```text
./Rigel_streaming_assessment_benchmark --vertical-only
./Rigel_streaming_assessment_benchmark --overlay-cpu-only --frames 120
./Rigel_streaming_assessment_benchmark --overlay-only --frames 120
```

The corrected Release CPU comparison used 7,153 tracked records in the
15,625-coordinate radius-12 cube, including one installed mesh that exercised
`WorldView` draw-evidence decoration. Its controlled synthetic loader reports
Missing inside distance-squared one and indefinitely Queued outside. The
matching pending callback leaves `m_loadPending` as the sole owner of all 7,146
retained loads; `sourceResolutionPending` is exactly zero. Five chunks wait for
neighbors. The lifecycle is explicitly Streaming, not Quiescent, only because
of that classified logical backlog. The field contained 7,146 waiting-for-data,
five waiting-for-neighbor, one voxel-empty, and one accepted-nonempty record,
with no scheduler-wait, mesh-work, dirty-remesh, or terminal record.

All seven generation jobs were completed, and the one physical mesh job was
completed and accepted before timing. Generation pending, in-flight,
completion, terminal, canonical-source, canonical-generation, and retired-work
gauges were zero; physical mesh in-flight, completion, and terminal gauges were
zero. The cumulative partitions were generation 7 started, 7 completed, zero
cancelled, zero failed and mesh 1 started, 1 completed, 1 accepted, zero stale,
zero failed.

Disabled P50/P95/P99 were all below the printed 0.001 ms precision. Enabled
P50/P95/P99 were 2.003/2.026/2.035 ms, a 2.026 ms P95 increase. This is a
CPU-side lower bound because GL and ImGui were excluded; the P95 increase is
12.2% of a 16.667 ms frame budget. The raw log has SHA-256
`4c1fc95999eae6ebd574e15372e792883146f7627c444102cd1550acbaef781b`.
All 120 raw pairs are emitted with their
index, execution order, and exact disabled/enabled durations. The independently
asserted disabled and enabled sample counts are both 120, equal to the requested
pair count. The summary records the Release build, 20 hardware threads, shipped
configuration, radius, scanned and tracked counts, draw-evidence count, exact
logical backlog, cumulative accounting partitions, and hard-zero physical
execution gauges.

The same runner reports `startup_overlay_enabled=false`. The focused toggle
regression verifies that F1 release changes false to true and a second release
changes it back. Production collection, presentation construction, GL work,
and the ImGui legend are therefore opt-in rather than per-frame startup work.

The same Release binary subsequently completed the existing production
GLFW/OpenGL/ImGui path under Xvfb with Mesa software rendering:

```text
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./Rigel_streaming_assessment_benchmark --overlay-only --frames 120
```

The binary used assessment source `499292343d871e5a65df4da55301cbc5a0aeb024`.
The repository checkout did not contain its ignored texture pack, so the
`assets/textures` directory from an adjacent clean Rigel checkout was staged
only while configuring and packing this validation binary, then removed; it is
an external validation input rather than a committed product artifact.
The imported shipped resources contained 248 registered blocks and 134 atlas
textures. The capture used the same classified 7,153-record cohort and reported
one current-revision `drawn` record after the main pass. It identified
`llvmpipe (LLVM 22.1.5, 256 bits)` and OpenGL
`4.5 (Core Profile) Mesa 26.0.8`. Disabled P50/P95/P99 were
0.545/0.669/0.776 ms; enabled values were 3.358/3.712/6.113 ms. The 3.043 ms
P95 increase is 18.3% of a 16.667 ms frame budget, so shipped-radius detail is
material and supports the default-off, F1-opt-in behavior. All 120 independently
counted pairs are present with alternating order; their raw log has SHA-256
`6c08a1e730800d9c4d43432c1974de2ecd4978b4f4648607606791c87afae1a2`.
This is a controlled virtual-display/software-renderer result, not an
interactive first-draw capture. Interactive first-draw validation with the
shipped persistence backend remains an external gate.

Assessment frame counts are strictly parsed and bounded to 10,000 per mode;
suffixes, overflow, repeated frame options, conflicting modes, and frame counts
for vertical-only mode are rejected before assets load. Graphics and ImGui
shutdown, renderer/view release, atlas release, and cached shader destruction
all occur while the real context remains current, including failure paths.
The context constructor rolls back GLFW and UI state locally if initialization
throws, and both assessment executables catch exceptions only outside their
work-owning scopes so automatic objects unwind before the failure is reported.

#### Scheduler boundary assessment

The following structural count treats a mechanism as one distinct policy/data
model, not every branch or container used to implement it. Mesh scheduling is
listed separately even though `ChunkStreamer` owns it.

| Area | Priority | Capacity | Pending | Cancellation | Wake/refill | Retry | Ownership |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ChunkStreamer` source/generation | 2 | 3 | 2 | 2 | 2 | 2 | 4 |
| `ChunkStreamer` mesh scheduling | 3 | 3 | 3 | 2 | 3 | 3 | 3 |
| `ChunkStreamer` persistence/eviction/version replacement | 2 | 2 | 3 | 3 | 3 | 2 | 4 |
| `AsyncChunkLoader` | 3 | 4 | 5 | 4 | 4 | 2 | 5 |
| `ThreadPool` | 2 | 1 | 2 | 1 | 2 | 0 | 1 |

The source/generation row counts the shared desired importance and ordered
generation index; update, dispatch, and apply bounds; the canonical source and
generation pending lanes; load and generation retirement; update/completion
refill; terminal requeue/config reconciliation; and source, load-request,
logical-generation, and physical-flight ownership. The mesh row counts shared
importance, missing/dirty class ordering, explicit dirty priority; dispatch,
class, and apply bounds; ready, dependency, and replacement pending state;
pending retirement and stale-flight rejection; neighbor, completion, and dirty
wakes; failure/revision/config reconciliation; and pending, in-flight, and
completion ownership.

The persistence/eviction/version row counts distance/cache-pressure and
version-replacement ordering; resident/cache and deferred-work bounds;
persistence retry, replacement retry, and replacement-wait pending state;
re-demand, reset, and configuration-change retirement; movement, update, and
retry-deadline wakes; delayed persistence and generation-version
reconciliation; and resident chunk, cache, retry, and replacement-wait
ownership. The canonical source handoff is counted only in the
source/generation row rather than duplicated here.

The loader row counts direct/speculative ordering, promotion, and executor
priority; chunk, region, executor, and drain bounds; direct, deferred, retry,
region, and payload pending state; coordinate, direct, speculative, and
submitted-pool cancellation; request, completion, retry-deadline, and prefetch
wakes; region and chunk retry policies; and request, region, dispatched,
payload, and terminal owners. The pool row counts its two priority lanes and
promotion, fixed worker capacity, two pending deques, incarnation-qualified
cancellation, enqueue/stop notifications, and pool-local job ownership.

A narrow generation-scheduler boundary has emerged inside `ChunkStreamer`:
`ChunkImportance`, the pending-generation map and ordered index,
`generationDispatchLimit`, reprioritization, dispatch, activation, retirement,
and settlement form one coherent unit. It is not yet an independently owned
module. Request epoch, generator version, visibility tracing, cancellation,
completion validity, state transitions, and source-resolution handoff still
cross that boundary. Extracting only the queue would duplicate ownership or
callbacks; an eventual extraction should move the complete logical-to-physical
owner transition, not introduce a public scheduler interface now.

Remaining maintainability debt is bounded and non-blocking:

- Configuration: `worker_threads` owns an implicit generation/mesh split, and
  the effective generation submission bound is not exposed alongside the
  mesh submission diagnostic. No new setting is needed for current behavior.
- Overlay language: the red field summary intentionally collapses source,
  load, generation-pending, capacity, queued, and running states into “waiting
  for chunk data”; the selected detail record is required for exact phase
  attribution and may describe retained history rather than the live owner.
- Test coupling: mutation-sensitive streamer regressions use a large private
  test-access surface and container-level assertions. This catches ownership
  mutations but makes internal extraction costly.
- Module boundary: source selection, logical generation scheduling, physical
  flight ownership, and completion settlement remain in one streamer class.
  That preserves one execution path today but raises change-review cost.
- Instrumentation: mesh exposes its effective submission limit directly while
  generation does not; visibility tracing is intentionally one-coordinate,
  bounded, and opt-in; full overlay snapshots remain per-enabled-frame cube
  walks rather than sampled telemetry, but the overlay now starts disabled.

Generation-priority runtime readiness is unaffected by these P2 maintainability
items. The matched motion evidence, deterministic regressions, exact lifecycle
accounting, and quiescence checks cover that behavioral change. Overall
streaming-program merge readiness is nevertheless withheld for the finite-world
clipping P1 measured above. The opt-in full GL/ImGui overlay comparison was
completed above; interactive shipped-backend first-draw timing remains an
explicit external performance gate and does not change the default-off runtime
path.

---

## 9. Reproducible Streaming Validation

Use the same world and streaming configuration for comparisons. Every
interactive result must include the successful resource record emitted before
spawn discovery, for example:

```text
world.resources blocks.loaded=248 blocks.failed=0 blocks.skipped=1 blocks.discovered=249 textures.loaded=134
```

Require `blocks.loaded` and `textures.loaded` to be greater than zero and
`blocks.failed` to be zero. If this record is absent, or resource initialization
reports a failure, do not use a later lifecycle line as a measurement boundary;
an all-air run is not representative streaming or remeshing evidence.

Record the resource line and the full
`streaming.lifecycle state=quiescent` line with every accepted result. Configure
an out-of-tree build as described in the project README, then set its location:

```bash
rigel_build_dir=/absolute/path/to/rigel-build
cmake --build "$rigel_build_dir" --parallel --target Rigel Rigel_tests
```

### 9.1 Renderer-independent behavior

Run the lifecycle and steady-state scheduler regressions directly. These tests
do not create a window or execute renderer code:

```bash
"$rigel_build_dir/Rigel_tests" \
  --filter ChunkStreamer_QuiescenceRequiresStableIdleUpdates --verbose
"$rigel_build_dir/Rigel_tests" \
  --filter ChunkStreamer_SteadyStateSchedulerWorkDoesNotScaleWithViewVolume --verbose
```

The first test covers startup gating, pending and in-flight load states, the
stable update window, stationary work counts, and leaving quiescence after an
edit. The second verifies that a settled update performs no scheduler or desired
set inspection even when the configured view volume is large. Use these results
for claims about streaming behavior independent of rendering.

### 9.2 Interactive hardware-GPU run

On a machine with a hardware OpenGL driver, launch Rigel with profiling enabled
and capture its log:

```bash
RIGEL_PROFILE=1 RIGEL_CHUNK_BENCH=1 \
  "$rigel_build_dir/bin/Rigel" 2>&1 | tee /tmp/rigel-hardware.log
```

Confirm that the `OpenGL Version` log identifies the intended hardware driver.
Confirm the successful `world.resources` record and its block/texture counts.
From another terminal, wait for the lifecycle signal rather than using a
startup delay:

```bash
tail -n +1 -F /tmp/rigel-hardware.log | \
  sed -n '/streaming.lifecycle state=quiescent /q'
```

Begin the stationary CPU capture only after that command exits. Keep the camera
and world unchanged during the capture. Use the profiler's `Streaming` scopes
to measure streaming and the `Render` scopes to measure renderer work; total
process CPU alone does not identify which subsystem consumed it.

### 9.3 Xvfb with llvmpipe

Run the same configuration under a virtual display with software rendering:

```bash
LIBGL_ALWAYS_SOFTWARE=1 RIGEL_PROFILE=1 RIGEL_CHUNK_BENCH=1 \
  xvfb-run -a -s "-screen 0 1280x720x24" \
  "$rigel_build_dir/bin/Rigel" 2>&1 | tee /tmp/rigel-llvmpipe.log
```

Verify that the `OpenGL Version` line identifies llvmpipe and that the successful
`world.resources` record has valid block/texture counts, then wait for the same
quiescent record before measuring. Xvfb plus llvmpipe is useful for repeatable
startup and streaming validation when no hardware GPU is available, but
software rasterization can dominate process CPU. Attribute CPU to streaming
only when the `Streaming` scopes or renderer-independent regressions provide
that evidence; do not infer it from the process total or from a hardware versus
llvmpipe comparison.

---

## 10. Known Limitations

- Overlay state is owned by the application's `FrameRenderer`, not by a world
  or `WorldView`.
- Missing shaders disable that overlay component.
- Entity boxes reflect AABB extents, not exact mesh silhouettes.

---

## Related Docs

- `docs/RenderingPipeline.md`
- `docs/InputSystem.md`
- `docs/EntitySystem.md`
- `docs/ApplicationLifecycle.md`
- `docs/WorldGeneration.md`
