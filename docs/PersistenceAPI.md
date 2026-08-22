# Persistence API

This document describes the current persistence API for saving and loading
world data (chunks and entities) in Rigel.

---

## 1. Overview

Persistence is structured around formats and containers:

- `PersistenceService` orchestrates save/load operations.
- `PersistenceFormat` exposes codecs and containers for a format.
- `ChunkContainer` and `EntityContainer` read/write region data.
- `RegionLayout` maps chunk coordinates to region/storage keys.
- `StorageBackend` abstracts the filesystem (with atomic write support).

The system is format-agnostic; formats are registered in `FormatRegistry` and
selected via `PersistenceContext`.

---

## 2. Core Types

### 2.1 Keys and Snapshots

Core storage types live in `Rigel::Persistence::Types`:

- `WorldMetadata`, `ZoneMetadata`
- `RegionKey`, `ChunkKey`, `EntityRegionKey`
- `ChunkSpan`, `ChunkData`, `ChunkSnapshot`, `ChunkRegionSnapshot`
- `EntityPersistedEntity`, `EntityPersistedChunk`, `EntityRegionSnapshot`

`ChunkSpan` allows partial chunk storage by addressing a sub-region inside a
chunk. A region file can contain multiple spans for the same chunk.

---

## 3. Persistence Context

`PersistenceContext` supplies configuration and dependencies:

- `rootPath`: base directory for saves.
- `preferredFormat`: format ID hint (e.g. `"cr"`).
- `manifestPath`: optional metadata file location.
- `policies`: behavior for unsupported format features.
- `storage`: `StorageBackend` instance (filesystem or custom).
- `providers`: `ProviderRegistry` for passing runtime data (e.g. block registry).

### 3.1 Policies

`PersistencePolicies` control error handling for unsupported format features:

- `UnsupportedFeaturePolicy`: `Fail`, `NoOp`, or `Warn`.

Unknown block identifiers are rejected before live chunk mutation. Rigel does
not silently replace or omit blocks: doing so would change persisted block
cardinality or destroy data on a later save. CR resolves stable string
identifiers while decoding; Memory validates its numeric identifiers when the
decoded spans are staged for application. A save or load can be retried after
the required registry entry becomes available.

Unknown entity types have one identifier-preserving lifecycle rather than a
policy switch:
loading creates a generic `Entity` placeholder that retains the persistent
entity ID, original type identifier, motion state, and model identifier. Saving
the placeholder writes the original identifiers unchanged. A later load uses
the registered `EntityFactory` implementation when that type becomes available.

---

## 4. Formats and Capabilities

Formats implement `PersistenceFormat`:

- `WorldMetadataCodec`
- `ZoneMetadataCodec`
- `ChunkContainer`
- `EntityContainer`
- `RegionLayout`

Each format provides a `FormatDescriptor`:

- `id` and on-disk `version`, used by registry selection and manifest checks
- `FormatCapabilities::supportsEntityRegions`, used to reject entity
  persistence when a backend does not implement that lifecycle
- `FormatCapabilities::fillMissingChunkSpans`, used by asynchronous loading to
  select generator base-fill for partial Memory-format chunk spans

Compression, metadata encoding, region indexing, and chunk-span behavior belong
to each backend implementation and its typed settings. They are not negotiated
through unused capability descriptors.

`FormatRegistry` resolves a format using:

- preferred format from context
- manifest probe (if any)
- storage probe (format-specific)

---

## 5. Containers and Codecs

### 5.1 ChunkContainer

Required methods:

- `saveRegion(const ChunkRegionSnapshot&)`
- `loadRegion(const RegionKey&)`
- `listRegions(const std::string& zoneId)`

### 5.2 EntityContainer

Required methods:

- `saveRegion(const EntityRegionSnapshot&)`
- `loadRegion(const EntityRegionKey&)`
- `forEachRegion(const std::string& zoneId, const EntityRegionVisitor&)`
- `listRegions(const std::string& zoneId)`

