# Simulation Authority

`SimulationHost` is the CPU-only owner for bounded authoritative simulation.
It owns one `World`, its fixed clock, one active local session, command receipts,
and revisioned publications. Submission, simulation, and replica consumption are
separate operations: `submit()` only admits owned command data, `advance()` runs
fixed ticks, and `LoopbackReplica::pumpOne()` applies queued publications.

## Current policy and limits

The normal application deliberately configures a small local authority. These
are current policy values, not a promise that future worlds use the same bounds:

| Resource or rule | Current normal-mode policy |
| --- | --- |
| Exact terrain | Two vertically adjacent 32-cubed chunks (65,536 cells), generated before tick zero and pinned |
| Fixed time | 60 ticks/second; at most 8 catch-up ticks per `advance()`, with remaining nanosecond debt retained |
| Interaction | At most 8 cells per atomic command and 8 world units of reach |
| Session commands | 64 pending, 256 retained receipts, and 64 KiB of owned payload per receipt |
| Replicas | 8 connections, 32 queued publications per connection, and 64 MiB retained per replica |
| Entities | 256 built-in entities; 16 tags and 1 KiB of retained semantic strings and tag storage per entity |
| Semantic dictionary | 16 MiB retained by the host |
| Checkpoint and replay | 64 MiB configured for each checkpoint or recording; 4,096 replay admissions |

The checkpoint encoding also rejects any single payload above its independent
256 MiB format ceiling. Constructors can select smaller or different fixture
limits, but normal startup overrides terrain coverage to exactly the two chunks
above. Resource exhaustion is a typed refusal or recovery requirement; it does
not authorize eviction of receipts, partial commands, missing terrain, or
unmodeled entity state.

## Content binding

`ContentDictionary` is constructed from a frozen `BlockRegistry` and the installed
`WorldGenerator`. It hashes canonical semantic records for every block and the
generator inputs into its SHA-256 manifest identity, then retains only the sorted
stable-key/local-ID mapping needed at runtime. A block record includes its stable
identifier, oriented selectable cuboid bounds and face presence, and collision
shape. Orientations that produce the same selectable geometry are equivalent.
Model names, texture slots and coordinates, shading, ambient occlusion,
render culling, collision provenance, opacity, and light presentation do not
contribute because the authority does not consume them. Generator inputs and the
built-in entity update and hitbox rules remain part of simulation identity.

Commands carry stable block keys plus semantic metadata. Publications additionally
carry packed light as separate transitional presentation state.
The dictionary maps these records to the current registry's compact `BlockID` only
after manifest validation. Registry registration order therefore does not alter
the manifest identity, while a generator seed or semantic rule change does.
Untyped block extension data is rejected because its simulation meaning cannot be
encoded safely.

The host's `maxContentBytes` policy bounds the retained dictionary independently
of replica limits (16 MiB by default). Construction preflights aggregate stable-key
and lookup-table storage before copying keys, then verifies actual retained
capacity before exposing the dictionary. Replica budgets also account for their
shared dictionary ownership. The asset registry itself remains owned by semantic
content initialization; this dictionary cap is not a general asset-memory budget.

The current authoritative chunk storage preserves semantic metadata and a separate
packed light byte for non-air blocks. Air is representable only as canonical
`base:air` with zero metadata and zero packed light because empty subchunks are
compressed. Commands contain no light field and reject noncanonical air metadata;
replicas and checkpoint reconstruction additionally reject nonzero air light before
changing visible state. The host never acknowledges or publishes a state different
from retained storage.

The developer block-gallery generator has additional runtime placements beyond
its serialized terrain definition. This initial authority rejects that generator
explicitly rather than assigning it the ordinary empty generator's identity.

The manifest also binds the built-in entity update rule and its centered one-unit
authority hitbox. Cosmetic entity model identifiers are retained for graphical
projection but do not contribute to simulation content identity. This bounded host
admits only exact `Entity` instances using that rule and hitbox. It retains a
supported model identifier without loading a graphical model into authority; the
graphical replica resolves that handle for rendering.
Virtual subclasses, custom local bounds, preassigned IDs, non-finite state, loaded
model handles, and model-derived authority hitboxes are rejected before spawn
because their simulation or replay meaning is outside this manifest.
Configured entity-count, tag-count, and retained semantic-storage limits bound admitted
entity state. The retained byte check includes tag hash buckets, tag string objects
and capacities, and the fixed entity-type and empty model-handle/identifier capacities,
so a logically empty but churned container is rejected. The host is the symmetric
spawn/despawn owner; removing the active session actor causes its already admitted
commands to complete as actor unavailable.

## Exact bounded terrain

