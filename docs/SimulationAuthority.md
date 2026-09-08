# Simulation Authority

`SimulationHost` is the CPU-only owner for bounded authoritative simulation.
It owns one `World`, its fixed clock, one active local session, command receipts,
and revisioned publications. Submission, simulation, and replica consumption are
separate operations: `submit()` only admits owned command data, `advance()` runs
fixed ticks, and `LoopbackReplica::pumpOne()` applies queued publications.

## Content binding

`ContentDictionary` is constructed from a frozen `BlockRegistry` and the installed
`WorldGenerator`. It hashes canonical semantic records for every block and the
generator inputs into its SHA-256 manifest identity, then retains only the sorted
stable-key/local-ID mapping needed at runtime. A block record includes its stable
identifier, selection model and orientation, collision shape and provenance,
opacity, culling, and light behavior. Cosmetic texture paths and render layers are
not simulation identity.

Publications and commands carry stable block keys plus metadata and light bytes.
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

The current authoritative chunk storage preserves metadata and packed light bytes
for non-air blocks. Air is representable only as canonical `base:air` with both
bytes zero because empty subchunks are compressed. Commands reject noncanonical
air before admission, and replicas reject it before changing visible state; the
host never acknowledges or publishes a state different from retained storage.

The developer block-gallery generator has additional runtime placements beyond
its serialized terrain definition. This initial authority rejects that generator
explicitly rather than assigning it the ordinary empty generator's identity.

The manifest also binds the built-in entity update rule and its centered one-unit
hitbox. This bounded host admits only exact `Entity` instances using that rule and
hitbox, with no entity model attached. Virtual subclasses, custom local bounds,
preassigned IDs, non-finite state, and model-derived hitboxes are rejected before
spawn because their simulation or replay meaning is outside this manifest.
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

Entity spawn/despawn, session start, and command submission are inter-tick
admissions. Their effects are immediately visible to an owner-side checkpoint and
to the next tick; they never appear partway through a tick. The recording stores
successful admissions in that same order with the tick after which they became
visible. Rejected calls are observations, not replay inputs.

## Checkpoints and replay

`SimulationCheckpointManager` is the sole live publisher for one supplied save
root. It holds the storage root lock for its lifetime and permits one asynchronous
writer. A request copies one coherent host cut before returning; requests arriving
while that write or its terminal outcome is outstanding coalesce without a second
capture. Destruction joins the writer before releasing the root lock.

Checkpoint payloads contain the complete semantic dictionary, content/generator
manifest, normalized loaded-chunk coverage and block states, complete admitted
built-in entity state, tick and time debt, allocator, session, command receipts,
admission counters, configuration, and pending decisions. Compact block IDs in the
payload index the saved semantic dictionary and are rebound only after its stable
keys and manifest match the current process. Unsupported entity rules, models,
states, or block meaning reject capture or recovery. The current admitted rule has
no mutable RNG; the checkpoint records that RNG scheme explicitly, while generator
randomness remains fixed by the generator definition and seed in the manifest.

Restored state is validated as a complete authority cut before the candidate host
is returned. Entity IDs must belong to the saved world's authority allocation
domain and precede its saved frontier. Entity state must have finite motion, rule
values, tint and bounds, strictly ordered local bounds, finite translated world
bounds, matching type identity, sorted unique tags, and bounded aggregate owned
storage. Session, receipt, command, admission, outcome, and allocator identities
must be unique and mutually consistent; pending and aggregate receipt limits are
reapplied. Validation and owning-payload allocation finish on the unpublished
candidate, so malformed input cannot replace the live host or alter save files.

Each immutable payload binds its generation and the complete serialized parent-cut
hash, including scheduler debt. This durable ancestry is deliberately distinct
from the frame-pacing-independent simulation comparison hash. After the payload
is committed, a small atomic pointer publishes that generation, ancestry, cut,
payload length, and payload hash. A durable publication-pending fence is installed
before pointer replacement and removed only after replacement succeeds. Its
presence survives manager teardown and makes recovery and later publication stop
conservatively after a post-replacement durability uncertainty. Only a durable
pointer advances acknowledged history. A definitely unpublished error removes the
fence and permits a later request. Unknown root entries and incompatible pointer
formats are reported without rewriting or removing them. Historical payloads
never replace the live pointer.

State playback recovers the exact saved cut. Command resimulation is separate:
`recording()` returns the initial state plus the ordered successful inter-tick
admissions, and `resimulate()` applies them to the real CPU host while advancing
with caller-supplied frame pacing. It compares a canonical semantic hash at the
declared final tick and reports envelope mismatch, malformed input, or divergence.
The replay byte policy covers the retained baseline, admission containers, and
owned event payloads rather than only the eventual encoding. Exceeding either the
event or aggregate byte limit creates a permanent detectable gap and immediately
releases the unusable baseline and events.
The envelope is the current built-in CPU entity rule and bounded terrain; this is
not a claim of cross-platform floating-point or external physics lockstep.

## Loopback replicas

Connecting a replica captures a complete immutable baseline and installs its
subscription at the same revision. Each replica has independent bounded message
and cell storage; the host never exposes its mutable `World`. Change batches name
their base and resulting revisions and contain the complete atomic edit projection
plus ordered outcomes.

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