`forEachRegion` visits stored region keys without first retaining the complete
result and may be stopped by returning `false`. `listRegions` is the
materialized convenience form.

### 5.3 Codecs

Codecs are used by formats to encode/decode metadata and region payloads:

- `WorldMetadataCodec`
- `ZoneMetadataCodec`
- `ChunkCodec`
- `EntityRegionCodec`

---

## 6. Region Layout

`RegionLayout` maps between world coordinates and storage keys:

- `regionForChunk`: chunk -> region
- `storageKeysForChunk`: chunk -> one or more storage keys
- `spanForStorageKey`: storage key -> span metadata

Layouts define how partial chunk spans are packed into region files.

---

## 7. Chunk Serialization Helpers

Utilities in `ChunkSerializer` and `ChunkSpanMerge` provide:

- `serializeChunk` and `serializeChunkSpan`
- `applyChunkData` to write data into a `Voxel::Chunk`
- `mergeChunkSpans` to apply multiple stored spans to a chunk

`mergeChunkSpans` returns a summary describing which subchunks were filled and
whether the base fill was applied.

---

## 8. Storage Backend

`StorageBackend` abstracts filesystem access and allows alternative storage
implementations.

Core APIs:

- `openRead` / `openWrite` (with `AtomicWriteSession`)
- `exists`, `forEachEntry`, `list`, `mkdirs`, `remove`

`forEachEntry` provides stoppable directory enumeration. `list` collects the
same enumeration when a complete result is required.

`ByteReader`/`ByteWriter` supports random access via `seek`, `readAt`, and
`writeAt` for formats that require region indexes.

`AtomicWriteSession` promises process-safe publication: writes remain on a
backend-owned staging path until `commit`, and an uncommitted session leaves
the destination unchanged. Durability beyond the process depends on the
storage implementation.

On the supported Linux filesystem backend, a successful commit closes and
flushes the writer, calls `fsync` on the staging file, atomically renames it
over the destination without first deleting the destination, and calls `fsync`
on the containing directory. Before `mkdirs` returns or `openWrite` creates a
staging file, the backend walks the requested directory hierarchy one component
at a time. It creates or observes each component, synchronizes that component's
parent directory, and only then proceeds to the child. Already-present
components follow the same synchronization sequence, so retrying after a
failed parent synchronization cannot mistake a visible but unfinished
directory entry for a durable one. Removal likewise calls `fsync` on the
containing directory even when the path is already absent, so retrying a
removal can complete an earlier interrupted directory synchronization.

Any synchronization error is reported by the operation. A directory hierarchy
failure occurs before a dependent staging file is opened. A failure before
rename leaves the previous destination in place and removes only that session's
staging file. A failure while synchronizing the directory is reported after
publication and does not remove the published file or a newly created file
that happens to reuse the old staging pathname.

The non-Windows POSIX implementation uses the same file and directory `fsync`
sequence, but Linux is the environment covered by the project's durability
tests. On Windows, the staging file is synchronized with `FlushFileBuffers` and
replacement requests `MOVEFILE_WRITE_THROUGH`; the backend does not currently
have an independently validated equivalent for synchronizing parent-directory
replacement and removal entries. Consequently, the Linux power-loss guarantee
must not be assumed on Windows or an unvalidated POSIX environment.

---

## 9. World Save/Load Flow

`src/persistence/WorldPersistence.cpp` provides top-level helpers:

- `loadBootstrapEntities`
- `saveWorldToDisk`
- `saveChunkToDisk`

Current behavior:

- The default zone ID is `rigel:default`.
- The world root path is `saves/world_<worldId>`.
- `PersistenceService::saveWorldMetadata` writes one world-metadata document.
- `PersistenceService::saveZoneMetadata` writes one zone's metadata only.
- Metadata writes stage and validate one document, up to 4 MiB, before opening
  its output file. Each metadata file is atomically replaced independently;
  there is no aggregate world-plus-zone publication lifecycle.