The host configuration declares one inclusive cell domain and explicit resource
caps. Construction synchronously generates either every chunk intersecting that
domain or the configured subset. Moved configuration strings or preload vectors
whose retained capacity exceeds their corresponding command or chunk cap
are rejected, and accepted preload coordinates are released from the retained
configuration after terrain construction. Exact reads distinguish known block state
(including air), unavailable chunks, coordinates outside the domain, and invalid
local state.

Authoritative ray validation uses the existing oriented visual-model targeting
algorithm. Before raycasting, it verifies the complete possible owner footprint
against the exact domain and loaded chunks. Placement also checks the selected
block's physical collision shape against the session actor. Entity collision
queries preflight their full model-overhang candidate coverage before invoking any
callback; missing or out-of-domain coverage suspends that axis movement.

## Commands and fixed ticks

A session binds its monotonic session ID, actor, world, zone, and content manifest.
Each command has a stable command ID and owned payload. Receipts retain that full
payload for the session:

- an identical retry reports pending or the original terminal outcome;
- a reused ID with different payload is rejected;
- old sessions and mismatched content or domains are rejected;
- receipt and pending-command exhaustion reject new work without evicting prior
  deduplication protection.

Remove and place commands require an exact shape-aware interaction expectation.
The ray origin must match the authoritative session actor position, and reach is
limited by the configured host policy; non-finite rays reject without admission.
The graphical client has one narrower exception for its existing local free-fly
camera: a host-bound capability may admit the tagged no-clip observer's finite pose
between ticks. That pose admission is recorded before the edit and becomes the
ordinary authoritative ray origin. It is not a player controller, a remote pose
proposal, or a general movement API. The local observer is excluded from placement
collision because it is a developer camera rather than physical occupancy.
The multi-cell atomic edit form additionally requires a capability issued by the
same host, so a client action value alone cannot invoke it. It requires expected
state for every addressed cell. All cells, deterministic entity tick membership,
and publication effects are prepared before block state changes. A stale,
unavailable, invalid, or out-of-domain member rejects the whole edit.

The clock uses a rational ticks-per-second rate and integer nanosecond debt.
`advance()` runs at most the configured catch-up count and retains remaining debt.
Each tick admits at most one queued command in arrival order, ticks entities in
sorted `EntityId` order, and then publishes one completed-tick revision. An empty
cell delta still communicates completed simulation time; it is not a duplicate of
an earlier cut. Consumers must pump these bounded publications, or explicitly
recover after falling behind. Host entity
IDs are allocated by the host rather than by entity constructors.

Entity spawn/despawn, session start, local-observer pose, and command submission are inter-tick
admissions. Their effects are immediately visible to an owner-side checkpoint and
to the next tick; they never appear partway through a tick. The recording stores
successful admissions in that same order with the tick after which they became
visible. Rejected calls are observations, not replay inputs.

A checkpoint can therefore contain accepted commands that have not reached their
first tick. The host exposes a bounded immutable active-session cursor containing
the actor, next unused command ID, and pending command IDs. Normal graphical
startup reattaches to that session when pending commands exist, rather than
clearing its receipts or advancing a hidden recovery tick. The ordinary fixed
tick then applies each command once and publishes its terminal outcome to the
new replica. A recovered session with no pending commands is replaced normally.

## Checkpoints and replay

Normal saves use a dedicated authority checkpoint directory alongside the
save-owned generator, settings, and backend identity. They do not import legacy
region, player, or entity files into the authority. An existing normal save root
with those entries, unknown siblings, an incompatible checkpoint marker, or a
mixture of authority and legacy data is rejected without rewriting or deleting
it. Conversion is not automatic; use an explicitly fresh destination when the
old data must be preserved.

`SimulationCheckpointManager` is the sole live publisher for one supplied save
root. It holds the storage root lock for its lifetime and permits one asynchronous
writer. A request copies one coherent host cut before returning; requests arriving
while that write or its terminal outcome is outstanding coalesce without a second
capture. Destruction joins the writer before releasing the root lock.

Checkpoint payloads contain the complete semantic dictionary, content/generator
manifest, world/zone/domain, fixed tick and interaction rules, normalized
loaded-chunk coverage and block states, complete admitted built-in entity state,
tick and time debt, allocator, session, command receipts, admission counters, and
pending decisions. Compact block IDs in the payload index the saved semantic
dictionary and are rebound only after its stable keys and manifest match the current
process. Unsupported entity rules, models, states, or block meaning reject capture
or recovery. The current admitted rule has no mutable RNG; the checkpoint records
that RNG scheme explicitly, while generator randomness remains fixed by the
generator definition and seed in the manifest.

