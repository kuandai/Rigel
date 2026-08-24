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
(F3 by default).

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
fixture for the camera-containing chunk and one face-adjacent chunk. It uses the
production generator, streamer, worker pools, mesh builder, and visibility
trace, but it is neither shipped nor interactive scheduling. Persistence
returns a controlled missing-probe outcome, so every sample follows generation
without changing production scheduler settings.

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

The runner emits each raw sample followed by nearest-rank P50/P95 summaries for
distance and distance/first-observed-missing-desired-cardinal-neighbor cohorts.
`desired_to_visible` uses `first_draw` when a renderer supplies it and otherwise
uses `result_accepted`. The current runner is headless, so its header explicitly
reports `accepted` as the endpoint. Dependency wait, eligible-to-worker-start,
scheduler wait, pool wait, and worker execution are reported separately.
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
