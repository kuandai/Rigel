# Simulation Authority

`SimulationHost` is the CPU-only owner for bounded authoritative simulation.
It owns one `World`, its fixed clock, one active local session, command receipts,
and revisioned publications. Submission, simulation, and replica consumption are
separate operations: `submit()` only admits owned command data, `advance()` runs
fixed ticks, and `LoopbackReplica::pumpOne()` applies queued publications.

## Content binding

`ContentDictionary` is constructed from a frozen `BlockRegistry` and the installed
`WorldGenerator`. It retains sorted canonical semantic records for every block and
the generator inputs, and exposes their SHA-256 manifest identity. A block record
includes its stable identifier, selection model and orientation, collision shape
and provenance, opacity, culling, and light behavior. Cosmetic texture paths and
render layers are not simulation identity.

Publications and commands carry stable block keys plus metadata and light bytes.
The dictionary maps these records to the current registry's compact `BlockID` only
after manifest validation. Registry registration order therefore does not alter
the manifest identity, while a generator seed or semantic rule change does.
Untyped block extension data is rejected because its simulation meaning cannot be
encoded safely.

## Exact bounded terrain

The host configuration declares one inclusive cell domain and explicit resource
caps. Construction synchronously generates either every chunk intersecting that
domain or the configured subset. Exact reads distinguish known block state
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
The internal atomic edit form requires expected state for every addressed cell.
All cells and publication effects are prepared before block state changes. A stale,
unavailable, invalid, or out-of-domain member rejects the whole edit.

The clock uses a rational ticks-per-second rate and integer nanosecond debt.
`advance()` runs at most the configured catch-up count and retains remaining debt.
Each tick admits at most one queued command in arrival order, ticks entities in
sorted `EntityId` order, and then publishes one completed-tick revision. Host entity
IDs are allocated by the host rather than by entity constructors.

## Loopback replicas

Connecting a replica captures a complete immutable baseline and installs its
subscription at the same revision. Each replica has independent bounded message
and cell storage; the host never exposes its mutable `World`. Change batches name
their base and resulting revisions and contain the complete atomic edit projection
plus ordered outcomes.

Replica application validates manifest, world, zone, completeness, bounds, stable
keys, duplicate cells, and revision continuity before swapping visible state.
Malformed, missing, future-based, or oversized input requires a fresh baseline.
A slow replica whose queue fills is marked as needing resnapshot without blocking
or corrupting other replicas. The host publication ring, each replica queue,
snapshots, command payloads, and session receipts all have explicit caps.

This publication stream is process-local applied state. It does not acknowledge
durability, define a wire codec, or expose transport or authentication behavior.