- `PersistenceService` chunk and entity payload writes require explicit region snapshots.
- Runtime chunk loads use region snapshots through `AsyncChunkLoader`.
- Only chunks marked `isPersistDirty()` are saved.
- Regions are merged: existing region data is loaded, dirty spans overwrite,
  and all-air spans are skipped.
- Save and entity load validate a pending `entity-regions.journal` schema,
  persistence format identity and version, and expected zone before changing
  region files. A mismatch fails without removing the journal or applying it
  through the selected backend.
- A world save replays a valid pending journal before preparing new persistence
  work. It then prepares and validates the complete desired populated regions
  and obsolete-region key set without publishing them, writes dirty chunk
  regions, and only then publishes and applies the prepared entity journal.
  Replay writes desired regions, removes obsolete regions, and removes the
  journal only after all region operations succeed. A dirty chunk write failure
  therefore leaves the prepared current entity changes unpublished. A pristine
  world with no existing entity regions does not publish a journal. Chunk and
  entity persistence are ordered operations, not a cross-domain transaction.
- One close or recovery journal is bounded to 64 MiB of encoded data, 4,096
  combined desired and obsolete region declarations, 65,536 decoded chunks,
  and 65,536 decoded entities. These fixed product bounds cover the complete
  journal, in addition to the per-region payload and nested collection limits.
  Publication measures the same encoding and decoded work that replay accepts
  before staging the journal. Replay preflights all declarations, payload
  lengths, and nested work before reserving or retaining decoded snapshots.
  World close applies those limits while grouping live entities and completes
  entity journal validation before writing dirty chunk regions.
  The complete desired entity snapshot is also checked for null and duplicate
  persistent IDs before publication.
- Entity bootstrap uses stoppable region enumeration and applies the same
  aggregate region, encoding, chunk, and entity bounds as recovery before
  retaining each decoded region. It validates the complete bounded snapshot
  and collisions with live persistent IDs before constructing or spawning any
  entity. It does not clear chunks or unrelated live entities.
- Entity journal replay is idempotent process-interruption recovery. With the
  Linux filesystem backend, the journal becomes durably authoritative after
  its file and directory entry have been synchronized. It remains authoritative
  while desired region replacements and obsolete-region removals are applied;
  newly needed region-directory components and the region files are
  individually synchronized before the journal is removed, and that removal is
  synchronized. Replay repeats both directory preparation for desired regions
  and obsolete-region removals, even when the directory already exists or a
  removed pathname is already absent, ensuring an interrupted parent-directory
  synchronization completes before journal authority is discarded.
- A failed journal publication leaves either the previous region state with no
  durable new journal, or a published journal that is replayed before the next
  save. If final journal removal or its directory synchronization reports a
  failure, all declared region operations have already completed; a retained
  journal can safely replay them. These are the authoritative recovery states,
  and no success is reported for the failed operation.
- Atomic replacement protects process recovery on any conforming backend. The
  tested Linux synchronization sequence additionally orders writes across
  sudden power or kernel loss only when the filesystem, mount configuration,
  storage device, and its volatile caches honestly honor `fsync`. This contract
  does not extend to Windows or an unvalidated filesystem, and it does not
  protect against media corruption, filesystem defects, or devices that falsely
  report flush completion.
- Entity load reads and validates every persisted region before spawning. Null
  persistent IDs and duplicate IDs within or across regions fail the load.
- Entities tagged `EntityTags::NoSaveInChunks` are skipped.

---

## 10. Backends

Available backends live under `include/Rigel/Persistence/Backends`:

- `CR`: Cosmic Reach-compatible format.
- `Memory`: in-memory backend for tests.

Each backend supplies a format descriptor, factory, and probe function.

For format-specific details, see `docs/PersistenceBackends.md`.

---

## Related Docs

- `docs/PersistenceBackends.md`
- `docs/EntitySystem.md`
- `docs/ConfigurationSystem.md`