Memory, queue, replica, catch-up, checkpoint, and replay resource limits belong to
the current process and are not restored from a save. The replay execution envelope
separately records the admission limits needed to reconstruct admitted work and
`maxSnapshotCells`, which gates interaction execution. Recovery overlays the saved
world rules on caller-supplied current policy, then requires the complete dictionary,
terrain, receipts, pending commands, and entities to fit those current bounds before
returning a candidate host. A pending interaction requires the current and recorded
snapshot-cell budgets to match; either direction of change rejects recovery instead
of adopting the old process policy or changing an admitted outcome. Once pending
interactions drain, a changed current budget is accepted normally. An insufficient
or incompatible policy rejects recovery without truncating state or changing
checkpoint files. A later checkpoint uses that host's current policy while retaining
the acknowledged payload hash as its durable parent.

Command mutation count/bytes and entity tag count/bytes are checked against both
the saved execution envelope and the current process policy before decoder vectors
or strings are allocated. Malformed saved counts remain corrupt input; a valid
saved cut that exceeds a lower current sub-limit is reported as incompatible.

Restored state is validated as a complete authority cut before the candidate host
is returned. Entity IDs must belong to the saved world's authority allocation
domain and precede its saved frontier. Entity state must have finite motion, rule
values, tint and bounds, strictly ordered local bounds, finite translated world
bounds, matching type identity, sorted unique tags, and bounded aggregate owned
storage. Session, receipt, command, admission, outcome, and allocator identities
must be unique and mutually consistent; pending and aggregate receipt limits are
reapplied. Validation and owning-payload allocation finish on the unpublished
candidate, so malformed input cannot replace the live host or alter save files.

Each captured payload binds its generation and the complete serialized parent-cut
hash, including scheduler debt. This durable ancestry is deliberately distinct
from the frame-pacing-independent simulation comparison hash. The comparison hash
includes saved world rules and authoritative state while omitting scheduler debt,
packed presentation light, current runtime policy, and the replay execution
envelope. After the payload is committed, a small atomic pointer publishes that
generation, ancestry, cut, payload length, and payload hash. A durable
publication-pending fence is installed before pointer replacement and removed only
after replacement succeeds. Its
presence survives manager teardown and makes recovery and later publication stop
conservatively after a post-replacement durability uncertainty. Only a durable
pointer advances acknowledged history. A definitely unpublished error removes the
fence and permits a later request. Unknown root entries and incompatible pointer
formats are reported without rewriting or removing them. Uncertainty in either
the fence commit or pointer commit blocks reuse, including after reopening.

The current storage policy uses two alternating payload slots. Generation identity
is in the payload/pointer, not the filename. Only the inactive slot is replaced,
so repeated failures cannot accumulate orphan files or overwrite the last
acknowledged payload. Two retained payloads plus one staged atomic replacement
bound payload storage to three times the per-payload limit during a write (the
format imposes a 256 MiB maximum per payload); pointer/format/fence records are
small and fixed-size. Uncertain publication preserves both slots for inspection.
This is not a historical-checkpoint archive or a permanent storage requirement.
The versioned root marker establishes ownership of the fixed slot names. Earlier
development formats fail explicitly; no automatic conversion or destructive
rewrite is attempted. An in-flight `Coalesced` request captures nothing newer:
the caller retains its save demand and requests the current cut after polling
the terminal result, whose tick/revision alone describe acknowledged progress.
Recovery on the live manager reports `PublicationPending` while a write is running
or its terminal outcome has not been polled. It neither consumes that outcome nor
mistakes the previously observed pointer for the completed write's cut.

State playback recovers the exact saved cut. Command resimulation is separate:
`recording()` returns the initial state plus the ordered successful inter-tick
admissions, and `resimulate()` applies them to the real CPU host while advancing
with caller-supplied frame pacing. It compares a canonical semantic hash at the
declared final tick and reports envelope mismatch, malformed input, or divergence.
The final cut and every admission must be at or after the saved baseline tick;
a recording cannot report success for a different tick than the one it declares.
The recording stores only the admission limits that can change whether its already
accepted commands and entities are admitted again: per-command changes, pending and
retained receipts, entity/tag storage, and command storage. This narrow replay
envelope does not become recovered-host policy. The recording's live event and byte
limits still cover the retained baseline, admission containers, and owned event
payloads rather than only the eventual encoding. Exceeding either limit creates a
permanent detectable gap and immediately releases the unusable baseline and events.
The envelope is the current built-in CPU entity rule and bounded terrain; this is
not a claim of cross-platform floating-point or external physics lockstep.

## Loopback replicas

Connecting a replica captures a complete immutable baseline and installs its
subscription at the same revision. Each replica has independent bounded message
and cell storage; the host never exposes its mutable `World`. Change batches name
their base and resulting revisions and contain the complete atomic edit projection
plus ordered outcomes. Baselines and tick changes also carry a bounded full list
of built-in entity simulation states. Entity state storage is allocated and copied
before an edit becomes visible; after the fixed update only scalar dynamic fields
are refreshed in that prepared publication.

