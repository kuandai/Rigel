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

### 2.2 LoadScope

World loading can be scoped:

- `ChunksOnly`
- `EntitiesOnly`
- `All`

Helper functions select chunk and entity payloads from the requested scope.

---

## 3. Persistence Context

`PersistenceContext` supplies configuration and dependencies:

- `rootPath`: base directory for saves.
- `preferredFormat`: format ID hint (e.g. `"cr"`).
- `manifestPath`: optional metadata file location.
- `policies`: behavior for unknown IDs or unsupported features.
- `storage`: `StorageBackend` instance (filesystem or custom).
- `providers`: `ProviderRegistry` for passing runtime data (e.g. block registry).

### 3.1 Policies

`PersistencePolicies` control error handling:

- `UnknownIdPolicy`: `Fail`, `Placeholder`, or `Skip`.
- `UnsupportedFeaturePolicy`: `Fail`, `NoOp`, or `Warn`.

---

## 4. Formats and Capabilities

Formats implement `PersistenceFormat`:

- `WorldMetadataCodec`
- `ZoneMetadataCodec`
- `ChunkContainer`
- `EntityContainer`
- `RegionLayout`

Each format provides a `FormatDescriptor`:

- `id`, `version`, `extensions`
- `FormatCapabilities`

Capabilities advertise behavior like:

- compression type (`None`, `Lz4`, `Custom`)
- partial chunk support
- random access support
- entity region support
- versioning support

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

Chunk-level IO is optional and gated by `supportsChunkIO()`.

### 5.2 EntityContainer

Required methods:

- `saveRegion(const EntityRegionSnapshot&)`
- `loadRegion(const EntityRegionKey&)`
- `listRegions(const std::string& zoneId)`

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
- `chunksForRegion`: region -> chunks

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
- `exists`, `list`, `mkdirs`, `remove`

`ByteReader`/`ByteWriter` supports random access via `seek`, `readAt`, and
`writeAt` for formats that require region indexes.

---

## 9. World Save/Load Flow

`src/persistence/WorldPersistence.cpp` provides top-level helpers:

- `loadWorldFromDisk`
- `saveWorldToDisk`
- `loadChunkFromDisk`

Current behavior:

- The default zone ID is `rigel:default`.
- The world root path is `saves/world_<worldId>`.
- `PersistenceService::saveWorld` writes world and zone metadata only.
- `PersistenceService::saveZoneMetadata` writes one zone's metadata only.
- Metadata writes stage documents up to 4 MiB each. A world metadata save
  preflights at most 4,096 zones and 8 MiB of encoded metadata before opening
  any output file.
- Metadata files are atomically replaced one at a time. A world metadata save
  does not provide a cross-file transaction.
- `PersistenceService` chunk and entity payload writes require explicit region snapshots.
- Only chunks marked `isPersistDirty()` are saved.
- Regions are merged: existing region data is loaded, dirty spans overwrite,
  and all-air spans are skipped.
- Save and entity load validate a pending `entity-regions.journal` schema,
  persistence format identity and version, and expected zone before changing
  region files. A mismatch fails without removing the journal or applying it
  through the selected backend.
- A world save replays a valid pending journal before other region writes, then
  atomically replaces the journal with the complete desired populated regions
  and complete obsolete-region key set when entity region changes are needed.
  Replay writes desired regions, removes obsolete regions, and removes the
  journal only after all region operations succeed. A pristine world with no
  existing entity regions does not publish a journal.
- Journal declarations and nested entity payload counts are bounded and checked
  against remaining input before allocation. The complete desired entity
  snapshot is checked for null and duplicate persistent IDs before publication.
- Entity journal replay is idempotent process-interruption recovery built on
  atomic file replacement. It does not provide fsync-backed survival of power
  loss or storage-device or filesystem failure.
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