Public message ingestion validates payload capacity before taking a private
immutable copy. A sender retaining a mutable shared-pointer alias cannot change or
grow queued data afterward. The authority's own publications are constructed const
and shared directly between its replicas without another per-recipient copy.

Replica application validates manifest, world, zone, completeness, bounds, stable
keys, duplicate cells, revision continuity, and strictly advancing ticks for newer
publications before swapping visible state. It also validates each outcome's
identity and publication cut, installs outcomes atomically with that cut, and makes
them available in order through `takeOutcome()`.
Malformed, missing, future-based, or oversized input requires a fresh baseline.
Old deltas must still have a valid base/result pair and valid cells/outcomes before
being ignored as duplicates. Repeated delivery never duplicates an unread outcome.
`resnapshot()` repairs only replicas already needing recovery; a healthy replica
returns `NotNeeded` and retains its readable cut, queued changes, and outcomes.
A slow replica whose queue fills is marked as needing resnapshot without blocking
or corrupting other replicas. Each replica applies one aggregate byte cap to its
installed cells, queued immutable messages, replica container storage, string
storage, retained outcome capacity, the complete shared content dictionary, and
apply-time copies. Counting the complete dictionary for every replica keeps the
per-replica bound valid even when a replica outlives its host and resource owner.
Before copying a baseline zone or cell key or
reserving its cell vector, the producer checks a conservative worst-case bound
using the frozen dictionary's longest key and the configured interest, queue, and
byte limits. Allocation failure leaves a connection unregistered or an existing
replica requiring resnapshot so the operation can be retried. Message count and
element caps remain independent. The host retains no unused publication ring.
Command payloads have a per-receipt byte cap, and the receipt count therefore
bounds aggregate session memory.

This publication stream is process-local applied state. It does not acknowledge
durability, define a wire codec, or expose transport or authentication behavior.

## Graphical bounded client

Normal graphical startup uses the same semantic registry, saved generator snapshot,
and `SimulationHost` as the CPU path. The initial policy pins the camera's one
horizontal chunk and the chunk immediately below it. It is deliberately not general
streaming authority. `WorldView` receives a separately allocated replica `World`;
the host's mutable world is never passed to input or rendering. A loopback baseline
populates the replica before readiness, and only revision-continuous change batches
alter it afterward. Each published chunk carries the saved generator semantics
version into the replica. `WorldView` meshes an exact resident-presentation set of
the two authority chunks; missing members wait for publication and the streamer
cannot load, generate, or evict them. Changed chunks drive mesh priority. Camera
motion and View Distance preference changes therefore cannot claim terrain outside
the initial coverage; a live View Distance change is rejected for this mode.

Input captures remove/place press edges against the replica's shape-aware target.
The graphical client converts compact block IDs to semantic states, records the
actual origin, direction, owner cell and face, submits asynchronously, and changes
nothing immediately. The host re-raycasts exact coverage at a fixed tick. Outcomes
and cell changes return in that tick's immutable publication. Holding a key cannot
submit another command because edge capture occurs once per presentation frame,
even when the host performs multiple catch-up ticks.

The graphical client tracks every accepted command until its terminal outcome. If
the host's bounded receipt table is full and no old-session command remains pending,
the client starts the host's next monotonic session and retries that same intent
once with command id one. Capacity reached while an outcome is pending is returned
as a typed deferred replacement instead of clearing deduplication state or silently
disabling later edits. Other observer, replica, and host refusals are likewise
reported as typed submission results and counted.
Pending command IDs restored from the checkpoint are installed in the same
bounded client accounting before the first recovered tick, so session rotation
remains deferred until those outcomes arrive.

Graphical projection is transactional with respect to each semantic publication.
The client resolves block IDs and model assets, constructs a complete replacement
entity set, clones only affected chunks, and reserves chunk-map, mesh-notification,
changed-chunk, and outcome bookkeeping before installation. Installation itself
cannot allocate. A failed baseline therefore never exposes a partially populated
world, while a failed change leaves the previous visible revision intact. The
loopback publication and unread outcomes remain owned by the client for retry;
later calls retry projection before admitting more host time. Elapsed time received
while projection remains blocked is retained and passed to the host after recovery.
If an independently advancing producer overflows the semantic queue meanwhile,
the client finishes its retained cut, resnapshots the latest baseline, and recovers
terminal outcomes for its bounded pending command identities from host receipts.
This path does not copy unchanged chunks or rebuild renderer model instances whose
entity ID and retained model handle are unchanged.

The replica also replaces its entity set from each coherent publication. It alone
loads cosmetic model assets; entity removal drops the replica entity before the
renderer prunes its model instance. The block gallery keeps its previous read-only
presentation generator and streaming path and never constructs a simulation host.
